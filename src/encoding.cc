#include "xweb/encoding.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <unordered_map>

namespace fbx::xweb {

namespace {

std::string lower(std::string_view in) {
    std::string s(in);
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
    return s;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// What the web means by latin1: windows-1252, for the range ISO-8859-1 leaves to controls.
constexpr uint16_t kCp1252High[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                      0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013,
                                      0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};

std::string fromCp1252(std::string_view in) {
    std::string out;
    out.reserve(in.size() + in.size() / 8);
    for (const char ch : in) {
        const auto c = static_cast<unsigned char>(ch);
        appendUtf8(out, c >= 0x80 && c <= 0x9F ? kCp1252High[c - 0x80] : c);
    }
    return out;
}

std::string fromUtf16(std::string_view in, bool big_endian) {
    std::string out;
    out.reserve(in.size());
    const auto unit = [&](size_t i) {
        const auto a = static_cast<unsigned char>(in[i]), b = static_cast<unsigned char>(in[i + 1]);
        return static_cast<uint32_t>(big_endian ? (a << 8) | b : (b << 8) | a);
    };
    for (size_t i = 0; i + 1 < in.size(); i += 2) {
        uint32_t cp = unit(i);
        if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < in.size()) {
            const uint32_t low = unit(i + 2);
            if (low >= 0xDC00 && low < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xD800 && cp < 0xE000) {
            cp = 0xFFFD;
        }
        appendUtf8(out, cp);
    }
    return out;
}

#include "euc_kr_index.inc"

}  // namespace

bool isEucKrLabel(std::string_view label) {
    static constexpr std::string_view kLabels[] = {"euc-kr", "cseuckr", "csksc56011987", "iso-ir-149", "korean", "ks_c_5601-1987",
                                                   "ks_c_5601-1989", "ksc5601", "ksc_5601", "windows-949", "cp949", "uhc"};
    const std::string l = lower(label);
    return std::find(std::begin(kLabels), std::end(kLabels), l) != std::end(kLabels);
}

// The Encoding Standard's EUC-KR decoder: a lead 0x81-0xFE and a trail 0x41-0xFE make a
// pointer into the index; anything else is U+FFFD (and an ASCII trail is read again).
std::string eucKrToUtf8(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size() * 3 / 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
        const auto b = static_cast<uint8_t>(bytes[i]);
        if (b < 0x80) {
            out += static_cast<char>(b);
            continue;
        }
        if (b < 0x81 || b > 0xFE || i + 1 >= bytes.size()) {
            appendUtf8(out, 0xFFFD);
            continue;
        }
        const auto t = static_cast<uint8_t>(bytes[i + 1]);
        uint16_t cp = 0;
        if (t >= 0x41 && t <= 0xFE) cp = kEucKrIndex[(b - 0x81) * 190 + (t - 0x41)];
        if (cp) {
            appendUtf8(out, cp);
            ++i;
        } else {
            appendUtf8(out, 0xFFFD);
            if (t >= 0x80) ++i;   // an ASCII trail is a character of its own
        }
    }
    return out;
}

// The other way, for forms on an EUC-KR page: what the index has, as its two bytes; the
// rest as &#n; (the HTML form encoding's way). Expects UTF-8.
std::string utf8ToEucKr(std::string_view text) {
    static const auto* const reverse = [] {
        auto* m = new std::unordered_map<uint16_t, uint16_t>();
        m->reserve(17100);
        for (uint16_t p = 0; p < std::size(kEucKrIndex); ++p)
            if (kEucKrIndex[p] && !m->contains(kEucKrIndex[p])) (*m)[kEucKrIndex[p]] = p;
        return m;
    }();
    std::string out;
    for (size_t i = 0; i < text.size();) {
        const auto b = static_cast<uint8_t>(text[i]);
        uint32_t cp = b;
        size_t len = 1;
        if (b >= 0xF0 && i + 3 < text.size()) { cp = ((b & 7u) << 18) | ((text[i + 1] & 0x3Fu) << 12) | ((text[i + 2] & 0x3Fu) << 6) | (text[i + 3] & 0x3Fu); len = 4; }
        else if (b >= 0xE0 && i + 2 < text.size()) { cp = ((b & 15u) << 12) | ((text[i + 1] & 0x3Fu) << 6) | (text[i + 2] & 0x3Fu); len = 3; }
        else if (b >= 0xC0 && i + 1 < text.size()) { cp = ((b & 31u) << 6) | (text[i + 1] & 0x3Fu); len = 2; }
        i += len;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
            continue;
        }
        if (cp < 0x10000)
            if (const auto it = reverse->find(static_cast<uint16_t>(cp)); it != reverse->end()) {
                out += static_cast<char>(it->second / 190 + 0x81);
                out += static_cast<char>(it->second % 190 + 0x41);
                continue;
            }
        out += "&#" + std::to_string(cp) + ";";
    }
    return out;
}

namespace {

std::string charsetIn(std::string_view text) {
    std::string s = lower(text);
    const size_t at = s.find("charset");
    if (at == std::string::npos) return {};
    size_t i = at + 7;
    while (i < s.size() && (s[i] == ' ' || s[i] == '=')) ++i;
    if (i < s.size() && (s[i] == '"' || s[i] == '\'')) ++i;
    const size_t b = i;
    while (i < s.size() && s[i] != '"' && s[i] != '\'' && s[i] != ';' && s[i] != ' ' && s[i] != '>' && s[i] != '/') ++i;
    return s.substr(b, i - b);
}

}  // namespace

std::string detectCharset(std::string_view bytes, std::string_view content_type) {
    if (bytes.starts_with("\xEF\xBB\xBF")) return "utf-8";
    if (bytes.starts_with("\xFF\xFE")) return "utf-16le";
    if (bytes.starts_with("\xFE\xFF")) return "utf-16be";
    std::string charset = charsetIn(content_type);
    if (charset.empty()) charset = charsetIn(bytes.substr(0, 1024));
    return charset;
}

std::string decodeToUtf8(std::string_view bytes, std::string_view content_type) {
    if (bytes.starts_with("\xEF\xBB\xBF")) return std::string(bytes.substr(3));
    if (bytes.starts_with("\xFF\xFE")) return fromUtf16(bytes.substr(2), false);
    if (bytes.starts_with("\xFE\xFF")) return fromUtf16(bytes.substr(2), true);
    std::string charset = charsetIn(content_type);
    if (charset.empty()) charset = charsetIn(bytes.substr(0, 1024));
    if (charset == "utf-16le" || charset == "utf-16") return fromUtf16(bytes, false);
    if (charset == "utf-16be") return fromUtf16(bytes, true);
    if (charset == "iso-8859-1" || charset == "latin1" || charset == "latin-1" || charset == "windows-1252" ||
        charset == "cp1252" || charset == "us-ascii" || charset == "ascii" || charset == "iso8859-1")
        return fromCp1252(bytes);
    if (isEucKrLabel(charset)) return eucKrToUtf8(bytes);
    return std::string(bytes);
}

}  // namespace fbx::xweb
