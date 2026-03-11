// netcode_hooks.h
// Battlezone 98 Redux - Windows netcode patch

#pragma once
// winsock2.h must come before windows.h to avoid the double-inclusion warning.
#include <winsock2.h>
#include <windows.h>

// Called from DllMain's hook thread after process attach.
// Walks the game EXE's IAT, replaces WSASocketW with our hook,
// and applies SO_SNDBUF / SO_RCVBUF to every UDP socket created.
void InstallNetcodeHooks();
