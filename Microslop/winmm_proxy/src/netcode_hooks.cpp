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
static uint32_t          g_reorder_drain       = kReorderDrainCapDef;
static ReorderCtx        g_rx;                            // zero-initialized (BSS)
static CRITICAL_SECTION  g_reorder_cs          = {};
static bool              g_reorder_cs_ready    = false;

// Periodic stats emission.  These counters are the point of V4.7: before them
// every tuning decision was made blind, through the game's own drop counter,
// which cannot see the latency this buffer adds.  BZ_REORDER_STATS=0 silences.
static bool              g_reorder_stats       = true;
static uint64_t          g_stats_last_ms       = 0;
constexpr uint64_t       kReorderStatsMs       = 10000;

// Whether the session summary already reached the log.  hooked_closesocket is
// the normal emit site, but a game that exits without closing its P2P socket
// never reaches it — the 2026-07-26 V4.8 match did exactly that on the Proton
// proxy and lost the whole send measurement.  ShutdownNetcodeHooks re-emits
// when these are still false; they exist so a clean shutdown does not get a
// second, duplicate line.
static bool              g_reorder_stats_logged = false;
static bool              g_send_stats_logged    = false;

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

// The game's whole [Net] tunable block, written directly into .data.  Table,
// addresses, presets and the sanity gate live in shared/net_globals.h; this is
// just the state the poll thread owns.  Version-gated on kGovSig, host-enforced
// for the auto-kick subset, effective on every machine for the governor subset.
// Fixed addresses are identical on Proton and real Windows (base 0x400000, no ASLR).
static NetGlobal         g_net_tbl[kNetGlobalCount];
static volatile LONG     g_net_stop            = 0;
static HANDLE            g_net_patch_thread    = nullptr;

// Outbound burst measurement, and optional smoothing (shared/send_pace.h).
// The measurement is always on: nothing in this project had ever looked at what
// the local machine puts on the wire, even though a peer's retransmit flood is
// the failure that has actually ended matches.  Smoothing is opt-in via
// BZ_SEND_PACE=<bytes/sec> because it trades send latency for burst shape.
static PaceCtx           g_tx;
static CRITICAL_SECTION  g_pace_cs             = {};
static bool              g_pace_cs_ready       = false;
static volatile LONG     g_pace_stop           = 0;
static HANDLE            g_pace_thread         = nullptr;
static uint32_t          g_pace_rate           = 0;   // 0 = measure only
static uint32_t          g_pace_max_ms         = kPaceMaxDelayDef;

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
// ── Send pacer ───────────────────────────────────────────────────────────────
// Sends run while g_pace_cs is held.  The syscall under a lock is deliberate:
// it is what guarantees queued packets leave in the order the game produced
// them, and BZ sends from a single thread so there is nothing to contend with.

// Flush everything queued, in order, ignoring due times.  Caller holds g_pace_cs.
static void pace_flush_locked() {
    PaceEntry e;
    while (pace_pop_any(&g_tx, &e)) {
        if (g_realSendto != nullptr) {
            g_realSendto((SOCKET)e.sock, (const char *)e.data, (int)e.len, 0,
                         (const sockaddr *)e.to, e.tolen);
        }
    }
}

// Offer a datagram to the pacer.  Returns true when the pacer has taken
// ownership and the caller must report success WITHOUT sending; false when the
// caller should perform the real send itself (always the case while pacing is
// off, so the measurement costs a lock and nothing else).
static bool pace_take(SOCKET s, const uint8_t *data, uint32_t len,
                      const sockaddr *to, int tolen) {
    if (!g_pace_cs_ready || g_realSendto == nullptr || data == nullptr
        || to == nullptr || tolen <= 0 || len == 0 || len > kReorderMaxPktBytes
        || (uint32_t)tolen > kPaceAddrBytes) {
        return false;
    }

    const uint64_t now = GetTickCount64();
    uint64_t due = 0;

    EnterCriticalSection(&g_pace_cs);
    PaceDecision d = pace_admit(&g_tx, len, now, &due);
    if (d == kPaceQueued) {
        if (pace_enqueue(&g_tx, (uintptr_t)s, to, tolen, data, len, due)) {
            LeaveCriticalSection(&g_pace_cs);
            return true;
        }
        d = kPaceFlushThenSend;   // could not store it; give up on shaping
    }
    if (d == kPaceFlushThenSend) {
        pace_flush_locked();
    }
    LeaveCriticalSection(&g_pace_cs);
    return false;
}

// Releases queued datagrams as they come due, and closes measurement windows
// so a burst that ends the traffic still gets counted.
static DWORD WINAPI SendPaceThread(LPVOID) {
    while (InterlockedCompareExchange(&g_pace_stop, 0, 0) == 0) {
        // While pacing is off there is nothing to release, so tick lazily —
        // just often enough to roll the one-second stats window.
        Sleep(g_pace_rate ? kPaceTickMs : 250);
        if (!g_pace_cs_ready) {
            continue;
        }
        const uint64_t now = GetTickCount64();
        EnterCriticalSection(&g_pace_cs);
        pace_tick(&g_tx, now);
        PaceEntry e;
        while (pace_pop_due(&g_tx, now, &e)) {
            if (g_realSendto != nullptr) {
                g_realSendto((SOCKET)e.sock, (const char *)e.data, (int)e.len, 0,
                             (const sockaddr *)e.to, e.tolen);
            }
        }
        LeaveCriticalSection(&g_pace_cs);
    }
    return 0;
}

