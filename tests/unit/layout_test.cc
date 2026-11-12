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

TEST(Floats, TextWrapsAroundThem) {
    const auto r = layout("<div style=width:100px><div id=f style='float:left;width:50px;height:15px'></div>aaaaaaaaaa bbbbbbbbbb cccccccccc</div>");
    // 5px a character: ten fit beside the float's 50px, then the float ends at 15px.
    ASSERT_GE(r.page.ops.size(), 2u);
    EXPECT_FLOAT_EQ(r.box("f").x, 0);
    float first_x = -1;
    for (const auto& op : r.page.ops)
        if (op.kind == PaintKind::Text && first_x < 0) first_x = op.x;
    EXPECT_FLOAT_EQ(first_x, 50);
}

TEST(Floats, SideBySideThenBelow) {
    const auto r = layout("<div style=width:100px><div id=a style='float:left;width:40px;height:10px'></div><div id=b style='float:left;width:40px;height:10px'></div>"
                          "<div id=c style='float:left;width:40px;height:10px'></div><div id=d style='float:right;width:10px;height:10px'></div></div>");
    EXPECT_FLOAT_EQ(r.box("b").x, 40);
    EXPECT_FLOAT_EQ(r.box("b").y, 0);
    EXPECT_FLOAT_EQ(r.box("c").x, 0);
    EXPECT_FLOAT_EQ(r.box("c").y, 10);
    EXPECT_FLOAT_EQ(r.box("d").x, 90);
}

TEST(Floats, ClearAndContainment) {
    const auto r = layout("<style>.cf::after{content:'';display:table;clear:both}</style>"
                          "<div id=none><div style='float:left;width:9px;height:30px'></div></div><div id=c style='clear:both;height:1px'></div>"
                          "<div id=fix class=cf><div style='float:left;width:9px;height:30px'></div></div>"
                          "<div id=ov style=overflow:hidden><div style='float:left;width:9px;height:30px'></div></div>");
    EXPECT_FLOAT_EQ(r.box("none").h, 0);   // a plain block doesn't contain its float
    EXPECT_FLOAT_EQ(r.box("c").y, 30);
    EXPECT_FLOAT_EQ(r.box("fix").h, 30);   // the clearfix does
    EXPECT_FLOAT_EQ(r.box("ov").h, 30);    // so does a new formatting context
}

TEST(Floats, NewFormattingContextsSitBeside) {
    const auto r = layout("<div style='float:left;width:100px;height:50px'></div><div id=b style='overflow:hidden;height:10px'></div>");
    EXPECT_FLOAT_EQ(r.box("b").x, 100);
    EXPECT_FLOAT_EQ(r.box("b").w, 300);
}

TEST(Tables, RowsColumnsAndSpacing) {
    const auto r = layout("<table style=border-spacing:2px><tr><td id=a style=padding:0>aa</td><td id=b style=padding:0>bbbb</td></tr>"
                          "<tr><td id=c style=padding:0>cccc</td><td id=d style=padding:0>b</td></tr></table>");
    EXPECT_FLOAT_EQ(r.box("a").w, 20);    // the column's widest: "cccc"
    EXPECT_FLOAT_EQ(r.box("b").x, 24);
    EXPECT_FLOAT_EQ(r.box("b").w, 20);
    EXPECT_FLOAT_EQ(r.box("c").y, 14);    // spacing, a 10px line, spacing
}

TEST(Tables, SpansAndWidths) {
    const auto r = layout("<table id=t style='width:200px;border-spacing:0'><tr><td id=s colspan=2 style=padding:0>x</td></tr><tr><td id=a style='padding:0;width:50%'>a</td><td id=b style=padding:0>b</td></tr></table>"
                          "<table style=border-spacing:0><tr><td id=r rowspan=2 style=padding:0>y</td><td style=padding:0>1</td></tr><tr><td id=n style=padding:0>2</td></tr></table>");
    EXPECT_FLOAT_EQ(r.box("t").w, 200);
    EXPECT_FLOAT_EQ(r.box("s").w, 200);
    EXPECT_FLOAT_EQ(r.box("a").w, 100);
    EXPECT_FLOAT_EQ(r.box("b").w, 100);
    EXPECT_FLOAT_EQ(r.box("r").h, 20);
}

