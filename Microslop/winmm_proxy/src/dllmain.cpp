// dllmain.cpp
// Battlezone 98 Redux - Windows netcode patch
//
// winmm.dll proxy entry point.
// Loads the real System32\winmm.dll, forwards all exports to it, then
// installs the WSASocketW IAT hook so UDP sockets get enlarged buffers.
//
// Build: i686-w64-mingw32-g++ (see Makefile)

#include <windows.h>
#include <cstdio>
#include <cstring>
#include "netcode_hooks.h"

// Shared handle to the real winmm.dll - used by winmm_proxy.cpp stubs.
HMODULE g_hRealWinmm = nullptr;

// Resolve a function from the real winmm.dll by name.
// Called lazily by each stub in winmm_proxy.cpp on first use.
extern "C" FARPROC ResolveRealWinmm(const char* name)
{
    if (!g_hRealWinmm) return nullptr;
    return GetProcAddress(g_hRealWinmm, name);
}

// ---------------------------------------------------------
// Logging
// Log file lands in the same directory as the game .exe.
// ---------------------------------------------------------
static FILE* g_log = nullptr;

static void OpenLog()
{
    // Derive log path from the module's own location (game dir).
    char modPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, modPath, MAX_PATH);

    // Replace the exe name with the log filename.
    char* sep = strrchr(modPath, '\\');
    if (sep)
        *(sep + 1) = '\0';
    else
        modPath[0] = '\0';

    char logPath[MAX_PATH] = {};
    _snprintf(logPath, MAX_PATH, "%swinmm_proxy.log", modPath);

    g_log = fopen(logPath, "w");
    if (g_log)
    {
        fprintf(g_log, "=== winmm_proxy.dll loaded ===\n");
        fprintf(g_log, "  Game dir : %s\n", modPath);
        fprintf(g_log, "  Log file : %s\n", logPath);
        fflush(g_log);
    }
}

// Public log function used by netcode_hooks.cpp.
void ProxyLog(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------
// Hook installation thread – runs after DLL_PROCESS_ATTACH.
// We defer into a thread so the process has finished its
// own DLL loading before we walk IAT entries.
// ---------------------------------------------------------
static DWORD WINAPI HookThread(LPVOID)
{
    // Give the loader a moment to finish wiring all imports.
    Sleep(10);
    InstallNetcodeHooks();
    return 0;
}

// ---------------------------------------------------------
// DllMain
// ---------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);
        OpenLog();

        // Load the real winmm.dll from System32.
        char sysPath[MAX_PATH] = {};
        UINT len = GetSystemDirectoryA(sysPath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH - 16)
        {
            ProxyLog("DllMain: GetSystemDirectoryA failed (len=%u err=%lu)", len, GetLastError());
            return FALSE;
        }
        strncat(sysPath, "\\winmm.dll", sizeof(sysPath) - strlen(sysPath) - 1);

        g_hRealWinmm = LoadLibraryA(sysPath);
        if (!g_hRealWinmm)
        {
            ProxyLog("DllMain: failed to load real winmm from %s (err=%lu)", sysPath, GetLastError());
            return FALSE;
        }
        ProxyLog("DllMain: real winmm loaded from %s (handle=0x%p)", sysPath, (void*)g_hRealWinmm);

        // Spawn the hook thread.
        HANDLE ht = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
        if (ht) CloseHandle(ht);
        break;
    }

    case DLL_PROCESS_DETACH:
        if (g_log)
        {
            fprintf(g_log, "=== winmm_proxy.dll unloaded ===\n");
            fclose(g_log);
            g_log = nullptr;
        }
        if (g_hRealWinmm)
        {
            FreeLibrary(g_hRealWinmm);
            g_hRealWinmm = nullptr;
        }
        break;

    default:
        break;
    }
    return TRUE;
}
