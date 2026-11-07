// Regular expressions: the pattern parsed by hand into a tree, the tree compiled to a
// small program, and the program run by a backtracking matcher with its own stack,
// so neither a long text nor a deep pattern can overflow the C++ one. Written from
// the ECMAScript specification (22.2), Annex B's web-compatible syntax included.

#include "regexp.h"

#include <algorithm>

#include "xweb/js.h"

namespace fbx::xweb::js::regex {

namespace {

constexpr uint32_t kInf = UINT32_MAX;

bool isLineTerm(char32_t c) { return c == u'\n' || c == u'\r' || c == 0x2028 || c == 0x2029; }
bool isWordChar(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
bool isHigh(char32_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool isLow(char32_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
bool isDigit(char32_t c) { return c >= '0' && c <= '9'; }
int hexValue(char32_t c) {
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<int>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<int>(c - 'A' + 10);
    return -1;
}

// ── Case ──
// The simple one-to-one mappings of the scripts pages use: Latin, Greek, Cyrillic,
// Armenian, the fullwidth forms and Deseret. Not the whole Unicode table.

char32_t lowerOf(char32_t c) {
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x100 && c <= 0x137) return c % 2 == 0 && c != 0x130 ? c + 1 : c;
    if (c >= 0x139 && c <= 0x148) return c % 2 == 1 ? c + 1 : c;
    if (c >= 0x14A && c <= 0x177) return c % 2 == 0 ? c + 1 : c;
    if (c == 0x178) return 0xFF;
    if (c >= 0x179 && c <= 0x17E) return c % 2 == 1 ? c + 1 : c;
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 63;
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) return c % 2 == 0 ? c + 1 : c;
    if (c >= 0x531 && c <= 0x556) return c + 48;
    if ((c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF)) return c % 2 == 0 ? c + 1 : c;
    if (c == 0x212A) return 'k';    // KELVIN SIGN
    if (c == 0x212B) return 0xE5;   // ANGSTROM SIGN
    if (c == 0x2126) return 0x3C9;  // OHM SIGN
    if (c >= 0xFF21 && c <= 0xFF3A) return c + 32;
    if (c >= 0x10400 && c <= 0x10427) return c + 40;
    return c;
}

char32_t upperOf(char32_t c) {
    if (c < 0x80) return c >= 'a' && c <= 'z' ? c - 32 : c;
    if (c == 0xB5) return 0x39C;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x137) return c % 2 == 1 && c != 0x131 ? c - 1 : c;
    if (c >= 0x139 && c <= 0x148) return c % 2 == 0 ? c - 1 : c;
    if (c >= 0x14A && c <= 0x177) return c % 2 == 1 ? c - 1 : c;
    if (c >= 0x179 && c <= 0x17E) return c % 2 == 0 ? c - 1 : c;
    if (c == 0x17F) return 'S';
    if (c == 0x3AC) return 0x386;
    if (c >= 0x3AD && c <= 0x3AF) return c - 37;
    if (c == 0x3CC) return 0x38C;
    if (c == 0x3CD || c == 0x3CE) return c - 63;
    if (c == 0x3C2) return 0x3A3;
    if (c >= 0x3B1 && c <= 0x3CB) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) return c % 2 == 1 ? c - 1 : c;
    if (c >= 0x561 && c <= 0x586) return c - 48;
    if ((c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF)) return c % 2 == 1 ? c - 1 : c;
    if (c >= 0xFF41 && c <= 0xFF5A) return c - 32;
    if (c >= 0x10428 && c <= 0x1044F) return c - 40;
    return c;
}

// Canonicalize (22.2.2.7.3): without the u flag, to upper case, but never from beyond
// ASCII into it; with it, simple case folding.
char32_t canonical(char32_t c, bool unicode) {
    if (unicode) return lowerOf(upperOf(c));
    const char32_t u = upperOf(c);
    return c >= 0x80 && u < 0x80 ? c : u;
}

// ── Unicode properties ──
// \p{…} with the u flag. The categories and scripts by their blocks: near enough for
// the tests pages make (letters, digits, punctuation, CJK, emoji), not the full table.

enum class Prop : uint8_t {
    None, Any, Ascii, AsciiHex, Letter, Upper, Lower, Titlecase, CasedLetter, Modifier, OtherLetter, Mark, Number, Decimal,
    LetterNumber, OtherNumber, Punct, Symbol, MathSymbol, Currency, Separator, SpaceSeparator, LineSeparator, ParaSeparator,
    Other, Control, Format, PrivateUse, WhiteSpace, Alphabetic, Emoji, EmojiPresentation, Pictographic, IdStart, IdContinue,
    Latin, Greek, Cyrillic, Han, Hiragana, Katakana, Hangul, Arabic, Hebrew, Thai, Devanagari, Armenian, Georgian,
};

template <size_t N>
bool inTable(char32_t c, const char32_t (&t)[N][2]) {
    for (const auto& r : t)
        if (c >= r[0] && c <= r[1]) return true;
    return false;
}

constexpr char32_t kMarks[][2] = {{0x300, 0x36F}, {0x483, 0x489}, {0x591, 0x5BD}, {0x5BF, 0x5BF}, {0x5C1, 0x5C2}, {0x5C4, 0x5C5},
    {0x610, 0x61A}, {0x64B, 0x65F}, {0x670, 0x670}, {0x6D6, 0x6DC}, {0x6DF, 0x6E4}, {0x900, 0x903}, {0x93A, 0x94F}, {0x951, 0x957},
    {0x962, 0x963}, {0xE31, 0xE31}, {0xE34, 0xE3A}, {0xE47, 0xE4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x20D0, 0x20FF},
    {0x302A, 0x302F}, {0x3099, 0x309A}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xE0100, 0xE01EF}};
constexpr char32_t kDecimal[][2] = {{'0', '9'}, {0x660, 0x669}, {0x6F0, 0x6F9}, {0x7C0, 0x7C9}, {0x966, 0x96F}, {0x9E6, 0x9EF},
    {0xA66, 0xA6F}, {0xAE6, 0xAEF}, {0xB66, 0xB6F}, {0xBE6, 0xBEF}, {0xC66, 0xC6F}, {0xCE6, 0xCEF}, {0xD66, 0xD6F}, {0xDE6, 0xDEF},
    {0xE50, 0xE59}, {0xED0, 0xED9}, {0xF20, 0xF29}, {0x1040, 0x1049}, {0x1090, 0x1099}, {0x17E0, 0x17E9}, {0x1810, 0x1819},
    {0x1946, 0x194F}, {0xA620, 0xA629}, {0xA8D0, 0xA8D9}, {0xA900, 0xA909}, {0xFF10, 0xFF19}, {0x104A0, 0x104A9}, {0x1D7CE, 0x1D7FF}};
constexpr char32_t kLetterNumber[][2] = {{0x16EE, 0x16F0}, {0x2160, 0x2188}, {0x3007, 0x3007}, {0x3021, 0x3029}, {0x3038, 0x303A}};
constexpr char32_t kOtherNumber[][2] = {{0xB2, 0xB3}, {0xB9, 0xB9}, {0xBC, 0xBE}, {0x2070, 0x2070}, {0x2074, 0x2079}, {0x2080, 0x2089},
    {0x2150, 0x215F}, {0x2460, 0x249B}, {0x24EA, 0x24FF}, {0x2776, 0x2793}, {0x3192, 0x3195}, {0x3220, 0x3229}, {0x3248, 0x324F},
    {0x3251, 0x325F}, {0x3280, 0x3289}, {0x32B1, 0x32BF}};
constexpr char32_t kPunct[][2] = {{'!', '#'}, {'%', '*'}, {',', '/'}, {':', ';'}, {'?', '@'}, {'[', ']'}, {'_', '_'}, {'{', '{'}, {'}', '}'},
    {0xA1, 0xA1}, {0xA7, 0xA7}, {0xAB, 0xAB}, {0xB6, 0xB7}, {0xBB, 0xBB}, {0xBF, 0xBF}, {0x37E, 0x37E}, {0x387, 0x387},
    {0x55A, 0x55F}, {0x589, 0x58A}, {0x5BE, 0x5BE}, {0x5C0, 0x5C0}, {0x5F3, 0x5F4}, {0x60C, 0x60D}, {0x61B, 0x61F}, {0x66A, 0x66D},
    {0x6D4, 0x6D4}, {0x964, 0x965}, {0x970, 0x970}, {0xE4F, 0xE4F}, {0xE5A, 0xE5B}, {0x2010, 0x2027}, {0x2030, 0x2043},
    {0x2045, 0x2051}, {0x2053, 0x205E}, {0x207D, 0x207E}, {0x208D, 0x208E}, {0x2308, 0x230B}, {0x2329, 0x232A}, {0x2768, 0x2775},
    {0x27C5, 0x27C6}, {0x27E6, 0x27EF}, {0x2983, 0x2998}, {0x29D8, 0x29DB}, {0x29FC, 0x29FD}, {0x2E00, 0x2E4F}, {0x3001, 0x3003},
    {0x3008, 0x3011}, {0x3014, 0x301F}, {0x3030, 0x3030}, {0x303D, 0x303D}, {0x30A0, 0x30A0}, {0x30FB, 0x30FB}, {0xFE10, 0xFE19},
    {0xFE30, 0xFE52}, {0xFE54, 0xFE61}, {0xFE63, 0xFE63}, {0xFE68, 0xFE68}, {0xFE6A, 0xFE6B}, {0xFF01, 0xFF03}, {0xFF05, 0xFF0A},
    {0xFF0C, 0xFF0F}, {0xFF1A, 0xFF1B}, {0xFF1F, 0xFF20}, {0xFF3B, 0xFF3D}, {0xFF3F, 0xFF3F}, {0xFF5B, 0xFF5B}, {0xFF5D, 0xFF5D},
    {0xFF5F, 0xFF65}};
