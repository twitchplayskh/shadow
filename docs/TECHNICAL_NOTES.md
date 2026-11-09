# Shadow of Destiny PC — Phase 1 technical findings (verified)

Target: `shadow.exe`, 7,057,408 bytes, PE timestamp `0x3DB6B7C3` (2002-10-23),
SHA-256 `529d30e8…e764`. Image base 0x400000, no ASLR (DllCharacteristics 0), no manifest.
Note: the file's modification time on disk is 2026-11-07. If you've patched it, the
offsets below apply to this copy.

## Rendering architecture
- Imports `d3d8.dll!Direct3DCreate8` (single call @0x7B1F4C, SDK 0x78) → **Direct3D 8**, a
  direct port of the PS2/Xbox codebase (`xbox_shaders.cpp`, `BASLUS-20146`, VU0-style matrices).
- D3D8 vertex shaders: 9 built-in (`*prefix\engine\shader_*.vsh`), 4 loaded from disk:
  `shader_shavol.vsh`, `shader_shavol_point.vsh` (shadow volumes), `shader_weather.vsh`, `shader_rain.vsh`.
  D3DX8 shader assembler is statically linked.
- **Original shadows are stencil shadow volumes** (parallel + point light variants), driven by script
  commands `SetGameShadowLight`, `SetShadowParallelLight`, `SetShadowPointLight`,
  `SetLightShadowDensity`, `SetLightShadowLength`, `SetCharaShadowSwitch`. So the game already
  carries explicit per-scene shadow light data that a ray-traced path can reuse.
- `DDRAW.dll!DirectDrawCreateEx` @0x79DFD6 / 0x7B283A: DirectDraw7 + DirectShow for FMV
  (`works\conv\video\open_e.mpg`) and display-mode enumeration (mode list @0xF46DD8, count @0xF477B4).
- No `EnumAdapterModes` calls: the D3D8 device does not validate against an adapter mode list.

## Resolution
| Item | Location |
|---|---|
| Back buffer W/H (applied) | `0xF68EF8` / `0xF68EFC` |
| Pending W/H (applied on change each frame @0x7B2440 → Reset @0x7B25C0) | `0xF68F00` / `0xF68F04` |
| Constructor default 640×480 | `0x7B1D9D`, `0x7B1DA7` |
| D3DPRESENT_PARAMETERS builder | `0x7B23B0` (struct @0xF68DD0, X8R8G8B8/R5G6B5, DISCARD, own depth surface) |
| SetResolution(w,h) — ignores args, reads globals, forces fullscreen | `0x7A5A00` |
| Menu resolution table (4 entries, all 4:3) | `0x939330`: 640×480, 800×600, 1024×768, 1280×960 |
| Selected index (byte) | `0xAB8FD9` |
| Menu mode availability check vs DDraw list | `0x7A5A40` |

The table is followed directly by other data, so it can't grow in place. Extending the menu means
redirecting the four `[idx*8+0x939330]` loads to a new table and patching the index clamp and
the label art (`works/conv/misc/option*.tbn2`, which is image-based).

## Aspect ratio
- Projection is PS2 `ViewScreenMatrix(out, scrz, ax, ay, cx, cy, zmin, zmax, near, far)` @0x79CC40.
  m00 = arg4 (x scale), m11 = arg5 (y scale).
- Main camera @0x72BB50: `scrz=[0xADD8D8], ax=1.0, ay=4/3` → Hor+ = set ax to (4/3)/aspect (imm @0x72BB6E).
- 0x75D1BB, 0x76B5C3, 0x781CA2 are off-screen render-target projections (256²/2048² centres). Left alone.
- UI/2D: positions scale by `width * 1/640` and `height * 1/480` (@0x7A923D, 0x7A928C): **stretches at 16:9**.

## Settings / persistence
- Options and save data: `works\conv\savedata\shadow.cfg`, `shadow`, `shadow2` (game-relative path).
  Program Files + no manifest → likely redirected to `%LOCALAPPDATA%\VirtualStore`.
- Registry is only read for `Software\Microsoft\DirectX` (SDK sample path), not for game settings.
- No shadow-quality option string was found in the exe. Shadows are scripted on/off per character/scene.
  Whether the options menu has a shadow toggle still has to be checked in-game (`option.tbn2` art).

## Timing
- QueryPerformanceCounter/Frequency + timeGetTime; fullscreen presents with interval ONE.
  Not yet analysed. Keep vsync and don't unlock FPS.

## Implemented in 0.1.0 (SoDFix, dinput8 proxy)
1. Ctor default 640/480 → W/H. 2. SetResolution → constant W/H. 3. Main camera Hor+.
4. (opt-in, untested) windowed flag. All patches verify original bytes and are restored on unload.

## Next
- Phase 2/3 test on hardware with DXVK ≥ 2.4 (d3d8 + d3d9).
- UI pillarbox: hook the 2D transform near 0x7A91E0 to use (h*4/3)/640 and an x offset.
- Ray tracing: the game submits skinned geometry through vertex shaders (Remix needs fixed-function
  or captured post-VS geometry), so RTX Remix likely won't capture characters as-is. A custom
  DXVK-side capture driven by the existing `SetShadow*Light` data is the probable path. Prototype first.

---
# 0.2.0 additions (verified statically)

## GRAPHIC SETTING menu internals
- Item range table `0x939408` (6 bytes: min,max,id): RESOLUTION {0,3,12}, DRAW DISTANCE {0,2,13},
  FOCUS/BLUR {0,1,14}, SHADOW CASTING {0,2,15}. Values live at `[menu+0x54+id*4]`.
