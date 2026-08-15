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

Its second job is to measure delivery.  That needs the BZ P2P header, which as
of V4.9 is known exactly rather than guessed — `tools/seq_crossmatch.py` derives
it from BZLogger's own logged ordinals and matches 100% of 65,860 samples:

    offset  size  field
      0       1   flags   0x40 base, |0x80 = TRY (this datagram is a retransmit)
      1       1   class   0x00 game data, 0x02 master-server probe,
                          0x03 CON/connect, 0x04, 0x05, 0x06, 0x07 control
      2       3   0x000001  protocol version
      5       5   u40 BE millisecond clock (verified against wall time)
     10       4   u32 BE  SEQUENCE - the sender's own message counter
     14       4   u32 BE  ACK      - highest sequence seen from the peer
     18      ..   body

Two consequences that invalidate every delivery number published before V4.9:

  * V4.8 measured "loss" and "duplication" at offset 16 u16 BE.  That is the
    low half of the ACK field.  An acknowledgement repeats by design, which is
    the entire explanation for its "88.5% duplicate rate".
  * The SEQUENCE field is a *message* counter, not a per-datagram counter.  It
    is stamped on every datagram the sender emits, so one sequence value
    legitimately appears on dozens of datagrams.  Counting repeats as network
    duplicates is meaningless; this decoder reports them as `advert` and
    measures delivery over distinct sequence values instead.

`--seq-scan` remains for reverse-engineering an unknown build, but it is a
weak instrument — monotonicity scoring cannot tell a sequence from an ack, and
it picked the wrong field twice.  Prefer seq_crossmatch.py.

Record layout is `BufferLogRecordHeader` in the proxy sources, `#pragma pack(1)`:
52-byte header followed by `payload_bytes` of payload prefix.  The stride comes
from bz_buffer_log.meta.txt when it is present and is otherwise recovered by
scanning for the record magic.

Usage:
  decode_buffer_log.py BINFILE [--meta META] [--sid N] [--seq-scan]
                               [--seq-offset N --seq-width {1,2,4} --seq-endian {le,be}]
                               [--class HEX] [--limit N] [--dump N]
"""
import argparse
import struct
import sys
from collections import Counter, defaultdict

MAGIC = 0x474C5A42          # 'BZLG'
HDR = struct.Struct('<IIIIQIIIIIHHHH')
HDR_SIZE = HDR.size          # 52

# The BZ P2P header, settled by tools/seq_crossmatch.py (V4.9).
BZ_FLAG_OFF = 0
BZ_CLASS_OFF = 1
BZ_CLOCK_OFF, BZ_CLOCK_WIDTH = 5, 5
BZ_SEQ_OFF, BZ_SEQ_WIDTH, BZ_SEQ_ENDIAN = 10, 4, 'be'
BZ_ACK_OFF, BZ_ACK_WIDTH = 14, 4
BZ_HEADER_BYTES = 18         # smallest payload that carries a full header
BZ_FLAG_TRY = 0x80           # this datagram is a retransmit

CLASS_NAMES = {
    0x00: 'game data',
    0x02: 'master probe',
    0x03: 'CON/connect',
    0x04: 'control',
    0x05: 'control',
    0x06: 'control',
    0x07: 'control',
}

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

    Older Linux proxies wrote literal backslash-r-n instead of real CRLF, so
    split on both.
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


def bz_class(payload):
    """(flags, class) of a datagram, or None if it is too short to have a header."""
    if len(payload) < BZ_HEADER_BYTES:
        return None
    return payload[BZ_FLAG_OFF], payload[BZ_CLASS_OFF]


def score_offset(by_peer, off, width, endian):
    """Fraction of consecutive same-peer packets whose field steps forward a little.

    A real sequence counter advances by a small positive amount almost every
    packet.  Length fields, checksums and timestamps do not.

    This heuristic is retained only for reverse-engineering an unknown build.
    It cannot distinguish a sequence from an acknowledgement, and it chose the
    wrong field in both V3 and V4.8.  seq_crossmatch.py is the authority.
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
    print("  sequence-field scan (heuristic — see tools/seq_crossmatch.py for the")
    print("  ground-truth derivation; this scan picked the wrong field twice)")
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
    print(f"    best heuristic guess: offset={best[1]} width={best[2]} endian={best[3]}")
    print(f"    known-good answer:    offset={BZ_SEQ_OFF} width={BZ_SEQ_WIDTH} "
          f"endian={BZ_SEQ_ENDIAN}")
    if (best[1], best[2], best[3]) != (BZ_SEQ_OFF, BZ_SEQ_WIDTH, BZ_SEQ_ENDIAN):
        print("    -> they disagree, as expected; trust the known-good answer")
    return best


