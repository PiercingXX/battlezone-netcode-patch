// netcode_hooks.h
// Battlezone 98 Redux - Windows netcode patch

#pragma once
// winsock2.h must come before windows.h to avoid the double-inclusion warning.
#include <winsock2.h>
#include <windows.h>
#include <cstdint>

// ── Reorder buffer structures ────────────────────────────────────────────────

constexpr uint32_t kReorderSeqOffset     = 13;    // byte offset in payload
constexpr uint32_t kReorderSeqMinPay     = 17;    // minimum payload length with seq field
constexpr uint32_t kReorderDefaultMs     = 100;   // max hold window (ms); 45->100 cut drops ~65% in live A/B testing
constexpr uint32_t kReorderMinMsDef      = 5;     // adaptive window floor (ms)
constexpr uint32_t kReorderGrowPadMs     = 5;     // safety margin added on window growth
constexpr uint32_t kReorderDecayMs       = 2000;  // quiet period before the window decays
constexpr uint32_t kReorderDecayStepMs   = 5;     // window shrink per decay step
constexpr uint32_t kReorderSlotCap       = 8;     // max per-peer buffered packet slots
constexpr uint32_t kReorderPeerCap       = 32;    // max distinct IPv4 sources
constexpr uint32_t kReorderDrainCapDef   = 96;    // default real WSARecvFrom calls per hook invocation
constexpr uint32_t kReorderDrainCapMax   = 128;   // hard cap for drain loop
constexpr uint32_t kReorderMaxPktBytes   = 1500;  // max UDP datagram size
constexpr uint32_t kReorderWakeTickMs    = 10;    // wake-thread poll interval
constexpr uint32_t kReorderWakeIdleMs    = 10;    // send wake only if game hasn't polled this long
constexpr uint32_t kReorderWakeBurstCap  = 8;     // max wakes without an intervening game poll

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

// Called from DllMain's hook thread after process attach.
// Walks the game EXE's IAT, replaces WSASocketW and WSARecvFrom with our hooks,
// applies SO_SNDBUF / SO_RCVBUF to every UDP socket created, and enables
// per-peer OOO packet reordering via WSARecvFrom hook with drain-and-deliver.
void InstallNetcodeHooks();

// Called from DllMain during DLL_PROCESS_DETACH.
// Flushes binary packet logs if enabled and releases hook-owned resources.
void ShutdownNetcodeHooks();
