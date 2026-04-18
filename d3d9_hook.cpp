#include "pch.h"
#include "d3d9_hook.h"
#include "bitmap_font.h"
#include "vfd_font.h"
#include "music_cfg.h"
#include "track_watch.h"

#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "d3d9.lib")

namespace {

// IDirect3DDevice9 vtable slot indices (from d3d9.h declaration order).
// Slot 0..2 are IUnknown (QI/AddRef/Release).
constexpr int kSlotReset    = 16;
constexpr int kSlotEndScene = 42;

using EndScene_t = HRESULT (WINAPI*)(IDirect3DDevice9*);
using Reset_t    = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

EndScene_t g_origEndScene = nullptr;
Reset_t    g_origReset    = nullptr;

bool g_hookInstalled = false;

// Polling state.
int g_lastTrackIdx  = -2; // -2 = never read yet, -1 = chain invalid
int g_pollFrame     = 0;
constexpr int kPollInterval = 10;

// Key state for edge detection — we only fire on the frame the key
// transitions from up to down, not every frame it's held.
// ; = VK_OEM_1  (0xBA)   ' = VK_OEM_7  (0xDE)
// [ = VK_OEM_4  (0xDB)  — diagnostic snapshot / diff key
bool g_keyPrevDown = false;
bool g_keyNextDown = false;
bool g_keyDumpDown = false;
bool g_rootDumped  = false; // log root fields once on first skip

// ---- Three-point snapshot diff ----
// Finds the pause flag via a racing → paused → resumed cycle.
//
// Usage:
//   Press [ while RACING        (snap A)
//   Press [ while PAUSED        (snap B)  → logs A→B diff
//   Press [ while RACING again  (snap C)  → logs only TRUE TOGGLE addresses:
//                                           those where A==C (reverted) AND A!=B (changed on pause)
//
// Range: 0x31D700..0x370000 = 0x52900 bytes = 84544 DWORDs (~330 KB each).
constexpr unsigned int kSnapStartOff = 0x31D700u;
constexpr unsigned int kSnapEndOff   = 0x370000u;
constexpr unsigned int kSnapDwords   = (kSnapEndOff - kSnapStartOff) / 4;
static unsigned int g_snapBufA[kSnapDwords]; // snap A: racing state
static unsigned int g_snapBufB[kSnapDwords]; // snap B: paused state
// 0 = fresh, 1 = A taken, 2 = B taken (waiting for C)
static int g_snapState = 0;

// ---- Race-gate state ----
// g_wasGated  — armed on InRaceFlag 0→1; cleared by the wasGated reset block.
// g_raceLoaded — false from race start until [root+0x40] clears (loading done);
//                stays true for the remainder of the race so mid-race IsInRace()
//                blips (e.g. pause menu touching the field) don't reset the display.
bool g_wasGated   = false;
bool g_raceLoaded = false;

// ---- State-change auto-dump ----
// We snapshot the raw InRaceFlag on every EndScene call. When it changes
// we emit a DumpState so the log captures exactly which other values
// changed at the same moment (= loading screen start, race end, etc.).
int  g_lastInRaceRaw  = -999; // sentinel: never yet read
int  g_prevPauseRaw   = -1;   // -1 = uninitialised; tracks InPauseFlag edge transitions
int  g_dumpFrameCount = 0;    // periodic dump counter
constexpr int kDumpPeriodFrames = 300; // ~5 s at 60 fps

// Pause-flag debouncing. 0x0035DCB4 dips to 0 for 1–3 frames during some
// engine events (track transitions, etc.) even while racing. Require N
// consecutive frames of a new value before flipping the gate.
int  g_pauseConsecutive  = 0;
int  g_resumeConsecutive = 0;
bool g_paused            = false;
constexpr int kPauseDebounceFrames = 4;   // ~67 ms at 60 fps
// Failsafe: if InPauseFlag has read 0 for more than this many frames while
// we're in-race, the flag is empirically stuck (observed in races 2/3 of a
// session — see 2026-04-16 log around 20:27:30 and 20:30:29). Distrust it
// and let the overlay draw, accepting the cosmetic cost of showing during
// the rare genuine multi-second pause-menu open.
constexpr int kStuckPauseFrames    = 300; // ~5 s at 60 fps

// ---- Background thread for song skip ----
// vtable[1] inside 0x408EC0 opens an audio stream file synchronously;
// doing that on the render thread causes a visible stutter. We fire-and-
// forget onto a worker thread instead. g_skipBusy prevents two overlapping
// skips (which would race on [AudioMgr+0x10]).
static volatile LONG g_skipBusy = 0;

static DWORD WINAPI SkipWorker(LPVOID pv)
{
    TrackWatch_PlayTrack(static_cast<int>(reinterpret_cast<intptr_t>(pv)));
    InterlockedExchange(&g_skipBusy, 0);
    return 0;
}

static void FireSkip(int newIdx)
{
    if (InterlockedCompareExchange(&g_skipBusy, 1, 0) != 0) return; // already busy
    HANDLE th = CreateThread(nullptr, 0, SkipWorker,
                             reinterpret_cast<LPVOID>(static_cast<intptr_t>(newIdx)),
                             0, nullptr);
    if (th) CloseHandle(th);
    else    InterlockedExchange(&g_skipBusy, 0);
}

// 9.7 — display state machine.
enum NpState { NP_HIDDEN, NP_FADE_IN, NP_SCROLL, NP_FADE_OUT };
NpState  g_npState = NP_HIDDEN;
float    g_npStateStart = 0.0f;
// When skipDraw is true (paused), we freeze the animation clock so the overlay
// resumes from exactly where it left off when pause clears.  -1 means not frozen.
float    g_npFrozenElapsed = -1.0f;
char     g_npText[128] = {};
int      g_npTextW = 0;    // actual pixel width of the rasterized text
int      g_npTextH = 0;    // actual pixel height
int      g_npTextTexW = 0; // POT texture width (for UV calculation)
int      g_npTextTexH = 0;
IDirect3DTexture9* g_npTextTex  = nullptr; // rasterized song title
IDirect3DTexture9* g_npBaseTex  = nullptr; // VFD base "888...8" dim
int      g_npBaseW = 0;
int      g_npBaseH = 0;
int      g_npBaseTexW = 0;
int      g_npBaseTexH = 0;
int      g_npDisplayedTrack = -1;
int      g_npLastPanelChars = 0;  // cached panel char count for base tex

constexpr float kFadeInSec      = 0.4f;
constexpr float kFadeOutSec     = 0.5f;
constexpr float kScrollPxPerSec = 120.0f;
// Panel width ~1/phi of the center gap between minimap and speedometer.
// The gap is roughly 60% of viewport; 60%/phi ≈ 37%, but we want the
// scroll to be noticeable so we go tighter: ~20% of viewport.
constexpr float kPanelWidthFrac = 0.20f;
constexpr int   kFontHeight     = 36;

LARGE_INTEGER g_perfFreq = {};
bool          g_perfInited = false;

float NowSec()
{
    if (!g_perfInited) {
        QueryPerformanceFrequency(&g_perfFreq);
        g_perfInited = true;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<float>(
        static_cast<double>(now.QuadPart) /
        static_cast<double>(g_perfFreq.QuadPart));
}

void Log(const char* fmt, ...);

// ---------- drawing helpers ----------

// Next power of 2 (for UV calculation against POT textures).
int NextPow2(int v) { int p = 1; while (p < v) p <<= 1; return p; }

struct TLVTX {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};
constexpr DWORD TLVTX_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

struct TLUNTEX {
    float x, y, z, rhw;
    DWORD color;
};
constexpr DWORD TLUNTEX_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

inline DWORD MakeColor(BYTE r, BYTE g, BYTE b, BYTE a)
{
    return (static_cast<DWORD>(a) << 24) | (r << 16) | (g << 8) | b;
}

inline DWORD ScaleAlpha(DWORD c, float mul)
{
    DWORD a = (c >> 24) & 0xFF;
    a = static_cast<DWORD>(a * mul + 0.5f);
    if (a > 255) a = 255;
    return (a << 24) | (c & 0x00FFFFFFu);
}

// Draw a textured quad (pre-transformed, alpha-blended).
void DrawTexQuad(IDirect3DDevice9* dev, IDirect3DTexture9* tex,
                 float x, float y, float w, float h,
                 float uMax, float vMax, DWORD color)
{
    TLVTX verts[6] = {
        { x,     y,     0, 1, color, 0,    0    },
        { x + w, y,     0, 1, color, uMax, 0    },
        { x,     y + h, 0, 1, color, 0,    vMax },
        { x + w, y,     0, 1, color, uMax, 0    },
        { x + w, y + h, 0, 1, color, uMax, vMax },
        { x,     y + h, 0, 1, color, 0,    vMax },
    };
    dev->SetTexture(0, tex);
    dev->SetFVF(TLVTX_FVF);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, verts, sizeof(TLVTX));
}

// Draw soft-edged panel background (untextured).
void DrawSoftPanel(IDirect3DDevice9* dev,
                   float fx0, float fy0, float fx1, float fy1,
                   float blur, DWORD cFull)
{
    DWORD cZero = cFull & 0x00FFFFFFu;
    float b = blur;

    // Center + 4 fade strips + 4 corners = 30 verts max.
    TLUNTEX verts[54]; // 9 * 6
    int n = 0;

    auto quad = [&](float x0, float y0, float x1, float y1,
                    DWORD tl, DWORD tr, DWORD bl, DWORD br) {
        verts[n++] = { x0, y0, 0, 1, tl };
        verts[n++] = { x1, y0, 0, 1, tr };
        verts[n++] = { x0, y1, 0, 1, bl };
        verts[n++] = { x1, y0, 0, 1, tr };
        verts[n++] = { x1, y1, 0, 1, br };
        verts[n++] = { x0, y1, 0, 1, bl };
    };
    auto tri = [&](float x0, float y0, DWORD c0,
                   float x1, float y1, DWORD c1,
                   float x2, float y2, DWORD c2) {
        verts[n++] = { x0, y0, 0, 1, c0 };
        verts[n++] = { x1, y1, 0, 1, c1 };
        verts[n++] = { x2, y2, 0, 1, c2 };
    };

    // Center.
    quad(fx0, fy0, fx1, fy1, cFull, cFull, cFull, cFull);
    // Fade strips.
    quad(fx0, fy0 - b, fx1, fy0, cZero, cZero, cFull, cFull); // top
    quad(fx0, fy1, fx1, fy1 + b, cFull, cFull, cZero, cZero); // bottom
    quad(fx0 - b, fy0, fx0, fy1, cZero, cFull, cZero, cFull); // left
    quad(fx1, fy0, fx1 + b, fy1, cFull, cZero, cFull, cZero); // right
    // Corners.
    tri(fx0, fy0, cFull, fx0 - b, fy0, cZero, fx0, fy0 - b, cZero);
    tri(fx1, fy0, cFull, fx1, fy0 - b, cZero, fx1 + b, fy0, cZero);
    tri(fx0, fy1, cFull, fx0, fy1 + b, cZero, fx0 - b, fy1, cZero);
    tri(fx1, fy1, cFull, fx1 + b, fy1, cZero, fx1, fy1 + b, cZero);

    dev->SetTexture(0, nullptr);
    dev->SetFVF(TLUNTEX_FVF);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, verts, sizeof(TLUNTEX));
}

