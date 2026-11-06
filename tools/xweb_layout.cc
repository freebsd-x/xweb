// xweb_layout file.html width height — every element's border box, one per line:
//   tag x y w h
// Base for comparing our layout with another engine's (scripts/xweb-compare.py).

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "xrender/font_manager.h"
#include "xweb/html_parser.h"
#include "xweb/layout.h"
#include "xweb/style.h"
#include "xweb/text_measurer.h"

int main(int argc, char** argv) {
    using namespace fbx::xweb;
    if (argc != 4) {
        std::cerr << "usage: xweb_layout file.html width height\n";
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
    const StyledDocument styles(*doc, {w, h});
    XrenderTextMeasurer text;
    const PageLayout page = layoutPage(*doc, styles, text, nullptr, w, h);
    for (const ElementBox& b : page.boxes)
        std::printf("%s %.2f %.2f %.2f %.2f\n", b.node->name.c_str(), b.x, b.y, b.w, b.h);
    std::printf("#page %.2f %.2f ops=%zu links=%zu\n", page.width, page.height, page.ops.size(), page.links.size());
    fbx::xrender::FontManager::cleanup();
    return 0;
}
