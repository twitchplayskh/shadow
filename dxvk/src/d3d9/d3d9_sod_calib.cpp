// Shadow HDR CALIBRATION screen. SoDFix (dinput8) owns the screen's state and input and saves the values; this
// file draws it over the option menu while it is open, on the menu's own background (640 x 480 units: title band,
// parchment body, help band; SoDFix hides the menu's text meanwhile), and feeds the HDR pass a preview so the sliders show their effect live:
//  - the preview is the last scene played: while the game runs with HDR on, the frame and its glow image (the
//    light draws, see d3d9_sod_hdr.cpp) are copied small every two seconds, and kept in
//    %LOCALAPPDATA%\SoDRT\hdr_preview.bin so the title screen's menu has a scene too;
//  - with no scene yet: a test pattern (grey steps; white patches lit more and more strongly).
// Everything else on the screen has no glow, so text and sliders stay at paper white like every menu.

#include "d3d9_device.h"
#include "d3d9_swapchain.h"
#include "d3d9_texture.h"
#include "d3d9_sod_calib_art.h"

#include <cstdio>
#include <vector>

namespace dxvk {

  namespace {
    constexpr UINT kCapW = 960, kCapH = 720;
    struct Range { int lo, hi; };
    constexpr Range kRanges[5] = { { 60, 300 }, { 400, 2000 }, { 0, 200 }, { 180, 240 }, { 50, 150 } };   // as SoDFix Cal::items
    struct V { float x, y, z, rhw; DWORD c; float u, v; };
    constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    void Hold(IDirect3DTexture9*& slot, IDirect3DTexture9* tex) {       // device-held: private reference only
      static_cast<D3D9Texture2D*>(tex)->AddRefPrivate(); tex->Release(); slot = tex;
    }
    void Drop(IDirect3DTexture9*& slot) {
      if (slot) static_cast<D3D9Texture2D*>(slot)->ReleasePrivate();
      slot = nullptr;
    }
    std::string PreviewPath() {
      char dir[MAX_PATH] = { };
      if (!::GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH)) return std::string();
      std::string d = std::string(dir) + "\\SoDRT";
      ::CreateDirectoryA(d.c_str(), nullptr);
      return d + "\\hdr_preview.bin";
    }
    struct FileHdr { uint32_t magic, w, h; };
    constexpr uint32_t kMagic = 0x50484453u;   // 'SDHP'
  }

  void D3D9DeviceEx::SodCalibRelease() {
    Drop(m_sodCalAtlas); Drop(m_sodCalPar); Drop(m_sodCalColor); Drop(m_sodCalGlow);
    m_sodCalHave = false; m_sodCalIsRT = false; m_sodCalTriedLoad = false;
  }

  // the captured scene -> disk (read back from the GPU; once in a while only)
  bool D3D9DeviceEx::SodCalibSave() {
    if (!m_sodCalHave || !m_sodCalIsRT) return false;
    const std::string path = PreviewPath();
    if (path.empty()) return false;
    bool ok = false;
    IDirect3DSurface9 *c = nullptr, *g = nullptr, *sc = nullptr, *sg = nullptr;
    m_sodCalColor->GetSurfaceLevel(0, &c); m_sodCalGlow->GetSurfaceLevel(0, &g);
    if (c && g
     && SUCCEEDED(CreateOffscreenPlainSurface(kCapW, kCapH, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sc, nullptr))
     && SUCCEEDED(CreateOffscreenPlainSurface(kCapW, kCapH, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sg, nullptr))
     && SUCCEEDED(GetRenderTargetData(c, sc)) && SUCCEEDED(GetRenderTargetData(g, sg))) {
      D3DLOCKED_RECT lc = { }, lg = { };
      if (SUCCEEDED(sc->LockRect(&lc, nullptr, D3DLOCK_READONLY))) {
        if (SUCCEEDED(sg->LockRect(&lg, nullptr, D3DLOCK_READONLY))) {
          if (FILE* f = std::fopen(path.c_str(), "wb")) {
            FileHdr h = { kMagic, kCapW, kCapH };
            std::fwrite(&h, sizeof h, 1, f);
            for (UINT y = 0; y < kCapH; y++) std::fwrite(static_cast<const uint8_t*>(lc.pBits) + size_t(y) * lc.Pitch, 4, kCapW, f);
            for (UINT y = 0; y < kCapH; y++) std::fwrite(static_cast<const uint8_t*>(lg.pBits) + size_t(y) * lg.Pitch, 8, kCapW, f);
            ok = std::fclose(f) == 0;
          }
          sg->UnlockRect();
        }
        sc->UnlockRect();
      }
    }
    if (sc) sc->Release();
    if (sg) sg->Release();
    if (c) c->Release();
    if (g) g->Release();
    Logger::info(str::format("SoD-RT: HDR calibration: preview scene ", ok ? "saved" : "NOT saved"));
    return ok;
  }

  bool D3D9DeviceEx::SodCalibLoad() {
    if (m_sodCalTriedLoad) return false;
    m_sodCalTriedLoad = true;
    const std::string path = PreviewPath();
    FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
    if (!f) return false;
    FileHdr h = { };
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && h.magic == kMagic && h.w == kCapW && h.h == kCapH;
    IDirect3DTexture9 *tc = nullptr, *tg = nullptr;
    ok = ok && SUCCEEDED(CreateTexture(kCapW, kCapH, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &tc, nullptr))
            && SUCCEEDED(CreateTexture(kCapW, kCapH, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED, &tg, nullptr));
    D3DLOCKED_RECT lr = { };
    if (ok && SUCCEEDED(tc->LockRect(0, &lr, nullptr, 0))) {
      for (UINT y = 0; y < kCapH && ok; y++) ok = std::fread(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, 4, kCapW, f) == kCapW;
      tc->UnlockRect(0);
    } else ok = false;
    if (ok && SUCCEEDED(tg->LockRect(0, &lr, nullptr, 0))) {
      for (UINT y = 0; y < kCapH && ok; y++) ok = std::fread(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, 8, kCapW, f) == kCapW;
      tg->UnlockRect(0);
    } else ok = false;
    std::fclose(f);
    if (!ok) { if (tc) tc->Release(); if (tg) tg->Release(); return false; }
    Drop(m_sodCalColor); Drop(m_sodCalGlow);
    Hold(m_sodCalColor, tc); Hold(m_sodCalGlow, tg);
    m_sodCalHave = true; m_sodCalIsRT = false;
    Logger::info("SoD-RT: HDR calibration: preview scene loaded");
    return true;
  }

  // every present, before the glow image is handed to the HDR pass
  void D3D9DeviceEx::SodCalibFrame() {
    if (m_sodInternal || !m_implicitSwapchain) return;
    D3D9Surface* bb = m_implicitSwapchain->GetBackBuffer(0);
    if (!bb) return;
    const VkExtent2D ext = bb->GetSurfaceExtent();
    const bool hdr = m_sodRt.Hdr() > 0;

    const int screen = m_sodRt.MenuScreen();             // 0 none, 1 HDR CALIBRATION, 2 GRAPHIC SETTING
    if (screen != 1 && screen != 2) {
      m_sodCalWasOpen = false;
      // a played scene with lights in it (not while the option menu covers it): copy it small now and then
      if (!hdr || m_sodGlowTex == nullptr || m_sodGlowFrame != m_sodRt.Frame() || m_sodGlowDraws < 4u || m_sodRt.MenuActive()) return;
      if ((++m_sodCalLit % 120u) != 30u) return;
      m_sodInternal = true;
      if (!m_sodCalIsRT) { Drop(m_sodCalColor); Drop(m_sodCalGlow); m_sodCalHave = false; }
      if (!m_sodCalColor) {
        IDirect3DTexture9 *tc = nullptr, *tg = nullptr;
        if (SUCCEEDED(CreateTexture(kCapW, kCapH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &tc, nullptr))
         && SUCCEEDED(CreateTexture(kCapW, kCapH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &tg, nullptr))) {
          Hold(m_sodCalColor, tc); Hold(m_sodCalGlow, tg); m_sodCalIsRT = true;
        } else { if (tc) tc->Release(); if (tg) tg->Release(); }
      }
      if (m_sodCalColor && m_sodCalGlow) {
        IDirect3DSurface9 *c = nullptr, *g = nullptr, *gs = nullptr;
        m_sodCalColor->GetSurfaceLevel(0, &c); m_sodCalGlow->GetSurfaceLevel(0, &g); m_sodGlowTex->GetSurfaceLevel(0, &gs);
        if (c && g && gs && SUCCEEDED(StretchRect(bb, nullptr, c, nullptr, D3DTEXF_LINEAR))
                         && SUCCEEDED(StretchRect(gs, nullptr, g, nullptr, D3DTEXF_LINEAR)))
          m_sodCalHave = true;
        if (c) c->Release();
        if (g) g->Release();
        if (gs) gs->Release();
        if (m_sodCalHave && !m_sodCalSaved && m_sodCalLit >= 600u) { m_sodCalSaved = true; SodCalibSave(); }
      }
      m_sodInternal = false;
      return;
    }

    // ---- the screen is open
    m_sodInternal = true;
    if (screen == 1 && !m_sodCalWasOpen) {
      m_sodCalWasOpen = true;
      if (m_sodCalHave && m_sodCalIsRT) SodCalibSave(); else if (!m_sodCalHave) SodCalibLoad();
      Logger::info(str::format("SoD-RT: HDR calibration screen opened (preview: ", m_sodCalHave ? "scene" : "test pattern", ", HDR ", hdr ? "on" : "off", ")"));
    }
    if (!m_sodCalAtlas) {
      IDirect3DTexture9* t = nullptr;
      D3DLOCKED_RECT lr = { };
      if (SUCCEEDED(CreateTexture(sodcal::kAtlasW, sodcal::kAtlasH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) && t
       && SUCCEEDED(t->LockRect(0, &lr, nullptr, 0))) {
        size_t n = 0; const size_t total = size_t(sodcal::kAtlasW) * sodcal::kAtlasH;
        for (size_t i = 0; i + 1 < sizeof sodcal::kAtlasRle && n < total; i += 2)
          for (uint32_t k = 0; k < sodcal::kAtlasRle[i] && n < total; k++, n++) {
            uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + (n / sodcal::kAtlasW) * lr.Pitch);
            row[n % sodcal::kAtlasW] = (uint32_t(sodcal::kAtlasRle[i + 1]) << 24) | 0x00FFFFFFu;
          }
        t->UnlockRect(0);
        Hold(m_sodCalAtlas, t);
      } else if (t) t->Release();
    }
    if (!m_sodCalPar) {
      IDirect3DTexture9* t = nullptr;
      D3DLOCKED_RECT lr = { };
      if (SUCCEEDED(CreateTexture(sodcal::kParW, sodcal::kParH, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) && t
       && SUCCEEDED(t->LockRect(0, &lr, nullptr, 0))) {
        for (int y = 0; y < sodcal::kParH; y++) {
          uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch);
          for (int x = 0; x < sodcal::kParW; x++) {
            const uint8_t* p = sodcal::kParchment + (size_t(y) * sodcal::kParW + x) * 3;
            row[x] = 0xFF000000u | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
          }
        }
        t->UnlockRect(0);
        Hold(m_sodCalPar, t);
      } else if (t) t->Release();
    }
    if (!m_sodCalAtlas || !m_sodCalPar) { m_sodInternal = false; return; }

    // menu units -> back buffer pixels (a centred 4:3 area)
    const float sc = float(ext.height) / 480.0f, ox = (float(ext.width) - 640.0f * sc) * 0.5f;
    std::vector<V> batch;
    auto quadUV = [&] (float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, DWORD col) {
      const float X0 = ox + x0 * sc - 0.5f, X1 = ox + x1 * sc - 0.5f, Y0 = y0 * sc - 0.5f, Y1 = y1 * sc - 0.5f;
      const V a = { X0, Y0, 0, 1, col, u0, v0 }, b = { X1, Y0, 0, 1, col, u1, v0 }, c = { X0, Y1, 0, 1, col, u0, v1 }, d = { X1, Y1, 0, 1, col, u1, v1 };
      batch.insert(batch.end(), { a, b, c, c, b, d });
    };
    auto fill = [&] (float x0, float y0, float x1, float y1, DWORD col) {            // plain colour (the atlas' white block)
      const float u = 4.0f / sodcal::kAtlasW, v = 4.0f / sodcal::kAtlasH;
      quadUV(x0, y0, x1, y1, u, v, u, v, col);
    };
    auto sprite = [&] (const sodcal::Rect& r, float x, float y, DWORD col) {          // returns the width in units
      const float w = float(r.w) / sodcal::kScale, h = float(r.h) / sodcal::kScale;
      quadUV(x, y, x + w, y + h, float(r.u) / sodcal::kAtlasW, float(r.v) / sodcal::kAtlasH,
        float(r.u + r.w) / sodcal::kAtlasW, float(r.v + r.h) / sodcal::kAtlasH, col);
      return w;
    };
    auto flush = [&] (IDirect3DTexture9* tex) {
      if (batch.empty()) return;
      SetTexture(0, tex);
      DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(batch.size() / 3), batch.data(), sizeof(V));
      batch.clear();
    };

    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) { m_sodInternal = false; return; }
    IDirect3DSurface9* savedRt = nullptr;
    GetRenderTarget(0, &savedRt);
    SetRenderTarget(0, bb);
    D3DVIEWPORT9 vp = { 0, 0, ext.width, ext.height, 0.0f, 1.0f };
    SetViewport(&vp);
    SetVertexShader(nullptr); SetPixelShader(nullptr);
    SetFVF(kFvf);
    for (DWORD i = 1; i < 8; i++) SetTexture(i, nullptr);
    SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE); SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE); SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0); SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    const std::pair<D3DRENDERSTATETYPE, DWORD> rs[] = {
      { D3DRS_ZENABLE, D3DZB_FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
      { D3DRS_ALPHABLENDENABLE, TRUE }, { D3DRS_SRCBLEND, D3DBLEND_SRCALPHA }, { D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA },
      { D3DRS_BLENDOP, D3DBLENDOP_ADD }, { D3DRS_SEPARATEALPHABLENDENABLE, FALSE }, { D3DRS_FOGENABLE, FALSE },
      { D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_LIGHTING, FALSE }, { D3DRS_SPECULARENABLE, FALSE },
      { D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA },
      { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_FILLMODE, D3DFILL_SOLID },
      { D3DRS_STENCILENABLE, FALSE }, { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_SHADEMODE, D3DSHADE_GOURAUD },
    };
    for (const auto& kv : rs) SetRenderState(kv.first, kv.second);

    // texts from SoDFix, drawn glyph by glyph from the atlas' fonts
    auto textW = [&] (const sodcal::Glyph* f, const char* t, float scale) {
      float w = 0.0f;
      for (; *t; t++) { unsigned c = static_cast<unsigned char>(*t); if (c < 32u || c > 126u) c = '?'; w += f[c - 32u].adv / sodcal::kScale * scale; }
      return w;
    };
    auto textN = [&] (const sodcal::Glyph* f, const char* t, size_t len, float x, float y, float scale, DWORD col) {
      for (size_t i = 0; i < len && t[i]; i++) {
        unsigned c = static_cast<unsigned char>(t[i]); if (c < 32u || c > 126u) c = '?';
        const sodcal::Glyph& g = f[c - 32u];
        if (g.w) {
          const float gx = x - g.ox / sodcal::kScale * scale;
          quadUV(gx, y, gx + float(g.w) / sodcal::kScale * scale, y + float(g.h) / sodcal::kScale * scale,
            float(g.u) / sodcal::kAtlasW, float(g.v) / sodcal::kAtlasH, float(g.u + g.w) / sodcal::kAtlasW, float(g.v + g.h) / sodcal::kAtlasH, col);
        }
        x += g.adv / sodcal::kScale * scale;
      }
      return x;
    };
    auto text = [&] (const sodcal::Glyph* f, const char* t, float x, float y, float scale, DWORD col) { return textN(f, t, ~size_t(0), x, y, scale, col); };

    if (screen == 2) {
      // ---- an option page (GRAPHIC, DISPLAY, SOUND, CONTROLLER): every row at once on the left, the selected one
      // explained on the right. Layout as SoDFix (Gs::Pitch / LeftW): rows from y 91, 19 apart with 15 rows, else 30.
      constexpr DWORD kInk = 0xFF14100Cu, kShade = 0x70805030u, kLight = 0xFFF4EEE0u, kGold = 0xFFC8A040u, kBar = 0xEB261A10u, kFoot = 0xFFEFE8DCu;
      const int page = m_sodRt.MenuPage();
      const int rows = std::clamp(m_sodRt.MenuRows(), 1, 15), sel = std::clamp(m_sodRt.MenuRow(), 0, rows - 1);
      const sodcal::Glyph* L = sodcal::kFontLabel; const sodcal::Glyph* H = sodcal::kFontHelp;
      const float pitch = rows > 10 ? 19.0f : page == 2 ? 40.0f : 30.0f, sc2 = rows > 10 ? 0.70f : 0.92f;
      const float leftW = page == 2 ? 362.0f : 336.0f;
      if (page == 3) sprite(sodcal::k_title_gs, 8.0f, 47.0f, 0xFFFFFFFFu);
      else {
        // 4 / 5: SoDFix's REMAP screen for the keyboard / a controller
        static const char* kTitles[6] = { "CONTROLLER", "SOUND SETTING", "DISPLAY SETTING", "", "KEYBOARD CONTROLS", "CONTROLLER BUTTONS" };
        text(L, kTitles[std::clamp(page, 0, 5)], 9.0f, 49.0f, 1.47f, 0xFFFFFFFFu);
      }
      for (int r = 0; r < rows; r++) {
        const float y = 91.0f + pitch * r, ty = y + (pitch - 76.0f / sodcal::kScale * sc2) * 0.5f;
        const bool on = r == sel;
        const int kind = m_sodRt.MenuItem(r, 0);         // 0 text, 1 slider, 2 DOORWAY icons, 3 EXIT
        // DOORWAY EMPHASIS: SoDFix has the game draw the chosen style still at x ~322..354 of this row (the flashing
        // three go to the right-hand panel); our bar stops short of it, as the bar is drawn on top
        // (with a style chosen, SoDFix has the game draw this row's bar under the icon; we add only the gold lines)
        const bool gameBar = kind == 2 && m_sodRt.MenuItem(r, 1) != 3;
        const char* label = m_sodRt.MenuText(0, r); const char* value = m_sodRt.MenuText(1, r);
        if (on) {
          if (!gameBar) fill(4.0f, y, leftW, y + pitch, kBar);
          fill(4.0f, y, leftW, y + 0.8f, kGold); fill(4.0f, y + pitch - 0.8f, leftW, y + pitch, kGold);
        }
        else text(L, label, 10.8f, ty + 0.8f, sc2, kShade);
        text(L, label, 10.0f, ty, sc2, on ? kLight : kInk);
        if (kind == 1) {                                 // slider (SOUND volumes)
          const int v = m_sodRt.MenuItem(r, 1), lo = m_sodRt.MenuItem(r, 2), hi = std::max(m_sodRt.MenuItem(r, 3), lo + 1);
          const float t = std::clamp(float(v - lo) / float(hi - lo), 0.0f, 1.0f);
          const float x0 = 170.0f, x1 = leftW - 40.0f, ys = y + pitch * 0.5f, xt = x0 + (x1 - x0) * t;
          const DWORD done = on ? kGold : kInk;
          fill(x0, ys - 0.8f, x1, ys + 0.8f, on ? 0xFF8A7458u : 0xFF8F7352u);
          fill(x0, ys - 1.6f, xt, ys + 1.6f, done);
          fill(xt - 2.4f, ys - 6.0f, xt + 2.4f, ys + 6.0f, done);
          text(L, value, leftW - 8.0f - textW(L, value, sc2), ty, sc2, on ? kGold : 0xFF4A3018u);
        } else if (*value)
          text(L, value, leftW - 6.0f - textW(L, value, sc2), ty, sc2, on ? kGold : 0xFF4A3018u);
      }
      const float px = leftW + 12.0f, pw = 632.0f - px;
      fill(leftW + 4.0f, 94.0f, leftW + 4.8f, 386.0f, 0x80503020u);
      { const char* label = m_sodRt.MenuText(0, sel); const char* value = m_sodRt.MenuText(1, sel);
        text(L, label, px + 1.0f, 95.0f, 0.92f, kShade);
        text(L, label, px, 94.0f, 0.92f, kInk);
        fill(px, 119.5f, 632.0f, 120.5f, kInk);
        if (*value) text(L, value, 632.0f - textW(L, value, 0.8f), 123.0f, 0.8f, 0xFF6A3A10u);
        // description, wrapped to the panel
        const char* d = m_sodRt.MenuText(2, sel);
        const float maxW = pw, lineH = 19.0f; float y = 150.0f;
        const char* line = d; const char* lastSpace = nullptr; float w = 0.0f;
        for (const char* p = d; ; p++) {
          const bool end = *p == 0;
          if (*p == ' ' || end) {
            const float lw = [&] { float a = 0.0f; for (const char* q = line; q < p; q++) { unsigned c = static_cast<unsigned char>(*q); if (c < 32u || c > 126u) c = '?'; a += H[c - 32u].adv / sodcal::kScale; } return a; }();
            if (lw > maxW && lastSpace) {                 // the line up to the previous space, then carry on after it
              textN(H, line, size_t(lastSpace - line), px, y, 1.0f, kInk); y += lineH;
              line = lastSpace + 1;
            }
            lastSpace = p;
            if (end) { textN(H, line, size_t(p - line), px, y, 1.0f, kInk); break; }
          }
        }
        (void)w; }
      { const char* a = "UP and DOWN : select item      LEFT and RIGHT : change contents";
        const char* b = "confirm : Enter      back : Esc      mouse : click a row to change it";
        if (*m_sodRt.MenuText(3, 0)) { a = m_sodRt.MenuText(3, 0); b = m_sodRt.MenuText(3, 1); }   // SoDFix's own (REMAP)
        text(H, a, 320.0f - textW(H, a, 1.0f) * 0.5f, 409.0f, 1.0f, kFoot);
        text(H, b, 320.0f - textW(H, b, 1.0f) * 0.5f, 443.0f, 1.0f, kFoot); }
      flush(m_sodCalAtlas);
      SetRenderTarget(0, savedRt ? savedRt : static_cast<IDirect3DSurface9*>(bb));
      if (savedRt) savedRt->Release();
      sb->Apply();
      sb->Release();
      m_sodInternal = false;
      return;
    }

    const int sel = std::clamp(m_sodRt.CalibRow(), 0, 6);
    constexpr DWORD kInk = 0xFF14100Cu, kShade = 0x70805030u, kLight = 0xFFF4EEE0u, kGold = 0xFFC8A040u, kBar = 0xEB261A10u;
    constexpr float kPx0 = 14.0f, kPy0 = 100.0f, kPx1 = 294.0f, kPy1 = 310.0f;      // preview

    // 1. the background is the option menu's own (SoDFix moves the menu's text and cursor off screen while the
    //    calibration screen is open): title band art, parchment, help band
    // 2. preview: the scene, or the test pattern
    if (m_sodCalHave && m_sodCalColor) {
      SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      quadUV(kPx0, kPy0, kPx1, kPy1, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu);
      flush(m_sodCalColor);
      SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    } else {
      fill(kPx0, kPy0, kPx1, kPy1, 0xFF000000u);
      const float pw = (kPx1 - kPx0) / 8.0f, ph = (kPy1 - kPy0) * 0.5f;
      for (int i = 0; i < 8; i++) { const DWORD g = DWORD(i * 255 / 7); fill(kPx0 + i * pw, kPy0, kPx0 + (i + 1) * pw, kPy0 + ph, 0xFF000000u | g << 16 | g << 8 | g); }
      const float qw = (kPx1 - kPx0) / 6.0f;
      for (int i = 0; i < 6; i++) fill(kPx0 + i * qw + 3.0f, kPy0 + ph + 8.0f, kPx0 + (i + 1) * qw - 3.0f, kPy1 - 8.0f, 0xFFFFFFFFu);
    }

    // 3. everything else
    fill(kPx0 - 1.5f, kPy0 - 1.5f, kPx1 + 1.5f, kPy0, kInk); fill(kPx0 - 1.5f, kPy1, kPx1 + 1.5f, kPy1 + 1.5f, kInk);
    fill(kPx0 - 1.5f, kPy0, kPx0, kPy1, kInk);               fill(kPx1, kPy0, kPx1 + 1.5f, kPy1, kInk);
    sprite(sodcal::k_title, 8.0f, 47.0f, 0xFFFFFFFFu);
    sprite(m_sodCalHave ? sodcal::k_capscene : sodcal::k_cappat, kPx0, kPy1 + 6.0f, 0xFF3A2A1Cu);
    for (int r = 0; r < 7; r++) {
      const float y = 98.0f + 40.0f * r, x = 312.0f, xr = 624.0f;
      const bool on = r == sel;
      if (on) {
        fill(304.0f, y - 3.0f, 634.0f, y + (r < 5 ? 36.0f : 28.0f), kBar);
        fill(304.0f, y - 3.0f, 634.0f, y - 2.0f, kGold); fill(304.0f, y + (r < 5 ? 35.0f : 27.0f), 634.0f, y + (r < 5 ? 36.0f : 28.0f), kGold);
      } else sprite(sodcal::kLabels[r], x + 1.2f, y + 1.2f, kShade);
      const DWORD ink = on ? kLight : kInk;
      sprite(sodcal::kLabels[r], x, y, ink);
      if (r >= 5) continue;
      // value, right-aligned
      const int val = std::clamp(m_sodRt.HdrVal(r == 0 ? 0 : r == 1 ? 1 : r + 1), kRanges[r].lo, kRanges[r].hi);
      char txt[16];
      if (r == 3) std::snprintf(txt, sizeof txt, "%d.%02d", val / 100, val % 100);
      else std::snprintf(txt, sizeof txt, r < 2 ? "%d" : "%d%%", val);
      auto glyph = [] (char ch) -> const sodcal::Rect& { return ch == '.' ? sodcal::k_dot : ch == '%' ? sodcal::k_pct : sodcal::kDigits[(ch - '0') % 10]; };
      const float pad = 4.0f / sodcal::kScale;
      float w = 0.0f;
      for (const char* p = txt; *p; p++) w += float(glyph(*p).w) / sodcal::kScale - pad;
      if (r < 2) w += float(sodcal::k_nits.w) / sodcal::kScale + 2.0f;
      float cx = xr - w;
      for (const char* p = txt; *p; p++) cx += sprite(glyph(*p), cx, y, ink) - pad;
      if (r < 2) sprite(sodcal::k_nits, cx + 4.0f, y, ink);
      // slider
      const float t = float(val - kRanges[r].lo) / float(kRanges[r].hi - kRanges[r].lo);
      const float x0 = x + 2.0f, x1 = xr - 2.0f, ys = y + 28.5f, xt = x0 + (x1 - x0) * t;
      const DWORD track = on ? 0xFF8A7458u : 0xFF8F7352u, done = on ? kGold : kInk;
      fill(x0, ys - 0.8f, x1, ys + 0.8f, track);
      fill(x0, ys - 1.4f, xt, ys + 1.4f, done);
      fill(xt - 2.2f, ys - 4.5f, xt + 2.2f, ys + 4.5f, done);
    }
    { const sodcal::Rect& h = sodcal::kHelps[sel];
      sprite(h, 320.0f - float(h.w) / sodcal::kScale * 0.5f, 409.0f, 0xFFEFE8DCu);
      sprite(sodcal::k_keys, 320.0f - float(sodcal::k_keys.w) / sodcal::kScale * 0.5f, 443.0f, 0xFFEFE8DCu); }
    flush(m_sodCalAtlas);

    // 4. the preview's light into the glow image; the rest of the screen has none
    if (hdr && SodGlowEnsure(ext.width, ext.height)) {
      IDirect3DSurface9* glow = nullptr;
      if (SUCCEEDED(m_sodGlowTex->GetSurfaceLevel(0, &glow)) && glow) {
        ColorFill(glow, nullptr, 0);
        SetRenderTarget(0, glow);
        SetViewport(&vp);
        if (m_sodCalHave && m_sodCalGlow) {
          SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
          quadUV(kPx0, kPy0, kPx1, kPy1, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu);
          flush(m_sodCalGlow);
        } else {                                       // test pattern: patch i lit i times over
          SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE); SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
          const float ph = (kPy1 - kPy0) * 0.5f, qw = (kPx1 - kPx0) / 6.0f;
          for (int i = 1; i < 6; i++) for (int n = 0; n < i; n++)
            fill(kPx0 + i * qw + 3.0f, kPy0 + ph + 8.0f, kPx0 + (i + 1) * qw - 3.0f, kPy1 - 8.0f, 0xFFFFFFFFu);
          flush(m_sodCalAtlas);
        }
        glow->Release();
        m_sodGlowFrame = m_sodRt.Frame();
        m_sodGlowDraws = 1;
      }
    }
    SetRenderTarget(0, savedRt ? savedRt : static_cast<IDirect3DSurface9*>(bb));
    if (savedRt) savedRt->Release();
    sb->Apply();
    sb->Release();
    m_sodInternal = false;
  }

}
