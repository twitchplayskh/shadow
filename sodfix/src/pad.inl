// ---------------------------------------------------------------- modern controllers (SDL3)
// The game only knows DirectInput pads, found once at start (no hot-plug), and reads the left stick and four
// buttons. Instead, every controller SDL3 knows (Xbox, PlayStation 3/4/5, Switch Pro / Joy-Cons, Steam Deck and
// Steam Controller, 8BitDo and other XInput / DirectInput / HID pads) is read here, plugged in or out at any time,
// and its buttons are pressed as the game's own keys in the keyboard state the game reads every frame
// (hk_KeyboardState). So every menu, cutscene skip, pause and camera action works exactly as it does from the
// keyboard, and the keyboard and mouse keep working alongside. The game's own DirectInput pad read (0x7AE1C0) is
// switched off so nothing is pressed twice. SDL3.dll is loaded from the SoDFix folder (or next to the game); without
// it the game's own pad support stays as it was.
//
// Which kind of device was used last (keyboard, Xbox-style, PlayStation, Nintendo) is exported for the renderer,
// which shows the matching button prompts in the menus (SoDFix_GetPromptStyle).

namespace Pad {

enum { BTN_SOUTH = 0, BTN_EAST, BTN_WEST, BTN_NORTH, BTN_BACK, BTN_GUIDE, BTN_START, BTN_LSTICK, BTN_RSTICK,
       BTN_LSHOULDER, BTN_RSHOULDER, BTN_DUP, BTN_DDOWN, BTN_DLEFT, BTN_DRIGHT };
enum { AX_LX = 0, AX_LY, AX_RX, AX_RY, AX_LT, AX_RT };
enum { EV_GAMEPAD_ADDED = 0x653, EV_GAMEPAD_REMOVED = 0x654 };
enum { T_UNKNOWN = 0, T_STANDARD, T_XBOX360, T_XBOXONE, T_PS3, T_PS4, T_PS5, T_SWITCH_PRO, T_JOYCON_L, T_JOYCON_R,
       T_JOYCON_PAIR, T_GAMECUBE, T_STEAM };
enum Style { STYLE_KEYBOARD = 0, STYLE_XBOX = 1, STYLE_PLAYSTATION = 2, STYLE_NINTENDO = 3 };

typedef bool        (__cdecl* PFN_Init)(uint32_t);
typedef bool        (__cdecl* PFN_SetHint)(const char*, const char*);
typedef bool        (__cdecl* PFN_PollEvent)(void*);
typedef void*       (__cdecl* PFN_OpenGamepad)(uint32_t);
typedef void        (__cdecl* PFN_CloseGamepad)(void*);
typedef bool        (__cdecl* PFN_GetButton)(void*, int);
typedef int16_t     (__cdecl* PFN_GetAxis)(void*, int);
typedef int         (__cdecl* PFN_GetType)(void*);
typedef const char* (__cdecl* PFN_GetName)(void*);
typedef uint32_t    (__cdecl* PFN_GetID)(void*);
typedef const char* (__cdecl* PFN_GetError)();

static struct {
    HMODULE dll = nullptr;
    PFN_Init Init; PFN_SetHint SetHint; PFN_PollEvent PollEvent; PFN_OpenGamepad Open; PFN_CloseGamepad Close;
    PFN_GetButton Button; PFN_GetAxis Axis; PFN_GetType Type; PFN_GetName Name; PFN_GetID Id; PFN_GetError Error;
} S;

struct Device { void* pad; uint32_t id; int type; uint32_t lastKeys; };
static std::vector<Device> g_devs;
static bool g_tried = false, g_ready = false;
static volatile int g_style = STYLE_KEYBOARD;
static uint32_t g_raw = 0;                   // inputs held on any controller: bit per SDL button, 15 LT, 16 RT (REMAP)

static int StyleOf(int type) {
    switch (type) {
        case T_PS3: case T_PS4: case T_PS5: return STYLE_PLAYSTATION;
        case T_SWITCH_PRO: case T_JOYCON_L: case T_JOYCON_R: case T_JOYCON_PAIR: case T_GAMECUBE: return STYLE_NINTENDO;
        default: return STYLE_XBOX;          // Xbox, Steam Deck / Steam Controller, XInput and generic pads: A / B
    }
}
static const char* StyleName(int s) {
    static const char* n[4] = { "keyboard", "Xbox-style (A/B)", "PlayStation", "Nintendo" };
    return s >= 0 && s < 4 ? n[s] : "?";
}
static void SetStyle(int s, const char* why) {
    if (g_style == s) return;
    g_style = s;
    Log("controller: prompts -> %s (%s)", StyleName(s), why);
}

static bool Load() {
    if (g_tried) return g_ready;
    g_tried = true;
    if (!g_cfg.gamepad) { Log("controller: modern controller support off ([Controls] Gamepad=0)"); return false; }
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\SoDFix\\SDL3.dll", g_dir);
    S.dll = LoadLibraryA(p);
    if (!S.dll) { snprintf(p, sizeof p, "%s\\SDL3.dll", g_dir); S.dll = LoadLibraryA(p); }
    if (!S.dll) { Log("controller: SDL3.dll not found, modern controller support off"); return false; }
    #define SYM(f, n) S.f = (decltype(S.f))GetProcAddress(S.dll, n); if (!S.f) { Log("controller: SDL3.dll has no %s", n); return false; }
    SYM(Init, "SDL_Init") SYM(SetHint, "SDL_SetHint") SYM(PollEvent, "SDL_PollEvent") SYM(Open, "SDL_OpenGamepad")
    SYM(Close, "SDL_CloseGamepad") SYM(Button, "SDL_GetGamepadButton") SYM(Axis, "SDL_GetGamepadAxis")
    SYM(Type, "SDL_GetGamepadType") SYM(Name, "SDL_GetGamepadName") SYM(Id, "SDL_GetGamepadID") SYM(Error, "SDL_GetError")
    #undef SYM
    // the game window has focus or not: SoDFix decides (keys are only pressed while it is in front), SDL keeps reading
    S.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    S.SetHint("SDL_JOYSTICK_HIDAPI_PS5_PLAYER_LED", "0");
    if (!S.Init(0x00002000u)) { Log("controller: SDL_Init(GAMEPAD) failed: %s", S.Error()); return false; }
    Log("controller: SDL3 ready (%s), controllers can be plugged in or out at any time", p);
    g_ready = true;
    return true;
}

// one poll per keyboard read: hot-plug events, then every pad's buttons as game keys
static void Poll(uint8_t* keys, bool focused) {
    if (!Load()) return;
    alignas(8) uint8_t ev[128];
    while (S.PollEvent(ev)) {
        const uint32_t type = *(uint32_t*)ev, which = *(uint32_t*)(ev + 16);
        if (type == EV_GAMEPAD_ADDED) {
            bool have = false;
            for (auto& d : g_devs) if (d.id == which) have = true;
            if (have) continue;
            void* pad = S.Open(which);
            if (!pad) { Log("controller: could not open controller %u: %s", which, S.Error()); continue; }
            Device d = { pad, which, S.Type(pad), 0 };
            g_devs.push_back(d);
            const char* nm = S.Name(pad);
            Log("controller: connected \"%s\" (type %d, %s)", nm ? nm : "?", d.type, StyleName(StyleOf(d.type)));
            SetStyle(StyleOf(d.type), "controller connected");
        } else if (type == EV_GAMEPAD_REMOVED) {
            for (size_t i = 0; i < g_devs.size(); i++) if (g_devs[i].id == which) {
                Log("controller: disconnected (id %u)", which);
                S.Close(g_devs[i].pad);
                g_devs.erase(g_devs.begin() + i);
                break;
            }
            if (g_devs.empty() && g_style != STYLE_KEYBOARD) SetStyle(STYLE_KEYBOARD, "last controller removed");
        }
    }
    if (!focused || !keys) { g_raw = 0; return; }

    // the game's keys (DirectInput scan codes), as in the manual: Enter talk / inspect / select, Backspace cancel /
    // skip a scene, Right Ctrl inventory, Right Shift city map, Delete / Page Down rotate the camera left / right,
    // End change view, Esc pause (and skip videos), arrows move / menus
    enum : uint8_t { K_ENTER = 0x1C, K_BACK = 0x0E, K_RCTRL = 0x9D, K_RSHIFT = 0x36, K_DEL = 0xD3, K_PGDN = 0xD1,
                     K_END = 0xCF, K_ESC = 0x01, K_UP = 0xC8, K_DOWN = 0xD0, K_LEFT = 0xCB, K_RIGHT = 0xCD };
    const float dz = g_cfg.padDeadzone / 100.0f;
    Bind::Load();
    uint32_t rawAll = 0;
    for (auto& d : g_devs) {
        auto B = [&](int b) { return S.Button(d.pad, b); };
        auto Ax = [&](int a) { return S.Axis(d.pad, a) / 32767.0f; };
        // Nintendo pads: confirm is the right face button (A), back the bottom one (B), as on the Switch
        const bool nin = StyleOf(d.type) == STYLE_NINTENDO;
        const float lx = Ax(AX_LX), ly = Ax(AX_LY), rx = Ax(AX_RX);
        const float lt = Ax(AX_LT), rt = Ax(AX_RT);
        uint32_t held = 0;
        for (int b = 0; b < 15; b++) if (b != BTN_GUIDE && B(b)) held |= 1u << b;
        if (lt > 0.5f) held |= 1u << Bind::PAD_LT;
        if (rt > 0.5f) held |= 1u << Bind::PAD_RT;
        rawAll |= held;
        uint32_t k = 0;                                   // bit per game key, for change detection
        auto press = [&](uint8_t key, bool on, int bit) { if (on) { keys[key] = 0x80; k |= 1u << bit; } };
        // each action's button (REMAP; the defaults while the REMAP screen itself is open)
        uint32_t used = 0;
        for (int a = 0; a < Bind::N; a++) {
            int in = g_rmOpenFlag ? -1 : Bind::pad[a];
            if (in < 0) in = (nin && a == 4) ? BTN_EAST : (nin && a == 5) ? BTN_SOUTH : Bind::kActions[a].pad;
            used |= 1u << in;
            press(Bind::kActions[a].key, (held >> in) & 1u, a);
        }
        // the sticks always move and turn the camera; LT / RT / View too, unless given to an action
        press(K_UP,    ly < -dz, 0);  press(K_DOWN,  ly > dz, 1);
        press(K_LEFT,  lx < -dz, 2);  press(K_RIGHT, lx > dz, 3);
        press(K_DEL,   rx < -0.5f || (!(used >> Bind::PAD_LT & 1u) && lt > 0.5f), 8);
        press(K_PGDN,  rx >  0.5f || (!(used >> Bind::PAD_RT & 1u) && rt > 0.5f), 9);
        press(K_END,   !(used >> BTN_BACK & 1u) && B(BTN_BACK), 10);
        if (k && k != d.lastKeys) SetStyle(StyleOf(d.type), "controller used");
        d.lastKeys = k;
    }
    g_raw = rawAll;
}

} // namespace Pad

extern "C" __declspec(dllexport) int __cdecl SoDFix_GetPromptStyle() { return Pad::g_style; }
