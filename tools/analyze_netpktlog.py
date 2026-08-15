#!/usr/bin/env python3
"""Application-layer packet analysis for BZ98R's `-netpktlog` output.

Launching the game with `-netpktlog` turns on a layer of logging that nothing
in this project knew about until 2026-08-10, and it answers questions that a
year of transport-level analysis could not.  Three new line kinds appear in
BZLogger.txt:

    TX SRC  2 DST  1  Sent: Yes Packet: 7a7501000c0000... Send Type: 1
    RX SRC  1 DST  1 Packet: 0101fb147a6a00000000
    Chat Message: Bandwidth = 39950, used rate = 120

`Send Type` is the channel, and it is the field this whole tool exists for:

    Send Type 0  unreliable.  ~95% of all packets.  Dominated by type `5f00`,
                 ~220-byte datagrams carrying several objects' position and
                 velocity at once.  NEVER retransmitted -- no `5f00` has ever
                 appeared in a `TRY Sent Packet` line in any capture.
    Send Type 1  reliable.  ~5% of packets.  Types `7a75`, `236c`, `236e`,
                 `4d4b`, `504b`, `5053`.  These are the ONLY types that enter
                 the retry path, and therefore the only ones that can storm.

That distinction settles an argument that ran for weeks.  "The game sends
positions and velocities for all objects every tick" is true, and it is Send
Type 0 -- unreliable, droppable, and not what the retransmit storms are made
of.  The storms are Send Type 1 traffic for a single object.  Both statements
can be true at once, and the reason they looked contradictory is that
BZLogger's default logging shows only `TRY`, i.e. only retries of the reliable
channel.  See resources/CAMERAPOD_STORM.md.

`Sent: Yes/No` is the send decision -- the prioritise-and-drop that Nathan
Mates described for BZ1 in 1999 ("it had to prioritize packets, and could drop
some lesser-priority packets in order to keep bandwidth reasonable").  A
session with bandwidth headroom shows almost no drops.  Whether raising
MaxBandwidth 20x suppresses drops that would otherwise shed a storm is an open
question this tool is meant to answer: compare `drops` between a patched arm
and a `BZ_NET_TUNE=0` arm.

The headline number is the per-object emission rate.  A settled object sits
near or below 1 Hz.  Anything sustained above ~5 Hz on the reliable channel is
the fault condition, and one such object is enough to consume most of a
player's uplink once the ~10 ms retry timer multiplies it 4-8x.

Usage:
    tools/analyze_netpktlog.py BZLogger.txt [--top N] [--since HH:MM] [--until HH:MM]

BZLogger has torn/concatenated lines with no newline between records, so
everything here counts regex matches rather than lines.  Counting lines
undercounts, sometimes badly.
"""

import argparse
import binascii
import collections
import re
import sys

# ── Line formats ─────────────────────────────────────────────────────────────
TS = r'(\d\d:\d\d:\d\d)\.\d+'
TX_RE = re.compile(
    TS + r' TX SRC\s+(\d+) DST\s+(\d+)\s+Sent: (Yes|No)\s+Packet: ([0-9a-f]+) Send Type: (\d+)')
RX_RE = re.compile(TS + r' RX SRC\s+(\d+) DST\s+(\d+) Packet: ([0-9a-f]+)')
BW_RE = re.compile(TS + r' Chat Message: Bandwidth = (\d+), used rate = (\d+)')
# Default-on transport logging, present with or without -netpktlog.
TRY_RE = re.compile(TS + r' BZRNet P2P TRY Sent Packet \((\d+),(\d+)\) to (\S+?):')
# -netpktlog also turns on the scheduler and ping logging.  Two numbers matter:
# the unreliable position broadcast interval, and the game's own RTT measurement
# -- the reliable retry timer is ~10 ms, so RTT is what says how many redundant
# copies every reliable message costs.
PONG_RE = re.compile(TS + r' PONG RECEIVED: MST \d+ NET DELAY FROM PING = (\d+) PLAYER (\d+)')
PPI_RE = re.compile(r'Net::NextPositionPacketInterval (\d+)')
# Player chat, which is how a test run labels its own segments.  The game also
# emits several pseudo-chat diagnostics; only the <name> form is a real player.
CHAT_RE = re.compile(TS + r' Chat Message: <([^>]+)> (.+?)(?:\s*$|(?=\d{4}-\d\d-\d\d ))')

