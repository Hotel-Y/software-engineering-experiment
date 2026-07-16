#include <windows.h>
#include <shlobj.h>
#include <CommCtrl.h>

#include "GuiDefaults.h"

#include <string>
#include <vector>

namespace {
constexpr int idSource = 101;
constexpr int idBackup = 102;
constexpr int idArchive = 103;
constexpr int idRestore = 104;
constexpr int idPassword = 105;
constexpr int idOutput = 106;
constexpr int idStatus = 107;
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
    SendMessageW(outputBox, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(text.c_str()));
}

void publishCommandOutput(const std::wstring& text,
                          const std::wstring& heading,
                          bool append) {
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
}

bool runCommand(const std::wstring& command,
                const std::wstring& heading = L"",
                bool append = false) {
    // Capture the child process stdout+stderr through an anonymous pipe.
    // CreateProcessW does not interpret shell redirection (">", "2>&1"), so we
    // must redirect via STARTUPINFO handles instead of relying on cmd syntax.
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
    // The parent's read end must not be inherited by the child.
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
    // The child has inherited its own copy of the write end; close ours so
    // ReadFile returns EOF once the child exits.
    CloseHandle(writeEnd);

    if (!launched) {
        CloseHandle(readEnd);
        publishCommandOutput(L"Failed to start sbm.exe. Please build the project first.",
                             heading, append);
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

    if (bytes.empty()) {
        publishCommandOutput(L"(command produced no output)", heading, append);
        return exitCode == 0;
    }

    int wideSize = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                        static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring output;
    const UINT codePage = wideSize > 0 ? CP_UTF8 : CP_ACP;
    if (wideSize <= 0) {
        wideSize = MultiByteToWideChar(CP_ACP, 0, bytes.data(),
                                       static_cast<int>(bytes.size()), nullptr, 0);
    }
    output.resize(wideSize);
    MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()),
                        output.data(), wideSize);
    publishCommandOutput(output, heading, append);
    return exitCode == 0;
}

bool ensureBackupReady(const std::wstring& source,
                       const std::wstring& backup,
                       const std::wstring& requestedAction) {
    const auto preparation = sbm::gui::prepareBackupForDependentAction(source, backup);
    if (preparation == sbm::gui::BackupPreparation::Ready) {
        return true;
    }
    if (preparation == sbm::gui::BackupPreparation::Unavailable) {
        setOutput(requestedAction +
                  L" needs a manifest backup. For an .sba input, click Unpack; "
                  L"otherwise choose an existing source or backup directory.");
        return false;
    }
    return runCommand(L"backup " + quote(source) + L" " + quote(backup),
                      L"Step 1/2: creating the prerequisite backup");
}

void applyDefaultFont(HWND control) {
    SendMessageW(control, WM_SETFONT,
                 reinterpret_cast<WPARAM>(uiFont != nullptr
                                              ? uiFont
                                              : GetStockObject(DEFAULT_GUI_FONT)),
                 TRUE);
}

void addLabel(HWND window, const wchar_t* text, int x, int y) {
    const auto control = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                       x, y, 80, 22, window, nullptr, nullptr, nullptr);
    applyDefaultFont(control);
}

void addEdit(HWND window, int id, int x, int y, int width) {
    const auto control = CreateWindowW(
        L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        x, y, width, 24, window, reinterpret_cast<HMENU>(id), nullptr, nullptr);
    applyDefaultFont(control);
}

