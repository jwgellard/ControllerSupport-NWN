// Loader: this DLL is installed as bin/win32/version.dll.
//
// nwmain.exe imports VERSION.dll, which isn't a KnownDLL, so Windows loads this
// copy from the game folder first. All version.dll exports are forwarded to the
// system copy. When the host is nwmain.exe we set SDL_DYNAMIC_API to our own
// path; the game's statically linked SDL reads that variable on its first call
// and hands its jump table to our SDL_DYNAPI_entry, where we fill it with the
// game's own SDL implementation and then hook SDL_PollEvent.
#include <windows.h>

#include <cstdint>
#include <string>

#include "controller.h"
#include "log.h"
#include "sdl_api.h"

static HMODULE g_self;

static HMODULE SystemVersionDll()
{
    static HMODULE module = [] {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 16)
            return static_cast<HMODULE>(nullptr);
        wcscat_s(path, L"\\version.dll");
        return LoadLibraryW(path);
    }();
    return module;
}

template <typename Fn>
static Fn SystemProc(const char* name)
{
    HMODULE module = SystemVersionDll();
    return module ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
}

#define FORWARD(ret, name, params, args)                               \
    extern "C" ret WINAPI Proxy_##name params                          \
    {                                                                  \
        static auto fn = SystemProc<ret(WINAPI*) params>(#name);       \
        if (!fn) {                                                     \
            SetLastError(ERROR_PROC_NOT_FOUND);                        \
            return 0;                                                  \
        }                                                              \
        return fn args;                                                \
    }

FORWARD(BOOL, GetFileVersionInfoA, (LPCSTR f, DWORD h, DWORD n, LPVOID d), (f, h, n, d))
FORWARD(BOOL, GetFileVersionInfoW, (LPCWSTR f, DWORD h, DWORD n, LPVOID d), (f, h, n, d))
FORWARD(BOOL, GetFileVersionInfoExA, (DWORD fl, LPCSTR f, DWORD h, DWORD n, LPVOID d), (fl, f, h, n, d))
FORWARD(BOOL, GetFileVersionInfoExW, (DWORD fl, LPCWSTR f, DWORD h, DWORD n, LPVOID d), (fl, f, h, n, d))
FORWARD(DWORD, GetFileVersionInfoSizeA, (LPCSTR f, LPDWORD h), (f, h))
FORWARD(DWORD, GetFileVersionInfoSizeW, (LPCWSTR f, LPDWORD h), (f, h))
FORWARD(DWORD, GetFileVersionInfoSizeExA, (DWORD fl, LPCSTR f, LPDWORD h), (fl, f, h))
FORWARD(DWORD, GetFileVersionInfoSizeExW, (DWORD fl, LPCWSTR f, LPDWORD h), (fl, f, h))
FORWARD(DWORD, VerFindFileA, (DWORD fl, LPCSTR a, LPCSTR b, LPCSTR c, LPSTR d, PUINT e, LPSTR f, PUINT g),
        (fl, a, b, c, d, e, f, g))
FORWARD(DWORD, VerFindFileW, (DWORD fl, LPCWSTR a, LPCWSTR b, LPCWSTR c, LPWSTR d, PUINT e, LPWSTR f, PUINT g),
        (fl, a, b, c, d, e, f, g))
FORWARD(DWORD, VerInstallFileA, (DWORD fl, LPCSTR a, LPCSTR b, LPCSTR c, LPCSTR d, LPCSTR e, LPSTR f, PUINT g),
        (fl, a, b, c, d, e, f, g))
FORWARD(DWORD, VerInstallFileW,
        (DWORD fl, LPCWSTR a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPCWSTR e, LPWSTR f, PUINT g),
        (fl, a, b, c, d, e, f, g))
FORWARD(DWORD, VerLanguageNameA, (DWORD l, LPSTR s, DWORD n), (l, s, n))
FORWARD(DWORD, VerLanguageNameW, (DWORD l, LPWSTR s, DWORD n), (l, s, n))
FORWARD(BOOL, VerQueryValueA, (LPCVOID b, LPCSTR s, LPVOID* p, PUINT n), (b, s, p, n))
FORWARD(BOOL, VerQueryValueW, (LPCVOID b, LPCWSTR s, LPVOID* p, PUINT n), (b, s, p, n))
// Undocumented; every x64 argument is register-sized, so passing six through is ABI-safe.
FORWARD(uintptr_t, GetFileVersionInfoByHandle,
        (uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f), (a, b, c, d, e, f))

static std::string ModuleDirectory(HMODULE module)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return std::string();
    std::string s(path, n);
    return s.substr(0, s.find_last_of("\\/") + 1);
}

static bool IsGameProcess()
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return false;
    const char* base = strrchr(path, '\\');
    return _stricmp(base ? base + 1 : path, "nwmain.exe") == 0;
}

using DynApiEntryFn = Sint32(SDLCALL*)(Uint32, void*, Uint32);

extern "C" Sint32 SDLCALL SDL_DYNAPI_entry(Uint32 apiver, void* table, Uint32 tableSize)
{
    // Don't leak the override into child processes.
    SetEnvironmentVariableA("SDL_DYNAMIC_API", nullptr);

    HMODULE exe = GetModuleHandleW(nullptr);
    auto gameEntry = reinterpret_cast<DynApiEntryFn>(GetProcAddress(exe, "SDL_DYNAPI_entry"));
    if (!gameEntry || gameEntry == &SDL_DYNAPI_entry)
        return -1; // SDL falls back to its built-in table

    // Populate the game's table with the game's own SDL, then patch our hook in.
    Sint32 rc = gameEntry(apiver, table, tableSize);
    if (rc < 0)
        return rc;

    std::string dir = ModuleDirectory(g_self);
    LogOpen(dir + "nwn_controller.log");
    Log("NWN controller support loaded (SDL dynamic API version %u)", apiver);
    if (!InstallController(exe, table, tableSize, dir))
        Log("controller support NOT active; the game will run normally");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        if (IsGameProcess() && GetProcAddress(GetModuleHandleW(nullptr), "SDL_DYNAPI_entry")) {
            char self[MAX_PATH];
            DWORD n = GetModuleFileNameA(instance, self, MAX_PATH);
            if (n > 0 && n < MAX_PATH)
                SetEnvironmentVariableA("SDL_DYNAMIC_API", self);
        }
    }
    return TRUE;
}
