// The CPU rasterizer under SVG, gradients and masks: polygons filled by scanline with
// eight sub-rows per pixel and exact coverage along each, then composited source over.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "xweb/paint.h"

namespace fbx::xweb {

namespace {

constexpr int kSubRows = 8;
constexpr float kPi = 3.14159265358979f;
constexpr size_t kMaxEdges = 2'000'000;   // one fill's polygons, past which the rest are dropped

bool finite(Point p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::fabs(p.x) < 1e7f && std::fabs(p.y) < 1e7f; }

struct Premul {
    float r = 0, g = 0, b = 0, a = 0;
};

Premul premul(Color c, float opacity) {
    const float a = static_cast<float>(c.a) / 255.0f * opacity;
    return {static_cast<float>(c.r) / 255.0f * a, static_cast<float>(c.g) / 255.0f * a, static_cast<float>(c.b) / 255.0f * a, a};
}

// A coordinate as an int, whatever it was: NaN and the huge are clamped first.
int toInt(float v) {
    if (!(v > -1e8f)) return -100'000'000;
    if (!(v < 1e8f)) return 100'000'000;
    return static_cast<int>(v);
}

uint8_t toByte(float v) { return static_cast<uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f)); }

}  // namespace

// ── Matrices ─────────────────────────────────────────────────────────────────

Matrix Matrix::after(const Matrix& m) const noexcept {
    return {a * m.a + c * m.b, b * m.a + d * m.b, a * m.c + c * m.d, b * m.c + d * m.d, a * m.e + c * m.f + e, b * m.e + d * m.f + f};
}

std::optional<Matrix> Matrix::inverse() const noexcept {
    const float det = a * d - b * c;
    if (!std::isfinite(det) || std::fabs(det) < 1e-12f) return std::nullopt;
    const float inv = 1.0f / det;
    Matrix r;
    r.a = d * inv;
    r.b = -b * inv;
    r.c = -c * inv;
    r.d = a * inv;
    r.e = (c * f - d * e) * inv;
    r.f = (b * e - a * f) * inv;
    return r;
}

float Matrix::scale() const noexcept {
    const float s = std::sqrt(std::fabs(a * d - b * c));
    return std::isfinite(s) ? s : 1.0f;
}

// ── Gradients ────────────────────────────────────────────────────────────────

Color GradientPaint::at(Point p) const noexcept {
    if (stops.empty()) return {0, 0, 0, 0};
    float t = 0;
    if (kind == Kind::Linear) {
        const float dx = x2 - x1, dy = y2 - y1;
        const float len2 = dx * dx + dy * dy;
        if (len2 <= 1e-12f) return stops.back().color;
        t = ((p.x - x1) * dx + (p.y - y1) * dy) / len2;
    } else if (kind == Kind::Radial) {
        // Two-point conical, as SVG's focus makes it: p is on the circle centred at
        // f + t (c - f) with radius t r.
        const float cdx = cx - fx, cdy = cy - fy, pdx = p.x - fx, pdy = p.y - fy;
        const float qa = cdx * cdx + cdy * cdy - r * r;
        const float qb = pdx * cdx + pdy * cdy;
        const float qc = pdx * pdx + pdy * pdy;
        if (std::fabs(qa) < 1e-9f) {
            if (std::fabs(qb) < 1e-12f) return stops.back().color;
            t = qc / (2 * qb);
        } else {
            const float disc = qb * qb - qa * qc;
            if (disc < 0) return {0, 0, 0, 0};
            const float root = std::sqrt(disc);
            const float t1 = (qb + root) / qa, t2 = (qb - root) / qa;
            t = std::max(t1, t2);
            if (t * r < 0) t = std::min(t1, t2);
        }
    } else {
        float angle = std::atan2(p.x - cx, -(p.y - cy)) - start_angle;
        t = angle / (2 * kPi);
        t -= std::floor(t);
    }
    if (!std::isfinite(t)) return stops.back().color;
    if (spread != Spread::Pad) {
        const float period = period_end - period_start;
        if (period <= 1e-6f) return stops.back().color;
        float u = (t - period_start) / period;
        if (spread == Spread::Repeat) {
            u -= std::floor(u);
        } else {
            u = std::fmod(std::fabs(u), 2.0f);
            if (u > 1) u = 2 - u;
        }
        t = period_start + u * period;
    }
    if (t <= stops.front().offset) return stops.front().color;
    if (t >= stops.back().offset) return stops.back().color;
    size_t i = 1;
    while (i < stops.size() && stops[i].offset < t) ++i;
    if (i >= stops.size()) return stops.back().color;
    const ColorStop& s0 = stops[i - 1];
    const ColorStop& s1 = stops[i];
    const float span = s1.offset - s0.offset;
    const float f = span <= 1e-9f ? 1.0f : (t - s0.offset) / span;
    // In premultiplied colour, as CSS and SVG do it.
    const Premul a = premul(s0.color, 1), b = premul(s1.color, 1);
    const float ra = a.r + (b.r - a.r) * f, ga = a.g + (b.g - a.g) * f, ba = a.b + (b.b - a.b) * f, aa = a.a + (b.a - a.a) * f;
    if (aa <= 0) return {0, 0, 0, 0};
    return {toByte(ra / aa), toByte(ga / aa), toByte(ba / aa), toByte(aa)};
}

