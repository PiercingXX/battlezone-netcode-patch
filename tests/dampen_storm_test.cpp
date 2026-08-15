// tests/dampen_storm_test.cpp — host-side multi-sequence storm test (rule 6)
// for shared/send_dampen.h.
//
// The shipped bug this test reproduces: when two reliable sequences are in
// flight concurrently and one is retransmitted past its own window while the
// other is still inside it, a naive epoch heuristic can mistake the older
// sequence's backward retransmit for a restart and wipe the ring — which then
// lets the still-in-flight sequence's redundant copies through and disables
// the very suppression the damper exists to provide.
//
// The fix keys the epoch reset on a sequence jumping backward BELOW the ring's
// OLDEST retained sequence.  A retransmit of a message still in the ring is one
// of the retained sequences, so it can never be below the oldest and never
// triggers the reset.  These tests pin that: the ring survives, the in-flight
// peer's redundant copy is still suppressed, high_seq never walks backward, and
// the backoff-doubling branch stays reachable for a sequence below high_seq.
//
//   make -C tests dampen_storm_test && ./tests/dampen_storm_test

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

void begin(const char *name) {
    g_case = name;
    std::printf("- %s\n", name);
}

// Build a reliable datagram carrying `seq` as u32 big-endian at offset 10.
void make_pkt(uint8_t *buf, uint32_t seq) {
    const uint32_t len = kReorderSeqMinPay;
    std::memset(buf, 0, len);
    buf[0] = kBzHdrFlagReliable;
    buf[kReorderSeqOffset]     = static_cast<uint8_t>((seq >> 24) & 0xff);
    buf[kReorderSeqOffset + 1] = static_cast<uint8_t>((seq >> 16) & 0xff);
    buf[kReorderSeqOffset + 2] = static_cast<uint8_t>((seq >> 8) & 0xff);
    buf[kReorderSeqOffset + 3] = static_cast<uint8_t>(seq & 0xff);
}

// Test: two concurrently-retried sequences for one peer.  seq1 and seq2 are
// both first-sent; then seq1 is retransmitted past its own window while seq2 is
// still in flight.  Assert ALL of: the ring is NOT wiped, seq2's redundant
// in-window copy IS suppressed, high_seq does not walk backward, and the
// backoff-doubling branch stays reachable for a sequence below high_seq (seq1's
// window doubles 60 -> 120 ms rather than resetting to the floor).
void test_two_sequence_storm() {
    begin("two concurrent retries: ring survives, in-flight copy suppressed, backoff doubles");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    const uint32_t len = kReorderSeqMinPay;
    const uint32_t addr = 0x0A000001;

    // Both sequences first-sent.  rtt_ewma is 0, so each gets the floor window
    // of 60 ms.  seq1 at t=1000, seq2 at t=1010.
    make_pkt(pkt, 100);
    CHECK_EQ(dampen_admit(&c, addr, pkt, len, 1000), kDampenSend);
    make_pkt(pkt, 101);
    CHECK_EQ(dampen_admit(&c, addr, pkt, len, 1010), kDampenSend);

    DampenPeer *p = nullptr;
    for (uint32_t i = 0; i < kDampenPeers; ++i) {
        if (c.peers[i].addr == addr) { p = &c.peers[i]; break; }
    }
    CHECK(p != nullptr);
    CHECK_EQ(p->used, 2u);          // both sequences retained
    CHECK_EQ(p->high_seq, 101u);    // high-water mark at the newer sequence

    // seq1 retransmitted past its own window (61 ms >= 60 ms floor) while seq2
    // is still in flight (sent at 1010, window 60, in-window until 1070).
    // This is the exact interaction the single-sequence tests missed: a naive
    // epoch heuristic could mistake seq1's backward retransmit for a restart
    // and wipe the ring.
    make_pkt(pkt, 100);
    CHECK_EQ(dampen_admit(&c, addr, pkt, len, 1061), kDampenSend);

    // The ring survived the retransmit: both sequences still retained.
    CHECK_EQ(p->used, 2u);
    // The backoff-doubling branch fired for a sequence below high_seq: seq1's
    // window doubled 60 -> 120 ms rather than resetting to the floor.  seq1 is
    // the oldest slot (head == 0), so slots[0] is its entry.
    CHECK_EQ(p->slots[0].window_ms, 120u);
    // high_seq did not walk backward.
    CHECK_EQ(p->high_seq, 101u);

    // seq2's redundant in-window copy IS suppressed (55 ms < 60 ms window).
    make_pkt(pkt, 101);
    CHECK_EQ(dampen_admit(&c, addr, pkt, len, 1065), kDampenSuppress);
    CHECK_EQ(c.st.suppressed, 1u);
    // Ring still intact after the suppression.
    CHECK_EQ(p->used, 2u);
}

}  // namespace

int main() {
    std::printf("dampen storm tests\n");
    test_two_sequence_storm();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}