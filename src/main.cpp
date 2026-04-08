#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#include "resource.h"

namespace {
constexpr DWORD kExpectedOptionsSize = 931048;
constexpr size_t kOffsetUnlockA = 0x0C;
constexpr size_t kOffsetMirrorA = 0x0D;
constexpr size_t kOffsetMiniA = 0x0E;
constexpr size_t kOffsetUnlockBLegacy = 0x0F;
constexpr size_t kOffsetUnlockB = 0x10;
constexpr size_t kOffsetMirrorB = 0x11;
constexpr size_t kOffsetMiniB = 0x12;

constexpr int kIdEditPath = 1001;
constexpr int kIdCheckboxAll = 1002;
constexpr int kIdCheckboxMirror = 1003;
constexpr int kIdCheckboxMini = 1004;
constexpr int kIdCheckboxReset = 1005;
constexpr int kIdButtonApply = 1006;
constexpr int kIdMenuFileExit = 2001;
constexpr int kIdMenuAboutVersion = 2002;

constexpr DWORD kPatchAll = 1u << 0;
constexpr DWORD kPatchMirror = 1u << 1;
constexpr DWORD kPatchMini = 1u << 2;
constexpr DWORD kPatchReset = 1u << 3;

struct AppState {
    wchar_t installPath[1024]{};
    wchar_t installPathDisplay[1024]{};
    bool installDetected = false;
    HWND editPath = nullptr;
    HWND cbAll = nullptr;
    HWND cbMirror = nullptr;
    HWND cbMini = nullptr;
    HWND cbReset = nullptr;
};

bool IsChecked(HWND checkbox) {
    return SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void SetChecked(HWND checkbox, bool checked) {
    SendMessageW(checkbox, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool FileExists(const wchar_t* path) {
    const DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const wchar_t* path) {
    const DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool BuildPath(const wchar_t* base, const wchar_t* suffix, wchar_t* outPath, size_t outCount) {
    if (!base || !suffix || !outPath || outCount == 0) {
        return false;
    }

    outPath[0] = L'\0';

    size_t i = 0;
    for (; base[i] != L'\0' && i + 1 < outCount; ++i) {
        outPath[i] = base[i];
    }

    if (base[i] != L'\0') {
        return false;
    }

    size_t j = 0;
    while (suffix[j] != L'\0' && i + 1 < outCount) {
        outPath[i++] = suffix[j++];
    }

    if (suffix[j] != L'\0') {
        return false;
    }

    outPath[i] = L'\0';
    return true;
}

bool ReadInstallPathFromRegistry(wchar_t* outPath, DWORD outChars) {
    if (!outPath || outChars == 0) {
        return false;
    }

    const wchar_t* subKey = L"SOFTWARE\\Codemasters\\Colin McRae Rally 2005";
    const wchar_t* valueName = L"INSTALL_PATH";
    const REGSAM accessModes[] = {KEY_READ | KEY_WOW64_32KEY, KEY_READ};

    outPath[0] = L'\0';

    for (REGSAM access : accessModes) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, access, &key) != ERROR_SUCCESS) {
            continue;
        }

        DWORD type = 0;
        DWORD sizeBytes = outChars * sizeof(wchar_t);
        LONG valueResult = RegQueryValueExW(
            key,
            valueName,
            nullptr,
            &type,
            reinterpret_cast<LPBYTE>(outPath),
            &sizeBytes);

        RegCloseKey(key);

        if (valueResult == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && outPath[0] != L'\0') {
            if (type == REG_EXPAND_SZ) {
                wchar_t expanded[1024]{};
                DWORD expandedLen = ExpandEnvironmentStringsW(outPath, expanded, static_cast<DWORD>(_countof(expanded)));
                if (expandedLen == 0 || expandedLen > _countof(expanded)) {
                    return false;
                }
                lstrcpynW(outPath, expanded, outChars);
            }
            return true;
        }
    }

    return false;
}

bool ReadFileBytes(const wchar_t* path, BYTE** outBuffer, DWORD* outSize, DWORD* outError) {
    if (!path || !outBuffer || !outSize) {
        if (outError) {
            *outError = ERROR_INVALID_PARAMETER;
        }
        return false;
    }

    *outBuffer = nullptr;
    *outSize = 0;
    if (outError) {
        *outError = ERROR_SUCCESS;
    }

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (outError) {
            *outError = GetLastError();
        }
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 0x7FFFFFFF) {
        if (outError) {
            *outError = GetLastError();
        }
        CloseHandle(file);
        return false;
    }

    DWORD fileSize = static_cast<DWORD>(size.QuadPart);
    BYTE* buffer = nullptr;

    if (fileSize > 0) {
        buffer = new (std::nothrow) BYTE[fileSize];
        if (!buffer) {
            if (outError) {
                *outError = ERROR_OUTOFMEMORY;
            }
            CloseHandle(file);
            return false;
        }

        DWORD bytesRead = 0;
        if (!ReadFile(file, buffer, fileSize, &bytesRead, nullptr) || bytesRead != fileSize) {
            if (outError) {
                *outError = GetLastError();
            }
            delete[] buffer;
            CloseHandle(file);
            return false;
        }
    }

    CloseHandle(file);
    *outBuffer = buffer;
    *outSize = fileSize;
    return true;
}

bool WriteFileBytes(const wchar_t* path, const BYTE* buffer, DWORD size, DWORD* outError) {
    if (outError) {
        *outError = ERROR_SUCCESS;
    }

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (outError) {
            *outError = GetLastError();
        }
        return false;
    }

