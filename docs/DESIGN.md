# Controller-first UI: design (stage 2)

Status: draft, 2026-10-29. Stage 1 (gamepad → synthetic keyboard/mouse, right stick as
pointer) is built and passes the offline harness. This document plans stage 2: a UI you
can play entirely with a pad, built on direct movement, a soft target, and focus
navigation instead of a pointer.

Evidence tags used below: **[C]** confirmed by decompiling this build (26c6e573),
**[N]** inferred from symbol/RTTI names, **[U]** unverified or needs an in-game spike.

## 1. Goals and constraints

1. **Every common action without the pointer.** The pointer becomes an explicit fallback
   ("cursor mode"), not the way you play.
2. **Direct movement.** The left stick moves the character relative to the camera, with
   walk or run chosen by stick deflection.
3. **Works everywhere stage 1 works.** That means the official campaigns, community
   modules and haks, and multiplayer servers that know nothing about the mod. So no
   server or module scripts, and no gameplay information the client doesn't already show.
4. **Fails soft.** Any feature whose game hooks don't match the running build switches
   itself off and falls back to the stage 1 behaviour. The game never crashes because of
   the mod.
5. **The legacy UI is kept, not replaced.** The inventory, character sheet, spellbook,
   dialogue and stores are hardcoded panels. We make them navigable instead of
   rebuilding them. Rebuilding them is a later, optional project.

## 2. What the research established

### Things that rule options out
- **There is no console-port code in the PC binary.** The scan covered all 6,054 RTTI
  names, about 66k strings and all 18,245 exports [C]. The `CExoInput` joystick stubs
  are Aurora-era leftovers that only add dead key names. Beamdog's FAQ says there are
  no plans for controller support on PC.
- **NUI cannot be the foundation.**
  - It is server-driven. Windows are created by nwscript and events go back to module
    scripts.
  - It has no focus or navigation model; only TextEdit has focus/blur events.
  - It cannot touch the legacy panels.
  - It is absent on any server that doesn't script it.
- **The console edition is a warning, not a template.** Reviewers singled out:
  - vague "face it and hope it glows" targeting
  - stick-click for inventory pages
  - items that couldn't go on the quickbar
  - the chat window covering button prompts

  Other ports were criticised for pointer-first screens left unchanged (PoE,
  Pathfinder), hotbars split across awkward pages (DOS2) and laggy radials (BG3).

### Things that make the design possible
- **Rendering.**
  - OpenGL through SDL. `GLRender::SwapBuffers` is the only caller of
    `SDL_GL_SwapWindow` [C].
  - All GL entry points are loaded at runtime [C], almost certainly through
    `SDL_GL_GetProcAddress`, which is exported and so hookable through the same dynapi
    table as `SDL_PollEvent` [N].
- **The GUI manager is reachable through exported names.**
  - Globals: `g_pGuiMan`, `g_pAppManager`, `g_pExoInput`.
  - `CGuiMan` functions: `FindInActivePanels`, `GuiObjectHitCheck`, `SetMousePosition`,
    `CaptureMouse`/`CaptureKeyboard`.
  - `CGuiPanel::FindTag` [C].
  - Active panels are a `CExoLinkedList<CGuiPanel>`, and each panel has a name such as
    `PNL_PARTY_BAR` and a tag table of controls [C].
- **Every control shares one `CGuiObject` vtable** [C]:
  - slot 2 `SetHilight`
  - slots 15–16 key handlers
  - slots 18–19 `HandleLMouseDown/Up`, 22–23 right button
  - slots 30–31 mouseover hilite
  - slots 32–33 activate/deactivate
- **Game actions have dedicated client senders.** They go through
  `CNWCMessage::SendPlayerToServerMessage` (`FUN_1407c25c0`) [C]. Each one below is a
  major/minor message pair:

  | Action | Message | Sender |
  |---|---|---|
  | Attack | 6/2 | `FUN_1407bf180` |
  | UseObject | 6/0xB | `FUN_1407c07c0` |
  | WalkToWaypoint | 6/1 | `FUN_1407c0970` |
  | UseItem | 6/9 | `FUN_1407c0610` |
  | CastSpell | 6/0x12 | `FUN_1407bf4e0` |
  | **DriveControl** | 6/0x1D | `FUN_1407bf980` |
  | AbortDriveControl | 6/0x21 | `FUN_1407bf040` |

  None of the senders are exported. The world click handler `FUN_1406efa40(app,
  CNWCCreature*)` sends Attack or UseObject for a given object, so it is a
  "left-click this object" with no screen position needed [C].
- **Objects.**
  - Objects are looked up by ID through `CGameObjectArray` (`GetGameObject` is
    exported) and cast with the exported `CGameObject::AsNWC*` casts [C].
  - The controlled creature is the object ID at `CClientExoAppInternal+0x48` [C].
  - The party is an array at `+0x38` with its count at `+0x40` [C].
- **The native radial menu already supports keyboard selection.** `keymap.2da` has
  RadialN…NW, RadialCancel and ActivateOwnRadial. An open radial can be detected
  because it holds mouse capture, with GUI type `0x524d6e75` [C for the check].
- **Drive mode is a real protocol, not repeated clicks.** This is what makes direct
  movement possible. Decompiled `FUN_1406f1e40` [C]:
  - The client keeps seven held-direction flags in `CClientExoAppInternal`
    (`+0x1b4…+0x1cc`). Their mapping to forward, auto-run, back, turn and strafe is
    inferred from the bit logic [N].
  - Every 250–500 ms, or on change, it sends DriveControl containing:
    - the character's position and area
    - its **facing**, quantised from the creature's orientation vector at
      `creature+0x44`
    - a sequence byte
    - a mode bitmask: 1 = walk forward, 3 = run forward, 2 = back,
      4/8 = strafe, 0x10/0x20 = turn
  - It sets the orientation locally through `creature->vtbl[0x120/8]`.
  - The server (`HandlePlayerToServerInputDriveControl` → `AddDriveAction`, both
    exported) reads the position, area, facing and mode, then queues a drive action.
  - Drive actions are built in, so drive mode works on every server.
