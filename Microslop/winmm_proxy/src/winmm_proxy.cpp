// winmm_proxy.cpp
// Battlezone 98 Redux - Windows netcode patch
//
// Forwards every winmm.dll export to the real System32\winmm.dll.
// Each stub resolves lazily via GetProcAddress on first call so we
// avoid paying the resolve cost for functions the game never calls.
//
// g_hRealWinmm is loaded in dllmain.cpp before any of these stubs
// could possibly be invoked.

#include <windows.h>
// Re-define WINMMAPI to nothing: mmsystem.h marks all its declarations as
// __declspec(dllimport), but since we ARE the DLL being built (not a consumer
// of the system winmm.dll) those attributes don't apply to our definitions.
// Undefining it here suppresses the expected "redeclared without dllimport"
// warnings without needing -Wno-attributes in the build flags.
#ifdef WINMMAPI
#undef WINMMAPI
#endif
#define WINMMAPI
#include <mmsystem.h>

// Provided by dllmain.cpp
extern HMODULE g_hRealWinmm;
extern "C" FARPROC ResolveRealWinmm(const char* name);

// ---------------------------------------------------------------------------
// Helper macro – lazy resolver.
// Declares a static function-pointer, resolves it once, then calls through.
// Return type is "whatever the real function returns"; the cast is controlled
// per-stub below.
// ---------------------------------------------------------------------------
#define FORWARD(name) \
    static decltype(&::name) _fn = nullptr; \
    if (!_fn) _fn = reinterpret_cast<decltype(&::name)>(ResolveRealWinmm(#name)); \
    return _fn

// ---------------------------------------------------------------------------
// WOW32 / legacy thunks — the real winmm may or may not export these on
// a given Windows version.  We provide them as best-effort stubs; if the
// real DLL doesn't have them, GetProcAddress returns nullptr and the call
// becomes a no-op returning 0.  The game is very unlikely to call these.
// ---------------------------------------------------------------------------
#define FORWARD_FARPROC(name) \
    static FARPROC _fp_##name = nullptr; \
    if (!_fp_##name) _fp_##name = ResolveRealWinmm(#name); \
    if (_fp_##name) return (DWORD)(ULONG_PTR)_fp_##name; \
    return 0

extern "C"
{
// ---- MCI -----------------------------------------------------------------
MCIERROR WINAPI mciSendCommandA(MCIDEVICEID a,UINT b,DWORD_PTR c,DWORD_PTR d)     { FORWARD(mciSendCommandA)(a,b,c,d); }
MCIERROR WINAPI mciSendCommandW(MCIDEVICEID a,UINT b,DWORD_PTR c,DWORD_PTR d)     { FORWARD(mciSendCommandW)(a,b,c,d); }
MCIERROR WINAPI mciSendStringA(LPCSTR a,LPSTR b,UINT c,HWND d)                    { FORWARD(mciSendStringA)(a,b,c,d); }
MCIERROR WINAPI mciSendStringW(LPCWSTR a,LPWSTR b,UINT c,HWND d)                  { FORWARD(mciSendStringW)(a,b,c,d); }
BOOL     WINAPI mciGetErrorStringA(MCIERROR a,LPSTR b,UINT c)                      { FORWARD(mciGetErrorStringA)(a,b,c); }
BOOL     WINAPI mciGetErrorStringW(MCIERROR a,LPWSTR b,UINT c)                     { FORWARD(mciGetErrorStringW)(a,b,c); }
BOOL     WINAPI mciSetYieldProc(MCIDEVICEID a,YIELDPROC b,DWORD c)                 { FORWARD(mciSetYieldProc)(a,b,c); }
HTASK    WINAPI mciGetCreatorTask(MCIDEVICEID a)                                    { FORWARD(mciGetCreatorTask)(a); }
YIELDPROC WINAPI mciGetYieldProc(MCIDEVICEID a,LPDWORD b)                          { FORWARD(mciGetYieldProc)(a,b); }
BOOL     WINAPI mciExecute(LPCSTR a)                                                { FORWARD(mciExecute)(a); }
MCIDEVICEID WINAPI mciGetDeviceIDA(LPCSTR a)                                       { FORWARD(mciGetDeviceIDA)(a); }
MCIDEVICEID WINAPI mciGetDeviceIDW(LPCWSTR a)                                      { FORWARD(mciGetDeviceIDW)(a); }
MCIDEVICEID WINAPI mciGetDeviceIDFromElementIDA(DWORD a,LPCSTR b)                  { FORWARD(mciGetDeviceIDFromElementIDA)(a,b); }
MCIDEVICEID WINAPI mciGetDeviceIDFromElementIDW(DWORD a,LPCWSTR b)                 { FORWARD(mciGetDeviceIDFromElementIDW)(a,b); }
DWORD_PTR WINAPI mciGetDriverData(MCIDEVICEID a)                                   { FORWARD(mciGetDriverData)(a); }
BOOL     WINAPI mciSetDriverData(MCIDEVICEID a,DWORD_PTR b)                        { FORWARD(mciSetDriverData)(a,b); }

// ---- PlaySound -----------------------------------------------------------
BOOL WINAPI PlaySoundA(LPCSTR a,HMODULE b,DWORD c)                                 { FORWARD(PlaySoundA)(a,b,c); }
BOOL WINAPI PlaySoundW(LPCWSTR a,HMODULE b,DWORD c)                                { FORWARD(PlaySoundW)(a,b,c); }
BOOL WINAPI sndPlaySoundA(LPCSTR a,UINT b)                                         { FORWARD(sndPlaySoundA)(a,b); }
BOOL WINAPI sndPlaySoundW(LPCWSTR a,UINT b)                                        { FORWARD(sndPlaySoundW)(a,b); }

// ---- Timing --------------------------------------------------------------
// Note: timeGetTime is emitted as "timeGetTime_fwd" in the .def so that
// the compiler and linker don't trip over the timeGetTime macro in some
// MinGW mmsystem.h versions.
DWORD    WINAPI timeGetTime_fwd()                                                   { static FARPROC _fp=nullptr; if(!_fp) _fp=ResolveRealWinmm("timeGetTime"); return _fp ? ((DWORD(WINAPI*)())_fp)() : 0; }
MMRESULT WINAPI timeBeginPeriod(UINT a)                                             { FORWARD(timeBeginPeriod)(a); }
MMRESULT WINAPI timeEndPeriod(UINT a)                                               { FORWARD(timeEndPeriod)(a); }
MMRESULT WINAPI timeGetDevCaps(LPTIMECAPS a,UINT b)                                 { FORWARD(timeGetDevCaps)(a,b); }
MMRESULT WINAPI timeGetSystemTime(LPMMTIME a,UINT b)                                { FORWARD(timeGetSystemTime)(a,b); }
MMRESULT WINAPI timeKillEvent(UINT a)                                               { FORWARD(timeKillEvent)(a); }
MMRESULT WINAPI timeSetEvent(UINT a,UINT b,LPTIMECALLBACK c,DWORD_PTR d,UINT e)    { FORWARD(timeSetEvent)(a,b,c,d,e); }

// ---- Wave output ---------------------------------------------------------
MMRESULT WINAPI waveOutOpen(LPHWAVEOUT a,UINT b,LPCWAVEFORMATEX c,DWORD_PTR d,DWORD_PTR e,DWORD f) { FORWARD(waveOutOpen)(a,b,c,d,e,f); }
MMRESULT WINAPI waveOutClose(HWAVEOUT a)                                            { FORWARD(waveOutClose)(a); }
MMRESULT WINAPI waveOutWrite(HWAVEOUT a,LPWAVEHDR b,UINT c)                        { FORWARD(waveOutWrite)(a,b,c); }
MMRESULT WINAPI waveOutPause(HWAVEOUT a)                                            { FORWARD(waveOutPause)(a); }
MMRESULT WINAPI waveOutRestart(HWAVEOUT a)                                          { FORWARD(waveOutRestart)(a); }
MMRESULT WINAPI waveOutReset(HWAVEOUT a)                                            { FORWARD(waveOutReset)(a); }
MMRESULT WINAPI waveOutBreakLoop(HWAVEOUT a)                                        { FORWARD(waveOutBreakLoop)(a); }
MMRESULT WINAPI waveOutPrepareHeader(HWAVEOUT a,LPWAVEHDR b,UINT c)                { FORWARD(waveOutPrepareHeader)(a,b,c); }
MMRESULT WINAPI waveOutUnprepareHeader(HWAVEOUT a,LPWAVEHDR b,UINT c)              { FORWARD(waveOutUnprepareHeader)(a,b,c); }
MMRESULT WINAPI waveOutGetPosition(HWAVEOUT a,LPMMTIME b,UINT c)                   { FORWARD(waveOutGetPosition)(a,b,c); }
MMRESULT WINAPI waveOutGetPitch(HWAVEOUT a,LPDWORD b)                              { FORWARD(waveOutGetPitch)(a,b); }
MMRESULT WINAPI waveOutSetPitch(HWAVEOUT a,DWORD b)                                { FORWARD(waveOutSetPitch)(a,b); }
MMRESULT WINAPI waveOutGetPlaybackRate(HWAVEOUT a,LPDWORD b)                       { FORWARD(waveOutGetPlaybackRate)(a,b); }
MMRESULT WINAPI waveOutSetPlaybackRate(HWAVEOUT a,DWORD b)                         { FORWARD(waveOutSetPlaybackRate)(a,b); }
MMRESULT WINAPI waveOutGetVolume(HWAVEOUT a,LPDWORD b)                             { FORWARD(waveOutGetVolume)(a,b); }
MMRESULT WINAPI waveOutSetVolume(HWAVEOUT a,DWORD b)                               { FORWARD(waveOutSetVolume)(a,b); }
MMRESULT WINAPI waveOutGetErrorTextA(MMRESULT a,LPSTR b,UINT c)                    { FORWARD(waveOutGetErrorTextA)(a,b,c); }
MMRESULT WINAPI waveOutGetErrorTextW(MMRESULT a,LPWSTR b,UINT c)                   { FORWARD(waveOutGetErrorTextW)(a,b,c); }
UINT     WINAPI waveOutGetNumDevs()                                                  { FORWARD(waveOutGetNumDevs)(); }
MMRESULT WINAPI waveOutGetDevCapsA(UINT_PTR a,LPWAVEOUTCAPSA b,UINT c)             { FORWARD(waveOutGetDevCapsA)(a,b,c); }
MMRESULT WINAPI waveOutGetDevCapsW(UINT_PTR a,LPWAVEOUTCAPSW b,UINT c)             { FORWARD(waveOutGetDevCapsW)(a,b,c); }
MMRESULT WINAPI waveOutGetID(HWAVEOUT a,LPUINT b)                                  { FORWARD(waveOutGetID)(a,b); }
MMRESULT WINAPI waveOutMessage(HWAVEOUT a,UINT b,DWORD_PTR c,DWORD_PTR d)         { FORWARD(waveOutMessage)(a,b,c,d); }

// ---- Wave input ----------------------------------------------------------
MMRESULT WINAPI waveInOpen(LPHWAVEIN a,UINT b,LPCWAVEFORMATEX c,DWORD_PTR d,DWORD_PTR e,DWORD f) { FORWARD(waveInOpen)(a,b,c,d,e,f); }
MMRESULT WINAPI waveInClose(HWAVEIN a)                                              { FORWARD(waveInClose)(a); }
MMRESULT WINAPI waveInAddBuffer(HWAVEIN a,LPWAVEHDR b,UINT c)                      { FORWARD(waveInAddBuffer)(a,b,c); }
MMRESULT WINAPI waveInStart(HWAVEIN a)                                              { FORWARD(waveInStart)(a); }
MMRESULT WINAPI waveInStop(HWAVEIN a)                                               { FORWARD(waveInStop)(a); }
MMRESULT WINAPI waveInReset(HWAVEIN a)                                              { FORWARD(waveInReset)(a); }
MMRESULT WINAPI waveInPrepareHeader(HWAVEIN a,LPWAVEHDR b,UINT c)                  { FORWARD(waveInPrepareHeader)(a,b,c); }
MMRESULT WINAPI waveInUnprepareHeader(HWAVEIN a,LPWAVEHDR b,UINT c)                { FORWARD(waveInUnprepareHeader)(a,b,c); }
MMRESULT WINAPI waveInGetPosition(HWAVEIN a,LPMMTIME b,UINT c)                     { FORWARD(waveInGetPosition)(a,b,c); }
MMRESULT WINAPI waveInGetErrorTextA(MMRESULT a,LPSTR b,UINT c)                     { FORWARD(waveInGetErrorTextA)(a,b,c); }
MMRESULT WINAPI waveInGetErrorTextW(MMRESULT a,LPWSTR b,UINT c)                    { FORWARD(waveInGetErrorTextW)(a,b,c); }
UINT     WINAPI waveInGetNumDevs()                                                   { FORWARD(waveInGetNumDevs)(); }
MMRESULT WINAPI waveInGetDevCapsA(UINT_PTR a,LPWAVEINCAPSA b,UINT c)               { FORWARD(waveInGetDevCapsA)(a,b,c); }
MMRESULT WINAPI waveInGetDevCapsW(UINT_PTR a,LPWAVEINCAPSW b,UINT c)               { FORWARD(waveInGetDevCapsW)(a,b,c); }
MMRESULT WINAPI waveInGetID(HWAVEIN a,LPUINT b)                                    { FORWARD(waveInGetID)(a,b); }
MMRESULT WINAPI waveInMessage(HWAVEIN a,UINT b,DWORD_PTR c,DWORD_PTR d)           { FORWARD(waveInMessage)(a,b,c,d); }

// ---- MIDI output ---------------------------------------------------------
MMRESULT WINAPI midiOutOpen(LPHMIDIOUT a,UINT b,DWORD_PTR c,DWORD_PTR d,DWORD e)  { FORWARD(midiOutOpen)(a,b,c,d,e); }
MMRESULT WINAPI midiOutClose(HMIDIOUT a)                                            { FORWARD(midiOutClose)(a); }
MMRESULT WINAPI midiOutShortMsg(HMIDIOUT a,DWORD b)                                { FORWARD(midiOutShortMsg)(a,b); }
MMRESULT WINAPI midiOutLongMsg(HMIDIOUT a,LPMIDIHDR b,UINT c)                     { FORWARD(midiOutLongMsg)(a,b,c); }
MMRESULT WINAPI midiOutPrepareHeader(HMIDIOUT a,LPMIDIHDR b,UINT c)               { FORWARD(midiOutPrepareHeader)(a,b,c); }
MMRESULT WINAPI midiOutUnprepareHeader(HMIDIOUT a,LPMIDIHDR b,UINT c)             { FORWARD(midiOutUnprepareHeader)(a,b,c); }
MMRESULT WINAPI midiOutReset(HMIDIOUT a)                                            { FORWARD(midiOutReset)(a); }
MMRESULT WINAPI midiOutGetVolume(HMIDIOUT a,LPDWORD b)                             { FORWARD(midiOutGetVolume)(a,b); }
MMRESULT WINAPI midiOutSetVolume(HMIDIOUT a,DWORD b)                               { FORWARD(midiOutSetVolume)(a,b); }
MMRESULT WINAPI midiOutCacheDrumPatches(HMIDIOUT a,UINT b,WORD* c,UINT d)         { FORWARD(midiOutCacheDrumPatches)(a,b,c,d); }
MMRESULT WINAPI midiOutCachePatches(HMIDIOUT a,UINT b,WORD* c,UINT d)             { FORWARD(midiOutCachePatches)(a,b,c,d); }
MMRESULT WINAPI midiOutGetErrorTextA(MMRESULT a,LPSTR b,UINT c)                   { FORWARD(midiOutGetErrorTextA)(a,b,c); }
MMRESULT WINAPI midiOutGetErrorTextW(MMRESULT a,LPWSTR b,UINT c)                  { FORWARD(midiOutGetErrorTextW)(a,b,c); }
UINT     WINAPI midiOutGetNumDevs()                                                  { FORWARD(midiOutGetNumDevs)(); }
MMRESULT WINAPI midiOutGetDevCapsA(UINT_PTR a,LPMIDIOUTCAPSA b,UINT c)            { FORWARD(midiOutGetDevCapsA)(a,b,c); }
MMRESULT WINAPI midiOutGetDevCapsW(UINT_PTR a,LPMIDIOUTCAPSW b,UINT c)            { FORWARD(midiOutGetDevCapsW)(a,b,c); }
MMRESULT WINAPI midiOutGetID(HMIDIOUT a,LPUINT b)                                  { FORWARD(midiOutGetID)(a,b); }
MMRESULT WINAPI midiOutMessage(HMIDIOUT a,UINT b,DWORD_PTR c,DWORD_PTR d)        { FORWARD(midiOutMessage)(a,b,c,d); }

// ---- MIDI input ----------------------------------------------------------
MMRESULT WINAPI midiInOpen(LPHMIDIIN a,UINT b,DWORD_PTR c,DWORD_PTR d,DWORD e)   { FORWARD(midiInOpen)(a,b,c,d,e); }
MMRESULT WINAPI midiInClose(HMIDIIN a)                                              { FORWARD(midiInClose)(a); }
MMRESULT WINAPI midiInAddBuffer(HMIDIIN a,LPMIDIHDR b,UINT c)                     { FORWARD(midiInAddBuffer)(a,b,c); }
MMRESULT WINAPI midiInStart(HMIDIIN a)                                              { FORWARD(midiInStart)(a); }
MMRESULT WINAPI midiInStop(HMIDIIN a)                                               { FORWARD(midiInStop)(a); }
MMRESULT WINAPI midiInReset(HMIDIIN a)                                              { FORWARD(midiInReset)(a); }
MMRESULT WINAPI midiInPrepareHeader(HMIDIIN a,LPMIDIHDR b,UINT c)                 { FORWARD(midiInPrepareHeader)(a,b,c); }
MMRESULT WINAPI midiInUnprepareHeader(HMIDIIN a,LPMIDIHDR b,UINT c)               { FORWARD(midiInUnprepareHeader)(a,b,c); }
MMRESULT WINAPI midiInGetErrorTextA(MMRESULT a,LPSTR b,UINT c)                    { FORWARD(midiInGetErrorTextA)(a,b,c); }
MMRESULT WINAPI midiInGetErrorTextW(MMRESULT a,LPWSTR b,UINT c)                   { FORWARD(midiInGetErrorTextW)(a,b,c); }
UINT     WINAPI midiInGetNumDevs()                                                   { FORWARD(midiInGetNumDevs)(); }
MMRESULT WINAPI midiInGetDevCapsA(UINT_PTR a,LPMIDIINCAPSA b,UINT c)              { FORWARD(midiInGetDevCapsA)(a,b,c); }
MMRESULT WINAPI midiInGetDevCapsW(UINT_PTR a,LPMIDIINCAPSW b,UINT c)              { FORWARD(midiInGetDevCapsW)(a,b,c); }
MMRESULT WINAPI midiInGetID(HMIDIIN a,LPUINT b)                                    { FORWARD(midiInGetID)(a,b); }
MMRESULT WINAPI midiInMessage(HMIDIIN a,UINT b,DWORD_PTR c,DWORD_PTR d)          { FORWARD(midiInMessage)(a,b,c,d); }
MMRESULT WINAPI midiConnect(HMIDI a,HMIDIOUT b,LPVOID c)                           { FORWARD(midiConnect)(a,b,c); }
MMRESULT WINAPI midiDisconnect(HMIDI a,HMIDIOUT b,LPVOID c)                        { FORWARD(midiDisconnect)(a,b,c); }

// ---- MIDI stream ---------------------------------------------------------
MMRESULT WINAPI midiStreamOpen(LPHMIDISTRM a,LPUINT b,DWORD c,DWORD_PTR d,DWORD_PTR e,DWORD f) { FORWARD(midiStreamOpen)(a,b,c,d,e,f); }
MMRESULT WINAPI midiStreamClose(HMIDISTRM a)                                       { FORWARD(midiStreamClose)(a); }
MMRESULT WINAPI midiStreamOut(HMIDISTRM a,LPMIDIHDR b,UINT c)                     { FORWARD(midiStreamOut)(a,b,c); }
MMRESULT WINAPI midiStreamPause(HMIDISTRM a)                                        { FORWARD(midiStreamPause)(a); }
MMRESULT WINAPI midiStreamPosition(HMIDISTRM a,LPMMTIME b,UINT c)                  { FORWARD(midiStreamPosition)(a,b,c); }
MMRESULT WINAPI midiStreamProperty(HMIDISTRM a,LPBYTE b,DWORD c)                   { FORWARD(midiStreamProperty)(a,b,c); }
MMRESULT WINAPI midiStreamRestart(HMIDISTRM a)                                      { FORWARD(midiStreamRestart)(a); }
MMRESULT WINAPI midiStreamStop(HMIDISTRM a)                                         { FORWARD(midiStreamStop)(a); }

// ---- Aux / Mixer ---------------------------------------------------------
UINT     WINAPI auxGetNumDevs()                                                      { FORWARD(auxGetNumDevs)(); }
MMRESULT WINAPI auxGetDevCapsA(UINT_PTR a,LPAUXCAPSA b,UINT c)                    { FORWARD(auxGetDevCapsA)(a,b,c); }
MMRESULT WINAPI auxGetDevCapsW(UINT_PTR a,LPAUXCAPSW b,UINT c)                    { FORWARD(auxGetDevCapsW)(a,b,c); }
MMRESULT WINAPI auxGetVolume(UINT a,LPDWORD b)                                      { FORWARD(auxGetVolume)(a,b); }
MMRESULT WINAPI auxSetVolume(UINT a,DWORD b)                                        { FORWARD(auxSetVolume)(a,b); }
MMRESULT WINAPI auxOutMessage(UINT a,UINT b,DWORD_PTR c,DWORD_PTR d)              { FORWARD(auxOutMessage)(a,b,c,d); }

UINT     WINAPI mixerGetNumDevs()                                                    { FORWARD(mixerGetNumDevs)(); }
MMRESULT WINAPI mixerOpen(LPHMIXER a,UINT b,DWORD_PTR c,DWORD_PTR d,DWORD e)     { FORWARD(mixerOpen)(a,b,c,d,e); }
MMRESULT WINAPI mixerClose(HMIXER a)                                                { FORWARD(mixerClose)(a); }
DWORD    WINAPI mixerMessage(HMIXER a,UINT b,DWORD_PTR c,DWORD_PTR d)             { FORWARD(mixerMessage)(a,b,c,d); }
MMRESULT WINAPI mixerGetDevCapsA(UINT_PTR a,LPMIXERCAPSA b,UINT c)                { FORWARD(mixerGetDevCapsA)(a,b,c); }
MMRESULT WINAPI mixerGetDevCapsW(UINT_PTR a,LPMIXERCAPSW b,UINT c)                { FORWARD(mixerGetDevCapsW)(a,b,c); }
MMRESULT WINAPI mixerGetID(HMIXEROBJ a,UINT* b,DWORD c)                           { FORWARD(mixerGetID)(a,b,c); }
MMRESULT WINAPI mixerGetLineInfoA(HMIXEROBJ a,LPMIXERLINEA b,DWORD c)             { FORWARD(mixerGetLineInfoA)(a,b,c); }
MMRESULT WINAPI mixerGetLineInfoW(HMIXEROBJ a,LPMIXERLINEW b,DWORD c)             { FORWARD(mixerGetLineInfoW)(a,b,c); }
MMRESULT WINAPI mixerGetLineControlsA(HMIXEROBJ a,LPMIXERLINECONTROLSA b,DWORD c) { FORWARD(mixerGetLineControlsA)(a,b,c); }
MMRESULT WINAPI mixerGetLineControlsW(HMIXEROBJ a,LPMIXERLINECONTROLSW b,DWORD c) { FORWARD(mixerGetLineControlsW)(a,b,c); }
MMRESULT WINAPI mixerGetControlDetailsA(HMIXEROBJ a,LPMIXERCONTROLDETAILS b,DWORD c) { FORWARD(mixerGetControlDetailsA)(a,b,c); }
MMRESULT WINAPI mixerGetControlDetailsW(HMIXEROBJ a,LPMIXERCONTROLDETAILS b,DWORD c) { FORWARD(mixerGetControlDetailsW)(a,b,c); }
MMRESULT WINAPI mixerSetControlDetails(HMIXEROBJ a,LPMIXERCONTROLDETAILS b,DWORD c)  { FORWARD(mixerSetControlDetails)(a,b,c); }

// ---- Joystick ------------------------------------------------------------
MMRESULT WINAPI joyGetDevCapsA(UINT_PTR a,LPJOYCAPSA b,UINT c)                    { FORWARD(joyGetDevCapsA)(a,b,c); }
MMRESULT WINAPI joyGetDevCapsW(UINT_PTR a,LPJOYCAPSW b,UINT c)                    { FORWARD(joyGetDevCapsW)(a,b,c); }
UINT     WINAPI joyGetNumDevs()                                                      { FORWARD(joyGetNumDevs)(); }
MMRESULT WINAPI joyGetPos(UINT a,LPJOYINFO b)                                      { FORWARD(joyGetPos)(a,b); }
MMRESULT WINAPI joyGetPosEx(UINT a,LPJOYINFOEX b)                                  { FORWARD(joyGetPosEx)(a,b); }
MMRESULT WINAPI joyGetThreshold(UINT a,LPUINT b)                                   { FORWARD(joyGetThreshold)(a,b); }
MMRESULT WINAPI joyReleaseCapture(UINT a)                                           { FORWARD(joyReleaseCapture)(a); }
MMRESULT WINAPI joySetCapture(HWND a,UINT b,UINT c,BOOL d)                         { FORWARD(joySetCapture)(a,b,c,d); }
MMRESULT WINAPI joySetThreshold(UINT a,UINT b)                                      { FORWARD(joySetThreshold)(a,b); }
MMRESULT WINAPI joyConfigChanged(DWORD a)                                           { FORWARD(joyConfigChanged)(a); }

// ---- mmio ----------------------------------------------------------------
HMMIO    WINAPI mmioOpenA(LPSTR a,LPMMIOINFO b,DWORD c)                             { FORWARD(mmioOpenA)(a,b,c); }
HMMIO    WINAPI mmioOpenW(LPWSTR a,LPMMIOINFO b,DWORD c)                            { FORWARD(mmioOpenW)(a,b,c); }
MMRESULT WINAPI mmioClose(HMMIO a,UINT b)                                          { FORWARD(mmioClose)(a,b); }
LONG     WINAPI mmioRead(HMMIO a,HPSTR b,LONG c)                                   { FORWARD(mmioRead)(a,b,c); }
LONG     WINAPI mmioWrite(HMMIO a,const char* b,LONG c)                            { FORWARD(mmioWrite)(a,b,c); }
LONG     WINAPI mmioSeek(HMMIO a,LONG b,int c)                                     { FORWARD(mmioSeek)(a,b,c); }
MMRESULT WINAPI mmioGetInfo(HMMIO a,LPMMIOINFO b,UINT c)                           { FORWARD(mmioGetInfo)(a,b,c); }
MMRESULT WINAPI mmioSetInfo(HMMIO a,LPCMMIOINFO b,UINT c)                          { FORWARD(mmioSetInfo)(a,b,c); }
MMRESULT WINAPI mmioSetBuffer(HMMIO a,LPSTR b,LONG c,UINT d)                       { FORWARD(mmioSetBuffer)(a,b,c,d); }
MMRESULT WINAPI mmioFlush(HMMIO a,UINT b)                                          { FORWARD(mmioFlush)(a,b); }
MMRESULT WINAPI mmioAdvance(HMMIO a,LPMMIOINFO b,UINT c)                           { FORWARD(mmioAdvance)(a,b,c); }
MMRESULT WINAPI mmioDescend(HMMIO a,LPMMCKINFO b,const MMCKINFO* c,UINT d)        { FORWARD(mmioDescend)(a,b,c,d); }
MMRESULT WINAPI mmioAscend(HMMIO a,LPMMCKINFO b,UINT c)                            { FORWARD(mmioAscend)(a,b,c); }
MMRESULT WINAPI mmioCreateChunk(HMMIO a,LPMMCKINFO b,UINT c)                       { FORWARD(mmioCreateChunk)(a,b,c); }
MMRESULT WINAPI mmioRenameA(LPCSTR a,LPCSTR b,const MMIOINFO* c,DWORD d)          { FORWARD(mmioRenameA)(a,b,c,d); }
MMRESULT WINAPI mmioRenameW(LPCWSTR a,LPCWSTR b,const MMIOINFO* c,DWORD d)        { FORWARD(mmioRenameW)(a,b,c,d); }
LRESULT  WINAPI mmioSendMessage(HMMIO a,UINT b,LPARAM c,LPARAM d)                  { FORWARD(mmioSendMessage)(a,b,c,d); }
LPMMIOPROC WINAPI mmioInstallIOProcA(FOURCC a,LPMMIOPROC b,DWORD c)               { FORWARD(mmioInstallIOProcA)(a,b,c); }
LPMMIOPROC WINAPI mmioInstallIOProcW(FOURCC a,LPMMIOPROC b,DWORD c)               { FORWARD(mmioInstallIOProcW)(a,b,c); }
FOURCC   WINAPI mmioStringToFOURCCA(LPCSTR a,UINT b)                               { FORWARD(mmioStringToFOURCCA)(a,b); }
FOURCC   WINAPI mmioStringToFOURCCW(LPCWSTR a,UINT b)                              { FORWARD(mmioStringToFOURCCW)(a,b); }

// ---- Driver helpers ------------------------------------------------------
HDRVR    WINAPI OpenDriver(LPCWSTR a,LPCWSTR b,LPARAM c)                           { FORWARD(OpenDriver)(a,b,c); }
LRESULT  WINAPI CloseDriver(HDRVR a,LONG b,LONG c)                                 { FORWARD(CloseDriver)(a,b,c); }
LRESULT  WINAPI SendDriverMessage(HDRVR a,UINT b,LPARAM c,LPARAM d)               { FORWARD(SendDriverMessage)(a,b,c,d); }
HMODULE  WINAPI DrvGetModuleHandle(HDRVR a)                                         { FORWARD(DrvGetModuleHandle)(a); }
HMODULE  WINAPI GetDriverModuleHandle(HDRVR a)                                      { FORWARD(GetDriverModuleHandle)(a); }
LRESULT  WINAPI DefDriverProc(DWORD_PTR a,HDRVR b,UINT c,LPARAM d,LPARAM e)       { FORWARD(DefDriverProc)(a,b,c,d,e); }
BOOL     WINAPI DriverCallback(DWORD_PTR a,DWORD b,HDRVR c,DWORD d,DWORD_PTR e,DWORD_PTR f,DWORD_PTR g) { FORWARD(DriverCallback)(a,b,c,d,e,f,g); }
BOOL     WINAPI NotifyCallbackData(HDRVR a,UINT b,DWORD_PTR c,DWORD_PTR d,DWORD_PTR e) { FORWARD(NotifyCallbackData)(a,b,c,d,e); }

// ---- mmsystem version ----------------------------------------------------
UINT     WINAPI mmsystemGetVersion()                                                 { FORWARD(mmsystemGetVersion)(); }

} // extern "C"
