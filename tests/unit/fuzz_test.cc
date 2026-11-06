// Hostile input can't crash or hang the engine: mutated pages and CSS through the whole
// pipeline. Seeded, so a failure reproduces. Run under ASan for what it's really for.

#include <gtest/gtest.h>

#include <random>
#include <string>

#include "xweb/css.h"
#include "xweb/html_parser.h"
#include "xweb/layout.h"
#include "xweb/style.h"

using namespace fbx::xweb;

namespace {

class FixedText : public TextMeasurer {
public:
    float width(std::string_view s, const FontSpec& f) override { return static_cast<float>(s.size()) * f.size / 2; }
    float ascent(const FontSpec& f) override { return f.size * 0.75f; }
    float descent(const FontSpec& f) override { return f.size * 0.25f; }
};

const char* const kSeeds[] = {
    "<!doctype html><html><head><style>a{color:red}#b>c:nth-child(2n+1){margin:1em auto;width:calc(100% - 1px)}@media (min-width:1px){d{--x:1;e:var(--x)}}</style></head>"
    "<body><p>one<b>two<i>three</b>four</i><table><tr><td>x<select><option>y</select></table><ul><li>a<li>b</ul><svg><g/><foreignObject><div>z</div></foreignObject></svg>"
    "<math><mi>q</mi><annotation-xml encoding=text/html><p>r</annotation-xml></math><template><td>s</template><img src=x alt=y><input value=v><pre>\n t\n</pre>&amp;&#x41;&notin;",
    "<a href=x><div style='position:absolute;left:1px;display:inline-block;float:left;white-space:pre-wrap;font:italic 1em/2 x;border:1px solid red;opacity:.5'>a b c</div></a>"
    "<ol start=-5><li value=99999999999>x</ol><frameset><frame></frameset><!-- c --><![CDATA[d]]><?pi?></x><plaintext>y",
    "<style>@import 'x';@media print,not screen and (max-width:1e30px){a{b:c}}:not(:is(a,b)>c~d+e)[f~='g' i]::before{h:url(i) rgb(1 2 3/4%) hsl(5,6%,7%) \\41 \"\\\n\"!important}"
    "<!-- --> @supports (a:b){c{d:e}} @x{y{z:w}} a{{}}{}};;;(]) #{} .{} @{}</style><div style='width:1e30px;margin:-1e30px;font-size:1e30px;line-height:1e30;padding:1e30%;height:calc(1px/0)'>big",
};

std::string mutate(std::string s, std::mt19937& rng) {
    static const char* const kBits[] = {"<", ">", "</", "<div>", "</div>", "<table>", "<td>", "<b>", "</b>", "<a>", "<p>", "<svg>", "<template>", "<select>",
                                        "\"", "'", "=", "&", "&#", ";", "{", "}", "(", ")", ":", "@media", "var(--x)", "calc(", "\\", "\0", "\xFF", "\xC3", "--", "<!--", "-->",
                                        "display:inline-block", "position:absolute", "display:none", "white-space:pre", "100%", "auto", " ", "\n"};
    const int edits = static_cast<int>(rng() % 12) + 1;
    for (int i = 0; i < edits && !s.empty(); ++i) {
        const size_t at = rng() % s.size();
        switch (rng() % 4) {
            case 0: s.insert(at, kBits[rng() % std::size(kBits)]); break;
            case 1: s.erase(at, rng() % 16); break;
            case 2: s[at] = static_cast<char>(rng() % 256); break;
            case 3: {
                const size_t from = rng() % s.size();
                s.insert(at, s.substr(from, rng() % 64));
                break;
            }
        }
    }
    return s;
}

void throughEverything(const std::string& html, float width) {
    const auto doc = parseHtml(html);
    const StyledDocument styles(*doc, {width, 300});
    FixedText text;
    const PageLayout page = layoutPage(*doc, styles, text, nullptr, width, 300);
    (void)serializeDocument(*doc);
    for (const auto& op : page.ops) {
        EXPECT_FALSE(op.x != op.x);   // never NaN
        EXPECT_FALSE(op.y != op.y);
    }
}

}  // namespace

TEST(Fuzz, MutatedPages) {
    std::mt19937 rng(20261106);
    for (int round = 0; round < 2000; ++round) {
        std::string page = kSeeds[rng() % std::size(kSeeds)];
        const int generations = static_cast<int>(rng() % 4) + 1;
        for (int g = 0; g < generations; ++g) page = mutate(std::move(page), rng);
        throughEverything(page, static_cast<float>(rng() % 1200));
    }
}

TEST(Fuzz, RandomBytes) {
    std::mt19937 rng(7);
    for (int round = 0; round < 500; ++round) {
        std::string bytes(rng() % 4096, '\0');
        for (char& c : bytes) c = static_cast<char>(rng() % 256);
        throughEverything(bytes, 500);
        (void)css::parseStylesheet(bytes);
    }
}

TEST(Fuzz, Pathological) {
    const auto repeat = [](std::string_view s, int n) {
        std::string out;
        for (int i = 0; i < n; ++i) out += s;
        return out;
    };
    throughEverything(repeat("<div>", 100000), 500);
    throughEverything(repeat("<span>", 100000), 500);
    throughEverything(repeat("<b>", 100000), 500);
    throughEverything(repeat("<a>x", 20000), 500);
    throughEverything(repeat("<p><b>", 3000) + repeat("</b>", 3000), 500);
    throughEverything(repeat("<table><tr><td>", 3000), 500);
    throughEverything(repeat("<span style='display:inline-block'>", 400) + "x", 500);
    throughEverything(repeat("<svg>", 100000), 500);
    throughEverything(repeat("<li>", 50000), 500);
    throughEverything(repeat("a ", 200000), 500);
    throughEverything("<div style='width:1e30px;font-size:1e30px'>" + repeat("x ", 1000) + "</div>", 500);
    throughEverything("<style>" + repeat("a b c d e f g h{color:red}", 20000) + "</style>" + repeat("<a><b><c><d><e><f><g><h>", 50), 500);
    (void)css::parseStylesheet(repeat("@media x{", 100000));
    (void)css::parseStylesheet(repeat("{", 100000));
    (void)css::parseStylesheet(repeat("(", 100000));
    (void)css::parseDeclarations(repeat("a:b;", 100000));
}
