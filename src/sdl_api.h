// SDL entry points resolved from nwmain.exe's own exports.
//
// The game links SDL statically but exports every SDL_* symbol. Each export is a
// one-instruction stub that jumps through SDL's dynamic-API table, so calling
// these always reaches the game's SDL. Nothing here links against SDL2.lib;
// the headers are used only for types and prototypes.
#pragma once

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <windows.h>

#define NWNC_SDL_FUNCS(X)                   \
    X(SDL_GetVersion)                       \
    X(SDL_GetError)                         \
    X(SDL_GetTicks)                         \
    X(SDL_GetPerformanceCounter)            \
    X(SDL_GetPerformanceFrequency)          \
    X(SDL_InitSubSystem)                    \
    X(SDL_NumJoysticks)                     \
    X(SDL_JoystickNameForIndex)             \
    X(SDL_JoystickGetDeviceGUID)            \
    X(SDL_JoystickGetGUIDString)            \
    X(SDL_JoystickInstanceID)               \
    X(SDL_IsGameController)                 \
    X(SDL_GameControllerOpen)               \
    X(SDL_GameControllerClose)              \
    X(SDL_GameControllerName)               \
    X(SDL_GameControllerGetJoystick)        \
    X(SDL_GameControllerGetButton)          \
    X(SDL_GameControllerGetAxis)            \
    X(SDL_GameControllerAddMappingsFromRW)  \
    X(SDL_RWFromFile)                       \
    X(SDL_GetKeyboardFocus)                 \
    X(SDL_GetWindowID)                      \
    X(SDL_GetWindowSize)                    \
    X(SDL_GL_GetDrawableSize)               \
    X(SDL_GL_GetCurrentContext)             \
    X(SDL_GetMouseState)                    \
    X(SDL_WarpMouseInWindow)                \
    X(SDL_GetModState)                      \
    X(SDL_GetKeyFromScancode)               \
    X(SDL_GetScancodeFromName)              \
    X(SDL_GetScancodeName)

struct SdlApi {
#define NWNC_DECLARE(fn) decltype(&::fn) fn;
    NWNC_SDL_FUNCS(NWNC_DECLARE)
#undef NWNC_DECLARE
};

extern SdlApi sdl;

// Fills `sdl` from the exe's exports. Returns false (and logs) if any are missing.
bool ResolveSdlApi(HMODULE exe);

// Returns the jump-table slot that the exported stub `name` dispatches through,
// or nullptr if the stub isn't the expected `jmp [rip+disp32]` into `table`.
void** FindDynApiSlot(HMODULE exe, const char* name, void* table, unsigned tableSize);
