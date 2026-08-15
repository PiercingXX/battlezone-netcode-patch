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

#include "reorder_core.h"
#include "net_globals.h"
#include "send_pace.h"
#include "send_dampen.h"
#include "net_rtt.h"
#include "gov_trace.h"
#include "buffer_filter.h"

namespace {

using namespace bznet;

// Injected by the Makefile.  A tester's log could not previously say which
// build produced it, which mattered most exactly when it was hardest to check:
// the shipped Windows prebuilt was deliberately a different vintage from the
// source it sat beside.
#ifndef BZ_BUILD_ID
#define BZ_BUILD_ID "unknown (built without -DBZ_BUILD_ID)"
#endif
constexpr char kBuildId[] = BZ_BUILD_ID;

constexpr wchar_t kLogFileName[] = L"dsound_proxy.log";
wchar_t g_log_path[MAX_PATH] = L"dsound_proxy.log";
bool g_log_path_ready = false;

// Serialises log_line.  Initialised at DLL_PROCESS_ATTACH before any of our
// threads exist, and deliberately never deleted — see ShutdownProxy.
CRITICAL_SECTION g_log_cs = {};
volatile LONG    g_log_cs_ready = 0;

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
PeerFilter g_buffer_peer_filter = {};
EnvOutcome g_buffer_ring_outcome = kEnvUsed;
EnvOutcome g_buffer_bytes_outcome = kEnvUsed;
uint32_t g_buffer_stride = static_cast<uint32_t>(sizeof(BufferLogRecordHeader) + kDefaultPayloadBytes);
uint32_t g_buffer_head = 0;
uint32_t g_buffer_count = 0;
uint32_t g_buffer_sequence = 0;
uint64_t g_buffer_total_events = 0;
uint8_t *g_buffer_ring = nullptr;

// ── Per-peer reorder buffer ──────────────────────────────────────────────────
// Enabled by default.  Set BZ_REORDER=0 to disable.  WSARecvFrom packets are
// held in a per-source queue ordered by sequence number and delivered to the
// game in order.
//
// The state machine itself lives in shared/reorder_core.h, shared verbatim with
// the Windows winmm proxy: the two used to carry character-identical copies of
// this logic, which meant every defect existed twice and every fix had to be
// written twice.  It is covered by tests/reorder_test.cpp, which runs natively
// with no game involved.
static bool              g_reorder_enabled  = false;
static uint32_t          g_reorder_drain    = kReorderDrainCapDef;
static ReorderCtx        g_rx;                        // zero-initialized (BSS)
static CRITICAL_SECTION  g_reorder_cs       = {};
static bool              g_reorder_cs_ready = false;

// Periodic stats emission.  These counters are the point of V4.7: before them
// every tuning decision was made blind, through the game's own drop counter,
// which cannot see the latency this buffer adds.  BZ_REORDER_STATS=0 silences.
static bool              g_reorder_stats    = true;
static uint64_t          g_stats_last_ms    = 0;
constexpr uint64_t       kReorderStatsMs    = 10000;

// Whether the session summary already reached the log.  hooked_closesocket is
// the normal emit site, but a game that exits without closing its P2P socket
// never reaches it — the 2026-07-26 V4.8 match did exactly that and lost the
// whole send measurement.  DLL_PROCESS_DETACH re-emits when these are still
// false; they exist so a clean shutdown does not get a second, duplicate line.
static bool              g_reorder_stats_logged = false;
static bool              g_send_stats_logged    = false;
static bool              g_dampen_stats_logged  = false;
// Last pace/dampen lines emitted, for suppressing byte-identical repeats when
// teardown closes several sockets in a row (guarded by g_pace_cs).
static char              g_last_pace_line[512]   = "";
static char              g_last_dampen_line[512] = "";

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

// Outbound burst measurement, and optional smoothing (shared/send_pace.h).
// The measurement is always on: nothing in this project had ever looked at what
// the local machine puts on the wire, even though a peer's retransmit flood is
// the failure that has actually ended matches.  Smoothing is opt-in via
// BZ_SEND_PACE=<bytes/sec> because it trades send latency for burst shape and
// there is no evidence yet that this machine needs it.
static PaceCtx           g_tx;
static CRITICAL_SECTION  g_pace_cs          = {};
static bool              g_pace_cs_ready    = false;

// Per-peer round-trip sampling (shared/net_rtt.h).  ON by default since
// V4.94.  See the header for why the ack field and not the send clock: the
// two machines' clocks are unsynchronised, so only a loop that closes inside
// one clock measures a round trip.  Own lock, because the send half and the
// receive half run on different threads under different locks.
static RttCtx            g_rtt;
static CRITICAL_SECTION  g_rtt_cs           = {};
static bool              g_rtt_cs_ready     = false;
static bool              g_rtt_stats_logged = false;
static volatile LONG     g_pace_stop        = 0;
static uint32_t          g_pace_rate        = 0;   // 0 = measure only
static uint32_t          g_pace_max_ms      = kPaceMaxDelayDef;

// Outbound duplicate suppressor (shared/send_dampen.h).  ON by default since
// V4.94 (BZ_SEND_DAMPEN=0 disables): every reliable message goes out 6-9 times before
// an ack can physically return, and this drops the redundant in-window copies
// on the send path.  The peer table is per-destination; dampen_purge_peer is the
// explicit reset the proxy fires on the disconnect path so a real reconnect is
// signalled rather than inferred.  Guarded by g_pace_cs, not a lock of its own:
// the damper decides before the pacer takes ownership, and one lock keeps that
// ordering obvious.
static DampenCtx         g_dampen;
// The socket whose close ends the dampened session, mirroring g_reorder_sock:
// set on the dampened send path, checked by dampen_close_ends_session in the
// closesocket hook so an unrelated socket close (lobby/discovery, stats
// upload) cannot wipe the peer ring mid-match.  Guarded by g_pace_cs.
static SOCKET            g_dampen_sock      = INVALID_SOCKET;

// The game's whole [Net] tunable block, written directly into .data.  Table,
// addresses, presets and the sanity gate live in shared/net_globals.h; this is
// just the state the poll thread owns.  Version-gated on kGovSig, host-enforced
// for the auto-kick subset, effective on every machine for the governor subset.
static NetGlobal         g_net_tbl[kNetGlobalCount];
static volatile LONG     g_net_stop         = 0;

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

// ── Send pacer ───────────────────────────────────────────────────────────────
// Sends run while g_pace_cs is held.  The syscall under a lock is deliberate:
// it is what guarantees queued packets leave in the order the game produced
// them, and BZ sends from a single thread so there is nothing to contend with.

// Flush everything queued, in order, ignoring due times.  Caller holds g_pace_cs.
static void pace_flush_locked() {
    PaceEntry e;
    while (pace_pop_any(&g_tx, &e)) {
        if (g_real_sendto != nullptr) {
            g_real_sendto(static_cast<SOCKET>(e.sock),
                          reinterpret_cast<const char *>(e.data),
                          static_cast<int>(e.len), 0,
                          reinterpret_cast<const sockaddr *>(e.to), e.tolen);
        }
    }
}

// Offer a datagram to the pacer.  Returns true when the pacer has taken
// ownership and the caller must report success WITHOUT sending; false when the
// caller should perform the real send itself (which is always the case while
// pacing is off, so the measurement costs a lock and nothing else).
static bool pace_take(SOCKET s, const uint8_t *data, uint32_t len,
                      const sockaddr *to, int tolen) {
    if (!g_pace_cs_ready || g_real_sendto == nullptr || data == nullptr
        || to == nullptr || tolen <= 0 || len == 0 || len > kReorderMaxPktBytes
        || static_cast<uint32_t>(tolen) > kPaceAddrBytes) {
        return false;
    }

    const uint64_t now = GetTickCount64();
    uint64_t due = 0;

    EnterCriticalSection(&g_pace_cs);
    PaceDecision d = pace_admit(&g_tx, len, now, &due);
    if (d == kPaceQueued) {
        if (pace_enqueue(&g_tx, static_cast<uintptr_t>(s), to, tolen, data, len, due)) {
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
static DWORD WINAPI send_pace_thread(LPVOID) {
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
            if (g_real_sendto != nullptr) {
                g_real_sendto(static_cast<SOCKET>(e.sock),
                              reinterpret_cast<const char *>(e.data),
                              static_cast<int>(e.len), 0,
                              reinterpret_cast<const sockaddr *>(e.to), e.tolen);
            }
        }
        LeaveCriticalSection(&g_pace_cs);
    }
    return 0;
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

    // Report what we were ASKED for next to what we used.  The one successful
    // capture asked for ring=500000 and ran with the 65536 default, losing 48%
    // of its events including the whole match start, and nothing in the bundle
    // said so — the variable was simply absent because the game was launched
    // before the launch options were pasted.
    const char *raw_bytes = std::getenv("BZ_BUFFER_LOG_BYTES");
    const char *raw_ring  = std::getenv("BZ_BUFFER_LOG_RING");
    const uint32_t want_bytes = parse_env_u32("BZ_BUFFER_LOG_BYTES", kDefaultPayloadBytes);
    const uint32_t want_ring  = parse_env_u32("BZ_BUFFER_LOG_RING", kDefaultRingRecords);
    g_buffer_payload_bytes = clamp_u32(want_bytes, kMinPayloadBytes, kMaxPayloadBytes);
    g_buffer_ring_records = clamp_u32(want_ring, kMinRingRecords, kMaxRingRecords);
    g_buffer_ring_outcome = env_outcome(raw_ring, want_ring, g_buffer_ring_records);
    g_buffer_bytes_outcome = env_outcome(raw_bytes, want_bytes, g_buffer_payload_bytes);
    g_buffer_stride = static_cast<uint32_t>(sizeof(BufferLogRecordHeader) + g_buffer_payload_bytes);

    peer_filter_parse(&g_buffer_peer_filter, std::getenv("BZ_BUFFER_LOG_PEER"));

    size_t total = static_cast<size_t>(g_buffer_stride) * static_cast<size_t>(g_buffer_ring_records);
    g_buffer_ring = reinterpret_cast<uint8_t *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total));
    if (g_buffer_ring == nullptr) {
        log_line("buffer_log: allocation failed bytes=%lu", static_cast<unsigned long>(total));
        return;
    }

