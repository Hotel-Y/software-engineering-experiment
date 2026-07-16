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

std::wstring getText(HWND window, int id) {
    wchar_t buffer[1024]{};
    GetWindowTextW(GetDlgItem(window, id), buffer, 1024);
    return buffer;
}

void setOutput(const std::wstring& text) {
    SetWindowTextW(outputBox, text.c_str());
}

void appendOutput(const std::wstring& text) {
    const auto length = GetWindowTextLengthW(outputBox);
    SendMessageW(outputBox, EM_SETSEL, length, length);
    SendMessageW(outputBox, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

void publishCommandOutput(const std::wstring& text, const std::wstring& heading, bool append) {
    auto message = heading.empty() ? text : heading + L"\r\n" + text;
    if (append) {
        appendOutput(L"\r\n\r\n" + message);
    } else {
        setOutput(message);
    }
}

void setText(HWND window, int id, const std::wstring& text) {
    SetWindowTextW(GetDlgItem(window, id), text.c_str());
}

std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

std::wstring executablePath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring fullPath = path;
    const auto slash = fullPath.find_last_of(L"\\/");
    const auto dir = slash == std::wstring::npos ? L"." : fullPath.substr(0, slash);
    return dir + L"\\sbm.exe";
}

std::wstring getComboText(HWND window, int comboId) {
    const HWND combo = GetDlgItem(window, comboId);
    const int idx = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (idx < 0) return L"";
    wchar_t buffer[256]{};
    SendMessageW(combo, CB_GETLBTEXT, idx, reinterpret_cast<LPARAM>(buffer));
    return buffer;
}

void clearCombo(HWND window, int comboId) {
    const HWND combo = GetDlgItem(window, comboId);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(none)"));
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

void addComboStrings(HWND window, int comboId, const std::vector<const wchar_t*>& items) {
    const HWND combo = GetDlgItem(window, comboId);
    for (const auto* item : items) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

void addCombo(HWND window, int id, int x, int y, int width) {
    CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                  x, y, width, 200, window, reinterpret_cast<HMENU>(id), nullptr, nullptr);
}

void addBrowseButton(HWND window, int id, int x, int y) {
    CreateWindowW(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE,
                  x, y, 30, 24, window, reinterpret_cast<HMENU>(id), nullptr, nullptr);
}

void scanSourceAndPopulateFilters(HWND window, const std::wstring& sourcePath) {
    if (sourcePath.empty()) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(sourcePath, ec)) return;

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

void applySmartDefaults(HWND window) {
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

bool runCommand(const std::wstring& command, const std::wstring& heading = L"", bool append = false) {
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
    CloseHandle(writeEnd);

    if (!launched) {
        CloseHandle(readEnd);
        publishCommandOutput(L"Failed to start sbm.exe. Please build the project first.", heading, append);
        return false;
    }

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

bool ensureBackupReady(const std::wstring& source, const std::wstring& backup, const std::wstring& action) {
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

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
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
        const int id = LOWORD(wParam);
        const auto source = getText(window, idSource);
        const auto backup = getText(window, idBackup);
        const auto archive = getText(window, idArchive);
        const auto restore = getText(window, idRestore);
        const auto password = getText(window, idPassword);

        if (id == idBrowseSource) { browseFolder(window, idSource); return 0; }
        if (id == idBrowseBackup) { browseFolder(window, idBackup); return 0; }
        if (id == idBrowseArchive) { browseFile(window, idArchive); return 0; }
        if (id == idBrowseRestore) { browseFolder(window, idRestore); return 0; }

        if (id == idAutoButton) {
            applySmartDefaults(window);
            return 0;
        }

        if (id == idBackupButton) {
            if (source.empty() || backup.empty()) {
                setOutput(L"Enter a source path and use Auto Fill before starting Backup.");
                return 0;
            }
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
        if (uiFont != nullptr) { DeleteObject(uiFont); uiFont = nullptr; }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    const wchar_t className[] = L"SimpleBackupManagerGui";
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
