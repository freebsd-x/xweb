#include <gtest/gtest.h>

#include <string>

#include "xweb/css.h"

using namespace fbx::xweb::css;

namespace {
std::vector<ComplexSelector> selectors(const std::string& s) {
    std::vector<ComplexSelector> out;
    EXPECT_TRUE(parseSelectorList(tokenize(s), out)) << s;
    return out;
}
bool valid(const std::string& s) {
    std::vector<ComplexSelector> out;
    return parseSelectorList(tokenize(s), out);
}
}  // namespace

TEST(CssTokenizer, Numbers) {
    const auto t = tokenize("12 1.5e2 -3px 50% +.5em");
    ASSERT_EQ(t.size(), 9u);
    EXPECT_EQ(t[0].type, TokenType::Number);
    EXPECT_TRUE(t[0].is_integer);
    EXPECT_DOUBLE_EQ(t[2].number, 150);
    EXPECT_EQ(t[4].type, TokenType::Dimension);
    EXPECT_EQ(t[4].text, "px");
    EXPECT_DOUBLE_EQ(t[4].number, -3);
    EXPECT_EQ(t[6].type, TokenType::Percentage);
    EXPECT_DOUBLE_EQ(t[8].number, 0.5);
}

TEST(CssTokenizer, StringsEscapesCommentsUrls) {
    auto t = tokenize("/* c */ \"a\\62 c\" 'x\\\ny' \\66oo url( a.png ) url('b.png') #fff");
    std::vector<Token> kept;
    for (auto& k : t)
        if (k.type != TokenType::Whitespace) kept.push_back(k);
    ASSERT_EQ(kept.size(), 8u);
    EXPECT_EQ(kept[0].text, "abc");
    EXPECT_EQ(kept[1].text, "xy");
    EXPECT_EQ(kept[2].type, TokenType::Ident);
    EXPECT_EQ(kept[2].text, "foo");
    EXPECT_EQ(kept[3].type, TokenType::Url);
    EXPECT_EQ(kept[3].text, "a.png");
    EXPECT_EQ(kept[4].type, TokenType::Function);
    EXPECT_EQ(kept[7].type, TokenType::Hash);
}

TEST(CssTokenizer, UnterminatedThingsEndCleanly) {
    for (const char* s : {"/* open", "\"open", "url(open", "#", "@", "\\", "-", "--", "1e", "a(", "<!-", "url(a b"})
        (void)tokenize(s);
}

TEST(CssParser, RulesAndDeclarations) {
    const Stylesheet s = parseStylesheet("a, b.c { color: red; margin: 0 !important } /* x */ d{}");
    ASSERT_EQ(s.rules.size(), 1u);
    EXPECT_EQ(s.rules[0].selectors.size(), 2u);
    ASSERT_EQ(s.rules[0].declarations.size(), 2u);
    EXPECT_EQ(s.rules[0].declarations[0].name, "color");
    EXPECT_FALSE(s.rules[0].declarations[0].important);
    EXPECT_TRUE(s.rules[0].declarations[1].important);
    EXPECT_EQ(s.rules[0].declarations[1].value.size(), 1u);
}

TEST(CssParser, ErrorRecoveryKeepsTheRest) {
    const Stylesheet s = parseStylesheet("a{color:red;;bad;width:1px;{nested}height:2px} ??{x:y} b{top:0} @bad; c{left:0} d{e:f");
    ASSERT_EQ(s.rules.size(), 4u);
    EXPECT_EQ(s.rules[0].declarations.size(), 3u);
    EXPECT_EQ(s.rules[3].declarations[0].name, "e");
}

TEST(CssParser, AtRules) {
    const Stylesheet s = parseStylesheet(
        "@import 'x.css'; @import url(y.css) print; @media (min-width:1px){a{b:c} @media print{d{e:f}}} "
        "@font-face{font-family:x} @keyframes k{from{top:0}} @supports (display:grid){g{h:i}} @supports not (display:grid){j{k:l}}");
    ASSERT_EQ(s.imports.size(), 2u);
    EXPECT_EQ(s.imports[1], "y.css");
    ASSERT_EQ(s.rules.size(), 3u);
    EXPECT_EQ(s.rules[0].media.size(), 1u);
    EXPECT_EQ(s.rules[1].media.size(), 2u);
    EXPECT_EQ(s.rules[2].media.size(), 0u);
}

