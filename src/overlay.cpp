#include "overlay.h"

#include <GL/gl.h>

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "log.h"
#include "sdl_api.h"

namespace overlay {
namespace {

// GL 3.x queries; opengl32's headers stop at 1.1.
constexpr GLenum kGlMajorVersion = 0x821B, kGlMinorVersion = 0x821C;
constexpr GLenum kGlContextProfileMask = 0x9126;
constexpr GLint kGlCoreProfileBit = 0x1;
constexpr GLenum kGlClampToEdge = 0x812F, kGlPixelUnpackBuffer = 0x88EC, kGlPixelUnpackBufferBinding = 0x88EF;
using BindBufferFn = void(APIENTRY*)(GLenum target, GLuint buffer);

using SwapFn = void(SDLCALL*)(SDL_Window*);
SwapFn g_realSwap;

bool g_configured, g_enabled, g_failed;
float g_scale = 1.0f;
SDL_GLContext g_context; // the GL context our ImGui backend objects belong to
BindBufferFn g_glBindBuffer; // GL 1.5, from the driver
GLuint g_portraitTex;        // the portrait's texture in g_context (0 = none yet)
std::string g_portraitKey;   // which Image it holds
Uint64 g_lastCounter;
int g_frames;

std::mutex g_hudLock;
Hud g_hud;

// NWN-ish parchment and bronze, kept dark enough to read over any scene.
const ImU32 kBarFill = IM_COL32(24, 18, 12, 200);
const ImU32 kBarEdge = IM_COL32(150, 112, 60, 220);
const ImU32 kText = IM_COL32(236, 224, 200, 255);
const ImU32 kTextDim = IM_COL32(140, 128, 110, 255);
const ImU32 kSelectFill = IM_COL32(150, 112, 60, 110);
const ImU32 kBarEdgeText = IM_COL32(214, 172, 100, 255); // titles, in the edge's bronze

float g_bodyScrollMax; // written by the render thread, read by the input side

ImU32 ButtonColour(const std::string& b)
{
    if (b == "A")
        return IM_COL32(76, 160, 70, 255);
    if (b == "B")
        return IM_COL32(190, 60, 50, 255);
    if (b == "X")
        return IM_COL32(60, 110, 200, 255);
    if (b == "Y")
        return IM_COL32(210, 170, 40, 255);
    return IM_COL32(110, 100, 90, 255); // bumpers, sticks, d-pad
}

// (Re)creates the ImGui context and GL objects for the current GL context. A
// context NWN has thrown away (window recreated) is simply abandoned: deleting its
// objects from the new context could delete the game's.
void Init()
{
    GLint major = 0, minor = 0, profile = 0;
    glGetIntegerv(kGlMajorVersion, &major);
    glGetIntegerv(kGlMinorVersion, &minor);
    if (major * 10 + minor >= 32)
        glGetIntegerv(kGlContextProfileMask, &profile);
    bool core = (profile & kGlCoreProfileBit) != 0;
    const char* glsl = core ? "#version 150" : "#version 130";

    ImGui::SetCurrentContext(ImGui::CreateContext());
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImFontConfig fc;
    fc.OversampleH = 2;
    const char* fonts[] = {"C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\arialbd.ttf"};
    bool loaded = false;
    for (const char* f : fonts)
        if (GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES && io.Fonts->AddFontFromFileTTF(f, 30.0f, &fc)) {
            loaded = true;
            break;
        }
    if (!loaded)
        io.Fonts->AddFontDefault();
    g_glBindBuffer = reinterpret_cast<BindBufferFn>(wglGetProcAddress("glBindBuffer"));
    g_portraitTex = 0; // a texture of the old context isn't ours to use or delete
    g_portraitKey.clear();
    bool ok = ImGui_ImplOpenGL3_Init(glsl);
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    Log("overlay: %s on GL %s (%s profile, %s)", ok ? "ready" : "backend init FAILED", version ? version : "?",
        core ? "core" : "compatibility", glsl);
    if (!ok)
        g_failed = true;
}

// Uploads the image to our texture if it isn't the one there already, leaving the
// game's GL state as it was.
GLuint PortraitTexture(const Image* img)
{
    if (!img || img->width <= 0 || img->height <= 0 ||
        img->rgba.size() < static_cast<size_t>(img->width) * img->height * 4)
        return 0;
    if (g_portraitTex && img->key == g_portraitKey)
        return g_portraitTex;
    GLint prevTex = 0, prevAlign = 4, prevRow = 0, prevPbo = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prevRow);
    glGetIntegerv(kGlPixelUnpackBufferBinding, &prevPbo);
    if (prevPbo && g_glBindBuffer)
        g_glBindBuffer(kGlPixelUnpackBuffer, 0);
    if (!g_portraitTex)
        glGenTextures(1, &g_portraitTex);
    glBindTexture(GL_TEXTURE_2D, g_portraitTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGlClampToEdge);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGlClampToEdge);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img->width, img->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img->rgba.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, prevRow);
    if (prevPbo && g_glBindBuffer)
        g_glBindBuffer(kGlPixelUnpackBuffer, static_cast<GLuint>(prevPbo));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    g_portraitKey = img->key;
    return g_portraitTex;
}

