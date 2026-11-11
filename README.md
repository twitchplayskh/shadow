# shadow

A modernisation layer for the PC version of **Shadow of Destiny / Shadow of Memories** (Konami, 2002).

Named after the town the game takes place in.

> **Status: experimental and still very buggy.** Expect visual glitches, missing effects in some rooms and the
> occasional crash. Back up your game folder and your saves before installing. Every change can be undone by
> deleting the added files.

## How it works

shadow doesn't change any of the game's own files. It's a set of drop-in DLLs that load next to
`shadow.exe` and work on the game as it runs.

- **`dinput8.dll` (SoDFix)** is a proxy for the system DirectInput DLL, so Windows loads it automatically when the
  game starts. It forwards every DirectInput call to the real `dinput8.dll` and patches `shadow.exe` in memory:
  new menu rows, extra resolutions, screen modes, keyboard layout, draw distance, alt-tab behaviour and so on.
  Every patch checks the original bytes first and is reverted when the game exits.
- **`d3d8.dll` / `d3d9.dll`** are a fork of [DXVK](https://github.com/doitsujin/dxvk) 3.1.1. The game's
  Direct3D 8 calls are translated to Vulkan, and the new rendering features (ray-traced shadows, ambient
  occlusion, reflections, texture upscaling, SMAA, filtering) are added inside that translation layer. The game never sees them.
- **`sod_rt64.exe`** is a small 64-bit helper. Hardware ray tracing in Vulkan is only offered to 64-bit programs,
  and the game is 32-bit, so the helper traces the shadow rays on the same GPU and hands the result back every
  frame through shared GPU memory. It starts and stops with the game.
  The characters' ray-tracing structures are refitted from one frame to the next instead of rebuilt (same
  triangles at their new positions, so the shadows are identical), with a full rebuild every 30 frames or whenever
  the set of characters changes. `SODRT_NO_REFIT=1` turns this off for comparison.
  Soft shadows (HIGH / ULTRA) only send their cone of rays where a penumbra can actually reach: from each pixel's
  distance to the casters, the light's size and the angle the light meets the surface, the shader works out how
  far a soft edge could spread on screen and checks for a hard shadow edge within that distance. Everywhere else
  every ray of the cone would agree with the one hard ray already traced, so none are sent.
- **`SoDFix\option_e.tbn2`** holds the options menu graphics with the new rows added, built from the game's own
  font. The game's original file is left untouched; SoDFix redirects the read to this copy.
- **`SoDFix\reflect.txt`** lists the polished surfaces that get reflections. It was generated from the game's
  room data (`ccc.wad`), so every room is covered without needing a playthrough.
- **`SoDFix\crisp\*.a8`** are the redrawn subtitle font and clock digits (see CRISP TEXT below); all other text is
  redrawn from the game's own images as they load.
- **`relief.txt`** (in the source only) lists ground textures for the experimental surface depth, which is off.

All new options appear in the game's own OPTION menus and are saved to `SoDFix.ini`.

## Enhancements

**Display**
- New resolutions: 1440 x 1080, 1920 x 1440 and 2880 x 2160 (native 4:3 on a 4K screen)
- Correct 4:3 picture with black bars at any monitor resolution, no stretching
- SCREEN MODE: borderless or fullscreen, both without a display mode switch
- Intro and attract videos shown in 4:3 instead of stretched
- CINEMATIC BARS: turn the cutscene letterbox off for full-height cutscenes
- SMOOTH CAMERA (ON by default): some of the game's follow cameras lurch while you walk at a steady pace (measured
  60 to 105 world units per frame around 80, while the frames themselves arrive evenly). The camera's position is
  evened out before each frame is drawn: its speed follows the average of the last half second, with a pull back to
  the game's own position (on average 26 to 39 units away, at most about 120). Cuts and snaps are passed through.
  Shadow-volume shaders get the same camera, so everything in the frame agrees. A camera recorder (F10, or the file
  `%LOCALAPPDATA%\SoDRT\camtrace.go`) writes 600 frames of camera and timing to `camtrace.csv`
- Working alt-tab. The original game blocked it with a keyboard hook, a hotkey and a Windows 98 trick. Keys
  pressed in other programs no longer reach the game.

**Graphics (Vulkan)**
- Vulkan renderer through a DXVK fork
- RT SHADOW CASTING: hardware ray-traced character shadows (MEDIUM sharp, HIGH / ULTRA soft), replacing the
  stencil shadows. Needs a GPU with Vulkan ray tracing.
- AMBIENT OCCLUSION: GTAO from the depth buffer, LOW / HIGH
- REFLECTIONS: screen-space reflections on the polished floors, marble and metal surfaces listed in
  `reflect.txt`, LOW / HIGH
