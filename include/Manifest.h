#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// 清单中的一条目录或文件记录；路径始终相对于备份根目录。
struct ManifestEntry {
    std::string relativePath;          // UTF-8 相对路径。
    std::uintmax_t size = 0;           // 文件字节数，目录为 0。
    std::int64_t modifiedTime = 0;     // Unix 秒格式的最后修改时间。
    std::uint64_t checksum = 0;        // 文件 FNV-1a 校验值，目录为 0。
    bool isDirectory = false;          // true 表示目录项，false 表示普通文件。
};

// manifest.sbm 的内存模型，负责 SBM1 文本格式的序列化与严格解析。
class Manifest {
public:
    // 按遍历顺序添加记录；顺序会原样写入文件。
    void add(const ManifestEntry& entry);
    const std::vector<ManifestEntry>& entries() const;

    // 保存或读取完整清单；读取时检查格式、条目类型和重复路径。
    void save(const std::filesystem::path& filePath) const;
    static Manifest load(const std::filesystem::path& filePath);

private:
    std::vector<ManifestEntry> entries_;
};
