#!/usr/bin/env python3
"""Per-link BZRNet discard analysis.

BZRNet logs every rejected packet as:
  BZRNet P2P Dropping Packet Type 0 For Client <steamid> (Packet #R received, #E expected)

These lines are NOT loss, and they are not out-of-order arrivals either.  On the
2026-07-26 five-log set, 6,998 of 7,012 such lines had R < E — the packet had
*already been consumed*.  A packet arriving early (R > E) is accepted, not
dropped, so it never appears here at all.  Every line is therefore a duplicate
or a retransmission that arrived after the original.  Reading them as "stale /
out-of-order" and scoring an A/B on that number, as this tool did until
2026-07-26, overstated the interesting quantity by 2-3x.

Classified per sender:
  dup1    (E-R == 1)  duplicate of the packet just consumed — engine resend or
                      a network duplicate.  The ordinary background rate.
  resend  (E-R >= 2)  a block of already-delivered packets sent again; E-R is
                      how far back the block reached.  Rises with real loss on
                      the reverse path, because the sender is not being acked.
  exp0    (E == 0)    no expected sequence for this peer: the same R repeats
                      2-5x within a millisecond.  23-46% of all lines.
  ahead   (R > E)     a packet from the future.  Vanishingly rare (14 of 7,012);
                      if it climbs, the sequence model here is wrong.

None of these four is a loss count.  The real loss signal in BZLogger is the
sender retransmitting unacked data, and it comes in two forms that mean
different things:

  BZRNet P2P TRY Sent 303 to IP:PORT                    one BURST, 303 bytes
  BZRNet P2P TRY Sent Packet (i,n) to IP:PORT: <hex>    one retransmitted DATAGRAM

The bare number is the byte total of the Packet lines just above it.  Until
V4.9 this tool counted bursts as though they were packets, discarded the byte
count, and did not match the Packet form at all — 11,986 bursts counted where
there were 65,806 datagrams and 5.07 MB of retransmission.  All three are now
reported, and the headline is the retransmit **share of outbound bytes**.

That share needs a real denominator, and one has been sitting in the logs all
along.  Host logs write `Actual Used` next to every governor adjustment: the
game's own measurement of what it is really sending, in bytes per second, in
the same unit as the budget beside it.  Client logs do not have it, so the
proxy's `session end: send_stats: bytes=` counter stands in when you pass the
matching proxy log on the same command line.  The old median-governor-budget
denominator is retired: it measured the allowance, not the traffic.

`Actual Used` also answers the question a BZ_GOV_START A/B is actually asking
— utilisation.  Under ~60% of budget, raising the governor cannot help that
session; over ~90%, the session is genuinely governor-limited and is a valid
sample.

Drop counts are a misleading score for a second reason: a packet the proxy
holds and later releases in order stops being counted whether or not the player
is better off.  So this also reads the proxy's own V4.7 `reorder_stats:` lines
out of dsound_proxy.log / winmm_proxy.log and reports what the buffer did —
above all hold_ms(max), the latency the patch itself added, and
delivered_evicted, packets it had to release out of order under storage
pressure.  Judge an A/B on all three numbers, not on the drop count alone.

If those lines are absent entirely, the reorder buffer never ran: it is
implemented only in the WSARecvFrom hook and bypasses overlapped receives.
Confirm with buffer-logging/decode_buffer_log.py before crediting it for
anything.

Three things this also flags, because each one silently invalidated an earlier
conclusion:

  * **How the log ended.** Clean exit, ends abruptly (both committed crashes
    look identical: a normal match end, then nothing — no shutdown lines), or
    copied while the game was still in the lobby — which is not a crash and
    must not be counted as one.  An observer.mesh error flood is a log-volume
    problem, not a crash signature (see resources/CAMERAPOD_STORM.md).
  * **Torn proxy-log lines.** Two committed logs contain them, because several
    threads write the log with no shared lock. A torn line is silently
    unparseable by everything downstream.
  * **Whether the governor poke landed.** A proxy log states the BZ_GOV_START
    it asked for; the game's log states what it actually got. When they
    disagree, the session is not a sample of that setting. A V4.9 proxy also
    says so itself.

Usage:
  analyze_drops.py LOG [LOG ...] [--launch SUBSTR] [--names steamid=Name,...]

  LOG              BZLogger.txt files, and/or dsound_proxy.log / winmm_proxy.log
                   (the proxy logs are detected by content, not by name).
  --launch SUBSTR  Restrict to the session whose "Launching Network Game" line
                   contains SUBSTR (default: the last launch in each file).
  --names          Comma-separated steamid=Name map for readable output.
                   A built-in map covers the 2026-07 test crew.

And drops are not the symptom either.  Warping is.  BZLogger records every
suspicious position update as "Possible Large Warp", but on a real session log
91% of those lines are sub-metre corrections that no player could see, so the
raw count (10,227 in one session) is meaningless.  This buckets them by actual
distance and reports the visible ones per minute, alongside the governor's
bandwidth range and any auto-kick / lag events.

Exit status is 0 on success, 1 if nothing analysable was found in any file.
"""
import argparse
import math
import re
import sys
from collections import defaultdict
from datetime import datetime

DROP_RE = re.compile(
    r'^([\d\-]+ [\d:.]+) BZRNet P2P Dropping Packet Type 0 For Client '
    r'(S\d+) \(Packet #(\d+) received, #(\d+) expected\)'
)
LAUNCH_RE = re.compile(r'Launching Network Game (.+?), Map ([^,\s]+)')
START_RE = re.compile(r'Starting BattleZone 98 Redux')
# The match actually ending — the player returning to the shell.  This, not the
# next process start, is a session's real end boundary.
MATCH_END_RE = re.compile(r'SetRunning: was RUN_STARTED, now RUN_WAS_QUIT')

# V4.7 proxy counters, emitted every 10 s and once more at session end.
REORDER_RE = re.compile(r'(session end: )?reorder_stats: (.+)$')
# Fields inside that line are all "name=number" or "name(a=1 b=2)" groups.
FIELD_RE = re.compile(r'([a-z_]+)=(\d+)')
# max_hold_ms only exists from V4.7 on; matching it optionally means a
# pre-V4.7 proxy log is still recognised as a proxy log and reported as such
# rather than being mistaken for a BZLogger with no drops in it.
CONFIG_RE = re.compile(
    r'reorder: (enabled|DISABLED) max_window_ms=(\d+) min_window_ms=(\d+)'
    r'(?: max_hold_ms=(\d+))?')

