// netcode_hooks.cpp
// Battlezone 98 Redux - Windows netcode patch
//
// Strategy:
//   Walk the game EXE's Import Address Table (IAT) and replace the
//   WS2_32.dll!WSASocketW pointer with our own hook. When the game
//   calls WSASocketW to open its P2P UDP socket, we call the real
//   function and then enlarge SO_SNDBUF / SO_RCVBUF on the resulting
//   socket handle. Readback getsockopt values are written to the log
//   so testers can confirm the patch is working.
//
// Target values (match the Linux dsound proxy):
//   SO_SNDBUF = 524288   (512 KB)
//   SO_RCVBUF = 4194304  (  4 MB)

#include "netcode_hooks.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cwchar>

// Provided by dllmain.cpp
extern void ProxyLog(const char* fmt, ...);

// ---------------------------------------------------------
// Constants
// ---------------------------------------------------------
static const int kTargetSndBuf = 524288;   // 512 KB
static const int kTargetRcvBuf = 4194304;  //   4 MB

// DSCP class for the game P2P socket.  46 == Expedited Forwarding: routers
// with WMM (WiFi voice queue) or SQM/fq_codel serve these ahead of bulk
// traffic, which directly targets the queueing-delay mechanism behind the
// stale-drop bursts we measured.  On Proton, Wine forwards IP_TOS to the
// Linux socket and the kernel honours it; on stock Windows setsockopt(IP_TOS)
// is silently ignored (needs qWAVE), so this is a safe no-op there.
// BZ_DSCP overrides the class; BZ_DSCP=0 disables the marking entirely.
static const uint32_t kDscpDefault = 46;   // EF

constexpr wchar_t kBufferBinName[] = L"bz_buffer_log.bin";
constexpr wchar_t kBufferMetaName[] = L"bz_buffer_log.meta.txt";
constexpr uint32_t kBufferLogVersion = 1;
constexpr uint32_t kBufferLogMagic = 0x474c5a42; // 'BZLG'
constexpr uint32_t kEventTypeWSARecvFrom = 2;
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

// ---------------------------------------------------------
// Real-function pointers (resolved from ws2_32.dll)
// ---------------------------------------------------------
typedef SOCKET (WSAAPI* PFN_WSASocketW)(
    int af, int type, int protocol,
    LPWSAPROTOCOL_INFOW lpProtocolInfo,
    GROUP g, DWORD dwFlags);

typedef int (WSAAPI* PFN_setsockopt)(SOCKET s, int level, int optname,
    const char* optval, int optlen);
typedef int (WSAAPI* PFN_getsockopt)(SOCKET s, int level, int optname,
    char* optval, int* optlen);
typedef int (WSAAPI* PFN_WSARecvFrom)(
    SOCKET s, LPWSABUF buffers, DWORD buffer_count,
    LPDWORD bytes_received, LPDWORD inout_flags,
    struct sockaddr *from, LPINT fromlen, LPWSAOVERLAPPED ov,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE cr);
typedef int (WSAAPI* PFN_closesocket)(SOCKET s);
typedef SOCKET (WSAAPI* PFN_socket)(int af, int type, int protocol);
typedef int (WSAAPI* PFN_sendto)(SOCKET s, const char* buf, int len, int flags,
    const struct sockaddr* to, int tolen);
typedef int (WSAAPI* PFN_WSASendTo)(
    SOCKET s, LPWSABUF buffers, DWORD buffer_count,
    LPDWORD bytes_sent, DWORD flags,
    const struct sockaddr* to, int tolen, LPWSAOVERLAPPED ov,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE cr);
typedef int (WSAAPI* PFN_getsockname)(SOCKET s, struct sockaddr* name, int* namelen);

static PFN_WSASocketW  g_realWSASocketW = nullptr;
static PFN_setsockopt  g_realSetsockopt  = nullptr;
static PFN_getsockopt  g_realGetsockopt  = nullptr;
static PFN_WSARecvFrom g_realWSARecvFrom = nullptr;
static PFN_closesocket g_realClosesocket = nullptr;
static PFN_socket      g_realSocket      = nullptr;
static PFN_sendto      g_realSendto      = nullptr;
static PFN_WSASendTo   g_realWSASendTo   = nullptr;
static PFN_getsockname g_realGetsockname = nullptr;

static wchar_t          g_buffer_bin_path[MAX_PATH] = L"bz_buffer_log.bin";
static wchar_t          g_buffer_meta_path[MAX_PATH] = L"bz_buffer_log.meta.txt";
static bool             g_buffer_paths_ready = false;
static CRITICAL_SECTION g_buffer_lock = {};
static bool             g_buffer_lock_ready = false;
static bool             g_buffer_log_initialized = false;
static bool             g_buffer_log_enabled = false;
static uint32_t         g_buffer_payload_bytes = kDefaultPayloadBytes;
static uint32_t         g_buffer_ring_records = kDefaultRingRecords;
static uint32_t         g_buffer_stride = static_cast<uint32_t>(sizeof(BufferLogRecordHeader) + kDefaultPayloadBytes);
static uint32_t         g_buffer_head = 0;
static uint32_t         g_buffer_count = 0;
static uint32_t         g_buffer_sequence = 0;
static uint64_t         g_buffer_total_events = 0;
static uint8_t         *g_buffer_ring = nullptr;

// ---------------------------------------------------------
// Reorder globals (per-peer packet buffering)
// ---------------------------------------------------------
static bool              g_reorder_enabled     = true;   // BZ_REORDER=0 disables
static bool              g_reorder_adapt       = true;   // BZ_REORDER_ADAPT=0 -> fixed window
static uint32_t          g_reorder_ms          = kReorderDefaultMs;   // window ceiling
static uint32_t          g_reorder_min_ms      = kReorderMinMsDef;    // adaptive floor
static uint32_t          g_reorder_depth       = kReorderSlotCap;
static uint32_t          g_reorder_peers       = kReorderPeerCap;
static uint32_t          g_reorder_drain       = kReorderDrainCapDef;
static PeerBuf           g_peers[kReorderPeerCap];        // zero-initialized (BSS)
static CRITICAL_SECTION  g_reorder_cs          = {};
static bool              g_reorder_cs_ready    = false;

// ---------------------------------------------------------
// Wake helper: the reorder hook drains the kernel socket, so a game thread
// sleeping in select()/WSAEventSelect() never sees the socket readable while
// packets sit in our userspace queue.  A background thread sends a tiny magic
// datagram to the game socket's own bound port to mark it readable, waking
// the game so held packets are released within the reorder window instead of
// stranding until the next real packet arrives.  BZ_REORDER_WAKE=0 disables.
// ---------------------------------------------------------
static const uint8_t     kWakeMagic[8]         = {'B','Z','W','K','P','K','T','1'};
static bool              g_wake_enabled        = true;
static volatile LONG     g_wake_stop           = 0;
static HANDLE            g_wake_thread         = nullptr;
static SOCKET            g_wake_sender         = INVALID_SOCKET;
static SOCKET            g_reorder_sock        = INVALID_SOCKET;  // last socket seen in reorder path
static uint64_t          g_last_recv_call_ms   = 0;               // last game WSARecvFrom (reorder path)
static bool              g_wake_logged         = false;

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
constexpr uint32_t       kDupQueueSlots        = 128;
constexpr uint32_t       kDupTickMs            = 5;
constexpr uint32_t       kDupDelayMsDef        = 25;
constexpr uint32_t       kDupMaxPpsDef         = 40;

