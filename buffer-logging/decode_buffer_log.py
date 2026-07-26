#!/usr/bin/env python3
"""Offline decoder for bz_buffer_log.bin (the proxy's binary receive capture).

The buffer log exists to answer questions the text proxy log cannot.  As of
2026-07-26 the most urgent one is this: a full 2.5 h session produced *no*
`reorder_stats:` line at all, and no `session end: reorder_stats:` either.  That
line is gated on `s == g_reorder_sock`, which is assigned in exactly one place —
the moment a packet is buffered — so the reorder buffer handled zero packets.

The reorder path lives only in `hooked_WSARecvFrom`, and it bypasses whenever
`overlapped != nullptr`.  `hooked_recvfrom` has no reorder path at all.  So the
buffer is dead code if the game receives by either of those routes, and this
decoder's first job is to say which route the game actually uses:

  * only `recvfrom` records  -> the reorder path can never run as written; it
    has to be ported into `hooked_recvfrom`.
  * `WSARecvFrom` records but still no reorder_stats -> the receives are
    overlapped (IOCP), and only the BZ_IOCP_REORDER path can help.

Its second job is to size the thing.  `--seq-scan` finds the sequence field in
the payload prefix by scoring every byte offset for per-peer monotonicity, which
independently checks the proxy's hardcoded `seq_offset`.  With an offset known,
the per-peer reorder report measures how *late* an out-of-order packet actually
was — the number `BZ_REORDER_MAX_HOLD_MS` should be set from, rather than the
current guessed 100 ms.

Record layout is `BufferLogRecordHeader` in the proxy sources, `#pragma pack(1)`:
52-byte header followed by `payload_bytes` of payload prefix.  The stride comes
from bz_buffer_log.meta.txt when it is present and is otherwise recovered by
scanning for the record magic.

Usage:
  decode_buffer_log.py BINFILE [--meta META] [--sid N] [--seq-scan]
                               [--seq-offset N --seq-width {2,4} --seq-endian {le,be}]
                               [--limit N] [--dump N]
"""
import argparse
import struct
import sys
from collections import Counter, defaultdict

MAGIC = 0x474C5A42          # 'BZLG'
HDR = struct.Struct('<IIIIQIIIIIHHHH')
HDR_SIZE = HDR.size          # 52

EVENT_NAMES = {
    1: 'recvfrom',           # synchronous; NO reorder path in this hook
    2: 'WSARecvFrom',        # reorder path lives here, bypassed when overlapped
    3: 'ioctlsocket',
    4: 'WSAIoctl',
}
# The reorder buffer only ever sees traffic arriving through event type 2.
REORDERABLE_EVENT = 2
RECV_EVENTS = (1, 2)

FIONBIO = 0x8004667E         # ioctlsocket opcode that sets non-blocking mode


def read_meta(path):
    """Parse bz_buffer_log.meta.txt into a dict.

    The Linux proxy writes literal backslash-r-n instead of real CRLF (a
    escaping bug in dsound_proxy.cpp), so split on both.
    """
    try:
        with open(path, errors='replace') as fh:
            text = fh.read()
    except OSError:
        return {}
    text = text.replace('\\r\\n', '\n').replace('\\n', '\n').replace('\r\n', '\n')
    out = {}
    for line in text.split('\n'):
        if '=' in line:
            k, v = line.split('=', 1)
            out[k.strip()] = v.strip()
    return out


def detect_stride(blob, meta):
    """Record stride: from meta if sane, else recovered from magic spacing."""
    try:
        stride = int(meta.get('record_stride', 0))
        if stride > HDR_SIZE and len(blob) % stride == 0:
            return stride, 'meta'
    except ValueError:
        pass
    # Recover it: find the offset of the second record by looking for the magic.
    first = blob.find(struct.pack('<I', MAGIC))
    if first < 0:
        return 0, 'none'
    nxt = blob.find(struct.pack('<I', MAGIC), first + 4)
    if nxt < 0:
        return 0, 'none'
    return nxt - first, 'scanned'