# BZLogger position-correction records.  The vector is the distance the object
# was moved by; most are noise, so callers filter by magnitude.
WARP_RE = re.compile(
    r'^([\d\-]+ [\d:.]+) Possible Large Warp: Distributed Object Updated Position By '
    r'\(([-\d.eE+]+),([-\d.eE+]+),([-\d.eE+]+)\)'
)
# The governor announcing its current send budget.  There are TWO formats, and
# matching only the first meant every host log in the dataset silently reported
# "no governor data" — 171 and 184 lines thrown away:
#   client:  Net: Bandwidth usage now set to 16000, Interval 50 ms
#   host:    Net: Bandwidth usage now set to 82100, Interval 48, Actual Used 61270
# `Actual Used` is the real-bytes-sent denominator the CHANGELOG's "Known limit
# in the loss metric" section says does not exist.  It has been in every host
# log all along.
BANDWIDTH_RE = re.compile(
    r'Net: Bandwidth usage now set to (\d+), Interval (\d+)(?: ms|, Actual Used (\d+))')
# A line's leading BZLogger timestamp, only when it is intact. Torn
# multi-writer lines can carry any garbage before a matched message, and a
# captured garbage "timestamp" crashes parse_ts() long after collection.
TS_PREFIX_RE = re.compile(r'^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d[\d.]*)')
# Host-side auto-kick machinery (see the AutoKick* entries in shared/net_globals.h).
LAG_RE  = re.compile(r'Chat Message: (.+?) is lagging')
UNLAG_RE = re.compile(r'Chat Message: (.+?) stopped lagging')
KICK_RE = re.compile(r'Auto kicking player (.+?) due to (.+)')
# A second drop class, distinct from the Type 0 stale discards.
UNKNOWN_SRC_RE = re.compile(r"Dropping Packet \(Source IP doesn't match any known player\)")

# The sender retransmitting unacked reliable data.  This, not the Type 0
# discards, is the log's actual loss signal — and it is logged in two forms
# that mean different things:
#
#   BZRNet P2P TRY Sent 303 to IP:PORT            one retransmit BURST, 303 bytes
#   BZRNet P2P TRY Sent Packet (i,n) to IP:PORT: <hex>   one retransmitted DATAGRAM
#
# The bare number is the byte total of the Packet lines immediately preceding
# it — verified exactly on every burst in the committed capture.  Until V4.9
# this tool counted bursts as if they were packets and threw the byte count
# away, and did not see the Packet form at all: 11,986 bursts counted against
# 65,806 datagrams and 5.07 MB ignored.  Bursts and bytes rank sessions
# differently (that session averaged 423 B/burst against ~80 elsewhere), so the
# choice of unit was silently deciding A/B outcomes.
TRY_SENT_RE = re.compile(r'BZRNet P2P TRY Sent (\d+) to ([\d.]+):(\d+)')
TRY_SENT_PACKET_RE = re.compile(
    r'^([\d\-]+ [\d:.]+) .*BZRNet P2P TRY Sent Packet \((\d+),\d+\) to '
    r'([\d.]+):(\d+): ([0-9a-fA-F]*)')

# Body offset in the BZ P2P header (see resources/BZ_P2P_HEADER.md).  The first
# four body bytes identify the message type; the fourth is the destination
# player id, so `7a7501` and `7a7503` are the same type to two different peers.
BZ_BODY_OFFSET = 18

# A clean shutdown.  Its absence is how both committed crashes are identifiable.
EXIT_RE = re.compile(r'Exiting Game With Return Code (-?\d+)')
# The teardown-crash signature: a flood of these precedes both hard stops.
MESH_ERR_RE = re.compile(r'ERROR: could not load observer\.mesh')
# Lines that mean "the game was still sitting in the lobby when this file was
# copied", which is a different thing from a crash and must not be reported as
# one.
IDLE_TAIL_RE = re.compile(r'OnLobbyListReceived|OnLobbyDataUpdate|CNetGameLobby::')

# Proxy log framing.  Every line starts with a bracketed timestamp; anything
# else is a torn write (see the log-serialisation item in todo.md).
PROXY_LINE_RE = re.compile(r'^\[\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+\]\[pid=\d+\] ')
PROXY_STAMP_RE = re.compile(r'\]\[pid=\d+\] ')
# Client-side real-bytes denominator, the counterpart to the host's Actual Used.
SEND_STATS_RE = re.compile(r'session end: send_stats: packets=(\d+) bytes=(\d+)')
# The same line with its framing, so each proxy session can be paired to the
# BZLogger by pid and time window instead of silently scoring the largest one.
SEND_STATS_LINE_RE = re.compile(
    r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]\[pid=(\d+)\] '
    r'session end: send_stats: packets=(\d+) bytes=(\d+)')
# Leading timestamp of any proxy-framed line.  The process's first line is the
# earliest a session could have started, so a one-process-one-match capture's
# only session is seeded from it instead of collapsing to a zero-width window
# that fails to overlap its own BZLogger match.
PROXY_TS_RE = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]\[pid=\d+\] ')
# What the proxy intended the governor to open at, so the game's own log can be
# checked against it without hand-correlating two files.
GOV_CFG_RE = re.compile(r'governor_patch: enabled \(BZ_GOV_START=(\d+)')
# The V4.9 read-back verdicts.  The clamp regex captures the read-back value:
# pre-V4.91 proxies convicted the governor's own DownCount steps (reads just
# below target — 39650..39900 across eight matches on 2026-08-02), and the
# captured value is what lets those old logs be rescored instead of thrown out.
GOV_HELD_RE = re.compile(r'governor_patch: poke held')
GOV_HELD_ACTIVE_RE = re.compile(r'governor_patch: poke held.*governor already adjusting')
GOV_CLAMP_RE = re.compile(r'governor_patch: POKE DID NOT HOLD [-—] wrote \d+, reads (\d+)')
GOV_READBACK_RE = re.compile(r'governor_patch: read-back on')
GOV_STOCK_FLOOR = 16000  # stock MaxBandwidth; a real overwrite reverts to/below this

# Distance buckets in metres.  "visible" starts at 50 m: below that a correction
# is inside the noise a player reads as ordinary movement.
WARP_BUCKETS = (1.0, 10.0, 50.0, 200.0, 1000.0)
WARP_VISIBLE_M = 50.0

# dup1-rate thresholds for inferring outbound duplication, per minute per link.
# Measured: ~8/min is ordinary engine resend noise, ~40/min is reachable under
# heavy congestion with dup off, ~300/min is BZ_SEND_DUP actually running.
# These only raise a flag now.  The old code silently halved the "real" column
# whenever it tripped, which meant the headline number changed by 2x on a
# heuristic — a worse failure than the noise it was correcting for.
DUP_RATE_PER_MIN = 150.0
DUP_RATE_SUSPECT = 60.0

