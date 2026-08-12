// tests/send_dampen_test.cpp — host-side tests for shared/send_dampen.h
//
// The damper sits on the game's send path, so its failure modes are worse than
// its benefits: suppressing a distinct (peer, seq) or a first transmission
// would discard real game traffic.  These tests pin the invariants that make it
// safe to ship — only a 2nd-and-later copy of a sequence already sent inside
// its window is ever suppressed, a short or non-reliable datagram always goes,
// and a full table never causes a drop — plus the RTT-driven window and the
// backoff that bounds redundant copies.
//
//   make -C tests send_dampen_test && ./tests/send_dampen_test

#include "../shared/send_dampen.h"

#include <cstdio>
#include <cstring>

using namespace bznet;

namespace {

int g_checks = 0;
int g_failures = 0;
const char *g_case = "";

void check(bool ok, const char *what, long long got, long long want) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL [%s] %s: got %lld, want %lld\n", g_case, what, got, want);
    }
}

#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); \
                            check(a_ == b_, #a, a_, b_); } while (0)
#define CHECK_LE(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); \
                            check(a_ <= b_, #a " <= " #b, a_, b_); } while (0)
#define CHECK_GE(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); \
                            check(a_ >= b_, #a " >= " #b, a_, b_); } while (0)
#define CHECK(x)       do { bool x_ = (x); check(x_, #x, (long long)x_, 1LL); } while (0)

void begin(const char *name) {
    g_case = name;
    std::printf("- %s\n", name);
}

// Build a datagram the way the wire carries it: reliable flag in byte 0,
// sequence as u32 big-endian at offset 10.  `len` is the payload length; a
// payload shorter than kReorderSeqMinPay cannot carry a sequence.
void make_pkt(uint8_t *buf, uint32_t seq, bool reliable, uint32_t len) {
    std::memset(buf, 0, len);
    buf[0] = reliable ? kBzHdrFlagReliable : 0;
    if (len >= kReorderSeqMinPay) {
        buf[kReorderSeqOffset]     = static_cast<uint8_t>((seq >> 24) & 0xff);
        buf[kReorderSeqOffset + 1] = static_cast<uint8_t>((seq >> 16) & 0xff);
        buf[kReorderSeqOffset + 2] = static_cast<uint8_t>((seq >> 8) & 0xff);
        buf[kReorderSeqOffset + 3] = static_cast<uint8_t>(seq & 0xff);
    }
}

// Find a peer entry by address, for reading its window / rtt directly.
const DampenPeer *find_peer(const DampenCtx *c, uint32_t addr) {
    for (uint32_t i = 0; i < kDampenPeers; ++i) {
        if (c->peers[i].addr == addr) {
            return &c->peers[i];
        }
    }
    return nullptr;
}

// Test 1: the first copy of a sequence always sends.
void test_first_copy_always_sends() {
    begin("first copy of a sequence always sends");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    make_pkt(pkt, 42, true, kReorderSeqMinPay);
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, kReorderSeqMinPay, 1000), kDampenSend);
    CHECK_EQ(c.st.seen, 1u);
    CHECK_EQ(c.st.suppressed, 0u);
}

// Test 2: a second copy inside the window suppresses and counts the bytes.
void test_second_copy_inside_window_suppresses() {
    begin("second copy inside the window suppresses and counts bytes");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    make_pkt(pkt, 7, true, kReorderSeqMinPay);
    const uint32_t len = kReorderSeqMinPay;
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1000), kDampenSend);
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1010), kDampenSuppress);
    CHECK_EQ(c.st.suppressed, 1u);
    CHECK_EQ(c.st.bytes_saved, (uint64_t)len);
}

// Test 3: a copy just past the window sends, and the window doubles.
void test_copy_past_window_sends_and_doubles() {
    begin("a copy just past the window sends and doubles the window");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    make_pkt(pkt, 9, true, kReorderSeqMinPay);
    const uint32_t len = kReorderSeqMinPay;
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1000), kDampenSend);
    // First send window is the floor (no RTT estimate yet).
    const DampenPeer *p = find_peer(&c, 0x0A000001);
    CHECK_EQ(p->slots[0].window_ms, kDampenFloorMs);
    // Just past the window: sends, and the window doubles.
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1000 + kDampenFloorMs + 1),
             kDampenSend);
    CHECK_EQ(p->slots[0].window_ms, kDampenFloorMs * 2);
    CHECK_EQ(c.st.suppressed, 0u);
    // A copy now inside the doubled window is suppressed again.
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1000 + kDampenFloorMs + 2),
             kDampenSuppress);
}

