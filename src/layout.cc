#include "xweb/layout.h"
#include "xweb/forms.h"

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
    std::pair<float, float> laid_out_for{-1, -1};   // absolutely positioned: the containing block it was sized for
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
    bool flex_item = false;        // or a grid item
    float measured_width = -1, measured_height = 0;   // a grid item's natural height at that width
    float laid_width = -1, laid_height = -2;          // what it was last laid out at; -1 height is auto
    bool anonymous_table_part = false;
    bool quirky_no_top_margin = false, quirky_no_bottom_margin = false;
    float static_y = 0;            // where an absolutely positioned box would have gone
    float static_x = 0;            // across, for one that was inline-level (else 0)
    float pref_width = -1;         // max-content width, once computed
    float min_content = -1;        // min-content width, once computed
    int list_number = 0;
    const Box* deco_parent = nullptr;   // an inline with a background or border: the next such out
    float inline_extent = 0;       // how far its widest line reaches, closing padding included
    bool synthetic = false;        // a pseudo-element's image: its node is the layout's own, not the page's
    float cb_height_hint = -1;     // its containing block's definite content height, if known: for a
                                   // picture's percentage height (an SVG at height: 100% of a 26px box)
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
        const Box* deco = nullptr;      // the innermost inline with a background or border it's in
        float baseline = 0;             // its line's
        uint32_t line = 0;              // which line of the block
    };
    std::vector<Fragment> fragments;

    // (Text and line breaks share their element's style: its float or position isn't theirs.)
    [[nodiscard]] bool outOfFlow() const noexcept {
        return style && kind != BoxKind::Text && kind != BoxKind::LineBreak &&
               (style->position == Position::Absolute || style->position == Position::Fixed);
    }
    [[nodiscard]] bool floating() const noexcept {
        return style && kind != BoxKind::Text && kind != BoxKind::LineBreak && style->float_side != Float::None;
    }
    [[nodiscard]] bool blockLevel() const noexcept { return kind == BoxKind::Block; }
    [[nodiscard]] bool inFlowBlock() const noexcept { return blockLevel() && !outOfFlow() && !floating(); }
    [[nodiscard]] Display display() const noexcept { return style ? style->display : Display::Block; }
    [[nodiscard]] bool isTable() const noexcept { return display() == Display::Table || display() == Display::InlineTable; }
    [[nodiscard]] bool isFlex() const noexcept { return display() == Display::Flex || display() == Display::InlineFlex; }
    [[nodiscard]] bool isGrid() const noexcept { return display() == Display::Grid || display() == Display::InlineGrid; }
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

bool isDisclosure(ListStyle style) { return style == ListStyle::DisclosureClosed || style == ListStyle::DisclosureOpen; }

