#include "pch.h"
#include "ini_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

bool ResolveIniPath(char* out, size_t outSize)
{
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&ResolveIniPath),
        &self);
    if (!self) return false;
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(self, path, MAX_PATH) == 0) return false;
    char* dot = strrchr(path, '.');
    if (!dot) return false;
    size_t prefixLen = static_cast<size_t>(dot - path);
    if (prefixLen + 5 >= MAX_PATH) return false;
    strcpy_s(dot, MAX_PATH - prefixLen, ".ini");
    if (strlen(path) + 1 > outSize) return false;
    strcpy_s(out, outSize, path);
    return true;
}

unsigned int ParseUInt(const char* s)
{
    if (!s || !*s) return 0;
    return static_cast<unsigned int>(strtoul(s, nullptr, 0));
}

int ParseCsvHex(const char* s, unsigned int* out, int maxOut)
{
    if (!s || !*s) return 0;
    int n = 0;
    const char* p = s;
    while (*p && n < maxOut) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        if (!*p) break;
        char* end = nullptr;
        unsigned long v = strtoul(p, &end, 0);
        if (end == p) break;
        out[n++] = static_cast<unsigned int>(v);
        p = end;
    }
    return n;
}

void ParseRgb(const char* s, unsigned char* r, unsigned char* g, unsigned char* b)
{
    unsigned int vals[3] = { 0, 255, 255 };
    int n = 0;
    const char* p = s;
    while (*p && n < 3) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        if (!*p) break;
        char* end = nullptr;
        unsigned long v = strtoul(p, &end, 0);
        if (end == p) break;
        vals[n++] = static_cast<unsigned int>(v);
        p = end;
    }
    *r = static_cast<unsigned char>(vals[0] & 0xFF);
    *g = static_cast<unsigned char>(vals[1] & 0xFF);
    *b = static_cast<unsigned char>(vals[2] & 0xFF);
}

} // namespace

bool Ini_Load(IniConfig* out)
{
    memset(out, 0, sizeof(*out));
    // Defaults.
    out->trackIdBase          = 0x0035DC74;
    out->trackIdOffsets[0]    = 0x48;
    out->trackIdOffsets[1]    = 0x20;
    out->trackIdOffsetCount   = 2;
    out->inRaceFlag           = 0x0032024C;
    out->ledR = 0;
    out->ledG = 255;
    out->ledB = 255;
    out->prevTrackKey = 0xBA;  // VK_OEM_1 = ;
    out->nextTrackKey = 0xDE;  // VK_OEM_7 = '

    char path[MAX_PATH] = {};
    if (!ResolveIniPath(path, sizeof(path))) return false;
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return false;

    char buf[256];

    GetPrivateProfileStringA("Memory", "TrackIdBase", "",
                             buf, sizeof(buf), path);
    if (buf[0]) out->trackIdBase = ParseUInt(buf);

    GetPrivateProfileStringA("Memory", "TrackIdOffsets", "",
                             buf, sizeof(buf), path);
    if (buf[0]) {
        int n = ParseCsvHex(buf, out->trackIdOffsets, 8);
        if (n > 0) out->trackIdOffsetCount = n;
    }

    GetPrivateProfileStringA("Memory", "InRaceFlag", "",
                             buf, sizeof(buf), path);
    if (buf[0]) out->inRaceFlag = ParseUInt(buf);

    GetPrivateProfileStringA("Memory", "InPauseFlag", "",
                             buf, sizeof(buf), path);
    if (buf[0]) out->inPauseFlag = ParseUInt(buf);

    GetPrivateProfileStringA("Display", "LedColor", "",
                             buf, sizeof(buf), path);
    if (buf[0]) ParseRgb(buf, &out->ledR, &out->ledG, &out->ledB);

    GetPrivateProfileStringA("Controls", "PrevTrackKey", "",
                             buf, sizeof(buf), path);
    if (buf[0]) out->prevTrackKey = ParseUInt(buf);

    GetPrivateProfileStringA("Controls", "NextTrackKey", "",
                             buf, sizeof(buf), path);
    if (buf[0]) out->nextTrackKey = ParseUInt(buf);

    return true;
}
