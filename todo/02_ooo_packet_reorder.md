# Task: Out-of-Order Packet Resequencing in dsound_proxy / winmm_proxy

## Goal
Hook `recvfrom` / `WSARecvFrom` in the proxy DLL to buffer and resequence packets that arrive 1–4 positions out-of-order before handing them to the game, dramatically reducing the OOO drop count without touching the game binary.

## Background
The game drops any packet whose sequence number is not exactly the next expected value.  When a peer sends bursts (common on wireless or bursty home links) packets arrive in groups with the sequence counter jumping ahead by 1–3.  The game discards all but the one it wants, causing cascading re-send requests and visible warping.

This is a pure proxy-layer fix: intercept `recvfrom`, hold packets in a small per-source ring buffer ordered by sequence number, and only dispatch them to the game in order with a configurable wait window (e.g. 20–40 ms or 4-packet lookahead).

## Drop Format Observed in BZLogger

```
2026-03-14 19:23:50.858096 BZRNet P2P Dropping Packet Type 0 For Client CLIENT_B (Packet #0 received, #0 expected)
2026-03-14 19:23:53.615146 BZRNet P2P Dropping Packet Type 0 For Client CLIENT_B (Packet #5 received, #0 expected)
2026-03-14 19:24:04.054336 BZRNet P2P Dropping Packet Type 0 For Client CLIENT_B (Packet #7 received, #6 expected)
2026-03-14 19:24:04.054336 BZRNet P2P Dropping Packet Type 0 For Client CLIENT_B (Packet #8 received, #7 expected)
2026-03-14 19:24:04.054336 BZRNet P2P Dropping Packet Type 0 For Client CLIENT_B (Packet #9 received, #7 expected)
```

Note the `#5 received, #0 expected` line — a burst of 5 packets arrived at once and the engine only consumed packet 0, dropping 1–4.
Also note identical timestamps on lines 3–5: these four are the same burst, only #7 was accepted.

## Drop Counts Per Session (4-session dataset)

| Session | Total OOO Drops | Worst offender                |
|---------|-----------------|-------------------------------|
| 232149Z | **6,857**       | CLIENT_A (Player_A, wireless) — 4,855 |
| 234646Z | 482             | CLIENT_B (Player_B)              |
| 235824Z | 1,366           | CLIENT_B (Player_B)              |
| 001821Z | 3,051           | CLIENT_B (Player_B) — 1,775      |

Player_A confirmed wireless in chat; Player_B also appears to be on a lossy/bursty connection (0.9 s clock skew).

## Valid Capture Incident Timeline (Signal-Only)

This section uses only the validated bundle with binary logger output:

- `/path/to/test_bundles/buffer_linux_unknown-host_20260315T221910Z`

Derived artifacts in this repo:

- `resources/valid_capture_reorder_signal_only.csv`
- `resources/valid_capture_reorder_signal_clusters_250ms.csv`
- `resources/valid_capture_reorder_signal_only.md`

Signal filter used:

- keep `backward` and `forward_gap_small` only
- drop `duplicate` and `forward_jump_large` for incident ranking

Signal summary from this capture:

- total signal events: `240`
- active 250 ms windows: `109`
- dominant problem peer in hottest windows: `203.0.113.10:55452`

Relative timeline reference:

- `T0 = tick_ms 21396021` (first signal event)
- `T+N ms = window_start_ms - T0`

Highest-priority incident windows (250 ms):

1. `T+35.229s` (`21431250`) -> `37` signal events (`14` backward, `23` gap-small), top peer `203.0.113.10:55452` (`21`)
2. `T+35.479s` (`21431500`) -> `21` signal events (`5` backward, `16` gap-small), top peer `203.0.113.10:55452` (`15`)
3. `T+40.229s` (`21436250`) -> `14` signal events (`1` backward, `13` gap-small), top peer `203.0.113.10:55452` (`10`)
4. `T+124.979s` (`21521000`) -> `7` signal events (`1` backward, `6` gap-small), top peer `203.0.113.11:46080` (`6`)
5. `T+215.229s` (`21611250`) -> `9` signal events (`1` backward, `8` gap-small), top peer `203.0.113.11:46080` (`9`)

