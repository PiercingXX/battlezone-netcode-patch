# Battlezone Netcode Patch Investigation Write-Up

## Scope

This document captures the current state of the Battlezone 98 Redux Linux runtime netcode patch investigation, including:

- What the patch repository does
- What was found in the installed game
- What was tested
- What succeeded
- What failed
- What can be ruled out
- What the next technically credible path is

This write-up reflects the investigation state as of 2026-03-11.

## Current Repository State

The repository has now been redefined around the working Linux and Proton startup-interception path.

Current active workflow files:

- `Linux/deploy_linux.sh`
- `Linux/run_test_linux.sh`
- `Linux/verify_net_patch.sh`
- `Linux/proton_dsound_proxy/`
- `Microslop/winmm.dll`
- `Microslop/deploy_windows.ps1`
- `Microslop/verify_windows.ps1`
- `Microslop/winmm_proxy/`
- `README.md`
- `INVESTIGATION_WRITEUP.md`

The old post-launch ptrace and Windows runtime-memory patch scripts were removed because they no longer represent the validated approach.

## Repository Review Summary

The patch repository originally started as a runtime-first memory patching workflow intended to avoid modifying the game executable on disk.

Relevant files reviewed during that earlier phase:

- `runtime_patch_linux.py`
- `runtime_patch_linux.sh`
- `launch_and_patch_linux.sh`
- `verify_net_patch.sh`
- `runtime_patch_windows.ps1`
- `verify_net_patch.ps1`
- `run_test_linux.sh`
- `run_test_windows.ps1`
- `README.md`

The design intent is:

1. Launch the game.
2. Find the running process.
3. Patch the two immediate values in process memory.
4. Verify through `BZLogger.txt` that the game reports the expected socket buffer sizes.

Target buffer values:

- Send buffer: `524288`
- Receive buffer: `2097152`

Expected verification line:

`BZRNet P2P Socket Opened With 2097152 received buffer, 524288 send buffer`

## Installed Game Review

The installed game was located at:

`/home/piercingxx/.local/share/Steam/steamapps/common/Battlezone 98 Redux`

Observed install characteristics:

- Approximate file count: `2254`
- Approximate size: `2.0G`

Important files confirmed:

- `battlezone98redux.exe`
- `BZLogger.txt`
- `multi.ini`
- Workshop-loaded `net.ini`

The active `net.ini` loaded by the game was found at:

`/home/piercingxx/.local/share/Steam/steamapps/workshop/content/301650/1895622040/net.ini`

That file corresponds to the current Workshop mod named:

`Auto-Kick Reduction Patch`

## What net.ini Actually Controls

The active `net.ini` contains these types of settings:

- `MaxPing`
- `UpCount`
- `DownCount`
- `MinBandwidth`
- `MaxBandwidth`
- `AutoKickStart`
- `AutoKickPing`
- `AutoKickLoss`
- `AutoKickTime`
- `UseCompression`

What it does **not** contain:

- Any send socket buffer size key
- Any receive socket buffer size key
- Any `SendBuffer`, `RecvBuffer`, `ReceiveBuffer`, `SocketSend`, `SocketRecv`, or equivalent field

Wider search across the game install also failed to find any file-based configuration entry for these socket buffer values.

## Runtime Patch Behavior Observed

The Linux runtime patcher successfully patches the running process memory.

Repeated successful in-memory results looked like this:

- `send now: 00000800`
- `recv now: 00002000`
- `Runtime patch applied in memory.`

This proves the following:

1. The process can be found.
2. The target addresses are correct for the tested build.
3. The patch bytes can be written successfully.
4. The patcher is not failing because of bad addresses or write errors.

## What the Game Logs Show

Despite successful in-memory patching, every verified game session continued to log default socket buffer values at startup.

Representative log evidence:

- Startup timestamp: `2026-03-11 11:11:41.354874`
- Socket open timestamp: `2026-03-11 11:11:41.384875`
- Logged values: `1048576 received buffer, 32768 send buffer`

This is the critical finding.

The socket initialization happens roughly `30 ms` after startup, which is far earlier than the runtime patching flow is currently able to influence.

## Tests Performed

### 1. Manual runtime patch after game launch

Process:

1. Launch game.
2. Run `runtime_patch_linux.sh`.
3. Enter multiplayer.
4. Verify latest log session.

