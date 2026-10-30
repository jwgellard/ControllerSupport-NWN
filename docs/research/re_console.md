# Does nwmain.exe (PC, 26c6e573) still contain console/gamepad gameplay code?

**Bottom line: No.** The PC build contains no reusable console/gamepad targeting, target cycling, virtual-cursor, stick-radial or GUI focus-navigation code. Only dead legacy stubs and the statically linked SDL 2.0.8 library remain. Nothing in the exe, the data files or the settings can enable it. Confidence is high (about 90%) that no gamepad code path exists. It is medium for "no console-only code hides in unnamed functions": I found no string, symbol or xref evidence of any, but the client object-picking code has no symbol names.

Note: the "rva" in `strings.txt` is really a file offset. For .rdata, VA = 0x140000000 + offset + 0xC00. For example, `ui.targeting-feedback-mode` is listed at 00e29658 and sits at VA 140e2a258.

## 1. Strings
Relevant hits (listed offsets; add 0xC00 for the VA):
- Legacy joystick key-binding names: "Joystick Button 1/2" (e2dbd8, e2dbf0), "Joystick Y/X" (e2dc08, e2dc18), "Joystick Hat" (e2dc28). The only reference is `FUN_1406df630` (1406df630, the keybind-device table builder). It is guarded by `CExoInput::IsJoystickActive()`, which is a stub (see section 2).
- SDL internals: SDL_GAMECONTROLLERCONFIG and related env-var names, the built-in controller mapping DB (e8e700 to e90af0: Xbox, PS3/PS4, Switch Pro and others), and XInput/DirectInput strings. This is stock library data, not game code.
- Radial menu: `CRadialMenu`, "GUI_radialband", "gui_radial_slect", `ui.radial.spellcasting.always-show-as-subradial`, `ui.radial.class-abilities.always-show-as-subradial`. This is the classic mouse radial. Its input is the keymap actions Radial* (Action47-55) and ActivateOwnRadial (Action62).
- Targeting strings are server, script or spell-shape related: `TargetingData_EnterTargetMode`, `ClientSideMessage_EnterTargetingMode`, `Input_TargetSelected`, `targetShape*`, `graphics.spell-targeting-effect.*`, and `ui.targeting-feedback-mode` (settings.tml has `targeting-feedback-mode = 2`, a hover/highlight style only).
- Not present at all: gamepad, virtual cursor, reticle, soft target, snap, aim, focus navigation, UI_CONTROLLER, interaction prompt, next/prev/cycle target. "cycle" only hits textures, day/night and `forcecyclespeed`. "nearest" only hits `IR_ATTACKNEAREST` / `ASSOCIATE_COMMAND_ATTACKNEAREST` (server-side AI) and GL filters.
- Settings keys (`ui.`, `input.`, `game.` list at e27e48 to e2c6e0) contain no controller keys. The only `input.*` keys are `input.keyboard.shift-walk-mode-inverted`, `input.keyboard.tooltip-doubletap-threshold` and `input.mouse.hardware-pointer`.

## 2. Symbols and decompiled stubs
- `CExoInput` (14016e740 to 14016ebd0) is a hollow shell. Both `AddEvent` overloads (14016e830, 14016e840) are one-line stubs.
- `CExoInput::GetNumberJoysticks` (14016eb20) decompiles to `return 0;`.
- `CExoInput::IsJoystickActive` (14016eb30) decompiles to `return 0;`.
- `CAppManager::GetPlatformId` (1402956d0) decompiles to `return 2;` (PC). It is sent as the X-Game-PlatformId header. I found no platform-id branching that selects console code.
- No symbols match gamepad, controller, virtual cursor, cycle-target or focus-navigation. The "controller" symbols are `NewController` (MDL animation), `*TextureController` and `RouterController` (UPnP). There are no `CNWC*` / `CGuiInGame` target, select or mouse-pick symbols; only `CNWCCMessageData` exists.
- The nearest/target gameplay symbols are all server-side (`CNWSCreature::GetNearestEnemy`, `GetNearestObject`, `SetEnterTargetingModeData` and similar).

