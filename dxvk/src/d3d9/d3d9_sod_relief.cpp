#include "d3d9_sod_relief.h"
#include "d3d9_device.h"

#include <fstream>
#include <sstream>
#include <unordered_map>

#include <sod_relief.h>
#include <sod_relief_gen.h>

namespace dxvk::sod {

  static uint8_t ReliefClassOf(const std::string& s) {
    return s == "C" ? ReliefCobble : s == "P" ? ReliefSlab : s == "B" ? ReliefBrick : ReliefNone;
  }


  bool ReliefTable::Load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::unordered_map<int, size_t> pages;
    std::string line;
    while (std::getline(f, line)) {
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ss(line);
      std::string kind, cls;
      ss >> kind;
      if (kind == "page") {
        int index, pw, ph, x, y, w, h;
        if (!(ss >> index >> pw >> ph >> x >> y >> w >> h >> cls) || !ReliefClassOf(cls) || w <= 0 || h <= 0) continue;
        auto it = pages.find(index);
        if (it == pages.end()) {
          it = pages.emplace(index, m_entries.size()).first;
          ReliefEntry e = { };
          e.index = index; e.page = true; e.pw = uint16_t(pw); e.ph = uint16_t(ph);
          m_entries.push_back(e);
        }
        m_entries[it->second].regions.push_back({ uint16_t(x), uint16_t(y), uint16_t(w), uint16_t(h), ReliefClassOf(cls) });
      } else if (kind == "slot") {
        int index;
        if (!(ss >> index >> cls) || !ReliefClassOf(cls)) continue;
        ReliefEntry e = { };
        e.index = index; e.page = false; e.cls = ReliefClassOf(cls);
        m_entries.push_back(e);
      }
      if (m_entries.size() >= 255) break;                   // ids are one byte in the UV / id target
    }
    return !m_entries.empty();
  }


  uint32_t ReliefPass::FormatCode(VkFormat f) {
    switch (f) {
      case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return 0u;
      case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return 1u;
      case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return 2u;
      case VK_FORMAT_R5G6B5_UNORM_PACK16:   return 3u;
      case VK_FORMAT_A4R4G4B4_UNORM_PACK16: return 4u;
      case VK_FORMAT_B4G4R4A4_UNORM_PACK16: return 5u;
      case VK_FORMAT_R4G4B4A4_UNORM_PACK16: return 6u;
      case VK_FORMAT_B5G5R5A1_UNORM_PACK16: return 7u;
      case VK_FORMAT_B5G6R5_UNORM_PACK16:   return 8u;
      default: return ~0u;
    }
  }


  struct GenPush { uint32_t a[4], b[4], c[4]; };
  struct PomPush { uint32_t size[4]; float proj[4], vp[4], light[4], fog[4], par[4], up[4]; };


  ReliefPass::ReliefPass(const Rc<DxvkDevice>& device)
  : m_device(device) {
    std::array<DxvkDescriptorSetLayoutBinding, 7> bindings;
    for (auto& b : bindings) b = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
    m_genLayout = m_device->createBuiltInPipelineLayout(0u, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(GenPush), 4u, bindings.data());
    m_pomLayout = m_device->createBuiltInPipelineLayout(0u, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(PomPush), 7u, bindings.data());
    util::DxvkBuiltInShaderStage genStage(sod_relief_gen, nullptr);
    util::DxvkBuiltInShaderStage pomStage(sod_relief, nullptr);
    m_genPipe = m_device->createBuiltInComputePipeline(m_genLayout, genStage);
    m_pomPipe = m_device->createBuiltInComputePipeline(m_pomLayout, pomStage);
    m_raw     = MakeBuffer(VkDeviceSize(kGenSlots) * kMaxItemTexels * 4u, "SoD relief raw texels");
    m_albedo  = MakeBuffer(VkDeviceSize(kAtlasTexels) * 4u, "SoD relief albedo atlas");
    m_relief  = MakeBuffer(VkDeviceSize(kReliefTexels) * 4u, "SoD relief height atlas");
    m_scratch = MakeBuffer(VkDeviceSize(kGenSlots) * (4u * kMaxItemTexels + 258u) * 4u, "SoD relief scratch");
    m_table   = MakeBuffer(VkDeviceSize(kTableWords) * 4u, "SoD relief items");
    Logger::info("SoD-RT: surface depth ready");
  }


  ReliefPass::~ReliefPass() {
    auto vk = m_device->vkd();
    if (m_genPipe) vk->vkDestroyPipeline(vk->device(), m_genPipe, nullptr);
    if (m_pomPipe) vk->vkDestroyPipeline(vk->device(), m_pomPipe, nullptr);
  }


  Rc<DxvkBuffer> ReliefPass::MakeBuffer(VkDeviceSize size, const char* name) {
    DxvkBufferCreateInfo info = { };
    info.size   = size;
    info.usage  = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    info.debugName = name;
    return m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }


  static void SodBarrier(const Rc<DxvkCommandList>& cmd, VkPipelineStageFlags2 src, VkAccessFlags2 srcA,
                         VkPipelineStageFlags2 dst, VkAccessFlags2 dstA) {
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = src; mb.srcAccessMask = srcA; mb.dstStageMask = dst; mb.dstAccessMask = dstA;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1u; dep.pMemoryBarriers = &mb;
    cmd->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dep);
  }


  void ReliefPass::Generate(D3D9DeviceEx* parent, const std::vector<ReliefGenJob>& jobs) {
    if (jobs.empty() || !m_genPipe) return;
    parent->EmitCs([
      cLayout = m_genLayout, cPipe = m_genPipe, cRaw = m_raw, cAlbedo = m_albedo, cRelief = m_relief,
      cScratch = m_scratch, cJobs = jobs
    ] (DxvkContext* ctx) {
      VkImageSubresourceLayers sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
      for (size_t k = 0; k < cJobs.size() && k < kGenSlots; k++)
        ctx->copyImageToBuffer(cRaw, VkDeviceSize(k) * kMaxItemTexels * 4u, 0, 0, cJobs[k].image->info().format,
          cJobs[k].image, sub, cJobs[k].offset, cJobs[k].extent);

      Rc<DxvkCommandList> cmd = ctx->beginExternalRendering();
      SodBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
      std::array<DxvkDescriptorWrite, 4> d = { };
      for (auto& w : d) w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      d[0].buffer = cRaw->getSliceInfo(); d[1].buffer = cAlbedo->getSliceInfo();
      d[2].buffer = cRelief->getSliceInfo(); d[3].buffer = cScratch->getSliceInfo();
      cmd->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, cPipe);
      // pass by pass for all jobs: the slots use separate raw / scratch ranges
      for (uint32_t mode = 0; mode < 8u; mode++) {
        for (size_t k = 0; k < cJobs.size() && k < kGenSlots; k++) {
          const auto& j = cJobs[k];
          GenPush pc = { };
          const uint32_t rw = j.extent.width * j.scale, rh = j.extent.height * j.scale;
          pc.a[0] = rw; pc.a[1] = rh; pc.a[2] = mode; pc.a[3] = j.format;
          pc.b[0] = uint32_t(k) * kMaxItemTexels;
          pc.b[1] = j.atlasOffset;
          pc.b[2] = uint32_t(k) * (4u * kMaxItemTexels + 258u);
          pc.b[3] = j.cls;
          pc.c[0] = j.scale; pc.c[1] = j.reliefOffset; pc.c[2] = j.extent.width; pc.c[3] = j.extent.height;
          cmd->bindResources(DxvkCmdBuffer::ExecBuffer, cLayout, d.size(), d.data(), sizeof(pc), &pc);
          uint32_t gx = mode == 0u ? (std::max(rw, 258u) + 7u) / 8u : (rw + 7u) / 8u;
          cmd->cmdDispatch(DxvkCmdBuffer::ExecBuffer, gx, (rh + 7u) / 8u, 1u);
        }
        SodBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
      }
      cmd->track(cRaw, DxvkAccess::Read);
      cmd->track(cAlbedo, DxvkAccess::Write);
      cmd->track(cRelief, DxvkAccess::Write);
      cmd->track(cScratch, DxvkAccess::Write);
    });
  }


  bool ReliefPass::Record(D3D9DeviceEx* parent, const Rc<DxvkImage>& depthImage, const Rc<DxvkImage>& colorImage,
                          const Rc<DxvkImage>& uvImage, uint32_t width, uint32_t height, const ReliefParams& p,
                          const std::vector<uint32_t>& table) {
    if (!m_pomPipe || width < 8 || height < 8 || table.size() > kTableWords) return false;
    if (width != m_width || height != m_height) {
      VkDeviceSize n = VkDeviceSize(width) * height;
      m_depth = MakeBuffer(n * 4u, "SoD relief depth");
      m_color = MakeBuffer(n * 4u, "SoD relief colour");
      m_uv    = MakeBuffer(n * 8u, "SoD relief uv / id");
      m_out   = MakeBuffer(n * 4u, "SoD relief result");
      m_width = width; m_height = height;
    }
    PomPush pc = { };
    pc.size[0] = width; pc.size[1] = height; pc.size[2] = p.steps; pc.size[3] = p.flags;
    for (int i = 0; i < 4; i++) {
      pc.proj[i] = p.proj[i]; pc.vp[i] = p.viewport[i]; pc.light[i] = p.light[i]; pc.fog[i] = p.fog[i]; pc.par[i] = p.depth[i];
      pc.up[i] = p.up[i];
    }
    parent->EmitCs([
      cLayout = m_pomLayout, cPipe = m_pomPipe, cDepthImg = depthImage, cColorImg = colorImage, cUvImg = uvImage,
      cDepth = m_depth, cColor = m_color, cUv = m_uv, cAlbedo = m_albedo, cRelief = m_relief, cTable = m_table,
      cOut = m_out, cPc = pc, cItems = table, cExtent = VkExtent3D { width, height, 1u }
    ] (DxvkContext* ctx) {
      VkImageSubresourceLayers dsub = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 0u, 1u };
      VkImageSubresourceLayers csub = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
      ctx->copyImageToBuffer(cDepth, 0, 0, 0, VK_FORMAT_D32_SFLOAT, cDepthImg, dsub, VkOffset3D { 0, 0, 0 }, cExtent);
      ctx->copyImageToBuffer(cColor, 0, 0, 0, cColorImg->info().format, cColorImg, csub, VkOffset3D { 0, 0, 0 }, cExtent);
      ctx->copyImageToBuffer(cUv, 0, 0, 0, cUvImg->info().format, cUvImg, csub, VkOffset3D { 0, 0, 0 }, cExtent);
      ctx->updateBuffer(cTable, 0, VkDeviceSize(cItems.size()) * 4u, cItems.data());

      Rc<DxvkCommandList> cmd = ctx->beginExternalRendering();
      SodBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
      std::array<DxvkDescriptorWrite, 7> d = { };
      for (auto& w : d) w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      d[0].buffer = cDepth->getSliceInfo();  d[1].buffer = cColor->getSliceInfo(); d[2].buffer = cUv->getSliceInfo();
      d[3].buffer = cAlbedo->getSliceInfo(); d[4].buffer = cRelief->getSliceInfo(); d[5].buffer = cTable->getSliceInfo();
      d[6].buffer = cOut->getSliceInfo();
      cmd->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, cPipe);
      PomPush pc = cPc;
      cmd->bindResources(DxvkCmdBuffer::ExecBuffer, cLayout, d.size(), d.data(), sizeof(pc), &pc);
      cmd->cmdDispatch(DxvkCmdBuffer::ExecBuffer, (pc.size[0] + 7u) / 8u, (pc.size[1] + 7u) / 8u, 1u);
      SodBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
      cmd->track(cDepth, DxvkAccess::Read);
      cmd->track(cColor, DxvkAccess::Read);
      cmd->track(cUv, DxvkAccess::Read);
      cmd->track(cAlbedo, DxvkAccess::Read);
      cmd->track(cRelief, DxvkAccess::Read);
      cmd->track(cTable, DxvkAccess::Read);
      cmd->track(cOut, DxvkAccess::Write);
    });
    return true;
  }

}
