// tests/dampen_rtt_feed_test.cpp — the RTT feed into the damper's window
//
// Game 3 on 2026-08-15 leaked ~11,000 retransmit copies through a damper
// whose design says roughly 3 per message should pass.  Two mechanisms, both
// pinned here:
//
//   1. dampen_observe_ack existed and was never called from either proxy, so
//      rtt_ewma_ms stayed 0 and every window sat at the 60 ms floor — 3x the
//      copies the 1.2xRTT design window allows at the measured 149 ms RTT.
//      dampen_set_rtt is the wiring fix; these tests pin that a fed RTT
//      actually widens the window.
//
//   2. The 64-slot ring held ~6 s of the storm's sequence churn while
//      retries spanned 9.5 s, so retries of evicted sequences fell below the
//      ring's oldest retained sequence and fired the epoch reset as if the
//      peer had restarted — wiping suppression state invisibly.  The ring is
//      now 512 slots and the wipes are counted (epoch_resets), and the test
//      here reproduces the false-reset cascade at the OLD size to show what
//      the resize prevents.
//
//   make -C tests run

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
#define CHECK(x)       do { bool x_ = (x); check(x_, #x, (long long)x_, 1LL); } while (0)

void begin(const char *name) { g_case = name; std::printf("- %s\n", name); }

// Reliable datagram with the real header layout (see BZ_P2P_HEADER.md).
void make_pkt(uint8_t *buf, uint32_t seq) {
    std::memset(buf, 0, kReorderSeqMinPay);
    buf[0] = 0xC0;
    buf[kReorderSeqOffset]     = (uint8_t)(seq >> 24);
    buf[kReorderSeqOffset + 1] = (uint8_t)(seq >> 16);
    buf[kReorderSeqOffset + 2] = (uint8_t)(seq >> 8);
    buf[kReorderSeqOffset + 3] = (uint8_t)seq;
}

const uint32_t kPeer = 0x0A000001;
const uint32_t kLen  = kReorderSeqMinPay;

// Before any RTT is known the window is the floor: a copy 70 ms after the
// first send passes.  With a fed 149 ms RTT the window is 1.2x that = 178 ms,
// and the same 70 ms copy is suppressed.  This is the 3x difference the
// unwired feed cost.
void test_fed_rtt_widens_window() {
    begin("a fed RTT widens the window from the floor to 1.2 x RTT");
    uint8_t pkt[64];
    DampenCtx c;
    dampen_init(&c, true, 0);

    make_pkt(pkt, 100);
    CHECK_EQ(dampen_admit(&c, kPeer, pkt, kLen, 1000), kDampenSend);
    // 70 ms later, floor window (60 ms) has expired: the copy passes.
    CHECK_EQ(dampen_admit(&c, kPeer, pkt, kLen, 1070), kDampenSend);

    DampenCtx d;
    dampen_init(&d, true, 0);
    make_pkt(pkt, 100);
    CHECK_EQ(dampen_admit(&d, kPeer, pkt, kLen, 1000), kDampenSend);
    dampen_set_rtt(&d, kPeer, 149);          // the 2026-08-15 measured RTT
    make_pkt(pkt, 101);                       // new seq picks up the new window
    CHECK_EQ(dampen_admit(&d, kPeer, pkt, kLen, 1010), kDampenSend);
    // 70 ms after seq 101's send: inside the 178 ms window now - suppressed.
    CHECK_EQ(dampen_admit(&d, kPeer, pkt, kLen, 1080), kDampenSuppress);
    CHECK_EQ(d.st.suppressed, 1u);
}

// The feed only updates peers the send path created; it must not claim slots.
void test_feed_does_not_create_peers() {
    begin("feeding an unknown peer creates nothing");
    DampenCtx c;
    dampen_init(&c, true, 0);
    dampen_set_rtt(&c, kPeer, 149);
    for (uint32_t i = 0; i < kDampenPeers; ++i) {
        CHECK_EQ(c.peers[i].addr, 0u);
    }
}

// A zero RTT (no samples yet) must not zero an existing estimate.
void test_zero_rtt_ignored() {
    begin("a zero RTT is ignored rather than resetting the window");
    uint8_t pkt[64];
    DampenCtx c;
    dampen_init(&c, true, 0);
    make_pkt(pkt, 100);
    dampen_admit(&c, kPeer, pkt, kLen, 1000);
    dampen_set_rtt(&c, kPeer, 149);
    dampen_set_rtt(&c, kPeer, 0);
    CHECK_EQ(c.peers[0].rtt_ewma_ms, 149u);
}