// ── The canvas ───────────────────────────────────────────────────────────────

Canvas::Canvas(uint32_t width, uint32_t height)
    : width_(width), height_(height), px_(static_cast<size_t>(width) * height * 4, 0.0f) {
    resetClip();
}

void Canvas::setClip(float x, float y, float w, float h) {
    clip_x0_ = std::clamp(toInt(std::floor(x)), 0, static_cast<int>(width_));
    clip_y0_ = std::clamp(toInt(std::floor(y)), 0, static_cast<int>(height_));
    clip_x1_ = std::clamp(toInt(std::ceil(x + w)), clip_x0_, static_cast<int>(width_));
    clip_y1_ = std::clamp(toInt(std::ceil(y + h)), clip_y0_, static_cast<int>(height_));
}

void Canvas::resetClip() {
    clip_x0_ = 0;
    clip_y0_ = 0;
    clip_x1_ = static_cast<int>(width_);
    clip_y1_ = static_cast<int>(height_);
}

void Canvas::blendSpan(uint32_t y, uint32_t x0, uint32_t x1, const float* cover, const Paint& paint) {
    const Premul solid = premul(paint.color, paint.opacity);
    std::optional<Matrix> inv;
    if (paint.gradient) inv = paint.gradient->to_canvas.inverse();
    float* row = px_.data() + static_cast<size_t>(y) * width_ * 4;
    for (uint32_t x = x0; x < x1; ++x) {
        const float c = std::min(1.0f, std::fabs(cover[x]));
        if (c <= 0.0005f) continue;
        Premul src = solid;
        if (paint.gradient) {
            if (!inv) continue;
            const Point g = inv->apply({static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f});
            src = premul(paint.gradient->at(g), paint.opacity);
        }
        float* d = row + static_cast<size_t>(x) * 4;
        const float sa = src.a * c;
        d[0] = src.r * c + d[0] * (1 - sa);
        d[1] = src.g * c + d[1] * (1 - sa);
        d[2] = src.b * c + d[2] * (1 - sa);
        d[3] = sa + d[3] * (1 - sa);
    }
}

namespace {

struct Edge {
    float x0, y0, x1, y1;
    int dir;
};

// Coverage of polygons, row by row, into `out(y, cover, x0, x1)`.
template <typename Out>
void scan(const Contours& contours, bool even_odd, int cx0, int cy0, int cx1, int cy1, Out out) {
    if (cx1 <= cx0 || cy1 <= cy0) return;
    std::vector<Edge> edges;
    float min_y = 1e30f, max_y = -1e30f, min_x = 1e30f, max_x = -1e30f;
    for (const auto& c : contours) {
        if (c.size() < 2) continue;
        for (size_t i = 0; i < c.size() && edges.size() < kMaxEdges; ++i) {
            Point a = c[i], b = c[(i + 1) % c.size()];
            if (!finite(a) || !finite(b) || a.y == b.y) continue;
            int dir = 1;
            if (a.y > b.y) {
                std::swap(a, b);
                dir = -1;
            }
            edges.push_back({a.x, a.y, b.x, b.y, dir});
            min_y = std::min(min_y, a.y);
            max_y = std::max(max_y, b.y);
            min_x = std::min({min_x, a.x, b.x});
            max_x = std::max({max_x, a.x, b.x});
        }
    }
    if (edges.empty()) return;
    const int y_start = std::max(cy0, static_cast<int>(std::floor(min_y)));
    const int y_end = std::min(cy1, static_cast<int>(std::ceil(max_y)));
    const int x_start = std::max(cx0, static_cast<int>(std::floor(min_x)));
    const int x_end = std::min(cx1, static_cast<int>(std::ceil(max_x)) + 1);
    if (y_start >= y_end || x_start >= x_end) return;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });

    const size_t w = static_cast<size_t>(cx1);
    std::vector<float> cover(w + 2, 0.0f), run(w + 2, 0.0f);
    std::vector<const Edge*> active;
    std::vector<std::pair<float, int>> xs;
    size_t next = 0;
    constexpr float kWeight = 1.0f / kSubRows;
    for (int y = y_start; y < y_end; ++y) {
        // Edges reaching this row come in, those finished before it go.
        while (next < edges.size() && edges[next].y0 < static_cast<float>(y + 1)) active.push_back(&edges[next++]);
        active.erase(std::remove_if(active.begin(), active.end(), [y](const Edge* e) { return e->y1 <= static_cast<float>(y); }), active.end());
        if (active.empty()) continue;
        std::fill(cover.begin() + x_start, cover.begin() + x_end + 1, 0.0f);
        std::fill(run.begin() + x_start, run.begin() + x_end + 1, 0.0f);
        bool any = false;
        for (int s = 0; s < kSubRows; ++s) {
            const float sy = static_cast<float>(y) + (static_cast<float>(s) + 0.5f) / kSubRows;
            xs.clear();
            for (const Edge* e : active) {
                if (sy < e->y0 || sy >= e->y1) continue;
                const float x = e->x0 + (sy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0);
                xs.emplace_back(x, e->dir);
            }
            if (xs.size() < 2) continue;
            std::sort(xs.begin(), xs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            int winding = 0;
            for (size_t k = 0; k + 1 < xs.size(); ++k) {
                winding += even_odd ? 1 : xs[k].second;
                const bool inside = even_odd ? (winding & 1) != 0 : winding != 0;
                if (!inside) continue;
                float xa = std::max(xs[k].first, static_cast<float>(x_start));
                float xb = std::min(xs[k + 1].first, static_cast<float>(x_end));
                if (xa >= xb) continue;
                any = true;
                const int ia = static_cast<int>(std::floor(xa)), ib = static_cast<int>(std::floor(xb));
                if (ia == ib) {
                    cover[ia] += (xb - xa) * kWeight;
                } else {
                    cover[ia] += (static_cast<float>(ia + 1) - xa) * kWeight;
                    run[ia + 1] += kWeight;
                    run[ib] -= kWeight;
                    if (ib < x_end) cover[ib] += (xb - static_cast<float>(ib)) * kWeight;
                }
            }
        }
        if (!any) continue;
        float acc = 0;
        for (int x = x_start; x < x_end; ++x) {
            acc += run[x];
            cover[x] += acc;
        }
        out(y, cover.data(), x_start, x_end);
    }
}

}  // namespace

