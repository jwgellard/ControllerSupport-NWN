#include "controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <vector>

#include "config.h"
#include "diag.h"
#include "game.h"
#include "inject.h"
#include "overlay.h"
#include "log.h"
#include "panelnav.h"
#include "quickbar.h"
#include "text.h"
#include "sdl_api.h"

// How this works
// --------------
// NWN:EE drains SDL once per frame with `while (SDL_PollEvent(&e)) WinMessageHandler(&e);`
// and every keyboard/mouse event is routed to CExoInput::AddEvent (keys are looked
// up by scancode) and to the Nuklear UI. The game never initialises SDL's joystick
// subsystem and CExoInput's own joystick methods are stubs that return 0.
//
// We replace SDL_PollEvent in SDL's jump table. When the real queue runs dry we
// sample the controllers and append synthetic SDL_KEYDOWN/UP, SDL_MOUSEBUTTONDOWN/UP
// events to a private queue that is drained before returning 0, so the game sees
// controller input exactly as if it came from the keyboard and mouse. The cursor
// is moved with SDL_WarpMouseInWindow, which moves the OS pointer and produces
// genuine motion events.

namespace {

using PollEventFn = int(SDLCALL*)(SDL_Event*);
using ShowCursorFn = int(SDLCALL*)(int);

PollEventFn g_realPollEvent;
// SDL_ShowCursor is hooked so the mod can hide the pointer on top of what the game
// asks for, and give the game's own choice back when it stops.
ShowCursorFn g_realShowCursor;
int g_gameCursor = -1;  // SDL_ENABLE / SDL_DISABLE as the game last asked; -1 = not yet asked
bool g_pointerHidden;   // the mod is hiding the pointer
std::string g_modDir;
Config g_cfg;
bool g_initialized;
bool g_disabled; // set after a fault so the game keeps running without us
bool g_updatedThisPump;

struct Pad {
    SDL_GameController* controller;
    SDL_JoystickID id;
};
std::vector<Pad> g_pads;
std::deque<SDL_Event> g_pending;

// Output reference counts: several inputs may hold the same key or button.
int g_keyRefs[SDL_NUM_SCANCODES];
int g_mouseRefs[8];
Uint32 g_mouseMask;

// Per-input state. The action is latched at press time so releasing a layer
// button before the face button still releases the right key.
bool g_inputDown[kInputCount];
const Action* g_latched[kInputCount];
bool g_moveHeld[kDirectionCount];

SDL_Window* g_window; // only valid during Update(); NWN recreates its window on mode changes
Uint32 g_windowId;
bool g_cursorActive;
float g_cursorX, g_cursorY;
int g_lastWarpX = -1, g_lastWarpY = -1;
Uint64 g_lastCounter;
bool g_stickCamera;  // cursor stick rotates the camera (falls back to the pointer when there's no camera)
bool g_toggleArmed;  // stick:toggle pressed on its own, not as part of the L3+R3 combo
bool g_cameraLogged; // last logged camera availability, so fallbacks show up once

// Stage-2 prototype (docs/DESIGN.md, M0 spikes S1-S3): direct movement through
// drive mode, and a soft target the pointer is pinned to.
bool g_proto;
bool g_protoAutoStarted;
bool g_snapshotRequested, g_protoToggleRequested;
float g_comboTime = -1.0f; // how long L3+R3 have been held together; -1 when not
bool g_comboToggled;
float g_comboGap; // how long the combo has been released, so a momentary dropout doesn't end it
float g_refreshTimer;
const Action* g_protoHeld[3]; // forward, walk modifier, turn
Action g_walkAction; // Shift: walk modifier for the steer fallback
bool g_walkDriving;  // DriveModeForward is held through the game's input handler (facing mode walk)
bool g_frameLocked;
float g_lockedFwdX, g_lockedFwdY;
float g_lastForcedHeading;
bool g_haveForcedHeading; // false until the first forced send
Uint32 g_lastForceTicks;
float g_protoLogTimer;
game::Vec3 g_protoLastPos;
bool g_protoHavePos;
const char* g_protoState; // last logged reason the stick is (not) driving; null = released
float g_stopTimer;        // after release: keep ending the server's drive action this long
float g_syncTimer;        // after release: keep the server on the drawn stop point this long
int g_syncCount;
float g_coastTimer;       // after release: when to log how far the character coasted
game::Vec3 g_releasePos, g_releaseFacing;
bool g_stopped;           // the server drive was ended at least once since the release
float g_traceTime = -1;   // seconds since release while the stop trace runs; -1 = off
float g_traceNext;
bool g_steerFallbackLogged;
uint32_t g_targetOid = game::kInvalidOid;
int g_layer;           // binding layer held this frame (LB = 1, RB = 2, both = 3)
uint8_t g_targetType;  // for the prompt bar's verb
bool g_targetHostile;
bool g_targetLocked; // picked with the d-pad: kept until out of reach, not replaced by the soft target
uint32_t g_challengerOid = game::kInvalidOid; // a better soft target waiting out the switch delay
float g_challengerTime;
float g_softTimer;
int g_radialDepth;       // radial nesting we last steered (0 = none open)
int g_radialSlot = -1;   // slot the pointer is snapped to (-1 = centre, nothing chosen yet)
int g_radialCheck = -1;  // slot whose highlight hasn't been seen yet (orientation check)
int g_radialCheckFrom;   // the game's highlighted slot before that snap
int g_radialFlip = -1;   // the game's slots run the other way vertically: -1 unknown, 0 no, 1 yes
bool g_pickHooked;   // the game's world pick is patched (InstallPickHook)
bool g_parked;        // the pointer is held mid-screen for targeting
bool g_mouseTakeover; // the real mouse moved: targeting steps aside until the pad is used again
bool g_aimFromStick;  // the left stick is aiming (else the camera does)
float g_aimX, g_aimY;

void DropTarget();
int g_recentWarps[4][2]; // our own warps, so their motion events don't unpin the target
int g_recentWarpNext;

// ---------------------------------------------------------------------------
// Synthetic events

Uint16 SyntheticModifiers()
{
    static const struct { SDL_Scancode sc; Uint16 mod; } kMods[] = {
        {SDL_SCANCODE_LSHIFT, KMOD_LSHIFT}, {SDL_SCANCODE_RSHIFT, KMOD_RSHIFT},
        {SDL_SCANCODE_LCTRL, KMOD_LCTRL},   {SDL_SCANCODE_RCTRL, KMOD_RCTRL},
        {SDL_SCANCODE_LALT, KMOD_LALT},     {SDL_SCANCODE_RALT, KMOD_RALT},
        {SDL_SCANCODE_LGUI, KMOD_LGUI},     {SDL_SCANCODE_RGUI, KMOD_RGUI},
    };
    Uint16 mods = 0;
    for (const auto& m : kMods)
        if (g_keyRefs[m.sc] > 0)
            mods |= m.mod;
    return mods;
}

void QueueKey(SDL_Scancode sc, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.timestamp = sdl.SDL_GetTicks();
    e.key.windowID = g_windowId;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = sc;
    e.key.keysym.sym = sdl.SDL_GetKeyFromScancode(sc);
    e.key.keysym.mod = static_cast<Uint16>(sdl.SDL_GetModState() | SyntheticModifiers());
    g_pending.push_back(e);
}

void CursorPosition(int* x, int* y)
{
    if (g_cursorActive || g_parked) {
        *x = static_cast<int>(lroundf(g_cursorX));
        *y = static_cast<int>(lroundf(g_cursorY));
    } else {
        sdl.SDL_GetMouseState(x, y);
    }
}

// One mouse-wheel notch at the pointer: positive scrolls up (away from the user).
void QueueMouseWheel(int notches)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.wheel.type = SDL_MOUSEWHEEL;
    e.wheel.timestamp = sdl.SDL_GetTicks();
    e.wheel.windowID = g_windowId;
    e.wheel.y = notches;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    g_pending.push_back(e);
}

void QueueMouseButton(Uint8 button, bool down)
{
    if (down)
        g_mouseMask |= SDL_BUTTON(button);
    else
        g_mouseMask &= ~SDL_BUTTON(button);

    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.button.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.timestamp = sdl.SDL_GetTicks();
    e.button.windowID = g_windowId;
    e.button.button = button;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.button.clicks = 1; // NWN tracks double-clicks itself
    CursorPosition(&e.button.x, &e.button.y);
    g_pending.push_back(e);
}

void CenterCursor()
{
    int w = 0, h = 0;
    sdl.SDL_GetWindowSize(g_window, &w, &h);
    if (w <= 0 || h <= 0)
        return;
    g_cursorX = w * 0.5f;
    g_cursorY = h * 0.5f;
    sdl.SDL_WarpMouseInWindow(g_window, w / 2, h / 2);
    g_lastWarpX = w / 2;
    g_lastWarpY = h / 2;
}

void Press(const Action& a)
{
    switch (a.kind) {
    case ActionKind::Keys:
        for (int i = 0; i < a.keyCount; ++i)
            if (g_keyRefs[a.keys[i]]++ == 0)
                QueueKey(a.keys[i], true);
        break;
    case ActionKind::Mouse:
        if (g_mouseRefs[a.mouseButton]++ == 0)
            QueueMouseButton(a.mouseButton, true);
        break;
    case ActionKind::CursorCenter:
        CenterCursor();
        break;
    case ActionKind::StickToggle:
        g_toggleArmed = g_comboTime < 0;
        break;
    default:
        break;
    }
}

void Release(const Action& a)
{
    switch (a.kind) {
    case ActionKind::Keys:
        for (int i = a.keyCount - 1; i >= 0; --i)
            if (g_keyRefs[a.keys[i]] > 0 && --g_keyRefs[a.keys[i]] == 0)
                QueueKey(a.keys[i], false);
        break;
    case ActionKind::Mouse:
        if (g_mouseRefs[a.mouseButton] > 0 && --g_mouseRefs[a.mouseButton] == 0)
            QueueMouseButton(a.mouseButton, false);
        break;
    case ActionKind::StickToggle:
        if (g_toggleArmed) {
            g_toggleArmed = false;
            g_stickCamera = !g_stickCamera;
            g_cursorActive = false; // re-read the real pointer position next time it moves
            if (!g_stickCamera)
                DropTarget(); // the pointer is yours again
            Log("right stick: %s", g_stickCamera ? "camera" : "pointer");
        }
        break;
    default:
        break;
    }
}

void ProtoHold(int slot, const Action* a)
{
    if (g_protoHeld[slot] == a)
        return;
    if (g_protoHeld[slot])
        Release(*g_protoHeld[slot]);
    g_protoHeld[slot] = a;
    if (a)
        Press(*a);
}

// Lets go of the walk drive (DriveModeForward), as releasing its key would.
void ReleaseWalkDrive()
{
    if (g_walkDriving) {
        g_walkDriving = false;
        game::InputAction(game::kActionDriveForwardWalk, false);
    }
}

