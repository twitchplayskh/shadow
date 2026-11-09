#pragma once

// SoD ambient occlusion (GTAO) from the game's depth buffer, computed on the game's own queue.
// The device decides when (after the opaque 3D scene, before see-through effects and 2D) and multiplies the
// back buffer with the result (D3D9DeviceEx::SodMultiplyPass), like the ray-traced shadow pass.

#include "../dxvk/dxvk_device.h"

namespace dxvk {

  class D3D9DeviceEx;

  namespace sod {

    struct AoParams {
      float proj[4];          // P0, P5, P10, P14 of the 3D pass (fixed function, reversed depth)
      float viewport[4];      // x, y, w, h
      float radius;           // view-space units
      float strength;         // 0..1
      float power;
      float maxRadiusPx;      // screen-space cap
      float fogStart, fogEnd; // linear fog of the 3D pass (fogOn = 0: none)
      float fogOn;
      float fadeStart;        // view z where the effect starts fading out (0 = never)
      uint32_t slices, steps;
    };

    class AoPass {
    public:
      explicit AoPass(const Rc<DxvkDevice>& device);
      ~AoPass();
      // records copy + 2 compute passes; the result (1 byte per pixel, 255 = untouched) is Result()
      bool Record(D3D9DeviceEx* parent, const Rc<DxvkImage>& depthImage, uint32_t width, uint32_t height, const AoParams& p);
      const Rc<DxvkBuffer>& Result() const { return m_out; }
    private:
      Rc<DxvkDevice>            m_device;
      const DxvkPipelineLayout* m_layout   = nullptr;
      VkPipeline                m_pipeline = VK_NULL_HANDLE;
      Rc<DxvkBuffer>            m_depth, m_half, m_out;
      uint32_t                  m_width = 0, m_height = 0;
      Rc<DxvkBuffer> MakeBuffer(VkDeviceSize size, const char* name);
    };

  }
}
