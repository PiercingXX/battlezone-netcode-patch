
## Deep Diagnostics (Lag + Crash Investigation)

Use this only for bad sessions (heavy lag, desync, crash, freeze). It captures deeper network/system data.

If your repo is not in `~/Downloads/battlezone-netcode-patch-master`, replace that path in all commands below.

Windows:

```powershell
.\Microslop\start_deep_diag.ps1
```

Run your match, then stop and bundle:

```powershell
.\Microslop\stop_deep_diag.ps1
```

Linux Native Steam:

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/start_deep_diag.sh "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

Run your match, then stop and bundle:

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/stop_deep_diag.sh
```

Linux Snap Steam:

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/start_deep_diag.sh "/home/$USER/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

Run your match, then stop and bundle:

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/stop_deep_diag.sh
```

Linux Flatpak Steam:

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/start_deep_diag.sh "/home/$USER/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux"
```

Run your match, then stop and bundle:

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/stop_deep_diag.sh
```

Notes:

- Windows `netsh` ETW capture usually requires an elevated PowerShell (Run as Administrator).
- Windows crash dumps are collected automatically if `procdump.exe` is installed.
- Linux Proton logs are captured if Steam launch options include `PROTON_LOG=1 %command%`.
- Deep diagnostics bundles are written under `test_bundles/deep_*`.