CHANNEL = {'0': 'unreliable', '1': 'reliable'}
# Thresholds for calling something a runaway.  Rate alone is not enough --
# see the note at the flag site.
kRunawayMinLifeS = 20
kRunawayMinPkts = 100
# Transport header is 18 bytes; the application payload TX logs starts there.
HDR_HEX = 36


def secs(t):
    h, m, s = t.split(':')
    return int(h) * 3600 + int(m) * 60 + int(s)


def identity(raw_hex):
    """Trailing printable token -- the object identity a packet carries, if any.

    `7a75` bodies end with `<odf><odf><instance>_<classLabel>`, e.g.
    `apcamr230_camerapod`.  Position/velocity packets usually carry an ODF name
    but no instance, and control packets carry none at all.
    """
    try:
        b = binascii.unhexlify(raw_hex)
    except (binascii.Error, ValueError):
        return None
    s = ''.join(chr(x) if 32 <= x < 127 else '.' for x in b)
    names = re.findall(r'[A-Za-z0-9_]{5,}', s)
    return names[-1] if names else None


def chat_markers(path):
    """Player chat lines, in order -- the boundaries of a narrated test run.

    A tester calling out "still" / "driving" / "selected" as they go turns the
    log into a self-labelling experiment, which beats trying to reconstruct
    what happened afterwards from memory.  See docs/PACKET_LOG_TEST.md.
    """
    res = []
    with open(path, errors='replace') as f:
        for line in f:
            for m in CHAT_RE.finditer(line):
                res.append((m.group(1), m.group(2), m.group(3).strip()[:52]))
    return res     # (timestamp, who, text)


def segment_report(path, top, chat=frozenset()):
    """Score each narrated segment separately."""
    marks = chat_markers(path)
    if len(marks) < 2:
        print("   (no player chat markers found -- narrate the run in chat to "
              "get per-segment scoring)")
        return
    print(f"\n   SEGMENTS from {len(marks)} chat markers")
    print(f"   {'from':<10}{'dur':>5}  {'rel/s':>7}{'unrel/s':>9}{'drops':>7}  label")
    for i, (t, who, text) in enumerate(marks):
        t1 = marks[i + 1][0] if i + 1 < len(marks) else None
        d = scan(path, t, t1)
        if not d['tx']:
            continue
        dur = max(1, (secs(t1) if t1 else secs(d['last'])) - secs(t))
        rel = d['chan'].get('1', 0) / dur
        unrel = d['chan'].get('0', 0) / dur
        print(f"   {t:<10}{dur:>4}s  {rel:>7.2f}{unrel:>9.1f}"
              f"{sum(d['drops'].values()):>7}  {text}")
        hot = [(w, o) for w, o in d['obj'].items()
               if o['st'].get('1') and w not in chat]
        for w, o in sorted(hot, key=lambda kv: -kv[1]['st']['1'])[:3]:
            print(f"          {o['st']['1']:>5} reliable  {w}")


def chat_tokens(path):
    """Words a player typed.  Chat packets carry their text where an object
    packet carries its identity, so without this the OBJECTS table fills up
    with the tester's own callouts."""
    toks = set()
    for _, _, text in chat_markers(path):
        toks.update(re.findall(r'[A-Za-z0-9_]{5,}', text))
    return toks


