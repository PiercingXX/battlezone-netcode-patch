
## Deep Diagnostics (Lag + Crash Investigation)

Use this only for bad sessions (heavy lag, desync, crash, freeze). It captures deeper network/system data.

Privacy scope:

- Captures game logs, proxy logs, route/path diagnostics, interface counters, and timing markers.
- Does not capture chat logs.
- Does not capture packet payloads.
- Proxy environment is captured as enabled/disabled flags only, not proxy credentials.

If your repo is not in `~/Downloads/battlezone-netcode-patch-master`, replace that path in all commands below.

## Required Test Rule (Dual-Side Collection)

For every bad match, both host and client must run deep diagnostics and both must upload bundles.

- Both sides include the same short match label in markers (for example: `match-20260314-a`).
- The scripts will try to auto-detect likely peer IPs from captured match traffic.

Windows:

```powershell
.\Microslop\tester_diag.ps1 -Action Start
```

Run your match, then stop and bundle:

```powershell
.\Microslop\tester_diag.ps1 -Action Stop
```

Optional marker during test:

```powershell
.\Microslop\tester_diag.ps1 -Action Mark -Message "match-20260314-a lag spike after spawn"
```

Linux Native Steam:

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh start "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

Run your match, then stop and bundle:

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh stop
```

Optional marker during test:

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh mark "match-20260314-a lag spike after spawn"
```

Linux Snap Steam:

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh start "/home/$USER/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

Run your match, then stop and bundle:

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh stop
```

Linux Flatpak Steam:

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh start "/home/$USER/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux"
```

Run your match, then stop and bundle:

```bash
cd ~/Downloads/battlezone-netcode-patch-master
./Linux/tester_diag.sh stop
```

Notes:

- Windows `netsh` ETW capture usually requires an elevated PowerShell (Run as Administrator).
- Windows crash dumps are collected automatically if `procdump.exe` is installed.
- Linux Proton logs are captured if Steam launch options include `PROTON_LOG=1 %command%`.
- Deep diagnostics bundles are written under `test_bundles/deep_*`.
- Bundles always include `ping_timeline.log` (baseline).
- When peer detection succeeds, bundles also include inferred peer candidates and peer route diagnostics.
- Linux captures periodic `mtr` timelines when installed: `mtr_peer_timeline.log` and `mtr_baseline_timeline.log`.
- Start/end snapshots now include peer and baseline route traces, interface counters, UDP queue/congestion snapshots, and network noise profile signals.
- Background network rate timelines are captured as byte/sec estimates for the default interface.
