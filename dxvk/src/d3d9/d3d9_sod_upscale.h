#pragma once

// Lebensbaum texture upscaling. The game's ordinary textures (managed pool, colour formats, up to 512x512) get a
// Vulkan image 4x (up to 128) or 2x (up to 512) their size; the game keeps seeing, locking and copying the original
// (the system-memory copy DXVK keeps for managed textures). On every upload the original texels go through an
// edge-directed 2x upscaler on the GPU (sod_upscale.comp, once or twice) into the big image; automatic mips are
// then built from it as before. No option: SODRT_UPSCALE=0 turns it off, =2 limits it to 2x.

#include "../dxvk/dxvk_device.h"

namespace dxvk {

  class D3D9DeviceEx;

  namespace sod {

    class UpscalePass {
    public:
      static constexpr uint32_t kMaxOutTexels = 1024u * 1024u;   // 512x512 at 2x, 256x256 at 4x... 128x128 at 4x

      explicit UpscalePass(const Rc<DxvkDevice>& device);
      ~UpscalePass();

      // VkFormat -> decoder code (~0u: not supported)
      static uint32_t FormatCode(VkFormat f);

      // src: tightly packed texels of the original (w x h, device format); writes mip 0 of image (w*s x h*s)
      // plain: text (edge-directed at full strength + sharp edge rebuild)
      void Upload(D3D9DeviceEx* parent, const DxvkBufferSlice& src, VkDeviceSize srcBytes, uint32_t w, uint32_t h,
                  uint32_t scale, uint32_t format, bool opaque, const Rc<DxvkImage>& image, bool plain = false);
      // already upscaled texels (disk cache): straight into mip 0
      static void UploadPacked(D3D9DeviceEx* parent, const DxvkBufferSlice& src, uint32_t ow, uint32_t oh, const Rc<DxvkImage>& image);

    private:
      Rc<DxvkDevice>            m_device;
      const DxvkPipelineLayout* m_layout = nullptr;
      VkPipeline                m_pipe = VK_NULL_HANDLE;
      Rc<DxvkBuffer>            m_raw, m_a, m_b, m_c, m_out;
      Rc<DxvkBuffer> MakeBuffer(VkDeviceSize size, const char* name);
    };

  }
}