constexpr char32_t kMath[][2] = {{'+', '+'}, {'<', '>'}, {'|', '|'}, {'~', '~'}, {0xAC, 0xAC}, {0xB1, 0xB1}, {0xD7, 0xD7}, {0xF7, 0xF7},
    {0x3F6, 0x3F6}, {0x2044, 0x2044}, {0x2052, 0x2052}, {0x207A, 0x207C}, {0x208A, 0x208C}, {0x2118, 0x2118}, {0x2140, 0x2144},
    {0x2190, 0x2194}, {0x21D2, 0x21D2}, {0x21D4, 0x21D4}, {0x2200, 0x22FF}, {0x2320, 0x2321}, {0x237C, 0x237C}, {0x25B7, 0x25B7},
    {0x25C1, 0x25C1}, {0x27C0, 0x27C4}, {0x27C7, 0x27E5}, {0x27F0, 0x27FF}, {0x2900, 0x2982}, {0x2999, 0x29D7}, {0x29DC, 0x29FB},
    {0x29FE, 0x2AFF}, {0xFB29, 0xFB29}, {0xFE62, 0xFE62}, {0xFE64, 0xFE66}, {0xFF0B, 0xFF0B}, {0xFF1C, 0xFF1E}, {0xFF5C, 0xFF5C},
    {0xFF5E, 0xFF5E}};
constexpr char32_t kCurrency[][2] = {{'$', '$'}, {0xA2, 0xA5}, {0x58F, 0x58F}, {0x60B, 0x60B}, {0x9F2, 0x9F3}, {0xE3F, 0xE3F},
    {0x20A0, 0x20C0}, {0xFDFC, 0xFDFC}, {0xFE69, 0xFE69}, {0xFF04, 0xFF04}, {0xFFE0, 0xFFE1}, {0xFFE5, 0xFFE6}};
constexpr char32_t kOtherSymbol[][2] = {{'^', '^'}, {'`', '`'}, {0xA6, 0xA6}, {0xA8, 0xA9}, {0xAE, 0xB0}, {0xB4, 0xB4}, {0xB8, 0xB8},
    {0x2C2, 0x2C5}, {0x2D2, 0x2DF}, {0x2E5, 0x2EB}, {0x2ED, 0x2ED}, {0x2EF, 0x2FF}, {0x375, 0x375}, {0x384, 0x385}, {0x482, 0x482},
    {0x606, 0x608}, {0x60E, 0x60F}, {0x6DE, 0x6DE}, {0x6E9, 0x6E9}, {0x2100, 0x2101}, {0x2103, 0x2106}, {0x2108, 0x2109},
    {0x2114, 0x2114}, {0x2116, 0x2117}, {0x211E, 0x2123}, {0x2125, 0x2125}, {0x2127, 0x2127}, {0x2129, 0x2129}, {0x212E, 0x212E},
    {0x213A, 0x213B}, {0x214A, 0x214D}, {0x214F, 0x214F}, {0x2195, 0x21FF}, {0x2300, 0x2307}, {0x230C, 0x231F}, {0x2322, 0x2328},
    {0x232B, 0x23FF}, {0x2400, 0x2426}, {0x2440, 0x244A}, {0x249C, 0x24E9}, {0x2500, 0x2767}, {0x2794, 0x27BF}, {0x2800, 0x28FF},
    {0x2B00, 0x2BFF}, {0x2CE5, 0x2CEA}, {0x2E80, 0x2FFB}, {0x3004, 0x3004}, {0x3012, 0x3013}, {0x3020, 0x3020}, {0x3036, 0x3037},
    {0x303E, 0x303F}, {0x309B, 0x309C}, {0x3190, 0x3191}, {0x3196, 0x319F}, {0x31C0, 0x31E3}, {0x3200, 0x321E}, {0x322A, 0x3247},
    {0x3250, 0x3250}, {0x3260, 0x327F}, {0x328A, 0x32B0}, {0x32C0, 0x33FF}, {0x4DC0, 0x4DFF}, {0xFFE2, 0xFFE4}, {0xFFE8, 0xFFEE},
    {0xFFFC, 0xFFFD}, {0x1F000, 0x1FAFF}};
constexpr char32_t kSpaces[][2] = {{0x20, 0x20}, {0xA0, 0xA0}, {0x1680, 0x1680}, {0x2000, 0x200A}, {0x202F, 0x202F}, {0x205F, 0x205F},
    {0x3000, 0x3000}};
constexpr char32_t kFormat[][2] = {{0xAD, 0xAD}, {0x600, 0x605}, {0x61C, 0x61C}, {0x6DD, 0x6DD}, {0x180E, 0x180E}, {0x200B, 0x200F},
    {0x202A, 0x202E}, {0x2060, 0x2064}, {0x2066, 0x206F}, {0xFEFF, 0xFEFF}, {0xFFF9, 0xFFFB}, {0xE0001, 0xE0001}, {0xE0020, 0xE007F}};
constexpr char32_t kModifier[][2] = {{0x2B0, 0x2C1}, {0x2C6, 0x2D1}, {0x2E0, 0x2E4}, {0x2EC, 0x2EC}, {0x2EE, 0x2EE}, {0x374, 0x374},
    {0x37A, 0x37A}, {0x559, 0x559}, {0x640, 0x640}, {0x6E5, 0x6E6}, {0xE46, 0xE46}, {0x1D2C, 0x1D6A}, {0x1D78, 0x1D78},
    {0x1D9B, 0x1DBF}, {0x2071, 0x2071}, {0x207F, 0x207F}, {0x2090, 0x209C}, {0x2C7C, 0x2C7D}, {0x3005, 0x3005}, {0x3031, 0x3035},
    {0x303B, 0x303B}, {0x309D, 0x309E}, {0x30FC, 0x30FE}, {0xA015, 0xA015}, {0xFF70, 0xFF70}, {0xFF9E, 0xFF9F}};
constexpr char32_t kLetterBlocks[][2] = {{'A', 'Z'}, {'a', 'z'}, {0xAA, 0xAA}, {0xB5, 0xB5}, {0xBA, 0xBA}, {0xC0, 0xD6}, {0xD8, 0xF6},
    {0xF8, 0x2C1}, {0x2C6, 0x2D1}, {0x2E0, 0x2E4}, {0x2EC, 0x2EC}, {0x2EE, 0x2EE}, {0x370, 0x374}, {0x376, 0x377}, {0x37A, 0x37D},
    {0x37F, 0x37F}, {0x386, 0x386}, {0x388, 0x3F5}, {0x3F7, 0x481}, {0x48A, 0x52F}, {0x531, 0x556}, {0x559, 0x559}, {0x560, 0x588},
    {0x5D0, 0x5EA}, {0x5EF, 0x5F2}, {0x620, 0x64A}, {0x66E, 0x66F}, {0x671, 0x6D3}, {0x6D5, 0x6D5}, {0x6E5, 0x6E6}, {0x6EE, 0x6EF},
    {0x6FA, 0x6FC}, {0x6FF, 0x6FF}, {0x710, 0x72F}, {0x74D, 0x7A5}, {0x904, 0x939}, {0x93D, 0x93D}, {0x950, 0x950}, {0x958, 0x961},
    {0x971, 0x980}, {0x985, 0x9B9}, {0xA05, 0xA39}, {0xA85, 0xAB9}, {0xB05, 0xB39}, {0xB85, 0xBB9}, {0xC05, 0xC39}, {0xC85, 0xCB9},
    {0xD05, 0xD3A}, {0xD85, 0xDC6}, {0xE01, 0xE30}, {0xE32, 0xE33}, {0xE40, 0xE46}, {0xE81, 0xEB0}, {0xF40, 0xF6C}, {0x1000, 0x102A},
    {0x10A0, 0x10FF}, {0x1100, 0x1248}, {0x1250, 0x135A}, {0x13A0, 0x13FD}, {0x1401, 0x166C}, {0x1780, 0x17B3}, {0x1820, 0x1878},
    {0x1D00, 0x1DBF}, {0x1E00, 0x1F15}, {0x1F18, 0x1F1D}, {0x1F20, 0x1F45}, {0x1F48, 0x1F4D}, {0x1F50, 0x1F7D}, {0x1F80, 0x1FBC},
    {0x1FBE, 0x1FBE}, {0x1FC2, 0x1FCC}, {0x1FD0, 0x1FDB}, {0x1FE0, 0x1FEC}, {0x1FF2, 0x1FFC}, {0x2071, 0x2071}, {0x207F, 0x207F},
    {0x2090, 0x209C}, {0x2102, 0x2102}, {0x2107, 0x2107}, {0x210A, 0x2113}, {0x2115, 0x2115}, {0x2119, 0x211D}, {0x2124, 0x2124},
    {0x2126, 0x2126}, {0x2128, 0x2128}, {0x212A, 0x212D}, {0x212F, 0x2139}, {0x213C, 0x213F}, {0x2145, 0x2149}, {0x214E, 0x214E},
    {0x2183, 0x2184}, {0x2C00, 0x2CE4}, {0x2D00, 0x2D25}, {0x2D30, 0x2D67}, {0x3005, 0x3006}, {0x3031, 0x3035}, {0x303B, 0x303C},
    {0x3041, 0x3096}, {0x309D, 0x309F}, {0x30A1, 0x30FA}, {0x30FC, 0x30FF}, {0x3105, 0x312F}, {0x3131, 0x318E}, {0x31A0, 0x31BF},
    {0x31F0, 0x31FF}, {0x3400, 0x4DBF}, {0x4E00, 0xA48C}, {0xA4D0, 0xA4FD}, {0xA500, 0xA60C}, {0xA640, 0xA66E}, {0xA680, 0xA69D},
    {0xA6A0, 0xA6E5}, {0xA717, 0xA7FF}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFB00, 0xFB06}, {0xFB1D, 0xFB28}, {0xFB2A, 0xFDFB},
    {0xFE70, 0xFEFC}, {0xFF21, 0xFF3A}, {0xFF41, 0xFF5A}, {0xFF66, 0xFFDC}, {0x10000, 0x100FA}, {0x10400, 0x1049D},
    {0x1D400, 0x1D7CB}, {0x1E900, 0x1E943}, {0x20000, 0x3134A}};
