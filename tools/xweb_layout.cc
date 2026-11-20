// xweb_layout file.html width height — every element's border box, one per line:
//   tag x y w h
// Base for comparing our layout with another engine's (scripts/xweb-compare.py).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <utility>
#include <chrono>
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
    // XWEB_LAYOUT_BENCH=N: styled afresh, styled again (all kept) and laid out N times
    // each, and how long each took on average, for measuring the engine on a real page.
    if (const char* bench = std::getenv("XWEB_LAYOUT_BENCH")) {
        const int n = std::max(1, std::atoi(bench));
        using Clock = std::chrono::steady_clock;
        const auto ms = [](Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); };
        double fresh = 0, kept = 0, laid = 0;
        const bool only_layout = std::getenv("XWEB_LAYOUT_BENCH_ONLY") != nullptr;   // (for profiling it)
        for (int i = 0; i < n; ++i) {
            auto t = Clock::now();
            if (!only_layout) {
                const StyledDocument again(*doc, {w, h}, sheet_text, ch);
                fresh += ms(t);
                t = Clock::now();
                const StyledDocument reused(*doc, {w, h}, sheet_text, ch, nullptr, &styles);
                kept += ms(t);
            }
            t = Clock::now();
            const PageLayout l = layoutPage(*doc, styles, text, &images, w, h);
            laid += ms(t);
        }
        std::fprintf(stderr, "bench: styling %.1f ms, restyling (all kept) %.1f ms, layout %.1f ms\n", fresh / n, kept / n, laid / n);
        return 0;
    }
    const PageLayout page = layoutPage(*doc, styles, text, &images, w, h);
    // XWEB_LAYOUT_OPS=1: the paint ops instead, in order (kind, rectangle, text or source).
    if (std::getenv("XWEB_LAYOUT_OPS")) {
        for (const PaintOp& op : page.ops) {
            std::printf("%d %.1f %.1f %.1f %.1f %s a=%d", static_cast<int>(op.kind), op.x, op.y, op.w, op.h, op.text.substr(0, 60).c_str(), op.color.a);
            if (op.kind == PaintKind::TransformBegin)
                std::printf(" [%g %g %g %g %g %g]", op.transform.a, op.transform.b, op.transform.c, op.transform.d, op.transform.e, op.transform.f);
            std::printf("\n");
        }
        return 0;
    }
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
            // XWEB_LAYOUT_CLASS=1: and .class, and its depth in the tree (for reading one).
            const std::string* cls = std::getenv("XWEB_LAYOUT_CLASS") ? n->attr("class") : nullptr;
            int depth = 0;
            if (cls || std::getenv("XWEB_LAYOUT_CLASS"))
                for (const Node* p = n->parent; p; p = p->parent) ++depth;
            if (std::getenv("XWEB_LAYOUT_CLASS")) std::printf("%*s", depth, "");
            std::printf("%s%s%s%s%s %.2f %.2f %.2f %.2f", n->name.c_str(), id ? "#" : "", id ? id->c_str() : "", cls ? "." : "",
                        cls ? cls->substr(0, 50).c_str() : "", it->second->x, it->second->y, it->second->w, it->second->h);
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
