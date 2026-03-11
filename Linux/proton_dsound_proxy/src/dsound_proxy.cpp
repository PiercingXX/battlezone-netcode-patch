#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>

namespace {

constexpr wchar_t kLogFileName[] = L"dsound_proxy.log";
wchar_t g_log_path[MAX_PATH] = L"dsound_proxy.log";
bool g_log_path_ready = false;

constexpr int kSendBuf = 524288;
constexpr int kRecvBuf = 2097152;

HMODULE g_real_dsound = nullptr;
FARPROC g_real_ordinal_1 = nullptr;
using SetSockOptFn = int(WSAAPI *)(SOCKET, int, int, const char *, int);
SetSockOptFn g_real_setsockopt = nullptr;
using WSASetSockOptFn = int(WSAAPI *)(SOCKET, int, int, const char *, int);
WSASetSockOptFn g_real_wsasetsocketoption = nullptr;
using GetSockOptFn = int(WSAAPI *)(SOCKET, int, int, char *, int *);
GetSockOptFn g_real_getsockopt = nullptr;
using WSAGetSockOptFn = int(WSAAPI *)(SOCKET, int, int, char *, int *);
WSAGetSockOptFn g_real_wsagetsocketoption = nullptr;
using SocketFn = SOCKET(WSAAPI *)(int, int, int);
SocketFn g_real_socket = nullptr;
using WSASocketWFn = SOCKET(WSAAPI *)(int, int, int, LPWSAPROTOCOL_INFOW, GROUP, DWORD);
WSASocketWFn g_real_wsasocketw = nullptr;
using CloseSocketFn = int(WSAAPI *)(SOCKET);
CloseSocketFn g_real_closesocket = nullptr;
using GetProcAddressFn = FARPROC(WINAPI *)(HMODULE, LPCSTR);
GetProcAddressFn g_real_getprocaddress = nullptr;
bool g_installed_getproc_hook = false;
bool g_logged_real_setsockopt = false;
bool g_logged_real_wsaset = false;
bool g_logged_real_getsockopt = false;
bool g_logged_real_wsaget = false;
bool g_logged_real_socket = false;
bool g_logged_real_wsasocketw = false;
bool g_logged_real_closesocket = false;

constexpr int kSocketTrackCap = 256;
struct SocketTrack {
    SOCKET s;
    int id;
};
SocketTrack g_socket_tracks[kSocketTrackCap] = {};
int g_next_socket_id = 1;
CRITICAL_SECTION g_track_lock = {};
bool g_track_lock_ready = false;

void log_line(const char *fmt, ...);

void init_log_path() {
    if (g_log_path_ready) {
        return;
    }

    wchar_t exe_path[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        g_log_path_ready = true;
        return;
    }

    wchar_t *sep = wcsrchr(exe_path, L'\\');
    if (sep == nullptr) {
        sep = wcsrchr(exe_path, L'/');
    }

    if (sep != nullptr) {
        *(sep + 1) = L'\0';
    } else {
        exe_path[0] = L'\0';
    }

    g_log_path[0] = L'\0';
    if (lstrlenW(exe_path) + lstrlenW(kLogFileName) + 1 < MAX_PATH) {
        lstrcpyW(g_log_path, exe_path);
    }
    lstrcatW(g_log_path, kLogFileName);
    g_log_path_ready = true;
}

int get_socket_id(SOCKET s, bool create_if_missing) {
    if (!g_track_lock_ready) {
        return -1;
    }

    EnterCriticalSection(&g_track_lock);

    for (int i = 0; i < kSocketTrackCap; ++i) {
        if (g_socket_tracks[i].s == s) {
            int id = g_socket_tracks[i].id;
            LeaveCriticalSection(&g_track_lock);
            return id;
        }
    }

    if (!create_if_missing) {
        LeaveCriticalSection(&g_track_lock);
        return -1;
    }

    for (int i = 0; i < kSocketTrackCap; ++i) {
        if (g_socket_tracks[i].s == INVALID_SOCKET) {
            g_socket_tracks[i].s = s;
            g_socket_tracks[i].id = g_next_socket_id++;
            int id = g_socket_tracks[i].id;
            LeaveCriticalSection(&g_track_lock);
            return id;
        }
    }

    LeaveCriticalSection(&g_track_lock);
    return -1;
}

void forget_socket_id(SOCKET s) {
    if (!g_track_lock_ready) {
        return;
    }

    EnterCriticalSection(&g_track_lock);
    for (int i = 0; i < kSocketTrackCap; ++i) {
        if (g_socket_tracks[i].s == s) {
            g_socket_tracks[i].s = INVALID_SOCKET;
            g_socket_tracks[i].id = 0;
            break;
        }
    }
    LeaveCriticalSection(&g_track_lock);
}

void log_effective_bufs(const char *source, SOCKET s) {
    if (g_real_getsockopt == nullptr) {
        return;
    }

    int snd = -1;
    int snd_len = static_cast<int>(sizeof(snd));
    int snd_rc = g_real_getsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char *>(&snd), &snd_len);
    int snd_wsa = static_cast<int>(WSAGetLastError());