void ProtoReleaseMovement()
{
    ReleaseWalkDrive();
    for (int i = 0; i < 3; ++i)
        ProtoHold(i, nullptr);
    g_frameLocked = false;
    g_protoHavePos = false;
    g_haveForcedHeading = false; // send the new heading as soon as the stick moves again
}

void DropTarget()
{
    g_targetOid = game::kInvalidOid;
    g_targetLocked = false;
    g_challengerOid = game::kInvalidOid;
    g_parked = false;
    game::SetPickTarget(false, game::kInvalidOid);
}

void ReleaseAll()
{
    ProtoReleaseMovement();
    DropTarget();
    for (int i = 0; i < kInputCount; ++i) {
        if (g_latched[i])
            Release(*g_latched[i]);
        g_latched[i] = nullptr;
        g_inputDown[i] = false;
    }
    for (int d = 0; d < kDirectionCount; ++d) {
        if (g_moveHeld[d])
            Release(g_cfg.move[d]);
        g_moveHeld[d] = false;
    }
    g_cursorActive = false;
}

// ---------------------------------------------------------------------------
// Controller devices

void OpenPad(int deviceIndex)
{
    if (!sdl.SDL_IsGameController(deviceIndex)) {
        char guid[33];
        sdl.SDL_JoystickGetGUIDString(sdl.SDL_JoystickGetDeviceGUID(deviceIndex), guid, sizeof(guid));
        const char* name = sdl.SDL_JoystickNameForIndex(deviceIndex);
        Log("joystick %d '%s' (GUID %s) has no controller mapping; add one to gamecontrollerdb.txt",
            deviceIndex, name ? name : "?", guid);
        return;
    }
    SDL_GameController* gc = sdl.SDL_GameControllerOpen(deviceIndex);
    if (!gc) {
        Log("failed to open controller %d: %s", deviceIndex, sdl.SDL_GetError());
        return;
    }
    SDL_JoystickID id = sdl.SDL_JoystickInstanceID(sdl.SDL_GameControllerGetJoystick(gc));
    for (const Pad& p : g_pads) {
        if (p.id == id) {
            sdl.SDL_GameControllerClose(gc); // already open; drop the extra reference
            return;
        }
    }
    g_pads.push_back({gc, id});
    const char* name = sdl.SDL_GameControllerName(gc);
    Log("controller connected: '%s' (instance %d)", name ? name : "?", id);
}

void ClosePad(SDL_JoystickID id)
{
    for (size_t i = 0; i < g_pads.size(); ++i) {
        if (g_pads[i].id == id) {
            Log("controller disconnected (instance %d)", id);
            sdl.SDL_GameControllerClose(g_pads[i].controller);
            g_pads.erase(g_pads.begin() + i);
            break;
        }
    }
    if (g_pads.empty())
        ReleaseAll();
}

// Returns true for joystick/controller events, which are consumed here.
bool HandleDeviceEvent(const SDL_Event& e)
{
    switch (e.type) {
    case SDL_CONTROLLERDEVICEADDED:
        OpenPad(e.cdevice.which);
        return true;
    case SDL_CONTROLLERDEVICEREMOVED:
        ClosePad(e.cdevice.which);
        return true;
    case SDL_JOYDEVICEADDED:
        if (!sdl.SDL_IsGameController(e.jdevice.which))
            OpenPad(e.jdevice.which); // only logs the missing mapping
        return true;
    default:
        return e.type >= SDL_JOYAXISMOTION && e.type < SDL_FINGERDOWN;
    }
}

// ---------------------------------------------------------------------------
// Sampling

bool AnyButton(int button)
{
    if (inject::Button(button))
        return true;
    for (const Pad& p : g_pads)
        if (sdl.SDL_GameControllerGetButton(p.controller, static_cast<SDL_GameControllerButton>(button)))
            return true;
    return false;
}

float AxisValue(SDL_GameController* gc, SDL_GameControllerAxis axis)
{
    float v = sdl.SDL_GameControllerGetAxis(gc, axis) / 32767.0f;
    return v < -1.0f ? -1.0f : v;
}

float MaxTrigger(SDL_GameControllerAxis axis)
{
    float best = inject::Button(axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? kInputLeftTrigger : kInputRightTrigger) ? 1.0f
                                                                                                                 : 0.0f;
    for (const Pad& p : g_pads) {
        float v = AxisValue(p.controller, axis);
        if (v > best)
            best = v;
    }
    return best;
}

// The stick with the largest deflection across all connected pads.
void ReadStick(int stick, float* x, float* y)
{
    SDL_GameControllerAxis ax = stick == 0 ? SDL_CONTROLLER_AXIS_LEFTX : SDL_CONTROLLER_AXIS_RIGHTX;
    SDL_GameControllerAxis ay = stick == 0 ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_RIGHTY;
    inject::Stick(stick, x, y);
    float best = *x * *x + *y * *y;
    for (const Pad& p : g_pads) {
        float px = AxisValue(p.controller, ax), py = AxisValue(p.controller, ay);
        float m = px * px + py * py;
        if (m > best) {
            best = m;
            *x = px;
            *y = py;
        }
    }
}

// ---------------------------------------------------------------------------
// Stage-2 prototype

constexpr float kPi = 3.14159265f;
constexpr float kRadToDeg = 180.0f / kPi;

// Wraps to [-pi, pi]. Not a subtract loop: on a large float, adding 2*pi changes nothing.
float WrapPi(float a)
{
    return std::isfinite(a) ? remainderf(a, 2 * kPi) : 0.0f;
}

void WarpTo(int x, int y)
{
    sdl.SDL_WarpMouseInWindow(g_window, x, y);
    g_lastWarpX = x;
    g_lastWarpY = y;
    g_recentWarps[g_recentWarpNext][0] = x;
    g_recentWarps[g_recentWarpNext][1] = y;
    g_recentWarpNext = (g_recentWarpNext + 1) % 4;
}

bool IsRecentWarp(int x, int y)
{
    for (const auto& w : g_recentWarps)
        if (abs(w[0] - x) <= 3 && abs(w[1] - y) <= 3)
            return true;
    return false;
}

void SetPrototype(bool on)
{
    if (on == g_proto)
        return;
    if (on) {
        const game::Caps& c = game::Refresh();
        if (!c.player || !c.camera) {
            Log("proto: can't start yet (player=%d camera=%d); load a game first", c.player, c.camera);
            return;
        }
        for (int d = 0; d < kDirectionCount; ++d) {
            if (g_moveHeld[d])
                Release(g_cfg.move[d]);
            g_moveHeld[d] = false;
        }
    } else {
        ProtoReleaseMovement();
        DropTarget();
    }
    if (on && g_cfg.diag.targeting)
        g_pickHooked = game::InstallPickHook();
    game::SetRadialCentered(on); // radials open mid-screen, where the stick steers them
    g_proto = on;
    g_protoState = nullptr;
    Log("proto: prototype mode %s (left stick = direct movement [%s], targeting %s)", on ? "ON" : "OFF",
        g_cfg.diag.driveMode == DriveMode::Steer ? "steer" : "facing",
        !g_cfg.diag.targeting ? "off" : g_pickHooked ? "on" : "unavailable");
}

// True when the object's aim point is on screen.
bool OnScreen(const game::ObjectInfo& o)
{
    game::Vec3 aim = o.pos;
    aim.z += o.type == game::kTypeCreature ? 1.0f : 0.5f;
    float sx, sy;
    return game::WorldToScreen(aim, &sx, &sy) && sx > 0.0f && sx < 1.0f && sy > 0.0f && sy < 1.0f;
}

// Targeting (docs/STATUS.md, targeting v2). While the right stick drives the camera,
// the game's world pick is patched to report the controller's target, so the game
// highlights and names it itself, A (left click) does its default action on it and
// X (right click) opens its radial menu. The pointer stays parked mid-screen, which
// is the character, never a GUI panel.
// Radial mode: a native radial menu is open (the game centres it on screen while
// prototype mode runs). The right stick snaps the pointer onto the option nearest
// its direction, skipping empty and disabled slots, so the game highlights exactly
// one option by its own angle test; A clicks it, B goes back to the parent radial
// (or closes the top one). Movement and the camera wait.
bool RadialOpen()
{
    return g_proto && g_stickCamera && game::RadialDepth() > 0;
}

// Screen direction (y down) of radial slot k: 0 = down, then anticlockwise in 45° steps.
void RadialSlotDir(int k, float* x, float* y)
{
    float a = k * 0.78539816f;
    *x = sinf(a);
    *y = g_radialFlip == 1 ? -cosf(a) : cosf(a);
}

float AngleBetween(float ax, float ay, float bx, float by)
{
    return fabsf(atan2f(ax * by - ay * bx, ax * bx + ay * by));
}

void UpdateRadial()
{
    int depth = game::RadialDepth();
    if (depth != g_radialDepth) { // opened, or a sub-radial: start from the middle
        g_radialDepth = depth;
        g_radialSlot = -1;
        g_radialCheck = -1;
    }
    uint8_t shown = 0xff, enabled = 0xff;
    int selected = -1;
    bool haveSlots = game::RadialSlots(&shown, &enabled, &selected);
    if (!haveSlots)
        shown = enabled = 0xff; // unknown layout: snap to all eight directions
    // Orientation check, once: when the game's highlight moves after we snap, it
    // should land on our slot. If it lands on the vertical mirror instead, the game's
    // y axis runs the other way. Slots 2 and 6 (left/right) look the same either way.
    if (haveSlots && g_radialFlip < 0 && g_radialCheck >= 0 && selected != g_radialCheckFrom) {
        int mirror = (12 - g_radialCheck) & 7;
        if (selected == g_radialCheck || selected == mirror) {
            g_radialFlip = selected == g_radialCheck ? 0 : 1;
            Log("radial: slot order %s (pointed at %d, game chose %d)", g_radialFlip ? "flipped" : "as expected",
                g_radialCheck, selected);
            if (g_radialFlip)
                g_radialSlot = -1; // re-snap with the corrected order
        }
        g_radialCheck = -1;
    }
    float x, y;
    ReadStick(1, &x, &y); // right stick only; the left one just waits
    float m = sqrtf(x * x + y * y);
    uint8_t choosable = enabled ? enabled : shown;
    if (m > 0.5f && choosable) { // the pointer stays on the last option when the stick lets go, ready for A
        x /= m;
        y /= m;
        int best = -1;
        float bestAngle = 10.0f, curAngle = 10.0f;
        for (int k = 0; k < 8; ++k) {
            if (!(choosable & (1 << k)))
                continue;
            float sx, sy;
            RadialSlotDir(k, &sx, &sy);
            float a = AngleBetween(x, y, sx, sy);
            if (a < bestAngle) {
                bestAngle = a;
                best = k;
            }
            if (k == g_radialSlot)
                curAngle = a;
        }
        // A little stickiness (10°) so the choice doesn't flicker between two slots
        // when the stick sits on the line between them.
        if (best >= 0 && (g_radialSlot < 0 || curAngle > bestAngle + 0.1745f) && best != g_radialSlot) {
            g_radialSlot = best;
            if (g_radialFlip < 0 && haveSlots && best != 2 && best != 6 && selected != best &&
                selected != ((12 - best) & 7)) {
                g_radialCheck = best;
                g_radialCheckFrom = selected;
            }
        }
    } else if (g_radialSlot >= 0 && haveSlots && !(choosable & (1 << g_radialSlot))) {
        g_radialSlot = -1; // the slot went away (menu rebuilt)
    }
    int w = 0, h = 0;
    sdl.SDL_GetWindowSize(g_window, &w, &h);
    if (w <= 0 || h <= 0)
        return;
    // The game counts the pointer as choosing a slot 56-150 GUI pixels from the centre.
    float scale = game::GuiScale();
    float r = 100.0f * (scale > 0.0f ? scale : 1.0f);
    float dx = 0.0f, dy = 0.0f;
    if (g_radialSlot >= 0)
        RadialSlotDir(g_radialSlot, &dx, &dy);
    int px = static_cast<int>(lroundf(w * 0.5f + dx * r)), py = static_cast<int>(lroundf(h * 0.5f + dy * r));
    g_cursorX = static_cast<float>(px);
    g_cursorY = static_cast<float>(py);
    g_parked = true; // clicks land at g_cursorX/Y
    if (abs(px - g_lastWarpX) > 1 || abs(py - g_lastWarpY) > 1)
        WarpTo(px, py);
}

