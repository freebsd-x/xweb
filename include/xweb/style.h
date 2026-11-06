#pragma once

// Selector matching, the cascade and computed values.

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xweb/css.h"
#include "xweb/dom.h"

namespace fbx::xweb {

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    [[nodiscard]] bool visible() const noexcept { return a != 0; }
    bool operator==(const Color&) const = default;
};

// A length as computed: pixels plus a share of what percentages resolve against
// (so calc(100% - 20px) needs nothing more), or auto.
struct Length {
    bool is_auto = false;
    float px = 0;
    float percent = 0;
    [[nodiscard]] float resolve(float base) const noexcept { return px + percent * base / 100.0f; }
    [[nodiscard]] bool hasPercent() const noexcept { return percent != 0; }
    static Length autoLength() noexcept { Length l; l.is_auto = true; return l; }
    static Length pixels(float v) noexcept { Length l; l.px = v; return l; }
};

enum class Display {
    None, Inline, Block, InlineBlock, ListItem, FlowRoot,
    Table, InlineTable, TableCaption, TableRowGroup, TableRow, TableCell, TableColumn,
    Flex, InlineFlex,
};
enum class Float { None, Left, Right };
enum class Clear { None, Left, Right, Both };
enum class VerticalAlign { Baseline, Top, Middle, Bottom, Sub, Super };
enum class FlexAlign { Auto, Start, End, Center, Stretch, Baseline, SpaceBetween, SpaceAround, SpaceEvenly };
enum class Position { Static, Relative, Absolute, Fixed };
enum class TextAlign { Left, Right, Center };
enum class WhiteSpace { Normal, Pre, NoWrap, PreWrap, PreLine };
enum class TextTransform { None, Uppercase, Lowercase, Capitalize };
enum class ListStyle { None, Disc, Circle, Square, Decimal, LowerAlpha, UpperAlpha, LowerRoman, UpperRoman };
enum Side { Top = 0, Right = 1, Bottom = 2, Left = 3 };

struct ComputedStyle {
    Display display = Display::Inline;
    Position position = Position::Static;
    bool hidden = false;   // visibility: hidden
    bool border_box = false;
    float opacity = 1;

    // Inherited
    Color color{0, 0, 0, 255};
    float font_size = 16;
    float font_keyword = 1;            // share of "medium" if the size came from a keyword, else 0
    int font_weight = 400;
    bool italic = false;
    std::string font_family = "sans-serif"; // as listed, comma separated
    float line_height = 0;             // px; 0 with a zero factor means "normal"
    float line_height_factor = 0;      // a unitless line-height inherits as the number
    TextAlign text_align = TextAlign::Left;
    TextTransform text_transform = TextTransform::None;
    WhiteSpace white_space = WhiteSpace::Normal;
    ListStyle list_style = ListStyle::Disc;
    Length text_indent;
    std::shared_ptr<const std::map<std::string, std::vector<css::Token>>> custom;
    float border_spacing = 0;
    bool border_collapse = false;

    // Not inherited, though underlines reach descendants' text
    Color background{0, 0, 0, 0};
    Length margin[4], padding[4], offset[4] = {Length::autoLength(), Length::autoLength(), Length::autoLength(), Length::autoLength()};
    float border_width[4] = {0, 0, 0, 0};
    Color border_color[4];
    Length width = Length::autoLength(), height = Length::autoLength();
    Length min_width, min_height;
    Length max_width = Length::autoLength(), max_height = Length::autoLength();
    int z_index = 0;
    bool ua_margin_top = true, ua_margin_bottom = true;   // not set by the page
    Float float_side = Float::None;
    Clear clear = Clear::None;
    bool overflow_clip = false;        // overflow other than visible; we don't draw scrollbars
    float border_radius = 0;
    VerticalAlign vertical_align = VerticalAlign::Baseline;
    bool table_fixed = false;
    // Flexbox
    bool flex_column = false, flex_reverse = false, flex_wrap = false;
    FlexAlign justify_content = FlexAlign::Start, align_items = FlexAlign::Stretch;
    FlexAlign align_self = FlexAlign::Auto, align_content = FlexAlign::Stretch;
    float flex_grow = 0, flex_shrink = 1;
    Length flex_basis = Length::autoLength();
    int order = 0;
    float row_gap = 0, column_gap = 0;
    // ::before and ::after
    bool has_content = false;
    std::string content;
    bool underline = false, line_through = false;   // includes those of ancestors

    [[nodiscard]] float usedLineHeight() const noexcept {
        if (line_height_factor > 0) return line_height_factor * font_size;
        return line_height > 0 ? line_height : font_size * 1.2f;
    }
};

// Limits what one page can make us do. Past them things stop matching or nesting.
inline constexpr size_t kMaxStyledDepth = 1024;

class StyledDocument {
public:
    // A stylesheet's text for a <link rel=stylesheet> or @import, or empty.
    using SheetLoader = std::function<std::string(const std::string& href)>;

    // The width of "0" in a style's font: what the ch unit means. Style knows nothing of
    // fonts, so someone has to say; without an answer it's taken as half an em.
    using ChMeasure = std::function<float(const ComputedStyle&)>;

    StyledDocument(const Document& doc, css::MediaContext media, const SheetLoader& loader = {}, const ChMeasure& measure_ch = {});

    // Nullptr for anything that has no box-generating style (or is too deep).
    [[nodiscard]] const ComputedStyle* styleOf(const Node* element) const;
    // The element's ::before or ::after, if it has one with content.
    [[nodiscard]] const ComputedStyle* beforeOf(const Node* element) const;
    [[nodiscard]] const ComputedStyle* afterOf(const Node* element) const;
    [[nodiscard]] const css::MediaContext& media() const noexcept { return media_; }

private:
    css::MediaContext media_;
    ChMeasure measure_ch_;
    std::unordered_map<const Node*, const ComputedStyle*> styles_;
    std::unordered_map<const Node*, const ComputedStyle*> before_, after_;
    std::deque<ComputedStyle> storage_;
    friend class StyleResolver;
};

[[nodiscard]] bool parseColor(const std::vector<css::Token>& value, const Color& current, Color& out);

}  // namespace fbx::xweb
