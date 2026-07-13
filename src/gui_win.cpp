#include <windows.h>

#include <string>
#include <vector>

namespace {
constexpr int idSource = 101;
constexpr int idBackup = 102;
constexpr int idArchive = 103;
constexpr int idRestore = 104;
constexpr int idPassword = 105;
constexpr int idOutput = 106;
constexpr int idBackupButton = 201;
constexpr int idPackButton = 202;
constexpr int idUnpackButton = 203;
constexpr int idRestoreButton = 204;
constexpr int idVerifyButton = 205;

HWND outputBox = nullptr;

std::wstring getText(HWND window, int id) {
    wchar_t buffer[1024]{};
    GetWindowTextW(GetDlgItem(window, id), buffer, 1024);
    return buffer;
}

void setOutput(const std::wstring& text) {
    SetWindowTextW(outputBox, text.c_str());
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

void runCommand(const std::wstring& command) {
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
        setOutput(L"Failed to create output pipe.");
        return;
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
        setOutput(L"Failed to start sbm.exe. Please build the project first.");
        return;
    }

    std::string bytes;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        bytes.append(buffer, read);
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(readEnd);

    if (bytes.empty()) {
        setOutput(L"(command produced no output)");
        return;
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
    setOutput(output);
}

void addLabel(HWND window, const wchar_t* text, int x, int y) {
    CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
                  x, y, 80, 22, window, nullptr, nullptr, nullptr);
}

void addEdit(HWND window, int id, int x, int y, int width) {
    CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                  x, y, width, 24, window, reinterpret_cast<HMENU>(id), nullptr, nullptr);
}

void addButton(HWND window, int id, const wchar_t* text, int x, int y) {
    CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE,
                  x, y, 100, 30, window, reinterpret_cast<HMENU>(id), nullptr, nullptr);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        addLabel(window, L"Source", 20, 22);
        addLabel(window, L"Backup", 20, 58);
        addLabel(window, L"Archive", 20, 94);
        addLabel(window, L"Restore", 20, 130);
        addLabel(window, L"Password", 20, 166);
        addEdit(window, idSource, 110, 20, 560);
        addEdit(window, idBackup, 110, 56, 560);
        addEdit(window, idArchive, 110, 92, 560);
        addEdit(window, idRestore, 110, 128, 560);
        addEdit(window, idPassword, 110, 164, 220);
        CreateWindowW(L"STATIC", L"Compression: LZ77 + Huffman (block-wise)",
                      WS_CHILD | WS_VISIBLE, 360, 168, 300, 18, window, nullptr, nullptr, nullptr);
        addButton(window, idBackupButton, L"Backup", 20, 210);
        addButton(window, idPackButton, L"Pack", 140, 210);
        addButton(window, idUnpackButton, L"Unpack", 260, 210);
        addButton(window, idRestoreButton, L"Restore", 380, 210);
        addButton(window, idVerifyButton, L"Verify", 500, 210);
        outputBox = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                                      ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                  20, 260, 650, 180, window, reinterpret_cast<HMENU>(idOutput), nullptr, nullptr);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const auto source = getText(window, idSource);
        const auto backup = getText(window, idBackup);
        const auto archive = getText(window, idArchive);
        const auto restore = getText(window, idRestore);
        const auto password = getText(window, idPassword);

        if (id == idBackupButton) {
            runCommand(L"backup " + quote(source) + L" " + quote(backup) + L" --overwrite");
        } else if (id == idPackButton) {
            auto command = L"pack " + quote(backup) + L" " + quote(archive);
            if (!password.empty()) command += L" --password=" + password;
            runCommand(command);
        } else if (id == idUnpackButton) {
            auto command = L"unpack " + quote(archive) + L" " + quote(backup);
            if (!password.empty()) command += L" --password=" + password;
            runCommand(command);
        } else if (id == idRestoreButton) {
            runCommand(L"restore " + quote(backup) + L" " + quote(restore));
        } else if (id == idVerifyButton) {
            runCommand(L"verify " + quote(backup));
        }
        return 0;
    }
    case WM_DESTROY:
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
                                  720, 500, nullptr, nullptr, instance, nullptr);
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
