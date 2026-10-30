# Status and decisions

This is a running log so work isn't lost between sessions. Newest entries are at
the top of each section. Dates are absolute. How to work on the mod is in
`docs/MAINTAINING.md`.

Naming: entries before 2026-10-30 say "prototype mode". It has been controller mode
since then (still `proto` in the code), and its settings moved from `[diag]` to
`[play]`.

## Project (2026-10-30)
- **Public repo:** https://github.com/jwgellard/ControllerSupport-NWN (MIT). Game-derived material (`re/ghidra_proj`, `re/decompiles`, `re/data`) is gitignored and must never be pushed; commits use the GitHub no-reply email and UTC timestamps.
- **Controller mode is the default.** `[play] mode = controller` (or `classic`) replaces `[diag] prototype_on_start`; older inis still load (harness check). The mode switch (hold L3+R3, Ctrl+Shift+F11) no longer depends on `[diag] enabled`, which now only covers snapshots. Self-tested: starts in controller mode after loading a save, L3+R3 hold switches to classic and back.
- **Documentation for maintainers:** `docs/MAINTAINING.md` (setup, architecture, safety rules, debugging, testing, porting to a new game build, releasing, glossary), `CHANGELOG.md`, a map at the top of `controller.cpp`, `.gitattributes`.
- **Next:** the user plays an hour pad-only and lists every point that needed the mouse or keyboard. That list orders the backlog below.

## Working (confirmed in game by the user)
- **Stage 1 (2026-10-29): key and mouse emulation.** The pad drives the game through synthesized SDL key and mouse events. There are four layers, with LB and RB as modifiers; bindings are in the ini.
- **Direct movement (prototype mode).** The left stick is camera-relative: the character turns to the stick direction and runs. The mod sets facing through the creature's `SetOrientation`, holds W, and forces DriveControl sends.
  - On at load by default (`[play] mode = controller`). Holding L3+R3 or pressing Ctrl+Shift+F11 switches to classic and back.
  - The user's verdict: "the movement is good".
- **Stopping dead on release.** The user's verdict: "perfect".
  - Problem 1: the server's drive waypoint sits about 0.4 s ahead, so the character coasted after release.
  - Fix 1: on release the mod ends the in-process server's drive action.
  - Problem 2: the client draws one frame past that point and then eases back, which looked like a slide or step backwards.
  - Fix 2: for 0.6 s after release, the mod syncs the server position and the client's server-confirmed position (`+0x2c0`) to the drawn position.
  - Single player only. The setting is `stop_on_release`.
- **Right-stick analog camera.** It writes the follow camera's one-shot yaw and pitch deltas.
  - Clicking R3 toggles the right stick between camera and pointer. The user chose this over auto-switching.
  - With no game camera (main menu), the stick moves the pointer.

## In progress: targeting (the user wants it next)
- **Attempt 1 (2026-10-29), rejected.** The user: "nothing about it was really good, the d-pad didn't really target things". It's kept behind `targeting = 0`. How it worked:
  - It auto-picked from the character's facing (10 m, 70° cone, hostiles first).
  - It showed the target by warping the mouse pointer onto it.
  - The d-pad cycled everything on screen within 30 m.
- **Lessons from attempt 1:**
  - Never show the target by moving the pointer.
  - Keep party members and companions out of targeting.
  - The d-pad cycled through far-away objects and objects behind the player.
  - The facing-based pick flickered onto the companion next to the player.
- **Research (2026-10-29)** is in `docs/research/`:
  - `targeting_design.md`, from other games. It recommends:
    - a soft target aimed by the stick direction, or camera forward when the stick is idle
    - a whitelist that excludes the party
    - hysteresis: a challenger must win by about 20% and hold for 0.3 s
    - the mod's own marker, name plate and prompt
    - buttons: A for the default action, a separate button for the radial, bumpers to cycle, and hold to label everything nearby
  - `re_console.md`: there's no console code to reuse (already known).
  - `re_pick.md` (pending): how the game picks, highlights and acts on objects, and opens radials, so the mod can drive them without the pointer.
