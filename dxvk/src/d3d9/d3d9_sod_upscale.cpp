#include "d3d9_sod_upscale.h"
#include "d3d9_device.h"

#include <sod_upscale.h>

namespace dxvk::sod {

  uint32_t UpscalePass::FormatCode(VkFormat f) {
    switch (f) {
      case VK_FORMAT_B8G8R8A8_UNORM: return 0u;
      case VK_FORMAT_R8G8B8A8_UNORM: return 1u;
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


  struct UpPush { uint32_t a[4], b[4]; };


  UpscalePass::UpscalePass(const Rc<DxvkDevice>& device)
  : m_device(device) {
    std::array<DxvkDescriptorSetLayoutBinding, 4> bindings;
    for (auto& b : bindings) b = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
    m_layout = m_device->createBuiltInPipelineLayout(0u, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(UpPush), 4u, bindings.data());
    util::DxvkBuiltInShaderStage stage(sod_upscale, nullptr);
    m_pipe = m_device->createBuiltInComputePipeline(m_layout, stage);
    m_raw = MakeBuffer(VkDeviceSize(512u * 512u) * 4u, "SoD upscale raw");
    m_a   = MakeBuffer(VkDeviceSize(512u * 512u) * 16u, "SoD upscale 1x");
    m_b   = MakeBuffer(VkDeviceSize(kMaxOutTexels) * 16u, "SoD upscale 2x");
    m_c   = MakeBuffer(VkDeviceSize(kMaxOutTexels) * 16u, "SoD upscale 4x");
    m_out = MakeBuffer(VkDeviceSize(kMaxOutTexels) * 4u, "SoD upscale packed");
    Logger::info("SoD-RT: texture upscaling ready");
  }


  UpscalePass::~UpscalePass() {
    auto vk = m_device->vkd();
    if (m_pipe) vk->vkDestroyPipeline(vk->device(), m_pipe, nullptr);
  }


  Rc<DxvkBuffer> UpscalePass::MakeBuffer(VkDeviceSize size, const char* name) {
    DxvkBufferCreateInfo info = { };
    info.size   = size;
    info.usage  = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    info.debugName = name;
    return m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }


  void UpscalePass::UploadPacked(D3D9DeviceEx* parent, const DxvkBufferSlice& src, uint32_t ow, uint32_t oh, const Rc<DxvkImage>& image) {
    parent->EmitCs([cSrc = src, cW = ow, cH = oh, cImage = image] (DxvkContext* ctx) {
      VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
      ctx->copyBufferToImage(cImage, layers, VkOffset3D { 0, 0, 0 }, VkExtent3D { cW, cH, 1u },
        cSrc.buffer(), cSrc.offset(), 0, 0, VK_FORMAT_UNDEFINED);
    });
  }


  void UpscalePass::Upload(D3D9DeviceEx* parent, const DxvkBufferSlice& src, VkDeviceSize srcBytes, uint32_t w, uint32_t h,
                           uint32_t scale, uint32_t format, bool opaque, const Rc<DxvkImage>& image, bool plain) {
    parent->EmitCs([
      cLayout = m_layout, cPipe = m_pipe, cRaw = m_raw, cA = m_a, cB = m_b, cC = m_c, cOut = m_out,
      cSrc = src, cBytes = srcBytes, cW = w, cH = h, cScale = scale, cFmt = format, cOpaque = opaque, cImage = image,
      cPlain = plain
    ] (DxvkContext* ctx) {
      ctx->copyBuffer(cRaw, 0, cSrc.buffer(), cSrc.offset(), cBytes);

      Rc<DxvkCommandList> cmd = ctx->beginExternalRendering();
      auto barrier = [&] (VkPipelineStageFlags2 s, VkAccessFlags2 sa, VkPipelineStageFlags2 d, VkAccessFlags2 da) {
        VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = s; mb.srcAccessMask = sa; mb.dstStageMask = d; mb.dstAccessMask = da;
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1u; dep.pMemoryBarriers = &mb;
        cmd->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dep);
      };
      const VkPipelineStageFlags2 cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, tr = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
      const VkAccessFlags2 rw = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
      barrier(tr | cs, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | rw, cs, rw);
      cmd->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, cPipe);

      auto run = [&] (const Rc<DxvkBuffer>& b1, const Rc<DxvkBuffer>& b2, uint32_t pw, uint32_t ph, uint32_t mode,
                      uint32_t strength, uint32_t gx, uint32_t gy, uint32_t fs = 0u) {
        std::array<DxvkDescriptorWrite, 4> d = { };
        for (auto& x : d) x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        d[0].buffer = cRaw->getSliceInfo(); d[1].buffer = b1->getSliceInfo(); d[2].buffer = b2->getSliceInfo(); d[3].buffer = cOut->getSliceInfo();
        const uint32_t flags = (cOpaque ? 1u : 0u) | (cPlain ? 2u : 0u);
        UpPush pc = { { pw, ph, mode, cFmt }, { flags, strength, cScale, fs } };
        cmd->bindResources(DxvkCmdBuffer::ExecBuffer, cLayout, d.size(), d.data(), sizeof(pc), &pc);
        cmd->cmdDispatch(DxvkCmdBuffer::ExecBuffer, (gx + 7u) / 8u, (gy + 7u) / 8u, 1u);
        barrier(cs, VK_ACCESS_2_SHADER_WRITE_BIT, cs, rw);
      };
      // 2x edge-directed, a second 2x step for 3x / 4x (3x is resampled from 4x when packing); text (cPlain): both
      // steps at full strength, then its edges are rebuilt sharply in the pack pass
      run(cA, cB, cW, cH, 0u, 0u, cW, cH);                       // decode
      run(cA, cB, cW, cH, 1u, 256u, cW, cH);                     // 2x: diagonal
      run(cA, cB, cW, cH, 2u, 256u, cW, cH);                     //     orthogonal
      Rc<DxvkBuffer> fin = cB;
      uint32_t fs = 2u;
      if (cScale >= 3u) {
        const uint32_t s2 = cPlain ? 256u : 128u;
        run(cB, cC, cW * 2u, cH * 2u, 1u, s2, cW * 2u, cH * 2u);
        run(cB, cC, cW * 2u, cH * 2u, 2u, s2, cW * 2u, cH * 2u);
        fin = cC; fs = 4u;
      }
      const uint32_t ow = cW * cScale, oh = cH * cScale;
      run(fin, cA, ow, oh, 3u, 0u, cFmt <= 1u ? ow : ow / 2u, oh, fs);   // pack
      barrier(cs, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT, tr | cs,
              VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | rw);
      cmd->track(cRaw, DxvkAccess::Read);
      cmd->track(cA, DxvkAccess::Write);
      cmd->track(cB, DxvkAccess::Write);
      cmd->track(cC, DxvkAccess::Write);
      cmd->track(cOut, DxvkAccess::Write);

      VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
      ctx->copyBufferToImage(cImage, layers, VkOffset3D { 0, 0, 0 }, VkExtent3D { ow, oh, 1u },
        cOut, 0, 0, 0, VK_FORMAT_UNDEFINED);
    });
  }

}
