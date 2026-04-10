#include "pch.h"
#include "bitmap_font.h"
#include "font5x7.h"

#include <d3d9.h>
#include <ctype.h>
#include <string.h>
#include <math.h>

namespace {

struct TLVERTEX {
    float x, y, z, rhw;
    DWORD color;
};
constexpr DWORD TLVERTEX_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

// Per-frame scratch buffer for the untextured (cell) pass.
constexpr size_t kMaxQuads     = 4000;
constexpr size_t kVertsPerQuad = 6;
TLVERTEX g_scratch[kMaxQuads * kVertsPerQuad];
size_t   g_scratchUsed = 0;

// Textured vertex used by the Gaussian glow halo pass.
struct TLVERTEXTEX {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};
constexpr DWORD TLVERTEXTEX_FVF =
    D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

constexpr size_t kMaxTexQuads = 600;
TLVERTEXTEX g_texScratch[kMaxTexQuads * 6];
size_t      g_texScratchUsed = 0;

// Pre-baked 32x32 Gaussian alpha texture, lazy-created on first draw.
// White RGB + Gaussian alpha falloff so it can be tinted to any LED
// color via vertex color modulation.
constexpr int       kGlowTexSize = 32;
IDirect3DTexture9*  g_glowTex    = nullptr;

bool EnsureGlowTexture(IDirect3DDevice9* dev)
{
    if (g_glowTex) return true;
    if (!dev) return false;

    HRESULT hr = dev->CreateTexture(
        kGlowTexSize, kGlowTexSize, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_glowTex, nullptr);
    if (FAILED(hr) || !g_glowTex) {
        g_glowTex = nullptr;
        return false;
    }

    D3DLOCKED_RECT lr = {};
    if (FAILED(g_glowTex->LockRect(0, &lr, nullptr, 0))) {
        g_glowTex->Release();
        g_glowTex = nullptr;
        return false;
    }

    const float center = (kGlowTexSize - 1) * 0.5f;
    // Sigma chosen so the gaussian falls to ~5% at the texture edge.
    // exp(-r^2/(2*sigma^2)) ~= 0.05 at r = (size/2) ~= 15.5.
    const float sigma      = kGlowTexSize / 5.0f;
    const float twoSigmaSq = 2.0f * sigma * sigma;

    BYTE* base = static_cast<BYTE*>(lr.pBits);
    for (int y = 0; y < kGlowTexSize; ++y) {
        DWORD* row = reinterpret_cast<DWORD*>(base + y * lr.Pitch);
        for (int x = 0; x < kGlowTexSize; ++x) {
            const float dx = static_cast<float>(x) - center;
            const float dy = static_cast<float>(y) - center;
            const float r2 = dx * dx + dy * dy;
            const float v  = expf(-r2 / twoSigmaSq);
            BYTE a = static_cast<BYTE>(v * 255.0f + 0.5f);
            row[x] = (static_cast<DWORD>(a) << 24) | 0x00FFFFFFu;
        }
    }
    g_glowTex->UnlockRect(0);
    return true;
}

inline DWORD DimColor(DWORD argb)
{
    // ~12.5% brightness, alpha preserved. Cyan 0xFF00FFFF -> 0xFF001F1F.
    DWORD a =  (argb >> 24) & 0xFF;
    DWORD r = ((argb >> 16) & 0xFF) >> 3;
    DWORD g = ((argb >>  8) & 0xFF) >> 3;
    DWORD b = ( argb        & 0xFF) >> 3;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

inline void EmitQuad(float x0, float y0, float x1, float y1, DWORD col)
{
    if (g_scratchUsed + kVertsPerQuad > kMaxQuads * kVertsPerQuad) {
        return; // silently drop overflow
    }
    TLVERTEX* v = &g_scratch[g_scratchUsed];
    g_scratchUsed += kVertsPerQuad;
    v[0] = { x0, y0, 0.0f, 1.0f, col };
    v[1] = { x1, y0, 0.0f, 1.0f, col };
    v[2] = { x0, y1, 0.0f, 1.0f, col };
    v[3] = { x1, y0, 0.0f, 1.0f, col };
    v[4] = { x1, y1, 0.0f, 1.0f, col };
    v[5] = { x0, y1, 0.0f, 1.0f, col };
}

// Quad with per-corner colors (TL, TR, BL, BR). D3D interpolates the
// colors linearly across each triangle, giving a smooth gradient.
inline void EmitQuad4(float x0, float y0, float x1, float y1,
                      DWORD cTL, DWORD cTR, DWORD cBL, DWORD cBR)
{
    if (g_scratchUsed + kVertsPerQuad > kMaxQuads * kVertsPerQuad) return;
    TLVERTEX* v = &g_scratch[g_scratchUsed];
    g_scratchUsed += kVertsPerQuad;
    v[0] = { x0, y0, 0.0f, 1.0f, cTL };
    v[1] = { x1, y0, 0.0f, 1.0f, cTR };
    v[2] = { x0, y1, 0.0f, 1.0f, cBL };
    v[3] = { x1, y0, 0.0f, 1.0f, cTR };
    v[4] = { x1, y1, 0.0f, 1.0f, cBR };
    v[5] = { x0, y1, 0.0f, 1.0f, cBL };
}

// Single triangle with 3 individual vertex colors. Used for soft corner
// fades on the panel border.
inline void EmitTri(float x0, float y0, DWORD c0,
                    float x1, float y1, DWORD c1,
                    float x2, float y2, DWORD c2)
{
    if (g_scratchUsed + 3 > kMaxQuads * kVertsPerQuad) return;
    TLVERTEX* v = &g_scratch[g_scratchUsed];
    g_scratchUsed += 3;
    v[0] = { x0, y0, 0.0f, 1.0f, c0 };
    v[1] = { x1, y1, 0.0f, 1.0f, c1 };
    v[2] = { x2, y2, 0.0f, 1.0f, c2 };
}

inline void EmitTexQuad(float x0, float y0, float x1, float y1, DWORD col)
{
    if (g_texScratchUsed + 6 > kMaxTexQuads * 6) return;
    TLVERTEXTEX* v = &g_texScratch[g_texScratchUsed];
    g_texScratchUsed += 6;
    v[0] = { x0, y0, 0.0f, 1.0f, col, 0.0f, 0.0f };
    v[1] = { x1, y0, 0.0f, 1.0f, col, 1.0f, 0.0f };
    v[2] = { x0, y1, 0.0f, 1.0f, col, 0.0f, 1.0f };
    v[3] = { x1, y0, 0.0f, 1.0f, col, 1.0f, 0.0f };
    v[4] = { x1, y1, 0.0f, 1.0f, col, 1.0f, 1.0f };
    v[5] = { x0, y1, 0.0f, 1.0f, col, 0.0f, 1.0f };
}

void FlushTexScratch(IDirect3DDevice9* dev)
{
    if (g_texScratchUsed == 0) return;
    UINT primCount = static_cast<UINT>(g_texScratchUsed / 3);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, primCount,
                         g_texScratch, sizeof(TLVERTEXTEX));
    g_texScratchUsed = 0;
}

void FlushScratch(IDirect3DDevice9* dev)
{
    if (g_scratchUsed == 0) return;
    UINT primCount = static_cast<UINT>(g_scratchUsed / 3);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, primCount,
                         g_scratch, sizeof(TLVERTEX));
    g_scratchUsed = 0;
}

