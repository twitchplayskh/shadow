// SoDFix — compatibility layer for Shadow of Destiny / Shadow of Memories (PC, 2002)
// Loaded as a dinput8.dll proxy. Forwards DirectInput8Create to the system DLL and
// applies small, verified, reversible in-memory patches to shadow.exe.
// The executable and the original data files on disk are never modified.
//
// Target build: shadow.exe, PE timestamp 0x3DB6B7C3 (2002-10-23),
// SHA-256 529d30e84d292b3617551e4f9df76f7cc497744b2549d5d8605e179b8e91e764.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>

#define SODFIX_VERSION "0.24.0"
#ifndef SODFIX_BUILD
#define SODFIX_BUILD "?"
#endif

// ---------------------------------------------------------------- logging
static FILE* g_log = nullptr;
static char  g_dir[MAX_PATH], g_ini[MAX_PATH];

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------- config
enum ScreenMode { SM_WINDOWED = 0, SM_BORDERLESS = 1, SM_FULLSCREEN = 2 };
static const char* kModeName[3] = { "windowed", "borderless", "fullscreen" };

struct Config {
    bool enabled     = true;
    bool log         = true;
    bool resMenu     = true;   // extra 4:3 resolutions in GRAPHIC SETTING > RESOLUTION
    bool modeMenu    = true;   // SCREEN MODE row in GRAPHIC SETTING
    int  mode        = SM_BORDERLESS;
    bool overrideRes = false;
    int  width = 0, height = 0;
    bool horPlus     = false;
    int  rtLevel     = 0;      // RT SHADOW CASTING row: 0 off, 1 medium, 2 high, 3 ultra
    int  aaMode      = 0;      // ANTI-ALIASING row: 0 off, 1 SMAA
    int  tfMode      = 0;      // TEXTURE FILTERING row: 0 standard, 1 trilinear, 2 anisotropic 16x
    int  bars        = 1;      // CINEMATIC BARS row: 1 on (the game's letterbox in cutscenes), 0 off
    int  aoMode      = 0;      // AMBIENT OCCLUSION row: 0 off, 1 low, 2 high
    int  ssrMode     = 0;      // REFLECTIONS row: 0 off, 1 low, 2 high
    int  upMode      = 3;      // TEXTURE UPSCALING row: 0 off, 1 2X, 2 3X, 3 4X
    int  geomMode    = 0;      // IMPROVED GEOMETRY row: 0 off, 1 on
    int  smoothCam   = 1;      // SMOOTH CAMERA row (DISPLAY SETTING): 1 on, 0 off
    int  kbLayout    = 0;      // CONTROLLER SETTING > LAYOUT (keyboard type only): 0 classic, 1 modern
    int  gamepad     = 1;      // modern controllers through SDL3 (hot-plug), [Controls] Gamepad
    int  padDeadzone = 35;     // left stick: percent of full travel before it counts as a direction
    int  hdr         = 0;      // HDR row: 0 off, 1 on (HDR10 output; Windows HDR must be on)
    int  hdrPaper    = 100;    // [Display] HdrPaperWhite: nits of the game's white; 0 = Windows' SDR content brightness
    int  hdrPeak     = 1000;   // [Display] HdrPeakBrightness: nits the brightest lights may reach
    int  hdrHighlight = 60;    // [Display] HdrHighlight: 0..200 %, how far light sources go above white (0 = none)
    int  hdrGamma    = 210;    // [Display] HdrGamma: decode gamma x 100 (180..240)
    int  hdrSat      = 100;    // [Display] HdrSaturation: colour strength % (50..150, 100 = original)
    int  hdrDebug    = 0;      // [Display] HdrDebug: 0 normal, 1 no Shadow HDR pass (DXVK's own conversion), 2 grey test
    int  newMenu     = 1;      // [Display] NewGraphicsMenu: 1 the redesigned GRAPHIC SETTING screen, 0 the original list
    int  lamps       = 1;      // LAMP LIGHTING (0.24.0, [Display] LampLighting): the game's lamps light the scene (ray traced)
    int  fpsUnlock   = 0;      // UNCAPPED FRAME RATE row (experimental): 0 the game's 30 / 60 fps limiter, 1 none
} g_cfg;

static int g_mode = SM_BORDERLESS;      // active screen mode
static volatile int g_menuMode = -1;     // value currently shown in the menu (-1 = menu not open)

static void LoadConfig() {
    snprintf(g_ini, sizeof g_ini, "%s\\SoDFix.ini", g_dir);
    auto I = [&](const char* s, const char* k, int d) { return (int)GetPrivateProfileIntA(s, k, d, g_ini); };
    g_cfg.enabled     = I("General", "Enabled", 1) != 0;
    g_cfg.log         = I("General", "Log", 1) != 0;
    g_cfg.resMenu     = I("Display", "ExtendedResolutionMenu", 1) != 0;
    g_cfg.modeMenu    = I("Display", "ScreenModeMenu", 1) != 0;
    int legacy        = I("Display", "KeepAspect4x3", 1) ? SM_BORDERLESS : SM_FULLSCREEN;   // 0.2 setting
    g_cfg.mode        = I("Display", "ScreenMode", legacy);
    if (g_cfg.mode != SM_FULLSCREEN) g_cfg.mode = SM_BORDERLESS;   // WINDOWED removed in 0.3.1
    g_cfg.overrideRes = I("Display", "OverrideResolution", 0) != 0;
    g_cfg.width       = I("Display", "Width", 0);
    g_cfg.height      = I("Display", "Height", 0);
    g_cfg.horPlus     = I("Display", "WidescreenFOV", 0) != 0;
    g_cfg.rtLevel     = I("Shadows", "RayTracing", 0);
    if (g_cfg.rtLevel < 0 || g_cfg.rtLevel > 3) g_cfg.rtLevel = 0;
    g_cfg.aaMode      = I("Display", "AntiAliasing", 0);
    if (g_cfg.aaMode < 0 || g_cfg.aaMode > 1) g_cfg.aaMode = 0;
    g_cfg.tfMode      = I("Display", "TextureFiltering", 0);
    if (g_cfg.tfMode < 0 || g_cfg.tfMode > 2) g_cfg.tfMode = 0;
    g_cfg.bars        = I("Display", "CinematicBars", 1) != 0;
    g_cfg.kbLayout    = I("Controls", "KeyboardLayout", 0) == 1 ? 1 : 0;
    g_cfg.aoMode      = I("Display", "AmbientOcclusion", 0);
    if (g_cfg.aoMode == 3) g_cfg.aoMode = 2;              // RAY TRACED (0.20.0 test, removed): HIGH
    if (g_cfg.aoMode < 0 || g_cfg.aoMode > 2) g_cfg.aoMode = 0;
    g_cfg.ssrMode     = I("Display", "Reflections", 0);
    if (g_cfg.ssrMode < 0 || g_cfg.ssrMode > 2) g_cfg.ssrMode = 0;
    g_cfg.upMode      = I("Display", "TextureUpscaling", 3);
    if (g_cfg.upMode < 0 || g_cfg.upMode > 3) g_cfg.upMode = 3;
    g_cfg.geomMode    = I("Display", "ImprovedGeometry", 0);
    if (g_cfg.geomMode < 0 || g_cfg.geomMode > 1) g_cfg.geomMode = 0;
    g_cfg.smoothCam   = I("Display", "SmoothCamera", 1) != 0;
    g_cfg.gamepad     = I("Controls", "Gamepad", 1) != 0;
    g_cfg.fpsUnlock   = 0;
    g_cfg.lamps       = I("Display", "LampLighting", 1) != 0;   // UNCAPPED FRAME RATE removed (0.24.0): always the game's own limiter
    g_cfg.newMenu     = I("Display", "NewGraphicsMenu", 1) != 0;
    g_cfg.hdr         = I("Display", "HDR", 0) != 0;
    g_cfg.hdrPaper    = I("Display", "HdrPaperWhite", 100);
    if (g_cfg.hdrPaper != 0 && (g_cfg.hdrPaper < 60 || g_cfg.hdrPaper > 600)) g_cfg.hdrPaper = 100;
    g_cfg.hdrPeak     = I("Display", "HdrPeakBrightness", 1000);
    if (g_cfg.hdrPeak < 200 || g_cfg.hdrPeak > 10000) g_cfg.hdrPeak = 1000;
    g_cfg.hdrHighlight = I("Display", "HdrHighlight", 60);
    if (g_cfg.hdrHighlight < 0 || g_cfg.hdrHighlight > 200) g_cfg.hdrHighlight = 60;
    g_cfg.hdrGamma    = I("Display", "HdrGamma", -1);
    if (g_cfg.hdrGamma < 0) {                              // 0.21 files: HdrCurve 0 sRGB, 1 2.2, 2 2.0, 3 1.9, 4 2.1
        static const int kCurve[5] = { 220, 220, 200, 190, 210 };
        int c = I("Display", "HdrCurve", 4);
        g_cfg.hdrGamma = kCurve[c < 0 || c > 4 ? 4 : c];
    }
    if (g_cfg.hdrGamma < 180 || g_cfg.hdrGamma > 240) g_cfg.hdrGamma = 210;
    g_cfg.hdrSat      = I("Display", "HdrSaturation", 100);
    if (g_cfg.hdrSat < 50 || g_cfg.hdrSat > 150) g_cfg.hdrSat = 100;
    g_cfg.hdrDebug    = I("Display", "HdrDebug", 0);
    g_cfg.padDeadzone = I("Controls", "GamepadDeadzone", 35);
    if (g_cfg.padDeadzone < 5 || g_cfg.padDeadzone > 90) g_cfg.padDeadzone = 35;
    g_mode = g_cfg.mode;
}
static void SaveRt() {
    char v[8]; snprintf(v, sizeof v, "%d", g_cfg.rtLevel);
    WritePrivateProfileStringA("Shadows", "RayTracing", v, g_ini);
}
static void SaveAa() {
    char v[8]; snprintf(v, sizeof v, "%d", g_cfg.aaMode);
    WritePrivateProfileStringA("Display", "AntiAliasing", v, g_ini);
}
static void SaveTf() {
    char v[8]; snprintf(v, sizeof v, "%d", g_cfg.tfMode);
    WritePrivateProfileStringA("Display", "TextureFiltering", v, g_ini);
}
static void SaveBars() {
    WritePrivateProfileStringA("Display", "CinematicBars", g_cfg.bars ? "1" : "0", g_ini);
}
static void SaveAo() {
    char v[8]; snprintf(v, sizeof v, "%d", g_cfg.aoMode);
    WritePrivateProfileStringA("Display", "AmbientOcclusion", v, g_ini);
}
static void SaveSsr() {
    char v[8]; snprintf(v, sizeof v, "%d", g_cfg.ssrMode);
    WritePrivateProfileStringA("Display", "Reflections", v, g_ini);
}
static void SaveUp() {
    char v[8]; snprintf(v, sizeof v, "%d", g_cfg.upMode);
    WritePrivateProfileStringA("Display", "TextureUpscaling", v, g_ini);
}
static void SaveGeom() {
    char v[8]; snprintf(v, sizeof v, "%d", g_cfg.geomMode);
    WritePrivateProfileStringA("Display", "ImprovedGeometry", v, g_ini);
}
static void SaveHdr() {
    WritePrivateProfileStringA("Display", "HDR", g_cfg.hdr ? "1" : "0", g_ini);
}
static void SaveFps() {
    WritePrivateProfileStringA("Display", "UncappedFrameRate", g_cfg.fpsUnlock ? "1" : "0", g_ini);
}
// UNCAPPED FRAME RATE: the game's limiter (0x7B2F60, 30 fps, 60 on some screens) spins on QueryPerformanceCounter
// until a frame's time has passed; its 'jne' over that wait (0x7B2FD5) becomes a 'jmp'. The game's logic steps once
// per frame, so everything runs faster with the frame rate: experimental, for seeing what happens.
static void ApplyFps() {
    const uint8_t op = g_cfg.fpsUnlock ? 0xEB : 0x75;
    DWORD old;
    if (VirtualProtect((void*)0x7B2FD5, 1, PAGE_EXECUTE_READWRITE, &old)) {
        if (*(uint8_t*)0x7B2FD5 == 0x75 || *(uint8_t*)0x7B2FD5 == 0xEB) *(uint8_t*)0x7B2FD5 = op;
        VirtualProtect((void*)0x7B2FD5, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)0x7B2FD5, 1);
    }
}
static void SaveSmoothCam() {
    WritePrivateProfileStringA("Display", "SmoothCamera", g_cfg.smoothCam ? "1" : "0", g_ini);
}
static void SaveKb() {
    WritePrivateProfileStringA("Controls", "KeyboardLayout", g_cfg.kbLayout ? "1" : "0", g_ini);
}
static void SaveMode() {
    char v[8]; snprintf(v, sizeof v, "%d", g_mode);
    WritePrivateProfileStringA("Display", "ScreenMode", v, g_ini);
}

// ---------------------------------------------------------------- patch engine
struct Patch { const char* name; uintptr_t addr; std::vector<uint8_t> expect, repl; bool applied = false; };
static std::vector<Patch> g_patches;
static bool g_patchesActive = false;