static bool              g_send_dup            = false;
static uint32_t          g_dup_delay_ms        = kDupDelayMsDef;
static uint32_t          g_dup_max_pps         = kDupMaxPpsDef;
static uint32_t          g_dscp                = kDscpDefault;
// Opt-in diagnostic (BZ_GOV_SCAN=1, default off).  The exe is SteamStub-DRM
// wrapped so .text is encrypted on disk and cannot be signature-scanned
// offline; this scans the DECRYPTED .text at runtime for the governor's
// hardcoded 4000 B/s start constant (0x00000FA0) and logs candidate sites,
// so the runtime governor patch can be built from a genuine signature.
// Read-only; never patches.  Parity with the Proton dsound proxy.
static bool              g_gov_scan            = false;
static HANDLE            g_gov_thread          = nullptr;

// Governor cold-start patch (BZ_GOV_START=<bytes/sec>, default 0 = disabled).
// The send governor hardcodes a 4000 B/s start for every match (net.ini
// MinBandwidth is copied to the live rate BEFORE net.ini is read), starving
// the opening world-state burst.  Rewriting the 4000 immediate in .text works
// mechanically but SteamStub's runtime integrity check then kills the process,
// so we do NOT touch code.  Instead we watch the governor's live send-rate
// DATA global and rewrite the 4000 cold-start sentinel to g_gov_start (the
// ramp and the MinBandwidth floor move it off 4000 immediately and never
// return to exactly 4000).  A 32-bit aligned store is atomic on x86 and .data
// carries no integrity check, so the DRM is untouched.  Verified end-to-end
// under Proton; the addresses are identical on real Windows (fixed base
// 0x400000, no ASLR).  Sender-side: improves how our packets reach every peer.
static uint32_t          g_gov_start           = 0;
static volatile LONG     g_gov_stop            = 0;
static HANDLE            g_gov_patch_thread     = nullptr;
constexpr DWORD          kGovPollMs            = 100;
static uint32_t *const   kGovRateAddr          = reinterpret_cast<uint32_t *>(0x008e8d14);
constexpr uint32_t       kGovColdStart         = 4000;
// Unique version fingerprint: push 4000; push 1000; push -3000.
static const uint8_t     kGovSig[15] = {
    0x68, 0xA0, 0x0F, 0x00, 0x00,
    0x68, 0xE8, 0x03, 0x00, 0x00,
    0x68, 0x48, 0xF4, 0xFF, 0xFF
};

// AutoKick threshold overrides (BZ_AUTOKICK_*, each 0 = leave the game's value).
// The kick that ejects a "lagging" player is governed by four .data globals the
// session parser reads from net.ini's [Net] section at match start (captured
// 2026-07-04 from the decrypted image; monitor at 0x576c40):
//   AutoKickStart 0x8e8d0c  grace period (ms) after a join before monitoring  (default 10000)
//   AutoKickPing  0x8e8cf8  ping ceiling (ms); a tick above this is "bad"      (default 750)
//   AutoKickLoss  0x8e8bfc  loss-count ceiling; a tick above this is "bad"     (default 25)
//   AutoKickTime  0x8e8ce4  ms the connection must stay continuously bad       (default 15000)
// A tick is bad when ping > AutoKickPing OR loss > AutoKickLoss; once bad for
// AutoKickTime the host kicks the player.  Auto-kick is HOST-ENFORCED, so these
// only bite when THIS machine hosts the session.  Same DRM-safe data-poke as the
// governor (no .text write); re-asserted every poll so our value wins over both
// the stock default and any net.ini value.  Version-gated on kGovSig.  Fixed
// addresses identical on Proton and real Windows (base 0x400000, no ASLR).
static uint32_t          g_ak_time             = 0;
static uint32_t          g_ak_ping             = 0;
static uint32_t          g_ak_loss             = 0;
static uint32_t          g_ak_start            = 0;
static volatile LONG     g_ak_stop             = 0;
static HANDLE            g_ak_patch_thread     = nullptr;
static uint32_t *const   kAkStartAddr          = reinterpret_cast<uint32_t *>(0x008e8d0c);
static uint32_t *const   kAkPingAddr           = reinterpret_cast<uint32_t *>(0x008e8cf8);
static uint32_t *const   kAkLossAddr           = reinterpret_cast<uint32_t *>(0x008e8bfc);
static uint32_t *const   kAkTimeAddr           = reinterpret_cast<uint32_t *>(0x008e8ce4);

struct DupEntry {
    SOCKET           sock;
    uint64_t         due_ms;
    int              tolen;
    uint32_t         len;
    sockaddr_storage to;
    uint8_t          data[kReorderMaxPktBytes];
};
static DupEntry          g_dup_q[kDupQueueSlots];
static uint32_t          g_dup_q_head          = 0;
static uint32_t          g_dup_q_count         = 0;
static CRITICAL_SECTION  g_dup_cs              = {};
static bool              g_dup_cs_ready        = false;
static volatile LONG     g_dup_stop            = 0;
static HANDLE            g_dup_thread          = nullptr;
static uint64_t          g_dup_bucket_start_ms = 0;
static uint32_t          g_dup_bucket_sent     = 0;