// ── IOCP (overlapped) receive reordering — Windows only, opt-in ─────────────
//
// On real Windows the game receives through overlapped/IOCP calls, which the
// synchronous drain path deliberately bypasses.  That bypass is why the
// flagship reorder feature does nothing for most installs — and why routing
// those calls through the sync path in V4.1 hung the game at the loading
// screen.
//
// Why the obvious "safe" design does not work.  The tempting approach is to
// never delay anything: let every completion through, and merely swap which
// bytes it carries.  That cannot help.  Whatever we hand the game, the game's
// own BZRNet sequencing advances immediately, and we have no way to un-advance
// it — so by the time a late packet arrives, the cursor has already moved past
// it.  Reordering at the receiver requires *not handing over* the early packet
// yet.  There is no version of this that does not hold something back.
//
// So we hold completions, not buffers.  A completion we hold keeps its own
// OVERLAPPED and its own buffer contents untouched; we only defer the moment
// the game learns it finished.  No data is ever copied between the game's
// buffers, so a mistake here cannot corrupt a packet — the worst case is a
// delayed or out-of-order delivery, which is what the vanilla game already
// gets.  Three bounds keep it from repeating the V4.1 freeze:
//
//   - at most kIocpHoldCap completions are ever held, so the game always has
//     receives outstanding and its completion loop can never be starved dry;
//   - nothing is held past the peer's reorder window, and the wait is taken out
//     of the caller's own timeout budget, so we never block longer than the
//     game asked to block;
//   - a watchdog disables the whole path permanently if a hold ever overruns,
//     falling back to stock behaviour rather than risking a hang.
//
// UNVALIDATED: this has never run against real Windows — the machine this was
// written on has no Windows install, and the game's completion path (whether it
// calls GetQueuedCompletionStatus, the Ex variant, or waits on events) has
// never actually been observed.  Hence BZ_IOCP_SCAN=1, which only logs which
// APIs the game uses, and BZ_IOCP_REORDER=1, which is off by default.  Run the
// scan first; do not enable the reorder path on a machine you are not willing
// to have fail to launch.

constexpr uint32_t kIocpRecvTrack  = 128;   // outstanding overlapped recvs tracked
constexpr uint32_t kIocpHoldCap    = 4;     // completions we may defer at once
constexpr uint32_t kIocpWatchdogX  = 5;     // overrun multiple that trips the watchdog

struct IocpRecv {                  // an overlapped WSARecvFrom the game posted
    LPWSAOVERLAPPED ov;
    SOCKET          s;
    char           *buf;
    uint32_t        buflen;
    sockaddr       *from;
    LPINT           fromlen;
};

struct IocpHeld {                  // a completion deferred from the game
    bool         used;
    DWORD        bytes;
    ULONG_PTR    key;
    LPOVERLAPPED ov;
    uint32_t     seq;
    uint64_t     ts;
    sockaddr_in  from;
};

typedef BOOL (WINAPI *PFN_GetQueuedCompletionStatus)(HANDLE, LPDWORD, PULONG_PTR, LPOVERLAPPED*, DWORD);
static PFN_GetQueuedCompletionStatus g_realGQCS = nullptr;

static bool              g_iocp_scan       = false;
static bool              g_iocp_reorder    = false;
static volatile LONG     g_iocp_disabled   = 0;   // watchdog latch
static IocpRecv          g_iocp_recvs[kIocpRecvTrack];
static IocpHeld          g_iocp_held[kIocpHoldCap];
static CRITICAL_SECTION  g_iocp_cs         = {};
static bool              g_iocp_cs_ready   = false;
static uint64_t          g_iocp_posted     = 0;   // overlapped recvs seen
static uint64_t          g_iocp_completed  = 0;
static uint64_t          g_iocp_deferred   = 0;
static uint64_t          g_iocp_inorder    = 0;
static uint64_t          g_iocp_forced     = 0;
static bool              g_iocp_gqcs_logged = false;

// Remember where an overlapped receive will land, so its completion can be
// classified.  Called from the WSARecvFrom hook; never alters the call.
static void IocpTrackRecv(LPWSAOVERLAPPED ov, SOCKET s, LPWSABUF buffers,
                          DWORD buffer_count, sockaddr *from, LPINT fromlen) {
    if (!g_iocp_cs_ready || ov == nullptr || buffers == nullptr || buffer_count == 0) {
        return;
    }
    EnterCriticalSection(&g_iocp_cs);
    g_iocp_posted++;
    int slot = -1;
    for (uint32_t i = 0; i < kIocpRecvTrack; ++i) {
        if (g_iocp_recvs[i].ov == ov) { slot = (int)i; break; }
        if (slot < 0 && g_iocp_recvs[i].ov == nullptr) { slot = (int)i; }
    }
    if (slot >= 0) {
        g_iocp_recvs[slot].ov      = ov;
        g_iocp_recvs[slot].s       = s;
        g_iocp_recvs[slot].buf     = buffers[0].buf;
        g_iocp_recvs[slot].buflen  = (uint32_t)buffers[0].len;
        g_iocp_recvs[slot].from    = from;
        g_iocp_recvs[slot].fromlen = fromlen;
    }
    LeaveCriticalSection(&g_iocp_cs);
}

// Caller holds g_iocp_cs.
static IocpRecv *IocpFindRecv(LPOVERLAPPED ov) {
    for (uint32_t i = 0; i < kIocpRecvTrack; ++i) {
        if (g_iocp_recvs[i].ov == (LPWSAOVERLAPPED)ov) {
            return &g_iocp_recvs[i];
        }
    }
    return nullptr;
}

// Hand a held completion back to the caller.  Its buffer was never touched, so
// this is purely "the game learns about it now instead of earlier".
static void IocpRelease(IocpHeld *h, LPDWORD bytes, PULONG_PTR key,
                        LPOVERLAPPED *ov, uint64_t now, int kind) {
    *bytes = h->bytes;
    *key   = h->key;
    *ov    = h->ov;
    h->used = false;

    PeerBuf *pb = reorder_get_peer(&g_rx, h->from, now);
    if (pb != nullptr) {
        reorder_note_delivery(&g_rx, pb, h->seq, h->ts, now, kind);
    }
}

// Pick a held completion that is ready: the in-order successor first, else one
// that has waited out its peer's window.  Caller holds g_iocp_cs.
static IocpHeld *IocpPickReady(uint64_t now, int *kind_out) {
    IocpHeld *oldest = nullptr;
    for (uint32_t i = 0; i < kIocpHoldCap; ++i) {
        IocpHeld *h = &g_iocp_held[i];
        if (!h->used) {
            continue;
        }
        PeerBuf *pb = reorder_get_peer(&g_rx, h->from, now);
        if (pb != nullptr && pb->seq_init && h->seq == pb->last_seq + 1) {
            *kind_out = kDeliverInOrder;
            return h;
        }
        uint32_t hold = (pb != nullptr) ? pb->win_ms : g_rx.win_min_ms;
        if (g_rx.max_hold_ms != 0 && hold > g_rx.max_hold_ms) {
            hold = g_rx.max_hold_ms;
        }
        if (now - h->ts >= hold && (oldest == nullptr || h->ts < oldest->ts)) {
            oldest = h;
        }
    }
    if (oldest != nullptr) {
        *kind_out = kDeliverForced;
        return oldest;
    }
    return nullptr;
}

