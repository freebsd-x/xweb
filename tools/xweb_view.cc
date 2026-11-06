// xwebview page.html — a window showing a local page through our own engine: parsing,
// style, layout and painting are all ours. For developing the engine; the Browser app
// keeps using Chromium until this can replace it. Everything is read before the
// sandbox: there's no network or link following yet.

#include <xkit/application.h>
#include <xkit/element.h>
#include <xkit/render_node.h>
#include <xkit/widget.h>
#include <xkit/xbus_client.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xrender/font_manager.h"
#include "xrender/image_decoder.h"
#include "xweb/html_parser.h"
#include "xweb/layout.h"
#include "xweb/style.h"
#include "xweb/text_measurer.h"

using namespace fbx::xkit;
namespace fs = std::filesystem;
namespace web = fbx::xweb;

namespace {

constexpr float kOffscreenMargin = 256;   // painted past the window, for smooth scrolling
constexpr float kMaxWindowHeight = 8192;

// A page refers to files beside it, and nothing else: no absolute paths, no "..".
std::optional<fs::path> besidePage(const fs::path& base, const std::string& ref) {
    if (ref.empty() || ref[0] == '/' || ref.find("://") != std::string::npos) return std::nullopt;
    const fs::path relative(ref);
    if (std::any_of(relative.begin(), relative.end(), [](const fs::path& part) { return part == ".."; })) return std::nullopt;
    return base / relative;
}

struct LoadedImage {
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    uint32_t id = 0;
};

class PageImages : public web::ImageSizes {
public:
    explicit PageImages(fs::path base) : base_(std::move(base)) {}

    std::optional<std::pair<float, float>> imageSize(const std::string& src) override {
        const LoadedImage* image = load(src);
        if (!image) return std::nullopt;
        return std::make_pair(static_cast<float>(image->width), static_cast<float>(image->height));
    }

    const LoadedImage* load(const std::string& src) {
        const auto cached = images_.find(src);
        if (cached != images_.end()) return cached->second ? &*cached->second : nullptr;
        std::optional<LoadedImage> loaded;
        if (const auto path = besidePage(base_, src)) {
            if (auto decoded = fbx::xrender::ImageDecoder::decode(path->string(), 512, 512)) {
                LoadedImage image;
                image.pixels = std::move(decoded->pixels_);
                image.width = decoded->width_;
                image.height = decoded->height_;
                image.id = nextImageId();
                loaded = std::move(image);
            }
        }
        const auto& stored = images_.emplace(src, std::move(loaded)).first->second;
        return stored ? &*stored : nullptr;
    }

private:
    fs::path base_;
    std::map<std::string, std::optional<LoadedImage>> images_;
};

// Holds the parsed page; lays it out for whatever width it's given.
class RenderWebPage : public RenderNode {
public:
    RenderWebPage(std::unique_ptr<web::Document> doc, std::map<std::string, std::string> sheets, fs::path base)
        : doc_(std::move(doc)), sheets_(std::move(sheets)), images_(std::move(base)) {
        setKeepColors(true);   // a page's colours are the page's, in every theme
    }

    [[nodiscard]] Size performLayout(const Constraints& c) noexcept override {
        const float width = c.max_width_ < 1e6f ? c.max_width_ : 1000.0f;
        int window_w = 0, window_h = 0;
        XApplication::windowSize(window_w, window_h);
        const float height = window_h > 0 ? static_cast<float>(window_h) : 800.0f;
        if (width != laid_out_width_ || height != laid_out_height_) {
            laid_out_width_ = width;
            laid_out_height_ = height;
            relayout(width, height);
        }
        return c.clamp({width, page_.height});
    }

    // Layout runs inside the sandbox, too late to open files.
    void loadResources() {
        std::vector<const web::Node*> todo{doc_->root()};
        while (!todo.empty()) {
            const web::Node* n = todo.back();
            todo.pop_back();
            if (n->isHtml("img"))
                if (const std::string* src = n->attr("src")) (void)images_.load(*src);
            todo.insert(todo.end(), n->children.begin(), n->children.end());
        }
    }

    void paintSelf(fbx::xrender::DisplayList& dl, Offset at) noexcept override {
        fill(dl, at.x_, at.y_, size_.width_, size_.height_, page_.canvas);
        for (size_t i = 0; i < page_.ops.size(); ++i) {
            const web::PaintOp& op = page_.ops[i];
            const float y = op.y + at.y_;
            if (y > kMaxWindowHeight + kOffscreenMargin || y + op.h < -kOffscreenMargin) continue;
            switch (op.kind) {
                case web::PaintKind::Rect: fill(dl, op.x + at.x_, y, op.w, op.h, op.color); break;
                case web::PaintKind::Text: text(dl, op, op.x + at.x_, text_y_[i] + at.y_); break;
                case web::PaintKind::Image: image(dl, op, op.x + at.x_, y); break;
            }
        }
    }

    [[nodiscard]] std::string visibleText() const override { return page_.title; }

private:
    void relayout(float width, float height) {
        const web::StyledDocument styles(*doc_, {width, height}, [this](const std::string& href) {
            const auto it = sheets_.find(href);
            return it == sheets_.end() ? std::string{} : it->second;
        });
        page_ = web::layoutPage(*doc_, styles, measurer_, &images_, width, height);
        // The engine puts a run's baseline 0.8em below where it's told: answer in those
        // terms, for the baseline the font's own ascent put it on.
        text_y_.assign(page_.ops.size(), 0);
        for (size_t i = 0; i < page_.ops.size(); ++i)
            if (page_.ops[i].kind == web::PaintKind::Text) {
                const web::PaintOp& op = page_.ops[i];
                text_y_[i] = op.y + measurer_.ascent(op.font) - 0.8f * op.font.size;
            }
    }

