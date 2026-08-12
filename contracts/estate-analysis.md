# Estate-buildable marathon: battlezone-netcode-patch analysis & tooling

## Scoping decision (read before building)

This marathon covers only work that is **buildable and provable on this machine**
— pure-offline analysis and tooling whose `verify:` step runs a real test or a
real analysis script against data present in the repo, with no live game, no
Windows host, no peers, and no A/B match. Field sessions and anything gated on a
live run are deferred to the operator at the bottom of this file; they are never
tasks here.

**Verified evidence per decision.** Every claim below was checked against the
tree on `experimental/v4.9` this session:

- `todo.md` read in full (1848 lines). The backlog is the T-tasks (T1–T15) and
  field sessions (F1–F6); the P0/P1/P2 priority labels live on those.
- **The operator's two named examples are already DONE, so no task is built on
  them.** "P0 item 1" (sequence-field offset) is implemented by
  `tools/seq_crossmatch.py` (derives SEQUENCE u32 BE at offset 10, ACK at 14,
  100% of 65,860 samples) and recorded DONE in `CHANGELOG.md` V4.9 and todo.md's
  "Done in V4.9" table. "P0 item 3" (analyze_drops.py's four silent bugs — host
  governor format, `Actual Used` denominator, burst-vs-datagram counting,
  discarded byte counts) is fixed in the current `tools/analyze_drops.py`
  (docstring lines 1–97, `BANDWIDTH_RE` line 141, `TRY_SENT_PACKET_RE` line 169)
  and recorded DONE. Both tools run cleanly against the committed fixture
  `tests/fixtures/sample_bzlogger.txt` this session (see below).