static bool WriteMem(uintptr_t addr, const void* src, size_t n) {
    DWORD old;
    if (!VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)addr, src, n);
    VirtualProtect((void*)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
    return true;
}
static std::vector<uint8_t> U32(uint32_t v) { return { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; }
static std::vector<uint8_t> U16(uint16_t v) { return { (uint8_t)v, (uint8_t)(v >> 8) }; }
static uint32_t F2U(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static std::vector<uint8_t> Rel32(uintptr_t at, uintptr_t target) { return U32((uint32_t)(target - (at + 4))); }
static void Add(const char* name, uintptr_t addr, std::vector<uint8_t> expect, std::vector<uint8_t> repl) {
    g_patches.push_back({ name, addr, std::move(expect), std::move(repl) });
}
static void ApplyAll() {
    for (auto& p : g_patches) {
        if (memcmp((void*)p.addr, p.expect.data(), p.expect.size()) != 0) {
            Log("SKIP  %-34s @%08X: original bytes differ", p.name, (unsigned)p.addr); continue;
        }
        p.applied = WriteMem(p.addr, p.repl.data(), p.repl.size());
        Log("%s %-34s @%08X (%u bytes)", p.applied ? "OK   " : "FAIL ", p.name, (unsigned)p.addr, (unsigned)p.repl.size());
    }
}
static void RestoreAll() {
    for (auto it = g_patches.rbegin(); it != g_patches.rend(); ++it)
        if (it->applied) { WriteMem(it->addr, it->expect.data(), it->expect.size()); it->applied = false; }
}

// runtime-built x86 stubs ------------------------------------------------------------
static uint8_t* g_stubs = nullptr; static size_t g_stubUsed = 0;
struct Asm {
    std::vector<uint8_t> b; uintptr_t at;
    explicit Asm(uintptr_t where) : at(where) {}
    Asm& raw(std::initializer_list<uint8_t> x) { b.insert(b.end(), x); return *this; }
    Asm& u32(uint32_t v) { auto x = U32(v); b.insert(b.end(), x.begin(), x.end()); return *this; }
    Asm& callAbs(void* const* slot) { raw({ 0xFF, 0x15 }); return u32((uint32_t)(uintptr_t)slot); }   // call [slot]
    Asm& call(uintptr_t t) { raw({ 0xE8 }); return u32((uint32_t)(t - (at + b.size() + 4))); }
    Asm& jmp(uintptr_t t)  { raw({ 0xE9 }); return u32((uint32_t)(t - (at + b.size() + 4))); }
};
static uintptr_t NextStub() {
    if (!g_stubs) g_stubs = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    return (uintptr_t)g_stubs + g_stubUsed;
}
static uintptr_t Emit(const Asm& a) {
    memcpy((void*)a.at, a.b.data(), a.b.size()); g_stubUsed += (a.b.size() + 15) & ~15u; return a.at;
}

// ---------------------------------------------------------------- game addresses (verified, see TECHNICAL_NOTES.md)
namespace A {
    const uintptr_t ResTable        = 0x939330;
    const uintptr_t ResRangeMax     = 0x93940A;
    const uintptr_t SpriteTable     = 0x939450;
    const uintptr_t SpriteMultiple  = 0x9398A0;
    const uintptr_t Layout          = 0x9398B0;   // {s16 sprite, rect, x, y} x 59
    const uintptr_t Page3Rows       = 0x939B0C;   // ptr -> short[] label sprite per row
    const uintptr_t Page3Help       = 0x939AC8;   // ptr -> short[] help rect per row
    const uintptr_t Page2Rows       = 0x939B08;   // DISPLAY SETTING
    const uintptr_t Page2Help       = 0x939AC4;
    const uintptr_t Windowed        = 0xF68E0C;
    const uintptr_t BackBufW        = 0xF68EF8;
    const uintptr_t BackBufH        = 0xF68EFC;
    const uintptr_t MainHwnd        = 0xF68D54;
    const uintptr_t ClassNamePtr    = 0xF68DA0;
    const uintptr_t CreateDevWindow = 0x7B25C0;
    const uintptr_t ValidateMode    = 0x7A5A40;   // int (w, h) against DirectDraw mode list
    const uintptr_t MenuInit        = 0x716650;   // copies settings into the menu object
    const uintptr_t SpriteDraw      = 0x776C50;   // update + draw N pool sprites
    const uintptr_t ApplyRes        = 0x715F80;   // menu exit: apply resolution
    const uintptr_t IAT_CreateFileA     = 0x1206424;
    const uintptr_t IAT_CreateFileW     = 0x1206438;
    const uintptr_t IAT_SetWindowPos    = 0x120650C;
    const uintptr_t IAT_CreateWindowExA = 0x1206554;
}

// ---------------------------------------------------------------- menu data
struct ResEntry { uint32_t w, h; };
struct SpriteRect { uint16_t tex, pal; int16_t u, v, w, h; };
struct RangeItem { int16_t min, max, id; };

static ResEntry   g_res[]    = { {640,480}, {800,600}, {1024,768}, {1280,960}, {1440,1080}, {1920,1440}, {2880,2160} };
static const int  kResCount  = sizeof g_res / sizeof g_res[0];
static SpriteRect g_resLabels[kResCount];
static const SpriteRect kNewResLabels[3] = {
    { 887, 13991, 392, 192, 111, 32 }, { 887, 13991, 266, 226, 108, 32 }, { 887, 13991, 382, 226, 104, 32 } };   // 1440 moved off rect 0x22 (0.8.1)

// SCREEN MODE row (art in image 7: texture 819 / palette 13226), labels cropped to ink rows (+1 px)
static const SpriteRect kModeTitle   = { 819, 13226, 234, 326, 174, 24 };
static const SpriteRect kModeValue[2] = { { 819, 13226, 274, 234, 153, 24 },    // 0 BORDERLESS (borderless full-screen window)
                                          { 819, 13226, 274, 262, 150, 24 } };  // 1 FULLSCREEN
static int MenuToMode(int v) { return v == 1 ? SM_FULLSCREEN : SM_BORDERLESS; }
static int ModeToMenu(int m) { return m == SM_FULLSCREEN ? 1 : 0; }
static const SpriteRect kModeHelp    = { 819, 13226, 330,  93, 155, 19 };       // "Change screen mode"
static const int kModeLabelYOff = 1;

// RT SHADOW CASTING row art (image 7, renamed from RAY TRACING in 0.10) and value labels (OFF / MEDIUM / HIGH
// from the game's own atlas)
static const SpriteRect kRtTitle   = { 819, 13226, 234, 352, 252, 24 };   // yoff 1
static const SpriteRect kRtHelp    = { 819, 13226, 272, 286, 142, 18 };   // "Ray traced shadows" (18 rows: clear of rect 0x4F)
static const SpriteRect kRtValue[4] = { { 887, 13991,  48,   0,  48, 32 },  // OFF     (table idx 0x0B)
                                        { 887, 13991, 298, 366, 110, 32 },  // MEDIUM  (0x5A)
                                        { 887, 13991, 412, 366,  90, 32 },  // HIGH    (0x5B)
                                        { 819, 13226, 410, 326,  85, 23 } };// ULTRA   (new, yoff 1)
static const int kRtValueYOff[4] = { 0, 0, 0, 1 };
// ANTI-ALIASING, TEXTURE FILTERING, CINEMATIC BARS and LAYOUT art: image 7 rows 400..511 (added in 0.8.1; the
// record already declared a 512x512 texture, so existing UVs are unchanged and no game rect reaches these rows).
// Rects come from scripts/make_menu_art_v2.py (menu_art.h); yoff = rows of blank above the ink vs the game's labels.
#include "menu_art.h"
static const SpriteRect& kAaTitle = kArt_ANTI_ALIASING;
static const SpriteRect& kAaHelp  = kArt_help_aa;           // "Smooth jagged edges"
static const SpriteRect kAaValue[2] = { { 887, 13991,  48,   0,  48, 32 },  // OFF
                                        { 819, 13226, 300, 376,  70, 23 } };// SMAA (yoff 2)
static const int kAaValueYOff[2] = { 0, 2 };
static const SpriteRect& kTfTitle = kArt_TEXTURE_FILTERING;
static const SpriteRect& kTfHelp  = kArt_help_tf;           // "Reduce texture shimmer"
static const SpriteRect kTfValue[3] = { kArt_STANDARD, kArt_TRILINEAR, kArt_ANISOTROPIC };
static const int kTfValueYOff[3] = { 1, 1, 1 };
static const SpriteRect& kAoTitle = kArt_AMBIENT_OCCLUSION;
static const SpriteRect& kAoHelp  = kArt_help_ao;           // "Shade corners and contact points"
static const SpriteRect kAoValue[4] = { { 887, 13991,  48,   0,  48, 32 },  // OFF  (game rect 0x0B)
                                        { 887, 13991, 216, 366,  82, 32 },  // LOW  (0x59)
                                        { 887, 13991, 412, 366,  90, 32 },  // HIGH (0x5B)
                                        kArt_RAY_TRACED };                  // RAY TRACED (image 10, 0.20)
static const SpriteRect& kSsrTitle = kArt_REFLECTIONS;     // image 10 (texture 4999, new in 0.11)
static const SpriteRect& kSsrHelp  = kArt_help_ssr;        // "Reflections on polished floors"
static const SpriteRect& kUpTitle  = kArt_TEXTURE_UPSCALING; // image 10 (0.17)
static const SpriteRect& kUpHelp   = kArt_help_up;         // "Sharper, more detailed textures"
static const SpriteRect& kGeomTitle = kArt_IMPROVED_GEOMETRY; // image 10 (0.18)
static const SpriteRect& kGeomHelp  = kArt_help_geom;       // "Seamless streets, no repeating tiles"
static const SpriteRect kGeomValue[2] = { { 887, 13991, 48, 0, 48, 32 },   // OFF (game rect 0x0B)
                                          { 887, 13991,  0, 0, 40, 32 } }; // ON  (game rect 0x0A)
static const SpriteRect kUpValue[4] = { { 887, 13991, 48, 0, 48, 32 }, kArt_V2X, kArt_V3X, kArt_V4X };   // OFF (game rect 0x0B)
static const SpriteRect& kFpsTitle = kArt_UNCAPPED_FRAME_RATE; // image 10 (0.19.1)
static const SpriteRect& kFpsHelp  = kArt_help_fps;          // "Experimental: the game runs faster"
static const SpriteRect& kHdrTitle = kArt_HDR;               // image 10 (0.20.0)
static const SpriteRect& kHdrHelp  = kArt_help_hdr;          // "Brighter lights on HDR displays"
static const SpriteRect& kScTitle = kArt_SMOOTH_CAMERA;    // image 10 (0.18.6)
static const SpriteRect& kScHelp  = kArt_help_sc;          // "Steadier camera while walking"
static const SpriteRect& kCbTitle = kArt_CINEMATIC_BARS;
static const SpriteRect& kCbHelp  = kArt_help_cb;           // "Black bars in cutscenes"
static const SpriteRect kCbValue[2] = { { 887, 13991,   0,   0,  40, 32 },  // ON  (game rect 0x0A)
                                        { 887, 13991,  48,   0,  48, 32 } };// OFF (game rect 0x0B)
static const int kCbValueYOff[2] = { 0, 0 };
// scroll bar: untextured quads (texture 0), like the game's underline sprites
static SpriteRect g_sbTrack = { 0, 0, 0, 0, 1, 0 };
static SpriteRect g_sbThumb = { 0, 0, 0, 0, 3, 0 };
static SpriteRect g_sbTrack2 = { 0, 0, 0, 0, 1, 0 };
static SpriteRect g_sbThumb2 = { 0, 0, 0, 0, 3, 0 };

static const int kModeId = 16;                 // value slot obj+0x94
static const int kRtId   = 17;                 // value slot obj+0x98
static const int kAaId   = 18;                 // value slot obj+0x9C
static const int kTfId   = 19;                 // value slot obj+0xA0
static const int kCbId   = 20;                 // value slot obj+0xA4 (0 = ON, 1 = OFF)
static const int kKbId   = 21;                 // value slot obj+0xA8 (0 = CLASSIC, 1 = MODERN)
static const int kAoId   = 22;                 // value slot obj+0xAC (0 = OFF, 1 = LOW, 2 = HIGH)
static const int kSsrId  = 23;                 // value slot obj+0xB0 (0 = OFF, 1 = LOW, 2 = HIGH)
static const int kUpId   = 24;                 // value slot obj+0xB4 (0 = OFF, 1 = 2X, 2 = 3X, 3 = 4X)
static const int kGeomId = 25;                 // value slot obj+0xB8 (0 = OFF, 1 = ON)
static const int kScId   = 26;                 // value slot obj+0xBC (0 = ON, 1 = OFF, like CINEMATIC BARS)
static const int kFpsId  = 27;                 // value slot obj+0xC0 (0 = OFF, 1 = ON)
static const int kHdrId  = 28;                 // value slot obj+0xC4 (0 = OFF, 1 = ON)
static const int kRemapId = 29;                 // value slot obj+0xC8 (REMAP: no value, Enter opens the screen)
static const int kMenuObjSize = 0xCC;
// GRAPHIC SETTING (page 3): 14 items + EXIT
static RangeItem g_page3Items[13];
static const int kHdrRow = 12;                 // GRAPHIC SETTING row of HDR (UNCAPPED FRAME RATE removed in 0.24.0)
static int16_t   g_page3Rows[14] = { 97, 120, 98, 99, 100, 124, 130, 134, 154, 158, 162, 166, 181, 117 };
static int16_t   g_page3Help[14] = { 73, 73, 74, 75, 76, 76, 76, 76, 76, 76, 76, 76, 76, 62 };
// DISPLAY SETTING (page 2): the game's 4 items (ids 6..9: table 0x939434) + CINEMATIC BARS + SMOOTH CAMERA + EXIT
static RangeItem g_page2Items[6];
static int16_t   g_page2Rows[7] = { 73, 75, 79, 83, 138, 170, 89 };
static int16_t   g_page2Help[7] = { 69, 70, 71, 72, 72, 72, 62 };
enum { SPR_TITLE = 120, SPR_TITLE_SH = 121, SPR_VALUE = 122, SPR_VALUE_SH = 123,
       SPR_RT_TITLE = 124, SPR_RT_TITLE_SH = 125, SPR_RT_VALUE = 126, SPR_RT_VALUE_SH = 127,
       SPR_SB_TRACK = 128, SPR_SB_THUMB = 129,
       SPR_AA_TITLE = 130, SPR_AA_TITLE_SH = 131, SPR_AA_VALUE = 132, SPR_AA_VALUE_SH = 133,
       SPR_TF_TITLE = 134, SPR_TF_TITLE_SH = 135, SPR_TF_VALUE = 136, SPR_TF_VALUE_SH = 137,
       SPR_CB_TITLE = 138, SPR_CB_TITLE_SH = 139, SPR_CB_VALUE = 140, SPR_CB_VALUE_SH = 141,
       SPR_SB2_TRACK = 142, SPR_SB2_THUMB = 143,
       SPR_KB_TITLE = 144, SPR_KB_TITLE_SH = 145, SPR_KB_CLASSIC = 146, SPR_KB_CLASSIC_SH = 147,
       SPR_KB_MODERN = 148, SPR_KB_MODERN_SH = 149, SPR_KB_LINE = 150,
       SPR_CB_OFF = 151, SPR_CB_OFF_SH = 152, SPR_CB_LINE = 153,
       SPR_AO_TITLE = 154, SPR_AO_TITLE_SH = 155, SPR_AO_VALUE = 156, SPR_AO_VALUE_SH = 157,
       SPR_SSR_TITLE = 158, SPR_SSR_TITLE_SH = 159, SPR_SSR_VALUE = 160, SPR_SSR_VALUE_SH = 161,
       SPR_UP_TITLE = 162, SPR_UP_TITLE_SH = 163, SPR_UP_VALUE = 164, SPR_UP_VALUE_SH = 165,
       SPR_GEOM_TITLE = 166, SPR_GEOM_TITLE_SH = 167, SPR_GEOM_VALUE = 168, SPR_GEOM_VALUE_SH = 169,
       SPR_SC_TITLE = 170, SPR_SC_TITLE_SH = 171, SPR_SC_ON = 172, SPR_SC_ON_SH = 173, SPR_SC_OFF = 174, SPR_SC_OFF_SH = 175,
       SPR_SC_LINE = 176,
       SPR_FPS_TITLE = 177, SPR_FPS_TITLE_SH = 178, SPR_FPS_VALUE = 179, SPR_FPS_VALUE_SH = 180,
       SPR_HDR_TITLE = 181, SPR_HDR_TITLE_SH = 182, SPR_HDR_VALUE = 183, SPR_HDR_VALUE_SH = 184,
       SPR_COVER = 185, COVER_N = 24, POOL_NEW = SPR_COVER + COVER_N };   // background copies drawn over the old menu
// CONTROLLER SETTING (page 0): TYPE + LAYOUT (keyboard only) + EXIT. The game reads the item count (code immediate)
// and the row/help tables (pointers) every frame, so both are switched with TYPE.
static RangeItem g_page0Items[3];
static RangeItem g_page0Type, g_page0Layout;      // the game's TYPE item, our LAYOUT item
static int16_t   g_page0Rows[4] = { 23, SPR_KB_TITLE, 26, 26 };
static int16_t   g_page0Help[4] = { 63, 63, 62, 62 };
// 5 visible rows (104..296): a 6th row at 344 put its highlight bar over the help panel (0.8.1)
static const int kRowPitch = 48, kRow0Y = 104, kVisibleRows = 5, kMaxRows = 15;
static const uintptr_t kPage0Count = 0x716FB5;   // mov edi, imm32 (CONTROLLER SETTING item count)
static const uintptr_t kPage0RowsPtr = 0x939B00, kPage0HelpPtr = 0x939ABC;
static const uintptr_t kPage0RowsOrig = 0x939AD8, kPage0HelpOrig = 0x939A94;
static const uintptr_t kCtrlType = 0xAB8F88;     // byte: 0 keyboard, 1 joypad (applied on menu exit)
static int RowY(int r) { return kRow0Y + r * kRowPitch; }                                    // unscrolled
static const int kSbX = 509, kSbTop = 104, kSbH = kVisibleRows * kRowPitch - 16;

// ---------------------------------------------------------------- display
// Both modes use one borderless window covering the whole monitor (display mode never changes).
// The game renders at the menu resolution; Present() gets a centred 4:3 destination rect and
// DXVK's blitter scales into it and clears the rest of the screen to black.
static int  g_monX, g_monY, g_monW, g_monH;
static HINSTANCE g_exeInst;

static void QueryMonitor() {
    HMONITOR m = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof mi }; GetMonitorInfoA(m, &mi);
    g_monX = mi.rcMonitor.left; g_monY = mi.rcMonitor.top;
    g_monW = mi.rcMonitor.right - mi.rcMonitor.left; g_monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
}
static HWND ZOrderFor(int mode) { return HWND_TOPMOST; }   // refined per frame by UpdateTopmost

typedef HWND (WINAPI* PFN_CWEA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef BOOL (WINAPI* PFN_SWP)(HWND, HWND, int, int, int, int, UINT);
static PFN_CWEA o_CreateWindowExA = nullptr;
static PFN_SWP  o_SetWindowPos   = nullptr;

static bool IsGameClass(LPCSTR cls) {
    LPCSTR gameCls = *(LPCSTR*)A::ClassNamePtr;
    return cls && gameCls && !IS_INTRESOURCE(cls) && !strcmp(cls, gameCls);
}
static HWND WINAPI hk_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                                      HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    if (IsGameClass(cls)) {
        g_exeInst = inst; QueryMonitor();
        style = WS_POPUP; ex &= ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE);
        ex |= WS_EX_TOPMOST;
        x = g_monX; y = g_monY; w = g_monW; h = g_monH; parent = nullptr;
        Log("game window (%s): %dx%d at %d,%d, render %dx%d", kModeName[g_mode], w, h, x, y,
            *(int*)A::BackBufW, *(int*)A::BackBufH);
    }
    return o_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
}
static void PlaceGameWindow(HWND hw, UINT extra) {
    QueryMonitor();
    DWORD st = (DWORD)GetWindowLongA(hw, GWL_STYLE), want = WS_POPUP | (st & WS_VISIBLE);
    if (st != want) { SetWindowLongA(hw, GWL_STYLE, want); extra |= SWP_FRAMECHANGED; }
    o_SetWindowPos(hw, ZOrderFor(g_mode), g_monX, g_monY, g_monW, g_monH, extra);
}
static BOOL WINAPI hk_SetWindowPos(HWND hw, HWND after, int x, int y, int cx, int cy, UINT f) {
    if (hw && hw == *(HWND*)A::MainHwnd) {
        PlaceGameWindow(hw, f & ~(SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER));
        Log("SetWindowPos (%s): %dx%d, render %dx%d", kModeName[g_mode], g_monW, g_monH, *(int*)A::BackBufW, *(int*)A::BackBufH);
        return TRUE;
    }
    return o_SetWindowPos(hw, after, x, y, cx, cy, f);
}

