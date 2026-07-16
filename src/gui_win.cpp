#include <windows.h>
#include <shlobj.h>
#include <CommCtrl.h>

#include "GuiDefaults.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace {
// 控件 ID 将 Win32 的 WM_COMMAND 消息映射到具体输入框和按钮。
constexpr int idSource = 101;
constexpr int idBackup = 102;
constexpr int idArchive = 103;
constexpr int idRestore = 104;
constexpr int idPassword = 105;
constexpr int idOutput = 106;
constexpr int idStatus = 107;
constexpr int idBrowseSource = 108;
constexpr int idBrowseBackup = 109;
constexpr int idBrowseArchive = 110;
constexpr int idBrowseRestore = 111;
constexpr int idExtCombo = 112;
constexpr int idNameCombo = 113;
constexpr int idPathCombo = 114;
constexpr int idMaxSizeEdit = 115;
constexpr int idModifiedAfterCombo = 116;
constexpr int idModifiedBeforeCombo = 117;
constexpr int idBackupButton = 201;
constexpr int idPackButton = 202;
constexpr int idUnpackButton = 203;
constexpr int idRestoreButton = 204;
constexpr int idVerifyButton = 205;
constexpr int idAutoButton = 206;
constexpr UINT editSetCueBanner = 0x1501;

HWND outputBox = nullptr;
HWND statusLabel = nullptr;
HFONT uiFont = nullptr;

// 从指定编辑框读取 Unicode 文本，所有路径始终以宽字符在 GUI 内流转。
std::wstring getText(HWND window, int id) {
    wchar_t buffer[1024]{};
    GetWindowTextW(GetDlgItem(window, id), buffer, 1024);
    return buffer;
}

// 输出区提供“替换”和“追加”两种写法，供单步骤与自动两步骤操作复用。
void setOutput(const std::wstring& text) {
    SetWindowTextW(outputBox, text.c_str());
}

void appendOutput(const std::wstring& text) {
    const auto length = GetWindowTextLengthW(outputBox);
    SendMessageW(outputBox, EM_SETSEL, length, length);
    SendMessageW(outputBox, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

// 给命令输出补充阶段标题，并根据调用场景选择覆盖还是追加。
void publishCommandOutput(const std::wstring& text, const std::wstring& heading, bool append) {
    // 多步骤操作可追加第二阶段输出，单步骤操作则替换旧结果。
    auto message = heading.empty() ? text : heading + L"\r\n" + text;
    if (append) {
        appendOutput(L"\r\n\r\n" + message);
    } else {
        setOutput(message);
    }
}

// 按控件 ID 填写宽字符文本，是智能默认值写回界面的统一入口。
void setText(HWND window, int id, const std::wstring& text) {
    SetWindowTextW(GetDlgItem(window, id), text.c_str());
}

std::wstring quote(const std::wstring& value) {
    // 路径和密码统一加引号，避免空格被 CreateProcessW 拆成多个参数。
    return L"\"" + value + L"\"";
}

std::wstring executablePath() {
    // GUI 与核心程序部署在同一目录，不依赖当前工作目录或系统 PATH。
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring fullPath = path;
    const auto slash = fullPath.find_last_of(L"\\/");
    const auto dir = slash == std::wstring::npos ? L"." : fullPath.substr(0, slash);
    return dir + L"\\sbm.exe";
}

// 读取下拉框当前选项；未选择时返回空字符串，调用方据此省略 CLI 参数。
std::wstring getComboText(HWND window, int comboId) {
    const HWND combo = GetDlgItem(window, comboId);
    const int idx = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (idx < 0) return L"";
    wchar_t buffer[256]{};
    SendMessageW(combo, CB_GETLBTEXT, idx, reinterpret_cast<LPARAM>(buffer));
    return buffer;
}

// 重置动态筛选下拉框并恢复首项“(none)”，表示不启用该过滤条件。
void clearCombo(HWND window, int comboId) {
    const HWND combo = GetDlgItem(window, comboId);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(none)"));
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

// 向下拉框批量加入固定候选值，并默认选中第一项。
void addComboStrings(HWND window, int comboId, const std::vector<const wchar_t*>& items) {
    const HWND combo = GetDlgItem(window, comboId);
    for (const auto* item : items) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

// 创建只能选择预设项的下拉框，避免日期等规则出现任意格式输入。
void addCombo(HWND window, int id, int x, int y, int width) {
    CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                  x, y, width, 200, window, reinterpret_cast<HMENU>(id), nullptr, nullptr);
}

// 路径输入框旁的“...”按钮只负责触发对应系统选择对话框。
void addBrowseButton(HWND window, int id, int x, int y) {
    CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE,
                  x, y, 30, 24, window, reinterpret_cast<HMENU>(id), nullptr, nullptr);
}

// 递归扫描源目录，从真实文件中提取扩展名、文件主名和顶层目录作为筛选候选。
void scanSourceAndPopulateFilters(HWND window, const std::wstring& sourcePath) {
    if (sourcePath.empty()) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(sourcePath, ec)) return;

    // set 同时完成去重和排序，使下拉选项稳定且易查找。
    std::set<std::wstring> exts, names, paths;
    for (const auto& item : std::filesystem::recursive_directory_iterator(
             sourcePath, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (item.is_regular_file()) {
            const auto& filename = item.path().filename().wstring();
            const auto dot = filename.find_last_of(L'.');
            if (dot != std::wstring::npos) {
                exts.insert(filename.substr(dot));
                names.insert(filename.substr(0, dot));
            }
        }
        const auto rel = std::filesystem::relative(item.path(), sourcePath, ec);
        if (!ec && !rel.empty()) {
            paths.insert(rel.begin()->wstring());
        }
    }
    clearCombo(window, idExtCombo);
    clearCombo(window, idNameCombo);
    clearCombo(window, idPathCombo);
    for (const auto& e : exts) {
        SendMessageW(GetDlgItem(window, idExtCombo), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(e.c_str()));
    }
    for (const auto& n : names) {
        SendMessageW(GetDlgItem(window, idNameCombo), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n.c_str()));
    }
    for (const auto& p : paths) {
        SendMessageW(GetDlgItem(window, idPathCombo), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.c_str()));
    }
}