// Dialogue mode: a conversation window is open. The overlay shows who's speaking (with
// their portrait) and what they say (the right stick scrolls a long line) and lists the
// replies, and the game's own window is moved out of sight; the d-pad
// or left stick moves the choice (held, it repeats), A picks it through the game's
// own reply handler, B keeps its binding (Escape ends the conversation). X, Y and the
// d-pad's other uses wait, and so do movement and the camera.
struct DialogState {
    int count = -1; // replies on offer; -1 = no conversation
    std::string speaker, line; // UTF-8, for the overlay
    float scroll;              // the line's scroll, overlay pixels at 1080p
    std::shared_ptr<const overlay::Image> portrait;
    std::vector<overlay::ListItem> replies;
    int selected;
    int navDir;     // -1 up, 1 down, 0 none: the direction being held
    float navTimer; // until the held direction steps again
} g_dialog;

constexpr float kNavRepeatDelay = 0.35f, kNavRepeatEvery = 0.12f;
constexpr int kPortraitSize = 4; // L, 128x200: crisp at the overlay's size

// The speaker's portrait, loaded once per resref (a failed load is remembered too).
std::shared_ptr<const overlay::Image> SpeakerPortrait()
{
    static std::map<std::string, std::shared_ptr<const overlay::Image>> s_cache;
    char resref[32];
    if (!game::DialogPortrait(kPortraitSize, resref, sizeof(resref)))
        return nullptr;
    auto it = s_cache.find(resref);
    if (it != s_cache.end())
        return it->second;
    auto img = std::make_shared<overlay::Image>();
    img->key = resref;
    if (game::LoadTextureRgba(resref, &img->rgba, &img->width, &img->height)) {
        // Portrait textures are padded to a power of two (L: a 128x200 picture in a
        // 128x256 texture); the game's own UI shows only the 64:100 picture at the top.
        int picture = (img->width * 100 + 32) / 64;
        if (picture < img->height) {
            img->height = picture;
            img->rgba.resize(static_cast<size_t>(img->width) * picture * 4);
        }
        Log("dialog: portrait %s (%dx%d)", resref, img->width, img->height);
    } else {
        Log("dialog: portrait %s couldn't be loaded (not a true-colour TGA?)", resref);
        img = nullptr;
    }
    return s_cache[resref] = img;
}

bool DialogOpen()
{
    return g_dialog.count >= 0;
}

// Moves the choice to the next reply the game will take, in `dir`, wrapping.
void StepDialog(int dir)
{
    int n = static_cast<int>(g_dialog.replies.size());
    for (int k = 1; k <= n; ++k) {
        int i = ((g_dialog.selected + dir * k) % n + n) % n;
        if (g_dialog.replies[i].enabled) {
            g_dialog.selected = i;
            return;
        }
    }
}

// Reads the open conversation's replies; a new set puts the choice back on the first.
void RefreshDialog()
{
    int count = g_proto ? game::DialogReplyCount() : -1;
    if ((count >= 0) != DialogOpen())
        Log("dialog: %s", count >= 0 ? "conversation open" : "conversation closed");
    std::vector<overlay::ListItem> replies;
    for (int i = 0; i < count; ++i) {
        char raw[1024];
        overlay::ListItem it;
        if (!game::DialogReply(i, raw, sizeof(raw), &it.enabled))
            raw[0] = 0;
        it.text = text::Display(raw);
        replies.push_back(it);
    }
    char speaker[256], line[8192];
    if (count < 0 || !game::DialogText(speaker, sizeof(speaker), line, sizeof(line)))
        speaker[0] = line[0] = 0;
    std::string shown = text::Display(line);
    if (shown != g_dialog.line) {
        g_dialog.line = shown;
        g_dialog.scroll = 0;
        g_dialog.portrait = shown.empty() ? nullptr : SpeakerPortrait(); // a new line may be a new speaker
        if (!shown.empty())
            Log("dialog: %s says '%.200s%s'", speaker[0] ? speaker : "?", line, strlen(line) > 200 ? "..." : "");
    }
    g_dialog.speaker = text::Display(speaker);

    bool changed = replies.size() != g_dialog.replies.size();
    for (size_t i = 0; !changed && i < replies.size(); ++i)
        changed = replies[i].text != g_dialog.replies[i].text || replies[i].enabled != g_dialog.replies[i].enabled;
    g_dialog.count = count;
    if (changed) {
        g_dialog.replies = replies;
        g_dialog.selected = 0;
        if (!replies.empty() && !replies[0].enabled)
            StepDialog(1);
        if (count >= 0)
            Log("dialog: %d repl%s", count, count == 1 ? "y" : "ies");
    }
}