    g_buffer_log_enabled = true;
    log_line("buffer_log: enabled payload=%u ring=%u stride=%u", g_buffer_payload_bytes, g_buffer_ring_records, g_buffer_stride);
    if (g_buffer_ring_outcome != kEnvUsed) {
        log_line("buffer_log: WARNING BZ_BUFFER_LOG_RING is %s - running with %u"
                 " records. If you asked for more, the launch options did not"
                 " reach this process; close the game, set them, relaunch.",
                 env_outcome_name(g_buffer_ring_outcome),
                 static_cast<unsigned>(g_buffer_ring_records));
    }
    if (g_buffer_bytes_outcome != kEnvUsed) {
        log_line("buffer_log: WARNING BZ_BUFFER_LOG_BYTES is %s - running with %u",
                 env_outcome_name(g_buffer_bytes_outcome),
                 static_cast<unsigned>(g_buffer_payload_bytes));
    }
    if (g_buffer_peer_filter.count > 0) {
        log_line("buffer_log: BZ_BUFFER_LOG_PEER active, recording %u peer address(es)"
                 " only (%u entries rejected as unparseable)",
                 static_cast<unsigned>(g_buffer_peer_filter.count),
                 static_cast<unsigned>(g_buffer_peer_filter.rejected));
    } else if (g_buffer_peer_filter.rejected > 0) {
        log_line("buffer_log: WARNING BZ_BUFFER_LOG_PEER had %u unparseable entries"
                 " and no valid ones - recording ALL peers",
                 static_cast<unsigned>(g_buffer_peer_filter.rejected));
    }
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

    // BZ_BUFFER_LOG_PEER: keep only the peers the tester asked for, so the
    // ring covers the whole match instead of overflowing on lobby chatter.
    if (!peer_filter_accepts(&g_buffer_peer_filter, src_ipv4)) {
        return;
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
                              "format=buffer_log_v1\r\nrecord_header_size=%u\r\npayload_bytes=%u\r\nrecord_stride=%u\r\nring_records=%u\r\nring_env=%s\r\npayload_env=%s\r\npeer_filter=%u\r\nrecords_written=%u\r\ntotal_events_seen=%llu\r\n"
                              // wrapped: the ring discarded the oldest events; the capture is
                              // the session TAIL, not the session (bit the 2026-08-02 capture:
                              // 65536 kept of 126715 seen, match starts lost). Stated here so
                              // nobody has to compare two counters to notice.
                              "wrapped=%u\r\n",
                              static_cast<unsigned>(sizeof(BufferLogRecordHeader)),
                              static_cast<unsigned>(g_buffer_payload_bytes),
                              static_cast<unsigned>(g_buffer_stride),
                              static_cast<unsigned>(g_buffer_ring_records),
                              env_outcome_name(g_buffer_ring_outcome),
                              env_outcome_name(g_buffer_bytes_outcome),
                              static_cast<unsigned>(g_buffer_peer_filter.count),
                              static_cast<unsigned>(g_buffer_count),
                              static_cast<unsigned long long>(g_buffer_total_events),
                              static_cast<unsigned>(g_buffer_total_events > g_buffer_count ? 1 : 0));
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

// Serialised, one write per line.
//
// Two committed logs contain torn lines — `2026-07-26_crash_dsound_proxy.log`
// line 145 lost its leading `[2`, and the 183650Z bundle's log line 128 is two
// lines concatenated — because this function had no lock and issued *two*
// WriteFile calls per line, body then CRLF. Several threads log concurrently
// (the wake thread, the governor thread, the net-patch thread, the pacer, and
// every hooked call), so another thread could land its own append between the
// two. A torn line is silently unparseable by every tool downstream.
//
// Now: format the whole line including CRLF into one buffer, then take
// g_log_cs for exactly one WriteFile. The lock also covers open/close, which
// keeps the append offset consistent under Wine.
void log_line(const char *fmt, ...) {
    init_log_path();

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    // +2 for the CRLF appended below.
    char buf[770] = {0};
    DWORD pid = GetCurrentProcessId();
    int n = std::snprintf(buf, sizeof(buf) - 2, "[%04u-%02u-%02u %02u:%02u:%02u.%03u][pid=%lu] ",
                          static_cast<unsigned>(st.wYear),
                          static_cast<unsigned>(st.wMonth),
                          static_cast<unsigned>(st.wDay),
                          static_cast<unsigned>(st.wHour),
                          static_cast<unsigned>(st.wMinute),
                          static_cast<unsigned>(st.wSecond),
                          static_cast<unsigned>(st.wMilliseconds),
                          static_cast<unsigned long>(pid));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(buf) - 2) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    int m = std::vsnprintf(buf + n, sizeof(buf) - 2 - static_cast<size_t>(n), fmt, args);
    va_end(args);
    if (m < 0) {
        return;
    }