TEST(CssSelectors, Specificity) {
    EXPECT_EQ(selectors("#a .b c")[0].specificity.key(), (Specificity{1, 1, 1}).key());
    EXPECT_EQ(selectors("a:not(#b)")[0].specificity.key(), (Specificity{1, 0, 1}).key());
    EXPECT_EQ(selectors(":where(#a) b")[0].specificity.key(), (Specificity{0, 0, 1}).key());
    EXPECT_EQ(selectors("[x]:first-child")[0].specificity.key(), (Specificity{0, 2, 0}).key());
    EXPECT_EQ(selectors("::before")[0].specificity.key(), (Specificity{0, 0, 1}).key());
}

TEST(CssSelectors, Combinators) {
    const auto s = selectors("a > b + c ~ d e");
    ASSERT_EQ(s[0].parts.size(), 5u);
    EXPECT_EQ(s[0].parts[1].combinator, Combinator::Child);
    EXPECT_EQ(s[0].parts[2].combinator, Combinator::NextSibling);
    EXPECT_EQ(s[0].parts[3].combinator, Combinator::SubsequentSibling);
    EXPECT_EQ(s[0].parts[4].combinator, Combinator::Descendant);
}

TEST(CssSelectors, Nth) {
    const auto check = [](const std::string& arg, int a, int b) {
        const auto s = selectors(":nth-child(" + arg + ")");
        EXPECT_EQ(s[0].parts[0].simple[0].a, a) << arg;
        EXPECT_EQ(s[0].parts[0].simple[0].b, b) << arg;
    };
    check("odd", 2, 1);
    check("even", 2, 0);
    check("3", 0, 3);
    check("2n+1", 2, 1);
    check("-n + 3", -1, 3);
    check("n", 1, 0);
    check("2n - 1", 2, -1);
}

TEST(CssSelectors, InvalidListsAreRejectedWhole) {
    EXPECT_FALSE(valid("a,"));
    EXPECT_FALSE(valid(",a"));
    EXPECT_FALSE(valid("a > "));
    EXPECT_FALSE(valid("a..b"));
    EXPECT_FALSE(valid("#1a"));
    EXPECT_FALSE(valid("a:has(b)"));
    EXPECT_FALSE(valid("svg|a"));
    EXPECT_TRUE(valid("a:is(1bad, b)"));   // :is forgives
    EXPECT_FALSE(valid("a:not(1bad)"));
}

TEST(CssSelectors, DeepNestingIsRefusedNotFatal) {
    std::string s;
    for (int i = 0; i < 100000; ++i) s += ":not(";
    (void)valid(s);
}

TEST(MediaQueries, Match) {
    const MediaContext c{800, 600};
    const auto m = [&](const std::string& q) { return mediaMatches(tokenize(q), c); };
    EXPECT_TRUE(m("screen"));
    EXPECT_TRUE(m("all"));
    EXPECT_FALSE(m("print"));
    EXPECT_TRUE(m("print, screen"));
    EXPECT_TRUE(m("not print"));
    EXPECT_FALSE(m("not screen"));
    EXPECT_TRUE(m("(min-width: 500px)"));
    EXPECT_FALSE(m("(max-width: 500px)"));
    EXPECT_TRUE(m("screen and (min-width: 30em) and (max-height: 800px)"));
    EXPECT_FALSE(m("screen and (min-width: 1000px)"));
    EXPECT_TRUE(m("(prefers-color-scheme: light)"));
    EXPECT_FALSE(m("(prefers-color-scheme: dark)"));
    EXPECT_FALSE(m("(some-future-feature: 3)"));
    EXPECT_TRUE(m("(orientation: landscape)"));
}

