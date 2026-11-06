#include "xweb/layout.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <memory>
#include <string>

namespace fbx::xweb {

namespace {

constexpr float kUnbounded = 1e7f;     // "as wide as the content wants"
constexpr float kMaxCoord = 1e8f;      // nothing placed beyond this: keeps floats sane
constexpr int kMaxBoxDepth = 1024;

enum class BoxKind { Block, Inline, InlineBlock, Text, Replaced, LineBreak };

struct Box {
    BoxKind kind = BoxKind::Block;
    const Node* node = nullptr;
    const ComputedStyle* style = nullptr;
    std::vector<std::unique_ptr<Box>> kids;
    std::string text;              // Text
    std::string alt;               // Replaced with no image

    // Used values
    float x = 0, y = 0;            // border box, relative to the parent's content box
    float width = 0, height = 0;   // content box
    float margin[4] = {0, 0, 0, 0};
    float padding[4] = {0, 0, 0, 0};
    float border[4] = {0, 0, 0, 0};
    float margin_top_outer = 0, margin_bottom_outer = 0;   // after collapsing with children
    bool collapses_through = false;
    float static_y = 0;            // where an absolutely positioned box would have gone
    float pref_width = -1;         // max-content width, once computed
    int list_number = 0;
    float last_baseline = -1;      // of the last line box, from the content box's top

    struct Fragment {
        float x = 0, y = 0, w = 0, h = 0;
        float box_top = 0, box_h = 0;    // the text's own box, for backgrounds and lines
        std::string text;
        const ComputedStyle* style = nullptr;
        Box* atomic = nullptr;
        Color background{0, 0, 0, 0};
        const std::string* link = nullptr;
    };
    std::vector<Fragment> fragments;

