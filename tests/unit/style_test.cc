#include <gtest/gtest.h>

#include <string>

#include "xweb/html_parser.h"
#include "xweb/forms.h"
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

TEST(CustomProperties, LayersThroughDeepNestingAndResets) {
    // Each level sets one more over what it has (layered, flattened past a few levels);
    // initial clears a name for the subtree, inherit brings the parent's back.
    std::string html = "<style>:root{--w:1px;--h:2px}";
    for (int i = 0; i < 20; ++i) html += ".l" + std::to_string(i) + "{--v" + std::to_string(i) + ":" + std::to_string(i + 3) + "px}";
    html += ".l12{--w:7px}.l15{--h:initial}#t{width:var(--w);height:var(--h,9px);margin-left:var(--v0);margin-right:var(--v19)}"
            "#u{--w:inherit;width:var(--w)}#s{--q:3px}#s::before{content:'';--w:4px}</style>";
    for (int i = 0; i < 20; ++i) html += "<div class=l" + std::to_string(i) + ">";
    html += "<div id=t><div id=u></div></div>";
    for (int i = 0; i < 20; ++i) html += "</div>";
    html += "<div id=s style='width:var(--w)'></div>";
    const auto s = style(html);
    EXPECT_FLOAT_EQ(s.of("t").width.px, 7);
    EXPECT_FLOAT_EQ(s.of("t").height.px, 9);
    EXPECT_FLOAT_EQ(s.of("t").margin[Left].px, 3);
    EXPECT_FLOAT_EQ(s.of("t").margin[Right].px, 22);
    EXPECT_FLOAT_EQ(s.of("u").width.px, 7);
    EXPECT_FLOAT_EQ(s.of("s").width.px, 1);   // its ::before's --w is its own
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

TEST(Sheets, AStylePreloadIsAskedForButNotApplied) {
    // Fetched for its "load" (a script's preload: React's), its rules not the page's yet.
    std::vector<std::string> asked;
    const auto loader = [&asked](const std::string& href) -> std::string {
        asked.push_back(href);
        return "#a{color:red}";
    };
    const auto s = style("<link rel=preload as=style href=p.css><link rel=preload as=font href=f.woff2><div id=a></div>", loader);
    EXPECT_EQ(asked, std::vector<std::string>{"p.css"});
    EXPECT_EQ(s.of("a").color, (Color{0, 0, 0, 255}));
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

TEST(Mask, TwoOpaqueLayersThatExcludeEachOtherAreARing) {
    // The gradient-border idiom: one layer on the content box, one on the border box, xor.
    const auto doc = parseHtml("<!doctype html>"
        "<div id=a style='-webkit-mask-image:linear-gradient(#fff 0 0),linear-gradient(#fff 0 0);-webkit-mask-clip:content-box,border-box;-webkit-mask-composite:xor'></div>"
        "<div id=b style='mask:linear-gradient(#000 0 0) padding-box,linear-gradient(#000 0 0);mask-composite:exclude'></div>"
        "<div id=c style='-webkit-mask-image:linear-gradient(#fff 0 0),linear-gradient(#fff 0 0);-webkit-mask-clip:content-box,border-box'></div>"
        "<div id=d style='mask-image:linear-gradient(#fff,transparent),linear-gradient(#fff 0 0);mask-clip:content-box,border-box;mask-composite:exclude'></div>");
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
    EXPECT_EQ(styles.styleOf(find("a"))->mask_ring, 1 + static_cast<int>(BackgroundBox::Content));
    EXPECT_EQ(styles.styleOf(find("b"))->mask_ring, 1 + static_cast<int>(BackgroundBox::Padding));
    EXPECT_EQ(styles.styleOf(find("c"))->mask_ring, 0);   // added, not excluded: all of it shows
    EXPECT_EQ(styles.styleOf(find("d"))->mask_ring, 0);   // a fading layer isn't a ring
}

TEST(Values, LogicalPropertiesAreTheirPhysicalOnes) {
    const auto s = style("<style>#a{margin-left:0;margin-inline-start:20px;padding-block-end:3px;inline-size:50px;max-block-size:9px;"
                         "border-inline-end:2px solid red;inset-inline-start:4px;inset-block:1px 2px}#b{inset:5px}</style><div id=a></div><div id=b></div>");
    EXPECT_FLOAT_EQ(s.of("a").margin[Left].px, 20);
    EXPECT_FLOAT_EQ(s.of("a").padding[Bottom].px, 3);
    EXPECT_FLOAT_EQ(s.of("a").width.px, 50);
    EXPECT_FLOAT_EQ(s.of("a").max_height.px, 9);
    EXPECT_FLOAT_EQ(s.of("a").border_width[Right], 2);
    EXPECT_FLOAT_EQ(s.of("a").offset[Left].px, 4);
    EXPECT_FLOAT_EQ(s.of("a").offset[Top].px, 1);
    EXPECT_FLOAT_EQ(s.of("a").offset[Bottom].px, 2);
    EXPECT_FLOAT_EQ(s.of("b").offset[Right].px, 5);
}

TEST(Sheets, ImportsKeepToTheirConditions) {
    const auto loader = [](const std::string& href) -> std::string {
        if (href == "all.css") return "@import url(light.css); @import url('dark.css') (prefers-color-scheme: dark);"
                                      "@import \"print.css\" print; @import 'layered.css' layer(base) screen;"
                                      "@import 'nosupport.css' supports(display: nonsense-value);";
        if (href == "light.css") return "#a{color:red}";
        if (href == "dark.css") return "#a{color:blue}";
        if (href == "print.css") return "#b{color:blue}";
        if (href == "layered.css") return "#c{color:red}";
        if (href == "nosupport.css") return "#c{color:blue}";
        return {};
    };
    const auto s = style("<link rel=stylesheet href=all.css><div id=a></div><div id=b></div><div id=c></div>", loader);
    EXPECT_EQ(s.of("a").color, kRed);
    EXPECT_EQ(s.of("b").color, (Color{0, 0, 0, 255}));
    EXPECT_EQ(s.of("c").color, kRed);
}

TEST(Display, NoscriptHidesOnlyWhereScriptsRun) {
    const auto with = parseHtml("<p><noscript id=n><b>no js</b></noscript></p>", ParseOptions{.scripting = true});
    const StyledDocument styled(*with, css::MediaContext{800, 600});
    const Node* n = nullptr;
    std::vector<const Node*> todo{with->root()};
    while (!todo.empty()) {
        const Node* c = todo.back();
        todo.pop_back();
        if (const std::string* id = c->attr("id"); id && *id == "n") n = c;
        for (const Node* k : c->children) todo.push_back(k);
    }
    ASSERT_TRUE(n);
    ASSERT_TRUE(styled.styleOf(n));
    EXPECT_EQ(styled.styleOf(n)->display, Display::None);
    const auto s = style("<p><noscript id=n><b>no js</b></noscript></p>");   // no scripting: shown
    EXPECT_NE(s.of("n").display, Display::None);
}

TEST(Sheets, RulesInsertedIntoAStyleElementApply) {
    auto doc = parseHtml("<style id=s>#a{color:red}</style><div id=a></div><div id=b></div>");
    Node* st = nullptr;
    std::vector<Node*> todo{doc->root()};
    while (!todo.empty()) {
        Node* n = todo.back();
        todo.pop_back();
        if (const std::string* id = n->attr("id"); id && *id == "s") st = n;
        for (Node* c : n->children) todo.push_back(c);
    }
    ASSERT_TRUE(st);
    st->adopted_css = {std::make_shared<const std::string>("#b{color:blue}"), std::make_shared<const std::string>("#a{color:blue}")};
    const StyledDocument styled(*doc, css::MediaContext{800, 600});
    const auto colorOf = [&](const char* id) {
        std::vector<const Node*> t{doc->root()};
        while (!t.empty()) {
            const Node* n = t.back();
            t.pop_back();
            if (const std::string* v = n->attr("id"); v && *v == id) return styled.styleOf(n)->color;
            for (const Node* c : n->children) t.push_back(c);
        }
        return Color{};
    };
    EXPECT_EQ(colorOf("b"), kBlue);
    EXPECT_EQ(colorOf("a"), kBlue);   // after the element's own text
}

TEST(Selectors, CheckedFollowsTheControlsState) {
    auto doc = parseHtml("<style>:checked + b{color:red} option:checked{color:blue}</style>"
                         "<input type=radio name=g id=r1 checked><b id=a></b><input type=radio name=g id=r2><b id=b></b>"
                         "<select id=s><option id=o1 selected>1<option id=o2>2</select>");
    const auto find = [&](const char* id) {
        std::vector<Node*> todo{doc->root()};
        while (!todo.empty()) {
            Node* n = todo.back();
            todo.pop_back();
            if (const std::string* v = n->attr("id"); v && *v == id) return n;
            for (Node* c : n->children) todo.push_back(c);
        }
        return static_cast<Node*>(nullptr);
    };
    FormState forms;
    forms.setChecked(find("r2"), true);   // as a click or a script would
    forms.select(find("s"), 1);
    const StyledDocument styled(*doc, css::MediaContext{800, 600}, {}, {}, &forms);
    EXPECT_NE(styled.styleOf(find("a"))->color, kRed);
    EXPECT_EQ(styled.styleOf(find("b"))->color, kRed);
    EXPECT_EQ(styled.styleOf(find("o2"))->color, kBlue);
    EXPECT_NE(styled.styleOf(find("o1"))->color, kBlue);
}

TEST(Pseudo, PlaceholderHasItsOwnStyle) {
    auto doc = parseHtml("<style>.t::placeholder{color:transparent} .o::placeholder{opacity:0} .f::placeholder{font-style:italic}</style>"
                         "<input id=a placeholder=x class=t><textarea id=b placeholder=y class=o></textarea><input id=c placeholder=z class=f><input id=d>");
    StyledDocument styles(*doc, css::MediaContext{800, 600});
    const auto byId = [&](const char* id) {
        const Node* found = nullptr;
        std::vector<const Node*> todo{doc->root()};
        while (!todo.empty() && !found) {
            const Node* n = todo.back();
            todo.pop_back();
            if (const std::string* v = n->attr("id"); v && *v == id) found = n;
            for (const Node* k : n->children) todo.push_back(k);
        }
        return found;
    };
    ASSERT_NE(styles.placeholderOf(byId("a")), nullptr);
    EXPECT_EQ(styles.placeholderOf(byId("a"))->color.a, 0);
    EXPECT_FLOAT_EQ(styles.placeholderOf(byId("b"))->opacity, 0);
    EXPECT_EQ(styles.placeholderOf(byId("c"))->color.r, 0x75);   // the UA's grey stays
    EXPECT_EQ(styles.placeholderOf(byId("d")), nullptr);          // no placeholder, no style
}

TEST(Filter, FunctionsInOrder) {
    const auto s = style("<div id=a style='filter: blur(6px) grayscale(50%) invert() hue-rotate(90deg) brightness(2)'></div>"
                         "<div id=b style='filter: none'></div><div id=c style='filter: blur(2px) nonsense(1)'></div>"
                         "<div id=d style='filter: drop-shadow(0 0 2px red) sepia(3)'></div>");
    const auto& a = s.of("a");
    ASSERT_TRUE(a.filter);
    ASSERT_EQ(a.filter->size(), 5u);
    EXPECT_EQ((*a.filter)[0].kind, Filter::Kind::Blur);
    EXPECT_FLOAT_EQ((*a.filter)[0].amount, 6);
    EXPECT_FLOAT_EQ((*a.filter)[1].amount, 0.5f);
    EXPECT_FLOAT_EQ((*a.filter)[2].amount, 1);
    EXPECT_FLOAT_EQ((*a.filter)[3].amount, 90);
    EXPECT_FLOAT_EQ((*a.filter)[4].amount, 2);   // brightness may go past 100%
    EXPECT_FALSE(s.of("b").filter);
    EXPECT_FALSE(s.of("c").filter);               // a function it doesn't know drops the declaration
    ASSERT_TRUE(s.of("d").filter);
    ASSERT_EQ(s.of("d").filter->size(), 1u);      // drop-shadow skipped; sepia clamped
    EXPECT_FLOAT_EQ((*s.of("d").filter)[0].amount, 1);
}

TEST(Selectors, HasLooksInsideAndAfter) {
    auto doc = parseHtml("<style>div{color:#000} .a:has(> i){color:#f00} .b:has(em b){color:#0f0} .c:has(+ p){color:#00f}"
                         " .d:has(~ .x, nope){color:#fff} .e:has(i){color:#f0f}</style>"
                         "<div id=a class=a><i></i></div><div id=a2 class=a><span><i></i></span></div>"
                         "<div id=b class=b><em><span><b></b></span></em></div>"
                         "<div id=c class=c></div><p></p><div id=d class=d></div><span></span><span class=x></span>"
                         "<div id=e class=e></div><i></i>");
    StyledDocument styles(*doc, css::MediaContext{800, 600});
    const auto color = [&](const char* id) {
        const Node* found = nullptr;
        std::vector<const Node*> todo{doc->root()};
        while (!todo.empty() && !found) {
            const Node* n = todo.back();
            todo.pop_back();
            if (const std::string* v = n->attr("id"); v && *v == id) found = n;
            for (const Node* k : n->children) todo.push_back(k);
        }
        const Color c = styles.styleOf(found)->color;
        return (c.r << 16) | (c.g << 8) | c.b;
    };
    EXPECT_EQ(color("a"), 0xff0000);
    EXPECT_EQ(color("a2"), 0);          // not a child
    EXPECT_EQ(color("b"), 0x00ff00);
    EXPECT_EQ(color("c"), 0x0000ff);
    EXPECT_EQ(color("d"), 0xffffff);
    EXPECT_EQ(color("e"), 0);           // its <i> is a sibling, not inside
}

TEST(Selectors, NestedRulesApply) {
    // velog.io's CSS modules: .Header_block{.Header_inner{display:flex}} and the like.
    auto doc = parseHtml("<style>.a{color:#f00; .b{color:#0f0} &.c{color:#00f} > i{color:#fff} &:hover{color:#123}"
                         " @media (min-width: 100px){color:#0ff; .d{color:#f0f}} @media (max-width: 10px){.e{color:#ff0}} margin-left:3px}"
                         ".x, .y{ .z{color:#808080} }</style>"
                         "<div id=a class=a><p id=b class=b></p><i id=i></i><p id=e class=e></p></div><div id=c class='a c'></div>"
                         "<div id=d class=a><p class=d id=dd></p></div><div class=y><b id=z class=z></b></div>");
    StyledDocument styles(*doc, css::MediaContext{800, 600});
    const auto color = [&](const char* id) {
        std::vector<const Node*> todo{doc->root()};
        const Node* found = nullptr;
        while (!todo.empty() && !found) {
            const Node* n = todo.back();
            todo.pop_back();
            if (const std::string* v = n->attr("id"); v && *v == id) found = n;
            for (const Node* k : n->children) todo.push_back(k);
        }
        const Color c = styles.styleOf(found)->color;
        return (c.r << 16) | (c.g << 8) | c.b;
    };
    EXPECT_EQ(color("a"), 0x00ffff);   // the nested @media's declaration, later than color:#f00
    EXPECT_EQ(color("b"), 0x00ff00);
    EXPECT_EQ(color("c"), 0x0000ff);
    EXPECT_EQ(color("i"), 0xffffff);
    EXPECT_EQ(color("dd"), 0xff00ff);
    EXPECT_EQ(color("e"), 0x00ffff);   // the @media that doesn't hold: inherits a's
    EXPECT_EQ(color("z"), 0x808080);
    EXPECT_FLOAT_EQ(styles.styleOf(doc->root()->children.back()->children.back()->children.front())->margin[3].px, 3);   // after the nested rules
}

// Styles kept from the last time (StyledDocument's `previous`) must be what styling
// afresh gives, whatever changed: an element's own attributes, a sibling, an ancestor,
// something a :has() looks for, the children an :empty or :first-child sees.
namespace {

void expectSameAsFresh(const Document& doc, const StyledDocument& kept, const char* step) {
    const StyledDocument fresh(doc, css::MediaContext{800, 600});
    std::vector<const Node*> todo{doc.root()};
    while (!todo.empty()) {
        const Node* n = todo.back();
        todo.pop_back();
        todo.insert(todo.end(), n->children.begin(), n->children.end());
        if (!n->isElement()) continue;
        const ComputedStyle* a = kept.styleOf(n);
        const ComputedStyle* b = fresh.styleOf(n);
        ASSERT_EQ(a == nullptr, b == nullptr) << step << " <" << n->name << ">";
        if (!a) continue;
        const std::string* id = n->attr("id");
        EXPECT_EQ(a->color, b->color) << step << " #" << (id ? *id : n->name);
        EXPECT_EQ(a->display, b->display) << step << " #" << (id ? *id : n->name);
        EXPECT_EQ(a->font_size, b->font_size) << step << " #" << (id ? *id : n->name);
    }
}

Node* elementById(Document& doc, const std::string& id) {
    std::vector<Node*> todo{doc.root()};
    while (!todo.empty()) {
        Node* n = todo.back();
        todo.pop_back();
        if (const std::string* v = n->attr("id"); v && *v == id) return n;
        todo.insert(todo.end(), n->children.begin(), n->children.end());
    }
    return nullptr;
}

void setAttribute(Node* n, const std::string& name, const std::string& value) {
    for (auto& a : n->attrs)
        if (a.name == name) {
            a.value = value;
            return;
        }
    n->attrs.push_back({name, value});
}

}  // namespace

TEST(Restyle, KeptStylesMatchStylingAfresh) {
    auto doc = parseHtml(R"(<!doctype html><style>
        .on { color: rgb(200, 0, 0) }
        .on p { color: rgb(0, 0, 200); font-size: 30px }
        li + li.x { color: rgb(0, 150, 0) }
        li:first-child { font-size: 11px }
        div:has(.flag) { color: rgb(1, 2, 3) }
        span:empty { display: block }
        .hide { display: none }
        html { font-size: 10px }
        .rem { font-size: 2rem }
      </style>
      <div id=box><section><p id=para>text</p></section></div>
      <ul id=list><li id=a>a</li><li id=b>b</li><li id=c>c</li></ul>
      <div id=hasbox><em><span id=deep>x</span></em></div>
      <span id=e></span>)");
    auto styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600});
    const auto again = [&](const char* step) {
        styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600}, StyledDocument::SheetLoader{},
                                                  StyledDocument::ChMeasure{}, nullptr, styles.get());
        expectSameAsFresh(*doc, *styles, step);
    };
    again("nothing changed");
    setAttribute(elementById(*doc, "e"), "class", "rem");
    again("a rem font size, the root's style kept");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "e"))->font_size, 20);
    setAttribute(elementById(*doc, "box"), "class", "on");
    again("an ancestor's class");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "para"))->color, (Color{0, 0, 200, 255}));
    setAttribute(elementById(*doc, "b"), "class", "x");
    again("a sibling combinator's subject");
    Node* list = elementById(*doc, "list");
    list->remove(elementById(*doc, "a"));
    again("a first child removed");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "b"))->font_size, 11);
    setAttribute(elementById(*doc, "deep"), "class", "flag");
    again("what a :has() looks for, deep inside");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "hasbox"))->color, (Color{1, 2, 3, 255}));
    Node* text = doc->make(NodeType::Text);
    text->data = "now not empty";
    elementById(*doc, "e")->append(text);
    again("an :empty element given text");
    setAttribute(elementById(*doc, "box"), "class", "on hide");
    again("an ancestor hidden");
    setAttribute(elementById(*doc, "box"), "class", "");
    again("and shown again");
    for (int i = 0; i < 12; ++i) {
        setAttribute(elementById(*doc, "c"), "class", i % 2 ? "x" : "");
        again("many generations of kept styles");
    }
}