// Test 3b: the window comes from a measured RTT, never a constant, and never
// falls below the floor.
void test_window_from_rtt() {
    begin("window tracks measured RTT and floors at kDampenFloorMs");
    // 80 ms ack -> rtt_ewma 80 -> window 1.2*80 = 96.
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    make_pkt(pkt, 5, true, kReorderSeqMinPay);
    const uint32_t len = kReorderSeqMinPay;
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1000), kDampenSend);
    dampen_observe_ack(&c, 0x0A000001, 5, 1080);
    const DampenPeer *p = find_peer(&c, 0x0A000001);
    CHECK_EQ(p->rtt_ewma_ms, 80u);
    CHECK_EQ(dampen_window_for(p), 96u);

    // 10 ms ack -> rtt_ewma 10 -> window 12, clamped up to the floor.
    DampenCtx d;
    dampen_init(&d, true, 1000);
    make_pkt(pkt, 6, true, kReorderSeqMinPay);
    CHECK_EQ(dampen_admit(&d, 0x0A000002, pkt, len, 1000), kDampenSend);
    dampen_observe_ack(&d, 0x0A000002, 6, 1010);
    const DampenPeer *q = find_peer(&d, 0x0A000002);
    CHECK_EQ(q->rtt_ewma_ms, 10u);
    CHECK_EQ(dampen_window_for(q), kDampenFloorMs);
}

// Test 3d: a decreasing RTT sample pulls the estimate (and window) down; the
// EWMA delta is signed, so a downward sample must not underflow and pin the
// window at the ceiling.
void test_ewma_tracks_down() {
    begin("a decreasing RTT sample tracks the window down, not to the max");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    make_pkt(pkt, 8, true, kReorderSeqMinPay);
    const uint32_t len = kReorderSeqMinPay;
    // Seed a high estimate: send seq 8, ack 300 ms later -> rtt_ewma 300.
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1000), kDampenSend);
    dampen_observe_ack(&c, 0x0A000001, 8, 1300);
    DampenPeer *p = nullptr;
    for (uint32_t i = 0; i < kDampenPeers; ++i) {
        if (c.peers[i].addr == 0x0A000001) { p = &c.peers[i]; break; }
    }
    CHECK(p != nullptr);
    CHECK_EQ(p->rtt_ewma_ms, 300u);
    CHECK_EQ(dampen_window_for(p), 360u);   // 1.2*300 = 360

    // Now a much smaller sample (10 ms) must pull the estimate down.  With the
    // old unsigned delta this underflowed and pinned the window at the max.
    make_pkt(pkt, 9, true, kReorderSeqMinPay);
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 2000), kDampenSend);
    dampen_observe_ack(&c, 0x0A000001, 9, 2010);
    // ewma: 300 + (10 - 300) >> 3 = 300 + (-290 >> 3) = 300 - 37 = 263.
    CHECK_EQ(p->rtt_ewma_ms, 263u);
    CHECK_EQ(dampen_window_for(p), 315u);   // 1.2*263 = 315.6 -> 315, tracks down
    CHECK(dampen_window_for(p) < 360u);   // strictly below the pre-sample window
}

// Test 3c: the measured engine cadence against a 60 ms window suppresses copies
// 2-5 and sends only the first.
void test_engine_cadence() {
    begin("measured +12/+11/+10/+10 ms cadence suppresses copies 2-5");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    make_pkt(pkt, 3, true, kReorderSeqMinPay);
    const uint32_t len = kReorderSeqMinPay;
    uint64_t t = 1000;
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, t), kDampenSend);
    t += 12; CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, t), kDampenSuppress);
    t += 11; CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, t), kDampenSuppress);
    t += 10; CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, t), kDampenSuppress);
    t += 10; CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, t), kDampenSuppress);
    CHECK_EQ(c.st.suppressed, 4u);
    CHECK_EQ(c.st.bytes_saved, 4u * len);
}