// Release the oldest held completion regardless of its window.  Used when the
// caller's timeout expires: better an out-of-order delivery than a lost one.
static IocpHeld *IocpPickOldest() {
    IocpHeld *oldest = nullptr;
    for (uint32_t i = 0; i < kIocpHoldCap; ++i) {
        if (g_iocp_held[i].used
            && (oldest == nullptr || g_iocp_held[i].ts < oldest->ts)) {
            oldest = &g_iocp_held[i];
        }
    }
    return oldest;
}

static void IocpDisable(const char *why) {
    if (InterlockedExchange(&g_iocp_disabled, 1) == 0) {
        ProxyLog("iocp_reorder: DISABLED - %s. Falling back to stock overlapped "
                 "behaviour for the rest of this run.", why);
    }
}

static BOOL WINAPI Hooked_GetQueuedCompletionStatus(HANDLE port, LPDWORD bytes,
                                                    PULONG_PTR key,
                                                    LPOVERLAPPED *ov,
                                                    DWORD timeout) {
    if (g_realGQCS == nullptr) {
        return FALSE;
    }

    if (g_iocp_scan && !g_iocp_gqcs_logged) {
        g_iocp_gqcs_logged = true;
        ProxyLog("iocp_scan: the game calls GetQueuedCompletionStatus"
                 " (overlapped recvs posted so far: %llu)",
                 (unsigned long long)g_iocp_posted);
    }

    if (!g_iocp_reorder || !g_iocp_cs_ready || bytes == nullptr
        || key == nullptr || ov == nullptr
        || InterlockedCompareExchange(&g_iocp_disabled, 0, 0) != 0) {
        return g_realGQCS(port, bytes, key, ov, timeout);
    }

    const uint64_t entered  = GetTickCount64();
    // Never block longer than the caller asked to.  INFINITE is capped at the
    // window so a held packet always gets released.
    const uint32_t budget   = (timeout == INFINITE)
                              ? g_rx.win_max_ms
                              : ((timeout < g_rx.win_max_ms) ? timeout : g_rx.win_max_ms);
    const uint64_t deadline = entered + budget;

    for (;;) {
        uint64_t now = GetTickCount64();

        // 1. Anything held that is ready to go out?
        EnterCriticalSection(&g_iocp_cs);
        int kind = kDeliverInOrder;
        IocpHeld *ready = IocpPickReady(now, &kind);
        if (ready != nullptr) {
            IocpRelease(ready, bytes, key, ov, now, kind);
            if (kind == kDeliverInOrder) g_iocp_inorder++; else g_iocp_forced++;
            LeaveCriticalSection(&g_iocp_cs);
            return TRUE;
        }
        LeaveCriticalSection(&g_iocp_cs);

        // 2. Ask for a real completion with whatever budget is left.
        const DWORD remaining = (now >= deadline) ? 0
                                : (DWORD)(deadline - now);
        DWORD        b = 0;
        ULONG_PTR    k = 0;
        LPOVERLAPPED o = nullptr;
        const BOOL   rc = g_realGQCS(port, &b, &k, &o, remaining);

        if (o == nullptr) {
            // Timed out with nothing pending.  Release the oldest held rather
            // than leave it stranded, then report the timeout next time.
            EnterCriticalSection(&g_iocp_cs);
            IocpHeld *old = IocpPickOldest();
            if (old != nullptr) {
                IocpRelease(old, bytes, key, ov, GetTickCount64(), kDeliverForced);
                g_iocp_forced++;
                LeaveCriticalSection(&g_iocp_cs);
                return TRUE;
            }
            LeaveCriticalSection(&g_iocp_cs);
            *bytes = b; *key = k; *ov = o;
            return rc;
        }

        if (!rc) {
            // A failed operation: pass it straight through untouched.
            EnterCriticalSection(&g_iocp_cs);
            IocpRecv *r = IocpFindRecv(o);
            if (r != nullptr) r->ov = nullptr;
            LeaveCriticalSection(&g_iocp_cs);
            *bytes = b; *key = k; *ov = o;
            return rc;
        }

        EnterCriticalSection(&g_iocp_cs);
        IocpRecv *r = IocpFindRecv(o);
        if (r == nullptr || b < kReorderSeqMinPay || r->buf == nullptr
            || r->from == nullptr || r->from->sa_family != AF_INET) {
            // Not one of our receives (a send completion, say), or too short to
            // carry a sequence: hand it over unchanged.
            if (r != nullptr) r->ov = nullptr;
            LeaveCriticalSection(&g_iocp_cs);
            *bytes = b; *key = k; *ov = o;
            return rc;
        }

        g_iocp_completed++;
        uint32_t seq = 0;
        seq = reorder_seq_from_payload(reinterpret_cast<const uint8_t *>(r->buf));
        const sockaddr_in src = *reinterpret_cast<sockaddr_in *>(r->from);
        r->ov = nullptr;

        now = GetTickCount64();
        PeerBuf *pb = reorder_get_peer(&g_rx, src, now);

        // In order, or nothing to reorder against: straight through.
        if (pb == nullptr || !pb->seq_init || seq == pb->last_seq + 1) {
            if (pb != nullptr) {
                reorder_note_delivery(&g_rx, pb, seq, now, now,
                                      pb->seq_init ? kDeliverInOrder : kDeliverFirst);
            }
            g_iocp_inorder++;
            LeaveCriticalSection(&g_iocp_cs);
            *bytes = b; *key = k; *ov = o;
            return rc;
        }

        // Already superseded: the game would discard it as stale anyway, and
        // holding it helps nobody.  Pass it on without moving the cursor.
        if (seq_cmp(seq, pb->last_seq) <= 0) {
            g_rx.stats.dropped_stale++;
            LeaveCriticalSection(&g_iocp_cs);
            *bytes = b; *key = k; *ov = o;
            return rc;
        }

        // Ahead of the gap: defer it if there is room, otherwise let it through.
        reorder_adapt_on_arrival(&g_rx, pb, seq, now);
        IocpHeld *slot = nullptr;
        for (uint32_t i = 0; i < kIocpHoldCap; ++i) {
            if (!g_iocp_held[i].used) { slot = &g_iocp_held[i]; break; }
        }
        if (slot == nullptr) {
            reorder_note_delivery(&g_rx, pb, seq, now, now, kDeliverForced);
            g_iocp_forced++;
            LeaveCriticalSection(&g_iocp_cs);
            *bytes = b; *key = k; *ov = o;
            return rc;
        }
        slot->used  = true;
        slot->bytes = b;
        slot->key   = k;
        slot->ov    = o;
        slot->seq   = seq;
        slot->ts    = now;
        slot->from  = src;
        g_iocp_deferred++;
        LeaveCriticalSection(&g_iocp_cs);

        // Watchdog: if we have somehow spent far longer here than the window
        // allows, stop trying and never come back.
        if (GetTickCount64() - entered > (uint64_t)budget * kIocpWatchdogX) {
            IocpDisable("a completion was held past the watchdog limit");
            *bytes = 0; *key = 0; *ov = nullptr;
            return g_realGQCS(port, bytes, key, ov, 0);
        }
        // Loop: look for something releasable, or take another completion.
    }
}

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

