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

None of these four is a loss count.  The real loss signal in BZLogger is
"BZRNet P2P TRY Sent N to <ip>" — the sender retransmitting unacked data —
and it is reported per peer below, normalised per MB so it stays comparable
across bandwidth changes.

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
LAUNCH_RE = re.compile(r'Launching Network Game (.+?), Map (\S+)')
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
# The governor announcing its current send budget.
BANDWIDTH_RE = re.compile(r'Net: Bandwidth usage now set to (\d+), Interval (\d+) ms')
# Host-side auto-kick machinery (see the AutoKick* entries in shared/net_globals.h).
LAG_RE  = re.compile(r'Chat Message: (.+?) is lagging')
UNLAG_RE = re.compile(r'Chat Message: (.+?) stopped lagging')
KICK_RE = re.compile(r'Auto kicking player (.+?) due to (.+)')
# A second drop class, distinct from the Type 0 stale discards.
UNKNOWN_SRC_RE = re.compile(r"Dropping Packet \(Source IP doesn't match any known player\)")
# The sender retransmitting unacked reliable data.  This, not the Type 0
# discards, is the log's actual loss signal.
TRY_SENT_RE = re.compile(r'BZRNet P2P TRY Sent (\d+) to ([\d.]+):(\d+)')

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
    """Warp, governor, lag and kick events from a BZLogger slice."""
    warps = []          # (timestamp, magnitude in metres)
    bandwidth = []      # (timestamp, bytes/sec, interval ms)
    lag = []            # (timestamp, kind, who)
    unknown_src = 0
    retransmit = defaultdict(int)   # peer ip -> count

    for ln in lines:
        m = WARP_RE.match(ln)
        if m:
            x, y, z = float(m.group(2)), float(m.group(3)), float(m.group(4))
            warps.append((parse_ts(m.group(1)), math.sqrt(x * x + y * y + z * z)))
            continue
        m = BANDWIDTH_RE.search(ln)
        if m:
            ts = ln.split(' ', 2)[:2]
            bandwidth.append((' '.join(ts), int(m.group(1)), int(m.group(2))))
            continue
        for rx, kind in ((LAG_RE, 'lagging'), (UNLAG_RE, 'recovered'), (KICK_RE, 'KICKED')):
            m = rx.search(ln)
            if m:
                lag.append((ln.split(' ', 1)[0], kind, m.group(1)))
                break
        m = TRY_SENT_RE.search(ln)
        if m:
            retransmit[m.group(2)] += 1
            continue
        if UNKNOWN_SRC_RE.search(ln):
            unknown_src += 1

    return {'warps': warps, 'bandwidth': bandwidth, 'lag': lag,
            'unknown_src': unknown_src, 'retransmit': retransmit}


def print_session_events(ev, dur_min):
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
    if bw:
        rates = [r for _, r, _ in bw]
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
        print(f"   governor: {len(rates)} adjustments, {min(rates)} -> {max(rates)} B/s"
              f", median {sorted(rates)[len(rates) // 2]}{note}")
        # How long the opening trickle lasted: the ramp, not the ceiling, is
        # what a short match actually lives with.
        for target in (40000, 80000):
            if max(rates) >= target:
                t0 = bw[0][0]
                hit = next(t for t, r, _ in bw if r >= target)
                mins = (parse_ts(hit) - parse_ts(t0)).total_seconds() / 60.0
                print(f"   reached {target} B/s after {mins:.1f} min")

    rt = ev.get('retransmit')
    if rt:
        total = sum(rt.values())
        rate = (total / dur_min) if dur_min else 0.0
        detail = ", ".join(f"{ip}={n}" for ip, n in
                           sorted(rt.items(), key=lambda kv: -kv[1])[:6])
        print(f"   retransmits (TRY Sent): {total} = {rate:.1f}/min   {detail}")
        # Normalised, so a bandwidth change does not masquerade as a loss change.
        if bw:
            med_bps = sorted(r for _, r, _ in bw)[len(bw) // 2]
            mb_per_min = med_bps * 60.0 / 1e6
            if mb_per_min > 0:
                print(f"   retransmits per MB sent: {rate / mb_per_min:.1f}"
                      "   <-- compare A/Bs on this, not the raw rate")

    for ts, kind, who in ev['lag']:
        mark = "!!" if kind == 'KICKED' else " -"
        print(f"   {mark} {ts} {who}: {kind}")

    if ev['unknown_src']:
        print(f"   dropped {ev['unknown_src']} packets from unrecognised source IPs")


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


def print_reorder_report(path, config, stats):
    print(f"{path}   [proxy reorder counters]")
    if config:
        state = 'enabled' if config['enabled'] else 'DISABLED'
        hold = (f"max_hold={config['max_hold_ms']}ms" if config['max_hold_ms'] is not None
                else "max_hold=n/a (pre-V4.7)")
        print(f"   config: {state}  window={config['window_ms']}ms "
              f"floor={config['min_ms']}ms {hold}")
    if not stats:
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
    print(f"   dropped: stale={stats.get('stale', 0)} dup={stats.get('dup', 0)} "
          f"reclaim={stats.get('reclaim', 0)}   "
          f"bypass: short={stats.get('short', 0)} tblfull={stats.get('tblfull', 0)}")

    # The numbers that say what the patch cost, not just what it caught.
    hold_avg = stats.get('avg', 0)
    hold_max = stats.get('max', 0)
    print(f"   ADDED LATENCY: hold_ms avg={hold_avg} max={hold_max}")
    notes = []
    if stats.get('evicted', 0):
        notes.append(f"{stats['evicted']} packets released out of order under "
                     "storage pressure (raise BZ_REORDER_DEPTH)")
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

    # Drops can be sparse; warps usually bracket the session better.
    if events['warps']:
        wt = [t for t, _ in events['warps']]
        tmin = min(wt) if tmin is None else min(tmin, min(wt))
        tmax = max(wt) if tmax is None else max(tmax, max(wt))

    dur_min = ((tmax - tmin).total_seconds() / 60.0) if tmin and tmax else 0.0
    return {
        'path': path, 'lobby': lobby, 'map': mapname,
        'dur_min': dur_min, 'stats': stats, 'names': names,
        'events': events,
    }


def print_report(rep):
    names = rep['names']
    hdr = f"{rep['path']}"
    if rep['lobby']:
        hdr += f"   lobby={rep['lobby']} map={rep['map']} dur={rep['dur_min']:.1f}min"
    print(hdr)
    if not rep['stats']:
        print("   (no Type 0 drop lines in this session)")
        print_session_events(rep['events'], rep['dur_min'])
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
        print("   (none of these is loss — see TRY Sent below for that)")
    print_session_events(rep['events'], rep['dur_min'])
    print()


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

    found_any = False
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
            print_reorder_report(path, config, at_end or last)
            found_any = found_any or bool(last)
            continue

        rep = analyze_file(path, args.launch, names)
        if rep['stats'] or rep['events']['warps']:
            found_any = True
        print_report(rep)

    return 0 if found_any else 1


if __name__ == '__main__':
    sys.exit(main())