- **SDL 2.0.8 limits.** There is no controller-type API, no rumble and no sensors.
  - The pad type can be read from the joystick GUID's vendor and product IDs, with
    the device name as a fallback.
  - Rumble needs `XInputSetState` for Xbox pads, or HID reports for Sony pads.

## 3. Architecture

```
          ┌──────────────── nwmain.exe ────────────────┐
 SDL_PollEvent ──hook──► Input core ──► Mode machine ──► Action layer ──► synthetic SDL events (tier 0)
 SDL_GL_SwapWindow ─hook─► Overlay renderer ◄── UI model      │       └──► game calls (tier 1/2)
 SDL_GL_GetProcAddress ─hook─► GL entry points (+ uniform capture for projection)
                                   ▲
                       Game bridge (reads GUI/object state; capability-gated)
```

### 3.1 Dependency tiers

Every feature declares the tier it needs and what it falls back to.

| Tier | Depends on | How it is found | When it breaks |
|---|---|---|---|
| 0 | SDL dynapi slots | export stub decoding (already done) | never seen; SDL ABI is frozen |
| 1 | exported BioWare symbols | `GetProcAddress` by mangled name | a symbol renamed; layout drift caught by sanity checks |
| 2 | unexported functions and struct offsets | **byte-signature scan**, checked against a per-build table keyed on `bin/win32/build.txt` | any patch; the feature turns off and logs a warning |

Tier-2 rules:
- Each tier-2 item has a signature, an expected build hash, and a self-test. Examples:
  "controlled creature's OID resolves to an object whose `AsNWCCreature()` is non-null",
  or "panel list contains `PNL_PARTY_BAR`".
- A failed self-test disables only that feature.
- The log records exactly which features are on, so after a game patch the user sends
  one log and we know what to fix.

### 3.2 Modules (new code, under `src/`)

- `input/`
  - Pad sampling, already in `controller.cpp`. This gets split out.
  - Pad-type detection for glyphs.
  - Edge and hold timing, so each button can do separate tap / hold / double-tap
    actions.
- `modes/`
  - A state machine re-evaluated every frame from game state.
  - Exactly one mode owns the pad at a time (§4).
- `actions/`
  - `Synth`: the existing key and mouse synthesis.
  - `Game`: tier-1/2 calls such as `InteractWith(oid)`, `SetFacing`, `PressRadial(dir)`
    and `ClickControl(ctrl)`, each with a synth fallback.
- `bridge/`
  - Capability-gated readers: controlled creature, nearby objects, active panels and
    their controls, radial/dialogue/chat state, camera.
  - All raw pointer access lives here, wrapped in `__try`.
- `overlay/`
  - Dear ImGui (MIT, vendored) with the OpenGL 3 backend, drawn inside our
    `SDL_GL_SwapWindow` hook before calling the real swap.
  - Draws: button-prompt bar, focus ring, target reticle and name plate, quickbar HUD,
    hub radial, on-screen keyboard, toasts.
  - Uses a bundled TTF font, a parchment-and-bronze style to sit next to the NWN HUD,
    and its own UI-scale setting based on the drawable height.
  - ImGui's own gamepad nav is not used. Focus is ours, because it has to cover
    *game* controls, not ImGui widgets.
- `config/`
  - The existing INI, extended with per-mode sections, plus an in-game options page
    drawn by the overlay.

The overlay only ever draws on top of the game. It never takes input from the game,
except while the on-screen keyboard or our own options page is open.

## 4. Modes

The pad belongs to exactly one mode. These are evaluated top to bottom every frame:

| Mode | Active when | Detection |
|---|---|---|
| TextEntry | chat bar or an edit box has keyboard capture | `CGuiMan+0x50` non-null [C offset, N meaning] |
| Radial | a native radial menu is open | mouse-captured object is type `0x524d6e75` [C] |
| Dialogue | an NPC conversation is open | `CGuiInGameChatDialog` panel in the active list [N] |
| Panel | any other panel the player opened (inventory, store, character, spells, level-up, menus, modal popups) | active panel list minus the always-on HUD panels [N] |
| Cursor | user toggled it (R3), or Panel mode found a panel it can't navigate | ours |
| Explore | otherwise | — |

**On-screen prompts.** A prompt bar at the bottom always shows the current mode's
buttons, using glyphs for the detected pad. It moves above the chat window, never
under it.

## 5. Explore mode (moving through the world)

### 5.1 Direct movement
**Path A** is tier 2 and is the target. Every frame the stick is outside the deadzone:
1. Desired heading = camera yaw + stick angle.
2. Set the controlled creature's facing to that heading, through the same orientation
   setter the client calls (`vtbl+0x120`).
3. Hold the drive-forward flag.

The client then sends DriveControl with that facing on its normal 250–500 ms cadence,
and immediately when the flags change.
- Deflection below about 70% clears the run bit (walk); above it, the character runs.
- Releasing the stick clears the flags, so the game sends its own stop.
- To send promptly when the heading changes by more than about 20°, toggle the
  change-detect path, or call the sender after updating facing.

**Path B** is tier 0 and the fallback.
- Hold W to move.
- Steer with the turn keys toward the desired heading, using a proportional controller
  on the heading error. This needs only a *read* of the current facing (tier 1–2), or,
  with no reads at all, the stage 1 tank controls.
