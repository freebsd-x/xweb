#pragma once

// Our HTML parser: the WHATWG tokenizer and tree construction, written by hand. It
// accepts anything; a web page can't make it fail, only produce an odd tree.

#include <memory>
#include <string_view>

#include "xweb/dom.h"

namespace fbx::xweb {

// Deeper elements become siblings rather than nest further, as in other browsers.
inline constexpr size_t kMaxOpenElements = 512;

struct ParseOptions {
    // With scripting on (the page's scripts run), <noscript> is raw text, and hidden.
    bool scripting = false;
};

// `html` is UTF-8; invalid bytes become U+FFFD.
[[nodiscard]] std::unique_ptr<Document> parseHtml(std::string_view html, ParseOptions options = {});

// innerHTML's parsing: `html` as if inside a `context` element. The result is the
// children of documentElement().
[[nodiscard]] std::unique_ptr<Document> parseFragment(std::string_view html, std::string_view context,
                                                      Namespace context_ns = Namespace::Html, ParseOptions options = {});

}  // namespace fbx::xweb
