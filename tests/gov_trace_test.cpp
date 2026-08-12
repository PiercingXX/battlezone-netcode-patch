// tests/gov_trace_test.cpp — host tests for shared/gov_trace.h.
//
// The state machine takes its clock and its observations from the caller, so
// the whole thing runs here with no game and no Windows.  The two headline
// tests replay the two real V4.8 matches of 2026-07-26 from their logs:
// game 1, where the poke was written and did not stick, and game 2, where it
// did.  If a future change stops distinguishing those two, this fails.
//
//   make -C tests && ./tests/gov_trace_test

#include "../shared/gov_trace.h"

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

// Mirrors the governor thread's loop: poll every 100 ms, and on kGovBumped the
// caller writes the target into the global (here, into `live`).
struct Governor {
    GovTraceCfg   cfg;
    GovTraceState st;
    uint64_t      now = 1000;
    uint32_t      live = 0;

    // Counters standing in for the proxy's log lines.
    int bumped = 0, clamped = 0, held = 0, traced = 0;
    uint32_t last_clamp_observed = 0;
    uint64_t last_clamp_age = 0;

    Governor(uint32_t target) {
        gov_trace_cfg_defaults(&cfg, target, 4000);
        gov_trace_init(&st, now);
    }

    void poll(uint32_t value) {
        live = value;
        switch (gov_trace_step(&cfg, &st, live, now)) {
            case kGovBumped:  live = cfg.target; ++bumped; break;
            case kGovClamped: ++clamped;
                              last_clamp_observed = st.observed;
                              last_clamp_age = st.since_bump_ms;
                              break;
            case kGovHeld:    ++held; break;
            case kGovTrace: {
                uint32_t lo, hi, n;
                gov_trace_window(&st, &lo, &hi, &n);
                ++traced;
                break;
            }
            default: break;
        }
        now += 100;   // kGovPollMs
    }

    // Hold a value steady for `ms`.
    void steady(uint32_t value, uint64_t ms) {
        for (uint64_t t = 0; t < ms; t += 100) {
            poll(value);
        }
    }
};

// ── The two real matches ─────────────────────────────────────────────────────

// Game 1, 2026-07-26 18:43:22.082: the proxy logged the bump to 40000, and the
// game reported 16000 — stock MaxBandwidth — from 18:43:25.9, i.e. 3.8 s later,
// then ramped +100 B/s for twenty minutes.  The whole point of this file is
// that the proxy must now say so by itself.
void test_game1_poke_did_not_hold() {
    begin("game 1 (2026-07-26): a poke overwritten at +3.8 s is reported");
    Governor g(40000);
    g.steady(0, 1000);              // pre-match: governor not yet set up
    g.poll(4000);                   // cold start
    CHECK_EQ(g.bumped, 1);
    CHECK_EQ(g.live, 40000u);

    g.steady(40000, 3800);          // holds for 3.8 s...
    CHECK_EQ(g.clamped, 0);
    CHECK_EQ(g.held, 0);            // verify window is 10 s, so no verdict yet

    g.steady(16000, 500);           // ...then the game rewrites it to stock
    CHECK_EQ(g.clamped, 1);
    CHECK_EQ(g.last_clamp_observed, 16000u);
    CHECK(g.last_clamp_age >= 3800);

    // Ramping from 16000 must not re-report; one clamp is one finding.
    for (uint32_t r = 16100; r < 20000; r += 100) {
        g.steady(r, 200);
    }
    CHECK_EQ(g.clamped, 1);
    CHECK_EQ(g.held, 0);            // it never held, so it is never declared held
}

// Game 2, the same evening: 4000 -> 40000 instantly, ramping to 82100.
void test_game2_poke_held() {
    begin("game 2 (2026-07-26): a poke that sticks is declared held once");
    Governor g(40000);
    g.poll(4000);
    CHECK_EQ(g.bumped, 1);
    g.steady(40000, 11000);         // past the 10 s verify window
    CHECK_EQ(g.clamped, 0);
    CHECK_EQ(g.held, 1);

    // The ramp upward is normal and must not produce a second verdict.
    for (uint32_t r = 40100; r <= 82100; r += 100) {
        g.steady(r, 100);
    }
    CHECK_EQ(g.held, 1);
    CHECK_EQ(g.clamped, 0);
    CHECK_EQ(g.st.peak, 82100u);
}

