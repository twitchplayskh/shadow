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

#define SODFIX_VERSION "0.18.6"

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
    if (g_cfg.aoMode < 0 || g_cfg.aoMode > 2) g_cfg.aoMode = 0;
    g_cfg.ssrMode     = I("Display", "Reflections", 0);
    if (g_cfg.ssrMode < 0 || g_cfg.ssrMode > 2) g_cfg.ssrMode = 0;
    g_cfg.upMode      = I("Display", "TextureUpscaling", 3);
    if (g_cfg.upMode < 0 || g_cfg.upMode > 3) g_cfg.upMode = 3;
    g_cfg.geomMode    = I("Display", "ImprovedGeometry", 0);
    if (g_cfg.geomMode < 0 || g_cfg.geomMode > 1) g_cfg.geomMode = 0;
    g_cfg.smoothCam   = I("Display", "SmoothCamera", 1) != 0;
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
static const SpriteRect kAoValue[3] = { { 887, 13991,  48,   0,  48, 32 },  // OFF  (game rect 0x0B)
                                        { 887, 13991, 216, 366,  82, 32 },  // LOW  (0x59)
                                        { 887, 13991, 412, 366,  90, 32 } };// HIGH (0x5B)
static const SpriteRect& kSsrTitle = kArt_REFLECTIONS;     // image 10 (texture 4999, new in 0.11)
static const SpriteRect& kSsrHelp  = kArt_help_ssr;        // "Reflections on polished floors"
static const SpriteRect& kUpTitle  = kArt_TEXTURE_UPSCALING; // image 10 (0.17)
static const SpriteRect& kUpHelp   = kArt_help_up;         // "Sharper, more detailed textures"
static const SpriteRect& kGeomTitle = kArt_IMPROVED_GEOMETRY; // image 10 (0.18)
static const SpriteRect& kGeomHelp  = kArt_help_geom;       // "Seamless streets, no repeating tiles"
static const SpriteRect kGeomValue[2] = { { 887, 13991, 48, 0, 48, 32 },   // OFF (game rect 0x0B)
                                          { 887, 13991,  0, 0, 40, 32 } }; // ON  (game rect 0x0A)
static const SpriteRect kUpValue[4] = { { 887, 13991, 48, 0, 48, 32 }, kArt_V2X, kArt_V3X, kArt_V4X };   // OFF (game rect 0x0B)
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
static const int kMenuObjSize = 0xC0;
// GRAPHIC SETTING (page 3): 12 items + EXIT
static RangeItem g_page3Items[12];
static int16_t   g_page3Rows[13] = { 97, 120, 98, 99, 100, 124, 130, 134, 154, 158, 162, 166, 117 };
static int16_t   g_page3Help[13] = { 73, 73, 74, 75, 76, 76, 76, 76, 76, 76, 76, 76, 62 };
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
       SPR_SC_LINE = 176, POOL_NEW = 177 };