def parse(blob, stride, sid_filter, limit):
    """Yield decoded records.  Records failing the magic check are counted."""
    recs, bad = [], 0
    payload_bytes = stride - HDR_SIZE
    for off in range(0, len(blob) - stride + 1, stride):
        (magic, ver, etype, sid, tick, seq, req, xfer,
         wsa, ip, port, flags, plen, _rsv) = HDR.unpack_from(blob, off)
        if magic != MAGIC:
            bad += 1
            continue
        if sid_filter is not None and sid != sid_filter:
            continue
        plen = min(plen, payload_bytes)
        recs.append({
            'ver': ver, 'etype': etype, 'sid': sid, 'tick': tick, 'seq': seq,
            'req': req, 'xfer': xfer, 'wsa': wsa,
            'ip': '%d.%d.%d.%d' % (ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, (ip >> 24) & 255),
            'port': port, 'flags': flags, 'plen': plen,
            'payload': blob[off + HDR_SIZE:off + HDR_SIZE + plen],
        })
        if limit and len(recs) >= limit:
            break
    return recs, bad


def peer_key(r):
    return f"{r['ip']}:{r['port']}"


def read_field(buf, off, width, endian):
    if off + width > len(buf):
        return None
    raw = buf[off:off + width]
    return int.from_bytes(raw, 'little' if endian == 'le' else 'big')


def score_offset(by_peer, off, width, endian):
    """Fraction of consecutive same-peer packets whose field steps forward a little.

    A real sequence counter advances by a small positive amount almost every
    packet.  Length fields, checksums and timestamps do not.
    """
    good = total = 0
    for pkts in by_peer.values():
        prev = None
        for p in pkts:
            v = read_field(p['payload'], off, width, endian)
            if v is None:
                continue
            if prev is not None:
                d = v - prev
                total += 1
                if 1 <= d <= 64:
                    good += 1
            prev = v
    return (good / total if total else 0.0), total


def seq_scan(by_peer, payload_bytes):
    print("  sequence-field scan (higher score = looks more like a counter)")
    results = []
    for width in (2, 4):
        for endian in ('le', 'be'):
            for off in range(0, payload_bytes - width + 1):
                s, n = score_offset(by_peer, off, width, endian)
                if n >= 20:
                    results.append((s, off, width, endian, n))
    results.sort(reverse=True)
    if not results:
        print("    not enough same-peer packet pairs to score")
        return None
    for s, off, width, endian, n in results[:8]:
        print(f"    offset={off:<3} width={width} {endian}  score={s:.3f}  ({n} pairs)")
    best = results[0]
    print(f"  -> best guess: seq_offset={best[1]} width={best[2]} endian={best[3]} "
          f"(proxy currently hardcodes seq_offset=13)")
    return best