    int rcv = -1;
    int rcv_len = static_cast<int>(sizeof(rcv));
    int rcv_rc = g_real_getsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char *>(&rcv), &rcv_len);
    int rcv_wsa = static_cast<int>(WSAGetLastError());

    int sid = get_socket_id(s, true);
    log_line("%s: sid=%d sock=0x%08lx effective readback SO_SNDBUF=%d rc=%d wsa=%d | SO_RCVBUF=%d rc=%d wsa=%d", source, sid, static_cast<unsigned long>(s), snd, snd_rc, snd_wsa, rcv, rcv_rc, rcv_wsa);
}

bool is_target_main_module() {
    wchar_t path[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }

    const wchar_t *base = wcsrchr(path, L'\\');
    if (base == nullptr) {
        base = wcsrchr(path, L'/');
    }
    base = (base == nullptr) ? path : (base + 1);

    return _wcsicmp(base, L"battlezone98redux.exe") == 0;
}

bool module_is_ws2_32(HMODULE mod) {
    if (mod == nullptr) {
        return false;
    }
    wchar_t path[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(mod, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }
    const wchar_t *base = wcsrchr(path, L'\\');
    if (base == nullptr) {
        base = wcsrchr(path, L'/');
    }
    base = (base == nullptr) ? path : (base + 1);
    return _wcsicmp(base, L"ws2_32.dll") == 0;
}

void log_line(const char *fmt, ...) {
    init_log_path();

    HANDLE h = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    char buf[768] = {0};
    DWORD pid = GetCurrentProcessId();
    int n = std::snprintf(buf, sizeof(buf), "[%04u-%02u-%02u %02u:%02u:%02u.%03u][pid=%lu] ",
                          static_cast<unsigned>(st.wYear),
                          static_cast<unsigned>(st.wMonth),
                          static_cast<unsigned>(st.wDay),
                          static_cast<unsigned>(st.wHour),
                          static_cast<unsigned>(st.wMinute),
                          static_cast<unsigned>(st.wSecond),
                          static_cast<unsigned>(st.wMilliseconds),
                          static_cast<unsigned long>(pid));
    if (n < 0) {
        CloseHandle(h);
        return;
    }

    va_list args;
    va_start(args, fmt);
    int m = std::vsnprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), fmt, args);
    va_end(args);
    if (m < 0) {
        CloseHandle(h);
        return;
    }

    const char *newline = "\r\n";
    DWORD written = 0;
    WriteFile(h, buf, static_cast<DWORD>(std::strlen(buf)), &written, nullptr);
    WriteFile(h, newline, 2, &written, nullptr);
    CloseHandle(h);
}

