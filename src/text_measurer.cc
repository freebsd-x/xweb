#include "xweb/text_measurer.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

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

// ── Web fonts ─────────────────────────────────────────────────────────────────

constexpr uint64_t kMaxFontBytes = 256ull * 1024 * 1024;
constexpr size_t kMaxWantedFaces = 32;

struct FontData {
    uint32_t id = 0;
    std::shared_ptr<const std::string> bytes;
};

std::vector<WebFontFace> g_faces;
std::map<std::string, std::vector<size_t>> g_families;   // lower-case name to indexes in g_faces
std::unordered_map<std::string, FontData> g_font_data;   // by URL; across pages
uint64_t g_font_bytes = 0;
uint32_t g_next_font_data = 1;
uint64_t g_fonts_generation = 1;                         // moves when what text looks like may have
std::unordered_map<std::string, std::string> g_run_cache;

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

uint32_t nextCodePoint(std::string_view s, size_t& i) {
    const auto b = static_cast<unsigned char>(s[i]);
    const size_t len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
    if (len == 1 || i + len > s.size()) {
        ++i;
        return b;
    }
    uint32_t cp = b & (0xFF >> (len + 1));
    for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += len;
    return cp;
}

bool coversCodePoint(const WebFontFace& f, uint32_t cp) {
    if (f.ranges.empty()) return true;
    return std::any_of(f.ranges.begin(), f.ranges.end(), [cp](const auto& r) { return cp >= r.first && cp <= r.second; });
}

// How close a face's weight is to the one asked for, by the CSS font-matching order:
// lower is better. Inside its range is exact.
int weightDistance(const WebFontFace& f, int want) {
    if (want >= f.weight_min && want <= f.weight_max) return 0;
    const bool below = f.weight_max < want;
    const int gap = below ? want - f.weight_max : f.weight_min - want;
    // 400 looks up to 500 first, then down, then up; below that down first; above 500 up first.
    if (want >= 400 && want <= 500) {
        if (!below && f.weight_min <= 500) return gap;
        return below ? 1000 + gap : 2000 + gap;
    }
    if (want < 400) return below ? gap : 1000 + gap;
    return below ? 1000 + gap : gap;
}

// The faces of a family that answer to this weight and style: the closest, all of them,
// because a family split by unicode-range has several of equal standing.
std::vector<size_t> matchingFaces(const std::vector<size_t>& family, int weight, bool italic) {
    bool have_style = false;
    for (const size_t i : family) have_style = have_style || g_faces[i].italic == italic;
    int best = INT32_MAX;
    std::vector<size_t> out;
    for (const size_t i : family) {
        const WebFontFace& f = g_faces[i];
        if (have_style && f.italic != italic) continue;
        const int d = weightDistance(f, weight);
        if (d < best) {
            best = d;
            out.clear();
        }
        if (d == best) out.push_back(i);
    }
    return out;
}

const FontData* dataFor(const WebFontFace& f) {
    for (const std::string& src : f.sources) {
        const auto it = g_font_data.find(src);
        if (it != g_font_data.end()) return &it->second;
    }
    return nullptr;
}

// This face as the font manager has it, opened for this weight and style.
std::string webFaceFamily(const WebFontFace& f, const FontData& data, int weight, bool italic, uint32_t cp) {
    const std::string name = "xweb:" + std::to_string(data.id);
    const auto* face = FontManager::addMemoryFace(name, weightOf(weight), styleOf(italic), data.bytes, weightOf(std::clamp(weight, f.weight_min, f.weight_max)));
    if (!face || !FT_Get_Char_Index(face->ft_face_, cp)) return {};
    return name;
}