// Test 4: backoff saturates at kDampenMaxMs and does not overflow.
void test_backoff_saturates() {
    begin("backoff saturates at kDampenMaxMs");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    make_pkt(pkt, 11, true, kReorderSeqMinPay);
    const uint32_t len = kReorderSeqMinPay;
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1000), kDampenSend);
    const DampenPeer *p = find_peer(&c, 0x0A000001);
    // Each send past the window doubles the window; step far enough that it
    // would overflow a naive uint32 if not capped.
    uint64_t t = 1000;
    uint32_t window = kDampenFloorMs;
    for (int i = 0; i < 40; ++i) {
        t += window + 1;   // just past the current window
        CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, t), kDampenSend);
        window *= 2;
        if (window > kDampenMaxMs) {
            window = kDampenMaxMs;
        }
        CHECK_EQ(p->slots[0].window_ms, window);
    }
    CHECK_EQ(p->slots[0].window_ms, kDampenMaxMs);
}

// Test 5: a payload too short to carry a sequence always sends.
void test_short_payload_always_sends() {
    begin("a payload shorter than kReorderSeqMinPay always sends");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay - 1];
    make_pkt(pkt, 0, true, kReorderSeqMinPay - 1);
    for (int i = 0; i < 10; ++i) {
        CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, kReorderSeqMinPay - 1, 1000 + i),
                 kDampenSend);
    }
    CHECK_EQ(c.st.bypass_short, 10u);
    CHECK_EQ(c.st.suppressed, 0u);
}

// Test 6: a payload without the reliable bit always sends.
void test_non_reliable_always_sends() {
    begin("a payload without the reliable bit always sends");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    make_pkt(pkt, 4, false, kReorderSeqMinPay);
    for (int i = 0; i < 10; ++i) {
        CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, kReorderSeqMinPay, 1000 + i),
                 kDampenSend);
    }
    CHECK_EQ(c.st.bypass_notretx, 10u);
    CHECK_EQ(c.st.suppressed, 0u);
}

// Test 7: two peers with the same sequence number do not interfere.
void test_two_peers_same_seq() {
    begin("two peers with the same sequence do not interfere");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    make_pkt(pkt, 21, true, kReorderSeqMinPay);
    const uint32_t len = kReorderSeqMinPay;
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1000), kDampenSend);
    CHECK_EQ(dampen_admit(&c, 0x0A000002, pkt, len, 1000), kDampenSend);  // distinct peer
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1010), kDampenSuppress); // own repeat
    CHECK_EQ(dampen_admit(&c, 0x0A000002, pkt, len, 1010), kDampenSuppress); // own repeat
    CHECK_EQ(c.st.suppressed, 2u);
}

// Test 8: ring overflow sends and counts tbl_full; nothing is suppressed on a
// full table.
void test_ring_overflow_sends() {
    begin("ring overflow sends and counts tbl_full");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    // Fill the ring with distinct sequences (each a first send), all at the same
    // time so their windows stay active.
    for (uint32_t s = 0; s < kDampenSlots; ++s) {
        make_pkt(pkt, s, true, kReorderSeqMinPay);
        CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, kReorderSeqMinPay, 1000),
                 kDampenSend);
    }
    CHECK_EQ(c.st.tbl_full, 0u);
    // One more distinct sequence overflows the ring: sends, counts tbl_full.
    make_pkt(pkt, kDampenSlots, true, kReorderSeqMinPay);
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, kReorderSeqMinPay, 1000),
             kDampenSend);
    CHECK_EQ(c.st.tbl_full, 1u);
    // A repeat of a sequence still in the ring is suppressed normally.  The
    // overflow evicted seq 0, so seq 1 is the oldest survivor and is still
    // inside its window.
    make_pkt(pkt, 1, true, kReorderSeqMinPay);
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, kReorderSeqMinPay, 1001),
             kDampenSuppress);
}

// Test 9: peer-table overflow evicts the LRU, counts peers_evicted, still sends.
void test_peer_table_overflow_evicts_lru() {
    begin("peer-table overflow evicts LRU and still sends");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    make_pkt(pkt, 1, true, kReorderSeqMinPay);
    const uint32_t len = kReorderSeqMinPay;
    // Touch peers 0..kDampenPeers-1 in order; peer 0 is the LRU.
    for (uint32_t i = 0; i < kDampenPeers; ++i) {
        const uint32_t addr = 0x0A000100 + i;
        CHECK_EQ(dampen_admit(&c, addr, pkt, len, 1000 + i), kDampenSend);
    }
    // Re-touch peer 1 so peer 0 is now the least recently used.
    CHECK_EQ(dampen_admit(&c, 0x0A000101, pkt, len, 2000), kDampenSend);
    // A new peer must evict peer 0 (the LRU), count it, and still send.
    CHECK_EQ(dampen_admit(&c, 0x0A000200, pkt, len, 2001), kDampenSend);
    CHECK_EQ(c.st.peers_evicted, 1u);
    CHECK(find_peer(&c, 0x0A000100) == nullptr);   // peer 0 evicted
    CHECK(find_peer(&c, 0x0A000200) != nullptr);   // new peer present
}

