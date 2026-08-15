# Automatic bundle upload

Ends the "zip it and DM it" step, and with it the two ways sessions currently
die: **BZLogger overwritten by a relaunch**, and **crash bundles never sent
because the tester restarted first**.

This is opt-in tooling for the test crew. It is not part of the player install.

## Why a launch-option wrapper and not the DLL

The opt-in *is* the mechanism. Steam launch options can wrap the game command,
so the upload lives in a script that runs before and after the game process —
outside it. **No wrapper in your launch options, nothing ever uploads.**

That beats uploading from the proxy on every axis:

- it runs after the process has exited, with native tooling (`curl` /
  `Invoke-RestMethod`), so there is no HTTPS from a 32-bit mingw DLL and no
  work under the loader lock at `DLL_PROCESS_DETACH`;
- it fires **even when the game crashes**, which is precisely when the bundle
  matters and precisely when testers forget;
- the explicit launch option is the consent step, so nothing needs to prompt at
  exit time — a Steam-launched wrapper has no visible console to prompt in.

## Setup

```bash
# Linux
mkdir -p ~/.local/share/bz-netcode
cp upload/bz_wrap.sh ~/.local/share/bz-netcode/
~/.local/share/bz-netcode/bz_wrap.sh --setup
```

```powershell
# Windows
mkdir "$env:LOCALAPPDATA\bz-netcode"
copy upload\bz_wrap.ps1,upload\bz_wrap.bat "$env:LOCALAPPDATA\bz-netcode\"
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\bz-netcode\bz_wrap.ps1" -Setup
```

`--setup` asks for the webhook URL (pinned in the private Discord channel the
bundles land in — you are already there) and your player name, then prints the
exact launch-option line to paste.

```text
Linux (native / Flatpak):
          WINEDLLOVERRIDES=dsound=n,b "${XDG_DATA_HOME:-$HOME/.local/share}/bz-netcode/bz_wrap.sh" %command% -nointro
Linux (Snap):
          WINEDLLOVERRIDES=dsound=n,b "$SNAP_USER_COMMON/.local/share/bz-netcode/bz_wrap.sh" %command% -nointro
Windows:  cmd /c ""%LOCALAPPDATA%\bz-netcode\bz_wrap.bat" %command%"
```

The Windows line is also the same for everyone: Steam hands launch options to
the OS without expanding variables, but `cmd` is the program being launched
and cmd expands `%LOCALAPPDATA%` itself. A console window stays open while
the game runs — that is the wrapper waiting to bundle on exit; closing it
kills the upload, not the game.

The first Linux line covers native **and** Flatpak Steam: Steam evaluates
launch options through a shell, and Flatpak remaps `XDG_DATA_HOME` into its
sandbox, so the one line finds the wrapper at `~/.local/share/bz-netcode/`
natively and at `~/.var/app/com.valvesoftware.Steam/data/bz-netcode/` under
Flatpak.

**Snap Steam needs its own line.** Snap remaps `HOME` itself, and its
sandbox cannot read the host's dot-dirs at all — with the XDG line the
wrapper path resolves to nothing inside the sandbox, Steam's exec of the
launch options fails, and **the game never launches**. `$SNAP_USER_COMMON`
is set by snapd inside every snap and points at `~/snap/steam/common`, which
the sandbox can always read.

**What the upload itself needs:** `curl`, or failing that `python3` (the
wrapper carries a stdlib uploader). Stock Ubuntu desktop has python3 but not
curl, so a plain Ubuntu box works out of the box. The Steam **snap's**
runtime has *neither*, so under Snap the bundle parks in the outbox instead
of uploading — and the next wrapped launch runs inside the same toolless
sandbox, so nothing in there can ever send it. The installer therefore also
enables a systemd **user timer** (`bz-netcode-retry.timer`) on Snap machines
that drains the outbox with host tools every 10 minutes. Manual drain, same
effect, any normal terminal:

```bash
~/snap/steam/common/.local/share/bz-netcode/bz_wrap.sh --retry
```

`install_linux.sh` mirrors `bz_wrap.sh` and `upload.conf` into the Snap
(`~/snap/steam/common/.local/share/bz-netcode/`) and Flatpak
(`~/.var/app/com.valvesoftware.Steam/data/bz-netcode/`) dirs automatically
whenever those Steams exist. Installing by hand, copy both files there
yourself — the wrapper keeps its conf, outbox and parked bundles next to
itself whenever `upload.conf` sits beside it, so each sandbox copy is
self-contained, and running that copy's `--retry` from a normal host shell
(where curl works) drains the same outbox.

**Order matters.** Everything before `%command%` is an environment variable;
everything after it is an argument handed to the game. The wrapper goes
immediately before `%command%`.

## What it does

**Before launch** — snapshots `BZLogger.txt` aside. This is the whole reason
game 2 of 2026-07-26 has no PiercingXX log: game 1's relaunch overwrote it.
Then it flushes anything parked in the outbox, while there is still a network
and before the game saturates it.

**Launches the game**, passing `%command%` through untouched. A wrapper failure
never stops you playing, and the game's own exit code is passed back.

**After exit** — bundles the proxy log, the BZLogger snapshot, the final
BZLogger, `multi.ini`, any buffer capture, and a meta file recording UTC time,
your local offset, hostname, player name and the game's exit code. Proton logs
are skipped unless you asked for them. Then `xz` (Linux) or zip (Windows), and
`POST` via the webhook with a one-line summary:

```
**PiercingXX** — map `bltop04.bzn`, 15 min, exit 0
**KFK** — map `uliovol1.bzn`, 12 min, exit 0 **CRASH** (no `Exiting Game With Return Code`)
```

The crash flag uses the same rule as `tools/analyze_drops.py`: a BZLogger that
ends without `Exiting Game With Return Code` ended abruptly.

A bundle over the ~10 MB webhook cap is split; parts are labelled and
reassembled with `cat *.part* > bundle.tar.xz` (or `copy /b` on Windows).

**Menu-only sessions are sent too.** A "map `unknown`, 0 min" bundle teaches
nothing about the netcode, but it proves the uploader on that machine works —
without it, skipped-on-purpose and silently-broken look identical from the
channel, and testers have reported the uploader as broken when it had just
decided their session was uninteresting. The bundles are a few kB. Set
`BZ_UPLOAD_MENU=0` in `upload.conf` to skip them.

**Upload failed or offline** → the bundle is parked in an outbox and retried on
the next wrapped launch. Nothing is lost.

## The webhook URL is never committed

Discord participates in GitHub secret scanning: a webhook URL pushed to a
public repo is auto-revoked. `--setup` writes it to
`~/.config/bz-netcode/upload.conf` (mode 600) or
`%APPDATA%\bz-netcode\upload.conf`. `BZ_UPLOAD_WEBHOOK` overrides it for
unattended runs.

The URL stays a revocable weak secret whose blast radius is spam in one private
channel.

## Privacy

**Bundles contain every peer's public IP.** That is why the destination is a
private channel only. Choosing to add the wrapper to your launch options is the
consent step — there is no silent default and no way for this to run without
you having pasted that line yourself.

## Other commands

```bash
bz_wrap.sh --status    # configuration and how many bundles are waiting
bz_wrap.sh --retry     # send the outbox now and exit
```

## Relationship to tester_diag

`tester_diag` stays for the deep-capture path — mtr timelines, socket
timelines, coredump metadata, packet traces. This wrapper is the everyday one:
it collects the four files that answer most questions, and it collects them
without anyone having to remember.