int WSAAPI hooked_setsockopt(SOCKET s, int level, int optname, const char *optval, int optlen) {
    if (g_real_setsockopt == nullptr) {
        return SOCKET_ERROR;
    }

    if (level == SOL_SOCKET && optname == SO_SNDBUF) {
        int forced = kSendBuf;
        int sid = get_socket_id(s, true);
        log_line("hooked_setsockopt: sid=%d sock=0x%08lx forcing SO_SNDBUF=%d", sid, static_cast<unsigned long>(s), forced);
        int rc = g_real_setsockopt(s, level, optname, reinterpret_cast<const char *>(&forced), static_cast<int>(sizeof(forced)));
        log_line("hooked_setsockopt: sid=%d sock=0x%08lx SO_SNDBUF rc=%d wsa=%d", sid, static_cast<unsigned long>(s), rc, static_cast<int>(WSAGetLastError()));
        log_effective_bufs("hooked_setsockopt", s);
        return rc;
    }

    if (level == SOL_SOCKET && optname == SO_RCVBUF) {
        int forced = kRecvBuf;
        int sid = get_socket_id(s, true);
        log_line("hooked_setsockopt: sid=%d sock=0x%08lx forcing SO_RCVBUF=%d", sid, static_cast<unsigned long>(s), forced);
        int rc = g_real_setsockopt(s, level, optname, reinterpret_cast<const char *>(&forced), static_cast<int>(sizeof(forced)));
        log_line("hooked_setsockopt: sid=%d sock=0x%08lx SO_RCVBUF rc=%d wsa=%d", sid, static_cast<unsigned long>(s), rc, static_cast<int>(WSAGetLastError()));
        log_effective_bufs("hooked_setsockopt", s);
        return rc;
    }

    return g_real_setsockopt(s, level, optname, optval, optlen);
}

int WSAAPI hooked_WSASetSocketOption(SOCKET s, int level, int optname, const char *optval, int optlen) {
    if (g_real_wsasetsocketoption == nullptr) {
        return SOCKET_ERROR;
    }

    if (level == SOL_SOCKET && optname == SO_SNDBUF) {
        int forced = kSendBuf;
        int sid = get_socket_id(s, true);
        log_line("hooked_WSASetSocketOption: sid=%d sock=0x%08lx forcing SO_SNDBUF=%d", sid, static_cast<unsigned long>(s), forced);
        int rc = g_real_wsasetsocketoption(s, level, optname, reinterpret_cast<const char *>(&forced), static_cast<int>(sizeof(forced)));
        log_line("hooked_WSASetSocketOption: sid=%d sock=0x%08lx SO_SNDBUF rc=%d wsa=%d", sid, static_cast<unsigned long>(s), rc, static_cast<int>(WSAGetLastError()));
        log_effective_bufs("hooked_WSASetSocketOption", s);
        return rc;
    }

    if (level == SOL_SOCKET && optname == SO_RCVBUF) {
        int forced = kRecvBuf;
        int sid = get_socket_id(s, true);
        log_line("hooked_WSASetSocketOption: sid=%d sock=0x%08lx forcing SO_RCVBUF=%d", sid, static_cast<unsigned long>(s), forced);
        int rc = g_real_wsasetsocketoption(s, level, optname, reinterpret_cast<const char *>(&forced), static_cast<int>(sizeof(forced)));
        log_line("hooked_WSASetSocketOption: sid=%d sock=0x%08lx SO_RCVBUF rc=%d wsa=%d", sid, static_cast<unsigned long>(s), rc, static_cast<int>(WSAGetLastError()));
        log_effective_bufs("hooked_WSASetSocketOption", s);
        return rc;
    }

    return g_real_wsasetsocketoption(s, level, optname, optval, optlen);
}

int WSAAPI hooked_getsockopt(SOCKET s, int level, int optname, char *optval, int *optlen) {
    if (g_real_getsockopt == nullptr) {
        return SOCKET_ERROR;
    }
    int rc = g_real_getsockopt(s, level, optname, optval, optlen);
    if (level == SOL_SOCKET && (optname == SO_SNDBUF || optname == SO_RCVBUF) && rc == 0 && optval != nullptr && optlen != nullptr && *optlen >= static_cast<int>(sizeof(int))) {
        int v = *reinterpret_cast<int *>(optval);
        int sid = get_socket_id(s, true);
        log_line("hooked_getsockopt: sid=%d sock=0x%08lx opt=%s value=%d", sid, static_cast<unsigned long>(s), (optname == SO_SNDBUF) ? "SO_SNDBUF" : "SO_RCVBUF", v);
    }
    return rc;
}

int WSAAPI hooked_WSAGetSocketOption(SOCKET s, int level, int optname, char *optval, int *optlen) {
    if (g_real_wsagetsocketoption == nullptr) {
        return SOCKET_ERROR;
    }
    int rc = g_real_wsagetsocketoption(s, level, optname, optval, optlen);
    if (level == SOL_SOCKET && (optname == SO_SNDBUF || optname == SO_RCVBUF) && rc == 0 && optval != nullptr && optlen != nullptr && *optlen >= static_cast<int>(sizeof(int))) {
        int v = *reinterpret_cast<int *>(optval);
        int sid = get_socket_id(s, true);
        log_line("hooked_WSAGetSocketOption: sid=%d sock=0x%08lx opt=%s value=%d", sid, static_cast<unsigned long>(s), (optname == SO_SNDBUF) ? "SO_SNDBUF" : "SO_RCVBUF", v);
    }
    return rc;
}