constexpr char32_t kPictographic[][2] = {{0xA9, 0xA9}, {0xAE, 0xAE}, {0x203C, 0x203C}, {0x2049, 0x2049}, {0x2122, 0x2122}, {0x2139, 0x2139},
    {0x2194, 0x2199}, {0x21A9, 0x21AA}, {0x231A, 0x231B}, {0x2328, 0x2328}, {0x2388, 0x2388}, {0x23CF, 0x23CF}, {0x23E9, 0x23F3},
    {0x23F8, 0x23FA}, {0x24C2, 0x24C2}, {0x25AA, 0x25AB}, {0x25B6, 0x25B6}, {0x25C0, 0x25C0}, {0x25FB, 0x25FE}, {0x2600, 0x27BF},
    {0x2934, 0x2935}, {0x2B05, 0x2B07}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x3030, 0x3030}, {0x303D, 0x303D},
    {0x3297, 0x3297}, {0x3299, 0x3299}, {0x1F000, 0x1F0FF}, {0x1F10D, 0x1F10F}, {0x1F12F, 0x1F12F}, {0x1F16C, 0x1F171},
    {0x1F17E, 0x1F17F}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F1AD, 0x1F1E5}, {0x1F201, 0x1F20F}, {0x1F21A, 0x1F21A},
    {0x1F22F, 0x1F22F}, {0x1F232, 0x1F23A}, {0x1F23C, 0x1F23F}, {0x1F249, 0x1F3FA}, {0x1F400, 0x1F53D}, {0x1F546, 0x1F64F},
    {0x1F680, 0x1F6FF}, {0x1F774, 0x1F77F}, {0x1F7D5, 0x1F7FF}, {0x1F80C, 0x1F80F}, {0x1F848, 0x1F84F}, {0x1F85A, 0x1F85F},
    {0x1F888, 0x1F88F}, {0x1F8AE, 0x1F8FF}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1FAFF}, {0x1FC00, 0x1FFFD}};