void ApplyOverlayState(IDirect3DDevice9* dev)
{
    dev->SetTexture(0, nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(TLVERTEX_FVF);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
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
                        D3DCOLORWRITEENABLE_RED   |
                        D3DCOLORWRITEENABLE_GREEN |
                        D3DCOLORWRITEENABLE_BLUE  |
                        D3DCOLORWRITEENABLE_ALPHA);
}

} // namespace

int BitmapFont_MeasureCols(const char* text, const FontMetrics& m)
{
    if (!text || !*text) return 0;
    int n = 0;
    for (const char* p = text; *p; ++p) ++n;
    return n * kFontGlyphW + (n - 1) * m.charGapCols;
}

int BitmapFont_GridWidthPx(int cols, const FontMetrics& m)
{
    if (cols <= 0) return 0;
    // N LEDs of width ledPx, separated by (N-1) gaps of ledGap.
    return cols * m.ledPx + (cols - 1) * m.ledGap;
}

int BitmapFont_GridHeightPx(const FontMetrics& m)
{
    return kFontGlyphH * m.ledPx + (kFontGlyphH - 1) * m.ledGap;
}

void BitmapFont_DrawRect(IDirect3DDevice9* dev,
                         int x, int y, int w, int h,
                         unsigned int color)
{
    if (!dev || w <= 0 || h <= 0) return;
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;

    ApplyOverlayState(dev);
    g_scratchUsed = 0;
    EmitQuad(static_cast<float>(x),
             static_cast<float>(y),
             static_cast<float>(x + w),
             static_cast<float>(y + h),
             static_cast<DWORD>(color));
    FlushScratch(dev);

    sb->Apply();
    sb->Release();
}