// ---------- main draw ----------

void DrawNowPlaying(IDirect3DDevice9* dev)
{
    // ---- Raw InRaceFlag polling and auto-dump ----
    // Read the flag directly (not through IsInRace) so we can detect every
    // value transition and emit a labelled snapshot for post-session analysis.
    // This runs unconditionally — even on menus — so we capture loading-screen
    // entry (0->1) and race-exit (1->0) automatically.
    {
        int rawNow = TrackWatch_ReadRawInRaceFlag();
        if (rawNow != g_lastInRaceRaw) {
            if (g_lastInRaceRaw != -999) {
                // Transition: emit a snapshot labelled with the direction.
                char lbl[64];
                _snprintf_s(lbl, sizeof(lbl), _TRUNCATE,
                            "InRaceFlag_%d->%d", g_lastInRaceRaw, rawNow);
                TrackWatch_DumpState(lbl);
            }
            // On 0→1: a new race is starting. Arm wasGated so the animation
            // state is cleared once the loading screen finishes, and mark the
            // race as not yet loaded so the loading-screen suppression fires.
            if (rawNow != 0 && g_lastInRaceRaw == 0) {
                g_wasGated   = true;
                g_raceLoaded = false;
            }
            g_lastInRaceRaw = rawNow;
        }
    }

    // [ key (VK_OEM_4 = 0xDB) — three-point snapshot to find the pause flag.
    //
    // Cycle:
    //   Press [ while RACING         → snap A (baseline)
    //   Press [ while PAUSED         → snap B, log A→B diff
    //   Press [ while RACING (again) → snap C, log only TRUE TOGGLES:
    //                                   A==C (reverted) AND A!=B (changed on pause)
    //
    // TRUE TOGGLE lines are the pause flag candidates — copy the address into
    // JuicedNowPlaying.ini as InPauseFlag and restart the game to test.
    {
        bool dumpDown = (GetAsyncKeyState(VK_OEM_4) & 0x8000) != 0;
        if (dumpDown && !g_keyDumpDown) {
            unsigned char* base = TrackWatch_ModuleBase();

            if (g_snapState == 0) {
                // ---- Press 1: take snap A while RACING ----
                TrackWatch_DumpState("snap_A");
                Log("d3d9_hook: [snap A] racing baseline (base+0x%X..0x%X)\n",
                    kSnapStartOff, kSnapEndOff);
                __try {
                    for (unsigned int i = 0; i < kSnapDwords; ++i)
                        g_snapBufA[i] = *reinterpret_cast<unsigned int*>(
                            base + kSnapStartOff + i * 4);
                    g_snapState = 1;
                    Log("d3d9_hook: snap A taken — now PAUSE the game and press [ again\n");
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    Log("d3d9_hook: snap A SEH\n");
                }

            } else if (g_snapState == 1) {
                // ---- Press 2: take snap B while PAUSED, diff A→B ----
                TrackWatch_DumpState("snap_B");
                Log("d3d9_hook: [snap B] paused state — A→B diff:\n");
                int nChanged = 0;
                __try {
                    for (unsigned int i = 0; i < kSnapDwords; ++i) {
                        unsigned int cur = *reinterpret_cast<unsigned int*>(
                            base + kSnapStartOff + i * 4);
                        g_snapBufB[i] = cur;
                        if (cur != g_snapBufA[i]) {
                            unsigned int off = kSnapStartOff + i * 4;
                            bool isFlag = (g_snapBufA[i] <= 0xFFFFu || cur <= 0xFFFFu);
                            Log("  %s[base+0x%08X]  A=%08X  B=%08X\n",
                                isFlag ? "FLAG " : "     ",
                                off, g_snapBufA[i], cur);
                            ++nChanged;
                        }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    Log("d3d9_hook: snap B diff SEH\n");
                }
                Log("d3d9_hook: snap B done (%d A→B changes) — now RESUME and press [ again\n",
                    nChanged);
                g_snapState = 2;

            } else {
                // ---- Press 3: snap C while RACING again — find true toggles ----
                TrackWatch_DumpState("snap_C");
                Log("d3d9_hook: [snap C] resumed racing — TRUE PAUSE TOGGLES (A==C, A!=B):\n");
                int nToggle = 0;
                __try {
                    for (unsigned int i = 0; i < kSnapDwords; ++i) {
                        unsigned int a = g_snapBufA[i];
                        unsigned int b = g_snapBufB[i];
                        if (a == b) continue;          // didn't change on pause — skip
                        unsigned int c = *reinterpret_cast<unsigned int*>(
                            base + kSnapStartOff + i * 4);
                        if (c != a) continue;          // didn't revert on resume — skip
                        // This address: A (racing) != B (paused), and C (racing again) == A
                        // → it's a genuine pause toggle.
                        unsigned int off = kSnapStartOff + i * 4;
                        bool isFlag = (a <= 0xFFFFu && b <= 0xFFFFu);
                        Log("  %sTOGGLE [base+0x%08X]  racing=%08X  paused=%08X\n",
                            isFlag ? "FLAG " : "     ",
                            off, a, b);
                        ++nToggle;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    Log("d3d9_hook: snap C SEH\n");
                }
                Log("d3d9_hook: snap C done — %d true toggle candidates\n", nToggle);
                Log("d3d9_hook: copy a FLAG TOGGLE address into InPauseFlag in the .ini\n");
                g_snapState = 0; // reset for next cycle
            }
        }
        g_keyDumpDown = dumpDown;
    }

    // --- Song skip keys ---
    // Default: ; (VK_OEM_1=0xBA) = previous   ' (VK_OEM_7=0xDE) = next
    // Overridable via [Controls] PrevTrackKey / NextTrackKey in the ini.
    // Gated on the raw InRaceFlag (not IsInRace) so they work while paused.
    if (TrackWatch_ReadRawInRaceFlag() != 0) {
        constexpr int kTrackCount = 25;
        bool prevDown = (GetAsyncKeyState(static_cast<int>(TrackWatch_GetPrevKey())) & 0x8000) != 0;
        bool nextDown = (GetAsyncKeyState(static_cast<int>(TrackWatch_GetNextKey())) & 0x8000) != 0;

        if (prevDown && !g_keyPrevDown) {
            int cur = TrackWatch_ReadCurrent();
            if (cur >= 0) {
                int next = (cur - 1 + kTrackCount) % kTrackCount;
                Log("d3d9_hook: skip prev -> %d\n", next);
                FireSkip(next);
            }
        }
        if (nextDown && !g_keyNextDown) {
            int cur = TrackWatch_ReadCurrent();
            if (cur >= 0) {
                int next = (cur + 1) % kTrackCount;
                Log("d3d9_hook: skip next -> %d\n", next);
                FireSkip(next);
            }
        }
        g_keyPrevDown = prevDown;
        g_keyNextDown = nextDown;
    }

    // Gate on in-race (InRaceFlag + loading screen).
    // Pause is handled separately below so a stuck InPauseFlag can't prevent
    // the overlay from showing again after a race restart.
    //
    // Two-part gate:
    //   Primary  — InRaceFlag == 0: race over / in menus.  Hide and bail.
    //   Secondary — [root+0x40] != null: loading screen.  Hide and bail.
    //              Only checked until the loading screen clears for the
    //              first time per race (g_raceLoaded).  After that, any
    //              brief [root+0x40] blip (e.g. pause-menu touching the
    //              field) is ignored so it can't trigger a wasGated reset
    //              and restart the animation.
    //
    // g_wasGated is armed only by the InRaceFlag 0→1 transition (above),
    // not here, so a mid-race IsInRace() blip never resets the display.
    // g_wasGated and g_raceLoaded are namespace-scope (declared above DrawNowPlaying).
    if (TrackWatch_ReadRawInRaceFlag() == 0) {
        if (g_npState != NP_HIDDEN)
            g_npState = NP_HIDDEN;
        g_raceLoaded = false;
        return;
    }
    if (!g_raceLoaded) {
        if (!TrackWatch_IsInRace()) {
            if (g_npState != NP_HIDDEN)
                g_npState = NP_HIDDEN;
            return;
        }
        g_raceLoaded = true; // loading screen cleared — don't check [root+0x40] again
    }
    if (g_wasGated) {
        g_wasGated = false;
        g_npDisplayedTrack = -1;
        g_prevPauseRaw = -1;
        g_paused = false;
        g_pauseConsecutive = 0;
        g_resumeConsecutive = 0;
        g_npFrozenElapsed = -1.0f;
    }

    // ----------------------------------------------------------------------
    // STATE UPDATES — always run while in-race, regardless of pause.
    // ----------------------------------------------------------------------
    // Track polling and track-change detection are intentionally NOT gated
    // behind the pause check. Empirically the InPauseFlag at 0x0035DCB4 can
    // latch at 0 across whole races (see log 2026-04-16 race 2/3), and when
    // it did, the previous gating caused the overlay to never notice the
    // new song. Decoupling state from pause means: even if the pause flag
    // is wrong, the FADE_IN trigger still fires when a new track starts.
    if ((g_pollFrame++ % kPollInterval) == 0) {
        int idx = TrackWatch_ReadCurrent();
        if (idx != g_lastTrackIdx) {
            g_lastTrackIdx = idx;
            Log("d3d9_hook: track changed -> %d\n", idx);
        }
    }

    // Periodic snapshot while in-race (gives a "steady state" baseline).
    if ((++g_dumpFrameCount % kDumpPeriodFrames) == 0) {
        TrackWatch_DumpState("periodic");
    }

    // Detect track change → rasterize new text texture.
    const TrackInfo* t = nullptr;
    if (g_lastTrackIdx >= 0 && g_lastTrackIdx < MusicCfg_Count()) {
        t = MusicCfg_GetByIndex(g_lastTrackIdx);
    }
    if (t && g_lastTrackIdx != g_npDisplayedTrack) {
        g_npDisplayedTrack = g_lastTrackIdx;
        // Prefix "NOW PLAYING: " for the car-stereo feel.
        char raw[128];
        _snprintf_s(raw, sizeof(raw), _TRUNCATE,
                    "Now playing: %s", t->displayName);
        // Uppercase for VFD look.
        for (char* p = raw; *p; ++p) {
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        }
        strncpy_s(g_npText, sizeof(g_npText), raw, _TRUNCATE);
        // Release old texture.
        if (g_npTextTex) { g_npTextTex->Release(); g_npTextTex = nullptr; }
        g_npTextTex = VfdFont_Rasterize(dev, g_npText, kFontHeight,
                                        &g_npTextW, &g_npTextH);
        if (g_npTextTex) {
            g_npTextTexW = NextPow2(g_npTextW);
            g_npTextTexH = NextPow2(g_npTextH);
        }
        g_npState = NP_FADE_IN;
        g_npStateStart = NowSec();
        g_npFrozenElapsed = -1.0f; // new animation — discard any old frozen position
    }

    // ----------------------------------------------------------------------
    // PAUSE GATE — draw-only, with stuck-flag failsafe.
    // ----------------------------------------------------------------------
    // Confirm pause only between [kPauseDebounceFrames, kStuckPauseFrames).
    // - Below the debounce floor: ignore (1-3 frame dips during track-switch).
    // - Above the stuck ceiling: distrust the flag (it's wrong) and draw.
    // This block only suppresses the *render*; state updates above already
    // ran, so the overlay will reappear correctly when pause clears (or
    // when the failsafe trips).
    bool skipDraw = false;
    {
        int curPauseRaw = TrackWatch_ReadRawInPauseFlag();
        if (curPauseRaw >= 0) { // -1 means not configured — skip detection
            // IsPausedFlag semantics: nonzero = paused, zero = actively racing.
            // (Opposite of the old InPauseFlag "active-race indicator" convention.)
            if (curPauseRaw != 0) {
                g_pauseConsecutive++;
                g_resumeConsecutive = 0;
            } else {
                g_resumeConsecutive++;
                g_pauseConsecutive = 0;
            }

            const bool wantPaused = (g_pauseConsecutive >= kPauseDebounceFrames
                                  && g_pauseConsecutive <  kStuckPauseFrames);

            if (!g_paused && wantPaused) {
                g_paused = true;
                Log("d3d9_hook: Pause confirmed after %d frames\n", g_pauseConsecutive);
            } else if (g_paused && g_resumeConsecutive >= kPauseDebounceFrames) {
                g_paused = false;
                Log("d3d9_hook: Resume confirmed after %d frames\n", g_resumeConsecutive);
            } else if (g_paused && !wantPaused) {
                g_paused = false;
                Log("d3d9_hook: Pause flag stuck for %d frames - distrusting\n",
                    g_pauseConsecutive);
            }
            skipDraw = g_paused;
        }
    }

    if (skipDraw) {
        // Freeze the animation clock the first frame we skip drawing.
        // g_npStateStart is re-biased on resume so elapsed appears continuous,
        // preventing the one-frame full-alpha flash that occurred when elapsed
        // suddenly included the entire pause duration.
        if (g_npFrozenElapsed < 0.0f && g_npState != NP_HIDDEN && g_npTextTex)
            g_npFrozenElapsed = NowSec() - g_npStateStart;
        return;
    }
    // Coming out of skip: restore the clock so elapsed picks up from where
    // it was frozen, not from the moment the game was paused.
    if (g_npFrozenElapsed >= 0.0f) {
        g_npStateStart    = NowSec() - g_npFrozenElapsed;
        g_npFrozenElapsed = -1.0f;
    }

    if (g_npState == NP_HIDDEN) return;
    if (!g_npTextTex) return;

    D3DVIEWPORT9 vp;
    if (FAILED(dev->GetViewport(&vp))) return;
    if (vp.Width == 0 || vp.Height == 0) return;

    // Fixed-size panel: width = fraction of viewport, height = font height + padding.
    const int charW     = VfdFont_CharWidth(kFontHeight);
    int panelChars      = static_cast<int>(vp.Width * kPanelWidthFrac) / charW;
    if (panelChars < 8) panelChars = 8;
    const int panelInnerW = panelChars * charW;
    const int panelInnerH = g_npTextH > 0 ? g_npTextH : kFontHeight;

    const int padX   = 10;
    const int padY   = 6;
    const int blur   = 4;
    const int panelW = panelInnerW + padX * 2;
    const int panelH = panelInnerH + padY * 2;
    const int panelX = static_cast<int>(vp.Width)  / 2 - panelW / 2;
    // Position: above the bottom HUD row (minimap/speedometer occupy
    // the lowest ~28% of the viewport). Place our panel at about 1/phi
    // of the way up from the bottom edge of the center gap, which puts
    // it just above the car-name label and below the road view.
    const int bottomMargin = static_cast<int>(vp.Height * 0.07f);
    const int panelY = static_cast<int>(vp.Height) - panelH - bottomMargin;
    const int innerX = panelX + padX;
    const int innerY = panelY + padY;

    // Lazily rasterize the VFD base texture ("888...8") if the panel
    // char count changed (viewport resize) or it doesn't exist yet.
    if (!g_npBaseTex || g_npLastPanelChars != panelChars) {
        if (g_npBaseTex) { g_npBaseTex->Release(); g_npBaseTex = nullptr; }
        char baseStr[128];
        int fill = panelChars < 127 ? panelChars : 127;
        memset(baseStr, '8', fill);
        baseStr[fill] = '\0';
        g_npBaseTex = VfdFont_Rasterize(dev, baseStr, kFontHeight,
                                        &g_npBaseW, &g_npBaseH);
        if (g_npBaseTex) {
            g_npBaseTexW = NextPow2(g_npBaseW);
            g_npBaseTexH = NextPow2(g_npBaseH);
        }
        g_npLastPanelChars = panelChars;
    }

    // State machine.
    const float now     = NowSec();
    const float elapsed = now - g_npStateStart;
    float alpha    = 1.0f;
    int   scrollOff = panelInnerW;

    switch (g_npState) {
    case NP_FADE_IN:
        alpha = elapsed / kFadeInSec;
        if (alpha >= 1.0f) {
            alpha = 1.0f;
            g_npState = NP_SCROLL;
            g_npStateStart = now;
        }
        scrollOff = panelInnerW;
        break;

    case NP_SCROLL:
        scrollOff = panelInnerW -
                    static_cast<int>(kScrollPxPerSec * elapsed);
        // Once the text starts exiting the left edge of the panel (scrollOff < 0)
        // fade the entire overlay proportionally: alpha goes from 1.0 (text just
        // touching the left edge) to 0.0 (text fully off-screen left).
        // This eliminates the "empty bright panel" flash that occurred when the
        // old code transitioned to a separate NP_FADE_OUT state only after the
        // text was already fully invisible — leaving the panel at alpha=1.0 for
        // the entire kFadeOutSec duration with nothing in it.
        if (scrollOff < 0) {
            alpha = static_cast<float>(scrollOff + g_npTextW) /
                    static_cast<float>(g_npTextW);
            if (alpha <= 0.0f) {
                g_npState = NP_HIDDEN;
                return;
            }
        }
        break;

    case NP_FADE_OUT:
        // No longer reached from NP_SCROLL (fold-into-scroll above).
        // Kept as a safety net in case g_npState is set here externally.
        alpha = 1.0f - elapsed / kFadeOutSec;
        if (alpha <= 0.0f) {
            g_npState = NP_HIDDEN;
            return;
        }
        scrollOff = -g_npTextW;
        break;

    case NP_HIDDEN:
        return;
    }

    // ---- Render ----
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;

    // Common overlay state.
    dev->SetPixelShader(nullptr);
    dev->SetVertexShader(nullptr);
    dev->SetRenderState(D3DRS_ZENABLE,      FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP,   D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE,
                        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);

    const DWORD kPanelBg = ScaleAlpha(0xE00A0F1A, alpha);
    const DWORD kLedBright = ScaleAlpha(0xFF00FFFF, alpha);
    const DWORD kLedDim    = ScaleAlpha(MakeColor(0, 30, 30, 255), alpha);

    // 1. Soft panel background.
    DrawSoftPanel(dev,
        static_cast<float>(panelX), static_cast<float>(panelY),
        static_cast<float>(panelX + panelW), static_cast<float>(panelY + panelH),
        static_cast<float>(blur), kPanelBg);

    // 2. VFD base (dim "888...8") — fixed, clipped tightly to inner area.
    if (g_npBaseTex) {
        RECT scissor = { innerX, panelY, innerX + panelInnerW, panelY + panelH };
        dev->SetScissorRect(&scissor);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);

        float uMax = static_cast<float>(g_npBaseW) / g_npBaseTexW;
        float vMax = static_cast<float>(g_npBaseH) / g_npBaseTexH;
        DrawTexQuad(dev, g_npBaseTex,
            static_cast<float>(innerX), static_cast<float>(innerY),
            static_cast<float>(g_npBaseW), static_cast<float>(g_npBaseH),
            uMax, vMax, kLedDim);

        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    }

    // 3. Scrolling text — scissor-clipped to inner display area only.
    //    Using innerX (not panelX) prevents partial characters from peeking
    //    into the left padding strip as the text scrolls off.
    {
        RECT scissor = { innerX, panelY, innerX + panelInnerW, panelY + panelH };
        dev->SetScissorRect(&scissor);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);

        float uMax = static_cast<float>(g_npTextW) / g_npTextTexW;
        float vMax = static_cast<float>(g_npTextH) / g_npTextTexH;
        float tx   = static_cast<float>(innerX + scrollOff);
        float ty   = static_cast<float>(innerY);
        float tw   = static_cast<float>(g_npTextW);
        float th   = static_cast<float>(g_npTextH);

        // 3a. Glow halo — 4 cardinal taps at a tight spread (no diagonals,
        //     which caused a visible shadow offset in earlier versions).
        {
            dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            DWORD glowCol = ScaleAlpha(MakeColor(0, 255, 255, 20), alpha);
            constexpr float kGlowSpread = 2.0f;
            static const float offsets[][2] = {
                {-kGlowSpread, 0}, {kGlowSpread, 0},
                {0, -kGlowSpread}, {0,  kGlowSpread},
            };
            for (auto& off : offsets) {
                DrawTexQuad(dev, g_npTextTex,
                    tx + off[0], ty + off[1], tw, th,
                    uMax, vMax, glowCol);
            }
            // Restore standard alpha blend for the crisp text pass.
            dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        }

        // 3b. Crisp text on top.
        DrawTexQuad(dev, g_npTextTex, tx, ty, tw, th,
                    uMax, vMax, kLedBright);

        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    }

    sb->Apply();
    sb->Release();
}

