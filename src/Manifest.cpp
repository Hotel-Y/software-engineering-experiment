#include "Manifest.h"

#include "FileUtils.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace {
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

std::vector<std::string> splitEscaped(const std::string& line) {
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

void Manifest::add(const ManifestEntry& entry) {
    entries_.push_back(entry);
}

const std::vector<ManifestEntry>& Manifest::entries() const {
    return entries_;
}

void Manifest::save(const std::filesystem::path& filePath) const {
    std::ofstream output(filePath, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Cannot write manifest: " + pathToUtf8(filePath));
    }

    output << "SBM1\n";
    for (const auto& entry : entries_) {
        output << (entry.isDirectory ? "D" : "F") << '|'
               << escapeField(entry.relativePath) << '|'
               << entry.size << '|'
               << entry.modifiedTime << '|'
               << entry.checksum << '\n';
    }
}

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