// 打开 Windows 文件夹选择器；选择源目录后还会立即刷新自动筛选候选。
void browseFolder(HWND window, int editId) {
    BROWSEINFOW bi{};
    bi.hwndOwner = window;
    bi.lpszTitle = L"Select a folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    const auto pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH]{};
        SHGetPathFromIDListW(pidl, path);
        CoTaskMemFree(pidl);
        SetWindowTextW(GetDlgItem(window, editId), path);
        if (editId == idSource) {
            scanSourceAndPopulateFilters(window, std::wstring(path));
        }
    }
}

// 打开 SBA 保存文件对话框，并把最终文件名写回 Archive 输入框。
void browseFile(HWND window, int editId) {
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window;
    ofn.lpstrFilter = L"Simple Backup Archive (*.sba)\0*.sba\0All Files (*.*)\0*.*\0";
    ofn.lpstrDefExt = L"sba";
    wchar_t path[MAX_PATH]{};
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetSaveFileNameW(&ofn)) {
        SetWindowTextW(GetDlgItem(window, editId), path);
    }
}

// 根据一个输入路径填充备份、归档、还原目标，并联动扫描默认筛选项。
void applySmartDefaults(HWND window) {
    // 用户只输入一个路径，推断模块一次性生成其余路径和推荐操作。
    const auto defaults = sbm::gui::inferSmartDefaults(getText(window, idSource));
    if (!defaults.valid()) {
        SetWindowTextW(statusLabel, defaults.message.c_str());
        return;
    }
    setText(window, idSource, defaults.source.wstring());
    setText(window, idBackup, defaults.backup.wstring());
    setText(window, idArchive, defaults.archive.wstring());
    setText(window, idRestore, defaults.restore.wstring());
    const auto status = L"Recommended: " + defaults.recommendedAction + L". " + defaults.message;
    SetWindowTextW(statusLabel, status.c_str());
    scanSourceAndPopulateFilters(window, defaults.source.wstring());
}