def delivery_report(by_peer, off, width, endian, class_filter=None):
    """Measure delivery over the sender's message sequence.

    The sequence is a *message* counter stamped on every datagram, so the unit
    of analysis is the distinct sequence value, not the datagram:

      delivered   distinct sequence values observed
      missing     values inside the observed span that never appeared at all
      advert      datagrams re-advertising a sequence already seen; this is
                  normal protocol behaviour, not network duplication
      inversion   a sequence first observed after a higher one already was —
                  the only thing a reorder buffer could ever have fixed

    `missing` is an upper bound on loss, not a loss rate: a sequence value is
    only observable while it is the sender's *current* message, so a value that
    advanced between two captured datagrams is indistinguishable from one that
    was lost.  A wrapped ring inflates it further.
    """
    print(f"  delivery analysis @ seq offset={off} width={width} {endian}"
          + (f"  (class 0x{class_filter:02x} only)" if class_filter is not None else ""))
    grand_inv = []
    for peer, pkts in sorted(by_peer.items(), key=lambda kv: -len(kv[1])):
        if class_filter is not None:
            pkts = [p for p in pkts
                    if bz_class(p['payload']) and bz_class(p['payload'])[1] == class_filter]
        first, order = {}, []
        for p in pkts:
            v = read_field(p['payload'], off, width, endian)
            if v is None:
                continue
            if v not in first:
                first[v] = p['tick']
                order.append(v)
        if len(first) < 10:
            continue
        lo, hi = min(first), max(first)
        span = hi - lo + 1
        missing = span - len(first)
        advert = len(pkts) - len(first)
        # An inversion is a first sighting below the running high-water mark.
        inversions, peak, lateness = 0, order[0], []
        for v in order:
            if v < peak:
                inversions += 1
                lateness.append(first[v] - first[peak])
            peak = max(peak, v)
        grand_inv.extend(lateness)
        # A peer we barely captured looks catastrophically lossy for the same
        # reason a two-frame video looks like a slideshow.  Say so.
        thin = ' [thin sample — treat as unmeasured]' if len(pkts) < 100 else ''
        print(f"    {peer:<22} datagrams={len(pkts):<7} sequences={len(first)} "
              f"(span {lo}..{hi}){thin}")
        print(f"      {'':20} missing={missing} ({100.0 * missing / span:.2f}% of span)  "
              f"inversions={inversions} ({100.0 * inversions / max(len(order), 1):.2f}%)  "
              f"advert={advert} ({100.0 * advert / max(len(pkts), 1):.1f}% of datagrams)")
        if lateness:
            lateness.sort()
            print(f"      {'':20} inversion lateness ms: "
                  f"med={lateness[len(lateness) // 2]} "
                  f"p95={lateness[int(len(lateness) * 0.95)]} max={lateness[-1]}")
    if grand_inv:
        grand_inv.sort()
        n = len(grand_inv)
        p95 = grand_inv[int(n * 0.95)]
        print(f"\n  ALL PEERS: {n} inversions, lateness med={grand_inv[n // 2]}ms "
              f"p95={p95}ms max={grand_inv[-1]}ms")
        print(f"  -> reordering these would need a {p95} ms hold window; "
              f"BZ_REORDER_MAX_HOLD_MS was 100 when the buffer was retired")
    else:
        print("\n  no inversions seen — nothing for a reorder buffer to fix")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('binfile')
    ap.add_argument('--meta', default=None, help='bz_buffer_log.meta.txt (default: alongside binfile)')
    ap.add_argument('--sid', type=int, default=None, help='restrict to one socket id')
    ap.add_argument('--seq-scan', action='store_true',
                    help='heuristic search for the sequence field (prefer seq_crossmatch.py)')
    ap.add_argument('--seq-offset', type=int, default=BZ_SEQ_OFF)
    ap.add_argument('--seq-width', type=int, choices=(1, 2, 4), default=BZ_SEQ_WIDTH)
    ap.add_argument('--seq-endian', choices=('le', 'be'), default=BZ_SEQ_ENDIAN)
    ap.add_argument('--class', dest='cls', default=None,
                    help='restrict delivery analysis to one packet class, e.g. 0x00')
    ap.add_argument('--limit', type=int, default=0, help='stop after N records')
    ap.add_argument('--dump', type=int, default=0, help='hex-dump the first N records')
    args = ap.parse_args()

    class_filter = int(args.cls, 0) if args.cls is not None else None

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
    if payload_bytes < BZ_HEADER_BYTES:
        print(f"  WARNING: payload prefix is {payload_bytes}B but the BZ header is "
              f"{BZ_HEADER_BYTES}B — re-capture with BZ_BUFFER_LOG_BYTES>={BZ_HEADER_BYTES}")
    if meta:
        seen = meta.get('total_events_seen')
        wrote = meta.get('records_written')
        if seen and wrote and seen != wrote:
            try:
                lost = 100.0 * (int(seen) - int(wrote)) / int(seen)
                print(f"  ring wrapped: {seen} events seen, only the last {wrote} kept "
                      f"({lost:.0f}% discarded — raise BZ_BUFFER_LOG_RING)")
                print(f"  -> `missing` below is inflated by the wrap; treat it as an "
                      f"upper bound only")
            except ValueError:
                pass
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

    # ── Packet classes ──────────────────────────────────────────────────────
    classes = Counter()
    for r in data:
        c = bz_class(r['payload'])
        if c:
            classes[c] += 1
    short = sum(1 for r in data if bz_class(r['payload']) is None)
    if classes:
        print("\n  PACKET CLASSES (BZ P2P header)")
        for (flags, cls), n in classes.most_common():
            tag = ' TRY' if flags & BZ_FLAG_TRY else ''
            print(f"    0x{flags:02x}/0x{cls:02x}{tag:<4} {CLASS_NAMES.get(cls, 'unknown'):<14} "
                  f"{n:>8}  ({100.0*n/len(data):5.1f}%)")
        if short:
            print(f"    {'(no header)':<24} {short:>8}  "
                  f"(payload prefix shorter than {BZ_HEADER_BYTES}B)")
        retrans = sum(n for (f, _c), n in classes.items() if f & BZ_FLAG_TRY)
        if retrans:
            print(f"    -> {retrans} of {len(data)} received datagrams "
                  f"({100.0*retrans/len(data):.1f}%) are retransmits")

    if args.dump:
        print("\n  FIRST RECORDS")
        for r in recs[:args.dump]:
            print(f"    t={r['tick']} {EVENT_NAMES.get(r['etype'],r['etype']):<12} "
                  f"{peer_key(r):<22} len={r['xfer']:<5} {r['payload'].hex()}")

    if args.seq_scan and by_peer:
        print()
        seq_scan(by_peer, payload_bytes)

    if by_peer:
        print()
        delivery_report(by_peer, args.seq_offset, args.seq_width, args.seq_endian,
                        class_filter)

    return 0


if __name__ == '__main__':
    sys.exit(main())