DEFAULT_NAMES = {
    'S76561198884003346': 'PiercingXX',
    'S76561198094230200': 'KFK',
    'S76561199559935298': 'Bison',
    'S76561199732480793': 'Monkey',
}


def parse_ts(s):
    return datetime.strptime(s[:19], '%Y-%m-%d %H:%M:%S')


def parse_ts_precise(s):
    """Sub-second timestamp.  parse_ts truncates to whole seconds, which is
    fine for session boundaries and useless for retransmit timing — the whole
    point of that metric is that the copies land inside ~50 ms."""
    try:
        return datetime.strptime(s[:26], '%Y-%m-%d %H:%M:%S.%f')
    except ValueError:
        return parse_ts(s)


# WAN connect lines bind a Steam ID to the IP the retransmit table keys by.
# Without this join, "whose IP is that?" took a manual grep on 2026-08-03
# even though every log carries the answer.
WAN_CONNECT_RE = re.compile(r'WAN Connect For Client (S\d+) \(IP ([0-9.]+):')

# Game-level roster lines.  ID 1 is the host: every session with a known
# host in the corpus shows them as ID 1, and the host role is a controlled
# variable in every A/B — recording it automatically beats trusting the
# log-naming convention.
ADD_PLAYER_RE = re.compile(r'\bAdding Player (.+?) \(ID (\d+), Team \d+\)')


def all_match_slices(lines):
    """Every match in the log as (start_idx, end_idx, lobby, map).

    Same boundaries as session_slice: a match ends at its RUN_STARTED ->
    RUN_WAS_QUIT transition or at the next launch line, whichever comes first.
    """
    launches = [(i, m.group(1), m.group(2))
                for i, l in enumerate(lines)
                for m in [LAUNCH_RE.search(l)] if m]
    out = []
    for k, (start_idx, lobby, mapname) in enumerate(launches):
        limit = launches[k + 1][0] if k + 1 < len(launches) else len(lines)
        end_idx = limit
        for j in range(start_idx + 1, limit):
            if MATCH_END_RE.search(lines[j]):
                end_idx = j
                break
        out.append((start_idx, end_idx, lobby, mapname))
    return out


def match_overview(lines):
    """One cheap summary row per match, for logs that hold a whole evening.

    Until V4.91 the analyzer scored only the last lobby of a BZLogger — the
    2026-08-02 log held nine matches (including the 2-player configuration the
    retransmit-storm question needs) and eight of them were silently skipped.
    """
    rows = []
    for start, end, lobby, mapname in all_match_slices(lines):
        seg = lines[start:end]
        t0 = t1 = None
        for ln in seg:
            m = TS_PREFIX_RE.match(ln)
            if m:
                t = parse_ts(m.group(1))
                t0 = t if t0 is None else t0
                t1 = t
        dur_min = ((t1 - t0).total_seconds() / 60.0) if t0 and t1 else 0.0
        players = set()
        host = None
        for ln in seg:
            m = ADD_PLAYER_RE.search(ln)
            if m:
                players.add(m.group(1))
                if m.group(2) == '1':
                    host = m.group(1)
        discards = sum(1 for ln in seg if DROP_RE.match(ln))
        retx = sum(1 for ln in seg if TRY_SENT_PACKET_RE.match(ln))
        rows.append({'lobby': lobby, 'map': mapname, 'dur_min': dur_min,
                     'players': len(players), 'discards': discards,
                     'retx': retx, 'host': host})
    return rows


def session_slice(lines, launch_substr):
    """Return (start_idx, end_idx, lobby, map) for the chosen session.

    A match runs from its 'Launching Network Game' line to the RUN_STARTED ->
    RUN_WAS_QUIT transition that ends it.  Ending instead at the next 'Starting
    BattleZone' — a *process* restart, which most sessions never do — ran every
    match into all the matches after it: on the 2026-07-26 set that reported a
    20.8 min game as 72.5 min and folded five later matches' drops into it.
    Falling back to the next launch line, then to the process restart, keeps
    older logs that lack the SetRunning markers working.
    """
    launches = [(i, m.group(1), m.group(2))
                for i, l in enumerate(lines)
                for m in [LAUNCH_RE.search(l)] if m]
    if not launches:
        return 0, len(lines), None, None
    if launch_substr:
        matches = [x for x in launches if launch_substr in x[1]]
        chosen = matches[-1] if matches else launches[-1]
    else:
        chosen = launches[-1]
    start_idx = chosen[0]
    for j in range(start_idx + 1, len(lines)):
        if MATCH_END_RE.search(lines[j]):
            return start_idx, j, chosen[1], chosen[2]
        if LAUNCH_RE.search(lines[j]):
            return start_idx, j, chosen[1], chosen[2]
    end_idx = len(lines)
    for j in range(start_idx + 1, len(lines)):
        if START_RE.search(lines[j]):
            end_idx = j
            break
    return start_idx, end_idx, chosen[1], chosen[2]


