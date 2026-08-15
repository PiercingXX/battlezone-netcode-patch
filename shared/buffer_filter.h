// shared/buffer_filter.h — BZ_BUFFER_LOG_PEER, and honest reporting of the
// buffer logger's effective settings.
//
// Two V4.9 fixes live here, both from the same failed capture.
//
// 1. BZ_BUFFER_LOG_PEER was written into every tester's paste-ready launch
//    options by both logger scripts and **no proxy source read it**. It was a
//    knob that did nothing, offered to people trying to reduce capture volume.
//    It filters now.
//
// 2. The one successful capture asked for BZ_BUFFER_LOG_RING=500000 and ran
//    with 65,536 — the default — losing 48% of its events including the entire
//    match start. The clamp is 1024..1,000,000, so 500,000 was never clamped:
//    the variable simply was not in the environment, because the game had
//    already been launched before the launch options were pasted. Nothing said
//    so. Now the proxy records what it was *asked* for alongside what it used,
//    in the log and in the meta file, so a bundle proves it on its own.
//
// Pure and host-testable: no winsock, no Windows API, no environment access —
// the caller passes the strings in.

#ifndef BZNET_BUFFER_FILTER_H
#define BZNET_BUFFER_FILTER_H

#include <cstdint>
#include <cstring>

namespace bznet {

constexpr uint32_t kPeerFilterCap = 8;

struct PeerFilter {
    uint32_t count;                     // 0 = accept everything
    uint32_t ipv4[kPeerFilterCap];      // network byte order, as sockaddr_in holds it
    uint32_t rejected;                  // parse failures, for the log line
};

inline void peer_filter_init(PeerFilter *f) {
    f->count = 0;
    f->rejected = 0;
    for (uint32_t i = 0; i < kPeerFilterCap; ++i) {
        f->ipv4[i] = 0;
    }
}

// Parse one dotted-quad into network byte order.  Deliberately not inet_addr:
// that treats "10" and "0x7f.1" as addresses, so a typo in a launch option
// would silently filter to something nobody meant.  Four decimal octets or
// nothing.
inline bool peer_filter_parse_ipv4(const char *s, const char *end, uint32_t *out) {
    uint32_t parts[4] = {0, 0, 0, 0};
    uint32_t pi = 0;
    int digits = 0;
    uint32_t acc = 0;
    for (const char *p = s; p <= end; ++p) {
        const bool at_end = (p == end);
        if (!at_end && *p >= '0' && *p <= '9') {
            if (++digits > 3) {
                return false;
            }
            acc = acc * 10u + static_cast<uint32_t>(*p - '0');
            continue;
        }
        if (at_end || *p == '.') {
            if (digits == 0 || acc > 255 || pi > 3) {
                return false;
            }
            parts[pi++] = acc;
            acc = 0;
            digits = 0;
            continue;
        }
        return false;
    }
    if (pi != 4) {
        return false;
    }
    // Network byte order: first octet is the low byte on little-endian, which
    // is how sockaddr_in.sin_addr already holds it.  This word matches the
    // in-memory sin_addr ONLY on a little-endian host; the whole target is
    // win32 x86 under Wine/Windows, so that is an invariant here, not an
    // oversight — a big-endian port would need the <<24..<<0 construction.
    *out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return true;
}

// Parse a comma- or space-separated list.  An empty or null spec accepts
// everything, which is the default and the behaviour before V4.9.
inline void peer_filter_parse(PeerFilter *f, const char *spec) {
    peer_filter_init(f);
    if (spec == nullptr || *spec == '\0') {
        return;
    }
    const char *p = spec;
    while (*p != '\0') {
        while (*p == ',' || *p == ' ' || *p == '\t') {
            ++p;
        }
        const char *start = p;
        while (*p != '\0' && *p != ',' && *p != ' ' && *p != '\t') {
            ++p;
        }
        if (p == start) {
            continue;
        }
        uint32_t addr = 0;
        // `rejected` counts every token that is not being matched against,
        // whatever the reason — unparseable, or valid but past the cap.  A
        // 9-address list must say so in the log rather than silently record
        // nothing from address 9.
        if (peer_filter_parse_ipv4(start, p, &addr) && f->count < kPeerFilterCap) {
            f->ipv4[f->count++] = addr;
        } else {
            f->rejected++;
        }
    }
}

// Should an event from this peer be recorded?  An address of 0 means "no peer
// on this event" (an ioctlsocket record, say) and is always kept: filtering
// those out would hide the socket-mode answers the capture exists to give.
inline bool peer_filter_accepts(const PeerFilter *f, uint32_t ipv4_net_order) {
    if (f->count == 0 || ipv4_net_order == 0) {
        return true;
    }
    for (uint32_t i = 0; i < f->count; ++i) {
        if (f->ipv4[i] == ipv4_net_order) {
            return true;
        }
    }
    return false;
}

// Did the environment actually carry the setting the tester thought it did?
// `raw` is the env string exactly as read (null when unset).
enum EnvOutcome {
    kEnvUnset   = 0,   // not in the environment at all
    kEnvUsed    = 1,   // parsed and used as given
    kEnvClamped = 2,   // parsed but outside the allowed range
    kEnvBad     = 3,   // present and unparseable
};

inline EnvOutcome env_outcome(const char *raw, uint32_t parsed, uint32_t effective) {
    if (raw == nullptr || *raw == '\0') {
        return kEnvUnset;
    }
    if (parsed == 0 && effective != 0) {
        // parsed==0 is the callers' can't-parse sentinel, but an explicit
        // "0" also parses to 0 — that one was CLAMPED, not unparseable, and
        // this report exists to be honest about which.
        const char *p = raw;
        while (*p == ' ' || *p == '\t') ++p;
        bool digits = (*p >= '0' && *p <= '9');
        while (*p >= '0' && *p <= '9') ++p;
        while (*p == ' ' || *p == '\t') ++p;
        if (!(digits && *p == '\0')) {
            return kEnvBad;
        }
    }
    return (parsed == effective) ? kEnvUsed : kEnvClamped;
}

inline const char *env_outcome_name(EnvOutcome o) {
    switch (o) {
        case kEnvUnset:   return "NOT SET (the launch options did not reach the game)";
        case kEnvClamped: return "CLAMPED to the allowed range";
        case kEnvBad:     return "UNPARSEABLE";
        default:          return "as requested";
    }
}

}  // namespace bznet

#endif  // BZNET_BUFFER_FILTER_H
