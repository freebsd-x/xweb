#include "xweb/style.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_set>

#include "ua_stylesheet.h"

namespace fbx::xweb {

using css::AttrOp;
using css::Combinator;
using css::ComplexSelector;
using css::CompoundSelector;
using css::Declaration;
using css::SimpleKind;
using css::SimpleSelector;
using css::Token;
using css::TokenType;
using Tokens = std::vector<Token>;

namespace {

constexpr int kSelectorFuel = 20000;          // match steps per element
constexpr size_t kMaxVarTokens = 4096;        // tokens one value can expand to
constexpr int kMaxVarDepth = 16;
constexpr int kMaxImportDepth = 4;
constexpr size_t kMaxImports = 64;

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
    return out;
}

bool isWsChar(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

bool hasWord(std::string_view list, std::string_view word) {
    size_t i = 0;
    while (i < list.size()) {
        while (i < list.size() && isWsChar(list[i])) ++i;
        const size_t b = i;
        while (i < list.size() && !isWsChar(list[i])) ++i;
        if (i > b && list.substr(b, i - b) == word) return true;
    }
    return false;
}

// ── Colours ───────────────────────────────────────────────────────────────────

struct NamedColor {
    const char* name;
    uint32_t rgb;
};
constexpr NamedColor kColors[] = {
    {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff}, {"aquamarine", 0x7fffd4},
    {"azure", 0xf0ffff}, {"beige", 0xf5f5dc}, {"bisque", 0xffe4c4}, {"black", 0x000000},
    {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff}, {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a},
    {"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00}, {"chocolate", 0xd2691e},
    {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c},
    {"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b}, {"darkgoldenrod", 0xb8860b},
    {"darkgray", 0xa9a9a9}, {"darkgreen", 0x006400}, {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b},
    {"darkmagenta", 0x8b008b}, {"darkolivegreen", 0x556b2f}, {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc},
    {"darkred", 0x8b0000}, {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f}, {"darkslateblue", 0x483d8b},
    {"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f}, {"darkturquoise", 0x00ced1}, {"darkviolet", 0x9400d3},
    {"deeppink", 0xff1493}, {"deepskyblue", 0x00bfff}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1e90ff}, {"firebrick", 0xb22222}, {"floralwhite", 0xfffaf0}, {"forestgreen", 0x228b22},
    {"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc}, {"ghostwhite", 0xf8f8ff}, {"gold", 0xffd700},
    {"goldenrod", 0xdaa520}, {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xadff2f},
    {"grey", 0x808080}, {"honeydew", 0xf0fff0}, {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c},
    {"indigo", 0x4b0082}, {"ivory", 0xfffff0}, {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa},
    {"lavenderblush", 0xfff0f5}, {"lawngreen", 0x7cfc00}, {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6},
    {"lightcoral", 0xf08080}, {"lightcyan", 0xe0ffff}, {"lightgoldenrodyellow", 0xfafad2}, {"lightgray", 0xd3d3d3},
    {"lightgreen", 0x90ee90}, {"lightgrey", 0xd3d3d3}, {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a},
    {"lightseagreen", 0x20b2aa}, {"lightskyblue", 0x87cefa}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xb0c4de}, {"lightyellow", 0xffffe0}, {"lime", 0x00ff00}, {"limegreen", 0x32cd32},
    {"linen", 0xfaf0e6}, {"magenta", 0xff00ff}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66cdaa},
    {"mediumblue", 0x0000cd}, {"mediumorchid", 0xba55d3}, {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371},
    {"mediumslateblue", 0x7b68ee}, {"mediumspringgreen", 0x00fa9a}, {"mediumturquoise", 0x48d1cc},
    {"mediumvioletred", 0xc71585}, {"midnightblue", 0x191970}, {"mintcream", 0xf5fffa}, {"mistyrose", 0xffe4e1},
    {"moccasin", 0xffe4b5}, {"navajowhite", 0xffdead}, {"navy", 0x000080}, {"oldlace", 0xfdf5e6},
    {"olive", 0x808000}, {"olivedrab", 0x6b8e23}, {"orange", 0xffa500}, {"orangered", 0xff4500},
    {"orchid", 0xda70d6}, {"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98}, {"paleturquoise", 0xafeeee},
    {"palevioletred", 0xdb7093}, {"papayawhip", 0xffefd5}, {"peachpuff", 0xffdab9}, {"peru", 0xcd853f},
    {"pink", 0xffc0cb}, {"plum", 0xdda0dd}, {"powderblue", 0xb0e0e6}, {"purple", 0x800080},
    {"rebeccapurple", 0x663399}, {"red", 0xff0000}, {"rosybrown", 0xbc8f8f}, {"royalblue", 0x4169e1},
    {"saddlebrown", 0x8b4513}, {"salmon", 0xfa8072}, {"sandybrown", 0xf4a460}, {"seagreen", 0x2e8b57},
    {"seashell", 0xfff5ee}, {"sienna", 0xa0522d}, {"silver", 0xc0c0c0}, {"skyblue", 0x87ceeb},
    {"slateblue", 0x6a5acd}, {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xfffafa},
    {"springgreen", 0x00ff7f}, {"steelblue", 0x4682b4}, {"tan", 0xd2b48c}, {"teal", 0x008080},
    {"thistle", 0xd8bfd8}, {"tomato", 0xff6347}, {"turquoise", 0x40e0d0}, {"violet", 0xee82ee},
    {"wheat", 0xf5deb3}, {"white", 0xffffff}, {"whitesmoke", 0xf5f5f5}, {"yellow", 0xffff00},
    {"yellowgreen", 0x9acd32},
};

uint8_t clampByte(double v) { return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L)); }

double hue(double h, double m1, double m2) {
    h = std::fmod(h, 1.0);
    if (h < 0) h += 1;
    if (h * 6 < 1) return m1 + (m2 - m1) * h * 6;
    if (h * 2 < 1) return m2;
    if (h * 3 < 2) return m1 + (m2 - m1) * (2.0 / 3 - h) * 6;
    return m1;
}

bool colorFromHash(std::string_view hex, Color& out) {
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = static_cast<char>(c | 0x20);
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    if (!std::all_of(hex.begin(), hex.end(), [&](char c) { return digit(c) >= 0; })) return false;
    const auto one = [&](size_t i) { return static_cast<uint8_t>(digit(hex[i]) * 17); };
    const auto two = [&](size_t i) { return static_cast<uint8_t>(digit(hex[i]) * 16 + digit(hex[i + 1])); };
    if (hex.size() == 3 || hex.size() == 4) {
        out = {one(0), one(1), one(2), hex.size() == 4 ? one(3) : static_cast<uint8_t>(255)};
        return true;
    }
    if (hex.size() == 6 || hex.size() == 8) {
        out = {two(0), two(2), two(4), hex.size() == 8 ? two(6) : static_cast<uint8_t>(255)};
        return true;
    }
    return false;
}

// Function arguments as numbers, whatever separates them (commas, spaces, '/').
std::vector<Token> colorArgs(const Tokens& v, size_t begin, size_t end) {
    std::vector<Token> args;
    for (size_t i = begin; i < end; ++i)
        if (v[i].type == TokenType::Number || v[i].type == TokenType::Percentage || v[i].type == TokenType::Dimension)
            args.push_back(v[i]);
    return args;
}

bool colorFromTokens(const Tokens& v, size_t& i, const Color& current, Color& out) {
    const Token& t = v[i];
    if (t.type == TokenType::Hash) {
        if (!colorFromHash(t.text, out)) return false;
        ++i;
        return true;
    }
    if (t.type == TokenType::Ident) {
        const std::string n = lower(t.text);
        ++i;
        if (n == "transparent") {
            out = {0, 0, 0, 0};
            return true;
        }
        if (n == "currentcolor") {
            out = current;
            return true;
        }
        const auto* end = std::end(kColors);
        const auto* it = std::lower_bound(std::begin(kColors), end, n,
                                          [](const NamedColor& c, const std::string& s) { return s > c.name; });
        if (it != end && n == it->name) {
            out = {static_cast<uint8_t>(it->rgb >> 16), static_cast<uint8_t>(it->rgb >> 8), static_cast<uint8_t>(it->rgb), 255};
            return true;
        }
        --i;
        return false;
    }
    if (t.type != TokenType::Function) return false;
    const bool rgb = t.text == "rgb" || t.text == "rgba";
    const bool hsl = t.text == "hsl" || t.text == "hsla";
    if (!rgb && !hsl) return false;
    size_t close = i + 1;
    while (close < v.size() && v[close].type != TokenType::RightParen) ++close;
    const auto args = colorArgs(v, i + 1, close);
    if (args.size() < 3) return false;
    const auto alpha = [&] {
        if (args.size() < 4) return 255.0;
        return args[3].type == TokenType::Percentage ? args[3].number * 255.0 / 100 : args[3].number * 255.0;
    };
    if (rgb) {
        const auto channel = [](const Token& a) { return a.type == TokenType::Percentage ? a.number * 255.0 / 100 : a.number; };
        out = {clampByte(channel(args[0])), clampByte(channel(args[1])), clampByte(channel(args[2])), clampByte(alpha())};
    } else {
        const double h = args[0].number / 360.0, s = std::clamp(args[1].number / 100.0, 0.0, 1.0),
                     l = std::clamp(args[2].number / 100.0, 0.0, 1.0);
        const double m2 = l <= 0.5 ? l * (s + 1) : l + s - l * s;
        const double m1 = l * 2 - m2;
        out = {clampByte(hue(h + 1.0 / 3, m1, m2) * 255), clampByte(hue(h, m1, m2) * 255),
               clampByte(hue(h - 1.0 / 3, m1, m2) * 255), clampByte(alpha())};
    }
    i = close < v.size() ? close + 1 : close;
    return true;
}

// ── Lengths ───────────────────────────────────────────────────────────────────

struct LengthContext {
    float font_size = 16;
    float root_font_size = 16;
    float viewport_w = 0, viewport_h = 0;
};

bool dimensionPx(const Token& t, const LengthContext& c, float& out) {
    const auto v = static_cast<float>(t.number);
    const std::string& u = t.text;
    if (u == "px") out = v;
    else if (u == "em") out = v * c.font_size;
    else if (u == "rem") out = v * c.root_font_size;
    else if (u == "ex" || u == "ch") out = v * c.font_size * 0.5f;
    else if (u == "pt") out = v * 96.0f / 72.0f;
    else if (u == "pc") out = v * 16.0f;
    else if (u == "in") out = v * 96.0f;
    else if (u == "cm") out = v * 96.0f / 2.54f;
    else if (u == "mm") out = v * 96.0f / 25.4f;
    else if (u == "q") out = v * 96.0f / 101.6f;
    else if (u == "vw") out = v * c.viewport_w / 100.0f;
    else if (u == "vh" || u == "dvh" || u == "svh" || u == "lvh") out = v * c.viewport_h / 100.0f;
    else if (u == "vmin") out = v * std::min(c.viewport_w, c.viewport_h) / 100.0f;
    else if (u == "vmax") out = v * std::max(c.viewport_w, c.viewport_h) / 100.0f;
    else return false;
    return true;
}

// calc(), min(), max(), clamp(): a small recursive descent over tokens.
class CalcParser {
public:
    CalcParser(const Tokens& v, size_t pos, const LengthContext& c) : v_(v), i_(pos), c_(c) {}

    bool sum(Length& out, int depth) {
        if (depth > 32) return false;
        if (!product(out, depth)) return false;
        while (true) {
            skipWs();
            if (i_ < v_.size() && v_[i_].type == TokenType::Delim && (v_[i_].text == "+" || v_[i_].text == "-")) {
                const bool minus = v_[i_].text == "-";
                ++i_;
                Length rhs;
                if (!product(rhs, depth)) return false;
                out.px += minus ? -rhs.px : rhs.px;
                out.percent += minus ? -rhs.percent : rhs.percent;
                continue;
            }
            return true;
        }
    }

    [[nodiscard]] size_t pos() const { return i_; }

    bool closeParen() {
        skipWs();
        if (i_ >= v_.size() || v_[i_].type != TokenType::RightParen) return false;
        ++i_;
        return true;
    }

private:
    void skipWs() {
        while (i_ < v_.size() && v_[i_].type == TokenType::Whitespace) ++i_;
    }

    bool product(Length& out, int depth) {
        bool is_number = false;
        double number = 0;
        if (!value(out, is_number, number, depth)) return false;
        while (true) {
            skipWs();
            if (i_ < v_.size() && v_[i_].type == TokenType::Delim && (v_[i_].text == "*" || v_[i_].text == "/")) {
                const bool divide = v_[i_].text == "/";
                ++i_;
                Length rhs;
                bool rhs_number = false;
                double rn = 0;
                if (!value(rhs, rhs_number, rn, depth)) return false;
                if (divide) {
                    if (!rhs_number || rn == 0) return false;
                    if (is_number) number /= rn;
                    else { out.px /= static_cast<float>(rn); out.percent /= static_cast<float>(rn); }
                } else if (rhs_number) {
                    if (is_number) number *= rn;
                    else { out.px *= static_cast<float>(rn); out.percent *= static_cast<float>(rn); }
                } else if (is_number) {
                    out.px = rhs.px * static_cast<float>(number);
                    out.percent = rhs.percent * static_cast<float>(number);
                    is_number = false;
                } else {
                    return false;
                }
                continue;
            }
            break;
        }
        if (is_number) out.px = static_cast<float>(number);
        return true;
    }

    bool value(Length& out, bool& is_number, double& number, int depth) {
        skipWs();
        if (i_ >= v_.size()) return false;
        const Token& t = v_[i_];
        if (t.type == TokenType::Number) {
            is_number = true;
            number = t.number;
            ++i_;
            return true;
        }
        if (t.type == TokenType::Percentage) {
            out.percent = static_cast<float>(t.number);
            ++i_;
            return true;
        }
        if (t.type == TokenType::Dimension) {
            if (!dimensionPx(t, c_, out.px)) return false;
            ++i_;
            return true;
        }
        if (t.type == TokenType::LeftParen || (t.type == TokenType::Function && t.text == "calc")) {
            ++i_;
            return sum(out, depth + 1) && closeParen();
        }
        if (t.type == TokenType::Function && (t.text == "min" || t.text == "max" || t.text == "clamp")) {
            const std::string fn = t.text;
            ++i_;
            std::vector<Length> args;
            while (true) {
                Length a;
                if (!sum(a, depth + 1)) return false;
                args.push_back(a);
                skipWs();
                if (i_ < v_.size() && v_[i_].type == TokenType::Comma) {
                    ++i_;
                    continue;
                }
                break;
            }
            if (!closeParen() || args.empty()) return false;
            // With percentages in play the base isn't known here; the first argument
            // is the usual "preferred" one, so take it.
            const bool any_percent = std::any_of(args.begin(), args.end(), [](const Length& l) { return l.hasPercent(); });
            if (any_percent) {
                out = fn == "clamp" && args.size() == 3 ? args[1] : args[0];
                return true;
            }
            float r = args[0].px;
            if (fn == "min") for (const auto& a : args) r = std::min(r, a.px);
            else if (fn == "max") for (const auto& a : args) r = std::max(r, a.px);
            else if (args.size() == 3) r = std::max(args[0].px, std::min(args[1].px, args[2].px));
            out.px = r;
            return true;
        }
        return false;
    }

    const Tokens& v_;
    size_t i_;
    const LengthContext& c_;
};

// One length at v[i], which moves past it.
bool lengthAt(const Tokens& v, size_t& i, const LengthContext& c, Length& out, bool allow_auto) {
    if (i >= v.size()) return false;
    const Token& t = v[i];
    if (t.type == TokenType::Ident && allow_auto && lower(t.text) == "auto") {
        out = Length::autoLength();
        ++i;
        return true;
    }
    if (t.type == TokenType::Number && t.number == 0) {
        out = Length::pixels(0);
        ++i;
        return true;
    }
    if (t.type == TokenType::Percentage) {
        out = Length{};
        out.percent = static_cast<float>(t.number);
        ++i;
        return true;
    }
    if (t.type == TokenType::Dimension) {
        out = Length{};
        if (!dimensionPx(t, c, out.px)) return false;
        ++i;
        return true;
    }
    if (t.type == TokenType::Function && (t.text == "calc" || t.text == "min" || t.text == "max" || t.text == "clamp")) {
        CalcParser calc(v, i, c);
        Length l;
        if (!calc.sum(l, 0)) return false;
        out = l;
        i = calc.pos();
        return true;
    }
    return false;
}

void skipWs(const Tokens& v, size_t& i) {
    while (i < v.size() && v[i].type == TokenType::Whitespace) ++i;
}

// Top-level values of a declaration, without whitespace, functions kept whole by
// index (start of each value in `v`).
std::vector<size_t> valueStarts(const Tokens& v) {
    std::vector<size_t> starts;
    int depth = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        const TokenType ty = v[i].type;
        if (depth == 0 && ty != TokenType::Whitespace && ty != TokenType::RightParen) starts.push_back(i);
        if (ty == TokenType::Function || ty == TokenType::LeftParen) ++depth;
        else if (ty == TokenType::RightParen && depth > 0) --depth;
    }
    return starts;
}

std::string keyword(const Tokens& v) {
    size_t i = 0;
    skipWs(v, i);
    if (i >= v.size() || v[i].type != TokenType::Ident) return {};
    std::string w = lower(v[i].text);
    ++i;
    skipWs(v, i);
    return i == v.size() ? w : std::string{};
}

float borderWidthFrom(const Token& t, const LengthContext& c, bool& ok) {
    ok = true;
    if (t.type == TokenType::Ident) {
        const std::string n = lower(t.text);
        if (n == "thin") return 1;
        if (n == "medium") return 3;
        if (n == "thick") return 5;
    }
    if (t.type == TokenType::Number && t.number == 0) return 0;
    float px = 0;
    if (t.type == TokenType::Dimension && dimensionPx(t, c, px)) return std::max(0.0f, px);
    ok = false;
    return 0;
}

bool isBorderStyle(std::string_view s) {
    static constexpr std::string_view v[] = {"none", "hidden", "dotted", "dashed", "solid", "double", "groove", "ridge", "inset", "outset"};
    return std::find(std::begin(v), std::end(v), s) != std::end(v);
}

}  // namespace