// 无控制台启动 sbm.exe，捕获合并后的 stdout/stderr，并返回其退出码是否为 0。
bool runCommand(const std::wstring& command, const std::wstring& heading = L"", bool append = false) {
    // 通过匿名管道捕获子进程的标准输出和错误输出。CreateProcessW 不解释“>”或
    // “2>&1”等 shell 重定向，因此必须在 STARTUPINFO 中显式提供句柄。
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        publishCommandOutput(L"Failed to create output pipe.", heading, append);
        return false;
    }
    // 子进程不能继承父进程的读取端，否则管道可能无法正确产生 EOF。
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullptr;
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd;

    const auto fullCommand = quote(executablePath()) + L" " + command;
    std::vector<wchar_t> mutableCommand(fullCommand.begin(), fullCommand.end());
    mutableCommand.push_back(L'\0');

    PROCESS_INFORMATION process{};
    const BOOL launched = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr,
                                         TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                         &startup, &process);
    // 子进程已有写入端副本，父进程立即关闭自己的副本，子进程退出后 ReadFile 才会结束。
    CloseHandle(writeEnd);

    if (!launched) {
        CloseHandle(readEnd);
        publishCommandOutput(L"Failed to start sbm.exe. Please build the project first.", heading, append);
        return false;
    }

    // sbm 默认输出 UTF-8；若转换失败则回退到当前 Windows ANSI 代码页。
    std::string bytes;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        bytes.append(buffer, read);
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(readEnd);

    std::wstring output;
    if (bytes.empty()) {
        output = L"(command produced no output)";
    } else {
        int wideSize = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        const UINT codePage = wideSize > 0 ? CP_UTF8 : CP_ACP;
        if (wideSize <= 0) {
            wideSize = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        }
        output.resize(wideSize);
        MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), output.data(), wideSize);
    }
    publishCommandOutput(output, heading, append);
    return exitCode == 0;
}

// 为依赖 manifest.sbm 的操作补齐前置备份，或给出不可执行原因。
bool ensureBackupReady(const std::wstring& source, const std::wstring& backup, const std::wstring& action) {
    // Pack、Restore 和 Verify 都依赖清单；源目录输入时自动先执行一次 Backup。
    const auto preparation = sbm::gui::prepareBackupForDependentAction(source, backup);
    if (preparation == sbm::gui::BackupPreparation::CreateFromSource) {
        if (!runCommand(L"backup " + quote(source) + L" " + quote(backup) + L" --overwrite",
                        L"Step 1/2: creating backup")) {
            publishCommandOutput(L"Backup failed; cannot continue with " + action + L".", L"", true);
            return false;
        }
    } else if (preparation == sbm::gui::BackupPreparation::Unavailable) {
        setOutput(L"Backup not found. To create one from a source directory, click Backup.");
        return false;
    }
    return true;
}