def parse_session_events(lines):
    """Warp, governor, lag, retransmit and kick events from a BZLogger slice."""
    warps = []          # (timestamp, magnitude in metres)
    bandwidth = []      # (timestamp, bytes/sec, interval ms, actual_used or None)
    lag = []            # (timestamp, kind, who)
    unknown_src = 0
    mesh_errors = 0
    # Per peer: retransmit bursts, retransmitted datagrams, and bytes.
    retransmit = defaultdict(lambda: {'bursts': 0, 'packets': 0, 'bytes': 0})
    # Per (peer, sequence): how many copies, and over what span.
    msg_copies = defaultdict(lambda: {'n': 0, 'first': None, 'last': None})
    msg_types = defaultdict(int)

    for ln in lines:
        m = WARP_RE.match(ln)
        if m:
            x, y, z = float(m.group(2)), float(m.group(3)), float(m.group(4))
            warps.append((parse_ts(m.group(1)), math.sqrt(x * x + y * y + z * z)))
            continue
        m = BANDWIDTH_RE.search(ln)
        if m:
            # Torn multi-writer lines are real in this dataset; a garbled
            # prefix would otherwise blow up parse_ts() far from here.
            ts = TS_PREFIX_RE.match(ln)
            if ts:
                used = int(m.group(3)) if m.group(3) is not None else None
                bandwidth.append((ts.group(1), int(m.group(1)), int(m.group(2)), used))
            continue
        for rx, kind in ((LAG_RE, 'lagging'), (UNLAG_RE, 'recovered'), (KICK_RE, 'KICKED')):
            m = rx.search(ln)
            if m:
                ts = TS_PREFIX_RE.match(ln)
                lag.append((ts.group(1) if ts else '(no timestamp)', kind, m.group(1)))
                break
        # Packet form first: the bare form's regex is a prefix of it, so testing
        # the bare one first would count every datagram line as a burst.
        m = TRY_SENT_PACKET_RE.match(ln)
        if m:
            peer, seq, payload = m.group(3), int(m.group(2)), m.group(5)
            retransmit[peer]['packets'] += 1
            # Copies per message and how long a message keeps being resent are
            # what make a retransmit total interpretable. In the one extreme
            # session in the dataset each message was sent 3.6 times inside a
            # ~50 ms window and then never again — proactive redundancy, not
            # loss recovery, which no amount of bandwidth tuning will change.
            ts = parse_ts_precise(m.group(1))
            msg = msg_copies[(peer, seq)]
            msg['n'] += 1
            if msg['first'] is None:
                msg['first'] = ts
            msg['last'] = ts
            if len(payload) >= (BZ_BODY_OFFSET + 4) * 2:
                sig = payload[BZ_BODY_OFFSET * 2:(BZ_BODY_OFFSET + 4) * 2].lower()
                msg_types[sig] += 1
            continue
        m = TRY_SENT_RE.search(ln)
        if m:
            retransmit[m.group(2)]['bursts'] += 1
            retransmit[m.group(2)]['bytes'] += int(m.group(1))
            continue
        if MESH_ERR_RE.search(ln):
            mesh_errors += 1
            continue
        if UNKNOWN_SRC_RE.search(ln):
            unknown_src += 1

    return {'warps': warps, 'bandwidth': bandwidth, 'lag': lag,
            'unknown_src': unknown_src, 'mesh_errors': mesh_errors,
            'retransmit': retransmit, 'msg_copies': msg_copies,
            'msg_types': msg_types}


def classify_ending(lines):
    """How did this log stop: cleanly, abruptly, or was it copied mid-game?

    Both committed crashes end the same way — a normal match end, then
    nothing, with no shutdown lines at all.  But "no shutdown lines" on its
    own also describes a log copied while the game was still sitting in the
    lobby, which is not a crash.  The tail tells them apart: a live snapshot
    ends in lobby polling, a crash ends mid-activity.
    """
    for ln in reversed(lines):
        m = EXIT_RE.search(ln)
        if m:
            return {'kind': 'clean', 'code': int(m.group(1)), 'last': None}
    tail = lines[-40:]
    if any(IDLE_TAIL_RE.search(l) for l in tail):
        kind = 'running'
    else:
        kind = 'abrupt'
    last = next((l for l in reversed(lines) if l.strip()), '')
    return {'kind': kind, 'code': None, 'last': last[:110]}


