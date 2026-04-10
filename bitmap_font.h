#pragma once

// Sub-phase 9.2 — bitmap-font drawing on top of the D3D9 EndScene hook.
//
// No textures, no D3DX, no PNG. Each glyph is rendered as a grid of
// solid-colored quads (one per lit LED) using DrawPrimitiveUP, the same
// primitive the magenta probe used. The result reads as a real
// dot-matrix LED display.

struct IDirect3DDevice9;

// Metrics. ledPx and ledGap are in screen pixels; charGapCols is in LED
// columns (so the grid stays continuous and aligned).
struct FontMetrics {
    int ledPx       = 4; // size of one LED dot, in screen pixels
    int ledGap      = 1; // gap between adjacent LED dots, in screen pixels
    int charGapCols = 1; // blank LED columns between adjacent glyphs
};

// Total LED columns occupied by `text` (sum of glyph widths + gaps).
int  BitmapFont_MeasureCols  (const char* text, const FontMetrics& m);
// Pixel width/height of a grid spanning `cols` columns / kFontGlyphH rows.
int  BitmapFont_GridWidthPx  (int cols, const FontMetrics& m);
int  BitmapFont_GridHeightPx (const FontMetrics& m);

// Draws a filled rectangle in `color` (ARGB). Hard edges; use
// DrawSoftPanel when you want the blurry/glowing border look.
void BitmapFont_DrawRect(IDirect3DDevice9* dev,
                         int x, int y, int w, int h,
                         unsigned int color);

// Draws a filled rectangle with soft, faded borders. The center (x,y,w,h)
// is fully opaque in `color`; outside the rect, alpha fades to 0 over
// `blurPx` pixels in every direction (top/bottom/left/right + corners).
// Use this for the stereo display panel background.
void BitmapFont_DrawSoftPanel(IDirect3DDevice9* dev,
                              int x, int y, int w, int h,
                              unsigned int color,
                              int blurPx);

// Draws a continuous dot-matrix display: every LED position in the grid
// is rendered, unlit cells in a dimmed version of `litColor`, lit cells
// (the `text` glyphs) at full brightness. The grid is left-aligned at
// (originX, originY); callers handle centering / panel layout.
//
// String is uppercased internally (the font has no lowercase entries).
// Pass an explicit `cols` >= measured width to draw a wider grid than the
// text needs (e.g. fixed-size display panel that the text scrolls
// through later in 9.7); pass 0 to use exactly the text's width.
void BitmapFont_DrawDotMatrix(IDirect3DDevice9* dev,
                              int originX, int originY,
                              int cols,
                              unsigned int litColor,
                              const FontMetrics& m,
                              const char* text);

// 9.7: All-in-one "Now Playing" render. Draws the soft panel background,
// a fixed-width unlit LED grid, and the scrolling lit text + glow, all
// within a single state-block save/restore. Scissor-clips the scrolling
// text to the panel bounds.
//
//   scrollOffPx — pixel offset of the text grid relative to the panel's
//                 left inner edge. Positive = text is to the right of the
//                 panel (not yet visible). Decreases over time for R-to-L.
//   alphaMul    — 0..1 fade multiplier applied to all elements.
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
    float alphaMul);