// 主窗口过程：创建控件、响应按钮命令，并在销毁时释放 GDI 资源。
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        // 窗口创建阶段一次性构造路径输入、筛选下拉框、按钮和多行输出区。
        uiFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        CreateWindowW(L"STATIC", L"Source:", WS_CHILD | WS_VISIBLE, 20, 22, 80, 22, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                      110, 20, 450, 24, window, reinterpret_cast<HMENU>(idSource), nullptr, nullptr);
        addBrowseButton(window, idBrowseSource, 565, 20);

        CreateWindowW(L"STATIC", L"Backup:", WS_CHILD | WS_VISIBLE, 20, 58, 80, 22, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                      110, 56, 450, 24, window, reinterpret_cast<HMENU>(idBackup), nullptr, nullptr);
        addBrowseButton(window, idBrowseBackup, 565, 56);

        CreateWindowW(L"STATIC", L"Archive:", WS_CHILD | WS_VISIBLE, 20, 94, 80, 22, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                      110, 92, 450, 24, window, reinterpret_cast<HMENU>(idArchive), nullptr, nullptr);
        addBrowseButton(window, idBrowseArchive, 565, 92);

        CreateWindowW(L"STATIC", L"Restore:", WS_CHILD | WS_VISIBLE, 20, 130, 80, 22, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                      110, 128, 450, 24, window, reinterpret_cast<HMENU>(idRestore), nullptr, nullptr);
        addBrowseButton(window, idBrowseRestore, 565, 128);

        CreateWindowW(L"STATIC", L"Password:", WS_CHILD | WS_VISIBLE, 20, 166, 80, 22, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                      110, 164, 220, 24, window, reinterpret_cast<HMENU>(idPassword), nullptr, nullptr);

        const auto fy1 = 200;
        CreateWindowW(L"STATIC", L"Ext:", WS_CHILD | WS_VISIBLE, 20, fy1 + 3, 40, 22, window, nullptr, nullptr, nullptr);
        addCombo(window, idExtCombo, 58, fy1, 100);
        CreateWindowW(L"STATIC", L"Name:", WS_CHILD | WS_VISIBLE, 168, fy1 + 3, 50, 22, window, nullptr, nullptr, nullptr);
        addCombo(window, idNameCombo, 215, fy1, 100);
        CreateWindowW(L"STATIC", L"Path:", WS_CHILD | WS_VISIBLE, 325, fy1 + 3, 50, 22, window, nullptr, nullptr, nullptr);
        addCombo(window, idPathCombo, 370, fy1, 100);
        CreateWindowW(L"STATIC", L"MaxSize (bytes):", WS_CHILD | WS_VISIBLE, 470, fy1 + 3, 110, 22, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                      580, fy1, 95, 24, window, reinterpret_cast<HMENU>(idMaxSizeEdit), nullptr, nullptr);

        const auto fy2 = 232;
        CreateWindowW(L"STATIC", L"After:", WS_CHILD | WS_VISIBLE, 20, fy2 + 3, 50, 22, window, nullptr, nullptr, nullptr);
        addCombo(window, idModifiedAfterCombo, 60, fy2, 120);
        CreateWindowW(L"STATIC", L"Before:", WS_CHILD | WS_VISIBLE, 190, fy2 + 3, 50, 22, window, nullptr, nullptr, nullptr);
        addCombo(window, idModifiedBeforeCombo, 245, fy2, 120);

        addComboStrings(window, idExtCombo, { L"(none)" });
        addComboStrings(window, idNameCombo, { L"(none)" });
        addComboStrings(window, idPathCombo, { L"(none)" });
        addComboStrings(window, idModifiedAfterCombo, { L"(none)", L"2024-01-01", L"2024-06-01", L"2025-01-01", L"2025-06-01", L"2026-01-01" });
        addComboStrings(window, idModifiedBeforeCombo, { L"(none)", L"2024-12-31", L"2025-06-30", L"2025-12-31", L"2026-06-30", L"2026-12-31" });

        const auto btnY = 270;
        CreateWindowW(L"BUTTON", L"Auto Fill", WS_CHILD | WS_VISIBLE, 20, btnY, 100, 30, window, reinterpret_cast<HMENU>(idAutoButton), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Backup", WS_CHILD | WS_VISIBLE, 130, btnY, 100, 30, window, reinterpret_cast<HMENU>(idBackupButton), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Pack", WS_CHILD | WS_VISIBLE, 240, btnY, 100, 30, window, reinterpret_cast<HMENU>(idPackButton), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Unpack", WS_CHILD | WS_VISIBLE, 350, btnY, 100, 30, window, reinterpret_cast<HMENU>(idUnpackButton), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Restore", WS_CHILD | WS_VISIBLE, 460, btnY, 100, 30, window, reinterpret_cast<HMENU>(idRestoreButton), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Verify", WS_CHILD | WS_VISIBLE, 570, btnY, 100, 30, window, reinterpret_cast<HMENU>(idVerifyButton), nullptr, nullptr);

        statusLabel = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 20, 312, 660, 18, window, nullptr, nullptr, nullptr);

        outputBox = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                  20, 340, 660, 160, window, reinterpret_cast<HMENU>(idOutput), nullptr, nullptr);

        SendMessageW(GetDlgItem(window, idSource), WM_SETFONT, reinterpret_cast<WPARAM>(uiFont), TRUE);
        return 0;
    }
    case WM_COMMAND: {
        // 智能填充按钮和源路径失焦都触发推断，其余消息按按钮 ID 分发。
        const int id = LOWORD(wParam);
        const auto source = getText(window, idSource);
        const auto backup = getText(window, idBackup);
        const auto archive = getText(window, idArchive);
        const auto restore = getText(window, idRestore);
        const auto password = getText(window, idPassword);

        // 浏览按钮直接写回路径；Auto Fill 再根据输入生成其他默认路径和筛选项。
        if (id == idBrowseSource) { browseFolder(window, idSource); return 0; }
        if (id == idBrowseBackup) { browseFolder(window, idBackup); return 0; }
        if (id == idBrowseArchive) { browseFile(window, idArchive); return 0; }
        if (id == idBrowseRestore) { browseFolder(window, idRestore); return 0; }

        if (id == idAutoButton) {
            applySmartDefaults(window);
            return 0;
        }

        // GUI 只组合 CLI 参数，不重复实现文件处理逻辑。
        if (id == idBackupButton) {
            if (source.empty() || backup.empty()) {
                setOutput(L"Enter a source path and use Auto Fill before starting Backup.");
                return 0;
            }
            // 只把用户实际选择的规则附加到命令行；“(none)”保持核心默认行为。
            std::wstring cmd = L"backup " + quote(source) + L" " + quote(backup) + L" --overwrite";
            const auto ext = getComboText(window, idExtCombo);
            const auto name = getComboText(window, idNameCombo);
            const auto path = getComboText(window, idPathCombo);
            const auto maxSize = getText(window, idMaxSizeEdit);
            const auto after = getComboText(window, idModifiedAfterCombo);
            const auto before = getComboText(window, idModifiedBeforeCombo);
            if (!ext.empty() && ext != L"(none)") cmd += L" --ext=" + ext;
            if (!name.empty() && name != L"(none)") cmd += L" --name-contains=" + name;
            if (!path.empty() && path != L"(none)") cmd += L" --path-contains=" + path;
            if (!maxSize.empty()) cmd += L" --max-size=" + maxSize;
            if (!after.empty() && after != L"(none)") cmd += L" --modified-after=" + after;
            if (!before.empty() && before != L"(none)") cmd += L" --modified-before=" + before;
            runCommand(cmd);
        } else if (id == idPackButton) {
            if (backup.empty() || archive.empty()) {
                setOutput(L"Backup and archive paths are required for Pack.");
                return 0;
            }
            auto command = L"pack " + quote(backup) + L" " + quote(archive);
            if (!password.empty()) command += L" " + quote(L"--password=" + password);
            const auto preparation = sbm::gui::prepareBackupForDependentAction(source, backup);
            if (!ensureBackupReady(source, backup, L"Pack")) return 0;
            runCommand(command, preparation == sbm::gui::BackupPreparation::CreateFromSource ? L"Step 2/2: packing the backup" : L"", preparation == sbm::gui::BackupPreparation::CreateFromSource);
        } else if (id == idUnpackButton) {
            if (archive.empty() || backup.empty()) {
                setOutput(L"Archive and unpack output paths are required for Unpack.");
                return 0;
            }
            std::error_code archiveError;
            if (!std::filesystem::is_regular_file(archive, archiveError)) {
                setOutput(L"Archive file not found. To create one from a source directory, click Pack.");
                return 0;
            }
            auto command = L"unpack " + quote(archive) + L" " + quote(backup);
            if (!password.empty()) command += L" " + quote(L"--password=" + password);
            runCommand(command);
        } else if (id == idRestoreButton) {
            if (backup.empty() || restore.empty()) {
                setOutput(L"Backup and restore paths are required for Restore.");
                return 0;
            }
            const auto preparation = sbm::gui::prepareBackupForDependentAction(source, backup);
            if (!ensureBackupReady(source, backup, L"Restore")) return 0;
            runCommand(L"restore " + quote(backup) + L" " + quote(restore), preparation == sbm::gui::BackupPreparation::CreateFromSource ? L"Step 2/2: restoring the backup" : L"", preparation == sbm::gui::BackupPreparation::CreateFromSource);
        } else if (id == idVerifyButton) {
            if (backup.empty()) {
                setOutput(L"A backup path is required for Verify.");
                return 0;
            }
            const auto preparation = sbm::gui::prepareBackupForDependentAction(source, backup);
            if (!ensureBackupReady(source, backup, L"Verify")) return 0;
            runCommand(L"verify " + quote(backup), preparation == sbm::gui::BackupPreparation::CreateFromSource ? L"Step 2/2: verifying the backup" : L"", preparation == sbm::gui::BackupPreparation::CreateFromSource);
        }
        return 0;
    }
    case WM_DESTROY:
        // 释放手工创建的 GDI 字体后退出消息循环。
        if (uiFont != nullptr) {
            DeleteObject(uiFont);
            uiFont = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}
}

// 程序入口：注册窗口类、创建主窗口，然后进入标准 Win32 消息循环。
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    const wchar_t className[] = L"SimpleBackupManagerGui";
    // 初始化组合框等通用控件，再注册本项目自己的主窗口类。
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND window = CreateWindowExW(0, className, L"Simple Backup Manager - Native GUI",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  720, 560, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    ShowWindow(window, showCommand);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
