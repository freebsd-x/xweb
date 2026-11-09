#include <gtest/gtest.h>

#include <string>

#include "xweb/html_parser.h"
#include "xweb/style.h"

using namespace fbx::xweb;

namespace {

struct Styled {
    std::unique_ptr<Document> doc;
    std::unique_ptr<StyledDocument> styles;

    const Node* byId(const std::string& id) const {
        std::vector<const Node*> todo{doc->root()};
        while (!todo.empty()) {
            const Node* n = todo.back();
            todo.pop_back();
            if (const std::string* v = n->attr("id"); v && *v == id) return n;
            todo.insert(todo.end(), n->children.begin(), n->children.end());
        }
        return nullptr;
    }
    const ComputedStyle& of(const std::string& id) const {
        const ComputedStyle* s = styles->styleOf(byId(id));
        EXPECT_NE(s, nullptr) << id;
        static const ComputedStyle none;
        return s ? *s : none;
    }
};

Styled style(const std::string& html, const StyledDocument::SheetLoader& loader = {}) {
    Styled s;
    s.doc = parseHtml("<!doctype html>" + html);
    s.styles = std::make_unique<StyledDocument>(*s.doc, css::MediaContext{800, 600}, loader);
    return s;
}

constexpr Color kRed{255, 0, 0, 255}, kBlue{0, 0, 255, 255};

}  // namespace

TEST(Cascade, SpecificityThenOrder) {
    const auto s = style("<style>#a{color:red} .b{color:blue} div{color:green} .c{color:red} .c{color:blue}</style>"
                         "<div id=a class=b></div><div id=d class=b></div><div id=e class=c></div>");
    EXPECT_EQ(s.of("a").color, kRed);
    EXPECT_EQ(s.of("d").color, kBlue);
    EXPECT_EQ(s.of("e").color, kBlue);
}

TEST(Cascade, ImportantInlineAndHints) {
    const auto s = style("<style>#a{color:red!important} #b{color:red} #c{background-color:red}</style>"
                         "<div id=a style='color:blue'></div><div id=b style='color:blue'></div>"
                         "<div id=c bgcolor=blue></div><div id=d bgcolor=blue></div>");
    EXPECT_EQ(s.of("a").color, kRed);
    EXPECT_EQ(s.of("b").color, kBlue);
    EXPECT_EQ(s.of("c").background, kRed);   // a hint loses to any author rule
    EXPECT_EQ(s.of("d").background, kBlue);
}

TEST(Cascade, Inheritance) {
    const auto s = style("<style>#p{color:red;font-size:20px;margin:7px;line-height:1.5} #k{color:blue;margin:inherit} #i{color:initial}</style>"
                         "<div id=p><span id=c></span><span id=k></span><span id=i></span></div>");
    EXPECT_EQ(s.of("c").color, kRed);
    EXPECT_FLOAT_EQ(s.of("c").font_size, 20);
    EXPECT_FLOAT_EQ(s.of("c").margin[Top].px, 0);       // not inherited
    EXPECT_FLOAT_EQ(s.of("k").margin[Top].px, 7);       // unless asked
    EXPECT_FLOAT_EQ(s.of("c").usedLineHeight(), 30);
    EXPECT_EQ(s.of("i").color, (Color{0, 0, 0, 255}));
}

TEST(Colors, Forms) {
    const auto c = [](const std::string& v) {
        Color out{1, 2, 3, 4};
        EXPECT_TRUE(parseColor(css::tokenize(v), Color{9, 9, 9, 255}, out)) << v;
        return out;
    };
    EXPECT_EQ(c("#f00"), kRed);
    EXPECT_EQ(c("#ff0000"), kRed);
    EXPECT_EQ(c("#ff000080"), (Color{255, 0, 0, 128}));
    EXPECT_EQ(c("red"), kRed);
    EXPECT_EQ(c("RED"), kRed);
    EXPECT_EQ(c("rgb(255,0,0)"), kRed);
    EXPECT_EQ(c("rgb(255 0 0)"), kRed);
    EXPECT_EQ(c("rgb(100%, 0%, 0%)"), kRed);
    EXPECT_EQ(c("rgba(255,0,0,.5)"), (Color{255, 0, 0, 128}));
    EXPECT_EQ(c("rgb(255 0 0 / 50%)"), (Color{255, 0, 0, 128}));
    EXPECT_EQ(c("hsl(0,100%,50%)"), kRed);
    EXPECT_EQ(c("hsl(120,100%,25%)"), (Color{0, 128, 0, 255}));
    EXPECT_EQ(c("transparent"), (Color{0, 0, 0, 0}));
    EXPECT_EQ(c("currentcolor"), (Color{9, 9, 9, 255}));
    Color out;
    EXPECT_FALSE(parseColor(css::tokenize("#12"), {}, out));
    EXPECT_FALSE(parseColor(css::tokenize("notacolor"), {}, out));
    EXPECT_FALSE(parseColor(css::tokenize("red blue"), {}, out));
}

