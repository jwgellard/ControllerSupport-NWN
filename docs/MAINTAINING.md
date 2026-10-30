# Maintaining ControllerSupport-NWN

This is the handbook for whoever works on the mod next, person or AI. It explains how
the mod is put together, how to build and test it, how to debug it, and what to do
when the game updates. Read it before changing code.

## 1. The documents

| File | What it's for | When to update it |
|---|---|---|
| `README.md` | Players: what the mod does, install, controls | A feature or control changes |
| `CHANGELOG.md` | What changed in each release | Every user-visible change |
| `docs/MAINTAINING.md` | This handbook: how to work on the mod | The process or structure changes |
| `docs/STATUS.md` | Working log: what works, what was tried and rejected, the user's verdicts and preferences, what's next | After every milestone or in-game verdict |
| `docs/DESIGN.md` | Design and architecture. §12a is the reference for every game offset and function the mod uses, with how each was found | Whenever you use something new from the game |
| `docs/HANDOVER.md` | The brief for the next UI steps and for unattended in-game testing | When the plan changes |
| `docs/research/` | Early research notes (console builds, picking, targeting) | Rarely; they're a record |
| `re/README.md` | The reverse-engineering kit: Ghidra scripts and Python tools | When a tool is added |
| `data/nwn_controller.ini` | The shipped settings, commented for players | With every new setting |

STATUS records *decisions and why*, DESIGN records *how the game works and how the mod
uses it*, and this file records *how to work on the mod*. Don't let them repeat each
other: link instead.

## 2. Setting up

You need:
- Windows 10/11 x64 and NWN:EE from Steam (the mod is verified against build
  `26c6e573`, see `bin\win32\build.txt`).
- Visual Studio 2022 with "Desktop development with C++", and CMake 3.15 or newer
  (the one bundled with Visual Studio is fine).
