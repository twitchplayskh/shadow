#include "d3d9_sod_link.h"
#include "d3d9_device.h"
#include "d3d9_surface.h"

#include "../sod_bridge/sod_bridge_proto.h"

#include <cstring>
#include <cmath>
#include <cstdio>
#include <algorithm>

#define LAMP_DEFAULT 30.0f

namespace dxvk::sod {

  static constexpr VkExternalMemoryHandleTypeFlagBits    kMemHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  static constexpr VkExternalSemaphoreHandleTypeFlagBits kSemHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

  static uint64_t Qpc() { LARGE_INTEGER t; ::QueryPerformanceCounter(&t); return uint64_t(t.QuadPart); }
  static double QpcMs(uint64_t a, uint64_t b) {
    LARGE_INTEGER f; ::QueryPerformanceFrequency(&f);
    return double(int64_t(b - a)) * 1000.0 / double(f.QuadPart);
  }


  BridgeLink::BridgeLink(D3D9DeviceEx* parent, const Rc<DxvkDevice>& device, SodBridgeShared* shared,
                         HANDLE helperProcess, DWORD gamePid)
  : m_parent(parent), m_device(device), m_shared(shared), m_helperProcess(helperProcess) {
    m_requestEvent = ::CreateEventA(nullptr, FALSE, FALSE, str::format(SOD_BRIDGE_REQUEST, gamePid).c_str());
    if (!m_requestEvent) { Fail("could not create the request event"); return; }
    m_geomMap = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, SOD_BRIDGE_SLOTS * SOD_GEOM_SLOT_BYTES,
                                     str::format(SOD_BRIDGE_GEOMETRY, gamePid).c_str());
    if (m_geomMap) m_geom = static_cast<uint8_t*>(::MapViewOfFile(m_geomMap, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!m_geom) { Fail("could not create the geometry mapping"); return; }
    if (!CreateSemaphore()) return;
    m_watchdog = std::thread([this] { WatchdogMain(); });
    Logger::info("SoD-RT: link: shared timeline semaphore created, watchdog running");
  }


  BridgeLink::~BridgeLink() {
    // stop serving, let the watchdog release whatever the queue still waits for, then tear down
    m_failed = true;
    if (m_shared) { m_shared->linkAbort = 1; m_shared->linkGeneration = 0; }
    m_device->waitForIdle();
    m_stop = true;
    if (m_watchdog.joinable()) m_watchdog.join();
    m_readback = nullptr;
    m_depthReadback = nullptr;
    DestroySharedBuffer(m_depth);
    DestroySharedBuffer(m_result);
    m_fence = nullptr;
    m_readbackFence = nullptr;
    if (m_requestEvent) ::CloseHandle(m_requestEvent);
    if (m_geom) ::UnmapViewOfFile(m_geom);
    if (m_geomMap) ::CloseHandle(m_geomMap);
  }


  void BridgeLink::Fail(const std::string& why) {
    if (m_failed.exchange(true)) return;
    Logger::warn(str::format("SoD-RT: link DISABLED: ", why, " - the game keeps its original shadows"));
    if (m_shared) m_shared->linkAbort = 1;
  }


  uint64_t BridgeLink::Duplicate(HANDLE h) {
    HANDLE remote = nullptr;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    BOOL ok = ::DuplicateHandle(::GetCurrentProcess(), h, m_helperProcess, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS);
    ::CloseHandle(h);
    return ok ? uint64_t(reinterpret_cast<uintptr_t>(remote)) : 0;
  }


  bool BridgeLink::CreateSemaphore() {
    try {
      DxvkFenceCreateInfo fi = { };
      fi.initialValue = 0;
      fi.sharedType = kSemHandle;
      fi.skipKmtHandles = true;                          // our single NT-handle export is for the helper
      m_fence = new DxvkFence(m_device.ptr(), fi);
    } catch (const DxvkError& e) {
      Fail(str::format("timeline semaphore: ", e.message()));
      return false;
    }
    m_readbackFence = new DxvkFence(m_device.ptr(), DxvkFenceCreateInfo { 0 });
    HANDLE local = m_fence->sharedHandle();
    if (!local || local == INVALID_HANDLE_VALUE) { Fail("could not export the timeline semaphore"); return false; }
    uint64_t h = Duplicate(local);
    if (!h) { Fail(str::format("could not duplicate the semaphore handle into the helper (error ", ::GetLastError(), ")")); return false; }
    m_shared->hSemaphore = h;
    return true;
  }


