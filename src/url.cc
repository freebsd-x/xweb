#include "xweb/url.h"

#include <algorithm>
#include <cstdint>

#include <algorithm>
#include <vector>

namespace fbx::xweb {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
    return s;
}

// Whitespace pasted around or inside a URL isn't part of it.
std::string cleaned(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in)
        if (c != '\t' && c != '\n' && c != '\r') out += c;
    const size_t begin = out.find_first_not_of(' ');
    if (begin == std::string::npos) return {};
    const size_t end = out.find_last_not_of(' ');
    return out.substr(begin, end - begin + 1);
}

// RFC 3986 5.2.4
std::string removeDotSegments(std::string_view path) {
    const bool absolute = path.starts_with('/');
    std::vector<std::string_view> out;
    bool trailing = false;
    size_t i = absolute ? 1 : 0;
    while (i <= path.size()) {
        const size_t slash = path.find('/', i);
        const std::string_view seg = path.substr(i, slash == std::string_view::npos ? std::string_view::npos : slash - i);
        const bool last = slash == std::string_view::npos;
        if (seg == "..") {
            if (!out.empty() && out.back() != "..") out.pop_back();
            else if (!absolute) out.push_back(seg);
            trailing = last;
        } else if (seg == ".") {
            trailing = last;
        } else {
            out.push_back(seg);
            trailing = false;
        }
        if (last) break;
        i = slash + 1;
    }
    std::string result = absolute ? "/" : "";
    for (size_t k = 0; k < out.size(); ++k) {
        if (k) result += '/';
        result += out[k];
    }
    if (trailing && !result.empty() && result.back() != '/') result += '/';
    return result;
}

}  // namespace

std::string Url::withoutFragment() const {
    std::string out;
    if (!scheme.empty()) out += scheme + ":";
    if (has_authority) out += "//" + authority;
    out += path;
    if (has_query) out += "?" + query;
    return out;
}

std::string Url::toString() const {
    std::string out = withoutFragment();
    if (has_fragment) out += "#" + fragment;
    return out;
}

Url parseUrl(std::string_view text) {
    const std::string s = cleaned(text);
    std::string_view rest = s;
    Url u;
    if (const size_t hash = rest.find('#'); hash != std::string_view::npos) {
        u.fragment = std::string(rest.substr(hash + 1));
        u.has_fragment = true;
        rest = rest.substr(0, hash);
    }
    if (const size_t q = rest.find('?'); q != std::string_view::npos) {
        u.query = std::string(rest.substr(q + 1));
        u.has_query = true;
        rest = rest.substr(0, q);
    }
    // A scheme: letters first, and a ':' before any '/'.
    const size_t colon = rest.find(':');
    if (colon != std::string_view::npos && colon > 0 &&
        ((rest[0] >= 'a' && rest[0] <= 'z') || (rest[0] >= 'A' && rest[0] <= 'Z')) &&
        std::all_of(rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(colon), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        })) {
        u.scheme = lower(std::string(rest.substr(0, colon)));
        rest = rest.substr(colon + 1);
    }
    if (rest.starts_with("//")) {
        const size_t end = rest.find('/', 2);
        u.authority = lower(std::string(rest.substr(2, end == std::string_view::npos ? std::string_view::npos : end - 2)));
        u.has_authority = true;
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
    }
    u.path = std::string(rest);
    return u;
}

std::string resolveUrl(std::string_view base_text, std::string_view reference_text) {
    const Url ref = parseUrl(reference_text);
    if (ref.isAbsolute()) {
        Url out = ref;
        if (out.has_authority || out.path.starts_with('/')) out.path = removeDotSegments(out.path);
        if (out.has_authority && out.path.empty() && (out.scheme == "http" || out.scheme == "https")) out.path = "/";
        return out.toString();
    }
    const Url base = parseUrl(base_text);
    Url out;
    out.scheme = base.scheme;
    if (ref.has_authority) {
        out.authority = ref.authority;
        out.has_authority = true;
        out.path = removeDotSegments(ref.path);
        out.query = ref.query;
        out.has_query = ref.has_query;
    } else {
        out.authority = base.authority;
        out.has_authority = base.has_authority;
        if (ref.path.empty()) {
            out.path = base.path;
            out.query = ref.has_query ? ref.query : base.query;
            out.has_query = ref.has_query || base.has_query;
        } else {
            if (ref.path.starts_with('/')) {
                out.path = removeDotSegments(ref.path);
            } else {
                const size_t slash = base.path.rfind('/');
                std::string merged = slash == std::string::npos ? (base.has_authority ? "/" : "") : base.path.substr(0, slash + 1);
                merged += ref.path;
                out.path = removeDotSegments(merged);
            }
            out.query = ref.query;
            out.has_query = ref.has_query;
        }
    }
    out.fragment = ref.fragment;
    out.has_fragment = ref.has_fragment;
    return out.toString();
}

bool decodeDataUrl(std::string_view url, std::string& body, std::string& content_type) {
    if (url.size() < 5 || !std::equal(url.begin(), url.begin() + 5, "data:", [](char a, char b) { return (a >= 'A' && a <= 'Z' ? a + 32 : a) == b; })) return false;
    url.remove_prefix(5);
    const size_t comma = url.find(',');
    if (comma == std::string_view::npos) return false;
    std::string_view meta = url.substr(0, comma);
    std::string_view data = url.substr(comma + 1);
    if (const size_t hash = data.find('#'); hash != std::string_view::npos) data = data.substr(0, hash);
    bool base64 = false;
    if (meta.size() >= 7) {
        std::string tail(meta.substr(meta.size() - 7));
        std::transform(tail.begin(), tail.end(), tail.begin(), [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
        if (tail == ";base64") {
            base64 = true;
            meta.remove_suffix(7);
        }
    }
    content_type = meta.empty() ? "text/plain" : std::string(meta);
    std::transform(content_type.begin(), content_type.end(), content_type.begin(), [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
    const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1; };
    // Percent escapes come first even in base64, where some sites escape the padding.
    std::string text;
    text.reserve(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '%' && i + 2 < data.size() && hex(data[i + 1]) >= 0 && hex(data[i + 2]) >= 0) {
            text += static_cast<char>(hex(data[i + 1]) * 16 + hex(data[i + 2]));
            i += 2;
        } else {
            text += data[i];
        }
    }
    body.clear();
    if (!base64) {
        body = std::move(text);
        return true;
    }
    body.reserve(text.size() / 4 * 3);
    uint32_t bits = 0;
    int have = 0;
    for (const char c : text) {
        int v = -1;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        else return false;
        bits = (bits << 6) | static_cast<uint32_t>(v);
        have += 6;
        if (have >= 8) {
            have -= 8;
            body += static_cast<char>((bits >> have) & 0xFF);
        }
    }
    return true;
}

}  // namespace fbx::xweb