bool parseColor(const Tokens& value, const Color& current, Color& out) {
    size_t i = 0;
    skipWs(value, i);
    if (i >= value.size() || !colorFromTokens(value, i, current, out)) return false;
    skipWs(value, i);
    return i == value.size();
}

// ── The resolver ──────────────────────────────────────────────────────────────

enum class Origin { UserAgent, Hint, Author, Inline };

struct MatchedDecl {
    const Declaration* decl;
    bool important;
    Origin origin;
    uint64_t specificity;
    size_t order;
};

struct IndexedSelector {
    const ComplexSelector* selector;
    const css::Rule* rule;
    Origin origin;
    size_t order;
};

class StyleResolver {
public:
    StyleResolver(const Document& doc, StyledDocument& out, const StyledDocument::SheetLoader& loader)
        : doc_(doc), out_(out), loader_(loader) {
        lengths_.viewport_w = out.media_.width;
        lengths_.viewport_h = out.media_.height;
        addSheet(css::parseStylesheet(detail::kUserAgentStyle), Origin::UserAgent, 0);
        collectSheets();
        indexSiblings();
    }

    void run() {
        const Node* root = doc_.documentElement();
        if (!root) return;
        ComputedStyle initial;
        std::vector<const Node*> stack;
        walk(root, initial, 0);
    }

private:
    void addSheet(css::Stylesheet sheet, Origin origin, int depth) {
        if (loader_ && depth < kMaxImportDepth) {
            for (const std::string& url : sheet.imports) {
                if (++import_count_ > kMaxImports) break;
                addSheet(css::parseStylesheet(loader_(url)), origin, depth + 1);
            }
        }
        sheets_.push_back(std::make_unique<css::Stylesheet>(std::move(sheet)));
        const css::Stylesheet& kept = *sheets_.back();
        for (const css::Rule& rule : kept.rules) {
            const bool applies = std::all_of(rule.media.begin(), rule.media.end(), [&](const Tokens& q) {
                return css::mediaMatches(q, out_.media_);
            });
            if (!applies) continue;
            for (const ComplexSelector& sel : rule.selectors) {
                if (sel.parts.empty()) continue;
                index(sel, rule, origin);
            }
        }
    }