- Stepper `0x776910(cur,min,max,right,left,flags)`. Invalid-mode skip loop @0x7170E7 wraps at `cmp eax,3` (0x7170F6).
- Sprite rect table `0x939450`, 12 bytes `{u16 tex, u16 pal, s16 u, v, w, h}`. Resolution labels are
  `0x55+value`, written by `lea edi,[eax*4+0x939450]` @0x7161AA (the only +0x55 user).
- Settings blob: +0x9E99 res, +0x9E9A draw dist (`0xAB8FDA`), +0x9E9B focus (`0xAB8FDB`), +0x9E9C shadow (`0xAB8FDC`).

## SHADOW CASTING (`0xAB8FDC`, getter 0x716070)
- 0 = MULTIPLE, 1 = ON (light count clamped to 1 @0x748F04), 2 = OFF (shadow pass 0x748D80 skipped @0x744BE8).
- Forced to 2 at load if the stencil capability `[0xF68CF8]` is 0.
- RT plan: menu value 0 = RAY TRACED; shadow code sees 2 (stencil volumes off) through a stub on the two
  consumer calls; the RT renderer reads the raw byte. The help line "Multiple, single or no shadows"
  is in option_e.tbn2 image 7 and is not changed yet.

## option_e.tbn2 format
record = 0x40 header (`+0` image id, `+0x10` magic 34 12 34 12, `+0x28` u16 w, `+0x2A` u16 h, `+0x2C` bpp)
| pixels | 0x20 palette header (`+0` palette id) | BGRA palette. Labels: image 8 (id 887, 512x512 4bpp, pal 13991).
New labels were composed from the game's own glyphs into free atlas space:
1440x1080 (150,226,111,32), 1920x1440 (266,226,108,32), 2880x2160 (382,226,104,32), RAY TRACED (248,478,156,32).

## 4:3 presentation
Main window class "SoM", created by 0x7B25C0 (single caller 0x7B2476) as a WS_POPUP sized to the back buffer;
re-sized via SetWindowPos @0x7B2797 on mode change. 0.2 forces windowed D3D8, hooks CreateWindowExA/SetWindowPos
to a borderless screen-height 4:3 rect owned by a black backdrop window, and makes the process DPI-aware.

---
# 0.3.0: SCREEN MODE row (verified statically)
- Page 3 items: count `mov edi,4` @0x716FC1 → 5, table @0x716FC6 → {res, mode(id 16), draw, focus, shadow}.
- Menu object (handle 0x24) allocated 0x94 bytes @0x716631 → 0x98; value slot id 16 = obj+0x94.
  0x717210 (apply-per-id) ignores id>15, the generic stepper handles the new item.
- Row label sprites per page: `0x939B00[page]` (page 3 → [97,98,99,100,117]); help rect idx per row: `0x939ABC[page]`.
  Cursor bar (sprite 1) follows the row label sprite's y. Help sprite 118 rect pointer is overridden for the new row.
- Sprite pool (handle 0x25, 0x60-byte sprites) size 120: alloc `push 0x78` @0x71675E, draw `push 0x78` @0x716930 → 124.
  New sprites 120/121 (title + drop shadow), 122/123 (value + shadow) copy an unselected template each frame
  (98/94 or 99/95, and 103/104) with their own rect and a row offset, keeping slide/fade animations in sync.
- Sprite fields: +0x10/+0x14 pos (<<4), +0x20/+0x24 target, +0x30/+0x34 start, +0x40/+0x44/+0x54 selection,
  +0x48 rect ptr, +0x50 flags (bit0 draw), +0x56 alpha, +0x58 anim type, +0x5C anim counter.
- Layout table 0x9398B0 entries 48–58 moved one row (pitch 48); EXIT 312 → 344.
- Hooks: menu init call @0x71663D, per-frame draw call @0x716932 (esi = menu), apply-on-exit call @0x7165B9.
- Mode switch forces `[0xF68EF8]=0` so 0x7B2440 re-runs window + device creation; the create stub sets
  `[0xF68E0C]` (windowed) and the CreateWindowExA/SetWindowPos hooks set style, owner and rect.
- New art in image 7 (tex 819/pal 13226): SCREEN MODE (234,326,174,24), WINDOWED (234,352,151,23),
  BORDERLESS (274,234,153,24), FULLSCREEN (274,262,150,24), help "Change screen mode" (330,93,155,19).

## 0.3.1: fullscreen fix
- DXVK (src/d3d9/d3d9_swapchain.cpp, 829e377): exclusive fullscreen = ChangeDisplayMode to exactly
  BackBufferWidth x Height; failure -> D3DERR_INVALIDCALL from Reset -> game crash. Present source/dest
  rects are ignored in fullscreen and force a GDI StretchBlt fallback in windowed mode, so a 4:3 image
  inside a native-res exclusive mode is not viable through DXVK's present path.
- FULLSCREEN therefore only allows real display modes (0x7A5A40). cb_ValidateRes uses the menu's pending mode,
  remaps the startup check (return address 0x70E7A7) to the largest valid mode, and the menu/apply hooks move an
  unavailable resolution to the largest valid one. WINDOWED removed from the menu (value range {0,1}).

