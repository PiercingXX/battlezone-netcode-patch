// tests/dampen_gate_test.cpp — host-side tests for dampen_close_ends_session,
// the single source of truth for "this close ends the dampened session".
//
// The proxy must not purge the peer ring when an UNRELATED socket closes — the
// shipped bug purged unconditionally on every closesocket, so a lobby/discovery
// socket close wiped live suppression state.  The gate predicate is the fix:
// it returns true only when the closing socket is the tracked dampen (P2P)
// socket, and false for any other close, including before the P2P socket is
// even known (the untracked sentinel).
//
// These tests drive every branch of the predicate:
//   (a) closing == the tracked dampen socket -> true  (session ended, purge is
//       correct);
//   (b) closing != the tracked dampen socket while peers are live -> false
//       (an UNRELATED socket closed — the ring MUST survive);
//   (c) dampen_sock == the untracked sentinel -> false (never purge before the
//       P2P socket is even known);
//   (d) dampen_sock == 0 -> false (the zero handle is not a tracked socket).
//
// A false verdict must leave the ring and high_seq untouched — the regression
// the shipped bug caused.
//
//   make -C tests dampen_gate_test && ./tests/dampen_gate_test

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

// Test 1: closing the tracked dampen socket ends the session -> true.
void test_closing_tracked_socket_ends_session() {
    begin("closing the tracked dampen socket ends the session");
    const unsigned long long tracked = 0x1234ULL;
    CHECK(dampen_close_ends_session(tracked, tracked));
}

// Test 2: closing an UNRELATED socket while a tracked one is live -> false.
void test_closing_unrelated_socket_is_not_session_end() {
    begin("closing an unrelated socket is not the session end");
    const unsigned long long tracked = 0x1234ULL;
    const unsigned long long other   = 0x5678ULL;
    CHECK(!dampen_close_ends_session(other, tracked));
}

// Test 3: the untracked sentinel dampen_sock never ends a session -> false.
void test_untracked_sentinel_never_ends_session() {
    begin("the untracked sentinel never ends a session");
    // Before the P2P socket is known, dampen_sock is the sentinel.  Closing any
    // socket (even one numerically equal to the sentinel) must not purge.
    CHECK(!dampen_close_ends_session(kDampenInvalidSock, kDampenInvalidSock));
    CHECK(!dampen_close_ends_session(0x1234ULL, kDampenInvalidSock));
}

// Test 4: a zero dampen_sock is not a tracked handle -> false.
void test_zero_sock_never_ends_session() {
    begin("a zero dampen_sock never ends a session");
    CHECK(!dampen_close_ends_session(0, 0));
    CHECK(!dampen_close_ends_session(0x1234ULL, 0));
}

// Test 5: a false verdict leaves the ring and high_seq untouched — the exact
// regression the shipped bug caused.  Build a live peer, then close an
// unrelated socket; the ring must survive so in-window suppression still works.
void test_false_verdict_leaves_ring_untouched() {
    begin("a false verdict leaves the ring and high_seq untouched");
    DampenCtx c;
    dampen_init(&c, true, 1000);
    uint8_t pkt[kReorderSeqMinPay];
    const uint32_t len = kReorderSeqMinPay;
    const uint32_t addr = 0x0A000001;

    // Populate the peer ring with a live sequence.
    make_pkt(pkt, 7);
    CHECK_EQ(dampen_admit(&c, addr, pkt, len, 1000), kDampenSend);
    DampenPeer *p = find_peer(&c, addr);
    CHECK(p != nullptr);
    CHECK_EQ(p->used, 1u);
    CHECK_EQ(p->high_seq, 7u);

    // Closing an unrelated socket (the tracked dampen sock is a different
    // handle) must NOT purge the ring.
    const unsigned long long tracked = 0x1234ULL;
    const unsigned long long other   = 0x5678ULL;
    CHECK(!dampen_close_ends_session(other, tracked));

    p = find_peer(&c, addr);
    CHECK(p != nullptr);
    CHECK_EQ(p->used, 1u);
    CHECK_EQ(p->high_seq, 7u);
    // In-window suppression still works: the ring survived the unrelated close.
    CHECK_EQ(dampen_admit(&c, addr, pkt, len, 1010), kDampenSuppress);
    CHECK_EQ(c.st.suppressed, 1u);
}

}  // namespace

int main() {
    std::printf("dampen gate tests\n");
    test_closing_tracked_socket_ends_session();
    test_closing_unrelated_socket_is_not_session_end();
    test_untracked_sentinel_never_ends_session();
    test_zero_sock_never_ends_session();
    test_false_verdict_leaves_ring_untouched();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}