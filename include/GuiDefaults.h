#pragma once

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>

namespace sbm::gui {

enum class InputKind {
    Invalid,
    SourceDirectory,
    BackupDirectory,
    ArchiveFile,
};

enum class BackupPreparation {
    Ready,
    CreateFromSource,
    Unavailable,
};

struct SmartDefaults {
    InputKind kind = InputKind::Invalid;
    std::filesystem::path input;
    std::filesystem::path source;
    std::filesystem::path backup;
    std::filesystem::path archive;
    std::filesystem::path restore;
    std::wstring recommendedAction;
    std::wstring message;

    [[nodiscard]] bool valid() const {
        return kind != InputKind::Invalid;
    }
};

inline std::wstring trimPathInput(std::wstring value) {
    const auto isSpace = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

inline std::filesystem::path uniqueOutputPath(const std::filesystem::path& candidate,
                                              bool preserveExtension = false) {
    std::error_code error;
    if (!std::filesystem::exists(candidate, error)) {
        return candidate;
    }

    const auto parent = candidate.parent_path();
    const auto extension = preserveExtension ? candidate.extension().wstring() : std::wstring{};
    const auto stem = preserveExtension ? candidate.stem().wstring() : candidate.filename().wstring();
    for (unsigned int index = 2; index < 10000; ++index) {
        const auto name = stem + L"_" + std::to_wstring(index) + extension;
        const auto alternative = parent / std::filesystem::path(name);
        error.clear();
        if (!std::filesystem::exists(alternative, error)) {
            return alternative;
        }
    }
    return candidate;
}

inline bool hasArchiveExtension(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return extension == L".sba";
}

inline bool hasBackupManifest(const std::filesystem::path& backup) {
    std::error_code error;
    return !backup.empty() &&
           std::filesystem::is_regular_file(backup / L"manifest.sbm", error);
}

inline BackupPreparation prepareBackupForDependentAction(
    const std::filesystem::path& source,
    const std::filesystem::path& backup) {
    if (hasBackupManifest(backup)) {
        return BackupPreparation::Ready;
    }

    std::error_code error;
    if (!source.empty() && std::filesystem::is_directory(source, error)) {
        return BackupPreparation::CreateFromSource;
    }
    return BackupPreparation::Unavailable;
}

inline SmartDefaults inferSmartDefaults(const std::wstring& rawInput) {
    SmartDefaults result;
    const auto trimmed = trimPathInput(rawInput);
    if (trimmed.empty()) {
        result.message = L"Enter an existing directory or .sba archive.";
        return result;
    }

    std::error_code error;
    auto input = std::filesystem::absolute(std::filesystem::path(trimmed), error);
    if (error) {
        result.message = L"The input path could not be resolved.";
        return result;
    }
    input = input.lexically_normal();
    result.input = input;

    error.clear();
    if (std::filesystem::is_regular_file(input, error) && hasArchiveExtension(input)) {
        result.kind = InputKind::ArchiveFile;
        result.source = input;
        result.archive = input;
        const auto parent = input.parent_path();
        const auto name = input.stem().wstring();
        result.backup = uniqueOutputPath(parent / std::filesystem::path(name + L"_unpacked"));
        result.restore = uniqueOutputPath(parent / std::filesystem::path(name + L"_restored"));
        result.recommendedAction = L"Unpack";
        result.message = L"Detected SBA archive; output paths were generated without overwriting existing data.";
        return result;
    }

    error.clear();
    if (!std::filesystem::is_directory(input, error)) {
        result.message = L"Input not found. Use an existing source directory, backup directory, or .sba file.";
        return result;
    }

    result.source = input;
    const auto parent = input.parent_path();
    auto name = input.filename().wstring();
    if (name.empty()) {
        name = L"source";
    }

    error.clear();
    const auto manifest = input / L"manifest.sbm";
    if (std::filesystem::is_regular_file(manifest, error)) {
        result.kind = InputKind::BackupDirectory;
        result.backup = input;
        result.archive = uniqueOutputPath(parent / std::filesystem::path(name + L".sba"), true);
        result.restore = uniqueOutputPath(parent / std::filesystem::path(name + L"_restored"));
        result.recommendedAction = L"Verify / Pack / Restore";
        result.message = L"Detected backup directory from manifest.sbm; archive and restore targets were generated.";
        return result;
    }

    result.kind = InputKind::SourceDirectory;
    result.backup = uniqueOutputPath(parent / std::filesystem::path(name + L"_backup"));
    result.archive = uniqueOutputPath(
        parent / std::filesystem::path(result.backup.filename().wstring() + L".sba"), true);
    result.restore = uniqueOutputPath(parent / std::filesystem::path(name + L"_restored"));
    result.recommendedAction = L"Backup";
    result.message = L"Detected source directory; all files, automatic compression, and no-overwrite defaults are ready.";
    return result;
}

}  // namespace sbm::gui
