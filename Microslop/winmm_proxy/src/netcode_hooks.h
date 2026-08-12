// netcode_hooks.h
// Battlezone 98 Redux - Windows netcode patch

#pragma once
// winsock2.h must come before windows.h to avoid the double-inclusion warning.
#include <winsock2.h>
#include <ws2tcpip.h>   // IP_TOS / IPPROTO_IP for DSCP marking
#include <windows.h>
#include <cstdint>

// ── Reorder buffer ───────────────────────────────────────────────────────────
// The constants, structures and state machine live in shared/reorder_core.h,
// shared verbatim with the Linux dsound proxy.  The two proxies used to carry
// character-identical copies of this logic, so every defect existed twice and
// every fix had to be written twice; tests/reorder_test.cpp covers the shared
// version natively.
#include "reorder_core.h"

// ── [Net] tunable globals ────────────────────────────────────────────────────
// Addresses, presets and the sanity gate for the game's whole [Net] config
// block, also shared verbatim with the Linux proxy.
#include "net_globals.h"
#include "send_pace.h"
#include "send_dampen.h"
#include "gov_trace.h"
#include "buffer_filter.h"

using namespace bznet;

// Called from DllMain's hook thread after process attach.
// Walks the game EXE's IAT, replaces WSASocketW and WSARecvFrom with our hooks,
// applies SO_SNDBUF / SO_RCVBUF to every UDP socket created, and enables
// per-peer OOO packet reordering via WSARecvFrom hook with drain-and-deliver.
void InstallNetcodeHooks();
// True once the hooks that carry the patch's behaviour are in place.
bool NetcodeHooksComplete();

// Called from DllMain during DLL_PROCESS_DETACH.
// Flushes binary packet logs if enabled and releases hook-owned resources.
void ShutdownNetcodeHooks();
