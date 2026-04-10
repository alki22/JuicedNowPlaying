#pragma once

// Sub-phase 9.1 — D3D9 EndScene vtable hook + magenta probe.
//
// Install the hook from a worker thread (DllMain is loader-lock-restricted).
// The trampoline draws a 320x40 magenta bar at lower-center every frame.
// Vtable patching is per-class, so the patch applies to whichever
// IDirect3DDevice9 the game has already created (or will create later).

void D3D9Hook_Install();
