#include "xweb/encoding.h"

#include <algorithm>
#include <cstdint>

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
    return std::string(bytes);
}

}  // namespace fbx::xweb
