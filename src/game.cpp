#include "game.h"

#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "log.h"

// Facts below come from decompiling nwmain.exe build 26c6e573 (Oct 2025); the
// addresses in comments are that build's VAs. Offsets marked "known build"
// are only used when the exe's PE timestamp and image size match that build.

namespace game {
namespace {

constexpr DWORD kKnownTimestamp = 0x68e408bc;
constexpr DWORD kKnownImageSize = 0x1716000;

// CClientExoAppInternal (the object at *(*g_pAppManager)+8)
constexpr ptrdiff_t kAppObjectArray = 0x28; // CGameObjectArray*
constexpr ptrdiff_t kAppModule = 0x30;      // CNWCModule*
constexpr ptrdiff_t kAppPlayerOid = 0x48;   // controlled creature
constexpr ptrdiff_t kAppDriveState = 0x1b0; // int force-send flag, then 7 held-direction flags
// CGameObjectArray: bucket array at +0, buckets per half at +0x1c, total buckets at +0x24;
// node {u32 id; obj @+8; next @+0x10}. IDs with bit 31 set (every server object: see
// CNWSMessage::WriteOBJECTIDServer) hash into the upper half (GetGameObject, 0x140296470).
constexpr ptrdiff_t kArrayBucketCount = 0x1c;
constexpr ptrdiff_t kArrayBucketTotal = 0x24;
// CNWCModule
constexpr ptrdiff_t kModuleCamera = 0xe8; // CAurCamera* "camera" (FUN_1407cd2c0)
// CNWCObject (AsNWCObject and AsNWCCreature both return `this`)
constexpr ptrdiff_t kObjOid = 0x08;
constexpr ptrdiff_t kObjType = 0x0c; // byte; 5 = creature (FUN_1406d8100)
constexpr ptrdiff_t kObjArea = 0x30;
constexpr ptrdiff_t kObjPosition = 0x38;
constexpr ptrdiff_t kObjOrientation = 0x44;
constexpr ptrdiff_t kObjAnimation = 0x5c;     // u16 (getter @0x1407630d0)
constexpr ptrdiff_t kObjPredictBase = 0x2c0;  // drive prediction starts from here (0x1406ea9e0)
constexpr int kObjSlotAsCreature = 11;     // CGameObject::vftable +0x58
constexpr int kObjSlotSetOrientation = 36; // +0x120, FUN_140766220: writes +0x44, updates the model
constexpr uint32_t kSetOrientationRva = 0x766220;
// The module camera is a Camera (NewCAurCamera installs vtable 0x140d82e08, RTTI
// "Camera", over the CAurCamera base's 0x140d82d00). Slots in that vtable, checked
// against the exported Camera:: methods.
constexpr int kCamSlotGetPosition = 8, kCamSlotMapToScreen = 10, kCamSlotGetViewAngle = 18;
constexpr int kCamSlotGetYaw = 22, kCamSlotGetPitch = 23, kCamSlotGetDist = 24;
// Camera::+0x458 is its CAurBehavior; GetYaw/GetPitch use it when vtbl[8]
// (AsBehaviorCameraFollow) is non-null. CAurBehaviorCameraFollow::Control adds the
// one-shot "nonFPSturn"/"nonFPStilt" deltas (degrees) to yaw/pitch, zeroes them, then
// applies the game's own clamps (Message @0x140056740, Control @0x140055100).
constexpr ptrdiff_t kCamBehavior = 0x458;
constexpr int kBehaviorSlotAsCameraFollow = 8;
constexpr ptrdiff_t kFollowYawDelta = 0x88, kFollowPitchDelta = 0x8c;
// In-process server (single player / local host). The client and server main loops
// run one after the other on the main thread (0x140044d80), the same thread as our
// SDL hook. CAppManager+8 is the CServerExoApp.
constexpr ptrdiff_t kAppManagerServer = 0x08;
// CNWSCreature: id +0x08, position +0xa4, action list +0x100 (list -> head node ->
// action @+0x10), pathfinding info +0x530 with the waypoint array at +0x80.
constexpr ptrdiff_t kSrvObjId = 0x08, kSrvPosition = 0xa4, kSrvActions = 0x100, kSrvPathfind = 0x530;
constexpr ptrdiff_t kPathWaypoints = 0x80;
// Targeting, from the client's own click handling (FUN_1406efec0 default action,
// FUN_1406ed200 "interactable?", FUN_1406efa40 creature click). The game casts with
// these vtable slots, so we call them too rather than assume `this`.
constexpr int kObjSlotAsDoor = 5, kObjSlotAsItem = 13, kObjSlotAsPlaceable = 20; // +0x28, +0x68, +0xa0
constexpr ptrdiff_t kUseable = 0x11c;      // int on creature / placeable / door: server-pushed useable flag
constexpr ptrdiff_t kItemPickable = 0x164; // int on a ground item
constexpr ptrdiff_t kCreatureMaster = 0x270;   // u32 OID of the creature's master (associates), else invalid
constexpr ptrdiff_t kCreatureReaction = 0x388; // byte; > 1 = hostile (attack instead of talk)
// World pick under the pointer, int(app, CGameObject** obj, Vector* pos, uint* kind, int flags):
// returns 0 nothing, 1 ground/trigger, 2 object. Hover (FUN_1406f2830), left click
// (FUN_1406f02c0 -> FUN_1406efec0) and right click / radial (FUN_1406f10e0) all go
// through it; FUN_1406ebcf0 is the same pick used while choosing a spell/item target.
constexpr uint32_t kPickRva[2] = {0x6eae20, 0x6ebcf0};
// Both start: mov rax,rsp; push rbp/rbx/rsi/rdi/r12/r13/r14/r15 (15 bytes, no rip-relative).
constexpr uint8_t kPickPrologue[15] = {0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41,
                                       0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
// The client's keymap-action handler, uint(app, action, down, passthrough, fromKeyboard).
// Actions are keymap.2da row numbers: 89 DriveModeForward (walk; no default key),
// 90 DriveModeForwardRun (run; the stock W). Its switch goes through the byte table at
// 0x1406ef758: 89 -> case 0x2a sets app+0x1b4, 90 -> case 0x2b sets app+0x1b8.
constexpr uint32_t kInputActionRva = 0x6ed720;
// int g_rmUseScreenCenter: nonzero makes CRadialMenu::ActivateInternal (0x1401f7f00)
// centre every radial ring, sub-radials included, on the screen instead of the pointer.
constexpr uint32_t kRadialCenteredRva = 0x140cd5c;
// int at 0x14128f4a0 (named g_rmCenterCancelsMenu in our notes): when nonzero, a
// cancelled sub-radial reopens its parent (CRadialMenu::HandleMenuAction, state < 2)
// instead of closing the whole stack.
constexpr uint32_t kRadialCancelToParentRva = 0x128f4a0;
// CRadialMenu::HandleKeyEvent ids are keymap actions: 51 = RadialCancel (state 1).
constexpr uint32_t kRadialCancelAction = 51;
// CRadialMenu slots (RM_NUM = 8): CGuiIcon* per slot at +0x6e0 (ActivateInternal builds
// an icon only where one is set), flags per slot at +0x1f8 (bit 0 = DisableEntry), and
// the slot the pointer is on (m_nSelectedOctant) at +0x7cc.
constexpr ptrdiff_t kRadialSlotIcons = 0x6e0;
constexpr ptrdiff_t kRadialSlotFlags = 0x1f8;
constexpr ptrdiff_t kRadialSelected = 0x7cc;
// Drive action (AIActionDrive @0x1403b8b60): type 0x33; +0x60 is the remaining chain
// count, which the abort message zeroes (0x140452ba0).
constexpr int kActionDrive = 0x33;
constexpr ptrdiff_t kActionDriveChain = 0x60;
// Quickbar. CClientExoAppInternal+0x90 is the in-game GUI and its +0x60 the
// CPanelQuickBar (vtable 0x140e52900): the keymap handler fires F1-F12 through it
// (FUN_1406ed720 case 5 -> FUN_14085c3b0). It holds all 36 CGuiQuickButtons inline from
// +0xf0, 0x2c8 bytes apart (its destructor 0x140858bf0); +0x6510 points at the first
// button of the page F1-F12 fire.
constexpr ptrdiff_t kAppInGameGui = 0x90, kInGameGuiQuickbar = 0x60;
constexpr uint32_t kQuickbarVtableRva = 0xe52900;
constexpr ptrdiff_t kQbButtons = 0xf0, kQbShownPage = 0x6510;
constexpr ptrdiff_t kQbButtonSize = 0x2c8;
constexpr int kQbSlots = 36;
// CGuiQuickButton, as its tooltip (0x14085e9b0) reads it: type byte +0x140 (the
// server's quickbar types); item OID +0xec; spell id in the low 16 bits of +0x110 (also
// for spell-like abilities, getter 0x14085c480); command label CExoString +0x168; for
// every other type a name CExoString +0x100 if set, else the strref at +0x70 (the feat
// and skill setters 0x140867070 / 0x140868910 put the name strref there).
constexpr ptrdiff_t kQbtnStrref = 0x70, kQbtnItem = 0xec, kQbtnName = 0x100, kQbtnSpell = 0x110;
constexpr ptrdiff_t kQbtnType = 0x140, kQbtnCommandLabel = 0x168;
// CNWCItem name (FUN_14083d8a0): the CExoString at +0x290 once identified (+0x1c0),
// else the base item's name (base item id +0x40; CNWBaseItem +0 is its name strref).
constexpr ptrdiff_t kItemBaseItem = 0x40, kItemIdentified = 0x1c0, kItemName = 0x290;
// CNWRules: base item array +0x20, spell array +0xd0. CNWSpell: name strref +0x10.
constexpr ptrdiff_t kRulesBaseItems = 0x20, kRulesSpells = 0xd0, kSpellNameStrref = 0x10;
// Conversation window: the in-game GUI's +0x70 is the CGuiInGameChatDialog (vtable
// 0x140e55680), made on the first conversation and kept (FUN_1406fb560); like other
// panels it is open while +0xa0 is set (FUN_1406fa7b0 tests the quickbar the same
// way). Its replies (FUN_14087e7e0): CExoString texts at +0x118, flags at +0x128 (bit 0:
// can't be chosen), count +0x120. FUN_14087e750(panel, i) chooses reply i: the reply
// list's click handler (0x14087e1f0) and the number keys (FUN_1406f81c0) both end there.
// It ignores the choice while +0xbbc is set, and turns it into "continue" (-2) when
// the game offered no replies of its own (+0x138 bit 0).
constexpr ptrdiff_t kInGameGuiDialog = 0x70;
constexpr uint32_t kDialogVtableRva = 0xe55680;
constexpr ptrdiff_t kPanelOpen = 0xa0;
constexpr ptrdiff_t kDialogReplyTexts = 0x118, kDialogReplyFlags = 0x128, kDialogReplyCount = 0x120;
// Its text objects: the speaker label's at +0x1d0 and the NPC line's at +0x268 (the
// entry CGuiEdit at +0x1d8 keeps it at its +0x90; FUN_14087e7a0 sets it). They're
// StringGobs: CGuiEdit::GetText (0x1401e93b0) walks one through vtable +0xe8 (line
// count) and +0xe0 (line i) and concatenates the lines with nothing between them.
constexpr ptrdiff_t kDialogSpeakerText = 0x1d0, kDialogEntryText = 0x268;
constexpr int kGobSlotGetTextLine = 0xe0 / 8, kGobSlotGetTextLineCount = 0xe8 / 8;
// The speaker. The dialog message handler (around 0x14079666e) stores the speaker's id
// on the player's client creature at +0xdc and passes it to FUN_1406fb560, which picks
// the portrait: through vtbl +0x18 (the object), +0x60 (non-null for creatures) and
// +0x58 (the creature); a creature's portrait base is its stats' (+0x3b0) CResRef at
// +0xb4, turned into a resref by FUN_14084c2f0(base, out, size), which tries `size` then
// the nearest size that exists (0-5 = "", T, S, M, L, H; the window asks for M). Other
// objects answer vtbl +0x1d0(out, size) themselves. With no portrait it uses
// "po_hu_m_99_".
constexpr ptrdiff_t kCreatureTalkingTo = 0xdc, kCreatureStats = 0x3b0, kStatsPortrait = 0xb4;
constexpr int kObjSlotAsObject = 0x18 / 8, kObjSlotIsCreature = 0x60 / 8, kObjSlotPortrait = 0x1d0 / 8;
constexpr uint32_t kPortraitResRefRva = 0x84c2f0;
// Textures: CAuroraTexture (0x30 bytes) holds its CResTGA at +8. CResTGA::
// OnResourceServiced (0x1401a4730) leaves the raw 18-byte TGA header at +0x88 and the
// pixels at +0x80, RLE expanded and swapped to RGB(A) for true colour, rows in file
// order (descriptor bit 5 set = top row first).
constexpr size_t kAuroraTextureSize = 0x30;
constexpr ptrdiff_t kTextureRes = 0x08, kTgaPixels = 0x80, kTgaHeader = 0x88;
// Moving a panel: CGuiPanel::SetPositionScreen(x, y) and SetPosition(Vector) (exported),
// over its model's position (model vtbl +0xc0 GetPosition).
constexpr int kOffscreen = -20000;
constexpr uint32_t kDialogChooseRva = 0x87e750;
// CGuiMan
constexpr ptrdiff_t kGuiKeyboardCapture = 0x50, kGuiMouseCapture = 0x58;
constexpr ptrdiff_t kGuiMouseX = 0xd4, kGuiMouseY = 0xd8;
// CGuiObject / CGuiPanel
constexpr ptrdiff_t kGuiObjModel = 0x10;
constexpr ptrdiff_t kGuiObjParent = 0x68;  // the object it's attached to (null at the top)
constexpr int kGuiSlotAsGuiPanel = 8;     // +0x40: CGuiPanel returns this, CGuiObject null
// Always-on HUD panels (vtables from the M0 snapshot, named by RTTI): chat prompt,
// quickbar, compass, party bar, player state bar, chat window, name bubbles, action
// queue, the conversation window (dialogue mode handles it), tooltips
// (CGuiTextBubble), the area loading screen, floating speech (CGuiInGameFloatyText)
// and screen fades (CPanelScreenFade, CPanelScreenFadeClickable).
constexpr uint32_t kHudPanelVtables[] = {0xe54ff0, 0xe52900, 0xe30750, 0xe4f828, 0xe59590, 0xe54d78, 0xe2f358,
                                         0xe54498, 0xe55680, 0xdb3c90, 0xe4c0a8, 0xe70d18, 0xdb5338, 0xe358d8};
constexpr uint32_t kGuiLabelVtable = 0xdb05e8;
constexpr uint32_t kGuiTabButtonVtable = 0xe32b10;
constexpr int kModelSlotGetName = 3;      // returns const char*
constexpr int kModelSlotGetPosition = 24; // +0xc0, returns Vector (CGuiPanel::SetPositionScreen)

// The game's CExoString.
struct ExoString {
    char* text;
    uint32_t length, capacity;
};

// The game's CResRef: 16 characters, not necessarily terminated, then a spare byte.
struct ResRef {
    char name[16];
    uint8_t spare;
};

struct Exports {
    void** appManager; // &g_pAppManager
    void** guiMan;     // &g_pGuiMan
    int* radialDepth;  // &CRadialMenu::static_nMenuDepth
    float (*getGuiScale)();
    void* (*guiHitCheck)(void* guiMan, int x, int y, int force, int flags);
    Vec3* (*mapGuiSceneToScreen)(void* guiMan, Vec3* ret, Vec3* in);
    void* camGetPosition;
    void* camMapToScreen;
    void* camGetViewAngle;
    void* camGetYaw;
    void* camGetPitch;
    void* camGetDist;
    void* followAsCameraFollow; // CAurBehaviorCameraFollow::AsBehaviorCameraFollow
    void* (*serverGetCreature)(void* serverApp, uint32_t oid); // optional
    void (*serverSetPosition)(void* object, Vec3* byValue, int);  // optional; 12-byte Vector goes by pointer
    int (*objGetDead)(void* object); // optional; CNWSObject::GetDead, for targeting
    int (*radialHandleKey)(void* radial, uint32_t key, uint32_t down); // optional; CRadialMenu::HandleKeyEvent
    // Optional, for quickbar names.
    void** tlkTable; // &g_pTlkTable
    void** rules;    // &g_pRules
    ExoString* (*tlkGetSimpleString)(void* tlk, ExoString* ret, uint32_t strref);
    void (*exoStringDtor)(ExoString* s);
    void* (*getSpell)(void* spellArray, int id);
    void* (*getBaseItem)(void* baseItemArray, int id);
    // Optional, for conversation text: checked against the text objects' vtables.
    void* gobGetTextLine;      // StringGob::GetTextLine(int) -> const char*
    void* gobGetTextLineCount; // StringGob::GetTextLineCount() -> int
    // Optional, for portraits and hiding the conversation window.
    void* (*textureCtor)(void* tex, const ResRef* byValue, int);
    int (*textureLoad)(void* tex);
    void (*textureDtor)(void* tex);
    void (*panelSetPosition)(void* panel, const Vec3* byValue);
    void (*panelSetPositionScreen)(void* panel, int x, int y);
};


HMODULE g_exe;
uintptr_t g_base, g_size;
bool g_resolved, g_resolveFailed;
Exports g_x;
Caps g_caps;
bool g_capsLogged;

bool Plausible(const void* p)
{
    auto v = reinterpret_cast<uintptr_t>(p);
    return v > 0x10000 && v < 0x00007fffffffffffull && (v & 7) == 0;
}

template <class T>
T Slot(void* obj, int slot)
{
    void** vt = nullptr;
    void* fn = nullptr;
    if (!Read(obj, 0, &vt) || !Plausible(vt) || !SafeRead(vt + slot, &fn, sizeof(fn)))
        return nullptr;
    return reinterpret_cast<T>(fn);
}

// Fault-guarded calls. Each lives in its own function so __try has no C++
// objects to unwind.
bool CallVec3Getter(void* fn, void* self, Vec3* out)
{
    __try {
        reinterpret_cast<Vec3* (*)(void*, Vec3*)>(fn)(self, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallFloatGetter(void* fn, void* self, float* out)
{
    __try {
        *out = reinterpret_cast<float (*)(void*)>(fn)(self);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallMapToScreen(void* fn, void* cam, Vec3 world, Vec3* out)
{
    __try {
        reinterpret_cast<Vec3* (*)(void*, Vec3*, Vec3*, bool)>(fn)(cam, out, &world, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSetOrientation(void* fn, void* obj, const Vec3* dir)
{
    __try {
        reinterpret_cast<void (*)(void*, const Vec3*)>(fn)(obj, dir);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallPtrGetter(void* fn, void* self, void** out)
{
    __try {
        *out = reinterpret_cast<void* (*)(void*)>(fn)(self);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallServerSetPosition(void* object, Vec3 pos)
{
    __try {
        g_x.serverSetPosition(object, &pos, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallServerLookup(void* (*fn)(void*, uint32_t), void* serverApp, uint32_t oid, void** out)
{
    __try {
        *out = fn(serverApp, oid);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallIntGetter(int (*fn)(void*), void* object, int* out)
{
    __try {
        *out = fn(object);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallHitCheck(void* guiMan, int x, int y, void** out)
{
    __try {
        *out = g_x.guiHitCheck(guiMan, x, y, 1, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallMapGui(void* guiMan, Vec3 in, Vec3* out)
{
    __try {
        g_x.mapGuiSceneToScreen(guiMan, out, &in);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallGuiScale(float* out)
{
    __try {
        *out = g_x.getGuiScale();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// CTlkTable::GetSimpleString into buf. The CExoString it returns is the game's, so the
// game's destructor frees it.
bool CallTlkString(void* tlk, uint32_t strref, char* buf, size_t size)
{
    ExoString s = {};
    __try {
        g_x.tlkGetSimpleString(tlk, &s, strref);
        size_t n = s.text ? strnlen(s.text, s.length) : 0;
        if (n >= size)
            n = size - 1;
        memcpy(buf, s.text ? s.text : "", n);
        buf[n] = 0;
        g_x.exoStringDtor(&s);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallArrayLookup(void* (*fn)(void*, int), void* array, int id, void** out)
{
    __try {
        *out = fn(array, id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool Resolve(T& out, const char* mangled)
{
    out = reinterpret_cast<T>(GetProcAddress(g_exe, mangled));
    if (!out)
        Log("game: missing export %s", mangled);
    return out != nullptr;
}

bool ResolveExports()
{
    if (g_resolved || g_resolveFailed)
        return g_resolved;
    g_exe = GetModuleHandleA(nullptr);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exe);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const char*>(g_exe) + dos->e_lfanew);
    g_base = reinterpret_cast<uintptr_t>(g_exe);
    g_size = nt->OptionalHeader.SizeOfImage;
    g_caps.knownBuild = nt->FileHeader.TimeDateStamp == kKnownTimestamp && g_size == kKnownImageSize;

    bool ok = true;
    ok &= Resolve(g_x.appManager, "?g_pAppManager@@3PEAVCAppManager@@EA");
    ok &= Resolve(g_x.guiMan, "?g_pGuiMan@@3PEAVCGuiMan@@EA");
    ok &= Resolve(g_x.radialDepth, "?static_nMenuDepth@CRadialMenu@@1HA");
    ok &= Resolve(g_x.getGuiScale, "?GetGUIScale@CAurora@@SAMXZ");
    ok &= Resolve(g_x.guiHitCheck, "?GuiObjectHitCheck@CGuiMan@@QEAAPEAVCGuiObject@@HHHH@Z");
    ok &= Resolve(g_x.mapGuiSceneToScreen, "?MapGuiSceneToScreen@CGuiMan@@QEAA?AVVector@@V2@@Z");
    ok &= Resolve(g_x.camGetPosition, "?GetPosition@Camera@@UEAA?AVVector@@XZ");
    ok &= Resolve(g_x.camMapToScreen, "?MapToScreen@Camera@@UEAA?AVVector@@V2@_N@Z");
    ok &= Resolve(g_x.camGetViewAngle, "?GetViewAngle@Camera@@UEAAMXZ");
    ok &= Resolve(g_x.camGetYaw, "?GetYaw@Camera@@UEAAMXZ");
    ok &= Resolve(g_x.camGetPitch, "?GetPitch@Camera@@UEAAMXZ");
    ok &= Resolve(g_x.camGetDist, "?GetDist@Camera@@UEAAMXZ");
    ok &= Resolve(g_x.followAsCameraFollow,
                  "?AsBehaviorCameraFollow@CAurBehaviorCameraFollow@@UEAAPEAV1@XZ");
    if (!ok) {
        g_resolveFailed = true;
        return false;
    }
    Resolve(g_x.serverGetCreature, "?GetCreatureByGameObjectID@CServerExoApp@@QEAAPEAVCNWSCreature@@I@Z");
    Resolve(g_x.serverSetPosition, "?SetPosition@CNWSObject@@QEAAXVVector@@H@Z");
    Resolve(g_x.objGetDead, "?GetDead@CNWSObject@@QEAAHXZ");
    Resolve(g_x.radialHandleKey, "?HandleKeyEvent@CRadialMenu@@MEAAHII@Z");
    Resolve(g_x.tlkTable, "?g_pTlkTable@@3PEAVCTlkTable@@EA");
    Resolve(g_x.rules, "?g_pRules@@3PEAVCNWRules@@EA");
    Resolve(g_x.tlkGetSimpleString, "?GetSimpleString@CTlkTable@@QEAA?AVCExoString@@I@Z");
    Resolve(g_x.exoStringDtor, "??1CExoString@@QEAA@XZ");
    Resolve(g_x.getSpell, "?GetSpell@CNWSpellArray@@QEBAPEAVCNWSpell@@H@Z");
    Resolve(g_x.getBaseItem, "?GetBaseItem@CNWBaseItemArray@@QEBAPEAVCNWBaseItem@@H@Z");
    Resolve(g_x.gobGetTextLine, "?GetTextLine@StringGob@@UEAAPEBDH@Z");
    Resolve(g_x.gobGetTextLineCount, "?GetTextLineCount@StringGob@@UEAAHXZ");
    Resolve(g_x.textureCtor, "??0CAuroraTexture@@QEAA@VCResRef@@H@Z");
    Resolve(g_x.textureLoad, "?LoadTexture@CAuroraTexture@@QEAAHXZ");
    Resolve(g_x.textureDtor, "??1CAuroraTexture@@UEAA@XZ");
    Resolve(g_x.panelSetPosition, "?SetPosition@CGuiPanel@@QEAAXVVector@@@Z");
    Resolve(g_x.panelSetPositionScreen, "?SetPositionScreen@CGuiPanel@@QEAAXHH@Z");
    g_resolved = true;
    return true;
}

void* ObjectArray()
{
    void* arr = nullptr;
    void* app = AppInternal();
    if (!app || !Read(app, kAppObjectArray, &arr) || !Plausible(arr))
        return nullptr;
    return arr;
}

bool CameraSlotsMatch(void* cam)
{
    return Slot<void*>(cam, kCamSlotGetPosition) == g_x.camGetPosition &&
           Slot<void*>(cam, kCamSlotMapToScreen) == g_x.camMapToScreen &&
           Slot<void*>(cam, kCamSlotGetViewAngle) == g_x.camGetViewAngle &&
           Slot<void*>(cam, kCamSlotGetYaw) == g_x.camGetYaw &&
           Slot<void*>(cam, kCamSlotGetPitch) == g_x.camGetPitch &&
           Slot<void*>(cam, kCamSlotGetDist) == g_x.camGetDist;
}

void* PanelListHead()
{
    void* gm = GuiMan();
    void* p = nullptr;
    void* list = nullptr;
    void* head = nullptr;
    if (!gm || !Read(gm, 0, &p) || !Plausible(p) || !Read(p, 0, &list) || !Plausible(list) ||
        !Read(list, 0, &head))
        return nullptr;
    return head;
}

// Self-test failure details, logged once per distinct result so a broken
// offset shows up in the log without a debugger.
void LogProbe(char (&prev)[256], const char* line)
{
    if (strcmp(prev, line) == 0)
        return;
    snprintf(prev, sizeof(prev), "%s", line);
    Log("game: probe %s", line);
}

void LogPlayerProbe(uint32_t oid, void* obj, uint8_t type, uint32_t selfOid, void* asCreature)
{
    void* vt = nullptr;
    if (obj)
        Read(obj, 0, &vt);
    char buf[256];
    snprintf(buf, sizeof(buf), "player: oid=%08x obj=%p vt+%x type=%u selfOid=%08x asCreature=%p", oid, obj,
             Rva(vt), type, selfOid, asCreature);
    static char last[256];
    LogProbe(last, buf);
}

void LogCameraProbe(void* cam)
{
    void* vt = nullptr;
    Read(cam, 0, &vt);
    char buf[256];
    snprintf(buf, sizeof(buf), "camera: %p vt+%x slots %x %x %x %x %x %x want %x %x %x %x %x %x", cam, Rva(vt),
             Rva(Slot<void*>(cam, kCamSlotGetPosition)), Rva(Slot<void*>(cam, kCamSlotMapToScreen)),
             Rva(Slot<void*>(cam, kCamSlotGetViewAngle)), Rva(Slot<void*>(cam, kCamSlotGetYaw)),
             Rva(Slot<void*>(cam, kCamSlotGetPitch)), Rva(Slot<void*>(cam, kCamSlotGetDist)),
             Rva(g_x.camGetPosition), Rva(g_x.camMapToScreen), Rva(g_x.camGetViewAngle), Rva(g_x.camGetYaw),
             Rva(g_x.camGetPitch), Rva(g_x.camGetDist));
    static char last[256];
    LogProbe(last, buf);
}

// Total bucket count, either one half (server-side arrays) or both.
uint32_t BucketTotal(void* arr, uint32_t perHalf)
{
    uint32_t total = 0;
    if (!Read(arr, kArrayBucketTotal, &total) || (total != perHalf && total != 2 * perHalf))
        return perHalf;
    return total;
}

} // namespace

bool SafeRead(const void* src, void* dst, size_t n)
{
    if (!src)
        return false;
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t Rva(const void* p)
{
    auto v = reinterpret_cast<uintptr_t>(p);
    return g_base && v >= g_base && v < g_base + g_size ? static_cast<uint32_t>(v - g_base) : 0;
}

const char* ObjectTypeName(uint8_t type)
{
    switch (type) {
    case 4: return "area";
    case kTypeCreature: return "creature";
    case kTypeItem: return "item";
    case kTypeTrigger: return "trigger";
    case 8: return "projectile";
    case kTypePlaceable: return "placeable";
    case kTypeDoor: return "door";
    case 11: return "aoe";
    case 13: return "encounter";
    case 14: return "store";
    case 16: return "sound";
    default: return "?";
    }
}

const Caps& Current()
{
    return g_caps;
}

const Caps& Refresh()
{
    Caps before = g_caps;
    Caps c;
    c.exports = ResolveExports(); // also decides knownBuild
    c.knownBuild = g_caps.knownBuild;
    if (c.exports) {
        void* arr = ObjectArray();
        uint32_t buckets = 0;
        void* table = nullptr;
        c.objects = arr && Read(arr, kArrayBucketCount, &buckets) && buckets > 0 && buckets <= (1u << 22) &&
                    Read(arr, 0, &table) && Plausible(table);
        g_caps.objects = c.objects; // ObjectByOid needs it below

        if (c.objects) {
            uint32_t oid = PlayerOid();
            void* obj = oid != kInvalidOid ? ObjectByOid(oid) : nullptr;
            uint8_t type = 0;
            uint32_t selfOid = 0;
            void* asCreature = nullptr;
            c.player = obj && Read(obj, kObjType, &type) && type == kTypeCreature && Read(obj, kObjOid, &selfOid) &&
                       (selfOid & 0x7fffffff) == (oid & 0x7fffffff) &&
                       CallPtrGetter(Slot<void*>(obj, kObjSlotAsCreature), obj, &asCreature) && asCreature == obj;
            if (!c.player && !g_caps.player)
                LogPlayerProbe(oid, obj, type, selfOid, asCreature);
            c.setFacing = c.player && c.knownBuild &&
                          Rva(Slot<void*>(obj, kObjSlotSetOrientation)) == kSetOrientationRva;
            c.driveState = c.player && c.knownBuild;
        }
        void* cam = Camera();
        c.camera = cam && CameraSlotsMatch(cam);
        if (cam && !c.camera && !g_caps.camera)
            LogCameraProbe(cam);
        c.gui = PanelListHead() != nullptr;
    }
    g_caps = c;
    if (!g_capsLogged || memcmp(&before, &c, sizeof(c)) != 0) {
        g_capsLogged = true;
        Log("game: caps exports=%d knownBuild=%d objects=%d player=%d camera=%d gui=%d setFacing=%d driveState=%d",
            c.exports, c.knownBuild, c.objects, c.player, c.camera, c.gui, c.setFacing, c.driveState);
    }
    return g_caps;
}

void LogBuildInfo(const char* gameDir)
{
    ResolveExports();
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exe);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const char*>(g_exe) + dos->e_lfanew);
    char build[64] = "?";
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sbuild.txt", gameDir);
    if (FILE* f = fopen(path, "r")) {
        if (fgets(build, sizeof(build), f))
            build[strcspn(build, "\r\n")] = 0;
        fclose(f);
    }
    Log("game: nwmain.exe timestamp=0x%08lx image=0x%lx base=%p build.txt=%s (%s)",
        static_cast<unsigned long>(nt->FileHeader.TimeDateStamp), static_cast<unsigned long>(g_size),
        reinterpret_cast<void*>(g_base), build,
        g_caps.knownBuild ? "known build" : "UNKNOWN build: unexported offsets disabled");
}

void* AppInternal()
{
    if (!g_resolved)
        return nullptr;
    void* mgr = nullptr;
    void* client = nullptr;
    void* internal = nullptr;
    if (!Read(g_x.appManager, 0, &mgr) || !Plausible(mgr) || !Read(mgr, 0, &client) || !Plausible(client) ||
        !Read(client, 8, &internal) || !Plausible(internal))
        return nullptr;
    return internal;
}

uint32_t PlayerOid()
{
    uint32_t oid = kInvalidOid;
    void* app = AppInternal();
    if (!app || !Read(app, kAppPlayerOid, &oid))
        return kInvalidOid;
    return oid;
}

void* ObjectByOid(uint32_t oid)
{
    void* arr = ObjectArray();
    uint32_t count = 0;
    void** table = nullptr;
    if (!arr || !Read(arr, kArrayBucketCount, &count) || count == 0 || !Read(arr, 0, &table))
        return nullptr;
    // The game picks the half from bit 31; try the other half too in case an ID
    // reached us with that bit stripped.
    uint32_t halves = BucketTotal(arr, count) / count;
    uint32_t first = (oid & 0x80000000u) && halves == 2 ? 1 : 0;
    for (uint32_t i = 0; i < halves; ++i) {
        void* node = nullptr;
        if (!SafeRead(table + (first ^ i) * count + oid % count, &node, sizeof(node)))
            return nullptr;
        for (int guard = 0; node && guard < 100000; ++guard) {
            uint32_t id = 0;
            void* obj = nullptr;
            if (!Read(node, 0, &id) || !Read(node, 8, &obj))
                return nullptr;
            if (id == (oid & 0x7fffffff))
                return Plausible(obj) ? obj : nullptr;
            if (!Read(node, 0x10, &node))
                return nullptr;
        }
    }
    return nullptr;
}

bool ReadObject(void* obj, ObjectInfo* out)
{
    memset(out, 0, sizeof(*out));
    out->ptr = obj;
    return Plausible(obj) && Read(obj, kObjOid, &out->oid) && Read(obj, kObjType, &out->type) &&
           Read(obj, kObjArea, &out->area) && Read(obj, kObjPosition, &out->pos) &&
           Read(obj, kObjOrientation, &out->facing);
}

bool PlayerInfo(ObjectInfo* out)
{
    if (!g_caps.player)
        return false;
    void* obj = ObjectByOid(PlayerOid());
    return obj && ReadObject(obj, out);
}

int ForEachObject(ObjectVisitor visit, void* ctx)
{
    void* arr = ObjectArray();
    uint32_t count = 0;
    void** table = nullptr;
    if (!g_caps.objects || !arr || !Read(arr, kArrayBucketCount, &count) || !Read(arr, 0, &table))
        return -1;
    int visited = 0;
    uint32_t total = BucketTotal(arr, count);
    for (uint32_t b = 0; b < total; ++b) {
        void* node = nullptr;
        if (!SafeRead(table + b, &node, sizeof(node)))
            return -1;
        for (int guard = 0; node && guard < 100000; ++guard) {
            void* obj = nullptr;
            ObjectInfo info;
            if (Read(node, 8, &obj) && ReadObject(obj, &info)) {
                visit(ctx, info);
                ++visited;
            }
            if (!Read(node, 0x10, &node))
                break;
        }
    }
    return visited;
}

void* Camera()
{
    void* app = AppInternal();
    void* module = nullptr;
    void* cam = nullptr;
    if (!app || !Read(app, kAppModule, &module) || !Plausible(module) || !Read(module, kModuleCamera, &cam) ||
        !Plausible(cam))
        return nullptr;
    return cam;
}

bool CameraPosition(Vec3* out)
{
    void* cam = g_caps.camera ? Camera() : nullptr;
    return cam && CallVec3Getter(g_x.camGetPosition, cam, out);
}

bool CameraAngles(float* yaw, float* pitch, float* dist, float* viewAngle)
{
    void* cam = g_caps.camera ? Camera() : nullptr;
    return cam && CallFloatGetter(g_x.camGetYaw, cam, yaw) && CallFloatGetter(g_x.camGetPitch, cam, pitch) &&
           CallFloatGetter(g_x.camGetDist, cam, dist) && CallFloatGetter(g_x.camGetViewAngle, cam, viewAngle);
}

bool WorldToScreen(const Vec3& world, float* nx, float* ny)
{
    void* cam = g_caps.camera ? Camera() : nullptr;
    Vec3 out{};
    if (!cam || !CallMapToScreen(g_x.camMapToScreen, cam, world, &out))
        return false;
    *nx = out.x;
    *ny = out.y;
    return std::isfinite(out.x) && std::isfinite(out.y);
}

bool RotateCamera(float yawDegrees, float pitchDegrees)
{
    void* cam = g_caps.camera && g_caps.knownBuild ? Camera() : nullptr;
    void* behavior = nullptr;
    if (!cam || !Read(cam, kCamBehavior, &behavior) || !Plausible(behavior) ||
        Slot<void*>(behavior, kBehaviorSlotAsCameraFollow) != g_x.followAsCameraFollow)
        return false;
    static bool logged;
    if (!logged) {
        logged = true;
        float f[16] = {};
        SafeRead(static_cast<char*>(behavior) + 0x48, f, sizeof(f));
        Log("game: camera follow behavior %p dist=%.2f yaw=%.2f pitch=%.2f clamp yaw=%g [%g, %g] pitch=%g [%g, %g] "
            "dist=%g [%g, %g]",
            behavior, f[0], f[3], f[5], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
    }
    char* b = static_cast<char*>(behavior);
    __try {
        *reinterpret_cast<float*>(b + kFollowYawDelta) += yawDegrees;
        *reinterpret_cast<float*>(b + kFollowPitchDelta) += pitchDegrees;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace {

// The in-process server app, or null (remote server, or the build isn't the known one).
void* ServerApp()
{
    void *mgr = nullptr, *server = nullptr;
    if (!g_caps.knownBuild || !Read(g_x.appManager, 0, &mgr) || !Plausible(mgr) ||
        !Read(mgr, kAppManagerServer, &server) || !Plausible(server))
        return nullptr;
    return server;
}

// A server object of one kind for a client OID, checked by its own ID.
void* ServerObject(void* (*lookup)(void*, uint32_t), uint32_t oid)
{
    uint32_t id = oid & 0x7fffffff; // the server's own ID, without the client's high bit
    void* server = lookup ? ServerApp() : nullptr;
    void* obj = nullptr;
    uint32_t self = 0;
    if (!server || !CallServerLookup(lookup, server, id, &obj) || !Plausible(obj) || !Read(obj, kSrvObjId, &self) ||
        self != id)
        return nullptr;
    return obj;
}

// The in-process server's creature for a client OID, or null (remote server, not loaded).
void* ServerCreature(uint32_t oid)
{
    return ServerObject(g_x.serverGetCreature, oid);
}

// The creature's current (first) action node, or null.
void* ServerAction(void* creature, int* type)
{
    void *list = nullptr, *head = nullptr, *action = nullptr;
    *type = -1;
    if (!Read(creature, kSrvActions, &list) || !Plausible(list) || !Read(list, 0, &head) || !Plausible(head) ||
        !Read(head, 0x10, &action) || !Plausible(action) || !Read(action, 0, type))
        return nullptr;
    return action;
}

} // namespace

bool TargetInfoFor(const ObjectInfo& o, uint32_t playerOid, TargetInfo* out)
{
    *out = TargetInfo{};
    if (!g_caps.knownBuild || !Plausible(o.ptr))
        return false;
    // Cast the way the game's click code does, then read the same flags it checks.
    int slot = o.type == kTypeCreature ? kObjSlotAsCreature
               : o.type == kTypeDoor   ? kObjSlotAsDoor
               : o.type == kTypePlaceable ? kObjSlotAsPlaceable
               : o.type == kTypeItem      ? kObjSlotAsItem
                                          : -1;
    void* typed = nullptr;
    if (slot < 0 || !CallPtrGetter(Slot<void*>(o.ptr, slot), o.ptr, &typed) || !Plausible(typed))
        return false;
    int flag = 0;
    if (!Read(typed, o.type == kTypeItem ? kItemPickable : kUseable, &flag))
        return false;
    out->usable = flag != 0;
    if (o.type == kTypeCreature) {
        uint8_t reaction = 0;
        uint32_t master = kInvalidOid;
        Read(typed, kCreatureReaction, &reaction);
        Read(typed, kCreatureMaster, &master);
        out->hostile = reaction > 1;
        out->associate = master != kInvalidOid && (master & 0x7fffffff) == (playerOid & 0x7fffffff);
        // The client has no "dead" flag we trust yet; ask the local server when there is one.
        int dead = 0;
        void* c = ServerCreature(o.oid);
        if (c && g_x.objGetDead && CallIntGetter(g_x.objGetDead, c, &dead) && dead)
            out->usable = false;
    }
    return true;
}

namespace {

using PickFn = int (*)(void* app, void** obj, Vec3* pos, uint32_t* kind, int flags);
PickFn g_pickOriginal[2];
bool g_pickActive;               // controller targeting owns the pick
uint32_t g_pickOid = kInvalidOid; // what it should report, or none

int Pick(int which, void* app, void** outObj, Vec3* outPos, uint32_t* outKind, int flags)
{
    if (g_pickActive) {
        void* obj = g_pickOid != kInvalidOid ? ObjectByOid(g_pickOid) : nullptr;
        Vec3 pos{};
        if (obj && Read(obj, kObjPosition, &pos)) {
            if (outObj)
                *outObj = obj;
            if (outPos)
                *outPos = pos;
            if (outKind)
                *outKind = 1;
            return 2;
        }
        // No target: report nothing, so the pointer (parked mid-screen) doesn't highlight
        // or walk anywhere. While choosing a spell target, fall through so a ground
        // target is still possible.
        if (which == 0) {
            if (outObj)
                *outObj = nullptr;
            if (outPos)
                *outPos = Vec3{};
            if (outKind)
                *outKind = 0;
            return 0;
        }
    }
    return g_pickOriginal[which](app, outObj, outPos, outKind, flags);
}

int HookPick0(void* app, void** obj, Vec3* pos, uint32_t* kind, int flags)
{
    return Pick(0, app, obj, pos, kind, flags);
}

int HookPick1(void* app, void** obj, Vec3* pos, uint32_t* kind, int flags)
{
    return Pick(1, app, obj, pos, kind, flags);
}

// Redirects `target` to `hook` with a 14-byte absolute jump over a prologue we've
// checked byte for byte; the trampoline runs those bytes and jumps back.
bool Detour(uintptr_t target, const uint8_t* prologue, size_t len, void* hook, void** original)
{
    if (len < 14 || memcmp(reinterpret_cast<void*>(target), prologue, len) != 0)
        return false;
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp)
        return false;
    uint8_t jmp[14] = {0xff, 0x25, 0, 0, 0, 0}; // jmp [rip+0]; dq address
    memcpy(tramp, prologue, len);
    uintptr_t back = target + len;
    memcpy(jmp + 6, &back, 8);
    memcpy(tramp + len, jmp, sizeof(jmp));
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(target), len, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    *original = tramp;
    uintptr_t to = reinterpret_cast<uintptr_t>(hook);
    memcpy(jmp + 6, &to, 8);
    auto* t = reinterpret_cast<uint8_t*>(target);
    memcpy(t, jmp, sizeof(jmp));
    memset(t + sizeof(jmp), 0x90, len - sizeof(jmp));
    VirtualProtect(t, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, len);
    return true;
}

} // namespace

bool InstallPickHook()
{
    static int state; // 0 not tried, 1 installed, -1 failed
    if (state)
        return state > 0;
    state = -1;
    if (!g_caps.knownBuild) {
        Log("game: pick hook skipped (unknown build)");
        return false;
    }
    void* hooks[2] = {reinterpret_cast<void*>(&HookPick0), reinterpret_cast<void*>(&HookPick1)};
    for (int i = 0; i < 2; ++i) {
        uintptr_t target = g_base + kPickRva[i];
        void* original = nullptr;
        bool ok = false;
        __try {
            ok = Detour(target, kPickPrologue, sizeof(kPickPrologue), hooks[i], &original);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        if (!ok) {
            Log("game: pick hook failed at %p (prologue mismatch?); targeting off", reinterpret_cast<void*>(target));
            return false; // the first one stays hooked if only the second failed; it passes through while inactive
        }
        g_pickOriginal[i] = reinterpret_cast<PickFn>(original);
    }
    state = 1;
    Log("game: pick hooks installed");
    return true;
}

namespace {
int (*g_testOriginal)(void*, int);
int TestHook(void* p, int n)
{
    return g_testOriginal(p, n * 2);
}
} // namespace

bool DetourSelfTest()
{
    // A stand-in with the pick routines' exact prologue: pushes, pops, return n + 5.
    static const uint8_t kBody[] = {0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5f,
                                    0x5e, 0x5b, 0x5d, 0x8d, 0x42, 0x05, 0xc3};
    auto* fn = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!fn)
        return false;
    memcpy(fn, kPickPrologue, sizeof(kPickPrologue));
    memcpy(fn + sizeof(kPickPrologue), kBody, sizeof(kBody));
    auto call = reinterpret_cast<int (*)(void*, int)>(fn);
    bool before = call(nullptr, 10) == 15;
    void* original = nullptr;
    bool hooked = Detour(reinterpret_cast<uintptr_t>(fn), kPickPrologue, sizeof(kPickPrologue),
                         reinterpret_cast<void*>(&TestHook), &original);
    g_testOriginal = reinterpret_cast<int (*)(void*, int)>(original);
    bool after = hooked && call(nullptr, 10) == 25; // hook doubles, original adds 5
    return before && after;
}

void SetPickTarget(bool active, uint32_t oid)
{
    g_pickActive = active && g_pickOriginal[0] && g_pickOriginal[1];
    g_pickOid = oid;
}

bool ReadMoveTrace(uint32_t oid, MoveTrace* out)
{
    memset(out, 0, sizeof(*out));
    out->serverAction = -1;
    void* obj = g_caps.knownBuild ? ObjectByOid(oid) : nullptr;
    if (!obj || !Read(obj, kObjPosition, &out->client) || !Read(obj, kObjPredictBase, &out->predictBase) ||
        !Read(obj, kObjAnimation, &out->animation))
        return false;
    if (void* c = ServerCreature(oid)) {
        out->haveServer = Read(c, kSrvPosition, &out->server);
        ServerAction(c, &out->serverAction);
    }
    return true;
}

float SyncServerToClient(uint32_t oid, float maxGap)
{
    void* creature = ServerCreature(oid);
    void* obj = ObjectByOid(oid);
    int type = 0;
    Vec3 client{}, server{};
    if (!creature || !obj || !g_x.serverSetPosition || !Read(obj, kObjPosition, &client) ||
        !Read(creature, kSrvPosition, &server))
        return -1;
    if (ServerAction(creature, &type))
        return -1; // still driving, or doing something else the player asked for
    float dx = client.x - server.x, dy = client.y - server.y;
    float gap = sqrtf(dx * dx + dy * dy);
    if (gap > maxGap)
        return -1; // a real disagreement, not prediction overshoot
    // Make the client's server-confirmed position match what's drawn, so it has
    // nothing to ease back to, and move the server there so later updates agree.
    __try {
        memcpy(static_cast<char*>(obj) + kObjPredictBase, &client, sizeof(client));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (gap > 0.005f && !CallServerSetPosition(creature, client))
        return -1;
    return gap;
}

DriveStop StopServerDrive(uint32_t oid, const Vec3& stopAt, float* gap)
{
    void* creature = ServerCreature(oid);
    if (!creature)
        return DriveStop::Unavailable;
    void *action = nullptr, *path = nullptr, *waypoints = nullptr;
    int type = 0;
    if (!(action = ServerAction(creature, &type)) || type != kActionDrive)
        return DriveStop::NotDriving;
    float pos[2];
    if (!Read(creature, kSrvPathfind, &path) || !Plausible(path) || !Read(path, kPathWaypoints, &waypoints) ||
        !Plausible(waypoints) || !SafeRead(static_cast<char*>(creature) + kSrvPosition, pos, sizeof(pos)))
        return DriveStop::Unavailable;
    // End the chain and move the current waypoint to where the client shows the
    // character. The server, which trails the client's prediction slightly, walks
    // that last bit and stops, so the client isn't pulled back to the server's spot.
    // A large gap means something else is going on; then stop on the server's spot.
    float dx = stopAt.x - pos[0], dy = stopAt.y - pos[1];
    float d = sqrtf(dx * dx + dy * dy);
    if (gap)
        *gap = d;
    if (d < 2.0f) {
        pos[0] = stopAt.x;
        pos[1] = stopAt.y;
    }
    __try {
        *reinterpret_cast<int*>(static_cast<char*>(action) + kActionDriveChain) = 0;
        memcpy(waypoints, pos, sizeof(pos));
        return DriveStop::Stopped;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return DriveStop::Unavailable;
    }
}

bool SetFacing(void* obj, const Vec3& dir)
{
    if (!g_caps.setFacing)
        return false;
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
    if (len < 1e-4f)
        return false;
    Vec3 unit{dir.x / len, dir.y / len, 0.0f};
    void* fn = Slot<void*>(obj, kObjSlotSetOrientation);
    return Rva(fn) == kSetOrientationRva && CallSetOrientation(fn, obj, &unit);
}

bool ForceDriveSend()
{
    // FUN_1406f1e40 sends DriveControl immediately when this int is nonzero,
    // then clears it; otherwise it resends at most every 250-500 ms.
    void* app = g_caps.driveState ? AppInternal() : nullptr;
    if (!app)
        return false;
    int one = 1;
    __try {
        memcpy(static_cast<char*>(app) + kAppDriveState, &one, sizeof(one));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InputAction(int action, bool down)
{
    void* app = g_caps.knownBuild ? AppInternal() : nullptr;
    if (!app)
        return false;
    using Fn = uint32_t (*)(void* app, uint32_t action, uint32_t down, uint32_t passthrough, int fromKeyboard);
    auto fn = reinterpret_cast<Fn>(g_base + kInputActionRva);
    __try {
        fn(app, static_cast<uint32_t>(action), down ? 1u : 0u, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace {

// Sets one of the game's int settings to 1, or puts back the value it had.
bool OverrideGameFlag(uint32_t rva, bool on, int* saved)
{
    auto* flag = reinterpret_cast<int*>(g_base + rva);
    __try {
        if (on) {
            if (*saved < 0)
                *saved = *flag;
            *flag = 1;
        } else if (*saved >= 0) {
            *flag = *saved;
            *saved = -1;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallRadialKey(void* radial, uint32_t key)
{
    __try {
        g_x.radialHandleKey(radial, key, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool SetRadialCentered(bool on)
{
    static int savedCentered = -1, savedToParent = -1; // the game's own values
    if (!g_caps.knownBuild)
        return false;
    return OverrideGameFlag(kRadialCenteredRva, on, &savedCentered) &&
           OverrideGameFlag(kRadialCancelToParentRva, on, &savedToParent);
}

// The open radial menu: it holds the GUI mouse capture while open.
static void* OpenRadial()
{
    void* radial = g_caps.knownBuild && g_x.radialHandleKey && RadialDepth() > 0 ? MouseCapture() : nullptr;
    if (!Plausible(radial))
        return nullptr;
    // Only a radial menu has CRadialMenu::HandleKeyEvent in its vtable.
    for (int i = 0; i < 64; ++i)
        if (Slot<void*>(radial, i) == reinterpret_cast<void*>(g_x.radialHandleKey))
            return radial;
    return nullptr;
}

bool RadialBack()
{
    void* radial = OpenRadial();
    return radial && CallRadialKey(radial, kRadialCancelAction);
}

bool RadialSlots(uint8_t* shown, uint8_t* enabled, int* selected)
{
    void* radial = OpenRadial();
    if (!radial)
        return false;
    void* icons[8];
    uint32_t flags[8];
    if (!SafeRead(static_cast<char*>(radial) + kRadialSlotIcons, icons, sizeof(icons)) ||
        !SafeRead(static_cast<char*>(radial) + kRadialSlotFlags, flags, sizeof(flags)) ||
        !Read(radial, kRadialSelected, selected))
        return false;
    *shown = *enabled = 0;
    for (int i = 0; i < 8; ++i) {
        if (!icons[i])
            continue;
        *shown |= 1 << i;
        if (!(flags[i] & 1))
            *enabled |= 1 << i;
    }
    return true;
}

namespace {

// The in-game GUI's panel at `off`, if it has the expected vtable.
void* InGamePanel(ptrdiff_t off, uint32_t vtableRva)
{
    void* app = AppInternal();
    void* gui = nullptr;
    void* panel = nullptr;
    void* vt = nullptr;
    if (!g_caps.knownBuild || !app || !Read(app, kAppInGameGui, &gui) || !Plausible(gui) || !Read(gui, off, &panel) ||
        !Plausible(panel) || !Read(panel, 0, &vt) || Rva(vt) != vtableRva)
        return nullptr;
    return panel;
}

void* QuickbarPanel()
{
    return InGamePanel(kInGameGuiQuickbar, kQuickbarVtableRva);
}

bool ReadExoString(const void* base, ptrdiff_t off, char* buf, size_t size)
{
    ExoString s = {};
    buf[0] = 0;
    if (!Read(base, off, &s))
        return false;
    size_t n = s.length < size ? s.length : size - 1;
    if (!s.text || n == 0)
        return true;
    if (!SafeRead(s.text, buf, n))
        return false;
    buf[n] = 0;
    return true;
}

bool TlkString(uint32_t strref, char* buf, size_t size)
{
    void* tlk = nullptr;
    buf[0] = 0;
    return strref != 0xffffffffu && g_x.tlkTable && g_x.tlkGetSimpleString && g_x.exoStringDtor &&
           Read(g_x.tlkTable, 0, &tlk) && Plausible(tlk) && CallTlkString(tlk, strref, buf, size);
}

// An entry of one of CNWRules' arrays (spells, base items) through its exported getter.
void* RulesEntry(ptrdiff_t arrayOff, void* (*getter)(void*, int), int id)
{
    void* rules = nullptr;
    void* array = nullptr;
    void* entry = nullptr;
    if (!getter || !g_x.rules || !Read(g_x.rules, 0, &rules) || !Plausible(rules) || !Read(rules, arrayOff, &array) ||
        !Plausible(array) || !CallArrayLookup(getter, array, id, &entry))
        return nullptr;
    return entry;
}

bool ItemName(uint32_t oid, char* buf, size_t size)
{
    buf[0] = 0;
    void* obj = ObjectByOid(oid);
    void* item = nullptr;
    uint8_t type = 0;
    if (!obj || !Read(obj, kObjType, &type) || type != kTypeItem ||
        !CallPtrGetter(Slot<void*>(obj, kObjSlotAsItem), obj, &item) || !Plausible(item))
        return false;
    int identified = 0, baseItem = 0;
    if (Read(item, kItemIdentified, &identified) && identified && ReadExoString(item, kItemName, buf, size) && buf[0])
        return true;
    void* base = nullptr;
    uint32_t strref = 0;
    return Read(item, kItemBaseItem, &baseItem) &&
           (base = RulesEntry(kRulesBaseItems, g_x.getBaseItem, baseItem)) != nullptr && Read(base, 0, &strref) &&
           TlkString(strref, buf, size);
}

// What a button shows, read fresh. The cache below only calls this when the button's
// fields change.
bool ReadSlotName(const char* button, uint8_t type, char* buf, size_t size)
{
    buf[0] = 0;
    switch (type) {
    case kQuickEmpty: return true;
    case kQuickItem: {
        uint32_t oid = 0;
        return Read(button, kQbtnItem, &oid) && ItemName(oid, buf, size);
    }
    case kQuickSpell:
    case kQuickSpellAbility: {
        uint16_t id = 0;
        void* spell = nullptr;
        uint32_t strref = 0;
        return Read(button, kQbtnSpell, &id) && (spell = RulesEntry(kRulesSpells, g_x.getSpell, id)) != nullptr &&
               Read(spell, kSpellNameStrref, &strref) && TlkString(strref, buf, size);
    }
    case kQuickCommand: return ReadExoString(button, kQbtnCommandLabel, buf, size);
    default: {
        uint32_t strref = 0xffffffffu;
        if (ReadExoString(button, kQbtnName, buf, size) && buf[0])
            return true;
        return Read(button, kQbtnStrref, &strref) && TlkString(strref, buf, size);
    }
    }
}

// The fields a button's name comes from, plus the item's own name fields, so a renamed
// or newly identified item refreshes too.
struct SlotKey {
    uint8_t type;
    uint32_t strref, item;
    ExoString name, command;
    uint64_t spell;
    ExoString itemName;
    int identified;

    bool operator==(const SlotKey& o) const
    {
        return type == o.type && strref == o.strref && item == o.item && name.text == o.name.text &&
               name.length == o.name.length && command.text == o.command.text &&
               command.length == o.command.length && spell == o.spell && itemName.text == o.itemName.text &&
               itemName.length == o.itemName.length && identified == o.identified;
    }
};

struct SlotCache {
    bool valid;
    SlotKey key;
    char name[96];
};
SlotCache g_slotCache[kQbSlots];

bool ReadSlotKey(const char* button, SlotKey* k)
{
    *k = SlotKey{};
    if (!Read(button, kQbtnType, &k->type))
        return false;
    Read(button, kQbtnStrref, &k->strref);
    Read(button, kQbtnItem, &k->item);
    Read(button, kQbtnName, &k->name);
    Read(button, kQbtnCommandLabel, &k->command);
    Read(button, kQbtnSpell, &k->spell);
    if (k->type == kQuickItem) {
        void* obj = ObjectByOid(k->item);
        void* item = nullptr;
        if (obj && CallPtrGetter(Slot<void*>(obj, kObjSlotAsItem), obj, &item) && Plausible(item)) {
            Read(item, kItemName, &k->itemName);
            Read(item, kItemIdentified, &k->identified);
        }
    }
    return true;
}

} // namespace

int QuickbarPage()
{
    void* panel = QuickbarPanel();
    char* shown = nullptr;
    if (!panel || !Read(panel, kQbShownPage, &shown))
        return -1;
    ptrdiff_t off = shown - (static_cast<char*>(panel) + kQbButtons), page = 12 * kQbButtonSize;
    return off >= 0 && off % page == 0 && off / page < 3 ? static_cast<int>(off / page) : -1;
}

bool QuickbarSlotName(int index, uint8_t* type, char* buf, size_t size)
{
    void* panel = QuickbarPanel();
    if (!panel || index < 0 || index >= kQbSlots || size == 0)
        return false;
    const char* button = static_cast<char*>(panel) + kQbButtons + index * kQbButtonSize;
    SlotKey key;
    if (!ReadSlotKey(button, &key))
        return false;
    SlotCache& c = g_slotCache[index];
    if (!c.valid || !(c.key == key)) {
        // An item whose object isn't in yet reads as unnamed; leave it uncached so it
        // fills in once the object arrives.
        c.valid = ReadSlotName(button, key.type, c.name, sizeof(c.name)) && (key.type == kQuickEmpty || c.name[0]);
        c.key = key;
    }
    *type = key.type;
    snprintf(buf, size, "%s", c.name);
    return true;
}

namespace {

// The conversation window while it's up: flagged open and in the GUI's list of
// shown panels (both, so a stale flag can't leave dialogue mode stuck on).
void* OpenDialog()
{
    void* panel = InGamePanel(kInGameGuiDialog, kDialogVtableRva);
    void* open = nullptr;
    if (!panel || !Read(panel, kPanelOpen, &open) || !open)
        return nullptr;
    struct Find {
        void* want;
        bool found;
    } find = {panel, false};
    ForEachPanel([](void* ctx, void* p) {
        auto f = static_cast<Find*>(ctx);
        f->found |= p == f->want;
    }, &find);
    static char last[256];
    LogProbe(last, find.found ? "dialog: window open and shown" : "dialog: window flagged open but not shown");
    return find.found ? panel : nullptr;
}

// Copies line i of a StringGob into dst (at most room-1 chars, terminated); the
// game's pointer is only good until its text changes, so it's copied at once.
bool CallGobLine(void* fn, void* gob, int i, char* dst, size_t room)
{
    __try {
        const char* src = reinterpret_cast<const char* (*)(void*, int)>(fn)(gob, i);
        size_t n = src ? strnlen(src, room - 1) : 0;
        memcpy(dst, src ? src : "", n);
        dst[n] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The dialog panel's text object at `off`, if it's the StringGob we expect.
void* DialogGob(void* panel, ptrdiff_t off)
{
    void* gob = nullptr;
    if (!panel || !g_x.gobGetTextLine || !g_x.gobGetTextLineCount || !Read(panel, off, &gob) || !Plausible(gob))
        return nullptr;
    void* line = Slot<void*>(gob, kGobSlotGetTextLine);
    void* count = Slot<void*>(gob, kGobSlotGetTextLineCount);
    if (line != g_x.gobGetTextLine || count != g_x.gobGetTextLineCount) {
        char buf[256];
        snprintf(buf, sizeof(buf), "dialog: text object +%tx slots %x %x, want %x %x", off, Rva(line), Rva(count),
                 Rva(g_x.gobGetTextLine), Rva(g_x.gobGetTextLineCount));
        static char last[256];
        LogProbe(last, buf);
        return nullptr;
    }
    return gob;
}

// All of a StringGob's lines into buf, a space added at breaks that have none.
bool ReadGob(void* gob, char* buf, size_t size)
{
    buf[0] = 0;
    int lines = 0;
    if (!gob || !CallIntGetter(reinterpret_cast<int (*)(void*)>(g_x.gobGetTextLineCount), gob, &lines))
        return false;
    size_t len = 0;
    for (int i = 0; i < lines && i < 500 && len + 2 < size; ++i) {
        char line[1024];
        if (!CallGobLine(g_x.gobGetTextLine, gob, i, line, sizeof(line)))
            return false;
        if (!line[0])
            continue;
        if (len && !isspace(static_cast<unsigned char>(buf[len - 1])) && !isspace(static_cast<unsigned char>(line[0])))
            buf[len++] = ' ';
        size_t n = strlen(line);
        if (n > size - 1 - len)
            n = size - 1 - len;
        memcpy(buf + len, line, n);
        len += n;
        buf[len] = 0;
    }
    return true;
}

bool CallDialogChoose(void* panel, int i)
{
    __try {
        reinterpret_cast<void (*)(void*, int)>(g_base + kDialogChooseRva)(panel, i);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

int DialogReplyCount()
{
    void* panel = OpenDialog();
    int count = 0;
    if (!panel || !Read(panel, kDialogReplyCount, &count) || count < 0 || count > 100)
        return -1;
    return count;
}

bool DialogReply(int i, char* buf, size_t size, bool* enabled)
{
    void* panel = OpenDialog();
    int count = 0;
    char* texts = nullptr;
    uint32_t* flags = nullptr;
    uint32_t f = 0;
    if (!panel || !Read(panel, kDialogReplyCount, &count) || i < 0 || i >= count || !Read(panel, kDialogReplyTexts, &texts) ||
        !Read(panel, kDialogReplyFlags, &flags) || !texts || !flags || !SafeRead(flags + i, &f, sizeof(f)))
        return false;
    *enabled = !(f & 1);
    return ReadExoString(texts + i * sizeof(ExoString), 0, buf, size);
}

bool DialogChoose(int i)
{
    void* panel = OpenDialog();
    return panel && i >= 0 && i < DialogReplyCount() && CallDialogChoose(panel, i);
}

bool DialogText(char* speaker, size_t speakerSize, char* line, size_t lineSize)
{
    void* panel = OpenDialog();
    speaker[0] = line[0] = 0;
    void* entry = DialogGob(panel, kDialogEntryText);
    if (!entry || !ReadGob(entry, line, lineSize))
        return false;
    ReadGob(DialogGob(panel, kDialogSpeakerText), speaker, speakerSize); // a nameless speaker is fine
    return true;
}

namespace {

bool CallPortraitResRef(const ResRef* base, ResRef* out, uint8_t size)
{
    __try {
        reinterpret_cast<void (*)(const ResRef*, ResRef*, uint8_t)>(g_base + kPortraitResRefRva)(base, out, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// vtbl +0x1d0: the object's portrait as a CResRef returned by value (into *out).
bool CallObjectPortrait(void* fn, void* object, ResRef* out, int size)
{
    __try {
        reinterpret_cast<ResRef* (*)(void*, ResRef*, int)>(fn)(object, out, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Loads `ref` through a CAuroraTexture living in our own memory (so its destructor,
// not the game's operator delete, ends it) and copies the TGA header and pixels out
// before that destructor releases the resource.
bool CallLoadTga(const ResRef* ref, uint8_t header[18], uint8_t* pixels, size_t cap, size_t* got)
{
    alignas(16) unsigned char tex[kAuroraTextureSize + 16] = {};
    __try {
        g_x.textureCtor(tex, ref, 0);
        bool ok = g_x.textureLoad(tex) != 0;
        const char* res = ok ? *reinterpret_cast<char**>(tex + kTextureRes) : nullptr;
        const uint8_t* hdr = res ? *reinterpret_cast<const uint8_t* const*>(res + kTgaHeader) : nullptr;
        const uint8_t* px = res ? *reinterpret_cast<const uint8_t* const*>(res + kTgaPixels) : nullptr;
        ok = hdr && px;
        if (ok) {
            memcpy(header, hdr, 18);
            size_t w = header[12] | header[13] << 8, h = header[14] | header[15] << 8, need = w * h * (header[16] / 8);
            ok = need > 0 && need <= cap;
            if (ok) {
                memcpy(pixels, px, need);
                *got = need;
            }
        }
        g_x.textureDtor(tex);
        return ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSetPanelScreen(void* panel, int x, int y)
{
    __try {
        g_x.panelSetPositionScreen(panel, x, y);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSetPanelPosition(void* panel, Vec3 pos)
{
    __try {
        g_x.panelSetPosition(panel, &pos);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool PanelPosition(void* panel, Vec3* out)
{
    void* model = nullptr;
    return Read(panel, kGuiObjModel, &model) && Plausible(model) &&
           CallVec3Getter(Slot<void*>(model, kModelSlotGetPosition), model, out);
}

struct DialogHide {
    void* panel;
    bool hidden;
    Vec3 home; // where the game had the window
    Vec3 away; // where we put it
} g_dialogHide;

} // namespace

bool DialogPortrait(int size, char* resref, size_t resrefSize)
{
    resref[0] = 0;
    void* player = ObjectByOid(PlayerOid());
    uint32_t speaker = kInvalidOid;
    if (!OpenDialog() || !player || !Read(player, kCreatureTalkingTo, &speaker))
        return false;
    ResRef out = {};
    bool ok = false;
    if (void* obj = speaker != kInvalidOid ? ObjectByOid(speaker) : nullptr) {
        void* object = nullptr;
        void* isCreature = nullptr;
        void* creature = nullptr;
        void* stats = nullptr;
        if (CallPtrGetter(Slot<void*>(obj, kObjSlotAsObject), obj, &object) && Plausible(object)) {
            if (CallPtrGetter(Slot<void*>(obj, kObjSlotIsCreature), obj, &isCreature) && isCreature)
                CallPtrGetter(Slot<void*>(obj, kObjSlotAsCreature), obj, &creature);
            if (Plausible(creature))
                ok = Read(creature, kCreatureStats, &stats) && Plausible(stats) &&
                     CallPortraitResRef(reinterpret_cast<const ResRef*>(static_cast<char*>(stats) + kStatsPortrait),
                                        &out, static_cast<uint8_t>(size));
            else
                ok = CallObjectPortrait(Slot<void*>(object, kObjSlotPortrait), object, &out, size);
        }
    }
    if (!ok || !out.name[0]) { // no speaker, or one without a portrait: the window's default
        ResRef base = {};
        memcpy(base.name, "po_hu_m_99_", 11);
        out = ResRef{};
        if (!CallPortraitResRef(&base, &out, static_cast<uint8_t>(size)))
            return false;
    }
    snprintf(resref, resrefSize, "%.16s", out.name);
    return resref[0] != 0;
}

bool LoadTextureRgba(const char* resref, std::vector<uint8_t>* rgba, int* width, int* height)
{
    if (!g_caps.knownBuild || !g_x.textureCtor || !g_x.textureLoad || !g_x.textureDtor || !resref || !resref[0])
        return false;
    ResRef ref = {};
    for (size_t i = 0; i < sizeof(ref.name) && resref[i]; ++i)
        ref.name[i] = static_cast<char>(tolower(static_cast<unsigned char>(resref[i]))); // the size suffix is upper case
    uint8_t header[18];
    std::vector<uint8_t> raw(1024 * 1024 * 4);
    size_t got = 0;
    if (!CallLoadTga(&ref, header, raw.data(), raw.size(), &got))
        return false;
    int type = header[2], bpp = header[16] / 8, w = header[12] | header[13] << 8, h = header[14] | header[15] << 8;
    bool topDown = (header[17] & 0x20) != 0;
    if ((type != 2 && type != 10) || (bpp != 3 && bpp != 4))
        return false; // colour-mapped or greyscale: not worth it for a portrait
    rgba->resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = raw.data() + static_cast<size_t>(topDown ? y : h - 1 - y) * w * bpp;
        uint8_t* dst = rgba->data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x, src += bpp, dst += 4) {
            dst[0] = src[0]; // already RGB: OnResourceServiced swapped the TGA's BGR
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = bpp == 4 ? src[3] : 255;
        }
    }
    *width = w;
    *height = h;
    return true;
}

bool HideDialogWindow(bool hide)
{
    DialogHide& d = g_dialogHide;
    static char last[256];
    void* panel = InGamePanel(kInGameGuiDialog, kDialogVtableRva);
    if (!panel || !g_x.panelSetPosition || !g_x.panelSetPositionScreen) {
        if (d.hidden)
            Log("dialog: the game closed its window (it makes a new one for each conversation)");
        d.hidden = false;
        return false;
    }
    if (panel != d.panel) {
        if (d.hidden)
            Log("dialog: the game made a new conversation window");
        d.panel = panel;
        d.hidden = false;
    }
    Vec3 now;
    if (!PanelPosition(panel, &now)) {
        LogProbe(last, d.hidden ? "dialog: window position unreadable while hidden" : "dialog: window position unreadable");
        return d.hidden;
    }
    if (hide) {
        bool stillAway = d.hidden && fabsf(now.x - d.away.x) < 0.5f && fabsf(now.y - d.away.y) < 0.5f;
        if (!stillAway) { // first time, or the game placed the window again
            static int s_replaced;
            if (!d.hidden)
                Log("dialog: moving the game's window out of sight (it was at %.0f, %.0f)", now.x, now.y);
            else if (++s_replaced <= 5)
                Log("dialog: the game put its window at (%.0f, %.0f) again; moving it away again", now.x, now.y);
            d.home = now;
            d.hidden = CallSetPanelScreen(panel, kOffscreen, kOffscreen) && PanelPosition(panel, &d.away);
            if (!d.hidden)
                Log("dialog: couldn't move the game's window");
            else if (!s_replaced)
                Log("dialog: window now at (%.0f, %.0f)", d.away.x, d.away.y);
        }
    } else if (d.hidden) {
        CallSetPanelPosition(panel, d.home);
        d.hidden = false;
        Log("dialog: game window put back");
    }
    return d.hidden;
}

int ForEachDialogLine(LineVisitor visit, void* ctx)
{
    void* gob = DialogGob(OpenDialog(), kDialogEntryText);
    int lines = 0;
    if (!gob || !CallIntGetter(reinterpret_cast<int (*)(void*)>(g_x.gobGetTextLineCount), gob, &lines))
        return -1;
    int n = 0;
    for (int i = 0; i < lines && i < 500; ++i) {
        char line[1024];
        if (!CallGobLine(g_x.gobGetTextLine, gob, i, line, sizeof(line)))
            break;
        visit(ctx, i, line);
        ++n;
    }
    return n;
}

bool ReadDriveState(int out[8])
{
    void* app = g_caps.driveState ? AppInternal() : nullptr;
    return app && SafeRead(static_cast<char*>(app) + kAppDriveState, out, 8 * sizeof(int));
}

void* GuiMan()
{
    void* gm = nullptr;
    if (!g_resolved || !Read(g_x.guiMan, 0, &gm) || !Plausible(gm))
        return nullptr;
    return gm;
}

int RadialDepth()
{
    int d = 0;
    return g_resolved && Read(g_x.radialDepth, 0, &d) ? d : 0;
}

void* KeyboardCapture()
{
    void* p = nullptr;
    void* gm = GuiMan();
    return gm && Read(gm, kGuiKeyboardCapture, &p) ? p : nullptr;
}

void* MouseCapture()
{
    void* p = nullptr;
    void* gm = GuiMan();
    return gm && Read(gm, kGuiMouseCapture, &p) ? p : nullptr;
}

bool GuiMousePos(int* x, int* y)
{
    void* gm = GuiMan();
    return gm && Read(gm, kGuiMouseX, x) && Read(gm, kGuiMouseY, y);
}

float GuiScale()
{
    float s = 0.0f;
    return g_resolved && CallGuiScale(&s) ? s : 0.0f;
}

int ForEachPanel(PanelVisitor visit, void* ctx)
{
    void* node = PanelListHead();
    int n = 0;
    for (int guard = 0; node && guard < 1000; ++guard) {
        void* panel = nullptr;
        if (!Read(node, 0x10, &panel))
            break;
        if (Plausible(panel)) {
            visit(ctx, panel);
            ++n;
        }
        if (!Read(node, 8, &node))
            break;
    }
    return n;
}

bool ModelName(void* model, char* buf, size_t size)
{
    void* name = nullptr;
    if (size == 0 || !Plausible(model) || !CallPtrGetter(Slot<void*>(model, kModelSlotGetName), model, &name) ||
        !name)
        return false;
    size_t i = 0;
    for (; i + 1 < size; ++i) {
        char ch = 0;
        if (!SafeRead(static_cast<const char*>(name) + i, &ch, 1) || ch == 0)
            break;
        buf[i] = (ch >= 32 && ch < 127) ? ch : '?';
    }
    buf[i] = 0;
    return i > 0;
}

bool ModelScreenPos(void* model, Vec3* out)
{
    void* gm = GuiMan();
    Vec3 scene{};
    return gm && Plausible(model) && CallVec3Getter(Slot<void*>(model, kModelSlotGetPosition), model, &scene) &&
           CallMapGui(gm, scene, out);
}

bool IsHudPanel(const void* panel)
{
    void* vt = nullptr;
    if (!Read(panel, 0, &vt))
        return false;
    uint32_t rva = Rva(vt);
    for (uint32_t hud : kHudPanelVtables)
        if (rva == hud)
            return true;
    return false;
}

void* OpenMenuPanel()
{
    if (!g_caps.knownBuild)
        return nullptr;
    void* found = nullptr;
    ForEachPanel(
        [](void* ctx, void* panel) {
            if (!IsHudPanel(panel))
                *static_cast<void**>(ctx) = panel; // the list runs oldest first: keep the newest
        },
        &found);
    return found;
}

void* ControlRoot(void* control)
{
    void* at = control;
    for (int guard = 0; at && guard < 32; ++guard) {
        void* parent = nullptr;
        if (!Read(at, kGuiObjParent, &parent) || !Plausible(parent))
            return at;
        at = parent;
    }
    return at;
}

bool IsFocusable(void* control)
{
    void* vt = nullptr;
    void* asPanel = nullptr;
    if (!g_caps.knownBuild || !Read(control, 0, &vt) || Rva(vt) == kGuiLabelVtable)
        return false;
    return CallPtrGetter(Slot<void*>(control, kGuiSlotAsGuiPanel), control, &asPanel) && !asPanel;
}

bool IsTabButton(const void* control)
{
    void* vt = nullptr;
    return g_caps.knownBuild && Read(control, 0, &vt) && Rva(vt) == kGuiTabButtonVtable;
}

void* GuiHitTest(int x, int y)
{
    void* gm = GuiMan();
    void* hit = nullptr;
    return gm && CallHitCheck(gm, x, y, &hit) ? hit : nullptr;
}

const char* ClassForVtable(const void* obj)
{
    static const struct { uint32_t rva; const char* name; } kNames[] = {
#include "vtable_names.inc"
    };
    void* vt = nullptr;
    if (!g_caps.knownBuild || !Read(obj, 0, &vt))
        return nullptr;
    uint32_t rva = Rva(vt);
    for (const auto& n : kNames)
        if (n.rva == rva)
            return n.name;
    return nullptr;
}

} // namespace game
