#pragma once

struct IDirect3DDevice9;
struct IDirect3DTexture9;

// Sub-phase 9.7 — VFD font renderer.
//
// Loads the "Alarm Clock" TTF font and rasterizes strings into D3D9
// textures via GDI. Each texture is white-on-transparent so it can
// be tinted to any color at draw time via vertex color modulation.

// Load the TTF from the same directory as the .asi.
// Call once at startup (from DllMain, before any D3D calls).
bool VfdFont_Init();

// Rasterize `text` at `heightPx` into a D3D9 texture.
// Returns nullptr on failure. Caller must Release() when done.
// *outW and *outH receive the texture dimensions in pixels.
IDirect3DTexture9* VfdFont_Rasterize(IDirect3DDevice9* dev,
                                     const char* text,
                                     int heightPx,
                                     int* outW, int* outH);

// Character cell width for the given font height (the font is
// monospaced, so every glyph has the same advance).
int VfdFont_CharWidth(int heightPx);

// Clean up (RemoveFontResourceEx). Call from DLL_PROCESS_DETACH.
void VfdFont_Shutdown();
