# Related projects

Other people are working on BZ98R's netcode and internals. This is what they
have, what overlaps ours, and what is worth taking. Reviewed 2026-08-10.

---

## GrizzlyOne95/Battlezone98Redux_Shim

<https://github.com/GrizzlyOne95/Battlezone98Redux_Shim> — "WIP Shim DLL for
fixes", C, ~65 MB, actively pushed (same day as this review).

**The closest thing to a parallel project.** `src/patches/net_optimizer.cpp`
covers the same surface as our proxy — socket buffer sizing, DSCP marking,
auto-kick relaxation, packet reordering, packet duplication, governor start —
and `netcode_manifest.json` uses the identical 512 KB send / 4 MB receive
values. It also correlates explicitly against **our** `bz_buffer_log.bin` v2
capture format, so there is already interop whether or not it was intended.

### Worth taking

**1. Independent confirmation of the packet header.**
`reverse_engineering/bzrnet_protocol_capture_20260321.md` documents the same
18-byte header we derived, from paired host/joiner pcaps with Ghidra symbol
names:

| offset | field |
|---|---|
| `0x00` | flags: `0x40` final/unfragmented, `0x80` **reliable**, `0xC0` both |
| `0x01` | message kind |
| `0x02` | 8-byte clock |
| `0x0A` | reliable send sequence, big-endian |
| `0x0E` | peer ack sequence, big-endian |

This corroborates the correction made in
[`BZ_P2P_HEADER.md`](BZ_P2P_HEADER.md) on 2026-08-10 — that bit 7 means
*reliable*, not *retransmit*. Two independent derivations, same answer.

**2. Their safety rules.** `docs/NETWORK_PATCH_WORK_ORDERS.md` opens with a set
of global rules that are stricter and better stated than anything we have
written down:

> - Every memory write must be game-build gated and independently plausibility
>   checked.
> - Experimental packet-path behavior must be disabled by default.
> - No packet may be silently dropped, truncated, duplicated, delayed, or
>   reordered by a default configuration.
> - Every feature must preserve stock behavior when disabled or when validation
>   fails.
> - Runtime hooks must not perform expensive parsing or synchronous disk I/O on
>   latency-sensitive paths.
> - Ordinary telemetry must not retain raw public peer IP addresses.

Most of these we already follow by habit. Writing them down is better than
habit. See the note against [T1](../todo.md) about revisiting the damper's
default once it has field data.

**3. Tooling patterns.** A mature Python trace toolchain —
`align_bzrnet_traces.py` (two-PC clock alignment), `compare_bzrnet_traces.py`,
`validate_bzrnet_trace.py`, `bzrnet_capture_coverage.py`,
`correlate_bzrnet_wire.py`, `bzrnet_session_report.py` — plus a clean-room
matchmaking server at `reverse_engineering/bzrnet_server/server.py`. Their
two-PC clock alignment is a problem we have hit repeatedly and never solved
properly.

**4. They have decompiled the command-line parser, which answers our
[T15](../todo.md).** `reverse_engineering/decompilation_from_1.5_exe-pdb/Redux/Raw .C/FUN_007d5120-007d5120.c`
shows exactly what the logging switches do:

| switch | effect |
|---|---|
| `-netpktlog` | `if (DAT_009180d8 < 1) DAT_009180d8 = 1` — raises the level to 1, never lowers it |
| `-netlog` | same variable, same effect |
| `-netlog=<n>` | `DAT_009180d8 = atoi(n)` — **arbitrary level** |
| `-nonetpktlog` / `-nonetlog` | `DAT_009180d8 = 0` |
| `-bzrnetlog` | `DAT_008eda28 = 1` |
| `-bzrnetlog=<n>` | `DAT_008eda28 = atoi(n)` — **arbitrary level** |
| `-nobzrnetlog` | `DAT_008eda28 = 0` |

So there are two independent log levels behind two globals:

- **`DAT_009180d8`** — the packet log. `-netpktlog` only ever sets it to **1**,
  which is what we have been capturing. `-netlog=2` or higher is unexplored and
  may be what produces the transport-level `REL`/`UNR`/`BAS`/`CON` lines and the
  inbound `Received Packet … diff` timing that level 1 does not.
- **`DAT_008eda28`** — the BZRNet log, entirely unexplored.

