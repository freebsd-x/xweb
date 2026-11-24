#include "xweb/animation.h"

#include "xweb/style.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace fbx::xweb {

namespace {

constexpr size_t kMaxEntries = 2000;

float mixed(float a, float b, float t) { return a + (b - a) * t; }
Length mixed(const Length& a, const Length& b, float t) {
    if (a.is_auto || b.is_auto) return t < 0.5f ? a : b;
    Length l;
    l.px = mixed(a.px, b.px, t);
    l.percent = mixed(a.percent, b.percent, t);
    return l;
}
Color mixed(Color a, Color b, float t) {
    // By premultiplied values: from transparent, a colour fades in as itself.
    const float aa = a.a / 255.0f, ba = b.a / 255.0f, oa = std::clamp(mixed(aa, ba, t), 0.0f, 1.0f);
    if (oa <= 0) return {0, 0, 0, 0};
    const auto channel = [&](uint8_t x, uint8_t y) {
        return static_cast<uint8_t>(std::lround(std::clamp(mixed(x * aa, y * ba, t) / oa, 0.0f, 255.0f)));
    };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), static_cast<uint8_t>(std::lround(oa * 255))};
}
bool same(const Length& a, const Length& b) { return a.is_auto == b.is_auto && a.px == b.px && a.percent == b.percent; }
bool between(const Length& a, const Length& b) { return !same(a, b) && !a.is_auto && !b.is_auto; }

// Function lists that match, function by function, are gone between as they are (a whole
// turn is one); others by the matrices they come to.
void moveTransform(ComputedStyle& out, const ComputedStyle& a, const ComputedStyle& b, float t) {
    using List = std::vector<TransformFn>;
    static const List kNone;
    const List& la = a.transform_list ? *a.transform_list : kNone;
    const List& lb = b.transform_list ? *b.transform_list : kNone;
    const bool fits = (la.empty() || lb.empty() || la.size() == lb.size()) &&
                      (la.empty() || lb.empty() || std::equal(la.begin(), la.end(), lb.begin(), [](const TransformFn& x, const TransformFn& y) { return x.kind == y.kind && x.kind != TransformFn::Kind::Matrix; }));
    if (fits && (!la.empty() || !lb.empty())) {
        auto l = std::make_shared<List>(la.empty() ? lb : la);
        for (size_t i = 0; i < l->size(); ++i) {
            const TransformFn none = TransformFn::identity((*l)[i].kind);
            const TransformFn& x = la.empty() ? none : la[i];
            const TransformFn& y = lb.empty() ? none : lb[i];
            TransformFn& f = (*l)[i];
            f.x = mixed(x.x, y.x, t);
            f.y = mixed(x.y, y.y, t);
            for (int k = 0; k < 4; ++k) f.v[k] = mixed(x.v[k], y.v[k], t);
        }
        evaluateTransform(*l, out.transform_shift, out.transform_linear);
        out.transform_list = std::move(l);
        return;
    }
    for (int i = 0; i < 2; ++i) out.transform_shift[i] = mixed(a.transform_shift[i], b.transform_shift[i], t);
    for (int i = 0; i < 4; ++i) out.transform_linear[i] = mixed(a.transform_linear[i], b.transform_linear[i], t);
    out.transform_list.reset();
}

bool sameTransform(const ComputedStyle& a, const ComputedStyle& b) {
    if (a.transform_list != b.transform_list) {
        const size_t na = a.transform_list ? a.transform_list->size() : 0, nb = b.transform_list ? b.transform_list->size() : 0;
        if (na != nb) return false;
        for (size_t i = 0; i < na; ++i) {
            const TransformFn &x = (*a.transform_list)[i], &y = (*b.transform_list)[i];
            if (x.kind != y.kind || !same(x.x, y.x) || !same(x.y, y.y) || !std::equal(std::begin(x.v), std::end(x.v), std::begin(y.v))) return false;
        }
    }
    return same(a.transform_shift[0], b.transform_shift[0]) && same(a.transform_shift[1], b.transform_shift[1]) &&
           std::equal(std::begin(a.transform_linear), std::end(a.transform_linear), std::begin(b.transform_linear));
}

