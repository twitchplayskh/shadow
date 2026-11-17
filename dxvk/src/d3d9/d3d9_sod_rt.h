#pragma once

// SoD-RT: Shadow of Destiny ray-traced shadow layer (DXVK fork).
//
// Milestone 1 (this file): capture + validation only, no rendering change.
// Observes the game's original stencil shadow pass and records, per frame:
//   - each shadow light pass:  Clear(STENCIL) -> shadow-volume draws -> darken quad
//   - the light (shader_shavol.vsh c5 = dir*length, shader_shavol_point.vsh c5 = pos,length / c6 = dir)
//   - every caster draw (skinned caster mesh, bone palette c8..c95, view*proj c0..c3)
//   - the darken quad's alpha (= the scene's shadow density)
// and logs it so the data can be checked against the game before the RT pass replaces the stencil test.

#include <cstdint>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <string>

#include "d3d9_include.h"
#include "../util/util_matrix.h"
#include "../vulkan/vulkan_loader.h"
#include "../util/rc/util_rc_ptr.h"

struct SodBridgeShared;

namespace dxvk {

  class D3D9DeviceEx;

  namespace sod {

    enum class ShaderKind : uint32_t { None = 0, ShadowVolumeParallel = 1, ShadowVolumePoint = 2, Untagged = 3 };

    // registry filled by d3d8 (via the exported DXVK_SoD_TagVertexShader)
    void       TagVertexShader(const void* vs9, ShaderKind kind);
    ShaderKind GetShaderKind(const void* vs9);

    struct CasterDraw {
      uint32_t primType    = 0;
      uint32_t primCount   = 0;
      int32_t  baseVertex  = 0;
      uint32_t minIndex    = 0;
      uint32_t numVertices = 0;
      uint32_t startIndex  = 0;
      bool     indexed     = false;
      uint32_t vbStride    = 0;
      uint32_t vbOffset    = 0;
      uint32_t vbSize      = 0;
      uint32_t boneRegs    = 0;      // highest c8+N register written with non-zero data
    };

    struct LightPass {
      ShaderKind kind = ShaderKind::None;
      Vector4    c5   = { };         // parallel: dir*len   point: pos.xyz, len
      Vector4    c6   = { };         // point: normalised dir
      Matrix4    viewProj;           // c0..c3 (rows)
      float      density = -1.0f;    // darken quad alpha / 255
      std::vector<CasterDraw> casters;
      std::vector<std::string> draws;     // diagnostics: every draw inside the pass (first 12)
      uint32_t   drawCount = 0;
    };

    class RtCapture {
    public:
      void OnInit(VkPhysicalDevice adapter, const Rc<vk::InstanceFn>& vki);
      void OnClear(DWORD flags);
      // anything that changes the back buffer since the last Present (draws are counted in OnAnyDraw)
      void MarkWork() { m_work++; }
      bool HadWorkSincePresent() const { return m_allDraws != 0 || m_work != 0; }
      bool SoDFixPresent() const { return m_getRtLevel != nullptr; }
      void OnCasterDraw(ShaderKind kind, const CasterDraw& draw, const Vector4* vsConsts);
      void OnDrawUP(bool stencilEnabled, bool positionT, D3DPRIMITIVETYPE type, UINT primCount,
                    const void* data, UINT stride);
      void OnPresent();
      // any draw (diagnostics): what is drawn between the stencil clear and the darken quad
      void OnAnyDraw(const void* vs, uint32_t fvf, const char* api, uint32_t primType, uint32_t primCount,
                     bool stencil, uint32_t stencilPass, uint32_t colorWrite);

