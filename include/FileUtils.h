#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

std::uint64_t fnv1aFileChecksum(const std::filesystem::path& filePath);
std::int64_t toUnixSeconds(std::filesystem::file_time_type fileTime);
std::filesystem::file_time_type fromUnixSeconds(std::int64_t unixSeconds);
std::string pathToUtf8(const std::filesystem::path& path);
std::filesystem::path pathFromUtf8(const std::string& value);
std::string normalizeExtension(std::string extension);