constexpr size_t kPromptsPerRow = 6;

// Width of one [glyph] label pair.
float PromptWidth(const Prompt& p, float s)
{
    ImFont* font = ImGui::GetFont();
    float size = 26.0f * s, r = 15.0f * s;
    float glyph = p.button.size() <= 1 ? 2 * r : font->CalcTextSizeA(size * 0.7f, FLT_MAX, 0, p.button.c_str()).x + r;
    return glyph + 8 * s + font->CalcTextSizeA(size, FLT_MAX, 0, p.label.c_str()).x;
}

// One row of [glyph] label pairs, centred at the given baseline.
void DrawPromptRow(const Prompt* prompts, size_t n, float w, float y0, float s)
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float size = 26.0f * s, pad = 14.0f * s, gap = 22.0f * s, r = 15.0f * s;
    float total = 0;
    for (size_t i = 0; i < n; ++i)
        total += PromptWidth(prompts[i], s);
    total += gap * (n - 1);
    float barH = 2 * r + 2 * pad * 0.6f, x = (w - total) / 2;
    dl->AddRectFilled({x - pad, y0}, {x + total + pad, y0 + barH}, kBarFill, 8 * s);
    dl->AddRect({x - pad, y0}, {x + total + pad, y0 + barH}, kBarEdge, 8 * s, 0, 1.5f * s);
    float cy = y0 + barH / 2;
    for (size_t i = 0; i < n; ++i) {
        const Prompt& p = prompts[i];
        float glyphW;
        if (p.button.size() <= 1) {
            glyphW = 2 * r;
            dl->AddCircleFilled({x + r, cy}, r, ButtonColour(p.button));
            ImVec2 t = font->CalcTextSizeA(size * 0.8f, FLT_MAX, 0, p.button.c_str());
            dl->AddText(font, size * 0.8f, {x + r - t.x / 2, cy - t.y / 2}, IM_COL32_WHITE, p.button.c_str());
        } else { // LB, R3, D-pad...: a rounded pill
            ImVec2 t = font->CalcTextSizeA(size * 0.7f, FLT_MAX, 0, p.button.c_str());
            glyphW = t.x + r;
            dl->AddRectFilled({x, cy - r}, {x + glyphW, cy + r}, ButtonColour(p.button), r);
            dl->AddText(font, size * 0.7f, {x + r / 2, cy - t.y / 2}, IM_COL32_WHITE, p.button.c_str());
        }
        x += glyphW + 8 * s;
        ImVec2 t = font->CalcTextSizeA(size, FLT_MAX, 0, p.label.c_str());
        dl->AddText(font, size, {x, cy - t.y / 2}, kText, p.label.c_str());
        x += t.x + gap;
    }
}

