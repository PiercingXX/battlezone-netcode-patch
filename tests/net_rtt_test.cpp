// tests/net_rtt_test.cpp — host-side tests for shared/net_rtt.h
//
// The RTT sampler exists to answer one question the 2026-08-15 logs could not:
// did the link degrade DURING the match, or was it flat?  A measurement that
// silently reports a wrong number is worse than no measurement, because it
// would have been trusted for exactly the diagnosis that had no other
// evidence.  These tests pin the four ways it could lie:
//
//   1. Sampling a repeated ack — the ack stays put until the peer sees
//      something newer, so re-sampling it measures age, not round trip, and
//      drags the mean up without bound.
//   2. Sampling a retransmitted sequence — ambiguous by construction
//      (Karn's algorithm).  This protocol resends hard; the 2026-08-15 host
//      resent one sequence 27 times.
//   3. Carrying slots across a disconnect, so a new match's low sequences
//      match the previous match's outstanding ones.
//   4. Reporting a clean average while quietly filtering most samples.
//
//   make -C tests run

#include "../shared/net_rtt.h"

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

// Build a datagram with the real header layout: flags, kind, u64 BE send
// clock, u32 BE sequence, u32 BE ack.  See resources/BZ_P2P_HEADER.md.
struct Pkt {
    uint8_t b[64];
    uint32_t len;
};

Pkt make_pkt(uint32_t seq, uint32_t ack, bool reliable = true) {
    Pkt p;
    std::memset(p.b, 0, sizeof(p.b));
    p.len  = 32;
    p.b[0] = reliable ? 0xC0 : 0x40;
    p.b[1] = 0x00;                       // gameplay
    for (int i = 0; i < 8; ++i) p.b[2 + i] = 0;   // send clock unused here
    p.b[10] = (uint8_t)(seq >> 24); p.b[11] = (uint8_t)(seq >> 16);
    p.b[12] = (uint8_t)(seq >> 8);  p.b[13] = (uint8_t)seq;
    p.b[14] = (uint8_t)(ack >> 24); p.b[15] = (uint8_t)(ack >> 16);
    p.b[16] = (uint8_t)(ack >> 8);  p.b[17] = (uint8_t)ack;
    return p;
}

const uint32_t kPeer = 0x0100007f;   // 127.0.0.1, network order

// A clean send->ack loop yields exactly the elapsed time.
void test_basic_sample() {
    begin("a single send/ack loop measures the elapsed time");
    RttCtx c;
    rtt_init(&c, true, 15000, 0);

    Pkt out = make_pkt(/*seq*/100, /*ack*/0);
    rtt_on_send(&c, kPeer, out.b, out.len, 1000);

    Pkt in = make_pkt(/*seq*/7, /*ack*/100);
    uint32_t r = rtt_on_recv(&c, kPeer, in.b, in.len, 1170);

    CHECK_EQ(r, 170);
    CHECK_EQ(c.st.samples, 1);
    CHECK_EQ(c.st.unmatched, 0);
}

// The ack repeats on every datagram until the peer sees something newer.
// Only the advance may sample; the repeats must be inert.
void test_repeated_ack_is_not_resampled() {
    begin("a repeated ack does not produce a second sample");
    RttCtx c;
    rtt_init(&c, true, 15000, 0);

    Pkt a = make_pkt(100, 0); rtt_on_send(&c, kPeer, a.b, a.len, 1000);
    Pkt b = make_pkt(101, 0); rtt_on_send(&c, kPeer, b.b, b.len, 1010);

    Pkt in1 = make_pkt(7, 100);
    CHECK_EQ(rtt_on_recv(&c, kPeer, in1.b, in1.len, 1170), 170);

    // Same ack, much later.  If this sampled, it would report ~2 seconds.
    Pkt in2 = make_pkt(8, 100);
    CHECK_EQ(rtt_on_recv(&c, kPeer, in2.b, in2.len, 3000), 0);
    Pkt in3 = make_pkt(9, 100);
    CHECK_EQ(rtt_on_recv(&c, kPeer, in3.b, in3.len, 5000), 0);

    CHECK_EQ(c.st.samples, 1);
    CHECK_EQ(c.st.acks_seen, 1);
}

// Karn's algorithm: a sequence sent twice can never be attributed.
void test_retransmit_is_discarded() {
    begin("a retransmitted sequence yields no sample (Karn)");
    RttCtx c;
    rtt_init(&c, true, 15000, 0);

    Pkt out = make_pkt(100, 0);
    rtt_on_send(&c, kPeer, out.b, out.len, 1000);
    rtt_on_send(&c, kPeer, out.b, out.len, 1100);   // resend, same seq

    Pkt in = make_pkt(7, 100);
    CHECK_EQ(rtt_on_recv(&c, kPeer, in.b, in.len, 1250), 0);

    CHECK_EQ(c.st.sends_ambiguous, 1);
    CHECK_EQ(c.st.discarded_ambig, 1);
    CHECK_EQ(c.st.samples, 0);
    // The send is counted once, not twice: the resend spoils the slot rather
    // than claiming another.
    CHECK_EQ(c.st.sends_tracked, 1);
}

