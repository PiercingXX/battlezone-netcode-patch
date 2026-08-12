# Test logs

Session logs from the test series. **The logs themselves are not in git** —
`.gitignore` excludes `test-logs/*.log` and `**/BZLogger*.txt`, because one
session runs to tens of megabytes. Only analysis write-ups like this one and
`2026-07-03_dup_test_summary.md` are tracked. Whatever is in this directory on
your disk is your local copy; there is no other copy.

## Naming

```
YYYY-MM-DD_<version>_<HHMM-map>_<player>_<host|client>_<BZLogger.txt|dsound_proxy.log|winmm_proxy.log>
```

The role is part of the name from V4.9 on. Every anomaly in the 2026-07-26
dataset splits on host vs client — the log format, both crashes, the retransmit
storm — and a filename that does not say which is a filename you have to open a
log to interpret. See "Host role is a variable" in `docs/TESTING.md`.

## Reading them

```bash
python3 tools/analyze_drops.py <session>_dsound_proxy.log <session>_BZLogger.txt
```

Pass the proxy log and the BZLogger from the same session on one command line.
The proxy's `session end: send_stats: bytes=` counter is the denominator the
BZLogger's retransmit counts need, and the proxy's `BZ_GOV_START=` line is what
lets the tool tell you whether the governor poke actually landed.

## Known problems with the current set

- **`2026-07-26_crash_dsound_proxy.log` is not an independent artifact.** It is
  byte-for-byte the first 157 lines of
  `2026-07-26_v48_game1_piercingxx_dsound_proxy.log` (verified: the md5 of that
  prefix matches the whole file). Counting it as a separate session makes one
  session look like two. The crash it is named for has no BZLogger at all — the
  15:06–16:07 window is unlogged. Do not analyse it separately.

- **No `winmm_proxy.log` anywhere.** Every proxy-side measurement ever recorded
  comes from one Linux box, while three of four regular testers are on Windows.
  Until one Windows bundle lands, the Windows proxy's field behaviour is
  unverified.

- **Game 2 of 2026-07-26 has no PiercingXX BZLogger.** Game 1's relaunch
  overwrote it. `upload/bz_wrap.sh` snapshots BZLogger *before* launch, which
  ends this class of loss.

- **Neither hard stop has a dump.** Both end mid-teardown after an
  `observer.mesh` error flood, with no shutdown lines. See the crash-capture
  preflight in `docs/TESTING.md`; `tester_diag start` now refuses to be quiet
  about an unready dump path.

- **`Source IP doesn't match any known player` is benign — investigated and
  closed 2026-07-27.** These appear 1–21 times per session and had never been
  looked at. Every single one lands in the *same millisecond* as a
  `BZRNet P2P CON Sent Packet` handshake, and the handshake probes every
  candidate address for a peer at once — the observed destination set includes
  `127.0.0.1`, a Docker bridge at `172.17.0.1`, a LAN address and a CGNAT
  address alongside the real public peers. Replies arrive from addresses that
  are not in the player table yet, and are dropped. It is the connect probe
  working as designed, not NAT rebinding and not loss. No action needed.

- **Wall clocks differ by up to an hour between testers.** Two logs in this set
  are exactly 3 h apart. Both `tester_diag` scripts now write
  `clock_offset.txt`, and the upload wrapper records the same in its meta file.
