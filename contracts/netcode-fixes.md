# Netcode fixes — the six defects an external review found in the v4.9 build

Claude-authored (2026-08-11) after a full external review found four real bugs,
a config/comment mismatch, and a CI gap in the shipped estate build. Each task
FIXES the defect AND adds a test that FAILS on the current code and passes after
— the missing coverage is the point: the original tests only exercised
fresh-peer seeding paths, so the update-path bugs shipped green.

BRANCH LAW: all work lands on experimental/v4.9 — NEVER master.

## Rules for this marathon

Every task ships a test that reproduces its defect (red before, green after) —
a fix with no failing-first test does not count. C tests run under
`make -C tests run` at BOTH 64-bit and 32-bit; Python tests run by their named
file. No two tasks share a verify. The final gate is `make -C tests run` green
AND both Python tests green AND CI green. Do not weaken any existing test.

## Tasks

- [ ] Fix the unsigned EWMA in send_dampen.h: the RTT estimate delta must be computed in int32_t so a downward sample does not underflow and pin the suppression window at the ceiling; add a send_dampen_test case that feeds a decreasing RTT sample and asserts the window tracks down instead of jumping to the max
  - verify: make -C tests send_dampen_test && ./tests/send_dampen_test
  - files: shared/send_dampen.h, tests/send_dampen_test.cpp

- [ ] Fix the epoch reset in send_dampen.h: a peer restarting near zero (a small backward step) must trigger the new-epoch reset, and when the reset fires it must reset high_seq so it does not re-fire on every subsequent packet and disable suppression; add tests for both the restart-near-zero case and the no-re-fire invariant
  - verify: make -C tests dampen_epoch_test && ./tests/dampen_epoch_test
  - files: shared/send_dampen.h, tests/dampen_epoch_test.cpp, tests/Makefile

- [ ] Fix the gov_trace held false-positive: when the verify window times out with a reverted (cold-start sentinel) read and low_seen was armed, the result must be kGovClamped, not kGovHeld — a reverted session must not be promoted into the A/B sample set; add a gov_trace_test case for the timeout-with-reverted-read path
  - verify: ./tests/gov_trace_test
  - files: shared/gov_trace.h, tests/gov_trace_test.cpp

- [ ] Fix the first-session proxy pairing in analyze_drops.py: seed the session start from the log's first timestamp so the ordinary one-process-one-match capture pairs instead of silently reporting "no matching proxy session"; extend the pairing test with a single-session fixture that reproduces the common capture shape
  - verify: python3 tests/analyze_drops_pairing_test.py
  - files: tools/analyze_drops.py, tests/analyze_drops_pairing_test.py

- [ ] Reconcile the DownCount tuning across all three sources and fix its comment: net_globals.h, net-ini/net.ini, and both proxy READMEs must state the SAME DownCount value with a correct description (DownCount is the over-MaxPing back-off adjustment, not a receive budget); add/adjust the net_globals_test so it pins the reconciled value and the invariant that net.ini mirrors the compiled default
  - verify: ./tests/net_globals_test
  - files: shared/net_globals.h, net-ini/net.ini, Linux/proton_dsound_proxy/README.md, tests/net_globals_test.cpp

- [ ] Wire the declared Python verifies into CI: .github/workflows/ci.yml must run tests/analyze_drops_mesh_test.py and tests/analyze_drops_pairing_test.py (and the new dampen_epoch_test binary) — a task's declared verify command that CI never executes is how these defects shipped green; the final gate is not met until CI runs every task's verify
  - verify: python3 tests/analyze_drops_mesh_test.py
  - files: .github/workflows/ci.yml

## Final gate

`make -C tests run` green at 64-bit and 32-bit, both Python tests green, and CI
(which now runs them all) green on experimental/v4.9.
