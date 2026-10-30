# Controller targeting research (NWN:EE mod)

Caveat: web search surfaced little hard detail. The Beamdog button maps are images (not fetchable), and no review documents NWN:EE console target selection in detail. Tags: [CONFIRMED] a fetched source says it; [REPORTED] single user/preview; [INFERRED] my extrapolation.

## Per game

### NWN:EE console (Switch/PS4/Xbox One, Beamdog 2019)
- Beamdog FAQ: "controls optimized for console gameplay"; controller only; no PC controller support planned. Button maps exist only as images. [CONFIRMED] https://www.beamdog.com/news/faq-neverwinter-nights-enhanced-edition-switch-xbox-ps4/
- Direct analog movement. Quick items go on a radial menu, assigned from the inventory. Touchpad (PS4) pauses and queues actions. [CONFIRMED, one review] https://www.playstationcountry.com/neverwinter-nights-enhanced-edition-ps4-review/
- The combat radial menu applies to "whatever the camera and not the character is looking at". Strongest datapoint: Beamdog keyed targeting to camera direction, not character facing. The isometric tactical view was removed. [CONFIRMED, one review] https://waytoomany.games/2019/12/07/review-neverwinter-nights-enhanced-edition/
- The radial hotbar replaced the PC hotbar. One reviewer calls it over-sensitive (a slight stick tilt picks the wrong slice) and badly labeled, blaming old mouse-based radial code. Another says radials "work pretty well" but are awkward to track. Same URL, plus https://www.thesixthaxis.com/2020/03/12/neverwinter-nights-enhanced-edition-review/
- Menus use a stick-as-mouse cursor (a Switch GameFAQs post says pause, then the left stick acts like a mouse). Shops and inventory are fiddly. [REPORTED]
- Reviewers: "targeting enemies could feel like a chore", controls "never felt quite right", "needs a mouse and keyboard". https://useapotion.com/2019/12/neverwinter-nights-enhanced-edition-review
- Not found: the cycling button, party-control scheme, how doors/containers are targeted. Reviews suggest Beamdog did not solve this well, so it is not a model to copy.

### Baldur's Gate 3
- Direct third-person movement on the stick. A separate "Cursor Mode" gives a point-and-click cursor. Triggers open radial menus (light pull peeks, hard press locks open). D-pad has Jump and Examine. A/Cross is interact, hold for "active search"; Square/X is the context menu. Zooming fully out opens Tactical View. [CONFIRMED / controls table] https://blog.playstation.com/2023/09/05/how-baldurs-gate-3-adapts-its-expansive-rpg-gameplay-for-your-dualsense-controller/
- A Steam Deck user reports d-pad cycling of targets. [REPORTED]

### Divinity: Original Sin 1 EE / 2 DE
- Hold the confirm button to show a "search radius" that labels every interactable nearby (equivalent of holding Alt on PC). [REPORTED] https://www.gamerevolution.com/preview/68250-divinity-original-sin-enhanced-edition-preview
- A PS4 reviewer notes picking one specific object is hard. DE patch notes cite "improved selecting doors when using a controller". The dense-cluster problem persisted in shipped games.

### Diablo III console
- Blizzard: "movement feels better on the controller... targeting is the issue". They avoided a reticle: auto-aim locks onto an enemy, skills land at the locked target; a red outline marks it. [CONFIRMED via previews] https://gamebanshee.com/ku69d and https://di.diablowiki.net/Red_target_outline

### Pathfinder (Kingmaker/WotR), Dragon Age Origins, Pillars, Tyranny, Wasteland 3
- No usable targeting detail found. DAO console used a Mass Effect-style radial and a closer camera. Console CRPG reviews are generally negative about UI. No numbers found.

## Synthesis: credible schemes

Shared rules for all:
- Interactable is a hard whitelist: doors, containers, levers, NPCs, hostile creatures, ground items. Exclude party members and the player, and anything dead or non-usable.
- No OS pointer warping. Draw your own marker and issue the action through game API (or a synthetic click at the projected position only at activation).

### A. Camera/stick-aimed soft target plus hold-to-scan (recommended)
- Eligible: whitelist, on screen, line of sight, within ~3.5 m for use-type objects (doors, chests, levers, items, NPCs) and within ~25 m for hostiles.
- Score = angle between aim direction and the direction to the object (ground plane), in degrees, plus ~4 degrees per metre. Aim direction = stick direction while the stick is held, else camera forward (not character facing). Reject beyond ~60 degrees. Lower is better. Hostiles in combat get -10.
- Hysteresis: switch only if the challenger beats the current by 20% (or 10 points) and persists 0.3 s. Drop only after ineligible for 0.25 s. Evaluate at ~10 Hz.
- Display: own outline or ring, name plate, and a button prompt ("A: Open").
- Buttons: A = default action (left-click). Y/X = radial/context menu for the targeted object (right-click). LB/RB = cycle next/previous in score order, from the same filtered list. B = clear. Hold A ~0.3 s = scan mode: label all interactables within ~6 m, pick by stick direction. D-pad stays for quick slots.

### B. Interact radius plus hostile lock
- Interact mode (default): only objects within ~2.5-3 m, scored by distance and facing; a short, stable list. A uses the nearest; LB/RB cycles.
- Combat mode (hostile within ~20 m, or LT toggles): hard-lock a hostile. RB/LB cycles hostiles by angle from camera forward. Lock persists until death, out of range, or release. Red outline on the lock, white on the interact target.

### C. Free cursor (BG3 Cursor Mode / NWN console menus)
- Hold LT or R3 and the right stick moves a ground cursor that magnetically snaps to objects within ~60 px. Exact for dense clusters but slow, and reviewers disliked it in NWN console. Use as a fallback only.

## Recommendation
Scheme A, adding B's hostile lock later. Against the observed failures:
- Companion flicker: whitelist excludes party; hysteresis plus minimum hold.
- Far/behind cycling: cycling uses only the filtered eligible list (range per type, on screen, LOS, within the cone), never "all on-screen objects sorted by x".
- Pointer jumping: own marker plus API activation.
- "D-pad didn't target": move cycling to bumpers and show a clear prompt so the user sees what A will do.
- Facing-based picks are unstable while running; camera forward or stick direction matches what the player sees (and matches Beamdog's camera-based choice).

## Tuning numbers
No numeric values were found in sources; these are [INFERRED] starting points:
- Use range 2.5-3.5 m; scan radius ~6 m; hostile range 20-25 m.
- Cone 45-60 degrees off camera forward or stick direction.
- Distance weight ~4 deg/m.
- Switch margin 20% for 0.3 s; drop grace 0.25 s; hold-to-scan 0.3 s.
- Any radial: stick threshold >= 0.5 deflection and ~15 degree dead band between slices (NWN console's radial was criticised as over-sensitive).
