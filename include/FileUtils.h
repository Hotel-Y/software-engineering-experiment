#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// 以流式方式计算文件的 FNV-1a 64 位校验值，避免把大文件一次读入内存。
std::uint64_t fnv1aFileChecksum(const std::filesystem::path& filePath);
// 在 filesystem 时钟与 Unix 时间之间转换，用于持久化文件修改时间。
std::int64_t toUnixSeconds(std::filesystem::file_time_type fileTime);
std::filesystem::file_time_type fromUnixSeconds(std::int64_t unixSeconds);
// 统一路径与 UTF-8 文本的转换，确保中文文件名可以进入清单和命令行。
std::string pathToUtf8(const std::filesystem::path& path);
std::filesystem::path pathFromUtf8(const std::string& value);
// 判断 child 是否等于 parent 或位于 parent 内部，是路径越界检查的基础。
bool isPathSameOrInside(const std::filesystem::path& child,
                        const std::filesystem::path& parent);
// 将清单中的相对路径安全解析到 root 下，拒绝绝对路径和“..”逃逸。
std::filesystem::path resolvePathInside(const std::filesystem::path& root,
                                        const std::string& relativePath);
// 将扩展名转成小写并补齐前导点，便于不区分大小写地比较过滤条件。
std::string normalizeExtension(std::string extension);
