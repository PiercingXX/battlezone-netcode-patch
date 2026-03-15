# Task: NAT Traversal — Reduce Relay Fallback Frequency

## Goal
Improve UDP hole-punch success rate so peers that currently fall back to the relay server (68.183.35.188:1339) instead achieve direct P2P connections.  Hook the `connect` / send path in the proxy to implement more aggressive simultaneous-open and shorten the time spent in the relay.

## Background
BZRNet uses a standard NAT hole-punch sequence: both sides send to each other's external address simultaneously, relying on the NAT device to open a pinhole.  When this fails (carrier-grade NAT, symmetric NAT, firewall) BZRNet falls back to routing all traffic through a relay server.

In session 1 (232149Z) player **ITA_r0y** (client ID `G193494139556766225`) failed direct P2P and was looped through `68.183.35.188:1339`, adding ≈154 ms latency.  The relay fallback happened *twice* in that session (19:23 and 19:26), suggesting the direct route was being renegotiated and failing again.  ITA_r0y also had a 999,592 µs (≈1 s) clock offset, which is typical of high-latency relay paths where round-trip timing inflates the skew estimate.  ITA_r0y dropped out of the session at 19:25:49.

## Observed Relay Failure Sequence (session 1 — 232149Z)

```
2026-03-14 19:23:31.921753 BZRNet P2P FAILED WAN Connect For Client G193494139556766225, ATTEMPTING RELAY
2026-03-14 19:23:32.032755 BZRNet P2P Starting RELAY Connect For Client G193494139556766225 - P2P Relay Response
2026-03-14 19:23:33.198776 BZRNet P2P Completed RELAY Connect For Client G193494139556766225, using address 68.183.35.188:1339
...
2026-03-14 19:26:36.141110 BZRNet P2P FAILED WAN Connect For Client G193494139556766225, ATTEMPTING RELAY
2026-03-14 19:26:36.252112 BZRNet P2P Starting RELAY Connect For Client G193494139556766225 - P2P Relay Response
2026-03-14 19:26:37.411133 BZRNet P2P Completed RELAY Connect For Client G193494139556766225, using address 68.183.35.188:1339
```

The `FAILED WAN Connect` → relay sequence completes in ~1.3 s, implying the hole-punch retry budget is short and the relay response is fast.

## What to Investigate

### 1. Packet injection during hole-punch window
BZRNet sends a finite number of simultaneous-open attempts.  The proxy could monitor UDP `sendto` calls targeting a non-relay WAN address, detect the hole-punch phase (packets before `Completed RELAY Connect`), and inject additional keepalive probes at a higher cadence (e.g. every 10 ms for 500 ms) to increase the chance of a pinhole opening before the game gives up.

### 2. Delay relay fallback notification
If the game decides to relay based on a timeout (e.g. no response within N ms), the proxy could artificially continue forwarding retried probes for an additional 200–500 ms before allowing the relay response to be acted on.  This requires intercepting either `connect()` or the game's relay-handshake recv path.

### 3. Post-relay reconnect attempt
After falling back to relay, periodically (every 30 s) inject a fresh hole-punch attempt; if it succeeds, transparently migrate the session off the relay by returning the direct address from the next `recvfrom` call.

## Affected Source Files

### Linux proxy
```
Linux/proton_dsound_proxy/src/dsound_proxy.cpp
```
Currently hooks: `setsockopt`, `WSASetSocketOption`, `getsockopt`, `WSAGetSocketOption`, `WSASocketW`, `GetProcAddress`, `socket`, `closesocket`.
`connect` / `sendto` / `WSAConnect` are **not yet hooked**.

### Windows proxy
```
Microslop/winmm_proxy/src/netcode_hooks.cpp
```
Same hook list — `connect`/`sendto` not yet hooked.

## Key Unknowns

- What triggers `FAILED WAN Connect`?  Grep the game binary for that string to find the timeout value (likely a hardcoded constant of 2–5 s).
- Does the game use `connect()` + `send()` or `sendto()` for each P2P packet?  The answer determines which hook intercepts the hole-punch traffic.
- The relay server `68.183.35.188:1339` — is it the official BZRNet relay or a community one?  Check whether it supports TURN-style channel data so future work could improve relay throughput.
- Wireshark capture of a relay-falling-back session would show whether the hole-punch packets actually reach the peer's WAN address.

## Player Reference

| Player     | Client ID              | Clock offset  | Outcome                  |
|------------|------------------------|---------------|--------------------------|
| ITA_r0y    | G193494139556766225    | 999,592 µs    | Relay, dropped at 19:25  |

## Done When
A test session with ITA_r0y (or a volunteer behind symmetric NAT) shows `Completed … direct` in BZLogger instead of `Completed RELAY Connect`, or relay handoff latency drops below 50 ms in the worst case.

## Test Bundle Reference
Parent dir: `/home/piercingxx/Downloads/Testing 1 with 2mb receive/`
Session 1 bundle: `deep_linux_unknown-host_20260314T232149Z/BZLogger.txt`