// IDirect3DDevice8::Present replacement (call sites 0x7B29DA, 0x7B31CD) -------------------------
typedef HRESULT (WINAPI* PFN_Present)(void*, const RECT*, const RECT*, HWND, const void*);
static RECT g_lastDst = { };
static int g_topmost = -1;
static void UpdateTopmost(HWND hw) {
    // Above the taskbar while it has focus, drops back when you alt-tab away (both screen modes).
    int want = GetForegroundWindow() == hw ? 1 : 0;      // 0.12.1: FULLSCREEN too, so alt-tab shows the other window
    if (want == g_topmost) return;
    g_topmost = want;
    o_SetWindowPos(hw, want ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
static volatile uint32_t g_presentSite = 0;      // return address of the game's Present call (diagnostics for SoD-RT)
static HRESULT WINAPI hk_Present(void* dev, const RECT* src, const RECT* dst, HWND wnd, const void* dirty) {
    g_presentSite = (uint32_t)(uintptr_t)__builtin_return_address(0);
    auto real = (*(PFN_Present**)dev)[15];
    HWND hw = *(HWND*)A::MainHwnd;
    if (hw) UpdateTopmost(hw);
    RECT cr;
    if (!dst && hw && GetClientRect(hw, &cr) && cr.right > 0 && cr.bottom > 0) {
        int cw = cr.right, ch = cr.bottom, gw = ch * 4 / 3, gh = ch;
        if (gw > cw) { gw = cw; gh = cw * 3 / 4; }
        RECT d = { (cw - gw) / 2, (ch - gh) / 2, (cw - gw) / 2 + gw, (ch - gh) / 2 + gh };
        if (memcmp(&d, &g_lastDst, sizeof d)) {
            g_lastDst = d;
            Log("present: render %dx%d -> %dx%d at %d,%d in %dx%d", *(int*)A::BackBufW, *(int*)A::BackBufH,
                gw, gh, (int)d.left, (int)d.top, cw, ch);
        }
        return real(dev, src, &d, wnd, dirty);
    }
    return real(dev, src, dst, wnd, dirty);
}

// ---------------------------------------------------------------- game-side callbacks (called from stubs, cdecl)
extern "C" {

// before every window/device (re)creation: D3D always windowed (no display mode switch)
static void __cdecl cb_BeforeCreateDevice() { *(uint8_t*)A::Windowed = 1; }

// 0x715FB0 replacement: every listed resolution works (the display mode never changes)
static int __cdecl cb_ValidateRes(int idx) { return (idx >= 0 && idx < kResCount) ? 1 : 0; }

// --- GRAPHIC SETTING page: extra rows, scrolling, RT <-> SHADOW CASTING interlock ---------------------
static uint8_t* g_menuObj = nullptr;
static int  g_calMenuSeen = 0;          // counts down from the last option-menu frame
static bool g_gsFlag = false;
// option menu state (obj+0x2C): 4 the option list, 5-6 a page sliding in, 7 inside a page, 8 sliding back out
static int MenuState(const uint8_t* obj) { return obj ? *(const int*)(obj + 0x2C) : -1; }
static bool g_calOpenFlag = false;      // HDR CALIBRATION screen is open (Cal::open)
static bool g_rmOpenFlag = false;       // REMAP screen is open (Rm::open)
static uint8_t* g_calPool = nullptr;    // the pool of the sprites blanked for our screens

static int  g_lastRt = -1, g_lastSc = -1;

// Options pages with rows added by SoDFix. Per page: the sprites of every row (label, drop shadow, values, value
// shadows, underlines; -1 ends), unselected label templates (sprite, shadow, row) for our new rows' titles, a
// single-value sprite (+ shadow, row) as template for their values, the underline used for the scroll bar's
// colour and visibility, and the scroll-bar sprites. New rows are built from templates of their own page, so they
// appear and slide with that page only. Label/shadow pairs: table 0x939360.
struct PageDef {
    int page, nRows;
    int16_t rows[kMaxRows][10];
    int labT[3], shT[3], rowT[3];
    int valT, valShT, valRowT;
    int sbTemplate, sbTrack, sbThumb;
    int scroll;
};
static PageDef g_pages[] = {
    { 3, 14, {
        {  97,  93, 101, 102, -1 },                               // RESOLUTION
        { 120, 121, 122, 123, -1 },                               // SCREEN MODE
        {  98,  94, 103, 104, -1 },                               // DRAW DISTANCE
        {  99,  95, 105, 106, 107, 108, 109, -1 },                // FOCUS, BLUR AND FADES
        { 100,  96, 110, 113, 111, 114, 112, 115, 116, -1 },      // SHADOW CASTING
        { 124, 125, 126, 127, -1 },                               // RT SHADOW CASTING
        { 130, 131, 132, 133, -1 },                               // ANTI-ALIASING
        { 134, 135, 136, 137, -1 },                               // TEXTURE FILTERING
        { 154, 155, 156, 157, -1 },                               // AMBIENT OCCLUSION
        { 158, 159, 160, 161, -1 },                               // REFLECTIONS
        { 162, 163, 164, 165, -1 },                               // TEXTURE UPSCALING
        { 166, 167, 168, 169, -1 },                               // IMPROVED GEOMETRY
        { 181, 182, 183, 184, -1 },                               // HDR
        { 117, -1 } },                                            // EXIT
      { 98, 99, 100 }, { 94, 95, 96 }, { 2, 3, 4 }, 103, 104, 2, 109, SPR_SB_TRACK, SPR_SB_THUMB, 0 },
    { 2, 7, {
        {  73,  57,  74,  58, -1 },                               // TEXT DISPLAY
        {  75,  59,  76,  60,  77,  61,  78, -1 },                // COMPASS
        {  79,  62,  80,  63,  81,  64,  82, -1 },                // ITEM EMPHASIS
        {  83,  65,  84,  85,  86,  87,  88,  66, -1 },           // DOOR EMPHASIS
        { 138, 139, 140, 141, 151, 152, 153, -1 },                // CINEMATIC BARS (ON, OFF, underline like COMPASS)
        { 170, 171, 172, 173, 174, 175, 176, -1 },                // SMOOTH CAMERA (same)
        {  89,  67, -1 } },                                       // EXIT
      { 75, 79, 83 }, { 59, 62, 65 }, { 1, 2, 3 }, 74, 58, 0, 78, SPR_SB2_TRACK, SPR_SB2_THUMB, 0 },
};
static PageDef* FindPage(int page) { for (auto& p : g_pages) if (p.page == page) return &p; return nullptr; }

static inline uint8_t* Spr(uint8_t* pool, int i) { return pool + i * 0x60; }
static void ShiftX(uint8_t* sp, int px) {
    int d = px << 4; *(int*)(sp + 0x10) += d; *(int*)(sp + 0x20) += d; *(int*)(sp + 0x30) += d;
}
// our label art has 5 px of blank left of the ink, the game's label rects 1: titles are drawn 4 px further left
static const int kTitleDx = -4;
static void ShiftY(uint8_t* sp, int px) {
    int d = px << 4; *(int*)(sp + 0x14) += d; *(int*)(sp + 0x24) += d; *(int*)(sp + 0x34) += d;
}

static void SetPos(uint8_t* sp, const uint8_t* ref, int dx, int dy) {   // ref-relative, keeps ref's animation
    *(int*)(sp + 0x10) = *(const int*)(ref + 0x10) + (dx << 4); *(int*)(sp + 0x14) = *(const int*)(ref + 0x14) + (dy << 4);
    *(int*)(sp + 0x20) = *(const int*)(ref + 0x20) + (dx << 4); *(int*)(sp + 0x24) = *(const int*)(ref + 0x24) + (dy << 4);
    *(int*)(sp + 0x30) = *(const int*)(ref + 0x30) + (dx << 4); *(int*)(sp + 0x34) = *(const int*)(ref + 0x34) + (dy << 4);
}
// Rows outside the visible window draw an empty rect. The draw flag can't be used for this: the game's sprite
// update turns it back on while a page slides in, so the extra rows flashed over the help panel (0.10.1).
// The rect pointer is only ever set by the game's own menu code, before our per-frame hook runs.
static const SpriteRect kEmptyRect = { 0, 0, 0, 0, 0, 0 };
static const SpriteRect* g_savedRect[POOL_NEW];
// Our screens (HDR CALIBRATION, the redesigned GRAPHIC SETTING) blank the menu's own text and cursor bar the same way:
// their rect is pointed at kEmptyRect right before drawing and given back at the start of the next frame (input
// poll, and again at the menu hook). Positions are never touched: the menu's logic and the sprite update read them.
static const SpriteRect* g_calRect[POOL_NEW];
static void MenuSpritesRestore() {
    if (!g_calPool) return;
    for (int i = 0; i < POOL_NEW; i++) if (g_calRect[i]) {
        const SpriteRect** r = (const SpriteRect**)(Spr(g_calPool, i) + 0x48);
        if (*r == &kEmptyRect) *r = g_calRect[i];
        g_calRect[i] = nullptr;
    }
    g_calPool = nullptr;
}
static void CopySprite(uint8_t* pool, int dst, int src, const SpriteRect* rect, int dyPx) {
    uint8_t* d = Spr(pool, dst);
    memcpy(d, Spr(pool, src), 0x60);
    *(const SpriteRect**)(d + 0x48) = rect;
    ShiftY(d, dyPx);
}
// one of our rows: title + shadow from an unselected label of the same page, value + shadow from the page's
// single-value sprite (templates already carry the page's scroll offset and slide animation)
static void BuildRowSprites(uint8_t* pool, const PageDef& pd, int row, const int16_t* spr, const SpriteRect* titleRect,
                            int titleYOff, const SpriteRect* valueRect, int valueYOff, int myRow) {
    int k = 0; while (k < 2 && pd.rowT[k] == row) ++k;
    int tl = pd.labT[k], ts = pd.shT[k], ty = RowY(pd.rowT[k]);
    uint8_t keep[10]; uint8_t* t = Spr(pool, spr[0]);
    memcpy(keep, t + 0x40, 8); memcpy(keep + 8, t + 0x54, 2);
    CopySprite(pool, spr[0], tl, titleRect, RowY(myRow) - ty + titleYOff);
    CopySprite(pool, spr[1], ts, titleRect, RowY(myRow) - ty + titleYOff);
    ShiftX(Spr(pool, spr[0]), kTitleDx); ShiftX(Spr(pool, spr[1]), kTitleDx);
    if (row == myRow) { memcpy(t + 0x40, keep, 8); memcpy(t + 0x54, keep + 8, 2); }   // keep game's highlight
    CopySprite(pool, spr[2], pd.valT,   valueRect, RowY(myRow) - RowY(pd.valRowT) + valueYOff);
    CopySprite(pool, spr[3], pd.valShT, valueRect, RowY(myRow) - RowY(pd.valRowT) + valueYOff);
}

// Some game sprites are re-placed at their unscrolled position by the game's value refresh (0x7160E0, run on every
// cursor move): DOOR EMPHASIS's underline (sprite 84) is set to y 276 absolutely (0x716317), so it stayed behind when
// the page scrolled (0.11.1). Every frame, each game sprite of a scrolled page whose y is exactly the unscrolled
// position of where we left it gets the scroll offset back. Only pos is written by the game (0x776B10).
static int  g_trackY[120];
static bool g_trackOk[120];
static void PageTrackFix(uint8_t* pool, PageDef& pd) {
    int off = pd.scroll * kRowPitch << 4;
    for (int r = 0; r < pd.nRows; ++r)
        for (int k = 0; k < 10 && pd.rows[r][k] >= 0; ++k) {
            int i = pd.rows[r][k]; if (i >= 120) continue;
            int* y = (int*)(Spr(pool, i) + 0x14);
            if (off && g_trackOk[i] && *y == g_trackY[i] + off) *y -= off;
        }
}
static void PageTrackSave(uint8_t* pool, PageDef& pd) {
    for (int r = 0; r < pd.nRows; ++r)
        for (int k = 0; k < 10 && pd.rows[r][k] >= 0; ++k) {
            int i = pd.rows[r][k]; if (i >= 120) continue;
            g_trackY[i] = *(int*)(Spr(pool, i) + 0x14); g_trackOk[i] = true;
        }
}
// keep the cursor row inside the visible window; scroll bar; rows outside the window draw an empty rect
static void PageScroll(uint8_t* pool, PageDef& pd, int row, bool moveBar) {
    PageTrackFix(pool, pd);
    int want = pd.scroll;
    if (row >= 0) {
        if (row < want) want = row;
        if (row > want + kVisibleRows - 1) want = row - (kVisibleRows - 1);
    }
    if (want < 0) want = 0;
    if (want > pd.nRows - kVisibleRows) want = pd.nRows - kVisibleRows;
    if (want != pd.scroll) {
        int dy = -(want - pd.scroll) * kRowPitch;
        for (int r = 0; r < pd.nRows; ++r)
            for (int k = 0; k < 10 && pd.rows[r][k] >= 0; ++k) if (pd.rows[r][k] < 120) ShiftY(Spr(pool, pd.rows[r][k]), dy);
        if (moveBar) ShiftY(Spr(pool, 1), dy);         // cursor bar, already placed for this frame (only while it is
                                                       // on the page: in the list or a slide it is the list's bar)
        pd.scroll = want;
    }
    PageTrackSave(pool, pd);
}
static void PageFinish(uint8_t* pool, PageDef& pd) {
    // scroll bar: track + thumb, visibility/colour from one of the page's underlines, position from a label
    int thumbH = kSbH * kVisibleRows / pd.nRows;
    SpriteRect* track = pd.page == 3 ? &g_sbTrack : &g_sbTrack2; SpriteRect* thumb = pd.page == 3 ? &g_sbThumb : &g_sbThumb2;
    track->h = (int16_t)kSbH; thumb->h = (int16_t)thumbH;
    int thumbY = kSbTop + pd.scroll * (kSbH - thumbH) / (pd.nRows - kVisibleRows);
    const uint8_t* ref = Spr(pool, pd.labT[0]);
    int refY = RowY(pd.rowT[0]) - pd.scroll * kRowPitch;     // where the reference label sits now
    CopySprite(pool, pd.sbTrack, pd.sbTemplate, track, 0); SetPos(Spr(pool, pd.sbTrack), ref, kSbX - 6, kSbTop - refY);
    CopySprite(pool, pd.sbThumb, pd.sbTemplate, thumb, 0); SetPos(Spr(pool, pd.sbThumb), ref, kSbX - 7, thumbY - refY);
    for (int r = 0; r < pd.nRows; ++r) {
        bool vis = (r >= pd.scroll && r < pd.scroll + kVisibleRows);
        for (int k = 0; k < 10 && pd.rows[r][k] >= 0; ++k) {
            int i = pd.rows[r][k];
            const SpriteRect** rp = (const SpriteRect**)(Spr(pool, i) + 0x48);
            if (*rp != &kEmptyRect) g_savedRect[i] = *rp;        // the game (or our row builder) set a new rect
            if (!vis) *rp = &kEmptyRect;
            else if (*rp == &kEmptyRect) *rp = g_savedRect[i];
        }
    }
}

// ---------------------------------------------------------------- REMAP (0.24.0): own keys and buttons per action
// The game's actions are its own keys (DirectInput scan codes, as in the manual). Keyboard: each action's key can be
// any key ([Controls] KeyMap, 12 scan codes). Controller: each action's button ([Controls] PadMap, 12 codes: 0..14
// SDL buttons, 15 LT, 16 RT; -1 = the default, which for Nintendo pads swaps confirm and back). The sticks always
// move (left) and turn the camera (right); LT / RT / View keep turning the camera / changing the view unless bound.
namespace Bind {
    enum { N = 12, PAD_LT = 15, PAD_RT = 16, PAD_N = 17 };
    struct Action { const char* label; const char* desc; uint8_t key; int8_t pad; };
    static const Action kActions[N] = {
        { "MOVE UP",          "Walk forward, and up in menus.",                                0xC8, 11 },
        { "MOVE DOWN",        "Walk back, and down in menus.",                                 0xD0, 12 },
        { "MOVE LEFT",        "Walk left, and left in menus (changes a setting).",            0xCB, 13 },
        { "MOVE RIGHT",       "Walk right, and right in menus (changes a setting).",          0xCD, 14 },
        { "ACTION / CONFIRM", "Talk, look at things, open doors, and confirm in menus.",      0x1C, 0 },
        { "CANCEL / SKIP",    "Go back in menus and skip scenes.",                            0x0E, 1 },
        { "ITEMS",            "Open the items you carry.",                                     0x9D, 2 },
        { "CITY MAP",         "Open the map of Lebensbaum.",                                   0x36, 3 },
        { "CAMERA LEFT",      "Turn the camera to the left.",                                  0xD3, 9 },
        { "CAMERA RIGHT",     "Turn the camera to the right.",                                 0xD1, 10 },
        { "CHANGE VIEW",      "Switch between the camera views.",                              0xCF, 8 },
        { "PAUSE",            "Pause the game and skip videos.",                               0x01, 6 },
    };
    static uint8_t key[N]; static int8_t pad[N]; static bool loaded = false;
    static void Load() {
        if (loaded) return; loaded = true;
        for (int a = 0; a < N; a++) { key[a] = kActions[a].key; pad[a] = -1; }
        char b[256];
        GetPrivateProfileStringA("Controls", "KeyMap", "", b, sizeof b, g_ini);
        { const char* q = b; for (int a = 0; a < N && *q; a++) { char* e; long v = strtol(q, &e, 10); if (e == q) break;
            if (v > 0 && v < 256) key[a] = (uint8_t)v; q = *e == ',' ? e + 1 : e; } }
        GetPrivateProfileStringA("Controls", "PadMap", "", b, sizeof b, g_ini);
        { const char* q = b; for (int a = 0; a < N && *q; a++) { char* e; long v = strtol(q, &e, 10); if (e == q) break;
            pad[a] = (v >= 0 && v < PAD_N && v != 5) ? (int8_t)v : -1; q = *e == ',' ? e + 1 : e; } }
    }
    static void Save() {
        char b[256]; int n = 0;
        for (int a = 0; a < N; a++) n += snprintf(b + n, sizeof b - n, a ? ",%d" : "%d", key[a]);
        WritePrivateProfileStringA("Controls", "KeyMap", b, g_ini);
        n = 0;
        for (int a = 0; a < N; a++) n += snprintf(b + n, sizeof b - n, a ? ",%d" : "%d", pad[a]);
        WritePrivateProfileStringA("Controls", "PadMap", b, g_ini);
        Log("remap saved: KeyMap and PadMap");
    }
    static bool KeyCustom() { Load(); for (int a = 0; a < N; a++) if (key[a] != kActions[a].key) return true; return false; }
    // the pad input an action uses on a pad of this kind (nin: Nintendo layout, confirm on the right face button)
    static int PadOf(int a, bool nin) {
        Load();
        if (pad[a] >= 0) return pad[a];
        if (nin && a == 4) return 1;
        if (nin && a == 5) return 0;
        return kActions[a].pad;
    }
    // keyboard: the game's keys from the keys chosen for them (in = the keys as pressed)
    static void ApplyKeys(uint8_t* k, const uint8_t* in) {
        if (!KeyCustom()) return;
        for (int a = 0; a < N; a++) k[kActions[a].key] = 0;
        for (int a = 0; a < N; a++) if (in[key[a]] & 0x80) k[kActions[a].key] = 0x80;
    }
    static bool KeyBound(uint8_t sc) { Load(); for (int a = 0; a < N; a++) if (key[a] == sc && key[a] != kActions[a].key) return true; return false; }
}
#include "pad.inl"
static bool  g_haveAtlas = false;
static bool RemapOn() { return g_cfg.newMenu && g_cfg.modeMenu && g_haveAtlas; }
// the CONTROLLER page's TYPE follows the device in use (SDL3 reads the controllers and presses the game's keys,
// so the game itself stays on KEYBOARD)
static bool TypeAuto() { return RemapOn() && Pad::g_ready; }
static int RemapRow(const uint8_t* obj) { return (obj && *(const int*)(obj + 0x54) == 0) ? 2 : 1; }

// --- CONTROLLER SETTING page: LAYOUT row under TYPE while TYPE is KEYBOARD -----------------------------------
// Game sprites: TYPE label 23 (shadow 15), KEYBOARD 24 (16) at x 272, JOYPAD 25 (17) at x 408, underline 18 under the
// selected value (0x716450: selected value style via 0x716350(spr, 1), underline at value pos + (-2, +28)), EXIT 26 (19)
// at y 152, keyboard/joypad pictures 21/22 (faded by TYPE, drawn behind the rows). TYPE value 0 = KEYBOARD.
static int g_p0Shift = 0;                        // px EXIT (26/19) has been moved down by us
static int g_p0Count = 1;                        // item count currently written at kPage0Count
static SpriteRect g_kbLine = { 0, 0, 0, 0, 86, 2 };
static const int16_t kKbSprites[] = { SPR_KB_TITLE, SPR_KB_TITLE_SH, SPR_KB_CLASSIC, SPR_KB_CLASSIC_SH,
                                      SPR_KB_MODERN, SPR_KB_MODERN_SH, SPR_KB_LINE };
static const int kKbValueYOff = 3;               // our labels' ink starts 3 rows above KEYBOARD's in its 32-row rect
static const int kModernDx = -20;
static const int kClassicDx = -1;                // ink 5 px into our rect, 4 px into KEYBOARD's                // MODERN is wider than JOYPAD: 388..504 instead of 408..524
static void SetPage0Layout(bool kb) {
    const bool rm = RemapOn();
    int count = (kb ? 2 : 1) + (rm ? 1 : 0);
    if (rm) {                                    // TYPE, [LAYOUT,] REMAP, EXIT
        RangeItem t = g_page0Type; if (TypeAuto()) t.max = t.min = 0;
        const RangeItem remap = { 0, 0, (int16_t)kRemapId };
        g_page0Items[0] = t;
        if (kb) { g_page0Items[1] = g_page0Layout; g_page0Items[2] = remap;
                  g_page0Rows[1] = SPR_KB_TITLE; g_page0Rows[2] = SPR_KB_TITLE; g_page0Rows[3] = 26;
                  g_page0Help[1] = 63; g_page0Help[2] = 63; g_page0Help[3] = 62; }
        else    { g_page0Items[1] = remap;
                  g_page0Rows[1] = SPR_KB_TITLE; g_page0Rows[2] = 26; g_page0Help[1] = 63; g_page0Help[2] = 62; }
    }
    if (count != g_p0Count) { WriteMem(kPage0Count, &count, 4); g_p0Count = count; }
    uint32_t rows = (kb || rm) ? (uint32_t)(uintptr_t)g_page0Rows : (uint32_t)kPage0RowsOrig;
    uint32_t help = (kb || rm) ? (uint32_t)(uintptr_t)g_page0Help : (uint32_t)kPage0HelpOrig;
    if (*(uint32_t*)kPage0RowsPtr != rows) WriteMem(kPage0RowsPtr, &rows, 4);
    if (*(uint32_t*)kPage0HelpPtr != help) WriteMem(kPage0HelpPtr, &help, 4);
}
// dst = style of styleSrc (selected/dim, slide state), placed at posSrc + (dx, dy)
static void CopyStyled(uint8_t* pool, int dst, int styleSrc, int posSrc, const SpriteRect* rect, int dx, int dy) {
    uint8_t* d = Spr(pool, dst); const uint8_t* p = Spr(pool, posSrc);
    memcpy(d, Spr(pool, styleSrc), 0x60);
    *(const SpriteRect**)(d + 0x48) = rect;
    for (int f : { 0x10, 0x20, 0x30 }) {
        *(int*)(d + f)     = *(const int*)(p + f) + (dx << 4);
        *(int*)(d + f + 4) = *(const int*)(p + f + 4) + (dy << 4);
    }
}
static void Page0Frame(uint8_t* pool, uint8_t* obj, int row0) {
    if (TypeAuto()) *(int*)(obj + 0x54) = 0;     // TYPE: the game stays on KEYBOARD, the text follows the device
    bool kb = *(int*)(obj + 0x54) == 0;
    int* lay = (int*)(obj + 0x54 + kKbId * 4);
    if (*lay < 0 || *lay > 1) *lay = 0;
    SetPage0Layout(kb);
    int want = kb ? kRowPitch : 0;
    if (want != g_p0Shift) {                     // EXIT and the keyboard/joypad pictures move down a row together
        for (int i : { 26, 19 }) ShiftY(Spr(pool, i), want - g_p0Shift);
        for (int i : { 21, 22 }) ShiftY(Spr(pool, i), (want - g_p0Shift) * 2 / 3);   // 32 px: clear of the help panel
        g_p0Shift = want;
    }
    if (!kb) {
        for (int i : kKbSprites) *(const SpriteRect**)(Spr(pool, i) + 0x48) = &kEmptyRect;
        return;
    }
    // title: from TYPE's label, unselected unless the cursor is on LAYOUT (the game highlights rows[1] = our sprite)
    uint8_t* t = Spr(pool, SPR_KB_TITLE); uint8_t keep[10];
    memcpy(keep, t + 0x40, 8); memcpy(keep + 8, t + 0x54, 2);
    CopySprite(pool, SPR_KB_TITLE,    23, &kArt_LAYOUT, kRowPitch + kArt_LAYOUT_yoff);
    CopySprite(pool, SPR_KB_TITLE_SH, 15, &kArt_LAYOUT, kRowPitch + kArt_LAYOUT_yoff);
    ShiftX(t, kTitleDx); ShiftX(Spr(pool, SPR_KB_TITLE_SH), kTitleDx);
    if (row0 == 1) { memcpy(t + 0x40, keep, 8); memcpy(t + 0x54, keep + 8, 2); }
    else { *(int*)(t + 0x40) = 0; *(int*)(t + 0x44) = 0; *(uint16_t*)(t + 0x54) = 0; }
    // values: KEYBOARD (24) is the selected one while this row is shown, JOYPAD (25) the dim one
    bool modern = *lay == 1;
    int selV = 24, dimV = 25, selS = 16, dimS = 17;
    CopyStyled(pool, SPR_KB_CLASSIC,    modern ? dimV : selV, 24, &kArt_CLASSIC, kClassicDx, kRowPitch + kKbValueYOff);
    CopyStyled(pool, SPR_KB_CLASSIC_SH, modern ? dimS : selS, 16, &kArt_CLASSIC, kClassicDx, kRowPitch + kKbValueYOff);
    CopyStyled(pool, SPR_KB_MODERN,     modern ? selV : dimV, 25, &kArt_MODERN, kModernDx, kRowPitch + kKbValueYOff);
    CopyStyled(pool, SPR_KB_MODERN_SH,  modern ? selS : dimS, 17, &kArt_MODERN, kModernDx, kRowPitch + kKbValueYOff);
    // underline: like 0x716450, under the selected value
    g_kbLine.w = modern ? kArt_MODERN.w - 6 : kArt_CLASSIC.w - 5;
    CopySprite(pool, SPR_KB_LINE, 18, &g_kbLine, kRowPitch);
    uint8_t* ln = Spr(pool, SPR_KB_LINE); const uint8_t* v = Spr(pool, modern ? 25 : 24);
    *(int*)(ln + 0x10) = *(const int*)(v + 0x10) + ((modern ? kModernDx : kClassicDx) << 4) - 0x20;
    *(int*)(ln + 0x14) = *(const int*)(v + 0x14) + (kRowPitch << 4) + 0x1C0;
}

// CINEMATIC BARS values like COMPASS (row 1: ON 76/60, OFF 77/61, underline 78; 0x716380: selected value via
// 0x716350(spr,1), underline rect 0x24 (ON) / 0x25 (OFF) at selected value pos + (-2, +28)); COMPASS = obj+0x70
static const SpriteRect* GameRect(int i) { return (const SpriteRect*)(A::SpriteTable + 12 * i); }
static void BuildOnOffRow(uint8_t* pool, const int16_t* spr, int myRow, int v, int compassV) {
    int dy = RowY(myRow) - RowY(1);
    int selC = compassV == 0 ? 76 : 77, dimC = compassV == 0 ? 77 : 76;
    int selS = compassV == 0 ? 60 : 61, dimS = compassV == 0 ? 61 : 60;
    CopyStyled(pool, spr[2], v == 0 ? selC : dimC, 76, GameRect(0x0A), 0, dy);
    CopyStyled(pool, spr[3], v == 0 ? selS : dimS, 60, GameRect(0x0A), 0, dy);
    CopyStyled(pool, spr[4], v == 1 ? selC : dimC, 77, GameRect(0x0B), 0, dy);
    CopyStyled(pool, spr[5], v == 1 ? selS : dimS, 61, GameRect(0x0B), 0, dy);
    CopySprite(pool, spr[6], 78, GameRect(v == 0 ? 0x24 : 0x25), dy);
    uint8_t* ln = Spr(pool, spr[6]); const uint8_t* sv = Spr(pool, v == 0 ? spr[2] : spr[4]);
    *(int*)(ln + 0x10) = *(const int*)(sv + 0x10) - 0x20;
    *(int*)(ln + 0x14) = *(const int*)(sv + 0x14) + 0x1C0;
}
// the three DOORWAY EMPHASIS icons aren't sprites: 0x716C10 draws them at x 0x130 + 40i, y 244 (push imm @0x716C35)
static const uintptr_t kDoorIconY = 0x716C36;
static int g_doorY = 0xF4;
static const uintptr_t kDoorIconX = 0x716C19;    // mov ebx, 0x130 (first icon's x; the next two +40)
static int g_doorX = 0x130;
static const int kDoorPanelX = 342, kDoorPanelY = 232;   // the flashing icons in the right-hand panel
static const int kDoorStillX = 250, kDoorStillY = 222;   // the chosen style, still, as the DOORWAY row's value
// DOORWAY row selected with a style chosen: its selection bar is drawn by the game (a copy of the cursor bar sprite,
// plain quad sized to our row) so the still icon, drawn after the menu sprites, sits on top of it. Our layout is
// 640 x 480 over the menu's 512 x 448 sprite space. Row 3 of DISPLAY: x 4..362, y 211..251 (pitch 40).
static SpriteRect g_doorBarRect = { 0, 0, 0, 0, (int16_t)((362 - 4) * 512 / 640), (int16_t)(40 * 448 / 480) };

// after the menu object has been filled from settings
static void __cdecl cb_MenuInit(uint8_t* obj) {
    *(int*)(obj + 0x54 + kModeId * 4) = ModeToMenu(g_mode);
    *(int*)(obj + 0x54 + kRtId * 4) = g_cfg.rtLevel;
    *(int*)(obj + 0x54 + kAaId * 4) = g_cfg.aaMode;
    *(int*)(obj + 0x54 + kTfId * 4) = g_cfg.tfMode;
    *(int*)(obj + 0x54 + kCbId * 4) = g_cfg.bars ? 0 : 1;
    *(int*)(obj + 0x54 + kKbId * 4) = g_cfg.kbLayout;
    *(int*)(obj + 0x54 + kAoId * 4) = g_cfg.aoMode;
    *(int*)(obj + 0x54 + kSsrId * 4) = g_cfg.ssrMode;
    *(int*)(obj + 0x54 + kUpId * 4) = g_cfg.upMode;
    *(int*)(obj + 0x54 + kGeomId * 4) = g_cfg.geomMode;
    *(int*)(obj + 0x54 + kScId * 4) = g_cfg.smoothCam ? 0 : 1;
    *(int*)(obj + 0x54 + kFpsId * 4) = g_cfg.fpsUnlock;
    *(int*)(obj + 0x54 + kHdrId * 4) = g_cfg.hdr;
    *(int*)(obj + 0x54 + kRemapId * 4) = 0;
    if (TypeAuto()) *(int*)(obj + 0x54) = 0;
    g_p0Shift = 0;                                   // layout positions were just applied (0x7173D0)
    SetPage0Layout(*(int*)(obj + 0x54) == 0);
    memset(g_trackOk, 0, sizeof g_trackOk);
    if (g_cfg.rtLevel > 0) { *(int*)(obj + 0x90) = 2; ((void (__cdecl*)(int))0x716060)(2); }   // SHADOW CASTING OFF
    g_menuMode = ModeToMenu(g_mode);
    if (obj != g_menuObj) { g_menuObj = obj; for (auto& p : g_pages) p.scroll = 0; }
    g_lastRt = -1; g_lastSc = -1;
}

// every menu frame, before the sprite pool is updated and drawn (esi = menu object)
static void __cdecl cb_MenuFrame(uint8_t* obj) {
    if (!obj) return;
    g_calMenuSeen = 3;
    uint8_t* pool = *(uint8_t**)(obj + 0x28);
    if (!pool) return;
    MenuSpritesRestore();                               // blanked sprites back before anything below reads them
    // HDR CALIBRATION open: the cursor stays on the HDR row (the mouse can move it underneath), so leaving the
    // screen returns there
    if (g_calOpenFlag && *(int*)(obj + 0x30) == 3) *(int*)(obj + 0x38) = kHdrRow;
    if (g_rmOpenFlag && *(int*)(obj + 0x30) == 0) *(int*)(obj + 0x38) = RemapRow(obj);
    int page = *(int*)(obj + 0x30), cur = *(int*)(obj + 0x38);
    // Rows only count while that page is open (states 5-8). In the option list obj+0x30 is just the highlighted
    // entry and obj+0x38 the row that page was left on: treating that as an open page scrolled the page and the
    // list's cursor bar with it, and showed the page's help line under the list.
    const int st0 = MenuState(obj);
    const bool pageOpen = st0 >= 5 && st0 <= 8;
    int row = pageOpen && page == 3 ? cur : -1, row2 = pageOpen && page == 2 ? cur : -1, row0 = pageOpen && page == 0 ? cur : -1;
    int* mode = (int*)(obj + 0x54 + kModeId * 4);
    int* rt   = (int*)(obj + 0x54 + kRtId * 4);
    int* aa   = (int*)(obj + 0x54 + kAaId * 4);
    int* tf   = (int*)(obj + 0x54 + kTfId * 4);
    int* cb   = (int*)(obj + 0x54 + kCbId * 4);
    int* ao   = (int*)(obj + 0x54 + kAoId * 4);
    int* ssr  = (int*)(obj + 0x54 + kSsrId * 4);
    int* up   = (int*)(obj + 0x54 + kUpId * 4);
    int* geom = (int*)(obj + 0x54 + kGeomId * 4);
    int* smc  = (int*)(obj + 0x54 + kScId * 4);
    int* fps  = (int*)(obj + 0x54 + kFpsId * 4);
    int* hdr  = (int*)(obj + 0x54 + kHdrId * 4);
    if (*hdr < 0 || *hdr > 1) *hdr = 0;
    int* sc   = (int*)(obj + 0x90);                     // SHADOW CASTING value
    if (*mode < 0 || *mode > 1) *mode = ModeToMenu(g_mode);
    if (*rt < 0 || *rt > 3) *rt = 0;
    if (*aa < 0 || *aa > 1) *aa = 0;
    if (*tf < 0 || *tf > 2) *tf = 0;
    if (*cb < 0 || *cb > 1) *cb = 0;
    if (*ao < 0 || *ao > 2) *ao = 0;
    if (*ssr < 0 || *ssr > 2) *ssr = 0;
    if (*up < 0 || *up > 3) *up = 3;
    if (*geom < 0 || *geom > 1) *geom = 0;
    if (*smc < 0 || *smc > 1) *smc = 0;
    if (*fps < 0 || *fps > 1) *fps = 0;
    g_menuMode = *mode;

    // RT SHADOW CASTING on -> SHADOW CASTING OFF; SHADOW CASTING back on -> RT SHADOW CASTING OFF
    if (g_lastRt >= 0) {
        if (*rt != g_lastRt && *rt > 0 && *sc != 2) {
            *sc = 2; ((void (__cdecl*)(int))0x716060)(2);
            ((void (__cdecl*)(uint8_t*))0x7160E0)(obj);   // refresh value labels / underline
            Log("menu: RT SHADOW CASTING on -> SHADOW CASTING off");
        } else if (*sc != g_lastSc && *sc != 2 && *rt > 0) {
            *rt = 0;
            Log("menu: SHADOW CASTING on -> RT SHADOW CASTING off");
        }
    }
    g_lastRt = *rt; g_lastSc = *sc;

    PageDef& g3 = *FindPage(3); PageDef& g2 = *FindPage(2);
    PageScroll(pool, g3, row, st0 == 7);
    PageScroll(pool, g2, row2, st0 == 7);

    // CINEMATIC BARS (DISPLAY SETTING): like the game's own display options, it applies as soon as it changes
    if ((*cb == 0) != g_cfg.bars) {
        g_cfg.bars = *cb == 0; SaveBars();
        Log("cinematic bars: %s", g_cfg.bars ? "on" : "off");
    }
    if ((*smc == 0) != (g_cfg.smoothCam != 0)) {
        g_cfg.smoothCam = *smc == 0; SaveSmoothCam();
        Log("smooth camera: %s", g_cfg.smoothCam ? "on" : "off");
    }

    Page0Frame(pool, obj, row0);
    // our rows (their templates already carry the scroll offset)
    // DRAW DISTANCE EXTRA: the game picks the value label from rects 0x59 + value; 3 would be rect 0x5C
    if (*(int*)(obj + 0x54 + 13 * 4) == 3) {
        *(const SpriteRect**)(Spr(pool, 103) + 0x48) = &kArt_EXTRA;
        *(const SpriteRect**)(Spr(pool, 104) + 0x48) = &kArt_EXTRA;
    }
    BuildRowSprites(pool, g3, row, g3.rows[1], &kModeTitle, kModeLabelYOff, &kModeValue[*mode], kModeLabelYOff, 1);
    BuildRowSprites(pool, g3, row, g3.rows[5], &kRtTitle, 1, &kRtValue[*rt], kRtValueYOff[*rt], 5);
    BuildRowSprites(pool, g3, row, g3.rows[6], &kAaTitle, 1, &kAaValue[*aa], kAaValueYOff[*aa], 6);
    BuildRowSprites(pool, g3, row, g3.rows[7], &kTfTitle, kArt_TEXTURE_FILTERING_yoff, &kTfValue[*tf], kTfValueYOff[*tf], 7);
    BuildRowSprites(pool, g3, row, g3.rows[8], &kAoTitle, kArt_AMBIENT_OCCLUSION_yoff, &kAoValue[*ao], 0, 8);
    BuildRowSprites(pool, g3, row, g3.rows[9], &kSsrTitle, kArt_REFLECTIONS_yoff, &kAoValue[*ssr], 0, 9);
    BuildRowSprites(pool, g3, row, g3.rows[10], &kUpTitle, kArt_TEXTURE_UPSCALING_yoff, &kUpValue[*up], 0, 10);
    BuildRowSprites(pool, g3, row, g3.rows[11], &kGeomTitle, kArt_IMPROVED_GEOMETRY_yoff, &kGeomValue[*geom], 0, 11);
    BuildRowSprites(pool, g3, row, g3.rows[12], &kHdrTitle, kArt_HDR_yoff, &kGeomValue[*hdr], 0, 12);
    BuildRowSprites(pool, g2, row2, g2.rows[4], &kCbTitle, 1, GameRect(0x0A), 0, 4);
    BuildOnOffRow(pool, g2.rows[4], 4, *cb, *(int*)(obj + 0x70));
    BuildRowSprites(pool, g2, row2, g2.rows[5], &kScTitle, kArt_SMOOTH_CAMERA_yoff, GameRect(0x0A), 0, 5);
    BuildOnOffRow(pool, g2.rows[5], 5, *smc, *(int*)(obj + 0x70));
    // the icon y argument moves the icons twice as far as sprite pixels (seen in game: -48 put them 2 rows up)
    { int y = 0xF4 - g2.scroll * kRowPitch / 2, x = 0x130;
      if (g_cfg.newMenu) {
          // the redesigned DISPLAY SETTING: the game's three flashing icons go to the right-hand panel, under the
          // description, while DOORWAY EMPHASIS (row 3) is selected, and off screen otherwise. The icons' own
          // coordinates are the menu's 512 x 448 space (x 1.25, y about 1.07 of ours); their y argument moves 2 px
          // per unit (0xF4 = the original row at 248). Panel icons: ours x ~437..570, top ~240.
          x = kDoorPanelX;
          y = (page == 2 && cur == 3) ? kDoorPanelY : 2000;
      }
      if (y != g_doorY) { WriteMem(kDoorIconY, &y, 4); g_doorY = y; }
      if (x != g_doorX) { WriteMem(kDoorIconX, &x, 4); g_doorX = x; } }
    // help line (sprite 118): our rect, centred like the game's own (0x776B30 mode 4: pivot = size / 2)
    const SpriteRect* help = row == 1 ? &kModeHelp : row == 5 ? &kRtHelp : row == 6 ? &kAaHelp : row == 7 ? &kTfHelp : row == 8 ? &kAoHelp : row == 9 ? &kSsrHelp : row == 10 ? &kUpHelp : row == 11 ? &kGeomHelp : row == 12 ? &kHdrHelp
                           : row2 == 4 ? &kCbHelp : row2 == 5 ? &kScHelp : (row0 == 1 && g_p0Count == 2) ? &kArt_help_kb : nullptr;
    if (help) {
        uint8_t* h = Spr(pool, 118);
        *(const SpriteRect**)(h + 0x48) = help;
        *(int*)(h + 0x40) = (help->w & ~1) << 3;
        *(int*)(h + 0x44) = (help->h & ~1) << 3;
    }

    PageFinish(pool, g3);
    PageFinish(pool, g2);

    // HDR CALIBRATION screen open: the menu keeps its background (title band art, parchment, help band) and the
    // renderer draws the screen's own text over it. Everything else goes off screen for this frame: all text (textures
    // 819 / 887 / ours), the cursor bar and its ornaments (texture 968, untextured 280 x 32, texture 610), key icons (1671),
    // underlines and scroll bars (small untextured quads). The full-width untextured bands and lines stay.
    const int mstate = MenuState(obj);
    // failsafe: a page slide that has not finished after 2 seconds is left alone (the menu stays usable)
    static int slideFrames = 0;
    slideFrames = (mstate == 5 || mstate == 6 || mstate == 8) ? slideFrames + 1 : 0;
    if (slideFrames == 120) Log("option menu: page slide (state %d) not finished after 120 frames; not hiding", mstate);
    {   static int lastState = -1, lastPage = -1;           // menu state changes, for the log
        if (mstate != lastState || page != lastPage) {
            Log("option menu: page %d state %d row %d (new screen %d, calibration %d)", page, mstate, cur, g_gsFlag ? 1 : 0, g_calOpenFlag ? 1 : 0);
            lastState = mstate; lastPage = page;
        } }
    const bool slide = mstate == 5 || mstate == 6 || mstate == 8;
    // leaving one of our redesigned pages: the game fades the old page's sprites out (sprite mode 3 at +0x58) while
    // the list slides back in, so the old layout showed for a moment. Their fade is finished at once instead: alpha 0
    // (+0x56, or +0x0E with flag bit 1), and the game's own fade step then stops drawing them as it normally does.
    { static bool fromPage = false;
      if (g_cfg.newMenu && mstate == 7 && page >= 0 && page <= 3) fromPage = true;
      else if (mstate == 4 || mstate < 2 || mstate > 8) fromPage = false;
      if (fromPage && (mstate == 8 || mstate == 2 || mstate == 3)) {
          for (int i = 0; i < SPR_COVER; i++) {
              if (i == 1) continue;
              uint8_t* sp = Spr(pool, i);
              if (*(int16_t*)(sp + 0x58) != 3) continue;
              *(int16_t*)(sp + 0x56) = 0; *(int16_t*)(sp + 0x0E) = 0;
          }
      } }
    int coverN = 0;
    if (g_calOpenFlag || (g_cfg.newMenu && page >= 0 && page <= 3 && ((slide && slideFrames < 120) || mstate == 7))) {
        // Our screens draw over the old menu instead of hiding it: the menu's background pieces (every sprite that is not
        // text, an icon, the cursor bar or a small plain quad: parchment, title band art, help band, full-width bands) are copied, in their order, into the
        // last pool slots, so the game draws them again after everything else and they cover its text and cursor bar.
        // The game's own sprites are not touched. Our text goes on top at present (d3d9_sod_calib.cpp).
        int n = 0;
        for (int i = 0; i < SPR_COVER && n < COVER_N; i++) {
            if (i == 1) continue;                           // the cursor bar
            const uint8_t* sp = Spr(pool, i);
            const SpriteRect* r = *(const SpriteRect* const*)(sp + 0x48);
            if (!(*(const uint32_t*)(sp + 0x50) & 1) || !r || r == &kEmptyRect || IsBadReadPtr(r, sizeof *r)) continue;
            // everything that is not text, icons, the cursor bar's parts or small plain quads (underlines, scroll bars)
            if (r->tex == 819 || r->tex == 887 || r->tex == 4999 || r->tex == 610 || r->tex == 1671 || r->tex == 968 || (r->tex == 0 && r->w < 480)) continue;
            memcpy(Spr(pool, SPR_COVER + n), sp, 0x60);
            n++;
        }
        coverN = n;
        if (g_cfg.newMenu && page == 2 && cur == 3 && mstate >= 5 && mstate <= 7 && n < COVER_N) {
            const int v = *(int*)(obj + 0x78);
            if (v >= 0 && v <= 2) {
                uint8_t* b = Spr(pool, SPR_COVER + n);
                memcpy(b, Spr(pool, 1), 0x60);
                *(const SpriteRect**)(b + 0x48) = &g_doorBarRect;
                *(int*)(b + 0x40) = 0; *(int*)(b + 0x44) = 0;
                const int bx = (4 * 512 / 640) << 4, by = ((91 + 40 * 3) * 448 / 480) << 4;
                for (int f : { 0x10, 0x20, 0x30 }) { *(int*)(b + f) = bx; *(int*)(b + f + 4) = by; }
                // no movement: sprite 1's own slide (mode at +0x58) would drag this copy around the screen
                *(int16_t*)(b + 0x58) = 0; *(int16_t*)(b + 0x5C) = 0;
                // colour: flag bit 1 makes the draw (0x79ED50) take R/G/B/A from +8/+0xA/+0xC/+0xE instead of the grey
                // level at +0x54. Same brown as the d3d9 overlay's bar (0xEB261A10); game alpha 0x80 = opaque (doubled
                // at 0x79FEA5), so 0xEB -> 0x76, scaled by the menu's current fade (sprite 1's alpha, +0x56).
                int fade = *(int16_t*)(Spr(pool, 1) + 0x56); if (fade < 0) fade = 0; if (fade > 0x80) fade = 0x80;
                *(int16_t*)(b + 0x08) = 0x26; *(int16_t*)(b + 0x0A) = 0x1A; *(int16_t*)(b + 0x0C) = 0x10;
                *(int16_t*)(b + 0x0E) = (int16_t)(0x76 * fade / 0x80);
                *(uint32_t*)(b + 0x50) = (*(uint32_t*)(b + 0x50) & ~0x64u) | 0x3u;   // draw, own colour; no 2x size, no alt path
                coverN = n + 1;
            }
        }
    }
    for (int i = coverN; i < COVER_N; i++) {               // unused cover slots draw nothing
        uint8_t* sp = Spr(pool, SPR_COVER + i);
        *(const SpriteRect**)(sp + 0x48) = &kEmptyRect;
        *(uint32_t*)(sp + 0x50) &= ~1u;
    }
}

// leaving GRAPHIC SETTING with confirm: runs right before the game applies the resolution
static void __cdecl cb_ApplySettings() {
    if (g_menuObj) {
        int rt = *(int*)(g_menuObj + 0x54 + kRtId * 4);
        if (rt >= 0 && rt <= 3 && rt != g_cfg.rtLevel) {
            Log("RT shadow casting: %d -> %d", g_cfg.rtLevel, rt);
            g_cfg.rtLevel = rt; SaveRt();
        }
        int aa = *(int*)(g_menuObj + 0x54 + kAaId * 4);
        if (aa >= 0 && aa <= 1 && aa != g_cfg.aaMode) {
            Log("anti-aliasing: %d -> %d", g_cfg.aaMode, aa);
            g_cfg.aaMode = aa; SaveAa();
        }
        int kb = *(int*)(g_menuObj + 0x54 + kKbId * 4);
        if (kb >= 0 && kb <= 1 && kb != g_cfg.kbLayout) {
            Log("keyboard layout: %s", kb ? "modern" : "classic");
            g_cfg.kbLayout = kb; SaveKb();
        }
        int ssr = *(int*)(g_menuObj + 0x54 + kSsrId * 4);
        if (ssr >= 0 && ssr <= 2 && ssr != g_cfg.ssrMode) {
            Log("reflections: %d -> %d", g_cfg.ssrMode, ssr);
            g_cfg.ssrMode = ssr; SaveSsr();
        }
        int up = *(int*)(g_menuObj + 0x54 + kUpId * 4);
        if (up >= 0 && up <= 3 && up != g_cfg.upMode) {
            Log("texture upscaling: %d -> %d", g_cfg.upMode, up);
            g_cfg.upMode = up; SaveUp();
        }
        int geom = *(int*)(g_menuObj + 0x54 + kGeomId * 4);
        if (geom >= 0 && geom <= 1 && geom != g_cfg.geomMode) {
            Log("improved geometry: %d -> %d", g_cfg.geomMode, geom);
            g_cfg.geomMode = geom; SaveGeom();
        }
        int fps = *(int*)(g_menuObj + 0x54 + kFpsId * 4);
        if (fps >= 0 && fps <= 1 && fps != g_cfg.fpsUnlock) {
            Log("uncapped frame rate: %s", fps ? "on" : "off");
            g_cfg.fpsUnlock = fps; SaveFps(); ApplyFps();
        }
        int hdrv = *(int*)(g_menuObj + 0x54 + kHdrId * 4);
        if (hdrv >= 0 && hdrv <= 1 && hdrv != g_cfg.hdr) {
            Log("hdr: %s", hdrv ? "on" : "off");
            g_cfg.hdr = hdrv; SaveHdr();
        }
        int ao = *(int*)(g_menuObj + 0x54 + kAoId * 4);
        if (ao >= 0 && ao <= 2 && ao != g_cfg.aoMode) {
            Log("ambient occlusion: %d -> %d", g_cfg.aoMode, ao);
            g_cfg.aoMode = ao; SaveAo();
        }
        int tf = *(int*)(g_menuObj + 0x54 + kTfId * 4);
        if (tf >= 0 && tf <= 2 && tf != g_cfg.tfMode) {
            Log("texture filtering: %d -> %d", g_cfg.tfMode, tf);
            g_cfg.tfMode = tf; SaveTf();
        }
    }
    int v = g_menuMode;
    g_menuMode = -1;
    int mode = (v < 0) ? g_mode : MenuToMode(v);
    if (mode == g_mode) return;
    Log("screen mode: %s -> %s", kModeName[g_mode], kModeName[mode]);
    g_mode = mode; SaveMode();
    g_topmost = -1;   // z-order re-evaluated on the next frame
}

// shadow-volume code's view of SHADOW CASTING (call sites 0x744BE8 pass gate, 0x748F04 light count).
// With RAY TRACING on, the menu shows SHADOW CASTING OFF but the game keeps running every shadow light
// pass (MULTIPLE): the SoD-RT DXVK layer takes its lights and casters from those passes.
static int __cdecl cb_ShadowSettingForRenderer() {
    if (g_cfg.rtLevel > 0) return 0;
    return *(uint8_t*)0xAB8FDC;
}

// DRAW DISTANCE EXTRA (0.13.2). The game keeps one far distance per area at 0x11051AC (setter 0x78C570, called on
// area load): it is the projection's far plane and, through 0x715FF0, the outdoor cell-culling radius (LOW 1/3,
// MIDDLE 2/3, HIGH all of it). EXTRA doubles the area's far distance itself, so both the far plane and the culled
// radius move out together; the game's fog (its own start/end per area) is unchanged.
static int g_baseFar = 0;                     // the area's own far distance, as the game set it
static const int kExtraFarScale = 2;
static void ApplyFar() {
    if (g_baseFar <= 0) return;
    int far_ = *(uint8_t*)0xAB8FDA == 3 ? g_baseFar * kExtraFarScale : g_baseFar;
    if (*(int*)0x11051AC != far_) { *(int*)0x11051AC = far_; *(uint8_t*)0x1105512 = 1; }
}
static void __cdecl cb_SetAreaFar(int v) { g_baseFar = v; *(uint8_t*)0x1105512 = 1; *(int*)0x11051AC = v; ApplyFar(); }
static void __cdecl cb_SetDrawDistance(int v) { *(uint8_t*)0xAB8FDA = (uint8_t)v; ApplyFar(); }
// Outdoor cell culling (0x790B20) normally uses a precomputed visibility set per player cell: a rectangle of cells
// plus a bitmask of the ones the designers marked visible from there. Cells outside it pop in and out as the player
// crosses cell borders (castle walls at the end of a street, 0.13.2 video). The game's own "no visibility set" path
// (byte 0x1103AA4 -> 0x791640) draws every cell within the far distance, through the view frustum test 0x791050.
// EXTRA takes that path.
static int __cdecl cb_FullCellCulling() { return *(uint8_t*)0x1103AA4 || *(uint8_t*)0xAB8FDA == 3; }
static int __cdecl cb_DrawDistanceRadius() {
    int far_ = *(int*)0x11051AC;
    switch (*(uint8_t*)0xAB8FDA) {
    case 0: return far_ / 3;
    case 1: return far_ * 2 / 3;
    case 2: case 3: return far_;              // EXTRA: the far distance is already scaled
    default: return 0;
    }
}

// exported for the SoD-RT DXVK layer
__declspec(dllexport) int __cdecl SoDFix_GetRayTracingLevel() { return g_cfg.rtLevel; }
__declspec(dllexport) int __cdecl SoDFix_GetAntiAliasing() { return g_cfg.aaMode; }
__declspec(dllexport) int __cdecl SoDFix_GetTextureFiltering() { return g_cfg.tfMode; }
__declspec(dllexport) int __cdecl SoDFix_GetAmbientOcclusion() { return g_cfg.aoMode; }
// HDR: on / off; nits of paper white (which = 0) and of the brightest lights (which = 1)
__declspec(dllexport) int __cdecl SoDFix_GetHdr() { return g_cfg.hdr; }
// HDR CALIBRATION screen (0.22.0): opened with confirm on the HDR row while it says ON. SoDFix owns the state and the
// input (keyboard and controller, which reach hk_KeyboardState as the game's keys); the renderer (d3d9.dll) draws
// the screen over the menu and shows the sliders' effect live. Leaving it saves the values to SoDFix.ini.
namespace Cal {
    struct Item { int* v; int lo, hi, step, def; const char* key; };
    static Item items[5] = {
        { &g_cfg.hdrPaper,     60,  300,  5,  100, "HdrPaperWhite" },
        { &g_cfg.hdrPeak,      400, 2000, 50, 1000, "HdrPeakBrightness" },
        { &g_cfg.hdrHighlight, 0,   200,  5,  60,  "HdrHighlight" },
        { &g_cfg.hdrGamma,     180, 240,  5,  210, "HdrGamma" },
        { &g_cfg.hdrSat,       50,  150,  5,  100, "HdrSaturation" },
    };
    enum { ROW_DEFAULT = 5, ROW_EXIT = 6, ROWS = 7 };
    // Mouse. The screen's layout in menu units (640 x 480 over the game picture; as drawn by d3d9_sod_calib.cpp):
    // row r at y 98 + 40 r, 39 high (the two buttons 31), x 304..634; slider track x 314..622.
    // While the screen is open the game's own mouse reading is blanked (hk_MouseRead), so nothing underneath reacts.
    static bool mDown = false; static int mDrag = -1; static POINT mLast = { -1, -1 };
    static bool MousePos(float& x, float& y) {
        HWND hw = *(HWND*)A::MainHwnd; POINT p;
        if (!hw || !GetCursorPos(&p) || !ScreenToClient(hw, &p)) return false;
        const float w = float(g_lastDst.right - g_lastDst.left), h = float(g_lastDst.bottom - g_lastDst.top);
        if (w <= 0 || h <= 0) return false;
        x = (p.x - g_lastDst.left) * 640.0f / w; y = (p.y - g_lastDst.top) * 480.0f / h;
        const bool moved = p.x != mLast.x || p.y != mLast.y; mLast = p;
        return moved;
    }
    static int RowAt(float x, float y) {
        if (x < 304 || x > 634) return -1;
        for (int r = 0; r < ROWS; r++) { float y0 = 95.0f + 40.0f * r; if (y >= y0 && y < y0 + (r < 5 ? 39.0f : 31.0f)) return r; }
        return -1;
    }
    static bool open = false, swallow = false, menuActive = false;
    static int row = 0, hold = 0;
    static bool waitLR = false;                       // opened with RIGHT: no adjusting until it is released
    static bool pUp, pDown, pEnter, pBack;
    static void Save() {
        char v[16];
        for (auto& it : items) { snprintf(v, sizeof v, "%d", *it.v); WritePrivateProfileStringA("Display", it.key, v, g_ini); }
        Log("hdr calibration saved: white %d nits, peak %d nits, highlights %d%%, gamma %d.%02d, saturation %d%%",
            g_cfg.hdrPaper, g_cfg.hdrPeak, g_cfg.hdrHighlight, g_cfg.hdrGamma / 100, g_cfg.hdrGamma % 100, g_cfg.hdrSat);
    }
    static void Adjust(int dir) {
        if (row >= 5) return;
        Item& it = items[row];
        int v = ((*it.v + (dir > 0 ? it.step : -1)) / it.step) * it.step;      // to the next step up / down
        if (dir > 0 && v <= *it.v) v += it.step;
        *it.v = v < it.lo ? it.lo : v > it.hi ? it.hi : v;
    }
    // once per frame with the game's key array (keyboard and controller merged); true: the game gets no keys
    static void Frame(uint8_t* k) {
        menuActive = g_calMenuSeen > 0; if (g_calMenuSeen > 0) g_calMenuSeen--;
        if (menuActive && MenuState(g_menuObj) != 7) menuActive = false;   // only inside a page, not the list or a slide
        const bool up = k[0xC8] & 0x80, down = k[0xD0] & 0x80, left = k[0xCB] & 0x80, right = k[0xCD] & 0x80;
        const bool enter = k[0x1C] & 0x80, back = (k[0x01] & 0x80) || (k[0x0E] & 0x80);
        if (swallow) {                                   // the key that opened / closed the screen, until released
            if (enter || back) { k[0x1C] = k[0x01] = k[0x0E] = 0; } else swallow = false;
        }
        if (!open) {
            const bool onRow = menuActive && g_menuObj && *(int*)(g_menuObj + 0x30) == 3 && *(int*)(g_menuObj + 0x38) == kHdrRow;
            const bool byEnter = onRow && enter && !pEnter && !swallow && *(int*)(g_menuObj + 0x54 + kHdrId * 4) == 1;
            if (enter && !pEnter && !byEnter && menuActive && g_menuObj)
                Log("hdr calibration: confirm at page %d row %d hdr %d (not opened)", *(int*)(g_menuObj + 0x30),
                    *(int*)(g_menuObj + 0x38), *(int*)(g_menuObj + 0x54 + kHdrId * 4));
            const bool byRight = false;
            if (byEnter || byRight) {
                if (!g_cfg.hdr) { g_cfg.hdr = 1; SaveHdr(); Log("hdr: on (calibration)"); }
                if (g_cfg.hdrPaper == 0) g_cfg.hdrPaper = 100;
                open = true; row = 0; hold = 0; waitLR = byRight; swallow = true;
                mDown = true; mDrag = -1;                // a held mouse button does nothing until released

                Log("hdr calibration opened");
                memset(k, 0, 256);
            }
        } else {
            if (!menuActive) { open = false; Save(); }   // the menu went away underneath
            else {
                if (up && !pUp) row = (row + ROWS - 1) % ROWS;
                if (down && !pDown) row = (row + 1) % ROWS;
                // mouse: pointing selects a row, the button drags a slider or presses DEFAULT / EXIT
                { float mx = 0, my = 0; const bool moved = MousePos(mx, my);
                  const bool btn = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                  const int at = RowAt(mx, my);
                  if (moved && at >= 0 && mDrag < 0) row = at;
                  if (btn && !mDown && at >= 0) {
                      row = at;
                      if (at < 5) mDrag = at;
                      else if (at == ROW_DEFAULT) for (auto& it : items) *it.v = it.def;
                      else { open = false; swallow = true; Save();
                             if (g_menuObj && *(int*)(g_menuObj + 0x30) == 3) *(int*)(g_menuObj + 0x38) = kHdrRow; }
                  }
                  if (!btn) mDrag = -1;
                  if (mDrag >= 0) {
                      Item& it = items[mDrag];
                      float t = (mx - 314.0f) / (622.0f - 314.0f); t = t < 0 ? 0 : t > 1 ? 1 : t;
                      *it.v = it.lo + int(t * (it.hi - it.lo) / it.step + 0.5f) * it.step;
                  }
                  mDown = btn; }
                if (!left && !right) waitLR = false;
                if ((left || right) && !waitLR) {        // held: repeats after a third of a second
                    if (hold == 0 || (hold > 20 && hold % 3 == 0)) Adjust(right ? 1 : -1);
                    hold++;
                } else hold = 0;
                if (enter && !pEnter && row == ROW_DEFAULT) for (auto& it : items) *it.v = it.def;
                if ((enter && !pEnter && row == ROW_EXIT) || (back && !pBack)) {
                    open = false; swallow = true; Save();
                    if (g_menuObj && *(int*)(g_menuObj + 0x30) == 3) *(int*)(g_menuObj + 0x38) = kHdrRow;   // back on the HDR row
                }
                memset(k, 0, 256);
            }
        }
        pUp = up; pDown = down; pEnter = enter; pBack = back;
        g_calOpenFlag = open;
    }
}
// SOUND SETTING volumes (0.24.0): the game has 8 levels (0..7, menu ids 2..4), sent as sound commands and turned into
// a gain level / 7 by 0x7AA8A0 (id 2, gain 0xF47A6C), 0x7AA850 (id 3, gain 0xF47A78, applied at once through 0x7A8750)
// and 0x7AA880 (id 4, gain 0xF47A70). Those three are replaced (hk_Vol*) so a volume can be any percentage, set by
// dragging its slider with the mouse ([Sound] Volume2..4). The game keeps the nearest level; when it changes the
// level itself (left / right) the percentage follows it.
namespace Vol {
    static int pct[3] = { -1, -1, -1 };
    static bool loaded = false;
    static const char* kKeys[3] = { "Volume2", "Volume3", "Volume4" };
    static void Load() {
        if (loaded) return; loaded = true;
        for (int i = 0; i < 3; i++) { int v = GetPrivateProfileIntA("Sound", kKeys[i], -1, g_ini); pct[i] = v < 0 ? -1 : v > 100 ? 100 : v; }
    }
    static void Save() {
        char b[16];
        for (int i = 0; i < 3; i++) if (pct[i] >= 0) { snprintf(b, sizeof b, "%d", pct[i]); WritePrivateProfileStringA("Sound", kKeys[i], b, g_ini); }
    }
    static int LevelOf(int p) { return (p * 7 + 50) / 100; }
    static float Gain(int i, int lvl) {
        Load();
        lvl = lvl < 0 ? 0 : lvl > 7 ? 7 : lvl;
        if (pct[i] < 0 || LevelOf(pct[i]) != lvl) pct[i] = (lvl * 100 + 3) / 7;
        return pct[i] / 100.0f;
    }
    static int Pct(int i, int lvl) { Gain(i, lvl); return pct[i]; }
}
extern "C" void __cdecl hk_Vol3(int lvl) {
    *(float*)0xF47A78 = Vol::Gain(1, lvl) + 1e-4f;
    ((char (__cdecl*)(int, float))0x7A8750)(0, *(float*)0xAB02E8);
}
extern "C" void __cdecl hk_Vol4(int lvl) { *(float*)0xF47A70 = Vol::Gain(2, lvl); }
extern "C" void __cdecl hk_Vol2(int lvl) { *(float*)0xF47A6C = Vol::Gain(0, lvl) + 1e-4f; }
namespace Vol {
    // a slider dragged to p %: the menu keeps the nearest level, the sound follows at once
    static void Set(int i, int p) {
        p = p < 0 ? 0 : p > 100 ? 100 : p;
        if (p == pct[i]) return;
        pct[i] = p;
        const int lvl = LevelOf(p);
        if (g_menuObj) *(int*)(g_menuObj + 0x54 + (2 + i) * 4) = lvl;
        if (i == 0) hk_Vol2(lvl); else if (i == 1) hk_Vol3(lvl); else hk_Vol4(lvl);
    }
}
// REMAP screen (0.24.0): opened with confirm on the CONTROLLER page's REMAP row. Keyboard or controller, whichever
// was used last. Drawn by the renderer like the option pages (screen 2, page 4 keyboard / 5 controller).
namespace Rm {
    enum { ROW_DEFAULT = Bind::N, ROW_EXIT = Bind::N + 1, ROWS = Bind::N + 2 };
    static bool open = false, capture = false, armed = false, waitUp = false, mDown = true;
    static int row = 0, mode = 0;                         // mode 0 keyboard, 1 controller
    static DWORD capT = 0;
    static bool pUp, pDown, pEnter, pBack;
    static POINT mLast = { -1, -1 };
    static char g_txt[64];
    static const char* KeyName(uint8_t sc) {
        char n[48] = "";
        if (!GetKeyNameTextA(LONG((sc & 0x7F) << 16) | ((sc & 0x80) ? (1L << 24) : 0), n, sizeof n) || !n[0])
            snprintf(n, sizeof n, "KEY %d", sc);
        size_t j = 0; for (const char* q = n; *q && j + 1 < sizeof g_txt; q++) { unsigned char c = (unsigned char)*q;
            g_txt[j++] = (c >= 32 && c < 127) ? (char)toupper(c) : '?'; }
        g_txt[j] = 0; return g_txt;
    }
    static const char* PadName(int b, int style) {
        static const char* x[Bind::PAD_N] = { "A", "B", "X", "Y", "VIEW", "GUIDE", "MENU", "LS", "RS", "LB", "RB",
            "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT", "D-PAD RIGHT", "LT", "RT" };
        static const char* ps[Bind::PAD_N] = { "CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "CREATE", "PS", "OPTIONS", "L3", "R3",
            "L1", "R1", "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT", "D-PAD RIGHT", "L2", "R2" };
        static const char* nn[Bind::PAD_N] = { "B", "A", "Y", "X", "-", "HOME", "+", "L STICK", "R STICK", "L", "R",
            "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT", "D-PAD RIGHT", "ZL", "ZR" };
        if (b < 0 || b >= Bind::PAD_N) return "?";
        return style == Pad::STYLE_PLAYSTATION ? ps[b] : style == Pad::STYLE_NINTENDO ? nn[b] : x[b];
    }
    static int Style() { return Pad::g_style == Pad::STYLE_KEYBOARD ? Pad::STYLE_XBOX : Pad::g_style; }
    static const char* Value(int r) {
        if (r < 0 || r >= Bind::N) return "";
        if (capture && r == row) return mode ? "PRESS A BUTTON" : "PRESS A KEY";
        if (mode == 0) return KeyName(Bind::key[r]);
        return PadName(Bind::PadOf(r, Pad::g_style == Pad::STYLE_NINTENDO), Style());
    }
    static void Open() {
        Bind::Load();
        open = true; row = 0; capture = false; waitUp = true; mDown = true;
        mode = Pad::g_style == Pad::STYLE_KEYBOARD ? 0 : 1;
        Log("remap opened (%s)", mode ? "controller" : "keyboard");
    }
    static void Close() {
        open = false; capture = false; waitUp = true; Bind::Save();
        if (g_menuObj && *(int*)(g_menuObj + 0x30) == 0) *(int*)(g_menuObj + 0x38) = RemapRow(g_menuObj);
    }
    // action a takes input v; an action that had v gets a's old one (no input does two things)
    static void SetKey(int a, uint8_t v) {
        const uint8_t old = Bind::key[a];
        for (int b = 0; b < Bind::N; b++) if (b != a && Bind::key[b] == v) Bind::key[b] = old;
        Bind::key[a] = v;
        Log("remap: %s -> key %d", Bind::kActions[a].label, v);
    }
    static void SetPad(int a, int v) {
        const bool nin = Pad::g_style == Pad::STYLE_NINTENDO;
        const int old = Bind::PadOf(a, nin);
        int eff[Bind::N]; for (int b = 0; b < Bind::N; b++) eff[b] = Bind::PadOf(b, nin);
        for (int b = 0; b < Bind::N; b++) if (b != a && eff[b] == v) Bind::pad[b] = (int8_t)old;
        Bind::pad[a] = (int8_t)v;
        Log("remap: %s -> pad input %d", Bind::kActions[a].label, v);
    }
    static void Defaults() {
        for (int a = 0; a < Bind::N; a++) { if (mode == 0) Bind::key[a] = Bind::kActions[a].key; else Bind::pad[a] = -1; }
        Log("remap: %s defaults", mode ? "controller" : "keyboard");
    }
    static void StartCapture() { capture = true; armed = false; capT = GetTickCount(); }
    // once per frame: raw = the keys as pressed (before any mapping), k = the game's keys (keyboard and controller)
    static void Frame(uint8_t* k, const uint8_t* raw) {
        if (!open) {
            g_rmOpenFlag = false;
            if (!RemapOn() || !Cal::menuActive || Cal::open || !g_menuObj || *(int*)(g_menuObj + 0x30) != 0) { pEnter = (k[0x1C] & 0x80) != 0; return; }
            const bool enter = (k[0x1C] & 0x80) != 0;
            if (enter && !pEnter && *(int*)(g_menuObj + 0x38) == RemapRow(g_menuObj)) { Open(); memset(k, 0, 256); }
            pEnter = enter;
            if (!open) return;
        }
        g_rmOpenFlag = true;
        if (!Cal::menuActive) { Close(); g_rmOpenFlag = false; return; }
        bool anyKey = false; for (int i = 0; i < 256; i++) if (raw[i] & 0x80) { anyKey = true; break; }
        const uint32_t padNow = Pad::g_raw;
        const bool up = k[0xC8] & 0x80, down = k[0xD0] & 0x80, enter = k[0x1C] & 0x80, back = (k[0x01] & 0x80) || (k[0x0E] & 0x80);
        memset(k, 0, 256);
        // mouse: rows at y 91 + 19 r (menu units), x 6..334
        float mx = -1, my = -1; bool moved = false; const bool lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        { HWND hw = *(HWND*)A::MainHwnd; POINT p;
          const float w = float(g_lastDst.right - g_lastDst.left), h = float(g_lastDst.bottom - g_lastDst.top);
          if (hw && w > 0 && h > 0 && GetCursorPos(&p) && ScreenToClient(hw, &p)) {
              mx = (p.x - g_lastDst.left) * 640.0f / w; my = (p.y - g_lastDst.top) * 480.0f / h;
              moved = p.x != mLast.x || p.y != mLast.y; mLast = p; } }
        const int at = (mx >= 6 && mx <= 334 && my >= 91 && my < 91 + 19.0f * ROWS) ? int((my - 91) / 19.0f) : -1;
        const bool click = lb && !mDown; mDown = lb;
        if (waitUp) {                                     // the key / button that got us here, until released
            if (!anyKey && !padNow && !lb) waitUp = false;
            pUp = up; pDown = down; pEnter = enter; pBack = back; return;
        }
        if (capture) {
            if (!armed) { if (!anyKey && !padNow) armed = true; }
            else if (click || GetTickCount() - capT > 8000) capture = false;
            else if (mode == 0) {
                for (int i = 1; i < 256; i++) if (raw[i] & 0x80) {
                    if (i == 0x01 && row != Bind::N - 1) capture = false;          // Esc cancels (except for PAUSE)
                    else { SetKey(row, (uint8_t)i); capture = false; }
                    waitUp = true; break;
                }
            } else {
                if (raw[0x01] & 0x80) { capture = false; waitUp = true; }
                else for (int b = 0; b < Bind::PAD_N; b++) if (b != 5 && (padNow & (1u << b))) {
                    SetPad(row, b); capture = false; waitUp = true; break;
                }
            }
            pUp = up; pDown = down; pEnter = enter; pBack = back; return;
        }
        if (at >= 0 && moved) row = at;
        if (up && !pUp) row = (row + ROWS - 1) % ROWS;
        if (down && !pDown) row = (row + 1) % ROWS;
        int act = -1;
        if (enter && !pEnter) act = row;
        if (click && at >= 0) { row = at; act = at; }
        if (act >= 0) {
            if (act < Bind::N) StartCapture();
            else if (act == ROW_DEFAULT) Defaults();
            else Close();
        } else if (back && !pBack) Close();
        pUp = up; pDown = down; pEnter = enter; pBack = back;
        g_rmOpenFlag = open;
    }
    static const char* kDescDefault = "Put every action back on its original key or button.";
    static const char* kDescExit = "Save and return to CONTROLLER.";
}
// Redesigned GRAPHIC SETTING screen (0.23.0, [Display] NewGraphicsMenu=1). The game's menu keeps running underneath
// exactly as before: it owns the cursor (menu object +0x38) and the values (+0x54 + id * 4), takes the keyboard and
// controller, applies and saves. Only the look changes: the menu's text is moved off screen (cb_MenuFrame) and the
// renderer draws every row at once with a description of the selected one, from the texts below. The mouse is
// handled here (the game's own mouse rows no longer match): pointing selects, left / right click changes the value,
// clicking EXIT confirms.
namespace Gs {
    static bool active = false, mDown = true;
    static int inject = 0;
    static POINT mLast = { -1, -1 };
    static const char* kLabels[14] = { "RESOLUTION", "SCREEN MODE", "DRAW DISTANCE", "FOCUS, BLUR AND FADES", "SHADOW CASTING",
        "RAY TRACED SHADOWS", "ANTI-ALIASING", "TEXTURE FILTERING", "AMBIENT OCCLUSION", "REFLECTIONS", "TEXTURE UPSCALING",
        "IMPROVED GEOMETRY", "HDR", "EXIT" };
    static const char* kDesc[14] = {
        "The size the game is drawn at, always in its original 4:3 shape. Higher is sharper. 2880 x 2160 fills a 4K screen from top to bottom.",
        "BORDERLESS lets other windows come to the front when you switch away. FULLSCREEN keeps the game on top.",
        "How far away buildings and streets are drawn. EXTRA shows the whole town with nothing appearing as you walk.",
        "The game's own soft-focus, blur and fade effects in scenes and transitions.",
        "The game's original shadows under characters. Turned off automatically while RAY TRACED SHADOWS is on.",
        "Real shadows traced from the sun and the lamps: characters, buildings, trees and flags all cast them. Needs a graphics card with ray tracing. Higher levels give softer, more exact edges.",
        "Smooths the jagged, stair-stepped edges of buildings and characters.",
        "How textures look at a distance and at shallow angles. ANISOTROPIC keeps streets and walls sharp far into the distance.",
        "Soft shading where surfaces meet: corners, doorways, under ledges, where feet touch the ground. Adds depth to rooms and streets.",
        "Polished floors and wet streets mirror the scene around them.",
        "Redraws the game's textures at 2, 3 or 4 times their detail, so walls, signs and clothes stay sharp close up.",
        "Removes the visible seams and repeating tiles in the streets.",
        "Brighter lamps and glows on HDR displays, with the rest of the picture as the game drew it. Windows HDR must be on. While ON, press Enter here to calibrate.",
        "Apply the settings and return.",
    };
    // ---- the other option pages (0.23.0): CONTROLLER 0, SOUND 1, DISPLAY 2
    static const char* kLabels0[4] = { "TYPE", "LAYOUT", "REMAP", "EXIT" };
    static const char* kDesc0[5] = {
        "KEYBOARD or JOYPAD: which controls the game uses, and which buttons its prompts show.",
        "CLASSIC keeps the original keys. MODERN adds W A S D to move, E or Space to act, Q to cancel, Tab for the items, M for the map, Z and C to turn the camera and V to change the view. The original keys keep working.",
        "Choose your own key or button for every action. Shows KEYBOARD or CONTROLLER, whichever you are using: press Enter to change its controls.",
        "Apply the settings and return.",
        "The controls you are using. Changes by itself when you use the keyboard or a controller, and the button prompts follow." };
    static const char* kLabels1[4] = { "BGM VOLUME", "SE VOLUME", "EVENT VOLUME", "EXIT" };
    static const char* kDesc1[4] = {
        "Loudness of the music.",
        "Loudness of sound effects: footsteps, doors and the world around you.",
        "Loudness of the scenes: voices and their sounds.",
        "Apply the settings and return." };
    static const char* kLabels2[7] = { "LANGUAGE", "COMPASS", "ITEM EMPHASIS", "DOORWAY EMPHASIS", "CINEMATIC BARS", "SMOOTH CAMERA", "EXIT" };
    static const char* kDesc2[7] = {
        "The language of the text on screen and the subtitles.",
        "Shows the compass while you explore the town.",
        "Makes the things you can pick up or look at stand out.",
        "Marks the doors you can open. Choose one of the three marker styles, or OFF.",
        "Black bars at the top and bottom of the screen during scenes.",
        "Steadier camera while walking.",
        "Apply the settings and return." };
    // CONTROLLER page row -> 0 TYPE, 1 LAYOUT, 2 REMAP, 3 EXIT
    static int P0Kind(int r) {
        const bool kb = g_menuObj && *(int*)(g_menuObj + 0x54) == 0, rm = RemapOn();
        static const int a[4] = { 0, 1, 2, 3 }, b[3] = { 0, 1, 3 }, c[3] = { 0, 2, 3 }, d[2] = { 0, 3 };
        const int* t = kb ? (rm ? a : b) : (rm ? c : d); const int n = (kb ? 3 : 2) + (rm ? 1 : 0);
        return r >= 0 && r < n ? t[r] : 3;
    }
    static int Page() { return g_menuObj ? *(int*)(g_menuObj + 0x30) : -1; }
    // rows of the open page, EXIT included
    static int Rows(int page) {
        if (page == 3) return 14;
        if (page == 2) return 7;
        if (page == 1) return 4;
        if (page == 0) return ((g_menuObj && *(int*)(g_menuObj + 0x54) == 0) ? 3 : 2) + (RemapOn() ? 1 : 0);   // LAYOUT only for the keyboard
        return 0;
    }
    static const RangeItem* Item(int page, int r) {        // nullptr for EXIT
        if (r < 0 || r >= Rows(page) - 1) return nullptr;
        if (page == 3) return &g_page3Items[r];
        if (page == 2) return &g_page2Items[r];
        if (page == 1) return (const RangeItem*)0x939420 + r;
        if (page == 0) return &g_page0Items[r];
        return nullptr;
    }
    // layout in menu units, shared with d3d9_sod_calib.cpp: rows from y 91, 19 apart with 15 rows, else 30
    static float Pitch(int page) { return Rows(page) > 10 ? 19.0f : page == 2 ? 40.0f : 30.0f; }   // DISPLAY: room for the icon
    static float LeftW(int page) { return page == 2 ? 362.0f : 336.0f; }
    // DOORWAY EMPHASIS: the game draws its three icons itself (0x716C10, after the menu sprites); they go to x 194,
    // 234, 274 of our DOORWAY row, OFF is drawn by us after them
    static const int kDoorX = 194;
    static char g_val[32];
    static const char* Pick(int v, int n, const char* const* names) { return v >= 0 && v < n ? names[v] : "?"; }
    static const char* Value(int r);
    static const char* ValueOther(int page, int r) {
        const RangeItem* it = Item(page, r);
        if (!it) return "";
        const int v = *(int*)(g_menuObj + 0x54 + it->id * 4);
        static const char* onOff[] = { "ON", "OFF" }, *type[] = { "KEYBOARD", "JOYPAD" }, *lay[] = { "CLASSIC", "MODERN" },
            *lang[] = { "JAPANESE", "ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH" };
        if (page == 0) {
            const int kd = P0Kind(r);
            const char* dev = Pad::g_style == Pad::STYLE_KEYBOARD ? "KEYBOARD" : "CONTROLLER";
            if (kd == 0) return TypeAuto() ? dev : Pick(v, 2, type);
            if (kd == 2) return dev;
            return Pick(v, 2, lay);
        }
        if (page == 1) { snprintf(g_val, sizeof g_val, "%d", Vol::Pct(r, v)); return g_val; }
        if (r == 0) return Pick(v, 6, lang);
        if (r == 3) return v == 3 ? "OFF" : "";              // the styles: a still icon drawn by the game
        return Pick(v, 2, onOff);
    }
    static const char* Value(int r) {
        if (Page() != 3) return ValueOther(Page(), r);
        if (!g_menuObj || r < 0 || r > 12) return "";
        const int v = *(int*)(g_menuObj + 0x54 + g_page3Items[r].id * 4);
        static const char* onOff[] = { "ON", "OFF" }, *offOn[] = { "OFF", "ON" }, *mode[] = { "BORDERLESS", "FULLSCREEN" },
            *draw[] = { "LOW", "MEDIUM", "HIGH", "EXTRA" }, *shadow[] = { "MULTIPLE", "SINGLE", "OFF" },
            *rt[] = { "OFF", "MEDIUM", "HIGH", "ULTRA" }, *aa[] = { "OFF", "SMAA" }, *tf[] = { "STANDARD", "TRILINEAR", "ANISOTROPIC" },
            *lowHigh[] = { "OFF", "LOW", "HIGH" }, *up[] = { "OFF", "2X", "3X", "4X" };
        switch (r) {
            case 0: if (v >= 0 && v < kResCount) { snprintf(g_val, sizeof g_val, "%u x %u", g_res[v].w, g_res[v].h); return g_val; } return "?";
            case 1: return Pick(v, 2, mode);
            case 2: return Pick(v, 4, draw);
            case 3: return Pick(v, 2, onOff);
            case 4: return Pick(v, 3, shadow);
            case 5: return Pick(v, 4, rt);
            case 6: return Pick(v, 2, aa);
            case 7: return Pick(v, 3, tf);
            case 8: case 9: return Pick(v, 3, lowHigh);
            case 10: return Pick(v, 4, up);
            default: return Pick(v, 2, offOn);
        }
    }
    // once per frame with the game's key array, after the calibration screen had its turn
    static void Frame(uint8_t* k) {
        const int page = Page();
        active = g_cfg.newMenu && Cal::menuActive && !Cal::open && !Rm::open && g_menuObj && page >= 0 && page <= 3
              && MenuState(g_menuObj) == 7;
        g_gsFlag = active;
        if (!active) { mDown = true; inject = 0; return; }
        if (inject) { k[0x1C] |= 0x80; inject = 0; }
        // mouse: rows at y 91 + pitch r (menu units), x 6..left column
        HWND hw = *(HWND*)A::MainHwnd; POINT p;
        const float w = float(g_lastDst.right - g_lastDst.left), h = float(g_lastDst.bottom - g_lastDst.top);
        if (!hw || w <= 0 || h <= 0 || !GetCursorPos(&p) || !ScreenToClient(hw, &p)) return;
        const float mx = (p.x - g_lastDst.left) * 640.0f / w, my = (p.y - g_lastDst.top) * 480.0f / h;
        const bool moved = p.x != mLast.x || p.y != mLast.y; mLast = p;
        const int rows = Rows(page); const float pitch = Pitch(page);
        const int at = (mx >= 6 && mx <= LeftW(page) - 2 && my >= 91 && my < 91 + pitch * rows) ? int((my - 91) / pitch) : -1;
        const bool lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, rb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        // SOUND: a slider follows the mouse while the button is held (from x 160 of its row to the end of the track)
        if (page == 1) {
            static int drag = -1;
            if (!lb) { if (drag >= 0) { Vol::Save(); Log("sound: volumes %d / %d / %d %%", Vol::pct[0], Vol::pct[1], Vol::pct[2]); } drag = -1; }
            else if (!mDown && at >= 0 && at < 3 && mx >= 160) drag = at;
            if (drag >= 0) {
                *(int*)(g_menuObj + 0x38) = drag;
                const float x0 = 170.0f, x1 = LeftW(1) - 40.0f;
                float t = (mx - x0) / (x1 - x0); t = t < 0 ? 0 : t > 1 ? 1 : t;
                Vol::Set(drag, int(t * 100.0f + 0.5f));
                mDown = lb || rb; return;
            }
            if (at >= 0 && at < 3) {                      // clicks on a volume's label only select it
                if (moved || ((lb || rb) && !mDown)) *(int*)(g_menuObj + 0x38) = at;
                mDown = lb || rb; return;
            }
        }
        if (at >= 0 && (moved || ((lb || rb) && !mDown))) *(int*)(g_menuObj + 0x38) = at;
        if ((lb || rb) && !mDown && at >= 0) {
            const RangeItem* it = Item(page, at);
            if (!it) inject = 1;                          // EXIT: the game's own confirm
            else if (page == 0 && P0Kind(at) == 2) { Rm::Open(); mDown = true; return; }
            else {
                int* v = (int*)(g_menuObj + 0x54 + it->id * 4);
                const int n = it->max - it->min + 1;
                *v = it->min + ((*v - it->min) + (lb ? 1 : n - 1)) % n;
            }
        }
        mDown = lb || rb;
    }
}
// The game reads the mouse once per frame (0x7AE310, called from 0x7AF417 and 0x7AE512), after the keyboard: cursor
// position 0xF62568 / 0xF62688 (menu units), movement 0xF62680 / 0xF63790, buttons 0xF63B98 / 0xF63B9C, wheel
// 0xF63BAC / 0xF63BA0. The option menu uses them for hover and clicks on its own layout. While one of our screens is up
// they are blanked (position off screen), so only our own mouse handling acts.
extern "C" void __cdecl hk_MouseRead() {
    ((void (__cdecl*)())0x7AE310)();
    if (!(Cal::open || Gs::active || Rm::open)) return;
    *(int*)0xF62568 = -1000; *(int*)0xF62688 = -1000;
    *(int*)0xF62680 = 0; *(int*)0xF63790 = 0;
    *(int*)0xF63B98 = 0; *(int*)0xF63B9C = 0;
    *(int*)0xF63BAC = 0; *(int*)0xF63BA0 = 0;
}
// DOORWAY EMPHASIS icons (0x716C10, called @0x71694F after the menu sprites are drawn). With the redesigned DISPLAY
// SETTING the chosen style is also drawn still (phase 0) as the row's value, by the game's own icon routine
// 0x716C70 (obj, style, selected, x, y, phase, alpha).
extern "C" void __cdecl hk_DoorIcons(uint8_t* obj) {
    ((void (__cdecl*)(uint8_t*))0x716C10)(obj);
    if (!g_cfg.newMenu || !obj || *(int*)(obj + 0x30) != 2) return;
    const int st = MenuState(obj);
    if (st < 5 || st > 7) return;
    const int v = *(int*)(obj + 0x78);                     // DOORWAY EMPHASIS: 0..2 the styles, 3 OFF
    if (v < 0 || v > 2) return;
    const int alpha = *(int16_t*)(*(uint8_t**)(obj + 0x28) + 0x1F76);
    ((void (__cdecl*)(uint8_t*, int, int, int, int, int, int))0x716C70)(obj, v, 1, kDoorStillX, kDoorStillY, 0, alpha);
}
// which: 0 the screen the renderer draws over the option menu (0 none, 1 HDR CALIBRATION, 2 GRAPHIC SETTING),
// 1 its selected row, 2 its number of rows
__declspec(dllexport) int __cdecl SoDFix_GetMenuScreen(int which) {
    // GRAPHIC SETTING is drawn from the moment the page starts to open (states 5-7), so it is there at once
    const int st = MenuState(g_menuObj);
    const int pg = Gs::Page();
    const bool gsShow = g_cfg.newMenu && g_calMenuSeen > 0 && g_menuObj && pg >= 0 && pg <= 3 && st >= 5 && st <= 8;   // 8: until the list is back
    if (Rm::open) {                                               // REMAP: an option page of its own (4 / 5)
        if (which == 0) return 2; if (which == 1) return Rm::row; if (which == 2) return Rm::ROWS;
        if (which == 3) return 4 + Rm::mode; return 0;
    }
    const int id = Cal::open ? 1 : gsShow ? 2 : 0;
    if (which == 0) return id;
    if (which == 1) return id == 1 ? Cal::row : (id == 2 && g_menuObj) ? *(int*)(g_menuObj + 0x38) : 0;
    if (which == 2) return id == 1 ? Cal::ROWS : id == 2 ? Gs::Rows(pg) : 0;
    if (which == 3) return id == 2 ? pg : -1;                     // the option page (0 CONTROLLER .. 3 GRAPHIC)
    if (which == 4 && id == 2 && pg == 1) {                       // SOUND: row's value as a slider: value
        const RangeItem* it = Gs::Item(1, *(int*)(g_menuObj + 0x38)); (void)it; }
    return 0;
}
// GRAPHIC SETTING texts: kind 0 row label, 1 current value, 2 description
__declspec(dllexport) const char* __cdecl SoDFix_GetMenuText(int kind, int row) {
    if (kind == 3) {                                              // the footer's two lines (empty: the renderer's own)
        if (!Rm::open) return "";
        if (Rm::capture) return row == 0 ? (Rm::mode ? "Press the new button for this action" : "Press the new key for this action")
                                         : "Esc : cancel      it swaps with the action that had it";
        return row == 0 ? "UP and DOWN : select action      confirm : change it"
                        : "back : save and return      mouse : click a row to change it";
    }
    if (Rm::open) {
        if (row < 0 || row >= Rm::ROWS) return "";
        if (row < Bind::N) return kind == 0 ? Bind::kActions[row].label : kind == 1 ? Rm::Value(row) : Bind::kActions[row].desc;
        if (kind == 1) return "";
        return row == Rm::ROW_DEFAULT ? (kind == 0 ? "DEFAULT" : Rm::kDescDefault) : (kind == 0 ? "EXIT" : Rm::kDescExit);
    }
    const int pg = Gs::Page();
    if (row < 0 || row >= Gs::Rows(pg)) return "";
    if (kind == 1) return Gs::Value(row);
    if (pg == 0) { const int r = Gs::P0Kind(row);
                   if (kind == 2 && r == 0 && TypeAuto()) return Gs::kDesc0[4];
                   return kind == 0 ? Gs::kLabels0[r] : Gs::kDesc0[r]; }
    if (pg == 1) return kind == 0 ? Gs::kLabels1[row] : Gs::kDesc1[row];
    if (pg == 2) return kind == 0 ? Gs::kLabels2[row] : Gs::kDesc2[row];
    return kind == 0 ? Gs::kLabels[row] : Gs::kDesc[row];
}
// per row: which 0 kind (0 text value, 1 slider, 2 DOORWAY EMPHASIS icons, 3 EXIT), 1 value, 2 lowest, 3 highest
__declspec(dllexport) int __cdecl SoDFix_GetMenuItem(int row, int which) {
    if (Rm::open) return which == 0 ? (row == Rm::ROW_EXIT ? 3 : 0) : 0;
    const int pg = Gs::Page();
    const RangeItem* it = Gs::Item(pg, row);
    if (!it) return which == 0 ? 3 : 0;
    if (which == 0) return pg == 1 ? 1 : (pg == 2 && row == 3) ? 2 : 0;
    if (pg == 1 && row < 3) { const int v = *(int*)(g_menuObj + 0x54 + it->id * 4);   // volumes in %
        return which == 1 ? Vol::Pct(row, v) : which == 2 ? 0 : 100; }
    if (which == 1) return *(int*)(g_menuObj + 0x54 + it->id * 4);
    return which == 2 ? it->min : it->max;
}
// which: 0 calibration screen open, 1 selected row (0..4 sliders, 5 DEFAULT, 6 EXIT), 2 the option menu is showing
__declspec(dllexport) int __cdecl SoDFix_GetHdrCalib(int which) {
    return which == 0 ? (Cal::open ? 1 : 0) : which == 1 ? Cal::row : which == 2 ? (Cal::menuActive ? 1 : 0) : 0;
}
// which: 0 paper white nits (0 = Windows' SDR content brightness), 1 peak nits, 2 HdrDebug, 3 highlight %,
// 4 gamma x 100, 5 saturation %
__declspec(dllexport) int __cdecl SoDFix_GetHdrNits(int which) {
    switch (which) {
        case 0: return g_cfg.hdrPaper;
        case 1: return g_cfg.hdrPeak;
        case 2: return g_cfg.hdrDebug;
        case 3: return g_cfg.hdrHighlight;
        case 4: return g_cfg.hdrGamma;
        case 5: return g_cfg.hdrSat;
        default: return 0;
    }
}
__declspec(dllexport) int __cdecl SoDFix_GetReflections() { return g_cfg.ssrMode; }
__declspec(dllexport) int __cdecl SoDFix_GetTextureUpscaling() { return g_cfg.upMode; }
__declspec(dllexport) int __cdecl SoDFix_GetImprovedGeometry() { return g_cfg.geomMode; }
// the game's texture table: 40-byte entries indexed -16384..16383 (IDirect3DTexture8* at +0x10). Room draws refer to
// pages (index > 0) and cut sub-textures (< 0) by these indices (sod/ssr/NOTES.md); 0 = unsupported exe
__declspec(dllexport) uintptr_t __cdecl SoDFix_GetTextureTable() { return g_patchesActive ? *(uintptr_t*)0xAB3998 : 0; }
__declspec(dllexport) int __cdecl SoDFix_GetLampLighting() { return g_cfg.lamps && g_cfg.rtLevel > 0 ? 1 : 0; }
__declspec(dllexport) int __cdecl SoDFix_GetCinematicBars() { return g_cfg.bars ? 1 : 0; }
__declspec(dllexport) int __cdecl SoDFix_GetSmoothCamera() { return g_cfg.smoothCam ? 1 : 0; }
__declspec(dllexport) uint32_t __cdecl SoDFix_GetPresentSite() { return g_presentSite; }

} // extern "C"

// ---------------------------------------------------------------- MODERN keyboard layout
// The game polls the keyboard once per frame: IDirectInputDevice8::GetDeviceState(256, 0xF63D28) at 0x7AE1AA, and
// every key test reads that array. With LAYOUT MODERN (and TYPE KEYBOARD) the new keys are folded into the keys the
// game knows; the original keys keep working.
struct KeyMap { uint8_t from, to; };
static const KeyMap kModernKeys[] = {
    { 0x11, 0xC8 }, { 0x1E, 0xCB }, { 0x1F, 0xD0 }, { 0x20, 0xCD },   // W A S D      -> arrows (move, menus)
    { 0x12, 0x1C }, { 0x39, 0x1C },                                   // E, Space     -> Enter (talk/inspect/select)
    { 0x10, 0x0E },                                                   // Q            -> Backspace (cancel/skip)
    { 0x0F, 0x9D },                                                   // Tab          -> Right Ctrl (inventory)
    { 0x32, 0x36 },                                                   // M            -> Right Shift (map)
    { 0x2C, 0xD3 }, { 0x2E, 0xD1 },                                   // Z / C        -> Delete / Page Down (camera)
    { 0x2F, 0xCF },                                                   // V            -> End (change view)
};
// PROFILER: the game's file opens and reads are timed (the renderer's hitch report shows them by name)
namespace Prof {
    static volatile LONG64 openTicks = 0, readTicks = 0, readBytes = 0, opens = 0, reads = 0, padTicks = 0;
    struct Opened { int64_t qpc; char name[88]; };
    static Opened ring[64]; static volatile LONG ringPos = 0;
    static int64_t Now() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
    static void NoteOpen(const char* p, int64_t t0) {
        const int64_t t1 = Now();
        InterlockedExchangeAdd64(&openTicks, t1 - t0); InterlockedIncrement64(&opens);
        if (!p) return;
        const char* b = p; for (const char* q = p; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
        LONG i = InterlockedIncrement(&ringPos) & 63;
        ring[i].qpc = t1; strncpy_s(ring[i].name, b, _TRUNCATE);
    }
}

typedef HRESULT (WINAPI* PFN_GetState)(void*, DWORD, void*);
extern "C" HRESULT WINAPI hk_KeyboardState(void* dev, DWORD size, void* buf) {
    MenuSpritesRestore();                               // our screens' hidden menu sprites back before the menu logic
    HRESULT hr = ((PFN_GetState)((*(void***)dev)[9]))(dev, size, buf);
    // alt-tabbed away: the keyboard device keeps reporting keys typed into other programs (0.12.1)
    HWND hw = *(HWND*)A::MainHwnd;
    if (hr >= 0 && hw && GetForegroundWindow() != hw) { memset(buf, 0, size); Pad::Poll(nullptr, false); return hr; }
    if (hr < 0 || size != 256) { if (hr < 0) memset(buf, 0, size); Pad::Poll(size == 256 ? (uint8_t*)buf : nullptr, true); return hr; }
    uint8_t* k = (uint8_t*)buf;
    // a real key pressed: the prompts show the keyboard again
    for (int i = 0; i < 256; i++) if (k[i] & 0x80) { Pad::SetStyle(Pad::STYLE_KEYBOARD, "keyboard used"); break; }
    { static bool f11 = false; const bool down = (k[0x57] & 0x80) != 0;   // F11: HDR test mode (HdrDebug 0 -> 1 -> 2)
      if (down && !f11) {
          g_cfg.hdrDebug = (g_cfg.hdrDebug + 1) % 3;
          static const char* names[3] = { "normal", "DXVK conversion only", "grey test" };
          Log("hdr test mode %d: %s (F11)", g_cfg.hdrDebug, names[g_cfg.hdrDebug]);
      }
      f11 = down; k[0x57] = 0; }
    { static bool f7 = false; const bool down = (k[0x41] & 0x80) != 0;   // F7: LAMP LIGHTING on / off (comparison; F10 is the camera recorder)
      if (down && !f7) { g_cfg.lamps = !g_cfg.lamps; Log("lamp lighting: %s (F7)", g_cfg.lamps ? "on" : "off"); }
      f7 = down; k[0x41] = 0; }
    uint8_t raw[256]; memcpy(raw, k, 256);              // the keys as pressed (REMAP screen)
    if (!Rm::open) Bind::ApplyKeys(k, raw);             // REMAP: the game's keys from the keys chosen for them
    if (g_cfg.kbLayout == 1 && *(uint8_t*)kCtrlType == 0) {
        for (auto& m : kModernKeys) if (!Bind::KeyBound(m.from)) k[m.from] = 0;
        for (auto& m : kModernKeys) if (!Bind::KeyBound(m.from)) k[m.to] |= raw[m.from];
    }
    { LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
      Pad::Poll(k, true);                               // controllers press the game's own keys
      QueryPerformanceCounter(&q1); InterlockedExchangeAdd64(&Prof::padTicks, q1.QuadPart - q0.QuadPart); }
    Cal::Frame(k);                                      // HDR CALIBRATION screen: takes the keys while it is open
    Rm::Frame(k, raw);                                  // REMAP screen: likewise
    Gs::Frame(k);                                       // redesigned GRAPHIC SETTING screen: mouse
    return hr;
}

// Crash catcher: a crash anywhere in the process logs the exception and where it happened (module + offset, and
// the call chain's return addresses found on the stack) to SoDFix.log, and writes a minidump to
// %LOCALAPPDATA%\SoDRT\crash_<time>.dmp for debugging.
static void LogAddr(const char* what, uintptr_t a) {
    HMODULE m = nullptr; char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m)
        GetModuleFileNameA(m, path, sizeof path);
    const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
    Log("  %s %08X  %s+0x%X", what, (unsigned)a, base, (unsigned)(a - (uintptr_t)m));
}
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    Log("CRASH: exception %08X at %08X (thread %lu)", (unsigned)er->ExceptionCode, (unsigned)(uintptr_t)er->ExceptionAddress, GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        Log("  %s address %08X", er->ExceptionInformation[0] == 1 ? "writing" : er->ExceptionInformation[0] == 8 ? "executing" : "reading",
            (unsigned)er->ExceptionInformation[1]);
    LogAddr("at", (uintptr_t)er->ExceptionAddress);
    const CONTEXT* c = ep->ContextRecord;
    Log("  eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X ebp %08X esp %08X",
        (unsigned)c->Eax, (unsigned)c->Ebx, (unsigned)c->Ecx, (unsigned)c->Edx, (unsigned)c->Esi, (unsigned)c->Edi, (unsigned)c->Ebp, (unsigned)c->Esp);
    // code addresses on the stack (return addresses of the callers, roughly)
    MEMORY_BASIC_INFORMATION mbi;
    const uintptr_t* sp = (const uintptr_t*)c->Esp;
    int found = 0;
    for (int i = 0; i < 2048 && found < 16; i++) {
        if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
        uintptr_t v = sp[i];
        if (v < 0x10000 || !VirtualQuery((void*)v, &mbi, sizeof mbi)) continue;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) { LogAddr("stack", v); found++; }
    }
    char dir[MAX_PATH] = { };
    if (GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof dir)) {
        SYSTEMTIME t; GetLocalTime(&t);
        char file[MAX_PATH];
        snprintf(file, sizeof file, "%s\\SoDRT\\crash_%04d%02d%02d_%02d%02d%02d.dmp", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        typedef BOOL (WINAPI* PFN_Dump)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
        HMODULE dh = LoadLibraryA("dbghelp.dll");
        PFN_Dump dump = dh ? (PFN_Dump)GetProcAddress(dh, "MiniDumpWriteDump") : nullptr;
        HANDLE f = CreateFileA(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dump && f != INVALID_HANDLE_VALUE) {
            struct { DWORD tid; EXCEPTION_POINTERS* ep; BOOL client; } mei = { GetCurrentThreadId(), ep, FALSE };
            // MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo
            BOOL ok = dump(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1 | 0x4 | 0x1000, &mei, nullptr, nullptr);
            Log("  minidump %s: %s", ok ? "written" : "failed", file);
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void* p_BeforeCreateDevice = (void*)cb_BeforeCreateDevice;
static void* p_MenuInit           = (void*)cb_MenuInit;
static void* p_MenuFrame          = (void*)cb_MenuFrame;
static void* p_ApplySettings      = (void*)cb_ApplySettings;

// ---------------------------------------------------------------- videos (DirectShow player @0x7A8250)
// Intro/attract videos (open_e.mpg) play in a DirectShow video window parented to the game window and
// sized to its whole client area via IVideoWindow::SetWindowPosition(0,0,w,h) @0x7A8324. The game window
// now covers the monitor, so the video window is placed as a centred 4:3 rect instead.
typedef HRESULT (WINAPI* PFN_VWSetPos)(void*, long, long, long, long);
extern "C" HRESULT WINAPI hk_VideoSetWindowPosition(void* vw, long left, long top, long w, long h) {
    auto real = (PFN_VWSetPos)((*(void***)vw)[39]);
    long gw = h * 4 / 3, gh = h;
    if (gw > w) { gw = w; gh = w * 3 / 4; }
    long l = left + (w - gw) / 2, t = top + (h - gh) / 2;
    Log("video window: client %ldx%ld -> %ldx%ld at %ld,%ld", w, h, gw, gh, l, t);
    return real(vw, l, t, gw, gh);
}

// ---------------------------------------------------------------- file redirection (atlas)
typedef HANDLE (WINAPI* PFN_CFA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI* PFN_CFW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static PFN_CFA o_CreateFileA = nullptr;
static PFN_CFW o_CreateFileW = nullptr;
static char  g_atlasPath[MAX_PATH];

static bool EndsWithAtlas(const char* p) {
    static const char* tail = "conv\\misc\\option_e.tbn2";
    size_t n = strlen(p), t = strlen(tail);
    if (n < t) return false;
    for (size_t i = 0; i < t; ++i) {
        char a = p[n - t + i];
        if (a == '/') a = '\\';
        if (a >= 'A' && a <= 'Z') a += 32;
        if (a != tail[i]) return false;
    }
    return true;
}
typedef BOOL (WINAPI* PFN_RF)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static PFN_RF o_ReadFile = nullptr;
static BOOL WINAPI hk_ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    const int64_t t0 = Prof::Now();
    BOOL r = o_ReadFile(h, b, n, got, ov);
    InterlockedExchangeAdd64(&Prof::readTicks, Prof::Now() - t0); InterlockedIncrement64(&Prof::reads);
    if (r && got) InterlockedExchangeAdd64(&Prof::readBytes, *got);
    return r;
}
static HANDLE WINAPI hk_CreateFileA(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    if (g_haveAtlas && p && EndsWithAtlas(p) && !(a & GENERIC_WRITE)) { Log("redirect %s -> %s", p, g_atlasPath); p = g_atlasPath; }
    const int64_t t0 = Prof::Now();
    HANDLE r = o_CreateFileA(p, a, s, sa, c, f, t);
    Prof::NoteOpen(p, t0);
    return r;
}
static HANDLE WINAPI hk_CreateFileW(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    char n[MAX_PATH * 2] = { };
    if (p) WideCharToMultiByte(CP_ACP, 0, p, -1, n, sizeof n, nullptr, nullptr);
    if (g_haveAtlas && p && !(a & GENERIC_WRITE) && EndsWithAtlas(n)) {
        Log("redirect (W) %s -> %s", n, g_atlasPath);
        const int64_t t0 = Prof::Now(); HANDLE r = o_CreateFileA(g_atlasPath, a, s, sa, c, f, t); Prof::NoteOpen(g_atlasPath, t0); return r;
    }
    const int64_t t0 = Prof::Now();
    HANDLE r = o_CreateFileW(p, a, s, sa, c, f, t);
    Prof::NoteOpen(n, t0);
    return r;
}
// counters for the renderer's profiler: QPC ticks / counts since start
extern "C" __declspec(dllexport) int __cdecl SoDFix_GetProfileCounters(int64_t* out, int n) {
    const int64_t v[6] = { Prof::openTicks, Prof::opens, Prof::readTicks, Prof::reads, Prof::readBytes, Prof::padTicks };
    for (int i = 0; i < n && i < 6; i++) out[i] = v[i];
    return 6;
}
// names of the files opened after QPC time 'since' (newest last), joined with ", "
extern "C" __declspec(dllexport) int __cdecl SoDFix_GetOpenedFiles(int64_t since, char* out, int size) {
    if (!out || size <= 0) return 0;
    out[0] = 0; int len = 0, count = 0;
    const LONG end = Prof::ringPos;
    for (LONG k = end - 63; k <= end; k++) {
        if (k < 0) continue;
        const Prof::Opened& o = Prof::ring[k & 63];
        if (o.qpc <= since || !o.name[0]) continue;
        count++;
        int w = snprintf(out + len, size - len, "%s%s", len ? ", " : "", o.name);
        if (w < 0 || len + w >= size) break;
        len += w;
    }
    return count;
}
static void HookIAT(const char* name, uintptr_t slot, void* hook, void** orig) {
    *orig = *(void**)slot;
    Add(name, slot, U32((uint32_t)(uintptr_t)*orig), U32((uint32_t)(uintptr_t)hook));
}

// ---------------------------------------------------------------- exe check / DPI
static bool IsSupportedExe() {
    auto base = (uint8_t*)GetModuleHandleA(nullptr);
    if ((uintptr_t)base != 0x400000) { Log("exe base %p != 0x400000", base); return false; }
    auto nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    Log("exe TimeDateStamp=%08X", (unsigned)nt->FileHeader.TimeDateStamp);
    if (nt->FileHeader.TimeDateStamp != 0x3DB6B7C3) { Log("unsupported shadow.exe build - no patches applied"); return false; }
    return true;
}
static void MakeDpiAware() {
    HMODULE u = GetModuleHandleA("user32.dll");
    typedef BOOL (WINAPI* PFN_ctx)(HANDLE);
    if (auto f = (PFN_ctx)GetProcAddress(u, "SetProcessDpiAwarenessContext"))
        if (f((HANDLE)-4)) { Log("DPI awareness: per-monitor v2"); return; }
    typedef BOOL (WINAPI* PFN_aw)();
    if (auto f = (PFN_aw)GetProcAddress(u, "SetProcessDPIAware")) { f(); Log("DPI awareness: system"); }
}

// ---------------------------------------------------------------- build patch set
static void BuildPatches() {
    snprintf(g_atlasPath, sizeof g_atlasPath, "%s\\SoDFix\\option_e.tbn2", g_dir);
    g_haveAtlas = GetFileAttributesA(g_atlasPath) != INVALID_FILE_ATTRIBUTES;
    Log("atlas %s: %s", g_atlasPath, g_haveAtlas ? "found" : "MISSING (menu additions disabled)");
    QueryMonitor();
    Log("monitor %dx%d at %d,%d; screen mode %s", g_monW, g_monH, g_monX, g_monY, kModeName[g_mode]);

    // --- 1. extended resolution list -----------------------------------------------------
    if (g_cfg.resMenu && g_haveAtlas) {
        uint32_t tbl = (uint32_t)(uintptr_t)g_res;
        Add("res table w (apply)", 0x715F8A, U32(A::ResTable),     U32(tbl));
        Add("res table h (apply)", 0x715F91, U32(A::ResTable + 4), U32(tbl + 4));
        Add("res menu max",        A::ResRangeMax, U16(3), U16(kResCount - 1));
        Add("res menu wrap",       0x7170F8, { 0x03 }, { (uint8_t)(kResCount - 1) });
        memcpy(g_resLabels, (void*)(A::SpriteTable + 0x55 * 12), 4 * sizeof(SpriteRect));
        memcpy(g_resLabels + 4, kNewResLabels, sizeof kNewResLabels);
        Add("res label sprites", 0x7161AD, U32(A::SpriteTable), U32((uint32_t)(uintptr_t)g_resLabels - 0x55 * 12));
    }

    // --- 2. display: windowed device in a monitor-sized window, 4:3 placed at Present --------
    {
        Asm s(NextStub()); s.callAbs(&p_BeforeCreateDevice).jmp(A::CreateDevWindow); uintptr_t st = Emit(s);
        Add("CreateDevice: windowed", 0x7B2477, Rel32(0x7B2477, A::CreateDevWindow), Rel32(0x7B2477, st));
        // (0.4.0-0.8.3 forced BackBufferCount 2 to keep DXVK off its GDI path for dest-rect presents. 3 rotating
        //  buffers showed stale frames whenever the game presented without redrawing the back buffer - the
        //  pre-cutscene flicker. The SoD-RT d3d9.dll uses the Vulkan blit with 1 back buffer, like D3D8 windowed.)
        // mov edx,[eax]; push 0; push eax; call [edx+3Ch]  ->  push 0; push eax; call hk_Present
        for (uintptr_t site : { (uintptr_t)0x7B29DA, (uintptr_t)0x7B31CD }) {
            std::vector<uint8_t> r = { 0x6A, 0x00, 0x50, 0xE8 }; auto rel = Rel32(site + 4, (uintptr_t)hk_Present);
            r.insert(r.end(), rel.begin(), rel.end());
            Add("Present -> 4:3 dest rect", site, { 0x8B, 0x10, 0x6A, 0x00, 0x50, 0xFF, 0x52, 0x3C }, r);
        }
        Add("res validate -> SoDFix", 0x715FB0, { 0x8B, 0x44, 0x24, 0x04, 0x8B },
            [] { std::vector<uint8_t> v = { 0xE9 }; auto r = Rel32(0x715FB1, (uintptr_t)cb_ValidateRes); v.insert(v.end(), r.begin(), r.end()); return v; }());
        HookIAT("IAT CreateWindowExA", A::IAT_CreateWindowExA, (void*)hk_CreateWindowExA, (void**)&o_CreateWindowExA);
        HookIAT("IAT SetWindowPos",    A::IAT_SetWindowPos,    (void*)hk_SetWindowPos,    (void**)&o_SetWindowPos);
    }

    // --- 2b. videos: 4:3 DirectShow video window ---------------------------------------------
    {
        std::vector<uint8_t> r = { 0xE8 }; auto rel = Rel32(0x7A8325, (uintptr_t)hk_VideoSetWindowPosition);
        r.insert(r.end(), rel.begin(), rel.end()); r.push_back(0x90);
        Add("video window 4:3", 0x7A8324, { 0xFF, 0x91, 0x9C, 0x00, 0x00, 0x00 }, r);
    }

    // --- 3. GRAPHIC SETTING: SCREEN MODE, RT SHADOW CASTING and ANTI-ALIASING rows, scrolling ----------------------------
    if (g_cfg.modeMenu && g_haveAtlas) {
        int16_t resMax = (int16_t)((g_cfg.resMenu ? kResCount : 4) - 1);
        g_page3Items[0] = { 0, resMax, 12 };
        g_page3Items[1] = { 0, 1, (int16_t)kModeId };
        g_page3Items[2] = { 0, 3, 13 };                     // DRAW DISTANCE: LOW, MIDDLE, HIGH, EXTRA (0.13.2)
        g_page3Items[3] = { 0, 1, 14 };
        g_page3Items[4] = { 0, 2, 15 };
        g_page3Items[5] = { 0, 3, (int16_t)kRtId };
        g_page3Items[6] = { 0, 1, (int16_t)kAaId };
        g_page3Items[7] = { 0, 2, (int16_t)kTfId };
        g_page3Items[8] = { 0, 2, (int16_t)kAoId };
        g_page3Items[9] = { 0, 2, (int16_t)kSsrId };
        g_page3Items[10] = { 0, 3, (int16_t)kUpId };
        g_page3Items[11] = { 0, 1, (int16_t)kGeomId };
        g_page3Items[12] = { 0, 1, (int16_t)kHdrId };
        Add("page3 item count 4->13", 0x716FC1, U32(4), U32(13));
        Add("page3 item table",       0x716FC6, U32(0x939408), U32((uint32_t)(uintptr_t)g_page3Items));
        Add("page3 row sprites",      A::Page3Rows, U32(0x939AF0), U32((uint32_t)(uintptr_t)g_page3Rows));
        Add("page3 help rows",        A::Page3Help, U32(0x939AAC), U32((uint32_t)(uintptr_t)g_page3Help));
        // DISPLAY SETTING (page 2): CINEMATIC BARS + SMOOTH CAMERA after DOOR EMPHASIS, EXIT 312 -> 392 (layout entry 45)
        memcpy(g_page2Items, (const void*)0x939434, 4 * sizeof(RangeItem));
        g_page2Items[4] = { 0, 1, (int16_t)kCbId };
        g_page2Items[5] = { 0, 1, (int16_t)kScId };
        Add("page2 item count 4->6",  0x716FCD, U32(4), U32(6));
        Add("page2 item table",       0x716FD2, U32(0x939434), U32((uint32_t)(uintptr_t)g_page2Items));
        Add("page2 row sprites",      A::Page2Rows, U32(0x939AE4), U32((uint32_t)(uintptr_t)g_page2Rows));
        Add("page2 help rows",        A::Page2Help, U32(0x939AA0), U32((uint32_t)(uintptr_t)g_page2Help));
        Add("door icons y (runtime)",  kDoorIconY, U32(0xF4), U32(0xF4));
        Add("page2 EXIT row",         A::Layout + 45 * 8 + 6, U16(312), U16(392));
        Add("menu object 0x94->0xC4", 0x716631, U32(0x94), U32(kMenuObjSize));
        // sprite pool 120 -> 151 (push imm8 can't hold 130: call a stub that pushes the count)
        { Asm s(NextStub()); s.raw({ 0x58, 0x68 }).u32(POOL_NEW).raw({ 0x50 }).jmp(0x776AB0); uintptr_t st = Emit(s);
          std::vector<uint8_t> r = { 0xE8 }; auto rel = Rel32(0x71675F, st); r.insert(r.end(), rel.begin(), rel.end()); r.push_back(0x90); r.push_back(0x90);
          Add("sprite pool alloc 120->181", 0x71675E, { 0x6A, 0x78, 0xE8, 0x4B, 0x03, 0x06, 0x00 }, r); }
        { Asm s(NextStub());
          s.raw({ 0x58, 0x68 }).u32(POOL_NEW).raw({ 0x50, 0x60, 0x56 }).callAbs(&p_MenuFrame).raw({ 0x83, 0xC4, 0x04, 0x61 }).jmp(A::SpriteDraw);
          uintptr_t st = Emit(s);
          std::vector<uint8_t> r = { 0xE8 }; auto rel = Rel32(0x716931, st); r.insert(r.end(), rel.begin(), rel.end()); r.push_back(0x90); r.push_back(0x90);
          Add("sprite pool draw 120->181 + frame hook", 0x716930, { 0x6A, 0x78, 0xE8, 0x19, 0x03, 0x06, 0x00 }, r); }
        // rows: RES 104, SCREEN MODE 152, DRAW 200, FOCUS 248, SHADOW 296, RT SHADOW CASTING 344, ANTI-ALIASING 392,
        // TEXTURE FILTERING 440, AMBIENT OCCLUSION 488, REFLECTIONS 536, TEXTURE UPSCALING 584, IMPROVED GEOMETRY 632, UNCAPPED FRAME RATE 680, HDR 728, EXIT 776 (5 visible, scrolled into view)
        struct { int entry; int16_t from, to; } mv[] = {
            {48,152,200},{49,200,248},{50,248,296},{51,312,728},
            {53,152,200},{54,200,248},{55,200,248},{56,248,296},{57,248,296},{58,248,296} };
        for (auto& m : mv) Add("layout row shift", A::Layout + m.entry * 8 + 6, U16((uint16_t)m.from), U16((uint16_t)m.to));
        { Asm s(NextStub());
          s.raw({ 0xFF, 0x74, 0x24, 0x04 }).call(A::MenuInit).raw({ 0x83, 0xC4, 0x04 })
           .raw({ 0xFF, 0x74, 0x24, 0x04 }).callAbs(&p_MenuInit).raw({ 0x83, 0xC4, 0x04, 0xC3 });
          uintptr_t st = Emit(s);
          Add("menu init hook", 0x71663E, Rel32(0x71663E, A::MenuInit), Rel32(0x71663E, st)); }
        { Asm s(NextStub()); s.callAbs(&p_ApplySettings).jmp(A::ApplyRes); uintptr_t st = Emit(s);
          Add("menu apply hook", 0x7165BA, Rel32(0x7165BA, A::ApplyRes), Rel32(0x7165BA, st)); }
    }

    // --- 3a. SOUND SETTING: volumes as percentages (Vol)
    {
        auto jmp = [](uintptr_t at, void* to) { std::vector<uint8_t> r = { 0xE9 }; auto rel = Rel32(at + 1, (uintptr_t)to); r.insert(r.end(), rel.begin(), rel.end()); return r; };
        Add("volume id 3 -> SoDFix", 0x7AA850, { 0xDB, 0x44, 0x24, 0x04, 0xA1 }, jmp(0x7AA850, (void*)hk_Vol3));
        Add("volume id 4 -> SoDFix", 0x7AA880, { 0xDB, 0x44, 0x24, 0x04, 0xD8 }, jmp(0x7AA880, (void*)hk_Vol4));
        Add("volume id 2 -> SoDFix", 0x7AA8A0, { 0xDB, 0x44, 0x24, 0x04, 0xD8 }, jmp(0x7AA8A0, (void*)hk_Vol2));
    }

    // --- 3a. CONTROLLER SETTING: LAYOUT row; MODERN keyboard layout ---------------------------------------
    if (g_cfg.modeMenu && g_haveAtlas) {
        memcpy(g_page0Items, (const void*)0x939400, sizeof(RangeItem));
        g_page0Items[1] = { 0, 1, (int16_t)kKbId };
        g_page0Type = g_page0Items[0]; g_page0Layout = g_page0Items[1];
        Add("page0 item table",        0x716FBA, U32(0x939400), U32((uint32_t)(uintptr_t)g_page0Items));
        Add("page0 item count (runtime)", kPage0Count, U32(1), U32(1));   // written per frame: 2 with KEYBOARD
        Add("page0 row sprites (runtime)", kPage0RowsPtr, U32(kPage0RowsOrig), U32(kPage0RowsOrig));
        Add("page0 help rows (runtime)",   kPage0HelpPtr, U32(kPage0HelpOrig), U32(kPage0HelpOrig));
    }
    {   // mov eax,[edx]; push buf; push 100h; push edx; call [eax+24h]  ->  push buf; push 100h; push edx; call hook
        std::vector<uint8_t> r = { 0x68, 0x28, 0x3D, 0xF6, 0x00, 0x68, 0x00, 0x01, 0x00, 0x00, 0x52, 0xE8 };
        auto rel = Rel32(0x7AE1A9, (uintptr_t)hk_KeyboardState); r.insert(r.end(), rel.begin(), rel.end());
        Add("keyboard state -> layout", 0x7AE19D,
            { 0x8B, 0x02, 0x68, 0x28, 0x3D, 0xF6, 0x00, 0x68, 0x00, 0x01, 0x00, 0x00, 0x52, 0xFF, 0x50, 0x24 }, r);
    }

    // --- 3a. UNCAPPED FRAME RATE (experimental): the limiter's wait, switched at runtime by ApplyFps()
    Add("frame limiter (runtime)", 0x7B2FD5, { 0x75, 0x34 }, { 0x75, 0x34 });

    // --- 3a. modern controllers: the game's own DirectInput pad read (0x7AE1C0) returns "no pad" when SDL3 is
    // there, so a pad is never read twice; the game then keeps the controller type at KEYBOARD, and the pads press
    // the game's keys (hk_KeyboardState)
    {
        char p1[MAX_PATH], p2[MAX_PATH];
        snprintf(p1, sizeof p1, "%s\\SoDFix\\SDL3.dll", g_dir); snprintf(p2, sizeof p2, "%s\\SDL3.dll", g_dir);
        const bool sdl = GetFileAttributesA(p1) != INVALID_FILE_ATTRIBUTES || GetFileAttributesA(p2) != INVALID_FILE_ATTRIBUTES;
        if (g_cfg.gamepad && sdl)
            Add("DirectInput pad read off (SDL3)", 0x7AE1C0, { 0x51, 0x53, 0x56, 0x8B, 0x74, 0x24, 0x10 },
                { 0x33, 0xC0, 0xC3, 0x90, 0x90, 0x90, 0x90 });
        else Log("controller: %s, the game's own pad support is kept", g_cfg.gamepad ? "SDL3.dll not found" : "Gamepad=0");
    }

    // --- 3a. DRAW DISTANCE EXTRA: far distance setter, draw distance setter and cull radius getter -> SoDFix
    {
        auto jmpTo = [](uintptr_t at, uintptr_t fn) { std::vector<uint8_t> v = { 0xE9 }; auto r = Rel32(at + 1, fn); v.insert(v.end(), r.begin(), r.end()); return v; };
        Add("area far distance -> SoDFix", 0x78C570, { 0x8B, 0x44, 0x24, 0x04, 0xC6 }, jmpTo(0x78C570, (uintptr_t)cb_SetAreaFar));
        Add("draw distance set -> SoDFix", 0x715FD0, { 0x8A, 0x44, 0x24, 0x04, 0xA2 }, jmpTo(0x715FD0, (uintptr_t)cb_SetDrawDistance));
        Add("draw distance radius -> SoDFix", 0x715FF0, { 0x33, 0xC0, 0xA0, 0xDA, 0x8F }, jmpTo(0x715FF0, (uintptr_t)cb_DrawDistanceRadius));
        // mov al,[0x1103AA4] -> call stub (al = full cell culling); ecx and edx are live at this point
        Asm st(NextStub());
        st.raw({ 0x51, 0x52 }).call((uintptr_t)cb_FullCellCulling).raw({ 0x5A, 0x59, 0xC3 });   // push ecx; push edx; call; pop edx; pop ecx; ret
        uintptr_t stub = Emit(st);
        std::vector<uint8_t> r = { 0xE8 }; auto rel = Rel32(0x790CA8, stub); r.insert(r.end(), rel.begin(), rel.end());
        Add("outdoor visibility set -> SoDFix", 0x790CA7, { 0xA0, 0xA4, 0x3A, 0x10, 0x01 }, r);
    }

    // --- 3c. alt-tab: the game blocks it three ways at window creation (0x7B26FE..0x7B273E): a low-level keyboard
    // hook (0x7B24C0/0x7B2540) that eats Alt+Tab, Alt+Esc and Ctrl+Esc, RegisterHotKey(Alt+Tab), and
    // SystemParametersInfo(SPI_SCREENSAVERRUNNING, 1), the Windows 9x way to disable task switching. Each call
    // becomes "pop its 4 arguments, return 0"; the matching cleanup at exit (0x7B1C00) then fails harmlessly.
    {
        const std::vector<uint8_t> skip4 = { 0x83, 0xC4, 0x10, 0x31, 0xC0, 0x90 };   // add esp,16; xor eax,eax; nop
        Add("alt-tab: no keyboard hook",      0x7B26FE, { 0xFF, 0x15, 0x80, 0x65, 0x20, 0x01 }, skip4);
        Add("alt-tab: no Alt+Tab hotkey",     0x7B272E, { 0xFF, 0x15, 0x04, 0x65, 0x20, 0x01 }, skip4);
        Add("alt-tab: no screensaver flag",   0x7B273E, { 0xFF, 0x15, 0x24, 0x65, 0x20, 0x01 }, skip4);
        Add("alt-tab: no screensaver reset",  0x7B1C2F, { 0xFF, 0x15, 0x24, 0x65, 0x20, 0x01 }, skip4);
    }

    // --- 3b. shadow passes keep running while RAY TRACING is on ----------------------------------
    Add("shadow pass gate (744BE8)",   0x744BE9, Rel32(0x744BE9, 0x716070), Rel32(0x744BE9, (uintptr_t)cb_ShadowSettingForRenderer));
    Add("shadow light count (748F04)", 0x748F05, Rel32(0x748F05, 0x716070), Rel32(0x748F05, (uintptr_t)cb_ShadowSettingForRenderer));

    // --- 4. optional fixed resolution ------------------------------------------------------
    if (g_cfg.overrideRes) {
        int w = g_cfg.width, h = g_cfg.height;
        if (w <= 0 || h <= 0) { w = g_monH * 4 / 3; h = g_monH; }
        Log("OverrideResolution %dx%d", w, h);
        Add("ctor default width",  0x7B1DA3, U32(640), U32(w));
        Add("ctor default height", 0x7B1DAD, U32(480), U32(h));
        std::vector<uint8_t> r = { 0xB8 }; auto hh = U32(h); r.insert(r.end(), hh.begin(), hh.end());
        r.push_back(0xB9); auto ww = U32(w); r.insert(r.end(), ww.begin(), ww.end()); r.push_back(0x90);
        Add("SetResolution override", 0x7A5A00, { 0xA1, 0x40, 0x40, 0x20, 0x01, 0x8B, 0x0D, 0x2C, 0xC8, 0x1F, 0x01 }, r);
    }

    // file opens (atlas redirect, profiler) and reads (profiler)
    { // DOORWAY EMPHASIS icons -> hk_DoorIcons (still copy of the chosen style for the redesigned page)
        const uintptr_t site = 0x71694F;
        std::vector<uint8_t> o = { 0xE8 }; auto ob = U32(uint32_t(int32_t(0x716C10 - (site + 5)))); o.insert(o.end(), ob.begin(), ob.end());
        std::vector<uint8_t> r = { 0xE8 }; auto rel = Rel32(site + 1, (uintptr_t)hk_DoorIcons); r.insert(r.end(), rel.begin(), rel.end());
        Add("doorway icons -> SoDFix", site, o, r);
    }
    // option menu mouse blanked under our screens (hk_MouseRead)
    for (uintptr_t site : { (uintptr_t)0x7AF417, (uintptr_t)0x7AE512 }) {
        const int32_t orig = int32_t(0x7AE310 - (site + 5));
        std::vector<uint8_t> o = { 0xE8 }; auto ob = U32(uint32_t(orig)); o.insert(o.end(), ob.begin(), ob.end());
        std::vector<uint8_t> r = { 0xE8 }; auto rel = Rel32(site + 1, (uintptr_t)hk_MouseRead); r.insert(r.end(), rel.begin(), rel.end());
        Add(site == 0x7AF417 ? "mouse read -> SoDFix (1)" : "mouse read -> SoDFix (2)", site, o, r);
    }
    HookIAT("IAT CreateFileA", A::IAT_CreateFileA, (void*)hk_CreateFileA, (void**)&o_CreateFileA);
    HookIAT("IAT CreateFileW", A::IAT_CreateFileW, (void*)hk_CreateFileW, (void**)&o_CreateFileW);
    HookIAT("IAT ReadFile",    0x12064EC,          (void*)hk_ReadFile,    (void**)&o_ReadFile);
}

// ---------------------------------------------------------------- dinput8 proxy
typedef HRESULT (WINAPI* PFN_DI8C)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);
static HMODULE  g_real = nullptr;
static PFN_DI8C g_realCreate = nullptr;
static bool LoadReal() {
    if (g_realCreate) return true;
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat_s(path, "\\dinput8.dll");
    g_real = LoadLibraryA(path);
    if (g_real) g_realCreate = (PFN_DI8C)GetProcAddress(g_real, "DirectInput8Create");
    Log("real dinput8: %s -> %p", path, (void*)g_realCreate);
    return g_realCreate != nullptr;
}
extern "C" HRESULT WINAPI DirectInput8Create_proxy(HINSTANCE h, DWORD v, REFIID r, LPVOID* o, void* u) {
    if (!LoadReal()) return E_FAIL;
    return g_realCreate(h, v, r, o, u);
}

// ---------------------------------------------------------------- entry
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_exeInst = GetModuleHandleA(nullptr);
        GetModuleFileNameA(nullptr, g_dir, MAX_PATH);
        if (char* s = strrchr(g_dir, '\\')) *s = 0;
        LoadConfig();
        if (g_cfg.log) { char lp[MAX_PATH]; snprintf(lp, sizeof lp, "%s\\SoDFix.log", g_dir); g_log = fopen(lp, "w"); }
        Log("SoDFix " SODFIX_VERSION " (build " SODFIX_BUILD ") loaded (enabled=%d)", g_cfg.enabled);
        SetUnhandledExceptionFilter(CrashFilter);
        if (g_cfg.enabled && IsSupportedExe()) { MakeDpiAware(); BuildPatches(); ApplyAll(); g_patchesActive = true; ApplyFps(); }
    } else if (reason == DLL_PROCESS_DETACH) {
        RestoreAll();
        Log("unloaded, patches restored");
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
