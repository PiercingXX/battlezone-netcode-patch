# Battlezone 98 Redux Netcode Patch

Battlezone caps every player's send rate at **16,000 B/s** and opens each match
at a 4,000 B/s trickle. Lifting both took measured matches to 82,000+ B/s and
cut visible position corrections by 55-70% on identical maps.

The patch is a DLL proxy that writes the game's own network tuning into memory,
enlarges the socket buffers, and marks your traffic for router priority. No game
code is modified.

**Everyone in the lobby should install it** — the tuning is per-machine, so your
install fixes your rate and your buffers, nobody else's.

Current version: **V4.8** · [CHANGELOG](CHANGELOG.md)

---

## Install

### Windows

Press Start, type `powershell`, Enter — the blue window, not Command Prompt.
Paste this in:

```powershell
irm https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_windows.ps1 | iex
```

That's it. No launch options — just start the game.

<details>
<summary>If it fails with <code>Cannot bind argument to parameter 'Command' because it is an empty string</code></summary>

That error means `irm` downloaded nothing and handed the empty result to
`iex` — the script never ran. The URL above is fine, so something on your
machine emptied the response: an ad-blocker or corporate DNS blackholing
`raw.githubusercontent.com`, or antivirus HTTPS inspection stripping the
body.

Download to a file first, which shows you the real failure instead of an
empty string:

```powershell
$dst = "$env:TEMP\bznet_install.ps1"
Invoke-WebRequest -UseBasicParsing -Uri 'https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_windows.ps1' -OutFile $dst
(Get-Item $dst).Length   # expect ~9000, not 0
powershell -NoProfile -ExecutionPolicy Bypass -File $dst
```

If the length is 0, the download is being blocked — try another network or
temporarily disable the blocker. If it looks right, the last line installs.
</details>

<details>
<summary>If Defender quarantines <code>winmm.dll</code></summary>

Some users see it flagged as `Program:Win32/Contebrew.A!ml`, a heuristic
detection common for unsigned DLL proxies. Restore it from Protection History
and add an exception for that one file in the game folder. Don't disable AV
globally.
</details>

### Linux / Proton

```bash
curl -fsSL https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_linux.sh | bash
```

Then set Steam launch options for Battlezone 98 Redux:

```text
WINEDLLOVERRIDES=dsound=n,b %command% -nointro
```

Without that second step the DLL is never loaded.

Prefer to do it by hand? [docs/MANUAL_INSTALL.md](docs/MANUAL_INSTALL.md)

---

## Check it worked

Play one multiplayer match, quit, then run:

```bash
Linux/verify_net_patch.sh          # from the game folder
```
```powershell
.\Microslop\verify_windows.ps1
```

Expect `VERIFY RESULT: PASS`.

To eyeball it instead, open the proxy log next to the game exe
(`dsound_proxy.log` or `winmm_proxy.log`) and look for `net_patch: version
confirmed`.

`reorder: DISABLED` is expected — that buffer ships off by default.
`net_patch: … VETOED` means the game updated and the patch safely fell back to
stock behaviour; open an issue with the log.

---

## What it does

- **Lifts the send governor.** `MaxBandwidth` 16,000 → 320,000, and each match
  opens at 40,000 B/s instead of the stock 4,000 trickle.
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
| `BZ_REORDER` | `0` | `1` enables inbound reordering |

Full tables: [Linux proxy](Linux/proton_dsound_proxy/README.md) ·
[Windows proxy](Microslop/winmm_proxy/README.md)

---

## Good to know

- **A workshop mod shipping its own `net.ini` overrides the patch's.**
  Unsubscribe — disabling it in-game isn't enough.
- **The addresses are pinned to one game build.** If Rebellion patches the game
  you get stock behaviour, not a crash.
- **It fixes tuning, not loss.** A saturated uplink on the *sending* peer's end
  is theirs to fix.
- **The inbound reorder buffer is off by default** as of 2026-07-26. Measurement
  showed it never ran, and that there was almost nothing to reorder.

---

Running a test session? [docs/TESTING.md](docs/TESTING.md) —
what to collect and how to read it.

Why things are the way they are: [docs/RESEARCH.md](docs/RESEARCH.md) ·
[resources/](resources/) · [test-logs/](test-logs/)
