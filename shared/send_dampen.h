// shared/send_dampen.h — per-(peer,seq) duplicate suppressor.
//
// BZRNet's reliable retry timer is fixed at ~10 ms with no backoff, against an
// RTT the game itself reports as 56–91 ms.  Every reliable message therefore
// goes out 6–9 times before an acknowledgement can physically return — an
// engine defect nobody outside Rebellion can fix, and one that applies to all
// reliable traffic, always.  See resources/CAMERAPOD_STORM.md, §A5.
//
// This header answers one question per outbound datagram: *have I already sent
// this exact (peer, sequence) recently enough that this copy is redundant?*  If
// yes, the caller drops it.  Only 2nd-and-later copies of a sequence already
// sent are ever suppressed; a distinct (peer, seq), a datagram too short to
// carry a sequence, and a first transmission always go.
//
// It is a pure state machine, mirroring send_pace.h: the caller owns the lock,
// supplies the clock, and performs the real send.  No threads, no syscalls, no
// globals, no allocation — fixed-size storage only, because the proxies run
// inside a game process and an allocation on the send path is not acceptable.
// Covered by tests/send_dampen_test.cpp.
//
// The suppression window comes from a measured per-peer RTT estimate, not a
// constant: the right window is "long enough that an ack had a fair chance",
// which is a property of the link.  It is clamped to [kDampenFloorMs,
// kDampenMaxMs] and never allowed below the floor — a window shorter than RTT
// reproduces the very bug this corrects.

#ifndef BZNET_SEND_DAMPEN_H
#define BZNET_SEND_DAMPEN_H

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "reorder_core.h"   // reorder_is_reliable, reorder_seq_from_payload,
                            // kReorderSeqMinPay, seq_cmp