// Hand one datagram to the caller's WSARecvFrom arguments.  Every exit from
// the reorder path goes through here: buffered release, short/non-IPv4
// pass-through, peer-table-full fallback, and eviction overflow.  Must be
// called with g_reorder_cs released — buffer_log_event takes its own lock.
static int DeliverToCaller(SOCKET s,
                           LPWSABUF buffers, DWORD buffer_count,
                           LPDWORD bytes_received, LPDWORD inout_flags,
                           sockaddr *from, LPINT fromlen,
                           const sockaddr_in &src,
                           const uint8_t *data, uint32_t len)
{
    uint32_t copied = scatter_copy(buffers, buffer_count, data, len);
    if (bytes_received != nullptr) *bytes_received = copied;
    if (inout_flags != nullptr) *inout_flags = 0;
    if (from != nullptr && fromlen != nullptr) {
        int sa = (*fromlen < static_cast<int>(sizeof(src)))
                 ? *fromlen : static_cast<int>(sizeof(src));
        if (sa > 0) std::memcpy(from, &src, static_cast<size_t>(sa));
        *fromlen = static_cast<int>(sizeof(src));
    }

    if (g_buffer_log_enabled) {
        uint32_t requested = 0;
        for (DWORD i = 0; i < buffer_count && buffers != nullptr; ++i) {
            requested += buffers[i].len;
        }
        uint16_t pay_len = static_cast<uint16_t>((copied < g_buffer_payload_bytes) ? copied : g_buffer_payload_bytes);
        const uint8_t *pay = (pay_len > 0 && buffers != nullptr && buffers[0].buf != nullptr)
                             ? reinterpret_cast<const uint8_t *>(buffers[0].buf) : nullptr;
        buffer_log_event(kEventTypeWSARecvFrom, s,
                         reinterpret_cast<const sockaddr *>(&src),
                         0, requested, copied, 0u, pay, pay_len);
    }

    WSASetLastError(0);
    return 0;
}

