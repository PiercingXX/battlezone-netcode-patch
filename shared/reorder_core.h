// shared/reorder_core.h — per-peer UDP reorder buffer, shared by both proxies.
//
// Battlezone's receiver discards any datagram whose sequence number is not the
// exact successor of the last one it accepted.  This buffer sits in front of
// the game's WSARecvFrom, holds out-of-order arrivals briefly, and releases
// them in sequence.
//
// Everything here is platform-independent (no winsock hooks, no Windows API):
// the caller supplies the clock and owns the lock, so the whole state machine
// can be exercised by tests/reorder_test.cpp on the host with no game running.
//
// Locking: every function below requires the caller to hold its reorder
// critical section.  Nothing here blocks, allocates, or does I/O.
//
// V4.7 — this file exists because the two proxies carried character-identical
// copies of this logic, and the copies carried identical defects.  Each fix
// below is annotated with the defect it closes.

#ifndef BZNET_REORDER_CORE_H
#define BZNET_REORDER_CORE_H

#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#  include <winsock2.h>
#  define BZ_SIN_ADDR_RAW(a) ((a).sin_addr.S_un.S_addr)
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  define BZ_SIN_ADDR_RAW(a) ((a).sin_addr.s_addr)
#endif

namespace bznet {

// ── Wire layout ──────────────────────────────────────────────────────────────
// Sequence field: u16 BIG-endian at payload byte offset 16.
//
// This was u32 little-endian at offset 13 until 2026-07-26, from
// resources/valid_capture_reorder_signal_only.csv.  A live capture that day
// (65,536 datagrams, bz_buffer_log.bin) showed that field cannot be the packet
// counter: two datagrams whose sequence numbers differ read back *identical*
// under it —
//     payload ...c1 00 00 38 f3   u32le@13 = 0x380000c1   u16be@16 = 0x38f3
//     payload ...c1 00 00 38 f5   u32le@13 = 0x380000c1   u16be@16 = 0x38f5
// because byte 16 is the counter's high byte and lands as the *most*
// significant byte of that little-endian u32.  The old field therefore only
// changed once per 256 packets: 340 distinct values where the real counter had
// 3,806.  Scoring every offset/width/endianness for per-peer monotonicity put
// u16be@16 at 0.984-1.000 forward-step across all three observed packet
// classes (18-byte heartbeat, ~250-byte state, mid), independently.
constexpr uint32_t kReorderSeqOffset    = 16;
constexpr uint32_t kReorderSeqBytes     = 2;
constexpr uint32_t kReorderSeqMask      = 0xffffu;
constexpr uint32_t kReorderSeqMinPay    = 18;   // shortest payload carrying a seq

// ── Window tuning ────────────────────────────────────────────────────────────
constexpr uint32_t kReorderDefaultMs    = 100;  // window ceiling
constexpr uint32_t kReorderMinMsDef     = 5;    // adaptive floor
constexpr uint32_t kReorderGrowPadMs    = 5;    // safety margin on growth
constexpr uint32_t kReorderDecayMs      = 500;  // quiet period between decay steps
constexpr uint32_t kReorderDecayKeepNum = 3;    // decay keeps 3/4 of the excess
constexpr uint32_t kReorderDecayKeepDen = 4;    // over the floor, per step

// ── Capacity ─────────────────────────────────────────────────────────────────
// Defect A: the drain loop can pull up to kReorderDrainCapDef datagrams per
// call.  With the old 8-slot rings, a burst overran storage instantly and the
// overflow was *discarded*.  Rings are now deep enough to absorb a full drain
// across a realistic lobby, the drain stops when storage is exhausted
// (reorder_all_full), and overflow is handed to the game instead of dropped.
constexpr uint32_t kReorderSlotCap      = 32;   // buffered packets per peer
constexpr uint32_t kReorderPeerCap      = 16;   // distinct IPv4 sources
constexpr uint32_t kReorderDrainCapDef  = 96;   // real recv calls per hook invocation
constexpr uint32_t kReorderDrainCapMax  = 128;
constexpr uint32_t kReorderMaxPktBytes  = 2048; // datagram copy size (was 1500)
constexpr uint32_t kReorderPeerIdleMs   = 30000;// reclaim a peer entry after this

// ── Wake helper (used by the proxies, kept here so both agree) ───────────────
constexpr uint32_t kReorderWakeTickMs   = 10;
constexpr uint32_t kReorderWakeIdleMs   = 10;
constexpr uint32_t kReorderWakeBurstCap = 8;

// Hold-time histogram buckets, in ms: 0, 1-5, 6-15, 16-30, 31-60, 61+
constexpr uint32_t kReorderHoldBuckets  = 6;

// Why a packet was handed to the game.  Distinguishing these is the whole
// point of the stats block: "in order" is the buffer working, "forced" is a
// gap we gave up on, "evicted" is storage pressure.
enum DeliverKind {
    kDeliverFirst    = 0,   // first packet seen from this peer
    kDeliverInOrder  = 1,   // exact successor of last_seq
    kDeliverForced   = 2,   // hold window expired with the gap still open
    kDeliverEvicted  = 3,   // ring was full; oldest handed out to make room
};

enum InsertResult {
    kInsertBuffered  = 0,
    kInsertStale     = 1,   // already superseded; rejected (defect B)
    kInsertDuplicate = 2,   // same seq already buffered
    kInsertEvicted   = 3,   // ring full; caller must deliver *evicted_out now
};

struct ReorderSlot {
    uint64_t    ts;                          // arrival time (ms)
    uint32_t    seq;                         // BZRNet sequence (u16be at payload[16])
    uint32_t    len;                         // payload byte count
    uint32_t    used;                        // 1 = slot occupied
    uint32_t    _pad;
    sockaddr_in from;                        // source address
    uint8_t     data[kReorderMaxPktBytes];   // full packet contents
};

struct PeerBuf {
    uint64_t    key;                // (ipv4_raw << 16) | port; 0 = empty
    uint32_t    seq_init;           // 1 once last_seq is valid
    uint32_t    last_seq;           // last sequence delivered to the game
    uint32_t    filled;             // occupied slots
    uint32_t    win_ms;             // adaptive hold window for this peer
    uint64_t    last_adjust_ms;     // last window grow/decay
    uint64_t    last_activity_ms;   // last packet seen (drives reclaim)
    uint64_t    last_deliver_ms;    // last delivery (sizes late-arrival growth)
    uint64_t    delivered;          // packets handed to the game from this peer
    ReorderSlot slots[kReorderSlotCap];
};

struct ReorderStats {
    uint64_t delivered_first;
    uint64_t delivered_in_order;
    uint64_t delivered_forced;
    uint64_t delivered_evicted;
    uint64_t dropped_stale;
    uint64_t dropped_duplicate;
    uint64_t dropped_reclaim;       // packets lost reclaiming an idle peer
    uint64_t bypass_short;          // too short / not IPv4: passed straight through
    uint64_t bypass_table_full;
    uint64_t emsgsize;              // oversized datagram destroyed by the stack
    uint64_t peers_reclaimed;
    uint64_t hold_ms_sum;
    uint32_t hold_ms_max;
    uint32_t max_drain_depth;
    uint64_t hold_hist[kReorderHoldBuckets];
};

struct ReorderCtx {
    // Config (set once from env at startup).
    bool     adapt;
    uint32_t win_max_ms;    // window ceiling
    uint32_t win_min_ms;    // adaptive floor
    uint32_t max_hold_ms;   // absolute per-packet hold ceiling (0 = window only)
    uint32_t depth;         // active slots per peer (<= kReorderSlotCap)
    uint32_t peers;         // active peer entries (<= kReorderPeerCap)

