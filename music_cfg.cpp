#include "pch.h"
#include "music_cfg.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>

namespace {

constexpr int kMaxTracks = 32;

TrackInfo g_tracks[kMaxTracks];
int       g_trackCount = 0;
bool      g_loaded     = false;

// Mirrors dllmain.cpp / d3d9_hook.cpp logging — write next to the .asi.
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

// Resolve "<game exe dir>\audio\music\music.cfg".
bool ResolveCfgPath(char* out, size_t outSize)
{
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) return false;
    char* slash = strrchr(exePath, '\\');
    if (!slash) return false;
    *(slash + 1) = '\0';
    if (strlen(exePath) + strlen("audio\\music\\music.cfg") + 1 >= outSize)
        return false;
    strcpy_s(out, outSize, exePath);
    strcat_s(out, outSize, "audio\\music\\music.cfg");
    return true;
}

const char* SkipSpaces(const char* p)
{
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

// Trim trailing whitespace (incl. CR/LF) in place.
void RTrim(char* s)
{
    size_t n = strlen(s);
    while (n > 0) {
        unsigned char c = static_cast<unsigned char>(s[n - 1]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            s[--n] = '\0';
        } else {
            break;
        }
    }
}

bool LineStartsWith(const char* line, const char* prefix)
{
    size_t n = strlen(prefix);
    return strncmp(line, prefix, n) == 0;
}

// Extract value of `displayname "..."`. Returns pointer into `line`
// (null-terminated by overwriting the closing quote) or nullptr.
char* ExtractQuoted(char* line, const char* key)
{
    size_t keyLen = strlen(key);
    if (strncmp(line, key, keyLen) != 0) return nullptr;
    char* p = line + keyLen;
    p = const_cast<char*>(SkipSpaces(p));
    if (*p != '"') return nullptr;
    ++p;
    char* end = strchr(p, '"');
    if (!end) return nullptr;
    *end = '\0';
    return p;
}

// Extract bare token after a key (e.g. `name track01`).
char* ExtractToken(char* line, const char* key)
{
    size_t keyLen = strlen(key);
    if (strncmp(line, key, keyLen) != 0) return nullptr;
    char* p = line + keyLen;
    if (*p != ' ' && *p != '\t') return nullptr;
    p = const_cast<char*>(SkipSpaces(p));
    char* end = p;
    while (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n')
        ++end;
    *end = '\0';
    return p;
}

} // namespace

bool MusicCfg_Load()
{
    if (g_loaded) return g_trackCount > 0;
    g_loaded = true;
    g_trackCount = 0;

    char path[MAX_PATH];
    if (!ResolveCfgPath(path, sizeof(path))) {
        Log("music_cfg: ResolveCfgPath failed\n");
        return false;
    }

    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "rb") != 0 || !fp) {
        Log("music_cfg: fopen failed for %s\n", path);
        return false;
    }

    char line[512];
    bool inBlock = false;
    TrackInfo cur = {};

    while (fgets(line, sizeof(line), fp)) {
        // Strip leading whitespace for keyword matching.
        char* p = const_cast<char*>(SkipSpaces(line));
        RTrim(p);
        if (*p == '\0') continue;

        if (LineStartsWith(p, "[BeginSS]")) {
            inBlock = true;
            memset(&cur, 0, sizeof(cur));
            continue;
        }
        if (LineStartsWith(p, "[EndSS]")) {
            if (inBlock && cur.name[0] && cur.displayName[0] &&
                g_trackCount < kMaxTracks) {
                g_tracks[g_trackCount++] = cur;
            }
            inBlock = false;
            continue;
        }
        if (!inBlock) continue;

        if (char* v = ExtractToken(p, "name")) {
            strncpy_s(cur.name, v, _TRUNCATE);
            continue;
        }
        if (char* v = ExtractQuoted(p, "displayname")) {
            // Trim any trailing space inside the quotes (track07).
            size_t n = strlen(v);
            while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t')) v[--n] = '\0';
            strncpy_s(cur.displayName, v, _TRUNCATE);
            continue;
        }
    }

    fclose(fp);

    Log("music_cfg: parsed %d tracks from %s\n", g_trackCount, path);
    if (g_trackCount > 0) {
        Log("music_cfg: track[0] = %s / \"%s\"\n",
            g_tracks[0].name, g_tracks[0].displayName);
    }
    return g_trackCount > 0;
}

int MusicCfg_Count()
{
    return g_trackCount;
}

const TrackInfo* MusicCfg_GetByIndex(int i)
{
    if (i < 0 || i >= g_trackCount) return nullptr;
    return &g_tracks[i];
}

const TrackInfo* MusicCfg_GetByName(const char* name)
{
    if (!name) return nullptr;
    for (int i = 0; i < g_trackCount; ++i) {
        if (strcmp(g_tracks[i].name, name) == 0) return &g_tracks[i];
    }
    return nullptr;
}
