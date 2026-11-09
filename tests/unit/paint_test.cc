// The CPU rasterizer, CSS gradients, SVG, and how style and layout hand them over.
// Against a real browser, see scripts/xweb-pixels.py.

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <string>

#include "xweb/html_parser.h"
#include "xweb/layout.h"
#include "xweb/paint.h"
#include "xweb/style.h"

using namespace fbx::xweb;

namespace {

struct Px {
    int r, g, b, a;
};

Px at(const Raster& r, uint32_t x, uint32_t y) {
    const uint8_t* p = r.rgba.data() + (static_cast<size_t>(y) * r.width + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

class FixedText : public TextMeasurer {
public:
    float width(std::string_view s, const FontSpec& f) override { return static_cast<float>(s.size()) * f.size / 2; }
    float ascent(const FontSpec& f) override { return f.size * 0.75f; }
    float descent(const FontSpec& f) override { return f.size * 0.25f; }
};

class Sizes : public ImageSizes {
public:
    std::optional<std::pair<float, float>> imageSize(const std::string& src) override {
        if (src == "a.png") return std::make_pair(10.0f, 20.0f);
        return std::nullopt;
    }
    std::optional<float> imageRatio(const std::string& src) override {
        if (src == "shape.svg") return 2.0f;
        return std::nullopt;
    }
};

const ComputedStyle* styleById(const Document& doc, const StyledDocument& styles, const std::string& id) {
    std::vector<const Node*> todo{doc.root()};
    while (!todo.empty()) {
        const Node* n = todo.back();
        todo.pop_back();
        if (const std::string* v = n->isElement() ? n->attr("id") : nullptr; v && *v == id) return styles.styleOf(n);
        todo.insert(todo.end(), n->children.begin(), n->children.end());
    }
    return nullptr;
}

Raster svgOf(const std::string& markup, uint32_t w, uint32_t h) {
    auto img = SvgImage::parse(markup);
    if (!img) return {};
    return img->render(w, h);
}

}  // namespace

TEST(Raster, FillsASquareExactly) {
    Canvas c(10, 10);
    Paint p;
    p.color = {255, 0, 0, 255};
    c.fill({{{2, 2}, {8, 2}, {8, 8}, {2, 8}}}, false, p);
    const Raster r = c.toRaster();
    EXPECT_EQ(at(r, 5, 5).a, 255);
    EXPECT_EQ(at(r, 2, 2).r, 255);
    EXPECT_EQ(at(r, 1, 5).a, 0);
    EXPECT_EQ(at(r, 8, 5).a, 0);
}

TEST(Raster, HalfCoveredPixelsAreHalf) {
    Canvas c(4, 4);
    Paint p;
    p.color = {0, 0, 0, 255};
    c.fill({{{0, 0}, {1.5f, 0}, {1.5f, 4}, {0, 4}}}, false, p);
    const Raster r = c.toRaster();
    EXPECT_NEAR(at(r, 1, 1).a, 128, 3);
    EXPECT_EQ(at(r, 0, 1).a, 255);
}

TEST(Raster, EvenOddLeavesTheHole) {
    Canvas c(20, 20);
    Paint p;
    p.color = {0, 0, 255, 255};
    const Contours two = {{{0, 0}, {20, 0}, {20, 20}, {0, 20}}, {{5, 5}, {15, 5}, {15, 15}, {5, 15}}};
    c.fill(two, true, p);
    EXPECT_EQ(at(c.toRaster(), 10, 10).a, 0);
    Canvas d(20, 20);
    d.fill(two, false, p);
    EXPECT_EQ(at(d.toRaster(), 10, 10).a, 255);   // same direction: non-zero fills it
}

TEST(Raster, CircleAreaComesOutRight) {
    Canvas c(40, 40);
    Paint p;
    p.color = {0, 0, 0, 255};
    c.fill(ellipse(20, 20, 3, 3, 1), false, p);
    double area = 0;
    const auto& px = c.pixels();
    for (size_t i = 3; i < px.size(); i += 4) area += px[i];
    EXPECT_NEAR(area, 3.14159 * 9, 0.4);
}

TEST(Raster, PathDataCommands) {
    // Relative, implicit repeats, compact numbers, arcs and closes.
    const auto paths = parsePathData("m10 10h10v10h-10z M0 0l5.5.5-1-1 a5 5 0 0 1 10 0", 1);
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_TRUE(paths[0].second);
    EXPECT_EQ(paths[0].first.size(), 4u);
    EXPECT_FLOAT_EQ(paths[0].first[2].x, 20);
    EXPECT_FLOAT_EQ(paths[0].first[2].y, 20);
    EXPECT_FLOAT_EQ(paths[1].first[1].x, 5.5f);
    EXPECT_FLOAT_EQ(paths[1].first[1].y, 0.5f);
    EXPECT_NEAR(paths[1].first.back().x, 14.5f, 1e-3);
    // Garbage ends the path where it starts, keeping what came before.
    const auto partial = parsePathData("M0 0 L10 10 L oops 20", 1);
    ASSERT_EQ(partial.size(), 1u);
    EXPECT_EQ(partial[0].first.size(), 2u);
}

TEST(Raster, Transforms) {
    const Matrix m = parseTransform("translate(10 20) scale(2)");
    const Point p = m.apply({1, 1});
    EXPECT_FLOAT_EQ(p.x, 12);
    EXPECT_FLOAT_EQ(p.y, 22);
    const Point q = parseTransform("rotate(90 5 5)").apply({10, 5});
    EXPECT_NEAR(q.x, 5, 1e-4);
    EXPECT_NEAR(q.y, 10, 1e-4);
    // An error anywhere means no transform at all.
    EXPECT_FLOAT_EQ(parseTransform("scale(2) bogus(1)").a, 1);
}

TEST(Raster, StrokeOutlineCoversTheLine) {
    Canvas c(20, 20);
    Paint p;
    p.color = {0, 0, 0, 255};
    StrokeStyle s;
    s.width = 4;
    c.fill(strokeOutline({{2, 10}, {18, 10}}, false, s, 1), false, p);
    const Raster r = c.toRaster();
    EXPECT_EQ(at(r, 10, 9).a, 255);
    EXPECT_EQ(at(r, 10, 11).a, 255);
    EXPECT_EQ(at(r, 10, 13).a, 0);
    EXPECT_EQ(at(r, 1, 10).a, 0);   // butt caps stop at the ends
}

TEST(Gradient, LinearEndsAndMiddle) {
    Gradient g;
    g.angle = 90;   // to the right
    g.stops = {{{255, 0, 0, 255}}, {{0, 0, 255, 255}}};
    const Raster r = renderGradient(g, 100, 10, 100, 10);
    EXPECT_GT(at(r, 0, 5).r, 250);
    EXPECT_GT(at(r, 99, 5).b, 250);
    EXPECT_NEAR(at(r, 50, 5).r, 125, 4);
    EXPECT_NEAR(at(r, 50, 5).b, 128, 4);
}

TEST(Gradient, HardStopsAndRepeating) {
    Gradient g;   // to bottom, red to 50% then blue
    GradientStop a{{255, 0, 0, 255}, true, Length{false, 0, 50}};
    GradientStop b{{0, 0, 255, 255}, true, Length{false, 0, 50}};
    g.stops = {a, b};
    const Raster r = renderGradient(g, 10, 100, 10, 100);
    EXPECT_EQ(at(r, 5, 40).r, 255);
    EXPECT_EQ(at(r, 5, 60).b, 255);

    Gradient rep;
    rep.repeating = true;
    rep.angle = 90;
    rep.stops = {{{0, 0, 0, 255}, true, Length::pixels(0)}, {{0, 0, 0, 255}, true, Length::pixels(10)},
                 {{255, 255, 255, 255}, true, Length::pixels(10)}, {{255, 255, 255, 255}, true, Length::pixels(20)}};
    const Raster s = renderGradient(rep, 100, 4, 100, 4);
    EXPECT_EQ(at(s, 5, 1).r, 0);
    EXPECT_EQ(at(s, 15, 1).r, 255);
    EXPECT_EQ(at(s, 45, 1).r, 0);
    EXPECT_EQ(at(s, 55, 1).r, 255);
}

TEST(Gradient, TransparentFadesInPremultiplied) {
    // Red to transparent mustn't go through grey.
    Gradient g;
    g.angle = 90;
    g.stops = {{{255, 0, 0, 255}}, {{0, 0, 0, 0}}};
    const Raster r = renderGradient(g, 100, 2, 100, 2);
    const Px mid = at(r, 50, 0);
    EXPECT_GT(mid.r, 250);
    EXPECT_NEAR(mid.a, 128, 4);
}

TEST(Gradient, RadialCentreAndEdge) {
    Gradient g;
    g.kind = Gradient::Kind::Radial;
    g.circle = true;
    g.extent = Gradient::Extent::ClosestSide;
    g.stops = {{{255, 255, 255, 255}}, {{0, 0, 0, 255}}};
    const Raster r = renderGradient(g, 100, 100, 100, 100);
    EXPECT_GT(at(r, 50, 50).r, 245);
    EXPECT_LT(at(r, 50, 1).r, 10);
    EXPECT_LT(at(r, 1, 1).r, 10);   // past the end: the last colour
}

TEST(Style, BackgroundShorthandLayers) {
    const auto doc = parseHtml("<style>#a{background:url(x.png) no-repeat right 10px top / 50% auto, linear-gradient(red, blue) content-box, #123}"
                               "#b{background-image:url(y.png),none;background-repeat:repeat-x;background-position:center}</style>"
                               "<div id=a></div><div id=b></div>");
    const StyledDocument styles(*doc, {800, 600});
    const ComputedStyle* a = styleById(*doc, styles, "a");
    ASSERT_TRUE(a && a->background_layers);
    const BackgroundLayers& la = *a->background_layers;
    ASSERT_EQ(la.images.size(), 3u);   // the colour is a layer of its own, with no image
    EXPECT_TRUE(la.images[2].none());
    EXPECT_EQ(la.images[0].url, "x.png");
    ASSERT_TRUE(la.images[1].gradient);
    EXPECT_EQ(la.images[1].gradient->stops.size(), 2u);
    EXPECT_FALSE(la.repeats[0].first);
    EXPECT_FLOAT_EQ(la.positions[0].first.percent, 100);
    EXPECT_FLOAT_EQ(la.positions[0].first.px, -10);
    EXPECT_FLOAT_EQ(la.sizes[0].width.percent, 50);
    EXPECT_TRUE(la.sizes[0].height.is_auto);
    EXPECT_EQ(la.origins[1], BackgroundBox::Content);
    EXPECT_EQ(a->background, (Color{0x11, 0x22, 0x33, 255}));

    const ComputedStyle* b = styleById(*doc, styles, "b");
    ASSERT_TRUE(b && b->background_layers);
    EXPECT_EQ(b->background_layers->images.size(), 2u);
    EXPECT_TRUE(b->background_layers->repeats[0].first);
    EXPECT_FALSE(b->background_layers->repeats[0].second);
    EXPECT_FLOAT_EQ(b->background_layers->positions[0].second.percent, 50);
}

TEST(Style, GradientSyntax) {
    const auto doc = parseHtml("<style>#a{background:linear-gradient(to top left,red,blue)}"
                               "#b{background:radial-gradient(circle closest-side at 20% 30px,red,blue 40%,green)}"
                               "#c{background:conic-gradient(from 45deg,red,blue)}"
                               "#d{background:-webkit-linear-gradient(top,red,blue)}"
                               "#e{background:linear-gradient(red 10% 20%,blue)}</style>"
                               "<div id=a></div><div id=b></div><div id=c></div><div id=d></div><div id=e></div>");
    const StyledDocument styles(*doc, {800, 600});
    const auto g = [&](const char* id) { return styleById(*doc, styles, id)->background_layers->images[0].gradient; };
    EXPECT_EQ(g("a")->to_x, -1);
    EXPECT_EQ(g("a")->to_y, -1);
    EXPECT_TRUE(g("b")->circle);
    EXPECT_EQ(g("b")->extent, Gradient::Extent::ClosestSide);
    EXPECT_FLOAT_EQ(g("b")->center_x.percent, 20);
    EXPECT_FLOAT_EQ(g("b")->center_y.px, 30);
    EXPECT_EQ(g("c")->kind, Gradient::Kind::Conic);
    EXPECT_FLOAT_EQ(g("c")->from, 45);
    EXPECT_EQ(g("d")->to_y, 1);   // the prefixed form says where it starts: from the top
    EXPECT_EQ(g("e")->stops.size(), 3u);   // a stop with two positions is two
}

TEST(Style, SheetUrlsAreRelativeToTheSheet) {
    const auto doc = parseHtml("<link rel=stylesheet href='https://a.example/css/site.css'><div id=a></div>");
    const StyledDocument styles(*doc, {800, 600}, [](const std::string&) { return std::string("#a{background:url(../img/bg.png)}"); });
    const ComputedStyle* a = styleById(*doc, styles, "a");
    ASSERT_TRUE(a && a->background_layers);
    EXPECT_EQ(a->background_layers->images[0].url, "https://a.example/img/bg.png");
}

TEST(Style, SvgPresentationAttributesAndCss) {
    const auto doc = parseHtml("<style>.c{fill:currentColor} #p2{stroke:red}</style>"
                               "<svg id=s class=c style='color:#00f'><path id=p1 fill='#0f0' d='M0 0'/><path id=p2 stroke-width=3 d='M0 0'/></svg>");
    const StyledDocument styles(*doc, {800, 600});
    const ComputedStyle* s = styleById(*doc, styles, "s");
    const ComputedStyle* p1 = styleById(*doc, styles, "p1");
    const ComputedStyle* p2 = styleById(*doc, styles, "p2");
    ASSERT_TRUE(s && p1 && p2);
    EXPECT_EQ(s->fill.kind, SvgPaint::Kind::CurrentColor);
    EXPECT_TRUE(s->svg_set & kSetFill);
    EXPECT_EQ(p1->fill.kind, SvgPaint::Kind::Color);
    EXPECT_EQ(p1->fill.color, (Color{0, 255, 0, 255}));
    EXPECT_EQ(p2->fill.kind, SvgPaint::Kind::CurrentColor);   // inherited
    EXPECT_FALSE(p2->svg_set & kSetFill);
    EXPECT_EQ(p2->stroke.kind, SvgPaint::Kind::Color);
    EXPECT_FLOAT_EQ(p2->stroke_width, 3);
}

TEST(Svg, ShapesViewBoxAndCurrentColor) {
    const Raster r = svgOf("<svg viewBox='0 0 10 10'><rect x='0' y='0' width='5' height='10' fill='red'/>"
                           "<circle cx='7.5' cy='5' r='2' fill='currentColor'/></svg>", 20, 20);
    ASSERT_EQ(r.width, 20u);
    EXPECT_EQ(at(r, 4, 10).r, 255);
    EXPECT_EQ(at(r, 10, 10).a, 0);
    EXPECT_EQ(at(r, 15, 10).a, 255);   // the circle, in black: no colour was given
    EXPECT_EQ(at(r, 15, 10).r, 0);
}

TEST(Svg, UseInheritsFromTheUse) {
    // The symbol's path has no fill of its own: it takes the <use>'s, not its parent's.
    const Raster r = svgOf("<svg width='10' height='10'><defs fill='red'><symbol id='s' viewBox='0 0 1 1'><rect width='1' height='1'/></symbol></defs>"
                           "<use href='#s' fill='#00f' width='10' height='10'/></svg>", 10, 10);
    EXPECT_EQ(at(r, 5, 5).b, 255);
    EXPECT_EQ(at(r, 5, 5).r, 0);
}

TEST(Svg, StyleSheetInsideTheFile) {
    const Raster r = svgOf("<svg width='10' height='10'><style>.x{fill:#0f0}</style><rect class='x' width='10' height='10' fill='red'/></svg>", 10, 10);
    EXPECT_EQ(at(r, 5, 5).g, 255);
    EXPECT_EQ(at(r, 5, 5).r, 0);
}

TEST(Svg, GradientFillAndClipPath) {
    const Raster r = svgOf("<svg width='100' height='10'><defs><linearGradient id='g'><stop offset='0' stop-color='#000'/><stop offset='1' stop-color='#fff'/></linearGradient>"
                           "<clipPath id='c'><rect width='50' height='10'/></clipPath></defs>"
                           "<rect width='100' height='10' fill='url(#g)' clip-path='url(#c)'/></svg>", 100, 10);
    EXPECT_LT(at(r, 2, 5).r, 10);
    EXPECT_NEAR(at(r, 40, 5).r, 103, 6);
    EXPECT_EQ(at(r, 75, 5).a, 0);   // clipped away
}

TEST(Svg, NaturalSize) {
    auto a = SvgImage::parse("<?xml version='1.0'?><svg width='40' height='20'></svg>");
    ASSERT_TRUE(a);
    EXPECT_FLOAT_EQ(*a->size().width, 40);
    EXPECT_FLOAT_EQ(a->size().ratio, 2);
    auto b = SvgImage::parse("<svg viewBox='0 0 30 10'></svg>");
    ASSERT_TRUE(b);
    EXPECT_FALSE(b->size().width);
    EXPECT_FLOAT_EQ(b->size().ratio, 3);
    EXPECT_FALSE(SvgImage::parse("<html>not an image</html>"));
}

TEST(Layout, BackgroundTilesAndWantedImages) {
    const auto doc = parseHtml("<style>body{margin:0}#a{width:35px;height:20px;background:url(a.png) repeat-x}"
                               "#b{width:10px;height:10px;background:url(missing.png)}"
                               "#c{width:50px;height:10px;background:linear-gradient(red,blue)}</style><div id=a></div><div id=b></div><div id=c></div>");
    const StyledDocument styles(*doc, {800, 600});
    FixedText text;
    Sizes sizes;
    const PageLayout page = layoutPage(*doc, styles, text, &sizes, 800, 600);
    int tiles = 0, gradients = 0;
    for (const PaintOp& op : page.ops) {
        if (op.kind == PaintKind::Image && op.text == "a.png") {
            ++tiles;
            EXPECT_FLOAT_EQ(op.w, 10);
            EXPECT_FLOAT_EQ(op.h, 20);
        }
        if (op.kind == PaintKind::Gradient) {
            ++gradients;
            EXPECT_FLOAT_EQ(op.w, 50);
        }
    }
    EXPECT_EQ(tiles, 4);   // 35px of 10px tiles
    EXPECT_EQ(gradients, 1);
    ASSERT_EQ(page.images_wanted.size(), 1u);
    EXPECT_EQ(page.images_wanted[0], "missing.png");
}

TEST(Layout, MaskTintsTheBackgroundColour) {
    const auto doc = parseHtml("<style>#m{width:20px;height:20px;background:#f00;mask:url(a.png) no-repeat}</style><div id=m></div>");
    const StyledDocument styles(*doc, {800, 600});
    FixedText text;
    Sizes sizes;
    const PageLayout page = layoutPage(*doc, styles, text, &sizes, 800, 600);
    bool tinted = false;
    for (const PaintOp& op : page.ops) {
        if (op.kind == PaintKind::Rect) EXPECT_FALSE(op.color == (Color{255, 0, 0, 255}));   // no plain block of it
        if (op.kind == PaintKind::Image && op.tint) {
            tinted = true;
            EXPECT_EQ(op.color, (Color{255, 0, 0, 255}));
        }
    }
    EXPECT_TRUE(tinted);
}

TEST(Layout, SvgImageWithOnlyAShapeFillsTheWidth) {
    const auto doc = parseHtml("<style>body{margin:0;width:300px}</style><img id=i src=shape.svg>");
    const StyledDocument styles(*doc, {800, 600});
    FixedText text;
    Sizes sizes;
    const PageLayout page = layoutPage(*doc, styles, text, &sizes, 800, 600);
    bool found = false;
    for (const ElementBox& b : page.boxes)
        if (b.node->isHtml("img")) {
            found = true;
            EXPECT_FLOAT_EQ(b.w, 300);
            EXPECT_FLOAT_EQ(b.h, 150);
        }
    EXPECT_TRUE(found);
}

TEST(Fuzz, HostileSvgAndBackgrounds) {
    // None of this may hang, crash or run out of memory.
    std::string deep = "<rect width='1' height='1'/>";
    for (int i = 0; i < 2000; ++i) deep = "<g opacity='0.5' clip-path='url(#c)'>" + deep + "</g>";
    (void)svgOf("<svg width='50' height='50'><clipPath id='c'><rect width='9' height='9'/></clipPath>" + deep + "</svg>", 50, 50);
    (void)svgOf("<svg width='50' height='50'><g id='a'><use href='#b'/></g><g id='b'><use href='#a'/></g><use href='#a'/></svg>", 50, 50);
    std::string fan = "<svg width='50' height='50'><defs><g id='l0'><rect width='1' height='1'/></g>";
    for (int i = 1; i < 30; ++i) fan += "<g id='l" + std::to_string(i) + "'><use href='#l" + std::to_string(i - 1) + "'/><use href='#l" + std::to_string(i - 1) + "'/></g>";
    (void)svgOf(fan + "</defs><use href='#l29'/></svg>", 50, 50);   // 2^29 copies, if nothing stopped it
    (void)svgOf("<svg width='50' height='50'><path d='M1e30 1e30 L-1e30 5 C 1e38 1e38 1e38 1e38 1e38 1e38 Z' stroke='red' stroke-width='1e9'/></svg>", 50, 50);
    (void)svgOf("<svg width='50' height='50'><path d='M0 0L50 50' stroke='red' stroke-dasharray='0.0001 0.0001'/></svg>", 50, 50);
    (void)svgOf("<svg width='50' height='50'><circle r='1e9' fill='red'/><circle r='1e-9'/><rect width='-5' height='nan'/></svg>", 50, 50);
    (void)svgOf("<svg viewBox='0 0 0 0' width='50' height='50'><polygon points='1 2 3'/></svg>", 50, 50);
    EXPECT_TRUE(renderSvg(*parseHtml("<svg></svg>")->body()->children[0], 100000, 100000, nullptr, {}).empty());

    std::mt19937 rng(11);
    for (int round = 0; round < 300; ++round) {
        std::string d = "M";
        const char* cmds = "MmLlHhVvCcSsQqTtAaZz ,.-0123456789e";
        for (int i = 0; i < 200; ++i) d += cmds[rng() % 36];
        (void)parsePathData(d, 1);
        (void)parseTransform(d);
    }

    const auto run = [](const std::string& html) {
        const auto doc = parseHtml(html);
        const StyledDocument styles(*doc, {800, 600});
        FixedText text;
        Sizes sizes;
        (void)layoutPage(*doc, styles, text, &sizes, 800, 600);
    };
    run("<style>div{width:100000px;height:100000px;background:url(a.png);background-size:0.001px}</style><div></div>");
    run("<style>div{width:100px;height:100px;background:url(a.png) 1e30px 1e30px / 1e-30px 1e-30px}</style><div></div>");
    std::string many = "background:";
    for (int i = 0; i < 5000; ++i) many += "linear-gradient(red,blue),";
    run("<style>div{width:10px;height:10px;" + many + "red}</style><div></div>");
    std::string stops = "linear-gradient(";
    for (int i = 0; i < 5000; ++i) stops += "red " + std::to_string(i) + "px,";
    run("<style>div{background:" + stops + "blue)}</style><div></div>");
}

TEST(BoxPaint, DashedSideHasGaps) {
    BoxPaint b;
    b.part = BoxPaint::Part::Border;
    b.w = 100;
    b.h = 20;
    b.border[2] = 3;
    b.colors[2] = {0, 0, 0, 255};
    b.styles[2] = BorderStyle::Dashed;
    const Raster r = renderBoxPaint(b, 100, 20, 100, 20);
    int on = 0, off = 0;
    for (uint32_t x = 0; x < 100; ++x) (at(r, x, 18).a > 128 ? on : off)++;
    EXPECT_GT(on, 40);
    EXPECT_GT(off, 20);
    EXPECT_EQ(at(r, 50, 5).a, 0);
}

TEST(Style, BorderStylesPerSide) {
    const auto doc = parseHtml("<style>#a{border-top:5px solid #333;border-bottom:3px dashed #333}"
                               "#b{border:2px dotted red;border-left-style:double;border-radius:10px 20px / 5px}</style><div id=a></div><div id=b></div>");
    const StyledDocument styles(*doc, {800, 600});
    const ComputedStyle* a = styleById(*doc, styles, "a");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->border_style[Top], BorderStyle::Solid);
    EXPECT_EQ(a->border_style[Bottom], BorderStyle::Dashed);
    EXPECT_FLOAT_EQ(a->border_width[Left], 0);
    const ComputedStyle* b = styleById(*doc, styles, "b");
    EXPECT_EQ(b->border_style[Left], BorderStyle::Double);
    EXPECT_EQ(b->border_style[Top], BorderStyle::Dotted);
    EXPECT_FLOAT_EQ(b->radius[1][0].px, 20);
    EXPECT_FLOAT_EQ(b->radius[3][0].px, 20);
    EXPECT_FLOAT_EQ(b->radius[2][1].px, 5);
}

TEST(BoxPaint, ShadowsAndCorners) {
    // An inset shadow darkens the edges, not the middle.
    BoxPaint b;
    b.part = BoxPaint::Part::InsetShadow;
    b.w = 60;
    b.h = 60;
    b.shadow.blur = 10;
    b.shadow.color = {0, 0, 0, 255};
    const Raster r = renderBoxPaint(b, 60, 60, 60, 60);
    EXPECT_GT(at(r, 1, 30).a, 60);
    EXPECT_LT(at(r, 30, 30).a, 5);
    // An outer one spreads beyond the box, but not under it.
    BoxPaint o;
    o.part = BoxPaint::Part::OuterShadow;
    o.w = o.h = 20;
    o.ox = o.oy = 10;
    o.shadow.spread = 5;
    o.shadow.color = {255, 0, 0, 255};
    const Raster s = renderBoxPaint(o, 40, 40, 40, 40);
    EXPECT_EQ(at(s, 7, 20).a, 255);
    EXPECT_EQ(at(s, 20, 20).a, 0);
    EXPECT_EQ(at(s, 2, 20).a, 0);
    // Uneven corners: the rounded one is cut, the square one isn't.
    BoxPaint c;
    c.part = BoxPaint::Part::Background;
    c.w = c.h = 40;
    c.radii[0][0] = c.radii[0][1] = 20;
    c.background = {0, 0, 255, 255};
    const Raster t = renderBoxPaint(c, 40, 40, 40, 40);
    EXPECT_EQ(at(t, 1, 1).a, 0);
    EXPECT_EQ(at(t, 38, 1).a, 255);
    float radii[4][2] = {{100, 100}, {100, 100}, {0, 0}, {0, 0}};
    fitRadii(radii, 50, 300);
    EXPECT_FLOAT_EQ(radii[0][0], 25);
}

TEST(Fuzz, HostileBoxes) {
    const auto run = [](const std::string& html) {
        const auto doc = parseHtml(html);
        const StyledDocument styles(*doc, {800, 600});
        FixedText text;
        const PageLayout page = layoutPage(*doc, styles, text, nullptr, 800, 600);
        for (const PaintOp& op : page.ops)
            if (op.kind == PaintKind::Box && op.box) {
                const float w = std::min(op.w, 300.0f), h = std::min(op.h, 300.0f);
                (void)renderBoxPaint(*op.box, w, h, static_cast<uint32_t>(std::max(1.0f, w)), static_cast<uint32_t>(std::max(1.0f, h)));
            }
    };
    run("<div style='width:50px;height:50px;box-shadow:0 0 1e9px 1e9px red, inset 1e9px 0 0 -1e9px blue'></div>");
    run("<div style='width:50px;height:50px;border-radius:1e9px 0 / 0 1e9px;border:1e9px dashed red'></div>");
    run("<div style='width:100000px;height:1px;border:1px dotted red;border-radius:50%'></div>");
    run("<div style='width:0;height:0;border:5px double red;border-radius:3px;box-shadow:inset 0 0 5px red'></div>");
    std::string many = "box-shadow:";
    for (int i = 0; i < 100; ++i) many += "1px 1px 30px red,";
    run("<div style='width:20px;height:20px;" + many + "0 0 red'></div>");
    run("<div style='width:20px;height:20px;border-style:groove ridge inset outset;border-width:1e-9px 7px 3px nanpx;border-radius:-5px'></div>");
}

TEST(Raster, DrawsAnImageThroughAMatrix) {
    // A 4x2 image, left half red and right half blue, turned a quarter about its corner.
    Raster img;
    img.width = 4;
    img.height = 2;
    for (uint32_t y = 0; y < 2; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
            const bool left = x < 2;
            img.rgba.insert(img.rgba.end(), {static_cast<uint8_t>(left ? 255 : 0), 0, static_cast<uint8_t>(left ? 0 : 255), 255});
        }
    Canvas c(10, 10);
    const Matrix quarter{0, 1, -1, 0, 0, 0};   // (x, y) -> (-y, x)
    c.drawRaster(img, Matrix::translate(5, 2).after(quarter), 1);
    const Raster r = c.toRaster();
    // The image now runs down from (3..5, 2) to (3..5, 6): red on top, blue below.
    EXPECT_GT(at(r, 4, 2).r, 200);
    EXPECT_GT(at(r, 4, 5).b, 200);
    EXPECT_EQ(at(r, 6, 3).a, 0);
    EXPECT_EQ(at(r, 4, 7).a, 0);
}
