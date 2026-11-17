// Shadow HDR (GRAPHIC SETTING HDR). The game draws in standard range: its white wall and a street lamp's glass
// both end at 1.0. What it does say is which draws are light: its glows, halos and light effects are drawn
// additively (destination blend ONE) onto the frame. While HDR is on, every such draw onto the game's frame is drawn
// a second time into a 16-bit float glow image the size of the frame, where overlapping glows add up instead of
// clipping at white. At present (dxvk_swapchain_blitter.cpp runSodHdr) the frame keeps its look at paper white and
// the glow image lifts the light sources above it, towards the display's peak; the swap chain is scRGB.

#include "d3d9_device.h"
#include "d3d9_swapchain.h"
#include "d3d9_texture.h"

namespace dxvk {

  bool D3D9DeviceEx::SodGlowWanted() {
    if (m_sodInternal || m_sodRt.Hdr() <= 0 || !m_state.vertexDecl) return false;
    const auto& rs = m_state.renderStates;
    if (!rs[D3DRS_ALPHABLENDENABLE] || rs[D3DRS_DESTBLEND] != D3DBLEND_ONE || rs[D3DRS_STENCILENABLE]) return false;
    if (rs[D3DRS_COLORWRITEENABLE] == 0) return false;
    // only 3D light draws: pre-transformed (screen-space) draws are the menus, text and HUD, whose additive
    // sparkles and highlights must stay at paper white like the rest of the 2D picture
    if (m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT)) return false;
    // only onto the game's frame (the back buffer), not into its own render-to-texture passes
    D3D9Surface* rt = m_state.renderTargets[0].ptr();
    if (!rt || !m_implicitSwapchain) return false;
    D3D9Surface* bb = m_implicitSwapchain->GetBackBuffer(0);
    return bb && rt == bb;
  }

  bool D3D9DeviceEx::SodGlowEnsure(UINT width, UINT height) {
    if (m_sodGlowTex != nullptr && m_sodGlowW == width && m_sodGlowH == height) return true;
    if (m_sodGlowTex) static_cast<D3D9Texture2D*>(m_sodGlowTex)->ReleasePrivate();
    m_sodGlowTex = nullptr; m_sodGlowW = m_sodGlowH = 0;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &tex, nullptr)) || !tex) {
      if (!m_sodGlowWarned) { m_sodGlowWarned = true; Logger::warn("SoD-RT: HDR: could not create the glow image"); }
      return false;
    }
    static_cast<D3D9Texture2D*>(tex)->AddRefPrivate();
    tex->Release();
    m_sodGlowTex = tex; m_sodGlowW = width; m_sodGlowH = height;
    Logger::info(str::format("SoD-RT: HDR: glow image ", width, "x", height));
    return true;
  }

  void D3D9DeviceEx::SodGlowRedraw(uint32_t api, D3DPRIMITIVETYPE type, UINT a, UINT b, UINT c, UINT d, UINT e,
                                   const void* idata, D3DFORMAT ifmt, const void* vdata, UINT stride) {
    D3D9Surface* rt = m_state.renderTargets[0].ptr();
    VkExtent2D ext = rt->GetSurfaceExtent();
    if (!SodGlowEnsure(ext.width, ext.height)) return;
    IDirect3DSurface9* glow = nullptr;
    if (FAILED(m_sodGlowTex->GetSurfaceLevel(0, &glow)) || !glow) return;
    m_sodInternal = true;
    const uint64_t frame = m_sodRt.Frame();
    if (m_sodGlowFrame != frame) {                      // first glow of the frame: start from black
      ColorFill(glow, nullptr, 0);
      m_sodGlowFrame = frame;
      m_sodGlowDraws = 0;
    }
    IDirect3DSurface9* saved = nullptr;
    GetRenderTarget(0, &saved);
    const D3DVIEWPORT9 vp = m_state.viewport;
    const RECT sc = m_state.scissorRect;
    SetRenderTarget(0, glow);
    SetViewport(&vp);
    SetScissorRect(&sc);
    switch (api) {
      case 0: DrawPrimitive(type, a, b); break;
      case 1: DrawIndexedPrimitive(type, INT(a), b, c, d, e); break;
      case 2: DrawPrimitiveUP(type, a, vdata, stride); break;
      case 3: DrawIndexedPrimitiveUP(type, a, b, c, idata, ifmt, vdata, stride); break;
    }
    if (saved) { SetRenderTarget(0, saved); saved->Release(); }
    SetViewport(&vp);
    SetScissorRect(&sc);
    glow->Release();
    m_sodInternal = false;
    m_sodGlowDraws++;
  }

  Rc<DxvkImageView> D3D9DeviceEx::SodGlowView() {
    // the glow image of the frame being presented (frame counter: PresentEx counts the frame before the swap chain
    // presents, so this frame's glows carry the previous count)
    if (m_sodGlowTex == nullptr || !m_sodGlowReady) return nullptr;
    D3D9CommonTexture* t = static_cast<D3D9Texture2D*>(m_sodGlowTex)->GetCommonTexture();
    return t ? t->GetSampleView(false) : nullptr;
  }

}