// The d-pad and A while a conversation is open; true if the button was used here.
// A press that began before the window opened finishes through its own binding.
bool DialogButton(int i, bool down, bool wasDown)
{
    bool used = i == SDL_CONTROLLER_BUTTON_A || i == SDL_CONTROLLER_BUTTON_X || i == SDL_CONTROLLER_BUTTON_Y ||
                (i >= SDL_CONTROLLER_BUTTON_DPAD_UP && i <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    if (!used || g_latched[i])
        return false;
    if (i == SDL_CONTROLLER_BUTTON_A && down && !wasDown && !g_dialog.replies.empty()) {
        int sel = g_dialog.selected;
        bool ok = g_dialog.replies[sel].enabled && game::DialogChoose(sel);
        Log("dialog: chose reply %d%s", sel + 1, ok ? "" : " (not taken)");
    }
    return true;
}

// Up/down from the d-pad or the left stick: a step on the first push, then repeats.
// The right stick scrolls the NPC's line when it's too long for its box.
void UpdateDialogNav(float dt)
{
    float sx, sy;
    ReadStick(g_cfg.cursorStick < 0 ? 1 : g_cfg.cursorStick, &sx, &sy);
    if (fabsf(sy) > g_cfg.cursorDeadzone) {
        float max = overlay::BodyScrollMax();
        g_dialog.scroll += sy * 600.0f * dt;
        g_dialog.scroll = g_dialog.scroll < 0 ? 0 : g_dialog.scroll > max ? max : g_dialog.scroll;
    }
    float x, y;
    ReadStick(g_cfg.moveStick < 0 ? 0 : g_cfg.moveStick, &x, &y);
    int dir = g_inputDown[SDL_CONTROLLER_BUTTON_DPAD_UP] ? -1 : g_inputDown[SDL_CONTROLLER_BUTTON_DPAD_DOWN] ? 1
              : y < -0.5f && fabsf(y) > fabsf(x)                                                     ? -1
              : y > 0.5f && fabsf(y) > fabsf(x)                                                      ? 1
                                                                                                     : 0;
    if (dir != g_dialog.navDir) {
        g_dialog.navDir = dir;
        g_dialog.navTimer = kNavRepeatDelay;
        if (dir)
            StepDialog(dir);
    } else if (dir && (g_dialog.navTimer -= dt) <= 0) {
        g_dialog.navTimer = kNavRepeatEvery;
        StepDialog(dir);
    }
}

// Panel mode: one of the game's own menu panels is open (inventory, character sheet, a
// popup...). Its controls are found by hit-testing the screen a few hundred points a
// frame; the d-pad or left stick moves focus between them (held, it repeats) and the
// pointer sits on the focused one, so the game shows its own highlight and tooltip and
// A / X click there. Holding A while moving drags. B keeps Escape. The right stick moves
// the pointer freely; focus follows it. Movement and targeting wait.
struct PanelState {
    void* panel;  // the newest open menu panel, kept while a radial or conversation is up over it
    bool active;  // panel mode ran last frame
    panelnav::Scanner scan;
    std::vector<panelnav::Control> controls;
    int focus = -1;
    void* focusObj;
    int focusCell = -1;
    float rescanIn;   // seconds until the next scan starts
    Uint64 scanStart; // performance counter when the running scan began
    bool scannedOnce; // for this panel
    int navX, navY;   // the direction being held
    float navTimer;
    void* tab;        // the tab last clicked (null: assume the first)
    float wheelIn;    // seconds until the right stick scrolls another notch
    int clickIn;      // frames until a queued click (after the pointer has moved there)
} g_panel;

constexpr int kScanStep = 12, kScanBudget = 700; // ~0.4 s to cover 1080p
constexpr float kRescanEvery = 1.5f, kRescanAfterClick = 0.25f;

// A menu panel is up with nothing over it; it's scanned while this holds.
bool PanelUp()
{
    return g_panel.panel && !DialogOpen() && !RadialOpen();
}

// Panel mode: a menu panel is up and has controls we can see. One whose scan found none
// (a NUI window such as the module browser, drawn over a legacy panel) keeps the
// ordinary pointer.
bool PanelOpen()
{
    return PanelUp() && !(g_panel.scannedOnce && g_panel.controls.empty());
}

// A focusable control of a menu panel at screen (x, y): the hit test works in GUI
// coordinates, whose y runs up from the bottom of the window.
void* PanelHit(void* ctx, int x, int y)
{
    int h = *static_cast<int*>(ctx);
    void* obj = game::GuiHitTest(x, h - y);
    if (!obj || !game::IsFocusable(obj))
        return nullptr;
    void* root = game::ControlRoot(obj);
    return root && root != obj && !game::IsHudPanel(root) ? obj : nullptr;
}

// Inventory-style grids are one control; their model names give the cell count.
bool PanelGrid(void*, void* obj, int* cols, int* rows)
{
    void* model = nullptr;
    char name[64];
    return game::Read(obj, 0x10, &model) && game::ModelName(model, name, sizeof(name)) &&
           sscanf(name, "INV_BOX_%dX%d", cols, rows) == 2;
}

void SetPanelFocus(int i)
{
    g_panel.focus = i;
    if (i < 0)
        return;
    const panelnav::Control& c = g_panel.controls[i];
    const char* cls = game::ClassForVtable(c.obj);
    Log("panel: focus %d/%d %s cell %d at (%.0f, %.0f)-(%.0f, %.0f)", i, static_cast<int>(g_panel.controls.size()),
        cls ? cls : "?", c.cell, c.r.x0, c.r.y0, c.r.x1, c.r.y1);
    g_panel.focusObj = c.obj;
    g_panel.focusCell = c.cell;
    int x = static_cast<int>(lroundf(c.r.CenterX())), y = static_cast<int>(lroundf(c.r.CenterY()));
    g_cursorX = static_cast<float>(x);
    g_cursorY = static_cast<float>(y);
    g_cursorActive = false;
    g_parked = true; // clicks land on the control
    WarpTo(x, y);
}

int PointerX()
{
    int x = 0, y = 0;
    sdl.SDL_GetMouseState(&x, &y);
    return x;
}

int PointerY()
{
    int x = 0, y = 0;
    sdl.SDL_GetMouseState(&x, &y);
    return y;
}

// Follows the open menu panel and keeps its control list fresh.
void RefreshPanel(float dt)
{
    // In prototype mode, or out of a game (main menu, loading a game, character
    // creation) where the stock bindings have nothing better to offer.
    void* panel = g_proto || !game::Current().player ? game::OpenMenuPanel() : nullptr;
    if (panel != g_panel.panel) {
        if (panel) {
            Log("panel: %s open", game::ClassForVtable(panel) ? game::ClassForVtable(panel) : "a menu panel");
            g_panel.scannedOnce = false;
            g_panel.rescanIn = 0;
            g_panel.scan = panelnav::Scanner{};
            g_panel.focus = -1; // a new panel starts fresh
            g_panel.focusObj = nullptr;
            g_panel.tab = nullptr;
            g_panel.clickIn = 0;
        } else {
            Log("panel: menu closed");
            g_panel.controls.clear();
            g_panel.focus = -1;
            g_panel.focusObj = nullptr;
        }
        g_panel.panel = panel;
    }
    if (!PanelUp()) {
        g_panel.active = false;
        return;
    }
    if (!g_panel.active) { // starting, or back from a radial or conversation over the panel
        g_panel.active = true;
        DropTarget(); // the world waits while a menu is up
        g_panel.rescanIn = 0;
        if (g_panel.focus >= 0 && g_panel.focus < static_cast<int>(g_panel.controls.size()))
            SetPanelFocus(g_panel.focus); // a radial moved the pointer: put it back
    }

    if (g_panel.clickIn > 0 && --g_panel.clickIn == 0) { // a tab: the pointer got there last frame
        QueueMouseButton(SDL_BUTTON_LEFT, true);
        QueueMouseButton(SDL_BUTTON_LEFT, false);
        g_panel.rescanIn = kRescanAfterClick;
    }
    int w = 0, h = 0;
    sdl.SDL_GetWindowSize(g_window, &w, &h);
    // A scan rewrites the GUI's hit cache; not while a button is down (a drag) or the
    // game is mid-click.
    bool buttonDown = g_mouseMask != 0;
    if (!g_panel.scan.Running() && !buttonDown && (g_panel.rescanIn -= dt) <= 0) {
        g_panel.scan.Start(w, h, kScanStep);
        g_panel.scanStart = sdl.SDL_GetPerformanceCounter();
    }
    if (g_panel.scan.Running() && !buttonDown) {
        bool done = g_panel.scan.Run(PanelHit, &h, kScanBudget);
        game::GuiHitTest(PointerX(), h - PointerY()); // leave the game's hover where the pointer is
        if (done) {
            g_panel.controls = g_panel.scan.Result(PanelGrid, nullptr);
            g_panel.rescanIn = g_panel.controls.empty() ? 2 * kRescanEvery : kRescanEvery; // NUI: less often
            if (!g_panel.scannedOnce) {
                g_panel.scannedOnce = true;
                double ms = double(sdl.SDL_GetPerformanceCounter() - g_panel.scanStart) * 1000.0 /
                            double(sdl.SDL_GetPerformanceFrequency());
                Log("panel: %d controls found in %.0f ms", static_cast<int>(g_panel.controls.size()), ms);
            }
            int keep = panelnav::Find(g_panel.controls, g_panel.focusObj, g_panel.focusCell);
            int firstCell = -1;
            for (int i = 0; i < static_cast<int>(g_panel.controls.size()) && firstCell < 0; ++i)
                if (g_panel.controls[i].cell == 0)
                    firstCell = i;
            // Otherwise a new panel starts on its first real control in reading order:
            // not a tab or a scroll bar, and wide enough to be a button or a list row
            // rather than a small widget (close button, sort toggles, check boxes).
            int firstMain = -1;
            for (int i = 0; i < static_cast<int>(g_panel.controls.size()) && firstMain < 0; ++i) {
                const panelnav::Control& c = g_panel.controls[i];
                const char* cls = game::ClassForVtable(c.obj);
                bool scroll = cls && (!strcmp(cls, "CGuiScrollbar") || !strcmp(cls, "CGuiTimerButton"));
                if (!game::IsTabButton(c.obj) && !scroll && c.r.x1 - c.r.x0 >= 160)
                    firstMain = i;
            }
            if (keep >= 0)
                g_panel.focus = keep; // same control: leave the pointer alone
            else if (!g_panel.focusObj && firstCell >= 0)
                SetPanelFocus(firstCell); // a new inventory-style panel: start on its grid
            else if (!g_panel.focusObj && firstMain >= 0)
                SetPanelFocus(firstMain);
            else // a new panel, or the focused control went away: what's nearest the pointer
                SetPanelFocus(panelnav::Nearest(g_panel.controls, static_cast<float>(PointerX()),
                                                static_cast<float>(PointerY())));
        }
    }
    // The right stick moved the pointer: focus is whatever it's over.
    if (g_cursorActive && !g_panel.controls.empty()) {
        int under = panelnav::Nearest(g_panel.controls, g_cursorX, g_cursorY);
        const panelnav::Rect* r = under >= 0 ? &g_panel.controls[under].r : nullptr;
        bool inside = r && g_cursorX >= r->x0 && g_cursorX < r->x1 && g_cursorY >= r->y0 && g_cursorY < r->y1;
        g_panel.focus = inside ? under : -1;
        if (inside) {
            g_panel.focusObj = g_panel.controls[under].obj;
            g_panel.focusCell = g_panel.controls[under].cell;
        }
    }
}

// The d-pad and left stick move focus: a step on the first push, then repeats.
void UpdatePanelNav(float dt)
{
    float x, y;
    ReadStick(g_cfg.moveStick < 0 ? 0 : g_cfg.moveStick, &x, &y);
    int dx = g_inputDown[SDL_CONTROLLER_BUTTON_DPAD_LEFT] ? -1 : g_inputDown[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] ? 1 : 0;
    int dy = g_inputDown[SDL_CONTROLLER_BUTTON_DPAD_UP] ? -1 : g_inputDown[SDL_CONTROLLER_BUTTON_DPAD_DOWN] ? 1 : 0;
    if (!dx && !dy && x * x + y * y > 0.5f * 0.5f) { // the stick, as 8 directions
        float a = atan2f(y, x), oct = roundf(a / 0.78539816f);
        dx = static_cast<int>(roundf(cosf(oct * 0.78539816f)));
        dy = static_cast<int>(roundf(sinf(oct * 0.78539816f)));
    }
    bool step = false;
    if (dx != g_panel.navX || dy != g_panel.navY) {
        g_panel.navX = dx;
        g_panel.navY = dy;
        g_panel.navTimer = kNavRepeatDelay;
        step = dx || dy;
    } else if ((dx || dy) && (g_panel.navTimer -= dt) <= 0) {
        g_panel.navTimer = kNavRepeatEvery;
        step = true;
    }
    if (!step || g_panel.controls.empty())
        return;
    int from = g_panel.focus;
    if (from < 0 || g_cursorActive) // after the free pointer: start from what's nearest it
        from = panelnav::Nearest(g_panel.controls, g_cursorX, g_cursorY);
    int to = panelnav::Step(g_panel.controls, from, static_cast<float>(dx), static_cast<float>(dy));
    SetPanelFocus(to >= 0 ? to : from);
    if (to < 0 && dy && !dx) { // nothing further that way: scroll whatever list it's in
        QueueMouseWheel(-dy);
        g_panel.rescanIn = kRescanAfterClick;
    }
}

// In camera mode the right stick scrolls (mouse wheel at the focus), faster the further
// it's pushed; after R3 it moves the pointer instead (UpdateCursor).
void UpdatePanelScroll(float dt)
{
    float x, y;
    ReadStick(g_cfg.cursorStick < 0 ? 1 : g_cfg.cursorStick, &x, &y);
    if (!g_stickCamera || fabsf(y) < 0.4f || fabsf(y) < fabsf(x)) {
        g_panel.wheelIn = 0;
        return;
    }
    if ((g_panel.wheelIn -= dt) > 0)
        return;
    g_panel.wheelIn = 0.22f - 0.14f * std::min(1.0f, fabsf(y));
    QueueMouseWheel(y < 0 ? 1 : -1);
    g_panel.rescanIn = kRescanAfterClick;
}

// LB / RB tapped on their own: the previous / next tab of the panel, in reading order.
// The tab last clicked counts as the current one; a new panel starts on its first.
void PanelTab(int dir)
{
    std::vector<int> tabs;
    for (int i = 0; i < static_cast<int>(g_panel.controls.size()); ++i)
        if (g_panel.controls[i].cell < 0 && game::IsTabButton(g_panel.controls[i].obj))
            tabs.push_back(i);
    int n = static_cast<int>(tabs.size());
    if (n == 0)
        return;
    // Along the strip they form (a row, or a column like the inventory's bags): the
    // selected tab sits a little proud of the others, so reading order would misplace it.
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (int i : tabs) {
        const panelnav::Rect& r = g_panel.controls[i].r;
        minX = std::min(minX, r.CenterX());
        maxX = std::max(maxX, r.CenterX());
        minY = std::min(minY, r.CenterY());
        maxY = std::max(maxY, r.CenterY());
    }
    bool row = maxX - minX >= maxY - minY;
    std::sort(tabs.begin(), tabs.end(), [&](int a, int b) {
        const panelnav::Rect &ra = g_panel.controls[a].r, &rb = g_panel.controls[b].r;
        return row ? ra.CenterX() < rb.CenterX() : ra.CenterY() < rb.CenterY();
    });
    int cur = 0;
    for (int k = 0; k < n; ++k)
        if (g_panel.controls[tabs[k]].obj == g_panel.tab)
            cur = k;
    int next = ((cur + dir) % n + n) % n;
    SetPanelFocus(tabs[next]);
    g_panel.tab = g_panel.controls[tabs[next]].obj;
    g_panel.clickIn = 2;
    Log("panel: tab %d of %d", next + 1, n);
}

// The d-pad while a menu panel is open; true if the button was used here.
bool PanelButton(int i, bool down, bool wasDown)
{
    if (i == SDL_CONTROLLER_BUTTON_A && down && !wasDown && g_panel.focus >= 0 &&
        g_panel.focus < static_cast<int>(g_panel.controls.size()) &&
        game::IsTabButton(g_panel.controls[g_panel.focus].obj))
        g_panel.tab = g_panel.controls[g_panel.focus].obj; // picked a tab by hand
    if (i >= SDL_CONTROLLER_BUTTON_DPAD_UP && i <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT && !g_latched[i])
        return true; // UpdatePanelNav reads it
    if ((i == SDL_CONTROLLER_BUTTON_A || i == SDL_CONTROLLER_BUTTON_X) && !down && wasDown)
        g_panel.rescanIn = kRescanAfterClick; // the click may have changed the panel
    return false;
}

bool TargetingActive()
{
    return g_proto && g_cfg.diag.targeting && g_stickCamera && !g_mouseTakeover && g_pickHooked;
}

struct Candidate {
    uint32_t oid;
    uint8_t type;
    float dist;  // metres
    float angle; // radians off the aim direction; + = to the left (anticlockwise)
    game::TargetInfo info;
};

// Where the player is aiming: the left stick while it's pushed, otherwise where the
// camera looks across the ground.
bool AimDirection(const game::ObjectInfo& pl, float* ax, float* ay)
{
    float fx = g_aimX, fy = g_aimY;
    game::Vec3 cam;
    if (!g_aimFromStick) {
        if (!game::CameraPosition(&cam))
            return false;
        fx = pl.pos.x - cam.x;
        fy = pl.pos.y - cam.y;
    }
    float fl = sqrtf(fx * fx + fy * fy);
    if (fl < 0.01f)
        return false;
    *ax = fx / fl;
    *ay = fy / fl;
    return true;
}

float RangeFor(const game::TargetInfo& info)
{
    return info.hostile ? g_cfg.diag.targetHostileRange : g_cfg.diag.targetInteractRange;
}

// Everything the player could target right now, at any angle. `slack` stretches the
// ranges, so a current target isn't dropped the moment it crosses the line.
std::vector<Candidate> TargetCandidates(float slack)
{
    struct Ctx {
        game::ObjectInfo pl;
        float ax, ay, slack;
        std::vector<Candidate> list;
    } ctx{};
    if (!game::PlayerInfo(&ctx.pl) || !AimDirection(ctx.pl, &ctx.ax, &ctx.ay))
        return {};
    ctx.slack = slack;
    game::ForEachObject(
        [](void* p, const game::ObjectInfo& o) {
            auto* c = static_cast<Ctx*>(p);
            if (o.area != c->pl.area || o.oid == c->pl.oid)
                return;
            game::TargetInfo info;
            if (!game::TargetInfoFor(o, c->pl.oid, &info) || !info.usable || info.associate)
                return;
            float dx = o.pos.x - c->pl.pos.x, dy = o.pos.y - c->pl.pos.y;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist > RangeFor(info) * c->slack || !OnScreen(o))
                return;
            float angle = WrapPi(atan2f(dy, dx) - atan2f(c->ay, c->ax));
            c->list.push_back({o.oid, o.type, dist, angle, info});
        },
        &ctx);
    return ctx.list;
}

