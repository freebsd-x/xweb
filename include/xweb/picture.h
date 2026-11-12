#pragma once

// Which picture an <img> shows: its <picture>'s first <source> that fits (a type we can
// decode, a media query that holds), else its own srcset, else its src. From a srcset,
// the smallest candidate that's sharp enough (for "1x 2x", one at least 1x; for widths,
// one at least as wide as the viewport, which is what sizes defaults to).

#include <optional>
#include <string>

#include "xweb/css.h"
#include "xweb/dom.h"

namespace fbx::xweb {

// The URL as written in the page (not resolved), or nothing if it has none.
[[nodiscard]] std::optional<std::string> imageSourceOf(const Node* img, const css::MediaContext& viewport);

}  // namespace fbx::xweb
