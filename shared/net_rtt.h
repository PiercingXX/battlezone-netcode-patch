// Per-peer round-trip sampling from the protocol's own ack field.
//
// WHY THIS EXISTS
// ---------------
// On 2026-08-15 two testers reported lag in their last match.  The evidence
// that actually explained it was a single number — BZLogger's `Delay:` line,
// 141 ms host-side and 174 ms client-side, against 73 ms two matches earlier
// the same evening.  But BZLogger prints that block only at match start, so
// the question "did it spike during the warp storm at 16:55, or was it flat
// and something else broke?" had no answer in any log we collect.  The
// diagnosis stopped at "the link was bad" and could not go further.
//
// This module closes that hole: a periodic, per-peer RTT line from inside the
// proxy, on the same cadence as governor_trace.
//
// WHY THE ACK AND NOT THE SEND CLOCK
// ----------------------------------
// The header carries the sender's wall clock at offset 2 (u64 epoch ms,
// stamped per copy — see resources/BZ_P2P_HEADER.md).  Subtracting it from
// local time on receive looks like a free one-way delay measurement and is
// not: the two machines' clocks are not synchronised, so that difference is
// (true delay + clock offset) and nothing in the datagram separates the two
// terms.  On the 2026-08-15 pair the offsets were hours apart — the machines
// were in different time zones with independently drifting clocks.
//
// The ack field (offset 14, "highest sequence the sender has seen from this
// peer") closes a loop that stays entirely inside ONE clock: record when we
// sent our own sequence S, and when a peer's datagram acknowledges S, the
// elapsed local time is a true round trip.  No clock sync, no offset term.
//
// WHAT THE NUMBER INCLUDES
// ------------------------
// The ack is piggybacked on the peer's normal outbound traffic, not sent
// immediately, so a sample is (network RTT + however long the peer sat on the
// ack before its next send).  It is therefore an UPPER BOUND on path RTT.
// That is the honest reading and the line says so.  It is still the right
// metric for "is the link degrading", because the padding term is bounded by
// the peer's send interval and does not grow with distance or congestion.
//
// AMBIGUOUS SAMPLES ARE DISCARDED (Karn's algorithm)
// --------------------------------------------------
// If we sent sequence S more than once, an ack for S cannot be attributed to
// a particular copy, and counting it against the first send inflates the RTT
// while counting it against the last deflates it.  Both are wrong, and this
// protocol retransmits heavily — the 2026-08-15 host resent one sequence 27
// times.  Slots that see a second send are marked ambiguous and never yield a
// sample.  This is Karn's algorithm and it is not optional here.

#ifndef BZ_NET_RTT_H
#define BZ_NET_RTT_H

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "reorder_core.h"

namespace bznet {

// Peers tracked concurrently.  Matches kDampenPeers: the same 16-player
// ceiling the rest of the patch assumes.
constexpr uint32_t kRttPeers = 16;

// Outstanding own-sequences remembered per peer.  Sized from the worst
// measured burst rather than the average: the 2026-08-15 host peaked at 3,136
// packets/sec, and at a 170 ms RTT that leaves ~530 sequences in flight.  512
// covers that; beyond it the oldest slot is evicted and the sample is simply
// not taken, which costs resolution during a burst but never a wrong number.
constexpr uint32_t kRttSlots = 512;

// Samples above this are dropped as bookkeeping artefacts rather than link
// measurements — a slot that survived a long stall, or a peer that went away
// and came back on the same address.  30 s is far outside anything the game
// tolerates (it auto-kicks well below that), so nothing real is discarded.
constexpr uint64_t kRttMaxSampleMs = 30000;

struct RttSlot {
    uint32_t seq;           // one of OUR sequence numbers
    uint64_t sent_ms;       // when the first copy went out
    bool     used;
    bool     ambiguous;     // resent before it was acked - Karn: never sample
};

struct RttPeer {
    uint32_t addr;              // network-order IPv4, 0 = free
    uint64_t last_use_ms;
    RttSlot  slots[kRttSlots];
    uint32_t head;              // ring insert point
    uint32_t used;
    uint32_t last_ack;          // highest ack seen from this peer
    bool     ack_init;