    void index(const ComplexSelector& sel, const css::Rule& rule, Origin origin) {
        const CompoundSelector& last = sel.parts.back();
        // Generated content and the like: no box to style yet.
        for (const SimpleSelector& s : last.simple)
            if (s.kind == SimpleKind::PseudoElement) return;
        const IndexedSelector entry{&sel, &rule, origin, ++order_};
        for (const SimpleSelector& s : last.simple) {
            if (s.kind == SimpleKind::Id) { by_id_[s.name].push_back(entry); return; }
        }
        for (const SimpleSelector& s : last.simple) {
            if (s.kind == SimpleKind::Class) { by_class_[s.name].push_back(entry); return; }
        }
        for (const SimpleSelector& s : last.simple) {
            if (s.kind == SimpleKind::Type) { by_type_[s.name].push_back(entry); return; }
        }
        universal_.push_back(entry);
    }

    void collectSheets() {
        std::vector<const Node*> todo{doc_.root()};
        while (!todo.empty()) {
            const Node* n = todo.back();
            todo.pop_back();
            if (n->isHtml("style")) {
                const std::string* media = n->attr("media");
                if (!media || css::mediaMatches(css::tokenize(*media), out_.media_))
                    addSheet(css::parseStylesheet(n->textContent()), Origin::Author, 0);
            } else if (n->isHtml("link") && loader_) {
                const std::string* rel = n->attr("rel");
                const std::string* href = n->attr("href");
                if (rel && href && hasWord(lower(*rel), "stylesheet") && !hasWord(lower(*rel), "alternate")) {
                    const std::string* media = n->attr("media");
                    if (!media || css::mediaMatches(css::tokenize(*media), out_.media_))
                        addSheet(css::parseStylesheet(loader_(*href)), Origin::Author, 0);
                }
            }
            todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        }
    }

    struct SiblingInfo {
        const Node* prev = nullptr;
        const Node* next = nullptr;
        int index = 0;      // 1-based among element siblings
        int count = 0;
    };

    void indexSiblings() {
        std::vector<const Node*> todo{doc_.root()};
        while (!todo.empty()) {
            const Node* n = todo.back();
            todo.pop_back();
            const Node* prev = nullptr;
            int index = 0;
            for (const Node* c : n->children) {
                if (!c->isElement()) continue;
                SiblingInfo info;
                info.prev = prev;
                info.index = ++index;
                if (prev) siblings_[prev].next = c;
                siblings_[c] = info;
                prev = c;
                todo.push_back(c);
            }
            for (const Node* c : n->children)
                if (c->isElement()) siblings_[c].count = index;
        }
    }

    // ── Matching ──────────────────────────────────────────────────────────────

    bool matchSimple(const Node* el, const SimpleSelector& s) {
        switch (s.kind) {
            case SimpleKind::Universal: return true;
            case SimpleKind::Type: return el->name == s.name || (el->ns != Namespace::Html && lower(el->name) == s.name);
            case SimpleKind::Id: {
                const std::string* v = el->attr("id");
                return v && *v == s.name;
            }
            case SimpleKind::Class: {
                const std::string* v = el->attr("class");
                return v && hasWord(*v, s.name);
            }
            case SimpleKind::Attribute: return matchAttribute(el, s);
            case SimpleKind::PseudoClass: return matchPseudo(el, s);
            case SimpleKind::PseudoElement: return false;
        }
        return false;
    }

    static bool matchAttribute(const Node* el, const SimpleSelector& s) {
        const std::string* found = el->attr(s.name);
        if (!found) return false;
        if (s.op == AttrOp::Exists) return true;
        std::string have = *found, want = s.value;
        if (s.case_insensitive || s.name == "type") {
            have = lower(have);
            want = lower(want);
        }
        switch (s.op) {
            case AttrOp::Equals: return have == want;
            case AttrOp::Includes: return !want.empty() && hasWord(have, want);
            case AttrOp::DashMatch: return have == want || have.starts_with(want + "-");
            case AttrOp::Prefix: return !want.empty() && have.starts_with(want);
            case AttrOp::Suffix: return !want.empty() && have.ends_with(want);
            case AttrOp::Substring: return !want.empty() && have.find(want) != std::string::npos;
            case AttrOp::Exists: return true;
        }
        return false;
    }

    static bool nthMatches(int a, int b, int index) {
        if (a == 0) return index == b;
        const int diff = index - b;
        return diff % a == 0 && diff / a >= 0;
    }