void addButton(HWND window, int id, const wchar_t* text, int x, int y) {
    const auto control = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE,
                                       x, y, 100, 30, window,
                                       reinterpret_cast<HMENU>(id), nullptr, nullptr);
    applyDefaultFont(control);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        uiFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        addLabel(window, L"Input", 20, 22);
        addLabel(window, L"Backup", 20, 58);
        addLabel(window, L"Archive", 20, 94);
        addLabel(window, L"Restore", 20, 130);
        addLabel(window, L"Password", 20, 166);
        addEdit(window, idSource, 110, 20, 445);
        SendMessageW(GetDlgItem(window, idSource), editSetCueBanner, TRUE,
                     reinterpret_cast<LPARAM>(L"Existing source, backup, or .sba path"));
        addButton(window, idAutoButton, L"Auto Fill", 570, 17);
        addEdit(window, idBackup, 110, 56, 560);
        addEdit(window, idArchive, 110, 92, 560);
        addEdit(window, idRestore, 110, 128, 560);
        addEdit(window, idPassword, 110, 164, 220);
        const auto compressionLabel = CreateWindowW(
            L"STATIC", L"Compression: LZ77 + Huffman (block-wise)",
            WS_CHILD | WS_VISIBLE, 360, 168, 300, 18, window, nullptr, nullptr, nullptr);
        applyDefaultFont(compressionLabel);
        statusLabel = CreateWindowW(
            L"STATIC", L"Default rules: all files, automatic compression, no overwrite.",
            WS_CHILD | WS_VISIBLE, 20, 198, 650, 38, window,
            reinterpret_cast<HMENU>(idStatus), nullptr, nullptr);
        applyDefaultFont(statusLabel);
        addButton(window, idBackupButton, L"Backup", 20, 240);
        addButton(window, idPackButton, L"Pack", 140, 240);
        addButton(window, idUnpackButton, L"Unpack", 260, 240);
        addButton(window, idRestoreButton, L"Restore", 380, 240);
        addButton(window, idVerifyButton, L"Verify", 500, 240);
        outputBox = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                                      ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                  20, 282, 650, 194, window,
                                  reinterpret_cast<HMENU>(idOutput), nullptr, nullptr);
        applyDefaultFont(outputBox);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int notification = HIWORD(wParam);
        if (id == idAutoButton || (id == idSource && notification == EN_KILLFOCUS)) {
            applySmartDefaults(window);
            if (id == idAutoButton) {
                return 0;
            }
        }

        const auto source = getText(window, idSource);
        const auto backup = getText(window, idBackup);
        const auto archive = getText(window, idArchive);
        const auto restore = getText(window, idRestore);
        const auto password = getText(window, idPassword);

        if (id == idBackupButton) {
            if (source.empty() || backup.empty()) {
                setOutput(L"Enter a source path and use Auto Fill before starting Backup.");
                return 0;
            }
            runCommand(L"backup " + quote(source) + L" " + quote(backup));
        } else if (id == idPackButton) {
            if (backup.empty() || archive.empty()) {
                setOutput(L"Backup and archive paths are required for Pack.");
                return 0;
            }
            auto command = L"pack " + quote(backup) + L" " + quote(archive);
            if (!password.empty()) command += L" " + quote(L"--password=" + password);
            const auto preparation =
                sbm::gui::prepareBackupForDependentAction(source, backup);
            if (!ensureBackupReady(source, backup, L"Pack")) {
                return 0;
            }
            runCommand(command,
                       preparation == sbm::gui::BackupPreparation::CreateFromSource
                           ? L"Step 2/2: packing the backup"
                           : L"",
                       preparation == sbm::gui::BackupPreparation::CreateFromSource);
        } else if (id == idUnpackButton) {
            if (archive.empty() || backup.empty()) {
                setOutput(L"Archive and unpack output paths are required for Unpack.");
                return 0;
            }
            std::error_code archiveError;
            if (!std::filesystem::is_regular_file(archive, archiveError)) {
                setOutput(L"Archive file not found. To create one from a source directory, "
                          L"click Pack; the prerequisite Backup will run automatically.");
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
            const auto preparation =
                sbm::gui::prepareBackupForDependentAction(source, backup);
            if (!ensureBackupReady(source, backup, L"Restore")) {
                return 0;
            }
            runCommand(L"restore " + quote(backup) + L" " + quote(restore),
                       preparation == sbm::gui::BackupPreparation::CreateFromSource
                           ? L"Step 2/2: restoring the backup"
                           : L"",
                       preparation == sbm::gui::BackupPreparation::CreateFromSource);
        } else if (id == idVerifyButton) {
            if (backup.empty()) {
                setOutput(L"A backup path is required for Verify.");
                return 0;
            }
            const auto preparation =
                sbm::gui::prepareBackupForDependentAction(source, backup);
            if (!ensureBackupReady(source, backup, L"Verify")) {
                return 0;
            }
            runCommand(L"verify " + quote(backup),
                       preparation == sbm::gui::BackupPreparation::CreateFromSource
                           ? L"Step 2/2: verifying the backup"
                           : L"",
                       preparation == sbm::gui::BackupPreparation::CreateFromSource);
        }
        return 0;
    }
    case WM_DESTROY:
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

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    const wchar_t className[] = L"SimpleBackupManagerGui";
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
    if (!window) {
        return 1;
    }

    ShowWindow(window, showCommand);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