- For the offline tests: the official **SDL2 2.0.8 x64** `SDL2.dll` from
  [libsdl.org's releases](https://github.com/libsdl-org/SDL/releases/tag/release-2.0.8).
  Locally it lives in `re/data/SDL2.dll` (not in the repo).
- For reverse engineering (only when the game updates or you need something new):
  Ghidra 12.2 with JDK 21, and Python 3 with `pefile` and `capstone`. See
  `re/README.md`.
- A gamepad, with **Steam Input disabled for NWN** (game Properties → Controller →
  Disabled). Otherwise Steam turns the pad into keyboard and mouse before the mod
  sees it.

Keep the repo at `<NWN>\controller_mod`: `install.bat` and the test tools find the
game from there.

## 3. Build, test, install

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DNWNC_BUILD_TESTS=ON
cmake --build build --config Release
build\Release\nwnc_harness.exe re\data\SDL2.dll data
install.bat
```

- `build.bat` does the first two steps (without the tests). Calling it through
  `cmd /c` from some tools fails; run the cmake commands directly then.
- The harness must end with `ALL PASSED`. It has 42 checks (section 8).
- `install.bat` copies `version.dll` and `gamecontrollerdb.txt` into `bin\win32`,
  and the ini only if there isn't one. **Close the game first**: it locks the DLL.
- Your own settings live in `bin\win32\nwn_controller.ini`, which install never
  overwrites. When you add or change a setting, update it by hand as well as
  `data\nwn_controller.ini`, and keep the two in step.
- `uninstall.bat` removes the DLL, the mapping database and the log, and keeps the ini.

## 4. How it works

### 4.1 Getting in

1. `nwmain.exe` imports `VERSION.dll`. That isn't a KnownDLL, so Windows loads our
   `bin\win32\version.dll` first. `src/proxy.cpp` forwards every real `version.dll`
   export to the system copy (`src/version.def`).
2. In `nwmain.exe` only, `proxy.cpp` sets the `SDL_DYNAMIC_API` environment variable
   to our DLL. The game links SDL 2.0.x statically, but SDL checks that variable
   on its first call and asks our `SDL_DYNAPI_entry` to fill its jump table.
3. We fill it with the game's own SDL (its exported `SDL_DYNAPI_entry`), then swap
   three slots for hooks (`src/sdl_api.cpp` decodes each export's `jmp [rip+X]`
   to find its slot):
   - `SDL_PollEvent` → `HookedPollEvent` (`controller.cpp`): runs our per-frame
     update once the game has drained the real queue, then feeds it our events.
   - `SDL_GL_SwapWindow` → the overlay (`overlay.cpp`), drawn just before the frame
     is shown.
   - `SDL_ShowCursor` → `HookedShowCursor`, so we can hide the pointer on top of
     what the game wants.
4. Every other SDL call goes to the game's SDL unchanged. No `SDL2.dll` is loaded.

### 4.2 Two ways of driving the game

- **Synthetic input**: SDL key and mouse events added to the game's queue, and
  `SDL_WarpMouseInWindow` for the pointer. Works on any game build. Classic mode is
  only this.
- **Calling the game directly** (`src/game.cpp`): reading objects, the camera, the
  GUI, and calling the game's own functions (pick a dialogue reply, set facing,
  load a texture). See section 5 for the safety rules.

### 4.3 Modes

| Mode | When | What the pad does |
|---|---|---|
| Classic | `[play] mode = classic`, after an L3+R3 hold, or when controller mode can't start | Exactly the ini bindings, as keys and mouse |
| Controller (`g_proto`) | Default once a game is loaded | Direct movement, soft targeting, stick radials, prompts |
| ↳ Radial | A radial menu is open (`RadialOpen`) | Right stick snaps to slots, A picks, B goes back |
| ↳ Dialogue | A conversation is open (`DialogOpen`) | Our picker: d-pad / left stick choose, A says, B ends |
| Panel | A menu panel with controls is open (`PanelOpen`), also out of a game | D-pad focus ring, A clicks, X right-clicks, LB/RB tabs |
| Pointer | R3 toggles the right stick to the mouse pointer | Plain pointer; targeting pauses |

"proto" in the code means controller mode. The name is historical: it began as the
stage-2 prototype. The "proto:" log lines, `g_proto`, `SetPrototype` and the
`Proto*` functions all refer to controller mode.

### 4.4 One frame

`HookedPollEvent` calls `Update()` (`controller.cpp`) once per frame, inside
`GuardedUpdate()`'s SEH guard. In order:

1. Timing, window focus, and a `game::Refresh()` once a second (auto-starts
   controller mode once a game is loaded).
2. The L3+R3 combo (snapshot / mode switch) and the scripted pad (`inject`).
3. `RefreshDialog()` and `RefreshPanel()`: what's open right now.
4. `UpdateButtons()`: buttons → bindings, with the mode-specific overrides (A/X on
   the target, dialogue and panel buttons, LB/RB tab taps).
5. Navigation for an open dialogue or panel.
6. Movement: `UpdateProtoMove()` in controller mode, else `UpdateMovement()` (keys).
7. `UpdateCursor()`: right stick as camera or pointer.
8. `UpdateRadial()` if a radial is open, else `UpdateTargeting()`.
9. Pointer visibility and `UpdateHud()`, which hands the overlay what to draw.

The overlay never reads input. It draws what `UpdateHud()` last gave it
(`overlay::SetHud`).

### 4.5 Source files

| File | Responsibility |
|---|---|
| `src/proxy.cpp`, `version.def` | The `version.dll` stand-in, `SDL_DYNAPI_entry` |
| `src/sdl_api.{h,cpp}` | SDL functions resolved from the game's exports, jump-slot decoding |
| `src/controller.{h,cpp}` | The hub: hooks, pads, bindings, every mode, per-frame update, HUD content. A map is at the top of the file |
| `src/game.{h,cpp}` | Everything that touches game memory. `game.h` documents each call; `game.cpp` opens with every offset and where it came from |
| `src/overlay.{h,cpp}` | Dear ImGui on OpenGL 3.3: prompt bar, layer prompts, dialogue box, portrait, focus ring |
| `src/panelnav.{h,cpp}` | Pure logic: discover a panel's controls by hit-testing, step focus between them |
| `src/quickbar.{h,cpp}` | Pure logic: which quickbar slot a key fires |
| `src/text.{h,cpp}` | Pure logic: clean game text (colour tags, CP1252 → UTF-8, shortening) |
| `src/config.{h,cpp}` | The ini: bindings, `[play]`, `[sticks]`, `[overlay]`, `[diag]` |
| `src/diag.{h,cpp}` | Snapshots for debugging (section 7) |
| `src/inject.{h,cpp}` | The scripted virtual pad for testing (section 8) |
| `src/log.{h,cpp}` | `nwn_controller.log` next to the DLL |
| `src/vtable_names.inc` | Vtable RVA → class name, for readable snapshots |
| `tests/harness.cpp` | The offline end-to-end test |
| `tools/*.ps1` | In-game test helpers (screenshots, input, launch and load) |

The "pure logic" files have no game access, so the harness can test them directly.
Put new logic that can be separated from the game in files like these.

## 5. The safety rules for touching the game

The mod reads and calls into a closed-source game that changes between patches. A
mistake mustn't crash the game or corrupt a save. Every game access follows these
rules:

1. **Prefer exports.** `nwmain.exe` exports thousands of BioWare's C++ symbols.
   Resolve them by mangled name (`Resolve` in `game.cpp`). They survive patches as
   long as the signature doesn't change.
2. **Check virtual calls against exports.** Before calling vtable slot N, compare
   the function there with the exported one it should be (`CameraSlotsMatch` does
   this for the camera).
3. **Gate everything else on the known build.** Unexported offsets, RVAs and
   globals are used only when `g_caps.knownBuild` is true (PE timestamp
   `0x68e408bc` and image size `0x1716000` match). Where possible, also check a
   vtable RVA before trusting an object, as `InGamePanel()` does.
4. **Guard every read and call.** Reads go through `Read`/`SafeRead` (fault-safe).
   Calls go through a small `Call*` wrapper with `__try/__except`. A wrapper must
   not hold C++ objects with destructors, or MSVC won't compile `__try`.
5. **Fail by switching off.** A failed check logs once and the feature reports
   itself unavailable. A fault in `Update()` disables the whole mod for the session
   (`g_disabled`) and the game runs on as if we weren't there.
6. **Single player only for server calls.** The client and the in-process server
   run on the main thread, our thread, so calling server functions from the hook
   is safe. In multiplayer there is no local server, so those features (stop on
   release, server sync) do nothing.
7. **Object IDs from the server have bit 31 set.** The client's object table puts
   them in its upper half (`ObjectByOid` handles it).
8. **Write down where it came from.** Every new constant in `game.cpp` gets a
   comment naming the function or export it was read from (VA in build 26c6e573),
   and an entry in DESIGN §12a. That's what makes it possible to find again after
   a patch.

GUI coordinates have y pointing **up** (`gui y = window height − screen y`). The
game's hit test (`GuiObjectHitCheck`) changes the GUI's hover state, so after
scanning a panel the mod hit-tests the real pointer again to restore it.

## 6. Common changes

### Adding a setting
1. Add the field with its default to the right struct in `src/config.h`
   (`PlayConfig`, `OverlayConfig`, `DiagConfig`, or `Config` itself).
2. Read it in `LoadConfig()` (`src/config.cpp`), and add it to that section's
   `config:` log line.
3. Add it, with a comment a player can follow, to `data/nwn_controller.ini` **and**
   to your `bin\win32\nwn_controller.ini`.
4. If players will want it, mention it in `README.md`. Add a harness check if it
   changes behaviour in a way that can be tested offline.

### Adding a binding or default control
Change `kDefaults` in `src/config.cpp` and the matching line in
`data/nwn_controller.ini`. Update the controls table in `README.md`.

### Using something new from the game
1. Find it (section 9.2 and `re/README.md`). Prefer an export.
2. Add the constant to the top of `game.cpp` with its provenance comment, a
   guarded wrapper, and a public function in `game.h` that says what it returns and
   whether it's "known build only".
3. Log the first success or failure (the `LogProbe` helper logs only on change).
4. Record it in DESIGN §12a.
5. Test it in game. Add a harness check for any pure logic around it.

### Treating a new always-on panel as HUD
If panel mode wrongly takes over (focus ring or prompts appear with no menu open),
the panel is probably an always-on HUD element. Take a snapshot, find its vtable in
the `panel` lines, and add it to `kHudPanelVtables` in `game.cpp` with its class name.

### A dedicated layout for a menu
Generic navigation (DESIGN §8.1) covers every legacy panel. For a panel that needs
better handling (DESIGN §8.2), identify it by vtable (`OpenMenuPanel()`), and adjust
the control list, starting focus or button meanings in `RefreshPanel()` and
`PanelButton()`. Keep the generic path as the fallback.

## 7. Debugging

### The log
`bin\win32\nwn_controller.log` is rewritten each time the game starts. Each line
starts with a time and an area:

| Prefix | From |
|---|---|
| `config:` / `bindings` | Settings as loaded. Check these first when behaviour doesn't match the ini |
| `game:` | Build check (`known build` or `UNKNOWN build`), exports, capabilities (`caps ... player=1 camera=1`), hooks |
| `proto:` | Controller mode on/off, movement telemetry |
| `target:` | Target changes and why |
| `radial:` | Radial slot order self-check |
| `quickbar:` | Quickbar name reading |
| `dialog:` | The conversation window: found, hidden, restored, portraits |
| `panel:` | Menu panels: scans, controls found, focus, tabs |
| `overlay:` | GL setup and failures |
| `right stick:` | Camera / pointer switches |
| `inject:` | The scripted pad |
| `snap:` | Snapshot output (below) |
| `trace:` | `trace_stops` output |

A healthy start shows `known build`, then `caps ... player=1 camera=1 gui=1` once a
game is loaded, then `proto: controller mode ON`.

### Snapshots
Tap L3+R3 together (or press Ctrl+Shift+F10) with `[diag] enabled = 1`. The mod
writes the camera, nearby objects, the player's drive state, the quickbar, any
conversation, and every open panel with its controls, vtables and a hit-test grid
to the log. This is the first tool for anything GUI-related.

### Other switches
- `[diag] trace_stops = 1` logs client and server positions for 1.2 s after each stop.
- `[play] mode = classic` takes controller mode out of the picture.
- `[overlay] enabled = 0` takes the overlay out of the picture.
- `[general] enabled = 0` turns the mod off without uninstalling it.

### Prove it before fixing it
Read the log and a snapshot before changing code, and add a log line if they don't
answer the question. Many wrong guesses in this project's history (STATUS.md) were
caught this way.

## 8. Testing

### Offline harness (`tests/harness.cpp`)
The official `SDL2.dll` 2.0.8 has the same dynamic-API layout as the game's SDL, so
the harness hooks it exactly as the mod hooks the game. It fakes the controller,
then checks:
- Hooks, the pick-hook detour, config loading (including old inis).
- Quickbar mapping, text cleanup, panel scanning and focus steps, inject parsing.
- Buttons, layers, Shift combos, movement keys, triggers, pointer motion, clicks,
  focus loss.
- Overlay rendering, in a hidden GL window. It writes `overlay_preview.bmp` and
  `overlay_dialog_preview.bmp` so you can look at the result.

Add a check for every piece of pure logic you write. The harness can't test
anything that needs the game itself.

### In game, by hand
Run through this after any change to `controller.cpp` or `game.cpp`, and always
after a game update:

1. The log shows `known build`, then `proto: controller mode ON` after loading.
2. Walk with a full and a half push. Let go: you stop dead.
3. Rotate the camera with the right stick. R3: pointer mode and back.
4. Walk up to a door, a container and an NPC: each gets targeted. A acts on it,
   X opens its radial. D-pad left/right steps between targets.
5. Radial: X on a target, right stick picks a slot, A chooses, B goes back.
6. Hold LB and RB: the prompt bar shows quickbar names. Fire a slot.
7. Talk to an NPC: our picker shows their portrait and line. D-pad, A, B work.
8. RB+Y inventory: d-pad moves, A drags an item, X opens its radial, tap RB to
   switch bags, B closes.
9. RB+B character sheet: tap LB/RB through the tabs, right stick scrolls.
10. B for the game menu, then Resume. From the main menu, load a save with the pad.
11. Hold L3+R3: classic mode (keys only). Hold again: back to controller mode.

### In game, unattended
For a session without anyone at the pad (an AI session, or long checks):
- Set `[diag] inject = 1` in the live ini, then restart the game.
- `tools/launch_load.ps1` starts the game, skips the intro and loads the newest
  save. Its click positions assume a 1920x1080 window.
- Write steps to `bin\win32\nwn_controller_inject.txt` (format in `src/inject.h`):
  `press a`, `press leftstick+rightstick 1.2`, `stick left 0 -1 2`, `wait 1`,
  `snapshot`. The mod plays the file each time it changes.
- `tools/gamectl.ps1 shot out.png` takes a screenshot, and `focus`, `click`, `key`,
  `cursor` do what they say. The game must have focus for the mod to act.
- Read the log for the result. Set `inject = 0` again afterwards.
- Never save the game in a test: the saves are the player's.

## 9. When the game updates

Beamdog patches change addresses. The mod is built for that: on a new build it logs
`UNKNOWN build: unexported offsets disabled`, and only the features that rely on
exports keep working. Nothing should crash.

### 9.1 What survives a patch
This is what the code's gating predicts; it hasn't been seen on a real patch yet.

Keeps working: classic mode (every binding, so the d-pad still turns the camera),
the right stick as a pointer, snapshots, and the overlay's prompt bar.

Switches off until the offsets are re-verified (everything gated on
`knownBuild`): the right-stick camera (the stick moves the pointer instead), direct
movement in `facing` mode (`drive_mode = steer` may still work), stop on release,
targeting, radial centring and snapping, quickbar names, the dialogue picker, and
menu navigation.

### 9.2 Porting to a new build
1. **Get the new facts.** Note the new PE timestamp and image size from the
   `game: nwmain.exe timestamp=... image=...` log line.
2. **Analyse the new exe.** Copy the new `nwmain.exe` and analyse it in a fresh
   Ghidra project (`re/README.md`; full analysis takes a long time). Keep the old
   project to compare against.
3. **Re-find each constant** at the top of `src/game.cpp`, in order. Each comment
   names where the value was read: an exported function, a string, a vtable (by RTTI
   name), or a function at a VA in the old build. Ways to re-find them:
   - Exported neighbours: decompile the export the comment names and read the
     offset again (`re/ghidra_scripts/Decomp.java`).
   - Unexported functions: find them through their callers or a string they use
     (`StrXref.java`, `Callers.java`). Compare the old decompile with the new one.
   - Vtables: by RTTI class name (`re/tools/vt.py`). Update `vtable_names.inc` and
     `kHudPanelVtables` too.
   - Struct offsets: they move least. Confirm each in the decompile of a function
     that uses it.
   - The pick detour also checks the function's first 15 bytes (`kPickPrologue`).
     If the compiler changed them, the detour needs a new prologue copy.
4. **Update `kKnownTimestamp` and `kKnownImageSize`** only once every constant is
   checked. Turning the gate on with a stale offset is the one way to crash the game.
5. **Test.** Run the harness, then the in-game checklist (section 8) in full. Check
   the log for each `game:` self-test.
6. **Record it.** Update DESIGN §12a with the new build and any changed reasoning,
   note it in STATUS.md, and add a changelog line ("Supports build ...").

Supporting two builds at once would need the constants in a per-build table. That
isn't done yet. So far the mod has followed the current Steam build only.

## 10. Releasing

There's no CI or release automation yet. To make a release by hand:
1. Build Release and run the harness (`ALL PASSED`).
2. Run the in-game checklist.
3. Move `CHANGELOG.md`'s "Unreleased" items under a new version heading
   (`v0.N.0`, date).
4. Zip `version.dll`, `nwn_controller.ini`, `gamecontrollerdb.txt`, `README.md`,
   `LICENSE` and `third_party/imgui/LICENSE.txt`, with a short install note: copy
   the files into `<NWN>\bin\win32`, disable Steam Input.
5. Tag the commit (`git tag v0.N.0`) and publish a GitHub release with the zip.
   Mention the supported game build in the notes.

Good next steps here: a GitHub Actions workflow that builds and runs the harness on
every push (it needs only MSVC, CMake and the SDL2 2.0.8 DLL), and code signing
(SignPath's free plan for open-source projects).

## 11. Repository rules

- **Never commit material derived from the game**: `re/ghidra_proj/`,
  `re/decompiles/`, `re/data/` (strings, 2DA files, the game's DLLs). `.gitignore`
  keeps them out; don't force-add them. Short quotes of a few decompiled lines in
  the docs, to explain an offset, are fine.
- Don't commit logs, screenshots of someone's game, or files that contain local
  paths with a user name.
- Line endings: `.gitattributes` makes the repo store LF and gives `.bat` files CRLF
  on checkout. Other files may be either in a working copy; the compiler doesn't
  care. Watch for tools that rewrite a whole file (PowerShell's `Set-Content` adds a
  BOM and changes the encoding; Git Bash's `sed -i` changes line endings): check
  `git diff --stat` before committing.
- C++17, MSVC `/W4 /permissive-`, 4-space indents, braces on the same line except
  for functions. Globals are `g_name`, constants `kName`. Comments explain why, and
  name the game function an assumption comes from.
- Vendored code (`third_party/`) stays unmodified. Dear ImGui is 1.91.5; the SDL
  headers are 2.0.8, used for types only.

## 12. Glossary

| Term | Meaning |
|---|---|
| Controller mode / proto | The controller-first layer. `proto` is its historical name in code |
| Classic mode | Pad → keys and mouse only, from the ini bindings |
| Known build | `nwmain.exe` matches build 26c6e573 (timestamp and image size) |
| Export | A function or global `nwmain.exe` exports by mangled C++ name; stable across patches |
| RVA / VA | Address relative to the exe's base / absolute in the default image (0x140000000 + RVA) |
| OID | Game object ID. Server objects have bit 31 set |
| Soft target | The object the mod picked for A and X, shown by the game's own highlight. The pick hook makes the game believe the pointer is on it |
| Lock | A target chosen with the d-pad, kept until it's out of range |
| Panel | A legacy GUI window (inventory, character sheet...). `CGuiPanel` in the game |
| HUD panel | An always-on panel (quickbar, compass, chat...) that must not trigger panel mode |
| NUI | The game's newer Nuklear-based UI (module browser, server windows). Not navigable yet |
| Snapshot | A diagnostic dump to the log (L3+R3 tap) |
| Inject | The scripted virtual pad used for unattended tests |
| Harness | `tests/harness.cpp`, the offline end-to-end test |
