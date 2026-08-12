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
//
// V4.9 fixes three things here:
//
//  * The log was opened "w", truncating it on every launch. A crash followed
//    by a relaunch therefore destroyed the evidence for the crash -- and a
//    relaunch is exactly what a tester does after one. It appends now, with a
//    run separator, matching the Linux proxy.
//  * No timestamps. Correlating a proxy log against BZLogger meant guessing.
//    Same "[YYYY-MM-DD HH:MM:SS.mmm][pid=N] " prefix as the Linux proxy now.
//  * No lock, and three separate stdio calls per line from several threads.
//    Two logs in the repo contain torn lines because of the equivalent bug on
//    the Linux side. The line is now formatted whole and written under a lock
//    in one call.
// ---------------------------------------------------------
// Injected by the Makefile.  See the matching note in the Linux proxy.
#ifndef BZ_BUILD_ID
#define BZ_BUILD_ID "unknown (built without -DBZ_BUILD_ID)"
#endif

static FILE*             g_log = nullptr;
static CRITICAL_SECTION  g_log_cs;
static volatile LONG     g_log_ready = 0;

static void OpenLog()
{
    InitializeCriticalSection(&g_log_cs);

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

    // Append, never truncate.
    g_log = fopen(logPath, "a");
    if (g_log)
    {
        fprintf(g_log, "\n=== winmm_proxy.dll loaded (pid=%lu) ===\n",
                (unsigned long)GetCurrentProcessId());
        fprintf(g_log, "  Game dir : %s\n", modPath);
        fprintf(g_log, "  Log file : %s\n", logPath);
        fprintf(g_log, "  proxy build: %s\n", BZ_BUILD_ID);
        fflush(g_log);
    }
    InterlockedExchange(&g_log_ready, 1);
}

// Public log function used by netcode_hooks.cpp.
void ProxyLog(const char* fmt, ...)
{
    if (InterlockedCompareExchange(&g_log_ready, 0, 0) == 0) return;

    // Format the whole line -- prefix, body, newline -- before taking the
    // lock, so the lock covers exactly one write.
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char line[1024];
    int n = _snprintf(line, sizeof(line),
                      "[%04u-%02u-%02u %02u:%02u:%02u.%03u][pid=%lu] ",
                      (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                      (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                      (unsigned)st.wMilliseconds,
                      (unsigned long)GetCurrentProcessId());
    if (n < 0 || (size_t)n >= sizeof(line) - 2) return;

    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf(line + n, sizeof(line) - (size_t)n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) return;

    size_t len = strlen(line);
    line[len++] = '\n';
    line[len]   = '\0';

    EnterCriticalSection(&g_log_cs);
    if (g_log)
    {
        fwrite(line, 1, len, g_log);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_log_cs);
}

// ---------------------------------------------------------
// Hook installation thread – runs after DLL_PROCESS_ATTACH.
// We defer into a thread so the process has finished its
// own DLL loading before we walk IAT entries.
// ---------------------------------------------------------
// V4.9: retry, the way the Linux proxy has always done.
//
// This used to sleep 10 ms and patch the EXE's IAT exactly once. A module that
// resolves its winsock imports slightly later -- or resolves them dynamically
// through GetProcAddress -- simply bypassed the proxy, silently, with nothing
// in the log to say so. InstallNetcodeHooks is idempotent (PatchIAT skips an
// entry that already points at our hook), so re-running it for a couple of
// seconds costs nothing and closes the window.
static DWORD WINAPI HookThread(LPVOID)
{
    // Give the loader a moment to finish wiring all imports.
    Sleep(10);
    InstallNetcodeHooks();
    // Retry only while something is still unpatched, so a healthy launch
    // installs once and logs once. Thread creation inside is guarded, and
    // PatchIAT skips an entry that already points at our hook, so repeating
    // it is safe.
    for (int i = 0; i < 160 && !NetcodeHooksComplete(); ++i) {
        Sleep(10);
        InstallNetcodeHooks();
    }
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
        ShutdownNetcodeHooks();
        // Worker threads are signalled but deliberately not joined -- joining
        // under the loader lock deadlocks. So one of them may still be inside
        // ProxyLog right now, or about to enter it. Closing the FILE* and
        // deleting the lock out from under it is a use-after-free.
        //
        // Instead: take the lock, write the trailer, drop the pointer, and
        // leave both the FILE* and the critical section alive. A thread that
        // was blocked on the lock wakes, sees a null g_log, and does nothing.
        // Every line is fflushed as it is written, so nothing is buffered and
        // nothing is lost by not calling fclose. The process is exiting; the
        // OS reclaims the handle. One leaked FILE* beats a torn-down one.
        EnterCriticalSection(&g_log_cs);
        if (g_log)
        {
            fprintf(g_log, "=== winmm_proxy.dll unloaded ===\n");
            fflush(g_log);
            g_log = nullptr;
        }
        LeaveCriticalSection(&g_log_cs);
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
