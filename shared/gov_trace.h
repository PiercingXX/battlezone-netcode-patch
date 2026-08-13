// shared/gov_trace.h — read-back instrumentation for the send-rate governor.
//
// Why this exists.  BZ_GOV_START pokes the governor's live send-rate global
// the instant it reads the hardcoded 4000 cold start.  In the 2026-07-26
// game-1 session the proxy logged `cold-start caught, send-rate 4000 -> 40000`
// at 18:43:22.082 — and the game's own log reported the governor at **16000**
// from 18:43:25.9 onward, ramping +100 B/s for the next twenty minutes.  16000
// is exactly stock `MaxBandwidth`: the same wrote-but-didn't-stick signature
// that got MinBandwidth's address flagged unconfirmed.  The same evening,
// game 2 shows the poke landing and holding all the way to 82100.
//
// So the poke is intermittent, and the only way anyone found out was by hand-
// correlating two logs from two programs with clocks an hour apart.  This
// state machine re-reads the global on the governor thread's existing poll and
// reports what it actually sees, so the next A/B can be scored from the proxy
// log alone.
//
// It is deliberately pure: no I/O, no Windows API, no clock of its own.  The
// caller supplies the observed value and the time and acts on the returned
// event, which is what lets tests/gov_trace_test.cpp replay the 18:43 session
// on the host with no game running.
//
// This observes; it does not correct.  Re-asserting the target on a detected
// clamp would change what the A/B measures, and the A/B has not been run yet.
//
// ── The sentinel is not only a match start (V4.94) ───────────────────────────
//
// Both proxies treat a read of exactly 4000 as "a match just started, poke it
// to BZ_GOV_START".  The comment justifying that (dsound_proxy.cpp, the
// governor_patch_thread banner) claims the ramp "moves it off 4000 immediately
// and never returns to exactly 4000".  The 2026-08-12 `xxMonke1.bzn` match
// falsified it.  Four runaway repair-kit objects flooded the reliable channel,
// ping went past MaxPing, and the governor took 54 consecutive DownCount steps
// over 107 seconds — 25,900 -> 4,150 -> the floor — no up-step in between.  The
// host's proxy logged `poke held ... reads 38000` at 20:36:03, i.e. a bump at
// 20:35:53: thirteen minutes into the match, the governor walked DOWN onto the
// sentinel and the patch read it as a match start.
//
// Two things were wrong with that.  The rate jumped 10x mid-match with nobody
// asking, and every such floor hit was counted as a match: the analyzer
// reported 32 "matches verified held" for an evening with three matches in it,
// and 126 on the host.  A sample set counted that way cannot score an A/B.
//
// So the sentinel is now classified by how it was ARRIVED AT.  A match start
// writes 4000 over a value that has been sitting still — the lobby's, or the
// previous match's parting rate, held for as long as the lobby lasted.  A
// collapse arrives from just above 4000, off a value that itself lasted one
// governor step.  `descent_band` and `descent_ms` are that distinction: a
// sentinel read reached from within `descent_band` bytes above it, off a value
// held for less than `descent_ms`, is kGovFloorRescue — not kGovBumped.
//
// Note it is the PREVIOUS value's lifetime that decides, not the age of the
// change into the sentinel.  That change is always "just now" whichever thing
// happened, so its own age tells you nothing.
//
// The rescue still writes the target, because with the game's own floor at
// 4000 the alternative is a match that spends the rest of its life at 4 kB/s;
// what changes is that it is named correctly, rate-limited, and counted apart
// from real match starts.  Raising the floor out of the sentinel's reach is
// the structural fix and lives in net_globals.h (MinBandwidth).
//
// Known limit: a match that ENDS while the governor is near the floor leaves a
// parting value inside the band, and the next match's genuine cold start would
// look like a descent — except that value has by then been unchanged for the
// whole lobby, which is what `descent_ms` tests.  A lobby shorter than
// `descent_ms` with a sub-band parting rate would miss one poke.  The match
// opens at the stock 4000 and says so in the log; that is the benign direction
// to fail in, and the reverse (a silent 10x jump mid-fight) is not.
//
// What counts as a clamp was narrowed for V4.91.  The first field evening
// with this instrumentation (2026-08-02, nine matches) reported POKE DID NOT
// HOLD eight times — and every failing read was 39650..39900 against a 40000
// target: the governor itself taking a few DownCount=50 steps off the poked
// baseline, which is the poke *working*.  The original rule (any read below
// target) assumed the governor only ever ramps up; DownCount exists precisely
// to walk it down.  A real overwrite reverts to stock MaxBandwidth (16000) or
// re-initialises to the 4000 cold start — so the clamp verdict now requires
// the read to be at/below the stock floor AND more than a few down-steps
// under the target.  Anything else below target survives to the verify window
// and is reported held, with the observed value for the log line to qualify.