namespace {
bool media(const std::string& q, float w = 800, float h = 600) {
    return mediaMatches(tokenize(q), MediaContext{w, h});
}
size_t rulesUnderSupports(const std::string& q) {
    return parseStylesheet("@supports " + q + "{a{color:red}}").rules.size();
}
}  // namespace

TEST(Media, RangeSyntax) {
    EXPECT_TRUE(media("(width <= 800px)"));
    EXPECT_FALSE(media("(width < 800px)"));
    EXPECT_TRUE(media("(width >= 800px)"));
    EXPECT_FALSE(media("(width > 800px)"));
    EXPECT_TRUE(media("(width = 800px)"));
    EXPECT_TRUE(media("(800px >= width)"));
    EXPECT_FALSE(media("(801px <= width)"));
    EXPECT_TRUE(media("(400px < width < 900px)"));
    EXPECT_FALSE(media("(400px < width < 700px)"));
    EXPECT_TRUE(media("(height<=600px)"));
    EXPECT_TRUE(media("not (width <= 769px)"));
    EXPECT_TRUE(media("screen and (width > 10px)"));
    EXPECT_FALSE(media("print and (width > 10px)"));
    EXPECT_TRUE(media("(width > 49em)"));
    EXPECT_FALSE(media("(width > 50em)"));
}

TEST(Media, Calc) {
    EXPECT_TRUE(media("(width >= calc(1rem * 2 + 15rem + 2rem + 31rem))"));    // 800
    EXPECT_TRUE(media("(width < calc(1rem * 2 + (15rem + 2rem) * 2 + 31rem))"));   // 1072
    EXPECT_TRUE(media("(min-width: calc(100px + 100px))"));
    EXPECT_FALSE(media("(max-width: calc(100px + 100px))"));
    EXPECT_FALSE(media("(width > calc(1px / 0))"));
}

TEST(Media, BracketsAndOr) {
    EXPECT_TRUE(media("((width > 10px) and (height > 10px))"));
    EXPECT_FALSE(media("((width > 10px) and (height > 10000px))"));
    EXPECT_TRUE(media("(width > 10000px) or (height > 10px)"));
    EXPECT_FALSE(media("(not (width > 10px))"));
    std::string deep = "(width > 10px)";
    for (int i = 0; i < 5000; ++i) deep = "(" + deep + ")";
    (void)media(deep);   // just mustn't overflow the stack
}

TEST(Media, RangeNonsenseMatchesNothing) {
    EXPECT_FALSE(media("(width <)"));
    EXPECT_FALSE(media("(< width)"));
    EXPECT_FALSE(media("(width < 1px < 2px < 3px)"));
    EXPECT_FALSE(media("(color > 3)"));
    EXPECT_FALSE(media("(10px < 20px)"));
    EXPECT_FALSE(media("(width < height)"));
}

TEST(Supports, AnswersForThisEngine) {
    EXPECT_EQ(rulesUnderSupports("(display:grid)"), 1u);
    EXPECT_EQ(rulesUnderSupports("not (display:grid)"), 0u);
    EXPECT_EQ(rulesUnderSupports("(color:oklab(0% 0 0%))"), 0u);
    EXPECT_EQ(rulesUnderSupports("not (color:oklab(0% 0 0%))"), 1u);
    EXPECT_EQ(rulesUnderSupports("(color:light-dark(red,red))"), 1u);
    EXPECT_EQ(rulesUnderSupports("(color:light-dark(red,red)) and (color:oklab(0% 0 0%))"), 0u);
    EXPECT_EQ(rulesUnderSupports("(color:oklab(0% 0 0%)) or (display:flex)"), 1u);
    EXPECT_EQ(rulesUnderSupports("(-moz-appearance:none)"), 0u);
    EXPECT_EQ(rulesUnderSupports("selector(a > b)"), 1u);
    EXPECT_EQ(rulesUnderSupports("((display:grid) and (display:flex))"), 1u);
    EXPECT_EQ(rulesUnderSupports("nonsense"), 0u);
}
