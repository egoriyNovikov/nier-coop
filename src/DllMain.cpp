#include <chrono>
#include <mutex>
#include <thread>
#include <string_view>
#include <algorithm>
#include <filesystem>
#include <vector>
#include <windows.h>
#include <winternl.h>
#include <shlobj.h>

#include <utility/Module.hpp>
#include <utility/Thread.hpp>
#include <utility/FunctionHook.hpp>

#include "ExceptionHandler.hpp"
#include "TestMode.hpp"
#include "AutomataMP.hpp"

HMODULE g_dinput = 0;
std::mutex g_load_mutex{};

void failed() {
    MessageBox(0, "AutomataMP: Unable to load the original dinput8.dll. Please report this to the developer.", "AutomataMP", 0);
    ExitProcess(0);
}

bool load_dinput8() {
    std::scoped_lock _{g_load_mutex};

    if (g_dinput) {
        return true;
    }

    wchar_t buffer[MAX_PATH]{0};
    if (GetSystemDirectoryW(buffer, MAX_PATH) != 0) {
        // Load the original dinput8.dll
        if ((g_dinput = LoadLibraryW((std::wstring{buffer} + L"\\dinput8.dll").c_str())) == NULL) {
            failed();
            return false;
        }

        return true;
    }

    failed();
    return false;
}

extern "C" {
// DirectInput8Create wrapper for dinput8.dll
__declspec(dllexport) HRESULT WINAPI
    direct_input8_create(HINSTANCE hinst, DWORD dw_version, const IID& riidltf, LPVOID* ppv_out, LPUNKNOWN punk_outer) {
// This needs to be done because when we include dinput.h in DInputHook,
// It is a redefinition, so we assign an export by not using the original name
#pragma comment(linker, "/EXPORT:DirectInput8Create=direct_input8_create")

    load_dinput8();
    return ((decltype(direct_input8_create)*)GetProcAddress(g_dinput, "DirectInput8Create"))(hinst, dw_version, riidltf, ppv_out, punk_outer);
}
}

std::unique_ptr<FunctionHook> g_create_event_a_hook{};

// Allows us to launch the game more than once to test multiplayer.
HANDLE WINAPI create_event_a_hook(
  LPSECURITY_ATTRIBUTES lpEventAttributes,
  BOOL                  bManualReset,
  BOOL                  bInitialState,
  LPCSTR                lpName
)
{
    const auto result = g_create_event_a_hook->get_original<decltype(CreateEventA)>()(lpEventAttributes, bManualReset, bInitialState, lpName);

    if (lpName != nullptr) {
        spdlog::info("{}", lpName);

        if (std::string_view{lpName} == "NieR:AutomataEvent") {
            if (GetLastError() == ERROR_ALREADY_EXISTS) {
                spdlog::info("AutomataMP: AutomataEvent already exists...");
                std::this_thread::sleep_for(std::chrono::milliseconds(2000)); // so the game remembers the graphics settings, because it won't be able to access the config in use by the other instance
                return g_create_event_a_hook->get_original<decltype(CreateEventA)>()(lpEventAttributes, bManualReset, bInitialState, "AutomataMPEvent");
            }
        }
    }

    return result;
}

// SetCursorPos hook to prevent the game from taking control of the cursor.
std::unique_ptr<FunctionHook> g_set_cursor_pos_hook{};

BOOL WINAPI set_cursor_pos_hook(int x, int y) {
    return TRUE;
}

// Test mode: a test copy of the game (tools/qa/make_test_install.ps1) has coop_test.ini next to the exe.
// The game finds its saves and Graphic.ini through SHGetSpecialFolderPathA(CSIDL_PERSONAL), so the test copy
// gets its own "Documents" and never touches the owner's saves or Steam Cloud.
// Done in DllMain through the exe's import table: the game asks for the folder before startup_thread runs.
char g_test_documents[MAX_PATH]{};
char g_test_appdata[MAX_PATH]{}; // config.ini of the game: %APPDATA%\PlatinumGames\NierAutomata
decltype(SHGetSpecialFolderPathA)* g_original_get_special_folder_path{};