TEST(Tables, MalformedTablesStillLayOut) {
    const auto r = layout("<div style=display:table-cell id=orphan>x</div><table><tr><td colspan=99999 rowspan=99999>y</td></tr></table><table></table><table><caption>c</caption></table>");
    EXPECT_FLOAT_EQ(r.box("orphan").h, 10);
}

TEST(Flex, GrowsAndJustifies) {
    const auto r = layout("<div style=display:flex><div id=a style=flex:1></div><div id=b style=flex:3></div></div>"
                          "<div style='display:flex;justify-content:space-between;width:100px'><i id=c style=width:10px></i><i id=d style=width:10px></i></div>"
                          "<div style='display:flex;justify-content:center;width:100px'><i id=e style=width:10px></i></div>");
    EXPECT_FLOAT_EQ(r.box("a").w, 100);
    EXPECT_FLOAT_EQ(r.box("b").x, 100);
    EXPECT_FLOAT_EQ(r.box("b").w, 300);
    EXPECT_FLOAT_EQ(r.box("d").x, 90);
    EXPECT_FLOAT_EQ(r.box("e").x, 45);
}

TEST(Flex, ShrinksStretchesAndWraps) {
    const auto r = layout("<div style='display:flex;width:100px'><div id=a style='flex-basis:100px;min-width:0'></div><div id=b style='flex-basis:100px;flex-shrink:3;min-width:0'></div></div>"
                          "<div style='display:flex;height:40px'><div id=c></div></div>"
                          "<div style='display:flex;flex-wrap:wrap;width:50px'><div id=d style='width:30px;height:10px'></div><div id=e style='width:30px;height:10px'></div></div>"
                          "<div style='display:flex;flex-direction:column;height:30px'><div id=f style=flex:1></div><div id=g style=height:10px></div></div>");
    EXPECT_FLOAT_EQ(r.box("a").w, 75);
    EXPECT_FLOAT_EQ(r.box("b").w, 25);
    EXPECT_FLOAT_EQ(r.box("c").h, 40);
    EXPECT_FLOAT_EQ(r.box("e").x, 0);
    EXPECT_FLOAT_EQ(r.box("e").y - r.box("d").y, 10);
    EXPECT_FLOAT_EQ(r.box("f").h, 20);
}

TEST(Generated, BeforeAndAfter) {
    const auto r = layout("<style>#a::before{content:'x'}#a::after{content:attr(data-v)}#b::before{content:none}#c::before{color:red}</style>"
                          "<div id=a data-v=y>m</div><div id=b>n</div><div id=c>o</div>");
    EXPECT_EQ(r.texts(), (std::vector<std::string>{"x", "m", "y", "n", "o"}));
}

TEST(Clip, OverflowHiddenWrapsItsContent) {
    const auto r = layout("<div style='overflow:hidden;height:10px'>x</div>");
    int begins = 0, ends = 0;
    for (const auto& op : r.page.ops) {
        begins += op.kind == PaintKind::ClipBegin;
        ends += op.kind == PaintKind::ClipEnd;
    }
    EXPECT_EQ(begins, 1);
    EXPECT_EQ(ends, 1);
}

TEST(Intrinsic, PercentWidthsDontExplodeMeasuring) {
    const auto r = layout("<div style=display:flex><nav id=n><img src=wide.png style=width:50%></nav><div id=m style=flex:1>x</div></div>");
    EXPECT_LE(r.box("n").w, 200);
}

TEST(Links, ABlockLinkIsClickableAllOver) {
    const auto r = layout("<a href=/x style='display:block;width:50px;height:20px'></a><a href=/y style='display:inline-block;width:30px;height:9px'></a>");
    ASSERT_GE(r.page.links.size(), 2u);
    EXPECT_EQ(r.page.links[0].href, "/x");
    EXPECT_FLOAT_EQ(r.page.links[0].w, 50);
    EXPECT_FLOAT_EQ(r.page.links[0].h, 20);
    EXPECT_EQ(r.page.links[1].href, "/y");
}

