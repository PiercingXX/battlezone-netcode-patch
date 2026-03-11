# Battlezone Netcode Patch - Tester Guide

This repo helps test larger multiplayer socket buffers for Battlezone 98 Redux.

Target values:

- Send buffer: 524288
- Receive buffer: 2097152

> **These instructions assume you downloaded this repo as a ZIP from GitHub and extracted it to your Downloads folder.**
> All commands below are fully copy-pasteable — `$USER` and `$HOME` expand automatically to your username and home folder.

---

## Linux - Native Steam

Use this if you installed Steam by downloading it from the Steam website or via `apt install steam`.

### Step 1: Install required tools

Open a terminal and run the command for your distro:

**Debian / Ubuntu / Mint:**
```bash
sudo apt install mingw-w64 make
```

**Arch / Manjaro / EndeavourOS:**
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
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

7. Close the window

### Step 4: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Exit the game

### Step 5: Verify

```bash
cd "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
VERIFY_PROXY_READBACK=1 ~/Downloads/battlezone-netcode-patch-master/Linux/verify_net_patch.sh
```

**Success = `VERIFY RESULT: PASS`**

---

## Linux - Snap Steam

Use this if you installed Steam via Snap (`snap install steam`).

### Step 1: Install required tools

Open a terminal and run the command for your distro:

**Debian / Ubuntu / Mint:**
```bash
sudo apt install mingw-w64 make
```

**Arch / Manjaro / EndeavourOS:**
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
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

7. Close the window

### Step 4: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Exit the game

### Step 5: Verify

```bash
cd "/home/$USER/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
VERIFY_PROXY_READBACK=1 ~/Downloads/battlezone-netcode-patch-master/Linux/verify_net_patch.sh
```

**Success = `VERIFY RESULT: PASS`**

---

## Linux - Flatpak Steam

Use this if you installed Steam via Flatpak (`flatpak install steam`).

### Step 1: Install required tools

Open a terminal and run the command for your distro:

**Debian / Ubuntu / Mint:**
```bash
sudo apt install mingw-w64 make
```

**Arch / Manjaro / EndeavourOS:**
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
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

7. Close the window

### Step 4: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Exit the game

### Step 5: Verify

```bash
cd "/home/$USER/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux"
VERIFY_PROXY_READBACK=1 ~/Downloads/battlezone-netcode-patch-master/Linux/verify_net_patch.sh
```

**Success = `VERIFY RESULT: PASS`**

---

## Windows

### Step 1: Copy the DLL

1. Open Steam
2. Right-click **Battlezone 98 Redux** in your library
3. Click **Manage → Browse local files**
4. A folder opens — this is your game folder
5. Copy `Microslop\winmm.dll` from this repo into that game folder

### Step 2: Play

1. Launch **Battlezone 98 Redux** from Steam
2. Go to **Multiplayer**
3. Exit the game

### Step 3: Verify

Open PowerShell in the repo folder and run:

```powershell
.\Microslop\verify_windows.ps1
```

**Success = `RESULT: PASS`**

---

## Important Note

The Battlezone startup text line can still show old values even when the patch is working.
Use proxy log readback (`dsound_proxy.log` or `winmm_proxy.log`) as source of truth.

## Technical Details

- Full investigation history: `INVESTIGATION_WRITEUP.md`


## Linux - Native Steam

Follow these steps if you installed Steam normally (not Snap or Flatpak).

### Prerequisites

Install tools (one time only):

```bash
sudo apt install mingw-w64 make
```

### Steps

1. **Open a terminal** in the repo folder

2. **Run the deploy script:**