TEST(Values, LightDarkAnywhereIsTheLightOne) {
    auto s = style(R"(<style>
        :root { --bar: light-dark(linear-gradient(90deg, #00a69c, #28b472), #000) }
        #g { background: var(--bar) }
        #c { background-color: light-dark(rgb(1, 2, 3), black) }
        #w { width: light-dark(120px, 10px) }
      </style><div id=g></div><div id=c></div><div id=w></div>)");
    EXPECT_TRUE(s.of("g").background_layers != nullptr);
    EXPECT_EQ(s.of("c").background, (Color{1, 2, 3, 255}));
    EXPECT_EQ(s.of("w").width.px, 120);
}

TEST(Restyle, HasAnchoredByAnAttribute) {
    auto doc = parseHtml(R"(<!doctype html><style>
        [data-k="card"]:has(.flag) { color: rgb(4, 5, 6) }
        [data-k="card"]:has(.flag) p { font-size: 25px }
      </style>
      <div id=card data-k=card><section><p id=para>x</p><span id=deep>y</span></section></div>
      <div id=other data-k=other><span id=o>z</span></div>)");
    auto styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600});
    const auto again = [&](const char* step) {
        styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600}, StyledDocument::SheetLoader{},
                                                  StyledDocument::ChMeasure{}, nullptr, styles.get());
        expectSameAsFresh(*doc, *styles, step);
    };
    again("nothing changed");
    setAttribute(elementById(*doc, "deep"), "class", "flag");
    again("a flag deep inside the card");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "card"))->color, (Color{4, 5, 6, 255}));
    EXPECT_EQ(styles->styleOf(elementById(*doc, "para"))->font_size, 25);
    setAttribute(elementById(*doc, "deep"), "class", "");
    again("and gone again");
    setAttribute(elementById(*doc, "o"), "class", "flag");
    again("a flag in another card");
    EXPECT_NE(styles->styleOf(elementById(*doc, "other"))->color, (Color{4, 5, 6, 255}));
}