// Bottom-centre prompt rows, with the badge on top. A row takes at most kPromptsPerRow
// prompts and wraps early when the next one wouldn't fit the screen. Returns the top
// of what it drew.
float DrawPromptBar(const Hud& hud, float w, float h, float s)
{
    if (hud.prompts.empty() && hud.badge.empty())
        return h - 36.0f * s;
    float pad = 14.0f * s, gap = 22.0f * s, maxW = w - 2 * (pad + 24.0f * s);
    std::vector<size_t> starts; // first prompt of each row
    for (size_t i = 0; i < hud.prompts.size();) {
        starts.push_back(i);
        float rowW = PromptWidth(hud.prompts[i++], s);
        for (size_t k = 1; i < hud.prompts.size() && k < kPromptsPerRow; ++k, ++i) {
            float next = PromptWidth(hud.prompts[i], s);
            if (rowW + gap + next > maxW)
                break;
            rowW += gap + next;
        }
    }
    float rowH = 2 * 15.0f * s + 2 * pad * 0.6f, rowGap = 8 * s;
    size_t rows = starts.size();
    float top = h - 36.0f * s - rows * rowH - (rows ? (rows - 1) * rowGap : 0);
    for (size_t r = 0; r < rows; ++r) {
        size_t end = r + 1 < rows ? starts[r + 1] : hud.prompts.size();
        DrawPromptRow(&hud.prompts[starts[r]], end - starts[r], w, top + r * (rowH + rowGap), s);
    }
    if (!hud.badge.empty()) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImFont* font = ImGui::GetFont();
        float size = 26.0f * s * 0.8f;
        ImVec2 t = font->CalcTextSizeA(size, FLT_MAX, 0, hud.badge.c_str());
        float bx = (w - t.x) / 2, by = (rows ? top : h - 36.0f * s) - t.y - 14 * s;
        dl->AddRectFilled({bx - pad, by - 4 * s}, {bx + t.x + pad, by + t.y + 4 * s}, kBarFill, 6 * s);
        dl->AddText(font, size, {bx, by}, kText, hud.badge.c_str());
        return by - 4 * s;
    }
    return top;
}

// The focus ring: a bronze outline with a soft glow, just outside the control.
void DrawRing(const Hud& hud, float s)
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float g = 3.0f * s;
    ImVec2 p0{hud.ringX0 - g, hud.ringY0 - g}, p1{hud.ringX1 + g, hud.ringY1 + g};
    dl->AddRect({p0.x - 2 * s, p0.y - 2 * s}, {p1.x + 2 * s, p1.y + 2 * s}, IM_COL32(255, 200, 110, 70), 6 * s, 0,
                5.0f * s);
    dl->AddRect(p0, p1, IM_COL32(255, 210, 120, 255), 5 * s, 0, 2.5f * s);
}

// Small arrows on a box's top and bottom edge where more is hidden.
void DrawMoreArrows(float cx, float y0, float y1, bool up, bool down, float s)
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float r = 7.0f * s;
    if (up)
        dl->AddTriangleFilled({cx - r, y0 + 2 * s + r}, {cx + r, y0 + 2 * s + r}, {cx, y0 + 2 * s}, kBarEdge);
    if (down)
        dl->AddTriangleFilled({cx - r, y1 - 2 * s - r}, {cx + r, y1 - 2 * s - r}, {cx, y1 - 2 * s}, kBarEdge);
}

float ListBoxWidth(float w, float s)
{
    return w * 0.6f < 1150.0f * s ? w * 0.6f : 1150.0f * s;
}

