// tests/net_globals_test.cpp — host-side tests for shared/net_globals.h
//
// This header writes raw 32-bit values to hardcoded addresses inside a running
// game.  If an address is wrong, the write lands on something else.  The sanity
// gate is the only thing standing between a stale address and memory
// corruption, so it is worth testing on its own.
//
// The table entries here point at ordinary variables instead of the game's
// .data, which exercises the same code with no game involved.
//
//   make -C tests run

#include "../shared/net_globals.h"
// For kGovColdStartSentinel / kGovDescentBandDef: the MinBandwidth floor exists
// to put the governor out of the cold-start sentinel's reach, and that is a
// relationship between the two headers, not a magic number in this one.
#include "../shared/gov_trace.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

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

// Each macro evaluates its arguments exactly once: several call sites pass
// net_globals_apply(), which advances state, and re-evaluating it for the
// failure message would both corrupt the test and misreport the value.
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); \
                            check(a_ == b_, #a, a_, b_); } while (0)
#define CHECK(x)       do { bool x_ = (x); check(x_, #x, (long long)x_, 1LL); } while (0)

void begin(const char *name) {
    g_case = name;
    std::printf("- %s\n", name);
}

// A one-entry table aimed at a caller-owned word, shaped like MaxBandwidth.
NetGlobal make_entry(uint32_t *target, uint32_t want) {
    NetGlobal g{};
    g.va      = reinterpret_cast<uintptr_t>(target);
    g.ini_key = "MaxBandwidth";
    g.env     = "BZ_NET_MAXBANDWIDTH";
    g.stock   = 16000;
    g.lo      = 1000;
    g.hi      = 4000000;
    g.want    = want;
    g.state   = kNgPending;
    return g;
}

// The happy path: a plausible live value is accepted and overwritten, and the
// pre-write value is recorded for the log.
void test_applies_plausible_value() {
    begin("a plausible live value is gated through and written");
    uint32_t live = 16000;                 // stock default
    NetGlobal t = make_entry(&live, 320000);

    CHECK_EQ(net_globals_apply(&t, 1), 1); // first pass is notable
    CHECK_EQ(t.state, kNgApplied);
    CHECK_EQ(t.seen, 16000u);              // what the game had
    CHECK_EQ(live, 320000u);               // what we wrote
    CHECK_EQ(t.changed, 1);

    // Steady state: no further writes, nothing more to log.
    t.changed = 0;
    CHECK_EQ(net_globals_apply(&t, 1), 0);
    CHECK_EQ(t.changed, 0);
    CHECK_EQ(live, 320000u);
}

// The whole point of the gate: an address that does not hold what we think must
// never be written.  A stale address after a game update looks exactly like this.
void test_vetoes_implausible_value() {
    begin("an implausible live value is vetoed, never written");
    uint32_t not_bandwidth = 0x0042f1a0;   // looks like a pointer, not a rate
    NetGlobal t = make_entry(&not_bandwidth, 320000);

    CHECK_EQ(net_globals_apply(&t, 1), 1);
    CHECK_EQ(t.state, kNgVetoed);
    CHECK_EQ(t.seen, 0x0042f1a0u);
    CHECK_EQ(not_bandwidth, 0x0042f1a0u);  // untouched

    // Veto is permanent: later passes must not retry it.
    t.changed = 0;
    CHECK_EQ(net_globals_apply(&t, 1), 0);
    CHECK_EQ(not_bandwidth, 0x0042f1a0u);
}

void test_vetoes_zero() {
    begin("a zeroed address is vetoed (uninitialised or wrong)");
    uint32_t zero = 0;
    NetGlobal t = make_entry(&zero, 320000);
    net_globals_apply(&t, 1);
    CHECK_EQ(t.state, kNgVetoed);
    CHECK_EQ(zero, 0u);
}

// The game's session parser rewrites these at every match start; we must win
// the next poll without needing a restart.
void test_reasserts_after_game_overwrites() {
    begin("the value is re-asserted after the game's session parser rewrites it");
    uint32_t live = 16000;
    NetGlobal t = make_entry(&live, 320000);
    net_globals_apply(&t, 1);
    CHECK_EQ(live, 320000u);

    live = 16000;                          // match start: parser writes stock
    t.changed = 0;
    net_globals_apply(&t, 1);
    CHECK_EQ(live, 320000u);               // reclaimed within one poll
    CHECK_EQ(t.changed, 0);                // but not re-logged every match
}

void test_zero_want_leaves_value_alone() {
    begin("want=0 means leave the game's value alone");
    uint32_t live = 16000;
    NetGlobal t = make_entry(&live, 0);
    CHECK_EQ(net_globals_apply(&t, 1), 0);
    CHECK_EQ(live, 16000u);
    CHECK_EQ(t.state, kNgPending);         // never even read
}

// The shipped table must be internally consistent: every documented stock
// default has to pass its own sanity gate, or the patch would veto itself on a
// stock game.
void test_shipped_table_admits_stock_defaults() {
    begin("every shipped entry's stock default passes its own sanity gate");
    NetGlobal tbl[kNetGlobalCount];
    net_globals_defaults(tbl);
    for (size_t i = 0; i < kNetGlobalCount; ++i) {
        const NetGlobal &g = tbl[i];
        check(g.stock >= g.lo && g.stock <= g.hi,
              g.ini_key, g.stock, (long long)g.lo);
        check(g.va != 0, "va set", (long long)g.va, 1);
        check(g.env != nullptr && g.env[0] == 'B', "env name set", 1, 1);
    }
}

// And every preset value we would write must also pass, for the same reason:
// the entry is re-gated against the value we ourselves left behind.
void test_shipped_presets_pass_their_own_gate() {
    begin("every preset value passes the gate it will later be re-read through");
    NetGlobal tbl[kNetGlobalCount];
    net_globals_defaults(tbl);
    for (size_t i = 0; i < kNetGlobalCount; ++i) {
        const NetGlobal &g = tbl[i];
        for (uint32_t v : {kNetTunePreset[i], kAutoKickRelaxPreset[i]}) {
            if (v == 0) {
                continue;
            }
            check(v >= g.lo && v <= g.hi, g.ini_key, v, (long long)g.hi);
        }
    }
}

void test_env_presets_and_overrides() {
    begin("env: presets default on, per-key override wins, BZ_NET_TUNE=0 opts out");
    NetGlobal tbl[kNetGlobalCount];

    // Defaults: both presets on.
    unsetenv("BZ_NET_TUNE");
    unsetenv("BZ_AUTOKICK_RELAX");
    unsetenv("BZ_NET_MAXBANDWIDTH");
    net_globals_defaults(tbl);
    net_globals_configure(tbl, kNetGlobalCount);
    // MinBandwidth is written by default since V4.94: the 2026-07-26 A/B ruled
    // it out as the OPENING rate, which is a different question from whether it
    // floors a collapse.  2026-08-12 produced the collapse and it bottomed out
    // at the stock 4000, not this.  See the entry in shared/net_globals.h.
    CHECK_EQ(tbl[kNgMinBandwidth].want, 16000u);
    // V5: MaxBandwidth 320000 -> 64000 (nine instrumented matches never
    // measured a send rate above 24,872 B/s; the uncapped headroom was
    // untested surface).
    CHECK_EQ(tbl[kNgMaxBandwidth].want, 64000u);
    // V5.1 auto-kick: Loss stays at V5.0's reachable 50 (V4.9's 200 could not
    // fire, which is how a dead match ran on for five minutes), while Ping and
    // Time revert to the V4.9 values that demonstrably did not false-kick.
    // V5.0's 1000/20000 ejected a live tester twice on 2026-08-15, both times
    // at exactly AutoKickStart + AutoKickTime.  Pinned as a quartet because
    // the two failure modes are only balanced by all four together.
    CHECK_EQ(tbl[kNgAutoKickStart].want, 20000u);
    CHECK_EQ(tbl[kNgAutoKickPing].want,   2000u);
    CHECK_EQ(tbl[kNgAutoKickLoss].want,     50u);
    CHECK_EQ(tbl[kNgAutoKickTime].want,  60000u);
    CHECK_EQ(tbl[kNgMaxPingsLost].want, 0u);   // deliberately left alone
    // Recovery must outpace back-off 2:1, as stock intends (V4.94).
    CHECK_EQ(tbl[kNgUpCount].want, 100u);      // recovery step
    CHECK_EQ(tbl[kNgDownCount].want, 50u);     // over-MaxPing back-off step
    check(tbl[kNgUpCount].want >= 2 * tbl[kNgDownCount].want,
          "the governor recovers at least twice as fast as it cuts",
          tbl[kNgUpCount].want, 2 * tbl[kNgDownCount].want);

    // ...but anyone re-testing it can still force a value by env.
    setenv("BZ_NET_MINBANDWIDTH", "40000", 1);
    net_globals_defaults(tbl);
    net_globals_configure(tbl, kNetGlobalCount);
    CHECK_EQ(tbl[kNgMinBandwidth].want, 40000u);
    unsetenv("BZ_NET_MINBANDWIDTH");

    // Per-key override beats the preset.
    setenv("BZ_NET_MAXBANDWIDTH", "48000", 1);
    net_globals_defaults(tbl);
    net_globals_configure(tbl, kNetGlobalCount);
    CHECK_EQ(tbl[kNgMaxBandwidth].want, 48000u);

    // An override outside the sanity range falls back to the preset rather
    // than queuing a write the gate would reject.
    setenv("BZ_NET_MAXBANDWIDTH", "5", 1);
    net_globals_defaults(tbl);
    net_globals_configure(tbl, kNetGlobalCount);
    CHECK_EQ(tbl[kNgMaxBandwidth].want, 64000u);
    unsetenv("BZ_NET_MAXBANDWIDTH");

    // BZ_NET_TUNE=0 drops the governor preset but leaves auto-kick relax.
    setenv("BZ_NET_TUNE", "0", 1);
    net_globals_defaults(tbl);
    net_globals_configure(tbl, kNetGlobalCount);
    CHECK_EQ(tbl[kNgMinBandwidth].want, 0u);
    CHECK_EQ(tbl[kNgMaxBandwidth].want, 0u);
    CHECK_EQ(tbl[kNgAutoKickTime].want, 60000u);
    CHECK(net_globals_any(tbl, kNetGlobalCount));

    // Both off: nothing to do, and the proxy skips the thread entirely.
    setenv("BZ_AUTOKICK_RELAX", "0", 1);
    net_globals_defaults(tbl);
    net_globals_configure(tbl, kNetGlobalCount);
    CHECK(!net_globals_any(tbl, kNetGlobalCount));

    unsetenv("BZ_NET_TUNE");
    unsetenv("BZ_AUTOKICK_RELAX");
}

// DownCount is the governor's over-MaxPing back-off step (bytes removed from the
// send budget per adjustment while over MaxPing), NOT a receive budget.  All
// three tuning sources — net_globals.h, net-ini/net.ini, and both proxy READMEs
// — must state the same reconciled value.  net-ini/net.ini mirrors this compiled
// default; if either drifts, the mirror invariant below is what catches it.

// Read one [Net] key from net-ini/net.ini (relative to the tests/ CWD that
// `make -C tests run` uses). Returns -1 if absent/unparseable. This is what
// makes the mirror invariant REAL: pinning kNetTunePreset alone never reads
// net.ini, so a drift there stays green (2026-08-11 audit — the comment
// claimed the invariant was covered; it was not).
static long read_netini_key(const char *key) {
    // Try both CWDs: tests/ (make -C tests run) and repo root (a manual run).
    FILE *f = fopen("../net-ini/net.ini", "r");
    if (!f) f = fopen("net-ini/net.ini", "r");
    if (!f) return -1;
    char line[256];
    long val = -1;
    bool in_net = false;
    while (fgets(line, sizeof line, f)) {
        char *h = line;
        while (*h == ' ' || *h == '\t') ++h;
        if (*h == '[') { in_net = (strncmp(h, "[Net]", 5) == 0); continue; }
        if (!in_net) continue;
        char k[64]; long v;
        if (sscanf(h, "%63[^= \t] = %ld", k, &v) == 2 && strcmp(k, key) == 0) { val = v; break; }
    }
    fclose(f);
    return val;
}

// The mirror invariant, ACTUALLY implemented: net.ini must equal the compiled
// default for BOTH ramp knobs. Reverting either net.ini value now fails here
// (2026-08-11 audit: the old test only pinned kNetTunePreset and never read
// net.ini, so a drift stayed green — the exact vacuous-validator trap).
void test_netini_mirrors_compiled_defaults() {
    begin("net.ini UpCount/DownCount mirror the compiled preset defaults");
    long ini_up = read_netini_key("UpCount");
    long ini_down = read_netini_key("DownCount");
    CHECK_EQ((unsigned)ini_up, kNetTunePreset[kNgUpCount]);
    CHECK_EQ((unsigned)ini_down, kNetTunePreset[kNgDownCount]);
    CHECK_EQ((unsigned)ini_up, 100u);
    CHECK_EQ((unsigned)ini_down, 50u);
}

void test_downcount_reconciled() {
    begin("DownCount is reconciled to 50 and net.ini mirrors the compiled default");
    NetGlobal tbl[kNetGlobalCount];
    net_globals_defaults(tbl);

    // The reconciled value, pinned here so a drift in any source is a test fail.
    CHECK_EQ(kNetTunePreset[kNgDownCount], 50u);

    // The value must pass the sanity gate it will later be re-read through.
    const NetGlobal &g = tbl[kNgDownCount];
    check(kNetTunePreset[kNgDownCount] >= g.lo
              && kNetTunePreset[kNgDownCount] <= g.hi,
          "DownCount preset passes its own gate", kNetTunePreset[kNgDownCount],
          (long long)g.hi);

    // With the BZ_NET_TUNE preset on (default), the configured want is 200.
    unsetenv("BZ_NET_TUNE");
    unsetenv("BZ_AUTOKICK_RELAX");
    unsetenv("BZ_NET_DOWNCOUNT");
    net_globals_defaults(tbl);
    net_globals_configure(tbl, kNetGlobalCount);
    CHECK_EQ(tbl[kNgDownCount].want, 50u);
}

// V4.94: the collapse floor.  Pinned in all three sources, with net.ini held
// to the same mirror invariant the ramp knobs have and for the same reason.
void test_min_bandwidth_floor_is_asserted() {
    begin("MinBandwidth is written as the collapse floor and net.ini mirrors it");
    NetGlobal tbl[kNetGlobalCount];
    unsetenv("BZ_NET_TUNE");
    unsetenv("BZ_AUTOKICK_RELAX");
    unsetenv("BZ_NET_MINBANDWIDTH");
    net_globals_defaults(tbl);
    net_globals_configure(tbl, kNetGlobalCount);
    CHECK_EQ(kNetTunePreset[kNgMinBandwidth], 16000u);
    CHECK_EQ(tbl[kNgMinBandwidth].want, 16000u);
    CHECK_EQ((unsigned)read_netini_key("MinBandwidth"), kNetTunePreset[kNgMinBandwidth]);

    // The value must pass the gate it will be re-read through.
    const NetGlobal &g = tbl[kNgMinBandwidth];
    check(kNetTunePreset[kNgMinBandwidth] >= g.lo
              && kNetTunePreset[kNgMinBandwidth] <= g.hi,
          "MinBandwidth preset passes its own gate",
          kNetTunePreset[kNgMinBandwidth], (long long)g.hi);
    // ...and the gate must still admit the values the game itself is known to
    // hold here (1000 at the menu, 4000 at cold start), or the entry vetoes
    // itself before the first write.
    check(1000u >= g.lo && 4000u <= g.hi,
          "the gate admits the known as-found values", g.lo, g.hi);

    // The point of the floor is to put the governor out of reach of the 4000
    // cold-start sentinel, so that gov_trace.h's floor rescue becomes the
    // backstop rather than the mechanism.  A floor inside the descent band
    // would leave the collapse landing on the sentinel exactly as before.
    check(kNetTunePreset[kNgMinBandwidth] > kGovColdStartSentinel + kGovDescentBandDef,
          "the floor clears the cold-start sentinel and its descent band",
          kNetTunePreset[kNgMinBandwidth],
          kGovColdStartSentinel + kGovDescentBandDef);
}

// A wrong address must not take the rest of the table down with it.
void test_veto_is_per_entry() {
    begin("one vetoed entry does not stop the others");
    uint32_t good = 16000;
    uint32_t bad  = 0xdeadbeef;
    NetGlobal tbl[2] = {make_entry(&good, 320000), make_entry(&bad, 320000)};
    net_globals_apply(tbl, 2);
    CHECK_EQ(tbl[0].state, kNgApplied);
    CHECK_EQ(tbl[1].state, kNgVetoed);
    CHECK_EQ(good, 320000u);
    CHECK_EQ(bad, 0xdeadbeefu);
}

}  // namespace

int main() {
    std::printf("net_globals V4.7 tests\n");
    test_applies_plausible_value();
    test_vetoes_implausible_value();
    test_vetoes_zero();
    test_reasserts_after_game_overwrites();
    test_zero_want_leaves_value_alone();
    test_shipped_table_admits_stock_defaults();
    test_shipped_presets_pass_their_own_gate();
    test_downcount_reconciled();
    test_min_bandwidth_floor_is_asserted();
    test_netini_mirrors_compiled_defaults();
    test_env_presets_and_overrides();
    test_veto_is_per_entry();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
