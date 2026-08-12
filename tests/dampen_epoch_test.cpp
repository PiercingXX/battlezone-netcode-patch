// tests/dampen_epoch_test.cpp — host-side tests for the epoch-reset logic in
// shared/send_dampen.h.
//
// The damper's epoch reset is what lets a peer that reconnects and restarts its
// sequence counter near zero keep working: without it, a fresh low sequence is
// suppressed against a stale high-water mark and the new epoch never sends.
// These tests pin the two invariants that make the reset safe:
//
//   1. a small backward step (a peer restarting near zero) fires the reset and
//      sends — the reset fires when a sequence jumps below the ring's oldest
//      retained sequence, which a restart-near-zero always does; and
//   2. when the reset fires it resets high_seq, so it does not re-fire on every
//      subsequent packet and disable suppression for the whole new epoch.
//
//   make -C tests dampen_epoch_test && ./tests/dampen_epoch_test

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

// Test 1: a peer restarting near zero — a small backward step below the ring's
// oldest retained sequence — must fire the epoch reset and send, not be
// suppressed against a stale low high-water mark.
void test_restart_near_zero() {
    begin("a peer restarting near zero (small backward step) fires the epoch reset");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    const uint32_t len = kReorderSeqMinPay;
    // First epoch: a few low sequences.  high_seq ends near zero (12), so a
    // restart at seq 0 is a small backward step below the ring's oldest
    // retained sequence (10) — the reset must fire on that in-band criterion.
    for (uint32_t s = 10; s <= 12; ++s) {
        make_pkt(pkt, s);
        CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1000 + s), kDampenSend);
    }
    DampenPeer *p = nullptr;
    for (uint32_t i = 0; i < kDampenPeers; ++i) {
        if (c.peers[i].addr == 0x0A000001) { p = &c.peers[i]; break; }
    }
    CHECK(p != nullptr);
    CHECK_EQ(p->high_seq, 12u);
    CHECK_EQ(p->used, 3u);
    // Restart: the peer reconnects and sends a fresh low sequence that was
    // never in the ring.  It must fire the reset, clear the stale ring, and
    // send rather than be suppressed.
    make_pkt(pkt, 0);
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 2000), kDampenSend);
    CHECK_EQ(c.st.suppressed, 0u);
    CHECK_EQ(p->used, 1u);       // ring cleared, then re-recorded seq 0
    CHECK_EQ(p->high_seq, 0u);   // high_seq reset so it does not re-fire
}

// Test 2: after the reset, high_seq is reset so the new epoch advances without
// re-firing the reset on every packet, and suppression re-engages.
void test_no_refire() {
    begin("after the epoch reset, high_seq is reset and suppression re-engages");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    const uint32_t len = kReorderSeqMinPay;
    // First epoch: low sequences.
    for (uint32_t s = 10; s <= 12; ++s) {
        make_pkt(pkt, s);
        CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 1000 + s), kDampenSend);
    }
    DampenPeer *p = nullptr;
    for (uint32_t i = 0; i < kDampenPeers; ++i) {
        if (c.peers[i].addr == 0x0A000001) { p = &c.peers[i]; break; }
    }
    CHECK(p != nullptr);
    // Restart at seq 0.
    make_pkt(pkt, 0);
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 2000), kDampenSend);
    CHECK_EQ(p->high_seq, 0u);
    // The new epoch advances normally: seq 1, 2, 3 are ahead of the reset
    // high_seq, so the reset must not re-fire and clear the ring each time.
    make_pkt(pkt, 1);
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 2010), kDampenSend);
    make_pkt(pkt, 2);
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 2020), kDampenSend);
    make_pkt(pkt, 3);
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 2030), kDampenSend);
    CHECK_EQ(p->high_seq, 3u);
    CHECK_EQ(p->used, 4u);       // ring accumulated 0,1,2,3 — no re-fire cleared it
    // Suppression re-engages: a duplicate of seq 3 inside its window is
    // suppressed, proving the ring is accumulating again instead of being
    // cleared by a re-fired reset.
    CHECK_EQ(dampen_admit(&c, 0x0A000001, pkt, len, 2040), kDampenSuppress);
    CHECK_EQ(c.st.suppressed, 1u);
}

}  // namespace

int main() {
    std::printf("dampen epoch tests\n");
    test_restart_near_zero();
    test_no_refire();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}