// Grid. Boxes against Chromium are in tests/pages/grid.html; these pin the rules down.

TEST(Grid, FixedFrAndGaps) {
    const auto r = layout("<!doctype html><style>body{margin:0}#g{display:grid;grid-template-columns:100px 1fr 3fr;gap:10px 20px;grid-auto-rows:30px}</style>"
                          "<div id=g><i id=a style=display:block>a</i><i id=b style=display:block>b</i><i id=c style=display:block>c</i><i id=d style=display:block>d</i></div>", 660);
    EXPECT_FLOAT_EQ(r.box("a").w, 100);
    EXPECT_FLOAT_EQ(r.box("b").x, 120);
    EXPECT_FLOAT_EQ(r.box("b").w, 130);
    EXPECT_FLOAT_EQ(r.box("c").x, 270);
    EXPECT_FLOAT_EQ(r.box("c").w, 390);
    EXPECT_FLOAT_EQ(r.box("d").y, 40);
    EXPECT_FLOAT_EQ(r.box("g").h, 70);
}

TEST(Grid, NamedAreasPlaceItems) {
    const auto r = layout("<!doctype html><style>body{margin:0}#g{display:grid;grid-template:'h h' 10px 'n m' 20px / 50px 1fr}"
                          "#m{grid-area:m}#h{grid-area:h}#n{grid-area:n}</style><div id=g><div id=m></div><div id=n></div><div id=h></div></div>", 300);
    EXPECT_FLOAT_EQ(r.box("h").w, 300);
    EXPECT_FLOAT_EQ(r.box("n").y, 10);
    EXPECT_FLOAT_EQ(r.box("n").w, 50);
    EXPECT_FLOAT_EQ(r.box("m").x, 50);
    EXPECT_FLOAT_EQ(r.box("m").h, 20);
}

TEST(Grid, AutoFillAndAutoFit) {
    const std::string kids = "<div id=a></div><div id=b></div>";
    auto r = layout("<!doctype html><style>body{margin:0}#g{display:grid;grid-template-columns:repeat(auto-fill,minmax(100px,1fr))}</style><div id=g>" + kids + "</div>", 450);
    EXPECT_FLOAT_EQ(r.box("a").w, 112.5f);   // four tracks
    r = layout("<!doctype html><style>body{margin:0}#g{display:grid;grid-template-columns:repeat(auto-fit,minmax(100px,1fr))}</style><div id=g>" + kids + "</div>", 450);
    EXPECT_FLOAT_EQ(r.box("a").w, 225);      // the empty ones collapse
}

TEST(Grid, ItemsAreAlignedInTheirAreas) {
    const auto r = layout("<!doctype html><style>body{margin:0}#g{display:grid;grid-template-columns:100px;grid-template-rows:100px}"
                          "#a{width:20px;height:10px;justify-self:end;align-self:center}</style><div id=g><div id=a></div></div>", 300);
    EXPECT_FLOAT_EQ(r.box("a").x, 80);
    EXPECT_FLOAT_EQ(r.box("a").y, 45);
}

TEST(Grid, AnInvalidTemplateIsIgnored) {
    // "b" isn't a rectangle, so the whole declaration goes, as if it weren't there.
    const auto r = layout("<!doctype html><style>body{margin:0}#g{display:grid;grid-template-areas:'a b' 'b b'}</style><div id=g><div id=a style=height:5px></div><div id=c style=height:5px></div></div>", 300);
    EXPECT_FLOAT_EQ(r.box("c").y, 5);
    EXPECT_FLOAT_EQ(r.box("c").w, 300);
}

