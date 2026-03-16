# Buffer Logging README

Use this for packet-order / out-of-order packet testing only.

> **These instructions assume you downloaded this repo as a ZIP from GitHub and extracted it to your Downloads folder.**
> All commands below are fully copy-pasteable — `$USER` and `$HOME` expand automatically to your username and home folder.
> If your extracted folder name differs, replace `Battlezone Netcode Testing` in all commands below.

How to use buffer logging:
1. Start buffer logging.
2. Copy the generated Steam launch options.
3. Play game.
4. Exit game.
5. Stop buffer logging.
6. Send the generated bundle archive to devs.

This logger is intended to be much lighter than the full diagnostics logger.

---

## Noob Quick Start (Buffer Logging Only)

Windows:

1. Open PowerShell.
2. Run: `Set-ExecutionPolicy -Scope Process Bypass -Force`
3. Start buffer logging: `& "$HOME\Downloads\Battlezone Netcode Testing\buffer-logging\buffer_logger_windows.ps1" -Action Start`
4. Open the generated `launch_options.txt` file in the new `test_bundles\buffer_windows_*` folder.
5. Copy the launch option line into Steam.
6. Play and exit game.
7. Stop buffer logging: `& "$HOME\Downloads\Battlezone Netcode Testing\buffer-logging\buffer_logger_windows.ps1" -Action Stop`
8. Send the generated `.zip` bundle from `test_bundles`.

Linux (all Steam variants):

1. Start buffer logging:
    `cd "$HOME/Downloads/Battlezone Netcode Testing" && ./buffer-logging/buffer_logger_linux.sh start "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"`
2. Open the generated `launch_options.txt` file in the new `test_bundles/buffer_linux_*` folder.
3. Copy the launch option line into Steam.
4. Play and exit game.
5. Stop buffer logging:
    `cd "$HOME/Downloads/Battlezone Netcode Testing" && ./buffer-logging/buffer_logger_linux.sh stop`
6. Send the generated `.tar.gz` bundle from `test_bundles`.

---

## What This Gives You

The scripts collect only the lightweight files we care about:

- `BZLogger.txt`
- proxy log (`dsound_proxy.log` or `winmm_proxy.log`)
- `bz_buffer_log.bin`
- `bz_buffer_log.meta.txt`
- `multi.ini`

If `bz_buffer_log.bin` is missing, that means the proxy-side binary logger has not been implemented yet.  The scripts will still collect the rest of the bundle.

---

## Windows

### Step 1: Start Buffer Logging

Open PowerShell, then enable scripts for this session:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
```

Then in the repo folder run:

```powershell
cd "$HOME\Downloads\Battlezone Netcode Testing"
.\buffer-logging\buffer_logger_windows.ps1 -Action Start
```

If you are not in the repo folder, run with full path:

```powershell
& "$HOME\Downloads\Battlezone Netcode Testing\buffer-logging\buffer_logger_windows.ps1" -Action Start
```

The script creates a new `test_bundles\buffer_windows_*` session folder and writes a `launch_options.txt` file.

### Step 2: Copy Launch Options Into Steam

Open the generated `launch_options.txt` file and copy the single line inside it into Steam launch options for Battlezone 98 Redux.

### Step 3: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Reproduce the packet-order issue
4. Exit the game

### Step 4: Stop Buffer Logging And Send Bundle

```powershell
cd "$HOME\Downloads\Battlezone Netcode Testing"
.\buffer-logging\buffer_logger_windows.ps1 -Action Stop
```

If you are not in the repo folder, use:

```powershell
& "$HOME\Downloads\Battlezone Netcode Testing\buffer-logging\buffer_logger_windows.ps1" -Action Stop
```

The script will create a `.zip` bundle under `test_bundles`.

---

## Linux - Native Steam

Use this if you installed Steam natively. If you installed Steam via Snap or Flatpak, use the sections below.

### Step 1: Start Buffer Logging

```bash
cd "$HOME/Downloads/Battlezone Netcode Testing"
./buffer-logging/buffer_logger_linux.sh start "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

The script creates a new `test_bundles/buffer_linux_*` session folder and writes a `launch_options.txt` file.

### Step 2: Copy Launch Options Into Steam

Open the generated `launch_options.txt` file and copy the single line inside it into Steam launch options for Battlezone 98 Redux.

### Step 3: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Reproduce the packet-order issue
4. Exit the game

### Step 4: Stop Buffer Logging And Send Bundle

```bash
cd "$HOME/Downloads/Battlezone Netcode Testing"
./buffer-logging/buffer_logger_linux.sh stop
```

