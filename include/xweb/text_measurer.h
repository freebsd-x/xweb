#pragma once

// Measures text with the same fonts and shaping that xrender draws it with.

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xweb/layout.h"
#include "xweb/style.h"

namespace fbx::xweb {

// A CSS family name as the font manager wants it (empty is the sans-serif default).
[[nodiscard]] std::string fontFamilyFor(const std::string& css_family);

// ── Web fonts ─────────────────────────────────────────────────────────────────
//
// The current page's @font-face rules and what has arrived for them. One thread's
// business, the one laying pages out; only webFontUsable may be called from another.

// Replaces the faces in force. Bytes that have arrived stay, for the next page using them.
void setPageWebFonts(std::vector<WebFontFace> faces);
// The faces this page's text really calls for: only those get downloaded.
[[nodiscard]] std::vector<WebFontFace> wantedWebFonts(const Document& doc, const StyledDocument& styles);
// Whether these bytes open as a font at all.
[[nodiscard]] bool webFontUsable(const std::shared_ptr<const std::string>& bytes);
[[nodiscard]] bool haveWebFontData(const std::string& url);
// False if we're holding as many fonts as we're willing to.
bool addWebFontData(const std::string& url, std::shared_ptr<const std::string> bytes);

// A stretch of text, the font for it, and that font as the font manager names it.
struct FontRun {
    size_t begin = 0, end = 0;
    std::string family;
};

// Text falls into runs when a face lacks some of its characters or hasn't arrived yet.
[[nodiscard]] std::vector<FontRun> fontRuns(std::string_view utf8, const FontSpec& font);

class XrenderTextMeasurer : public TextMeasurer {
public:
    [[nodiscard]] float width(std::string_view utf8, const FontSpec& font) override;
    [[nodiscard]] float ascent(const FontSpec& font) override;
    [[nodiscard]] float descent(const FontSpec& font) override;
    [[nodiscard]] float lineGap(const FontSpec& font) override;

private:
    [[nodiscard]] float runWidth(std::string_view utf8, const std::string& family, const FontSpec& font);
    std::unordered_map<std::string, float> cache_;   // pages repeat their words
    uint64_t fonts_generation_ = 0;
};

}  // namespace fbx::xweb