SOCKET WSAAPI hooked_socket(int af, int type, int protocol) {
    if (g_real_socket == nullptr) {
        WSASetLastError(WSANOTINITIALISED);
        return INVALID_SOCKET;
    }

    SOCKET s = g_real_socket(af, type, protocol);
    int sid = -1;
    if (s != INVALID_SOCKET) {
        sid = get_socket_id(s, true);
    }
    log_line("hooked_socket: af=%d type=%d proto=%d -> sid=%d sock=0x%08lx rc=%s wsa=%d", af, type, protocol, sid, static_cast<unsigned long>(s), (s == INVALID_SOCKET) ? "INVALID_SOCKET" : "OK", static_cast<int>(WSAGetLastError()));
    return s;
}

SOCKET WSAAPI hooked_WSASocketW(int af, int type, int protocol, LPWSAPROTOCOL_INFOW lpProtocolInfo, GROUP g, DWORD dwFlags) {
    if (g_real_wsasocketw == nullptr) {
        WSASetLastError(WSANOTINITIALISED);
        return INVALID_SOCKET;
    }

    SOCKET s = g_real_wsasocketw(af, type, protocol, lpProtocolInfo, g, dwFlags);
    if (s == INVALID_SOCKET) {
        log_line("hooked_WSASocketW: af=%d type=%d proto=%d -> INVALID_SOCKET wsa=%d", af, type, protocol, static_cast<int>(WSAGetLastError()));
        return s;
    }

    int sid = get_socket_id(s, true);
    log_line("hooked_WSASocketW: af=%d type=%d proto=%d -> sid=%d sock=0x%08lx", af, type, protocol, sid, static_cast<unsigned long>(s));

    // Parity with Windows path: apply target buffers immediately at socket creation.
    if ((type == SOCK_DGRAM || protocol == IPPROTO_UDP) && g_real_setsockopt != nullptr) {
        int snd = kSendBuf;
        int rcv = kRecvBuf;

        int snd_rc = g_real_setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&snd), static_cast<int>(sizeof(snd)));
        int snd_wsa = static_cast<int>(WSAGetLastError());
        int rcv_rc = g_real_setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&rcv), static_cast<int>(sizeof(rcv)));
        int rcv_wsa = static_cast<int>(WSAGetLastError());

        log_line("hooked_WSASocketW: sid=%d sock=0x%08lx apply SO_SNDBUF=%d rc=%d wsa=%d | SO_RCVBUF=%d rc=%d wsa=%d",
                 sid,
                 static_cast<unsigned long>(s),
                 kSendBuf,
                 snd_rc,
                 snd_wsa,
                 kRecvBuf,
                 rcv_rc,
                 rcv_wsa);

        log_effective_bufs("hooked_WSASocketW", s);
    }

    return s;
}

int WSAAPI hooked_closesocket(SOCKET s) {
    if (g_real_closesocket == nullptr) {
        return SOCKET_ERROR;
    }

    int sid = get_socket_id(s, false);
    int rc = g_real_closesocket(s);
    log_line("hooked_closesocket: sid=%d sock=0x%08lx rc=%d wsa=%d", sid, static_cast<unsigned long>(s), rc, static_cast<int>(WSAGetLastError()));
    forget_socket_id(s);
    return rc;
}