constexpr char32_t kEmojiPresentation[][2] = {{0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE},
    {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
    {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA},
    {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
    {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
    {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F1E6, 0x1F1FF}, {0x1F201, 0x1F201},
    {0x1F21A, 0x1F21A}, {0x1F22F, 0x1F22F}, {0x1F232, 0x1F236}, {0x1F238, 0x1F23A}, {0x1F250, 0x1F251}, {0x1F300, 0x1F320},
    {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0},
    {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E},
    {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5},
    {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB},
    {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF}};

bool isMark(char32_t c) { return inTable(c, kMarks); }
bool isLetter(char32_t c) { return inTable(c, kLetterBlocks) && !isMark(c) && !inTable(c, kDecimal); }
bool isUpper(char32_t c) { return lowerOf(c) != c || (c >= 0x1D400 && c <= 0x1D419) || c == 0x2102 || c == 0x210B || c == 0x2110; }
bool isLower(char32_t c) {
    return upperOf(c) != c || c == 0xDF || c == 0x138 || c == 0x149 || (c >= 0x250 && c <= 0x2AF) || (c >= 0x1D00 && c <= 0x1D2B) ||
           c == 0x210A || c == 0x210E || c == 0x210F || c == 0x2113;
}
bool isTitle(char32_t c) { return c == 0x1C5 || c == 0x1C8 || c == 0x1CB || c == 0x1F2 || (c >= 0x1F88 && c <= 0x1FAF && (c & 0xF) >= 8); }
bool isWhite(char32_t c) { return (c >= 9 && c <= 13) || inTable(c, kSpaces) || c == 0x85 || c == 0x2028 || c == 0x2029 || c == 0xFEFF; }

bool hasProp(Prop p, char32_t c) {
    switch (p) {
        case Prop::None: return false;
        case Prop::Any: return true;
        case Prop::Ascii: return c < 0x80;
        case Prop::AsciiHex: return c < 0x80 && hexValue(c) >= 0;
        case Prop::Letter: return isLetter(c);
        case Prop::Upper: return isLetter(c) && isUpper(c) && !isTitle(c);
        case Prop::Lower: return isLetter(c) && isLower(c) && !isTitle(c);
        case Prop::Titlecase: return isTitle(c);
        case Prop::CasedLetter: return isLetter(c) && (isUpper(c) || isLower(c));
        case Prop::Modifier: return inTable(c, kModifier);
        case Prop::OtherLetter: return isLetter(c) && !isUpper(c) && !isLower(c) && !inTable(c, kModifier);
        case Prop::Mark: return isMark(c);
        case Prop::Number: return inTable(c, kDecimal) || inTable(c, kLetterNumber) || inTable(c, kOtherNumber);
        case Prop::Decimal: return inTable(c, kDecimal);
        case Prop::LetterNumber: return inTable(c, kLetterNumber);
        case Prop::OtherNumber: return inTable(c, kOtherNumber);
        case Prop::Punct: return inTable(c, kPunct);
        case Prop::Symbol: return inTable(c, kMath) || inTable(c, kCurrency) || inTable(c, kOtherSymbol);
        case Prop::MathSymbol: return inTable(c, kMath);
        case Prop::Currency: return inTable(c, kCurrency);
        case Prop::Separator: return inTable(c, kSpaces) || c == 0x2028 || c == 0x2029;
        case Prop::SpaceSeparator: return inTable(c, kSpaces);
        case Prop::LineSeparator: return c == 0x2028;
        case Prop::ParaSeparator: return c == 0x2029;
        case Prop::Other: return c < 0x20 || (c >= 0x7F && c <= 0x9F) || inTable(c, kFormat) || (c >= 0xD800 && c <= 0xF8FF);
        case Prop::Control: return c < 0x20 || (c >= 0x7F && c <= 0x9F);
        case Prop::Format: return inTable(c, kFormat);
        case Prop::PrivateUse: return (c >= 0xE000 && c <= 0xF8FF) || c >= 0xF0000;
        case Prop::WhiteSpace: return isWhite(c);
        case Prop::Alphabetic: return isLetter(c) || inTable(c, kLetterNumber) || (isMark(c) && c >= 0x900);
        case Prop::Emoji: return inTable(c, kPictographic) || isDigit(c) || c == '#' || c == '*' || (c >= 0x1F1E6 && c <= 0x1F1FF);
        case Prop::EmojiPresentation: return inTable(c, kEmojiPresentation);
        case Prop::Pictographic: return inTable(c, kPictographic);
        case Prop::IdStart: return isLetter(c) || inTable(c, kLetterNumber);
        case Prop::IdContinue: return isLetter(c) || inTable(c, kLetterNumber) || isMark(c) || inTable(c, kDecimal) || c == '_';
        case Prop::Latin:
            return (c < 0x80 && isLetter(c)) || c == 0xAA || c == 0xBA || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
                   (c >= 0x1E00 && c <= 0x1EFF) || (c >= 0x2C60 && c <= 0x2C7F) || (c >= 0xA720 && c <= 0xA7FF) ||
                   (c >= 0xFF21 && c <= 0xFF3A) || (c >= 0xFF41 && c <= 0xFF5A) || (c >= 0xFB00 && c <= 0xFB06);
        case Prop::Greek: return (c >= 0x370 && c <= 0x3FF && c != 0x37E && c != 0x387) || (c >= 0x1F00 && c <= 0x1FFF);
        case Prop::Cyrillic: return (c >= 0x400 && c <= 0x52F) || (c >= 0x2DE0 && c <= 0x2DFF) || (c >= 0xA640 && c <= 0xA69F);
        case Prop::Han:
            return (c >= 0x2E80 && c <= 0x2FD5) || c == 0x3005 || c == 0x3007 || (c >= 0x3021 && c <= 0x3029) ||
                   (c >= 0x3038 && c <= 0x303B) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
                   (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x20000 && c <= 0x3134F);
        case Prop::Hiragana: return (c >= 0x3041 && c <= 0x3096) || (c >= 0x309D && c <= 0x309F);
        case Prop::Katakana: return (c >= 0x30A1 && c <= 0x30FA) || (c >= 0x30FD && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) || (c >= 0xFF66 && c <= 0xFF9D && c != 0xFF70);
        case Prop::Hangul: return (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3131 && c <= 0x318E) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xFFA0 && c <= 0xFFDC);
        case Prop::Arabic: return (c >= 0x600 && c <= 0x6FF && c != 0x60C && c != 0x61B && c != 0x61F && c != 0x640) || (c >= 0x750 && c <= 0x77F) || (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFC);
        case Prop::Hebrew: return (c >= 0x591 && c <= 0x5F4) || (c >= 0xFB1D && c <= 0xFB4F);
        case Prop::Thai: return (c >= 0xE01 && c <= 0xE3A) || (c >= 0xE40 && c <= 0xE5B);
        case Prop::Devanagari: return (c >= 0x900 && c <= 0x97F && c != 0x964 && c != 0x965) || (c >= 0xA8E0 && c <= 0xA8FF);
        case Prop::Armenian: return (c >= 0x531 && c <= 0x58A) || (c >= 0xFB13 && c <= 0xFB17);
        case Prop::Georgian: return (c >= 0x10A0 && c <= 0x10FF) || (c >= 0x2D00 && c <= 0x2D2D) || (c >= 0x1C90 && c <= 0x1CBF);
    }
    return false;
}

// A name in \p{…}: a category or a binary property alone, or Key=Value.
Prop propNamed(const std::string& key, const std::string& value) {
    struct Entry { const char* names[4]; Prop prop; };
    static const Entry kCategories[] = {
        {{"L", "Letter"}, Prop::Letter}, {{"Lu", "Uppercase_Letter"}, Prop::Upper}, {{"Ll", "Lowercase_Letter"}, Prop::Lower},
        {{"Lt", "Titlecase_Letter"}, Prop::Titlecase}, {{"LC", "Cased_Letter"}, Prop::CasedLetter}, {{"Lm", "Modifier_Letter"}, Prop::Modifier},
        {{"Lo", "Other_Letter"}, Prop::OtherLetter}, {{"M", "Mark", "Combining_Mark"}, Prop::Mark},
        {{"Mn", "Nonspacing_Mark", "Mc", "Spacing_Mark"}, Prop::Mark}, {{"Me", "Enclosing_Mark"}, Prop::Mark},
        {{"N", "Number"}, Prop::Number}, {{"Nd", "Decimal_Number", "digit"}, Prop::Decimal}, {{"Nl", "Letter_Number"}, Prop::LetterNumber},
        {{"No", "Other_Number"}, Prop::OtherNumber}, {{"P", "Punctuation", "punct"}, Prop::Punct},
        {{"Pc", "Connector_Punctuation", "Pd", "Dash_Punctuation"}, Prop::Punct}, {{"Ps", "Open_Punctuation", "Pe", "Close_Punctuation"}, Prop::Punct},
        {{"Pi", "Initial_Punctuation", "Pf", "Final_Punctuation"}, Prop::Punct}, {{"Po", "Other_Punctuation"}, Prop::Punct},
        {{"S", "Symbol"}, Prop::Symbol}, {{"Sm", "Math_Symbol"}, Prop::MathSymbol}, {{"Sc", "Currency_Symbol"}, Prop::Currency},
        {{"Sk", "Modifier_Symbol", "So", "Other_Symbol"}, Prop::Symbol}, {{"Z", "Separator"}, Prop::Separator},
        {{"Zs", "Space_Separator"}, Prop::SpaceSeparator}, {{"Zl", "Line_Separator"}, Prop::LineSeparator},
        {{"Zp", "Paragraph_Separator"}, Prop::ParaSeparator}, {{"C", "Other"}, Prop::Other}, {{"Cc", "Control", "cntrl"}, Prop::Control},
        {{"Cf", "Format"}, Prop::Format}, {{"Co", "Private_Use"}, Prop::PrivateUse}, {{"Cs", "Surrogate", "Cn", "Unassigned"}, Prop::None},
    };
    static const Entry kBinary[] = {
        {{"Any"}, Prop::Any}, {{"ASCII"}, Prop::Ascii}, {{"ASCII_Hex_Digit", "AHex", "Hex_Digit", "Hex"}, Prop::AsciiHex},
        {{"Alphabetic", "Alpha"}, Prop::Alphabetic}, {{"White_Space", "space"}, Prop::WhiteSpace}, {{"Uppercase", "Upper"}, Prop::Upper},
        {{"Lowercase", "Lower"}, Prop::Lower}, {{"Emoji"}, Prop::Emoji}, {{"Emoji_Presentation", "EPres"}, Prop::EmojiPresentation},
        {{"Extended_Pictographic", "ExtPict"}, Prop::Pictographic}, {{"ID_Start", "IDS"}, Prop::IdStart},
        {{"ID_Continue", "IDC"}, Prop::IdContinue}, {{"Assigned"}, Prop::Any},
    };
    static const Entry kScripts[] = {
        {{"Latin", "Latn"}, Prop::Latin}, {{"Greek", "Grek"}, Prop::Greek}, {{"Cyrillic", "Cyrl"}, Prop::Cyrillic},
        {{"Han", "Hani"}, Prop::Han}, {{"Hiragana", "Hira"}, Prop::Hiragana}, {{"Katakana", "Kana"}, Prop::Katakana},
        {{"Hangul", "Hang"}, Prop::Hangul}, {{"Arabic", "Arab"}, Prop::Arabic}, {{"Hebrew", "Hebr"}, Prop::Hebrew},
        {{"Thai"}, Prop::Thai}, {{"Devanagari", "Deva"}, Prop::Devanagari}, {{"Armenian", "Armn"}, Prop::Armenian},
        {{"Georgian", "Geor"}, Prop::Georgian},
    };
    const auto find = [](const auto& table, const std::string& n) -> std::optional<Prop> {
        for (const Entry& e : table)
            for (const char* name : e.names)
                if (name && n == name) return e.prop;
        return std::nullopt;
    };
    if (value.empty()) {
        if (auto p = find(kCategories, key)) return *p;
        if (auto p = find(kBinary, key)) return *p;
        return Prop::None;
    }
    if (key == "General_Category" || key == "gc") return find(kCategories, value).value_or(Prop::None);
    if (key == "Script" || key == "sc" || key == "Script_Extensions" || key == "scx") return find(kScripts, value).value_or(Prop::None);
    return Prop::None;
}

// ── Programs ──

struct Class {
    std::vector<std::pair<char32_t, char32_t>> ranges;   // sorted, merged
    std::vector<std::pair<Prop, bool>> props;            // \p{…}, or \P{…} when true
    bool negated = false;
};

void addRange(Class& k, char32_t a, char32_t b) { k.ranges.emplace_back(a, b); }

void addEscapeSet(Class& k, char32_t which) {
    const bool neg = which == 'D' || which == 'S' || which == 'W';
    std::vector<std::pair<char32_t, char32_t>> r;
    switch (which | 0x20) {
        case 'd': r = {{'0', '9'}}; break;
        case 'w': r = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}}; break;
        case 's': r = {{9, 13}, {0x20, 0x20}, {0xA0, 0xA0}, {0x1680, 0x1680}, {0x2000, 0x200A}, {0x2028, 0x2029}, {0x202F, 0x202F},
                       {0x205F, 0x205F}, {0x3000, 0x3000}, {0xFEFF, 0xFEFF}};
                  break;
        default: break;
    }
    if (!neg) {
        k.ranges.insert(k.ranges.end(), r.begin(), r.end());
        return;
    }
    char32_t next = 0;
    for (const auto& [a, b] : r) {
        if (a > next) k.ranges.emplace_back(next, a - 1);
        next = b + 1;
    }
    k.ranges.emplace_back(next, 0x10FFFF);
}

void finish(Class& k) {
    auto& r = k.ranges;
    std::sort(r.begin(), r.end());
    size_t w = 0;
    for (size_t i = 0; i < r.size(); ++i) {
        if (w > 0 && r[i].first <= r[w - 1].second + 1) r[w - 1].second = std::max(r[w - 1].second, r[i].second);
        else r[w++] = r[i];
    }
    r.resize(w);
}

bool inClass(const Class& k, char32_t c) {
    const auto it = std::upper_bound(k.ranges.begin(), k.ranges.end(), c, [](char32_t v, const auto& r) { return v < r.first; });
    if (it != k.ranges.begin() && c <= std::prev(it)->second) return true;
    for (const auto& [p, neg] : k.props)
        if (hasProp(p, c) != neg) return true;
    return false;
}

enum class Op : uint8_t { Char, Any, Class, Split, Jmp, Save, Reset, Bol, Eol, WordB, NotWordB, BackRef, Look, LoopEnter, LoopCheck, Star, Match };

struct Inst {
    Op op;
    bool back = false;   // reads leftward (inside a lookbehind)
    bool flag = false;   // Look: negative; Star: greedy
    uint32_t x = 0, y = 0;
    char32_t c = 0;
    uint32_t min = 0, max = 0;
};

struct Node {
    enum Kind : uint8_t { Empty, Char, Any, Class, Seq, Alt, Group, Look, BackRef, Bol, Eol, WordB, NotWordB, Repeat } kind;
    char32_t c = 0;
    int index = -1;            // Class: which; Group: capture number, or -1; BackRef: group
    bool neg = false, behind = false;
    uint32_t min = 0, max = 0;
    bool greedy = true;
    int first_group = 0, end_group = 0;   // Repeat: the groups inside, to clear each time round
    std::u16string name;       // a named backreference, until resolved
    std::vector<std::unique_ptr<Node>> kids;
    explicit Node(Kind k) : kind(k) {}
};

