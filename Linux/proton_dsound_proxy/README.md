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

The finalized profile is baked into defaults (window `45`, depth `8`, peers `32`,
drain `96`), so these are optional overrides only.

| Variable | Default | Description |
|---|---|---|
| `BZ_REORDER_WINDOW_MS` | `45` | Max hold time before releasing oldest queued packet |
| `BZ_REORDER_DEPTH` | `8` | Active per-peer reorder queue depth (max `8`) |
| `BZ_REORDER_PEERS` | `32` | Active peer table size (max `32`) |
| `BZ_REORDER_DRAIN` | `96` | Max socket drain iterations per hook call (max `128`) |
| `BZ_BUFFER_LOG` | *(off)* | Set to `1` to capture binary packet trace |

## Steam Launch Options

```
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

## Current Limitations

- Proton-specific: depends on Wine loading a local native `dsound.dll`.
- Windows uses a separate implementation in `Microslop/winmm_proxy/`.

Linux and Windows now ship as separate startup-interception paths with matching socket buffer targets.