The script will create a `.tar.gz` bundle under `test_bundles`.

---

## Linux - Snap Steam

Use this if you installed Steam via Snap (`snap install steam`).

### Step 1: Start Buffer Logging

```bash
cd "$HOME/Downloads/Battlezone Netcode Testing"
./buffer-logging/buffer_logger_linux.sh start "/home/$USER/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

### Step 2: Copy Launch Options Into Steam

Open the generated `launch_options.txt` file and copy the single line inside it into Steam launch options for Battlezone 98 Redux.

### Step 3: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Reproduce the packet-order issue
4. Exit the game

### Step 4: Stop Buffer Logging And Send Bundle

```bash
cd "$HOME/Downloads/Battlezone Netcode Testing"
./buffer-logging/buffer_logger_linux.sh stop
```

---

## Linux - Flatpak Steam

Use this if you installed Steam via Flatpak (`flatpak install steam`).

### Step 1: Start Buffer Logging

```bash
cd "$HOME/Downloads/Battlezone Netcode Testing"
./buffer-logging/buffer_logger_linux.sh start "/home/$USER/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux"
```

### Step 2: Copy Launch Options Into Steam

Open the generated `launch_options.txt` file and copy the single line inside it into Steam launch options for Battlezone 98 Redux.

### Step 3: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Reproduce the packet-order issue
4. Exit the game

### Step 4: Stop Buffer Logging And Send Bundle

```bash
cd "$HOME/Downloads/Battlezone Netcode Testing"
./buffer-logging/buffer_logger_linux.sh stop
```

---

## Purpose

This directory defines a **new low-overhead logger** for packet-order, send/receive-buffer, and socket-mode analysis.

It exists because the current deep diagnostics approach captures too much system-wide data and can materially affect gameplay, timing, and packet behavior.  For the out-of-order packet work, we need logging that is:

- process-local,
- socket-local,
- packet-focused,
- bounded in memory,
- and cheap enough to leave on during a real match.

This logger is for both:

- Linux proxy: `Linux/proton_dsound_proxy/src/dsound_proxy.cpp`
- Windows proxy: `Microslop/winmm_proxy/src/netcode_hooks.cpp`

This directory also includes lightweight collection scripts:

- `buffer-logging/buffer_logger_linux.sh`
- `buffer-logging/buffer_logger_windows.ps1`

## Primary Goal

Collect exactly enough evidence to build and validate the out-of-order resequencing fix without adding enough overhead to create new lag.

## Do Not Reuse The Old Heavy Logger

The existing deep diagnostics tooling is useful for broad environment capture, but it is the wrong tool for packet-order debugging because it:

- collects too much unrelated system/network activity,
- performs expensive text output,
- produces large bundles,
- and can distort timing-sensitive behavior.

This new logger must be implemented separately and kept minimal.

## What We Need To Learn

Before building packet resequencing, we must answer these questions from live traffic:

1. Where is the BZRNet packet sequence field in the UDP payload?
2. Is the receive path blocking or non-blocking?
3. Are out-of-order bursts arriving from one peer at a time or from many peers simultaneously?
4. How often do we see short reorder windows like `N+1`, `N+2`, `N+3` before `N` arrives?
5. Is local socket pressure contributing, or are we only seeing remote burstiness?

## Logging Design Requirements

### Hard requirements

- No system-wide `netstat` polling.
- No per-packet formatted text output during live receive path.
- No full packet dumps to the terminal.
- No synchronous file writes per packet.
- No logging of traffic from unrelated processes.

### Required performance properties

- Fixed-size ring buffer in memory.
- Batched flush to disk.
- Logging can be disabled entirely at compile-time or runtime.
- Logging can be filtered to one socket or one peer.
- Packet payload capture must be capped to a small prefix only.

## Recommended Architecture

### 1. In-memory ring buffer

Use a fixed-size ring buffer of compact binary records.

Each record should be a fixed-size struct so append cost is constant.

Example shape:

```cpp
struct BufferLogEvent {
    uint64_t mono_us;
    uint32_t pid;
    uint32_t tid;
    uint32_t event_type;
    uint32_t socket_id;
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t payload_len;
    uint32_t seq_guess;
    uint32_t aux0;
    uint32_t aux1;
    uint8_t payload_prefix[32];
};
```

The logger should write this structure into memory only.  A background flush path can write the ring to disk every few seconds or on shutdown.

### 2. Binary log file, not text log file

Write to a dedicated binary file:

- Linux: `bz_buffer_log.bin`
- Windows: `bz_buffer_log.bin`

Do not log packet events into `dsound_proxy.log` or `winmm_proxy.log` directly.  Those logs should only contain a few startup/status lines and a pointer to the binary capture file.

### 3. Optional post-run decoder

Add a separate offline decoder later:

- `buffer-logging/decode_buffer_log.py`

Its job is to convert binary records into readable summaries after the match is over.

## Event Types To Capture

### Required event type: socket-open

Capture when the UDP socket is created.

Fields to record:

- timestamp
- socket id
- AF / type / protocol
- effective `SO_SNDBUF`
- effective `SO_RCVBUF`

### Required event type: setsockopt-buffer

Capture only when `SO_SNDBUF` or `SO_RCVBUF` is set.

Fields to record:

- timestamp
- socket id
- option name
- requested value
- forced value
- readback value
- return code

### Required event type: recv-packet

This is the most important event.

Record on every UDP receive for the game socket.

Fields to record:

- timestamp (monotonic microseconds)
- socket id
- source IP
- source port
- payload length
- first 32 bytes of payload
- guessed packet type if recognizable
- guessed sequence field if recognizable

### Required event type: recv-gap

After enough knowledge exists to identify the sequence field, log a derived event for sequence gaps.

Fields to record:

- peer key
- previous sequence
- current sequence
- expected sequence
- delta
- duplicate / in-order / out-of-order classification

### Required event type: socket-mode

Capture whether the game uses blocking or non-blocking sockets.

Hook and log:

- `ioctlsocket`
- `WSAIoctl`
- `WSAAsyncSelect`
- `WSAEventSelect`

Fields to record:

- socket id
- API name
- mode or flags
- return code

### Optional event type: send-packet

Useful later, but lower priority than receive logging.

If implemented, record:

- timestamp
- socket id
- destination IP:port
- payload length
- first 16 to 32 bytes

## First Implementation Scope

The first build of this logger should do only this:

1. Hook socket creation.
2. Hook `setsockopt` / `getsockopt` for buffer values.
3. Hook `recvfrom` and `WSARecvFrom`.
4. Hook socket mode APIs.
5. Write compact binary records to a ring buffer.

Do **not** implement packet resequencing yet.

The goal of phase 1 is evidence collection only.

## Linux Instructions

### Session script

Use:

```bash
cd "$HOME/Downloads/Battlezone Netcode Testing"
./buffer-logging/buffer_logger_linux.sh start "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

