#include "d3d9_sod_ssr.h"
#include "d3d9_device.h"

#include <fstream>
#include <sstream>

#include <sod_ssr.h>

namespace dxvk::sod {

  static uint8_t ClassOf(const std::string& s) { return s == "M" ? 1 : s == "W" ? 2 : s == "L" ? 3 : s == "S" ? 4 : 0; }

  uint32_t SsrTable::ClassTexel(uint8_t cls) {
    // A8R8G8B8: R = strength, G = roughness
    // S (0.13.9): polished stone walls - marble / granite cladding, a little softer and weaker than floors
    uint32_t strength = cls == 1 ? 150u : cls == 2 ? 95u : cls == 3 ? 165u : cls == 4 ? 80u : 0u;
    uint32_t rough    = cls == 1 ? 25u  : cls == 2 ? 150u : cls == 3 ? 60u  : cls == 4 ? 110u : 0u;
    return 0xFF000000u | (strength << 16) | (rough << 8);
  }


  bool SsrTable::Load(const std::string& path) {
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
        if (!(ss >> index >> pw >> ph >> x >> y >> w >> h >> cls)) continue;
        auto it = pages.find(index);
        if (it == pages.end()) {
          it = pages.emplace(index, m_entries.size()).first;
          SsrEntry e = { };
          e.index = index; e.page = true; e.pw = uint16_t(pw); e.ph = uint16_t(ph);
          m_entries.push_back(e);
        }
        m_entries[it->second].regions.push_back({ uint16_t(x), uint16_t(y), uint16_t(w), uint16_t(h), ClassOf(cls) });
      } else if (kind == "slot") {
        int index;
        if (!(ss >> index >> cls)) continue;
        SsrEntry e = { };
        e.index = index; e.page = false; e.cls = ClassOf(cls);
        m_entries.push_back(e);
      }
    }
    return !m_entries.empty();
  }


  struct SsrPush {
    uint32_t size[4];
    float    proj[4], vp[4], par[4], fog[4];
  };

  SsrPass::SsrPass(const Rc<DxvkDevice>& device)
  : m_device(device) {
    static const std::array<DxvkDescriptorSetLayoutBinding, 4> bindings = {{
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
    }};
    m_layout = m_device->createBuiltInPipelineLayout(0u, VK_SHADER_STAGE_COMPUTE_BIT,
      sizeof(SsrPush), bindings.size(), bindings.data());
    util::DxvkBuiltInShaderStage stage(sod_ssr, nullptr);
    m_pipeline = m_device->createBuiltInComputePipeline(m_layout, stage);
    Logger::info("SoD-RT: screen-space reflections ready");
  }


  SsrPass::~SsrPass() {
    auto vk = m_device->vkd();
    if (m_pipeline) vk->vkDestroyPipeline(vk->device(), m_pipeline, nullptr);
  }


  Rc<DxvkBuffer> SsrPass::MakeBuffer(VkDeviceSize size, const char* name) {
    DxvkBufferCreateInfo info = { };
    info.size   = size;
    info.usage  = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    info.debugName = name;
    return m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }


  bool SsrPass::Record(D3D9DeviceEx* parent, const Rc<DxvkImage>& depthImage, const Rc<DxvkImage>& colorImage,
                       const Rc<DxvkImage>& maskImage, uint32_t width, uint32_t height, const SsrParams& p) {
    if (!m_pipeline || width < 8 || height < 8) return false;
    if (width != m_width || height != m_height) {
      VkDeviceSize n = VkDeviceSize(width) * height * 4u;
      m_depth = MakeBuffer(n, "SoD SSR depth");
      m_color = MakeBuffer(n, "SoD SSR colour");
      m_mask  = MakeBuffer(n, "SoD SSR mask");
      m_raw   = MakeBuffer(n, "SoD SSR trace");
      m_out   = MakeBuffer(n, "SoD SSR result");
      m_width = width; m_height = height;
    }
    SsrPush pc = { };
    pc.size[0] = width; pc.size[1] = height; pc.size[2] = p.steps; pc.size[3] = p.refine;
    for (int i = 0; i < 4; i++) { pc.proj[i] = p.proj[i]; pc.vp[i] = p.viewport[i]; }
    pc.par[0] = p.maxDist; pc.par[1] = p.thickness; pc.par[2] = p.strength; pc.par[3] = p.nearZ;
    pc.fog[0] = p.fogStart; pc.fog[1] = p.fogEnd; pc.fog[2] = p.fogOn; pc.fog[3] = p.debug;

    parent->EmitCs([
      cLayout = m_layout, cPipe = m_pipeline, cDepthImg = depthImage, cColorImg = colorImage, cMaskImg = maskImage,
      cDepth = m_depth, cColor = m_color, cMask = m_mask, cRaw = m_raw, cOut = m_out, cPc = pc,
      cExtent = VkExtent3D { width, height, 1u }
    ] (DxvkContext* ctx) {
      VkImageSubresourceLayers dsub = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 0u, 1u };
      VkImageSubresourceLayers csub = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
      ctx->copyImageToBuffer(cDepth, 0, 0, 0, VK_FORMAT_D32_SFLOAT, cDepthImg, dsub, VkOffset3D { 0, 0, 0 }, cExtent);
      ctx->copyImageToBuffer(cColor, 0, 0, 0, cColorImg->info().format, cColorImg, csub, VkOffset3D { 0, 0, 0 }, cExtent);
      ctx->copyImageToBuffer(cMask, 0, 0, 0, cMaskImg->info().format, cMaskImg, csub, VkOffset3D { 0, 0, 0 }, cExtent);

      Rc<DxvkCommandList> cmd = ctx->beginExternalRendering();
      auto barrier = [&] (VkPipelineStageFlags2 src, VkAccessFlags2 srcA, VkPipelineStageFlags2 dst, VkAccessFlags2 dstA) {
        VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = src; mb.srcAccessMask = srcA; mb.dstStageMask = dst; mb.dstAccessMask = dstA;
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1u; dep.pMemoryBarriers = &mb;
        cmd->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dep);
      };
      barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

      std::array<DxvkDescriptorWrite, 4> d = { };
      d[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; d[0].buffer = cDepth->getSliceInfo();
      d[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; d[1].buffer = cColor->getSliceInfo();
      d[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; d[2].buffer = cMask->getSliceInfo();
      d[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; d[3].buffer = cRaw->getSliceInfo();
      cmd->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, cPipe);
      SsrPush pc = cPc;
      cmd->bindResources(DxvkCmdBuffer::ExecBuffer, cLayout, d.size(), d.data(), sizeof(pc), &pc);
      cmd->cmdDispatch(DxvkCmdBuffer::ExecBuffer, (pc.size[0] + 7u) / 8u, (pc.size[1] + 7u) / 8u, 1u);
      // resolve: depth-aware 5x5 average of the trace (binding 1 = trace, bit 31 of size.w)
      barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
      d[1].buffer = cRaw->getSliceInfo();
      d[3].buffer = cOut->getSliceInfo();
      pc.size[3] |= 0x80000000u;
      cmd->bindResources(DxvkCmdBuffer::ExecBuffer, cLayout, d.size(), d.data(), sizeof(pc), &pc);
      cmd->cmdDispatch(DxvkCmdBuffer::ExecBuffer, (pc.size[0] + 7u) / 8u, (pc.size[1] + 7u) / 8u, 1u);
      barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
              VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
      cmd->track(cDepth, DxvkAccess::Read);
      cmd->track(cColor, DxvkAccess::Read);
      cmd->track(cMask, DxvkAccess::Read);
      cmd->track(cRaw, DxvkAccess::Write);
      cmd->track(cOut, DxvkAccess::Write);
    });
    return true;
  }

}
