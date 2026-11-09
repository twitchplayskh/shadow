#pragma once

// SoD screen-space reflections on the polished floors listed in SoDFix\reflect.txt (generated from ccc.wad).
//
// Texture identity: room draws bind entries of the game's texture table (SoDFix_GetTextureTable: 40-byte entries,
// IDirect3DTexture8* at +0x10, indices -16384..16383; > 0 = 512x512 pages, < 0 = sub-textures cut from pages).
// reflect.txt names pages (with the reflective regions of each) and cut sub-textures. Every frame the d3d9 texture
// behind each listed entry is looked up through d3d8.dll (DXVK_SoD_D3D9Texture).
//
// Per frame: opaque fixed-function room draws with such a texture are recorded; after the opaque scene they are drawn
// again into a reflectivity mask (depth test against the game's depth, no depth write; pages through a per-page mask
// texture with the draw's own UVs, cut textures with a constant), then the compute pass traces reflections from the
// game's depth and colour and the result is blended over the frame (D3D9DeviceEx::SodSsrApply).

#include <unordered_map>
#include <vector>
#include <string>

#include "../dxvk/dxvk_device.h"

namespace dxvk {

  class D3D9DeviceEx;

  namespace sod {

    struct SsrRegion { uint16_t x, y, w, h; uint8_t cls; };
    struct SsrEntry  { int index; bool page; uint16_t pw, ph; uint8_t cls; std::vector<SsrRegion> regions; };

    class SsrTable {
    public:
      bool Load(const std::string& path);
      const std::vector<SsrEntry>& Entries() const { return m_entries; }
      // mask texel for a class: R = strength, G = roughness
      static uint32_t ClassTexel(uint8_t cls);
    private:
      std::vector<SsrEntry> m_entries;
    };

    struct SsrParams {
      float proj[4], viewport[4];
      float maxDist, thickness, strength, nearZ;
      float fogStart, fogEnd, fogOn, debug;
      uint32_t steps, refine;
    };

    class SsrPass {
    public:
      explicit SsrPass(const Rc<DxvkDevice>& device);
      ~SsrPass();
      bool Record(D3D9DeviceEx* parent, const Rc<DxvkImage>& depthImage, const Rc<DxvkImage>& colorImage,
                  const Rc<DxvkImage>& maskImage, uint32_t width, uint32_t height, const SsrParams& p);
      const Rc<DxvkBuffer>& Result() const { return m_out; }
    private:
      Rc<DxvkDevice>            m_device;
      const DxvkPipelineLayout* m_layout   = nullptr;
      VkPipeline                m_pipeline = VK_NULL_HANDLE;
      Rc<DxvkBuffer>            m_depth, m_color, m_mask, m_raw, m_out;
      uint32_t                  m_width = 0, m_height = 0;
      Rc<DxvkBuffer> MakeBuffer(VkDeviceSize size, const char* name);
    };

  }
}