## 0.4.0: one monitor-sized window, 4:3 via Present dest rect
- DXVK swapchain (d3d9_swapchain.h SwapWithFrontBuffer): DISCARD + 1 back buffer + windowed uses a GDI
  fallback for partial presents; any other combo uses the Vulkan blitter, which honours pDestRect in windowed
  mode and clears the rest of the swap image (dxvk_swapchain_blitter.cpp: LOAD_OP_CLEAR when dstRect != extent).
- Present params builder call @0x7B27B0 wrapped: BackBufferCount (0xF68DDC) = 2.
- Present call sites 0x7B29DA (post-reset clears) and 0x7B31CD (frame) rewritten to call hk_Present, which passes
  a centred 4:3 destination rect computed from the window client size.
- D3D is always created windowed; the window is WS_POPUP covering the primary monitor; FULLSCREEN = HWND_TOPMOST.
  The backdrop window is gone. Mode changes only change z-order (no device reset).

## 0.5.0
- FMV (open_e.mpg: MPEG-1 512x408, square pixels, full-frame artwork, no baked bars) is played by 0x79DB90
  through DirectDraw7 + amstream: per frame dest RECT {0,0,640,480} @ [esp+38h], Blt to back buffer [esp+20h],
  Flip. 0x79DCE7..0x79DCFA replaced by a call that sizes the rect to a centred 4:3 area of the target surface
  (GetSurfaceDesc) and colour-fills the surface black first.
- SCREEN MODE value 0 = WINDOWED: monitor-sized borderless window, HWND_TOPMOST only while foreground
  (checked every Present); FULLSCREEN = always topmost.

## 0.5.2: videos
- The DirectDraw/amstream player at 0x79D9F0..0x79E240 is dead code (its entry points 0x79DA10/0x79DA20 are
  `mov eax,1; ret` stubs); the 0.5.0 patch there had no effect and was removed.
- Videos play through DirectShow at 0x7A8250(filename): CLSID_FilterGraph, RenderFile, IVideoWindow put_Owner(main hwnd),
  put_WindowStyle(WS_CHILD), GetClientRect(main) -> SetWindowPosition(0,0,w,h) @0x7A8324 (vtable +0x9C).
  That call now goes to hk_VideoSetWindowPosition, which centres a 4:3 rect in the client area.

## 0.6.0: RAY TRACING row + scrolling
- Environment geometry is drawn fixed-function with FVF 0x142 (XYZ | DIFFUSE | TEX1, no normals): its lighting and
  shadows are baked into vertex colours (no lightmaps). Models use the skinned vertex shaders (decl 0xAAD878) with normals.
- Original shadows: per shadow light, Clear(STENCIL) @0x7A1AB0, volumes via shader_shavol(_point).vsh
  (c0-3 viewproj, c5 light dir*len or pos+len, c8-95 bones), then 0x7A1AD0(density) draws a full-screen black quad
  with alpha = density through the stencil. SHADOW CASTING 0 = all lights, 1 = first light only, 2 = pass skipped.
- Page 3 now has 6 items (+EXIT): RES, SCREEN MODE (id 16, obj+0x94), DRAW, FOCUS, SHADOW, RAY TRACING (id 17, obj+0x98).
  Menu object 0x9C bytes. Sprite pool 130 (alloc/draw `push 0x78` sites replaced by stubs pushing 130).
- Sprites: 124/125 RAY TRACING title + shadow, 126/127 value + shadow, 128/129 scroll track/thumb (texture-0 quads).
  Shadow-casting row sprites: 100/96 label, 110-112 values, 113-115 shadows, 116 underline; focus underline 109.
- Rows at 104 + 48*n, EXIT at 392; 6 visible rows. Scrolling shifts pos/target/start (+0x14/+0x24/+0x34) of the
  page's sprites and the cursor bar (sprite 1) and clears draw flag bit0 on rows outside the window.
- Interlock in the per-frame hook: RT>0 forces SHADOW CASTING value 2 (0x716060(2) + 0x7160E0 refresh);
  SHADOW CASTING set to 0/1 while RT>0 resets RT to 0. RT level persisted in SoDFix.ini [Shadows] RayTracing.
- New art (image 7): RAY TRACING (234,352,164,23), ULTRA (410,326,85,23), help "Ray traced shadows" (272,286,142,20);
  WINDOWED art removed.

---
# SoD-RT (ray-traced shadows) — DXVK fork, based on DXVK v3.1.1
Repo: `sod-dxvk` branch `sod-rt` (patches in `patches/`). Builds with mingw-w64 i686 + meson (`build-win32.txt`).

## Design (replaces only the stencil test; keeps everything SHADOW CASTING did)
Per shadow light pass the game issues: Clear(STENCIL) -> shadow-volume draws (tagged VS) -> darken quad (alpha = density).
- Casters: the shadow-volume draws themselves. Running the VS math with extrusion = 0 gives the exact skinned caster
  mesh in world space (volume edge quads collapse to zero area). Same casters as the original, per-character switches included.
- Light: c5 (parallel: dir*length; point: pos.xyz + length.w) and c6 (point: dir) of the volume draws.
- Camera: c0..c3 = view*proj -> world position per pixel from the depth buffer.
- Density / blending: the game's darken quad (and its blend state) is kept; only the stencil test is replaced by a
  ray-traced visibility mask, so darkness, MULTIPLE vs ON light count and scripted shadow lights all stay identical.
- SoDFix: with RAY TRACING > 0 the menu shows SHADOW CASTING OFF but the renderer-side getter returns MULTIPLE
  (0x744BE8 / 0x748F04), so every light pass still runs; SoDFix exports `SoDFix_GetRayTracingLevel`.

