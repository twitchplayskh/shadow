# SHADOW

A modernisation layer for the PC version of **Shadow of Destiny / Shadow of Memories** (Konami, 2002).

Named after the town the game takes place in.

> **Status: experimental and still very buggy.** Expect visual glitches, missing effects in some rooms and the
> occasional crash. Back up your game folder and your saves before installing. Every change can be undone by
> deleting the added files.

## How it works

SHADOW doesn't change any of the game's own files. It's a set of drop-in DLLs that load next to
`shadow.exe` and work on the game as it runs.

- **`dinput8.dll` (SoDFix)** is a proxy for the system DirectInput DLL, so Windows loads it automatically when the
  game starts. It forwards every DirectInput call to the real `dinput8.dll` and patches `shadow.exe` in memory:
  new menu rows, extra resolutions, screen modes, keyboard layout, draw distance, alt-tab behaviour and so on.
  Every patch checks the original bytes first and is reverted when the game exits.
- **`d3d8.dll` / `d3d9.dll`** are a fork of [DXVK](https://github.com/doitsujin/dxvk) 3.1.1. The game's
  Direct3D 8 calls are translated to Vulkan, and the new rendering features (ray-traced shadows, ambient
  occlusion, reflections, SMAA, filtering) are added inside that translation layer. The game never sees them.
- **`sod_rt64.exe`** is a small 64-bit helper. Hardware ray tracing in Vulkan is only offered to 64-bit programs,
  and the game is 32-bit, so the helper traces the shadow rays on the same GPU and hands the result back every
  frame through shared GPU memory. It starts and stops with the game.
- **`SoDFix\option_e.tbn2`** holds the options menu graphics with the new rows added, built from the game's own
  font. The game's original file is left untouched; SoDFix redirects the read to this copy.
- **`SoDFix\reflect.txt`** lists the polished surfaces that get reflections. It was generated from the game's
  room data (`ccc.wad`), so every room is covered without needing a playthrough.

All new options appear in the game's own OPTION menus and are saved to `SoDFix.ini`.

## Enhancements

**Display**
- New resolutions: 1440 x 1080, 1920 x 1440 and 2880 x 2160 (native 4:3 on a 4K screen)
- Correct 4:3 picture with black bars at any monitor resolution, no stretching
- SCREEN MODE: borderless or fullscreen, both without a display mode switch
- Intro and attract videos shown in 4:3 instead of stretched
- CINEMATIC BARS: turn the cutscene letterbox off for full-height cutscenes
- Working alt-tab. The original game blocked it with a keyboard hook, a hotkey and a Windows 98 trick. Keys
  pressed in other programs no longer reach the game.

**Graphics (Vulkan)**
- Vulkan renderer through a DXVK fork
- RT SHADOW CASTING: hardware ray-traced character shadows (MEDIUM sharp, HIGH / ULTRA soft), replacing the
  stencil shadows. Needs a GPU with Vulkan ray tracing.
- AMBIENT OCCLUSION: GTAO from the depth buffer, LOW / HIGH
- REFLECTIONS: screen-space reflections on the polished floors, marble and metal surfaces listed in
  `reflect.txt`, LOW / HIGH
- ANTI-ALIASING: SMAA
- TEXTURE FILTERING: standard, trilinear or 16x anisotropic
- DRAW DISTANCE: new EXTRA setting that doubles the outdoor view distance. It also draws everything within it,
  so distant walls no longer pop in and out.

**Controls**
- CONTROLLER SETTING > LAYOUT: MODERN keyboard layout (WASD to move, E / Space to interact, Tab inventory, M map,
  Z / C camera). The original keys keep working too.

**Menus**
- New option rows drawn in the game's own font, with page scrolling and help lines
- Menu fixes for the extra rows (cursor, scroll bar, door icons)

## Known issues

- Reflections look wrong on some floors (see the cafe, now excluded) and are limited to what is on screen
- Wall reflections are disabled for now
- Baked blob shadows under props stay; props do not cast ray-traced shadows yet
- Some rooms show artefacts where the game's own see-through effects meet the new ones
- Ray-traced shadows only come from characters (the game's own shadow casters)
- Not tested on AMD or Intel GPUs

## Requirements

- Shadow of Destiny / Shadow of Memories for PC (the Konami release, `shadow.exe`)
- Windows 10 or 11
- A GPU with Vulkan 1.3. RT SHADOW CASTING needs Vulkan ray tracing (developed on an RTX 4070).

## Installing

1. Download the release and copy everything in it into the game folder, next to `shadow.exe`
   (`C:\Program Files (x86)\Konami\Shadow Of Destiny` by default; Windows will ask for admin rights).
2. Start the game and open OPTION > GRAPHIC SETTING.

To uninstall, delete `dinput8.dll`, `d3d8.dll`, `d3d9.dll`, `sod_rt64.exe`, `dxvk.conf`, `SoDFix.ini` and the
`SoDFix` folder.

Logs (`SoDFix.log`, `shadow_d3d9.log`) are written next to `shadow.exe`, or under
`%LOCALAPPDATA%\VirtualStore\Program Files (x86)\Konami\Shadow Of Destiny` when the game is in Program Files.

## Repository layout

| Folder | Contents |
| --- | --- |
| `dxvk/` | DXVK 3.1.1 fork. Game-specific code is in `src/d3d9/d3d9_sod_*`, `src/d3d9/shaders/sod_*`, `src/d3d8/d3d8_main.cpp` and `src/sod_bridge/` (the 64-bit helper) |
| `sodfix/` | The `dinput8.dll` proxy (`src/sodfix.cpp`) and the default `SoDFix.ini`, `dxvk.conf` and `reflect.txt` |
| `tools/menu_art/` | Python scripts that build `option_e.tbn2` from the game's copy |
| `tools/reflections/` | `ccc.wad` reader and the scripts that produced `reflect.txt` |
| `tools/capture/` | Readers for the F12 frame captures (`%LOCALAPPDATA%\SoDRT\sod_rt_capture2_N.bin`) |
| `docs/` | Reverse-engineering notes: addresses, data formats, design decisions |

## Building

**DXVK fork** (`d3d8.dll`, `d3d9.dll`), on Linux with mingw-w64, meson, ninja and glslang:

```
cd dxvk
meson setup --cross-file build-win32.txt --buildtype release build32
ninja -C build32
```

**64-bit helper** (`sod_rt64.exe`): `sh dxvk/src/sod_bridge/build_helper.sh` (mingw-w64 x86_64 + glslangValidator).

**SoDFix** (`dinput8.dll`): `sh sodfix/build.sh` (uses zig as the cross compiler: `pip install ziglang`).

**Menu graphics and reflection list**: the scripts in `tools/` read the game's own files. Put a copy of the game
in `work/game` (or set `SOD_GAME_DIR`). Game data is not included in this repository.

## Credits and licence

- DXVK by Philip Rebohle and contributors, zlib licence (`dxvk/LICENSE`)
- SMAA by Jorge Jimenez et al. (MIT, bundled in the DXVK fork)
- Shadow of Destiny / Shadow of Memories is © Konami. This project contains no game code. You need your own copy
  of the game.

The rest of the project is released under the zlib licence (`LICENSE`).