    DWORD bytesWritten = 0;
    bool ok = true;
    if (size > 0) {
        ok = WriteFile(file, buffer, size, &bytesWritten, nullptr) && bytesWritten == size;
    }

    if (!ok && outError) {
        *outError = GetLastError();
    }

    CloseHandle(file);
    return ok;
}

void ApplyPatch(BYTE* data, DWORD flags) {
    const bool preResetForModes = (flags & (kPatchMirror | kPatchMini)) != 0u;

    if (preResetForModes || (flags & kPatchReset) != 0u) {
        // Explicit default reset before applying mirror/mini (or reset-only).
        data[kOffsetUnlockA] = 0x00;
        data[kOffsetMirrorA] = 0x00;
        data[kOffsetMiniA] = 0x00;
        data[kOffsetUnlockBLegacy] = 0x00;
        data[kOffsetUnlockB] = 0x00;
        data[kOffsetMirrorB] = 0x00;
        data[kOffsetMiniB] = 0x00;
    }

    if ((flags & kPatchReset) != 0u) {
        return;
    }

    if ((flags & kPatchAll) != 0u) {
        data[kOffsetUnlockA] = 0xFF;
        data[kOffsetUnlockB] = 0xFF;
    }

    if ((flags & kPatchMirror) != 0u) {
        data[kOffsetMirrorA] = 0xFF;
        data[kOffsetMirrorB] = 0xFF;
    }

    if ((flags & kPatchMini) != 0u) {
        // Mini compatibility mode:
        // - First block:  FF 00 FF at 0x0C..0x0E
        // - Second block: write both offset interpretations:
        //   0x0F..0x11 and 0x10..0x12 => final 0x0F..0x12 = FF
        data[kOffsetUnlockA] = 0xFF;
        if ((flags & kPatchMirror) == 0u) {
            data[kOffsetMirrorA] = 0x00;
        }
        data[kOffsetMiniA] = 0xFF;

        data[kOffsetUnlockBLegacy] = 0xFF;
        data[kOffsetUnlockB] = 0xFF;
        data[kOffsetMirrorB] = 0xFF;
        data[kOffsetMiniB] = 0xFF;
    }
}

enum class PatchStatus {
    Ok = 0,
    InvalidInput,
    OptionsMissing,
    ReadFailed,
    SizeTooSmall,
    WriteFailed,
};