// A style attribute setting only the element's own box (a carousel's width) restyles it,
// not what's in it; one its children can see does.
TEST(Restyle, AnOwnBoxStyleAttributeKeepsTheChildren) {
    const auto restyler = [](std::unique_ptr<Document>& doc, std::unique_ptr<StyledDocument>& styles) {
        return [&](const char* step) {
            styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600}, StyledDocument::SheetLoader{},
                                                      StyledDocument::ChMeasure{}, nullptr, styles.get());
            expectSameAsFresh(*doc, *styles, step);
        };
    };
    auto doc = parseHtml(R"(<!doctype html><style>.slide b { color: rgb(1, 2, 3) }</style>
      <div id=slide class=slide><p><b id=b>b</b></p></div>)");
    auto styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600});
    const auto again = restyler(doc, styles);
    again("nothing changed");
    const ComputedStyle* b = styles->styleOf(elementById(*doc, "b"));
    setAttribute(elementById(*doc, "slide"), "style", "width: 1240px; transform: translate3d(-10px, 0, 0)");
    again("its width and transform");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "b")), b);   // kept
    setAttribute(elementById(*doc, "slide"), "style", "");
    again("cleared");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "b")), b);
    setAttribute(elementById(*doc, "slide"), "style", "width: 10px; color: rgb(9, 9, 9)");
    again("and a color, which its children inherit");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "b"))->color, (Color{1, 2, 3, 255}));

    // Something has `inherit` for one of those (by its shorthand): children see them all.
    auto doc2 = parseHtml(R"(<!doctype html><style>.wide i { display: block; margin: inherit }</style>
      <div id=wide class=wide><i id=i>i</i></div>)");
    auto styles2 = std::make_unique<StyledDocument>(*doc2, css::MediaContext{800, 600});
    const auto again2 = restyler(doc2, styles2);
    setAttribute(elementById(*doc2, "wide"), "style", "margin-top: 30px");
    again2("a margin, inherited");
    EXPECT_EQ(styles2->styleOf(elementById(*doc2, "i"))->margin[0].px, 30);
    setAttribute(elementById(*doc2, "wide"), "style", "margin-top: 20px");
    again2("another");
    EXPECT_EQ(styles2->styleOf(elementById(*doc2, "i"))->margin[0].px, 20);
}

