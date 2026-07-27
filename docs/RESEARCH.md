# Known limits and research notes

Why the patch is shaped the way it is, and what measurement has ruled out.

## Known limits

- **It fixes tuning, not loss or congestion.** No receiver-side patch can fix a
  saturated uplink on the *sending* peer's end. That's a wired-ethernet or
  router-QoS problem for them.

- **Nobody gets inbound reordering — including under Proton.** This was believed
  to be a Windows-only gap until 2026-07-26, when a wire capture settled it. The
  game issues **overlapped** `WSARecvFrom` on every receive (28% return
  `WSA_IO_PENDING`), and the reorder hook deliberately bypasses overlapped calls
  — routing them through it froze the game at the loading screen back in V4.1.
  Across a 2.5-hour Proton session the buffer handled **zero packets** and
  emitted no `reorder_stats` at all. Every measured improvement to date belongs
  to the `[Net]` tuning, the socket buffers and DSCP marking. `BZ_IOCP_REORDER=1`
  exists but has never run on real Windows.

- **…and on these links there was nothing to reorder.** The same capture put
  out-of-order arrivals at **0.0-0.2%** across all three packet classes,
  corroborated by the game's own log: 6,998 of 7,012 discards were packets
  *already consumed*, and only 14 arrived early. What the links actually show is
  duplication (56-83% of inbound) and outright loss (10-27% of sequence numbers
  never arriving) — neither of which a reorder buffer can fix. That is why it
  ships off, and why building an IOCP receive path to reach it is not currently
  worth the risk.

- **Reordering is not free, and the drop counter hides the cost.** Holding a
  packet adds latency, and the game's drop count stops counting a packet the
  moment it's held — whether or not you're better off. The old "65% fewer drops"
  headline was never the whole story.

- **Reverse-engineered addresses are provisional.** The sanity gate in
  `shared/net_globals.h` only rejects wildly implausible values, not a
  plausibly-wrong address. Two entries were believed for weeks before being
  caught: see below.

- **Ping packets aren't yet exempt from receive buffering.** If ping replies ride
  the ordered queue, the buffer inflates the round-trip time a host measures
  against the kick threshold. `BZ_REORDER_MAX_HOLD_MS` caps the damage; a proper
  fast lane needs one capture session to identify the packet type.

- **The memory addresses are pinned to one game build.** If Rebellion patches the
  game, expect `net_patch: … VETOED` and stock behaviour — not a crash, but not
  the fix either.

## Things we learned the hard way

- **`net.ini` only loads through the mod system — and even then, found ≠
  applied.** A copy next to the exe is silently ignored. Delivered as a local mod
  the game logs `MOD FOUND`, yet a 2026-07-05 match proved the values still
  weren't used: the host ran `AutoKickTime = 45000` and kicked at the stock 15 s.
  Working theory is that only the session's *active* mod (the map's) is parsed.
  This is why V4.7 writes the values into memory instead.

- **`BZ_GOV_START` is the lever for the opening send rate — and writing
  `MinBandwidth` suppresses it.** `MinBandwidth` does not set the opening rate,
  despite what its comment claimed for weeks. Worse, on 2026-07-26 a match run
  with `BZ_NET_MINBANDWIDTH=40000` opened at 16000 and crawled to 31400 over 20
  minutes, while the same machine with that variable removed opened at 4000 and
  jumped to **40000 in the same second**. Two Windows testers, who never write
  `MinBandwidth`, show the identical instant 4000 → 40000 signature, one ramping
  on to 82100 B/s. Four observations, not yet a controlled A/B. `MinBandwidth` is
  no longer written by default and its address is flagged unconfirmed.

- **The reorder sequence field was read wrong.** u32 little-endian at payload
  offset 13, when it is **u16 big-endian at offset 16**. Two datagrams with
  different sequence numbers both read `0x380000c1`. Corrected, with regression
  tests covering the 16-bit wrap.

- **The send governor runs on every machine**, not just the host's — contrary to
  long-standing community wisdom. Only auto-kick is host-enforced.

- **The raw warp count means nothing.** ~90% of `Possible Large Warp` lines are
  sub-metre corrections. Filter by distance or you're measuring noise.

- **The game imports winsock functions by ordinal** and sends all P2P traffic via
  `WSASendTo`; `sendto` isn't in its import table at all.

- **Session counters used to die with an unclean exit.** Both proxies emitted
  their `session end:` summary only from `closesocket`. A game that exits without
  closing its P2P socket skipped it and discarded the whole measurement;
  `DLL_PROCESS_DETACH` now re-emits as a backstop.

- **The Windows installer must not hardcode the prebuilt's hash.** It is cached
  by raw.githubusercontent and saved to disk by users, so refreshing the prebuilt
  stranded every copy made before it. The hash is read from the `.sha256` sidecar
  at run time instead.

---

Full data: [../resources/](../resources/) · [../test-logs/](../test-logs/) ·
[../CHANGELOG.md](../CHANGELOG.md)