struct ParseError {
    std::string what;
};

}  // namespace

struct Program {
    std::vector<Inst> code;
    std::vector<Class> classes;
    size_t groups = 0;
    std::vector<std::pair<std::u16string, int>> names;
    Flags flags;
    uint32_t regs = 0;
    int first_char = -1;   // a literal every match starts with, to skip ahead to
    bool anchored = false; // starts with ^ outside multiline mode
};

namespace {

// ── Parsing ──

class Parser {
public:
    Parser(std::u16string_view src, const Flags& f) : flags_(f), u_(f.unicode || f.unicode_sets) {
        // In unicode mode the pattern is code points; otherwise code units.
        for (size_t i = 0; i < src.size(); ++i) {
            char32_t c = src[i];
            if (u_ && isHigh(c) && i + 1 < src.size() && isLow(src[i + 1])) {
                c = 0x10000 + ((c - 0xD800) << 10) + (src[i + 1] - 0xDC00);
                ++i;
            }
            p_ += c;
        }
        prescan();
    }

    std::unique_ptr<Node> parse() {
        auto n = disjunction();
        if (i_ < p_.size()) fail(p_[i_] == ')' ? "unmatched ')'" : "unexpected character");
        resolve(n.get());
        return n;
    }

    std::vector<Class> classes;
    int groups = 0;
    std::vector<std::pair<std::u16string, int>> names;

private:
    [[noreturn]] void fail(const std::string& what) { throw ParseError{what}; }
    bool more() const { return i_ < p_.size(); }
    char32_t peek(size_t ahead = 0) const { return i_ + ahead < p_.size() ? p_[i_ + ahead] : 0xFFFFFFFF; }
    bool eat(char32_t c) {
        if (peek() != c) return false;
        ++i_;
        return true;
    }

    // How many capturing groups the pattern has, and whether any is named: what
    // \1 and \k mean depends on both, wherever in the pattern they are.
    void prescan() {
        bool in_class = false;
        for (size_t i = 0; i < p_.size(); ++i) {
            const char32_t c = p_[i];
            if (c == '\\') {
                ++i;
            } else if (in_class) {
                if (c == ']') in_class = false;
            } else if (c == '[') {
                in_class = true;
            } else if (c == '(') {
                if (i + 1 < p_.size() && p_[i + 1] == '?') {
                    if (i + 3 < p_.size() && p_[i + 2] == '<' && p_[i + 3] != '=' && p_[i + 3] != '!') {
                        ++total_groups_;
                        has_named_ = true;
                    }
                } else {
                    ++total_groups_;
                }
            }
        }
    }

    struct Depth {
        Parser& p;
        explicit Depth(Parser& pp) : p(pp) {
            if (++p.depth_ > kMaxNesting) p.fail("pattern nested too deeply");
        }
        ~Depth() { --p.depth_; }
    };

    std::unique_ptr<Node> disjunction() {
        Depth d(*this);
        auto first = alternative();
        if (peek() != '|') return first;
        auto alt = std::make_unique<Node>(Node::Alt);
        alt->kids.push_back(std::move(first));
        while (eat('|')) alt->kids.push_back(alternative());
        return alt;
    }

    std::unique_ptr<Node> alternative() {
        auto seq = std::make_unique<Node>(Node::Seq);
        while (more() && peek() != '|' && peek() != ')') seq->kids.push_back(term());
        if (seq->kids.size() == 1) return std::move(seq->kids[0]);
        if (seq->kids.empty()) return std::make_unique<Node>(Node::Empty);
        return seq;
    }

    std::unique_ptr<Node> term() {
        const char32_t c = peek();
        if (c == '^') { ++i_; return std::make_unique<Node>(Node::Bol); }
        if (c == '$') { ++i_; return std::make_unique<Node>(Node::Eol); }
        if (c == '\\' && (peek(1) == 'b' || peek(1) == 'B')) {
            i_ += 2;
            auto n = std::make_unique<Node>(p_[i_ - 1] == 'b' ? Node::WordB : Node::NotWordB);
            if (isQuantifierStart()) fail("nothing to repeat");
            return n;
        }
        if (c == '(' && peek(1) == '?' && (peek(2) == '=' || peek(2) == '!' || (peek(2) == '<' && (peek(3) == '=' || peek(3) == '!')))) {
            auto n = std::make_unique<Node>(Node::Look);
            n->behind = peek(2) == '<';
            n->neg = peek(n->behind ? 3 : 2) == '!';
            i_ += n->behind ? 4 : 3;
            n->kids.push_back(disjunction());
            if (!eat(')')) fail("unterminated group");
            // Annex B: a lookahead may be quantified, outside unicode mode.
            if (!n->behind && !u_) return quantified(std::move(n), groups, groups);
            if (isQuantifierStart()) fail("nothing to repeat");
            return n;
        }
        const int before = groups;
        auto atom = this->atom();
        return quantified(std::move(atom), before, groups);
    }

    bool isQuantifierStart() {
        const char32_t c = peek();
        if (c == '*' || c == '+' || c == '?') return true;
        if (c != '{') return false;
        const size_t save = i_;
        uint32_t a, b;
        const bool ok = braces(a, b);
        i_ = save;
        return ok;
    }

    // {n}, {n,} or {n,m}, read if that's what's here.
    bool braces(uint32_t& min, uint32_t& max) {
        if (!eat('{')) return false;
        const auto number = [&](uint32_t& out) {
            if (!isDigit(peek())) return false;
            uint64_t v = 0;
            while (isDigit(peek())) {
                v = std::min<uint64_t>(v * 10 + (p_[i_] - '0'), kInf - 1);
                ++i_;
            }
            out = static_cast<uint32_t>(v);
            return true;
        };
        if (!number(min)) return false;
        max = min;
        if (eat(',')) {
            if (!number(max)) max = kInf;
        }
        return eat('}');
    }

    std::unique_ptr<Node> quantified(std::unique_ptr<Node> atom, int first_group, int end_group) {
        uint32_t min = 0, max = 0;
        const char32_t c = peek();
        if (c == '*') { ++i_; min = 0; max = kInf; }
        else if (c == '+') { ++i_; min = 1; max = kInf; }
        else if (c == '?') { ++i_; min = 0; max = 1; }
        else if (c == '{') {
            const size_t save = i_;
            if (!braces(min, max)) {
                if (u_) fail("incomplete quantifier");
                i_ = save;
                return atom;
            }
            if (min > max) fail("numbers out of order in {} quantifier");
        } else {
            return atom;
        }
        auto r = std::make_unique<Node>(Node::Repeat);
        r->min = min;
        r->max = max;
        r->greedy = !eat('?');
        r->first_group = first_group;
        r->end_group = end_group;
        r->kids.push_back(std::move(atom));
        return r;
    }

    std::unique_ptr<Node> charNode(char32_t c) {
        auto n = std::make_unique<Node>(Node::Char);
        n->c = c;
        return n;
    }

    std::unique_ptr<Node> classNode(Class k) {
        finish(k);
        auto n = std::make_unique<Node>(Node::Class);
        n->index = static_cast<int>(classes.size());
        classes.push_back(std::move(k));
        return n;
    }

    std::unique_ptr<Node> atom() {
        const char32_t c = p_[i_];
        switch (c) {
            case '.': ++i_; return std::make_unique<Node>(Node::Any);
            case '(': {
                ++i_;
                std::unique_ptr<Node> g = std::make_unique<Node>(Node::Group);
                if (eat('?')) {
                    if (eat(':')) {
                        g->index = -1;
                    } else if (eat('<')) {
                        const std::u16string name = groupName();
                        for (const auto& [n, i] : names)
                            if (n == name) fail("duplicate capture group name");
                        g->index = ++groups;
                        names.emplace_back(name, g->index);
                    } else {
                        fail("invalid group");
                    }
                } else {
                    g->index = ++groups;
                }
                g->kids.push_back(disjunction());
                if (!eat(')')) fail("unterminated group");
                return g;
            }
            case '[': ++i_; return characterClass();
            case '\\': return atomEscape();
            case '*': case '+': case '?': fail("nothing to repeat");
            case '{':
                if (u_ || isQuantifierStart()) fail(u_ ? "lone quantifier brackets" : "nothing to repeat");
                ++i_;
                return charNode(c);
            case ')': fail("unmatched ')'");
            case ']': case '}':
                if (u_) fail("lone quantifier brackets");
                ++i_;
                return charNode(c);
            default: ++i_; return charNode(c);
        }
    }

