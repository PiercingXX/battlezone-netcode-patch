// tests/buffer_filter_test.cpp — host tests for shared/buffer_filter.h.
//
//   make -C tests && ./tests/buffer_filter_test

#include "../shared/buffer_filter.h"

#include <cstdio>

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

// Build the same 32-bit value sockaddr_in.sin_addr holds for a.b.c.d.
uint32_t addr(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return a | (b << 8) | (c << 16) | (d << 24);
}

void test_no_filter_accepts_everything() {
    begin("an unset BZ_BUFFER_LOG_PEER records every peer");
    PeerFilter f;
    peer_filter_parse(&f, nullptr);
    CHECK_EQ(f.count, 0u);
    CHECK(peer_filter_accepts(&f, addr(203, 0, 113, 20)));
    peer_filter_parse(&f, "");
    CHECK(peer_filter_accepts(&f, addr(10, 0, 0, 1)));
}

void test_single_and_list() {
    begin("one address, and a comma- or space-separated list");
    PeerFilter f;
    peer_filter_parse(&f, "203.0.113.20");
    CHECK_EQ(f.count, 1u);
    CHECK(peer_filter_accepts(&f, addr(203, 0, 113, 20)));
    CHECK(!peer_filter_accepts(&f, addr(203, 0, 113, 22)));

    peer_filter_parse(&f, "203.0.113.20,203.0.113.21 10.0.0.1");
    CHECK_EQ(f.count, 3u);
    CHECK(peer_filter_accepts(&f, addr(203, 0, 113, 21)));
    CHECK(peer_filter_accepts(&f, addr(10, 0, 0, 1)));
    CHECK(!peer_filter_accepts(&f, addr(8, 8, 8, 8)));
}

void test_events_with_no_peer_are_always_kept() {
    begin("events with no peer address survive the filter");
    // ioctlsocket / WSAIoctl records carry no address. Filtering them out
    // would hide the socket-mode answers the capture exists to give.
    PeerFilter f;
    peer_filter_parse(&f, "203.0.113.20");
    CHECK(peer_filter_accepts(&f, 0));
}

void test_bad_addresses_are_rejected_not_guessed() {
    begin("a typo is rejected and counted, never silently reinterpreted");
    PeerFilter f;
    // inet_addr would read "10" as 0.0.0.10 and "0x7f.1" as something, so a
    // typo in a launch option would filter to an address nobody meant.
    peer_filter_parse(&f, "10");
    CHECK_EQ(f.count, 0u);
    CHECK_EQ(f.rejected, 1u);

    peer_filter_parse(&f, "1.2.3.4.5");
    CHECK_EQ(f.count, 0u);
    peer_filter_parse(&f, "256.1.1.1");
    CHECK_EQ(f.count, 0u);
    peer_filter_parse(&f, "1.2.3.");
    CHECK_EQ(f.count, 0u);
    peer_filter_parse(&f, "a.b.c.d");
    CHECK_EQ(f.count, 0u);
    peer_filter_parse(&f, "1.2.3.4x");
    CHECK_EQ(f.count, 0u);

    // A valid entry beside a bad one still works, and the bad one is counted.
    peer_filter_parse(&f, "nonsense,10.0.0.5");
    CHECK_EQ(f.count, 1u);
    CHECK_EQ(f.rejected, 1u);
    CHECK(peer_filter_accepts(&f, addr(10, 0, 0, 5)));
}

void test_boundaries() {
    begin("0.0.0.0 and 255.255.255.255 parse; the list is capped");
    PeerFilter f;
    peer_filter_parse(&f, "255.255.255.255");
    CHECK_EQ(f.count, 1u);
    CHECK(peer_filter_accepts(&f, addr(255, 255, 255, 255)));

    peer_filter_parse(&f, "1.1.1.1,2.2.2.2,3.3.3.3,4.4.4.4,5.5.5.5,"
                          "6.6.6.6,7.7.7.7,8.8.8.8,9.9.9.9,10.10.10.10");
    CHECK_EQ(f.count, kPeerFilterCap);
    // The two past the cap are not matched against, and the log must say so
    // rather than silently recording nothing from address 9 onward.
    CHECK_EQ(f.rejected, 2u);
}

// The capture that motivated all this: ring=500000 requested, 65536 used.
void test_env_outcome_names_the_failure() {
    begin("an absent env var is reported as absent, not as a clamp");
    // Absent: exactly the 2026-07-26 capture. The clamp is 1024..1000000, so
    // 500000 was never clamped -- the variable simply was not there.
    CHECK_EQ(env_outcome(nullptr, 65536, 65536), kEnvUnset);
    CHECK_EQ(env_outcome("", 65536, 65536), kEnvUnset);
    // Present and honoured.
    CHECK_EQ(env_outcome("500000", 500000, 500000), kEnvUsed);
    // Present but out of range.
    CHECK_EQ(env_outcome("99", 99, 1024), kEnvClamped);
    CHECK_EQ(env_outcome("9999999", 9999999, 1000000), kEnvClamped);
    // An explicit "0" parses to the callers' can't-parse sentinel, but it was
    // clamped, not unparseable — the report must not call it garbage.
    CHECK_EQ(env_outcome("0", 0, 1024), kEnvClamped);
    CHECK_EQ(env_outcome("banana", 0, 1024), kEnvBad);
}

}  // namespace

int main() {
    std::printf("buffer_filter V4.9 tests\n");
    test_no_filter_accepts_everything();
    test_single_and_list();
    test_events_with_no_peer_are_always_kept();
    test_bad_addresses_are_rejected_not_guessed();
    test_boundaries();
    test_env_outcome_names_the_failure();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
