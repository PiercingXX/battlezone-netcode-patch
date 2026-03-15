# Task: Out-of-Order Packet Resequencing in dsound_proxy / winmm_proxy

## Goal
Hook `recvfrom` / `WSARecvFrom` in the proxy DLL to buffer and resequence packets that arrive 1–4 positions out-of-order before handing them to the game, dramatically reducing the OOO drop count without touching the game binary.

## Background
The game drops any packet whose sequence number is not exactly the next expected value.  When a peer sends bursts (common on wireless or bursty home links) packets arrive in groups with the sequence counter jumping ahead by 1–3.  The game discards all but the one it wants, causing cascading re-send requests and visible warping.

This is a pure proxy-layer fix: intercept `recvfrom`, hold packets in a small per-source ring buffer ordered by sequence number, and only dispatch them to the game in order with a configurable wait window (e.g. 20–40 ms or 4-packet lookahead).

## Drop Format Observed in BZLogger

```
2026-03-14 19:23:50.858096 BZRNet P2P Dropping Packet Type 0 For Client S76561198118689662 (Packet #0 received, #0 expected)
2026-03-14 19:23:53.615146 BZRNet P2P Dropping Packet Type 0 For Client S76561198118689662 (Packet #5 received, #0 expected)
2026-03-14 19:24:04.054336 BZRNet P2P Dropping Packet Type 0 For Client S76561198118689662 (Packet #7 received, #6 expected)
2026-03-14 19:24:04.054336 BZRNet P2P Dropping Packet Type 0 For Client S76561198118689662 (Packet #8 received, #7 expected)
2026-03-14 19:24:04.054336 BZRNet P2P Dropping Packet Type 0 For Client S76561198118689662 (Packet #9 received, #7 expected)
```

Note the `#5 received, #0 expected` line — a burst of 5 packets arrived at once and the engine only consumed packet 0, dropping 1–4.
Also note identical timestamps on lines 3–5: these four are the same burst, only #7 was accepted.

## Drop Counts Per Session (4-session dataset)

| Session | Total OOO Drops | Worst offender                |
|---------|-----------------|-------------------------------|
| 232149Z | **6,857**       | S76561198094230200 (KingFurykiller, wireless) — 4,855 |
| 234646Z | 482             | S76561198118689662 (korn champ)              |
| 235824Z | 1,366           | S76561198118689662 (korn champ)              |
| 001821Z | 3,051           | S76561198118689662 (korn champ) — 1,775      |

KingFurykiller confirmed wireless in chat; korn champ also appears to be on a lossy/bursty connection (0.9 s clock skew).

## Affected Source Files

### Linux proxy
```
Linux/proton_dsound_proxy/src/dsound_proxy.cpp
```
- IAT hooks are installed via `hooked_GetProcAddress` (line ~384) and `hooked_WSASocketW` (line ~331).
- Currently hooks: `setsockopt`, `WSASetSocketOption`, `getsockopt`, `WSAGetSocketOption`, `WSASocketW`, `GetProcAddress`, `socket`, `closesocket`.
- `recvfrom` / `WSARecvFrom` are **not yet hooked** — this is where the new logic goes.
- Build: `Linux/proton_dsound_proxy/` — CMake project, outputs `build/dsound.dll`.

### Windows proxy
```
Microslop/winmm_proxy/src/netcode_hooks.cpp
```
- Same IAT approach, same buffer constants.
- Build: `Microslop/winmm_proxy/` — CMake project, outputs `build/winmm.dll`.  Packaged copy at `Microslop/winmm.dll`.

## Implementation Sketch

```cpp
// Per-source reorder buffer (add to dsound_proxy.cpp)
struct PendingPkt {
    uint32_t seq;
    std::vector<char> data;
    sockaddr_in src;
    DWORD arrived_ms; // GetTickCount()
};
// Map keyed by source addr, value = small priority_queue of PendingPkt
static std::unordered_map<uint64_t, std::priority_queue<...>> g_reorder_bufs;
static std::mutex g_reorder_mu;

int WSAAPI hooked_recvfrom(SOCKET s, char *buf, int len, int flags,
                           sockaddr *from, int *fromlen) {
    // 1. call g_real_recvfrom
    // 2. parse BZRNet packet header for sequence number (field location TBD)
    // 3. push into per-source ring buffer
    // 4. pop and return lowest-seq packet if:
    //    a) it is exactly next expected, OR
    //    b) any packet in buffer is > 40 ms old (flush stale)
    // 5. if nothing ready yet, synthesise WSAEWOULDBLOCK
}
```

**Key unknowns you must resolve:**
- Byte offset of the sequence number field inside a BZRNet UDP payload (grep BZLogger for `Packet Type 0` and correlate with a Wireshark capture, or search the game binary for the drop log string to find the comparison code).
- Whether the game uses blocking or non-blocking recvfrom (check `hooked_WSASocketW` — it currently sets SO_RCVBUF inline; look for `FIONBIO` or `WSAAsyncSelect` calls to determine socket mode).
- Windows proxy needs identical logic in `netcode_hooks.cpp`.

## Done When
A test session with korn champ or KingFurykiller present shows OOO drop count reduced by ≥ 50 % in BZLogger, with no new warp artefacts introduced by the reorder delay.

## Test Bundle Reference
Parent dir: `/home/piercingxx/Downloads/Testing 1 with 2mb receive/`
All four bundles contain `BZLogger.txt` and `dsound_proxy.log`.