    // <name>: an identifier, \u escapes allowed (in either mode, as ES2020 has it).
    std::u16string groupName() {
        std::u16string name;
        while (more() && peek() != '>') {
            char32_t c = p_[i_++];
            if (c == '\\') {
                if (!eat('u')) fail("invalid capture group name");
                c = nameEscape();
            } else if (!u_ && isHigh(c) && more() && isLow(peek())) {
                c = 0x10000 + ((c - 0xD800) << 10) + (p_[i_++] - 0xDC00);
            }
            const bool start = c == '$' || c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0x80 && hasProp(Prop::IdStart, c));
            const bool part = start || isDigit(c) || c == 0x200C || c == 0x200D || (c >= 0x80 && hasProp(Prop::IdContinue, c));
            if (name.empty() ? !start : !part) fail("invalid capture group name");
            if (c >= 0x10000) {
                name += static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10));
                name += static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF));
            } else {
                name += static_cast<char16_t>(c);
            }
        }
        if (!eat('>') || name.empty()) fail("invalid capture group name");
        return name;
    }

    // After \u in a group name: {code point}, or four digits (a surrogate pair's two halves joined).
    char32_t nameEscape() {
        if (eat('{')) {
            uint32_t v = 0;
            int digits = 0;
            while (hexValue(peek()) >= 0) {
                v = v * 16 + static_cast<uint32_t>(hexValue(p_[i_++]));
                if (v > 0x10FFFF) fail("invalid capture group name");
                ++digits;
            }
            if (!digits || !eat('}')) fail("invalid capture group name");
            return v;
        }
        const auto four = [&](uint32_t& out) {
            out = 0;
            for (int k = 0; k < 4; ++k) {
                const int h = hexValue(peek());
                if (h < 0) return false;
                out = out * 16 + static_cast<uint32_t>(h);
                ++i_;
            }
            return true;
        };
        uint32_t v;
        if (!four(v)) fail("invalid capture group name");
        if (isHigh(v) && peek() == '\\' && peek(1) == 'u') {
            const size_t save = i_;
            i_ += 2;
            uint32_t lo;
            if (four(lo) && isLow(lo)) return 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
            i_ = save;
        }
        return v;
    }

    std::unique_ptr<Node> atomEscape() {
        ++i_;   // the backslash
        if (!more()) fail("\\ at end of pattern");
        const char32_t c = peek();
        if (c >= '1' && c <= '9') {
            const size_t save = i_;
            uint64_t n = 0;
            while (isDigit(peek())) n = std::min<uint64_t>(n * 10 + (p_[i_++] - '0'), 1u << 30);
            if (n <= static_cast<uint64_t>(total_groups_)) {
                auto b = std::make_unique<Node>(Node::BackRef);
                b->index = static_cast<int>(n);
                return b;
            }
            if (u_) fail("invalid escape");
            i_ = save;   // Annex B: an octal escape, or the digit itself
            return charNode(legacyOctalOrDigit());
        }
        if (c == 'k') {
            if (u_ || has_named_) {
                ++i_;
                if (!eat('<')) fail("invalid named reference");
                auto b = std::make_unique<Node>(Node::BackRef);
                b->name = groupName();
                return b;
            }
            ++i_;
            return charNode('k');
        }
        if (c == 'd' || c == 'D' || c == 's' || c == 'S' || c == 'w' || c == 'W') {
            ++i_;
            Class k;
            addEscapeSet(k, c);
            return classNode(std::move(k));
        }
        if ((c == 'p' || c == 'P') && u_) {
            ++i_;
            Class k;
            k.props.emplace_back(property(), c == 'P');
            return classNode(std::move(k));
        }
        return charNode(characterEscape(false));
    }

    // After a backslash, at a digit that isn't a backreference (outside unicode mode):
    // up to three octal digits worth at most 0377, or else 8 or 9 as themselves.
    char32_t legacyOctalOrDigit() {
        const char32_t c = p_[i_];
        if (c >= '8') {
            ++i_;
            return c;
        }
        uint32_t v = 0;
        for (int k = 0; k < 3 && peek() >= '0' && peek() <= '7'; ++k) {
            const uint32_t nv = v * 8 + (peek() - '0');
            if (nv > 0377) break;
            v = nv;
            ++i_;
        }
        return v;
    }

    Prop property() {
        if (!eat('{')) fail("invalid property name");
        std::string key, value;
        bool in_value = false;
        while (more() && peek() != '}') {
            const char32_t c = p_[i_++];
            if (c == '=' && !in_value) { in_value = true; continue; }
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDigit(c) || c == '_')) fail("invalid property name");
            (in_value ? value : key) += static_cast<char>(c);
        }
        if (!eat('}') || key.empty() || (in_value && value.empty())) fail("invalid property name");
        // A name we don't know matches nothing, rather than stopping the script.
        return propNamed(key, value);
    }

    // \ followed by anything that stands for one character.
    char32_t characterEscape(bool in_class) {
        const char32_t c = p_[i_];
        switch (c) {
            case 'f': ++i_; return 0x0C;
            case 'n': ++i_; return 0x0A;
            case 'r': ++i_; return 0x0D;
            case 't': ++i_; return 0x09;
            case 'v': ++i_; return 0x0B;
            case 'c': {
                const char32_t l = peek(1);
                if ((l >= 'a' && l <= 'z') || (l >= 'A' && l <= 'Z') || (in_class && !u_ && (isDigit(l) || l == '_'))) {
                    i_ += 2;
                    return l % 32;
                }
                if (u_) fail("invalid unicode escape");
                return '\\';   // Annex B: the backslash itself; the c is read next
            }
            case 'x': {
                const int a = hexValue(peek(1)), b = hexValue(peek(2));
                if (a >= 0 && b >= 0) {
                    i_ += 3;
                    return static_cast<char32_t>(a * 16 + b);
                }
                if (u_) fail("invalid escape");
                ++i_;
                return 'x';
            }
            case 'u': {
                ++i_;
                if (u_ && peek() == '{') {
                    ++i_;
                    uint32_t v = 0;
                    int digits = 0;
                    while (hexValue(peek()) >= 0) {
                        v = v * 16 + static_cast<uint32_t>(hexValue(p_[i_++]));
                        if (v > 0x10FFFF) fail("invalid unicode escape");
                        ++digits;
                    }
                    if (!digits || !eat('}')) fail("invalid unicode escape");
                    return v;
                }
                const auto four = [&](size_t at, uint32_t& out) {
                    out = 0;
                    for (size_t k = 0; k < 4; ++k) {
                        const int h = hexValue(peek(at + k));
                        if (h < 0) return false;
                        out = out * 16 + static_cast<uint32_t>(h);
                    }
                    return true;
                };
                uint32_t v;
                if (four(0, v)) {
                    i_ += 4;
                    uint32_t lo;
                    if (u_ && isHigh(v) && peek() == '\\' && peek(1) == 'u' && four(2, lo) && isLow(lo)) {
                        i_ += 6;
                        return 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    return v;
                }
                if (u_) fail("invalid unicode escape");
                return 'u';
            }
            case '0':
                if (!isDigit(peek(1))) {
                    ++i_;
                    return 0;
                }
                if (u_) fail("invalid decimal escape");
                return legacyOctalOrDigit();
            default: break;
        }
        if (isDigit(c)) {
            if (u_) fail("invalid class escape");
            return legacyOctalOrDigit();
        }
        if (in_class && c == 'b') { ++i_; return 0x08; }
        if (in_class && c == '-' && u_) { ++i_; return '-'; }
        static const std::u32string kSyntax = U"^$\\.*+?()[]{}|/";
        if (kSyntax.find(c) != std::u32string::npos) { ++i_; return c; }
        if (u_) fail("invalid escape");
        ++i_;   // Annex B: anything else stands for itself
        return c;
    }

    // One thing inside [...]: a character, or a set (\d, \p{…}) when `set` comes back true.
    char32_t classAtom(Class& set_out, bool& set) {
        set = false;
        const char32_t c = p_[i_];
        if (c != '\\') {
            ++i_;
            return c;
        }
        ++i_;
        if (!more()) fail("\\ at end of pattern");
        const char32_t e = peek();
        if (e == 'd' || e == 'D' || e == 's' || e == 'S' || e == 'w' || e == 'W') {
            ++i_;
            addEscapeSet(set_out, e);
            set = true;
            return 0;
        }
        if ((e == 'p' || e == 'P') && u_) {
            ++i_;
            set_out.props.emplace_back(property(), e == 'P');
            set = true;
            return 0;
        }
        if (e == 'B' || e == 'k') {
            if (u_) fail("invalid class escape");
            ++i_;
            return e;
        }
        return characterEscape(true);
    }

    std::unique_ptr<Node> characterClass() {
        Class k;
        k.negated = eat('^');
        while (true) {
            if (!more()) fail("unterminated character class");
            if (eat(']')) break;
            Class set_a;
            bool is_set_a;
            const char32_t a = classAtom(set_a, is_set_a);
            if (peek() == '-' && peek(1) != ']' && i_ + 1 < p_.size()) {
                ++i_;
                Class set_b;
                bool is_set_b;
                const char32_t b = classAtom(set_b, is_set_b);
                if (is_set_a || is_set_b) {
                    if (u_) fail("invalid character class");
                    // Annex B: [\w-x] is \w, a dash and x.
                    const auto add = [&](Class& s, bool is, char32_t ch) {
                        if (is) {
                            k.ranges.insert(k.ranges.end(), s.ranges.begin(), s.ranges.end());
                            k.props.insert(k.props.end(), s.props.begin(), s.props.end());
                        } else {
                            addRange(k, ch, ch);
                        }
                    };
                    add(set_a, is_set_a, a);
                    addRange(k, '-', '-');
                    add(set_b, is_set_b, b);
                    continue;
                }
                if (a > b) fail("range out of order in character class");
                addRange(k, a, b);
                continue;
            }
            if (is_set_a) {
                k.ranges.insert(k.ranges.end(), set_a.ranges.begin(), set_a.ranges.end());
                k.props.insert(k.props.end(), set_a.props.begin(), set_a.props.end());
            } else {
                addRange(k, a, a);
            }
        }
        return classNode(std::move(k));
    }

    void resolve(Node* n) {
        if (n->kind == Node::BackRef && n->index < 0) {
            for (const auto& [name, i] : names)
                if (name == n->name) n->index = i;
            if (n->index < 0) fail("invalid named capture referenced");
        }
        for (auto& k : n->kids) resolve(k.get());
    }

    Flags flags_;
    bool u_;
    std::u32string p_;
    size_t i_ = 0;
    size_t depth_ = 0;
    int total_groups_ = 0;
    bool has_named_ = false;
};