- Path B is less responsive: it is limited by the turn rate. That's why it isn't the
  default.

**Spike S1** must run before building either path. Check:
- that the server moves the character along the facing we send
- whether a facing change mid-drive is honoured without stutter
- how the camera behaves in each NWN camera mode

### 5.2 Camera
- The right stick orbits (yaw) and tilts (pitch). This replaces the stage 1 pointer.
- First version: synthesise the camera keys (Left/Right, PageUp/PageDown), using pulse
  width so that stick deflection controls speed.
- Later: write the camera yaw and pitch directly, once **spike S2** finds the camera
  object. S2 also provides camera yaw for §5.1.
- D-pad up/down zooms. R3 toggles Cursor mode.

### 5.3 Soft target (replaces "point at it")
- **Candidates:**
  - Creatures, doors, placeables and items within about 25 m.
  - Taken from the client object array (tier 1: `CGameObjectArray`) or from
    add/delete hooks on the object-update handlers (tier 2).
  - Filtered to selectable objects: placeables flagged usable, doors, and creatures
    that aren't dead and hidden.
- **Auto-target:**
  - The best-scoring candidate in a cone in front of the character.
  - Score = angle × distance.
  - Hostiles score higher while in combat mode.
- **Cycling:**
  - D-pad left/right cycle by screen angle from the current target.
  - Holding LB while cycling restricts the list to hostiles.
- **Display:**
  - Overlay reticle with the target's name and a health bar (only what the game already
    shows on mouse-over).
  - The game's own highlight is driven by setting the mouse-over object the game
    already tracks (`DAT_141296f48`, tier 2 [N]).
  - The reticle needs world-to-screen projection: **spike S3**. Try camera object
    matrices first; otherwise capture the view-projection uniform through our
    `SDL_GL_GetProcAddress` → `glUniformMatrix4fv` wrapper.
  - Without projection, the name plate is pinned at the top of the screen instead.

### 5.4 Buttons in Explore

| Input | Tap | Hold |
|---|---|---|
| A | interact with target (attack, talk, open, pick up: `FUN_1406efa40`) | — |
| X | radial menu on target | — |
| Y | radial menu on self | — |
| B | cancel current action | (when nothing to cancel) game menu |
| Start | pause / unpause | — |
| Back | **hub radial**: inventory, character, spells, journal, map, rest, options, chat | — |
| LT | — | quickbar mode (§6) |
| RT | — | quickbar mode, second half (§6) |
| LB / RB | previous / next quickbar page (normal / Shift / Ctrl bars) | LB: hostile-only cycling |
| D-pad ←/→ | cycle target | — |
| D-pad ↑/↓ | zoom | — |
| L3 | highlight all objects (Tab, toggle) | — |
| R3 | cursor mode toggle | — |

Notes:
- With no target, A does nothing and the prompt bar shows "no target". It doesn't
  click at the screen centre.
- Hold actions have toggle alternatives in the options page, following the Xbox
  accessibility guidelines.
- Everything is rebindable per mode in the INI.

## 6. Quickbar
NWN has 3 bars × 12 slots. The current page is ours: LB/RB choose it, and it is shown
in the HUD.
- **Hold LT:** the HUD shows slots 1–8 of the page as a cross, D-pad = slots 1–4 and
  face buttons = slots 5–8. Pressing one fires `Fn`, or `Shift+Fn` / `Ctrl+Fn` for the
  other pages.
- **Hold RT:** the D-pad fires slots 9–12. The face buttons are user-assignable extras,
  by default: examine target, toggle stealth, toggle search, quick-save.
- Firing goes through the native quickbar keys (tier 0), so slot contents stay exactly
  what the game shows. This works for any module, and there's nothing to sync.
- Assigning to slots happens in Panel mode: focus an item, spell or feat, then press X
  and choose a slot. This avoids the console's "can't assign" complaint.

## 7. Radial mode (native radial menus)
- The stick angle maps to the eight `RadialN…NW` actions. Selection happens on stick
  return or A; B is `RadialCancel`. With a small deadzone, a flick is enough.
- An overlay wedge highlight shows where the stick points before you commit.
- Sub-radials work the same way, because they are the same control.
- **Spike S4:** confirm that the keyboard radial actions behave as described on
  creature, item and spell sub-radials, and check the
  `ui.radial.spellcasting.always-show-as-subradial` behaviour.

**As built (2026-10-29).** The keyboard RadialN…NW actions *activate* a slot
(`HandleKeyEvent` sets state 2 and runs `HandleMenuAction`); they don't highlight
one. So the mod steers the native highlight with the pointer instead:
- `CRadialMenu::HandleMouseMoveCaptured` (`0x1401f9640`) picks the slot as
  `int((π/2 − atan2(dy, dx)) × 8 / 2π + 0.5) mod 8`, where (dx, dy) is the pointer minus
  the ring centre (`+0x7dc/+0x7e0`). That makes slot 0 straight down, 2 right, 4 up,
  6 left, if GUI y runs down like SDL's (checked at runtime, see below).
- Distance band, as round(dist² / GuiScale²): ≤ 400 (20 px) centre; > 0xc40 (56 px)
  select (state 2); > 0x57e4 (150 px) submenu pause (state 3, still acts); > 90000
  (300 px) exit. The mod uses 100 px × GuiScale.
- Slot data: `CGuiIcon*` per slot at `+0x6e0` (an option exists only where one is set;
  `ActivateInternal` builds icons from it); flags per slot at `+0x1f8`, bit 0 =
  `DisableEntry`; selected slot `m_nSelectedOctant` at `+0x7cc`; state at `+0x7c8`;
  sub-radial per slot at `+0x298`; parent at `+0x2d8`.