// Emit the reorder counters every kReorderStatsMs.  Formatting is pure
// snprintf so it is cheap enough to do under the lock; the file write is not,
// so ProxyLog runs after the lock is released.
static void MaybeLogReorderStats(uint64_t now_ms)
{
    if (!g_reorder_stats || !g_reorder_cs_ready) {
        return;
    }
    char line[1024];
    bool due = false;
    EnterCriticalSection(&g_reorder_cs);
    if (now_ms - g_stats_last_ms >= kReorderStatsMs) {
        g_stats_last_ms = now_ms;
        due = reorder_format_stats(&g_rx, line, sizeof(line)) > 0;
    }
    LeaveCriticalSection(&g_reorder_cs);
    if (!due) {
        return;
    }
    ProxyLog("%s", line);

    // Outbound counters share the cadence: an A/B is only readable when the
    // receive and send sides are timestamped together.
    if (g_pace_cs_ready) {
        char pline[512];
        EnterCriticalSection(&g_pace_cs);
        pace_tick(&g_tx, now_ms);
        const bool ok = pace_format_stats(&g_tx, pline, sizeof(pline)) > 0;
        LeaveCriticalSection(&g_pace_cs);
        if (ok) {
            ProxyLog("%s", pline);
        }
    }

    if (g_iocp_scan || g_iocp_reorder) {
        EnterCriticalSection(&g_iocp_cs);
        const unsigned long long posted = g_iocp_posted, completed = g_iocp_completed,
                                 deferred = g_iocp_deferred, inorder = g_iocp_inorder,
                                 forced = g_iocp_forced;
        LeaveCriticalSection(&g_iocp_cs);
        ProxyLog("iocp_stats: overlapped_recvs_posted=%llu classified=%llu"
                 " in_order=%llu deferred=%llu forced=%llu reorder=%s",
                 posted, completed, inorder, deferred, forced,
                 (InterlockedCompareExchange(&g_iocp_disabled, 0, 0) != 0)
                     ? "disabled-by-watchdog"
                     : (g_iocp_reorder ? "on" : "off"));
    }
}

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
        // Record where an overlapped receive will land before issuing it, so
        // its completion can be classified later.  Purely observational: the
        // call itself is untouched, and this runs even with the IOCP reorder
        // path disabled so BZ_IOCP_SCAN can report what the game does.
        if (ov != nullptr && cr == nullptr && (g_iocp_reorder || g_iocp_scan)) {
            IocpTrackRecv(ov, s, buffers, buffer_count, from, fromlen);
        }
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

    // Drain loop: pull datagrams the kernel already has into per-peer queues,
    // then deliver the best in-order candidate.
    //
    // The drain stops as soon as any peer's ring fills.  Before V4.7 it pulled
    // up to 96 datagrams into 8-slot rings and *discarded* the overflow, so
    // under exactly the bursts this buffer is meant to help it destroyed
    // packets the vanilla game would have received.  Undrained datagrams now
    // simply stay in the 4 MB kernel receive buffer, in order, costing nothing.
    uint8_t     drain_buf[kReorderMaxPktBytes];
    sockaddr_in drain_src;

    uint32_t drained = 0;
    for (uint32_t drain_count = 0; drain_count < g_reorder_drain; ++drain_count) {
        if (reorder_drain_saturated(&g_rx)) {
            break;  // no room left; deliver what we have and come back
        }

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
        if (drc != 0) {
            if (static_cast<int>(WSAGetLastError()) == WSAEMSGSIZE) {
                // Datagram larger than our drain buffer.  The stack has already
                // consumed and truncated it, so treating this as "socket empty"
                // would strand everything queued behind it.  Count it instead
                // and keep draining; a non-zero emsgsize in the stats means
                // kReorderMaxPktBytes is too small for this game's traffic.
                ++drained;
                g_rx.stats.emsgsize++;
                continue;
            }
            break;  // socket drained (WSAEWOULDBLOCK) or a real error
        }
        if (drain_bytes == 0) {
            break;
        }
        ++drained;

        // Discard our own wake datagrams (see wake thread): they exist only
        // to mark the socket readable and must never reach the game.
        if (drain_bytes == sizeof(kWakeMagic)
            && std::memcmp(drain_buf, kWakeMagic, sizeof(kWakeMagic)) == 0) {
            continue;
        }

        // Packets too short for a sequence field, or from non-IPv4 sources,
        // cannot be reordered: deliver the first such packet immediately.
        if (drain_src.sin_family != AF_INET || drain_bytes < kReorderSeqMinPay) {
            g_rx.stats.bypass_short++;
            LeaveCriticalSection(&g_reorder_cs);
            return DeliverToCaller(s, buffers, buffer_count, bytes_received, inout_flags,
                                   from, fromlen, drain_src, drain_buf, drain_bytes);
        }

        uint32_t seq = 0;
        seq = reorder_seq_from_payload(reinterpret_cast<const uint8_t *>(drain_buf));

        uint64_t arrival_ms = GetTickCount64();
        PeerBuf *pb = reorder_get_peer(&g_rx, drain_src, arrival_ms);
        if (pb == nullptr) {
            // Every peer entry is live and none is idle enough to reclaim:
            // pass this packet straight through rather than buffering it.
            g_rx.stats.bypass_table_full++;
            LeaveCriticalSection(&g_reorder_cs);
            return DeliverToCaller(s, buffers, buffer_count, bytes_received, inout_flags,
                                   from, fromlen, drain_src, drain_buf, drain_bytes);
        }

        reorder_adapt_on_arrival(&g_rx, pb, seq, arrival_ms);

        ReorderSlot evicted;
        InsertResult ins = reorder_insert(&g_rx, pb, seq, arrival_ms, drain_src,
                                          drain_buf, drain_bytes, &evicted);

        // This socket demonstrably carries reorderable traffic: it is the one
        // the wake thread should target, and its polls reset the wake budget.
        g_reorder_sock = s;
        g_last_recv_call_ms = arrival_ms;

        if (ins == kInsertEvicted) {
            // Ring was full: the displaced packet goes to the game now rather
            // than being dropped.  Out of order beats not delivered at all.
            reorder_note_delivery(&g_rx, pb, evicted.seq, evicted.ts, arrival_ms, kDeliverEvicted);
            if (drained > g_rx.stats.max_drain_depth) {
                g_rx.stats.max_drain_depth = drained;
            }
            LeaveCriticalSection(&g_reorder_cs);
            return DeliverToCaller(s, buffers, buffer_count, bytes_received, inout_flags,
                                   from, fromlen, evicted.from, evicted.data, evicted.len);
        }
    }

    if (drained > g_rx.stats.max_drain_depth) {
        g_rx.stats.max_drain_depth = drained;
    }

    uint64_t now_ms  = GetTickCount64();
    uint32_t best_pi = 0;
    int      best_si = -1;
    int      kind    = kDeliverInOrder;
    if (!reorder_next_ready(&g_rx, now_ms, &best_pi, &best_si, &kind)) {
        // Nothing is ready yet: tell the game the socket is empty for now.
        LeaveCriticalSection(&g_reorder_cs);
        MaybeLogReorderStats(now_ms);
        WSASetLastError(WSAEWOULDBLOCK);
        return SOCKET_ERROR;
    }

    ReorderSlot pkt;
    reorder_take(&g_rx, &g_rx.tbl[best_pi], best_si, now_ms, kind, &pkt);

    LeaveCriticalSection(&g_reorder_cs);

    int rc = DeliverToCaller(s, buffers, buffer_count, bytes_received, inout_flags,
                             from, fromlen, pkt.from, pkt.data, pkt.len);
    MaybeLogReorderStats(now_ms);
    return rc;
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
    // it ends the session, so all buffered packets are now stale.  Dump the
    // session's counters first — this is the summary line to compare between
    // A/B runs, and after the reset it would be lost.
    if (g_reorder_cs_ready) {
        char line[1024];
        bool have_stats = false;
        EnterCriticalSection(&g_reorder_cs);
        bool was_reorder_sock = (s == g_reorder_sock);
        if (was_reorder_sock && g_reorder_stats) {
            have_stats = reorder_format_stats(&g_rx, line, sizeof(line)) > 0;
        }
        reorder_reset(&g_rx);
        if (was_reorder_sock) {
            g_reorder_sock = INVALID_SOCKET;
        }
        LeaveCriticalSection(&g_reorder_cs);
        if (have_stats) {
            ProxyLog("session end: %s", line);
            g_reorder_stats_logged = true;
        }
    }

    dup_purge_socket(s);
    if (g_pace_cs_ready) {
        char pline[512];
        bool have_pace = false;
        EnterCriticalSection(&g_pace_cs);
        pace_flush_locked();                    // do not strand the game's tail
        pace_purge_socket(&g_tx, (uintptr_t)s);
        if (g_reorder_stats) {
            pace_tick(&g_tx, GetTickCount64());
            have_pace = pace_format_stats(&g_tx, pline, sizeof(pline)) > 0;
        }
        LeaveCriticalSection(&g_pace_cs);
        if (have_pace) {
            ProxyLog("session end: %s", pline);
            g_send_stats_logged = true;
        }
    }

    return rc;
}