FARPROC WINAPI hooked_GetProcAddress(HMODULE module, LPCSTR proc_name) {
    if (g_real_getprocaddress == nullptr) {
        return nullptr;
    }

    FARPROC real = g_real_getprocaddress(module, proc_name);
    if (real == nullptr || proc_name == nullptr) {
        return real;
    }

    if (!module_is_ws2_32(module)) {
        return real;
    }

    if (HIWORD(proc_name) == 0) {
        return real;
    }

    if (_stricmp(proc_name, "setsockopt") == 0 || std::strcmp(proc_name, "_setsockopt@20") == 0) {
        g_real_setsockopt = reinterpret_cast<SetSockOptFn>(real);
        log_line("hooked_GetProcAddress: redirecting %s real=%p hook=%p", proc_name, reinterpret_cast<void *>(real), reinterpret_cast<void *>(&hooked_setsockopt));
        return reinterpret_cast<FARPROC>(&hooked_setsockopt);
    }

    if (_stricmp(proc_name, "WSASetSocketOption") == 0 || std::strcmp(proc_name, "_WSASetSocketOption@20") == 0) {
        g_real_wsasetsocketoption = reinterpret_cast<WSASetSockOptFn>(real);
        log_line("hooked_GetProcAddress: redirecting %s real=%p hook=%p", proc_name, reinterpret_cast<void *>(real), reinterpret_cast<void *>(&hooked_WSASetSocketOption));
        return reinterpret_cast<FARPROC>(&hooked_WSASetSocketOption);
    }

    if (_stricmp(proc_name, "getsockopt") == 0 || std::strcmp(proc_name, "_getsockopt@20") == 0) {
        g_real_getsockopt = reinterpret_cast<GetSockOptFn>(real);
        log_line("hooked_GetProcAddress: redirecting %s real=%p hook=%p", proc_name, reinterpret_cast<void *>(real), reinterpret_cast<void *>(&hooked_getsockopt));
        return reinterpret_cast<FARPROC>(&hooked_getsockopt);
    }

    if (_stricmp(proc_name, "WSAGetSocketOption") == 0 || std::strcmp(proc_name, "_WSAGetSocketOption@20") == 0) {
        g_real_wsagetsocketoption = reinterpret_cast<WSAGetSockOptFn>(real);
        log_line("hooked_GetProcAddress: redirecting %s real=%p hook=%p", proc_name, reinterpret_cast<void *>(real), reinterpret_cast<void *>(&hooked_WSAGetSocketOption));
        return reinterpret_cast<FARPROC>(&hooked_WSAGetSocketOption);
    }

    if (_stricmp(proc_name, "socket") == 0 || std::strcmp(proc_name, "_socket@12") == 0) {
        g_real_socket = reinterpret_cast<SocketFn>(real);
        log_line("hooked_GetProcAddress: redirecting %s real=%p hook=%p", proc_name, reinterpret_cast<void *>(real), reinterpret_cast<void *>(&hooked_socket));
        return reinterpret_cast<FARPROC>(&hooked_socket);
    }

    if (_stricmp(proc_name, "WSASocketW") == 0 || std::strcmp(proc_name, "_WSASocketW@24") == 0) {
        g_real_wsasocketw = reinterpret_cast<WSASocketWFn>(real);
        log_line("hooked_GetProcAddress: redirecting %s real=%p hook=%p", proc_name, reinterpret_cast<void *>(real), reinterpret_cast<void *>(&hooked_WSASocketW));
        return reinterpret_cast<FARPROC>(&hooked_WSASocketW);
    }

    if (_stricmp(proc_name, "closesocket") == 0 || std::strcmp(proc_name, "_closesocket@4") == 0) {
        g_real_closesocket = reinterpret_cast<CloseSocketFn>(real);
        log_line("hooked_GetProcAddress: redirecting %s real=%p hook=%p", proc_name, reinterpret_cast<void *>(real), reinterpret_cast<void *>(&hooked_closesocket));
        return reinterpret_cast<FARPROC>(&hooked_closesocket);
    }

    return real;
}

bool patch_iat_slot(void **slot, void *replacement) {
    DWORD old_protect = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    *slot = replacement;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void *), old_protect, &ignored);
    return true;
}