- Snapping: the right stick chooses the nearest shown, enabled slot (10° hysteresis)
  and the pointer sits exactly on it. The first time a snap lands on a slot other
  than 2 or 6 (those read the same either way up), the mod compares `+0x7cc` with
  the slot it aimed at and logs `radial: slot order as expected` or `flipped`.
- In game (2026-10-29) the check logged `flipped` (pointed at 5, game chose 7): GUI y
  runs up, so on screen slot 0 is up and 4 is down. The mod corrects itself at runtime.
- Decompiles: `re/decompiles/radial/radial.c`.

## 8. Panel mode (navigating the legacy GUI)

### 8.1 Generic focus navigation (works on any panel)
1. Enumerate the active panels from `**g_pGuiMan`. The top-most modal panel wins;
   otherwise the most recently opened.
2. Collect the panel's interactive controls (buttons, list rows, inventory cells,
   scroll bars) with their screen rectangles. They come from the panel's tag table, or
   from hit-testing a coarse grid with `GuiObjectHitCheck` if rectangles aren't
   readable.
3. Spatial navigation:
   - D-pad / left stick move to the nearest control in the pressed direction (the
     usual nearest-neighbour cone).
   - Focus wraps within a panel.
   - Per-panel override tables let us fix awkward spots.
4. Focus is shown with an overlay ring plus the game's own `SetHilight`.
5. Buttons:
   - **A**: warp the cursor to the control and left-click. Tier 0, works on every
     panel; tier 2 alternative is calling the control's `HandleLMouseDown/Up`.
   - **X**: right-click (context radial).
   - **Y**: examine / tooltip.
   - **B**: close the panel (Escape).
   - **LB/RB**: previous/next tab, where the panel has tabs (per-panel table).
   - **Right stick**: scroll.
6. **Unknown panels** (custom haks, NUI windows from servers):
   - If enumeration finds no usable controls, switch to Cursor mode automatically and
     say so in the prompt bar.
   - NUI windows can't be enumerated, so they always use Cursor mode.

**Spike S5:** find the tag entry → `CGuiObject` pointer offset and each control's
on-screen rectangle. This is the one missing fact for 8.1.

**As built (2026-10-29).**
- Panel mode is on while a menu panel is shown and prototype mode runs, and no radial
  or conversation is over it. "Menu panel" means any panel in `**g_pGuiMan`'s list
  that isn't HUD. The HUD is a vtable list in `game.cpp`: chat prompt, quickbar,
  compass, party bar, player state bar, chat window, name bubbles, action queue,
  conversation window, tooltip (`CGuiTextBubble`) and the area load screen.
- Controls: a 12 px grid of `GuiObjectHitCheck` calls (GUI coordinates: y runs up,
  `gui y = window h - screen y`), ~700 points a frame, so about 0.3 s for 1080p. A
  hit counts if it isn't a label (`CGuiLabel`, vtable `0x140db05e8`) or a panel
  (vtable slot 8 `AsGuiPanel` non-null), and its parent chain (`+0x68`) ends at a
  non-HUD panel. Each control's box is its hit points grown by half a step.
  Inventory grids (`INV_BOX_<cols>X<rows>` models, a `CGuiInvRepository`) are split
  into cells. Every call rewrites `GuiMan+0x78` (the cached hit) and the hit info at
  `+0xe0..+0x158`; after each batch the mod hit-tests the real pointer once to put
  them back. No scanning happens while a mouse button is down (drags).
- Focus moves to the nearest control ahead (score = distance ahead + 2.5 x distance
  sideways, within a ±68° cone). It warps the real pointer there, so the game's own
  hover highlight and tooltip appear. A and X are the normal mouse bindings landing
  on the focus, so holding A while moving is a drag. The ring is drawn by the overlay.
- Moves: for a straight move, controls in line with the focus (extents overlapping
  across the move) win, nearest first; otherwise the angle score above. A control
  whose box encloses the focus (a list box behind its rows) is never a target.
- Starting focus: the first cell of a grid; else the first control in reading order
  that is at least 160 px wide and isn't a tab (`CGuiTabButton`, vtable
  `0x140e32b10`) or scroll bar; else the one nearest the pointer.
- Tabs: LB / RB tapped alone (< 0.4 s, nothing else pressed) click the previous /
  next `CGuiTabButton`, ordered along the strip they form. The click is queued two
  frames after the pointer moves there, so the GUI has seen the motion first.
- Scrolling: `SDL_MOUSEWHEEL` events at the pointer (the right stick in camera mode,
  and the d-pad when nothing lies further that way).
- Out of a game (no player creature) panel mode runs without prototype mode, so the
  main menu, Load screen and character creation get the same navigation.
- Panels seen: `CPanelInventory` (83 controls with the 60 cells), `CPanelCharSheet`
  (9: close, 3 tabs, scroll buttons and bar, the stats list box; 19 on the feats tab,
  one `CGuiCharSheetRadioButton` per visible row), the game menu (9 text buttons),
  the journal (16; quests are `CGuiQuestButton`), the main menu (7
  `CGuiCharGenButton`), `CPanelLoadGame` (18; saves are `CGuiSaveGameButton` rows on a
  `CGuiButtonListbox`, buttons `CGuiPregameButton`).

### 8.2 Per-panel refinements, on top of 8.1