namespace bznet {

constexpr uint32_t kDampenPeers    = 16;   // same order as reorder's peer table
constexpr uint32_t kDampenSlots    = 64;   // per-peer ring of recent sequences
constexpr uint32_t kDampenFloorMs  = 60;   // never suppress on a shorter window
constexpr uint32_t kDampenMaxMs    = 400;  // backoff ceiling
constexpr uint32_t kDampenRttShift = 3;    // ewma: rtt += (sample - rtt) >> 3

// The untracked/invalid socket sentinel, mirroring Winsock's INVALID_SOCKET
// ((SOCKET)(~0)).  The proxy stores the P2P socket here until it is known, and
// clears it back here when the dampened session ends.  A handle equal to this
// (or zero) is "not a tracked dampen socket".
constexpr unsigned long long kDampenInvalidSock = ~0ULL;

enum DampenDecision { kDampenSend, kDampenSuppress };

struct DampenEntry {
    uint32_t seq;
    uint64_t last_sent_ms;
    uint32_t window_ms;
};

struct DampenPeer {
    uint32_t addr;          // IPv4 raw, network order; 0 = empty
    uint32_t rtt_ewma_ms;
    uint32_t high_seq;      // high-water mark, drives epoch detection
    uint32_t used;          // occupied slots
    uint32_t head;          // ring head (oldest slot)
    uint64_t last_use_ms;   // LRU for peer eviction
    DampenEntry slots[kDampenSlots];
};

struct DampenStats {
    uint64_t seen;
    uint64_t suppressed;
    uint64_t bytes_saved;
    uint64_t peers_evicted;
    uint64_t bypass_short;
    uint64_t bypass_notretx;
    uint64_t tbl_full;
};

struct DampenCtx {
    bool enabled;
    DampenPeer peers[kDampenPeers];
    DampenStats st;
};

// The suppression window for a peer: 1.2 * RTT estimate, clamped to
// [kDampenFloorMs, kDampenMaxMs].  Before any estimate exists (rtt_ewma == 0)
// this is the floor, so nothing is suppressed below it.
inline uint32_t dampen_window_for(const DampenPeer *p) {
    uint32_t w = (p->rtt_ewma_ms * 12) / 10;   // 1.2 * rtt
    if (w < kDampenFloorMs) w = kDampenFloorMs;
    if (w > kDampenMaxMs)   w = kDampenMaxMs;
    return w;
}

// Look up or create the peer entry for addr.  When the table is full the
// least-recently-used entry is evicted and counted; a peer is never refused.
inline DampenPeer *dampen_get_peer(DampenCtx *c, uint32_t addr, uint64_t now) {
    for (uint32_t i = 0; i < kDampenPeers; ++i) {
        if (c->peers[i].addr == addr) {
            c->peers[i].last_use_ms = now;
            return &c->peers[i];
        }
    }
    DampenPeer *fresh = nullptr;
    for (uint32_t i = 0; i < kDampenPeers; ++i) {
        if (c->peers[i].addr == 0) {
            fresh = &c->peers[i];
            break;
        }
    }
    if (fresh == nullptr) {
        DampenPeer *oldest = &c->peers[0];
        for (uint32_t i = 1; i < kDampenPeers; ++i) {
            if (c->peers[i].last_use_ms < oldest->last_use_ms) {
                oldest = &c->peers[i];
            }
        }
        c->st.peers_evicted++;
        fresh = oldest;
    }
    std::memset(fresh, 0, sizeof(*fresh));
    fresh->addr       = addr;
    fresh->last_use_ms = now;
    return fresh;
}

// Add a slot to the peer's ring.  On overflow the oldest slot is evicted and
// reused; the caller counts tbl_full.  Returns the slot to fill.
inline DampenEntry *dampen_add_slot(DampenPeer *p) {
    if (p->used < kDampenSlots) {
        DampenEntry *e = &p->slots[(p->head + p->used) % kDampenSlots];
        p->used++;
        return e;
    }
    DampenEntry *e = &p->slots[p->head];
    p->head = (p->head + 1) % kDampenSlots;
    return e;
}

// Initialise the table.  The caller supplies the clock for symmetry with the
// other hooks; it is not needed at init.
inline void dampen_init(DampenCtx *c, bool enabled, uint64_t now) {
    std::memset(c, 0, sizeof(*c));
    c->enabled = enabled;
    (void)now;
}

// Decide whether one outbound datagram is a redundant copy.  Returns
// kDampenSuppress only for a 2nd-or-later copy of a (peer, seq) already sent
// inside its window; everything else returns kDampenSend.
inline DampenDecision dampen_admit(DampenCtx *c, uint32_t peer_addr,
                                   const uint8_t *pay, uint32_t len, uint64_t now) {
    c->st.seen++;
    if (!c->enabled) {
        return kDampenSend;
    }
    if (len < kReorderSeqMinPay) {
        c->st.bypass_short++;
        return kDampenSend;
    }
    if (!reorder_is_reliable(pay)) {
        c->st.bypass_notretx++;
        return kDampenSend;
    }
    const uint32_t seq = reorder_seq_from_payload(pay);

    DampenPeer *p = dampen_get_peer(c, peer_addr, now);

    // Search the ring first so a live duplicate can be told apart from a
    // restart: a retransmit of a message still inside its window is genuine
    // traffic, not a new epoch.
    int32_t found = -1;
    for (uint32_t i = 0; i < p->used; ++i) {
        if (p->slots[(p->head + i) % kDampenSlots].seq == seq) {
            found = (int32_t)i;
            break;
        }
    }

    // Epoch reset: the peer reconnected and restarted its counter near zero.
    // The criterion is pure in-band — no liveness check, no gap threshold.  A
    // sequence that jumps backward BELOW the ring's oldest retained sequence
    // can only be a restart: a retransmit of a message still in the ring is
    // never below the oldest still-retained (it is one of the retained
    // sequences), while a restart-near-zero is.  The ring itself is the memory,
    // so a live duplicate is necessarily inside the retained set and never
    // triggers the reset.
    // Clear the ring so a fresh low sequence is not suppressed against a stale
    // high one, and reset high_seq so the reset does not re-fire on every
    // subsequent packet and disable suppression.
    if (p->used > 0) {
        const uint32_t oldest = p->slots[p->head].seq;
        if (seq_cmp(seq, oldest) < 0) {
            p->used     = 0;
            p->head     = 0;
            p->high_seq = seq;
            found       = -1;   // ring was cleared; the sequence is unrecorded
        }
    }
    if (seq_cmp(seq, p->high_seq) > 0) {
        p->high_seq = seq;
    }

    if (found < 0) {
        // First copy of this sequence: record it and always send.
        if (p->used >= kDampenSlots) {
            c->st.tbl_full++;
        }
        DampenEntry *e = dampen_add_slot(p);
        e->seq          = seq;
        e->last_sent_ms = now;
        e->window_ms    = dampen_window_for(p);
        return kDampenSend;
    }

    DampenEntry &e = p->slots[(p->head + (uint32_t)found) % kDampenSlots];
    if (now - e.last_sent_ms < e.window_ms) {
        // Inside the window: redundant copy.  Leave last_sent_ms alone so the
        // window is measured from the last *actual* send, not the last attempt.
        c->st.suppressed++;
        c->st.bytes_saved += len;
        return kDampenSuppress;
    }
    // Past the window: a genuine loss-recovery retransmit gets through.  Double
    // the backoff window, capped at kDampenMaxMs.
    e.last_sent_ms = now;
    e.window_ms *= 2;
    if (e.window_ms > kDampenMaxMs) {
        e.window_ms = kDampenMaxMs;
    }
    return kDampenSend;
}

// Feed an inbound acknowledgement: the ring entry whose seq == acked_seq (exact
// sequence we sent gives a true RTT sample, which updates the peer's ewma.
// Seeded from the first sample so a single ack already yields a usable window.
inline void dampen_observe_ack(DampenCtx *c, uint32_t peer_addr,
                               uint32_t acked_seq, uint64_t now) {
    if (!c->enabled) {
        return;
    }
    DampenPeer *p = nullptr;
    for (uint32_t i = 0; i < kDampenPeers; ++i) {
        if (c->peers[i].addr == peer_addr) {
            p = &c->peers[i];
            break;
        }
    }
    if (p == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < p->used; ++i) {
        DampenEntry &e = p->slots[(p->head + i) % kDampenSlots];
        if (e.seq == acked_seq) {
            const uint32_t sample = (now > e.last_sent_ms)
                ? static_cast<uint32_t>(now - e.last_sent_ms) : 0;
            if (p->rtt_ewma_ms == 0) {
                p->rtt_ewma_ms = sample;
            } else {
                // The delta is signed: a downward sample must be able to pull the
                // estimate down.  Computed in int32_t so an unsigned subtraction
                // does not underflow, wrap to a huge positive, and pin the window
                // at the ceiling.
                const int32_t delta = (int32_t)sample - (int32_t)p->rtt_ewma_ms;
                p->rtt_ewma_ms = (uint32_t)((int32_t)p->rtt_ewma_ms +
                                            (delta >> kDampenRttShift));
            }
            break;
        }
    }
}

// Forget everything about one peer (it disconnected).  Other peers are
// untouched; cumulative stats survive.
inline void dampen_purge_peer(DampenCtx *c, uint32_t peer_addr) {
    for (uint32_t i = 0; i < kDampenPeers; ++i) {
        if (c->peers[i].addr == peer_addr) {
            std::memset(&c->peers[i], 0, sizeof(c->peers[i]));
            break;
        }
    }
}

// The single source of truth for "this close ends the dampened session".
// Returns true IFF `dampen_sock` is a valid tracked handle (non-zero and not
// the invalid sentinel) AND `closing == dampen_sock`.  A close of any other
// socket — the lobby/discovery socket, or any socket before the P2P socket is
// even known — must NOT purge the peer ring, which is exactly the regression
// this guards against.
inline bool dampen_close_ends_session(unsigned long long closing,
                                      unsigned long long dampen_sock) {
    if (dampen_sock == 0 || dampen_sock == kDampenInvalidSock) {
        return false;
    }
    return closing == dampen_sock;
}

inline int dampen_format_stats(const DampenCtx *c, char *buf, size_t n) {
    const DampenStats &s = c->st;
    return std::snprintf(buf, n,
        "dampen: enabled=%d seen=%llu suppressed=%llu bytes_saved=%llu"
        " peers_evicted=%llu bypass_short=%llu bypass_notretx=%llu tbl_full=%llu",
        c->enabled ? 1 : 0,
        static_cast<unsigned long long>(s.seen),
        static_cast<unsigned long long>(s.suppressed),
        static_cast<unsigned long long>(s.bytes_saved),
        static_cast<unsigned long long>(s.peers_evicted),
        static_cast<unsigned long long>(s.bypass_short),
        static_cast<unsigned long long>(s.bypass_notretx),
        static_cast<unsigned long long>(s.tbl_full));
}

}  // namespace bznet

#endif  // BZNET_SEND_DAMPEN_H