void BitmapFont_DrawSoftPanel(IDirect3DDevice9* dev,
                              int x, int y, int w, int h,
                              unsigned int color,
                              int blurPx)
{
    if (!dev || w <= 0 || h <= 0) return;
    if (blurPx < 0) blurPx = 0;

    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;

    ApplyOverlayState(dev);
    g_scratchUsed = 0;

    const DWORD cFull = static_cast<DWORD>(color);
    const DWORD cZero = cFull & 0x00FFFFFF; // alpha = 0, RGB preserved

    const float fx0 = static_cast<float>(x);
    const float fy0 = static_cast<float>(y);
    const float fx1 = static_cast<float>(x + w);
    const float fy1 = static_cast<float>(y + h);
    const float b   = static_cast<float>(blurPx);

    // Center: solid full-alpha rect.
    EmitQuad(fx0, fy0, fx1, fy1, cFull);

    if (blurPx > 0) {
        // Top fade: top edge alpha=0, bottom edge alpha=full.
        EmitQuad4(fx0, fy0 - b, fx1, fy0,
                  cZero, cZero, cFull, cFull);
        // Bottom fade.
        EmitQuad4(fx0, fy1, fx1, fy1 + b,
                  cFull, cFull, cZero, cZero);
        // Left fade.
        EmitQuad4(fx0 - b, fy0, fx0, fy1,
                  cZero, cFull, cZero, cFull);
        // Right fade.
        EmitQuad4(fx1, fy0, fx1 + b, fy1,
                  cFull, cZero, cFull, cZero);

        // Corner triangles. Each is a right triangle whose right-angle
        // vertex sits on the panel corner (alpha=full) and whose two
        // other vertices sit on the outer edges of the adjacent fade
        // strips (alpha=0). Linear interpolation across the triangle
        // gives a soft diagonal falloff in the corner cap.
        // Top-left.
        EmitTri(fx0,     fy0,     cFull,
                fx0 - b, fy0,     cZero,
                fx0,     fy0 - b, cZero);
        // Top-right.
        EmitTri(fx1,     fy0,     cFull,
                fx1,     fy0 - b, cZero,
                fx1 + b, fy0,     cZero);
        // Bottom-left.
        EmitTri(fx0,     fy1,     cFull,
                fx0,     fy1 + b, cZero,
                fx0 - b, fy1,     cZero);
        // Bottom-right.
        EmitTri(fx1,     fy1,     cFull,
                fx1 + b, fy1,     cZero,
                fx1,     fy1 + b, cZero);
    }

    FlushScratch(dev);
    sb->Apply();
    sb->Release();
}