| Panel | Additions |
|---|---|
| Dialogue | Replies already take the number keys 1–9. D-pad ↑/↓ highlights a reply; A sends its number. Y = examine speaker. B = end, where allowed. |
| Inventory | Grid focus treats a multi-cell item as one unit. **Lift and place** replaces drag-and-drop: A lifts (mouse down at the item), you move to the destination, A places (mouse up there). B cancels and returns the item. LB/RB switch bag pages. Equip slots form a second focus region reached past the grid edge. |
| Container / store / barter | Two regions, the player's and the other side's; LT/RT jump between them. A on an item moves it (or buys or sells it). Store quantity popups use the generic modal handling. |
| Spellbook / memorisation | Spell-level tabs on LB/RB. A on a known spell memorises it into the next empty slot. X assigns to quickbar. |
| Character sheet, level-up, character creation | Generic navigation plus tab tables. Level-up and character creation are "wizards": focus starts on the primary choice list, and Start = Next/Accept. |
| Game menu, options, save/load | Generic navigation. Save names use the on-screen keyboard. |

## 9. Text entry
- When an edit box or the chat bar takes keyboard capture, the overlay shows a gamepad
  keyboard. It is laid out as a grid with the D-pad, plus shoulder shortcuts for
  space, backspace and shift.
- Characters are sent as SDL text input events.
- Chat also gets a quick-phrase wheel from the existing VoiceChat/QuickChat (`V`)
  binding.
- A physical keyboard keeps working; the overlay hides on the first real keypress.

## 10. Cursor mode (fallback)
This is the stage 1 behaviour: the right stick moves the pointer, A is left-click and
X is right-click. It is entered with R3 or automatically for unknown panels. While it
is active, the HUD shows a "Cursor" badge, so it's always clear why the sticks behave
differently.

## 11. Polish (later)
- **Glyphs.** Xbox, PlayStation and Nintendo sets, detected per pad and switched to the
  last pad used.
- **Rumble.** XInput for Xbox pads on hit taken, critical hit and level-up. Sony
  rumble only if there's demand.
- **Steam Input.** Detect a Steam virtual pad (VID 0x28DE) and say in the log that
  "Gamepad" template mode is expected.
- **Options page in the overlay.** Deadzones, sensitivity, hold vs toggle, glyph
  override, rebinding.

## 12. Milestones
Each milestone ends with an offline harness extension and an in-game check by the user.
The mod can't be validated in-game from here.

| # | Deliverable | Tiers | Depends on spikes |
|---|---|---|---|
| M0 | **Spikes S1–S5**: a diagnostic build that logs drive messages, camera and projection candidates, panel/control enumeration and radial state, plus a hotkey to test-set facing | 1–2 | — |
| M1 | Overlay foundation: SwapWindow hook, ImGui, prompt bar, glyph detection, mode machine skeleton | 0 | — |
| M2 | Direct movement (path A, B fallback) and analog camera | 0, 2 | S1, S2 |
| M3 | Soft target: candidates, scoring, cycling, reticle, A = interact | 1, 2 | S3 |
| M4 | Radial mode and quickbar HUD | 0 | S4 |
| M5 | Panel mode, generic navigation, plus dialogue | 1, 2 | S5 |
| M6 | Inventory / container / store lift-and-place | 1, 2 | M5 |
| M7 | Spellbook, character, level-up, character creation tables; on-screen keyboard; options page | 0–2 | M5 |
| M8 | Rumble, glyph polish, Steam Input detection | 0 | — |

M0 and M1 are independent and can proceed in parallel.

## 12a. M0 status: diagnostic build (2026-10-29)

More static work while building M0 removed several unknowns. These facts are
confirmed by decompiling build 26c6e573; each one also has a runtime self-test in
`src/game.cpp`.

**S2, camera (solved without guessing).**
- The world camera is `CNWCModule+0xe8`, where `CClientExoAppInternal+0x30` is the
  module (`FUN_1407cd2c0` creates it as `NewCAurCamera("camera")`).
