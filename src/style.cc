#include "xweb/style.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_set>

#include "ua_stylesheet.h"
#include "xweb/url.h"

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
    if (lower(t.text) == "light-dark") {
        // We're always in light mode. The nesting the grammar would allow isn't followed.
        ++i;
        while (i < v.size() && v[i].type == TokenType::Whitespace) ++i;
        if (i >= v.size() || (v[i].type == TokenType::Function && lower(v[i].text) == "light-dark")) return false;
        Color light;
        if (!colorFromTokens(v, i, current, light)) return false;
        // An rgb() or hsl() leaves us on its own first token, so its brackets are counted too.
        int nesting = 1;
        for (; i < v.size() && nesting > 0; ++i) {
            if (v[i].type == TokenType::Function || v[i].type == TokenType::LeftParen) ++nesting;
            else if (v[i].type == TokenType::RightParen) --nesting;
        }
        if (nesting != 0) return false;
        out = light;
        return true;
    }
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
    float ch = 0;   // the width of "0" in this font, if known
    float line_height = 0, root_line_height = 0;   // for lh and rlh (0: 1.2 of the font size)
};

bool dimensionPx(const Token& t, const LengthContext& c, float& out) {
    const auto v = static_cast<float>(t.number);
    const std::string& u = t.text;
    if (u == "px") out = v;
    else if (u == "em") out = v * c.font_size;
    else if (u == "rem") out = v * c.root_font_size;
    else if (u == "ch") out = v * (c.ch > 0 ? c.ch : c.font_size * 0.5f);
    else if (u == "ex") out = v * c.font_size * 0.5f;
    else if (u == "lh") out = v * (c.line_height > 0 ? c.line_height : c.font_size * 1.2f);
    else if (u == "rlh") out = v * (c.root_line_height > 0 ? c.root_line_height : c.root_font_size * 1.2f);
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

// ── Grid values ───────────────────────────────────────────────────────────────

constexpr size_t kMaxGridListTracks = 1000;   // what one template may spell out

bool gridSizeAt(const Tokens& v, size_t& i, const LengthContext& c, GridSize& out) {
    skipWs(v, i);
    if (i >= v.size()) return false;
    const Token& t = v[i];
    if (t.type == TokenType::Ident) {
        const std::string k = lower(t.text);
        if (k == "auto") out.kind = GridSize::Auto;
        else if (k == "min-content") out.kind = GridSize::MinContent;
        else if (k == "max-content") out.kind = GridSize::MaxContent;
        else return false;
        ++i;
        return true;
    }
    if (t.type == TokenType::Dimension && lower(t.text) == "fr") {
        if (t.number < 0) return false;
        out.kind = GridSize::Fr;
        out.fr = static_cast<float>(std::min(t.number, 1e6));
        ++i;
        return true;
    }
    Length l;
    if (!lengthAt(v, i, c, l, false) || l.px < 0 || l.percent < 0) return false;
    out.kind = GridSize::Fixed;
    out.length = l;
    return true;
}

bool closesFunction(const Tokens& v, size_t& i) {
    skipWs(v, i);
    if (i >= v.size() || v[i].type != TokenType::RightParen) return false;
    ++i;
    return true;
}

bool gridTrackAt(const Tokens& v, size_t& i, const LengthContext& c, GridTrack& out) {
    skipWs(v, i);
    if (i >= v.size()) return false;
    if (v[i].type == TokenType::Function && lower(v[i].text) == "minmax") {
        ++i;
        if (!gridSizeAt(v, i, c, out.min) || out.min.kind == GridSize::Fr) return false;
        skipWs(v, i);
        if (i >= v.size() || v[i].type != TokenType::Comma) return false;
        ++i;
        if (!gridSizeAt(v, i, c, out.max)) return false;
        return closesFunction(v, i);
    }
    if (v[i].type == TokenType::Function && lower(v[i].text) == "fit-content") {
        ++i;
        skipWs(v, i);
        out.min.kind = GridSize::Auto;
        out.max.kind = GridSize::MaxContent;
        out.fit_content = true;
        if (!lengthAt(v, i, c, out.fit_limit, false)) return false;
        return closesFunction(v, i);
    }
    GridSize size;
    if (!gridSizeAt(v, i, c, size)) return false;
    out.max = size;
    out.min = size;
    if (size.kind == GridSize::Fr) out.min = GridSize{};
    return true;
}

// [ name name ], adding to what a line is already called.
bool lineNamesAt(const Tokens& v, size_t& i, std::vector<std::string>& into) {
    skipWs(v, i);
    if (i >= v.size() || v[i].type != TokenType::LeftBracket) return false;
    ++i;
    while (i < v.size() && v[i].type != TokenType::RightBracket) {
        if (v[i].type == TokenType::Ident) into.push_back(v[i].text);
        ++i;
    }
    if (i < v.size()) ++i;
    return true;
}

bool isSlash(const Token& t) { return t.type == TokenType::Delim && t.text == "/"; }

// A track list up to `end` or a slash.
bool trackListAt(const Tokens& v, size_t& i, const LengthContext& c, GridTrackList& out) {
    out.names.assign(1, {});
    bool any = false, have_repeat = false;
    while (true) {
        skipWs(v, i);
        if (i >= v.size() || isSlash(v[i])) break;
        std::vector<std::string>& line = have_repeat && out.repeat_at == out.tracks.size() ? out.names_after_repeat : out.names.back();
        if (lineNamesAt(v, i, line)) continue;
        if (v[i].type == TokenType::Function && lower(v[i].text) == "repeat") {
            ++i;
            skipWs(v, i);
            if (i >= v.size()) return false;
            int count = 0;
            bool automatic = false;
            if (v[i].type == TokenType::Ident && (lower(v[i].text) == "auto-fill" || lower(v[i].text) == "auto-fit")) {
                automatic = true;
                out.auto_fit = lower(v[i].text) == "auto-fit";
            } else if (v[i].type == TokenType::Number && v[i].is_integer && v[i].number >= 1) {
                count = static_cast<int>(std::min(v[i].number, 1e6));
            } else {
                return false;
            }
            ++i;
            skipWs(v, i);
            if (i >= v.size() || v[i].type != TokenType::Comma) return false;
            ++i;
            std::vector<GridTrack> tracks;
            std::vector<std::vector<std::string>> names(1);
            while (true) {
                skipWs(v, i);
                if (i >= v.size()) return false;
                if (v[i].type == TokenType::RightParen) { ++i; break; }
                if (lineNamesAt(v, i, names.back())) continue;
                GridTrack t;
                if (!gridTrackAt(v, i, c, t)) return false;
                tracks.push_back(std::move(t));
                names.emplace_back();
                if (tracks.size() > kMaxGridListTracks) return false;
            }
            if (tracks.empty()) return false;
            if (automatic) {
                if (have_repeat) return false;   // only one may be automatic
                have_repeat = true;
                out.repeat = std::move(tracks);
                out.repeat_names = std::move(names);
                out.repeat_at = out.tracks.size();
            } else {
                for (int n = 0; n < count && out.tracks.size() < kMaxGridListTracks; ++n) {
                    for (size_t t = 0; t < tracks.size() && out.tracks.size() < kMaxGridListTracks; ++t) {
                        auto& before = out.names.back();
                        before.insert(before.end(), names[t].begin(), names[t].end());
                        out.tracks.push_back(tracks[t]);
                        out.names.emplace_back();
                    }
                    auto& after = out.names.back();
                    after.insert(after.end(), names.back().begin(), names.back().end());
                }
            }
            any = true;
            continue;
        }
        GridTrack t;
        if (!gridTrackAt(v, i, c, t)) return false;
        out.tracks.push_back(std::move(t));
        out.names.emplace_back();
        any = true;
        if (out.tracks.size() > kMaxGridListTracks) return false;
    }
    return any;
}

// The rows of grid-template-areas. A name has to make a rectangle.
bool addAreaRow(const std::string& row, GridAreas& out, std::map<std::string, int>& cells) {
    int column = 0;
    size_t pos = 0;
    while (pos < row.size()) {
        const unsigned char ch = static_cast<unsigned char>(row[pos]);
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\f' || ch == '\r') { ++pos; continue; }
        if (ch == '.') {
            while (pos < row.size() && row[pos] == '.') ++pos;
            ++column;
            continue;
        }
        const size_t from = pos;
        while (pos < row.size() && row[pos] != ' ' && row[pos] != '\t' && row[pos] != '\n' && row[pos] != '\f' && row[pos] != '\r' && row[pos] != '.') ++pos;
        const std::string name = row.substr(from, pos - from);
        auto [it, fresh] = out.areas.try_emplace(name, GridAreaRect{out.rows, out.rows + 1, column, column + 1});
        if (!fresh) {
            it->second.row_end = out.rows + 1;
            it->second.column_start = std::min(it->second.column_start, column);
            it->second.column_end = std::max(it->second.column_end, column + 1);
        }
        ++cells[name];
        ++column;
    }
    if (column == 0) return false;
    if (out.rows > 0 && column != out.columns) return false;
    out.columns = column;
    ++out.rows;
    return static_cast<size_t>(out.rows) * static_cast<size_t>(out.columns) <= kMaxGridListTracks * 10;
}

bool areasAreRectangles(const GridAreas& a, const std::map<std::string, int>& cells) {
    for (const auto& [name, r] : a.areas)
        if ((r.row_end - r.row_start) * (r.column_end - r.column_start) != cells.at(name)) return false;
    return true;
}

// One end of a placement, up to a slash.
bool gridLineAt(const Tokens& v, size_t& i, GridLine& out) {
    out = GridLine{};
    bool span = false, any = false;
    int number = 0;
    for (; i < v.size() && !isSlash(v[i]); ++i) {
        const Token& t = v[i];
        if (t.type == TokenType::Whitespace) continue;
        any = true;
        if (t.type == TokenType::Number && t.is_integer && t.number != 0) {
            number = static_cast<int>(std::clamp(t.number, -100000.0, 100000.0));
        } else if (t.type == TokenType::Ident) {
            const std::string k = lower(t.text);
            if (k == "span") span = true;
            else if (k != "auto") out.name = t.text;
        } else {
            return false;
        }
    }
    if (!any) return false;
    if (span) {
        out.span = number > 0 ? number : 1;
    } else {
        out.line = number;
    }
    return true;
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

// ── Backgrounds, masks and gradients ──────────────────────────────────────────

constexpr size_t kMaxBackgroundLayers = 32;
constexpr size_t kMaxGradientStops = 256;

// A value's top-level comma-separated parts.
std::vector<Tokens> commaParts(const Tokens& v, size_t begin = 0, size_t end = SIZE_MAX) {
    std::vector<Tokens> parts(1);
    int depth = 0;
    end = std::min(end, v.size());
    for (size_t i = begin; i < end; ++i) {
        const TokenType ty = v[i].type;
        if (ty == TokenType::Function || ty == TokenType::LeftParen) ++depth;
        else if (ty == TokenType::RightParen && depth > 0) --depth;
        if (depth == 0 && ty == TokenType::Comma) {
            parts.emplace_back();
            continue;
        }
        parts.back().push_back(v[i]);
    }
    return parts;
}

// Where the function opened at v[i] closes.
size_t closingParen(const Tokens& v, size_t i) {
    int depth = 0;
    for (; i < v.size(); ++i) {
        if (v[i].type == TokenType::Function || v[i].type == TokenType::LeftParen) ++depth;
        else if (v[i].type == TokenType::RightParen && --depth == 0) return i;
    }
    return v.size();
}

bool angleOf(const Token& t, float& deg) {
    if (t.type == TokenType::Number && t.number == 0) {
        deg = 0;
        return true;
    }
    if (t.type != TokenType::Dimension) return false;
    const std::string u = lower(t.text);
    const auto n = static_cast<float>(t.number);
    if (u == "deg") deg = n;
    else if (u == "rad") deg = n * 180.0f / 3.14159265f;
    else if (u == "grad") deg = n * 0.9f;
    else if (u == "turn") deg = n * 360.0f;
    else return false;
    return std::isfinite(deg);
}

// A url() at v[i], which moves past it.
bool urlAt(const Tokens& v, size_t& i, std::string& out) {
    if (i >= v.size()) return false;
    if (v[i].type == TokenType::Url) {
        out = v[i].text;
        ++i;
        return true;
    }
    if (v[i].type == TokenType::Function && lower(v[i].text) == "url") {
        size_t k = i + 1;
        skipWs(v, k);
        if (k >= v.size() || v[k].type != TokenType::String) return false;
        out = v[k].text;
        i = closingParen(v, i) + 1;
        return true;
    }
    return false;
}

// left/center/right/top/bottom as a share along their axis; which axis, in `vertical`.
bool positionKeyword(const std::string& k, float& percent, int& axis) {
    if (k == "left") { percent = 0; axis = 0; }
    else if (k == "right") { percent = 100; axis = 0; }
    else if (k == "top") { percent = 0; axis = 1; }
    else if (k == "bottom") { percent = 100; axis = 1; }
    else if (k == "center") { percent = 50; axis = -1; }
    else return false;
    return true;
}

// A <position> (background-position, "at" in gradients) from the items in `v`, all of
// which it must use.
bool positionFrom(const Tokens& v, const LengthContext& c, Length& x, Length& y) {
    struct Item { bool is_kw; std::string kw; Length len; };
    std::vector<Item> items;
    for (size_t start : valueStarts(v)) {
        size_t k = start;
        if (v[k].type == TokenType::Ident) {
            items.push_back({true, lower(v[k].text), {}});
            continue;
        }
        Length l;
        if (!lengthAt(v, k, c, l, false)) return false;
        items.push_back({false, {}, l});
    }
    if (items.empty() || items.size() > 4) return false;
    const auto pct = [](float p) { Length l; l.percent = p; return l; };
    if (items.size() <= 2) {
        Length out[2] = {pct(50), pct(50)};
        int axes[2] = {-1, -1};
        for (size_t n = 0; n < items.size(); ++n) {
            if (items[n].is_kw) {
                float p = 0;
                if (!positionKeyword(items[n].kw, p, axes[n])) return false;
                out[n] = pct(p);
            } else {
                out[n] = items[n].len;
                axes[n] = -2;   // a length: its place says which axis
            }
        }
        if (items.size() == 1) {
            if (axes[0] == 1) { x = pct(50); y = out[0]; } else { x = out[0]; y = pct(50); }
            return true;
        }
        const bool swap = axes[0] == 1 || axes[1] == 0;
        if (swap && (axes[0] == -2 || axes[1] == -2)) return false;   // "top 10px" isn't two values
        if (axes[0] == axes[1] && axes[0] >= 0) return false;
        x = swap ? out[1] : out[0];
        y = swap ? out[0] : out[1];
        return true;
    }
    // Three or four: keywords, each maybe with an offset from that edge.
    bool have_x = false, have_y = false;
    Length rx = pct(50), ry = pct(50);
    for (size_t n = 0; n < items.size(); ++n) {
        if (!items[n].is_kw) return false;
        float p = 0;
        int axis = 0;
        if (!positionKeyword(items[n].kw, p, axis)) return false;
        Length l = pct(p);
        if (n + 1 < items.size() && !items[n + 1].is_kw) {
            if (axis == -1) return false;   // center takes no offset
            const Length off = items[++n].len;
            if (p == 100) { l.px -= off.px; l.percent -= off.percent; }
            else { l.px += off.px; l.percent += off.percent; }
        }
        if (axis == -1) axis = have_x ? 1 : 0;
        if (axis == 0) { if (have_x) return false; have_x = true; rx = l; }
        else { if (have_y) return false; have_y = true; ry = l; }
    }
    x = rx;
    y = ry;
    return true;
}

// A gradient function at v[i] (its Function token); moves past it.
std::shared_ptr<const Gradient> gradientAt(const Tokens& v, size_t& i, const LengthContext& c, const Color& current) {
    std::string name = lower(v[i].text);
    const bool legacy = name.starts_with("-webkit-") || name.starts_with("-moz-") || name.starts_with("-o-");
    if (legacy) name = name.substr(name.find('-', 1) + 1);
    auto g = std::make_shared<Gradient>();
    if (name.starts_with("repeating-")) {
        g->repeating = true;
        name = name.substr(10);
    }
    if (name == "linear-gradient") g->kind = Gradient::Kind::Linear;
    else if (name == "radial-gradient") g->kind = Gradient::Kind::Radial;
    else if (name == "conic-gradient") g->kind = Gradient::Kind::Conic;
    else return nullptr;
    const size_t close = closingParen(v, i);
    std::vector<Tokens> parts = commaParts(v, i + 1, close);
    i = close + 1;
    if (parts.empty()) return nullptr;

    // The first part says how, unless it's already a stop.
    size_t first_stop = 0;
    {
        const Tokens& head = parts[0];
        const std::vector<size_t> starts = valueStarts(head);
        size_t k0 = starts.empty() ? 0 : starts[0];
        Color probe;
        const bool is_stop = !starts.empty() && colorFromTokens(head, k0, current, probe);
        if (!is_stop && !starts.empty()) {
            first_stop = 1;
            if (g->kind == Gradient::Kind::Linear) {
                float deg = 0;
                if (starts.size() == 1 && angleOf(head[starts[0]], deg)) {
                    // The old prefixed syntax counted from "to right", anticlockwise.
                    g->angle = legacy ? 90 - deg : deg;
                } else {
                    size_t n = 0;
                    if (!legacy) {
                        if (head[starts[0]].type != TokenType::Ident || lower(head[starts[0]].text) != "to") return nullptr;
                        n = 1;
                    }
                    if (n >= starts.size()) return nullptr;
                    for (; n < starts.size(); ++n) {
                        if (head[starts[n]].type != TokenType::Ident) return nullptr;
                        const std::string k = lower(head[starts[n]].text);
                        // The prefixed form names where it starts, not where it goes.
                        const int sign = legacy ? -1 : 1;
                        if (k == "left") g->to_x = -sign;
                        else if (k == "right") g->to_x = sign;
                        else if (k == "top") g->to_y = -sign;
                        else if (k == "bottom") g->to_y = sign;
                        else if (!(legacy && k == "center")) return nullptr;
                    }
                    if (g->to_x == 0 && g->to_y == 0) g->to_y = 1;
                    if (g->to_x == 0) g->angle = g->to_y < 0 ? 0 : 180;
                    else if (g->to_y == 0) g->angle = g->to_x < 0 ? 270 : 90;
                }
            } else {
                // Before "at": shape, extent or size (radial), "from <angle>" (conic).
                size_t n = 0;
                std::vector<Length> sizes;
                for (; n < starts.size(); ++n) {
                    const Token& t = head[starts[n]];
                    if (t.type == TokenType::Ident) {
                        const std::string k = lower(t.text);
                        if (k == "at") break;
                        if (g->kind == Gradient::Kind::Conic) {
                            if (k != "from" || n + 1 >= starts.size()) return nullptr;
                            float deg = 0;
                            if (!angleOf(head[starts[++n]], deg)) return nullptr;
                            g->from = deg;
                            continue;
                        }
                        if (k == "circle") g->circle = true;
                        else if (k == "ellipse") g->circle = false;
                        else if (k == "closest-side" || k == "contain") g->extent = Gradient::Extent::ClosestSide;
                        else if (k == "closest-corner") g->extent = Gradient::Extent::ClosestCorner;
                        else if (k == "farthest-side") g->extent = Gradient::Extent::FarthestSide;
                        else if (k == "farthest-corner" || k == "cover") g->extent = Gradient::Extent::FarthestCorner;
                        else return nullptr;
                        continue;
                    }
                    if (g->kind == Gradient::Kind::Conic) return nullptr;
                    size_t k = starts[n];
                    Length l;
                    if (!lengthAt(head, k, c, l, false)) return nullptr;
                    sizes.push_back(l);
                }
                if (sizes.size() == 1) {
                    g->circle = true;
                    g->explicit_size = true;
                    g->radius_x = g->radius_y = sizes[0];
                } else if (sizes.size() == 2) {
                    g->explicit_size = true;
                    g->radius_x = sizes[0];
                    g->radius_y = sizes[1];
                } else if (sizes.size() > 2) {
                    return nullptr;
                }
                if (n < starts.size()) {
                    Tokens where(head.begin() + static_cast<std::ptrdiff_t>(starts[n] + 1), head.end());
                    if (!positionFrom(where, c, g->center_x, g->center_y)) return nullptr;
                }
            }
        }
    }
    for (size_t p = first_stop; p < parts.size() && g->stops.size() < kMaxGradientStops; ++p) {
        const Tokens& part = parts[p];
        const std::vector<size_t> starts = valueStarts(part);
        if (starts.empty()) return nullptr;
        size_t k = starts[0];
        GradientStop stop;
        if (!colorFromTokens(part, k, current, stop.color)) {
            // A lone position is a hint for where halfway falls: drawn as if it weren't there.
            Length l;
            k = starts[0];
            if (starts.size() == 1 && (lengthAt(part, k, c, l, false) || angleOf(part[starts[0]], l.px))) continue;
            return nullptr;
        }
        std::vector<Length> at;
        for (size_t n = 1; n < starts.size(); ++n) {
            size_t q = starts[n];
            Length l;
            float deg = 0;
            if (g->kind == Gradient::Kind::Conic && angleOf(part[q], deg)) l.percent = deg / 3.6f;
            else if (!lengthAt(part, q, c, l, false)) return nullptr;
            at.push_back(l);
        }
        if (at.size() > 2) return nullptr;
        if (at.empty()) {
            g->stops.push_back(stop);
        } else {
            for (const Length& l : at) {
                stop.has_position = true;
                stop.position = l;
                g->stops.push_back(stop);
            }
        }
    }
    if (g->stops.empty()) return nullptr;
    if (g->stops.size() == 1) g->stops.push_back(g->stops[0]);
    return g;
}

// One layer's image at v[i]; moves past it.
bool backgroundImageAt(const Tokens& v, size_t& i, const LengthContext& c, const Color& current, BackgroundImage& out) {
    if (i >= v.size()) return false;
    if (v[i].type == TokenType::Ident && lower(v[i].text) == "none") {
        out = {};
        ++i;
        return true;
    }
    std::string url;
    if (urlAt(v, i, url)) {
        out = {};
        out.url = url;
        return true;
    }
    if (v[i].type == TokenType::Function && lower(v[i].text).find("gradient") != std::string::npos) {
        size_t k = i;
        auto g = gradientAt(v, k, c, current);
        i = k;
        out = {};
        out.gradient = std::move(g);
        return true;   // one we can't read is none
    }
    return false;
}

bool repeatKeyword(const std::string& k, bool& x, bool& y, bool second, bool& two_value) {
    two_value = false;
    if (k == "repeat-x") { x = true; y = false; return true; }
    if (k == "repeat-y") { x = false; y = true; return true; }
    const bool repeat = k == "repeat" || k == "space" || k == "round";
    if (!repeat && k != "no-repeat") return false;
    if (second) y = repeat;
    else x = y = repeat;
    two_value = true;
    return true;
}

bool boxKeyword(const std::string& k, BackgroundBox& out) {
    if (k == "border-box" || k == "text") out = BackgroundBox::Border;
    else if (k == "padding-box") out = BackgroundBox::Padding;
    else if (k == "content-box") out = BackgroundBox::Content;
    else return false;
    return true;
}

bool sizeFrom(const Tokens& v, const LengthContext& c, BackgroundSize& out) {
    const std::vector<size_t> starts = valueStarts(v);
    if (starts.empty() || starts.size() > 2) return false;
    if (starts.size() == 1 && v[starts[0]].type == TokenType::Ident) {
        const std::string k = lower(v[starts[0]].text);
        if (k == "cover") { out = {BackgroundSize::Kind::Cover}; return true; }
        if (k == "contain") { out = {BackgroundSize::Kind::Contain}; return true; }
    }
    BackgroundSize s;
    Length got[2] = {Length::autoLength(), Length::autoLength()};
    for (size_t n = 0; n < starts.size(); ++n) {
        size_t k = starts[n];
        if (!lengthAt(v, k, c, got[n], true)) return false;
        if (!got[n].is_auto && (got[n].px < 0 || got[n].percent < 0)) return false;
    }
    s.width = got[0];
    s.height = got[1];
    out = s;
    return true;
}

// The background (or mask) shorthand. Its colour, background's only, goes in `color`.
bool layersFromShorthand(const Tokens& v, const LengthContext& c, const Color& current, bool mask, BackgroundLayers& out, Color* color) {
    out = BackgroundLayers{};
    out.sizes.clear();
    out.positions.clear();
    out.repeats.clear();
    out.origins.clear();
    out.clips.clear();
    std::vector<Tokens> parts = commaParts(v);
    if (parts.size() > kMaxBackgroundLayers) parts.resize(kMaxBackgroundLayers);
    for (size_t p = 0; p < parts.size(); ++p) {
        const Tokens& part = parts[p];
        BackgroundImage image;
        BackgroundSize size;
        Length px, py;
        bool rx = true, ry = true;
        BackgroundBox origin = mask ? BackgroundBox::Border : BackgroundBox::Padding, clip = BackgroundBox::Border;
        int boxes = 0, repeats = 0;
        Tokens position, size_tokens;
        bool in_size = false;
        const std::vector<size_t> starts = valueStarts(part);
        for (size_t n = 0; n < starts.size(); ++n) {
            size_t k = starts[n];
            const Token& t = part[k];
            if (t.type == TokenType::Delim && t.text == "/") {
                in_size = true;
                continue;
            }
            if (backgroundImageAt(part, k, c, current, image)) continue;
            if (t.type == TokenType::Ident) {
                const std::string kw = lower(t.text);
                bool two = false;
                if (repeatKeyword(kw, rx, ry, repeats == 1, two)) { ++repeats; continue; }
                BackgroundBox b;
                if (boxKeyword(kw, b)) {
                    if (boxes++ == 0) origin = clip = b;
                    else clip = b;
                    continue;
                }
                if (kw == "scroll" || kw == "fixed" || kw == "local") continue;
                if (mask && (kw == "alpha" || kw == "luminance" || kw == "match-source" || kw == "add" || kw == "subtract" ||
                             kw == "intersect" || kw == "exclude" || kw == "no-clip")) continue;
            }
            const bool positional = t.type == TokenType::Dimension || t.type == TokenType::Percentage || t.type == TokenType::Number ||
                                    (t.type == TokenType::Function && (t.text == "calc" || t.text == "min" || t.text == "max" || t.text == "clamp")) ||
                                    (t.type == TokenType::Ident && (lower(t.text) == "auto" || lower(t.text) == "cover" || lower(t.text) == "contain"));
            float probe_pct = 0;
            int probe_axis = 0;
            const bool position_kw = t.type == TokenType::Ident && positionKeyword(lower(t.text), probe_pct, probe_axis);
            if (positional || position_kw) {
                const size_t end = n + 1 < starts.size() ? starts[n + 1] : part.size();
                Tokens& into = in_size ? size_tokens : position;
                into.insert(into.end(), part.begin() + static_cast<std::ptrdiff_t>(k), part.begin() + static_cast<std::ptrdiff_t>(end));
                into.push_back(Token{TokenType::Whitespace});
                continue;
            }
            Color col;
            size_t q = k;
            if (color && p + 1 == parts.size() && colorFromTokens(part, q, current, col)) {
                *color = col;
                continue;
            }
            return false;
        }
        if (!position.empty() && !positionFrom(position, c, px, py)) return false;
        if (!size_tokens.empty() && !sizeFrom(size_tokens, c, size)) return false;
        out.images.push_back(image);
        out.sizes.push_back(size);
        out.positions.emplace_back(px, py);
        out.repeats.emplace_back(rx, ry);
        out.origins.push_back(origin);
        out.clips.push_back(clip);
    }
    return true;
}

// One background-* (or mask-*) longhand into `layers`.
bool setLayerLonghand(const std::string& prop, const Tokens& v, const LengthContext& c, const Color& current, BackgroundLayers& layers) {
    std::vector<Tokens> parts = commaParts(v);
    if (parts.size() > kMaxBackgroundLayers) parts.resize(kMaxBackgroundLayers);
    if (prop == "image") {
        std::vector<BackgroundImage> images;
        for (const Tokens& part : parts) {
            size_t k = 0;
            skipWs(part, k);
            BackgroundImage image;
            if (!backgroundImageAt(part, k, c, current, image)) return false;
            images.push_back(image);
        }
        layers.images = std::move(images);
        return true;
    }
    if (prop == "size") {
        std::vector<BackgroundSize> sizes;
        for (const Tokens& part : parts) {
            BackgroundSize s;
            if (!sizeFrom(part, c, s)) return false;
            sizes.push_back(s);
        }
        layers.sizes = std::move(sizes);
        return true;
    }
    if (prop == "position" || prop == "position-x" || prop == "position-y") {
        std::vector<std::pair<Length, Length>> positions;
        for (size_t n = 0; n < parts.size(); ++n) {
            Length x, y;
            if (prop == "position") {
                if (!positionFrom(parts[n], c, x, y)) return false;
            } else {
                Length a, b;
                if (!positionFrom(parts[n], c, a, b)) return false;
                const auto old = n < layers.positions.size() ? layers.positions[n] : layers.positions.empty() ? std::pair<Length, Length>{} : layers.positions[n % layers.positions.size()];
                // One value: positionFrom put a vertical keyword in b.
                const Length one = prop == "position-x" ? a : (b.percent == 50 && b.px == 0 && !(a.percent == 50 && a.px == 0) ? a : b);
                x = prop == "position-x" ? one : old.first;
                y = prop == "position-y" ? one : old.second;
            }
            positions.emplace_back(x, y);
        }
        layers.positions = std::move(positions);
        return true;
    }
    if (prop == "repeat") {
        std::vector<std::pair<bool, bool>> repeats;
        for (const Tokens& part : parts) {
            bool x = true, y = true;
            int n = 0;
            for (size_t start : valueStarts(part)) {
                bool two = false;
                if (part[start].type != TokenType::Ident || !repeatKeyword(lower(part[start].text), x, y, n == 1, two)) return false;
                ++n;
            }
            if (n == 0) return false;
            repeats.emplace_back(x, y);
        }
        layers.repeats = std::move(repeats);
        return true;
    }
    if (prop == "origin" || prop == "clip") {
        std::vector<BackgroundBox> boxes;
        for (const Tokens& part : parts) {
            const std::vector<size_t> starts = valueStarts(part);
            BackgroundBox b;
            if (starts.size() != 1 || part[starts[0]].type != TokenType::Ident) return false;
            const std::string k = lower(part[starts[0]].text);
            if (!boxKeyword(k, b)) {
                if (k == "no-clip") b = BackgroundBox::Border;
                else return false;
            }
            boxes.push_back(b);
        }
        (prop == "origin" ? layers.origins : layers.clips) = std::move(boxes);
        return true;
    }
    return prop == "attachment" || prop == "mode" || prop == "composite" || prop == "type" || prop == "blend-mode";
}

// ── SVG paint ─────────────────────────────────────────────────────────────────

bool svgPaintFrom(const Tokens& v, const Color& current, SvgPaint& out) {
    const std::vector<size_t> starts = valueStarts(v);
    if (starts.empty()) return false;
    size_t k = starts[0];
    SvgPaint p;
    std::string url;
    if (urlAt(v, k, url)) {
        p.kind = SvgPaint::Kind::Url;
        const size_t hash = url.find('#');
        p.url = hash == std::string::npos ? std::string{} : url.substr(hash);
        skipWs(v, k);
        if (k < v.size()) {
            if (v[k].type == TokenType::Ident && lower(v[k].text) == "none") p.fallback = SvgPaint::Kind::None;
            else if (v[k].type == TokenType::Ident && lower(v[k].text) == "currentcolor") p.fallback = SvgPaint::Kind::CurrentColor;
            else if (colorFromTokens(v, k, current, p.fallback_color)) p.fallback = SvgPaint::Kind::Color;
        }
        out = p;
        return true;
    }
    if (v[k].type == TokenType::Ident) {
        const std::string kw = lower(v[k].text);
        if (kw == "none") { out = {}; return true; }
        if (kw == "currentcolor") { out = {SvgPaint::Kind::CurrentColor}; return true; }
        if (kw == "context-fill" || kw == "context-stroke") { out = {}; return true; }
    }
    Color c;
    if (!parseColor(v, current, c)) return false;
    p.kind = SvgPaint::Kind::Color;
    p.color = c;
    out = p;
    return true;
}

// An SVG number or length: unitless numbers are pixels there.
bool svgLengthFrom(const Tokens& v, const LengthContext& c, float& out) {
    size_t k = 0;
    skipWs(v, k);
    if (k >= v.size()) return false;
    if (v[k].type == TokenType::Number) {
        out = static_cast<float>(v[k].number);
        return std::isfinite(out);
    }
    Length l;
    if (!lengthAt(v, k, c, l, false)) return false;
    out = l.px + l.percent * 0.01f * 100;   // a percentage of an unknown viewport: of a 100px one
    return std::isfinite(out);
}

bool svgNumberFrom(const Tokens& v, float& out) {
    size_t k = 0;
    skipWs(v, k);
    if (k >= v.size()) return false;
    if (v[k].type == TokenType::Number) out = static_cast<float>(v[k].number);
    else if (v[k].type == TokenType::Percentage) out = static_cast<float>(v[k].number) / 100.0f;
    else return false;
    return std::isfinite(out);
}

// url() tokens in a sheet's declarations, made absolute against where the sheet came from.
void absolutizeUrls(std::vector<Declaration>& decls, const std::string& base) {
    for (Declaration& d : decls) {
        Tokens& v = d.value;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i].type == TokenType::Url) {
                if (!v[i].text.starts_with('#') && !v[i].text.starts_with("data:")) v[i].text = resolveUrl(base, v[i].text);
            } else if (v[i].type == TokenType::Function && lower(v[i].text) == "url") {
                size_t k = i + 1;
                skipWs(v, k);
                if (k < v.size() && v[k].type == TokenType::String && !v[k].text.starts_with('#') && !v[k].text.starts_with("data:"))
                    v[k].text = resolveUrl(base, v[k].text);
            }
        }
    }
}