void BitmapFont_DrawDotMatrix(IDirect3DDevice9* dev,
                              int originX, int originY,
                              int cols,
                              unsigned int litColor,
                              const FontMetrics& m,
                              const char* text)
{
    if (!dev) return;

    const int textCols = BitmapFont_MeasureCols(text, m);
    if (cols <= 0) cols = textCols;
    if (cols <= 0) return;

    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;

    ApplyOverlayState(dev);
    g_scratchUsed = 0;

    const int    led    = m.ledPx;
    const int    gap    = m.ledGap;
    const int    pitch  = led + gap;
    const DWORD  litC   = static_cast<DWORD>(litColor);
    const DWORD  dimC   = DimColor(litC);
    const int    rows   = kFontGlyphH;
    const int    glyphAdvanceCols = kFontGlyphW + m.charGapCols;

    // 1) Build a per-cell "is lit" bitmap so we draw exactly one quad per
    //    cell (lit or dim) and never overdraw.
    // For "HELLO JUICED" cols=71, rows=7 -> 497 cells, comfortably small.
    constexpr int kMaxCols = kMaxQuads / kFontGlyphH; // upper bound
    if (cols > kMaxCols) cols = kMaxCols;
    static uint8_t s_lit[kMaxQuads];
    for (int i = 0; i < cols * rows; ++i) s_lit[i] = 0;

    if (text && *text) {
        int charIdx = 0;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
             *p; ++p, ++charIdx)
        {
            unsigned char c = *p;
            if (c >= 128) c = '?';
            if (c >= 'a' && c <= 'z') c -= 32;

            const int baseCol = charIdx * glyphAdvanceCols;
            if (baseCol >= cols) break; // ran past the panel

            const uint8_t* glyph = kFontGlyphs[c];
            for (int row = 0; row < rows; ++row) {
                uint8_t bits = glyph[row];
                if (bits == 0) continue;
                for (int gc = 0; gc < kFontGlyphW; ++gc) {
                    if (bits & (1 << (kFontGlyphW - 1 - gc))) {
                        const int col_ = baseCol + gc;
                        if (col_ < 0 || col_ >= cols) continue;
                        s_lit[row * cols + col_] = 1;
                    }
                }
            }
        }
    }

    // 2) Pass 1 — emit one quad per cell at the standard alpha-blend.
    for (int row = 0; row < rows; ++row) {
        const float y0 = static_cast<float>(originY + row * pitch);
        const float y1 = y0 + led;
        for (int col_ = 0; col_ < cols; ++col_) {
            const float x0 = static_cast<float>(originX + col_ * pitch);
            const float x1 = x0 + led;
            const DWORD c  = s_lit[row * cols + col_] ? litC : dimC;
            EmitQuad(x0, y0, x1, y1, c);
        }
    }
    FlushScratch(dev);

    // 3) Pass 2 — soft Gaussian halo around each lit LED, additive
    //    blended. The halo size is tuned so within-glyph halos overlap
    //    strongly (letter shapes look continuous and glowy) but the
    //    inter-glyph blank LED column stays visibly darker.
    if (EnsureGlowTexture(dev)) {
        dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        dev->SetTexture(0, g_glowTex);
        dev->SetFVF(TLVERTEXTEX_FVF);
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

        // Halo quad ~12 px across (LED is 4, so it extends ~4 px past
        // each edge). Within-row stride is 5 px so adjacent within-glyph
        // halos overlap heavily; inter-glyph adjacent lit LEDs are 10 px
        // apart so their halos only barely meet at the dim column.
        constexpr int  kGlowHalf = 8;
        const DWORD    glowVCol  =
            (litC & 0x00FFFFFFu) | 0x73000000u; // ~45% peak intensity

        g_texScratchUsed = 0;
        for (int row = 0; row < rows; ++row) {
            for (int col_ = 0; col_ < cols; ++col_) {
                if (!s_lit[row * cols + col_]) continue;
                const float ledCx =
                    static_cast<float>(originX + col_ * pitch) + led * 0.5f;
                const float ledCy =
                    static_cast<float>(originY + row  * pitch) + led * 0.5f;
                EmitTexQuad(ledCx - kGlowHalf, ledCy - kGlowHalf,
                            ledCx + kGlowHalf, ledCy + kGlowHalf,
                            glowVCol);
            }
        }
        FlushTexScratch(dev);
    }

    sb->Apply();
    sb->Release();
}

