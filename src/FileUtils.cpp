#include "FileUtils.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>

// 计算快速的非密码学内容指纹，用于日常备份完整性校验。
std::uint64_t fnv1aFileChecksum(const std::filesystem::path& filePath) {
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open file for checksum: " + pathToUtf8(filePath));
    }

    constexpr std::uint64_t offsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offsetBasis;

    // 固定缓冲区流式读取，内存占用不会随文件大小增加。
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

// 将文件系统时钟映射到可写入清单的 Unix 秒。
std::int64_t toUnixSeconds(std::filesystem::file_time_type fileTime) {
    // C++17 未提供两个时钟的直接转换，使用“距现在的偏移”映射到系统时钟。
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(systemTime.time_since_epoch()).count();
}

// 将清单中的 Unix 秒反向映射为 filesystem 修改时间。
std::filesystem::file_time_type fromUnixSeconds(std::int64_t unixSeconds) {
    const auto systemTime = std::chrono::system_clock::time_point{std::chrono::seconds{unixSeconds}};
    return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(
        systemTime - std::chrono::system_clock::now() + std::filesystem::file_time_type::clock::now());
}

// 清单统一保存 UTF-8 路径，并使用正斜杠作为通用分隔形式。
std::string pathToUtf8(const std::filesystem::path& path) {
    return path.generic_u8string();
}

// 从清单 UTF-8 字段重建当前平台可使用的 filesystem 路径。
std::filesystem::path pathFromUtf8(const std::string& value) {
    return std::filesystem::u8path(value);
}

// 规范化后按路径组件比较，判断候选路径是否受指定根目录约束。
bool isPathSameOrInside(const std::filesystem::path& child,
                        const std::filesystem::path& parent) {
    // 规范化后逐段比较，避免简单字符串前缀把 C:\data2 误判为 C:\data 的子目录。
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

// 安全解析来自清单的相对路径，是 Restore 防目录穿越的最终入口。
std::filesystem::path resolvePathInside(const std::filesystem::path& root,
                                        const std::string& relativePath) {
    // 先从语法层拒绝绝对路径和 ..，再从规范化后的真实路径做第二次根目录检查。
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

// 将用户输入与文件实际扩展名规范为相同形式后再比较。
std::string normalizeExtension(std::string extension) {
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!extension.empty() && extension.front() != '.') {
        extension.insert(extension.begin(), '.');
    }
    return extension;
}
