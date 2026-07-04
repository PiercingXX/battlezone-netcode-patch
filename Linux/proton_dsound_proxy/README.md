# Proton DSOUND Proxy (Linux) — Buffer Sizing + OOO Resequencing

This build forces socket buffer sizes and adds in-proxy packet resequencing in
`WSARecvFrom`.

## Why DSOUND.dll

The game imports `DSOUND.dll` very early and only uses ordinal `1` from it, making it a low-risk proxy target compared to a full `WS2_32.dll` proxy.

## What This Proxy Does

On process attach, the proxy:

1. Installs an early `GetProcAddress` hook in the main module.
2. Resolves and patches Winsock imports: `setsockopt`, `WSASetSocketOption`, `getsockopt`, `WSAGetSocketOption`, `socket`, `WSASocketW`, `closesocket`, `recvfrom`, `WSARecvFrom`, `ioctlsocket`, `WSAIoctl`.
3. Forces these target values when Battlezone configures socket buffers:
   - `SO_SNDBUF = 524288`
   - `SO_RCVBUF = 4194304`
4. Immediately reads the effective values back from the same socket handle.
5. If reorder is enabled, holds slightly out-of-order packets and delivers to the game in sequence.
6. When `BZ_BUFFER_LOG=1` is set, writes a binary packet trace to `bz_buffer_log.bin`.
7. Logs socket IDs, handles, force calls, readbacks, reorder state, and close events to `dsound_proxy.log`.
8. Forwards ordinal `1` to the real system `dsound.dll` on demand.

## Important Behavioral Finding

`BZLogger.txt` still prints the old default startup buffer line even when the patch is working.
`dsound_proxy.log` is the source of truth for verification.

## Build Requirements

```bash
sudo apt install mingw-w64   # Debian/Ubuntu
sudo pacman -S mingw-w64-gcc  # Arch
```

## Build

```bash
make
```

Output: `build/dsound.dll`

## Deploy

From the repository root:

```bash
./Linux/deploy_linux.sh "/path/to/Battlezone 98 Redux"
```

## Reorder Configuration

The hold window is **adaptive per peer**: it starts at a small floor
(`BZ_REORDER_MIN_MS`, default `5` ms) so clean connections get near-zero
added latency, grows toward the ceiling (`BZ_REORDER_WINDOW_MS`, default
`100` ms) only when reordering is actually observed on that link, and decays
back down after ~2 s without reorder evidence.

A background **wake thread** prevents held packets from stranding when the
game sleeps in `select()` on the (already drained) socket: it nudges the
socket readable with a tiny internal datagram that the hook discards.

| Variable | Default | Description |
|---|---|---|
| `BZ_REORDER` | `1` | Set to `0` to disable reordering entirely |
| `BZ_REORDER_WINDOW_MS` | `100` | Max (ceiling) hold time before releasing oldest queued packet |
| `BZ_REORDER_MIN_MS` | `5` | Adaptive window floor; `0` = deliver immediately unless reordering seen |
| `BZ_REORDER_ADAPT` | `1` | Set to `0` for a fixed window equal to `BZ_REORDER_WINDOW_MS` |
| `BZ_REORDER_WAKE` | `1` | Set to `0` to disable the wake thread |
| `BZ_REORDER_DEPTH` | `8` | Active per-peer reorder queue depth (max `8`) |
| `BZ_REORDER_PEERS` | `32` | Active peer table size (max `32`) |
| `BZ_REORDER_DRAIN` | `96` | Max socket drain iterations per hook call (max `128`) |
| `BZ_SEND_DUP` | `0` | **Deprecated** (off by default). Re-sends outbound P2P datagrams. Live A/B testing showed it doesn't help this game and degrades busy uplinks by ~doubling packet rate. Kept for completeness; leave off |
| `BZ_DUP_DELAY_MS` | `25` | Delay before the duplicate is transmitted (max `500`). Time-shifting the copy means one queue spike can't kill both. `0` = legacy back-to-back duplicate |
| `BZ_DUP_MAX_PPS` | `40` | Cap on duplicates per second (max `2000`). Low-rate control traffic keeps redundancy; bulk bursts shed theirs. `0` = unlimited |
| `BZ_DSCP` | `46` | DSCP class marked on the P2P socket via IP_TOS (max `63`). 46 = EF; WMM/SQM routers prioritize it over bulk traffic. Effective under Proton. `0` disables |
| `BZ_GOV_START` | `0` | **Opt-in.** Raise the send governor's hardcoded 4000 B/s match-start rate to this many bytes/sec (e.g. `16000`). Data-only patch of the live send-rate global (never touches `.text`, so SteamStub's integrity check is untouched); watches for the 4000 cold-start and bumps it. `0` = disabled. Targets the first-60-seconds drop clusters; sender-side, so it helps how your packets reach every peer |
| `BZ_GOV_SCAN` | `0` | Diagnostic: 15 s after launch, scan the DRM-decrypted `.text` for the 4000 B/s governor start constant and log candidate addresses. Read-only; never patches. (The signature is already captured; this is for re-locating it if the game updates) |
| `BZ_BUFFER_LOG` | *(off)* | Set to `1` to capture binary packet trace |

## Kernel Socket Buffer Limits (Required for Full Effect)

The Linux kernel silently clamps `setsockopt(SO_RCVBUF/SO_SNDBUF)` to
`net.core.rmem_max` / `net.core.wmem_max` (~208 KB on most distros).
Without raising them, the 4 MB receive buffer is mostly fictional — and the
Wine `getsockopt` readback can still report the requested value, so the
proxy log alone does not prove the kernel honored it.

```bash
sudo sysctl -w net.core.rmem_max=4194304 net.core.wmem_max=524288
printf 'net.core.rmem_max=4194304\nnet.core.wmem_max=524288\n' | sudo tee /etc/sysctl.d/99-battlezone-netcode.conf
```

The installer (`install/install_linux.sh`) offers to do this automatically.
Inspect the live socket while the game runs with `ss -uampn`.

## Steam Launch Options

```
WINEDLLOVERRIDES=dsound=n,b %command% -nointro
```

## Current Limitations

- Proton-specific: depends on Wine loading a local native `dsound.dll`.
- Windows uses a separate implementation in `Microslop/winmm_proxy/`.

Linux and Windows now ship as separate startup-interception paths with matching socket buffer targets and matching reorder/tuning env vars.