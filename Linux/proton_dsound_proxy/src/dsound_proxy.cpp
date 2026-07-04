#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <cstdint>
#include <cstdlib>
#include <cctype>

namespace {

constexpr wchar_t kLogFileName[] = L"dsound_proxy.log";
wchar_t g_log_path[MAX_PATH] = L"dsound_proxy.log";
bool g_log_path_ready = false;

constexpr wchar_t kBufferBinName[] = L"bz_buffer_log.bin";
constexpr wchar_t kBufferMetaName[] = L"bz_buffer_log.meta.txt";
wchar_t g_buffer_bin_path[MAX_PATH] = L"bz_buffer_log.bin";
wchar_t g_buffer_meta_path[MAX_PATH] = L"bz_buffer_log.meta.txt";
bool g_buffer_paths_ready = false;

constexpr int kSendBuf = 524288;
constexpr int kRecvBuf = 4194304;

// DSCP class for the game P2P socket.  46 == Expedited Forwarding: routers
// with WMM (WiFi voice queue) or SQM/fq_codel serve these ahead of bulk
// traffic, directly targeting the queueing-delay mechanism behind the
// stale-drop bursts we measured.  Under Proton, Wine forwards IP_TOS to the
// Linux socket and the kernel honours it (real effect on this platform).
// BZ_DSCP overrides the class; BZ_DSCP=0 disables the marking entirely.
constexpr uint32_t kDscpDefault = 46;   // EF

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
bool g_logged_real_recvfrom = false;
bool g_logged_real_wsarecvfrom = false;
bool g_logged_real_ioctlsocket = false;
bool g_logged_real_wsaioctl = false;

using RecvFromFn = int(WSAAPI *)(SOCKET, char *, int, int, sockaddr *, int *);
RecvFromFn g_real_recvfrom = nullptr;
using WSARecvFromFn = int(WSAAPI *)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, sockaddr *, LPINT, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
WSARecvFromFn g_real_wsarecvfrom = nullptr;
using IoctlSocketFn = int(WSAAPI *)(SOCKET, long, u_long *);
IoctlSocketFn g_real_ioctlsocket = nullptr;
using WSAIoctlFn = int(WSAAPI *)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
WSAIoctlFn g_real_wsaioclt = nullptr;

constexpr uint32_t kBufferLogVersion = 1;
constexpr uint32_t kBufferLogMagic = 0x474c5a42; // 'BZLG'
constexpr uint32_t kEventTypeRecvFrom = 1;
constexpr uint32_t kEventTypeWSARecvFrom = 2;
constexpr uint32_t kEventTypeIoctlSocket = 3;
constexpr uint32_t kEventTypeWSAIoctl = 4;

constexpr uint32_t kDefaultPayloadBytes = 32;
constexpr uint32_t kDefaultRingRecords = 65536;
constexpr uint32_t kMinPayloadBytes = 8;
constexpr uint32_t kMaxPayloadBytes = 256;
constexpr uint32_t kMinRingRecords = 1024;
constexpr uint32_t kMaxRingRecords = 1000000;

#pragma pack(push, 1)
struct BufferLogRecordHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t event_type;
    uint32_t sid;
    uint64_t tick_ms;
    uint32_t sequence;
    uint32_t requested_len;
    uint32_t transferred_len;
    uint32_t wsa_error;
    uint32_t src_ipv4;
    uint16_t src_port;
    uint16_t flags;
    uint16_t payload_len;
    uint16_t reserved;
};
#pragma pack(pop)

CRITICAL_SECTION g_buffer_lock = {};
bool g_buffer_lock_ready = false;
bool g_buffer_log_initialized = false;
bool g_buffer_log_enabled = false;
uint32_t g_buffer_payload_bytes = kDefaultPayloadBytes;
uint32_t g_buffer_ring_records = kDefaultRingRecords;
uint32_t g_buffer_stride = static_cast<uint32_t>(sizeof(BufferLogRecordHeader) + kDefaultPayloadBytes);
uint32_t g_buffer_head = 0;
uint32_t g_buffer_count = 0;
uint32_t g_buffer_sequence = 0;
uint64_t g_buffer_total_events = 0;
uint8_t *g_buffer_ring = nullptr;

// ── Per-peer reorder buffer ──────────────────────────────────────────────────
// Enabled by default.  Set BZ_REORDER=0 to disable.  WSARecvFrom packets are
// held in a per-source priority queue ordered by sequence number and delivered
// to the game in order.  BZ_REORDER_WINDOW_MS (default 45 ms) is the max time
// a packet may be held before being forced out regardless of whether its
// predecessor arrived.
//
// Sequence field location: u32le at payload byte offset 13, confirmed via
// live binary capture analysis (resources/valid_capture_reorder_signal_only.csv).
constexpr uint32_t kReorderSeqOffset   = 13;    // byte offset in payload
constexpr uint32_t kReorderSeqMinPay   = 17;    // minimum payload length with seq field
constexpr uint32_t kReorderDefaultMs   = 100;   // max hold window (ms); 45->100 cut drops ~65% in live A/B testing
constexpr uint32_t kReorderMinMsDef    = 5;     // adaptive window floor (ms)
constexpr uint32_t kReorderGrowPadMs   = 5;     // safety margin added on window growth
constexpr uint32_t kReorderDecayMs     = 2000;  // quiet period before the window decays
constexpr uint32_t kReorderDecayStepMs = 5;     // window shrink per decay step
constexpr uint32_t kReorderSlotCap     = 8;     // max per-peer buffered packet slots
constexpr uint32_t kReorderPeerCap     = 32;    // max distinct IPv4 sources
constexpr uint32_t kReorderDrainCapDef = 96;    // default real WSARecvFrom calls per hook invocation
constexpr uint32_t kReorderDrainCapMax = 128;   // hard cap for drain loop
constexpr uint32_t kReorderMaxPktBytes = 1500;  // max UDP datagram size
constexpr uint32_t kReorderWakeTickMs  = 10;    // wake-thread poll interval
constexpr uint32_t kReorderWakeIdleMs  = 10;    // send wake only if game hasn't polled this long
constexpr uint32_t kReorderWakeBurstCap = 8;    // max wakes without an intervening game poll

struct ReorderSlot {
    uint64_t    ts;                          // GetTickCount64() on arrival
    uint32_t    seq;                         // BZRNet sequence number (payload[13] u32le)
    uint32_t    len;                         // payload byte count
    uint32_t    used;                        // 1 = slot is occupied
    uint32_t    _pad;
    sockaddr_in from;                        // source address
    uint8_t     data[kReorderMaxPktBytes];   // full packet contents
};

struct PeerBuf {
    uint64_t    key;            // (ipv4_raw << 16) | port_host_order; 0 = empty
    uint32_t    seq_init;       // 1 once last_seq is valid
    uint32_t    last_seq;       // last sequence number delivered to the game
    uint32_t    filled;         // number of occupied slots
    uint32_t    win_ms;         // adaptive hold window for this peer
    uint64_t    last_adjust_ms; // last window grow/decay timestamp
    ReorderSlot slots[kReorderSlotCap];
};

static bool              g_reorder_enabled  = false;
static bool              g_reorder_adapt    = true;    // BZ_REORDER_ADAPT=0 -> fixed window
static uint32_t          g_reorder_ms       = kReorderDefaultMs;   // window ceiling
static uint32_t          g_reorder_min_ms   = kReorderMinMsDef;    // adaptive floor
static uint32_t          g_reorder_depth    = kReorderSlotCap;
static uint32_t          g_reorder_peers    = kReorderPeerCap;
static uint32_t          g_reorder_drain    = kReorderDrainCapDef;
static PeerBuf           g_peers[kReorderPeerCap];    // zero-initialized (BSS)
static CRITICAL_SECTION  g_reorder_cs       = {};
static bool              g_reorder_cs_ready = false;

// Wake helper: the reorder hook drains the kernel socket, so a game thread
// sleeping in select()/WSAEventSelect() never sees the socket readable while
// packets sit in our userspace queue.  A background thread sends a tiny magic
// datagram to the game socket's own bound port to mark it readable, waking
// the game so held packets are released within the reorder window instead of
// stranding until the next real packet arrives.  BZ_REORDER_WAKE=0 disables.
static const uint8_t     kWakeMagic[8]      = {'B','Z','W','K','P','K','T','1'};
static bool              g_wake_enabled     = true;
static volatile LONG     g_wake_stop        = 0;
static SOCKET            g_wake_sender      = INVALID_SOCKET;
static SOCKET            g_reorder_sock     = INVALID_SOCKET;  // last socket seen in reorder path
static uint64_t          g_last_recv_call_ms = 0;              // last game WSARecvFrom (reorder path)
static bool              g_wake_logged      = false;
using SendToFn = int(WSAAPI *)(SOCKET, const char *, int, int, const sockaddr *, int);
static SendToFn g_real_sendto = nullptr;

// Opt-in loss redundancy: BZ_SEND_DUP=1 re-sends outbound game P2P datagrams.
// Reordering cannot recover a packet the network dropped; a duplicate can.
// The receiver tolerates duplicates whether patched (the reorder buffer
// dedups by sequence) or vanilla (BZRNet drops stale sequence numbers).
//
// Live testing (2026-07-03 KFK set) showed naive back-to-back duplication
// degrades constrained uplinks: it doubles packets-per-second at the exact
// moment the link is queueing, and a copy sent in the same burst dies in the
// same burst.  Three mitigations, all sender-side and safe against unpatched
// receivers:
//   - never duplicate to loopback (the game keeps a P2P connection to
//     itself; duplicating it only pollutes drop metrics),
//   - transmit the copy BZ_DUP_DELAY_MS later (RFC2198-style time shift, so
//     one queue spike cannot kill both copies; 0 = legacy back-to-back),
//   - cap duplicates at BZ_DUP_MAX_PPS per second (low-rate control traffic
//     gets redundancy first; bulk bursts shed theirs; 0 = unlimited).
constexpr uint32_t       kDupQueueSlots     = 128;
constexpr uint32_t       kDupTickMs         = 5;
constexpr uint32_t       kDupDelayMsDef     = 25;
constexpr uint32_t       kDupMaxPpsDef      = 40;

