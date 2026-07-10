#pragma once

#include "Manifest.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct BackupOptions {
    std::vector<std::string> includeExtensions;
    std::string nameContains;
    std::string pathContains;
    std::optional<std::int64_t> modifiedAfter;
    std::optional<std::int64_t> modifiedBefore;
    std::optional<std::uintmax_t> maxSizeBytes;
    bool overwrite = false;
};

struct OperationStats {
    std::size_t directories = 0;
    std::size_t files = 0;
    std::size_t skippedFiles = 0;
    std::size_t failedFiles = 0;
    std::uintmax_t bytes = 0;
};

class BackupManager {
public:
    explicit BackupManager(BackupOptions options = {});

    OperationStats backup(const std::filesystem::path& sourceDir,
                          const std::filesystem::path& backupDir) const;

    OperationStats restore(const std::filesystem::path& backupDir,
                           const std::filesystem::path& restoreDir) const;

    OperationStats verify(const std::filesystem::path& backupDir) const;

private:
    bool shouldIncludeFile(const std::filesystem::path& filePath,
                           std::uintmax_t fileSize) const;

    BackupOptions options_;
};