// One element's style by styling only it and its ancestors: as styling the whole page
// gives, kept parts and changed ones, and not done where that can't tell (display: none).
TEST(Restyle, AChainOfAncestorsStylesAsTheWholePage) {
    auto doc = parseHtml(R"(<!doctype html><style>
        html { font-size: 20px } .big p { font-size: 2rem } .red .x { color: rgb(200, 0, 0) }
        div:has(> .flag) .x { color: rgb(0, 0, 200) } .gone { display: none }
      </style>
      <div id=outer class=big><section id=mid><p id=p><span id=x class=x>x</span></p></section><b id=f></b></div>
      <div class=gone><i id=hidden>h</i></div>)");
    auto styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600});
    const auto chainSameAsFresh = [&](const char* id, const char* step) {
        const Node* n = elementById(*doc, id);
        auto chain = StyledDocument::chainOf(n, *doc, css::MediaContext{800, 600}, {}, {}, nullptr, styles.get());
        ASSERT_TRUE(chain) << step;
        const StyledDocument fresh(*doc, css::MediaContext{800, 600});
        EXPECT_EQ(chain->styleOf(n)->color, fresh.styleOf(n)->color) << step;
        EXPECT_EQ(chain->styleOf(n)->font_size, fresh.styleOf(n)->font_size) << step;
    };
    chainSameAsFresh("x", "nothing changed");
    setAttribute(elementById(*doc, "mid"), "class", "red");
    chainSameAsFresh("x", "an ancestor's class");
    setAttribute(elementById(*doc, "f"), "class", "flag");
    chainSameAsFresh("x", "a :has() anchor's");
    chainSameAsFresh("p", "rem");
    EXPECT_FALSE(StyledDocument::chainOf(elementById(*doc, "hidden"), *doc, css::MediaContext{800, 600}, {}, {}, nullptr, styles.get())->styleOf(elementById(*doc, "hidden")));
}

// Siblings: one added after an element changes nothing of it, unless it can be asked
// about those after it (:last-child), and one added before does.
TEST(Restyle, SiblingsAddedAfterKeepWhatsBefore) {
    auto doc = parseHtml(R"(<!doctype html><style>
        li:last-child { color: rgb(1, 1, 1) } .a + .b { color: rgb(2, 2, 2) } p:first-child { font-size: 30px }
      </style>
      <div id=box><p id=p><i id=i>i</i></p><ul id=ul><li id=l1>1</li><li id=l2>2</li></ul></div>)");
    auto styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600});
    const auto again = [&](const char* step) {
        styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600}, StyledDocument::SheetLoader{},
                                                  StyledDocument::ChMeasure{}, nullptr, styles.get());
        expectSameAsFresh(*doc, *styles, step);
    };
    const auto element = [&](const char* tag, const char* cls) {
        Node* n = doc->make(NodeType::Element);
        n->name = tag;
        if (cls) setAttribute(n, "class", cls);
        return n;
    };
    again("nothing changed");
    const ComputedStyle* i = styles->styleOf(elementById(*doc, "i"));
    elementById(*doc, "box")->append(element("div", nullptr));
    again("a div after");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "i")), i);   // kept
    elementById(*doc, "ul")->append(element("li", nullptr));
    again("an li after the last");
    EXPECT_NE(styles->styleOf(elementById(*doc, "l2"))->color, (Color{1, 1, 1, 255}));
    Node* box = elementById(*doc, "box");
    box->insertBefore(element("span", "a"), box->children.front());
    again("one before the first");
    EXPECT_NE(styles->styleOf(elementById(*doc, "p"))->font_size, 30);
    setAttribute(elementById(*doc, "p"), "class", "b");
    again("and a + b");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "p"))->color, (Color{2, 2, 2, 255}));
}

TEST(Selectors, NthChildOfASelector) {
    auto doc = parseHtml(R"(<!doctype html><style>
        li:nth-child(2 of .x) { color: rgb(1, 2, 3) } li:nth-last-child(1 of .x) { font-size: 30px }
      </style><ul><li id=a class=x></li><li id=b></li><li id=c class=x></li><li id=d class=x></li><li id=e></li></ul>)");
    const StyledDocument styles(*doc, css::MediaContext{800, 600});
    EXPECT_EQ(styles.styleOf(elementById(*doc, "c"))->color, (Color{1, 2, 3, 255}));
    EXPECT_NE(styles.styleOf(elementById(*doc, "b"))->color, (Color{1, 2, 3, 255}));
    EXPECT_EQ(styles.styleOf(elementById(*doc, "d"))->font_size, 30);
    EXPECT_NE(styles.styleOf(elementById(*doc, "e"))->font_size, 30);
}

TEST(Values, RevertIsTheUserAgentsValue) {
    // A reset's `all: unset; display: revert` (toss.im's).
    auto doc = parseHtml(R"(<!doctype html><style>* { display: revert } div { display: inline } .r { display: revert }</style>
      <div id=d class=r></div><script id=s class=r></script><span id=p class=r></span>)");
    const StyledDocument styles(*doc, css::MediaContext{800, 600});
    EXPECT_EQ(styles.styleOf(elementById(*doc, "d"))->display, Display::Block);
    ASSERT_NE(styles.styleOf(elementById(*doc, "s")), nullptr);
    EXPECT_EQ(styles.styleOf(elementById(*doc, "s"))->display, Display::None);
    EXPECT_EQ(styles.styleOf(elementById(*doc, "p"))->display, Display::Inline);

    // all: unset drops what came before it; what comes after still counts.
    auto doc2 = parseHtml(R"(<!doctype html><style>a { color: rgb(0, 0, 238) } .u { all: unset; display: revert } .u.k { font-size: 30px }</style>
      <div style='color: rgb(1, 2, 3)'><a id=a class='u k' href=#>x</a><script id=s class=u></script></div>)");
    const StyledDocument styles2(*doc2, css::MediaContext{800, 600});
    EXPECT_EQ(styles2.styleOf(elementById(*doc2, "a"))->color, (Color{1, 2, 3, 255}));   // inherited, not the link's
    EXPECT_EQ(styles2.styleOf(elementById(*doc2, "a"))->font_size, 30);
    ASSERT_NE(styles2.styleOf(elementById(*doc2, "s")), nullptr);
    EXPECT_EQ(styles2.styleOf(elementById(*doc2, "s"))->display, Display::None);   // reverted past the unset
}

TEST(Values, TextDecorationInherits) {
    // Tailwind's preflight: a { color: inherit; text-decoration: inherit }.
    auto doc = parseHtml(R"(<!doctype html><style>a { text-decoration: inherit }</style>
      <div><a id=a href=#>x</a></div><u><a id=b href=#>y</a></u>)");
    const StyledDocument styles(*doc, css::MediaContext{800, 600});
    EXPECT_FALSE(styles.styleOf(elementById(*doc, "a"))->underline);
    EXPECT_TRUE(styles.styleOf(elementById(*doc, "b"))->underline);
}

// With :has() only of the element styled, a change under an anchor restyles the anchor,
// and its children only when what it matched changed (they inherit from it).
TEST(Restyle, UnderAHasAnchorOnlyTheAnchorRestyles) {
    auto doc = parseHtml(R"(<!doctype html><style>
        [data-k="card"]:has(.flag) { color: rgb(4, 5, 6) }
        .big { font-size: 30px }
        .box:has(.flag) i { font-size: 25px }
      </style>
      <div id=card data-k=card><section><p id=para>x</p></section><div><span id=deep>y</span></div></div>
      <div class=box><section><i id=it>i</i></section><b id=b>b</b></div>)");
    auto styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600});
    const auto again = [&](const char* step) {
        styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600}, StyledDocument::SheetLoader{},
                                                  StyledDocument::ChMeasure{}, nullptr, styles.get());
        expectSameAsFresh(*doc, *styles, step);
    };
    again("nothing changed");
    const ComputedStyle* para = styles->styleOf(elementById(*doc, "para"));
    setAttribute(elementById(*doc, "deep"), "class", "big");
    again("a change under the card that it doesn't match on");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "para")), para);   // kept
    EXPECT_EQ(styles->styleOf(elementById(*doc, "deep"))->font_size, 30);
    setAttribute(elementById(*doc, "deep"), "class", "big flag");
    again("one it does");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "para"))->color, (Color{4, 5, 6, 255}));
    setAttribute(elementById(*doc, "deep"), "class", "big");
    again("and back");
    EXPECT_NE(styles->styleOf(elementById(*doc, "para"))->color, (Color{4, 5, 6, 255}));
    // One asked for its descendants: those see what's under it.
    setAttribute(elementById(*doc, "b"), "class", "flag");
    again("a flag in the box");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "it"))->font_size, 25);
    setAttribute(elementById(*doc, "b"), "class", "");
    again("gone from the box");
    EXPECT_NE(styles->styleOf(elementById(*doc, "it"))->font_size, 25);
}

