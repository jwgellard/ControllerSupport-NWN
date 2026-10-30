#include "panelnav.h"

#include <algorithm>
#include <cmath>

namespace panelnav {

void Scanner::Start(int w, int h, int step)
{
    boxes_.clear();
    w_ = w;
    h_ = h;
    step_ = step > 0 ? step : 1;
    next_ = 0;
    running_ = w > 0 && h > 0;
}

bool Scanner::Run(HitFn hit, void* ctx, int budget)
{
    int cols = (w_ + step_ - 1) / step_, rows = (h_ + step_ - 1) / step_;
    for (; running_ && budget > 0; --budget, ++next_) {
        if (next_ >= cols * rows) {
            running_ = false;
            break;
        }
        int x = (next_ % cols) * step_ + step_ / 2, y = (next_ / cols) * step_ + step_ / 2;
        void* obj = hit(ctx, x, y);
        if (!obj)
            continue;
        auto it = std::find_if(boxes_.begin(), boxes_.end(), [&](const Box& b) { return b.obj == obj; });
        if (it == boxes_.end()) {
            boxes_.push_back({obj, x, y, x, y});
        } else {
            it->x0 = std::min(it->x0, x);
            it->y0 = std::min(it->y0, y);
            it->x1 = std::max(it->x1, x);
            it->y1 = std::max(it->y1, y);
        }
    }
    return !running_;
}

std::vector<Control> Scanner::Result(GridFn grid, void* ctx) const
{
    std::vector<Control> out;
    float half = step_ / 2.0f;
    for (const Box& b : boxes_) {
        Rect r{b.x0 - half, b.y0 - half, b.x1 + half, b.y1 + half};
        int cols = 0, rows = 0;
        if (grid && grid(ctx, b.obj, &cols, &rows) && cols > 0 && rows > 0) {
            float cw = (r.x1 - r.x0) / cols, ch = (r.y1 - r.y0) / rows;
            for (int row = 0; row < rows; ++row)
                for (int col = 0; col < cols; ++col)
                    out.push_back({b.obj, row * cols + col,
                                   {r.x0 + col * cw, r.y0 + row * ch, r.x0 + (col + 1) * cw, r.y0 + (row + 1) * ch}});
        } else {
            out.push_back({b.obj, -1, r});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Control& a, const Control& b) {
        float ay = a.r.CenterY(), by = b.r.CenterY();
        return fabsf(ay - by) > 4 ? ay < by : a.r.CenterX() < b.r.CenterX();
    });
    return out;
}

int Step(const std::vector<Control>& cs, int from, float dx, float dy)
{
    if (from < 0 || from >= static_cast<int>(cs.size()))
        return -1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len <= 0)
        return -1;
    dx /= len;
    dy /= len;
    const Rect& f = cs[from].r;
    float fx = f.CenterX(), fy = f.CenterY();
    // A straight move prefers controls in line with this one (their extents overlap
    // across the move), however far: a Load button across the row beats a list row up
    // and to the side. Only when nothing lines up does the angle decide.
    bool horizontal = fabsf(dy) < 0.01f, vertical = fabsf(dx) < 0.01f;
    int best = -1, bestInLine = -1;
    float bestScore = 0, bestInLineScore = 0;
    for (int i = 0; i < static_cast<int>(cs.size()); ++i) {
        if (i == from)
            continue;
        const Rect& r = cs[i].r;
        if (r.x0 <= f.x0 && r.x1 >= f.x1 && r.y0 <= f.y0 && r.y1 >= f.y1)
            continue; // a backdrop around this one (a list box behind its rows)
        float ox = r.CenterX() - fx, oy = r.CenterY() - fy;
        float ahead = ox * dx + oy * dy, side = fabsf(ox * dy - oy * dx);
        if (ahead < 4) // behind or level with it
            continue;
        bool inLine = (horizontal && r.y0 < f.y1 && r.y1 > f.y0) || (vertical && r.x0 < f.x1 && r.x1 > f.x0);
        if (inLine) {
            float score = ahead + 0.5f * side;
            if (bestInLine < 0 || score < bestInLineScore) {
                bestInLine = i;
                bestInLineScore = score;
            }
        }
        if (side > ahead * 2.5f) // too far off to the side
            continue;
        float score = ahead + 2.5f * side;
        if (best < 0 || score < bestScore) {
            best = i;
            bestScore = score;
        }
    }
    return bestInLine >= 0 ? bestInLine : best;
}

int Nearest(const std::vector<Control>& cs, float x, float y)
{
    int best = -1;
    float bestD = 0;
    for (int i = 0; i < static_cast<int>(cs.size()); ++i) {
        const Rect& r = cs[i].r;
        if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1)
            return i;
        float ox = r.CenterX() - x, oy = r.CenterY() - y, d = ox * ox + oy * oy;
        if (best < 0 || d < bestD) {
            best = i;
            bestD = d;
        }
    }
    return best;
}

int Find(const std::vector<Control>& cs, void* obj, int cell)
{
    for (int i = 0; i < static_cast<int>(cs.size()); ++i)
        if (cs[i].obj == obj && cs[i].cell == cell)
            return i;
    return -1;
}

} // namespace panelnav
