# Handover: next UI steps (written 2026-10-29)

This is for the next session picking up the controller mod. It says where things
stand, what to build next and how. The durable records are still `docs/STATUS.md`
(log and decisions) and `docs/DESIGN.md` (§12a holds every verified offset). Keep
both updated as you go; this file is only the starting brief.

## Read first
0. `docs/MAINTAINING.md`: the maintainer's handbook (supersedes the practical notes
   below where they differ).
1. `CLAUDE.md` in the game root: build, test, install and the working rules.
2. `docs/STATUS.md`, especially "UI: hybrid custom overlay" and "User preferences".
3. `docs/DESIGN.md` §6 (quickbar), §7 (radial, with the as-built notes), §8 (panel
   mode), §12a (offsets).
4. `re/README.md` for the Ghidra and Python tooling.

## Where things stand
Confirmed in game by the user:
- Direct analog movement, stopping dead on release, walking on a half push.
- Right-stick camera, R3 toggles camera/pointer.
- Targeting v2: soft target through the patched world pick, A acts, X opens the radial.
- Radials open mid-screen; B goes back one level; only the right stick steers.

Built and installed, the user said "Okay great" but hasn't reported a test:
- Radial snapping: the right stick snaps to the nearest shown, enabled slot. Check
  `bin\win32\nwn_controller.log` for `radial: slot order as expected` (or `flipped`).
  If it says flipped, the self-correction ran; note it in DESIGN §7.

Built, not yet looked at in game:
- The ImGui overlay (`src/overlay.cpp`, `SDL_GL_SwapWindow` hook) with the prompt bar
  (A verb for the target, X Options, Y Self, R3 mode, a badge) and layer prompts while
  LB/RB are held. Previews are in `docs/images/`.

Ask the user about the overlay's look before stacking more on it.

## The plan (the user chose "Hybrid" and "keep going"; order is ours)
Custom controller-first screens where the stock UI is worst for a pad, focus
navigation over the remaining stock panels. Do one step at a time and get the user's
in-game verdict before the next.

### Step 4: quickbar HUD (done 2026-10-29)
Names are shown in the layer prompts. The user chose no icons, no always-on strip and
no binding change (see STATUS.md). The RE notes are in DESIGN §12a (quickbar); the
route below is kept for reference.

Goal: the overlay shows what is actually in the slots the pad fires, by name first,
icons second, so the player isn't firing "Slot 5" blind.

What exists:
- The ini's layer bindings already fire quickbar slots through the native keys
  (F1–F12, Shift/Ctrl for the other bars), and the layer prompts label them from the
  ini ("Slot n"). DESIGN §6 sketches a fuller LT/RT scheme; the user hasn't chosen it,
  so ask before changing the bindings.
- Client classes (vtables, from `re/decompiles/ui/syms.txt`):
  `CPanelQuickBar` 0x140e52900, `CGuiQuickButton` 0x140e527d8 and 0x140e528e8,
  `CGuiQuickbarMenu` 0x140e77340. `game::ForEachPanel` + `ClassForVtable` can find
  the quickbar panel at runtime.
- Server side, exported (single player / local host only):
  `CNWSCreature::GetQuickbarButton(uchar slot)` 0x140392f60 returns a
  `CNWSQuickbarButton*`; the `SetQuickbarButton_*` exports (0x1403a3a50…0x1403a4090)
  show its field layout (type, item oid, spell id, class, metamagic, etc.).

Suggested route:
1. Quick win, single player: call `GetQuickbarButton` on the server creature (the
   mod already reaches it for `StopServerDrive`), decompile the `SetQuickbarButton_*`
   setters to learn the fields, and turn each slot into a label: item name from the
   item object, spell or feat name via the game's 2DA/TLK lookups (find the
   exported `C2DA` / `CTlkTable::FetchInternal` calls; `SetEntryx` in
   `re/decompiles/radial/radial.c` shows `FetchInternal` in use).
2. Then the client route, which also works in multiplayer: decompile the
   `CPanelQuickBar` vtable methods and `CGuiQuickButton` to find each button's
   contents and icon resref. Prefer it if it's not much harder.