TEST(Values, LengthsAndCalc) {
    const auto s = style("<style>html{font-size:10px}#a{font-size:20px;margin:1em 2rem 10% 1vw;width:calc(100% - 2em);height:calc((1px + 2px)*3)}"
                         "#b{width:min(100px,50px);height:max(1px,2px);margin:clamp(1px,5px,3px)}</style><div id=a></div><div id=b></div>");
    const ComputedStyle& a = s.of("a");
    EXPECT_FLOAT_EQ(a.margin[Top].px, 20);
    EXPECT_FLOAT_EQ(a.margin[Right].px, 20);
    EXPECT_FLOAT_EQ(a.margin[Bottom].percent, 10);
    EXPECT_FLOAT_EQ(a.margin[Left].px, 8);
    EXPECT_FLOAT_EQ(a.width.percent, 100);
    EXPECT_FLOAT_EQ(a.width.px, -40);
    EXPECT_FLOAT_EQ(a.width.resolve(800), 760);
    EXPECT_FLOAT_EQ(a.height.px, 9);
    EXPECT_FLOAT_EQ(s.of("b").width.px, 50);
    EXPECT_FLOAT_EQ(s.of("b").height.px, 2);
    EXPECT_FLOAT_EQ(s.of("b").margin[Top].px, 3);
}

TEST(Values, ShorthandsExpand) {
    const auto s = style("<style>#a{margin:1px 2px;padding:1px 2px 3px;border:4px solid red;border-left:none}"
                         "#b{font:italic bold 12px/30px serif}#c{background:blue url(x.png) no-repeat}#d{border-width:1px;border-style:solid}"
                         "#e{border-style:solid;border-width:0}#f{border-width:5px}</style>"
                         "<div id=a></div><div id=b></div><div id=c></div><div id=d></div><div id=e></div><div id=f></div>");
    const ComputedStyle& a = s.of("a");
    EXPECT_FLOAT_EQ(a.margin[Left].px, 2);
    EXPECT_FLOAT_EQ(a.padding[Bottom].px, 3);
    EXPECT_FLOAT_EQ(a.padding[Left].px, 2);
    EXPECT_FLOAT_EQ(a.border_width[Top], 4);
    EXPECT_FLOAT_EQ(a.border_width[Left], 0);
    EXPECT_EQ(a.border_color[Top], kRed);
    const ComputedStyle& b = s.of("b");
    EXPECT_TRUE(b.italic);
    EXPECT_EQ(b.font_weight, 700);
    EXPECT_FLOAT_EQ(b.font_size, 12);
    EXPECT_FLOAT_EQ(b.usedLineHeight(), 30);
    EXPECT_EQ(b.font_family, "serif");
    EXPECT_EQ(s.of("c").background, kBlue);
    EXPECT_FLOAT_EQ(s.of("d").border_width[Top], 1);
    EXPECT_FLOAT_EQ(s.of("e").border_width[Top], 0);
    EXPECT_FLOAT_EQ(s.of("f").border_width[Top], 0);   // a width alone, with no style, draws nothing
}

TEST(Values, InvalidDeclarationsAreIgnored) {
    const auto s = style("<style>#a{color:red;color:nonsense;width:10px;width:ten;margin:1px 2px 3px 4px 5px}</style><div id=a></div>");
    EXPECT_EQ(s.of("a").color, kRed);
    EXPECT_FLOAT_EQ(s.of("a").width.px, 10);
    EXPECT_FLOAT_EQ(s.of("a").margin[Top].px, 0);
}

