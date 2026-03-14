# Battlezone Netcode Patch - Tester Guide

This repo helps test larger multiplayer socket buffers for Battlezone 98 Redux.

Target values:

- Send buffer: 524288
- Receive buffer: 2097152

> **These instructions assume you downloaded this repo as a ZIP from GitHub and extracted it to your Downloads folder.**
> All commands below are fully copy-pasteable — `$USER` and `$HOME` expand automatically to your username and home folder.


How to use this patch:
0. Download this patch. Your repo should stay in your Downloads directory.
1. Ensure patch DLL/proxy is in the game folder (one-time setup). Thats it. 
(For Testing ONLY) 
2. Start logging.
3. Play game.
4. Exit game.
5. Stop logging.
6. Send the generated bundle archive to devs (not the script itself).

Ideally we want to have no more than 3 games logged. 

**IF YOU CRASH, STOP LOGGING AND SEND BUNDLE BEFORE RESTARTING LOGGING AND THEN BATTLEZONE**

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

## Windows

### Step 1: Copy the DLL

1. Open Steam
2. Right-click **Battlezone 98 Redux** in your library
3. Click **Manage → Browse local files**
4. A folder opens — this is your game folder
5. Copy `Microslop\winmm.dll` from this repo into that game folder


![alt text](resources/image.png)

![alt text](resources/iaWY5xDy9t.gif)

### Step 2: Start Logging

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

### Step 3: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Exit the game

### Step 4: Stop Logging And Send Bundle

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

### Step 1: Install required tools

Open a terminal and run the command for your distro:

**Debian:**
```bash
sudo apt install mingw-w64 make
```

**Arch:**
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
2. Right-click **Battlezone 98 Redux** in your library
3. Click **Properties**
4. Click **General** on the left
5. Find the **Launch Options** box at the bottom
6. Paste this into it:

```
PROTON_LOG=1 WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

7. Close the window

### Step 4: Start Logging

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh start "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

The script will attempt to log any errors including lag and CTD.

### Step 5: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Exit the game

### Step 6: Stop Logging And Send Bundle

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh stop
```

---

## Linux - Snap Steam

Use this if you installed Steam via Snap (`snap install steam`).

### Step 1: Install required tools

Open a terminal and run the command for your distro:

**Debian:**
```bash
sudo apt install mingw-w64 make
```

**Arch:**
```bash
sudo pacman -S mingw-w64-gcc make
```

### Step 2: Deploy the patch

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/deploy_linux.sh "/home/$USER/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

> If this fails with "Missing game executable", your Snap Steam path is different.
> Open Steam → right-click Battlezone 98 Redux → **Manage → Browse local files**,
> then open a terminal in that folder and run `pwd` to get the exact path.
> Replace the path above with that.

### Step 3: Set Steam launch options

1. Open Steam
2. Right-click **Battlezone 98 Redux** in your library
3. Click **Properties**
4. Click **General** on the left
5. Find the **Launch Options** box at the bottom
6. Paste this into it:

```
PROTON_LOG=1 WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

7. Close the window

### Step 4: Start Logging

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh start "/home/$USER/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

The script will attempt to log any errors including lag and CTD.

### Step 5: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Exit the game

### Step 6: Stop Logging And Send Bundle

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh stop
```

---

## Linux - Flatpak Steam

Use this if you installed Steam via Flatpak (`flatpak install steam`).

### Step 1: Install required tools

Open a terminal and run the command for your distro:

**Debian:**
```bash
sudo apt install mingw-w64 make
```

**Arch:**
```bash
sudo pacman -S mingw-w64-gcc make
```

### Step 2: Deploy the patch

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/deploy_linux.sh "/home/$USER/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux"
```

> If this fails with "Missing game executable", your Flatpak Steam path is different.
> Open Steam → right-click Battlezone 98 Redux → **Manage → Browse local files**,
> then open a terminal in that folder and run `pwd` to get the exact path.
> Replace the path above with that.

### Step 3: Set Steam launch options

1. Open Steam
2. Right-click **Battlezone 98 Redux** in your library
3. Click **Properties**
4. Click **General** on the left
5. Find the **Launch Options** box at the bottom
6. Paste this into it:

```
PROTON_LOG=1 WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

7. Close the window

### Step 4: Start Logging

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh start "/home/$USER/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux"
```

The script will attempt to log any errors including lag and CTD.

### Step 5: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Exit the game

### Step 6: Stop Logging And Send Bundle

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh stop
```


---


## Important Note

The Battlezone startup text line can still show old values even when the patch is working.
Use proxy log readback (`dsound_proxy.log` or `winmm_proxy.log`) as source of truth.

## Technical Details

- Full investigation history: `INVESTIGATION_WRITEUP.md`