bool sameFilter(const ComputedStyle& a, const ComputedStyle& b) {
    const size_t na = a.filter ? a.filter->size() : 0, nb = b.filter ? b.filter->size() : 0;
    if (na != nb) return false;
    for (size_t i = 0; i < na; ++i)
        if ((*a.filter)[i].kind != (*b.filter)[i].kind || (*a.filter)[i].amount != (*b.filter)[i].amount) return false;
    return true;
}

}  // namespace

float Easing::at(float t) const noexcept {
    if (kind == Kind::Linear) return t;
    if (kind == Kind::Steps) {
        const float n = static_cast<float>(std::max(1, steps));
        const float k = std::floor(t * n + (jump == 0 || jump == 3 ? 1.0f : 0.0f));
        const float of = jump == 2 ? std::max(1.0f, n - 1) : jump == 3 ? n + 1 : n;
        return std::clamp(k / of, 0.0f, 1.0f);
    }
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    const auto curve = [](float p1, float p2, float u) {
        const float v = 1 - u;
        return 3 * v * v * u * p1 + 3 * v * u * u * p2 + u * u * u;
    };
    float lo = 0, hi = 1, u = t;
    for (int i = 0; i < 24; ++i) {
        const float x = curve(x1, x2, u);
        if (std::abs(x - t) < 1e-4f) break;
        (x < t ? lo : hi) = u;
        u = (lo + hi) / 2;
    }
    return curve(y1, y2, u);
}

bool easingAt(const std::vector<css::Token>& v, size_t& i, Easing& out) {
    using css::TokenType;
    const auto cubic = [&](float a, float b, float c, float d) {
        out = Easing{};
        out.x1 = a, out.y1 = b, out.x2 = c, out.y2 = d;
        return true;
    };
    if (i >= v.size()) return false;
    const auto low = [](std::string s) {
        for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        return s;
    };
    const std::string name = low(v[i].text);
    if (v[i].type == TokenType::Ident) {
        ++i;
        if (name == "ease") return cubic(0.25f, 0.1f, 0.25f, 1);
        if (name == "ease-in") return cubic(0.42f, 0, 1, 1);
        if (name == "ease-out") return cubic(0, 0, 0.58f, 1);
        if (name == "ease-in-out") return cubic(0.42f, 0, 0.58f, 1);
        out = Easing{};
        if (name == "linear") { out.kind = Easing::Kind::Linear; return true; }
        if (name == "step-start" || name == "step-end") {
            out.kind = Easing::Kind::Steps;
            out.jump = name == "step-start" ? 0 : 1;
            return true;
        }
        --i;
        return false;
    }
    if (v[i].type != TokenType::Function) return false;
    const size_t begin = i++;
    std::vector<float> numbers;
    std::string word;
    int depth = 1;
    while (i < v.size() && depth > 0) {
        if (v[i].type == TokenType::Function || v[i].type == TokenType::LeftParen) ++depth;
        else if (v[i].type == TokenType::RightParen) --depth;
        else if (v[i].type == TokenType::Number) numbers.push_back(static_cast<float>(v[i].number));
        else if (v[i].type == TokenType::Ident) word = low(v[i].text);
        ++i;
    }
    if (name == "cubic-bezier" && numbers.size() == 4) return cubic(std::clamp(numbers[0], 0.0f, 1.0f), numbers[1], std::clamp(numbers[2], 0.0f, 1.0f), numbers[3]);
    out = Easing{};
    if (name == "steps" && !numbers.empty() && numbers[0] >= 1) {
        out.kind = Easing::Kind::Steps;
        out.steps = static_cast<int>(std::min(numbers[0], 10000.0f));
        out.jump = word == "start" || word == "jump-start" ? 0 : word == "jump-none" ? 2 : word == "jump-both" ? 3 : 1;
        return true;
    }
    if (name == "linear") { out.kind = Easing::Kind::Linear; return true; }   // (its points aside)
    i = begin;
    return false;
}