void Canvas::fill(const Contours& contours, bool even_odd, const Paint& paint) {
    scan(contours, even_odd, clip_x0_, clip_y0_, clip_x1_, clip_y1_, [&](int y, const float* cover, int x0, int x1) {
        blendSpan(static_cast<uint32_t>(y), static_cast<uint32_t>(x0), static_cast<uint32_t>(x1), cover, paint);
    });
}

Canvas Canvas::coverage(uint32_t width, uint32_t height, const Contours& contours, bool even_odd) {
    Canvas out(width, height);
    scan(contours, even_odd, 0, 0, static_cast<int>(width), static_cast<int>(height), [&](int y, const float* cover, int x0, int x1) {
        float* row = out.px_.data() + static_cast<size_t>(y) * width * 4;
        for (int x = x0; x < x1; ++x) row[static_cast<size_t>(x) * 4 + 3] = std::min(1.0f, std::fabs(cover[x]));
    });
    return out;
}

void Canvas::fillRect(float x, float y, float w, float h, Color color, float radius) {
    if (w <= 0 || h <= 0) return;
    Paint p;
    p.color = color;
    fill(roundedRect(x, y, w, h, radius, radius), false, p);
}

void Canvas::drawRaster(const Raster& image, float x, float y, float w, float h, float opacity, const Color* tint) {
    if (image.empty() || w <= 0 || h <= 0 || !std::isfinite(x) || !std::isfinite(y)) return;
    const int x0 = std::max(clip_x0_, toInt(std::floor(x)));
    const int y0 = std::max(clip_y0_, toInt(std::floor(y)));
    const int x1 = std::min(clip_x1_, toInt(std::ceil(x + w)));
    const int y1 = std::min(clip_y1_, toInt(std::ceil(y + h)));
    const float sx = static_cast<float>(image.width) / w, sy = static_cast<float>(image.height) / h;
    const auto texel = [&](int tx, int ty) {
        tx = std::clamp(tx, 0, static_cast<int>(image.width) - 1);
        ty = std::clamp(ty, 0, static_cast<int>(image.height) - 1);
        const uint8_t* p = image.rgba.data() + (static_cast<size_t>(ty) * image.width + static_cast<size_t>(tx)) * 4;
        const float a = p[3] / 255.0f;
        if (tint) return Premul{tint->r / 255.0f * a, tint->g / 255.0f * a, tint->b / 255.0f * a, a};
        return Premul{p[0] / 255.0f * a, p[1] / 255.0f * a, p[2] / 255.0f * a, a};
    };
    const float tint_alpha = tint ? tint->a / 255.0f : 1.0f;
    for (int py = y0; py < y1; ++py) {
        const float cov_y = std::min(static_cast<float>(py + 1), y + h) - std::max(static_cast<float>(py), y);
        if (cov_y <= 0) continue;
        const float v = (static_cast<float>(py) + 0.5f - y) * sy - 0.5f;
        const int ty = toInt(std::floor(v));
        const float fy = v - static_cast<float>(ty);
        float* row = px_.data() + static_cast<size_t>(py) * width_ * 4;
        for (int px = x0; px < x1; ++px) {
            const float cov_x = std::min(static_cast<float>(px + 1), x + w) - std::max(static_cast<float>(px), x);
            if (cov_x <= 0) continue;
            const float u = (static_cast<float>(px) + 0.5f - x) * sx - 0.5f;
            const int tx = toInt(std::floor(u));
            const float fx = u - static_cast<float>(tx);
            const Premul a = texel(tx, ty), b = texel(tx + 1, ty), c = texel(tx, ty + 1), d = texel(tx + 1, ty + 1);
            const auto mix = [&](float pa, float pb, float pc, float pd) {
                return (pa * (1 - fx) + pb * fx) * (1 - fy) + (pc * (1 - fx) + pd * fx) * fy;
            };
            const float k = std::min(1.0f, cov_x) * std::min(1.0f, cov_y) * opacity * tint_alpha;
            const float sr = mix(a.r, b.r, c.r, d.r) * k, sg = mix(a.g, b.g, c.g, d.g) * k, sb = mix(a.b, b.b, c.b, d.b) * k,
                        sa = mix(a.a, b.a, c.a, d.a) * k;
            float* o = row + static_cast<size_t>(px) * 4;
            o[0] = sr + o[0] * (1 - sa);
            o[1] = sg + o[1] * (1 - sa);
            o[2] = sb + o[2] * (1 - sa);
            o[3] = sa + o[3] * (1 - sa);
        }
    }
}

