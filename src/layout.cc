#include "xweb/layout.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <string>

namespace fbx::xweb {

namespace {

constexpr float kUnbounded = 1e7f;     // "as wide as the content wants"
constexpr float kMaxCoord = 1e8f;      // nothing placed beyond this: keeps floats sane
constexpr float kFitSlack = 0.05f;     // of a pixel: what summing widths can be off by
constexpr int kMaxBoxDepth = 1024;
constexpr int kMaxRelayouts = 20000;   // second passes one page can ask for
constexpr size_t kMaxTableColumns = 1000;

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
    bool collapsed_border = false; // a table with border-collapse: edges are shared
    bool flex_item = false;
    bool anonymous_table_part = false;
    bool quirky_no_top_margin = false, quirky_no_bottom_margin = false;
    float static_y = 0;            // where an absolutely positioned box would have gone
    float pref_width = -1;         // max-content width, once computed
    float min_content = -1;        // min-content width, once computed
    int list_number = 0;
    float last_baseline = -1;      // of the last line box, from the content box's top
    float percent_min_h = 0, percent_max_h = -1;

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
    [[nodiscard]] bool floating() const noexcept { return style && style->float_side != Float::None; }
    [[nodiscard]] bool blockLevel() const noexcept { return kind == BoxKind::Block; }
    [[nodiscard]] bool inFlowBlock() const noexcept { return blockLevel() && !outOfFlow() && !floating(); }
    [[nodiscard]] Display display() const noexcept { return style ? style->display : Display::Block; }
    [[nodiscard]] bool isTable() const noexcept { return display() == Display::Table || display() == Display::InlineTable; }
    [[nodiscard]] bool isFlex() const noexcept { return display() == Display::Flex || display() == Display::InlineFlex; }
    [[nodiscard]] float frameWidth() const noexcept { return padding[Left] + padding[Right] + border[Left] + border[Right]; }
    [[nodiscard]] float frameHeight() const noexcept { return padding[Top] + padding[Bottom] + border[Top] + border[Bottom]; }
    [[nodiscard]] float outerWidth() const noexcept { return width + frameWidth(); }
    [[nodiscard]] float outerHeight() const noexcept { return height + frameHeight(); }
};

// A block formatting context's floats, in its own coordinates.
struct FloatArea {
    float x = 0, y = 0, w = 0, h = 0;   // margin box
    bool left = true;
};

struct Bfc {
    std::vector<FloatArea> floats;

    // The free span across [y, y + h), inside [left, right].
    void freeSpan(float y, float h, float& left, float& right) const {
        for (const FloatArea& f : floats) {
            if (f.y + f.h <= y || f.y >= y + std::max(h, 0.01f)) continue;
            if (f.left) left = std::max(left, f.x + f.w);
            else right = std::min(right, f.x);
        }
    }
    // The next place below `y` where a float ends, or y if none does.
    [[nodiscard]] float nextBottom(float y) const {
        float best = -1;
        for (const FloatArea& f : floats) {
            const float bottom = f.y + f.h;
            if (bottom > y && (best < 0 || bottom < best)) best = bottom;
        }
        return best < 0 ? y : best;
    }
    [[nodiscard]] float clearance(Clear side) const {
        float y = -kMaxCoord;
        for (const FloatArea& f : floats)
            if (side == Clear::Both || (side == Clear::Left) == f.left) y = std::max(y, f.y + f.h);
        return y;
    }
    [[nodiscard]] float bottom() const {
        float y = 0;
        for (const FloatArea& f : floats) y = std::max(y, f.y + f.h);
        return y;
    }
};

// Where a box's containing block's content box is, within its formatting context.
struct Flow {
    Bfc* bfc = nullptr;
    float ox = 0, oy = 0;
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
        quirks_ = doc.quirks;
        auto root = build(root_el, *root_style, 0);
        if (!root) {
            page.height = vh_;
            return page;
        }
        if (root->kind != BoxKind::Block) root->kind = BoxKind::Block;
        root_ = root.get();
        Bfc none;
        layoutBlock(*root, vw_, vh_, Flow{&none, 0, 0}, 0);

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

    const ComputedStyle* anonymousStyle(const ComputedStyle& parent, Display display = Display::Block) {
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
        s.border_spacing = parent.border_spacing;
        s.border_collapse = parent.border_collapse;
        s.display = display;
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

    static BoxKind kindFor(Display d) {
        switch (d) {
            case Display::Inline: return BoxKind::Inline;
            case Display::InlineBlock:
            case Display::InlineTable:
            case Display::InlineFlex: return BoxKind::InlineBlock;
            default: return BoxKind::Block;
        }
    }

    std::unique_ptr<Box> pseudoBox(const ComputedStyle& style) {
        auto box = std::make_unique<Box>();
        box->style = &style;
        box->kind = kindFor(style.display);
        if (!style.content.empty()) {
            auto t = std::make_unique<Box>();
            t->kind = BoxKind::Text;
            t->style = &style;
            t->text = style.content;
            box->kids.push_back(std::move(t));
        }
        finishBox(*box);
        return box;
    }

    std::unique_ptr<Box> build(const Node* el, const ComputedStyle& style, int depth) {
        if (style.display == Display::None || style.display == Display::TableColumn || depth > kMaxBoxDepth) return nullptr;
        auto box = std::make_unique<Box>();
        box->node = el;
        box->style = &style;
        box->kind = kindFor(style.display);
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
                box->kids.push_back(std::move(inner));
            }
            return box;
        }

        // The element's <ol> counter, seen by its list items.
        int counter = 1;
        if (const std::string* start = el->attr("start"))
            counter = static_cast<int>(std::strtol(start->c_str(), nullptr, 10));

        if (const ComputedStyle* before = styles_.beforeOf(el)) box->kids.push_back(pseudoBox(*before));
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
        if (const ComputedStyle* after = styles_.afterOf(el)) box->kids.push_back(pseudoBox(*after));

        finishBox(*box);

        // Quirks mode: the first and last things in a body or cell lose the margins the
        // page didn't set itself.
        if (quirks_ && (el->isHtml("body") || el->isHtml("td") || el->isHtml("th"))) {
            Box* first = nullptr;
            Box* last = nullptr;
            for (auto& k : box->kids) {
                if (!k->inFlowBlock()) continue;
                if (!first) first = k.get();
                last = k.get();
            }
            if (first && first->style->ua_margin_top && first->node) first->quirky_no_top_margin = true;
            if (last && last->style->ua_margin_bottom && last->node) last->quirky_no_bottom_margin = true;
        }
        return box;
    }

    static bool isBlankText(const Box& k) {
        return k.kind == BoxKind::Text && (!k.style || isCollapsible(k.style->white_space)) &&
               std::all_of(k.text.begin(), k.text.end(), isSpaceChar);
    }

    static bool isTablePart(const Box& k) {
        const Display d = k.display();
        return k.kind != BoxKind::Text &&
               (d == Display::TableRow || d == Display::TableRowGroup || d == Display::TableCell || d == Display::TableCaption);
    }

    // Wraps runs of kids failing `fits` in new boxes of the given display.
    void wrapKids(Box& box, Display display, const std::function<bool(const Box&)>& fits, bool drop_blank) {
        std::vector<std::unique_ptr<Box>> result;
        std::unique_ptr<Box> run;
        const auto finish = [&] {
            if (!run) return;
            finishBox(*run);
            result.push_back(std::move(run));
            run.reset();
        };
        for (auto& k : box.kids) {
            if (fits(*k) || k->outOfFlow()) {
                finish();
                result.push_back(std::move(k));
                continue;
            }
            if (drop_blank && !run && isBlankText(*k)) continue;
            if (!run) {
                run = std::make_unique<Box>();
                run->kind = BoxKind::Block;
                run->style = anonymousStyle(*box.style, display);
                run->anonymous_table_part = true;
            }
            run->kids.push_back(std::move(k));
        }
        finish();
        box.kids = std::move(result);
    }