def scan(path, t0=None, t1=None):
    d = {
        'tx': 0, 'rx': 0, 'tx_bytes': 0,
        'chan': collections.Counter(),            # sendtype -> packets
        'chan_bytes': collections.Counter(),
        'ptype': collections.defaultdict(collections.Counter),   # sendtype -> type
        'drops': collections.Counter(),           # (sendtype, ptype) -> n
        'peer': collections.defaultdict(collections.Counter),    # dst -> sendtype
        'obj': collections.defaultdict(lambda: {'n': 0, 'st': collections.Counter(),
                                                'first': None, 'last': None,
                                                'bytes': 0}),
        'bw': [],                                  # (t, budget, used)
        'try_types': collections.Counter(),
        'try_n': 0,
        'first': None, 'last': None,
        'rtt': collections.defaultdict(list),      # player -> [ms]
        'ppi': collections.Counter(),              # position-packet interval, ms
    }

    def in_window(t):
        if t0 and t < t0:
            return False
        if t1 and t > t1:
            return False
        return True

    with open(path, errors='replace') as f:
        for line in f:
            for m in TX_RE.finditer(line):
                t, src, dst, sent, pkt, st = m.groups()
                if not in_window(t):
                    continue
                n = len(pkt) // 2
                d['tx'] += 1
                d['tx_bytes'] += n
                d['chan'][st] += 1
                d['chan_bytes'][st] += n
                ptype = pkt[:4]
                d['ptype'][st][ptype] += 1
                d['peer'][dst][st] += 1
                if sent == 'No':
                    d['drops'][(st, ptype)] += 1
                d['first'] = d['first'] or t
                d['last'] = t
                who = identity(pkt)
                if who:
                    o = d['obj'][who]
                    o['n'] += 1
                    o['bytes'] += n
                    o['st'][st] += 1
                    o['first'] = o['first'] or t
                    o['last'] = t
            for m in RX_RE.finditer(line):
                if in_window(m.group(1)):
                    d['rx'] += 1
            for m in BW_RE.finditer(line):
                t, budget, used = m.groups()
                if in_window(t):
                    d['bw'].append((t, int(budget), int(used)))
            for m in TRY_RE.finditer(line):
                if in_window(m.group(1)):
                    d['try_n'] += 1
            for m in PONG_RE.finditer(line):
                if in_window(m.group(1)):
                    d['rtt'][m.group(3)].append(int(m.group(2)))
            for m in PPI_RE.finditer(line):
                d['ppi'][int(m.group(1))] += 1
    # Retransmitted application types, read past the 18-byte transport header.
    with open(path, errors='replace') as f:
        for line in f:
            for m in re.finditer(
                    TS + r' BZRNet P2P TRY Sent Packet \(\d+,\d+\) to \S+?: ([0-9a-f]+)', line):
                if in_window(m.group(1)):
                    d['try_types'][m.group(2)[HDR_HEX:HDR_HEX + 4]] += 1
    return d


