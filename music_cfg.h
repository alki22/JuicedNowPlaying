#pragma once

// Sub-phase 9.3 — music.cfg parser.
//
// Reads C:\Games\Juiced\audio\music\music.cfg (next to the running
// game executable) and exposes the [BeginSS]/[EndSS] track table:
//   name        -> internal id like "track01"
//   displayname -> "Artist - Title" shown to the player
//
// No dynamic allocation; all storage is a fixed-size static table.

struct TrackInfo {
    char name[16];         // e.g. "track01"
    char displayName[128]; // e.g. "Beans - Down By Law"
};

// Loads music.cfg relative to the game's executable directory.
// Returns true on success and at least one parsed track.
bool MusicCfg_Load();

int               MusicCfg_Count();
const TrackInfo*  MusicCfg_GetByIndex(int i);
const TrackInfo*  MusicCfg_GetByName(const char* name);
