#include "xweb/text_measurer.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cctype>

#include "xrender/font_manager.h"

namespace fbx::xweb {

using fbx::xrender::FontManager;

namespace {

fbx::xrender::FontWeight weightOf(int w) {
    return static_cast<fbx::xrender::FontWeight>(std::clamp((w + 50) / 100 * 100, 100, 900));
}

fbx::xrender::FontStyle styleOf(bool italic) {
    return italic ? fbx::xrender::FontStyle::Italic : fbx::xrender::FontStyle::Normal;
}

FT_Face faceFor(const FontSpec& f) {
    auto* face = FontManager::getFace(fontFamilyFor(f.family), weightOf(f.weight), styleOf(f.italic));
    return face ? face->ft_face_ : nullptr;
}

float scaled(FT_Short units, const FT_Face face, float size) {
    return static_cast<float>(units) * size / static_cast<float>(face->units_per_EM ? face->units_per_EM : 1000);
}

}  // namespace

std::string fontFamilyFor(const std::string& css) {
    std::string lower = css;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.empty() || lower == "sans-serif" || lower == "system-ui" || lower == "ui-sans-serif" || lower == "-apple-system")
        return {};
    if (lower == "ui-monospace") return "monospace";
    if (lower == "ui-serif") return "serif";
    return css;
}

float XrenderTextMeasurer::width(std::string_view utf8, const FontSpec& font) {
    std::string key;
    key.reserve(utf8.size() + font.family.size() + 16);
    key += font.family;
    key += '\x01';
    key += std::to_string(static_cast<int>(font.size * 64));
    key += '\x01';
    key += std::to_string(font.weight);
    key += font.italic ? 'i' : 'n';
    key.append(utf8);
    const auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    const float w = FontManager::measureText(std::string(utf8), font.size, fontFamilyFor(font.family), weightOf(font.weight),
                                             styleOf(font.italic));
    cache_.emplace(std::move(key), w);
    return w;
}

float XrenderTextMeasurer::ascent(const FontSpec& font) {
    const FT_Face face = faceFor(font);
    return face ? scaled(face->ascender, face, font.size) : font.size * 0.8f;
}

float XrenderTextMeasurer::descent(const FontSpec& font) {
    const FT_Face face = faceFor(font);
    return face ? -scaled(face->descender, face, font.size) : font.size * 0.2f;
}

float XrenderTextMeasurer::lineGap(const FontSpec& font) {
    const FT_Face face = faceFor(font);
    return face ? std::max(0.0f, scaled(static_cast<FT_Short>(face->height - face->ascender + face->descender), face, font.size)) : 0.0f;
}

}  // namespace fbx::xweb
