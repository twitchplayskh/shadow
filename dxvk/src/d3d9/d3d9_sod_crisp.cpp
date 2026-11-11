// CRISP TEXT: the game's text images are small 4-bit sheets of white letters with a dark outline baked around them.
// Enlarged four to five times for a 4K screen, the outline turns into dark smudges and the letters into blurry
// blocks. Each listed sheet is redrawn once, from its own pixels: the white core of every letter (the outline
// dropped) is enlarged eight times with a smooth curve and cut with a narrow edge, so the lettering keeps exactly
// its shape and place on the sheet but comes out as clean white text with smooth edges. The redrawn sheet is used
// in place of the game's for its draws (same texture coordinates; it is just bigger), with mipmaps and smooth
// filtering. The game's sheet itself is never changed. SODRT_CRISPTEXT=0 turns it off.
#include "d3d9_device.h"
#include "d3d9_texture.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <atomic>
#include <string>

namespace dxvk {

  namespace {
    // texture table indices of the outlined lettering: subtitle font (fonte_all / fontu_all), clock digits
    // (timefont), place names (placename), the "Time passed" counter (ccs_font). Every other text image the game
    // has (menus, settings, help, save screens, credits...: SoDFix\text_textures.txt) is redrawn too, by kind.
    const int kFontIds[] = { 77, 488, 978, 833 };
    bool IsFontId(int id) { for (int f : kFontIds) if (f == id) return true; return false; }

    template <typename F> void ParallelRows(UINT n, F&& f) {
      const UINT t = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
      if (n < 64 || t == 1) { for (UINT y = 0; y < n; y++) f(y); return; }
      std::vector<std::thread> th;
      std::atomic<UINT> next { 0 };
      for (UINT k = 0; k < t; k++) th.emplace_back([&] { for (UINT y; (y = next.fetch_add(1)) < n; ) f(y); });
      for (auto& x : th) x.join();
    }

    float Cubic(float a, float b, float c, float d, float t) {        // Catmull-Rom
      return b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
    }
  }


  IDirect3DTexture9* D3D9DeviceEx::SodCrispFor(IDirect3DBaseTexture9* tex) {
    static const bool off = [] { char v[8] = { }; return ::GetEnvironmentVariableA("SODRT_CRISPTEXT", v, 8) && v[0] == '0'; }();
    if (off || !tex || tex->GetType() != D3DRTYPE_TEXTURE) return nullptr;
    // which textures the listed table entries hold right now: looked up once per frame (three entries)
    const uint64_t frame = m_sodRt.Frame();
    if (frame != m_sodCrispFrame) {
      m_sodCrispFrame = frame;
      m_sodCrispIds.clear();
      if (!m_sodCrispTried) {
        m_sodCrispTried = true;
        HMODULE fix = ::GetModuleHandleA("dinput8.dll"), d3d8 = ::GetModuleHandleA("d3d8.dll");
        if (fix && !m_sodSsrGetTable) m_sodSsrGetTable = reinterpret_cast<uintptr_t(*)()>(::GetProcAddress(fix, "SoDFix_GetTextureTable"));
        if (d3d8 && !m_sodSsrTex9) m_sodSsrTex9 = reinterpret_cast<void*(__stdcall*)(void*)>(::GetProcAddress(d3d8, "DXVK_SoD_D3D9Texture"));
        // the text images: the menu line takes every id; a file's line (after its "# name.tbn2" comment) only its
        // page id, the first number (the rest are image ids inside the file, not table entries)
        for (int f : kFontIds) m_sodCrispList.push_back(f);
        char path[MAX_PATH] = { };
        ::GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (char* sl = std::strrchr(path, '\\')) sl[1] = 0;
        std::ifstream tf(std::string(path) + "SoDFix\\text_textures.txt");
        std::string line; bool fileLine = false;
        while (std::getline(tf, line)) {
          if (line.empty()) continue;
          if (line[0] == '#') { fileLine = line.find(".tbn2") != std::string::npos; continue; }
          std::istringstream ss(line); int id;
          while (ss >> id) {
            if (id >= -16384 && id <= 16383 && std::find(m_sodCrispList.begin(), m_sodCrispList.end(), id) == m_sodCrispList.end())
              m_sodCrispList.push_back(id);
            if (fileLine) break;
          }
        }
        Logger::info(str::format("SoD-RT: crisp text ", (m_sodSsrGetTable && m_sodSsrTex9) ? "on" : "unavailable (no texture table)",
          ", ", m_sodCrispList.size(), " text images"));
      }
      const uintptr_t table = (m_sodSsrGetTable && m_sodSsrTex9) ? m_sodSsrGetTable() : 0;
      if (table) {
        for (int id : m_sodCrispList) {
          void* t8 = *reinterpret_cast<void* const*>(table + intptr_t(id) * 40 + 0x10);
          if (!t8) continue;
          if (void* t9 = m_sodSsrTex9(t8))
            m_sodCrispIds.push_back({ id, static_cast<IDirect3DBaseTexture9*>(static_cast<IDirect3DTexture9*>(t9)) });
        }
      }
      // redrawn sheets not used for a while give their memory back once the total passes 384 MB
      size_t total = 0;
      for (auto& kv : m_sodCrisp) total += kv.second.bytes;
      while (total > (384u << 20)) {
        auto old = m_sodCrisp.end();
        for (auto it = m_sodCrisp.begin(); it != m_sodCrisp.end(); ++it)
          if (it->second.used + 2u < frame && (old == m_sodCrisp.end() || it->second.used < old->second.used)) old = it;
        if (old == m_sodCrisp.end()) break;
        total -= old->second.bytes; m_sodCrisp.erase(old);
      }
    }
    int id = 0;
    for (auto& p : m_sodCrispIds) if (p.second == tex) { id = p.first; break; }
    m_sodCrispLastId = id;
    if (!id) return nullptr;
    SodCrispTex& c = m_sodCrisp[tex];
    c.used = frame;
    // redrawn when first seen and again if the game puts other pixels in the sheet (checked every 2 seconds)
    if (!c.hi || c.id != id || frame - c.checked >= 120u) {
      c.checked = frame; c.id = id;
      if (!SodCrispBuild(static_cast<IDirect3DTexture9*>(tex), c)) { m_sodCrisp.erase(tex); return nullptr; }
    }
    return c.hi.ptr();
  }