uint32_t movingBits(const std::string& p) {
    static const std::map<std::string, uint32_t> kBits = {
        {"all", kMoveAll}, {"opacity", kMoveOpacity}, {"transform", kMoveTransform}, {"-webkit-transform", kMoveTransform}, {"translate", kMoveTranslate},
        {"rotate", kMoveRotate}, {"scale", kMoveScale}, {"color", kMoveColor}, {"background-color", kMoveBackground}, {"background", kMoveBackground},
        {"width", kMoveWidth}, {"inline-size", kMoveWidth}, {"height", kMoveHeight}, {"block-size", kMoveHeight},
        {"min-width", kMoveMinMax}, {"min-height", kMoveMinMax}, {"max-width", kMoveMaxWidth}, {"max-height", kMoveMaxHeight},
        {"top", kMoveTop}, {"right", kMoveRight}, {"bottom", kMoveBottom}, {"left", kMoveLeft}, {"inset", kMoveTop | kMoveRight | kMoveBottom | kMoveLeft},
        {"visibility", kMoveVisibility}, {"font-size", kMoveFontSize}, {"filter", kMoveFilter}, {"-webkit-filter", kMoveFilter}};
    if (const auto it = kBits.find(p); it != kBits.end()) return it->second;
    if (p.starts_with("margin")) return kMoveMargin;
    if (p.starts_with("padding")) return kMovePadding;
    if (p.ends_with("radius")) return kMoveRadius;
    if (p == "border" || (p.starts_with("border") && p.ends_with("color"))) return kMoveBorderColor;
    return 0;
}

uint32_t movingDiff(const ComputedStyle& a, const ComputedStyle& b) {
    uint32_t d = 0;
    if (a.opacity != b.opacity) d |= kMoveOpacity;
    if (!sameTransform(a, b)) d |= kMoveTransform;
    if (!same(a.translate_shift[0], b.translate_shift[0]) || !same(a.translate_shift[1], b.translate_shift[1])) d |= kMoveTranslate;
    if (a.rotate_deg != b.rotate_deg) d |= kMoveRotate;
    if (a.scale_prop[0] != b.scale_prop[0] || a.scale_prop[1] != b.scale_prop[1]) d |= kMoveScale;
    if (!(a.color == b.color)) d |= kMoveColor;
    if (!(a.background == b.background)) d |= kMoveBackground;
    if (between(a.width, b.width)) d |= kMoveWidth;
    if (between(a.height, b.height)) d |= kMoveHeight;
    if (between(a.min_width, b.min_width) || between(a.min_height, b.min_height)) d |= kMoveMinMax;
    if (between(a.max_width, b.max_width)) d |= kMoveMaxWidth;
    if (between(a.max_height, b.max_height)) d |= kMoveMaxHeight;
    for (int i = 0; i < 4; ++i) {
        if (between(a.margin[i], b.margin[i])) d |= kMoveMargin;
        if (between(a.padding[i], b.padding[i])) d |= kMovePadding;
        if (between(a.offset[i], b.offset[i])) d |= kMoveTop << i;
        if (!(a.border_color[i] == b.border_color[i])) d |= kMoveBorderColor;
        if (!same(a.radius[i][0], b.radius[i][0]) || !same(a.radius[i][1], b.radius[i][1])) d |= kMoveRadius;
    }
    if (a.hidden != b.hidden) d |= kMoveVisibility;
    if (a.font_size != b.font_size) d |= kMoveFontSize;
    if (!sameFilter(a, b)) d |= kMoveFilter;
    return d;
}

