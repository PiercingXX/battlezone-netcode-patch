"""Verify analyze_drops.py counts observer.mesh errors inside the match window
and no longer makes the false causal claim.

The committed fixture tests/fixtures/mesh_fixture.txt holds two observer.mesh
errors inside the selected match window and five more after its RUN_WAS_QUIT
boundary (teardown).  The pre-T8 tool counted all seven whole-log errors and
divided them by the match's duration; it also printed "this flood precedes
both committed hard stops", which the T8 finding showed is false (the flood is
absent from vbgthykuj and runs after the storms die).

Run:  python3 tests/analyze_drops_mesh_test.py
"""

import subprocess
import sys

TOOL = 'tools/analyze_drops.py'
FIXTURE = 'tests/fixtures/mesh_fixture.txt'

# Two mesh errors fall inside the match window (14:45:02, 14:45:03); five more
# follow the RUN_WAS_QUIT boundary.  The in-window count must be 2, not 7.
IN_WINDOW_COUNT = 2

NEW_PHRASE = "'could not load observer.mesh' errors in this match"
OLD_CLAIM = "this flood precedes both committed hard stops"


def main():
    proc = subprocess.run([sys.executable, TOOL, FIXTURE],
                          capture_output=True, text=True, check=False)
    out = proc.stdout
    failures = 0

    def check(ok, what):
        nonlocal failures
        if not ok:
            failures += 1
            print(f"  FAIL: {what}")

    check(f"{IN_WINDOW_COUNT} {NEW_PHRASE}" in out,
          f"expected in-window count {IN_WINDOW_COUNT} + reworded phrase "
          f"{NEW_PHRASE!r} in output")
    check(OLD_CLAIM not in out,
          f"old causal claim {OLD_CLAIM!r} must not appear")
    check("7 'could not load observer.mesh' errors in this match" not in out,
          "whole-log count (7) must not be reported as in-window")

    print(f"analyze_drops mesh test: {IN_WINDOW_COUNT} in-window, "
          f"{'PASS' if failures == 0 else f'{failures} FAILURES'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())