// A :host:has() in a shadow tree's sheet doesn't stop the page's styles being kept, and
// what it's asked for (the host, its shadow tree) follows what's in that tree; one in
// :host()'s argument never matches. (As Chromium has both.)
TEST(Restyle, AHostsHasIsAnchoredOnTheHost) {
    auto doc = parseHtml(R"(<!doctype html><p id=other>x</p>
      <x-card id=host><template shadowrootmode=open><style>
        :host:has(.flag) { color: rgb(4, 5, 6) }
        :host:has(.flag) b { font-size: 25px }
        :host(:has(.flag)) i { font-size: 27px }
      </style><b>b</b><i>i</i><slot></slot></template><section><span id=deep>y</span></section></x-card>)");
    attachDeclarativeShadowRoots(*doc);
    Node* host = elementById(*doc, "host");
    ASSERT_NE(host->shadow_root, nullptr);
    const auto inner = [&](const char* tag) -> Node* {
        for (Node* c : host->shadow_root->children)
            if (c->isElement() && c->name == tag) return c;
        return nullptr;
    };
    auto styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600});
    const auto again = [&](const char* step, float b) {
        styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600}, StyledDocument::SheetLoader{},
                                                  StyledDocument::ChMeasure{}, nullptr, styles.get());
        expectSameAsFresh(*doc, *styles, step);
        EXPECT_EQ(styles->styleOf(inner("b"))->font_size, b) << step;
        EXPECT_EQ(styles->styleOf(inner("i"))->font_size, 16) << step;
        EXPECT_EQ(styles->styleOf(host)->color, b == 25 ? (Color{4, 5, 6, 255}) : (Color{0, 0, 0, 255})) << step;
    };
    const ComputedStyle* other = styles->styleOf(elementById(*doc, "other"));
    again("nothing changed", 16);
    EXPECT_EQ(styles->styleOf(elementById(*doc, "other")), other);   // kept
    setAttribute(elementById(*doc, "deep"), "class", "flag");
    again("in the light tree: not looked at", 16);
    setAttribute(inner("i"), "class", "flag");
    again("in the shadow tree", 25);
    setAttribute(inner("i"), "class", "");
    again("not any more", 16);
}

// Siblings alike but for what their shadow trees' :host rules and ::slotted ones say
// aren't styled as one.
TEST(Shadow, HostsDontShareStylesTheirHostRulesSetApart) {
    auto doc = parseHtml(R"(<!doctype html>
      <x-a><template shadowrootmode=open><style>:host { color: rgb(1, 1, 1) } ::slotted(u) { color: rgb(3, 3, 3) }</style><slot></slot></template><u>1</u></x-a>
      <x-a><template shadowrootmode=open><style>:host { color: rgb(2, 2, 2) }</style><slot></slot></template><u>2</u></x-a>)");
    attachDeclarativeShadowRoots(*doc);
    const StyledDocument styles(*doc, css::MediaContext{800, 600});
    std::vector<Color> hosts, slotted;
    std::vector<const Node*> todo{doc->root()};
    while (!todo.empty()) {
        const Node* n = todo.back();
        todo.pop_back();
        todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        if (n->isElement() && n->name == "x-a") hosts.push_back(styles.styleOf(n)->color);
        if (n->isElement() && n->name == "u") slotted.push_back(styles.styleOf(n)->color);
    }
    EXPECT_EQ(hosts, (std::vector<Color>{{1, 1, 1, 255}, {2, 2, 2, 255}}));
    EXPECT_EQ(slotted, (std::vector<Color>{{3, 3, 3, 255}, {2, 2, 2, 255}}));
}

// :has() with nothing to tell its anchor by: what has what it looks for under it is one.
TEST(Restyle, AnAnchorlessHasGoesByWhatItLooksFor) {
    auto doc = parseHtml(R"(<!doctype html><style>
        :has(> .flag) { color: rgb(4, 5, 6) }
        :has([class*=Err]) i { font-size: 25px }
        :is(.a .b, .c):has(u) { font-size: 22px }
        .kin :has(+ .next) { font-size: 23px }
      </style>
      <p id=other><b id=bold>x</b></p>
      <div id=box><section id=sec><span id=deep>y</span></section><i id=it>i</i></div>
      <div class=a><div class=b id=b>t<em id=em>e</em></div></div>
      <div class=kin><s id=s>1</s><q id=q>2</q></div>)");
    auto styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600});
    const auto again = [&](const char* step) {
        styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600}, StyledDocument::SheetLoader{},
                                                  StyledDocument::ChMeasure{}, nullptr, styles.get());
        expectSameAsFresh(*doc, *styles, step);
    };
    const ComputedStyle* bold = styles->styleOf(elementById(*doc, "bold"));
    again("nothing changed");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "bold")), bold);   // kept
    setAttribute(elementById(*doc, "deep"), "class", "flag");
    again("a child to match");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "sec"))->color, (Color{4, 5, 6, 255}));
    bold = styles->styleOf(elementById(*doc, "bold"));   // (its ancestors have only now become anchors)
    again("nothing changed since");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "bold")), bold);
    setAttribute(elementById(*doc, "deep"), "class", "MyErr");
    again("for another element, and not for this one");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "it"))->font_size, 25);
    setAttribute(elementById(*doc, "deep"), "class", "");
    again("neither");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "it"))->font_size, 16);
    setAttribute(elementById(*doc, "q"), "class", "next");
    again("a sibling to match");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "s"))->font_size, 23);
    setAttribute(elementById(*doc, "q"), "class", "");
    again("not any more");
    EXPECT_EQ(styles->styleOf(elementById(*doc, "s"))->font_size, 16);
}