def print_session_events(ev, dur_min, ip_names=None, match_window=None):
    warps = ev['warps']
    if warps:
        mags = sorted(m for _, m in warps)
        n = len(mags)
        visible = sum(1 for m in mags if m >= WARP_VISIBLE_M)
        rate = (visible / dur_min) if dur_min else 0.0
        print(f"   warps: {n} logged, {visible} visible (>={WARP_VISIBLE_M:.0f}m) "
              f"= {rate:.1f}/min   median={mags[n // 2]:.1f}m "
              f"p99={mags[min(n - 1, int(n * 0.99))]:.0f}m max={mags[-1]:.0f}m")
        parts = []
        prev = 0.0
        for b in WARP_BUCKETS:
            c = sum(1 for m in mags if prev <= m < b)
            parts.append(f"<{b:g}m:{c}")
            prev = b
        parts.append(f">={WARP_BUCKETS[-1]:g}m:{sum(1 for m in mags if m >= WARP_BUCKETS[-1])}")
        print("          " + "  ".join(parts))

    bw = ev['bandwidth']
    real_bytes = None
    if bw:
        rates = [r for _, r, _, _ in bw]
        # The logged figure is a total across peers, so a lone 4000 sample at
        # match start is just the cold start before the last peer joins and is
        # normal even when the poke is working.  Only a rate that *stays* there
        # means the bump never landed.  Warning on any 4000 sample, as this did
        # until 2026-07-26, fired on healthy patched sessions.
        cold = sum(1 for r in rates if r <= 4000)
        note = ""
        if cold >= 3:
            note = (f"   <-- {cold} samples still at/below the 4000 cold start"
                    " (BZ_GOV_START / MinBandwidth poke not landing)")
        role = 'host' if any(u is not None for _, _, _, u in bw) else 'client'
        print(f"   governor [{role} log]: {len(rates)} adjustments, "
              f"{min(rates)} -> {max(rates)} B/s"
              f", median {sorted(rates)[len(rates) // 2]}{note}")

        # Cross-check the game's own numbers against what the proxy said it
        # was writing.  Doing this by hand across two logs with clocks an hour
        # apart is how the 2026-07-26 game-1 failure was found in the first
        # place; a V4.9 proxy also says so itself in its own log.
        want = ev.get('gov_start')
        if want:
            opened = rates[0]
            if opened < want and max(rates) < want:
                print(f"   ! the proxy asked for BZ_GOV_START={want}; this session "
                      f"opened at {opened} and never reached it. The poke did not "
                      "land — not a valid sample for that setting.")
            elif opened < want:
                print(f"   ! the proxy asked for BZ_GOV_START={want} but the "
                      f"session opened at {opened} and only ramped there later")
        # How long the opening trickle lasted: the ramp, not the ceiling, is
        # what a short match actually lives with.
        for target in (40000, 80000):
            if max(rates) >= target:
                t0 = bw[0][0]
                hit = next(t for t, r, _, _ in bw if r >= target)
                mins = (parse_ts(hit) - parse_ts(t0)).total_seconds() / 60.0
                print(f"   reached {target} B/s after {mins:.1f} min")

        # `Actual Used` is the governor's own measurement of what the game is
        # really sending, in the same unit as the budget it sits next to: bytes
        # per second, not bytes in that interval (at 21,250 B/s with a 48 ms
        # interval it reads ~20,600, which is a second's worth, not 48 ms').
        # It is present in every host log and has been all along — the
        # "Known limit in the loss metric" section of the CHANGELOG says no
        # real-bytes signal exists, and this is it.
        #
        # Utilisation is the number that matters: it says whether the governor
        # budget was ever the binding constraint, which is exactly what a
        # BZ_GOV_START A/B is trying to find out.
        samples = [(t, r, u) for t, r, _iv, u in bw if u is not None]
        if samples:
            mean_used = sum(u for _, _, u in samples) / len(samples)
            mean_rate = sum(r for _, r, _ in samples) / len(samples)
            util = (100.0 * mean_used / mean_rate) if mean_rate else 0.0
            peak_used = max(u for _, _, u in samples)
            print(f"   actual send rate: mean {mean_used:.0f} B/s, peak {peak_used} B/s "
                  f"= {util:.0f}% of the governor's own budget")
            if util < 60:
                print("   ! the budget was not the binding constraint in this "
                      "session — raising BZ_GOV_START cannot help it")
            elif util > 90:
                print("   ! the game was pressed against the budget — this session "
                      "IS governor-limited and is a valid BZ_GOV_START sample")
            # Integrate the rate over the sampled span for a byte estimate.
            # Estimate, not a counter: samples are only emitted when the
            # governor adjusts, and they cover part of the session.
            total = 0.0
            for a, b in zip(samples, samples[1:]):
                dt = (parse_ts(b[0]) - parse_ts(a[0])).total_seconds()
                total += (a[2] + b[2]) / 2.0 * dt
            span_min = (parse_ts(samples[-1][0]) - parse_ts(samples[0][0])).total_seconds() / 60.0
            if total > 0:
                real_bytes = total
                print(f"   ~{total / 1e6:.2f} MB sent over the {span_min:.1f} min "
                      "these samples span (estimate: integrated Actual Used, not a counter)")

    rt = ev.get('retransmit')
    if rt:
        bursts = sum(d['bursts'] for d in rt.values())
        packets = sum(d['packets'] for d in rt.values())
        rbytes = sum(d['bytes'] for d in rt.values())
        rate = (packets / dur_min) if dur_min else 0.0
        def peer_label(ip):
            name = (ip_names or {}).get(ip)
            return f"{name}({ip})" if name else ip
        detail = ", ".join(
            f"{peer_label(ip)}={d['packets']}pkt/{d['bytes'] / 1e3:.0f}kB" for ip, d in
            sorted(rt.items(), key=lambda kv: -kv[1]['packets'])[:6])
        print(f"   retransmits: {packets} datagrams in {bursts} bursts, "
              f"{rbytes / 1e6:.2f} MB = {rate:.1f} datagrams/min   {detail}")
        if bursts:
            print(f"                {rbytes / bursts:.0f} B per burst "
                  f"({packets / bursts:.1f} datagrams)")

        # Copies per message separates 'the link is losing packets' from 'the
        # protocol sends everything several times regardless'.
        mc = ev.get('msg_copies') or {}
        if mc:
            msgs = len(mc)
            lifetimes = sorted((d['last'] - d['first']).total_seconds() * 1000.0
                               for d in mc.values() if d['first'] and d['last'])
            n = len(lifetimes)
            med = lifetimes[n // 2] if n else 0
            p99 = lifetimes[int(n * 0.99)] if n else 0
            print(f"                {msgs} distinct messages, "
                  f"{packets / msgs:.2f} copies each, resent over "
                  f"med={med:.0f}ms p99={p99:.0f}ms")
            if packets / msgs > 2.5 and med < 250:
                print("   ! every message is being sent several times inside a "
                      "window far shorter than a round trip. That is proactive "
                      "redundancy, not loss recovery — bandwidth tuning will not "
                      "reduce it (see resources/RETRANSMIT_STORM.md)")
        mt = ev.get('msg_types') or {}
        if mt:
            tt = sum(mt.values())
            top = sorted(mt.items(), key=lambda kv: -kv[1])[:3]
            print("                by message type: " + "  ".join(
                f"{k}={100.0 * v / tt:.0f}%" for k, v in top))

        # Normalise against real bytes sent, and always say which denominator
        # was used — a counter and an estimate are not comparable with each
        # other, so an A/B has to hold the denominator fixed too.
        # Preference order: the proxy session that actually covers this match,
        # then the integrated Actual Used estimate, then nothing.  The
        # median-governor-budget denominator this tool used until V4.9 is
        # retired: it measured the allowance, not the traffic.
        matched = match_proxy_session(ev.get('proxy_sessions') or [], match_window)
        if matched:
            pb = matched['bytes']
            print(f"   retransmit share of outbound bytes: {100.0 * rbytes / pb:.1f}%"
                  f"   <-- compare A/Bs on this (denominator: proxy `session end "
                  f"bytes=`, pid {matched['pid']} session "
                  f"{matched['start']}..{matched['end']}, {pb / 1e6:.2f} MB)")
        elif ev.get('proxy_sessions'):
            print(f"   ! no matching proxy session: {len(ev['proxy_sessions'])} "
                  "proxy session(s) present but none overlaps this match's time "
                  "window — not scoring a retransmit share against the wrong "
                  "session")
        elif real_bytes:
            print(f"   retransmit share of outbound bytes: ~"
                  f"{100.0 * rbytes / real_bytes:.1f}%"
                  "   <-- compare A/Bs on this (denominator: integrated Actual "
                  "Used, an estimate)")
        elif bw:
            med_bps = sorted(r for _, r, _, _ in bw)[len(bw) // 2]
            mb_per_min = med_bps * 60.0 / 1e6
            if mb_per_min > 0:
                print(f"   retransmits per MB of governor budget: "
                      f"{rate / mb_per_min:.1f}")
                print("   ! no real-bytes denominator in this log (client-side "
                      "format). Pair it with the matching proxy log for a "
                      "comparable number.")

    for ts, kind, who in ev['lag']:
        mark = "!!" if kind == 'KICKED' else " -"
        print(f"   {mark} {ts} {who}: {kind}")

    if ev['unknown_src']:
        print(f"   dropped {ev['unknown_src']} packets from unrecognised source IPs")

    if ev.get('mesh_errors'):
        n = ev['mesh_errors']
        rate = (n / dur_min) if dur_min else 0.0
        print(f"   ! {n} 'could not load observer.mesh' errors in this match "
              f"({rate:.0f}/min) — log-volume problem; not correlated with the "
              "retransmit storms (see resources/CAMERAPOD_STORM.md)")


def parse_reorder_stats(lines):
    """Return (config, last_periodic, session_end) from a proxy log.

    Counters are cumulative, so the last line seen is the session total.  A
    "session end:" line is the authoritative one when present (it is emitted
    from closesocket, before the per-peer state is reset).
    """
    config = None
    last = None
    at_end = None
    for ln in lines:
        cm = CONFIG_RE.search(ln)
        if cm:
            config = {
                'enabled': cm.group(1) == 'enabled',
                'window_ms': int(cm.group(2)),
                'min_ms': int(cm.group(3)),
                'max_hold_ms': int(cm.group(4)) if cm.group(4) else None,
            }
            continue
        m = REORDER_RE.search(ln)
        if not m:
            continue
        fields = {k: int(v) for k, v in FIELD_RE.findall(m.group(2))}
        last = fields
        if m.group(1):
            at_end = fields
    return config, last, at_end


def match_proxy_session(sessions, window):
    """Return the proxy session whose time window overlaps the BZLogger's match
    window, or None.

    A proxy log spans the whole process and usually holds several matches; the
    old code silently scored the largest, which is often a different match.  The
    BZLogger carries no pid of its own, so the operative criterion is window
    overlap; pid is kept on each session so sessions from different game
    processes stay distinct candidates.
    """
    if not sessions or not window:
        return None
    bz_start, bz_end = window
    for s in sessions:
        s_start = parse_ts(s['start'])
        s_end = parse_ts(s['end'])
        if s_end >= bz_start and s_start <= bz_end:
            return s
    return None


def proxy_log_health(path, lines):
    """Framing and session sanity for a proxy log.

    Two committed logs contain torn lines — one missing its leading `[2`, one
    with two lines concatenated — because multiple threads write the log
    without a shared lock.  A torn line is silently unparseable by everything
    downstream, so it has to be reported, not skipped.
    """
    torn_missing = [i + 1 for i, l in enumerate(lines)
                    if l.strip() and not PROXY_LINE_RE.match(l)]
    torn_joined = [i + 1 for i, l in enumerate(lines)
                   if len(PROXY_STAMP_RE.findall(l)) > 1]
    if torn_missing or torn_joined:
        n = len(torn_missing) + len(torn_joined)
        print(f"   ! {n} torn log line(s): concurrent writers, no shared lock")
        for lineno in (torn_missing + torn_joined)[:5]:
            print(f"     line {lineno}")

    text = '\n'.join(lines)
    gov_start = 0
    gov = GOV_CFG_RE.search(text)
    if gov:
        gov_start = int(gov.group(1))
        held = len(GOV_HELD_RE.findall(text))
        active = len(GOV_HELD_ACTIVE_RE.findall(text))
        clamp_reads = [int(v) for v in GOV_CLAMP_RE.findall(text)]
        # A pre-V4.91 proxy called any read below target a failed poke; a read
        # above the stock floor is the governor's own DownCount steps and the
        # poke actually held.  Rescore instead of discarding the session.
        false_alarms = [v for v in clamp_reads if v > GOV_STOCK_FLOOR]
        real_clamps = [v for v in clamp_reads if v <= GOV_STOCK_FLOOR]
        parts = []
        if real_clamps:
            parts.append(f"<-- {len(real_clamps)} match(es) reported POKE DID "
                         "NOT HOLD (reverted to stock) — not valid samples")
        if false_alarms:
            parts.append(f"{len(false_alarms)} pre-V4.91 false alarm(s) rescored "
                         f"as held: reads {min(false_alarms)}..{max(false_alarms)} "
                         "are the governor adjusting, the poke worked")
        if held:
            note = f" ({active} with the governor already adjusting)" if active else ""
            parts.append(f"{held} match(es) verified held{note}")
        if not parts:
            if GOV_READBACK_RE.search(text):
                parts.append("read-back armed but no cold-start poke was ever "
                             "observed — no verdict, not a proxy version issue")
            else:
                parts.append("no read-back in this log — pre-V4.9 proxy")
        print(f"   BZ_GOV_START={gov.group(1)}  " + "; ".join(parts))

    if SEND_STATS_LINE_RE.search(text) or SEND_STATS_RE.search(text):
        all_sessions = [(m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)))
                        for m in (SEND_STATS_LINE_RE.match(l) for l in lines) if m]
        empty = sum(1 for _, _, p, b in all_sessions if p == 0 and b == 0)
        if empty:
            print(f"   {empty} empty session(s) (packets=0 bytes=0) — failed "
                  "launches, not measurements; ignore them")
        # Build per-session records with a pid and a time window, so the
        # BZLogger can be paired to the session that actually covers it instead
        # of the largest one.  A proxy log spans the whole process and usually
        # holds several matches; each session runs from the previous session-end
        # line to its own, which keeps the windows contiguous.
        sessions = []
        prev_end = None
        first_ts = None
        for ln in lines:
            m = PROXY_TS_RE.match(ln)
            if m:
                first_ts = m.group(1)
                break
        for ts, pid, p, b in all_sessions:
            if p or b:
                # The first session has no previous session-end to bound its
                # start, so seed it from the process's first line.  A
                # one-process-one-match capture (the ordinary shape) otherwise
                # collapses to a zero-width window that fails to overlap its own
                # BZLogger match and is silently reported as unmatched.
                start = prev_end if prev_end is not None else (first_ts or ts)
                sessions.append({'pid': pid, 'start': start, 'end': ts,
                                 'packets': p, 'bytes': b})
            prev_end = ts
        if sessions:
            best = max(s['bytes'] for s in sessions)
            print(f"   {len(sessions)} session(s) with traffic, largest sent "
                  f"{best / 1e6:.2f} MB")
            return sessions, gov_start
    return [], gov_start


