// Controller bindings loaded from nwn_controller.ini (with built-in defaults).
#pragma once

#include <string>

#include "sdl_api.h"

// Inputs are the SDL game-controller buttons followed by the two triggers.
constexpr int kInputLeftTrigger = SDL_CONTROLLER_BUTTON_MAX;
constexpr int kInputRightTrigger = SDL_CONTROLLER_BUTTON_MAX + 1;
constexpr int kInputCount = SDL_CONTROLLER_BUTTON_MAX + 2;

// Layer 0 is the base layer; holding layer button 1 and/or 2 selects layers 1-3.
constexpr int kLayerCount = 4;

enum class ActionKind {
    Inherit, // unset in a layer: fall back to the base layer
    None,
    Keys, // one to four keys pressed in order and released in reverse
    Mouse,
    CursorCenter,
    StickToggle, // on release: switch the cursor stick between camera and pointer
};

struct Action {
    ActionKind kind = ActionKind::Inherit;
    int keyCount = 0;
    SDL_Scancode keys[4] = {};
    Uint8 mouseButton = 0;
};

enum Direction { kUp, kDown, kLeft, kRight, kDirectionCount };

// Stage-2 spike settings ([diag] section, docs/DESIGN.md milestone M0).
enum class DriveMode {
    Facing, // set the character's facing to the stick direction and hold forward
    Steer,  // hold forward and steer with the turn keys toward the stick direction
};

struct DiagConfig {
    bool enabled = true;          // snapshots (L3+R3 tap, Ctrl+Shift+F10) and prototype mode
    bool prototypeOnStart = false;
    DriveMode driveMode = DriveMode::Facing;
    bool lockFrame = false;       // keep the camera frame captured when the stick engages
    float forceSendDegrees = 12;  // resend DriveControl when the heading turns this much (0 = never)
    float walkThreshold = 0.6f;   // stick deflection below this walks (holds Shift); 0 = always run
    float steerTolerance = 8;     // degrees of heading error before Steer mode turns
    bool targeting = true;          // soft target + d-pad lock; A/X act on the target (camera mode)
    float targetCone = 60;          // degrees either side of the aim for the automatic target
    float targetInteractRange = 10; // metres; doors, containers, items, people to talk to
    float targetHostileRange = 25;  // metres; hostile creatures
    bool hideIdlePointer = true;    // hide the mouse pointer when nothing is targeted, and in conversations
    bool stopOnRelease = true;    // end the local server's drive action when the stick is released
    bool traceStops = false;      // log client/server positions for 1.2 s after each release
    bool inject = false;          // play nwn_controller_inject.txt as a virtual pad (testing)
};

// [overlay]: our UI drawn on top of the game (prompt bar etc.).
struct OverlayConfig {
    bool enabled = true;
    float scale = 1.0f; // on top of scaling with the window height (1080p reference)
    bool hideStockDialog = true; // move the game's conversation window away while ours shows
};

struct Config {
    bool enabled = true;
    float triggerThreshold = 0.30f;
    int layerButton[2] = {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER};
    Action bind[kLayerCount][kInputCount];

    int moveStick = 0; // 0 = left, 1 = right, -1 = off
    float moveDeadzone = 0.35f;
    Action move[kDirectionCount];

    int cursorStick = 1;
    float cursorDeadzone = 0.15f;
    float cursorSpeed = 1300.0f; // pixels/second at full deflection, for a 1080p window
    float cursorCurve = 2.0f;

    // The cursor stick can rotate the camera instead (stick:toggle switches).
    bool cursorStickCamera = true;  // start in camera mode
    float cameraYawSpeed = 150.0f;  // degrees/second at full deflection
    float cameraPitchSpeed = 60.0f;
    bool cameraInvertX = false;
    bool cameraInvertY = false;

    OverlayConfig overlay;
    DiagConfig diag;
};

const char* InputName(int input);
int InputFromName(const std::string& name); // an INPUTS name from the ini, or -1
void LoadConfig(const std::string& path, Config& cfg);
std::string DescribeAction(const Action& a);
void LogBindings(const Config& cfg);
