// SoD PROFILER (see d3d9_sod_prof.h)
#include "d3d9_sod_prof.h"
#include "d3d9_device.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sod::prof {

  using namespace dxvk;

  namespace {

    const char* kZoneName[ZCount] = {
      "text redraw", "geometry build", "geometry draws", "texture upload", "texture network", "RT hand-off",
      "RT shadow pass", "AO + reflections", "feature upkeep", "smooth camera", "game creates resources", "present"
    };

    int64_t Qpc() { LARGE_INTEGER q; ::QueryPerformanceCounter(&q); return q.QuadPart; }
    double  QpcMs(int64_t t) { static const double f = [] { LARGE_INTEGER q; ::QueryPerformanceFrequency(&q); return 1000.0 / double(q.QuadPart); }(); return double(t) * f; }

    // ---------------------------------------------------------------- one frame
    struct Frame {
      uint64_t no = 0;
      int64_t  start = 0;
      double   ms = 0.0;
      int64_t  zone[ZCount] = { };
      uint32_t count[ZCount] = { };
      // DXVK (deltas)
      uint64_t pipes = 0, csSyncs = 0, csSyncUs = 0, gpuSyncs = 0, gpuSyncUs = 0, gpuIdleUs = 0, submits = 0, draws = 0;
      // SoDFix (deltas, QPC ticks / counts)
      int64_t  fOpenT = 0, fOpens = 0, fReadT = 0, fReads = 0, fBytes = 0, padT = 0;
    };

    // ---------------------------------------------------------------- sampler
    struct Sample {
      uint64_t frame;
      uint32_t eip;
      uint32_t caller;        // first return address into the game's own code found on the stack (0: none)
    };

    struct State {
      bool     tried = false, on = false;
      DWORD    mainTid = 0;
      HANDLE   mainThread = nullptr;
      // zones: exclusive time, a stack for nesting
      Zone     stack[16]; int depth = 0; int64_t segStart = 0;
      Frame    cur;
      int64_t  lastPresent = 0;
      uint64_t frameNo = 0;
      std::vector<double> recent;           // the last 120 frame times
      size_t   recentPos = 0;
      // DXVK / SoDFix running totals at the last frame boundary
      DxvkStatCounters prevCtr;
      bool     havePrevCtr = false;
      int64_t  prevFix[6] = { };
      int (__cdecl* fixCounters)(int64_t*, int) = nullptr;
      int (__cdecl* fixFiles)(int64_t, char*, int) = nullptr;
      // sampler
      std::thread sampler;
      std::atomic<bool> stop { false };
      std::atomic<int64_t> frameStart { 0 };
      std::atomic<uint64_t> frameAtomic { 0 };
      std::atomic<bool> sessionOn { false };
      std::mutex sampleLock;
      std::vector<Sample> samples;          // this frame's (and, in a session, everything's) samples
      // output
      std::string dir;
      FILE* hitchLog = nullptr;
      uint32_t hitches = 0;
      // session
      FILE* csv = nullptr; std::string sessionName; int64_t sessionStart = 0;
      std::vector<Frame> sessionFrames;
      std::unordered_map<uint64_t, uint32_t> sessionLoc;   // (location key) -> samples
      uint64_t sessionSamples = 0;
      struct Hitch { double ms; double median; std::string text; };
      std::vector<Hitch> sessionHitches;
      bool f9Down = false;
      int64_t lastGoCheck = 0;
    };

    State& S() { static State s; return s; }

    // ---------------------------------------------------------------- where an address is
    struct ModInfo { std::string name; uintptr_t base = 0; };
    ModInfo ModuleOf(uintptr_t a) {
      static std::unordered_map<uintptr_t, ModInfo> cache;     // by 64 KB page
      auto it = cache.find(a >> 16);
      if (it != cache.end()) return it->second;
      ModInfo m;
      HMODULE h = nullptr;
      if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(a), &h) && h) {
        char p[MAX_PATH] = { };
        ::GetModuleFileNameA(h, p, MAX_PATH);
        const char* b = std::strrchr(p, '\\');
        m.name = b ? b + 1 : p;
        m.base = reinterpret_cast<uintptr_t>(h);
      } else m.name = "?";
      cache[a >> 16] = m;
      return m;
    }
    bool IsGameCode(uintptr_t a) { return a >= 0x401000 && a < 0x7E7000; }
    // a game return address: the bytes before it are a call
    bool LooksLikeReturn(uintptr_t v) {
      if (!IsGameCode(v)) return false;
      const uint8_t* p = reinterpret_cast<const uint8_t*>(v);
      if (p[-5] == 0xE8) return true;                                        // call rel32
      if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;              // call [abs] / [reg+disp32]
      if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;              // call [reg+disp8]
      if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;              // call reg / [reg]
      return false;
    }
    std::string Where(uint32_t eip, uint32_t caller) {
      char buf[160];
      const ModInfo m = ModuleOf(eip);
      const bool ours = m.name == "d3d9.dll" || m.name == "d3d8.dll" || m.name == "dinput8.dll" || m.name == "SDL3.dll";
      if (IsGameCode(eip)) std::snprintf(buf, sizeof buf, "game code 0x%06X", eip & ~0xFu);
      else if (ours) std::snprintf(buf, sizeof buf, "Shadow %s+0x%X", m.name.c_str(), unsigned(eip - m.base) & ~0xFu);
      else std::snprintf(buf, sizeof buf, "%s", m.name.c_str());
      std::string s = buf;
      if (caller && !IsGameCode(eip)) { std::snprintf(buf, sizeof buf, " <- game 0x%06X", caller); s += buf; }
      return s;
    }
    uint64_t LocKey(uint32_t eip, uint32_t caller) {
      // game code: by address (16-byte steps); elsewhere: by module and the game caller
      if (IsGameCode(eip)) return uint64_t(eip & ~0xFu);
      const ModInfo m = ModuleOf(eip);
      const bool ours = m.name == "d3d9.dll" || m.name == "d3d8.dll" || m.name == "dinput8.dll" || m.name == "SDL3.dll";
      const uint64_t where = ours ? (eip & ~0xFu) : m.base;
      return (where << 32) | caller;
    }

    // ---------------------------------------------------------------- sampler thread
    void SamplerLoop() {
      State& s = S();
      typedef UINT (WINAPI* PFN_tbp)(UINT);
      if (HMODULE mm = ::LoadLibraryA("winmm.dll"))
        if (auto f = reinterpret_cast<PFN_tbp>(::GetProcAddress(mm, "timeBeginPeriod"))) f(1);
      uint32_t stackCopy[256];
      while (!s.stop.load()) {
        ::Sleep(1);
        const int64_t fs = s.frameStart.load();
        if (!fs || !s.mainThread) continue;
        // only while a frame runs long (or always during a recording)
        if (!s.sessionOn.load() && QpcMs(Qpc() - fs) < 20.0) continue;
        CONTEXT ctx = { }; ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        SIZE_T got = 0;
        if (::SuspendThread(s.mainThread) == DWORD(-1)) continue;
        const bool okCtx = ::GetThreadContext(s.mainThread, &ctx);
        if (okCtx) ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<void*>(uintptr_t(
#ifdef _WIN64
          ctx.Rsp
#else
          ctx.Esp
#endif
          )), stackCopy, sizeof stackCopy, &got);
        ::ResumeThread(s.mainThread);
        if (!okCtx) continue;
        Sample sm = { s.frameAtomic.load(),
#ifdef _WIN64
          uint32_t(ctx.Rip),
#else
          uint32_t(ctx.Eip),
#endif
          0 };
        for (size_t i = 0; i < got / 4; i++) if (LooksLikeReturn(stackCopy[i])) { sm.caller = stackCopy[i]; break; }
        std::lock_guard<std::mutex> lk(s.sampleLock);
        if (s.samples.size() < 200000) s.samples.push_back(sm);
      }
    }

    // ---------------------------------------------------------------- setup
    void Init() {
      State& s = S();
      s.tried = true;
      char v[8] = { };
      if (::GetEnvironmentVariableA("SODRT_PROFILER", v, 8) && v[0] == '0') return;
      char la[MAX_PATH] = { };
      if (!::GetEnvironmentVariableA("LOCALAPPDATA", la, MAX_PATH)) return;
      s.dir = std::string(la) + "\\SoDRT";
      ::CreateDirectoryA(s.dir.c_str(), nullptr);
      ::MoveFileExA((s.dir + "\\hitches.log").c_str(), (s.dir + "\\hitches_previous.log").c_str(), MOVEFILE_REPLACE_EXISTING);
      s.hitchLog = std::fopen((s.dir + "\\hitches.log").c_str(), "w");
      if (s.hitchLog) {
        SYSTEMTIME t; ::GetLocalTime(&t);
        std::fprintf(s.hitchLog, "Shadow hitch log, %04d-%02d-%02d %02d:%02d:%02d\n"
          "A hitch is a frame much slower than the frames around it (1.8x their median and 12 ms more).\n"
          "'ours' = time in features Shadow adds; 'game + driver' = what is left of the frame;\n"
          "'where the game thread was' = samples taken every millisecond while the frame ran long.\n\n",
          t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        std::fflush(s.hitchLog);
      }
      if (HMODULE fix = ::GetModuleHandleA("dinput8.dll")) {
        s.fixCounters = reinterpret_cast<int (__cdecl*)(int64_t*, int)>(::GetProcAddress(fix, "SoDFix_GetProfileCounters"));
        s.fixFiles = reinterpret_cast<int (__cdecl*)(int64_t, char*, int)>(::GetProcAddress(fix, "SoDFix_GetOpenedFiles"));
      }
      s.recent.assign(120, 0.0);
      s.on = true;
      Logger::info(str::format("SoD-RT: profiler on: hitches to ", s.dir, "\\hitches.log, F9 records a profile"));
    }

    void StartSampler() {
      State& s = S();
      s.mainTid = ::GetCurrentThreadId();
      s.mainThread = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, s.mainTid);
      if (s.mainThread) s.sampler = std::thread(SamplerLoop);
    }

    // ---------------------------------------------------------------- reports
    std::string Breakdown(const Frame& f, bool withFiles, int64_t filesSince) {
      State& s = S();
      std::string o;
      char b[256];
      double ours = 0.0;
      for (uint32_t z = 0; z < ZCount; z++) if (z != ZPresent && z != ZCreate) ours += QpcMs(f.zone[z]);
      const double present = QpcMs(f.zone[ZPresent]), create = QpcMs(f.zone[ZCreate]);
      const double files = QpcMs(f.fOpenT + f.fReadT), pad = QpcMs(f.padT);
      const double cs = f.csSyncUs / 1000.0, gpu = f.gpuSyncUs / 1000.0;
      const double rest = std::max(0.0, f.ms - ours - present - create - files - cs - gpu);
      std::snprintf(b, sizeof b, "  ours %.1f ms:", ours); o += b;
      bool any = false;
      for (uint32_t z = 0; z < ZCount; z++) {
        if (z == ZPresent || z == ZCreate) continue;
        const double ms = QpcMs(f.zone[z]);
        if (ms < 0.05 && !f.count[z]) continue;
        std::snprintf(b, sizeof b, "%s %s %.2f (%u)", any ? "," : "", kZoneName[z], ms, f.count[z]); o += b; any = true;
      }
      if (!any) o += " nothing";
      std::snprintf(b, sizeof b, "\n  renderer: present / waiting for a frame slot %.1f ms, game thread waiting for DXVK %.1f ms (%llu), "
        "for the GPU %.1f ms (%llu), new pipelines %llu, game creating resources %.1f ms (%u), draws %llu\n",
        present, cs, (unsigned long long)f.csSyncs, gpu, (unsigned long long)f.gpuSyncs, (unsigned long long)f.pipes, create, f.count[ZCreate],
        (unsigned long long)f.draws); o += b;
      std::snprintf(b, sizeof b, "  game: file opens %lld (%.1f ms), reads %lld (%.1f ms, %.0f KB), controller %.2f ms, game + driver %.1f ms\n",
        (long long)f.fOpens, QpcMs(f.fOpenT), (long long)f.fReads, QpcMs(f.fReadT), f.fBytes / 1024.0, pad, rest); o += b;
      if (withFiles && f.fOpens && s.fixFiles) {
        char names[1024] = { };
        if (s.fixFiles(filesSince, names, sizeof names) > 0) { o += "  files opened: "; o += names; o += "\n"; }
      }
      // the biggest share
      struct { const char* what; double ms; } parts[] = {
        { "features Shadow adds", ours }, { "present / waiting for the GPU", present + gpu }, { "waiting for DXVK's thread", cs },
        { "the game loading files", files }, { "the game creating resources", create }, { "the game itself or the driver", rest } };
      auto* top = &parts[0];
      for (auto& p : parts) if (p.ms > top->ms) top = &p;
      std::snprintf(b, sizeof b, "  mostly: %s (%.0f%%)\n", top->what, f.ms > 0 ? 100.0 * top->ms / f.ms : 0.0); o += b;
      return o;
    }

    std::string SampleSummary(const std::vector<Sample>& v, uint64_t frame, size_t maxLines) {
      std::unordered_map<uint64_t, std::pair<uint32_t, Sample>> agg;
      uint32_t n = 0;
      for (const Sample& sm : v) {
        if (frame != ~0ull && sm.frame != frame) continue;
        auto& e = agg[LocKey(sm.eip, sm.caller)]; e.first++; e.second = sm; n++;
      }
      if (!n) return "";
      std::vector<std::pair<uint32_t, Sample>> list;
      for (auto& kv : agg) list.push_back(kv.second);
      std::sort(list.begin(), list.end(), [] (auto& a, auto& b) { return a.first > b.first; });
      std::string o = str::format("  where the game thread was (", n, " samples):\n");
      char b[256];
      for (size_t i = 0; i < list.size() && i < maxLines; i++) {
        std::snprintf(b, sizeof b, "    %3.0f%%  %s\n", 100.0 * list[i].first / n, Where(list[i].second.eip, list[i].second.caller).c_str());
        o += b;
      }
      return o;
    }

    void SessionStart() {
      State& s = S();
      SYSTEMTIME t; ::GetLocalTime(&t);
      char nm[64]; std::snprintf(nm, sizeof nm, "profile_%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
      s.sessionName = nm;
      s.csv = std::fopen((s.dir + "\\" + s.sessionName + ".csv").c_str(), "w");
      if (s.csv) {
        std::fprintf(s.csv, "frame,ms");
        for (uint32_t z = 0; z < ZCount; z++) std::fprintf(s.csv, ",%s ms", kZoneName[z]);
        std::fprintf(s.csv, ",dxvk wait ms,gpu wait ms,new pipelines,draws,file opens,file ms,file KB,controller ms,game+driver ms\n");
      }
      s.sessionFrames.clear(); s.sessionLoc.clear(); s.sessionSamples = 0; s.sessionHitches.clear();
      s.sessionStart = Qpc();
      { std::lock_guard<std::mutex> lk(s.sampleLock); s.samples.clear(); }
      s.sessionOn = true;
      Logger::info(str::format("SoD-RT: profiler: recording ", s.dir, "\\", s.sessionName, ".csv"));
    }

    void SessionStop() {
      State& s = S();
      s.sessionOn = false;
      if (s.csv) { std::fclose(s.csv); s.csv = nullptr; }
      FILE* f = std::fopen((s.dir + "\\" + s.sessionName + ".txt").c_str(), "w");
      if (f && !s.sessionFrames.empty()) {
        std::vector<double> ms; double sum = 0;
        for (auto& fr : s.sessionFrames) { ms.push_back(fr.ms); sum += fr.ms; }
        std::vector<double> srt = ms; std::sort(srt.begin(), srt.end());
        auto pct = [&] (double p) { return srt[std::min(srt.size() - 1, size_t(p * double(srt.size() - 1) + 0.5))]; };
        std::fprintf(f, "Shadow profile %s: %zu frames, %.1f s\n\n", s.sessionName.c_str(), ms.size(), sum / 1000.0);
        std::fprintf(f, "Frame time: average %.2f ms (%.1f fps), median %.2f, 95%% %.2f, 99%% %.2f, worst %.2f ms; hitches %zu\n\n",
          sum / ms.size(), 1000.0 * ms.size() / sum, pct(0.5), pct(0.95), pct(0.99), srt.back(), s.sessionHitches.size());
        std::fprintf(f, "Per frame (average / worst, ms; how often per frame):\n");
        for (uint32_t z = 0; z < ZCount; z++) {
          double tot = 0, worst = 0; uint64_t cnt = 0;
          for (auto& fr : s.sessionFrames) { const double v = QpcMs(fr.zone[z]); tot += v; worst = std::max(worst, v); cnt += fr.count[z]; }
          std::fprintf(f, "  %-24s %8.3f / %8.2f   %.2f\n", kZoneName[z], tot / ms.size(), worst, double(cnt) / ms.size());
        }
        double cs = 0, csW = 0, gpu = 0, gpuW = 0, files = 0, filesW = 0, rest = 0, restW = 0; uint64_t pipes = 0;
        for (auto& fr : s.sessionFrames) {
          cs += fr.csSyncUs / 1000.0; csW = std::max(csW, fr.csSyncUs / 1000.0);
          gpu += fr.gpuSyncUs / 1000.0; gpuW = std::max(gpuW, fr.gpuSyncUs / 1000.0);
          const double fl = QpcMs(fr.fOpenT + fr.fReadT); files += fl; filesW = std::max(filesW, fl);
          double ours = 0; for (uint32_t z = 0; z < ZCount; z++) ours += QpcMs(fr.zone[z]);
          const double r = std::max(0.0, fr.ms - ours - fr.csSyncUs / 1000.0 - fr.gpuSyncUs / 1000.0 - fl);
          rest += r; restW = std::max(restW, r); pipes += fr.pipes;
        }
        const double n = double(ms.size());
        std::fprintf(f, "  %-24s %8.3f / %8.2f\n", "waiting for DXVK", cs / n, csW);
        std::fprintf(f, "  %-24s %8.3f / %8.2f\n", "waiting for the GPU", gpu / n, gpuW);
        std::fprintf(f, "  %-24s %8.3f / %8.2f\n", "game file loading", files / n, filesW);
        std::fprintf(f, "  %-24s %8.3f / %8.2f\n", "game + driver", rest / n, restW);
        std::fprintf(f, "  new pipelines: %llu\n\n", (unsigned long long)pipes);
        // overall hotspots
        std::vector<std::pair<uint64_t, uint32_t>> loc(s.sessionLoc.begin(), s.sessionLoc.end());
        std::sort(loc.begin(), loc.end(), [] (auto& a, auto& b) { return a.second > b.second; });
        std::fprintf(f, "Where the game thread spent its time (%llu samples, 1 per ms):\n", (unsigned long long)s.sessionSamples);
        for (size_t i = 0; i < loc.size() && i < 40; i++) {
          const uint64_t k = loc[i].first;
          const uint32_t hi = uint32_t(k >> 32), lo = uint32_t(k);
          const std::string w = hi ? Where(hi, lo) : Where(lo, 0);
          std::fprintf(f, "  %5.1f%%  %s\n", 100.0 * loc[i].second / std::max<uint64_t>(s.sessionSamples, 1), w.c_str());
        }
        // worst hitches
        auto hs = s.sessionHitches;
        std::sort(hs.begin(), hs.end(), [] (auto& a, auto& b) { return a.ms > b.ms; });
        std::fprintf(f, "\nWorst hitches:\n");
        for (size_t i = 0; i < hs.size() && i < 15; i++) std::fprintf(f, "%s\n", hs[i].text.c_str());
      }
      if (f) std::fclose(f);
      Logger::info(str::format("SoD-RT: profiler: recording stopped, summary in ", s.dir, "\\", s.sessionName, ".txt"));
      { std::lock_guard<std::mutex> lk(s.sampleLock); s.samples.clear(); }
    }

  }

  bool Enabled() {
    State& s = S();
    if (!s.tried) Init();
    return s.on && (s.mainTid == 0 || s.mainTid == ::GetCurrentThreadId());
  }

  bool TopLevel() { return S().depth == 0; }

  void Begin(Zone z) {
    State& s = S();
    const int64_t now = Qpc();
    if (s.depth > 0) s.cur.zone[s.stack[s.depth - 1]] += now - s.segStart;
    if (s.depth < 16) s.stack[s.depth] = z;
    s.depth++;
    s.cur.count[z]++;
    s.segStart = now;
  }

  void End(Zone z) {
    State& s = S();
    const int64_t now = Qpc();
    if (s.depth > 0) {
      if (s.depth <= 16) s.cur.zone[s.stack[s.depth - 1]] += now - s.segStart;
      s.depth--;
    }
    s.segStart = now;
  }

  void FrameEnd(D3D9DeviceEx* dev, uint64_t drawsHint) {
    State& s = S();
    if (!s.tried) Init();
    if (!s.on) return;
    if (!s.mainTid) StartSampler();
    if (s.mainTid != ::GetCurrentThreadId()) return;
    const int64_t now = Qpc();

    // F9 / profile.go: start or stop a recording
    const bool f9 = (::GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    bool toggle = f9 && !s.f9Down;
    s.f9Down = f9;
    if (QpcMs(now - s.lastGoCheck) > 500.0) {
      s.lastGoCheck = now;
      const std::string go = s.dir + "\\profile.go";
      if (::GetFileAttributesA(go.c_str()) != INVALID_FILE_ATTRIBUTES) { ::DeleteFileA(go.c_str()); toggle = true; }
    }

    Frame f = s.cur;
    if (s.lastPresent) {
      f.ms = QpcMs(now - s.lastPresent);
      f.no = s.frameNo;
      f.start = s.lastPresent;
      // DXVK counters
      DxvkStatCounters c = dev->GetDXVKDevice()->getStatCounters();
      if (s.havePrevCtr) {
        DxvkStatCounters d = c.diff(s.prevCtr);
        f.pipes = d.getCtr(DxvkStatCounter::PipeCountGraphics) + d.getCtr(DxvkStatCounter::PipeCountCompute);
        f.csSyncs = d.getCtr(DxvkStatCounter::CsSyncCount); f.csSyncUs = d.getCtr(DxvkStatCounter::CsSyncTicks);
        f.gpuSyncs = d.getCtr(DxvkStatCounter::GpuSyncCount); f.gpuSyncUs = d.getCtr(DxvkStatCounter::GpuSyncTicks);
        f.gpuIdleUs = d.getCtr(DxvkStatCounter::GpuIdleTicks); f.submits = d.getCtr(DxvkStatCounter::QueueSubmitCount);
        f.draws = drawsHint ? drawsHint : d.getCtr(DxvkStatCounter::CmdDrawCalls);
      }
      s.prevCtr = c; s.havePrevCtr = true;
      // SoDFix counters
      if (s.fixCounters) {
        int64_t v[6] = { };
        s.fixCounters(v, 6);
        f.fOpenT = v[0] - s.prevFix[0]; f.fOpens = v[1] - s.prevFix[1]; f.fReadT = v[2] - s.prevFix[2];
        f.fReads = v[3] - s.prevFix[3]; f.fBytes = v[4] - s.prevFix[4]; f.padT = v[5] - s.prevFix[5];
        std::memcpy(s.prevFix, v, sizeof v);
      }

      // hitch: much slower than the frames around it
      std::vector<double> r = s.recent;
      std::sort(r.begin(), r.end());
      const double median = r[r.size() / 2];
      const bool warm = s.frameNo > 60 && median > 0.0;
      const bool hitch = warm && f.ms > std::max(1.8 * median, median + 12.0);
      s.recent[s.recentPos++ % s.recent.size()] = f.ms;

      std::vector<Sample> frameSamples;
      {
        std::lock_guard<std::mutex> lk(s.sampleLock);
        if (s.sessionOn) {
          for (const Sample& sm : s.samples) { s.sessionLoc[LocKey(sm.eip, sm.caller)]++; s.sessionSamples++; }
          for (const Sample& sm : s.samples) if (sm.frame == f.no) frameSamples.push_back(sm);
        } else frameSamples = s.samples;
        s.samples.clear();
      }

      if (hitch) {
        s.hitches++;
        SYSTEMTIME t; ::GetLocalTime(&t);
        char head[200];
        std::snprintf(head, sizeof head, "hitch %u at %02d:%02d:%02d.%03d (frame %llu): %.1f ms, usual %.1f ms\n",
          s.hitches, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, (unsigned long long)f.no, f.ms, median);
        const std::string text = head + Breakdown(f, true, f.start) + SampleSummary(frameSamples, f.no, 8);
        if (s.hitchLog) { std::fprintf(s.hitchLog, "%s\n", text.c_str()); std::fflush(s.hitchLog); }
        if (s.sessionOn) s.sessionHitches.push_back({ f.ms, median, text });
        if (s.hitches <= 200 || (s.hitches % 100) == 0)
          Logger::info(str::format("SoD-RT: hitch ", s.hitches, ": ", uint32_t(f.ms), " ms (usual ", uint32_t(median), " ms), details in hitches.log"));
      }

      if (s.sessionOn) {
        s.sessionFrames.push_back(f);
        if (s.csv) {
          std::fprintf(s.csv, "%llu,%.3f", (unsigned long long)f.no, f.ms);
          double ours = 0;
          for (uint32_t z = 0; z < ZCount; z++) { const double v = QpcMs(f.zone[z]); ours += v; std::fprintf(s.csv, ",%.3f", v); }
          const double fl = QpcMs(f.fOpenT + f.fReadT);
          std::fprintf(s.csv, ",%.3f,%.3f,%llu,%llu,%lld,%.3f,%.1f,%.3f,%.3f\n", f.csSyncUs / 1000.0, f.gpuSyncUs / 1000.0,
            (unsigned long long)f.pipes, (unsigned long long)f.draws, (long long)f.fOpens, fl, f.fBytes / 1024.0, QpcMs(f.padT),
            std::max(0.0, f.ms - ours - f.csSyncUs / 1000.0 - f.gpuSyncUs / 1000.0 - fl));
        }
        if (s.sessionFrames.size() > 30u * 60u * 10u) toggle = true;      // ten minutes at most
      }
    }

    if (toggle) { if (s.sessionOn) SessionStop(); else SessionStart(); }

    // the next frame starts now (zones opened across the boundary keep counting into the next frame)
    s.cur = Frame();
    s.lastPresent = now;
    s.frameNo++;
    s.frameAtomic = s.frameNo;
    s.frameStart = now;
    if (s.depth > 0) s.segStart = now;
  }

}
