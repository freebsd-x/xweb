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
};

enum class PaintKind { Rect, Text, Image };

struct PaintOp {
    PaintKind kind = PaintKind::Rect;
    float x = 0, y = 0, w = 0, h = 0;
    Color color;
    std::string text;      // Text: the run. Image: its source.
    FontSpec font;
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
};

// `images` may be null: an image then takes its size from attributes and style only.
[[nodiscard]] PageLayout layoutPage(const Document& doc, const StyledDocument& styles, TextMeasurer& text,
                                    ImageSizes* images, float viewport_width, float viewport_height);

}  // namespace fbx::xweb
