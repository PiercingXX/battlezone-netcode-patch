# Battlezone Netcode Patch

Final release repository for the Battlezone 98 Redux netcode patch.

This project is focused only on shipping the two game-folder proxy DLL paths:

1. Linux (Proton): `dsound.dll`
2. Windows: `winmm.dll`

Target socket values forced by the patch:

1. `SO_SNDBUF = 524288`
2. `SO_RCVBUF = 4194304`

## Repository Layout

1. `Linux/deploy_linux.sh`: Build and deploy Linux/Proton `dsound.dll`.
2. `Linux/proton_dsound_proxy/`: Linux proxy source and build.
3. `Microslop/deploy_windows.ps1`: Deploy Windows `winmm.dll`.
4. `Microslop/winmm.dll`: Prebuilt Windows drop-in DLL.
5. `Microslop/winmm_proxy/`: Windows proxy source and build.

## Linux Deploy

Install tools:

```bash
sudo apt install mingw-w64 make
```

Deploy for native Steam:

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/deploy_linux.sh "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

Steam launch options on Linux/Proton:

```text
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

For Snap Steam, use:

```text
/home/$USER/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux
```

For Flatpak Steam, use:

```text
/home/$USER/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux
```

## Windows Deploy

From PowerShell on the Windows machine:

```powershell
cd "path\to\battlezone-netcode-patch"
.\Microslop\deploy_windows.ps1
```

If Steam is installed in a non-default path:

```powershell
.\Microslop\deploy_windows.ps1 -GamePath "D:\Steam\steamapps\common\Battlezone 98 Redux"
```

No Steam launch option changes are required on Windows.