- **There is no capture data on this machine.** `test_bundles/` holds only
  `README.md`; `test-logs/` holds only `README.md` and
  `2026-07-03_dup_test_summary.md`; `new logs/` does not exist; a workspace-wide
  search for `*.tar.gz`, `*.zip`, `BZLogger*`, `*_proxy.log`, `bz_buffer_log.bin`
  returned nothing. The `.gitignore` excludes `test_bundles/` (except README),
  `test-logs/*.log`, `**/BZLogger*.txt`, and `new logs/`. The READMEs state the
  bundles "are not in git" and "whatever is here on your disk is the only copy."
  **Consequence:** every todo item that cites a real capture (T1's replay test,
  T7's 08-08 acceptance, T9's four 08-09 bundles, T11's crashed-pid session,
  T14's wrong-offset resources, F-sessions) **cannot be verified here** and is
  scoped out or reduced to its data-independent core. The only committed data
  fixture is `tests/fixtures/sample_bzlogger.txt` (13 synthetic lines), which is
  a smoke fixture, not a ground-truth source (seq_crossmatch on it returns a
  wrong-looking answer because it holds only 2 truncated samples).
- Test runner confirmed: `make -C tests run` builds and runs five host-native
  C++ binaries (`reorder_test`, `net_globals_test`, `send_pace_test`,
  `gov_trace_test`, `buffer_filter_test`) with no game and no Windows
  (`tests/Makefile`). This is the repo's real test entrypoint and the final gate
  for every C++ task here.

**Frozen-assertion sweep (done):** `tests/net_globals_test.cpp` contains no
`DownCount`/`UpCount` assertion, so T3's default changes do not break an existing
test *by name* — but the preset table it exercises lives in
`shared/net_globals.h` and must be re-read by the builder before editing, and the
test file updated in the same task. `tests/gov_trace_test.cpp` asserts specific
verdicts for the two real V4.8 matches and the DownCount-step cases; T10 adds a
new rejoin case and does not alter existing ones. Any task that changes behavior
licenses the corresponding test file in the same task (see rules).

## Rules for this marathon

- **Branch law (operator's order):** all work in this repo lands on
  `experimental/v4.9` — NEVER `master`. You are checked out on
  `experimental/v4.9`; commit here.
- A `verify:` line RUNS behavior — a named test via `make -C tests run` (or a
  single named binary), or the analysis script itself executed against committed
  data with its exit code and output asserted by a test. `verify:` never greps.
- No two tasks share a verify command; no task's verify equals the final gate.
- **Final gate:** `make -C tests run` green at 64-bit (and 32-bit with
  `CXXFLAGS=-m32` where the task says so), plus CI green. This is the repo's real
  test entrypoint.
- Every code task ships with a test; the suite must stay green at 64-bit and
  32-bit. Update the test file inside the same task that changes the behavior —
  never leave the builder trapped between a failing gate and a rule violation.
- Do not build a task on unverified evidence. If a cited file or line is missing,
  record it and scope around it (see header). Do not invent data that is not on
  this machine.
- Nothing here needs a live game, Windows, peers, or an A/B. If a task turns out
  to need one, stop and move it to the deferred section rather than improvising.

---

## Tasks

- [ ] Implement the header-only per-(peer,seq) duplicate suppressor with unit tests, scoped to the pure-logic core that needs no field data.
  - **Title:** Implement the header-only per-(peer,seq) duplicate suppressor with unit tests, scoped to the pure-logic core that needs no field data.
  - **Scope note:** The full todo.md T1 includes a replay test (test 13) that feeds the 08-08 storm's `(peer, seq, len, timestamp)` trace extracted from a 75 MB log that is NOT on this machine. That replay is deferred (see bottom). This task delivers tests 1–12, which are pure unit tests over the header's own state machine and need no capture.
  - verify: make -C tests send_dampen_test && ./tests/send_dampen_test
  - files: shared/send_dampen.h, tests/send_dampen_test.cpp, tests/Makefile
  - **Acceptance:** tests 1–12 from todo.md T1 all pass; header compiles clean under `-Wall -Wextra`; no dynamic allocation, no platform headers, no `static` mutable state.

- [ ] Change `BZ_NET_DOWNCOUNT` 50→200 and `BZ_NET_UPCOUNT` 100→50.
  - **Title:** Change `BZ_NET_DOWNCOUNT` 50→200 and `BZ_NET_UPCOUNT` 100→50.
  - **Scope note:** The todo.md acceptance also wants a live session's `net_patch:` lines to report the new values — that needs a game and is deferred. This task proves the default change via the preset-table test only.
  - verify: ./tests/net_globals_test
  - files: shared/net_globals.h, tests/net_globals_test.cpp, README.md
  - **Gotcha:** re-read `shared/net_globals.h` before editing — the preset table lives around line 154 and defaults around line 88 per todo.md; verify the exact lines before changing. Do not also cap `BZ_NET_MAXBANDWIDTH` (todo.md T3 explicitly forbids it).

- [ ] Count `observer.mesh` errors inside the selected match window and reword the false causal claim.
  - **Title:** Count `observer.mesh` errors inside the selected match window and reword the false causal claim.
  - **Scope note:** The current code counts whole-log mesh errors (`analyze_drops.py:825`) and prints the misleading line at `analyze_drops.py:622–626`. Fix both; the acceptance number (vbgthykuj reports 0 in-window instead of 705,161) needs the vbgthykuj bundle which is not here, so the verify runs `tests/analyze_drops_mesh_test.py` against the committed `tests/fixtures/mesh_fixture.txt` and asserts the reworded, in-window behaviour with a synthetic case.
  - verify: python3 tests/analyze_drops_mesh_test.py
  - files: tools/analyze_drops.py, tests/analyze_drops_mesh_test.py, tests/fixtures/mesh_fixture.txt

- [ ] Require two consecutive low reads before declaring a revert, and add a `gov_trace_test` case for the client-rejoin pattern.
  - **Title:** Require two consecutive low reads before declaring a revert, and add a `gov_trace_test` case for the client-rejoin pattern.
  - **Scope note:** todo.md prefers the "two consecutive low reads" rule (needs no host/client knowledge). The acceptance re-runs the 08-09 PiercingXX bundle (not here); the verify proves the new state-machine rule via the added test.
  - verify: ./tests/gov_trace_test
  - files: shared/gov_trace.h, tests/gov_trace_test.cpp, tools/analyze_drops.py

- [ ] Match the proxy session to the BZLogger by pid and time window; say "no matching proxy session" instead of scoring the wrong one.
  - **Title:** Match the proxy session to the BZLogger by pid and time window; say "no matching proxy session" instead of scoring the wrong one.
  - **Scope note:** The acceptance needs the crashed-pid KFK bundle (not here). The verify runs `tests/analyze_drops_pairing_test.py` against the committed `tests/fixtures/proxy_pair_proxy.log` + `tests/fixtures/proxy_pair_bzlogger.txt` pair, asserting the new pairing logic.
  - verify: python3 tests/analyze_drops_pairing_test.py
  - files: tools/analyze_drops.py, tests/analyze_drops_pairing_test.py, tests/fixtures/proxy_pair_proxy.log, tests/fixtures/proxy_pair_bzlogger.txt

---

## Deferred to the operator (not tasks — require a live game, Windows, peers, an A/B, or data not on this machine)

- **T1 test 13 (replay fixture):** needs the 08-08 storm trace extracted from a
  75 MB log not present here. Operator must place the log under `test_bundles/`
  (or grant read access) and re-run the replay test after the core ships.
- **T2 (wire the damper into both proxies):** needs a cross-build and a live
  session on both platforms to verify `BZ_SEND_DAMPEN=0` byte-identity. Operator
  runs the two proxy builds and a session.
- **T4 (wrapper detects a real crash):** verify needs `taskkill /F` / `kill -9`
  on a live game and reading `meta.txt`. Operator runs the wrapper against a real
  session.
- **T5 (crash capture configured by installer):** needs a Windows host and a
  freshly installed machine. Operator runs the installer and a killed game.
- **T6 (build-skew guard):** the wrapper side needs a live bundle; the analyzer
  side alone is not the whole item. Operator runs the wrapper at bundle time.
- **T7 (per-object emission rate):** acceptance needs the 08-08 bundle (not
  here). Operator supplies the bundle to re-score the archive.
- **T9 (flag a client out-sending the host):** acceptance needs the four 08-09
  session-B bundles (not here). Operator supplies them.
- **T12 (file the mod bug upstream):** already REPORTED; awaiting the mod
  author's release. Operator watches workshop 3406347034 and diffs
  `SBPNavLogic.lua` against `resources/navfix/SBPNavLogic.patch`.
- **T13 (merge the two storm documents):** pure doc merge, no test; operator
  decides scope (not a buildable-code task).
- **T14 (delete/regenerate bogus reorder resources):** the two wrong-offset
  resources carry derived numbers from a capture not here; operator decides
  delete vs regenerate against `resources/BZ_P2P_HEADER.md`.
- **T15 (map the log switches):** needs a live game launch loop with
  `-netlog=2/3/9` and `-bzrnetlog=1`. Operator runs the two-minute cycles.
- **F1–F6 (all field sessions):** need people in a lobby; operator runs them per
  todo.md. F6 is BLOCKED on the mod fix shipping.
- **D1–D3 (open decisions):** need a person to decide, not an agent to implement.
  Do not action without asking.