void Canvas::composite(const Canvas& layer, float opacity, const Canvas* mask) {
    if (layer.width_ != width_ || layer.height_ != height_) return;
    if (mask && (mask->width_ != width_ || mask->height_ != height_)) return;
    for (int y = clip_y0_; y < clip_y1_; ++y)
        for (int x = clip_x0_; x < clip_x1_; ++x) {
            const size_t i = (static_cast<size_t>(y) * width_ + static_cast<size_t>(x)) * 4;
            const float k = opacity * (mask ? mask->px_[i + 3] : 1.0f);
            if (k <= 0) continue;
            const float sa = layer.px_[i + 3] * k;
            px_[i] = layer.px_[i] * k + px_[i] * (1 - sa);
            px_[i + 1] = layer.px_[i + 1] * k + px_[i + 1] * (1 - sa);
            px_[i + 2] = layer.px_[i + 2] * k + px_[i + 2] * (1 - sa);
            px_[i + 3] = sa + px_[i + 3] * (1 - sa);
        }
}

Raster Canvas::toRaster() const {
    Raster r;
    r.width = width_;
    r.height = height_;
    r.rgba.resize(static_cast<size_t>(width_) * height_ * 4);
    for (size_t i = 0; i < px_.size(); i += 4) {
        const float a = px_[i + 3];
        if (a <= 0) {
            std::memset(&r.rgba[i], 0, 4);
            continue;
        }
        r.rgba[i] = toByte(px_[i] / a);
        r.rgba[i + 1] = toByte(px_[i + 1] / a);
        r.rgba[i + 2] = toByte(px_[i + 2] / a);
        r.rgba[i + 3] = toByte(a);
    }
    return r;
}

// ── Shapes ───────────────────────────────────────────────────────────────────