// CONTROLLER SETTING (page 0): TYPE + LAYOUT (keyboard only) + EXIT. The game reads the item count (code immediate)
// and the row/help tables (pointers) every frame, so both are switched with TYPE.
static RangeItem g_page0Items[2];
static int16_t   g_page0Rows[3] = { 23, SPR_KB_TITLE, 26 };
static int16_t   g_page0Help[3] = { 63, 63, 62 };
// 5 visible rows (104..296): a 6th row at 344 put its highlight bar over the help panel (0.8.1)
static const int kRowPitch = 48, kRow0Y = 104, kVisibleRows = 5, kMaxRows = 13;
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
    { 3, 13, {
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
static void PageScroll(uint8_t* pool, PageDef& pd, int row) {
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
        ShiftY(Spr(pool, 1), dy);                      // cursor bar, already placed for this frame
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
    int count = kb ? 2 : 1;
    if (count != g_p0Count) { WriteMem(kPage0Count, &count, 4); g_p0Count = count; }
    uint32_t rows = kb ? (uint32_t)(uintptr_t)g_page0Rows : (uint32_t)kPage0RowsOrig;
    uint32_t help = kb ? (uint32_t)(uintptr_t)g_page0Help : (uint32_t)kPage0HelpOrig;
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
    uint8_t* pool = *(uint8_t**)(obj + 0x28);
    if (!pool) return;
    int page = *(int*)(obj + 0x30), cur = *(int*)(obj + 0x38);
    int row = page == 3 ? cur : -1, row2 = page == 2 ? cur : -1, row0 = page == 0 ? cur : -1;
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
    PageScroll(pool, g3, row);
    PageScroll(pool, g2, row2);

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
    BuildRowSprites(pool, g2, row2, g2.rows[4], &kCbTitle, 1, GameRect(0x0A), 0, 4);
    BuildOnOffRow(pool, g2.rows[4], 4, *cb, *(int*)(obj + 0x70));
    BuildRowSprites(pool, g2, row2, g2.rows[5], &kScTitle, kArt_SMOOTH_CAMERA_yoff, GameRect(0x0A), 0, 5);
    BuildOnOffRow(pool, g2.rows[5], 5, *smc, *(int*)(obj + 0x70));
    // the icon y argument moves the icons twice as far as sprite pixels (seen in game: -48 put them 2 rows up)
    { int y = 0xF4 - g2.scroll * kRowPitch / 2; if (y != g_doorY) { WriteMem(kDoorIconY, &y, 4); g_doorY = y; } }
    // help line (sprite 118): our rect, centred like the game's own (0x776B30 mode 4: pivot = size / 2)
    const SpriteRect* help = row == 1 ? &kModeHelp : row == 5 ? &kRtHelp : row == 6 ? &kAaHelp : row == 7 ? &kTfHelp : row == 8 ? &kAoHelp : row == 9 ? &kSsrHelp : row == 10 ? &kUpHelp : row == 11 ? &kGeomHelp
                           : row2 == 4 ? &kCbHelp : row2 == 5 ? &kScHelp : (row0 == 1 && g_p0Count == 2) ? &kArt_help_kb : nullptr;
    if (help) {
        uint8_t* h = Spr(pool, 118);
        *(const SpriteRect**)(h + 0x48) = help;
        *(int*)(h + 0x40) = (help->w & ~1) << 3;
        *(int*)(h + 0x44) = (help->h & ~1) << 3;
    }

    PageFinish(pool, g3);
    PageFinish(pool, g2);
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
__declspec(dllexport) int __cdecl SoDFix_GetReflections() { return g_cfg.ssrMode; }
__declspec(dllexport) int __cdecl SoDFix_GetTextureUpscaling() { return g_cfg.upMode; }
__declspec(dllexport) int __cdecl SoDFix_GetImprovedGeometry() { return g_cfg.geomMode; }
// the game's texture table: 40-byte entries indexed -16384..16383 (IDirect3DTexture8* at +0x10). Room draws refer to
// pages (index > 0) and cut sub-textures (< 0) by these indices (sod/ssr/NOTES.md); 0 = unsupported exe
__declspec(dllexport) uintptr_t __cdecl SoDFix_GetTextureTable() { return g_patchesActive ? *(uintptr_t*)0xAB3998 : 0; }
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
typedef HRESULT (WINAPI* PFN_GetState)(void*, DWORD, void*);
extern "C" HRESULT WINAPI hk_KeyboardState(void* dev, DWORD size, void* buf) {
    HRESULT hr = ((PFN_GetState)((*(void***)dev)[9]))(dev, size, buf);
    // alt-tabbed away: the keyboard device keeps reporting keys typed into other programs (0.12.1)
    HWND hw = *(HWND*)A::MainHwnd;
    if (hr >= 0 && hw && GetForegroundWindow() != hw) { memset(buf, 0, size); return hr; }
    if (hr >= 0 && size == 256 && g_cfg.kbLayout == 1 && *(uint8_t*)kCtrlType == 0) {
        uint8_t* k = (uint8_t*)buf; uint8_t in[256]; memcpy(in, k, 256);
        for (auto& m : kModernKeys) k[m.from] = 0;
        for (auto& m : kModernKeys) k[m.to] |= in[m.from];
    }
    return hr;
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
static bool  g_haveAtlas = false;

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
static HANDLE WINAPI hk_CreateFileA(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    if (g_haveAtlas && p && EndsWithAtlas(p) && !(a & GENERIC_WRITE)) { Log("redirect %s -> %s", p, g_atlasPath); p = g_atlasPath; }
    return o_CreateFileA(p, a, s, sa, c, f, t);
}
static HANDLE WINAPI hk_CreateFileW(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    if (g_haveAtlas && p && !(a & GENERIC_WRITE)) {
        char n[MAX_PATH * 2];
        if (WideCharToMultiByte(CP_ACP, 0, p, -1, n, sizeof n, nullptr, nullptr) && EndsWithAtlas(n)) {
            Log("redirect (W) %s -> %s", n, g_atlasPath); return o_CreateFileA(g_atlasPath, a, s, sa, c, f, t);
        }
    }
    return o_CreateFileW(p, a, s, sa, c, f, t);
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
        Add("page3 item count 4->12", 0x716FC1, U32(4), U32(12));
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
        Add("menu object 0x94->0xC0", 0x716631, U32(0x94), U32(kMenuObjSize));
        // sprite pool 120 -> 151 (push imm8 can't hold 130: call a stub that pushes the count)
        { Asm s(NextStub()); s.raw({ 0x58, 0x68 }).u32(POOL_NEW).raw({ 0x50 }).jmp(0x776AB0); uintptr_t st = Emit(s);
          std::vector<uint8_t> r = { 0xE8 }; auto rel = Rel32(0x71675F, st); r.insert(r.end(), rel.begin(), rel.end()); r.push_back(0x90); r.push_back(0x90);
          Add("sprite pool alloc 120->177", 0x71675E, { 0x6A, 0x78, 0xE8, 0x4B, 0x03, 0x06, 0x00 }, r); }
        { Asm s(NextStub());
          s.raw({ 0x58, 0x68 }).u32(POOL_NEW).raw({ 0x50, 0x60, 0x56 }).callAbs(&p_MenuFrame).raw({ 0x83, 0xC4, 0x04, 0x61 }).jmp(A::SpriteDraw);
          uintptr_t st = Emit(s);
          std::vector<uint8_t> r = { 0xE8 }; auto rel = Rel32(0x716931, st); r.insert(r.end(), rel.begin(), rel.end()); r.push_back(0x90); r.push_back(0x90);
          Add("sprite pool draw 120->177 + frame hook", 0x716930, { 0x6A, 0x78, 0xE8, 0x19, 0x03, 0x06, 0x00 }, r); }
        // rows: RES 104, SCREEN MODE 152, DRAW 200, FOCUS 248, SHADOW 296, RT SHADOW CASTING 344, ANTI-ALIASING 392,
        // TEXTURE FILTERING 440, AMBIENT OCCLUSION 488, REFLECTIONS 536, TEXTURE UPSCALING 584, IMPROVED GEOMETRY 632, EXIT 680 (5 visible, scrolled into view)
        struct { int entry; int16_t from, to; } mv[] = {
            {48,152,200},{49,200,248},{50,248,296},{51,312,680},
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

    // --- 3a. CONTROLLER SETTING: LAYOUT row; MODERN keyboard layout ---------------------------------------
    if (g_cfg.modeMenu && g_haveAtlas) {
        memcpy(g_page0Items, (const void*)0x939400, sizeof(RangeItem));
        g_page0Items[1] = { 0, 1, (int16_t)kKbId };
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

    if (g_haveAtlas) {
        HookIAT("IAT CreateFileA", A::IAT_CreateFileA, (void*)hk_CreateFileA, (void**)&o_CreateFileA);
        HookIAT("IAT CreateFileW", A::IAT_CreateFileW, (void*)hk_CreateFileW, (void**)&o_CreateFileW);
    }
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
        Log("SoDFix " SODFIX_VERSION " loaded (enabled=%d)", g_cfg.enabled);
        if (g_cfg.enabled && IsSupportedExe()) { MakeDpiAware(); BuildPatches(); ApplyAll(); g_patchesActive = true; }
    } else if (reason == DLL_PROCESS_DETACH) {
        RestoreAll();
        Log("unloaded, patches restored");
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