TEST(Width, ContentKeywords) {
    const auto r = layout("<!doctype html><style>body{margin:0;font-size:10px}div{margin:auto}</style>"
                          "<div id=a style=width:max-content>aaaa bbbb</div><div id=b style=width:min-content>aaaa bbbb</div><div id=c style=width:fit-content>aaaa bbbb</div>", 300);
    EXPECT_FLOAT_EQ(r.box("a").w, 45);
    EXPECT_FLOAT_EQ(r.box("b").w, 20);
    EXPECT_FLOAT_EQ(r.box("c").w, 45);
    EXPECT_FLOAT_EQ(r.box("c").x, 127.5f);   // centred, as it has a width of its own
}

TEST(Contents, ChildrenTakeItsPlace) {
    const auto r = layout("<!doctype html><style>body{margin:0}#g{display:grid;grid-template-columns:100px 100px}</style>"
                          "<div id=g><section style='display:contents;border:9px solid'><div id=a></div><div style=display:contents><div id=b></div></div></section></div>", 400);
    EXPECT_FLOAT_EQ(r.box("a").x, 0);
    EXPECT_FLOAT_EQ(r.box("a").w, 100);
    EXPECT_FLOAT_EQ(r.box("b").x, 100);
}

TEST(Contents, HasNoBoxButPassesOnItsStyle) {
    const auto r = layout("<!doctype html><body style=margin:0><div id=a style='display:contents;font-size:20px'><p id=b style=margin:0>x</p></div>", 400);
    for (const auto& b : r.page.boxes) {
        const std::string* id = b.node->attr("id");
        EXPECT_FALSE(id && *id == "a");
    }
    EXPECT_FLOAT_EQ(r.box("b").w, 400);
}

TEST(ShadowDom, ComposedTreeScopedStylesAndSlots) {
    const auto r = layout(R"(<style>p { height: 99px } .outer { height: 3px } x-card { display: block } .light { width: 50px }</style>
        <x-card id=card><template shadowrootmode=open>
          <style>:host { width: 300px; padding: 1px } p { height: 20px; margin: 0 } .inner { height: 5px }
                 ::slotted(span) { display: block; width: 77px; height: 7px } slot[name=title] { color: red }</style>
          <p id=sp>shadow</p><div id=si class=inner></div><slot name=title></slot><slot></slot></template>
          <span id=title slot=title></span><span id=light class=light></span><em id=unslotted slot=nowhere>gone</em></x-card>
        <p id=outer class=outer></p>)");
    // :host gave the host its size; the document's p rule didn't reach into the shadow tree.
    EXPECT_FLOAT_EQ(r.box("card").w, 302);
    EXPECT_FLOAT_EQ(r.box("sp").h, 20);
    EXPECT_FLOAT_EQ(r.box("si").y, 21);
    // ::slotted() styled the slotted spans; the document's own rule wins over it.
    EXPECT_FLOAT_EQ(r.box("title").w, 77);
    EXPECT_FLOAT_EQ(r.box("title").y, 26);
    EXPECT_FLOAT_EQ(r.box("light").w, 50);
    EXPECT_FLOAT_EQ(r.box("light").y, 33);
    // A child no slot takes isn't rendered; the shadow tree's p rule didn't leak out.
    for (const auto& b : r.page.boxes) EXPECT_FALSE(b.node->attr("id") && *b.node->attr("id") == "unslotted");
    EXPECT_FLOAT_EQ(r.box("outer").h, 3);
}

// A flex row sized to its content counts its gaps: without them its items shrank to
// fit, and one with overflow hidden lost the end of its text.
TEST(Flex, ContentSizedRowCountsItsGaps) {
    const auto r = layout("<div style=display:flex><div id=row style='display:flex;gap:8px'>"
                          "<div id=a style='min-width:0;overflow:hidden'>abcd</div><div id=b>efgh</div></div></div>");
    EXPECT_FLOAT_EQ(r.box("row").w, 48);
    EXPECT_FLOAT_EQ(r.box("a").w, 20);
    EXPECT_FLOAT_EQ(r.box("b").x, 28);
}