namespace {

int arcSegments(float radius, float sweep, float scale) {
    const float r = std::max(0.01f, radius * std::max(scale, 0.01f));
    // Each segment's chord strays at most a fifth of a pixel from the arc.
    const float tol = 0.1f;
    const float step = r <= tol ? kPi / 2 : 2 * std::acos(std::max(-1.0f, 1 - tol / r));
    const float n = std::ceil(std::fabs(sweep) / std::max(step, 0.01f));
    if (!(n >= 1)) return 1;
    return static_cast<int>(std::min(n, 1024.0f));
}

void arcPoints(std::vector<Point>& out, float cx, float cy, float rx, float ry, float a0, float a1, float scale) {
    const int n = arcSegments(std::max(rx, ry), a1 - a0, scale);
    // Chords cut inside the arc: push them out so the area comes out the same.
    const float step = std::fabs(a1 - a0) / static_cast<float>(n);
    if (step > 1e-4f) {
        const float k = std::sqrt(step / std::sin(step));
        rx *= k;
        ry *= k;
    }
    for (int i = 0; i <= n; ++i) {
        const float a = a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(n);
        out.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
    }
}

float signedArea(const std::vector<Point>& c) {
    float a = 0;
    for (size_t i = 0; i < c.size(); ++i) {
        const Point& p = c[i];
        const Point& q = c[(i + 1) % c.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return a / 2;
}

void addOriented(Contours& out, std::vector<Point> c) {
    if (c.size() < 3) return;
    if (signedArea(c) < 0) std::reverse(c.begin(), c.end());
    out.push_back(std::move(c));
}

}  // namespace

Contours roundedRect(float x, float y, float w, float h, float rx, float ry) {
    rx = std::clamp(rx, 0.0f, w / 2);
    ry = std::clamp(ry, 0.0f, h / 2);
    std::vector<Point> c;
    if (rx <= 0 || ry <= 0) {
        c = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    } else {
        arcPoints(c, x + w - rx, y + ry, rx, ry, -kPi / 2, 0, 1);
        arcPoints(c, x + w - rx, y + h - ry, rx, ry, 0, kPi / 2, 1);
        arcPoints(c, x + rx, y + h - ry, rx, ry, kPi / 2, kPi, 1);
        arcPoints(c, x + rx, y + ry, rx, ry, kPi, 3 * kPi / 2, 1);
    }
    return {c};
}

Contours ellipse(float cx, float cy, float rx, float ry, float scale) {
    std::vector<Point> c;
    if (rx <= 0 || ry <= 0) return {};
    arcPoints(c, cx, cy, rx, ry, 0, 2 * kPi, scale);
    c.pop_back();
    return {c};
}

Contours strokeOutline(const std::vector<Point>& in, bool closed, const StrokeStyle& style, float scale) {
    Contours out;
    const float h = style.width / 2;
    if (h <= 0 || in.empty()) return out;
    std::vector<Point> pts;
    for (const Point& p : in)
        if (finite(p) && (pts.empty() || std::hypot(p.x - pts.back().x, p.y - pts.back().y) > 1e-6f)) pts.push_back(p);
    if (closed && pts.size() > 1 && std::hypot(pts.front().x - pts.back().x, pts.front().y - pts.back().y) <= 1e-6f) pts.pop_back();
    const auto disc = [&](Point c) {
        for (auto& e : ellipse(c.x, c.y, h, h, scale)) addOriented(out, std::move(e));
    };
    if (pts.size() == 1) {
        // A zero-length line still shows its caps.
        if (style.cap == 1) disc(pts[0]);
        else if (style.cap == 2) addOriented(out, {{pts[0].x - h, pts[0].y - h}, {pts[0].x + h, pts[0].y - h}, {pts[0].x + h, pts[0].y + h}, {pts[0].x - h, pts[0].y + h}});
        return out;
    }
    const size_t n = pts.size();
    const size_t segs = closed ? n : n - 1;
    const auto dirOf = [&](size_t i) {
        const Point a = pts[i % n], b = pts[(i + 1) % n];
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        return Point{(b.x - a.x) / len, (b.y - a.y) / len};
    };
    for (size_t i = 0; i < segs; ++i) {
        const Point a = pts[i], b = pts[(i + 1) % n];
        const Point d = dirOf(i);
        const Point nn{-d.y * h, d.x * h};
        addOriented(out, {{a.x + nn.x, a.y + nn.y}, {b.x + nn.x, b.y + nn.y}, {b.x - nn.x, b.y - nn.y}, {a.x - nn.x, a.y - nn.y}});
    }
    // Joins, where two segments meet.
    const size_t first_join = closed ? 0 : 1;
    const size_t last_join = closed ? n : n - 1;
    for (size_t j = first_join; j < last_join; ++j) {
        const Point v = pts[j % n];
        const Point d1 = dirOf((j + n - 1) % n), d2 = dirOf(j % n);
        const float cross = d1.x * d2.y - d1.y * d2.x;
        const float dot = d1.x * d2.x + d1.y * d2.y;
        if (std::fabs(cross) < 1e-6f && dot > 0) continue;   // straight on
        if (style.join == 1) {
            disc(v);
            continue;
        }
        const float s = cross > 0 ? -1.0f : 1.0f;   // the outside of the turn
        const Point a{v.x - s * d1.y * h, v.y + s * d1.x * h};
        const Point b{v.x - s * d2.y * h, v.y + s * d2.x * h};
        bool mitred = false;
        if (style.join == 0) {
            // The miter's length over the stroke width is 1 / sin(half the angle between them).
            const float theta = std::acos(std::clamp(-dot, -1.0f, 1.0f));
            const float ratio = 1.0f / std::max(1e-6f, std::sin(theta / 2));
            if (ratio <= style.miter_limit) {
                const float denom = d1.x * d2.y - d1.y * d2.x;
                if (std::fabs(denom) > 1e-6f) {
                    const float t = ((b.x - a.x) * d2.y - (b.y - a.y) * d2.x) / denom;
                    const Point m{a.x + d1.x * t, a.y + d1.y * t};
                    addOriented(out, {v, a, m, b});
                    mitred = true;
                }
            }
        }
        if (!mitred) addOriented(out, {v, a, b});
    }
    if (!closed) {
        for (int end = 0; end < 2; ++end) {
            const Point p = end == 0 ? pts.front() : pts.back();
            Point d = end == 0 ? dirOf(0) : dirOf(n - 2);
            if (end == 0) d = {-d.x, -d.y};   // pointing out of the line
            if (style.cap == 1) {
                disc(p);
            } else if (style.cap == 2) {
                const Point nn{-d.y * h, d.x * h};
                const Point q{p.x + d.x * h, p.y + d.y * h};
                addOriented(out, {{p.x + nn.x, p.y + nn.y}, {q.x + nn.x, q.y + nn.y}, {q.x - nn.x, q.y - nn.y}, {p.x - nn.x, p.y - nn.y}});
            }
        }
    }
    return out;
}

// ── Path data ────────────────────────────────────────────────────────────────

namespace {

class PathReader {
public:
    explicit PathReader(const std::string& d) : d_(d) {}
    void skip() {
        while (i_ < d_.size() && (d_[i_] == ' ' || d_[i_] == ',' || d_[i_] == '\t' || d_[i_] == '\n' || d_[i_] == '\r' || d_[i_] == '\f')) ++i_;
    }
    bool atEnd() {
        skip();
        return i_ >= d_.size();
    }
    bool atNumber() {
        skip();
        if (i_ >= d_.size()) return false;
        const char c = d_[i_];
        return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
    }
    char command() {
        skip();
        return i_ < d_.size() ? d_[i_++] : '\0';
    }
    bool number(float& out) {
        skip();
        const size_t start = i_;
        if (i_ < d_.size() && (d_[i_] == '-' || d_[i_] == '+')) ++i_;
        bool digits = false;
        while (i_ < d_.size() && d_[i_] >= '0' && d_[i_] <= '9') { ++i_; digits = true; }
        if (i_ < d_.size() && d_[i_] == '.') {
            ++i_;
            while (i_ < d_.size() && d_[i_] >= '0' && d_[i_] <= '9') { ++i_; digits = true; }
        }
        if (!digits) { i_ = start; return false; }
        if (i_ < d_.size() && (d_[i_] == 'e' || d_[i_] == 'E')) {
            size_t k = i_ + 1;
            if (k < d_.size() && (d_[k] == '-' || d_[k] == '+')) ++k;
            if (k < d_.size() && d_[k] >= '0' && d_[k] <= '9') {
                while (k < d_.size() && d_[k] >= '0' && d_[k] <= '9') ++k;
                i_ = k;
            }
        }
        out = std::strtof(d_.substr(start, i_ - start).c_str(), nullptr);
        return std::isfinite(out);
    }
    bool flag(bool& out) {
        skip();
        if (i_ < d_.size() && (d_[i_] == '0' || d_[i_] == '1')) {
            out = d_[i_++] == '1';
            return true;
        }
        return false;
    }

private:
    const std::string& d_;
    size_t i_ = 0;
};

// Clamped as a float: a hostile path's lengths don't fit an int.
int segmentCount(float n, int most) {
    if (!(n >= 1)) return 1;
    return static_cast<int>(std::min(n, static_cast<float>(most)));
}

int curveSegments(float length, float scale) {
    return segmentCount(std::ceil(std::sqrt(std::max(0.0f, length * scale)) * 1.2f), 512);
}

}  // namespace

std::vector<std::pair<std::vector<Point>, bool>> parsePathData(const std::string& d, float scale) {
    std::vector<std::pair<std::vector<Point>, bool>> out;
    PathReader r(d);
    Point cur, start, ctrl;
    char prev = 0;
    size_t total = 0;
    const auto contour = [&]() -> std::vector<Point>& {
        if (out.empty() || out.back().second) {
            out.push_back({{cur}, false});
            if (out.size() >= 2 && out[out.size() - 2].second) start = cur;
        }
        return out.back().first;
    };
    const auto lineTo = [&](Point p) {
        contour().push_back(p);
        cur = p;
        ++total;
    };
    char cmd = 0;
    while (!r.atEnd() && total < 4'000'000) {
        if (!r.atNumber()) {
            cmd = r.command();
        } else if (cmd == 'M') {
            cmd = 'L';   // numbers after a moveto are linetos
        } else if (cmd == 'm') {
            cmd = 'l';
        } else if (cmd == 0 || cmd == 'Z' || cmd == 'z') {
            break;
        }
        const bool rel = cmd >= 'a' && cmd <= 'z';
        const char up = static_cast<char>(rel ? cmd - 32 : cmd);
        const Point o = rel ? cur : Point{0, 0};
        float v[7];
        const auto read = [&](int count) {
            for (int k = 0; k < count; ++k)
                if (!r.number(v[k])) return false;
            return true;
        };
        if (up == 'M') {
            if (!read(2)) break;
            cur = {o.x + v[0], o.y + v[1]};
            start = cur;
            out.push_back({{cur}, false});
            ctrl = cur;
        } else if (up == 'L') {
            if (!read(2)) break;
            lineTo({o.x + v[0], o.y + v[1]});
            ctrl = cur;
        } else if (up == 'H') {
            if (!read(1)) break;
            lineTo({rel ? cur.x + v[0] : v[0], cur.y});
            ctrl = cur;
        } else if (up == 'V') {
            if (!read(1)) break;
            lineTo({cur.x, rel ? cur.y + v[0] : v[0]});
            ctrl = cur;
        } else if (up == 'C' || up == 'S') {
            Point p1, p2, p3;
            if (up == 'C') {
                if (!read(6)) break;
                p1 = {o.x + v[0], o.y + v[1]};
                p2 = {o.x + v[2], o.y + v[3]};
                p3 = {o.x + v[4], o.y + v[5]};
            } else {
                if (!read(4)) break;
                const bool smooth = prev == 'C' || prev == 'c' || prev == 'S' || prev == 's';
                p1 = smooth ? Point{2 * cur.x - ctrl.x, 2 * cur.y - ctrl.y} : cur;
                p2 = {o.x + v[0], o.y + v[1]};
                p3 = {o.x + v[2], o.y + v[3]};
            }
            const Point p0 = cur;
            const float len = std::hypot(p1.x - p0.x, p1.y - p0.y) + std::hypot(p2.x - p1.x, p2.y - p1.y) + std::hypot(p3.x - p2.x, p3.y - p2.y);
            const int n = curveSegments(len, scale);
            for (int k = 1; k <= n; ++k) {
                const float t = static_cast<float>(k) / static_cast<float>(n), u = 1 - t;
                lineTo({u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x,
                        u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y});
            }
            ctrl = p2;
        } else if (up == 'Q' || up == 'T') {
            Point p1, p2;
            if (up == 'Q') {
                if (!read(4)) break;
                p1 = {o.x + v[0], o.y + v[1]};
                p2 = {o.x + v[2], o.y + v[3]};
            } else {
                if (!read(2)) break;
                const bool smooth = prev == 'Q' || prev == 'q' || prev == 'T' || prev == 't';
                p1 = smooth ? Point{2 * cur.x - ctrl.x, 2 * cur.y - ctrl.y} : cur;
                p2 = {o.x + v[0], o.y + v[1]};
            }
            const Point p0 = cur;
            const float len = std::hypot(p1.x - p0.x, p1.y - p0.y) + std::hypot(p2.x - p1.x, p2.y - p1.y);
            const int n = curveSegments(len, scale);
            for (int k = 1; k <= n; ++k) {
                const float t = static_cast<float>(k) / static_cast<float>(n), u = 1 - t;
                lineTo({u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x, u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y});
            }
            ctrl = p1;
        } else if (up == 'A') {
            bool large = false, sweep = false;
            if (!r.number(v[0]) || !r.number(v[1]) || !r.number(v[2]) || !r.flag(large) || !r.flag(sweep) || !r.number(v[3]) || !r.number(v[4])) break;
            const Point p0 = cur, p1{o.x + v[3], o.y + v[4]};
            float rx = std::fabs(v[0]), ry = std::fabs(v[1]);
            if ((p0.x == p1.x && p0.y == p1.y)) {
                ctrl = cur;
            } else if (rx == 0 || ry == 0) {
                lineTo(p1);
            } else {
                // The endpoint form made a centre form: SVG's implementation notes, F.6.5.
                const float phi = v[2] * kPi / 180.0f, cs = std::cos(phi), sn = std::sin(phi);
                const float dx = (p0.x - p1.x) / 2, dy = (p0.y - p1.y) / 2;
                const float x1 = cs * dx + sn * dy, y1 = -sn * dx + cs * dy;
                const float lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
                if (lambda > 1) {
                    rx *= std::sqrt(lambda);
                    ry *= std::sqrt(lambda);
                }
                const float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
                const float den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
                float coef = den > 0 ? std::sqrt(std::max(0.0f, num / den)) : 0;
                if (large == sweep) coef = -coef;
                const float cxp = coef * rx * y1 / ry, cyp = -coef * ry * x1 / rx;
                const float ccx = cs * cxp - sn * cyp + (p0.x + p1.x) / 2, ccy = sn * cxp + cs * cyp + (p0.y + p1.y) / 2;
                const auto angle = [](float ux, float uy, float vx, float vy) {
                    return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
                };
                const float t1 = angle(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
                float dt = angle((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
                if (!sweep && dt > 0) dt -= 2 * kPi;
                else if (sweep && dt < 0) dt += 2 * kPi;
                const int n = arcSegments(std::max(rx, ry), dt, scale);
                for (int k = 1; k <= n; ++k) {
                    const float t = t1 + dt * static_cast<float>(k) / static_cast<float>(n);
                    const float ex = rx * std::cos(t), ey = ry * std::sin(t);
                    lineTo(k == n ? p1 : Point{cs * ex - sn * ey + ccx, sn * ex + cs * ey + ccy});
                }
            }
            ctrl = cur;
        } else if (up == 'Z') {
            if (!out.empty() && !out.back().second) {
                out.back().second = true;
                cur = start;
            }
            ctrl = cur;
        } else {
            break;   // not a command: the rest of the path is in error
        }
        prev = cmd;
    }
    return out;
}

// ── Transforms ───────────────────────────────────────────────────────────────

Matrix parseTransform(const std::string& text) {
    Matrix total;
    size_t i = 0;
    const auto skip = [&] {
        while (i < text.size() && (text[i] == ' ' || text[i] == ',' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
    };
    while (true) {
        skip();
        if (i >= text.size()) break;
        const size_t name_start = i;
        while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i]))) ++i;
        const std::string name = text.substr(name_start, i - name_start);
        skip();
        if (name.empty() || i >= text.size() || text[i] != '(') return Matrix{};   // in error: no transform
        const size_t close = text.find(')', i);
        if (close == std::string::npos) return Matrix{};
        const std::string args_text = text.substr(i + 1, close - i - 1);
        i = close + 1;
        std::vector<float> args;
        PathReader r(args_text);
        float v = 0;
        while (r.number(v) && args.size() < 6) args.push_back(v);
        if (!r.atEnd()) return Matrix{};
        Matrix m;
        const auto rad = [](float deg) { return deg * kPi / 180.0f; };
        if (name == "matrix" && args.size() == 6) m = {args[0], args[1], args[2], args[3], args[4], args[5]};
        else if (name == "translate" && (args.size() == 1 || args.size() == 2)) m = Matrix::translate(args[0], args.size() == 2 ? args[1] : 0);
        else if (name == "scale" && (args.size() == 1 || args.size() == 2)) m = Matrix::scaling(args[0], args.size() == 2 ? args[1] : args[0]);
        else if (name == "rotate" && (args.size() == 1 || args.size() == 3)) {
            const float a = rad(args[0]);
            const Matrix rot{std::cos(a), std::sin(a), -std::sin(a), std::cos(a), 0, 0};
            m = args.size() == 3 ? Matrix::translate(args[1], args[2]).after(rot).after(Matrix::translate(-args[1], -args[2])) : rot;
        } else if (name == "skewX" && args.size() == 1) m = {1, 0, std::tan(rad(args[0])), 1, 0, 0};
        else if (name == "skewY" && args.size() == 1) m = {1, std::tan(rad(args[0])), 0, 1, 0, 0};
        else return Matrix{};
        total = total.after(m);
    }
    return total;
}

// ── CSS gradients ────────────────────────────────────────────────────────────

Raster renderGradient(const Gradient& g, float box_w, float box_h, uint32_t px_w, uint32_t px_h) {
    Raster out;
    if (px_w == 0 || px_h == 0 || box_w <= 0 || box_h <= 0 || g.stops.empty()) return out;
    if (px_w > kMaxRasterSide || px_h > kMaxRasterSide || static_cast<uint64_t>(px_w) * px_h > kMaxRasterPixels) return out;
    GradientPaint p;
    float length = 1;   // what stop positions are fractions of
    if (g.kind == Gradient::Kind::Linear) {
        float dx, dy;
        if (g.to_x != 0 && g.to_y != 0) {
            // Toward a corner: the angle that puts the other two corners on the 50% line.
            dx = static_cast<float>(g.to_x) * box_h;
            dy = static_cast<float>(g.to_y) * box_w;
            const float n = std::hypot(dx, dy);
            dx /= n;
            dy /= n;
        } else {
            const float a = g.angle * kPi / 180.0f;
            dx = std::sin(a);
            dy = -std::cos(a);
        }
        length = std::fabs(box_w * dx) + std::fabs(box_h * dy);
        p.kind = GradientPaint::Kind::Linear;
        p.x1 = box_w / 2 - dx * length / 2;
        p.y1 = box_h / 2 - dy * length / 2;
        p.x2 = box_w / 2 + dx * length / 2;
        p.y2 = box_h / 2 + dy * length / 2;
    } else {
        const float cx = g.center_x.resolve(box_w), cy = g.center_y.resolve(box_h);
        if (g.kind == Gradient::Kind::Conic) {
            p.kind = GradientPaint::Kind::Conic;
            p.cx = cx;
            p.cy = cy;
            p.start_angle = g.from * kPi / 180.0f;
            length = 1;
        } else {
            float rx = 0, ry = 0;
            const float sx_near = std::min(std::fabs(cx), std::fabs(box_w - cx)), sy_near = std::min(std::fabs(cy), std::fabs(box_h - cy));
            const float sx_far = std::max(std::fabs(cx), std::fabs(box_w - cx)), sy_far = std::max(std::fabs(cy), std::fabs(box_h - cy));
            if (g.explicit_size) {
                rx = g.radius_x.resolve(box_w);
                ry = g.circle ? rx : g.radius_y.resolve(box_h);
            } else {
                switch (g.extent) {
                    case Gradient::Extent::ClosestSide:
                        rx = g.circle ? std::min(sx_near, sy_near) : sx_near;
                        ry = g.circle ? rx : sy_near;
                        break;
                    case Gradient::Extent::FarthestSide:
                        rx = g.circle ? std::max(sx_far, sy_far) : sx_far;
                        ry = g.circle ? rx : sy_far;
                        break;
                    case Gradient::Extent::ClosestCorner:
                        if (g.circle) rx = ry = std::hypot(sx_near, sy_near);
                        else { rx = sx_near * std::sqrt(2.0f); ry = sy_near * std::sqrt(2.0f); }
                        break;
                    case Gradient::Extent::FarthestCorner:
                        if (g.circle) rx = ry = std::hypot(sx_far, sy_far);
                        else { rx = sx_far * std::sqrt(2.0f); ry = sy_far * std::sqrt(2.0f); }
                        break;
                }
            }
            rx = std::max(rx, 0.01f);
            ry = std::max(ry, 0.01f);
            p.kind = GradientPaint::Kind::Radial;
            p.cx = p.cy = p.fx = p.fy = 0;
            p.r = 1;
            p.to_canvas = Matrix::translate(cx, cy).after(Matrix::scaling(rx, ry));
            length = rx;
        }
    }
    length = std::max(length, 1e-4f);

    // Stop positions: those given, then the missing ones spread between their neighbours.
    std::vector<float> at(g.stops.size(), std::nanf(""));
    for (size_t i = 0; i < g.stops.size(); ++i)
        if (g.stops[i].has_position) at[i] = g.kind == Gradient::Kind::Conic ? g.stops[i].position.percent / 100.0f : g.stops[i].position.resolve(length) / length;
    if (std::isnan(at.front())) at.front() = 0;
    if (std::isnan(at.back())) at.back() = 1;
    float highest = at.front();
    for (float& a : at)
        if (!std::isnan(a)) {
            a = std::max(a, highest);
            highest = a;
        }
    for (size_t i = 1; i < at.size();) {
        if (!std::isnan(at[i])) { ++i; continue; }
        size_t j = i;
        while (std::isnan(at[j])) ++j;
        for (size_t k = i; k < j; ++k) at[k] = at[i - 1] + (at[j] - at[i - 1]) * static_cast<float>(k - i + 1) / static_cast<float>(j - i + 1);
        i = j;
    }
    for (size_t i = 0; i < g.stops.size(); ++i) p.stops.push_back({at[i], g.stops[i].color});
    if (g.repeating) {
        p.spread = GradientPaint::Spread::Repeat;
        p.period_start = at.front();
        p.period_end = at.back();
    }

    out.width = px_w;
    out.height = px_h;
    out.rgba.resize(static_cast<size_t>(px_w) * px_h * 4);
    const std::optional<Matrix> inv = p.to_canvas.inverse();
    if (!inv) return {};
    const float kx = box_w / static_cast<float>(px_w), ky = box_h / static_cast<float>(px_h);
    for (uint32_t y = 0; y < px_h; ++y)
        for (uint32_t x = 0; x < px_w; ++x) {
            const Point q = inv->apply({(static_cast<float>(x) + 0.5f) * kx, (static_cast<float>(y) + 0.5f) * ky});
            const Color c = p.at(q);
            uint8_t* o = out.rgba.data() + (static_cast<size_t>(y) * px_w + x) * 4;
            o[0] = c.r;
            o[1] = c.g;
            o[2] = c.b;
            o[3] = c.a;
        }
    return out;
}

}  // namespace fbx::xweb
