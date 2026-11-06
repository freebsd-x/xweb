#include "xweb/text_measurer.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

#include <fontconfig/fontconfig.h>

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

// The first family in the list that's really installed. Fontconfig always answers
// with something, so a name counts only if the font it picks goes by that name.
std::string fontFamilyFor(const std::string& css_list) {
    static std::unordered_map<std::string, std::string> cache;
    const auto cached = cache.find(css_list);
    if (cached != cache.end()) return cached->second;

    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const auto installed = [&](const std::string& name) {
        FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(""));
        if (!pattern) return false;
        FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(name.c_str()));
        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result = FcResultNoMatch;
        FcPattern* match = FcFontMatch(nullptr, pattern, &result);
        bool found = false;
        if (match) {
            // Bitmap fonts don't count: they exist at a few sizes only.
            FcBool scalable = FcFalse;
            FcPatternGetBool(match, FC_SCALABLE, 0, &scalable);
            FcChar8* family = nullptr;
            for (int i = 0; scalable && !found && FcPatternGetString(match, FC_FAMILY, i, &family) == FcResultMatch; ++i)
                found = lower(reinterpret_cast<const char*>(family)) == lower(name);
            FcPatternDestroy(match);
        }
        FcPatternDestroy(pattern);
        return found;
    };

    std::string chosen = "sans-serif";   // what a page naming nothing we have gets
    size_t start = 0;
    while (start <= css_list.size()) {
        const size_t comma = css_list.find(',', start);
        const std::string name = css_list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const std::string key = lower(name);
        if (key == "serif" || key == "sans-serif" || key == "monospace" || key == "cursive" || key == "fantasy") {
            chosen = key;
            break;
        }
        if (key == "system-ui" || key == "ui-sans-serif") {
            chosen = "sans-serif";
            break;
        }
        if (key == "ui-monospace") { chosen = "monospace"; break; }
        if (key == "ui-serif") { chosen = "serif"; break; }
        if (!name.empty() && installed(name)) {
            chosen = name;
            break;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    cache.emplace(css_list, chosen);
    return chosen;
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

// Whole pixels, as browsers use them: pages' line heights come out the same, and
// otherwise the fractions add up down a long document.
float XrenderTextMeasurer::ascent(const FontSpec& font) {
    const FT_Face face = faceFor(font);
    return std::round(face ? scaled(face->ascender, face, font.size) : font.size * 0.8f);
}

float XrenderTextMeasurer::descent(const FontSpec& font) {
    const FT_Face face = faceFor(font);
    return std::round(face ? -scaled(face->descender, face, font.size) : font.size * 0.2f);
}

float XrenderTextMeasurer::lineGap(const FontSpec& font) {
    const FT_Face face = faceFor(font);
    return face ? std::round(std::max(0.0f, scaled(static_cast<FT_Short>(face->height - face->ascender + face->descender), face, font.size))) : 0.0f;
}

}  // namespace fbx::xweb