TEST(CustomProperties, ResolveAndInherit) {
    const auto s = style("<style>:root{--c:#f00;--w:10px;--d:var(--w)}#a{color:var(--c);width:var(--d);height:var(--none,5px);margin:var(--none)}"
                         "#b{--c:blue;color:var(--c)}</style><div id=a><i id=k></i></div><div id=b></div>");
    EXPECT_EQ(s.of("a").color, kRed);
    EXPECT_FLOAT_EQ(s.of("a").width.px, 10);
    EXPECT_FLOAT_EQ(s.of("a").height.px, 5);
    EXPECT_EQ(s.of("k").color, kRed);
    EXPECT_EQ(s.of("b").color, kBlue);
}

TEST(CustomProperties, CyclesAndExpansionBombsAreHarmless) {
    std::string css = "<style>:root{--a:var(--b);--b:var(--a);--x0:1px";
    for (int i = 1; i < 40; ++i)
        css += ";--x" + std::to_string(i) + ":var(--x" + std::to_string(i - 1) + ") var(--x" + std::to_string(i - 1) + ")";
    css += "}#a{width:var(--a);height:var(--x39);margin:1px}</style><div id=a></div>";
    const auto s = style(css);
    EXPECT_TRUE(s.of("a").width.is_auto);
    EXPECT_TRUE(s.of("a").height.is_auto);
    EXPECT_FLOAT_EQ(s.of("a").margin[Top].px, 1);
}

TEST(CustomProperties, InvalidVariablesTakeTheFallback) {
    // initial unsets a variable; one referring to a missing variable is invalid itself;
    // an empty one is valid (the space toggle light-dark() polyfills use).
    const auto s = style("<style>:root{--off:initial;--on: ;--t1:var(--off) blue;--t2:var(--on) blue;--t3:var(--nope) 1px}"
                         "#a{color:var(--t1,red);background-color:var(--t2,red);width:var(--t3,7px)}"
                         "#b{--on:inherit;--t2:var(--on) blue;color:var(--t2,red)}</style><div id=a></div><div id=b></div>");
    EXPECT_EQ(s.of("a").color, kRed);
    EXPECT_EQ(s.of("a").background, kBlue);
    EXPECT_FLOAT_EQ(s.of("a").width.px, 7);
    EXPECT_EQ(s.of("b").color, kBlue);
}

TEST(CustomProperties, FailedFallbacksDontMultiply) {
    std::string css = "<style>:root{--v0:var(--missing)";
    for (int i = 1; i < 40; ++i)
        css += ";--v" + std::to_string(i) + ":var(--v" + std::to_string(i - 1) + ",var(--v" + std::to_string(i - 1) + "))";
    css += "}#a{width:var(--v39,3px)}</style><div id=a></div>";
    const auto s = style(css);
    EXPECT_FLOAT_EQ(s.of("a").width.px, 3);
}

TEST(Selectors, HostileSelectorsStopEarly) {
    std::string css = "<style>";
    for (int i = 0; i < 40; ++i) css += "div ";
    css += "#no{color:red}</style>";
    std::string body;
    for (int i = 0; i < 400; ++i) body += "<div>";
    body += "<span id=a></span>";
    const auto s = style(css + body);
    EXPECT_NE(s.styles->styleOf(s.byId("a")), nullptr);
}

TEST(Sheets, LinksImportsAndMedia) {
    const auto loader = [](const std::string& href) -> std::string {
        if (href == "a.css") return "@import 'b.css'; #a{color:red}";
        if (href == "b.css") return "#b{color:blue} @import 'late.css';";
        if (href == "loop.css") return "@import 'loop.css'; #c{color:red}";
        return {};
    };
    const auto s = style("<link rel=stylesheet href=a.css><link rel=stylesheet href=loop.css><link rel=stylesheet media=print href=a.css>"
                         "<link rel='alternate stylesheet' href=b.css><style media=print>#d{color:red}</style>"
                         "<div id=a></div><div id=b></div><div id=c></div><div id=d></div>", loader);
    EXPECT_EQ(s.of("a").color, kRed);
    EXPECT_EQ(s.of("b").color, kBlue);
    EXPECT_EQ(s.of("c").color, kRed);
    EXPECT_EQ(s.of("d").color, (Color{0, 0, 0, 255}));
}

