# Changelog

User-visible changes, newest first. The working log with the reasoning behind each
step is `docs/STATUS.md`.

## Unreleased

Supports NWN:EE build `26c6e573` (64-bit Windows).

### Added
- Controller mode, now the default once a game is loaded. Hold L3+R3 (or press
  Ctrl+Shift+F11) to switch to classic mode and back.
  - Direct analog movement relative to the camera: a half push walks, and you stop
    dead on release (single player).
  - Analog camera on the right stick; R3 switches the stick to the mouse pointer.
  - Automatic soft targeting of what you aim at, hostiles first. A acts on the
    target, X opens its radial menu, d-pad left/right steps between targets.
  - Radial menus open mid-screen and the right stick snaps to their options. B goes
    back one level.
  - The mouse pointer is hidden while nothing is targeted.
- On-screen prompt bar showing what each button does, and quickbar names while LB /
  RB are held.
- Conversation picker: the speaker's portrait, name and line in large text, the
  replies as a list. D-pad or left stick chooses, A says it, B ends. The game's
  own conversation window is hidden while it shows.
- Menu navigation with the d-pad in the game's menus (inventory, character sheet,
  journal, game menu, main menu, Load screen): a focus ring, A clicks, holding A
  drags, X right-clicks, B closes, LB/RB tapped switch tabs, the right stick
  scrolls. Works out of a game too.
- `[play]` ini section for controller-mode settings (`mode`, movement, targeting,
  pointer hiding). Settings left in `[diag]` by older inis are still read.
- Classic mode: every pad input bound to keys or mouse buttons through
  `nwn_controller.ini`, with LB, RB and LB+RB layers for the quickbar and panels.
- Diagnostics: snapshots (tap L3+R3), movement traces, and a scripted virtual pad
  for unattended testing.

### Known limitations
- New game / character creation: the module browser uses the game's newer NUI
  windows, which can't be navigated yet.
- Typing (names, chat) needs a keyboard.
- Containers, stores, spellbook, level-up and multiplayer are untested.
- Features that depend on the game's internals switch off on other game builds
  (classic mode keeps working).
