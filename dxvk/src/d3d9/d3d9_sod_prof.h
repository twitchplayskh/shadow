#pragma once

// SoD PROFILER: finds what causes hitches - the original game or something Shadow adds.
//
// Every feature Shadow adds is timed per frame on the game's thread (zones below; time inside a nested zone
// counts for the inner one only), together with DXVK's own counters (new pipelines, waits for the rendering thread
// and the GPU) and SoDFix's (the game's file opens and reads, controller polling). Whatever is left of the frame is
// the game itself plus the graphics driver. A sampler thread wakes only while a frame runs long and records where
// the game's thread is (game code, Shadow, the driver, Windows; with the game function that called it), so a hitch
// inside the original game is pinned down too.
//
// Always on (SODRT_PROFILER=0 turns it off): every hitch (a frame much slower than the ones around it) is written
// to %LOCALAPPDATA%\SoDRT\hitches.log with the full breakdown. F9 (or the file %LOCALAPPDATA%\SoDRT\profile.go)
// starts / stops a recording: every frame to profile_<time>.csv, and a summary (frame time percentiles, every
// zone's average and worst, where the game's thread spends its time overall, the worst hitches) to
// profile_<time>.txt.

#include <cstdint>

namespace dxvk { class D3D9DeviceEx; }

namespace sod::prof {

  enum Zone : uint32_t {
    ZCrispText,       // CRISP TEXT: redrawing a text sheet
    ZGeomBuild,       // IMPROVED GEOMETRY: working out a street / wall draw's split
    ZGeomDraw,        // IMPROVED GEOMETRY: the converted draws
    ZUpscale,         // TEXTURE UPSCALING: a texture upload (fast upscaler, cache)
    ZSuperRes,        // TEXTURE UPSCALING: the network's frame budget, cache writes
    ZRtLink,          // RT SHADOW CASTING: per-frame hand-off to the 64-bit helper
    ZRtShadow,        // RT SHADOW CASTING: the shadow pass at the game's shadow draw
    ZAoSsr,           // AMBIENT OCCLUSION + REFLECTIONS passes
    ZFrameWork,       // per-frame upkeep of the features (reflections, relief, geometry cache, camera trace)
    ZSmoothCam,       // SMOOTH CAMERA
    ZCreate,          // the game creating textures / buffers (the driver allocating them)
    ZPresent,         // Present: handing the frame over, waiting for a free frame slot (GPU / vsync)
    ZCount
  };

  bool Enabled();
  bool TopLevel();       // no zone open: a game call (not one made by a feature)
  void Begin(Zone z);
  void End(Zone z);
  void FrameEnd(dxvk::D3D9DeviceEx* dev, uint64_t drawsHint);

  struct GameScope {        // a zone only when the game itself made the call
    Zone z; bool on;
    explicit GameScope(Zone zone) : z(zone), on(Enabled() && TopLevel()) { if (on) Begin(z); }
    ~GameScope() { if (on) End(z); }
  };

  struct Scope {
    Zone z; bool on;
    explicit Scope(Zone zone) : z(zone), on(Enabled()) { if (on) Begin(z); }
    ~Scope() { if (on) End(z); }
  };

}
