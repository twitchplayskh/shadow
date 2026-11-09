#pragma once

// SoD-RT bridge B2: per-frame GPU link between DXVK (32-bit, inside shadow.exe) and the 64-bit
// hardware ray tracing helper sod_rt64.exe.
//
// Shared objects (same physical GPU, NT handles duplicated into the helper):
//   - timeline semaphore: request k -> DXVK signals 2k+1 after writing inputs, the helper signals 2k+2
//     after writing outputs, DXVK's queue waits for 2k+2 before it continues
//   - depth buffer copy (float32 per pixel), result buffer (1 byte per pixel: bit l = shadowed by light l)
//   - frame geometry (CPU shared memory, one slot per request): lights, caster meshes, camera
//
// Per frame (B2b): the game's shadow volumes give the light directions (one light per darken quad) and
// which characters cast; the characters' own meshes (CPU-skinned, world space) are the occluders. The
// request is made just after the characters are drawn, when the depth buffer holds room + characters.
//
// Safety: a watchdog releases the game's queue from the CPU if the helper stops answering or exits,
// and the link is then disabled for the session (the original shadows stay).

#include <atomic>
#include <thread>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "d3d9_include.h"
#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_fence.h"

struct SodBridgeShared;
struct SodGeomFrame;

namespace dxvk {

  class D3D9DeviceEx;
  class D3D9Surface;

  namespace sod {

    struct VolumeInfo {
      float dir[3];            // unit extrusion direction (away from the light), first silhouette edge
      float lo[3], hi[3];      // bounds of the caster outline (near points)
      // point lights: the extrusion edges fan out from the light's position. Least-squares meeting point of all
      // edge lines: sum (I - u u^T) X = sum (I - u u^T) a; A packed xx xy xz yy yz zz.
      double A[6] = { }, b[3] = { };
      float minDot = 1.0f;     // smallest cos between an edge and dir (1 = all parallel)
    };

    class BridgeLink {
    public:
      BridgeLink(D3D9DeviceEx* parent, const Rc<DxvkDevice>& device, SodBridgeShared* shared,
                 HANDLE helperProcess, DWORD gamePid);
      ~BridgeLink();

      bool Failed() const { return m_failed.load(); }

      // ---- profiling (QPC ticks, device thread) ----
      enum CpuStage { CpuFetch = 0, CpuTrigger, CpuApply, CpuPresent, CpuStageCount };
      void AddCpu(CpuStage s, uint64_t ticks) { m_cpu[s] += ticks; }
      bool SeenDraw(uint64_t drawKey) const   { return m_drawKeys.count(drawKey) != 0; }

      // ---- frame observation (device thread, device lock held) ----
      void OnClearStencil()                    { m_curVolumes.clear(); }
      void OnVolume(const VolumeInfo& v)       { m_curVolumes.push_back(v); }
      void OnDarken(float density);
      void OnCharacter(uint64_t drawKey, uint32_t meshKey, const float* tris, size_t triCount);
      void OnCamera(const float viewProj[16], const D3DVIEWPORT9& vp);

      // a darken quad was seen this frame and nothing has been traced yet
      bool WantsTrigger() const { return !m_lights.empty() && !m_triggered; }

      // trace now: copy depth, post the frame geometry, signal, flush, wait (before the current draw)
      void Trigger(D3D9Surface* depthStencil, uint32_t frame, bool verifyDump, bool apply = false);

      // at Present: trigger if a shadowed frame did not reach a trigger point, then reset the frame state
      void EndFrame(D3D9Surface* depthStencil, uint32_t frame, bool verifyDump, bool apply = false);

    private:
      struct SharedBuffer {
        VkBuffer        buffer   = VK_NULL_HANDLE;
        VkDeviceMemory  memory   = VK_NULL_HANDLE;
        VkDeviceSize    size     = 0;
        VkDeviceSize    allocSize = 0;
        uint32_t        memTypeBits = 0;
        Rc<DxvkBuffer>  dxvk;
      };
      struct LightGroup {
        float density = 0.0f;
        std::vector<VolumeInfo> volumes;
      };
      struct Caster {
        std::vector<float> tris;           // 9 floats per triangle
        float lo[3] = {  1e30f,  1e30f,  1e30f };
        float hi[3] = { -1e30f, -1e30f, -1e30f };
      };

      D3D9DeviceEx*         m_parent;
      Rc<DxvkDevice>        m_device;
      SodBridgeShared*      m_shared;
      HANDLE                m_helperProcess;
      HANDLE                m_requestEvent = nullptr;
      HANDLE                m_geomMap      = nullptr;
      uint8_t*              m_geom         = nullptr;   // SOD_BRIDGE_SLOTS * SOD_GEOM_SLOT_BYTES

      Rc<DxvkFence>         m_fence;
      Rc<DxvkFence>         m_readbackFence;      // local: readback copy done (value = request)
      SharedBuffer          m_depth;
      SharedBuffer          m_result;
      Rc<DxvkBuffer>        m_readback;            // host-visible copy of the result
      Rc<DxvkBuffer>        m_depthReadback;       // host-visible copy of the depth (verification dumps only)
      uint32_t              m_width = 0, m_height = 0;
      VkFormat              m_format = VK_FORMAT_UNDEFINED;
      uint32_t              m_generation = 0;

      // frame state
      std::vector<VolumeInfo>            m_curVolumes;
      std::vector<LightGroup>            m_lights;
      std::unordered_map<uint32_t, Caster> m_casters;
      std::unordered_set<uint64_t>       m_drawKeys;
      float                              m_viewProj[16] = { };
      D3DVIEWPORT9                       m_viewport = { };
      bool                               m_haveCamera = false;
      bool                               m_triggered = false;

      std::atomic<uint64_t> m_requested = { 0 };   // highest odd value posted
      uint64_t              m_readbackValue = 0;   // request whose result is being read back (0 = none)
      bool                  m_readbackDump = false;
      uint32_t              m_readbackFrame = 0;
      std::vector<uint8_t>  m_readbackGeom;        // geometry of the dumped request
      uint32_t              m_dumps = 0;
      uint32_t              m_requests = 0;
      uint32_t              m_verified = 0, m_mismatched = 0, m_emptyChecks = 0;
      uint64_t              m_lastStatsLog = 0;
      uint32_t              m_lastTris = 0, m_lastCasters = 0, m_lastLights = 0;
      uint64_t              m_cpu[CpuStageCount] = { };
      uint64_t              m_lastPresentQpc = 0, m_frameTicks = 0;
      uint32_t              m_statFrames = 0, m_statTraced = 0;
      void                  LogStats();

      std::atomic<bool>     m_failed  = { false };
      std::atomic<bool>     m_stop    = { false };
      std::thread           m_watchdog;

      bool CreateSemaphore();
      bool CreateSharedBuffer(SharedBuffer& b, VkDeviceSize size, const char* name, HANDLE* ntOut);
      void DestroySharedBuffer(SharedBuffer& b);
      bool EnsureResources(uint32_t width, uint32_t height, VkFormat format);
      uint32_t BuildGeometry(SodGeomFrame* g, uint32_t frame, uint32_t width, uint32_t height);
      uint32_t m_softWanted = 0;
      void ResetFrame();
      uint64_t Duplicate(HANDLE h);
      void Fail(const std::string& why);
      void WatchdogMain();
      void CheckReadback();
    };

  }
}
