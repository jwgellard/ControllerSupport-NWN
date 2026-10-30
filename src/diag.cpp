#include "diag.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "game.h"
#include "log.h"

namespace {

constexpr float kRadToDeg = 57.2957795f;

const char* ClassOf(const void* obj)
{
    const char* c = game::ClassForVtable(obj);
    return c ? c : "?";
}

std::string Describe(void* obj)
{
    if (!obj)
        return "null";
    char buf[160], name[64] = "";
    void* model = nullptr;
    if (game::Read(obj, 0x10, &model))
        game::ModelName(model, name, sizeof(name));
    void* vt = nullptr;
    game::Read(obj, 0, &vt);
    snprintf(buf, sizeof(buf), "%p %s vt+%x model '%s'", obj, ClassOf(obj), game::Rva(vt), name);
    return buf;
}

void LogWorld()
{
    game::ObjectInfo pl;
    bool havePlayer = game::PlayerInfo(&pl);
    if (havePlayer) {
        Log("snap: player oid=%08x pos=(%.2f, %.2f, %.2f) facing=(%.3f, %.3f, %.3f) = %.1f deg area=%p", pl.oid,
            pl.pos.x, pl.pos.y, pl.pos.z, pl.facing.x, pl.facing.y, pl.facing.z,
            atan2f(pl.facing.y, pl.facing.x) * kRadToDeg, pl.area);
        int drive[8];
        if (game::ReadDriveState(drive))
            Log("snap: drive state force=%d flags=[%d %d %d %d %d %d %d]", drive[0], drive[1], drive[2], drive[3],
                drive[4], drive[5], drive[6], drive[7]);
    } else {
        Log("snap: no player creature (caps.player=%d, oid %08x)", game::Current().player, game::PlayerOid());
    }

    game::Vec3 cam;
    float yaw = 0, pitch = 0, dist = 0, fov = 0;
    if (game::CameraPosition(&cam) && game::CameraAngles(&yaw, &pitch, &dist, &fov)) {
        Log("snap: camera %p pos=(%.2f, %.2f, %.2f) yaw=%.4f pitch=%.4f dist=%.2f viewAngle=%.2f", game::Camera(),
            cam.x, cam.y, cam.z, yaw, pitch, dist, fov);
        float sx, sy;
        if (havePlayer) {
            float bx = pl.pos.x - cam.x, by = pl.pos.y - cam.y;
            Log("snap: camera->player bearing %.1f deg, ground distance %.2f", atan2f(by, bx) * kRadToDeg,
                sqrtf(bx * bx + by * by));
            if (game::WorldToScreen(pl.pos, &sx, &sy))
                Log("snap: player feet project to screen (%.3f, %.3f)", sx, sy);
        }
    } else {
        Log("snap: camera unavailable (caps.camera=%d, ptr %p)", game::Current().camera, game::Camera());
    }
    if (!havePlayer) {
        int n = game::ForEachObject([](void*, const game::ObjectInfo&) {}, nullptr);
        Log("snap: %d client objects", n);
        return;
    }

    struct Ctx {
        game::ObjectInfo pl;
        std::vector<std::pair<float, game::ObjectInfo>> list;
        int total = 0;
    } ctx{pl, {}, 0};
    game::ForEachObject(
        [](void* p, const game::ObjectInfo& o) {
            auto* c = static_cast<Ctx*>(p);
            ++c->total;
            if (o.area != c->pl.area || o.oid == c->pl.oid)
                return;
            float dx = o.pos.x - c->pl.pos.x, dy = o.pos.y - c->pl.pos.y;
            c->list.push_back({sqrtf(dx * dx + dy * dy), o});
        },
        &ctx);
    std::sort(ctx.list.begin(), ctx.list.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    Log("snap: %d client objects, %d in the player's area", ctx.total, static_cast<int>(ctx.list.size()));
    for (size_t i = 0; i < ctx.list.size() && i < 40; ++i) {
        const auto& o = ctx.list[i].second;
        float sx = -1, sy = -1;
        game::Vec3 aim = o.pos;
        aim.z += o.type == game::kTypeCreature ? 1.0f : 0.5f;
        game::WorldToScreen(aim, &sx, &sy);
        Log("snap:   %-9s oid=%08x dist=%6.2f pos=(%.2f, %.2f, %.2f) screen=(%.3f, %.3f) %s",
            game::ObjectTypeName(o.type), o.oid, ctx.list[i].first, o.pos.x, o.pos.y, o.pos.z, sx, sy,
            ClassOf(o.ptr));
    }
}

void LogPanels()
{
    Log("snap: radialDepth=%d keyboardCapture=[%s] mouseCapture=[%s]", game::RadialDepth(),
        Describe(game::KeyboardCapture()).c_str(), Describe(game::MouseCapture()).c_str());
    game::ForEachPanel(
        [](void*, void* panel) {
            game::Vec3 pos{-1, -1, 0};
            void* model = nullptr;
            game::Read(panel, 0x10, &model);
            game::ModelScreenPos(model, &pos);
            char* tags = nullptr;
            uint32_t tagCount = 0;
            game::Read(panel, 0xe0, &tags);
            game::Read(panel, 0xe8, &tagCount);
            Log("snap: panel [%s] at gui (%.1f, %.1f), %u tag slots", Describe(panel).c_str(), pos.x, pos.y,
                tagCount);
            for (uint32_t i = 0; tags && i < tagCount && i < 400; ++i) {
                uint64_t q[6];
                if (!game::SafeRead(tags + i * 0x30, q, sizeof(q)))
                    break;
                char name[64] = "";
                for (size_t k = 0; k + 1 < sizeof(name); ++k) {
                    char ch = 0;
                    if (!q[1] || !game::SafeRead(reinterpret_cast<const char*>(q[1]) + k, &ch, 1) || !ch)
                        break;
                    name[k] = ch;
                    name[k + 1] = 0;
                }
                if (!name[0])
                    continue;
                std::string refs;
                for (int k : {0, 3, 4, 5}) {
                    char buf[96];
                    void* vt = nullptr;
                    const char* cls = nullptr;
                    if (q[k] && game::Read(reinterpret_cast<void*>(q[k]), 0, &vt) && game::Rva(vt))
                        cls = game::ClassForVtable(reinterpret_cast<void*>(q[k]));
                    snprintf(buf, sizeof(buf), " q%d=%llx%s%s", k, static_cast<unsigned long long>(q[k]),
                             game::Rva(vt) ? (cls ? ":" : ":vt") : "", cls ? cls : "");
                    refs += buf;
                }
                Log("snap:     tag %3u '%s' len=%u%s", i, name, static_cast<unsigned>(q[2]), refs.c_str());
            }
        },
        nullptr);
}

void LogHitScan(SDL_Window* window)
{
    int w = 0, h = 0;
    sdl.SDL_GetWindowSize(window, &w, &h);
    if (w <= 0 || h <= 0)
        return;
    struct Box {
        void* obj;
        int x0, y0, x1, y1, hits;
    };
    std::vector<Box> boxes;
    int step = std::max(12, h / 45);
    for (int y = step / 2; y < h; y += step) {
        for (int x = step / 2; x < w; x += step) {
            void* hit = game::GuiHitTest(x, y);
            if (!hit)
                continue;
            auto it = std::find_if(boxes.begin(), boxes.end(), [&](const Box& b) { return b.obj == hit; });
            if (it == boxes.end()) {
                boxes.push_back({hit, x, y, x, y, 1});
            } else {
                it->x0 = std::min(it->x0, x);
                it->y0 = std::min(it->y0, y);
                it->x1 = std::max(it->x1, x);
                it->y1 = std::max(it->y1, y);
                ++it->hits;
            }
        }
    }
    int mx = 0, my = 0;
    if (game::GuiMousePos(&mx, &my))
        game::GuiHitTest(mx, my); // leave the game's hover cache as it was
    Log("snap: GUI hit scan %dx%d step %d: %d distinct controls", w, h, step, static_cast<int>(boxes.size()));
    for (const Box& b : boxes) {
        void* parent = nullptr;
        game::Read(b.obj, 0x68, &parent);
        Log("snap:   [%d..%d]x[%d..%d] n=%d %s parent=[%s]", b.x0, b.x1, b.y0, b.y1, b.hits, Describe(b.obj).c_str(),
            Describe(parent).c_str());
    }
}

// Every quickbar slot, as the layer prompts would name it.
void LogQuickbar()
{
    int page = game::QuickbarPage();
    if (page < 0) {
        Log("snap: quickbar panel not found");
        return;
    }
    Log("snap: quickbar shows page %d", page);
    for (int i = 0; i < 36; ++i) {
        uint8_t type = 0;
        char name[96];
        if (!game::QuickbarSlotName(i, &type, name, sizeof(name)))
            Log("snap:   slot %2d unreadable", i);
        else if (type != game::kQuickEmpty)
            Log("snap:   slot %2d (%s F%d) type %u '%s'", i, i < 12 ? "" : i < 24 ? "Shift" : "Ctrl", i % 12 + 1,
                type, name);
    }
}

// The open conversation's replies, as the game holds them.
void LogDialog()
{
    int n = game::DialogReplyCount();
    if (n < 0) {
        Log("snap: no conversation open");
        return;
    }
    Log("snap: conversation open, %d replies", n);
    int lines = game::ForEachDialogLine(
        [](void*, int i, const char* line) { Log("snap:   npc line %d '%s'", i, line); }, nullptr);
    if (lines < 0)
        Log("snap:   npc text unreadable");
    for (int i = 0; i < n; ++i) {
        char text[512];
        bool enabled = false;
        if (game::DialogReply(i, text, sizeof(text), &enabled))
            Log("snap:   reply %d%s '%s'", i + 1, enabled ? "" : " (disabled)", text);
        else
            Log("snap:   reply %d unreadable", i + 1);
    }
}

} // namespace

void DiagSnapshot(SDL_Window* window)
{
    Log("=== snapshot ===");
    const game::Caps& caps = game::Refresh();
    int w = 0, h = 0, dw = 0, dh = 0, mx = 0, my = 0, gx = 0, gy = 0;
    sdl.SDL_GetWindowSize(window, &w, &h);
    sdl.SDL_GL_GetDrawableSize(window, &dw, &dh);
    sdl.SDL_GetMouseState(&mx, &my);
    game::GuiMousePos(&gx, &gy);
    Log("snap: window %dx%d drawable %dx%d guiScale=%.3f mouse sdl=(%d, %d) gui=(%d, %d)", w, h, dw, dh,
        game::GuiScale(), mx, my, gx, gy);
    if (!caps.exports)
        return;
    LogWorld();
    LogQuickbar();
    LogDialog();
    LogPanels();
    LogHitScan(window);
    Log("=== end snapshot ===");
}
