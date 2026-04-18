#pragma once

// Sub-phase 9.5 — poll the pointer chain configured in
// JuicedNowPlaying.ini for the currently-playing track index.

// Resolve the chain once at init. Safe to call before the game has
// allocated the audio object — subsequent reads just return -1 until
// the pointer becomes valid.
void TrackWatch_Init();

// Wrap the pointer walk in SEH so a bad deref (typical during
// loading screens) can't take the game down.
//
// Returns -1 if the chain can't be resolved, or the raw 4-byte value
// at the end of the chain otherwise. 0-indexed in Juiced.
int  TrackWatch_ReadCurrent();

// Direct static read: returns true if the in-race flag is nonzero AND
// all secondary checks (pause heuristic, etc.) pass.
bool TrackWatch_IsInRace();

// Returns the raw integer value at inRaceFlag offset (no secondary checks).
// Used by the auto-dump logic to track value transitions.
int  TrackWatch_ReadRawInRaceFlag();

// Returns the raw value at inPauseFlag offset (0 if flag not configured).
// Used by the overlay to detect pause/resume transitions directly,
// bypassing IsInRace() so a permanently-stuck flag can't break cross-race state.
int  TrackWatch_ReadRawInPauseFlag();

// Returns the game module base address (used by d3d9_hook for snapshot scans).
unsigned char* TrackWatch_ModuleBase();

// Write a new track index through the same pointer chain.
// Returns true if the write succeeded.
bool TrackWatch_WriteCurrent(int newIdx);

// Write a new track index AND call the game's internal track-start
// routine (Juiced.exe+0x8EC0) so the audio engine actually switches
// streams.  Returns true on success.  Safe to call from the render
// thread — wrapped in SEH; worst case is a no-op.
bool TrackWatch_PlayTrack(int newIdx);

// Total number of tracks (from music.cfg). Convenience for wrapping.
int  TrackWatch_TrackCount();

// Diagnostic: dump root object fields +0x30..+0x70 and AudioMgr state to the
// log file. Call once after a skip attempt to help identify the shuffle buffer
// pointer chain.
void TrackWatch_LogRoot();

// Diagnostic: dump 64 DWORDs centred on InRaceFlag, root fields, and AudioMgr
// state to the log, prefixed with |label|.  Use this to identify candidate
// InPauseFlag / better-InRaceFlag addresses by comparing snapshots taken at
// loading-screen, race-start, and pause transitions.
void TrackWatch_DumpState(const char* label);

// VK codes for the skip-previous and skip-next keys (from [Controls] in the ini).
// Defaults: VK_OEM_1 (0xBA = ;) and VK_OEM_7 (0xDE = ').
unsigned int TrackWatch_GetPrevKey();
unsigned int TrackWatch_GetNextKey();
