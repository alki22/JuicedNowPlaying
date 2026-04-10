#pragma once

// Sub-phase 9.5 — minimal .ini reader for JuicedNowPlaying.ini.
//
// We only need three keys right now, so this is not a general-purpose
// parser: it just wraps GetPrivateProfileStringA and parses small
// numeric/hex/CSV values.

struct IniConfig {
    // [Memory]
    unsigned int trackIdBase;       // offset within Juiced.exe
    unsigned int trackIdOffsets[8]; // outermost first
    int          trackIdOffsetCount;
    unsigned int inRaceFlag;        // direct offset within Juiced.exe

    // [Display]
    unsigned char ledR;
    unsigned char ledG;
    unsigned char ledB;
};

// Loads the ini file sitting next to the .asi. Missing keys get
// sensible defaults. Returns true if the file was found and read.
bool Ini_Load(IniConfig* out);
