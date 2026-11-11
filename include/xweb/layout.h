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
};

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
};

// A StyledDocument::ChMeasure that asks `text`.
[[nodiscard]] float chWidth(TextMeasurer& text, const ComputedStyle& style);

// `images` may be null: an image then takes its size from attributes and style only.
// `forms`: what the page's controls hold now (null: what the page said).
[[nodiscard]] PageLayout layoutPage(const Document& doc, const StyledDocument& styles, TextMeasurer& text,
                                    ImageSizes* images, float viewport_width, float viewport_height,
                                    const class FormState* forms = nullptr);

}  // namespace fbx::xweb
