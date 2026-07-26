// tests/reorder_test.cpp — host-side regression tests for shared/reorder_core.h
//
// The reorder core has no winsock or Windows dependency and takes its clock
// from the caller, so the whole state machine runs natively here with a
// controllable clock and a simulated socket.  Every test below pins one of the
// defects fixed in V4.7; if a future change reintroduces one, this fails.
//
//   make -C tests && ./tests/reorder_test
//
// The simulator mirrors the drain/deliver loop in both proxies'
// hooked_WSARecvFrom.  Keep them in step: if the loop there changes shape, it
// changes here too.

#include "../shared/reorder_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

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

// Each macro evaluates its arguments exactly once: several call sites pass a
// state-advancing call (net_globals_apply, recv_call), and re-evaluating it for
// the failure message would both corrupt the test and misreport the value.
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); \
                            check(a_ == b_, #a, a_, b_); } while (0)
#define CHECK_LE(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); \
                            check(a_ <= b_, #a " <= " #b, a_, b_); } while (0)
#define CHECK_GE(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); \
                            check(a_ >= b_, #a " >= " #b, a_, b_); } while (0)
#define CHECK(x)       do { bool x_ = (x); check(x_, #x, (long long)x_, 1LL); } while (0)

// ── Simulated wire + game ────────────────────────────────────────────────────

struct WirePkt {
    uint32_t peer;   // peer index -> synthetic 10.0.0.<peer+1>:20000+peer
    uint32_t seq;
    uint32_t len;
};

struct Delivered {
    uint32_t peer;
    uint32_t seq;
};

sockaddr_in peer_addr(uint32_t peer) {
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(static_cast<uint16_t>(20000 + peer));
    BZ_SIN_ADDR_RAW(a) = htonl(0x0A000001u + peer);
    return a;
}

struct Sim {
    ReorderCtx            ctx;
    uint64_t              now = 1000;
    uint32_t              drain_cap = kReorderDrainCapDef;
    std::deque<WirePkt>   wire;      // datagrams queued in the "kernel socket"
    std::vector<Delivered> out;      // datagrams handed to the game

    Sim() {
        reorder_init(&ctx);
    }

    void send(uint32_t peer, uint32_t seq, uint32_t len = 64) {
        wire.push_back(WirePkt{peer, seq, len});
    }

    // One hooked_WSARecvFrom call.  Returns false for "socket empty"
    // (WSAEWOULDBLOCK), true when a datagram was handed to the game.
    bool recv_call() {
        uint8_t  buf[kReorderMaxPktBytes];
        uint32_t drained = 0;

        for (uint32_t di = 0; di < drain_cap; ++di) {
            if (reorder_drain_saturated(&ctx)) {
                break;
            }
            if (wire.empty()) {
                break;
            }
            const WirePkt w = wire.front();
            wire.pop_front();
            ++drained;

            const sockaddr_in from = peer_addr(w.peer);
            std::memset(buf, 0, sizeof(buf));
            // Write the sequence exactly as the wire carries it: u16 big-endian.
            // This used to memcpy a host-order u32, which silently agreed with
            // whatever the header said and so could never catch a wrong field.
            buf[kReorderSeqOffset]     = static_cast<uint8_t>((w.seq >> 8) & 0xff);
            buf[kReorderSeqOffset + 1] = static_cast<uint8_t>(w.seq & 0xff);

            // Read it back out the way the proxies do, so the wire encoding is
            // on the tested path rather than bypassed by handing w.seq along.
            const uint32_t seq = reorder_seq_from_payload(buf);

            PeerBuf *pb = reorder_get_peer(&ctx, from, now);
            if (pb == nullptr) {
                ctx.stats.bypass_table_full++;
                out.push_back(Delivered{w.peer, seq});
                return true;
            }

            reorder_adapt_on_arrival(&ctx, pb, seq, now);

            ReorderSlot evicted;
            std::memset(&evicted, 0, sizeof(evicted));
            const InsertResult r =
                reorder_insert(&ctx, pb, seq, now, from, buf, w.len, &evicted);
            if (r == kInsertEvicted) {
                reorder_note_delivery(&ctx, pb, evicted.seq, evicted.ts, now, kDeliverEvicted);
                out.push_back(Delivered{w.peer, evicted.seq});
                return true;
            }
        }

        if (drained > ctx.stats.max_drain_depth) {
            ctx.stats.max_drain_depth = drained;
        }

        uint32_t pi = 0;
        int      si = 0;
        int      kind = kDeliverInOrder;
        if (!reorder_next_ready(&ctx, now, &pi, &si, &kind)) {
            return false;
        }
        ReorderSlot got;
        reorder_take(&ctx, &ctx.tbl[pi], si, now, kind, &got);
        out.push_back(Delivered{static_cast<uint32_t>(ntohs(got.from.sin_port) - 20000), got.seq});
        return true;
    }

    // Drain until the game would block.
    void pump(int max_calls = 100000) {
        while (max_calls-- > 0 && recv_call()) {
        }
    }

    // Advance the clock and pump again, so held packets age out.
    void advance(uint64_t ms) {
        now += ms;
    }

    uint64_t delivered_total() const {
        return ctx.stats.delivered_first + ctx.stats.delivered_in_order
             + ctx.stats.delivered_forced + ctx.stats.delivered_evicted;
    }

    uint32_t buffered_total() const {
        uint32_t n = 0;
        for (uint32_t i = 0; i < ctx.peers; ++i) {
            n += ctx.tbl[i].filled;
        }
        return n;
    }

    uint32_t win_of(uint32_t peer) const {
        const sockaddr_in a = peer_addr(peer);
        const uint64_t k =
            (static_cast<uint64_t>(static_cast<uint32_t>(BZ_SIN_ADDR_RAW(a))) << 16)
            | static_cast<uint64_t>(ntohs(a.sin_port));
        for (uint32_t i = 0; i < ctx.peers; ++i) {
            if (ctx.tbl[i].key == k) {
                return ctx.tbl[i].win_ms;
            }
        }
        return 0;
    }
};

// True when every delivery for `peer` is in strictly ascending sequence.
bool ascending_for(const Sim &s, uint32_t peer) {
    bool seen = false;
    uint32_t last = 0;
    for (const Delivered &d : s.out) {
        if (d.peer != peer) {
            continue;
        }
        if (seen && seq_cmp(d.seq, last) <= 0) {
            return false;
        }
        last = d.seq;
        seen = true;
    }
    return true;
}

void begin(const char *name) {
    g_case = name;
    std::printf("- %s\n", name);
}

// ── Cases ────────────────────────────────────────────────────────────────────

// A clean link must cost nothing: no holds, no forced releases, window pinned
// at the floor.
void test_clean_in_order() {
    begin("clean in-order stream is pass-through");
    Sim s;
    for (uint32_t seq = 1; seq <= 100; ++seq) {
        s.send(0, seq);
        s.pump();
        s.advance(10);
    }
    CHECK_EQ(s.out.size(), 100u);
    CHECK(ascending_for(s, 0));
    CHECK_EQ(s.ctx.stats.delivered_forced, 0u);
    CHECK_EQ(s.ctx.stats.delivered_evicted, 0u);
    CHECK_EQ(s.ctx.stats.dropped_stale, 0u);
    CHECK_EQ(s.ctx.stats.hold_ms_max, 0u);
    CHECK_EQ(s.win_of(0), kReorderMinMsDef);
}

// The thing the buffer exists for: adjacent swaps must come out ordered.
void test_mild_reorder() {
    begin("adjacent swaps are re-ordered, window grows modestly");
    Sim s;
    for (uint32_t base = 1; base <= 99; base += 2) {
        s.send(0, base + 1);   // arrives first
        s.send(0, base);       // its predecessor, late
        s.pump();
        s.advance(10);
    }
    s.advance(kReorderDefaultMs * 2);
    s.pump();

    CHECK_EQ(s.out.size(), 100u);
    CHECK(ascending_for(s, 0));
    CHECK_EQ(s.ctx.stats.dropped_stale, 0u);
    CHECK_EQ(s.ctx.stats.delivered_evicted, 0u);
    // Reordering is real here, so the window may grow — but never to the
    // ceiling on swaps this small.
    CHECK(s.win_of(0) < kReorderDefaultMs);
}

// Defect A regression.  200 datagrams land before the game polls.  The old code
// drained 96 of them into 8-slot rings and silently destroyed the overflow.
// Nothing may vanish: every datagram consumed from the wire must be accounted
// for as delivered, still buffered, or explicitly counted as stale/duplicate.
void test_burst_loses_nothing() {
    begin("200-packet burst destroys no packets (defect A)");
    Sim s;
    for (uint32_t seq = 1; seq <= 200; ++seq) {
        s.send(0, seq);
    }
    for (int i = 0; i < 400 && !(s.wire.empty() && s.buffered_total() == 0); ++i) {
        s.pump();
        s.advance(kReorderDefaultMs + 10);
    }

    const uint64_t accounted = s.delivered_total()
                             + s.buffered_total()
                             + s.ctx.stats.dropped_stale
                             + s.ctx.stats.dropped_duplicate;
    CHECK_EQ(accounted, 200u);
    CHECK_EQ(s.out.size(), 200u);
    CHECK(ascending_for(s, 0));
    // With the drain bounded by ring capacity, eviction should not trigger at
    // all for a single in-order peer.
    CHECK_EQ(s.ctx.stats.delivered_evicted, 0u);
    CHECK_LE(s.ctx.stats.max_drain_depth, kReorderSlotCap);
}

// Defect C regression.  Pure loss with no reordering is not evidence that the
// link reorders, so the window must stay near the floor.  The old blind
// doubling drove it to the ceiling and 5 ms/2 s decay kept it there ~38 s.
void test_pure_loss_does_not_pin_window() {
    begin("pure loss does not ratchet the window (defect C)");
    Sim s;
    uint32_t seq = 1;
    for (int i = 0; i < 60; ++i) {
        if (i % 5 != 0) {          // every fifth packet is lost outright
            s.send(0, seq);
        }
        ++seq;
        s.pump();
        s.advance(20);
        s.pump();                  // let the gap age out
    }
    CHECK(s.win_of(0) < kReorderDefaultMs);
    CHECK_LE(s.ctx.stats.hold_ms_max, kReorderDefaultMs);
}

// Defect C regression, decay half.  After the window is driven up, a quiet
// clean stretch must bring it back down in seconds, not ~38 of them.
void test_window_decays_quickly() {
    begin("window decays back to the floor in seconds (defect C)");
    Sim s;
    // Drive the window up with genuine late arrivals.
    for (uint32_t base = 1; base <= 40; base += 2) {
        s.send(0, base + 1);
        s.advance(40);
        s.pump();
        s.send(0, base);
        s.pump();
        s.advance(10);
    }
    const uint32_t peak = s.win_of(0);
    CHECK_GE(peak, kReorderMinMsDef);

    // Now a clean in-order stretch: 5 s of traffic must return it to the floor.
    uint32_t seq = 100;
    for (int i = 0; i < 250; ++i) {
        s.send(0, seq++);
        s.pump();
        s.advance(20);
    }
    CHECK_EQ(s.win_of(0), kReorderMinMsDef);
}

// Defect B regression.  Replayed old sequences must be rejected at insert, must
// not consume ring slots, and must never drag the peer cursor backwards.
void test_stale_flood_rejected() {
    begin("replayed old sequences are rejected, cursor never regresses (defect B)");
    Sim s;
    for (uint32_t seq = 1; seq <= 100; ++seq) {
        s.send(0, seq);
        s.pump();
        s.advance(10);
    }
    const size_t delivered_before = s.out.size();

    for (uint32_t seq = 1; seq <= 50; ++seq) {   // replay
        s.send(0, seq);
        s.pump();
        s.advance(1);
    }

    CHECK_EQ(s.ctx.stats.dropped_stale, 50u);
    CHECK_EQ(s.out.size(), delivered_before);    // nothing stale reached the game
    CHECK_EQ(s.buffered_total(), 0u);            // no slots wasted on them

    // Cursor still at 100, so the next real packet is accepted in order.
    s.send(0, 101);
    s.pump();
    CHECK_EQ(s.out.size(), delivered_before + 1);
    CHECK_EQ(s.out.back().seq, 101u);
    CHECK(ascending_for(s, 0));
}

// Defect G regression.  In a mesh, one busy peer must not starve the others:
// the delivery scan is round-robin, not always-from-zero.
void test_mesh_fairness() {
    begin("no peer is starved in a 4-peer mesh (defect G)");
    Sim s;
    std::map<uint32_t, uint32_t> seq;
    for (uint32_t p = 0; p < 4; ++p) {
        seq[p] = 1;
    }
    for (int round = 0; round < 200; ++round) {
        // Peer 0 is chatty (index 0 is the one the old scan always favoured).
        for (int i = 0; i < 5; ++i) {
            s.send(0, seq[0]++);
        }
        for (uint32_t p = 1; p < 4; ++p) {
            s.send(p, seq[p]++);
        }
        s.pump();
        s.advance(10);
    }
    s.advance(kReorderDefaultMs * 2);
    s.pump();

    std::map<uint32_t, int> got;
    for (const Delivered &d : s.out) {
        got[d.peer]++;
    }
    for (uint32_t p = 0; p < 4; ++p) {
        check(got[p] > 0, "peer delivered > 0", got[p], 1);
        CHECK(ascending_for(s, p));
    }
    // Each quiet peer sent 200; none should be more than a packet or two short.
    for (uint32_t p = 1; p < 4; ++p) {
        CHECK_GE(got[p], 199);
    }
}

// Defect F regression.  A full peer table must reclaim entries that have gone
// quiet rather than bypassing the buffer forever.
void test_peer_reclaim() {
    begin("idle peer entries are reclaimed when the table fills (defect F)");
    Sim s;
    for (uint32_t p = 0; p < kReorderPeerCap; ++p) {
        s.send(p, 1);
        s.pump();
    }
    CHECK_EQ(s.ctx.stats.bypass_table_full, 0u);

    // A new source arrives while every entry is live: pass through, no reclaim.
    s.send(kReorderPeerCap, 1);
    s.pump();
    CHECK_EQ(s.ctx.stats.bypass_table_full, 1u);
    CHECK_EQ(s.ctx.stats.peers_reclaimed, 0u);

    // After the idle timeout, the same new source claims a stale entry.
    s.advance(kReorderPeerIdleMs + 1000);
    s.send(kReorderPeerCap, 2);
    s.pump();
    CHECK_EQ(s.ctx.stats.peers_reclaimed, 1u);
    CHECK_EQ(s.ctx.stats.bypass_table_full, 1u);
}

// The absolute hold ceiling must bound added latency independently of the
// adaptive window.
void test_max_hold_ceiling() {
    begin("BZ_REORDER_MAX_HOLD_MS bounds added latency");
    Sim s;
    s.ctx.max_hold_ms = 20;
    s.ctx.win_min_ms  = 100;   // window deliberately larger than the ceiling
    s.ctx.win_max_ms  = 100;
    s.ctx.adapt       = false;

    s.send(0, 1);
    s.pump();
    s.send(0, 3);              // gap: seq 2 never arrives
    s.pump();
    CHECK_EQ(s.out.size(), 1u);   // held, not delivered yet

    s.advance(25);             // past the 20 ms ceiling, well under the window
    s.pump();
    CHECK_EQ(s.out.size(), 2u);
    CHECK_EQ(s.out.back().seq, 3u);
    CHECK_LE(s.ctx.stats.hold_ms_max, 30u);
}

// Exact duplicates of buffered packets are dropped, not delivered twice.
void test_duplicate_dropped() {
    begin("duplicate sequences are dropped once buffered");
    Sim s;
    s.send(0, 1);
    s.pump();
    s.send(0, 3);
    s.send(0, 3);
    s.pump();
    CHECK_EQ(s.ctx.stats.dropped_duplicate, 1u);
    s.advance(kReorderDefaultMs * 2);
    s.pump();
    CHECK_EQ(s.out.size(), 2u);
}

}  // namespace

// The sequence field is 16 bits, so it wraps every 65,536 packets — at the
// 100-200 packets/sec measured live that is every 6-11 minutes, i.e. at least
// once in an ordinary match.  Comparing in 32-bit space reads 0xffff -> 0x0000
// as a 65,535-packet *backward* jump, rejects everything after it as stale, and
// stalls the peer for the rest of the game.
void test_sequence_wrap() {
    begin("16-bit sequence wrap is not read as a backward jump");
    Sim s;
    for (uint32_t i = 0; i < 40; ++i) {
        s.send(0, (0xffe0 + i) & kReorderSeqMask);
        s.pump();
        s.advance(10);
    }
    CHECK_EQ(s.out.size(), 40u);
    CHECK(ascending_for(s, 0));
    CHECK_EQ(s.ctx.stats.dropped_stale, 0u);
    CHECK_EQ(s.ctx.stats.delivered_forced, 0u);
    CHECK_EQ(s.win_of(0), kReorderMinMsDef);
}

// Reordering that straddles the wrap must still be repaired, not passed
// through: the gap-fill arithmetic has to wrap too, not just the comparison.
void test_reorder_across_wrap() {
    begin("out-of-order arrival straddling the wrap is still reordered");
    Sim s;
    s.send(0, 0xfffe); s.pump(); s.advance(10);
    s.send(0, 0x0000); s.pump(); s.advance(5);   // jumped the wrap, gap open
    s.send(0, 0xffff); s.pump(); s.advance(10);  // straggler fills it
    s.advance(kReorderDefaultMs * 2);
    s.pump();
    CHECK_EQ(s.out.size(), 3u);
    CHECK(ascending_for(s, 0));
    CHECK_EQ(s.ctx.stats.dropped_stale, 0u);
}

int main() {
    std::printf("reorder_core V4.7 regression tests\n");
    test_clean_in_order();
    test_mild_reorder();
    test_burst_loses_nothing();
    test_pure_loss_does_not_pin_window();
    test_window_decays_quickly();
    test_stale_flood_rejected();
    test_mesh_fairness();
    test_peer_reclaim();
    test_max_hold_ceiling();
    test_duplicate_dropped();
    test_sequence_wrap();
    test_reorder_across_wrap();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