BOOL WINAPI get_special_folder_path_hook(HWND hwnd, LPSTR path, int csidl, BOOL create) {
    if ((csidl & 0xFF) == CSIDL_PERSONAL) {
        strcpy_s(path, MAX_PATH, g_test_documents);
        return TRUE;
    }

    if ((csidl & 0xFF) == CSIDL_APPDATA) {
        strcpy_s(path, MAX_PATH, g_test_appdata);
        return TRUE;
    }

    return g_original_get_special_folder_path(hwnd, path, csidl, create);
}

// Write guard of the test mode: the owner's Documents and game install (paths from coop_test.ini) can't be
// opened for writing. NtCreateFile is below every Win32 file API, so this also covers code that doesn't go
// through SHGetSpecialFolderPathA (the first test run still changed the owner's SystemData.dat).
// Every write outside the test install goes to coop_test_guard.txt, blocked ones with "BLOCKED".
using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
    ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);

std::unique_ptr<FunctionHook> g_nt_create_file_hook{};
std::wstring g_test_root{};
std::vector<std::wstring> g_protected_paths{};
HANDLE g_guard_log{INVALID_HANDLE_VALUE};
std::mutex g_guard_mutex{};
thread_local bool g_in_guard{false};

std::wstring lowercase(std::wstring s) {
    for (auto& c : s) {
        c = towlower(c);
    }

    return s;
}

bool starts_with_path(const std::wstring& path, const std::wstring& prefix) {
    return !prefix.empty() && path.size() >= prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
        (path.size() == prefix.size() || path[prefix.size()] == L'\\' || prefix.back() == L'\\');
}

void guard_log(const std::wstring& line) {
    std::scoped_lock _{g_guard_mutex};
    const auto utf8_size = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(utf8_size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), utf8.data(), utf8_size, nullptr, nullptr);
    utf8 += "\r\n";

    DWORD written{};
    WriteFile(g_guard_log, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
}

NTSTATUS NTAPI nt_create_file_hook(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes, PIO_STATUS_BLOCK io,
    PLARGE_INTEGER allocation_size, ULONG file_attributes, ULONG share, ULONG disposition, ULONG options, PVOID ea, ULONG ea_length)
{
    const auto original = g_nt_create_file_hook->get_original<std::remove_pointer_t<NtCreateFileFn>>();
    constexpr ACCESS_MASK write_access = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA |
        DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;
    const auto writes = (access & write_access) != 0 || disposition != FILE_OPEN || (options & FILE_DELETE_ON_CLOSE) != 0;

    if (!writes || g_in_guard || attributes == nullptr || attributes->ObjectName == nullptr || attributes->ObjectName->Buffer == nullptr) {
        return original(handle, access, attributes, io, allocation_size, file_attributes, share, disposition, options, ea, ea_length);
    }

    g_in_guard = true;

    auto path = lowercase(std::wstring{attributes->ObjectName->Buffer, attributes->ObjectName->Length / sizeof(wchar_t)});

    if (path.starts_with(L"\\??\\")) {
        path = path.substr(4);
    }

    // Relative opens (RootDirectory set) and devices/pipes are not files on a drive: keep them, but log.
    const auto is_drive_path = attributes->RootDirectory == nullptr && path.size() > 2 && path[1] == L':';
    const auto blocked = is_drive_path && std::any_of(g_protected_paths.begin(), g_protected_paths.end(),
        [&](const auto& p) { return starts_with_path(path, p); });

    if (blocked || (is_drive_path && !starts_with_path(path, g_test_root))) {
        guard_log((blocked ? L"BLOCKED write: " : L"write outside the test install: ") + path);
    }

    g_in_guard = false;

    if (blocked) {
        return (NTSTATUS)0xC0000022L; // STATUS_ACCESS_DENIED
    }

    return original(handle, access, attributes, io, allocation_size, file_attributes, share, disposition, options, ea, ea_length);
}

namespace testmode {
bool enabled() {
    return g_test_documents[0] != '\0';
}

std::filesystem::path root() {
    char exe_path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    return std::filesystem::path{exe_path}.parent_path();
}
}

