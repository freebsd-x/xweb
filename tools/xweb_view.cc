// xwebview [address] — a small browser on our own engine: parsing, style, layout and
// painting are all ours. Pages come over the network through the compositor's broker
// and xfetch, since this process runs sandboxed and can't open a socket. The Browser app
// keeps using Chromium until this can replace it: no scripts, no forms, no web fonts.

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <xkit/application.h>
#include <xkit/element.h>
#include <xkit/fetch.h>
#include <xkit/media_file.h>
#include <xkit/render_node.h>
#include <xkit/widget.h>
#include <xkit/xbus_client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "xrender/font_manager.h"
#include "xweb/css.h"
#include "xweb/encoding.h"
#include "xweb/html_parser.h"
#include "xweb/layout.h"
#include "xweb/style.h"
#include "xweb/text_measurer.h"
#include "xweb/url.h"

using namespace fbx::xkit;
namespace fs = std::filesystem;
namespace web = fbx::xweb;

namespace {

constexpr float kOffscreenMargin = 256;     // painted past the window, for smooth scrolling
constexpr float kMaxWindowHeight = 8192;
constexpr float kToolbarHeight = 44;
constexpr size_t kMaxSheets = 48;           // stylesheets one page can make us fetch
constexpr size_t kMaxImages = 256;
constexpr size_t kImageWorkers = 4;
constexpr uint64_t kMaxImagePixels = 40'000'000;     // one image
constexpr uint64_t kMaxPagePixels = 120'000'000;     // all of a page's, about 480 MB
constexpr size_t kMaxLocalFile = 32u << 20;
constexpr auto kRelayoutEvery = std::chrono::milliseconds(200);

std::string lowerCase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
    return s;
}