// The text box: a portrait on the left if there is one, then a title line and wrapped
// body, its bottom at `bottom`, no taller than maxH; a longer body scrolls by
// hud.bodyScroll. The box is at least as tall as the portrait.
void DrawBody(const Hud& hud, float w, float bottom, float maxH, float s)
{
    g_bodyScrollMax = 0;
    if (hud.body.empty() && hud.title.empty())
        return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float size = 27.0f * s, titleSize = 24.0f * s, pad = 16.0f * s, gap = 8.0f * s;
    float boxW = ListBoxWidth(w, s);

    GLuint tex = PortraitTexture(hud.portrait.get());
    float picH = tex ? 180.0f * s : 0, picW = 0;
    if (picH > maxH - 2 * pad)
        picH = maxH - 2 * pad;
    if (tex && picH > 40.0f * s)
        picW = picH * hud.portrait->width / hud.portrait->height;
    else
        picH = 0;
    float textX = pad + (picW > 0 ? picW + pad : 0), textW = boxW - textX - pad;

    float titleH = hud.title.empty() ? 0 : font->CalcTextSizeA(titleSize, FLT_MAX, 0, hud.title.c_str()).y + gap;
    float bodyH = font->CalcTextSizeA(size, FLT_MAX, textW, hud.body.c_str()).y;
    float viewH = bodyH;
    if (titleH + viewH + 2 * pad > maxH)
        viewH = maxH - titleH - 2 * pad;
    if (viewH < size)
        return; // no room at all
    float scrollMax = bodyH - viewH, scroll = hud.bodyScroll * s;
    scroll = scroll < 0 ? 0 : scroll > scrollMax ? scrollMax : scroll;
    g_bodyScrollMax = scrollMax / s;

    float contentH = titleH + viewH > picH ? titleH + viewH : picH;
    float x0 = (w - boxW) / 2, y1 = bottom, y0 = y1 - contentH - 2 * pad;
    dl->AddRectFilled({x0, y0}, {x0 + boxW, y1}, kBarFill, 8 * s);
    dl->AddRect({x0, y0}, {x0 + boxW, y1}, kBarEdge, 8 * s, 0, 1.5f * s);
    if (picW > 0) {
        ImVec2 p0{x0 + pad, y0 + pad}, p1{x0 + pad + picW, y0 + pad + picH};
        dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), p0, p1);
        dl->AddRect(p0, p1, kBarEdge, 0, 0, 1.5f * s);
    }
    if (titleH > 0)
        dl->AddText(font, titleSize, {x0 + textX, y0 + pad}, kBarEdgeText, hud.title.c_str());
    float ty = y0 + pad + titleH;
    dl->PushClipRect({x0 + textX, ty}, {x0 + boxW - pad, ty + viewH}, true);
    dl->AddText(font, size, {x0 + textX, ty - scroll}, kText, hud.body.c_str(), nullptr, textW);
    dl->PopClipRect();
    DrawMoreArrows(x0 + textX + textW / 2, ty - pad, y1, scroll > 0.5f, scroll < scrollMax - 0.5f, s);
}

// The choice list: a numbered, wrapped column centred above `bottom`, scrolled so the
// selected item shows, with small arrows where more items are hidden. Returns its top.
float DrawList(const Hud& hud, float w, float bottom, float maxH, float s)
{
    if (hud.list.empty())
        return bottom;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float size = 24.0f * s, pad = 14.0f * s, itemPad = 7.0f * s, numW = 38.0f * s;
    float boxW = ListBoxWidth(w, s), textW = boxW - 2 * pad - numW;
    int n = static_cast<int>(hud.list.size());
    std::vector<float> heights(n);
    for (int i = 0; i < n; ++i)
        heights[i] = font->CalcTextSizeA(size, FLT_MAX, textW, hud.list[i].text.c_str()).y + 2 * itemPad;

    // Grow a window around the selection until the next item wouldn't fit.
    maxH -= 2 * pad;
    int sel = hud.listSelected >= 0 && hud.listSelected < n ? hud.listSelected : 0;
    int first = sel, last = sel;
    float used = heights[sel];
    for (bool grew = true; grew;) {
        grew = false;
        if (last + 1 < n && used + heights[last + 1] <= maxH) {
            used += heights[++last];
            grew = true;
        }
        if (first > 0 && used + heights[first - 1] <= maxH) {
            used += heights[--first];
            grew = true;
        }
    }

    float x0 = (w - boxW) / 2, y1 = bottom, y0 = y1 - used - 2 * pad;
    dl->AddRectFilled({x0, y0}, {x0 + boxW, y1}, kBarFill, 8 * s);
    dl->AddRect({x0, y0}, {x0 + boxW, y1}, kBarEdge, 8 * s, 0, 1.5f * s);
    float y = y0 + pad;
    for (int i = first; i <= last; ++i) {
        const ListItem& it = hud.list[i];
        if (i == sel) {
            dl->AddRectFilled({x0 + pad * 0.5f, y}, {x0 + boxW - pad * 0.5f, y + heights[i]}, kSelectFill, 5 * s);
            dl->AddRectFilled({x0 + pad * 0.5f, y}, {x0 + pad * 0.5f + 4 * s, y + heights[i]}, kBarEdge, 2 * s);
        }
        ImU32 col = it.enabled ? kText : kTextDim;
        char num[8];
        snprintf(num, sizeof(num), "%d.", i + 1);
        dl->AddText(font, size, {x0 + pad + 6 * s, y + itemPad}, col, num);
        dl->AddText(font, size, {x0 + pad + numW, y + itemPad}, col, it.text.c_str(), nullptr, textW);
        y += heights[i];
    }
    DrawMoreArrows(w / 2, y0, y1, first > 0, last < n - 1, s);
    return y0;
}

