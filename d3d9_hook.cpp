#include "pch.h"
#include "d3d9_hook.h"
#include "bitmap_font.h"
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

// 9.7 — display state machine.
enum NpState { NP_HIDDEN, NP_FADE_IN, NP_SCROLL, NP_FADE_OUT };
NpState  g_npState = NP_HIDDEN;
float    g_npStateStart = 0.0f;
char     g_npText[128] = {};
int      g_npTextGridW = 0;
int      g_npDisplayedTrack = -1;

constexpr float kFadeInSec     = 0.4f;
constexpr float kFadeOutSec    = 0.5f;
constexpr float kScrollPxPerSec = 120.0f;
constexpr float kPanelWidthFrac = 0.28f;

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

// ---------- drawing ----------

void DrawNowPlaying(IDirect3DDevice9* dev)
{
    // Gate on in-race. Reset state when leaving a race.
    if (!TrackWatch_IsInRace()) {
        if (g_npState != NP_HIDDEN) {
            g_npState = NP_HIDDEN;
            g_npDisplayedTrack = -1;
        }
        return;
    }

    D3DVIEWPORT9 vp;
    if (FAILED(dev->GetViewport(&vp))) return;
    if (vp.Width == 0 || vp.Height == 0) return;

    FontMetrics fm;
    fm.ledPx       = 4;
    fm.ledGap      = 1;
    fm.charGapCols = 1;

    // Poll the pointer chain.
    if ((g_pollFrame++ % kPollInterval) == 0) {
        int idx = TrackWatch_ReadCurrent();
        if (idx != g_lastTrackIdx) {
            g_lastTrackIdx = idx;
            Log("d3d9_hook: track changed -> %d\n", idx);
        }
    }

    // Detect track change → trigger the display.
    const TrackInfo* t = nullptr;
    if (g_lastTrackIdx >= 0 && g_lastTrackIdx < MusicCfg_Count()) {
        t = MusicCfg_GetByIndex(g_lastTrackIdx);
    }
    if (t && g_lastTrackIdx != g_npDisplayedTrack) {
        g_npDisplayedTrack = g_lastTrackIdx;
        strncpy_s(g_npText, sizeof(g_npText), t->displayName, _TRUNCATE);
        g_npTextGridW = BitmapFont_GridWidthPx(
            BitmapFont_MeasureCols(g_npText, fm), fm);
        g_npState = NP_FADE_IN;
        g_npStateStart = NowSec();
    }

    if (g_npState == NP_HIDDEN) return;

    // Fixed-size panel geometry.
    const int pitch = fm.ledPx + fm.ledGap;
    int panelInnerW = static_cast<int>(vp.Width * kPanelWidthFrac);
    int panelCols   = (panelInnerW + fm.ledGap) / pitch;
    if (panelCols < 20) panelCols = 20;
    panelInnerW     = BitmapFont_GridWidthPx(panelCols, fm);

    const int padX   = 8;
    const int padY   = 6;
    const int blur   = 4;
    const int panelW = panelInnerW + padX * 2;
    const int panelH = BitmapFont_GridHeightPx(fm) + padY * 2;
    const int panelX = static_cast<int>(vp.Width)  / 2 - panelW / 2;
    const int panelY = static_cast<int>(vp.Height) - panelH - 60;

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
        if (scrollOff < -g_npTextGridW) {
            g_npState = NP_FADE_OUT;
            g_npStateStart = now;
            scrollOff = -g_npTextGridW;
        }
        break;

    case NP_FADE_OUT:
        alpha = 1.0f - elapsed / kFadeOutSec;
        if (alpha <= 0.0f) {
            g_npState = NP_HIDDEN;
            return;
        }
        scrollOff = -g_npTextGridW;
        break;

    case NP_HIDDEN:
        return;
    }

    const unsigned int kPanelBg  = 0xE00A0F1A;
    const unsigned int kLedColor = 0xFF00FFFF;

    BitmapFont_DrawNowPlaying(dev,
        panelX, panelY, panelW, panelH,
        kPanelBg, blur, padX, padY,
        panelCols, kLedColor, fm,
        g_npText, scrollOff, alpha);
}

// ---------- trampolines ----------

HRESULT WINAPI HkEndScene(IDirect3DDevice9* dev)
{
    __try {
        DrawNowPlaying(dev);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Swallow any draw fault — never let our overlay crash the game.
    }
    return g_origEndScene(dev);
}

HRESULT WINAPI HkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    // No D3DPOOL_DEFAULT resources owned by us yet (9.1 uses DrawPrimitiveUP
    // with inline vertices), so we don't need to release/recreate anything.
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