// What's hovered, pressed and focused: what a rule asks it of restyles, and what's under that.
TEST(Restyle, StatesRestyleWhatTheyCanChange) {
    auto doc = parseHtml(R"(<!doctype html><style>
        .m:hover .d { font-size: 30px } a:active { color: rgb(1, 2, 3) } .f:focus-within { color: rgb(4, 5, 6) }
        .x:not(:hover) { font-size: 21px } .p:has(.k:hover) { font-size: 22px } .s:hover + i { font-size: 23px }
      </style><p id=other>o</p><div class=m id=m><b class=d id=d>x</b></div><a id=a>a</a><div class=f id=f><input id=q></div>
      <u class=x id=x>x</u><div class=p id=p><em class=k id=k>k</em></div><s class=s id=s>s</s><i id=i>i</i>)");
    FormState forms;
    const auto make = [&](const StyledDocument* previous) {
        return std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600}, StyledDocument::SheetLoader{}, StyledDocument::ChMeasure{}, &forms, previous);
    };
    auto styles = make(nullptr);
    EXPECT_TRUE(styles->statesMatter());
    const auto by = [&](const char* id) { return styles->styleOf(elementById(*doc, id)); };
    const ComputedStyle* other = by("other");
    int steps = 0;
    const auto go = [&](const char* step) {
        styles = make(styles.get());
        const auto fresh = make(nullptr);
        for (const char* id : {"other", "m", "d", "a", "f", "q", "x", "p", "k", "s", "i"}) {
            EXPECT_EQ(by(id)->font_size, fresh->styleOf(elementById(*doc, id))->font_size) << step << " " << id;
            EXPECT_EQ(by(id)->color, fresh->styleOf(elementById(*doc, id))->color) << step << " " << id;
        }
        if (++steps < 6) EXPECT_EQ(by("other"), other) << step;   // kept (and copied, after some generations)
    };
    forms.hovered = elementById(*doc, "d"); go("over the menu's child");
    EXPECT_EQ(by("d")->font_size, 30);
    forms.hovered = elementById(*doc, "other"); go("over something else");
    EXPECT_EQ(by("d")->font_size, 16);
    forms.hovered = elementById(*doc, "x"); go("not :not");
    EXPECT_EQ(by("x")->font_size, 16);
    forms.hovered = elementById(*doc, "k"); go("for what holds it");
    EXPECT_EQ(by("p")->font_size, 22);
    forms.hovered = elementById(*doc, "s"); go("for its sibling");
    EXPECT_EQ(by("i")->font_size, 23);
    forms.hovered = nullptr;
    forms.pressed = elementById(*doc, "a"); go("pressed");
    EXPECT_EQ(by("a")->color, (Color{1, 2, 3, 255}));
    forms.pressed = nullptr;
    forms.focused = elementById(*doc, "q"); go("focused");
    EXPECT_EQ(by("f")->color, (Color{4, 5, 6, 255}));
    forms.focused = nullptr; go("none of them");
    EXPECT_EQ(by("x")->font_size, 21);
}

// Transitions and animations, by the clock given them.
TEST(Motion, TransitionsGoFromWhatItWasAndTurnBack) {
    auto doc = parseHtml(R"(<!doctype html><style>
        #a { opacity: 1; width: 100px; color: rgb(0,0,0); transition: opacity 1s linear, width 2s linear 1s, color 0s }
        .on { opacity: 0 !important; width: 300px !important; color: rgb(9,9,9) !important }
        #b { width: auto; transition: all 1s } .wide { width: 50px !important }</style><p id=a>x</p><p id=b>y</p>)");
    Animator an;
    std::unique_ptr<StyledDocument> styles;
    const auto restyle = [&](double t) {
        auto next = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600}, StyledDocument::SheetLoader{}, StyledDocument::ChMeasure{}, nullptr, styles.get());
        an.styled(*next, styles.get(), t);
        styles = std::move(next);
    };
    const auto a = [&] { return styles->styleOf(elementById(*doc, "a")); };
    restyle(0);
    EXPECT_FALSE(an.running());   // nothing moves as it's first shown
    setAttribute(elementById(*doc, "a"), "class", "on");
    setAttribute(elementById(*doc, "b"), "class", "wide");
    restyle(10);
    EXPECT_TRUE(an.running());
    EXPECT_FLOAT_EQ(a()->opacity, 1);
    EXPECT_EQ(a()->color, (Color{9, 9, 9, 255}));                                     // no time to take
    EXPECT_FLOAT_EQ(styles->styleOf(elementById(*doc, "b"))->width.px, 50);           // nothing between auto and a length
    an.tick(*styles, 10.5);
    EXPECT_FLOAT_EQ(a()->opacity, 0.5f);
    EXPECT_FLOAT_EQ(a()->width.px, 100);                                              // waiting
    restyle(10.5);                                                                    // kept, and still laid over
    EXPECT_FLOAT_EQ(a()->opacity, 0.5f);
    an.tick(*styles, 12);
    EXPECT_FLOAT_EQ(a()->opacity, 0);
    EXPECT_FLOAT_EQ(a()->width.px, 200);
    setAttribute(elementById(*doc, "a"), "class", "");
    restyle(12);                                                                      // back, from where it's got to
    an.tick(*styles, 12.5);
    EXPECT_FLOAT_EQ(a()->opacity, 0.5f);
    EXPECT_FLOAT_EQ(a()->width.px, 200);
    an.tick(*styles, 20);
    EXPECT_FALSE(an.running());
    EXPECT_FLOAT_EQ(a()->width.px, 100);
    int ends = 0;
    for (const Animator::Event& e : an.takeEvents()) ends += e.type == "transitionend";
    EXPECT_EQ(ends, 3);   // opacity's, opacity's back, width's back
}

TEST(Motion, KeyframesFillsDirectionsAndTurns) {
    auto doc = parseHtml(R"(<!doctype html><style>
        @keyframes fade { from { opacity: 0 } 50% { opacity: .8; animation-timing-function: linear } to { opacity: 1 } }
        @keyframes spin { to { transform: rotate(360deg) } } @keyframes slide { from { left: 0 } to { left: 100px } }
        #f { opacity: 0; animation: fade 2s linear 1s both } #s { animation: spin 4s linear infinite }
        #p { position: absolute; animation: slide 1s linear 2 alternate } #n { animation: nothing 1s; opacity: .3 }
        #q::before { content: 'x'; animation: fade 2s linear forwards }</style>
        <p id=f>f</p><p id=s>s</p><p id=p>p</p><p id=n>n</p><p id=q>q</p>)");
    Animator an;
    StyledDocument styles(*doc, css::MediaContext{800, 600});
    const auto by = [&](const char* id) { return styles.styleOf(elementById(*doc, id)); };
    an.styled(styles, nullptr, 100);
    EXPECT_FLOAT_EQ(by("f")->opacity, 0);
    EXPECT_FLOAT_EQ(by("n")->opacity, 0.3f);
    an.tick(styles, 102);      // 1 s in: half way
    EXPECT_NEAR(by("f")->opacity, 0.8f, 1e-4);
    EXPECT_NEAR(styles.beforeOf(elementById(*doc, "q"))->opacity, 1, 1e-4);
    EXPECT_NEAR(by("s")->transform_linear[0], -1, 1e-4);   // half a turn
    EXPECT_FALSE(an.finiteRunning() && !an.running());
    an.tick(styles, 100.25);
    EXPECT_NEAR(by("p")->offset[Left].px, 25, 1e-3);
    an.tick(styles, 101.25);   // on its way back
    EXPECT_NEAR(by("p")->offset[Left].px, 75, 1e-3);
    an.tick(styles, 110);
    EXPECT_TRUE(by("p")->offset[Left].is_auto);            // over, and not filling
    EXPECT_FLOAT_EQ(by("f")->opacity, 1);                  // filling
    EXPECT_TRUE(an.running());                             // the turning one
    EXPECT_FALSE(an.finiteRunning());
    int ended = 0;
    for (const Animator::Event& e : an.takeEvents()) ended += e.type == "animationend";
    EXPECT_EQ(ended, 3);
}

TEST(Motion, Easings) {
    const auto easing = [](const char* css) {
        Easing e;
        size_t i = 0;
        EXPECT_TRUE(easingAt(css::tokenize(css), i, e)) << css;
        return e;
    };
    EXPECT_FLOAT_EQ(easing("linear").at(0.3f), 0.3f);
    EXPECT_NEAR(easing("ease").at(0.5f), 0.8024f, 1e-3);
    EXPECT_NEAR(easing("ease-in-out").at(0.5f), 0.5f, 1e-3);
    EXPECT_FLOAT_EQ(easing("steps(4)").at(0.3f), 0.25f);
    EXPECT_FLOAT_EQ(easing("steps(4, start)").at(0.3f), 0.5f);
    EXPECT_FLOAT_EQ(easing("step-end").at(0.99f), 0);
    EXPECT_NEAR(easing("cubic-bezier(.5,-.5,.5,1.5)").at(0.5f), 0.5f, 1e-3);
}