Backward-heavy windows to target first for resequencing validation:

1. `T+35.229s` (`21431250`) -> backward `14`
2. `T+35.479s` (`21431500`) -> backward `5`
3. `T+16.229s` (`21412250`) -> backward `2`
4. `T+36.729s` (`21432750`) -> backward `2`
5. `T+120.979s` (`21517000`) -> backward `2`

Practical test target:

- initial resequencing prototype should reduce backward events in the `T+35.2s` to `T+35.7s` cluster first, then re-check the secondary clusters above.

## Affected Source Files

### Linux proxy — **patch implemented** ✓
```
Linux/proton_dsound_proxy/src/dsound_proxy.cpp
```
- `hooked_WSARecvFrom` now contains the full drain-and-deliver reorder engine.
- `hooked_closesocket` clears the peer table on socket close.
- `DllMain` initialises `g_reorder_cs` and reads env vars.
- Build: `make` in `Linux/proton_dsound_proxy/` → `build/dsound.dll`

### Windows proxy — **not yet implemented**
```
Microslop/winmm_proxy/src/netcode_hooks.cpp
```
- Still missing recv-path hooks entirely.  Port the same logic when ready.

## Implementation — as built

New globals added after the buffer-ring block:

```cpp
// key structs
struct ReorderSlot { uint64_t ts; uint32_t seq, len, used, _pad;
                     sockaddr_in from; uint8_t data[1500]; };
struct PeerBuf     { uint64_t key; uint32_t seq_init, last_seq, filled, _pad;
                     ReorderSlot slots[8]; };
static PeerBuf  g_peers[32];          // zero-initialised BSS (~384 KB)
static CRITICAL_SECTION g_reorder_cs;
static bool g_reorder_enabled;        // default ON; BZ_REORDER=0 disables
static uint32_t g_reorder_ms;         // BZ_REORDER_WINDOW_MS (default 30)
```

New env vars (set alongside `BZ_BUFFER_LOG=1`):

| Variable | Default | Effect |
|---|---|---|
| `BZ_REORDER` | *(on)* | Set to `0` to disable reorder buffering |
| `BZ_REORDER_WINDOW_MS` | `30` | Max hold time (ms) before forcing a packet out |

Per-call flow inside `hooked_WSARecvFrom`:

1. **Bypass** — if reorder disabled, or overlapped/async path, falls through to original passthrough + buffer-log.
2. **Drain** — calls `g_real_wsarecvfrom` in a non-blocking loop (up to 32 times) and inserts each IPv4 packet with a parseable sequence field (`payload[13..16]` u32le) into the per-peer slot array.
3. **Deliver** — scans peer table for the first ready slot: exact in-order successor of `last_seq`, or oldest slot that has been waiting ≥ `g_reorder_ms` ms.
4. **Return** — copies the chosen packet into the caller's scatter-gather buffers, returns `0` (success).  If nothing is ready, returns `SOCKET_ERROR / WSAEWOULDBLOCK`.

Fallback cases (deliver immediately without buffering):
- Non-IPv4 source, payload < 17 bytes, or peer table full.

## Validation target

Run a session with `BZ_BUFFER_LOG=1` set (reorder is enabled by default).  Compare the new binary capture against `resources/valid_capture_reorder_signal_only.csv`:

- **Pass**: backward event count in `T+35.229s` window drops significantly below baseline `14`.
- **Pass**: total signal events (`240`) reduced, not increased.
- **Fail signal**: new latency artefacts (warping / rubberbanding) not present before.

## Done When
A test session with Player_A or Player_B present shows OOO drop count reduced by ≥ 50 % in BZLogger, with no new warp artefacts introduced by the reorder delay.

## Test Bundle Reference
Parent dir: `/path/to/test-bundles/`
All four bundles contain `BZLogger.txt` and `dsound_proxy.log`.