// ---------- trampolines ----------

HRESULT WINAPI HkEndScene(IDirect3DDevice9* dev)
{
    // Only draw when render target 0 is the swap-chain back buffer.
    // Juiced runs separate BeginScene/EndScene passes for its shadow maps
    // (render-to-texture), and our hook fires on those too.  If we draw
    // into a shadow-map target our quads get projected onto the road as a
    // shadow rectangle.  The back-buffer check skips all off-screen passes.
    __try {
        bool isBackBuffer = false;
        IDirect3DSurface9* rt = nullptr;
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt) {
            if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
                isBackBuffer = (rt == bb);
                bb->Release();
            }
            rt->Release();
        }
        if (isBackBuffer) {
            DrawNowPlaying(dev);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Swallow any draw fault — never let our overlay crash the game.
    }
    return g_origEndScene(dev);
}

HRESULT WINAPI HkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    // MANAGED textures survive Reset, but if the device is fully lost the
    // pointers become invalid. Release and let them re-create next frame.
    if (g_npTextTex) { g_npTextTex->Release(); g_npTextTex = nullptr; }
    if (g_npBaseTex) { g_npBaseTex->Release(); g_npBaseTex = nullptr; }
    g_npLastPanelChars = 0;
    g_npDisplayedTrack = -1; // force re-rasterize after device reset; prevents
                             // stuck-texture when g_npDisplayedTrack still matches
                             // g_lastTrackIdx but the texture was just released
    return g_origReset(dev, pp);
}