- TEXTURE UPSCALING: OFF / 2X / 3X / 4X. The game's textures are uploaded to the GPU enlarged (4X: 4x up to
  128 x 128 and 2x up to 512 x 512; 3X: 3x and 2x; 2X: 2x). An edge-directed upscaler fills them straight away;
  then, in the background with a time budget per frame (lower at lower settings), a compact Real-ESRGAN network
  running in compute shaders replaces them with a properly detailed version, colour-corrected so every texel
  keeps its original average colour. Results are kept in a disk cache (`%LOCALAPPDATA%\SoDRT\texcache`), so a
  texture seen once is upscaled instantly from then on. Text (fonts, menus, credits; `text_textures.txt`) never
  goes through the network: it is redrawn by CRISP TEXT instead. The game still works with its original
  textures; only the copy on the graphics card is bigger. A change applies to textures loaded afterwards.
- IMPROVED GEOMETRY: ON / OFF (default OFF = the original look). The game lays its cobbled streets one polygon
  at a time, each polygon showing one whole tile of the same texture rotated or mirrored on its own, so every
  polygon edge is a visible seam. With this on, those streets are textured in world space instead: the tile repeats
  seamlessly at the game's own scale, and three rotated / mirrored copies are blended by a large soft noise mask so
  no repeat shows. Done in the fixed-function pipeline at draw time, so the game's own lighting and fog are kept.
  Each street draw is split triangle by triangle: everything facing up (flat ground and slopes up to about 45
  degrees) whose texture coordinates lie in a listed street tile gets the seamless layout; curb sides, steps, walls
  and other images on the same texture page (tree leaves, for example) keep the game's own mapping. The split is
  worked out once per draw and kept as an index buffer of its own (rebuilt only if the game rewrites that vertex
  or index data), so a converted street costs about the same as the game's own draw. 35 ground textures
  (`geometry.txt`, measured from the game's own vertex data). Set `SODRT_GEOM_DEBUG=1` to tint converted ground
  red; textures that are skipped are logged once with the reason in `d3d9.log`
  Walls: 243 plain wall surfaces (stone, brick, render, plaster; picked by eye from every image the game uses on
  vertical faces, including the town's own building sheets 4401-4443, leaving out windows, doors, timber framing,
  wood, plants, roofs, signs, decorations and images with a painted band). Each wall face gets one layout aligned to
  the world: along the wall and down from a fixed height, so brick rows and joints continue across polygons. No
  rotation, mirroring or blending. The tile size, the way up and whether the image lies on its side are measured on
  each wall as it is converted, from the game's own mapping of that face (`geometry.txt` holds a fallback size).
  A draw may hold any number of wall faces. The log lists every texture seen on vertical faces once
  (`wall survey`), with whether it is in the list, to find walls still missing
- CRISP TEXT (always on): the game's text is stored as small sheets of letters, most with a dark outline baked
  around them; enlarged 4 to 5 times for a 4K screen the outline turns into dark smudges and the letters blur.
  Every text image is redrawn once when it is first used, at 4 to 8 times its size, and drawn in its place (same
  texture coordinates, with mipmaps and smooth filtering). The game's own images are not changed.
  - Subtitles: redrawn from Inter Medium (a thinner, clean typeface), every letter on the original baseline at its
    original place in the font sheet (`SoDFix\crisp\77.a8`); accented letters and button symbols are traced from
    the sheet. The redrawn sheet is matched to the game's texture band by band, so it lines up whichever part of
    the font the game loads. A faint, soft dark band sits behind subtitle lines (text from the subtitle font in the
    lower part of the screen), at most 42% darker in the middle and fading with the text.
  - Clock and date: digits redrawn from Liberation Serif, each fitted to the original digit (`SoDFix\crisp\488.a8`)
  - Area names, the "Time passed" counter: letters traced from the sheet with the outline dropped
  - Menus, settings, help, save screens, chapter screens, credits, logos: the same look (colours, backgrounds,
    artwork) with sharp edges: every channel is enlarged with a smooth curve, then pulled toward the darkest or
    brightest of its four source pixels, so edges sharpen and nothing new appears
  - Small outlined labels with nothing else on the sheet (pause menu, YES / NO, ARE YOU SURE): the dark halo is
    dropped, the letters kept as they are and enlarged with sharp edges
  Which kind a sheet is, is decided from its pixels (how much dark halo sits around the white letters, whether it
  holds pictures or coloured artwork) and logged once per sheet. `SODRT_CRISPTEXT=0` turns it off,
  `SODRT_SUBBAND=0` only the subtitle band
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
- Surface depth (parallax relief on ground textures) is in the code but switched off until it looks right
- Baked blob shadows under props stay; props do not cast ray-traced shadows yet
- Some rooms show artefacts where the game's own see-through effects meet the new ones
- Ray-traced shadows only come from characters (the game's own shadow casters)
- Not tested on AMD or Intel GPUs
- CRISP TEXT: the smallest help lines are about 10 pixels tall in the game's files, so they come out cleaner but
  not much sharper; text next to the screenshots on the help pages keeps its dark halo. Each text sheet is redrawn
  the first time it appears, which can cause a short hitch when a menu screen opens for the first time. Letter
  spacing of the subtitles follows the game's own width table, so the thinner letters can look slightly loose

## Requirements

- Shadow of Destiny / Shadow of Memories for PC (the Konami release, `shadow.exe`)
- Windows 10 or 11
- A GPU with Vulkan 1.3. RT SHADOW CASTING needs Vulkan ray tracing (developed on an RTX 4070).

## Installing

1. Download the fix EXE and place in game folder skip if updateing https://www.moddb.com/downloads/fixed-exe-for-sodsom-vista-newer
2. Download the release and copy everything in it into the game folder, next to `shadow.exe`
   (`C:\Program Files (x86)\Konami\Shadow Of Destiny` by default; Windows will ask for admin rights).
4. Start the game and open OPTION > GRAPHIC SETTING.

To uninstall, delete `dinput8.dll`, `d3d8.dll`, `d3d9.dll`, `sod_rt64.exe`, `dxvk.conf`, `SoDFix.ini` and the
`SoDFix` folder.

Logs (`SoDFix.log`, `shadow_d3d9.log`, or `shadow_lb_d3d9.log` for the 4 GB copy) are written next to `shadow.exe`, or under
`%LOCALAPPDATA%\VirtualStore\Program Files (x86)\Konami\Shadow Of Destiny` when the game is in Program Files.

## Repository layout

| Folder | Contents |
| --- | --- |
| `dxvk/` | DXVK 3.1.1 fork. Game-specific code is in `src/d3d9/d3d9_sod_*`, `src/d3d9/shaders/sod_*`, `src/d3d8/d3d8_main.cpp` and `src/sod_bridge/` (the 64-bit helper) |
| `sodfix/` | The `dinput8.dll` proxy (`src/sodfix.cpp`) and the default `SoDFix.ini`, `dxvk.conf`, `reflect.txt`, `relief.txt`, `geometry.txt`, `crisp/` |
| `tools/menu_art/` | Python scripts that build `option_e.tbn2` from the game's copy |
| `tools/reflections/` | `ccc.wad` reader and the scripts that produced `reflect.txt` |
| `tools/upscale/` | Converts the Real-ESRGAN weights to `sr_weights.bin`; builds the list of text textures |
| `tools/geometry/` | Measures the street tiles' and walls' world size for `geometry.txt`; wall picks (`make_wall_list.py`, `picks_town.py`); offline preview of the new layout |
| `tools/text/` | CRISP TEXT: decodes the lettering sheets and redraws the subtitle font and clock digits (`SoDFix\crisp\*.a8`); font licences |
| `tools/relief/` | The script that produced `relief.txt` and an offline prototype of the height map generator |
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

**Redrawn text**: `python3 tools/text/extract_sheets.py <game folder> work/text`, then
`crisp_subtitle_font.py work/text/fonte_all.png 77.a8` and `crisp_timefont.py work/text/timefont.png 488.a8`
(Inter and Liberation fonts installed; Debian / Ubuntu: `fonts-inter fonts-liberation`).

**Menu graphics and reflection list**: the scripts in `tools/` read the game's own files. Put a copy of the game
in `work/game` (or set `SOD_GAME_DIR`). Game data is not included in this repository.

## Credits and licence

- DXVK by Philip Rebohle and contributors, zlib licence (`dxvk/LICENSE`)
- SMAA by Jorge Jimenez et al. (MIT, bundled in the DXVK fork)
- Belleza by Eduardo Tunni (SIL Open Font License): the digits of the 2X / 3X / 4X menu labels
  (`tools/menu_art/fonts`)
- Inter by The Inter Project Authors (SIL Open Font License 1.1): the redrawn subtitle letters
- Liberation Serif / Sans by Red Hat and Google (SIL Open Font License 1.1): the redrawn clock digits
  (licence texts in `tools/text/licences`)
- Real-ESRGAN by Xintao Wang et al. (BSD-3-Clause): the texture super-resolution uses the weights of
  `realesr-general-x4v3` and `realesr-general-wdn-x4v3`, converted by `tools/upscale/export_weights.py`
  (`sodfix/data/sr_weights.bin`, licence in `sodfix/data/LICENSE_RealESRGAN.txt`)
- Shadow of Destiny / Shadow of Memories is © Konami. This project contains no game code. You need your own copy
  of the game.

The rest of the project is released under the zlib licence (`LICENSE`).
