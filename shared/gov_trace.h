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

    // Session totals, for the closing summary.
    uint64_t bumps;
    uint64_t clamps;
    uint32_t peak;              // highest send rate ever observed
    uint32_t last_seen;
};

constexpr uint32_t kGovVerifyMsDef     = 10000;  // the observed failure showed at +3.9 s
constexpr uint32_t kGovTraceMsDef      = 15000;
constexpr uint32_t kGovClampFloorDef   = 16000;  // stock MaxBandwidth
constexpr uint32_t kGovDownSlackDef    = 500;    // 10 DownCount=50 steps

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
    s->bumps          = 0;
    s->clamps         = 0;
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
}

// Feed one observation of the live send-rate global.  Returns at most one
// event; call again on the next poll.  Returning one event per call keeps the
// caller a plain switch and keeps the log one line per thing that happened.
inline GovEvent gov_trace_step(const GovTraceCfg *c, GovTraceState *s,
                               uint32_t live, uint64_t now) {
    s->last_seen = live;
    if (live > s->peak) {
        s->peak = live;
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
    if (live == c->cold_start && !s->armed &&
        (s->bumps == 0 || (now - s->bumped_at) >= c->verify_ms)) {
        s->bumps++;
        s->bumped_at = now;
        s->armed     = 1;
        s->observed  = live;
        return kGovBumped;
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
