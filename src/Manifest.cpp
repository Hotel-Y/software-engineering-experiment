#include "Manifest.h"

#include "FileUtils.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace {
// SBM1 使用“|”分隔字段；路径中的反斜杠和分隔符写入前必须转义。
std::string escapeField(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        if (ch == '\\' || ch == '|') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

// 按未转义的“|”拆分一行，并在读取过程中移除字段转义符。
std::vector<std::string> splitEscaped(const std::string& line) {
    // 逐字符解析而非直接 split，确保“\|”仍属于路径内容。
    std::vector<std::string> fields;
    std::string current;
    bool escaped = false;
    for (char ch : line) {
        if (escaped) {
            current.push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '|') {
            fields.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    fields.push_back(current);
    return fields;
}
}

// 内存模型保持插入顺序，写回时目录和文件顺序与遍历结果一致。
void Manifest::add(const ManifestEntry& entry) {
    entries_.push_back(entry);
}

// 返回常量引用，避免复制大量清单条目，同时禁止调用方修改内部状态。
const std::vector<ManifestEntry>& Manifest::entries() const {
    return entries_;
}

// 以 SBM1 文本格式保存：类型|路径|大小|修改时间|校验值。
void Manifest::save(const std::filesystem::path& filePath) const {
    std::ofstream output(filePath, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Cannot write manifest: " + pathToUtf8(filePath));
    }

    // 首行版本号用于快速拒绝未知格式；后续每行固定为五个字段。
    output << "SBM1\n";
    for (const auto& entry : entries_) {
        output << (entry.isDirectory ? "D" : "F") << '|'
               << escapeField(entry.relativePath) << '|'
               << entry.size << '|'
               << entry.modifiedTime << '|'
               << entry.checksum << '\n';
    }
}

// 严格读取 SBM1；格式、类型、字段数或路径唯一性异常都会拒绝整个清单。
Manifest Manifest::load(const std::filesystem::path& filePath) {
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read manifest: " + pathToUtf8(filePath));
    }

    std::string header;
    std::getline(input, header);
    if (header != "SBM1") {
        throw std::runtime_error("Unsupported or broken manifest format.");
    }

    // 同一路径只允许出现一次，避免还原时后写条目覆盖先写条目。
    Manifest manifest;
    std::unordered_set<std::string> paths;
    std::string line;
    std::size_t lineNumber = 1;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (line.empty()) {
            continue;
        }
        const auto fields = splitEscaped(line);
        if (fields.size() != 5) {
            throw std::runtime_error("Broken manifest line: " + std::to_string(lineNumber));
        }

        if (fields[0] != "D" && fields[0] != "F") {
            throw std::runtime_error("Broken manifest entry type on line: "
                                     + std::to_string(lineNumber));
        }
        if (fields[1].empty() || !paths.insert(fields[1]).second) {
            throw std::runtime_error("Empty or duplicate manifest path on line: "
                                     + std::to_string(lineNumber));
        }

        ManifestEntry entry;
        entry.isDirectory = fields[0] == "D";
        entry.relativePath = fields[1];
        entry.size = static_cast<std::uintmax_t>(std::stoull(fields[2]));
        entry.modifiedTime = std::stoll(fields[3]);
        entry.checksum = std::stoull(fields[4]);
        manifest.add(entry);
    }
    return manifest;
}