// ── Compiling ──

class Compiler {
public:
    Compiler(Program& p, bool icase, bool unicode) : p_(p), icase_(icase), unicode_(unicode) {}

    void emit(const Node* n, bool back) {
        if (p_.code.size() > kMaxProgramSize) throw ParseError{"regular expression too large"};
        switch (n->kind) {
            case Node::Empty: return;
            case Node::Char: {
                Inst in{Op::Char};
                in.c = icase_ ? canonical(n->c, unicode_) : n->c;
                in.back = back;
                p_.code.push_back(in);
                return;
            }
            case Node::Any: p_.code.push_back(Inst{Op::Any, back}); return;
            case Node::Class: {
                Inst in{Op::Class, back};
                in.x = static_cast<uint32_t>(n->index);
                p_.code.push_back(in);
                return;
            }
            case Node::Seq:
                if (back)
                    for (auto it = n->kids.rbegin(); it != n->kids.rend(); ++it) emit(it->get(), back);
                else
                    for (const auto& k : n->kids) emit(k.get(), back);
                return;
            case Node::Alt: {
                std::vector<size_t> jumps;
                for (size_t i = 0; i < n->kids.size(); ++i) {
                    if (i + 1 < n->kids.size()) {
                        const size_t split = here();
                        p_.code.push_back(Inst{Op::Split});
                        p_.code[split].x = here();
                        emit(n->kids[i].get(), back);
                        jumps.push_back(here());
                        p_.code.push_back(Inst{Op::Jmp});
                        p_.code[split].y = here();
                    } else {
                        emit(n->kids[i].get(), back);
                    }
                }
                for (size_t j : jumps) p_.code[j].x = here();
                return;
            }
            case Node::Group: {
                if (n->index < 0) {
                    emit(n->kids[0].get(), back);
                    return;
                }
                const uint32_t start = 2 * static_cast<uint32_t>(n->index), end = start + 1;
                save(back ? end : start);
                emit(n->kids[0].get(), back);
                save(back ? start : end);
                return;
            }
            case Node::Look: {
                const size_t at = here();
                Inst in{Op::Look};
                in.flag = n->neg;
                p_.code.push_back(in);
                emit(n->kids[0].get(), n->behind);
                p_.code.push_back(Inst{Op::Match});
                p_.code[at].x = here();
                return;
            }
            case Node::BackRef: {
                Inst in{Op::BackRef, back};
                in.x = static_cast<uint32_t>(n->index);
                p_.code.push_back(in);
                return;
            }
            case Node::Bol: p_.code.push_back(Inst{Op::Bol}); return;
            case Node::Eol: p_.code.push_back(Inst{Op::Eol}); return;
            case Node::WordB: p_.code.push_back(Inst{Op::WordB}); return;
            case Node::NotWordB: p_.code.push_back(Inst{Op::NotWordB}); return;
            case Node::Repeat: repeat(n, back); return;
        }
    }

private:
    size_t here() const { return p_.code.size(); }
    void save(uint32_t slot) {
        Inst in{Op::Save};
        in.x = slot;
        p_.code.push_back(in);
    }

    void repeat(const Node* n, bool back) {
        const Node* atom = n->kids[0].get();
        if (n->max == 0) return;
        if (atom->kind == Node::Char || atom->kind == Node::Any || atom->kind == Node::Class) {
            // One character at a time: a loop of its own, with no stack per step.
            Inst in{Op::Star};
            in.min = n->min;
            in.max = n->max;
            in.flag = n->greedy;
            p_.code.push_back(in);
            emit(atom, back);
            return;
        }
        const bool groups = n->end_group > n->first_group;
        const auto reset = [&] {
            if (!groups) return;
            Inst in{Op::Reset};
            in.x = static_cast<uint32_t>(n->first_group + 1);
            in.y = static_cast<uint32_t>(n->end_group + 1);
            p_.code.push_back(in);
        };
        for (uint32_t i = 0; i < n->min; ++i) {
            if (here() > kMaxProgramSize) throw ParseError{"regular expression too large"};
            reset();
            emit(atom, back);
        }
        if (n->max == kInf) {
            const size_t loop = here();
            p_.code.push_back(Inst{Op::Split});
            body(n, atom, back, reset);
            Inst j{Op::Jmp};
            j.x = static_cast<uint32_t>(loop);
            p_.code.push_back(j);
            link(loop, n->greedy, here());
            return;
        }
        std::vector<size_t> splits;
        for (uint32_t i = n->min; i < n->max; ++i) {
            if (here() > kMaxProgramSize) throw ParseError{"regular expression too large"};
            splits.push_back(here());
            p_.code.push_back(Inst{Op::Split});
            body(n, atom, back, reset);
        }
        for (size_t s : splits) link(s, n->greedy, here());
    }

    // One optional time round: no progress, no match (the spec's empty check).
    template <typename Reset>
    void body(const Node*, const Node* atom, bool back, const Reset& reset) {
        const uint32_t reg = p_.regs++;
        Inst enter{Op::LoopEnter};
        enter.x = reg;
        p_.code.push_back(enter);
        reset();
        emit(atom, back);
        Inst check{Op::LoopCheck};
        check.x = reg;
        p_.code.push_back(check);
    }

    // A split at `at` goes into the body after it and out to `exit`, greedy trying the body first.
    void link(size_t at, bool greedy, size_t exit) {
        const uint32_t in = static_cast<uint32_t>(at + 1), out = static_cast<uint32_t>(exit);
        p_.code[at].x = greedy ? in : out;
        p_.code[at].y = greedy ? out : in;
    }

    Program& p_;
    bool icase_, unicode_;
};

// ── Matching ──

class Matcher {
public:
    Matcher(Runtime& rt, const Program& p, std::u16string_view s, std::vector<int>& caps)
        : rt_(rt), p_(p), code_(p.code.data()), s_(s), len_(static_cast<int>(s.size())), caps_(caps),
          u_(p.flags.unicode || p.flags.unicode_sets), icase_(p.flags.ignore_case), multiline_(p.flags.multiline),
          dot_all_(p.flags.dot_all) {
        regs_.assign(p.regs, -1);
    }

    bool run(uint32_t pc, int pos, size_t depth) {
        const size_t base = st_.size();
        for (;;) {
            if ((++ticks_ & 255) == 0) rt_.steps(64);   // a step is about four instructions' work
            const Inst& in = code_[pc];
            switch (in.op) {
                case Op::Char:
                case Op::Any:
                case Op::Class:
                    if (single(in, pos)) { ++pc; continue; }
                    goto fail;
                case Op::Split:
                    push(kBranch, in.y, pos);
                    pc = in.x;
                    continue;
                case Op::Jmp: pc = in.x; continue;
                case Op::Save: setCap(in.x, pos); ++pc; continue;
                case Op::Reset:
                    for (uint32_t g = in.x; g < in.y; ++g) {
                        if (caps_[2 * g] != -1) setCap(2 * g, -1);
                        if (caps_[2 * g + 1] != -1) setCap(2 * g + 1, -1);
                    }
                    ++pc;
                    continue;
                case Op::Bol:
                    if (pos == 0 || (multiline_ && isLineTerm(s_[pos - 1]))) { ++pc; continue; }
                    goto fail;
                case Op::Eol:
                    if (pos == len_ || (multiline_ && isLineTerm(s_[pos]))) { ++pc; continue; }
                    goto fail;
                case Op::WordB:
                case Op::NotWordB: {
                    const bool a = pos > 0 && isWordChar(s_[pos - 1]), b = pos < len_ && isWordChar(s_[pos]);
                    if ((a != b) == (in.op == Op::WordB)) { ++pc; continue; }
                    goto fail;
                }
                case Op::BackRef: {
                    const int from = caps_[2 * in.x], to = caps_[2 * in.x + 1];
                    if (from < 0 || to < 0) { ++pc; continue; }
                    const int n = to - from;
                    const int at = in.back ? pos - n : pos;
                    if (at < 0 || at + n > len_) goto fail;
                    bool same = true;
                    for (int k = 0; k < n && same; ++k) {
                        const char32_t x = s_[from + k], y = s_[at + k];
                        same = x == y || (icase_ && canonical(x, u_) == canonical(y, u_));
                    }
                    if (!same) goto fail;
                    pos = in.back ? at : at + n;
                    ++pc;
                    continue;
                }
                case Op::Look: {
                    if (depth >= kMaxNesting) rt_.throwError("RangeError", "regular expression nested too deeply");
                    const size_t mark = st_.size();
                    const bool ok = run(pc + 1, pos, depth + 1);
                    if (ok) {
                        // Atomic: what it chose stays chosen, but what it captured can be undone.
                        size_t w = mark;
                        for (size_t r = mark; r < st_.size(); ++r)
                            if (st_[r].kind == kCap || st_[r].kind == kReg) st_[w++] = st_[r];
                        st_.resize(w);
                        if (in.flag) {
                            unwindTo(mark);
                            goto fail;
                        }
                        pc = in.x;
                        continue;
                    }
                    if (in.flag) { pc = in.x; continue; }
                    goto fail;
                }
                case Op::LoopEnter: setReg(in.x, pos); ++pc; continue;
                case Op::LoopCheck:
                    if (pos == regs_[in.x]) goto fail;
                    ++pc;
                    continue;
                case Op::Star: {
                    const Inst& atom = code_[pc + 1];
                    uint32_t count = 0;
                    while (count < in.min) {
                        if (!single(atom, pos)) goto fail;
                        ++count;
                        if ((++ticks_ & 255) == 0) rt_.steps(32);
                    }
                    if (in.flag) {
                        const int lo = pos;
                        while (count < in.max) {
                            int q = pos;
                            if (!single(atom, q)) break;
                            pos = q;
                            ++count;
                            if ((++ticks_ & 255) == 0) rt_.steps(32);
                        }
                        if (pos != lo) push(kGreedy, pc + 2, pos, lo);
                    } else if (count < in.max) {
                        push(kLazy, pc, pos, static_cast<int>(count));
                    }
                    pc += 2;
                    continue;
                }
                case Op::Match: return true;
            }
        fail:
            for (;;) {
                if (st_.size() == base) return false;
                const Frame f = st_.back();
                st_.pop_back();
                switch (f.kind) {
                    case kCap: caps_[f.a] = f.b; continue;
                    case kReg: regs_[f.a] = f.b; continue;
                    case kBranch: pc = f.a; pos = f.b; break;
                    case kGreedy: {
                        // Give one character back.
                        const bool back = code_[f.a - 1].back;
                        int q = f.b;
                        if (!back) q -= u_ && q - 2 >= f.c && isLow(s_[q - 1]) && isHigh(s_[q - 2]) ? 2 : 1;
                        else q += u_ && q + 2 <= f.c && isHigh(s_[q]) && isLow(s_[q + 1]) ? 2 : 1;
                        if (q != f.c) push(kGreedy, f.a, q, f.c);
                        pc = f.a;
                        pos = q;
                        break;
                    }
                    case kLazy: {
                        // Take one character more.
                        const Inst& star = code_[f.a];
                        int q = f.b;
                        if (!single(code_[f.a + 1], q)) continue;
                        const uint32_t count = static_cast<uint32_t>(f.c) + 1;
                        if (count < star.max) push(kLazy, f.a, q, static_cast<int>(count));
                        pc = f.a + 2;
                        pos = q;
                        break;
                    }
                }
                break;
            }
        }
    }