// In shadow trees too, and of what's slotted: up the composed tree. Roots of the same
// sheets share their rules, each still with its own host.
TEST(Restyle, AChainGoesThroughShadowTrees) {
    auto doc = parseHtml(R"(<!doctype html><style>.r { color: rgb(9, 0, 0) } x-a { font-size: 20px } x-a::part(p) { font-weight: 700 }</style>
      <x-a class=r><template shadowrootmode=open><style>:host(.r) b { color: rgb(1, 0, 0) } ::slotted(u) { font-size: 2em } div { font-size: 2em }</style>
        <div><slot></slot></div><b part=p>b</b><slot name=none><s>f</s></slot></template><u id=u1>1</u><i id=i slot=nowhere>n</i></x-a>
      <x-a><template shadowrootmode=open><style>:host(.r) b { color: rgb(1, 0, 0) } ::slotted(u) { font-size: 2em } div { font-size: 2em }</style>
        <div><slot></slot></div><b part=p>b</b><slot name=none><s>f</s></slot></template><u id=u2>2</u></x-a>)");
    attachDeclarativeShadowRoots(*doc);
    const StyledDocument fresh(*doc, css::MediaContext{800, 600});
    std::vector<const Node*> asked, todo{doc->documentElement()};
    while (!todo.empty()) {
        const Node* n = todo.back();
        todo.pop_back();
        if (n->isElement() && fresh.styleOf(n) && fresh.styleOf(n)->display != Display::None) asked.push_back(n);
        todo.insert(todo.end(), n->children.begin(), n->children.end());
        if (n->shadow_root) todo.push_back(n->shadow_root);
    }
    size_t in_shadow = 0;
    for (const Node* n : asked) {
        auto chain = StyledDocument::chainOf(n, *doc, css::MediaContext{800, 600}, {}, {}, nullptr, &fresh);
        ASSERT_TRUE(chain) << n->name;
        EXPECT_EQ(chain->styleOf(n)->color, fresh.styleOf(n)->color) << n->name;
        EXPECT_EQ(chain->styleOf(n)->font_size, fresh.styleOf(n)->font_size) << n->name;
        EXPECT_EQ(chain->styleOf(n)->font_weight, fresh.styleOf(n)->font_weight) << n->name;
        in_shadow += n->containingShadowRoot() != nullptr;
        if (n->name == "b") {
            EXPECT_EQ(fresh.styleOf(n)->font_weight, 700);
            EXPECT_EQ(fresh.styleOf(n)->color.r, n->containingShadowRoot()->shadow_host->attr("class") ? 1 : 0);
        }
        if (n->name == "u") EXPECT_FLOAT_EQ(fresh.styleOf(n)->font_size, 80);
    }
    EXPECT_GE(in_shadow, 10u);
    EXPECT_FALSE(StyledDocument::chainOf(elementById(*doc, "i"), *doc, css::MediaContext{800, 600}, {}, {}, nullptr, &fresh)->styleOf(elementById(*doc, "i")));   // in no slot
}

TEST(Restyle, AsksFontsOnlyForCh) {
    auto doc = parseHtml("<!doctype html><style>#a{width:10em}</style><p id=a>x</p>");
    const StyledDocument plain(*doc, css::MediaContext{800, 600});
    EXPECT_FALSE(plain.asksFonts());
    auto doc2 = parseHtml("<!doctype html><style>#a{width:10ch}</style><p id=a>x</p>");
    const StyledDocument ch(*doc2, css::MediaContext{800, 600});
    EXPECT_TRUE(ch.asksFonts());
    const StyledDocument kept(*doc2, css::MediaContext{800, 600}, {}, {}, nullptr, &ch);
    EXPECT_TRUE(kept.asksFonts());
}

// As Chrome computes them, to a unit.
TEST(Colors, SpacesAndMixes) {
    const auto near = [](const char* css, int r, int g, int b, int a) {
        Color c{1, 2, 3, 4};
        ASSERT_TRUE(parseColor(css::tokenize(css), Color{10, 20, 30, 255}, c)) << css;
        EXPECT_NEAR(c.r, r, 1) << css;
        EXPECT_NEAR(c.g, g, 1) << css;
        EXPECT_NEAR(c.b, b, 1) << css;
        EXPECT_NEAR(c.a, a, 1) << css;
    };
    near("color-mix(in srgb,#FFFFFF 80%,transparent)", 255, 255, 255, 204);
    near("color-mix(in srgb, red, blue)", 128, 0, 128, 255);
    near("color-mix(in srgb, 25% red, blue)", 64, 0, 191, 255);
    near("color-mix(in srgb, red 20%, blue 20%)", 128, 0, 128, 102);
    near("color-mix(in srgb, currentColor 50%, color-mix(in srgb, rgb(10 20 30), rgb(10 20 30)))", 10, 20, 30, 255);
    near("color-mix(in oklab, white, black)", 99, 99, 99, 255);
    near("color-mix(in hsl, red, lime)", 255, 255, 0, 255);
    near("oklch(0.628 0.2577 29.23)", 255, 0, 0, 255);
    near("oklch(62.8% 0.2577 29.23deg / 50%)", 255, 0, 0, 128);
    near("oklab(1 0 0)", 255, 255, 255, 255);
    near("lab(54.29 80.8 69.89)", 255, 0, 0, 255);
    near("hwb(120 0% 50%)", 0, 128, 0, 255);
    near("color(srgb 1 0.5 0 / .5)", 255, 128, 0, 128);
    near("color(display-p3 1 1 1)", 255, 255, 255, 255);
    Color c;
    for (const char* bad : {"color-mix(in nothing, red, blue)", "color-mix(red, blue)", "color-mix(in srgb, red 0%, blue 0%)", "oklch(1 2)", "color(srgb 1 1)", "oklch(from red l c h)"})
        EXPECT_FALSE(parseColor(css::tokenize(bad), Color{}, c)) << bad;
}

TEST(Motion, AMovingColourIsInherited) {
    auto doc = parseHtml(R"(<!doctype html><style>@keyframes c { from { color: rgb(0,0,0) } to { color: rgb(100,0,0) } }
        #a { animation: c 1s linear forwards } #own { color: rgb(0,0,9) } #q::before { content: 'x' }</style>
        <div id=a><p id=p><b id=b>b</b><i id=own>i</i></p><u id=q>u</u></div>)");
    Animator an;
    StyledDocument styles(*doc, css::MediaContext{800, 600});
    an.styled(styles, nullptr, 0);
    an.tick(styles, 0.5);
    const auto red = [&](const char* id) { return static_cast<int>(styles.styleOf(elementById(*doc, id))->color.r); };
    EXPECT_EQ(red("a"), 50);
    EXPECT_EQ(red("b"), 50);
    EXPECT_EQ(red("own"), 0);
    EXPECT_EQ(styles.beforeOf(elementById(*doc, "q"))->color.r, 50);
    an.tick(styles, 9);
    EXPECT_EQ(red("b"), 100);
}

TEST(Containers, QueriesAndUnits) {
    auto doc = parseHtml(R"(<!doctype html><style>
        #c { container: card / inline-size } #o { container-type: inline-size }
        @container (min-width: 300px) { p { color: rgb(1,0,0) } .n { & b { color: rgb(2,0,0) } } }
        @container other (min-width: 1px) { p { color: rgb(3,0,0) } }
        @container card (inline-size > 350px) { p { opacity: .5 } }
        @container style(--x) { p { opacity: .1 } }
        i { width: 50cqw }</style>
        <div id=o><div id=c><p id=p>x</p><div class=n><b id=b>b</b></div><i id=i>i</i></div></div><p id=out>y</p>)");
    css::ContainerSizes sizes;
    auto styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600, &sizes});
    EXPECT_EQ(styles->containers().size(), 2u);
    const auto by = [&](const char* id) { return styles->styleOf(elementById(*doc, id)); };
    EXPECT_EQ(by("p")->color.r, 0);               // not laid out yet
    const auto at = [&](float w) {
        sizes[elementById(*doc, "c")] = {w, 0};
        styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600, &sizes}, StyledDocument::SheetLoader{}, StyledDocument::ChMeasure{}, nullptr, styles.get());
    };
    at(320);
    EXPECT_EQ(by("p")->color.r, 1);
    EXPECT_EQ(by("b")->color.r, 2);
    EXPECT_FLOAT_EQ(by("p")->opacity, 1);
    EXPECT_FLOAT_EQ(by("i")->width.px, 160);
    EXPECT_EQ(by("out")->color.r, 0);
    at(400);
    EXPECT_FLOAT_EQ(by("p")->opacity, 0.5f);
    at(200);                                      // (kept styles don't stay behind)
    EXPECT_EQ(by("p")->color.r, 0);
    EXPECT_FLOAT_EQ(by("i")->width.px, 100);
}