def print_reorder_report(path, config, stats, crashed=False):
    print(f"{path}   [proxy reorder counters]")
    if config:
        state = 'enabled' if config['enabled'] else 'DISABLED'
        hold = (f"max_hold={config['max_hold_ms']}ms" if config['max_hold_ms'] is not None
                else "max_hold=n/a (pre-V4.7)")
        print(f"   config: {state}  window={config['window_ms']}ms "
              f"floor={config['min_ms']}ms {hold}")
    if not stats:
        if crashed:
            # A log whose process died before teardown loses its session-end
            # stats by construction — the detach path never runs.  King's
            # 2026-08-03 crash bundle got the misleading V4.7 warning below
            # for exactly this, and "old proxy" and "crashed process" call for
            # opposite responses.
            print("   ! no session-end stats — the process died before teardown "
                  "(hard crash);")
            print("     counters were lost with it, not missing from an old proxy.\n")
            return
        if config and config['max_hold_ms'] is not None:
            # max_hold_ms only exists from V4.7, and V4.7 emits reorder_stats
            # every 10 s plus once from closesocket.  Absent counters on a V4.7
            # proxy are not ambiguous: the buffer handled zero packets.
            print("   ! NO reorder_stats at all on a V4.7 proxy — the reorder buffer")
            print("     never buffered a packet this session. It is implemented only in")
            print("     the WSARecvFrom hook and bypasses overlapped receives, so any")
            print("     improvement measured here belongs to the [Net] poke and socket")
            print("     buffers instead. Confirm the receive path with")
            print("     buffer-logging/decode_buffer_log.py before crediting it.\n")
        else:
            print("   (no reorder_stats lines — pre-V4.7 proxy, or reorder never ran)\n")
        return

    delivered = stats.get('delivered', 0)
    in_order = stats.get('in_order', 0)
    ordered_pct = (100.0 * in_order / delivered) if delivered else 0.0
    print(f"   delivered={delivered}  in_order={in_order} ({ordered_pct:.1f}%)  "
          f"forced={stats.get('forced', 0)}  evicted={stats.get('evicted', 0)}")
    # stale/dup are pass-throughs from V4.9 on, not drops: the sequence counts
    # messages, so a repeated one is ordinary payload. Pre-V4.9 proxies really
    # did drop them, which is why the label matters.
    print(f"   passed through: stale={stats.get('stale', 0)} dup={stats.get('dup', 0)}"
          f"   dropped: reclaim={stats.get('reclaim', 0)}   "
          f"bypass: short={stats.get('short', 0)} tblfull={stats.get('tblfull', 0)}")

    # The numbers that say what the patch cost, not just what it caught.
    hold_avg = stats.get('avg', 0)
    hold_max = stats.get('max', 0)
    print(f"   ADDED LATENCY: hold_ms avg={hold_avg} max={hold_max}")
    notes = []
    if stats.get('evicted', 0):
        notes.append(f"{stats['evicted']} packets released out of order under "
                     "storage pressure (raise BZ_REORDER_DEPTH)")
    if stats.get('truncated', 0):
        notes.append(f"{stats['truncated']} datagrams did not fit the game's "
                     "receive buffer and were reported as WSAEMSGSIZE rather "
                     "than silently shortened")
    if stats.get('emsgsize', 0):
        notes.append(f"{stats['emsgsize']} datagrams exceeded the drain buffer "
                     "(raise kReorderMaxPktBytes)")
    if stats.get('tblfull', 0):
        notes.append(f"{stats['tblfull']} packets bypassed a full peer table")
    if config and config['max_hold_ms'] is not None and hold_max > config['max_hold_ms'] + 20:
        notes.append(f"hold_ms max={hold_max} exceeds the {config['max_hold_ms']}ms "
                     "ceiling by more than scheduling jitter explains")
    for n in notes:
        print(f"   ! {n}")
    print()