// text-overflow: ellipsis ends a clipped line in "…" (in the block's font), keeping
// the characters that fit with it; clip, the default, keeps the text as it is.
TEST(Inline, TextOverflowEllipsis) {
    const std::string style = "width:50px;white-space:nowrap;overflow:hidden";
    auto r = layout("<div style='" + style + ";text-overflow:ellipsis'>abcdefghijklmnop</div>");
    EXPECT_EQ(r.texts(), (std::vector<std::string>{"abcdefghi", "…"}));
    r = layout("<div style='" + style + ";text-overflow:ellipsis'>ab cd <b>efgh ijkl</b></div>");
    EXPECT_EQ(r.texts(), (std::vector<std::string>{"ab cd ", "efg", "…"}));
    r = layout("<div style='" + style + ";text-overflow:ellipsis'>short</div>");
    EXPECT_EQ(r.texts(), (std::vector<std::string>{"short"}));
    r = layout("<div style='" + style + "'>abcdefghijklmnop</div>");
    EXPECT_EQ(r.texts(), (std::vector<std::string>{"abcdefghijklmnop"}));
}

// Buttons are border-box, as in other browsers: a width is the whole button's.
TEST(Forms, ButtonsAreBorderBox) {
    const auto r = layout("<button id=b style='width:100px;height:30px'>x</button><input id=i type=submit style=width:80px>");
    EXPECT_FLOAT_EQ(r.box("b").w, 100);
    EXPECT_FLOAT_EQ(r.box("b").h, 30);
    EXPECT_FLOAT_EQ(r.box("i").w, 80);
}

TEST(Paint, TranslateMovesTheBoxAsPainted) {
    const auto r = layout(
        "<style>body{margin:0}</style>"
        "<div id=a style='width:100px;height:40px;transform:translate(10px,-50%)'></div>"
        "<div id=b style='width:50px;height:20px;transform:translateY(calc(5px + 10%)) translateX(-100%)'></div>"
        "<div id=c style='width:50px;height:20px;translate:4px 6px'></div>"
        "<div style='position:relative'><span id=d style='position:absolute;left:0;top:0;width:8px;height:8px;"
        "transform:matrix(1,0,0,1,3,7)'></span></div>");
    EXPECT_FLOAT_EQ(r.box("a").x, 10);
    EXPECT_FLOAT_EQ(r.box("a").y, -20);
    EXPECT_FLOAT_EQ(r.box("b").x, -50);
    EXPECT_FLOAT_EQ(r.box("b").y, 40 + 7);
    EXPECT_FLOAT_EQ(r.box("c").x, 4);
    EXPECT_FLOAT_EQ(r.box("c").y, 60 + 6);
    EXPECT_FLOAT_EQ(r.box("d").x, 3);
    EXPECT_FLOAT_EQ(r.box("d").y, 80 + 7);
}

TEST(Forms, BorderBoxInputsInFlexAndGridKeepTheirWidth) {
    const auto r = layout(
        "<style>*{box-sizing:border-box}body{margin:0;width:216px}input{width:100%;padding:5px 20px;border:1px solid}</style>"
        "<div style='display:grid;grid-template-columns:20px 1fr'><input id=g style='grid-column:1/-1'></div>"
        "<div style='display:flex'><input id=f></div>");
    EXPECT_FLOAT_EQ(r.box("g").x, 0);
    EXPECT_FLOAT_EQ(r.box("g").w, 216);
    EXPECT_FLOAT_EQ(r.box("f").w, 216);
}

TEST(Inline, DetailsShowsOnlyItsSummaryUntilOpen) {
    const auto closed = layout("<details><summary>Sum</summary><p>Body</p></details>");
    EXPECT_EQ(closed.texts(), (std::vector<std::string>{"\xE2\x96\xB8 Sum"}));
    const auto open = layout("<details open><summary>Sum</summary><p>Body</p></details>");
    EXPECT_EQ(open.texts(), (std::vector<std::string>{"\xE2\x96\xBE Sum", "Body"}));
    // A summary laid out as something else has no marker.
    const auto flex = layout("<details><summary style=display:flex>Sum</summary>x</details>");
    EXPECT_EQ(flex.texts(), (std::vector<std::string>{"Sum"}));
}