// ---------------------------------------------------------------------------
// 9.7 — combined "Now Playing" scrolling display
// ---------------------------------------------------------------------------

namespace {

inline DWORD ScaleAlpha(DWORD argb, float mul)
{
    DWORD a = (argb >> 24) & 0xFF;
    a = static_cast<DWORD>(static_cast<float>(a) * mul + 0.5f);
    if (a > 255) a = 255;
    return (a << 24) | (argb & 0x00FFFFFFu);
}

} // namespace

void BitmapFont_DrawNowPlaying(
    IDirect3DDevice9* dev,
    int panelX, int panelY, int panelW, int panelH,
    unsigned int panelBg, int blurPx,
    int padX, int padY,
    int panelCols,
    unsigned int litColor,
    const FontMetrics& m,
    const char* text,
    int scrollOffPx,
    float alphaMul)
{
    if (!dev || panelW <= 0 || panelH <= 0 || alphaMul <= 0.0f) return;
    if (alphaMul > 1.0f) alphaMul = 1.0f;

    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;

    ApplyOverlayState(dev);

    const int   led   = m.ledPx;
    const int   gap   = m.ledGap;
    const int   pitch = led + gap;
    const int   rows  = kFontGlyphH;
    const DWORD litC  = ScaleAlpha(static_cast<DWORD>(litColor), alphaMul);
    const DWORD dimC  = ScaleAlpha(DimColor(static_cast<DWORD>(litColor)), alphaMul);
    const DWORD bgC   = ScaleAlpha(static_cast<DWORD>(panelBg), alphaMul);

    const int innerX = panelX + padX;
    const int innerY = panelY + padY;

    // ---- 1. Soft panel background ----
    {
        g_scratchUsed = 0;
        const DWORD cFull = bgC;
        const DWORD cZero = cFull & 0x00FFFFFFu;
        const float fx0 = static_cast<float>(panelX);
        const float fy0 = static_cast<float>(panelY);
        const float fx1 = static_cast<float>(panelX + panelW);
        const float fy1 = static_cast<float>(panelY + panelH);
        const float b   = static_cast<float>(blurPx);

        EmitQuad(fx0, fy0, fx1, fy1, cFull);
        if (blurPx > 0) {
            EmitQuad4(fx0, fy0 - b, fx1, fy0, cZero, cZero, cFull, cFull);
            EmitQuad4(fx0, fy1, fx1, fy1 + b, cFull, cFull, cZero, cZero);
            EmitQuad4(fx0 - b, fy0, fx0, fy1, cZero, cFull, cZero, cFull);
            EmitQuad4(fx1, fy0, fx1 + b, fy1, cFull, cZero, cFull, cZero);
            EmitTri(fx0, fy0, cFull, fx0 - b, fy0, cZero, fx0, fy0 - b, cZero);
            EmitTri(fx1, fy0, cFull, fx1, fy0 - b, cZero, fx1 + b, fy0, cZero);
            EmitTri(fx0, fy1, cFull, fx0, fy1 + b, cZero, fx0 - b, fy1, cZero);
            EmitTri(fx1, fy1, cFull, fx1 + b, fy1, cZero, fx1, fy1 + b, cZero);
        }
        FlushScratch(dev);
    }

    // ---- 2. Unlit (dim) LED grid — fills the fixed panel interior ----
    {
        g_scratchUsed = 0;
        for (int row = 0; row < rows; ++row) {
            const float y0 = static_cast<float>(innerY + row * pitch);
            const float y1 = y0 + led;
            for (int col = 0; col < panelCols; ++col) {
                const float x0 = static_cast<float>(innerX + col * pitch);
                const float x1 = x0 + led;
                EmitQuad(x0, y0, x1, y1, dimC);
            }
        }
        FlushScratch(dev);
    }

    // ---- 3. Lit LEDs — scrolled, scissor-clipped to panel ----
    if (text && *text) {
        RECT scissor;
        scissor.left   = static_cast<LONG>(panelX);
        scissor.top    = static_cast<LONG>(panelY);
        scissor.right  = static_cast<LONG>(panelX + panelW);
        scissor.bottom = static_cast<LONG>(panelY + panelH);
        dev->SetScissorRect(&scissor);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);

        const int glyphAdvancePx = (kFontGlyphW + m.charGapCols) * pitch;

        g_scratchUsed = 0;
        int charIdx = 0;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
             *p; ++p, ++charIdx)
        {
            unsigned char c = *p;
            if (c >= 128) c = '?';
            if (c >= 'a' && c <= 'z') c -= 32;

            const int charPxX = innerX + scrollOffPx + charIdx * glyphAdvancePx;

            // Skip glyphs entirely outside the panel.
            if (charPxX + kFontGlyphW * pitch < panelX) continue;
            if (charPxX > panelX + panelW) break;

            const uint8_t* glyph = kFontGlyphs[c];
            for (int row = 0; row < rows; ++row) {
                uint8_t bits = glyph[row];
                if (bits == 0) continue;
                const float y0 = static_cast<float>(innerY + row * pitch);
                const float y1 = y0 + led;
                for (int gc = 0; gc < kFontGlyphW; ++gc) {
                    if (!(bits & (1 << (kFontGlyphW - 1 - gc)))) continue;
                    const float x0 = static_cast<float>(charPxX + gc * pitch);
                    const float x1 = x0 + led;
                    EmitQuad(x0, y0, x1, y1, litC);
                }
            }
        }
        FlushScratch(dev);

        // ---- 4. Glow halos for lit LEDs (additive, still scissored) ----
        if (EnsureGlowTexture(dev)) {
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            dev->SetTexture(0, g_glowTex);
            dev->SetFVF(TLVERTEXTEX_FVF);
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

            constexpr int kGlowHalf = 8;
            const DWORD glowVCol =
                ScaleAlpha((static_cast<DWORD>(litColor) & 0x00FFFFFFu) | 0x73000000u,
                           alphaMul);

            g_texScratchUsed = 0;
            charIdx = 0;
            for (const unsigned char* p2 =
                     reinterpret_cast<const unsigned char*>(text);
                 *p2; ++p2, ++charIdx)
            {
                unsigned char c = *p2;
                if (c >= 128) c = '?';
                if (c >= 'a' && c <= 'z') c -= 32;

                const int charPxX =
                    innerX + scrollOffPx + charIdx * glyphAdvancePx;
                if (charPxX + kFontGlyphW * pitch < panelX - kGlowHalf)
                    continue;
                if (charPxX > panelX + panelW + kGlowHalf) break;

                const uint8_t* glyph = kFontGlyphs[c];
                for (int row = 0; row < rows; ++row) {
                    uint8_t bits = glyph[row];
                    if (bits == 0) continue;
                    for (int gc = 0; gc < kFontGlyphW; ++gc) {
                        if (!(bits & (1 << (kFontGlyphW - 1 - gc)))) continue;
                        const float cx =
                            static_cast<float>(charPxX + gc * pitch) +
                            led * 0.5f;
                        const float cy =
                            static_cast<float>(innerY + row * pitch) +
                            led * 0.5f;
                        EmitTexQuad(cx - kGlowHalf, cy - kGlowHalf,
                                    cx + kGlowHalf, cy + kGlowHalf,
                                    glowVCol);
                    }
                }
            }
            FlushTexScratch(dev);
        }

        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    }

    sb->Apply();
    sb->Release();
}