def analyze_file(path, launch_substr, names):
    with open(path, errors='replace') as fh:
        lines = fh.read().splitlines()
    start_idx, end_idx, lobby, mapname = session_slice(lines, launch_substr)

    stats = defaultdict(lambda: {'dup1': 0, 'resend': 0, 'exp0': 0, 'ahead': 0,
                                 'depths': []})
    tmin = tmax = None
    for ln in lines[start_idx:end_idx]:
        m = DROP_RE.match(ln)
        if not m:
            continue
        cid, r, e = m.group(2), int(m.group(3)), int(m.group(4))
        d = stats[cid]
        if e == 0:
            d['exp0'] += 1
        elif r > e:
            d['ahead'] += 1
        elif e - r == 1:
            d['dup1'] += 1
        else:
            d['resend'] += 1
            d['depths'].append(e - r)
        t = parse_ts(m.group(1))
        tmin = t if tmin is None else tmin
        tmax = t
    events = parse_session_events(lines[start_idx:end_idx])
    # mesh_errors is already counted inside the slice by parse_session_events.
    # It used to be re-counted over the whole file here, which both divided a
    # whole-log count by one match's duration (116,556/min for a 6-minute match
    # whose true peak was 58,398/min) and implied the teardown flood was part of
    # the selected match.  The flood is a log-volume problem, not a causal
    # signature: it is absent from vbgthykuj entirely and runs after the storms
    # die (see resources/CAMERAPOD_STORM.md).

    # Drops can be sparse; warps usually bracket the session better.
    if events['warps']:
        wt = [t for t, _ in events['warps']]
        tmin = min(wt) if tmin is None else min(tmin, min(wt))
        tmax = max(wt) if tmax is None else max(tmax, max(wt))

    # The session's own time span — the window a proxy session must overlap to
    # be the right denominator.  A match with no drops and no warps would
    # otherwise have no window to pair against.
    for ln in lines[start_idx:end_idx]:
        m = TS_PREFIX_RE.match(ln)
        if m:
            t = parse_ts(m.group(1))
            tmin = t if tmin is None else min(tmin, t)
            tmax = t if tmax is None else max(tmax, t)

    host = None
    for ln in lines[start_idx:end_idx]:
        m = ADD_PLAYER_RE.search(ln)
        if m and m.group(2) == '1':
            host = m.group(1)
            break

    # IP -> player name, from the whole file: connects happen in the lobby,
    # before any session slice starts.
    ip_names = {}
    for ln in lines:
        m = WAN_CONNECT_RE.search(ln)
        if m:
            ip_names[m.group(2)] = names.get(m.group(1), m.group(1))

    dur_min = ((tmax - tmin).total_seconds() / 60.0) if tmin and tmax else 0.0
    return {
        'path': path, 'lobby': lobby, 'map': mapname, 'host': host,
        'dur_min': dur_min, 'stats': stats, 'names': names,
        'events': events, 'ending': classify_ending(lines),
        'overview': match_overview(lines), 'ip_names': ip_names,
        'tmin': tmin, 'tmax': tmax,
    }


