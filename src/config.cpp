#include "config.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace {

// Same spelling as SDL's mapping strings, so the INI reads like a controller mapping.
const char* const kInputNames[kInputCount] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
    "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright",
    "lefttrigger", "righttrigger",
};

const char* const kLayerSections[kLayerCount] = {"base", "layer1", "layer2", "layer1+2"};
const char* const kDirectionNames[kDirectionCount] = {"move_up", "move_down", "move_left", "move_right"};

struct DefaultBinding {
    int layer;
    const char* input;
    const char* action;
};

// Defaults follow NWN:EE's stock keymap (read from keymap.2da and the scancode
// table that CExoInputInternal builds at startup).
const DefaultBinding kDefaults[] = {
    {0, "a", "mouse:left"},
    {0, "b", "key:Escape"},
    {0, "x", "mouse:right"},
    {0, "y", "key:Keypad 0"},       // radial menu on self
    {0, "back", "key:M"},           // map
    {0, "guide", "none"},
    {0, "start", "key:Space"},      // pause
    {0, "leftstick", "key:Tab"},    // inspect / highlight
    {0, "rightstick", "stick:toggle"}, // right stick: camera <-> pointer
    {0, "dpup", "key:PageUp"},      // camera up
    {0, "dpdown", "key:PageDown"},  // camera down
    {0, "dpleft", "key:Left"},      // rotate camera left
    {0, "dpright", "key:Right"},    // rotate camera right
    {0, "lefttrigger", "key:Down"}, // zoom out
    {0, "righttrigger", "key:Up"},  // zoom in

    // LB: quickbar slots F1-F8 (d-pad left/up/right/down, then X/Y/B/A).
    {1, "dpleft", "key:F1"}, {1, "dpup", "key:F2"}, {1, "dpright", "key:F3"}, {1, "dpdown", "key:F4"},
    {1, "x", "key:F5"}, {1, "y", "key:F6"}, {1, "b", "key:F7"}, {1, "a", "key:F8"},

    // RB: quickbar F9-F12 on the d-pad, game panels on the face buttons.
    {2, "dpleft", "key:F9"}, {2, "dpup", "key:F10"}, {2, "dpright", "key:F11"}, {2, "dpdown", "key:F12"},
    {2, "x", "key:B"},     // spellbook
    {2, "y", "key:I"},     // inventory
    {2, "b", "key:C"},     // character sheet
    {2, "a", "key:J"},     // journal
    {2, "back", "key:R"},  // rest
    {2, "start", "key:O"}, // options

    // LB+RB: the Shift quickbar, same layout as LB.
    {3, "dpleft", "key:Left Shift+F1"}, {3, "dpup", "key:Left Shift+F2"},
    {3, "dpright", "key:Left Shift+F3"}, {3, "dpdown", "key:Left Shift+F4"},
    {3, "x", "key:Left Shift+F5"}, {3, "y", "key:Left Shift+F6"},
    {3, "b", "key:Left Shift+F7"}, {3, "a", "key:Left Shift+F8"},
};

const char* const kDefaultMove[kDirectionCount] = {"key:W", "key:S", "key:A", "key:D"};

const char* DefaultFor(int layer, const char* input)
{
    for (const auto& d : kDefaults)
        if (d.layer == layer && strcmp(d.input, input) == 0)
            return d.action;
    return "";
}

std::string Trim(std::string s)
{
    // GetPrivateProfileString keeps inline comments; treat " ;" as the start of one.
    size_t c = s.find(" ;");
    if (c != std::string::npos)
        s.resize(c);
    size_t b = s.find_first_not_of(" \t");
    size_t e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string Lower(std::string s)
{
    for (char& ch : s)
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string ReadIni(const std::string& path, const char* section, const char* key, const char* def)
{
    char buf[256];
    GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), path.c_str());
    return Trim(buf);
}

float ReadFloat(const std::string& path, const char* section, const char* key, float def)
{
    std::string v = ReadIni(path, section, key, "");
    return v.empty() ? def : static_cast<float>(atof(v.c_str()));
}

