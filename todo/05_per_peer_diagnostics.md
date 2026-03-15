# Task: Per-Peer Real-Time Diagnostics Logging in dsound_proxy

## Goal
Add a `recvfrom` hook to the proxy that records per-source-address inter-arrival timing, burst size, and gap statistics to `dsound_proxy.log` in real time during sessions.  The resulting log data should be machine-readable so the existing analysis scripts can produce per-player jitter profiles automatically.

## Background
The current proxy logs socket buffer operations (`setsockopt`, `getsockopt`, `WSASocketW`) but does **not** observe the actual packet stream.  Post-session analysis currently relies entirely on BZLogger, which only reports the drop/warp outcome — not the underlying inter-arrival jitter that caused it.  Adding a `recvfrom` hook gives us timing data to:
- Confirm wireless vs. wired jitter signature per player.
- Correlate burst arrivals to OOO drop windows.
- Build a per-session heatmap of packet inter-arrival time (IAT) per peer.

## What Currently Exists in the Proxy

### Linux proxy: `Linux/proton_dsound_proxy/src/dsound_proxy.cpp`
Currently hooked functions (installed via `hooked_GetProcAddress`):
- `setsockopt`, `WSASetSocketOption`
- `getsockopt`, `WSAGetSocketOption`
- `WSASocketW`, `GetProcAddress`
- `socket`, `closesocket`

**Not yet hooked:** `recvfrom`, `WSARecvFrom`

Logging uses `log_line(...)` which writes timestamped lines to `dsound_proxy.log` in the game directory.

### Windows proxy: `Microslop/winmm_proxy/src/netcode_hooks.cpp`
Same hook set; same gap.

## Proposed Log Format

One line per received UDP datagram:

```
RECV peer=<ip>:<port> seq=<N> len=<bytes> iat_us=<N> burst=<N>
```

Where:
- `peer` = source address from `recvfrom`'s `from` param
- `seq` = BZRNet sequence number parsed from packet header (see offset investigation in `02_ooo_packet_reorder.md`)
- `len` = number of bytes returned
- `iat_us` = microseconds since last packet from this peer (use `clock_gettime(CLOCK_MONOTONIC)` / `QueryPerformanceCounter`)
- `burst` = number of packets from this peer received within a 5 ms window (detect burst arrival)

Example output:
```
2026-03-14 19:24:04.054336 RECV peer=192.168.1.42:2300 seq=7 len=128 iat_us=51200 burst=1
2026-03-14 19:24:04.054336 RECV peer=192.168.1.42:2300 seq=8 len=128 iat_us=0    burst=2
2026-03-14 19:24:04.054336 RECV peer=192.168.1.42:2300 seq=9 len=128 iat_us=0    burst=3
```
These three lines show a burst of 3 back-to-back packets — the signature that causes OOO drops.

## Per-Player Drop Stats (from existing BZLogger analysis)

| Player            | Steam ID               | Sessions w/ drops | Peak drops (session) | Avg clock offset |
|-------------------|------------------------|-------------------|----------------------|------------------|
| KingFurykiller     | S76561198094230200     | 1                 | **4,855** (232149Z)  | 1,589,589 µs     |
| korn champ         | S76561198118689662     | 4/4               | 1,775 (001821Z)      | 904,461 µs       |
| ITA_r0y            | G193494139556766225    | 1 (relay)         | 154 ms latency       | 999,592 µs       |
| PiercingXX (host)  | —                      | 0                 | 0                    | ~0 µs (reference)|

KingFurykiller: wireless (self-confirmed in chat), single bad session.
korn champ: consistent across all 4 sessions — likely bursty ISP or unstable home router.

## Implementation Notes

```cpp
// Add to dsound_proxy.cpp — after existing using/function declarations

using RecvFromFn = int(WSAAPI *)(SOCKET, char *, int, int, sockaddr *, int *);
static RecvFromFn g_real_recvfrom = nullptr;

struct PeerStats {
    DWORD last_recv_tick;  // GetTickCount64()
    int   burst_count;
    DWORD burst_window_start;
};
static std::unordered_map<uint64_t, PeerStats> g_peer_stats;  // key = ip<<16|port
static std::mutex g_peer_mu;

int WSAAPI hooked_recvfrom(SOCKET s, char *buf, int len, int flags,
                           sockaddr *from, int *fromlen) {
    int r = g_real_recvfrom(s, buf, len, flags, from, fromlen);
    if (r > 0 && from && from->sa_family == AF_INET) {
        auto *sin = reinterpret_cast<sockaddr_in *>(from);
        uint64_t key = ((uint64_t)sin->sin_addr.s_addr << 16) | ntohs(sin->sin_port);
        // compute IAT, burst count, log
        // parse seq from buf if offset known (see 02_ooo_packet_reorder.md)
    }
    return r;
}
```

Register in `hooked_GetProcAddress`:
```cpp
if (_stricmp(proc_name, "recvfrom") == 0) {
    g_real_recvfrom = reinterpret_cast<RecvFromFn>(real);
    return reinterpret_cast<FARPROC>(&hooked_recvfrom);
}
// repeat for WSARecvFrom
```

**Performance note:** `log_line` is called on every packet — at 19 Hz × 5 peers = ~95 lines/sec.  Make sure the log write is async (file write on a background thread or buffered) to avoid adding latency to the game's recv path.

## Analysis Script Extension

After implementing the hook, extend `runtime_patch_linux.py` (or create `analyze_proxy_log.py`) to:
1. Parse `RECV` lines from `dsound_proxy.log`.
2. Group by peer address.
3. Compute: mean IAT, 99th-percentile IAT, burst ratio (packets with iat_us < 5000 / total).
4. Output a per-peer table similar to the existing BZLogger analysis.

## Done When
`dsound_proxy.log` from a live session contains `RECV` lines for each peer, and the analysis script prints a per-peer jitter table showing mean/p99 IAT and burst ratio without requiring post-session manual grep work.

## Test Bundle Reference
Parent dir: `/home/piercingxx/Downloads/Testing 1 with 2mb receive/`
All four bundles contain both `BZLogger.txt` and `dsound_proxy.log` — compare before/after to validate new log lines appear.