// Lower is better: degrees off the aim plus 4 per metre; hostiles first.
float TargetScore(const Candidate& c)
{
    float s = fabsf(c.angle) * kRadToDeg + 4.0f * c.dist;
    return c.info.hostile ? s * 0.7f : s;
}

void SetTarget(const Candidate* c, bool locked, const char* why)
{
    uint32_t oid = c ? c->oid : game::kInvalidOid;
    if (c && (oid != g_targetOid || locked != g_targetLocked))
        Log("target: %s %s oid=%08x dist=%.1f angle=%.0f%s", why, game::ObjectTypeName(c->type), oid, c->dist,
            c->angle * kRadToDeg, c->info.hostile ? " hostile" : "");
    else if (!c && g_targetOid != game::kInvalidOid)
        Log("target: none (%s)", why);
    g_targetOid = oid;
    g_targetType = c ? c->type : 0;
    g_targetHostile = c && c->info.hostile;
    g_targetLocked = c && locked;
    g_challengerOid = game::kInvalidOid;
    g_challengerTime = 0;
}

// Keeps the pointer at the middle of the window, where clicks land on the world.
void ParkPointer()
{
    int w = 0, h = 0;
    sdl.SDL_GetWindowSize(g_window, &w, &h);
    if (w <= 0 || h <= 0)
        return;
    int cx = w / 2, cy = h / 2, mx = 0, my = 0;
    sdl.SDL_GetMouseState(&mx, &my);
    g_cursorX = static_cast<float>(cx);
    g_cursorY = static_cast<float>(cy);
    if (!g_parked || abs(mx - cx) > 2 || abs(my - cy) > 2)
        WarpTo(cx, cy);
    g_parked = true;
}

void UpdateTargeting(float dt)
{
    if (!TargetingActive()) {
        if (g_targetOid != game::kInvalidOid)
            SetTarget(nullptr, false, "targeting off");
        game::SetPickTarget(false, game::kInvalidOid);
        g_parked = false;
        return;
    }
    ParkPointer();
    g_softTimer -= dt;
    if (g_softTimer <= 0) {
        g_softTimer = 0.1f;
        std::vector<Candidate> list = TargetCandidates(1.3f);
        const Candidate *current = nullptr, *best = nullptr;
        float cone = g_cfg.diag.targetCone / kRadToDeg;
        for (const Candidate& c : list) {
            if (c.oid == g_targetOid)
                current = &c;
            if (fabsf(c.angle) <= cone && c.dist <= RangeFor(c.info) && (!best || TargetScore(c) < TargetScore(*best)))
                best = &c;
        }
        if (g_targetLocked) {
            if (!current)
                SetTarget(nullptr, false, "locked target out of reach");
        } else if (!current || fabsf(current->angle) > cone * 1.5f) {
            SetTarget(best, false, best ? "soft" : "nothing in front");
        } else if (best && best->oid != current->oid && TargetScore(*best) < 0.8f * TargetScore(*current)) {
            // A better target has to stay better for a moment before it takes over.
            if (g_challengerOid != best->oid) {
                g_challengerOid = best->oid;
                g_challengerTime = 0;
            }
            g_challengerTime += 0.1f;
            if (g_challengerTime >= 0.3f - 1e-3f)
                SetTarget(best, false, "soft");
        } else {
            g_challengerOid = game::kInvalidOid;
            g_challengerTime = 0;
        }
    }
    game::SetPickTarget(true, g_targetOid);
}

// D-pad left/right: step to the next target to the left or right of the current one
// (around the aim direction, within range) and lock it.
void CycleTarget(int dir)
{
    std::vector<Candidate> list = TargetCandidates(1.0f);
    if (list.empty()) {
        Log("target: nothing in reach to cycle to");
        return;
    }
    // Left to right: larger (anticlockwise) angles first.
    std::sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) { return a.angle > b.angle; });
    int n = static_cast<int>(list.size()), idx = -1;
    for (int i = 0; i < n; ++i)
        if (list[i].oid == g_targetOid)
            idx = (i + dir + n) % n;
    if (idx < 0) { // nothing targeted: start with the one nearest the aim
        for (int i = 0; i < n; ++i)
            if (idx < 0 || fabsf(list[i].angle) < fabsf(list[idx].angle))
                idx = i;
    }
    SetTarget(&list[idx], true, "locked");
    game::SetPickTarget(true, g_targetOid);
}

// Logs client vs server position every 50 ms for 1.2 s after a release, relative to
// the release point, to show what the character does when it stops.
void TraceStop(float dt)
{
    if (g_traceTime < 0)
        return;
    g_traceTime += dt;
    if (g_traceTime < g_traceNext)
        return;
    g_traceNext = g_traceTime + 0.05f;
    if (g_traceTime > 1.2f)
        g_traceTime = -1;
    game::MoveTrace t;
    if (!game::ReadMoveTrace(game::PlayerOid(), &t))
        return;
    auto rel = [](const game::Vec3& p, float* fwd, float* side) {
        // Along / across the direction the character was facing at release.
        float dx = p.x - g_releasePos.x, dy = p.y - g_releasePos.y;
        float fx = g_releaseFacing.x, fy = g_releaseFacing.y;
        *fwd = dx * fx + dy * fy;
        *side = dx * fy - dy * fx;
    };
    float cf, cs, pf, ps, sf = 0, ss = 0;
    rel(t.client, &cf, &cs);
    rel(t.predictBase, &pf, &ps);
    if (t.haveServer)
        rel(t.server, &sf, &ss);
    Log("trace: +%3.0fms client %+.2f/%+.2f base %+.2f/%+.2f server %+.2f/%+.2f action %d anim %u",
        (g_traceTime < 0 ? 1.2f : g_traceTime) * 1000.0f, cf, cs, pf, ps, sf, ss, t.serverAction, t.animation);
}

// After the stick is released: the server's drive action would carry the character on
// to its waypoint (~0.4 s ahead), so end it where the character stands. Repeats briefly
// in case a drive update already in flight re-adds the action.
void UpdateProtoStop(float dt)
{
    TraceStop(dt);
    if (g_stopTimer > 0) {
        g_stopTimer -= dt;
        float gap = 0;
        game::DriveStop r = game::StopServerDrive(game::PlayerOid(), g_releasePos, &gap);
        if (r == game::DriveStop::Stopped && !g_stopped) {
            g_stopped = true;
            Log("proto: stopped the server's drive action at release (server was %.2f m from the client)", gap);
        } else if (r == game::DriveStop::Unavailable) {
            static bool logged;
            if (!logged) {
                logged = true;
                Log("proto: can't reach the game's server (remote server?); the character finishes its step");
            }
            g_stopTimer = 0;
        }
    }
    // The client draws the character a frame or two past the stop point; pull the
    // server (and the client's server-confirmed position) to it instead of letting
    // the client ease back with no animation.
    if (g_syncTimer > 0) {
        g_syncTimer -= dt;
        float closed = game::SyncServerToClient(game::PlayerOid(), 0.5f);
        if (closed > 0.005f && ++g_syncCount <= 3) // a repeat means something keeps pulling it back
            Log("proto: matched the server to the drawn stop point (%.2f m)", closed);
    }
    if (g_coastTimer > 0 && (g_coastTimer -= dt) <= 0) {
        game::ObjectInfo now;
        if (game::PlayerInfo(&now)) {
            float dx = now.pos.x - g_releasePos.x, dy = now.pos.y - g_releasePos.y;
            Log("proto: coasted %.2f m after release%s", sqrtf(dx * dx + dy * dy),
                g_stopped ? " (server drive stopped)" : "");
        }
    }
}

