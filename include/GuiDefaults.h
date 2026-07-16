#pragma once

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>

namespace sbm::gui {

// 智能输入框识别出的路径类型，GUI 据此决定哪些操作可直接执行。
enum class InputKind {
    Invalid,          // 空路径、不存在路径或不受支持的普通文件。
    SourceDirectory,  // 不含清单、等待创建备份的普通目录。
    BackupDirectory,  // 含 manifest.sbm、可直接验证或打包的目录。
    ArchiveFile,      // 扩展名为 .sba 的现有普通文件。
};

// Pack、Restore 等依赖备份目录的操作在执行前需要达到的准备状态。
enum class BackupPreparation {
    Ready,             // 已有合法 manifest.sbm，可直接继续。
    CreateFromSource,  // 当前是源目录，需要先自动创建备份。
    Unavailable,       // 既没有备份清单，也没有可用源目录。
};

// 一次路径识别生成的完整默认值，供原生 GUI 同步填充多个输入框。
struct SmartDefaults {
    InputKind kind = InputKind::Invalid;       // 推断出的输入类型。
    std::filesystem::path input;               // 规范化后的原始输入路径。
    std::filesystem::path source;              // GUI 的 Input/Source 字段。
    std::filesystem::path backup;              // 备份目录或解包目标目录。
    std::filesystem::path archive;             // SBA 输入或默认输出文件。
    std::filesystem::path restore;             // 默认还原目标目录。
    std::wstring recommendedAction;            // 建议用户首先执行的按钮操作。
    std::wstring message;                      // 路径识别结果或错误原因。

    [[nodiscard]] bool valid() const {
        return kind != InputKind::Invalid;
    }
};

// 清理粘贴路径两端的空白和成对引号，兼容从资源管理器复制的路径。
inline std::wstring trimPathInput(std::wstring value) {
    const auto isSpace = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

// 默认输出已存在时追加 _2、_3……，防止智能填充覆盖用户数据。
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

// 扩展名判断不区分大小写，因此 .SBA 与 .sba 都可识别。
inline bool hasArchiveExtension(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return extension == L".sba";
}

// 只把含 manifest.sbm 的目录认作备份目录，避免把普通源目录误判为备份。
inline bool hasBackupManifest(const std::filesystem::path& backup) {
    std::error_code error;
    return !backup.empty() &&
           std::filesystem::is_regular_file(backup / L"manifest.sbm", error);
}

// 为依赖备份的按钮判断：直接使用、先自动备份，还是提示用户补充输入。
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

// 核心推断流程：根据一个已存在路径生成备份、归档、还原路径及推荐操作。
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
    // SBA 文件优先按归档处理，并生成互不覆盖的解包与还原目录。
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
    // 目录内存在清单时，它已是备份结果，不再为它生成新的 _backup 目录。
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

    // 其余已存在目录视作源目录，按默认规则生成同级输出路径。
    result.kind = InputKind::SourceDirectory;
    result.backup = uniqueOutputPath(parent / std::filesystem::path(name + L"_backup"));
    result.archive = uniqueOutputPath(
        parent / std::filesystem::path(result.backup.filename().wstring() + L".sba"), true);
    result.restore = uniqueOutputPath(parent / std::filesystem::path(name + L"_restored"));
    result.recommendedAction = L"Backup";
    result.message = L"Detected source directory; all files, automatic compression, and no-overwrite defaults are ready.";
    return result;
}

}  // 命名空间 sbm::gui
