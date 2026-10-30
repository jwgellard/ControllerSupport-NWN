# ControllerSupport-NWN

Native gamepad support for Neverwinter Nights: Enhanced Edition (64-bit Windows
client). Works with Xbox, PlayStation, Switch Pro and any other SDL-supported pad,
without Steam Input or AntiMicro. Nothing in the game install is modified: the mod is
a `version.dll` the game loads from `bin\win32`, and deleting it removes the mod.

**Work in progress.** It's verified against one game build, `26c6e573` (Oct 2025; see
`bin\win32\build.txt`). On any other build the features that rely on the game's
internals switch themselves off, and the basic key and mouse emulation keeps working.
Only single player has been tested.

## What it does

- **Controller mode** (the default once a game is loaded), a controller-first
  layer:
  - **Movement:** analog, camera-relative movement on the left stick. A half push
    walks, and you stop dead on release (single player).
  - **Camera:** analog camera control on the right stick.
  - **Targeting:** whatever you aim at is targeted automatically, hostiles first.
    You aim with the left stick while moving, otherwise with the camera. A acts on
    the target and X opens its radial menu, so you don't need the pointer. D-pad
    left / right steps between targets.
  - **Radials:** radial menus are driven with the stick.
  - **Prompt bar:** an on-screen bar shows what each button does. While LB / RB is
    held, it shows your real quickbar slot names.
  - **Conversations:** a full-screen dialogue picker with the speaker's portrait,
    their line in large text and the replies. The d-pad picks a reply and A says it.
  - **Menus:** d-pad focus navigation in the game's menus: inventory (including
    dragging items), character sheet, journal, game menu, main menu and Load screen.
    LB / RB switch tabs and the right stick scrolls.
  - **Pointer:** the mouse pointer is hidden when nothing is targeted.

- **Classic mode:** the pad only drives the game's own keys and mouse: movement
  keys, clicks, radial menus and the quickbar on LB / RB layers, all configurable
  in an INI.

Hold **L3+R3 for a second** (or press Ctrl+Shift+F11) to switch between the two.
To start in classic mode, set `mode = classic` under `[play]` in the INI.

## Install

1. Put this folder inside your Neverwinter Nights install, as
   `Neverwinter Nights\controller_mod`.
2. Run `build.bat`. It needs CMake and Visual Studio 2022 with the C++ workload.
3. Run `install.bat`, which copies three files into `bin\win32`:
   - `version.dll`: the mod
   - `nwn_controller.ini`: bindings and settings (an existing file is never overwritten)
   - `gamecontrollerdb.txt`: community controller mappings (PS5 and other pads newer
     than the game's SDL)
4. **Turn off Steam Input for NWN:** in Steam, open the game's Properties →
   Controller and choose *Disabled*. Otherwise Steam turns the pad into keyboard and
   mouse before the mod sees it.
5. Launch the game normally.

`uninstall.bat` removes the mod and keeps your INI. Steam's "Verify integrity"
leaves the files alone, because they aren't part of the game's depot.

## Default controls

| Input | Action |
|---|---|
| Left stick | Move (controller mode: analog, camera-relative; classic: W run, S back, A/D turn) |
| Right stick | Camera; R3 switches it to the mouse pointer and back |
| A / Cross | Left click: select, move, attack, talk, drag |
| X / Square | Right click: radial menu |
| B / Circle | Escape: close window / cancel / game menu |
| Y / Triangle | Radial menu on yourself |
| Start | Pause |
| Back / Share | Map |
| L3 | Inspect / highlight (Tab) |
| D-pad | Camera (controller mode: ←→ step between targets; in menus: move focus) |
| LT / RT | Zoom out / in |
| **Hold LB** | D-pad ← ↑ → ↓ = F1–F4, X Y B A = F5–F8 (quickbar) |
| **Hold RB** | D-pad = F9–F12, X spells, Y inventory, B character, A journal, Back rest, Start options |
| **Hold LB+RB** | Shift quickbar, same layout as LB |

Everything is configurable in `bin\win32\nwn_controller.ini`, and the comments there
explain the syntax. The defaults match the game's stock keymap, so if you've remapped
keys in-game, update the INI to match.

Typing (chat, character names) still needs a keyboard or an on-screen keyboard.
Windows built with the game's newer NUI system (the module browser behind "New", and
server-made windows) aren't navigable yet, so they use the pointer.

## Troubleshooting

- The mod writes what it's doing to `bin\win32\nwn_controller.log`. A healthy start
  says `known build`, and `controller mode ON` once a game is loaded.
- **Tap L3+R3 together** (or Ctrl+Shift+F10) to add a snapshot of the camera, nearby
  objects, open menus and their controls to the log. Include the log when reporting
  a problem.
- `enabled = 0` under `[general]` turns the mod off without uninstalling it.

## How it works

Findings from reverse-engineering `nwmain.exe`, using Ghidra plus the binary's own
exported C++ symbols:

