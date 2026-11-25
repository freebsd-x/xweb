#pragma once

// Boxes: block and inline layout (CSS 2.1 chapters 9-10) over a styled document,
// with no idea how anything gets drawn. It asks TextMeasurer about text and emits
// paint operations.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xweb/dom.h"
#include "xweb/paint.h"
#include "xweb/style.h"

namespace fbx::xweb {

struct FontSpec {
    float size = 16;
    int weight = 400;
    bool italic = false;
    std::string family;
    float letter_spacing = 0;   // after each letter; `width` has it
};

// The text cut where letter-spacing goes: after each letter, with the marks on it. Nothing
// when it can't be put in (letters that join).
[[nodiscard]] std::vector<std::string_view> spacedLetters(std::string_view utf8);

class TextMeasurer {
public:
    virtual ~TextMeasurer() = default;
    [[nodiscard]] virtual float width(std::string_view utf8, const FontSpec& font) = 0;
    [[nodiscard]] virtual float ascent(const FontSpec& font) = 0;
    [[nodiscard]] virtual float descent(const FontSpec& font) = 0;
    // The font's own extra space between lines, part of line-height: normal.
    [[nodiscard]] virtual float lineGap(const FontSpec& font) { (void)font; return 0; }
};

class ImageSizes {
public:
    virtual ~ImageSizes() = default;
    // The image's natural size, if it could be loaded.
    [[nodiscard]] virtual std::optional<std::pair<float, float>> imageSize(const std::string& src) = 0;
    // For a loaded image with a shape but no size (an SVG with only a viewBox): width over height.
    [[nodiscard]] virtual std::optional<float> imageRatio(const std::string& src) { (void)src; return std::nullopt; }
    // Whether it may still come: then it's an empty box, as browsers show an image loading,
    // not its alt text (that's for one that failed, or a host that loads none).
    [[nodiscard]] virtual bool imageLoading(const std::string& src) { (void)src; return false; }
};

enum class PaintKind {
    Rect, Text, Image, ClipBegin, ClipEnd,
    Gradient,   // `gradient` filling the rectangle
    Svg,        // the inline <svg> `node`, drawn into the rectangle; currentColor is `color`
    Box,        // `box` (uneven corners, fancy borders, a shadow) drawn into the rectangle
    // What's painted until the matching TransformEnd goes through `transform` (page
    // coordinates to page coordinates): rotated, scaled or skewed.
    TransformBegin, TransformEnd,
};

// An Image op's source when it's a <canvas> (op.node): its pixels are the page script's.
inline constexpr const char* kCanvasSource = "xweb:canvas";
// An Image op that is an <iframe>'s area: whoever draws the page puts the frame's own
// page there (op.node is the iframe).
inline constexpr const char* kFrameSource = "xweb:frame";

struct PaintOp {
    PaintKind kind = PaintKind::Rect;
    float x = 0, y = 0, w = 0, h = 0;
    Color color;
    float radius = 0;      // Rect: rounded corners. Image: its corners' horizontal radius
    float radius_y = 0;    // Image: their vertical radius
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;   // Image: the part of the picture shown (object-fit cover crops)
    std::shared_ptr<const std::vector<Filter>> filter;   // Image: its CSS filter
    std::string text;      // Text: the run. Image: its source.
    FontSpec font;
    bool tint = false;     // Image: only its alpha, in `color` (a mask over a background colour)
    std::shared_ptr<const Gradient> gradient;
    const Node* node = nullptr;
    std::shared_ptr<const BoxPaint> box;
    Matrix transform;      // TransformBegin
    float opacity = 1;     // Svg, Box: all of it. (An Image's is its colour's alpha, but a tint's.)
};

struct LinkArea {
    float x = 0, y = 0, w = 0, h = 0;
    std::string href;
};

// An element's border box on the page.
struct ElementBox {
    const Node* node = nullptr;
    float x = 0, y = 0, w = 0, h = 0;
};

struct PageLayout {
    float width = 0, height = 0;
    Color canvas{255, 255, 255, 255};
    std::vector<PaintOp> ops;
    std::vector<LinkArea> links;
    std::vector<ElementBox> boxes;
    std::string title;
    std::vector<std::string> images_wanted;   // CSS images it would have drawn, not loaded yet
    struct LazyImage {
        std::string src;
        float y = 0, h = 0;
    };
    std::vector<LazyImage> lazy_images;       // <img loading=lazy>s not loaded yet: wanted when they come near
};

// A StyledDocument::ChMeasure that asks `text`.
[[nodiscard]] float chWidth(TextMeasurer& text, const ComputedStyle& style);

// `images` may be null: an image then takes its size from attributes and style only.
// `forms`: what the page's controls hold now (null: what the page said).
// A page's last layout, for the next to keep what's unchanged of: the boxes of what has the
// same content and styles, and their sizes where they're asked for the same. reset() when
// what isn't in those changes (fonts).
class LayoutState {
public:
    LayoutState();
    ~LayoutState();
    LayoutState(const LayoutState&) = delete;
    LayoutState& operator=(const LayoutState&) = delete;
    void reset();
    // Of the last layout: boxes made and kept, and how many times their contents were
    // laid out, or were as they had been.
    size_t made = 0, kept = 0, laid = 0, skipped = 0;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

[[nodiscard]] PageLayout layoutPage(const Document& doc, const StyledDocument& styles, TextMeasurer& text,
                                    ImageSizes* images, float viewport_width, float viewport_height,
                                    const class FormState* forms = nullptr, LayoutState* state = nullptr);

}  // namespace fbx::xweb
