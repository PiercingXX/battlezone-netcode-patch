# Epoch re-fix — discriminate a peer RESTART from a loss-recovery RETRANSMIT

Claude-authored (2026-08-11) under build-plan rule 6. The first epoch fix gated
the reset on liveness, so an ordinary loss-recovery retransmit (a backward
sequence step the damper explicitly wants to let through) wiped the whole
suppression ring — collapsing suppression in the multi-message storm the feature
exists for. Its tests used ONE sequence per peer, so nothing caught it. The real
distinction is NOT liveness: a restart invalidates the whole counter; a
retransmit does not.

Criterion (operator-approved): an EXPLICIT reconnect signal is primary, with a
pure in-band backstop.

BRANCH LAW: experimental/v4.9, never master.

## Rules for this marathon (rule 6 is LAW here)

Every test that touches the ring MUST drive at least TWO concurrently-retried
sequences stepping backward past each other — the storm traffic pattern
(CAMERAPOD_STORM.md §A5: every reliable message goes out 6-9 times, several in
flight). A single-sequence test does not exercise this behavior and does not
count. Each fix names the adjacent regression it could cause and tests it. Final
gate: `make -C tests run` green at 64 and 32-bit.

## Tasks

- [ ] Replace the liveness-gated epoch heuristic with a pure in-band criterion: reset the peer ONLY when a sequence jumps backward BELOW the ring's oldest retained sequence (a retransmit is never below the oldest still-retained; a restart-near-zero is). Remove the old backward-step-plus-liveness branch entirely
  - verify: make -C tests dampen_epoch_test && ./tests/dampen_epoch_test
  - files: shared/send_dampen.h, tests/dampen_epoch_test.cpp

- [ ] Add the MULTI-SEQUENCE storm test (rule 6) that reproduces the shipped bug and proves the fix holds: for ONE peer, drive two concurrently-retried sequences (seq1 and seq2 both first-sent, then seq1 retransmitted past its own window while seq2 is still in flight). Assert ALL of: the ring is NOT wiped; seq2's redundant in-window copy IS suppressed; high_seq does not walk backward; AND the backoff-doubling branch stays reachable for a sequence below high_seq (an older sequence's second in-window copy backs off, e.g. 60->120 ms, rather than resetting to the floor). This is the exact interaction the single-sequence tests missed
  - verify: make -C tests dampen_storm_test && ./tests/dampen_storm_test
  - files: shared/send_dampen.h, tests/dampen_storm_test.cpp, tests/Makefile

- [ ] Wire dampen_purge_peer as the explicit PRIMARY reset signal: the damper exposes purge-on-reconnect and the proxy connect/disconnect path calls it, so a real restart is signalled explicitly rather than inferred; add a test that a purge clears the peer's ring and high_seq, and that after a purge a low sequence is treated as first-copy. (Proving the proxy actually calls it on a live reconnect is the operator's in-game step — noted deferred.)
  - verify: make -C tests dampen_purge_test && ./tests/dampen_purge_test
  - files: shared/send_dampen.h, Linux/proton_dsound_proxy/dsound_proxy.cpp, tests/dampen_purge_test.cpp, tests/Makefile

## Final gate

`make -C tests run` green at 64 and 32-bit, with the storm test proving
suppression HOLDS through concurrent backward-stepping retransmits.

## Deferred (operator, needs the game)

Proving dampen_purge_peer fires on a real Proton reconnect, and that
suppression behaves under a live 6-9x storm, are in-game steps. The offline
storm test is the maximally-rigorous proxy (rule 6), not a substitute.
