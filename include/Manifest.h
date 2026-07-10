#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct ManifestEntry {
    std::string relativePath;
    std::uintmax_t size = 0;
    std::int64_t modifiedTime = 0;
    std::uint64_t checksum = 0;
    bool isDirectory = false;
};

class Manifest {
public:
    void add(const ManifestEntry& entry);
    const std::vector<ManifestEntry>& entries() const;

    void save(const std::filesystem::path& filePath) const;
    static Manifest load(const std::filesystem::path& filePath);

private:
    std::vector<ManifestEntry> entries_;
};
