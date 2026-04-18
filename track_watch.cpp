#include "pch.h"
#include "track_watch.h"
#include "ini_reader.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

namespace {

IniConfig g_cfg;
unsigned char* g_moduleBase = nullptr;
bool g_inited = false;

// Set to 1 while TrackWatch_PlayTrack is executing its audio-switch sequence.
// During that window we deliberately zero [AudioMgr+0x10], so IsInRace() must
// not treat that zero as "paused" and must not hide the overlay mid-skip.
static volatile LONG g_trackSwitchActive = 0;


void Log(const char* fmt, ...)
{
    char modulePath[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&Log),
        &self);
    if (!self) return;
    if (GetModuleFileNameA(self, modulePath, MAX_PATH) == 0) return;
    char* dot = strrchr(modulePath, '.');
    if (!dot) return;
    size_t prefixLen = static_cast<size_t>(dot - modulePath);
    if (prefixLen + 5 >= MAX_PATH) return;
    strcpy_s(dot, MAX_PATH - prefixLen, ".log");

    FILE* fp = nullptr;
    if (fopen_s(&fp, modulePath, "ab") != 0 || !fp) return;

    char stamp[32];
    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_s(&tmNow, &now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmNow);
    fprintf(fp, "[%s] ", stamp);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    fclose(fp);
}