  bool D3D9DeviceEx::SodCrispBuild(IDirect3DTexture9* src, SodCrispTex& c) {
    D3DSURFACE_DESC desc = { };
    if (FAILED(src->GetLevelDesc(0, &desc))) return false;
    const UINT w = desc.Width, h = desc.Height;
    if (w < 8 || h < 8 || w > 512 || h > 512) return false;
    if (std::min(w, h) < 8 && !IsFontId(c.id)) return false;
    D3DLOCKED_RECT lr = { };
    if (FAILED(src->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) return false;
    std::vector<float> rgba(size_t(w) * h * 4);
    bool fmtOk = true;
    uint64_t hash = 1469598103934665603ull;
    for (UINT y = 0; y < h && fmtOk; y++) {
      const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
      const size_t rowBytes = (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8) ? size_t(w) * 4 : size_t(w) * 2;
      for (size_t k = 0; k < rowBytes; k++) { hash ^= row[k]; hash *= 1099511628211ull; }
      for (UINT x = 0; x < w; x++) {
        float* o = &rgba[(size_t(y) * w + x) * 4];
        switch (desc.Format) {
          case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: {
            const uint8_t* p = row + x * 4;
            o[0] = p[2] / 255.0f; o[1] = p[1] / 255.0f; o[2] = p[0] / 255.0f;
            o[3] = desc.Format == D3DFMT_X8R8G8B8 ? 1.0f : p[3] / 255.0f; break; }
          case D3DFMT_A4R4G4B4: {
            uint16_t v; std::memcpy(&v, row + x * 2, 2);
            o[0] = ((v >> 8) & 15) / 15.0f; o[1] = ((v >> 4) & 15) / 15.0f; o[2] = (v & 15) / 15.0f; o[3] = (v >> 12) / 15.0f; break; }
          case D3DFMT_A1R5G5B5: {
            uint16_t v; std::memcpy(&v, row + x * 2, 2);
            o[0] = ((v >> 10) & 31) / 31.0f; o[1] = ((v >> 5) & 31) / 31.0f; o[2] = (v & 31) / 31.0f; o[3] = (v >> 15) ? 1.0f : 0.0f; break; }
          default: fmtOk = false; break;
        }
      }
    }
    src->UnlockRect(0);
    if (!fmtOk) {
      if (m_sodCrispLogged.insert(c.id).second)
        Logger::warn(str::format("SoD-RT: crisp text: sheet ", c.id, " format ", uint32_t(desc.Format), " not handled"));
      return false;
    }
    if (c.hi && hash == c.hash) return true;                       // unchanged

    // the letters' white core: bright and opaque; the dark outline and the transparent background count as nothing.
    // The colour of the core is kept (the sheets are white; a tinted letter stays tinted).
    std::vector<float> core(size_t(w) * h), col(size_t(w) * h * 3, 1.0f);
    for (size_t i = 0; i < size_t(w) * h; i++) {
      const float* p = &rgba[i * 4];
      const float lum = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
      const float t = std::min(std::max((lum - 0.30f) / 0.45f, 0.0f), 1.0f);
      core[i] = p[3] * t * t * (3.0f - 2.0f * t);
      if (core[i] > 0.5f) { const float m = std::max(std::max(p[0], p[1]), std::max(p[2], 1e-3f)); for (int k = 0; k < 3; k++) col[i * 3 + k] = p[k] / m; }
    }
    // which kind of sheet: outlined lettering (white letters with a dark outline or halo baked around them, nothing
    // else on the sheet) is redrawn as clean white letters, outline dropped; everything else (menu and settings
    // labels, help pages, save screens, credits, logos; sheets with pictures or artwork) keeps its look exactly and is
    // only enlarged with sharp edges
    int mode = 1;
    if (!IsFontId(c.id)) {
      size_t nCore = 0, nHalo = 0, nArt = 0, nOpaque = 0;
      std::vector<uint8_t> isCore(size_t(w) * h, 0), opaque(size_t(w) * h, 0);
      for (size_t i = 0; i < size_t(w) * h; i++) {
        const float* p = &rgba[i * 4];
        const float lum = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
        const float sat = std::max(std::max(p[0], p[1]), p[2]) - std::min(std::min(p[0], p[1]), p[2]);
        if (p[3] > 0.5f && lum > 0.7f && sat < 0.15f) { isCore[i] = 1; nCore++; }
        if (p[3] > 0.9f) { opaque[i] = 1; nOpaque++; if (sat > 0.2f) nArt++; }
      }
      for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++) {
        const float* p = &rgba[(size_t(y) * w + x) * 4];
        if (!(p[3] > 0.3f && 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2] < 0.3f)) continue;
        bool nearCore = false;
        for (int dy = -2; dy <= 2 && !nearCore; dy++) for (int dx = -2; dx <= 2; dx++) {
          const int sx = int(x) + dx, sy = int(y) + dy;
          if (sx >= 0 && sy >= 0 && sx < int(w) && sy < int(h) && isCore[size_t(sy) * w + sx]) { nearCore = true; break; }
        }
        if (nearCore) nHalo++;
      }
      // the largest solid patch (a screenshot, a background, a panel)
      size_t largest = 0;
      std::vector<uint32_t> stack;
      for (size_t i0 = 0; i0 < size_t(w) * h; i0++) {
        if (opaque[i0] != 1) continue;
        size_t n = 0; stack.assign(1, uint32_t(i0)); opaque[i0] = 2;
        while (!stack.empty()) {
          const uint32_t i = stack.back(); stack.pop_back(); n++;
          const UINT x = i % w, y = i / w;
          const uint32_t nb[4] = { x > 0 ? i - 1 : ~0u, x + 1 < w ? i + 1 : ~0u, y > 0 ? i - w : ~0u, y + 1 < h ? i + w : ~0u };
          for (uint32_t j : nb) if (j != ~0u && opaque[j] == 1) { opaque[j] = 2; stack.push_back(j); }
        }
        largest = std::max(largest, n);
      }
      const float halo = float(nHalo) / float(std::max<size_t>(nCore, 1)), art = float(nArt) / float(size_t(w) * h);
      const float opq = float(nOpaque) / float(size_t(w) * h);
      mode = (halo > 0.8f && art < 0.02f && largest < 1500u && opq < 0.35f && nCore > 20u) ? 3 : 2;
      if (m_sodCrispLogged.insert(c.id + 200000).second)
        Logger::info(str::format("SoD-RT: crisp text: sheet ", c.id, " (", w, "x", h, "): ", mode == 3 ? "outlined lettering, outline dropped" : "kept style, sharp edges",
          " (halo ", halo, ", art ", art, ", opaque ", opq, ", largest patch ", largest, ")"));
    }
    c.mode = mode;