TEST(ShadowDom, PartRulesReachExposedElements) {
    const auto r = layout(R"(<style>x-b { display: block } .login::part(label) { width: 0; height: 0; overflow: hidden }
                                 x-b::part(icon box) { height: 9px } x-b::part(nope) { height: 50px }</style>
        <x-b id=host class=login><template shadowrootmode=open>
          <style>div { width: 40px; height: 4px }</style>
          <div id=label part=label>Log in</div><div id=icon part="box icon"></div><div id=plain></div></template></x-b>)");
    // The outer tree's ::part() rule beats the shadow tree's own.
    EXPECT_FLOAT_EQ(r.box("label").w, 0);
    EXPECT_FLOAT_EQ(r.box("label").h, 0);
    EXPECT_FLOAT_EQ(r.box("icon").h, 9);
    EXPECT_FLOAT_EQ(r.box("plain").h, 4);
}

TEST(Paint, RotationsAndScalesWrapWhatTheyPaint) {
    const auto r = layout("<style>body{margin:0}</style><div style='width:40px;height:20px;background:red;transform:rotate(90deg) scale(2)'></div>"
                          "<div style='width:10px;height:10px;background:blue;transform-origin:0 0;scale:3'></div><div style='transform:none'></div>");
    std::vector<PaintKind> kinds;
    const PaintOp* first = nullptr;
    const PaintOp* second = nullptr;
    for (const PaintOp& op : r.page.ops) {
        if (op.kind == PaintKind::TransformBegin) (first ? second : first) = &op;
        if (op.kind == PaintKind::TransformBegin || op.kind == PaintKind::TransformEnd || op.kind == PaintKind::Rect) kinds.push_back(op.kind);
    }
    EXPECT_EQ(kinds, (std::vector<PaintKind>{PaintKind::TransformBegin, PaintKind::Rect, PaintKind::TransformEnd,
                                             PaintKind::TransformBegin, PaintKind::Rect, PaintKind::TransformEnd}));
    ASSERT_TRUE(first && second);
    // About the centre (20, 10): the box's right middle (40, 10) goes 40 below it.
    const Point p = first->transform.apply({40, 10});
    EXPECT_NEAR(p.x, 20, 1e-3);
    EXPECT_NEAR(p.y, 50, 1e-3);
    // About the top left (0, 20): three times as far from it.
    const Point q = second->transform.apply({10, 30});
    EXPECT_NEAR(q.x, 30, 1e-3);
    EXPECT_NEAR(q.y, 50, 1e-3);
}

TEST(Flex, MinWidthZeroLetsAnItemShrinkBelowItsContent) {
    const auto r = layout(
        "<style>body{margin:0}.row{display:flex;width:100px}</style>"
        "<div class=row><div id=z style='min-width:0;display:flex'><span style='white-space:nowrap;overflow:hidden'>aaaa bbbb cccc dddd eeee</span></div></div>"
        "<div class=row><div id=a style='display:flex'><span style='white-space:nowrap'>aaaa bbbb cccc dddd eeee</span></div></div>");
    EXPECT_FLOAT_EQ(r.box("z").w, 100);
    EXPECT_GT(r.box("a").w, 100);   // auto: no smaller than its content
}

TEST(Floats, WhitespaceInsideAFloatIsntAFloat) {
    // Text shares its element's style; the float's float: left isn't the text's. Counted
    // as floats, the blank runs around the <p> each painted the float's background again.
    for (const char* pos : {"float:left", "position:absolute"}) {
        const auto r = layout(std::string("<div><div style='") + pos + ";width:190px;height:40px;background:red'> <p></p> </div></div>");
        int reds = 0;
        for (const PaintOp& op : r.page.ops)
            if (op.kind == PaintKind::Rect && op.color.r == 255 && op.color.g == 0) ++reds;
        EXPECT_EQ(reds, 1) << pos;
    }
}

