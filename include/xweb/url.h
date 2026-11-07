#pragma once

// URLs as pages use them: RFC 3986 parsing and relative resolution.

#include <string>
#include <string_view>

namespace fbx::xweb {

struct Url {
    std::string scheme;     // lower case; empty for a relative reference
    std::string authority;  // host[:port], lower case
    bool has_authority = false;
    std::string path;
    std::string query;      // without the '?'
    bool has_query = false;
    std::string fragment;   // without the '#'
    bool has_fragment = false;

    [[nodiscard]] bool isAbsolute() const noexcept { return !scheme.empty(); }
    [[nodiscard]] std::string toString() const;
    // Without its fragment: what you'd actually fetch.
    [[nodiscard]] std::string withoutFragment() const;
};

[[nodiscard]] Url parseUrl(std::string_view text);

// `reference` as seen from `base`. A base may itself be relative (a page opened by a
// relative path): the result then is too.
[[nodiscard]] std::string resolveUrl(std::string_view base, std::string_view reference);

// What a data: URL holds. False if it isn't one, or is damaged.
[[nodiscard]] bool decodeDataUrl(std::string_view url, std::string& body, std::string& content_type);

// Whether two addresses are the same site, as cookies count it: the same scheme family
// and registrable domain ("www.example.co.kr" and "img.example.co.kr" are). Without the
// full public suffix list, a two-part country suffix (co.uk, co.kr, com.au...) is known
// by its shape; xfetch's curl checks the real list before keeping any cookie.
[[nodiscard]] bool sameSite(const std::string& a, const std::string& b);
[[nodiscard]] std::string registrableDomain(const std::string& host);

}  // namespace fbx::xweb
