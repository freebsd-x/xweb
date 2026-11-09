// xweb_layout file.html width height — every element's border box, one per line:
//   tag x y w h
// Base for comparing our layout with another engine's (scripts/xweb-compare.py).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <utility>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "xrender/font_manager.h"
#include "xrender/image_decoder.h"
#include "xweb/html_parser.h"
#include "xweb/layout.h"
#include "xweb/style.h"
#include "xweb/text_measurer.h"
#include "xweb/url.h"
#include "file_images.h"

namespace {

}  // namespace

int main(int argc, char** argv) {
    using namespace fbx::xweb;
    // --fonts: each box's font too (size, weight, the family list and the family that
    // draws it), for text that measures one way and draws another.
    const bool fonts = argc == 5 && std::string(argv[4]) == "--fonts";
    if (argc != 4 && !fonts) {
        std::cerr << "usage: xweb_layout file.html width height [--fonts]\n";
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::cerr << "can't read " << argv[1] << "\n";
        return 1;
    }
    const std::string html((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto w = static_cast<float>(std::atof(argv[2]));
    const auto h = static_cast<float>(std::atof(argv[3]));
    fbx::xrender::FontManager::init();
    const auto doc = parseHtml(html);
    const std::filesystem::path page_path(argv[1]);
    const std::filesystem::path base = page_path.has_parent_path() ? page_path.parent_path() : std::filesystem::path(".");
    XrenderTextMeasurer text;
    const auto sheet_text = [&](const std::string& href) -> std::string {
        std::ifstream sheet(base / href.substr(0, href.find_first_of("?#")), std::ios::binary);
        return {std::istreambuf_iterator<char>(sheet), std::istreambuf_iterator<char>()};
    };
    const auto ch = [&](const ComputedStyle& s) { return chWidth(text, s); };
    {
        // Fonts have arrived before layout here: there's nothing to wait for.
        const StyledDocument first(*doc, {w, h}, sheet_text, ch);
        setPageWebFonts(first.fontFaces());
        for (const WebFontFace& face : wantedWebFonts(*doc, first)) {
            for (const std::string& src : face.sources) {
                std::string body, type;
                if (!decodeDataUrl(src, body, type)) {
                    std::ifstream file(base / src.substr(0, src.find_first_of("?#")), std::ios::binary);
                    body.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
                }
                auto bytes = std::make_shared<const std::string>(std::move(body));
                if (!webFontUsable(bytes)) continue;
                (void)addWebFontData(src, bytes);
                break;
            }
        }
    }
    const StyledDocument styles(*doc, {w, h}, sheet_text, ch);
    fbx::xweb::tools::FileImages images(base);
    const PageLayout page = layoutPage(*doc, styles, text, &images, w, h);
    // In document order, whatever order they were painted in.
    std::unordered_map<const Node*, const ElementBox*> by_node;
    for (const ElementBox& b : page.boxes) by_node[b.node] = &b;
    std::vector<const Node*> todo{doc->root()};
    while (!todo.empty()) {
        const Node* n = todo.back();
        todo.pop_back();
        const auto it = by_node.find(n);
        if (it != by_node.end()) {
            // XWEB_LAYOUT_IDS=1 names elements with ids as tag#id, for finding them.
            const std::string* id = std::getenv("XWEB_LAYOUT_IDS") ? n->attr("id") : nullptr;
            std::printf("%s%s%s %.2f %.2f %.2f %.2f", n->name.c_str(), id ? "#" : "", id ? id->c_str() : "", it->second->x, it->second->y,
                        it->second->w, it->second->h);
            if (const ComputedStyle* s = fonts ? styles.styleOf(n) : nullptr) {
                FontSpec f;
                f.size = s->font_size;
                f.weight = s->font_weight;
                f.italic = s->italic;
                f.family = s->font_family;
                const std::vector<FontRun> runs = fontRuns("x", f);
                std::printf(" | %.2fpx %d [%s] -> %s", s->font_size, s->font_weight, s->font_family.c_str(), runs.empty() ? "" : runs[0].family.c_str());
            }
            std::printf("\n");
        }
        todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
    }
    std::printf("#page %.2f %.2f ops=%zu links=%zu\n", page.width, page.height, page.ops.size(), page.links.size());
    fbx::xrender::FontManager::cleanup();
    return 0;
}