- The object is a `Camera` (subclass of `CAurCamera`): `NewCAurCamera` installs
  vtable `0x140d82e08` (RTTI `Camera`) over the base's `0x140d82d00`. In that vtable,
  slots 8/10/18/22/23/24 are the *exported* `Camera::GetPosition`, `MapToScreen`,
  `GetViewAngle`, `GetYaw`, `GetPitch` and `GetDist`. (M0 first used 41/43/…/57,
  the base vtable's slots, so the check failed in game. Fixed 2026-10-29.)
- The self-test compares them, so the camera check is build-independent.
- `CurrentCamera` (exported) is only "the last camera rendered" and can be a GUI
  camera, so it isn't used.

**Coasting after release (2026-10-29).**
- How the server moves you (`AIActionDrive`, `0x1403b8b60`):
  - Each DriveControl replaces your actions with a drive action (type `0x33`).
  - That action plants a waypoint `run rate × 400 / 1e6` ahead, about 0.4 s of
    movement (~1.8 m at run speed). Tiles have nothing to do with it.
  - `DriveUpdateLocation` walks the character to the waypoint, then re-adds the action
    while the chain count at `node+0x60` is still ≥ 2.
- Releasing the forward key sends AbortDriveControl, which only zeroes that count, so
  the character still walks on to the current waypoint.
- The server runs in-process, on the same thread as the mod. So on release, the mod
  calls `CServerExoApp::GetCreatureByGameObjectID` (with the high bit cleared), zeroes
  the chain count, and moves the waypoint (`creature+0x530 → +0x80`) to where the
  *client* shows the creature at release.
- The first version used the server's own position (`+0xa4`). The server trails the
  client's prediction slightly, so the client was pulled back to the server's spot
  with no animation, which the user saw as a slide. With the waypoint at the client's
  position, the server walks that last bit instead. Gaps over 2 m fall back to the
  server's position.
- That still slid, as the stop trace (`trace_stops`) showed:
  - After release, the client draws the character 5–6 cm further for a frame.
  - Its server-confirmed position, `CNWCCreature+0x2c0`, stays at the stop point.
    The drive prediction (`0x1406ea9e0`) also starts from there.
  - The drawn position `+0x38` then eases back to `+0x2c0` over ~0.5 s while idle.
- Fix: for 0.6 s after release, once the server has no action,
  `SyncServerToClient` writes the drawn position into `+0x2c0` and calls
  `CNWSObject::SetPosition` on the server creature. It only does this for gaps under
  0.5 m.
- It repeats this for 0.25 s in case a drive update already in flight re-adds the
  action. The `stop_on_release` setting turns it off.
- It does nothing on a remote server.
- The server also snaps the creature to the position the client sends in DriveControl
  when they differ by more than 0.5 m (`ComputeSafeLocation`).

**Walk vs run in drive mode (2026-10-29).**
- The keymap handler is `FUN_1406ed720(app, action, down, passthrough, fromKeyboard)`.
  Its action ids are `keymap.2da` row numbers, and the switch goes through the byte
  table at `0x1406ef758`.
- Action 89 is DriveModeForward: case 0x2a sets `app+0x1b4`. It sends walk (drive flag
  1), unless the int at `*(app+0)+4` is set.
- Action 90 is DriveModeForwardRun (the stock W key): case 0x2b sets `app+0x1b8` and
  sends run (flag 3).
- Releasing either sends AbortDriveControl.
- The mod calls action 89 directly for a partial stick push.

**Targeting v2 (2026-10-29, replaces v1 below).**
- **The patch:** `game::InstallPickHook` detours the world pick `FUN_1406eae20` and its
  spell-target variant `FUN_1406ebcf0`.
  - Both start with the same 15-byte prologue: `mov rax,rsp` plus 8 pushes, with no
    rip-relative bytes.
  - The patch is a 14-byte `jmp [rip]` plus a NOP. The trampoline is a copy of the
    prologue plus a jump back.
  - The patch is known-build only, and the bytes are compared before patching.
- **While active,** the pick reports the controller's target as a kind-2 hit (object,
  its position, kind 1).
  - The hover update (`FUN_1406f2830`) then highlights and labels it natively.
  - Left click runs `FUN_1406efec0`, the default action.
  - Right click runs `vtbl[0x148]`, the radial.
- **The pointer is parked at screen centre,** so the GUI check in the click handlers
  sees the world.
- **With no target,** the normal pick reports nothing. The spell-target variant falls
  through to the real pick.
- **Eligibility, client-side,** exactly as the click code decides:
  - The type cast goes through vtable slots `+0x58` (creature), `+0x28` (door),
    `+0xa0` (placeable) and `+0x68` (item); the item cast can return null.
  - Usable: the int at `+0x11c`, or `+0x164` for items.
  - Hostile: the byte at `+0x388` > 1.
  - Associate: master OID at `+0x270` == the player.
  - Dead: `CNWSObject::GetDead` via the local server.
- **Choosing the target:**
  - Aim comes from the stick direction, or camera forward when the stick is idle.
  - Score: degrees off aim + 4 × metres, ×0.7 for hostiles.
  - It re-picks every 0.1 s within a 60° cone, with 5 m range for interactables and
    15 m for hostiles.
  - The current target is kept within 1.3× range and 1.5× cone. A challenger must beat
    it by 20% for 0.3 s.
  - The d-pad cycles by signed angle and locks the pick.
- **Mouse takeover:** a real mouse movement pauses targeting until the left stick or
  the d-pad is used again.

**Targeting v1 (2026-10-29, rejected).** Usability came from the in-process server, by
exported lookups:
- **Creatures:** `GetCreatureByGameObjectID`, then `CNWSObject::GetDead` and
  `CNWSObject::GetAIStateReaction(player)` (2 hostile, 1 friendly, 0 neutral, as in
  `ExecuteCommandGetIsReactionType`).
- **Placeables:** `GetPlaceableByGameObjectID`, then the int at `+0x264` (as in
  `ExecuteCommandGetUseableFlag`).
- **Doors:** `GetDoorByGameObjectID`, then `CNWSDoor::GetIsClickable`.
- **Ground items:** always usable.
- **Remote server:** placeables are skipped.

How targets are picked and used:
- The soft target is the lowest `dist × (1 + 2(angle/cone)²)`, ×0.6 for hostile
  creatures, within 10 m and 70°. It re-picks every 0.1 s and switches only when
  another candidate scores 25% better.
- The d-pad cycles on-screen candidates within 30 m and locks one.
- The pointer is pinned to the target's aim point, so the stock left and right clicks
  act on it (A interacts, X opens the radial). With no target, A and X are swallowed
  while the stick is in camera mode.

**Camera rotation (right stick, 2026-10-29).**
- `Camera+0x458` is the camera's behaviour, a `CAurBehaviorCameraFollow`. Its vtbl[8]
  is the exported `AsBehaviorCameraFollow`, which is how the mod checks it.
- Its fields, as named by its `Message` parser (`0x140056740`), are: dist `+0x48`,
  yaw `+0x54`, turn rate `+0x58`, pitch `+0x5c`, tilt rate `+0x60`, the clamp
  flags/min/max at `+0x64..+0x84`, and the one-shot deltas `nonFPSturn` `+0x88` and
  `nonFPStilt` `+0x8c` (all in degrees).
- `Control` (`0x140055100`) adds the deltas once, zeroes them, then clamps.
- `game::RotateCamera` adds `stick × speed × dt` to those deltas. That gives analog,
  frame-rate-independent rotation, and the game's own pitch and zoom limits still
  apply.
