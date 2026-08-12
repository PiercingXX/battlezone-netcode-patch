# The BZ P2P packet header

Derived 2026-07-27 (V4.9). Reproduce with:

```sh
tools/seq_crossmatch.py test_bundles/*/BZLogger.txt --verify-clock
```

This supersedes every earlier statement about the sequence field, including the
V4.8 CHANGELOG entry, `resources/valid_capture_reorder_signal_*`, and the
comment block that stood at the top of `shared/reorder_core.h`.

## Layout

**Revised 2026-08-10.** Two fields below are corrected from the 2026-07-27
derivation; see [Corrections](#corrections-2026-08-10). The sequence and ack —
the fields this document exists to settle — are unchanged and still hold at
100%.

| offset | size | field | notes |
|---|---|---|---|
| 0 | 1 | flags | `0x40` = final/unfragmented, `0x80` = **reliable**, `0xC0` = both |
| 1 | 1 | kind | `0x00` gameplay, `0x02` endpoint discovery, `0x03` connect offer, `0x04` accept, `0x05` confirm, `0x06` endpoint update, `0x07` sequence ack |
| 2 | 8 | **SEND CLOCK** | u64 big-endian, Unix epoch **milliseconds**, stamped fresh on every copy including each retry |
| 10 | 4 | **SEQUENCE** | u32 big-endian — the sender's own message counter |
| 14 | 4 | **ACK** | u32 big-endian — highest sequence the sender has seen from this peer |
| 18 | .. | body | game-level payload; see [CAMERAPOD_STORM.md](CAMERAPOD_STORM.md) for what is decoded of it |

### Corrections (2026-08-10)

**Byte 0 bit 7 is `reliable`, not `retransmit`.** The original reading —
"`|0x80` = TRY, i.e. this datagram is a retransmit" — was wrong, and wrong in a
flattering direction. It made `reorder_is_retransmit()` look like it detected
resends; it detects reliability. Every datagram BZLogger prints as `TRY Sent
Packet` carries `0xC0` because TRY is the retry path *for reliable messages*,
so the bit tested true on 144,228 of 144,232 storm packets for a reason that
had nothing to do with retransmission.

Byte 0 is `0xC0` on **100% of 100,820** sampled TRY datagrams. A genuine
retransmit flag would have to be clear on first transmissions; no other value
of byte 0 occurs anywhere in the corpus. **Nothing in the header tells you a
datagram was resent** — the only way to know is to have seen that `(peer, seq)`
before.

**Offsets 2–9 are one u64 epoch-ms clock, not `3 bytes version + u40 clock`.**
The "always `00 00 01`" version field was the high three bytes of a
millisecond timestamp that had not yet rolled past them. Decoding all eight
bytes gives Unix epoch milliseconds that match BZLogger's own wall clock to the
millisecond, on every capture in the corpus:

| capture | field | decodes to | log clock |
|---|---|---|---|
| 2026-07-26 fixture (`tests/reorder_test.cpp`) | `0000019f9fc236ff` | 18:48:53.247Z | 18:48:53.2 |
| 2026-08-08, Windows host | `000001 9fe38d0de7` | 22:36:53.932Z | 16:36:53.932 (UTC−6) |

The old u40-at-offset-5 reading tracked the clock to "a median error of 0.3 ms"
because it was reading the low five bytes of the true field — correct enough to
validate the layout, wrong about where the field started.

Both corrections were found independently and agree with the public BZRNet
protocol capture in
[GrizzlyOne95/Battlezone98Redux_Shim](https://github.com/GrizzlyOne95/Battlezone98Redux_Shim),
`reverse_engineering/bzrnet_protocol_capture_20260321.md`, which documents the
same 18-byte header from paired host/joiner pcaps with Ghidra symbol names.
That is the only other public description of this header we are aware of.

**Verified against that source directly, 2026-08-10.** Their table reads
`0x40` final/unfragmented, `0x80` reliable, `0xC0` reliable + final; kind byte
at `0x01`; 8-byte clock at `0x02`; reliable send sequence big-endian at `0x0A`;
peer ack sequence big-endian at `0x0E`. Identical to the layout above.

The corroboration is worth more than usual because the two derivations share no
method: theirs is packet capture plus Ghidra, ours is BZLogger's own printed
ordinals brute-forced against `(offset, width, endian)`. See
[`RELATED_PROJECTS.md`](RELATED_PROJECTS.md).

### Why the send clock matters

It is stamped **per copy**, not once per message. Retransmissions therefore
carry the time they were actually sent, which makes the reliable-retry cadence
measurable from an ordinary BZLogger with no extra instrumentation — see
[CAMERAPOD_STORM.md](CAMERAPOD_STORM.md), where it turns out to be ~10 ms flat
with no backoff.

## Why this is not another guess

Every previous answer came from scoring payload offsets for per-peer
monotonicity. That method cannot distinguish a sequence number from an
acknowledgement number, a clock, or a length — and it was wrong twice.

BZLogger supplies ground truth. It prints its retransmits as

```
2026-07-26 14:41:33.627163 BZRNet P2P TRY Sent Packet (7,10) to 203.0.113.20:34354: c0000000019f9fbb81bb000000070000000a0001
```

so for every one of those lines both header ordinals are known *and* so are the
bytes they were encoded into. Brute-forcing `(offset, width, endian)` against a
known value either reproduces it on every sample or it does not.

* `u32be@10 == i` on **100% of 65,860 samples**
* `u32be@14 == n` on **100% of 65,860 samples**

across five packet classes, in every committed BZLogger. Two further formats
act as independent controls, because they carry ordinals from different code
paths: `CON Sent Packet (0,0)` and the bare `Sent Packet to` (implicitly
`(0,0)`).

The clock is the confirmation that the layout is *complete* rather than merely
consistent: it tracks BZLogger's own wall-clock timestamps to a median error of
**0.3 ms** across every log. Every byte of the header is accounted for, which
leaves no room for an alternative reading.

> Read at the time as a u40 at offset 5. It is a **u64 at offset 2** — see
> [Corrections](#corrections-2026-08-10). The 0.3 ms agreement held because the
> low five bytes are the same bytes; the error was where the field began, not
> what it was.

## What the earlier answers actually were

| version | claimed field | what it really is |
|---|---|---|
| V3 | u32 LE @13 | straddles the sequence's low byte and the ack's high bytes; changed once per 256 packets |
| V4.8 | u16 BE @16 | the **low half of the ACK field** |

The V4.8 error is the interesting one. An acknowledgement repeats by design —
you re-advertise the same "highest seen" on every datagram until the peer sends
you something newer. Read as a sequence number, that repetition looks exactly
like an 88.5% packet duplication rate, and that fabricated duplication rate is
what the V4.8 CHANGELOG and README published as a measured property of the
links.

## The consequence nobody expected

**The sequence counts messages, not datagrams.**

One message routinely spans several datagrams, and all of them carry the
identical sequence, ack and clock. In the committed capture there are 19,572
distinct 18-byte headers across 46,935 datagrams — a median of two to three
datagrams per header, up to six.

This demolishes the premise the reorder buffer was built on. `reorder_core.h`
opened with:

> Battlezone's receiver discards any datagram whose sequence number is not the
> exact successor of the last one it accepted.

It does not, and it cannot: 97.8% of the datagrams a peer receives carry a
sequence that has already been delivered, and the game plainly consumes them.
There is no per-datagram ordering key in this protocol, so there is nothing for
a reorder buffer to order.

It also exposed a live bug — see "Defect E" in `shared/reorder_core.h`.
`reorder_insert` rejected any already-delivered sequence as stale and both
proxies then *dropped* the datagram. With `BZ_REORDER=1` that discarded roughly
98% of inbound game traffic. `BZ_REORDER` has defaulted to `0` since V4.8, so no
shipped default was affected, but the option was documented and settable.
V4.9 passes those datagrams through to the game instead.

## Delivery, measured on the correct field

From the 8.1-minute committed capture (`buffer_linux_unknown-host_20260726T183650Z`),
host peer `203.0.113.20:34354`:

| metric | value |
|---|---|
| message sequences observed | 1,021 (span 705..1772) |
| never observed | 47 (4.40% of span) |
| first-arrival inversions | 28 (2.74%) |
| inversion lateness | median 690 ms, p95 883 ms, max 884 ms |
| retransmits among inbound datagrams | 576 of 46,935 (1.2%) |

Two caveats, both structural:

* **`never observed` is an upper bound on loss, not a loss rate.** A sequence
  value is only visible while it is the sender's *current* message. A value that
  advanced between two captured datagrams is indistinguishable from one that was
  lost. This capture's ring also wrapped and discarded 48% of events, which
  inflates it further.
* **The inversion figure is what retires the reorder buffer, on correct
  evidence this time.** Repairing those 28 inversions would have needed an
  883 ms hold window. The buffer shipped with a 100 ms ceiling, which would have
  recovered none of them — while adding latency to everything else.

## What this does not change

The V4.8 conclusion that the reorder buffer should ship off was correct. The
reasoning behind it was not. The corrected reasoning is stronger: it is not that
these particular links happen not to reorder, it is that the protocol carries no
key a receive-side reorder buffer could sort by.

The FEC proposal in `PATCH_OPTIONS_RESEARCH.md` §E is gated on "the sequence
field settled". It now is — but the finding moves the goalposts rather than
opening the gate. Parity coding needs a per-datagram sequence to reconstruct
against, and there isn't one; any FEC scheme between patched proxies would have
to carry its own.
