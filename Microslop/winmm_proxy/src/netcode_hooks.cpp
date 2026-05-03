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

static PFN_WSASocketW  g_realWSASocketW = nullptr;
static PFN_setsockopt  g_realSetsockopt  = nullptr;
static PFN_getsockopt  g_realGetsockopt  = nullptr;
static PFN_WSARecvFrom g_realWSARecvFrom = nullptr;
static PFN_closesocket g_realClosesocket = nullptr;

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
static bool              g_reorder_enabled     = true;   // always on (finalized profile)
static uint32_t          g_reorder_ms          = kReorderDefaultMs;
static uint32_t          g_reorder_depth       = kReorderSlotCap;
static uint32_t          g_reorder_peers       = kReorderPeerCap;
static uint32_t          g_reorder_drain       = kReorderDrainCapDef;
static PeerBuf           g_peers[kReorderPeerCap];        // zero-initialized (BSS)
static CRITICAL_SECTION  g_reorder_cs          = {};
static bool              g_reorder_cs_ready    = false;

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
            return &g_peers[i];
        }
    }
    return nullptr; // peer table full
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
            if (now_ms < pb->slots[i].ts || (now_ms - pb->slots[i].ts) < g_reorder_ms) {
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

        // Immediate readback – this is what testers verify in the log.
        int snd_read = -1, rcv_read = -1;
        int snd_len  = sizeof(snd_read), rcv_len = sizeof(rcv_read);
        g_realGetsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&snd_read, &snd_len);
        g_realGetsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&rcv_read, &rcv_len);

        ProxyLog(
            "WSASocketW hook: sock=0x%p af=%d type=%d proto=%d"
            "  SO_SNDBUF set_rc=%d effective readback SO_SNDBUF=%d"
            "  SO_RCVBUF set_rc=%d effective readback SO_RCVBUF=%d",
            (void*)s, af, type, protocol,
            rc_snd, snd_read,
            rc_rcv, rcv_read);
    }

    return s;
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

    if (!g_reorder_enabled || !g_reorder_cs_ready) {
        // Passthrough if reorder not configured or not ready
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
        reorder_insert(pb, seq, arrival_ms, drain_src, drain_buf, drain_bytes);
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
        LeaveCriticalSection(&g_reorder_cs);
    }

    return rc;
}

// ---------------------------------------------------------
// IAT patcher
// Finds moduleName!funcName in the IAT of `module` and
// replaces the slot with newFunc.  If oldFunc is non-null,
// the previous value is stored there.
// ---------------------------------------------------------
static bool PatchIAT(HMODULE module, const char* dllName,
    const char* funcName, void* newFunc, void** oldFunc)
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
            if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;

            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                reinterpret_cast<BYTE*>(module) +
                origThunk->u1.AddressOfData);

            if (strcmp(reinterpret_cast<const char*>(ibn->Name), funcName) != 0)
                continue;

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

    if (!g_realWSASocketW || !g_realSetsockopt || !g_realGetsockopt || !g_realWSARecvFrom || !g_realClosesocket)
    {
        ProxyLog("InstallNetcodeHooks: failed to resolve ws2_32 functions");
        return;
    }

    // Apply user-tunable reorder parameters (all optional, all have defaults baked in)
    g_reorder_ms = g_reorder_ms;     // already set to kReorderDefaultMs, no env read
    g_reorder_depth = g_reorder_depth;
    g_reorder_peers = g_reorder_peers;
    g_reorder_drain = g_reorder_drain;

    // IAT-patch WSASocketW and WSARecvFrom in the game EXE.
    HMODULE exe = GetModuleHandleA(nullptr);
    void* savedRealSocket = nullptr;
    void* savedRealRecvFrom = nullptr;

    bool patchedSocket = PatchIAT(exe, "WS2_32.dll", "WSASocketW",
        reinterpret_cast<void*>(Hooked_WSASocketW), &savedRealSocket);
    if (!patchedSocket)
    {
        // Some builds lowercase the DLL name in the import directory.
        patchedSocket = PatchIAT(exe, "ws2_32.dll", "WSASocketW",
            reinterpret_cast<void*>(Hooked_WSASocketW), &savedRealSocket);
    }

    bool patchedRecvFrom = PatchIAT(exe, "WS2_32.dll", "WSARecvFrom",
        reinterpret_cast<void*>(Hooked_WSARecvFrom), &savedRealRecvFrom);
    if (!patchedRecvFrom)
    {
        patchedRecvFrom = PatchIAT(exe, "ws2_32.dll", "WSARecvFrom",
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
                 "  OOO reorder enabled window_ms=%u depth=%u peers=%u drain=%u",
                 static_cast<unsigned>(g_reorder_ms),
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
    bool patchedClosesocket = PatchIAT(exe, "WS2_32.dll", "closesocket",
        reinterpret_cast<void*>(Hooked_closesocket), &savedRealClosesocket);
    if (!patchedClosesocket)
    {
        patchedClosesocket = PatchIAT(exe, "ws2_32.dll", "closesocket",
            reinterpret_cast<void*>(Hooked_closesocket), &savedRealClosesocket);
    }

    if (patchedClosesocket)
    {
        if (savedRealClosesocket) g_realClosesocket = reinterpret_cast<PFN_closesocket>(savedRealClosesocket);
        ProxyLog("InstallNetcodeHooks: closesocket IAT patched OK");
    }
}

void ShutdownNetcodeHooks()
{
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
}
