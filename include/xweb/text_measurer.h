#pragma once

// Measures text with the same fonts and shaping that xrender draws it with.

#include <string>
#include <string_view>
#include <unordered_map>

#include "xweb/layout.h"

namespace fbx::xweb {

// A CSS family name as the font manager wants it (empty is the sans-serif default).
[[nodiscard]] std::string fontFamilyFor(const std::string& css_family);

class XrenderTextMeasurer : public TextMeasurer {
public:
    [[nodiscard]] float width(std::string_view utf8, const FontSpec& font) override;
    [[nodiscard]] float ascent(const FontSpec& font) override;
    [[nodiscard]] float descent(const FontSpec& font) override;
    [[nodiscard]] float lineGap(const FontSpec& font) override;

private:
    std::unordered_map<std::string, float> cache_;   // pages repeat their words
};

}  // namespace fbx::xweb
