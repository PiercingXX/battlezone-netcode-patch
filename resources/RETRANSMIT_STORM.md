# The retransmit storm of 2026-07-26 14:44–14:53

The most extreme event in the whole dataset, and nothing had looked at it.
Reproduce everything below with:

```bash
python3 tools/analyze_drops.py test_bundles/buffer_linux_unknown-host_20260726T183650Z/BZLogger.txt
```

## What happened

PiercingXX hosting `bltop04.bzn`, two players, 15.2 minutes. Over nine of those
minutes the host retransmitted **65,806 datagrams / 5.07 MB** to a single peer —
**31% of every byte it sent**.

| minute | retransmitted datagrams | kB | sequence range |
|---|---:|---:|---|
| 14:41–14:43 | 14 | 0.5 | 7..342 |
| 14:44 | 694 | 52 | 490..965 |
| **14:45** | **21,637** | **1,678** | 966..6,961 |
| 14:46 | 7,384 | 569 | 7,065..9,757 |
| 14:47 | 5,817 | 448 | 9,786..11,979 |
| 14:48 | 7,740 | 596 | 11,979..14,987 |
| 14:49 | 6,274 | 479 | 14,987..17,630 |
| 14:50 | 4,457 | 342 | 17,657..19,748 |
| 14:51 | 6,207 | 477 | 19,773..22,106 |
| 14:52 | 4,189 | 321 | 22,107..24,024 |
| 14:53 | 1,386 | 106 | 24,047..24,702 |
| 14:54–14:55 | 7 | 0.3 | 24,739..24,866 |

Four in one minute, 23,000 the next, sustained for eight minutes, then back to
four. It looks like a wall.

## It is not loss recovery

The sequence range advances continuously through the storm. These are not the
same packets being resent over and over while acks fail to arrive — they are
*new* messages, each sent several times.

The decisive measurement is how long a message keeps being retransmitted:

| | copies per message | resent over |
|---|---:|---|
| this session | **3.57** | median **46 ms**, p99 77 ms |
| 2026-07-26 game 2 (KFK host) | 1.36 | — |
| 2026-07-05 | 1.37 | — |

Every message is emitted three to five times inside about **50 milliseconds**,
and then never again. No acknowledgement from a WAN peer can possibly arrive
inside 50 ms of the first transmission. **BZ is not retransmitting because an
ack failed to arrive — it is sending each reliable message several times up
front, on principle.** This is proactive redundancy, and it is invisible to
every bandwidth knob the patch has.

The peer was acking normally throughout: the host's acknowledgement field
(offset 14, see [BZ_P2P_HEADER.md](BZ_P2P_HEADER.md)) advanced continuously and
reached 24,999 — everything that was sent.

## And it is one message type

99% of the retransmitted datagrams are the same 78-byte message:

```
body signature 7a 75 <player-id> 00     65,248 of 65,806
```

That type dominates the retransmits in *every* session in the set (33–99%); the
fourth byte is just the destination player id. So the message type is not
special to this session either.

## What is actually different: the reliable-message rate

| session | reliable messages | duration | rate | copies each |
|---|---:|---|---:|---:|
| **this one** | **18,434** | 15.2 min | **20/s** | 3.57 |
| 2026-07-05 (5 peers) | 2,106 | ~19 min | 1.9/s | 1.37 |
| game 2 crash (KFK host, 4 peers) | 661 | 12.4 min | 0.9/s | 1.36 |
| game 2 (4 peers) | 445 | ~13 min | 0.6/s | 1.81 |
| game 1 (client, partial log) | 171 | 19.7 min | 0.14/s | 1.25 |

The storm is a **140× difference in how many reliable messages the host emitted
per second**, multiplied by a 2.6× difference in copies per message. Fewer
players, not more. The interesting question is not "why did it retransmit so
much" but "why did this host have twenty reliable messages a second to send".

## The syncJoin hypothesis is dead

`todo.md` §5 proposed that this run was the only one with `syncJoin = 1` in
`multi.ini` and suggested an A/B. **That A/B is not worth running**, on two
independent grounds already in the data:

1. Every session in the set — all five — logs `Sync: On` in its
   `Launching Network Game` line. `multi.ini` is only captured in the two
   bundles, so "every other run: 0" was read from a file the other runs never
   contributed.
2. The other bundle (`…183554Z`) *does* have `syncJoin = 0`, the same
   `vehicle = bvobserv`, and the same everything else — and it is a failed
   launch with no traffic, so it cannot support the contrast either.

Cross it off and spend the evening on the list below instead.

## What to test next, cheapest first

Both bundles are PiercingXX-as-host with `vehicle = bvobserv`. The one clean
contrast available is KFK-as-host, and that differs in four ways at once. In
rough order of cost:

1. **Player count.** This was a 2-player game; every low-retransmit session had
   4–5. A host may send per-peer reliable state at a rate that is *higher* with
   fewer peers (more budget per peer). Free to test: run the same map with 2
   and then 4 players.
2. **The map.** `bltop04.bzn` appears nowhere else in the set. Run one match on
   it and one on `uliovol1.bzn` with the same players and host.
3. **The observer vehicle.** `multi.ini` says `vehicle = bvobserv` in both
   bundles, and the packet bodies contain `bvobservbvobserv2_walker`. The two
   hard stops are both preceded by an `ERROR: could not load observer.mesh`
   flood (14,489 and 201,933 lines). Whether the observer is implicated in the
   retransmit rate, the crash, both, or neither is untested — change the vehicle
   in `multi.ini` and see.
4. **Host role held constant.** Whatever you test, one person hosts every arm.
   See "Host role is a variable" in [../docs/TESTING.md](../docs/TESTING.md).

## Why this matters beyond one session

If the protocol sends every reliable message ~3.6× up front whenever the
reliable-message rate is high, then a third of the uplink can be consumed by
redundancy the moment a session gets busy — and no governor setting changes
that. It also reframes the FEC proposal in `PATCH_OPTIONS_RESEARCH.md` §E:
adding parity to a stream that already carries 3.6 copies of everything would
be adding redundancy on top of redundancy.

`analyze_drops.py` now reports copies-per-message, the resend window, and the
message-type split on every run, and warns when copies exceed 2.5 inside a
sub-round-trip window — so the next occurrence is one command away instead of
nine months of nobody looking.