static bool              g_send_dup         = false;
static uint32_t          g_dup_delay_ms     = kDupDelayMsDef;
static uint32_t          g_dup_max_pps      = kDupMaxPpsDef;
static uint32_t          g_dscp             = kDscpDefault;
// Opt-in diagnostic (BZ_GOV_SCAN=1, default off).  The exe is SteamStub-DRM
// wrapped so .text is encrypted on disk and cannot be signature-scanned
// offline; this scans the DECRYPTED .text at runtime for the governor's
// hardcoded 4000-byte/s start constant (0x00000FA0) and logs each candidate
// site with surrounding bytes, so the real runtime governor patch can be
// built from a genuine signature next iteration.  Read-only; never patches.
static bool              g_gov_scan         = false;

// Governor cold-start patch (BZ_GOV_START=<bytes/sec>, default 0 = disabled).
// The send governor hardcodes a 4000 B/s start for every match (immune to
// net.ini MinBandwidth, which is copied to the live rate BEFORE net.ini is
// read), which starves the opening world-state burst and produces the
// first-60-seconds drop clusters.  Captured 2026-07-03 from decrypted .text:
// a single unique site `push 4000; push 1000; push -3000` feeds the
// session-start governor setup.  We rewrite the 4000 immediate to
// g_gov_start.  Signature-scanned (not a fixed address) so it survives game
// updates; patched only on an exactly-one match whose immediate is still
// 4000.  Sender-side: improves how our packets arrive at every peer, patched
// or not.
static uint32_t          g_gov_start        = 0;
static volatile LONG     g_gov_stop         = 0;
constexpr DWORD          kGovPollMs         = 100;   // send-rate watch interval
// Unique version fingerprint: push 0xFA0(4000); push 0x3E8(1000); push -3000.
// Used read-only to confirm the game build before trusting the data address.
static const uint8_t     kGovSig[15] = {
    0x68, 0xA0, 0x0F, 0x00, 0x00,
    0x68, 0xE8, 0x03, 0x00, 0x00,
    0x68, 0x48, 0xF4, 0xFF, 0xFF
};

struct DupEntry {
    SOCKET           sock;
    uint64_t         due_ms;
    int              tolen;
    uint32_t         len;
    sockaddr_storage to;
    uint8_t          data[kReorderMaxPktBytes];
};
static DupEntry          g_dup_q[kDupQueueSlots];
static uint32_t          g_dup_q_head       = 0;
static uint32_t          g_dup_q_count      = 0;
static CRITICAL_SECTION  g_dup_cs           = {};
static bool              g_dup_cs_ready     = false;
static volatile LONG     g_dup_stop         = 0;
static uint64_t          g_dup_bucket_start_ms = 0;
static uint32_t          g_dup_bucket_sent  = 0;
bool g_logged_real_sendto = false;
// The game exe's IAT has no plain sendto import: all P2P sends go through
// WSASendTo, so that hook is the one that makes BZ_SEND_DUP effective.
using WSASendToFn = int(WSAAPI *)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr *, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static WSASendToFn g_real_wsasendto = nullptr;
bool g_logged_real_wsasendto = false;
using GetSockNameFn = int(WSAAPI *)(SOCKET, sockaddr *, int *);
static GetSockNameFn g_real_getsockname = nullptr;

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
int get_socket_id(SOCKET s, bool create_if_missing);

bool env_truthy(const char *s) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    if (std::strcmp(s, "1") == 0) {
        return true;
    }
    char lower[16] = {0};
    size_t n = std::strlen(s);
    if (n >= sizeof(lower)) {
        n = sizeof(lower) - 1;
    }
    for (size_t i = 0; i < n; ++i) {
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    }
    lower[n] = '\0';
    return std::strcmp(lower, "true") == 0 || std::strcmp(lower, "yes") == 0 || std::strcmp(lower, "on") == 0;
}

uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

uint32_t parse_env_u32(const char *name, uint32_t fallback) {
    const char *v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    char *end = nullptr;
    unsigned long parsed = std::strtoul(v, &end, 10);
    if (end == nullptr || *end != '\0') {
        return fallback;
    }
    if (parsed > 0xffffffffUL) {
        return fallback;
    }
    return static_cast<uint32_t>(parsed);
}

// True when the destination is the game's loopback self-connection.
static bool dup_is_loopback(const sockaddr *to) {
    if (to == nullptr || to->sa_family != AF_INET) {
        return false;
    }
    const sockaddr_in *in4 = reinterpret_cast<const sockaddr_in *>(to);
    return (ntohl(in4->sin_addr.s_addr) >> 24) == 127;
}

// Mark a UDP socket with the configured DSCP class via IP_TOS.  No-op when
// g_dscp is 0 or setsockopt is unavailable.  Returns the setsockopt rc.
static int apply_dscp(SOCKET s) {
    if (g_dscp == 0 || g_real_setsockopt == nullptr) {
        return 0;
    }
    // The TOS byte carries DSCP in its top 6 bits.
    int tos = static_cast<int>(g_dscp << 2);
    return g_real_setsockopt(s, IPPROTO_IP, IP_TOS,
        reinterpret_cast<const char *>(&tos), static_cast<int>(sizeof(tos)));
}

// Rate-gate and enqueue a delayed duplicate.  Drops the duplicate (never
// blocks, never fails the original send) when the budget or queue is full.
static void dup_enqueue(SOCKET s, const uint8_t *data, uint32_t len,
                        const sockaddr *to, int tolen) {
    if (!g_dup_cs_ready || data == nullptr || len == 0 || len > kReorderMaxPktBytes) {
        return;
    }
    if (to == nullptr || tolen <= 0
        || static_cast<size_t>(tolen) > sizeof(sockaddr_storage)) {
        return;
    }
    uint64_t now = GetTickCount64();
    EnterCriticalSection(&g_dup_cs);
    if (now - g_dup_bucket_start_ms >= 1000) {
        g_dup_bucket_start_ms = now;
        g_dup_bucket_sent = 0;
    }
    if ((g_dup_max_pps != 0 && g_dup_bucket_sent >= g_dup_max_pps)
        || g_dup_q_count >= kDupQueueSlots) {
        LeaveCriticalSection(&g_dup_cs);
        return;
    }
    g_dup_bucket_sent++;
    DupEntry &e = g_dup_q[(g_dup_q_head + g_dup_q_count) % kDupQueueSlots];
    e.sock = s;
    e.due_ms = now + g_dup_delay_ms;
    e.tolen = tolen;
    e.len = len;
    std::memcpy(&e.to, to, static_cast<size_t>(tolen));
    std::memcpy(e.data, data, len);
    g_dup_q_count++;
    LeaveCriticalSection(&g_dup_cs);
}

// Drop queued duplicates for a socket the game just closed: the handle may
// be reused, and a stale duplicate on a fresh socket would corrupt state.
static void dup_purge_socket(SOCKET s) {
    if (!g_dup_cs_ready) {
        return;
    }
    EnterCriticalSection(&g_dup_cs);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g_dup_q_count; ++i) {
        DupEntry &e = g_dup_q[(g_dup_q_head + i) % kDupQueueSlots];
        if (e.sock == s) {
            continue;
        }
        if (kept != i) {
            g_dup_q[(g_dup_q_head + kept) % kDupQueueSlots] = e;
        }
        kept++;
    }
    g_dup_q_count = kept;
    LeaveCriticalSection(&g_dup_cs);
}

// Pacer thread: transmits queued duplicates once their delay elapses.
static DWORD WINAPI dup_pacer_thread(LPVOID) {
    while (InterlockedCompareExchange(&g_dup_stop, 0, 0) == 0) {
        Sleep(kDupTickMs);
        if (!g_dup_cs_ready || g_real_sendto == nullptr) {
            continue;
        }
        for (;;) {
            DupEntry local;
            EnterCriticalSection(&g_dup_cs);
            if (g_dup_q_count == 0
                || g_dup_q[g_dup_q_head].due_ms > GetTickCount64()) {
                LeaveCriticalSection(&g_dup_cs);
                break;
            }
            local = g_dup_q[g_dup_q_head];
            g_dup_q_head = (g_dup_q_head + 1) % kDupQueueSlots;
            g_dup_q_count--;
            LeaveCriticalSection(&g_dup_cs);
            g_real_sendto(local.sock, reinterpret_cast<const char *>(local.data),
                          static_cast<int>(local.len), 0,
                          reinterpret_cast<const sockaddr *>(&local.to), local.tolen);
        }
    }
    return 0;
}

static inline int32_t seq_cmp_u32(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b);
}

static inline bool seq_ahead_or_equal(uint32_t seq, uint32_t want) {
    return seq_cmp_u32(seq, want) >= 0;
}

