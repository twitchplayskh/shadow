#pragma once

// Lebensbaum texture super-resolution (on top of d3d9_sod_upscale.h). Every upscaled texture is first filled by
// the fast edge-directed upscaler on upload; it is then queued here and, in the background (a time budget per
// frame), run through a compact Real-ESRGAN network on the GPU (sod_sr.comp; weights in SoDFix\sr_weights.bin,
// BSD-3-Clause, Xintao Wang). The result is colour-corrected to the original texture and replaces the image.
// Without the weights file only the edge-directed upscaler is used.

#include <memory>
#include <string>
#include <vector>

#include "../dxvk/dxvk_device.h"

namespace dxvk {

  class D3D9DeviceEx;

  namespace sod {

    struct SrLayer { uint32_t cin, cout, prelu, wOff, bOff, aOff; };

    class SrPass {
    public:
      static constexpr uint32_t kMaxSrcTexels = 512u * 512u;

      static bool LoadWeights(const std::string& path, std::vector<float>& data, std::vector<SrLayer>& layers);

      SrPass(const Rc<DxvkDevice>& device, const std::vector<float>& weights, const std::vector<SrLayer>& layers);
      ~SrPass();

      uint32_t LayerCount() const { return uint32_t(m_layers.size()); }

      // job steps (one job at a time: the activations live in the pass between frames)
      void Start(D3D9DeviceEx* parent, const DxvkBufferSlice& texels, VkDeviceSize bytes, uint32_t w, uint32_t h,
                 uint32_t format, bool opaque);
      void Layers(D3D9DeviceEx* parent, uint32_t w, uint32_t h, uint32_t first, uint32_t count);
      // readback (optional, host-visible): receives the packed result for the disk cache
      void Finish(D3D9DeviceEx* parent, uint32_t w, uint32_t h, uint32_t scale, uint32_t format, bool opaque,
                  const Rc<DxvkImage>& image, const Rc<DxvkBuffer>& readback, VkDeviceSize readbackBytes);
      Rc<DxvkBuffer> MakeReadback(VkDeviceSize size);

    private:
      Rc<DxvkDevice>            m_device;
      const DxvkPipelineLayout* m_layout = nullptr;
      VkPipeline                m_pipe = VK_NULL_HANDLE;
      std::vector<SrLayer>      m_layers;
      std::vector<float>        m_weightData;
      bool                      m_weightsUploaded = false;
      Rc<DxvkBuffer>            m_weights, m_raw, m_act, m_src, m_corr, m_out;
      Rc<DxvkBuffer> MakeBuffer(VkDeviceSize size, const char* name);
    };

  }
}

#include "../util/thread.h"
#include <deque>

namespace dxvk::sod {

  // Disk cache of super-resolved textures (%LOCALAPPDATA%\SoDRT\texcache): a texture seen once is upscaled
  // straight from the cache on every later load, with no pop-in. Files are written by a background thread.
  class TexCache {
  public:
    TexCache();
    ~TexCache();
    bool Enabled() const { return !m_dir.empty(); }
    static uint64_t Key(const void* texels, size_t bytes, uint32_t w, uint32_t h, uint32_t format, uint32_t scale, bool opaque);
    bool Load(uint64_t key, size_t bytes, std::vector<uint8_t>& out) const;
    void Store(uint64_t key, std::vector<uint8_t>&& data);
  private:
    std::string Path(uint64_t key) const;
    void Run();
    std::string                     m_dir;
    dxvk::mutex                     m_mutex;
    dxvk::condition_variable        m_cond;
    std::deque<std::pair<uint64_t, std::vector<uint8_t>>> m_queue;
    bool                            m_stop = false;
    uint64_t                        m_bytes = 0;            // on disk (at start + written)
    dxvk::thread                    m_thread;
  };

}
