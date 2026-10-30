// Controller UI overlay: Dear ImGui drawn into the game's frame from an
// SDL_GL_SwapWindow hook (docs/DESIGN.md §3.2). It only draws; it never takes
// input from the game. Any fault turns it off for the session and the frame is
// presented untouched.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace overlay {

// What to show this frame, built by the controller from game state.
struct Prompt {
    std::string button; // "A", "B", "X", "Y", "LB", "RB", "L3", "R3", "D-pad"...
    std::string label;
};
// A picture to show, top-down RGBA. `key` names it: the overlay uploads it to a GL
// texture once per key.
struct Image {
    std::string key;
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};
struct ListItem {
    std::string text; // UTF-8
    bool enabled = true;
};
struct Hud {
    std::vector<Prompt> prompts; // bottom-centre prompt bar
    std::string badge;           // small mode tag above it, e.g. "Pointer"
    std::vector<ListItem> list;  // a numbered choice list above the bar (dialogue replies)
    int listSelected = -1;
    std::string title;           // a text box above the list (the speaker), UTF-8
    std::string body;            // and its text (what the NPC says)
    float bodyScroll = 0;        // pixels scrolled down, at 1080p; clamped when drawn
    std::shared_ptr<const Image> portrait; // beside the text box (the speaker's portrait)
    bool ring = false;            // a focus ring around a screen rectangle (window pixels)
    float ringX0 = 0, ringY0 = 0, ringX1 = 0, ringY1 = 0;
};

// Swaps the SDL_GL_SwapWindow slot in the game's jump table for our hook.
bool Install(HMODULE exe, void* table, unsigned tableSize);
// Nothing is drawn until configured (the controller loads the INI lazily).
void Configure(bool enabled, float scale);
// Called from the input side every frame; copied under a lock.
void SetHud(const Hud& hud);
// How far the body text could scroll on the last frame drawn (pixels at 1080p); 0 when
// it all fits.
float BodyScrollMax();

// For the offline harness.
int FramesDrawn();
bool Failed();

} // namespace overlay