void setup_test_mode() {
    char exe_path[MAX_PATH]{};
    const auto length = GetModuleFileNameA(nullptr, exe_path, MAX_PATH);

    if (length == 0 || length >= MAX_PATH) {
        return;
    }

    const auto ini = (std::filesystem::path{exe_path}.parent_path() / "coop_test.ini").string();

    if (GetPrivateProfileStringA("test", "documents", "", g_test_documents, MAX_PATH, ini.c_str()) == 0) {
        g_test_documents[0] = '\0';
        return;
    }

    const auto ini_w = std::filesystem::path{ini}.wstring();
    g_test_root = lowercase(std::filesystem::path{exe_path}.parent_path().wstring());

    // The game reads config.ini from getenv("APPDATA"); the exe's CRT copies the environment after DllMain.
    if (GetPrivateProfileStringA("test", "appdata", "", g_test_appdata, MAX_PATH, ini.c_str()) != 0) {
        SetEnvironmentVariableA("APPDATA", g_test_appdata);
    }

    for (const auto key : {L"owner_documents", L"owner_install", L"owner_appdata"}) {
        wchar_t value[MAX_PATH]{};

        if (GetPrivateProfileStringW(L"test", key, L"", value, MAX_PATH, ini_w.c_str()) != 0) {
            g_protected_paths.push_back(lowercase(value));
        }
    }

    g_guard_log = CreateFileW((std::filesystem::path{exe_path}.parent_path() / "coop_test_guard.txt").c_str(),
        GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto nt_create_file = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile");

    if (g_test_appdata[0] == '\0' || g_protected_paths.size() != 3 || g_guard_log == INVALID_HANDLE_VALUE || nt_create_file == nullptr) {
        TerminateProcess(GetCurrentProcess(), 0xC00F);
    }

    g_nt_create_file_hook = std::make_unique<FunctionHook>(nt_create_file, &nt_create_file_hook);

    if (!g_nt_create_file_hook->create()) {
        TerminateProcess(GetCurrentProcess(), 0xC00F);
    }

    guard_log(L"guard on, protected: " + g_protected_paths[0] + L" | " + g_protected_paths[1] + L" | " + g_protected_paths[2]);

    const auto base = (uintptr_t)GetModuleHandleW(nullptr);
    const auto dos = (IMAGE_DOS_HEADER*)base;
    const auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const auto target = (uintptr_t)GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHGetSpecialFolderPathA");

    for (auto desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); target != 0 && desc->Name != 0; ++desc) {
        for (auto thunk = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk); thunk->u1.Function != 0; ++thunk) {
            if (thunk->u1.Function != target) {
                continue;
            }

            DWORD old{};
            VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), PAGE_READWRITE, &old);
            g_original_get_special_folder_path = (decltype(SHGetSpecialFolderPathA)*)target;
            thunk->u1.Function = (uintptr_t)&get_special_folder_path_hook;
            VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), old, &old);
            return;
        }
    }

    // A test copy must never fall back to the owner's Documents.
    TerminateProcess(GetCurrentProcess(), 0xC00F);
}

void startup_thread(HMODULE automatamp_module) {
    // We will set it once here, then do it continuously
    // every now and then because it gets replaced
    automatamp::setup_exception_handler();

#ifndef NDEBUG
    AllocConsole();
    freopen("CONIN$", "r", stdin);
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
#endif

    if (load_dinput8()) {
        const auto cva = (decltype(create_event_a_hook)*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateEventA");
        g_create_event_a_hook = std::make_unique<FunctionHook>(
            cva,
            &create_event_a_hook
        );
        g_create_event_a_hook->create();

        const auto scp = (decltype(set_cursor_pos_hook)*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetCursorPos");

        g_set_cursor_pos_hook = std::make_unique<FunctionHook>(
            scp,
            &set_cursor_pos_hook
        );
        g_set_cursor_pos_hook->create();

        g_framework = std::make_unique<AutomataMP>(automatamp_module);

        if (g_test_documents[0] != '\0') {
            spdlog::info("[Test] Test mode: documents folder is {} (hooked: {})", g_test_documents,
                g_original_get_special_folder_path != nullptr);
        }
    }
}

BOOL APIENTRY DllMain(HANDLE handle, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        setup_test_mode();
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)startup_thread, handle, 0, nullptr);
    }

    return TRUE;
}