```bash
./Linux/deploy_linux.sh "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

3. **Set Steam launch options:**
   - Open Steam
   - Right-click Battlezone 98 Redux
   - Click `Properties`
   - Under "General" find "Launch Options"
   - Paste: `WINEDLLOVERRIDES="dsound=n,b" %command% -nointro`
   - Close

4. **Play and verify:**
   - Launch the game from Steam
   - Go to multiplayer
   - Exit the game
   - Open a terminal in the game folder:
   
   ```bash
   cd "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
   ```

   - Then verify. Replace `/home/username/Downloads/battlezone-netcode-patch` with where you cloned this repo:

   ```bash
   VERIFY_PROXY_READBACK=1 "/home/username/Downloads/battlezone-netcode-patch/Linux/verify_net_patch.sh"
   ```

**Success = `VERIFY RESULT: PASS`**

---

## Linux - Snap Steam

Follow these steps if you installed Steam via Snap.

### Prerequisites

Install tools (one time only):

```bash
sudo apt install mingw-w64 make
```

### Steps

1. **Find your game path:**
   - Open Steam (Snap)
   - Right-click Battlezone 98 Redux
   - Click `Manage` → `Browse local files`
   - Copy the folder path shown (or type `pwd` in terminal there)

2. **Open a terminal** in the repo folder

3. **Run the deploy script** with your copied path:

```bash
./Linux/deploy_linux.sh "/your/copied/path"
```

Example (if your path is in snap):

```bash
./Linux/deploy_linux.sh "/home/$USER/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

4. **Set Steam launch options:**
   - Open Steam (Snap)
   - Right-click Battlezone 98 Redux
   - Click `Properties`
   - Under "General" find "Launch Options"
   - Paste: `WINEDLLOVERRIDES="dsound=n,b" %command% -nointro`
   - Close

5. **Play and verify:**
   - Launch the game from Steam
   - Go to multiplayer
   - Exit the game
   - Open a terminal in the game folder:
   
   ```bash
   cd "/your/copied/path"
   ```

   - Then verify. Replace `/home/username/Downloads/battlezone-netcode-patch` with where you cloned this repo:

   ```bash
   VERIFY_PROXY_READBACK=1 "/home/username/Downloads/battlezone-netcode-patch/Linux/verify_net_patch.sh"
   ```

**Success = `VERIFY RESULT: PASS`**

---

## Linux - Flatpak Steam

Follow these steps if you installed Steam via Flatpak.

### Prerequisites

Install tools (one time only):

```bash
sudo apt install mingw-w64 make
```

### Steps

1. **Find your game path:**
   - Open Steam (Flatpak)
   - Right-click Battlezone 98 Redux
   - Click `Manage` → `Browse local files`
   - Copy the folder path shown (or type `pwd` in terminal there)

2. **Open a terminal** in the repo folder

3. **Run the deploy script** with your copied path:

```bash
./Linux/deploy_linux.sh "/your/copied/path"
```

Example (if your path is in flatpak):

```bash
./Linux/deploy_linux.sh "/home/$USER/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux"
```

4. **Set Steam launch options:**
   - Open Steam (Flatpak)
   - Right-click Battlezone 98 Redux
   - Click `Properties`
   - Under "General" find "Launch Options"
   - Paste: `WINEDLLOVERRIDES="dsound=n,b" %command% -nointro`
   - Close

5. **Play and verify:**
   - Launch the game from Steam
   - Go to multiplayer
   - Exit the game
   - Open a terminal in the game folder:
   
   ```bash
   cd "/your/copied/path"
   ```

   - Then verify. Replace `/home/username/Downloads/battlezone-netcode-patch` with where you cloned this repo:

   ```bash
   VERIFY_PROXY_READBACK=1 "/home/username/Downloads/battlezone-netcode-patch/Linux/verify_net_patch.sh"
   ```

**Success = `VERIFY RESULT: PASS`**

---

## Windows

### What You Need

- Windows with Steam and Battlezone 98 Redux installed
- The file `Microslop/winmm.dll` from this repo

### Steps

1. **Find your game folder:**
   - Open Steam
   - Right-click Battlezone 98 Redux
   - Click `Manage` → `Browse local files`
   - A folder opens - that's your game folder

2. **Copy the DLL:**
   - Copy the file `Microslop/winmm.dll` from this repo
   - Paste it into your game folder (from step 1)

3. **Play and verify:**
   - Launch the game
   - Go to multiplayer
   - Exit the game
   - Open PowerShell in the repo folder and run:

```powershell
.\Microslop\verify_windows.ps1
```

**Success = `RESULT: PASS`**

---

## Important Note

The Battlezone startup text line can still show old values even when the patch is working.
Use proxy log readback (`dsound_proxy.log` or `winmm_proxy.log`) as source of truth.

## Technical Details

- Full investigation history: `INVESTIGATION_WRITEUP.md`
