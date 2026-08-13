# Battlezone 98 Redux Netcode Patch — V4.94 experimental

> **This is the experimental branch.** It is for the test crew and it changes
> under you. Stable players belong on
> [master (V4.8)](https://github.com/PiercingXX/battlezone-netcode-patch/tree/master).

Battlezone caps every player's send rate at **16,000 B/s** and opens each match
at a 4,000 B/s trickle. Lifting both took measured matches to 82,000+ B/s and
cut visible position corrections by 55-70% on identical maps.

The patch is a DLL proxy that writes the game's own network tuning into memory,
enlarges the socket buffers, and marks your traffic for router priority. No game
code is modified.

**Everyone in a test lobby should run the same build** — the tuning is
per-machine, and every V4.9 DLL logs its own build id so a log can always
answer "which build was that?".

Current version: **V4.94 experimental** · [CHANGELOG](CHANGELOG.md)

---

## Windows

**Test crew:** use the install command pinned in the private Discord channel —
it is this one with the webhook already in it, so everything below happens
automatically and nothing prompts you.

**1. Run one command — in PowerShell** (press Start, type `powershell`,
Enter; the blue window, not Command Prompt):

```powershell
irm https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/experimental/v4.9/install/install_windows.ps1 | iex
```

The script installs this branch on its own — no `$env:` prefix needed. (The
old command wrapped this in `powershell -Command "..."` for pasting into
Command Prompt; pasted into PowerShell instead, the outer shell ate the
`$env:` variables and the installer silently fell back to `master`. The
pinned test-crew command adds only the webhook in front, and must also be
pasted into PowerShell.)

Installs the prebuilt `winmm.dll` (verified against its SHA256 sidecar), the
tuning mod, and — with the pinned command — the log uploader, fully
configured. No questions asked.

**2. Paste this into the Steam launch options** (Steam → Battlezone 98 Redux →
Properties → Launch Options):

```text
cmd /c ""%LOCALAPPDATA%\bz-netcode\bz_wrap.bat" %command%"
```

That's it. Same line on every Windows machine — nothing to edit. Copy it
exactly: **the doubled quotes at each end are required.** `cmd` only preserves
quotes when there are exactly two on the line, and `%command%` already brings
its own pair, so without the outer pair `cmd` strips the wrong ones and the
game never starts — no splash, no log, no error. A console window stays open
while the game runs; that is the wrapper waiting to bundle your logs on exit.
Closing it kills the upload, not the game.

Installed from the plain command above instead of the pinned one? Then there
is no uploader and step 2 does not apply — the patch itself still works.

<details>
<summary>If Windows blocks it ("potentially malicious/unwanted app", or
<code>winmm.dll</code> disappears)</summary>

`winmm.dll` is an unsigned, MinGW-built proxy that hooks the game's
networking — exactly the shape antivirus heuristics flag, commonly as
`Program:Win32/Contebrew.A!ml`. It shows up two ways:

- **The install command fails** with *"the file contains a virus or
  potentially unwanted software"* — real-time protection blocked the
  download.
- **The game runs unpatched, or `winmm.dll` vanishes from the game folder**
  — Defender quarantined it moments after install. The installer now
  detects this and tells you.

Fix, keeping Defender on (never disable AV globally):

1. Windows Security → Virus & threat protection → **Protection history**
2. Find the block → **Actions → Allow** (or Restore)
3. Or allow just this one file from an admin PowerShell:
   `Add-MpPreference -ExclusionPath "C:\...\Battlezone 98 Redux\winmm.dll"`
4. Re-run the install command.

Why allowing it is reasonable: the installer verifies the DLL's SHA256
against the sidecar published in this repo, so what you allow is
bit-for-bit the build whose source you can read here — and you can build it
from that source yourself instead of using the prebuilt at all.

**Third-party antivirus (Bitdefender, etc.):** same idea, different UI —
the `Add-MpPreference` command only configures Windows Defender, and if a
third-party AV is installed, Defender usually isn't even the one blocking.
Add two exceptions in that product's own settings (Bitdefender: Protection →
Antivirus → Settings → Manage Exceptions): the game folder (for
`winmm.dll`) and `%LOCALAPPDATA%\bz-netcode` (for the uploader), and restore
anything already quarantined. In Bitdefender, also add the game to
Advanced Threat Defense's exceptions — it watches running programs, and a
DLL that hooks the game's networking is exactly what it likes to kill
mid-match.