    // Everything a box's children have to be fixed up with, once they're all known.
    void finishBox(Box& box) {
        const Display d = box.display();
        if (d == Display::Table || d == Display::InlineTable) {
            box.collapsed_border = box.style->border_collapse;
            wrapKids(box, Display::TableRowGroup, [](const Box& k) { return k.display() == Display::TableRowGroup || k.display() == Display::TableCaption; }, true);
            return;
        }
        if (d == Display::TableRowGroup) {
            wrapKids(box, Display::TableRow, [](const Box& k) { return k.display() == Display::TableRow; }, true);
            return;
        }
        if (d == Display::TableRow) {
            wrapKids(box, Display::TableCell, [](const Box& k) { return k.display() == Display::TableCell; }, true);
            return;
        }
        if (d == Display::TableCell) box.collapsed_border = box.style->border_collapse;

        // Table parts outside a table get one made for them.
        if (std::any_of(box.kids.begin(), box.kids.end(), [](const auto& k) { return isTablePart(*k); }))
            wrapKids(box, Display::Table, [](const Box& k) { return !isTablePart(k) && !isBlankText(k); }, true);

        if (d == Display::Flex || d == Display::InlineFlex) {
            // Each child is an item, and an item is a block; loose text gets one made.
            wrapKids(box, Display::Block, [](const Box& k) { return k.kind != BoxKind::Text && k.kind != BoxKind::LineBreak; }, true);
            for (auto& k : box.kids) {
                if (k->outOfFlow()) continue;
                k->flex_item = true;
                if (k->kind == BoxKind::Replaced) {
                    auto inner = std::make_unique<Box>();
                    inner->node = k->node;
                    inner->style = k->style;
                    inner->kind = BoxKind::Replaced;
                    inner->alt = k->alt;
                    k->kids.push_back(std::move(inner));
                }
                if (k->kind != BoxKind::Block) {
                    k->kind = BoxKind::Block;
                    wrapInlineRuns(*k);
                }
            }
            return;
        }

        // A block inside an inline: treat the inline as a block too. (The real
        // thing splits it in two around the block; rarely worth the difference.)
        if (box.kind == BoxKind::Inline && std::any_of(box.kids.begin(), box.kids.end(), [](const auto& k) { return k->inFlowBlock(); }))
            box.kind = BoxKind::Block;

        if (box.kind == BoxKind::Block || box.kind == BoxKind::InlineBlock) wrapInlineRuns(box);
    }

