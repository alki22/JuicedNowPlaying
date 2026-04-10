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

bool TrackWatch_IsInRace()
{
    if (!g_inited || !g_moduleBase) return false;
    __try {
        int val = *reinterpret_cast<int*>(g_moduleBase + g_cfg.inRaceFlag);
        return val != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
