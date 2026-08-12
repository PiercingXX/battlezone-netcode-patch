# Epoch purge — gate the reset to the P2P socket (Task-3 correction)

Claude-authored (2026-08-11) after Nagatha's adversarial audit caught a real
wiring bug the storm test and the human audit both missed. The epoch re-fix
wired `dampen_purge_peer` into `hooked_closesocket` (correct instinct) but did
NOT scope it to the P2P socket: the purge loop fires on EVERY `closesocket` call
and wipes the suppression ring for ALL peers. The sibling `reorder_reset` block
directly above gates itself with `was_reorder_sock = (s == g_reorder_sock)`;
the dampen block copied that block's "one UDP socket / session end" comment but
NOT its socket guard. Consequence: any unrelated socket close during a live
match (masterserver re-resolve, stats upload, a voice/aux socket) collapses P2P
suppression mid-session — the exact storm the damper exists to prevent.

BRANCH LAW: experimental/v4.9, never master.

## Rules for this marathon (rule 6 is LAW)

The decision "does closing socket S end the dampened session?" MUST be a pure,
host-testable predicate that BOTH the WinSock hook and the test call — so the
test exercises the real decision code, not a restatement of it. A test that
asserts a constant, or that re-implements the predicate instead of calling it,
is vacuous and does not count. `shared/send_dampen.h` must stay host-clean (no
`SOCKET`/WinSock types — the host tests compile it on Linux); the predicate
takes a plain integer handle. Final gate: `make -C tests run` green at 64 and
32-bit.

## Tasks

- [ ] Add a pure, host-testable predicate `dampen_close_ends_session(unsigned long long closing, unsigned long long dampen_sock)` to shared/send_dampen.h that returns true IFF `dampen_sock` is a valid tracked handle (non-zero / not the sentinel) AND `closing == dampen_sock`. This is the single source of truth for "this close ends the dampened session"
  - verify: make -C tests dampen_gate_test && ./tests/dampen_gate_test
  - files: shared/send_dampen.h, tests/dampen_gate_test.cpp, tests/Makefile

- [ ] Add the rule-6 test (dampen_gate_test) that drives the predicate through EVERY branch with a populated peer ring, proving the purge is correctly scoped: (a) closing == the tracked dampen socket -> true (session ended, purge is correct); (b) closing != the tracked dampen socket while peers are live -> false (an UNRELATED socket closed — the ring MUST survive); (c) dampen_sock == the untracked sentinel -> false (never purge before the P2P socket is even known). Assert that a false verdict leaves the ring and high_seq untouched — the regression the shipped bug caused
  - verify: make -C tests dampen_gate_test && ./tests/dampen_gate_test
  - files: shared/send_dampen.h, tests/dampen_gate_test.cpp

- [ ] Track the P2P socket and gate the proxy purge with the predicate, mirroring g_reorder_sock exactly. Set g_dampen_sock = s on the dampened send path (where g_reorder_sock is set at ~line 1534). In hooked_closesocket, compute was_dampen_sock via dampen_close_ends_session over the closing socket and g_dampen_sock, purge the peer ring ONLY when it is true, and clear g_dampen_sock to the invalid sentinel after — exactly as the reorder block clears g_reorder_sock. Remove the unconditional purge loop
  - verify: grep -n was_dampen_sock Linux/proton_dsound_proxy/src/dsound_proxy.cpp
  - files: Linux/proton_dsound_proxy/src/dsound_proxy.cpp

## Final gate

`make -C tests run` green at 64 and 32-bit. dampen_gate_test proves an unrelated
socket close leaves the ring intact; the proxy purge is gated on the tracked
dampen socket, mirroring the sibling reorder logic.

## Deferred (operator, needs the game)

Proving the tracked g_dampen_sock is exactly the live P2P socket under real
Proton play remains the operator's in-game step. The offline predicate test is
the maximally-rigorous proxy for the gating decision itself.