// An ack covers every sequence at or below it, so older outstanding slots are
// confirmed delivered and must not linger to match a later, unrelated ack.
void test_cumulative_ack_retires_older_slots() {
    begin("an ack retires every older outstanding sequence");
    RttCtx c;
    rtt_init(&c, true, 15000, 0);

    for (uint32_t s = 100; s <= 104; ++s) {
        Pkt p = make_pkt(s, 0);
        rtt_on_send(&c, kPeer, p.b, p.len, 1000 + (s - 100) * 10);
    }
    // Ack the newest: 100..103 are implicitly confirmed too.
    Pkt in = make_pkt(7, 104);
    CHECK_EQ(rtt_on_recv(&c, kPeer, in.b, in.len, 1200), 160);

    // Nothing outstanding should remain to be matched.
    Pkt late = make_pkt(8, 102);
    CHECK_EQ(rtt_on_recv(&c, kPeer, late.b, late.len, 1300), 0);
    CHECK_EQ(c.st.samples, 1);
}

// The game reuses one UDP socket across matches.  Without an explicit purge a
// new epoch's sequences would match the previous match's slots.
void test_purge_clears_peer() {
    begin("purge drops outstanding slots so a new epoch cannot match them");
    RttCtx c;
    rtt_init(&c, true, 15000, 0);

    Pkt out = make_pkt(100, 0);
    rtt_on_send(&c, kPeer, out.b, out.len, 1000);
    rtt_purge_peer(&c, kPeer);

    Pkt in = make_pkt(7, 100);
    CHECK_EQ(rtt_on_recv(&c, kPeer, in.b, in.len, 1170), 0);
    CHECK_EQ(c.st.samples, 0);
    CHECK_EQ(c.st.unmatched, 1);
}

// A stall must not be laundered into a plausible-looking RTT.
void test_implausible_sample_rejected() {
    begin("an implausibly long elapsed time is discarded, not reported");
    RttCtx c;
    rtt_init(&c, true, 15000, 0);

    Pkt out = make_pkt(100, 0);
    rtt_on_send(&c, kPeer, out.b, out.len, 1000);

    Pkt in = make_pkt(7, 100);
    CHECK_EQ(rtt_on_recv(&c, kPeer, in.b, in.len, 1000 + kRttMaxSampleMs + 1), 0);
    CHECK_EQ(c.st.discarded_range, 1);
    CHECK_EQ(c.st.samples, 0);
}

// Disabled must be inert, not merely quiet.
void test_disabled_is_inert() {
    begin("disabled records nothing at all");
    RttCtx c;
    rtt_init(&c, false, 15000, 0);

    Pkt out = make_pkt(100, 0);
    rtt_on_send(&c, kPeer, out.b, out.len, 1000);
    Pkt in = make_pkt(7, 100);
    CHECK_EQ(rtt_on_recv(&c, kPeer, in.b, in.len, 1170), 0);
    CHECK_EQ(c.st.sends_tracked, 0);
    CHECK_EQ(c.st.acks_seen, 0);
}

// Short datagrams carry no header fields to read.
void test_short_payload_ignored() {
    begin("a datagram shorter than the header is ignored");
    RttCtx c;
    rtt_init(&c, true, 15000, 0);

    Pkt out = make_pkt(100, 0);
    rtt_on_send(&c, kPeer, out.b, kReorderSeqMinPay - 1, 1000);
    CHECK_EQ(c.st.sends_tracked, 0);
}

// Unreliable datagrams are never tracked: the ack acknowledges the RELIABLE
// sequence stream, and unreliable sequences repeat values that collide with
// reliable ones in flight.  Game 6 of 2026-08-15 measured the damage of
// tracking them: 1,122 ack advances, 65 samples, 1,052 discarded as falsely
// "ambiguous".
void test_unreliable_not_tracked() {
    begin("an unreliable datagram is not tracked and cannot spoil a sample");
    RttCtx c;
    rtt_init(&c, true, 15000, 0);

    Pkt rel = make_pkt(100, 0, /*reliable=*/true);
    rtt_on_send(&c, kPeer, rel.b, rel.len, 1000);
    // An unreliable datagram reusing the same sequence value: previously this
    // marked seq 100 ambiguous and killed the sample.
    Pkt unrel = make_pkt(100, 0, /*reliable=*/false);
    rtt_on_send(&c, kPeer, unrel.b, unrel.len, 1050);
    CHECK_EQ(c.st.sends_tracked, 1);
    CHECK_EQ(c.st.sends_ambiguous, 0);

    Pkt in = make_pkt(7, 100);
    CHECK_EQ(rtt_on_recv(&c, kPeer, in.b, in.len, 1170), 170);
    CHECK_EQ(c.st.samples, 1);
}