#ifndef BZNET_GOV_TRACE_H
#define BZNET_GOV_TRACE_H

#include <cstdint>

namespace bznet {

// One event per poll, highest priority first.
enum GovEvent {
    kGovNone     = 0,
    kGovBumped   = 1,   // cold-start sentinel seen; caller writes cfg->target
    kGovClamped  = 2,   // read back BELOW the target: something overwrote us
    kGovHeld     = 3,   // verify window closed with the poke intact
    kGovTrace    = 4,   // periodic observation is due
    kGovFloorRescue = 5,// the sentinel arrived at by descent: the governor
                        // collapsed onto its floor mid-match.  The caller
                        // writes cfg->target exactly as for a bump, but this
                        // is NOT a match start and must not be counted as one.
};

struct GovTraceCfg {
    uint32_t target;        // value written at cold start (BZ_GOV_START)
    uint32_t cold_start;    // the hardcoded sentinel, 4000
    uint32_t verify_ms;     // how long after a bump to declare it held
    uint32_t trace_ms;      // periodic observation interval, 0 = off
    uint32_t clamp_floor;   // stock MaxBandwidth; an armed read at/below this
                            // is an overwrite, not governor adjustment
    uint32_t down_slack;    // reads within this of the target are legitimate
                            // DownCount steps, never a clamp (guards targets
                            // at or near the stock floor, e.g. the 16000 arm)
    uint32_t descent_band;  // a sentinel read reached from within this many
                            // bytes ABOVE it was walked onto, not written: the
                            // governor hitting its floor, not a match start
    uint32_t descent_ms;    // ...and only if that value changed this recently.
                            // A lobby holds its value for minutes; a collapse
                            // steps every couple of seconds.  Doubles as the
                            // rescue cooldown.
};

struct GovTraceState {
    // Verification of the most recent bump.
    uint64_t bumped_at;
    uint32_t armed;             // 1 while a bump is awaiting its verdict
    uint32_t low_seen;          // 1 when the immediately preceding poll was a
                                // qualifying low read; two consecutive are
                                // required before a revert is declared
    uint64_t last_clamp_ms;

    // Rolling window for the periodic trace.
    uint64_t last_trace_ms;
    uint32_t win_min;
    uint32_t win_max;
    uint32_t win_samples;

    // What the caller should report about the event just returned.
    uint32_t observed;          // the value that triggered it
    uint64_t since_bump_ms;     // age of the bump, for kGovClamped / kGovHeld

    // Arrival tracking, so a sentinel read can be told apart from a match
    // start by where it came from (see the header comment).
    uint64_t polls;             // observations fed so far; 0 = nothing to
                                // compare against, so the first is a bump
    uint32_t prev_distinct;     // the value held before the most recent change
    uint64_t last_change_ms;    // when the observed value last changed
    uint64_t prev_hold_ms;      // how long prev_distinct was held before it
                                // changed.  This, not "how long ago", is the
                                // lobby test: the change INTO the sentinel is
                                // always "just now", so its own age says
                                // nothing.  A collapse steps every 2 s; a
                                // lobby holds its parting value for minutes.
    uint64_t last_rescue_ms;

