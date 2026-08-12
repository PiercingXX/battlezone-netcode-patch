#!/usr/bin/env python3
"""Derive the BZ P2P header layout empirically from BZLogger's own ordinals.

Every previous answer for "where is the sequence number" was obtained by
scoring payload offsets for monotonicity, and every one of them was wrong:

    V3   offset 13, u16 LE   -> 99.5% "duplicates"
    V4.8 offset 16, u16 BE   -> 88.5% "duplicates"

Monotonicity scoring cannot distinguish a sequence number from an
acknowledgement number, a clock, or a length, and it silently mixes packet
classes that do not share a layout.  This tool does not score anything.  It
uses ground truth.

BZLogger prints its retransmits as

    BZRNet P2P TRY Sent Packet (i,n) to IP:PORT: <full payload hex>

so for 65,806 packets in the committed capture we know two header values
*and* the bytes they were encoded into.  Brute-forcing (offset, width,
endian) against a known ordinal either matches every single sample or it does
not; there is no scoring, no threshold and no judgement call.  Two further
formats carry ordinals too and act as independent controls:

    BZRNet P2P CON Sent Packet (0,0) to IP:PORT: <hex>
    BZRNet P2P Sent Packet to IP:PORT: <hex>        (implicitly (0,0))

The answer, at 100% over every sample in every committed log:

    offset  size  field
      0       1   flags   0x40 base, |0x80 = TRY (this is a retransmit)
      1       1   class   0x00 game data, 0x02 master-server probe,
                          0x03 CON/connect, 0x06 and 0x07 18-byte control
      2       3   0x000001  protocol version
      5       5   u40 BE millisecond clock
     10       4   u32 BE  SEQUENCE  - the sender's own message counter ("i")
     14       4   u32 BE  ACK       - highest sequence seen from the peer ("n")
     18      ..   body

`--verify-clock` checks the u40 field against BZLogger's own wall-clock
timestamps, which is what promotes the layout from "matches the ordinals" to
"every field is accounted for".  It agrees to the millisecond.

Usage:
  seq_crossmatch.py BZLOGGER.txt [--verify-clock] [--max-offset N]
"""
import argparse
import re
import sys
from collections import Counter
from datetime import datetime

# `(i,n)` forms carry both ordinals; the bare `Sent Packet to` form is only
# ever emitted for unsequenced traffic, where both ordinals are zero.
ORDINAL_RE = re.compile(
    r'BZRNet \S+ (?:TRY |CON )?Sent Packet \((\d+),(\d+)\) to '
    r'(\d+\.\d+\.\d+\.\d+):(\d+): ([0-9a-fA-F]+)')
BARE_RE = re.compile(
    r'BZRNet \S+ Sent Packet to (\d+\.\d+\.\d+\.\d+):(\d+): ([0-9a-fA-F]+)')
TS_RE = re.compile(r'^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)')

# Candidate encodings.  Widths past 4 exist because the clock field is 5 bytes;
# nothing in this protocol is wider.
WIDTHS = (1, 2, 4, 5)
ENDIANS = ('big', 'little')


def load(path):
    """Yield (timestamp|None, i, n, peer, payload) for every logged packet."""
    rows = []
    with open(path, errors='replace') as fh:
        for line in fh:
            m = ORDINAL_RE.search(line)
            if m:
                i, n, ip, port, hexs = m.groups()
                # A crash-cut final line leaves an odd-length hex payload;
                # the truncated bytes are unusable for offset solving anyway.
                if len(hexs) % 2:
                    continue
                rows.append((_ts(line), int(i), int(n),
                             f'{ip}:{port}', bytes.fromhex(hexs)))
                continue
            m = BARE_RE.search(line)
            if m:
                ip, port, hexs = m.groups()
                if len(hexs) % 2:
                    continue
                rows.append((_ts(line), 0, 0,
                             f'{ip}:{port}', bytes.fromhex(hexs)))
    return rows


def _ts(line):
    m = TS_RE.match(line)
    if not m:
        return None
    try:
        return datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f')
    except ValueError:
        return None


