"""Verify analyze_drops.py pairs the proxy session to the BZLogger by pid and
time window, and says \"no matching proxy session\" instead of scoring the wrong
one.

The old code used the proxy log's *largest* session as the client-side real-bytes
denominator for any BZLogger passed on the same command line.  A proxy log spans
the whole process and usually holds several matches, so that silently scored the
wrong session whenever the BZLogger's match was not the largest.

The committed fixtures hold the two cases:

  * proxy_pair_proxy.log      two sessions (14:30, 14:50) that do NOT overlap the
                              BZLogger's 15:00 match window.  The old code scored
                              the largest (5.00 MB); the new code must say
                              \"no matching proxy session\".
  * proxy_pair_match_proxy.log  a session (14:30..15:00:30) that DOES overlap the
                              BZLogger window, plus a larger one (14:30, 5.00 MB)
                              that is not the match.  The new code must use the
                              overlapping session (1.00 MB), not the largest.
  * proxy_pair_single_proxy.log  a single session (14:20..15:00:30) whose only
                              `session end:` line lands at 15:00:30 — the
                              ordinary one-process-one-match capture shape.  The
                              BZLogger's match (14:30..14:55) sits inside that
                              span but ends before the session-end line, so a
                              session seeded from its own end collapses to a
                              zero-width window and is reported unmatched.  The
                              new code seeds the session start from the log's
                              first timestamp (14:20) and must pair it.

Run:  python3 tests/analyze_drops_pairing_test.py
"""

import subprocess
import sys

TOOL = 'tools/analyze_drops.py'
BZLOGGER = 'tests/fixtures/proxy_pair_bzlogger.txt'
NO_MATCH_PROXY = 'tests/fixtures/proxy_pair_proxy.log'
MATCH_PROXY = 'tests/fixtures/proxy_pair_match_proxy.log'
SINGLE_PROXY = 'tests/fixtures/proxy_pair_single_proxy.log'
SINGLE_BZLOGGER = 'tests/fixtures/proxy_pair_single_bzlogger.txt'

NO_MATCH_PHRASE = "no matching proxy session"
# The old wrong-session output picked the largest session and named it.
OLD_LARGEST = "largest of 2 sessions"
OLD_SHARE = "retransmit share of outbound bytes"


def run(*args):
    proc = subprocess.run([sys.executable, TOOL, *args],
                          capture_output=True, text=True, check=False)
    return proc.stdout, proc.returncode


def main():
    failures = 0

    def check(ok, what):
        nonlocal failures
        if not ok:
            failures += 1
            print(f"  FAIL: {what}")

    # Case 1: no proxy session overlaps the BZLogger window -> must say so.
    out, rc = run(NO_MATCH_PROXY, BZLOGGER)
    check(rc == 0, f"no-match run should exit 0, got {rc}")
    check(NO_MATCH_PHRASE in out,
          f"expected {NO_MATCH_PHRASE!r} in output when no session overlaps")
    check(OLD_SHARE not in out,
          "must NOT score a retransmit share against the wrong (largest) session")
    check(OLD_LARGEST not in out,
          f"old wrong-session wording {OLD_LARGEST!r} must not appear")

    # Case 2: an overlapping session exists -> use it, not the largest.
    out, rc = run(MATCH_PROXY, BZLOGGER)
    check(rc == 0, f"match run should exit 0, got {rc}")
    check(NO_MATCH_PHRASE not in out,
          f"must not claim {NO_MATCH_PHRASE!r} when an overlapping session exists")
    check("pid 1234 session 2026-08-09 14:30:00.000..2026-08-09 15:00:30.000, 1.00 MB"
          in out,
          "must use the overlapping session (1.00 MB, pid 1234) as the denominator")
    check("5.00 MB)" not in out,
          "must NOT use the largest session (5.00 MB) as the denominator")

    # Case 3: a single-session capture (one process, one match) — the ordinary
    # shape.  The match ends before the session-end line, so a session seeded
    # from its own end collapses to a zero-width window and is wrongly reported
    # as unmatched; the fix seeds it from the log's first timestamp.
    out, rc = run(SINGLE_PROXY, SINGLE_BZLOGGER)
    check(rc == 0, f"single-session run should exit 0, got {rc}")
    check(NO_MATCH_PHRASE not in out,
          f"must not claim {NO_MATCH_PHRASE!r} for a single-session capture")
    check("pid 1234 session 2026-08-09 14:20:00.000..2026-08-09 15:00:30.000, "
          "1.00 MB" in out,
          "must seed the session start from the log's first timestamp "
          "(14:20:00), not collapse it to the session-end time")

    print(f"analyze_drops pairing test: "
          f"{'PASS' if failures == 0 else f'{failures} FAILURES'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())