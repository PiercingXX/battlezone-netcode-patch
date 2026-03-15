# Battlezone Netcode Patch - Logging Guide

Use this for bad sessions only (lag spikes, desync, freeze, CTD/crash).

> **These instructions assume you downloaded this repo as a ZIP from GitHub and extracted it to your Downloads folder.**
> All commands below are fully copy-pasteable — `$USER` and `$HOME` expand automatically to your username and home folder.
> If your extracted folder is named `battlezone-netcode-patch-main` instead of `battlezone-netcode-patch-master`, replace `...-master` in all commands below.

How to use logging:
1. Start logging.
2. Play game.
3. Exit game.
4. Stop logging.
5. Send the generated bundle archive to devs (not the script itself).

Ideally we want to have no more than 3 games logged. Both host and client should log and upload bundles for the same match.

**IF YOU CRASH, STOP LOGGING AND SEND BUNDLE BEFORE RESTARTING LOGGING AND THEN BATTLEZONE**

---

## Noob Quick Start (Logging Only)

Windows:

1. Open PowerShell as Administrator.
2. Run: `Set-ExecutionPolicy -Scope Process Bypass -Force`
3. Start logging: `& "$HOME\Downloads\battlezone-netcode-patch-master\Microslop\tester_diag.ps1" -Action Start`
4. Play and exit game.
5. Stop logging: `& "$HOME\Downloads\battlezone-netcode-patch-master\Microslop\tester_diag.ps1" -Action Stop`
6. Send the generated `.zip` bundle from `test_bundles`.

Linux (all Steam variants):

1. Start logging:
	`./Linux/tester_diag.sh start "/path/to/Battlezone 98 Redux"`
2. Play and exit game.
3. Stop logging:
	`./Linux/tester_diag.sh stop`
4. Send the generated `.tar.gz` bundle from `test_bundles`.

Linux note: Proton logs are copied with a 64 MB cap per log by default. To skip Proton log copy on stop, run:
`DISABLE_PROTON_LOG_COPY=1 ./Linux/tester_diag.sh stop`

---

## What Is Captured

- Game logs and proxy logs.
- Route/path diagnostics and interface counters.
- Crash data (Windows dumps when `procdump.exe` is installed).
- Baseline ping timeline and peer candidate inference from socket metadata.
- Linux Proton logs are included but capped to 64 MB per `steam-*.log` by default.

---

## Windows

### Step 1: Start Logging

Open PowerShell as Administrator, then enable scripts for this session:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
```

Then in the repo folder run:

```powershell
.\Microslop\tester_diag.ps1 -Action Start
```

If your prompt is already inside `...\Microslop>`, run this instead:

```powershell
.\tester_diag.ps1 -Action Start
```

If you are not in the repo folder, run with full path:

```powershell
& "$HOME\Downloads\battlezone-netcode-patch-master\Microslop\tester_diag.ps1" -Action Start
```

The script will attempt to log any errors including lag and CTD.

### Step 2: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Exit the game

### Step 3: Stop Logging And Send Bundle

After the match, run:

```powershell
.\Microslop\tester_diag.ps1 -Action Stop
```

If your prompt is `...\Microslop>`, use:

```powershell
.\tester_diag.ps1 -Action Stop
```

If you are not in the repo folder, use:

```powershell
& "$HOME\Downloads\battlezone-netcode-patch-master\Microslop\tester_diag.ps1" -Action Stop
```

---

## Linux - Native Steam

Use this if you installed Steam natively. If you installed Steam via Snap or Flatpak, use the sections below.

### Step 1: Start Logging

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh start "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

The script will attempt to log any errors including lag and CTD.

### Step 2: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Exit the game

### Step 3: Stop Logging And Send Bundle

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh stop
```

---

## Proton Log Controls (Linux)

- Keep Proton logging enabled for crash/startup correlation.
- To reduce bundle size at stop time:
	`PROTON_LOG_MAX_MB=16 ./Linux/tester_diag.sh stop`
- To skip Proton log copy entirely at stop time:
	`DISABLE_PROTON_LOG_COPY=1 ./Linux/tester_diag.sh stop`

---

## Privacy Scope

- Does not capture chat logs.
- Does not capture packet payloads.
- Proxy environment is logged as flags only, not credentials.

---

## Output Bundles

- Windows: `.zip` under `test_bundles/deep_*`
- Linux: `.tar.gz` under `test_bundles/deep_*`

---

## Linux - Snap Steam

Use this if you installed Steam via Snap (`snap install steam`).

### Step 1: Start Logging

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh start "/home/$USER/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

The script will attempt to log any errors including lag and CTD.

### Step 2: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Exit the game

### Step 3: Stop Logging And Send Bundle

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh stop
```

---

## Linux - Flatpak Steam

Use this if you installed Steam via Flatpak (`flatpak install steam`).

### Step 1: Start Logging

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh start "/home/$USER/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux"
```

The script will attempt to log any errors including lag and CTD.

### Step 2: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Exit the game

### Step 3: Stop Logging And Send Bundle

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh stop
```
