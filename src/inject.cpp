#include "inject.h"

#include <windows.h>

#include <deque>
#include <sstream>
#include <vector>

#include "config.h"
#include "log.h"

namespace inject {
namespace {

struct Step {
    enum Kind { Press, Down, Up, Stick, Wait, Snapshot, Marker } kind;
    std::vector<int> inputs; // Press
    int stick = 0;           // Stick
    float x = 0, y = 0;
    float seconds = 0;
    std::string text; // Marker
};

constexpr float kReleaseGap = 0.1f; // after a press, so the next one is a fresh edge

bool g_enabled;
std::string g_path;
FILETIME g_stamp;
float g_sinceCheck;
std::deque<Step> g_steps;
float g_left;       // time left in the current step (and its release gap)
bool g_started;     // the front step has begun
bool g_down[kInputCount];
float g_stick[2][2];
bool g_snapshot;

void ClearOutputs()
{
    for (bool& d : g_down)
        d = false;
    g_stick[0][0] = g_stick[0][1] = g_stick[1][0] = g_stick[1][1] = 0;
}

bool ParseLine(const std::string& line, Step* out)
{
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    if (verb == "press") {
        std::string spec;
        in >> spec;
        out->kind = Step::Press;
        for (size_t start = 0; start <= spec.size();) {
            size_t plus = spec.find('+', start);
            std::string name = spec.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
            int input = InputFromName(name);
            if (input < 0)
                return false;
            out->inputs.push_back(input);
            if (plus == std::string::npos)
                break;
            start = plus + 1;
        }
        out->seconds = 0.15f;
        in >> out->seconds;
        return !out->inputs.empty();
    }
    if (verb == "down" || verb == "up") {
        std::string name;
        in >> name;
        out->kind = verb == "down" ? Step::Down : Step::Up;
        int input = InputFromName(name);
        out->inputs.push_back(input);
        return input >= 0;
    }
    if (verb == "stick") {
        std::string which;
        in >> which >> out->x >> out->y >> out->seconds;
        out->kind = Step::Stick;
        out->stick = which == "right" ? 1 : 0;
        return (which == "left" || which == "right") && !in.fail();
    }
    if (verb == "wait") {
        out->kind = Step::Wait;
        in >> out->seconds;
        return !in.fail();
    }
    if (verb == "snapshot") {
        out->kind = Step::Snapshot;
        return true;
    }
    if (verb == "log") {
        out->kind = Step::Marker;
        std::getline(in, out->text);
        return true;
    }
    return false;
}

void CheckFile()
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(g_path.c_str(), GetFileExInfoStandard, &a))
        return;
    if (CompareFileTime(&a.ftLastWriteTime, &g_stamp) == 0)
        return;
    g_stamp = a.ftLastWriteTime;
    HANDLE f = CreateFileA(g_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    std::string text(a.nFileSizeLow, '\0');
    DWORD got = 0;
    ReadFile(f, text.data(), a.nFileSizeLow, &got, nullptr);
    CloseHandle(f);
    text.resize(got);
    if (Load(text))
        Log("inject: playing %s (%d steps)", g_path.c_str(), static_cast<int>(g_steps.size()));
}

} // namespace

void Configure(bool enabled, const std::string& scriptPath)
{
    g_enabled = enabled;
    g_path = scriptPath;
    if (enabled) { // a script already there when the game starts was for a past run
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExA(g_path.c_str(), GetFileExInfoStandard, &a))
            g_stamp = a.ftLastWriteTime;
    }
}

bool Enabled()
{
    return g_enabled;
}

bool Load(const std::string& text)
{
    std::deque<Step> steps;
    std::istringstream in(text);
    std::string line;
    for (int n = 1; std::getline(in, line); ++n) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == ';' || line[first] == '#')
            continue;
        Step s;
        if (!ParseLine(line.substr(first), &s)) {
            Log("inject: can't read line %d: '%s'; script ignored", n, line.c_str());
            return false;
        }
        steps.push_back(s);
    }
    g_steps = steps;
    g_started = false;
    ClearOutputs();
    return true;
}

void Update(float dt)
{
    if (!g_enabled)
        return;
    if ((g_sinceCheck += dt) >= 0.25f) {
        g_sinceCheck = 0;
        CheckFile();
    }
    while (!g_steps.empty()) {
        Step& s = g_steps.front();
        if (!g_started) {
            g_started = true;
            g_left = s.seconds;
            switch (s.kind) {
            case Step::Press:
                for (int i : s.inputs)
                    g_down[i] = true;
                g_left += kReleaseGap;
                break;
            case Step::Stick:
                g_stick[s.stick][0] = s.x;
                g_stick[s.stick][1] = s.y;
                break;
            case Step::Down:
            case Step::Up: g_down[s.inputs[0]] = s.kind == Step::Down; break;
            case Step::Snapshot: g_snapshot = true; break;
            case Step::Marker: Log("inject: --%s", s.text.c_str()); break;
            case Step::Wait: break;
            }
        }
        g_left -= dt;
        if (s.kind == Step::Press && g_left < kReleaseGap)
            for (int i : s.inputs)
                g_down[i] = false;
        if (g_left > 0)
            return;
        if (s.kind == Step::Stick)
            g_stick[s.stick][0] = g_stick[s.stick][1] = 0;
        dt = -g_left; // carry what's left of this frame into the next step
        g_steps.pop_front();
        g_started = false;
        if (g_steps.empty())
            Log("inject: script done");
    }
}

bool Button(int input)
{
    return g_enabled && input >= 0 && input < kInputCount && g_down[input];
}

void Stick(int stick, float* x, float* y)
{
    *x = g_enabled ? g_stick[stick & 1][0] : 0;
    *y = g_enabled ? g_stick[stick & 1][1] : 0;
}

bool TakeSnapshotRequest()
{
    bool r = g_snapshot;
    g_snapshot = false;
    return r;
}

bool Idle()
{
    return g_steps.empty();
}

} // namespace inject
