// Shadow WORLD SHADOWS (part of RAY TRACING). The rooms and streets are drawn fixed-function with FVF 0x142
// (position, baked vertex colour, one texture), already in world space. Their triangles become occluders for the
// ray-traced shadow pass, so buildings, pillars, lamp posts and furniture cast real shadows from the same lights
// the characters' shadows use. Their vertex colours go along: the shader weakens a world shadow where the baked
// lighting is already dark, so the bake's own shadows are not darkened twice.
//
// The area's draws are kept by identity (buffers and range) across frames, so geometry the camera turns away
// from still casts shadows into view; a draw not seen for 3 seconds is dropped (another area), at once when most
// of a frame's draws are new. The helper gets
// the whole set through its own shared mapping (two halves, published by a generation number) and rebuilds one
// acceleration structure for it only when the set changes, at most twice a second.
//
// Not occluders: alpha-blended draws (glass, effects), draws without depth writes, anything larger than 60000 units
// across (sky domes), and cut-out (alpha-tested) draws whose texture is largely see-through (foliage cards and
// railings would cast solid squares). The game alpha-tests nearly everything, buildings included, so a cut-out
// draw still casts when at least 95 % of its texture passes the test.
//
// Geometry the game rewrites every frame (flags, cloth: drawn from a buffer refilled each frame, at a new place in
// it each time) does not go into the area's set: it is read again every frame and goes with the frame's request
// like the characters, so its shadow moves with it.

#include "d3d9_device.h"
#include "d3d9_sod_link.h"
#include "../sod_bridge/sod_bridge_proto.h"

namespace dxvk {

