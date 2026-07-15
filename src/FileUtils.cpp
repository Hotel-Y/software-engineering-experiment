#include "FileUtils.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>

std::uint64_t fnv1aFileChecksum(const std::filesystem::path& filePath) {
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open file for checksum: " + pathToUtf8(filePath));
    }

    constexpr std::uint64_t offsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offsetBasis;

    char buffer[8192];
    while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
        const auto count = input.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= prime;
        }
    }

    return hash;
}

std::int64_t toUnixSeconds(std::filesystem::file_time_type fileTime) {
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(systemTime.time_since_epoch()).count();
}

std::filesystem::file_time_type fromUnixSeconds(std::int64_t unixSeconds) {
    const auto systemTime = std::chrono::system_clock::time_point{std::chrono::seconds{unixSeconds}};
    return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(
        systemTime - std::chrono::system_clock::now() + std::filesystem::file_time_type::clock::now());
}

std::string pathToUtf8(const std::filesystem::path& path) {
    return path.generic_u8string();
}

std::filesystem::path pathFromUtf8(const std::string& value) {
    return std::filesystem::u8path(value);
}

bool isPathSameOrInside(const std::filesystem::path& child,
                        const std::filesystem::path& parent) {
    const auto absoluteChild = std::filesystem::weakly_canonical(child);
    const auto absoluteParent = std::filesystem::weakly_canonical(parent);

    auto childIt = absoluteChild.begin();
    auto parentIt = absoluteParent.begin();
    for (; parentIt != absoluteParent.end(); ++parentIt, ++childIt) {
        if (childIt == absoluteChild.end() || *childIt != *parentIt) {
            return false;
        }
    }
    return true;
}

std::filesystem::path resolvePathInside(const std::filesystem::path& root,
                                        const std::string& relativePath) {
    const auto relative = pathFromUtf8(relativePath).lexically_normal();
    if (relative.empty() || relative == "." || relative.is_absolute()
        || relative.has_root_name() || relative.has_root_directory()) {
        throw std::runtime_error("Path must be a non-empty relative path: " + relativePath);
    }
    for (const auto& part : relative) {
        if (part == "..") {
            throw std::runtime_error("Path escapes its root directory: " + relativePath);
        }
    }

    const auto candidate = std::filesystem::weakly_canonical(root / relative);
    if (!isPathSameOrInside(candidate, root)) {
        throw std::runtime_error("Path escapes its root directory: " + relativePath);
    }
    return candidate;
}

std::string normalizeExtension(std::string extension) {
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!extension.empty() && extension.front() != '.') {
        extension.insert(extension.begin(), '.');
    }
    return extension;
}
