# Battlezone Netcode Patch — Out-of-Order Packet Resequencing

This repo includes socket buffer sizing plus **in-proxy reorder buffering**.

Battlezone 98 Redux drops any UDP packet whose internal sequence number is not exactly the next expected value.  On bursty or wireless connections this causes cascading drops and visible player warping.  This patch intercepts `WSARecvFrom` in the proxy DLL, holds packets arriving just out of order in a small per-peer buffer, and delivers them to the game in the correct sequence — without touching the game binary.

> **These instructions assume you downloaded this repo as a ZIP from GitHub and extracted it to your Downloads folder.**
> All commands below are fully copy-pasteable — `$USER` and `$HOME` expand automatically.
> If your extracted folder is named `battlezone-netcode-patch-main` instead of `battlezone-netcode-patch-master`, replace `...-master` in all commands below.

For logging instructions, see [logging_readme.md](logging_readme.md).

---

## Windows

> **Not yet implemented.** The Windows `winmm_proxy` does not yet contain recv-path hooks.
> Use Patch 00 on Windows for now — it still applies the socket buffer fix.

---

## Linux — Native Steam

### Step 1: Install required tools

**Debian / Ubuntu:**
```bash
sudo apt install mingw-w64 make
```

**Arch / Manjaro:**
```bash
sudo pacman -S mingw-w64-gcc make
```

### Step 2: Deploy the patch

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/deploy_linux.sh "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

### Step 3: Set Steam launch options

1. Open Steam
2. Right-click **Battlezone 98 Redux** → **Properties**
3. Click **General** on the left
4. In the **Launch Options** box paste:

```
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

> Reordering is **on by default**.  Add `BZ_REORDER=0` to the front of the launch options only if you experience issues.

---

## Linux — Snap Steam

### Step 1: Install required tools

**Debian / Ubuntu:**
```bash
sudo apt install mingw-w64 make
```

**Arch / Manjaro:**
```bash
sudo pacman -S mingw-w64-gcc make
```

### Step 2: Deploy the patch

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/deploy_linux.sh "/home/$USER/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

### Step 3: Set Steam launch options

```
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

---

## Linux — Flatpak Steam

### Step 1: Install required tools

**Debian / Ubuntu:**
```bash
sudo apt install mingw-w64 make
```

**Arch / Manjaro:**
```bash
sudo pacman -S mingw-w64-gcc make
```

### Step 2: Deploy the patch

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/deploy_linux.sh "/home/$USER/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux"
```

### Step 3: Set Steam launch options

```
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

---

## Environment Variables

| Variable | Default | Description |
|---|---|---|
| `BZ_REORDER` | *(on)* | Set to `0` to disable reorder buffering |
| `BZ_REORDER_WINDOW_MS` | `30` | Max time (ms) a packet may be held waiting for its predecessor. Increase to `50`–`60` for very bursty/wireless peers. |
| `BZ_BUFFER_LOG` | *(off)* | Set to `1` to capture binary packet trace for analysis |

---

## What Changed vs. Patch 00

This patch includes everything from Patch 00 (SO_SNDBUF / SO_RCVBUF forcing) and adds:

- Per-peer reorder buffer inside `hooked_WSARecvFrom`
- Up to 8 packets held per peer, keyed by IPv4 source
- Sequence field read from `payload[13..16]` (u32le) — confirmed via live binary capture analysis
- Hold window eviction: oldest packet released once it exceeds `BZ_REORDER_WINDOW_MS`
- Peer table cleared on `closesocket` to prevent stale state across reconnections

## Technical Details

- Source: `Linux/proton_dsound_proxy/src/dsound_proxy.cpp`
- Sequence field analysis: `../resources/valid_capture_reorder_signal_only.csv`
- Full investigation: `INVESTIGATION_WRITEUP.md`