void init_buffer_paths() {
    if (g_buffer_paths_ready) {
        return;
    }

    wchar_t exe_path[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        g_buffer_paths_ready = true;
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

    g_buffer_bin_path[0] = L'\0';
    if (lstrlenW(exe_path) + lstrlenW(kBufferBinName) + 1 < MAX_PATH) {
        lstrcpyW(g_buffer_bin_path, exe_path);
    }
    lstrcatW(g_buffer_bin_path, kBufferBinName);

    g_buffer_meta_path[0] = L'\0';
    if (lstrlenW(exe_path) + lstrlenW(kBufferMetaName) + 1 < MAX_PATH) {
        lstrcpyW(g_buffer_meta_path, exe_path);
    }
    lstrcatW(g_buffer_meta_path, kBufferMetaName);

    g_buffer_paths_ready = true;
}

void init_buffer_log_if_needed() {
    if (g_buffer_log_initialized) {
        return;
    }
    g_buffer_log_initialized = true;
    init_buffer_paths();

    const char *enabled = std::getenv("BZ_BUFFER_LOG");
    if (!env_truthy(enabled)) {
        log_line("buffer_log: disabled (set BZ_BUFFER_LOG=1 to enable)");
        return;
    }

    g_buffer_payload_bytes = clamp_u32(parse_env_u32("BZ_BUFFER_LOG_BYTES", kDefaultPayloadBytes), kMinPayloadBytes, kMaxPayloadBytes);
    g_buffer_ring_records = clamp_u32(parse_env_u32("BZ_BUFFER_LOG_RING", kDefaultRingRecords), kMinRingRecords, kMaxRingRecords);
    g_buffer_stride = static_cast<uint32_t>(sizeof(BufferLogRecordHeader) + g_buffer_payload_bytes);

    size_t total = static_cast<size_t>(g_buffer_stride) * static_cast<size_t>(g_buffer_ring_records);
    g_buffer_ring = reinterpret_cast<uint8_t *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total));
    if (g_buffer_ring == nullptr) {
        log_line("buffer_log: allocation failed bytes=%lu", static_cast<unsigned long>(total));
        return;
    }

    g_buffer_log_enabled = true;
    log_line("buffer_log: enabled payload=%u ring=%u stride=%u", g_buffer_payload_bytes, g_buffer_ring_records, g_buffer_stride);
}

void buffer_log_event(uint32_t event_type,
                      SOCKET s,
                      const sockaddr *src,
                      uint16_t flags,
                      uint32_t requested_len,
                      uint32_t transferred_len,
                      uint32_t wsa_error,
                      const uint8_t *payload,
                      uint16_t payload_len) {
    if (!g_buffer_log_enabled || !g_buffer_lock_ready || g_buffer_ring == nullptr) {
        return;
    }

    if (payload_len > g_buffer_payload_bytes) {
        payload_len = static_cast<uint16_t>(g_buffer_payload_bytes);
    }

    uint32_t src_ipv4 = 0;
    uint16_t src_port = 0;
    if (src != nullptr && src->sa_family == AF_INET) {
        const sockaddr_in *in = reinterpret_cast<const sockaddr_in *>(src);
        src_ipv4 = static_cast<uint32_t>(in->sin_addr.S_un.S_addr);
        src_port = ntohs(in->sin_port);
    }

    int sid = get_socket_id(s, true);
    if (sid < 0) {
        sid = 0;
    }

    EnterCriticalSection(&g_buffer_lock);
    uint32_t idx = g_buffer_head;
    uint8_t *slot = g_buffer_ring + (static_cast<size_t>(idx) * static_cast<size_t>(g_buffer_stride));

    BufferLogRecordHeader rec = {};
    rec.magic = kBufferLogMagic;
    rec.version = kBufferLogVersion;
    rec.event_type = event_type;
    rec.sid = static_cast<uint32_t>(sid);
    rec.tick_ms = GetTickCount64();
    rec.sequence = g_buffer_sequence++;
    rec.requested_len = requested_len;
    rec.transferred_len = transferred_len;
    rec.wsa_error = wsa_error;
    rec.src_ipv4 = src_ipv4;
    rec.src_port = src_port;
    rec.flags = flags;
    rec.payload_len = payload_len;
    std::memcpy(slot, &rec, sizeof(rec));

    uint8_t *payload_dst = slot + sizeof(rec);
    if (payload_len > 0 && payload != nullptr) {
        std::memcpy(payload_dst, payload, payload_len);
    }
    if (payload_len < g_buffer_payload_bytes) {
        std::memset(payload_dst + payload_len, 0, g_buffer_payload_bytes - payload_len);
    }

    g_buffer_head = (g_buffer_head + 1) % g_buffer_ring_records;
    if (g_buffer_count < g_buffer_ring_records) {
        ++g_buffer_count;
    }
    ++g_buffer_total_events;
    LeaveCriticalSection(&g_buffer_lock);
}

void flush_buffer_log_files() {
    if (!g_buffer_log_enabled || g_buffer_ring == nullptr) {
        return;
    }

    init_buffer_paths();

    HANDLE bin = CreateFileW(g_buffer_bin_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (bin != INVALID_HANDLE_VALUE) {
        EnterCriticalSection(&g_buffer_lock);
        uint32_t count = g_buffer_count;
        uint32_t start = (g_buffer_head + g_buffer_ring_records - g_buffer_count) % g_buffer_ring_records;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t idx = (start + i) % g_buffer_ring_records;
            const uint8_t *slot = g_buffer_ring + (static_cast<size_t>(idx) * static_cast<size_t>(g_buffer_stride));
            DWORD written = 0;
            WriteFile(bin, slot, g_buffer_stride, &written, nullptr);
        }
        LeaveCriticalSection(&g_buffer_lock);
        CloseHandle(bin);
    }

    HANDLE meta = CreateFileW(g_buffer_meta_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (meta != INVALID_HANDLE_VALUE) {
        char text[1024] = {0};
        int n = std::snprintf(text,
                              sizeof(text),
                              "format=buffer_log_v1\\r\\nrecord_header_size=%u\\r\\npayload_bytes=%u\\r\\nrecord_stride=%u\\r\\nring_records=%u\\r\\nrecords_written=%u\\r\\ntotal_events_seen=%llu\\r\\n",
                              static_cast<unsigned>(sizeof(BufferLogRecordHeader)),
                              static_cast<unsigned>(g_buffer_payload_bytes),
                              static_cast<unsigned>(g_buffer_stride),
                              static_cast<unsigned>(g_buffer_ring_records),
                              static_cast<unsigned>(g_buffer_count),
                              static_cast<unsigned long long>(g_buffer_total_events));
        if (n > 0) {
            DWORD written = 0;
            WriteFile(meta, text, static_cast<DWORD>(n), &written, nullptr);
        }
        CloseHandle(meta);
    }

    log_line("buffer_log: flushed records=%u total_events=%llu", static_cast<unsigned>(g_buffer_count), static_cast<unsigned long long>(g_buffer_total_events));
}

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
        apply_dscp(s);
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
        int tos_rc = apply_dscp(s);

        log_line("hooked_WSASocketW: sid=%d sock=0x%08lx apply SO_SNDBUF=%d rc=%d wsa=%d | SO_RCVBUF=%d rc=%d wsa=%d | DSCP=%u IP_TOS rc=%d",
                 sid,
                 static_cast<unsigned long>(s),
                 kSendBuf,
                 snd_rc,
                 snd_wsa,
                 kRecvBuf,
                 rcv_rc,
                 rcv_wsa,
                 static_cast<unsigned>(g_dscp),
                 tos_rc);

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
    // Reset per-peer reorder state.  BZ uses one UDP socket for all P2P; closing
    // it ends the session, so all buffered packets are now stale.
    if (g_reorder_cs_ready) {
        EnterCriticalSection(&g_reorder_cs);
        std::memset(g_peers, 0, sizeof(g_peers));
        if (s == g_reorder_sock) {
            g_reorder_sock = INVALID_SOCKET;
        }
        LeaveCriticalSection(&g_reorder_cs);
    }
    dup_purge_socket(s);
    return rc;
}

// ── Reorder buffer helpers ────────────────────────────────────────────────────

// Copy from a flat buffer into caller's WSA scatter-gather segments.
// Returns the number of bytes written across all segments.
static uint32_t scatter_copy(LPWSABUF bufs, DWORD nbufs, const uint8_t *src, uint32_t srclen) {
    uint32_t done = 0;
    for (DWORD bi = 0; bi < nbufs && done < srclen; ++bi) {
        if (bufs[bi].buf == nullptr || bufs[bi].len == 0) {
            continue;
        }
        uint32_t chunk = srclen - done;
        if (chunk > static_cast<uint32_t>(bufs[bi].len)) {
            chunk = static_cast<uint32_t>(bufs[bi].len);
        }
        std::memcpy(bufs[bi].buf, src + done, chunk);
        done += chunk;
    }
    return done;
}

// Look up or create the PeerBuf for addr.  Caller must hold g_reorder_cs.
static PeerBuf *reorder_get_peer(const sockaddr_in &addr) {
    uint64_t k = (static_cast<uint64_t>(static_cast<uint32_t>(addr.sin_addr.S_un.S_addr)) << 16)
                 | static_cast<uint64_t>(ntohs(addr.sin_port));
    for (uint32_t i = 0; i < g_reorder_peers; ++i) {
        if (g_peers[i].key == k) {
            return &g_peers[i];
        }
    }
    for (uint32_t i = 0; i < g_reorder_peers; ++i) {
        if (g_peers[i].key == 0) {
            std::memset(&g_peers[i], 0, sizeof(g_peers[i]));
            g_peers[i].key = k;
            g_peers[i].win_ms = g_reorder_adapt ? g_reorder_min_ms : g_reorder_ms;
            g_peers[i].last_adjust_ms = GetTickCount64();
            return &g_peers[i];
        }
    }
    return nullptr; // peer table full
}

