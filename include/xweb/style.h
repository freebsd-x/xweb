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
#include <utility>
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

// One function of a filter: list (filter: blur(4px) grayscale(1) ...).
struct Filter {
    enum class Kind : uint8_t { Blur, Brightness, Contrast, Grayscale, Invert, Opacity, Saturate, Sepia, HueRotate } kind;
    float amount = 0;   // px for blur, degrees for hue-rotate, otherwise a fraction (1 = 100%)
};

enum class Display {
    None, Inline, Block, InlineBlock, ListItem, FlowRoot,
    Table, InlineTable, TableCaption, TableRowGroup, TableRow, TableCell, TableColumn,
    Flex, InlineFlex, Grid, InlineGrid,
    Contents,   // no box of its own: its children go where it would have
};
enum class Float { None, Left, Right };
enum class Clear { None, Left, Right, Both };
enum class VerticalAlign { Baseline, Top, Middle, Bottom, Sub, Super, TextTop, TextBottom };
enum class FlexAlign { Auto, Start, End, Center, Stretch, Baseline, SpaceBetween, SpaceAround, SpaceEvenly };
enum class Position { Static, Relative, Absolute, Fixed };
enum class TextAlign { Left, Right, Center };
enum class WhiteSpace { Normal, Pre, NoWrap, PreWrap, PreLine };
enum class TextTransform { None, Uppercase, Lowercase, Capitalize };
enum class ListStyle { None, Disc, Circle, Square, Decimal, LowerAlpha, UpperAlpha, LowerRoman, UpperRoman, DisclosureClosed, DisclosureOpen };
enum Side { Top = 0, Right = 1, Bottom = 2, Left = 3 };

// Grid tracks, as specified: both ends of a track's minmax().
struct GridSize {
    enum Kind { Auto, MinContent, MaxContent, Fixed, Fr } kind = Auto;
    Length length;   // Fixed
    float fr = 0;    // Fr
};

struct GridTrack {
    GridSize min, max;
    bool fit_content = false;
    Length fit_limit;
};

// A template's tracks, with the one repeat(auto-fill | auto-fit) it may hold still folded
// up: how many times it runs depends on the room there turns out to be.
struct GridTrackList {
    std::vector<GridTrack> tracks;
    std::vector<std::vector<std::string>> names;   // of the lines: tracks.size() + 1
    std::vector<GridTrack> repeat;
    std::vector<std::vector<std::string>> repeat_names;   // repeat.size() + 1
    std::vector<std::string> names_after_repeat;
    size_t repeat_at = 0;
    bool auto_fit = false;
};

struct GridAreaRect { int row_start = 0, row_end = 0, column_start = 0, column_end = 0; };

struct GridAreas {
    std::map<std::string, GridAreaRect> areas;
    int rows = 0, columns = 0;
};

// One end of a placement: `auto`, a line by number and/or name, or a span.
struct GridLine {
    int line = 0;       // 0 is none; negative counts from the end
    int span = 0;       // nonzero for "span n"
    std::string name;
    [[nodiscard]] bool isAuto() const noexcept { return line == 0 && span == 0 && name.empty(); }
};

enum class BorderStyle : uint8_t { None, Hidden, Solid, Dashed, Dotted, Double, Groove, Ridge, Inset, Outset };

// One box-shadow.
struct BoxShadow {
    float x = 0, y = 0, blur = 0, spread = 0;
    Color color{0, 0, 0, 255};
    bool inset = false;
};

// A CSS gradient as specified: what it comes to depends on the box it fills.
struct GradientStop {
    Color color;
    bool has_position = false;
    Length position;   // along the gradient line; percentages of its length
};

struct Gradient {
    enum class Kind { Linear, Radial, Conic } kind = Kind::Linear;
    bool repeating = false;
    // Linear: an angle (0 is up, clockwise), or toward a side or corner.
    float angle = 180;
    int to_x = 0, to_y = 0;   // -1, 0, 1: "to left top" is (-1, -1); both 0 means the angle
    // Radial and conic: the centre, and radial's size.
    Length center_x = Length{false, 0, 50}, center_y = Length{false, 0, 50};
    bool circle = false;
    enum class Extent { ClosestSide, ClosestCorner, FarthestSide, FarthestCorner } extent = Extent::FarthestCorner;
    bool explicit_size = false;
    Length radius_x, radius_y;
    float from = 0;   // conic: where it starts, degrees
    std::vector<GradientStop> stops;
};

// One layer's image: a url(), a gradient, or nothing (none).
struct BackgroundImage {
    std::string url;   // absolute if the sheet's address was
    std::shared_ptr<const Gradient> gradient;
    [[nodiscard]] bool none() const noexcept { return url.empty() && !gradient; }
};

enum class BackgroundBox { Border, Padding, Content };

struct BackgroundSize {
    enum class Kind { Explicit, Cover, Contain } kind = Kind::Explicit;
    Length width = Length::autoLength(), height = Length::autoLength();
};

