# Test bundles

Capture bundles from `buffer-logging/buffer_logger_*.sh|ps1` and
`Linux/tester_diag.sh` / `Microslop/tester_diag.ps1`.

**Not in git.** `.gitignore` excludes `test_bundles/` entirely — a single
session runs to tens of megabytes. Whatever is here on your disk is the only
copy.

## Retention policy

1. **Keep the archive, not the extracted tree.** Every bundle ships as a
   `.tar.gz` / `.zip` *and* was being kept extracted beside it, doubling the
   size for nothing. Extract to a scratch directory when you need to look, and
   delete the tree afterwards. Removing the two duplicated trees on 2026-07-27
   took this directory from 38 MB to 2.4 MB.
2. **Keep every bundle that produced a published number.** Anything cited in
   `CHANGELOG.md`, `docs/RESEARCH.md` or `resources/` must survive, because the
   claim is only as good as the ability to re-derive it. The V4.8 loss figures
   were withdrawn in V4.9 precisely because someone could go back to the
   capture and check.
3. **Delete failed launches once they are understood.** A bundle whose
   `capture_verify.txt` says the settings never took, and whose cause is
   recorded, has nothing left to give. Note it in `test-logs/README.md` first.
4. **Never delete the last bundle for a platform.** There is still no Windows
   bundle of any kind.

## Verifying a bundle

`stop` now writes `capture_verify.txt` into every bundle and prints
`capture verified` or a loud `CAPTURE IS NOT CLEAN` block. Read it before
relying on a capture: the one successful capture of 2026-07-26 silently ran
with a 65,536-record ring instead of the 500,000 requested, discarding 48% of
its events including the entire match start.

## Current contents

| bundle | what it is |
|---|---|
| `buffer_linux_unknown-host_20260726T183554Z.tar.gz` | failed launch, no traffic. Kept only as the negative control for the `syncJoin` question. |
| `buffer_linux_unknown-host_20260726T183650Z.tar.gz` | the one real capture. Every number in `resources/BZ_P2P_HEADER.md` and `resources/RETRANSMIT_STORM.md` comes from it. **Do not delete.** Its ring wrapped: it covers 8.1 of the session's 15.2 minutes and misses the match start. |

Both are named `unknown-host` because the logger used only `hostname`, which is
not installed by default on Arch. Fixed in V4.9; future bundles carry the real
name.
