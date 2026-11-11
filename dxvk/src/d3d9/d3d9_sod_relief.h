#pragma once

// Lebensbaum surface depth: parallax occlusion mapping on the ground textures of SoDFix\relief.txt (cobblestones,
// paving slabs, brick paving; generated from ccc.wad by tools/relief/make_relief_list.py).
//
// The game draws its rooms with the fixed-function pipeline, so the effect is applied in screen space, like the
// reflections (d3d9_sod_ssr.h), with the same texture identity (game texture table -> d3d8 -> d3d9 texture):
//   1. the first time a listed texture is drawn, its top mip level is copied on the GPU and a compute shader builds
//      its height and normal map (sod_relief_gen.comp) into an atlas that stays in GPU memory;
//   2. every frame, the ground draws with a listed texture are recorded and, after the opaque scene, drawn again into
//      a UV / id target: their own texture coordinates through a mirrored identity texture plus the entry id;
//   3. sod_relief.comp rebuilds the tangent frame per pixel from depth and UVs, ray-marches the height field and
//      writes the shifted texture detail (the frame's colour x albedo ratio) with normal-map shading, blended over
//      the frame before reflections and ambient occlusion.

#include <vector>
#include <string>

#include "../dxvk/dxvk_device.h"

namespace dxvk {

  class D3D9DeviceEx;

  namespace sod {

    // material classes (relief.txt letter)
    enum : uint8_t { ReliefNone = 0, ReliefCobble = 1, ReliefSlab = 2, ReliefBrick = 3 };

    struct ReliefRegion { uint16_t x, y, w, h; uint8_t cls; };
    struct ReliefEntry  { int index; bool page; uint16_t pw, ph; uint8_t cls; std::vector<ReliefRegion> regions; };

    class ReliefTable {
    public:
      bool Load(const std::string& path);
      const std::vector<ReliefEntry>& Entries() const { return m_entries; }
    private:
      std::vector<ReliefEntry> m_entries;
    };

    struct ReliefParams {
      float proj[4], viewport[4];
      float light[4];               // view-space direction to the light, shading strength
      float fog[4];                 // start, end, on, debug
      float depth[4];               // depth (fraction of the tile) for cobbles, slabs, bricks; max shift
      float up[4];                  // view-space world up, edge feather width (texels)
      uint32_t steps, flags;        // flags bit 0: self-shadow
    };

    // one texture (slot) or texture page region in the atlas
    struct ReliefGenJob {
      Rc<DxvkImage> image;
      VkOffset3D    offset;
      VkExtent3D    extent;
      uint32_t      format;         // decoder code, see FormatCode
      uint32_t      atlasOffset;    // albedo atlas, texels
      uint32_t      reliefOffset;   // relief atlas, texels
      uint32_t      scale;          // relief texels per texture texel
      uint32_t      cls;
    };

    class ReliefPass {
    public:
      static constexpr uint32_t kAtlasTexels  = 1u << 21;   // 2M texels: 8 MB albedo
      static constexpr uint32_t kReliefTexels = 1u << 23;   // 8M texels: 32 MB relief (up to 4x the texture)
      static constexpr uint32_t kMaxItemTexels = 256u * 256u;
      static constexpr uint32_t kGenSlots     = 4u;         // textures generated per frame at most
      static constexpr uint32_t kTableWords   = 512u + 8u * 1024u;

      explicit ReliefPass(const Rc<DxvkDevice>& device);
      ~ReliefPass();

      // VkFormat -> decoder code (~0u: not supported)
      static uint32_t FormatCode(VkFormat f);

      void Generate(D3D9DeviceEx* parent, const std::vector<ReliefGenJob>& jobs);
      bool Record(D3D9DeviceEx* parent, const Rc<DxvkImage>& depthImage, const Rc<DxvkImage>& colorImage,
                  const Rc<DxvkImage>& uvImage, uint32_t width, uint32_t height, const ReliefParams& p,
                  const std::vector<uint32_t>& table);
      const Rc<DxvkBuffer>& Result() const { return m_out; }

    private:
      Rc<DxvkDevice>            m_device;
      const DxvkPipelineLayout* m_genLayout = nullptr;
      const DxvkPipelineLayout* m_pomLayout = nullptr;
      VkPipeline                m_genPipe = VK_NULL_HANDLE, m_pomPipe = VK_NULL_HANDLE;
      Rc<DxvkBuffer>            m_raw, m_albedo, m_relief, m_scratch, m_table;
      Rc<DxvkBuffer>            m_depth, m_color, m_uv, m_out;
      uint32_t                  m_width = 0, m_height = 0;
      Rc<DxvkBuffer> MakeBuffer(VkDeviceSize size, const char* name);
    };

  }
}