static bool env_truthy(const char *s) {
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

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static uint32_t parse_env_u32(const char *name, uint32_t fallback) {
    const char *v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    char *end = nullptr;
    unsigned long parsed = std::strtoul(v, &end, 10);
    if (end == nullptr || *end != '\0' || parsed > 0xffffffffUL) {
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
static DWORD WINAPI DupPacerThread(LPVOID) {
    while (InterlockedCompareExchange(&g_dup_stop, 0, 0) == 0) {
        Sleep(kDupTickMs);
        if (!g_dup_cs_ready || g_realSendto == nullptr) {
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
            g_realSendto(local.sock, reinterpret_cast<const char *>(local.data),
                         static_cast<int>(local.len), 0,
                         reinterpret_cast<const sockaddr *>(&local.to), local.tolen);
        }
    }
    return 0;
}

static void init_buffer_paths() {
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

static void init_buffer_log_if_needed() {
    if (g_buffer_log_initialized) {
        return;
    }
    g_buffer_log_initialized = true;
    init_buffer_paths();

    const char *enabled = std::getenv("BZ_BUFFER_LOG");
    if (!env_truthy(enabled)) {
        ProxyLog("buffer_log: disabled (set BZ_BUFFER_LOG=1 to enable)");
        return;
    }

    g_buffer_payload_bytes = clamp_u32(parse_env_u32("BZ_BUFFER_LOG_BYTES", kDefaultPayloadBytes), kMinPayloadBytes, kMaxPayloadBytes);
    g_buffer_ring_records = clamp_u32(parse_env_u32("BZ_BUFFER_LOG_RING", kDefaultRingRecords), kMinRingRecords, kMaxRingRecords);
    g_buffer_stride = static_cast<uint32_t>(sizeof(BufferLogRecordHeader) + g_buffer_payload_bytes);

    size_t total = static_cast<size_t>(g_buffer_stride) * static_cast<size_t>(g_buffer_ring_records);
    g_buffer_ring = reinterpret_cast<uint8_t *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total));
    if (g_buffer_ring == nullptr) {
        ProxyLog("buffer_log: allocation failed bytes=%lu", static_cast<unsigned long>(total));
        return;
    }

    g_buffer_log_enabled = true;
    ProxyLog("buffer_log: enabled payload=%u ring=%u stride=%u",
             static_cast<unsigned>(g_buffer_payload_bytes),
             static_cast<unsigned>(g_buffer_ring_records),
             static_cast<unsigned>(g_buffer_stride));
}

static void buffer_log_event(uint32_t event_type,
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

    EnterCriticalSection(&g_buffer_lock);
    uint32_t idx = g_buffer_head;
    uint8_t *slot = g_buffer_ring + (static_cast<size_t>(idx) * static_cast<size_t>(g_buffer_stride));

    BufferLogRecordHeader rec = {};
    rec.magic = kBufferLogMagic;
    rec.version = kBufferLogVersion;
    rec.event_type = event_type;
    rec.sid = static_cast<uint32_t>(s);
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

static void flush_buffer_log_files() {
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
                              "format=buffer_log_v1\r\nrecord_header_size=%u\r\npayload_bytes=%u\r\nrecord_stride=%u\r\nring_records=%u\r\nrecords_written=%u\r\ntotal_events_seen=%llu\r\n",
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

    ProxyLog("buffer_log: flushed records=%u total_events=%llu",
             static_cast<unsigned>(g_buffer_count),
             static_cast<unsigned long long>(g_buffer_total_events));
}

// Helper: sequence number comparison.  BZRNet wraps at 2^32, so we use
// modular arithmetic (sint32 overflow to detect wrap).
static inline int seq_cmp_u32(uint32_t a, uint32_t b) {
    return (static_cast<int32_t>(a - b) > 0) ? 1 : ((a == b) ? 0 : -1);
}

static inline bool seq_ahead_or_equal(uint32_t seq, uint32_t want) {
    return seq_cmp_u32(seq, want) >= 0;
}

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

    int cmp = seq_cmp_u32(seq, pb->last_seq);
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

// -----

// ---------------------------------------------------------
// Our WSASocketW hook
// ---------------------------------------------------------
// Mark a UDP socket with the configured DSCP class via IP_TOS.  No-op when
// g_dscp is 0 or setsockopt is unavailable.  Returns the setsockopt rc (or 0
// when disabled) so the caller can log it.  Effective on Proton; harmless on
// stock Windows, where the option is ignored by policy.
static int apply_dscp(SOCKET s)
{
    if (g_dscp == 0 || g_realSetsockopt == nullptr) {
        return 0;
    }
    // The TOS byte carries DSCP in its top 6 bits.
    int tos = static_cast<int>(g_dscp << 2);
    return g_realSetsockopt(s, IPPROTO_IP, IP_TOS,
        (const char*)&tos, sizeof(tos));
}

static SOCKET WSAAPI Hooked_WSASocketW(
    int af, int type, int protocol,
    LPWSAPROTOCOL_INFOW lpProtocolInfo,
    GROUP g, DWORD dwFlags)
{
    SOCKET s = g_realWSASocketW(af, type, protocol, lpProtocolInfo, g, dwFlags);

    if (s == INVALID_SOCKET)
        return s;

    // Apply only to UDP datagram sockets (game P2P transport).
    if (type == SOCK_DGRAM || protocol == IPPROTO_UDP)
    {
        int sndVal = kTargetSndBuf;
        int rcvVal = kTargetRcvBuf;

        int rc_snd = g_realSetsockopt(s, SOL_SOCKET, SO_SNDBUF,
            (const char*)&sndVal, sizeof(sndVal));
        int rc_rcv = g_realSetsockopt(s, SOL_SOCKET, SO_RCVBUF,
            (const char*)&rcvVal, sizeof(rcvVal));
        int rc_tos = apply_dscp(s);

        // Immediate readback – this is what testers verify in the log.
        int snd_read = -1, rcv_read = -1;
        int snd_len  = sizeof(snd_read), rcv_len = sizeof(rcv_read);
        g_realGetsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&snd_read, &snd_len);
        g_realGetsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&rcv_read, &rcv_len);

        ProxyLog(
            "WSASocketW hook: sock=0x%p af=%d type=%d proto=%d"
            "  SO_SNDBUF set_rc=%d effective readback SO_SNDBUF=%d"
            "  SO_RCVBUF set_rc=%d effective readback SO_RCVBUF=%d"
            "  DSCP=%u IP_TOS set_rc=%d",
            (void*)s, af, type, protocol,
            rc_snd, snd_read,
            rc_rcv, rcv_read,
            g_dscp, rc_tos);
    }

    return s;
}

// ---------------------------------------------------------
// Our setsockopt hook – re-force the socket buffers and DSCP that the game
// clobbers.  On real Windows the game issues its own setsockopt(SO_SNDBUF,
// 32768) after WSASocketW returns, undoing our enlargement; intercepting it
// keeps the buffers (and QoS marking) at our targets.  Parity with the Linux
// dsound proxy.  Non-buffer options pass through untouched.
// ---------------------------------------------------------
static int WSAAPI Hooked_setsockopt(SOCKET s, int level, int optname,
    const char* optval, int optlen)
{
    if (!g_realSetsockopt) {
        WSASetLastError(WSANOTINITIALISED);
        return SOCKET_ERROR;
    }

    if (level == SOL_SOCKET && optname == SO_SNDBUF) {
        int forced = kTargetSndBuf;
        int rc = g_realSetsockopt(s, level, optname,
            (const char*)&forced, sizeof(forced));
        apply_dscp(s);
        return rc;
    }

    if (level == SOL_SOCKET && optname == SO_RCVBUF) {
        int forced = kTargetRcvBuf;
        int rc = g_realSetsockopt(s, level, optname,
            (const char*)&forced, sizeof(forced));
        return rc;
    }

    return g_realSetsockopt(s, level, optname, optval, optlen);
}

// ---------------------------------------------------------
// Our WSARecvFrom hook – implements OOO packet reorder
// ---------------------------------------------------------
static int WSAAPI Hooked_WSARecvFrom(
    SOCKET s,
    LPWSABUF buffers,
    DWORD buffer_count,
    LPDWORD bytes_received,
    LPDWORD inout_flags,
    struct sockaddr *from,
    LPINT fromlen,
    LPWSAOVERLAPPED ov,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    if (!g_realWSARecvFrom) {
        WSASetLastError(WSANOTINITIALISED);
        return SOCKET_ERROR;
    }

    // Bypass: overlapped/async path, reorder disabled, or bad arguments.
    // The overlapped check is load-bearing: the game's asio engine uses
    // IOCP overlapped receives on Windows, and routing those through the
    // synchronous drain path stalls its completion loop forever (game
    // freezes at the splash screen).  Parity with the Linux dsound proxy.
    if (!g_reorder_enabled || !g_reorder_cs_ready
        || ov != nullptr || cr != nullptr
        || buffers == nullptr || buffer_count == 0) {
        int rc = g_realWSARecvFrom(s, buffers, buffer_count, bytes_received, inout_flags,
                                   from, fromlen, ov, cr);
        int wsa = static_cast<int>(WSAGetLastError());
        if (g_buffer_log_enabled) {
            uint32_t requested = 0;
            for (DWORD i = 0; i < buffer_count && buffers != nullptr; ++i) {
                requested += buffers[i].len;
            }
            uint32_t transferred = (rc == 0 && bytes_received != nullptr) ? *bytes_received : 0u;
            uint16_t recv_flags = (inout_flags != nullptr) ? static_cast<uint16_t>(*inout_flags & 0xffffUL) : 0;
            uint16_t payload_len = static_cast<uint16_t>((transferred < g_buffer_payload_bytes) ? transferred : g_buffer_payload_bytes);
            const uint8_t *payload = (payload_len > 0 && buffers != nullptr && buffers[0].buf != nullptr)
                                     ? reinterpret_cast<const uint8_t *>(buffers[0].buf) : nullptr;
            buffer_log_event(kEventTypeWSARecvFrom, s, from, recv_flags, requested, transferred,
                             (rc == SOCKET_ERROR) ? static_cast<uint32_t>(wsa) : 0u, payload, payload_len);
        }
        WSASetLastError(wsa);
        return rc;
    }

    EnterCriticalSection(&g_reorder_cs);

    // Tell the wake thread the game is actively polling the reorder socket.
    // Only polls of THAT socket count: a second UDP socket (lobby/discovery)
    // must neither retarget wakes nor suppress them.  g_reorder_sock itself
    // is assigned below, at the point a reorderable packet is buffered.
    if (s == g_reorder_sock) {
        g_last_recv_call_ms = GetTickCount64();
    }

    // Drain loop: pull up to g_reorder_drain packets from the socket without
    // delivering them, buffer them per-source, then deliver the first ready one.
    uint8_t  drain_buf[kReorderMaxPktBytes];
    sockaddr_in drain_src;

    for (uint32_t drain_count = 0; drain_count < g_reorder_drain; ++drain_count) {
        WSABUF drain_wsabuf = {
            static_cast<u_long>(sizeof(drain_buf)),
            reinterpret_cast<char*>(drain_buf)
        };
        DWORD drain_flags = 0;
        DWORD drain_bytes = 0;
        int drain_srclen = static_cast<int>(sizeof(drain_src));

        int drc = g_realWSARecvFrom(s, &drain_wsabuf, 1, &drain_bytes, &drain_flags,
                                    reinterpret_cast<sockaddr*>(&drain_src), &drain_srclen,
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
                const uint8_t *pay = (pay_len > 0 && buffers != nullptr && buffers[0].buf != nullptr)
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
                const uint8_t *pay = (pay_len > 0 && buffers != nullptr && buffers[0].buf != nullptr)
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

    uint32_t delivered = scatter_copy(buffers, buffer_count, pkt->data, pkt->len);

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

    sockaddr_in deliver_from = pkt->from;

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

// ---------------------------------------------------------
// Our closesocket hook – reset per-peer reorder state
// ---------------------------------------------------------
static int WSAAPI Hooked_closesocket(SOCKET s)
{
    if (!g_realClosesocket) {
        return SOCKET_ERROR;
    }

    int rc = g_realClosesocket(s);

    // Reset per-peer reorder state. BZ uses one UDP socket for all P2P; closing
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

// ---------------------------------------------------------
// Our sendto hook – opt-in outbound duplication (BZ_SEND_DUP)
// ---------------------------------------------------------
static int WSAAPI Hooked_sendto(SOCKET s, const char* buf, int len, int flags,
    const struct sockaddr* to, int tolen)
{
    if (!g_realSendto) {
        WSASetLastError(WSANOTINITIALISED);
        return SOCKET_ERROR;
    }

    int rc = g_realSendto(s, buf, len, flags, to, tolen);

    // Duplicate only IPv4 datagrams large enough to carry a BZRNet sequence
    // field: control/wake packets stay single-shot.  The first call's result
    // and error state are what the game sees.
    if (g_send_dup && rc >= 0 && buf != nullptr && to != nullptr
        && to->sa_family == AF_INET && len >= static_cast<int>(kReorderSeqMinPay)
        && !dup_is_loopback(to)) {
        int wsa = static_cast<int>(WSAGetLastError());
        if (g_dup_delay_ms == 0) {
            g_realSendto(s, buf, len, flags, to, tolen);
        } else {
            dup_enqueue(s, reinterpret_cast<const uint8_t *>(buf),
                        static_cast<uint32_t>(len), to, tolen);
        }
        WSASetLastError(wsa);
    }

    return rc;
}

// ---------------------------------------------------------
// Our WSASendTo hook – the game sends all P2P traffic through WSASendTo
// (its IAT has no plain sendto import), so BZ_SEND_DUP lives here.
// ---------------------------------------------------------
static int WSAAPI Hooked_WSASendTo(
    SOCKET s, LPWSABUF buffers, DWORD buffer_count,
    LPDWORD bytes_sent, DWORD flags,
    const struct sockaddr* to, int tolen,
    LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    if (!g_realWSASendTo) {
        WSASetLastError(WSANOTINITIALISED);
        return SOCKET_ERROR;
    }

    int rc = g_realWSASendTo(s, buffers, buffer_count, bytes_sent, flags, to, tolen, ov, cr);
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
                dup_buf.buf = reinterpret_cast<char*>(flat);
                dup_buf.len = static_cast<u_long>(total);
                DWORD dup_sent = 0;
                g_realWSASendTo(s, &dup_buf, 1, &dup_sent, 0, to, tolen, nullptr, nullptr);
            } else {
                dup_enqueue(s, flat, total, to, tolen);
            }
        }
    }

    WSASetLastError(wsa);
    return rc;
}

// ---------------------------------------------------------
// Wake thread – prevents held packets from stranding.
// While the reorder queue holds packets and the game is not actively polling
// WSARecvFrom, nudge the (drained) socket readable again by sending a small
// magic datagram to its own bound address.  The game's select()/event wait
// fires, it calls WSARecvFrom, our hook discards the magic packet and
// releases any packet whose hold window has expired.
// ---------------------------------------------------------
static DWORD WINAPI ReorderWakeThread(LPVOID)
{
    uint64_t seen_call = 0;   // last g_last_recv_call_ms we acted on
    uint32_t burst     = 0;   // wakes sent since the game last polled

    while (InterlockedCompareExchange(&g_wake_stop, 0, 0) == 0) {
        Sleep(kReorderWakeTickMs);

        if (!g_reorder_enabled || !g_reorder_cs_ready
            || !g_realGetsockname || !g_realSendto || !g_realSocket) {
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
        if (g_realGetsockname(target, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0
            || bound.sin_family != AF_INET || bound.sin_port == 0) {
            continue; // unbound or already closed
        }
        if (bound.sin_addr.S_un.S_addr == htonl(INADDR_ANY)) {
            bound.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
        }

        if (g_wake_sender == INVALID_SOCKET) {
            g_wake_sender = g_realSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (g_wake_sender == INVALID_SOCKET) {
                continue;
            }
        }

        g_realSendto(g_wake_sender,
                     reinterpret_cast<const char*>(kWakeMagic),
                     static_cast<int>(sizeof(kWakeMagic)), 0,
                     reinterpret_cast<const sockaddr*>(&bound),
                     static_cast<int>(sizeof(bound)));
        ++burst;

        if (!g_wake_logged) {
            g_wake_logged = true;
            ProxyLog("reorder: wake helper active (held packets, idle game poll)");
        }
    }
    return 0;
}

// ---------------------------------------------------------
// IAT patcher
// Finds moduleName!funcName in the IAT of `module` and
// replaces the slot with newFunc.  If oldFunc is non-null,
// the previous value is stored there.
// ---------------------------------------------------------
// `ordinal` handles ws2_32's classic winsock functions (closesocket=3,
// sendto=20, ...): the game exe imports those by ordinal, not by name, so
// a name-only walk never finds them.  Pass 0 to match by name only.
static bool PatchIAT(HMODULE module, const char* dllName,
    const char* funcName, WORD ordinal, void* newFunc, void** oldFunc)
{
    if (!module) return false;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<BYTE*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    DWORD rva = nt->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (rva == 0) return false;

    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        reinterpret_cast<BYTE*>(module) + rva);

    for (; imp->Name; ++imp)
    {
        const char* name = reinterpret_cast<const char*>(
            reinterpret_cast<BYTE*>(module) + imp->Name);

        if (_stricmp(name, dllName) != 0) continue;

        // Use OriginalFirstThunk for names; fall back to FirstThunk
        // if OriginalFirstThunk is zero (some linkers omit it).
        auto* origThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<BYTE*>(module) +
            (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                     : imp->FirstThunk));
        auto* iatThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<BYTE*>(module) + imp->FirstThunk);

        for (; origThunk->u1.AddressOfData; ++origThunk, ++iatThunk)
        {
            if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal))
            {
                if (ordinal == 0 || IMAGE_ORDINAL(origThunk->u1.Ordinal) != ordinal)
                    continue;
            }
            else
            {
                auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    reinterpret_cast<BYTE*>(module) +
                    origThunk->u1.AddressOfData);

                if (strcmp(reinterpret_cast<const char*>(ibn->Name), funcName) != 0)
                    continue;
            }

            // Patch: make the page writable, swap the pointer, restore.
            void** slot = reinterpret_cast<void**>(&iatThunk->u1.Function);
            DWORD  oldProt = 0;
            if (!VirtualProtect(slot, sizeof(void*),
                    PAGE_READWRITE, &oldProt))
                return false;

            if (oldFunc) *oldFunc = *slot;
            *slot = newFunc;

            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void*), oldProt, &ignored);
            return true;
        }

        // Found the right DLL block but didn't find the function name.
        ProxyLog("PatchIAT: '%s' not found in import block for '%s'",
            funcName, dllName);
        return false;
    }

    // dllName not present in the import table at all.
    return false;
}