Both are plain data addresses, so a proxy can raise them at runtime without a
launch-option change — which would let the wrapper turn capture on for one
session without the crew editing Steam settings.

### What they do not have

**Nothing on the retry timer.** Verified against the full object tree with a
positive control in the same pass: `retry timer`, `send window`, `ack timeout`
and `unacked` return zero; `backoff` hits a HUD sprite discovery timer and a
shader-cache offset; `retransmit` appears once, as an intention in their work
orders ("correlate candidate fields against capture order, duplicates,
retransmits, and game state"), not as analysis. `RTO` has no standalone token.

So the ~10 ms no-backoff measurement, the ack-stagnation analysis, and the
nav-beacon cause and fix remain original to this project.

### What we might tell them

An earlier draft of this file claimed they did not know `-netpktlog` existed.
**That was wrong** — see the correction note at the end. They found the
switches by decompiling the parser, which is a better route than ours.

What they may not have is the *output* side: what level 1 actually emits in a
live match, and what it is good for. `Send Type` separating reliable from
unreliable, `Sent: Yes/No` as the engine's own prioritise-and-drop,
`TempStateSendAll` giving the 48 ms position interval, and
`PONG RECEIVED … NET DELAY FROM PING` giving per-peer RTT are all things this
project has measured against live sessions —
[`../docs/PACKET_LOG_TEST.md`](../docs/PACKET_LOG_TEST.md),
[`../tools/analyze_netpktlog.py`](../tools/analyze_netpktlog.py). Their corpus
mentions those strings only as decompiler output, not as measurements.

---

## GrizzlyOne95/ExtraUtilities

<https://github.com/GrizzlyOne95/ExtraUtilities> — script extender and utility
mod, C++, LGPL, a fork of VTrider's original.

**`src/bzr.h`** is a substantial catalogue of validated memory addresses and
function pointers for 2.2.301 — camera, cheats, environment, selection,
multiplayer. Useful as a reference whenever this project needs an address it
has not derived itself.

**`src/Game/Multiplayer.cpp` has a replication lever already located.** Inline
patches at `0x005C833D` / `0x005C833B` force `BuildObject` to create objects
synchronously or asynchronously across the network, exposed to Lua as
`BuildAsyncObject` / `BuildSyncObject`:

```cpp
inline InlinePatch buildObjectAlwaysAsync(0x005C833D, BasicPatch::NOP, 11, ...);
inline InlinePatch buildObjectAlwaysSync (0x005C833B, BasicPatch::NOP,  2, ...);
```

Not our defect — the nav beacon storm is object *state*, not creation — but it
establishes that object replication has a toggleable sync/async path, and that
someone has found where it lives.

It also ships its own Lua 5.1 build (`Lua5.1-BZR/`), which is the mechanism by
which a script extender could expose new API surface.

---

## Not relevant to netcode

**GrizzlyOne95/bzfile** — <https://github.com/GrizzlyOne95/bzfile> — file IO
library with Lua bindings. Archive and asset handling only.

**GrizzlyOne95/Battlezone98Redux_WorldBuilder** —
<https://github.com/GrizzlyOne95/Battlezone98Redux_WorldBuilder> — Python tool
for terrain atlases, material files, TRN entries, cubemaps. No network content.

---

## Correction, 2026-08-10

The first version of this file asserted that the Shim project "appears not to
know `-netpktlog` exists". **That was false**, and it was published before being
checked. The claim came from a `git clone` that silently checked out 146 of
~45,900 files; grepping that 0.3% found nothing and the absence was read as
evidence. Searching the object database instead (`git grep` against `HEAD` on a
`--no-checkout` clone) finds `netpktlog` in 5 files, `bzrnetlog` in 5,
`TempStateSendAll` in 14, and `NET DELAY FROM PING` in 3 — including a full
decompilation of the switch parser that is *better* than what this project had.

A negative result from a search is only worth anything if a positive control
was run in the same pass. There was one available — `bzrnet`, `net_optimizer`,
`WSARecvFrom` — and it would have returned zero too, exposing the broken clone
immediately.

## Standing question

How much of the Shim's overlap with this project is convergent and how much is
shared lineage is not established, and the answer does not change what is worth
taking. The header corroboration is valuable precisely because the two
derivations used different methods — theirs pcap-and-Ghidra, ours BZLogger
ground truth against known ordinals.
