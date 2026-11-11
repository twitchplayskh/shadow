// Lebensbaum diagnostics: camera / frame-pacing trace. Started with F10 in the game or by creating the file
// %LOCALAPPDATA%\SoDRT\camtrace.go (deleted when picked up). Records 600 presented frames to
// %LOCALAPPDATA%\SoDRT\camtrace.csv: present time, the camera (inverse of the first room draw's view matrix) and
// the world position of the first character part drawn (fixed-function FVF 0x1D2 without stencil). A frame whose
// camera and character are both unchanged while the player is moving shows the game's simulation did not tick.

#include "d3d9_device.h"

#include <cstdio>

namespace dxvk {

  static std::string SodTraceDir() {
    char buf[MAX_PATH] = { };
    DWORD n = ::GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return std::string();
    return std::string(buf) + "\\SoDRT";
  }

  void D3D9DeviceEx::SodCamObserve() {
    if (!m_sodCamFile || m_sodInternal || m_state.vertexShader.ptr() || !m_state.vertexDecl) return;
    const DWORD fvf = m_state.vertexDecl->GetFVF();
    if (!m_sodCamHaveView && fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)) {
      std::memcpy(m_sodCamView, &m_state.transforms[GetTransformIndex(D3DTS_VIEW)], sizeof(m_sodCamView));
      m_sodCamHaveView = true;
    }
    if (!m_sodCamHaveChar && fvf == 0x1D2u && !m_state.renderStates[D3DRS_STENCILENABLE]) {
      const auto& W = m_state.transforms[GetTransformIndex(D3DTS_WORLD)];
      m_sodCamChar[0] = W[3][0]; m_sodCamChar[1] = W[3][1]; m_sodCamChar[2] = W[3][2];
      m_sodCamHaveChar = true;
    }
    m_sodCamDraws++;
  }

  void D3D9DeviceEx::SodCamFrame() {
    LARGE_INTEGER now, freq;
    ::QueryPerformanceCounter(&now); ::QueryPerformanceFrequency(&freq);
    // start: F10 (edge) or the trigger file
    const bool key = (::GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    bool start = key && !m_sodCamKey;
    m_sodCamKey = key;
    if (!m_sodCamFile && (m_sodCamPoll++ % 30u) == 0u) {
      const std::string go = SodTraceDir() + "\\camtrace.go";
      if (::GetFileAttributesA(go.c_str()) != INVALID_FILE_ATTRIBUTES) { ::DeleteFileA(go.c_str()); start = true; }
    }
    if (m_sodCamFile) {
      // the frame that just ended
      const double t = double(now.QuadPart - m_sodCamT0) * 1000.0 / double(freq.QuadPart);
      float cam[3] = { }, fwd[3] = { };
      if (m_sodCamHaveView) {
        // view = [R | 0; t 1] (row vectors): camera position = -t * R^T, forward = third column
        const float* v = m_sodCamView;
        for (int k = 0; k < 3; k++) cam[k] = -(v[12] * v[k * 4 + 0] + v[13] * v[k * 4 + 1] + v[14] * v[k * 4 + 2]);
        fwd[0] = v[2]; fwd[1] = v[6]; fwd[2] = v[10];
      }
      float raw[3] = { };                            // the game's own camera (before SMOOTH CAMERA)
      if (m_sodScFrameSet) {
        const D3DMATRIX& v = m_sodScRaw;
        for (int k = 0; k < 3; k++) raw[k] = -(v.m[3][0] * v.m[k][0] + v.m[3][1] * v.m[k][1] + v.m[3][2] * v.m[k][2]);
      } else std::memcpy(raw, cam, sizeof raw);
      std::fprintf(static_cast<FILE*>(m_sodCamFile), "%u,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.5f,%.5f,%.5f,%d,%.3f,%.3f,%.3f,%u,%.3f,%.3f,%.3f\n",
        m_sodCamCount, t, t - m_sodCamLast, int(m_sodCamHaveView), cam[0], cam[1], cam[2], fwd[0], fwd[1], fwd[2],
        int(m_sodCamHaveChar), m_sodCamChar[0], m_sodCamChar[1], m_sodCamChar[2], m_sodCamDraws, raw[0], raw[1], raw[2]);
      m_sodCamLast = t;
      if (++m_sodCamCount >= 600u) {
        std::fclose(static_cast<FILE*>(m_sodCamFile)); m_sodCamFile = nullptr;
        Logger::info("SoD: camera trace: 600 frames written to camtrace.csv");
      }
    } else if (start) {
      const std::string path = SodTraceDir() + "\\camtrace.csv";
      FILE* fp = std::fopen(path.c_str(), "w");
      if (fp) {
        std::fprintf(fp, "frame,t_ms,dt_ms,cam_ok,cam_x,cam_y,cam_z,fwd_x,fwd_y,fwd_z,char_ok,char_x,char_y,char_z,draws,raw_x,raw_y,raw_z\n");
        m_sodCamFile = fp; m_sodCamCount = 0; m_sodCamT0 = now.QuadPart; m_sodCamLast = 0.0;
        Logger::info(str::format("SoD: camera trace started -> ", path));
      }
    }
    m_sodCamHaveView = m_sodCamHaveChar = false;
    m_sodCamDraws = 0;
  }

}