PatchStatus PatchOptionsFile(
    const wchar_t* installPath,
    DWORD flags,
    bool allowUnexpectedSize,
    bool* outNeedsElevation,
    DWORD* outError,
    bool* outUnexpectedSize) {

    if (outNeedsElevation) {
        *outNeedsElevation = false;
    }
    if (outError) {
        *outError = ERROR_SUCCESS;
    }
    if (outUnexpectedSize) {
        *outUnexpectedSize = false;
    }

    if (!installPath || flags == 0u) {
        return PatchStatus::InvalidInput;
    }

    wchar_t optionsPath[1200]{};
    wchar_t backupPath[1210]{};

    if (!BuildPath(installPath, L"\\SG\\OPTIONS", optionsPath, _countof(optionsPath))) {
        if (outError) {
            *outError = ERROR_BUFFER_OVERFLOW;
        }
        return PatchStatus::InvalidInput;
    }

    if (!FileExists(optionsPath)) {
        return PatchStatus::OptionsMissing;
    }

    BYTE* data = nullptr;
    DWORD dataSize = 0;
    DWORD readError = ERROR_SUCCESS;
    if (!ReadFileBytes(optionsPath, &data, &dataSize, &readError)) {
        if (outError) {
            *outError = readError;
        }
        if (outNeedsElevation && (readError == ERROR_ACCESS_DENIED || readError == ERROR_SHARING_VIOLATION)) {
            *outNeedsElevation = true;
        }
        return PatchStatus::ReadFailed;
    }

    if (dataSize != kExpectedOptionsSize) {
        if (outUnexpectedSize) {
            *outUnexpectedSize = true;
        }
        if (!allowUnexpectedSize) {
            delete[] data;
            return PatchStatus::ReadFailed;
        }
    }

    if (dataSize < 0x13) {
        delete[] data;
        if (outError) {
            *outError = ERROR_INVALID_DATA;
        }
        return PatchStatus::SizeTooSmall;
    }

    ApplyPatch(data, flags);

    if (BuildPath(optionsPath, L".bak", backupPath, _countof(backupPath)) && !FileExists(backupPath)) {
        if (!CopyFileW(optionsPath, backupPath, TRUE)) {
            DWORD backupError = GetLastError();
            if (backupError == ERROR_ACCESS_DENIED || backupError == ERROR_SHARING_VIOLATION) {
                delete[] data;
                if (outNeedsElevation) {
                    *outNeedsElevation = true;
                }
                if (outError) {
                    *outError = backupError;
                }
                return PatchStatus::WriteFailed;
            }
        }
    }

    DWORD writeError = ERROR_SUCCESS;
    if (!WriteFileBytes(optionsPath, data, dataSize, &writeError)) {
        delete[] data;
        if (outError) {
            *outError = writeError;
        }
        if (outNeedsElevation && (writeError == ERROR_ACCESS_DENIED || writeError == ERROR_SHARING_VIOLATION)) {
            *outNeedsElevation = true;
        }
        return PatchStatus::WriteFailed;
    }

    delete[] data;
    return PatchStatus::Ok;
}

bool LaunchElevatedPatchAndWait(HWND owner, const wchar_t* installPath, DWORD flags, DWORD* outExitCode, DWORD* outLaunchError) {
    if (outExitCode) {
        *outExitCode = 1;
    }
    if (outLaunchError) {
        *outLaunchError = ERROR_SUCCESS;
    }

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, _countof(exePath)) == 0) {
        if (outLaunchError) {
            *outLaunchError = GetLastError();
        }
        return false;
    }

    wchar_t parameters[1400]{};
    if (FAILED(StringCchPrintfW(parameters, _countof(parameters), L"--elevated-patch \"%s\" %lu", installPath, flags))) {
        if (outLaunchError) {
            *outLaunchError = ERROR_BUFFER_OVERFLOW;
        }
        return false;
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = parameters;
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        if (outLaunchError) {
            *outLaunchError = GetLastError();
        }
        return false;
    }

    WaitForSingleObject(sei.hProcess, INFINITE);

    DWORD exitCode = 1;
    GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);

    if (outExitCode) {
        *outExitCode = exitCode;
    }

    return exitCode == 0;
}

void HandleResetMutualExclusion(HWND hwnd, AppState* state, bool resetChecked) {
    if (!state) {
        return;
    }

    if (resetChecked) {
        SetChecked(state->cbAll, false);
        SetChecked(state->cbMirror, false);
        SetChecked(state->cbMini, false);
        EnableWindow(state->cbAll, FALSE);
        EnableWindow(state->cbMirror, FALSE);
        EnableWindow(state->cbMini, FALSE);
    } else {
        EnableWindow(state->cbAll, TRUE);
        EnableWindow(state->cbMirror, TRUE);
        EnableWindow(state->cbMini, TRUE);
    }

    InvalidateRect(hwnd, nullptr, TRUE);
}

