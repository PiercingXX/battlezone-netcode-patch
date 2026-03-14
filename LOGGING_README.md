# Deep Diagnostics (Lag + Crash Investigation)

Use this for bad sessions only (lag spikes, desync, freeze, CTD/crash).

## Fast Flow

1. Start logging.
2. Play and exit game.
3. Stop logging.
4. Send bundle archive to devs.

Both host and client should log and upload bundles for the same match.

## Windows (Noob Safe Commands)

Open PowerShell as Administrator.

Enable scripts for this PowerShell session:

`Set-ExecutionPolicy -Scope Process Bypass -Force`

Start logging:

`& "$HOME\Downloads\battlezone-netcode-patch-master\Microslop\tester_diag.ps1" -Action Start`

Stop logging:

`& "$HOME\Downloads\battlezone-netcode-patch-master\Microslop\tester_diag.ps1" -Action Stop`

If your extracted folder name is `battlezone-netcode-patch-main`, replace `...-master` in both commands.

## Linux (Any Steam Install)

Start logging:

`./Linux/tester_diag.sh start "/path/to/Battlezone 98 Redux"`

Stop logging:

`./Linux/tester_diag.sh stop`

Native Steam path example:

`./Linux/tester_diag.sh start "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"`

Snap Steam path example:

`./Linux/tester_diag.sh start "/home/$USER/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux"`

Flatpak Steam path example:

`./Linux/tester_diag.sh start "/home/$USER/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux"`

## What Is Captured

- Game logs and proxy logs.
- Route/path diagnostics and interface counters.
- Crash data (Windows dumps when `procdump.exe` is installed).
- Baseline ping timeline and peer candidate inference from socket metadata.
- Linux Proton logs are included but capped to 64 MB per `steam-*.log` by default.

## Proton Log Controls (Linux)

- Keep Proton logging enabled for crash/startup correlation.
- To reduce bundle size further at stop time:
	`PROTON_LOG_MAX_MB=16 ./Linux/tester_diag.sh stop`
- To skip Proton log copy entirely at stop time:
	`DISABLE_PROTON_LOG_COPY=1 ./Linux/tester_diag.sh stop`

## Privacy Scope

- Does not capture chat logs.
- Does not capture packet payloads.
- Proxy environment is logged as flags only, not credentials.

## Output Bundles

- Windows: `.zip` under `test_bundles/deep_*`
- Linux: `.tar.gz` under `test_bundles/deep_*`