## Milestones
- M1 (done, needs in-game log check): capture + validation. d3d8 tags shader_shavol*.vsh by declaration
  {STREAM0, v0-v4 FLOAT4, v5 FLOAT1} (+ rsq => point light); d3d9 records passes in Clear/Draw*/DrawPrimitiveUP/PresentEx
  and logs to the DXVK d3d9 log ("SoD-RT:" lines) incl. GPU ray-query support.
- M2: enable VK_KHR_acceleration_structure + ray_query + buffer device address in DXVK device creation (only when
  supported, RT stays optional); compute pass skins caster vertices (extrusion 0) into a per-frame BLAS; TLAS per light pass.
- M3: at the darken quad, bind depth (read-only) + TLAS, fragment shader traces one shadow ray per pixel toward the
  light, discards lit pixels, outputs the original quad colour/alpha -> hard RT shadows identical in darkness (MEDIUM).
- M4: soft shadows (light radius, blue-noise jitter, temporal accumulation / denoise) for HIGH/ULTRA; optional world
  geometry (FVF 0x142 draws) as occluders only, so shadows stop at walls without double-darkening baked lighting.

## Finding (0.7.0 log): no hardware RT in-process
RTX 4070 / NVIDIA 621.1.0 reports acceleration_structure=0 ray_query=0 inside 32-bit shadow.exe: the 32-bit Vulkan
driver does not expose ray tracing. Hardware RT therefore runs in a 64-bit helper (bridge); a compute-shader tracer
in-process remains the fallback. External memory/semaphore win32 extensions are available in-process (needed to share).

## Bridge B1 (0.7.1)
- `sod_rt64.exe` (x86_64 mingw, static, loads vulkan-1.dll at runtime). Started by d3d9 SoD-RT OnInit, suspended ->
  job object (KILL_ON_JOB_CLOSE) -> resumed; exits when shadow.exe exits.
- Shared block `Local\SoDRT_Bridge_<pid>` (sod_bridge_proto.h), ready event `Local\SoDRT_Ready_<pid>`.
  32-bit writes DXVK's deviceUUID/LUID; helper matches the GPU, creates a device with acceleration_structure +
  ray_query (+ external memory/semaphore), builds a 1-triangle BLAS/TLAS and runs 2 ray queries (hit + miss).
- Helper log: %LOCALAPPDATA%\SoDRT\sod_rt64.log; summary in shadow_d3d9.log.
- Shadow-volume diagnostics (M1.1): every CreateVertexShader's real D3D8 declaration is logged, every draw inside
  a shadow pass is logged; stencil-only VS draws in a pass count as casters even if untagged.
Next: B2 = shared images (depth in, shadow mask out) + timeline semaphore handshake per frame.

## Finding (0.7.1 logs)
- Bridge B1 PASSED on RTX 4070: same GPU, acceleration_structure + ray_query in 64-bit, external memory/semaphore +
  timeline available; BLAS+TLAS build + 2 ray queries 0.91 ms.
- shader_shavol*.vsh are created (decl matches, tagged) but NOT used for drawing on PC. Shadow volumes are drawn
  fixed-function: FVF 0x1D2 (XYZ|NORMAL|DIFFUSE|SPECULAR|TEX1) triangle strips, two passes per caster
  (STENCILPASS DECR(8) then INCR(7)), i.e. CPU-built volumes, z-pass. Typical: 1989 / 873 / 1833 / 585 tris.
  Light direction and caster mesh are therefore in the CPU-written vertex data + fixed-function transforms,
  not in shader constants.
- M1.2: one-frame capture of every draw (rt_analysis/parse_capture.py) to work out volume construction and
  how to recover casters + light from it.

## SoD-RT capture 1 findings (frame 980, 1280x960, two characters)
- Shadow volumes are world-space (WORLD = identity, real VIEW/PROJ), FVF 0x1D2, stride 40, 16-bit indexed strips.
- Topology is fixed per mesh (index buffer constant); vertices come in pairs (near, near + L*1500).
  Non-silhouette edges are written as (0,0,0) pairs -> degenerate. Only position is meaningful; other fields are stale.
- Extrusion is constant per draw -> parallel light. Two lights this frame: dir (0.2854, 0.7957, -0.5342) density 49/255,
  dir (0.2854, 0.7957, +0.5342) density 41/255 (world Y points down; light from above).
- Z-pass stencil: cull CW + DECR, cull CCW + INCR, no caps. Darken quad: XYZRHW 0x44, SRCALPHA/INVSRCALPHA, stencil test.
- First light pass is not preceded by Clear(STENCIL) alone (frame-start clear), so it logs as pass -1.
- The frame between two Presents had only 19 draws (volumes, 2 darken quads, one 0x144 full-screen textured quad at z 0.999,
  UV 0..0.625 x 0..0.9375 = 1280x960 inside a 2048x1024 texture). No scene draws -> capture v2 (SODCAP02) records 4 frames
  with clears, render targets, scenes, copies and the Present call site (SoDFix_GetPresentSite, return address 0x7B29E2 / 0x7B31D5).
- Capture v2 #2 (frames 5323-5326): every frame = Clear(T|Z|S, black, z 0) -> BeginScene -> volumes -> quad
  [-> Clear(S) -> volumes -> quad] -> 0x144 quad (no texture) -> EndScene -> Present @0x7B31D5. 11 draws, no RT switches.
  Z cleared to 0 means no volume pixel passes LESSEQUAL: a black frame. Both captures fired right after "light setup changed"
  = room change / fade-in. 0.7.4 arms only when the frame has >= shadow draws + 40 draw calls and logs draws per frame.

