// The CPU rasterizer under SVG, gradients and masks: polygons filled by scanline with
// eight sub-rows per pixel and exact coverage along each, then composited source over.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>

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

Canvas Canvas::fromAlpha(uint32_t width, uint32_t height, const std::vector<float>& alpha) {
    Canvas out(width, height);
    for (size_t i = 0; i < alpha.size() && i * 4 + 3 < out.px_.size(); ++i) out.px_[i * 4 + 3] = alpha[i];
    return out;
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

// ── Boxes: uneven corners, fancy borders, shadows ────────────────────────────

namespace fbx::xweb {

Contours roundedRect(float x, float y, float w, float h, const float radii[4][2]) {
    if (w <= 0 || h <= 0) return {};
    std::vector<Point> c;
    const auto corner = [&](int i, float cx, float cy, float a0) {
        const float rx = radii[i][0], ry = radii[i][1];
        if (rx <= 0 || ry <= 0) {
            c.push_back({cx + (i == 1 || i == 2 ? 0 : 0), cy});
            return false;
        }
        arcPoints(c, cx, cy, rx, ry, a0, a0 + kPi / 2, 1);
        return true;
    };
    // Each corner's arc centre, or the corner itself when it's square.
    if (!corner(0, x + radii[0][0], y + radii[0][1], kPi)) c.back() = {x, y};
    if (!corner(1, x + w - radii[1][0], y + radii[1][1], 3 * kPi / 2)) c.back() = {x + w, y};
    if (!corner(2, x + w - radii[2][0], y + h - radii[2][1], 0)) c.back() = {x + w, y + h};
    if (!corner(3, x + radii[3][0], y + h - radii[3][1], kPi / 2)) c.back() = {x, y + h};
    return {c};
}

void fitRadii(float radii[4][2], float w, float h) {
    float f = 1;
    const auto check = [&](float side, float a, float b) {
        if (a + b > side && a + b > 0) f = std::min(f, side / (a + b));
    };
    check(w, radii[0][0], radii[1][0]);
    check(h, radii[1][1], radii[2][1]);
    check(w, radii[2][0], radii[3][0]);
    check(h, radii[3][1], radii[0][1]);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 2; ++j) radii[i][j] = std::max(0.0f, radii[i][j] * f);
}

namespace {

bool isBlack(Color c) { return c.r == 0 && c.g == 0 && c.b == 0; }

// As Blink's Color::Dark and Color::Light.
Color shade(Color c, bool dark) {
    if (isBlack(c)) return {0x54, 0x54, 0x54, c.a};
    const float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
    const float v = std::max({r, g, b});
    float m;
    if (dark) m = v == 0 ? 0 : std::max(0.0f, (v - 0.33f) / v);
    else m = v == 0 ? 0 : std::min(1.0f, v + 0.33f) / v;
    const float k = 255.99998f;
    return {static_cast<uint8_t>(std::clamp(r * m * k, 0.0f, 255.0f)), static_cast<uint8_t>(std::clamp(g * m * k, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(b * m * k, 0.0f, 255.0f)), c.a};
}

int distanceSquared(Color a, Color b) {
    const int dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return dr * dr + dg * dg + db * db;
}

// Blink's CalculateBorderStyleColor: the top and left of an inset darken, and so on.
Color styleColor(BorderStyle style, int side, Color c) {
    const bool top_left = side == 0 || side == 3;
    const bool darken = top_left == (style == BorderStyle::Inset || style == BorderStyle::Groove);
    if (darken) {
        if (distanceSquared(c, {0, 0, 0, 255}) > distanceSquared({0x20, 0x20, 0x20, 255}, {0, 0, 0, 255})) return shade(c, true);
    } else if (distanceSquared(c, {255, 255, 255, 255}) > distanceSquared({0xEB, 0xEB, 0xEB, 255}, {255, 255, 255, 255})) {
        return shade(c, false);
    }
    return c;
}

// Three box blurs, which come close to a Gaussian of this deviation, over alpha.
// `outside`: what lies beyond the edges, which an inset shadow needs to be solid.
void blurAlpha(std::vector<float>& a, int w, int h, float sigma, float outside = 0) {
    if (sigma < 0.3f || w <= 0 || h <= 0) return;
    int sizes[3];
    const float ideal = std::sqrt(12 * sigma * sigma / 3 + 1);
    int wl = static_cast<int>(std::floor(ideal));
    if (wl % 2 == 0) --wl;
    const int wu = wl + 2;
    const float mi = (12 * sigma * sigma - 3.0f * wl * wl - 4.0f * 3 * wl - 3 * 3) / (-4.0f * wl - 4);
    const int m = static_cast<int>(std::lround(mi));
    for (int i = 0; i < 3; ++i) sizes[i] = i < m ? wl : wu;
    std::vector<float> tmp(a.size());
    const auto pass = [&](std::vector<float>& src, std::vector<float>& dst, int radius, bool horizontal) {
        const int n = horizontal ? w : h, lines = horizontal ? h : w;
        const float inv = 1.0f / static_cast<float>(2 * radius + 1);
        for (int l = 0; l < lines; ++l) {
            const auto at = [&](int i) -> float& { return horizontal ? src[static_cast<size_t>(l) * w + i] : src[static_cast<size_t>(i) * w + l]; };
            const auto out = [&](int i) -> float& { return horizontal ? dst[static_cast<size_t>(l) * w + i] : dst[static_cast<size_t>(i) * w + l]; };
            float acc = 0;
            for (int i = -radius; i <= radius; ++i) acc += (i >= 0 && i < n) ? at(i) : outside;
            for (int i = 0; i < n; ++i) {
                out(i) = acc * inv;
                const int add = i + radius + 1, sub = i - radius;
                acc += (add < n ? at(add) : outside) - (sub >= 0 ? at(sub) : outside);
            }
        }
    };
    for (int i = 0; i < 3; ++i) {
        const int r = (sizes[i] - 1) / 2;
        if (r <= 0) continue;
        pass(a, tmp, r, true);
        pass(tmp, a, r, false);
    }
}

std::vector<float> coverageOf(int w, int h, const Contours& contours, bool even_odd) {
    std::vector<float> out(static_cast<size_t>(w) * h, 0.0f);
    scan(contours, even_odd, 0, 0, w, h, [&](int y, const float* cover, int x0, int x1) {
        for (int x = x0; x < x1; ++x) out[static_cast<size_t>(y) * w + x] = std::min(1.0f, std::fabs(cover[x]));
    });
    return out;
}

Contours scaled(Contours c, float kx, float ky, float dx, float dy) {
    for (auto& poly : c)
        for (Point& p : poly) p = {(p.x + dx) * kx, (p.y + dy) * ky};
    return c;
}

// The rounded rectangle inside this one by these insets: its radii shrink by them.
Contours insetShape(float x, float y, float w, float h, const float radii[4][2], float top, float right, float bottom, float left) {
    float r[4][2] = {{radii[0][0] - left, radii[0][1] - top},
                     {radii[1][0] - right, radii[1][1] - top},
                     {radii[2][0] - right, radii[2][1] - bottom},
                     {radii[3][0] - left, radii[3][1] - bottom}};
    for (auto& c : r) {
        c[0] = std::max(0.0f, c[0]);
        c[1] = std::max(0.0f, c[1]);
    }
    return roundedRect(x + left, y + top, w - left - right, h - top - bottom, r);
}

// Points along a polyline, by distance.
struct Walk {
    std::vector<Point> pts;
    std::vector<float> at;   // distance to each point
    explicit Walk(std::vector<Point> p) : pts(std::move(p)) {
        float d = 0;
        for (size_t i = 0; i < pts.size(); ++i) {
            if (i) d += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
            at.push_back(d);
        }
    }
    [[nodiscard]] float length() const { return at.empty() ? 0 : at.back(); }
    [[nodiscard]] Point point(float d) const {
        if (pts.empty()) return {};
        if (d <= 0) return pts.front();
        for (size_t i = 1; i < pts.size(); ++i)
            if (at[i] >= d) {
                const float seg = at[i] - at[i - 1];
                const float t = seg > 0 ? (d - at[i - 1]) / seg : 0;
                return {pts[i - 1].x + (pts[i].x - pts[i - 1].x) * t, pts[i - 1].y + (pts[i].y - pts[i - 1].y) * t};
            }
        return pts.back();
    }
    [[nodiscard]] std::vector<Point> piece(float d0, float d1) const {
        std::vector<Point> out{point(d0)};
        for (size_t i = 0; i < pts.size(); ++i)
            if (at[i] > d0 && at[i] < d1) out.push_back(pts[i]);
        out.push_back(point(d1));
        return out;
    }
};

// A polygon cut to the side of a line where `keep(p) >= 0`, the line given by `keep` linear.
std::vector<Point> clipHalf(const std::vector<Point>& in, const std::function<float(Point)>& keep) {
    std::vector<Point> out;
    for (size_t i = 0; i < in.size(); ++i) {
        const Point a = in[i], b = in[(i + 1) % in.size()];
        const float ka = keep(a), kb = keep(b);
        if (ka >= 0) out.push_back(a);
        if ((ka >= 0) != (kb >= 0)) {
            const float t = ka / (ka - kb);
            out.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
        }
    }
    return out;
}

Contours sideRegionUncut(int side, float w, float h, const float b[4]);

// One side's share of the ring: from its outer corners in toward the middle, split from
// the neighbours along the line through each outer corner and its inner one, and from the
// opposite side where their widths say they meet.
Contours sideRegion(int side, float w, float h, const float b[4]) {
    Contours c = sideRegionUncut(side, w, h, b);
    if (c.empty()) return c;
    const float split_y = b[0] + b[2] > 0 ? h * b[0] / (b[0] + b[2]) : h / 2;
    const float split_x = b[3] + b[1] > 0 ? w * b[3] / (b[3] + b[1]) : w / 2;
    std::function<float(Point)> keep;
    switch (side) {
        case 0: keep = [=](Point p) { return split_y - p.y; }; break;
        case 1: keep = [=](Point p) { return p.x - split_x; }; break;
        case 2: keep = [=](Point p) { return p.y - split_y; }; break;
        default: keep = [=](Point p) { return split_x - p.x; }; break;
    }
    c[0] = clipHalf(c[0], keep);
    return c;
}

Contours sideRegionUncut(int side, float w, float h, const float b[4]) {
    const float big = 4 * (w + h) + 100;
    const auto toward = [&](Point outer, float dx, float dy) {
        const float n = std::hypot(dx, dy);
        if (n <= 0) return outer;
        return Point{outer.x + dx / n * big, outer.y + dy / n * big};
    };
    Point a, c, a_far, c_far;
    switch (side) {
        case 0: a = {0, 0}; c = {w, 0}; a_far = toward(a, b[3], b[0]); c_far = toward(c, -b[1], b[0]); break;
        case 1: a = {w, 0}; c = {w, h}; a_far = toward(a, -b[1], b[0]); c_far = toward(c, -b[1], -b[2]); break;
        case 2: a = {w, h}; c = {0, h}; a_far = toward(a, -b[1], -b[2]); c_far = toward(c, b[3], -b[2]); break;
        default: a = {0, h}; c = {0, 0}; a_far = toward(a, b[3], -b[2]); c_far = toward(c, b[3], b[0]); break;
    }
    // Don't let the two cut lines cross into a bow tie: stop where they meet.
    const float d1x = a_far.x - a.x, d1y = a_far.y - a.y, d2x = c_far.x - c.x, d2y = c_far.y - c.y;
    const float den = d1x * d2y - d1y * d2x;
    if (std::fabs(den) > 1e-6f) {
        const float t = ((c.x - a.x) * d2y - (c.y - a.y) * d2x) / den;
        const float u = ((c.x - a.x) * d1y - (c.y - a.y) * d1x) / den;
        if (t > 0 && t < 1 && u > 0 && u < 1) {
            const Point m{a.x + d1x * t, a.y + d1y * t};
            return {{a, c, m}};
        }
    }
    return {{a, c, c_far, a_far}};
}

// The middle of a side's border, from the middle of one corner to the middle of the next.
std::vector<Point> sideCentreLine(int side, float w, float h, const float r[4][2], const float b[4]) {
    // The ring's middle: a rounded rectangle halfway in.
    float mid[4][2];
    const float hb[4] = {b[0] / 2, b[1] / 2, b[2] / 2, b[3] / 2};
    const float ins[4][2] = {{hb[3], hb[0]}, {hb[1], hb[0]}, {hb[1], hb[2]}, {hb[3], hb[2]}};
    for (int i = 0; i < 4; ++i) {
        mid[i][0] = std::max(0.0f, r[i][0] - ins[i][0]);
        mid[i][1] = std::max(0.0f, r[i][1] - ins[i][1]);
    }
    const float x0 = hb[3], y0 = hb[0], x1 = w - hb[1], y1 = h - hb[2];
    std::vector<Point> out;
    const auto arc = [&](int i, float cx, float cy, float a0, float a1) {
        if (mid[i][0] <= 0 || mid[i][1] <= 0) {
            // A square corner: the side's line runs on to the box's edge, as Chromium's does.
            const bool horizontal = side == 0 || side == 2;
            const float x = i == 0 || i == 3 ? (horizontal ? 0 : x0) : (horizontal ? w : x1);
            const float y = i < 2 ? (horizontal ? y0 : 0) : (horizontal ? y1 : h);
            out.push_back({x, y});
            return;
        }
        arcPoints(out, cx, cy, mid[i][0], mid[i][1], a0, a1, 1);
    };
    switch (side) {
        case 0:
            arc(0, x0 + mid[0][0], y0 + mid[0][1], 5 * kPi / 4, 3 * kPi / 2);
            arc(1, x1 - mid[1][0], y0 + mid[1][1], 3 * kPi / 2, 7 * kPi / 4);
            break;
        case 1:
            arc(1, x1 - mid[1][0], y0 + mid[1][1], 7 * kPi / 4, 2 * kPi);
            arc(2, x1 - mid[2][0], y1 - mid[2][1], 0, kPi / 4);
            break;
        case 2:
            arc(2, x1 - mid[2][0], y1 - mid[2][1], kPi / 4, kPi / 2);
            arc(3, x0 + mid[3][0], y1 - mid[3][1], kPi / 2, 3 * kPi / 4);
            break;
        default:
            arc(3, x0 + mid[3][0], y1 - mid[3][1], 3 * kPi / 4, kPi);
            arc(0, x0 + mid[0][0], y0 + mid[0][1], kPi, 5 * kPi / 4);
            break;
    }
    return out;
}

}  // namespace

Color darker(Color c) { return shade(c, true); }
Color lighter(Color c) { return shade(c, false); }

Raster renderBoxPaint(const BoxPaint& box, float area_w, float area_h, uint32_t px_w, uint32_t px_h) {
    if (px_w == 0 || px_h == 0 || area_w <= 0 || area_h <= 0 || px_w > kMaxRasterSide || px_h > kMaxRasterSide ||
        static_cast<uint64_t>(px_w) * px_h > kMaxRasterPixels)
        return {};
    const float kx = static_cast<float>(px_w) / area_w, ky = static_cast<float>(px_h) / area_h;
    const int W = static_cast<int>(px_w), H = static_cast<int>(px_h);
    Canvas canvas(px_w, px_h);
    const Contours outer = scaled(roundedRect(0, 0, box.w, box.h, box.radii), kx, ky, box.ox, box.oy);
    const float* b = box.border;

    switch (box.part) {
        case BoxPaint::Part::Checkbox:
        case BoxPaint::Part::Radio:
        case BoxPaint::Part::SelectArrow: {
            // Chromium's light theme: #767676 outlines, #0075ff when on.
            const Color grey{0x76, 0x76, 0x76, 255}, blue{0x00, 0x75, 0xff, 255}, white{255, 255, 255, 255};
            const float x0 = box.ox, y0 = box.oy, w = box.w, h = box.h;
            const auto fillShape = [&](const Contours& c, Color col, bool even_odd = false) {
                Paint p;
                p.color = col;
                canvas.fill(scaled(c, kx, ky, 0, 0), even_odd, p);
            };
            const auto strokeLine = [&](std::vector<Point> pts, float width, Color col) {
                Contours all;
                const StrokeStyle st{width, 1, 1, 4};
                for (auto& poly : strokeOutline(pts, false, st, kx)) all.push_back(std::move(poly));
                fillShape(all, col);
            };
            if (box.part == BoxPaint::Part::SelectArrow) {
                strokeLine({{x0 + w * 0.3f, y0 + h * 0.4f}, {x0 + w * 0.5f, y0 + h * 0.6f}, {x0 + w * 0.7f, y0 + h * 0.4f}},
                           std::max(1.0f, w * 0.12f), box.colors[0]);
                break;
            }
            if (box.part == BoxPaint::Part::Checkbox) {
                const float r[4][2] = {{2, 2}, {2, 2}, {2, 2}, {2, 2}};
                if (box.checked) {
                    fillShape(roundedRect(x0, y0, w, h, r), blue);
                    strokeLine({{x0 + w * 0.24f, y0 + h * 0.52f}, {x0 + w * 0.42f, y0 + h * 0.70f}, {x0 + w * 0.77f, y0 + h * 0.31f}},
                               std::max(1.5f, w * 0.15f), white);
                } else {
                    Contours ring = roundedRect(x0, y0, w, h, r);
                    const float ri[4][2] = {{1, 1}, {1, 1}, {1, 1}, {1, 1}};
                    Contours inner = roundedRect(x0 + 1, y0 + 1, w - 2, h - 2, ri);
                    fillShape(inner, white);
                    ring.insert(ring.end(), inner.begin(), inner.end());
                    fillShape(ring, grey, true);
                }
                break;
            }
            const float cx = x0 + w / 2, cy = y0 + h / 2, rr = std::min(w, h) / 2;
            Contours outer_c = ellipse(cx, cy, rr, rr, kx);
            Contours inner_c = ellipse(cx, cy, rr - 1, rr - 1, kx);
            fillShape(inner_c, white);
            outer_c.insert(outer_c.end(), inner_c.begin(), inner_c.end());
            fillShape(outer_c, box.checked ? blue : grey, true);
            if (box.checked) fillShape(ellipse(cx, cy, rr * 0.55f, rr * 0.55f, kx), blue);
            break;
        }
        case BoxPaint::Part::Background: {
            Paint p;
            p.color = box.background;
            canvas.fill(outer, false, p);
            break;
        }
        case BoxPaint::Part::Border: {
            const Contours inner = scaled(insetShape(0, 0, box.w, box.h, box.radii, b[0], b[1], b[2], b[3]), kx, ky, box.ox, box.oy);
            Contours both = outer;
            both.insert(both.end(), inner.begin(), inner.end());
            const std::vector<float> ringcov = coverageOf(W, H, both, true);
            for (int side = 0; side < 4; ++side) {
                const BorderStyle st = box.styles[side];
                if (b[side] <= 0 || st == BorderStyle::None || st == BorderStyle::Hidden || !box.colors[side].visible()) continue;
                const std::vector<float> region = coverageOf(W, H, scaled(sideRegion(side, box.w, box.h, b), kx, ky, box.ox, box.oy), false);
                // What this side paints, before its region cuts it out.
                std::vector<std::pair<Contours, Color>> shapes;
                const Color c = box.colors[side];
                const auto ring = [&](float from, float to) {
                    // The band between `from` and `to` of the way in, as an even-odd pair.
                    Contours a = scaled(insetShape(0, 0, box.w, box.h, box.radii, b[0] * from, b[1] * from, b[2] * from, b[3] * from), kx, ky, box.ox, box.oy);
                    const Contours z = scaled(insetShape(0, 0, box.w, box.h, box.radii, b[0] * to, b[1] * to, b[2] * to, b[3] * to), kx, ky, box.ox, box.oy);
                    a.insert(a.end(), z.begin(), z.end());
                    return a;
                };
                bool even_odd = true;
                if (st == BorderStyle::Solid) {
                    shapes.push_back({ring(0, 1), c});
                } else if (st == BorderStyle::Inset || st == BorderStyle::Outset) {
                    shapes.push_back({ring(0, 1), styleColor(st, side, c)});
                } else if (st == BorderStyle::Groove || st == BorderStyle::Ridge) {
                    const BorderStyle out_half = st == BorderStyle::Groove ? BorderStyle::Inset : BorderStyle::Outset;
                    const BorderStyle in_half = st == BorderStyle::Groove ? BorderStyle::Outset : BorderStyle::Inset;
                    shapes.push_back({ring(0, 0.5f), styleColor(out_half, side, c)});
                    shapes.push_back({ring(0.5f, 1), styleColor(in_half, side, c)});
                } else if (st == BorderStyle::Double) {
                    if (b[side] < 3) shapes.push_back({ring(0, 1), c});
                    else {
                        shapes.push_back({ring(0, 1.0f / 3), c});
                        shapes.push_back({ring(2.0f / 3, 1), c});
                    }
                } else {
                    // Dashes or dots along the middle of the border.
                    even_odd = false;
                    const Walk walk(sideCentreLine(side, box.w, box.h, box.radii, b));
                    const float width = b[side], len = walk.length();
                    Contours marks;
                    // Blink's lengths: a dash twice the width (three times when thin), a gap
                    // once (twice); a dot the width with a gap the same. The gap then
                    // stretches or shrinks so whole dashes start and end the side.
                    const bool dots = st == BorderStyle::Dotted;
                    const float dash = dots ? width : width * (width >= 3 ? 2 : 3);
                    const float gap = dots ? width : width * (width >= 3 ? 1 : 2);
                    const float fewer = std::floor((len + gap) / (dash + gap));
                    const float more = fewer + 1;
                    const float gap_fewer = fewer > 1 ? (len - fewer * dash) / (fewer - 1) : 0;
                    const float gap_more = (len - more * dash) / (more - 1);
                    const bool use_more = gap_more > 0 && std::fabs(gap_more - gap) < std::fabs(gap_fewer - gap);
                    const int n = static_cast<int>(std::clamp(use_more ? more : fewer, 1.0f, 10000.0f));
                    const float real_gap = use_more ? gap_more : gap_fewer;
                    const StrokeStyle stroke{width, 0, 0, 4};
                    for (int i = 0; i < n; ++i) {
                        const float d0 = n == 1 ? std::max(0.0f, (len - dash) / 2) : i * (dash + real_gap);
                        if (dots && width >= 3) {
                            const Point p = walk.point(d0 + dash / 2);
                            for (auto& e : ellipse(p.x, p.y, width / 2, width / 2, kx)) marks.push_back(std::move(e));
                            continue;
                        }
                        const float d1 = std::min(len, d0 + dash);
                        for (auto& poly : strokeOutline(walk.piece(d0, d1), false, stroke, kx)) marks.push_back(std::move(poly));
                    }
                    shapes.push_back({scaled(marks, kx, ky, box.ox, box.oy), c});
                }
                // The side's region, within the ring: dashes mustn't stray outside the border.
                std::vector<float> cut = region;
                for (size_t i = 0; i < cut.size(); ++i) cut[i] *= ringcov[i];
                const Canvas mask = Canvas::fromAlpha(px_w, px_h, cut);
                for (const auto& [shape, color] : shapes) {
                    Canvas layer(px_w, px_h);
                    Paint p;
                    p.color = color;
                    layer.fill(shape, even_odd, p);
                    canvas.composite(layer, 1, &mask);
                }
            }
            break;
        }
        case BoxPaint::Part::OuterShadow:
        case BoxPaint::Part::InsetShadow: {
            const BoxShadow& sh = box.shadow;
            const float sigma = sh.blur / 2;
            std::vector<float> alpha;
            if (box.part == BoxPaint::Part::OuterShadow) {
                // The box grown by the spread and moved by the offset, blurred, and not under the box.
                const float s = sh.spread;
                float r[4][2];
                for (int i = 0; i < 4; ++i)
                    for (int j = 0; j < 2; ++j) r[i][j] = box.radii[i][j] > 0 ? std::max(0.0f, box.radii[i][j] + s) : 0;
                const Contours shape = scaled(roundedRect(-s, -s, box.w + 2 * s, box.h + 2 * s, r), kx, ky, box.ox + sh.x, box.oy + sh.y);
                alpha = coverageOf(W, H, shape, false);
                blurAlpha(alpha, W, H, sigma * kx);
                const std::vector<float> under = coverageOf(W, H, outer, false);
                for (size_t i = 0; i < alpha.size(); ++i) alpha[i] *= 1 - under[i];
            } else {
                // Inside the padding box: everywhere but the padding box shrunk by the spread and moved.
                const Contours padding = scaled(insetShape(0, 0, box.w, box.h, box.radii, b[0], b[1], b[2], b[3]), kx, ky, box.ox, box.oy);
                const float s = sh.spread;
                const float pw = box.w - b[1] - b[3], ph = box.h - b[0] - b[2];
                float r[4][2] = {{box.radii[0][0] - b[3] - s, box.radii[0][1] - b[0] - s}, {box.radii[1][0] - b[1] - s, box.radii[1][1] - b[0] - s},
                                 {box.radii[2][0] - b[1] - s, box.radii[2][1] - b[2] - s}, {box.radii[3][0] - b[3] - s, box.radii[3][1] - b[2] - s}};
                for (auto& c : r) {
                    c[0] = std::max(0.0f, c[0]);
                    c[1] = std::max(0.0f, c[1]);
                }
                const std::vector<float> hole = pw - 2 * s > 0 && ph - 2 * s > 0
                    ? coverageOf(W, H, scaled(roundedRect(b[3] + s, b[0] + s, pw - 2 * s, ph - 2 * s, r), kx, ky, box.ox + sh.x, box.oy + sh.y), false)
                    : std::vector<float>(static_cast<size_t>(W) * H, 0.0f);
                alpha.resize(hole.size());
                for (size_t i = 0; i < hole.size(); ++i) alpha[i] = 1 - hole[i];
                blurAlpha(alpha, W, H, sigma * kx, 1);
                const std::vector<float> inside = coverageOf(W, H, padding, false);
                for (size_t i = 0; i < alpha.size(); ++i) alpha[i] *= inside[i];
            }
            Raster out;
            out.width = px_w;
            out.height = px_h;
            out.rgba.resize(static_cast<size_t>(W) * H * 4);
            for (size_t i = 0; i < alpha.size(); ++i) {
                out.rgba[i * 4] = sh.color.r;
                out.rgba[i * 4 + 1] = sh.color.g;
                out.rgba[i * 4 + 2] = sh.color.b;
                out.rgba[i * 4 + 3] = toByte(std::clamp(alpha[i], 0.0f, 1.0f) * sh.color.a / 255.0f);
            }
            return out;
        }
    }
    return canvas.toRaster();
}

}  // namespace fbx::xweb
