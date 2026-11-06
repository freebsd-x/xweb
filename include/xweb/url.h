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

}  // namespace fbx::xweb