      bool Enabled() const { return m_enabled; }
      // bridge access for the B2 frame link
      bool BridgeReady() const;
      struct SodBridgeShared* Bridge() const { return m_bridge; }
      void* HelperProcess() const { return m_helperProcess; }
      int  RtLevel() const { return m_getRtLevel ? m_getRtLevel() : 0; }
      int  LampLighting() const { return m_getLamps ? m_getLamps() : 0; }   // SoDFix [Display] LampLighting: 0 off, 1 on
      int  AntiAliasing() const { return m_getAa ? m_getAa() : 0; }   // SoDFix ANTI-ALIASING: 0 off, 1 SMAA
      int  TextureFiltering() const { return m_getTf ? m_getTf() : 0; }   // 0 standard, 1 trilinear, 2 anisotropic 16x
      bool HasTextureFiltering() const { return m_getTf != nullptr; }
      int  CinematicBars() const { return m_getBars ? m_getBars() : 1; }   // 1 on (game default), 0 off
      int  AmbientOcclusion() const;                                         // 0 off, 1 low, 2 high, 3 ray traced (SODRT_AO overrides)
      int  Reflections() const;                                              // 0 off, 1 low, 2 high (SODRT_SSR overrides)
      int  SurfaceDepth() const;                                             // 0 off, 1 low, 2 high (SODRT_RELIEF overrides)
      int  TextureUpscaling() const;                                         // 0 off, 1 2X, 2 3X, 3 4X (SODRT_UPSCALE=0/2/3/4)
      int  ImprovedGeometry() const;
      int  SmoothCamera() const;                                             // 0 off, 1 on (SODRT_SMOOTHCAM overrides)                                         // 0 off, 1 on (SODRT_GEOM overrides)
      int  Hdr() const;                                                      // 0 off, 1 on (SODRT_HDR overrides)
      float HdrPaperNits() const { return m_getHdrNits ? float(m_getHdrNits(0)) : 0.0f; }   // 0 = Windows' SDR content brightness
      float HdrHighlight() const { return m_getHdrNits ? float(m_getHdrNits(3)) / 100.0f : 0.6f; } // SoDFix.ini HdrHighlight %
      int   HdrVal(int which) const { return m_getHdrNits ? m_getHdrNits(which) : 0; }            // SoDFix_GetHdrNits
      float HdrGamma() const     { int g = HdrVal(4); return g >= 100 && g <= 300 ? float(g) / 100.0f : 2.1f; }
      float HdrSaturation() const { int v = HdrVal(5); return v >= 1 && v <= 300 ? float(v) / 100.0f : 1.0f; }
      bool  CalibOpen() const    { return m_getHdrCalib && m_getHdrCalib(0) != 0; }                // HDR CALIBRATION screen
      int   CalibRow() const     { return m_getHdrCalib ? m_getHdrCalib(1) : 0; }
      // the screen drawn over the option menu: 0 none, 1 HDR CALIBRATION, 2 GRAPHIC SETTING; its row; its texts
      int   MenuScreen() const   { return m_getMenuScreen ? m_getMenuScreen(0) : CalibOpen() ? 1 : 0; }
      int   MenuRow() const      { return m_getMenuScreen ? m_getMenuScreen(1) : CalibRow(); }
      int   MenuRows() const     { return m_getMenuScreen ? m_getMenuScreen(2) : 0; }
      int   MenuPage() const     { return m_getMenuScreen ? m_getMenuScreen(3) : 3; }
      int   MenuItem(int row, int which) const { return m_getMenuItem ? m_getMenuItem(row, which) : 0; }
      const char* MenuText(int kind, int row) const { const char* t = m_getMenuText ? m_getMenuText(kind, row) : nullptr; return t ? t : ""; }
      bool  MenuActive() const   { return m_getHdrCalib && m_getHdrCalib(2) != 0; }
      float HdrPeakNits() const  { return m_getHdrNits ? float(m_getHdrNits(1)) : 1000.0f; }
      int   HdrDebug() const;   // SoDFix.ini [Display] HdrDebug
      uint64_t Frame() const { return m_frame; }
      bool InPass()  const { return m_inPass; }

      // frame capture for offline analysis (%LOCALAPPDATA%\SoDRT\sod_rt_capture_N.bin)
      bool Dumping() const { return m_dumping; }
      static bool AutoCapture();
      int  PassIndex() const { return m_inPass ? int(m_passes.size()) - 1 : -1; }
      void DumpRecord(const void* hdr, size_t hdrSize, const void* vb, size_t vbSize, const void* ib, size_t ibSize);
      // capture v2: tagged events (clears, render-target switches, scenes, copies, presents)
      void DumpEvent(uint32_t tag, const void* payload, size_t size);

    private:
      bool                   m_enabled   = true;
      bool                   m_inPass    = false;
      uint64_t               m_frame     = 0;
      uint64_t               m_lastLog   = 0;
      uint32_t               m_lastSig   = 0;
      uint64_t               m_sigSince  = 0;
      std::vector<LightPass> m_passes;
      bool                   m_dumping   = false;
      bool                   m_dumpArmed = false;
      uint32_t               m_dumpCount = 0;
      uint32_t               m_work = 0;
      uint64_t               m_lastDump  = 0;
      uint32_t               m_dumpDraws = 0;
      uint32_t               m_dumpFramesLeft = 0;
      uint32_t               m_allDraws = 0, m_lastAllDraws = 0, m_maxAllDraws = 0;   // every draw call of the frame
      uint32_t               m_dumpRecords = 0;
      uint32_t               m_frameDraws = 0, m_frameClears = 0, m_frameRts = 0, m_frameCopies = 0;
      uint32_t             (*m_getPresentSite)() = nullptr;
      std::vector<uint8_t>   m_dumpBuf;
      void                   FinishDump();
      int                  (*m_getRtLevel)() = nullptr;
      int                  (*m_getLamps)() = nullptr;
      int                  (*m_getAa)() = nullptr;
      int                  (*m_getTf)() = nullptr;
      int                  (*m_getBars)() = nullptr;
      int                  (*m_getAo)() = nullptr;
      int                  (*m_getSsr)() = nullptr;
      int                  (*m_getRelief)() = nullptr;
      int                  (*m_getUp)() = nullptr;
      int                  (*m_getGeom)() = nullptr;
      int                  (*m_getSmoothCam)() = nullptr;
      int                  (*m_getHdr)() = nullptr;
      int                  (*m_getHdrNits)(int) = nullptr;
      int                  (*m_getHdrCalib)(int) = nullptr;
      int                  (*m_getMenuScreen)(int) = nullptr;
      const char*          (*m_getMenuText)(int, int) = nullptr;
      int                  (*m_getMenuItem)(int, int) = nullptr;

      // 64-bit hardware RT bridge (sod_rt64.exe)
      void*                  m_bridgeMap    = nullptr;   // HANDLE
      void*                  m_bridgeReady  = nullptr;   // HANDLE
      void*                  m_bridgeJob    = nullptr;   // HANDLE
      void*                  m_helperProcess = nullptr;  // HANDLE (kept for DuplicateHandle)
      struct SodBridgeShared* m_bridge      = nullptr;
      bool                   m_bridgeLogged = false;
      void                   StartBridge(VkPhysicalDevice adapter, const Rc<vk::InstanceFn>& vki);
      void                   PollBridge();

      LightPass& CurrentPass();
      void       LogFrame(const char* why);
    };

  }
}