    // Block children and inline content can't share a box: inline runs get an
    // anonymous block. Whitespace between blocks disappears here.
    void wrapInlineRuns(Box& box) {
        if (std::none_of(box.kids.begin(), box.kids.end(), [](const auto& k) { return k->inFlowBlock(); })) return;
        std::vector<std::unique_ptr<Box>> result;
        std::unique_ptr<Box> run;
        const auto finish = [&] {
            if (!run) return;
            const bool blank = std::all_of(run->kids.begin(), run->kids.end(), [](const auto& k) { return isBlankText(*k); });
            if (!blank) result.push_back(std::move(run));
            run.reset();
        };
        for (auto& k : box.kids) {
            if (k->inFlowBlock() || k->outOfFlow() || (k->floating() && !run)) {
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

    // While measuring content with "unlimited" room, percentages have nothing to be a
    // percentage of: a width counts as auto, and padding and margins as none.
    [[nodiscard]] static bool widthIsAuto(const ComputedStyle& s, float cb_width) {
        return s.width.is_auto || (s.width.hasPercent() && cb_width >= kUnbounded / 2);
    }

    static void resolveSpacing(Box& b, float cb_width_in) {
        const ComputedStyle& s = *b.style;
        const float cb_width = cb_width_in >= kUnbounded / 2 ? 0 : cb_width_in;
        for (int i = 0; i < 4; ++i) {
            b.padding[i] = std::max(0.0f, clampCoord(s.padding[i].resolve(cb_width)));
            b.border[i] = b.collapsed_border ? s.border_width[i] / 2 : s.border_width[i];
            b.margin[i] = s.margin[i].is_auto ? 0 : clampCoord(s.margin[i].resolve(cb_width));
        }
        if (b.quirky_no_top_margin) b.margin[Top] = 0;
        if (b.quirky_no_bottom_margin) b.margin[Bottom] = 0;
    }

    float clampWidth(const Box& b, float content, float cb_width) const {
        const ComputedStyle& s = *b.style;
        const float extra = s.border_box ? b.frameWidth() : 0;
        const float base = cb_width >= kUnbounded / 2 ? 0 : cb_width;
        if (!s.max_width.is_auto && (!s.max_width.hasPercent() || base > 0))
            content = std::min(content, std::max(0.0f, s.max_width.resolve(base) - extra));
        content = std::max(content, std::max(0.0f, s.min_width.resolve(base) - extra));
        return content;
    }

    [[nodiscard]] bool establishesBfc(const Box& b) const {
        const Display d = b.display();
        return &b == root_ || b.kind == BoxKind::InlineBlock || b.outOfFlow() || b.floating() || b.flex_item ||
               b.style->overflow_clip || d == Display::FlowRoot || d == Display::TableCell || d == Display::TableCaption ||
               d == Display::Flex || d == Display::InlineFlex;
    }

    // `flow` is the containing block's content box; `est_top` where this box's border
    // box is expected to start in it.
    void layoutBlock(Box& b, float cb_width, std::optional<float> cb_height, Flow flow, float est_top) {
        const ComputedStyle& s = *b.style;
        resolveSpacing(b, cb_width);
        const float frame = b.frameWidth();

        // Width (10.3.3). A table takes what its content wants, not all there is.
        float content;
        const bool width_auto = s.width.is_auto;
        if (!width_auto) {
            content = s.width.resolve(cb_width);
            if (s.border_box) content -= frame;
            content = std::max(0.0f, content);
        } else if (b.isTable()) {
            content = std::min(preferredWidth(b), std::max(0.0f, cb_width - frame - b.margin[Left] - b.margin[Right]));
        } else {
            content = std::max(0.0f, cb_width - frame - b.margin[Left] - b.margin[Right]);
        }
        content = clampWidth(b, content, cb_width);
        b.width = clampCoord(content);

        // Auto margins share what's left.
        const float free_space = cb_width - b.width - frame;
        const bool ml_auto = s.margin[Left].is_auto, mr_auto = s.margin[Right].is_auto;
        if (!width_auto || !s.max_width.is_auto || b.isTable()) {
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
            if (s.border_box) h -= b.frameHeight();
            own_height = std::max(0.0f, h);
        }

        // Percentages of a height need a definite one above; only here is it known.
        b.percent_min_h = s.min_height.hasPercent() && cb_height ? s.min_height.resolve(*cb_height) : 0;
        b.percent_max_h = s.max_height.hasPercent() && cb_height ? s.max_height.resolve(*cb_height) : -1;

        Flow inner = flow;
        inner.ox += b.margin[Left] + b.border[Left] + b.padding[Left];
        inner.oy += est_top + b.border[Top] + b.padding[Top];
        layoutContents(b, own_height, inner);
    }

    void layoutContents(Box& b, std::optional<float> own_height, Flow flow) {
        const ComputedStyle& s = *b.style;
        const bool isolated = establishesBfc(b);
        b.collapses_through = false;
        if (b.isTable()) {
            layoutTable(b, own_height);
            b.margin_top_outer = b.margin[Top];
            b.margin_bottom_outer = b.margin[Bottom];
            return;
        }
        if (b.isFlex()) {
            layoutFlex(b, own_height);
            b.margin_top_outer = b.margin[Top];
            b.margin_bottom_outer = b.margin[Bottom];
            return;
        }

        Bfc own;
        if (isolated) flow = Flow{&own, 0, 0};

        const bool inline_content = std::none_of(b.kids.begin(), b.kids.end(), [](const auto& k) { return k->inFlowBlock(); });
        float content_h = 0;
        float mt = b.margin[Top], mb = b.margin[Bottom];
        const bool top_open = !isolated && b.padding[Top] == 0 && b.border[Top] == 0;
        const bool bottom_open = !isolated && b.padding[Bottom] == 0 && b.border[Bottom] == 0 && !own_height;
        // Margins collapse through a box with nothing in the way and no height.
        const bool can_collapse_through = top_open && !isolated && b.padding[Bottom] == 0 && b.border[Bottom] == 0 &&
                                          (!own_height || *own_height == 0) && s.min_height.px <= 0 && !s.min_height.hasPercent();

        if (inline_content) {
            content_h = layoutInline(b, flow);
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
                if (k.floating()) {
                    layoutFloat(k, b.width);
                    placeFloat(k, flow, b.width, pos + (first ? 0 : pending));
                    continue;
                }
                const auto own_margin = [&](const Box& x) { return x.style->margin[Top].is_auto ? 0.0f : x.style->margin[Top].resolve(b.width); };
                const auto topFor = [&](float margin) {
                    if (first && top_open) return 0.0f;
                    return pos + (first ? margin : collapse(pending, margin));
                };

                // Clearance: down past the floats it has to clear.
                float clear_to = -kMaxCoord;
                if (k.style->clear != Clear::None) clear_to = flow.bfc->clearance(k.style->clear) - flow.oy;

                float cb_w = b.width, shift = 0;
                float est = std::max(topFor(own_margin(k)), clear_to);
                // A box with its own formatting context doesn't go under floats: it fits
                // beside them.
                if (establishesBfc(k) && !flow.bfc->floats.empty()) {
                    float left = flow.ox, right = flow.ox + b.width;
                    flow.bfc->freeSpan(flow.oy + est, 1, left, right);
                    shift = left - flow.ox;
                    cb_w = std::max(0.0f, right - left);
                }
                const size_t floats_before = flow.bfc->floats.size();
                Flow child = flow;
                child.ox += shift;
                layoutBlock(k, cb_w, own_height, child, est);
                k.x = k.margin[Left] + shift;

                // Clearance that really moves a box ends any margin collapsing across it.
                const bool cleared = clear_to > topFor(k.margin_top_outer);
                if (cleared) k.collapses_through = false;

                float actual;
                if (k.collapses_through) {
                    actual = est;
                } else if (cleared) {
                    actual = clear_to;
                    pending = 0;
                } else {
                    actual = std::max(topFor(k.margin_top_outer), clear_to);
                    // Its margins turned out to collapse differently: anything it floated
                    // went to the wrong height, so do it again knowing.
                    if (actual != est && flow.bfc->floats.size() > floats_before && relayouts_ < kMaxRelayouts) {
                        ++relayouts_;
                        flow.bfc->floats.resize(floats_before);
                        layoutBlock(k, cb_w, own_height, child, actual);
                    }
                }

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
                if (first && top_open && !cleared) {
                    through_top = collapse(through_top, k.margin_top_outer);
                    k.y = 0;
                } else {
                    pos = actual;
                    k.y = pos;
                }
                first = false;
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

        // A formatting context grows to hold its floats.
        if (isolated) content_h = std::max(content_h, own.bottom());

        float h = own_height.value_or(content_h);
        const float extra = s.border_box ? b.frameHeight() : 0;
        if (!s.max_height.is_auto && !s.max_height.hasPercent()) h = std::min(h, std::max(0.0f, s.max_height.px - extra));
        else if (b.percent_max_h >= 0) h = std::min(h, std::max(0.0f, b.percent_max_h - extra));
        if (!s.min_height.hasPercent()) h = std::max(h, std::max(0.0f, s.min_height.px - extra));
        else h = std::max(h, std::max(0.0f, b.percent_min_h - extra));
        b.height = clampCoord(h);
        b.margin_top_outer = mt;
        b.margin_bottom_outer = mb;
    }

    void layoutOutOfFlow(Box& k, float cb_width) {
        const ComputedStyle& s = *k.style;
        const float base = s.position == Position::Fixed ? vw_ : cb_width;
        resolveSpacing(k, base);
        const float frame = k.frameWidth();
        float width;
        if (!widthIsAuto(s, base)) {
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
            if (s.border_box) h -= k.frameHeight();
            own = std::max(0.0f, h);
        } else if (!s.height.is_auto) {
            own = std::max(0.0f, s.height.resolve(vh_));
        }
        layoutContents(k, own, Flow{});
    }

    void layoutFloat(Box& k, float cb_width) {
        const ComputedStyle& s = *k.style;
        resolveSpacing(k, cb_width);
        const float frame = k.frameWidth();
        float width;
        if (!widthIsAuto(s, cb_width)) {
            width = s.width.resolve(cb_width);
            if (s.border_box) width -= frame;
        } else {
            width = std::min(preferredWidth(k), std::max(0.0f, cb_width - frame - k.margin[Left] - k.margin[Right]));
        }
        k.width = clampWidth(k, std::max(0.0f, width), cb_width);
        std::optional<float> own;
        if (!s.height.is_auto && !s.height.hasPercent()) {
            float h = s.height.px;
            if (s.border_box) h -= k.frameHeight();
            own = std::max(0.0f, h);
        }
        layoutContents(k, own, Flow{});
    }

    // Finds the float its place (9.5.1) and records it. `y` is the earliest it may
    // start, in the containing block.
    void placeFloat(Box& k, Flow flow, float cb_width, float y) {
        Bfc& bfc = *flow.bfc;
        const float outer_w = k.outerWidth() + k.margin[Left] + k.margin[Right];
        const float outer_h = k.outerHeight() + k.margin[Top] + k.margin[Bottom];
        float top = flow.oy + y;
        // Never above a float that came earlier.
        for (const FloatArea& f : bfc.floats) top = std::max(top, f.y);
        if (k.style->clear != Clear::None) top = std::max(top, bfc.clearance(k.style->clear));
        const bool left_side = k.style->float_side == Float::Left;
        float left = flow.ox, right = flow.ox + cb_width;
        for (int guard = 0; guard < 10000; ++guard) {
            left = flow.ox;
            right = flow.ox + cb_width;
            bfc.freeSpan(top, outer_h, left, right);
            if (right - left + kFitSlack >= outer_w) break;
            const float next = bfc.nextBottom(top);
            if (next <= top) break;   // nothing below to wait for: it overflows here
            top = next;
        }
        FloatArea area;
        area.w = outer_w;
        area.h = outer_h;
        area.y = top;
        area.left = left_side;
        area.x = left_side ? left : right - outer_w;
        bfc.floats.push_back(area);
        k.x = area.x - flow.ox + k.margin[Left];
        k.y = area.y - flow.oy + k.margin[Top];
    }

    // ── Intrinsic widths ──────────────────────────────────────────────────────

    // The width the content would take with nowhere to wrap.
    float preferredWidth(Box& b) {
        if (b.pref_width >= 0) return b.pref_width;
        const ComputedStyle& s = *b.style;
        if (!s.width.is_auto && !s.width.hasPercent()) {
            b.pref_width = std::max(0.0f, s.width.px);
            return b.pref_width;
        }
        b.pref_width = std::min(intrinsicWidth(b, true), kUnbounded);
        return b.pref_width;
    }

    // The narrowest it can go: its longest word.
    float minContentWidth(Box& b) {
        if (b.min_content >= 0) return b.min_content;
        b.min_content = std::min(intrinsicWidth(b, false), kUnbounded);
        return b.min_content;
    }

    float outerIntrinsic(Box& k, bool max_content) {
        resolveSpacing(k, 0);
        float w = max_content ? preferredWidth(k) : minContentWidth(k);
        const ComputedStyle& s = *k.style;
        if (!s.width.is_auto && !s.width.hasPercent()) w = s.border_box ? std::max(0.0f, s.width.px - k.frameWidth()) : s.width.px;
        return w + k.frameWidth() + k.margin[Left] + k.margin[Right];
    }

    float intrinsicWidth(Box& b, bool max_content) {
        if (b.isTable()) {
            const TableGrid grid = tableGrid(b);
            std::vector<float> mins, maxes;
            columnWidths(grid, mins, maxes);
            float total = 0;
            for (const float v : (max_content ? maxes : mins)) total += v;
            return total + tableSpacing(b) * static_cast<float>(mins.size() + 1);
        }
        if (b.isFlex()) {
            const bool row = !b.style->flex_column;
            float total = 0;
            for (auto& k : b.kids) {
                if (k->outOfFlow()) continue;
                const float w = outerIntrinsic(*k, max_content);
                if (row && (max_content || !b.style->flex_wrap)) total += w;
                else total = std::max(total, w);
            }
            return total;
        }
        const bool inline_content = std::none_of(b.kids.begin(), b.kids.end(), [](const auto& k) { return k->inFlowBlock(); });
        float result = 0;
        if (inline_content) {
            const float saved = b.width;
            b.width = max_content ? kUnbounded : 0.0f;
            Bfc scratch;
            (void)layoutInline(b, Flow{&scratch, 0, 0});
            for (const auto& f : b.fragments)
                if (f.x + f.w < kUnbounded / 2) result = std::max(result, f.x + f.w);
            float floats = 0;
            for (const FloatArea& f : scratch.floats) {
                if (max_content) floats += f.w;
                else floats = std::max(floats, f.w);
            }
            result = max_content ? result : std::max(result, floats);
            if (max_content) {
                float text = 0;
                for (const auto& f : b.fragments) text = std::max(text, f.w);
                result = std::max(result, text + floats);
            }
            b.width = saved;
        } else {
            float floats = 0;
            for (auto& k : b.kids) {
                if (k->outOfFlow()) continue;
                const float w = outerIntrinsic(*k, max_content);
                if (k->floating() && max_content) floats += w;
                else result = std::max(result, w);
            }
            result = std::max(result, floats);
        }
        return result;
    }

    void layoutInlineBlock(Box& b, float cb_width) {
        const ComputedStyle& s = *b.style;
        resolveSpacing(b, cb_width);
        const float frame = b.frameWidth();
        float width;
        if (!widthIsAuto(s, cb_width)) {
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
            if (s.border_box) h -= b.frameHeight();
            own = std::max(0.0f, h);
        }
        layoutContents(b, own, Flow{});
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
            } else {
                // A browser's size="20" default: 20 average characters.
                float chars = 20;
                if (const std::string* size = n->attr("size")) {
                    const float v = std::strtof(size->c_str(), nullptr);
                    if (v > 0 && v < 10000) chars = v;
                }
                natural_w = chars * s.font_size * 0.75f;
                natural_h = lineHeightOf(s);
                const FontSpec font = fontOf(s);
                b.last_baseline = text_.ascent(font) + (natural_h - text_.ascent(font) - text_.descent(font)) / 2;
                const std::string shown = controlText(n);
                if (ty && (*ty == "submit" || *ty == "button" || *ty == "reset"))
                    natural_w = text_.width(shown.empty() ? "Submit" : shown, fontOf(s)) + 8;
            }
        } else if (n->isHtml("textarea")) {
            natural_w = 20 * s.font_size * 0.73f;
            natural_h = lineHeightOf(s) * 2;
            if (const std::string* rows = n->attr("rows")) {
                const float v = std::strtof(rows->c_str(), nullptr);
                if (v > 0 && v < 10000) natural_h = lineHeightOf(s) * v;
            }
            if (const std::string* cols = n->attr("cols")) {
                const float v = std::strtof(cols->c_str(), nullptr);
                if (v > 0 && v < 10000) natural_w = v * s.font_size * 0.73f;
            }
        } else if (n->isHtml("select")) {
            const FontSpec font = fontOf(s);
            natural_w = text_.width(controlText(n), font) + 20;   // the arrow's room
            natural_h = lineHeightOf(s) + 2;
            b.last_baseline = text_.ascent(font) + (natural_h - text_.ascent(font) - text_.descent(font)) / 2;
        } else if (n->isHtml("progress") || n->isHtml("meter")) {
            natural_w = 160;
            natural_h = 16;
        }
        const bool has_w = !widthIsAuto(s, cb_width);
        const bool has_h = !s.height.is_auto && !s.height.hasPercent();
        float w = natural_w, h = natural_h;
        if (has_w) {
            w = s.width.resolve(cb_width);
            if (s.border_box) w -= b.frameWidth();
        }
        if (has_h) {
            h = s.height.px;
            if (s.border_box) h -= b.frameHeight();
        }
        // Only pictures keep their shape: a text field made wider doesn't get taller.
        const bool has_ratio = !(n->isHtml("input") || n->isHtml("textarea") || n->isHtml("select") || n->isHtml("progress") || n->isHtml("meter"));
        const float ratio = (has_ratio && natural_w > 0 && natural_h > 0) ? natural_w / natural_h : 0;
        if (has_w && !has_h && ratio > 0) h = w / ratio;
        if (has_h && !has_w && ratio > 0) w = h * ratio;
        const float auto_w = w;
        w = clampWidth(b, std::max(0.0f, w), cb_width);
        // max-width: 100% on an image keeps its shape.
        if (w != auto_w && !has_h && ratio > 0) h = w / ratio;
        b.width = w;
        b.height = std::max(0.0f, h);
    }

    // ── Tables ────────────────────────────────────────────────────────────────

    struct TableCell {
        Box* box;
        size_t row, col, rowspan, colspan;
    };

    struct TableRowInfo {
        Box* box;
        Box* group;
    };

    struct TableGrid {
        std::vector<TableCell> cells;
        std::vector<TableRowInfo> rows;
        size_t columns = 0;
        float spacing = 0;
        bool fixed = false;
    };

    static size_t spanOf(const Box& cell, const char* name, size_t limit) {
        if (!cell.node) return 1;
        const std::string* v = cell.node->attr(name);
        if (!v) return 1;
        const long n = std::strtol(v->c_str(), nullptr, 10);
        return n >= 1 ? std::min(static_cast<size_t>(n), limit) : 1;
    }

    static float tableSpacing(const Box& t) { return t.style->border_collapse ? 0 : t.style->border_spacing; }

    TableGrid tableGrid(Box& t) {
        TableGrid grid;
        grid.fixed = t.style->table_fixed && !t.style->width.is_auto;
        grid.spacing = tableSpacing(t);
        // Header groups go first and footers last, wherever they were written.
        std::vector<Box*> groups;
        const auto groupRank = [](const Box& g) {
            if (!g.node) return 1;
            return g.node->isHtml("thead") ? 0 : g.node->isHtml("tfoot") ? 2 : 1;
        };
        for (int rank = 0; rank < 3; ++rank)
            for (auto& g : t.kids)
                if (g->display() == Display::TableRowGroup && groupRank(*g) == rank) groups.push_back(g.get());
        std::vector<size_t> open_until;   // per column: the row a rowspan runs to
        for (Box* g : groups) {
            const size_t first_row = grid.rows.size();
            size_t group_rows = 0;
            for (auto& r : g->kids)
                if (r->display() == Display::TableRow) ++group_rows;
            for (auto& r : g->kids) {
                if (r->display() != Display::TableRow) continue;
                const size_t row = grid.rows.size();
                grid.rows.push_back({r.get(), g});
                size_t col = 0;
                for (auto& c : r->kids) {
                    if (c->display() != Display::TableCell) continue;
                    while (col < open_until.size() && open_until[col] > row) ++col;
                    const size_t colspan = spanOf(*c, "colspan", kMaxTableColumns);
                    const size_t rowspan = spanOf(*c, "rowspan", first_row + group_rows - row);
                    if (col + colspan > kMaxTableColumns) break;
                    if (open_until.size() < col + colspan) open_until.resize(col + colspan, 0);
                    for (size_t k = col; k < col + colspan; ++k) open_until[k] = row + rowspan;
                    grid.cells.push_back({c.get(), row, col, rowspan, colspan});
                    col += colspan;
                }
            }
            open_until.assign(open_until.size(), 0);   // rowspans end with their group
        }
        grid.columns = open_until.size();
        return grid;
    }

    void columnWidths(const TableGrid& grid, std::vector<float>& mins, std::vector<float>& maxes) {
        mins.assign(grid.columns, 0);
        maxes.assign(grid.columns, 0);
        const auto frameOf = [](Box& c) {
            resolveSpacing(c, 0);
            return c.frameWidth();
        };
        for (int pass = 0; pass < 2; ++pass) {   // single-column cells first, then spans
            for (const TableCell& c : grid.cells) {
                if ((pass == 0) != (c.colspan == 1)) continue;
                Box& box = *c.box;
                const float frame = frameOf(box);
                const ComputedStyle& s = *box.style;
                float min_w = minContentWidth(box) + frame;
                float max_w = preferredWidth(box) + frame;
                if (!s.width.is_auto && !s.width.hasPercent()) {
                    const float spec = s.width.px + (s.border_box ? 0 : frame);
                    min_w = std::max(min_w, spec);
                    max_w = std::max(min_w, spec);
                }
                max_w = std::max(max_w, min_w);
                if (c.colspan == 1) {
                    mins[c.col] = std::max(mins[c.col], min_w);
                    maxes[c.col] = std::max(maxes[c.col], max_w);
                } else {
                    // A span also has the spacing between its columns to use.
                    float have_min = grid.spacing * static_cast<float>(c.colspan - 1), have_max = have_min;
                    for (size_t k = c.col; k < c.col + c.colspan; ++k) {
                        have_min += mins[k];
                        have_max += maxes[k];
                    }
                    const float n = static_cast<float>(c.colspan);
                    if (min_w > have_min)
                        for (size_t k = c.col; k < c.col + c.colspan; ++k) mins[k] += (min_w - have_min) / n;
                    if (max_w > have_max)
                        for (size_t k = c.col; k < c.col + c.colspan; ++k) maxes[k] += (max_w - have_max) / n;
                }
            }
        }
        for (size_t k = 0; k < grid.columns; ++k) maxes[k] = std::max(maxes[k], mins[k]);
    }

    void layoutTable(Box& t, std::optional<float> own_height) {
        TableGrid grid = tableGrid(t);
        const float spacing = tableSpacing(t);
        const float gaps = spacing * static_cast<float>(grid.columns + 1);
        std::vector<float> mins, maxes;
        columnWidths(grid, mins, maxes);
        std::vector<float> widths(grid.columns, 0);
        float inner = std::max(0.0f, t.width - gaps);

        // Percent widths are honoured first.
        std::vector<float> percent(grid.columns, 0);
        for (const TableCell& c : grid.cells)
            if (c.colspan == 1 && c.box->style->width.hasPercent())
                percent[c.col] = std::max(percent[c.col], c.box->style->width.percent);

        float sum_min = 0, sum_max = 0, reserved = 0;
        for (size_t k = 0; k < grid.columns; ++k) {
            if (percent[k] > 0) {
                widths[k] = std::max(mins[k], percent[k] * inner / 100);
                reserved += widths[k];
            } else {
                sum_min += mins[k];
                sum_max += maxes[k];
            }
        }
        const float rest = std::max(0.0f, inner - reserved);
        if (grid.fixed) {
            // Fixed layout: the first row decides, content doesn't.
            float claimed = 0;
            size_t unclaimed = 0;
            std::vector<bool> set(grid.columns, false);
            for (const TableCell& c : grid.cells) {
                if (c.row != 0) continue;
                const ComputedStyle& s = *c.box->style;
                if (!s.width.is_auto && c.colspan == 1) {
                    resolveSpacing(*c.box, inner);
                    widths[c.col] = s.width.resolve(inner) + (s.border_box ? 0 : c.box->frameWidth());
                    claimed += widths[c.col];
                    set[c.col] = true;
                }
            }
            for (size_t k = 0; k < grid.columns; ++k) unclaimed += set[k] ? 0 : 1;
            for (size_t k = 0; k < grid.columns; ++k)
                if (!set[k]) widths[k] = std::max(0.0f, inner - claimed) / static_cast<float>(unclaimed);
        } else {
            for (size_t k = 0; k < grid.columns; ++k) {
                if (percent[k] > 0) continue;
                if (rest >= sum_max) {
                    // More room than wanted: a table given a width spreads it out.
                    const float share = sum_max > 0 ? maxes[k] / sum_max : 1.0f / static_cast<float>(grid.columns);
                    widths[k] = maxes[k] + share * (rest - sum_max);
                } else if (rest > sum_min) {
                    const float span = sum_max - sum_min;
                    widths[k] = mins[k] + (span > 0 ? (maxes[k] - mins[k]) * (rest - sum_min) / span : 0);
                } else {
                    widths[k] = mins[k];
                }
            }
        }
        float used = gaps;
        for (const float w : widths) used += w;
        t.width = std::max(t.width, used);

        std::vector<float> col_x(grid.columns + 1, spacing);
        for (size_t k = 0; k < grid.columns; ++k) col_x[k + 1] = col_x[k] + widths[k] + spacing;

        // Cells at their column widths, which tells each row its height.
        std::vector<float> row_h(grid.rows.size(), 0);
        for (TableRowInfo& r : grid.rows) {
            const ComputedStyle& s = *r.box->style;
            if (!s.height.is_auto && !s.height.hasPercent())
                row_h[static_cast<size_t>(&r - grid.rows.data())] = s.height.px;
        }
        for (const TableCell& c : grid.cells) {
            Box& box = *c.box;
            resolveSpacing(box, t.width);
            box.width = std::max(0.0f, col_x[c.col + c.colspan] - col_x[c.col] - spacing - box.frameWidth());
            layoutContents(box, std::nullopt, Flow{});
            const ComputedStyle& s = *box.style;
            if (!s.height.is_auto && !s.height.hasPercent()) {
                const float spec = s.height.px - (s.border_box ? box.frameHeight() : 0);
                box.height = std::max(box.height, spec);
            }
            if (c.rowspan == 1) row_h[c.row] = std::max(row_h[c.row], box.outerHeight());
        }
        for (const TableCell& c : grid.cells) {
            if (c.rowspan == 1) continue;
            float have = spacing * static_cast<float>(c.rowspan - 1);
            for (size_t k = c.row; k < c.row + c.rowspan; ++k) have += row_h[k];
            if (c.box->outerHeight() > have) row_h[c.row + c.rowspan - 1] += c.box->outerHeight() - have;
        }

        // Caption, then rows.
        float y = 0;
        for (auto& k : t.kids) {
            if (k->display() != Display::TableCaption) continue;
            resolveSpacing(*k, t.width);
            k->width = std::max(0.0f, t.width - k->frameWidth() - k->margin[Left] - k->margin[Right]);
            layoutContents(*k, std::nullopt, Flow{});
            k->x = k->margin[Left];
            k->y = y + k->margin[Top];
            y += k->outerHeight() + k->margin[Top] + k->margin[Bottom];
        }
        float rows_total = spacing;
        for (const float h : row_h) rows_total += h + spacing;
        if (own_height && *own_height > y + rows_total && !row_h.empty()) {
            const float extra = (*own_height - y - rows_total) / static_cast<float>(row_h.size());
            for (float& h : row_h) h += extra;
            rows_total = *own_height - y;
        }

        std::vector<float> row_y(grid.rows.size(), 0);
        float cursor = y + spacing;
        for (size_t i = 0; i < grid.rows.size(); ++i) {
            row_y[i] = cursor;
            cursor += row_h[i] + spacing;
        }

        // Rows sit in their groups, and cells in their rows.
        const float inner_width = t.width - spacing * 2;
        for (size_t i = 0; i < grid.rows.size(); ++i) {
            Box& row = *grid.rows[i].box;
            Box& group = *grid.rows[i].group;
            if (i == 0 || grid.rows[i - 1].group != &group) {
                group.x = spacing;
                group.y = row_y[i];
                group.width = inner_width;
                group.height = 0;
                for (size_t j = i; j < grid.rows.size() && grid.rows[j].group == &group; ++j)
                    group.height = row_y[j] + row_h[j] - row_y[i];
            }
            row.x = 0;
            row.y = row_y[i] - group.y;
            row.width = inner_width;
            row.height = row_h[i];
        }
        // Groups with no rows take no room.
        for (auto& g : t.kids)
            if (g->display() == Display::TableRowGroup && g->kids.empty()) g->height = 0;

        for (const TableCell& c : grid.cells) {
            Box& box = *c.box;
            float span_h = spacing * static_cast<float>(c.rowspan - 1);
            for (size_t k = c.row; k < c.row + c.rowspan; ++k) span_h += row_h[k];
            const float content_h = box.height;
            const float room = std::max(0.0f, span_h - box.frameHeight() - content_h);
            // vertical-align moves the content, not the cell.
            const VerticalAlign va = box.style->vertical_align;
            const float above = va == VerticalAlign::Middle ? room / 2 : va == VerticalAlign::Bottom ? room : 0;
            box.padding[Top] += above;
            box.padding[Bottom] += room - above;
            box.x = col_x[c.col] - spacing;
            box.y = 0;
        }

        t.height = clampCoord(std::max(cursor, y + rows_total));
        if (grid.rows.empty()) t.height = y;
        if (own_height) t.height = std::max(t.height, *own_height);
    }

    // ── Flexbox ───────────────────────────────────────────────────────────────

    struct FlexItem {
        Box* box;
        float base = 0;        // flex base size, content box
        float min_main = 0, max_main = kMaxCoord;
        float main = 0;        // resolved, content box
        float frame_main = 0;  // padding, border and margins along the main axis
        float frame_cross = 0;
        bool frozen = false;
        float cross_outer = 0;
    };

    // Lays out a flex item at a given content width, and height if it's fixed.
    void layoutItem(Box& k, float width, std::optional<float> height) {
        k.width = std::max(0.0f, width);
        layoutContents(k, height, Flow{});
    }

    std::optional<float> specifiedHeight(const Box& k, std::optional<float> cb_height) const {
        const ComputedStyle& s = *k.style;
        if (s.height.is_auto || (s.height.hasPercent() && !cb_height)) return std::nullopt;
        float h = s.height.resolve(cb_height.value_or(0));
        if (s.border_box) h -= k.frameHeight();
        return std::max(0.0f, h);
    }

    void layoutFlex(Box& c, std::optional<float> own_height) {
        const ComputedStyle& s = *c.style;
        const bool row = !s.flex_column;
        const float gap_main = row ? s.column_gap : s.row_gap;
        const float gap_cross = row ? s.row_gap : s.column_gap;
        const std::optional<float> main_avail = row ? std::optional<float>(c.width) : own_height;
        const std::optional<float> cross_avail = row ? own_height : std::optional<float>(c.width);

        std::vector<FlexItem> items;
        for (auto& kp : c.kids) {
            if (kp->outOfFlow()) {
                kp->static_y = 0;
                layoutOutOfFlow(*kp, c.width);
                continue;
            }
            FlexItem it;
            it.box = kp.get();
            items.push_back(it);
        }
        std::stable_sort(items.begin(), items.end(), [](const FlexItem& a, const FlexItem& b) { return a.box->style->order < b.box->style->order; });

        for (FlexItem& it : items) {
            Box& k = *it.box;
            const ComputedStyle& ks = *k.style;
            resolveSpacing(k, c.width);
            it.frame_main = row ? k.frameWidth() + k.margin[Left] + k.margin[Right] : k.frameHeight() + k.margin[Top] + k.margin[Bottom];
            it.frame_cross = row ? k.frameHeight() + k.margin[Top] + k.margin[Bottom] : k.frameWidth() + k.margin[Left] + k.margin[Right];
            const float box_extra = ks.border_box ? (row ? k.frameWidth() : k.frameHeight()) : 0;
            const Length& main_size = row ? ks.width : ks.height;
            const Length& min_size = row ? ks.min_width : ks.min_height;
            const Length& max_size = row ? ks.max_width : ks.max_height;
            const float base_for_percent = main_avail.value_or(0);

            if (!ks.flex_basis.is_auto && (!ks.flex_basis.hasPercent() || main_avail)) {
                it.base = ks.flex_basis.resolve(base_for_percent) - box_extra;
            } else if (!main_size.is_auto && (!main_size.hasPercent() || main_avail)) {
                it.base = main_size.resolve(base_for_percent) - box_extra;
            } else if (row) {
                it.base = preferredWidth(k);
            } else {
                // A column's base size is the height at the width it will get.
                const float width = columnItemWidth(c, it);
                layoutItem(k, width, std::nullopt);
                it.base = k.height;
            }
            it.base = std::max(0.0f, it.base);
            // Automatic minimum: an item doesn't shrink below its content.
            if (min_size.px > 0 || min_size.hasPercent()) {
                it.min_main = std::max(0.0f, min_size.resolve(base_for_percent) - box_extra);
            } else if (row && !ks.overflow_clip) {
                it.min_main = minContentWidth(k);
                if (!main_size.is_auto && !main_size.hasPercent()) it.min_main = std::min(it.min_main, std::max(0.0f, main_size.px - box_extra));
            } else if (!row && !ks.overflow_clip) {
                it.min_main = std::min(it.base, it.base);   // content height, already measured above
                if (!ks.flex_basis.is_auto || !main_size.is_auto) it.min_main = 0;
            }
            if (!max_size.is_auto) it.max_main = std::max(0.0f, max_size.resolve(base_for_percent) - box_extra);
            if (it.max_main < it.min_main) it.max_main = it.min_main;
        }

        // Lines
        std::vector<std::vector<FlexItem*>> lines;
        if (s.flex_wrap && main_avail) {
            std::vector<FlexItem*> line;
            float used = 0;
            for (FlexItem& it : items) {
                const float hyp = std::clamp(it.base, it.min_main, it.max_main) + it.frame_main;
                if (!line.empty() && used + gap_main + hyp > *main_avail) {
                    lines.push_back(std::move(line));
                    line.clear();
                    used = 0;
                }
                used += (line.empty() ? 0 : gap_main) + hyp;
                line.push_back(&it);
            }
            if (!line.empty()) lines.push_back(std::move(line));
        } else if (!items.empty()) {
            std::vector<FlexItem*> line;
            for (FlexItem& it : items) line.push_back(&it);
            lines.push_back(std::move(line));
        }

        // Each line's flexible lengths (9.7).
        for (auto& line : lines) {
            for (FlexItem* it : line) {
                it->frozen = false;
                it->main = std::clamp(it->base, it->min_main, it->max_main);
            }
            if (!main_avail) continue;
            const float gaps = gap_main * static_cast<float>(line.size() - 1);
            float hypothetical = gaps;
            for (FlexItem* it : line) hypothetical += it->main + it->frame_main;
            const bool growing = hypothetical < *main_avail;
            for (FlexItem* it : line) {
                const float factor = growing ? it->box->style->flex_grow : it->box->style->flex_shrink;
                if (factor == 0 || (growing && it->base > it->main) || (!growing && it->base < it->main)) it->frozen = true;
            }
            for (size_t round = 0; round <= line.size(); ++round) {
                float used = gaps, total_factor = 0;
                bool any = false;
                for (FlexItem* it : line) {
                    if (it->frozen) {
                        used += it->main + it->frame_main;
                    } else {
                        any = true;
                        used += it->base + it->frame_main;
                        total_factor += growing ? it->box->style->flex_grow : it->box->style->flex_shrink * it->base;
                    }
                }
                if (!any) break;
                float free_space = *main_avail - used;
                // Factors summing below 1 use only that share of the space.
                if (growing && total_factor < 1) free_space *= total_factor;
                float violation = 0;
                std::vector<std::pair<FlexItem*, float>> changes;
                for (FlexItem* it : line) {
                    if (it->frozen) continue;
                    const float factor = growing ? it->box->style->flex_grow : it->box->style->flex_shrink * it->base;
                    const float share = total_factor > 0 ? free_space * factor / (growing && total_factor < 1 ? std::max(total_factor, 0.0001f) : total_factor) : 0;
                    const float target = it->base + share;
                    const float clamped = std::clamp(target, it->min_main, it->max_main);
                    violation += clamped - target;
                    changes.emplace_back(it, clamped);
                }
                for (auto& [it, value] : changes) {
                    const float target_diff = value - it->main;
                    it->main = value;
                    (void)target_diff;
                }
                if (std::abs(violation) < 0.01f) break;
                // Freeze those that hit a limit in the direction of the total violation.
                for (auto& [it, value] : changes) {
                    if (violation > 0 && value <= it->min_main + 0.001f) it->frozen = true;
                    else if (violation < 0 && value >= it->max_main - 0.001f) it->frozen = true;
                }
            }
        }

        // Cross sizes
        std::vector<float> line_cross(lines.size(), 0);
        for (size_t li = 0; li < lines.size(); ++li) {
            for (FlexItem* it : lines[li]) {
                Box& k = *it->box;
                if (row) {
                    layoutItem(k, it->main, specifiedHeight(k, own_height));
                } else {
                    layoutItem(k, columnItemWidth(c, *it), it->main);
                }
                it->cross_outer = (row ? k.outerHeight() : k.outerWidth()) + (row ? k.margin[Top] + k.margin[Bottom] : k.margin[Left] + k.margin[Right]);
                line_cross[li] = std::max(line_cross[li], it->cross_outer);
            }
        }
        float cross_total = gap_cross * static_cast<float>(lines.empty() ? 0 : lines.size() - 1);
        for (const float v : line_cross) cross_total += v;

        // A definite cross size is shared out among the lines.
        float cross_start = 0, cross_between = gap_cross;
        if (cross_avail && !lines.empty()) {
            const float free_cross = *cross_avail - cross_total;
            const auto n = static_cast<float>(lines.size());
            if (lines.size() == 1 && !s.flex_wrap) {
                line_cross[0] = *cross_avail;
            } else if (free_cross > 0) {
                switch (s.align_content) {
                    case FlexAlign::Stretch:
                    case FlexAlign::Auto:
                        for (float& v : line_cross) v += free_cross / n;
                        break;
                    case FlexAlign::End: cross_start = free_cross; break;
                    case FlexAlign::Center: cross_start = free_cross / 2; break;
                    case FlexAlign::SpaceBetween:
                        if (lines.size() > 1) cross_between += free_cross / (n - 1);
                        break;
                    case FlexAlign::SpaceAround:
                        cross_start = free_cross / n / 2;
                        cross_between += free_cross / n;
                        break;
                    case FlexAlign::SpaceEvenly:
                        cross_start = free_cross / (n + 1);
                        cross_between += free_cross / (n + 1);
                        break;
                    default: break;
                }
            }
        }

        // Positions
        float cross_pos = cross_start;
        float main_extent = 0;
        for (size_t li = 0; li < lines.size(); ++li) {
            auto& line = lines[li];
            float used = gap_main * static_cast<float>(line.size() - 1);
            int auto_margins = 0;
            for (FlexItem* it : line) {
                used += it->main + it->frame_main;
                const ComputedStyle& ks = *it->box->style;
                auto_margins += (row ? ks.margin[Left].is_auto : ks.margin[Top].is_auto) ? 1 : 0;
                auto_margins += (row ? ks.margin[Right].is_auto : ks.margin[Bottom].is_auto) ? 1 : 0;
            }
            main_extent = std::max(main_extent, used);
            const float room = main_avail.value_or(used);
            const float free_main = room - used;
            const float per_auto = (auto_margins > 0 && free_main > 0) ? free_main / static_cast<float>(auto_margins) : 0;
            float pos = 0, between = gap_main;
            if (auto_margins == 0 && free_main > 0) {
                const auto n = static_cast<float>(line.size());
                switch (s.justify_content) {
                    case FlexAlign::End: pos = free_main; break;
                    case FlexAlign::Center: pos = free_main / 2; break;
                    case FlexAlign::SpaceBetween:
                        if (line.size() > 1) between += free_main / (n - 1);
                        break;
                    case FlexAlign::SpaceAround:
                        pos = free_main / n / 2;
                        between += free_main / n;
                        break;
                    case FlexAlign::SpaceEvenly:
                        pos = free_main / (n + 1);
                        between += free_main / (n + 1);
                        break;
                    default: break;
                }
            } else if (auto_margins == 0 && free_main < 0) {
                if (s.justify_content == FlexAlign::End) pos = free_main;
                else if (s.justify_content == FlexAlign::Center) pos = free_main / 2;
            }
            for (FlexItem* it : line) {
                Box& k = *it->box;
                const ComputedStyle& ks = *k.style;
                const FlexAlign align = ks.align_self == FlexAlign::Auto ? s.align_items : ks.align_self;
                const bool cross_auto = row ? ks.height.is_auto : ks.width.is_auto;
                const bool cross_margin_auto = row ? (ks.margin[Top].is_auto || ks.margin[Bottom].is_auto) : (ks.margin[Left].is_auto || ks.margin[Right].is_auto);
                if (align == FlexAlign::Stretch && cross_auto && !cross_margin_auto) {
                    const float inner_cross = std::max(0.0f, line_cross[li] - it->frame_cross);
                    if (row) {
                        if (std::abs(k.height - inner_cross) > 0.01f) layoutItem(k, it->main, inner_cross);
                    } else if (std::abs(k.width - inner_cross) > 0.01f) {
                        layoutItem(k, inner_cross, it->main);
                    }
                    it->cross_outer = line_cross[li];
                }
                float cross_free = line_cross[li] - it->cross_outer;
                float cross_at = cross_pos;
                if (cross_margin_auto && cross_free > 0) {
                    const bool first_auto = row ? ks.margin[Top].is_auto : ks.margin[Left].is_auto;
                    const bool second_auto = row ? ks.margin[Bottom].is_auto : ks.margin[Right].is_auto;
                    if (first_auto && second_auto) cross_at += cross_free / 2;
                    else if (first_auto) cross_at += cross_free;
                } else if (align == FlexAlign::End) {
                    cross_at += cross_free;
                } else if (align == FlexAlign::Center) {
                    cross_at += cross_free / 2;
                }
                float lead = row ? k.margin[Left] : k.margin[Top];
                float trail = row ? k.margin[Right] : k.margin[Bottom];
                if ((row ? ks.margin[Left].is_auto : ks.margin[Top].is_auto)) lead += per_auto;
                if ((row ? ks.margin[Right].is_auto : ks.margin[Bottom].is_auto)) trail += per_auto;
                const float main_outer = it->main + (row ? k.frameWidth() : k.frameHeight());
                float main_at = pos + lead;
                if (s.flex_reverse) main_at = room - main_at - main_outer;
                if (row) {
                    k.x = main_at;
                    k.y = cross_at + k.margin[Top];
                } else {
                    k.y = main_at;
                    k.x = cross_at + k.margin[Left];
                }
                pos += lead + main_outer + trail + between;
            }
            cross_pos += line_cross[li] + cross_between;
        }

        float cross_extent = 0;
        for (const float v : line_cross) cross_extent += v;
        cross_extent += gap_cross * static_cast<float>(lines.empty() ? 0 : lines.size() - 1);

        float h;
        if (row) {
            h = own_height.value_or(cross_extent);
        } else {
            h = own_height.value_or(main_extent);
        }
        if (!s.min_height.hasPercent()) h = std::max(h, std::max(0.0f, s.min_height.px - (s.border_box ? c.frameHeight() : 0)));
        if (!s.max_height.is_auto && !s.max_height.hasPercent()) h = std::min(h, std::max(0.0f, s.max_height.px - (s.border_box ? c.frameHeight() : 0)));
        c.height = clampCoord(h);
    }

    // The width a column-direction item gets: its own, or as much as there is if it
    // stretches, or else what its content wants.
    float columnItemWidth(const Box& c, const FlexItem& it) {
        Box& k = *it.box;
        const ComputedStyle& ks = *k.style;
        const ComputedStyle& s = *c.style;
        if (!ks.width.is_auto) {
            float w = ks.width.resolve(c.width);
            if (ks.border_box) w -= k.frameWidth();
            return clampWidth(k, std::max(0.0f, w), c.width);
        }
        const FlexAlign align = ks.align_self == FlexAlign::Auto ? s.align_items : ks.align_self;
        const float room = std::max(0.0f, c.width - it.frame_cross);
        if (align == FlexAlign::Stretch && !ks.margin[Left].is_auto && !ks.margin[Right].is_auto) return clampWidth(k, room, c.width);
        return clampWidth(k, std::min(preferredWidth(k), room), c.width);
    }

    // ── Inline layout ─────────────────────────────────────────────────────────

    struct InlineItem {
        enum Kind { Text, Spacer, Atomic, Break, Positioned, Floated } kind = Text;
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
                case BoxKind::Block: {
                    if (!k.outOfFlow() && !k.floating()) break;
                    InlineItem it;
                    it.kind = k.floating() ? InlineItem::Floated : InlineItem::Positioned;
                    it.style = k.style;
                    it.atomic = &k;
                    items.push_back(std::move(it));
                    break;
                }
            }
        }
    }