- Only `HandleServerToPlayerCamera` (the script `SetCameraFacing`) writes yaw by
  message, so normal play doesn't overwrite the deltas.

**Quickbar (2026-10-29).** Decompiles in `re/decompiles/quickbar/` (`qb1.c`, `qb2.c`).
- The panel is `*(CClientExoAppInternal+0x90)+0x60`, a `CPanelQuickBar` (vtable
  `0x140e52900`, checked). The keymap handler fires actions 5–16 (F1–F12) through
  `FUN_14085c3b0(panel, n)`, which clicks button `*(panel+0x6510) + n × 0x2c8`. So
  `+0x6510` points at the first button of the page shown now.
- All 36 `CGuiQuickButton`s sit inline from `+0xf0`, `0x2c8` bytes apart (the
  destructor `0x140858bf0` iterates them). 0–11 is the first page, 12–23 Shift,
  24–35 Ctrl. This is the client's copy, so it works on any server.
- Button fields, as its tooltip handler `0x14085e9b0` reads them:
  - type byte `+0x140`, the server's quickbar types: 0 empty, 1 item, 2 spell,
    3 skill, 4 feat, 0x12 command, 0x2c spell-like ability, and so on
  - item OID `+0xec` (second item `+0xf0`), item-property index `+0x118`
  - spell: a qword at `+0x110`, spell id in the low 16 bits; metamagic is
    `(q >> 31) & 0x3f` and the class index sits in bits 28–29 and 36
  - command label: a CExoString at `+0x168`
  - any other type: the CExoString at `+0x100` if set, else the strref at `+0x70`. The
    feat and skill setters (`0x140867070`, `0x140868910`) store the name strref there.
  - icon resref: a CResRef at `+0x11c`; the icon model is at `+0x130`
- Item names (`FUN_14083d8a0`): the client item is the object array entry cast by
  vtbl `+0x68`. Once identified (`+0x1c0`), its name is the CExoString at `+0x290`;
  otherwise the base item's (`+0x40` → `CNWBaseItemArray::GetBaseItem`, strref at +0).
- Spell names: `CNWSpellArray::GetSpell` on `g_pRules+0xd0`, strref at `CNWSpell+0x10`
  (the icon resref is at `+0x18`).
- Strings come from the exported `CTlkTable::GetSimpleString(g_pTlkTable, strref)` and
  are freed with the exported `~CExoString`. The mod caches each slot's name until the
  button's fields change.

**Conversation window (2026-10-29).** Decompiles in `re/decompiles/dialog/`
(`dlg1.c`, `dlg2.c`, and `range.c`, which covers 0x14087d000–0x140880800).
- The in-game GUI `*(CClientExoAppInternal+0x90)` holds the `CGuiInGameChatDialog` at
  `+0x70` (vtable `0x140e55680`, checked), next to the quickbar at `+0x60`.
  `FUN_1406fb560` creates it when `+0x70` is null. When a conversation ends the game
  destroys it and clears `+0x70` (seen in game 2026-10-29), so each conversation gets
  a fresh window at its default place. While it's up, `+0xa0` is set; the mod also
  requires it to be in the GUI's shown-panel list.
- Layout (constructor `FUN_14087d1e0`, size `0xbc8`): speaker label `+0x140` (string
  object `+0x1d0`), NPC text `CGuiEditScroll` `+0x1d8` (string object `+0x268`, set by
  `FUN_14087e7a0`), reply picklist `+0x5e0` (`CGuiInGameChatDialog_CustomPicklist`;
  its line → reply table is an int array at `+0x5e0+0x418`).
- Replies (`FUN_14087e7e0`, called by `FUN_1406fb8e0` with the server's list):
  - texts: a CExoString array at `+0x118`
  - ids: an int array at `+0x100`
  - flags: at `+0x128`; bit 0 means the reply can't be chosen
  - count: `+0x120`
  - With no replies the game adds its own (strref 0x6ce) and sets `+0x138` bit 0.
- Choosing: `FUN_14087e750(panel, i)`. It returns early if `+0xbbc` is set, if
  `i >= count`, or if the reply's bit 0 is set. Otherwise it calls the owner's callback
  (`+0xf0`/`+0xf8` = in-game GUI, `FUN_1406f81e0`) with `i`, or -2 when the game supplied
  the reply. Clicking a reply (`0x14087e1f0`) and pressing number key n
  (`FUN_1406f81c0`, which passes n−1) both end here; -3 aborts.
- Text: the speaker label's text object is at `+0x1d0` and the NPC line's at `+0x268`
  (the entry `CGuiEdit` at `+0x1d8` keeps it at its `+0x90`). Both are StringGobs. vtbl
  `+0xe8` is `GetTextLineCount` and `+0xe0` is `GetTextLine(i)`; the mod checks both
  against the exports. The exported `CGuiEdit::GetText` concatenates the wrapped lines
  with nothing between them, so the mod reads the lines itself and adds a space at
  breaks that have none.
- Not used yet: the reply ids, and highlighting a line in the stock list (picklist vtbl
  `+0x108(line, on)` on `+0x90`, range `+0x4c0/+0x4c4`).

- Speaker and portrait: the dialog message handler (`FUN_140796440`, which calls
  `FUN_1406fb560` at `0x14079666e`) reads three ids. The second is the speaker. The
  handler stores it on the player's client creature at `+0xdc` (cleared when the
  conversation ends) and passes it to `FUN_1406fb560` for the portrait and the name.
  The portrait comes from the object (vtbl `+0x18`) and, for creatures (vtbl `+0x60`
  non-null), from `AsNWCCreature` (vtbl `+0x58`). The portrait base is a CResRef at
  stats (`creature+0x3b0`) `+0xb4`. Other objects return their portrait from vtbl
  `+0x1d0(out, size)`. `FUN_14084c2f0(base, out, size)` appends the size suffix,
  trying `size`, then size±1, ±2 and so on until a resource exists. The suffixes for
  0–5 are none, T, S, M, L, H. The window asks for M; the default base is
  `po_hu_m_99_`. `FUN_14084c550(model, resref)` puts the portrait on the panel model's
  `gui_replace01` mesh.