// ── @font-face ────────────────────────────────────────────────────────────────

constexpr size_t kMaxFontFaces = 512;

std::vector<std::pair<uint32_t, uint32_t>> parseUnicodeRange(std::string_view in) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    size_t i = 0;
    const auto hex = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : (ch | 0x20) >= 'a' && (ch | 0x20) <= 'f' ? (ch | 0x20) - 'a' + 10 : -1; };
    while (i < in.size() && out.size() < 4096) {
        while (i < in.size() && (in[i] == ' ' || in[i] == ',' || in[i] == '\n' || in[i] == '\t' || in[i] == '\r')) ++i;
        if (i + 1 >= in.size() || (in[i] | 0x20) != 'u' || in[i + 1] != '+') {
            // Not a range: on to the next comma.
            while (i < in.size() && in[i] != ',') ++i;
            continue;
        }
        i += 2;
        uint32_t lo = 0, hi = 0;
        int digits = 0, wild = 0;
        while (i < in.size() && digits + wild < 6) {
            if (in[i] == '?') {
                lo <<= 4;
                hi = (hi << 4) | 0xF;
                ++wild;
            } else if (hex(in[i]) >= 0 && wild == 0) {
                const auto d = static_cast<uint32_t>(hex(in[i]));
                lo = (lo << 4) | d;
                hi = (hi << 4) | d;
                ++digits;
            } else {
                break;
            }
            ++i;
        }
        if (digits + wild == 0) continue;
        if (wild == 0 && i < in.size() && in[i] == '-') {
            ++i;
            hi = 0;
            int n = 0;
            while (i < in.size() && hex(in[i]) >= 0 && n < 6) {
                hi = (hi << 4) | static_cast<uint32_t>(hex(in[i]));
                ++i;
                ++n;
            }
            if (n == 0) hi = lo;
        }
        if (lo <= hi && lo <= 0x10FFFF) out.emplace_back(lo, std::min(hi, 0x10FFFFu));
    }
    return out;
}

