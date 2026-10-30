// Access to NWN:EE client internals.
//
// Three kinds of dependency, from most to least stable (see docs/DESIGN.md §3.1):
//   - exported symbols (BioWare's C++ exports), resolved by mangled name;
//   - virtual calls whose vtable slots are first checked against those exports;
//   - struct offsets taken from build 26c6e573, each covered by a runtime self-test.
// Every read and call is fault-guarded. A failed check turns the matching
// capability off instead of crashing the game.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace game {

struct Vec3 {
    float x, y, z;
};

enum ObjectType : uint8_t {
    kTypeCreature = 5,
    kTypeItem = 6,
    kTypeTrigger = 7,
    kTypePlaceable = 9,
    kTypeDoor = 10,
};

struct Caps {
    bool exports = false;    // every required exported symbol resolved
    bool knownBuild = false; // nwmain.exe is the build the unexported offsets came from
    bool objects = false;    // client app and object array look sane
    bool player = false;     // controlled creature resolves through the object array
    bool camera = false;     // module camera passes the vtable-slot check
    bool gui = false;        // GUI manager's panel list is readable
    bool setFacing = false;  // creature orientation setter (known build only)
    bool driveState = false; // drive-control state block (known build only)
};

struct ObjectInfo {
    void* ptr;
    uint32_t oid;
    uint8_t type;
    void* area;
    Vec3 pos;
    Vec3 facing;
};

constexpr uint32_t kInvalidOid = 0x7f000000;

// Resolves exports once, then re-runs the cheap self-tests. Logs whenever the
// result changes (e.g. the player object appears after the module loads).
const Caps& Refresh();
const Caps& Current();
void LogBuildInfo(const char* gameDir);

uint32_t Rva(const void* p); // offset into nwmain.exe, or 0 if outside it
const char* ObjectTypeName(uint8_t type);

bool SafeRead(const void* src, void* dst, size_t n);
template <class T>
bool Read(const void* base, ptrdiff_t off, T* out)
{
    return base && SafeRead(static_cast<const char*>(base) + off, out, sizeof(T));
}

// Objects
void* AppInternal();
uint32_t PlayerOid();
void* ObjectByOid(uint32_t oid);
bool ReadObject(void* obj, ObjectInfo* out);
bool PlayerInfo(ObjectInfo* out);
using ObjectVisitor = void (*)(void* ctx, const ObjectInfo& info);
int ForEachObject(ObjectVisitor visit, void* ctx); // number visited, or -1

// Camera
void* Camera();
bool CameraPosition(Vec3* out);
bool CameraAngles(float* yaw, float* pitch, float* dist, float* viewAngle);
bool WorldToScreen(const Vec3& world, float* nx, float* ny); // 0..1, top-left origin
// Adds to the follow camera's yaw/pitch (degrees) on its next update; the game
// applies its own pitch/zoom limits. Known build only.
bool RotateCamera(float yawDegrees, float pitchDegrees);

// Movement (known build only)
bool SetFacing(void* obj, const Vec3& dir);
bool ForceDriveSend();
bool ReadDriveState(int out[8]); // ints at app+0x1b0 .. app+0x1cc
// Runs a keymap action as if its key went down/up (known build only).
constexpr int kActionDriveForwardWalk = 89; // DriveModeForward: walks
bool InputAction(int action, bool down);
// Ends the in-process server's drive action for this creature at stopAt (the
// client's position for it), instead of letting it walk on to the waypoint it was
// given (~0.4 s ahead). *gap receives the server-to-stopAt distance. Only works when
// the server runs in this process (single player / local host).
enum class DriveStop { Unavailable, NotDriving, Stopped };
DriveStop StopServerDrive(uint32_t oid, const Vec3& stopAt, float* gap);

// Once the server's drive has ended: moves the local server's creature, and the
// client's server-confirmed position, to where the client draws it, if they're less
// than maxGap apart. Returns the gap it closed, or -1 if it did nothing.
float SyncServerToClient(uint32_t oid, float maxGap);

// Targeting (known build only).
// What the player could do with an object, read from the same client fields the
// game's click code checks: the useable flag (creature/placeable/door) or the
// pickable flag (ground item), hostility, and whether it's one of the player's
// associates. Dead creatures count as unusable when the local server can tell.
// False for types that are never targets.
struct TargetInfo {
    bool usable = false;
    bool hostile = false;
    bool associate = false;
};
bool TargetInfoFor(const ObjectInfo& o, uint32_t playerOid, TargetInfo* out);
// Patches the game's world pick (hover, left click, right click, spell targeting) so
// that, while active, it reports `oid` as the object under the pointer, or nothing.
// The game then highlights it, names it and acts on it as if the mouse were there.
bool InstallPickHook();
void SetPickTarget(bool active, uint32_t oid);
bool DetourSelfTest(); // patches a stand-in with the same prologue; for the offline harness