- Loading a texture's pixels: the exported `CAuroraTexture(CResRef, int)` (0x30 bytes;
  `+8` = CResTGA), `LoadTexture` and the destructor. `CAuroraInterface::GetRaster`
  does the same but leaks the object and hides the header. `CResTGA::
  OnResourceServiced` (`0x1401a4730`) handles TGA types 2, 3, 10 and colour-mapped
  1/9. It expands RLE and swaps BGR to RGB in place. The pixels are at `+0x80` and
  the raw 18-byte header at `+0x88`; rows stay in file order, so header byte 17 bit
  5 says whether the top row comes first. DDS goes through
  `CAuroraInterface::GetCompressedRaster` instead (not used yet).
- Hiding the window: the exported `CGuiPanel::SetPositionScreen(x, y)` moves it (via
  `CGuiMan::MapScreenToGuiScene`), and `CGuiPanel::SetPosition(Vector)` restores the
  model position saved from model vtbl `+0xc0`.

**Pointer hiding (2026-10-29).** The game uses SDL's cursor (`SDL_CreateColorCursor`,
`SDL_SetCursor`, `SDL_ShowCursor`). The mod hooks `SDL_ShowCursor` in the dynapi table,
records what the game last asked for, and shows the cursor only when the game wants
it and the mod isn't hiding it.

**S3, projection.** The exported `Camera::MapToScreen(Vector, bool normalized)`
projects world to screen and accounts for the UI scale. There's no need to capture GL
matrices.

**Objects.**
- The client object array (`app+0x28`) is a hash table:
  - buckets at `+0`, buckets per half at `+0x1c`, total buckets at `+0x24`
    (two halves on the client)
  - each node is `{u32 id; obj @+8; next @+0x10}`
  - IDs with bit 31 set hash into the upper half (`count + id % count`). Every
    server object has that bit, because `WriteOBJECTIDServer` ORs in `0x80000000`.
    That includes the player OID at `app+0x48` (initialised to `0x7f000000`). M0
    searched only the lower half, so it never found the player. Fixed 2026-10-29.
- Objects:
  - fields: type byte `+0xc` (5 = creature), OID `+8`, area `+0x30`, position
    `+0x38`, orientation vector `+0x44`
  - `AsNWCObject` and `AsNWCCreature` return `this`

**S1, drive mode.**
- `vtbl[36]` (`FUN_140766220`) is `SetOrientation(Vector*)`: it writes `+0x44` and
  updates the model.
- The client's local drive prediction (`FUN_1406ea9e0`) moves along `+0x44`.
- `FUN_1406f1e40` sends DriveControl with that facing, immediately if the int at
  `app+0x1b0` is set, otherwise every 250–500 ms.
- Remaining open question: how the server and camera modes behave. The prototype
  logs this.

**S5, GUI.**
- Every control is a 3D model in a GUI scene. Its screen position is
  `CGuiMan::MapGuiSceneToScreen(model->GetPosition())`.
- `GuiObjectHitCheck(x, y, force, 1)` is the game's own pick.
- The snapshot dumps:
  - panels, with class names from a generated vtable table
  - tag tables, with raw fields, so the tag → control link can be read off
  - a hit-test grid, giving each control's on-screen box

**The M0 build (`src/game.*`, `src/diag.*`, prototype in `controller.cpp`).**

| Trigger | Action |
|---|---|
| Tap L3+R3, or Ctrl+Shift+F10 | Snapshot to the log |
| Hold L3+R3, or Ctrl+Shift+F11 | Toggle prototype mode |

In prototype mode:
- Left stick: camera-relative drive. `facing` mode uses `SetOrientation` + W + forced
  sends, and falls back to `steer` with W and A/D on unknown builds. Shift is held
  below 60% deflection.
- D-pad ←/→: pins the pointer to the next on-screen object via `MapToScreen`, so
  stage 1's A click acts on it.
- Every 0.5 s the log records wanted vs actual heading, movement direction and speed,
  camera yaw/pitch and the drive flags.

## 13. Risks

| Risk | Mitigation |
|---|---|
| A game patch moves tier-2 functions and offsets | Signature scan, per-build self-tests, per-feature fallback; the log lists what turned off |
| Setting facing locally desyncs from the server, or a server rejects the drive | S1 measures it. The server already accepts client facing in DriveControl, so we send nothing the game doesn't. Fallback is path B. |
| GL state corruption from the overlay | Use ImGui's state backup; draw last, right before swap; test with HDR/AA options on and off |
| Panel controls not enumerable for some panels | Automatic Cursor mode for that panel |
| Multiplayer etiquette | The mod reads only what the client already has and sends only messages the stock client sends. Document this in the README. |
| Overlay hooks clash with ReShade, Discord, Steam overlay | Our hooks are SDL slots, not WGL, so they don't collide. Still test, and document Discord's known toolset crash. |

## 14. Open questions for the user
1. In the official campaigns, is pausing (Start) and acting while paused the main way
   you play? That decides whether actions issued while paused queue visibly in the HUD.
2. Should party/henchman control matter? NWN's associate commands live in the radial
   menu, so they come for free, but a dedicated associate wheel could be added.
3. Is matching the NWN look important for the overlay, or is a clean modern style fine?