Result:

- Patch succeeded in memory.
- Verification failed.
- Game still logged default socket buffer values.

Conclusion:

- Manual attach after launch is too late.

### 2. One-shot launcher plus patch flow

Process:

1. Launch using `launch_and_patch_linux.sh`.
2. Script waits for spawned process.
3. Script applies runtime patch.
4. Enter multiplayer.
5. Verify.

Result:

- Patch succeeded in memory.
- Verification still failed.
- Game still logged default socket buffer values.

Conclusion:

- Even the automated post-launch flow is still too late.

### 3. Inspect active Workshop net.ini

Process:

1. Read active `net.ini` loaded by the game.
2. Search for any socket buffer related keys.

Result:

- No configurable send or receive socket buffer parameters exist in the file.

Conclusion:

- `net.ini` cannot be used to control these values in this build.

### 4. Aggressive pre-init suspend experiment

An experimental launcher variant was created that:

1. Waited for the game process to spawn.
2. Immediately sent `SIGSTOP`.
3. Patched the exact PID while paused.
4. Sent `SIGCONT` to resume.

Observed result:

- This approach triggered Steam/Proton startup instability.
- A visible application load error was observed:

`Application load error 0:0000065432`

Interpretation:

- Suspending the process that early appears to disrupt Steam/Proton startup or app bootstrap.

Conclusion:

- Pre-init suspend is too invasive for safe default use.
- That mode was removed again.

### 5. ptrace policy test with `kernel.yama.ptrace_scope=0`

Rationale:

- If ptrace restrictions were adding enough latency to miss the 30 ms startup window, lowering `ptrace_scope` might allow the patcher to attach soon enough.

Process:

1. Temporarily changed Linux ptrace policy to `0`.
2. Killed any old game process.
3. Launched with the cleaned launcher.
4. Patch succeeded in memory.
5. Entered multiplayer.
6. Verified latest log session.

Result:

- Patch still succeeded in memory.
- Verification still failed.
- Logged socket buffer values remained default.

Conclusion:

- `ptrace_scope=0` did not solve the problem.
- The core blocker is not ptrace policy anymore.

## What Can Be Ruled Out

The following have effectively been ruled out:

### Bad target addresses

No. The patcher repeatedly writes the expected values to the expected process memory locations.

### net.ini misconfiguration

No. The active `net.ini` does not expose the socket buffer settings being targeted.

### Verification script bug

No. The logs consistently show the game itself reporting default socket buffers at startup.

### ptrace policy as the root cause

No. Lowering `ptrace_scope` to `0` did not change the outcome.

### Basic launch sequencing mistake

No. Manual patching, one-shot launch+patch, and ptrace-policy-adjusted launch+patch all converged on the same result.

## What the Evidence Means

The runtime patch succeeds, but it succeeds **after** the game has already initialized the P2P socket using the default values.

That means the current runtime patching strategy is architecturally too late for this target.

The actual problem is timing, not patch correctness.

More specifically:

- The game starts.
- The game opens the P2P socket almost immediately.
- The game reports the default buffer sizes.
- The runtime patch lands only afterward.

At that point, the patch changes memory, but it does not retroactively change the already-created socket configuration that the game is using.

## Important User-Provided Constraint

There is prior evidence from earlier testing that modifying the executable on disk causes runtime errors.

Because of that, direct on-disk EXE patching is not the preferred route at this time.

This means the remaining credible solution space is focused on pre-start or startup-time interception rather than traditional binary patching.

## Current State of the Scripts

The Linux launcher and patch wrapper were adjusted during investigation to improve safety and clarity.

Current useful changes retained:

- `runtime_patch_linux.sh` can target an explicit PID through `BATTLEZONE_TARGET_PID`
- `launch_and_patch_linux.sh` passes the detected PID into the runtime patcher
- The risky pre-init suspend mode was removed again

This leaves the safer live-process patch path in place, even though it is now proven insufficient for the final socket-buffer objective.

## Best Technical Conclusion So Far

The investigation currently supports this conclusion:

**Post-launch ptrace patching is not a viable way to change Battlezone 98 Redux socket buffer sizes on Linux/Proton for this build.**

That conclusion is based on:

- Successful memory patching
- Repeated failed behavioral verification
- Proven lack of file-based config for these values
- Failed aggressive suspend experiment
- Failed ptrace policy relaxation experiment

