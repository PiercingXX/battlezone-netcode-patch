#!/usr/bin/env python3
"""Per-link BZRNet drop analysis for BZ_SEND_DUP A/B testing.

BZRNet logs every rejected packet as:
  BZRNet P2P Dropping Packet Type 0 For Client <steamid> (Packet #R received, #E expected)

Raw counts are meaningless once duplication is in play, so this classifies each
drop per sender:
  echo  (E-R == 1): discard of an intentional duplicate / engine resend. Harmless.
  real  (E-R >= 2): a genuine stale / out-of-order arrival. The number that matters.

When the sender was duplicating, each real stale packet is logged twice (original
+ copy), so real counts are also reported halved ("uniq").

Usage:
  analyze_drops.py LOG [LOG ...] [--launch SUBSTR] [--names steamid=Name,...]

  --launch SUBSTR  Restrict to the session whose "Launching Network Game" line
                   contains SUBSTR (default: the last launch in each file).
  --names          Comma-separated steamid=Name map for readable output.
                   A built-in map covers the 2026-07 test crew.

Exit status is 0 on success, 1 if no drop lines were found in any file.
"""
import argparse
import re
import sys
from collections import defaultdict
from datetime import datetime

DROP_RE = re.compile(
    r'^([\d\-]+ [\d:.]+) BZRNet P2P Dropping Packet Type 0 For Client '
    r'(S\d+) \(Packet #(\d+) received, #(\d+) expected\)'
)
LAUNCH_RE = re.compile(r'Launching Network Game (\S+), Map (\S+)')
START_RE = re.compile(r'Starting BattleZone 98 Redux')

DEFAULT_NAMES = {
    'S76561198884003346': 'PiercingXX',
    'S76561198094230200': 'KFK',
    'S76561199559935298': 'Bison',
    'S76561199732480793': 'Monkey',
}


def parse_ts(s):
    return datetime.strptime(s[:19], '%Y-%m-%d %H:%M:%S')


def session_slice(lines, launch_substr):
    """Return (start_idx, lobby, map) for the chosen session.

    A session runs from its 'Launching Network Game' line to the next 'Starting
    BattleZone' (a relaunch) or end of file.  Without --launch, the last launch
    in the file is used.
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
    end_idx = len(lines)
    for j in range(start_idx + 1, len(lines)):
        if START_RE.search(lines[j]):
            end_idx = j
            break
    return start_idx, end_idx, chosen[1], chosen[2]


def analyze_file(path, launch_substr, names):
    with open(path, errors='replace') as fh:
        lines = fh.read().splitlines()
    start_idx, end_idx, lobby, mapname = session_slice(lines, launch_substr)

    stats = defaultdict(lambda: {'echo': 0, 'real': 0})
    tmin = tmax = None
    for ln in lines[start_idx:end_idx]:
        m = DROP_RE.match(ln)
        if not m:
            continue
        cid, r, e = m.group(2), int(m.group(3)), int(m.group(4))
        key = 'echo' if (e - r) == 1 else 'real'
        stats[cid][key] += 1
        t = parse_ts(m.group(1))
        tmin = t if tmin is None else tmin
        tmax = t
    dur_min = ((tmax - tmin).total_seconds() / 60.0) if tmin and tmax else 0.0
    return {
        'path': path, 'lobby': lobby, 'map': mapname,
        'dur_min': dur_min, 'stats': stats, 'names': names,
    }


def print_report(rep):
    names = rep['names']
    hdr = f"{rep['path']}"
    if rep['lobby']:
        hdr += f"   lobby={rep['lobby']} map={rep['map']} dur={rep['dur_min']:.1f}min"
    print(hdr)
    if not rep['stats']:
        print("   (no drop lines in this session)\n")
        return
    rows = sorted(rep['stats'].items(), key=lambda kv: -kv[1]['real'])
    dur = rep['dur_min'] or 0
    for cid, d in rows:
        name = names.get(cid, cid)
        uniq = d['real'] // 2 if d['echo'] > 50 else d['real']
        rate = (uniq / dur) if dur else 0
        dupflag = '  [sender dup ON]' if d['echo'] > 50 else ''
        selfflag = '  [loopback self-echo]' if name and d['real'] == 0 and d['echo'] else ''
        print(f"   from {name:<12} echo={d['echo']:<6} real={d['real']:<6} "
              f"uniq~{uniq:<5} {rate:5.1f}/min{dupflag}{selfflag}")
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
            rep = analyze_file(path, args.launch, names)
        except OSError as e:
            print(f"{path}: {e}", file=sys.stderr)
            continue
        if rep['stats']:
            found_any = True
        print_report(rep)

    return 0 if found_any else 1


if __name__ == '__main__':
    sys.exit(main())
