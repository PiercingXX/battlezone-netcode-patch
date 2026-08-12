// shared/net_globals.h — the game's [Net] tunables, poked directly in .data.
//
// Battlezone reads its whole [Net] configuration block out of net.ini at match
// start into a set of fixed .data globals.  Two live matches proved that path
// is unreliable: BZLogger reported `MOD FOUND net.ini at packaged_mods\9990001`
// while the values in that file were demonstrably NOT applied (a host with
// AutoKickTime=45000 kicked at the stock 15 s; a governor sat below the file's
// MinBandwidth).  Working theory is that only the session's *active* mod (the
// map's) is parsed.  Either way, shipping tuning through net.ini does not work.
//
// So we write the globals directly.  This is the same DRM-safe mechanism the
// governor cold-start fix uses and for the same reason: rewriting `.text` was
// verified to apply cleanly and then trip SteamStub's code-integrity check ~12 s
// later, while `.data` carries no integrity check.  Aligned 32-bit stores are
// atomic on x86.  The session parser rewrites these at every match start, so
// the caller re-asserts on a poll loop: within one tick of any match starting,
// our value wins over both net.ini and the stock default.
//
// Addresses were captured 2026-07-04 from the DRM-decrypted image of a running
// game (fixed base 0x400000, no ASLR).  They are only trusted after the caller
// confirms the build with the unique 15-byte governor signature, and each entry
// is additionally sanity-gated here: if the value the game currently holds is
// outside a plausible range, the address is not what we think it is for this
// build and that entry is vetoed permanently rather than written blind.