// The background-* (or mask-*) longhands, each a list; layer i takes entry i of each,
// the shorter lists repeating. The first layer is on top.
struct BackgroundLayers {
    std::vector<BackgroundImage> images;
    std::vector<BackgroundSize> sizes{BackgroundSize{}};
    std::vector<std::pair<Length, Length>> positions{{Length{}, Length{}}};
    std::vector<std::pair<bool, bool>> repeats{{true, true}};   // x, y
    std::vector<BackgroundBox> origins{BackgroundBox::Padding};
    std::vector<BackgroundBox> clips{BackgroundBox::Border};
    [[nodiscard]] bool empty() const noexcept {
        for (const BackgroundImage& i : images)
            if (!i.none()) return false;
        return true;
    }
};

// SVG's paint: fill and stroke.
struct SvgPaint {
    enum class Kind { None, Color, CurrentColor, Url } kind = Kind::None;
    Color color;
    std::string url;          // a fragment, "#id"
    Kind fallback = Kind::None;   // url(#x) red: if #x isn't a paint server
    Color fallback_color;
};

// Which SVG properties an element sets itself, rather than inheriting: in a <use>'s copy
// of a symbol, what's inherited comes from the <use>, not the symbol's own parents.
enum SvgSetBits : uint32_t {
    kSetFill = 1, kSetStroke = 2, kSetStrokeWidth = 4, kSetFillOpacity = 8, kSetStrokeOpacity = 16,
    kSetFillRule = 32, kSetLinecap = 64, kSetLinejoin = 128, kSetDash = 256, kSetColor = 512, kSetMiter = 1024,
    kSetDashOffset = 2048, kSetVisibility = 4096,
};

struct ComputedStyle {
    Display display = Display::Inline;
    Position position = Position::Static;
    bool hidden = false;   // visibility: hidden
    bool border_box = false;
    float opacity = 1;
    // A picture's fit in its box: fill, contain, cover, none, scale-down; and where.
    enum class Fit : uint8_t { Fill, Contain, Cover, None, ScaleDown } object_fit = Fit::Fill;
    Length object_x{false, 0, 50}, object_y{false, 0, 50};

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
    // text-align: -webkit-center / -webkit-right (as <center> and align= give it): block
    // children are aligned too. 0 none, 1 centre, 2 right. Inherited; `parent_block_align`
    // is the parent's, which is what places this box.
    uint8_t block_align = 0, parent_block_align = 0;
    TextTransform text_transform = TextTransform::None;
    WhiteSpace white_space = WhiteSpace::Normal;
    ListStyle list_style = ListStyle::Disc;
    Length text_indent;
    std::shared_ptr<const std::map<std::string, std::vector<css::Token>>> custom;
    float border_spacing = 0;
    bool border_collapse = false;
    // SVG, inherited
    SvgPaint fill{SvgPaint::Kind::Color, Color{0, 0, 0, 255}};
    SvgPaint stroke;
    float stroke_width = 1, stroke_miterlimit = 4, stroke_dashoffset = 0;
    float fill_opacity = 1, stroke_opacity = 1;
    bool fill_evenodd = false;
    uint8_t stroke_linecap = 0;    // butt, round, square
    uint8_t stroke_linejoin = 0;   // miter, round, bevel
    std::shared_ptr<const std::vector<float>> stroke_dasharray;

