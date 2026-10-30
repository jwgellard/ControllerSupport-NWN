// Gamepad -> SDL keyboard/mouse event translation, injected via the game's SDL_PollEvent.
#pragma once

#include <windows.h>

#include <string>

// Called from SDL_DYNAPI_entry once the game's jump table is populated.
// Resolves the SDL API and swaps the SDL_PollEvent slot for our hook.
bool InstallController(HMODULE exe, void* table, unsigned tableSize, const std::string& modDir);