DWORD BuildPatchFlagsFromUI(const AppState* state) {
    DWORD flags = 0;

    if (state && IsChecked(state->cbAll)) {
        flags |= kPatchAll;
    }
    if (state && IsChecked(state->cbMirror)) {
        flags |= kPatchMirror;
    }
    if (state && IsChecked(state->cbMini)) {
        flags |= kPatchMini;
    }
    if (state && IsChecked(state->cbReset)) {
        flags |= kPatchReset;
    }

    return flags;
}

bool ImpliesUnlockAll(DWORD flags) {
    return (flags & (kPatchAll | kPatchMini)) != 0u;
}
HMENU BuildAppMenu() {
    HMENU mainMenu = CreateMenu();
    HMENU fileMenu = CreatePopupMenu();
    HMENU aboutMenu = CreatePopupMenu();

    if (!mainMenu || !fileMenu || !aboutMenu) {
        if (aboutMenu) {
            DestroyMenu(aboutMenu);
        }
        if (fileMenu) {
            DestroyMenu(fileMenu);
        }
        if (mainMenu) {
            DestroyMenu(mainMenu);
        }
        return nullptr;
    }

    AppendMenuW(fileMenu, MF_STRING, static_cast<UINT_PTR>(kIdMenuFileExit), L"Exit");
    AppendMenuW(mainMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"File");

    AppendMenuW(aboutMenu, MF_STRING, static_cast<UINT_PTR>(kIdMenuAboutVersion), L"Version");
    AppendMenuW(mainMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(aboutMenu), L"About");

    return mainMenu;
}