Optional explicit form:

```bash
cd "$HOME/Downloads/Battlezone Netcode Testing"
./buffer-logging/buffer_logger_linux.sh start "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux" 32 65536
```

Optional peer-targeted form:

```bash
cd "$HOME/Downloads/Battlezone Netcode Testing"
./buffer-logging/buffer_logger_linux.sh start "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux" 32 65536 "203.0.113.44:37218"
```

The script creates a lightweight session directory under `test_bundles/`, writes the exact Steam launch options into `launch_options.txt`, and does not start any heavy background polling.

### Source file

Add the logger to:

- `Linux/proton_dsound_proxy/src/dsound_proxy.cpp`

### Functions to hook

The Linux proxy currently already hooks:

- `setsockopt`
- `WSASetSocketOption`
- `getsockopt`
- `WSAGetSocketOption`
- `socket`
- `WSASocketW`
- `closesocket`
- `GetProcAddress`

Add hooks for:

- `recvfrom`
- `WSARecvFrom`
- `ioctlsocket`
- `WSAIoctl`
- `WSAAsyncSelect`
- `WSAEventSelect`

### Linux runtime control

Support these environment variables:

- `BZ_BUFFER_LOG=1` to enable logging
- `BZ_BUFFER_LOG_SOCKET=<id>` to filter to one socket
- `BZ_BUFFER_LOG_PEER=<ip:port>` to filter to one peer
- `BZ_BUFFER_LOG_BYTES=32` to control payload-prefix capture length
- `BZ_BUFFER_LOG_RING=65536` for number of records in ring buffer

If `BZ_BUFFER_LOG` is not set, packet logging should stay off.

### Linux stop / collection

After the match ends, run:

```bash
cd "$HOME/Downloads/Battlezone Netcode Testing"
./buffer-logging/buffer_logger_linux.sh stop
```

The stop step copies any available lightweight outputs from the game directory into the session bundle:

- `BZLogger.txt`
- `dsound_proxy.log`
- `bz_buffer_log.bin`
- `bz_buffer_log.meta.txt`
- `multi.ini`

It also creates a `.tar.gz` archive of the session directory.

### Linux output files

Write:

- `bz_buffer_log.bin`
- `bz_buffer_log.meta.txt`