    // State.
    PeerBuf      tbl[kReorderPeerCap];
    uint32_t     rr;        // rotating delivery-scan cursor (defect G)
    ReorderStats stats;
};

// Sequence comparison must wrap in the counter's OWN width, not in 32 bits.
// The field is 16 bits, so it wraps every 65,536 packets — at the ~100-200
// packets/sec measured in a live match that is roughly every 6-11 minutes, i.e.
// at least once in an ordinary game.  Comparing in 32-bit space would read the
// wrap as a 65,535-packet backward jump and stall the peer for the rest of the
// match.
inline int32_t seq_cmp(uint32_t a, uint32_t b) {
    return static_cast<int16_t>(static_cast<uint16_t>(a) - static_cast<uint16_t>(b));
}

// Successor of a sequence number, wrapping in the field's width.
inline uint32_t seq_next(uint32_t s) {
    return (s + 1) & kReorderSeqMask;
}

inline bool seq_ahead_or_equal(uint32_t seq, uint32_t want) {
    return seq_cmp(seq, want) >= 0;
}

// Read the sequence out of a datagram.  Single definition so the two proxies
// and the IOCP path cannot drift on width or endianness — they previously each
// did their own memcpy, which is how a wrong offset stayed wrong in three
// places at once.  Caller must have checked len >= kReorderSeqMinPay.
inline uint32_t reorder_seq_from_payload(const uint8_t *p) {
    return (static_cast<uint32_t>(p[kReorderSeqOffset]) << 8)
         |  static_cast<uint32_t>(p[kReorderSeqOffset + 1]);
}

inline void reorder_init(ReorderCtx *c) {
    std::memset(c, 0, sizeof(*c));
    c->adapt       = true;
    c->win_max_ms  = kReorderDefaultMs;
    c->win_min_ms  = kReorderMinMsDef;
    c->max_hold_ms = kReorderDefaultMs;
    c->depth       = kReorderSlotCap;
    c->peers       = kReorderPeerCap;
}

// Forget all buffered traffic (the game closed the P2P socket).  Config and
// cumulative stats survive so the session summary stays meaningful.
inline void reorder_reset(ReorderCtx *c) {
    std::memset(c->tbl, 0, sizeof(c->tbl));
    c->rr = 0;
}

inline bool reorder_peer_full(const ReorderCtx *c, const PeerBuf *pb) {
    return pb->filled >= c->depth;
}

// Defect A: bound the drain by free storage.  Draining more datagrams out of
// the kernel than we can hold is how the old code came to discard packets the
// vanilla game would have received.
//
// Once any live peer's ring is full, stop pulling from the socket and deliver
// instead.  The undrained datagrams stay in the kernel receive buffer — 4 MB of
// it, forced at socket creation, versus 64 KB of ring per peer — where they
// keep their arrival order and cost nothing.  The kernel is the better buffer;
// our job is only to reorder the working set.
inline bool reorder_drain_saturated(const ReorderCtx *c) {
    for (uint32_t i = 0; i < c->peers; ++i) {
        if (c->tbl[i].key != 0 && c->tbl[i].filled >= c->depth) {
            return true;
        }
    }
    return false;
}

// Look up or create the PeerBuf for addr.  Defect F: when the table is full,
// reclaim an entry that has been silent for kReorderPeerIdleMs instead of
// permanently bypassing the buffer for every new source.
inline PeerBuf *reorder_get_peer(ReorderCtx *c, const sockaddr_in &addr, uint64_t now) {
    const uint64_t k =
        (static_cast<uint64_t>(static_cast<uint32_t>(BZ_SIN_ADDR_RAW(addr))) << 16)
        | static_cast<uint64_t>(ntohs(addr.sin_port));

    for (uint32_t i = 0; i < c->peers; ++i) {
        if (c->tbl[i].key == k) {
            c->tbl[i].last_activity_ms = now;
            return &c->tbl[i];
        }
    }

    PeerBuf *fresh = nullptr;
    for (uint32_t i = 0; i < c->peers; ++i) {
        if (c->tbl[i].key == 0) {
            fresh = &c->tbl[i];
            break;
        }
    }

    if (fresh == nullptr) {
        // Table full: find the least recently active entry and reclaim it if
        // it has been idle long enough that anything it holds is stale anyway.
        PeerBuf *oldest = &c->tbl[0];
        for (uint32_t i = 1; i < c->peers; ++i) {
            if (c->tbl[i].last_activity_ms < oldest->last_activity_ms) {
                oldest = &c->tbl[i];
            }
        }
        if (now - oldest->last_activity_ms < kReorderPeerIdleMs) {
            return nullptr;   // every peer is live; caller passes the packet through
        }
        c->stats.peers_reclaimed++;
        c->stats.dropped_reclaim += oldest->filled;
        fresh = oldest;
    }

    std::memset(fresh, 0, sizeof(*fresh));
    fresh->key              = k;
    fresh->win_ms           = c->adapt ? c->win_min_ms : c->win_max_ms;
    fresh->last_adjust_ms   = now;
    fresh->last_activity_ms = now;
    fresh->last_deliver_ms  = now;
    return fresh;
}

// Adapt the peer's hold window from the arriving packet, BEFORE insertion.
//
// Grow only on evidence that this link actually reorders:
//   - a packet we already released past arrives late, or
//   - the awaited successor closes a gap, and the wait it resolved tells us
//     how big the window needed to be.
// True loss never grows the window: a lost packet simply never arrives.
//
// Defect C: the late-arrival branch used to double the window blindly
// (5 -> 15 -> 35 -> 75 -> 100 in four events), so a single burst pinned the
// peer at the ceiling.  It now grows by the lateness actually measured.
inline void reorder_adapt_on_arrival(ReorderCtx *c, PeerBuf *pb, uint32_t seq, uint64_t now) {
    if (!c->adapt || !pb->seq_init) {
        return;
    }

    const int32_t cmp = seq_cmp(seq, pb->last_seq);
    if (cmp == 0) {
        // Exact duplicate of the last delivered packet (link-layer retransmit,
        // common on WiFi): not reorder evidence, must not grow the window.
        return;
    }

    if (cmp < 0) {
        // Late/backward arrival: we released its successors too early.  How
        // much too early is exactly how long ago we last delivered.
        uint32_t late = static_cast<uint32_t>(now - pb->last_deliver_ms) + kReorderGrowPadMs;
        if (late > c->win_max_ms) {
            late = c->win_max_ms;
        }
        if (late > pb->win_ms) {
            pb->win_ms = late;
            pb->last_adjust_ms = now;
        }
        return;
    }

    if (seq == seq_next(pb->last_seq) && pb->filled > 0) {
        // Gap just closed: measure how long the held packets waited.
        uint64_t oldest_ts = now;
        for (uint32_t i = 0; i < c->depth; ++i) {
            if (pb->slots[i].used && pb->slots[i].ts < oldest_ts) {
                oldest_ts = pb->slots[i].ts;
            }
        }
        uint32_t waited = static_cast<uint32_t>(now - oldest_ts) + kReorderGrowPadMs;
        if (waited > c->win_max_ms) {
            waited = c->win_max_ms;
        }
        if (waited > pb->win_ms) {
            pb->win_ms = waited;
            pb->last_adjust_ms = now;
        }
    }
}

// Shrink the window back toward the floor after a quiet period with no reorder
// evidence.  Defect C: the old decay was 5 ms per 2000 ms, so returning from
// the 100 ms ceiling took ~38 seconds and the window was effectively pinned for
// the rest of a match.  Decay is now proportional — a clean link is back at the
// floor in a few seconds.
inline void reorder_decay(ReorderCtx *c, PeerBuf *pb, uint64_t now) {
    if (!c->adapt || now - pb->last_adjust_ms < kReorderDecayMs) {
        return;
    }
    if (pb->win_ms <= c->win_min_ms) {
        pb->win_ms = c->win_min_ms;
        pb->last_adjust_ms = now;
        return;
    }
    uint32_t excess = pb->win_ms - c->win_min_ms;
    uint32_t keep   = excess * kReorderDecayKeepNum / kReorderDecayKeepDen;
    if (keep >= excess) {
        keep = excess - 1;   // guarantee forward progress at small excesses
    }
    pb->win_ms = c->win_min_ms + keep;
    pb->last_adjust_ms = now;
}

// Drop any buffered packet the game has now moved past.
//
// Defect B, second half: rejecting stale packets at insert is not enough.  A
// packet can be accepted legitimately and *become* stale when a later delivery
// advances the cursor past it — the first packet from a peer, or any forced
// release over a gap, does exactly this.  Left in the ring it occupies a slot
// and is eventually force-delivered out of order, which is the very thing this
// buffer exists to prevent.  Caller must hold the lock.
inline void reorder_purge_stale(ReorderCtx *c, PeerBuf *pb) {
    if (!pb->seq_init) {
        return;
    }
    for (uint32_t i = 0; i < c->depth; ++i) {
        if (!pb->slots[i].used) {
            continue;
        }
        if (seq_cmp(pb->slots[i].seq, pb->last_seq) <= 0) {
            pb->slots[i].used = 0;
            if (pb->filled > 0) {
                --pb->filled;
            }
            c->stats.dropped_stale++;
        }
    }
}

// Record that a packet was handed to the game: advance the peer cursor and
// account the hold time.
//
// Defect B: the cursor used to be assigned unconditionally, so delivering a
// packet older than last_seq walked it *backwards* and corrupted in-order
// tracking for everything that followed.  It now only ever advances.
inline void reorder_note_delivery(ReorderCtx *c, PeerBuf *pb, uint32_t seq,
                                  uint64_t arrival_ts, uint64_t now, int kind) {
    if (!pb->seq_init || seq_cmp(seq, pb->last_seq) > 0) {
        pb->last_seq = seq;
    }
    pb->seq_init = 1;
    reorder_purge_stale(c, pb);
    pb->last_deliver_ms = now;
    pb->last_activity_ms = now;
    pb->delivered++;

    const uint64_t held = (now > arrival_ts) ? (now - arrival_ts) : 0;
    const uint32_t held32 = (held > 0xffffffffULL) ? 0xffffffffU : static_cast<uint32_t>(held);
    c->stats.hold_ms_sum += held32;
    if (held32 > c->stats.hold_ms_max) {
        c->stats.hold_ms_max = held32;
    }
    uint32_t bucket = 0;
    if (held32 == 0)       bucket = 0;
    else if (held32 <= 5)  bucket = 1;
    else if (held32 <= 15) bucket = 2;
    else if (held32 <= 30) bucket = 3;
    else if (held32 <= 60) bucket = 4;
    else                   bucket = 5;
    c->stats.hold_hist[bucket]++;

    switch (kind) {
        case kDeliverFirst:   c->stats.delivered_first++;    break;
        case kDeliverInOrder: c->stats.delivered_in_order++; break;
        case kDeliverForced:  c->stats.delivered_forced++;   break;
        case kDeliverEvicted: c->stats.delivered_evicted++;  break;
        default: break;
    }
}

// Insert a received packet.
//
// Defect B: packets the game has already moved past are rejected outright.
// Buffering them wasted a scarce slot, delivered them out of order anyway, and
// dragged the peer cursor backwards.
//
// Defect A: when the ring is full the oldest packet is *returned to the caller*
// via evicted_out for immediate delivery, never discarded.  Losing ordering on
// one packet is strictly better than losing the packet.
inline InsertResult reorder_insert(ReorderCtx *c, PeerBuf *pb, uint32_t seq, uint64_t ts,
                                   const sockaddr_in &from, const uint8_t *data, uint32_t len,
                                   ReorderSlot *evicted_out) {
    if (pb->seq_init && seq_cmp(seq, pb->last_seq) <= 0) {
        c->stats.dropped_stale++;
        return kInsertStale;
    }

    for (uint32_t i = 0; i < c->depth; ++i) {
        if (pb->slots[i].used && pb->slots[i].seq == seq) {
            c->stats.dropped_duplicate++;
            return kInsertDuplicate;
        }
    }

    const uint32_t n = (len > kReorderMaxPktBytes) ? kReorderMaxPktBytes : len;

    for (uint32_t i = 0; i < c->depth; ++i) {
        if (!pb->slots[i].used) {
            pb->slots[i].used = 1;
            pb->slots[i].seq  = seq;
            pb->slots[i].ts   = ts;
            pb->slots[i].from = from;
            std::memcpy(pb->slots[i].data, data, n);
            pb->slots[i].len  = n;
            ++pb->filled;
            return kInsertBuffered;
        }
    }

    // Ring full: evict the oldest arrival and hand it back for delivery.
    uint32_t oix = 0;
    for (uint32_t i = 1; i < c->depth; ++i) {
        if (pb->slots[i].used && pb->slots[i].ts < pb->slots[oix].ts) {
            oix = i;
        }
    }
    if (evicted_out != nullptr) {
        *evicted_out = pb->slots[oix];
    }
    pb->slots[oix].seq = seq;
    pb->slots[oix].ts  = ts;
    pb->slots[oix].from = from;
    std::memcpy(pb->slots[oix].data, data, n);
    pb->slots[oix].len = n;
    // filled unchanged: one out, one in.
    return kInsertEvicted;
}

// Find the slot to deliver next for this peer.  Prefers the exact in-order
// successor; otherwise releases an aged-out packet.  Returns the slot index or
// -1 when nothing is ready, and reports whether the release was forced.
inline int reorder_pick(ReorderCtx *c, PeerBuf *pb, uint64_t now, int *kind_out) {
    *kind_out = kDeliverInOrder;
    if (pb->filled == 0) {
        return -1;
    }

    if (!pb->seq_init) {
        // First delivery for this peer sets the cursor, so it decides what
        // counts as stale from here on.  Pick the *lowest sequence* buffered,
        // not merely the oldest arrival: if a whole drain pass landed at once,
        // the earliest slot is just the first one recv returned, and starting
        // the cursor above a packet we are still holding makes that packet
        // stale on arrival.
        int best = -1;
        for (uint32_t i = 0; i < c->depth; ++i) {
            if (!pb->slots[i].used) {
                continue;
            }
            if (best < 0) {
                best = static_cast<int>(i);
                continue;
            }
            const int32_t cmp = seq_cmp(pb->slots[i].seq, pb->slots[best].seq);
            if (cmp < 0 || (cmp == 0 && pb->slots[i].ts < pb->slots[best].ts)) {
                best = static_cast<int>(i);
            }
        }
        *kind_out = kDeliverFirst;
        return best;
    }

    const uint32_t want = seq_next(pb->last_seq);
    for (uint32_t i = 0; i < c->depth; ++i) {
        if (pb->slots[i].used && pb->slots[i].seq == want) {
            return static_cast<int>(i);
        }
    }

    // Nothing in order.  A packet may be released once it has waited its peer's
    // adaptive window — or the absolute max-hold ceiling, whichever is smaller,
    // so no packet is ever held longer than a value we can state.
    uint32_t hold = pb->win_ms;
    if (c->max_hold_ms != 0 && hold > c->max_hold_ms) {
        hold = c->max_hold_ms;
    }

    int      best_ahead  = -1;
    uint32_t best_dist   = 0;
    int      best_oldest = -1;
    for (uint32_t i = 0; i < c->depth; ++i) {
        if (!pb->slots[i].used) {
            continue;
        }
        if (now < pb->slots[i].ts || (now - pb->slots[i].ts) < hold) {
            continue;
        }
        if (best_oldest < 0 || pb->slots[i].ts < pb->slots[best_oldest].ts) {
            best_oldest = static_cast<int>(i);
        }
        if (seq_ahead_or_equal(pb->slots[i].seq, want)) {
            const uint32_t dist = pb->slots[i].seq - want;
            if (best_ahead < 0 || dist < best_dist) {
                best_ahead = static_cast<int>(i);
                best_dist  = dist;
            }
        }
    }

    *kind_out = kDeliverForced;
    if (best_ahead >= 0) {
        return best_ahead;
    }
    return best_oldest;   // -1 when nothing has aged out yet
}

// Scan peers for the next deliverable packet.
//
// Defect G: the scan used to start at peer 0 every call and break on the first
// hit, so in a 4-player mesh the low-index peer monopolised service.  It now
// resumes where it left off, round-robin.
inline bool reorder_next_ready(ReorderCtx *c, uint64_t now,
                               uint32_t *peer_out, int *slot_out, int *kind_out) {
    for (uint32_t n = 0; n < c->peers; ++n) {
        const uint32_t i = (c->rr + n) % c->peers;
        if (c->tbl[i].key == 0) {
            continue;
        }
        const int s = reorder_pick(c, &c->tbl[i], now, kind_out);
        if (s >= 0) {
            c->rr      = (i + 1) % c->peers;
            *peer_out  = i;
            *slot_out  = s;
            return true;
        }
    }
    return false;
}

// Release a picked slot: copy it out, free it, advance the cursor, decay.
inline void reorder_take(ReorderCtx *c, PeerBuf *pb, int slot, uint64_t now,
                         int kind, ReorderSlot *out) {
    ReorderSlot *pkt = &pb->slots[slot];
    if (out != nullptr) {
        *out = *pkt;
    }
    pkt->used = 0;
    if (pb->filled > 0) {
        --pb->filled;
    }
    reorder_note_delivery(c, pb, pkt->seq, pkt->ts, now, kind);
    reorder_decay(c, pb, now);
}

// Render the counters as one log line.  hold_ms_max / hold_ms_avg are the
// numbers that say how much latency this buffer added to the game's streams —
// the measurement every test before V4.7 was missing.
inline int reorder_format_stats(const ReorderCtx *c, char *buf, size_t n) {
    const ReorderStats &s = c->stats;
    const uint64_t delivered = s.delivered_first + s.delivered_in_order
                             + s.delivered_forced + s.delivered_evicted;
    const unsigned avg = delivered ? static_cast<unsigned>(s.hold_ms_sum / delivered) : 0u;

    int p = std::snprintf(buf, n,
        "reorder_stats: delivered=%llu (in_order=%llu forced=%llu evicted=%llu first=%llu)"
        " dropped(stale=%llu dup=%llu reclaim=%llu) bypass(short=%llu tblfull=%llu)"
        " emsgsize=%llu peers_reclaimed=%llu drain_max=%u"
        " hold_ms(avg=%u max=%u) hist[0,1-5,6-15,16-30,31-60,61+]=[%llu,%llu,%llu,%llu,%llu,%llu]",
        static_cast<unsigned long long>(delivered),
        static_cast<unsigned long long>(s.delivered_in_order),
        static_cast<unsigned long long>(s.delivered_forced),
        static_cast<unsigned long long>(s.delivered_evicted),
        static_cast<unsigned long long>(s.delivered_first),
        static_cast<unsigned long long>(s.dropped_stale),
        static_cast<unsigned long long>(s.dropped_duplicate),
        static_cast<unsigned long long>(s.dropped_reclaim),
        static_cast<unsigned long long>(s.bypass_short),
        static_cast<unsigned long long>(s.bypass_table_full),
        static_cast<unsigned long long>(s.emsgsize),
        static_cast<unsigned long long>(s.peers_reclaimed),
        static_cast<unsigned>(s.max_drain_depth),
        avg, static_cast<unsigned>(s.hold_ms_max),
        static_cast<unsigned long long>(s.hold_hist[0]),
        static_cast<unsigned long long>(s.hold_hist[1]),
        static_cast<unsigned long long>(s.hold_hist[2]),
        static_cast<unsigned long long>(s.hold_hist[3]),
        static_cast<unsigned long long>(s.hold_hist[4]),
        static_cast<unsigned long long>(s.hold_hist[5]));
    if (p < 0 || static_cast<size_t>(p) >= n) {
        return p;
    }

    // Per-peer live window, so a pinned window is visible at a glance.
    p += std::snprintf(buf + p, n - static_cast<size_t>(p), " win_ms=[");
    bool first = true;
    for (uint32_t i = 0; i < c->peers && p > 0 && static_cast<size_t>(p) < n; ++i) {
        if (c->tbl[i].key == 0) {
            continue;
        }
        p += std::snprintf(buf + p, n - static_cast<size_t>(p), "%s%u",
                           first ? "" : ",", static_cast<unsigned>(c->tbl[i].win_ms));
        first = false;
    }
    if (p > 0 && static_cast<size_t>(p) < n) {
        p += std::snprintf(buf + p, n - static_cast<size_t>(p), "]");
    }
    return p;
}

}  // namespace bznet

#endif  // BZNET_REORDER_CORE_H