void UpdateProtoMove(float dt)
{
    float x, y;
    ReadStick(g_cfg.moveStick < 0 ? 0 : g_cfg.moveStick, &x, &y);
    if (RadialOpen() || DialogOpen() || PanelOpen())
        x = y = 0; // the stick is choosing a radial option, a reply or a control; stand still
    float mag = sqrtf(x * x + y * y);
    game::ObjectInfo pl{};
    game::Vec3 cam{};
    // Log each change in why the stick is or isn't driving, so a silent stall shows up.
    static const char kDriving[] = "driving";
    const char* state = mag <= g_cfg.moveDeadzone   ? nullptr
                        : !game::PlayerInfo(&pl)      ? "no player object"
                        : !game::CameraPosition(&cam) ? "no camera position"
                                                      : kDriving;
    if (state != g_protoState) {
        if (state) {
            Log("proto: stick (%.2f, %.2f) -> %s", x, y, state);
            g_stopTimer = g_syncTimer = g_coastTimer = 0;
        } else {
            Log("proto: stick released");
            game::ObjectInfo now;
            if (g_protoState == kDriving && game::PlayerInfo(&now)) {
                g_stopTimer = g_cfg.diag.stopOnRelease ? 0.25f : 0.0f;
                g_syncTimer = g_cfg.diag.stopOnRelease ? 0.6f : 0.0f;
                g_syncCount = 0;
                g_coastTimer = 0.75f;
                g_releasePos = now.pos;
                g_releaseFacing = now.facing;
                g_stopped = false;
                if (g_cfg.diag.traceStops) {
                    g_traceTime = 0;
                    g_traceNext = 0;
                    TraceStop(0); // the moment of release
                }
            }
        }
        g_protoState = state;
    }
    if (state != kDriving) {
        g_aimFromStick = false;
        ProtoReleaseMovement();
        UpdateProtoStop(dt);
        return;
    }
    // Camera-relative: "up" on the stick is away from the camera across the ground.
    float fx = pl.pos.x - cam.x, fy = pl.pos.y - cam.y;
    float fl = sqrtf(fx * fx + fy * fy);
    if (fl < 0.01f) {
        fx = pl.facing.x;
        fy = pl.facing.y;
        fl = sqrtf(fx * fx + fy * fy);
        if (fl < 0.01f)
            return;
    }
    fx /= fl;
    fy /= fl;
    const DiagConfig& d = g_cfg.diag;
    if (d.lockFrame) {
        if (!g_frameLocked) {
            g_frameLocked = true;
            g_lockedFwdX = fx;
            g_lockedFwdY = fy;
        }
        fx = g_lockedFwdX;
        fy = g_lockedFwdY;
    }
    // World direction = forward * stick-up + right * stick-right, right = (fy, -fx).
    float dx = fx * -y + fy * x;
    float dy = fy * -y - fx * x;
    float heading = atan2f(dy, dx);
    float facing = atan2f(pl.facing.y, pl.facing.x);
    float err = WrapPi(heading - facing);
    g_aimFromStick = true; // targeting aims where the stick points
    g_aimX = dx;
    g_aimY = dy;
    g_mouseTakeover = false;

    bool walk = d.walkThreshold > 0 && mag < d.walkThreshold;
    bool useFacing = d.driveMode == DriveMode::Facing && game::Current().setFacing;
    if (d.driveMode == DriveMode::Facing && !useFacing && !g_steerFallbackLogged) {
        g_steerFallbackLogged = true;
        Log("proto: facing drive unavailable on this build; steering with the turn keys instead");
    }
    if (useFacing) {
        game::SetFacing(pl.ptr, {dx, dy, 0.0f});
        // The stock forward key is DriveModeForwardRun, which always runs, and Shift
        // doesn't slow drive mode. Walking is the separate DriveModeForward action.
        bool switched = walk != g_walkDriving;
        if (walk) {
            if (!g_walkDriving)
                g_walkDriving = game::InputAction(game::kActionDriveForwardWalk, true);
            ProtoHold(0, g_walkDriving ? nullptr : &g_cfg.move[kUp]);
        } else {
            ProtoHold(0, &g_cfg.move[kUp]); // run first, so the walk release doesn't leave a gap
            ReleaseWalkDrive();
        }
        ProtoHold(1, nullptr);
        ProtoHold(2, nullptr);
        if (switched)
            game::ForceDriveSend(); // send the new pace now, not in a quarter second
        Uint32 now = sdl.SDL_GetTicks();
        if (d.forceSendDegrees > 0 &&
            (!g_haveForcedHeading ||
             fabsf(WrapPi(heading - g_lastForcedHeading)) > d.forceSendDegrees / kRadToDeg) &&
            now - g_lastForceTicks >= 80) {
            game::ForceDriveSend();
            g_haveForcedHeading = true;
            g_lastForcedHeading = heading;
            g_lastForceTicks = now;
        }
    } else {
        ProtoHold(1, walk ? &g_walkAction : nullptr);
        float tol = d.steerTolerance / kRadToDeg;
        ProtoHold(0, fabsf(err) < 1.75f ? &g_cfg.move[kUp] : nullptr); // turn on the spot past ~100 degrees
        ProtoHold(2, err > tol ? &g_cfg.move[kLeft] : err < -tol ? &g_cfg.move[kRight] : nullptr);
    }

    if (!g_protoHavePos) {
        g_protoHavePos = true;
        g_protoLastPos = pl.pos;
        g_protoLogTimer = 0;
        return;
    }
    g_protoLogTimer += dt;
    if (g_protoLogTimer < 0.5f)
        return;
    float mx = pl.pos.x - g_protoLastPos.x, my = pl.pos.y - g_protoLastPos.y;
    float moved = sqrtf(mx * mx + my * my);
    float yaw = 0, pitch = 0, dist = 0, fov = 0;
    game::CameraAngles(&yaw, &pitch, &dist, &fov);
    int ds[8] = {};
    game::ReadDriveState(ds);
    Log("proto: stick=(%.2f, %.2f) %s want=%.0f facing=%.0f err=%.0f | moved %.0f deg at %.2f m/s | "
        "cam yaw=%.3f pitch=%.3f bearing=%.0f | drive=[%d %d %d %d %d %d %d %d]",
        x, y, walk ? "walk" : "run", heading * kRadToDeg, facing * kRadToDeg, err * kRadToDeg,
        moved > 0.01f ? atan2f(my, mx) * kRadToDeg : 0.0f, moved / g_protoLogTimer, yaw, pitch,
        atan2f(fy, fx) * kRadToDeg, ds[0], ds[1], ds[2], ds[3], ds[4], ds[5], ds[6], ds[7]);
    g_protoLastPos = pl.pos;
    g_protoLogTimer = 0;
}