## Frame structure (capture2 #3, frame 1160, 304 draws) - verified offline (rt_analysis/offline_ref.py)
1. Clear(T|Z|S, black, Z=0). Depth is REVERSED: proj z = -0.00131 z + 64.08, w = z  ->  depth = 64.08/z - 0.00131;
   room draws use ZFUNC GREATEREQUAL. View-space z from depth: z = 64.08 / (depth + 0.00131).
2. Room: ~34 x FVF 0x142 (XYZ|DIFFUSE|TEX1), world = identity, baked vertex colour, opaque, zwrite.
3. Shadows: per darken quad, the volumes drawn since the last Clear(STENCIL); volumes ZFUNC LESSEQUAL (= behind the
   surface on reversed Z), cull CW + DECR, cull CCW + INCR; quad shades where stencil != 0. Shadows land on the ROOM ONLY.
4. Characters: FVF 0x1D2, CPU-skinned, already in world space (world = identity), 16-bit indexed strips, one dynamic VB per
   character (~6k tris for two characters). Some parts drawn twice (second pass = 32x32 overlay, alpha blended).
5. After: 156 x DrawPrimitive 0x142 alpha-blended (effects), 37 x 0x144 XYZRHW (2D/UI).
- Casters for a light = character meshes whose bounds overlap a volume's caster outline (near points).
- Offline check: ray from the depth-reconstructed surface point towards the light (-extrusion) against those meshes vs the
  exact stencil emulation: 99.8 % of pixels agree (differences = 1 px silhouette edges).