void moveBetween(ComputedStyle& out, const ComputedStyle& a, const ComputedStyle& b, float t, uint32_t bits) {
    if (bits & kMoveOpacity) out.opacity = std::clamp(mixed(a.opacity, b.opacity, t), 0.0f, 1.0f);
    if (bits & kMoveTransform) {
        moveTransform(out, a, b, t);
        out.makes_stacking_context = true;
    }
    if (bits & kMoveTranslate)
        for (int i = 0; i < 2; ++i) out.translate_shift[i] = mixed(a.translate_shift[i], b.translate_shift[i], t);
    if (bits & kMoveRotate) out.rotate_deg = mixed(a.rotate_deg, b.rotate_deg, t);
    if (bits & kMoveScale)
        for (int i = 0; i < 2; ++i) out.scale_prop[i] = mixed(a.scale_prop[i], b.scale_prop[i], t);
    if (bits & kMoveColor) out.color = mixed(a.color, b.color, t);
    if (bits & kMoveBackground) out.background = mixed(a.background, b.background, t);
    if (bits & kMoveWidth) out.width = mixed(a.width, b.width, t);
    if (bits & kMoveHeight) out.height = mixed(a.height, b.height, t);
    if (bits & kMoveMinMax) {
        out.min_width = mixed(a.min_width, b.min_width, t);
        out.min_height = mixed(a.min_height, b.min_height, t);
    }
    if (bits & kMoveMaxWidth) out.max_width = mixed(a.max_width, b.max_width, t);
    if (bits & kMoveMaxHeight) out.max_height = mixed(a.max_height, b.max_height, t);
    const auto positive = [](Length l) {
        if (!l.hasPercent()) l.px = std::max(0.0f, l.px);
        return l;
    };
    for (int i = 0; i < 4; ++i) {
        if (bits & kMoveMargin) out.margin[i] = mixed(a.margin[i], b.margin[i], t);
        if (bits & kMovePadding) out.padding[i] = positive(mixed(a.padding[i], b.padding[i], t));
        if (bits & (kMoveTop << i)) out.offset[i] = mixed(a.offset[i], b.offset[i], t);
        if (bits & kMoveBorderColor) out.border_color[i] = mixed(a.border_color[i], b.border_color[i], t);
        if (bits & kMoveRadius)
            for (int k = 0; k < 2; ++k) out.radius[i][k] = positive(mixed(a.radius[i][k], b.radius[i][k], t));
    }
    if (bits & (kMoveWidth | kMoveHeight)) out.width = positive(out.width), out.height = positive(out.height);
    if (bits & kMoveVisibility) out.hidden = t <= 0 ? a.hidden : t >= 1 ? b.hidden : a.hidden && b.hidden;
    if (bits & kMoveFontSize) out.font_size = std::max(0.0f, mixed(a.font_size, b.font_size, t));
    if (bits & kMoveFilter) {
        const size_t na = a.filter ? a.filter->size() : 0, nb = b.filter ? b.filter->size() : 0;
        bool fits = na == nb;
        for (size_t i = 0; fits && i < na; ++i) fits = (*a.filter)[i].kind == (*b.filter)[i].kind;
        if (fits && na) {
            auto f = std::make_shared<std::vector<Filter>>(*a.filter);
            for (size_t i = 0; i < na; ++i) (*f)[i].amount = mixed((*a.filter)[i].amount, (*b.filter)[i].amount, t);
            out.filter = std::move(f);
        } else {
            out.filter = t < 0.5f ? a.filter : b.filter;
        }
    }
}

// ── What's running ──

void Animator::clear() {
    entries_.clear();
    events_.clear();
    running_ = finite_running_ = false;
}

std::vector<Animator::Event> Animator::takeEvents() {
    std::vector<Event> out;
    out.swap(events_);
    return out;
}

std::vector<const Node*> Animator::moving() const {
    std::vector<const Node*> out;
    for (const auto& [key, e] : entries_) out.push_back(key.first);
    return out;
}

bool Animator::wouldStart(const StyledDocument& now, const StyledDocument& before) const {
    for (const StyledDocument::Moving& m : now.movingElements()) {
        const ComputedStyle* was = before.plainStyle(m.node, m.pseudo);
        const auto it = entries_.find(Key{m.node, m.pseudo});
        if (was == m.style || (it != entries_.end() && it->second.base_at == m.style)) continue;
        if (it != entries_.end() || m.style->animations) return true;
        if (was && m.style->transitions && was->display != Display::None && m.style->display != Display::None && movingDiff(*was, *m.style)) return true;
    }
    return false;
}

