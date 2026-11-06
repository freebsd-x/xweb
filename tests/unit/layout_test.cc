// Layout with fixed-width text, so positions are exact. Against a real browser, see
// scripts/xweb-compare.py.

#include <gtest/gtest.h>

#include <map>
#include <string>

#include "xweb/html_parser.h"
#include "xweb/layout.h"
#include "xweb/style.h"

using namespace fbx::xweb;

namespace {

class FixedText : public TextMeasurer {
public:
    float width(std::string_view s, const FontSpec& f) override {
        size_t chars = 0;
        for (char c : s)
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++chars;
        return static_cast<float>(chars) * f.size / 2;
    }
    float ascent(const FontSpec& f) override { return f.size * 0.75f; }
    float descent(const FontSpec& f) override { return f.size * 0.25f; }
};

class TestImages : public ImageSizes {
public:
    std::optional<std::pair<float, float>> imageSize(const std::string& src) override {
        if (src == "wide.png") return std::make_pair(200.0f, 100.0f);
        return std::nullopt;
    }
};

struct Result {
    std::unique_ptr<Document> doc;
    std::unique_ptr<StyledDocument> styles;
    PageLayout page;

    const ElementBox& box(const std::string& id) const {
        for (const auto& b : page.boxes)
            if (const std::string* v = b.node->attr("id"); v && *v == id) return b;
        static const ElementBox none;
        ADD_FAILURE() << "no box #" << id;
        return none;
    }
    std::vector<std::string> texts() const {
        std::vector<std::string> out;
        for (const auto& op : page.ops)
            if (op.kind == PaintKind::Text) out.push_back(op.text);
        return out;
    }
};

Result layout(const std::string& html, float width = 400) {
    Result r;
    r.doc = parseHtml("<!doctype html><style>body{margin:0;font-size:10px;line-height:10px}</style>" + html);
    r.styles = std::make_unique<StyledDocument>(*r.doc, css::MediaContext{width, 300});
    FixedText text;
    TestImages images;
    r.page = layoutPage(*r.doc, *r.styles, text, &images, width, 300);
    return r;
}

}  // namespace

TEST(Block, StacksAndFillsWidth) {
    const auto r = layout("<div id=a style=height:10px></div><div id=b style=height:20px;padding:5px;border:1px;border-style:solid></div><div id=c>x</div>");
    EXPECT_FLOAT_EQ(r.box("a").w, 400);
    EXPECT_FLOAT_EQ(r.box("b").y, 10);
    EXPECT_FLOAT_EQ(r.box("b").h, 32);
    EXPECT_FLOAT_EQ(r.box("b").w, 400);
    EXPECT_FLOAT_EQ(r.box("c").y, 42);
    EXPECT_FLOAT_EQ(r.box("c").h, 10);
}

TEST(Block, MarginsCollapse) {
    const auto r = layout("<div id=a style='height:1px;margin-bottom:20px'></div><div id=b style='height:1px;margin-top:30px'></div>"
                          "<div style=margin-top:5px><div id=c style='margin-top:40px;height:1px'></div></div>"
                          "<div id=d style='margin:50px 0;height:0'></div><div id=e style='height:1px;margin-top:-10px'></div>");
    EXPECT_FLOAT_EQ(r.box("a").y, 0);
    EXPECT_FLOAT_EQ(r.box("b").y, 31);
    EXPECT_FLOAT_EQ(r.box("c").y, 72);
    EXPECT_FLOAT_EQ(r.box("e").y, 73 + 50 - 10);
}

TEST(Block, BordersAndPaddingStopCollapse) {
    const auto r = layout("<div id=p style=padding-top:1px><div id=c style='margin-top:10px;height:1px'></div></div>");
    EXPECT_FLOAT_EQ(r.box("p").y, 0);
    EXPECT_FLOAT_EQ(r.box("c").y, 11);
}