// A [play] setting; inis from before [play] existed kept it in [diag].
std::string ReadPlay(const std::string& path, const char* key, const char* def)
{
    std::string v = ReadIni(path, "play", key, "");
    return v.empty() ? ReadIni(path, "diag", key, def) : v;
}

float ReadPlayFloat(const std::string& path, const char* key, float def)
{
    std::string v = ReadPlay(path, key, "");
    return v.empty() ? def : static_cast<float>(atof(v.c_str()));
}

int ParseStick(const std::string& v, int def)
{
    std::string s = Lower(v);
    if (s == "left")
        return 0;
    if (s == "right")
        return 1;
    if (s == "none" || s == "off")
        return -1;
    return def;
}

int ParseInput(const std::string& v)
{
    std::string s = Lower(v);
    for (int i = 0; i < kInputCount; ++i)
        if (s == kInputNames[i])
            return i;
    return -1;
}

bool ParseKeys(const std::string& spec, Action& out)
{
    // Whole-name lookup first so names containing '+' ("Keypad +") still work.
    SDL_Scancode whole = sdl.SDL_GetScancodeFromName(spec.c_str());
    if (whole != SDL_SCANCODE_UNKNOWN) {
        out.keys[0] = whole;
        out.keyCount = 1;
        return true;
    }
    size_t start = 0;
    while (start <= spec.size()) {
        size_t plus = spec.find('+', start);
        std::string name = Trim(spec.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
        SDL_Scancode sc = sdl.SDL_GetScancodeFromName(name.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN || out.keyCount == 4)
            return false;
        out.keys[out.keyCount++] = sc;
        if (plus == std::string::npos)
            break;
        start = plus + 1;
    }
    return out.keyCount > 0;
}

Action ParseAction(const std::string& text, const char* where)
{
    Action a;
    if (text.empty())
        return a; // Inherit
    std::string lower = Lower(text);
    if (lower == "none") {
        a.kind = ActionKind::None;
        return a;
    }
    if (lower == "cursor:center") {
        a.kind = ActionKind::CursorCenter;
        return a;
    }
    if (lower == "stick:toggle") {
        a.kind = ActionKind::StickToggle;
        return a;
    }
    if (lower.compare(0, 6, "mouse:") == 0) {
        static const struct { const char* name; Uint8 button; } kButtons[] = {
            {"left", SDL_BUTTON_LEFT}, {"middle", SDL_BUTTON_MIDDLE}, {"right", SDL_BUTTON_RIGHT},
            {"x1", SDL_BUTTON_X1}, {"x2", SDL_BUTTON_X2},
        };
        for (const auto& b : kButtons) {
            if (lower.compare(6, std::string::npos, b.name) == 0) {
                a.kind = ActionKind::Mouse;
                a.mouseButton = b.button;
                return a;
            }
        }
    } else if (lower.compare(0, 4, "key:") == 0) {
        a.kind = ActionKind::Keys;
        if (ParseKeys(Trim(text.substr(4)), a))
            return a;
    }
    Log("config: can't parse '%s' for %s; leaving it unbound", text.c_str(), where);
    a = Action();
    a.kind = ActionKind::None;
    return a;
}

} // namespace

int InputFromName(const std::string& name)
{
    return ParseInput(name);
}

const char* InputName(int input)
{
    return input >= 0 && input < kInputCount ? kInputNames[input] : "?";
}

std::string DescribeAction(const Action& a)
{
    switch (a.kind) {
    case ActionKind::Inherit:
        return "(base)";
    case ActionKind::None:
        return "none";
    case ActionKind::CursorCenter:
        return "cursor:center";
    case ActionKind::StickToggle:
        return "stick:toggle";
    case ActionKind::Mouse: {
        static const char* const kNames[] = {"?", "left", "middle", "right", "x1", "x2"};
        return std::string("mouse:") + (a.mouseButton < 6 ? kNames[a.mouseButton] : "?");
    }
    case ActionKind::Keys: {
        std::string s = "key:";
        for (int i = 0; i < a.keyCount; ++i) {
            if (i)
                s += '+';
            s += sdl.SDL_GetScancodeName(a.keys[i]);
        }
        return s;
    }
    }
    return "?";
}