void UpdateDiagCombo(float dt)
{
    bool both = AnyButton(SDL_CONTROLLER_BUTTON_LEFTSTICK) && AnyButton(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    if (both) {
        g_toggleArmed = false; // R3 as part of the combo doesn't also flip the right stick
        if (g_comboTime < 0) {
            g_comboTime = 0;
            g_comboToggled = false;
        }
        g_comboGap = 0;
        g_comboTime += dt;
        if (!g_comboToggled && g_comboTime >= 0.8f) {
            g_comboToggled = true;
            g_protoToggleRequested = true;
        }
    } else if (g_comboTime >= 0) {
        g_comboGap += dt;
        if (g_comboGap < 0.15f) // stick clicks often drop out for a frame while held
            return;
        if (!g_comboToggled)
            g_snapshotRequested = true;
        g_comboTime = -1.0f;
    }
}

// ---------------------------------------------------------------------------
// Per-frame update

void UpdateButtons()
{
    bool down[kInputCount];
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
        down[b] = AnyButton(b);
    down[kInputLeftTrigger] = MaxTrigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > g_cfg.triggerThreshold;
    down[kInputRightTrigger] = MaxTrigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > g_cfg.triggerThreshold;

    int layer = 0;
    for (int i = 0; i < 2; ++i)
        if (g_cfg.layerButton[i] >= 0 && down[g_cfg.layerButton[i]])
            layer |= 1 << i;
    g_layer = layer;

    // A bumper tapped on its own (nothing else pressed while it was down) turns the
    // page of a menu panel; held, it's still the quickbar layer.
    static struct {
        bool down, clean;
        Uint32 since;
    } s_tap[2];
    Uint32 nowMs = sdl.SDL_GetTicks();
    for (int k = 0; k < 2; ++k) {
        int b = g_cfg.layerButton[k];
        if (b < 0)
            continue;
        if (down[b] && !s_tap[k].down)
            s_tap[k] = {true, true, nowMs};
        else if (!down[b] && s_tap[k].down) {
            s_tap[k].down = false;
            if (s_tap[k].clean && nowMs - s_tap[k].since < 400 && PanelOpen())
                PanelTab(k == 0 ? -1 : 1);
        }
        if (layer == 3)
            s_tap[k].clean = false;
    }

    for (int i = 0; i < kInputCount; ++i) {
        if (i == g_cfg.layerButton[0] || i == g_cfg.layerButton[1])
            continue;
        if (down[i] && !g_inputDown[i])
            s_tap[0].clean = s_tap[1].clean = false; // a layer chord, not a tap
        if (layer == 0 && DialogOpen() && DialogButton(i, down[i], g_inputDown[i])) {
            g_inputDown[i] = down[i];
            continue;
        }
        if (layer == 0 && PanelOpen() && PanelButton(i, down[i], g_inputDown[i])) {
            g_inputDown[i] = down[i];
            continue;
        }
        if (layer == 0 && g_proto && g_cfg.diag.targeting && g_stickCamera && g_pickHooked &&
            (i == SDL_CONTROLLER_BUTTON_DPAD_LEFT || i == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
            if (down[i] && !g_inputDown[i]) {
                g_mouseTakeover = false; // the pad is aiming again
                CycleTarget(i == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -1 : 1);
            } else if (!down[i] && g_inputDown[i] && g_latched[i]) {
                Release(*g_latched[i]);
                g_latched[i] = nullptr;
            }
            g_inputDown[i] = down[i];
            continue;
        }
        if (down[i] && !g_inputDown[i]) {
            const Action* a = &g_cfg.bind[layer][i];
            if (a->kind == ActionKind::Inherit)
                a = &g_cfg.bind[0][i];
            if (i == SDL_CONTROLLER_BUTTON_B && layer == 0 && RadialOpen() && game::RadialBack()) {
                // Back one radial level, not Escape (which closes the whole stack).
                g_latched[i] = nullptr;
                g_inputDown[i] = down[i];
                continue;
            }
            if (TargetingActive() && !RadialOpen() && !PanelOpen() && g_targetOid == game::kInvalidOid &&
                a->kind == ActionKind::Mouse) {
                // No target: a click at the parked pointer would just walk to the middle of the screen.
                g_latched[i] = nullptr;
                g_inputDown[i] = down[i];
                continue;
            }
            g_latched[i] = a;
            Press(*a);
        } else if (!down[i] && g_inputDown[i] && g_latched[i]) {
            Release(*g_latched[i]);
            g_latched[i] = nullptr;
        }
        g_inputDown[i] = down[i];
    }
}

void UpdateMovement()
{
    if (g_cfg.moveStick < 0)
        return;
    float x, y;
    ReadStick(g_cfg.moveStick, &x, &y);
    if (PanelOpen())
        x = y = 0; // the stick moves focus; its keys would type into a name field
    bool want[kDirectionCount] = {};
    float mag = sqrtf(x * x + y * y);
    if (mag > g_cfg.moveDeadzone) {
        // 8-way: a direction is held when the stick is within 67.5 degrees of it.
        const float k = 0.3827f; // sin(22.5 degrees)
        float nx = x / mag, ny = y / mag;
        want[kUp] = ny < -k;
        want[kDown] = ny > k;
        want[kLeft] = nx < -k;
        want[kRight] = nx > k;
    }
    for (int d = 0; d < kDirectionCount; ++d) {
        if (want[d] == g_moveHeld[d])
            continue;
        if (want[d])
            Press(g_cfg.move[d]);
        else
            Release(g_cfg.move[d]);
        g_moveHeld[d] = want[d];
    }
}

// Analog camera rotation from the cursor stick. Returns false when there's no
// game camera to turn (menus, unknown build), so the stick drives the pointer.
bool UpdateCameraStick(float x, float y, float dt)
{
    float mag = sqrtf(x * x + y * y);
    float yaw = 0, pitch = 0;
    if (mag > g_cfg.cursorDeadzone) {
        float t = ((mag > 1.0f ? 1.0f : mag) - g_cfg.cursorDeadzone) / (1.0f - g_cfg.cursorDeadzone);
        float scale = powf(t, g_cfg.cursorCurve) / mag * dt;
        // Stick right turns the view right (yaw decreases); stick up raises the view.
        yaw = -x * scale * g_cfg.cameraYawSpeed * (g_cfg.cameraInvertX ? -1.0f : 1.0f);
        pitch = -y * scale * g_cfg.cameraPitchSpeed * (g_cfg.cameraInvertY ? -1.0f : 1.0f);
    }
    bool ok = game::RotateCamera(yaw, pitch);
    if (ok != g_cameraLogged) {
        g_cameraLogged = ok;
        Log("right stick: %s", ok ? "rotating the camera" : "no game camera; moving the pointer");
    }
    return ok;
}

void UpdateCursor(float dt)
{
    if (g_cfg.cursorStick < 0)
        return;
    float x, y;
    ReadStick(g_cfg.cursorStick, &x, &y);
    if (RadialOpen() || DialogOpen())
        return; // UpdateRadial owns the pointer; in a conversation the stick scrolls the text
    if (g_stickCamera && PanelOpen())
        return; // UpdatePanelScroll: in a menu the stick scrolls
    if (g_stickCamera && UpdateCameraStick(x, y, dt))
        return;
    float mag = sqrtf(x * x + y * y);
    if (mag <= g_cfg.cursorDeadzone) {
        g_cursorActive = false;
        return;
    }
    int w = 0, h = 0;
    sdl.SDL_GetWindowSize(g_window, &w, &h);
    if (w <= 0 || h <= 0)
        return;
    if (!g_cursorActive) {
        // Pick up from wherever the real mouse left the pointer.
        int mx, my;
        sdl.SDL_GetMouseState(&mx, &my);
        g_cursorX = static_cast<float>(mx);
        g_cursorY = static_cast<float>(my);
        g_lastWarpX = mx;
        g_lastWarpY = my;
        g_cursorActive = true;
    }
    float t = ((mag > 1.0f ? 1.0f : mag) - g_cfg.cursorDeadzone) / (1.0f - g_cfg.cursorDeadzone);
    float speed = g_cfg.cursorSpeed * (h / 1080.0f) * powf(t, g_cfg.cursorCurve);
    g_cursorX += x / mag * speed * dt;
    g_cursorY += y / mag * speed * dt;
    g_cursorX = g_cursorX < 0 ? 0 : (g_cursorX > w - 1 ? w - 1 : g_cursorX);
    g_cursorY = g_cursorY < 0 ? 0 : (g_cursorY > h - 1 ? h - 1 : g_cursorY);
    int px = static_cast<int>(lroundf(g_cursorX)), py = static_cast<int>(lroundf(g_cursorY));
    if (px != g_lastWarpX || py != g_lastWarpY) {
        sdl.SDL_WarpMouseInWindow(g_window, px, py);
        g_lastWarpX = px;
        g_lastWarpY = py;
    }
}

// What the game will do with the target if you press A (the same choice its click
// code makes).
const char* TargetVerb()
{
    switch (g_targetType) {
    case game::kTypeCreature: return g_targetHostile ? "Attack" : "Talk";
    case game::kTypeDoor: return "Open";
    case game::kTypePlaceable: return "Use";
    case game::kTypeItem: return "Pick up";
    default: return "Interact";
    }
}

// What's in quickbar slot `index` (0-35), as the prompt shows it; "" if it can't be read.
std::string QuickSlotLabel(int index)
{
    static int s_lastOk = -1;
    uint8_t type = 0;
    char raw[96];
    bool ok = game::QuickbarSlotName(index, &type, raw, sizeof(raw));
    if (s_lastOk != static_cast<int>(ok)) {
        s_lastOk = ok;
        Log("quickbar: %s (page %d)", ok ? "slot names on" : "panel not found; prompts show slot numbers",
            game::QuickbarPage());
    }
    if (!ok)
        return "";
    if (type == game::kQuickEmpty)
        return "Empty";
    return text::Display(raw, 20);
}

// A readable name for what a binding does, from NWN's stock keymap.
std::string ActionLabel(const Action& a)
{
    if (a.kind == ActionKind::Mouse)
        return a.mouseButton == SDL_BUTTON_LEFT ? "Click" : a.mouseButton == SDL_BUTTON_RIGHT ? "Right-click" : "Mouse";
    if (a.kind != ActionKind::Keys || a.keyCount == 0)
        return "";
    SDL_Scancode last = a.keys[a.keyCount - 1];
    bool shift = false, ctrl = false;
    for (int i = 0; i + 1 < a.keyCount; ++i) {
        shift |= a.keys[i] == SDL_SCANCODE_LSHIFT || a.keys[i] == SDL_SCANCODE_RSHIFT;
        ctrl |= a.keys[i] == SDL_SCANCODE_LCTRL || a.keys[i] == SDL_SCANCODE_RCTRL;
    }
    if (last >= SDL_SCANCODE_F1 && last <= SDL_SCANCODE_F12) {
        int slot = last - SDL_SCANCODE_F1 + 1;
        std::string name = QuickSlotLabel(quickbar::SlotIndex(slot, shift, ctrl, game::QuickbarPage()));
        if (!name.empty())
            return name;
        return "Slot " + std::to_string(slot) + (shift ? " (Shift)" : ctrl ? " (Ctrl)" : "");
    }
    static const struct { SDL_Scancode sc; const char* label; } kNames[] = {
        {SDL_SCANCODE_I, "Inventory"}, {SDL_SCANCODE_C, "Character"}, {SDL_SCANCODE_B, "Spellbook"},
        {SDL_SCANCODE_J, "Journal"},   {SDL_SCANCODE_M, "Map"},       {SDL_SCANCODE_R, "Rest"},
        {SDL_SCANCODE_O, "Options"},   {SDL_SCANCODE_SPACE, "Pause"}, {SDL_SCANCODE_ESCAPE, "Cancel"},
        {SDL_SCANCODE_TAB, "Highlight"}, {SDL_SCANCODE_KP_0, "Self"},
    };
    for (const auto& n : kNames)
        if (n.sc == last && !shift && !ctrl)
            return n.label;
    return sdl.SDL_GetScancodeName(last);
}

// Button names as the prompt bar shows them.
const char* PromptButton(int input)
{
    static const char* const kNames[kInputCount] = {
        "A", "B", "X", "Y", "Back", "Guide", "Start", "L3", "R3", "LB", "RB",
        "Up", "Down", "Left", "Right", "LT", "RT",
    };
    return input >= 0 && input < kInputCount ? kNames[input] : "?";
}

// The prompt bar: what the face buttons do right now.
void UpdateHud()
{
    overlay::Hud hud;
    if (!g_proto && !PanelOpen()) { // stock bindings
        overlay::SetHud(hud);
        return;
    }
    if (RadialOpen()) {
        hud.prompts = {{"Stick", "Choose"}, {"A", "Select"}, {"B", "Back"}};
        overlay::SetHud(hud);
        return;
    }
    if (PanelOpen() && g_layer == 0) {
        hud.prompts = {{"D-pad", "Move"}, {"A", "Select"}, {"X", "Right-click"}, {"B", "Close"}};
        hud.prompts.push_back({"RS", g_stickCamera ? "Scroll" : "Pointer"});
        for (const panelnav::Control& c : g_panel.controls)
            if (c.cell < 0 && game::IsTabButton(c.obj)) {
                hud.prompts.push_back({"LB/RB", "Tabs"});
                break;
            }
        if (g_panel.focus >= 0 && g_panel.focus < static_cast<int>(g_panel.controls.size())) {
            const panelnav::Rect& r = g_panel.controls[g_panel.focus].r;
            hud.ring = true;
            hud.ringX0 = r.x0;
            hud.ringY0 = r.y0;
            hud.ringX1 = r.x1;
            hud.ringY1 = r.y1;
        }
        overlay::SetHud(hud);
        return;
    }
    if (DialogOpen() && g_layer == 0) {
        hud.prompts = {{"D-pad", "Choose"}, {"A", "Select"}, {"B", "End"}};
        if (overlay::BodyScrollMax() > 0)
            hud.prompts.push_back({"RS", "Scroll"});
        hud.list = g_dialog.replies;
        hud.listSelected = g_dialog.selected;
        hud.title = g_dialog.speaker;
        hud.body = g_dialog.line;
        hud.portrait = g_dialog.portrait;
        hud.bodyScroll = g_dialog.scroll;
        overlay::SetHud(hud);
        return;
    }
    if (g_layer != 0) { // a bumper is held: show that layer's bindings
        static const int kOrder[] = {
            SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
            SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
            SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_BACK,
            SDL_CONTROLLER_BUTTON_START, kInputLeftTrigger, kInputRightTrigger,
        };
        for (int input : kOrder) {
            const Action& a = g_cfg.bind[g_layer][input];
            if (a.kind == ActionKind::Inherit || a.kind == ActionKind::None)
                continue;
            std::string label = ActionLabel(a);
            if (!label.empty())
                hud.prompts.push_back({PromptButton(input), label});
        }
        hud.badge = g_layer == 1 ? "LB" : g_layer == 2 ? "RB" : "LB + RB";
        overlay::SetHud(hud);
        return;
    }
    if (!g_stickCamera) {
        hud.badge = "Pointer";
        hud.prompts = {{"A", "Click"}, {"X", "Right-click"}, {"R3", "Camera"}};
    } else {
        if (TargetingActive() && g_targetOid != game::kInvalidOid) {
            hud.prompts.push_back({"A", TargetVerb()});
            hud.prompts.push_back({"X", "Options"});
        }
        hud.prompts.push_back({"Y", "Self"});
        if (g_targetLocked)
            hud.badge = "Locked";
        else if (g_mouseTakeover)
            hud.badge = "Mouse";
        hud.prompts.push_back({"R3", "Pointer"});
    }
    overlay::SetHud(hud);
}

int SDLCALL HookedShowCursor(int toggle)
{
    if (g_gameCursor < 0)
        g_gameCursor = g_realShowCursor(-1);
    if (toggle < 0)
        return g_gameCursor;
    int prev = g_gameCursor;
    g_gameCursor = toggle ? 1 : 0;
    g_realShowCursor(g_gameCursor && !g_pointerHidden);
    return prev;
}

void SetPointerHidden(bool hide)
{
    if (hide == g_pointerHidden || !g_realShowCursor)
        return;
    if (g_gameCursor < 0)
        g_gameCursor = g_realShowCursor(-1);
    g_pointerHidden = hide;
    g_realShowCursor(g_gameCursor && !hide);
}

// The pointer only gets in the way while the pad aims at nothing (it sits parked
// mid-screen) and while the conversation picker is up. A target brings it back as the
// game's action cursor; so do pointer mode, radials and the real mouse.
bool WantPointerHidden()
{
    if (!g_proto || !g_cfg.diag.hideIdlePointer || !g_stickCamera || g_mouseTakeover || RadialOpen() ||
        PanelOpen() || !game::Current().camera)
        return false;
    return DialogOpen() || (TargetingActive() && g_targetOid == game::kInvalidOid);
}

void Update()
{
    Uint64 now = sdl.SDL_GetPerformanceCounter();
    float dt = 0.0f;
    if (g_lastCounter)
        dt = static_cast<float>(double(now - g_lastCounter) / double(sdl.SDL_GetPerformanceFrequency()));
    g_lastCounter = now;
    if (dt > 0.1f)
        dt = 0.1f; // don't fling the cursor after a loading screen

    // Only drive the game while it has keyboard focus.
    SDL_Window* focus = sdl.SDL_GetKeyboardFocus();
    if (focus) {
        g_window = focus;
        g_refreshTimer += dt;
        if (g_refreshTimer >= 1.0f) {
            g_refreshTimer = 0;
            const game::Caps& c = game::Refresh(); // the camera stick needs it too
            if (g_cfg.diag.enabled && g_cfg.diag.prototypeOnStart && !g_protoAutoStarted && c.player && c.camera) {
                g_protoAutoStarted = true;
                SetPrototype(true);
            }
        }
    }
    if (focus && g_cfg.diag.enabled) {
        UpdateDiagCombo(dt);
        if (g_snapshotRequested) {
            g_snapshotRequested = false;
            DiagSnapshot(focus);
        }
        if (g_protoToggleRequested) {
            g_protoToggleRequested = false;
            SetPrototype(!g_proto);
        }
    }
    inject::Update(dt);
    if (inject::TakeSnapshotRequest())
        g_snapshotRequested = true;
    if (!focus || (g_pads.empty() && !inject::Enabled())) {
        ReleaseAll();
        overlay::SetHud({});
        SetPointerHidden(false);
        game::HideDialogWindow(false);
        return;
    }
    g_window = focus;
    g_windowId = sdl.SDL_GetWindowID(focus);
    RefreshDialog();
    // Our conversation box replaces the game's while the overlay is up to show it.
    game::HideDialogWindow(DialogOpen() && g_cfg.overlay.enabled && g_cfg.overlay.hideStockDialog &&
                           !overlay::Failed() && overlay::FramesDrawn() > 0);
    RefreshPanel(dt);
    UpdateButtons();
    if (DialogOpen())
        UpdateDialogNav(dt);
    else
        g_dialog.navDir = 0;
    if (PanelOpen()) {
        UpdatePanelNav(dt);
        UpdatePanelScroll(dt);
    } else
        g_panel.navX = g_panel.navY = 0;
    if (g_proto)
        UpdateProtoMove(dt);
    else
        UpdateMovement();
    UpdateCursor(dt);
    if (RadialOpen()) {
        UpdateRadial();
    } else {
        g_radialDepth = 0;
        if (g_proto && !DialogOpen() && !PanelOpen())
            UpdateTargeting(dt);
    }
    SetPointerHidden(WantPointerHidden());
    UpdateHud();
}

// Ctrl+Shift+F10 = snapshot, Ctrl+Shift+F11 = prototype mode. Returns true if consumed.
bool HandleDiagKey(const SDL_Event& e)
{
    if ((e.type != SDL_KEYDOWN && e.type != SDL_KEYUP) || !g_cfg.diag.enabled)
        return false;
    Uint16 mod = e.key.keysym.mod;
    SDL_Scancode sc = e.key.keysym.scancode;
    if (!(mod & KMOD_CTRL) || !(mod & KMOD_SHIFT) || (sc != SDL_SCANCODE_F10 && sc != SDL_SCANCODE_F11))
        return false;
    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        if (sc == SDL_SCANCODE_F10)
            g_snapshotRequested = true;
        else
            g_protoToggleRequested = true;
    }
    return true;
}

void Initialize()
{
    g_initialized = true;
    LoadConfig(g_modDir + "nwn_controller.ini", g_cfg);
    LogBindings(g_cfg);
    inject::Configure(g_cfg.diag.enabled && g_cfg.diag.inject, g_modDir + "nwn_controller_inject.txt");
    g_stickCamera = g_cfg.cursorStickCamera;
    overlay::Configure(g_cfg.enabled && g_cfg.overlay.enabled, g_cfg.overlay.scale);
    if (!g_cfg.enabled) {
        Log("disabled in config");
        g_disabled = true;
        return;
    }
    if (sdl.SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        Log("SDL_InitSubSystem(GAMECONTROLLER) failed: %s", sdl.SDL_GetError());
        g_disabled = true;
        return;
    }
    std::string db = g_modDir + "gamecontrollerdb.txt";
    if (SDL_RWops* rw = sdl.SDL_RWFromFile(db.c_str(), "rb")) {
        int n = sdl.SDL_GameControllerAddMappingsFromRW(rw, 1);
        Log("loaded %d controller mappings from gamecontrollerdb.txt", n);
    }
    int count = sdl.SDL_NumJoysticks();
    Log("controller subsystem ready, %d joystick device(s) present", count);
    for (int i = 0; i < count; ++i)
        OpenPad(i);

    g_walkAction.kind = ActionKind::Keys;
    g_walkAction.keyCount = 1;
    g_walkAction.keys[0] = SDL_SCANCODE_LSHIFT;
    if (g_cfg.diag.enabled) {
        game::LogBuildInfo(g_modDir.c_str());
        game::Refresh();
        Log("diag: L3+R3 tap or Ctrl+Shift+F10 = snapshot; hold L3+R3 or Ctrl+Shift+F11 = prototype mode");
    }
}

// SEH guard: a fault in the mod disables it instead of crashing the game.
// Kept free of C++ objects with destructors so __try is allowed.
bool GuardedUpdate()
{
    __try {
        if (!g_initialized)
            Initialize();
        if (!g_disabled)
            Update();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int SDLCALL HookedPollEvent(SDL_Event* event)
{
    if (!event)
        return g_pending.empty() ? g_realPollEvent(nullptr) : 1;

    for (;;) {
        if (!g_pending.empty()) {
            *event = g_pending.front();
            g_pending.pop_front();
            return 1;
        }
        if (g_realPollEvent(event)) {
            if (!g_disabled && (HandleDeviceEvent(*event) || HandleDiagKey(*event)))
                continue;
            if (event->type == SDL_MOUSEMOTION) {
                // A real mouse movement hands the pointer back until the pad aims again.
                if (g_parked && !IsRecentWarp(event->motion.x, event->motion.y)) {
                    g_mouseTakeover = true;
                    g_parked = false;
                    Log("target: mouse moved; targeting paused until the left stick or d-pad is used");
                }
                // Let drags see the buttons we're holding on behalf of the controller.
                event->motion.state |= g_mouseMask;
            }
            return 1;
        }
        // The game's queue is empty: sample controllers once per drain.
        if (g_updatedThisPump || g_disabled) {
            g_updatedThisPump = false;
            return 0;
        }
        g_updatedThisPump = true;
        if (!GuardedUpdate()) {
            Log("unexpected fault in controller update; controller support disabled for this session");
            g_disabled = true;
            g_pending.clear();
        }
    }
}

} // namespace

bool InstallController(HMODULE exe, void* table, unsigned tableSize, const std::string& modDir)
{
    g_modDir = modDir;
    if (!ResolveSdlApi(exe))
        return false;
    SDL_version v;
    sdl.SDL_GetVersion(&v);
    Log("game SDL version %d.%d.%d, jump table %u bytes", v.major, v.minor, v.patch, tableSize);

    void** slot = FindDynApiSlot(exe, "SDL_PollEvent", table, tableSize);
    if (!slot) {
        Log("couldn't locate SDL_PollEvent in the jump table; not hooking");
        return false;
    }
    g_realPollEvent = reinterpret_cast<PollEventFn>(*slot);
    *slot = reinterpret_cast<void*>(&HookedPollEvent);
    Log("hooked SDL_PollEvent");
    if (void** cursorSlot = FindDynApiSlot(exe, "SDL_ShowCursor", table, tableSize)) {
        g_realShowCursor = reinterpret_cast<ShowCursorFn>(*cursorSlot);
        *cursorSlot = reinterpret_cast<void*>(&HookedShowCursor);
        Log("hooked SDL_ShowCursor");
    } else {
        Log("couldn't locate SDL_ShowCursor in the jump table; the pointer stays visible");
    }
    overlay::Install(exe, table, tableSize); // optional: the pad works without it
    return true;
}