bool CheckWriteAccessForPatch(const wchar_t* installPath, DWORD* outError) {
    if (outError) {
        *outError = ERROR_SUCCESS;
    }

    if (!installPath || installPath[0] == L'\0') {
        if (outError) {
            *outError = ERROR_INVALID_PARAMETER;
        }
        return false;
    }

    wchar_t optionsPath[1200]{};
    if (!BuildPath(installPath, L"\\SG\\OPTIONS", optionsPath, _countof(optionsPath))) {
        if (outError) {
            *outError = ERROR_BUFFER_OVERFLOW;
        }
        return false;
    }

    if (!FileExists(optionsPath)) {
        return true;
    }

    HANDLE optionsHandle = CreateFileW(
        optionsPath,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (optionsHandle == INVALID_HANDLE_VALUE) {
        if (outError) {
            *outError = GetLastError();
        }
        return false;
    }
    CloseHandle(optionsHandle);

    wchar_t testPath[1220]{};
    if (!BuildPath(installPath, L"\\SG\\.__cmr5_write_test.tmp", testPath, _countof(testPath))) {
        if (outError) {
            *outError = ERROR_BUFFER_OVERFLOW;
        }
        return false;
    }

    if (FileExists(testPath)) {
        DeleteFileW(testPath);
    }

    HANDLE testHandle = CreateFileW(
        testPath,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (testHandle == INVALID_HANDLE_VALUE) {
        if (outError) {
            *outError = GetLastError();
        }
        return false;
    }

    CloseHandle(testHandle);
    DeleteFileW(testPath);
    return true;
}

void RunPatch(HWND hwnd, AppState* state) {
    if (!state) {
        return;
    }

    if (!state->installDetected) {
        MessageBoxW(hwnd, L"Game is not installed or not detected.", L"CMR5 Patcher", MB_ICONWARNING | MB_OK);
        return;
    }

    DWORD flags = BuildPatchFlagsFromUI(state);
    if (flags == 0u) {
        MessageBoxW(hwnd, L"Select at least one option.", L"CMR5 Patcher", MB_ICONINFORMATION | MB_OK);
        return;
    }

    DWORD writeAccessError = ERROR_SUCCESS;
    if (!CheckWriteAccessForPatch(state->installPath, &writeAccessError)) {
        wchar_t accessText[320]{};
        StringCchPrintfW(
            accessText,
            _countof(accessText),
            L"Backup/output files are not writable (Win32 error: %lu).\nPlease rerun this application as Administrator.",
            writeAccessError);
        MessageBoxW(hwnd, accessText, L"CMR5 Patcher", MB_ICONWARNING | MB_OK);
        return;
    }

    bool needsElevation = false;
    bool unexpectedSize = false;
    DWORD patchError = ERROR_SUCCESS;

    PatchStatus status = PatchOptionsFile(
        state->installPath,
        flags,
        false,
        &needsElevation,
        &patchError,
        &unexpectedSize);

    if (unexpectedSize) {
        int answer = MessageBoxW(
            hwnd,
            L"Unexpected OPTIONS file size. Continue anyway?",
            L"CMR5 Patcher",
            MB_ICONQUESTION | MB_YESNO);

        if (answer != IDYES) {
            return;
        }

        status = PatchOptionsFile(
            state->installPath,
            flags,
            true,
            &needsElevation,
            &patchError,
            nullptr);
    }

    if (status == PatchStatus::Ok) {
        if (ImpliesUnlockAll(flags)) {
            MessageBoxW(hwnd, L"Patch applied successfully.\nUnlock-all option applied (or implied by mini compatibility pattern).", L"CMR5 Patcher", MB_ICONINFORMATION | MB_OK);
        } else {
            MessageBoxW(hwnd, L"Patch applied successfully.", L"CMR5 Patcher", MB_ICONINFORMATION | MB_OK);
        }
        return;
    }

    if (status == PatchStatus::OptionsMissing) {
        MessageBoxW(
            hwnd,
            L"File SG\\OPTIONS not found. Launch the game once before patching.",
            L"CMR5 Patcher",
            MB_ICONWARNING | MB_OK);
        return;
    }

    if (needsElevation) {
        int answer = MessageBoxW(
            hwnd,
            L"Writing requires administrator privileges.\nDo you want to retry with elevated rights?",
            L"CMR5 Patcher",
            MB_ICONQUESTION | MB_YESNO);

        if (answer != IDYES) {
            return;
        }

        DWORD elevatedExitCode = 1;
        DWORD launchError = ERROR_SUCCESS;
        if (LaunchElevatedPatchAndWait(hwnd, state->installPath, flags, &elevatedExitCode, &launchError)) {
            if (ImpliesUnlockAll(flags)) {
                MessageBoxW(hwnd, L"Patch applied successfully (elevated).\nUnlock-all option applied (or implied by mini compatibility pattern).", L"CMR5 Patcher", MB_ICONINFORMATION | MB_OK);
            } else {
                MessageBoxW(hwnd, L"Patch applied successfully (elevated).", L"CMR5 Patcher", MB_ICONINFORMATION | MB_OK);
            }
            return;
        }

        if (launchError == ERROR_CANCELLED) {
            MessageBoxW(hwnd, L"Elevation was cancelled.", L"CMR5 Patcher", MB_ICONWARNING | MB_OK);
            return;
        }

        wchar_t errorText[256]{};
        StringCchPrintfW(errorText, _countof(errorText), L"Elevated patch failed (code: %lu).", elevatedExitCode);
        MessageBoxW(hwnd, errorText, L"CMR5 Patcher", MB_ICONERROR | MB_OK);
        return;
    }

    if (status == PatchStatus::SizeTooSmall) {
        MessageBoxW(hwnd, L"OPTIONS file is too small.", L"CMR5 Patcher", MB_ICONERROR | MB_OK);
        return;
    }

    wchar_t errorText[256]{};
    StringCchPrintfW(errorText, _countof(errorText), L"Patch failed (Win32 error: %lu).", patchError);
    MessageBoxW(hwnd, errorText, L"CMR5 Patcher", MB_ICONERROR | MB_OK);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* app = reinterpret_cast<AppState*>(createStruct->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            state = app;

            HMENU appMenu = BuildAppMenu();
            if (appMenu) {
                SetMenu(hwnd, appMenu);
            }

            CreateWindowExW(0, L"STATIC", L"Game path:", WS_VISIBLE | WS_CHILD,
                            16, 16, 140, 20, hwnd, nullptr, nullptr, nullptr);

            state->editPath = CreateWindowExW(
                WS_EX_CLIENTEDGE,
                L"EDIT",
                state->installPathDisplay,
                WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | ES_READONLY,
                16,
                38,
                500,
                24,
                hwnd,
                reinterpret_cast<HMENU>(kIdEditPath),
                nullptr,
                nullptr);

            state->cbAll = CreateWindowExW(0, L"BUTTON", L"Unlock all cars and circuits",
                                           WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                                           16, 80, 340, 24, hwnd, reinterpret_cast<HMENU>(kIdCheckboxAll), nullptr, nullptr);

            state->cbMirror = CreateWindowExW(0, L"BUTTON", L"Enable mirror mode",
                                              WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                                              16, 108, 280, 24, hwnd, reinterpret_cast<HMENU>(kIdCheckboxMirror), nullptr, nullptr);

            state->cbMini = CreateWindowExW(0, L"BUTTON", L"Enable mini cars",
                                            WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                                            16, 136, 300, 24, hwnd, reinterpret_cast<HMENU>(kIdCheckboxMini), nullptr, nullptr);

            state->cbReset = CreateWindowExW(0, L"BUTTON", L"Reset to default",
                                             WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                                             16, 164, 220, 24, hwnd, reinterpret_cast<HMENU>(kIdCheckboxReset), nullptr, nullptr);

            CreateWindowExW(0, L"BUTTON", L"Apply patch",
                            WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
                            16, 204, 170, 32, hwnd, reinterpret_cast<HMENU>(kIdButtonApply), nullptr, nullptr);

            CreateWindowExW(0, L"STATIC", L"Backup file: OPTIONS.bak",
                            WS_VISIBLE | WS_CHILD,
                            210, 212, 220, 20, hwnd, nullptr, nullptr, nullptr);

            return 0;
        }

        case WM_COMMAND: {
            const int controlId = LOWORD(wParam);
            const int notifyCode = HIWORD(wParam);

            if (controlId == kIdMenuFileExit) {
                DestroyWindow(hwnd);
                return 0;
            }

            if (controlId == kIdMenuAboutVersion) {
                MessageBoxW(hwnd, L"By tlt\nversion 1.0.0\ntolotra.alwaysdata.net", L"Version", MB_ICONINFORMATION | MB_OK);
                return 0;
            }

            if (controlId == kIdCheckboxReset && notifyCode == BN_CLICKED) {
                const bool resetChecked = state && IsChecked(state->cbReset);
                HandleResetMutualExclusion(hwnd, state, resetChecked);
                return 0;
            }

            if (controlId == kIdButtonApply && notifyCode == BN_CLICKED) {
                RunPatch(hwnd, state);
                return 0;
            }

            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

DWORD RunElevatedPatchFromArgs(int argc, wchar_t** argv) {
    if (argc < 4) {
        return 2;
    }

    const wchar_t* installPath = argv[2];
    DWORD flags = wcstoul(argv[3], nullptr, 10);
    if (!installPath || installPath[0] == L'\0' || flags == 0u) {
        return 3;
    }

    bool needsElevation = false;
    DWORD patchError = ERROR_SUCCESS;

    PatchStatus status = PatchOptionsFile(
        installPath,
        flags,
        true,
        &needsElevation,
        &patchError,
        nullptr);

    if (status == PatchStatus::Ok) {
        return 0;
    }

    if (needsElevation) {
        return 10;
    }

    if (status == PatchStatus::OptionsMissing) {
        return 20;
    }

    if (status == PatchStatus::SizeTooSmall) {
        return 30;
    }

    return 100 + (patchError % 100);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argv && argc >= 2 && lstrcmpiW(argv[1], L"--elevated-patch") == 0) {
        DWORD code = RunElevatedPatchFromArgs(argc, argv);
        LocalFree(argv);
        return static_cast<int>(code);
    }

    if (argv) {
        LocalFree(argv);
    }

    AppState state{};

    if (ReadInstallPathFromRegistry(state.installPath, static_cast<DWORD>(_countof(state.installPath))) &&
        DirectoryExists(state.installPath)) {
        state.installDetected = true;
        lstrcpynW(state.installPathDisplay, state.installPath, static_cast<int>(_countof(state.installPathDisplay)));
    } else {
        state.installDetected = false;
        state.installPath[0] = L'\0';
        lstrcpynW(state.installPathDisplay, L"Game is not installed or not detected.", static_cast<int>(_countof(state.installPathDisplay)));
    }

    const wchar_t kClassName[] = L"CMR5PatcherWin32Class";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Cannot register window class.", L"CMR5 Patcher", MB_ICONERROR | MB_OK);
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        kClassName,
        L"CMR5 Patcher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        550,
        290,
        nullptr,
        nullptr,
        hInstance,
        &state);

    if (!hwnd) {
        MessageBoxW(nullptr, L"Cannot create main window.", L"CMR5 Patcher", MB_ICONERROR | MB_OK);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}



