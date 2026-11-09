#include "d3d9_sod_rt.h"

#include <cstring>
#include <cmath>
#include <cstdio>

#include <windows.h>

#include "../util/log/log.h"
#include "../util/util_string.h"
#include "../sod_bridge/sod_bridge_proto.h"

namespace dxvk::sod {

  // ---------------------------------------------------------------- shader tag registry
  static std::mutex                                     g_tagMutex;
  static std::unordered_map<const void*, ShaderKind>    g_tags;
  static const void*                                    g_lastVs   = nullptr;
  static ShaderKind                                     g_lastKind = ShaderKind::None;

  void TagVertexShader(const void* vs9, ShaderKind kind) {
    std::lock_guard<std::mutex> lock(g_tagMutex);
    g_tags[vs9] = kind;
    g_lastVs = nullptr;
    Logger::info(str::format("SoD-RT: tagged vertex shader ", vs9, " as ",
      kind == ShaderKind::ShadowVolumePoint ? "shadow volume (point light)" : "shadow volume (parallel light)"));
  }

  ShaderKind GetShaderKind(const void* vs9) {
    if (!vs9) return ShaderKind::None;
    std::lock_guard<std::mutex> lock(g_tagMutex);
    if (vs9 == g_lastVs) return g_lastKind;
    auto it = g_tags.find(vs9);
    g_lastVs   = vs9;
    g_lastKind = (it == g_tags.end()) ? ShaderKind::None : it->second;
    return g_lastKind;
  }

  // ---------------------------------------------------------------- capture
  static const char* KindName(ShaderKind k) {
    switch (k) {
      case ShaderKind::ShadowVolumeParallel: return "parallel";
      case ShaderKind::ShadowVolumePoint:    return "point";
      case ShaderKind::Untagged:             return "untagged (stencil-only draw in pass)";
      default:                               return "none";
    }
  }

  int RtCapture::AmbientOcclusion() const {
    static const int env = [] { char v[8] = { }; return ::GetEnvironmentVariableA("SODRT_AO", v, 8) ? std::atoi(v) : -1; }();
    if (env >= 0) return std::min(env, 2);
    return m_getAo ? m_getAo() : 0;
  }

  int RtCapture::Reflections() const {
    static const int env = [] { char v[8] = { }; return ::GetEnvironmentVariableA("SODRT_SSR", v, 8) ? std::atoi(v) : -1; }();
    if (env >= 0) return std::min(env, 2);
    return m_getSsr ? m_getSsr() : 0;
  }

