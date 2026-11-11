#include "d3d9_sod_sr.h"
#include "d3d9_device.h"

#include <fstream>

#include <sod_sr.h>

namespace dxvk::sod {

  static float HalfToFloat(uint16_t h) {
    const uint32_t s = uint32_t(h >> 15) << 31, e = (h >> 10) & 31u, m = h & 1023u;
    uint32_t f;
    if (e == 0u) {
      if (m == 0u) f = s;
      else { int ex = -1; uint32_t mm = m; do { ex++; mm <<= 1; } while (!(mm & 1024u)); f = s | uint32_t(127 - 15 - ex) << 23 | (mm & 1023u) << 13; }
    } else if (e == 31u) f = s | 0x7F800000u | m << 13;
    else f = s | (e + 112u) << 23 | m << 13;
    float r; std::memcpy(&r, &f, 4); return r;
  }


  bool SrPass::LoadWeights(const std::string& path, std::vector<float>& data, std::vector<SrLayer>& layers) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    if (b.size() < 12 || std::memcmp(b.data(), "LBSR", 4) != 0) return false;
    uint32_t ver, n;
    std::memcpy(&ver, b.data() + 4, 4); std::memcpy(&n, b.data() + 8, 4);
    if (ver != 1u || n < 2u || n > 64u || b.size() < 12u + 12u * n) return false;
    uint32_t pos = 0;
    for (uint32_t i = 0; i < n; i++) {
      SrLayer l = { };
      std::memcpy(&l.cin, b.data() + 12 + 12 * i, 4); std::memcpy(&l.cout, b.data() + 16 + 12 * i, 4); std::memcpy(&l.prelu, b.data() + 20 + 12 * i, 4);
      if (l.cin == 0 || l.cin > 64 || l.cout == 0 || l.cout > 64) return false;
      l.wOff = pos; pos += l.cout * l.cin * 9u;
      l.bOff = pos; pos += l.cout;
      l.aOff = pos; if (l.prelu) pos += l.cout;
      layers.push_back(l);
    }
    // the network shape the shader expects: 3 -> ... -> 48 (pixel shuffle x4)
    if (layers.front().cin != 3u || layers.back().cout != 48u) return false;
    const size_t off = 12u + 12u * n;
    if (b.size() < off + size_t(pos) * 2u) return false;
    data.resize(pos);
    for (uint32_t i = 0; i < pos; i++) { uint16_t h; std::memcpy(&h, b.data() + off + size_t(i) * 2u, 2); data[i] = HalfToFloat(h); }
    return true;
  }


  struct SrPush { uint32_t a[4], b[4], c[4]; };

  static VkDeviceSize Align256(VkDeviceSize v) { return (v + 255u) & ~VkDeviceSize(255u); }


  SrPass::SrPass(const Rc<DxvkDevice>& device, const std::vector<float>& weights, const std::vector<SrLayer>& layers)
  : m_device(device), m_layers(layers), m_weightData(weights) {
    std::array<DxvkDescriptorSetLayoutBinding, 7> bindings;
    for (auto& b : bindings) b = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
    m_layout = m_device->createBuiltInPipelineLayout(0u, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(SrPush), 7u, bindings.data());
    util::DxvkBuiltInShaderStage stage(sod_sr, nullptr);
    m_pipe = m_device->createBuiltInComputePipeline(m_layout, stage);
    const VkDeviceSize N = kMaxSrcTexels;
    m_weights = MakeBuffer(VkDeviceSize(weights.size()) * 4u, "SoD SR weights");
    m_raw  = MakeBuffer(N * 4u, "SoD SR raw");
    m_act  = MakeBuffer(Align256(3u * N * 4u) + 2u * Align256(64u * N * 4u), "SoD SR activations");
    m_src  = MakeBuffer(N * 16u, "SoD SR source");
    m_corr = MakeBuffer(N * 16u, "SoD SR correction");
    m_out  = MakeBuffer(4u * N * 4u, "SoD SR packed");
    Logger::info(str::format("SoD-RT: texture super-resolution ready (", layers.size(), " layers, ", weights.size(), " weights)"));
  }


  SrPass::~SrPass() {
    auto vk = m_device->vkd();
    if (m_pipe) vk->vkDestroyPipeline(vk->device(), m_pipe, nullptr);
  }


  Rc<DxvkBuffer> SrPass::MakeBuffer(VkDeviceSize size, const char* name) {
    DxvkBufferCreateInfo info = { };
    info.size   = std::max<VkDeviceSize>(size, 256u);
    info.usage  = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    info.debugName = name;
    return m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }


  // shared by the three steps: activations [X: 3 planes][P0: 64 planes][P1: 64 planes], 256-byte aligned
  struct SrRegions { VkDeviceSize x, p0, p1, xLen, pLen; };
  static SrRegions Regions(uint32_t w, uint32_t h) {
    const VkDeviceSize N = VkDeviceSize(w) * h;
    SrRegions r;
    r.xLen = Align256(3u * N * 4u); r.pLen = Align256(64u * N * 4u);
    r.x = 0; r.p0 = r.xLen; r.p1 = r.xLen + r.pLen;
    return r;
  }

  static void SrBarrier(const Rc<DxvkCommandList>& cmd) {
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1u; dep.pMemoryBarriers = &mb;
    cmd->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dep);
  }


  void SrPass::Start(D3D9DeviceEx* parent, const DxvkBufferSlice& texels, VkDeviceSize bytes, uint32_t w, uint32_t h,
                     uint32_t format, bool opaque) {
    const bool upload = !m_weightsUploaded;
    m_weightsUploaded = true;
    std::shared_ptr<std::vector<float>> wd;
    if (upload) wd = std::make_shared<std::vector<float>>(m_weightData);
    const SrRegions R = Regions(w, h);
    parent->EmitCs([
      cLayout = m_layout, cPipe = m_pipe, cW8 = m_weights, cRaw = m_raw, cAct = m_act, cSrc = m_src, cCorr = m_corr,
      cOut = m_out, cTex = texels, cBytes = bytes, cW = w, cH = h, cFmt = format, cOpaque = opaque, cR = R, cWd = wd
    ] (DxvkContext* ctx) {
      if (cWd) {
        const VkDeviceSize n = VkDeviceSize(cWd->size()) * 4u;
        for (VkDeviceSize off = 0; off < n; off += 65536u)
          ctx->updateBuffer(cW8, off, std::min<VkDeviceSize>(65536u, n - off), reinterpret_cast<const uint8_t*>(cWd->data()) + off);
      }
      ctx->copyBuffer(cRaw, 0, cTex.buffer(), cTex.offset(), cBytes);
      Rc<DxvkCommandList> cmd = ctx->beginExternalRendering();
      SrBarrier(cmd);
      cmd->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, cPipe);
      std::array<DxvkDescriptorWrite, 7> d = { };
      for (auto& x : d) x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      d[0].buffer = cRaw->getSliceInfo(); d[1].buffer = cW8->getSliceInfo();
      d[2].buffer = cAct->getSliceInfo(); d[3].buffer = cAct->getSliceInfo();
      d[4].buffer = cSrc->getSliceInfo(); d[5].buffer = cCorr->getSliceInfo(); d[6].buffer = cOut->getSliceInfo();
      for (uint32_t mode = 0; mode < 2u; mode++) {
        SrPush pc = { { cW, cH, mode, cFmt }, { 0u, cOpaque ? 1u : 0u, 0u, 0u }, { 0u, 0u, uint32_t(cR.x / 4u), 0u } };
        cmd->bindResources(DxvkCmdBuffer::ExecBuffer, cLayout, d.size(), d.data(), sizeof(pc), &pc);
        cmd->cmdDispatch(DxvkCmdBuffer::ExecBuffer, (cW + 7u) / 8u, (cH + 7u) / 8u, 1u);
        SrBarrier(cmd);
      }
      cmd->track(cRaw, DxvkAccess::Read); cmd->track(cW8, DxvkAccess::Read);
      cmd->track(cAct, DxvkAccess::Write); cmd->track(cSrc, DxvkAccess::Write);
    });
  }


  void SrPass::Layers(D3D9DeviceEx* parent, uint32_t w, uint32_t h, uint32_t first, uint32_t count) {
    const SrRegions R = Regions(w, h);
    std::vector<SrLayer> ls(m_layers.begin() + first, m_layers.begin() + std::min<size_t>(m_layers.size(), first + count));
    parent->EmitCs([
      cLayout = m_layout, cPipe = m_pipe, cW8 = m_weights, cRaw = m_raw, cAct = m_act, cSrc = m_src, cCorr = m_corr,
      cOut = m_out, cW = w, cH = h, cR = R, cFirst = first, cLs = std::move(ls)
    ] (DxvkContext* ctx) {
      Rc<DxvkCommandList> cmd = ctx->beginExternalRendering();
      SrBarrier(cmd);
      cmd->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, cPipe);
      for (size_t k = 0; k < cLs.size(); k++) {
        const uint32_t L = cFirst + uint32_t(k);
        const SrLayer& l = cLs[k];
        const VkDeviceSize inOff = L == 0 ? cR.x : (L % 2u == 1u ? cR.p0 : cR.p1);
        const VkDeviceSize inLen = L == 0 ? cR.xLen : cR.pLen;
        const VkDeviceSize outOff = L % 2u == 0u ? cR.p0 : cR.p1;
        std::array<DxvkDescriptorWrite, 7> d = { };
        for (auto& x : d) x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        d[0].buffer = cRaw->getSliceInfo(); d[1].buffer = cW8->getSliceInfo();
        d[2].buffer = cAct->getSliceInfo(inOff, inLen); d[3].buffer = cAct->getSliceInfo(outOff, cR.pLen);
        d[4].buffer = cSrc->getSliceInfo(); d[5].buffer = cCorr->getSliceInfo(); d[6].buffer = cOut->getSliceInfo();
        SrPush pc = { { cW, cH, 2u, 0u }, { l.cin, l.cout, l.prelu, l.wOff }, { l.bOff, l.aOff, 0u, 0u } };
        cmd->bindResources(DxvkCmdBuffer::ExecBuffer, cLayout, d.size(), d.data(), sizeof(pc), &pc);
        cmd->cmdDispatch(DxvkCmdBuffer::ExecBuffer, (cW + 15u) / 16u, (cH + 15u) / 16u, (l.cout + 15u) / 16u);
        SrBarrier(cmd);
      }
      cmd->track(cW8, DxvkAccess::Read); cmd->track(cAct, DxvkAccess::Write);
    });
  }


  Rc<DxvkBuffer> SrPass::MakeReadback(VkDeviceSize size) {
    DxvkBufferCreateInfo info = { };
    info.size   = std::max<VkDeviceSize>(size, 256u);
    info.usage  = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT;
    info.debugName = "SoD SR readback";
    return m_device->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  }


  void SrPass::Finish(D3D9DeviceEx* parent, uint32_t w, uint32_t h, uint32_t scale, uint32_t format, bool opaque,
                      const Rc<DxvkImage>& image, const Rc<DxvkBuffer>& readback, VkDeviceSize readbackBytes) {
    const SrRegions R = Regions(w, h);
    const uint32_t lastOut = (uint32_t(m_layers.size()) - 1u) % 2u == 0u ? uint32_t(R.p0 / 4u) : uint32_t(R.p1 / 4u);
    parent->EmitCs([
      cLayout = m_layout, cPipe = m_pipe, cW8 = m_weights, cRaw = m_raw, cAct = m_act, cSrc = m_src, cCorr = m_corr,
      cOut = m_out, cW = w, cH = h, cR = R, cLast = lastOut, cScale = scale, cFmt = format, cOpaque = opaque, cImage = image,
      cRb = readback, cRbBytes = readbackBytes
    ] (DxvkContext* ctx) {
      Rc<DxvkCommandList> cmd = ctx->beginExternalRendering();
      SrBarrier(cmd);
      cmd->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, cPipe);
      std::array<DxvkDescriptorWrite, 7> d = { };
      for (auto& x : d) x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      d[0].buffer = cRaw->getSliceInfo(); d[1].buffer = cW8->getSliceInfo();
      d[2].buffer = cAct->getSliceInfo(); d[3].buffer = cAct->getSliceInfo();
      d[4].buffer = cSrc->getSliceInfo(); d[5].buffer = cCorr->getSliceInfo(); d[6].buffer = cOut->getSliceInfo();
      const uint32_t ow = cW * cScale, oh = cH * cScale;
      {
        SrPush pc = { { cW, cH, 3u, cFmt }, { 0u, 0u, 0u, 0u }, { 0u, 0u, uint32_t(cR.x / 4u), cLast } };
        cmd->bindResources(DxvkCmdBuffer::ExecBuffer, cLayout, d.size(), d.data(), sizeof(pc), &pc);
        cmd->cmdDispatch(DxvkCmdBuffer::ExecBuffer, (cW + 7u) / 8u, (cH + 7u) / 8u, 1u);
        SrBarrier(cmd);
      }
      {
        SrPush pc = { { cW, cH, 4u, cFmt }, { cScale, cOpaque ? 1u : 0u, 0u, 0u }, { 0u, 0u, uint32_t(cR.x / 4u), cLast } };
        cmd->bindResources(DxvkCmdBuffer::ExecBuffer, cLayout, d.size(), d.data(), sizeof(pc), &pc);
        const uint32_t gx = cFmt <= 1u ? ow : ow / 2u;
        cmd->cmdDispatch(DxvkCmdBuffer::ExecBuffer, (gx + 7u) / 8u, (oh + 7u) / 8u, 1u);
        SrBarrier(cmd);
      }
      cmd->track(cAct, DxvkAccess::Read); cmd->track(cSrc, DxvkAccess::Read);
      cmd->track(cCorr, DxvkAccess::Write); cmd->track(cOut, DxvkAccess::Write);
      VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
      ctx->copyBufferToImage(cImage, layers, VkOffset3D { 0, 0, 0 }, VkExtent3D { ow, oh, 1u }, cOut, 0, 0, 0, VK_FORMAT_UNDEFINED);
      if (cRb != nullptr) ctx->copyBuffer(cRb, 0, cOut, 0, cRbBytes);
    });
  }

}