// The sources of a src: list that we could read.
std::vector<std::string> fontSources(const Tokens& v, const std::string& base) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < v.size() && out.size() < 16) {
        std::string url;
        bool usable = true, has_url = false;
        for (; i < v.size() && v[i].type != TokenType::Comma; ++i) {
            const Token& t = v[i];
            if (t.type == TokenType::Url) {
                url = t.text;
                has_url = true;
            } else if (t.type == TokenType::Function) {
                const std::string name = lower(t.text);
                size_t j = i + 1;
                skipWs(v, j);
                const bool has_arg = j < v.size() && (v[j].type == TokenType::String || v[j].type == TokenType::Ident);
                if (name == "url" && has_arg) {
                    url = v[j].text;
                    has_url = true;
                } else if (name == "format" && has_arg) {
                    const std::string f = lower(v[j].text);
                    usable = f == "woff" || f == "woff2" || f == "truetype" || f == "opentype" || f == "woff-variations" ||
                             f == "woff2-variations" || f == "truetype-variations" || f == "opentype-variations";
                }
                // Skip to the end of the function.
                int depth = 1;
                while (i + 1 < v.size() && depth > 0) {
                    ++i;
                    if (v[i].type == TokenType::Function || v[i].type == TokenType::LeftParen) ++depth;
                    else if (v[i].type == TokenType::RightParen) --depth;
                }
            }
        }
        if (i < v.size()) ++i;
        if (!has_url || !usable) continue;
        const std::string file = lower(url.substr(0, url.find_first_of("?#")));
        if (file.ends_with(".eot") || file.ends_with(".svg")) continue;
        out.push_back(resolveUrl(base, url));
    }
    return out;
}