    size_t len = std::strlen(buf);
    buf[len++] = '\r';
    buf[len++] = '\n';

    if (!g_log_cs_ready) {
        return;
    }
    EnterCriticalSection(&g_log_cs);
    HANDLE h = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, buf, static_cast<DWORD>(len), &written, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_log_cs);
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
            log_line("session end: %s", line);
            g_reorder_stats_logged = true;
        }
    }
    dup_purge_socket(s);
    if (g_pace_cs_ready) {
        char pline[512];
        char dline[512];
        bool have_pace   = false;
        bool have_dampen = false;
        EnterCriticalSection(&g_pace_cs);
        pace_flush_locked();                    // do not strand the game's tail
        pace_purge_socket(&g_tx, static_cast<uintptr_t>(s));
        // Explicit dampen reset: the game uses one UDP socket for all P2P, so
        // closing it ends the session and any peer that reconnects is a fresh
        // epoch.  dampen_purge_peer is the PRIMARY reset signal — a real
        // restart is signalled here rather than inferred by the in-band epoch
        // heuristic.  Purge ONLY when the closing socket is the tracked P2P
        // socket (dampen_close_ends_session, mirroring the reorder block's
        // was_reorder_sock guard): closing an unrelated socket — lobby,
        // discovery, stats upload, or anything before the P2P socket is even
        // known — must leave the peer ring intact, or it collapses suppression
        // mid-match.  Cumulative stats survive the purge; dump them below with
        // the same repeat-suppression as the pace line — the counters are
        // process-wide and teardown closes several sockets back to back.
        if (dampen_close_ends_session(
                static_cast<unsigned long long>(s),
                (g_dampen_sock == INVALID_SOCKET)
                    ? kDampenInvalidSock
                    : static_cast<unsigned long long>(g_dampen_sock))) {
            for (uint32_t i = 0; i < kDampenPeers; ++i) {
                if (g_dampen.peers[i].addr != 0) {
                    // Same reset for the RTT slots: one UDP socket is reused
                    // across matches, so a new epoch's low sequences would
                    // otherwise match the previous match's outstanding ones.
                    if (g_rtt_cs_ready) {
                        EnterCriticalSection(&g_rtt_cs);
                        rtt_purge_peer(&g_rtt, g_dampen.peers[i].addr);
                        LeaveCriticalSection(&g_rtt_cs);
                    }
                    dampen_purge_peer(&g_dampen, g_dampen.peers[i].addr);
                }
            }
            g_dampen_sock = INVALID_SOCKET;
        }
        if (g_reorder_stats) {
            have_dampen = dampen_format_stats(&g_dampen, dline, sizeof(dline)) > 0;
            if (have_dampen && std::strcmp(dline, g_last_dampen_line) == 0) {
                have_dampen = false;
            } else if (have_dampen) {
                std::snprintf(g_last_dampen_line, sizeof(g_last_dampen_line), "%s", dline);
            }
        }
        if (g_reorder_stats) {
            pace_tick(&g_tx, GetTickCount64());
            have_pace = pace_format_stats(&g_tx, pline, sizeof(pline)) > 0;
            // Teardown closes several sockets back to back, and unlike the
            // reorder line above the pace counters are process-wide, so every
            // close re-logged the identical send_stats line (twice, 100 ms
            // apart, in both 2026-08-02 Linux logs — doubling counters for
            // anything that greps naively).  Same counters = nothing new
            // happened = say nothing.
            if (have_pace && std::strcmp(pline, g_last_pace_line) == 0) {
                have_pace = false;
            } else if (have_pace) {
                std::snprintf(g_last_pace_line, sizeof(g_last_pace_line), "%s", pline);
            }
        }
        LeaveCriticalSection(&g_pace_cs);
        if (have_pace) {
            log_line("session end: %s", pline);
            g_send_stats_logged = true;
        }
        if (have_dampen) {
            log_line("session end: %s", dline);
            g_dampen_stats_logged = true;
        }
    }
    return rc;
}

