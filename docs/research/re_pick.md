# NWN:EE nwmain.exe (build 26c6e573) - world-object targeting RE

Image base 0x140000000. All VAs below are absolute. Everything is x64 MSVC (`this` in RCX, args RDX/R8/R9), not exported, so you must hook or call by address (pattern-scan or rebase; don't hardcode if the build changes).
Evidence files: `...\targeting_research\out\m2.txt .. m9.txt` (Ghidra decompiles); scripts in `...\targeting_research\scripts`. Ghidra project copy: `proj_pick` (headless needs the junction `C:\nwn_pick_link`, since Ghidra rejects ".claude" in a path).

## 0. Roots and naming

- `app` = `*(CClientExoApp*)(*g_pAppManager) + 8` = CClientExoAppInternal. All `clientinput.cpp` functions below are methods on this `app`. Confirmed: `FUN_1406d8300(app)` returns `app+0x30` = CNWCModule; `FUN_1406d8ad0(app)` = player creature (via `app+0x48` OID); `app+0x208` = CNWCMessage (sender); `module+0xe8` camera, `module+0xf8` current CNWCArea, `area+0x248` = `Scene*`.
- Input fields on `app`: `+0x120/+0x124` = mouse X/Y (copied from `CGuiMan+0xd4/+0xd8` via `CGuiMan::GetMousePos`), `+0x138` = hit-mask u64, `+0x140` byte mode (1 = special click mode), `+0x154` int (walk-held/blocked flag), `+0x424` = OID of the object a radial menu was opened on.
- Object from OID: `FUN_1406abb60(CClientExoApp*, oid)` -> `CGameObject*` or 0 (`FUN_1406abc80` is another getter used for creatures; `FUN_1406abb70(CClientExoApp*)` returns the CGameObjectArray (`internalApp+0x28`) for `CGameObjectArray::GetGameObject`). Invalid OID = `0x7f000000`.
- CNWCObject vtables: CNWCObject 0x140e3bf58, Creature 0x140e474e8, Item 0x140e4ceb0, Door 0x140e52f68, Placeable 0x140e53950. Type byte `obj+0xc`: 5 creature, 6 item, 9 placeable, 10 door. Casts (from usage; likely): vtbl+0x58 AsCreature, +0x68 AsItem, +0xa0 AsPlaceable, +0x28 AsDoor, +0x78 AsTrigger.
- Threading: everything runs on the main thread inside the `"Input"` profiler scope of `FUN_1406f14e0` (per-frame, called from `FUN_1406db800`). No locks. Call/hook only from the main thread.

## 1. World pick under the mouse and where the result lives

Per-frame hover update: **`FUN_1406f2830(app, bool guiBlocked)`** @0x1406f2830 (its log string is `UpdateMouseCursorAppearance`, clientinput.cpp). Called once per frame at the end of `FUN_1406f14e0` (call site 0x1406f1af2), right after `FUN_1406f1e40` (movement).

Pick routine: **`int FUN_1406eae20(app, CGameObject** outObj, Vector* outPos, int* outKind, int flags)`** @0x1406eae20. Variant **`FUN_1406ebcf0`** (same signature) is used instead when `FUN_14083bd80()` is non-null and its `+0x138`==0, i.e. when the GUI capture object (`g_pGuiMan+0x58`) has class id 0x41635467 (very likely the spell/item "choose target" cursor mode, cf. `guicreatureactiontarget.cpp`; likely). Callers: `FUN_1406f2830`, `FUN_1406f02c0` (LMB down), `FUN_1406f10e0` (RMB down). `flags`: hover/LMB pass 0, RMB passes 1; 0 makes it exclude the player's own object from the hit list (likely).

How it picks (confirmed): reads `app+0x120/+0x124`, builds a camera ray (camera vtbl+0x58 screen->ray, +0x40 camera pos), then `Scene vtbl+0x60(x,y,CHitInfo*)` = `Scene::HitCheck`; loops up to 50 hits (gob vtbl+0xf0 = user-data CNWC object, vtbl+0x100 = type 5/6/9/10), filling nine candidate slots. Return: 0 = nothing, 1 = ground or trigger/transition (outObj may be a trigger), 2 = game object (outObj = CGameObject*, outKind = hit info). Excerpt:
```
iVar8 = FUN_1406eae20(param_1,&local_1b8,&local_1a8,local_188);   // inside FUN_1406f2830
... if (iVar8 != 2) {ground/trigger} else
iVar8 = FUN_1406ed200(param_1,local_1b8,local_188,&local_168);    // interactable? -> cursor id + hilite pos
```
Where the result is stored:
- **Global `DAT_141296f48` (u32 OID) = current mouse-over object**; default 0x7f000000. Only `FUN_1406f2830` reads/writes it (xrefs 1406f2b1a/b92/f03/f24/f94/34a9). Confirmed.
- `player_creature + 0x428` (u32) = last hovered OID (set when hovered != player; reset to `DAT_140dc2a50` on un-hover). Likely used for hotbar/target actions.
- Pointer icon: `FUN_14083b3e0(*(app+200), cursorId, ...)`.

## 2. Highlight and hover name

Tail of `FUN_1406f2830` (confirmed):
```
if (newOid != DAT_141296f48) { old=GetObj(DAT_141296f48); old->vtbl[0x110](old); old->vtbl[0x160](old,1); player+0x428=DAT_140dc2a50; }
DAT_141296f48 = newOid;
if (obj && newOid != 0x7f000000) {
  obj->vtbl[0x108](obj,0);                                   // hover label
  if (!(plc && plc[+0x1d0]) && !(door && door[+0x1a0]))
      obj->vtbl[0x158](obj,&hitPos,1,0);                     // 3D outline highlight, mode 1
  if (AsCreature(obj) != player) player[+0x428] = newOid;
}
```
- Highlight on: `obj->vtbl[0x158](obj, Vector* pos, char mode=1, int a=0)`. Default (door/placeable/base) `FUN_140763520`: requires `*(u8*)(obj+0x120)&1`, else calls `vtbl[0x160](obj,1)` and returns 0. Creature `FUN_140801e00` also checks the appearance 2DA "Targetable" column (row = `*(u16*)(*(obj+0x300)+0xcc)`). Both end in `FUN_140965cf0(animBase, pos, mode, flag)` (nwcanimbase.cpp): sets `animBase+0x9c+mode*4` and runs `FUN_140966170` (`RefreshHilite`) which drives the `Scene`/Gob highlight virtuals (`Scene::AddHighlightGob`@0x1400c3350, `RemoveHighlightGob`@0x1400cc600 are virtual-only). Colours: config `graphics.hilite.color.{self,enemy,friend,neutral,item,door,placeable,trigger}`, `graphics.hilite.enabled/glow`, `ui.mouseover-feedback` (read in `FUN_1406ae3b0`/`FUN_140823e60`).
- Highlight off: `obj->vtbl[0x160](obj,1)` (default `FUN_140766e00` -> `FUN_140968590(animBase,mode)`, creature `FUN_14080f310`) plus `obj->vtbl[0x110](obj)` (creature `FUN_140804770` clears `obj+0x448`; placeable `FUN_1408718d0` clears `+0x1f8`).
- Hover name: `obj->vtbl[0x108](obj, mode)`: creature `FUN_1408047c0`, placeable `FUN_140871920`; if `(obj+0x120)&4` they call `vtbl[0x220]` = label builder (creature `FUN_14080b240`: sets `+0x448=1`, builds coloured name/description; placeable `FUN_140871b60`: sets `+0x1f8=1`, floaty text `FUN_140766450`). Item `FUN_14083f600` and base `FUN_140763b70` only hilite the inventory tree entry.

Set the highlighted object directly? You can call `vtbl[0x108]`/`[0x158]` on any object (main thread) and the game draws its own outline and name. But next frame `FUN_1406f2830` recomputes hover from the real pointer and un-highlights if it differs from `DAT_141296f48`; writing `DAT_141296f48` alone does not help. Confirmed. Fix = detour the pick (section 6).

## 3. Default left-click action

Chain (confirmed): SDL/CExoInput event -> `FUN_1406ed720` (action switch; LMB at 0x1406eda82) -> **`FUN_1406f02c0(app, int button)`** (log string `PerformLButtonDownAction`) -> `CGuiMan::HandleLMouseDown` (GUI first; returns if consumed) -> pick `FUN_1406eae20` -> if result 2: **`FUN_1406efec0(app, CGameObject* obj)`** -> per-type handler -> message. No hit: walk-to-point (`FUN_1406f3650`; `FUN_1407c0970`/`FUN_1407c0ab0`).

**`char FUN_1406efec0(app, CGameObject* obj)`** @0x1406efec0 returns 1 if it acted. Call this with an object pointer to do what a click does. Preconditions inside: `app+0x140 != 1`, `app+0x154 == 0`, usable flag `obj+0x11c` != 0. Dispatch on `obj+0xc`:
- 5 creature: `FUN_1406efa40(app, creature)` @0x1406efa40. Hostile (`creature+0x388 > 1`) -> `FUN_1407bf180(msg,oid)` = **attack** (input msg major 6 / minor 2; server `CNWSCreature::AddAttackActions`). Friendly/conversable -> `FUN_1407bf900(msg,oid)` (minor 8; server action 0x18 = **talk**). Some flagged creatures (`+0x37c==0 && +0x22c==1`) -> `FUN_1407c07c0(msg,oid,0,0)` (use, minor 0xb). Has DM/GM branches.
- 6 item: ground pickup `FUN_1408053b0(player, item+0x50)` -> `FUN_1407c0f60`; if the item resolves to a creature/corpse (`FUN_14083d7e0`) it re-dispatches to `FUN_1406efa40`. (`FUN_1406efdc0(app, item)` = same logic.)
- 9 placeable: `FUN_1408720a0(plc)` = `FUN_1407c07c0(msg, oid, 0, 1)` -> **UseObject** (minor 0xb; server `AddUseObjectAction`). Needs `plc+0x11c`.
- 10 door: by `door+0x5c` anim state `FUN_14086e810(door)` -> `FUN_1407bf860(msg,oid,0x15 open / 0x16 close)` (minor 3); locked case -> `FUN_1407c0970`.
Messages: `msg = FUN_1406abf90(CClientExoApp)` (= `app+0x208`), sent with `FUN_1407c25c0(msg, major=6, minor, buf, len)`. Server handler: `CNWSMessage::HandlePlayerToServerInputMessage` @0x1404531b0 (minors 2 attack, 3 door, 5 examine, 8 talk, 0xb use).

Risk: takes a `CGameObject*`, not an OID. The decompiler dropped the 2nd arg at the call in `FUN_1406f02c0`; the callee body clearly reads it (confirmed).

## 4. Radial (context) menu for an object

Per-object virtual **`obj->vtbl[0x148](obj)`** opens that object's radial menu (confirmed in RMB handler `FUN_1406f10e0` @0x1406f10e0: `*(app+0x424)=obj oid; obj->vtbl[0x148]()`):
- Creature `FUN_14098f450` (allocs 0x840-byte menu, `FUN_14098e140`, `FUN_14098e550(menu,0)`; refuses if one for the OID already open via `FUN_1406f7870(guiPanelMgr,0x12,oid)`), Door `FUN_14086d9e0` (-> `FUN_1409b00e0`), Placeable `FUN_1408703d0` (-> `FUN_1409b19d0`), Item `FUN_14085e810` (-> `FUN_1409ab4d0`/`FUN_1409a59d0`), base = empty stub.
- Ground: `FUN_1407db200(area,&pos)` then `FUN_14084def0(x,&pos)`.
All end in `CRadialMenu::Activate` @0x1401f7e80, which does `CGuiMan::GetMousePos(g_pGuiMan,&this+0x7dc,&this+0x7e0)` then `ActivateInternal` @0x1401f7f00 (centres ring on that point, `CaptureMouse`, `SetModal`, `SetMousePosition`, `static_nMenuDepth++`). Globals (int): `g_rmUseScreenCenter` @0x14140cd5c (nonzero = ring centred on screen centre, confirmed in `ActivateInternal`), `g_rmCenterCancelsMenu` 0x14128f4a0, `g_rmLeftMouseMirrorsRight` 0x14128f4a4, `g_rmSubmenuPreviewMode012` 0x14128f4a8, `g_rmTextPreviewMode012` 0x14128f4ac, `CRadialMenu::static_nMenuDepth` @0x14140cd58.

Pointer-free selection: `CRadialMenu::HandleKeyEvent(this, keyId, isDown)` @0x1401f90e0 natively handles keys: ids 0x36,0x37,0x34,0x31,0x30,0x2f,0x32,0x35 = octants 0..7 (sets `this+0x7c8=2`, `this+0x7cc=octant`, calls `vtbl+0x98` -> `HandleMenuAction` @0x1401f9350 which runs the entry, submenu-recurses or releases modal), 0x33 = cancel (state 1). Otherwise selection is by pointer angle in `HandleMouseMoveCaptured` @0x1401f9640. Get the menu `this` by hooking `ActivateInternal`. Confidence: opening confirmed; key ids confirmed from code, mapping to physical keys unknown (likely numpad).

## 5. Selectable / clickable test (bonus)

1. Geometry (in pick): only gobs matching the CHitInfo mask `app+0x138` with user type 5/6/9/10 (or trigger) are candidates (confirmed).
2. Interactability: **`FUN_1406ed200(app, obj, int* cursorId, Vector* hilitePos)`** @0x1406ed200, returns 0 = not interactable (confirmed). Creature (`FUN_1406ecba0`) needs `creature+0x11c != 0`; item needs `item+0x164 != 0`; placeable needs `plc+0x11c != 0` (almost certainly the server-pushed Useable flag, message `HandleServerToPlayerObjectUpdate_Useable`; cursor from `plc+0x118`); door `FUN_1406ece80`. Then highlight needs `(obj+0x120)&1`, `plc+0x1d0 == 0`, `door+0x1a0 == 0` (meaning unidentified), plus creature "Targetable" 2DA column.
Use `FUN_1406ed200` as the mod's "valid target" predicate (also gives the highlight anchor position).

## 6. Recommended integration

Cleanest: **detour `FUN_1406eae20` and `FUN_1406ebcf0`** (same signature). When the controller has target T (`CGameObject*` from `FUN_1406abb60`) and the call is for the world pick, return 2 with `*outObj=T`, `*outPos=*(Vector*)(T+0x38)`, `*outKind` from `FUN_1406ed200(app,T,&kind,&pos)` (return 0 if it returns 0). Otherwise call the original. Native effects: highlight, hover name, cursor, `DAT_141296f48`, `player+0x428`; mouse handlers also see T.
(a) Highlight X: make X the override target. Without the detour (flickers off next frame): `X->vtbl[0x108](X,0); X->vtbl[0x158](X,&X_pos,1,0)`; remove with `X->vtbl[0x110](X); X->vtbl[0x160](X,1)`.
(b) Default action on X: `FUN_1406efec0(app, X)`, main thread, check return 1; optionally pre-check `FUN_1406ed200`. Do not call `FUN_1406f02c0` (it first runs `CGuiMan::HandleLMouseDown` against the real pointer).
(c) Radial menu on X: set `g_rmUseScreenCenter=1` (restore after), `*(u32*)(app+0x424)=X->oid; X->vtbl[0x148](X)`, guard `static_nMenuDepth==0`; drive with `HandleKeyEvent` octant ids.

Risks: addresses are build-specific (pattern-scan). Main thread only, between frames, with module/area loaded (`app+0x30`, `module+0xf8`, `area+0x248` non-null, else `FUN_1406f2830` returns early). `FUN_1406ebcf0` (target-selection mode) must also be handled or spell/item targeting still uses the pointer. Menu objects are game-allocated (0x800-0x840 bytes); do not free. Unverified: meaning of `plc+0x1d0`/`door+0x1a0`, exact `flags` arg semantics, vtable cast slot names (+0x28/+0x58/+0x68/+0x78/+0xa0 inferred from usage).