// ---------------------------------------------------------
// Governor scanner (opt-in BZ_GOV_SCAN) – parity with the Proton proxy.
// ---------------------------------------------------------

// Locate a named PE section in the main module.  Headers and (post-DRM-
// decryption) section bodies are mapped/readable.
static bool FindSection(const char *want, BYTE **out_start, size_t *out_size)
{
    auto *base = reinterpret_cast<BYTE *>(GetModuleHandleW(nullptr));
    if (base == nullptr) return false;
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto *sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char name[9] = {0};
        std::memcpy(name, sec->Name, 8);
        if (std::strncmp(name, want, 8) == 0) {
            *out_start = base + sec->VirtualAddress;
            *out_size  = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// Raise the governor's 4000 B/s cold start to g_gov_start by watching the
// live send-rate DATA global (no .text write; see the note at g_gov_start).
static DWORD WINAPI GovernorPatchThread(LPVOID)
{
    if (g_gov_start == 0) {
        return 0;
    }
    Sleep(15000);   // let SteamStub decrypt .text first

    BYTE *text = nullptr;
    size_t text_size = 0;
    if (!FindSection(".text", &text, &text_size)) {
        ProxyLog("governor_patch: .text section not found");
        return 0;
    }
    int matches = 0;
    for (size_t i = 0; i + sizeof(kGovSig) <= text_size; ++i) {
        if (std::memcmp(text + i, kGovSig, sizeof(kGovSig)) == 0) {
            if (++matches > 1) break;
        }
    }
    if (matches != 1) {
        ProxyLog("governor_patch: %d governor signature matches (need exactly 1) - "
                 "disabled. Game version may have changed; re-run BZ_GOV_SCAN.", matches);
        return 0;
    }
    ProxyLog("governor_patch: version confirmed; watching send-rate 0x%08lx, "
             "cold-start %u -> %u (data-only, no .text write)",
             (unsigned long)(uintptr_t)kGovRateAddr,
             (unsigned)kGovColdStart, (unsigned)g_gov_start);

    unsigned long bumps = 0;
    while (InterlockedCompareExchange(&g_gov_stop, 0, 0) == 0) {
        if (*kGovRateAddr == kGovColdStart) {
            *kGovRateAddr = g_gov_start;
            if (bumps == 0) {
                ProxyLog("governor_patch: cold-start caught, send-rate %u -> %u (match started)",
                         (unsigned)kGovColdStart, (unsigned)g_gov_start);
            }
            ++bumps;
        }
        Sleep(kGovPollMs);
    }
    ProxyLog("governor_patch: stopping after %lu cold-start bump(s)", bumps);
    return 0;
}

// Relax the host's auto-kick thresholds to the BZ_AUTOKICK_* values.  Same
// DRM-safe strategy as GovernorPatchThread: never touch .text (SteamStub's
// integrity check would kill the process), only the .data threshold globals via
// aligned 32-bit stores.  The session parser rewrites them at each match start
// (from net.ini or the stock default), so we re-assert on a poll loop — within
// one tick of any match starting, our value wins.  Version-gated on kGovSig so
// the fixed addresses are only trusted on the build they were captured from.
// Host-enforced: only affects kicks when this machine is the session host.
static DWORD WINAPI AutoKickPatchThread(LPVOID)
{
    if (g_ak_time == 0 && g_ak_ping == 0 && g_ak_loss == 0 && g_ak_start == 0) {
        return 0;
    }
    Sleep(15000);   // let SteamStub decrypt .text first

    BYTE *text = nullptr;
    size_t text_size = 0;
    if (!FindSection(".text", &text, &text_size)) {
        ProxyLog("autokick_patch: .text section not found");
        return 0;
    }
    int matches = 0;
    for (size_t i = 0; i + sizeof(kGovSig) <= text_size; ++i) {
        if (std::memcmp(text + i, kGovSig, sizeof(kGovSig)) == 0) {
            if (++matches > 1) break;
        }
    }
    if (matches != 1) {
        ProxyLog("autokick_patch: %d version signature matches (need exactly 1) - "
                 "disabled. Game version may have changed; re-run BZ_GOV_SCAN.", matches);
        return 0;
    }

    struct AkSlot { uint32_t *addr; uint32_t val; const char *name; bool logged; };
    AkSlot slots[4] = {
        { kAkStartAddr, g_ak_start, "AutoKickStart", false },
        { kAkPingAddr,  g_ak_ping,  "AutoKickPing",  false },
        { kAkLossAddr,  g_ak_loss,  "AutoKickLoss",  false },
        { kAkTimeAddr,  g_ak_time,  "AutoKickTime",  false },
    };
    ProxyLog("autokick_patch: version confirmed; overriding start=%u ping=%u loss=%u "
             "time=%u (0=leave; re-asserted every %ums, host-enforced)",
             (unsigned)g_ak_start, (unsigned)g_ak_ping, (unsigned)g_ak_loss,
             (unsigned)g_ak_time, (unsigned)kGovPollMs);

    while (InterlockedCompareExchange(&g_ak_stop, 0, 0) == 0) {
        for (AkSlot &s : slots) {
            if (s.val == 0 || *s.addr == s.val) {
                continue;
            }
            uint32_t prev = *s.addr;
            *s.addr = s.val;
            if (!s.logged) {
                ProxyLog("autokick_patch: %s %u -> %u (match started)",
                         s.name, (unsigned)prev, (unsigned)s.val);
                s.logged = true;
            }
        }
        Sleep(kGovPollMs);
    }
    ProxyLog("autokick_patch: stopping");
    return 0;
}

static DWORD WINAPI GovernorScanThread(LPVOID)
{
    // Let SteamStub decrypt .text and the game reach the menu first.
    Sleep(15000);

    BYTE *text = nullptr;
    size_t text_size = 0;
    if (!FindSection(".text", &text, &text_size)) {
        ProxyLog("governor_scan: .text section not found");
        return 0;
    }
    ProxyLog("governor_scan: scanning .text base=%p size=%u for 0x00000FA0 (4000)",
             (void *)text, static_cast<unsigned>(text_size));

    const uint8_t pat[4] = {0xA0, 0x0F, 0x00, 0x00};   // 4000, little-endian
    int hits = 0;
    const int kMaxHits = 48;
    for (size_t i = 0; i + sizeof(pat) <= text_size && hits < kMaxHits; ++i) {
        if (std::memcmp(text + i, pat, sizeof(pat)) != 0) continue;
        size_t ctx_lo = (i >= 3) ? 3 : i;
        char ctx[64] = {0};
        int p = 0;
        for (size_t k = i - ctx_lo; k < i + 8 && k < text_size && p < 60; ++k) {
            p += std::snprintf(ctx + p, sizeof(ctx) - p, "%02x ", text[k]);
        }
        ProxyLog("governor_scan: hit #%d va=0x%08lx  bytes[ %s]",
                 hits + 1, (unsigned long)(text + i), ctx);
        hits++;
    }
    ProxyLog("governor_scan: done, %d candidate site(s)%s. Report these to build "
             "the runtime governor patch.", hits, (hits >= kMaxHits) ? " (capped)" : "");
    return 0;
}

// ---------------------------------------------------------
// InstallNetcodeHooks – public entry point
// ---------------------------------------------------------
void InstallNetcodeHooks()
{
    ProxyLog("InstallNetcodeHooks: starting");

    if (!g_buffer_lock_ready) {
        InitializeCriticalSection(&g_buffer_lock);
        g_buffer_lock_ready = true;
    }
    init_buffer_log_if_needed();

    // Initialize reorder critical section
    if (!g_reorder_cs_ready) {
        InitializeCriticalSection(&g_reorder_cs);
        g_reorder_cs_ready = true;
    }

    // Initialize dup pacer critical section
    if (!g_dup_cs_ready) {
        InitializeCriticalSection(&g_dup_cs);
        g_dup_cs_ready = true;
    }

    // Resolve WS2 functions we need.
    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (!ws2) ws2 = LoadLibraryA("ws2_32.dll");
    if (!ws2)
    {
        ProxyLog("InstallNetcodeHooks: cannot load ws2_32.dll (err=%lu)",
            GetLastError());
        return;
    }

    g_realWSASocketW = (PFN_WSASocketW) GetProcAddress(ws2, "WSASocketW");
    g_realSetsockopt = (PFN_setsockopt) GetProcAddress(ws2, "setsockopt");
    g_realGetsockopt = (PFN_getsockopt) GetProcAddress(ws2, "getsockopt");
    g_realWSARecvFrom = (PFN_WSARecvFrom) GetProcAddress(ws2, "WSARecvFrom");
    g_realClosesocket = (PFN_closesocket) GetProcAddress(ws2, "closesocket");
    g_realSocket      = (PFN_socket)      GetProcAddress(ws2, "socket");
    g_realSendto      = (PFN_sendto)      GetProcAddress(ws2, "sendto");
    g_realWSASendTo   = (PFN_WSASendTo)   GetProcAddress(ws2, "WSASendTo");
    g_realGetsockname = (PFN_getsockname) GetProcAddress(ws2, "getsockname");

    if (!g_realWSASocketW || !g_realSetsockopt || !g_realGetsockopt || !g_realWSARecvFrom || !g_realClosesocket)
    {
        ProxyLog("InstallNetcodeHooks: failed to resolve ws2_32 functions");
        return;
    }

    // Apply user-tunable reorder parameters (all optional; parity with the
    // Linux dsound proxy env vars)
    {
        const char *reorder_env = std::getenv("BZ_REORDER");
        g_reorder_enabled = (reorder_env == nullptr || *reorder_env == '\0')
                            ? true : env_truthy(reorder_env);
        const char *adapt_env = std::getenv("BZ_REORDER_ADAPT");
        g_reorder_adapt = (adapt_env == nullptr || *adapt_env == '\0')
                          ? true : env_truthy(adapt_env);
        const char *wake_env = std::getenv("BZ_REORDER_WAKE");
        g_wake_enabled = (wake_env == nullptr || *wake_env == '\0')
                         ? true : env_truthy(wake_env);
        g_reorder_ms     = clamp_u32(parse_env_u32("BZ_REORDER_WINDOW_MS", kReorderDefaultMs), 5, 200);
        g_reorder_min_ms = clamp_u32(parse_env_u32("BZ_REORDER_MIN_MS", kReorderMinMsDef), 0, g_reorder_ms);
        g_reorder_depth  = clamp_u32(parse_env_u32("BZ_REORDER_DEPTH", kReorderSlotCap), 1, kReorderSlotCap);
        g_reorder_peers  = clamp_u32(parse_env_u32("BZ_REORDER_PEERS", kReorderPeerCap), 1, kReorderPeerCap);
        g_reorder_drain  = clamp_u32(parse_env_u32("BZ_REORDER_DRAIN", kReorderDrainCapDef), 1, kReorderDrainCapMax);
        // Off by default: adds upstream traffic on the P2P socket.
        g_send_dup = env_truthy(std::getenv("BZ_SEND_DUP"));
        g_dup_delay_ms = clamp_u32(parse_env_u32("BZ_DUP_DELAY_MS", kDupDelayMsDef), 0, 500);
        g_dup_max_pps  = clamp_u32(parse_env_u32("BZ_DUP_MAX_PPS", kDupMaxPpsDef), 0, 2000);
        // DSCP class for the P2P socket (0 disables); clamp to the 6-bit field.
        g_dscp = clamp_u32(parse_env_u32("BZ_DSCP", kDscpDefault), 0, 63);
        g_gov_scan = env_truthy(std::getenv("BZ_GOV_SCAN"));
        // Governor cold-start rate (0 = disabled). Clamp to a sane band.
        g_gov_start = clamp_u32(parse_env_u32("BZ_GOV_START", 0), 0, 200000);
        // AutoKick threshold overrides (each 0 = leave the game's value).
        // The relax preset is ON by default (BZ_AUTOKICK_RELAX=0 restores
        // stock kicking) and fills only the knobs not set individually.
        // Takes precedence over net.ini, which the game ignores unless it
        // ships inside the session's active mod (2026-07-05: 15s stock
        // kick fired with the 9990001 net.ini found-but-unapplied).
        {
            const char *ak_env = std::getenv("BZ_AUTOKICK_RELAX");
            bool ak_relax = (ak_env == nullptr || *ak_env == '\0')
                            ? true : env_truthy(ak_env);
            g_ak_start = clamp_u32(parse_env_u32("BZ_AUTOKICK_START", ak_relax ? 60000 : 0), 0, 600000);
            g_ak_ping  = clamp_u32(parse_env_u32("BZ_AUTOKICK_PING",  ak_relax ? 2000  : 0), 0, 60000);
            g_ak_loss  = clamp_u32(parse_env_u32("BZ_AUTOKICK_LOSS",  ak_relax ? 200   : 0), 0, 100000);
            g_ak_time  = clamp_u32(parse_env_u32("BZ_AUTOKICK_TIME",  ak_relax ? 60000 : 0), 0, 600000);
        }
    }
    ProxyLog("governor_patch: %s (BZ_GOV_START=%u; 0=disabled)",
             g_gov_start ? "enabled" : "disabled", g_gov_start);
    ProxyLog("autokick_patch: %s (start=%u ping=%u loss=%u time=%u; 0=leave, host-enforced)",
             (g_ak_time || g_ak_ping || g_ak_loss || g_ak_start) ? "enabled" : "disabled",
             g_ak_start, g_ak_ping, g_ak_loss, g_ak_time);

    // IAT-patch WSASocketW and WSARecvFrom in the game EXE.
    HMODULE exe = GetModuleHandleA(nullptr);
    void* savedRealSocket = nullptr;
    void* savedRealRecvFrom = nullptr;

    bool patchedSocket = PatchIAT(exe, "WS2_32.dll", "WSASocketW", 0,
        reinterpret_cast<void*>(Hooked_WSASocketW), &savedRealSocket);
    if (!patchedSocket)
    {
        // Some builds lowercase the DLL name in the import directory.
        patchedSocket = PatchIAT(exe, "ws2_32.dll", "WSASocketW", 0,
            reinterpret_cast<void*>(Hooked_WSASocketW), &savedRealSocket);
    }

    bool patchedRecvFrom = PatchIAT(exe, "WS2_32.dll", "WSARecvFrom", 0,
        reinterpret_cast<void*>(Hooked_WSARecvFrom), &savedRealRecvFrom);
    if (!patchedRecvFrom)
    {
        patchedRecvFrom = PatchIAT(exe, "ws2_32.dll", "WSARecvFrom", 0,
            reinterpret_cast<void*>(Hooked_WSARecvFrom), &savedRealRecvFrom);
    }

    if (patchedSocket)
    {
        // Use the actual IAT slot value as our real-function pointer so
        // any upstream IAT hook (e.g. Steam overlay) is preserved in the chain.
        if (savedRealSocket) g_realWSASocketW = reinterpret_cast<PFN_WSASocketW>(savedRealSocket);
        ProxyLog("InstallNetcodeHooks: WSASocketW IAT patched OK"
                 "  SO_SNDBUF target=%d  SO_RCVBUF target=%d",
                 kTargetSndBuf, kTargetRcvBuf);
    }
    else
    {
        ProxyLog("InstallNetcodeHooks: WSASocketW not found in game IAT"
                 " - buffers will NOT be applied");
    }

    if (patchedRecvFrom)
    {
        if (savedRealRecvFrom) g_realWSARecvFrom = reinterpret_cast<PFN_WSARecvFrom>(savedRealRecvFrom);
        ProxyLog("InstallNetcodeHooks: WSARecvFrom IAT patched OK"
                 "  OOO reorder %s max_window_ms=%u min_window_ms=%u adapt=%d wake=%d depth=%u peers=%u drain=%u",
                 g_reorder_enabled ? "enabled" : "DISABLED",
                 static_cast<unsigned>(g_reorder_ms),
                 static_cast<unsigned>(g_reorder_min_ms),
                 g_reorder_adapt ? 1 : 0,
                 g_wake_enabled ? 1 : 0,
                 static_cast<unsigned>(g_reorder_depth),
                 static_cast<unsigned>(g_reorder_peers),
                 static_cast<unsigned>(g_reorder_drain));
    }
    else
    {
        ProxyLog("InstallNetcodeHooks: WSARecvFrom not found in game IAT"
                 " - OOO reorder will NOT be applied");
    }

    // Also patch closesocket to reset reorder state when socket closes
    void* savedRealClosesocket = nullptr;
    bool patchedClosesocket = PatchIAT(exe, "WS2_32.dll", "closesocket", 3,
        reinterpret_cast<void*>(Hooked_closesocket), &savedRealClosesocket);
    if (!patchedClosesocket)
    {
        patchedClosesocket = PatchIAT(exe, "ws2_32.dll", "closesocket", 3,
            reinterpret_cast<void*>(Hooked_closesocket), &savedRealClosesocket);
    }

    if (patchedClosesocket)
    {
        if (savedRealClosesocket) g_realClosesocket = reinterpret_cast<PFN_closesocket>(savedRealClosesocket);
        ProxyLog("InstallNetcodeHooks: closesocket IAT patched OK");
    }

    // Patch setsockopt so the game cannot shrink our enlarged buffers back
    // down (it re-sets SO_SNDBUF=32768 on real Windows) and so DSCP survives.
    void* savedRealSetsockopt = nullptr;
    bool patchedSetsockopt = PatchIAT(exe, "WS2_32.dll", "setsockopt", 21,
        reinterpret_cast<void*>(Hooked_setsockopt), &savedRealSetsockopt);
    if (!patchedSetsockopt)
    {
        patchedSetsockopt = PatchIAT(exe, "ws2_32.dll", "setsockopt", 21,
            reinterpret_cast<void*>(Hooked_setsockopt), &savedRealSetsockopt);
    }

    if (patchedSetsockopt)
    {
        if (savedRealSetsockopt) g_realSetsockopt = reinterpret_cast<PFN_setsockopt>(savedRealSetsockopt);
        ProxyLog("InstallNetcodeHooks: setsockopt IAT patched OK"
                 "  re-force SO_SNDBUF=%d SO_RCVBUF=%d DSCP=%u",
                 kTargetSndBuf, kTargetRcvBuf, g_dscp);
    }
    else
    {
        ProxyLog("InstallNetcodeHooks: setsockopt not found in game IAT"
                 " - game may shrink buffers back on real Windows");
    }

    // Patch sendto for opt-in outbound duplication (passthrough when disabled).
    void* savedRealSendto = nullptr;
    bool patchedSendto = PatchIAT(exe, "WS2_32.dll", "sendto", 20,
        reinterpret_cast<void*>(Hooked_sendto), &savedRealSendto);
    if (!patchedSendto)
    {
        patchedSendto = PatchIAT(exe, "ws2_32.dll", "sendto", 20,
            reinterpret_cast<void*>(Hooked_sendto), &savedRealSendto);
    }

    if (patchedSendto)
    {
        if (savedRealSendto) g_realSendto = reinterpret_cast<PFN_sendto>(savedRealSendto);
        ProxyLog("InstallNetcodeHooks: sendto IAT patched OK  send_dup=%s",
                 g_send_dup ? "enabled" : "disabled");
    }

    // The game's own P2P sends go through WSASendTo (sendto is not in its
    // IAT at all), so this is the patch that makes BZ_SEND_DUP effective.
    void* savedRealWSASendTo = nullptr;
    bool patchedWSASendTo = PatchIAT(exe, "WS2_32.dll", "WSASendTo", 0,
        reinterpret_cast<void*>(Hooked_WSASendTo), &savedRealWSASendTo);
    if (!patchedWSASendTo)
    {
        patchedWSASendTo = PatchIAT(exe, "ws2_32.dll", "WSASendTo", 0,
            reinterpret_cast<void*>(Hooked_WSASendTo), &savedRealWSASendTo);
    }

    if (patchedWSASendTo)
    {
        if (savedRealWSASendTo) g_realWSASendTo = reinterpret_cast<PFN_WSASendTo>(savedRealWSASendTo);
        ProxyLog("InstallNetcodeHooks: WSASendTo IAT patched OK  send_dup=%s"
                 "  dup_delay_ms=%u dup_max_pps=%u loopback_dup=skip",
                 g_send_dup ? "enabled" : "disabled",
                 g_dup_delay_ms, g_dup_max_pps);
    }
    else if (g_send_dup && !patchedSendto)
    {
        ProxyLog("InstallNetcodeHooks: neither WSASendTo nor sendto found in game IAT"
                 " - BZ_SEND_DUP will NOT be applied");
    }

    // Start the wake thread only if the reorder hook is actually in place.
    if (patchedRecvFrom && g_reorder_enabled && g_wake_enabled && g_wake_thread == nullptr)
    {
        g_wake_thread = CreateThread(nullptr, 0, ReorderWakeThread, nullptr, 0, nullptr);
        if (g_wake_thread == nullptr)
        {
            ProxyLog("InstallNetcodeHooks: wake thread creation failed (err=%lu)", GetLastError());
        }
    }

    // Opt-in governor scanner (read-only diagnostic).
    if (g_gov_scan && g_gov_thread == nullptr)
    {
        g_gov_thread = CreateThread(nullptr, 0, GovernorScanThread, nullptr, 0, nullptr);
        if (g_gov_thread == nullptr)
        {
            ProxyLog("InstallNetcodeHooks: governor scan thread creation failed (err=%lu)", GetLastError());
        }
    }

    // Opt-in governor cold-start patch (data-only; DRM-safe).
    if (g_gov_start != 0 && g_gov_patch_thread == nullptr)
    {
        g_gov_patch_thread = CreateThread(nullptr, 0, GovernorPatchThread, nullptr, 0, nullptr);
        if (g_gov_patch_thread == nullptr)
        {
            ProxyLog("InstallNetcodeHooks: governor patch thread creation failed (err=%lu)", GetLastError());
        }
    }

    // Opt-in AutoKick threshold override (data-only; DRM-safe).
    if ((g_ak_time || g_ak_ping || g_ak_loss || g_ak_start) && g_ak_patch_thread == nullptr)
    {
        g_ak_patch_thread = CreateThread(nullptr, 0, AutoKickPatchThread, nullptr, 0, nullptr);
        if (g_ak_patch_thread == nullptr)
        {
            ProxyLog("InstallNetcodeHooks: autokick patch thread creation failed (err=%lu)", GetLastError());
        }
    }

    // Start the dup pacer only when delayed duplication can actually fire.
    if (g_send_dup && g_dup_delay_ms > 0
        && (patchedWSASendTo || patchedSendto) && g_dup_thread == nullptr)
    {
        g_dup_thread = CreateThread(nullptr, 0, DupPacerThread, nullptr, 0, nullptr);
        if (g_dup_thread == nullptr)
        {
            ProxyLog("InstallNetcodeHooks: dup pacer thread creation failed (err=%lu)"
                     " - falling back to back-to-back duplicates", GetLastError());
            g_dup_delay_ms = 0;
        }
    }
}

void ShutdownNetcodeHooks()
{
    // Signal the wake thread to exit.  Do not wait on it here: this runs
    // under the loader lock during DLL_PROCESS_DETACH and joining a thread
    // would deadlock.  At process exit the thread is gone anyway.
    InterlockedExchange(&g_wake_stop, 1);
    InterlockedExchange(&g_dup_stop, 1);
    InterlockedExchange(&g_gov_stop, 1);
    InterlockedExchange(&g_ak_stop, 1);
    if (g_wake_sender != INVALID_SOCKET && g_realClosesocket != nullptr) {
        g_realClosesocket(g_wake_sender);
        g_wake_sender = INVALID_SOCKET;
    }
    if (g_wake_thread != nullptr) {
        CloseHandle(g_wake_thread);
        g_wake_thread = nullptr;
    }
    if (g_dup_thread != nullptr) {
        CloseHandle(g_dup_thread);
        g_dup_thread = nullptr;
    }
    if (g_gov_thread != nullptr) {
        CloseHandle(g_gov_thread);
        g_gov_thread = nullptr;
    }
    if (g_gov_patch_thread != nullptr) {
        CloseHandle(g_gov_patch_thread);
        g_gov_patch_thread = nullptr;
    }
    if (g_ak_patch_thread != nullptr) {
        CloseHandle(g_ak_patch_thread);
        g_ak_patch_thread = nullptr;
    }

    flush_buffer_log_files();

    if (g_buffer_ring != nullptr) {
        HeapFree(GetProcessHeap(), 0, g_buffer_ring);
        g_buffer_ring = nullptr;
    }
    g_buffer_log_enabled = false;

    if (g_buffer_lock_ready) {
        DeleteCriticalSection(&g_buffer_lock);
        g_buffer_lock_ready = false;
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
