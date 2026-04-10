// JuicedNowPlaying.asi
//
// Sub-phase 9.0: bootstrap.
// Goal: prove the .asi loads under ThirteenAG's Ultimate ASI Loader by
// dropping a single line into a log file at DLL_PROCESS_ATTACH time.
// No hooks, no D3D, no game-state reads. If this line shows up next to
// the game install after launch, the toolchain works and we can move on
// to 9.1 (D3D9 EndScene hook).

#include "pch.h"
#include "d3d9_hook.h"
#include "music_cfg.h"
#include "track_watch.h"
#include <stdio.h>
#include <time.h>

namespace
{
    void WriteLoadLine()
    {
        // Resolve the path of the .asi we're running from, then write the
        // log next to it. This works regardless of the game's CWD.
        char modulePath[MAX_PATH] = {};
        HMODULE self = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&WriteLoadLine),
            &self);
        if (self == nullptr) {
            return;
        }
        if (GetModuleFileNameA(self, modulePath, MAX_PATH) == 0) {
            return;
        }

        // Replace ".asi" (or whatever extension) with ".log".
        char* dot = strrchr(modulePath, '.');
        if (dot == nullptr) {
            return;
        }
        // Make sure ".log" fits within MAX_PATH.
        size_t prefixLen = static_cast<size_t>(dot - modulePath);
        if (prefixLen + 5 >= MAX_PATH) {
            return;
        }
        strcpy_s(dot, MAX_PATH - prefixLen, ".log");

        FILE* fp = nullptr;
        if (fopen_s(&fp, modulePath, "ab") != 0 || fp == nullptr) {
            return;
        }

        time_t now = time(nullptr);
        struct tm tmNow;
        localtime_s(&tmNow, &now);
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmNow);

        fprintf(fp, "[%s] JuicedNowPlaying loaded (sub-phase 9.5)\n", stamp);
        fclose(fp);
    }
}

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD reason, LPVOID /*lpReserved*/)
{
    if (reason == DLL_PROCESS_ATTACH) {
        WriteLoadLine();
        MusicCfg_Load();
        TrackWatch_Init();
        D3D9Hook_Install();
    }
    return TRUE;
}