    // Returns the height of the lines; fills b.fragments.
    float layoutInline(Box& b, Flow flow) {
        b.fragments.clear();
        b.last_baseline = -1;
        // An inline-block counts its own <a> for what it contains.
        InlineContext ctx;
        if (b.node && b.node->isHtml("a"))
            if (const std::string* href = b.node->attr("href")) ctx.link = href;
        if (b.kids.size() == 1 && b.kids[0]->kind == BoxKind::Replaced && b.kind == BoxKind::Block && b.kids[0]->node == b.node) {
            // A display:block replaced element is exactly its one box.
            Box& r = *b.kids[0];
            layoutReplaced(r, b.width);
            r.x = r.y = 0;
            for (int i = 0; i < 4; ++i) r.margin[i] = r.padding[i] = r.border[i] = 0;
            Box::Fragment f;
            f.w = r.width;
            f.h = r.height;
            f.atomic = &r;
            f.style = r.style;
            b.fragments.push_back(f);
            return r.height;
        }
        std::vector<InlineItem> items;
        collectInline(b, ctx, items, b.width);

        LineBuilder lines(*this, b, flow);
        for (InlineItem& it : items) lines.add(it);
        lines.finish();
        return lines.totalHeight();
    }

    class LineBuilder {
    public:
        LineBuilder(Layouter& l, Box& b, Flow flow) : l_(l), b_(b), flow_(flow) {
            first_line_indent_ = b.style->text_indent.resolve(b.width);
            beginLine();
            x_ += first_line_indent_;
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
                    endLine(true);
                    break;
                case InlineItem::Positioned:
                    // Its static position: where the next block would have started.
                    flushRun();
                    l_.layoutOutOfFlow(*it.atomic, b_.width);
                    it.atomic->static_y = y_ + (line_.empty() ? 0 : l_.lineHeightOf(*b_.style));
                    break;
                case InlineItem::Floated: addFloat(*it.atomic); break;
            }
        }