    // Not inherited, though underlines reach descendants' text
    Color background{0, 0, 0, 0};
    std::shared_ptr<const BackgroundLayers> background_layers;   // null: no images
    std::shared_ptr<const BackgroundLayers> mask_layers;
    bool masked = false;   // shaped by a mask we can't draw
    // SVG, not inherited
    uint32_t svg_set = 0;   // SvgSetBits
    Color stop_color{0, 0, 0, 255};
    float stop_opacity = 1;
    std::string clip_path;   // "#id"
    Length margin[4], padding[4], offset[4] = {Length::autoLength(), Length::autoLength(), Length::autoLength(), Length::autoLength()};
    float border_width[4] = {0, 0, 0, 0};
    Color border_color[4];
    enum class WidthKeyword { None, MinContent, MaxContent, FitContent } width_keyword = WidthKeyword::None;   // width is auto then
    Length width = Length::autoLength(), height = Length::autoLength();
    // auto (the initial value) resolves to 0, but a flex item's is its content.
    Length min_width = Length::autoLength(), min_height = Length::autoLength();
    Length max_width = Length::autoLength(), max_height = Length::autoLength();
    int z_index = 0;
    // How far transform's translate functions, and the translate property, move it as
    // painted; percentages are of its own border box.
    Length transform_shift[2];
    Length translate_shift[2];
    // The rest of transform (rotate, scale, skew, matrix) as a 2x2 matrix a b c d, as SVG
    // orders them; and the rotate and scale properties. About transform-origin.
    float transform_linear[4] = {1, 0, 0, 1};
    float rotate_deg = 0;
    float scale_prop[2] = {1, 1};
    Length transform_origin[2] = {Length{false, 0, 50}, Length{false, 0, 50}};
    [[nodiscard]] bool linearlyTransformed() const noexcept {
        return transform_linear[0] != 1 || transform_linear[1] != 0 || transform_linear[2] != 0 || transform_linear[3] != 1 ||
               rotate_deg != 0 || scale_prop[0] != 1 || scale_prop[1] != 1;
    }
    bool ua_margin_top = true, ua_margin_bottom = true;   // not set by the page
    bool native_look = true;   // a form control the page hasn't given a border, background or appearance: drawn as Chromium's
    Float float_side = Float::None;
    Clear clear = Clear::None;
    bool overflow_clip = false;        // overflow other than visible; we don't draw scrollbars
    bool text_overflow_ellipsis = false;
    // Absolutely positioned, and inline-level before that made it a block: its static
    // position is where it would have been on its line, not the start of the next one.
    bool inline_level_origin = false;   // text-overflow: ellipsis (with overflow_clip, on a line too long)
    Length radius[4][2];   // corners top-left, top-right, bottom-right, bottom-left; horizontal and vertical
    BorderStyle border_style[4] = {BorderStyle::None, BorderStyle::None, BorderStyle::None, BorderStyle::None};
    std::shared_ptr<const std::vector<BoxShadow>> box_shadow;   // first on top
    std::shared_ptr<const std::vector<Filter>> filter;          // in order; null for none
    VerticalAlign vertical_align = VerticalAlign::Baseline;
    bool table_fixed = false;
    // Flexbox
    bool flex_column = false, flex_reverse = false, flex_wrap = false;
    FlexAlign justify_content = FlexAlign::Auto, align_items = FlexAlign::Stretch;
    FlexAlign align_self = FlexAlign::Auto, align_content = FlexAlign::Stretch;
    float flex_grow = 0, flex_shrink = 1;
    Length flex_basis = Length::autoLength();
    int order = 0;
    float row_gap = 0, column_gap = 0;
    // Grid
    std::shared_ptr<const GridTrackList> grid_columns, grid_rows;
    std::shared_ptr<const std::vector<GridTrack>> grid_auto_columns, grid_auto_rows;
    std::shared_ptr<const GridAreas> grid_areas;
    bool grid_flow_column = false, grid_dense = false;
    GridLine grid_row_start, grid_row_end, grid_column_start, grid_column_end;
    FlexAlign justify_items = FlexAlign::Auto, justify_self = FlexAlign::Auto;
    // ::before and ::after
    bool has_content = false;
    std::string content;
    std::string content_image;   // content: url(...): the image a ::before or ::after shows
    bool underline = false, line_through = false;   // includes those of ancestors

    [[nodiscard]] float usedLineHeight() const noexcept {
        if (line_height_factor > 0) return line_height_factor * font_size;
        return line_height > 0 ? line_height : font_size * 1.2f;
    }
};

// An @font-face, in the terms choosing a font needs.
struct WebFontFace {
    std::string family;           // lower case
    int weight_min = 400, weight_max = 400;
    bool italic = false;
    std::vector<std::string> sources;   // in order of preference, of formats we can read
    std::vector<std::pair<uint32_t, uint32_t>> ranges;   // empty is everything
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

    // `forms`: what the page's controls hold now, for :checked (null: what the page said).
    StyledDocument(const Document& doc, css::MediaContext media, const SheetLoader& loader = {}, const ChMeasure& measure_ch = {},
                   const class FormState* forms = nullptr);

    // Nullptr for anything that has no box-generating style (or is too deep).
    [[nodiscard]] const ComputedStyle* styleOf(const Node* element) const;
    // The element's ::before or ::after, if it has one with content.
    [[nodiscard]] const ComputedStyle* beforeOf(const Node* element) const;
    [[nodiscard]] const ComputedStyle* afterOf(const Node* element) const;
    // An <input> or <textarea>'s ::placeholder, if a rule styles it.
    [[nodiscard]] const ComputedStyle* placeholderOf(const Node* element) const;
    [[nodiscard]] const css::MediaContext& media() const noexcept { return media_; }
    // The page's @font-face rules, those applying. Sources are as absolute as the sheet's
    // address was: those in a <style> are still relative to the page.
    [[nodiscard]] const std::vector<WebFontFace>& fontFaces() const noexcept { return font_faces_; }

private:
    css::MediaContext media_;
    ChMeasure measure_ch_;
    const class FormState* forms_ = nullptr;
    std::unordered_map<const Node*, const ComputedStyle*> styles_;
    std::unordered_map<const Node*, const ComputedStyle*> before_, after_, placeholder_;
    std::deque<ComputedStyle> storage_;
    std::vector<WebFontFace> font_faces_;
    friend class StyleResolver;
};

[[nodiscard]] bool parseColor(const std::vector<css::Token>& value, const Color& current, Color& out);

}  // namespace fbx::xweb