    static void fill(fbx::xrender::DisplayList& dl, float x, float y, float w, float h, web::Color c) {
        fbx::shared::RectData r{};
        r.x_ = x;
        r.y_ = y;
        r.width_ = w;
        r.height_ = h;
        r.r_ = static_cast<float>(c.r) / 255;
        r.g_ = static_cast<float>(c.g) / 255;
        r.b_ = static_cast<float>(c.b) / 255;
        r.a_ = static_cast<float>(c.a) / 255;
        dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&r), sizeof(r)});
    }

    static void text(fbx::xrender::DisplayList& dl, const web::PaintOp& op, float x, float y) {
        fbx::shared::TextData td{};
        td.x_ = x;
        td.y_ = y;
        td.font_size_ = op.font.size;
        td.r_ = static_cast<float>(op.color.r) / 255;
        td.g_ = static_cast<float>(op.color.g) / 255;
        td.b_ = static_cast<float>(op.color.b) / 255;
        td.a_ = static_cast<float>(op.color.a) / 255;
        td.string_length_ = static_cast<uint32_t>(op.text.size());
        td.font_weight_ = static_cast<uint16_t>(std::clamp((op.font.weight + 50) / 100 * 100, 100, 900));
        td.font_style_ = static_cast<uint8_t>(op.font.italic ? fbx::xrender::FontStyle::Italic : fbx::xrender::FontStyle::Normal);
        const std::string family = web::fontFamilyFor(op.font.family);
        const size_t n = std::min(family.size(), sizeof(td.font_family_) - 1);
        std::memcpy(td.font_family_, family.data(), n);
        td.font_family_[n] = '\0';
        std::vector<uint8_t> payload(sizeof(td) + op.text.size());
        std::memcpy(payload.data(), &td, sizeof(td));
        std::memcpy(payload.data() + sizeof(td), op.text.data(), op.text.size());
        dl.addCommand(fbx::shared::CommandType::DrawText, payload);
    }

    void image(fbx::xrender::DisplayList& dl, const web::PaintOp& op, float x, float y) {
        const LoadedImage* loaded = images_.load(op.text);
        if (!loaded) return;
        fbx::shared::ImageData d{};
        d.x_ = x;
        d.y_ = y;
        d.width_ = op.w;
        d.height_ = op.h;
        d.texture_id_ = loaded->id;
        d.pixel_ptr_ = loaded->pixels.data();
        d.pixel_width_ = loaded->width;
        d.pixel_height_ = loaded->height;
        d.uv_x0_ = 0;
        d.uv_y0_ = 0;
        d.uv_x1_ = 1;
        d.uv_y1_ = 1;
        dl.addCommand(fbx::shared::CommandType::DrawImage, {reinterpret_cast<const uint8_t*>(&d), sizeof(d)});
    }

    std::unique_ptr<web::Document> doc_;
    std::map<std::string, std::string> sheets_;
    PageImages images_;
    web::XrenderTextMeasurer measurer_;
    web::PageLayout page_;
    std::vector<float> text_y_;
    float laid_out_width_ = -1, laid_out_height_ = -1;
};

class WebPageWidget;

class WebPageElement : public Element {
public:
    explicit WebPageElement(std::shared_ptr<Widget> widget, std::shared_ptr<RenderNode> node) {
        widget_ = std::move(widget);
        renderNode_ = std::move(node);
    }
    void mount(std::shared_ptr<Element> parent) noexcept override { parent_ = parent; }
    void update(std::shared_ptr<Widget> w) noexcept override { widget_ = std::move(w); }
    void unmount() noexcept override {}
};

class WebPageWidget : public Widget {
public:
    explicit WebPageWidget(std::shared_ptr<RenderWebPage> node) : node_(std::move(node)) {}
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override {
        return std::make_shared<WebPageElement>(shared_from_this(), node_);
    }

private:
    std::shared_ptr<RenderWebPage> node_;
};

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Stylesheets the page links to, while we can still read them.
std::map<std::string, std::string> readSheets(const web::Document& doc, const fs::path& base) {
    std::map<std::string, std::string> sheets;
    std::vector<const web::Node*> todo{doc.root()};
    while (!todo.empty()) {
        const web::Node* n = todo.back();
        todo.pop_back();
        if (n->isHtml("link")) {
            const std::string* href = n->attr("href");
            if (href) {
                if (const auto path = besidePage(base, *href)) sheets[*href] = readFile(*path);
            }
        }
        todo.insert(todo.end(), n->children.begin(), n->children.end());
    }
    return sheets;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: xwebview page.html\n";
        return 2;
    }
    const fs::path path(argv[1]);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "can't read " << argv[1] << "\n";
        return 1;
    }
    const std::string html((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto doc = web::parseHtml(html);
    const fs::path base = path.has_parent_path() ? path.parent_path() : fs::path(".");
    auto sheets = readSheets(*doc, base);

    auto node = std::make_shared<RenderWebPage>(std::move(doc), std::move(sheets), base);
    node->loadResources();
    auto page = std::make_shared<WebPageWidget>(node);
    auto scroll = std::make_shared<ScrollViewWidget>(page);
    return XApplication::Run(scroll, "/tmp/freebsdx.sock", false, false, [](XBusClient&) {});
}