TEST(CustomProperties, Registered) {
    auto doc = parseHtml(R"(<!doctype html><style>
        @property --w { syntax: "*"; inherits: false; initial-value: 7px }
        @property --i { syntax: "<length>"; inherits: true; initial-value: 3px }
        @property --bad { syntax: "<length>"; inherits: false }
        p { width: var(--w, 1px); height: var(--i); margin-left: var(--bad, 5px) } #set { --w: 9px; --i: 4px }
        </style><div id=set><p id=in>x</p></div><p id=out style="--w: 2px">y</p><p id=plain>z</p>)");
    const StyledDocument styles(*doc, css::MediaContext{800, 600});
    const auto by = [&](const char* id) { return styles.styleOf(elementById(*doc, id)); };
    EXPECT_FLOAT_EQ(by("in")->width.px, 7);      // not inherited; its initial value before the fallback
    EXPECT_FLOAT_EQ(by("in")->height.px, 4);
    EXPECT_FLOAT_EQ(by("out")->width.px, 2);
    EXPECT_FLOAT_EQ(by("plain")->height.px, 3);
    EXPECT_FLOAT_EQ(by("plain")->margin[3].px, 5);
}

// Style attributes changed at random, styled again with only those: as styling it all gives.
TEST(Restyle, StyleAttributesOnly) {
    uint32_t seed = std::getenv("FUZZ_SEED") ? static_cast<uint32_t>(std::atoi(std::getenv("FUZZ_SEED"))) : 99;
    const int rounds = std::getenv("FUZZ_ROUNDS") ? std::atoi(std::getenv("FUZZ_ROUNDS")) : 40;
    const auto rnd = [&seed](uint32_t n) {
        seed = seed * 1664525 + 1013904223;
        return (seed >> 8) % n;
    };
    static const char* const kStyles[] = {"", "color:red", "display:none", "width:10px", "font-size:30px;color:inherit", "--v:7px", "--v:9px;opacity:.5", "display:block;margin:inherit",
                                          "transform:scale(2)", "visibility:hidden", "container-type:inline-size", "display:contents", "all:unset", "letter-spacing:2px"};
    size_t parts = 0, kept = 0;
    for (int round = 0; round < rounds; ++round) {
        const std::string html = "<!doctype html><style>p{padding-left:var(--v,1px)} div>p+b{color:blue} :is(i,b):first-child{width:5em} p::before{content:'x';color:inherit}"
                                 ".a .b{font-weight:700} div:has(> .b) i{opacity:.3} b{margin:inherit} u{font-size:2em} " + std::string(round % 7 == 0 ? "[style*=red]{width:77px}" : "") + "</style><div id=r></div>";
        auto doc = parseHtml(html);
        std::vector<Node*> all{const_cast<Node*>(elementById(*doc, "r"))};
        for (int i = 0; i < 30; ++i) {
            static const char* const kTags[] = {"div", "p", "b", "i", "u", "span"};
            Node* n = doc->make(NodeType::Element);
            n->name = kTags[rnd(6)];
            n->ns = Namespace::Html;
            if (rnd(3) == 0) n->attrs.push_back({"class", rnd(2) ? "a" : "b"});
            if (rnd(3) == 0) n->attrs.push_back({"style", kStyles[rnd(std::size(kStyles))]});
            all[rnd(static_cast<uint32_t>(all.size()))]->insertBefore(n, nullptr);
            all.push_back(n);
        }
        auto styles = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600});
        for (int step = 0; step < 20; ++step) {
            std::vector<const Node*> changed;
            const uint32_t how_many = 1 + rnd(3);
            for (uint32_t k = 0; k < how_many; ++k) {
                Node* n = all[rnd(static_cast<uint32_t>(all.size()))];
                setAttribute(n, "style", kStyles[rnd(std::size(kStyles))]);
                changed.push_back(n);
            }
            auto next = StyledDocument::withStyleAttributes(changed, *doc, css::MediaContext{800, 600}, {}, {}, nullptr, *styles);
            ASSERT_EQ(next != nullptr, round % 7 != 0 && next) << round;
            if (next) ++parts;
            else next = std::make_unique<StyledDocument>(*doc, css::MediaContext{800, 600}, StyledDocument::SheetLoader{}, StyledDocument::ChMeasure{}, nullptr, styles.get());
            const StyledDocument fresh(*doc, css::MediaContext{800, 600});
            const bool all_same = next->allKept();
            kept += all_same;
            for (const Node* n : all) {
                const ComputedStyle *a = next->styleOf(n), *b = fresh.styleOf(n);
                ASSERT_EQ(a != nullptr, b != nullptr) << round << " " << step << " " << n->name;
                if (all_same) ASSERT_EQ(a, styles->styleOf(n)) << round << " " << step;
                if (!a) continue;
                ASSERT_EQ(a->color, b->color) << round << " " << step << " " << n->name;
                ASSERT_EQ(a->display, b->display) << round << " " << step << " " << n->name;
                ASSERT_FLOAT_EQ(a->font_size, b->font_size) << round << " " << step << " " << n->name;
                ASSERT_FLOAT_EQ(a->width.px, b->width.px) << round << " " << step << " " << n->name;
                ASSERT_FLOAT_EQ(a->padding[3].px, b->padding[3].px) << round << " " << step << " " << n->name;
                ASSERT_FLOAT_EQ(a->margin[0].px, b->margin[0].px) << round << " " << step << " " << n->name;
                ASSERT_FLOAT_EQ(a->opacity, b->opacity) << round << " " << step << " " << n->name;
                ASSERT_EQ(a->font_weight, b->font_weight) << round << " " << step << " " << n->name;
                ASSERT_EQ(a->hidden, b->hidden) << round << " " << step << " " << n->name;
                const ComputedStyle *pa = next->beforeOf(n), *pb = fresh.beforeOf(n);
                ASSERT_EQ(pa != nullptr, pb != nullptr) << round << " " << step << " " << n->name;
                if (pa) ASSERT_EQ(pa->color, pb->color) << round << " " << step << " " << n->name;
            }
            ASSERT_EQ(next->containers().size(), fresh.containers().size()) << round << " " << step;
            styles = std::move(next);
        }
    }
    EXPECT_GT(parts, static_cast<size_t>(rounds) * 10);
    EXPECT_GT(kept, 0u);
}

TEST(CustomProperties, AsText) {
    auto doc = parseHtml(R"(<!doctype html><style>:root{--r: 5px; --bad: var(--none); --f: var(--none, 2em)}
        #a{--c:red;--u:url(x.svg);--k: calc(1px + var(--r)) , "q\"t" #fff;--l:var(--l)}</style><p id=a>x</p>)");
    const StyledDocument styles(*doc, css::MediaContext{800, 600});
    const ComputedStyle& s = *styles.styleOf(elementById(*doc, "a"));
    EXPECT_EQ(customPropertyText(s, "--r"), "5px");
    EXPECT_EQ(customPropertyText(s, "--c"), "red");
    EXPECT_EQ(customPropertyText(s, "--u"), "url(x.svg)");
    EXPECT_EQ(customPropertyText(s, "--k"), "calc(1px + 5px) , \"q\\\"t\" #fff");
    EXPECT_EQ(customPropertyText(s, "--bad"), "");
    EXPECT_EQ(customPropertyText(s, "--f"), "2em");
    EXPECT_EQ(customPropertyText(s, "--l"), "");   // (of itself)
    EXPECT_EQ(customPropertyText(s, "--none"), "");
}
