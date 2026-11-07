// xweb_paint file.html width height out.png — the page's first screenful, painted on
// the CPU: boxes, borders, images, SVG, gradients and masks, but no text (glyphs are the
// compositor's). scripts/xweb-pixels.py checks it against Chromium's, with text hidden.

#include <png.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "xrender/font_manager.h"
#include "xrender/image_decoder.h"
#include "xweb/html_parser.h"
#include "xweb/layout.h"
#include "xweb/paint.h"
#include "xweb/style.h"
#include "xweb/text_measurer.h"
#include "xweb/url.h"
#include "file_images.h"

namespace {

using namespace fbx::xweb;
using fbx::xweb::tools::FileImages;
using fbx::xweb::tools::Loaded;

bool writePng(const char* path, const Raster& r) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(f);
        return false;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, r.width, r.height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (uint32_t y = 0; y < r.height; ++y) png_write_row(png, const_cast<png_bytep>(r.rgba.data() + static_cast<size_t>(y) * r.width * 4));
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: xweb_paint file.html width height out.png\n";
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::cerr << "can't read " << argv[1] << "\n";
        return 1;
    }
    const std::string html((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto w = static_cast<uint32_t>(std::atoi(argv[2]));
    const auto h = static_cast<uint32_t>(std::atoi(argv[3]));
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return 2;
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
    const StyledDocument styles(*doc, {static_cast<float>(w), static_cast<float>(h)}, sheet_text, ch);
    FileImages images(base);
    const PageLayout page = layoutPage(*doc, styles, text, &images, static_cast<float>(w), static_cast<float>(h));

    // XWEB_PAINT_OPS=1 lists what's painted besides text, for finding what drew what.
    if (std::getenv("XWEB_PAINT_OPS")) {
        static const char* kNames[] = {"rect", "text", "image", "clip", "unclip", "gradient", "svg"};
        for (const PaintOp& op : page.ops)
            if (op.kind != PaintKind::Text)
                std::fprintf(stderr, "%s %.1f %.1f %.1f %.1f %s%s\n", kNames[static_cast<int>(op.kind)], op.x, op.y, op.w, op.h, op.text.substr(0, 60).c_str(), op.tint ? " (tint)" : "");
    }
    Canvas canvas(w, h);
    canvas.fillRect(0, 0, static_cast<float>(w), static_cast<float>(h), page.canvas);
    std::vector<PaintOp> clips;
    const auto applyClip = [&] {
        float l = 0, t = 0, r = static_cast<float>(w), b = static_cast<float>(h);
        for (const PaintOp& c : clips) {
            l = std::max(l, c.x);
            t = std::max(t, c.y);
            r = std::min(r, c.x + c.w);
            b = std::min(b, c.y + c.h);
        }
        canvas.setClip(l, t, std::max(0.0f, r - l), std::max(0.0f, b - t));
    };
    for (const PaintOp& raw : page.ops) {
        // The compositor fills a quad's pixels whose centres are inside it, so square
        // edges land on whole pixels; as Chromium snaps them too.
        PaintOp op = raw;
        if (op.kind != PaintKind::Text && op.kind != PaintKind::ClipBegin && op.kind != PaintKind::ClipEnd && op.radius == 0) {
            const float x0 = std::round(op.x), y0 = std::round(op.y);
            op.w = std::round(op.x + op.w) - x0;
            op.h = std::round(op.y + op.h) - y0;
            op.x = x0;
            op.y = y0;
            if (op.w <= 0 || op.h <= 0) continue;
        }
        switch (op.kind) {
            case PaintKind::ClipBegin: clips.push_back(op); applyClip(); break;
            case PaintKind::ClipEnd: if (!clips.empty()) clips.pop_back(); applyClip(); break;
            case PaintKind::Rect: canvas.fillRect(op.x, op.y, op.w, op.h, op.color, op.radius); break;
            case PaintKind::Text: break;
            case PaintKind::Gradient: {
                if (!op.gradient) break;
                const auto pw = static_cast<uint32_t>(std::clamp(std::ceil(op.w), 1.0f, 2048.0f));
                const auto ph = static_cast<uint32_t>(std::clamp(std::ceil(op.h), 1.0f, 2048.0f));
                const Raster r = renderGradient(*op.gradient, op.w, op.h, pw, ph);
                canvas.drawRaster(r, op.x, op.y, op.w, op.h, op.color.a / 255.0f);
                break;
            }
            case PaintKind::Svg: {
                if (!op.node) break;
                const auto pw = static_cast<uint32_t>(std::clamp(std::lround(op.w), 1L, 4096L));
                const auto ph = static_cast<uint32_t>(std::clamp(std::lround(op.h), 1L, 4096L));
                const Raster r = renderSvg(*op.node, pw, ph, &styles, op.color);
                canvas.drawRaster(r, op.x, op.y, op.w, op.h);
                break;
            }
            case PaintKind::Image: {
                const Loaded* l = images.get(op.text);
                if (!l) break;
                Raster svg_raster;
                const Raster* r = &l->bitmap;
                if (l->svg) {
                    const auto pw = static_cast<uint32_t>(std::clamp(std::lround(op.w), 1L, 4096L));
                    const auto ph = static_cast<uint32_t>(std::clamp(std::lround(op.h), 1L, 4096L));
                    svg_raster = l->svg->render(pw, ph);
                    r = &svg_raster;
                }
                canvas.drawRaster(*r, op.x, op.y, op.w, op.h, op.tint ? 1.0f : op.color.a / 255.0f, op.tint ? &op.color : nullptr);
                break;
            }
        }
    }
    const bool ok = writePng(argv[4], canvas.toRaster());
    fbx::xrender::FontManager::cleanup();
    return ok ? 0 : 1;
}
