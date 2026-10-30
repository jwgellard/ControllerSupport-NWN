// Focus navigation over the game's own menu panels (docs/DESIGN.md §8.1): find a
// panel's controls and their screen rectangles by hit-testing a grid of points, then
// move focus between them with the d-pad. Pure logic over a hit-test callback, so the
// offline harness can check it.
#pragma once

#include <vector>

namespace panelnav {

struct Rect {
    float x0, y0, x1, y1; // screen pixels, y down
    float CenterX() const { return (x0 + x1) / 2; }
    float CenterY() const { return (y0 + y1) / 2; }
};
struct Control {
    void* obj;
    int cell; // -1, or which cell of a grid control (row-major)
    Rect r;
};

// The focusable control at screen (x, y), or null; `ctx` is the caller's.
using HitFn = void* (*)(void* ctx, int x, int y);
// Columns and rows if `obj` is a grid of cells (an inventory box), else false.
using GridFn = bool (*)(void* ctx, void* obj, int* cols, int* rows);

// Hit-tests a w x h screen in a grid of `step` pixels, a few rows at a time.
class Scanner {
public:
    void Start(int w, int h, int step);
    // Tests up to `budget` points; true once the whole screen is done.
    bool Run(HitFn hit, void* ctx, int budget);
    bool Running() const { return running_; }
    // Each control's box (its sample points grown by half a step), grid controls split
    // into their cells, in reading order.
    std::vector<Control> Result(GridFn grid, void* ctx) const;

private:
    struct Box {
        void* obj;
        int x0, y0, x1, y1;
    };
    std::vector<Box> boxes_;
    int w_ = 0, h_ = 0, step_ = 1, next_ = 0;
    bool running_ = false;
};

// From controls[from], the nearest control ahead in direction (dx, dy) (screen, y down):
// for a straight move, one in line with it if there is any (extents overlapping across
// the move), else the best by distance and angle; -1 if nothing is ahead.
int Step(const std::vector<Control>& cs, int from, float dx, float dy);
// The control containing (x, y), else the one whose centre is nearest; -1 if none.
int Nearest(const std::vector<Control>& cs, float x, float y);
// The control with this identity, or -1.
int Find(const std::vector<Control>& cs, void* obj, int cell);

} // namespace panelnav