        void finish() {
            flushRun();
            pending_space_ = false;
            has_line_space_ = false;
            if (!line_.empty() || line_has_content_) endLine(false);
            for (Box* k : deferred_floats_) l_.placeFloat(*k, flow_, b_.width, y_);
            deferred_floats_.clear();
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

        // The free span for a line starting at y_, floats taken out.
        void beginLine() {
            float left = flow_.ox, right = flow_.ox + b_.width;
            if (flow_.bfc) flow_.bfc->freeSpan(flow_.oy + y_, l_.lineHeightOf(*b_.style), left, right);
            line_left_ = left - flow_.ox;
            line_right_ = right - flow_.ox;
            x_ = line_left_;
        }

        void addFloat(Box& k) {
            flushRun();
            l_.layoutFloat(k, b_.width);
            const float outer = k.outerWidth() + k.margin[Left] + k.margin[Right];
            const bool fits = x_ + (has_line_space_ ? line_space_.width : 0) + outer <= line_right_ + kFitSlack || (line_.empty() && !line_has_content_);
            if (!fits) {
                deferred_floats_.push_back(&k);   // goes below this line
                return;
            }
            l_.placeFloat(k, flow_, b_.width, y_);
            // The line it's on makes room.
            float left = flow_.ox, right = flow_.ox + b_.width;
            if (flow_.bfc) flow_.bfc->freeSpan(flow_.oy + y_, l_.lineHeightOf(*b_.style), left, right);
            const float new_left = left - flow_.ox;
            if (new_left > line_left_) {
                const float moved = new_left - line_left_;
                for (auto& f : line_) f.x += moved;
                x_ += moved;
                line_left_ = new_left;
            }
            line_right_ = right - flow_.ox;
        }

        // From the top of its margin box down to the baseline it sits on: that of its
        // last line, or with none, its bottom margin edge.
        float atomicBaseline(const Box& a) const {
            const float total = a.outerHeight() + a.margin[Top] + a.margin[Bottom];
            if (a.style->vertical_align == VerticalAlign::Middle)
                return total / 2 + b_.style->font_size * 0.25f;
            if (a.kind == BoxKind::Replaced && a.last_baseline >= 0)
                return a.margin[Top] + a.border[Top] + a.padding[Top] + a.last_baseline;
            if (a.kind != BoxKind::InlineBlock || a.style->overflow_clip) return total;
            const Box* inner = &a;
            float origin = a.margin[Top] + a.border[Top] + a.padding[Top];
            while (inner->last_baseline < 0) {
                // A flex container or a table goes by its first line, a block by its last.
                const Box* next = nullptr;
                const bool from_start = inner->isFlex() || inner->isTable() || inner->display() == Display::TableRowGroup ||
                                        inner->display() == Display::TableRow;
                for (const auto& k : inner->kids) {
                    if (!k->inFlowBlock() || k->collapses_through) continue;
                    next = k.get();
                    if (from_start) break;
                }
                if (!next) return total;
                origin += next->y + next->border[Top] + next->padding[Top];
                inner = next;
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
            // A box sized to fit exactly must not wrap on a rounding error.
            if (wrap_allowed && (!line_.empty() || line_has_content_) && x_ + space_w + run_w > line_right_ + kFitSlack) {
                has_line_space_ = false;
                endLine(false);
            }
            // Nothing on the line yet and floats leave too little: go below them.
            if (wrap_allowed && line_.empty() && flow_.bfc) {
                for (int guard = 0; guard < 1000 && x_ + run_w > line_right_ + kFitSlack && line_right_ - line_left_ < b_.width; ++guard) {
                    const float next = flow_.bfc->nextBottom(flow_.oy + y_) - flow_.oy;
                    if (next <= y_) break;
                    y_ = next;
                    beginLine();
                }
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
                    endLine(true);
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

        void endLine(bool forced) {
            const ComputedStyle& container = *b_.style;
            const FontSpec strut_font = fontOf(container);
            const float strut_a = l_.text_.ascent(strut_font);
            const float strut_d = l_.text_.descent(strut_font);
            const float strut_half = (l_.lineHeightOf(container) - (strut_a + strut_d)) / 2;
            float above = strut_a + strut_half, below = strut_d + strut_half;
            if (line_.empty() && !forced) above = below = 0;

            const auto textShift = [](const ComputedStyle& s) {
                // Measured from the parent's size; "smaller" is what usually got us here.
                if (s.vertical_align == VerticalAlign::Sub) return s.font_size * 1.2f / 5 + 1;
                if (s.vertical_align == VerticalAlign::Super) return -(s.font_size * 1.2f / 3 + 1);
                return 0.0f;
            };

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
                const float shift = textShift(*f.style);
                above = std::max(above, a + half - shift);
                below = std::max(below, d + half + shift);
            }

            // Drop whitespace hanging off the end of the line.
            float line_w = line_.empty() ? line_left_ : line_.back().x + line_.back().w;
            if (!line_.empty() && !line_.back().atomic && line_.back().style && isCollapsible(line_.back().style->white_space)) {
                auto& last = line_.back();
                if (!last.text.empty() && last.text.back() == ' ') {
                    last.text.pop_back();
                    last.w = l_.text_.width(last.text, fontOf(*last.style));
                    line_w = last.x + last.w;
                }
            }

            float shift = 0;
            if (b_.width < kUnbounded) {
                if (container.text_align == TextAlign::Center) shift = (line_right_ - line_w) / 2;
                else if (container.text_align == TextAlign::Right) shift = line_right_ - line_w;
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
                    f.box_top = baseline - l_.text_.ascent(font) + textShift(*f.style);
                    f.box_h = l_.text_.ascent(font) + l_.text_.descent(font);
                    f.y = f.box_top;
                    f.h = f.box_h;
                }
                b_.fragments.push_back(std::move(f));
            }
            line_.clear();
            y_ = clampCoord(y_ + above + below);
            line_has_content_ = false;
            after_space_ = true;
            for (Box* k : deferred_floats_) l_.placeFloat(*k, flow_, b_.width, y_);
            deferred_floats_.clear();
            beginLine();
        }

        Layouter& l_;
        Box& b_;
        Flow flow_;
        float first_line_indent_ = 0;
        float line_left_ = 0, line_right_ = 0;
        float x_ = 0, y_ = 0;
        std::vector<Box::Fragment> line_;
        std::vector<Chunk> run_;
        std::vector<Box*> deferred_floats_;
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

    void addRect(float x, float y, float w, float h, Color c, float radius = 0) {
        if (w <= 0 || h <= 0 || !c.visible()) return;
        PaintOp op;
        op.kind = PaintKind::Rect;
        op.x = x;
        op.y = y;
        op.w = w;
        op.h = h;
        op.color = c;
        op.radius = radius;
        out_->ops.push_back(std::move(op));
    }

    void paintFrame(const Box& b, float x, float y, float opacity) {
        const ComputedStyle& s = *b.style;
        const float w = b.outerWidth(), h = b.outerHeight();
        if (s.hidden) return;
        const float r = std::min({s.border_radius, w / 2, h / 2});
        const bool same_border = b.border[Top] > 0 && b.border[Top] == b.border[Right] && b.border[Top] == b.border[Bottom] &&
                                 b.border[Top] == b.border[Left] && s.border_color[Top] == s.border_color[Right] &&
                                 s.border_color[Top] == s.border_color[Bottom] && s.border_color[Top] == s.border_color[Left];
        const Color bg = &b == skip_background_ ? Color{0, 0, 0, 0} : faded(s.background, opacity);
        if (r > 0 && same_border && bg.visible()) {
            // A rounded border is the border colour's shape with the fill's inside it.
            const float bw = b.border[Top];
            addRect(x, y, w, h, faded(s.border_color[Top], opacity), r);
            addRect(x + bw, y + bw, w - 2 * bw, h - 2 * bw, bg, std::max(0.0f, r - bw));
            return;
        }
        addRect(x, y, w, h, bg, r);
        // A collapsed border is centred on the edge the two cells share.
        const float t = b.border[Top], rt = b.border[Right], bt = b.border[Bottom], l = b.border[Left];
        const float grow = b.collapsed_border ? 1 : 0;
        if (t > 0) addRect(x - grow * l, y - grow * t, w + grow * (l + rt), t * (1 + grow), faded(s.border_color[Top], opacity));
        if (bt > 0) addRect(x - grow * l, y + h - bt, w + grow * (l + rt), bt * (1 + grow), faded(s.border_color[Bottom], opacity));
        if (l > 0) addRect(x - grow * l, y - grow * t, l * (1 + grow), h + grow * (t + bt), faded(s.border_color[Left], opacity));
        if (rt > 0) addRect(x + w - rt, y - grow * t, rt * (1 + grow), h + grow * (t + bt), faded(s.border_color[Right], opacity));
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
                if (k->inFlowBlock()) {
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
        // A link that's a box: all of it can be clicked.
        if (b.node && b.node->isHtml("a"))
            if (const std::string* href = b.node->attr("href")) out_->links.push_back({x, y, b.outerWidth(), b.outerHeight(), *href});
        const float cx = x + b.border[Left] + b.padding[Left];
        const float cy = y + b.border[Top] + b.padding[Top];
        // Positioned descendants measure from this box's padding box, if it's positioned.
        const Rect cb = s.position != Position::Static
                            ? Rect{x + b.border[Left], y + b.border[Top], b.width + b.padding[Left] + b.padding[Right],
                                   b.height + b.padding[Top] + b.padding[Bottom]}
                            : cb_in;
        if (s.overflow_clip) {
            PaintOp op;
            op.kind = PaintKind::ClipBegin;
            op.x = x + b.border[Left];
            op.y = y + b.border[Top];
            op.w = b.width + b.padding[Left] + b.padding[Right];
            op.h = b.height + b.padding[Top] + b.padding[Bottom];
            out_->ops.push_back(std::move(op));
        }
        // Floats paint above their context's in-flow blocks: later siblings' backgrounds
        // must not cover them. So they wait for the context's end.
        const bool context_root = establishesBfc(b);
        std::vector<PendingFloat> outer_floats;
        if (context_root) outer_floats.swap(pending_floats_);

        paintMarker(b, cx, cy, opacity);
        paintFragments(b, cx, cy, cb, opacity);
        for (const auto& kp : b.kids) {
            const Box& k = *kp;
            if (k.outOfFlow()) {
                positioned_.push_back({&k, cx, cy + k.static_y, cb, opacity, order_++});
                continue;
            }
            if (k.floating()) {
                pending_floats_.push_back({&k, cx, cy, cb, opacity});
                continue;
            }
            if (k.kind == BoxKind::Inline) {
                collectOutOfFlowIn(k, cx, cy, cb, opacity);
                continue;
            }
            if (k.kind != BoxKind::Block) continue;
            paint(k, cx, cy, cb, opacity);
        }

        if (context_root) {
            while (!pending_floats_.empty()) {
                std::vector<PendingFloat> batch;
                batch.swap(pending_floats_);
                for (const PendingFloat& p : batch) paint(*p.box, p.cx, p.cy, p.cb, p.opacity);
            }
            pending_floats_.swap(outer_floats);
        }
        if (s.overflow_clip) {
            PaintOp op;
            op.kind = PaintKind::ClipEnd;
            out_->ops.push_back(std::move(op));
        }
    }

    // Absolute and floated boxes inside inline elements still need painting.
    void collectOutOfFlowIn(const Box& b, float cx, float cy, const Rect& cb, float opacity) {
        for (const auto& kp : b.kids) {
            if (kp->outOfFlow()) positioned_.push_back({kp.get(), cx, cy + kp->static_y, cb, opacity, order_++});
            else if (kp->floating()) pending_floats_.push_back({kp.get(), cx, cy, cb, opacity});
            else if (kp->kind == BoxKind::Inline) collectOutOfFlowIn(*kp, cx, cy, cb, opacity);
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

    struct PendingFloat {
        const Box* box;
        float cx, cy;
        Rect cb;
        float opacity;
    };

    const StyledDocument& styles_;
    TextMeasurer& text_;
    ImageSizes* images_;
    float vw_, vh_;
    std::deque<ComputedStyle> anon_styles_;
    PageLayout* out_ = nullptr;
    const Box* skip_background_ = nullptr;
    const Box* root_ = nullptr;
    float max_bottom_ = 0;
    std::vector<Positioned> positioned_;
    std::vector<PendingFloat> pending_floats_;
    size_t order_ = 0;
    int relayouts_ = 0;
    bool quirks_ = false;
};

}  // namespace

float chWidth(TextMeasurer& text, const ComputedStyle& style) { return text.width("0", fontOf(style)); }

PageLayout layoutPage(const Document& doc, const StyledDocument& styles, TextMeasurer& text, ImageSizes* images,
                      float viewport_width, float viewport_height) {
    return Layouter(styles, text, images, viewport_width, viewport_height).run(doc);
}

}  // namespace fbx::xweb