void patch_module_ws2_iat_by_pointer(BYTE *base) {
    if (base == nullptr || (g_real_setsockopt == nullptr && g_real_wsasetsocketoption == nullptr && g_real_getsockopt == nullptr && g_real_wsagetsocketoption == nullptr && g_real_socket == nullptr && g_real_wsasocketw == nullptr && g_real_closesocket == nullptr)) {
        return;
    }

    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }

    auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) {
        return;
    }

    auto *imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress);
    for (; imports->Name != 0; ++imports) {
        const char *dll_name = reinterpret_cast<const char *>(base + imports->Name);
        if (_stricmp(dll_name, "WS2_32.dll") != 0) {
            continue;
        }

        auto *iat_thunk = reinterpret_cast<IMAGE_THUNK_DATA32 *>(base + imports->FirstThunk);
        int patched_count = 0;
        for (; iat_thunk->u1.Function != 0; ++iat_thunk) {
            void **slot = reinterpret_cast<void **>(&iat_thunk->u1.Function);
            if (g_real_setsockopt != nullptr && *slot == reinterpret_cast<void *>(g_real_setsockopt)) {
                if (patch_iat_slot(slot, reinterpret_cast<void *>(&hooked_setsockopt))) {
                    ++patched_count;
                }
            }
            if (g_real_wsasetsocketoption != nullptr && *slot == reinterpret_cast<void *>(g_real_wsasetsocketoption)) {
                if (patch_iat_slot(slot, reinterpret_cast<void *>(&hooked_WSASetSocketOption))) {
                    ++patched_count;
                }
            }
            if (g_real_getsockopt != nullptr && *slot == reinterpret_cast<void *>(g_real_getsockopt)) {
                if (patch_iat_slot(slot, reinterpret_cast<void *>(&hooked_getsockopt))) {
                    ++patched_count;
                }
            }
            if (g_real_wsagetsocketoption != nullptr && *slot == reinterpret_cast<void *>(g_real_wsagetsocketoption)) {
                if (patch_iat_slot(slot, reinterpret_cast<void *>(&hooked_WSAGetSocketOption))) {
                    ++patched_count;
                }
            }
            if (g_real_socket != nullptr && *slot == reinterpret_cast<void *>(g_real_socket)) {
                if (patch_iat_slot(slot, reinterpret_cast<void *>(&hooked_socket))) {
                    ++patched_count;
                }
            }
            if (g_real_wsasocketw != nullptr && *slot == reinterpret_cast<void *>(g_real_wsasocketw)) {
                if (patch_iat_slot(slot, reinterpret_cast<void *>(&hooked_WSASocketW))) {
                    ++patched_count;
                }
            }
            if (g_real_closesocket != nullptr && *slot == reinterpret_cast<void *>(g_real_closesocket)) {
                if (patch_iat_slot(slot, reinterpret_cast<void *>(&hooked_closesocket))) {
                    ++patched_count;
                }
            }
        }
        if (patched_count > 0) {
            log_line("patch_module_ws2_iat_by_pointer: patched %d slot(s) in module %p", patched_count, base);
        }
        return;
    }
}

void patch_all_loaded_modules_ws2_iat() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        log_line("patch_all_loaded_modules_ws2_iat: snapshot failed gle=%lu", static_cast<unsigned long>(GetLastError()));
        return;
    }

    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    if (!Module32FirstW(snap, &me)) {
        CloseHandle(snap);
        return;
    }

    do {
        patch_module_ws2_iat_by_pointer(reinterpret_cast<BYTE *>(me.modBaseAddr));
    } while (Module32NextW(snap, &me));

    CloseHandle(snap);
}

bool install_getproc_hook_main_module() {
    if (g_installed_getproc_hook) {
        return true;
    }

    auto *base = reinterpret_cast<BYTE *>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
        return false;
    }

    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) {
        return false;
    }

    auto *imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress);
    for (; imports->Name != 0; ++imports) {
        const char *dll_name = reinterpret_cast<const char *>(base + imports->Name);
        if (_stricmp(dll_name, "KERNEL32.dll") != 0 && _stricmp(dll_name, "KERNELBASE.dll") != 0) {
            continue;
        }

        auto *orig_thunk = reinterpret_cast<IMAGE_THUNK_DATA32 *>(base + imports->OriginalFirstThunk);
        auto *iat_thunk = reinterpret_cast<IMAGE_THUNK_DATA32 *>(base + imports->FirstThunk);
        if (imports->OriginalFirstThunk == 0) {
            orig_thunk = iat_thunk;
        }

        for (; orig_thunk->u1.AddressOfData != 0; ++orig_thunk, ++iat_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL32(orig_thunk->u1.Ordinal)) {
                continue;
            }

            auto *name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + orig_thunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char *>(name->Name), "GetProcAddress") != 0) {
                continue;
            }

            void **slot = reinterpret_cast<void **>(&iat_thunk->u1.Function);
            g_real_getprocaddress = reinterpret_cast<GetProcAddressFn>(*slot);
            if (!patch_iat_slot(slot, reinterpret_cast<void *>(&hooked_GetProcAddress))) {
                log_line("install_getproc_hook_main_module: failed patch gle=%lu", static_cast<unsigned long>(GetLastError()));
                return false;
            }

            g_installed_getproc_hook = true;
            log_line("install_getproc_hook_main_module: installed real=%p hook=%p", reinterpret_cast<void *>(g_real_getprocaddress), reinterpret_cast<void *>(&hooked_GetProcAddress));
            return true;
        }
    }

    log_line("install_getproc_hook_main_module: GetProcAddress import not found");
    return false;
}