TEST(Display, UaSheetAndNone) {
    const auto s = style("<div id=d><span id=sp></span><li id=li></li><p id=hid hidden></p></div><div style=display:none><b id=under></b></div>");
    EXPECT_EQ(s.of("d").display, Display::Block);
    EXPECT_EQ(s.of("sp").display, Display::Inline);
    EXPECT_EQ(s.of("li").display, Display::ListItem);
    EXPECT_EQ(s.of("hid").display, Display::None);
    EXPECT_EQ(s.styles->styleOf(s.byId("under")), nullptr);
}

TEST(FontSize, MonospaceKeywordsAreSmaller) {
    const auto s = style("<style>#m{font-family:monospace}#n{font-family:monospace;font-size:large}#x{font-family:monospace;font-size:20px}"
                         "#o{font-size:large}</style><div id=m></div><div id=n></div><div id=x></div><div id=o></div><pre id=p></pre>");
    EXPECT_FLOAT_EQ(s.of("m").font_size, 13);
    EXPECT_FLOAT_EQ(s.of("n").font_size, 13 * 18.0f / 16);
    EXPECT_FLOAT_EQ(s.of("x").font_size, 20);
    EXPECT_FLOAT_EQ(s.of("o").font_size, 18);
    EXPECT_FLOAT_EQ(s.of("p").font_size, 13);
}

TEST(Sheets, ImportsAreRelativeToTheirSheet) {
    const auto loader = [](const std::string& href) -> std::string {
        if (href == "css/a.css") return "@import 'b.css';";
        if (href == "css/b.css") return "#a{color:red}";
        return {};
    };
    EXPECT_EQ(style("<link rel=stylesheet href=css/a.css><div id=a></div>", loader).of("a").color, kRed);
}

TEST(Hints, OldPresentationalAttributes) {
    const auto s = style("<img id=i align=right hspace=3><table id=t cellspacing=7 cellpadding=5><tr><td id=c valign=top nowrap>x</td></tr></table>");
    EXPECT_EQ(s.of("i").float_side, Float::Right);
    EXPECT_FLOAT_EQ(s.of("i").margin[Left].px, 3);
    EXPECT_FLOAT_EQ(s.of("t").border_spacing, 7);
    EXPECT_FLOAT_EQ(s.of("c").padding[Top].px, 5);
    EXPECT_EQ(s.of("c").vertical_align, VerticalAlign::Top);
    EXPECT_EQ(s.of("c").white_space, WhiteSpace::NoWrap);
}

TEST(FontFamily, KeepsTheWholeList) {
    const auto s = style("<div id=a style='font-family:\"No Such\", Other Name ,serif'></div><div id=b></div>");
    EXPECT_EQ(s.of("a").font_family, "No Such,Other Name,serif");
    EXPECT_EQ(s.of("b").font_family, "sans-serif");
}

TEST(Pseudo, GetTheirOwnStyle) {
    const auto s = style("<style>#a::before{content:'x';color:red;display:block}#b::after{color:red}</style><div id=a></div><div id=b></div>");
    const ComputedStyle* before = s.styles->beforeOf(s.byId("a"));
    ASSERT_NE(before, nullptr);
    EXPECT_EQ(before->color, kRed);
    EXPECT_EQ(before->display, Display::Block);
    EXPECT_EQ(before->content, "x");
    EXPECT_EQ(s.styles->afterOf(s.byId("b")), nullptr);   // no content, no box
}

namespace {
std::vector<WebFontFace> facesOf(const std::string& css, float width = 800) {
    const auto doc = parseHtml("<!doctype html><style>" + css + "</style><p>x");
    const StyledDocument styles(*doc, {width, 600});
    return styles.fontFaces();
}
}  // namespace

