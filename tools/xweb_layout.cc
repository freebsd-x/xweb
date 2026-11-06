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
#include <string>
#include <unordered_map>
#include <vector>

#include "xrender/font_manager.h"
#include "xrender/image_decoder.h"
#include "xweb/html_parser.h"
#include "xweb/layout.h"
#include "xweb/style.h"
#include "xweb/text_measurer.h"

namespace {

class FileImages : public fbx::xweb::ImageSizes {
public:
    explicit FileImages(std::filesystem::path base) : base_(std::move(base)) {}
    std::optional<std::pair<float, float>> imageSize(const std::string& src) override {
        auto decoded = fbx::xrender::ImageDecoder::decode((base_ / src).string(), 512, 512);
        if (!decoded) return std::nullopt;
        return std::make_pair(static_cast<float>(decoded->width_), static_cast<float>(decoded->height_));
    }

private:
    std::filesystem::path base_;
};

}  // namespace

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
    const std::filesystem::path page_path(argv[1]);
    const std::filesystem::path base = page_path.has_parent_path() ? page_path.parent_path() : std::filesystem::path(".");
    XrenderTextMeasurer text;
    const StyledDocument styles(
        *doc, {w, h},
        [&](const std::string& href) -> std::string {
            std::ifstream sheet(base / href.substr(0, href.find_first_of("?#")), std::ios::binary);
            return {std::istreambuf_iterator<char>(sheet), std::istreambuf_iterator<char>()};
        },
        [&](const ComputedStyle& s) { return chWidth(text, s); });
    FileImages images(base);
    const PageLayout page = layoutPage(*doc, styles, text, &images, w, h);
    // In document order, whatever order they were painted in.
    std::unordered_map<const Node*, const ElementBox*> by_node;
    for (const ElementBox& b : page.boxes) by_node[b.node] = &b;
    std::vector<const Node*> todo{doc->root()};
    while (!todo.empty()) {
        const Node* n = todo.back();
        todo.pop_back();
        const auto it = by_node.find(n);
        if (it != by_node.end())
            std::printf("%s %.2f %.2f %.2f %.2f\n", n->name.c_str(), it->second->x, it->second->y, it->second->w, it->second->h);
        todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
    }
    std::printf("#page %.2f %.2f ops=%zu links=%zu\n", page.width, page.height, page.ops.size(), page.links.size());
    fbx::xrender::FontManager::cleanup();
    return 0;
}