#ifndef BZNET_NET_GLOBALS_H
#define BZNET_NET_GLOBALS_H

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace bznet {

enum NetGlobalState {
    kNgPending = 0,   // not yet examined
    kNgApplied = 1,   // sanity-gated and being asserted
    kNgVetoed  = 2,   // gate failed; address looks wrong for this build
};

struct NetGlobal {
    uintptr_t   va;        // fixed virtual address of the .data global
    const char *ini_key;   // the net.ini [Net] key this mirrors
    const char *env;       // environment override
    uint32_t    stock;     // documented stock default, for the log
    uint32_t    lo;        // sanity gate: plausible minimum
    uint32_t    hi;        // sanity gate: plausible maximum
    uint32_t    want;      // target value; 0 = leave the game's value alone
    uint32_t    seen;      // value observed before our first write
    uint8_t     state;     // NetGlobalState
    uint8_t     changed;   // set by apply when this entry is worth logging
};

// The table.  `want` is filled in from the environment by the proxy before the
// poll loop starts; 0 means "leave whatever the game has".
//
// Defaults (when the BZ_NET_* preset is on) deliberately mirror the repo's
// net-ini/net.ini, because that file already encodes the intended tuning — it
// just never reliably reached the game.  This is that same configuration,
// delivered by a path that works.
//
// Sanity ranges are wide enough to admit both the stock default and our own
// written value, and narrow enough to reject a pointer, a zero, or garbage.
// ABOUT THE `stock` COLUMN — it is phase-dependent, and the first live
// values-as-found reads (2026-08-03, identical on a Windows host and a Linux
// client, taken at the MENU before our first write) prove it:
//
//   key            stock below   as found at menu
//   MinBandwidth   4000          1000
//   MaxBandwidth   16000         4000
//   UpCount        10            10    (match)
//   DownCount      5             5     (match)
//   MaxPing        300           700
//   MaxPingsLost   20            20    (match)
//   AutoKickStart  10000         10000 (match)
//   AutoKickPing   750           1000
//   AutoKickLoss   25            50
//   AutoKickTime   15000         10000
//
// The two are not in contradiction for MaxBandwidth: 16000 was READ LIVE
// mid-match on 2026-07-26 (the revert that built the governor read-back), so
// session init rewrites at least that global from its menu-time 4000.  For
// MaxPing and the AutoKick trio the `stock` numbers are from static RE and no
// in-match unpatched read exists yet — the menu reads may be pre-init values
// or the real ones.  Treat `stock` as "documented, unverified in-match" until
// an unpatched in-match values-as-found read settles it.  The sanity ranges
// below span both candidates in every case, so the veto gate is unaffected.
inline void net_globals_defaults(NetGlobal *t) {
    size_t i = 0;

    // ── Send governor ────────────────────────────────────────────────────────
    // NOT the opening send rate — tested 2026-07-26 and disproved.  A match run
    // with BZ_NET_MINBANDWIDTH=40000 (proxy log confirms `MinBandwidth 1000 ->
    // 40000` applied) still opened at 16000, and governor_patch logged
    // `cold-start caught, send-rate 4000 -> 16000`, meaning the live rate was
    // still 4000 at session setup.  Had this value been copied there, the rate
    // would have read 40000 and the cold-start watcher — which fires only on
    // exactly 4000 — could never have triggered.  It did not act as a floor
    // afterwards either: the rate climbed 16000 -> 16500 -> 17000 without ever
    // jumping to 40000.
    //
    // So the claim that this is copied into the live send rate at 0x56fdb2 is
    // wrong for this build, and by extension the identification of this address
    // as MinBandwidth is itself unconfirmed.  The sanity gate below only rejects
    // values outside [500, 2000000], which is far too wide to catch a wrong
    // address.  BZ_GOV_START is what actually sets the opening rate; tune the
    // ramp with that.  Left at 0 (leave the game's value) until the address is
    // re-verified.
    t[i++] = NetGlobal{0x008e8cf4, "MinBandwidth",  "BZ_NET_MINBANDWIDTH",  4000,   500, 2000000, 0, 0, kNgPending, 0};
    // Ceiling.  A 2026-07-19 session was observed ramping to 79,600 B/s, so a
    // "safe" low cap would be a regression; the governor is closed-loop
    // (it only climbs while ping < MaxPing) so a high ceiling is not itself a
    // risk.  320000 effectively removes the cap, matching net.ini.
    t[i++] = NetGlobal{0x008e8d08, "MaxBandwidth",  "BZ_NET_MAXBANDWIDTH",  16000, 1000, 4000000, 0, 0, kNgPending, 0};
    // Ramp rate.  Stock +10 bytes per ~3 s adjustment is dial-up-era pacing: a
    // 4-minute match never escapes the opening trickle.
    t[i++] = NetGlobal{0x008e8cf0, "UpCount",       "BZ_NET_UPCOUNT",       10,       1,  100000, 0, 0, kNgPending, 0};
    t[i++] = NetGlobal{0x008e8d10, "DownCount",     "BZ_NET_DOWNCOUNT",     5,        1,  100000, 0, 0, kNgPending, 0};
    // Ping ceiling before the governor starts cutting the send rate.  Stock 300
    // turns a jitter spike into a rate cut into fewer state updates into more
    // warping — and the cut itself keeps ping high, so the spiral sustains.
    // 450 gives a jittery link headroom before the governor bites.
    t[i++] = NetGlobal{0x008e8cec, "MaxPing",       "BZ_NET_MAXPING",       300,     50,   60000, 0, 0, kNgPending, 0};
    t[i++] = NetGlobal{0x008e8cfc, "MaxPingsLost",  "BZ_NET_MAXPINGSLOST",  20,       1,  100000, 0, 0, kNgPending, 0};

    // ── Auto-kick (host-enforced: only bites when this machine hosts) ────────
    // A tick is "bad" when ping > AutoKickPing OR loss > AutoKickLoss; once bad
    // continuously for AutoKickTime the host ejects the player.  Confirmed
    // twice in live matches against the stock 15 s timer.
    t[i++] = NetGlobal{0x008e8d0c, "AutoKickStart", "BZ_AUTOKICK_START",    10000, 1000,  600000, 0, 0, kNgPending, 0};
    t[i++] = NetGlobal{0x008e8cf8, "AutoKickPing",  "BZ_AUTOKICK_PING",     750,     50,   60000, 0, 0, kNgPending, 0};
    t[i++] = NetGlobal{0x008e8bfc, "AutoKickLoss",  "BZ_AUTOKICK_LOSS",     25,       1,  100000, 0, 0, kNgPending, 0};
    t[i++] = NetGlobal{0x008e8ce4, "AutoKickTime",  "BZ_AUTOKICK_TIME",     15000, 1000,  600000, 0, 0, kNgPending, 0};
}

constexpr size_t kNetGlobalCount = 10;

// Indices into the table, for the proxy's env parsing.
enum {
    kNgMinBandwidth = 0,
    kNgMaxBandwidth = 1,
    kNgUpCount      = 2,
    kNgDownCount    = 3,
    kNgMaxPing      = 4,
    kNgMaxPingsLost = 5,
    kNgAutoKickStart = 6,
    kNgAutoKickPing  = 7,
    kNgAutoKickLoss  = 8,
    kNgAutoKickTime  = 9,
};

// Preset values applied when the BZ_NET_TUNE preset is on.  These mirror
// net-ini/net.ini.  Index-aligned with the table above; 0 = leave alone.
constexpr uint32_t kNetTunePreset[kNetGlobalCount] = {
    0,       // MinBandwidth — writing this changed nothing measurable in a live
             // A/B (see the entry above) and the address is unconfirmed, so we
             // no longer write it by default.  BZ_NET_MINBANDWIDTH still forces
             // a value for anyone re-testing it.
    320000,  // MaxBandwidth
    50,      // UpCount — slower ramp, so the governor climbs less aggressively
    200,     // DownCount — bytes removed from the send budget per adjustment while
             // over MaxPing (the governor's back-off step, not a receive budget).
    450,     // MaxPing
    0,       // MaxPingsLost — no evidence a change helps; leave the game's
    0, 0, 0, 0,  // auto-kick group has its own preset (BZ_AUTOKICK_RELAX)
};

// Preset applied when BZ_AUTOKICK_RELAX is on (the default).
constexpr uint32_t kAutoKickRelaxPreset[kNetGlobalCount] = {
    0, 0, 0, 0, 0, 0,
    60000,   // AutoKickStart — long grace after a join
    2000,    // AutoKickPing  — a 2 s spike is not a kickable offence
    200,     // AutoKickLoss
    60000,   // AutoKickTime  — must be bad for a full minute, not 15 s
};

// ── Environment configuration ────────────────────────────────────────────────
// Kept here rather than in each proxy so the two cannot drift apart on what a
// given variable means.  Private helpers are prefixed ng_ to avoid colliding
// with the proxies' own env parsing.

inline bool ng_env_truthy(const char *s) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    if (std::strcmp(s, "1") == 0) {
        return true;
    }
    char lower[16] = {0};
    size_t len = std::strlen(s);
    if (len >= sizeof(lower)) {
        len = sizeof(lower) - 1;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return std::strcmp(lower, "true") == 0
        || std::strcmp(lower, "yes") == 0
        || std::strcmp(lower, "on") == 0;
}

inline uint32_t ng_parse_env_u32(const char *name, uint32_t fallback) {
    const char *v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    char *end = nullptr;
    unsigned long parsed = std::strtoul(v, &end, 10);
    if (end == nullptr || *end != '\0' || parsed > 0xffffffffUL) {
        return fallback;
    }
    return static_cast<uint32_t>(parsed);
}

// True when a default-on switch is not explicitly disabled.
inline bool ng_default_on(const char *name) {
    const char *v = std::getenv(name);
    return (v == nullptr || *v == '\0') ? true : ng_env_truthy(v);
}

// Fill the table's targets from the environment.
//
// Two independent presets, both on by default:
//   BZ_NET_TUNE=0        restores the game's stock governor behaviour
//   BZ_AUTOKICK_RELAX=0  restores stock auto-kicking
// Either way, a per-key BZ_NET_* / BZ_AUTOKICK_* variable always wins, and 0
// explicitly means "leave the game's value alone".
inline void net_globals_configure(NetGlobal *t, size_t n) {
    const bool tune  = ng_default_on("BZ_NET_TUNE");
    const bool relax = ng_default_on("BZ_AUTOKICK_RELAX");

    for (size_t i = 0; i < n && i < kNetGlobalCount; ++i) {
        uint32_t preset = 0;
        if (tune) {
            preset = kNetTunePreset[i];
        }
        if (relax && kAutoKickRelaxPreset[i] != 0) {
            preset = kAutoKickRelaxPreset[i];
        }
        uint32_t want = ng_parse_env_u32(t[i].env, preset);
        // Never write something the sanity gate would reject anyway.
        if (want != 0 && (want < t[i].lo || want > t[i].hi)) {
            want = preset;
        }
        t[i].want = want;
    }
}

// True when any entry has something to do.
inline bool net_globals_any(const NetGlobal *t, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (t[i].want != 0) {
            return true;
        }
    }
    return false;
}

