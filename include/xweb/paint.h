#pragma once

// Drawing what layout can't hand the compositor as rectangles and text: SVG, CSS
// gradients and masks, rasterized here on the CPU into RGBA images. Written by hand,
// like the parsers; nothing here touches the GPU, so it runs in the sandbox and in tests.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xweb/dom.h"
#include "xweb/style.h"

namespace fbx::xweb {

// Straight (not premultiplied) RGBA, rows top to bottom.
struct Raster {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
};

// What one raster may cost; bigger ones are drawn smaller and stretched.
inline constexpr uint32_t kMaxRasterSide = 4096;
inline constexpr uint64_t kMaxRasterPixels = 4'000'000;

struct Point {
    float x = 0, y = 0;
};

// x' = a x + c y + e, y' = b x + d y + f, as SVG writes it.
struct Matrix {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    [[nodiscard]] Point apply(Point p) const noexcept { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    // This after `first`.
    [[nodiscard]] Matrix after(const Matrix& first) const noexcept;
    [[nodiscard]] std::optional<Matrix> inverse() const noexcept;
    // How much it scales lengths, about.
    [[nodiscard]] float scale() const noexcept;
    static Matrix translate(float x, float y) noexcept { return {1, 0, 0, 1, x, y}; }
    static Matrix scaling(float x, float y) noexcept { return {x, 0, 0, y, 0, 0}; }
};

struct ColorStop {
    float offset = 0;   // 0 to 1, in order
    Color color;
};

// A gradient in its own coordinates, and how they map onto the canvas.
struct GradientPaint {
    enum class Kind { Linear, Radial, Conic } kind = Kind::Linear;
    enum class Spread { Pad, Repeat, Reflect } spread = Spread::Pad;
    float x1 = 0, y1 = 0, x2 = 1, y2 = 0;   // linear: from and to
    float cx = 0, cy = 0, r = 1, fx = 0, fy = 0;   // radial: end circle, and the focus
    float start_angle = 0;   // conic, radians clockwise from up, about (cx, cy)
    std::vector<ColorStop> stops;
    float period_start = 0, period_end = 1;   // what Repeat and Reflect repeat
    Matrix to_canvas;
    // The colour at a point in gradient coordinates.
    [[nodiscard]] Color at(Point p) const noexcept;
};

struct Paint {
    Color color;
    std::shared_ptr<const GradientPaint> gradient;   // instead of the colour
    float opacity = 1;
};

using Contours = std::vector<std::vector<Point>>;

// An RGBA canvas that paths and images are composited onto, source over, with a clip.
class Canvas {
public:
    Canvas(uint32_t width, uint32_t height);
    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }

    // Polygons in canvas pixels, filled antialiased by the non-zero or even-odd rule.
    void fill(const Contours& contours, bool even_odd, const Paint& paint);
    void fillRect(float x, float y, float w, float h, Color color, float radius = 0);
    // Stretched over the rectangle, bilinear; `tint` (if any) replaces its colour, keeping alpha.
    void drawRaster(const Raster& image, float x, float y, float w, float h, float opacity = 1, const Color* tint = nullptr);
    // Another canvas the same size on top of this, faded, and through a coverage mask (if any).
    void composite(const Canvas& layer, float opacity, const Canvas* mask = nullptr);
    // Coverage of these polygons, as a canvas whose alpha is it: for clip paths.
    static Canvas coverage(uint32_t width, uint32_t height, const Contours& contours, bool even_odd);

    void setClip(float x, float y, float w, float h);
    void resetClip();
    [[nodiscard]] Raster toRaster() const;
    // Premultiplied RGBA, 0..1, row by row.
    [[nodiscard]] const std::vector<float>& pixels() const noexcept { return px_; }

private:
    void blendSpan(uint32_t y, uint32_t x0, uint32_t x1, const float* cover, const Paint& paint);
    uint32_t width_, height_;
    std::vector<float> px_;
    int clip_x0_, clip_y0_, clip_x1_, clip_y1_;
};

// Shapes, as polygons.
[[nodiscard]] Contours roundedRect(float x, float y, float w, float h, float rx, float ry);
[[nodiscard]] Contours ellipse(float cx, float cy, float rx, float ry, float scale);
// A polyline's stroke, as polygons to fill non-zero. `closed` joins its end to its start.
struct StrokeStyle {
    float width = 1;
    uint8_t cap = 0;    // butt, round, square
    uint8_t join = 0;   // miter, round, bevel
    float miter_limit = 4;
};
[[nodiscard]] Contours strokeOutline(const std::vector<Point>& line, bool closed, const StrokeStyle& style, float scale);
// SVG path data, flattened. Each contour says whether it was closed.
[[nodiscard]] std::vector<std::pair<std::vector<Point>, bool>> parsePathData(const std::string& d, float scale);
// A transform attribute's value.
[[nodiscard]] Matrix parseTransform(const std::string& text);

// A CSS gradient filling a box this size, at this many pixels.
[[nodiscard]] Raster renderGradient(const Gradient& gradient, float box_w, float box_h, uint32_t px_w, uint32_t px_h);

// An SVG's natural size, as its attributes give it: width and height, or the ratio of a
// viewBox, or neither.
struct SvgSize {
    std::optional<float> width, height;
    float ratio = 0;   // width over height, 0 if unknown
};
[[nodiscard]] SvgSize svgNaturalSize(const Node& svg);

// An <svg> element and what's in it, drawn at this size. `styles` is how the cascade left
// its elements (null: attributes only); `color` is what currentColor is, at the top.
// `natural`: drawn as an image, an SVG without a viewBox is stretched from its own size,
// as a bitmap would be.
[[nodiscard]] Raster renderSvg(const Node& svg, uint32_t width, uint32_t height, const StyledDocument* styles, Color color,
                               const SvgSize* natural = nullptr);

// The first <svg> in a parsed SVG file.
[[nodiscard]] const Node* findSvgRoot(const Document& doc);

// An SVG file, parsed and styled once, drawn at whatever sizes it's asked for.
class SvgImage {
public:
    // Null if it holds no <svg>.
    [[nodiscard]] static std::shared_ptr<const SvgImage> parse(std::string_view text);
    [[nodiscard]] const SvgSize& size() const noexcept { return size_; }
    [[nodiscard]] Raster render(uint32_t width, uint32_t height) const;
    // Whether `bytes` look like SVG rather than a bitmap format.
    [[nodiscard]] static bool sniff(std::string_view bytes, std::string_view content_type, std::string_view url);

private:
    std::unique_ptr<Document> doc_;
    std::unique_ptr<StyledDocument> styles_;
    const Node* root_ = nullptr;
    SvgSize size_;
};

}  // namespace fbx::xweb
