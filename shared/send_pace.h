// shared/send_pace.h — outbound burst measurement, and optional smoothing.
//
// The failure that has actually ended matches is not receive-side reordering:
// it is a peer's reliable-send queue backing up and then burst-retransmitting.
// One measured case pushed 1,179 packets carrying a single reliable message in
// 13.4 seconds, drove the sender's real usage to ~14.5 KB/s against a 6.4 KB/s
// governor budget, and drowned the ping exchange badly enough that the host's
// auto-kick fired ten seconds after the flood ended.  Receive-side buffering
// cannot help with that; only the sender can.
//
// This does two things:
//
//   1. Always measures.  Peak packets/sec and bytes/sec, and how many seconds
//      ran above a burst threshold.  Nothing in this project has ever measured
//      its own outbound behaviour, so there is no evidence about whether the
//      local machine produces these floods at all.  Measurement is free and
//      comes first.
//
//   2. Optionally smooths, when BZ_SEND_PACE is set.  A token bucket delays
//      packets that exceed the configured rate, spreading a volley over a few
//      milliseconds instead of slamming a bloated queue.  Two rules keep it
//      from doing harm: packets too small to carry a sequence number — the
//      control and ping traffic whose latency the auto-kick actually measures —
//      are never delayed, and a packet that would wait longer than max_delay_ms
//      is sent immediately instead.  Nothing is ever dropped.
//
// Off by default: it adds send latency, and there is no evidence yet that it is
// needed.  Turn it on once the stats show bursts worth smoothing.
//
// Platform-independent: the caller owns the lock, supplies the clock, and
// performs the real send.  Covered by tests/send_pace_test.cpp.

#ifndef BZNET_SEND_PACE_H
#define BZNET_SEND_PACE_H

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "reorder_core.h"   // kReorderMaxPktBytes, kReorderSeqMinPay

namespace bznet {

constexpr uint32_t kPaceQueueSlots   = 256;
constexpr uint32_t kPaceTickMs       = 2;      // pacer thread wake interval
constexpr uint32_t kPaceMaxDelayDef  = 20;     // ms; beyond this, send immediately
constexpr uint32_t kPaceBurstPps     = 100;    // BZ's steady rate is ~30 pkt/s
constexpr uint32_t kPaceAddrBytes    = 128;    // room for any sockaddr we see

enum PaceDecision {
    kPaceSendNow       = 0,  // within budget, or exempt: send it now
    kPaceQueued        = 1,  // caller must not send; the pacer thread will
    kPaceFlushThenSend = 2,  // drain the whole queue in order, then send this
};

struct PaceEntry {
    uintptr_t sock;
    uint64_t  due_ms;
    int       tolen;
    uint32_t  len;
    uint8_t   to[kPaceAddrBytes];
    uint8_t   data[kReorderMaxPktBytes];
};

struct PaceStats {
    uint64_t packets;            // everything that passed through the hook
    uint64_t bytes;
    uint64_t paced;              // delayed by the bucket
    uint64_t exempt_small;       // control/ping traffic, never delayed
    uint64_t over_budget_passed; // would have waited > max_delay_ms
    uint64_t queue_full_passed;  // queue was full; sent immediately, never dropped
    uint32_t peak_pps;           // busiest one-second window
    uint32_t peak_bps;
    uint64_t burst_seconds;      // seconds above kPaceBurstPps
    uint32_t max_delay_ms;       // worst delay actually applied
    uint32_t peak_queue;         // deepest the queue ever got
};

struct PaceCtx {
    // Config.
    uint32_t rate_bps;       // 0 = measure only, never delay
    uint32_t burst_bytes;    // token bucket capacity
    uint32_t max_delay_ms;

    // Token bucket.
    int64_t  tokens;         // bytes; may go negative while a burst drains
    uint64_t last_fill_ms;

    // One-second measurement window.
    uint64_t win_start_ms;
    uint32_t win_packets;
    uint32_t win_bytes;

    // Pending queue (FIFO; preserves the game's send order).
    PaceEntry q[kPaceQueueSlots];
    uint32_t  head;
    uint32_t  count;