namespace dxvk::sod {

  static constexpr uint64_t kCacheLimit = 4ull << 30;        // stop adding files above 4 GB

  TexCache::TexCache() {
    static const int env = [] { char v[8] = { }; return ::GetEnvironmentVariableA("SODRT_TEXCACHE", v, 8) ? std::atoi(v) : -1; }();
    if (env == 0) return;
    char base[MAX_PATH] = { };
    if (!::GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH)) return;
    std::string dir = std::string(base) + "\\SoDRT";
    ::CreateDirectoryA(dir.c_str(), nullptr);
    dir += "\\texcache";
    ::CreateDirectoryA(dir.c_str(), nullptr);
    if (::GetFileAttributesA(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    // size on disk
    WIN32_FIND_DATAA fd;
    HANDLE h = ::FindFirstFileA((dir + "\\*.lbt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do { m_bytes += (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow; } while (::FindNextFileA(h, &fd));
      ::FindClose(h);
    }
    m_dir = dir;
    m_thread = dxvk::thread([this] { Run(); });
    Logger::info(str::format("SoD-RT: texture cache ", m_dir, " (", m_bytes >> 20, " MB)"));
  }


  TexCache::~TexCache() {
    if (m_dir.empty()) return;
    { std::lock_guard<dxvk::mutex> lock(m_mutex); m_stop = true; }
    m_cond.notify_one();
    m_thread.join();
  }


  uint64_t TexCache::Key(const void* texels, size_t bytes, uint32_t w, uint32_t h, uint32_t format, uint32_t scale, bool opaque) {
    // FNV-1a 64 over the original texels and everything that changes the result (bump kVersion with the network)
    static constexpr uint64_t kVersion = 1;
    uint64_t x = 0xCBF29CE484222325ull;
    auto mix = [&] (uint64_t v) { for (int i = 0; i < 8; i++) { x ^= (v >> (i * 8)) & 0xFF; x *= 0x100000001B3ull; } };
    mix(kVersion); mix(w); mix(h); mix(format); mix(scale); mix(opaque ? 1 : 0);
    const uint8_t* p = static_cast<const uint8_t*>(texels);
    for (size_t i = 0; i < bytes; i++) { x ^= p[i]; x *= 0x100000001B3ull; }
    return x;
  }


  std::string TexCache::Path(uint64_t key) const {
    char name[32];
    std::snprintf(name, sizeof name, "\\%016llx.lbt", static_cast<unsigned long long>(key));
    return m_dir + name;
  }


  bool TexCache::Load(uint64_t key, size_t bytes, std::vector<uint8_t>& out) const {
    if (m_dir.empty()) return false;
    HANDLE h = ::CreateFileA(Path(key).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = { };
    bool ok = ::GetFileSizeEx(h, &sz) && uint64_t(sz.QuadPart) == bytes;
    if (ok) {
      out.resize(bytes);
      DWORD got = 0;
      ok = ::ReadFile(h, out.data(), DWORD(bytes), &got, nullptr) && got == bytes;
    }
    ::CloseHandle(h);
    return ok;
  }


  void TexCache::Store(uint64_t key, std::vector<uint8_t>&& data) {
    if (m_dir.empty()) return;
    { std::lock_guard<dxvk::mutex> lock(m_mutex);
      if (m_queue.size() > 256) return;
      m_queue.emplace_back(key, std::move(data)); }
    m_cond.notify_one();
  }


  void TexCache::Run() {
    for (;;) {
      std::pair<uint64_t, std::vector<uint8_t>> item;
      { std::unique_lock<dxvk::mutex> lock(m_mutex);
        m_cond.wait(lock, [this] { return m_stop || !m_queue.empty(); });
        if (m_queue.empty()) return;
        item = std::move(m_queue.front()); m_queue.pop_front(); }
      if (m_bytes + item.second.size() > kCacheLimit) continue;
      const std::string path = Path(item.first), tmp = path + ".tmp";
      HANDLE h = ::CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE) continue;
      DWORD put = 0;
      const bool ok = ::WriteFile(h, item.second.data(), DWORD(item.second.size()), &put, nullptr) && put == item.second.size();
      ::CloseHandle(h);
      if (ok && ::MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) m_bytes += item.second.size();
      else ::DeleteFileA(tmp.c_str());
    }
  }

}
