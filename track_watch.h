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

// Direct static read: returns true if the in-race flag is nonzero.
bool TrackWatch_IsInRace();