void Animator::styled(StyledDocument& now, const StyledDocument* before, double seconds) {
    std::set<Key> here;
    for (const StyledDocument::Moving& m : now.movingElements()) {
        const Key key{m.node, m.pseudo};
        if (!here.insert(key).second) continue;
        auto it = entries_.find(key);
        const ComputedStyle* was = before ? before->plainStyle(m.node, m.pseudo) : nullptr;
        // Nothing of it changed: as it was.
        if (it != entries_.end() && it->second.base_at == m.style) continue;
        const bool may_start = was && was != m.style && was->display != Display::None && m.style->display != Display::None && m.style->transitions;
        const uint32_t diff = may_start ? movingDiff(*was, *m.style) : 0;
        if (it == entries_.end()) {
            if (!m.style->animations && !diff) continue;
            if (entries_.size() >= kMaxEntries) continue;
            it = entries_.emplace(key, Entry{}).first;
        }
        Entry& e = it->second;
        const std::shared_ptr<const ComputedStyle> shown = e.shown ? e.shown : was ? std::make_shared<const ComputedStyle>(*was) : nullptr;
        e.base_at = m.style;
        e.base = std::make_shared<const ComputedStyle>(*m.style);
        for (uint32_t bit = 1; bit < kMoveAll && diff; bit <<= 1) {
            if (!(diff & bit)) continue;
            std::erase_if(e.transitions, [&](const Transition& t) { return t.bit == bit; });
            const TransitionSpec* spec = nullptr;
            for (const TransitionSpec& t : *m.style->transitions)
                if (t.bits & bit) spec = &t;
            if (!spec || spec->duration + spec->delay <= 0 || spec->duration < 0 || !shown) continue;
            Transition t;
            t.bit = bit;
            t.from = shown;
            t.to = e.base;
            t.start = seconds;
            t.duration = spec->duration;
            t.delay = spec->delay;
            t.easing = spec->easing;
            static const std::map<uint32_t, std::string> kNames = {
                {kMoveOpacity, "opacity"}, {kMoveTransform, "transform"}, {kMoveTranslate, "translate"}, {kMoveRotate, "rotate"}, {kMoveScale, "scale"},
                {kMoveColor, "color"}, {kMoveBackground, "background-color"}, {kMoveWidth, "width"}, {kMoveHeight, "height"}, {kMoveMinMax, "min-height"},
                {kMoveMargin, "margin-top"}, {kMovePadding, "padding-top"}, {kMoveTop, "top"}, {kMoveRight, "right"}, {kMoveBottom, "bottom"}, {kMoveLeft, "left"},
                {kMoveBorderColor, "border-top-color"}, {kMoveVisibility, "visibility"}, {kMoveFontSize, "font-size"}, {kMoveRadius, "border-top-left-radius"},
                {kMoveFilter, "filter"}, {kMoveMaxHeight, "max-height"}, {kMoveMaxWidth, "max-width"}};
            const bool own = spec->property != "all" && spec->property != "margin" && spec->property != "padding" && spec->property != "inset" &&
                             spec->property != "background" && spec->property != "border" && !spec->property.starts_with("-webkit-");
            t.property = own ? spec->property : kNames.at(bit);
            e.transitions.push_back(std::move(t));
        }
        if (!m.style->transitions) e.transitions.clear();
        // Animations named still go on; others stop, or start.
        std::vector<Playing> playing;
        if (m.style->animations)
            for (const AnimationSpec& a : *m.style->animations) {
                const auto had = std::find_if(e.playing.begin(), e.playing.end(), [&](const Playing& p) { return p.name == a.name; });
                if (had != e.playing.end()) {
                    playing.push_back(std::move(*had));
                    e.playing.erase(had);
                    continue;
                }
                Playing p;
                p.name = a.name;
                p.start = seconds;
                playing.push_back(std::move(p));
            }
        e.playing = std::move(playing);
    }
    // Gone, or with nothing to move any more.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!here.count(it->first)) it = entries_.erase(it);
        else ++it;
    }
    tick(now, seconds);
}