  bool BridgeLink::CreateSharedBuffer(SharedBuffer& b, VkDeviceSize size, const char* name, HANDLE* ntOut) {
    auto vkd = m_device->vkd();
    VkDevice dev = vkd->device();

    VkExternalMemoryBufferCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    ext.handleTypes = kMemHandle;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext };
    bi.size  = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
             | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT      // DXVK writes depth aspects through a texel-buffer compute copy
             | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;    // descriptor-heap binding model builds descriptors from addresses
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkd->vkCreateBuffer(dev, &bi, nullptr, &b.buffer) != VK_SUCCESS) { Fail(str::format(name, ": vkCreateBuffer failed")); return false; }

    VkMemoryRequirements req = { };
    vkd->vkGetBufferMemoryRequirements(dev, b.buffer, &req);
    const auto& mp = m_device->adapter()->memoryProperties();
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
      if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
       && !(mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
        type = i;
    if (type == UINT32_MAX) { Fail(str::format(name, ": no device-local memory type")); return false; }

    VkMemoryAllocateFlagsInfo flags = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &flags };
    ded.buffer = b.buffer;
    VkExportMemoryAllocateInfo exp = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded };
    exp.handleTypes = kMemHandle;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp };
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = type;
    if (vkd->vkAllocateMemory(dev, &ai, nullptr, &b.memory) != VK_SUCCESS) { Fail(str::format(name, ": exportable allocation failed")); return false; }
    if (vkd->vkBindBufferMemory(dev, b.buffer, b.memory, 0) != VK_SUCCESS) { Fail(str::format(name, ": bind failed")); return false; }

    VkMemoryGetWin32HandleInfoKHR gi = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
    gi.memory = b.memory;
    gi.handleType = kMemHandle;
    HANDLE nt = nullptr;
    if (vkd->vkGetMemoryWin32HandleKHR(dev, &gi, &nt) != VK_SUCCESS || !nt) { Fail(str::format(name, ": export failed")); return false; }
    *ntOut = nt;

    DxvkBufferCreateInfo ci = { };
    ci.size   = size;
    ci.usage  = bi.usage;
    ci.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    ci.access = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    ci.debugName = name;
    DxvkBufferImportInfo ii = { };
    ii.buffer = b.buffer;
    b.dxvk = m_device->importBuffer(ci, ii, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    b.size = size;
    b.allocSize = req.size;
    b.memTypeBits = 1u << type;                         // the helper imports with exactly this type
    return true;
  }


  void BridgeLink::DestroySharedBuffer(SharedBuffer& b) {
    auto vkd = m_device->vkd();
    b.dxvk = nullptr;
    if (b.buffer) vkd->vkDestroyBuffer(vkd->device(), b.buffer, nullptr);
    if (b.memory) vkd->vkFreeMemory(vkd->device(), b.memory, nullptr);
    b = SharedBuffer();
  }


  bool BridgeLink::EnsureResources(uint32_t width, uint32_t height, VkFormat format) {
    if (width == m_width && height == m_height && format == m_format && m_depth.dxvk != nullptr)
      return true;

    // resolution change: nothing may be in flight on the old buffers
    if (m_generation) {
      m_shared->linkGeneration = 0;
      m_device->waitForIdle();
      DestroySharedBuffer(m_depth);
      DestroySharedBuffer(m_result);
      m_readback = nullptr;
      m_depthReadback = nullptr;
      m_readbackValue = 0;
    }

    uint32_t bpp;
    switch (format) {
      case VK_FORMAT_D24_UNORM_S8_UINT:
      case VK_FORMAT_X8_D24_UNORM_PACK32:
      case VK_FORMAT_D32_SFLOAT:
      case VK_FORMAT_D32_SFLOAT_S8_UINT: bpp = 4; break;
      default: Fail(str::format("depth format ", format, " not supported yet")); return false;
    }

    VkDeviceSize depthSize  = VkDeviceSize(width) * height * bpp;
    VkDeviceSize resultSize = (VkDeviceSize(width) * height + 3u) & ~VkDeviceSize(3);
    HANDLE hd = nullptr, hr = nullptr;
    if (!CreateSharedBuffer(m_depth,  depthSize,  "SoD-RT depth",  &hd)) return false;
    if (!CreateSharedBuffer(m_result, resultSize, "SoD-RT result", &hr)) return false;

    DxvkBufferCreateInfo rb = { };
    rb.size   = resultSize;
    rb.usage  = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    rb.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    rb.access = VK_ACCESS_TRANSFER_WRITE_BIT;
    rb.debugName = "SoD-RT readback";
    m_readback = m_device->createBuffer(rb, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

    m_width = width; m_height = height; m_format = format;
    SodBridgeShared& s = *m_shared;
    s.linkWidth = width; s.linkHeight = height;
    s.depthFormat = uint32_t(VK_FORMAT_D32_SFLOAT); s.depthBytesPerPixel = bpp;   // always delivered as float32
    s.hDepthMem  = Duplicate(hd); s.depthAllocSize  = m_depth.allocSize;  s.depthBufSize  = depthSize;  s.depthMemTypeBits  = m_depth.memTypeBits;
    s.hResultMem = Duplicate(hr); s.resultAllocSize = m_result.allocSize; s.resultBufSize = resultSize; s.resultMemTypeBits = m_result.memTypeBits;
    if (!s.hDepthMem || !s.hResultMem) { Fail("could not share the buffers with the helper"); return false; }
    ::MemoryBarrier();
    s.linkGeneration = ++m_generation;
    Logger::info(str::format("SoD-RT: link: generation ", m_generation, " shared buffers ", width, "x", height,
      " depth format ", format, " (", depthSize / 1024, " KB in, ", resultSize / 1024, " KB out)"));
    return true;
  }


  // ---------------------------------------------------------------- frame observation
  void BridgeLink::OnDarken(float density) {
    if (m_lights.size() >= SOD_MAX_LIGHTS) return;
    LightGroup g;
    g.density = density;
    g.volumes = std::move(m_curVolumes);      // the stencil content this quad shades
    m_curVolumes.clear();
    if (!g.volumes.empty()) m_lights.push_back(std::move(g));
  }


  void BridgeLink::OnCharacter(uint64_t drawKey, uint32_t meshKey, const float* tris, size_t triCount) {
    if (!m_drawKeys.insert(drawKey).second) return;      // second (overlay) pass of the same part
    Caster& c = m_casters[meshKey];
    c.tris.insert(c.tris.end(), tris, tris + triCount * 9);
    for (size_t i = 0; i < triCount * 3; i++)
      for (uint32_t j = 0; j < 3; j++) {
        float v = tris[i * 3 + j];
        c.lo[j] = std::min(c.lo[j], v);
        c.hi[j] = std::max(c.hi[j], v);
      }
  }


  void BridgeLink::OnCamera(const float viewProj[16], const D3DVIEWPORT9& vp) {
    std::memcpy(m_viewProj, viewProj, sizeof m_viewProj);
    m_viewport = vp;
    m_haveCamera = true;
  }


  void BridgeLink::ResetFrame() {
    m_curVolumes.clear();
    m_lights.clear();
    m_casters.clear();
    m_drawKeys.clear();
    m_haveCamera = false;
    m_triggered = false;
  }


  static bool Invert4(const double m[16], double out[16]) {
    double inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (std::abs(det) < 1e-30) return false;
    for (int i = 0; i < 16; i++) out[i] = inv[i] / det;
    return true;
  }


  // fills the geometry slot; returns the number of triangles (0 = nothing to trace)
  uint32_t BridgeLink::BuildGeometry(SodGeomFrame* g, uint32_t frame, uint32_t width, uint32_t height) {
    std::memset(g, 0, sizeof *g);
    g->magic = SOD_GEOM_MAGIC; g->frame = frame; g->width = width; g->height = height;
    g->flags = 1u;                                       // result = light factor byte (255 = lit)
    // soft shadows (RAY TRACING HIGH = 8 rays per light near shadow edges, ULTRA = 16; MEDIUM stays sharp).
    // Light angular radius: tan 0.005 (~0.3 deg), so the penumbra is ~1 unit wide per 100 units between the
    // caster and the floor: sharp at the feet, soft further up, but narrower than the gaps between fingers
    // (0.025 in 0.9.3 merged them). SODRT_SOFT=0 forces sharp, =N forces N rays; SODRT_SOFTNESS=tan overrides.
    { static int forced = [] { char v[16] = { }; return ::GetEnvironmentVariableA("SODRT_SOFT", v, sizeof v) ? atoi(v) : -1; }();
      int level = m_parent->m_sodRt.RtLevel();
      uint32_t k = forced >= 0 ? uint32_t(forced) : level >= 3 ? 16u : level == 2 ? 8u : 0u;
      if (k > 64u) k = 64u;
      static uint32_t tan4 = [] { char v[16] = { }; double t = ::GetEnvironmentVariableA("SODRT_SOFTNESS", v, sizeof v) ? atof(v) : 0.005;
                                  return uint32_t(std::min(std::max(t, 0.0), 6.5) * 1e4 + 0.5); }();
      if (k >= 4u && tan4) g->flags |= 2u | (k << 8) | (tan4 << 16);
      m_softWanted = k >= 4u ? k : 0u; }
    g->vpX = float(m_viewport.X); g->vpY = float(m_viewport.Y);
    g->vpW = float(m_viewport.Width ? m_viewport.Width : width); g->vpH = float(m_viewport.Height ? m_viewport.Height : height);
    double vp[16], inv[16];
    for (int i = 0; i < 16; i++) vp[i] = m_viewProj[i];
    if (!m_haveCamera || !Invert4(vp, inv)) return 0;
    for (int i = 0; i < 16; i++) g->invViewProj[i] = float(inv[i]);

    // lights: one per darken quad. Parallel light (all extrusion edges parallel): toLight = -direction.
    // Point light (edges fan out, e.g. ceiling lamps): toLight = the light's position, the meeting point of the
    // edge lines, flagged in boxes[l].pad = 1. (0.9.4 and earlier treated every light as parallel, so lamp
    // shadows pointed the wrong way by up to ~15 degrees.)
    bool isPoint[SOD_MAX_LIGHTS] = { };
    for (const LightGroup& l : m_lights) {
      if (g->lightCount >= SOD_MAX_LIGHTS) break;
      float d[3] = { 0, 0, 0 };
      double A[6] = { }, b[3] = { };
      float minDot = 1.0f;
      for (const VolumeInfo& v : l.volumes) {
        for (int j = 0; j < 3; j++) d[j] += v.dir[j];
        for (int j = 0; j < 6; j++) A[j] += v.A[j];
        for (int j = 0; j < 3; j++) b[j] += v.b[j];
        minDot = std::min(minDot, v.minDot);
      }
      float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      if (len < 1e-6f) continue;
      for (const VolumeInfo& v : l.volumes)
        minDot = std::min(minDot, (v.dir[0] * d[0] + v.dir[1] * d[1] + v.dir[2] * d[2]) / len);
      uint32_t li = g->lightCount;
      SodGeomLight& gl = g->lights[g->lightCount++];
      gl.density = l.density;
      for (int j = 0; j < 3; j++) gl.toLight[j] = -d[j] / len;
      if (minDot < 0.9999f) {                         // > ~0.8 deg apart: point light
        double a = A[0], bb = A[1], c = A[2], e = A[3], f = A[4], i = A[5];
        double det = a * (e * i - f * f) - bb * (bb * i - f * c) + c * (bb * f - e * c);
        if (std::abs(det) > 1e-9) {
          double X = ( b[0] * (e * i - f * f) - bb * (b[1] * i - f * b[2]) + c * (b[1] * f - e * b[2])) / det;
          double Y = ( a * (b[1] * i - f * b[2]) - b[0] * (bb * i - c * f) + c * (bb * b[2] - b[1] * c)) / det;
          double Z = ( a * (e * b[2] - b[1] * f) - bb * (bb * b[2] - b[1] * c) + b[0] * (bb * f - e * c)) / det;
          gl.toLight[0] = float(X); gl.toLight[1] = float(Y); gl.toLight[2] = float(Z);
          isPoint[li] = true;
        }
      }
    }

    // casters: character meshes that the game drew a shadow volume for (per light)
    float* out = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(g) + sizeof(SodGeomFrame));
    const float pad = 30.0f;
    for (auto& kv : m_casters) {
      const Caster& c = kv.second;
      uint32_t mask = 0;
      for (uint32_t l = 0; l < m_lights.size() && l < g->lightCount; l++)
        for (const VolumeInfo& v : m_lights[l].volumes) {
          bool overlap = true;
          for (int j = 0; j < 3; j++)
            overlap &= c.lo[j] <= v.hi[j] + pad && c.hi[j] >= v.lo[j] - pad;
          if (overlap) { mask |= 1u << l; break; }
        }
      if (!mask || g->casterCount >= SOD_MAX_CASTERS) continue;
      uint32_t n = uint32_t(c.tris.size() / 9);
      if (g->triCount + n > SOD_GEOM_MAX_TRIS) n = uint32_t(SOD_GEOM_MAX_TRIS) - g->triCount;
      if (!n) continue;
      SodGeomCaster& gc = g->casters[g->casterCount++];
      gc.firstTri = g->triCount; gc.triCount = n; gc.lightMask = mask;
      std::memcpy(out + size_t(g->triCount) * 9, c.tris.data(), size_t(n) * 36);
      g->triCount += n;
    }
    // per light: caster bounds along the light (lets the helper skip pixels that cannot be shadowed);
    // point lights: bounding sphere of the casters (u = centre, uMin = radius, pad = 1)
    for (uint32_t l = 0; l < g->lightCount; l++) {
      if (isPoint[l]) {
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        for (uint32_t c = 0; c < g->casterCount; c++) {
          if (!((g->casters[c].lightMask >> l) & 1u)) continue;
          const float* t = out + size_t(g->casters[c].firstTri) * 9;
          for (uint32_t i = 0; i < g->casters[c].triCount * 3; i++)
            for (int j = 0; j < 3; j++) { lo[j] = std::min(lo[j], t[i * 3 + j]); hi[j] = std::max(hi[j], t[i * 3 + j]); }
        }
        SodGeomLightBox& b = g->boxes[l];
        std::memset(&b, 0, sizeof b);
        float r2 = 0.0f;
        for (int j = 0; j < 3; j++) { b.u[j] = 0.5f * (lo[j] + hi[j]); float h = 0.5f * (hi[j] - lo[j]); r2 += h * h; }
        b.uMin = lo[0] <= hi[0] ? std::sqrt(r2) : -1.0f;  // no casters: negative radius, never hit
        b.pad = 1.0f;
        // lamp radius (world units; characters are ~1000 tall): soft shadows widen with the distance between
        // caster and receiver. SODRT_LAMPSIZE overrides (0 = point-sized, as sharp as distant lights)
        static float lampR = [] { char v[16] = { }; return ::GetEnvironmentVariableA("SODRT_LAMPSIZE", v, sizeof v)
                                    ? float(std::max(0.0, atof(v))) : LAMP_DEFAULT; }();
        b.v[0] = lampR;
        continue;
      }
      const float* L = g->lights[l].toLight;
      float a[3] = { 1.0f, 0.0f, 0.0f };
      if (std::abs(L[0]) > 0.9f) { a[0] = 0.0f; a[1] = 1.0f; }
      float u[3] = { L[1] * a[2] - L[2] * a[1], L[2] * a[0] - L[0] * a[2], L[0] * a[1] - L[1] * a[0] };
      float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
      for (int j = 0; j < 3; j++) u[j] /= ul;
      float v[3] = { L[1] * u[2] - L[2] * u[1], L[2] * u[0] - L[0] * u[2], L[0] * u[1] - L[1] * u[0] };
      SodGeomLightBox& b = g->boxes[l];
      for (int j = 0; j < 3; j++) { b.u[j] = u[j]; b.v[j] = v[j]; }
      b.pad = 0.0f;                                   // parallel light
      b.uMin = b.vMin = 1e30f; b.uMax = b.vMax = b.lMax = -1e30f;
      for (uint32_t c = 0; c < g->casterCount; c++) {
        if (!((g->casters[c].lightMask >> l) & 1u)) continue;
        const float* t = out + size_t(g->casters[c].firstTri) * 9;
        for (uint32_t i = 0; i < g->casters[c].triCount * 3; i++) {
          const float* p = t + size_t(i) * 3;
          float pu = p[0] * u[0] + p[1] * u[1] + p[2] * u[2];
          float pv = p[0] * v[0] + p[1] * v[1] + p[2] * v[2];
          float pl = p[0] * L[0] + p[1] * L[1] + p[2] * L[2];
          b.uMin = std::min(b.uMin, pu); b.uMax = std::max(b.uMax, pu);
          b.vMin = std::min(b.vMin, pv); b.vMax = std::max(b.vMax, pv);
          b.lMax = std::max(b.lMax, pl);
        }
      }
    }
    m_lastTris = g->triCount; m_lastCasters = g->casterCount; m_lastLights = g->lightCount;
    return g->triCount;
  }


  void BridgeLink::EndFrame(D3D9Surface* depthStencil, uint32_t frame, bool verifyDump, bool apply) {
    if (WantsTrigger()) Trigger(depthStencil, frame, verifyDump, apply);
    ResetFrame();
    uint64_t now = Qpc();
    if (m_lastPresentQpc) m_frameTicks += now - m_lastPresentQpc;
    m_lastPresentQpc = now;
    m_statFrames++;
    if (QpcMs(m_lastStatsLog, now) > 10000.0 || !m_lastStatsLog) {
      if (m_lastStatsLog) LogStats();
      m_lastStatsLog = now;
    }
  }


  void BridgeLink::LogStats() {
    LARGE_INTEGER f; ::QueryPerformanceFrequency(&f);
    auto ms = [&] (uint64_t t, uint32_t n) { return n ? double(t) * 1000.0 / double(f.QuadPart) / double(n) : 0.0; };
    const SodBridgeShared& h = *m_shared;
    Logger::info(str::format("SoD-RT: profile: ", m_statFrames, " frames, ", m_statTraced, " traced | frame ",
      ms(m_frameTicks, m_statFrames), " ms (", (m_statFrames && m_frameTicks) ? 1000.0 / ms(m_frameTicks, m_statFrames) : 0.0, " fps), in Present ",
      ms(m_cpu[CpuPresent], m_statFrames), " ms | CPU per frame: geometry fetch ", ms(m_cpu[CpuFetch], m_statFrames),
      " ms, trigger ", ms(m_cpu[CpuTrigger], m_statFrames), " ms, shadow pass ", ms(m_cpu[CpuApply], m_statFrames),
      " ms | GPU hand-off: post -> game GPU at trigger ", h.reachUsAvg / 1000.0f, " ms, game GPU stalled ", h.stallUsAvg / 1000.0f,
      " ms (max ", h.stallUsMax / 1000.0f, "), helper submit lag ", h.submitLagUsAvg / 1000.0f, " ms, helper GPU trace ", h.traceUsAvg / 1000.0f,
      " ms, build (overlapped) ", h.buildUsAvg / 1000.0f, " ms | last ", m_lastLights, " light(s) ", m_lastCasters, " caster(s) ", m_lastTris,
      " tris | soft shadows ", m_softWanted ? str::format(h.softSamples, "/", m_softWanted, " rays") : std::string("off"),
      " | checks ok ", m_verified, " empty ", m_emptyChecks, " mismatched ", m_mismatched));
    for (auto& c : m_cpu) c = 0;
    m_frameTicks = 0; m_statFrames = 0; m_statTraced = 0;
  }


  void BridgeLink::Trigger(D3D9Surface* depthStencil, uint32_t frame, bool verifyDump, bool apply) {
    m_triggered = true;
    struct Timer { BridgeLink* l; uint64_t t0 = Qpc(); ~Timer() { l->AddCpu(CpuTrigger, Qpc() - t0); } } timer = { this };
    if (m_failed || !depthStencil) return;
    if (m_shared->linkState == SOD_LINK_FAILED) {
      Fail(str::format("helper: ", m_shared->linkError));
      return;
    }

    D3D9CommonTexture* tex = depthStencil->GetCommonTexture();
    const Rc<DxvkImage>& image = tex->GetImage();
    if (image == nullptr) return;
    VkExtent2D ext = depthStencil->GetSurfaceExtent();
    VkFormat fmt = image->info().format;
    if (!EnsureResources(ext.width, ext.height, fmt)) return;

    CheckReadback();

    uint64_t value = m_requested.load() + 2u;          // 1, 3, 5, ...
    if (value == 2u) value = 1u;
    uint32_t slot = (value >> 1) % SOD_BRIDGE_SLOTS;
    auto* g = reinterpret_cast<SodGeomFrame*>(m_geom + size_t(slot) * SOD_GEOM_SLOT_BYTES);
    uint32_t tris = BuildGeometry(g, frame, ext.width, ext.height);
    if (!tris) return;                                   // no caster for any light: nothing to trace

    // post the request (CPU side) before the GPU work is recorded: the helper waits on the GPU
    SodFrameRequest& r = m_shared->req[slot];
    r.value = value; r.generation = m_generation; r.frame = frame;
    r.width = ext.width; r.height = ext.height; r.postedQpc = Qpc(); r.hasGeometry = 1;
    ::MemoryBarrier();
    ::InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&m_shared->requestValue), LONG64(value));
    m_requested = value;
    ::SetEvent(m_requestEvent);
    m_requests++;
    m_statTraced++;

    bool dump = verifyDump && m_dumps < 2 && m_readbackValue == 0;
    bool readback = dump || (m_readbackValue == 0 && (m_requests % 300u) == 10u);
    if (readback) {
      m_readbackValue = value; m_readbackDump = dump; m_readbackFrame = frame;
      if (dump) {
        m_readbackGeom.assign(reinterpret_cast<uint8_t*>(g), reinterpret_cast<uint8_t*>(g) + sizeof(SodGeomFrame) + size_t(tris) * 36);
        if (m_depthReadback == nullptr) {
          DxvkBufferCreateInfo rb = { };
          rb.size   = m_depth.size;
          rb.usage  = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
          rb.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
          rb.access = VK_ACCESS_TRANSFER_WRITE_BIT;
          rb.debugName = "SoD-RT depth readback";
          m_depthReadback = m_device->createBuffer(rb, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        }
      }
    }

    m_parent->EmitCs([
      cImage    = image,
      cDepth    = m_depth.dxvk,
      cFence    = m_fence,
      cValue    = value,
      cExtent   = VkExtent3D { ext.width, ext.height, 1u }
    ] (DxvkContext* ctx) {
      // (the semaphore signal at the end of this submission makes the copy visible to the helper)
      VkImageSubresourceLayers sub = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 0u, 1u };
      ctx->copyImageToBuffer(cDepth, 0, 0, 0, VK_FORMAT_D32_SFLOAT, cImage, sub, VkOffset3D { 0, 0, 0 }, cExtent);
      ctx->signalFence(cFence, cValue);
    });
    m_parent->ExecuteFlush(false);                       // submits the signal 2k+1
    m_parent->EmitCs([
      cResult   = m_result.dxvk,
      cDepth    = m_depth.dxvk,
      cReadback = readback ? m_readback : Rc<DxvkBuffer>(),
      cDepthRb  = dump ? m_depthReadback : Rc<DxvkBuffer>(),
      cFence    = m_fence,
      cDone     = m_readbackFence,
      cValue    = value,
      cBytes    = m_result.size,
      cDepthBytes = m_depth.size
    ] (DxvkContext* ctx) {
      ctx->waitFence(cFence, cValue + 1u);               // this submission waits for the helper (2k+2)
      if (cReadback != nullptr) {
        ctx->copyBuffer(cReadback, 0, cResult, 0, cBytes);
        if (cDepthRb != nullptr)
          ctx->copyBuffer(cDepthRb, 0, cDepth, 0, cDepthBytes);
        ctx->signalFence(cDone, cValue);                 // readback copies complete
      }
    });

    // B2c: darken the frame with the helper's light factors (after the wait above, before the next game draw)
    if (apply) {
      uint64_t t0 = Qpc();
      m_parent->SodApplyShadowPass(m_result.dxvk, ext.width, ext.height);
      AddCpu(CpuApply, Qpc() - t0);
    }

  }


  void BridgeLink::CheckReadback() {
    if (!m_readbackValue) return;
    if (m_readbackFence->getValue() < m_readbackValue) return;   // copy (after the helper's result) not done yet
    const uint8_t* p = static_cast<const uint8_t*>(m_readback->mapPtr(0));
    uint32_t n = m_width * m_height, shadowed = 0;
    for (uint32_t i = 0; i < n; i++) shadowed += p[i] != 255;
    uint32_t slot = (m_readbackValue >> 1) % SOD_BRIDGE_SLOTS;
    if (m_shared->resultValue[slot] != m_readbackValue + 1u) {
      if (m_requested.load() < m_readbackValue + 2u * SOD_BRIDGE_SLOTS) return;    // helper has not posted its count yet
      Logger::warn(str::format("SoD-RT: link: no helper count for request ", m_readbackValue));
      m_readbackValue = 0;
      return;
    }
    uint32_t helperCount = m_shared->resultCount[slot];
    bool ok = helperCount == shadowed;
    if (!ok) m_mismatched++; else if (!shadowed) m_emptyChecks++; else m_verified++;
    if (!ok || m_verified <= 2 || m_readbackDump)
      Logger::info(str::format("SoD-RT: link: check request ", m_readbackValue, " (frame ", m_readbackFrame, "): ", shadowed, " of ", n,
        " pixels ray-traced as shadowed, helper counted ", helperCount, ok ? " -> OK" : " -> MISMATCH"));

    if (m_readbackDump) {
      // verification dump: geometry + depth + mask of one frame (compare offline with tools/check_mask.py)
      char dir[MAX_PATH] = { };
      if (::GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH)) {
        std::string d = std::string(dir) + "\\SoDRT";
        ::CreateDirectoryA(d.c_str(), nullptr);
        std::string f = str::format(d, "\\sod_rt_mask_", m_readbackFrame, ".bin");
        if (FILE* fp = std::fopen(f.c_str(), "wb")) {
          const uint32_t hdr[6] = { 0x4B4D4453u /* 'SDMK' */, 1u, m_readbackFrame, m_width, m_height, uint32_t(m_readbackGeom.size()) };
          std::fwrite(hdr, 1, sizeof hdr, fp);
          std::fwrite(m_readbackGeom.data(), 1, m_readbackGeom.size(), fp);
          std::fwrite(m_depthReadback->mapPtr(0), 1, size_t(n) * 4, fp);
          std::fwrite(p, 1, n, fp);
          std::fclose(fp);
          Logger::info(str::format("SoD-RT: link: verification dump written: ", f));
        }
      }
      m_dumps++;
      m_readbackGeom.clear();
    }
    m_readbackValue = 0;
    m_readbackDump = false;
  }


  void BridgeLink::WatchdogMain() {
    uint64_t stuckValue = 0, stuckSince = 0;
    while (!m_stop) {
      ::Sleep(20);
      uint64_t requested = m_requested.load();
      if (!requested) continue;
      uint64_t value = m_fence->getValue();
      bool helperDead = m_helperProcess && ::WaitForSingleObject(m_helperProcess, 0) == WAIT_OBJECT_0;
      if (helperDead && !m_failed) Fail("the 64-bit helper exited");

      // DXVK signalled 2k+1 and nobody answered: the game's queue is (or will be) stalled on 2k+2
      bool waitingForHelper = (value & 1u) && value <= requested;
      if (!waitingForHelper) { stuckValue = 0; continue; }
      if (value != stuckValue) { stuckValue = value; stuckSince = Qpc(); }
      bool tooLong = QpcMs(stuckSince, Qpc()) > 1000.0;
      if (!m_failed && tooLong) Fail(str::format("helper did not answer request ", value, " within 1 s"));
      if (m_failed) {
        auto vkd = m_device->vkd();
        VkSemaphoreSignalInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
        si.semaphore = m_fence->handle();
        si.value = value + 1u;                           // release exactly this wait; later ones are released in turn
        vkd->vkSignalSemaphore(vkd->device(), &si);
        stuckValue = 0;
      }
    }
  }

}