// The 2026-08-02 evening, nine matches, seven players: the poke landed and the
// governor immediately began taking DownCount=50 steps off it as traffic
// flowed.  The proxy read 39650..39900 inside the verify window and declared
// POKE DID NOT HOLD eight times — every one a false alarm that would have
// thrown the whole evening out of a BZ_GOV_START A/B.  This replay is the fix's
// reason to exist.
void test_governor_downsteps_are_not_a_clamp() {
    begin("2026-08-02: DownCount steps off the poked baseline are not a clamp");
    Governor g(40000);
    g.poll(4000);
    CHECK_EQ(g.bumped, 1);
    g.steady(40000, 2500);          // holds while the lobby loads...
    // ...then traffic starts and the governor walks down 50 at a time.
    g.steady(39950, 1000);
    g.steady(39900, 1000);
    g.steady(39800, 1000);
    g.steady(39650, 1000);          // the lowest read of the real evening
    CHECK_EQ(g.clamped, 0);         // the old rule fired right here
    g.steady(39650, 6000);          // verify window closes
    CHECK_EQ(g.held, 1);
    CHECK_EQ(g.st.observed, 39650u); // caller can say "governor already adjusting"
    CHECK_EQ(g.clamped, 0);
}

// A revert to stock after some legitimate down-steps is still an overwrite.
void test_revert_after_downsteps_is_still_a_clamp() {
    begin("down-steps followed by a revert to stock still earn the clamp verdict");
    Governor g(40000);
    g.poll(4000);
    g.steady(40000, 2000);
    g.steady(39900, 2000);          // governor active — no verdict
    CHECK_EQ(g.clamped, 0);
    g.steady(16000, 500);           // then something rewrites it to stock
    CHECK_EQ(g.clamped, 1);
    CHECK_EQ(g.last_clamp_observed, 16000u);
    CHECK_EQ(g.held, 0);
}

// The 16000 arm sits ON the stock floor, so its own first down-steps read
// below both target and floor.  The down_slack guard is what keeps that arm
// scoreable at all.
void test_stock_floor_arm_downsteps_are_not_a_clamp() {
    begin("the 16000 arm's own down-steps are not a clamp (down_slack guard)");
    Governor g(16000);
    g.poll(4000);
    CHECK_EQ(g.bumped, 1);
    g.steady(16000, 2000);
    g.steady(15950, 2000);          // one DownCount step below the floor
    g.steady(15800, 2000);          // four steps
    CHECK_EQ(g.clamped, 0);
    g.steady(15800, 5000);          // verify window closes
    CHECK_EQ(g.held, 1);
    // A genuine re-init to the 4000 cold start is far below the slack and
    // still convicts.
    g.poll(4000);                   // next match: sentinel bumps again
    CHECK_EQ(g.st.bumps, 2u);
    g.steady(4000, 11000);          // write never takes this time
    CHECK(g.clamped >= 1);
}

// ── Contract details ─────────────────────────────────────────────────────────

void test_ramp_above_target_is_not_a_clamp() {
    begin("a governor ramping above the target is never reported as clamped");
    Governor g(16000);
    g.poll(4000);
    for (uint32_t r = 16000; r <= 60000; r += 100) {
        g.steady(r, 100);
    }
    CHECK_EQ(g.clamped, 0);
    CHECK_EQ(g.held, 1);
}

void test_one_verdict_per_match() {
    begin("a clamped match reports exactly once, however long it stays low");
    Governor g(40000);
    g.poll(4000);
    g.steady(16000, 1000);
    CHECK_EQ(g.clamped, 1);
    g.steady(16000, 120000);        // two more minutes below the target
    CHECK_EQ(g.clamped, 1);
    CHECK_EQ(g.held, 0);            // and it is never retroactively "held"
}

void test_second_match_rearms() {
    begin("a second match re-arms the verdict");
    Governor g(40000);
    g.poll(4000);
    g.steady(40000, 11000);
    CHECK_EQ(g.held, 1);
    CHECK_EQ(g.st.bumps, 1u);

    g.poll(4000);                   // next match starts
    CHECK_EQ(g.st.bumps, 2u);
    g.steady(16000, 200);           // and this time it is overwritten
    CHECK_EQ(g.clamped, 1);
}

void test_periodic_trace_and_window() {
    begin("the periodic trace fires on schedule and reports its own window");
    Governor g(40000);
    g.poll(4000);
    g.steady(40000, 11000);
    const int after_verify = g.traced;
    g.steady(50000, 16000);
    CHECK(g.traced > after_verify);
    // Window min/max reset each time, so the last window saw only 50000.
    uint32_t lo, hi, n;
    g.steady(50000, 500);
    gov_trace_window(&g.st, &lo, &hi, &n);
    CHECK_EQ(lo, 50000u);
    CHECK_EQ(hi, 50000u);
}