## Recommended Next Step

The next technically credible path is a **pre-start injection/interception approach**.

Most plausible options on Linux/Proton:

1. A Windows DLL override loaded by Proton at process start.
2. A Proton/Wine launch wrapper that intercepts the relevant networking API calls before the game configures the socket.
3. A carefully designed startup-time injection method that runs before the game reaches socket initialization.

Of those, the least invasive next research direction is likely:

**DLL override or startup injection under Proton**, so the game starts with the hook already present instead of trying to attach afterward.

## Addendum: DSOUND Native Override Experiment

A `dsound.dll` native override was implemented and tested via Steam launch options:

`WINEDLLOVERRIDES="dsound=n,b" %command% -nointro`

### What was observed

1. The proxy DLL did load in process startup.
2. Diagnostic logs showed successful in-process patching of target bytes:
	 - Before: `send=00800000 recv=00001000`
	 - After: `send=00000800 recv=00002000`
3. Startup produced `Application load error 0:0000065432` in some runs.

### Interpretation

This indicates startup-time patching can be reached, but modifying those code bytes at that stage appears to conflict with game/launcher startup integrity or load flow under Steam/Proton.

### Outcome

- DSOUND native override path is currently considered unstable for production use.
- The override file was rolled back by renaming:
	- `dsound.dll` -> `dsound.dll.disabled`

## Addendum: DSOUND API Interception Experiment

After startup byte patching proved unstable, the DSOUND proxy was refactored to avoid direct game code-byte edits and instead intercept socket option APIs.

### Interception strategy implemented

1. Load proxy via Steam launch option override:
	- `WINEDLLOVERRIDES="dsound=n,b" %command% -nointro`
2. Install startup hooks in the game process:
	- `setsockopt`
	- `WSASetSocketOption`
3. Force values when socket options are set:
	- `SO_RCVBUF = 2097152`
	- `SO_SNDBUF = 524288`
4. Add timestamped diagnostics to `dsound_proxy.log`.

### What was observed

From `dsound_proxy.log` in the latest run:

- `11:47:27.994` hooks installed before first socket log line
- `11:47:28.252` forced `SO_RCVBUF=2097152`
- `11:47:28.253` forced `SO_SNDBUF=524288`

From `BZLogger.txt` in the same run:

- `11:47:28.253` `BZRNet P2P Socket Opened With 1048576 received buffer, 32768 send buffer`

### Interpretation

This is a key result:

- Interception is active.
- Interception timing is early enough.
- Forced values are being submitted.
- The game still reports default effective socket buffer values.

This suggests one of the following:

1. Values are clamped/overridden downstream (Wine/OS/network stack behavior).
2. Another subsequent socket option call resets values.
3. The logged line reflects effective values from a different code path/socket than the intercepted call pair.

### Conclusion from Addendum 2

The problem is no longer “can we run early enough” or “can we intercept calls.”

The unresolved part is now **effective value control** under this Proton/Wine runtime path.

## Addendum: Immediate Readback Instrumentation Result (2026-03-11)

The DSOUND proxy was extended to perform immediate post-set readback using the real `getsockopt` call on the same socket handle after forcing values.

### What was observed in the same startup instant

From `dsound_proxy.log`:

- `12:02:40.500` forced `SO_RCVBUF=2097152`, return `rc=0`, `wsa=0`
- `12:02:40.500` immediate readback: `SO_SNDBUF=212992`, `SO_RCVBUF=2097152`
- `12:02:40.500` forced `SO_SNDBUF=524288`, return `rc=0`, `wsa=0`
- `12:02:40.500` immediate readback: `SO_SNDBUF=524288`, `SO_RCVBUF=2097152`

From `BZLogger.txt` in that exact run:

- `12:02:40.500449` `BZRNet P2P Socket Opened With 1048576 received buffer, 32768 send buffer`

### Interpretation

This result changes the confidence level significantly:

1. The forced values are not just requested; they are immediately readable as effective values on the socket handle we intercepted.
2. The Battlezone startup socket log line is not reflecting those immediate effective values for that call path.

Most likely explanations now:

1. The log line is sourced from internal/default constants rather than a live `getsockopt` readback.
2. The log line reflects a different socket path than the one intercepted at that instant.
3. A later reset may still occur, but that is now a post-start sequencing issue rather than initial-apply failure.

