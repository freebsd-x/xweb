#include "xweb/url.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

#include <algorithm>
#include <cctype>
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

std::string registrableDomain(const std::string& host_in) {
    std::string host;
    for (char c : host_in) host += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!host.empty() && host.back() == '.') host.pop_back();
    // An address, or a name with no dot: itself.
    if (host.empty() || host.front() == '[' || host.find('.') == std::string::npos ||
        std::all_of(host.begin(), host.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; }))
        return host;
    std::vector<std::string> labels;
    size_t start = 0;
    while (true) {
        const size_t dot = host.find('.', start);
        labels.push_back(host.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    const size_t n = labels.size();
    size_t keep = 2;
    static const char* kSecondLevel[] = {"co", "com", "net", "org", "gov", "ac", "or", "go", "ne", "edu", "gob", "mil", "ltd", "plc", "sch", "nhs"};
    if (n >= 3 && labels[n - 1].size() == 2 &&
        std::any_of(std::begin(kSecondLevel), std::end(kSecondLevel), [&](const char* l) { return labels[n - 2] == l; }))
        keep = 3;
    // Hosts where each name is someone else's site.
    static const char* kShared[] = {"github.io", "gitlab.io", "pages.dev", "netlify.app", "vercel.app", "herokuapp.com", "blogspot.com", "appspot.com", "web.app", "firebaseapp.com", "azurewebsites.net", "cloudfront.net"};
    const std::string last_two = labels[n - 2] + "." + labels[n - 1];
    if (n >= 3 && std::any_of(std::begin(kShared), std::end(kShared), [&](const char* l) { return last_two == l; })) keep = 3;
    if (n <= keep) return host;
    std::string out;
    for (size_t k = n - keep; k < n; ++k) out += (out.empty() ? "" : ".") + labels[k];
    return out;
}

bool sameSite(const std::string& a, const std::string& b) {
    const Url ua = parseUrl(a), ub = parseUrl(b);
    const auto web = [](const std::string& s) { return s == "http" || s == "https"; };
    if (!web(ua.scheme) || !web(ub.scheme)) return false;
    const auto host = [](const Url& u) {
        std::string h = u.authority;
        if (const size_t at = h.rfind('@'); at != std::string::npos) h = h.substr(at + 1);
        if (!h.empty() && h.front() == '[') return h.substr(0, h.find(']') + 1);
        return h.substr(0, h.find(':'));
    };
    const std::string ha = host(ua), hb = host(ub);
    return !ha.empty() && registrableDomain(ha) == registrableDomain(hb);
}

}  // namespace fbx::xweb