// Last-chance emit for the session counters, called from ShutdownNetcodeHooks
// on the DLL_PROCESS_DETACH path.
//
// Three things differ from the closesocket path, all forced by running under
// the loader lock at process exit:
//   - Locks are taken with TryEnterCriticalSection.  The worker threads are
//     already terminated by this point, so a section one of them still owned
//     will never be released and EnterCriticalSection would hang the exit.
//   - The pacer queue is not flushed.  Sending is not safe once ws2_32 may
//     have unwound, and the counters are what we came for, not the tail.
//   - Nothing is reset afterwards.  The process is going away regardless, and
//     reorder_reset/pace_purge_socket would only touch state nobody reads.
//
// The "session end: " prefix is kept byte-identical to the closesocket path so
// tools/analyze_drops.py keeps matching it; provenance goes on its own line.
static void emit_session_stats_at_exit()
{
    if (!g_reorder_stats) {
        return;                                 // BZ_REORDER_STATS=0
    }
    if (g_reorder_stats_logged && g_send_stats_logged) {
        return;                                 // clean shutdown already did it
    }

    bool noted = false;
    auto note = [&noted]() {
        if (!noted) {
            ProxyLog("process exit without closesocket: emitting session"
                     " counters from DLL_PROCESS_DETACH");
            noted = true;
        }
    };

    if (!g_reorder_stats_logged && g_reorder_cs_ready) {
        char line[1024];
        bool have_stats = false;
        if (TryEnterCriticalSection(&g_reorder_cs)) {
            have_stats = reorder_format_stats(&g_rx, line, sizeof(line)) > 0;
            LeaveCriticalSection(&g_reorder_cs);
        } else {
            note();
            ProxyLog("session end: reorder lock held at exit, reorder_stats lost");
        }
        if (have_stats) {
            note();
            ProxyLog("session end: %s", line);
            g_reorder_stats_logged = true;
        }
    }

    if (!g_send_stats_logged && g_pace_cs_ready) {
        char pline[512];
        bool have_pace = false;
        if (TryEnterCriticalSection(&g_pace_cs)) {
            pace_tick(&g_tx, GetTickCount64());
            have_pace = pace_format_stats(&g_tx, pline, sizeof(pline)) > 0;
            LeaveCriticalSection(&g_pace_cs);
        } else {
            note();
            ProxyLog("session end: pace lock held at exit, send_stats lost");
        }
        if (have_pace) {
            note();
            ProxyLog("session end: %s", pline);
            g_send_stats_logged = true;
        }
    }
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

    // Measure (always) and optionally pace.  When the pacer takes ownership the
    // game is told the send succeeded, which is what a UDP send means anyway:
    // handed off, no delivery promise.
    int  rc;
    bool paced = (flags == 0 && len > 0
                  && pace_take(s, (const uint8_t *)buf, (uint32_t)len, to, tolen));
    if (paced) {
        rc = len;
    } else {
        rc = g_realSendto(s, buf, len, flags, to, tolen);
    }

    // Duplicate only IPv4 datagrams large enough to carry a BZRNet sequence
    // field: control/wake packets stay single-shot.  The first call's result
    // and error state are what the game sees.
    // Skip duplication when the pacer owns the packet: the original has not
    // left yet, so a copy sent now would arrive first and be seen as a reorder.
    if (g_send_dup && !paced && rc >= 0 && buf != nullptr && to != nullptr
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

    // Measure (always) and optionally pace.  Only the synchronous path: an
    // overlapped send is the caller's to complete, and taking ownership of one
    // would mean completing its OVERLAPPED ourselves — the same class of
    // mistake that froze the game on the receive side in V4.1.
    int rc = 0;
    bool paced = false;
    if (ov == nullptr && cr == nullptr && flags == 0
        && to != nullptr && buffers != nullptr && buffer_count > 0) {
        uint8_t  flat[kReorderMaxPktBytes];
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
        if (fits && total > 0 && pace_take(s, flat, total, to, tolen)) {
            if (bytes_sent != nullptr) {
                *bytes_sent = total;
            }
            paced = true;
        }
    }
    if (!paced) {
        rc = g_realWSASendTo(s, buffers, buffer_count, bytes_sent, flags, to, tolen, ov, cr);
    }
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
        for (uint32_t i = 0; i < g_rx.peers; ++i) {
            if (g_rx.tbl[i].key != 0 && g_rx.tbl[i].filled > 0) {
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

// Write the game's [Net] tunables straight into .data.
//
// Same DRM-safe strategy as GovernorPatchThread and for the same reason: a
// `.text` rewrite was verified to apply and then trip SteamStub's integrity
// check, while `.data` carries no such check and aligned 32-bit stores are
// atomic on x86.  The session parser rewrites these globals at every match
// start (from net.ini, or the stock default when net.ini is found-but-not-
// applied, which is the observed behaviour), so we re-assert on a poll loop:
// within one tick of any match starting, our values win.
//
// The build is confirmed via the unique kGovSig scan before the fixed addresses
// are trusted, and each entry is sanity-gated against a plausible range in
// net_globals.h — a wrong address is vetoed and logged rather than written blind.
static DWORD WINAPI NetPatchThread(LPVOID)
{
    if (!net_globals_any(g_net_tbl, kNetGlobalCount)) {
        return 0;
    }
    // Let SteamStub decrypt .text first (well before any match starts).
    Sleep(15000);

    BYTE *text = nullptr;
    size_t text_size = 0;
    if (!FindSection(".text", &text, &text_size)) {
        ProxyLog("net_patch: .text section not found");
        return 0;
    }

    int matches = 0;
    for (size_t i = 0; i + sizeof(kGovSig) <= text_size; ++i) {
        if (std::memcmp(text + i, kGovSig, sizeof(kGovSig)) == 0) {
            if (++matches > 1) break;
        }
    }
    if (matches != 1) {
        ProxyLog("net_patch: %d version signature matches (need exactly 1) - "
                 "disabled. Game version may have changed; re-run BZ_GOV_SCAN.", matches);
        return 0;
    }

    ProxyLog("net_patch: version confirmed; asserting [Net] globals every %ums "
             "(re-applied at every match start; auto-kick entries are host-enforced)",
             (unsigned)kGovPollMs);

    while (InterlockedCompareExchange(&g_net_stop, 0, 0) == 0) {
        if (net_globals_apply(g_net_tbl, kNetGlobalCount) > 0) {
            for (size_t i = 0; i < kNetGlobalCount; ++i) {
                NetGlobal &g = g_net_tbl[i];
                if (!g.changed) {
                    continue;
                }
                g.changed = 0;
                if (g.state == kNgVetoed) {
                    ProxyLog("net_patch: %s VETOED - 0x%08lx holds %u, outside the "
                             "plausible range %u..%u (stock is %u). Address is wrong "
                             "for this build; not writing it.",
                             g.ini_key, (unsigned long)g.va, (unsigned)g.seen,
                             (unsigned)g.lo, (unsigned)g.hi, (unsigned)g.stock);
                } else {
                    ProxyLog("net_patch: %s %u -> %u (%s, stock %u)",
                             g.ini_key, (unsigned)g.seen, (unsigned)g.want,
                             g.env, (unsigned)g.stock);
                }
            }
        }
        Sleep(kGovPollMs);
    }
    ProxyLog("net_patch: stopping");
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
        // Reorder is OFF by default since 2026-07-26; BZ_REORDER=1 enables.
        // See the matching note in the Linux proxy: the game's overlapped
        // receives never reach this path, and a live capture measured
        // out-of-order arrivals at 0.0-0.2%.  Kept, tested, and off.
        reorder_init(&g_rx);
        const char *reorder_env = std::getenv("BZ_REORDER");
        g_reorder_enabled = (reorder_env == nullptr || *reorder_env == '\0')
                            ? false : env_truthy(reorder_env);
        const char *adapt_env = std::getenv("BZ_REORDER_ADAPT");
        g_rx.adapt = (adapt_env == nullptr || *adapt_env == '\0')
                     ? true : env_truthy(adapt_env);
        const char *wake_env = std::getenv("BZ_REORDER_WAKE");
        g_wake_enabled = (wake_env == nullptr || *wake_env == '\0')
                         ? true : env_truthy(wake_env);
        const char *stats_env = std::getenv("BZ_REORDER_STATS");
        g_reorder_stats = (stats_env == nullptr || *stats_env == '\0')
                          ? true : env_truthy(stats_env);
        g_rx.win_max_ms  = clamp_u32(parse_env_u32("BZ_REORDER_WINDOW_MS", kReorderDefaultMs), 5, 200);
        g_rx.win_min_ms  = clamp_u32(parse_env_u32("BZ_REORDER_MIN_MS", kReorderMinMsDef), 0, g_rx.win_max_ms);
        // Absolute ceiling on how long any single packet may be held,
        // independent of the adaptive window.  This is the number that bounds
        // the latency the buffer can add to the game's streams — and therefore
        // to the round-trip ping a host measures against AutoKickPing.
        g_rx.max_hold_ms = clamp_u32(parse_env_u32("BZ_REORDER_MAX_HOLD_MS", g_rx.win_max_ms), 0, 500);
        g_rx.depth       = clamp_u32(parse_env_u32("BZ_REORDER_DEPTH", kReorderSlotCap), 1, kReorderSlotCap);
        g_rx.peers       = clamp_u32(parse_env_u32("BZ_REORDER_PEERS", kReorderPeerCap), 1, kReorderPeerCap);
        g_reorder_drain  = clamp_u32(parse_env_u32("BZ_REORDER_DRAIN", kReorderDrainCapDef), 1, kReorderDrainCapMax);
        // Off by default: adds upstream traffic on the P2P socket.
        g_send_dup = env_truthy(std::getenv("BZ_SEND_DUP"));
        g_dup_delay_ms = clamp_u32(parse_env_u32("BZ_DUP_DELAY_MS", kDupDelayMsDef), 0, 500);
        g_dup_max_pps  = clamp_u32(parse_env_u32("BZ_DUP_MAX_PPS", kDupMaxPpsDef), 0, 2000);
        // DSCP class for the P2P socket (0 disables); clamp to the 6-bit field.
        g_dscp = clamp_u32(parse_env_u32("BZ_DSCP", kDscpDefault), 0, 63);
        // Outbound pacing.  Measurement is unconditional; smoothing needs an
        // explicit rate because it trades send latency for burst shape.  The
        // pacer can only absorb BZ_SEND_PACE_MAX_MS worth of budget, so at BZ's
        // rates the 20 ms default shapes very little — read send_stats before
        // raising either knob.
        g_pace_rate   = clamp_u32(parse_env_u32("BZ_SEND_PACE", 0), 0, 10000000);
        g_pace_max_ms = clamp_u32(parse_env_u32("BZ_SEND_PACE_MAX_MS", kPaceMaxDelayDef), 0, 200);
        // IOCP receive path.  The scan is read-only and safe; the reorder path
        // is off by default and has never run against real Windows.
        g_iocp_scan    = env_truthy(std::getenv("BZ_IOCP_SCAN"));
        g_iocp_reorder = env_truthy(std::getenv("BZ_IOCP_REORDER"));
        g_gov_scan = env_truthy(std::getenv("BZ_GOV_SCAN"));
        // Governor cold-start rate (0 = disabled). Clamp to a sane band.
        // Governor cold-start rate.  ON by default since V4.7: the game
        // hardcodes a 4000 B/s start for every match, which starves the opening
        // world-state burst.  Poking MinBandwidth below covers the session-setup
        // copy; this covers the separate hardcoded push.  BZ_GOV_START=0 disables.
        // Raised 16000 -> 40000 on 2026-07-26.  This, not MinBandwidth, is what
        // sets a match's opening send rate (see shared/net_globals.h).  A live
        // match sat at 16000 for 72 s *after* the simulation started, then took
        // 2.4 min to reach 40 kB/s and 4.7 min to reach 80 kB/s, while the
        // governor eventually ran to 91,900-112,700 with no ping-driven cutback
        // and measured peak send was only 64,361 B/s.  The opening was therefore
        // far below what the link demonstrably carried.  Not yet validated at
        // 40000 across a full match; BZ_GOV_START=16000 restores the old value.
        g_gov_start = clamp_u32(parse_env_u32("BZ_GOV_START", 40000), 0, 200000);
        // The whole [Net] block, written straight into .data.  Presets are on by
        // default (BZ_NET_TUNE=0 / BZ_AUTOKICK_RELAX=0 restore stock) and mirror
        // net-ini/net.ini — which encodes the intended tuning but has twice been
        // proven found-but-not-applied by the game.
        net_globals_defaults(g_net_tbl);
        net_globals_configure(g_net_tbl, kNetGlobalCount);
    }
    ProxyLog("governor_patch: %s (BZ_GOV_START=%u; 0=disabled)",
             g_gov_start ? "enabled" : "disabled", g_gov_start);
    {
        char nets[512];
        int p = std::snprintf(nets, sizeof(nets), "net_patch: %s",
                              net_globals_any(g_net_tbl, kNetGlobalCount) ? "enabled" : "disabled");
        for (size_t i = 0; i < kNetGlobalCount && p > 0 && (size_t)p < sizeof(nets); ++i) {
            if (g_net_tbl[i].want == 0) {
                continue;
            }
            p += std::snprintf(nets + p, sizeof(nets) - (size_t)p, " %s=%u",
                               g_net_tbl[i].ini_key, (unsigned)g_net_tbl[i].want);
        }
        ProxyLog("%s (0/absent=leave game value; AutoKick* are host-enforced)", nets);
    }

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
                 "  OOO reorder %s max_window_ms=%u min_window_ms=%u max_hold_ms=%u adapt=%d"
                 " wake=%d stats=%d depth=%u peers=%u drain=%u",
                 g_reorder_enabled ? "enabled" : "DISABLED",
                 static_cast<unsigned>(g_rx.win_max_ms),
                 static_cast<unsigned>(g_rx.win_min_ms),
                 static_cast<unsigned>(g_rx.max_hold_ms),
                 g_rx.adapt ? 1 : 0,
                 g_wake_enabled ? 1 : 0,
                 g_reorder_stats ? 1 : 0,
                 static_cast<unsigned>(g_rx.depth),
                 static_cast<unsigned>(g_rx.peers),
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
    if (!g_pace_cs_ready) {
        InitializeCriticalSection(&g_pace_cs);
        g_pace_cs_ready = true;
    }
    if (!g_iocp_cs_ready) {
        InitializeCriticalSection(&g_iocp_cs);
        g_iocp_cs_ready = true;
    }
    if (g_iocp_scan || g_iocp_reorder) {
        void *savedGQCS = nullptr;
        bool patched = PatchIAT(exe, "KERNEL32.dll", "GetQueuedCompletionStatus", 0,
                                reinterpret_cast<void *>(Hooked_GetQueuedCompletionStatus),
                                &savedGQCS);
        if (!patched) {
            patched = PatchIAT(exe, "KERNELBASE.dll", "GetQueuedCompletionStatus", 0,
                               reinterpret_cast<void *>(Hooked_GetQueuedCompletionStatus),
                               &savedGQCS);
        }
        if (patched && savedGQCS != nullptr) {
            g_realGQCS = reinterpret_cast<PFN_GetQueuedCompletionStatus>(savedGQCS);
            ProxyLog("iocp: GetQueuedCompletionStatus IAT patched OK"
                     "  scan=%d reorder=%s",
                     g_iocp_scan ? 1 : 0,
                     g_iocp_reorder ? "ENABLED (UNVALIDATED - see README)" : "off");
        } else {
            g_iocp_reorder = false;
            ProxyLog("iocp: GetQueuedCompletionStatus not found in the game IAT."
                     " The game does not retrieve completions this way; the IOCP"
                     " reorder path cannot apply. Report this - it tells us which"
                     " completion API to hook instead.");
        }
    }
    if (g_pace_thread == nullptr) {
        pace_init(&g_tx, g_pace_rate, g_pace_max_ms, GetTickCount64());
        ProxyLog("send_pace: %s rate_bps=%u max_delay_ms=%u"
                 " (burst measurement is always on; BZ_SEND_PACE=<bytes/sec> to smooth)",
                 g_pace_rate ? "enabled" : "measure-only", g_pace_rate, g_pace_max_ms);
        g_pace_thread = CreateThread(nullptr, 0, SendPaceThread, nullptr, 0, nullptr);
        if (g_pace_thread == nullptr) {
            ProxyLog("InstallNetcodeHooks: failed to create send pace thread"
                     " - pacing disabled, measurement continues");
            g_pace_rate = 0;
            g_tx.rate_bps = 0;
        }
    }

    if (net_globals_any(g_net_tbl, kNetGlobalCount) && g_net_patch_thread == nullptr)
    {
        g_net_patch_thread = CreateThread(nullptr, 0, NetPatchThread, nullptr, 0, nullptr);
        if (g_net_patch_thread == nullptr)
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
    InterlockedExchange(&g_net_stop, 1);
    InterlockedExchange(&g_pace_stop, 1);
    if (g_wake_sender != INVALID_SOCKET && g_realClosesocket != nullptr) {
        g_realClosesocket(g_wake_sender);
        g_wake_sender = INVALID_SOCKET;
    }
    // Before anything below tears the critical sections down: a game that
    // exited without closing its P2P socket has not written its counters yet,
    // and this is the last place they can be recovered.
    emit_session_stats_at_exit();
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
    if (g_pace_thread != nullptr) {
        CloseHandle(g_pace_thread);
        g_pace_thread = nullptr;
    }
    if (g_iocp_cs_ready) {
        DeleteCriticalSection(&g_iocp_cs);
        g_iocp_cs_ready = false;
    }
    if (g_net_patch_thread != nullptr) {
        CloseHandle(g_net_patch_thread);
        g_net_patch_thread = nullptr;
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