  static inline uint64_t SodWorldMix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
  }

  // WORLD SHADOWS: the frame's first view matrix; a jump or a sharp turn from the last frame's is a camera cut
  void D3D9DeviceEx::SodWorldView(const D3DMATRIX* m) {
    if (!m || m_sodInternal || m_sodLink == nullptr) return;
    const uint32_t frame = uint32_t(m_sodRt.Frame());
    if (frame == m_sodWvFrame && m_sodWvValid) return;
    m_sodWvFrame = frame;
    float pos[3], fwd[3] = { m->m[0][2], m->m[1][2], m->m[2][2] };
    for (int k = 0; k < 3; k++) pos[k] = -(m->m[3][0] * m->m[k][0] + m->m[3][1] * m->m[k][1] + m->m[3][2] * m->m[k][2]);
    if (m_sodWvValid) {
      const float dx = pos[0] - m_sodWvPos[0], dy = pos[1] - m_sodWvPos[1], dz = pos[2] - m_sodWvPos[2];
      const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
      const float dot = fwd[0] * m_sodWvFwd[0] + fwd[1] * m_sodWvFwd[1] + fwd[2] * m_sodWvFwd[2];
      if (step > 600.0f || dot < 0.97f) m_sodLink->NoteCut(frame);
    }
    std::memcpy(m_sodWvPos, pos, sizeof pos); std::memcpy(m_sodWvFwd, fwd, sizeof fwd);
    m_sodWvValid = true;
  }

  void D3D9DeviceEx::SodWorldObserve(uint32_t api, D3DPRIMITIVETYPE type, UINT primCount, INT baseVertex, UINT minIndex,
                                     UINT numVertices, UINT startIndex) {
    static const bool off = [] { char v[8] = { }; return ::GetEnvironmentVariableA("SODRT_WORLD", v, 8) && v[0] == '0'; }();
    if (off || m_sodLink == nullptr || m_sodRt.RtLevel() <= 0 || !primCount) return;
    if (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP && type != D3DPT_TRIANGLEFAN) return;
    const auto& rs = m_state.renderStates;
    if (rs[D3DRS_ALPHABLENDENABLE])                  { m_sodLink->NoteWorldSkip(0); return; }
    if (!rs[D3DRS_ZENABLE] || !rs[D3DRS_ZWRITEENABLE]) { m_sodLink->NoteWorldSkip(2); return; }
    if (rs[D3DRS_ALPHATESTENABLE] && !SodWorldTexSolid(m_state.textures[0], rs[D3DRS_ALPHAREF], rs[D3DRS_ALPHAFUNC]))
      { m_sodLink->NoteWorldSkip(1); return; }
    const D3D9VBO& vbo = m_state.vertexBuffers[0];
    D3D9CommonBuffer* vb = GetCommonBuffer(vbo.vertexBuffer);
    if (!vb || vbo.stride < 16) return;
    D3D9CommonBuffer* ib = api == 1 ? GetCommonBuffer(m_state.indices) : nullptr;
    if (api == 1 && !ib) return;

    float wm[16];
    std::memcpy(wm, &m_state.transforms[GetTransformIndex(D3DTS_WORLD)], 64);
    uint64_t id = 0x5EED;
    for (uint64_t v : { uint64_t(reinterpret_cast<uintptr_t>(vb)), uint64_t(reinterpret_cast<uintptr_t>(ib)),
                        uint64_t(vbo.offset), uint64_t(vbo.stride), uint64_t(startIndex), uint64_t(primCount),
                        uint64_t(uint32_t(baseVertex)), uint64_t(minIndex), uint64_t(numVertices), uint64_t(type), uint64_t(api) })
      id = SodWorldMix(id, v);
    uint64_t stamp = SodWorldMix(vb->SodWriteGen(), ib ? ib->SodWriteGen() : 0u);
    for (int i = 0; i < 16; i++) { uint32_t b; std::memcpy(&b, &wm[i], 4); stamp = SodWorldMix(stamp, b); }
    const uint32_t frame = uint32_t(m_sodRt.Frame());
    uint64_t object = 0x0B1EC7, variant = 0x7A21A7;
    for (uint64_t v : { uint64_t(reinterpret_cast<uintptr_t>(vb)), uint64_t(reinterpret_cast<uintptr_t>(ib)),
                        uint64_t(vbo.offset), uint64_t(vbo.stride), uint64_t(primCount), uint64_t(type), uint64_t(api) })
      object = SodWorldMix(object, v);
    for (uint64_t v : { uint64_t(startIndex), uint64_t(uint32_t(baseVertex)), uint64_t(minIndex), uint64_t(numVertices) })
      variant = SodWorldMix(variant, v);
    const bool posed = m_sodLink->PosesAnimated(object, variant, frame);
    const bool animated = posed
                       || m_sodLink->BufferAnimated(reinterpret_cast<uintptr_t>(vb), vb->SodWriteGen(), frame)
                       || (ib && m_sodLink->BufferAnimated(reinterpret_cast<uintptr_t>(ib), ib->SodWriteGen(), frame));
    if (!animated && m_sodLink->WorldSeen(id, stamp, frame)) return;

    // read the draw: positions (offset 0) and baked colours (offset 12) of every triangle
    const uint8_t* vdata = vb->GetMappedSlice() ? static_cast<const uint8_t*>(vb->GetMappedSlice()->mapPtr()) : nullptr;
    if (!vdata) return;
    const size_t vbSize = vb->Desc()->Size;
    const uint32_t count = GetVertexCount(type, primCount);
    const uint8_t* idata = nullptr; bool i32 = false;
    if (ib) {
      idata = ib->GetMappedSlice() ? static_cast<const uint8_t*>(ib->GetMappedSlice()->mapPtr()) : nullptr;
      i32 = ib->Desc()->Format == D3D9Format::INDEX32;
      if (!idata || (size_t(startIndex) + count) * (i32 ? 4u : 2u) > ib->Desc()->Size) return;
      idata += size_t(startIndex) * (i32 ? 4u : 2u);
    }
    auto vertexAt = [&](uint32_t k, uint32_t& v) -> bool {     // k-th vertex of the draw -> vertex number
      int64_t n;
      if (ib) n = int64_t(baseVertex) + int64_t(i32 ? reinterpret_cast<const uint32_t*>(idata)[k] : reinterpret_cast<const uint16_t*>(idata)[k]);
      else    n = int64_t(baseVertex) + k;                      // DrawPrimitive: baseVertex = StartVertex
      if (n < 0 || size_t(vbo.offset) + size_t(n) * vbo.stride + 16 > vbSize) return false;
      v = uint32_t(n); return true;
    };
    std::vector<float> tris; std::vector<uint32_t> cols;
    tris.reserve(size_t(primCount) * 9); cols.reserve(size_t(primCount) * 3);
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
      uint32_t v[3];
      if (!vertexAt(a, v[0]) || !vertexAt(b, v[1]) || !vertexAt(c, v[2])) return;
      if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) return;
      for (uint32_t i : v) {
        const uint8_t* q = vdata + vbo.offset + size_t(i) * vbo.stride;
        float p[3]; std::memcpy(p, q, 12);
        uint32_t col; std::memcpy(&col, q + 12, 4);
        for (int j = 0; j < 3; j++) {
          float x = p[0] * wm[0 * 4 + j] + p[1] * wm[1 * 4 + j] + p[2] * wm[2 * 4 + j] + wm[3 * 4 + j];
          tris.push_back(x); lo[j] = std::min(lo[j], x); hi[j] = std::max(hi[j], x);
        }
        cols.push_back(col);
      }
    };
    if (type == D3DPT_TRIANGLELIST)       for (uint32_t k = 0; k + 2 < count; k += 3) emit(k, k + 1, k + 2);
    else if (type == D3DPT_TRIANGLESTRIP) for (uint32_t k = 0; k + 2 < count; k++) (k & 1) ? emit(k + 1, k, k + 2) : emit(k, k + 1, k + 2);
    else                                  for (uint32_t k = 1; k + 1 < count; k++) emit(0, k, k + 1);
    if (!tris.empty() && std::max({ hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] }) > 60000.0f) {
      m_sodLink->NoteWorldSkip(3);                             // sky dome / backdrop: kept as seen, without triangles
      tris.clear(); cols.clear();
    }
    if (animated) m_sodLink->OnDynamicDraw(tris);
    else m_sodLink->OnWorldDraw(id, stamp, frame, reinterpret_cast<uintptr_t>(vb), object, std::move(tris), std::move(cols));
  }

  // cut-out texture: does (nearly) all of it pass the alpha test? Judged once per texture from its texels.
  static bool SodAlphaPass(uint32_t a, uint32_t ref, uint32_t func) {
    switch (func) {
      case D3DCMP_GREATER:      return a > ref;
      case D3DCMP_GREATEREQUAL: return a >= ref;
      case D3DCMP_LESS:         return a < ref;
      case D3DCMP_LESSEQUAL:    return a <= ref;
      case D3DCMP_EQUAL:        return a == ref;
      case D3DCMP_NOTEQUAL:     return a != ref;
      case D3DCMP_NEVER:        return false;
      default:                  return true;
    }
  }

  bool D3D9DeviceEx::SodWorldTexSolid(IDirect3DBaseTexture9* bt, DWORD ref, DWORD func) {
    if (!bt) return true;
    const uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(bt)) << 16) ^ (uint64_t(ref & 255u) << 4) ^ (func & 15u);
    auto it = m_sodWorldTexSolid.find(key);
    if (it != m_sodWorldTexSolid.end()) return it->second != 0;
    bool solid = false, readable = false;
    if (bt->GetType() == D3DRTYPE_TEXTURE) {
      IDirect3DTexture9* t = static_cast<IDirect3DTexture9*>(bt);
      D3DSURFACE_DESC desc = { };
      D3DLOCKED_RECT lr = { };
      if (SUCCEEDED(t->GetLevelDesc(0, &desc)) && desc.Width && desc.Height && SUCCEEDED(t->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) {
        readable = true;
        uint64_t pass = 0, total = 0;
        const uint32_t w = desc.Width, h = desc.Height;
        auto count = [&](uint32_t a) { total++; if (SodAlphaPass(a, ref & 255u, func)) pass++; };
        const uint8_t* bits = static_cast<const uint8_t*>(lr.pBits);
        switch (desc.Format) {
          case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_R8G8B8: case D3DFMT_X4R4G4B4:
            pass = total = 1; break;                                   // no alpha: always passes
          case D3DFMT_A8R8G8B8:
            for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) count(bits[size_t(y) * lr.Pitch + x * 4 + 3]);
            break;
          case D3DFMT_A4R4G4B4:
            for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
              uint16_t v; std::memcpy(&v, bits + size_t(y) * lr.Pitch + x * 2, 2); count((v >> 12) * 17u); }
            break;
          case D3DFMT_A1R5G5B5:
            for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
              uint16_t v; std::memcpy(&v, bits + size_t(y) * lr.Pitch + x * 2, 2); count((v >> 15) ? 255u : 0u); }
            break;
          case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: {
            const bool d1 = desc.Format == D3DFMT_DXT1, d3 = desc.Format == D3DFMT_DXT2 || desc.Format == D3DFMT_DXT3;
            const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bsz = d1 ? 8u : 16u;
            for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
              const uint8_t* b = bits + size_t(by) * lr.Pitch + size_t(bx) * bsz;
              if (d1) {
                uint16_t c0, c1; uint32_t idx; std::memcpy(&c0, b, 2); std::memcpy(&c1, b + 2, 2); std::memcpy(&idx, b + 4, 4);
                for (int i = 0; i < 16; i++) count(c0 <= c1 && ((idx >> (2 * i)) & 3u) == 3u ? 0u : 255u);
              } else if (d3) {
                for (int i = 0; i < 16; i++) count(((b[i / 2] >> ((i & 1) * 4)) & 15u) * 17u);
              } else {
                uint32_t a[8] = { b[0], b[1] };
                if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
                else { for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5; a[6] = 0; a[7] = 255; }
                uint64_t bitsA = 0; for (int i = 0; i < 6; i++) bitsA |= uint64_t(b[2 + i]) << (8 * i);
                for (int i = 0; i < 16; i++) count(a[(bitsA >> (3 * i)) & 7u]);
              }
            }
            break;
          }
          default: readable = false; break;
        }
        t->UnlockRect(0);
        if (readable) solid = total && double(pass) >= 0.95 * double(total);
      }
    }
    if (!readable) solid = true;                                     // cannot tell: the game's surfaces are mostly solid
    m_sodWorldTexSolid[key] = solid ? 1 : 0;
    if (m_sodLink != nullptr) m_sodLink->NoteWorldTexture(solid, readable);
    return solid;
  }

  namespace sod {

    bool BridgeLink::WorldSeen(uint64_t id, uint64_t stamp, uint32_t frame) {
      auto it = m_world.find(id);
      if (it == m_world.end() || it->second.stamp != stamp) { CountAreaDraw(frame, it != m_world.end()); return false; }
      it->second.lastFrame = frame;
      CountAreaDraw(frame, true);
      return true;
    }

    bool BridgeLink::BufferAnimated(uintptr_t key, uint32_t gen, uint32_t frame) {
      auto it = m_bufGen.find(key);
      if (it == m_bufGen.end()) { m_bufGen[key] = { gen, frame - 1000u, false }; return false; }
      BufGen& b = it->second;
      if (b.gen != gen) { b.gen = gen; b.changed = frame; }
      const bool now = frame - b.changed < 60u;                 // rewritten within the last two seconds
      if (now && !b.animated) {                                 // became animated: its copies leave the area's set
        for (auto w = m_world.begin(); w != m_world.end(); ) {
          if (w->second.vb == key) { w = m_world.erase(w); m_worldDirty = true; } else ++w;
        }
      }
      b.animated = now;
      return now;
    }

    bool BridgeLink::PosesAnimated(uint64_t object, uint64_t variant, uint32_t frame) {
      PoseObj& o = m_poses[object];
      if (o.frame != frame) { o.frame = frame; o.now = 0; }
      o.now++;
      o.most = std::max(o.most, o.now);
      if (o.animated) return true;
      if (std::find(o.seen.begin(), o.seen.end(), variant) == o.seen.end() && o.seen.size() < 256) o.seen.push_back(variant);
      // more different poses over time than drawn at once (and at least three): an animation
      if (o.seen.size() >= 3 && o.seen.size() > o.most + 1) {
        o.animated = true; m_poseObjects++;
        for (auto w = m_world.begin(); w != m_world.end(); ) {        // its stored poses leave the area's set
          if (w->second.object == object) { w = m_world.erase(w); m_worldDirty = true; } else ++w;
        }
        o.seen.clear(); o.seen.shrink_to_fit();
        return true;
      }
      return false;
    }

    void BridgeLink::OnWorldDraw(uint64_t id, uint64_t stamp, uint32_t frame, uintptr_t vb, uint64_t object, std::vector<float>&& tris, std::vector<uint32_t>&& cols) {
      WorldDraw& d = m_world[id];
      d.vb = vb; d.object = object;
      const bool changed = d.stamp != stamp || d.tris.size() != tris.size() || d.tris != tris;
      d.stamp = stamp; d.lastFrame = frame;
      if (changed) { d.tris = std::move(tris); d.cols = std::move(cols); m_worldDirty = true; }
    }

    void BridgeLink::UpdateWorld(uint32_t frame) {
      // camera cut (entering a door, switching to another fixed camera): everything not drawn since the cut
      // leaves at once, so a room never inherits the street's geometry
      if (m_cutPending && frame - m_cutFrame < 1000u) {
        m_cutPending = false;
        size_t before = m_world.size();
        for (auto it = m_world.begin(); it != m_world.end(); ) {
          if (it->second.lastFrame - m_cutFrame >= 1000u) { it = m_world.erase(it); m_worldDirty = true; } else ++it;
        }
        if (before != m_world.size()) m_worldLastWrite = frame - 1000u;   // publish now
        if (m_cuts++ < 50)
          Logger::info(str::format("SoD-RT: world shadows: camera cut: ", before - m_world.size(), " room draws dropped, ",
            m_world.size(), " kept"));
      }
      // another area (a door, a cut): most of this frame's room draws are new. What was kept from the area before
      // (geometry behind the camera) would otherwise sit in the new one for 20 seconds, overlapping it and
      // casting shadows into it: only what this frame drew stays.
      if (m_areaFrame == frame && m_areaNew >= 20 && m_areaNew > 2 * m_areaKnown && !m_world.empty()) {
        size_t before = m_world.size();
        for (auto it = m_world.begin(); it != m_world.end(); ) {
          if (it->second.lastFrame != frame) { it = m_world.erase(it); m_worldDirty = true; } else ++it;
        }
        m_worldLastWrite = frame - 1000u;                          // publish now, not after the half-second wait
        if (m_areaChanges++ < 50)
          Logger::info(str::format("SoD-RT: world shadows: new area (", m_areaNew, " new room draws, ", m_areaKnown,
            " known): ", before - m_world.size(), " draws of the previous area dropped"));
      }
      // geometry not drawn for 3 seconds leaves the set: long enough to keep what a quick camera turn hides,
      // short enough that the street does not stay over a shop entered from it (the shop's own draws were
      // already known from the street, so no "new area" is seen: the street just stops being drawn)
      size_t pruned = 0;
      for (auto it = m_world.begin(); it != m_world.end(); ) {
        if (frame - it->second.lastFrame > 90u) { it = m_world.erase(it); m_worldDirty = true; pruned++; }
        else ++it;
      }
      if (pruned >= 8 && m_areaChanges++ < 100)
        Logger::info(str::format("SoD-RT: world shadows: ", pruned, " room draws not seen for 3 s dropped, ", m_world.size(), " kept"));
      if (!m_worldDirty) return;
      if (m_worldTris && frame - m_worldLastWrite < 15u) return;    // at most twice a second
      if (!m_worldView) {
        m_worldMap = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(SOD_WORLD_BYTES),
                                          str::format(SOD_BRIDGE_WORLD, m_gamePid).c_str());
        if (m_worldMap) m_worldView = static_cast<uint8_t*>(::MapViewOfFile(m_worldMap, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!m_worldView) { Logger::warn("SoD-RT: world shadows: could not create the world mapping"); m_worldDirty = false; return; }
      }
      auto* h = reinterpret_cast<SodWorldHeader*>(m_worldView);
      const uint32_t gen = h->generation, half = (gen + 1u) & 1u;
      uint8_t* base = m_worldView + sizeof(SodWorldHeader) + size_t(half) * SOD_WORLD_HALF_BYTES;
      float* outT = reinterpret_cast<float*>(base);
      uint32_t* outC = reinterpret_cast<uint32_t*>(base + size_t(SOD_WORLD_MAX_TRIS) * 36);
      uint32_t n = 0, draws = 0;
      uint32_t hist[256] = { }, verts = 0;
      for (const auto& kv : m_world) {
        const WorldDraw& d = kv.second;
        uint32_t t = uint32_t(d.tris.size() / 9);
        if (!t) continue;
        if (n + t > SOD_WORLD_MAX_TRIS) t = SOD_WORLD_MAX_TRIS - n;
        if (!t) break;
        std::memcpy(outT + size_t(n) * 9, d.tris.data(), size_t(t) * 36);
        std::memcpy(outC + size_t(n) * 3, d.cols.data(), size_t(t) * 12);
        for (uint32_t i = 0; i < t * 3; i++) {
          const uint32_t c = d.cols[i];
          const uint32_t lum = (299u * ((c >> 16) & 255u) + 587u * ((c >> 8) & 255u) + 114u * (c & 255u)) / 1000u;
          hist[std::min(lum, 255u)]++; verts++;
        }
        n += t; draws++;
      }
      // the area's fully lit baked colour: the 90th percentile of the vertex colours' brightness
      float litRef = 0.5f;
      if (verts) {
        uint32_t acc = 0, target = verts - verts / 10;
        for (uint32_t i = 0; i < 256; i++) { acc += hist[i]; if (acc >= target) { litRef = float(i) / 255.0f; break; } }
      }
      h->triCount[half] = n;
      h->litRef[half] = litRef;
      ::MemoryBarrier();
      h->magic = SOD_WORLD_MAGIC;
      h->generation = gen + 1u;
      m_worldTris = n; m_worldDirty = false; m_worldLastWrite = frame;
      if (m_worldWrites++ < 20 || (m_worldWrites % 50u) == 0)
        Logger::info(str::format("SoD-RT: world shadows: ", n, " tris from ", draws, " room draws (lit reference ", litRef,
          "); animated objects (poses): ", m_poseObjects, "; cut-out textures: ", m_worldTex[0], " solid, ", m_worldTex[1], " see-through (", m_worldTex[2], " unreadable)",
          "; not occluders so far: ", m_worldSkips[0], " blended, ", m_worldSkips[1], " see-through cut-outs, ",
          m_worldSkips[2], " without depth writes, ", m_worldSkips[3], " sky-sized"));
    }

  }

}