// Splits a family list, trimming: "a, b" into "a" and "b".
std::vector<std::string> familyNames(const std::string& list) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        std::string name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const size_t a = name.find_first_not_of(" \t");
        out.push_back(a == std::string::npos ? std::string{} : name.substr(a, name.find_last_not_of(" \t") - a + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// A family name that isn't a web font, if we have such a thing. Fontconfig always answers
// with something, so a name counts only if the font it picks goes by that name.
std::optional<std::string> ordinaryFamily(const std::string& name) {
    static std::unordered_map<std::string, std::optional<std::string>> cache;
    const auto cached = cache.find(name);
    if (cached != cache.end()) return cached->second;
    const std::string key = lowerAscii(name);
    const auto installed = [&] {
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
                found = lowerAscii(reinterpret_cast<const char*>(family)) == key;
            FcPatternDestroy(match);
        }
        FcPatternDestroy(pattern);
        return found;
    };
    std::optional<std::string> out;
    if (key == "serif" || key == "sans-serif" || key == "monospace" || key == "cursive" || key == "fantasy") out = key;
    else if (key == "system-ui" || key == "ui-sans-serif") out = "sans-serif";
    else if (key == "ui-monospace") out = "monospace";
    else if (key == "ui-serif") out = "serif";
    else if (!name.empty() && installed()) out = name;
    cache.emplace(name, out);
    return out;
}

// What draws this character: the first family in the list that can.
std::string familyForCodePoint(const FontSpec& spec, uint32_t cp) {
    std::string key = spec.family;
    key += '\x01';
    key += std::to_string(spec.weight);
    key += spec.italic ? 'i' : 'n';
    key += std::to_string(cp);
    const auto cached = g_run_cache.find(key);
    if (cached != g_run_cache.end()) return cached->second;

    std::string found;
    for (const std::string& name : familyNames(spec.family)) {
        const auto family = g_families.find(lowerAscii(name));
        if (family != g_families.end()) {
            const std::vector<size_t> faces = matchingFaces(family->second, spec.weight, spec.italic);
            // The last rule to cover a character wins it.
            for (auto it = faces.rbegin(); it != faces.rend() && found.empty(); ++it) {
                const WebFontFace& f = g_faces[*it];
                if (!coversCodePoint(f, cp)) continue;
                const FontData* data = dataFor(f);
                if (data) found = webFaceFamily(f, *data, spec.weight, spec.italic, cp);
            }
            if (!found.empty()) break;
        } else if (std::optional<std::string> ordinary = ordinaryFamily(name)) {
            // Which glyphs it has is left to the font manager's own fallback.
            found = std::move(*ordinary);
            break;
        }
    }
    if (found.empty()) found = "sans-serif";
    g_run_cache.emplace(std::move(key), found);
    return found;
}

FT_Face faceFor(const FontSpec& f) {
    // A line's metrics come from the first font available, as judged by a space.
    const std::string family = g_faces.empty() ? fontFamilyFor(f.family) : familyForCodePoint(f, ' ');
    auto* face = FontManager::getFace(family, weightOf(f.weight), styleOf(f.italic));
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
    std::string chosen = "sans-serif";   // what a page naming nothing we have gets
    for (const std::string& name : familyNames(css_list)) {
        if (std::optional<std::string> family = ordinaryFamily(name)) {
            chosen = std::move(*family);
            break;
        }
    }
    cache.emplace(css_list, chosen);
    return chosen;
}

void setPageWebFonts(std::vector<WebFontFace> faces) {
    if (faces.empty() && g_faces.empty()) return;
    g_faces = std::move(faces);
    g_families.clear();
    for (size_t i = 0; i < g_faces.size(); ++i) g_families[g_faces[i].family].push_back(i);
    g_run_cache.clear();
    ++g_fonts_generation;
}

std::vector<WebFontFace> wantedWebFonts(const Document& doc, const StyledDocument& styles) {
    std::vector<WebFontFace> out;
    if (g_faces.empty()) return out;
    std::set<size_t> wanted;
    std::unordered_set<std::string> seen;
    const auto ask = [&](const ComputedStyle& s, std::string_view text) {
        const std::string key_base = s.font_family + '\x01' + std::to_string(s.font_weight) + (s.italic ? 'i' : 'n');
        const auto one = [&](uint32_t cp) {
            if (!seen.insert(key_base + std::to_string(cp)).second) return;
            for (const std::string& name : familyNames(s.font_family)) {
                const auto family = g_families.find(lowerAscii(name));
                if (family == g_families.end()) {
                    if (ordinaryFamily(name)) break;
                    continue;
                }
                const std::vector<size_t> faces = matchingFaces(family->second, s.font_weight, s.italic);
                bool covered = false;
                for (auto it = faces.rbegin(); it != faces.rend(); ++it) {
                    if (!coversCodePoint(g_faces[*it], cp)) continue;
                    wanted.insert(*it);
                    covered = true;
                    break;
                }
                if (covered) break;
            }
        };
        one(' ');
        for (size_t i = 0; i < text.size();) one(nextCodePoint(text, i));
    };
    std::vector<const Node*> todo{doc.root()};
    while (!todo.empty()) {
        const Node* n = todo.back();
        todo.pop_back();
        if (const ComputedStyle* s = styles.styleOf(n)) {
            if (s->display != Display::None) {
                for (const Node* k : n->children)
                    if (k->type == NodeType::Text) ask(*s, k->data);
                if (const ComputedStyle* b = styles.beforeOf(n)) ask(*b, b->content);
                if (const ComputedStyle* a = styles.afterOf(n)) ask(*a, a->content);
            } else {
                continue;   // nothing below shows
            }
        }
        for (const Node* k : n->children)
            if (k->type == NodeType::Element) todo.push_back(k);
    }
    for (const size_t i : wanted) {
        if (out.size() >= kMaxWantedFaces) break;
        out.push_back(g_faces[i]);
    }
    return out;
}

bool webFontUsable(const std::shared_ptr<const std::string>& bytes) {
    if (!bytes || bytes->empty()) return false;
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib)) return false;
    FT_Face face = nullptr;
    const bool ok = !FT_New_Memory_Face(lib, reinterpret_cast<const FT_Byte*>(bytes->data()), static_cast<FT_Long>(bytes->size()), 0, &face) && FT_IS_SCALABLE(face);
    if (face) FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return ok;
}