- SDL 2.0.x is linked statically, but every `SDL_*` function is **exported**, and
  each export is a single `jmp [rip+X]` through SDL's *dynamic API* jump table
  (627 entries, 0x1398 bytes).
- SDL's `SDL_DYNAMIC_API` override is compiled in and read with
  `GetEnvironmentVariableA` on the first SDL call. If an override fails, SDL
  quietly falls back to the built-in table.
- The game calls `SDL_Init(TIMER|VIDEO|EVENTS)` and never initializes
  joystick/gamecontroller. `CExoInput::GetNumberJoysticks` / `IsJoystickActive`
  are stubs that return 0, so the original BioWare joystick code is gone.
- Input comes from one loop in `Update()`: `while (SDL_PollEvent(&e)) WinMessageHandler(&e)`.
  `CExoInputInternal::AddEvent(SDL_Event*)` maps keys by **scancode** through a
  table built in the `CExoInputInternal` constructor, takes mouse buttons and
  coordinates from the event, and never calls `SDL_GetMouseState`. Events are also
  copied into Nuklear (NUI).

So the mod:

1. Loads as `version.dll`, which `nwmain.exe` imports and which isn't a KnownDLL.
   In `nwmain.exe` only, it points `SDL_DYNAMIC_API` at itself.
2. When SDL calls our `SDL_DYNAPI_entry`, it calls the game's own exported
   `SDL_DYNAPI_entry` to fill the table with the game's SDL. It then decodes the
   `SDL_PollEvent`, `SDL_GL_SwapWindow` and `SDL_ShowCursor` exports' `jmp`s to find
   their slots and swaps in hooks. The game keeps running its own SDL; no SDL2.dll
   is involved.
3. Once per frame, after the game drains the real queue, the hook reads every
   connected controller and feeds back synthetic key and mouse events, or calls the
   game's own functions (targeting, conversation replies, GUI hit tests) by their
   exported names. Controller events are consumed and never reach the game.
4. The overlay (prompt bar, radials, dialogue picker, focus ring) is drawn with
   Dear ImGui on OpenGL just before the game's frame is presented.

Addresses that aren't exported are checked against the known build and covered by
self-tests at start-up. Every read or call into the game is guarded, so if anything
goes wrong the mod logs it to `bin\win32\nwn_controller.log` and switches that
feature off rather than crashing. `docs/DESIGN.md` §12a lists every game offset and
function the mod uses, with the reasoning.

## Working on the mod

Start with [`docs/MAINTAINING.md`](docs/MAINTAINING.md): setup, how the mod is put
together, the safety rules for touching the game, debugging, testing, and what to do
when the game updates. [`CHANGELOG.md`](CHANGELOG.md) lists what changed.

## Tests

`cmake -S . -B build -DNWNC_BUILD_TESTS=ON && cmake --build build --config Release`,
then run

    build\Release\nwnc_harness.exe <path to official SDL2.dll 2.0.8 x64> data

The harness hooks the official `SDL2.dll` (from libsdl.org) the same way the mod
hooks the game (same dynamic-API layout). It uses real SDL for initialization, the
mapping DB and key names, and runs a scripted controller through clicks, layers,
Shift combos, movement, triggers, pointer motion, focus loss, text cleanup, menu
navigation and the scripted-pad parser. It also renders overlay previews to `.bmp`
files.

## Layout

    src/proxy.cpp        version.dll forwarding, SDL_DYNAPI_entry
    src/controller.cpp   SDL hooks, controller sampling, controller mode, menus, dialogue
    src/config.cpp       INI parsing and default bindings
    src/sdl_api.cpp      SDL exports resolved from nwmain.exe, jump-slot decoding
    src/game.cpp         guarded access to game internals (objects, camera, GUI)
    src/overlay.cpp      Dear ImGui overlay
    src/panelnav.cpp     menu control discovery and focus movement
    src/text.cpp         game text cleanup (colour tags, CP1252)
    src/inject.cpp       scripted virtual pad for unattended testing
    src/diag.cpp         snapshot diagnostics
    data/                default INI and gamecontrollerdb.txt
    docs/                MAINTAINING (start here to work on the mod), STATUS (progress
                         log and decisions), DESIGN (architecture, offsets), research
    re/                  reverse-engineering scripts (Ghidra, Python)
    tools/               in-game test helpers (screenshots, input, launch and load)
    tests/harness.cpp    offline end-to-end test
    third_party/         Dear ImGui (MIT), SDL 2.0.8 headers (zlib)

## Credits and licenses

This project is MIT licensed (`LICENSE`).
Dear ImGui by Omar Cornut and contributors (MIT, `third_party/imgui/LICENSE.txt`).
SDL headers and `gamecontrollerdb.txt` from the SDL project and the SDL_GameControllerDB
community (zlib). Neverwinter Nights is a trademark of its owners; this project is
unofficial and not affiliated with Beamdog or Wizards of the Coast.