TEST(Flex, ColumnItemsKeepTheirContentHeightWhateverTheirBasis) {
    const auto r = layout("<style>body{margin:0}</style><div style='display:flex;flex-direction:column;min-height:100px'>"
                          "<main id=m style='flex:1'><div style='height:250px'></div></main><footer id=f style='height:20px'></footer></div>");
    EXPECT_FLOAT_EQ(r.box("m").h, 250);
    EXPECT_FLOAT_EQ(r.box("f").y, 250);
}

TEST(Positioned, APictureKeepsItsSizeBetweenItsOffsets) {
    // CSS 2.1 10.3.8 / 10.6.5: the right and bottom offsets give way (Reddit's blurred backdrops).
    const auto r = layout("<div style='position:relative;overflow:hidden;width:100px;height:50px'>"
                          "<img id=i src=wide.png style='position:absolute;top:0;bottom:0;left:0;right:0'>"
                          "<div id=d style='position:absolute;top:0;bottom:0;left:0;right:0'></div></div>");
    EXPECT_FLOAT_EQ(r.box("i").w, 200);
    EXPECT_FLOAT_EQ(r.box("i").h, 100);
    EXPECT_FLOAT_EQ(r.box("d").w, 100);
    EXPECT_FLOAT_EQ(r.box("d").h, 50);
}

TEST(Replaced, ObjectFitPlacesAndCropsThePicture) {
    // wide.png is 200x100.
    const auto r = layout("<img id=c src=wide.png style='width:100px;height:100px;object-fit:cover'>"
                          "<img id=t src=wide.png style='width:100px;height:100px;object-fit:contain;object-position:0 100%'>"
                          "<img id=n src=wide.png style='width:100px;height:50px;object-fit:none;object-position:left top'>");
    std::vector<PaintOp> images;
    for (const auto& op : r.page.ops)
        if (op.kind == PaintKind::Image) images.push_back(op);
    ASSERT_EQ(images.size(), 3u);
    // cover: fills the box, the middle half of the picture.
    EXPECT_FLOAT_EQ(images[0].w, 100);
    EXPECT_FLOAT_EQ(images[0].h, 100);
    EXPECT_FLOAT_EQ(images[0].u0, 0.25f);
    EXPECT_FLOAT_EQ(images[0].u1, 0.75f);
    // contain: 100x50, at the bottom.
    EXPECT_FLOAT_EQ(images[1].w, 100);
    EXPECT_FLOAT_EQ(images[1].h, 50);
    EXPECT_FLOAT_EQ(images[1].y - r.box("t").y, 50);
    EXPECT_FLOAT_EQ(images[1].u1, 1);
    // none: the picture's own size, its top-left 100x50.
    EXPECT_FLOAT_EQ(images[2].w, 100);
    EXPECT_FLOAT_EQ(images[2].u1, 0.5f);
    EXPECT_FLOAT_EQ(images[2].v1, 0.5f);
}

TEST(Replaced, AVideoShowsItsPosterAtItsSize) {
    const auto r = layout("<video id=v poster=wide.png></video>");
    EXPECT_FLOAT_EQ(r.box("v").w, 200);
    EXPECT_FLOAT_EQ(r.box("v").h, 100);
    bool drawn = false;
    for (const auto& op : r.page.ops)
        if (op.kind == PaintKind::Image && op.text == "wide.png") drawn = true;
    EXPECT_TRUE(drawn);
}

TEST(Inline, TopAndBottomAlignedBoxesSitAtTheLineEdges) {
    const auto r = layout("<div><img id=t src=wide.png style='width:20px;height:20px;vertical-align:top'>"
                          "<img id=big src=wide.png style='width:50px;height:50px;vertical-align:top'>"
                          "<img id=b src=wide.png style='width:20px;height:20px;vertical-align:bottom'></div>");
    EXPECT_FLOAT_EQ(r.box("t").y, 0);
    EXPECT_FLOAT_EQ(r.box("big").y, 0);
    EXPECT_FLOAT_EQ(r.box("b").y, 30);   // its bottom on the bottom of a line the 50px box made
}