The metadata file should be tiny and contain:

- build timestamp
- enabled flags
- ring size
- payload-prefix size
- socket ids seen

## Windows Instructions

### Session script

Use:

```powershell
cd "$HOME\Downloads\Battlezone Netcode Testing"
.\buffer-logging\buffer_logger_windows.ps1 -Action Start
```

Optional explicit form:

```powershell
cd "$HOME\Downloads\Battlezone Netcode Testing"
.\buffer-logging\buffer_logger_windows.ps1 -Action Start -GamePath "C:\Program Files (x86)\Steam\steamapps\common\Battlezone 98 Redux" -PayloadBytes 32 -RingRecords 65536
```

Optional peer-targeted form:

```powershell
cd "$HOME\Downloads\Battlezone Netcode Testing"
.\buffer-logging\buffer_logger_windows.ps1 -Action Start -PeerFilter "203.0.113.44:37218"
```

The script creates a lightweight session directory under `test_bundles\`, writes the exact Steam launch options into `launch_options.txt`, and avoids the heavy diagnostics capture.

### Source file

Add the logger to:

- `Microslop/winmm_proxy/src/netcode_hooks.cpp`

### Current limitation

The Windows proxy currently only hooks `WSASocketW` for buffer application.

Before the logger can be useful on Windows, extend the IAT patcher to support:

- `recvfrom`
- `WSARecvFrom`
- `setsockopt`
- `getsockopt`
- `ioctlsocket`
- `WSAIoctl`
- `WSAAsyncSelect`
- `WSAEventSelect`
- optionally `sendto`

### Windows runtime control

Support these environment variables:

- `BZ_BUFFER_LOG=1`
- `BZ_BUFFER_LOG_SOCKET=<id>`
- `BZ_BUFFER_LOG_PEER=<ip:port>`
- `BZ_BUFFER_LOG_BYTES=32`
- `BZ_BUFFER_LOG_RING=65536`

These should be read once at startup.

### Windows stop / collection

After the match ends, run:

```powershell
cd "$HOME\Downloads\Battlezone Netcode Testing"
.\buffer-logging\buffer_logger_windows.ps1 -Action Stop
```

The stop step copies any available lightweight outputs from the game directory into the session bundle:

- `BZLogger.txt`
- `winmm_proxy.log`
- `bz_buffer_log.bin`
- `bz_buffer_log.meta.txt`
- `multi.ini`

It also creates a `.zip` archive of the session directory.

### Windows output files

Write:

- `bz_buffer_log.bin`
- `bz_buffer_log.meta.txt`

Do not flood `winmm_proxy.log` with packet events.

## Minimal Metadata We Must Decode Later

The offline decoder must be able to reconstruct:

- packet arrival timeline per peer
- inter-arrival gap per peer
- burst groups with same or near-same timestamp
- first seen socket id
- guessed sequence deltas once the field is known

## Low-Overhead Safety Rules

### Rule 1

Never allocate memory per packet.

### Rule 2

Never format strings per packet on the receive hot path.

### Rule 3

Never flush to disk per packet.

### Rule 4

If the ring buffer fills, overwrite oldest records or increment a single dropped-record counter.  Do not block the game.

### Rule 5

If parsing the payload header is not yet trustworthy, log raw prefix bytes only and decode them offline.

## What To Capture From The First Test Run

For the first logger-enabled test, collect:

1. One Linux session with a known bad peer.
2. One Windows session with a known bad peer.
3. One clean Linux control session.

The first test goal is to answer:

- which socket id is the actual game UDP socket,
- whether receives are blocking or non-blocking,
- and what bytes in the packet correspond to the sequence counter.

## Important Limitation Right Now

These scripts are ready to collect the new lightweight logger output, but the proxy-side packet logger still needs to be implemented in:

- `Linux/proton_dsound_proxy/src/dsound_proxy.cpp`
- `Microslop/winmm_proxy/src/netcode_hooks.cpp`

Until that code exists, the scripts will still collect `BZLogger.txt` and the proxy text logs, but `bz_buffer_log.bin` may be missing.  That is expected.

## Success Criteria For This Logger

This new logger is successful when:

- the game remains playable while it is enabled,
- the binary capture stays reasonably small,
- we can identify the sequence-number field from real traffic,
- and we can confidently build the reorder buffer without guessing.

## Immediate Next Steps

1. Implement Linux packet logging first.
2. Decode one bad session and identify the sequence field.
3. Mirror the logging hooks into Microslop.
4. Only then implement resequencing.

That order matters.  Logging first, reorder second.