    bool matchPseudo(const Node* el, const SimpleSelector& s) {
        const std::string& n = s.name;
        const auto sibIt = siblings_.find(el);
        const SiblingInfo info = sibIt == siblings_.end() ? SiblingInfo{} : sibIt->second;
        if (n == "not") {
            for (const ComplexSelector& a : s.args)
                if (matchComplex(el, a)) return false;
            return true;
        }
        if (n == "is" || n == "where") {
            for (const ComplexSelector& a : s.args)
                if (matchComplex(el, a)) return true;
            return false;
        }
        if (n == "root") return el->parent && el->parent->type == NodeType::Document;
        if (n == "first-child") return info.index == 1;
        if (n == "last-child") return info.index == info.count;
        if (n == "only-child") return info.count == 1;
        if (n == "nth-child") return nthMatches(s.a, s.b, info.index);
        if (n == "nth-last-child") return nthMatches(s.a, s.b, info.count - info.index + 1);
        if (n == "first-of-type" || n == "last-of-type" || n == "only-of-type" || n == "nth-of-type" || n == "nth-last-of-type") {
            int before = 0, after = 0;
            for (const Node* p = info.prev; p && fuel_-- > 0; p = siblings_[p].prev)
                if (p->name == el->name) ++before;
            for (const Node* p = info.next; p && fuel_-- > 0; p = siblings_[p].next)
                if (p->name == el->name) ++after;
            if (n == "first-of-type") return before == 0;
            if (n == "last-of-type") return after == 0;
            if (n == "only-of-type") return before == 0 && after == 0;
            if (n == "nth-of-type") return nthMatches(s.a, s.b, before + 1);
            return nthMatches(s.a, s.b, after + 1);
        }
        if (n == "empty") {
            return std::none_of(el->children.begin(), el->children.end(), [](const Node* c) {
                return c->isElement() || (c->type == NodeType::Text && !c->data.empty());
            });
        }
        if (n == "link" || n == "any-link" || n == "-webkit-any-link")
            return (el->isHtml("a") || el->isHtml("area")) && el->attr("href");
        if (n == "checked") return el->attr("checked") != nullptr || el->attr("selected") != nullptr;
        if (n == "disabled") return el->attr("disabled") != nullptr;
        if (n == "enabled") return (el->isHtml("input") || el->isHtml("button") || el->isHtml("select") || el->isHtml("textarea")) && !el->attr("disabled");
        if (n == "lang") {
            for (const Node* p = el; p; p = p->parent)
                if (const std::string* v = p->attr("lang")) return lower(*v).starts_with(lower(s.value));
            return false;
        }
        // :hover, :visited, :focus and other states of a page nobody has touched yet.
        return false;
    }

    bool matchCompound(const Node* el, const CompoundSelector& c) {
        for (const SimpleSelector& s : c.simple) {
            if (--fuel_ < 0) return false;
            if (!matchSimple(el, s)) return false;
        }
        return true;
    }

    bool matchFrom(const Node* el, const ComplexSelector& sel, size_t part) {
        if (fuel_ < 0 || !matchCompound(el, sel.parts[part])) return false;
        if (part == 0) return true;
        const size_t before = part - 1;
        switch (sel.parts[part].combinator) {
            case Combinator::Descendant:
                for (const Node* p = el->parent; p && p->isElement(); p = p->parent)
                    if (matchFrom(p, sel, before)) return true;
                return false;
            case Combinator::Child:
                return el->parent && el->parent->isElement() && matchFrom(el->parent, sel, before);
            case Combinator::NextSibling: {
                const auto it = siblings_.find(el);
                return it != siblings_.end() && it->second.prev && matchFrom(it->second.prev, sel, before);
            }
            case Combinator::SubsequentSibling: {
                const auto it = siblings_.find(el);
                for (const Node* p = it == siblings_.end() ? nullptr : it->second.prev; p; p = siblings_[p].prev)
                    if (matchFrom(p, sel, before)) return true;
                return false;
            }
        }
        return false;
    }

    bool matchComplex(const Node* el, const ComplexSelector& sel) {
        return !sel.parts.empty() && matchFrom(el, sel, sel.parts.size() - 1);
    }

    void gather(const Node* el, const std::vector<IndexedSelector>& list, std::vector<MatchedDecl>& out) {
        for (const IndexedSelector& e : list) {
            if (!matchComplex(el, *e.selector)) continue;
            for (const Declaration& d : e.rule->declarations)
                out.push_back({&d, d.important, e.origin, e.selector->specificity.key(), e.order});
        }
    }

    // ── Presentational hints ──────────────────────────────────────────────────

    static void hints(const Node* el, std::string& css_text) {
        if (el->ns != Namespace::Html) return;
        const auto dimension = [&](std::string_view attr_name, std::string_view prop) {
            const std::string* v = el->attr(attr_name);
            if (!v || v->empty()) return;
            const bool plain = std::all_of(v->begin(), v->end(), [](char c) { return c >= '0' && c <= '9'; });
            if (plain) css_text += std::string(prop) + ":" + *v + "px;";
            else if (v->back() == '%') css_text += std::string(prop) + ":" + *v + ";";
        };
        const std::string& n = el->name;
        if (n == "img" || n == "table" || n == "td" || n == "th" || n == "video" || n == "canvas" || n == "iframe" ||
            n == "embed" || n == "object" || n == "hr") {
            dimension("width", "width");
            dimension("height", "height");
        }
        if (const std::string* v = el->attr("bgcolor")) css_text += "background-color:" + *v + ";";
        if (n == "body") {
            if (const std::string* v = el->attr("text")) css_text += "color:" + *v + ";";
            if (const std::string* v = el->attr("link")) (void)v;
        }
        if (n == "font") {
            if (const std::string* v = el->attr("color")) css_text += "color:" + *v + ";";
            if (const std::string* v = el->attr("face")) css_text += "font-family:" + *v + ";";
            if (const std::string* v = el->attr("size")) {
                static constexpr const char* kSizes[] = {"x-small", "small", "medium", "large", "x-large", "xx-large", "48px"};
                if (v->size() == 1 && (*v)[0] >= '1' && (*v)[0] <= '7')
                    css_text += std::string("font-size:") + kSizes[(*v)[0] - '1'] + ";";
            }
        }
        if (const std::string* v = el->attr("align")) {
            const std::string a = lower(*v);
            if ((n == "div" || n == "p" || n == "h1" || n == "h2" || n == "h3" || n == "h4" || n == "h5" || n == "h6" ||
                 n == "td" || n == "th" || n == "tr") &&
                (a == "center" || a == "left" || a == "right"))
                css_text += "text-align:" + a + ";";
        }
        if (n == "img" || n == "table") {
            if (const std::string* v = el->attr("border")) {
                const bool plain = !v->empty() && std::all_of(v->begin(), v->end(), [](char c) { return c >= '0' && c <= '9'; });
                if (plain) css_text += "border:" + *v + "px solid gray;";
            }
        }
    }

    // ── Values ────────────────────────────────────────────────────────────────

