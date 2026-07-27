# Future plans

A full review of the repo, logs, bundles and source on 2026-07-26 (post-V4.8).
Ordered by priority; every item cites the evidence that motivated it. The theme
of the top tier: **the data already collected contains answers the tooling is
not extracting**, and two V4.8 conclusions rest on evidence weaker than the
docs claim.

---

## P0 — Findings that challenge the current record

### 1. The sequence field is not actually settled — and the reorder retirement rests on it

V4.8 corrected the sequence field to u16 big-endian at payload offset 16 and
retired the reorder buffer on the strength of "0.0–0.2% out-of-order". But
sweeping the committed capture (`test_bundles/buffer_linux_unknown-host_20260726T183650Z/bz_buffer_log.bin`)
with `decode_buffer_log.py` across candidate offsets:

| offset/width/endian | out-of-order | dup | "never arrived" |
|---|---|---|---|
| 6 / 4 / be (the decoder's own `--seq-scan` best guess) | 0.0% | 61.7% | 465,516 |
| **16 / 2 / be (the documented answer)** | 0.0% | **88.5%** | 5,013 |
| 13 / 2 / le (the old, wrong answer) | 0.4% | 99.5% | — |

No offset yields a plausible monotone per-peer counter; an 88.5% duplicate rate
for a real packet counter is not credible; and the decoder's own scoring picks
a *different* offset than the documented one. The "nothing to reorder"
conclusion survives every candidate (all read ~0% out-of-order), but the loss
and duplication percentages quoted in the CHANGELOG and README are
offset-dependent and should not be treated as measured fact yet.

**The fix costs zero new sessions.** BZLogger's second retransmit format —
`BZRNet P2P TRY Sent Packet (i,n) to IP:PORT: <hex>` — carries the full payload
hex with a known ordinal. Cross-matching those hex dumps against the buffer
capture's payload prefixes derives the counter offset *empirically* instead of
scoring monotonicity. Do this first; it either confirms or re-opens the V4.8
reorder rationale, and fixes the loss/dup numbers everywhere they're quoted.
Then make `decode_buffer_log.py`'s defaults match whatever the answer is — its
defaults are still `--seq-width 4 --seq-endian le` with "seq_offset=13" prose
(`decode_buffer_log.py:169,245`), i.e. the *V3* answer.

### 2. `BZ_GOV_START=40000` failed to hold in the only match that measured it

The 18:42 game-1 session: proxy logged `cold-start caught, send-rate 4000 ->
40000` at 18:43:22.082, and BZLogger reported the governor at **16000** from
18:43:25.9 onward, ramping +100 B/s to 31400 over 20 minutes
(`test-logs/2026-07-26_v48_game1_piercingxx_dsound_proxy.log:286` vs
`…_BZLogger.txt`). 16000 is exactly stock `MaxBandwidth` — the same
wrote-but-didn't-stick signature that got `MinBandwidth`'s address flagged
unconfirmed. Meanwhile game 2 the same evening shows the poke landing and
sticking (4000 → 40000 instantly, ramping to 82100 in Monkey's log). So it's
intermittent or role-dependent, not dead.

Build the instrumentation before the next A/B: have the governor thread
**re-read `0x008e8d14` on a timer and log the observed value**, so a poke that
gets clamped or rewritten is visible in the proxy log itself instead of only by
manual BZLogger correlation. Then run the A/B the CHANGELOG already calls for
(16000 / 40000 / 80000, same map, same peers, **same host role** — see item 4).

### 3. `analyze_drops.py` cannot see host logs, and throws away the denominator it needs

Four independent bugs, all silent, all fixable with data already in hand:

- **`BANDWIDTH_RE` requires a trailing ` ms`** (`analyze_drops.py:98`). Host
  logs use `Net: Bandwidth usage now set to 82100, Interval 48, Actual Used
  61270` — zero matches. Every host-side log in the dataset (KFK's crash log:
  184 lines; the bundle session: 171) silently reports "no governor data". The
  player at ID 1 is the host; the split is clean across all five logs.
- **`Actual Used` is the real-bytes denominator** that the CHANGELOG's "Known
  limit in the loss metric" section says doesn't exist. It exists in every host
  log and has all along. Use it host-side, and the proxy's `session end:
  bytes=` client-side, and retire the median-governor-budget denominator.
- **`TRY_SENT_RE` captures the byte count and discards it**, counting events
  instead. Events and bytes rank sessions differently (bundle session: 11,986
  events but 5.07 MB — avg 423 B/event vs ~80 elsewhere).
- **The `TRY Sent Packet (i,n)` format isn't counted at all.** Bundle session:
  11,986 matches of the counted form vs 65,806 of the uncounted one.

While in there: add a crash detector (a BZLogger that ends without `Exiting
Game With Return Code` — both crashes in the repo are identifiable by that one
rule), an `observer.mesh`/ERROR-flood metric, a warning when `ahead` exceeds
~1% of discards, and a torn-line warning for proxy logs (see item 7).

### 4. The teardown crash — reproduce it with a dump, and control for host role

Both hard-terminated logs die the same way: a normal match end, then an
`observer.mesh` load-error flood, then the log just stops mid-teardown with no
shutdown lines. KFK's 2026-07-26 game-2 host log: abort at 17:53:38, 14,491
`ERROR: could not load observer.mesh` lines, file ends at `0 players
remaining`. The 2026-07-05 hard crash: same shape, 201,940 mesh errors.
Neither has a dump, a proxy log, or a `session end` line — the `procdump` path
promised in `logging_readme.md` has produced nothing in any committed bundle.

Also striking: every anomaly in the 2026-07-26 dataset splits on host vs
client — the log format (item 3), both crashes, and the retransmit storm
(item 5) are all host-side. No A/B has ever held role constant. Add "who
hosts" to the standard test protocol in `docs/TESTING.md` as a controlled
variable, and verify the procdump path actually fires before the next session.

(Bookkeeping: `test-logs/2026-07-26_crash_dsound_proxy.log` is not an
independent artifact — it is byte-identical to the first 157 lines of
`2026-07-26_v48_game1_piercingxx_dsound_proxy.log`. Mark or remove it. The
crash window 15:06–16:07 has no BZLogger at all.)

### 5. The unanalysed retransmit storm (and the `syncJoin` coincidence)

The bundle session (PiercingXX hosting, `bltop04.bzn`, 15.2 min) hit **786
retransmits/min** — 20× any other session — with ~31% of all outbound bytes
being `TRY Sent` (5,067,904 of 16,224,841). The time distribution is a wall:
4 events in one minute, then 23,546 the next, sustained 6–9k/min for eight
minutes, then back to ~4. It is the most extreme event in the whole dataset
and nothing has looked at it. That run is also the only one with `syncJoin =
1` in `multi.ini` (every other run: 0). Single sample, obvious hypothesis,
cheap A/B next session.

---

## P1 — Real bugs in the shipped code

### 6. The IOCP path missed the V4.8 sequence fixes entirely

`Microslop/winmm_proxy/src/netcode_hooks.cpp:586` and `:725` still compare
`seq == pb->last_seq + 1` in 32-bit space — the exact wrap bug V4.8 fixed in
`reorder_core.h` (`seq_next`/`seq_cmp`, 16-bit wrap every 6–11 min). The same
path mutates the reorder peer table (`IocpRelease`, `IocpPickReady`, the GQCS
body at `:570-596,722-765`) holding only `g_iocp_cs`, when `reorder_core.h:12`
requires the reorder critical section — a data race with the wake thread. It
also has zero test coverage. Since `BZ_IOCP_REORDER` has never run on real
Windows and the reorder buffer is retired, the cheapest correct move may be to
**delete the IOCP reorder path** (keep the read-only `BZ_IOCP_SCAN`) rather
than fix code with no validated purpose. Decide deliberately; don't leave it
half-fixed.

### 7. Proxy log writes are not atomic — and it's already corrupted real data

Two committed logs contain torn/interleaved lines:
`2026-07-26_crash_dsound_proxy.log:145` (leading `[2` missing) and the
183650Z bundle's `dsound_proxy.log:128` (two lines concatenated). Multiple
threads write the log concurrently; the Linux side opens/appends/closes per
line (`dsound_proxy.cpp:824-861`). Serialize log writes under one lock (or a
single writer). Related lifetime races: Windows `fclose(g_log)` at
`DLL_PROCESS_DETACH` while worker threads that use `ProxyLog` were signalled
but never joined; Linux `HeapFree(g_buffer_ring)` at detach with only an
unlocked null-check in `buffer_log_event`; `g_wake_sender` closed while the
wake thread may be in `sendto`.

### 8. Silent datagram truncation in the receive hooks

`deliver_to_caller`/`DeliverToCaller` copy `min(len, caller buffer)` and
report success (`dsound_proxy.cpp:1154-1156`, `netcode_hooks.cpp:1009-1011`) —
no `WSAEMSGSIZE`, no `MSG_PARTIAL`. A short caller buffer turns a whole
datagram into a silently corrupt short one; stock winsock would error. Only
matters with `BZ_REORDER=1`, but it's wrong, and it's the kind of wrong that
would burn a week of debugging some future session.

### 9. The buffer logger's ring size wasn't honoured — and one knob is dead

The successful capture asked for `BZ_BUFFER_LOG_RING=500000`
(`ring_records.txt`) and ran with 65,536 (`bz_buffer_log.meta.txt`), losing
48% of events including the entire match start. Find out whether the proxy
clamps the ring (if so: log the clamp loudly) — or whether the game launched
before the launch options were pasted (if so: the logger must fail loudly when
meta ≠ requested). Also `BZ_BUFFER_LOG_PEER` is written into every tester's
paste-ready launch options by both logger scripts
(`buffer_logger_linux.sh:42`, `buffer_logger_windows.ps1:144`) but **no proxy
source reads it** — implement it or stop emitting it. And the bundle hostname
capture is broken: both bundles are named `unknown-host`.

---

## P2 — The Windows side is unmeasured and behind

### 10. Not one `winmm_proxy.log` exists in the repo

Every proxy-side measurement ever committed — `send_stats`, `governor_patch`,
`net_patch`, reorder — is from one Linux box. Three of four regular testers
are on Windows. Until one Windows bundle with `winmm_proxy.log` lands, the
Windows proxy's behaviour is unverified in the field. Compounding it: the
shipped Windows prebuilt is deliberately the older `2438ff1` build without the
detach counter backstop (CHANGELOG V4.8), so even a clean Windows test would
lose its counters on an unclean exit. **Refresh the Windows prebuilt in a
deliberate release** now that the sidecar-reading installer has had time to
propagate, and get one Windows tester to run `tester_diag.ps1`.

### 11. Windows/Linux proxy parity gaps

In rough order of impact:

- **Windows log is truncated every run** (`fopen(logPath, "w")`,
  `dllmain.cpp`) and has no timestamps; a crash-then-relaunch destroys the
  evidence. Match the Linux append+timestamp behaviour. (The BZLogger
  overwrite-on-launch problem bit again this cycle too — game 2 is missing
  PiercingXX's BZLogger because game 1's launch overwrote it. The upload
  wrapper in item 15 snapshots BZLogger pre-launch, which ends this whole
  class of loss without touching the proxy.)
- **`analyze_drops.py` can't parse the Windows config line** — Windows emits
  `OOO reorder enabled …` where the regex wants `reorder: …`
  (`netcode_hooks.cpp:2156-2168` vs `analyze_drops.py:87-90`). Emit the same
  config line from both proxies.
- Windows patches only the EXE's IAT once (`dllmain.cpp:77-83`); Linux hooks
  `GetProcAddress` and re-patches every module on a retry loop. A dynamically
  resolved winsock import bypasses the Windows proxy silently.
- Missing Windows hooks: `recvfrom`, `getsockopt`, `ioctlsocket`, `WSAIoctl`,
  `socket` — so the Windows buffer log can only ever answer "WSARecvFrom",
  never the receive-API question it was built for.
- `is_target_main_module()` has no Windows equivalent — winmm hooks whatever
  process loads it.
- Buffer-log `sid` semantics differ (Linux: tracked index; Windows: raw
  handle), so `decode_buffer_log.py --sid` isn't portable across platforms.

---

## P3 — Infrastructure the last two releases proved we need

### 12. CI

There is no `.github/`. Nothing builds either DLL, runs `make -C tests run`,
or executes `tools/check_prebuilt_pins.sh` on push — the pin checker's own
header laments that "nothing could have caught" the installer-hash incident,
and that is still true because nothing runs it. Minimum viable workflow:
mingw cross-build of both proxies, host test run, pin check. Add `-m32` (or a
mingw run) to the test build — host tests are 64-bit while the shipped DLLs
are 32-bit, so the shipped ABI is never exercised. Fix the Makefiles' `HDRS`
under-declaration while at it (`net_globals.h`/`send_pace.h` edits don't
trigger rebuilds), and add a round-trip test asserting the C++
`BufferLogRecordHeader` layout matches `decode_buffer_log.py`'s
`struct.Struct` — nothing pins those two today.

### 13. Version-stamp the binaries

Neither DLL logs a build id. The Windows prebuilt being deliberately a
different vintage than the source makes this acute: a tester's log currently
cannot tell you which build produced it. One `proxy build: <version> <commit>
<date>` line at attach, injected via `-DBZ_BUILD_ID` in both Makefiles, ends
every "which build was that?" thread. Also: `check_prebuilt_pins.sh:57` greps
only lowercase hex, so an uppercase hardcoded hash slips through.

### 14. Installer hygiene

`install_linux.sh` downloads the source tarball with no integrity check
(`:311-315`) while the Windows path verifies SHA256, and interpolates `--ref`
straight into the URL. It also writes `/etc/sysctl.d/99-battlezone-netcode.conf`
via sudo with no uninstall path — there is no uninstall script for either
platform. And both install scripts plus `deploy_linux.sh` still tell users the
reorder buffer is on by default (`install_linux.sh:383-385`,
`install_windows.ps1:219`, `deploy_linux.sh:68-70`) — stale since the V4.8
retirement; the README says the opposite.

### 15. Automatic bundle upload — Discord webhook, opt-in via Steam launch option

Ends the "zip it and DM it" step, and with it the two ways sessions currently
die: BZLogger overwritten by a relaunch, and crash bundles never sent because
the tester restarted first.

**Architecture: a launch-option wrapper, not the DLL.** The opt-in *is* the
mechanism. Steam launch options can wrap the game command, so the upload lives
in a script that runs before and after the game process — outside it:

```text
Linux:    WINEDLLOVERRIDES=dsound=n,b ~/.local/share/bz-netcode/bz_wrap.sh %command% -nointro
Windows:  cmd /c "%LOCALAPPDATA%\bz-netcode\bz_wrap.bat" %command%
```

No wrapper in the launch options → nothing ever uploads. This beats uploading
from the proxy on every axis: it runs after process exit with native tooling
(`curl`/`Invoke-RestMethod`), so no HTTPS-from-a-32-bit-mingw-DLL, no work
under the loader lock at `DLL_PROCESS_DETACH` — and it fires **even when the
game crashes**, which is precisely when the bundle matters and precisely when
testers currently forget. Windows gains its first launch option, but it's
opt-in tooling for the four regulars, not part of the player install.

**What the wrapper does.**

- *Pre-launch:* snapshot `BZLogger.txt` aside (closes the overwrite-loss
  class for good — game 2's missing log this cycle), write a meta file with
  UTC time (`date -u`), local offset, player name, patch/build version, and
  hostname (also fixes the `unknown-host` bundle naming).
- *Launch the game*, passing `%command%` through untouched.
- *Post-exit:* bundle proxy log + BZLogger snapshot + final BZLogger +
  `multi.ini` + meta; skip Proton logs by default. `xz` the tarball — the
  31 MB BZLogger compresses far under Discord's ~10 MB webhook attachment
  cap; `split -b 9M` the rare oversized one into parts. POST via
  `curl -F` with a summary line: player, duration, map, and a ⚠️ crash flag
  when the log ends without `Exiting Game With Return Code` (the detector
  from item 3). Upload failed / offline → park the bundle in an outbox
  directory and retry on the next wrapped launch, so nothing is lost.

**The webhook URL must never be committed.** Discord participates in GitHub
secret scanning — webhook URLs pushed to a public repo are auto-revoked, the
same trap as the GitHub-token route. Instead: `bz_wrap.sh --setup` prompts
once for the URL (pinned in the private Discord channel the bundles land in —
testers are already there) and the player name, and writes
`~/.config/bz-netcode/upload.conf` / `%APPDATA%\bz-netcode\upload.conf`. The
URL stays a revocable weak secret whose blast radius is spam in one private
channel.

**Privacy:** bundles contain every peer's public IP, so the destination is a
private channel only, and `docs/TESTING.md` must say plainly what an opted-in
launch option sends. The explicit launch-option opt-in is the consent step —
no silent default, no prompt needed at exit time (Steam-launched wrappers have
no visible console to prompt in anyway).

**Pieces:** `upload/bz_wrap.sh`, `upload/bz_wrap.ps1` (+ a one-line `.bat`
shim), `--setup` mode, installer offer + printed launch-option line,
TESTING.md/logging_readme updates, and the Discord channel + webhook itself.
`tester_diag` stays for the deep-capture path; the wrapper is the everyday
one.

---

## P4 — Experiments and forward work (next play sessions)

In recommended order:

1. **Sequence-field cross-match** (item 1) — before any new capture; it
   recalibrates everything else.
2. **`BZ_GOV_START` A/B with landing proof** (item 2) — 16000/40000/80000,
   same map, same peers, role held constant.
3. **`syncJoin` A/B** (item 5) and a **role-controlled session** (item 4) —
   both fold into the same evening of testing.
4. **Buffer capture re-run with the ring honoured** (item 9) — must cover the
   match start; the current capture's 8.1 of 15.2 minutes misses it entirely.
5. **`UpCount`/`DownCount` ramp A/B** — proposed in
   `resources/PATCH_OPTIONS_RESEARCH.md` (§A1) in early July, still never
   run. Now that the governor thread exists, the memory-poke route can test
   the ramp increment directly too. The V4.8 data makes the case: game 1
   crawled +100 B/s per ~2 s for 20 minutes while the link demonstrably
   carried 82k in game 2.
6. **The NPPI keys** (`MaxPingsLost`, `LimitLowNPPI`, `LimitHiNPPI`,
   `DivisorMPPI2NPPI`, `DivisorPing2NPPI`) remain unmapped
   (`net-ini/README.md`). Locate their globals the same way the ten known
   keys were found; even stock-value confirmation would close the file.
7. **FEC between patched proxies** (PATCH_OPTIONS_RESEARCH §E) — still the
   highest-ceiling item, and the measured problem profile (real loss, heavy
   duplication, ~zero reordering) is exactly what parity coding fixes and
   reorder buffering doesn't. Gate on: (a) the sequence field settled, (b) a
   controlled test that unpatched receivers provably discard the parity
   packet type harmlessly. The auto-kick ping exemption already built for the
   pacer shows the shape of the care needed.
8. **Auto-kick relax has never been observed working.** No lag/kick event
   appears in any committed log since V4.6 shipped it. One deliberate
   bad-link test (throttle a client) would confirm the relaxed thresholds
   actually hold on a patched host.

Also worth capturing while testers are engaged: the three players' wall
clocks differ by up to an hour and bundles record no UTC offset — add a
clock-offset line (`date -u` at bundle start) to both tester_diag scripts,
and note the upload wrapper (item 15) records it in its meta file too, so
cross-host correlation stops being archaeology.

## P5 — Housekeeping

- `resources/valid_capture_reorder_signal_only.md` and
  `…_clusters_250ms.md` are byte-identical; the clusters `.csv` has no
  rendered counterpart. Both are marked superseded — regenerate against the
  settled sequence field (item 1) or delete.
- `new logs/` (18 MB, space in the name) → fold into `test-logs/` with the
  standard naming convention; `test_bundles/` (38 MB) needs a retention
  policy — the two committed bundles duplicate their own extracted trees.
- Two 18:34/18:35 proxy attaches logged `packets=0 bytes=0` sessions (failed
  launches). Harmless, but the analyzer should label empty sessions rather
  than list them.
- `Source IP doesn't match any known player` drops appear in every log
  (1–21 per session) and have never been investigated. Probably NAT
  rebinding; worth one look, low priority.
