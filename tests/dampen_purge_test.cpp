// tests/dampen_purge_test.cpp — host-side tests for dampen_purge_peer, the
// explicit PRIMARY reset signal for shared/send_dampen.h.
//
// The in-band epoch heuristic (a sequence jumping backward BELOW the ring's
// oldest retained sequence) is a backstop for a peer that reconnects without the
// proxy noticing.  dampen_purge_peer is the primary path: the proxy's
// connect/disconnect path calls it, so a real restart is signalled explicitly
// rather than inferred.  These tests pin what that signal must do:
//
//   1. a purge clears the peer's ring AND its high-water mark, so a fresh low
//      sequence is not suppressed against a stale high_seq; and
//   2. a purge is per-peer — other peers' suppression state is untouched.
//
//   make -C tests dampen_purge_test && ./tests/dampen_purge_test

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

DampenPeer *find_peer(DampenCtx *c, uint32_t addr) {
    for (uint32_t i = 0; i < kDampenPeers; ++i) {
        if (c->peers[i].addr == addr) {
            return &c->peers[i];
        }
    }
    return nullptr;
}

// Test 1: a purge clears the peer's ring AND its high-water mark.  The point of
// clearing high_seq is that a fresh low sequence after a reconnect must be a
// first copy, not a suppressed duplicate against a stale high-water mark.
void test_purge_clears_ring_and_high_seq() {
    begin("a purge clears the peer's ring and high_seq");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    const uint32_t len = kReorderSeqMinPay;
    const uint32_t addr = 0x0A000001;

    // First epoch: several sequences, so the ring accumulates and high_seq rises.
    for (uint32_t s = 10; s <= 12; ++s) {
        make_pkt(pkt, s);
        CHECK_EQ(dampen_admit(&c, addr, pkt, len, 1000 + s), kDampenSend);
    }
    DampenPeer *p = find_peer(&c, addr);
    CHECK(p != nullptr);
    CHECK_EQ(p->used, 3u);
    CHECK_EQ(p->high_seq, 12u);

    // Purge: the proxy signalled a reconnect explicitly.
    dampen_purge_peer(&c, addr);

    p = find_peer(&c, addr);
    CHECK(p == nullptr);            // the peer entry is gone entirely
    // A fresh low sequence must now be a first copy: it sends, is not suppressed
    // against the stale high-water mark, and re-creates the peer with high_seq
    // reset to that low sequence.
    make_pkt(pkt, 0);
    CHECK_EQ(dampen_admit(&c, addr, pkt, len, 2000), kDampenSend);
    CHECK_EQ(c.st.suppressed, 0u);
    p = find_peer(&c, addr);
    CHECK(p != nullptr);
    CHECK_EQ(p->used, 1u);          // ring re-accumulated only the fresh sequence
    CHECK_EQ(p->high_seq, 0u);      // high_seq reset, not the stale 12
}

// Test 2: a purge is per-peer.  Purge one peer; the other keeps its ring and
// suppression state so its in-window redundant copies are still suppressed.
void test_purge_is_per_peer() {
    begin("a purge touches only the named peer");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    const uint32_t len = kReorderSeqMinPay;
    const uint32_t a = 0x0A000001;
    const uint32_t b = 0x0A000002;

    make_pkt(pkt, 5);
    CHECK_EQ(dampen_admit(&c, a, pkt, len, 1000), kDampenSend);
    CHECK_EQ(dampen_admit(&c, b, pkt, len, 1000), kDampenSend);

    dampen_purge_peer(&c, a);

    // Peer a is gone; peer b is untouched.
    CHECK(find_peer(&c, a) == nullptr);
    DampenPeer *pb = find_peer(&c, b);
    CHECK(pb != nullptr);
    CHECK_EQ(pb->used, 1u);
    CHECK_EQ(pb->high_seq, 5u);
    // Peer b still suppresses its own in-window repeat.
    CHECK_EQ(dampen_admit(&c, b, pkt, len, 1010), kDampenSuppress);
    CHECK_EQ(c.st.suppressed, 1u);
    // Peer a, freshly recreated, treats its sequence as a first copy again.
    CHECK_EQ(dampen_admit(&c, a, pkt, len, 1010), kDampenSend);
    CHECK_EQ(c.st.suppressed, 1u);   // only peer b's repeat was suppressed
}

// Test 3: purging a peer that the table does not know is a harmless no-op.
void test_purge_unknown_peer_is_noop() {
    begin("purging an unknown peer is a no-op");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    const uint32_t len = kReorderSeqMinPay;
    const uint32_t addr = 0x0A000001;

    make_pkt(pkt, 4);
    CHECK_EQ(dampen_admit(&c, addr, pkt, len, 1000), kDampenSend);
    dampen_purge_peer(&c, 0x0A0000FF);   // never seen
    DampenPeer *p = find_peer(&c, addr);
    CHECK(p != nullptr);
    CHECK_EQ(p->used, 1u);             // untouched
    CHECK_EQ(p->high_seq, 4u);
    CHECK_EQ(c.st.peers_evicted, 0u);  // no eviction counted
}

}  // namespace

int main() {
    std::printf("dampen purge tests\n");
    test_purge_clears_ring_and_high_seq();
    test_purge_is_per_peer();
    test_purge_unknown_peer_is_noop();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}