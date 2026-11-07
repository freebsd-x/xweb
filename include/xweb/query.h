#pragma once

// Selectors outside the cascade: what querySelector, matches() and closest() use.

#include <optional>
#include <string_view>
#include <vector>

#include "xweb/css.h"
#include "xweb/dom.h"

namespace fbx::xweb {

// Nullopt if the text isn't a valid selector list (the DOM then throws SyntaxError).
[[nodiscard]] std::optional<std::vector<css::ComplexSelector>> parseSelectors(std::string_view text);
[[nodiscard]] bool matchesSelectors(const Node* element, const std::vector<css::ComplexSelector>& selectors);
// Elements under root (not root itself) that match, in document order; `first` stops at one.
[[nodiscard]] std::vector<Node*> querySelectorAll(Node* root, const std::vector<css::ComplexSelector>& selectors, bool first = false);

}  // namespace fbx::xweb
