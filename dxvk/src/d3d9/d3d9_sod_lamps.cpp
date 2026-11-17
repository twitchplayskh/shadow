// Shadow LAMP LIGHTING: which lamps light the scene.
//
// Two sources, gathered into one registry that holds every lamp seen in the last second or so:
//  - the game's Direct3D point lights (fires, the candles it lights its characters with, ceiling lamps). The game
//    gives each character draw its nearest few lights and reuses the light slots between draws, so the lights set
//    at any one moment are only part of the room's lamps: the registry keeps all of them, matched by position.
//  - flames and glows the game draws itself without a light: small additive sprites in the 3D scene (wall candles,
//    lamp halos). Each becomes a small warm lamp at the sprite's centre, unless a Direct3D light is already there.
// The registry is read when the ray-traced pass is posted (d3d9_sod_link.cpp BuildGeometry).

#include "d3d9_device.h"
#include "d3d9_swapchain.h"
#include "../sod_bridge/sod_bridge_proto.h"
#include <algorithm>

namespace dxvk {

  void D3D9DeviceEx::SodLampNote(const float pos[3], const float col[3], const float att[3], float range, bool flame) {
    const uint64_t now = m_sodRt.Frame();
    const float match = flame ? 60.0f : 100.0f;
    SodLampEntry* hit = nullptr;
    for (auto& e : m_sodLamps) {
      float d2 = 0.0f; for (int j = 0; j < 3; j++) d2 += (e.pos[j] - pos[j]) * (e.pos[j] - pos[j]);
      // a flame where a Direct3D light already is (seen lately): that light is the better description
      if (flame && !e.flame && now - e.last < 90u && d2 < 200.0f * 200.0f) return;
      if (e.flame == flame && d2 < match * match) { hit = &e; break; }
    }
    if (!hit) {
      if (m_sodLamps.size() >= 48) {                    // full: the one unseen the longest goes
        size_t o = 0; for (size_t i = 1; i < m_sodLamps.size(); i++) if (m_sodLamps[i].last < m_sodLamps[o].last) o = i;
        m_sodLamps.erase(m_sodLamps.begin() + o);
      }
      m_sodLamps.push_back({ }); hit = &m_sodLamps.back();
      for (int j = 0; j < 3; j++) { hit->pos[j] = pos[j]; hit->col[j] = col[j]; hit->att[j] = att[j]; }
      hit->range = range; hit->flame = flame; hit->first = now;
      if (m_sodLampLogged < 200) {
        m_sodLampLogged++;
        Logger::info(str::format("SoD-RT: lamp lighting: new ", flame ? "flame" : "light", " at ",
          int(pos[0]), " ", int(pos[1]), " ", int(pos[2]), " colour ", col[0], " ", col[1], " ", col[2], " (frame ", now, ")"));
      }
    } else if (hit->last != now) {
      // the game's flicker moves and tints a light a little every frame: followed, slightly softened
      for (int j = 0; j < 3; j++) { hit->pos[j] += (pos[j] - hit->pos[j]) * 0.5f; hit->col[j] += (col[j] - hit->col[j]) * 0.6f; hit->att[j] = att[j]; }
      hit->range = range;
    }
    hit->last = now;
  }


  void D3D9DeviceEx::SodLampNoteLight(const D3DLIGHT9& d) {
    if (d.Type != D3DLIGHT_POINT || d.Diffuse.r + d.Diffuse.g + d.Diffuse.b < 0.01f) return;
    const float p[3] = { d.Position.x, d.Position.y, d.Position.z };
    const float c[3] = { d.Diffuse.r, d.Diffuse.g, d.Diffuse.b };
    const float a[3] = { d.Attenuation0, d.Attenuation1, d.Attenuation2 };
    SodLampNote(p, c, a, d.Range > 0.0f ? d.Range : 1e9f, false);
  }


