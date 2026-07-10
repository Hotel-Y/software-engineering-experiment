#pragma once

#include <filesystem>
#include <string>

struct ArchiveStats {
    std::size_t files = 0;
    std::uintmax_t bytes = 0;
    std::uintmax_t storedBytes = 0;
};

struct ArchiveOptions {
    bool useRleCompression = false;
    std::string password;
};

class ArchiveManager {
public:
    explicit ArchiveManager(ArchiveOptions options = {});

    ArchiveStats pack(const std::filesystem::path& backupDir,
                      const std::filesystem::path& archiveFile) const;

    ArchiveStats unpack(const std::filesystem::path& archiveFile,
                        const std::filesystem::path& outputDir) const;

private:
    ArchiveOptions options_;
};