// Adapt the peer's hold window based on the arriving packet, BEFORE insertion.
// Grow on evidence that reordering actually happens on this link:
//   - a packet we already skipped past arrives late (window was too small), or
//   - the awaited in-order successor arrives while later packets are held
//     (the wait it resolved tells us how big the window needs to be).
// True loss never grows the window: a lost packet simply never arrives.
// Caller must hold g_reorder_cs.
static void reorder_adapt_on_arrival(PeerBuf *pb, uint32_t seq, uint64_t now_ms) {
    if (!g_reorder_adapt || !pb->seq_init) {
        return;
    }

    int32_t cmp = seq_cmp_u32(seq, pb->last_seq);
    if (cmp == 0) {
        // Exact duplicate of the last delivered packet (link-layer retransmit,
        // common on WiFi): not reorder evidence, must not grow the window.
        return;
    }
    if (cmp < 0) {
        // Late/backward arrival: we released its successors too early.
        uint32_t grown = pb->win_ms * 2 + kReorderGrowPadMs;
        pb->win_ms = (grown > g_reorder_ms) ? g_reorder_ms : grown;
        pb->last_adjust_ms = now_ms;
        return;
    }

    if (seq == pb->last_seq + 1 && pb->filled > 0) {
        // Gap just closed: measure how long the held packets waited.
        uint64_t oldest_ts = now_ms;
        for (uint32_t i = 0; i < g_reorder_depth; ++i) {
            if (pb->slots[i].used && pb->slots[i].ts < oldest_ts) {
                oldest_ts = pb->slots[i].ts;
            }
        }
        uint32_t waited = static_cast<uint32_t>(now_ms - oldest_ts) + kReorderGrowPadMs;
        if (waited > g_reorder_ms) {
            waited = g_reorder_ms;
        }
        if (waited > pb->win_ms) {
            pb->win_ms = waited;
            pb->last_adjust_ms = now_ms;
        }
    }
}

// Shrink the window back toward the floor after a quiet period with no
// reorder evidence.  Called on delivery.  Caller must hold g_reorder_cs.
static void reorder_decay(PeerBuf *pb, uint64_t now_ms) {
    if (!g_reorder_adapt || now_ms - pb->last_adjust_ms < kReorderDecayMs) {
        return;
    }
    pb->win_ms = (pb->win_ms > g_reorder_min_ms + kReorderDecayStepMs)
                 ? pb->win_ms - kReorderDecayStepMs : g_reorder_min_ms;
    pb->last_adjust_ms = now_ms;
}

// Insert a received packet.  Duplicates are silently dropped.  When all slots
// are full the oldest packet is evicted to make room.  Caller must hold g_reorder_cs.
static void reorder_insert(PeerBuf *pb, uint32_t seq, uint64_t ts,
                           const sockaddr_in &from, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < g_reorder_depth; ++i) {
        if (pb->slots[i].used && pb->slots[i].seq == seq) {
            return; // duplicate
        }
    }
    for (uint32_t i = 0; i < g_reorder_depth; ++i) {
        if (!pb->slots[i].used) {
            pb->slots[i].used = 1;
            pb->slots[i].seq  = seq;
            pb->slots[i].ts   = ts;
            pb->slots[i].from = from;
            uint32_t n = (len > kReorderMaxPktBytes) ? kReorderMaxPktBytes : len;
            std::memcpy(pb->slots[i].data, data, n);
            pb->slots[i].len = n;
            ++pb->filled;
            return;
        }
    }
    // All slots occupied: evict the oldest.
    uint32_t oix = 0;
    for (uint32_t i = 1; i < g_reorder_depth; ++i) {
        if (pb->slots[i].used && pb->slots[i].ts < pb->slots[oix].ts) {
            oix = i;
        }
    }
    pb->slots[oix].used = 1;
    pb->slots[oix].seq  = seq;
    pb->slots[oix].ts   = ts;
    pb->slots[oix].from = from;
    uint32_t n = (len > kReorderMaxPktBytes) ? kReorderMaxPktBytes : len;
    std::memcpy(pb->slots[oix].data, data, n);
    pb->slots[oix].len = n;
    // filled count unchanged: one evicted, one inserted
}

// Find the best slot to deliver.  Prefers the exact in-order successor of
// last_seq, falling back to the lowest-seq packet once it has aged out.
// Returns slot index or -1 if nothing is ready.  Caller must hold g_reorder_cs.
static int reorder_pick(PeerBuf *pb, uint64_t now_ms) {
    if (pb->filled == 0) {
        return -1;
    }
    if (pb->seq_init) {
        uint32_t want = pb->last_seq + 1;
        for (uint32_t i = 0; i < g_reorder_depth; ++i) {
            if (pb->slots[i].used && pb->slots[i].seq == want) {
                return static_cast<int>(i);
            }
        }

        int best_ahead = -1;
        uint32_t best_dist = 0;
        int best_oldest = -1;
        for (uint32_t i = 0; i < g_reorder_depth; ++i) {
            if (!pb->slots[i].used) {
                continue;
            }
            if (now_ms < pb->slots[i].ts || (now_ms - pb->slots[i].ts) < pb->win_ms) {
                continue;
            }

            if (best_oldest < 0 || pb->slots[i].ts < pb->slots[best_oldest].ts) {
                best_oldest = static_cast<int>(i);
            }

            if (seq_ahead_or_equal(pb->slots[i].seq, want)) {
                uint32_t dist = pb->slots[i].seq - want;
                if (best_ahead < 0 || dist < best_dist) {
                    best_ahead = static_cast<int>(i);
                    best_dist = dist;
                }
            }
        }
        if (best_ahead >= 0) {
            return best_ahead;
        }
        if (best_oldest >= 0) {
            return best_oldest;
        }
        return -1;
    }

    // On first packet for a peer, deliver the oldest buffered slot immediately.
    int oldest = -1;
    for (uint32_t i = 0; i < g_reorder_depth; ++i) {
        if (!pb->slots[i].used) {
            continue;
        }
        if (oldest < 0 || pb->slots[i].ts < pb->slots[oldest].ts) {
            oldest = static_cast<int>(i);
        }
    }
    return oldest;
}

int WSAAPI hooked_recvfrom(SOCKET s, char *buf, int len, int flags, sockaddr *from, int *fromlen) {
    if (g_real_recvfrom == nullptr) {
        WSASetLastError(WSANOTINITIALISED);
        return SOCKET_ERROR;
    }

    int rc;
    int wsa;
    for (;;) {
        rc = g_real_recvfrom(s, buf, len, flags, from, fromlen);
        wsa = static_cast<int>(WSAGetLastError());
        if (rc != static_cast<int>(sizeof(kWakeMagic)) || buf == nullptr
            || std::memcmp(buf, kWakeMagic, sizeof(kWakeMagic)) != 0) {
            break;
        }
        // Swallow internal reorder wake datagrams (see reorder_wake_thread).
        // A MSG_PEEK caller never consumed it: pull it off the queue for real
        // before retrying, or the peek loop would re-see it forever.
        if ((flags & MSG_PEEK) != 0) {
            char scratch[sizeof(kWakeMagic)];
            sockaddr_in scratch_src = {};
            int scratch_len = static_cast<int>(sizeof(scratch_src));
            g_real_recvfrom(s, scratch, static_cast<int>(sizeof(scratch)), 0,
                            reinterpret_cast<sockaddr *>(&scratch_src), &scratch_len);
        }
    }

    if (g_buffer_log_enabled) {
        uint32_t transferred = (rc == SOCKET_ERROR || rc < 0) ? 0u : static_cast<uint32_t>(rc);
        uint16_t payload_len = static_cast<uint16_t>((transferred < g_buffer_payload_bytes) ? transferred : g_buffer_payload_bytes);
        const uint8_t *payload = (payload_len > 0 && buf != nullptr) ? reinterpret_cast<const uint8_t *>(buf) : nullptr;
        buffer_log_event(kEventTypeRecvFrom,
                         s,
                         from,
                         static_cast<uint16_t>(flags),
                         (len > 0) ? static_cast<uint32_t>(len) : 0u,
                         transferred,
                         (rc == SOCKET_ERROR) ? static_cast<uint32_t>(wsa) : 0u,
                         payload,
                         payload_len);
    }

    WSASetLastError(wsa);
    return rc;
}

