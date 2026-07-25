// tests/send_pace_test.cpp — host-side tests for shared/send_pace.h
//
// The pacer sits on the game's send path, so its failure modes are worse than
// its benefits: a dropped packet, a reordered one, or a stalled ping would each
// be more damaging than the burst it is trying to smooth.  These tests pin the
// three invariants that make it safe to ship — nothing is ever dropped, order
// is preserved, and small control packets are never delayed — plus the
// always-on measurement the feature is really for.
//
//   make -C tests run

#include "../shared/send_pace.h"

#include <cstdio>
#include <cstring>
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

#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); \
                            check(a_ == b_, #a, a_, b_); } while (0)
#define CHECK_LE(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); \
                            check(a_ <= b_, #a " <= " #b, a_, b_); } while (0)
#define CHECK(x)       do { bool x_ = (x); check(x_, #x, (long long)x_, 1LL); } while (0)

void begin(const char *name) {
    g_case = name;
    std::printf("- %s\n", name);
}

// Mirrors the send hook + pacer thread in both proxies.
struct Sim {
    PaceCtx  ctx;
    uint64_t now = 1000;
    std::vector<std::pair<uint64_t, uint32_t>> wire;   // (time sent, marker)

    Sim(uint32_t rate_bps, uint32_t max_delay = kPaceMaxDelayDef) {
        pace_init(&ctx, rate_bps, max_delay, now);
    }

    // One WSASendTo call.  `marker` stands in for packet identity.
    void send(uint32_t len, uint32_t marker) {
        uint8_t  data[kReorderMaxPktBytes] = {0};
        uint32_t addr = 0x0A000001;
        std::memcpy(data, &marker, sizeof(marker));

        uint64_t due = 0;
        const PaceDecision d = pace_admit(&ctx, len, now, &due);
        if (d == kPaceQueued
            && pace_enqueue(&ctx, 1, &addr, sizeof(addr), data, len, due)) {
            return;
        }
        if (d == kPaceFlushThenSend || d == kPaceQueued) {
            // Giving up on shaping: everything already queued goes out first,
            // in order, then this packet.
            flush();
        }
        wire.push_back({now, marker});
    }

    // Send everything queued, regardless of due time, preserving order.
    void flush() {
        PaceEntry e;
        while (pace_pop_any(&ctx, &e)) {
            uint32_t marker = 0;
            std::memcpy(&marker, e.data, sizeof(marker));
            wire.push_back({now, marker});
        }
    }

    // The pacer thread waking up.
    void drain() {
        PaceEntry e;
        while (pace_pop_due(&ctx, now, &e)) {
            uint32_t marker = 0;
            std::memcpy(&marker, e.data, sizeof(marker));
            wire.push_back({now, marker});
        }
    }

    void advance(uint64_t ms) {
        now += ms;
        pace_tick(&ctx, now);
        drain();
    }
};

// Invariant 1: every packet the game sends reaches the wire. Losing one to a
// full queue or an expired budget would be worse than not pacing at all.
void test_nothing_is_ever_dropped() {
    begin("every packet reaches the wire, even past the queue limit");
    Sim s(8000);                            // deliberately tight budget
    const uint32_t n = kPaceQueueSlots * 3; // far more than the queue holds
    for (uint32_t i = 0; i < n; ++i) {
        s.send(200, i);
    }
    for (int i = 0; i < 200; ++i) {
        s.advance(10);
    }
    CHECK_EQ(s.wire.size(), n);
    CHECK_EQ(s.ctx.count, 0u);              // queue fully drained
    CHECK_EQ(s.ctx.stats.packets, n);
}

// Invariant 2: the game's ordering must survive. A pacer that reorders would
// manufacture exactly the stale-sequence drops this whole project fights.
void test_order_is_preserved() {
    begin("send order is preserved end to end");
    Sim s(16000);
    for (uint32_t i = 0; i < 400; ++i) {
        s.send(300, i);
        if (i % 10 == 0) {
            s.advance(1);
        }
    }
    for (int i = 0; i < 200; ++i) {
        s.advance(10);
    }
    CHECK_EQ(s.wire.size(), 400u);
    bool ordered = true;
    for (size_t i = 0; i < s.wire.size(); ++i) {
        if (s.wire[i].second != i) {
            ordered = false;
            break;
        }
    }
    CHECK(ordered);
}

// Invariant 3: ping/control traffic must never be delayed. Its round-trip time
// is the number the host's auto-kick tests, so delaying it to smooth a bulk
// burst would trade the symptom for the cause.
void test_small_packets_are_never_delayed() {
    begin("control packets below the sequence-field size are never delayed");
    Sim s(1000);                            // budget so tight nothing else fits
    for (uint32_t i = 0; i < 50; ++i) {
        s.send(kReorderSeqMinPay - 1, i);   // too short to carry a sequence
    }
    CHECK_EQ(s.wire.size(), 50u);           // all out immediately, no drain
    CHECK_EQ(s.ctx.stats.exempt_small, 50u);
    CHECK_EQ(s.ctx.stats.paced, 0u);
    for (const auto &w : s.wire) {
        CHECK_EQ(w.first, 1000u);           // sent at t0, never held
    }
}

// The bounded-latency promise: no packet waits longer than max_delay_ms.
void test_delay_is_bounded() {
    begin("no packet is delayed beyond max_delay_ms");
    Sim s(8000, 15);
    for (uint32_t i = 0; i < 300; ++i) {
        s.send(500, i);
    }
    for (int i = 0; i < 300; ++i) {
        s.advance(5);
    }
    CHECK_EQ(s.wire.size(), 300u);
    CHECK_LE(s.ctx.stats.max_delay_ms, 15u);
    for (const auto &w : s.wire) {
        CHECK_LE(w.first - 1000u, 15u);
    }
}

// A steady stream inside the budget must be untouched — pacing that taxes
// normal play is not worth having.
void test_steady_stream_is_untouched() {
    begin("a stream inside the budget is never delayed");
    Sim s(16000);
    // BZ's steady rate: ~30 packets/sec of ~300 bytes = ~9 KB/s.
    for (uint32_t i = 0; i < 300; ++i) {
        s.send(300, i);
        s.advance(33);
    }
    CHECK_EQ(s.wire.size(), 300u);
    CHECK_EQ(s.ctx.stats.paced, 0u);
    CHECK_EQ(s.ctx.stats.max_delay_ms, 0u);
}

// With pacing off the hook must be a pure observer.
void test_measure_only_mode() {
    begin("rate 0 measures without ever delaying");
    Sim s(0);
    for (uint32_t i = 0; i < 500; ++i) {
        s.send(400, i);
    }
    CHECK_EQ(s.wire.size(), 500u);
    CHECK_EQ(s.ctx.count, 0u);
    CHECK_EQ(s.ctx.stats.paced, 0u);
    CHECK_EQ(s.ctx.stats.packets, 500u);
    CHECK_EQ(s.ctx.stats.bytes, 500u * 400u);
}

// The measurement that justifies the feature: a retransmit flood must show up
// as a burst second with a high peak, and ordinary play must not.
void test_burst_detection() {
    begin("a retransmit flood registers as a burst second; steady play does not");
    Sim quiet(0);
    for (uint32_t i = 0; i < 300; ++i) {    // ~30 pkt/s for 10 s
        quiet.send(300, i);
        quiet.advance(33);
    }
    CHECK_EQ(quiet.ctx.stats.burst_seconds, 0u);
    CHECK_LE(quiet.ctx.stats.peak_pps, kPaceBurstPps);

    Sim flood(0);
    for (uint32_t i = 0; i < 900; ++i) {    // 1179-packets-in-13.4s shape
        flood.send(300, i);
        if (i % 3 == 0) {
            flood.advance(10);              // ~300 pkt/s across 3 s
        }
    }
    flood.advance(1500);                    // pace_tick closes the last window
    CHECK(flood.ctx.stats.burst_seconds > 0);
    CHECK(flood.ctx.stats.peak_pps > kPaceBurstPps);
}

// A documented limitation, not a defect: the pacer can only hold a packet for
// max_delay_ms, so it can only absorb max_delay_ms * rate bytes.  At BZ's rates
// the 20 ms default buys only a few hundred bytes — under one packet — so an
// instantaneous burst is passed through rather than shaped.  It must still come
// out complete and in order, and the stats must say pacing did nothing, so the
// operator can see that raising BZ_SEND_PACE_MAX_MS is what would change it.
void test_tight_budget_cannot_pace() {
    begin("a delay budget under one packet passes traffic through, in order");
    Sim s(16000, 20);                       // 20 ms * 16 B/ms = 320 bytes
    for (uint32_t i = 0; i < 40; ++i) {
        s.send(400, i);                     // each packet exceeds the budget
    }
    CHECK_EQ(s.wire.size(), 40u);           // nothing held, nothing lost
    CHECK_EQ(s.ctx.count, 0u);
    CHECK(s.ctx.stats.over_budget_passed > 0);
    bool ordered = true;
    for (size_t i = 0; i < s.wire.size(); ++i) {
        if (s.wire[i].second != i) {
            ordered = false;
            break;
        }
    }
    CHECK(ordered);
}

// A closed socket's queued packets must not be sent on the reused handle.
void test_purge_on_socket_close() {
    begin("queued packets are purged when their socket closes");
    // max_delay_ms must buy at least a packet's worth of budget or nothing
    // ever queues — see test_tight_budget_cannot_pace.
    Sim s(16000, 200);
    for (uint32_t i = 0; i < 40; ++i) {
        s.send(400, i);
    }
    CHECK(s.ctx.count > 0);
    pace_purge_socket(&s.ctx, 1);
    CHECK_EQ(s.ctx.count, 0u);
    const size_t before = s.wire.size();
    for (int i = 0; i < 50; ++i) {
        s.advance(10);
    }
    CHECK_EQ(s.wire.size(), before);        // nothing escaped afterwards
}

// Idle gaps must not be mistaken for traffic when the window rolls forward.
void test_idle_gap_does_not_fake_bursts() {
    begin("a long idle gap does not corrupt the measurement window");
    Sim s(0);
    s.send(300, 0);
    s.advance(60000);                       // a minute of silence
    s.send(300, 1);
    CHECK_EQ(s.ctx.stats.burst_seconds, 0u);
    CHECK_LE(s.ctx.stats.peak_pps, 1u);
}

}  // namespace

int main() {
    std::printf("send_pace V4.7 tests\n");
    test_nothing_is_ever_dropped();
    test_order_is_preserved();
    test_small_packets_are_never_delayed();
    test_delay_is_bounded();
    test_steady_stream_is_untouched();
    test_measure_only_mode();
    test_burst_detection();
    test_tight_budget_cannot_pace();
    test_purge_on_socket_close();
    test_idle_gap_does_not_fake_bursts();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