// Only a rule that can really be used comes back.
std::optional<WebFontFace> webFontFrom(const css::FontFaceRule& rule, const std::string& base) {
    WebFontFace face;
    for (const css::Declaration& d : rule.declarations) {
        const Tokens& v = d.value;
        if (d.name == "font-family") {
            std::string name;
            for (const Token& t : v) {
                if (t.type == TokenType::String) {
                    name = t.text;
                } else if (t.type == TokenType::Ident) {
                    if (!name.empty()) name += ' ';
                    name += t.text;
                }
            }
            face.family = lower(name);
        } else if (d.name == "src") {
            face.sources = fontSources(v, base);
        } else if (d.name == "font-weight") {
            std::vector<int> weights;
            for (const Token& t : v) {
                if (t.type == TokenType::Number) weights.push_back(std::clamp(static_cast<int>(t.number), 1, 1000));
                else if (t.type == TokenType::Ident && lower(t.text) == "bold") weights.push_back(700);
                else if (t.type == TokenType::Ident && lower(t.text) == "normal") weights.push_back(400);
            }
            if (!weights.empty()) {
                face.weight_min = std::min(weights.front(), weights.back());
                face.weight_max = std::max(weights.front(), weights.back());
            }
        } else if (d.name == "font-style") {
            const std::string k = keyword(v);
            const bool oblique = !v.empty() && std::any_of(v.begin(), v.end(), [](const Token& t) { return t.type == TokenType::Ident && lower(t.text) == "oblique"; });
            face.italic = k == "italic" || oblique;
        }
    }
    if (!rule.unicode_range.empty()) face.ranges = parseUnicodeRange(rule.unicode_range);
    if (face.family.empty() || face.sources.empty()) return std::nullopt;
    return face;
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

BorderStyle borderStyleOf(std::string_view s) {
    if (s == "hidden") return BorderStyle::Hidden;
    if (s == "solid") return BorderStyle::Solid;
    if (s == "dashed") return BorderStyle::Dashed;
    if (s == "dotted") return BorderStyle::Dotted;
    if (s == "double") return BorderStyle::Double;
    if (s == "groove") return BorderStyle::Groove;
    if (s == "ridge") return BorderStyle::Ridge;
    if (s == "inset") return BorderStyle::Inset;
    if (s == "outset") return BorderStyle::Outset;
    return BorderStyle::None;
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

struct Buckets {
    std::unordered_map<std::string, std::vector<IndexedSelector>> by_id, by_class, by_type;
    std::vector<IndexedSelector> universal;
    [[nodiscard]] bool empty() const { return by_id.empty() && by_class.empty() && by_type.empty() && universal.empty(); }
};

// ::slotted(sel) rule: what the slot must match (the selector without ::slotted), and
// what the slotted element must (its argument).
struct SlottedRule {
    ComplexSelector slot_side;
    const SimpleSelector* slotted;
    const css::Rule* rule;
    Origin origin;
    size_t order;
};

// The rules of one tree: the user agent's, the document's, or a shadow root's. A shadow
// tree's rules only reach into it, and to its host through :host.
struct Scope {
    Buckets normal, before, after;
    std::vector<IndexedSelector> host;      // rules whose subject is :host(...)
    std::deque<SlottedRule> slotted;
    const Node* host_el = nullptr;          // a shadow scope's host
};

class StyleResolver {
public:
    StyleResolver(const Document& doc, StyledDocument& out, const StyledDocument::SheetLoader& loader)
        : doc_(doc), out_(out), loader_(loader) {
        lengths_.viewport_w = out.media_.width;
        lengths_.viewport_h = out.media_.height;
        target_ = &ua_;
        addSheet(css::parseStylesheet(detail::kUserAgentStyle), Origin::UserAgent, 0);
        if (doc.quirks) addSheet(css::parseStylesheet(detail::kQuirksStyle), Origin::UserAgent, 0);
        target_ = &doc_scope_;
        collectSheets(doc_.root());
        for (const std::string& css : doc_.root()->adopted_css) addSheet(css::parseStylesheet(css), Origin::Author, 0);   // document.adoptedStyleSheets
        collectShadowScopes();
        indexSiblings();
    }

    void run() {
        const Node* root = doc_.documentElement();
        if (!root) return;
        ComputedStyle initial;
        std::vector<const Node*> stack;
        walk(root, initial, 0, &doc_scope_, nullptr);
    }

private:
    // `url` is where the sheet came from: its @imports are relative to that.
    void addSheet(css::Stylesheet sheet, Origin origin, int depth, const std::string& url = {}) {
        if (loader_ && depth < kMaxImportDepth) {
            for (const std::string& ref : sheet.imports) {
                if (++import_count_ > kMaxImports) break;
                const std::string target = resolveUrl(url, ref);
                addSheet(css::parseStylesheet(loader_(target)), origin, depth + 1, target);
            }
        }
        for (const css::FontFaceRule& rule : sheet.font_faces) {
            if (out_.font_faces_.size() >= kMaxFontFaces) break;
            if (!std::all_of(rule.media.begin(), rule.media.end(), [&](const Tokens& q) { return css::mediaMatches(q, out_.media_); })) continue;
            if (auto face = webFontFrom(rule, url)) out_.font_faces_.push_back(std::move(*face));
        }
        if (!url.empty())
            for (css::Rule& rule : sheet.rules) absolutizeUrls(rule.declarations, url);
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
        Buckets* buckets = &target_->normal;
        for (const SimpleSelector& s : last.simple) {
            if (s.kind != SimpleKind::PseudoElement) continue;
            if (s.name == "before") buckets = &target_->before;
            else if (s.name == "after") buckets = &target_->after;
            else if (s.name == "slotted" && target_->host_el) {
                SlottedRule r{sel, &s, &rule, origin, ++order_};
                auto& simple = r.slot_side.parts.back().simple;
                simple.erase(std::remove_if(simple.begin(), simple.end(), [](const SimpleSelector& x) {
                    return x.kind == SimpleKind::PseudoElement && x.name == "slotted";
                }), simple.end());
                target_->slotted.push_back(std::move(r));
                target_->slotted.back().slotted = nullptr;
                for (const SimpleSelector& x : last.simple)
                    if (x.kind == SimpleKind::PseudoElement && x.name == "slotted") target_->slotted.back().slotted = &x;
                return;
            } else {
                return;   // ::marker, ::placeholder and so on: nothing to style yet
            }
        }
        const IndexedSelector entry{&sel, &rule, origin, ++order_};
        if (target_->host_el && buckets == &target_->normal)
            for (const SimpleSelector& s : last.simple)
                if (s.kind == SimpleKind::PseudoClass && (s.name == "host" || s.name == "host-context")) {
                    target_->host.push_back(entry);
                    return;
                }
        for (const SimpleSelector& s : last.simple) {
            if (s.kind == SimpleKind::Id) { buckets->by_id[s.name].push_back(entry); return; }
        }
        for (const SimpleSelector& s : last.simple) {
            if (s.kind == SimpleKind::Class) { buckets->by_class[s.name].push_back(entry); return; }
        }
        for (const SimpleSelector& s : last.simple) {
            if (s.kind == SimpleKind::Type) { buckets->by_type[s.name].push_back(entry); return; }
        }
        buckets->universal.push_back(entry);
    }

    // A tree's <style> and <link rel=stylesheet>, into target_ (not into shadow trees:
    // they aren't children).
    void collectSheets(const Node* root) {
        std::vector<const Node*> todo{root};
        while (!todo.empty()) {
            const Node* n = todo.back();
            todo.pop_back();
            if (n->isHtml("style") || (n->ns == Namespace::Svg && n->name == "style")) {
                const std::string* media = n->attr("media");
                if (!media || css::mediaMatches(css::tokenize(*media), out_.media_))
                    addSheet(css::parseStylesheet(n->textContent()), Origin::Author, 0);
            } else if (n->isHtml("link") && loader_) {
                const std::string* rel = n->attr("rel");
                const std::string* href = n->attr("href");
                if (rel && href && hasWord(lower(*rel), "stylesheet") && !hasWord(lower(*rel), "alternate")) {
                    const std::string* media = n->attr("media");
                    if (!media || css::mediaMatches(css::tokenize(*media), out_.media_))
                        addSheet(css::parseStylesheet(loader_(*href)), Origin::Author, 0, *href);
                }
            }
            todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        }
    }

    // Every shadow root's scope, its sheets and its adopted ones.
    void collectShadowScopes() {
        std::vector<const Node*> todo{doc_.root()};
        size_t count = 0;
        while (!todo.empty() && count < 10'000) {
            const Node* n = todo.back();
            todo.pop_back();
            if (n->shadow_root) {
                ++count;
                auto scope = std::make_unique<Scope>();
                scope->host_el = n;
                target_ = scope.get();
                collectSheets(n->shadow_root);
                for (const std::string& css : n->shadow_root->adopted_css) addSheet(css::parseStylesheet(css), Origin::Author, 0);
                shadow_scopes_[n->shadow_root] = std::move(scope);
                todo.push_back(n->shadow_root);
            }
            todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        }
        target_ = &doc_scope_;
    }

    const Scope* scopeFor(const Node* el) const {
        if (const Node* root = el->containingShadowRoot()) {
            const auto it = shadow_scopes_.find(root);
            if (it != shadow_scopes_.end()) return it->second.get();
        }
        return &doc_scope_;
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
            if (n->shadow_root) todo.push_back(n->shadow_root);
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
            case SimpleKind::PseudoElement: return true;   // the bucket it's in decides
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
        if (n == "host" || n == "host-context") {
            if (el != host_context_) return false;
            if (s.args.empty()) return true;
            // The argument is matched against the host in its own tree.
            const Node* saved = host_context_;
            host_context_ = nullptr;
            bool hit = false;
            for (const Node* p = el; p && !hit; p = n == "host" ? nullptr : p->parent)
                for (const ComplexSelector& a : s.args)
                    if (p->isElement() && matchComplex(p, a)) hit = true;
            host_context_ = saved;
            return hit;
        }
        if (n == "defined") {
            // Custom element names aren't defined until upgraded.
            return !(el->ns == Namespace::Html && el->name.find('-') != std::string::npos && el->custom_state != 2);
        }
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
        // Seen from inside its shadow tree, the host is featureless: only :host matches it.
        if (el == host_context_)
            for (const SimpleSelector& s : c.simple)
                if (!(s.kind == SimpleKind::PseudoClass && (s.name == "host" || s.name == "host-context"))) return false;
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
                for (const Node* p = matchParent(el); p; p = matchParent(p))
                    if (matchFrom(p, sel, before)) return true;
                return false;
            case Combinator::Child: {
                const Node* p = matchParent(el);
                return p && matchFrom(p, sel, before);
            }
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

    // An element's parent for selector matching: within its tree, and from a shadow
    // tree's top to its host (which only :host can match).
    const Node* matchParent(const Node* el) const {
        if (el == host_context_) return nullptr;
        const Node* p = el->parent;
        if (p && p->isElement()) return p;
        if (p && p->type == NodeType::Fragment && p->shadow_host && p->shadow_host == host_context_) return p->shadow_host;
        return nullptr;
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
        if (el->ns == Namespace::Svg) {
            // Presentation attributes: author rules of no specificity.
            static constexpr const char* kPresentation[] = {
                "fill", "stroke", "stroke-width", "fill-opacity", "stroke-opacity", "fill-rule", "stroke-linecap",
                "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset", "stop-color",
                "stop-opacity", "opacity", "color", "display", "visibility", "clip-path"};
            for (const char* name : kPresentation) {
                const std::string* v = el->attr(name);
                if (!v || v->find_first_of(";{}") != std::string::npos) continue;
                css_text += std::string(name) + ":" + *v + ";";
            }
            // An <svg>'s width and height are presentation attributes too.
            if (el->name == "svg") {
                for (const char* name : {"width", "height"}) {
                    const std::string* v = el->attr(name);
                    if (!v || v->empty() || v->size() > 32) continue;
                    char* end = nullptr;
                    const double n = std::strtod(v->c_str(), &end);
                    if (end == v->c_str() || n < 0) continue;
                    const std::string unit(end);
                    if (unit.empty()) css_text += std::string(name) + ":" + *v + "px;";
                    else if (unit == "px" || unit == "%" || unit == "em" || unit == "rem" || unit == "pt") css_text += std::string(name) + ":" + *v + ";";
                }
            }
            return;
        }
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
        if (n == "img" || n == "object" || n == "embed" || n == "iframe") {
            if (const std::string* v = el->attr("align")) {
                const std::string a = lower(*v);
                if (a == "left" || a == "right") css_text += "float:" + a + ";";
                else if (a == "middle" || a == "absmiddle" || a == "center") css_text += "vertical-align:middle;";
                else if (a == "top" || a == "texttop") css_text += "vertical-align:top;";
            }
            const auto spacing = [&](const char* attr_name, const char* a, const char* b) {
                if (const std::string* v = el->attr(attr_name))
                    css_text += std::string(a) + ":" + *v + "px;" + b + ":" + *v + "px;";
            };
            spacing("hspace", "margin-left", "margin-right");
            spacing("vspace", "margin-top", "margin-bottom");
        }
        if (n == "table") {
            if (const std::string* v = el->attr("cellspacing")) css_text += "border-spacing:" + *v + "px;";
            if (const std::string* v = el->attr("align")) {
                const std::string a = lower(*v);
                if (a == "center") css_text += "margin-left:auto;margin-right:auto;";
                else if (a == "left" || a == "right") css_text += "float:" + a + ";";
            }
        }
        if (n == "td" || n == "th" || n == "tr" || n == "tbody" || n == "thead" || n == "tfoot") {
            if (const std::string* v = el->attr("valign")) {
                const std::string a = lower(*v);
                if (a == "top" || a == "middle" || a == "bottom" || a == "baseline") css_text += "vertical-align:" + a + ";";
            }
            if (n != "tr" && el->attr("nowrap")) css_text += "white-space:nowrap;";
        }
        if (n == "td" || n == "th") {
            // Set on the table, meant for its cells.
            const Node* table = el->parent;
            while (table && !table->isHtml("table")) table = table->parent;
            if (table) {
                if (const std::string* v = table->attr("cellpadding")) css_text += "padding:" + *v + "px;";
                if (const std::string* v = table->attr("border"); v && *v != "0") css_text += "border:1px inset gray;";
            }
        }
        if (n == "hr") {
            if (const std::string* v = el->attr("size")) css_text += "height:" + *v + "px;";
            if (el->attr("noshade")) css_text += "border-style:solid;";
        }
        if (n == "body") {
            if (const std::string* v = el->attr("leftmargin")) css_text += "margin-left:" + *v + "px;";
            if (const std::string* v = el->attr("topmargin")) css_text += "margin-top:" + *v + "px;";
            if (const std::string* v = el->attr("marginwidth")) css_text += "margin-left:" + *v + "px;margin-right:" + *v + "px;";
            if (const std::string* v = el->attr("marginheight")) css_text += "margin-top:" + *v + "px;margin-bottom:" + *v + "px;";
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
        c.line_height = s.usedLineHeight();
        if (out_.measure_ch_) c.ch = out_.measure_ch_(s);
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
        BorderStyle style = BorderStyle::None;
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
                style = borderStyleOf(n);
                continue;
            }
            size_t i = start;
            Color parsed;
            if (colorFromTokens(v, i, s.color, parsed)) color = parsed;
        }
        for (int side : sides) {
            s.border_width[side] = width;
            border_none_[side] = none;
            s.border_style[side] = style;
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

    // The whole list: which of them is installed is for whoever measures text to say.
    static void setFontFamily(ComputedStyle& s, const Tokens& v, size_t i) {
        std::string list, name;
        const auto finish = [&] {
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (name.empty()) return;
            if (!list.empty()) list += ',';
            list += name;
            name.clear();
        };
        for (; i < v.size(); ++i) {
            const Token& t = v[i];
            if (t.type == TokenType::Comma) finish();
            else if (t.type == TokenType::Whitespace) {
                if (!name.empty()) name += ' ';
            } else if (t.type == TokenType::Ident || t.type == TokenType::String) {
                name += t.text;
            }
        }
        finish();
        if (!list.empty()) s.font_family = list;
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
        static const std::map<std::string, Display> kDisplays = {
            {"none", Display::None}, {"inline", Display::Inline}, {"contents", Display::Contents}, {"ruby", Display::Inline},
            {"ruby-text", Display::Inline}, {"inline-block", Display::InlineBlock}, {"inline-grid", Display::InlineGrid},
            {"inline-table", Display::InlineTable}, {"inline-flex", Display::InlineFlex}, {"list-item", Display::ListItem},
            {"block", Display::Block}, {"grid", Display::Grid}, {"flow", Display::Block}, {"flow-root", Display::FlowRoot},
            {"table", Display::Table}, {"table-caption", Display::TableCaption}, {"table-row-group", Display::TableRowGroup},
            {"table-header-group", Display::TableRowGroup}, {"table-footer-group", Display::TableRowGroup},
            {"table-row", Display::TableRow}, {"table-cell", Display::TableCell}, {"table-column", Display::TableColumn},
            {"table-column-group", Display::TableColumn}, {"flex", Display::Flex},
        };
        const auto it = kDisplays.find(k);
        if (it != kDisplays.end()) s.display = it->second;
    }

    static FlexAlign flexAlign(const std::string& k, FlexAlign keep) {
        if (k == "flex-start" || k == "start" || k == "self-start" || k == "left" || k == "normal") return k == "normal" ? keep : FlexAlign::Start;
        if (k == "flex-end" || k == "end" || k == "self-end" || k == "right") return FlexAlign::End;
        if (k == "center") return FlexAlign::Center;
        if (k == "stretch") return FlexAlign::Stretch;
        if (k == "baseline") return FlexAlign::Baseline;
        if (k == "space-between") return FlexAlign::SpaceBetween;
        if (k == "space-around") return FlexAlign::SpaceAround;
        if (k == "space-evenly") return FlexAlign::SpaceEvenly;
        if (k == "auto") return FlexAlign::Auto;
        return keep;
    }

    // The keyword at each side of a slash or space, for place-*.
    static std::pair<std::string, std::string> twoKeywords(const Tokens& v) {
        std::vector<std::string> words;
        for (const Token& t : v)
            if (t.type == TokenType::Ident) words.push_back(lower(t.text));
        if (words.empty()) return {};
        return {words.front(), words.back()};
    }

    static void clearGridTemplate(ComputedStyle& s) {
        s.grid_rows.reset();
        s.grid_columns.reset();
        s.grid_areas.reset();
    }

    static std::shared_ptr<const GridTrackList> trackList(const Tokens& v, size_t& i, const LengthContext& c, bool& ok) {
        auto list = std::make_shared<GridTrackList>();
        ok = trackListAt(v, i, c, *list);
        return list;
    }

    // grid-template: rows / columns, or the form with strings of area names in it.
    static bool setGridTemplate(ComputedStyle& s, const Tokens& v, const LengthContext& c) {
        size_t i = 0;
        const bool has_strings = std::any_of(v.begin(), v.end(), [](const Token& t) { return t.type == TokenType::String; });
        if (!has_strings) {
            bool ok = false;
            auto rows = trackList(v, i, c, ok);
            if (!ok) return false;
            skipWs(v, i);
            std::shared_ptr<const GridTrackList> columns;
            if (i < v.size() && isSlash(v[i])) {
                ++i;
                columns = trackList(v, i, c, ok);
                if (!ok) return false;
            }
            clearGridTemplate(s);
            s.grid_rows = std::move(rows);
            s.grid_columns = std::move(columns);
            return true;
        }
        auto areas = std::make_shared<GridAreas>();
        std::map<std::string, int> cells;
        auto rows = std::make_shared<GridTrackList>();
        rows->names.assign(1, {});
        while (true) {
            skipWs(v, i);
            if (i >= v.size() || isSlash(v[i])) break;
            if (lineNamesAt(v, i, rows->names.back())) continue;
            if (v[i].type != TokenType::String || !addAreaRow(v[i].text, *areas, cells)) return false;
            ++i;
            skipWs(v, i);
            GridTrack track;
            if (i < v.size() && v[i].type != TokenType::String && v[i].type != TokenType::LeftBracket && !isSlash(v[i])) {
                if (!gridTrackAt(v, i, c, track)) return false;
            }
            rows->tracks.push_back(std::move(track));
            rows->names.emplace_back();
        }
        if (!areasAreRectangles(*areas, cells)) return false;
        std::shared_ptr<const GridTrackList> columns;
        if (i < v.size() && isSlash(v[i])) {
            ++i;
            bool ok = false;
            columns = trackList(v, i, c, ok);
            if (!ok) return false;
        }
        clearGridTemplate(s);
        s.grid_areas = std::move(areas);
        s.grid_rows = std::move(rows);
        s.grid_columns = std::move(columns);
        return true;
    }

    static bool autoTracksAt(const Tokens& v, size_t& i, const LengthContext& c, std::shared_ptr<const std::vector<GridTrack>>& into) {
        auto tracks = std::make_shared<std::vector<GridTrack>>();
        while (true) {
            skipWs(v, i);
            if (i >= v.size() || isSlash(v[i])) break;
            GridTrack t;
            if (!gridTrackAt(v, i, c, t)) return false;
            tracks->push_back(std::move(t));
            if (tracks->size() > kMaxGridListTracks) return false;
        }
        if (!tracks->empty()) into = std::move(tracks);
        return true;
    }

    // [ dense ] after auto-flow, in the grid shorthand or on its own.
    static bool setAutoFlow(ComputedStyle& s, const Tokens& v, size_t& i, bool column) {
        s.grid_flow_column = column;
        s.grid_dense = false;
        skipWs(v, i);
        if (i < v.size() && v[i].type == TokenType::Ident && lower(v[i].text) == "dense") {
            s.grid_dense = true;
            ++i;
        }
        return true;
    }

    // Grid's properties, and the alignment ones only it has. False if it isn't one.
    static bool applyGrid(ComputedStyle& s, const std::string& p, const Tokens& v, const std::string& kw, const LengthContext& c) {
        size_t i = 0;
        bool ok = false;
        if (p == "grid-template-columns" || p == "grid-template-rows") {
            auto& into = p == "grid-template-rows" ? s.grid_rows : s.grid_columns;
            if (kw == "none" || kw == "subgrid" || kw == "masonry") {
                into.reset();
                return true;
            }
            auto list = trackList(v, i, c, ok);
            if (ok) into = std::move(list);
            return true;
        }
        if (p == "grid-template-areas") {
            if (kw == "none") {
                s.grid_areas.reset();
                return true;
            }
            auto areas = std::make_shared<GridAreas>();
            std::map<std::string, int> cells;
            for (const Token& t : v) {
                if (t.type == TokenType::Whitespace) continue;
                if (t.type != TokenType::String || !addAreaRow(t.text, *areas, cells)) return true;
            }
            if (areas->rows > 0 && areasAreRectangles(*areas, cells)) s.grid_areas = std::move(areas);
            return true;
        }
        if (p == "grid-template") {
            if (kw == "none") clearGridTemplate(s);
            else (void)setGridTemplate(s, v, c);
            return true;
        }
        if (p == "grid") {
            // Everything it doesn't mention goes back to its initial value.
            const auto reset = [&s] {
                clearGridTemplate(s);
                s.grid_auto_rows.reset();
                s.grid_auto_columns.reset();
                s.grid_flow_column = false;
                s.grid_dense = false;
            };
            if (kw == "none") {
                reset();
                return true;
            }
            const auto auto_flow = std::find_if(v.begin(), v.end(), [](const Token& t) { return t.type == TokenType::Ident && lower(t.text) == "auto-flow"; });
            if (auto_flow == v.end()) {
                ComputedStyle trial = s;
                if (setGridTemplate(trial, v, c)) {
                    reset();
                    s.grid_rows = trial.grid_rows;
                    s.grid_columns = trial.grid_columns;
                    s.grid_areas = trial.grid_areas;
                }
                return true;
            }
            ComputedStyle trial = s;
            clearGridTemplate(trial);
            trial.grid_auto_rows.reset();
            trial.grid_auto_columns.reset();
            const auto slash = std::find_if(v.begin(), v.end(), isSlash);
            if (auto_flow < slash) {
                // auto-flow [dense] [auto rows] / columns
                i = static_cast<size_t>(auto_flow - v.begin()) + 1;
                (void)setAutoFlow(trial, v, i, false);
                if (!autoTracksAt(v, i, c, trial.grid_auto_rows)) return true;
                if (i < v.size() && isSlash(v[i])) {
                    ++i;
                    trial.grid_columns = trackList(v, i, c, ok);
                    if (!ok) return true;
                }
            } else {
                // rows / auto-flow [dense] [auto columns]
                trial.grid_rows = trackList(v, i, c, ok);
                if (!ok) return true;
                i = static_cast<size_t>(auto_flow - v.begin()) + 1;
                (void)setAutoFlow(trial, v, i, true);
                if (!autoTracksAt(v, i, c, trial.grid_auto_columns)) return true;
            }
            s = std::move(trial);
            return true;
        }
        if (p == "grid-auto-columns" || p == "grid-auto-rows") {
            std::shared_ptr<const std::vector<GridTrack>> tracks;
            if (autoTracksAt(v, i, c, tracks)) (p == "grid-auto-rows" ? s.grid_auto_rows : s.grid_auto_columns) = std::move(tracks);
            return true;
        }
        if (p == "grid-auto-flow") {
            bool column = false, dense = false;
            for (const Token& t : v) {
                if (t.type != TokenType::Ident) continue;
                const std::string k = lower(t.text);
                column = column || k == "column";
                dense = dense || k == "dense";
            }
            s.grid_flow_column = column;
            s.grid_dense = dense;
            return true;
        }
        if (p == "grid-row-start" || p == "grid-row-end" || p == "grid-column-start" || p == "grid-column-end") {
            GridLine line;
            if (gridLineAt(v, i, line)) {
                (p == "grid-row-start" ? s.grid_row_start : p == "grid-row-end" ? s.grid_row_end : p == "grid-column-start" ? s.grid_column_start : s.grid_column_end) = line;
            }
            return true;
        }
        if (p == "grid-row" || p == "grid-column") {
            GridLine start, end;
            if (!gridLineAt(v, i, start)) return true;
            if (i < v.size() && isSlash(v[i])) {
                ++i;
                if (!gridLineAt(v, i, end)) return true;
            } else if (!start.name.empty() && start.span == 0 && start.line == 0) {
                end = start;   // a lone name is both ends
            }
            (p == "grid-row" ? s.grid_row_start : s.grid_column_start) = start;
            (p == "grid-row" ? s.grid_row_end : s.grid_column_end) = end;
            return true;
        }
        if (p == "grid-area") {
            GridLine parts[4];
            int n = 0;
            while (n < 4) {
                if (!gridLineAt(v, i, parts[n])) return true;
                ++n;
                if (i >= v.size()) break;
                ++i;   // the slash
            }
            const auto is_name = [](const GridLine& l) { return !l.name.empty() && l.span == 0 && l.line == 0; };
            if (n < 2 && is_name(parts[0])) parts[1] = parts[0];
            if (n < 3 && is_name(parts[0])) parts[2] = parts[0];
            if (n < 4 && is_name(parts[1])) parts[3] = parts[1];
            s.grid_row_start = parts[0];
            s.grid_column_start = parts[1];
            s.grid_row_end = parts[2];
            s.grid_column_end = parts[3];
            return true;
        }
        if (p == "justify-content") { s.justify_content = kw == "normal" ? FlexAlign::Auto : flexAlign(kw, s.justify_content); return true; }
        if (p == "justify-items") { s.justify_items = kw == "normal" || kw == "legacy" ? FlexAlign::Auto : flexAlign(kw, s.justify_items); return true; }
        if (p == "justify-self") { s.justify_self = kw == "normal" ? FlexAlign::Auto : flexAlign(kw, s.justify_self); return true; }
        if (p == "place-items") {
            const auto [a, b] = twoKeywords(v);
            s.align_items = a == "normal" ? FlexAlign::Stretch : flexAlign(a, s.align_items);
            s.justify_items = b == "normal" ? FlexAlign::Auto : flexAlign(b, s.justify_items);
            return true;
        }
        if (p == "place-self") {
            const auto [a, b] = twoKeywords(v);
            s.align_self = a == "normal" ? FlexAlign::Auto : flexAlign(a, s.align_self);
            s.justify_self = b == "normal" ? FlexAlign::Auto : flexAlign(b, s.justify_self);
            return true;
        }
        if (p == "place-content") {
            const auto [a, b] = twoKeywords(v);
            s.align_content = a == "normal" ? FlexAlign::Stretch : flexAlign(a, s.align_content);
            s.justify_content = b == "normal" ? FlexAlign::Auto : flexAlign(b, s.justify_content);
            return true;
        }
        return false;
    }

    void setFlexShorthand(ComputedStyle& s, const Tokens& v, const LengthContext& c) {
        const std::string kw = keyword(v);
        if (kw == "none") {
            s.flex_grow = 0;
            s.flex_shrink = 0;
            s.flex_basis = Length::autoLength();
            return;
        }
        if (kw == "auto") {
            s.flex_grow = 1;
            s.flex_shrink = 1;
            s.flex_basis = Length::autoLength();
            return;
        }
        float grow = -1, shrink = -1;
        Length basis = Length::pixels(0);
        for (size_t start : valueStarts(v)) {
            const Token& t = v[start];
            if (t.type == TokenType::Number && t.number != 0 && grow >= 0 && shrink >= 0) continue;
            if (t.type == TokenType::Number && (grow < 0 || shrink < 0)) {
                (grow < 0 ? grow : shrink) = std::max(0.0f, static_cast<float>(t.number));
                continue;
            }
            size_t k = start;
            Length l;
            if (lengthAt(v, k, c, l, true)) basis = l;
            else if (t.type == TokenType::Ident && lower(t.text) == "content") basis = Length::autoLength();
        }
        s.flex_grow = std::max(grow, 0.0f);
        s.flex_shrink = shrink < 0 ? 1 : shrink;
        s.flex_basis = basis;
    }

    void setContent(ComputedStyle& s, const Tokens& v) {
        const std::string kw = keyword(v);
        s.content.clear();
        s.has_content = kw != "none" && kw != "normal" && !v.empty();
        if (!s.has_content) return;
        for (size_t i = 0; i < v.size(); ++i) {
            const Token& t = v[i];
            // content: "\200b" / " (external)": what follows the slash is alternative
            // text for assistive technology, not shown.
            if (t.type == TokenType::Delim && t.text == "/") break;
            if (t.type == TokenType::String) {
                s.content += t.text;
            } else if (t.type == TokenType::Ident) {
                const std::string k = lower(t.text);
                if (k == "open-quote") s.content += "\xE2\x80\x9C";
                else if (k == "close-quote") s.content += "\xE2\x80\x9D";
            } else if (t.type == TokenType::Function) {
                if (t.text == "attr" && i + 1 < v.size() && current_element_) {
                    if (const std::string* value = current_element_->attr(lower(v[i + 1].text))) s.content += *value;
                }
                while (i < v.size() && v[i].type != TokenType::RightParen) ++i;
            }
        }
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
        else if (p == "text-overflow") to.text_overflow_ellipsis = from.text_overflow_ellipsis;
        else if (p == "list-style-type" || p == "list-style") to.list_style = from.list_style;
        else if (p == "text-indent") to.text_indent = from.text_indent;
        else if (p == "visibility") to.hidden = from.hidden;
        else if (p == "border-spacing") to.border_spacing = from.border_spacing;
        else if (p == "border-collapse") to.border_collapse = from.border_collapse;
        else if (p == "fill") to.fill = from.fill;
        else if (p == "stroke") to.stroke = from.stroke;
        else if (p == "stroke-width") to.stroke_width = from.stroke_width;
        else if (p == "stroke-miterlimit") to.stroke_miterlimit = from.stroke_miterlimit;
        else if (p == "stroke-dashoffset") to.stroke_dashoffset = from.stroke_dashoffset;
        else if (p == "stroke-dasharray") to.stroke_dasharray = from.stroke_dasharray;
        else if (p == "fill-opacity") to.fill_opacity = from.fill_opacity;
        else if (p == "stroke-opacity") to.stroke_opacity = from.stroke_opacity;
        else if (p == "fill-rule") to.fill_evenodd = from.fill_evenodd;
        else if (p == "stroke-linecap") to.stroke_linecap = from.stroke_linecap;
        else if (p == "stroke-linejoin") to.stroke_linejoin = from.stroke_linejoin;
        else return false;
        return true;
    }

    static void copyAny(ComputedStyle& to, const ComputedStyle& from, const std::string& p) {
        if (p == "grid-template-columns") { to.grid_columns = from.grid_columns; return; }
        if (p == "grid-template-rows") { to.grid_rows = from.grid_rows; return; }
        if (p == "grid-template-areas") { to.grid_areas = from.grid_areas; return; }
        if (p == "grid-auto-columns") { to.grid_auto_columns = from.grid_auto_columns; return; }
        if (p == "grid-auto-rows") { to.grid_auto_rows = from.grid_auto_rows; return; }
        if (p == "grid-auto-flow") { to.grid_flow_column = from.grid_flow_column; to.grid_dense = from.grid_dense; return; }
        if (p == "grid-row-start" || p == "grid-row") to.grid_row_start = from.grid_row_start;
        if (p == "grid-row-end" || p == "grid-row") to.grid_row_end = from.grid_row_end;
        if (p == "grid-column-start" || p == "grid-column") to.grid_column_start = from.grid_column_start;
        if (p == "grid-column-end" || p == "grid-column") to.grid_column_end = from.grid_column_end;
        if (p.starts_with("grid-row") || p.starts_with("grid-column")) return;
        if (p == "justify-items") { to.justify_items = from.justify_items; return; }
        if (p == "justify-self") { to.justify_self = from.justify_self; return; }
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
        else if (p == "vertical-align") to.vertical_align = from.vertical_align;
        else if (p == "float") to.float_side = from.float_side;
        else if (p == "clear") to.clear = from.clear;
        else if (p == "position") to.position = from.position;
        else if (p == "width") { to.width = from.width; to.width_keyword = from.width_keyword; }
        else if (p == "height") to.height = from.height;
        else if (p == "min-width") to.min_width = from.min_width;
        else if (p == "min-height") to.min_height = from.min_height;
        else if (p == "max-width") to.max_width = from.max_width;
        else if (p == "max-height") to.max_height = from.max_height;
        else if (p == "background-color") to.background = from.background;
        else if (p == "background") { to.background = from.background; to.background_layers = from.background_layers; }
        else if (p.starts_with("background-")) to.background_layers = from.background_layers;
        else if (p == "mask" || p == "-webkit-mask" || p.starts_with("mask-") || p.starts_with("-webkit-mask-")) { to.mask_layers = from.mask_layers; to.masked = from.masked; }
        else if (p == "stop-color") to.stop_color = from.stop_color;
        else if (p == "stop-opacity") to.stop_opacity = from.stop_opacity;
        else if (p == "clip-path") to.clip_path = from.clip_path;
        else if (p == "opacity") to.opacity = from.opacity;
        else if (p == "box-sizing") to.border_box = from.border_box;
        else if (p == "box-shadow") to.box_shadow = from.box_shadow;
        else if (p == "z-index") to.z_index = from.z_index;
        else if (p.ends_with("radius")) {
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 2; ++j) to.radius[i][j] = from.radius[i][j];
        }
        else if (p.starts_with("border")) {
            std::copy(std::begin(from.border_width), std::end(from.border_width), std::begin(to.border_width));
            std::copy(std::begin(from.border_style), std::end(from.border_style), std::begin(to.border_style));
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
        if (p.starts_with("border") && !p.ends_with("radius") && p != "border-spacing" && p != "border-collapse" &&
            (kw == "inherit" || kw == "initial" || kw == "unset" || kw == "revert")) {
            const bool from_parent = kw == "inherit";
            for (int side = 0; side < 4; ++side) {
                s.border_width[side] = from_parent ? parent.border_width[side] : 3.0f;
                s.border_color[side] = from_parent ? parent.border_color[side] : s.color;
                border_none_[side] = from_parent ? parent.border_width[side] == 0 : true;
                s.border_style[side] = from_parent ? parent.border_style[side] : BorderStyle::None;
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
        if (p == "float") {
            s.float_side = kw == "left" || kw == "inline-start" ? Float::Left : kw == "right" || kw == "inline-end" ? Float::Right : Float::None;
            return;
        }
        if (p == "clear") {
            s.clear = kw == "left" ? Clear::Left : kw == "right" ? Clear::Right : kw == "both" ? Clear::Both : Clear::None;
            return;
        }
        if (p == "overflow" || p == "overflow-x" || p == "overflow-y") {
            const auto starts = valueStarts(v);
            bool clip = false;
            for (size_t start : starts)
                if (v[start].type == TokenType::Ident && lower(v[start].text) != "visible") clip = true;
            if (p == "overflow" || clip) s.overflow_clip = clip;
            return;
        }
        if (p == "border-radius") {
            // Up to four horizontal radii, then maybe "/" and up to four vertical ones.
            std::vector<Length> lists[2];
            int which = 0;
            for (size_t start : valueStarts(v)) {
                if (v[start].type == TokenType::Delim && v[start].text == "/") {
                    if (which == 1) return;
                    which = 1;
                    continue;
                }
                size_t k = start;
                Length l;
                if (!lengthAt(v, k, c, l, false) || l.px < 0 || l.percent < 0 || lists[which].size() == 4) return;
                lists[which].push_back(l);
            }
            if (lists[0].empty() || (which == 1 && lists[1].empty())) return;
            if (which == 0) lists[1] = lists[0];
            for (int axis = 0; axis < 2; ++axis) {
                const std::vector<Length>& l = lists[axis];
                const Length tl = l[0], tr = l.size() > 1 ? l[1] : l[0], br = l.size() > 2 ? l[2] : l[0], bl = l.size() > 3 ? l[3] : tr;
                s.radius[0][axis] = tl;
                s.radius[1][axis] = tr;
                s.radius[2][axis] = br;
                s.radius[3][axis] = bl;
            }
            return;
        }
        if (p == "border-top-left-radius" || p == "border-top-right-radius" || p == "border-bottom-right-radius" || p == "border-bottom-left-radius") {
            const int corner = p == "border-top-left-radius" ? 0 : p == "border-top-right-radius" ? 1 : p == "border-bottom-right-radius" ? 2 : 3;
            std::vector<Length> got;
            for (size_t start : valueStarts(v)) {
                size_t k = start;
                Length l;
                if (!lengthAt(v, k, c, l, false) || l.px < 0 || l.percent < 0 || got.size() == 2) return;
                got.push_back(l);
            }
            if (got.empty()) return;
            s.radius[corner][0] = got[0];
            s.radius[corner][1] = got.size() > 1 ? got[1] : got[0];
            return;
        }
        if (p == "box-shadow" || p == "-webkit-box-shadow") {
            if (kw == "none") { s.box_shadow = nullptr; return; }
            auto shadows = std::make_shared<std::vector<BoxShadow>>();
            for (const Tokens& part : commaParts(v)) {
                BoxShadow sh;
                sh.color = s.color;
                std::vector<float> lengths;
                for (size_t start : valueStarts(part)) {
                    size_t k = start;
                    if (part[k].type == TokenType::Ident && lower(part[k].text) == "inset") { sh.inset = true; continue; }
                    Length l;
                    if (lengthAt(part, k, c, l, false)) {
                        if (lengths.size() == 4) return;
                        lengths.push_back(l.px);
                        continue;
                    }
                    k = start;
                    Color col;
                    if (!colorFromTokens(part, k, s.color, col)) return;
                    sh.color = col;
                }
                if (lengths.size() < 2) return;
                sh.x = lengths[0];
                sh.y = lengths[1];
                sh.blur = lengths.size() > 2 ? std::max(0.0f, lengths[2]) : 0;
                sh.spread = lengths.size() > 3 ? lengths[3] : 0;
                if (shadows->size() < 16) shadows->push_back(sh);
            }
            s.box_shadow = shadows->empty() ? nullptr : std::move(shadows);
            return;
        }
        if (p == "vertical-align") {
            if (kw == "top" || kw == "text-top") s.vertical_align = VerticalAlign::Top;
            else if (kw == "middle") s.vertical_align = VerticalAlign::Middle;
            else if (kw == "bottom" || kw == "text-bottom") s.vertical_align = VerticalAlign::Bottom;
            else if (kw == "sub") s.vertical_align = VerticalAlign::Sub;
            else if (kw == "super") s.vertical_align = VerticalAlign::Super;
            else s.vertical_align = VerticalAlign::Baseline;
            return;
        }
        if (p == "table-layout") { s.table_fixed = kw == "fixed"; return; }
        if (p == "border-collapse") { s.border_collapse = kw == "collapse"; return; }
        if (p == "border-spacing") {
            Length l;
            size_t k = i;
            if (lengthAt(v, k, c, l, false)) s.border_spacing = std::max(0.0f, l.px);
            return;
        }
        if (p == "content") { setContent(s, v); return; }
        if (p == "mask" || p == "-webkit-mask" || p.starts_with("mask-") || p.starts_with("-webkit-mask-")) {
            const std::string longhand = p == "mask" || p == "-webkit-mask" ? std::string{} : p.substr(p.find("mask-") + 5);
            if (longhand.empty()) {
                BackgroundLayers layers;
                if (layersFromShorthand(v, c, s.color, true, layers, nullptr)) s.mask_layers = std::make_shared<BackgroundLayers>(std::move(layers));
                return;
            }
            BackgroundLayers layers = s.mask_layers ? *s.mask_layers : BackgroundLayers{};
            if (!s.mask_layers) layers.origins = {BackgroundBox::Border};
            if (setLayerLonghand(longhand, v, c, s.color, layers)) s.mask_layers = std::make_shared<BackgroundLayers>(std::move(layers));
            return;
        }
        if (p == "fill" || p == "stroke") {
            SvgPaint paint;
            if (!svgPaintFrom(v, s.color, paint)) return;
            (p == "fill" ? s.fill : s.stroke) = paint;
            s.svg_set |= p == "fill" ? kSetFill : kSetStroke;
            return;
        }
        if (p == "stroke-width") { float w = 0; if (svgLengthFrom(v, c, w) && w >= 0) { s.stroke_width = w; s.svg_set |= kSetStrokeWidth; } return; }
        if (p == "stroke-miterlimit") { float m = 0; if (svgNumberFrom(v, m) && m >= 1) { s.stroke_miterlimit = m; s.svg_set |= kSetMiter; } return; }
        if (p == "stroke-dashoffset") { float d = 0; if (svgLengthFrom(v, c, d)) { s.stroke_dashoffset = d; s.svg_set |= kSetDashOffset; } return; }
        if (p == "fill-opacity" || p == "stroke-opacity" || p == "stop-opacity") {
            float o = 0;
            if (!svgNumberFrom(v, o)) return;
            o = std::clamp(o, 0.0f, 1.0f);
            if (p == "fill-opacity") { s.fill_opacity = o; s.svg_set |= kSetFillOpacity; }
            else if (p == "stroke-opacity") { s.stroke_opacity = o; s.svg_set |= kSetStrokeOpacity; }
            else s.stop_opacity = o;
            return;
        }
        if (p == "fill-rule") { s.fill_evenodd = kw == "evenodd"; s.svg_set |= kSetFillRule; return; }
        if (p == "stroke-linecap") { s.stroke_linecap = kw == "round" ? 1 : kw == "square" ? 2 : 0; s.svg_set |= kSetLinecap; return; }
        if (p == "stroke-linejoin") { s.stroke_linejoin = kw == "round" ? 1 : kw == "bevel" ? 2 : 0; s.svg_set |= kSetLinejoin; return; }
        if (p == "stroke-dasharray") {
            s.svg_set |= kSetDash;
            if (kw == "none") { s.stroke_dasharray = nullptr; return; }
            auto dashes = std::make_shared<std::vector<float>>();
            for (size_t start : valueStarts(v)) {
                if (v[start].type == TokenType::Comma) continue;
                Tokens one{v[start]};
                float d = 0;
                if (!svgLengthFrom(one, c, d) || d < 0) return;
                if (dashes->size() < 64) dashes->push_back(d);
            }
            if (dashes->size() % 2) dashes->insert(dashes->end(), dashes->begin(), dashes->end());
            float total = 0;
            for (float d : *dashes) total += d;
            s.stroke_dasharray = total > 0 ? std::move(dashes) : nullptr;
            return;
        }
        if (p == "stop-color") { oneColor(s.stop_color); return; }
        if (p == "clip-path") {
            size_t k = i;
            std::string url;
            s.clip_path.clear();
            if (urlAt(v, k, url) && url.find('#') != std::string::npos) s.clip_path = url.substr(url.find('#'));
            return;
        }
        if (p.starts_with("grid") || p.starts_with("justify-") || p.starts_with("place-")) {
            if (applyGrid(s, p, v, kw, c)) return;
        }
        if (p == "flex") { setFlexShorthand(s, v, c); return; }
        if (p == "flex-grow") { if (i < v.size() && v[i].type == TokenType::Number) s.flex_grow = std::max(0.0f, static_cast<float>(v[i].number)); return; }
        if (p == "flex-shrink") { if (i < v.size() && v[i].type == TokenType::Number) s.flex_shrink = std::max(0.0f, static_cast<float>(v[i].number)); return; }
        if (p == "flex-basis") { oneLength(s.flex_basis, true); return; }
        if (p == "flex-direction" || p == "flex-wrap" || p == "flex-flow") {
            for (size_t start : valueStarts(v)) {
                if (v[start].type != TokenType::Ident) continue;
                const std::string k = lower(v[start].text);
                if (k == "row") { s.flex_column = false; s.flex_reverse = false; }
                else if (k == "row-reverse") { s.flex_column = false; s.flex_reverse = true; }
                else if (k == "column") { s.flex_column = true; s.flex_reverse = false; }
                else if (k == "column-reverse") { s.flex_column = true; s.flex_reverse = true; }
                else if (k == "wrap" || k == "wrap-reverse") s.flex_wrap = true;
                else if (k == "nowrap") s.flex_wrap = false;
            }
            return;
        }
        if (p == "align-items") { s.align_items = kw == "normal" ? FlexAlign::Stretch : flexAlign(kw, s.align_items); return; }
        if (p == "align-self") { s.align_self = kw == "normal" ? FlexAlign::Auto : flexAlign(kw, s.align_self); return; }
        if (p == "align-content") { s.align_content = kw == "normal" ? FlexAlign::Stretch : flexAlign(kw, s.align_content); return; }
        if (p == "order") { if (i < v.size() && v[i].type == TokenType::Number) s.order = static_cast<int>(std::clamp(v[i].number, -1e6, 1e6)); return; }
        if (p == "grid-row-gap" || p == "grid-column-gap") {
            Length l;
            size_t k = i;
            if (lengthAt(v, k, c, l, false)) (p == "grid-row-gap" ? s.row_gap : s.column_gap) = std::max(0.0f, l.px);
            return;
        }
        if (p == "gap" || p == "row-gap" || p == "column-gap" || p == "grid-gap") {
            Length first, second;
            size_t k = i;
            if (!lengthAt(v, k, c, first, false)) return;
            skipWs(v, k);
            second = first;
            if (k < v.size()) (void)lengthAt(v, k, c, second, false);
            if (p != "column-gap") s.row_gap = std::max(0.0f, first.px);
            if (p != "row-gap") s.column_gap = std::max(0.0f, p == "column-gap" ? first.px : second.px);
            return;
        }
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
        if (p == "visibility") { s.hidden = kw == "hidden" || kw == "collapse"; s.svg_set |= kSetVisibility; return; }
        if (p == "opacity") {
            if (i < v.size() && v[i].type == TokenType::Number) s.opacity = std::clamp(static_cast<float>(v[i].number), 0.0f, 1.0f);
            else if (i < v.size() && v[i].type == TokenType::Percentage) s.opacity = std::clamp(static_cast<float>(v[i].number) / 100.0f, 0.0f, 1.0f);
            return;
        }
        if (p == "box-sizing") { s.border_box = kw == "border-box"; return; }
        if (p == "background-color") { oneColor(s.background); return; }
        if (p == "background") {
            BackgroundLayers layers;
            Color color{0, 0, 0, 0};
            if (!layersFromShorthand(v, c, s.color, false, layers, &color)) return;
            s.background = color;
            s.background_layers = layers.empty() ? nullptr : std::make_shared<BackgroundLayers>(std::move(layers));
            return;
        }
        if (p.starts_with("background-")) {
            BackgroundLayers layers = s.background_layers ? *s.background_layers : BackgroundLayers{};
            if (setLayerLonghand(p.substr(11), v, c, s.color, layers)) s.background_layers = std::make_shared<BackgroundLayers>(std::move(layers));
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
        if (p == "width") {
            using Keyword = ComputedStyle::WidthKeyword;
            const Keyword keyword_width = kw == "max-content" ? Keyword::MaxContent : kw == "min-content" ? Keyword::MinContent : kw == "fit-content" ? Keyword::FitContent : Keyword::None;
            if (keyword_width != Keyword::None) {
                s.width = Length::autoLength();
                s.width_keyword = keyword_width;
                return;
            }
            size_t k = i;
            Length l;
            if (lengthAt(v, k, c, l, true)) {
                s.width = l;
                s.width_keyword = Keyword::None;
            }
            noNegative(s.width);
            return;
        }
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
            const auto setSide = [&](int side, size_t idx) {
                border_none_[side] = noneAt(idx);
                s.border_style[side] = idx < starts.size() ? borderStyleOf(lower(v[starts[idx]].text)) : BorderStyle::None;
            };
            if (p == "border-top-style") setSide(Top, 0);
            else if (p == "border-right-style") setSide(Right, 0);
            else if (p == "border-bottom-style") setSide(Bottom, 0);
            else if (p == "border-left-style") setSide(Left, 0);
            else {
                const size_t n = starts.size();
                setSide(Top, 0);
                setSide(Right, n > 1 ? 1 : 0);
                setSide(Bottom, n > 2 ? 2 : 0);
                setSide(Left, n > 3 ? 3 : (n > 1 ? 1 : 0));
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
        if (p == "text-overflow") {
            // ellipsis or clip; a string or two values (one per end) count by their first.
            if (kw == "ellipsis") s.text_overflow_ellipsis = true;
            else if (kw == "clip") s.text_overflow_ellipsis = false;
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

    static void inherit(const ComputedStyle& parent, ComputedStyle& s) {
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
        s.border_spacing = parent.border_spacing;
        s.border_collapse = parent.border_collapse;
        s.fill = parent.fill;
        s.stroke = parent.stroke;
        s.stroke_width = parent.stroke_width;
        s.stroke_miterlimit = parent.stroke_miterlimit;
        s.stroke_dashoffset = parent.stroke_dashoffset;
        s.fill_opacity = parent.fill_opacity;
        s.stroke_opacity = parent.stroke_opacity;
        s.fill_evenodd = parent.fill_evenodd;
        s.stroke_linecap = parent.stroke_linecap;
        s.stroke_linejoin = parent.stroke_linejoin;
        s.stroke_dasharray = parent.stroke_dasharray;
    }

    void resolveInto(const Node* el, const ComputedStyle& parent, ComputedStyle& s, const Scope* scope, const Node* slot) {
        inherit(parent, s);
        std::vector<MatchedDecl> matched;
        host_context_ = nullptr;
        collect(el, ua_.normal, matched);
        host_context_ = scope->host_el;
        collect(el, scope->normal, matched);
        // A host's own :host rules, below the rules of the tree it's in.
        if (el->shadow_root) {
            const auto it = shadow_scopes_.find(el->shadow_root);
            if (it != shadow_scopes_.end()) {
                host_context_ = el;
                fuel_ = kSelectorFuel;
                std::vector<MatchedDecl> host_matched;
                gather(el, it->second->host, host_matched);
                for (MatchedDecl& m : host_matched) {
                    if (!m.important) m.origin = Origin::Hint;
                    matched.push_back(m);
                }
            }
        }
        // ::slotted() rules of the shadow tree whose slot this element went into.
        if (slot) {
            const Scope* slot_scope = scopeFor(slot);
            for (const SlottedRule& r : slot_scope->slotted) {
                if (!r.slotted) continue;
                host_context_ = nullptr;
                fuel_ = kSelectorFuel;
                bool hit = false;
                for (const ComplexSelector& a : r.slotted->args)
                    if (matchComplex(el, a)) hit = true;
                if (!hit) continue;
                host_context_ = slot_scope->host_el;
                if (!matchComplex(slot, r.slot_side)) continue;
                for (const Declaration& d : r.rule->declarations)
                    matched.push_back({&d, d.important, d.important ? r.origin : Origin::Hint, r.slot_side.specificity.key(), r.order});
            }
        }
        host_context_ = scope->host_el;

        std::string hint_text;
        hints(el, hint_text);
        std::vector<Declaration> hint_decls;
        if (!hint_text.empty()) hint_decls = css::parseDeclarations(hint_text);
        for (const Declaration& d : hint_decls) matched.push_back({&d, false, Origin::Hint, 0, 0});

        std::vector<Declaration> inline_decls;
        if (const std::string* style_attr = el->attr("style")) inline_decls = css::parseDeclarations(*style_attr);
        for (const Declaration& d : inline_decls) matched.push_back({&d, d.important, Origin::Inline, 0, 0});

        applyMatched(el, parent, s, matched);
    }

    void collect(const Node* el, const Buckets& buckets, std::vector<MatchedDecl>& matched) {
        fuel_ = kSelectorFuel;
        const auto lookup = [&](const auto& table, const std::string& key) {
            const auto it = table.find(key);
            if (it != table.end()) gather(el, it->second, matched);
        };
        if (const std::string* id = el->attr("id")) lookup(buckets.by_id, *id);
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
                    if (seen.insert(word).second) lookup(buckets.by_class, word);
                }
            }
        }
        lookup(buckets.by_type, el->ns == Namespace::Html ? el->name : lower(el->name));
        gather(el, buckets.universal, matched);
    }

    void applyMatched(const Node* el, const ComputedStyle& parent, ComputedStyle& s, std::vector<MatchedDecl>& matched) {
        current_element_ = el;
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

        // Old pages' <p> at the top of the body has no top margin: the quirk applies only
        // to margins the page didn't ask for itself.
        const auto setByPage = [&](const char* longhand) {
            return std::any_of(matched.begin(), matched.end(), [&](const MatchedDecl& m) {
                return m.origin != Origin::UserAgent && (m.decl->name == longhand || m.decl->name == "margin");
            });
        };
        s.native_look = !std::any_of(matched.begin(), matched.end(), [](const MatchedDecl& m) {
            if (m.origin == Origin::UserAgent) return false;
            const std::string& n = m.decl->name;
            if (n.starts_with("border") || n.starts_with("background")) return true;
            if (n == "appearance" || n == "-webkit-appearance") {
                const std::string k = keyword(m.decl->value);
                return k == "none" || k == "textfield";
            }
            return false;
        });
        s.ua_margin_top = !setByPage("margin-top");
        s.ua_margin_bottom = !setByPage("margin-bottom");

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
                const std::string kw = keyword(*v);
                if (kw == "inherit" || kw == "unset" || kw == "revert") {
                    s.font_size = parent.font_size;
                    s.font_keyword = parent.font_keyword;
                    s.font_weight = parent.font_weight;
                    s.italic = parent.italic;
                    s.font_family = parent.font_family;
                    s.line_height = parent.line_height;
                    s.line_height_factor = parent.line_height_factor;
                    continue;
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
                s.svg_set |= kSetColor;
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
        if (!resolving_pseudo_ && el->parent && el->parent->type == NodeType::Document) {
            lengths_.root_font_size = s.font_size;
            lengths_.root_line_height = s.usedLineHeight();
        }

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
        // Its colour only shows through the mask: layout draws it that way, or not at all.
        s.masked = s.mask_layers && std::any_of(s.mask_layers->images.begin(), s.mask_layers->images.end(),
                                                [](const BackgroundImage& i) { return !i.url.empty(); });

        // Absolute, fixed and floated boxes are blocks, whatever they said.
        const bool out_of_flow = s.position == Position::Absolute || s.position == Position::Fixed;
        if (out_of_flow) s.float_side = Float::None;
        if (out_of_flow)
            s.inline_level_origin = s.display == Display::Inline || s.display == Display::InlineBlock || s.display == Display::InlineFlex ||
                                    s.display == Display::InlineGrid || s.display == Display::InlineTable;
        if (out_of_flow || s.float_side != Float::None) {
            switch (s.display) {
                case Display::Inline:
                case Display::InlineBlock:
                case Display::TableCaption:
                case Display::TableRowGroup:
                case Display::TableRow:
                case Display::TableCell: s.display = Display::Block; break;
                case Display::InlineTable: s.display = Display::Table; break;
                case Display::InlineFlex: s.display = Display::Flex; break;
                case Display::InlineGrid: s.display = Display::Grid; break;
                default: break;
            }
        }

        // Lines drawn by an ancestor run through its descendants' text regardless.
        s.underline = s.underline || parent.underline;
        s.line_through = s.line_through || parent.line_through;
    }

    void resolvePseudo(const Node* el, const ComputedStyle& s, const Buckets& ua, const Buckets& buckets, const Node* host,
                       std::unordered_map<const Node*, const ComputedStyle*>& into) {
        if (ua.empty() && buckets.empty()) return;
        std::vector<MatchedDecl> matched;
        host_context_ = nullptr;
        collect(el, ua, matched);
        host_context_ = host;
        collect(el, buckets, matched);
        if (matched.empty()) return;
        ComputedStyle pseudo;
        inherit(s, pseudo);
        resolving_pseudo_ = true;
        applyMatched(el, s, pseudo, matched);
        resolving_pseudo_ = false;
        if (!pseudo.has_content || pseudo.display == Display::None) return;
        out_.storage_.push_back(std::move(pseudo));
        into[el] = &out_.storage_.back();
    }

    // Down the composed tree: a host's shadow tree (in its scope), a slot's assigned
    // nodes (in the host's tree's scope), everyone else's children.
    void walk(const Node* el, const ComputedStyle& parent, size_t depth, const Scope* scope, const Node* slot) {
        if (depth > kMaxStyledDepth) return;
        out_.storage_.emplace_back();
        ComputedStyle& s = out_.storage_.back();
        resolveInto(el, parent, s, scope, slot);
        out_.styles_[el] = &s;
        // Nothing below gets a box; but an SVG's hidden parts can still be drawn by <use>.
        if (s.display == Display::None && el->ns != Namespace::Svg) return;
        resolvePseudo(el, s, ua_.before, scope->before, scope->host_el, out_.before_);
        resolvePseudo(el, s, ua_.after, scope->after, scope->host_el, out_.after_);
        if (el->shadow_root) {
            const auto it = shadow_scopes_.find(el->shadow_root);
            const Scope* inner = it == shadow_scopes_.end() ? scope : it->second.get();
            for (const Node* c : el->shadow_root->children)
                if (c->isElement()) walk(c, s, depth + 1, inner, nullptr);
            return;
        }
        if (el->isHtml("slot") && scope->host_el) {
            std::vector<const Node*> assigned;
            assignedNodes(el, assigned);
            if (!assigned.empty()) {
                const Scope* outer = scopeFor(scope->host_el);
                for (const Node* c : assigned)
                    if (c->isElement()) walk(c, s, depth + 1, outer, el);
                return;
            }
        }
        for (const Node* c : el->children)
            if (c->isElement()) walk(c, s, depth + 1, scope, nullptr);
    }

    const Document& doc_;
    StyledDocument& out_;
    const StyledDocument::SheetLoader& loader_;
    std::vector<std::unique_ptr<css::Stylesheet>> sheets_;
    Scope ua_, doc_scope_;
    Scope* target_ = &ua_;   // where addSheet puts rules
    std::unordered_map<const Node*, std::unique_ptr<Scope>> shadow_scopes_;   // by shadow root
    const Node* host_context_ = nullptr;   // the host of the shadow tree being matched in
    const Node* current_element_ = nullptr;
    bool resolving_pseudo_ = false;
    std::unordered_map<const Node*, SiblingInfo> siblings_;
    LengthContext lengths_;
    size_t order_ = 0;
    size_t import_count_ = 0;
    int fuel_ = 0;
    bool border_none_[4] = {true, true, true, true};
};

StyledDocument::StyledDocument(const Document& doc, css::MediaContext media, const SheetLoader& loader, const ChMeasure& measure_ch)
    : media_(media), measure_ch_(measure_ch) {
    StyleResolver resolver(doc, *this, loader);
    resolver.run();
}

const ComputedStyle* StyledDocument::beforeOf(const Node* element) const {
    const auto it = before_.find(element);
    return it == before_.end() ? nullptr : it->second;
}

const ComputedStyle* StyledDocument::afterOf(const Node* element) const {
    const auto it = after_.find(element);
    return it == after_.end() ? nullptr : it->second;
}

const ComputedStyle* StyledDocument::styleOf(const Node* element) const {
    const auto it = styles_.find(element);
    return it == styles_.end() ? nullptr : it->second;
}

}  // namespace fbx::xweb