    // Session totals, for the closing summary.
    uint64_t bumps;
    uint64_t clamps;
    uint64_t floor_rescues;     // counted apart from bumps: these are NOT
                                // matches and must never enter an A/B set
    uint32_t peak;              // highest send rate ever observed
    uint32_t last_seen;
};

// The game's hardcoded per-match cold start.  Both proxies pass this as
// GovTraceCfg::cold_start; it is named here so there is one definition for
// anything that has to reason about it — net_globals.h's MinBandwidth floor
// has to clear it, for one.
constexpr uint32_t kGovColdStartSentinel = 4000;

constexpr uint32_t kGovVerifyMsDef     = 10000;  // the observed failure showed at +3.9 s
constexpr uint32_t kGovTraceMsDef      = 15000;
constexpr uint32_t kGovClampFloorDef   = 16000;  // stock MaxBandwidth
constexpr uint32_t kGovDownSlackDef    = 500;    // 10 DownCount=50 steps
// The widest single step the governor can be seen to take between two 100 ms
// polls.  The 2026-08-12 collapse stepped 400 B/s every 2 s with DownCount=200,
// so 2000 is an order of magnitude of headroom and still less than half the
// sentinel — a match start writes 4000 over a value nowhere near this close.
constexpr uint32_t kGovDescentBandDef  = 2000;
// A collapse steps every couple of seconds; a lobby sits unchanged for minutes.
constexpr uint32_t kGovDescentMsDef    = 30000;

inline void gov_trace_init(GovTraceState *s, uint64_t now) {
    s->bumped_at      = 0;
    s->armed          = 0;
    s->low_seen       = 0;
    s->last_clamp_ms  = 0;
    s->last_trace_ms  = now;
    s->win_min        = 0xffffffffu;
    s->win_max        = 0;
    s->win_samples    = 0;
    s->observed       = 0;
    s->since_bump_ms  = 0;
    s->polls          = 0;
    s->prev_distinct  = 0;
    s->last_change_ms = now;
    s->prev_hold_ms   = 0;
    s->last_rescue_ms = 0;
    s->bumps          = 0;
    s->clamps         = 0;
    s->floor_rescues  = 0;
    s->peak           = 0;
    s->last_seen      = 0;
}

inline void gov_trace_cfg_defaults(GovTraceCfg *c, uint32_t target, uint32_t cold_start) {
    c->target         = target;
    c->cold_start     = cold_start;
    c->verify_ms      = kGovVerifyMsDef;
    c->trace_ms       = kGovTraceMsDef;
    c->clamp_floor    = kGovClampFloorDef;
    c->down_slack     = kGovDownSlackDef;
    c->descent_band   = kGovDescentBandDef;
    c->descent_ms     = kGovDescentMsDef;
}

// Feed one observation of the live send-rate global.  Returns at most one
// event; call again on the next poll.  Returning one event per call keeps the
// caller a plain switch and keeps the log one line per thing that happened.
inline GovEvent gov_trace_step(const GovTraceCfg *c, GovTraceState *s,
                               uint32_t live, uint64_t now) {
    // Where this observation came from, recorded before last_seen is
    // overwritten.  Only a CHANGE updates prev_distinct, because the poll runs
    // ~20x faster than the governor adjusts: comparing against the previous
    // poll alone would see "unchanged" almost every time and lose the arrival.
    const uint32_t prev  = s->last_seen;
    const bool     first = (s->polls == 0);
    s->polls++;
    if (!first && live != prev) {
        s->prev_distinct  = prev;
        s->prev_hold_ms   = now - s->last_change_ms;
        s->last_change_ms = now;
    }
    s->last_seen = live;
    if (live > s->peak) {
        s->peak = live;
    }

    // Was the sentinel WRITTEN here (a match start) or WALKED onto (the
    // governor collapsing to its floor mid-match)?  See the header comment for
    // the 2026-08-12 session this exists for.  `from` is the value immediately
    // before this one: the previous poll if it differed, otherwise the value
    // held before the most recent change — which is what a run of identical
    // polls sitting on the floor sees.
    bool descent_arrival = false;
    if (live == c->cold_start && !first) {
        const uint32_t from = (live != prev) ? prev : s->prev_distinct;
        if (from > c->cold_start &&
            (from - c->cold_start) <= c->descent_band &&
            s->prev_hold_ms <= c->descent_ms) {
            descent_arrival = true;
        }
    }

    // The cold-start sentinel: the caller writes the target and we start the
    // clock on proving it stuck.
    //
    // Only when no bump is awaiting its verdict: reading the sentinel back
    // while armed means the write did not take (or the game re-initialised to
    // exactly 4000 mid-verify, which deserves the same verdict), so it falls
    // through to the clamp check below.  And a re-bump is only fresh once the
    // verify window has passed — without that, a write that never sticks
    // would alternate bump/clamp at poll rate instead of producing one
    // verdict per window.
    if (live == c->cold_start && !descent_arrival && !s->armed &&
        (s->bumps == 0 || (now - s->bumped_at) >= c->verify_ms)) {
        s->bumps++;
        s->bumped_at = now;
        s->armed     = 1;
        s->observed  = live;
        return kGovBumped;
    }

    // A collapse onto the floor.  Not armed, because there is no match start to
    // verify: arming here is exactly what put 32 "matches verified held" into
    // an evening with three matches in it.  Rate-limited by descent_ms so a
    // governor parked on the floor reports once, not ten times a second — and
    // while suppressed this falls through, so the periodic trace keeps running.
    // Skipped entirely while armed: a sentinel read with a bump still awaiting
    // its verdict is a poke that did not stick, and belongs to the clamp check.
    if (descent_arrival && !s->armed &&
        (s->floor_rescues == 0 || (now - s->last_rescue_ms) >= c->descent_ms)) {
        s->floor_rescues++;
        s->last_rescue_ms = now;
        s->observed       = live;
        return kGovFloorRescue;
    }

    if (live < s->win_min) s->win_min = live;
    if (live > s->win_max) s->win_max = live;
    s->win_samples++;

    // Did the poke survive?  A read below the target is only an overwrite if
    // it reverted to (or below) the stock floor — the governor legitimately
    // walks DOWN from the poked baseline in DownCount steps the moment
    // traffic flows, and the 2026-08-02 evening showed exactly that: eight
    // matches reading 39650..39900 under a 40000 target, all of them the
    // poke working.  The down_slack guard keeps a target at/near the stock
    // floor (the 16000 arm) from tripping on its own first down-steps.
    //
    // A revert also needs TWO consecutive qualifying low reads (todo.md T10):
    // a client re-syncing to the host's value at join can read the 4000
    // sentinel once before the host pushes its value, and a single transient
    // read must not convict a poke the very next sample shows healthy — that
    // is the single noisiest false positive in the analyzer output.  The
    // first qualifying read only arms `low_seen`; the second consecutive one
    // (no healthy read in between) earns the verdict.
    //
    // Exactly one verdict per bump: a match either took the poke or it did
    // not, and after a clamp the rate spends the rest of the match below the
    // target by definition.  Re-reporting that every few seconds for twenty
    // minutes would bury the finding in its own repetitions.  The periodic
    // governor_trace line carries the ongoing value from here on.
    if (s->armed && live < c->target && live <= c->clamp_floor &&
        (c->target - live) > c->down_slack) {
        if (s->low_seen) {
            s->armed          = 0;
            s->clamps++;
            s->last_clamp_ms  = now;
            s->observed       = live;
            s->since_bump_ms  = now - s->bumped_at;
            s->low_seen       = 0;
            return kGovClamped;
        }
        s->low_seen = 1;
    } else {
        s->low_seen = 0;
    }

    if (s->armed && (now - s->bumped_at) >= c->verify_ms) {
        s->armed         = 0;
        s->observed      = live;
        s->since_bump_ms = now - s->bumped_at;
        // The verify window timed out on a reverted (cold-start sentinel) read
        // that just armed low_seen.  The clamp check above needs TWO consecutive
        // qualifying reads and cannot fire on the first one, so without this the
        // read fell through to the held check and promoted a session whose poke
        // did not survive into the A/B sample set.  A reverted read at the
        // timeout is a clamp, not a hold.
        if (s->low_seen) {
            s->clamps++;
            s->last_clamp_ms = now;
            s->low_seen      = 0;
            return kGovClamped;
        }
        return kGovHeld;
    }

    if (c->trace_ms && (now - s->last_trace_ms) >= c->trace_ms) {
        s->last_trace_ms = now;
        s->observed      = live;
        return kGovTrace;
    }

    return kGovNone;
}

// Min/max since the last trace, then reset the window.  Split from
// gov_trace_step so the caller can format the line before the reset.
inline void gov_trace_window(GovTraceState *s, uint32_t *lo, uint32_t *hi, uint32_t *n) {
    *lo = (s->win_samples ? s->win_min : 0);
    *hi = s->win_max;
    *n  = s->win_samples;
    s->win_min     = 0xffffffffu;
    s->win_max     = 0;
    s->win_samples = 0;
}

}  // namespace bznet

#endif  // BZNET_GOV_TRACE_H