void Frame(SDL_Window* window)
{
    SDL_GLContext ctx = sdl.SDL_GL_GetCurrentContext();
    if (!ctx)
        return;
    if (ctx != g_context) {
        if (g_context)
            Log("overlay: GL context changed (window recreated); rebuilding");
        g_context = ctx;
        Init();
        if (g_failed)
            return;
    }
    int ww = 0, wh = 0, dw = 0, dh = 0;
    sdl.SDL_GetWindowSize(window, &ww, &wh);
    sdl.SDL_GL_GetDrawableSize(window, &dw, &dh);
    if (ww <= 0 || wh <= 0)
        return;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(ww), static_cast<float>(wh));
    io.DisplayFramebufferScale = ImVec2(static_cast<float>(dw) / ww, static_cast<float>(dh) / wh);
    Uint64 now = sdl.SDL_GetPerformanceCounter();
    float dt = g_lastCounter ? static_cast<float>(double(now - g_lastCounter) / sdl.SDL_GetPerformanceFrequency()) : 0;
    g_lastCounter = now;
    io.DeltaTime = dt > 0 && dt < 1 ? dt : 1.0f / 60.0f;

    Hud hud;
    {
        std::lock_guard<std::mutex> lock(g_hudLock);
        hud = g_hud;
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    float scale = g_scale * io.DisplaySize.y / 1080.0f;
    float w = io.DisplaySize.x, h = io.DisplaySize.y;
    if (hud.ring)
        DrawRing(hud, scale);
    float barTop = DrawPromptBar(hud, w, h, scale);
    // Above the bar: the list, then the text box, sharing what's left below a top margin.
    float room = barTop - 12.0f * scale - 0.08f * h;
    float listTop = DrawList(hud, w, barTop - 12.0f * scale, room * (hud.body.empty() ? 1.0f : 0.5f), scale);
    float bodyRoom = listTop - 10.0f * scale - 0.08f * h;
    DrawBody(hud, w, listTop - 10.0f * scale, bodyRoom < 0.4f * h ? bodyRoom : 0.4f * h, scale);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    ++g_frames;
}

bool GuardedFrame(SDL_Window* window)
{
    __try {
        Frame(window);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SDLCALL HookedSwapWindow(SDL_Window* window)
{
    if (g_configured && g_enabled && !g_failed && window && !GuardedFrame(window)) {
        g_failed = true;
        Log("overlay: fault while drawing; overlay off for this session");
    }
    g_realSwap(window);
}

} // namespace

bool Install(HMODULE exe, void* table, unsigned tableSize)
{
    void** slot = FindDynApiSlot(exe, "SDL_GL_SwapWindow", table, tableSize);
    if (!slot) {
        Log("overlay: couldn't locate SDL_GL_SwapWindow in the jump table; no overlay");
        return false;
    }
    g_realSwap = reinterpret_cast<SwapFn>(*slot);
    *slot = reinterpret_cast<void*>(&HookedSwapWindow);
    Log("overlay: hooked SDL_GL_SwapWindow");
    return true;
}

void Configure(bool enabled, float scale)
{
    g_enabled = enabled;
    g_scale = scale > 0.25f && scale < 4.0f ? scale : 1.0f;
    g_configured = true;
}

void SetHud(const Hud& hud)
{
    std::lock_guard<std::mutex> lock(g_hudLock);
    g_hud = hud;
}

float BodyScrollMax()
{
    return g_bodyScrollMax;
}

int FramesDrawn()
{
    return g_frames;
}

bool Failed()
{
    return g_failed;
}

} // namespace overlay