**What an AV-blocked uploader actually looks like** (field-verified against
Bitdefender): the install output shows `WARNING: Upload wrapper setup
failed: Access to the path '...\bz_wrap.ps1' is denied` and a red "THE LOG
UPLOADER DID NOT INSTALL" block — and then still ends with a green "Install
complete", because that last line is about the DLL, which installed fine.
It is easy to scroll past. The tell from the outside: bundles never arrive
and `%LOCALAPPDATA%\bz-netcode\bz_wrap.log` does not exist, no matter how
many times the install command is re-run — every run hits the same wall and
leaves whatever wrapper was already there. Worse, a blocked run can delete
the old `bz_wrap.ps1` before the denied write, so the Steam launch option
now points at a wrapper that cannot run and the game will not start through
it. Recovery, in order:

1. Clear the Steam launch options — the game and patch work fine unwrapped.
2. Add the two folder exceptions above in the AV's own UI and restore
   anything it quarantined.
3. In PowerShell: `Remove-Item -Recurse -Force "$env:LOCALAPPDATA\bz-netcode"`
   — clears the locked or ACL-broken leftovers the AV created.
4. Re-run the pinned install command. Success prints
   *"Automatic log upload configured for …"* and no red block.
5. Put the launch option back. To double-check the wrapper is current:
   `Select-String wrapper_version= "$env:LOCALAPPDATA\bz-netcode\bz_wrap.ps1"`
   — the line it prints is the installed generation; compare it against the
   latest one named in `CHANGELOG.md` (currently `V4.92-arms-20260803`)
   should return a match, and `bz_wrap.log` appears in that folder after the
   next launch.
</details>

## Linux / Proton

**Test crew:** use the install command pinned in the private Discord channel —
it is this one with the webhook already in it, so the uploader configures
itself and nothing prompts you.

**1. Run one command** (builds `dsound.dll` from this branch's source on your
machine, installing the MinGW cross-compiler first if it is missing):

```bash
curl -fsSL https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/experimental/v4.9/install/install_linux.sh | bash
```

**2. Paste this into the Steam launch options** — on Linux this step is **not
optional**: without `WINEDLLOVERRIDES` the DLL sits in the game folder and
never loads, because Wine has its own dsound. The installer prints the exact
line; with the uploader it is:

```text
WINEDLLOVERRIDES=dsound=n,b "${XDG_DATA_HOME:-$HOME/.local/share}/bz-netcode/bz_wrap.sh" %command% -nointro
```

Same line for native and Flatpak Steam — the installer copies `bz_wrap.sh`
and `upload.conf` into the Flatpak sandbox itself. **Snap Steam is the
exception**: its sandbox cannot read the host's dot-dirs, so with the line
above the wrapper path resolves to nothing and the game never launches. The
installer detects a Snap install and prints this line instead:

```text
WINEDLLOVERRIDES=dsound=n,b "$SNAP_USER_COMMON/.local/share/bz-netcode/bz_wrap.sh" %command% -nointro
```

**Each Steam client keeps its own launch-options field**: if you run more
than one install (native, Flatpak, Snap), set it in each client. If you skip
the uploader, the launch options are still required, just shorter:

```text
WINEDLLOVERRIDES=dsound=n,b %command% -nointro
```

Prefer to do it all by hand? [docs/MANUAL_INSTALL.md](docs/MANUAL_INSTALL.md)

---

> **Privacy — read this once.** Battlezone's own logs record the public IP of
> every player in the lobby; that is how a peer-to-peer game works, and it is
> why the upload wrapper only ever posts to a **private** channel, is strictly
> opt-in (no wrapper in your launch options, nothing ever uploads), and skips
> sessions where no multiplayer game was launched. Don't paste bundles or
> `BZLogger.txt` anywhere public.

---

## Check it worked

Start the game once, quit, and open the proxy log next to the game exe
(`dsound_proxy.log` or `winmm_proxy.log`). The first lines carry the build id:

```
proxy build: V4.94-experimental 0f2a1b3c4d5e 2026-08-13T01:19:46Z
```

No proxy log at all means the DLL never loaded — on Linux that is almost
always the launch-options line missing from the Steam client you actually
launched from.

After a multiplayer match, run the verifier:

```bash
Linux/verify_net_patch.sh          # from the game folder
```
```powershell
.\Microslop\verify_windows.ps1
```

Expect `VERIFY RESULT: PASS`. In the log itself, the line to care about is the
governor verdict: `poke held` is good; `POKE DID NOT HOLD` is exactly the
failure V4.9 exists to catch — report it with the log.

`FLOOR RESCUE` (new in V4.94) is also worth reporting. It means the send rate
collapsed all the way to the bottom mid-match — something was saturating the
link — and the patch lifted it back. It is not a fault in the patch, but the
log around it says what caused the collapse.