// SEH-wrapped chain walker. We have to isolate the __try block in its
// own function because objects with destructors aren't allowed inside
// a function that uses structured exception handling.
int WalkChain(unsigned char* base,
              unsigned int baseOffset,
              const unsigned int* offsets,
              int offsetCount)
{
    if (!base || offsetCount <= 0) return -1;

    __try {
        unsigned char* p = *reinterpret_cast<unsigned char**>(base + baseOffset);
        // Apply all but the last offset as dereferences.
        for (int i = 0; i < offsetCount - 1; ++i) {
            if (!p) return -1;
            p = *reinterpret_cast<unsigned char**>(p + offsets[i]);
        }
        if (!p) return -1;
        return *reinterpret_cast<int*>(p + offsets[offsetCount - 1]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

} // namespace

void TrackWatch_Init()
{
    if (g_inited) return;
    g_inited = true;

    Ini_Load(&g_cfg);

    g_moduleBase = reinterpret_cast<unsigned char*>(GetModuleHandleA(nullptr));
    if (!g_moduleBase) {
        Log("track_watch: GetModuleHandle(NULL) returned null\n");
        return;
    }

    // Sanity log — format the whole chain description on one line.
    char chain[128] = {};
    int used = 0;
    used += _snprintf_s(chain + used, sizeof(chain) - used, _TRUNCATE,
                        "Juiced.exe+0x%X", g_cfg.trackIdBase);
    for (int i = 0; i < g_cfg.trackIdOffsetCount; ++i) {
        used += _snprintf_s(chain + used, sizeof(chain) - used, _TRUNCATE,
                            " -> +0x%X", g_cfg.trackIdOffsets[i]);
    }
    Log("track_watch: base=%p chain=%s inRaceFlag=+0x%X\n",
        g_moduleBase, chain, g_cfg.inRaceFlag);
}

int TrackWatch_ReadCurrent()
{
    if (!g_inited) return -1;
    return WalkChain(g_moduleBase, g_cfg.trackIdBase,
                     g_cfg.trackIdOffsets, g_cfg.trackIdOffsetCount);
}

unsigned char* TrackWatch_ModuleBase()
{
    return g_moduleBase;
}

int TrackWatch_ReadRawInRaceFlag()
{
    if (!g_inited || !g_moduleBase) return 0;
    __try {
        return *reinterpret_cast<int*>(g_moduleBase + g_cfg.inRaceFlag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int TrackWatch_ReadRawInPauseFlag()
{
    // Returns -1 when the flag address is not configured (InPauseFlag = 0 in ini).
    // When configured, this address holds an ACTIVE-RACE indicator:
    //   >0  = game is actively running (not paused)
    //    0  = pause menu is open
    // Callers must treat -1 as "no pause detection" and 0 as "hide overlay".
    if (!g_inited || !g_moduleBase || g_cfg.inPauseFlag == 0) return -1;
    __try {
        return *reinterpret_cast<int*>(g_moduleBase + g_cfg.inPauseFlag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

unsigned int TrackWatch_GetPrevKey()
{
    return g_inited ? g_cfg.prevTrackKey : 0xBA;
}

unsigned int TrackWatch_GetNextKey()
{
    return g_inited ? g_cfg.nextTrackKey : 0xDE;
}

bool TrackWatch_IsInRace()
{
    if (!g_inited || !g_moduleBase) return false;
    __try {
        // Primary gate: InRaceFlag must be non-zero.
        int val = *reinterpret_cast<int*>(g_moduleBase + g_cfg.inRaceFlag);
        if (val == 0) {
            return false;
        }

        // Resolve the root object pointer (needed for the checks below).
        unsigned char* root =
            *reinterpret_cast<unsigned char**>(g_moduleBase + g_cfg.trackIdBase);

        // Loading-screen check:
        // [root+0x40] holds a non-null "loading state" pointer while the race
        // is still loading and becomes NULL once the race goes live.  Confirmed
        // by memory dump: loading=0x11f2adf0, active-race=0, paused=0.
        // InRaceFlag is set too early (during the load), so we must suppress the
        // overlay until this pointer clears.
        if (root) {
            void* loadingObj = *reinterpret_cast<void**>(root + 0x40);
            if (loadingObj != nullptr) return false; // still on loading screen
        }

        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Walk the chain to the final pointer (same as WalkChain but returns
// the address of the target int instead of its value, so we can write).
static int* ResolveChainPtr(unsigned char* base,
                            unsigned int baseOffset,
                            const unsigned int* offsets,
                            int offsetCount)
{
    if (!base || offsetCount <= 0) return nullptr;
    __try {
        unsigned char* p =
            *reinterpret_cast<unsigned char**>(base + baseOffset);
        for (int i = 0; i < offsetCount - 1; ++i) {
            if (!p) return nullptr;
            p = *reinterpret_cast<unsigned char**>(p + offsets[i]);
        }
        if (!p) return nullptr;
        return reinterpret_cast<int*>(p + offsets[offsetCount - 1]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool TrackWatch_WriteCurrent(int newIdx)
{
    if (!g_inited || !g_moduleBase) return false;
    int* addr = ResolveChainPtr(g_moduleBase, g_cfg.trackIdBase,
                                g_cfg.trackIdOffsets, g_cfg.trackIdOffsetCount);
    if (!addr) return false;
    __try {
        *addr = newIdx;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Walk the chain to the audio manager pointer — one level above the
// track-index field. With offsets [0x48, 0x20] this returns
// *(*(base + trackIdBase) + 0x48), i.e. the audio manager object.
static unsigned char* ResolveAudioMgr()
{
    if (!g_moduleBase || g_cfg.trackIdOffsetCount < 2) return nullptr;
    __try {
        unsigned char* p =
            *reinterpret_cast<unsigned char**>(g_moduleBase + g_cfg.trackIdBase);
        // Walk all dereferences except the last two (stop one before the field).
        for (int i = 0; i < g_cfg.trackIdOffsetCount - 2; ++i) {
            if (!p) return nullptr;
            p = *reinterpret_cast<unsigned char**>(p + g_cfg.trackIdOffsets[i]);
        }
        if (!p) return nullptr;
        // The second-to-last offset dereferences to the audio manager object.
        return *reinterpret_cast<unsigned char**>(
                    p + g_cfg.trackIdOffsets[g_cfg.trackIdOffsetCount - 2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Call the game's own track-start routine at Juiced.exe+0x8EC0.
//
// Reverse-engineered from binary analysis:
//   00408EC0  F6 05 10 D7 71 00 08   test byte ptr [0x71D710], 8   ; audio-enabled flag
//   00408EDD  83 7E 20 FF            cmp  [esi+0x20], -1           ; track-index field
//   00408F00  8B 4E 20               mov  ecx, [esi+0x20]          ; read index
//   ...       FF 52 04               call [esi.vtable+4]           ; start stream
//
// Calling convention (non-standard, MSVC-optimised):
//   esi  = audio manager object ("this")
//   [esp+4]  = arg1 = 1
//   [esp+8]  = arg2 = 1   (checked at 00408EE3; non-zero = stop-then-restart)
//   ret 8    (callee cleans both DWORDs from the stack)
//
// Observed callers (e.g. 00436DC9):
//   mov eax, [Juiced.exe+0x35DC74]
//   mov esi, [eax+0x48]            ; same chain we already use
//   push 1 / push 1
//   call 0x408EC0
bool TrackWatch_PlayTrack(int newIdx)
{
    if (!g_inited || !g_moduleBase) return false;

    unsigned char* audioMgr = ResolveAudioMgr();
    if (!audioMgr) {
        Log("track_watch: PlayTrack: audio manager not available\n");
        return false;
    }

    // Guard: [audioMgr+8] must be non-zero (audio stream allocated).
    __try {
        if (*reinterpret_cast<int*>(audioMgr + 8) == 0) {
            Log("track_watch: PlayTrack: audio manager not ready\n");
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    // Diagnostic snapshot before the call.
    __try {
        int stream = *reinterpret_cast<int*>(audioMgr + 0x10);
        int cur    = *reinterpret_cast<int*>(audioMgr + 0x20);
        Log("track_watch: PlayTrack %d: stream=%08X cur=%d\n",
            newIdx, stream, cur);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // -----------------------------------------------------------------------
    // How 0x408EC0 works (fully decoded):
    //
    //   408EC0: test [0x71D710], 8
    //   408EC7: jnz  408F99      ← bit-3 SET → EARLY RETURN (entire fn skipped)
    //                               bit-3 CLEAR (0x24 & 8 = 0) → fall through ✓
    //   408EE3: cmp  [esp+8], 0  ← arg2
    //   408EE8: jz   408EF1      ← arg2 = 0 → SKIP playlist-advance call ✓
    //                               (the hooked 0x408E90 overwrites [esi+20] with
    //                                the shuffle's next track — we must skip it)
    //   408EF1: call 409230      ← stream-state check
    //   408EFA: jnz  408F99      ← [ecx+10] != 0 (handle ≠ 0) → RETURN
    //                               we zero [esi+10] so 409230 sees "no stream" ✓
    //   408F00: mov ecx,[esi+20] ← reads our newIdx ✓
    //   408F21: call [edx+4]     ← vtable[1]: actually starts the stream ✓
    //   408F24: mov [esi+10],eax ← saves new stream handle
    // -----------------------------------------------------------------------

    // Signal that we're about to zero [am+0x10] intentionally.
    // TrackWatch_IsInRace() skips its stream-handle check while this is set,
    // preventing the overlay from hiding mid-skip.
    InterlockedExchange(&g_trackSwitchActive, 1);

    // Zero the stream handle: makes 0x409230 return 0 ("no active stream") so
    // 0x408EC0 proceeds past the active-stream guard and calls vtable[1].
    __try {
        *reinterpret_cast<int*>(audioMgr + 0x10) = 0;
        *reinterpret_cast<int*>(audioMgr + 0x20) = newIdx;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_trackSwitchActive, 0);
        return false;
    }

    // Call 0x408EC0 with arg2=0 so it skips the playlist-advance hook at
    // 0x408E90 and goes straight to vtable[1] with our newIdx.
    // bit-3 of [0x71D710] must remain CLEAR (it is: 0x24 & 8 = 0).
    // 0x408EC0 writes the new stream handle back to [esi+10] before returning,
    // so [am+0x10] is non-zero again by the time we clear g_trackSwitchActive.
    DWORD fnAddr = reinterpret_cast<DWORD>(g_moduleBase) + 0x8EC0u;
    unsigned char* mgr = audioMgr;
    __try {
        __asm {
            push esi
            mov  esi, mgr
            push 0          ; arg2=0: skip playlist-advance, use [esi+20] as-is
            push 1          ; arg1
            mov  eax, fnAddr
            call eax        ; fn does ret 8 — callee cleans args
            pop  esi
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_trackSwitchActive, 0);
        Log("track_watch: PlayTrack: SEH in asm call\n");
        return false;
    }

    InterlockedExchange(&g_trackSwitchActive, 0);

    // Diagnostic snapshot after the call.
    __try {
        int stream = *reinterpret_cast<int*>(audioMgr + 0x10);
        int cur    = *reinterpret_cast<int*>(audioMgr + 0x20);
        Log("track_watch: PlayTrack %d: after: stream=%08X cur=%d\n",
            newIdx, stream, cur);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    Log("track_watch: PlayTrack %d -> ok\n", newIdx);
    return true;
}

// Dump the root object's pointer-sized fields around the offsets we know about
// (+0x48 = AudioMgr, +0x64 = MusicMgr) so we can identify which field is the
// playlist / shuffle arg passed to 0x465980.
void TrackWatch_LogRoot()
{
    if (!g_inited || !g_moduleBase) return;
    __try {
        unsigned char* root =
            *reinterpret_cast<unsigned char**>(g_moduleBase + g_cfg.trackIdBase);
        if (!root) { Log("track_watch: LogRoot: root is null\n"); return; }

        Log("track_watch: LogRoot root=%p:\n", root);
        // Dump +0x30 through +0x70 in one compact line per dword.
        for (int off = 0x30; off <= 0x70; off += 4) {
            unsigned int val =
                *reinterpret_cast<unsigned int*>(root + off);
            Log("  [root+%02X]=%08X\n", off, val);
        }

        // Also log AudioMgr fields we care about.
        unsigned char* am =
            *reinterpret_cast<unsigned char**>(root + 0x48);
        if (am) {
            int stream = *reinterpret_cast<int*>(am + 0x10);
            int cur    = *reinterpret_cast<int*>(am + 0x20);
            Log("track_watch: AudioMgr=%p stream=%08X cur=%d\n", am, stream, cur);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("track_watch: LogRoot: SEH\n");
    }
}

int TrackWatch_TrackCount()
{
    // Proxy through music.cfg count, included transitively via dllmain.
    // Avoids a circular header dependency — callers that need MusicCfg_Count
    // can just call it directly; this is a convenience for track_watch users.
    return 25; // matches music.cfg bank_size
}

// ---------------------------------------------------------------------------
// TrackWatch_DumpState — memory snapshot helper
//
// Dumps three regions to the log file so that loading-screen, active-race,
// and pause-menu snapshots can be diffed to find better gate addresses:
//
//   Region A — 64 DWORDs centred on InRaceFlag
//               (offset range [inRaceFlag-0x78 .. inRaceFlag+0x84])
//               Marked with '*' on the known InRaceFlag line.
//
//   Region B — root object +0x30..+0x80 (same as TrackWatch_LogRoot)
//
//   Region C — AudioMgr +0x00..+0x30 (stream handle, track index, etc.)
//
// How to use:
//   1. Build and deploy.  Start the game and navigate into a race.
//   2. While loading screen is up: note the auto-triggered
//      "InRaceFlag_0->1" dump in the log.
//   3. Press ';' the moment the race starts (first controllable frame).
//   4. Pause the game and press ';' again.
//   5. Open the log.  Compare Region A across all three snapshots:
//      - A DWORD that is 0 in snapshot 2 (loading dump) and 1 in
//        snapshot 3 (race-start) is a better InRaceFlag candidate.
//      - A DWORD that is 0 in snapshot 3 and non-zero in snapshot 4
//        (pause) is the InPauseFlag candidate.
//   6. Record the winning offsets in JuicedNowPlaying.ini as
//      InRaceFlag = <hex>  and/or  InPauseFlag = <hex>.
// ---------------------------------------------------------------------------
void TrackWatch_DumpState(const char* label)
{
    if (!g_inited || !g_moduleBase) return;

    Log("track_watch: === DUMP [%s] ===\n", label);

    // Pause-flag value at the moment of capture. Lets us see, in the log,
    // whether 0x0035DCB4 actually toggles or stays latched at 0 across a race.
    {
        int pauseRaw = TrackWatch_ReadRawInPauseFlag();
        Log("track_watch: InPauseFlag (base+0x%X) = %d\n",
            g_cfg.inPauseFlag, pauseRaw);
    }

    // ---- Region A: 64 DWORDs around InRaceFlag ----
    // Start 0x78 bytes below the flag so the flag lands near the middle.
    unsigned int flagOff = g_cfg.inRaceFlag;
    unsigned int startOff = (flagOff >= 0x78u) ? (flagOff - 0x78u) : 0u;
    // Align down to 16 bytes for tidy output.
    startOff &= ~0xFu;
    Log("track_watch: RegionA base+0x%X..+0x%X (* = InRaceFlag):\n",
        startOff, startOff + 64 * 4 - 4);
    __try {
        for (int i = 0; i < 64; ++i) {
            unsigned int off = startOff + static_cast<unsigned int>(i) * 4;
            unsigned int val = *reinterpret_cast<unsigned int*>(g_moduleBase + off);
            char marker = (off == flagOff) ? '*' : ' ';
            Log("  %c[base+0x%08X]=%08X (%u)\n", marker, off, val, val);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("  SEH reading RegionA\n");
    }

    // ---- Region B: root object +0x30..+0x80 ----
    __try {
        unsigned char* root =
            *reinterpret_cast<unsigned char**>(g_moduleBase + g_cfg.trackIdBase);
        if (!root) {
            Log("track_watch: RegionB: root is null\n");
        } else {
            Log("track_watch: RegionB root=%p +0x30..+0x80:\n", root);
            for (int off = 0x30; off <= 0x80; off += 4) {
                unsigned int val =
                    *reinterpret_cast<unsigned int*>(root + off);
                Log("  [root+0x%02X]=%08X (%u)\n", off, val, val);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("  SEH reading RegionB\n");
    }

    // ---- Region C: AudioMgr +0x00..+0x30 ----
    __try {
        unsigned char* root =
            *reinterpret_cast<unsigned char**>(g_moduleBase + g_cfg.trackIdBase);
        if (root) {
            unsigned char* am =
                *reinterpret_cast<unsigned char**>(root + 0x48);
            if (!am) {
                Log("track_watch: RegionC: AudioMgr ptr null\n");
            } else {
                Log("track_watch: RegionC AudioMgr=%p +0x00..+0x30:\n", am);
                for (int off = 0; off <= 0x30; off += 4) {
                    unsigned int val =
                        *reinterpret_cast<unsigned int*>(am + off);
                    Log("  [am+0x%02X]=%08X (%u)\n", off, val, val);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("  SEH reading RegionC\n");
    }

    // ---- Region D: 64 DWORDs around 0x71D710 (audio-flags BYTE) ----
    // 0x408EC0 checks bit-3 of [0x71D710]; if SET the entire fn is skipped.
    // During racing this byte is 0x24 (bit-3 CLEAR = audio enabled).
    // If the game sets bit-3 on pause, this region will reveal it.
    // Module-base-relative offset = 0x71D710 - 0x400000 = 0x31D710.
    {
        unsigned int afOff = 0x31D710u;
        unsigned int afStart = (afOff >= 0x78u) ? (afOff - 0x78u) : 0u;
        afStart &= ~0xFu;
        Log("track_watch: RegionD (audioFlags) base+0x%X..+0x%X (* = 0x71D710 byte):\n",
            afStart, afStart + 64 * 4 - 4);
        __try {
            for (int i = 0; i < 64; ++i) {
                unsigned int off = afStart + static_cast<unsigned int>(i) * 4;
                unsigned int val = *reinterpret_cast<unsigned int*>(g_moduleBase + off);
                char marker = (off == afOff || off == (afOff & ~3u)) ? '*' : ' ';
                Log("  %c[base+0x%08X]=%08X (%u)\n", marker, off, val, val);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("  SEH reading RegionD\n");
        }
    }

    // ---- Region E: root object +0x30..+0x100 (wider than RegionB) ----
    __try {
        unsigned char* root =
            *reinterpret_cast<unsigned char**>(g_moduleBase + g_cfg.trackIdBase);
        if (!root) {
            Log("track_watch: RegionE: root is null\n");
        } else {
            Log("track_watch: RegionE root=%p +0x30..+0x100:\n", root);
            for (int off = 0x30; off <= 0x100; off += 4) {
                unsigned int val =
                    *reinterpret_cast<unsigned int*>(root + off);
                Log("  [root+0x%03X]=%08X (%u)\n", off, val, val);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("  SEH reading RegionE\n");
    }

    Log("track_watch: === END DUMP [%s] ===\n", label);
}