void LogBindings(const Config& cfg)
{
    for (int layer = 0; layer < kLayerCount; ++layer) {
        std::string line;
        for (int input = 0; input < kInputCount; ++input) {
            const Action& a = cfg.bind[layer][input];
            if (a.kind == ActionKind::Inherit || (layer == 0 && a.kind == ActionKind::None))
                continue;
            line += std::string(" ") + kInputNames[input] + "=" + DescribeAction(a);
        }
        Log("bindings [%s]:%s", kLayerSections[layer], line.c_str());
    }
    std::string move;
    for (int d = 0; d < kDirectionCount; ++d)
        move += std::string(" ") + kDirectionNames[d] + "=" + DescribeAction(cfg.move[d]);
    Log("bindings [sticks]: move_stick=%d cursor_stick=%d (starts as %s, camera %.0f/%.0f deg/s%s%s)%s",
        cfg.moveStick, cfg.cursorStick, cfg.cursorStickCamera ? "camera" : "pointer", cfg.cameraYawSpeed,
        cfg.cameraPitchSpeed, cfg.cameraInvertX ? " invert-x" : "", cfg.cameraInvertY ? " invert-y" : "",
        move.c_str());
}

void LoadConfig(const std::string& path, Config& cfg)
{
    bool exists = GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    Log("config: %s%s", path.c_str(), exists ? "" : " (not found, using defaults)");

    cfg.enabled = ReadIni(path, "general", "enabled", "1") != "0";
    cfg.triggerThreshold = ReadFloat(path, "general", "trigger_threshold", cfg.triggerThreshold);
    for (int i = 0; i < 2; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "layer%d_button", i + 1);
        std::string v = ReadIni(path, "general", key, "");
        if (!v.empty())
            cfg.layerButton[i] = Lower(v) == "none" ? -1 : ParseInput(v);
    }

    for (int layer = 0; layer < kLayerCount; ++layer) {
        for (int input = 0; input < kInputCount; ++input) {
            const char* name = kInputNames[input];
            std::string v = ReadIni(path, kLayerSections[layer], name, DefaultFor(layer, name));
            char where[64];
            snprintf(where, sizeof(where), "[%s] %s", kLayerSections[layer], name);
            cfg.bind[layer][input] = ParseAction(v, where);
        }
    }
    // Base-layer inputs with no binding at all do nothing rather than inherit.
    for (auto& a : cfg.bind[0])
        if (a.kind == ActionKind::Inherit)
            a.kind = ActionKind::None;

    cfg.moveStick = ParseStick(ReadIni(path, "sticks", "move_stick", ""), cfg.moveStick);
    cfg.moveDeadzone = ReadFloat(path, "sticks", "move_deadzone", cfg.moveDeadzone);
    for (int d = 0; d < kDirectionCount; ++d) {
        cfg.move[d] = ParseAction(ReadIni(path, "sticks", kDirectionNames[d], kDefaultMove[d]), kDirectionNames[d]);
        if (cfg.move[d].kind == ActionKind::Inherit)
            cfg.move[d].kind = ActionKind::None;
    }

    cfg.cursorStick = ParseStick(ReadIni(path, "sticks", "cursor_stick", ""), cfg.cursorStick);
    cfg.cursorDeadzone = ReadFloat(path, "sticks", "cursor_deadzone", cfg.cursorDeadzone);
    cfg.cursorSpeed = ReadFloat(path, "sticks", "cursor_speed", cfg.cursorSpeed);
    cfg.cursorCurve = ReadFloat(path, "sticks", "cursor_curve", cfg.cursorCurve);
    cfg.cursorStickCamera = Lower(ReadIni(path, "sticks", "cursor_stick_mode", "camera")) != "pointer";
    cfg.cameraYawSpeed = ReadFloat(path, "sticks", "camera_yaw_speed", cfg.cameraYawSpeed);
    cfg.cameraPitchSpeed = ReadFloat(path, "sticks", "camera_pitch_speed", cfg.cameraPitchSpeed);
    cfg.cameraInvertX = ReadIni(path, "sticks", "camera_invert_x", "0") == "1";
    cfg.cameraInvertY = ReadIni(path, "sticks", "camera_invert_y", "0") == "1";
    cfg.overlay.enabled = ReadIni(path, "overlay", "enabled", "1") != "0";
    cfg.overlay.scale = ReadFloat(path, "overlay", "scale", cfg.overlay.scale);
    cfg.overlay.hideStockDialog = ReadIni(path, "overlay", "hide_stock_dialog", "1") != "0";
    Log("config: overlay enabled=%d scale=%.2f hide_stock_dialog=%d", cfg.overlay.enabled, cfg.overlay.scale,
        cfg.overlay.hideStockDialog);
    PlayConfig& p = cfg.play;
    // Older inis: mode was "prototype_on_start" in [diag], default off.
    std::string mode = Lower(ReadPlay(path, "mode", ""));
    if (mode.empty())
        mode = ReadIni(path, "diag", "prototype_on_start", "") == "0" ? "classic" : "controller";
    p.controllerMode = mode != "classic";
    p.driveMode = Lower(ReadPlay(path, "drive_mode", "facing")) == "steer" ? DriveMode::Steer : DriveMode::Facing;
    p.lockFrame = ReadPlay(path, "lock_frame", "0") == "1";
    p.forceSendDegrees = ReadPlayFloat(path, "force_send_degrees", p.forceSendDegrees);
    p.walkThreshold = ReadPlayFloat(path, "walk_threshold", p.walkThreshold);
    p.steerTolerance = ReadPlayFloat(path, "steer_tolerance", p.steerTolerance);
    p.targeting = ReadPlay(path, "targeting", "1") != "0";
    p.targetCone = ReadPlayFloat(path, "target_cone", p.targetCone);
    p.targetInteractRange = ReadPlayFloat(path, "target_interact_range", p.targetInteractRange);
    p.targetHostileRange = ReadPlayFloat(path, "target_hostile_range", p.targetHostileRange);
    p.hideIdlePointer = ReadPlay(path, "hide_idle_pointer", "1") != "0";
    p.stopOnRelease = ReadPlay(path, "stop_on_release", "1") != "0";
    Log("config: play mode=%s drive_mode=%s lock_frame=%d force_send_degrees=%.1f walk_threshold=%.2f "
        "steer_tolerance=%.1f stop_on_release=%d targeting=%d (cone %.0f, interact %.1f m, hostile %.1f m) "
        "hide_idle_pointer=%d",
        p.controllerMode ? "controller" : "classic", p.driveMode == DriveMode::Steer ? "steer" : "facing",
        p.lockFrame, p.forceSendDegrees, p.walkThreshold, p.steerTolerance, p.stopOnRelease, p.targeting,
        p.targetCone, p.targetInteractRange, p.targetHostileRange, p.hideIdlePointer);

    DiagConfig& d = cfg.diag;
    d.enabled = ReadIni(path, "diag", "enabled", "1") != "0";
    d.traceStops = ReadIni(path, "diag", "trace_stops", "0") == "1";
    d.inject = ReadIni(path, "diag", "inject", "0") == "1";
    if (d.inject)
        Log("config: diag inject=1: playing nwn_controller_inject.txt as a virtual pad");
    Log("config: diag enabled=%d trace_stops=%d", d.enabled, d.traceStops);

    if (cfg.cursorCurve < 0.1f)
        cfg.cursorCurve = 0.1f;
    if (cfg.moveDeadzone > 0.95f)
        cfg.moveDeadzone = 0.95f;
    if (cfg.cursorDeadzone > 0.95f)
        cfg.cursorDeadzone = 0.95f;
}