int WSAAPI hooked_WSARecvFrom(SOCKET s,
                              LPWSABUF buffers,
                              DWORD buffer_count,
                              LPDWORD bytes_received,
                              LPDWORD inout_flags,
                              sockaddr *from,
                              LPINT fromlen,
                              LPWSAOVERLAPPED overlapped,
                              LPWSAOVERLAPPED_COMPLETION_ROUTINE completion_routine) {
    if (g_real_wsarecvfrom == nullptr) {
        WSASetLastError(WSANOTINITIALISED);
        return SOCKET_ERROR;
    }

    // ── Bypass: overlapped/async path, reorder disabled, or bad arguments ────
    if (!g_reorder_enabled || !g_reorder_cs_ready
        || overlapped != nullptr || completion_routine != nullptr
        || buffers == nullptr || buffer_count == 0) {
        int rc = g_real_wsarecvfrom(s, buffers, buffer_count, bytes_received, inout_flags,
                                    from, fromlen, overlapped, completion_routine);
        int wsa = static_cast<int>(WSAGetLastError());
        if (g_buffer_log_enabled) {
            uint32_t requested = 0;
            for (DWORD i = 0; i < buffer_count && buffers != nullptr; ++i) {
                requested += buffers[i].len;
            }
            uint32_t transferred = (rc == 0 && bytes_received != nullptr) ? *bytes_received : 0u;
            uint16_t recv_flags  = (inout_flags != nullptr) ? static_cast<uint16_t>(*inout_flags & 0xffffUL) : 0;
            uint16_t payload_len = static_cast<uint16_t>((transferred < g_buffer_payload_bytes) ? transferred : g_buffer_payload_bytes);
            const uint8_t *payload = (payload_len > 0 && buffers != nullptr && buffers[0].buf != nullptr)
                                     ? reinterpret_cast<const uint8_t *>(buffers[0].buf) : nullptr;
            buffer_log_event(kEventTypeWSARecvFrom, s, from, recv_flags, requested, transferred,
                             (rc == SOCKET_ERROR) ? static_cast<uint32_t>(wsa) : 0u, payload, payload_len);
        }
        WSASetLastError(wsa);
        return rc;
    }

    // ── Reorder-buffered synchronous path ────────────────────────────────────
    //
    // Strategy: drain all available UDP datagrams from the socket into per-peer
    // priority queues, then return the best in-order candidate to the caller.
    // Packets that are still waiting for their predecessor are held up to
    // g_reorder_ms milliseconds before being forced out.

    uint8_t drain_buf[kReorderMaxPktBytes];
    WSABUF  drain_wsabuf;
    drain_wsabuf.buf = reinterpret_cast<char *>(drain_buf);
    drain_wsabuf.len = kReorderMaxPktBytes;

    EnterCriticalSection(&g_reorder_cs);

    // Tell the wake thread the game is actively polling the reorder socket.
    // Only polls of THAT socket count: a second UDP socket (lobby/discovery)
    // must neither retarget wakes nor suppress them.  g_reorder_sock itself
    // is assigned below, at the point a reorderable packet is buffered.
    if (s == g_reorder_sock) {
        g_last_recv_call_ms = GetTickCount64();
    }

    for (uint32_t di = 0; di < g_reorder_drain; ++di) {
        DWORD       drain_bytes  = 0;
        DWORD       drain_flags  = 0;
        sockaddr_in drain_src    = {};
        int         drain_srclen = static_cast<int>(sizeof(drain_src));

        int drc = g_real_wsarecvfrom(s, &drain_wsabuf, 1, &drain_bytes, &drain_flags,
                                     reinterpret_cast<sockaddr *>(&drain_src), &drain_srclen,
                                     nullptr, nullptr);
        if (drc != 0 || drain_bytes == 0) {
            break; // socket drained (WSAEWOULDBLOCK) or error
        }

        // Discard our own wake datagrams (see wake thread): they exist only
        // to mark the socket readable and must never reach the game.
        if (drain_bytes == sizeof(kWakeMagic)
            && std::memcmp(drain_buf, kWakeMagic, sizeof(kWakeMagic)) == 0) {
            continue;
        }

        // Packets too short for a sequence field, or from non-IPv4 sources,
        // cannot be reordered: deliver the first such packet immediately.
        if (drain_src.sin_family != AF_INET || drain_bytes < kReorderSeqMinPay) {
            uint32_t copied = scatter_copy(buffers, buffer_count, drain_buf, drain_bytes);
            if (bytes_received != nullptr) *bytes_received = copied;
            if (inout_flags != nullptr) *inout_flags = 0;
            if (from != nullptr && fromlen != nullptr) {
                int sa = (*fromlen < drain_srclen) ? *fromlen : drain_srclen;
                if (sa > 0) std::memcpy(from, &drain_src, static_cast<size_t>(sa));
                *fromlen = drain_srclen;
            }
            LeaveCriticalSection(&g_reorder_cs);
            if (g_buffer_log_enabled) {
                uint32_t requested = 0;
                for (DWORD i = 0; i < buffer_count; ++i) requested += buffers[i].len;
                uint16_t pay_len = static_cast<uint16_t>((copied < g_buffer_payload_bytes) ? copied : g_buffer_payload_bytes);
                const uint8_t *pay = (pay_len > 0 && buffers[0].buf != nullptr)
                                     ? reinterpret_cast<const uint8_t *>(buffers[0].buf) : nullptr;
                buffer_log_event(kEventTypeWSARecvFrom, s,
                                 reinterpret_cast<const sockaddr *>(&drain_src),
                                 0, requested, copied, 0u, pay, pay_len);
            }
            WSASetLastError(0);
            return 0;
        }

        uint32_t seq = 0;
        std::memcpy(&seq, drain_buf + kReorderSeqOffset, sizeof(seq));

        PeerBuf *pb = reorder_get_peer(drain_src);
        if (pb == nullptr) {
            // Peer table is full: deliver this packet immediately (fallback).
            uint32_t copied = scatter_copy(buffers, buffer_count, drain_buf, drain_bytes);
            if (bytes_received != nullptr) *bytes_received = copied;
            if (inout_flags != nullptr) *inout_flags = 0;
            if (from != nullptr && fromlen != nullptr) {
                int sa = (*fromlen < drain_srclen) ? *fromlen : drain_srclen;
                if (sa > 0) std::memcpy(from, &drain_src, static_cast<size_t>(sa));
                *fromlen = drain_srclen;
            }
            LeaveCriticalSection(&g_reorder_cs);
            if (g_buffer_log_enabled) {
                uint32_t requested = 0;
                for (DWORD i = 0; i < buffer_count; ++i) requested += buffers[i].len;
                uint16_t pay_len = static_cast<uint16_t>((copied < g_buffer_payload_bytes) ? copied : g_buffer_payload_bytes);
                const uint8_t *pay = (pay_len > 0 && buffers[0].buf != nullptr)
                                     ? reinterpret_cast<const uint8_t *>(buffers[0].buf) : nullptr;
                buffer_log_event(kEventTypeWSARecvFrom, s,
                                 reinterpret_cast<const sockaddr *>(&drain_src),
                                 0, requested, copied, 0u, pay, pay_len);
            }
            WSASetLastError(0);
            return 0;
        }

        uint64_t arrival_ms = GetTickCount64();
        reorder_adapt_on_arrival(pb, seq, arrival_ms);
        reorder_insert(pb, seq, arrival_ms, drain_src, drain_buf, drain_bytes);
        // This socket demonstrably carries reorderable traffic: it is the one
        // the wake thread should target, and its polls reset the wake budget.
        g_reorder_sock = s;
        g_last_recv_call_ms = arrival_ms;
    }

    // Scan the peer table for the first packet that is ready to deliver.
    uint64_t now_ms = GetTickCount64();
    int best_pi = -1;
    int best_si = -1;
    for (uint32_t pi = 0; pi < g_reorder_peers; ++pi) {
        if (g_peers[pi].key == 0) {
            continue;
        }
        int si = reorder_pick(&g_peers[pi], now_ms);
        if (si >= 0) {
            best_pi = pi;
            best_si = si;
            break;
        }
    }

    if (best_pi < 0) {
        // Nothing is ready yet: tell the game the socket is empty for now.
        LeaveCriticalSection(&g_reorder_cs);
        WSASetLastError(WSAEWOULDBLOCK);
        return SOCKET_ERROR;
    }

    // Deliver the chosen packet to the caller.
    PeerBuf     *pb  = &g_peers[best_pi];
    ReorderSlot *pkt = &pb->slots[best_si];

    uint32_t    delivered      = scatter_copy(buffers, buffer_count, pkt->data, pkt->len);
    sockaddr_in deliver_from   = pkt->from;

    if (bytes_received != nullptr) *bytes_received = delivered;
    if (inout_flags != nullptr) *inout_flags = 0;
    if (from != nullptr && fromlen != nullptr) {
        int sa = (*fromlen < static_cast<int>(sizeof(pkt->from)))
                 ? *fromlen : static_cast<int>(sizeof(pkt->from));
        if (sa > 0) std::memcpy(from, &pkt->from, static_cast<size_t>(sa));
        *fromlen = static_cast<int>(sizeof(pkt->from));
    }

    pb->last_seq = pkt->seq;
    pb->seq_init = 1;
    pkt->used    = 0;
    if (pb->filled > 0) --pb->filled;
    reorder_decay(pb, now_ms);

    LeaveCriticalSection(&g_reorder_cs);

    if (g_buffer_log_enabled) {
        uint32_t requested = 0;
        for (DWORD i = 0; i < buffer_count; ++i) requested += buffers[i].len;
        uint16_t pay_len = static_cast<uint16_t>((delivered < g_buffer_payload_bytes) ? delivered : g_buffer_payload_bytes);
        const uint8_t *pay = (pay_len > 0 && buffers != nullptr && buffers[0].buf != nullptr)
                             ? reinterpret_cast<const uint8_t *>(buffers[0].buf) : nullptr;
        buffer_log_event(kEventTypeWSARecvFrom, s,
                         reinterpret_cast<const sockaddr *>(&deliver_from),
                         0, requested, delivered, 0u, pay, pay_len);
    }

    WSASetLastError(0);
    return 0;
}

int WSAAPI hooked_ioctlsocket(SOCKET s, long cmd, u_long *argp) {
    if (g_real_ioctlsocket == nullptr) {
        WSASetLastError(WSANOTINITIALISED);
        return SOCKET_ERROR;
    }

    int rc = g_real_ioctlsocket(s, cmd, argp);
    int wsa = static_cast<int>(WSAGetLastError());

    if (g_buffer_log_enabled && cmd == static_cast<long>(FIONBIO)) {
        uint32_t mode = (argp != nullptr) ? static_cast<uint32_t>(*argp) : 0u;
        uint16_t flags = static_cast<uint16_t>((mode & 1u) ? 1u : 0u);
        buffer_log_event(kEventTypeIoctlSocket,
                         s,
                         nullptr,
                         flags,
                         static_cast<uint32_t>(cmd),
                         mode,
                         (rc == SOCKET_ERROR) ? static_cast<uint32_t>(wsa) : 0u,
                         nullptr,
                         0);
    }

    WSASetLastError(wsa);
    return rc;
}

int WSAAPI hooked_WSAIoctl(SOCKET s,
                           DWORD control_code,
                           LPVOID in_buffer,
                           DWORD in_buffer_len,
                           LPVOID out_buffer,
                           DWORD out_buffer_len,
                           LPDWORD bytes_returned,
                           LPWSAOVERLAPPED overlapped,
                           LPWSAOVERLAPPED_COMPLETION_ROUTINE completion_routine) {
    if (g_real_wsaioclt == nullptr) {
        WSASetLastError(WSANOTINITIALISED);
        return SOCKET_ERROR;
    }

    int rc = g_real_wsaioclt(s,
                             control_code,
                             in_buffer,
                             in_buffer_len,
                             out_buffer,
                             out_buffer_len,
                             bytes_returned,
                             overlapped,
                             completion_routine);
    int wsa = static_cast<int>(WSAGetLastError());

    if (g_buffer_log_enabled && control_code == static_cast<DWORD>(FIONBIO)) {
        uint32_t mode = 0;
        if (in_buffer != nullptr && in_buffer_len >= sizeof(u_long)) {
            mode = static_cast<uint32_t>(*reinterpret_cast<u_long *>(in_buffer));
        }
        uint16_t flags = static_cast<uint16_t>((mode & 1u) ? 1u : 0u);
        buffer_log_event(kEventTypeWSAIoctl,
                         s,
                         nullptr,
                         flags,
                         control_code,
                         mode,
                         (rc == SOCKET_ERROR) ? static_cast<uint32_t>(wsa) : 0u,
                         nullptr,
                         0);
    }

    WSASetLastError(wsa);
    return rc;
}

