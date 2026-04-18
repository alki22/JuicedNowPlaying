# JuicedNowPlaying

An ASI plugin for **Juiced** (Juice Games / THQ, 2005) that displays a
"Now Playing" song title overlay during races, styled as a retro dot-matrix
car stereo display.

![D3D9 overlay](https://img.shields.io/badge/D3D9-overlay-blue)
![Win32](https://img.shields.io/badge/platform-Win32-lightgrey)
![License](https://img.shields.io/badge/license-MIT-green)

## Features

- Fixed-size LED panel centered at the bottom of the screen
- 5x7 dot-matrix font with unlit LED positions visible (dimmed)
- Right-to-left marquee scroll, then fade out
- Gaussian glow halos around lit LEDs (additive blending)
- Soft blurry panel borders
- Only appears during races (not in menus, garage, loading screens, or results)
- Reads the game's `music.cfg` for real track metadata
- **In-race song skip**: `,` = previous track, `.` = next track (configurable)
- Configurable pointer chain, LED color, and skip-key VK codes via `.ini` file

## Installation

### Prerequisites

- **Juiced** (2005 PC release) installed
- **ThirteenAG's Ultimate ASI Loader** (`dinput8.dll`) placed in
  `C:\Games\Juiced\` (the game's root directory). Download from
  [github.com/ThirteenAG/Ultimate-ASI-Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).
  The loader must be configured to load `.asi` files from a `scripts\`
  subdirectory.

### Steps

1. Create a `scripts` folder inside the game directory if it doesn't
   exist:
   ```
   C:\Games\Juiced\scripts\
   ```

2. Copy **`JuicedNowPlaying.asi`** from the
   [Releases](https://github.com/alki22/JuicedNowPlaying/releases) page
   into `scripts\`.

3. Copy **`JuicedNowPlaying.ini`** (from the repo root or the release
   zip) into `scripts\` next to the `.asi`.

4. Launch Juiced. Start a race. The LED panel appears at the bottom
   center, scrolls the current song title once, then fades out. It
   re-triggers on every song change.

### Configuration

Edit `scripts\JuicedNowPlaying.ini` to customize:

| Key | Section | Description |
|-----|---------|-------------|
| `TrackIdBase` | `[Memory]` | Offset of the track-id pointer in `Juiced.exe` |
| `TrackIdOffsets` | `[Memory]` | Comma-separated pointer chain offsets |
| `InRaceFlag` | `[Memory]` | Offset of the in-race boolean flag |
| `LedColor` | `[Display]` | `R, G, B` (0-255) for the LED dot color |
| `PrevTrackKey` | `[Controls]` | VK code for skip-previous (default `0xBC` = `,`) |
| `NextTrackKey` | `[Controls]` | VK code for skip-next (default `0xBE` = `.`) |

All values accept hex (`0xBC`) or decimal. Changes take effect on the next
game launch — no rebuild needed. The default addresses were verified on the
original 2005 retail release (unpatched). If they don't work on your version,
see [Re-finding the addresses](#re-finding-the-addresses-with-cheat-engine)
below.

#### Skip-key reference

Common VK codes if you want to remap the skip keys:

| Key | VK code |
|-----|---------|
| `,` | `0xBC` (default prev) |
| `.` | `0xBE` (default next) |
| `;` | `0xBA` |
| `'` | `0xDE` |

Full list: [Microsoft Virtual-Key Codes](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes)

## Design

### Aesthetic

The overlay is modeled after a 2005-era car stereo vacuum-fluorescent
display (VFD). Key visual decisions:

- **Continuous dot-matrix grid**: every LED position is drawn, not just
  the lit ones. Unlit dots are rendered at ~12.5% of the lit color's
  brightness. This makes the display read as a physical LED matrix, not
  floating text.
- **Fixed-size panel**: the panel has a constant width (~28% of viewport
  width). Long titles scroll through it right-to-left rather than
  resizing the panel. This matches how real head units work.
- **Gaussian glow**: each lit LED gets an additive-blended halo from a
  pre-baked 32x32 Gaussian alpha texture (sigma = 32/5). The halo size
  (8px radius) is tuned so within-glyph halos overlap heavily (letters
  look like continuous glowing shapes) but the inter-glyph blank column
  stays visibly darker.
- **Soft panel border**: the dark background panel fades to transparent
  over 4 pixels using per-vertex alpha interpolation on fade strips and
  corner triangles. No textures needed.
- **Default cyan (#00FFFF)**: matches Juiced's stock HUD accent color.

### Architecture

```
DllMain (dllmain.cpp)
  |-> MusicCfg_Load()      -- parse audio/music/music.cfg
  |-> TrackWatch_Init()     -- read .ini, resolve Juiced.exe base
  |-> D3D9Hook_Install()    -- spawn worker thread
        |-> sleep 1.5s (loader-lock safety)
        |-> LoadLibrary("d3d9.dll"), create temp device
        |-> patch vtable slots 42 (EndScene) and 16 (Reset)

EndScene hook (d3d9_hook.cpp)
  |-> skip keys: GetAsyncKeyState for prev/next track (VK codes from ini)
  |-> two-part race gate:
  |     primary  — InRaceFlag == 0 -> hide and bail
  |     secondary — [root+0x40] != null -> loading screen, hide and bail
  |                 (one-shot per race; ignored after loading screen clears)
  |-> every 10 frames: TrackWatch_ReadCurrent()  -- SEH-wrapped pointer walk
  |-> if track changed: trigger FADE_IN -> SCROLL -> FADE_OUT -> HIDDEN
  |-> draw overlay (single state-block save/restore):
        |-> soft panel background (untextured quads)
        |-> unlit LED grid (fixed position)
        |-> lit LEDs (scrolled, scissor-clipped)
        |-> glow halos (additive blend, scissor-clipped)
```

**Key engineering choices**:

- **Per-class vtable patching** instead of MinHook or Detours. The D3D9
  vtable is shared by all devices of the same class, so patching via a
  temporary dummy device retroactively hooks whichever device the game
  created. No imports beyond `d3d9.dll` and `kernel32.dll`.
- **SEH wrapping** (`__try / __except`) around every pointer dereference
  and draw call. A bad read during a loading screen returns -1; a draw
  failure is silently swallowed. The game never crashes from overlay code.
- **State block save/restore** (`IDirect3DStateBlock9` with `D3DSBT_ALL`)
  ensures the game's render state is untouched after overlay drawing.
- **No dynamic allocation**. All storage is static arrays sized at
  compile time (font glyphs, track table, vertex scratch buffers).
- **DrawPrimitiveUP** with pre-transformed vertices (`D3DFVF_XYZRHW`)
  for all geometry. No vertex buffers, no shaders, no D3DX dependency.
- **Two-part race gate**: `g_wasGated` is armed only by the `InRaceFlag`
  0→1 transition; the `[root+0x40]` loading-screen pointer is only checked
  until it first clears per race (`g_raceLoaded`). This prevents spurious
  animation resets when the pause menu briefly touches the same field.

### Font

The 5x7 dot-matrix font (`font5x7.h`) is a hand-designed 128-entry
ASCII table. Each glyph is 5 columns wide, 7 rows tall, stored as 7
bytes where bit 4 = leftmost column. Uppercase A-Z, digits 0-9, and
common punctuation are fully designed. Lowercase input is uppercased at
draw time.

### Music metadata

Track names are read from the game's own `audio\music\music.cfg`, which
contains 25 `[BeginSS]` / `[EndSS]` blocks with `displayname` fields
like `"Beans - Down By Law"`. The parser runs once at DLL load and builds
a static table mapping 0-indexed track IDs to display names.

## Re-finding the addresses with Cheat Engine

If the default `.ini` values don't work (e.g. a different game version or
region), you'll need Cheat Engine 7.x to locate two values:

### Track ID (pointer chain)

The currently-playing track is stored as a 0-indexed `int` on the heap.
We reach it via a pointer chain anchored in `Juiced.exe`'s static data.

1. Attach CE to `Juiced.exe`.
2. Start a race. Note which song is playing (check the table in
   `music.cfg` for its 0-based index).
3. **Scan type**: Exact Value, **Value type**: 4 Bytes, value = the
   0-based track index.
4. Wait for the song to change. Note the new index. **Next Scan** with
   the new value.
5. Repeat 3-4 times until you have a handful of addresses.
6. The address is on the heap (not static), so right-click it ->
   **Pointer scan for this address**. Set max level = 5, "Pointers must
   end with specific offsets" = the final field offset (likely `0x20`,
   matching the `mov [ecx+20], edx` write instruction).
7. **Quit the game, relaunch**, start a race, re-find the address with a
   fresh scan, then **Rescan memory** in the pointer-scan window with
   the new address.
8. Repeat step 7 once more (3 total game sessions). The surviving
   `Juiced.exe`-anchored chains are stable.
9. Pick the shortest 2-level chain. Update `TrackIdBase` and
   `TrackIdOffsets` in the `.ini`.

**Verified chain** (retail 2005 unpatched):
```
Juiced.exe + 0x0035DC74  ->  [+0x48]  ->  [+0x20]  =  track index
```

The write instruction is `Juiced.exe+C60C69: mov [ecx+20], edx`.

### In-race flag (static)

A simple boolean: `1` during a race (including the pause menu), `0`
otherwise.

1. In CE, scan for `0` while in the main menu.
2. Start a race. Next Scan for `1`.
3. Return to menu. Next Scan for `0`.
4. Repeat 2-3 times. Look for **static** (green) `Juiced.exe+XXXXXX`
   addresses.
5. Update `InRaceFlag` in the `.ini`.

**Verified addresses** (retail 2005 unpatched):
- `Juiced.exe + 0x0032024C` (used by default)
- `Juiced.exe + 0x0035E0E8` (alternative, same behavior)

## Building from source

### Requirements

- **MSVC 2022 Build Tools** (v143 toolset, Windows SDK 10.0)
- No full Visual Studio IDE needed

### Build

```bat
cd JuicedNowPlaying
build.bat
```

`build.bat` invokes `cl.exe` directly (bypassing MSBuild's LTCG
incremental-link cache, which can silently reuse stale object code).
All translation units are always recompiled from source. The script also
copies the built `.asi` to `C:\Games\Juiced\scripts\` automatically.

Output: `manual_build\JuicedNowPlaying.asi`

## File structure

```
JuicedNowPlaying.sln           Solution file
JuicedNowPlaying.vcxproj       MSBuild project (Win32, v143, C++17)
build.bat                      One-click build + install script
JuicedNowPlaying.ini           Runtime config (ships next to .asi)

dllmain.cpp                    DLL entry point, orchestrates init
d3d9_hook.cpp / .h             EndScene/Reset vtable hook + state machine
bitmap_font.cpp / .h           Dot-matrix renderer (untextured + glow)
vfd_font.cpp / .h              VFD-style font texture pipeline
font5x7.h                      Hand-designed 5x7 ASCII glyph table
music_cfg.cpp / .h             music.cfg parser (track name table)
ini_reader.cpp / .h            Minimal .ini reader for memory addresses + key codes
track_watch.cpp / .h           SEH-wrapped pointer chain walker + config accessors
framework.h                    Win32 lean-and-mean header
pch.h / pch.cpp                Precompiled header
```

## License

MIT
