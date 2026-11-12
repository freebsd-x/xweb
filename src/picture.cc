#include "xweb/picture.h"

#include <cctype>
#include <cstdlib>
#include <vector>

namespace fbx::xweb {

namespace {

struct Candidate {
    std::string url;
    float density = 0;   // an x descriptor
    float width = 0;     // a w descriptor
};

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// The HTML srcset grammar, leniently: URL, then descriptors up to a comma. (A URL may
// itself hold commas; it ends at white space, and a trailing comma is its end too.)
std::vector<Candidate> parseSrcset(const std::string& s) {
    std::vector<Candidate> out;
    size_t i = 0;
    while (i < s.size() && out.size() < 256) {
        while (i < s.size() && (isSpace(s[i]) || s[i] == ',')) ++i;
        if (i >= s.size()) break;
        const size_t start = i;
        while (i < s.size() && !isSpace(s[i])) ++i;
        std::string url = s.substr(start, i - start);
        bool ended = false;
        while (!url.empty() && url.back() == ',') {
            url.pop_back();
            ended = true;
        }
        Candidate c;
        c.url = url;
        if (!ended) {
            const size_t comma = s.find(',', i);
            const std::string desc = s.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
            i = comma == std::string::npos ? s.size() : comma + 1;
            size_t j = 0;
            while (j < desc.size()) {
                while (j < desc.size() && isSpace(desc[j])) ++j;
                const size_t d = j;
                while (j < desc.size() && !isSpace(desc[j])) ++j;
                const std::string word = desc.substr(d, j - d);
                if (word.size() < 2) continue;
                const float v = std::strtof(word.c_str(), nullptr);
                const char unit = static_cast<char>(std::tolower(static_cast<unsigned char>(word.back())));
                if (v > 0 && unit == 'x') c.density = v;
                else if (v > 0 && unit == 'w') c.width = v;
            }
        }
        if (!c.url.empty()) out.push_back(std::move(c));
    }
    return out;
}

std::optional<std::string> choose(const std::string& srcset, float viewport_w) {
    std::vector<Candidate> all = parseSrcset(srcset);
    if (all.empty()) return std::nullopt;
    // What each is worth in density: a width over the slot (the viewport, sizes's default).
    const float slot = viewport_w > 0 ? viewport_w : 1024;
    const auto density = [&](const Candidate& c) { return c.width > 0 ? c.width / slot : (c.density > 0 ? c.density : 1.0f); };
    const Candidate* best = nullptr;
    for (const Candidate& c : all)
        if (density(c) >= 1.0f && (!best || density(c) < density(*best))) best = &c;
    if (!best)   // none sharp enough: the sharpest
        for (const Candidate& c : all)
            if (!best || density(c) > density(*best)) best = &c;
    return best->url;
}

// Types we can decode (no AVIF or JPEG XL).
bool decodable(const std::string* type) {
    if (!type) return true;
    std::string t;
    for (const char c : *type) {
        if (c == ';') break;
        if (!isSpace(c)) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return t.empty() || t == "image/webp" || t == "image/jpeg" || t == "image/jpg" || t == "image/png" || t == "image/gif" ||
           t == "image/svg+xml" || t == "image/bmp" || t == "image/x-icon" || t == "image/vnd.microsoft.icon";
}

}  // namespace

std::optional<std::string> imageSourceOf(const Node* img, const css::MediaContext& viewport) {
    if (!img) return std::nullopt;
    if (img->parent && img->parent->isHtml("picture")) {
        for (const Node* c : img->parent->children) {
            if (c == img) break;
            if (!c->isHtml("source")) continue;
            const std::string* srcset = c->attr("srcset");
            if (!srcset || !decodable(c->attr("type"))) continue;
            if (const std::string* media = c->attr("media"); media && !media->empty() && !css::mediaMatches(css::tokenize(*media), viewport)) continue;
            if (auto url = choose(*srcset, viewport.width)) return url;
        }
    }
    if (const std::string* srcset = img->attr("srcset"); srcset && !srcset->empty())
        if (auto url = choose(*srcset, viewport.width)) return url;
    if (const std::string* src = img->attr("src"); src && !src->empty()) return *src;
    return std::nullopt;
}

}  // namespace fbx::xweb