TEST(Inline, TextBottomSitsOnTheParentsTextBottom) {
    // FixedText: ascent 7.5, descent 2.5 at 10px. Not the same as bottom.
    const auto r = layout("<div><img id=tb src=wide.png style='width:20px;height:20px;vertical-align:text-bottom'>"
                          "<img id=big src=wide.png style='width:40px;height:40px'></div>");
    EXPECT_FLOAT_EQ(r.box("big").y, 0);
    EXPECT_FLOAT_EQ(r.box("tb").y + 20, 40 + 2.5f);   // bottom edge: baseline (40) + descent
}

TEST(Positioned, NegativeZIndexGoesUnderTheContent) {
    const auto r = layout("<div style='position:fixed;z-index:5;width:100px;height:20px'>"
                          "<div id=bg style='position:absolute;top:0;left:0;right:0;bottom:0;z-index:-1;background:#00f'></div>"
                          "<div id=c>text</div></div>");
    int bg = -1, text = -1;
    for (size_t i = 0; i < r.page.ops.size(); ++i) {
        const auto& op = r.page.ops[i];
        if (op.kind == PaintKind::Rect && op.color.b == 255 && op.color.r == 0 && bg < 0) bg = static_cast<int>(i);
        if (op.kind == PaintKind::Text && op.text == "text") text = static_cast<int>(i);
    }
    ASSERT_GE(bg, 0);
    ASSERT_GE(text, 0);
    EXPECT_LT(bg, text);   // painted first: under
    // And the text's box is what's hit there, not the background's.
    const ElementBox* last = nullptr;
    for (const auto& b : r.page.boxes)
        if (b.x <= 5 && b.y <= 5 && b.x + b.w > 5 && b.y + b.h > 5) last = &b;
    ASSERT_NE(last, nullptr);
    ASSERT_TRUE(last->node->attr("id"));
    EXPECT_EQ(*last->node->attr("id"), "c");
}

TEST(Paint, AGradientFadesWithItsBox) {
    const auto r = layout("<div style='opacity:0.5'><div style='width:10px;height:10px;background:linear-gradient(#f00,#00f)'></div></div>");
    const PaintOp* g = nullptr;
    for (const auto& op : r.page.ops)
        if (op.kind == PaintKind::Gradient) g = &op;
    ASSERT_NE(g, nullptr);
    ASSERT_FALSE(g->gradient->stops.empty());
    EXPECT_NEAR(g->gradient->stops.front().color.a, 128, 1);
}

TEST(InlineBlock, ShrinkToFitCountsFloatsBesideTextAndMinWidth) {
    // Daum's shortcuts: a floated item holding a link that has min-width, an icon floated
    // before its label, and a clearing ::after block.
    const auto r = layout("<style>a{display:block;padding:0 10px}a:after{content:'';display:block;clear:both}"
                          "i{float:left;width:30px;height:10px}</style>"
                          "<div id=f style='float:left'><a id=a><i></i> label</a></div>"
                          "<div id=g style='float:left'><a style='min-width:200px'><i></i> x</a></div>");
    const float label = r.box("a").w - 20;
    EXPECT_GT(label, 30 + 20);                 // the icon and the word, side by side
    EXPECT_FLOAT_EQ(r.box("f").w, label + 20);
    EXPECT_FLOAT_EQ(r.box("g").w, 220);       // min-width, and the padding outside it
}

TEST(Paint, ABlockPictureIsTranslatedOnce) {
    // Daum's thumbnails: a frame moved by half its size, the picture in it moved back.
    const auto r = layout("<div style='position:relative;width:160px;height:100px;margin:40px'>"
                          "<span style='position:absolute;top:0;left:0;right:0;bottom:0;transform:translate(50%,50%)'>"
                          "<img id=g width=160 height=100 style='position:absolute;top:0;left:0;transform:translate(-50%,-50%)'></span>"
                          "<img id=h width=20 height=20 style='display:block;transform:translateX(10px)'></div>");
    EXPECT_FLOAT_EQ(r.box("g").x, 40);
    EXPECT_FLOAT_EQ(r.box("g").y, 40);
    EXPECT_FLOAT_EQ(r.box("h").x, 50);
}