bool haveWebFontData(const std::string& url) { return g_font_data.count(url) != 0; }

bool addWebFontData(const std::string& url, std::shared_ptr<const std::string> bytes) {
    if (!bytes || g_font_data.count(url)) return true;
    if (g_font_bytes + bytes->size() > kMaxFontBytes) return false;
    g_font_bytes += bytes->size();
    g_font_data[url] = FontData{g_next_font_data++, std::move(bytes)};
    g_run_cache.clear();
    ++g_fonts_generation;
    return true;
}

std::vector<FontRun> fontRuns(std::string_view utf8, const FontSpec& font) {
    std::vector<FontRun> out;
    if (g_faces.empty()) {
        out.push_back({0, utf8.size(), fontFamilyFor(font.family)});
        return out;
    }
    size_t i = 0;
    while (i < utf8.size()) {
        const size_t at = i;
        const uint32_t cp = nextCodePoint(utf8, i);
        // Spaces go with what's around them, unless nothing is yet.
        const bool blank = cp == ' ' || cp == '\t' || cp == '\n' || cp == 0xA0;
        if (blank && !out.empty()) {
            out.back().end = i;
            continue;
        }
        std::string family = familyForCodePoint(font, cp);
        if (!out.empty() && out.back().family == family) {
            out.back().end = i;
        } else {
            out.push_back({at, i, std::move(family)});
        }
    }
    if (out.empty()) out.push_back({0, utf8.size(), familyForCodePoint(font, ' ')});
    return out;
}

float XrenderTextMeasurer::runWidth(std::string_view utf8, const std::string& family, const FontSpec& font) {
    return FontManager::measureText(std::string(utf8), font.size, family, weightOf(font.weight), styleOf(font.italic));
}

float XrenderTextMeasurer::width(std::string_view utf8, const FontSpec& font) {
    if (fonts_generation_ != g_fonts_generation) {
        cache_.clear();
        fonts_generation_ = g_fonts_generation;
    }
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
    float w = 0;
    for (const FontRun& run : fontRuns(utf8, font)) w += runWidth(utf8.substr(run.begin, run.end - run.begin), run.family, font);
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
