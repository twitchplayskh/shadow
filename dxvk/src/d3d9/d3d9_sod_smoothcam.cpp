// Lebensbaum SMOOTH CAMERA. Some of the game's follow cameras move unevenly while the player walks at a constant
// speed: measured per frame 60..105 world units around a mean of 80 (surge, ease off, drop back), where a straight
// tracking camera in the same street moves a steady 76..80. The frames themselves arrive evenly; it is the camera
// that lurches. Here the camera POSITION of each frame is evened out before anything is drawn with it: its step
// follows the average of the last few raw steps, with a gentle pull back to the game's own position, so the
// camera glides at the walking speed and never drifts far from where the game puts it (the steady-state offset
// is zero when the raw movement is steady). The camera's orientation is the game's own. Cuts (jumps, sudden
// turns) are passed through untouched and reset the filter.
//
// Applied to: the frame's view matrix (SetTransform VIEW; the first one of each frame, and every later one equal
// to it) and to vertex-shader constants c0..c3 when they hold a matrix ending in the frame's view * projection
// (the shadow-volume shaders): those get the same change, so everything in the frame agrees.

#include "d3d9_device.h"

namespace dxvk {

  static D3DMATRIX SodCamMul(const D3DMATRIX& a, const D3DMATRIX& b) {
    D3DMATRIX o;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
      float s = 0.0f; for (int k = 0; k < 4; k++) s += a.m[i][k] * b.m[k][j]; o.m[i][j] = s;
    }
    return o;
  }

  static bool SodCamInverse(const D3DMATRIX& m, D3DMATRIX& o) {
    double a[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { a[i][j] = m.m[i][j]; a[i][j + 4] = i == j ? 1.0 : 0.0; }
    for (int c = 0; c < 4; c++) {
      int p = c;
      for (int r = c + 1; r < 4; r++) if (std::abs(a[r][c]) > std::abs(a[p][c])) p = r;
      if (std::abs(a[p][c]) < 1e-12) return false;
      for (int j = 0; j < 8; j++) std::swap(a[c][j], a[p][j]);
      const double d = a[c][c];
      for (int j = 0; j < 8; j++) a[c][j] /= d;
      for (int r = 0; r < 4; r++) if (r != c) { const double f = a[r][c]; for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) o.m[i][j] = float(a[i][j + 4]);
    return true;
  }

  // camera position of a view matrix (row vectors: p_view = p_world * V, V = [R 0; t 1]) -> -t * R^T
  static void SodCamPos(const D3DMATRIX& v, float out[3]) {
    for (int k = 0; k < 3; k++) out[k] = -(v.m[3][0] * v.m[k][0] + v.m[3][1] * v.m[k][1] + v.m[3][2] * v.m[k][2]);
  }

  bool D3D9DeviceEx::SodSmoothCamEnabled() const {
    return m_sodRt.SmoothCamera() > 0;
  }

  // the frame's first view matrix: run the filter and work out the replacement
  void D3D9DeviceEx::SodSmoothCamBegin(const D3DMATRIX& raw) {
    m_sodScRaw = raw;
    m_sodScFrameSet = true;
    m_sodScActive = false;
    float r[3]; SodCamPos(raw, r);
    // orientation change since the last frame (third column = view direction)
    const float fwd[3] = { raw.m[0][2], raw.m[1][2], raw.m[2][2] };
    const float dot = fwd[0] * m_sodScFwd[0] + fwd[1] * m_sodScFwd[1] + fwd[2] * m_sodScFwd[2];
    const float dx = r[0] - m_sodScLastRaw[0], dy = r[1] - m_sodScLastRaw[1], dz = r[2] - m_sodScLastRaw[2];
    const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
    // cut: a jump, more than ~5.7 degrees of turn, or a single step far beyond the current speed (the game's camera
    // snapping to a new spot along its path: measured 325 units in one frame while walking at 80 per frame)
    const float vel = std::sqrt(m_sodScVel[0] * m_sodScVel[0] + m_sodScVel[1] * m_sodScVel[1] + m_sodScVel[2] * m_sodScVel[2]);
    const bool snap = m_sodScHistN > 0 && step > std::max(250.0f, 3.0f * vel);
    const bool cut = !m_sodScValid || step > 600.0f || dot < 0.995f || snap;
    std::memcpy(m_sodScFwd, fwd, sizeof fwd);
    std::memcpy(m_sodScLastRaw, r, sizeof r);
    if (cut) {
      m_sodScValid = true;
      std::memcpy(m_sodScPos, r, sizeof r);
      m_sodScHistN = 0; m_sodScHistI = 0;
      m_sodScVel[0] = m_sodScVel[1] = m_sodScVel[2] = 0.0f;
      return;
    }
    // the camera's speed follows the average of the last N raw steps (N = 15, half a second); right after a cut it
    // starts at the first raw step. The position moves by that speed, then is pulled towards the game's position:
    // gently (3 %) while close, up to 50 % when it falls 100 units behind or ahead, so it never strays far.
    const bool first = m_sodScHistN == 0;
    float* h = m_sodScHist[m_sodScHistI];
    h[0] = dx; h[1] = dy; h[2] = dz;
    m_sodScHistI = (m_sodScHistI + 1) % kSodScHist;
    m_sodScHistN = std::min(m_sodScHistN + 1, kSodScHist);
    float avg[3] = { };
    for (uint32_t i = 0; i < m_sodScHistN; i++) for (int k = 0; k < 3; k++) avg[k] += m_sodScHist[i][k];
    for (int k = 0; k < 3; k++) {
      avg[k] /= float(m_sodScHistN);
      m_sodScVel[k] = first ? h[k] : m_sodScVel[k] + 0.5f * (avg[k] - m_sodScVel[k]);
    }
    float pred[3], e2 = 0.0f;
    for (int k = 0; k < 3; k++) { pred[k] = m_sodScPos[k] + m_sodScVel[k]; e2 += (r[k] - pred[k]) * (r[k] - pred[k]); }
    const float pull = 0.03f + 0.47f * std::min(1.0f, e2 / (100.0f * 100.0f));
    float s[3], off = 0.0f;
    for (int k = 0; k < 3; k++) { s[k] = pred[k] + pull * (r[k] - pred[k]); off += (s[k] - r[k]) * (s[k] - r[k]); }
    if (off > 400.0f * 400.0f) { std::memcpy(s, r, sizeof s); m_sodScHistN = 0; }   // lost it: back to the game's camera
    std::memcpy(m_sodScPos, s, sizeof s);
    // replacement view: same rotation, translation for the smoothed position (t = -s * R)
    D3DMATRIX v = raw;
    for (int j = 0; j < 3; j++) v.m[3][j] = -(s[0] * raw.m[0][j] + s[1] * raw.m[1][j] + s[2] * raw.m[2][j]);
    m_sodScView = v;
    m_sodScActive = (s[0] != r[0] || s[1] != r[1] || s[2] != r[2]);
  }

  // SetTransform(VIEW): returns the matrix to store
  const D3DMATRIX* D3D9DeviceEx::SodSmoothCamView(const D3DMATRIX* m) {
    if (!m || m_sodInternal || !SodSmoothCamEnabled()) return m;
    if (!m_sodScFrameSet) SodSmoothCamBegin(*m);
    if (!m_sodScActive || std::memcmp(m, &m_sodScRaw, sizeof(D3DMATRIX))) return m;
    return &m_sodScView;
  }

  // SetVertexShaderConstantF(0, .., >= 4): c0..c3 = transpose(M); if M = X * V * P (X affine), use X * V' * P
  bool D3D9DeviceEx::SodSmoothCamConstants(const float* in, float* out) {
    if (!m_sodScActive || m_sodInternal || !SodSmoothCamEnabled()) return false;
    D3DMATRIX M, P, VP, VPi, Vs;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) M.m[i][j] = in[j * 4 + i];
    std::memcpy(&P, &m_state.transforms[GetTransformIndex(D3DTS_PROJECTION)], sizeof P);
    VP = SodCamMul(m_sodScRaw, P);
    if (!SodCamInverse(VP, VPi)) return false;
    const D3DMATRIX X = SodCamMul(M, VPi);
    const float tol = 1e-3f * (std::abs(X.m[3][3]) + 1.0f);
    if (std::abs(X.m[0][3]) > tol || std::abs(X.m[1][3]) > tol || std::abs(X.m[2][3]) > tol || std::abs(X.m[3][3] - 1.0f) > 1e-2f)
      return false;                                                        // not this camera's view * projection
    Vs = SodCamMul(SodCamMul(X, m_sodScView), P);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[j * 4 + i] = Vs.m[i][j];
    return true;
  }

}
