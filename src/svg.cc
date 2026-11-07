// SVG, drawn from the document tree: the shapes, paths, groups, <use>, gradients and
// clip paths that icons and logos are made of. Styles come from the cascade, which has
// already turned presentation attributes into declarations; what an element doesn't set
// itself it inherits from whatever it's drawn inside, which for a <use> is the <use>.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_map>

#include "xweb/css.h"
#include "xweb/html_parser.h"
#include "xweb/paint.h"

namespace fbx::xweb {

namespace {

constexpr size_t kMaxSvgElements = 200'000;   // drawn, in one image
constexpr int kMaxSvgDepth = 256;
constexpr int kMaxLayerDepth = 8;   // nested group opacity and clip paths
constexpr size_t kMaxDashPieces = 100'000;

bool isSvg(const Node* n, std::string_view name) {
    return n && n->type == NodeType::Element && n->ns == Namespace::Svg && n->name == name;
}

// A number, maybe with a unit or percent. Percentages come back in `percent`.
bool parseLength(const std::string& text, float& value, bool& percent) {
    const char* s = text.c_str();
    while (*s == ' ' || *s == '\t' || *s == '\n') ++s;
    char* end = nullptr;
    value = std::strtof(s, &end);
    if (end == s || !std::isfinite(value)) return false;
    std::string unit(end);
    while (!unit.empty() && (unit.back() == ' ' || unit.back() == '\t' || unit.back() == '\n')) unit.pop_back();
    percent = false;
    if (unit.empty() || unit == "px") return true;
    if (unit == "%") { percent = true; return true; }
    if (unit == "em") { value *= 16; return true; }
    if (unit == "pt") { value *= 4.0f / 3.0f; return true; }
    if (unit == "pc") { value *= 16; return true; }
    if (unit == "in") { value *= 96; return true; }
    if (unit == "cm") { value *= 96 / 2.54f; return true; }
    if (unit == "mm") { value *= 96 / 25.4f; return true; }
    return false;
}

std::optional<float> lengthAttr(const Node* n, const char* name, float percent_of) {
    const std::string* v = n->attr(name);
    if (!v) return std::nullopt;
    float value = 0;
    bool percent = false;
    if (!parseLength(*v, value, percent)) return std::nullopt;
    return percent ? value * percent_of / 100.0f : value;
}

std::vector<float> numberList(const std::string& text) {
    std::vector<float> out;
    const char* s = text.c_str();
    while (*s && out.size() < 1'000'000) {
        while (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
        if (!*s) break;
        char* end = nullptr;
        const float v = std::strtof(s, &end);
        if (end == s || !std::isfinite(v)) break;
        out.push_back(v);
        s = end;
    }
    return out;
}

const std::string* hrefOf(const Node* n) {
    if (const std::string* h = n->attr("href")) return h;
    return n->attr("xlink:href");
}

struct ViewBox {
    float x = 0, y = 0, w = 0, h = 0;
};

std::optional<ViewBox> viewBoxOf(const Node* n) {
    const std::string* v = n->attr("viewBox");
    if (!v) return std::nullopt;
    const std::vector<float> nums = numberList(*v);
    if (nums.size() != 4 || nums[2] <= 0 || nums[3] <= 0) return std::nullopt;
    return ViewBox{nums[0], nums[1], nums[2], nums[3]};
}

// What viewBox and preserveAspectRatio make of a viewport this size.
Matrix viewBoxTransform(const Node* n, const ViewBox& vb, float width, float height) {
    std::string par = n->attr("preserveAspectRatio") ? *n->attr("preserveAspectRatio") : "xMidYMid meet";
    float sx = width / vb.w, sy = height / vb.h;
    if (par.find("none") != std::string::npos) return Matrix::scaling(sx, sy).after(Matrix::translate(-vb.x, -vb.y));
    const bool slice = par.find("slice") != std::string::npos;
    const float s = slice ? std::max(sx, sy) : std::min(sx, sy);
    float tx = -vb.x * s, ty = -vb.y * s;
    const float extra_w = width - vb.w * s, extra_h = height - vb.h * s;
    if (par.find("xMid") != std::string::npos) tx += extra_w / 2;
    else if (par.find("xMax") != std::string::npos) tx += extra_w;
    if (par.find("YMid") != std::string::npos) ty += extra_h / 2;
    else if (par.find("YMax") != std::string::npos) ty += extra_h;
    return {s, 0, 0, s, tx, ty};
}

// Polylines into dashes.
std::vector<std::vector<Point>> dashed(const std::vector<Point>& in, bool closed, const std::vector<float>& dashes, float offset) {
    std::vector<std::vector<Point>> out;
    float total = 0;
    for (float d : dashes) total += d;
    if (total <= 0 || in.size() < 2) return {in};
    std::vector<Point> pts = in;
    if (closed) pts.push_back(in.front());
    size_t idx = 0;
    float left = 0;
    offset = std::fmod(offset, total);
    if (offset < 0) offset += total;
    while (offset > 0) {
        if (offset >= dashes[idx]) {
            offset -= dashes[idx];
            idx = (idx + 1) % dashes.size();
        } else {
            left = dashes[idx] - offset;
            offset = 0;
            break;
        }
    }
    if (left == 0) left = dashes[idx];
    bool on = idx % 2 == 0;
    std::vector<Point> piece;
    if (on) piece.push_back(pts[0]);
    for (size_t i = 0; i + 1 < pts.size() && out.size() < kMaxDashPieces; ++i) {
        Point a = pts[i];
        const Point b = pts[i + 1];
        float seg = std::hypot(b.x - a.x, b.y - a.y);
        while (seg > 0 && out.size() < kMaxDashPieces) {
            if (left >= seg) {
                left -= seg;
                if (on) piece.push_back(b);
                seg = 0;
            } else {
                const float t = left / seg;
                const Point m{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                if (on) {
                    piece.push_back(m);
                    out.push_back(std::move(piece));
                    piece.clear();
                } else {
                    piece = {m};
                }
                on = !on;
                seg -= left;
                a = m;
                idx = (idx + 1) % dashes.size();
                left = dashes[idx];
            }
        }
    }
    if (on && piece.size() > 1) out.push_back(std::move(piece));
    return out;
}

// What's inherited as drawing goes down the tree.
struct State {
    Matrix m;
    SvgPaint fill{SvgPaint::Kind::Color, Color{0, 0, 0, 255}};
    SvgPaint stroke;
    float stroke_width = 1, miter = 4, dash_offset = 0, fill_opacity = 1, stroke_opacity = 1;
    bool even_odd = false, hidden = false;
    uint8_t cap = 0, join = 0;
    std::shared_ptr<const std::vector<float>> dashes;
    Color color{0, 0, 0, 255};
    float vp_w = 0, vp_h = 0;   // the nearest viewport, for percentages
};

struct Shape {
    std::vector<std::pair<std::vector<Point>, bool>> contours;   // user space; closed?
    bool fillable = true;
};

class Renderer {
public:
    Renderer(const Node& root, Canvas& canvas, const StyledDocument* styles) : canvas_(canvas), styles_(styles) {
        const Node* top = &root;
        while (top->parent) top = top->parent;
        std::vector<const Node*> todo{top};
        while (!todo.empty() && ids_.size() < 200'000) {
            const Node* n = todo.back();
            todo.pop_back();
            if (n->isElement())
                if (const std::string* id = n->attr("id")) ids_.emplace(*id, n);
            todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        }
    }

    void drawRoot(const Node& svg, float width, float height, Color color, const SvgSize* natural) {
        State s;
        s.color = color;
        s.vp_w = width;
        s.vp_h = height;
        if (auto vb = viewBoxOf(&svg)) {
            s.m = viewBoxTransform(&svg, *vb, width, height);
            s.vp_w = vb->w;
            s.vp_h = vb->h;
        } else if (natural && natural->width && natural->height && *natural->width > 0 && *natural->height > 0) {
            s.m = Matrix::scaling(width / *natural->width, height / *natural->height);
            s.vp_w = *natural->width;
            s.vp_h = *natural->height;
        }
        apply(&svg, s);
        children(&svg, s, 0, canvas_);
    }

private:
    const ComputedStyle* styleOf(const Node* n) const { return styles_ ? styles_->styleOf(n) : nullptr; }

    // An element's own settings over what it inherits.
    void apply(const Node* n, State& s) const {
        const ComputedStyle* st = styleOf(n);
        if (!st) {
            attributeFallback(n, s);
            return;
        }
        const uint32_t set = st->svg_set;
        if (set & kSetColor) s.color = st->color;
        if (set & kSetFill) s.fill = st->fill;
        if (set & kSetStroke) s.stroke = st->stroke;
        if (set & kSetStrokeWidth) s.stroke_width = st->stroke_width;
        if (set & kSetMiter) s.miter = st->stroke_miterlimit;
        if (set & kSetDashOffset) s.dash_offset = st->stroke_dashoffset;
        if (set & kSetFillOpacity) s.fill_opacity = st->fill_opacity;
        if (set & kSetStrokeOpacity) s.stroke_opacity = st->stroke_opacity;
        if (set & kSetFillRule) s.even_odd = st->fill_evenodd;
        if (set & kSetLinecap) s.cap = st->stroke_linecap;
        if (set & kSetLinejoin) s.join = st->stroke_linejoin;
        if (set & kSetDash) s.dashes = st->stroke_dasharray;
        if (set & kSetVisibility) s.hidden = st->hidden;
    }

    // No cascade for this element: its fill and stroke attributes, at least.
    static void attributeFallback(const Node* n, State& s) {
        const auto paint = [&](const char* name, SvgPaint& into) {
            const std::string* v = n->attr(name);
            if (!v) return;
            const std::string t = *v;
            if (t == "none") { into = {}; return; }
            if (t == "currentColor" || t == "currentcolor") { into = {SvgPaint::Kind::CurrentColor}; return; }
            if (t.starts_with("url(")) {
                const size_t hash = t.find('#'), close = t.find(')');
                if (hash != std::string::npos && close != std::string::npos && close > hash) into = {SvgPaint::Kind::Url, {}, t.substr(hash, close - hash)};
                return;
            }
            Color c;
            if (parseColor(css::tokenize(t), s.color, c)) into = {SvgPaint::Kind::Color, c};
        };
        paint("fill", s.fill);
        paint("stroke", s.stroke);
        if (auto w = lengthAttr(n, "stroke-width", 100)) s.stroke_width = std::max(0.0f, *w);
        if (const std::string* v = n->attr("fill-rule")) s.even_odd = *v == "evenodd";
    }

    float elementOpacity(const Node* n) const {
        const ComputedStyle* st = styleOf(n);
        if (st) return st->opacity;
        if (auto o = lengthAttr(n, "opacity", 1)) return std::clamp(*o, 0.0f, 1.0f);
        return 1;
    }

    bool displayed(const Node* n) const {
        const ComputedStyle* st = styleOf(n);
        if (st) return st->display != Display::None;
        const std::string* d = n->attr("display");
        return !d || *d != "none";
    }

    void children(const Node* n, const State& s, int depth, Canvas& into) {
        for (const Node* c : n->children) {
            if (!c->isElement() || c->ns != Namespace::Svg) continue;
            element(c, s, depth + 1, into);
            if (isSvg(n, "switch")) break;   // the first it can draw: we can draw them all
        }
    }

    void element(const Node* n, const State& parent, int depth, Canvas& into) {
        if (depth > kMaxSvgDepth || ++drawn_ > kMaxSvgElements) return;
        const std::string& name = n->name;
        if (name == "defs" || name == "symbol" || name == "clipPath" || name == "mask" || name == "linearGradient" ||
            name == "radialGradient" || name == "pattern" || name == "marker" || name == "style" || name == "script" ||
            name == "title" || name == "desc" || name == "metadata" || name == "filter" || name == "foreignObject" || name == "text" ||
            name == "image")
            return;
        if (!displayed(n)) return;
        State s = parent;
        apply(n, s);
        if (const std::string* t = n->attr("transform")) s.m = s.m.after(parseTransform(*t));

        // Opacity on a group, or a clip path, means drawing it apart first.
        const float opacity = elementOpacity(n);
        const ComputedStyle* st = styleOf(n);
        std::string clip_id = st ? st->clip_path : std::string{};
        if (!st)
            if (const std::string* cp = n->attr("clip-path"); cp && cp->find('#') != std::string::npos) {
                const size_t h = cp->find('#'), e = cp->find(')', h);
                clip_id = cp->substr(h, e == std::string::npos ? std::string::npos : e - h);
            }
        const Node* clip = clip_id.size() > 1 ? lookup(clip_id) : nullptr;
        if (clip && !isSvg(clip, "clipPath")) clip = nullptr;
        if (opacity <= 0) return;
        const bool container = name == "g" || name == "svg" || name == "a" || name == "switch" || name == "use";
        const bool layered = (clip || (container && opacity < 1)) && layers_ < kMaxLayerDepth;
        if (layered) {
            ++layers_;
            Canvas layer(into.width(), into.height());
            drawElement(n, s, depth, layer, container ? 1.0f : opacity);
            std::optional<Canvas> mask;
            if (clip) mask = clipCoverage(clip, s, depth);
            into.composite(layer, container ? opacity : 1.0f, mask ? &*mask : nullptr);
            --layers_;
            return;
        }
        drawElement(n, s, depth, into, container ? 1.0f : opacity);
    }

    void drawElement(const Node* n, State& s, int depth, Canvas& into, float opacity) {
        const std::string& name = n->name;
        if (name == "g" || name == "a" || name == "switch") {
            children(n, s, depth, into);
            return;
        }
        if (name == "svg") {
            // A nested viewport.
            const float x = lengthAttr(n, "x", s.vp_w).value_or(0), y = lengthAttr(n, "y", s.vp_h).value_or(0);
            const float w = lengthAttr(n, "width", s.vp_w).value_or(s.vp_w), h = lengthAttr(n, "height", s.vp_h).value_or(s.vp_h);
            if (w <= 0 || h <= 0) return;
            s.m = s.m.after(Matrix::translate(x, y));
            s.vp_w = w;
            s.vp_h = h;
            if (auto vb = viewBoxOf(n)) {
                s.m = s.m.after(viewBoxTransform(n, *vb, w, h));
                s.vp_w = vb->w;
                s.vp_h = vb->h;
            }
            children(n, s, depth, into);
            return;
        }
        if (name == "use") {
            const std::string* href = hrefOf(n);
            if (!href || href->empty() || (*href)[0] != '#') return;
            const Node* target = lookup(*href);
            if (!target || target->ns != Namespace::Svg || active_.count(target) || active_.size() > 32) return;
            for (const Node* up = n; up; up = up->parent)
                if (up == target) return;   // a <use> inside what it uses
            active_.insert(target);
            const float x = lengthAttr(n, "x", s.vp_w).value_or(0), y = lengthAttr(n, "y", s.vp_h).value_or(0);
            s.m = s.m.after(Matrix::translate(x, y));
            if (target->name == "symbol" || target->name == "svg") {
                const float w = lengthAttr(n, "width", s.vp_w).value_or(lengthAttr(target, "width", s.vp_w).value_or(s.vp_w));
                const float h = lengthAttr(n, "height", s.vp_h).value_or(lengthAttr(target, "height", s.vp_h).value_or(s.vp_h));
                if (w > 0 && h > 0 && displayedAsUsed(target)) {
                    State inner = s;
                    apply(target, inner);
                    if (const std::string* t = target->attr("transform"); t && target->name == "svg") inner.m = inner.m.after(parseTransform(*t));
                    if (auto vb = viewBoxOf(target)) {
                        inner.m = inner.m.after(viewBoxTransform(target, *vb, w, h));
                        inner.vp_w = vb->w;
                        inner.vp_h = vb->h;
                    } else {
                        inner.vp_w = w;
                        inner.vp_h = h;
                    }
                    children(target, inner, depth + 1, into);
                }
            } else {
                element(target, s, depth + 1, into);
            }
            active_.erase(target);
            return;
        }
        Shape shape = shapeOf(n, s);
        if (shape.contours.empty()) return;
        if (s.hidden) return;
        drawShape(shape, s, into, opacity);
    }

    // A symbol is drawn by <use> even though it isn't drawn where it is.
    bool displayedAsUsed(const Node* target) const {
        const ComputedStyle* st = styleOf(target);
        if (!st) return true;
        return st->display != Display::None || target->name == "symbol";
    }

    const Node* lookup(const std::string& ref) const {
        if (ref.size() < 2 || ref[0] != '#') return nullptr;
        const auto it = ids_.find(ref.substr(1));
        return it == ids_.end() ? nullptr : it->second;
    }

    Shape shapeOf(const Node* n, const State& s) const {
        Shape out;
        const std::string& name = n->name;
        const float diag = std::sqrt((s.vp_w * s.vp_w + s.vp_h * s.vp_h) / 2);
        const float scale = s.m.scale();
        if (name == "path") {
            if (const std::string* d = n->attr("d")) out.contours = parsePathData(*d, scale);
        } else if (name == "rect") {
            const float x = lengthAttr(n, "x", s.vp_w).value_or(0), y = lengthAttr(n, "y", s.vp_h).value_or(0);
            const float w = lengthAttr(n, "width", s.vp_w).value_or(0), h = lengthAttr(n, "height", s.vp_h).value_or(0);
            if (w <= 0 || h <= 0) return out;
            std::optional<float> rx = lengthAttr(n, "rx", s.vp_w), ry = lengthAttr(n, "ry", s.vp_h);
            if (rx && *rx < 0) rx.reset();
            if (ry && *ry < 0) ry.reset();
            if (rx && !ry) ry = rx;
            if (ry && !rx) rx = ry;
            Contours c = roundedRect(x, y, w, h, rx.value_or(0), ry.value_or(0));
            if (!c.empty()) out.contours.push_back({c[0], true});
        } else if (name == "circle" || name == "ellipse") {
            const float cx = lengthAttr(n, "cx", s.vp_w).value_or(0), cy = lengthAttr(n, "cy", s.vp_h).value_or(0);
            float rx = 0, ry = 0;
            if (name == "circle") {
                rx = ry = lengthAttr(n, "r", diag).value_or(0);
            } else {
                std::optional<float> a = lengthAttr(n, "rx", s.vp_w), b = lengthAttr(n, "ry", s.vp_h);
                if (a && !b) b = a;
                if (b && !a) a = b;
                rx = a.value_or(0);
                ry = b.value_or(0);
            }
            if (rx <= 0 || ry <= 0) return out;
            Contours c = ellipse(cx, cy, rx, ry, scale);
            if (!c.empty()) out.contours.push_back({c[0], true});
        } else if (name == "line") {
            const Point a{lengthAttr(n, "x1", s.vp_w).value_or(0), lengthAttr(n, "y1", s.vp_h).value_or(0)};
            const Point b{lengthAttr(n, "x2", s.vp_w).value_or(0), lengthAttr(n, "y2", s.vp_h).value_or(0)};
            out.contours.push_back({{a, b}, false});
            out.fillable = false;
        } else if (name == "polyline" || name == "polygon") {
            const std::string* p = n->attr("points");
            if (!p) return out;
            std::vector<float> nums = numberList(*p);
            if (nums.size() % 2) nums.pop_back();
            std::vector<Point> pts;
            for (size_t i = 0; i + 1 < nums.size(); i += 2) pts.push_back({nums[i], nums[i + 1]});
            if (pts.size() >= 2) out.contours.push_back({std::move(pts), name == "polygon"});
        }
        return out;
    }

    static void bounds(const Shape& shape, float& x0, float& y0, float& x1, float& y1) {
        x0 = y0 = 1e30f;
        x1 = y1 = -1e30f;
        for (const auto& [pts, closed] : shape.contours)
            for (const Point& p : pts) {
                x0 = std::min(x0, p.x);
                y0 = std::min(y0, p.y);
                x1 = std::max(x1, p.x);
                y1 = std::max(y1, p.y);
            }
    }

    // A paint as something the canvas can fill with, or nothing.
    std::optional<Paint> paintFor(const SvgPaint& p, const State& s, const Shape& shape, float opacity) const {
        Paint out;
        out.opacity = opacity;
        switch (p.kind) {
            case SvgPaint::Kind::None: return std::nullopt;
            case SvgPaint::Kind::Color: out.color = p.color; return out;
            case SvgPaint::Kind::CurrentColor: out.color = s.color; return out;
            case SvgPaint::Kind::Url: break;
        }
        const Node* server = lookup(p.url);
        if (server && (isSvg(server, "linearGradient") || isSvg(server, "radialGradient"))) {
            float x0, y0, x1, y1;
            bounds(shape, x0, y0, x1, y1);
            auto g = gradientFor(server, s, x0, y0, x1 - x0, y1 - y0);
            if (g) {
                if (g->stops.size() == 1) {
                    out.color = g->stops[0].color;
                    return out;
                }
                out.gradient = std::move(g);
                return out;
            }
            if (gradient_none_) return std::nullopt;
        }
        switch (p.fallback) {
            case SvgPaint::Kind::Color: out.color = p.fallback_color; return out;
            case SvgPaint::Kind::CurrentColor: out.color = s.color; return out;
            default: return std::nullopt;
        }
    }

    // A gradient element, with what it inherits by href, mapped for this shape.
    std::shared_ptr<GradientPaint> gradientFor(const Node* el, const State& s, float bx, float by, float bw, float bh) const {
        gradient_none_ = false;
        std::vector<const Node*> chain;
        for (const Node* g = el; g && chain.size() < 16; ) {
            if (std::find(chain.begin(), chain.end(), g) != chain.end()) break;
            chain.push_back(g);
            const std::string* h = hrefOf(g);
            const Node* next = h ? lookup(*h) : nullptr;
            g = next && (isSvg(next, "linearGradient") || isSvg(next, "radialGradient")) ? next : nullptr;
        }
        const auto attr = [&](const char* name) -> const std::string* {
            for (const Node* g : chain)
                if (const std::string* v = g->attr(name)) return v;
            return nullptr;
        };
        const Node* stops_from = nullptr;
        for (const Node* g : chain) {
            for (const Node* c : g->children)
                if (isSvg(c, "stop")) { stops_from = g; break; }
            if (stops_from) break;
        }
        auto out = std::make_shared<GradientPaint>();
        if (stops_from) {
            float last = 0;
            for (const Node* c : stops_from->children) {
                if (!isSvg(c, "stop")) continue;
                float offset = 0;
                if (const std::string* o = c->attr("offset")) {
                    bool pct = false;
                    if (parseLength(*o, offset, pct) && pct) offset /= 100;
                }
                offset = std::clamp(offset, last, 1.0f);
                last = offset;
                Color color{0, 0, 0, 255};
                float stop_opacity = 1;
                if (const ComputedStyle* st = styleOf(c)) {
                    color = st->stop_color;
                    stop_opacity = st->stop_opacity;
                } else {
                    if (const std::string* sc = c->attr("stop-color")) (void)parseColor(css::tokenize(*sc), s.color, color);
                    if (auto so = lengthAttr(c, "stop-opacity", 1)) stop_opacity = std::clamp(*so, 0.0f, 1.0f);
                }
                color.a = static_cast<uint8_t>(std::lround(color.a * stop_opacity));
                out->stops.push_back({offset, color});
            }
        }
        if (out->stops.empty()) {
            gradient_none_ = true;   // no stops: as if painted none
            return nullptr;
        }
        const std::string* units = attr("gradientUnits");
        const bool user_space = units && *units == "userSpaceOnUse";
        if (!user_space && (bw <= 0 || bh <= 0)) return nullptr;
        const float pw = user_space ? s.vp_w : 1, ph = user_space ? s.vp_h : 1;
        const float pd = user_space ? std::sqrt((s.vp_w * s.vp_w + s.vp_h * s.vp_h) / 2) : 1;
        const auto len = [&](const char* name, float def, float of) {
            const std::string* v = attr(name);
            if (!v) return def;
            float value = 0;
            bool pct = false;
            if (!parseLength(*v, value, pct)) return def;
            return pct ? value * of / 100.0f : value;
        };
        if (el->name == "linearGradient") {
            out->kind = GradientPaint::Kind::Linear;
            out->x1 = len("x1", 0, pw);
            out->y1 = len("y1", 0, ph);
            out->x2 = len("x2", pw, pw);
            out->y2 = len("y2", 0, ph);
        } else {
            out->kind = GradientPaint::Kind::Radial;
            out->cx = len("cx", pw / 2, pw);
            out->cy = len("cy", ph / 2, ph);
            out->r = len("r", pd / 2, pd);
            out->fx = len("fx", out->cx, pw);
            out->fy = len("fy", out->cy, ph);
            if (out->r <= 0) {
                out->stops = {out->stops.back()};
                return out;
            }
        }
        if (const std::string* sp = attr("spreadMethod")) {
            if (*sp == "repeat") out->spread = GradientPaint::Spread::Repeat;
            else if (*sp == "reflect") out->spread = GradientPaint::Spread::Reflect;
        }
        Matrix m = s.m;
        if (!user_space) m = m.after(Matrix::translate(bx, by)).after(Matrix::scaling(bw, bh));
        if (const std::string* t = attr("gradientTransform")) m = m.after(parseTransform(*t));
        out->to_canvas = m;
        return out;
    }

    void drawShape(const Shape& shape, const State& s, Canvas& into, float opacity) const {
        const auto toCanvas = [&](const std::vector<Point>& pts) {
            std::vector<Point> out;
            out.reserve(pts.size());
            for (const Point& p : pts) out.push_back(s.m.apply(p));
            return out;
        };
        if (shape.fillable) {
            if (auto paint = paintFor(s.fill, s, shape, s.fill_opacity * opacity)) {
                Contours c;
                for (const auto& [pts, closed] : shape.contours)
                    if (pts.size() >= 3) c.push_back(toCanvas(pts));
                into.fill(c, s.even_odd, *paint);
            }
        }
        if (s.stroke_width > 0) {
            if (auto paint = paintFor(s.stroke, s, shape, s.stroke_opacity * opacity)) {
                StrokeStyle style{s.stroke_width, s.cap, s.join, s.miter};
                Contours c;
                const float scale = s.m.scale();
                for (const auto& [pts, closed] : shape.contours) {
                    std::vector<std::vector<Point>> pieces;
                    if (s.dashes && !s.dashes->empty()) pieces = dashed(pts, closed, *s.dashes, s.dash_offset);
                    const bool whole = pieces.empty();
                    if (whole) pieces.push_back(pts);
                    for (const auto& piece : pieces)
                        for (auto& poly : strokeOutline(piece, whole && closed, style, scale)) c.push_back(toCanvas(poly));
                }
                into.fill(c, false, *paint);
            }
        }
    }

    Canvas clipCoverage(const Node* clip, const State& s, int depth) {
        Matrix m = s.m;
        if (const std::string* t = clip->attr("transform")) m = m.after(parseTransform(*t));
        Canvas mask(canvas_.width(), canvas_.height());
        Contours all;
        bool even_odd = false;
        for (const Node* c : clip->children) {
            if (!c->isElement() || c->ns != Namespace::Svg || !displayed(c) || depth > kMaxSvgDepth) continue;
            const Node* shape_el = c;
            Matrix cm = m;
            if (const std::string* t = c->attr("transform")) cm = cm.after(parseTransform(*t));
            if (c->name == "use") {
                const std::string* href = hrefOf(c);
                shape_el = href ? lookup(*href) : nullptr;
                if (!shape_el || shape_el == clip) continue;
                cm = cm.after(Matrix::translate(lengthAttr(c, "x", s.vp_w).value_or(0), lengthAttr(c, "y", s.vp_h).value_or(0)));
            }
            State cs = s;
            cs.m = cm;
            if (const ComputedStyle* st = styleOf(shape_el)) even_odd = st->fill_evenodd;
            for (const auto& [pts, closed] : shapeOf(shape_el, cs).contours) {
                std::vector<Point> out;
                for (const Point& p : pts) out.push_back(cm.apply(p));
                all.push_back(std::move(out));
            }
        }
        return Canvas::coverage(canvas_.width(), canvas_.height(), all, even_odd);
    }

    Canvas& canvas_;
    const StyledDocument* styles_;
    std::unordered_map<std::string, const Node*> ids_;
    std::set<const Node*> active_;
    size_t drawn_ = 0;
    int layers_ = 0;
    mutable bool gradient_none_ = false;
};

}  // namespace

SvgSize svgNaturalSize(const Node& svg) {
    SvgSize out;
    const auto size_attr = [&](const char* name) -> std::optional<float> {
        const std::string* v = svg.attr(name);
        if (!v) return std::nullopt;
        float value = 0;
        bool pct = false;
        if (!parseLength(*v, value, pct) || pct || value < 0 || value > 1e6f) return std::nullopt;
        return value;
    };
    out.width = size_attr("width");
    out.height = size_attr("height");
    if (auto vb = viewBoxOf(&svg)) out.ratio = vb->w / vb->h;
    else if (out.width && out.height && *out.height > 0) out.ratio = *out.width / *out.height;
    return out;
}

const Node* findSvgRoot(const Document& doc) {
    std::vector<const Node*> todo{doc.root()};
    while (!todo.empty()) {
        const Node* n = todo.back();
        todo.pop_back();
        if (isSvg(n, "svg")) return n;
        todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
    }
    return nullptr;
}

Raster renderSvg(const Node& svg, uint32_t width, uint32_t height, const StyledDocument* styles, Color color, const SvgSize* natural) {
    if (width == 0 || height == 0 || width > kMaxRasterSide || height > kMaxRasterSide ||
        static_cast<uint64_t>(width) * height > kMaxRasterPixels)
        return {};
    Canvas canvas(width, height);
    Renderer r(svg, canvas, styles);
    r.drawRoot(svg, static_cast<float>(width), static_cast<float>(height), color, natural);
    return canvas.toRaster();
}

std::shared_ptr<const SvgImage> SvgImage::parse(std::string_view text) {
    auto out = std::make_shared<SvgImage>();
    // The HTML parser reads SVG well enough: <svg> takes it into foreign content, where
    // self-closing tags and CDATA mean what they do in XML.
    std::string html(text);
    if (html.starts_with("<?xml")) {
        const size_t end = html.find("?>");
        if (end != std::string::npos) html.erase(0, end + 2);
    }
    out->doc_ = parseHtml(html);
    out->root_ = findSvgRoot(*out->doc_);
    if (!out->root_) return nullptr;
    out->size_ = svgNaturalSize(*out->root_);
    const float w = out->size_.width.value_or(300), h = out->size_.height.value_or(150);
    out->styles_ = std::make_unique<StyledDocument>(*out->doc_, css::MediaContext{w, h});
    return out;
}

Raster SvgImage::render(uint32_t width, uint32_t height) const {
    return renderSvg(*root_, width, height, styles_.get(), Color{0, 0, 0, 255}, &size_);
}

bool SvgImage::sniff(std::string_view bytes, std::string_view content_type, std::string_view url) {
    if (content_type.starts_with("image/svg")) return true;
    std::string_view path = url.substr(0, url.find_first_of("?#"));
    if (path.size() >= 4 && (path.ends_with(".svg") || path.ends_with(".SVG"))) return true;
    const std::string_view head = bytes.substr(0, 512);
    return head.find("<svg") != std::string_view::npos;
}

}  // namespace fbx::xweb