// ---------- installer ----------

bool PatchVTableSlot(void** vtable, int slot, void* newFn, void** outOriginal)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*),
                        PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    *outOriginal = vtable[slot];
    vtable[slot] = newFn;
    DWORD restored = 0;
    VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &restored);
    return true;
}

DWORD WINAPI InstallerThread(LPVOID)
{
    // Brief delay so the game has a chance to load d3d9.dll itself.
    // Vtables are per-class so the order doesn't actually matter for
    // patching, but we want LoadLibraryA below to attach to the same
    // module the game uses rather than loading our own copy first.
    Sleep(1500);

    HMODULE hD3D9 = LoadLibraryA("d3d9.dll");
    if (!hD3D9) {
        Log("d3d9_hook: LoadLibrary(d3d9.dll) failed (err=%lu)\n", GetLastError());
        return 1;
    }

    using Direct3DCreate9_t = IDirect3D9* (WINAPI*)(UINT);
    auto pDirect3DCreate9 = reinterpret_cast<Direct3DCreate9_t>(
        GetProcAddress(hD3D9, "Direct3DCreate9"));
    if (!pDirect3DCreate9) {
        Log("d3d9_hook: GetProcAddress(Direct3DCreate9) failed\n");
        return 2;
    }

    IDirect3D9* d3d = pDirect3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        Log("d3d9_hook: Direct3DCreate9 returned null\n");
        return 3;
    }

    HWND hwnd = CreateWindowExA(0, "STATIC", "JNP", 0, 0, 0, 1, 1,
                                nullptr, nullptr, nullptr, nullptr);
    if (!hwnd) {
        d3d->Release();
        Log("d3d9_hook: CreateWindow failed\n");
        return 4;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed         = TRUE;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow    = hwnd;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth  = 1;
    pp.BackBufferHeight = 1;

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING |
        D3DCREATE_DISABLE_DRIVER_MANAGEMENT,
        &pp, &dev);
    if (FAILED(hr) || !dev) {
        Log("d3d9_hook: CreateDevice failed (hr=0x%08lx)\n",
            static_cast<unsigned long>(hr));
        DestroyWindow(hwnd);
        d3d->Release();
        return 5;
    }

    void** vtable = *reinterpret_cast<void***>(dev);

    bool ok =
        PatchVTableSlot(vtable, kSlotEndScene, reinterpret_cast<void*>(&HkEndScene),
                        reinterpret_cast<void**>(&g_origEndScene)) &&
        PatchVTableSlot(vtable, kSlotReset,    reinterpret_cast<void*>(&HkReset),
                        reinterpret_cast<void**>(&g_origReset));

    dev->Release();
    DestroyWindow(hwnd);
    d3d->Release();

    if (!ok) {
        Log("d3d9_hook: vtable patch failed\n");
        return 6;
    }

    g_hookInstalled = true;
    Log("d3d9_hook: EndScene+Reset patched (sub-phase 9.1)\n");
    return 0;
}

// ---------- shared logging (mirrors dllmain.cpp's WriteLoadLine path) ----------

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

} // namespace

void D3D9Hook_Install()
{
    CreateThread(nullptr, 0, InstallerThread, nullptr, 0, nullptr);
}