## Bridge B2a (0.8.0-b2a): per-frame GPU link (d3d9_sod_link.cpp, sod_rt64.cpp Serve())
- Timeline semaphore (OPAQUE_WIN32 NT handle, DuplicateHandle into the helper). Request k: DXVK copies the game's depth
  (as float32, via DXVK's compute depth->buffer copy) into a shared buffer, signals 2k+1, flushes; next submission waits
  2k+2. Helper serves every request in order (ring of 8 slots), signals 2k+2.
- Watchdog (20 ms): if the semaphore sits on an odd value > 1 s, or the helper process exits, the link is disabled and
  the CPU signals 2k+2 so the game's queue never hangs.
- DXVK patch: per-command-list semaphore wait/signal stage masks TOP/BOTTOM_OF_PIPE -> ALL_COMMANDS.
- Test kernel: marks drawn pixels (depth != 0) in the result buffer and counts them; DXVK reads the result back every
  300 frames and compares with the helper's count. SODRT_LINK=0 env var disables the link.
- Planned apply point (B2c): defer the darken until after the character draws (FVF 0x1D2), mark character pixels in
  stencil, skip the game's volumes/quads while RT is on.

## Bridge B2b (0.8.2): ray-traced mask in the helper (not applied to the image yet)
- B2a result on the RTX 4070: ~4000 round trips, 5 resolution changes (640x480 / 1280x960 / 2880x2160), helper count ==
  readback count every time (6.2 M pixels at 2880x2160). "0 of N" checks = black loading frames.
- Per frame (d3d9 SodLinkObserveDraw): FVF 0x1D2 + stencil + STENCILPASS DECR = volume -> dir + caster-outline bounds;
  Clear(STENCIL) resets; darken quad (UP, 0x44) closes a light group; opaque FVF 0x1D2 without stencil = character part
  (deduped, grouped per vertex buffer; alpha-blended parts/overlays skipped - the game builds no volume for them).
  Trigger = first other draw after a darken quad (depth = room + characters). No casters -> no request.
- Geometry per request in "Local\SoDRT_Geom_<pid>" (8 x 1 MB): SodGeomFrame + tris (max 29092).
- Helper: BLAS with one non-opaque geometry per caster, TLAS 1 instance; rtmask.comp: world pos from reversed depth
  via inverse(view*proj) (row vectors), 1 ray per light towards it (bias 1 unit, tmin 0.5), candidate confirmed only if
  the caster's light mask has the light. Result byte = bit per light.
- Offline: make_dump.py (capture -> SDMK), rt_harness (lavapipe), check_mask.py. Kernel == CPU ray trace (0 px diff);
  vs the game's stencil on room pixels 91% IoU, differences = 1 px outline (volumes use a simplified silhouette).
- Verification dump during a frame capture: %LOCALAPPDATA%\SoDRT\sod_rt_mask_<frame>.bin (header, geometry, depth f32, mask u8).

## B2b result on the RTX 4070 (0.8.2) and B2c (0.9.0)
- 0.8.2: 1581 frames traced, 8/8 checks OK. Dump frame 2635 (1280x960): GPU mask vs CPU ray trace 1 px of 32917 differs;
  vs the game's stencil (capture of the same frame) 91.4 % IoU (1 px outline). Helper GPU time was 2.3-5.3 ms -> 0.9.0
  uses one OPAQUE BLAS per light + TLAS instance masks (no shader candidate loop), video-memory triangles, stage timestamps.
- Result byte is now the light factor: 255 = lit, else 255 * prod(1 - density) (geom flags bit 0).
- B2c apply: volumes + darken quads skipped while linked (observed, not drawn); opaque and alpha character parts drawn
  with stencil REPLACE 0x80 (mask 0x80); at the trigger, after the wait: result buffer -> L8 texture (copyBufferToImage),
  XYZRHW|TEX1 quad, SRCBLEND ZERO / DESTBLEND SRCCOLOR, stencil EQUAL 0 under mask 0x80, state block ALL save/restore.
  No apply at Present (UI already drawn). Mask texture: private ref, released at the start of Reset (D3DPOOL_DEFAULT
  resources would make Reset fail).
- Known gap: volumes of characters that are not drawn this frame (off-screen) cast nothing while RT is on.

## 0.9.0 on the RTX 4070: works
- RT shadows on screen in place of the stencil ones (user screenshots on/off: same placement, length and density, no
  artefacts on characters). 6/6 readback checks OK, no watchdog trips, link survived resets and RT toggles.
- Helper GPU time per frame swings 0.4 ms <-> 4 ms, dominated by "build" (staging copy + BLAS/TLAS for ~6k tris) -
  far above the real cost, likely inter-process GPU time-slicing. 0.9.1: HIGH global queue priority (if granted),
  PREFER_FAST_BUILD for the per-frame BLAS.

## Performance (0.9.1 -> 0.9.2)
- User: 4K (2880x2160) RT max 17 fps vs 29-30 fps without (game itself runs at 30). Helper GPU time ~3.5 ms cannot
  explain 25 ms/frame. HIGH queue priority refused (VK_ERROR_NOT_PERMITTED_KHR) - expected for a normal process.
- Prime suspect: DXVK puts the game's WRITEONLY dynamic VBs/IBs in DEVICE_LOCAL|HOST_VISIBLE (ReBAR) memory; the RT path
  reads the skinned characters + volumes back on the CPU every frame (uncached PCIe reads). 0.9.2: shadow.exe app profile
  d3d9.cachedWriteOnlyBuffers=True; de-dupe character parts before reading.
- Helper: build (upload + BLAS/TLAS) in its own submission without wait -> overlaps the game's frame; trace waits.
- Exact early-out per light (casters' u/v bounds along L + max along L): test frame 2.46 M -> 0.14 M rays, identical mask.
- Profile line every 10 s ('SoD-RT: profile:'): frame ms/fps, in-Present ms, CPU fetch/trigger/shadow-pass ms, post->reach,
  game GPU stall (2k+1 -> 2k+2, helper monitor thread with vkWaitSemaphores), helper submit lag, trace/build GPU ms.

## 0.8.0 (SoD-RT 0.10.0): RT SHADOW CASTING rename + ANTI-ALIASING row
- RAY TRACING title art replaced by "RT SHADOW CASTING" (image 7 at 234,352,252,24; game item-font glyphs).
- New row 6 ANTI-ALIASING (id 18, obj+0x9C): items 4->7, menu object 0x94->0xA0, sprite pool 120->134
  (130/131 title + shadow, 132/133 value + shadow), EXIT layout entry 51 y 312->440, 8 page rows / 6 visible.
- Art (image 7): ANTI-ALIASING (4,113,173,24, hyphen synthesised from the H crossbar), SMAA (300,376,70,23),
  help "Smooth jagged edges" (4,168,168,20, Liberation Serif 19 px quantised to the palette). OFF from tex 887.
- Persisted in SoDFix.ini [Display] AntiAliasing; exported as SoDFix_GetAntiAliasing().
- DXVK (dxvk_swapchain_blitter): SMAA 1x ULTRA (iryoku reference, MIT) runs on the back buffer before the
  4:3 blit: luma edges (RG8) -> blending weights (RGBA8, AreaTex/SearchTex D3D10 layout) -> neighbourhood
  blending into a copy in the back buffer's format. d3d9 PresentEx sets it from the SoDFix getter each
  frame; env SODRT_SMAA=0/1 overrides. TAA/DLSS-style temporal AA is not used: the engine has no motion vectors.

## 0.8.1 (SoD-RT 0.10.1): TEXTURE FILTERING row, menu art fixes
- Image 7 extended 512x400 -> 512x512 (record +0x2A h, +0x34 size): the record already declared a 512x512
  texture (+0x04..0x0B), so the texture and every existing rect's UVs are unchanged; rows 400..511 hold the
  new art. make_menu_art_v2.py asserts no new art overlaps any game rect (0.10.0 had ANTI-ALIASING / help art
  inside rects 0x40/0x4D/0x4E: the help line showed a second time above "UP and DOWN").
- Also fixed: 1440 x 1080 label overlapped rect 0x22 (moved to 392,192 in tex 887); "Ray traced shadows" help
  rect trimmed to 18 rows (clear of rect 0x4F).
- Help sprite 118: pivot (+0x40/+0x44) set from our rect (mode 4 = size/2) so the replaced line is centred.
- Page 3: 9 rows, 5 visible (a 6th visible row at 344 put the highlight bar over the help panel), EXIT 488,
  item count 8, menu object 0xA4 (TEXTURE FILTERING id 19 = obj+0xA0), sprite pool 138 (134..137).
- [Display] TextureFiltering 0/1/2, export SoDFix_GetTextureFiltering.
- DXVK d3d9: with SoDFix present, single-level non-RT/DS/dynamic MANAGED/DEFAULT textures in formats that
  support it get D3DUSAGE_AUTOGENMIPMAP (game still sees 1 level). BindSampler: STANDARD -> lod 0..0.25 nearest
  mip (base level, as before); TRILINEAR -> linear mips; ANISOTROPIC -> linear mips + 16x. Changing the setting
  marks every sampler dirty.

## 0.8.2 (SoD-RT 0.10.2)
- Page 3 rows outside the 5-row window are parked 8192 px below the screen (pos/target/start) instead of
  clearing the draw flag: the sprite update re-enables drawing while a page slides in (rows flashed over the
  help panel for ~8 frames). Scroll offset is read back from sprite 97's target y each frame.
- Pre-cutscene flicker: while the game loads a cutscene it keeps calling Present without drawing. With
  BackBufferCount 2 (0.4.0) DXVK rotates 3 buffers, one holding the HUD-less frame before the cut -> every
  third frame flashed. d3d9 PresentEx now skips a Present when nothing was drawn/cleared/copied/locked into
  the back buffer since the last one (log: "SoD: present with nothing drawn skipped"; SODRT_KEEP_EMPTY_PRESENTS=1
  disables). Game capture routine 0x7B34A0 (D3DX copy of back buffer 0 into a 512x512 texture, or the
  focus/blur RT pair 0xF68F50/54 when 0xF68F18) is unrelated: one-shot requests via 0x7A5F60.
- F12 = 4-frame SODCAP02 capture on demand (for the cutscene letterbox: 30 px bars at 480, picture 640x420).

## 0.8.3 (SoD-RT 0.10.3)
- 0.8.2's parking (target y +0x24) and scroll read-back were wrong (page drawn ~5 rows low, rows not hidden):
  reverted to 0.8.1 scrolling. Out-of-window rows now get an empty rect (+0x48 -> {0}); their own rect is
  remembered and restored when they scroll in. Draw flags are left to the game.
- Empty-present skip only fired once in a session (frame 13319, at a mode switch): the pre-cutscene frames
  are drawn. F11 = per-frame summary log for 300 presents (draws, scene viewport, 2D draws, clears, RT sets,
  copies, present rects) to find what the game draws in them.

## 0.9.0 (SoD-RT 0.11.0): 1 back buffer again, CINEMATIC BARS
- Original game has no pre-cutscene flicker (user test without the mod). Removed the BackBufferCount=2 present-
  params patch (0.4.0): with 3 rotating buffers, presents of frames not drawn into the back buffer showed a stale
  buffer. DXVK d3d9 swapchain: with SoDFix present, a dest-rect present with 1 back buffer uses the Vulkan
  blitter instead of the GDI fallback (D3D8 windowed semantics: the image persists, no rotation). The empty-
  present skip (0.10.2) is gone.
- Cutscene letterbox (F12 capture, frame 5567): full viewport scene, then DrawPrimitive FVF 0x144 untextured
  strips colour 0xFE000000 over y 0..H/16 and 15H/16..H. CINEMATIC BARS OFF skips exactly those (8 of 1108 draws
  in the capture match, 2 per frame). Row 8 (id 20 = obj+0xA4, object 0xA8, sprites 138..141, pool 142, EXIT 536),
  values ON/OFF = game rects 0x0A/0x0B, [Display] CinematicBars, export SoDFix_GetCinematicBars.

## 0.9.1: options pages generalised, CINEMATIC BARS in DISPLAY SETTING
- Page ids (obj+0x30): 1 SOUND (3 items @0x939420, ids 2-4), 2 DISPLAY (4 @0x939434, ids 6-9), 3 GRAPHIC
  (4 @0x939408, ids 12-15); item count/table immediates @0x716FCD/0x716FD2 (page 2), 0x716FC1/0x716FC6 (page 3).
  Row label ptrs 0x939B00[page], help 0x939ABC[page]. Page sprite blocks (0x7175AB switch): 53..89 DISPLAY,
  90..117 GRAPHIC. Label/shadow pairs table 0x939360 (40 pairs, shadows = rect copy, pos +2 px at init).
- DISPLAY rows: TEXT DISPLAY 73/57 value 74/58 (id 6, rect 0x17+v); COMPASS 75/59, 76/60, 77/61, underline 78
  (id 7); ITEM EMPHASIS 79/62, 80/63, 81/64, 82 (id 8); DOOR EMPHASIS 83/65, 84 (underline, x 0x12E+40v),
  85..88, 88/66 (id 9); EXIT 89/67 (layout entry 45, y 312 -> 344).
- PageDef per page (rows' sprites, label/value templates, scroll-bar template + sprites, scroll); new rows are
  copies of their own page's templates. CINEMATIC BARS = page 2 row 4 (sprites 138..141, id 20), saved as soon as
  it changes. Page-2 scroll bar sprites 142/143 (pool 144).

## 0.9.2: DOOR EMPHASIS scroll fix, CONTROLLER SETTING > LAYOUT, MODERN keyboard layout
- The value refresh 0x7160E0 (run by 0x716A90 on every cursor move) sets DOOR EMPHASIS's underline (sprite 84)
  to pos y 0x114 absolutely (0x716317 -> 0x776B10, pos only), so with the page scrolled it jumped back to its
  unscrolled row. PageScroll now tracks each game sprite's pos y and re-applies the scroll offset when a sprite
  sits at exactly (where we left it + scroll * 48), i.e. the game re-placed it unscrolled.
- CONTROLLER SETTING (page 0, default branch of 0x716FA0): count `mov edi,1` @0x716FB4 (imm 0x716FB5), table
  0x939400 {0,1,id 0} (imm 0x716FBA), rows {23,26} @0x939AD8 (ptr 0x939B00), help {63,62} @0x939A94 (ptr
  0x939ABC). All three are read every frame, so SoDFix writes count 2 / rows {23,144,26} / help {63,63,62} while
  TYPE (obj+0x54) is 0 = KEYBOARD and the originals for JOYPAD.
- TYPE sprites: label 23 (shadow 15), KEYBOARD 24 (16) x 272, JOYPAD 25 (17) x 408, underline 18 (0x716450:
  selected value via 0x716350(spr,1), underline at value pos + (-2, +28), rect 0x29/0x2A), EXIT 26 (19) layout
  entry 17 y 152, pictures 21 (keyboard) / 22 (joypad) faded by TYPE at 0x717126. Controller type global: byte
  0xAB8F88 (0x715F40), applied on confirm (0x7165C4).
- LAYOUT row: id 21 (obj+0xA8, object 0xAC), sprites 144..150 (pool 151): title copied from 23, values styled
  from 24 (selected) / 25 (dim), CLASSIC at x 272, MODERN at x 388; own untextured underline. EXIT 26/19 is
  shifted +48 while the row is shown. Art in image 7 rows 448/400/472 (menu_art.h, generated). Saved on confirm,
  [Controls] KeyboardLayout.
- Keyboard: one poll per frame, GetDeviceState(256, 0xF63D28) @0x7AE1AA ([0xF62684] = keyboard device); every key
  test reads 0xF63D28 (0x7AE480). Patched to call hk_KeyboardState, which with MODERN + TYPE KEYBOARD clears the
  new keys and ORs them into the original ones: WASD -> arrows, E/Space -> Enter, Q -> Backspace, Tab -> Right
  Ctrl, M -> Right Shift, Z/C -> Delete/Page Down, V -> End.

## 0.10.0 (SoD-RT 0.12.0): AMBIENT OCCLUSION (GTAO)
- Frame (captures): room = fixed-function FVF 0x142 (prelit, no normals), shadow volumes / darken quads (stencil),
  characters FVF 0x1D2 (some parts alpha-blended with depth write), then see-through effects (z write off) and 2D
  (XYZRHW). Depth D24S8, cleared to 0: the projection is reversed, d = P10 + P14 / z (P = 2.28125, -3.0417, -0.0013,
  64.08 in the captured room: near ~64, a person ~1000 units tall), P11 = 1.
- d3d9 (d3d9_sod_ao.cpp, shaders/sod_gtao.comp): every fixed-function draw that writes depth (no stencil, not RHW)
  records projection, viewport and linear fog; the first later draw without depth write or in 2D gets the effect,
  once per frame: depth -> float buffer, GTAO at half resolution (LOW 2 slices x 4 steps, HIGH 3 x 6; radius 250,
  4x4 interleaved noise), depth-aware 4x4 resolve at full resolution, fog fade, strength (LOW 0.85, HIGH 1.0) ->
  1 byte per pixel -> the same L8 multiply quad as the RT shadow pass (no character exclusion).
  Env: SODRT_AO (0/1/2 overrides the menu), SODRT_AO_RADIUS, SODRT_AO_STRENGTH.
- Menu: GRAPHIC SETTING row 8 (id 22 = obj+0xAC, object 0xB0, sprites 154..157, pool 158, EXIT 536), values
  OFF / LOW / HIGH = game rects 0x0B / 0x59 / 0x5B. Art: title + help in image 7 row 448 / 492; CLASSIC and MODERN
  moved to image 8 (420,440) and (366,488) to make room. [Display] AmbientOcclusion, export SoDFix_GetAmbientOcclusion.
- Offline check: lavapipe harness on the 2635 depth dump (gtao_test/), result reviewed against the CPU room render.

# 0.12.1 (package 0.13.2): DRAW DISTANCE EXTRA
- 0x715FF0 (cull radius for outdoor cells, single caller 0x790D72): LOW = far/3, MIDDLE = 2*far/3, HIGH = far,
  far = `[0x11051AC]`, set per area by 0x78C570 (also sets the dirty byte 0x1105512). The same value is the
  projection's far plane (P14 / -P10 = 49029 with near 64), so a larger cull radius alone would still be clipped.
- EXTRA (value 3, menu range {0,3,13}): SoDFix keeps the area's own far distance and writes 2x it to 0x11051AC;
  hooks: 0x78C570 (area far), 0x715FD0 (draw distance setter: re-applies), 0x715FF0 (radius; 3 -> far).
- The value label is the game's rect 0x59 + value; for 3 the per-frame hook points sprites 103/104 at kArt_EXTRA
  (image 10, item font, full 32-row cell like the game's LOW/MIDDLE/HIGH).
- Fog is the game's own vertex fog (FOGVERTEXMODE LINEAR, start/end per area from 0x1105170/74 via 0x78C450).
- Outdoor cell culling 0x790B20: per player cell, a visibility set (cell rectangle `[ebx]..[ebx+3]` + bitmask at
  `[esp+0x34]`) limits the drawn cells; byte 0x1103AA4 (or no set for the cell) takes 0x791640 instead: every cell
  within far distance, frustum-tested by 0x791050. EXTRA routes through 0x791640 (stub at 0x790CA7), which removes
  the pop-in of distant walls when crossing cell borders.

# 0.12.1: alt-tab
- At window creation the game installs WH_KEYBOARD_LL (0x7B24C0 / 0x7B2540: eats Alt+Tab, Alt+Esc, Ctrl+Esc),
  RegisterHotKey(MOD_ALT, VK_TAB) and SPI_SCREENSAVERRUNNING=1; all three calls (and the SPI reset at exit) are
  replaced by "add esp,16; xor eax,eax". Topmost only while focused in both screen modes; the DirectInput
  keyboard state is cleared while the game window is not in the foreground.
