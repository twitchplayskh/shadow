#include "d3d9_sod_ao.h"
#include "d3d9_device.h"

#include <sod_gtao.h>

namespace dxvk::sod {

  struct AoPush {
    uint32_t size[4];
    float    proj[4], vp[4], par[4], fog[4];
    uint32_t cfg[4];
  };

  AoPass::AoPass(const Rc<DxvkDevice>& device)
  : m_device(device) {
    static const std::array<DxvkDescriptorSetLayoutBinding, 3> bindings = {{
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
    }};
    m_layout = m_device->createBuiltInPipelineLayout(0u, VK_SHADER_STAGE_COMPUTE_BIT,
      sizeof(AoPush), bindings.size(), bindings.data());
    util::DxvkBuiltInShaderStage stage(sod_gtao, nullptr);
    m_pipeline = m_device->createBuiltInComputePipeline(m_layout, stage);
    Logger::info("SoD-RT: ambient occlusion (GTAO) ready");
  }


  AoPass::~AoPass() {
    auto vk = m_device->vkd();
    if (m_pipeline) vk->vkDestroyPipeline(vk->device(), m_pipeline, nullptr);
  }


  Rc<DxvkBuffer> AoPass::MakeBuffer(VkDeviceSize size, const char* name) {
    DxvkBufferCreateInfo info = { };
    info.size   = size;
    info.usage  = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    info.debugName = name;
    return m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }


  bool AoPass::Record(D3D9DeviceEx* parent, const Rc<DxvkImage>& depthImage, uint32_t width, uint32_t height, const AoParams& p) {
    if (!m_pipeline || width < 8 || height < 8 || (width & 3u)) return false;
    if (width != m_width || height != m_height) {
      uint32_t hw = (width + 1u) / 2u, hh = (height + 1u) / 2u;
      m_depth = MakeBuffer(VkDeviceSize(width) * height * 4u, "SoD AO depth");
      m_half  = MakeBuffer(VkDeviceSize(hw) * hh * 8u, "SoD AO half");
      m_out   = MakeBuffer(VkDeviceSize(width) * height, "SoD AO result");
      m_width = width; m_height = height;
    }

    AoPush pc = { };
    pc.size[0] = width; pc.size[1] = height; pc.size[2] = (width + 1u) / 2u; pc.size[3] = (height + 1u) / 2u;
    for (int i = 0; i < 4; i++) { pc.proj[i] = p.proj[i]; pc.vp[i] = p.viewport[i]; }
    pc.par[0] = p.radius; pc.par[1] = p.strength; pc.par[2] = p.power; pc.par[3] = p.maxRadiusPx > 0.0f ? p.maxRadiusPx : 256.0f * float(width) / 1280.0f;
    pc.fog[0] = p.fogStart; pc.fog[1] = p.fogEnd; pc.fog[2] = p.fogOn; pc.fog[3] = p.fadeStart;
    pc.cfg[0] = p.slices; pc.cfg[1] = p.steps;

    parent->EmitCs([
      cLayout = m_layout, cPipe = m_pipeline, cImage = depthImage,
      cDepth = m_depth, cHalf = m_half, cOut = m_out, cPc = pc,
      cExtent = VkExtent3D { width, height, 1u }
    ] (DxvkContext* ctx) {
      VkImageSubresourceLayers sub = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 0u, 1u };
      ctx->copyImageToBuffer(cDepth, 0, 0, 0, VK_FORMAT_D32_SFLOAT, cImage, sub, VkOffset3D { 0, 0, 0 }, cExtent);

      Rc<DxvkCommandList> cmd = ctx->beginExternalRendering();
      auto barrier = [&] (VkPipelineStageFlags2 src, VkAccessFlags2 srcA, VkPipelineStageFlags2 dst, VkAccessFlags2 dstA) {
        VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = src; mb.srcAccessMask = srcA; mb.dstStageMask = dst; mb.dstAccessMask = dstA;
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1u; dep.pMemoryBarriers = &mb;
        cmd->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dep);
      };
      // depth copy (and last frame's reads of the result) -> compute
      barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

      std::array<DxvkDescriptorWrite, 3> d = { };
      d[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; d[0].buffer = cDepth->getSliceInfo();
      d[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; d[1].buffer = cHalf->getSliceInfo();
      d[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; d[2].buffer = cOut->getSliceInfo();
      cmd->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, cPipe);

      AoPush pc = cPc;
      pc.cfg[2] = 0u;
      cmd->bindResources(DxvkCmdBuffer::ExecBuffer, cLayout, d.size(), d.data(), sizeof(pc), &pc);
      cmd->cmdDispatch(DxvkCmdBuffer::ExecBuffer, (pc.size[2] + 7u) / 8u, (pc.size[3] + 7u) / 8u, 1u);
      barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

      pc.cfg[2] = 1u;
      cmd->bindResources(DxvkCmdBuffer::ExecBuffer, cLayout, d.size(), d.data(), sizeof(pc), &pc);
      cmd->cmdDispatch(DxvkCmdBuffer::ExecBuffer, (pc.size[0] / 4u + 7u) / 8u, (pc.size[1] + 7u) / 8u, 1u);
      // result -> the upload copy into the multiply texture; depth / half buffers -> next frame's copy
      barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
              VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

      cmd->track(cDepth, DxvkAccess::Read);
      cmd->track(cHalf, DxvkAccess::Write);
      cmd->track(cOut, DxvkAccess::Write);
    });
    return true;
  }

}