    [[nodiscard]] bool outOfFlow() const noexcept {
        return style && (style->position == Position::Absolute || style->position == Position::Fixed);
    }
    [[nodiscard]] bool blockLevel() const noexcept { return kind == BoxKind::Block; }
    [[nodiscard]] float outerWidth() const noexcept {
        return width + padding[Left] + padding[Right] + border[Left] + border[Right];
    }
    [[nodiscard]] float outerHeight() const noexcept {
        return height + padding[Top] + padding[Bottom] + border[Top] + border[Bottom];
    }
};

float collapse(float a, float b) {
    if (a >= 0 && b >= 0) return std::max(a, b);
    if (a < 0 && b < 0) return std::min(a, b);
    return a + b;
}

float clampCoord(float v) { return std::clamp(v, -kMaxCoord, kMaxCoord); }

bool isCollapsible(WhiteSpace w) { return w == WhiteSpace::Normal || w == WhiteSpace::NoWrap; }
bool canWrap(WhiteSpace w) { return w == WhiteSpace::Normal || w == WhiteSpace::PreWrap || w == WhiteSpace::PreLine; }
bool keepsNewlines(WhiteSpace w) { return w == WhiteSpace::Pre || w == WhiteSpace::PreWrap || w == WhiteSpace::PreLine; }
bool isSpaceChar(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

FontSpec fontOf(const ComputedStyle& s) {
    FontSpec f;
    f.size = s.font_size;
    f.weight = s.font_weight;
    f.italic = s.italic;
    f.family = s.font_family;
    return f;
}

std::string transformed(std::string_view in, TextTransform t, bool& at_word_start) {
    std::string out(in);
    if (t == TextTransform::Uppercase) {
        std::transform(out.begin(), out.end(), out.begin(),
                       [](char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; });
    } else if (t == TextTransform::Lowercase) {
        std::transform(out.begin(), out.end(), out.begin(),
                       [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
    } else if (t == TextTransform::Capitalize) {
        for (char& c : out) {
            if (at_word_start && c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
            at_word_start = isSpaceChar(c);
        }
    }
    return out;
}

// A break is allowed around ideographic text, which has no spaces to break at.
bool breaksAroundCodePoint(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2FFFF);
}

uint32_t decodeAt(std::string_view s, size_t i, size_t& len) {
    const auto c = static_cast<unsigned char>(s[i]);
    const auto cont = [&](size_t k) { return i + k < s.size() ? static_cast<uint32_t>(static_cast<unsigned char>(s[i + k]) & 0x3F) : 0u; };
    if (c < 0x80) { len = 1; return c; }
    if (c < 0xE0) { len = 2; return ((c & 0x1Fu) << 6) | cont(1); }
    if (c < 0xF0) { len = 3; return ((c & 0x0Fu) << 12) | (cont(1) << 6) | cont(2); }
    len = 4;
    return ((c & 0x07u) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3);
}

std::string romanNumeral(int n) {
    static constexpr std::pair<int, const char*> table[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
                                                            {50, "l"}, {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}};
    if (n <= 0 || n >= 4000) return std::to_string(n);
    std::string out;
    for (const auto& [value, text] : table)
        while (n >= value) {
            out += text;
            n -= value;
        }
    return out;
}

std::string markerText(ListStyle style, int number) {
    switch (style) {
        case ListStyle::None: return {};
        case ListStyle::Disc: return "\xE2\x80\xA2";     // •
        case ListStyle::Circle: return "\xE2\x97\xA6";   // ◦
        case ListStyle::Square: return "\xE2\x96\xAA";   // ▪
        case ListStyle::Decimal: return std::to_string(number) + ".";
        case ListStyle::LowerAlpha:
        case ListStyle::UpperAlpha: {
            if (number <= 0) return std::to_string(number) + ".";
            std::string out;
            for (int n = number; n > 0; n = (n - 1) / 26)
                out.insert(out.begin(), static_cast<char>((style == ListStyle::LowerAlpha ? 'a' : 'A') + (n - 1) % 26));
            return out + ".";
        }
        case ListStyle::LowerRoman: return romanNumeral(number) + ".";
        case ListStyle::UpperRoman: {
            std::string r = romanNumeral(number);
            std::transform(r.begin(), r.end(), r.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
            return r + ".";
        }
    }
    return {};
}

class Layouter {
public:
    Layouter(const StyledDocument& styles, TextMeasurer& text, ImageSizes* images, float vw, float vh)
        : styles_(styles), text_(text), images_(images), vw_(vw), vh_(vh) {}

    PageLayout run(const Document& doc) {
        PageLayout page;
        page.width = vw_;
        if (const Node* head = doc.head()) {
            for (const Node* c : head->children) {
                if (c->isHtml("title")) {
                    page.title = c->textContent();
                    break;
                }
            }
        }
        const Node* root_el = doc.documentElement();
        const ComputedStyle* root_style = root_el ? styles_.styleOf(root_el) : nullptr;
        if (!root_el || !root_style || root_style->display == Display::None) {
            page.height = vh_;
            return page;
        }
        auto root = build(root_el, *root_style, 0);
        if (!root) {
            page.height = vh_;
            return page;
        }
        if (root->kind != BoxKind::Block) root->kind = BoxKind::Block;
        root_ = root.get();
        layoutBlock(*root, vw_, std::nullopt);

        // The canvas takes the root's background, or else the body's.
        page.canvas = {255, 255, 255, 255};
        const Node* body = doc.body();
        const ComputedStyle* body_style = body ? styles_.styleOf(body) : nullptr;
        const Box* skip_bg = nullptr;
        if (root_style->background.visible()) {
            page.canvas = root_style->background;
            skip_bg = root.get();
        } else if (body_style && body_style->background.visible()) {
            page.canvas = body_style->background;
            for (const auto& k : root->kids)
                if (k->node == body) skip_bg = k.get();
        }
        skip_background_ = skip_bg;
        out_ = &page;
        Rect viewport{0, 0, vw_, vh_};
        root->x = root->margin[Left];
        const float root_y = root->margin_top_outer;
        record(*root, root->x, root_y);
        paintContents(*root, root->x, root_y, viewport, 1.0f);
        flushPositioned();
        page.height = std::max({vh_, max_bottom_, root_y + root->outerHeight() + root->margin_bottom_outer});
        return page;
    }

private:
    struct Rect {
        float x, y, w, h;
    };

    float lineHeightOf(const ComputedStyle& s) {
        if (s.line_height_factor > 0 || s.line_height > 0) return s.usedLineHeight();
        const FontSpec font = fontOf(s);
        return text_.ascent(font) + text_.descent(font) + text_.lineGap(font);
    }

    // ── The box tree ──────────────────────────────────────────────────────────

    const ComputedStyle* anonymousStyle(const ComputedStyle& parent) {
        anon_styles_.emplace_back();
        ComputedStyle& s = anon_styles_.back();
        s.color = parent.color;
        s.font_size = parent.font_size;
        s.font_weight = parent.font_weight;
        s.italic = parent.italic;
        s.font_family = parent.font_family;
        s.line_height = parent.line_height;
        s.line_height_factor = parent.line_height_factor;
        s.text_align = parent.text_align;
        s.text_transform = parent.text_transform;
        s.white_space = parent.white_space;
        s.list_style = parent.list_style;
        s.hidden = parent.hidden;
        s.underline = parent.underline;
        s.line_through = parent.line_through;
        s.display = Display::Block;
        return &s;
    }

    static bool isReplaced(const Node* n) {
        if (n->ns == Namespace::Svg && n->name == "svg") return true;
        if (n->ns != Namespace::Html) return false;
        const std::string& t = n->name;
        return t == "img" || t == "video" || t == "canvas" || t == "iframe" || t == "embed" || t == "object" ||
               t == "input" || t == "textarea" || t == "select" || t == "progress" || t == "meter";
    }

    std::string controlText(const Node* n) {
        if (n->isHtml("input")) {
            if (const std::string* v = n->attr("value"); v && !v->empty()) return *v;
            if (const std::string* v = n->attr("placeholder")) return *v;
            return {};
        }
        if (n->isHtml("textarea")) return n->textContent();
        if (n->isHtml("select")) {
            // What's showing: the selected option, or else the first.
            const Node* first = nullptr;
            std::vector<const Node*> todo(n->children.rbegin(), n->children.rend());
            while (!todo.empty()) {
                const Node* c = todo.back();
                todo.pop_back();
                if (c->isHtml("option")) {
                    if (!first) first = c;
                    if (c->attr("selected")) return c->textContent();
                }
                todo.insert(todo.end(), c->children.rbegin(), c->children.rend());
            }
            return first ? first->textContent() : std::string{};
        }
        return {};
    }

    std::unique_ptr<Box> build(const Node* el, const ComputedStyle& style, int depth) {
        if (style.display == Display::None || depth > kMaxBoxDepth) return nullptr;
        auto box = std::make_unique<Box>();
        box->node = el;
        box->style = &style;
        switch (style.display) {
            case Display::Inline: box->kind = BoxKind::Inline; break;
            case Display::InlineBlock: box->kind = BoxKind::InlineBlock; break;
            default: box->kind = BoxKind::Block; break;
        }
        if (el->isHtml("br")) {
            box->kind = BoxKind::LineBreak;
            return box;
        }
        if (el->isHtml("input")) {
            const std::string* ty = el->attr("type");
            if (ty && (*ty == "checkbox" || *ty == "radio")) {
                box->kind = BoxKind::Replaced;
                return box;
            }
        }
        if (isReplaced(el)) {
            const bool was_block = box->kind == BoxKind::Block;
            box->kind = was_block ? BoxKind::Block : BoxKind::Replaced;
            if (const std::string* alt = el->attr("alt")) box->alt = *alt;
            if (was_block) {
                auto inner = std::make_unique<Box>();
                inner->node = el;
                inner->style = &style;
                inner->kind = BoxKind::Replaced;
                inner->alt = box->alt;
                replaced_inner_.push_back(inner.get());
                box->kids.push_back(std::move(inner));
            }
            return box;
        }

        // The element's <ol> counter, seen by its list items.
        int counter = 1;
        if (const std::string* start = el->attr("start"))
            counter = static_cast<int>(std::strtol(start->c_str(), nullptr, 10));

        for (const Node* c : el->children) {
            if (c->type == NodeType::Text) {
                if (c->data.empty()) continue;
                auto t = std::make_unique<Box>();
                t->kind = BoxKind::Text;
                t->node = c;
                t->style = &style;
                t->text = c->data;
                box->kids.push_back(std::move(t));
            } else if (c->isElement()) {
                const ComputedStyle* cs = styles_.styleOf(c);
                if (!cs) continue;
                auto kid = build(c, *cs, depth + 1);
                if (!kid) continue;
                if (cs->display == Display::ListItem) {
                    if (const std::string* v = c->attr("value"))
                        counter = static_cast<int>(std::strtol(v->c_str(), nullptr, 10));
                    kid->list_number = counter++;
                }
                box->kids.push_back(std::move(kid));
            }
        }

        // A block inside an inline: treat the inline as a block too. (The real
        // thing splits it in two around the block; rarely worth the difference.)
        if (box->kind == BoxKind::Inline &&
            std::any_of(box->kids.begin(), box->kids.end(), [](const auto& k) { return k->blockLevel() && !k->outOfFlow(); }))
            box->kind = BoxKind::Block;

        if (box->kind == BoxKind::Block || box->kind == BoxKind::InlineBlock) wrapInlineRuns(*box);
        return box;
    }

    // Block children and inline content can't share a box: inline runs get an
    // anonymous block. Whitespace between blocks disappears here.
    void wrapInlineRuns(Box& box) {
        const bool has_block = std::any_of(box.kids.begin(), box.kids.end(),
                                           [](const auto& k) { return k->blockLevel() && !k->outOfFlow(); });
        if (!has_block) return;
        std::vector<std::unique_ptr<Box>> result;
        std::unique_ptr<Box> run;
        const auto finish = [&] {
            if (!run) return;
            const bool blank = std::all_of(run->kids.begin(), run->kids.end(), [&](const auto& k) {
                return k->kind == BoxKind::Text && isCollapsible(box.style->white_space) &&
                       std::all_of(k->text.begin(), k->text.end(), isSpaceChar);
            });
            if (!blank) result.push_back(std::move(run));
            run.reset();
        };
        for (auto& k : box.kids) {
            if (k->blockLevel() || k->outOfFlow()) {
                finish();
                result.push_back(std::move(k));
            } else {
                if (!run) {
                    run = std::make_unique<Box>();
                    run->kind = BoxKind::Block;
                    run->style = anonymousStyle(*box.style);
                }
                run->kids.push_back(std::move(k));
            }
        }
        finish();
        box.kids = std::move(result);
    }

    // ── Block layout ──────────────────────────────────────────────────────────

    static void resolveSpacing(Box& b, float cb_width) {
        const ComputedStyle& s = *b.style;
        for (int i = 0; i < 4; ++i) {
            b.padding[i] = std::max(0.0f, clampCoord(s.padding[i].resolve(cb_width)));
            b.border[i] = s.border_width[i];
            b.margin[i] = s.margin[i].is_auto ? 0 : clampCoord(s.margin[i].resolve(cb_width));
        }
    }

    float clampWidth(const Box& b, float content, float cb_width) const {
        const ComputedStyle& s = *b.style;
        const float extra = s.border_box ? b.padding[Left] + b.padding[Right] + b.border[Left] + b.border[Right] : 0;
        if (!s.max_width.is_auto) content = std::min(content, std::max(0.0f, s.max_width.resolve(cb_width) - extra));
        content = std::max(content, std::max(0.0f, s.min_width.resolve(cb_width) - extra));
        return content;
    }

    void layoutBlock(Box& b, float cb_width, std::optional<float> cb_height) {
        const ComputedStyle& s = *b.style;
        resolveSpacing(b, cb_width);
        const float frame = b.padding[Left] + b.padding[Right] + b.border[Left] + b.border[Right];

        // Width (10.3.3)
        float content;
        const bool width_auto = s.width.is_auto;
        if (!width_auto) {
            content = s.width.resolve(cb_width);
            if (s.border_box) content -= frame;
            content = std::max(0.0f, content);
        } else {
            content = std::max(0.0f, cb_width - frame - b.margin[Left] - b.margin[Right]);
        }
        content = clampWidth(b, content, cb_width);
        b.width = clampCoord(content);

        // Auto margins share what's left.
        const float free_space = cb_width - b.width - frame;
        const bool ml_auto = s.margin[Left].is_auto, mr_auto = s.margin[Right].is_auto;
        if (!width_auto || !s.max_width.is_auto) {
            if (ml_auto && mr_auto) {
                b.margin[Left] = b.margin[Right] = std::max(0.0f, free_space / 2);
            } else if (ml_auto) {
                b.margin[Left] = free_space - b.margin[Right];
            } else if (mr_auto) {
                b.margin[Right] = free_space - b.margin[Left];
            }
        }

        std::optional<float> own_height;
        if (!s.height.is_auto && (!s.height.hasPercent() || cb_height)) {
            float h = s.height.resolve(cb_height.value_or(0));
            if (s.border_box) h -= b.padding[Top] + b.padding[Bottom] + b.border[Top] + b.border[Bottom];
            own_height = std::max(0.0f, h);
        }

        layoutContents(b, own_height);
    }

    void layoutContents(Box& b, std::optional<float> own_height) {
        const ComputedStyle& s = *b.style;
        const bool inline_content = std::none_of(b.kids.begin(), b.kids.end(),
                                                 [](const auto& k) { return k->blockLevel() && !k->outOfFlow(); });
        float content_h = 0;
        float mt = b.margin[Top], mb = b.margin[Bottom];
        // These establish a new formatting context: margins stop at their edge.
        const bool isolated = b.kind == BoxKind::InlineBlock || b.outOfFlow() || &b == root_;
        const bool top_open = !isolated && b.padding[Top] == 0 && b.border[Top] == 0;
        const bool bottom_open = !isolated && b.padding[Bottom] == 0 && b.border[Bottom] == 0 && !own_height;
        // Margins collapse through a box with nothing in the way and no height.
        const bool can_collapse_through = top_open && !isolated && b.padding[Bottom] == 0 && b.border[Bottom] == 0 &&
                                          (!own_height || *own_height == 0) && s.min_height.px <= 0 && !s.min_height.hasPercent();
        b.collapses_through = false;

        if (inline_content) {
            content_h = layoutInline(b);
            if (content_h == 0 && can_collapse_through) {
                b.collapses_through = true;
                mt = collapse(mt, mb);
                mb = mt;
            }
        } else {
            float pos = 0;
            float pending = 0;
            bool first = true;
            bool any_content = false;
            float through_top = mt;
            for (auto& kp : b.kids) {
                Box& k = *kp;
                if (k.outOfFlow()) {
                    k.static_y = pos + (first ? 0 : pending);
                    layoutOutOfFlow(k, b.width);
                    continue;
                }
                layoutBlock(k, b.width, own_height);
                k.x = k.margin[Left];
                if (k.collapses_through) {
                    if (first && top_open) {
                        through_top = collapse(through_top, k.margin_top_outer);
                        k.y = 0;
                    } else {
                        pending = collapse(pending, k.margin_top_outer);
                        k.y = pos + pending;
                    }
                    continue;
                }
                any_content = true;
                if (first && top_open) {
                    through_top = collapse(through_top, k.margin_top_outer);
                    k.y = 0;
                    first = false;
                } else {
                    const float gap = first ? k.margin_top_outer : collapse(pending, k.margin_top_outer);
                    pos += gap;
                    k.y = pos;
                    first = false;
                }
                pos = clampCoord(pos + k.outerHeight());
                pending = k.margin_bottom_outer;
            }
            mt = through_top;
            if (bottom_open && any_content) {
                mb = collapse(mb, pending);
            } else if (!any_content && can_collapse_through) {
                b.collapses_through = true;
                mt = collapse(collapse(mt, pending), mb);
                mb = mt;
            } else {
                pos += pending;
            }
            content_h = pos;
        }

        float h = own_height.value_or(content_h);
        const float extra = s.border_box ? b.padding[Top] + b.padding[Bottom] + b.border[Top] + b.border[Bottom] : 0;
        if (!s.max_height.is_auto && !s.max_height.hasPercent()) h = std::min(h, std::max(0.0f, s.max_height.px - extra));
        if (!s.min_height.hasPercent()) h = std::max(h, std::max(0.0f, s.min_height.px - extra));
        b.height = clampCoord(h);
        b.margin_top_outer = mt;
        b.margin_bottom_outer = mb;
    }

    void layoutOutOfFlow(Box& k, float cb_width) {
        const ComputedStyle& s = *k.style;
        const float base = s.position == Position::Fixed ? vw_ : cb_width;
        resolveSpacing(k, base);
        const float frame = k.padding[Left] + k.padding[Right] + k.border[Left] + k.border[Right];
        float width;
        if (!s.width.is_auto) {
            width = s.width.resolve(base);
            if (s.border_box) width -= frame;
        } else if (!s.offset[Left].is_auto && !s.offset[Right].is_auto) {
            width = base - s.offset[Left].resolve(base) - s.offset[Right].resolve(base) - frame - k.margin[Left] - k.margin[Right];
        } else {
            width = std::min(preferredWidth(k), base - frame);
        }
        k.width = clampWidth(k, std::max(0.0f, width), base);
        std::optional<float> own;
        if (!s.height.is_auto && !s.height.hasPercent()) {
            float h = s.height.px;
            if (s.border_box) h -= k.padding[Top] + k.padding[Bottom] + k.border[Top] + k.border[Bottom];
            own = std::max(0.0f, h);
        } else if (!s.height.is_auto) {
            own = std::max(0.0f, s.height.resolve(vh_));
        }
        layoutContents(k, own);
    }

    // The width the content would take with nowhere to wrap.
    float preferredWidth(Box& b) {
        if (b.pref_width >= 0) return b.pref_width;
        const ComputedStyle& s = *b.style;
        if (!s.width.is_auto && !s.width.hasPercent()) {
            b.pref_width = std::max(0.0f, s.width.px);
            return b.pref_width;
        }
        const bool inline_content = std::none_of(b.kids.begin(), b.kids.end(),
                                                 [](const auto& k) { return k->blockLevel() && !k->outOfFlow(); });
        float result = 0;
        if (inline_content) {
            const float saved = b.width;
            b.width = kUnbounded;
            (void)layoutInline(b);
            for (const auto& f : b.fragments) result = std::max(result, f.x + f.w);
            b.width = saved;
        } else {
            for (auto& k : b.kids) {
                if (k->outOfFlow()) continue;
                resolveSpacing(*k, 0);
                result = std::max(result, preferredWidth(*k) + k->padding[Left] + k->padding[Right] + k->border[Left] +
                                              k->border[Right] + k->margin[Left] + k->margin[Right]);
            }
        }
        b.pref_width = std::min(result, kUnbounded);
        return b.pref_width;
    }

    void layoutInlineBlock(Box& b, float cb_width) {
        const ComputedStyle& s = *b.style;
        resolveSpacing(b, cb_width);
        const float frame = b.padding[Left] + b.padding[Right] + b.border[Left] + b.border[Right];
        float width;
        if (!s.width.is_auto) {
            width = s.width.resolve(cb_width);
            if (s.border_box) width -= frame;
        } else {
            // Shrink to fit (10.3.9)
            width = std::min(preferredWidth(b), std::max(0.0f, cb_width - frame - b.margin[Left] - b.margin[Right]));
        }
        b.width = clampWidth(b, std::max(0.0f, width), cb_width);
        std::optional<float> own;
        if (!s.height.is_auto && !s.height.hasPercent()) {
            float h = s.height.px;
            if (s.border_box) h -= b.padding[Top] + b.padding[Bottom] + b.border[Top] + b.border[Bottom];
            own = std::max(0.0f, h);
        }
        layoutContents(b, own);
    }

    void layoutReplaced(Box& b, float cb_width) {
        const ComputedStyle& s = *b.style;
        resolveSpacing(b, cb_width);
        const Node* n = b.node;
        float natural_w = 300, natural_h = 150;
        bool have_natural = true;
        if (n->isHtml("img")) {
            have_natural = false;
            const std::string* src = n->attr("src");
            if (src && images_) {
                if (const auto size = images_->imageSize(*src)) {
                    natural_w = size->first;
                    natural_h = size->second;
                    have_natural = true;
                }
            }
            if (!have_natural) {
                natural_w = natural_h = 0;
                if (!b.alt.empty()) {
                    const FontSpec f = fontOf(s);
                    natural_w = text_.width(b.alt, f);
                    natural_h = lineHeightOf(s);
                }
            }
        } else if (n->isHtml("input")) {
            const std::string* ty = n->attr("type");
            if (ty && (*ty == "checkbox" || *ty == "radio")) {
                natural_w = natural_h = 13;
                resolveSpacing(b, cb_width);
                for (int i = 0; i < 4; ++i) b.padding[i] = 0;
            } else {
                natural_w = 150;
                natural_h = lineHeightOf(s);
                const std::string shown = controlText(n);
                if (ty && (*ty == "submit" || *ty == "button" || *ty == "reset"))
                    natural_w = text_.width(shown.empty() ? "Submit" : shown, fontOf(s)) + 8;
            }
        } else if (n->isHtml("textarea")) {
            natural_w = 200;
            natural_h = lineHeightOf(s) * 2;
        } else if (n->isHtml("select")) {
            natural_w = text_.width(controlText(n), fontOf(s)) + 24;
            natural_h = lineHeightOf(s);
        } else if (n->isHtml("progress") || n->isHtml("meter")) {
            natural_w = 160;
            natural_h = 16;
        }
        const float h_frame = b.padding[Left] + b.padding[Right] + b.border[Left] + b.border[Right];
        const float v_frame = b.padding[Top] + b.padding[Bottom] + b.border[Top] + b.border[Bottom];
        const bool has_w = !s.width.is_auto;
        const bool has_h = !s.height.is_auto && !s.height.hasPercent();
        float w = natural_w, h = natural_h;
        if (has_w) {
            w = s.width.resolve(cb_width);
            if (s.border_box) w -= h_frame;
        }
        if (has_h) {
            h = s.height.px;
            if (s.border_box) h -= v_frame;
        }
        const float ratio = (natural_w > 0 && natural_h > 0) ? natural_w / natural_h : 0;
        if (has_w && !has_h && ratio > 0) h = w / ratio;
        if (has_h && !has_w && ratio > 0) w = h * ratio;
        const float auto_w = w;
        w = clampWidth(b, std::max(0.0f, w), cb_width);
        // max-width: 100% on an image keeps its shape.
        if (w != auto_w && !has_h && ratio > 0) h = w / ratio;
        b.width = w;
        b.height = std::max(0.0f, h);
    }

    // ── Inline layout ─────────────────────────────────────────────────────────

    struct InlineItem {
        enum Kind { Text, Spacer, Atomic, Break, Positioned } kind = Text;
        const ComputedStyle* style = nullptr;
        std::string text;
        float width = 0;
        Box* atomic = nullptr;
        Color background{0, 0, 0, 0};
        const std::string* link = nullptr;
    };

    struct InlineContext {
        Color background{0, 0, 0, 0};
        const std::string* link = nullptr;
    };

    void collectInline(Box& box, InlineContext ctx, std::vector<InlineItem>& items, float cb_width) {
        for (auto& kp : box.kids) {
            Box& k = *kp;
            switch (k.kind) {
                case BoxKind::Text: {
                    InlineItem it;
                    it.kind = InlineItem::Text;
                    it.style = k.style;
                    it.text = k.text;
                    it.background = ctx.background;
                    it.link = ctx.link;
                    items.push_back(std::move(it));
                    break;
                }
                case BoxKind::LineBreak: {
                    InlineItem it;
                    it.kind = InlineItem::Break;
                    it.style = k.style;
                    items.push_back(std::move(it));
                    break;
                }
                case BoxKind::Inline: {
                    InlineContext inner = ctx;
                    const ComputedStyle& s = *k.style;
                    if (s.background.visible()) inner.background = s.background;
                    if (k.node && k.node->isHtml("a"))
                        if (const std::string* href = k.node->attr("href")) inner.link = href;
                    resolveSpacing(k, cb_width);
                    const float open = k.margin[Left] + k.padding[Left] + k.border[Left];
                    const float close = k.margin[Right] + k.padding[Right] + k.border[Right];
                    if (open != 0) {
                        InlineItem it;
                        it.kind = InlineItem::Spacer;
                        it.width = open;
                        it.style = k.style;
                        items.push_back(std::move(it));
                    }
                    collectInline(k, inner, items, cb_width);
                    if (close != 0) {
                        InlineItem it;
                        it.kind = InlineItem::Spacer;
                        it.width = close;
                        it.style = k.style;
                        items.push_back(std::move(it));
                    }
                    break;
                }
                case BoxKind::InlineBlock:
                case BoxKind::Replaced: {
                    if (k.outOfFlow()) break;
                    if (k.kind == BoxKind::InlineBlock) layoutInlineBlock(k, cb_width);
                    else layoutReplaced(k, cb_width);
                    InlineItem it;
                    it.kind = InlineItem::Atomic;
                    it.style = k.style;
                    it.atomic = &k;
                    it.link = ctx.link;
                    if (k.node && k.node->isHtml("a"))
                        if (const std::string* href = k.node->attr("href")) it.link = href;
                    items.push_back(std::move(it));
                    break;
                }
                case BoxKind::Block:
                    if (k.outOfFlow()) {
                        layoutOutOfFlow(k, cb_width);
                        InlineItem it;
                        it.kind = InlineItem::Positioned;
                        it.style = k.style;
                        it.atomic = &k;
                        items.push_back(std::move(it));
                    }
                    break;
            }
        }
    }

    // Returns the height of the lines; fills b.fragments.
    float layoutInline(Box& b) {
        b.fragments.clear();
        b.last_baseline = -1;
        std::vector<InlineItem> items;
        // An inline-block counts its own <a> for what it contains.
        InlineContext ctx;
        if (b.node && b.node->isHtml("a"))
            if (const std::string* href = b.node->attr("href")) ctx.link = href;
        if (b.kids.size() == 1 && b.kids[0]->kind == BoxKind::Replaced && b.kind == BoxKind::Block && b.kids[0]->node == b.node) {
            // A display:block replaced element is exactly its one box.
            Box& r = *b.kids[0];
            layoutReplaced(r, b.width);
            r.x = r.y = 0;
            r.margin[Left] = r.margin[Right] = r.margin[Top] = r.margin[Bottom] = 0;
            r.padding[Left] = r.padding[Right] = r.padding[Top] = r.padding[Bottom] = 0;
            r.border[Left] = r.border[Right] = r.border[Top] = r.border[Bottom] = 0;
            Box::Fragment f;
            f.x = 0;
            f.y = 0;
            f.w = r.width;
            f.h = r.height;
            f.atomic = &r;
            f.style = r.style;
            b.fragments.push_back(f);
            return r.height;
        }
        collectInline(b, ctx, items, b.width);

        LineBuilder lines(*this, b);
        for (InlineItem& it : items) lines.add(it);
        lines.finish();
        return lines.totalHeight();
    }

    class LineBuilder {
    public:
        LineBuilder(Layouter& l, Box& b) : l_(l), b_(b), avail_(b.width) {
            const ComputedStyle& s = *b.style;
            indent_ = s.text_indent.resolve(b.width);
            x_ = indent_;
        }

        void add(InlineItem& it) {
            switch (it.kind) {
                case InlineItem::Text: addText(it); break;
                case InlineItem::Spacer:
                    pushChunk(Chunk{Chunk::Space, {}, it.width, it.style, {0, 0, 0, 0}, nullptr, nullptr});
                    break;
                case InlineItem::Atomic:
                    flushRun();
                    addSpaceIfPending();
                    pushChunk(Chunk{Chunk::Atomic, {}, marginBoxWidth(*it.atomic), it.style, {0, 0, 0, 0}, it.link, it.atomic});
                    flushRun();
                    after_space_ = false;
                    line_has_content_ = true;
                    break;
                case InlineItem::Break:
                    flushRun();
                    pending_space_ = false;
                    endLine(it.style, true);
                    break;
                case InlineItem::Positioned:
                    // Its static position: where the next block would have started.
                    flushRun();
                    it.atomic->static_y = y_ + (line_.empty() ? 0 : l_.lineHeightOf(*b_.style));
                    break;
            }
        }

        void finish() {
            flushRun();
            pending_space_ = false;
            if (!line_.empty() || line_has_content_) endLine(nullptr, false);
        }

        [[nodiscard]] float totalHeight() const { return y_; }

    private:
        struct Chunk {
            enum Kind { Word, Space, Atomic } kind = Word;
            std::string text;
            float width = 0;
            const ComputedStyle* style = nullptr;
            Color background{0, 0, 0, 0};
            const std::string* link = nullptr;
            Box* atomic = nullptr;
        };

        // From the top of its margin box down to the baseline it sits on: that of its
        // last line, or with none, its bottom margin edge.
        static float atomicBaseline(const Box& a) {
            const float total = a.outerHeight() + a.margin[Top] + a.margin[Bottom];
            if (a.kind != BoxKind::InlineBlock) return total;
            const Box* inner = &a;
            float origin = a.margin[Top] + a.border[Top] + a.padding[Top];
            while (inner->last_baseline < 0) {
                const Box* last = nullptr;
                for (const auto& k : inner->kids)
                    if (k->kind == BoxKind::Block && !k->outOfFlow() && !k->collapses_through) last = k.get();
                if (!last) return total;
                origin += last->y + last->border[Top] + last->padding[Top];
                inner = last;
            }
            return origin + inner->last_baseline;
        }

        static float marginBoxWidth(const Box& a) {
            return a.outerWidth() + a.margin[Left] + a.margin[Right];
        }

        void pushChunk(Chunk c) { run_.push_back(std::move(c)); }

        void addSpaceIfPending() {
            if (!pending_space_) return;
            pending_space_ = false;
            if (line_.empty() && !line_has_content_) return;   // collapsed at line start
            Chunk c;
            c.kind = Chunk::Space;
            c.text = " ";
            c.style = space_style_;
            c.background = space_background_;
            c.link = space_link_;
            c.width = l_.text_.width(" ", fontOf(*space_style_));
            line_space_ = c;
            has_line_space_ = true;
        }

        void flushRun() {
            if (run_.empty()) return;
            float run_w = 0;
            for (const Chunk& c : run_) run_w += c.width;
            const float space_w = has_line_space_ ? line_space_.width : 0;
            const bool wrap_allowed = std::all_of(run_.begin(), run_.end(), [](const Chunk& c) {
                return !c.style || canWrap(c.style->white_space);
            });
            if (wrap_allowed && (!line_.empty() || line_has_content_) && x_ + space_w + run_w > avail_) {
                has_line_space_ = false;
                endLine(nullptr, false);
            }
            if (has_line_space_) {
                place(line_space_);
                has_line_space_ = false;
            }
            for (Chunk& c : run_) place(c);
            run_.clear();
        }

        void place(const Chunk& c) {
            Box::Fragment f;
            f.x = x_;
            f.w = c.width;
            f.style = c.style;
            f.background = c.background;
            f.link = c.link;
            if (c.kind == Chunk::Atomic) {
                f.atomic = c.atomic;
                line_has_content_ = true;
                line_.push_back(std::move(f));
            } else if (!c.text.empty()) {
                // Next to text in the same style: extend it, so it's drawn as one run.
                if (!line_.empty() && !line_.back().atomic && line_.back().style == c.style &&
                    line_.back().background == c.background && line_.back().link == c.link &&
                    std::abs(line_.back().x + line_.back().w - x_) < 0.01f) {
                    line_.back().text += c.text;
                    line_.back().w += c.width;
                } else {
                    f.text = c.text;
                    line_.push_back(std::move(f));
                }
                line_has_content_ = true;
            }
            x_ = clampCoord(x_ + c.width);
        }

        void addWord(std::string_view word, const InlineItem& it) {
            if (word.empty()) return;
            addSpaceIfPending();
            Chunk c;
            c.kind = Chunk::Word;
            c.text = std::string(word);
            c.width = l_.text_.width(word, fontOf(*it.style));
            c.style = it.style;
            c.background = it.background;
            c.link = it.link;
            pushChunk(std::move(c));
            after_space_ = false;
        }

        // Splits a word at the opportunities inside it: after a hyphen, around CJK.
        void addBreakableWord(std::string_view word, const InlineItem& it, bool wrap) {
            if (!wrap) {
                addWord(word, it);
                return;
            }
            size_t start = 0;
            for (size_t i = 0; i < word.size();) {
                size_t len = 1;
                const uint32_t cp = decodeAt(word, i, len);
                const bool cjk = breaksAroundCodePoint(cp);
                const bool hyphen = cp == '-' && i > start && i + 1 < word.size();
                if (cjk && i > start) {
                    addWord(word.substr(start, i - start), it);
                    flushRun();
                    start = i;
                }
                i += len;
                if (cjk || hyphen) {
                    addWord(word.substr(start, i - start), it);
                    flushRun();
                    start = i;
                }
            }
            if (start < word.size()) addWord(word.substr(start), it);
        }

        void addText(const InlineItem& it) {
            const WhiteSpace ws = it.style->white_space;
            const bool collapsible = isCollapsible(ws) || ws == WhiteSpace::PreLine;
            bool word_start = !prev_was_word_;
            const std::string text = transformed(it.text, it.style->text_transform, word_start);
            const bool wrap = canWrap(ws);
            size_t i = 0;
            std::string word;
            while (i < text.size()) {
                const char c = text[i];
                if (keepsNewlines(ws) && c == '\n') {
                    addBreakableWord(word, it, wrap);
                    word.clear();
                    flushRun();
                    pending_space_ = false;
                    has_line_space_ = false;
                    endLine(it.style, true);
                    ++i;
                    continue;
                }
                if (isSpaceChar(c)) {
                    addBreakableWord(word, it, wrap);
                    word.clear();
                    flushRun();
                    if (collapsible) {
                        if (!after_space_ || !pending_space_) {
                            pending_space_ = true;
                            space_style_ = it.style;
                            space_background_ = it.background;
                            space_link_ = it.link;
                        }
                        after_space_ = true;
                        while (i < text.size() && isSpaceChar(text[i]) && !(keepsNewlines(ws) && text[i] == '\n')) ++i;
                        continue;
                    }
                    // Preserved: each one is its own run, and may hang past the edge.
                    addSpaceIfPending();
                    Chunk sp;
                    sp.kind = Chunk::Word;
                    sp.text = c == '\t' ? std::string("    ") : std::string(" ");
                    sp.width = l_.text_.width(sp.text, fontOf(*it.style));
                    sp.style = it.style;
                    sp.background = it.background;
                    sp.link = it.link;
                    pushChunk(std::move(sp));
                    flushRun();
                    ++i;
                    continue;
                }
                word += c;
                ++i;
            }
            addBreakableWord(word, it, wrap);
            prev_was_word_ = !word.empty();
        }

        void endLine(const ComputedStyle* break_style, bool forced) {
            const ComputedStyle& container = *b_.style;
            const FontSpec strut_font = fontOf(container);
            const float strut_a = l_.text_.ascent(strut_font);
            const float strut_d = l_.text_.descent(strut_font);
            const float strut_half = (l_.lineHeightOf(container) - (strut_a + strut_d)) / 2;
            float above = strut_a + strut_half, below = strut_d + strut_half;
            if (line_.empty() && !forced) above = below = 0;
            (void)break_style;

            for (const auto& f : line_) {
                if (f.atomic) {
                    const float total = f.atomic->outerHeight() + f.atomic->margin[Top] + f.atomic->margin[Bottom];
                    const float to_baseline = atomicBaseline(*f.atomic);
                    above = std::max(above, to_baseline);
                    below = std::max(below, total - to_baseline);
                    continue;
                }
                const FontSpec font = fontOf(*f.style);
                const float a = l_.text_.ascent(font), d = l_.text_.descent(font);
                const float half = (l_.lineHeightOf(*f.style) - (a + d)) / 2;
                above = std::max(above, a + half);
                below = std::max(below, d + half);
            }

            // Drop whitespace hanging off the end of the line.
            float line_w = line_.empty() ? 0 : line_.back().x + line_.back().w;
            if (!line_.empty() && !line_.back().atomic && line_.back().style && isCollapsible(line_.back().style->white_space)) {
                auto& last = line_.back();
                if (!last.text.empty() && last.text.back() == ' ') {
                    last.text.pop_back();
                    last.w = l_.text_.width(last.text, fontOf(*last.style));
                    line_w = last.x + last.w;
                }
            }

            float shift = 0;
            if (avail_ < kUnbounded) {
                if (container.text_align == TextAlign::Center) shift = (avail_ - line_w) / 2;
                else if (container.text_align == TextAlign::Right) shift = avail_ - line_w;
                shift = std::max(shift, 0.0f);
            }

            const float baseline = y_ + above;
            b_.last_baseline = baseline;
            for (auto& f : line_) {
                f.x += shift;
                if (f.atomic) {
                    Box* a = f.atomic;
                    a->x = f.x + a->margin[Left];
                    a->y = baseline - atomicBaseline(*a) + a->margin[Top];
                    f.y = a->y;
                    f.h = a->outerHeight();
                } else {
                    const FontSpec font = fontOf(*f.style);
                    f.box_top = baseline - l_.text_.ascent(font);
                    f.box_h = l_.text_.ascent(font) + l_.text_.descent(font);
                    f.y = f.box_top;
                    f.h = f.box_h;
                }
                b_.fragments.push_back(std::move(f));
            }
            line_.clear();
            y_ = clampCoord(y_ + above + below);
            x_ = 0;
            line_has_content_ = false;
            after_space_ = true;
        }

        Layouter& l_;
        Box& b_;
        float avail_;
        float indent_ = 0;
        float x_ = 0, y_ = 0;
        std::vector<Box::Fragment> line_;
        std::vector<Chunk> run_;
        Chunk line_space_;
        bool has_line_space_ = false;
        bool pending_space_ = false;
        bool after_space_ = true;
        bool line_has_content_ = false;
        bool prev_was_word_ = false;
        const ComputedStyle* space_style_ = nullptr;
        Color space_background_{0, 0, 0, 0};
        const std::string* space_link_ = nullptr;
    };

    // ── Painting ──────────────────────────────────────────────────────────────

    static Color faded(Color c, float opacity) {
        c.a = static_cast<uint8_t>(std::lround(static_cast<float>(c.a) * opacity));
        return c;
    }

    void addRect(float x, float y, float w, float h, Color c) {
        if (w <= 0 || h <= 0 || !c.visible()) return;
        PaintOp op;
        op.kind = PaintKind::Rect;
        op.x = x;
        op.y = y;
        op.w = w;
        op.h = h;
        op.color = c;
        out_->ops.push_back(std::move(op));
    }

    void paintFrame(const Box& b, float x, float y, float opacity) {
        const ComputedStyle& s = *b.style;
        const float w = b.outerWidth(), h = b.outerHeight();
        if (s.hidden) return;
        if (&b != skip_background_) addRect(x, y, w, h, faded(s.background, opacity));
        if (b.border[Top] > 0) addRect(x, y, w, b.border[Top], faded(s.border_color[Top], opacity));
        if (b.border[Bottom] > 0) addRect(x, y + h - b.border[Bottom], w, b.border[Bottom], faded(s.border_color[Bottom], opacity));
        if (b.border[Left] > 0) addRect(x, y, b.border[Left], h, faded(s.border_color[Left], opacity));
        if (b.border[Right] > 0) addRect(x + w - b.border[Right], y, b.border[Right], h, faded(s.border_color[Right], opacity));
    }

    void addText(float x, float y, const std::string& text, const ComputedStyle& s, float opacity) {
        PaintOp op;
        op.kind = PaintKind::Text;
        op.x = x;
        op.y = y;
        op.text = text;
        op.font = fontOf(s);
        op.color = faded(s.color, opacity);
        op.w = text_.width(text, op.font);
        op.h = text_.ascent(op.font) + text_.descent(op.font);
        out_->ops.push_back(std::move(op));
    }

    void paintReplaced(const Box& b, float x, float y, float opacity) {
        const ComputedStyle& s = *b.style;
        paintFrame(b, x, y, opacity);
        if (s.hidden) return;
        const float cx = x + b.border[Left] + b.padding[Left];
        const float cy = y + b.border[Top] + b.padding[Top];
        const Node* n = b.node;
        if (n->isHtml("img")) {
            if (const std::string* src = n->attr("src")) {
                if (images_ && images_->imageSize(*src)) {
                    PaintOp op;
                    op.kind = PaintKind::Image;
                    op.x = cx;
                    op.y = cy;
                    op.w = b.width;
                    op.h = b.height;
                    op.text = *src;
                    op.color = faded(Color{255, 255, 255, 255}, opacity);
                    out_->ops.push_back(std::move(op));
                    return;
                }
            }
            if (!b.alt.empty()) addText(cx, cy, b.alt, s, opacity);
            return;
        }
        const std::string shown = controlText(n);
        if (!shown.empty()) {
            const std::string line = shown.substr(0, shown.find('\n'));
            addText(cx, cy, line, s, opacity);
        }
    }

    void paintFragments(const Box& b, float cx, float cy, const Rect& cb, float opacity) {
        for (const auto& f : b.fragments) {
            const float fx = cx + f.x;
            if (f.atomic) {
                const float ax = cx + f.atomic->x, ay = cy + f.atomic->y;
                record(*f.atomic, ax, ay);
                if (f.link) out_->links.push_back({ax, ay, f.atomic->outerWidth(), f.atomic->outerHeight(), *f.link});
                if (f.atomic->kind == BoxKind::Replaced) {
                    paintReplaced(*f.atomic, ax, ay, opacity);
                } else {
                    paintContents(*f.atomic, ax, ay, cb, opacity);
                }
                continue;
            }
            const ComputedStyle& s = *f.style;
            const float fy = cy + f.box_top;
            if (f.link) out_->links.push_back({fx, fy, f.w, f.box_h, *f.link});
            if (s.hidden) continue;
            if (f.background.visible()) addRect(fx, fy, f.w, f.box_h, faded(f.background, opacity));
            if (!f.text.empty()) addText(fx, fy, f.text, s, opacity);
            const float thickness = std::max(1.0f, s.font_size / 14.0f);
            if (s.underline) {
                const float baseline = fy + text_.ascent(fontOf(s));
                addRect(fx, baseline + 1, f.w, thickness, faded(s.color, opacity));
            }
            if (s.line_through) addRect(fx, fy + f.box_h * 0.55f, f.w, thickness, faded(s.color, opacity));
        }
    }

    void record(const Box& b, float x, float y) {
        if (b.node && b.node->isElement()) out_->boxes.push_back({b.node, x, y, b.outerWidth(), b.outerHeight()});
    }

    void paintMarker(const Box& b, float cx, float cy, float opacity) {
        const ComputedStyle& s = *b.style;
        if (s.display != Display::ListItem || s.hidden) return;
        const std::string marker = markerText(s.list_style, b.list_number);
        if (marker.empty()) return;
        const FontSpec font = fontOf(s);
        const float width = text_.width(marker, font);
        // On the first line of content, or where it would be if there's none.
        float top = cy + (lineHeightOf(s) - (text_.ascent(font) + text_.descent(font))) / 2;
        const Box* first = &b;
        float origin = cy;
        while (first && first->fragments.empty()) {
            const Box* next = nullptr;
            for (const auto& k : first->kids) {
                if (k->kind == BoxKind::Block && !k->outOfFlow()) {
                    next = k.get();
                    origin += k->y + k->border[Top] + k->padding[Top];
                    break;
                }
            }
            first = next;
        }
        if (first && !first->fragments.empty() && !first->fragments.front().atomic) top = origin + first->fragments.front().box_top;
        addText(cx - width - 6, top, marker, s, opacity);
    }

    void paintContents(const Box& b, float x, float y, const Rect& cb_in, float opacity_in) {
        const ComputedStyle& s = *b.style;
        const float opacity = opacity_in * s.opacity;
        paintFrame(b, x, y, opacity);
        max_bottom_ = std::max(max_bottom_, y + b.outerHeight());
        const float cx = x + b.border[Left] + b.padding[Left];
        const float cy = y + b.border[Top] + b.padding[Top];
        // Positioned descendants measure from this box's padding box, if it's positioned.
        const Rect cb = s.position != Position::Static
                            ? Rect{x + b.border[Left], y + b.border[Top], b.width + b.padding[Left] + b.padding[Right],
                                   b.height + b.padding[Top] + b.padding[Bottom]}
                            : cb_in;
        paintMarker(b, cx, cy, opacity);
        paintFragments(b, cx, cy, cb, opacity);
        for (const auto& kp : b.kids) {
            const Box& k = *kp;
            if (k.outOfFlow()) {
                positioned_.push_back({&k, cx, cy + k.static_y, cb, opacity, order_++});
                continue;
            }
            if (k.kind == BoxKind::Inline) {
                collectPositionedIn(k, cx, cy, cb, opacity);
                continue;
            }
            if (k.kind != BoxKind::Block) continue;
            paint(k, cx, cy, cb, opacity);
        }
    }

    // Absolute boxes inside inline elements still need painting.
    void collectPositionedIn(const Box& b, float cx, float cy, const Rect& cb, float opacity) {
        for (const auto& kp : b.kids) {
            if (kp->outOfFlow()) positioned_.push_back({kp.get(), cx, cy + kp->static_y, cb, opacity, order_++});
            else if (kp->kind == BoxKind::Inline) collectPositionedIn(*kp, cx, cy, cb, opacity);
        }
    }

    void paint(const Box& b, float parent_x, float parent_y, const Rect& cb, float opacity) {
        const ComputedStyle& s = *b.style;
        float x = parent_x + b.x, y = parent_y + b.y;
        if (s.position == Position::Relative) {
            if (!s.offset[Left].is_auto) x += s.offset[Left].resolve(cb.w);
            else if (!s.offset[Right].is_auto) x -= s.offset[Right].resolve(cb.w);
            if (!s.offset[Top].is_auto) y += s.offset[Top].resolve(cb.h);
            else if (!s.offset[Bottom].is_auto) y -= s.offset[Bottom].resolve(cb.h);
        }
        record(b, x, y);
        paintContents(b, x, y, cb, opacity);
    }

    void flushPositioned() {
        // Painted over everything in flow, lowest z-index first; each may add more.
        size_t safety = 0;
        while (!positioned_.empty() && safety++ < 100000) {
            std::vector<Positioned> batch;
            batch.swap(positioned_);
            std::stable_sort(batch.begin(), batch.end(), [](const Positioned& a, const Positioned& b) {
                return a.box->style->z_index < b.box->style->z_index;
            });
            for (const Positioned& p : batch) {
                const Box& k = *p.box;
                const ComputedStyle& s = *k.style;
                const Rect cb = s.position == Position::Fixed ? Rect{0, 0, vw_, vh_} : p.cb;
                float x = p.static_x + k.margin[Left], y = p.static_y + k.margin[Top];
                if (!s.offset[Left].is_auto) x = cb.x + s.offset[Left].resolve(cb.w) + k.margin[Left];
                else if (!s.offset[Right].is_auto) x = cb.x + cb.w - s.offset[Right].resolve(cb.w) - k.outerWidth() - k.margin[Right];
                if (!s.offset[Top].is_auto) y = cb.y + s.offset[Top].resolve(cb.h) + k.margin[Top];
                else if (!s.offset[Bottom].is_auto) y = cb.y + cb.h - s.offset[Bottom].resolve(cb.h) - k.outerHeight() - k.margin[Bottom];
                record(k, x, y);
                paintContents(k, x, y, cb, p.opacity);
            }
        }
    }

    struct Positioned {
        const Box* box;
        float static_x, static_y;
        Rect cb;
        float opacity;
        size_t order;
    };

    const StyledDocument& styles_;
    TextMeasurer& text_;
    ImageSizes* images_;
    float vw_, vh_;
    std::deque<ComputedStyle> anon_styles_;
    std::vector<Box*> replaced_inner_;
    PageLayout* out_ = nullptr;
    const Box* skip_background_ = nullptr;
    const Box* root_ = nullptr;
    float max_bottom_ = 0;
    std::vector<Positioned> positioned_;
    size_t order_ = 0;
};

}  // namespace

PageLayout layoutPage(const Document& doc, const StyledDocument& styles, TextMeasurer& text, ImageSizes* images,
                      float viewport_width, float viewport_height) {
    return Layouter(styles, text, images, viewport_width, viewport_height).run(doc);
}

}  // namespace fbx::xweb