    auto at = [&] (const std::vector<float>& v, int x, int y) {
      x = std::min(std::max(x, 0), int(w) - 1); y = std::min(std::max(y, 0), int(h) - 1); return v[size_t(y) * w + x]; };

    const UINT K = std::min<UINT>(8u, 2048u / std::max(w, h));
    const UINT W = w * K, H = h * K;
    IDirect3DTexture9* hi = nullptr;
    if (FAILED(CreateTexture(W, H, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &hi, nullptr)) || !hi) return false;
    Com<IDirect3DTexture9> hiRef = hi;
    hi->Release();

    std::vector<float> a0(size_t(W) * H);
    std::vector<uint8_t> c0(size_t(W) * H * 3, 255);
    // a sheet redrawn ahead from a real typeface (SoDFix\crisp\<id>.a8: width, height, then coverage bytes for the
    // whole page at 8x): used as it is when it fits this sheet
    bool drawn = false, ownColour = false;
    if (mode == 2 || mode == 3) {
      // KEPT STYLE: every channel (premultiplied) enlarged with a smooth curve, then pulled toward the darkest or
      // brightest of its four source pixels with an S-curve: edges come out sharp, flat areas and gradients stay
      // as they were, and nothing new (no colour, no outline) appears
      std::vector<float> pm(size_t(w) * h * 4);
      // (outlined lettering: only the letters' own colour and coverage go in, the dark outline drops out)
      for (size_t i = 0; i < size_t(w) * h; i++) {
        const float a = mode == 3 ? core[i] : rgba[i * 4 + 3];
        for (int k = 0; k < 3; k++) pm[i * 4 + k] = (mode == 3 ? col[i * 3 + k] : rgba[i * 4 + k]) * a;
        pm[i * 4 + 3] = a;
      }
      auto px = [&] (int x, int y, int k) { x = std::min(std::max(x, 0), int(w) - 1); y = std::min(std::max(y, 0), int(h) - 1); return pm[(size_t(y) * w + x) * 4 + k]; };
      ParallelRows(H, [&] (UINT Y) {
        const float fy = (Y + 0.5f) / float(K) - 0.5f; const int y0 = int(std::floor(fy)); const float ty = fy - y0;
        for (UINT X = 0; X < W; X++) {
          const float fx = (X + 0.5f) / float(K) - 0.5f; const int x0 = int(std::floor(fx)); const float tx = fx - x0;
          float o[4];
          for (int k = 0; k < 4; k++) {
            float r[4];
            for (int j = 0; j < 4; j++)
              r[j] = Cubic(px(x0 - 1, y0 - 1 + j, k), px(x0, y0 - 1 + j, k), px(x0 + 1, y0 - 1 + j, k), px(x0 + 2, y0 - 1 + j, k), tx);
            const float v = Cubic(r[0], r[1], r[2], r[3], ty);
            const float n0 = px(x0, y0, k), n1 = px(x0 + 1, y0, k), n2 = px(x0, y0 + 1, k), n3 = px(x0 + 1, y0 + 1, k);
            const float lo = std::min(std::min(n0, n1), std::min(n2, n3)), hi = std::max(std::max(n0, n1), std::max(n2, n3)), d = hi - lo;
            if (d > 0.02f) { float t = std::min(std::max((v - lo) / d, 0.0f), 1.0f); t = t * t * (3.0f - 2.0f * t); t = t * t * (3.0f - 2.0f * t); o[k] = lo + d * t; }
            else o[k] = std::min(std::max(v, 0.0f), 1.0f);
          }
          const size_t i = size_t(Y) * W + X;
          const float a = std::min(std::max(o[3], 0.0f), 1.0f);
          a0[i] = a;
          for (int k = 0; k < 3; k++) c0[i * 3 + k] = uint8_t(std::lround(std::min(a > 1e-4f ? o[k] / a : 0.0f, 1.0f) * 255.0f));
        }
      });
      drawn = true; ownColour = true;
    }
    if (mode == 1) {
      char path[MAX_PATH] = { };
      ::GetModuleFileNameA(nullptr, path, MAX_PATH);
      if (char* sl = std::strrchr(path, '\\')) sl[1] = 0;
      std::ifstream f(std::string(path) + "SoDFix\\crisp\\" + std::to_string(c.id) + ".a8", std::ios::binary);
      uint32_t fw = 0, fh = 0;
      if (f && f.read(reinterpret_cast<char*>(&fw), 4) && f.read(reinterpret_cast<char*>(&fh), 4) && fw == W && fh >= H && fh <= 4096) {
        std::vector<uint8_t> buf(size_t(fw) * fh);
        if (f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()))) {
          // the file may hold a taller page than the game's texture (e.g. accented letters above the main set):
          // the part that matches the game's own letters is used. Each candidate (every source row) is scored by
          // how well the file's coverage, averaged back to source pixels, follows the sheet's letter cores.
          const UINT rows = fh / K;
          float bestScore = -1e30f; UINT bestRow = 0;
          struct Part { UINT y, h, from; };
          std::vector<Part> drawnRows;
          std::vector<float> down(size_t(w) * rows, 0.0f);
          for (UINT y = 0; y < rows; y++) for (UINT x = 0; x < w; x++) {
            float acc = 0.0f;
            for (UINT dy = 0; dy < K; dy++) for (UINT dx = 0; dx < K; dx++) acc += buf[(size_t(y) * K + dy) * fw + size_t(x) * K + dx];
            down[size_t(y) * w + x] = acc / (255.0f * K * K);
          }
          // matched in bands of 64 source rows, each on its own (the halves of a page may be stacked either way)
          const UINT band = std::min<UINT>(64u, h);
          UINT matched = 0, bands = 0;
          for (UINT b0 = 0; b0 < h; b0 += band) {
            const UINT bh = std::min(band, h - b0); bands++;
            float sa = 0, saa = 0;
            for (UINT y = 0; y < bh; y++) for (UINT x = 0; x < w; x++) { const float a = core[size_t(b0 + y) * w + x]; sa += a; saa += a * a; }
            const float n = float(w) * bh, va = saa / n - (sa / n) * (sa / n);
            if (va < 1e-4f) { matched++; drawnRows.push_back({ b0, bh, ~0u }); continue; }     // empty band: nothing to draw
            for (UINT o = 0; o + bh <= rows; o++) {
              float sab = 0, sbb = 0, sb = 0;
              for (UINT y = 0; y < bh; y++) for (UINT x = 0; x < w; x++) {
                const float a = core[size_t(b0 + y) * w + x], bb = down[size_t(o + y) * w + x];
                sab += a * bb; sbb += bb * bb; sb += bb;
              }
              const float cov = sab / n - (sa / n) * (sb / n), vb = sbb / n - (sb / n) * (sb / n);
              const float score = cov / std::sqrt(std::max(va * vb, 1e-12f));
              if (score > bestScore) { bestScore = score; bestRow = o; }
            }
            if (bestScore > 0.6f) { matched++; drawnRows.push_back({ b0, bh, bestRow }); }
            bestScore = -1e30f;
          }
          if (matched == bands) {
            for (auto& d : drawnRows)
              for (UINT y = 0; y < d.h * K; y++) for (UINT x = 0; x < W; x++)
                a0[(size_t(d.y) * K + y) * W + x] = d.from == ~0u ? 0.0f : buf[(size_t(d.from) * K + y) * fw + x] / 255.0f;
            drawn = true;
          }
          if (m_sodCrispLogged.insert(c.id + 100000).second)
            {
            std::string where;
            for (auto& d : drawnRows) where += " " + std::to_string(d.y) + "<-" + (d.from == ~0u ? std::string("empty") : std::to_string(d.from));
            Logger::info(str::format("SoD-RT: crisp text: sheet ", c.id, " (", w, "x", h, ") ", drawn ? "from" : "does not match",
              " SoDFix\\crisp\\", c.id, ".a8 (rows", where, ")"));
          }
        }
      }
    }
    if (!drawn) {
      // the core, lightly smoothed (the sheets are stepped 4-bit pixels), enlarged with a smooth curve, then cut at
      // a fixed level with an edge about one texel wide wherever the letter boundary falls
      std::vector<float> tmp(size_t(w) * h), sm(size_t(w) * h);
      const float g[5] = { 0.0175f, 0.2193f, 0.5264f, 0.2193f, 0.0175f };          // gaussian, sigma 0.5
      for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++) {
        float acc = 0; for (int k = -2; k <= 2; k++) acc += g[k + 2] * at(core, int(x) + k, int(y)); tmp[size_t(y) * w + x] = acc; }
      for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++) {
        float acc = 0; for (int k = -2; k <= 2; k++) acc += g[k + 2] * at(tmp, int(x), int(y) + k); sm[size_t(y) * w + x] = acc; }
      std::vector<float> v(size_t(W) * H);
      ParallelRows(H, [&] (UINT Y) {
        const float fy = (Y + 0.5f) / float(K) - 0.5f; const int y0 = int(std::floor(fy)); const float ty = fy - y0;
        for (UINT X = 0; X < W; X++) {
          const float fx = (X + 0.5f) / float(K) - 0.5f; const int x0 = int(std::floor(fx)); const float tx = fx - x0;
          float r[4];
          for (int j = 0; j < 4; j++)
            r[j] = Cubic(at(sm, x0 - 1, y0 - 1 + j), at(sm, x0, y0 - 1 + j), at(sm, x0 + 1, y0 - 1 + j), at(sm, x0 + 2, y0 - 1 + j), tx);
          v[size_t(Y) * W + X] = Cubic(r[0], r[1], r[2], r[3], ty);
        }
      });
      for (UINT Y = 0; Y < H; Y++) for (UINT X = 0; X < W; X++) {
        const size_t i = size_t(Y) * W + X;
        const float gx = 0.5f * (v[size_t(Y) * W + std::min(X + 1, W - 1)] - v[size_t(Y) * W + (X ? X - 1 : 0)]);
        const float gy = 0.5f * (v[size_t(std::min(Y + 1, H - 1)) * W + X] - v[size_t(Y ? Y - 1 : 0) * W + X]);
        const float gl = std::sqrt(gx * gx + gy * gy) + 1e-4f;
        a0[i] = std::min(std::max((v[i] - 0.48f) / gl + 0.5f, 0.0f), 1.0f);
      }
    }
    // tinted letters keep their colour: each texel takes the colour of the nearby source pixel with the most core
    if (!ownColour) ParallelRows(H, [&] (UINT Y) {
      const int by = std::min(std::max(int(Y / K), 0), int(h) - 1);
      for (UINT X = 0; X < W; X++) {
        const int bx = std::min(std::max(int(X / K), 0), int(w) - 1);
        float best = -1.0f; int sx0 = bx, sy0 = by;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
          const int sx = std::min(std::max(bx + dx, 0), int(w) - 1), sy = std::min(std::max(by + dy, 0), int(h) - 1);
          const float cv = core[size_t(sy) * w + sx] - 0.05f * float(dx * dx + dy * dy);
          if (cv > best) { best = cv; sx0 = sx; sy0 = sy; }
        }
        for (int k = 0; k < 3; k++) c0[(size_t(Y) * W + X) * 3 + k] = uint8_t(std::lround(col[(size_t(sy0) * w + sx0) * 3 + k] * 255.0f));
      }
    });
    // every level: colour with coverage as alpha; smaller levels average the one above (premultiplied)
    std::vector<float> pa = a0, pc(size_t(W) * H * 3);
    for (size_t i = 0; i < size_t(W) * H; i++) for (int k = 0; k < 3; k++) pc[i * 3 + k] = c0[i * 3 + k] / 255.0f * a0[i];
    UINT lw = W, lh = H;
    const DWORD levels = hi->GetLevelCount();
    for (DWORD L = 0; L < levels; L++) {
      if (L > 0) {
        const UINT nw = std::max(lw / 2, 1u), nh = std::max(lh / 2, 1u);
        std::vector<float> na(size_t(nw) * nh), nc(size_t(nw) * nh * 3);
        for (UINT y = 0; y < nh; y++) for (UINT x = 0; x < nw; x++) {
          float s = 0, sc[3] = { };
          for (UINT dy = 0; dy < 2; dy++) for (UINT dx = 0; dx < 2; dx++) {
            const size_t i = size_t(std::min(2 * y + dy, lh - 1)) * lw + std::min(2 * x + dx, lw - 1);
            s += pa[i]; for (int k = 0; k < 3; k++) sc[k] += pc[i * 3 + k];
          }
          na[size_t(y) * nw + x] = s * 0.25f; for (int k = 0; k < 3; k++) nc[(size_t(y) * nw + x) * 3 + k] = sc[k] * 0.25f;
        }
        pa.swap(na); pc.swap(nc); lw = nw; lh = nh;
      }
      D3DLOCKED_RECT d = { };
      if (FAILED(hi->LockRect(L, &d, nullptr, 0))) return false;
      for (UINT y = 0; y < lh; y++) {
        uint8_t* row = static_cast<uint8_t*>(d.pBits) + size_t(y) * d.Pitch;
        for (UINT x = 0; x < lw; x++) {
          const size_t i = size_t(y) * lw + x;
          const float a = pa[i];
          float rgb[3];
          for (int k = 0; k < 3; k++) rgb[k] = a > 1e-4f ? std::min(pc[i * 3 + k] / a, 1.0f) : 1.0f;
          row[x * 4 + 0] = uint8_t(std::lround(rgb[2] * 255.0f)); row[x * 4 + 1] = uint8_t(std::lround(rgb[1] * 255.0f));
          row[x * 4 + 2] = uint8_t(std::lround(rgb[0] * 255.0f)); row[x * 4 + 3] = uint8_t(std::lround(std::min(a, 1.0f) * 255.0f));
        }
      }
      hi->UnlockRect(L);
    }
    c.hi = hiRef; c.hash = hash; c.bytes = size_t(W) * H * 4 * 4 / 3 * 2;    // texture + its system copy, with mipmaps
    if (m_sodCrispLogged.insert(c.id).second)
      Logger::info(str::format("SoD-RT: crisp text: sheet ", c.id, " (", w, "x", h, ") redrawn at ", W, "x", H));
    return true;
  }


  D3D9DeviceEx::SodCrispScope::SodCrispScope(D3D9DeviceEx* d) {
    if (d->m_sodInternal || d->m_state.pixelShader.ptr()) return;
    IDirect3DBaseTexture9* cur = d->m_state.textures[0];
    if (!cur) return;
    IDirect3DTexture9* hi = d->SodCrispFor(cur);
    if (!hi) return;
    dev = d; tex = cur; id = d->m_sodCrispLastId;
    static const D3DSAMPLERSTATETYPE kSs[5] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_MAXMIPLEVEL, D3DSAMP_MIPMAPLODBIAS };
    for (int k = 0; k < 5; k++) d->GetSamplerState(0, kSs[k], &ss[k]);
    const bool internal = d->m_sodInternal;
    d->m_sodInternal = true;
    d->SetTexture(0, hi);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR); d->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
    d->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
    d->m_sodInternal = internal;
  }


  D3D9DeviceEx::SodCrispScope::~SodCrispScope() {
    if (!dev) return;
    static const D3DSAMPLERSTATETYPE kSs[5] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_MAXMIPLEVEL, D3DSAMP_MIPMAPLODBIAS };
    const bool internal = dev->m_sodInternal;
    dev->m_sodInternal = true;
    dev->SetTexture(0, tex);
    for (int k = 0; k < 5; k++) dev->SetSamplerState(0, kSs[k], ss[k]);
    dev->m_sodInternal = internal;
  }

  void D3D9DeviceEx::SodBandObserve(const uint8_t* v, UINT stride, UINT count) {
    // XYZRHW + DIFFUSE + TEX1 quads of the subtitle font; only text in the lower 40% of the screen counts
    if (!v || stride < 28 || !count || !m_state.vertexDecl || m_state.vertexDecl->GetFVF() != (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)) return;
    const uint64_t frame = m_sodRt.Frame();
    if (frame != m_sodBandFrame) { m_sodBandPrev = m_sodBandCur; m_sodBandCur = SodBand(); m_sodBandFrame = frame; }
    const float vh = float(m_state.viewport.Height), vy = float(m_state.viewport.Y);
    SodBand b = m_sodBandCur;
    for (UINT i = 0; i < count; i++) {
      float x, y; DWORD c;
      std::memcpy(&x, v + size_t(i) * stride, 4); std::memcpy(&y, v + size_t(i) * stride + 4, 4); std::memcpy(&c, v + size_t(i) * stride + 16, 4);
      if (!(y > vy + 0.6f * vh) || !std::isfinite(x)) return;      // not subtitle text (menus, item names...)
      b.x0 = std::min(b.x0, x); b.x1 = std::max(b.x1, x); b.y0 = std::min(b.y0, y); b.y1 = std::max(b.y1, y);
      b.alpha = std::max(b.alpha, float(c >> 24) / 255.0f); b.any = true;
    }
    m_sodBandCur = b;
  }


  void D3D9DeviceEx::SodBandDraw() {
    static const bool off = [] { char v[8] = { }; return ::GetEnvironmentVariableA("SODRT_SUBBAND", v, 8) && v[0] == '0'; }();
    const uint64_t frame = m_sodRt.Frame();
    if (off || m_sodBandDrawn == frame) return;
    m_sodBandDrawn = frame;
    // the band follows the subtitle text of the frame before (the same lines, nearly always)
    const SodBand& b = frame == m_sodBandFrame ? m_sodBandPrev : m_sodBandCur;
    if (!b.any || b.alpha <= 0.01f || b.x1 - b.x0 < 8.0f) return;
    if (!m_sodBandTex) {
      // soft rounded band: full in the middle, fading out over the outer part on every side
      const UINT tw = 128, th = 32;
      IDirect3DTexture9* t = nullptr;
      if (FAILED(CreateTexture(tw, th, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) || !t) return;
      D3DLOCKED_RECT lr = { };
      if (SUCCEEDED(t->LockRect(0, &lr, nullptr, 0))) {
        for (UINT y = 0; y < th; y++) for (UINT x = 0; x < tw; x++) {
          auto ramp = [] (float d, float f) { float t = std::min(std::max(d / f, 0.0f), 1.0f); return t * t * (3.0f - 2.0f * t); };
          const float fx = (x + 0.5f) / tw, fy = (y + 0.5f) / th;
          const float a = ramp(std::min(fx, 1.0f - fx), 0.22f) * ramp(std::min(fy, 1.0f - fy), 0.45f);
          reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch)[x] = (uint32_t(std::lround(a * 255.0f)) << 24);
        }
        t->UnlockRect(0);
      }
      m_sodBandTex = t; t->Release();
    }
    if (!m_sodBandSb) {
      IDirect3DStateBlock9* sb = nullptr;
      if (FAILED(CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;
      m_sodBandSb = sb; sb->Release();
    }
    const bool internal = m_sodInternal;
    m_sodInternal = true;
    m_sodBandSb->Capture();
    const float lh = std::max(b.y1 - b.y0, 8.0f);
    const float x0 = b.x0 - 0.9f * lh, x1 = b.x1 + 0.9f * lh, y0 = b.y0 - 0.45f * lh, y1 = b.y1 + 0.45f * lh;
    const DWORD col = DWORD(std::lround(0.42f * b.alpha * 255.0f)) << 24;       // faint: at most 42% darker in the middle
    struct V { float x, y, z, w; DWORD c; float u, v; } q[4] = {
      { x0 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, col, 0.0f, 0.0f }, { x1 - 0.5f, y0 - 0.5f, 0.0f, 1.0f, col, 1.0f, 0.0f },
      { x0 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, col, 0.0f, 1.0f }, { x1 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, col, 1.0f, 1.0f } };
    SetVertexShader(nullptr); SetPixelShader(nullptr);
    SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    SetTexture(0, m_sodBandTex.ptr());
    for (DWORD s = 1; s < 8; s++) SetTexture(s, nullptr);
    SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0); SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    SetTextureStageState(0, D3DTSS_RESULTARG, D3DTA_CURRENT);
    SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE); SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA); SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    SetRenderState(D3DRS_ZENABLE, FALSE); SetRenderState(D3DRS_ZWRITEENABLE, FALSE); SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    SetRenderState(D3DRS_STENCILENABLE, FALSE); SetRenderState(D3DRS_FOGENABLE, FALSE); SetRenderState(D3DRS_LIGHTING, FALSE);
    SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE); SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    static const uint16_t idx[6] = { 0, 1, 2, 2, 1, 3 };
    DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 4, 2, idx, D3DFMT_INDEX16, q, sizeof(V));
    m_sodBandSb->Apply();
    m_sodInternal = internal;
  }

}