TEST(Block, WidthsAndAutoMargins) {
    const auto r = layout("<div id=a style='width:100px;margin:0 auto'></div><div id=b style='width:100px;margin-left:auto'></div>"
                          "<div id=c style='width:50%'></div><div id=d style='box-sizing:border-box;width:100px;padding:10px'></div>"
                          "<div id=e style='width:100px;max-width:50px'></div><div id=f style='width:10px;min-width:50px'></div><div id=g style='width:calc(50% - 10px)'></div>");
    EXPECT_FLOAT_EQ(r.box("a").x, 150);
    EXPECT_FLOAT_EQ(r.box("b").x, 300);
    EXPECT_FLOAT_EQ(r.box("c").w, 200);
    EXPECT_FLOAT_EQ(r.box("d").w, 100);
    EXPECT_FLOAT_EQ(r.box("e").w, 50);
    EXPECT_FLOAT_EQ(r.box("f").w, 50);
    EXPECT_FLOAT_EQ(r.box("g").w, 190);
}

TEST(Inline, WrapsAtSpacesAndCollapsesThem) {
    const auto r = layout("<div id=a style=width:50px>aaaa   bbbb\n\ncccc dddd eeee</div>");   // 5px per char: 10 chars a line
    EXPECT_EQ(r.texts(), (std::vector<std::string>{"aaaa bbbb", "cccc dddd", "eeee"}));
    EXPECT_FLOAT_EQ(r.box("a").h, 30);
}

TEST(Inline, LeadingAndTrailingSpaceGo) {
    const auto r = layout("<div>   a   </div><div> <b> b </b> <i> c </i> </div>");
    EXPECT_EQ(r.texts(), (std::vector<std::string>{"a", "b ", "c"}));
}

TEST(Inline, NoBreakAcrossElementsWithoutASpace) {
    const auto r = layout("<div style=width:20px>aaa<b>bbb</b></div>");
    EXPECT_EQ(r.texts(), (std::vector<std::string>{"aaa", "bbb"}));
    EXPECT_FLOAT_EQ(r.page.ops[0].y, r.page.ops[1].y);
}

TEST(Inline, BreaksAfterHyphensAndAroundCjk) {
    const auto r = layout("<div style=width:25px>well-known</div>");
    EXPECT_EQ(r.texts(), (std::vector<std::string>{"well-", "known"}));
    const auto cjk = layout("<div style=width:10px>\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E</div>");
    EXPECT_EQ(cjk.texts().size(), 2u);
}

