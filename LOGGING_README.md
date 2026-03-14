# All the logs (Lag + Crash Investigation)

The logging captures network/system diagnostics plus game/proxy logs, then builds a bundle to send back.


## Privacy Scope

- Captures game logs, proxy logs, route/path diagnostics, and interface counters.
- Does not capture chat logs.
- Does not capture packet payloads.
- Proxy environment is captured as enabled/disabled flags only, not proxy credentials.


## Required

I need BOTH host and client logging to ensure this is actually viable and to see what improvements will need to be made. All players need to upload logging bundles, Discord should work for this.


## Notes

- Windows `netsh` ETW capture usually requires elevated PowerShell (Run as Administrator).
- Windows crash dumps are collected automatically if `procdump.exe` is installed.
- Linux Proton logs are captured when Steam launch options include: `PROTON_LOG=1 WINEDLLOVERRIDES="dsound=n,b" %command% -nointro`.
- Bundles are written under `test_bundles/deep_*` as `.zip` (Windows) or `.tar.gz` (Linux).
- Bundles always include baseline ping timeline data (`ping_timeline.log`).
- When peer detection succeeds, bundles include `peer_candidates.txt`, `inferred_peer_target.txt` (when inferred), and peer route diagnostics.
- Linux captures periodic baseline `mtr` timelines when `mtr` is installed, and peer `mtr` timelines when an explicit peer target is provided.
- Start/end snapshots include route traces, interface counters, queue/congestion snapshots, and network noise profile signals.