    PaceStats stats;
};

inline void pace_init(PaceCtx *c, uint32_t rate_bps, uint32_t max_delay_ms, uint64_t now) {
    std::memset(c, 0, sizeof(*c));
    c->rate_bps     = rate_bps;
    c->max_delay_ms = max_delay_ms;
    // One tick's worth of traffic may always go out back-to-back, with a floor
    // so a single MTU-sized packet is never structurally unable to pass.
    c->burst_bytes  = rate_bps ? (rate_bps / 20) : 0;
    if (c->burst_bytes < kReorderMaxPktBytes) {
        c->burst_bytes = kReorderMaxPktBytes;
    }
    c->tokens        = c->burst_bytes;
    c->last_fill_ms  = now;
    c->win_start_ms  = now;
}

// Close the current measurement window if a second has elapsed.  Must also be
// called from the pacer thread's idle tick and before formatting stats: a burst
// that ends the traffic (which is exactly the case worth catching) would
// otherwise never have its window accounted, and the peak would read zero.
inline void pace_tick(PaceCtx *c, uint64_t now) {
    if (now - c->win_start_ms < 1000) {
        return;
    }
    if (c->win_packets > c->stats.peak_pps) {
        c->stats.peak_pps = c->win_packets;
    }
    if (c->win_bytes > c->stats.peak_bps) {
        c->stats.peak_bps = c->win_bytes;
    }
    if (c->win_packets > kPaceBurstPps) {
        c->stats.burst_seconds++;
    }
    // Skip whole idle seconds rather than looping over them.
    c->win_start_ms = now - ((now - c->win_start_ms) % 1000);
    c->win_packets  = 0;
    c->win_bytes    = 0;
}

// Roll the one-second measurement window.  Always runs, pacing or not.
inline void pace_observe(PaceCtx *c, uint32_t len, uint64_t now) {
    pace_tick(c, now);
    c->win_packets++;
    c->win_bytes += len;
    c->stats.packets++;
    c->stats.bytes += len;
}

// Decide what to do with one outbound datagram.  On kPaceQueued the caller must
// not send it itself; *due_ms is when the pacer should.
inline PaceDecision pace_admit(PaceCtx *c, uint32_t len, uint64_t now, uint64_t *due_ms) {
    pace_observe(c, len, now);
    *due_ms = now;

    if (c->rate_bps == 0) {
        return kPaceSendNow;   // measurement only
    }

    // Never delay traffic too small to carry a sequence number.  That is the
    // control and ping exchange, and its round-trip time is precisely what a
    // host measures against AutoKickPing — adding milliseconds to it to smooth
    // a bulk burst would trade the symptom for the cause.  These packets may
    // overtake queued bulk, which is the point: carrying no sequence number,
    // they are not part of the ordered stream and cannot be dropped as stale.
    if (len < kReorderSeqMinPay) {
        c->stats.exempt_small++;
        return kPaceSendNow;
    }

    // Refill.
    const uint64_t elapsed = now - c->last_fill_ms;
    if (elapsed > 0) {
        c->tokens += static_cast<int64_t>((elapsed * c->rate_bps) / 1000);
        if (c->tokens > static_cast<int64_t>(c->burst_bytes)) {
            c->tokens = c->burst_bytes;
        }
        c->last_fill_ms = now;
    }

    // Ordering rule: once anything is queued, everything sequenced queues
    // behind it.  Letting a later packet overtake a held one would manufacture
    // out-of-order arrivals at the peer — the exact defect the receive-side
    // half of this patch exists to undo.
    const bool queue_busy = (c->count > 0);

    if (!queue_busy && c->tokens >= static_cast<int64_t>(len)) {
        c->tokens -= len;
        return kPaceSendNow;
    }

    const int64_t  deficit = static_cast<int64_t>(len) - c->tokens;
    const uint64_t wait_ms = (deficit > 0)
                             ? (static_cast<uint64_t>(deficit) * 1000) / c->rate_bps
                             : 0;

    if (wait_ms > c->max_delay_ms || c->count >= kPaceQueueSlots) {
        // Either smoothing this packet would cost more latency than the burst
        // is worth, or we are out of storage.  Give up on shaping rather than
        // hold or drop anything: the caller flushes whatever is queued, in
        // order, and then sends this one.  Order survives; no packet is lost.
        if (wait_ms > c->max_delay_ms) {
            c->stats.over_budget_passed++;
        } else {
            c->stats.queue_full_passed++;
        }
        c->tokens = 0;
        return queue_busy ? kPaceFlushThenSend : kPaceSendNow;
    }

    c->tokens -= len;   // goes negative; the refill above works it off
    c->stats.paced++;
    if (wait_ms > c->stats.max_delay_ms) {
        c->stats.max_delay_ms = static_cast<uint32_t>(wait_ms);
    }
    *due_ms = now + wait_ms;
    return kPaceQueued;
}

// Store a datagram the pacer will send later.  Returns false if it cannot be
// stored, in which case the caller must send it immediately.
inline bool pace_enqueue(PaceCtx *c, uintptr_t sock, const void *to, int tolen,
                         const uint8_t *data, uint32_t len, uint64_t due_ms) {
    if (c->count >= kPaceQueueSlots || len > kReorderMaxPktBytes
        || tolen <= 0 || static_cast<uint32_t>(tolen) > kPaceAddrBytes) {
        return false;
    }
    PaceEntry &e = c->q[(c->head + c->count) % kPaceQueueSlots];
    e.sock   = sock;
    e.due_ms = due_ms;
    e.tolen  = tolen;
    e.len    = len;
    std::memcpy(e.to, to, static_cast<size_t>(tolen));
    std::memcpy(e.data, data, len);
    c->count++;
    if (c->count > c->stats.peak_queue) {
        c->stats.peak_queue = c->count;
    }
    return true;
}

// Pop the head if it is due.  FIFO, so the game's ordering is preserved.
inline bool pace_pop_due(PaceCtx *c, uint64_t now, PaceEntry *out) {
    if (c->count == 0 || c->q[c->head].due_ms > now) {
        return false;
    }
    *out    = c->q[c->head];
    c->head = (c->head + 1) % kPaceQueueSlots;
    c->count--;
    return true;
}

// Pop the head regardless of its due time.  Used by the flush path, which
// gives up on shaping in order to preserve ordering.
inline bool pace_pop_any(PaceCtx *c, PaceEntry *out) {
    if (c->count == 0) {
        return false;
    }
    *out    = c->q[c->head];
    c->head = (c->head + 1) % kPaceQueueSlots;
    c->count--;
    return true;
}

// Drop anything queued for a socket the game just closed: the handle can be
// reused, and a stale datagram on a fresh socket would corrupt its state.
inline void pace_purge_socket(PaceCtx *c, uintptr_t sock) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < c->count; ++i) {
        PaceEntry &e = c->q[(c->head + i) % kPaceQueueSlots];
        if (e.sock == sock) {
            continue;
        }
        if (kept != i) {
            c->q[(c->head + kept) % kPaceQueueSlots] = e;
        }
        kept++;
    }
    c->count = kept;
}

inline int pace_format_stats(const PaceCtx *c, char *buf, size_t n) {
    const PaceStats &s = c->stats;
    return std::snprintf(buf, n,
        "send_stats: packets=%llu bytes=%llu peak_pps=%u peak_bps=%u burst_seconds=%llu"
        " | pacing=%s paced=%llu exempt_small=%llu over_budget=%llu queue_full=%llu"
        " max_delay_ms=%u peak_queue=%u",
        static_cast<unsigned long long>(s.packets),
        static_cast<unsigned long long>(s.bytes),
        static_cast<unsigned>(s.peak_pps),
        static_cast<unsigned>(s.peak_bps),
        static_cast<unsigned long long>(s.burst_seconds),
        c->rate_bps ? "on" : "off (measure only)",
        static_cast<unsigned long long>(s.paced),
        static_cast<unsigned long long>(s.exempt_small),
        static_cast<unsigned long long>(s.over_budget_passed),
        static_cast<unsigned long long>(s.queue_full_passed),
        static_cast<unsigned>(s.max_delay_ms),
        static_cast<unsigned>(s.peak_queue));
}

}  // namespace bznet

#endif  // BZNET_SEND_PACE_H