TEST(FontFace, ReadsItsDescriptors) {
    const auto f = facesOf("@font-face{font-family:\"My Font\";font-weight:100 900;font-style:italic;"
                           "src:local(x),url(a.eot) format('embedded-opentype'),url(b.svg),url(c.woff2?v=1) format('woff2'),url('d.ttf')}");
    ASSERT_EQ(f.size(), 1u);
    EXPECT_EQ(f[0].family, "my font");
    EXPECT_EQ(f[0].weight_min, 100);
    EXPECT_EQ(f[0].weight_max, 900);
    EXPECT_TRUE(f[0].italic);
    EXPECT_EQ(f[0].sources, (std::vector<std::string>{"c.woff2?v=1", "d.ttf"}));
}

TEST(FontFace, UnicodeRanges) {
    const auto f = facesOf("@font-face{font-family:a;src:url(a);unicode-range:U+26,u+0-7F,U+0025-00FF,U+4??,U+110000-120000,bad}");
    ASSERT_EQ(f.size(), 1u);
    const std::vector<std::pair<uint32_t, uint32_t>> want{{0x26, 0x26}, {0, 0x7F}, {0x25, 0xFF}, {0x400, 0x4FF}};
    EXPECT_EQ(f[0].ranges, want);
}

TEST(FontFace, WhatCantBeUsedIsDropped) {
    EXPECT_TRUE(facesOf("@font-face{src:url(a)}").empty());
    EXPECT_TRUE(facesOf("@font-face{font-family:a}").empty());
    EXPECT_TRUE(facesOf("@font-face{font-family:a;src:local(b)}").empty());
    EXPECT_TRUE(facesOf("@font-face{font-family:a;src:url(a.eot)}").empty());
}

TEST(FontFace, ObeysMedia) {
    const std::string css = "@media (max-width:500px){@font-face{font-family:a;src:url(a)}}";
    EXPECT_TRUE(facesOf(css, 800).empty());
    EXPECT_EQ(facesOf(css, 400).size(), 1u);
}

TEST(FontFace, ManyAreCapped) {
    std::string css;
    for (int i = 0; i < 2000; ++i) css += "@font-face{font-family:a;src:url(a)}";
    EXPECT_LE(facesOf(css).size(), 512u);
}

TEST(Color, LightDarkTakesTheLight) {
    Color c;
    EXPECT_TRUE(parseColor(css::tokenize("light-dark(#102030, rgb(1,2,3))"), Color{}, c));
    EXPECT_EQ(c, (Color{0x10, 0x20, 0x30, 255}));
    EXPECT_TRUE(parseColor(css::tokenize("light-dark(rgb(1,2,3), red)"), Color{}, c));
    EXPECT_EQ(c, (Color{1, 2, 3, 255}));
    EXPECT_FALSE(parseColor(css::tokenize("light-dark(red"), Color{}, c));
    EXPECT_FALSE(parseColor(css::tokenize("light-dark(light-dark(red,blue),blue)"), Color{}, c));
}

// The colour stays: layout draws it through the mask's image (Layout.MaskTintsTheBackgroundColour).
TEST(Mask, AMaskedColourIsKeptForTheMask) {
    const auto doc = parseHtml("<!doctype html><div id=a style='background:red;mask-image:url(x.svg)'></div><div id=b style='background:red;mask:none'></div><div id=c style='background:red'></div>");
    const StyledDocument styles(*doc, {800, 600});
    const auto find = [&](const char* id) {
        std::vector<const Node*> todo{doc->root()};
        while (!todo.empty()) {
            const Node* n = todo.back();
            todo.pop_back();
            if (const std::string* v = n->attr("id"); v && *v == id) return n;
            todo.insert(todo.end(), n->children.begin(), n->children.end());
        }
        return static_cast<const Node*>(nullptr);
    };
    EXPECT_TRUE(styles.styleOf(find("a"))->masked);
    EXPECT_TRUE(styles.styleOf(find("a"))->background.visible());
    EXPECT_FALSE(styles.styleOf(find("b"))->masked);
    EXPECT_TRUE(styles.styleOf(find("b"))->background.visible());
    EXPECT_TRUE(styles.styleOf(find("c"))->background.visible());
}