// One poll pass.  Reads each live value, gates the address on first contact,
// and writes the target where it differs.  Sets `changed` on entries the caller
// should log (first application, or a veto).  Returns how many were set.
//
// Caller must have confirmed the game build first — this trusts fixed
// addresses and will happily scribble on the wrong ones otherwise.
inline int net_globals_apply(NetGlobal *t, size_t n) {
    int notable = 0;
    for (size_t i = 0; i < n; ++i) {
        NetGlobal &g = t[i];
        if (g.want == 0 || g.state == kNgVetoed) {
            continue;
        }

        uint32_t *p = reinterpret_cast<uint32_t *>(g.va);
        const uint32_t live = *p;

        if (g.state == kNgPending) {
            g.seen = live;
            if (live < g.lo || live > g.hi) {
                // Not a plausible value for this tunable.  Either the address
                // moved in a game update or it never meant what we think.
                // Refuse to write rather than corrupt whatever this really is.
                g.state   = kNgVetoed;
                g.changed = 1;
                ++notable;
                continue;
            }
            g.state   = kNgApplied;
            g.changed = 1;
            ++notable;
        }

        if (live != g.want) {
            *p = g.want;
        }
    }
    return notable;
}

}  // namespace bznet

#endif  // BZNET_NET_GLOBALS_H
