# Battlezone 98 Redux – Windows Netcode Patch

## Overview

This patched `winmm.dll` proxy for 64-bit Windows improves network performance for **Battlezone 98 Redux** multiplayer by:
1. **Enlarging UDP socket buffers** to handle burst traffic: SO_SNDBUF=512KB, SO_RCVBUF=4MB
2. **Out-of-order packet reordering** (OOO) with per-peer buffering and time-based delivery to reduce lag and frame drops
3. **Optional binary packet logging** to `bz_buffer_log.bin` for low-overhead capture and offline analysis

The patch is deployed as a 32-bit DLL (for Windows 7/10/11 compatibility) that is injected into the game process at startup.

## Architecture

**winmm.dll Proxy Chain:**
- Game process → [hooked winmm.dll] (this file) → System32\\winmm.dll (real implementation)
- All audio exports are forwarded transparently

**Netcode Hooks (IAT patching):**
- WSASocketW → Hooked_WSASocketW (apply buffer tuning at socket creation)
- WSARecvFrom → Hooked_WSARecvFrom (implement OOO reorder with drain-and-deliver)
- closesocket → Hooked_closesocket (reset per-peer state on socket close)

## Build Instructions (Linux cross-compile)

### Prerequisites
```bash
sudo apt install gcc-mingw-w64-i686 g++-mingw-w64-i686 binutils-mingw-w64-i686
```

### Build
```bash
cd Microslop/winmm_proxy
make          # produces build/winmm.dll
make clean    # remove build artifacts
```

## Deployment

### Manual Installation (Testing)
1. Build `winmm.dll` as above
2. Copy `build/winmm.dll` to your game directory (same folder as `battlezone98redux.exe`):
   ```
   C:\Program Files (x86)\Steam\steamapps\common\Battlezone 98 Redux\
   ```
3. Launch the game normally (Windows will prefer the local winmm.dll over System32)

### Steam Launch Options
No special launch options needed. Just start the game:
```
%command%
```

The netcode hook thread runs automatically inside DLL_PROCESS_ATTACH, resolves real API functions, patches the game's IAT, and initializes reorder state.

## Configuration

Core runtime parameters are baked into code defaults for simplicity:

| Parameter | Default | Notes |
|-----------|---------|-------|
| SO_SNDBUF | 524,288 bytes (512 KB) | Set at socket creation via WSASocketW hook |
| SO_RCVBUF | 4,194,304 bytes (4 MB) | Set at socket creation via WSASocketW hook |
| Reorder Window | 45 ms | Max hold time before forced delivery |
| Reorder Depth | 8 packets | Max buffered packets per peer |
| Reorder Peers | 32 sources | Max distinct IPv4 sources (P2P peers) |
| Reorder Drain | 96 calls | Real WSARecvFrom calls per hook invocation |

Optional logging controls:

| Variable | Default | Notes |
|-----------|---------|-------|
| BZ_BUFFER_LOG | off | Set to `1` to enable binary packet capture |
| BZ_BUFFER_LOG_BYTES | 32 | Payload prefix bytes stored per record |
| BZ_BUFFER_LOG_RING | 65536 | Number of ring-buffer records held in memory |

Reorder and socket tuning stay baked into defaults. Logging is the only runtime feature toggle.

## Verification

### Check That Patching Succeeded

Look for log output in `winmm_proxy.log` (same directory as the game exe):

```
=== winmm_proxy.dll loaded ===
  Game dir : C:\...\common\Battlezone 98 Redux\
  Log file : C:\...\common\Battlezone 98 Redux\winmm_proxy.log
...
InstallNetcodeHooks: starting
InstallNetcodeHooks: WSASocketW IAT patched OK  SO_SNDBUF target=524288  SO_RCVBUF target=4194304
InstallNetcodeHooks: WSARecvFrom IAT patched OK  OOO reorder enabled window_ms=45 depth=8 peers=32 drain=96
InstallNetcodeHooks: closesocket IAT patched OK
```

### Check Socket Buffer Readback

Subsequent log lines will show actual effective SO_SNDBUF / SO_RCVBUF values:

```
WSASocketW hook: sock=0x... af=2 type=2 proto=17  SO_SNDBUF set_rc=0 effective readback SO_SNDBUF=524288  SO_RCVBUF set_rc=0 effective readback SO_RCVBUF=4194304
```

If effective values are **less than target**, Windows may have clamped them due to registry limits. Consult Windows network tuning docs.

### Check Binary Packet Logging

If you launched with `BZ_BUFFER_LOG=1`, the proxy will flush these files into the game directory on exit:

```
bz_buffer_log.bin
bz_buffer_log.meta.txt
```

`winmm_proxy.log` will also include startup/shutdown lines such as:

```
buffer_log: enabled payload=32 ring=65536 stride=84
buffer_log: flushed records=... total_events=...
```

## OOO Reorder Engine (Technical)

### Packet Flow

1. **Socket Hookup:** Game calls WSASocketW → our hook applies SO_SNDBUF/SO_RCVBUF tuning → returns socket handle
2. **Drain Loop:** Game calls WSARecvFrom → our hook:
   - Pulls up to 96 packets from the real socket (non-blocking)
   - Extracts sequence number (BZRNet frame counter at byte offset 13)
   - Buffers each packet per source IP:port into per-peer PeerBuf