### Practical status after Addendum 3

The startup interception path is now proven to set and read back target values successfully on at least one real startup socket configuration call.

## Addendum: Socket Lifecycle Correlation (2026-03-11)

Per-socket lifecycle tracing was added by hooking `socket` and `closesocket` and tagging each socket handle with a stable `sid` value in `dsound_proxy.log`.

### What was observed

In the latest run:

1. Startup force/readback happened on `sid=1` / `sock=0x000000d8`.
2. Immediate readback on `sid=1` confirmed:
	- `SO_RCVBUF=2097152`
	- `SO_SNDBUF=524288`
3. No later `setsockopt` reset was observed on `sid=1`.
4. `sid=1` was only closed near shutdown (`12:07:43.947`).

At the same startup moment, `BZLogger.txt` still printed default values.

### Interpretation

The socket actually used in the intercepted startup path retained the forced values without a later reset event in this session.

Therefore, the startup BZLogger socket line should be treated as non-authoritative for effective buffer state in this Proton path.

This is no longer a timing failure or a force-call failure.

### Operational adjustment

`Linux/verify_net_patch.sh` now supports a proxy-readback validation mode:

```bash
VERIFY_PROXY_READBACK=1 /path/to/Battlezone\ Netcode\ Patch/Linux/verify_net_patch.sh
```

In this mode, verification can pass when `dsound_proxy.log` contains effective readback values matching target send/receive buffers, even if the startup BZLogger text line remains default.

## Fresh-Agent Handoff Notes

If a fresh AI agent continues this work, start from these facts:

1. Post-launch ptrace patching: verified too late.
2. net.ini path: does not expose send/recv socket buffer keys.
3. Startup byte-patching via DSOUND override: reaches target bytes, but caused startup instability in some runs.
4. API interception via DSOUND override: hooks fire and force values at startup, but logged effective values remain defaults.
5. Immediate readback plus socket lifecycle tracing now show the forced values remain applied on the intercepted startup socket.

Recommended immediate next instrumentation for handoff:

1. Track socket lifecycle (`socket`, `closesocket`) and assign per-socket IDs in logs.
2. Log later `setsockopt` calls on the same socket to detect post-start resets.
3. Optionally hook `sendto`/`recvfrom` to identify which socket handle is actually used for P2P traffic and correlate with the one that received forced values.

Those first two items are now complete in the current repo state.

## Windows Follow-Up

Native Windows support is now implemented in this repository using a `winmm.dll` proxy.

Current Windows path:

1. Deploy `Microslop/winmm.dll` into the game directory as `winmm.dll`.
2. Launch game normally from Steam (no launch option override needed).
3. Validate with `Microslop/verify_windows.ps1`.

Implementation notes:

- The proxy lives in `Microslop/winmm_proxy/`.
- It installs a `WSASocketW` IAT hook and applies the same target values:
	- `SO_SNDBUF = 524288`
	- `SO_RCVBUF = 2097152`
- Verification source of truth is `winmm_proxy.log`.

Remaining Windows work, if needed, is incremental hardening (game-update compatibility and broader tester validation), not initial implementation.

## Operational Notes

During the `ptrace_scope` test, the system ptrace policy was changed to:

`kernel.yama.ptrace_scope = 0`

If normal system policy should be restored afterward, the expected command is:

```bash
sudo sysctl -w kernel.yama.ptrace_scope=1
```

## Final Summary

What was learned:

- The original runtime-memory patch repository was useful for proving timing failure, but it is no longer the right operational workflow.
- The game does not expose these socket buffer values through `net.ini`.
- The active Workshop `Auto-Kick Reduction Patch` is unrelated to the target send/receive socket buffer values.
- The game initializes the relevant socket too early for post-launch patching to matter.
- Lowering ptrace restrictions does not solve it.
- Forcing an early suspend breaks startup reliability.
- Startup interception through a Proton `dsound.dll` proxy does apply the target values successfully.
- `BZLogger.txt` is not authoritative for effective socket state in this path; `dsound_proxy.log` readback is.
- Native Windows support is now implemented through the `winmm.dll` proxy path.

Working conclusion:

**Reliable behavior in this project comes from startup interception (Linux: `dsound.dll` via Proton override, Windows: `winmm.dll` proxy), not post-launch runtime patching.**