TEST(Inline, WhiteSpaceModes) {
    EXPECT_EQ(layout("<pre style='margin:0;font:10px/10px monospace'>a  b\nc</pre>").texts(), (std::vector<std::string>{"a  b", "c"}));
    EXPECT_EQ(layout("<div style='white-space:nowrap;width:5px'>a b c</div>").texts(), (std::vector<std::string>{"a b c"}));
    EXPECT_EQ(layout("<div style=white-space:pre-line>a  b\nc</div>").texts(), (std::vector<std::string>{"a b", "c"}));
    EXPECT_EQ(layout("a<br>b<br><br>c").texts(), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_FLOAT_EQ(layout("<div id=a>a<br>b<br><br>c</div>").box("a").h, 40);
}

TEST(Inline, TextAlign) {
    const auto c = layout("<div style=text-align:center>aaaa</div>");
    EXPECT_FLOAT_EQ(c.page.ops[0].x, 190);
    const auto r = layout("<div style=text-align:right>aaaa</div>");
    EXPECT_FLOAT_EQ(r.page.ops[0].x, 380);
}

TEST(Inline, LineHeightCentresTheText) {
    const auto r = layout("<div id=a style=line-height:30px>a</div>");
    EXPECT_FLOAT_EQ(r.box("a").h, 30);
    EXPECT_FLOAT_EQ(r.page.ops[0].y, 10);   // 10px of box in a 30px line
}

TEST(InlineBlock, ShrinksAndSitsOnTheLine) {
    const auto r = layout("<div>aa<span id=b style='display:inline-block;padding:2px'>bbbb</span>cc</div>");
    EXPECT_FLOAT_EQ(r.box("b").x, 10);
    EXPECT_FLOAT_EQ(r.box("b").w, 24);
    EXPECT_EQ(r.texts().back(), "cc");
}

TEST(Replaced, Images) {
    const auto r = layout("<img id=a src=wide.png><img id=b src=wide.png width=100><img id=c src=wide.png style=max-width:50px>"
                          "<img id=d src=missing.png alt=hello><img id=e src=missing.png>");
    EXPECT_FLOAT_EQ(r.box("a").w, 200);
    EXPECT_FLOAT_EQ(r.box("a").h, 100);
    EXPECT_FLOAT_EQ(r.box("b").w, 100);
    EXPECT_FLOAT_EQ(r.box("b").h, 50);
    EXPECT_FLOAT_EQ(r.box("c").w, 50);
    EXPECT_FLOAT_EQ(r.box("c").h, 25);
    EXPECT_FLOAT_EQ(r.box("d").w, 25);
    EXPECT_FLOAT_EQ(r.box("e").w, 0);
    int images = 0;
    for (const auto& op : r.page.ops) images += op.kind == PaintKind::Image;
    EXPECT_EQ(images, 3);
}

TEST(Lists, Markers) {
    const auto r = layout("<ul><li>a<li>b</ul><ol start=4><li>c<li value=9>d<li>e</ol><ol style=list-style-type:upper-roman><li>f</ol><ul style=list-style:none><li>g</ul>");
    const auto t = r.texts();
    const std::vector<std::string> expected{"\xE2\x80\xA2", "a", "\xE2\x80\xA2", "b", "4.", "c", "9.", "d", "10.", "e", "I.", "f", "g"};
    EXPECT_EQ(t, expected);
}

TEST(Positioned, OutOfFlow) {
    const auto r = layout("<div id=host style='position:relative;margin:10px;width:100px;height:100px'>"
                          "<div id=a style='position:absolute;left:5px;top:6px;width:7px;height:8px'></div>"
                          "<div id=b style='position:absolute;right:5px;bottom:6px;width:7px;height:8px'></div></div>"
                          "<div id=f style='position:fixed;left:1px;top:2px'></div><div id=c style=height:1px></div>"
                          "<div id=r style='position:relative;left:3px;top:4px;height:1px'></div>");
    EXPECT_FLOAT_EQ(r.box("a").x, 15);
    EXPECT_FLOAT_EQ(r.box("a").y, 16);
    EXPECT_FLOAT_EQ(r.box("b").x, 10 + 100 - 5 - 7);
    EXPECT_FLOAT_EQ(r.box("b").y, 10 + 100 - 6 - 8);
    EXPECT_FLOAT_EQ(r.box("f").x, 1);
    EXPECT_FLOAT_EQ(r.box("c").y, 120);   // the absolute ones took no room
    EXPECT_FLOAT_EQ(r.box("r").x, 3);
    EXPECT_FLOAT_EQ(r.box("r").y, 125);
}

TEST(Paint, BackgroundsBordersLinksAndCanvas) {
    const auto r = layout("<style>body{background:#00f}</style><div style='background:red;height:5px'></div><p><a href=/x>go</a></p>");
    EXPECT_EQ(r.page.canvas, (Color{0, 0, 255, 255}));
    ASSERT_FALSE(r.page.links.empty());
    EXPECT_EQ(r.page.links[0].href, "/x");
    EXPECT_FLOAT_EQ(r.page.links[0].w, 10);
    bool red = false, underline = false;
    for (const auto& op : r.page.ops) {
        if (op.kind != PaintKind::Rect) continue;
        red = red || op.color == Color{255, 0, 0, 255};
        underline = underline || (op.color == Color{0, 0, 238, 255} && op.w == 10);
    }
    EXPECT_TRUE(red);
    EXPECT_TRUE(underline);
}

TEST(Paint, HiddenAndOpacity) {
    const auto r = layout("<div style=visibility:hidden>gone</div><div id=a style=opacity:.5>half</div><div style=display:none>none</div>");
    EXPECT_EQ(r.texts(), (std::vector<std::string>{"half"}));
    EXPECT_EQ(r.page.ops.back().color.a, 128);
    EXPECT_FLOAT_EQ(r.box("a").y, 10);
}

TEST(Page, GrowsWithContentAndKeepsTheTitle) {
    const auto r = layout("<title> T </title><div style=height:5000px></div>");
    EXPECT_FLOAT_EQ(r.page.height, 5000);
    EXPECT_EQ(r.page.title, " T ");
    EXPECT_FLOAT_EQ(layout("x").page.height, 300);
}
