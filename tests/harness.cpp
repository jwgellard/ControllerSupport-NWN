// End-to-end test without the game or a physical controller.
//
// The official SDL2.dll stands in for nwmain.exe: it has the same dynamic-API
// layout (exported stubs that `jmp [rip+X]` through a jump table), so
// InstallController hooks its SDL_PollEvent exactly as it hooks the game's.
// Device-facing calls are swapped for scripted fakes; everything else (subsystem
// init, the mapping DB, key-name parsing, the event queue) is real SDL.
//
// Usage: harness <SDL2.dll> <dir containing nwn_controller.ini + gamecontrollerdb.txt>
#include <windows.h>

#include <GL/gl.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/config.h"
#include "../src/controller.h"
#include "../src/game.h"
#include "../src/inject.h"
#include "../src/overlay.h"
#include "../src/panelnav.h"
#include "../src/quickbar.h"
#include "../src/text.h"
#include "../src/log.h"
#include "../src/sdl_api.h"

namespace {

bool g_buttons[SDL_CONTROLLER_BUTTON_MAX];
Sint16 g_axes[SDL_CONTROLLER_AXIS_MAX];
SDL_Window* g_focus;
std::vector<std::pair<int, int>> g_warps;
int g_mouseX = 100, g_mouseY = 100;
int g_failures;

SDL_GameController* const kFakePad = reinterpret_cast<SDL_GameController*>(0x1000);
SDL_Joystick* const kFakeStick = reinterpret_cast<SDL_Joystick*>(0x2000);

int SDLCALL FakeNumJoysticks() { return 1; }
SDL_bool SDLCALL FakeIsGameController(int) { return SDL_TRUE; }
SDL_GameController* SDLCALL FakeOpen(int) { return kFakePad; }
void SDLCALL FakeClose(SDL_GameController*) {}
const char* SDLCALL FakeName(SDL_GameController*) { return "Scripted Pad"; }
SDL_Joystick* SDLCALL FakeGetJoystick(SDL_GameController*) { return kFakeStick; }
SDL_JoystickID SDLCALL FakeInstanceId(SDL_Joystick*) { return 7; }
Uint8 SDLCALL FakeButton(SDL_GameController*, SDL_GameControllerButton b) { return g_buttons[b]; }
Sint16 SDLCALL FakeAxis(SDL_GameController*, SDL_GameControllerAxis a) { return g_axes[a]; }
SDL_Window* SDLCALL FakeFocus() { return g_focus; }
Uint32 SDLCALL FakeMouseState(int* x, int* y)
{
    if (x) *x = g_mouseX;
    if (y) *y = g_mouseY;
    return 0;
}
void SDLCALL FakeWarp(SDL_Window*, int x, int y)
{
    g_warps.emplace_back(x, y);
    g_mouseX = x;
    g_mouseY = y;
}

using PollFn = int(SDLCALL*)(SDL_Event*);
PollFn g_poll;

// One game frame: drain the (hooked) queue and describe what came out.
std::string Frame()
{
    Sleep(16);
    std::string out;
    SDL_Event e;
    while (g_poll(&e)) {
        char buf[96] = "";
        switch (e.type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            snprintf(buf, sizeof(buf), "%s%s%s ", e.type == SDL_KEYDOWN ? "+" : "-",
                     sdl.SDL_GetScancodeName(e.key.keysym.scancode),
                     (e.key.keysym.mod & KMOD_SHIFT) ? "[shift]" : "");
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            snprintf(buf, sizeof(buf), "%smouse%d@%d,%d ", e.type == SDL_MOUSEBUTTONDOWN ? "+" : "-",
                     e.button.button, e.button.x, e.button.y);
            break;
        default:
            break; // window/system events from the real SDL
        }
        out += buf;
    }
    if (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

void Expect(const char* step, const std::string& got, const char* want)
{
    bool ok = got == want;
    printf("[%s] %-44s got '%s'%s%s%s\n", ok ? "PASS" : "FAIL", step, got.c_str(), ok ? "" : " want '",
           ok ? "" : want, ok ? "" : "'");
    if (!ok)
        ++g_failures;
}

void Check(const char* step, bool ok, const std::string& detail)
{
    printf("[%s] %-44s %s\n", ok ? "PASS" : "FAIL", step, detail.c_str());
    if (!ok)
        ++g_failures;
}

// A dark stand-in for the game's frame, so each preview shows only its own overlay.
void ClearFrame()
{
    glClearColor(0.12f, 0.14f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

// Writes what was just presented to a 24-bit BMP (best effort; not a check).
void SaveFrontBuffer(int w, int h, const char* path)
{
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 3);
    glReadBuffer(GL_FRONT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGR_EXT, GL_UNSIGNED_BYTE, px.data());
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h; // bottom-up, like GL
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(px.size());
    if (FILE* f = fopen(path, "wb")) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(px.data(), 1, px.size(), f);
        fclose(f);
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: harness <SDL2.dll> <mod dir>\n");
        return 2;
    }
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) {
        fprintf(stderr, "can't load %s\n", argv[1]);
        return 2;
    }
    LogOpen("harness.log");
    std::string dir = argv[2];
    if (dir.back() != '\\' && dir.back() != '/')
        dir += '\\';

    // Any SDL call populates the DLL's own jump table, as the game's init does.
    auto getVersion = reinterpret_cast<decltype(&SDL_GetVersion)>(GetProcAddress(dll, "SDL_GetVersion"));
    SDL_version v;
    getVersion(&v);

    // The table lives in the DLL's data section, within a few MB of the stubs.
    auto stub = reinterpret_cast<char*>(GetProcAddress(dll, "SDL_PollEvent"));
    void** slot = FindDynApiSlot(dll, "SDL_PollEvent", stub - 0x4000000, 0x8000000u);
    Check("SDL_PollEvent stub decodes to a slot", slot != nullptr, "");
    if (!slot)
        return 1;
    auto tableBase = reinterpret_cast<char*>(slot) - 0x2000;
    Check("InstallController hooks SDL_PollEvent", InstallController(dll, tableBase, 0x4000, dir), "");
    Check("pick-hook detour round trip", game::DetourSelfTest(), "hook -> trampoline -> original");

    // Quickbar labels: which slot a key fires, and how a game name is shown.
    Check("quickbar slot for F1 / Shift+F12 / Ctrl+F5",
          quickbar::SlotIndex(1, false, false, 0) == 0 && quickbar::SlotIndex(12, true, false, 0) == 23 &&
              quickbar::SlotIndex(5, false, true, 0) == 28,
          "0, 23, 28");
    Check("quickbar bare key follows the shown page",
          quickbar::SlotIndex(3, false, false, 2) == 26 && quickbar::SlotIndex(3, false, false, -1) == 2 &&
              quickbar::SlotIndex(13, false, false, 0) == -1,
          "page 2 -> 26, unknown -> 2, F13 -> -1");
    // Byte escapes: game strings are Windows-1252, the overlay wants UTF-8.
    std::string tagged = text::Display("<c\xff\x10\x10>Flame</c> Blade", 20);
    Check("quickbar label strips colour tags", tagged == "Flame Blade", tagged);
    std::string cut = text::Display("Cure Moderate Wounds of the North", 20);
    Check("quickbar label cuts at a word", cut == "Cure Moderate...", cut);
    std::string noted = text::Display("Barbarian Rage (1x per day)", 20);
    std::string shortNote = text::Display("Rage (1x)", 20);
    Check("quickbar label drops a long name's note", noted == "Barbarian Rage" && shortNote == "Rage (1x)",
          noted + " / " + shortNote);
    std::string reply = text::Display("<StartCheck>[Persuade]</Start>  You should\nlet us pass.");
    Check("dialogue text drops tags, folds lines", reply == "[Persuade] You should let us pass.", reply);
    std::string latin = text::Display("Ep\xe9" "e  ", 20);
    Check("quickbar label is UTF-8", latin == "Ep\xc3\xa9" "e", latin);

    // Config: [play] holds the controller-mode settings; an ini from before it existed
    // (prototype_on_start and the rest under [diag]) still loads the same way.
    {
        char tmp[MAX_PATH];
        GetTempPathA(sizeof(tmp), tmp);
        std::string ini = std::string(tmp) + "nwnc_harness_config.ini";
        auto load = [&](const char* text) {
            if (FILE* f = fopen(ini.c_str(), "w")) {
                fputs(text, f);
                fclose(f);
            }
            Config c;
            LoadConfig(ini, c);
            return c;
        };
        Config fresh = load("[general]\nenabled = 1\n");
        Config shipped = load("[play]\nmode = classic\ntarget_cone = 30\n[diag]\ntarget_cone = 90\n");
        Config legacy = load("[diag]\nprototype_on_start = 0\ntarget_cone = 45\nenabled = 0\n");
        Config legacyOn = load("[diag]\nprototype_on_start = 1\n");
        DeleteFileA(ini.c_str());
        Check("config: controller mode by default, [play] wins, old [diag] keys still read",
              fresh.play.controllerMode && fresh.play.targetCone == 60 && !shipped.play.controllerMode &&
                  shipped.play.targetCone == 30 && !legacy.play.controllerMode && legacy.play.targetCone == 45 &&
                  !legacy.diag.enabled && legacyOn.play.controllerMode,
              "");
    }

    // Panel focus: a fake 400x300 screen with two buttons on top and a 3x2 grid below.
    {
        static int kButtonA, kButtonB, kGrid; // their addresses stand in for controls
        auto hit = [](void*, int x, int y) -> void* {
            if (y >= 20 && y < 60 && x >= 20 && x < 120)
                return &kButtonA;
            if (y >= 20 && y < 60 && x >= 200 && x < 300)
                return &kButtonB;
            if (y >= 100 && y < 220 && x >= 20 && x < 200)
                return &kGrid;
            return nullptr;
        };
        auto grid = [](void*, void* obj, int* cols, int* rows) {
            *cols = 3;
            *rows = 2;
            return obj == &kGrid;
        };
        panelnav::Scanner scan;
        scan.Start(400, 300, 10);
        int rounds = 0;
        while (!scan.Run(hit, nullptr, 200))
            ++rounds;
        std::vector<panelnav::Control> cs = scan.Result(grid, nullptr);
        int a = panelnav::Find(cs, &kButtonA, -1), b = panelnav::Find(cs, &kButtonB, -1);
        int g0 = panelnav::Find(cs, &kGrid, 0), g1 = panelnav::Find(cs, &kGrid, 1), g4 = panelnav::Find(cs, &kGrid, 4);
        bool found = cs.size() == 8 && a >= 0 && b >= 0 && g0 >= 0 && fabsf(cs[g0].r.x0 - 20) <= 5 &&
                     fabsf(cs[g0].r.x1 - 80) <= 5;
        Check("panel scan finds controls and grid cells", found && rounds > 0,
              std::to_string(cs.size()) + " controls in " + std::to_string(rounds + 1) + " rounds");
        bool moves = panelnav::Step(cs, a, 1, 0) == b && panelnav::Step(cs, a, 0, 1) == g0 &&
                     panelnav::Step(cs, g0, 1, 0) == g1 && panelnav::Step(cs, g1, 0, 1) == g4 &&
                     panelnav::Step(cs, g4, 0, -1) == g1 && panelnav::Step(cs, b, 0, -1) == -1 &&
                     panelnav::Nearest(cs, 60, 40) == a;
        Check("panel focus steps by direction", moves, "");

        // The Load screen: a list above, a row of buttons below. Right from Delete goes
        // across the row to Load, not up to the nearer list row.
        static int kRow, kDelete, kLoad, kListBox;
        std::vector<panelnav::Control> load = {
            {&kRow, -1, {504, 684, 1032, 744}},
            {&kDelete, -1, {492, 804, 648, 864}},
            {&kLoad, -1, {1070, 804, 1220, 864}},
            {&kListBox, -1, {492, 264, 1068, 792}}, // behind the rows
        };
        Check("panel focus prefers controls in line", panelnav::Step(load, 1, 1, 0) == 2 &&
                                                          panelnav::Step(load, 1, 0, -1) == 0 &&
                                                          panelnav::Step(load, 0, 1, 0) == 2, ""); // not the box behind
    }

    // The scripted virtual pad (testing aid): a press holds, then releases with a gap.
    inject::Configure(true, "");
    bool loaded = inject::Load("; comment\npress leftshoulder+dpup 0.2\nwait 0.1\nstick left 0 -1 0.3\n");
    inject::Update(0.05f);
    bool held = inject::Button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) && inject::Button(SDL_CONTROLLER_BUTTON_DPAD_UP);
    inject::Update(0.2f);
    bool released = !inject::Button(SDL_CONTROLLER_BUTTON_DPAD_UP);
    inject::Update(0.2f);
    float sx = 0, sy = 0;
    inject::Stick(0, &sx, &sy);
    inject::Update(0.4f);
    float ex = 0, ey = 0;
    inject::Stick(0, &ex, &ey);
    Check("inject script: press, release, stick", loaded && held && released && sy == -1.0f && ey == 0.0f &&
          inject::Idle() && !inject::Load("press nosuchbutton"), "");
    inject::Configure(false, "");

    g_poll = reinterpret_cast<PollFn>(GetProcAddress(dll, "SDL_PollEvent"));
    auto init = reinterpret_cast<decltype(&SDL_Init)>(GetProcAddress(dll, "SDL_Init"));
    auto createWindow = reinterpret_cast<decltype(&SDL_CreateWindow)>(GetProcAddress(dll, "SDL_CreateWindow"));
    if (init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        return 2;
    SDL_Window* win = createWindow("harness", 0, 0, 1920, 1080, SDL_WINDOW_HIDDEN);
    Check("hidden 1920x1080 test window", win != nullptr, "");

    // Swap the hardware-facing calls for fakes (InstallController resolved them already).
    sdl.SDL_NumJoysticks = FakeNumJoysticks;
    sdl.SDL_IsGameController = FakeIsGameController;
    sdl.SDL_GameControllerOpen = FakeOpen;
    sdl.SDL_GameControllerClose = FakeClose;
    sdl.SDL_GameControllerName = FakeName;
    sdl.SDL_GameControllerGetJoystick = FakeGetJoystick;
    sdl.SDL_JoystickInstanceID = FakeInstanceId;
    sdl.SDL_GameControllerGetButton = FakeButton;
    sdl.SDL_GameControllerGetAxis = FakeAxis;
    sdl.SDL_GetKeyboardFocus = FakeFocus;
    sdl.SDL_GetMouseState = FakeMouseState;
    sdl.SDL_WarpMouseInWindow = FakeWarp;

    g_focus = win;
    Expect("idle", Frame(), "");

    g_buttons[SDL_CONTROLLER_BUTTON_A] = true;
    Expect("A down -> left click", Frame(), "+mouse1@100,100");
    g_buttons[SDL_CONTROLLER_BUTTON_A] = false;
    Expect("A up", Frame(), "-mouse1@100,100");

    g_buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = true;
    Expect("LB alone does nothing", Frame(), "");
    g_buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = true;
    Expect("LB + dpad left -> F1", Frame(), "+F1");
    g_buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = false;
    Expect("release LB first: F1 stays held", Frame(), "");
    g_buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = false;
    Expect("release dpad left -> F1 up", Frame(), "-F1");

    g_buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = true;
    g_buttons[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = true;
    Frame();
    g_buttons[SDL_CONTROLLER_BUTTON_A] = true;
    Expect("LB+RB + A -> Shift+F8", Frame(), "+Left Shift[shift] +F8[shift]");
    g_buttons[SDL_CONTROLLER_BUTTON_A] = false;
    Expect("release -> F8 then Shift", Frame(), "-F8[shift] -Left Shift");
    g_buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = false;
    g_buttons[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = false;
    Frame();

    g_buttons[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = true;
    g_buttons[SDL_CONTROLLER_BUTTON_Y] = true;
    Expect("RB + Y -> inventory (I)", Frame(), "+I");
    g_buttons[SDL_CONTROLLER_BUTTON_Y] = false;
    g_buttons[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = false;
    Expect("release", Frame(), "-I");

    g_buttons[SDL_CONTROLLER_BUTTON_Y] = true;
    Expect("Y -> radial on self (Keypad 0)", Frame(), "+Keypad 0");
    g_buttons[SDL_CONTROLLER_BUTTON_Y] = false;
    Frame();

    g_axes[SDL_CONTROLLER_AXIS_LEFTX] = -23000;
    g_axes[SDL_CONTROLLER_AXIS_LEFTY] = -23000;
    Expect("left stick up-left -> W + A", Frame(), "+W +A");
    g_axes[SDL_CONTROLLER_AXIS_LEFTX] = 0;
    Expect("stick straight up -> A released", Frame(), "-A");
    g_axes[SDL_CONTROLLER_AXIS_LEFTY] = 6000;
    Expect("stick inside deadzone -> W released", Frame(), "-W");
    g_axes[SDL_CONTROLLER_AXIS_LEFTY] = 0;

    g_axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 5000;
    Expect("RT below threshold", Frame(), "");
    g_axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 30000;
    Expect("RT pulled -> zoom in (Up)", Frame(), "+Up");
    g_axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 0;
    Expect("RT released", Frame(), "-Up");

    g_warps.clear();
    g_axes[SDL_CONTROLLER_AXIS_RIGHTX] = 32767;
    for (int i = 0; i < 20; ++i)
        Frame();
    g_axes[SDL_CONTROLLER_AXIS_RIGHTX] = 0;
    bool monotonic = !g_warps.empty();
    for (size_t i = 1; i < g_warps.size(); ++i)
        monotonic = monotonic && g_warps[i].first > g_warps[i - 1].first && g_warps[i].second == 100;
    int travelled = g_warps.empty() ? 0 : g_warps.back().first - 100;
    char detail[96];
    snprintf(detail, sizeof(detail), "%zu warps, moved %d px in ~0.33s", g_warps.size(), travelled);
    Check("right stick moves pointer right", monotonic && travelled > 200 && travelled < 900, detail);

    g_buttons[SDL_CONTROLLER_BUTTON_A] = true;
    std::string click = Frame();
    std::string want = "+mouse1@" + std::to_string(g_mouseX) + ",100";
    Check("click lands at the moved pointer", g_mouseX > 300 && click == want, click);
    g_focus = nullptr;
    Expect("focus lost -> held button released", Frame(), ("-" + want.substr(1)).c_str());
    g_buttons[SDL_CONTROLLER_BUTTON_A] = false;
    g_focus = win;

    g_buttons[SDL_CONTROLLER_BUTTON_START] = true;
    Expect("Start -> pause (Space)", Frame(), "+Space");
    g_buttons[SDL_CONTROLLER_BUTTON_START] = false;
    Expect("Start released", Frame(), "-Space");

    // Overlay: a real GL context, presented through the hooked SDL_GL_SwapWindow.
    auto createContext = reinterpret_cast<decltype(&SDL_GL_CreateContext)>(GetProcAddress(dll, "SDL_GL_CreateContext"));
    auto swapWindow = reinterpret_cast<decltype(&SDL_GL_SwapWindow)>(GetProcAddress(dll, "SDL_GL_SwapWindow"));
    SDL_Window* glWin = createWindow("overlay", 0, 0, 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext glContext = glWin ? createContext(glWin) : nullptr;
    Check("hidden GL window and context", glContext != nullptr, "");
    if (glContext) {
        overlay::Hud hud;
        // The longest case: a bumper layer with real quickbar names (wraps by width).
        hud.prompts = {{"Left", "Cure Moderate..."}, {"Up", "Magic Missile"}, {"Right", "Potion of Cure..."},
                       {"Down", "Power Attack"},     {"X", "Knockdown"},     {"Y", "Empty"},
                       {"B", "Ring of Protection"},  {"A", "Barbarian Rage"}};
        hud.badge = "LB";
        overlay::SetHud(hud);
        int before = overlay::FramesDrawn();
        for (int i = 0; i < 3; ++i) {
            ClearFrame();
            swapWindow(glWin);
        }
        int drawn = overlay::FramesDrawn() - before;
        Check("overlay draws through the SwapWindow hook", drawn == 3 && !overlay::Failed(),
              std::to_string(drawn) + " frames" + (overlay::Failed() ? ", FAILED" : ""));
        SaveFrontBuffer(1280, 720, "overlay_preview.bmp"); // for eyeballing the layout

        // A conversation: more replies than fit, one disabled, the selection mid-list.
        overlay::Hud talk;
        talk.prompts = {{"D-pad", "Choose"}, {"A", "Select"}, {"B", "End"}};
        const char* replies[] = {
            "Who are you?",
            "[Persuade] You should let us pass. The guards won't mind, and nobody needs to know we were here "
            "tonight.",
            "I'm looking for work.",
            "[Lore] That sigil on your shield is the mark of the Lords' Alliance, isn't it?",
            "What do you know about the missing caravan?",
            "Tell me about the city.",
            "Can you show me the way to the docks?",
            "I'll be going now.",
            "[Intimidate] Step aside or I'll move you myself.",
            "[End Dialog]",
        };
        for (const char* r : replies)
            talk.list.push_back({text::Display(r), true});
        talk.list[3].enabled = false;
        talk.listSelected = 6;
        auto portrait = std::make_shared<overlay::Image>(); // a stand-in: a 128x200 gradient
        portrait->key = "test_portrait";
        portrait->width = 128;
        portrait->height = 200;
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 128; ++x)
                portrait->rgba.insert(portrait->rgba.end(), {static_cast<uint8_t>(x * 2), static_cast<uint8_t>(y),
                                                             static_cast<uint8_t>(160), 255});
        talk.portrait = portrait;
        talk.title = "Captain Aribeth";
        talk.body = text::Display(
            "The plague has the whole city locked down, and the guards at the gate have orders to turn back anyone "
            "without a writ. <StartAction>[She studies you for a moment.]</Start> You don't look like a merchant. "
            "If you're here about the missing caravan, you'll want to speak with the harbourmaster first; he was the "
            "last to see it leave. Otherwise, I suggest you find an inn before the curfew bell. The watch has little "
            "patience after dark, and less for strangers who can't account for themselves. Now, what brings you to "
            "Neverwinter in times like these?");
        overlay::SetHud(talk);
        before = overlay::FramesDrawn();
        ClearFrame();
        swapWindow(glWin);
        Check("overlay draws a scrolled choice list", overlay::FramesDrawn() - before == 1 && !overlay::Failed(), "");
        SaveFrontBuffer(1280, 720, "overlay_dialog_preview.bmp");
    }

    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