    // Replaces var() in a value. False: the declaration is invalid at computed time.
    bool substitute(const Tokens& in, const ComputedStyle& s, Tokens& out, int depth, size_t& budget) {
        if (depth > kMaxVarDepth) return false;
        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i].type != TokenType::Function || in[i].text != "var") {
                if (budget == 0) return false;
                --budget;
                out.push_back(in[i]);
                continue;
            }
            size_t j = i + 1, nesting = 1;
            while (j < in.size() && nesting > 0) {
                if (in[j].type == TokenType::Function || in[j].type == TokenType::LeftParen) ++nesting;
                else if (in[j].type == TokenType::RightParen) --nesting;
                ++j;
            }
            size_t k = i + 1;
            skipWs(in, k);
            if (k >= j || in[k].type != TokenType::Ident) return false;
            const std::string& name = in[k].text;
            // The fallback, if any, is what follows the first comma.
            size_t comma = k + 1;
            while (comma < j && in[comma].type != TokenType::Comma) ++comma;
            bool done = false;
            if (s.custom) {
                const auto it = s.custom->find(name);
                if (it != s.custom->end()) {
                    if (!substitute(it->second, s, out, depth + 1, budget)) return false;
                    done = true;
                }
            }
            if (!done) {
                if (comma + 1 >= j) return false;
                const Tokens fallback(in.begin() + static_cast<std::ptrdiff_t>(comma + 1),
                                      in.begin() + static_cast<std::ptrdiff_t>(j - 1));
                if (!substitute(fallback, s, out, depth + 1, budget)) return false;
            }
            i = j - 1;
        }
        return true;
    }

    static bool hasVar(const Tokens& v) {
        return std::any_of(v.begin(), v.end(), [](const Token& t) { return t.type == TokenType::Function && t.text == "var"; });
    }

    [[nodiscard]] LengthContext lengthContext(const ComputedStyle& s) const {
        LengthContext c = lengths_;
        c.font_size = s.font_size;
        return c;
    }

    void setBoxSides(Length* sides, const Tokens& v, const LengthContext& c, bool allow_auto) {
        Length got[4];
        int n = 0;
        size_t i = 0;
        while (n < 4) {
            skipWs(v, i);
            if (i >= v.size()) break;
            if (!lengthAt(v, i, c, got[n], allow_auto)) return;
            ++n;
        }
        skipWs(v, i);
        if (n == 0 || i < v.size()) return;
        sides[Top] = got[0];
        sides[Right] = n > 1 ? got[1] : got[0];
        sides[Bottom] = n > 2 ? got[2] : got[0];
        sides[Left] = n > 3 ? got[3] : sides[Right];
    }

    static void noNegative(Length& l) {
        // A mix like calc(100% - 40px) is only known once resolved.
        if (l.hasPercent()) return;
        l.px = std::max(l.px, 0.0f);
    }

    void setBorder(ComputedStyle& s, const Tokens& v, std::initializer_list<int> sides) {
        const LengthContext c = lengthContext(s);
        float width = 3;
        bool none = true;
        Color color = s.color;
        for (size_t start : valueStarts(v)) {
            const Token& t = v[start];
            bool ok = false;
            const float w = borderWidthFrom(t, c, ok);
            if (ok) {
                width = w;
                continue;
            }
            if (t.type == TokenType::Ident && isBorderStyle(lower(t.text))) {
                const std::string n = lower(t.text);
                none = n == "none" || n == "hidden";
                continue;
            }
            size_t i = start;
            Color parsed;
            if (colorFromTokens(v, i, s.color, parsed)) color = parsed;
        }
        for (int side : sides) {
            s.border_width[side] = width;
            border_none_[side] = none;
            s.border_color[side] = color;
        }
    }

    // A keyword size, as a share of "medium".
    static float keywordScale(const std::string& k) {
        if (k == "xx-small") return 9.0f / 16;
        if (k == "x-small") return 10.0f / 16;
        if (k == "small") return 13.0f / 16;
        if (k == "medium") return 1;
        if (k == "large") return 18.0f / 16;
        if (k == "x-large") return 1.5f;
        if (k == "xx-large") return 2;
        if (k == "xxx-large") return 3;
        return 0;
    }

    static bool isFontSizeKeyword(const std::string& k) {
        return keywordScale(k) > 0 || k == "smaller" || k == "larger";
    }

    // "medium" is 16px, but 13px in the monospace generic family: every browser does
    // this, and pages' <pre> and <code> sizes depend on it. So a size that came from a
    // keyword stays a keyword until the family is known (finishFontSize).
    void setFontSize(ComputedStyle& s, const Tokens& v, size_t& i, float parent, float parent_keyword) {
        if (i < v.size() && v[i].type == TokenType::Ident) {
            const std::string k = lower(v[i].text);
            if (!isFontSizeKeyword(k)) return;
            ++i;
            if (k == "smaller" || k == "larger") {
                const float factor = k == "larger" ? 1.2f : 1 / 1.2f;
                if (parent_keyword > 0) {
                    s.font_keyword = parent_keyword * factor;
                } else {
                    s.font_keyword = 0;
                    s.font_size = parent * factor;
                }
            } else {
                s.font_keyword = keywordScale(k);
            }
            return;
        }
        LengthContext c = lengths_;
        c.font_size = parent;   // em in font-size means the parent's
        Length l;
        if (lengthAt(v, i, c, l, false)) {
            s.font_keyword = 0;
            s.font_size = std::clamp(l.resolve(parent), 0.0f, 2000.0f);
        }
    }

    static void finishFontSize(ComputedStyle& s) {
        if (s.font_keyword > 0) s.font_size = s.font_keyword * (lower(s.font_family) == "monospace" ? 13.0f : 16.0f);
    }

    static void setFontWeight(ComputedStyle& s, const Token& t, int parent) {
        if (t.type == TokenType::Number) {
            s.font_weight = std::clamp(static_cast<int>(t.number), 1, 1000);
            return;
        }
        const std::string k = lower(t.text);
        if (k == "normal") s.font_weight = 400;
        else if (k == "bold") s.font_weight = 700;
        else if (k == "bolder") s.font_weight = parent >= 600 ? 900 : (parent >= 350 ? 700 : 400);
        else if (k == "lighter") s.font_weight = parent >= 800 ? 700 : (parent >= 550 ? 400 : 100);
    }

    static void setFontFamily(ComputedStyle& s, const Tokens& v, size_t i) {
        // The first family; we can't tell yet whether a later one is the installed one.
        std::string name;
        for (; i < v.size(); ++i) {
            const Token& t = v[i];
            if (t.type == TokenType::Comma) break;
            if (t.type == TokenType::Whitespace) {
                if (!name.empty()) name += ' ';
            } else if (t.type == TokenType::Ident || t.type == TokenType::String) {
                name += t.text;
            }
        }
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (!name.empty()) s.font_family = name;
    }

    static void setLineHeight(ComputedStyle& s, const Token& t, const LengthContext& c, const Tokens& v, size_t i) {
        if (t.type == TokenType::Number) {
            s.line_height_factor = std::max(0.0f, static_cast<float>(t.number));
            s.line_height = 0;
            return;
        }
        if (t.type == TokenType::Ident && lower(t.text) == "normal") {
            s.line_height_factor = 0;
            s.line_height = 0;
            return;
        }
        Length l;
        size_t k = i;
        if (lengthAt(v, k, c, l, false)) {
            s.line_height_factor = 0;
            s.line_height = std::max(0.0f, l.resolve(s.font_size));
        }
    }

    void setFontShorthand(ComputedStyle& s, const Tokens& v, const ComputedStyle& parent) {
        // [style || weight || variant]? size[/line-height]? family
        size_t i = 0;
        ComputedStyle next = s;
        next.italic = false;
        next.font_weight = 400;
        next.line_height = 0;
        next.line_height_factor = 0;
        bool have_size = false;
        while (i < v.size()) {
            skipWs(v, i);
            if (i >= v.size()) break;
            const Token& t = v[i];
            if (t.type == TokenType::Ident) {
                const std::string k = lower(t.text);
                if (k == "italic" || k == "oblique") { next.italic = true; ++i; continue; }
                if (k == "normal" || k == "small-caps") { ++i; continue; }
                if (k == "bold" || k == "bolder" || k == "lighter") { setFontWeight(next, t, parent.font_weight); ++i; continue; }
                if (!isFontSizeKeyword(k)) break;
                setFontSize(next, v, i, parent.font_size, parent.font_keyword);
                have_size = true;
                break;
            }
            if (t.type == TokenType::Number && t.number >= 1) {
                setFontWeight(next, t, parent.font_weight);
                ++i;
                continue;
            }
            setFontSize(next, v, i, parent.font_size, parent.font_keyword);
            have_size = true;
            break;
        }
        if (!have_size) return;   // a system font keyword, or nonsense: leave things alone
        if (i < v.size() && v[i].type == TokenType::Delim && v[i].text == "/") {
            ++i;
            skipWs(v, i);
            if (i < v.size()) {
                setLineHeight(next, v[i], lengthContext(next), v, i);
                ++i;
                while (i < v.size() && v[i].type != TokenType::Whitespace) ++i;
            }
        }
        skipWs(v, i);
        if (i < v.size()) setFontFamily(next, v, i);
        s = std::move(next);
    }

    static void setDisplay(ComputedStyle& s, const std::string& k) {
        if (k == "none") s.display = Display::None;
        else if (k == "inline" || k == "contents" || k == "ruby" || k == "ruby-text") s.display = Display::Inline;
        else if (k == "inline-block" || k == "inline-flex" || k == "inline-grid" || k == "inline-table" ||
                 k == "table-cell")
            s.display = Display::InlineBlock;
        else if (k == "list-item") s.display = Display::ListItem;
        else if (k == "block" || k == "flex" || k == "grid" || k == "flow-root" || k == "table" || k == "table-row" ||
                 k == "table-row-group" || k == "table-header-group" || k == "table-footer-group" ||
                 k == "table-caption" || k == "flow")
            s.display = Display::Block;
    }

    static bool copyInherited(ComputedStyle& to, const ComputedStyle& from, const std::string& p) {
        if (p == "color") to.color = from.color;
        else if (p == "font-size") to.font_size = from.font_size;
        else if (p == "font-weight") to.font_weight = from.font_weight;
        else if (p == "font-style") to.italic = from.italic;
        else if (p == "font-family") to.font_family = from.font_family;
        else if (p == "font") {
            to.font_size = from.font_size;
            to.font_weight = from.font_weight;
            to.italic = from.italic;
            to.font_family = from.font_family;
            to.line_height = from.line_height;
            to.line_height_factor = from.line_height_factor;
        } else if (p == "line-height") {
            to.line_height = from.line_height;
            to.line_height_factor = from.line_height_factor;
        } else if (p == "text-align") to.text_align = from.text_align;
        else if (p == "text-transform") to.text_transform = from.text_transform;
        else if (p == "white-space") to.white_space = from.white_space;
        else if (p == "list-style-type" || p == "list-style") to.list_style = from.list_style;
        else if (p == "text-indent") to.text_indent = from.text_indent;
        else if (p == "visibility") to.hidden = from.hidden;
        else return false;
        return true;
    }

    static void copyAny(ComputedStyle& to, const ComputedStyle& from, const std::string& p) {
        if (copyInherited(to, from, p)) return;
        const auto sides = [&](auto& dst, const auto& src, std::string_view base) {
            if (p == base) std::copy(std::begin(src), std::end(src), std::begin(dst));
            else if (p == std::string(base) + "-top") dst[Top] = src[Top];
            else if (p == std::string(base) + "-right") dst[Right] = src[Right];
            else if (p == std::string(base) + "-bottom") dst[Bottom] = src[Bottom];
            else if (p == std::string(base) + "-left") dst[Left] = src[Left];
        };
        sides(to.margin, from.margin, "margin");
        sides(to.padding, from.padding, "padding");
        if (p == "display") to.display = from.display;
        else if (p == "position") to.position = from.position;
        else if (p == "width") to.width = from.width;
        else if (p == "height") to.height = from.height;
        else if (p == "min-width") to.min_width = from.min_width;
        else if (p == "min-height") to.min_height = from.min_height;
        else if (p == "max-width") to.max_width = from.max_width;
        else if (p == "max-height") to.max_height = from.max_height;
        else if (p == "background-color" || p == "background") to.background = from.background;
        else if (p == "opacity") to.opacity = from.opacity;
        else if (p == "box-sizing") to.border_box = from.border_box;
        else if (p == "z-index") to.z_index = from.z_index;
        else if (p.starts_with("border")) {
            std::copy(std::begin(from.border_width), std::end(from.border_width), std::begin(to.border_width));
            std::copy(std::begin(from.border_color), std::end(from.border_color), std::begin(to.border_color));
        }
        else if (p == "top") to.offset[Top] = from.offset[Top];
        else if (p == "right") to.offset[Right] = from.offset[Right];
        else if (p == "bottom") to.offset[Bottom] = from.offset[Bottom];
        else if (p == "left") to.offset[Left] = from.offset[Left];
    }

    void apply(ComputedStyle& s, const ComputedStyle& parent, const std::string& p, const Tokens& raw) {
        Tokens substituted;
        const Tokens* vp = &raw;
        if (hasVar(raw)) {
            size_t budget = kMaxVarTokens;
            if (!substitute(raw, s, substituted, 0, budget)) {
                // Invalid at computed time: as if "unset".
                ComputedStyle initial;
                if (copyInherited(s, parent, p)) return;
                copyAny(s, initial, p);
                return;
            }
            vp = &substituted;
        }
        const Tokens& v = *vp;
        const std::string kw = keyword(v);
        if (p.starts_with("border") && (kw == "inherit" || kw == "initial" || kw == "unset" || kw == "revert")) {
            const bool from_parent = kw == "inherit";
            for (int side = 0; side < 4; ++side) {
                s.border_width[side] = from_parent ? parent.border_width[side] : 3.0f;
                s.border_color[side] = from_parent ? parent.border_color[side] : s.color;
                border_none_[side] = from_parent ? parent.border_width[side] == 0 : true;
            }
            return;
        }
        if (kw == "inherit") {
            copyAny(s, parent, p);
            return;
        }
        if (kw == "initial") {
            ComputedStyle initial;
            copyAny(s, initial, p);
            return;
        }
        if (kw == "unset" || kw == "revert" || kw == "revert-layer") {
            if (!copyInherited(s, parent, p)) {
                ComputedStyle initial;
                copyAny(s, initial, p);
            }
            return;
        }
        size_t i = 0;
        skipWs(v, i);
        const LengthContext c = lengthContext(s);
        const auto oneLength = [&](Length& into, bool allow_auto) {
            size_t k = i;
            Length l;
            if (lengthAt(v, k, c, l, allow_auto)) into = l;
        };
        const auto oneColor = [&](Color& into) {
            Color parsed;
            if (parseColor(v, s.color, parsed)) into = parsed;
        };

        if (p == "display") { if (!kw.empty()) setDisplay(s, kw); return; }
        if (p == "position") {
            if (kw == "static" || kw == "sticky") s.position = Position::Static;
            else if (kw == "relative") s.position = Position::Relative;
            else if (kw == "absolute") s.position = Position::Absolute;
            else if (kw == "fixed") s.position = Position::Fixed;
            return;
        }
        if (p == "top") { oneLength(s.offset[Top], true); return; }
        if (p == "right") { oneLength(s.offset[Right], true); return; }
        if (p == "bottom") { oneLength(s.offset[Bottom], true); return; }
        if (p == "left") { oneLength(s.offset[Left], true); return; }
        if (p == "z-index") {
            if (i < v.size() && v[i].type == TokenType::Number)
                s.z_index = static_cast<int>(std::clamp(v[i].number, -1e6, 1e6));
            return;
        }
        if (p == "visibility") { s.hidden = kw == "hidden" || kw == "collapse"; return; }
        if (p == "opacity") {
            if (i < v.size() && v[i].type == TokenType::Number) s.opacity = std::clamp(static_cast<float>(v[i].number), 0.0f, 1.0f);
            else if (i < v.size() && v[i].type == TokenType::Percentage) s.opacity = std::clamp(static_cast<float>(v[i].number) / 100.0f, 0.0f, 1.0f);
            return;
        }
        if (p == "box-sizing") { s.border_box = kw == "border-box"; return; }
        if (p == "background-color") { oneColor(s.background); return; }
        if (p == "background") {
            // Colours only, for now: images and gradients need more than a rectangle.
            s.background = {0, 0, 0, 0};
            for (size_t start : valueStarts(v)) {
                size_t k = start;
                Color parsed;
                if (colorFromTokens(v, k, s.color, parsed)) s.background = parsed;
            }
            return;
        }
        if (p == "margin") { setBoxSides(s.margin, v, c, true); return; }
        if (p == "padding") {
            setBoxSides(s.padding, v, c, false);
            for (Length& l : s.padding) noNegative(l);
            return;
        }
        if (p == "margin-top") { oneLength(s.margin[Top], true); return; }
        if (p == "margin-right") { oneLength(s.margin[Right], true); return; }
        if (p == "margin-bottom") { oneLength(s.margin[Bottom], true); return; }
        if (p == "margin-left") { oneLength(s.margin[Left], true); return; }
        if (p == "padding-top") { oneLength(s.padding[Top], false); noNegative(s.padding[Top]); return; }
        if (p == "padding-right") { oneLength(s.padding[Right], false); noNegative(s.padding[Right]); return; }
        if (p == "padding-bottom") { oneLength(s.padding[Bottom], false); noNegative(s.padding[Bottom]); return; }
        if (p == "padding-left") { oneLength(s.padding[Left], false); noNegative(s.padding[Left]); return; }
        if (p == "margin-inline") {
            Length pair[4];
            setBoxSides(pair, v, c, true);
            s.margin[Left] = pair[Top];
            s.margin[Right] = pair[Right];
            return;
        }
        if (p == "margin-block") {
            Length pair[4];
            setBoxSides(pair, v, c, true);
            s.margin[Top] = pair[Top];
            s.margin[Bottom] = pair[Right];
            return;
        }
        if (p == "padding-inline") {
            Length pair[4];
            setBoxSides(pair, v, c, false);
            s.padding[Left] = pair[Top];
            s.padding[Right] = pair[Right];
            return;
        }
        if (p == "padding-block") {
            Length pair[4];
            setBoxSides(pair, v, c, false);
            s.padding[Top] = pair[Top];
            s.padding[Bottom] = pair[Right];
            return;
        }
        if (p == "width") { oneLength(s.width, true); noNegative(s.width); return; }
        if (p == "height") { oneLength(s.height, true); noNegative(s.height); return; }
        if (p == "min-width") { oneLength(s.min_width, true); if (s.min_width.is_auto) s.min_width = Length{}; noNegative(s.min_width); return; }
        if (p == "min-height") { oneLength(s.min_height, true); if (s.min_height.is_auto) s.min_height = Length{}; noNegative(s.min_height); return; }
        if (p == "max-width") { if (kw == "none") s.max_width = Length::autoLength(); else { oneLength(s.max_width, false); noNegative(s.max_width); } return; }
        if (p == "max-height") { if (kw == "none") s.max_height = Length::autoLength(); else { oneLength(s.max_height, false); noNegative(s.max_height); } return; }
        if (p == "border") { setBorder(s, v, {Top, Right, Bottom, Left}); return; }
        if (p == "border-top") { setBorder(s, v, {Top}); return; }
        if (p == "border-right") { setBorder(s, v, {Right}); return; }
        if (p == "border-bottom") { setBorder(s, v, {Bottom}); return; }
        if (p == "border-left") { setBorder(s, v, {Left}); return; }
        if (p == "border-width" || p == "border-top-width" || p == "border-right-width" || p == "border-bottom-width" || p == "border-left-width") {
            std::vector<float> widths;
            for (size_t start : valueStarts(v)) {
                bool ok = false;
                const float w = borderWidthFrom(v[start], c, ok);
                if (!ok) return;
                widths.push_back(w);
            }
            if (widths.empty()) return;
            if (p == "border-top-width") s.border_width[Top] = widths[0];
            else if (p == "border-right-width") s.border_width[Right] = widths[0];
            else if (p == "border-bottom-width") s.border_width[Bottom] = widths[0];
            else if (p == "border-left-width") s.border_width[Left] = widths[0];
            else {
                s.border_width[Top] = widths[0];
                s.border_width[Right] = widths.size() > 1 ? widths[1] : widths[0];
                s.border_width[Bottom] = widths.size() > 2 ? widths[2] : widths[0];
                s.border_width[Left] = widths.size() > 3 ? widths[3] : s.border_width[Right];
            }
            return;
        }
        if (p == "border-style" || p == "border-top-style" || p == "border-right-style" || p == "border-bottom-style" || p == "border-left-style") {
            const auto starts = valueStarts(v);
            const auto noneAt = [&](size_t idx) {
                if (idx >= starts.size()) return false;
                const std::string n = lower(v[starts[idx]].text);
                return n == "none" || n == "hidden";
            };
            const auto setSide = [&](int side, bool none) { border_none_[side] = none; };
            if (p == "border-top-style") setSide(Top, noneAt(0));
            else if (p == "border-right-style") setSide(Right, noneAt(0));
            else if (p == "border-bottom-style") setSide(Bottom, noneAt(0));
            else if (p == "border-left-style") setSide(Left, noneAt(0));
            else {
                const size_t n = starts.size();
                setSide(Top, noneAt(0));
                setSide(Right, noneAt(n > 1 ? 1 : 0));
                setSide(Bottom, noneAt(n > 2 ? 2 : 0));
                setSide(Left, noneAt(n > 3 ? 3 : (n > 1 ? 1 : 0)));
            }
            return;
        }
        if (p == "border-color" || p == "border-top-color" || p == "border-right-color" || p == "border-bottom-color" || p == "border-left-color") {
            std::vector<Color> colors;
            for (size_t start : valueStarts(v)) {
                size_t k = start;
                Color parsed;
                if (!colorFromTokens(v, k, s.color, parsed)) return;
                colors.push_back(parsed);
            }
            if (colors.empty()) return;
            if (p == "border-top-color") s.border_color[Top] = colors[0];
            else if (p == "border-right-color") s.border_color[Right] = colors[0];
            else if (p == "border-bottom-color") s.border_color[Bottom] = colors[0];
            else if (p == "border-left-color") s.border_color[Left] = colors[0];
            else {
                s.border_color[Top] = colors[0];
                s.border_color[Right] = colors.size() > 1 ? colors[1] : colors[0];
                s.border_color[Bottom] = colors.size() > 2 ? colors[2] : colors[0];
                s.border_color[Left] = colors.size() > 3 ? colors[3] : s.border_color[Right];
            }
            return;
        }
        if (p == "font-weight") { if (i < v.size()) setFontWeight(s, v[i], parent.font_weight); return; }
        if (p == "font-style") { s.italic = kw == "italic" || kw == "oblique" || (i < v.size() && lower(v[i].text) == "oblique"); return; }
        if (p == "line-height") { if (i < v.size()) setLineHeight(s, v[i], c, v, i); return; }
        if (p == "text-align") {
            if (kw == "left" || kw == "start" || kw == "justify" || kw == "-webkit-auto") s.text_align = TextAlign::Left;
            else if (kw == "right" || kw == "end" || kw == "-webkit-right") s.text_align = TextAlign::Right;
            else if (kw == "center" || kw == "-webkit-center") s.text_align = TextAlign::Center;
            return;
        }
        if (p == "text-transform") {
            if (kw == "none") s.text_transform = TextTransform::None;
            else if (kw == "uppercase") s.text_transform = TextTransform::Uppercase;
            else if (kw == "lowercase") s.text_transform = TextTransform::Lowercase;
            else if (kw == "capitalize") s.text_transform = TextTransform::Capitalize;
            return;
        }
        if (p == "white-space") {
            if (kw == "normal") s.white_space = WhiteSpace::Normal;
            else if (kw == "pre") s.white_space = WhiteSpace::Pre;
            else if (kw == "nowrap") s.white_space = WhiteSpace::NoWrap;
            else if (kw == "pre-wrap" || kw == "break-spaces") s.white_space = WhiteSpace::PreWrap;
            else if (kw == "pre-line") s.white_space = WhiteSpace::PreLine;
            return;
        }
        if (p == "text-indent") { oneLength(s.text_indent, false); return; }
        if (p == "text-decoration" || p == "text-decoration-line") {
            for (size_t start : valueStarts(v)) {
                if (v[start].type != TokenType::Ident) continue;
                const std::string k = lower(v[start].text);
                if (k == "underline") s.underline = true;
                else if (k == "line-through") s.line_through = true;
                else if (k == "none") {
                    // Only stops this element's own; an ancestor's line still crosses it,
                    // which the walk restores after the cascade.
                    s.underline = false;
                    s.line_through = false;
                }
            }
            return;
        }
        if (p == "list-style-type" || p == "list-style") {
            for (size_t start : valueStarts(v)) {
                if (v[start].type != TokenType::Ident) continue;
                const std::string k = lower(v[start].text);
                if (k == "none") s.list_style = ListStyle::None;
                else if (k == "disc") s.list_style = ListStyle::Disc;
                else if (k == "circle") s.list_style = ListStyle::Circle;
                else if (k == "square") s.list_style = ListStyle::Square;
                else if (k == "decimal" || k == "decimal-leading-zero") s.list_style = ListStyle::Decimal;
                else if (k == "lower-alpha" || k == "lower-latin") s.list_style = ListStyle::LowerAlpha;
                else if (k == "upper-alpha" || k == "upper-latin") s.list_style = ListStyle::UpperAlpha;
                else if (k == "lower-roman") s.list_style = ListStyle::LowerRoman;
                else if (k == "upper-roman") s.list_style = ListStyle::UpperRoman;
            }
            return;
        }
    }

    void resolveInto(const Node* el, const ComputedStyle& parent, ComputedStyle& s) {
        // Start from what inherits.
        s.color = parent.color;
        s.font_size = parent.font_size;
        s.font_keyword = parent.font_keyword;
        s.font_weight = parent.font_weight;
        s.italic = parent.italic;
        s.font_family = parent.font_family;
        s.line_height = parent.line_height;
        s.line_height_factor = parent.line_height_factor;
        s.text_align = parent.text_align;
        s.text_transform = parent.text_transform;
        s.white_space = parent.white_space;
        s.list_style = parent.list_style;
        s.text_indent = parent.text_indent;
        s.hidden = parent.hidden;
        s.custom = parent.custom;

        fuel_ = kSelectorFuel;
        std::vector<MatchedDecl> matched;
        const auto lookup = [&](const auto& table, const std::string& key) {
            const auto it = table.find(key);
            if (it != table.end()) gather(el, it->second, matched);
        };
        if (const std::string* id = el->attr("id")) lookup(by_id_, *id);
        if (const std::string* cls = el->attr("class")) {
            std::string_view list = *cls;
            std::set<std::string> seen;
            size_t i = 0;
            while (i < list.size()) {
                while (i < list.size() && isWsChar(list[i])) ++i;
                const size_t b = i;
                while (i < list.size() && !isWsChar(list[i])) ++i;
                if (i > b) {
                    std::string word(list.substr(b, i - b));
                    if (seen.insert(word).second) lookup(by_class_, word);
                }
            }
        }
        lookup(by_type_, el->ns == Namespace::Html ? el->name : lower(el->name));
        gather(el, universal_, matched);

        std::string hint_text;
        hints(el, hint_text);
        std::vector<Declaration> hint_decls;
        if (!hint_text.empty()) hint_decls = css::parseDeclarations(hint_text);
        for (const Declaration& d : hint_decls) matched.push_back({&d, false, Origin::Hint, 0, 0});

        std::vector<Declaration> inline_decls;
        if (const std::string* style_attr = el->attr("style")) inline_decls = css::parseDeclarations(*style_attr);
        for (const Declaration& d : inline_decls) matched.push_back({&d, d.important, Origin::Inline, 0, 0});

        const auto rank = [](const MatchedDecl& m) {
            // Normal: UA < hints < author < inline. Important reverses UA vs author.
            if (m.important) return m.origin == Origin::UserAgent ? 6 : 5;
            return static_cast<int>(m.origin);
        };
        std::stable_sort(matched.begin(), matched.end(), [&](const MatchedDecl& a, const MatchedDecl& b) {
            const int ra = rank(a), rb = rank(b);
            if (ra != rb) return ra < rb;
            if (a.specificity != b.specificity) return a.specificity < b.specificity;
            return a.order < b.order;
        });

        // Custom properties first, everything can refer to them.
        std::map<std::string, Tokens> custom;
        bool has_custom = false;
        for (const MatchedDecl& m : matched) {
            if (m.decl->name.starts_with("--")) {
                if (!has_custom) {
                    if (s.custom) custom = *s.custom;
                    has_custom = true;
                }
                custom[m.decl->name] = m.decl->value;
            }
        }
        if (has_custom) s.custom = std::make_shared<const std::map<std::string, Tokens>>(std::move(custom));

        // Then what other values are computed from: the font size and the colour.
        const auto first = [](const std::string& p) {
            return p == "font-size" || p == "font" || p == "font-family" || p == "color";
        };
        for (const MatchedDecl& m : matched) {
            const std::string& p = m.decl->name;
            if (p == "font") {
                Tokens sub;
                size_t budget = kMaxVarTokens;
                const Tokens* v = &m.decl->value;
                if (hasVar(*v)) {
                    if (!substitute(*v, s, sub, 0, budget)) continue;
                    v = &sub;
                }
                setFontShorthand(s, *v, parent);
            } else if (p == "font-size") {
                Tokens sub;
                size_t budget = kMaxVarTokens;
                const Tokens* v = &m.decl->value;
                if (hasVar(*v)) {
                    if (!substitute(*v, s, sub, 0, budget)) continue;
                    v = &sub;
                }
                const std::string kw = keyword(*v);
                if (kw == "inherit" || kw == "unset" || kw == "revert") {
                    s.font_size = parent.font_size;
                    s.font_keyword = parent.font_keyword;
                    continue;
                }
                if (kw == "initial") {
                    s.font_keyword = 1;
                    continue;
                }
                size_t i = 0;
                skipWs(*v, i);
                setFontSize(s, *v, i, parent.font_size, parent.font_keyword);
            } else if (p == "font-family") {
                Tokens sub;
                size_t budget = kMaxVarTokens;
                const Tokens* v = &m.decl->value;
                if (hasVar(*v)) {
                    if (!substitute(*v, s, sub, 0, budget)) continue;
                    v = &sub;
                }
                const std::string kw = keyword(*v);
                if (kw == "inherit" || kw == "unset" || kw == "revert") {
                    s.font_family = parent.font_family;
                    continue;
                }
                if (kw == "initial") {
                    s.font_family.clear();
                    continue;
                }
                size_t i = 0;
                skipWs(*v, i);
                setFontFamily(s, *v, i);
            } else if (p == "color") {
                Tokens sub;
                size_t budget = kMaxVarTokens;
                const Tokens* v = &m.decl->value;
                if (hasVar(*v)) {
                    if (!substitute(*v, s, sub, 0, budget)) continue;
                    v = &sub;
                }
                const std::string kw = keyword(*v);
                if (kw == "inherit" || kw == "unset" || kw == "revert" || kw == "currentcolor") { s.color = parent.color; continue; }
                if (kw == "initial") { s.color = {0, 0, 0, 255}; continue; }
                Color c;
                if (parseColor(*v, parent.color, c)) s.color = c;
            }
        }
        finishFontSize(s);
        if (el->parent && el->parent->type == NodeType::Document) lengths_.root_font_size = s.font_size;

        // Borders default to the text's colour, medium, and no style: a width only
        // shows once there's a style, and setting one doesn't change the other.
        for (Color& c : s.border_color) c = s.color;
        std::fill(std::begin(s.border_width), std::end(s.border_width), 3.0f);
        std::fill(std::begin(border_none_), std::end(border_none_), true);

        for (const MatchedDecl& m : matched) {
            const std::string& p = m.decl->name;
            if (first(p) || p.starts_with("--")) continue;
            apply(s, parent, p, m.decl->value);
        }

        for (int side = 0; side < 4; ++side)
            if (border_none_[side]) s.border_width[side] = 0;

        // Absolute and fixed boxes are blocks, whatever they said.
        if ((s.position == Position::Absolute || s.position == Position::Fixed) &&
            (s.display == Display::Inline || s.display == Display::InlineBlock))
            s.display = Display::Block;

        // Lines drawn by an ancestor run through its descendants' text regardless.
        s.underline = s.underline || parent.underline;
        s.line_through = s.line_through || parent.line_through;
    }

    void walk(const Node* el, const ComputedStyle& parent, size_t depth) {
        if (depth > kMaxStyledDepth) return;
        out_.storage_.emplace_back();
        ComputedStyle& s = out_.storage_.back();
        resolveInto(el, parent, s);
        out_.styles_[el] = &s;
        if (s.display == Display::None) return;   // nothing below gets a box
        for (const Node* c : el->children)
            if (c->isElement()) walk(c, s, depth + 1);
    }

    const Document& doc_;
    StyledDocument& out_;
    const StyledDocument::SheetLoader& loader_;
    std::vector<std::unique_ptr<css::Stylesheet>> sheets_;
    std::unordered_map<std::string, std::vector<IndexedSelector>> by_id_, by_class_, by_type_;
    std::vector<IndexedSelector> universal_;
    std::unordered_map<const Node*, SiblingInfo> siblings_;
    LengthContext lengths_;
    size_t order_ = 0;
    size_t import_count_ = 0;
    int fuel_ = 0;
    bool border_none_[4] = {true, true, true, true};
};

StyledDocument::StyledDocument(const Document& doc, css::MediaContext media, const SheetLoader& loader) : media_(media) {
    StyleResolver resolver(doc, *this, loader);
    resolver.run();
}

const ComputedStyle* StyledDocument::styleOf(const Node* element) const {
    const auto it = styles_.find(element);
    return it == styles_.end() ? nullptr : it->second;
}

}  // namespace fbx::xweb
