# Manual install

For when you'd rather not pipe a script into your shell, or you need to patch a
second Steam install.

These steps assume the repo is at `~/Downloads/battlezone-netcode-patch`.

## Linux / Proton

### 1. Build tools

Debian / Ubuntu:

```bash
sudo apt install mingw-w64 make
```

Arch / Manjaro:

```bash
sudo pacman -S mingw-w64-gcc make
```

Fedora:

```bash
sudo dnf install mingw32-gcc-c++ make
```

### 2. Raise the kernel UDP buffer limits

Without this the kernel silently clamps the patch's enlarged socket buffers to
around 208 KB, and you lose most of the burst tolerance.

```bash
sudo sysctl -w net.core.rmem_max=4194304 net.core.wmem_max=524288
printf 'net.core.rmem_max=4194304\nnet.core.wmem_max=524288\n' \
  | sudo tee /etc/sysctl.d/99-battlezone-netcode.conf
```

### 3. Deploy

Builds from source, installs `dsound.dll` and the `net.ini` mod, and runs the
EXU compatibility repair:

```bash
cd ~/Downloads/battlezone-netcode-patch
./Linux/deploy_linux.sh "/path/to/steamapps/common/Battlezone 98 Redux"
```

Common game paths:

| Steam flavour | Path |
|---|---|
| Native | `~/.local/share/Steam/steamapps/common/Battlezone 98 Redux` |
| Flatpak | `~/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux` |
| Snap | `~/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux` |

### 4. Steam launch options

Right-click Battlezone 98 Redux → Properties → Launch Options:

```text
WINEDLLOVERRIDES=dsound=n,b %command% -nointro
```

**Without this the DLL is never loaded.** Nothing else works until it's set.

### Two Steam installs?

Native and Flatpak Steam are entirely separate: separate game folders, separate
launch options, separate `dsound.dll`. Run `deploy_linux.sh` once per install
and set the launch options in each one. Verify each independently — the verify
script checks the folder you point it at.

## Windows

1. Use the prebuilt DLL from `prebuilt/windows/winmm.dll`, and verify it:

   ```powershell
   sha256sum -c prebuilt/windows/winmm.dll.sha256
   ```

   Or build it yourself: `cd Microslop/winmm_proxy && make` →
   `build/winmm.dll`.

2. Copy `winmm.dll` into the game folder, next to `battlezone98redux.exe`:

   ```text
   ...\steamapps\common\Battlezone 98 Redux\
   ```

3. Optionally copy `net-ini/net.ini` to
   `...\Battlezone 98 Redux\packaged_mods\9990001\net.ini`. This is a fallback
   only — as of V4.7 the proxy writes the same values into memory, which is
   the path that actually works.

4. Launch normally. There are no launch options to set on Windows.

**Defender note:** some users see `winmm.dll` quarantined as
`Program:Win32/Contebrew.A!ml`, a heuristic detection common for unsigned DLL
proxies. Restore it from Protection History and add an exception for that one
file in the game folder. Don't disable AV globally.

Full Windows notes: [../Microslop/winmm_proxy/README.md](../Microslop/winmm_proxy/README.md)

## Uninstall

Delete `dsound.dll` (Linux) or `winmm.dll` (Windows) from the game folder, and
remove the launch options on Linux. Optionally delete
`packaged_mods/9990001/`. Nothing else was modified — the patch never writes to
the game executable.