  void D3D9DeviceEx::SodFlameObserve(uint32_t api, D3DPRIMITIVETYPE type, UINT a, UINT b, UINT c,
                                     const void* vdata, UINT vstride) {
    if (m_sodInternal || !m_state.vertexDecl || m_state.vertexShader.ptr() || m_sodRt.LampLighting() <= 0) return;
    const auto& rs = m_state.renderStates;
    if (!rs[D3DRS_ALPHABLENDENABLE] || rs[D3DRS_STENCILENABLE]) return;
    const uint32_t fvf = m_state.vertexDecl->GetFVF();
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ) return;   // 3D draws only (not the menus and text)
    D3D9Surface* rt = m_state.renderTargets[0].ptr();
    if (!rt || !m_implicitSwapchain || rt != m_implicitSwapchain->GetBackBuffer(0)) return;
    const bool additive = rs[D3DRS_DESTBLEND] == D3DBLEND_ONE;
    // survey (research): every kind of blended 3D draw once, so flames drawn some other way can be found
    auto survey = [&] (const char* verdict, UINT n, float size, const float* at) {
      if (m_sodFlameSurvey >= 150) return;
      const uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(m_state.textures[0])) << 16) ^ (uint64_t(rs[D3DRS_SRCBLEND]) << 8)
                         ^ uint64_t(rs[D3DRS_DESTBLEND]) ^ (uint64_t(fvf) << 40);
      if (!m_sodFlameKeys.insert(key).second) return;
      m_sodFlameSurvey++;
      Logger::info(str::format("SoD-RT: lamp survey: blended draw fvf 0x", std::hex, fvf, std::dec, " blend ", rs[D3DRS_SRCBLEND], "/",
        rs[D3DRS_DESTBLEND], " zwrite ", rs[D3DRS_ZWRITEENABLE], " api ", api, " vertices ", n, " size ", int(size),
        at ? str::format(" at ", int(at[0]), " ", int(at[1]), " ", int(at[2])) : std::string(), " -> ", verdict, " (frame ", m_sodRt.Frame(), ")"));
    };
    // the vertices: position at the start of each, diffuse after it (and the normal, if any)
    const uint8_t* base = nullptr; UINT stride = 0, count = 0;
    if (api == 2 || api == 3) {
      base = static_cast<const uint8_t*>(vdata); stride = vstride;
      count = api == 2 ? GetVertexCount(type, a) : b;
      if (api == 3) base += size_t(a) * stride;
    } else {
      const D3D9VBO& vbo = m_state.vertexBuffers[0];
      D3D9CommonBuffer* buf = GetCommonBuffer(vbo.vertexBuffer);
      if (!buf || !buf->GetMappedSlice() || !buf->GetMappedSlice()->mapPtr()) return;
      stride = vbo.stride;
      int64_t first = api == 0 ? int64_t(a) : int64_t(int32_t(a)) + int64_t(b);
      count = api == 0 ? GetVertexCount(type, b) : c;
      size_t off = size_t(vbo.offset) + size_t(first) * stride;
      if (first < 0 || off + size_t(count) * stride > buf->Desc()->Size) return;
      base = static_cast<const uint8_t*>(buf->GetMappedSlice()->mapPtr()) + off;
    }
    if (!base || stride < 12 || count < 3) return;
    if (count > 64) { survey("too many vertices", count, 0.0f, nullptr); return; }   // a sprite or two, not a big effect mesh
    const float* w = reinterpret_cast<const float*>(&m_state.transforms[GetTransformIndex(D3DTS_WORLD)]);
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    float col[3] = { 0, 0, 0 }; bool haveCol = (fvf & D3DFVF_DIFFUSE) != 0;
    const UINT colOff = 12 + ((fvf & D3DFVF_NORMAL) ? 12 : 0);
    for (UINT i = 0; i < count; i++) {
      const float* p = reinterpret_cast<const float*>(base + size_t(i) * stride);
      for (int j = 0; j < 3; j++) {
        float v = p[0] * w[0 * 4 + j] + p[1] * w[1 * 4 + j] + p[2] * w[2 * 4 + j] + w[3 * 4 + j];
        lo[j] = std::min(lo[j], v); hi[j] = std::max(hi[j], v);
      }
      if (haveCol && colOff + 4 <= stride) {
        uint32_t d; std::memcpy(&d, base + size_t(i) * stride + colOff, 4);
        col[0] += float((d >> 16) & 255) / 255.0f; col[1] += float((d >> 8) & 255) / 255.0f; col[2] += float(d & 255) / 255.0f;
      }
    }
    float size = 0.0f; for (int j = 0; j < 3; j++) size = std::max(size, hi[j] - lo[j]);
    const float centre[3] = { 0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2]) };
    if (!additive) { survey("not additive", count, size, centre); return; }
    if (!(size > 2.0f && size < 400.0f)) { survey("size", count, size, centre); return; }   // candle flames and halos (characters ~1000 tall)
    survey("flame", count, size, centre);
    for (auto& v : col) v /= float(count);
    // a flame's colour: its vertex tint where it has one, warm candle light where it is plain white
    float c3[3] = { 1.0f, 0.62f, 0.30f };
    if (haveCol) {
      float mx = std::max(col[0], std::max(col[1], col[2]));
      if (mx < 0.05f) return;                               // faded out
      if (std::min(col[0], std::min(col[1], col[2])) < 0.85f * mx) for (int j = 0; j < 3; j++) c3[j] = col[j] / mx;
      for (auto& v : c3) v *= std::min(mx, 1.0f);
    }
    // small flames light a little way around them; bigger halos a bit further
    const float att[3] = { 0.6f, size < 80.0f ? 0.006f : 0.004f, 0.0f };
    SodLampNote(centre, c3, att, 900.0f, true);
  }


  uint32_t D3D9DeviceEx::SodLampList(::SodGeomLamp* out, uint32_t max, float strength) {
    const uint64_t now = m_sodRt.Frame();
    // forget what has not been seen for a second and a half (the room changed, the flame went out)
    m_sodLamps.erase(std::remove_if(m_sodLamps.begin(), m_sodLamps.end(),
      [now] (const SodLampEntry& e) { return now - e.last > 90u; }), m_sodLamps.end());
    // the most recently seen first; Direct3D lights before flames
    std::vector<const SodLampEntry*> v;
    for (const auto& e : m_sodLamps) v.push_back(&e);
    std::stable_sort(v.begin(), v.end(), [] (const SodLampEntry* x, const SodLampEntry* y) { return x->flame < y->flame; });
    static const float flameScale = [] { char s[16] = { }; return ::GetEnvironmentVariableA("SODRT_FLAME_STRENGTH", s, sizeof s) ? float(atof(s)) : 0.7f; }();
    uint32_t n = 0;
    for (const SodLampEntry* e : v) {
      if (n >= max) break;
      // fades in over a few frames (no pop when a lamp comes into view)
      const float fade = std::min(1.0f, float(now - e->first + 1) / 8.0f);
      SodGeomLamp& o = out[n++];
      for (int j = 0; j < 3; j++) { o.pos[j] = e->pos[j]; o.color[j] = e->col[j]; }
      o.range = e->range; o.att0 = e->att[0]; o.att1 = e->att[1]; o.att2 = e->att[2];
      o.strength = strength * fade * (e->flame ? flameScale : 1.0f);
    }
    return n;
  }

}