// The shared per-peer session formatter: content for a sampled peer, silence
// for an unknown or sampleless one.
void test_peer_session_formatter() {
    begin("the per-peer session line prints for sampled peers only");
    RttCtx c;
    rtt_init(&c, true, 15000, 0);
    char line[512];
    CHECK_EQ(rtt_format_peer_session(&c, kPeer, line, sizeof(line)), 0);

    Pkt out = make_pkt(100, 0); rtt_on_send(&c, kPeer, out.b, out.len, 1000);
    Pkt in  = make_pkt(7, 100); rtt_on_recv(&c, kPeer, in.b, in.len, 1170);
    CHECK(rtt_format_peer_session(&c, kPeer, line, sizeof(line)) > 0);
    CHECK(std::strstr(line, "srtt=170") != nullptr);
    CHECK(std::strstr(line, "over 1 samples") != nullptr);

    // After a purge the peer is gone and the formatter is silent - which is
    // exactly why the caller must format BEFORE purging.
    rtt_purge_peer(&c, kPeer);
    CHECK_EQ(rtt_format_peer_session(&c, kPeer, line, sizeof(line)), 0);
}

// The periodic line must fire on its interval and stay silent when a window
// produced nothing — a row of zeroes reads as "the link is fine".
void test_trace_cadence_and_silence() {
    begin("the periodic line fires on interval and stays silent when idle");
    RttCtx c;
    rtt_init(&c, true, 15000, 0);

    CHECK(!rtt_trace_due(&c, 14999));
    CHECK(rtt_trace_due(&c, 15000));
    CHECK(!rtt_trace_due(&c, 20000));
    CHECK(rtt_trace_due(&c, 30000));

    char line[512];
    // No samples yet for this peer -> nothing to say.
    CHECK_EQ(rtt_format_trace(&c, kPeer, line, sizeof(line)), 0);

    Pkt out = make_pkt(100, 0); rtt_on_send(&c, kPeer, out.b, out.len, 1000);
    Pkt in  = make_pkt(7, 100); rtt_on_recv(&c, kPeer, in.b, in.len, 1170);
    CHECK(rtt_format_trace(&c, kPeer, line, sizeof(line)) > 0);
    CHECK(std::strstr(line, "srtt=170") != nullptr);

    // After a window reset with no new samples, silent again.
    rtt_window_reset(&c);
    CHECK_EQ(rtt_format_trace(&c, kPeer, line, sizeof(line)), 0);
}

// Sequence numbers wrap; comparison must be modular, not absolute.
void test_wraparound() {
    begin("sequence wraparound is handled modularly");
    RttCtx c;
    rtt_init(&c, true, 15000, 0);

    const uint32_t near_top = 0xfffffffeu;
    Pkt a = make_pkt(near_top, 0);     rtt_on_send(&c, kPeer, a.b, a.len, 1000);
    Pkt b = make_pkt(near_top + 1, 0); rtt_on_send(&c, kPeer, b.b, b.len, 1010);
    Pkt d = make_pkt(near_top + 2, 0); rtt_on_send(&c, kPeer, d.b, d.len, 1020);  // wraps to 0

    Pkt in1 = make_pkt(7, near_top);
    CHECK_EQ(rtt_on_recv(&c, kPeer, in1.b, in1.len, 1100), 100);
    // 0 is AHEAD of 0xffffffff under modular comparison, so this must sample.
    Pkt in2 = make_pkt(8, near_top + 2);
    CHECK_EQ(rtt_on_recv(&c, kPeer, in2.b, in2.len, 1130), 110);
    CHECK_EQ(c.st.samples, 2);
}

// The stats line has to expose the denominators, so a filtered population is
// visible rather than hidden behind a clean mean.
void test_stats_line_exposes_denominators() {
    begin("the session line reports the filtered denominators");
    RttCtx c;
    rtt_init(&c, true, 15000, 0);

    Pkt out = make_pkt(100, 0);
    rtt_on_send(&c, kPeer, out.b, out.len, 1000);
    rtt_on_send(&c, kPeer, out.b, out.len, 1050);       // ambiguous
    Pkt in = make_pkt(7, 100);
    rtt_on_recv(&c, kPeer, in.b, in.len, 1200);

    char line[512];
    CHECK(rtt_format_stats(&c, line, sizeof(line)) > 0);
    CHECK(std::strstr(line, "ambiguous=1") != nullptr);
    CHECK(std::strstr(line, "discarded(ambig=1") != nullptr);
    CHECK(std::strstr(line, "samples=0") != nullptr);
}

}  // namespace

int main() {
    std::printf("net_rtt_test\n");
    test_basic_sample();
    test_repeated_ack_is_not_resampled();
    test_retransmit_is_discarded();
    test_cumulative_ack_retires_older_slots();
    test_purge_clears_peer();
    test_implausible_sample_rejected();
    test_disabled_is_inert();
    test_short_payload_ignored();
    test_unreliable_not_tracked();
    test_peer_session_formatter();
    test_trace_cadence_and_silence();
    test_wraparound();
    test_stats_line_exposes_denominators();

    std::printf("%s: %d checks, %d failures\n",
                g_failures ? "FAILED" : "ok", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
