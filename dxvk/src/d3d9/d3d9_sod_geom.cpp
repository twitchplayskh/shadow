// Lebensbaum IMPROVED GEOMETRY. The game lays its street paving one polygon at a time: every polygon shows one
// whole tile of the same cobble texture, rotated and mirrored independently (3278 floor triangles measured: four
// right-angle orientations, odd angles on curves, 30 % mirrored). Every polygon edge is therefore a seam, and the
// repeat is broken up by those seams. With IMPROVED GEOMETRY on, those draws are textured in world space instead:
// the tile repeats seamlessly at the game's own scale, and three copies of it (rotated, mirrored, offset) are
// blended by a large, soft noise mask so no repeat shows. All in the fixed-function pipeline, at draw time: the
// game's own lighting stage, fog and depth are kept, only the texture layout changes. SoDFix\geometry.txt lists
// the textures (page regions / cut slots, with the tile size in world units measured from ccc.wad).

#include "d3d9_device.h"
#include "d3d9_texture.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace dxvk {

  static D3DMATRIX SodInverse(const D3DMATRIX& m) {
    // general 4x4 inverse (Gauss-Jordan)
    double a[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { a[i][j] = m.m[i][j]; a[i][j + 4] = i == j ? 1.0 : 0.0; }
    for (int c = 0; c < 4; c++) {
      int p = c;
      for (int r = c + 1; r < 4; r++) if (std::abs(a[r][c]) > std::abs(a[p][c])) p = r;
      if (std::abs(a[p][c]) < 1e-12) { D3DMATRIX id = { }; id._11 = id._22 = id._33 = id._44 = 1.0f; return id; }
      for (int j = 0; j < 8; j++) std::swap(a[c][j], a[p][j]);
      const double d = a[c][c];
      for (int j = 0; j < 8; j++) a[c][j] /= d;
      for (int r = 0; r < 4; r++) if (r != c) { const double f = a[r][c]; for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j]; }
    }
    D3DMATRIX o;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) o.m[i][j] = float(a[i][j + 4]);
    return o;
  }

  static D3DMATRIX SodMul(const D3DMATRIX& a, const D3DMATRIX& b) {
    D3DMATRIX o;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
      float s = 0.0f; for (int k = 0; k < 4; k++) s += a.m[i][k] * b.m[k][j]; o.m[i][j] = s;
    }
    return o;
  }

  // world (x, y, z) -> tile coordinates: u, v on the ground plane (y up), rotated, scaled, mirrored, offset
  static D3DMATRIX SodGroundMap(float size, float angleDeg, bool mirror, float ou, float ov) {
    const float c = std::cos(angleDeg * 3.14159265f / 180.0f), s = std::sin(angleDeg * 3.14159265f / 180.0f);
    const float mu = mirror ? -1.0f : 1.0f;
    D3DMATRIX r = { };
    r._11 = mu * c / size; r._12 = -s / size;
    r._31 = mu * s / size; r._32 = c / size;
    r._41 = ou;            r._42 = ov;
    r._44 = 1.0f;
    return r;
  }


  void D3D9DeviceEx::SodGeomFrame() {
    if (m_sodGeomFailed || m_sodRt.ImprovedGeometry() <= 0) return;
    if (!m_sodGeomTried) {
      m_sodGeomTried = true;
      char path[MAX_PATH] = { };
      ::GetModuleFileNameA(nullptr, path, MAX_PATH);
      if (char* s = std::strrchr(path, '\\')) s[1] = 0;
      const std::string file = std::string(path) + "SoDFix\\geometry.txt";
      std::ifstream f(file);
      std::string line;
      while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line); std::string kind; ss >> kind;
        SodGeomEntry e;
        if (kind == "page") {
          int idx, pw, ph, x, y, w, h; float t;
          if (!(ss >> idx >> pw >> ph >> x >> y >> w >> h >> t) || w <= 0 || h <= 0 || t <= 1.0f) continue;
          e.index = idx; e.page = true; e.pw = uint16_t(pw); e.ph = uint16_t(ph); e.x = uint16_t(x); e.y = uint16_t(y);
          e.w = uint16_t(w); e.h = uint16_t(h); e.tile = t;
        } else if (kind == "slot") {
          int idx; float t;
          if (!(ss >> idx >> t) || t <= 1.0f) continue;
          e.index = idx; e.page = false; e.tile = t;
        } else if (kind == "wallpage") {                 // wall: tile width / height in world units, v sign (1 = upright)
          int idx, pw, ph, x, y, w, h; float tw, th, vs;
          if (!(ss >> idx >> pw >> ph >> x >> y >> w >> h >> tw >> th >> vs) || w <= 0 || h <= 0 || tw <= 1.0f || th <= 1.0f) continue;
          e.index = idx; e.page = true; e.pw = uint16_t(pw); e.ph = uint16_t(ph); e.x = uint16_t(x); e.y = uint16_t(y);
          e.w = uint16_t(w); e.h = uint16_t(h); e.wall = true; e.tileW = tw; e.tileH = th; e.vsign = vs < 0.0f ? -1.0f : 1.0f;
          e.tile = std::max(tw, th);
        } else if (kind == "wallslot") {
          int idx; float tw, th, vs;
          if (!(ss >> idx >> tw >> th >> vs) || tw <= 1.0f || th <= 1.0f) continue;
          e.index = idx; e.page = false; e.wall = true; e.tileW = tw; e.tileH = th; e.vsign = vs < 0.0f ? -1.0f : 1.0f;
          e.tile = std::max(tw, th);
        } else continue;
        m_sodGeomEntries.push_back(e);
      }
      HMODULE fix = ::GetModuleHandleA("dinput8.dll"), d3d8 = ::GetModuleHandleA("d3d8.dll");
      if (fix && !m_sodSsrGetTable) m_sodSsrGetTable = reinterpret_cast<uintptr_t(*)()>(::GetProcAddress(fix, "SoDFix_GetTextureTable"));
      if (d3d8 && !m_sodSsrTex9) m_sodSsrTex9 = reinterpret_cast<void*(__stdcall*)(void*)>(::GetProcAddress(d3d8, "DXVK_SoD_D3D9Texture"));
      if (m_sodGeomEntries.empty() || !m_sodSsrGetTable || !m_sodSsrTex9 || !SodGeomMakeMask()) {
        Logger::err(str::format("SoD-RT: improved geometry unavailable (", file, ")"));
        m_sodGeomFailed = true;
        return;
      }
      m_sodGeomTex8.assign(m_sodGeomEntries.size(), nullptr);
      m_sodGeomTex9Of.assign(m_sodGeomEntries.size(), nullptr);
      m_sodGeomTile.assign(m_sodGeomEntries.size(), nullptr);
      Logger::info(str::format("SoD-RT: improved geometry: ", m_sodGeomEntries.size(), " street textures from ", file));
    }
    const uintptr_t table = m_sodSsrGetTable();
    if (!table) return;
    m_sodGeomFrameNo++;
    bool changed = false;
    for (size_t i = 0; i < m_sodGeomEntries.size(); i++) {
      void* t8 = *reinterpret_cast<void* const*>(table + intptr_t(m_sodGeomEntries[i].index) * 40 + 0x10);
      if (t8 == m_sodGeomTex8[i]) continue;
      if (m_sodGeomTex9Of[i]) {
        auto it = m_sodGeomMap.find(static_cast<IDirect3DBaseTexture9*>(m_sodGeomTex9Of[i]));
        if (it != m_sodGeomMap.end()) {
          auto& v = it->second;
          v.erase(std::remove(v.begin(), v.end(), int(i)), v.end());
          if (v.empty()) m_sodGeomMap.erase(it);
        }
      }
      changed = true;
      if (m_sodGeomTile[i] && m_sodGeomEntries[i].page) static_cast<D3D9Texture2D*>(m_sodGeomTile[i])->ReleasePrivate();
      m_sodGeomTile[i] = nullptr;
      m_sodGeomTex8[i] = t8;
      void* t9 = t8 ? m_sodSsrTex9(t8) : nullptr;
      m_sodGeomTex9Of[i] = t9;
      if (t9) m_sodGeomMap[static_cast<IDirect3DBaseTexture9*>(static_cast<IDirect3DTexture9*>(t9))].push_back(int(i));
    }
    if (changed) { SodGeomDropCache(); m_sodGeomIdxOf.clear(); m_sodGeomSurveyed.clear(); }   // other textures now
    // forget draws not seen for a while (left areas)
    if ((m_sodGeomFrameNo & 63u) == 0u) {
      for (auto it = m_sodGeomCache.begin(); it != m_sodGeomCache.end(); ) {
        if (m_sodGeomFrameNo - it->second.lastFrame > 600u) {
          if (it->second.ib) static_cast<D3D9IndexBuffer*>(it->second.ib)->ReleasePrivate();
          it = m_sodGeomCache.erase(it);
        } else ++it;
      }
    }
  }


  bool D3D9DeviceEx::SodGeomMakeMask() {
    // two soft value-noise masks (RGB: variant 2, A: variant 3), tileable, steep transitions so each patch shows
    // one clean variant and the changes between them are narrow
    const UINT n = 64;
    IDirect3DTexture9* tex = nullptr;
    m_sodNoUpscale = true;
    const HRESULT hr = CreateTexture(n, n, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
    m_sodNoUpscale = false;
    if (FAILED(hr) || !tex) return false;
    auto noise = [] (uint32_t seed, int cells, float x, float y) {
      auto h = [seed] (int i, int j) { uint32_t v = uint32_t(i) * 374761393u + uint32_t(j) * 668265263u + seed * 2246822519u;
        v = (v ^ (v >> 13)) * 1274126177u; return float((v ^ (v >> 16)) & 0xFFFFu) / 65535.0f; };
      const float fx = x * cells, fy = y * cells; const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
      float tx = fx - x0, ty = fy - y0; tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty);
      auto g = [&] (int i, int j) { return h(((i % cells) + cells) % cells, ((j % cells) + cells) % cells); };
      return (g(x0, y0) * (1 - tx) + g(x0 + 1, y0) * tx) * (1 - ty) + (g(x0, y0 + 1) * (1 - tx) + g(x0 + 1, y0 + 1) * tx) * ty;
    };
    auto step = [] (float v) { float t = std::min(std::max((v - 0.47f) / 0.06f, 0.0f), 1.0f); return t * t * (3 - 2 * t); };
    D3DLOCKED_RECT lr = { };
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) { tex->Release(); return false; }
    for (UINT y = 0; y < n; y++) {
      uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch);
      for (UINT x = 0; x < n; x++) {
        const float u = (x + 0.5f) / n, v = (y + 0.5f) / n;
        const float m1 = step(0.65f * noise(1, 4, u, v) + 0.35f * noise(2, 8, u, v));
        const float m2 = step(0.65f * noise(3, 4, u, v) + 0.35f * noise(4, 8, u, v));
        const uint32_t a = uint32_t(m1 * 255.0f + 0.5f), b = uint32_t(m2 * 255.0f + 0.5f);
        row[x] = (b << 24) | (a << 16) | (a << 8) | a;
      }
    }
    tex->UnlockRect(0);
    static_cast<D3D9Texture2D*>(tex)->AddRefPrivate();
    tex->Release();
    m_sodGeomMask = tex;
    return true;
  }


  IDirect3DTexture9* D3D9DeviceEx::SodGeomTile(int entry) {
    // cut slots already repeat (wrap addressing); page regions are copied into a texture of their own. The copy is
    // an ordinary game-side texture, so TEXTURE UPSCALING enlarges and refines it like the others.
    const SodGeomEntry& e = m_sodGeomEntries[entry];
    IDirect3DTexture9* src = static_cast<IDirect3DTexture9*>(m_sodGeomTex9Of[entry]);
    if (!src) return nullptr;
    if (!e.page) return src;
    if (m_sodGeomTile[entry]) return m_sodGeomTile[entry];
    D3DSURFACE_DESC desc = { };
    if (FAILED(src->GetLevelDesc(0, &desc))) return nullptr;
    UINT bpp = 0;
    switch (desc.Format) {
      case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: bpp = 4; break;
      case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: case D3DFMT_R5G6B5: case D3DFMT_A4R4G4B4: bpp = 2; break;
      default: return nullptr;
    }
    const float sx = float(desc.Width) / float(std::max<uint16_t>(e.pw, 1)), sy = float(desc.Height) / float(std::max<uint16_t>(e.ph, 1));
    RECT r = { LONG(e.x * sx + 0.5f), LONG(e.y * sy + 0.5f), LONG((e.x + e.w) * sx + 0.5f), LONG((e.y + e.h) * sy + 0.5f) };
    if (r.right > LONG(desc.Width) || r.bottom > LONG(desc.Height) || r.right - r.left < 4 || r.bottom - r.top < 4) return nullptr;
    const UINT w = UINT(r.right - r.left), h = UINT(r.bottom - r.top);
    IDirect3DTexture9* tile = nullptr;
    if (FAILED(CreateTexture(w, h, 1, 0, desc.Format, D3DPOOL_MANAGED, &tile, nullptr)) || !tile) return nullptr;
    D3DLOCKED_RECT ls = { }, ld = { };
    bool ok = false;
    if (SUCCEEDED(src->LockRect(0, &ls, &r, D3DLOCK_READONLY))) {
      if (SUCCEEDED(tile->LockRect(0, &ld, nullptr, 0))) {
        for (UINT y = 0; y < h; y++)
          std::memcpy(static_cast<uint8_t*>(ld.pBits) + size_t(y) * ld.Pitch, static_cast<const uint8_t*>(ls.pBits) + size_t(y) * ls.Pitch, size_t(w) * bpp);
        tile->UnlockRect(0);
        ok = true;
      }
      src->UnlockRect(0);
    }
    if (!ok) { tile->Release(); return nullptr; }
    static_cast<D3D9Texture2D*>(tile)->AddRefPrivate();
    tile->Release();
    m_sodGeomTile[entry] = tile;
    return tile;
  }


  void D3D9DeviceEx::SodGeomSkip(int entry, uint32_t reason, const char* text) {
    const uint64_t key = (uint64_t(uint32_t(m_sodGeomEntries[entry].index)) << 8) | reason;
    if (m_sodGeomSkipLogged.insert(key).second)
      Logger::warn(str::format("SoD-RT: improved geometry: texture ", m_sodGeomEntries[entry].index, ": ", text));
  }


  void D3D9DeviceEx::SodGeomDropCache() {
    for (auto& kv : m_sodGeomCache)
      if (kv.second.ib) static_cast<D3D9IndexBuffer*>(kv.second.ib)->ReleasePrivate();
    m_sodGeomCache.clear();
  }


  // Sorts a street draw's triangles once: those facing up (ground, slopes up to about 45 degrees) whose texture
  // coordinates lie in a listed street tile go to that tile's group (drawn with the seamless world-space layout);
  // everything else (curb sides, steps, walls, and other images on the same texture page, e.g. tree leaves) stays
  // in the game's group. The groups are written to an index buffer of our own, so drawing them later costs no more
  // than the game's own draw: no vertex copies, no per-frame CPU work.
  bool D3D9DeviceEx::SodGeomBuild(SodGeomCached& c, const std::vector<int>& entries) {
    c.groups.clear();
    const int e0 = entries[0];
    const D3D9VBO& vbo = m_state.vertexBuffers[0];
    D3D9CommonBuffer* vb = GetCommonBuffer(vbo.vertexBuffer);
    D3D9CommonBuffer* ib = GetCommonBuffer(m_state.indices);
    if (!vb->GetMappedSlice() || !ib->GetMappedSlice() || !vb->GetMappedSlice()->mapPtr() || !ib->GetMappedSlice()->mapPtr()) {
      SodGeomSkip(e0, 3, "vertex / index data not readable"); return false;
    }
    const D3DPRIMITIVETYPE type = D3DPRIMITIVETYPE(c.type);
    const bool idx32 = ib->Desc()->Format == D3D9Format::INDEX32;
    const UINT isz = idx32 ? 4u : 2u;
    const UINT count = type == D3DPT_TRIANGLESTRIP ? c.primCount + 2u : c.primCount * 3u;
    if ((size_t(c.startIndex) + count) * isz > ib->Desc()->Size || vbo.stride < 24 || c.base < 0) { SodGeomSkip(e0, 4, "index range"); return false; }
    const uint8_t* idata = static_cast<const uint8_t*>(ib->GetMappedSlice()->mapPtr()) + size_t(c.startIndex) * isz;
    const size_t vbase = size_t(vbo.offset) + size_t(c.base) * vbo.stride;
    if (vbase + size_t(c.minIndex + c.numVertices) * vbo.stride > vb->Desc()->Size) { SodGeomSkip(e0, 5, "vertex range"); return false; }
    const uint8_t* vdata = static_cast<const uint8_t*>(vb->GetMappedSlice()->mapPtr()) + vbase;
    const float* R = c.rot;
    auto rd = [&] (UINT k) -> uint32_t { if (idx32) { uint32_t v; std::memcpy(&v, idata + size_t(k) * 4, 4); return v; }
                                         uint16_t v; std::memcpy(&v, idata + size_t(k) * 2, 2); return v; };
    // which listed tile a texture coordinate falls in (page regions; a cut slot is the whole texture)
    auto tileOf = [&] (float u, float v, bool wall) -> int {
      u -= std::floor(u); v -= std::floor(v);
      for (int e : entries) {
        const SodGeomEntry& en = m_sodGeomEntries[e];
        if (en.wall != wall) continue;
        if (!en.page) return e;
        const float px = u * en.pw, py = v * en.ph;
        if (px >= en.x - 0.5f && px <= en.x + en.w + 0.5f && py >= en.y - 0.5f && py <= en.y + en.h + 0.5f) return e;
      }
      return -1;
    };
    const uint32_t vmin = c.minIndex, vmax = c.minIndex + c.numVertices;
    m_sodGeomIdx.clear(); m_sodGeomTriEntry.clear(); m_sodGeomTriMeas.clear();
    float wallArea[360] = { };
    for (UINT t = 0; t < c.primCount; t++) {
      uint32_t a, b, d;
      if (type == D3DPT_TRIANGLESTRIP) { a = rd(t); b = rd(t + 1); d = rd(t + 2); if (t & 1u) std::swap(b, d); }
      else { a = rd(3 * t); b = rd(3 * t + 1); d = rd(3 * t + 2); }
      if (a == b || b == d || a == d) continue;
      if (a < vmin || b < vmin || d < vmin || a >= vmax || b >= vmax || d >= vmax) { SodGeomSkip(e0, 6, "index outside the vertex range"); return false; }
      float p[3][3], uv[3][2];
      const uint32_t ix[3] = { a, b, d };
      for (int k = 0; k < 3; k++) {
        const uint8_t* vp = vdata + size_t(ix[k]) * vbo.stride;
        float q[3]; std::memcpy(q, vp, 12); std::memcpy(uv[k], vp + 16, 8);
        for (int m = 0; m < 3; m++) p[k][m] = q[0] * R[m] + q[1] * R[3 + m] + q[2] * R[6 + m];   // rotation only
      }
      const float e1[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
      const float e2[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
      const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
      const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      // floor: faces up (ground, slopes); wall: within ~17 degrees of vertical, grouped by the direction it faces
      // (whole degrees), so every polygon of one wall face shares one layout
      int g = -1, deg = -1;
      const float uc = (uv[0][0] + uv[1][0] + uv[2][0]) / 3.0f, vc = (uv[0][1] + uv[1][1] + uv[2][1]) / 3.0f;
      SodGeomMeas meas = { 0.0f, 0.0f, 1.0f, 0.0f, false, false };
      if (nl > 1e-6f && std::abs(n[1]) >= 0.7f * nl) g = tileOf(uc, vc, false);
      else if (nl > 1e-6f && std::abs(n[1]) <= 0.3f * nl) {
        g = tileOf(uc, vc, true);
        if (g >= 0) {
          deg = int(std::lround(std::atan2(n[2], n[0]) * 57.29578f)); if (deg < 0) deg += 360; if (deg >= 360) deg -= 360;
          wallArea[deg] += nl;
          // how the game lays the image on this polygon: J = d(tile coordinates) / d(along the wall, up), in tiles
          // (a page region is one tile; a cut slot is the whole texture). From its inverse: the world size of one
          // tile along the wall and up it, which way up the image is, and whether it lies on its side.
          const SodGeomEntry& en = m_sodGeomEntries[g];
          const float su = en.page ? float(en.pw) / float(en.w) : 1.0f, sv = en.page ? float(en.ph) / float(en.h) : 1.0f;
          const float a0 = float(deg) * 3.14159265f / 180.0f, tx = -std::sin(a0), tz = std::cos(a0);
          const float m00 = e1[0] * tx + e1[2] * tz, m01 = e1[1], m10 = e2[0] * tx + e2[2] * tz, m11 = e2[1];
          const float d00 = (uv[1][0] - uv[0][0]) * su, d01 = (uv[1][1] - uv[0][1]) * sv;
          const float d10 = (uv[2][0] - uv[0][0]) * su, d11 = (uv[2][1] - uv[0][1]) * sv;
          // [dTile] = [dWorld] * J  ->  J = M^-1 D;  world per tile: Ji = D^-1 M
          const float dd = d00 * d11 - d01 * d10;
          if (std::abs(dd) > 1e-12f) {
            const float i00 = d11 / dd, i01 = -d01 / dd, i10 = -d10 / dd, i11 = d00 / dd;
            const float j00 = i00 * m00 + i01 * m10, j01 = i00 * m01 + i01 * m11;    // per tile u: (along, up)
            const float j10 = i10 * m00 + i11 * m10, j11 = i10 * m01 + i11 * m11;    // per tile v
            const float lu = std::sqrt(j00 * j00 + j01 * j01), lv = std::sqrt(j10 * j10 + j11 * j11);
            if (lu > 1.0f && lv > 1.0f) {
              if (std::abs(j11) >= 0.9f * lv) meas = { lu, lv, j11 < 0.0f ? 1.0f : -1.0f, nl, false, true };   // v up / down
              else if (std::abs(j01) >= 0.9f * lu) meas = { lv, lu, j01 < 0.0f ? 1.0f : -1.0f, nl, true, true };   // on its side
            }
          }
        }
      }
      m_sodGeomTriMeas.push_back(meas);
      m_sodGeomTriEntry.push_back(uint32_t(g + 1) | (uint32_t(deg + 1) << 16));
      m_sodGeomIdx.push_back(a); m_sodGeomIdx.push_back(b); m_sodGeomIdx.push_back(d);
    }
    // facings a few degrees apart are one wall face (slightly bent polygons, rounding): each takes the facing of the
    // largest face within 6 degrees, so the layout runs on across them without a jump where they meet
    int rep[360];
    for (int k = 0; k < 360; k++) rep[k] = -1;
    for (;;) {
      int best = -1;
      for (int k = 0; k < 360; k++) if (rep[k] < 0 && wallArea[k] > 0.0f && (best < 0 || wallArea[k] > wallArea[best])) best = k;
      if (best < 0) break;
      for (int dk = -6; dk <= 6; dk++) { const int k = (best + dk + 360) % 360; if (rep[k] < 0 && wallArea[k] > 0.0f) rep[k] = best; }
    }
    for (uint32_t& k : m_sodGeomTriEntry) {
      const int dg = int(k >> 16) - 1;
      if (dg >= 0 && rep[dg] >= 0) k = (k & 0xFFFFu) | (uint32_t(rep[dg] + 1) << 16);
    }
    // groups: game mapping first, then each tile (walls: each tile + facing); nothing converted -> the game draws it
    std::vector<uint32_t> keys = { 0u };
    for (uint32_t k : m_sodGeomTriEntry) if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
    std::vector<uint32_t> out; out.reserve(m_sodGeomIdx.size());
    bool any = false;
    for (uint32_t key : keys) {
      SodGeomGroup grp; grp.entry = int(key & 0xFFFFu) - 1; grp.wallDeg = int(key >> 16) - 1; grp.start = UINT(out.size());
      for (size_t t = 0; t < m_sodGeomTriEntry.size(); t++)
        if (m_sodGeomTriEntry[t] == key) { out.push_back(m_sodGeomIdx[3 * t]); out.push_back(m_sodGeomIdx[3 * t + 1]); out.push_back(m_sodGeomIdx[3 * t + 2]); }
      grp.tris = UINT(out.size() - grp.start) / 3u;
      if (grp.tris && grp.wallDeg >= 0) {
        // the wall's layout: the tile size the game used on most of this face (area-weighted median), its usual way
        // up and turn; geometry.txt's measured size if no polygon tells
        const SodGeomEntry& en = m_sodGeomEntries[grp.entry];
        float upA = 0.0f, sideA = 0.0f, vsA = 0.0f;
        for (size_t t = 0; t < m_sodGeomTriEntry.size(); t++)
          if (m_sodGeomTriEntry[t] == key && m_sodGeomTriMeas[t].ok) (m_sodGeomTriMeas[t].turned ? sideA : upA) += m_sodGeomTriMeas[t].area;
        const bool turned = sideA > upA;
        std::vector<std::pair<float, float>> ws, hs;
        for (size_t t = 0; t < m_sodGeomTriEntry.size(); t++) {
          const SodGeomMeas& m = m_sodGeomTriMeas[t];
          if (m_sodGeomTriEntry[t] != key || !m.ok || m.turned != turned) continue;
          ws.push_back({ m.tw, m.area }); hs.push_back({ m.th, m.area }); vsA += m.vs * m.area;
        }
        auto wmedian = [] (std::vector<std::pair<float, float>>& v) {
          std::sort(v.begin(), v.end()); float tot = 0.0f; for (auto& p : v) tot += p.second;
          float acc = 0.0f; for (auto& p : v) { acc += p.second; if (acc >= 0.5f * tot) return p.first; } return v.back().first; };
        if (!ws.empty()) { grp.tileW = wmedian(ws); grp.tileH = wmedian(hs); grp.vsign = vsA < 0.0f ? -1.0f : 1.0f; grp.turned = turned; }
        else { grp.tileW = en.tileW; grp.tileH = en.tileH; grp.vsign = en.vsign; grp.turned = false; }
        if (!(grp.tileW > 1.0f && grp.tileH > 1.0f)) { grp.entry = -1; grp.wallDeg = -1; }    // nothing sensible: game mapping
      }
      if (grp.tris) { c.groups.push_back(grp); if (grp.entry >= 0) any = true; }
    }
    if (!any) { c.groups.clear(); return true; }
    IDirect3DIndexBuffer9* nib = nullptr;
    const UINT bytes = UINT(out.size()) * isz;
    if (FAILED(CreateIndexBuffer(bytes, D3DUSAGE_WRITEONLY, idx32 ? D3DFMT_INDEX32 : D3DFMT_INDEX16, D3DPOOL_MANAGED, &nib, nullptr)) || !nib) {
      SodGeomSkip(e0, 8, "index buffer could not be made"); c.groups.clear(); return false;
    }
    void* dst = nullptr;
    if (FAILED(nib->Lock(0, bytes, &dst, 0)) || !dst) { nib->Release(); c.groups.clear(); return false; }
    if (idx32) std::memcpy(dst, out.data(), bytes);
    else { uint16_t* d16 = static_cast<uint16_t*>(dst); for (size_t k = 0; k < out.size(); k++) d16[k] = uint16_t(out[k]); }
    nib->Unlock();
    static_cast<D3D9IndexBuffer*>(nib)->AddRefPrivate();
    nib->Release();
    c.ib = nib;
    return true;
  }


  bool D3D9DeviceEx::SodGeomDraw(D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT numVertices, UINT startIndex, UINT primCount) {
    if (m_sodInternal || m_sodGeomFailed || m_sodRt.ImprovedGeometry() <= 0) return false;
    if (!m_state.vertexDecl || m_state.vertexShader.ptr() || m_state.pixelShader.ptr()) return false;
    if (m_state.vertexDecl->GetFVF() != (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)) return false;
    auto it = m_sodGeomMap.find(m_state.textures[0]);
    if (it == m_sodGeomMap.end() || it->second.empty()) return false;
    const int e0 = it->second[0];
    if (type != D3DPT_TRIANGLESTRIP && type != D3DPT_TRIANGLELIST) { SodGeomSkip(e0, 1, "not a triangle list or strip"); return false; }
    if (m_state.textureStages[1][DXVK_TSS_COLOROP] != D3DTOP_DISABLE) { SodGeomSkip(e0, 2, "draw uses two texture stages"); return false; }
    const D3D9VBO& vbo = m_state.vertexBuffers[0];
    D3D9CommonBuffer* vb = GetCommonBuffer(vbo.vertexBuffer);
    D3D9CommonBuffer* ib = GetCommonBuffer(m_state.indices);
    if (!vb || !ib) return false;

    // look the draw up (same buffers, same contents, same range, same orientation -> same split)
    const auto& W = m_state.transforms[GetTransformIndex(D3DTS_WORLD)];
    float rot[9];
    for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) rot[r * 3 + k] = W[r][k];
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h] (const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; } };
    const uint64_t vbUid = vb->SodUid(), ibUid = ib->SodUid();
    IDirect3DBaseTexture9* tex = m_state.textures[0];
    const UINT utype = UINT(type);
    mix(&vbUid, 8); mix(&ibUid, 8); mix(&base, 4); mix(&minIndex, 4); mix(&numVertices, 4); mix(&startIndex, 4);
    mix(&primCount, 4); mix(&utype, 4); mix(&tex, sizeof(tex)); mix(rot, sizeof(rot)); mix(&vbo.offset, 4); mix(&vbo.stride, 4);
    SodGeomCached& c = m_sodGeomCache[h];
    const bool same = c.vbUid == vbUid && c.ibUid == ibUid && c.base == base && c.minIndex == minIndex && c.numVertices == numVertices
      && c.startIndex == startIndex && c.primCount == primCount && c.type == utype && c.tex == tex && !std::memcmp(c.rot, rot, sizeof(rot));
    if (!same || c.vbGen != vb->SodWriteGen() || c.ibGen != ib->SodWriteGen()) {
      if (c.ib) { static_cast<D3D9IndexBuffer*>(c.ib)->ReleasePrivate(); c.ib = nullptr; }
      c.vbUid = vbUid; c.ibUid = ibUid; c.vbGen = vb->SodWriteGen(); c.ibGen = ib->SodWriteGen();
      c.base = base; c.minIndex = minIndex; c.numVertices = numVertices; c.startIndex = startIndex; c.primCount = primCount;
      c.type = utype; c.tex = tex; std::memcpy(c.rot, rot, sizeof(rot));
      SodGeomBuild(c, it->second);
      m_sodGeomBuilds++;
      if (m_sodGeomCache.size() > 8192u) { SodGeomDropCache(); return false; }
    }
    c.lastFrame = m_sodGeomFrameNo;
    if (c.groups.empty() || !c.ib) return false;     // nothing to convert: the game's own draw

    m_sodGeomTiles.assign(c.groups.size(), nullptr);
    IDirect3DTexture9** tiles = m_sodGeomTiles.data();
    for (size_t k = 0; k < c.groups.size(); k++)
      if (c.groups[k].entry >= 0) tiles[k] = SodGeomTile(c.groups[k].entry);   // made once, outside the internal draw
    Com<D3D9IndexBuffer> savedIb = m_state.indices.ptr();
    const bool wasInternal = m_sodInternal;
    m_sodInternal = true;
    SetIndices(c.ib);
    for (size_t k = 0; k < c.groups.size(); k++) {
      const SodGeomGroup& g = c.groups[k];
      SodGeomGuard guard;
      if (g.entry >= 0 && tiles[k]) guard.Begin(this, g.entry, tiles[k], g.wallDeg >= 0 ? &g : nullptr);
      SodDrawIndexedRaw(D3DPT_TRIANGLELIST, base, minIndex, numVertices, g.start, g.tris);
    }
    SetIndices(savedIb.ptr());
    m_sodInternal = wasInternal;
    return true;
  }


  bool D3D9DeviceEx::SodGeomGuard::Begin(D3D9DeviceEx* d, int entry, IDirect3DTexture9* tile, const SodGeomGroup* wall) {
    const float size = d->m_sodGeomEntries[entry].tile;
    static const int envDebug = [] { char v[8] = { }; return ::GetEnvironmentVariableA("SODRT_GEOM_DEBUG", v, 8) ? std::atoi(v) : 0; }();

    // save what we change
    static const D3DTEXTURESTAGESTATETYPE kTss[11] = { D3DTSS_COLOROP, D3DTSS_COLORARG0, D3DTSS_COLORARG1, D3DTSS_COLORARG2,
      D3DTSS_ALPHAOP, D3DTSS_ALPHAARG0, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS,
      D3DTSS_RESULTARG };
    static const D3DSAMPLERSTATETYPE kSs[7] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER,
      D3DSAMP_MIPFILTER, D3DSAMP_MAXANISOTROPY, D3DSAMP_SRGBTEXTURE };
    dev = d;
    internal = d->m_sodInternal;
    d->GetRenderState(D3DRS_TEXTUREFACTOR, &tfactor);
    for (DWORD s = 0; s < 5; s++) tex[s] = d->m_state.textures[s];
    for (DWORD s = 0; s < 6; s++) for (int k = 0; k < 11; k++) d->GetTextureStageState(s, kTss[k], &tss[s][k]);
    for (DWORD s = 0; s < 4; s++) for (int k = 0; k < 7; k++) d->GetSamplerState(s, kSs[k], &ss[s][k]);
    for (DWORD s = 0; s < 4; s++) d->GetTransform(D3DTRANSFORMSTATETYPE(D3DTS_TEXTURE0 + s), &tm[s]);

    // the game's own combine of stage 0 (texture with the vertex lighting) moves to stage 4, fed by the blended tile
    auto mapArg = [] (DWORD a) {
      const DWORD sel = a & D3DTA_SELECTMASK, mod = a & ~D3DTA_SELECTMASK;
      if (sel == D3DTA_TEXTURE) return D3DTA_CURRENT | mod;
      if (sel == D3DTA_CURRENT) return D3DTA_DIFFUSE | mod;
      return a;
    };
    d->m_sodInternal = true;
    D3DMATRIX view;
    std::memcpy(&view, &d->m_state.transforms[GetTransformIndex(D3DTS_VIEW)], sizeof(view));
    if (std::memcmp(&view, &d->m_sodGeomView, sizeof(view))) {      // once per camera change, not per draw
      d->m_sodGeomView = view;
      d->m_sodGeomInvView = SodInverse(view);
    }
    const D3DMATRIX& invView = d->m_sodGeomInvView;
    if (wall) {
      // WALL: one layout per wall face, aligned to the world: u runs along the wall (to the right seen from the front),
      // v down from world height 0, at the tile size measured from the game's own walls. Bricks keep their rows and
      // courses line up across polygons and walls; no rotation, mirroring or blending. Stage 0 keeps the game's own
      // combine (texture x vertex lighting), only its texture coordinates change.
      const float a = float(wall->wallDeg) * 3.14159265f / 180.0f;
      const float tx = -std::sin(a), tz = std::cos(a);                   // normal (cos a, 0, sin a) -> along the wall
      D3DMATRIX m = { };
      if (!wall->turned) {                           // u along the wall, v down it
        m._11 = tx / wall->tileW; m._31 = tz / wall->tileW;
        m._22 = -wall->vsign / wall->tileH;
      } else {                                       // the image on its side: u down the wall, v along it
        m._21 = -wall->vsign / wall->tileH;
        m._12 = tx / wall->tileW; m._32 = tz / wall->tileW;
      }
      m._44 = 1.0f;
      const D3DMATRIX tm0 = SodMul(invView, m);
      d->SetTexture(0, tile);
      d->SetTransform(D3DTS_TEXTURE0, &tm0);
      d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
      d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
      d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
      if (envDebug) {                                // SODRT_GEOM_DEBUG=1: converted walls tinted blue
        d->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFF8080FFu);
        d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
        d->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT); d->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
      }
      d->m_sodInternal = internal;
      return true;
    }
    const D3DMATRIX maps[4] = {
      SodMul(invView, SodGroundMap(size * 3.3f, 17.0f, false, 0.13f, 0.29f)),     // mask
      SodMul(invView, SodGroundMap(size, 0.0f, false, 0.0f, 0.0f)),
      SodMul(invView, SodGroundMap(size, 90.0f, false, 0.37f, 0.61f)),
      SodMul(invView, SodGroundMap(size, 180.0f, true, 0.71f, 0.23f)) };
    IDirect3DBaseTexture9* stageTex[4] = { d->m_sodGeomMask, tile, tile, tile };
    for (DWORD s = 0; s < 4; s++) {
      d->SetTexture(s, stageTex[s]);
      d->SetTransform(D3DTRANSFORMSTATETYPE(D3DTS_TEXTURE0 + s), &maps[s]);
      d->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION | s);
      d->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
      d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP); d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
      if (s > 0) for (int k = 2; k < 7; k++) d->SetSamplerState(s, kSs[k], ss[0][k]);   // filtering as the game's stage 0
      else { d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
             d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR); d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE); }
    }
    // stage 0: mask -> TEMP (rgb = variant 2 weight, alpha = variant 3 weight)
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_RESULTARG, D3DTA_TEMP);
    // stage 1: variant 1
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(1, D3DTSS_RESULTARG, D3DTA_CURRENT);
    // stage 2 / 3: blend in variants 2 and 3 by the mask
    d->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_LERP);
    d->SetTextureStageState(2, D3DTSS_COLORARG0, D3DTA_TEMP); d->SetTextureStageState(2, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(2, D3DTSS_COLORARG2, D3DTA_CURRENT);
    d->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(2, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
    d->SetTextureStageState(2, D3DTSS_RESULTARG, D3DTA_CURRENT);
    d->SetTextureStageState(3, D3DTSS_COLOROP, D3DTOP_LERP);
    d->SetTextureStageState(3, D3DTSS_COLORARG0, D3DTA_TEMP | D3DTA_ALPHAREPLICATE); d->SetTextureStageState(3, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(3, D3DTSS_COLORARG2, D3DTA_CURRENT);
    d->SetTextureStageState(3, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(3, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
    d->SetTextureStageState(3, D3DTSS_RESULTARG, D3DTA_CURRENT);
    // stage 4: the game's stage 0 combine (e.g. texture x vertex lighting)
    d->SetTexture(4, nullptr);
    d->SetTextureStageState(4, D3DTSS_COLOROP, tss[0][0]);
    d->SetTextureStageState(4, D3DTSS_COLORARG0, mapArg(tss[0][1]));
    d->SetTextureStageState(4, D3DTSS_COLORARG1, mapArg(tss[0][2]));
    d->SetTextureStageState(4, D3DTSS_COLORARG2, mapArg(tss[0][3]));
    d->SetTextureStageState(4, D3DTSS_ALPHAOP, tss[0][4]);
    d->SetTextureStageState(4, D3DTSS_ALPHAARG0, mapArg(tss[0][5]));
    d->SetTextureStageState(4, D3DTSS_ALPHAARG1, mapArg(tss[0][6]));
    d->SetTextureStageState(4, D3DTSS_ALPHAARG2, mapArg(tss[0][7]));
    d->SetTextureStageState(4, D3DTSS_TEXCOORDINDEX, 0);
    d->SetTextureStageState(4, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetTextureStageState(4, D3DTSS_RESULTARG, D3DTA_CURRENT);
    if (envDebug) {                                  // SODRT_GEOM_DEBUG=1: converted ground tinted red
      d->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFF8080u);
      d->SetTextureStageState(5, D3DTSS_COLOROP, D3DTOP_MODULATE);
      d->SetTextureStageState(5, D3DTSS_COLORARG1, D3DTA_CURRENT); d->SetTextureStageState(5, D3DTSS_COLORARG2, D3DTA_TFACTOR);
      d->SetTextureStageState(5, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(5, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
      d->SetTextureStageState(5, D3DTSS_RESULTARG, D3DTA_CURRENT);
    } else {
      d->SetTextureStageState(5, D3DTSS_COLOROP, D3DTOP_DISABLE);
      d->SetTextureStageState(5, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
    d->m_sodInternal = internal;
    if ((d->m_sodGeomDraws++ % 3000u) == 0u)
      Logger::info(str::format("SoD-RT: improved geometry: street draw ", d->m_sodGeomDraws, " (texture ", d->m_sodGeomEntries[entry].index,
        ", tile ", size, " units, stage 0 op ", tss[0][0], " args ", tss[0][2], " ", tss[0][3], ")"));
    return true;
  }


  D3D9DeviceEx::SodGeomGuard::~SodGeomGuard() {
    if (!dev) return;
    static const D3DTEXTURESTAGESTATETYPE kTss[11] = { D3DTSS_COLOROP, D3DTSS_COLORARG0, D3DTSS_COLORARG1, D3DTSS_COLORARG2,
      D3DTSS_ALPHAOP, D3DTSS_ALPHAARG0, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS,
      D3DTSS_RESULTARG };
    static const D3DSAMPLERSTATETYPE kSs[7] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER,
      D3DSAMP_MIPFILTER, D3DSAMP_MAXANISOTROPY, D3DSAMP_SRGBTEXTURE };
    dev->m_sodInternal = true;
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, tfactor);
    for (DWORD s = 0; s < 5; s++) dev->SetTexture(s, tex[s]);
    for (DWORD s = 0; s < 6; s++) for (int k = 0; k < 11; k++) dev->SetTextureStageState(s, kTss[k], tss[s][k]);
    for (DWORD s = 0; s < 4; s++) for (int k = 0; k < 7; k++) dev->SetSamplerState(s, kSs[k], ss[s][k]);
    for (DWORD s = 0; s < 4; s++) dev->SetTransform(D3DTRANSFORMSTATETYPE(D3DTS_TEXTURE0 + s), &tm[s]);
    dev->m_sodInternal = internal;
  }

}

namespace dxvk {

  // Survey (diagnostics, once per texture and area): for every fixed-function room draw (FVF 0x142) that has
  // vertical faces, log its texture table index, the page sub-image(s) its wall triangles use, whether the texture
  // is listed in geometry.txt and what IMPROVED GEOMETRY made of it. "SoD-RT: wall survey:" lines in d3d9.log.
  int D3D9DeviceEx::SodGeomTableIndex(IDirect3DBaseTexture9* tex) {
    auto it = m_sodGeomIdxOf.find(tex);
    if (it != m_sodGeomIdxOf.end()) return it->second;
    int found = 0x7FFFFFFF;
    if (m_sodSsrGetTable && m_sodSsrTex9)
      if (const uintptr_t table = m_sodSsrGetTable())
        for (int i = -16384; i < 16384 && found == 0x7FFFFFFF; i++) {
          void* t8 = *reinterpret_cast<void* const*>(table + intptr_t(i) * 40 + 0x10);
          if (t8 && m_sodSsrTex9(t8) == static_cast<void*>(static_cast<IDirect3DTexture9*>(static_cast<D3D9Texture2D*>(tex)))) found = i;
        }
    m_sodGeomIdxOf[tex] = found;
    return found;
  }

  void D3D9DeviceEx::SodGeomSurvey(const char* api, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT numVertices,
                                   UINT startIndex, UINT primCount, bool indexed) {
    if (m_sodInternal || m_sodGeomFailed || m_sodRt.ImprovedGeometry() <= 0 || !m_state.vertexDecl) return;
    if (m_state.vertexShader.ptr() || m_state.vertexDecl->GetFVF() != (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)) return;
    IDirect3DBaseTexture9* tex = m_state.textures[0];
    if (!tex || tex->GetType() != D3DRTYPE_TEXTURE) return;
    const uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(tex)) << 2) | (indexed ? 1u : 0u);
    if (!m_sodGeomSurveyed.insert(key).second) return;
    if (type != D3DPT_TRIANGLESTRIP && type != D3DPT_TRIANGLELIST) return;
    const D3D9VBO& vbo = m_state.vertexBuffers[0];
    D3D9CommonBuffer* vb = GetCommonBuffer(vbo.vertexBuffer);
    D3D9CommonBuffer* ib = indexed ? GetCommonBuffer(m_state.indices) : nullptr;
    if (!vb || !vb->GetMappedSlice() || !vb->GetMappedSlice()->mapPtr() || vbo.stride < 24 || base < 0) return;
    if (indexed && (!ib || !ib->GetMappedSlice() || !ib->GetMappedSlice()->mapPtr())) return;
    const bool idx32 = indexed && ib->Desc()->Format == D3D9Format::INDEX32;
    const UINT count = type == D3DPT_TRIANGLESTRIP ? primCount + 2u : primCount * 3u;
    if (indexed && (size_t(startIndex) + count) * (idx32 ? 4u : 2u) > ib->Desc()->Size) return;
    const uint8_t* idata = indexed ? static_cast<const uint8_t*>(ib->GetMappedSlice()->mapPtr()) + size_t(startIndex) * (idx32 ? 4u : 2u) : nullptr;
    const size_t vbase = size_t(vbo.offset) + size_t(base) * vbo.stride;
    const uint8_t* vdata = static_cast<const uint8_t*>(vb->GetMappedSlice()->mapPtr()) + vbase;
    const size_t vlimit = vb->Desc()->Size > vbase ? (vb->Desc()->Size - vbase) / vbo.stride : 0;
    auto rd = [&] (UINT k) -> uint32_t {
      if (!indexed) return startIndex + k;                 // DrawPrimitive: StartVertex in startIndex
      if (idx32) { uint32_t v; std::memcpy(&v, idata + size_t(k) * 4, 4); return v; }
      uint16_t v; std::memcpy(&v, idata + size_t(k) * 2, 2); return v; };
    D3DSURFACE_DESC desc = { };
    static_cast<IDirect3DTexture9*>(static_cast<D3D9Texture2D*>(tex))->GetLevelDesc(0, &desc);
    float areaAll = 0.0f, areaWall = 0.0f;
    float umin = 1e9f, vmin = 1e9f, umax = -1e9f, vmax = -1e9f;
    for (UINT t = 0; t < primCount; t++) {
      uint32_t a, b, c;
      if (type == D3DPT_TRIANGLESTRIP) { a = rd(t); b = rd(t + 1); c = rd(t + 2); }
      else { a = rd(3 * t); b = rd(3 * t + 1); c = rd(3 * t + 2); }
      if (a == b || b == c || a == c || a >= vlimit || b >= vlimit || c >= vlimit) continue;
      float p[3][3], uv[3][2];
      const uint32_t ix[3] = { a, b, c };
      for (int k = 0; k < 3; k++) { const uint8_t* vp = vdata + size_t(ix[k]) * vbo.stride; std::memcpy(p[k], vp, 12); std::memcpy(uv[k], vp + 16, 8); }
      const float e1[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
      const float e2[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
      const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
      const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      areaAll += nl;
      if (nl > 1e-6f && std::abs(n[1]) <= 0.3f * nl) {
        areaWall += nl;
        float uc = (uv[0][0] + uv[1][0] + uv[2][0]) / 3.0f, vc = (uv[0][1] + uv[1][1] + uv[2][1]) / 3.0f;
        uc -= std::floor(uc); vc -= std::floor(vc);
        umin = std::min(umin, uc); umax = std::max(umax, uc); vmin = std::min(vmin, vc); vmax = std::max(vmax, vc);
      }
    }
    if (areaWall < 0.2f * areaAll || areaWall <= 0.0f) return;
    const int idx = SodGeomTableIndex(tex);
    auto it = m_sodGeomMap.find(tex);
    std::string listed = it == m_sodGeomMap.end() ? std::string("NOT LISTED") : str::format("listed (", it->second.size(), " entries)");
    const DWORD op1 = m_state.textureStages[1][DXVK_TSS_COLOROP];
    Logger::warn(str::format("SoD-RT: wall survey: texture ", idx, " ", desc.Width, "x", desc.Height, " via ", api,
      ", walls ", int(100.0f * areaWall / std::max(areaAll, 1e-6f)), "% of the draw, wall UV centres x ",
      int(umin * desc.Width), "..", int(umax * desc.Width), " y ", int(vmin * desc.Height), "..", int(vmax * desc.Height),
      " | ", listed, op1 != D3DTOP_DISABLE ? " | uses a 2nd texture stage (skipped)" : ""));
  }

}
