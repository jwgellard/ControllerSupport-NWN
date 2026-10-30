// Diagnostics: a scripted virtual pad, so the mod can be tested without hands on the
// controller. Off unless [diag] inject = 1; then nwn_controller_inject.txt (next to
// the ini) is played whenever it changes, one step per line, each after the last:
//   press <input>[+<input>...] [seconds]  hold the inputs (default 0.15 s), then let go
//   down <input> / up <input>             hold an input across the steps that follow
//   stick <left|right> <x> <y> <seconds>  hold a stick (-1..1, y down), then centre it
//   wait <seconds>
//   snapshot                              write a snapshot to the log
//   log <text>                            write a marker line to the log
// Inputs use the ini's names (a, dpup, leftshoulder, lefttrigger...). The script adds
// to what the real pads report; it never replaces them.
#pragma once

#include <string>

namespace inject {

void Configure(bool enabled, const std::string& scriptPath);
bool Enabled();
// Replaces the script with `text`; false (and logs) on a line it can't read.
bool Load(const std::string& text);
// Advances the script by dt seconds, first reloading the file if it changed.
void Update(float dt);
bool Button(int input);                 // a pad button or kInputLeftTrigger/RightTrigger
void Stick(int stick, float* x, float* y); // 0 = left, 1 = right; 0, 0 when idle
bool TakeSnapshotRequest();
bool Idle(); // nothing left to play

} // namespace inject