bool Animator::advance(const Key& key, Entry& e, double seconds) {
    ComputedStyle s = *e.base;
    bool over = false;   // something is laid over it
    if (e.base->animations) {
        for (size_t i = 0; i < e.playing.size() && i < e.base->animations->size(); ++i) {
            const AnimationSpec& a = (*e.base->animations)[i];
            Playing& p = e.playing[i];
            if (a.paused && p.held < 0) p.held = seconds - p.start;
            if (!a.paused && p.held >= 0) {
                p.start = seconds - p.held;
                p.held = -1;
            }
            const double t = (p.held >= 0 ? p.held : seconds - p.start) - a.delay;
            const double total = a.duration <= 0 ? 0 : static_cast<double>(a.duration) * a.iterations;
            const auto tell = [&](const char* type, double elapsed) {
                if (events_.size() < 10000) events_.push_back({key.first, key.second, type, a.name, static_cast<float>(elapsed)});
            };
            double progress = 0;   // of the iteration it's in
            int iteration = 0;
            if (t < 0) {
                if (!a.paused) running_ = true, finite_running_ = true;
                if (!a.fill_backwards) continue;
            } else {
                if (!p.begun) {
                    p.begun = true;
                    if (a.frames && !a.frames->empty()) tell("animationstart", 0);
                }
                if (t >= total) {
                    if (!p.ended) {
                        p.ended = true;
                        if (a.frames && !a.frames->empty()) tell("animationend", total);
                    }
                    if (!a.fill_forwards) continue;
                    const double last = std::max(0.0, std::ceil(static_cast<double>(a.iterations)) - 1);
                    iteration = static_cast<int>(std::min(last, 1e6));
                    progress = a.iterations <= 0 ? 0 : a.iterations - last;
                } else {
                    iteration = static_cast<int>(std::min(std::floor(t / a.duration), 1e6));
                    progress = t / a.duration - std::floor(t / a.duration);
                    if (iteration != p.iteration) {
                        p.iteration = iteration;
                        if (a.frames && !a.frames->empty()) tell("animationiteration", iteration * static_cast<double>(a.duration));
                    }
                    if (!a.paused) {
                        running_ = true;
                        if (std::isfinite(a.iterations)) finite_running_ = true;
                    }
                }
            }
            if (!a.frames || a.frames->empty()) continue;
            using D = AnimationSpec::Direction;
            const bool odd = iteration % 2 == 1;
            const bool back = a.direction == D::Reverse || (a.direction == D::Alternate && odd) || (a.direction == D::AlternateReverse && !odd);
            const float at = static_cast<float>(back ? 1 - progress : progress);
            // Each group between the frames around it that set it (the element's own style
            // standing in for a first or last that doesn't).
            uint32_t all = 0;
            for (const auto& f : *a.frames) all |= f.bits;
            for (uint32_t bit = 1; bit < kMoveAll; bit <<= 1) {
                if (!(all & bit)) continue;
                const AnimationSpec::Frame *lo = nullptr, *hi = nullptr;
                for (const auto& f : *a.frames) {
                    if (!(f.bits & bit)) continue;
                    if (f.offset <= at) lo = &f;
                    else if (!hi) hi = &f;
                }
                if (lo && !hi && lo->offset >= 1) {
                    moveBetween(s, *lo->style, *lo->style, 1, bit);
                } else {
                    const float from = lo ? lo->offset : 0, to = hi ? hi->offset : 1;
                    const ComputedStyle& x = lo ? *lo->style : *e.base;
                    const ComputedStyle& y = hi ? *hi->style : *e.base;
                    float k = to > from ? (at - from) / (to - from) : 1;
                    const Easing& ease = lo && lo->has_easing ? lo->easing : a.easing;
                    k = back ? 1 - ease.at(1 - k) : ease.at(k);
                    moveBetween(s, x, y, k, bit);
                }
                over = true;
            }
        }
    }
    for (auto it = e.transitions.begin(); it != e.transitions.end();) {
        Transition& t = *it;
        const double in = seconds - t.start - t.delay;
        if (in >= t.duration) {
            if (events_.size() < 10000) events_.push_back({key.first, key.second, "transitionend", t.property, t.duration});
            it = e.transitions.erase(it);
            continue;
        }
        if (in >= 0 && !t.begun) {
            t.begun = true;
            if (events_.size() < 10000) events_.push_back({key.first, key.second, "transitionstart", t.property, 0});
        }
        running_ = finite_running_ = true;
        moveBetween(s, *t.from, *t.to, in <= 0 ? 0 : t.easing.at(static_cast<float>(in / t.duration)), t.bit);
        over = true;
        ++it;
    }
    const std::shared_ptr<const ComputedStyle> was = e.shown;
    if (!over) {
        e.shown.reset();
        return was != nullptr;
    }
    const ComputedStyle& seen = was ? *was : *e.base;
    const bool changed = !was || movingDiff(seen, s) != 0;
    if (changed) e.shown = std::make_shared<const ComputedStyle>(std::move(s));
    return changed;
}

