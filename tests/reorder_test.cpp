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
    std::vector<Delivered> passed_through;  // of those, ones the buffer declined

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
            // Write the sequence exactly as the wire carries it: u32 big-endian
            // at offset 10.  This used to memcpy a host-order u32, which
            // silently agreed with whatever the header said and so could never
            // catch a wrong field.
            buf[kReorderSeqOffset]     = static_cast<uint8_t>((w.seq >> 24) & 0xff);
            buf[kReorderSeqOffset + 1] = static_cast<uint8_t>((w.seq >> 16) & 0xff);
            buf[kReorderSeqOffset + 2] = static_cast<uint8_t>((w.seq >> 8) & 0xff);
            buf[kReorderSeqOffset + 3] = static_cast<uint8_t>(w.seq & 0xff);

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
            if (reorder_must_pass_through(r)) {
                // Defect E: a sequence we have already delivered is ordinary
                // payload under this protocol, not a duplicate to discard.
                // The proxies hand it to the game; so must the simulation, or
                // the tests cannot see the data loss the old code caused.
                passed_through.push_back(Delivered{w.peer, seq});
                out.push_back(Delivered{w.peer, seq});
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
                             + s.ctx.stats.dropped_duplicate
                             + s.ctx.stats.purged_stale;
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

// Defect B regression, amended by defect E (V4.9).  Replayed old sequences must
// not be buffered and must never drag the peer cursor backwards — but they must
// still reach the game.  Before V4.9 this test asserted they were *dropped*,
// which is what made a 98%-data-loss bug look like correct behaviour.
void test_stale_flood_rejected() {
    begin("replayed old sequences bypass the buffer, cursor never regresses (defects B+E)");
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
    CHECK_EQ(s.passed_through.size(), 50u);      // every one reached the game
    CHECK_EQ(s.out.size(), delivered_before + 50);
    CHECK_EQ(s.buffered_total(), 0u);            // no slots wasted on them

    // Cursor still at 100, so the next real packet is accepted in order.
    s.send(0, 101);
    s.pump();
    CHECK_EQ(s.out.size(), delivered_before + 51);
    CHECK_EQ(s.out.back().seq, 101u);
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

// A second datagram carrying a sequence already sitting in the ring is not
// buffered twice — one slot per sequence.  Defect E: it is still delivered.
// Under a message counter the second datagram is a different datagram with the
// same header, so discarding it discards payload.
void test_duplicate_not_buffered_twice() {
    begin("a repeated sequence takes one slot but is still delivered");
    Sim s;
    s.send(0, 1);
    s.pump();
    s.send(0, 3);
    s.send(0, 3);
    s.pump();
    CHECK_EQ(s.ctx.stats.dropped_duplicate, 1u);
    CHECK_EQ(s.passed_through.size(), 1u);
    CHECK_EQ(s.buffered_total(), 1u);      // seq 3 occupies exactly one slot
    s.advance(kReorderDefaultMs * 2);
    s.pump();
    CHECK_EQ(s.out.size(), 3u);            // 1, the pass-through copy, then 3
}

// ── The Windows IOCP deferral path (defect D, V4.9) ──────────────────────────
// That path never ran on real Windows and had no test at all, which is how it
// kept comparing `seq == last_seq + 1` in 32-bit space through V4.8's fix and
// into V4.9. The two decisions it makes now live in reorder_core.h, so the
// same code the proxy runs is exercised here.
void test_iocp_successor_wraps() {
    begin("IOCP successor test wraps (defect D)");
    ReorderCtx c;
    reorder_init(&c);
    PeerBuf pb;
    std::memset(&pb, 0, sizeof(pb));
    pb.seq_init = 1;

    pb.last_seq = 100;
    CHECK(reorder_is_successor(&pb, 101));
    CHECK(!reorder_is_successor(&pb, 102));
    CHECK(!reorder_is_successor(&pb, 100));

    // The case the 32-bit `+ 1` got wrong: at the top of the field the
    // successor is 0, and `last_seq + 1` overflows to exactly that only
    // because the field happens to be 32 bits wide. Pin it so a narrower
    // field would fail here rather than in a live match.
    pb.last_seq = kReorderSeqMask;
    CHECK(reorder_is_successor(&pb, 0));
    CHECK(!reorder_is_successor(&pb, 1));

    // No peer entry, or a peer with no sequence yet: never a successor.
    CHECK(!reorder_is_successor(nullptr, 1));
    pb.seq_init = 0;
    CHECK(!reorder_is_successor(&pb, 1));
}

void test_iocp_hold_window() {
    begin("IOCP hold window honours the per-peer window and the hard ceiling");
    ReorderCtx c;
    reorder_init(&c);
    c.win_min_ms = 5;
    c.max_hold_ms = 0;               // no ceiling
    PeerBuf pb;
    std::memset(&pb, 0, sizeof(pb));
    pb.win_ms = 60;

    CHECK_EQ(reorder_hold_window(&c, &pb), 60u);
    CHECK_EQ(reorder_hold_window(&c, nullptr), 5u);   // unknown peer -> floor

    c.max_hold_ms = 20;              // ceiling bites
    CHECK_EQ(reorder_hold_window(&c, &pb), 20u);
    pb.win_ms = 10;                  // ...but never raises a smaller window
    CHECK_EQ(reorder_hold_window(&c, &pb), 10u);
}

}  // namespace

// The sequence field is 32 bits (V4.9), so it will not wrap inside a session —
// at the ~1,600 messages/min measured live that is roughly five years of
// continuous play.  The wrap must still be handled: comparing without wrap
// arithmetic reads 0xffffffff -> 0x00000000 as a 4-billion-packet *backward*
// jump, rejects everything after it as stale, and stalls the peer permanently.
void test_sequence_wrap() {
    begin("32-bit sequence wrap is not read as a backward jump");
    Sim s;
    for (uint32_t i = 0; i < 40; ++i) {
        s.send(0, (0xffffffe0u + i) & kReorderSeqMask);
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
    s.send(0, 0xfffffffeu); s.pump(); s.advance(10);
    s.send(0, 0x00000000u); s.pump(); s.advance(5);   // jumped the wrap, gap open
    s.send(0, 0xffffffffu); s.pump(); s.advance(10);  // straggler fills it
    s.advance(kReorderDefaultMs * 2);
    s.pump();
    CHECK_EQ(s.out.size(), 3u);
    CHECK(ascending_for(s, 0));
    CHECK_EQ(s.ctx.stats.dropped_stale, 0u);
}

// The wire layout, asserted against the values tools/seq_crossmatch.py derives
// from BZLogger's own ordinals.  A real captured datagram is decoded here so
// that changing a constant in reorder_core.h without re-deriving the header
// fails the build rather than silently mis-reading every packet — which is how
// the offset stayed wrong from V3 through V4.8.
void test_wire_layout_matches_capture() {
    begin("header constants decode a real captured datagram");
    // From test_bundles/.../BZLogger.txt:
    //   BZRNet P2P TRY Sent Packet (705,14710) to 203.0.113.20:34354
    // c0 0000 0001 9f9fc236ff 000002c1 00003976 ...
    const uint8_t pkt[20] = {
        0xc0, 0x00, 0x00, 0x00, 0x01,
        0x9f, 0x9f, 0xc2, 0x36, 0xff,
        0x00, 0x00, 0x02, 0xc1,
        0x00, 0x00, 0x39, 0x76,
        0x7a, 0x75,
    };
    CHECK_EQ(kReorderSeqOffset, 10u);
    CHECK_EQ(kReorderSeqBytes, 4u);
    CHECK_EQ(reorder_seq_from_payload(pkt), 705u);
    CHECK_EQ(reorder_ack_from_payload(pkt), 14710u);
    // Byte 0 = 0xC0 = reliable | final.  This used to assert "is a retransmit";
    // the bit means reliable, and the rename is deliberate (see reorder_core.h).
    CHECK(reorder_is_reliable(pkt));
    CHECK((pkt[0] & kBzHdrFlagReliable) != 0);
    CHECK((pkt[0] & kBzHdrFlagFinal) != 0);
    CHECK_EQ(static_cast<uint32_t>(pkt[kBzHdrKindOffset]), 0u); // gameplay kind
    // Sender wall clock, epoch ms, stamped fresh on every copy.  This fixture
    // was captured on 2026-07-26 and the field decodes to 18:48:53.247Z that
    // day — the layout was validated on the 08-08 captures and this is an
    // independent confirmation on an older one, different machine, different
    // platform.  Bytes 5..9 were previously described as "a u40 clock at
    // offset 5"; they are the low five bytes of this u64.
    CHECK_EQ(reorder_send_time_ms(pkt), 1785091733247ull);
    // The V4.8 reading, for the record: u16 BE at offset 16 is the low half of
    // the ack, which is why it "duplicated" 88.5% of the time.
    CHECK_EQ(static_cast<uint32_t>((pkt[16] << 8) | pkt[17]), 14710u & 0xffffu);
}

// Defect E: a datagram whose sequence has already been delivered must reach
// the game.  Under this protocol the sequence counts messages, not datagrams,
// so ~98% of real inbound traffic repeats the previous value; the pre-V4.9
// code dropped every one of them.
void test_repeated_sequence_is_delivered_not_dropped() {
    begin("repeated sequence is passed through, not discarded");
    Sim s;
    s.send(0, 100); s.pump(); s.advance(5);
    // Four more datagrams of the same message, as a real sender emits.
    for (int i = 0; i < 4; ++i) {
        s.send(0, 100);
        s.pump();
        s.advance(5);
    }
    s.advance(kReorderDefaultMs * 2);
    s.pump();
    // All five datagrams reach the game; four of them bypassed the buffer.
    CHECK_EQ(s.out.size(), 5u);
    CHECK_EQ(s.passed_through.size(), 4u);
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
    test_duplicate_not_buffered_twice();
    test_sequence_wrap();
    test_reorder_across_wrap();
    test_wire_layout_matches_capture();
    test_iocp_successor_wraps();
    test_iocp_hold_window();
    test_repeated_sequence_is_delivered_not_dropped();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