- **Targeting v2: working. The user's verdict 2026-10-29: "this is looking good"** (after the range increase). The user approved the plan below.
  - Possible later additions: hold a button to label everything nearby (helps in dense areas), a hard lock-on for combat, and a proper on-screen marker with the overlay UI.
  - Code: `game::InstallPickHook` / `SetPickTarget` / `TargetInfoFor` (`game.cpp`) and `UpdateTargeting` / `CycleTarget` (`controller.cpp`).
  - The harness check "pick-hook detour round trip" tests the patch mechanism.
  - Settings in `[play]` (`[diag]` before 2026-10-30): `targeting`, `target_cone`, `target_interact_range`, `target_hostile_range`.
  - First in-game run (20:12) worked: hooks installed, and doors and creatures were targeted and locked.
  - The user wanted more reach, so the ranges went from 5/15 m to 10/25 m (interactables/hostiles). Changed in the ini and the defaults.
  - Log lines: `game: pick hooks installed` and `target: ...`.
- **Targeting v2 plan.** Mechanism, verified:
  - The game's world pick `FUN_1406eae20(app, CGameObject** out, Vector* pos, int* kind, int flags)` has a variant `FUN_1406ebcf0`.
  - It feeds three callers: the per-frame hover update `FUN_1406f2830` (highlight and name label), the left click `FUN_1406f02c0` → `FUN_1406efec0` (default action), and the right click `FUN_1406f10e0` (radial).
  - Detour the pick to return the controller's target as a kind-2 hit. The game then highlights it, shows its name and runs A/X on it natively, with no pointer movement.
  - Radial at screen centre: set `g_rmUseScreenCenter` (`0x14140cd5c`).

  Picking the target:
  - Aim with the left-stick direction, or camera forward when the stick is idle.
  - Score: angle in degrees + 4 × distance in metres, with hostiles favoured.
  - A challenger must win by 20% and hold for 0.3 s before the target switches.
  - Party and companions are excluded.
  - Per-type ranges: about 4–5 m for doors, containers, items and talking; about 15 m for hostiles.
  - The d-pad cycles by angle around the aim direction.

## Walk on a half push: working (the user confirmed 2026-10-29)
- Cause: the stock W key is keymap action 90, DriveModeForwardRun, which always runs. Shift doesn't affect drive mode.
- Walking is action 89, DriveModeForward, which has no default key.
- Fix: below `walk_threshold`, the mod calls the game's keymap handler `FUN_1406ed720(app, 89, down, 0, 0)` instead of holding W, then forces a drive send when the pace changes.
- Check: the `proto: stick=... walk` log lines should show about half the run speed.

## UI: hybrid custom overlay (the user's choice, 2026-10-29)
- **The approach:** custom controller-first screens where the stock UI is worst for a pad, plus focus navigation over the remaining stock menus. Screens get replaced one by one.
  - The user wasn't sure which screens matter most, so the order is: overlay → prompt bar → stick radials and quickbar HUD → dialogue picker → panel navigation.
- **Step 1, overlay foundation: built and installed 2026-10-29 20:29, awaiting the in-game look.**
  - Dear ImGui 1.91.5 lives in `third_party/imgui`.
  - `src/overlay.cpp` hooks `SDL_GL_SwapWindow` through the dynapi slot. It detects the GL profile, rebuilds if the GL context changes, and every frame is SEH-guarded.
  - The harness renders it on a real GL context: NVIDIA GL 4.6 compatibility profile, `#version 130`.
- **Step 2, prompt bar: built.** It sits at the bottom centre and shows:
  - A plus the verb for the target (Attack, Talk, Open, Use, Pick up)
  - X Options, Y Self, R3 Pointer or Camera
  - a badge: Locked, Mouse or Pointer

  Preview image: `docs/images/overlay_prompt_bar.png`. The ini section is `[overlay]` (`enabled`, `scale`).
- **Step 3a, stick-driven radials: built 2026-10-29, awaiting the in-game test.**
  - In prototype mode the mod sets `g_rmUseScreenCenter` (`0x14140cd5c`) to 1, so every radial, sub-radials included, opens at screen centre. The game's value is restored when prototype mode turns off.
  - While `static_nMenuDepth` > 0, the stick puts the pointer out from the centre in its direction. The game selects by angle, so it highlights that option itself.
  - The pointer stays on the last option when the stick lets go. A clicks it (left click); B is Escape.
  - Movement and the camera are paused while a radial is open. Prompts: Stick Choose / A Select / B Back.
  - First test (2026-10-29): B (Escape) closed the whole stack, and the user doesn't want the left stick steering. Fixed:
    - Only the right stick steers.
    - B calls the exported `CRadialMenu::HandleKeyEvent(radial, 51 RadialCancel, 1)` on the mouse-capture object. The mod checks it's a radial by finding that function in its vtable.
    - The parent-return flag at `0x14128f4a0` is set while prototype mode runs. With it, `HandleMenuAction` reopens the parent.
    - Retest 2026-10-29: the user confirmed both ("Yep this is good").
  - Snapping (built 2026-10-29; the user said "Okay great", no test report yet). The user asked for the stick to snap to options instead of moving freely.
    - The stick picks the slot nearest its direction, skipping empty and disabled slots, with 10° of stickiness. The pointer sits exactly on that slot, 100 GUI px × GuiScale out (the game accepts 56–150). `radial_radius` is gone from the ini.
    - Slot layout and offsets are in DESIGN.md §12a (radial slots).
    - The mod checks the slot order once against the game's highlight and logs `radial: slot order as expected` (or `flipped`, and corrects itself).