bool hook_setsockopt_iat() {
    if (!is_target_main_module()) {
        wchar_t path[MAX_PATH] = {0};
        if (GetModuleFileNameW(nullptr, path, MAX_PATH) > 0) {
            char mb[512] = {0};
            WideCharToMultiByte(CP_UTF8, 0, path, -1, mb, static_cast<int>(sizeof(mb)), nullptr, nullptr);
            log_line("hook_setsockopt_iat: skipping non-target process: %s", mb);
        } else {
            log_line("hook_setsockopt_iat: skipping non-target process (path unavailable)");
        }
        return false;
    }

    auto *base = reinterpret_cast<BYTE *>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
        log_line("hook_setsockopt_iat: GetModuleHandleW(NULL) returned null");
        return false;
    }

    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        log_line("hook_setsockopt_iat: invalid DOS signature");
        return false;
    }

    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        log_line("hook_setsockopt_iat: invalid NT signature");
        return false;
    }

    if (!install_getproc_hook_main_module()) {
        log_line("hook_setsockopt_iat: GetProcAddress hook not installed");
    }

    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    if (ws2 == nullptr) {
        ws2 = LoadLibraryW(L"ws2_32.dll");
    }
    if (ws2 == nullptr) {
        log_line("hook_setsockopt_iat: failed to load ws2_32.dll gle=%lu", static_cast<unsigned long>(GetLastError()));
        return false;
    }

    auto getproc = (g_real_getprocaddress != nullptr) ? g_real_getprocaddress : &GetProcAddress;

    FARPROC real_setsockopt = getproc(ws2, "setsockopt");
    FARPROC real_wsaset = getproc(ws2, "WSASetSocketOption");
    FARPROC real_getsockopt = getproc(ws2, "getsockopt");
    FARPROC real_wsaget = getproc(ws2, "WSAGetSocketOption");
    FARPROC real_socket = getproc(ws2, "socket");
    FARPROC real_wsasocketw = getproc(ws2, "WSASocketW");
    FARPROC real_closesocket = getproc(ws2, "closesocket");
    if (real_setsockopt == nullptr && real_wsaset == nullptr && real_getsockopt == nullptr && real_wsaget == nullptr && real_socket == nullptr && real_wsasocketw == nullptr && real_closesocket == nullptr) {
        log_line("hook_setsockopt_iat: failed to resolve target ws2_32 APIs");
        return false;
    }

    if (real_setsockopt != nullptr && g_real_setsockopt == nullptr) {
        g_real_setsockopt = reinterpret_cast<SetSockOptFn>(real_setsockopt);
    }
    if (real_wsaset != nullptr && g_real_wsasetsocketoption == nullptr) {
        g_real_wsasetsocketoption = reinterpret_cast<WSASetSockOptFn>(real_wsaset);
    }
    if (real_getsockopt != nullptr && g_real_getsockopt == nullptr) {
        g_real_getsockopt = reinterpret_cast<GetSockOptFn>(real_getsockopt);
    }
    if (real_wsaget != nullptr && g_real_wsagetsocketoption == nullptr) {
        g_real_wsagetsocketoption = reinterpret_cast<WSAGetSockOptFn>(real_wsaget);
    }
    if (real_socket != nullptr && g_real_socket == nullptr) {
        g_real_socket = reinterpret_cast<SocketFn>(real_socket);
    }
    if (real_wsasocketw != nullptr && g_real_wsasocketw == nullptr) {
        g_real_wsasocketw = reinterpret_cast<WSASocketWFn>(real_wsasocketw);
    }
    if (real_closesocket != nullptr && g_real_closesocket == nullptr) {
        g_real_closesocket = reinterpret_cast<CloseSocketFn>(real_closesocket);
    }

    if (!g_logged_real_setsockopt && g_real_setsockopt != nullptr) {
        g_logged_real_setsockopt = true;
        log_line("hook_setsockopt_iat: real setsockopt=%p", reinterpret_cast<void *>(g_real_setsockopt));
    }
    if (!g_logged_real_wsaset && g_real_wsasetsocketoption != nullptr) {
        g_logged_real_wsaset = true;
        log_line("hook_setsockopt_iat: real WSASetSocketOption=%p", reinterpret_cast<void *>(g_real_wsasetsocketoption));
    }
    if (!g_logged_real_getsockopt && g_real_getsockopt != nullptr) {
        g_logged_real_getsockopt = true;
        log_line("hook_setsockopt_iat: real getsockopt=%p", reinterpret_cast<void *>(g_real_getsockopt));
    }
    if (!g_logged_real_wsaget && g_real_wsagetsocketoption != nullptr) {
        g_logged_real_wsaget = true;
        log_line("hook_setsockopt_iat: real WSAGetSocketOption=%p", reinterpret_cast<void *>(g_real_wsagetsocketoption));
    }
    if (!g_logged_real_socket && g_real_socket != nullptr) {
        g_logged_real_socket = true;
        log_line("hook_setsockopt_iat: real socket=%p", reinterpret_cast<void *>(g_real_socket));
    }
    if (!g_logged_real_wsasocketw && g_real_wsasocketw != nullptr) {
        g_logged_real_wsasocketw = true;
        log_line("hook_setsockopt_iat: real WSASocketW=%p", reinterpret_cast<void *>(g_real_wsasocketw));
    }
    if (!g_logged_real_closesocket && g_real_closesocket != nullptr) {
        g_logged_real_closesocket = true;
        log_line("hook_setsockopt_iat: real closesocket=%p", reinterpret_cast<void *>(g_real_closesocket));
    }

    patch_all_loaded_modules_ws2_iat();
    return true;
}