// Test 10: the epoch reset is pure in-band.  It fires only when a sequence
// jumps backward BELOW the ring's oldest retained sequence (a restart-near-zero
// always does); a retransmit of a sequence still in the ring is never below the
// oldest and is a live duplicate, not a restart.
void test_sequence_epoch_reset() {
    begin("a peer restarting at a low sequence is not suppressed");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    make_pkt(pkt, 3, true, kReorderSeqMinPay);
    const uint32_t len = kReorderSeqMinPay;
    // First epoch: send seq 3, then push the high-water mark far ahead so the
    // peer looks like it reached a very high sequence.
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1000), kDampenSend);
    DampenPeer *p = nullptr;
    for (uint32_t i = 0; i < kDampenPeers; ++i) {
        if (c.peers[i].addr == 0x0A000001) { p = &c.peers[i]; break; }
    }
    CHECK(p != nullptr);
    p->high_seq = 0x70000000u;
    // The ring still holds the stale seq 3 entry; its oldest retained sequence
    // is 3.  A retransmit of seq 3 is not below the oldest, so it is a live
    // duplicate inside its window — suppressed, and the reset must NOT fire.
    CHECK_EQ(p->used, 1u);
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1001), kDampenSuppress);
    CHECK_EQ(p->used, 1u);   // ring untouched: a retransmit is not a restart
    // Restart-near-zero: a fresh low sequence (0) below the ring's oldest (3)
    // fires the reset, clears the stale ring, and sends rather than being
    // suppressed against the stale high-water mark.
    make_pkt(pkt, 0, true, kReorderSeqMinPay);
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1002), kDampenSend);
    CHECK_EQ(c.st.suppressed, 1u);   // only the retransmit was suppressed
    CHECK_EQ(p->used, 1u);   // ring was cleared, then re-recorded seq 0
    CHECK_EQ(p->high_seq, 0u);
}

// Test 11: dampen_purge_peer clears only that peer.
void test_purge_peer() {
    begin("dampen_purge_peer clears only that peer");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    make_pkt(pkt, 2, true, kReorderSeqMinPay);
    const uint32_t len = kReorderSeqMinPay;
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1000), kDampenSend);
    CHECK_EQ(dampen_admit(&c, 0x0A000002, pkt, len, 1000), kDampenSend);
    dampen_purge_peer(&c, 0x0A000001);
    CHECK(find_peer(&c, 0x0A000001) == nullptr);
    CHECK(find_peer(&c, 0x0A000002) != nullptr);
    // The purged peer starts fresh: its seq 2 sends again.
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1001), kDampenSend);
    // The untouched peer still suppresses its own repeat.
    CHECK_EQ(dampen_admit(&c, 0x0A000002, pkt, len, 1001), kDampenSuppress);
}

// Test 12: enabled=false sends everything and touches no counters except seen.
void test_disabled() {
    begin("enabled=false sends everything and only counts seen");
    DampenCtx c;
    dampen_init(&c, false, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    make_pkt(pkt, 2, true, kReorderSeqMinPay);
    const uint32_t len = kReorderSeqMinPay;
    for (int i = 0; i < 20; ++i) {
        CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1000 + i), kDampenSend);
    }
    CHECK_EQ(c.st.seen, 20u);
    CHECK_EQ(c.st.suppressed, 0u);
    CHECK_EQ(c.st.bytes_saved, 0u);
    CHECK_EQ(c.st.peers_evicted, 0u);
    CHECK_EQ(c.st.bypass_short, 0u);
    CHECK_EQ(c.st.bypass_notretx, 0u);
    CHECK_EQ(c.st.tbl_full, 0u);
}

}  // namespace

int main() {
    std::printf("send_dampen V4.9 tests\n");
    test_first_copy_always_sends();
    test_second_copy_inside_window_suppresses();
    test_copy_past_window_sends_and_doubles();
    test_window_from_rtt();
    test_ewma_tracks_down();
    test_engine_cadence();
    test_backoff_saturates();
    test_short_payload_always_sends();
    test_non_reliable_always_sends();
    test_two_peers_same_seq();
    test_ring_overflow_sends();
    test_peer_table_overflow_evicts_lru();
    test_sequence_epoch_reset();
    test_purge_peer();
    test_disabled();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}