std::string escapeHtml(std::string_view in) {
    std::string out;
    for (const char c : in) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

// ── Getting things ────────────────────────────────────────────────────────────

struct Resource {
    bool ok = false;
    std::string body, final_url, content_type, error;
    uint32_t http_status = 0;
};

// The folder of the local page we were started on, opened before the sandbox: pages in
// it can reach what's beside them, and capability mode keeps them from leaving it.
int g_local_root = -1;
std::string g_local_root_path;   // as a file URL's path, ending in '/'

std::string percentDecoded(std::string_view in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1; };
        if (in[i] == '%' && i + 2 < in.size() && hex(in[i + 1]) >= 0 && hex(in[i + 2]) >= 0) {
            out += static_cast<char>(hex(in[i + 1]) * 16 + hex(in[i + 2]));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

Resource loadLocal(const web::Url& url) {
    Resource r;
    r.final_url = url.toString();
    const std::string path = percentDecoded(url.path);
    if (g_local_root < 0 || !path.starts_with(g_local_root_path)) {
        r.error = "This page can't open files outside its own folder.";
        return r;
    }
    const std::string relative = path.substr(g_local_root_path.size());
    const fs::path parts(relative);
    if (relative.empty() || std::any_of(parts.begin(), parts.end(), [](const fs::path& p) { return p == ".."; })) {
        r.error = "This page can't open files outside its own folder.";
        return r;
    }
    const int fd = ::openat(g_local_root, relative.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        r.error = "The file can't be read.";
        return r;
    }
    if (!readAllFd(fd, r.body, kMaxLocalFile)) {
        ::close(fd);
        r.error = "The file can't be read.";
        return r;
    }
    ::close(fd);
    const std::string lower = lowerCase(relative);
    r.content_type = lower.ends_with(".css") ? "text/css" : lower.ends_with(".txt") ? "text/plain" : lower.ends_with(".html") || lower.ends_with(".htm") ? "text/html" : "";
    r.ok = true;
    return r;
}

Resource load(const std::string& address) {
    const web::Url url = web::parseUrl(address);
    if (url.scheme == "file") return loadLocal(url);
    Resource r;
    r.final_url = address;
    if (url.scheme != "http" && url.scheme != "https") {
        r.error = "This address isn't something a browser can open here: " + url.scheme + ":";
        return r;
    }
    // A fragment isn't sent to the server.
    auto got = fetchWebBlocking(url.withoutFragment());
    r.http_status = got.http_status;
    switch (got.status) {
        case fbx::shared::FetchStatus::Ok:
            r.ok = true;
            r.body = std::move(got.body);
            r.content_type = lowerCase(got.content_type);
            r.final_url = got.final_url;
            if (url.has_fragment) r.final_url += "#" + url.fragment;
            break;
        case fbx::shared::FetchStatus::Denied: r.error = "This address isn't allowed."; break;
        case fbx::shared::FetchStatus::TooLarge: r.error = "That's too large to open."; break;
        case fbx::shared::FetchStatus::Unavailable: r.error = "The connection service isn't running."; break;
        case fbx::shared::FetchStatus::BadRequest: r.error = "That address is too long."; break;
        default: r.error = "The site can't be reached."; break;
    }
    return r;
}

// ── A loaded page ─────────────────────────────────────────────────────────────

struct LoadedImage {
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    uint32_t id = 0;
};

struct Page {
    std::unique_ptr<web::Document> doc;
    std::string url;       // where it really came from
    std::string base;      // what its relative links are relative to
    std::map<std::string, std::string> sheets;   // by absolute URL
    std::map<std::string, std::shared_ptr<LoadedImage>> images;   // by absolute URL, UI thread only
    uint64_t pixels = 0;
};

std::string findBase(const web::Document& doc, const std::string& url) {
    std::vector<const web::Node*> todo{doc.root()};
    while (!todo.empty()) {
        const web::Node* n = todo.back();
        todo.pop_back();
        if (n->isHtml("base"))
            if (const std::string* href = n->attr("href")) return web::resolveUrl(url, *href);
        todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
    }
    return url;
}

// Stylesheets the page links to, and what those import.
void loadSheets(Page& page, const std::atomic<uint64_t>& generation, uint64_t mine) {
    std::deque<std::string> queue;
    std::vector<const web::Node*> todo{page.doc->root()};
    while (!todo.empty()) {
        const web::Node* n = todo.back();
        todo.pop_back();
        if (n->isHtml("link")) {
            const std::string* rel = n->attr("rel");
            const std::string* href = n->attr("href");
            if (rel && href && lowerCase(*rel).find("stylesheet") != std::string::npos) queue.push_back(web::resolveUrl(page.base, *href));
        } else if (n->isHtml("style")) {
            for (const std::string& ref : web::css::parseStylesheet(n->textContent()).imports)
                queue.push_back(web::resolveUrl(page.base, ref));
        }
        todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
    }
    size_t fetched = 0;
    while (!queue.empty() && fetched < kMaxSheets && generation.load() == mine) {
        const std::string url = queue.front();
        queue.pop_front();
        if (page.sheets.count(url)) continue;
        ++fetched;
        Resource got = load(url);
        std::string css = got.ok ? web::decodeToUtf8(got.body, got.content_type) : std::string{};
        for (const std::string& ref : web::css::parseStylesheet(css).imports) queue.push_back(web::resolveUrl(url, ref));
        page.sheets[url] = std::move(css);
    }
}

std::vector<std::string> imageSources(const Page& page) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    std::vector<const web::Node*> todo{page.doc->root()};
    while (!todo.empty() && out.size() < kMaxImages) {
        const web::Node* n = todo.back();
        todo.pop_back();
        if (n->isHtml("img"))
            if (const std::string* src = n->attr("src")) {
                std::string url = web::resolveUrl(page.base, *src);
                if (seen.insert(url).second) out.push_back(std::move(url));
            }
        todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
    }
    return out;
}

std::optional<LoadedImage> decodeImage(const std::string& url, const std::string& bytes) {
    // The decoder takes a descriptor; anonymous shared memory works inside the sandbox.
    const int fd = shm_open(SHM_ANON, O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return std::nullopt;
    std::optional<LoadedImage> out;
    if (::ftruncate(fd, static_cast<off_t>(bytes.size())) == 0 && !bytes.empty()) {
        void* map = ::mmap(nullptr, bytes.size(), PROT_WRITE, MAP_SHARED, fd, 0);
        if (map != MAP_FAILED) {
            std::memcpy(map, bytes.data(), bytes.size());
            ::munmap(map, bytes.size());
            LoadedImage image;
            uint32_t width = 0, height = 0;
            std::string error;
            const std::string name = web::parseUrl(url).path;
            if (decodeImageFd(fd, name, image.pixels, width, height, error) && width > 0 && height > 0 &&
                static_cast<uint64_t>(width) * height <= kMaxImagePixels) {
                image.width = width;
                image.height = height;
                image.id = nextImageId();
                out = std::move(image);
            }
        }
    }
    ::close(fd);
    return out;
}

// ── Drawing it ────────────────────────────────────────────────────────────────

class PageImages : public web::ImageSizes {
public:
    void setPage(Page* page) { page_ = page; }

    std::optional<std::pair<float, float>> imageSize(const std::string& src) override {
        const LoadedImage* image = find(src);
        if (!image) return std::nullopt;
        return std::make_pair(static_cast<float>(image->width), static_cast<float>(image->height));
    }

    const LoadedImage* find(const std::string& src) const {
        if (!page_) return nullptr;
        const auto it = page_->images.find(web::resolveUrl(page_->base, src));
        return it == page_->images.end() ? nullptr : it->second.get();
    }

private:
    Page* page_ = nullptr;
};

class RenderWebPage : public RenderNode {
public:
    RenderWebPage() { setKeepColors(true); }   // a page's colours are the page's, in every theme

    void show(std::shared_ptr<Page> page) {
        page_ = std::move(page);
        images_.setPage(page_.get());
        dirty_ = true;
    }
    void invalidate() { dirty_ = true; }
    [[nodiscard]] Page* page() const { return page_.get(); }

    [[nodiscard]] Size performLayout(const Constraints& c) noexcept override {
        const float width = c.max_width_ < 1e6f ? c.max_width_ : 1000.0f;
        int window_w = 0, window_h = 0;
        XApplication::windowSize(window_w, window_h);
        const float height = window_h > 0 ? static_cast<float>(window_h) - kToolbarHeight : 800.0f;
        // The scroll view shows the window's worth, not the page's height: it would be
        // nothing to scroll otherwise. Windows don't resize, so this stays true.
        if (RenderNode* scroll = parentNode()) scroll->setFixedSize(0, height);
        if (dirty_ || width != laid_out_width_ || height != laid_out_height_) {
            dirty_ = false;
            laid_out_width_ = width;
            laid_out_height_ = height;
            relayout(width, height);
        }
        return c.clamp({width, layout_.height});
    }

    void paintSelf(fbx::xrender::DisplayList& dl, Offset at) noexcept override {
        fill(dl, at.x_, at.y_, size_.width_, size_.height_, layout_.canvas);
        // Clips nest, but the engine only has "set" and "reset": each is set to what it
        // and its parents leave, and a pop goes back to the one before. The bottom of the
        // stack is the scroll view's own area, so a pop never uncovers the toolbar.
        std::vector<web::PaintOp> clips;
        const auto applyClip = [&] {
            float l = -at.x_, t = -at.y_, r = l + size_.width_, b = t + kMaxWindowHeight;
            for (const auto& c : clips) {
                l = std::max(l, c.x);
                t = std::max(t, c.y);
                r = std::min(r, c.x + c.w);
                b = std::min(b, c.y + c.h);
            }
            float top = t + at.y_;
            const float view_top = viewportTop();
            float height = b - t;
            if (top < view_top) {
                height -= view_top - top;
                top = view_top;
            }
            fbx::shared::ScissorData sd{l + at.x_, top, std::max(0.0f, r - l), std::max(0.0f, height)};
            dl.addCommand(fbx::shared::CommandType::SetScissor, {reinterpret_cast<const uint8_t*>(&sd), sizeof(sd)});
        };
        for (size_t i = 0; i < layout_.ops.size(); ++i) {
            const web::PaintOp& op = layout_.ops[i];
            if (op.kind == web::PaintKind::ClipBegin) {
                clips.push_back(op);
                applyClip();
                continue;
            }
            if (op.kind == web::PaintKind::ClipEnd) {
                if (!clips.empty()) clips.pop_back();
                applyClip();
                continue;
            }
            const float y = op.y + at.y_;
            if (y > kMaxWindowHeight + kOffscreenMargin || y + op.h < -kOffscreenMargin) continue;
            switch (op.kind) {
                case web::PaintKind::Rect: fill(dl, op.x + at.x_, y, op.w, op.h, op.color, op.radius); break;
                case web::PaintKind::Text: text(dl, op, op.x + at.x_, text_y_[i] + at.y_); break;
                case web::PaintKind::Image: image(dl, op, op.x + at.x_, y); break;
                default: break;
            }
        }
        if (!clips.empty()) {
            clips.clear();
            applyClip();
        }
    }

    [[nodiscard]] std::string visibleText() const override { return layout_.title; }
    [[nodiscard]] const std::string& title() const { return layout_.title; }

    [[nodiscard]] const std::string* linkAt(float page_x, float page_y) const {
        // Later areas are on top.
        for (auto it = layout_.links.rbegin(); it != layout_.links.rend(); ++it)
            if (page_x >= it->x && page_x < it->x + it->w && page_y >= it->y && page_y < it->y + it->h) return &it->href;
        return nullptr;
    }

    // Where an element with this id or name is on the page.
    [[nodiscard]] std::optional<float> anchorTop(const std::string& name) const {
        for (const web::ElementBox& b : layout_.boxes) {
            const std::string* id = b.node->attr("id");
            const std::string* anchor = b.node->isHtml("a") ? b.node->attr("name") : nullptr;
            if ((id && *id == name) || (anchor && *anchor == name)) return b.y;
        }
        return std::nullopt;
    }

    void scrollTo(float y) {
        if (auto* scroll = dynamic_cast<RenderScrollView*>(parentNode())) scroll->setScrollY(y);
    }
    void scrollBy(float dy) {
        if (auto* scroll = dynamic_cast<RenderScrollView*>(parentNode())) scroll->setScrollY(scroll->getScrollY() + dy);
    }
    [[nodiscard]] float scrollY() {
        auto* scroll = dynamic_cast<RenderScrollView*>(parentNode());
        return scroll ? scroll->getScrollY() : 0;
    }

private:
    [[nodiscard]] float viewportTop() const { return kToolbarHeight; }

    RenderNode* parentNode() const { return parent_.lock().get(); }

    void relayout(float width, float height) {
        if (!page_ || !page_->doc) {
            layout_ = {};
            text_y_.clear();
            return;
        }
        const Page& page = *page_;
        const web::StyledDocument styles(
            *page.doc, {width, height},
            [&page](const std::string& href) {
                const auto it = page.sheets.find(web::resolveUrl(page.base, href));
                return it == page.sheets.end() ? std::string{} : it->second;
            },
            [this](const web::ComputedStyle& s) { return web::chWidth(measurer_, s); });
        layout_ = web::layoutPage(*page.doc, styles, measurer_, &images_, width, height);
        // The engine puts a run's baseline 0.8em below where it's told: answer in those
        // terms, for the baseline the font's own ascent put it on.
        text_y_.assign(layout_.ops.size(), 0);
        for (size_t i = 0; i < layout_.ops.size(); ++i)
            if (layout_.ops[i].kind == web::PaintKind::Text) {
                const web::PaintOp& op = layout_.ops[i];
                text_y_[i] = op.y + measurer_.ascent(op.font) - 0.8f * op.font.size;
            }
    }

    static void fill(fbx::xrender::DisplayList& dl, float x, float y, float w, float h, web::Color c, float radius = 0) {
        fbx::shared::RectData r{};
        r.x_ = x;
        r.y_ = y;
        r.width_ = w;
        r.height_ = h;
        r.r_ = static_cast<float>(c.r) / 255;
        r.g_ = static_cast<float>(c.g) / 255;
        r.b_ = static_cast<float>(c.b) / 255;
        r.a_ = static_cast<float>(c.a) / 255;
        r.border_radius_ = radius;
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
        const LoadedImage* loaded = images_.find(op.text);
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

    std::shared_ptr<Page> page_;
    PageImages images_;
    web::XrenderTextMeasurer measurer_;
    web::PageLayout layout_;
    std::vector<float> text_y_;
    float laid_out_width_ = -1, laid_out_height_ = -1;
    bool dirty_ = true;
};

// ── The browser ───────────────────────────────────────────────────────────────

class Browser {
public:
    explicit Browser(std::shared_ptr<RenderWebPage> node) : node_(std::move(node)) {}

    void setAddressField(std::shared_ptr<TextInputWidget> field) { address_ = std::move(field); }

    void go(const std::string& typed) {
        const std::string url = normalize(typed);
        if (url.empty()) return;
        navigate(url, Mode::New);
    }

    void follow(const std::string& href) {
        Page* page = node_->page();
        const std::string url = web::resolveUrl(page ? page->base : std::string{}, href);
        const web::Url target = web::parseUrl(url);
        // In-page links don't load anything.
        if (page && target.has_fragment && target.withoutFragment() == web::parseUrl(page->url).withoutFragment()) {
            pushHistory(url);
            showAddress(url);
            scrollToFragment(target.fragment);
            return;
        }
        if (target.scheme != "http" && target.scheme != "https" && target.scheme != "file") return;   // mailto:, javascript:
        navigate(url, Mode::New);
    }

    void back() { travel(-1); }
    void forward() { travel(1); }
    void reload() {
        if (index_ < history_.size()) navigate(history_[index_].url, Mode::Reload);
    }

    // Once a frame: images arrive in bursts, and laying a page out for each would crawl.
    void onFrame() {
        if (!relayout_wanted_) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_relayout_ < kRelayoutEvery) return;
        last_relayout_ = now;
        relayout_wanted_ = false;
        node_->invalidate();
    }

    [[nodiscard]] RenderWebPage& node() { return *node_; }

private:
    enum class Mode { New, Reload, History };

    struct Entry {
        std::string url;
        float scroll = 0;
    };

    static std::string normalize(std::string typed) {
        const size_t begin = typed.find_first_not_of(" \t\n");
        if (begin == std::string::npos) return {};
        typed = typed.substr(begin, typed.find_last_not_of(" \t\n") - begin + 1);
        const web::Url parsed = web::parseUrl(typed);
        if (parsed.scheme == "http" || parsed.scheme == "https" || parsed.scheme == "file") return web::resolveUrl(typed, "");
        // "example.com" or "localhost:8000/x" mean the web, and "example.com:8000" isn't a scheme.
        return web::resolveUrl("https://" + typed, "");
    }

    void pushHistory(const std::string& url) {
        if (index_ < history_.size()) {
            history_[index_].scroll = node_->scrollY();
            history_.resize(index_ + 1);
            ++index_;
        } else {
            index_ = 0;
            history_.clear();
        }
        history_.push_back({url, 0});
    }

    void travel(int delta) {
        const auto target = static_cast<long>(index_) + delta;
        if (target < 0 || target >= static_cast<long>(history_.size())) return;
        history_[index_].scroll = node_->scrollY();
        index_ = static_cast<size_t>(target);
        navigate(history_[index_].url, Mode::History);
    }

    void showAddress(const std::string& url) {
        if (address_) address_->setText(url);
    }

    void scrollToFragment(const std::string& fragment) {
        // Layout has to have seen the page: next frame.
        pending_fragment_ = fragment;
        XApplication::post([this] {
            if (const auto top = node_->anchorTop(percentDecoded(pending_fragment_))) node_->scrollTo(*top);
        });
    }

    void navigate(const std::string& url, Mode mode) {
        if (mode == Mode::New) pushHistory(url);
        showAddress(url);
        XApplication::setTitle("Loading...");
        const uint64_t mine = ++generation_;
        const float restore = mode == Mode::History ? history_[index_].scroll : (mode == Mode::Reload ? node_->scrollY() : 0);
        std::thread([this, url, mine, restore] {
            auto page = std::make_shared<Page>();
            Resource got = load(url);
            if (generation_.load() != mine) return;
            page->url = got.ok ? got.final_url : url;
            page->doc = web::parseHtml(htmlFor(got, url));
            page->base = findBase(*page->doc, page->url);
            loadSheets(*page, generation_, mine);
            if (generation_.load() != mine) return;
            std::vector<std::string> images = imageSources(*page);
            XApplication::post([this, page, mine, restore, images = std::move(images)]() mutable {
                if (generation_.load() != mine) return;
                install(page, restore);
                loadImages(page, std::move(images), mine);
            });
        }).detach();
    }

    // What to show for what we got.
    static std::string htmlFor(const Resource& got, const std::string& url) {
        if (!got.ok) return message(got.error, url);
        const std::string& type = got.content_type;
        if (type.starts_with("image/")) return "<!doctype html><title>" + escapeHtml(url) + "</title><body style='margin:0;background:#222'><img src=\"" + escapeHtml(got.final_url) + "\">";
        if (type.starts_with("text/plain")) return "<!doctype html><title>" + escapeHtml(url) + "</title><pre>" + escapeHtml(web::decodeToUtf8(got.body, type)) + "</pre>";
        if (type.empty() || type.starts_with("text/") || type.find("html") != std::string::npos || type.find("xml") != std::string::npos)
            return web::decodeToUtf8(got.body, type);
        return message("This is a " + type + " file, which can't be shown here.", url);
    }

    static std::string message(const std::string& text, const std::string& url) {
        return "<!doctype html><title>Can't open this page</title><style>body{font-family:sans-serif;margin:64px;color:#333}"
               "h1{font-size:24px;font-weight:600}p{color:#666;word-break:break-all}</style><h1>" + escapeHtml(text) + "</h1><p>" + escapeHtml(url) + "</p>";
    }

    void install(const std::shared_ptr<Page>& page, float scroll) {
        node_->show(page);
        // Redirected? This is where we are now.
        if (index_ < history_.size()) history_[index_].url = page->url;
        showAddress(page->url);
        node_->scrollTo(scroll);
        const web::Url url = web::parseUrl(page->url);
        if (url.has_fragment && scroll == 0) scrollToFragment(url.fragment);
        XApplication::post([this] {
            // The title comes out of layout, which has just run.
            XApplication::setTitle(node_->title().empty() ? "Web" : node_->title());
        });
    }

    void loadImages(const std::shared_ptr<Page>& page, std::vector<std::string> sources, uint64_t mine) {
        if (sources.empty()) return;
        struct Work {
            std::mutex mutex;
            std::deque<std::string> queue;
        };
        auto work = std::make_shared<Work>();
        work->queue.assign(std::make_move_iterator(sources.begin()), std::make_move_iterator(sources.end()));
        for (size_t i = 0; i < kImageWorkers; ++i) {
            std::thread([this, page, work, mine] {
                while (generation_.load() == mine) {
                    std::string url;
                    {
                        std::lock_guard<std::mutex> lock(work->mutex);
                        if (work->queue.empty()) return;
                        url = std::move(work->queue.front());
                        work->queue.pop_front();
                    }
                    Resource got = load(url);
                    if (!got.ok || got.http_status >= 400) continue;
                    auto decoded = decodeImage(url, got.body);
                    if (!decoded) continue;
                    auto image = std::make_shared<LoadedImage>(std::move(*decoded));
                    XApplication::post([this, page, url, image, mine] {
                        if (generation_.load() != mine) return;
                        const uint64_t pixels = static_cast<uint64_t>(image->width) * image->height;
                        if (page->pixels + pixels > kMaxPagePixels) return;
                        page->pixels += pixels;
                        page->images[url] = image;
                        relayout_wanted_ = true;
                    });
                }
            }).detach();
        }
    }

    std::shared_ptr<RenderWebPage> node_;
    std::shared_ptr<TextInputWidget> address_;
    std::vector<Entry> history_;
    size_t index_ = 0;
    std::atomic<uint64_t> generation_{0};
    std::string pending_fragment_;
    bool relayout_wanted_ = false;
    std::chrono::steady_clock::time_point last_relayout_{};
};

class WebPageElement : public Element {
public:
    WebPageElement(std::shared_ptr<Widget> widget, std::shared_ptr<RenderWebPage> node, Browser* browser) : node_(node), browser_(browser) {
        widget_ = std::move(widget);
        renderNode_ = std::move(node);
    }
    void mount(std::shared_ptr<Element> parent) noexcept override { parent_ = parent; }
    void update(std::shared_ptr<Widget> w) noexcept override { widget_ = std::move(w); }
    void unmount() noexcept override {}

    [[nodiscard]] bool handleEvent(const fbx::shared::InputMessage& event) noexcept override {
        const auto at = node_->getAbsoluteOffset();
        const float x = static_cast<float>(event.value_a_) - at.x_;
        const float y = static_cast<float>(event.value_b_) - at.y_;
        if (event.type_ == fbx::shared::InputEventType::MouseMove) {
            node_->setCursor(node_->linkAt(x, y) ? CursorShape::Pointer : CursorShape::Default);
            return false;
        }
        if (event.type_ != fbx::shared::InputEventType::MouseButton) return false;
        // A click: pressed and released on the same link.
        const std::string* link = node_->linkAt(x, y);
        if (event.char_value_ != 0) {
            pressed_ = link ? *link : std::string{};
            return link != nullptr;
        }
        if (link && *link == pressed_) {
            const std::string href = *link;
            pressed_.clear();
            // Not from inside the event: following it replaces what's being clicked on.
            Browser* browser = browser_;
            XApplication::post([browser, href] { browser->follow(href); });
            return true;
        }
        pressed_.clear();
        return false;
    }

private:
    std::shared_ptr<RenderWebPage> node_;
    Browser* browser_;
    std::string pressed_;
};

class WebPageWidget : public Widget {
public:
    WebPageWidget(std::shared_ptr<RenderWebPage> node, Browser* browser) : node_(std::move(node)), browser_(browser) {}
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override {
        return std::make_shared<WebPageElement>(shared_from_this(), node_, browser_);
    }

private:
    std::shared_ptr<RenderWebPage> node_;
    Browser* browser_;
};

// What was asked for on the command line: an address, or a local file.
std::string startingAddress(const std::string& arg) {
    const web::Url parsed = web::parseUrl(arg);
    std::error_code ec;
    if (parsed.scheme.empty() || parsed.scheme == "file") {
        const fs::path path = parsed.scheme == "file" ? fs::path(percentDecoded(parsed.path)) : fs::path(arg);
        if (fs::is_regular_file(path, ec)) {
            const fs::path absolute = fs::absolute(path, ec).lexically_normal();
            g_local_root = ::open(absolute.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            g_local_root_path = absolute.parent_path().string();
            if (!g_local_root_path.ends_with('/')) g_local_root_path += '/';
            return "file://" + absolute.string();
        }
    }
    return arg;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string start = argc > 1 ? startingAddress(argv[1]) : std::string{};

    auto node = std::make_shared<RenderWebPage>();
    static Browser browser(node);

    auto address = std::make_shared<TextInputWidget>();
    address->setPlaceholder("Address");
    address->setFontSize(14);
    address->setPadding(6);
    address->setOnSubmit([](const std::string& text) { browser.go(text); });
    browser.setAddressField(address);

    auto toolbar = std::make_shared<FlexRow>();
    toolbar->setFixedSize(0, kToolbarHeight);
    toolbar->setPadding(6);
    toolbar->setGap(6);
    toolbar->setCrossAxisAlignment(CrossAxisAlignment::Center);
    const auto button = [&toolbar](const char* label, float width, std::function<void()> action) {
        auto b = std::make_shared<ButtonWidget>(label, std::move(action));
        b->setFixedSize(width, 30);
        toolbar->addChild(b);
    };
    button("Back", 56, [] { browser.back(); });
    button("Forward", 72, [] { browser.forward(); });
    button("Reload", 64, [] { browser.reload(); });
    toolbar->addChild(std::make_shared<FlexibleWidget>(address));

    auto page = std::make_shared<WebPageWidget>(node, &browser);
    auto scroll = std::make_shared<ScrollViewWidget>(page);

    auto root = std::make_shared<FlexColumn>();
    root->addChild(toolbar);
    root->addChild(std::make_shared<FlexibleWidget>(scroll));

    XApplication::setOnFrame([] { browser.onFrame(); });
    XApplication::setOnShortcut([address](const fbx::shared::InputMessage& key) {
        constexpr int kF5 = 63, kL = 38, kR = 19, kLeft = 105, kRight = 106;
        constexpr uint32_t kCtrl = 2, kAlt = 4;
        if (key.value_a_ == kF5 || (key.value_a_ == kR && (key.modifiers_ & kCtrl))) {
            browser.reload();
            return true;
        }
        if (key.value_a_ == kL && (key.modifiers_ & kCtrl)) {
            if (!XApplication::focusWidget(address)) return false;
            address->selectAll();
            return true;
        }
        if (key.modifiers_ & kAlt) {
            if (key.value_a_ == kLeft) { browser.back(); return true; }
            if (key.value_a_ == kRight) { browser.forward(); return true; }
        }
        return false;
    });

    const int status = XApplication::Run(root, "/tmp/freebsdx.sock", false, false, [start, address_field = address](XBusClient&) {
        // The field takes the rest of the row, which only now has a known width.
        int window_w = 0, window_h = 0;
        XApplication::windowSize(window_w, window_h);
        if (window_w > 0) address_field->setFixedSize(static_cast<float>(window_w) - 56 - 72 - 64 - 6 * 5, 30);
        // The last chance to open font files: the sandbox closes right after this.
        using fbx::xrender::FontManager;
        using fbx::xrender::FontStyle;
        using fbx::xrender::FontWeight;
        for (const char* family : {"", "sans-serif", "serif", "monospace"})
            for (const int weight : {100, 200, 300, 400, 500, 600, 700, 800, 900})
                for (const FontStyle style : {FontStyle::Normal, FontStyle::Italic})
                    (void)FontManager::getFace(family, static_cast<FontWeight>(weight), style);
        if (!start.empty()) XApplication::post([start] { browser.go(start); });
    });
    // Loads may still be running on other threads, using what would be destroyed here.
    std::_Exit(status);
}