3. Icons: the overlay is ImGui on OpenGL, so it needs GL textures. Find how
   `CGuiQuickButton` gets its icon (likely a resref loaded through the resource
   manager), then either borrow the game's GL texture id or load the TGA/DDS bytes
   ourselves. Names alone are an acceptable first delivery.
4. Show it: when a layer that holds quickbar slots is held, the layer prompts show
   the slot's real name (and icon) instead of "Slot n". Optionally a small always-on
   strip; ask the user.
5. Harness: add an offline check for any new pure logic (label formatting,
   slot → key mapping). Live game reads are gated on `knownBuild` and SEH-guarded
   like everything else.

### Step 5: dialogue picker (done 2026-10-29)
Built as an overlay conversation screen: speaker, portrait, NPC text and the replies,
with the game's window moved away (see STATUS.md). The user liked it; Claude
self-tested the last additions. RE notes: DESIGN §12a (conversation window).
Original brief:
- The stock conversation window is `CGuiInGameChatDialog` (with
  `CGuiInGameChatDialog_CustomPicklist`), replies are `CReplyButton`. Find these
  through `ForEachPanel` / the GUI scene, read the NPC line and the reply texts.
- Build an overlay list: d-pad or left stick moves, A picks, B ends or skips (match
  what Escape does). Pick a reply by driving the stock control (the reply's click
  handler or its number key), not by re-implementing the conversation.
- Keep movement and targeting paused while it's open, like radial mode.

### Step 6: focus navigation for stock panels (DESIGN §8) - generic part built
Built and self-tested 2026-10-29 (STATUS.md, DESIGN §8.1 as built). Next: the user's
verdict, then LB/RB tabs and per-panel refinements (§8.2), asking which screens hurt
most. Original brief:
- Generic: enumerate the top panel's controls (tag tables or a hit-test grid via
  `GuiHitTest`), move a focus ring with the d-pad, A = click at the control centre.
  The M0 snapshot (L3+R3 tap or Ctrl+Shift+F10) already dumps panels, tag tables and
  a hit-test grid, so start by reading those for inventory and the character sheet.
- Then replace the worst screens one at a time, asking the user which hurt most.
  Candidate panels: `CPanelInventory`, `CPanelInvEquip`, `CPanelCharSheet`,
  `CPanelSpellbook`, `CPanelSpellMemorization`, `CPanelStore`, `CPanelLoadGame`.

## Things that bit this session
- `analyzeHeadless.bat` breaks on the install path's spaces. Run Ghidra through a
  junction (`New-Item -ItemType Junction C:\nwn_re -Target <...>\re`) and remove only
  the link afterwards with `(Get-Item C:\nwn_re).Delete()`. `rmdir` on it is blocked.
- `re\tools\*.py` need `pefile` and `capstone`, and no venv is kept. Make one with
  `python -m venv re\venv; re\venv\Scripts\pip install pefile capstone` and run the
  scripts with `-I`. `fdis.py` takes hex addresses without `0x` and a decimal length.
- The keyboard radial actions (RadialN…NW) activate a slot outright; the highlight
  can only be steered with the pointer. Similar "the key does more than you think"
  traps are likely in dialogue; check the decompile before relying on a key.
- Never install while `nwmain.exe` runs; edit the user's ini in place and keep it in
  step with `data\nwn_controller.ini`.

## Testing in game without the user
- Set `inject = 1` in `[diag]` of `bin\win32\nwn_controller.ini` first (it's 0 in the
  user's copy between sessions).
- `[diag] inject = 1` plus `nwn_controller_inject.txt` is a scripted virtual pad
  (`src/inject.h`). `tools/launch_load.ps1` starts the game and loads the newest save;
  `tools/gamectl.ps1` takes screenshots (`shot <png> [scale]`), focuses the window,
  clicks, presses keys and reports whether the cursor is shown. Screenshots don't
  include the cursor. The game must be focused for the mod to act (`gamectl focus`).
- Close the game (`Stop-Process -Name nwmain`) before installing a new build. It
  doesn't save, so the user's saves are untouched.

## How to work with the user
- They test in game and answer in a few words. Give them a short, concrete test list
  with each build, and read the log before theorising.
- Offer two to four concrete options when there's a real design choice; otherwise
  pick and say so.
- They want a console action-RPG feel, not pointer emulation.