std::string markerText(ListStyle style, int number) {
    switch (style) {
        case ListStyle::None: return {};
        case ListStyle::DisclosureClosed: return "\xE2\x96\xB8";   // ▸
        case ListStyle::DisclosureOpen: return "\xE2\x96\xBE";     // ▾
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
    Layouter(const StyledDocument& styles, TextMeasurer& text, ImageSizes* images, float vw, float vh, const FormState* forms)
        : styles_(styles), text_(text), images_(images), forms_(forms), vw_(vw), vh_(vh) {}

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
        const auto hasBackground = [](const ComputedStyle& st) { return st.background.visible() || (st.background_layers && !st.masked); };
        if (hasBackground(*root_style)) {
            page.canvas = root_style->background;
            skip_bg = root.get();
        } else if (body_style && hasBackground(*body_style)) {
            page.canvas = body_style->background;
            for (const auto& k : root->kids)
                if (k->node == body) skip_bg = k.get();
        }
        if (!page.canvas.visible()) page.canvas = {255, 255, 255, 255};
        else if (page.canvas.a != 255) {
            // Over the white a browser starts from.
            const float a = page.canvas.a / 255.0f;
            const auto over = [a](uint8_t c) { return static_cast<uint8_t>(std::lround(c * a + 255 * (1 - a))); };
            page.canvas = {over(page.canvas.r), over(page.canvas.g), over(page.canvas.b), 255};
        }
        skip_background_ = skip_bg;
        out_ = &page;
        Rect viewport{0, 0, vw_, vh_};
        root->x = root->margin[Left];
        const float root_y = root->margin_top_outer;
        record(*root, root->x, root_y);
        paintContents(*root, root->x, root_y, viewport, 1.0f);
        flushPositioned();
        placeBelowContent();
        page.height = std::max({vh_, max_bottom_, root_y + root->outerHeight() + root->margin_bottom_outer});
        // The canvas's images, under everything: placed for their element, drawn everywhere.
        if (skip_bg && skip_bg->style->background_layers) {
            std::vector<PaintOp> painted = std::move(page.ops);
            page.ops.clear();
            float bx = root->x, by = root_y;
            if (skip_bg != root.get()) {
                bx += root->border[Left] + root->padding[Left] + skip_bg->x;
                by += root->border[Top] + root->padding[Top] + skip_bg->y;
            }
            paintLayers(*skip_bg, bx, by, *skip_bg->style->background_layers, 1.0f, nullptr, Rect{0, 0, std::max(vw_, page.width), page.height}, true);
            page.ops.insert(page.ops.end(), std::make_move_iterator(painted.begin()), std::make_move_iterator(painted.end()));
        }
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
            const ControlKind k = controlKind(n);
            if (k == ControlKind::Submit) return "Submit";
            if (k == ControlKind::Reset) return "Reset";
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
            case Display::InlineFlex:
            case Display::InlineGrid: return BoxKind::InlineBlock;
            default: return BoxKind::Block;
        }
    }

    std::unique_ptr<Box> pseudoBox(const ComputedStyle& style) {
        auto box = std::make_unique<Box>();
        box->style = &style;
        box->kind = kindFor(style.display);
        // content: url(icon.svg): a picture, as an <img> of its own would be.
        if (!style.content_image.empty() && synthetic_nodes_.size() < 10000) {
            Node& img = synthetic_nodes_.emplace_back();
            img.type = NodeType::Element;
            img.ns = Namespace::Html;
            img.name = "img";
            img.attrs.push_back({"src", style.content_image});
            auto pic = std::make_unique<Box>();
            pic->kind = BoxKind::Replaced;
            pic->node = &img;
            pic->style = anonymousStyle(style, Display::Inline);
            pic->synthetic = true;
            box->kids.push_back(std::move(pic));
        }
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

    // An element's content as boxes in `box`, which is its own unless it has none.
    void addChildren(const Node* el, const ComputedStyle& style, Box& box, int depth, int& counter) {
        // A disclosure marker sits inside, on the line, as text would, before ::before.
        if (style.display == Display::ListItem && isDisclosure(style.list_style)) {
            auto t = std::make_unique<Box>();
            t->kind = BoxKind::Text;
            t->style = &style;
            t->text = markerText(style.list_style, 0) + " ";
            box.kids.push_back(std::move(t));
        }
        if (const ComputedStyle* before = styles_.beforeOf(el)) box.kids.push_back(pseudoBox(*before));
        // The composed tree: a host's shadow tree, a slot's assigned nodes.
        std::vector<const Node*> kids;
        composedChildren(el, kids);
        // A closed <details> shows only its summary.
        if (el->isHtml("details") && !el->attr("open")) {
            const auto summary = std::find_if(kids.begin(), kids.end(), [](const Node* c) { return c->isHtml("summary"); });
            if (summary == kids.end()) kids.clear();
            else kids = {*summary};
        }
        for (const Node* c : kids) {
            if (c->type == NodeType::Text) {
                if (c->data.empty()) continue;
                auto t = std::make_unique<Box>();
                t->kind = BoxKind::Text;
                t->node = c;
                t->style = &style;
                t->text = c->data;
                box.kids.push_back(std::move(t));
            } else if (c->isElement()) {
                const ComputedStyle* cs = styles_.styleOf(c);
                if (!cs) continue;
                if (cs->display == Display::Contents && !isReplaced(c) && !c->isHtml("br") && !c->isHtml("input")) {
                    if (depth < kMaxBoxDepth) addChildren(c, *cs, box, depth + 1, counter);
                    continue;
                }
                auto kid = build(c, *cs, depth + 1);
                if (!kid) continue;
                if (cs->display == Display::ListItem) {
                    if (const std::string* v = c->attr("value"))
                        counter = static_cast<int>(std::strtol(v->c_str(), nullptr, 10));
                    kid->list_number = counter++;
                }
                box.kids.push_back(std::move(kid));
            }
        }
        if (const ComputedStyle* after = styles_.afterOf(el)) box.kids.push_back(pseudoBox(*after));
    }

    std::unique_ptr<Box> build(const Node* el, const ComputedStyle& style, int depth) {
        if (style.display == Display::None || style.display == Display::TableColumn || depth > kMaxBoxDepth) return nullptr;
        // Contents that reaches here is on something that has to have a box, and there it's nothing.
        if (style.display == Display::Contents && (isReplaced(el) || el->isHtml("br") || el->isHtml("input"))) return nullptr;
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

        addChildren(el, style, *box, depth, counter);

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

        if (d == Display::Flex || d == Display::InlineFlex || d == Display::Grid || d == Display::InlineGrid) {
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
               d == Display::Flex || d == Display::InlineFlex || d == Display::Grid || d == Display::InlineGrid;
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
        } else if (b.isTable() || s.width_keyword != ComputedStyle::WidthKeyword::None) {
            content = shrinkToFit(b, std::max(0.0f, cb_width - frame - b.margin[Left] - b.margin[Right]));
        } else {
            content = std::max(0.0f, cb_width - frame - b.margin[Left] - b.margin[Right]);
        }
        content = clampWidth(b, content, cb_width);
        b.width = clampCoord(content);

        // Auto margins share what's left.
        const float free_space = cb_width - b.width - frame;
        const bool ml_auto = s.margin[Left].is_auto, mr_auto = s.margin[Right].is_auto;
        if (!width_auto || !s.max_width.is_auto || b.isTable() || s.width_keyword != ComputedStyle::WidthKeyword::None) {
            if (ml_auto && mr_auto) {
                b.margin[Left] = b.margin[Right] = std::max(0.0f, free_space / 2);
            } else if (ml_auto) {
                b.margin[Left] = free_space - b.margin[Right];
            } else if (mr_auto) {
                b.margin[Right] = free_space - b.margin[Left];
            } else if (s.parent_block_align != 0 && free_space > b.margin[Left] + b.margin[Right]) {
                // Inside <center> or align=center: the box itself goes to the middle (or right).
                const float spare = free_space - b.margin[Left] - b.margin[Right];
                b.margin[Left] += s.parent_block_align == 1 ? spare / 2 : spare;
                b.margin[Right] += s.parent_block_align == 1 ? spare / 2 : 0;
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
        // Before anything measures them: what a percentage height of theirs is of.
        for (auto& k : b.kids) k->cb_height_hint = own_height ? *own_height : -1;
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
        if (b.isGrid()) {
            layoutGrid(b, own_height);
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
            applyEllipsis(b);
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

    // Laid out first against its parent's width; once painting knows the real containing
    // block, again if what it measures by turns out different (layoutForContainingBlock).
    void layoutOutOfFlow(Box& k, float cb_width, std::optional<float> cb_height = std::nullopt) {
        const ComputedStyle& s = *k.style;
        const float base = s.position == Position::Fixed ? vw_ : cb_width;
        const float base_h = s.position == Position::Fixed ? vh_ : cb_height.value_or(vh_);
        resolveSpacing(k, base);
        const float frame = k.frameWidth();
        // A positioned picture keeps its own size between its offsets (CSS 2.1 10.3.8,
        // 10.6.5): the far offsets give way, not the picture.
        const bool replaced = k.kids.size() == 1 && k.kids[0]->kind == BoxKind::Replaced && k.kids[0]->node == k.node;
        float width;
        if (!widthIsAuto(s, base)) {
            width = s.width.resolve(base);
            if (s.border_box) width -= frame;
        } else if (!replaced && !s.offset[Left].is_auto && !s.offset[Right].is_auto) {
            width = base - s.offset[Left].resolve(base) - s.offset[Right].resolve(base) - frame - k.margin[Left] - k.margin[Right];
        } else if (replaced) {
            width = shrinkToFit(k, kUnbounded);   // its natural width, however little room
        } else {
            width = shrinkToFit(k, base - frame);
        }
        k.width = clampWidth(k, std::max(0.0f, width), base);
        std::optional<float> own;
        if (!s.height.is_auto && !s.height.hasPercent()) {
            float h = s.height.px;
            if (s.border_box) h -= k.frameHeight();
            own = std::max(0.0f, h);
        } else if (!s.height.is_auto) {
            float h = s.height.resolve(base_h);
            if (s.border_box) h -= k.frameHeight();
            own = std::max(0.0f, h);
        } else if (cb_height && !replaced && !s.offset[Top].is_auto && !s.offset[Bottom].is_auto) {
            // Both edges given: as tall as they leave.
            own = std::max(0.0f, base_h - s.offset[Top].resolve(base_h) - s.offset[Bottom].resolve(base_h) - k.frameHeight() - k.margin[Top] - k.margin[Bottom]);
        }
        k.laid_out_for = {base, cb_height.value_or(-1)};
        layoutContents(k, own, Flow{});
    }

    // Whether an absolutely positioned box's size depends on its containing block in a way
    // its first layout couldn't know, and if so, lays it out again.
    void layoutForContainingBlock(const Box& kc, const Rect& cb) {
        Box& k = const_cast<Box&>(kc);   // laid out, not yet painted: nothing has its old size
        const ComputedStyle& s = *k.style;
        if (s.position != Position::Absolute) return;
        const bool by_height = s.height.hasPercent() || (s.height.is_auto && !s.offset[Top].is_auto && !s.offset[Bottom].is_auto);
        const bool by_width = s.width.hasPercent() || (!s.offset[Left].is_auto && !s.offset[Right].is_auto) ||
                              s.offset[Left].hasPercent() || s.margin[Left].hasPercent() || s.padding[Left].hasPercent();
        const bool width_changed = std::abs(k.laid_out_for.first - cb.w) > 0.01f;
        const bool height_changed = std::abs(k.laid_out_for.second - cb.h) > 0.01f;
        if ((by_height && height_changed) || (by_width && width_changed)) layoutOutOfFlow(k, cb.w, cb.h);
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
            width = shrinkToFit(k, std::max(0.0f, cb_width - frame - k.margin[Left] - k.margin[Right]));
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

    // How wide something sized by its content is, with this much room.
    float shrinkToFit(Box& b, float room) {
        switch (b.style->width_keyword) {
            case ComputedStyle::WidthKeyword::MaxContent: return preferredWidth(b);
            case ComputedStyle::WidthKeyword::MinContent: return minContentWidth(b);
            default: return std::min(preferredWidth(b), room);
        }
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
            const bool summed = row && (max_content || !b.style->flex_wrap);
            float total = 0;
            int items = 0;
            for (auto& k : b.kids) {
                if (k->outOfFlow()) continue;
                const float w = outerIntrinsic(*k, max_content);
                ++items;
                if (summed) total += w;
                else total = std::max(total, w);
            }
            // Side by side, the gaps between them count too: without them a row sized to
            // its content came out short by them, and its items shrank to fit (a label
            // with text-overflow lost its last letter).
            if (summed && items > 1) total += b.style->column_gap * static_cast<float>(items - 1);
            return total;
        }
        if (b.isGrid()) return gridIntrinsicWidth(b, max_content);
        const bool inline_content = std::none_of(b.kids.begin(), b.kids.end(), [](const auto& k) { return k->inFlowBlock(); });
        float result = 0;
        if (inline_content) {
            const float saved = b.width;
            b.width = max_content ? kUnbounded : 0.0f;
            Bfc scratch;
            (void)layoutInline(b, Flow{&scratch, 0, 0});
            for (const auto& f : b.fragments)
                if (f.x + f.w < kUnbounded / 2) result = std::max(result, f.x + f.w);
            if (b.inline_extent < kUnbounded / 2) result = std::max(result, b.inline_extent);
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
            width = shrinkToFit(b, std::max(0.0f, cb_width - frame - b.margin[Left] - b.margin[Right]));
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

    // `fill`: the content width it must be, when a block wrapping it already sized itself
    // from the same style.
    void layoutReplaced(Box& b, float cb_width, const float* fill = nullptr) {
        const ComputedStyle& s = *b.style;
        resolveSpacing(b, cb_width);
        const Node* n = b.node;
        float natural_w = 300, natural_h = 150;
        float shape = 0;   // an SVG's width over height from its viewBox, whatever its size
        bool have_natural = true;
        if (n->isHtml("img")) {
            have_natural = false;
            const std::string* src = n->attr("src");
            if (src && images_) {
                if (const auto size = images_->imageSize(*src)) {
                    natural_w = size->first;
                    natural_h = size->second;
                    have_natural = true;
                } else if (const auto ratio = images_->imageRatio(*src); ratio && *ratio > 0 && cb_width < kUnbounded / 2) {
                    // A shape but no size: as wide as there's room for, as an inline <svg> is.
                    natural_w = std::max(0.0f, cb_width - b.frameWidth() - b.margin[Left] - b.margin[Right]);
                    natural_h = natural_w / *ratio;
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
        } else if (n->isHtml("video")) {
            // Before its frames, a video is the size of its poster.
            if (const std::string* poster = n->attr("poster"); poster && !poster->empty() && images_)
                if (const auto size = images_->imageSize(*poster)) {
                    natural_w = size->first;
                    natural_h = size->second;
                }
        } else if (n->ns == Namespace::Svg && n->name == "svg") {
            const auto size_attr = [n](const char* name) -> std::optional<float> {
                const std::string* v = n->attr(name);
                if (!v) return std::nullopt;
                char* end = nullptr;
                const float got = std::strtof(v->c_str(), &end);
                if (end == v->c_str() || got < 0 || got > kMaxCoord || (*end != '\0' && std::string_view(end) != "px")) return std::nullopt;
                return got;
            };
            const std::optional<float> attr_w = size_attr("width"), attr_h = size_attr("height");
            float ratio = 0;   // width over height, if there's a viewBox to say
            if (const std::string* box = n->attr("viewBox")) {
                std::string text = *box;
                std::replace(text.begin(), text.end(), ',', ' ');
                float vb[4] = {0, 0, 0, 0};
                const char* p = text.c_str();
                bool ok = true;
                for (float& value : vb) {
                    char* end = nullptr;
                    value = std::strtof(p, &end);
                    ok = ok && end != p;
                    p = end;
                }
                if (ok && vb[2] > 0 && vb[3] > 0) ratio = vb[2] / vb[3];
            }
            shape = ratio;
            if (attr_w && attr_h) {
                natural_w = *attr_w;
                natural_h = *attr_h;
            } else if (attr_w) {
                natural_w = *attr_w;
                natural_h = ratio > 0 ? *attr_w / ratio : 150;
            } else if (attr_h) {
                natural_h = *attr_h;
                natural_w = ratio > 0 ? *attr_h * ratio : 300;
            } else if (ratio > 0 && cb_width < kUnbounded / 2) {
                // No size of its own, only a shape: as wide as there's room for.
                natural_w = std::max(0.0f, cb_width - b.frameWidth() - b.margin[Left] - b.margin[Right]);
                natural_h = natural_w / ratio;
            }
        } else if (n->isHtml("input")) {
            const std::string* ty = n->attr("type");
            if (ty && (*ty == "checkbox" || *ty == "radio")) {
                natural_w = natural_h = 13;
                b.last_baseline = 13;   // its bottom edge sits on the baseline
            } else {
                // A browser's size="20" default: 20 average characters.
                float chars = 20;
                if (const std::string* size = n->attr("size")) {
                    const float v = std::strtof(size->c_str(), nullptr);
                    if (v > 0 && v < 10000) chars = v;
                }
                natural_w = chars * s.font_size * 0.7425f;   // Chromium's 20 characters of the default font here
                natural_h = lineHeightOf(s);
                const FontSpec font = fontOf(s);
                b.last_baseline = text_.ascent(font) + (natural_h - text_.ascent(font) - text_.descent(font)) / 2;
                const std::string shown = controlText(n);
                if (ty && (*ty == "submit" || *ty == "button" || *ty == "reset"))
                    natural_w = text_.width(shown.empty() ? "Submit" : shown, fontOf(s));
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
            float widest = 0;
            for (const Node* o : optionsOf(n)) widest = std::max(widest, text_.width(optionLabel(o), font));
            natural_w = widest + 20;   // the arrow's room
            natural_h = lineHeightOf(s) + 2;
            b.last_baseline = text_.ascent(font) + (natural_h - text_.ascent(font) - text_.descent(font)) / 2;
        } else if (n->isHtml("progress") || n->isHtml("meter")) {
            natural_w = 160;
            natural_h = 16;
        }
        const bool has_w = fill || !widthIsAuto(s, cb_width);
        const bool has_h = !s.height.is_auto && (!s.height.hasPercent() || b.cb_height_hint >= 0);
        float w = natural_w, h = natural_h;
        if (fill) {
            w = *fill;
        } else if (has_w) {
            w = s.width.resolve(cb_width);
            if (s.border_box) w -= b.frameWidth();
        }
        if (has_h) {
            h = s.height.resolve(std::max(0.0f, b.cb_height_hint));
            if (s.border_box) h -= b.frameHeight();
        }
        // Only pictures keep their shape: a text field made wider doesn't get taller.
        const bool has_ratio = !(n->isHtml("input") || n->isHtml("textarea") || n->isHtml("select") || n->isHtml("progress") || n->isHtml("meter"));
        const float ratio = !has_ratio ? 0 : shape > 0 && !(n->attr("width") && n->attr("height")) ? shape
                                            : (natural_w > 0 && natural_h > 0) ? natural_w / natural_h : 0;
        if (has_w && !has_h && ratio > 0) h = w / ratio;
        if (has_h && !has_w && ratio > 0) w = h * ratio;
        const float auto_w = w;
        w = fill ? std::max(0.0f, w) : clampWidth(b, std::max(0.0f, w), cb_width);
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
            // Columns a cell gives a fixed width: more room goes to the others first.
            std::vector<bool> fixed_col(grid.columns, false);
            for (const TableCell& c : grid.cells)
                if (c.colspan == 1 && !c.box->style->width.is_auto && !c.box->style->width.hasPercent()) fixed_col[c.col] = true;
            float auto_max = 0;
            size_t auto_cols = 0;
            for (size_t k = 0; k < grid.columns; ++k)
                if (percent[k] <= 0 && !fixed_col[k]) {
                    auto_max += maxes[k];
                    ++auto_cols;
                }
            for (size_t k = 0; k < grid.columns; ++k) {
                if (percent[k] > 0) continue;
                if (rest >= sum_max && auto_cols > 0) {
                    // More room than wanted: a table given a width spreads it over its
                    // auto columns.
                    if (fixed_col[k]) {
                        widths[k] = maxes[k];
                    } else {
                        const float share = auto_max > 0 ? maxes[k] / auto_max : 1.0f / static_cast<float>(auto_cols);
                        widths[k] = maxes[k] + share * (rest - sum_max);
                    }
                } else if (rest >= sum_max) {
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
            // Automatic minimum: an item doesn't shrink below its content, unless it says how far.
            if (!min_size.is_auto) {
                it.min_main = std::max(0.0f, min_size.resolve(base_for_percent) - box_extra);
            } else if (row && !ks.overflow_clip) {
                it.min_main = minContentWidth(k);
                if (!main_size.is_auto && !main_size.hasPercent()) it.min_main = std::min(it.min_main, std::max(0.0f, main_size.px - box_extra));
            } else if (!row && !ks.overflow_clip) {
                // Its content height, whatever its basis (flex: 1 on a page's <main> mustn't
                // squash it to nothing); no more than a height it gives itself.
                float content = it.base;   // measured above when the basis came from content
                if (!ks.flex_basis.is_auto || !main_size.is_auto) {
                    layoutItem(k, columnItemWidth(c, it), std::nullopt);
                    content = k.height;
                }
                it.min_main = content;
                if (!main_size.is_auto && !main_size.hasPercent()) it.min_main = std::min(it.min_main, std::max(0.0f, main_size.px - box_extra));
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
        // A single line with no definite height of its own is still held to the
        // container's min-height and max-height (a sidebar capped at 100vh).
        if (row && !cross_avail && lines.size() == 1 && !s.flex_wrap) {
            const float frame = s.border_box ? c.frameHeight() : 0;
            if (!s.max_height.is_auto && !s.max_height.hasPercent()) line_cross[0] = std::min(line_cross[0], std::max(0.0f, s.max_height.px - frame));
            if (!s.min_height.is_auto && !s.min_height.hasPercent()) line_cross[0] = std::max(line_cross[0], std::max(0.0f, s.min_height.px - frame));
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
                    float inner_cross = std::max(0.0f, line_cross[li] - it->frame_cross);
                    // Stretched, but within its own max and min (min winning).
                    const Length& max_cross = row ? ks.max_height : ks.max_width;
                    const Length& min_cross = row ? ks.min_height : ks.min_width;
                    const float own_frame = ks.border_box ? (row ? k.frameHeight() : k.frameWidth()) : 0;
                    if (!max_cross.is_auto && !max_cross.hasPercent()) inner_cross = std::min(inner_cross, std::max(0.0f, max_cross.px - own_frame));
                    if (!min_cross.is_auto && !min_cross.hasPercent()) inner_cross = std::max(inner_cross, std::max(0.0f, min_cross.px - own_frame));
                    if (row) {
                        if (std::abs(k.height - inner_cross) > 0.01f) layoutItem(k, it->main, inner_cross);
                    } else if (std::abs(k.width - inner_cross) > 0.01f) {
                        layoutItem(k, inner_cross, it->main);
                    }
                    // A picture item stretches too, without keeping its shape.
                    if (row && k.kids.size() == 1 && k.kids[0]->kind == BoxKind::Replaced && k.kids[0]->node == k.node &&
                        !k.fragments.empty() && k.fragments[0].atomic == k.kids[0].get()) {
                        k.kids[0]->height = inner_cross;
                        k.fragments[0].h = inner_cross;
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
        // max first, so min wins.
        if (!s.max_height.is_auto && !s.max_height.hasPercent()) h = std::min(h, std::max(0.0f, s.max_height.px - (s.border_box ? c.frameHeight() : 0)));
        if (!s.min_height.hasPercent()) h = std::max(h, std::max(0.0f, s.min_height.px - (s.border_box ? c.frameHeight() : 0)));
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
        if (align == FlexAlign::Stretch && !ks.margin[Left].is_auto && !ks.margin[Right].is_auto && ks.width_keyword == ComputedStyle::WidthKeyword::None) return clampWidth(k, room, c.width);
        return clampWidth(k, shrinkToFit(k, room), c.width);
    }

    // ── Grid ──────────────────────────────────────────────────────────────────

    static constexpr int kMaxGridLine = 10000;         // lines past this are this one
    static constexpr int kMaxGridSpan = 1000;
    static constexpr int kMaxAutoRepeat = 1000;
    static constexpr uint64_t kPlacementBudget = 50000000;   // cells looked at per grid
    static constexpr float kNoLimit = std::numeric_limits<float>::infinity();

    struct GridAxis {
        std::vector<GridTrack> tracks;   // explicit
        std::vector<std::vector<std::string>> names;   // of each line
        std::vector<bool> repeated;      // this explicit track came from auto-fit
        const std::vector<GridTrack>* implicit = nullptr;
        size_t area_lines = 0;
    };

    struct GridPlacedItem {
        Box* box = nullptr;
        int col0 = 0, col1 = 0, row0 = 0, row1 = 0;
        bool col_set = false, row_set = false;
        float row_contribution = 0;
    };

    struct SizedTrack {
        GridTrack def;
        float base = 0, growth = 0;
        bool collapsed = false;
        float offset = 0;
    };

    struct GridPlan {
        std::vector<SizedTrack> cols, rows;
        std::vector<GridPlacedItem> items;
    };

    enum class TrackRoom { Definite, MaxContent, MinContent };

    // How wide a track would be for working out how many repeats fit.
    static float repeatSize(const GridTrack& t, float room) {
        const float at_max = t.max.kind == GridSize::Fixed ? t.max.length.resolve(room) : 0;
        const float at_min = t.min.kind == GridSize::Fixed ? t.min.length.resolve(room) : 0;
        if (t.max.kind == GridSize::Fixed && t.min.kind == GridSize::Fixed) return std::max(at_min, at_max);
        return t.max.kind == GridSize::Fixed ? at_max : at_min;
    }

    static GridAxis expandAxis(const GridTrackList* list, const std::vector<GridTrack>* implicit, std::optional<float> room, float gap, int area_tracks) {
        GridAxis axis;
        axis.implicit = implicit;
        if (list) {
            int repeats = 0;
            if (!list->repeat.empty()) {
                repeats = 1;
                if (room) {
                    float fixed = 0, unit = 0;
                    for (const GridTrack& t : list->tracks) fixed += repeatSize(t, *room) + gap;
                    for (const GridTrack& t : list->repeat) unit += repeatSize(t, *room) + gap;
                    if (unit > 0) repeats = std::clamp(static_cast<int>(std::floor((*room + gap - fixed) / unit)), 1, kMaxAutoRepeat);
                }
            }
            for (size_t i = 0; i <= list->tracks.size(); ++i) {
                std::vector<std::string> line = i < list->names.size() ? list->names[i] : std::vector<std::string>{};
                if (!list->repeat.empty() && i == list->repeat_at) {
                    for (int r = 0; r < repeats; ++r) {
                        for (size_t t = 0; t < list->repeat.size(); ++t) {
                            line.insert(line.end(), list->repeat_names[t].begin(), list->repeat_names[t].end());
                            axis.names.push_back(std::move(line));
                            line.clear();
                            axis.tracks.push_back(list->repeat[t]);
                            axis.repeated.push_back(list->auto_fit);
                        }
                        line.insert(line.end(), list->repeat_names.back().begin(), list->repeat_names.back().end());
                    }
                    line.insert(line.end(), list->names_after_repeat.begin(), list->names_after_repeat.end());
                }
                if (i < list->tracks.size()) {
                    axis.names.push_back(std::move(line));
                    axis.tracks.push_back(list->tracks[i]);
                    axis.repeated.push_back(false);
                } else {
                    axis.names.push_back(std::move(line));
                }
            }
        }
        if (axis.names.empty()) axis.names.emplace_back();
        // Named areas can make the explicit grid bigger than the template did.
        while (static_cast<int>(axis.tracks.size()) < area_tracks) {
            axis.tracks.push_back(implicit && !implicit->empty() ? (*implicit)[(axis.tracks.size() - (list ? list->tracks.size() : 0)) % implicit->size()] : GridTrack{});
            axis.repeated.push_back(false);
            axis.names.emplace_back();
        }
        return axis;
    }

    // Where a line is, counted from the explicit grid's first line as 0.
    static std::optional<int> resolveLine(const GridLine& l, const GridAxis& axis, const GridAreas* areas, bool rows, bool start) {
        const int count = static_cast<int>(axis.names.size());
        if (l.span) return std::nullopt;
        if (l.name.empty()) {
            if (l.line == 0) return std::nullopt;
            const int at = l.line > 0 ? l.line - 1 : count + l.line;
            return std::clamp(at, -kMaxGridLine, kMaxGridLine);
        }
        if (l.line == 0 && areas) {
            const auto it = areas->areas.find(l.name);
            if (it != areas->areas.end()) {
                const GridAreaRect& r = it->second;
                return rows ? (start ? r.row_start : r.row_end) : (start ? r.column_start : r.column_end);
            }
        }
        if (areas) {
            // A name-start or name-end line exists for each named area.
            const bool is_start = l.name.ends_with("-start"), is_end = l.name.ends_with("-end");
            if (is_start || is_end) {
                const auto it = areas->areas.find(l.name.substr(0, l.name.size() - (is_start ? 6 : 4)));
                if (it != areas->areas.end()) {
                    const GridAreaRect& r = it->second;
                    return rows ? (is_start ? r.row_start : r.row_end) : (is_start ? r.column_start : r.column_end);
                }
            }
        }
        // The nth line by that name, in either direction.
        const auto named = [&](const std::string& name) -> std::optional<int> {
            const int want = l.line == 0 ? 1 : std::abs(l.line);
            int seen = 0;
            for (int n = 0; n < count; ++n) {
                const int at = l.line < 0 ? count - 1 - n : n;
                if (std::find(axis.names[at].begin(), axis.names[at].end(), name) != axis.names[at].end() && ++seen == want) return at;
            }
            return std::nullopt;
        };
        if (auto found = named(l.name)) return found;
        return named(l.name + (start ? "-start" : "-end"));
    }

    // Both ends of an item on one axis: its lines, or just how many tracks it spans.
    static void placementOf(const GridLine& a, const GridLine& b, const GridAxis& axis, const GridAreas* areas, bool rows, int& from, int& to, bool& set) {
        const std::optional<int> start = resolveLine(a, axis, areas, rows, true);
        const std::optional<int> end = resolveLine(b, axis, areas, rows, false);
        const int span_a = std::min(a.span, kMaxGridSpan), span_b = std::min(b.span, kMaxGridSpan);
        set = true;
        if (start && end) {
            from = std::min(*start, *end);
            to = std::max(*start, *end);
            if (from == to) to = from + 1;
        } else if (start) {
            from = *start;
            to = from + std::max(1, span_b);
        } else if (end) {
            to = *end;
            from = to - std::max(1, span_a);
        } else {
            set = false;
            from = 0;
            to = std::max(1, span_a ? span_a : span_b);
        }
    }

    // The placement algorithm (8.5). `major` is the axis auto-placement moves along last:
    // rows, unless grid-auto-flow says column.
    static void autoPlace(std::vector<GridPlacedItem>& items, bool flow_column, bool dense, int minor_count) {
        const auto major0 = [&](GridPlacedItem& it) -> int& { return flow_column ? it.col0 : it.row0; };
        const auto major1 = [&](GridPlacedItem& it) -> int& { return flow_column ? it.col1 : it.row1; };
        const auto minor0 = [&](GridPlacedItem& it) -> int& { return flow_column ? it.row0 : it.col0; };
        const auto minor1 = [&](GridPlacedItem& it) -> int& { return flow_column ? it.row1 : it.col1; };
        const auto major_set = [&](const GridPlacedItem& it) { return flow_column ? it.col_set : it.row_set; };
        const auto minor_set = [&](const GridPlacedItem& it) { return flow_column ? it.row_set : it.col_set; };

        std::vector<std::vector<uint8_t>> taken;
        uint64_t budget = kPlacementBudget;
        const auto is_free = [&](int ma0, int ma1, int mi0, int mi1) {
            for (int ma = ma0; ma < ma1 && ma < static_cast<int>(taken.size()); ++ma) {
                const auto& row = taken[static_cast<size_t>(ma)];
                for (int mi = mi0; mi < mi1 && mi < static_cast<int>(row.size()); ++mi) {
                    if (budget == 0) return true;   // past the limit things may overlap
                    --budget;
                    if (row[static_cast<size_t>(mi)]) return false;
                }
            }
            return true;
        };
        const auto occupy = [&](int ma0, int ma1, int mi0, int mi1) {
            const uint64_t cells = static_cast<uint64_t>(ma1 - ma0) * static_cast<uint64_t>(mi1 - mi0);
            if (cells > budget) { budget = 0; return; }
            budget -= cells;
            if (static_cast<int>(taken.size()) < ma1) taken.resize(static_cast<size_t>(ma1));
            for (int ma = ma0; ma < ma1; ++ma) {
                auto& row = taken[static_cast<size_t>(ma)];
                if (static_cast<int>(row.size()) < mi1) row.resize(static_cast<size_t>(mi1), 0);
                for (int mi = mi0; mi < mi1; ++mi) row[static_cast<size_t>(mi)] = 1;
            }
        };

        for (GridPlacedItem& it : items)
            if (it.col_set && it.row_set) occupy(major0(it), major1(it), minor0(it), minor1(it));

        // Fixed on the major axis: they go first, in their own row.
        std::unordered_map<int, int> row_cursor;
        for (GridPlacedItem& it : items) {
            if (!major_set(it) || minor_set(it)) continue;
            const int span = minor1(it) - minor0(it);
            int mi = dense ? 0 : row_cursor[major0(it)];
            while (!is_free(major0(it), major1(it), mi, mi + span) && mi < kMaxGridLine) ++mi;
            minor0(it) = mi;
            minor1(it) = mi + span;
            occupy(major0(it), major1(it), mi, mi + span);
            if (!dense) row_cursor[major0(it)] = mi + span;
        }

        // The rest, with a cursor.
        int cursor_ma = 0, cursor_mi = 0;
        for (GridPlacedItem& it : items) {
            if (major_set(it)) continue;
            const int ma_span = major1(it) - major0(it);
            if (minor_set(it)) {
                if (dense) {
                    cursor_ma = 0;
                } else if (minor0(it) < cursor_mi) {
                    ++cursor_ma;
                }
                cursor_mi = minor0(it);
                while (!is_free(cursor_ma, cursor_ma + ma_span, minor0(it), minor1(it)) && cursor_ma < kMaxGridLine) ++cursor_ma;
            } else {
                const int mi_span = minor1(it) - minor0(it);
                if (dense) { cursor_ma = 0; cursor_mi = 0; }
                while (cursor_ma < kMaxGridLine) {
                    if (cursor_mi + mi_span > std::max(minor_count, mi_span)) {
                        cursor_mi = 0;
                        ++cursor_ma;
                        continue;
                    }
                    if (is_free(cursor_ma, cursor_ma + ma_span, cursor_mi, cursor_mi + mi_span)) break;
                    ++cursor_mi;
                }
                minor0(it) = cursor_mi;
                minor1(it) = cursor_mi + mi_span;
            }
            major0(it) = cursor_ma;
            major1(it) = cursor_ma + ma_span;
            occupy(major0(it), major1(it), minor0(it), minor1(it));
            if (minor_set(it)) continue;
            cursor_mi = minor1(it);
        }
    }

    static std::vector<SizedTrack> gridTracks(const GridAxis& axis, int first, int last) {
        // `first` is negative if lines before the explicit grid are in use.
        std::vector<SizedTrack> out;
        out.reserve(static_cast<size_t>(last - first));
        const int count = static_cast<int>(axis.tracks.size());
        const auto implicit_at = [&](int n) {
            if (!axis.implicit || axis.implicit->empty()) return GridTrack{};
            const int size = static_cast<int>(axis.implicit->size());
            return (*axis.implicit)[static_cast<size_t>(((n % size) + size) % size)];
        };
        for (int i = first; i < last; ++i) {
            SizedTrack t;
            if (i >= 0 && i < count) {
                t.def = axis.tracks[static_cast<size_t>(i)];
            } else {
                t.def = implicit_at(i < 0 ? i : i - count);
            }
            out.push_back(std::move(t));
        }
        return out;
    }

    GridPlan gridPlan(Box& c, std::optional<float> width, std::optional<float> height) {
        const ComputedStyle& s = *c.style;
        const GridAreas* areas = s.grid_areas.get();
        const GridAxis cols = expandAxis(s.grid_columns.get(), s.grid_auto_columns.get(), width, s.column_gap, areas ? areas->columns : 0);
        const GridAxis rows = expandAxis(s.grid_rows.get(), s.grid_auto_rows.get(), height, s.row_gap, areas ? areas->rows : 0);

        GridPlan plan;
        for (auto& kp : c.kids) {
            if (kp->outOfFlow()) continue;
            GridPlacedItem it;
            it.box = kp.get();
            plan.items.push_back(it);
        }
        std::stable_sort(plan.items.begin(), plan.items.end(), [](const GridPlacedItem& a, const GridPlacedItem& b) { return a.box->style->order < b.box->style->order; });

        int col_min = 0, row_min = 0;
        for (GridPlacedItem& it : plan.items) {
            const ComputedStyle& ks = *it.box->style;
            placementOf(ks.grid_column_start, ks.grid_column_end, cols, areas, false, it.col0, it.col1, it.col_set);
            placementOf(ks.grid_row_start, ks.grid_row_end, rows, areas, true, it.row0, it.row1, it.row_set);
            if (it.col_set) col_min = std::min(col_min, it.col0);
            if (it.row_set) row_min = std::min(row_min, it.row0);
        }
        // Lines before the explicit grid: move everything so that it starts at 0.
        int col_tracks = static_cast<int>(cols.tracks.size()) - col_min, row_tracks = static_cast<int>(rows.tracks.size()) - row_min;
        for (GridPlacedItem& it : plan.items) {
            if (it.col_set) { it.col0 -= col_min; it.col1 -= col_min; }
            if (it.row_set) { it.row0 -= row_min; it.row1 -= row_min; }
            const int col_extent = it.col_set ? it.col1 : it.col1 - it.col0;
            const int row_extent = it.row_set ? it.row1 : it.row1 - it.row0;
            if (s.grid_flow_column) row_tracks = std::max(row_tracks, row_extent);
            else col_tracks = std::max(col_tracks, col_extent);
        }
        autoPlace(plan.items, s.grid_flow_column, s.grid_dense, s.grid_flow_column ? row_tracks : col_tracks);
        int col_last = col_tracks, row_last = row_tracks;
        for (const GridPlacedItem& it : plan.items) {
            col_last = std::max(col_last, it.col1);
            row_last = std::max(row_last, it.row1);
        }
        col_last = std::min(col_last, kMaxGridLine);
        row_last = std::min(row_last, kMaxGridLine);
        for (GridPlacedItem& it : plan.items) {
            it.col1 = std::min(it.col1, col_last);
            it.col0 = std::min(it.col0, it.col1 - 1);
            it.row1 = std::min(it.row1, row_last);
            it.row0 = std::min(it.row0, it.row1 - 1);
        }
        plan.cols = gridTracks(cols, col_min, col_min + col_last);
        plan.rows = gridTracks(rows, row_min, row_min + row_last);

        // auto-fit: repeats with nothing in them aren't there.
        const auto collapse_empty = [&](std::vector<SizedTrack>& tracks, const GridAxis& axis, int shift, auto span_of) {
            for (size_t r = 0; r < axis.repeated.size(); ++r) {
                if (!axis.repeated[r]) continue;
                const int at = static_cast<int>(r) - shift;
                const bool used = std::any_of(plan.items.begin(), plan.items.end(), [&](const GridPlacedItem& it) {
                    const auto [a, b] = span_of(it);
                    return a <= at && at < b;
                });
                if (!used && at >= 0 && at < static_cast<int>(tracks.size())) tracks[static_cast<size_t>(at)].collapsed = true;
            }
        };
        collapse_empty(plan.cols, cols, col_min, [](const GridPlacedItem& it) { return std::pair(it.col0, it.col1); });
        collapse_empty(plan.rows, rows, row_min, [](const GridPlacedItem& it) { return std::pair(it.row0, it.row1); });
        return plan;
    }

    static float sumGaps(const std::vector<SizedTrack>& tracks, float gap, int from, int to) {
        int n = 0;
        for (int i = from; i < to; ++i) n += tracks[static_cast<size_t>(i)].collapsed ? 0 : 1;
        return n > 1 ? gap * static_cast<float>(n - 1) : 0;
    }

    // What an item asks of the tracks it spans, in the axis being sized.
    struct TrackContribution {
        int from = 0, to = 0;
        float automatic = 0;   // its minimum: where a track's own minimum is `auto`
        float min = 0, max = 0;
    };

    // The track sizing algorithm (11), one axis at a time.
    static void sizeTracks(std::vector<SizedTrack>& t, std::optional<float> avail, float gap, TrackRoom room, std::vector<TrackContribution> items, bool stretch_auto) {
        for (SizedTrack& k : t) {
            if (k.collapsed) continue;
            GridTrack& d = k.def;
            // Percentages of something not yet known count as auto.
            if (!avail) {
                if (d.min.kind == GridSize::Fixed && d.min.length.hasPercent()) d.min = GridSize{};
                if (d.max.kind == GridSize::Fixed && d.max.length.hasPercent()) d.max = GridSize{};
                if (d.fit_content && d.fit_limit.hasPercent()) d.fit_content = false;
            }
            const float base_of = avail.value_or(0);
            k.base = d.min.kind == GridSize::Fixed ? d.min.length.resolve(base_of) : 0;
            k.growth = d.max.kind == GridSize::Fixed ? std::max(k.base, d.max.length.resolve(base_of)) : kNoLimit;
        }

        // Smallest spans first, as the bigger ones then only ask for what's still missing.
        std::stable_sort(items.begin(), items.end(), [](const TrackContribution& a, const TrackContribution& b) { return a.to - a.from < b.to - b.from; });
        for (const TrackContribution& it : items) {
            bool any_fr = false;
            float fixed_max = 0;
            bool all_fixed_max = true;
            for (int i = it.from; i < it.to; ++i) {
                const SizedTrack& k = t[static_cast<size_t>(i)];
                if (k.collapsed) continue;
                any_fr = any_fr || k.def.max.kind == GridSize::Fr;
                if (k.def.max.kind == GridSize::Fixed) fixed_max += k.growth;
                else all_fixed_max = false;
            }
            const float gaps = sumGaps(t, gap, it.from, it.to);
            float automatic = it.automatic;
            if (all_fixed_max) automatic = std::min(automatic, fixed_max + gaps);

            // Base sizes.
            float have = gaps;
            float want = 0;
            std::vector<SizedTrack*> targets;
            for (int i = it.from; i < it.to; ++i) {
                SizedTrack& k = t[static_cast<size_t>(i)];
                if (k.collapsed) continue;
                have += k.base;
                if (k.def.min.kind == GridSize::Fixed) continue;
                if (any_fr && k.def.max.kind != GridSize::Fr) continue;
                targets.push_back(&k);
                const float c = k.def.min.kind == GridSize::Auto ? automatic : k.def.min.kind == GridSize::MinContent ? it.min : it.max;
                want = std::max(want, c);
            }
            if (!targets.empty() && want > have) {
                float extra = want - have;
                // Up to their growth limits first.
                for (int round = 0; round < 8 && extra > 0.001f; ++round) {
                    std::vector<SizedTrack*> open;
                    for (SizedTrack* k : targets)
                        if (k->base < k->growth) open.push_back(k);
                    if (open.empty()) break;
                    const float share = extra / static_cast<float>(open.size());
                    for (SizedTrack* k : open) {
                        const float take = std::min(share, k->growth - k->base);
                        k->base += take;
                        extra -= take;
                    }
                }
                if (extra > 0.001f) {
                    const float share = extra / static_cast<float>(targets.size());
                    for (SizedTrack* k : targets) k->base += share;
                }
            }

            // Growth limits.
            if (any_fr) continue;
            float grown = gaps;
            std::vector<SizedTrack*> growing;
            float want_growth = 0;
            for (int i = it.from; i < it.to; ++i) {
                SizedTrack& k = t[static_cast<size_t>(i)];
                if (k.collapsed) continue;
                grown += std::isinf(k.growth) ? k.base : k.growth;
                if (k.def.max.kind == GridSize::Fixed) continue;
                growing.push_back(&k);
                float c = k.def.max.kind == GridSize::MinContent ? it.min : it.max;
                if (k.def.fit_content) c = std::min(c, k.def.fit_limit.resolve(avail.value_or(0)));
                want_growth = std::max(want_growth, c);
            }
            if (growing.empty()) continue;
            const float extra = std::max(0.0f, want_growth - grown);
            const float share = extra / static_cast<float>(growing.size());
            for (SizedTrack* k : growing) k->growth = (std::isinf(k->growth) ? k->base : k->growth) + share;
        }
        for (SizedTrack& k : t) {
            if (k.collapsed) continue;
            if (std::isinf(k.growth)) k.growth = k.base;
            k.growth = std::max(k.growth, k.base);
        }

        const auto track_sum = [&] {
            float total = sumGaps(t, gap, 0, static_cast<int>(t.size()));
            for (const SizedTrack& k : t)
                if (!k.collapsed) total += k.base;
            return total;
        };

        // Maximize: the free space up to each growth limit.
        if (room == TrackRoom::MaxContent) {
            for (SizedTrack& k : t)
                if (!k.collapsed) k.base = k.growth;
        } else if (avail) {
            float free_space = *avail - track_sum();
            for (int round = 0; round < 32 && free_space > 0.001f; ++round) {
                int open = 0;
                for (const SizedTrack& k : t)
                    if (!k.collapsed && k.base + 0.0001f < k.growth) ++open;
                if (open == 0) break;
                const float share = free_space / static_cast<float>(open);
                for (SizedTrack& k : t) {
                    if (k.collapsed || k.base + 0.0001f >= k.growth) continue;
                    const float take = std::min(share, k.growth - k.base);
                    k.base += take;
                    free_space -= take;
                }
            }
        }

        // Flexible tracks.
        bool has_fr = false;
        for (const SizedTrack& k : t) has_fr = has_fr || (!k.collapsed && k.def.max.kind == GridSize::Fr);
        if (has_fr && room != TrackRoom::MinContent) {
            float fr_size = 0;
            if (avail) {
                // Tracks whose base is already more than their share stop being flexible.
                std::vector<bool> inflexible(t.size(), false);
                for (size_t round = 0; round <= t.size(); ++round) {
                    float left = *avail - sumGaps(t, gap, 0, static_cast<int>(t.size()));
                    float factors = 0;
                    for (size_t i = 0; i < t.size(); ++i) {
                        if (t[i].collapsed) continue;
                        if (t[i].def.max.kind == GridSize::Fr && !inflexible[i]) factors += t[i].def.max.fr;
                        else left -= t[i].base;
                    }
                    fr_size = std::max(0.0f, left) / std::max(factors, 1.0f);
                    bool changed = false;
                    for (size_t i = 0; i < t.size(); ++i) {
                        if (t[i].collapsed || t[i].def.max.kind != GridSize::Fr || inflexible[i]) continue;
                        if (t[i].base > fr_size * t[i].def.max.fr) {
                            inflexible[i] = true;
                            changed = true;
                        }
                    }
                    if (!changed) break;
                }
            } else {
                for (const SizedTrack& k : t) {
                    if (k.collapsed || k.def.max.kind != GridSize::Fr) continue;
                    fr_size = std::max(fr_size, k.base / std::max(k.def.max.fr, 1.0f));
                }
                for (const TrackContribution& it : items) {
                    float factors = 0, others = sumGaps(t, gap, it.from, it.to);
                    for (int i = it.from; i < it.to; ++i) {
                        const SizedTrack& k = t[static_cast<size_t>(i)];
                        if (k.collapsed) continue;
                        if (k.def.max.kind == GridSize::Fr) factors += k.def.max.fr;
                        else others += k.base;
                    }
                    if (factors > 0) fr_size = std::max(fr_size, (it.max - others) / std::max(factors, 1.0f));
                }
            }
            for (SizedTrack& k : t) {
                if (k.collapsed || k.def.max.kind != GridSize::Fr) continue;
                k.base = std::max(k.base, fr_size * k.def.max.fr);
            }
        }

        // What's left goes to the auto tracks, unless the page aligns them some other way.
        if (avail && stretch_auto) {
            const float free_space = *avail - track_sum();
            int autos = 0;
            for (const SizedTrack& k : t) autos += !k.collapsed && k.def.max.kind == GridSize::Auto && !k.def.fit_content ? 1 : 0;
            if (free_space > 0 && autos > 0) {
                const float share = free_space / static_cast<float>(autos);
                for (SizedTrack& k : t)
                    if (!k.collapsed && k.def.max.kind == GridSize::Auto && !k.def.fit_content) k.base += share;
            }
        }
        for (SizedTrack& k : t) k.base = clampCoord(k.base);
    }

    std::vector<TrackContribution> columnContributions(GridPlan& plan) {
        std::vector<TrackContribution> out;
        out.reserve(plan.items.size());
        for (const GridPlacedItem& it : plan.items) {
            Box& k = *it.box;
            const ComputedStyle& ks = *k.style;
            TrackContribution c;
            c.from = it.col0;
            c.to = it.col1;
            // Asked for both whether or not they matter: finding them out disturbs the box,
            // so it has to happen before its real layout. See gridItemLayout().
            const float min = outerIntrinsic(k, false);
            const float max = outerIntrinsic(k, true);
            c.min = min;
            c.max = std::max(min, max);
            const float explicit_min = ks.min_width.hasPercent() ? 0 : ks.min_width.px;
            if (explicit_min > 0) {
                const float outer = explicit_min + (ks.border_box ? 0 : k.frameWidth()) + k.margin[Left] + k.margin[Right];
                c.automatic = outer;
                c.min = std::max(c.min, outer);
                c.max = std::max(c.max, outer);
            } else {
                c.automatic = ks.overflow_clip ? k.frameWidth() + k.margin[Left] + k.margin[Right] : c.min;
            }
            out.push_back(c);
        }
        return out;
    }

    float gridIntrinsicWidth(Box& b, bool max_content) {
        GridPlan plan = gridPlan(b, std::nullopt, std::nullopt);
        sizeTracks(plan.cols, std::nullopt, b.style->column_gap, max_content ? TrackRoom::MaxContent : TrackRoom::MinContent, columnContributions(plan), false);
        float total = sumGaps(plan.cols, b.style->column_gap, 0, static_cast<int>(plan.cols.size()));
        for (const SizedTrack& t : plan.cols)
            if (!t.collapsed) total += t.base;
        return total;
    }

    // Lays an item out unless the last time was already at this size: nested grids would
    // otherwise double the work at every level.
    void gridItemLayout(Box& k, float width, std::optional<float> height) {
        const float wanted = height.value_or(-1.0f);
        if (k.laid_width == width && k.laid_height == wanted) return;
        layoutItem(k, width, height);
        k.laid_width = width;
        k.laid_height = wanted;
    }

    // Its natural height at a width; only worked out once for each width.
    float gridItemNaturalHeight(Box& k, float width) {
        if (k.measured_width == width) return k.measured_height;
        gridItemLayout(k, width, std::nullopt);
        k.measured_width = width;
        k.measured_height = k.height;
        return k.height;
    }

    float gridItemWidth(Box& k, float area, FlexAlign justify) {
        const ComputedStyle& ks = *k.style;
        const float room = std::max(0.0f, area - k.frameWidth() - k.margin[Left] - k.margin[Right]);
        if (!ks.width.is_auto) {
            float w = ks.width.resolve(area);
            if (ks.border_box) w -= k.frameWidth();
            return clampWidth(k, std::max(0.0f, w), area);
        }
        const bool stretch = (justify == FlexAlign::Auto || justify == FlexAlign::Stretch) && !ks.margin[Left].is_auto && !ks.margin[Right].is_auto;
        const bool keyword = ks.width_keyword != ComputedStyle::WidthKeyword::None;
        return clampWidth(k, stretch && !keyword ? room : shrinkToFit(k, room), area);
    }

    static void alignTracks(std::vector<SizedTrack>& t, float gap, std::optional<float> avail, FlexAlign align) {
        float total = sumGaps(t, gap, 0, static_cast<int>(t.size()));
        int live = 0;
        for (const SizedTrack& k : t) {
            if (k.collapsed) continue;
            total += k.base;
            ++live;
        }
        const float free_space = avail ? *avail - total : 0;
        float pos = 0, between = gap;
        if (live > 0) {
            const auto n = static_cast<float>(live);
            if (free_space > 0) {
                switch (align) {
                    case FlexAlign::End: pos = free_space; break;
                    case FlexAlign::Center: pos = free_space / 2; break;
                    case FlexAlign::SpaceBetween:
                        if (live > 1) between += free_space / (n - 1);
                        break;
                    case FlexAlign::SpaceAround:
                        pos = free_space / n / 2;
                        between += free_space / n;
                        break;
                    case FlexAlign::SpaceEvenly:
                        pos = free_space / (n + 1);
                        between += free_space / (n + 1);
                        break;
                    default: break;
                }
            } else if (free_space < 0) {
                if (align == FlexAlign::End) pos = free_space;
                else if (align == FlexAlign::Center) pos = free_space / 2;
            }
        }
        for (SizedTrack& k : t) {
            k.offset = pos;
            if (k.collapsed) continue;
            pos += k.base + between;
        }
    }

    static float areaSize(const std::vector<SizedTrack>& t, float gap, int from, int to) {
        float total = sumGaps(t, gap, from, to);
        for (int i = from; i < to; ++i)
            if (!t[static_cast<size_t>(i)].collapsed) total += t[static_cast<size_t>(i)].base;
        return total;
    }

    void layoutGrid(Box& c, std::optional<float> own_height) {
        const ComputedStyle& s = *c.style;
        for (auto& kp : c.kids) {
            if (!kp->outOfFlow()) continue;
            kp->static_y = 0;
            layoutOutOfFlow(*kp, c.width);
        }

        // Repeats that fit a height can go by a definite one or by a max-height.
        std::optional<float> repeat_height = own_height;
        if (!repeat_height && !s.max_height.is_auto && !s.max_height.hasPercent()) repeat_height = s.max_height.px - (s.border_box ? c.frameHeight() : 0);
        GridPlan plan = gridPlan(c, c.width, repeat_height);
        const FlexAlign justify_content = s.justify_content;
        const FlexAlign align_content = s.align_content;

        // Columns come first, as a row's height depends on how wide things in it are.
        sizeTracks(plan.cols, c.width, s.column_gap, TrackRoom::Definite, columnContributions(plan),
                   justify_content == FlexAlign::Auto || justify_content == FlexAlign::Stretch);

        std::vector<TrackContribution> row_asks;
        row_asks.reserve(plan.items.size());
        for (GridPlacedItem& it : plan.items) {
            Box& k = *it.box;
            const ComputedStyle& ks = *k.style;
            const float area_w = areaSize(plan.cols, s.column_gap, it.col0, it.col1);
            resolveSpacing(k, area_w);
            const FlexAlign justify = ks.justify_self == FlexAlign::Auto ? s.justify_items : ks.justify_self;
            const float w = gridItemWidth(k, area_w, justify);
            float natural;
            if (const auto fixed = !ks.height.hasPercent() ? specifiedHeight(k, std::nullopt) : std::nullopt) natural = *fixed;
            else natural = gridItemNaturalHeight(k, w);
            const float outer = natural + k.frameHeight() + k.margin[Top] + k.margin[Bottom];
            TrackContribution ask;
            ask.from = it.row0;
            ask.to = it.row1;
            ask.min = ask.max = outer;
            ask.automatic = ks.overflow_clip ? k.frameHeight() + k.margin[Top] + k.margin[Bottom] : outer;
            row_asks.push_back(ask);
        }

        const bool stretch_rows = align_content == FlexAlign::Auto || align_content == FlexAlign::Stretch;
        const std::vector<SizedTrack> rows_before = plan.rows;
        sizeTracks(plan.rows, own_height, s.row_gap, own_height ? TrackRoom::Definite : TrackRoom::MaxContent, row_asks, stretch_rows);
        const auto rows_total = [&] { return areaSize(plan.rows, s.row_gap, 0, static_cast<int>(plan.rows.size())); };
        const float box_extra = s.border_box ? c.frameHeight() : 0;
        const float min_h = s.min_height.hasPercent() ? 0 : std::max(0.0f, s.min_height.px - box_extra);
        const float max_h = s.max_height.is_auto || s.max_height.hasPercent() ? kNoLimit : std::max(0.0f, s.max_height.px - box_extra);
        std::optional<float> rows_room = own_height;
        if (!own_height) {
            // Under a minimum, or over a maximum, it's as if that had been the height.
            const float natural = rows_total();
            if (natural < min_h) rows_room = min_h;
            else if (natural > max_h) rows_room = max_h;
            if (rows_room) {
                plan.rows = rows_before;
                sizeTracks(plan.rows, rows_room, s.row_gap, TrackRoom::Definite, row_asks, stretch_rows);
            }
        }

        alignTracks(plan.cols, s.column_gap, c.width, justify_content);
        alignTracks(plan.rows, s.row_gap, rows_room, align_content);

        for (GridPlacedItem& it : plan.items) {
            Box& k = *it.box;
            const ComputedStyle& ks = *k.style;
            const float area_w = areaSize(plan.cols, s.column_gap, it.col0, it.col1);
            const float area_h = areaSize(plan.rows, s.row_gap, it.row0, it.row1);
            const float area_x = plan.cols[static_cast<size_t>(it.col0)].offset;
            const float area_y = plan.rows[static_cast<size_t>(it.row0)].offset;
            const FlexAlign justify = ks.justify_self == FlexAlign::Auto ? s.justify_items : ks.justify_self;
            const FlexAlign align = ks.align_self == FlexAlign::Auto ? s.align_items : ks.align_self;
            const float w = gridItemWidth(k, area_w, justify);
            const bool stretch_h = ks.height.is_auto && (align == FlexAlign::Auto || align == FlexAlign::Stretch) && !ks.margin[Top].is_auto && !ks.margin[Bottom].is_auto;
            std::optional<float> h = specifiedHeight(k, area_h);
            if (stretch_h) h = std::max(0.0f, area_h - k.frameHeight() - k.margin[Top] - k.margin[Bottom]);
            if (h) {
                if (!ks.max_height.is_auto) *h = std::min(*h, std::max(0.0f, ks.max_height.resolve(area_h) - (ks.border_box ? k.frameHeight() : 0)));
                *h = std::max(*h, std::max(0.0f, ks.min_height.resolve(area_h) - (ks.border_box ? k.frameHeight() : 0)));
            }
            gridItemLayout(k, w, h);

            const auto offset_in = [](float free_space, FlexAlign a, bool auto_a, bool auto_b) {
                if (auto_a && auto_b) return std::max(0.0f, free_space) / 2;
                if (auto_a) return std::max(0.0f, free_space);
                if (auto_b) return 0.0f;
                if (a == FlexAlign::End) return free_space;
                if (a == FlexAlign::Center) return free_space / 2;
                return 0.0f;
            };
            const float free_w = area_w - k.outerWidth() - k.margin[Left] - k.margin[Right];
            const float free_h = area_h - k.outerHeight() - k.margin[Top] - k.margin[Bottom];
            k.x = area_x + k.margin[Left] + offset_in(free_w, justify, ks.margin[Left].is_auto, ks.margin[Right].is_auto);
            k.y = area_y + k.margin[Top] + offset_in(free_h, align, ks.margin[Top].is_auto, ks.margin[Bottom].is_auto);
        }

        float h = own_height.value_or(rows_room.value_or(rows_total()));
        if (!own_height) h = std::min(h, max_h);
        h = std::max(h, min_h);
        c.height = clampCoord(h);
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
        const Box* deco = nullptr;
    };

    struct InlineContext {
        Color background{0, 0, 0, 0};
        const std::string* link = nullptr;
        const Box* deco = nullptr;
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
                    it.deco = ctx.deco;
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
                    if (k.node && k.node->isHtml("a"))
                        if (const std::string* href = k.node->attr("href")) inner.link = href;
                    resolveSpacing(k, cb_width);
                    // Its background and borders are painted around its fragments, line by line.
                    const bool bordered = std::any_of(std::begin(k.border), std::end(k.border), [](float w) { return w > 0; });
                    if (s.background.visible() || (s.background_layers && !s.masked) || bordered || s.box_shadow) {
                        k.deco_parent = ctx.deco;
                        inner.deco = &k;
                    }
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
                    it.deco = ctx.deco;
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

    // text-overflow: ellipsis. A clipping block's line that runs past its content box
    // ends in "…" instead: the fragments that still fit with it stay, the one across
    // the edge keeps the characters that fit, the rest of the line goes. The ellipsis
    // takes the block's own font and colour. (Left to right only.)
    void applyEllipsis(Box& b) {
        const ComputedStyle& s = *b.style;
        if (!s.overflow_clip || !s.text_overflow_ellipsis || b.fragments.empty()) return;
        static const std::string kEllipsis = "\u2026";
        const float ellipsis_w = text_.width(kEllipsis, fontOf(s));
        std::vector<Box::Fragment> out;
        out.reserve(b.fragments.size() + 1);
        size_t i = 0;
        while (i < b.fragments.size()) {
            // One line: the fragments on the same y.
            size_t end = i + 1;
            while (end < b.fragments.size() && std::abs(b.fragments[end].y - b.fragments[i].y) < 0.5f) ++end;
            float right = 0;
            for (size_t k = i; k < end; ++k) right = std::max(right, b.fragments[k].x + b.fragments[k].w);
            if (right <= b.width + 0.5f) {
                out.insert(out.end(), b.fragments.begin() + static_cast<ptrdiff_t>(i), b.fragments.begin() + static_cast<ptrdiff_t>(end));
                i = end;
                continue;
            }
            const float avail = b.width - ellipsis_w;
            const Box::Fragment* last = nullptr;
            float at = 0;
            for (size_t k = i; k < end; ++k) {
                Box::Fragment f = b.fragments[k];
                if (f.x + f.w <= avail) {
                    out.push_back(std::move(f));
                    last = &out.back();
                    at = last->x + last->w;
                    continue;
                }
                if (f.atomic || f.x >= avail) break;
                // Across the edge: as many whole characters as fit.
                const FontSpec font = fontOf(*f.style);
                std::string kept;
                for (size_t c = 0; c < f.text.size();) {
                    size_t n = 1;
                    const auto lead = static_cast<unsigned char>(f.text[c]);
                    if (lead >= 0xF0) n = 4;
                    else if (lead >= 0xE0) n = 3;
                    else if (lead >= 0xC0) n = 2;
                    const std::string next = kept + f.text.substr(c, n);
                    if (f.x + text_.width(next, font) > avail) break;
                    kept = next;
                    c += n;
                }
                while (!kept.empty() && kept.back() == ' ') kept.pop_back();
                if (!kept.empty()) {
                    f.text = kept;
                    f.w = text_.width(kept, font);
                    out.push_back(std::move(f));
                    last = &out.back();
                    at = last->x + last->w;
                }
                break;
            }
            Box::Fragment e = last ? *last : b.fragments[i];
            e.x = last ? at : 0;
            e.w = ellipsis_w;
            e.text = kEllipsis;
            e.style = &s;
            e.atomic = nullptr;
            e.background = Color{0, 0, 0, 0};
            out.push_back(std::move(e));
            i = end;
        }
        b.fragments = std::move(out);
    }

    // Returns the height of the lines; fills b.fragments.
    float layoutInline(Box& b, Flow flow) {
        b.fragments.clear();
        b.inline_extent = 0;
        b.last_baseline = -1;
        // An inline-block counts its own <a> for what it contains.
        InlineContext ctx;
        if (b.node && b.node->isHtml("a"))
            if (const std::string* href = b.node->attr("href")) ctx.link = href;
        if (b.kids.size() == 1 && b.kids[0]->kind == BoxKind::Replaced && b.kind == BoxKind::Block && b.kids[0]->node == b.node) {
            // A display:block replaced element is exactly its one box.
            Box& r = *b.kids[0];
            r.cb_height_hint = b.cb_height_hint;   // the wrapper is the picture: same containing block
            // A width the wrapper resolved (it has the same style) is the picture's too.
            const bool fills = !widthIsAuto(*b.style, b.width);
            layoutReplaced(r, b.width, fills ? &b.width : nullptr);
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
                    pushChunk(Chunk{Chunk::Space, {}, it.width, it.style, {0, 0, 0, 0}, nullptr, nullptr, nullptr});
                    break;
                case InlineItem::Atomic:
                    flushRun();
                    addSpaceIfPending();
                    pushChunk(Chunk{Chunk::Atomic, {}, marginBoxWidth(*it.atomic), it.style, {0, 0, 0, 0}, it.link, it.atomic, it.deco});
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
                    flushRun();
                    l_.layoutOutOfFlow(*it.atomic, b_.width);
                    if (it.atomic->style->inline_level_origin) {
                        // Inline-level: where it would have been on this line (an icon
                        // after a link's text).
                        it.atomic->static_x = x_;
                        it.atomic->static_y = y_;
                    } else {
                        // Its static position: where the next block would have started.
                        it.atomic->static_x = 0;
                        it.atomic->static_y = y_ + (line_.empty() ? 0 : l_.lineHeightOf(*b_.style));
                    }
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
            const Box* deco = nullptr;
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
            // Its bottom (top) edge where the parent's text bottom (top) is.
            if (a.style->vertical_align == VerticalAlign::TextBottom) return total - l_.text_.descent(fontOf(*b_.style));
            if (a.style->vertical_align == VerticalAlign::TextTop) return l_.text_.ascent(fontOf(*b_.style));
            if (a.kind == BoxKind::Replaced && a.last_baseline >= 0)
                return a.margin[Top] + a.border[Top] + a.padding[Top] + a.last_baseline;
            if (a.kind != BoxKind::InlineBlock || a.style->overflow_clip) return total;
            const Box* inner = &a;
            float origin = a.margin[Top] + a.border[Top] + a.padding[Top];
            while (inner->last_baseline < 0) {
                // A flex container or a table goes by its first line, a block by its last.
                const Box* next = nullptr;
                const bool from_start = inner->isFlex() || inner->isGrid() || inner->isTable() || inner->display() == Display::TableRowGroup ||
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
            c.deco = space_deco_;
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
            f.deco = c.deco;
            if (c.kind == Chunk::Atomic) {
                f.atomic = c.atomic;
                line_has_content_ = true;
                line_.push_back(std::move(f));
            } else if (!c.text.empty()) {
                // Next to text in the same style: extend it, so it's drawn as one run.
                if (!line_.empty() && !line_.back().atomic && line_.back().style == c.style &&
                    line_.back().background == c.background && line_.back().link == c.link && line_.back().deco == c.deco &&
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
            // An inline's closing padding, border and margin: no fragment, but the line
            // reaches past it.
            if (c.kind == Chunk::Space && c.text.empty()) tail_spacer_ = x_;
            else if (c.kind == Chunk::Atomic || !c.text.empty()) tail_spacer_ = -1;
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
            c.deco = it.deco;
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
                            space_deco_ = it.deco;
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
                    sp.deco = it.deco;
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

            const auto textShift = [&](const ComputedStyle& s) {
                // Measured from the parent's size; "smaller" is what usually got us here.
                if (s.vertical_align == VerticalAlign::Sub) return s.font_size * 1.2f / 5 + 1;
                if (s.vertical_align == VerticalAlign::Super) return -(s.font_size * 1.2f / 3 + 1);
                if (s.vertical_align == VerticalAlign::Middle && &s != &container) {
                    // Its middle at the baseline plus half the x-height (as for an atomic).
                    const FontSpec f = fontOf(s);
                    return (l_.text_.ascent(f) - l_.text_.descent(f)) / 2 - container.font_size * 0.25f;
                }
                return 0.0f;
            };

            // vertical-align: top and bottom boxes sit at the line's edges: they don't take
            // part in the baseline, and only make the line taller if it's shorter than them.
            const auto edgeAligned = [](const Box& a) {
                return a.style->vertical_align == VerticalAlign::Top || a.style->vertical_align == VerticalAlign::Bottom;
            };
            const auto totalOf = [](const Box& a) { return a.outerHeight() + a.margin[Top] + a.margin[Bottom]; };
            for (const auto& f : line_) {
                if (f.atomic) {
                    if (edgeAligned(*f.atomic)) continue;
                    const float total = totalOf(*f.atomic);
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

            for (const auto& f : line_)
                if (f.atomic && edgeAligned(*f.atomic)) {
                    const float total = totalOf(*f.atomic);
                    if (f.atomic->style->vertical_align == VerticalAlign::Top) below = std::max(below, total - above);
                    else above = std::max(above, total - below);
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

            if (!line_.empty() && tail_spacer_ > line_w) line_w = tail_spacer_;
            tail_spacer_ = -1;
            if (!line_.empty()) b_.inline_extent = std::max(b_.inline_extent, line_w);

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
                f.baseline = baseline;
                f.line = line_index_;
                if (f.atomic) {
                    Box* a = f.atomic;
                    a->x = f.x + a->margin[Left];
                    if (a->style->vertical_align == VerticalAlign::Top) a->y = y_ + a->margin[Top];
                    else if (a->style->vertical_align == VerticalAlign::Bottom) a->y = y_ + above + below - totalOf(*a) + a->margin[Top];
                    else a->y = baseline - atomicBaseline(*a) + a->margin[Top];
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
            ++line_index_;
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
        const Box* space_deco_ = nullptr;
        float tail_spacer_ = -1;   // where a closing spacer after the line's last fragment ends
        uint32_t line_index_ = 0;
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

    // Background (or mask) layers, bottom first. `tint`: draw the images' alpha in that colour.
    // `canvas`: paint over this area instead of the box's own (the root's background).
    void paintLayers(const Box& b, float x, float y, const BackgroundLayers& layers, float opacity, const Color* tint, Rect canvas,
                     bool use_canvas) {
        const float w = b.outerWidth(), h = b.outerHeight();
        const auto area = [&](BackgroundBox which) {
            Rect r{x, y, w, h};
            if (which == BackgroundBox::Border) return r;
            r.x += b.border[Left];
            r.y += b.border[Top];
            r.w -= b.border[Left] + b.border[Right];
            r.h -= b.border[Top] + b.border[Bottom];
            if (which == BackgroundBox::Content) {
                r.x += b.padding[Left];
                r.y += b.padding[Top];
                r.w -= b.padding[Left] + b.padding[Right];
                r.h -= b.padding[Top] + b.padding[Bottom];
            }
            r.w = std::max(0.0f, r.w);
            r.h = std::max(0.0f, r.h);
            return r;
        };
        constexpr size_t kMaxTiles = 10'000;
        for (size_t i = layers.images.size(); i-- > 0;) {
            const BackgroundImage& image = layers.images[i];
            if (image.none()) continue;
            if (tint && !image.gradient && image.url.empty()) continue;
            const auto pick = [&](const auto& list) { return list[i % list.size()]; };
            if (layers.sizes.empty() || layers.positions.empty() || layers.repeats.empty() || layers.origins.empty() || layers.clips.empty()) return;
            const Rect origin = area(pick(layers.origins));
            const Rect clip = use_canvas ? canvas : area(pick(layers.clips));
            if (clip.w <= 0 || clip.h <= 0) continue;
            // What the image brings: a size, a shape, or (a gradient) neither.
            std::optional<float> iw, ih;
            float ratio = 0;
            if (!image.url.empty()) {
                if (!images_) continue;
                if (const auto size = images_->imageSize(image.url)) {
                    iw = size->first;
                    ih = size->second;
                    if (*ih > 0) ratio = *iw / *ih;
                } else if (const auto rr = images_->imageRatio(image.url)) {
                    ratio = *rr;
                } else {
                    if (std::find(out_->images_wanted.begin(), out_->images_wanted.end(), image.url) == out_->images_wanted.end() &&
                        out_->images_wanted.size() < 512)
                        out_->images_wanted.push_back(image.url);
                    continue;
                }
            } else if (tint) {
                continue;   // a gradient mask: drawn as if there were none
            }
            const BackgroundSize size = pick(layers.sizes);
            float tw = 0, th = 0;
            if (size.kind != BackgroundSize::Kind::Explicit) {
                if (ratio > 0) {
                    const float k = size.kind == BackgroundSize::Kind::Contain ? std::min(origin.w / ratio, origin.h) : std::max(origin.w / ratio, origin.h);
                    th = k;
                    tw = k * ratio;
                } else {
                    tw = origin.w;
                    th = origin.h;
                }
            } else {
                const bool aw = size.width.is_auto, ah = size.height.is_auto;
                if (!aw) tw = size.width.resolve(origin.w);
                if (!ah) th = size.height.resolve(origin.h);
                if (aw && ah) {
                    if (iw && ih) { tw = *iw; th = *ih; }
                    else if (ratio > 0) {
                        const float k = std::min(origin.w / ratio, origin.h);
                        tw = k * ratio;
                        th = k;
                    } else { tw = origin.w; th = origin.h; }
                } else if (aw) {
                    tw = ratio > 0 ? th * ratio : iw ? *iw : origin.w;
                } else if (ah) {
                    th = ratio > 0 ? tw / ratio : ih ? *ih : origin.h;
                }
            }
            if (!(tw > 0.01f) || !(th > 0.01f) || tw > kMaxCoord || th > kMaxCoord) continue;
            const auto [pos_x, pos_y] = pick(layers.positions);
            const float px = origin.x + pos_x.px + pos_x.percent * (origin.w - tw) / 100.0f;
            const float py = origin.y + pos_y.px + pos_y.percent * (origin.h - th) / 100.0f;
            const auto [rx, ry] = pick(layers.repeats);
            float x0 = px, y0 = py;
            if (rx) x0 = px - std::ceil((px - clip.x) / tw) * tw;
            if (ry) y0 = py - std::ceil((py - clip.y) / th) * th;
            const float x_end = rx ? clip.x + clip.w : px + tw * 0.5f;
            const float y_end = ry ? clip.y + clip.h : py + th * 0.5f;
            const double tiles = (rx ? std::ceil((x_end - x0) / tw) : 1.0) * (ry ? std::ceil((y_end - y0) / th) : 1.0);
            if (!(tiles <= static_cast<double>(kMaxTiles))) continue;
            PaintOp begin;
            begin.kind = PaintKind::ClipBegin;
            begin.x = clip.x;
            begin.y = clip.y;
            begin.w = clip.w;
            begin.h = clip.h;
            out_->ops.push_back(begin);
            for (float ty = y0; ty < y_end; ty += th) {
                for (float tx = x0; tx < x_end; tx += tw) {
                    if (tx + tw <= clip.x || ty + th <= clip.y || tx >= clip.x + clip.w || ty >= clip.y + clip.h) {
                        if (!rx) break;
                        continue;
                    }
                    PaintOp op;
                    op.x = tx;
                    op.y = ty;
                    op.w = tw;
                    op.h = th;
                    if (image.gradient) {
                        op.kind = PaintKind::Gradient;
                        op.gradient = image.gradient;
                        op.color = faded(Color{255, 255, 255, 255}, opacity);
                    } else {
                        op.kind = PaintKind::Image;
                        op.text = image.url;
                        op.tint = tint != nullptr;
                        op.color = tint ? *tint : faded(Color{255, 255, 255, 255}, opacity);
                    }
                    out_->ops.push_back(std::move(op));
                    if (!rx) break;
                }
                if (!ry) break;
            }
            PaintOp end;
            end.kind = PaintKind::ClipEnd;
            out_->ops.push_back(end);
        }
    }

    void addBoxPaint(float x, float y, float w, float h, std::shared_ptr<const BoxPaint> box) {
        if (!(w > 0) || !(h > 0) || w > kMaxCoord || h > kMaxCoord) return;
        PaintOp op;
        op.kind = PaintKind::Box;
        op.x = x;
        op.y = y;
        op.w = w;
        op.h = h;
        op.box = std::move(box);
        out_->ops.push_back(std::move(op));
    }

    void paintShadows(const BoxPaint& base, float x, float y, float opacity, bool inset, const std::vector<BoxShadow>& shadows) {
        // The first is on top: painted last.
        for (size_t i = shadows.size(); i-- > 0;) {
            const BoxShadow& sh = shadows[i];
            if (sh.inset != inset || !sh.color.visible()) continue;
            auto p = std::make_shared<BoxPaint>(base);
            p->part = inset ? BoxPaint::Part::InsetShadow : BoxPaint::Part::OuterShadow;
            p->shadow = sh;
            p->shadow.color = faded(sh.color, opacity);
            if (inset) {
                p->ox = p->oy = 0;
                addBoxPaint(x, y, base.w, base.h, std::move(p));
                continue;
            }
            // Room for the blur, the spread and the offset around the box.
            const float m = std::max(0.0f, sh.spread) + sh.blur * 1.5f + 1;
            const float left = std::max(0.0f, m - sh.x), top = std::max(0.0f, m - sh.y);
            const float right = std::max(0.0f, m + sh.x), bottom = std::max(0.0f, m + sh.y);
            p->ox = left;
            p->oy = top;
            addBoxPaint(x - left, y - top, base.w + left + right, base.h + top + bottom, std::move(p));
        }
    }

    // Whether Chromium would draw this with its own theme instead of its CSS border.
    static bool nativeControl(const Box& b) {
        if (!b.node || !b.style->native_look) return false;
        const ControlKind k = controlKind(b.node);
        return k == ControlKind::Text || k == ControlKind::Password || k == ControlKind::TextArea || k == ControlKind::Select ||
               k == ControlKind::Submit || k == ControlKind::Reset || k == ControlKind::Button;
    }

    void paintFrame(const Box& b, float x, float y, float opacity) {
        const ComputedStyle& s = *b.style;
        const float w = b.outerWidth(), h = b.outerHeight();
        if (s.hidden) return;
        if (nativeControl(b)) {
            // Chromium's light theme: a 1px #767676 outline with corners of 2, darker when
            // it has the keyboard; the fill is the control's own.
            const bool focused = forms().focused == b.node;
            addRect(x, y, w, h, faded(s.background, opacity), 2);
            auto p = std::make_shared<BoxPaint>();
            p->part = BoxPaint::Part::Border;
            p->w = w;
            p->h = h;
            const float bw = focused ? 2 : 1;
            for (int i = 0; i < 4; ++i) {
                p->radii[i][0] = p->radii[i][1] = 2;
                p->border[i] = bw;
                p->colors[i] = faded(focused ? Color{0x10, 0x10, 0x10, 255} : Color{0x76, 0x76, 0x76, 255}, opacity);
                p->styles[i] = BorderStyle::Solid;
            }
            fitRadii(p->radii, w, h);
            addBoxPaint(x, y, w, h, std::move(p));
            return;
        }
        // Corners, as lengths of this box, fitted so neighbours don't overlap.
        BoxPaint base;
        base.w = w;
        base.h = h;
        bool rounded = false;
        for (int i = 0; i < 4; ++i) {
            base.radii[i][0] = std::max(0.0f, s.radius[i][0].resolve(w));
            base.radii[i][1] = std::max(0.0f, s.radius[i][1].resolve(h));
            if (base.radii[i][0] <= 0 || base.radii[i][1] <= 0) base.radii[i][0] = base.radii[i][1] = 0;
            rounded = rounded || base.radii[i][0] > 0;
        }
        fitRadii(base.radii, w, h);
        // One circular radius all round is what a plain rectangle can do.
        const float r0 = base.radii[0][0];
        bool uniform = true;
        for (int i = 0; i < 4; ++i) uniform = uniform && base.radii[i][0] == r0 && base.radii[i][1] == r0;
        for (int i = 0; i < 4; ++i) {
            base.border[i] = b.border[i];
            base.colors[i] = faded(s.border_color[i], opacity);
            base.styles[i] = s.border_style[i];
        }

        const bool own_background = &b != skip_background_;
        if (own_background && s.box_shadow) paintShadows(base, x, y, opacity, false, *s.box_shadow);
        // A mask shows the background colour only where the mask's images are.
        const Color bg = !own_background || s.masked ? Color{0, 0, 0, 0} : faded(s.background, opacity);
        if (bg.visible()) {
            if (uniform) {
                addRect(x, y, w, h, bg, r0);
            } else {
                auto p = std::make_shared<BoxPaint>(base);
                p->part = BoxPaint::Part::Background;
                p->background = bg;
                addBoxPaint(x, y, w, h, std::move(p));
            }
        }
        if (own_background) {
            if (s.masked) {
                const Color tint = faded(s.background, opacity);
                if (tint.visible()) paintLayers(b, x, y, *s.mask_layers, opacity, &tint, Rect{0, 0, 0, 0}, false);
            } else if (s.background_layers) {
                paintLayers(b, x, y, *s.background_layers, opacity, nullptr, Rect{0, 0, 0, 0}, false);
            }
            if (s.box_shadow) paintShadows(base, x, y, opacity, true, *s.box_shadow);
        }

        const float t = b.border[Top], rt = b.border[Right], bt = b.border[Bottom], l = b.border[Left];
        if (t <= 0 && rt <= 0 && bt <= 0 && l <= 0) return;
        // Square, solid and the same colour where sides meet: rectangles. Anything else is
        // drawn: rounded, patterned, shaded, or two colours meeting on a diagonal.
        bool plain = !rounded || b.collapsed_border;
        for (int i = 0; i < 4 && plain; ++i) {
            const BorderStyle st = s.border_style[i];
            if (b.border[i] > 0 && st != BorderStyle::Solid && st != BorderStyle::None && st != BorderStyle::Hidden && !b.collapsed_border) plain = false;
            const int next = (i + 1) % 4;
            if (b.border[i] > 0 && b.border[next] > 0 && std::max(b.border[i], b.border[next]) >= 2 && !(s.border_color[i] == s.border_color[next]) &&
                !b.collapsed_border)
                plain = false;
        }
        if (!plain) {
            auto p = std::make_shared<BoxPaint>(base);
            p->part = BoxPaint::Part::Border;
            addBoxPaint(x, y, w, h, std::move(p));
            return;
        }
        // A collapsed border is centred on the edge the two cells share.
        const float grow = b.collapsed_border ? 1 : 0;
        if (t > 0) addRect(x - grow * l, y - grow * t, w + grow * (l + rt), t * (1 + grow), faded(s.border_color[Top], opacity));
        if (bt > 0) addRect(x - grow * l, y + h - bt, w + grow * (l + rt), bt * (1 + grow), faded(s.border_color[Bottom], opacity));
        // Sides don't overlap at the corners, or a translucent colour would show twice there.
        const float side_top = grow > 0 ? y - t : y + t, side_h = grow > 0 ? h + t + bt : h - t - bt;
        if (l > 0 && side_h > 0) addRect(x - grow * l, side_top, l * (1 + grow), side_h, faded(s.border_color[Left], opacity));
        if (rt > 0 && side_h > 0) addRect(x + w - rt, side_top, rt * (1 + grow), side_h, faded(s.border_color[Right], opacity));
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

    // A control's placeholder: its ::placeholder's colour (and opacity), or the usual grey.
    Color placeholderColor(const Node* n) const {
        const ComputedStyle* ph = styles_.placeholderOf(n);
        if (!ph) return Color{0x75, 0x75, 0x75, 255};
        return faded(ph->color, std::clamp(ph->opacity, 0.0f, 1.0f));
    }

    // Where a picture goes in its content box (w x h at x, y) by object-fit and
    // object-position, and which part of it shows there.
    void fitPicture(PaintOp& op, const ComputedStyle& s, float x, float y, float w, float h, const std::string& src) {
        op.x = x;
        op.y = y;
        op.w = w;
        op.h = h;
        using F = ComputedStyle::Fit;
        if (s.object_fit == F::Fill || w <= 0 || h <= 0) return;
        float iw = 0, ih = 0;
        if (const auto size = images_->imageSize(src)) { iw = size->first; ih = size->second; }
        else if (const auto ratio = images_->imageRatio(src); ratio && *ratio > 0) { iw = w; ih = w / *ratio; }   // a shape only
        if (iw <= 0 || ih <= 0) return;
        const float contain = std::min(w / iw, h / ih), cover = std::max(w / iw, h / ih);
        float scale = 1;
        switch (s.object_fit) {
            case F::Contain: scale = contain; break;
            case F::Cover: scale = cover; break;
            case F::None: scale = 1; break;
            case F::ScaleDown: scale = std::min(1.0f, contain); break;
            case F::Fill: break;
        }
        const float dw = iw * scale, dh = ih * scale;
        const float dx = x + s.object_x.resolve(w - dw), dy = y + s.object_y.resolve(h - dh);
        // What of it falls inside the box.
        const float x0 = std::max(x, dx), y0 = std::max(y, dy);
        const float x1 = std::min(x + w, dx + dw), y1 = std::min(y + h, dy + dh);
        if (x1 <= x0 || y1 <= y0) { op.w = op.h = 0; return; }
        op.x = x0;
        op.y = y0;
        op.w = x1 - x0;
        op.h = y1 - y0;
        op.u0 = (x0 - dx) / dw;
        op.v0 = (y0 - dy) / dh;
        op.u1 = (x1 - dx) / dw;
        op.v1 = (y1 - dy) / dh;
    }

    void paintReplaced(const Box& b, float x, float y, float opacity) {
        const ComputedStyle& s = *b.style;
        paintFrame(b, x, y, opacity);
        if (s.hidden) return;
        const float cx = x + b.border[Left] + b.padding[Left];
        const float cy = y + b.border[Top] + b.padding[Top];
        const Node* n = b.node;
        // A <canvas>: what its scripts drew, which whoever draws the page asks them for.
        if (n->isHtml("canvas")) {
            if (b.width <= 0 || b.height <= 0) return;
            PaintOp op;
            op.kind = PaintKind::Image;
            op.text = kCanvasSource;
            op.node = n;
            op.x = cx;
            op.y = cy;
            op.w = b.width;
            op.h = b.height;
            op.color = faded(Color{255, 255, 255, 255}, opacity);
            op.filter = s.filter;
            out_->ops.push_back(std::move(op));
            return;
        }
        // A picture: an <img>'s source, or a <video>'s poster (all of a video we show).
        const bool is_img = n->isHtml("img");
        if (is_img || n->isHtml("video")) {
            const std::string* src = n->attr(is_img ? "src" : "poster");
            if (src && src->empty()) src = nullptr;
            if (src && images_ && (images_->imageSize(*src) || images_->imageRatio(*src))) {
                PaintOp op;
                op.kind = PaintKind::Image;
                op.text = *src;
                op.color = faded(Color{255, 255, 255, 255}, opacity);
                op.filter = s.filter;
                fitPicture(op, s, cx, cy, b.width, b.height, *src);
                if (op.w <= 0 || op.h <= 0) return;
                // border-radius rounds the picture too: inside its border and padding (when
                // the picture reaches the corners).
                if (op.x <= cx + 0.5f && op.y <= cy + 0.5f && op.x + op.w >= cx + b.width - 0.5f && op.y + op.h >= cy + b.height - 0.5f) {
                    float radii[4][2];
                    const float bw = b.outerWidth(), bh = b.outerHeight();
                    for (int i = 0; i < 4; ++i) {
                        radii[i][0] = std::max(0.0f, s.radius[i][0].resolve(bw));
                        radii[i][1] = std::max(0.0f, s.radius[i][1].resolve(bh));
                    }
                    fitRadii(radii, bw, bh);
                    op.radius = std::max(0.0f, radii[0][0] - b.border[Left] - b.padding[Left]);
                    op.radius_y = std::max(0.0f, radii[0][1] - b.border[Top] - b.padding[Top]);
                    if (op.radius <= 0 || op.radius_y <= 0) op.radius = op.radius_y = 0;
                }
                out_->ops.push_back(std::move(op));
                return;
            }
            // Not loaded: maybe one a script just added. Ask for it.
            if (src && images_ && out_->images_wanted.size() < 512 &&
                std::find(out_->images_wanted.begin(), out_->images_wanted.end(), *src) == out_->images_wanted.end())
                out_->images_wanted.push_back(*src);
            if (is_img && !b.alt.empty()) addText(cx, cy, b.alt, s, opacity);
            return;
        }
        if (n->ns == Namespace::Svg && n->name == "svg") {
            if (b.width <= 0 || b.height <= 0) return;
            PaintOp op;
            op.kind = PaintKind::Svg;
            op.x = cx;
            op.y = cy;
            op.w = b.width;
            op.h = b.height;
            op.node = n;
            op.color = faded(s.color, opacity);
            out_->ops.push_back(std::move(op));
            return;
        }
        paintControl(b, x, y, cx, cy, opacity);
    }

    // A form control's insides: its text (or what it's waiting for), a caret if it's
    // being typed into, a check, a dot or an arrow.
    void paintControl(const Box& b, float x, float y, float cx, float cy, float opacity) {
        const ComputedStyle& s = *b.style;
        const Node* n = b.node;
        const ControlKind kind = controlKind(n);
        const FormState& st = forms();
        if (kind == ControlKind::Checkbox || kind == ControlKind::Radio) {
            auto p = std::make_shared<BoxPaint>();
            p->part = kind == ControlKind::Checkbox ? BoxPaint::Part::Checkbox : BoxPaint::Part::Radio;
            p->w = b.outerWidth();
            p->h = b.outerHeight();
            p->checked = st.checked(n);
            addBoxPaint(x, y, p->w, p->h, std::move(p));
            return;
        }
        const FontSpec font = fontOf(s);
        const float line_h = lineHeightOf(s);
        const Rect content{cx, cy, b.width, b.height};
        const auto clipped = [&](auto&& draw) {
            PaintOp begin;
            begin.kind = PaintKind::ClipBegin;
            begin.x = content.x;
            begin.y = content.y;
            begin.w = content.w;
            begin.h = content.h;
            out_->ops.push_back(begin);
            draw();
            PaintOp end;
            end.kind = PaintKind::ClipEnd;
            out_->ops.push_back(end);
        };
        const auto text = [&](float tx, float ty, const std::string& t, Color c) {
            PaintOp op;
            op.kind = PaintKind::Text;
            op.x = tx;
            op.y = ty;
            op.text = t;
            op.font = font;
            op.color = faded(c, opacity);
            op.w = text_.width(t, font);
            op.h = text_.ascent(font) + text_.descent(font);
            out_->ops.push_back(std::move(op));
        };
        const bool focused = st.focused == n;
        if (kind == ControlKind::Text || kind == ControlKind::Password) {
            const std::string value = st.value(n);
            std::string shown = value;
            size_t caret = std::min(st.caret, value.size());
            if (kind == ControlKind::Password) {
                // A dot for each character, and the caret after as many.
                std::string dots;
                size_t dot_caret = 0;
                for (size_t i = 0; i < value.size(); ++i)
                    if ((static_cast<unsigned char>(value[i]) & 0xC0) != 0x80) {
                        if (i < caret) dot_caret += 3;
                        dots += "\u2022";
                    }
                shown = dots;
                caret = dot_caret;
            }
            const float ty = cy + (b.height - line_h) / 2;
            // Scrolled so the caret shows.
            const float caret_x = text_.width(shown.substr(0, caret), font);
            const float scroll = focused ? std::max(0.0f, caret_x - b.width + 1) : 0;
            clipped([&] {
                if (shown.empty()) {
                    if (const std::string* ph = n->attr("placeholder"); ph && !ph->empty()) text(cx, ty, *ph, placeholderColor(n));
                } else {
                    text(cx - scroll, ty, shown, s.color);
                }
                if (focused) addRect(cx + caret_x - scroll, ty, 1, line_h, faded(s.color, opacity));
            });
            return;
        }
        if (kind == ControlKind::TextArea) {
            const std::string value = st.value(n);
            const size_t caret = std::min(st.caret, value.size());
            clipped([&] {
                if (value.empty()) {
                    if (const std::string* ph = n->attr("placeholder"); ph && !ph->empty()) text(cx, cy, *ph, placeholderColor(n));
                }
                size_t start = 0;
                float ly = cy;
                for (int line = 0; start <= value.size() && line < 10000; ++line) {
                    size_t end = value.find('\n', start);
                    if (end == std::string::npos) end = value.size();
                    const std::string part = value.substr(start, end - start);
                    if (!part.empty()) text(cx, ly, part, s.color);
                    if (focused && caret >= start && caret <= end) addRect(cx + text_.width(value.substr(start, caret - start), font), ly, 1, line_h, faded(s.color, opacity));
                    start = end + 1;
                    ly += line_h;
                    if (end == value.size()) break;
                }
            });
            return;
        }
        if (kind == ControlKind::Select) {
            const std::vector<const Node*> options = optionsOf(n);
            const int i = st.selected(n);
            const std::string label = i >= 0 ? optionLabel(options[static_cast<size_t>(i)]) : std::string{};
            const float arrow = std::min(20.0f, b.width);
            clipped([&] {
                if (!label.empty()) text(cx, cy + (b.height - line_h) / 2, label, s.color);
            });
            auto p = std::make_shared<BoxPaint>();
            p->part = BoxPaint::Part::SelectArrow;
            p->w = arrow;
            p->h = b.height;
            p->colors[0] = faded(s.color, opacity);
            addBoxPaint(cx + b.width - arrow, cy, arrow, b.height, std::move(p));
            return;
        }
        if (kind == ControlKind::Submit || kind == ControlKind::Reset || kind == ControlKind::Button) {
            const std::string label = controlText(n);
            if (label.empty()) return;
            const float tw = text_.width(label, font);
            clipped([&] { text(cx + std::max(0.0f, (b.width - tw) / 2), cy + (b.height - line_h) / 2, label, s.color); });
            return;
        }
        const std::string shown = controlText(n);
        if (!shown.empty()) addText(cx, cy, shown.substr(0, shown.find('\n')), s, opacity);
    }

    // Inline boxes' backgrounds and borders: on each line, one box around its fragments,
    // as tall as its font plus its padding and borders; the start and end sides only
    // where the box starts and ends. Outer ones first.
    void paintInlineDecorations(const Box& b, float cx, float cy, float opacity) {
        struct Span {
            const Box* box;
            uint32_t line;
            int depth;
            float x0, x1, top, bottom;
            bool first = false, last = false;
        };
        std::vector<Span> spans;
        std::vector<std::pair<const Box*, size_t>> last_span;   // each box's latest span
        for (const auto& f : b.fragments) {
            int depth = 0;
            for (const Box* d = f.deco; d; d = d->deco_parent) ++depth;
            for (const Box* d = f.deco; d && spans.size() < 10000; d = d->deco_parent, --depth) {
                const auto seen = std::find_if(last_span.begin(), last_span.end(), [&](const auto& e) { return e.first == d; });
                const FontSpec font = fontOf(*d->style);
                const float top = f.baseline - text_.ascent(font), bottom = f.baseline + text_.descent(font);
                if (seen != last_span.end() && spans[seen->second].line == f.line) {
                    Span& sp = spans[seen->second];
                    sp.x0 = std::min(sp.x0, f.x);
                    sp.x1 = std::max(sp.x1, f.x + f.w);
                    continue;
                }
                spans.push_back({d, f.line, depth, f.x, f.x + f.w, top, bottom, seen == last_span.end()});
                if (seen == last_span.end()) last_span.emplace_back(d, spans.size() - 1);
                else seen->second = spans.size() - 1;
            }
        }
        if (spans.empty()) return;
        for (const auto& [box, i] : last_span) spans[i].last = true;
        std::stable_sort(spans.begin(), spans.end(), [](const Span& a, const Span& c) { return a.depth < c.depth; });
        for (const Span& sp : spans) {
            const Box& d = *sp.box;
            Box frame;
            frame.kind = BoxKind::Inline;
            frame.node = d.node;
            frame.style = d.style;
            for (int i = 0; i < 4; ++i) {
                frame.padding[i] = d.padding[i];
                frame.border[i] = d.border[i];
            }
            if (!sp.first) frame.padding[Left] = frame.border[Left] = 0;
            if (!sp.last) frame.padding[Right] = frame.border[Right] = 0;
            frame.width = std::max(0.0f, sp.x1 - sp.x0);
            frame.height = std::max(0.0f, sp.bottom - sp.top);
            paintFrame(frame, cx + sp.x0 - frame.padding[Left] - frame.border[Left], cy + sp.top - frame.padding[Top] - frame.border[Top], opacity);
        }
    }

    void paintFragments(const Box& b, float cx, float cy, const Rect& cb, float opacity) {
        paintInlineDecorations(b, cx, cy, opacity);
        for (const auto& f : b.fragments) {
            const float fx = cx + f.x;
            if (f.atomic) {
                float ax = cx + f.atomic->x, ay = cy + f.atomic->y;
                shift(*f.atomic, ax, ay);
                // (A picture in a wrapper of its own was transformed with the wrapper.)
                const bool transformed = f.atomic->node != b.node && beginTransform(*f.atomic, ax, ay);
                if (f.atomic->node != b.node) {
                    record(*f.atomic, ax, ay);
                } else if (b.node && b.node->isElement()) {
                    // A picture standing in a block of its own: the block holds its padding
                    // and borders, the picture its size.
                    recordRect(b.node, ax - b.padding[Left] - b.border[Left], ay - b.padding[Top] - b.border[Top],
                               f.atomic->outerWidth() + b.frameWidth(), f.atomic->outerHeight() + b.frameHeight());
                }
                if (f.link) out_->links.push_back({ax, ay, f.atomic->outerWidth(), f.atomic->outerHeight(), *f.link});
                if (f.atomic->kind == BoxKind::Replaced) {
                    paintReplaced(*f.atomic, ax, ay, opacity);
                } else {
                    paintContents(*f.atomic, ax, ay, cb, opacity);
                }
                if (transformed) endTransform();
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

    // Where translate and transform move a box as painted.
    static void shift(const Box& b, float& x, float& y) {
        const ComputedStyle& s = *b.style;
        const float w = b.outerWidth(), h = b.outerHeight();
        x += s.transform_shift[0].resolve(w) + s.translate_shift[0].resolve(w);
        y += s.transform_shift[1].resolve(h) + s.translate_shift[1].resolve(h);
    }

    void record(const Box& b, float x, float y) {
        if (b.node && b.node->isElement() && !b.synthetic) recordRect(b.node, x, y, b.outerWidth(), b.outerHeight());
    }

    // As getBoundingClientRect has it: through the transforms it's inside, the box around that.
    void recordRect(const Node* node, float x, float y, float w, float h) {
        if (transforms_.empty()) {
            out_->boxes.push_back({node, x, y, w, h});
            return;
        }
        const Matrix& m = transforms_.back();
        float l = INFINITY, t = INFINITY, r = -INFINITY, bt = -INFINITY;
        for (const Point p : {Point{x, y}, Point{x + w, y}, Point{x, y + h}, Point{x + w, y + h}}) {
            const Point q = m.apply(p);
            l = std::min(l, q.x);
            t = std::min(t, q.y);
            r = std::max(r, q.x);
            bt = std::max(bt, q.y);
        }
        out_->boxes.push_back({node, l, t, r - l, bt - t});
    }

    void paintMarker(const Box& b, float cx, float cy, float opacity) {
        const ComputedStyle& s = *b.style;
        if (s.display != Display::ListItem || s.hidden) return;
        if (isDisclosure(s.list_style)) return;   // inside, as text
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
        const size_t outer_cb_clip_depth = cb_clip_depth_;
        if (s.position != Position::Static) cb_clip_depth_ = clip_stack_.size();
        if (s.overflow_clip) {
            PaintOp op;
            op.kind = PaintKind::ClipBegin;
            op.x = x + b.border[Left];
            op.y = y + b.border[Top];
            op.w = b.width + b.padding[Left] + b.padding[Right];
            op.h = b.height + b.padding[Top] + b.padding[Bottom];
            clip_stack_.push_back({op.x, op.y, op.w, op.h});
            out_->ops.push_back(std::move(op));
        }
        // A stacking context: its negative z-index descendants go in here, over its own
        // background and under its content.
        const size_t outer_ctx_ops = ctx_ops_, outer_ctx_boxes = ctx_boxes_;
        const bool stacking = &b == root_ || s.position == Position::Fixed || (s.position != Position::Static && s.z_index != 0) ||
                              s.opacity < 1 || s.linearlyTransformed();
        if (stacking && !painting_below_) {
            ctx_ops_ = out_->ops.size();
            ctx_boxes_ = out_->boxes.size();
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
                positioned_.push_back({&k, cx + k.static_x, cy + k.static_y, cb, opacity, order_++, clipsFor(k), ctx_ops_, ctx_boxes_});
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
            if (!clip_stack_.empty()) clip_stack_.pop_back();
        }
        cb_clip_depth_ = outer_cb_clip_depth;
        ctx_ops_ = outer_ctx_ops;
        ctx_boxes_ = outer_ctx_boxes;
    }

    // Absolute and floated boxes inside inline elements still need painting.
    void collectOutOfFlowIn(const Box& b, float cx, float cy, const Rect& cb, float opacity) {
        for (const auto& kp : b.kids) {
            if (kp->outOfFlow()) positioned_.push_back({kp.get(), cx + kp->static_x, cy + kp->static_y, cb, opacity, order_++, clipsFor(*kp), ctx_ops_, ctx_boxes_});
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
        shift(b, x, y);
        const bool transformed = beginTransform(b, x, y);
        record(b, x, y);
        paintContents(b, x, y, cb, opacity);
        if (transformed) endTransform();
    }

    // rotate, scale, then transform's own rotations, scales and skews, about the origin.
    bool beginTransform(const Box& b, float x, float y) {
        const ComputedStyle& s = *b.style;
        if (!s.linearlyTransformed()) return false;
        const float w = b.outerWidth(), h = b.outerHeight();
        const float ox = x + s.transform_origin[0].resolve(w), oy = y + s.transform_origin[1].resolve(h);
        const double r = s.rotate_deg * M_PI / 180.0;
        const Matrix rotate{static_cast<float>(std::cos(r)), static_cast<float>(std::sin(r)), static_cast<float>(-std::sin(r)), static_cast<float>(std::cos(r)), 0, 0};
        const Matrix scale = Matrix::scaling(s.scale_prop[0], s.scale_prop[1]);
        const Matrix own{s.transform_linear[0], s.transform_linear[1], s.transform_linear[2], s.transform_linear[3], 0, 0};
        const Matrix m = Matrix::translate(ox, oy).after(rotate.after(scale.after(own))).after(Matrix::translate(-ox, -oy));
        PaintOp op;
        op.kind = PaintKind::TransformBegin;
        op.x = x;
        op.y = y;
        op.w = w;
        op.h = h;
        op.transform = m;
        out_->ops.push_back(std::move(op));
        transforms_.push_back(transforms_.empty() ? m : transforms_.back().after(m));
        return true;
    }

    void endTransform() {
        if (!transforms_.empty()) transforms_.pop_back();
        PaintOp op;
        op.kind = PaintKind::TransformEnd;
        out_->ops.push_back(std::move(op));
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
                // Below its stacking context's content: painted aside, put in its place after.
                const bool below = s.z_index < 0;
                std::vector<PaintOp> main_ops;
                std::vector<ElementBox> main_boxes;
                const size_t saved_ctx_ops = ctx_ops_, saved_ctx_boxes = ctx_boxes_;
                if (below) {
                    main_ops.swap(out_->ops);
                    main_boxes.swap(out_->boxes);
                    ++painting_below_;
                    ctx_ops_ = p.ctx_ops;
                    ctx_boxes_ = p.ctx_boxes;
                }
                const Rect cb = s.position == Position::Fixed ? Rect{0, 0, vw_, vh_} : p.cb;
                layoutForContainingBlock(k, cb);
                float x = p.static_x + k.margin[Left], y = p.static_y + k.margin[Top];
                if (!s.offset[Left].is_auto) x = cb.x + s.offset[Left].resolve(cb.w) + k.margin[Left];
                else if (!s.offset[Right].is_auto) x = cb.x + cb.w - s.offset[Right].resolve(cb.w) - k.outerWidth() - k.margin[Right];
                if (!s.offset[Top].is_auto) y = cb.y + s.offset[Top].resolve(cb.h) + k.margin[Top];
                else if (!s.offset[Bottom].is_auto) y = cb.y + cb.h - s.offset[Bottom].resolve(cb.h) - k.outerHeight() - k.margin[Bottom];
                shift(k, x, y);
                const bool transformed = beginTransform(k, x, y);
                record(k, x, y);
                // Inside the clips it was inside, as far as its containing block.
                for (const Rect& c : p.clips) {
                    PaintOp op;
                    op.kind = PaintKind::ClipBegin;
                    op.x = c.x;
                    op.y = c.y;
                    op.w = c.w;
                    op.h = c.h;
                    out_->ops.push_back(std::move(op));
                }
                const std::vector<Rect> saved = std::move(clip_stack_);
                clip_stack_ = p.clips;
                const size_t saved_depth = cb_clip_depth_;
                cb_clip_depth_ = clip_stack_.size();
                paintContents(k, x, y, cb, p.opacity);
                clip_stack_ = saved;
                cb_clip_depth_ = saved_depth;
                for (size_t i = 0; i < p.clips.size(); ++i) {
                    PaintOp op;
                    op.kind = PaintKind::ClipEnd;
                    out_->ops.push_back(std::move(op));
                }
                if (transformed) endTransform();
                if (below) {
                    below_content_.push_back({p.ctx_ops, p.ctx_boxes, below_content_.size(), std::move(out_->ops), std::move(out_->boxes)});
                    out_->ops = std::move(main_ops);
                    out_->boxes = std::move(main_boxes);
                    --painting_below_;
                    ctx_ops_ = saved_ctx_ops;
                    ctx_boxes_ = saved_ctx_boxes;
                }
            }
        }
    }

    // Negative z-index paint, into its places: last place first, so earlier places keep
    // their index; at one place, in the order painted (lower z first).
    void placeBelowContent() {
        std::vector<BelowContent*> order;
        for (BelowContent& b : below_content_) order.push_back(&b);
        std::stable_sort(order.begin(), order.end(), [](const BelowContent* a, const BelowContent* b) {
            return a->at_ops != b->at_ops ? a->at_ops > b->at_ops : a->order > b->order;
        });
        for (BelowContent* b : order) {
            const size_t at = std::min(b->at_ops, out_->ops.size());
            out_->ops.insert(out_->ops.begin() + static_cast<ptrdiff_t>(at), std::make_move_iterator(b->ops.begin()), std::make_move_iterator(b->ops.end()));
        }
        std::stable_sort(order.begin(), order.end(), [](const BelowContent* a, const BelowContent* b) {
            return a->at_boxes != b->at_boxes ? a->at_boxes > b->at_boxes : a->order > b->order;
        });
        for (BelowContent* b : order) {
            const size_t at = std::min(b->at_boxes, out_->boxes.size());
            out_->boxes.insert(out_->boxes.begin() + static_cast<ptrdiff_t>(at), b->boxes.begin(), b->boxes.end());
        }
        below_content_.clear();
    }

    struct Positioned {
        const Box* box;
        float static_x, static_y;
        Rect cb;
        float opacity;
        size_t order;
        std::vector<Rect> clips;   // overflow clips between it and its containing block
        size_t ctx_ops = 0, ctx_boxes = 0;   // where its stacking context's content starts
    };
    // A negative z-index box's paint, to go in under its stacking context's content.
    struct BelowContent {
        size_t at_ops, at_boxes, order;
        std::vector<PaintOp> ops;
        std::vector<ElementBox> boxes;
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
    const FormState* forms_ = nullptr;
    FormState no_forms_;   // what's shown when nobody keeps form state: what the page said
    [[nodiscard]] const FormState& forms() const { return forms_ ? *forms_ : no_forms_; }
    float vw_, vh_;
    std::deque<ComputedStyle> anon_styles_;
    std::deque<Node> synthetic_nodes_;   // pseudo-elements' pictures, for this layout only
    PageLayout* out_ = nullptr;
    const Box* skip_background_ = nullptr;
    const Box* root_ = nullptr;
    float max_bottom_ = 0;
    std::vector<Positioned> positioned_;
    std::vector<BelowContent> below_content_;
    size_t ctx_ops_ = 0, ctx_boxes_ = 0;   // the nearest stacking context's content start
    int painting_below_ = 0;               // inside a negative z-index box's paint
    std::vector<Rect> clip_stack_;   // overflow clips around what's being painted
    std::vector<Matrix> transforms_; // the transforms around what's being painted, composed
    size_t cb_clip_depth_ = 0;       // how many of them are outside the containing block

    // An absolutely positioned box is clipped by overflow inside its containing block, not
    // outside it; a fixed one by none.
    std::vector<Rect> clipsFor(const Box& k) const {
        if (k.style->position == Position::Fixed) return {};
        return {clip_stack_.begin() + static_cast<std::ptrdiff_t>(std::min(cb_clip_depth_, clip_stack_.size())), clip_stack_.end()};
    }
    std::vector<PendingFloat> pending_floats_;
    size_t order_ = 0;
    int relayouts_ = 0;
    bool quirks_ = false;
};

}  // namespace

float chWidth(TextMeasurer& text, const ComputedStyle& style) { return text.width("0", fontOf(style)); }

PageLayout layoutPage(const Document& doc, const StyledDocument& styles, TextMeasurer& text, ImageSizes* images,
                      float viewport_width, float viewport_height, const FormState* forms) {
    return Layouter(styles, text, images, viewport_width, viewport_height, forms).run(doc);
}

}  // namespace fbx::xweb
