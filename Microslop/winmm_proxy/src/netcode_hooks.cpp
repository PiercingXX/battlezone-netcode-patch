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

// Provided by dllmain.cpp
extern void ProxyLog(const char* fmt, ...);

// ---------------------------------------------------------
// Constants
// ---------------------------------------------------------
static const int kTargetSndBuf = 524288;   // 512 KB
static const int kTargetRcvBuf = 4194304;  //   4 MB

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
        return g_realWSARecvFrom(s, buffers, buffer_count, bytes_received, inout_flags,
                                from, fromlen, ov, cr);
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

    LeaveCriticalSection(&g_reorder_cs);

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