## 3. SDL and input pipeline (Ghidra project copy)
- `SDL_main` (140046340) calls `SDL_Init(0x4021)`, which is TIMER|VIDEO|EVENTS. It does **not** pass JOYSTICK (0x200) or GAMECONTROLLER (0x2000). On stock PC, SDL never opens the joystick subsystem and emits no controller events.
- The game never calls `SDL_GameControllerOpen`, `SDL_JoystickOpen`, `SDL_NumJoysticks` or `SDL_IsGameController`. The only references to them are the SDL export table (1410e1e50 and similar). The `SDL_GameController*` functions at 140ab16e0 and following are present only because SDL is linked in full.
- `Update` (1400448b0) loops `SDL_PollEvent` and calls `WinMessageHandler`. Every event first goes to the stub `CExoInput::AddEvent`.
- `WinMessageHandler` handles only these SDL event types: 0x100, 0x102, 0x200 (window events), 0x201, 0x300 (key down) and 0x303 (text input). I did not read the mouse-event part of the function in full. There is no branch for 0x600-0x654 (JOY*/CONTROLLER*) or 0x700 (FINGER) events.
- `FUN_1406ed720` is the keymap action handler. keymap.2da has no target-cycle or interact actions. The nearest equivalents are Radial* (47-55), ActivateOwnRadial (62), Tooltip/Inspect (97), DriveMode* (87-93), CameraModeToggle (60), Previous/Next Inv Tab (105/106) and Rest (107).
- The settings registration table (`FUN_1406ae3b0`, too large to decompile) is referenced by the setting-key strings. It registers no controller setting.
- `SDL_main` sets the hint `SDL_IOS_ORIENTATIONS`, so the codebase has mobile heritage. No mobile or console input path is compiled in.

## 4. Data files
- I searched all 113k resource names in `nwn_base.key` and `nwn_retail.key` (and the 93 in the retail key) for pad, ctrl, controller, console, joy, xbox, ps4, switch, gamep, virt, cursor, reticle, target, focus and glyph. There are no controller GUIs, button-glyph textures, gamepad keymaps or console layouts. The hits are unrelated: `cursors`, `fnt_console`, `ir_target`, `x2_inc_switches`, normal `ctl_btn_*` buttons and the like.
- `keymap.2da` has no controller columns.
- `Documents\Neverwinter Nights\settings.tml` and `nwn.ini` have no joystick, gamepad or controller keys. The only target-related key is `ui.targeting-feedback-mode`.

## 5. Debug and cheat commands
- The `Cheat_*` messages (Cheat_DebugMode, MoveToArea, Resurrect, CastSpell, Invulnerability, Kill, RunScript, LevelUp and others) are gated by DM status or DebugMode ("Cheat/debug command not allowed: Not a DM, or not in DebugMode"). None relate to targeting or input.
- Keymap actions 100-104 (Instagib, GetLastObject, ModuleDebugInfo, JumpToPoint, GetDebugInfo) do not toggle input modes either.
- I did not enumerate the `ConsoleFunc` names registered with the aurora developer console (`aurconsole.cpp`, font `fnt_console`). This is a low-confidence residual gap; no controller commands turned up in strings.

## 6. Implications for the mod
There is nothing to switch on. The console gameplay layer was not compiled into this PC binary, or it was removed. The mod must supply its own logic:
1. In the existing `SDL_PollEvent` hook, call `SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)` (SDL 2.0.8 is statically linked) and translate pad input into existing paths. Options are synthesized SDL mouse/key events, or calls to the keymap handler `FUN_1406ed720` with Radial*, Drive*, Camera* and Inventory-tab action ids.
2. Implement target cycling and "nearest object" picking in the mod. Enumerate client-side objects, pick one, then move the real or virtual cursor to its screen position. The game's own mouse picking, hover, tooltip and click-to-act code then handles the rest.
3. Reuse the stock `CRadialMenu` through the `ActivateOwnRadial` and `RadialN/NE/...` actions for a radial menu. For GUI navigation, drive the mouse cursor. The old GUI has no focus-navigation, and NUI (Nuklear-based, see `d_nui.c`) is mouse and keyboard only.

## Artifacts
- Ghidra headless rejects any path with a dot-prefixed element, so run it through a junction if the project sits under one.
- Decompiles and xref dumps are in the same folder: `d_main.c` (SDL_main and Update), `d_wm.c` (WinMessageHandler), `xr1.txt`, `xr2.txt`, `xr3.txt`. The xref script is `gs\Xrefs.java`.