def print_report(rep):
    names = rep['names']
    hdr = f"{rep['path']}"
    if rep['lobby']:
        hdr += f"   lobby={rep['lobby']} map={rep['map']} dur={rep['dur_min']:.1f}min"
        if rep.get('host'):
            hdr += f" host={rep['host']}"
    print(hdr)
    if len(rep.get('overview') or []) > 1:
        print(f"   {len(rep['overview'])} matches in this log — detailed report "
              "below covers the one named above (pick another with --launch):")
        print("     lobby                map           min  players  discards  retx  host")
        for r in rep['overview']:
            print(f"     {r['lobby'][:20]:<20} {r['map'][:13]:<13} "
                  f"{r['dur_min']:>4.1f}  {r['players']:>7}  {r['discards']:>8}  "
                  f"{r['retx']:>4}  {r['host'] or '?'}")
        hosts = {r['host'] for r in rep['overview'] if r['host']}
        if len(hosts) > 1:
            print("     ! host changed mid-evening — matches above are NOT "
                  "comparable as one A/B arm (host role is a controlled variable)")
    if not rep['stats']:
        print("   (no Type 0 drop lines in this session)")
        print_session_events(rep['events'], rep['dur_min'], rep.get('ip_names'),
                             (rep.get('tmin'), rep.get('tmax')))
        print()
        return
    rows = sorted(rep['stats'].items(), key=lambda kv: -kv[1]['resend'])
    dur = rep['dur_min'] or 0
    totals = defaultdict(int)
    for cid, d in rows:
        name = names.get(cid, cid)
        for k in ('dup1', 'resend', 'exp0', 'ahead'):
            totals[k] += d[k]
        dup1_rate = (d['dup1'] / dur) if dur else 0.0
        # Duplication is inferred from the dup1 rate, not a raw count: an 8
        # minute game accumulates ~60 from ordinary engine resends alone, so
        # the old absolute >50 test flagged healthy sessions as duplicating.
        # Measured baselines: ~8/min idle, up to ~40/min under heavy
        # congestion without dup, ~300/min with BZ_SEND_DUP actually on.
        if dup1_rate >= DUP_RATE_PER_MIN:
            flag = '  [sender dup very likely ON]'
        elif dup1_rate >= DUP_RATE_SUSPECT:
            flag = '  [dup1 rate high - congestion or dup; check the sender]'
        else:
            flag = ''
        total = d['dup1'] + d['resend'] + d['exp0'] + d['ahead']
        if total == d['dup1'] and d['dup1']:
            flag += '  [loopback self-echo]'
        depth = ''
        if d['depths']:
            g = sorted(d['depths'])
            depth = (f"  block med={g[len(g) // 2]} "
                     f"p95={g[min(len(g) - 1, int(len(g) * 0.95))]} max={g[-1]}")
        ahead = f" ahead={d['ahead']}" if d['ahead'] else ''
        print(f"   from {name:<12} dup1={d['dup1']:<6}({dup1_rate:5.1f}/min) "
              f"resend={d['resend']:<6}({(d['resend'] / dur if dur else 0):5.1f}/min) "
              f"exp0={d['exp0']:<6}({(d['exp0'] / dur if dur else 0):5.1f}/min)"
              f"{ahead}{depth}{flag}")
    grand = sum(totals[k] for k in ('dup1', 'resend', 'exp0', 'ahead'))
    if grand:
        parts = "  ".join(f"{k} {totals[k]} ({100.0 * totals[k] / grand:.0f}%)"
                          for k in ('dup1', 'resend', 'exp0', 'ahead') if totals[k])
        print(f"   {grand} discards total: {parts}")
        print("   (none of these is loss — see the retransmit line below for that)")
        # `ahead` is a packet from the future, which the classification model
        # says should not appear at all (14 of 7,012 across the 2026-07 set).
        # If it stops being vanishingly rare, the model is wrong and every
        # number above it is suspect — say so rather than printing it quietly.
        ahead_pct = 100.0 * totals['ahead'] / grand
        if ahead_pct > 1.0:
            print(f"   ! ahead={totals['ahead']} is {ahead_pct:.1f}% of discards, "
                  "far above the ~0.2% baseline. The sequence model behind this "
                  "classification is probably wrong for this log — do not score "
                  "an A/B on the columns above.")
    print_ending(rep['ending'])
    print_session_events(rep['events'], rep['dur_min'], rep.get('ip_names'),
                         (rep.get('tmin'), rep.get('tmax')))
    print()


def print_ending(end):
    if end['kind'] == 'clean':
        print(f"   ended cleanly (return code {end['code']})")
    elif end['kind'] == 'running':
        print("   log copied while the game was still in the lobby — not a crash, "
              "but the session is incomplete")
    else:
        print("   ! ENDS ABRUPTLY: no 'Exiting Game With Return Code' line and the "
              "log stops mid-activity")
        print(f"     last line: {end['last']}")
        print("     no dump will exist unless the procdump path fired — check "
              "the bundle before assuming one was captured")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('logs', nargs='+', help='BZLogger.txt files to analyze')
    ap.add_argument('--launch', default=None,
                    help='restrict to the session whose lobby name contains this')
    ap.add_argument('--names', default=None,
                    help='steamid=Name,steamid=Name overrides')
    args = ap.parse_args()

    names = dict(DEFAULT_NAMES)
    if args.names:
        for pair in args.names.split(','):
            if '=' in pair:
                k, v = pair.split('=', 1)
                names[k.strip()] = v.strip()

    # Proxy logs first: their `session end: bytes=` is the client-side
    # real-bytes denominator for any BZLogger passed alongside them (the host
    # side gets `Actual Used` from its own log).  Pairing is by pid and time
    # window: each proxy session carries a pid and a span, and a BZLogger is
    # paired to the session whose window overlaps its match — never silently to
    # the largest (a proxy log spans the whole process and usually holds
    # several matches).
    proxy_sessions = []
    gov_start = 0
    found_any = False
    deferred = []
    for path in args.logs:
        try:
            with open(path, errors='replace') as fh:
                lines = fh.read().splitlines()
        except OSError as e:
            print(f"{path}: {e}", file=sys.stderr)
            continue

        # Proxy logs and BZLogger are told apart by content, not filename.
        config, last, at_end = parse_reorder_stats(lines)
        if config or last:
            # A V4.9+ proxy log (it carries a build stamp) with no session-end
            # lines at all means the process never reached teardown — a crash,
            # not an old proxy.
            crashed = (any('proxy build:' in l for l in lines)
                       and not any('session end:' in l for l in lines))
            print_reorder_report(path, config, at_end or last, crashed=crashed)
            sess, gs = proxy_log_health(path, lines)
            if sess:
                proxy_sessions.extend(sess)
            gov_start = gov_start or gs
            print()
            # A proxy log with a config line, sessions, or torn lines has told
            # us something useful even with no reorder_stats in it — which is
            # now the normal case, since the buffer ships off.
            found_any = found_any or bool(last) or bool(config) or bool(sess)
            continue
        deferred.append(path)

    for path in deferred:
        rep = analyze_file(path, args.launch, names)
        if proxy_sessions:
            rep['events']['proxy_sessions'] = proxy_sessions
        if gov_start:
            rep['events']['gov_start'] = gov_start
        if rep['stats'] or rep['events']['warps']:
            found_any = True
        print_report(rep)

    return 0 if found_any else 1


if __name__ == '__main__':
    sys.exit(main())