3. **Delivery Selection:** Scan per-peer buffers for ready packets:
   - Prefer exact in-order successor of last_seq
   - Fallback to lowest-seq packet once it's aged ≥45ms
   - On first packet per peer, deliver oldest immediately
4. **Return to Game:** Copy selected packet to game's WSA buffers, update per-peer sequence state, return

### Per-Peer State Structure (PeerBuf)

```c
struct PeerBuf {
    uint64_t key;       // (ipv4 << 16) | port (0 = entry unused)
    uint32_t seq_init;  // 1 once last_seq is valid
    uint32_t last_seq;  // last delivered sequence number
    uint32_t filled;    // count of occupied slots
    ReorderSlot slots[8];  // 8 packet buffers per peer
};
```

Each ReorderSlot holds: arrival timestamp, sequence number, packet length, source address, full packet data (up to 1500 bytes).

### Sequence Number Extraction

- **Location:** Byte offset 13 in UDP payload (confirmed via binary capture analysis)
- **Format:** u32le (little-endian)
- **Packets too short** (<17 bytes) or non-IPv4 sources bypass reorder and deliver directly

### Drain Limit

The drain loop will pull **up to 96 packets per WSARecvFrom hook call**. If the real socket has ≥96 packets buffered, we pull 96 and pause delivery to prevent the game from hanging. Next WSARecvFrom call will drain more. This balances responsiveness with throughput.

## Files

| File | Purpose |
|------|---------|
| src/dllmain.cpp | Entry point, winmm.dll forwarding, logging setup, hook thread spawn |
| src/netcode_hooks.h | Reorder structures (PeerBuf, ReorderSlot), hook function imports |
| src/netcode_hooks.cpp | WSASocketW, WSARecvFrom, closesocket hooks; reorder helpers; IAT patcher |
| src/winmm_proxy.cpp | winmm.dll stub exports (forwarding) |
| src/winmm.def | DLL export table (.def file for linker) |
| Makefile | i686-w64-mingw32-g++ cross-compile build rules |
| README.md | This file |

## Comparison with Linux Version

Both Linux (`dsound.dll`) and Windows (`winmm.dll`) proxies implement the same reorder engine:

| Aspect | Linux | Windows |
|--------|-------|---------|
| **Real DLL** | dsound.dll (audio output) | winmm.dll (multimedia) |
| **Hook Target** | WSARecvFrom in Proton's WS2_32.dll | WSARecvFrom in native WS2_32.dll |
| **Tuning** | SO_SNDBUF=524KB, SO_RCVBUF=4MB | SO_SNDBUF=524KB, SO_RCVBUF=4MB |
| **Reorder Profile** | window=45ms, drain=96, depth=8, peers=32 | window=45ms, drain=96, depth=8, peers=32 |
| **Logging** | Optional binary packet capture (BZ_BUFFER_LOG) | Optional binary packet capture (BZ_BUFFER_LOG) |

## Known Limitations

- **32-bit only:** 64-bit wine/proton not supported; Windows native only (though 32-bit DLL runs on 64-bit Windows via SysWoW64)
- **Assumes single UDP socket:** Resets entire peer buffer table on closesocket (correct for BZ which uses one socket per session)

## Testing Notes

- **Optimal lobby quality:** ~4.38 packet drops per minute (45ms window, 96 drain on clean peers)
- **Longer RTT (100ms+):** Drop rate may increase due to higher variance; window may need tuning (but currently hardcoded)
- **Patching failures:** Check `winmm_proxy.log` for IAT patching errors; common causes are different DLL names in import table (code tries both "WS2_32.dll" and "ws2_32.dll")

## Troubleshooting

### Windows Defender quarantines winmm.dll (Program:Win32/Contebrew.A!ml)

- This project uses a DLL proxy/hook pattern, which can trigger heuristic or PUA detections on unsigned binaries.
- If Defender quarantines winmm.dll, confirm the detection details in Protection history and verify file integrity against your expected hash/build.
- Do not disable Defender globally. If you trust the exact file hash, restore only that item and apply a path-specific exception for the game-folder winmm.dll.
- Submit the quarantined sample to Microsoft as a false positive and include the detection name and file path.
- For maintainers/distribution: signed release artifacts and published SHA256 values reduce repeated false positives.

### "WSASocketW not found in game IAT"
- Game exe may have been linked differently; reorder will not be enabled
- Buffer tuning (SO_SNDBUF/SO_RCVBUF) may still help

### "WSARecvFrom not found in game IAT"
- OOO reorder will NOT be active; only buffer tuning will apply
- Check game version; may be newer build with different API usage

### Socket buffers not increasing
- Windows may have registry limits on SO_SNDBUF/SO_RCVBUF
- Check effective readback values in log; if clamped, tune registry (not covered here)

### High packet drop rate despite patch
- Check lobby quality (ping, packet loss) in game stats
- Verify log shows expected reorder config values
- If patching failed, fallback to buffer tuning alone may help but won't fix OOO issues

## License & Attribution

Based on live test data from Battlezone 98 Redux P2P network captures (March 2026). In-game sequence number location and reorder profile verified via binary packet analysis.