// Diagnostic snapshot of how the client and the local server see the creature.
struct MoveTrace {
    Vec3 client;      // client object position
    Vec3 predictBase; // where the client's drive prediction starts from
    uint16_t animation;
    bool haveServer;
    Vec3 server;
    int serverAction; // current server action type, -1 = none
};
bool ReadMoveTrace(uint32_t oid, MoveTrace* out); // known build only

// Quickbar (known build only). The client's quickbar panel holds all 36 slots, so this
// works on any server: 0-11 the first page, 12-23 the second (Shift), 24-35 the third
// (Ctrl).
int QuickbarPage(); // the page F1-F12 fire now (0-2), or -1 if the panel isn't there
enum QuickSlotType : uint8_t {
    kQuickEmpty = 0,
    kQuickItem = 1,
    kQuickSpell = 2,
    kQuickCommand = 0x12,      // a text command with its own label
    kQuickSpellAbility = 0x2c, // spell-like ability
};
// The slot's contents as the game names them (item, spell, feat, skill, mode...), in
// the game's Windows-1252 with any colour tags. Empty slots give type kQuickEmpty and
// "". False if the panel or slot can't be read.
bool QuickbarSlotName(int index, uint8_t* type, char* buf, size_t size);

// Conversation window (known build only). -1 when no conversation is open, else the
// number of replies on offer. With no real replies the game offers one of its own
// ("[Continue]" or similar), so an open window has at least one.
int DialogReplyCount();
// Reply i's text as the game shows it (Windows-1252, dialogue tags included), and
// whether the game will accept it. False if it can't be read.
bool DialogReply(int i, char* buf, size_t size, bool* enabled);
// Picks reply i, the way clicking it or pressing its number key does.
bool DialogChoose(int i);
// The open conversation's speaker and current line as the window shows them
// (Windows-1252, tags included). The window's wrapped lines are joined with a space
// where the break had none. False if they can't be read.
bool DialogText(char* speaker, size_t speakerSize, char* line, size_t lineSize);
// The resref of the current speaker's portrait at the given size (0-5 = none, T, S, M,
// L, H; the nearest size that exists is used), the way the conversation window picks
// it. False if there's no speaker to ask.
bool DialogPortrait(int size, char* resref, size_t resrefSize);
// Loads a TGA texture by resref through the game's resource manager, as top-down RGBA.
// False if it can't be loaded or isn't 24/32-bit true colour (DDS-only portraits).
bool LoadTextureRgba(const char* resref, std::vector<uint8_t>* rgba, int* width, int* height);
// Moves the game's conversation window out of sight (true) or back where the game had
// it (false). Call it every frame while hiding: it follows the game re-placing the
// window. Returns whether the window is hidden now.
bool HideDialogWindow(bool hide);
// The NPC line's text lines exactly as the window holds them, for the snapshot.
using LineVisitor = void (*)(void* ctx, int index, const char* line);
int ForEachDialogLine(LineVisitor visit, void* ctx); // lines visited, or -1

// GUI
void* GuiMan();
int RadialDepth(); // >0 while a radial menu is open
// Makes the game open radial menus at the screen centre and return to the parent
// radial when a sub-radial is cancelled (known build only); off restores the
// game's own settings.
bool SetRadialCentered(bool on);
// Cancels the open radial: back to its parent, or closed if it's the top one.
bool RadialBack();
// The open radial's 8 slots: bit i of *shown is set when slot i holds an option, of
// *enabled when it can also be chosen; *selected is the slot the pointer is on. The
// game picks the slot from the pointer's angle around the ring centre: slot 0 is
// straight down, then anticlockwise on screen (2 = right, 4 = up, 6 = left), each
// 45 degrees wide. It counts as a choice 56-150 GUI pixels (x GuiScale) out.
bool RadialSlots(uint8_t* shown, uint8_t* enabled, int* selected);
void* KeyboardCapture();
void* MouseCapture();
bool GuiMousePos(int* x, int* y);
float GuiScale();
using PanelVisitor = void (*)(void* ctx, void* panel);
int ForEachPanel(PanelVisitor visit, void* ctx);
bool ModelName(void* model, char* buf, size_t size);
bool ModelScreenPos(void* model, Vec3* out);
void* GuiHitTest(int x, int y); // same call the game makes for the mouse, forced fresh
// Menu panels (known build only). The HUD is the always-on panels (quickbar, compass,
// party bar, chat, name bubbles...) plus the conversation window, which has its own
// mode; anything else shown (inventory, character sheet, a popup) is a menu panel.
bool IsHudPanel(const void* panel);
void* OpenMenuPanel();               // a shown menu panel, or null
void* ControlRoot(void* control);    // the top of its parent chain (the panel it's on)
bool IsFocusable(void* control);     // worth putting focus on: not a panel's backdrop or a label
bool IsTabButton(const void* control); // a panel's tab (CGuiTabButton)
const char* ClassForVtable(const void* obj); // diagnostic naming; nullptr if unknown

} // namespace game