`reorder: DISABLED` is expected — that buffer ships off by default, and V4.9
established it has to stay off (see below). `net_patch: … VETOED` means the
game updated and the patch safely fell back to stock behaviour; open an issue
with the log.

---

## What changed in V4.94

**This one changes how the patch behaves by default.** V4.93 shipped the send
damper switched off; V4.94 turns it on, and changes three governor settings.
Everyone in a test lobby needs this build, including whoever hosts.

It all comes out of one logged event. On the evening of 2026-08-12, two minutes
of a match went bad in a way the logs explain completely:

- **Four repair-kit pickups got stuck moving** and would not stop. Each one sent
  about 17 position updates a second on the channel that guarantees delivery,
  and the game sends each of those 3–4 times over. Between them they put
  **30,691 packets / 2.64 MB** on the wire in 140 seconds — at the peak, **11×
  more than the whole bandwidth budget for everything else in the match**.
  Player positions and shots queued up behind repair kits.
- **The bandwidth governor then collapsed.** It cut the send rate 54 times in a
  row without once letting it back up: 25,900 → 4,150 B/s over 107 seconds, and
  the match spent its worst two minutes there. It only recovered because a bug
  in this patch accidentally rescued it.

Four changes, in the order they matter:

- **The send damper is now on by default** (`BZ_SEND_DAMPEN=0` turns it off).
  It drops the redundant copies of a reliable message — the ones the engine
  sends before any reply could physically arrive. Replaying that evening's
  actual traffic through it removes **64–69% of the flood**. It does not fix
  stuck repair kits; it stops them costing four times what they should.
- **The governor now recovers twice as fast as it cuts**, which is what the
  game's own stock settings do. The shipped tuning had it cutting *four* times
  faster than it recovered — measured, that made a 2-minute collapse take 9
  minutes to undo, so in a real fight it never recovered at all. Host and
  client were also running different values; both are now the same.
- **The send rate has a floor again** (16,000 B/s). It collapsed to 4,150 —
  the game's stock floor — because the value that was supposed to stop that
  was never being written. **This one is an experiment**: if the next collapse
  bottoms out at 16,000 instead of ~4,000, it works. Please report the log
  either way.
- **A real bug is fixed.** The patch watches for the value `4000` to spot a
  match starting, then boosts the rate. A collapsing governor walks down onto
  4000 too — so mid-fight, the patch jumped the rate 10× believing a new match
  had begun. It also meant the tools counted 32 "matches" in an evening with
  three in it, which quietly poisoned every measurement taken from them.
  Collapses are now told apart from match starts and logged as
  `FLOOR RESCUE`, and the rate is still raised.

**If you see `FLOOR RESCUE` in your proxy log, that is worth reporting** — it
means something saturated the link hard enough to bottom out the governor, and
the log will say what.

The root cause — pickups that never stop moving — is in the game or the map
mod, not in anything this patch can reach. It has been written up for the map's
author. Full derivation and the numbers behind every claim above:
[contracts/lag-collapse-20260812.md](contracts/lag-collapse-20260812.md).

## What changed in V4.93

If you were on V4.92: nothing behaves differently by default — update anyway,
because a test lobby only produces usable data when everyone runs the same
build. What the update carries:

- **The retransmit storm has its cause.** A mod object replicates on BZRNet's
  reliable channel, whose retry timer is a fixed ~10 ms with no backoff
  against a 56–91 ms RTT — so every reliable message ships 6–9 times, and one
  misbehaving object turns that into an unplayable match. The fix is written
  and handed to the mod's author, but until it ships, nothing has changed for
  anyone actually playing. Full derivation:
  [resources/CAMERAPOD_STORM.md](resources/CAMERAPOD_STORM.md).
- **The send damper** (`BZ_SEND_DAMPEN`, **off by default**) — the mitigation
  this project controls: it drops the redundant in-window copies of a reliable
  message on the send path, in both proxies. Only a 2nd-or-later copy of a
  `(peer, sequence)` already sent is ever suppressed; if in doubt, it sends.
  It stays off until it passes a live-match validation — the crew evening that
  flips it on is the next milestone. Details in each proxy README.
- **Two new log lines** you will see either way: a `send_dampen:` config line
  at startup and a `session end: dampen:` counter line at teardown. With the
  damper off they just record that it was off.
- **Hardening under the hood** — six defects an external review found in the
  V4.92 build are fixed with reproducing tests, the damper's session-reset
  logic survived two adversarial audits (restart-vs-retransmit, and a purge
  that used to fire on the wrong socket close), and the repo has CI for the
  first time: every push builds both proxies at the shipped 32-bit ABI and
  runs the full test suite.