DWORD WINAPI hook_worker_thread(LPVOID) {
    if (!is_target_main_module()) {
        return 0;
    }

    // Keep trying briefly in case networking-related modules resolve imports just after startup.
    for (int i = 0; i < 160; ++i) {
        hook_setsockopt_iat();
        Sleep(10);
    }
    return 0;
}

bool load_real_dsound() {
    if (g_real_dsound != nullptr && g_real_ordinal_1 != nullptr) {
        return true;
    }

    wchar_t system_dir[MAX_PATH] = {0};
    UINT len = GetSystemDirectoryW(system_dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH - 12) {
        return false;
    }

    lstrcatW(system_dir, L"\\dsound.dll");
    g_real_dsound = LoadLibraryW(system_dir);
    if (g_real_dsound == nullptr) {
        return false;
    }

    g_real_ordinal_1 = GetProcAddress(g_real_dsound, MAKEINTRESOURCEA(1));
    return g_real_ordinal_1 != nullptr;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (!g_track_lock_ready) {
            InitializeCriticalSection(&g_track_lock);
            g_track_lock_ready = true;
            for (int i = 0; i < kSocketTrackCap; ++i) {
                g_socket_tracks[i].s = INVALID_SOCKET;
                g_socket_tracks[i].id = 0;
            }
        }
        log_line("DllMain: DLL_PROCESS_ATTACH");
        if (!hook_setsockopt_iat()) {
            log_line("DllMain: initial hook install attempt failed");
        }
        HANDLE thread = CreateThread(nullptr, 0, &hook_worker_thread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        } else {
            log_line("DllMain: failed to create hook worker thread gle=%lu", static_cast<unsigned long>(GetLastError()));
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_track_lock_ready) {
            DeleteCriticalSection(&g_track_lock);
            g_track_lock_ready = false;
        }
    }

    return TRUE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DirectSoundCreate_proxy(LPCGUID guid, LPVOID *device, LPUNKNOWN outer) {
    using DirectSoundCreateFn = HRESULT(WINAPI *)(LPCGUID, LPVOID *, LPUNKNOWN);

    if (!load_real_dsound()) {
        log_line("DirectSoundCreate_proxy: failed to load real dsound.dll");
        return E_FAIL;
    }

    log_line("DirectSoundCreate_proxy: forwarding to real ordinal 1");
    auto real_fn = reinterpret_cast<DirectSoundCreateFn>(reinterpret_cast<void *>(g_real_ordinal_1));
    return real_fn(guid, device, outer);
}