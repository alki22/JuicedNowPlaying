#include "pch.h"
#include "vfd_font.h"

#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#pragma comment(lib, "gdi32.lib")

namespace {

char  g_fontPath[MAX_PATH] = {};
bool  g_fontLoaded = false;
char  g_fontFace[64] = "Alarm Clock";

void Log(const char* fmt, ...)
{
    char modulePath[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&Log), &self);
    if (!self) return;
    if (GetModuleFileNameA(self, modulePath, MAX_PATH) == 0) return;
    char* dot = strrchr(modulePath, '.');
    if (!dot) return;
    strcpy_s(dot, MAX_PATH - (dot - modulePath), ".log");

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

bool ResolveFontPath()
{
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&ResolveFontPath), &self);
    if (!self) return false;
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(self, path, MAX_PATH) == 0) return false;
    char* slash = strrchr(path, '\\');
    if (!slash) return false;
    *(slash + 1) = '\0';
    strcat_s(path, "alarm_clock.ttf");
    strcpy_s(g_fontPath, path);
    return true;
}

// Round up to next power of 2 (D3D textures often prefer POT dimensions).
int NextPow2(int v)
{
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

} // namespace

bool VfdFont_Init()
{
    if (g_fontLoaded) return true;

    if (!ResolveFontPath()) {
        Log("vfd_font: ResolveFontPath failed\n");
        return false;
    }

    int added = AddFontResourceExA(g_fontPath, FR_PRIVATE, nullptr);
    if (added == 0) {
        Log("vfd_font: AddFontResourceEx failed for %s\n", g_fontPath);
        return false;
    }

    g_fontLoaded = true;
    Log("vfd_font: loaded %s (%d fonts)\n", g_fontPath, added);
    return true;
}

int VfdFont_CharWidth(int heightPx)
{
    // The Alarm Clock font is monospaced. At any given point size the
    // advance is approximately 60% of the em height. Compute via GDI
    // once and cache.
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return heightPx * 6 / 10;

    HFONT hFont = CreateFontA(
        heightPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FF_DONTCARE, g_fontFace);
    if (!hFont) { DeleteDC(hdc); return heightPx * 6 / 10; }

    HFONT old = (HFONT)SelectObject(hdc, hFont);
    SIZE sz = {};
    GetTextExtentPoint32A(hdc, "8", 1, &sz);
    SelectObject(hdc, old);
    DeleteObject(hFont);
    DeleteDC(hdc);
    return sz.cx > 0 ? sz.cx : heightPx * 6 / 10;
}

IDirect3DTexture9* VfdFont_Rasterize(IDirect3DDevice9* dev,
                                     const char* text,
                                     int heightPx,
                                     int* outW, int* outH)
{
    if (!dev || !text || !*text || !g_fontLoaded) return nullptr;

    // ---- 1. Measure text extent via GDI ----
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return nullptr;

    HFONT hFont = CreateFontA(
        heightPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, FF_DONTCARE, g_fontFace);
    if (!hFont) { DeleteDC(hdc); return nullptr; }

    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
    int len = static_cast<int>(strlen(text));
    SIZE sz = {};
    GetTextExtentPoint32A(hdc, text, len, &sz);
    if (sz.cx <= 0 || sz.cy <= 0) {
        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
        DeleteDC(hdc);
        return nullptr;
    }

    int texW = NextPow2(sz.cx);
    int texH = NextPow2(sz.cy);

    // ---- 2. Create a DIB section and render text ----
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = texW;
    bmi.bmiHeader.biHeight      = -texH; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hBmp || !bits) {
        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
        DeleteDC(hdc);
        return nullptr;
    }

    HBITMAP oldBmp = (HBITMAP)SelectObject(hdc, hBmp);
    // Clear to transparent black.
    memset(bits, 0, texW * texH * 4);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255)); // white; tinted via vertex color
    TextOutA(hdc, 0, 0, text, len);

    GdiFlush();

    // ---- 3. Copy into a D3D9 texture ----
    // GDI antialiased text writes to RGB channels but leaves alpha at 0.
    // We derive alpha from the max of R,G,B so the glyph edges are smooth.
    DWORD* pixels = static_cast<DWORD*>(bits);
    for (int i = 0; i < texW * texH; ++i) {
        DWORD px = pixels[i];
        BYTE r = (px >> 16) & 0xFF;
        BYTE g = (px >>  8) & 0xFF;
        BYTE b =  px        & 0xFF;
        BYTE a = r > g ? (r > b ? r : b) : (g > b ? g : b);
        pixels[i] = (static_cast<DWORD>(a) << 24) | 0x00FFFFFFu;
    }

    IDirect3DTexture9* tex = nullptr;
    HRESULT hr = dev->CreateTexture(texW, texH, 1, 0,
                                    D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                    &tex, nullptr);
    if (FAILED(hr) || !tex) {
        SelectObject(hdc, oldBmp);
        SelectObject(hdc, oldFont);
        DeleteObject(hBmp);
        DeleteObject(hFont);
        DeleteDC(hdc);
        return nullptr;
    }

    D3DLOCKED_RECT lr = {};
    if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
        BYTE* dst = static_cast<BYTE*>(lr.pBits);
        BYTE* src = static_cast<BYTE*>(bits);
        for (int y = 0; y < texH; ++y) {
            memcpy(dst + y * lr.Pitch, src + y * texW * 4, texW * 4);
        }
        tex->UnlockRect(0);
    } else {
        tex->Release();
        tex = nullptr;
    }

    // ---- 4. Cleanup GDI ----
    SelectObject(hdc, oldBmp);
    SelectObject(hdc, oldFont);
    DeleteObject(hBmp);
    DeleteObject(hFont);
    DeleteDC(hdc);

    if (outW) *outW = sz.cx;  // actual text width, not POT
    if (outH) *outH = sz.cy;
    if (tex) {
        // Store the actual/POT ratio so the caller can set UV correctly.
        // We pass actual pixel dims via outW/outH; the caller computes
        // u_max = outW / texW, v_max = outH / texH.
    }

    return tex;
}

void VfdFont_Shutdown()
{
    if (g_fontLoaded && g_fontPath[0]) {
        RemoveFontResourceExA(g_fontPath, FR_PRIVATE, nullptr);
        g_fontLoaded = false;
    }
}