// The false-reset cascade this change closes.  Simulated at the OLD ring
// size by exhausting the current one: fill the ring past capacity, then retry
// an evicted sequence.  Before 2026-08-15 this wiped the ring silently; now
// it is at least counted.  With the ring at 512 the G3 storm (659 new seqs
// per minute, 9.5 s retry span) no longer reaches this path at all.
void test_evicted_retry_fires_counted_reset() {
    begin("a retry of an evicted sequence fires the (now counted) epoch reset");
    uint8_t pkt[64];
    DampenCtx c;
    dampen_init(&c, true, 0);
    for (uint32_t s = 0; s <= kDampenSlots; ++s) {   // one past capacity
        make_pkt(pkt, s + 10);
        dampen_admit(&c, kPeer, pkt, kLen, 1000 + s);
    }
    CHECK_EQ(c.st.tbl_full, 1u);
    CHECK_EQ(c.st.epoch_resets, 0u);
    // Seq 10 was the first recorded and has been evicted.  Its retry is below
    // the ring's oldest retained sequence: indistinguishable from a restart.
    make_pkt(pkt, 10);
    CHECK_EQ(dampen_admit(&c, kPeer, pkt, kLen, 3000), kDampenSend);
    CHECK_EQ(c.st.epoch_resets, 1u);
    CHECK_EQ(c.peers[0].used, 1u);   // the wipe: everything else is gone
}

// The G3 storm shape against the NEW ring: sequence churn at the measured
// rate, retries spanning 9.5 s.  Every retry must be found and suppressed -
// no evictions, no resets.
void test_storm_shape_fits_new_ring() {
    begin("the 2026-08-15 storm's churn fits the resized ring");
    uint8_t pkt[64];
    DampenCtx c;
    dampen_init(&c, true, 0);
    dampen_set_rtt(&c, kPeer, 149);
    // 11 new seqs/sec for 40 s = 440 distinct, interleaved with a retry of a
    // sequence 9.5 s old every second (the p99 retry span).
    uint64_t now = 1000;
    uint32_t seq = 0;
    uint64_t suppressed_before = 0;
    for (uint32_t sec = 0; sec < 40; ++sec) {
        for (uint32_t k = 0; k < 11; ++k) {
            make_pkt(pkt, seq++);
            dampen_admit(&c, kPeer, pkt, kLen, now);
            now += 30;
        }
        if (sec >= 10) {
            // retry something from ~9.5 s ago - about 105 seqs back
            make_pkt(pkt, seq - 105);
            suppressed_before = c.st.suppressed;
            dampen_admit(&c, kPeer, pkt, kLen, now);
            // Past its window (9.5 s >> 400 ms cap) so it SENDS - the point
            // is that it was FOUND: no eviction cascade, no reset.
            CHECK_EQ(c.st.suppressed, suppressed_before);
        }
        now += 1000 - 11 * 30;
    }
    CHECK_EQ(c.st.tbl_full, 0u);
    CHECK_EQ(c.st.epoch_resets, 0u);
}

// A genuine reconnect must still fire the reset exactly as before - the
// dampen_epoch_test behavior is untouched, this just pins the counter.
void test_genuine_restart_still_resets() {
    begin("a genuine restart-near-zero still fires the reset, now counted");
    uint8_t pkt[64];
    DampenCtx c;
    dampen_init(&c, true, 0);
    for (uint32_t s = 10; s <= 12; ++s) {
        make_pkt(pkt, s);
        dampen_admit(&c, kPeer, pkt, kLen, 1000 + s);
    }
    make_pkt(pkt, 0);
    CHECK_EQ(dampen_admit(&c, kPeer, pkt, kLen, 2000), kDampenSend);
    CHECK_EQ(c.st.epoch_resets, 1u);
    CHECK_EQ(c.peers[0].high_seq, 0u);
}

// The stats line must carry the new counter.
void test_stats_line_reports_resets() {
    begin("the stats line reports epoch_resets");
    DampenCtx c;
    dampen_init(&c, true, 0);
    char line[512];
    CHECK(dampen_format_stats(&c, line, sizeof(line)) > 0);
    CHECK(std::strstr(line, "epoch_resets=0") != nullptr);
}

}  // namespace

int main() {
    std::printf("dampen_rtt_feed_test\n");
    test_fed_rtt_widens_window();
    test_feed_does_not_create_peers();
    test_zero_rtt_ignored();
    test_evicted_retry_fires_counted_reset();
    test_storm_shape_fits_new_ring();
    test_genuine_restart_still_resets();
    test_stats_line_reports_resets();

    std::printf("%s: %d checks, %d failures\n",
                g_failures ? "FAILED" : "ok", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