def solve(rows, which, max_offset):
    """Every (offset,width,endian) that reproduces the ordinal on every sample.

    An exact solution is demanded deliberately.  A field that matches 99% of
    samples is not the field; it is a coincidence that will move under you the
    first time the value grows past a byte boundary, which is precisely how
    offset 13 survived from V3 to V4.8.
    """
    exact = []
    for width in WIDTHS:
        for endian in ENDIANS:
            for off in range(0, max_offset - width + 1):
                hit = tot = 0
                for _, i, n, _peer, p in rows:
                    if off + width > len(p):
                        continue
                    tot += 1
                    if int.from_bytes(p[off:off + width], endian) == (i if which == 'i' else n):
                        hit += 1
                    else:
                        break          # one miss disqualifies it; stop early
                if tot and hit == tot:
                    exact.append((off, width, endian, tot))
    return exact


def verify_clock(rows, off=5, width=5, endian='big'):
    """Does the u40 field advance in step with BZLogger's wall clock?"""
    pairs = [(t, int.from_bytes(p[off:off + width], endian))
             for t, _i, _n, _peer, p in rows
             if t is not None and off + width <= len(p)]
    if len(pairs) < 2:
        return None
    errs = []
    base_t, base_v = pairs[0]
    for t, v in pairs[1:]:
        wall_ms = (t - base_t).total_seconds() * 1000.0
        errs.append(abs((v - base_v) - wall_ms))
    errs.sort()
    return {
        'samples': len(errs),
        'median_err_ms': errs[len(errs) // 2],
        'p99_err_ms': errs[int(len(errs) * 0.99)],
        'max_err_ms': errs[-1],
    }


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('bzlogger', nargs='+')
    ap.add_argument('--verify-clock', action='store_true',
                    help='cross-check the u40 field against wall-clock timestamps')
    ap.add_argument('--max-offset', type=int, default=32,
                    help='highest header offset to consider (default 32)')
    args = ap.parse_args()

    rows = []
    per_file = []
    for path in args.bzlogger:
        try:
            got = load(path)
        except OSError as e:
            print(f'{path}: {e}', file=sys.stderr)
            return 1
        print(f'{path}: {len(got)} packets with a known ordinal')
        per_file.append((path, got))
        rows.extend(got)

    if not rows:
        print('no `Sent Packet` lines with payload hex found — this log has no '
              'ground truth to match against', file=sys.stderr)
        return 1

    print(f'\ntotal ground-truth samples: {len(rows)}')
    classes = Counter((p[0], p[1]) for _t, _i, _n, _peer, p in rows if len(p) > 1)
    print('packet classes seen (flags/class):')
    for (flags, cls), n in classes.most_common():
        tag = ' TRY' if flags & 0x80 else ''
        print(f'   0x{flags:02x}/0x{cls:02x}{tag:<4} {n}')

    rc = 0
    for which, label in (('i', 'first ordinal  "i"  (SEQUENCE)'),
                         ('n', 'second ordinal "n"  (ACK)')):
        print(f'\n--- {label} ---')
        sols = solve(rows, which, args.max_offset)
        if not sols:
            print('   NO exact solution — the header layout is not what this '
                  'tool assumes, or the log mixes protocol versions')
            rc = 1
            continue
        for off, width, endian, n in sols:
            print(f'   offset={off:<3} width={width} {endian:<6} '
                  f'exact on all {n} samples')
        # The widest exact solution is the true field: a narrow one only
        # matches because the high bytes happen to be zero at these values.
        off, width, endian, _n = max(sols, key=lambda s: (s[1], -s[0]))
        print(f'   => field is u{width * 8} {endian}-endian at offset {off}')

    if args.verify_clock:
        # Per file: two logs from different machines are three hours apart in
        # this dataset, so pooling them would only measure the testers' clock
        # skew.  See the clock-offset item in todo.md.
        print('\n--- u40 BE clock at offset 5 vs BZLogger wall time ---')
        for path, got in per_file:
            res = verify_clock(got)
            if not res:
                print(f'   {path}: not enough timestamped samples')
                continue
            verdict = ('millisecond clock' if res['median_err_ms'] < 5
                       else 'DOES NOT track wall time at millisecond scale')
            print(f"   {path}: {res['samples']} samples  median err "
                  f"{res['median_err_ms']:.1f} ms  p99 {res['p99_err_ms']:.1f} ms "
                  f"-> {verdict}")
    return rc


if __name__ == '__main__':
    sys.exit(main())
