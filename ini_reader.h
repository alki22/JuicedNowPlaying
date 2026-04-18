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
    unsigned int inPauseFlag;       // if non-zero: offset of a flag that is
                                    // non-zero while the game is paused; the
                                    // overlay is hidden when this flag is set.

    // [Display]
    unsigned char ledR;
    unsigned char ledG;
    unsigned char ledB;

    // [Controls]
    unsigned int prevTrackKey;  // VK code for skip-previous (default: VK_OEM_1 = 0xBA = ;)
    unsigned int nextTrackKey;  // VK code for skip-next     (default: VK_OEM_7 = 0xDE = ')
};

// Loads the ini file sitting next to the .asi. Missing keys get
// sensible defaults. Returns true if the file was found and read.
bool Ini_Load(IniConfig* out);