def report(d, path, top, chat=frozenset()):
    if not d['tx']:
        print(f"{path}   no TX lines")
        print("   this log was not captured with -netpktlog; see "
              "docs/PACKET_LOG_TEST.md")
        if d['try_n']:
            print(f"   ({d['try_n']} TRY retransmits are present -- those log by "
                  "default; use tools/analyze_drops.py for them)")
        return
    span = max(1, secs(d['last']) - secs(d['first']))
    print(f"{path}   {d['tx']} TX, {d['rx']} RX over {span}s "
          f"({d['first']}..{d['last']})")

    print("\n   CHANNEL SPLIT")
    print(f"   {'type':<12}{'packets':>10}{'share':>8}{'bytes':>12}{'share':>8}{'pkt/s':>9}")
    for st in sorted(d['chan']):
        n, b = d['chan'][st], d['chan_bytes'][st]
        print(f"   {CHANNEL.get(st, 'type ' + st):<12}{n:>10}{100*n/d['tx']:>7.1f}%"
              f"{b:>12}{100*b/max(d['tx_bytes'],1):>7.1f}%{n/span:>9.1f}")
    print(f"   {'total':<12}{d['tx']:>10}{'':>8}{d['tx_bytes']:>12}")

    print("\n   PACKET TYPES")
    for st in sorted(d['ptype']):
        line = '  '.join(f"{k}={v}" for k, v in d['ptype'][st].most_common(6))
        print(f"   {CHANNEL.get(st, st):<12}{line}")

    if d['try_types']:
        rel = set(d['ptype'].get('1', {}))
        unrel = set(d['ptype'].get('0', {}))
        seen = set(d['try_types'])
        print(f"\n   RETRANSMITTED TYPES ({d['try_n']} TRY datagrams)")
        print("   " + '  '.join(f"{k}={v}" for k, v in d['try_types'].most_common(8)))
        leaked = seen & unrel - rel
        if leaked:
            print(f"   ! unreliable types appear in the retry path: {sorted(leaked)}")
            print("     that contradicts Send Type 0 = never retransmitted -- investigate")
        else:
            print("   all retransmitted types are Send Type 1 (reliable), as expected")

    print("\n   SEND DECISIONS")
    drops = sum(d['drops'].values())
    if not drops:
        print("   no dropped packets -- the sender was never bandwidth-limited")
    else:
        print(f"   {drops} of {d['tx']} packets dropped before send ({100*drops/d['tx']:.2f}%)")
        for (st, pt), n in d['drops'].most_common(8):
            print(f"     {n:>7}  {CHANNEL.get(st, st):<11} {pt}")
        print("   drops are the engine's own prioritisation shedding load; compare this")
        print("   number between a patched arm and a BZ_NET_TUNE=0 arm")

    print("\n   PER PEER (destination player id)")
    for dst in sorted(d['peer']):
        c = d['peer'][dst]
        tot = sum(c.values())
        print(f"   dst {dst:<4}{tot:>9} packets   " +
              '  '.join(f"{CHANNEL.get(k, k)}={v}" for k, v in sorted(c.items())))

    print(f"\n   OBJECTS carrying an identity, top {top} by packet count")
    print(f"   {'object':<34}{'pkts':>8}{'bytes':>10}{'life':>8}{'rate':>9}  channel")
    rows = [kv for kv in sorted(d['obj'].items(), key=lambda kv: -kv[1]['n'])
            if kv[0] not in chat][:top]
    hot = []
    for who, o in rows:
        life = max(1, secs(o['last']) - secs(o['first']))
        rate = o['n'] / life
        chan = ','.join(f"{CHANNEL.get(k, k)}:{v}" for k, v in sorted(o['st'].items()))
        flag = ''
        # A runaway is SUSTAINED.  Requiring a lifetime and a packet count keeps
        # short bursts out: a dozen objects spawning in the same second reads as
        # 24/s over a 1 s lifetime and is not the fault condition.
        if (rate >= 5.0 and o['st'].get('1')
                and life >= kRunawayMinLifeS and o['n'] >= kRunawayMinPkts):
            flag = '  <-- RUNAWAY'
            hot.append((who, rate))
        print(f"   {who:<34}{o['n']:>8}{o['bytes']:>10}{life:>7}s{rate:>8.2f}/s  {chan}{flag}")
    if hot:
        print()
        for who, rate in hot:
            print(f"   ! {who} is emitting at {rate:.1f}/s on the reliable channel.")
        print("     A settled object sits near 1 Hz. Sustained >5 Hz reliable is the")
        print("     fault condition -- see resources/CAMERAPOD_STORM.md")

    if d['rtt'] or d['ppi']:
        print("\n   LINK")
        for pl, v in sorted(d['rtt'].items()):
            v = sorted(v)
            med = v[len(v) // 2]
            print(f"   RTT to player {pl}: median {med} ms  "
                  f"(min {v[0]}, p90 {v[int(len(v)*0.9)]}, n={len(v)})")
            print(f"     the reliable retry timer is ~10 ms, so a reliable message is "
                  f"resent ~{max(1, med // 10)}x before an ack can return")
        if d['ppi']:
            common = d['ppi'].most_common(1)[0]
            print(f"   position broadcast interval: {common[0]} ms "
                  f"({1000.0/max(common[0],1):.1f} Hz) on {common[1]} samples")

    if d['bw']:
        used = [u for _, _, u in d['bw']]
        budget = [b for _, b, _ in d['bw']]
        used.sort()
        print("\n   THE GAME'S OWN BANDWIDTH ACCOUNTING")
        print(f"   {len(d['bw'])} samples   budget {min(budget)}..{max(budget)} B/s")
        print(f"   used rate: median {used[len(used)//2]}  p90 {used[9*len(used)//10]}  "
              f"max {used[-1]} B/s")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('logs', nargs='+', help='BZLogger.txt captured with -netpktlog')
    ap.add_argument('--top', type=int, default=15, help='objects to list (default 15)')
    ap.add_argument('--since', default=None, metavar='HH:MM:SS')
    ap.add_argument('--until', default=None, metavar='HH:MM:SS')
    ap.add_argument('--segments', action='store_true',
                    help='score each narrated chat segment separately')
    a = ap.parse_args()
    rc = 0
    for p in a.logs:
        try:
            d = scan(p, a.since, a.until)
        except OSError as e:
            print(f"{p}: {e}", file=sys.stderr)
            rc = 1
            continue
        chat = chat_tokens(p)
        report(d, p, a.top, chat)
        if a.segments:
            segment_report(p, a.top, chat)
        print()
    return rc


if __name__ == '__main__':
    sys.exit(main())