The blow-by-blow, including what each audit caught, is in
[CHANGELOG.md](CHANGELOG.md).

## What V4.9 adds over V4.8

- **The packet header is settled by ground truth** — sequence is u32 big-endian
  at offset 10; the V4.8 duplication/loss figures are withdrawn (they were read
  from the ack field). Derivation: [resources/BZ_P2P_HEADER.md](resources/BZ_P2P_HEADER.md).
- **The governor reports whether its own poke landed** — one verdict per match
  in the proxy log, instead of hand-correlating two machines' logs.
- **Logging you can trust** — torn-line fix, crash-capturable sessions,
  per-peer capture filtering (`BZ_BUFFER_LOG_PEER`), analyzers that survive
  crash-cut logs.
- **Automatic bundle upload to Discord** — see the install steps above;
  details in [upload/README.md](upload/README.md).
- **Uninstallers for both platforms**, and a build id stamped into every DLL.

## What it does

- **Lifts the send governor.** `MaxBandwidth` 16,000 → 320,000, and each match
  opens at 40,000 B/s instead of the stock 4,000 trickle — and V4.9 verifies
  the write actually held, every match.
- **Bigger socket buffers.** 4 MB receive / 512 KB send, re-forced so the game
  can't shrink them back.
- **Relaxes auto-kick** (host only). A connection has to stay bad for 60 s
  instead of 15 s, so a transient spike no longer ejects anyone.
- **DSCP priority marking** so a QoS-capable router serves game traffic ahead of
  bulk downloads.

All memory writes are data-only and DRM-safe, and every address is
sanity-checked before it's written.

---

## Tuning

The defaults are meant to be what you want. The few worth knowing:

| Variable | Default | Effect |
|---|---|---|
| `BZ_NET_TUNE` | `1` | `0` restores the game's stock governor behaviour |
| `BZ_AUTOKICK_RELAX` | `1` | `0` restores stock auto-kicking |
| `BZ_GOV_START` | `40000` | opening send rate; `0` restores the stock 4000 |
| `BZ_REORDER` | `0` | `1` enables inbound reordering (leave off — see below) |
| `BZ_BUFFER_LOG_PEER` | unset | capture only these peer IPs (comma-separated) |

Full tables: [Linux proxy](Linux/proton_dsound_proxy/README.md) ·
[Windows proxy](Microslop/winmm_proxy/README.md)

---

## Uninstall / revert to stable

```bash
# Linux
./install/uninstall_linux.sh          # add --purge-logs to delete captures too
```
```powershell
# Windows
powershell -ExecutionPolicy Bypass -File install\uninstall_windows.ps1
```

The uninstallers only remove a DLL carrying this patch's own marker — another
mod's `dsound.dll`/`winmm.dll` (DSOAL, say) is left alone. Session logs are
kept by default: they are research data.

To drop back to stable instead, run the
[master installer](https://github.com/PiercingXX/battlezone-netcode-patch/tree/master#install) —
it overwrites the experimental DLL with V4.8. If you added the upload wrapper,
also take it out of your launch options (on Linux the plain
`WINEDLLOVERRIDES` line stays).

---

## Good to know

- **Multiplayer button says "Not Ready"?** The game could not get a session
  ticket from Steam. Running two Steam clients on one account (native +
  Flatpak) does this: whichever connected last holds the session. Close one
  client fully, restart the other. `BZLogger.txt` shows
  `RequestEncryptedAppTicket ... k_EResultNoConnection`.
- **A workshop mod shipping its own `net.ini` overrides the patch's.**
  Unsubscribe — disabling it in-game isn't enough. Check `BZLogger.txt` for
  `MOD FOUND net.ini at ...`. The `[Net]` memory poke is unaffected either way.
- **A bundle never arrived in the channel?** It parked — `bz_wrap.sh --status`
  shows what is waiting, `bz_wrap.sh --retry` sends it now, and the outbox
  flushes automatically before every wrapped launch.
- **The addresses are pinned to one game build.** If Rebellion patches the game
  you get stock behaviour, not a crash.
- **It fixes tuning, not loss.** A saturated uplink on the *sending* peer's end
  is theirs to fix.
- **The inbound reorder buffer is off by default, and stays off for a
  structural reason**: the protocol's sequence number counts messages, not
  datagrams, so there is no per-datagram key to reorder by.
  See [resources/BZ_P2P_HEADER.md](resources/BZ_P2P_HEADER.md).

---

Running a test session? [docs/TESTING.md](docs/TESTING.md) —
what to collect and how to read it.

Why things are the way they are: [docs/RESEARCH.md](docs/RESEARCH.md) ·
[resources/](resources/) · [test-logs/](test-logs/)