    void reset() { st_.clear(); }

private:
    enum : uint8_t { kBranch, kCap, kReg, kGreedy, kLazy };
    struct Frame {
        uint8_t kind;
        uint32_t a;
        int32_t b;
        int32_t c;
    };

    void push(uint8_t kind, uint32_t a, int b, int c = 0) {
        if (st_.size() >= kMaxBacktrack) rt_.throwError("RangeError", "regular expression too complex for this text");
        st_.push_back(Frame{kind, a, b, c});
    }
    void setCap(uint32_t slot, int v) {
        push(kCap, slot, caps_[slot]);
        caps_[slot] = v;
    }
    void setReg(uint32_t r, int v) {
        push(kReg, r, regs_[r]);
        regs_[r] = v;
    }
    void unwindTo(size_t mark) {
        while (st_.size() > mark) {
            const Frame f = st_.back();
            st_.pop_back();
            if (f.kind == kCap) caps_[f.a] = f.b;
            else if (f.kind == kReg) regs_[f.a] = f.b;
        }
    }

    // One character read in the instruction's direction; whether the instruction takes it.
    bool single(const Inst& in, int& pos) const {
        char32_t c;
        if (!in.back) {
            if (pos >= len_) return false;
            c = s_[pos];
            if (u_ && isHigh(c) && pos + 1 < len_ && isLow(s_[pos + 1])) {
                c = 0x10000 + ((c - 0xD800) << 10) + (s_[pos + 1] - 0xDC00);
                if (!test(in, c)) return false;
                pos += 2;
                return true;
            }
            if (!test(in, c)) return false;
            ++pos;
            return true;
        }
        if (pos <= 0) return false;
        c = s_[pos - 1];
        if (u_ && isLow(c) && pos >= 2 && isHigh(s_[pos - 2])) {
            c = 0x10000 + ((s_[pos - 2] - 0xD800) << 10) + (c - 0xDC00);
            if (!test(in, c)) return false;
            pos -= 2;
            return true;
        }
        if (!test(in, c)) return false;
        --pos;
        return true;
    }

    bool test(const Inst& in, char32_t c) const {
        switch (in.op) {
            case Op::Char: return c == in.c || (icase_ && canonical(c, u_) == in.c);
            case Op::Any: return dot_all_ || !isLineTerm(c);
            case Op::Class: {
                const Class& k = p_.classes[in.x];
                bool hit = inClass(k, c);
                if (!hit && icase_) {
                    const char32_t l = lowerOf(c), up = upperOf(c), f = lowerOf(up);
                    for (const char32_t cand : {l, up, f}) {
                        if (cand == c || (!u_ && c >= 0x80 && cand < 0x80)) continue;
                        if (inClass(k, cand)) { hit = true; break; }
                    }
                }
                return hit != k.negated;
            }
            default: return false;
        }
    }

    Runtime& rt_;
    const Program& p_;
    const Inst* code_;
    std::u16string_view s_;
    int len_;
    std::vector<int>& caps_;
    std::vector<int> regs_;
    std::vector<Frame> st_;
    uint32_t ticks_ = 0;
    bool u_, icase_, multiline_, dot_all_;
};

}  // namespace

std::optional<Flags> parseFlags(std::u16string_view s) {
    Flags f;
    for (const char16_t c : s) {
        bool* b = nullptr;
        switch (c) {
            case u'd': b = &f.has_indices; break;
            case u'g': b = &f.global; break;
            case u'i': b = &f.ignore_case; break;
            case u'm': b = &f.multiline; break;
            case u's': b = &f.dot_all; break;
            case u'u': b = &f.unicode; break;
            case u'v': b = &f.unicode_sets; break;
            case u'y': b = &f.sticky; break;
            default: return std::nullopt;
        }
        if (*b) return std::nullopt;
        *b = true;
    }
    if (f.unicode && f.unicode_sets) return std::nullopt;
    return f;
}

std::u16string flagsString(const Flags& f) {
    std::u16string s;
    if (f.has_indices) s += u'd';
    if (f.global) s += u'g';
    if (f.ignore_case) s += u'i';
    if (f.multiline) s += u'm';
    if (f.dot_all) s += u's';
    if (f.unicode) s += u'u';
    if (f.unicode_sets) s += u'v';
    if (f.sticky) s += u'y';
    return s;
}

std::shared_ptr<const Program> compile(std::u16string_view pattern, const Flags& flags, std::string& error) {
    if (pattern.size() > kMaxPatternLength) {
        error = "regular expression too large";
        return nullptr;
    }
    try {
        Parser parser(pattern, flags);
        const std::unique_ptr<Node> tree = parser.parse();
        auto p = std::make_shared<Program>();
        p->flags = flags;
        p->groups = static_cast<size_t>(parser.groups);
        p->names = std::move(parser.names);
        p->classes = std::move(parser.classes);
        Compiler c(*p, flags.ignore_case, flags.unicode || flags.unicode_sets);
        p->code.push_back(Inst{Op::Save, false, false, 0});
        c.emit(tree.get(), false);
        p->code.push_back(Inst{Op::Save, false, false, 1});
        p->code.push_back(Inst{Op::Match});
        // What every match must start with, when that's plain.
        size_t first = 1;
        while (first < p->code.size() && p->code[first].op == Op::Save) ++first;
        const Inst& f = p->code[first];
        if (f.op == Op::Char && !flags.ignore_case && f.c < 0xD800) p->first_char = static_cast<int>(f.c);
        if (f.op == Op::Bol && !flags.multiline) p->anchored = true;
        return p;
    } catch (const ParseError& e) {
        error = e.what;
        return nullptr;
    }
}

size_t groupCount(const Program& p) { return p.groups; }
const std::vector<std::pair<std::u16string, int>>& groupNames(const Program& p) { return p.names; }
const Flags& flagsOf(const Program& p) { return p.flags; }

bool exec(Runtime& rt, const Program& p, std::u16string_view input, size_t start, bool sticky, std::vector<int>& caps) {
    caps.assign(2 * (p.groups + 1), -1);
    if (start > input.size() || input.size() > static_cast<size_t>(INT32_MAX) - 2) return false;
    Matcher m(rt, p, input, caps);
    const bool unicode = p.flags.unicode || p.flags.unicode_sets;
    size_t pos = start;
    while (pos <= input.size()) {
        if (!sticky && p.anchored && pos > 0) return false;
        if (!sticky && p.first_char >= 0) {
            const size_t at = input.find(static_cast<char16_t>(p.first_char), pos);
            if (at == std::u16string_view::npos) return false;
            pos = at;
        }
        m.reset();
        if (m.run(0, static_cast<int>(pos), 0)) return true;
        if (sticky) return false;
        pos += unicode && pos + 1 < input.size() && isHigh(input[pos]) && isLow(input[pos + 1]) ? 2 : 1;
    }
    return false;
}

}  // namespace fbx::xweb::js::regex