  void RtCapture::OnInit(VkPhysicalDevice adapter, const Rc<vk::InstanceFn>& vki) {
    // SoDFix (dinput8 proxy) exports the menu's RAY TRACING level
    if (HMODULE m = ::GetModuleHandleA("dinput8.dll"))
    { m_getRtLevel = reinterpret_cast<int(*)()>(::GetProcAddress(m, "SoDFix_GetRayTracingLevel"));
      m_getPresentSite = reinterpret_cast<uint32_t(*)()>(::GetProcAddress(m, "SoDFix_GetPresentSite"));
      m_getAa = reinterpret_cast<int(*)()>(::GetProcAddress(m, "SoDFix_GetAntiAliasing"));
      m_getTf = reinterpret_cast<int(*)()>(::GetProcAddress(m, "SoDFix_GetTextureFiltering"));
      m_getBars = reinterpret_cast<int(*)()>(::GetProcAddress(m, "SoDFix_GetCinematicBars"));
      m_getAo = reinterpret_cast<int(*)()>(::GetProcAddress(m, "SoDFix_GetAmbientOcclusion"));
      m_getSsr = reinterpret_cast<int(*)()>(::GetProcAddress(m, "SoDFix_GetReflections")); }
    Logger::info(str::format("SoD-RT: capture layer active (milestone 1: capture/validation only), SoDFix link ",
      m_getRtLevel ? "found" : "not found"));

    // hardware ray tracing support of this GPU / driver
    uint32_t count = 0;
    vki->vkEnumerateDeviceExtensionProperties(adapter, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vki->vkEnumerateDeviceExtensionProperties(adapter, nullptr, &count, exts.data());
    auto has = [&] (const char* n) {
      for (auto& e : exts) if (!std::strcmp(e.extensionName, n)) return true;
      return false;
    };
    bool as = has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    bool rq = has(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    bool dh = has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    VkPhysicalDeviceProperties props = { };
    vki->vkGetPhysicalDeviceProperties(adapter, &props);
    Logger::info(str::format("SoD-RT: GPU ", props.deviceName, " | in-process (32-bit) acceleration_structure=", as ? 1 : 0,
      " ray_query=", rq ? 1 : 0, " deferred_host_operations=", dh ? 1 : 0,
      (as && rq && dh) ? " -> in-process hardware RT available" : " -> no hardware RT in this 32-bit process, starting 64-bit bridge"));
    StartBridge(adapter, vki);
  }

  // ---------------------------------------------------------------- 64-bit bridge (B1: connect + self-test)
  void RtCapture::StartBridge(VkPhysicalDevice adapter, const Rc<vk::InstanceFn>& vki) {
    DWORD pid = ::GetCurrentProcessId();
    std::string mapName   = str::format(SOD_BRIDGE_MAPPING, pid);
    std::string readyName = str::format(SOD_BRIDGE_READY, pid);

    HANDLE map = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SodBridgeShared), mapName.c_str());
    if (!map) { Logger::warn("SoD-RT: bridge: CreateFileMapping failed"); return; }
    auto* sh = static_cast<SodBridgeShared*>(::MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SodBridgeShared)));
    if (!sh) { ::CloseHandle(map); Logger::warn("SoD-RT: bridge: MapViewOfFile failed"); return; }
    std::memset(sh, 0, sizeof *sh);
    sh->magic = SOD_BRIDGE_MAGIC; sh->version = SOD_BRIDGE_VERSION; sh->parentPid = pid;

    VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 }; p2.pNext = &id;
    vki->vkGetPhysicalDeviceProperties2(adapter, &p2);
    std::memcpy(sh->deviceUUID, id.deviceUUID, 16);
    std::memcpy(sh->deviceLUID, id.deviceLUID, 8);
    sh->deviceLUIDValid = id.deviceLUIDValid;

    HANDLE ready = ::CreateEventA(nullptr, TRUE, FALSE, readyName.c_str());

    // sod_rt64.exe lives next to this d3d9.dll
    char path[MAX_PATH] = { };
    HMODULE self = nullptr;
    ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(&TagVertexShader), &self);
    ::GetModuleFileNameA(self, path, MAX_PATH);
    if (char* s = std::strrchr(path, '\\')) s[1] = 0;
    std::string exe = std::string(path) + "sod_rt64.exe";
    std::string cmd = str::format("\"", exe, "\" ", pid);

    // the helper dies with the game
    HANDLE job = ::CreateJobObjectA(nullptr, nullptr);
    if (job) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = { };
      li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi = { };
    std::vector<char> cmdBuf(cmd.begin(), cmd.end()); cmdBuf.push_back(0);
    if (!::CreateProcessA(exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                          nullptr, path, &si, &pi)) {
      Logger::warn(str::format("SoD-RT: bridge: could not start ", exe, " (error ", ::GetLastError(), ")"));
      sh->state = SOD_BRIDGE_FAILED;
    } else {
      if (job) ::AssignProcessToJobObject(job, pi.hProcess);
      ::ResumeThread(pi.hThread);
      ::CloseHandle(pi.hThread);
      m_helperProcess = pi.hProcess;
      Logger::info(str::format("SoD-RT: bridge: started ", exe, " (pid ", pi.dwProcessId, ")"));
    }
    m_bridgeMap = map; m_bridgeReady = ready; m_bridgeJob = job; m_bridge = sh;
  }

  bool RtCapture::BridgeReady() const {
    return m_bridge && m_bridgeLogged && m_bridge->state == SOD_BRIDGE_READY_OK && m_helperProcess;
  }

  void RtCapture::PollBridge() {
    if (!m_bridge || m_bridgeLogged) return;
    if (m_bridge->state == SOD_BRIDGE_STARTING) {
      if (!m_bridgeReady || ::WaitForSingleObject(m_bridgeReady, 0) != WAIT_OBJECT_0) return;
    }
    m_bridgeLogged = true;
    const SodBridgeShared& b = *m_bridge;
    if (b.state == SOD_BRIDGE_READY_OK) {
      Logger::info(str::format("SoD-RT: bridge READY: 64-bit helper pid ", b.helperPid, " on ", b.deviceName,
        " (same GPU ", b.uuidMatched ? "yes" : "no", ") | acceleration_structure=", b.hasAccelStruct, " ray_query=", b.hasRayQuery,
        " external_memory=", b.hasExternalMemory, " external_semaphore=", b.hasExternalSemaphore, " timeline=", b.hasTimelineSemaphore));
      Logger::info(str::format("SoD-RT: bridge self-test PASSED: hit=", b.selfTestHit, " miss=", b.selfTestMiss,
        " (BLAS+TLAS build + 2 ray queries in ", b.selfTestMs, " ms)"));
    } else {
      Logger::warn(str::format("SoD-RT: bridge FAILED: ", b.error[0] ? b.error : "helper exited without a report",
        " (details in %LOCALAPPDATA%\\SoDRT\\sod_rt64.log)"));
    }
  }

  LightPass& RtCapture::CurrentPass() {
    if (!m_inPass || m_passes.empty()) {     // volumes without a preceding stencil clear
      m_passes.emplace_back();
      m_inPass = true;
    }
    return m_passes.back();
  }

  void RtCapture::OnClear(DWORD flags) {
    m_work++;
    if (flags == D3DCLEAR_STENCIL) {          // 0x7A1AB0: start of one shadow light pass
      m_passes.emplace_back();
      m_inPass = true;
    }
  }

  void RtCapture::OnCasterDraw(ShaderKind kind, const CasterDraw& draw, const Vector4* c) {
    LightPass& p = CurrentPass();
    if (p.casters.empty()) {
      p.kind = kind;
      p.c5   = c[5];
      p.c6   = c[6];
      for (uint32_t r = 0; r < 4; r++) p.viewProj[r] = c[r];
    }
    CasterDraw d = draw;
    d.boneRegs = 0;
    for (uint32_t r = 8; r < 96; r++)
      if (c[r].x != 0.0f || c[r].y != 0.0f || c[r].z != 0.0f || c[r].w != 0.0f) d.boneRegs = r - 8 + 1;
    p.casters.push_back(d);
  }

  void RtCapture::OnDrawUP(bool stencil, bool positionT, D3DPRIMITIVETYPE type, UINT primCount,
                           const void* data, UINT stride) {
    // 0x7A1AD0: full-screen XYZRHW|DIFFUSE quad, alpha = density, drawn through the stencil
    if (!m_inPass || !stencil || !positionT || !data || stride < 20) return;
    if (!(type == D3DPT_TRIANGLESTRIP || type == D3DPT_TRIANGLEFAN || type == D3DPT_TRIANGLELIST)) return;
    uint32_t color = 0;
    std::memcpy(&color, reinterpret_cast<const uint8_t*>(data) + 16, 4);
    m_passes.back().density = float(color >> 24) / 255.0f;
    m_inPass = false;
    (void)primCount;
  }

  void RtCapture::OnAnyDraw(const void* vs, uint32_t fvf, const char* api, uint32_t primType, uint32_t primCount,
                            bool stencil, uint32_t stencilPass, uint32_t colorWrite) {
    m_allDraws++;
    if (!m_inPass || m_passes.empty()) return;
    LightPass& p = m_passes.back();
    p.drawCount++;
    if (p.draws.size() < 12)
      p.draws.push_back(str::format(api, " prim ", primType, " x", primCount, " vs ", vs,
        " kind ", KindName(GetShaderKind(vs)), " fvf ", std::hex, fvf, std::dec,
        " stencil ", stencil ? 1 : 0, " pass-op ", stencilPass, " colorwrite ", std::hex, colorWrite));
  }

  void RtCapture::DumpRecord(const void* hdr, size_t hdrSize, const void* vb, size_t vbSize, const void* ib, size_t ibSize) {
    if (!m_dumping || m_dumpBuf.size() > (256u << 20)) return;       // cap at 256 MB
    auto put = [&] (const void* p, size_t n) {
      const uint8_t* b = static_cast<const uint8_t*>(p); m_dumpBuf.insert(m_dumpBuf.end(), b, b + n);
    };
    uint32_t tag = 0x57415244u, len = uint32_t(hdrSize + 8 + (vb ? vbSize : 0) + (ib ? ibSize : 0)), n32;
    put(&tag, 4); put(&len, 4);
    put(hdr, hdrSize);
    n32 = vb ? uint32_t(vbSize) : 0; put(&n32, 4); if (vb && vbSize) put(vb, vbSize);
    n32 = ib ? uint32_t(ibSize) : 0; put(&n32, 4); if (ib && ibSize) put(ib, ibSize);
    m_dumpDraws++; m_dumpRecords++; m_frameDraws++;
  }

  void RtCapture::DumpEvent(uint32_t tag, const void* payload, size_t size) {
    if (!m_dumping || m_dumpBuf.size() > (256u << 20)) return;
    uint32_t len = uint32_t(size);
    const uint8_t* t = reinterpret_cast<const uint8_t*>(&tag);
    const uint8_t* l = reinterpret_cast<const uint8_t*>(&len);
    const uint8_t* b = static_cast<const uint8_t*>(payload);
    m_dumpBuf.insert(m_dumpBuf.end(), t, t + 4);
    m_dumpBuf.insert(m_dumpBuf.end(), l, l + 4);
    if (size) m_dumpBuf.insert(m_dumpBuf.end(), b, b + size);
    m_dumpRecords++;
    if (tag == 0x20524C43u) m_frameClears++;          // 'CLR '
    if (tag == 0x20545253u) m_frameRts++;             // 'SRT '
    if (tag == 0x59504F43u) m_frameCopies++;          // 'COPY'
  }

  bool RtCapture::AutoCapture() {
    // automatic shadow-frame captures were a development aid: only with SODRT_AUTOCAPTURE=1 (F12 always works)
    static const bool on = [] { char v[8] = { }; return ::GetEnvironmentVariableA("SODRT_AUTOCAPTURE", v, 8) && v[0] == '1'; }();
    return on;
  }

  void RtCapture::FinishDump() {
    m_dumping = false;
    // trailer: shadow passes (density, caster count) of the captured frame
    std::memcpy(m_dumpBuf.data() + 16, &m_dumpRecords, 4);
    char dir[MAX_PATH] = { };
    if (::GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH)) {
      std::string d = std::string(dir) + "\\SoDRT";
      ::CreateDirectoryA(d.c_str(), nullptr);
      std::string f = str::format(d, "\\sod_rt_capture2_", m_dumpCount, ".bin");
      if (FILE* fp = std::fopen(f.c_str(), "wb")) {
        std::fwrite(m_dumpBuf.data(), 1, m_dumpBuf.size(), fp); std::fclose(fp);
        Logger::info(str::format("SoD-RT: frame capture written: ", f, " (", m_dumpDraws, " draws, ", m_dumpRecords, " records, ",
          m_dumpBuf.size() / 1024, " KB, 4 frames)"));
      }
    }
    m_dumpBuf.clear(); m_dumpBuf.shrink_to_fit();
  }

  void RtCapture::OnPresent() {
    m_frame++;
    m_lastAllDraws = m_allDraws; m_allDraws = 0; m_work = 0;
    if (m_lastAllDraws > m_maxAllDraws) m_maxAllDraws = m_lastAllDraws;
    PollBridge();
    if (m_dumping) {
      // end-of-frame record: present site (SoDFix call site), shadow passes of the frame
      struct { uint32_t frame, site, passes, draws, clears, rts, copies; float density[8]; } pr = { };
      pr.frame = uint32_t(m_frame); pr.site = m_getPresentSite ? m_getPresentSite() : 0u;
      pr.passes = uint32_t(m_passes.size()); pr.draws = m_frameDraws; pr.clears = m_frameClears;
      pr.rts = m_frameRts; pr.copies = m_frameCopies;
      for (uint32_t i = 0; i < 8 && i < m_passes.size(); i++) pr.density[i] = m_passes[i].density;
      DumpEvent(0x53455250u, &pr, sizeof pr);           // 'PRES'
      Logger::info(str::format("SoD-RT: capture frame ", m_frame, ": present site 0x", std::hex, pr.site, std::dec,
        ", draws ", m_frameDraws, ", clears ", m_frameClears, ", render-target sets ", m_frameRts, ", copies ", m_frameCopies,
        ", shadow passes ", m_passes.size()));
      m_frameDraws = m_frameClears = m_frameRts = m_frameCopies = 0;
      if (--m_dumpFramesLeft == 0) FinishDump();
    }
    // F12: capture the next 4 frames on demand (e.g. a cutscene); auto captures below stay as they were
    if (!m_dumping && !m_dumpArmed && (::GetAsyncKeyState(VK_F12) & 1)) {
      m_dumpArmed = true;
      Logger::info(str::format("SoD-RT: F12 capture requested at frame ", m_frame));
    }
    // arm a capture once shadows have been active for a while (max 3 per session, 60 s apart)
    if (m_dumpArmed) {
      m_dumpArmed = false; m_dumping = true; m_dumpDraws = 0; m_dumpRecords = 0; m_dumpCount++; m_lastDump = m_frame;
      m_dumpFramesLeft = 4; m_frameDraws = m_frameClears = m_frameRts = m_frameCopies = 0;
      const char magic[8] = { 'S','O','D','C','A','P','0','2' };
      uint32_t fr = uint32_t(m_frame), zero = 0, frames = 4;
      m_dumpBuf.assign(magic, magic + 8);
      m_dumpBuf.insert(m_dumpBuf.end(), reinterpret_cast<const uint8_t*>(&fr), reinterpret_cast<const uint8_t*>(&fr) + 4);
      m_dumpBuf.insert(m_dumpBuf.end(), reinterpret_cast<const uint8_t*>(&frames), reinterpret_cast<const uint8_t*>(&frames) + 4);
      m_dumpBuf.insert(m_dumpBuf.end(), reinterpret_cast<const uint8_t*>(&zero), reinterpret_cast<const uint8_t*>(&zero) + 4);
    } else if (AutoCapture() && m_dumpCount < 3 && !m_passes.empty() && m_frame >= 600 && (m_dumpCount == 0 || m_frame - m_lastDump >= 3600)) {
      bool hasQuad = false; uint32_t shadowDraws = 0;
      for (auto& p : m_passes) { hasQuad |= p.density >= 0.0f; shadowDraws += p.drawCount; }
      // only frames that also draw the scene (not a black fade-in right after a room change)
      if (hasQuad && m_lastAllDraws >= shadowDraws + 40) m_dumpArmed = true;
    }
    // signature of this frame's light setup: pass count, kinds, rounded light vectors, density
    uint32_t sig = uint32_t(m_passes.size()) * 2654435761u;
    for (auto& p : m_passes) {
      sig = sig * 31u + uint32_t(p.kind);
      sig = sig * 31u + uint32_t(int32_t(std::floor(p.c5.x)) * 73856093 ^ int32_t(std::floor(p.c5.y)) * 19349663 ^ int32_t(std::floor(p.c5.z)) * 83492791);
      sig = sig * 31u + uint32_t(p.density * 100.0f);
      sig = sig * 31u + uint32_t(p.casters.size());
    }
    if (m_frame <= 3)                                      LogFrame("startup");
    else if (sig != m_lastSig && m_frame - m_lastLog > 30) LogFrame("light setup changed");
    else if (m_frame - m_lastLog >= 1800)                  LogFrame("periodic");
    if (sig != m_lastSig) m_sigSince = m_frame;
    m_lastSig = sig;
    m_passes.clear();
    m_inPass = false;
  }

  void RtCapture::LogFrame(const char* why) {
    m_lastLog = m_frame;
    int rt = m_getRtLevel ? m_getRtLevel() : -1;
    Logger::info(str::format("SoD-RT: frame ", m_frame, " (", why, ") RAY TRACING level ", rt,
      ", shadow light passes ", m_passes.size(), ", draw calls last frame ", m_lastAllDraws, " (max so far ", m_maxAllDraws, ")"));
    for (size_t i = 0; i < m_passes.size(); i++) {
      const LightPass& p = m_passes[i];
      uint32_t prims = 0; for (auto& c : p.casters) prims += c.primCount;
      std::string light;
      if (p.kind == ShaderKind::ShadowVolumePoint) {
        light = str::format("point pos (", p.c5.x, ", ", p.c5.y, ", ", p.c5.z, ") len ", p.c5.w,
                            " dir (", p.c6.x, ", ", p.c6.y, ", ", p.c6.z, ")");
      } else {
        float len = std::sqrt(p.c5.x * p.c5.x + p.c5.y * p.c5.y + p.c5.z * p.c5.z);
        light = str::format("parallel dir*len (", p.c5.x, ", ", p.c5.y, ", ", p.c5.z, ") len ", len);
      }
      Logger::info(str::format("SoD-RT:   pass ", i, ": ", KindName(p.kind), " | ", light,
        " | density ", p.density, " | casters ", p.casters.size(), " (", prims, " tris) | draws in pass ", p.drawCount));
      for (auto& d : p.draws) Logger::info(str::format("SoD-RT:     draw: ", d));
      for (size_t k = 0; k < p.casters.size() && k < 8; k++) {
        const CasterDraw& c = p.casters[k];
        Logger::info(str::format("SoD-RT:     caster ", k, ": ", c.indexed ? "indexed" : "non-indexed",
          " prim ", c.primType, " x", c.primCount, " verts ", c.numVertices, " base ", c.baseVertex,
          " start ", c.startIndex, " | vb stride ", c.vbStride, " offset ", c.vbOffset, " size ", c.vbSize,
          " | bone regs ", c.boneRegs));
      }
      if (i == 0) {
        const Matrix4& m = p.viewProj;
        Logger::info(str::format("SoD-RT:     viewProj r0 (", m[0].x, ", ", m[0].y, ", ", m[0].z, ", ", m[0].w,
          ") r3 (", m[3].x, ", ", m[3].y, ", ", m[3].z, ", ", m[3].w, ")"));
      }
    }
  }

}

// exported for d3d8.dll (d8vk): tag the d3d9 shader created for a D3D8 shadow-volume shader
extern "C" void __stdcall DXVK_SoD_TagVertexShader(void* vs9, uint32_t kind) {
  dxvk::sod::TagVertexShader(vs9, dxvk::sod::ShaderKind(kind));
}