    // Session totals.
    uint64_t samples;
    uint64_t sum_ms;
    uint32_t min_ms;
    uint32_t max_ms;

    // Since the last periodic emit.
    uint64_t win_samples;
    uint64_t win_sum_ms;
    uint32_t win_min_ms;
    uint32_t win_max_ms;

    // Smoothed RTT and variation, RFC 6298 form.  srtt/rttvar are the
    // stable "what is the link doing now" pair; the window min/max above are
    // the raw spread the periodic line reports alongside them.
    uint32_t srtt_ms;
    uint32_t rttvar_ms;
    bool     srtt_init;
};

struct RttStats {
    uint64_t sends_tracked;     // datagrams whose sequence we recorded
    uint64_t sends_ambiguous;   // sequences seen a second time before an ack
    uint64_t slots_evicted;     // ring full: an unacked sequence was forgotten
    uint64_t acks_seen;         // datagrams carrying an advancing ack
    uint64_t samples;           // acks that produced a usable RTT
    uint64_t unmatched;         // advancing ack for a sequence we no longer hold
    uint64_t discarded_ambig;   // matched a slot, but Karn says do not use it
    uint64_t discarded_range;   // matched, but the elapsed time was implausible
    uint64_t peers_evicted;
};

struct RttCtx {
    bool     enabled;
    uint32_t trace_ms;          // periodic emit interval, 0 = silent
    uint64_t last_trace_ms;
    RttPeer  peers[kRttPeers];
    RttStats st;
};

inline void rtt_init(RttCtx *c, bool enabled, uint32_t trace_ms, uint64_t now) {
    std::memset(c, 0, sizeof(*c));
    c->enabled       = enabled;
    c->trace_ms      = trace_ms;
    c->last_trace_ms = now;
}

// Find or claim the per-peer record.  Same eviction rule as the dampen table:
// oldest-touched loses, so a stale entry cannot pin a slot forever.
inline RttPeer *rtt_get_peer(RttCtx *c, uint32_t addr, uint64_t now) {
    for (uint32_t i = 0; i < kRttPeers; ++i) {
        if (c->peers[i].addr == addr) {
            c->peers[i].last_use_ms = now;
            return &c->peers[i];
        }
    }
    RttPeer *fresh = nullptr;
    for (uint32_t i = 0; i < kRttPeers; ++i) {
        if (c->peers[i].addr == 0) { fresh = &c->peers[i]; break; }
    }
    if (fresh == nullptr) {
        RttPeer *oldest = &c->peers[0];
        for (uint32_t i = 1; i < kRttPeers; ++i) {
            if (c->peers[i].last_use_ms < oldest->last_use_ms) {
                oldest = &c->peers[i];
            }
        }
        c->st.peers_evicted++;
        fresh = oldest;
    }
    std::memset(fresh, 0, sizeof(*fresh));
    fresh->addr        = addr;
    fresh->last_use_ms = now;
    return fresh;
}

// Drop everything held for one peer.  Called on disconnect for the same
// reason dampen_purge_peer is: the game reuses one UDP socket across matches,
// so without an explicit reset a new epoch's low sequences would be matched
// against the previous match's slots.
inline void rtt_purge_peer(RttCtx *c, uint32_t addr) {
    for (uint32_t i = 0; i < kRttPeers; ++i) {
        if (c->peers[i].addr == addr) {
            std::memset(&c->peers[i], 0, sizeof(c->peers[i]));
            return;
        }
    }
}

// Record one outbound datagram.  Call AFTER the send is committed, on the
// same payload the peer will see.
inline void rtt_on_send(RttCtx *c, uint32_t peer_addr,
                        const uint8_t *pay, uint32_t len, uint64_t now) {
    if (!c->enabled || len < kReorderSeqMinPay || peer_addr == 0) return;

    const uint32_t seq = reorder_seq_from_payload(pay);
    RttPeer *p = rtt_get_peer(c, peer_addr, now);

    // Already outstanding?  Then this is a resend and the sample is spoiled.
    for (uint32_t i = 0; i < p->used; ++i) {
        RttSlot &s = p->slots[(p->head + i) % kRttSlots];
        if (s.used && s.seq == seq) {
            if (!s.ambiguous) {
                s.ambiguous = true;
                c->st.sends_ambiguous++;
            }
            return;
        }
    }

    if (p->used == kRttSlots) {
        // Ring full: the oldest outstanding sequence is forgotten.  It may
        // still be acked later, which will land in `unmatched`.
        if (p->slots[p->head].used) c->st.slots_evicted++;
        p->head = (p->head + 1) % kRttSlots;
        p->used--;
    }
    RttSlot &slot = p->slots[(p->head + p->used) % kRttSlots];
    slot.seq       = seq;
    slot.sent_ms   = now;
    slot.used      = true;
    slot.ambiguous = false;
    p->used++;
    c->st.sends_tracked++;
}

// Record one inbound datagram and, when its ack advances, produce a sample.
// Returns the RTT in ms if this datagram yielded one, else 0.
inline uint32_t rtt_on_recv(RttCtx *c, uint32_t peer_addr,
                            const uint8_t *pay, uint32_t len, uint64_t now) {
    if (!c->enabled || len < kReorderSeqMinPay || peer_addr == 0) return 0;

    const uint32_t ack = reorder_ack_from_payload(pay);
    RttPeer *p = rtt_get_peer(c, peer_addr, now);

    // The ack repeats on every datagram until the peer sees something newer.
    // Sampling on repeats would measure "how long since we sent S" over and
    // over and drag the mean up without bound, so only an advance counts.
    if (p->ack_init && seq_cmp(ack, p->last_ack) <= 0) return 0;
    p->last_ack = ack;
    p->ack_init = true;
    c->st.acks_seen++;

    // Find the acked sequence and retire everything at or below it: those are
    // confirmed delivered and can never produce another sample.
    int32_t found = -1;
    for (uint32_t i = 0; i < p->used; ++i) {
        if (p->slots[(p->head + i) % kRttSlots].seq == ack) { found = (int32_t)i; break; }
    }
    if (found < 0) { c->st.unmatched++; return 0; }

    RttSlot &s = p->slots[(p->head + (uint32_t)found) % kRttSlots];
    const bool ambiguous = s.ambiguous;
    const uint64_t sent  = s.sent_ms;

    // Retire the acked slot and everything older in one step.
    const uint32_t retire = (uint32_t)found + 1;
    for (uint32_t i = 0; i < retire; ++i) {
        p->slots[(p->head + i) % kRttSlots].used = false;
    }
    p->head = (p->head + retire) % kRttSlots;
    p->used -= retire;

    if (ambiguous)  { c->st.discarded_ambig++; return 0; }
    if (now < sent) { c->st.discarded_range++; return 0; }

    const uint64_t rtt = now - sent;
    if (rtt > kRttMaxSampleMs) { c->st.discarded_range++; return 0; }
    const uint32_t r = (uint32_t)rtt;

    c->st.samples++;
    p->samples++;
    p->sum_ms += r;
    if (p->min_ms == 0 || r < p->min_ms) p->min_ms = r;
    if (r > p->max_ms) p->max_ms = r;

    p->win_samples++;
    p->win_sum_ms += r;
    if (p->win_min_ms == 0 || r < p->win_min_ms) p->win_min_ms = r;
    if (r > p->win_max_ms) p->win_max_ms = r;

    // RFC 6298 smoothing: srtt = 7/8 srtt + 1/8 r, rttvar = 3/4 var + 1/4 |srtt-r|.
    if (!p->srtt_init) {
        p->srtt_ms   = r;
        p->rttvar_ms = r / 2;
        p->srtt_init = true;
    } else {
        const uint32_t delta = (r > p->srtt_ms) ? (r - p->srtt_ms) : (p->srtt_ms - r);
        p->rttvar_ms = (3 * p->rttvar_ms + delta) / 4;
        p->srtt_ms   = (7 * p->srtt_ms + r) / 8;
    }
    return r;
}

// True when the periodic line is due.  Kept separate from formatting so the
// caller owns its own logging cadence and locking, exactly as gov_trace does.
inline bool rtt_trace_due(RttCtx *c, uint64_t now) {
    if (!c->enabled || c->trace_ms == 0) return false;
    if (now - c->last_trace_ms < c->trace_ms) return false;
    c->last_trace_ms = now;
    return true;
}

inline void rtt_window_reset(RttCtx *c) {
    for (uint32_t i = 0; i < kRttPeers; ++i) {
        c->peers[i].win_samples = 0;
        c->peers[i].win_sum_ms  = 0;
        c->peers[i].win_min_ms  = 0;
        c->peers[i].win_max_ms  = 0;
    }
}

// One line per active peer.  Returns bytes written, 0 if no peer has a
// sample this window — silence is correct then, rather than a row of zeroes.
inline int rtt_format_trace(const RttCtx *c, uint32_t addr, char *out, size_t cap) {
    const RttPeer *p = nullptr;
    for (uint32_t i = 0; i < kRttPeers; ++i) {
        if (c->peers[i].addr == addr) { p = &c->peers[i]; break; }
    }
    if (p == nullptr || p->win_samples == 0) return 0;

    const uint8_t a = (uint8_t)(addr & 0xff);
    const uint8_t b = (uint8_t)((addr >> 8) & 0xff);
    const uint8_t cc = (uint8_t)((addr >> 16) & 0xff);
    const uint8_t d = (uint8_t)((addr >> 24) & 0xff);
    return std::snprintf(out, cap,
        "rtt_trace: peer=%u.%u.%u.%u srtt=%u ms var=%u ms "
        "(window min=%u max=%u mean=%llu over %llu samples; "
        "session min=%u max=%u over %llu) upper bound: includes the peer's ack delay",
        (unsigned)a, (unsigned)b, (unsigned)cc, (unsigned)d,
        (unsigned)p->srtt_ms, (unsigned)p->rttvar_ms,
        (unsigned)p->win_min_ms, (unsigned)p->win_max_ms,
        (unsigned long long)(p->win_sum_ms / p->win_samples),
        (unsigned long long)p->win_samples,
        (unsigned)p->min_ms, (unsigned)p->max_ms,
        (unsigned long long)p->samples);
}

// Addresses currently held, so the caller can walk peers without reaching
// into the struct.  Returns how many were written.
inline uint32_t rtt_active_peers(const RttCtx *c, uint32_t *out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kRttPeers && n < cap; ++i) {
        if (c->peers[i].addr != 0 && c->peers[i].samples > 0) out[n++] = c->peers[i].addr;
    }
    return n;
}

// Session-end counters.  The denominators matter as much as the RTT here: a
// large `unmatched` or `discarded_ambig` means the samples that survived are
// a biased subset, and the line has to make that checkable rather than
// printing a clean average over a quietly filtered population.
inline int rtt_format_stats(const RttCtx *c, char *out, size_t cap) {
    const RttStats &s = c->st;
    return std::snprintf(out, cap,
        "rtt: enabled=%d sends_tracked=%llu ambiguous=%llu slots_evicted=%llu "
        "acks=%llu samples=%llu unmatched=%llu discarded(ambig=%llu range=%llu) "
        "peers_evicted=%llu",
        c->enabled ? 1 : 0,
        (unsigned long long)s.sends_tracked,
        (unsigned long long)s.sends_ambiguous,
        (unsigned long long)s.slots_evicted,
        (unsigned long long)s.acks_seen,
        (unsigned long long)s.samples,
        (unsigned long long)s.unmatched,
        (unsigned long long)s.discarded_ambig,
        (unsigned long long)s.discarded_range,
        (unsigned long long)s.peers_evicted);
}

}  // namespace bznet

#endif  // BZ_NET_RTT_H