// The failure mode this file exists for, taken to its limit: the write never
// takes, so every read-back is still the 4000 sentinel.  Before the fix the
// sentinel check outranked the armed verdict, so this produced a fresh
// kGovBumped 10x/s forever and never one kGovClamped.
void test_poke_never_sticks() {
    begin("a write that never sticks earns bump->clamp verdicts, not bump spam");
    Governor g(40000);
    g.poll(4000);
    CHECK_EQ(g.bumped, 1);
    // The harness wrote 40000 on the bump, but this global never takes it:
    // feed the sentinel back on every subsequent poll for 30 s.
    for (int i = 0; i < 300; ++i) {
        g.poll(4000);
    }
    // One clamp verdict per verify window (10 s), each observing the
    // sentinel — not three hundred bumps and no verdict.
    CHECK(g.clamped >= 1);
    CHECK_EQ(g.last_clamp_observed, 4000u);
    CHECK(g.bumped <= 4);
    // The final bump of the run may still be awaiting its verdict.
    CHECK(g.bumped - g.clamped <= 1);
    CHECK_EQ(g.held, 0);
}

// A client re-syncing to the host's value at join reads the 4000 sentinel
// once before the host pushes its value (todo.md T10).  A single transient
// low read must not convict a poke the very next sample shows healthy — this
// is the single noisiest false positive in the analyzer output.
void test_client_rejoin_single_low_read_is_not_a_clamp() {
    begin("client rejoin: one transient low read is not a revert");
    Governor g(40000);
    g.poll(4000);
    CHECK_EQ(g.bumped, 1);
    g.steady(40000, 2000);          // poke holds while the lobby loads...
    g.poll(4000);                   // ...then the client re-syncs: one 4000 read
    CHECK_EQ(g.clamped, 0);         // old single-read rule fired right here
    g.steady(40000, 11000);         // host pushes its value; poke healthy again
    CHECK_EQ(g.clamped, 0);
    CHECK_EQ(g.held, 1);            // and it is declared held, not clamped
}

// A real revert still convicts: two consecutive reads at/below the stock floor
// (no healthy read in between) earn the clamp verdict.
void test_two_consecutive_low_reads_still_convict() {
    begin("two consecutive low reads still earn the clamp verdict");
    Governor g(40000);
    g.poll(4000);
    g.steady(40000, 1000);
    g.poll(16000);                  // first low read: only arms the check
    CHECK_EQ(g.clamped, 0);
    g.poll(16000);                  // second consecutive low read: convicts
    CHECK_EQ(g.clamped, 1);
    CHECK_EQ(g.last_clamp_observed, 16000u);
    CHECK_EQ(g.held, 0);
}

// The verify window timing out on the SAME read that arms low_seen: a reverted
// (cold-start sentinel) read at the timeout must be a clamp, not a hold.  The
// clamp check needs two consecutive qualifying reads and cannot fire on the
// first one, so without the fix this read fell through to the held check and
// promoted a session whose poke did not survive into the A/B sample set.
void test_timeout_with_reverted_read_is_a_clamp() {
    begin("verify timeout on a reverted read is a clamp, not a hold");
    Governor g(40000);
    g.poll(4000);
    CHECK_EQ(g.bumped, 1);
    g.steady(40000, 9899);          // holds healthy right up to the verify edge
    CHECK_EQ(g.clamped, 0);
    CHECK_EQ(g.held, 0);            // last healthy poll is still inside the window
    g.poll(4000);                   // reverted read: arms low_seen AND times out
    CHECK_EQ(g.clamped, 1);
    CHECK_EQ(g.last_clamp_observed, 4000u);
    CHECK_EQ(g.held, 0);
}

void test_trace_can_be_silenced() {
    begin("BZ_GOV_TRACE_MS=0 silences the periodic line but not the verdicts");
    Governor g(40000);
    g.cfg.trace_ms = 0;
    g.poll(4000);
    g.steady(40000, 60000);
    CHECK_EQ(g.traced, 0);
    CHECK_EQ(g.held, 1);
}

}  // namespace

int main() {
    std::printf("gov_trace V4.9 tests\n");
    test_game1_poke_did_not_hold();
    test_game2_poke_held();
    test_governor_downsteps_are_not_a_clamp();
    test_revert_after_downsteps_is_still_a_clamp();
    test_stock_floor_arm_downsteps_are_not_a_clamp();
    test_ramp_above_target_is_not_a_clamp();
    test_one_verdict_per_match();
    test_second_match_rearms();
    test_periodic_trace_and_window();
    test_poke_never_sticks();
    test_trace_can_be_silenced();
    test_client_rejoin_single_low_read_is_not_a_clamp();
    test_two_consecutive_low_reads_still_convict();
    test_timeout_with_reverted_read_is_a_clamp();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