- **Step 3b, layer prompts: built.** While LB and/or RB is held, the prompt bar lists that layer's bindings, labelled from the ini (Slot n, Inventory, Spellbook, etc.). It wraps at 6 per row. Preview: `docs/images/overlay_layer_prompts.png`.
- **Overlay look (2026-10-29):** the user: "looks okay, good enough for a prototype and we can improve later maybe". Layer prompts: "Yes, fine".
- **Radial snapping:** the first in-game run logged `radial: slot order flipped (pointed at 5, game chose 7)`, so the self-correction ran (GUI y runs up). Noted in DESIGN §7.
- **Step 4, quickbar names: done 2026-10-29.** First run (21:10) worked: `quickbar: slot names on (page 0)`, and the snapshot read Stealth Mode, Detect Mode, Barbarian Rage (1x per day) and Power Attack. The user said "Continue".
  - The user's choices: no always-on quickbar strip, no icons ("names are enough"), keep the LB/RB bindings (the LT/RT scheme in DESIGN §6 is not wanted).
  - Follow-up: a long name drops its trailing "(...)" note before being cut, so "Barbarian Rage (1x per day)" shows as "Barbarian Rage".
  - While LB/RB is held, each quickbar binding shows what's in the slot (item, spell, feat, skill, mode, command label) instead of "Slot n". "Empty" for empty slots; "Slot n" if the panel can't be read.
  - It reads the client's `CPanelQuickBar`, which holds all 36 slots, so it works in multiplayer too. Offsets are in DESIGN §12a (quickbar).
  - Names: colour tags stripped, Windows-1252 → UTF-8, cut at a word to 20 characters. Prompt rows now wrap by width as well as at 6.
  - Log: `quickbar: slot names on (page 0)`. The snapshot lists every filled slot.
  - Not yet: icons, metamagic prefixes, the spell an item property casts (items show the item's name), use counts.
- **Step 5, dialogue picker: built and installed 2026-10-29, awaiting the in-game test.**
  - While a conversation window is open (prototype mode), the overlay lists the replies above the prompt bar, numbered. Replies the game won't take are dimmed and skipped. Long lists scroll, with arrows where more are hidden.
  - Controls: d-pad or left stick up/down moves the choice (hold to repeat, wraps); A picks it through the game's own reply handler (the same call as clicking it or pressing its number key); B keeps Escape (ends the conversation). X, Y and the d-pad's other uses do nothing while it's open, and movement and targeting pause.
  - The stock conversation window stays as it is; the NPC's line is read there.
  - Log: `dialog: conversation open`, `dialog: N replies`, `dialog: chose reply N`, `dialog: conversation closed`, plus a one-off `game: probe dialog: window ...`. The snapshot lists the open conversation's replies.
  - Text: `<StartAction>`/`<StartCheck>`/`<StartHighlight>`/`</Start>` and colour tags are stripped; line breaks folded. That cleaner is now `src/text.cpp` (shared with the quickbar names).
  - Preview: `docs/images/overlay_dialog_picker.png`. Offsets: DESIGN §12a (conversation window).
  - The user's verdict (2026-10-29): "The dialogue picker is looking good." They asked for the NPC's text in the overlay too, because the stock window reads badly from the couch.
- **Step 5b, NPC text in the overlay: built and installed 2026-10-29, awaiting the in-game test.**
  - A box above the replies shows the speaker's name (bronze) and what they say, at 27 px (1080p). It takes up to 40% of the screen height; longer text scrolls with the right stick, and the prompt bar shows "RS Scroll". The camera waits while a conversation is open.
  - The text comes from the window's own text objects (DESIGN §12a, conversation window). Their lines are joined with a space where the break had none. Unverified: the log line `dialog: <speaker> says '...'` and the snapshot's `npc line` entries show whether words join correctly.
  - If the text can't be read, the log shows a `game: probe dialog: text object ...` line.
- **Pointer hidden when idle (the user's request, 2026-10-29): built and installed, awaiting the test.**
  - In prototype camera mode, the pointer hides while nothing is targeted and while the picker is up. It comes back for a target (as the game's action cursor), in pointer mode, in radials, when the real mouse moves, and when prototype mode is off.
  - How: `SDL_ShowCursor` is hooked through the dynapi table. The game's own show/hide requests are kept, and the mod only hides on top of them.
  - Setting: `hide_idle_pointer` in `[play]` (default 1; `[diag]` before 2026-10-30).
- **Verdict 2026-10-29 on 5b and the pointer:** "All of it is looking good." The log confirmed that the window's wrapped lines keep their trailing spaces, so the text joins correctly (`npc line` entries in the snapshot). The user asked for the stock conversation window to be hidden and the speaker's portrait to be shown in ours.
- **Step 5c, stock window hidden + portrait: built and installed 2026-10-29, awaiting the test.**
  - While the picker shows a conversation, the game's conversation window is moved off-screen (`CGuiPanel::SetPositionScreen`). It is not deactivated, because that could end the conversation. If the game moves the window again, the mod follows. The window is put back when the conversation ends, prototype mode goes off, the pad disconnects, or the overlay isn't drawing. Setting: `hide_stock_dialog` in the new `[overlay]` ini section (which also holds `enabled` and `scale`; until now these were read from the ini but missing from it).
  - The portrait sits to the left of the NPC text, at 180 px (1080p). It's the speaker's portrait at size L (128x200), picked the way the window picks its own (DESIGN §12a). It's loaded as a TGA through the game's `CAuroraTexture` and uploaded to our own GL texture. Portraits that exist only as DDS aren't loaded yet; the log then says `dialog: portrait ... couldn't be loaded`.
  - Log: `dialog: moving the game's window out of sight (it was at x, y)`, `dialog: game window put back`, `dialog: portrait <resref> (WxH)`.
  - The user's own run (21:48) logged the window moved away and portraits for every speaker; then "All of it is looking good" for 5b. They went to bed and asked Claude to test in game and carry on.
- **Self-test of 5c by Claude, 2026-10-29 22:00 (SoU save, Dorna):** the game's window was gone and our box showed Dorna's portrait, name and line; d-pad moved the choice, A picked replies (next line and "Continue" arrived), B ended the conversation. Turning prototype mode off mid-conversation put the game's window back where it was. Fixed on the way: portraits are padded to a power of two (L = 128x200 picture in a 128x256 texture), so the empty bottom strip is now cropped. Learned: the game destroys its conversation window when a conversation ends and makes a new one next time, so there's nothing to put back after an ordinary close.
- **Pointer hiding, self-tested 2026-10-29:** hidden with nothing targeted, shown with a door targeted, shown in pointer mode (R3), hidden again back in camera mode (checked with `GetCursorInfo`).
- **Step 6, focus navigation over the game's menu panels: built 2026-10-29 and self-tested, awaiting the user's verdict.**
  - When a menu panel is open (anything shown that isn't the HUD: inventory, character sheet, the game menu, popups), the mod finds its controls by hit-testing the screen on a 12 px grid, ~700 points a frame (about 0.3 s for 1080p), and keeps the list fresh (every 1.5 s, and 0.25 s after a click).
  - D-pad or left stick moves focus to the nearest control in that direction (held, it repeats). A bronze ring marks it, and the real pointer sits on it, so the game shows its own hover highlight and tooltip. A / X click there (A held while moving drags: lifting and dropping items works). B is Escape (closes the panel). The right stick moves the pointer freely; focus follows it. Movement and targeting wait; the pointer stays visible.
  - The inventory's backpack is one control (`INV_BOX_10X6`); the mod splits it into its cells. A panel with a grid starts on its first cell; others start on the control nearest the pointer.
  - Focus survives a radial or conversation opened over the panel (e.g. X on an item opens its radial; B returns to the same item).
  - Self-tested: inventory (moved between cells, tooltips, X opened the item radial, a potion dragged one column), character sheet (close button, tabs, scroll buttons, the list box), the game menu (moved to Resume Game, A resumed).
  - Log: `panel: <class> open`, `panel: N controls found in M ms`, `panel: focus i/N <class> cell c at (rect)`, `panel: menu closed`.
  - Added the same night, all self-tested:
    - LB / RB tapped on their own switch to the previous / next tab (character sheet, journal, inventory bags); held, they're still the quickbar layers. Tabs are ordered along their strip (the selected tab sits proud of the others). The prompt bar shows "LB/RB Tabs" when a panel has tabs.
    - Right stick (camera mode) scrolls the list under the focus with mouse-wheel notches (faster the further it's pushed); after R3 it moves the pointer freely. The d-pad also scrolls when nothing lies further that way. The prompt bar shows "RS Scroll" or "RS Pointer".
    - Starting focus: the first backpack cell on inventory-style panels; otherwise the first control at least 160 px wide that isn't a tab or scroll bar (journal: first quest; character sheet: the stats list; game menu: Game Options).
    - Moves prefer controls in line with the focus (extents overlapping across the move), so right from Delete on the Load screen goes to Load, not to the nearer save row above; and never onto a backdrop that encloses the focus (the list box behind its rows).
    - Out of a game (main menu, Load screen, character creation) panel mode runs even though prototype mode can't: self-tested a pad-only load from the main menu (d-pad to Load, A, A on the save, d-pad to Load, A). The left stick sends no movement keys while a menu is up.
    - NUI windows (the module browser behind New, and anything a server builds with NUI) can't be hit-tested: a menu panel whose scan finds no controls doesn't count, so the ordinary pointer stays (self-tested: the right stick moves the pointer in the module browser; B back to the main menu, where focus navigation resumes).
    - More always-on panels are treated as HUD: floating speech (`CGuiInGameFloatyText`) and screen fades.
    - Regression pass with the final build: walking and stop-dead, LB quickbar names, conversation, inventory, character sheet, game menu, pad-only load from the main menu.
  - Not yet: per-panel tuning (DESIGN §8.2), containers and stores (not tried: the only placeable near the test save was a pressure plate next to a kobold ambush), character creation (it starts in the NUI module browser, so pointer only), the load screen counting as a menu while loading (harmless), lists can't tell when they're at their end (d-pad leaves the list instead of scrolling when something lies below it). All base-game portraits are TGA (6450 in nwn_base.key), so DDS portrait loading only matters for custom content.
- **Testing aid: scripted virtual pad (2026-10-29).** `[diag] inject = 1` makes the mod play `nwn_controller_inject.txt` (next to the ini) whenever the file changes: `press`, `down`/`up`, `stick`, `wait`, `snapshot`, `log`; see `src/inject.h`. With `controller_mod/tools/gamectl.ps1` (screenshot, focus, click, key, cursor state) and `tools/launch_load.ps1` (start the game, skip the intro, load the newest save) Claude can test in game unattended. Claude set the user's live ini back to `inject = 0` at the end of the session; turn it on again to test.
- **Next:** the user's verdict on step 6 and on the dialogue screen as a whole. Then per-panel work, asking which screens hurt most (DESIGN §8.2). See `docs/HANDOVER.md`.

## Known issues and backlog
Roughly in priority order until the user's pad-only playthrough reorders it.
- Containers and looting, stores and bartering: untested; generic panel navigation may already cover them.
- Spellbook and spell memorization, level-up: untested.
- Spells and abilities aimed at a spot on the ground: only through pointer mode (R3) for now.
- Setting up the quickbar (dragging spells and items onto it): untested.
- Lists: the d-pad leaves a long list at its end instead of scrolling it (DESIGN §8.1).
- New game: the module browser is NUI, which panel navigation can't see; character creation after it is untested.
- Text entry (names, chat): needs a keyboard; an on-screen keyboard is DESIGN §9.
- Multiplayer: untested; the server-side features can't work there.
- Map, rest, options screens: untested.
- Releases: no CI or release zip yet (MAINTAINING.md §10); code signing via SignPath later.
- Supporting more than one game build would need the offsets in a per-build table (MAINTAINING.md §9).
- Decided 2026-10-30: the diagnostics stay (snapshots, traces, inject); they only act when triggered or switched on.

## User preferences
- They test in game and report briefly ("Nope", "Perfect"). Ask them specific questions, and read the log before theorising.
- They want direct, analog control that feels like a console action RPG, not pointer emulation.
- They prefer to make design choices themselves when trade-offs exist. Offer two to four concrete options.