int WSAAPI hooked_sendto(SOCKET s, const char *buf, int len, int flags, const sockaddr *to, int tolen) {
    if (g_real_sendto == nullptr) {
        WSASetLastError(WSANOTINITIALISED);
        return SOCKET_ERROR;
    }

    int rc = g_real_sendto(s, buf, len, flags, to, tolen);

    // Duplicate only IPv4 datagrams large enough to carry a BZRNet sequence
    // field: control/wake packets stay single-shot.  The first call's result
    // and error state are what the game sees.
    if (g_send_dup && rc >= 0 && buf != nullptr && to != nullptr
        && to->sa_family == AF_INET && len >= static_cast<int>(kReorderSeqMinPay)
        && !dup_is_loopback(to)) {
        int wsa = static_cast<int>(WSAGetLastError());
        if (g_dup_delay_ms == 0) {
            g_real_sendto(s, buf, len, flags, to, tolen);
        } else {
            dup_enqueue(s, reinterpret_cast<const uint8_t *>(buf),
                        static_cast<uint32_t>(len), to, tolen);
        }
        WSASetLastError(wsa);
    }

    return rc;
}

int WSAAPI hooked_WSASendTo(SOCKET s,
                            LPWSABUF buffers,
                            DWORD buffer_count,
                            LPDWORD bytes_sent,
                            DWORD flags,
                            const sockaddr *to,
                            int tolen,
                            LPWSAOVERLAPPED overlapped,
                            LPWSAOVERLAPPED_COMPLETION_ROUTINE completion_routine) {
    if (g_real_wsasendto == nullptr) {
        WSASetLastError(WSANOTINITIALISED);
        return SOCKET_ERROR;
    }

    int rc = g_real_wsasendto(s, buffers, buffer_count, bytes_sent, flags, to, tolen, overlapped, completion_routine);
    int wsa = static_cast<int>(WSAGetLastError());

    // Duplicate only IPv4 datagrams large enough to carry a BZRNet sequence
    // field.  The duplicate is a separate synchronous send from a flat copy,
    // which keeps it safe for overlapped originals too: the caller's buffers
    // are only guaranteed valid for the duration of this call.
    if (g_send_dup && to != nullptr && to->sa_family == AF_INET
        && !dup_is_loopback(to)
        && buffers != nullptr && buffer_count > 0
        && (rc == 0 || (rc == SOCKET_ERROR && wsa == WSA_IO_PENDING))) {
        uint8_t flat[kReorderMaxPktBytes];
        uint32_t total = 0;
        bool fits = true;
        for (DWORD i = 0; i < buffer_count; ++i) {
            if (buffers[i].buf == nullptr || buffers[i].len == 0) {
                continue;
            }
            if (total + buffers[i].len > kReorderMaxPktBytes) {
                fits = false;
                break;
            }
            std::memcpy(flat + total, buffers[i].buf, buffers[i].len);
            total += buffers[i].len;
        }
        if (fits && total >= kReorderSeqMinPay) {
            if (g_dup_delay_ms == 0) {
                WSABUF dup_buf;
                dup_buf.buf = reinterpret_cast<char *>(flat);
                dup_buf.len = static_cast<u_long>(total);
                DWORD dup_sent = 0;
                g_real_wsasendto(s, &dup_buf, 1, &dup_sent, 0, to, tolen, nullptr, nullptr);
            } else {
                dup_enqueue(s, flat, total, to, tolen);
            }
        }
    }

    WSASetLastError(wsa);
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

    if (_stricmp(proc_name, "recvfrom") == 0 || std::strcmp(proc_name, "_recvfrom@24") == 0) {
        g_real_recvfrom = reinterpret_cast<RecvFromFn>(real);
        log_line("hooked_GetProcAddress: redirecting %s real=%p hook=%p", proc_name, reinterpret_cast<void *>(real), reinterpret_cast<void *>(&hooked_recvfrom));
        return reinterpret_cast<FARPROC>(&hooked_recvfrom);
    }

    if (_stricmp(proc_name, "WSARecvFrom") == 0 || std::strcmp(proc_name, "_WSARecvFrom@36") == 0) {
        g_real_wsarecvfrom = reinterpret_cast<WSARecvFromFn>(real);
        log_line("hooked_GetProcAddress: redirecting %s real=%p hook=%p", proc_name, reinterpret_cast<void *>(real), reinterpret_cast<void *>(&hooked_WSARecvFrom));
        return reinterpret_cast<FARPROC>(&hooked_WSARecvFrom);
    }

    if (_stricmp(proc_name, "ioctlsocket") == 0 || std::strcmp(proc_name, "_ioctlsocket@12") == 0) {
        g_real_ioctlsocket = reinterpret_cast<IoctlSocketFn>(real);
        log_line("hooked_GetProcAddress: redirecting %s real=%p hook=%p", proc_name, reinterpret_cast<void *>(real), reinterpret_cast<void *>(&hooked_ioctlsocket));
        return reinterpret_cast<FARPROC>(&hooked_ioctlsocket);
    }

    if (_stricmp(proc_name, "WSAIoctl") == 0 || std::strcmp(proc_name, "_WSAIoctl@36") == 0) {
        g_real_wsaioclt = reinterpret_cast<WSAIoctlFn>(real);
        log_line("hooked_GetProcAddress: redirecting %s real=%p hook=%p", proc_name, reinterpret_cast<void *>(real), reinterpret_cast<void *>(&hooked_WSAIoctl));
        return reinterpret_cast<FARPROC>(&hooked_WSAIoctl);
    }

    if (_stricmp(proc_name, "sendto") == 0 || std::strcmp(proc_name, "_sendto@24") == 0) {
        g_real_sendto = reinterpret_cast<SendToFn>(real);
        log_line("hooked_GetProcAddress: redirecting %s real=%p hook=%p", proc_name, reinterpret_cast<void *>(real), reinterpret_cast<void *>(&hooked_sendto));
        return reinterpret_cast<FARPROC>(&hooked_sendto);
    }

    if (_stricmp(proc_name, "WSASendTo") == 0 || std::strcmp(proc_name, "_WSASendTo@36") == 0) {
        g_real_wsasendto = reinterpret_cast<WSASendToFn>(real);
        log_line("hooked_GetProcAddress: redirecting %s real=%p hook=%p", proc_name, reinterpret_cast<void *>(real), reinterpret_cast<void *>(&hooked_WSASendTo));
        return reinterpret_cast<FARPROC>(&hooked_WSASendTo);
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
    if (base == nullptr || (g_real_setsockopt == nullptr && g_real_wsasetsocketoption == nullptr && g_real_getsockopt == nullptr && g_real_wsagetsocketoption == nullptr && g_real_socket == nullptr && g_real_wsasocketw == nullptr && g_real_closesocket == nullptr && g_real_recvfrom == nullptr && g_real_wsarecvfrom == nullptr && g_real_ioctlsocket == nullptr && g_real_wsaioclt == nullptr && g_real_sendto == nullptr && g_real_wsasendto == nullptr)) {
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
            if (g_real_recvfrom != nullptr && *slot == reinterpret_cast<void *>(g_real_recvfrom)) {
                if (patch_iat_slot(slot, reinterpret_cast<void *>(&hooked_recvfrom))) {
                    ++patched_count;
                }
            }
            if (g_real_wsarecvfrom != nullptr && *slot == reinterpret_cast<void *>(g_real_wsarecvfrom)) {
                if (patch_iat_slot(slot, reinterpret_cast<void *>(&hooked_WSARecvFrom))) {
                    ++patched_count;
                }
            }
            if (g_real_ioctlsocket != nullptr && *slot == reinterpret_cast<void *>(g_real_ioctlsocket)) {
                if (patch_iat_slot(slot, reinterpret_cast<void *>(&hooked_ioctlsocket))) {
                    ++patched_count;
                }
            }
            if (g_real_wsaioclt != nullptr && *slot == reinterpret_cast<void *>(g_real_wsaioclt)) {
                if (patch_iat_slot(slot, reinterpret_cast<void *>(&hooked_WSAIoctl))) {
                    ++patched_count;
                }
            }
            if (g_real_sendto != nullptr && *slot == reinterpret_cast<void *>(g_real_sendto)) {
                if (patch_iat_slot(slot, reinterpret_cast<void *>(&hooked_sendto))) {
                    ++patched_count;
                }
            }
            if (g_real_wsasendto != nullptr && *slot == reinterpret_cast<void *>(g_real_wsasendto)) {
                if (patch_iat_slot(slot, reinterpret_cast<void *>(&hooked_WSASendTo))) {
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
    FARPROC real_recvfrom = getproc(ws2, "recvfrom");
    FARPROC real_wsarecvfrom = getproc(ws2, "WSARecvFrom");
    FARPROC real_ioctlsocket = getproc(ws2, "ioctlsocket");
    FARPROC real_wsaioclt = getproc(ws2, "WSAIoctl");
    // sendto is also used by the reorder wake thread.
    FARPROC real_sendto = getproc(ws2, "sendto");
    FARPROC real_wsasendto = getproc(ws2, "WSASendTo");
    FARPROC real_getsockname = getproc(ws2, "getsockname");
    if (real_setsockopt == nullptr && real_wsaset == nullptr && real_getsockopt == nullptr && real_wsaget == nullptr && real_socket == nullptr && real_wsasocketw == nullptr && real_closesocket == nullptr && real_recvfrom == nullptr && real_wsarecvfrom == nullptr && real_ioctlsocket == nullptr && real_wsaioclt == nullptr) {
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
    if (real_recvfrom != nullptr && g_real_recvfrom == nullptr) {
        g_real_recvfrom = reinterpret_cast<RecvFromFn>(real_recvfrom);
    }
    if (real_wsarecvfrom != nullptr && g_real_wsarecvfrom == nullptr) {
        g_real_wsarecvfrom = reinterpret_cast<WSARecvFromFn>(real_wsarecvfrom);
    }
    if (real_ioctlsocket != nullptr && g_real_ioctlsocket == nullptr) {
        g_real_ioctlsocket = reinterpret_cast<IoctlSocketFn>(real_ioctlsocket);
    }
    if (real_wsaioclt != nullptr && g_real_wsaioclt == nullptr) {
        g_real_wsaioclt = reinterpret_cast<WSAIoctlFn>(real_wsaioclt);
    }
    if (real_sendto != nullptr && g_real_sendto == nullptr) {
        g_real_sendto = reinterpret_cast<SendToFn>(real_sendto);
    }
    if (real_wsasendto != nullptr && g_real_wsasendto == nullptr) {
        g_real_wsasendto = reinterpret_cast<WSASendToFn>(real_wsasendto);
    }
    if (real_getsockname != nullptr && g_real_getsockname == nullptr) {
        g_real_getsockname = reinterpret_cast<GetSockNameFn>(real_getsockname);
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
    if (!g_logged_real_recvfrom && g_real_recvfrom != nullptr) {
        g_logged_real_recvfrom = true;
        log_line("hook_setsockopt_iat: real recvfrom=%p", reinterpret_cast<void *>(g_real_recvfrom));
    }
    if (!g_logged_real_wsarecvfrom && g_real_wsarecvfrom != nullptr) {
        g_logged_real_wsarecvfrom = true;
        log_line("hook_setsockopt_iat: real WSARecvFrom=%p", reinterpret_cast<void *>(g_real_wsarecvfrom));
    }
    if (!g_logged_real_ioctlsocket && g_real_ioctlsocket != nullptr) {
        g_logged_real_ioctlsocket = true;
        log_line("hook_setsockopt_iat: real ioctlsocket=%p", reinterpret_cast<void *>(g_real_ioctlsocket));
    }
    if (!g_logged_real_wsaioctl && g_real_wsaioclt != nullptr) {
        g_logged_real_wsaioctl = true;
        log_line("hook_setsockopt_iat: real WSAIoctl=%p", reinterpret_cast<void *>(g_real_wsaioclt));
    }
    if (!g_logged_real_sendto && g_real_sendto != nullptr) {
        g_logged_real_sendto = true;
        log_line("hook_setsockopt_iat: real sendto=%p", reinterpret_cast<void *>(g_real_sendto));
    }
    if (!g_logged_real_wsasendto && g_real_wsasendto != nullptr) {
        g_logged_real_wsasendto = true;
        log_line("hook_setsockopt_iat: real WSASendTo=%p", reinterpret_cast<void *>(g_real_wsasendto));
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

// Locate a named PE section in the main module.  Returns false if not found.
// The PE headers and (post-DRM-decryption) section bodies are mapped/readable.
static bool find_section(const char *want, BYTE **out_start, size_t *out_size) {
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
    auto *sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char name[9] = {0};
        std::memcpy(name, sec->Name, 8);
        if (std::strncmp(name, want, 8) == 0) {
            *out_start = base + sec->VirtualAddress;
            *out_size = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// Opt-in: scan decrypted .text for the 32-bit governor start constant and log
// candidates with context.  Read-only.  See g_gov_scan.
DWORD WINAPI governor_scan_thread(LPVOID) {
    if (!is_target_main_module()) {
        return 0;
    }
    // Let SteamStub decrypt .text and the game reach the menu first.
    Sleep(15000);

    BYTE *text = nullptr;
    size_t text_size = 0;
    if (!find_section(".text", &text, &text_size)) {
        log_line("governor_scan: .text section not found");
        return 0;
    }
    log_line("governor_scan: scanning .text base=%p size=%zu for 0x00000FA0 (4000)",
             reinterpret_cast<void *>(text), text_size);

    const uint8_t pat[4] = {0xA0, 0x0F, 0x00, 0x00};   // 4000, little-endian
    int hits = 0;
    const int kMaxHits = 48;
    // Guard the tail so the 4-byte compare never reads past the section.
    for (size_t i = 0; i + sizeof(pat) <= text_size && hits < kMaxHits; ++i) {
        if (std::memcmp(text + i, pat, sizeof(pat)) != 0) {
            continue;
        }
        // Log the opcode byte before the immediate (mov=0xB8-0xBF, push=0x68,
        // cmp/mov-mem variants) plus 12 bytes of context to identify the site.
        BYTE *site = text + i;
        size_t ctx_lo = (i >= 3) ? 3 : i;
        char ctx[64] = {0};
        int p = 0;
        for (size_t k = i - ctx_lo; k < i + 8 && k < text_size && p < 60; ++k) {
            p += std::snprintf(ctx + p, sizeof(ctx) - p, "%02x ", text[k]);
        }
        uintptr_t va = reinterpret_cast<uintptr_t>(site);
        log_line("governor_scan: hit #%d va=0x%08lx  bytes[ %s]",
                 hits + 1, static_cast<unsigned long>(va), ctx);
        hits++;
    }
    log_line("governor_scan: done, %d candidate site(s)%s. Report these to build "
             "the runtime governor patch.", hits,
             (hits >= kMaxHits) ? " (capped)" : "");
    return 0;
}

// Raise the governor's 4000 B/s cold start to g_gov_start.
//
// The obvious approach — rewriting the 4000 immediate in .text — was verified
// to apply cleanly (4000 -> 16000, correct unique site) but SteamStub's
// runtime code-integrity check then kills the process.  So we do NOT touch
// .text.  Instead we watch the governor's live send-rate DATA global
// (kGovRateAddr) and, whenever it reads the hardcoded cold-start sentinel
// 4000 (which only occurs at the instant a match's governor is set up — the
// ramp and the MinBandwidth floor move it off 4000 immediately and never
// return to exactly 4000), overwrite it with g_gov_start.  A 32-bit aligned
// store is atomic on x86, and .data carries no integrity check, so the DRM is
// untouched.  We still signature-scan .text once (read-only) to confirm the
// game version, which is what ties the fixed data address and the 4000
// sentinel to code we recognise; if the signature doesn't match uniquely we
// do nothing.
//
// Address of the live send-rate global, captured from the decrypted image
// (fixed base 0x400000, no ASLR).  Logged by the game as
// "Net: Bandwidth usage now set to %d".
static uint32_t *const kGovRateAddr = reinterpret_cast<uint32_t *>(0x008e8d14);
constexpr uint32_t      kGovColdStart = 4000;

DWORD WINAPI governor_patch_thread(LPVOID) {
    if (!is_target_main_module() || g_gov_start == 0) {
        return 0;
    }
    // Let SteamStub decrypt .text first (well before any match starts).
    Sleep(15000);

    BYTE *text = nullptr;
    size_t text_size = 0;
    if (!find_section(".text", &text, &text_size)) {
        log_line("governor_patch: .text section not found");
        return 0;
    }

    int matches = 0;
    for (size_t i = 0; i + sizeof(kGovSig) <= text_size; ++i) {
        if (std::memcmp(text + i, kGovSig, sizeof(kGovSig)) == 0) {
            if (++matches > 1) break;
        }
    }
    if (matches != 1) {
        log_line("governor_patch: %d governor signature matches (need exactly 1) - "
                 "disabled. Game version may have changed; re-run BZ_GOV_SCAN.", matches);
        return 0;
    }

    log_line("governor_patch: version confirmed; watching send-rate 0x%08lx, "
             "cold-start %u -> %u (data-only, no .text write)",
             static_cast<unsigned long>(reinterpret_cast<uintptr_t>(kGovRateAddr)),
             static_cast<unsigned>(kGovColdStart), static_cast<unsigned>(g_gov_start));

    unsigned long bumps = 0;
    while (InterlockedCompareExchange(&g_gov_stop, 0, 0) == 0) {
        // Aligned 32-bit read/write: torn access is impossible on x86.
        if (*kGovRateAddr == kGovColdStart) {
            *kGovRateAddr = g_gov_start;
            if (bumps == 0) {
                log_line("governor_patch: cold-start caught, send-rate %u -> %u "
                         "(match started)", static_cast<unsigned>(kGovColdStart),
                         static_cast<unsigned>(g_gov_start));
            }
            ++bumps;
        }
        Sleep(kGovPollMs);
    }
    log_line("governor_patch: stopping after %lu cold-start bump(s)", bumps);
    return 0;
}

// Wake thread – prevents held packets from stranding.
// While the reorder queue holds packets and the game is not actively polling
// WSARecvFrom, nudge the (drained) socket readable again by sending a small
// magic datagram to its own bound address.  The game's select()/event wait
// fires, it calls WSARecvFrom, our hook discards the magic packet and
// releases any packet whose hold window has expired.
DWORD WINAPI reorder_wake_thread(LPVOID) {
    uint64_t seen_call = 0;   // last g_last_recv_call_ms we acted on
    uint32_t burst     = 0;   // wakes sent since the game last polled

    while (InterlockedCompareExchange(&g_wake_stop, 0, 0) == 0) {
        Sleep(kReorderWakeTickMs);

        if (!g_reorder_enabled || !g_reorder_cs_ready
            || g_real_getsockname == nullptr || g_real_sendto == nullptr || g_real_socket == nullptr) {
            continue;
        }

        SOCKET   target    = INVALID_SOCKET;
        bool     held      = false;
        uint64_t last_call = 0;

        EnterCriticalSection(&g_reorder_cs);
        for (uint32_t i = 0; i < g_reorder_peers; ++i) {
            if (g_peers[i].key != 0 && g_peers[i].filled > 0) {
                held = true;
                break;
            }
        }
        target = g_reorder_sock;
        last_call = g_last_recv_call_ms;
        LeaveCriticalSection(&g_reorder_cs);

        if (!held || target == INVALID_SOCKET) {
            continue;
        }

        // Game is polling on its own: no need to wake it.
        uint64_t now = GetTickCount64();
        if (now - last_call < kReorderWakeIdleMs) {
            continue;
        }

        // Burst cap: a game poll resets the budget.  If several nudges in a
        // row produced no poll, the game is not sleeping in select() — it is
        // paused (level load, alt-tab).  Piling more datagrams into the
        // receive buffer would only crowd out real packets.
        if (last_call != seen_call) {
            seen_call = last_call;
            burst = 0;
        }
        if (burst >= kReorderWakeBurstCap) {
            continue;
        }

        sockaddr_in bound = {};
        int bound_len = static_cast<int>(sizeof(bound));
        if (g_real_getsockname(target, reinterpret_cast<sockaddr *>(&bound), &bound_len) != 0
            || bound.sin_family != AF_INET || bound.sin_port == 0) {
            continue; // unbound or already closed
        }
        if (bound.sin_addr.S_un.S_addr == htonl(INADDR_ANY)) {
            bound.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
        }

        if (g_wake_sender == INVALID_SOCKET) {
            g_wake_sender = g_real_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (g_wake_sender == INVALID_SOCKET) {
                continue;
            }
        }

        g_real_sendto(g_wake_sender,
                      reinterpret_cast<const char *>(kWakeMagic),
                      static_cast<int>(sizeof(kWakeMagic)), 0,
                      reinterpret_cast<const sockaddr *>(&bound),
                      static_cast<int>(sizeof(bound)));
        ++burst;

        if (!g_wake_logged) {
            g_wake_logged = true;
            log_line("reorder: wake helper active (held packets, idle game poll)");
        }
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
        if (!g_buffer_lock_ready) {
            InitializeCriticalSection(&g_buffer_lock);
            g_buffer_lock_ready = true;
        }
        if (!g_reorder_cs_ready) {
            InitializeCriticalSection(&g_reorder_cs);
            g_reorder_cs_ready = true;
        }
        if (!g_dup_cs_ready) {
            InitializeCriticalSection(&g_dup_cs);
            g_dup_cs_ready = true;
        }
        init_buffer_log_if_needed();
        {
            // Reorder enabled by default; BZ_REORDER=0 disables.
            const char *reorder_env = std::getenv("BZ_REORDER");
            g_reorder_enabled = (reorder_env == nullptr || *reorder_env == '\0')
                                ? true : env_truthy(reorder_env);
            const char *adapt_env = std::getenv("BZ_REORDER_ADAPT");
            g_reorder_adapt = (adapt_env == nullptr || *adapt_env == '\0')
                              ? true : env_truthy(adapt_env);
            const char *wake_env = std::getenv("BZ_REORDER_WAKE");
            g_wake_enabled = (wake_env == nullptr || *wake_env == '\0')
                             ? true : env_truthy(wake_env);
            g_reorder_ms = clamp_u32(parse_env_u32("BZ_REORDER_WINDOW_MS", kReorderDefaultMs), 5, 200);
            g_reorder_min_ms = clamp_u32(parse_env_u32("BZ_REORDER_MIN_MS", kReorderMinMsDef), 0, g_reorder_ms);
            g_reorder_depth = clamp_u32(parse_env_u32("BZ_REORDER_DEPTH", kReorderSlotCap), 1, kReorderSlotCap);
            g_reorder_peers = clamp_u32(parse_env_u32("BZ_REORDER_PEERS", kReorderPeerCap), 1, kReorderPeerCap);
            g_reorder_drain = clamp_u32(parse_env_u32("BZ_REORDER_DRAIN", kReorderDrainCapDef), 1, kReorderDrainCapMax);
            // Off by default: adds upstream traffic on the P2P socket.
            g_send_dup = env_truthy(std::getenv("BZ_SEND_DUP"));
            g_dup_delay_ms = clamp_u32(parse_env_u32("BZ_DUP_DELAY_MS", kDupDelayMsDef), 0, 500);
            g_dup_max_pps  = clamp_u32(parse_env_u32("BZ_DUP_MAX_PPS", kDupMaxPpsDef), 0, 2000);
            // DSCP class for the P2P socket (0 disables); clamp to the 6-bit field.
            g_dscp = clamp_u32(parse_env_u32("BZ_DSCP", kDscpDefault), 0, 63);
            g_gov_scan = env_truthy(std::getenv("BZ_GOV_SCAN"));
            // Governor cold-start rate (0 = disabled). Clamp to a sane band.
            g_gov_start = clamp_u32(parse_env_u32("BZ_GOV_START", 0), 0, 200000);
        }
        log_line("DllMain: DLL_PROCESS_ATTACH");
        log_line("governor_patch: %s (BZ_GOV_START=%u; 0=disabled)",
                 g_gov_start ? "enabled" : "disabled", static_cast<unsigned>(g_gov_start));
        log_line("send_dup: %s dup_delay_ms=%u dup_max_pps=%u loopback_dup=skip dscp=%u"
                 " (BZ_SEND_DUP=1 to enable outbound packet duplication)",
                 g_send_dup ? "enabled" : "disabled",
                 static_cast<unsigned>(g_dup_delay_ms),
                 static_cast<unsigned>(g_dup_max_pps),
                 static_cast<unsigned>(g_dscp));
        log_line("reorder: %s max_window_ms=%u min_window_ms=%u adapt=%d wake=%d depth=%u peers=%u drain=%u seq_offset=%u",
                 g_reorder_enabled ? "enabled" : "DISABLED",
                 static_cast<unsigned>(g_reorder_ms),
                 static_cast<unsigned>(g_reorder_min_ms),
                 g_reorder_adapt ? 1 : 0,
                 g_wake_enabled ? 1 : 0,
                 static_cast<unsigned>(g_reorder_depth),
                 static_cast<unsigned>(g_reorder_peers),
                 static_cast<unsigned>(g_reorder_drain),
                 static_cast<unsigned>(kReorderSeqOffset));
        if (!hook_setsockopt_iat()) {
            log_line("DllMain: initial hook install attempt failed");
        }
        HANDLE thread = CreateThread(nullptr, 0, &hook_worker_thread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        } else {
            log_line("DllMain: failed to create hook worker thread gle=%lu", static_cast<unsigned long>(GetLastError()));
        }
        if (g_reorder_enabled && g_wake_enabled && is_target_main_module()) {
            HANDLE wake = CreateThread(nullptr, 0, &reorder_wake_thread, nullptr, 0, nullptr);
            if (wake != nullptr) {
                CloseHandle(wake);
            } else {
                log_line("DllMain: failed to create reorder wake thread gle=%lu", static_cast<unsigned long>(GetLastError()));
            }
        }
        if (g_gov_start != 0 && is_target_main_module()) {
            HANDLE govp = CreateThread(nullptr, 0, &governor_patch_thread, nullptr, 0, nullptr);
            if (govp != nullptr) {
                CloseHandle(govp);
            } else {
                log_line("DllMain: failed to create governor patch thread gle=%lu", static_cast<unsigned long>(GetLastError()));
            }
        }
        if (g_gov_scan && is_target_main_module()) {
            HANDLE gov = CreateThread(nullptr, 0, &governor_scan_thread, nullptr, 0, nullptr);
            if (gov != nullptr) {
                CloseHandle(gov);
            } else {
                log_line("DllMain: failed to create governor scan thread gle=%lu", static_cast<unsigned long>(GetLastError()));
            }
        }
        if (g_send_dup && g_dup_delay_ms > 0 && is_target_main_module()) {
            HANDLE pacer = CreateThread(nullptr, 0, &dup_pacer_thread, nullptr, 0, nullptr);
            if (pacer != nullptr) {
                CloseHandle(pacer);
            } else {
                log_line("DllMain: failed to create dup pacer thread gle=%lu"
                         " - falling back to back-to-back duplicates",
                         static_cast<unsigned long>(GetLastError()));
                g_dup_delay_ms = 0;
            }
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        // Signal the wake thread to exit.  Do not wait on it here: this runs
        // under the loader lock and joining a thread would deadlock.
        InterlockedExchange(&g_wake_stop, 1);
        InterlockedExchange(&g_dup_stop, 1);
        InterlockedExchange(&g_gov_stop, 1);
        if (g_wake_sender != INVALID_SOCKET && g_real_closesocket != nullptr) {
            g_real_closesocket(g_wake_sender);
            g_wake_sender = INVALID_SOCKET;
        }
        flush_buffer_log_files();
        if (g_buffer_ring != nullptr) {
            HeapFree(GetProcessHeap(), 0, g_buffer_ring);
            g_buffer_ring = nullptr;
        }
        if (g_buffer_lock_ready) {
            DeleteCriticalSection(&g_buffer_lock);
            g_buffer_lock_ready = false;
        }
        if (g_track_lock_ready) {
            DeleteCriticalSection(&g_track_lock);
            g_track_lock_ready = false;
        }
        if (g_reorder_cs_ready) {
            DeleteCriticalSection(&g_reorder_cs);
            g_reorder_cs_ready = false;
        }
        if (g_dup_cs_ready) {
            DeleteCriticalSection(&g_dup_cs);
            g_dup_cs_ready = false;
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