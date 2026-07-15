#include "BackupManager.h"

#include "FileUtils.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
constexpr const char* manifestName = "manifest.sbm";

void ensureDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        throw std::runtime_error("Cannot create directory: " + pathToUtf8(path));
    }
}

std::filesystem::path manifestPath(const std::filesystem::path& backupDir) {
    return backupDir / manifestName;
}

}

BackupManager::BackupManager(BackupOptions options) : options_(std::move(options)) {
    for (auto& extension : options_.includeExtensions) {
        extension = normalizeExtension(extension);
    }
}

OperationStats BackupManager::backup(const std::filesystem::path& sourceDir,
                                      const std::filesystem::path& backupDir) const {
    if (!std::filesystem::exists(sourceDir) || !std::filesystem::is_directory(sourceDir)) {
        throw std::runtime_error("Source directory does not exist: " + pathToUtf8(sourceDir));
    }
    if (isPathSameOrInside(backupDir, sourceDir)) {
        throw std::runtime_error("Backup directory must not be inside the source directory.");
    }
    if (std::filesystem::exists(backupDir) && !options_.overwrite) {
        throw std::runtime_error("Backup directory already exists. Use --overwrite to replace files.");
    }
    if (std::filesystem::exists(backupDir) && options_.overwrite) {
        std::error_code ec;
        std::filesystem::remove_all(backupDir, ec);
        if (ec) {
            throw std::runtime_error("Cannot clean backup directory before overwrite: " + pathToUtf8(backupDir));
        }
    }

    ensureDirectory(backupDir);
    Manifest manifest;
    OperationStats stats;

    const auto root = std::filesystem::absolute(sourceDir);
    for (const auto& item : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied)) {
        const auto relativePath = std::filesystem::relative(item.path(), root);
        const auto targetPath = backupDir / relativePath;

        if (item.is_directory()) {
            ensureDirectory(targetPath);
            manifest.add({pathToUtf8(relativePath), 0, toUnixSeconds(item.last_write_time()), 0, true});
            ++stats.directories;
            continue;
        }

        if (!item.is_regular_file()) {
            ++stats.skippedFiles;
            continue;
        }

        const auto fileSize = item.file_size();
        if (!shouldIncludeFile(item.path(), relativePath, fileSize)) {
            ++stats.skippedFiles;
            continue;
        }

        ensureDirectory(targetPath.parent_path());
        std::filesystem::copy_file(item.path(), targetPath,
                                   std::filesystem::copy_options::overwrite_existing);
        manifest.add({pathToUtf8(relativePath),
                      fileSize,
                      toUnixSeconds(item.last_write_time()),
                      fnv1aFileChecksum(item.path()),
                      false});
        ++stats.files;
        stats.bytes += fileSize;
    }

    manifest.save(manifestPath(backupDir));
    return stats;
}

OperationStats BackupManager::restore(const std::filesystem::path& backupDir,
                                       const std::filesystem::path& restoreDir) const {
    const auto manifest = Manifest::load(manifestPath(backupDir));
    ensureDirectory(restoreDir);
    OperationStats stats;

    for (const auto& entry : manifest.entries()) {
        const auto sourcePath = resolvePathInside(backupDir, entry.relativePath);
        const auto targetPath = resolvePathInside(restoreDir, entry.relativePath);

        if (entry.isDirectory) {
            ensureDirectory(targetPath);
            ++stats.directories;
            continue;
        }

        if (!std::filesystem::exists(sourcePath)) {
            throw std::runtime_error("Backup file is missing: " + pathToUtf8(sourcePath));
        }
        ensureDirectory(targetPath.parent_path());
        std::filesystem::copy_file(sourcePath, targetPath,
                                   std::filesystem::copy_options::overwrite_existing);

        const auto checksum = fnv1aFileChecksum(targetPath);
        if (checksum != entry.checksum) {
            throw std::runtime_error("Restored file checksum mismatch: " + pathToUtf8(targetPath));
        }
        std::filesystem::last_write_time(targetPath, fromUnixSeconds(entry.modifiedTime));
        ++stats.files;
        stats.bytes += entry.size;
    }
    return stats;
}

OperationStats BackupManager::verify(const std::filesystem::path& backupDir) const {
    const auto manifest = Manifest::load(manifestPath(backupDir));
    OperationStats stats;

    for (const auto& entry : manifest.entries()) {
        if (entry.isDirectory) {
            const auto directoryPath = resolvePathInside(backupDir, entry.relativePath);
            if (std::filesystem::exists(directoryPath)
                && std::filesystem::is_directory(directoryPath)) {
                ++stats.directories;
            } else {
                std::cerr << "Missing directory: " << pathToUtf8(directoryPath) << '\n';
                ++stats.failedFiles;
            }
            continue;
        }

        const auto filePath = resolvePathInside(backupDir, entry.relativePath);
        if (!std::filesystem::exists(filePath) || !std::filesystem::is_regular_file(filePath)) {
            std::cerr << "Missing: " << pathToUtf8(filePath) << '\n';
            ++stats.failedFiles;
            continue;
        }
        const auto actualSize = std::filesystem::file_size(filePath);
        const auto actualChecksum = fnv1aFileChecksum(filePath);
        if (actualSize != entry.size || actualChecksum != entry.checksum) {
            std::cerr << "Changed: " << pathToUtf8(filePath) << '\n';
            ++stats.failedFiles;
        } else {
            ++stats.files;
            stats.bytes += entry.size;
        }
    }

    return stats;
}

bool BackupManager::shouldIncludeFile(const std::filesystem::path& filePath,
                                      const std::filesystem::path& relativePath,
                                      std::uintmax_t fileSize) const {
    if (options_.maxSizeBytes && fileSize > *options_.maxSizeBytes) {
        return false;
    }

    const auto modifiedTime = toUnixSeconds(std::filesystem::last_write_time(filePath));
    if (options_.modifiedAfter && modifiedTime < *options_.modifiedAfter) {
        return false;
    }
    if (options_.modifiedBefore && modifiedTime > *options_.modifiedBefore) {
        return false;
    }

    const auto fileName = pathToUtf8(filePath.filename());
    const auto filterPath = pathToUtf8(relativePath);
    if (!options_.nameContains.empty() &&
        fileName.find(options_.nameContains) == std::string::npos) {
        return false;
    }
    if (!options_.pathContains.empty() &&
        filterPath.find(options_.pathContains) == std::string::npos) {
        return false;
    }

    if (!options_.includeExtensions.empty()) {
        const auto extension = normalizeExtension(pathToUtf8(filePath.extension()));
        return std::find(options_.includeExtensions.begin(),
                         options_.includeExtensions.end(),
                         extension) != options_.includeExtensions.end();
    }

    return true;
}