bool Animator::tick(StyledDocument& doc, double seconds) {
    running_ = finite_running_ = false;
    bool changed = false;
    for (auto& [key, e] : entries_) changed = advance(key, e, seconds) || changed;
    cover(doc);
    return changed;
}

void Animator::cover(StyledDocument& doc) const {
    doc.clearOverlay();
    for (const auto& [key, e] : entries_)
        if (e.shown) doc.lay(key.first, key.second, e.shown);
    // A colour (or visibility) that's moving is inherited: by what's inside and had its.
    for (const auto& [key, e] : entries_)
        if (e.shown && e.base && key.second == 0 && (e.shown->color != e.base->color || e.shown->hidden != e.base->hidden))
            inheritDown(doc, key.first, *e.base, *e.shown);
}

void Animator::inheritDown(StyledDocument& doc, const Node* from, const ComputedStyle& base, const ComputedStyle& shown) const {
    const bool color = shown.color != base.color, hidden = shown.hidden != base.hidden;
    struct At {
        const Node* node;
        int depth;
    };
    std::vector<At> todo{{from, 0}};
    std::vector<const Node*> kids;
    size_t budget = 5000;
    while (!todo.empty() && budget) {
        const At at = todo.back();
        todo.pop_back();
        const auto take = [&](const Node* n, int pseudo) {
            const ComputedStyle* plain = doc.plainStyle(n, pseudo);
            if (!plain) return false;
            const bool c = color && plain->color == base.color, h = hidden && plain->hidden == base.hidden;
            const auto own = entries_.find(Key{n, pseudo});
            // (Its own colour's moving: not inherited.)
            if ((!c && !h) || (own != entries_.end() && own->second.shown && own->second.base &&
                               (own->second.shown->color != own->second.base->color || own->second.shown->hidden != own->second.base->hidden)))
                return false;
            const ComputedStyle* laid = pseudo == 1 ? doc.beforeOf(n) : pseudo == 2 ? doc.afterOf(n) : doc.styleOf(n);
            auto with = std::make_shared<ComputedStyle>(laid ? *laid : *plain);
            if (c) with->color = shown.color;
            if (h) with->hidden = shown.hidden;
            doc.lay(n, pseudo, std::move(with));
            --budget;
            return true;
        };
        if (at.depth > 0 && !take(at.node, 0)) continue;
        for (const int pseudo : {1, 2})
            if (budget) (void)take(at.node, pseudo);
        composedChildren(at.node, kids);
        for (const Node* k : kids)
            if (k->isElement()) todo.push_back({k, at.depth + 1});
    }
}

}  // namespace fbx::xweb