// Last-chance emit for the session counters, called from DLL_PROCESS_DETACH.
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
static void emit_session_stats_at_exit() {
    if (!g_reorder_stats) {
        return;                                 // BZ_REORDER_STATS=0
    }
    if (g_reorder_stats_logged && g_send_stats_logged && g_dampen_stats_logged
        && (g_rtt_stats_logged || !g_rtt.enabled)) {
        return;                                 // clean shutdown already did it
    }
    // Note the rtt term above: the closesocket path emits the other three, so
    // without it a clean shutdown returned here and the rtt lines - the only
    // record of what the link was doing - were never written at all.  Each
    // block below carries its own !logged guard, so falling through costs a
    // few predictable branches and cannot double-log anything.

    bool noted = false;
    auto note = [&noted]() {
        if (!noted) {
            log_line("process exit without closesocket: emitting session"
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
            log_line("session end: reorder lock held at exit, reorder_stats lost");
        }
        if (have_stats) {
            note();
            log_line("session end: %s", line);
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
            log_line("session end: pace lock held at exit, send_stats lost");
        }
        if (have_pace) {
            note();
            log_line("session end: %s", pline);
            g_send_stats_logged = true;
        }
    }

    if (!g_rtt_stats_logged && g_rtt_cs_ready && g_rtt.enabled) {
        char rline[512];
        char rper[kRttPeers][512];
        uint32_t rn = 0;
        bool have_rtt = false;
        if (TryEnterCriticalSection(&g_rtt_cs)) {
            have_rtt = rtt_format_stats(&g_rtt, rline, sizeof(rline)) > 0;
            // The exit line reports the SESSION spread, so unlike the periodic
            // line it must not be gated on the current window having samples:
            // a match that went quiet in its last 15 s would print nothing.
            uint32_t addrs[kRttPeers];
            const uint32_t peers = rtt_active_peers(&g_rtt, addrs, kRttPeers);
            for (uint32_t i = 0; i < peers; ++i) {
                for (uint32_t k = 0; k < kRttPeers; ++k) {
                    if (g_rtt.peers[k].addr != addrs[i]) continue;
                    const RttPeer &pr = g_rtt.peers[k];
                    const uint8_t a = static_cast<uint8_t>(pr.addr & 0xff);
                    const uint8_t b = static_cast<uint8_t>((pr.addr >> 8) & 0xff);
                    const uint8_t c2 = static_cast<uint8_t>((pr.addr >> 16) & 0xff);
                    const uint8_t d = static_cast<uint8_t>((pr.addr >> 24) & 0xff);
                    std::snprintf(rper[rn], sizeof(rper[rn]),
                        "rtt: peer=%u.%u.%u.%u srtt=%u ms var=%u ms "
                        "session min=%u max=%u mean=%llu over %llu samples "
                        "(upper bound: includes the peer's ack delay)",
                        static_cast<unsigned>(a), static_cast<unsigned>(b),
                        static_cast<unsigned>(c2), static_cast<unsigned>(d),
                        static_cast<unsigned>(pr.srtt_ms),
                        static_cast<unsigned>(pr.rttvar_ms),
                        static_cast<unsigned>(pr.min_ms),
                        static_cast<unsigned>(pr.max_ms),
                        static_cast<unsigned long long>(pr.samples ? pr.sum_ms / pr.samples : 0),
                        static_cast<unsigned long long>(pr.samples));
                    rn++;
                    break;
                }
            }
            LeaveCriticalSection(&g_rtt_cs);
        } else {
            log_line("session end: rtt lock held at exit, rtt stats lost");
        }
        if (have_rtt) {
            log_line("session end: %s", rline);
            for (uint32_t i = 0; i < rn; ++i) {
                log_line("session end: %s", rper[i]);
            }
            g_rtt_stats_logged = true;
        }
    }

    if (!g_dampen_stats_logged && g_pace_cs_ready) {
        char dline[512];
        bool have_dampen = false;
        if (TryEnterCriticalSection(&g_pace_cs)) {
            have_dampen = dampen_format_stats(&g_dampen, dline, sizeof(dline)) > 0;
            LeaveCriticalSection(&g_pace_cs);
        } else {
            note();
            log_line("session end: pace lock held at exit, dampen stats lost");
        }
        if (have_dampen) {
            note();
            log_line("session end: %s", dline);
            g_dampen_stats_logged = true;
        }
    }
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

// Hand one datagram to the caller's WSARecvFrom arguments.  Every exit from
// the reorder path goes through here: buffered release, short/non-IPv4
// pass-through, peer-table-full fallback, and eviction overflow.  Must be
// called with g_reorder_cs released — buffer_log_event takes its own lock.
static int deliver_to_caller(SOCKET s,
                             LPWSABUF buffers, DWORD buffer_count,
                             LPDWORD bytes_received, LPDWORD inout_flags,
                             sockaddr *from, LPINT fromlen,
                             const sockaddr_in &src,
                             const uint8_t *data, uint32_t len) {
    uint32_t copied = scatter_copy(buffers, buffer_count, data, len);
    // A datagram that does not fit the caller's buffers is TRUNCATED, and the
    // rest of it is gone.  Reporting success with a short byte count hands the
    // game a silently corrupt datagram — half a packet that parses as a whole
    // one.  Stock winsock fails the call with WSAEMSGSIZE and sets
    // MSG_PARTIAL; so do we.  Only reachable with BZ_REORDER=1, but the kind
    // of wrong that would burn a week of some future debugging session.
    const bool truncated = (copied < len);
    if (bytes_received != nullptr) *bytes_received = copied;
    if (inout_flags != nullptr) *inout_flags = truncated ? MSG_PARTIAL : 0;
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

    if (truncated) {
        g_rx.stats.truncated++;
        WSASetLastError(WSAEMSGSIZE);
        return SOCKET_ERROR;
    }
    WSASetLastError(0);
    return 0;
}

// Emit the reorder counters every kReorderStatsMs.  Formatting is pure
// snprintf so it is cheap enough to do under the lock; the file write is not,
// so log_line runs after the lock is released.
static void maybe_log_reorder_stats(uint64_t now_ms) {
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
    log_line("%s", line);

    // Outbound counters share the cadence: an A/B is only readable when the
    // receive and send sides are timestamped together.
    if (g_pace_cs_ready) {
        char pline[512];
        EnterCriticalSection(&g_pace_cs);
        pace_tick(&g_tx, now_ms);
        const bool ok = pace_format_stats(&g_tx, pline, sizeof(pline)) > 0;
        LeaveCriticalSection(&g_pace_cs);
        if (ok) {
            log_line("%s", pline);
        }
    }
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

    // RTT: reorder is off in the shipped configuration, so this plain
    // recvfrom is the receive path that actually runs.
    if (rc > 0 && g_rtt.enabled && g_rtt_cs_ready && buf != nullptr
        && from != nullptr && from->sa_family == AF_INET) {
        const sockaddr_in *in4r = reinterpret_cast<const sockaddr_in *>(from);
        EnterCriticalSection(&g_rtt_cs);
        rtt_on_recv(&g_rtt, in4r->sin_addr.s_addr,
                    reinterpret_cast<const uint8_t *>(buf),
                    static_cast<uint32_t>(rc), GetTickCount64());
        LeaveCriticalSection(&g_rtt_cs);
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
    // Drain the datagrams the kernel already has into per-peer queues, then
    // return the best in-order candidate.  Packets still waiting for their
    // predecessor are held up to the peer's adaptive window (bounded by
    // g_rx.max_hold_ms) before being released regardless.
    //
    // The drain stops as soon as any peer's ring fills.  Before V4.7 it pulled
    // up to 96 datagrams into 8-slot rings and *discarded* the overflow, so
    // under exactly the bursts this buffer is meant to help it destroyed
    // packets the vanilla game would have received.  Undrained datagrams now
    // simply stay in the 4 MB kernel receive buffer, in order, costing nothing.

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

    uint32_t drained = 0;
    for (uint32_t di = 0; di < g_reorder_drain; ++di) {
        if (reorder_drain_saturated(&g_rx)) {
            break;  // no room left; deliver what we have and come back
        }

        DWORD       drain_bytes  = 0;
        DWORD       drain_flags  = 0;
        sockaddr_in drain_src    = {};
        int         drain_srclen = static_cast<int>(sizeof(drain_src));

        int drc = g_real_wsarecvfrom(s, &drain_wsabuf, 1, &drain_bytes, &drain_flags,
                                     reinterpret_cast<sockaddr *>(&drain_src), &drain_srclen,
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
            return deliver_to_caller(s, buffers, buffer_count, bytes_received, inout_flags,
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
            return deliver_to_caller(s, buffers, buffer_count, bytes_received, inout_flags,
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
            return deliver_to_caller(s, buffers, buffer_count, bytes_received, inout_flags,
                                     from, fromlen, evicted.from, evicted.data, evicted.len);
        }

        if (reorder_must_pass_through(ins)) {
            // Defect E: this datagram carries a sequence we have already
            // delivered.  Because the field counts messages rather than
            // datagrams, that describes ~98% of real inbound traffic — it is
            // ordinary payload, not a duplicate to discard.
            if (drained > g_rx.stats.max_drain_depth) {
                g_rx.stats.max_drain_depth = drained;
            }
            LeaveCriticalSection(&g_reorder_cs);
            return deliver_to_caller(s, buffers, buffer_count, bytes_received, inout_flags,
                                     from, fromlen, drain_src, drain_buf, drain_bytes);
        }
    }

    if (drained > g_rx.stats.max_drain_depth) {
        g_rx.stats.max_drain_depth = drained;
    }

    uint64_t now_ms = GetTickCount64();
    uint32_t best_pi = 0;
    int      best_si = -1;
    int      kind    = kDeliverInOrder;
    if (!reorder_next_ready(&g_rx, now_ms, &best_pi, &best_si, &kind)) {
        // Nothing is ready yet: tell the game the socket is empty for now.
        LeaveCriticalSection(&g_reorder_cs);
        maybe_log_reorder_stats(now_ms);
        WSASetLastError(WSAEWOULDBLOCK);
        return SOCKET_ERROR;
    }

    ReorderSlot pkt;
    reorder_take(&g_rx, &g_rx.tbl[best_pi], best_si, now_ms, kind, &pkt);

    LeaveCriticalSection(&g_reorder_cs);

    int rc = deliver_to_caller(s, buffers, buffer_count, bytes_received, inout_flags,
                               from, fromlen, pkt.from, pkt.data, pkt.len);
    maybe_log_reorder_stats(now_ms);
    return rc;
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

    // Duplicate suppressor: drop a redundant in-window retransmit of a
    // (peer, seq) already sent.  It runs before the pacer so a suppressed copy
    // is never queued, and a suppressed send looks to the game exactly like a
    // successful one — the same contract pace_take relies on: a UDP sendto
    // promises handoff, not delivery.  The burst measurement must still see
    // every datagram, including suppressed ones, so pace_observe runs for the
    // copies that never reach pace_take.  Loopback is the game talking to
    // itself and is skipped, matching the send_dup rule.  Gate on enabled
    // BEFORE taking the lock: with the damper off this path must cost nothing
    // (enabled is written once at init and never changes afterwards).
    if (g_dampen.enabled && flags == 0 && len > 0 && g_pace_cs_ready
        && buf != nullptr && to != nullptr && to->sa_family == AF_INET
        && !dup_is_loopback(to)) {
        const sockaddr_in *in4 = reinterpret_cast<const sockaddr_in *>(to);
        const uint64_t now = GetTickCount64();
        bool dampened;
        EnterCriticalSection(&g_pace_cs);
        dampened = (dampen_admit(&g_dampen, in4->sin_addr.s_addr,
                                 reinterpret_cast<const uint8_t *>(buf),
                                 static_cast<uint32_t>(len), now)
                    == kDampenSuppress);
        // This socket demonstrably carries dampened P2P traffic: it is the
        // one whose close ends the session.  Mirror g_reorder_sock.
        g_dampen_sock = s;
        if (dampened) {
            pace_observe(&g_tx, static_cast<uint32_t>(len), now);
        }
        LeaveCriticalSection(&g_pace_cs);
        if (dampened) {
            WSASetLastError(0);
            return len;
        }
    }

    // RTT: only datagrams that actually reach the wire.  A suppressed copy
    // never arrives, so counting it would mark the sequence ambiguous and
    // discard a sample that was never ambiguous.  Hence: after the return.
    if (g_rtt.enabled && g_rtt_cs_ready && flags == 0 && len > 0
        && buf != nullptr && to != nullptr && to->sa_family == AF_INET
        && !dup_is_loopback(to)) {
        const sockaddr_in *in4r = reinterpret_cast<const sockaddr_in *>(to);
        EnterCriticalSection(&g_rtt_cs);
        rtt_on_send(&g_rtt, in4r->sin_addr.s_addr,
                    reinterpret_cast<const uint8_t *>(buf),
                    static_cast<uint32_t>(len), GetTickCount64());
        LeaveCriticalSection(&g_rtt_cs);
    }

    // Measure (always) and optionally pace.  When the pacer takes ownership the
    // game is told the send succeeded, which is what a UDP send means anyway:
    // handed off, no delivery promise.
    int  rc;
    bool paced = (flags == 0 && len > 0
                  && pace_take(s, reinterpret_cast<const uint8_t *>(buf),
                               static_cast<uint32_t>(len), to, tolen));
    if (paced) {
        rc = len;
    } else {
        rc = g_real_sendto(s, buf, len, flags, to, tolen);
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

    // Measure (always) and optionally pace.  Only the synchronous path: an
    // overlapped send is the caller's to complete, and taking ownership of one
    // would mean completing its OVERLAPPED ourselves — the same class of
    // mistake that froze the game on the receive side in V4.1.
    int rc  = 0;
    int wsa = 0;
    bool paced    = false;
    bool dampened = false;
    if (overlapped == nullptr && completion_routine == nullptr && flags == 0
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
        // Duplicate suppressor: drop a redundant in-window retransmit of a
        // (peer, seq) already sent.  It runs before the pacer so a suppressed
        // copy is never queued; a suppressed send looks to the game exactly
        // like a successful one.  The burst measurement must still see every
        // datagram, including suppressed ones, so pace_observe runs for the
        // copies that never reach pace_take.  The peer key is the destination
        // address, matching the dampen peer table; loopback is the game
        // talking to itself and is skipped, matching the send_dup rule.  Gate
        // on enabled BEFORE taking the lock: with the damper off this path
        // must cost nothing (enabled is written once at init).
        if (g_dampen.enabled && fits && total > 0 && g_pace_cs_ready
            && to->sa_family == AF_INET && !dup_is_loopback(to)) {
            const sockaddr_in *in4 = reinterpret_cast<const sockaddr_in *>(to);
            const uint32_t peer_addr = in4->sin_addr.s_addr;
            const uint64_t now = GetTickCount64();
            EnterCriticalSection(&g_pace_cs);
            dampened = (dampen_admit(&g_dampen, peer_addr, flat, total, now)
                        == kDampenSuppress);
            // This socket demonstrably carries dampened P2P traffic: it is
            // the one whose close ends the session.  Mirror g_reorder_sock.
            g_dampen_sock = s;
            if (dampened) {
                pace_observe(&g_tx, total, now);
            }
            LeaveCriticalSection(&g_pace_cs);
        }
        // Same rule as the sendto path: only what actually reaches the wire.
        if (!dampened && g_rtt.enabled && g_rtt_cs_ready && fits && total > 0
            && to->sa_family == AF_INET && !dup_is_loopback(to)) {
            const sockaddr_in *in4r = reinterpret_cast<const sockaddr_in *>(to);
            EnterCriticalSection(&g_rtt_cs);
            rtt_on_send(&g_rtt, in4r->sin_addr.s_addr, flat, total, GetTickCount64());
            LeaveCriticalSection(&g_rtt_cs);
        }
        if (dampened) {
            if (bytes_sent != nullptr) {
                *bytes_sent = total;
            }
        } else if (fits && total > 0 && pace_take(s, flat, total, to, tolen)) {
            if (bytes_sent != nullptr) {
                *bytes_sent = total;
            }
            paced = true;
        }
    }

    if (!paced && !dampened) {
        rc = g_real_wsasendto(s, buffers, buffer_count, bytes_sent, flags, to, tolen, overlapped, completion_routine);
        wsa = static_cast<int>(WSAGetLastError());
    }

    // Duplicate only IPv4 datagrams large enough to carry a BZRNet sequence
    // field.  The duplicate is a separate synchronous send from a flat copy,
    // which keeps it safe for overlapped originals too: the caller's buffers
    // are only guaranteed valid for the duration of this call.
    // Skip it when the pacer owns the packet (the original has not left yet,
    // so a copy sent now would arrive first and be seen as a reorder — the
    // same rule the sendto hook already applies) and when the damper just
    // suppressed it: duplicating a copy judged redundant would be absurd.
    if (g_send_dup && !paced && !dampened
        && to != nullptr && to->sa_family == AF_INET
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
// (kGovRateAddr) and, when it reads the hardcoded cold-start sentinel 4000 at
// the instant a match's governor is set up, overwrite it with g_gov_start.  A
// 32-bit aligned
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
constexpr uint32_t      kGovColdStart = kGovColdStartSentinel;

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

    // Read-back instrumentation (V4.9).  V4.94 also classifies the sentinel by
    // arrival: a collapsing governor walks down onto 4000 mid-match, which this
    // read as a match start until 2026-08-12.  See kGovFloorRescue in
    // shared/gov_trace.h.
    // The poke was seen to not stick in one
    // of the two V4.8 matches, and it took hand-correlating the game's own log
    // to notice.  Now the proxy log says so itself.
    GovTraceCfg  gtc;
    GovTraceState gts;
    gov_trace_cfg_defaults(&gtc, g_gov_start, kGovColdStart);
    gtc.verify_ms = clamp_u32(parse_env_u32("BZ_GOV_VERIFY_MS", kGovVerifyMsDef), 0, 120000);
    gtc.trace_ms  = clamp_u32(parse_env_u32("BZ_GOV_TRACE_MS", kGovTraceMsDef), 0, 600000);
    gov_trace_init(&gts, GetTickCount64());
    log_line("governor_patch: read-back on (verify=%u ms, trace=%u ms; "
             "BZ_GOV_TRACE_MS=0 silences the periodic line)",
             static_cast<unsigned>(gtc.verify_ms), static_cast<unsigned>(gtc.trace_ms));
    // As-found value, unconditional: a session with no cold-start line after
    // this one means the 4000 sentinel never appeared at this address, which
    // is itself a finding (observed on the 2026-08-02 Snap client).
    log_line("governor_patch: send-rate reads %u at attach (cold-start sentinel "
             "is %u; a match start with no line after this one means the "
             "sentinel never appeared)",
             static_cast<unsigned>(*kGovRateAddr),
             static_cast<unsigned>(kGovColdStart));

    while (InterlockedCompareExchange(&g_gov_stop, 0, 0) == 0) {
        // Aligned 32-bit read/write: torn access is impossible on x86.  One
        // read per poll, so the value we act on is the value we report.
        const uint32_t live = *kGovRateAddr;
        const uint64_t now  = GetTickCount64();

        switch (gov_trace_step(&gtc, &gts, live, now)) {
            case kGovBumped:
                *kGovRateAddr = g_gov_start;
                if (gts.bumps == 1) {
                    log_line("governor_patch: cold-start caught, send-rate %u -> %u "
                             "(match started)", static_cast<unsigned>(kGovColdStart),
                             static_cast<unsigned>(g_gov_start));
                }
                break;
            case kGovFloorRescue:
                // The sentinel reached by descent, not written: the governor
                // collapsed onto its floor mid-match (2026-08-12, 20:35:53,
                // thirteen minutes into the xxMonke1.bzn match).  We still raise the
                // rate — the alternative is a match that spends the rest of its
                // life at 4 kB/s — but this is NOT a match start, is not
                // counted as one, and is not a BZ_GOV_START sample.
                *kGovRateAddr = g_gov_start;
                log_line("governor_patch: FLOOR RESCUE — the send-rate walked "
                         "down onto the %u floor mid-match and was raised to %u. "
                         "This is a collapse, not a match start: something is "
                         "saturating the link (check the retransmit share). "
                         "Rescue #%llu this session.",
                         static_cast<unsigned>(kGovColdStart),
                         static_cast<unsigned>(g_gov_start),
                         static_cast<unsigned long long>(gts.floor_rescues));
                break;
            case kGovClamped:
                log_line("governor_patch: POKE DID NOT HOLD — wrote %u, reads %u "
                         "%llu ms later — reverted to the stock floor (%u). "
                         "Something rewrote the global. This session is not a "
                         "valid BZ_GOV_START=%u sample.",
                         static_cast<unsigned>(g_gov_start),
                         static_cast<unsigned>(gts.observed),
                         static_cast<unsigned long long>(gts.since_bump_ms),
                         static_cast<unsigned>(gtc.clamp_floor),
                         static_cast<unsigned>(g_gov_start));
                break;
            case kGovHeld:
                // Below-target here means DownCount steps, not a failure —
                // the 2026-08-02 evening's eight false POKE DID NOT HOLD
                // verdicts were exactly this. Say so in the line itself so
                // nobody re-reads a working poke as a broken one.
                if (gts.observed >= g_gov_start) {
                    log_line("governor_patch: poke held — %llu ms after the bump "
                             "the send-rate reads %u",
                             static_cast<unsigned long long>(gts.since_bump_ms),
                             static_cast<unsigned>(gts.observed));
                } else {
                    log_line("governor_patch: poke held — %llu ms after the bump "
                             "the send-rate reads %u (governor already adjusting "
                             "off the %u baseline; this is the poke working)",
                             static_cast<unsigned long long>(gts.since_bump_ms),
                             static_cast<unsigned>(gts.observed),
                             static_cast<unsigned>(g_gov_start));
                }
                break;
            case kGovTrace: {
                uint32_t lo = 0, hi = 0, n = 0;
                gov_trace_window(&gts, &lo, &hi, &n);
                log_line("governor_trace: send-rate now=%u (window min=%u max=%u "
                         "over %u samples, session peak=%u)",
                         static_cast<unsigned>(gts.observed),
                         static_cast<unsigned>(lo), static_cast<unsigned>(hi),
                         static_cast<unsigned>(n), static_cast<unsigned>(gts.peak));
                break;
            }
            case kGovNone:
            default:
                break;
        }
        // Periodic RTT line.  Rides this thread: same cadence, and neither the
        // send nor the receive path is touched.  Formatting happens under the
        // lock, the file write outside it.
        if (g_rtt.enabled && g_rtt_cs_ready) {
            uint32_t addrs[kRttPeers];
            char rlines[kRttPeers][512];
            uint32_t rn = 0;
            EnterCriticalSection(&g_rtt_cs);
            if (rtt_trace_due(&g_rtt, GetTickCount64())) {
                const uint32_t peers = rtt_active_peers(&g_rtt, addrs, kRttPeers);
                for (uint32_t i = 0; i < peers; ++i) {
                    if (rtt_format_trace(&g_rtt, addrs[i], rlines[rn],
                                         sizeof(rlines[rn])) > 0) {
                        rn++;
                    }
                }
                rtt_window_reset(&g_rtt);
            }
            LeaveCriticalSection(&g_rtt_cs);
            for (uint32_t i = 0; i < rn; ++i) {
                log_line("%s", rlines[i]);
            }
        }
        Sleep(kGovPollMs);
    }
    log_line("governor_patch: stopping after %llu cold-start bump(s), "
             "%llu floor rescue(s), %llu clamp report(s), last send-rate %u, "
             "session peak %u",
             static_cast<unsigned long long>(gts.bumps),
             static_cast<unsigned long long>(gts.floor_rescues),
             static_cast<unsigned long long>(gts.clamps),
             static_cast<unsigned>(gts.last_seen), static_cast<unsigned>(gts.peak));
    return 0;
}

// Write the game's [Net] tunables straight into .data.
//
// Same DRM-safe strategy as governor_patch_thread and for the same reason: a
// `.text` rewrite was verified to apply and then trip SteamStub's integrity
// check, while `.data` carries no such check and aligned 32-bit stores are
// atomic on x86.  The session parser rewrites these globals at every match
// start (from net.ini, or the stock default when net.ini is found-but-not-
// applied, which is the observed behaviour), so we re-assert on a poll loop:
// within one tick of any match starting, our values win.
//
// The build is confirmed via the unique kGovSig scan before the fixed
// addresses are trusted, and each entry is sanity-gated against a plausible
// range in net_globals.h — a wrong address is vetoed and logged rather than
// written blind.
DWORD WINAPI net_patch_thread(LPVOID) {
    if (!is_target_main_module()) {
        return 0;
    }
    if (!net_globals_any(g_net_tbl, kNetGlobalCount)) {
        return 0;
    }
    // Let SteamStub decrypt .text first (well before any match starts).
    Sleep(15000);

    BYTE *text = nullptr;
    size_t text_size = 0;
    if (!find_section(".text", &text, &text_size)) {
        log_line("net_patch: .text section not found");
        return 0;
    }

    int matches = 0;
    for (size_t i = 0; i + sizeof(kGovSig) <= text_size; ++i) {
        if (std::memcmp(text + i, kGovSig, sizeof(kGovSig)) == 0) {
            if (++matches > 1) break;
        }
    }
    if (matches != 1) {
        log_line("net_patch: %d version signature matches (need exactly 1) - "
                 "disabled. Game version may have changed; re-run BZ_GOV_SCAN.", matches);
        return 0;
    }

    log_line("net_patch: version confirmed; asserting [Net] globals every %ums "
             "(re-applied at every match start; auto-kick entries are host-enforced)",
             static_cast<unsigned>(kGovPollMs));

    // One unconditional line stating every watched global AS FOUND, before the
    // first write.  A value already at target produces no change line, so the
    // 2026-08-02 Snap log was completely silent here and "the poke worked
    // silently" could not be told apart from "the proxy never looked".
    {
        char found[512];
        int off = std::snprintf(found, sizeof(found), "net_patch: values as found:");
        for (size_t i = 0; i < kNetGlobalCount; ++i) {
            if (off < 0 || off >= static_cast<int>(sizeof(found))) break;
            const NetGlobal &g = g_net_tbl[i];
            off += std::snprintf(found + off, sizeof(found) - off, " %s=%u",
                                 g.ini_key,
                                 static_cast<unsigned>(
                                     *reinterpret_cast<uint32_t *>(g.va)));
        }
        log_line("%s (already-at-target means net.ini or an earlier patch set it)",
                 found);
    }

    while (InterlockedCompareExchange(&g_net_stop, 0, 0) == 0) {
        if (net_globals_apply(g_net_tbl, kNetGlobalCount) > 0) {
            for (size_t i = 0; i < kNetGlobalCount; ++i) {
                NetGlobal &g = g_net_tbl[i];
                if (!g.changed) {
                    continue;
                }
                g.changed = 0;
                if (g.state == kNgVetoed) {
                    log_line("net_patch: %s VETOED - 0x%08lx holds %u, outside the "
                             "plausible range %u..%u (stock is %u). Address is wrong "
                             "for this build; not writing it.",
                             g.ini_key, static_cast<unsigned long>(g.va),
                             static_cast<unsigned>(g.seen),
                             static_cast<unsigned>(g.lo), static_cast<unsigned>(g.hi),
                             static_cast<unsigned>(g.stock));
                } else {
                    log_line("net_patch: %s %u -> %u (%s, stock %u)",
                             g.ini_key, static_cast<unsigned>(g.seen),
                             static_cast<unsigned>(g.want), g.env,
                             static_cast<unsigned>(g.stock));
                }
            }
        }
        Sleep(kGovPollMs);
    }
    log_line("net_patch: stopping");
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
        // First, before anything can log: log_line is a no-op until this is
        // ready, and every other init below logs.
        if (InterlockedCompareExchange(&g_log_cs_ready, 0, 0) == 0) {
            InitializeCriticalSection(&g_log_cs);
            InterlockedExchange(&g_log_cs_ready, 1);
        }
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
        if (!g_pace_cs_ready) {
            InitializeCriticalSection(&g_pace_cs);
            if (!g_rtt_cs_ready) {
                InitializeCriticalSection(&g_rtt_cs);
                g_rtt_cs_ready = true;
            }
            g_pace_cs_ready = true;
        }
        init_buffer_log_if_needed();
        {
            // Reorder is OFF by default since 2026-07-26; BZ_REORDER=1 enables.
            //
            // Two independent measurements retired it.  It never ran: the game
            // issues overlapped WSARecvFrom (28% of calls return
            // WSA_IO_PENDING), which hits the `overlapped != nullptr` bypass
            // below, so across a 2.5 h session it buffered zero packets and
            // emitted no reorder_stats at all.  And it had nothing to do: a
            // 65,536-datagram wire capture measured out-of-order arrivals at
            // 0.0-0.2% across all three packet classes, corroborated by the
            // game's own log, where 6,998 of 7,012 discards were packets
            // already consumed and only 14 arrived early.  The traffic problem
            // on these links is duplication (56-83%) and outright loss, neither
            // of which a reorder buffer addresses.
            //
            // The code is kept, tested and correct rather than deleted, because
            // the finding is about *these* links; a link that genuinely
            // reorders would still be served by it.  Turning it on also needs a
            // receive path that can reach it — see Known Limits in the README.
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
            // independent of the adaptive window.  This is the number that
            // bounds the latency the buffer can add to the game's streams —
            // and therefore to the round-trip ping a host measures against
            // AutoKickPing.  Defaults to the window ceiling.
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
            // Outbound pacing.  Measurement is unconditional; smoothing needs
            // an explicit rate because it trades send latency for burst shape.
            // Note the pacer can only absorb BZ_SEND_PACE_MAX_MS worth of
            // budget, so at BZ's rates the 20 ms default shapes very little —
            // read send_stats before raising either knob.
            g_pace_rate   = clamp_u32(parse_env_u32("BZ_SEND_PACE", 0), 0, 10000000);
            g_pace_max_ms = clamp_u32(parse_env_u32("BZ_SEND_PACE_MAX_MS", kPaceMaxDelayDef), 0, 200);
            // Duplicate suppressor.  ON by default since V4.94.  It shipped off
            // in V4.93 pending a live-match validation; the 2026-08-12 monkey
            // match supplied one, from the wrong side.  Four runaway repair-kit
            // objects put 30,691 retransmitted datagrams / 2.64 MB on the wire
            // in 140 seconds — 52.9 kB/s against a governor budget that had
            // collapsed to 4.5 kB/s — and the damper was not running to stop
            // any of it.  Replaying that logged send stream through
            // dampen_admit() suppresses 63.9% of the datagrams at the 60 ms
            // floor window and 69.0% at a realistic 1.2*RTT window, which is
            // the whole of the redundancy: 3.22 copies per message down to one.
            // BZ_SEND_DAMPEN=0 restores the old off-by-default behaviour.
            const char *dampen_env = std::getenv("BZ_SEND_DAMPEN");
            dampen_init(&g_dampen,
                        (dampen_env == nullptr || *dampen_env == '\0')
                            ? true : env_truthy(dampen_env),
                        GetTickCount64());
            // Round-trip sampling.  ON by default (BZ_RTT=0 disables):
            // observation only - two header fields read, nothing altered,
            // delayed or dropped - and without it a lag report cannot be told
            // apart from a frame-rate report.  See shared/net_rtt.h.
            {
                const char *rtt_env = std::getenv("BZ_RTT");
                const uint32_t rtt_trace = clamp_u32(
                    parse_env_u32("BZ_RTT_TRACE_MS", kGovTraceMsDef), 0, 600000);
                rtt_init(&g_rtt,
                         (rtt_env == nullptr || *rtt_env == '\0')
                             ? true : env_truthy(rtt_env),
                         rtt_trace, GetTickCount64());
            }
            g_gov_scan = env_truthy(std::getenv("BZ_GOV_SCAN"));
            // Governor cold-start rate.  ON by default since V4.7: the game
            // hardcodes a 4000 B/s start for every match (a 2026-07-19 session
            // log still shows "Bandwidth usage now set to 4000" at match start),
            // which starves the opening world-state burst.  Poking MinBandwidth
            // below covers the session-setup copy; this covers the separate
            // hardcoded push.  BZ_GOV_START=0 disables.
            // Raised 16000 -> 40000 on 2026-07-26.  This, not MinBandwidth, is what
            // sets a match's opening send rate (see shared/net_globals.h).  A live
            // match sat at 16000 for 72 s *after* the simulation started, then took
            // 2.4 min to reach 40 kB/s and 4.7 min to reach 80 kB/s, while the
            // governor eventually ran to 91,900-112,700 with no ping-driven cutback
            // and measured peak send was only 64,361 B/s.  The opening was therefore
            // far below what the link demonstrably carried.  Not yet validated at
            // 40000 across a full match; BZ_GOV_START=16000 restores the old value.
            g_gov_start = clamp_u32(parse_env_u32("BZ_GOV_START", 40000), 0, 200000);
            // The whole [Net] block, written straight into .data.  Presets are
            // on by default (BZ_NET_TUNE=0 / BZ_AUTOKICK_RELAX=0 restore stock)
            // and mirror net-ini/net.ini — which encodes the intended tuning but
            // has twice been proven found-but-not-applied by the game.
            net_globals_defaults(g_net_tbl);
            net_globals_configure(g_net_tbl, kNetGlobalCount);
        }
        log_line("proxy build: %s", kBuildId);
        log_line("DllMain: DLL_PROCESS_ATTACH");
        log_line("governor_patch: %s (BZ_GOV_START=%u; 0=disabled)",
                 g_gov_start ? "enabled" : "disabled", static_cast<unsigned>(g_gov_start));
        {
            char nets[512];
            int p = std::snprintf(nets, sizeof(nets), "net_patch: %s",
                                  net_globals_any(g_net_tbl, kNetGlobalCount) ? "enabled" : "disabled");
            for (size_t i = 0; i < kNetGlobalCount && p > 0 && static_cast<size_t>(p) < sizeof(nets); ++i) {
                if (g_net_tbl[i].want == 0) {
                    continue;
                }
                p += std::snprintf(nets + p, sizeof(nets) - static_cast<size_t>(p), " %s=%u",
                                   g_net_tbl[i].ini_key, static_cast<unsigned>(g_net_tbl[i].want));
            }
            log_line("%s (0/absent=leave game value; AutoKick* are host-enforced)", nets);
        }
        log_line("send_pace: %s rate_bps=%u max_delay_ms=%u"
                 " (burst measurement is always on; BZ_SEND_PACE=<bytes/sec> to smooth)",
                 g_pace_rate ? "enabled" : "measure-only",
                 static_cast<unsigned>(g_pace_rate),
                 static_cast<unsigned>(g_pace_max_ms));
        log_line("send_dup: %s dup_delay_ms=%u dup_max_pps=%u loopback_dup=skip dscp=%u"
                 " (BZ_SEND_DUP=1 to enable outbound packet duplication)",
                 g_send_dup ? "enabled" : "disabled",
                 static_cast<unsigned>(g_dup_delay_ms),
                 static_cast<unsigned>(g_dup_max_pps),
                 static_cast<unsigned>(g_dscp));
        log_line("send_dampen: %s floor_ms=%u max_ms=%u peers=%u slots=%u"
                 " (on by default since V4.94; BZ_SEND_DAMPEN=0 disables."
                 " Suppresses redundant in-window reliable retransmits;"
                 " purge-on-disconnect is the explicit reset)",
                 g_dampen.enabled ? "enabled" : "disabled",
                 static_cast<unsigned>(kDampenFloorMs),
                 static_cast<unsigned>(kDampenMaxMs),
                 static_cast<unsigned>(kDampenPeers),
                 static_cast<unsigned>(kDampenSlots));
        log_line("reorder: %s max_window_ms=%u min_window_ms=%u max_hold_ms=%u adapt=%d wake=%d"
                 " stats=%d depth=%u peers=%u drain=%u seq_offset=%u",
                 g_reorder_enabled ? "enabled" : "DISABLED",
                 static_cast<unsigned>(g_rx.win_max_ms),
                 static_cast<unsigned>(g_rx.win_min_ms),
                 static_cast<unsigned>(g_rx.max_hold_ms),
                 g_rx.adapt ? 1 : 0,
                 g_wake_enabled ? 1 : 0,
                 g_reorder_stats ? 1 : 0,
                 static_cast<unsigned>(g_rx.depth),
                 static_cast<unsigned>(g_rx.peers),
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
        if (net_globals_any(g_net_tbl, kNetGlobalCount) && is_target_main_module()) {
            HANDLE netp = CreateThread(nullptr, 0, &net_patch_thread, nullptr, 0, nullptr);
            if (netp != nullptr) {
                CloseHandle(netp);
            } else {
                log_line("DllMain: failed to create net patch thread gle=%lu", static_cast<unsigned long>(GetLastError()));
            }
        }
        if (is_target_main_module()) {
            pace_init(&g_tx, g_pace_rate, g_pace_max_ms, GetTickCount64());
            HANDLE pace = CreateThread(nullptr, 0, &send_pace_thread, nullptr, 0, nullptr);
            if (pace != nullptr) {
                CloseHandle(pace);
            } else {
                log_line("DllMain: failed to create send pace thread gle=%lu"
                         " - pacing disabled, measurement continues",
                         static_cast<unsigned long>(GetLastError()));
                g_pace_rate = 0;
                g_tx.rate_bps = 0;
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
        InterlockedExchange(&g_net_stop, 1);
        InterlockedExchange(&g_pace_stop, 1);
        // V4.9: g_wake_sender used to be closed here.  The wake thread is
        // signalled and *not* joined — joining under the loader lock deadlocks
        // — so it can be inside g_real_sendto on that very handle right now,
        // and the number is free to be reissued to another thread's socket().
        // Leave it open; the OS closes it when the process exits.

        // A game that exited without closing its P2P socket has not written
        // its counters yet, and this is the last place they can be recovered.
        emit_session_stats_at_exit();
        flush_buffer_log_files();

        // Stop new events entering the ring, under the lock that guards it,
        // and leave the ring allocated.  buffer_log_event checked
        // g_buffer_ring for null *outside* the lock, so a thread could pass
        // that check and then write into memory HeapFree had already returned.
        if (g_buffer_lock_ready) {
            EnterCriticalSection(&g_buffer_lock);
            g_buffer_log_enabled = false;
            LeaveCriticalSection(&g_buffer_lock);
        } else {
            g_buffer_log_enabled = false;
        }

        // V4.9: no critical section is deleted here and the ring is not freed.
        // Every worker thread above is signalled and not joined, so any of
        // them may be inside one of these sections at this moment, or blocked
        // entering it.  DeleteCriticalSection on a section another thread
        // holds is undefined behaviour, and freeing the ring under a writer
        // corrupts the heap on the way out.
        //
        // The process is exiting.  Leaking five CRITICAL_SECTIONs and one heap
        // block costs nothing and removes a class of exit-path crash — and an
        // exit-path crash is exactly what loses the session counters that V4.8
        // spent a release recovering.
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