def reorder_report(by_peer, off, width, endian):
    """Classify arrivals and measure how late the out-of-order ones were.

    `late_ms` is measured from the moment the gap *opened* — the arrival of the
    packet that skipped over this sequence number — to the straggler finally
    showing up.  That interval is exactly how long a reorder buffer would have
    had to hold the stream to put it back in order, so it is the number
    BZ_REORDER_MAX_HOLD_MS should be set from.

    Measuring instead from the last packet seen would understate it badly: on a
    stream sending every 20 ms, a straggler 200 ms late still arrives within
    20 ms of *some* packet.
    """
    print(f"  reorder analysis @ offset={off} width={width} {endian}")
    modulus = 1 << (width * 8)
    half = modulus // 2
    all_late = []
    never_arrived = 0
    for peer, pkts in sorted(by_peer.items(), key=lambda kv: -len(kv[1])):
        highest = None            # highest seq seen so far
        in_order = dup = ooo = jump = 0
        late = []
        missing = {}              # seq -> tick at which the gap opened
        seen = set()
        for p in pkts:
            v = read_field(p['payload'], off, width, endian)
            if v is None:
                continue
            if highest is None:
                highest = v
                in_order += 1
                seen.add(v)
                continue
            # Signed delta, wrap-aware.
            d = ((v - highest + half) % modulus) - half
            if d == 1:
                in_order += 1
            elif d <= 0:
                if v in missing:
                    ooo += 1
                    late.append(p['tick'] - missing.pop(v))
                elif v in seen:
                    dup += 1
                else:
                    ooo += 1      # older than anything tracked; lateness unknown
            else:
                # Forward jump: every sequence it skipped is now a gap that a
                # reorder buffer would be holding the stream open for.
                jump += 1
                for miss in range(1, min(d, 4096)):
                    missing.setdefault((highest + miss) % modulus, p['tick'])
            if d > 0:
                highest = v
            seen.add(v)
        tot = in_order + dup + ooo + jump
        if tot < 10:
            continue
        all_late.extend(late)
        never_arrived += len(missing)
        pct = lambda x: 100.0 * x / tot
        print(f"    {peer:<22} n={tot:<7} in_order={pct(in_order):5.1f}%  "
              f"fwd_gap={pct(jump):5.1f}%  out_of_order={pct(ooo):5.1f}%  dup={pct(dup):5.1f}%")
        if late:
            late.sort()
            print(f"      {'':20} straggler lateness ms: med={late[len(late)//2]} "
                  f"p90={late[int(len(late)*0.9)]} p99={late[int(len(late)*0.99)]} max={late[-1]}")
        if missing:
            print(f"      {'':20} {len(missing)} sequence numbers never arrived "
                  f"(genuine loss — no buffer recovers these)")
    if all_late:
        all_late.sort()
        n = len(all_late)
        p95 = all_late[int(n * 0.95)]
        print(f"\n  ALL PEERS: {n} recoverable out-of-order arrivals, lateness "
              f"med={all_late[n//2]}ms p95={p95}ms p99={all_late[int(n*0.99)]}ms max={all_late[-1]}ms")
        print(f"  -> a hold window of {p95} ms would reorder 95% of them; "
              f"BZ_REORDER_MAX_HOLD_MS is currently 100")
        if never_arrived:
            print(f"  -> {never_arrived} sequence numbers never arrived at all: that share is "
                  f"loss, not reordering, and no hold window recovers it")
    else:
        print("\n  no out-of-order arrivals seen — nothing for the reorder buffer to fix")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('binfile')
    ap.add_argument('--meta', default=None, help='bz_buffer_log.meta.txt (default: alongside binfile)')
    ap.add_argument('--sid', type=int, default=None, help='restrict to one socket id')
    ap.add_argument('--seq-scan', action='store_true', help='search for the sequence field')
    ap.add_argument('--seq-offset', type=int, default=None)
    ap.add_argument('--seq-width', type=int, choices=(2, 4), default=4)
    ap.add_argument('--seq-endian', choices=('le', 'be'), default='le')
    ap.add_argument('--limit', type=int, default=0, help='stop after N records')
    ap.add_argument('--dump', type=int, default=0, help='hex-dump the first N records')
    args = ap.parse_args()

    try:
        with open(args.binfile, 'rb') as fh:
            blob = fh.read()
    except OSError as e:
        print(f"{args.binfile}: {e}", file=sys.stderr)
        return 1
    if not blob:
        print(f"{args.binfile}: empty — the game did not flush the ring "
              f"(unclean exit, or BZ_BUFFER_LOG was not set)", file=sys.stderr)
        return 1

    meta_path = args.meta or (args.binfile.rsplit('.bin', 1)[0] + '.meta.txt')
    meta = read_meta(meta_path)
    stride, how = detect_stride(blob, meta)
    if stride <= HDR_SIZE:
        print(f"{args.binfile}: cannot determine record stride (no magic found)", file=sys.stderr)
        return 1
    payload_bytes = stride - HDR_SIZE

    recs, bad = parse(blob, stride, args.sid, args.limit)
    print(f"{args.binfile}")
    print(f"  stride={stride} ({how})  payload_prefix={payload_bytes}B  "
          f"records={len(recs)}  unreadable={bad}")
    if meta:
        seen = meta.get('total_events_seen')
        wrote = meta.get('records_written')
        if seen and wrote and seen != wrote:
            print(f"  ring wrapped: {seen} events seen, only the last {wrote} kept "
                  f"(raise BZ_BUFFER_LOG_RING)")
    if not recs:
        print("  no readable records")
        return 1

    # ── The headline question: which receive API does the game actually use? ──
    kinds = Counter(r['etype'] for r in recs)
    print("\n  RECEIVE PATH")
    for et, n in kinds.most_common():
        name = EVENT_NAMES.get(et, f'type{et}')
        print(f"    {name:<14} {n:>8}  ({100.0*n/len(recs):5.1f}%)")
    n_reorderable = kinds.get(REORDERABLE_EVENT, 0)
    n_recv = sum(kinds.get(e, 0) for e in RECV_EVENTS)
    if n_recv == 0:
        print("    no receive events captured at all")
    elif n_reorderable == 0:
        print("\n    => The game receives entirely via recvfrom(). The reorder buffer is")
        print("       implemented only in hooked_WSARecvFrom, so it can never run as")
        print("       written. It must be ported into hooked_recvfrom.")
    elif n_reorderable == n_recv:
        print("\n    => All receives go through WSARecvFrom. If reorder_stats is still")
        print("       absent from the proxy log, those calls are overlapped (IOCP) and")
        print("       are hitting the `overlapped != nullptr` bypass.")
    else:
        print(f"\n    => Mixed: {100.0*n_reorderable/n_recv:.1f}% of receives can reach the")
        print("       reorder path; the rest bypass it via recvfrom().")

    # ── Blocking vs non-blocking ────────────────────────────────────────────
    modes = [r for r in recs if r['etype'] in (3, 4)]
    if modes:
        print("\n  SOCKET MODE")
        for r in modes[:10]:
            op = r['req']
            tag = ' (FIONBIO: non-blocking)' if op == FIONBIO else ''
            print(f"    sid={r['sid']} {EVENT_NAMES.get(r['etype'])} opcode=0x{op:08x} "
                  f"arg={r['xfer']} rc_err={r['wsa']}{tag}")

    # ── Traffic shape ───────────────────────────────────────────────────────
    data = [r for r in recs if r['etype'] in RECV_EVENTS and r['plen'] > 0]
    by_peer = defaultdict(list)
    for r in data:
        by_peer[peer_key(r)].append(r)
    if by_peer:
        print("\n  PEERS")
        span_ms = max(r['tick'] for r in data) - min(r['tick'] for r in data)
        span_min = span_ms / 60000.0 if span_ms else 0
        for peer, pkts in sorted(by_peer.items(), key=lambda kv: -len(kv[1])):
            sizes = sorted(p['xfer'] for p in pkts)
            rate = (len(pkts) / span_min) if span_min else 0
            print(f"    {peer:<22} packets={len(pkts):<7} ({rate:7.1f}/min)  "
                  f"bytes med={sizes[len(sizes)//2]} max={sizes[-1]}")
        if span_min:
            print(f"    capture span {span_min:.1f} min")

    if args.dump:
        print("\n  FIRST RECORDS")
        for r in recs[:args.dump]:
            print(f"    t={r['tick']} {EVENT_NAMES.get(r['etype'],r['etype']):<12} "
                  f"{peer_key(r):<22} len={r['xfer']:<5} {r['payload'].hex()}")

    if args.seq_scan and by_peer:
        print()
        best = seq_scan(by_peer, payload_bytes)
        if best and args.seq_offset is None:
            print()
            reorder_report(by_peer, best[1], best[2], best[3])
    elif args.seq_offset is not None and by_peer:
        print()
        reorder_report(by_peer, args.seq_offset, args.seq_width, args.seq_endian)

    return 0


if __name__ == '__main__':
    sys.exit(main())
