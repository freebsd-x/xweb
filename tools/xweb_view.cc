// xwebview [--open] [address] — a browser on our own engine: parsing, style, layout and
// painting are all ours. Pages come over the network through the compositor's broker
// and xfetch, since this process runs sandboxed and can't open a socket. It's also the
// Browser app (apps/Browser.xapp), which builds this file in: there "Open in Chromium"
// hands the page to Chromium for what we can't do yet (scripts, forms, cookies).

#include <cxxabi.h>
#include <exception>
#include <typeinfo>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <xkit/application.h>
#include <xkit/clipboard.h>
#include <xkit/text_edit.h>
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
#include FT_SIZES_H
#include "xweb/picture.h"
#include "xweb/css.h"
#include "xweb/encoding.h"
#include "xweb/forms.h"
#include "xweb/html_parser.h"
#include "xweb/js.h"
#include "xweb/layout.h"
#include "xweb/paint.h"
#include "xweb/script.h"
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
constexpr float kChromiumButtonWidth = 136;
constexpr size_t kMaxSheets = 48;           // stylesheets one page can make us fetch
constexpr size_t kMaxImages = 256;
constexpr size_t kImageWorkers = 4;
constexpr uint64_t kMaxImagePixels = 40'000'000;     // one image
constexpr uint64_t kMaxPagePixels = 120'000'000;     // all of a page's, about 480 MB
constexpr uint64_t kMaxRasterCachePixels = 32'000'000;   // SVG, gradients and masks we've drawn, about 128 MB
constexpr size_t kMaxLocalFile = 32u << 20;
constexpr auto kRelayoutEvery = std::chrono::milliseconds(200);
constexpr size_t kFontWorkers = 4;

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

// `cookies`: for a page or the page's own site's files (see kFetchCookies).
Resource load(const std::string& address, bool cookies = false) {
    {
        Resource inline_data;
        if (web::decodeDataUrl(address, inline_data.body, inline_data.content_type)) {
            inline_data.ok = true;
            inline_data.final_url = address;
            return inline_data;
        }
    }
    const web::Url url = web::parseUrl(address);
    if (url.scheme == "file") return loadLocal(url);
    Resource r;
    r.final_url = address;
    if (url.scheme != "http" && url.scheme != "https") {
        r.error = "This address isn't something a browser can open here: " + url.scheme + ":";
        return r;
    }
    // A fragment isn't sent to the server.
    auto got = fetchWebBlocking(url.withoutFragment(), cookies);
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

// A form's POST: only to the web, never to a file.
Resource loadPost(const web::FormSubmission& sub, bool cookies = true) {
    Resource r;
    r.final_url = sub.url;
    const web::Url url = web::parseUrl(sub.url);
    if (url.scheme != "http" && url.scheme != "https") {
        r.error = "This form can't be sent from here.";
        return r;
    }
    auto got = postWebBlocking(url.withoutFragment(), sub.content_type, sub.body, cookies);
    r.http_status = got.http_status;
    if (got.status == fbx::shared::FetchStatus::Ok) {
        r.ok = true;
        r.body = std::move(got.body);
        r.content_type = lowerCase(got.content_type);
        r.final_url = got.final_url;
    } else if (got.status == fbx::shared::FetchStatus::Denied) {
        r.error = "This address isn't allowed.";
    } else if (got.status == fbx::shared::FetchStatus::BadRequest) {
        r.error = "This form is too large to send.";
    } else {
        r.error = "The site can't be reached.";
    }
    return r;
}

// A code point as UTF-8.
std::string utf8Of(char32_t cp) {
    std::string out;
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    return out;
}

// ── A loaded page ─────────────────────────────────────────────────────────────

struct LoadedImage {
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    uint32_t id = 0;
    std::shared_ptr<const web::SvgImage> svg;   // drawn at whatever size it's shown, instead
};

struct Page {
    std::unique_ptr<web::Document> doc;
    std::string url;       // where it really came from
    std::string base;      // what its relative links are relative to
    std::map<std::string, std::string> sheets;   // by absolute URL
    std::map<std::string, std::shared_ptr<LoadedImage>> images;   // by absolute URL, UI thread only
    std::set<std::string> requested;   // images asked for, loaded or not; UI thread only
    std::set<std::string> failed_images;   // asked for and not had; UI thread only
    uint64_t pixels = 0;
    bool fonts_known = false;   // its @font-face rules are in force, and what they need asked for
    web::FormState forms;       // what its controls hold; UI thread only
    std::map<std::string, std::string> scripts;   // external scripts' text, by absolute URL
    std::string charset;   // what the page itself was in (empty: UTF-8, as nothing said otherwise)
    std::set<std::string> sheets_requested;   // stylesheets asked for after it was shown; UI thread only
    std::unique_ptr<web::PageScript> script;      // its scripts' world; UI thread only. Last: it refers to doc
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
            const std::string* as = n->attr("as");
            // Stylesheets, and styles preloaded to become one (loadCSS: samsung.com/sec).
            if (rel && href && (lowerCase(*rel).find("stylesheet") != std::string::npos ||
                                (lowerCase(*rel).find("preload") != std::string::npos && as && lowerCase(*as) == "style")))
                queue.push_back(web::resolveUrl(page.base, *href));
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
        Resource got = load(url, web::sameSite(page.url, url));
        std::string css = got.ok ? web::decodeToUtf8(got.body, got.content_type) : std::string{};
        for (const std::string& ref : web::css::parseStylesheet(css).imports) queue.push_back(web::resolveUrl(url, ref));
        page.sheets[url] = std::move(css);
    }
}

// The page's external scripts, fetched with its stylesheets, so they run in order;
// and its modules with everything they import, so the graph is there when they run.
void loadScripts(Page& page, const std::atomic<uint64_t>& generation, uint64_t mine) {
    constexpr size_t kMaxScripts = 100;
    constexpr size_t kMaxModules = 400;
    constexpr size_t kMaxScriptBytes = 16 << 20;   // YouTube's app is 10.8 MB (parsing takes ~50x its size in memory)
    constexpr size_t kMaxTotalBytes = 64 << 20;
    std::vector<const web::Node*> todo{page.doc->root()};
    std::vector<std::string> urls;
    std::map<std::string, std::string> charsets;   // a script's charset attribute, by URL
    std::deque<std::pair<std::string, std::string>> modules;   // (its URL, its source if inline)
    web::js::ImportMap import_map;
    while (!todo.empty() && urls.size() < kMaxScripts) {
        const web::Node* n = todo.back();
        todo.pop_back();
        if (n->isHtml("script")) {
            const std::string* type = n->attr("type");
            const std::string t = type ? lowerCase(*type) : "";
            const std::string* src = n->attr("src");
            if (t == "importmap") {
                import_map.add(n->textContent(), page.base);
            } else if (t == "module") {
                if (src) modules.emplace_back(web::resolveUrl(page.base, *src), std::string{});
                else modules.emplace_back(std::string{}, n->textContent());
            } else if (src && !n->attr("nomodule") && (t.empty() || t.find("javascript") != std::string::npos || t == "text/ecmascript")) {
                urls.push_back(web::resolveUrl(page.base, *src));
                if (const std::string* cs = n->attr("charset")) charsets[urls.back()] = *cs;
            }
        }
        todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
    }
    size_t total = 0;
    const auto fetch = [&](const std::string& url) -> const std::string* {
        if (const auto it = page.scripts.find(url); it != page.scripts.end()) return &it->second;
        if (total > kMaxTotalBytes) return nullptr;
        Resource got = load(url, web::sameSite(page.url, url));
        if (!got.ok || got.http_status >= 400 || got.body.size() > kMaxScriptBytes) return nullptr;
        total += got.body.size();
        // A script that doesn't say is in its charset attribute's encoding, else the page's
        // (yes24's EUC-KR scripts).
        std::string type = got.content_type;
        if (web::detectCharset({}, type).empty()) {
            const auto it = charsets.find(url);
            const std::string cs = it != charsets.end() ? it->second : page.charset;
            type = "text/javascript; charset=" + (cs.empty() ? std::string("utf-8") : cs);
        }
        return &(page.scripts[url] = web::decodeToUtf8(got.body, type));
    };
    for (const std::string& url : urls) {
        if (generation.load() != mine) return;
        fetch(url);
    }
    // Breadth first through the module graph.
    std::set<std::string> seen;
    size_t fetched = 0;
    while (!modules.empty() && fetched < kMaxModules && generation.load() == mine) {
        auto [url, inline_source] = modules.front();
        modules.pop_front();
        const std::string* source = &inline_source;
        std::string base = url.empty() ? page.url : url;
        if (!url.empty()) {
            if (!seen.insert(url).second) continue;
            ++fetched;
            source = fetch(url);
            if (!source) continue;
        }
        for (const std::string& spec : web::js::moduleRequests(*source)) {
            const std::string dep = import_map.resolve(spec, base);
            if (!dep.empty() && !seen.count(dep)) modules.emplace_back(dep, std::string{});
        }
    }
}

// Text for a 2D canvas in a CSS font, through FreeType: glyph by glyph, unshaped, each
// from the first face that has it, at `scale` times its size. Its own FT_Size, so the
// faces the page's text is drawn with keep theirs.
std::optional<web::PageScript::Host::CanvasText> canvasText(const std::string& text, const std::string& font, float scale, bool render) {
    using fbx::xrender::FontFace;
    using fbx::xrender::FontManager;
    using fbx::xrender::FontStyle;
    using fbx::xrender::FontWeight;
    // The font shorthand: style and weight words, a size, maybe /line-height, families.
    float size = 10;
    FontWeight weight = FontWeight::Regular;
    FontStyle style = FontStyle::Normal;
    std::vector<std::string> families;
    {
        std::vector<std::string> words;
        std::string cur;
        char quote = 0;
        for (const char c : font) {
            if (quote) { if (c == quote) quote = 0; else cur += c; continue; }
            if (c == '"' || c == '\'') { quote = c; continue; }
            if (c == ' ' || c == ',') { if (!cur.empty()) words.push_back(cur), cur.clear(); if (c == ',') words.push_back(","); continue; }
            cur += c;
        }
        if (!cur.empty()) words.push_back(cur);
        size_t i = 0;
        for (; i < words.size(); ++i) {
            const std::string w = lowerCase(words[i]);
            if (w == "italic" || w == "oblique") style = w == "italic" ? FontStyle::Italic : FontStyle::Oblique;
            else if (w == "bold" || w == "bolder") weight = FontWeight::Bold;
            else if (w == "lighter") weight = FontWeight::Light;
            else if (w.size() == 3 && std::isdigit(static_cast<unsigned char>(w[0])) && w.substr(1) == "00") weight = static_cast<FontWeight>(std::stoi(w));
            else if (std::isdigit(static_cast<unsigned char>(w[0])) || w[0] == '.') {
                char* end = nullptr;
                const float v = std::strtof(w.c_str(), &end);
                const std::string unit = std::string(end).substr(0, std::string(end).find('/'));
                size = unit == "pt" ? v * 4 / 3 : unit == "em" || unit == "rem" ? v * 16 : unit == "%" ? v * 0.16f : v;
                ++i;
                break;
            }
        }
        std::string fam;
        for (; i < words.size(); ++i) {
            if (words[i] == ",") { if (!fam.empty()) families.push_back(fam), fam.clear(); continue; }
            if (words[i].starts_with("/")) continue;
            fam += (fam.empty() ? "" : " ") + words[i];
        }
        if (!fam.empty()) families.push_back(fam);
    }
    size = std::clamp(size, 1.0f, 512.0f);
    std::string family;
    if (!families.empty()) {
        const std::string f = lowerCase(families.front());
        family = f == "sans-serif" || f == "system-ui" || f == "-apple-system" || f == "blinkmacsystemfont" ? std::string() : families.front();
    }
    FontFace* primary = FontManager::getFace(family, weight, style);
    if (!primary || !primary->valid()) primary = FontManager::getFace("", weight, style);
    if (!primary || !primary->valid()) return std::nullopt;
    // Fallbacks: CJK text first (most pages' fonts lack it), then symbols.
    FontFace* fallback = FontManager::getCjkFace();
    FontFace* symbols = FontManager::getLastResortFace();
    const uint32_t px = static_cast<uint32_t>(std::clamp(std::lround(size * scale), 1L, 1024L));

    struct Sized {
        FT_Face face = nullptr;
        FT_Size mine = nullptr, theirs = nullptr;
        ~Sized() {
            if (mine) { FT_Activate_Size(theirs); FT_Done_Size(mine); }
        }
        bool use(FT_Face f, uint32_t pixels) {
            face = f;
            theirs = f->size;
            if (FT_New_Size(f, &mine) != 0) { mine = nullptr; return false; }
            FT_Activate_Size(mine);
            return FT_Set_Pixel_Sizes(f, 0, pixels) == 0;
        }
    };
    Sized a, b, sym;
    if (!a.use(primary->ft_face_, px)) return std::nullopt;
    const bool have_fallback = fallback && fallback->valid() && fallback->ft_face_ != primary->ft_face_ && b.use(fallback->ft_face_, px);
    const bool have_symbols = symbols && symbols->valid() && symbols->ft_face_ != primary->ft_face_ &&
                              (!have_fallback || symbols->ft_face_ != fallback->ft_face_) && sym.use(symbols->ft_face_, px);
    const float ascent = static_cast<float>(primary->ft_face_->size->metrics.ascender) / 64.0f;
    const float descent = static_cast<float>(-primary->ft_face_->size->metrics.descender) / 64.0f;

    struct Glyph { std::vector<uint8_t> coverage; int w = 0, h = 0, left = 0, top = 0; float pen = 0; };
    std::vector<Glyph> glyphs;
    float pen = 0;
    FT_UInt previous = 0;
    FT_Face previous_face = nullptr;
    // UTF-8 to code points.
    for (size_t i = 0; i < text.size() && glyphs.size() < 4096;) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        uint32_t cp = n == 1 ? c : c & (0x7F >> n);
        for (int k = 1; k < n && i + k < text.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        i += static_cast<size_t>(n);
        FT_Face face = a.face;
        FT_UInt index = FT_Get_Char_Index(face, cp);
        if (!index && have_fallback) {
            const FT_UInt other = FT_Get_Char_Index(b.face, cp);
            if (other) face = b.face, index = other;
        }
        if (!index && have_symbols) {
            const FT_UInt other = FT_Get_Char_Index(sym.face, cp);
            if (other) face = sym.face, index = other;
        }
        if (previous && face == previous_face && FT_HAS_KERNING(face)) {
            FT_Vector k;
            if (FT_Get_Kerning(face, previous, index, FT_KERNING_DEFAULT, &k) == 0) pen += static_cast<float>(k.x) / 64.0f;
        }
        if (FT_Load_Glyph(face, index, render ? FT_LOAD_RENDER : FT_LOAD_DEFAULT) != 0) continue;
        const FT_GlyphSlot slot = face->glyph;
        if (render && slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY && slot->bitmap.width > 0) {
            Glyph g;
            g.w = static_cast<int>(slot->bitmap.width);
            g.h = static_cast<int>(slot->bitmap.rows);
            g.left = slot->bitmap_left;
            g.top = slot->bitmap_top;
            g.pen = pen;
            g.coverage.resize(static_cast<size_t>(g.w) * g.h);
            for (int row = 0; row < g.h; ++row)
                std::memcpy(&g.coverage[static_cast<size_t>(row) * g.w], slot->bitmap.buffer + static_cast<ptrdiff_t>(row) * slot->bitmap.pitch, static_cast<size_t>(g.w));
            glyphs.push_back(std::move(g));
        }
        pen += static_cast<float>(slot->advance.x) / 64.0f;
        previous = index;
        previous_face = face;
    }
    web::PageScript::Host::CanvasText out;
    out.advance = pen / scale;
    out.ascent = ascent / scale;
    out.descent = descent / scale;
    if (render) {
        web::Raster& m = out.mask;
        m.width = static_cast<uint32_t>(std::clamp(std::ceil(pen) + 2, 1.0f, 8192.0f));
        m.height = static_cast<uint32_t>(std::clamp(std::ceil(ascent + descent) + 1, 1.0f, 2048.0f));
        if (static_cast<uint64_t>(m.width) * m.height > web::kMaxRasterPixels) return out;
        m.rgba.assign(static_cast<size_t>(m.width) * m.height * 4, 0);
        for (size_t p = 0; p < m.rgba.size(); p += 4) m.rgba[p] = m.rgba[p + 1] = m.rgba[p + 2] = 255;
        for (const Glyph& g : glyphs) {
            const int ox = static_cast<int>(std::lround(g.pen)) + g.left, oy = static_cast<int>(std::lround(ascent)) - g.top;
            for (int row = 0; row < g.h; ++row) {
                const int y = oy + row;
                if (y < 0 || y >= static_cast<int>(m.height)) continue;
                for (int col = 0; col < g.w; ++col) {
                    const int x = ox + col;
                    if (x < 0 || x >= static_cast<int>(m.width)) continue;
                    uint8_t& alpha = m.rgba[(static_cast<size_t>(y) * m.width + x) * 4 + 3];
                    alpha = std::max(alpha, g.coverage[static_cast<size_t>(row) * g.w + col]);
                }
            }
        }
    }
    return out;
}

std::vector<std::string> imageSources(const Page& page) {
    std::vector<std::string> out;
    int win_w = 0, win_h = 0;
    XApplication::windowSize(win_w, win_h);
    const web::css::MediaContext viewport{win_w > 0 ? static_cast<float>(win_w) : 1000, win_h > 0 ? static_cast<float>(win_h) - kToolbarHeight : 800};
    std::set<std::string> seen;
    std::vector<const web::Node*> todo{page.doc->root()};
    while (!todo.empty() && out.size() < kMaxImages) {
        const web::Node* n = todo.back();
        todo.pop_back();
        if (n->isHtml("img"))
            if (const auto src = web::imageSourceOf(n, viewport)) {
                std::string url = web::resolveUrl(page.base, *src);
                if (seen.insert(url).second) out.push_back(std::move(url));
            }
        todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
    }
    return out;
}

std::optional<LoadedImage> decodeImage(const std::string& url, const std::string& bytes, const std::string& type) {
    if (web::SvgImage::sniff(bytes, type, url)) {
        // Parsed here, drawn later at the size it turns out to be shown.
        auto svg = web::SvgImage::parse(bytes);
        if (!svg) return std::nullopt;
        LoadedImage image;
        image.svg = std::move(svg);
        return image;
    }
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
        if (image->svg) {
            const web::SvgSize& s = image->svg->size();
            if (s.width && s.height) return std::make_pair(*s.width, *s.height);
            if (s.width && s.ratio > 0) return std::make_pair(*s.width, *s.width / s.ratio);
            if (s.height && s.ratio > 0) return std::make_pair(*s.height * s.ratio, *s.height);
            if (s.ratio > 0) return std::nullopt;   // a shape only: see imageRatio
            return std::make_pair(300.0f, 150.0f);
        }
        return std::make_pair(static_cast<float>(image->width), static_cast<float>(image->height));
    }

    bool imageLoading(const std::string& src) override {
        if (!page_) return false;
        const std::string url = web::resolveUrl(page_->base, src);
        return !page_->images.count(url) && !page_->failed_images.count(url) && page_->requested.size() < kMaxImages;
    }

    std::optional<float> imageRatio(const std::string& src) override {
        const LoadedImage* image = find(src);
        if (!image || !image->svg) return std::nullopt;
        const web::SvgSize& s = image->svg->size();
        if (s.width || s.height || s.ratio <= 0) return std::nullopt;
        return s.ratio;
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

    // Told once a page, at its first layout, what fonts its text calls for.
    void onFontsWanted(std::function<void(const std::shared_ptr<Page>&, std::vector<web::WebFontFace>)> f) { on_fonts_ = std::move(f); }
    // Told after a layout what images its styles call for that haven't been asked for.
    void onImagesWanted(std::function<void(const std::shared_ptr<Page>&, std::vector<std::string>)> f) { on_images_ = std::move(f); }
    void onSheetsWanted(std::function<void(const std::shared_ptr<Page>&, std::vector<std::string>)> f) { on_sheets_ = std::move(f); }

    void show(std::shared_ptr<Page> page) {
        popup_ = nullptr;
        page_ = std::move(page);
        images_.setPage(page_.get());
        rasters_.clear();
        canvas_images_.clear();   // another page's nodes
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
        ++paint_frame_;
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
            if (op.kind == web::PaintKind::TransformBegin) {
                // Drawn as one image, unless it holds text: then as if untransformed.
                const size_t end = transformEnd(i);
                if (drawTransformed(dl, i, end, at)) i = end;
                continue;
            }
            const float y = op.y + at.y_;
            if (y > kMaxWindowHeight + kOffscreenMargin || y + op.h < -kOffscreenMargin) continue;
            switch (op.kind) {
                case web::PaintKind::Rect: fill(dl, op.x + at.x_, y, op.w, op.h, op.color, op.radius); break;
                case web::PaintKind::Text: text(dl, op, op.x + at.x_, text_y_[i] + at.y_); break;
                case web::PaintKind::Image: image(dl, op, op.x + at.x_, y); break;
                case web::PaintKind::Gradient: gradient(dl, op, op.x + at.x_, y); break;
                case web::PaintKind::Svg: inlineSvg(dl, op, op.x + at.x_, y); break;
                case web::PaintKind::Box: boxPaint(dl, op, op.x + at.x_, y); break;
                default: break;
            }
        }
        if (!clips.empty()) {
            clips.clear();
            applyClip();
        }
        if (popup_) paintPopup(dl, at);
    }

    [[nodiscard]] std::string visibleText() const override { return layout_.title; }
    [[nodiscard]] const std::string& title() const { return layout_.title; }

    // ── Form controls ──

    // The control at a point: one that's there, or what a label there labels.
    [[nodiscard]] const web::Node* controlAt(float page_x, float page_y) const {
        for (auto it = layout_.boxes.rbegin(); it != layout_.boxes.rend(); ++it) {
            if (page_x < it->x || page_x >= it->x + it->w || page_y < it->y || page_y >= it->y + it->h) continue;
            for (const web::Node* n = it->node; n; n = n->parent) {
                const web::ControlKind k = web::controlKind(n);
                if (k != web::ControlKind::None && k != web::ControlKind::Hidden) return web::isDisabled(n) ? nullptr : n;
                if (n->isHtml("label")) {
                    const web::Node* c = web::labelledControl(n);
                    return c && !web::isDisabled(c) ? c : nullptr;
                }
                if (n->isHtml("a")) return nullptr;   // a link's own
            }
            return nullptr;
        }
        return nullptr;
    }

    // The element a click at this point is on: the innermost box, or the link inside it.
    [[nodiscard]] web::Node* nodeAt(float page_x, float page_y) const {
        const web::Node* hit = nullptr;
        for (auto it = layout_.boxes.rbegin(); it != layout_.boxes.rend(); ++it)
            if (page_x >= it->x && page_x < it->x + it->w && page_y >= it->y && page_y < it->y + it->h) {
                hit = it->node;
                break;
            }
        if (const std::string* href = linkAt(page_x, page_y); href && hit) {
            std::vector<const web::Node*> todo{hit};
            while (!todo.empty()) {
                const web::Node* n = todo.back();
                todo.pop_back();
                if (n->isHtml("a"))
                    if (const std::string* h = n->attr("href"); h && *h == *href) return const_cast<web::Node*>(n);
                todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
            }
        }
        return const_cast<web::Node*>(hit);
    }

    // A few computed values, for getComputedStyle.
    // Laid out again now, at the size it was: a script reading styles or boxes after
    // changing the page sees the change, as browsers recompute when asked.
    void refresh() {
        if (laid_out_width_ > 0) relayout(laid_out_width_, laid_out_height_);
    }

    [[nodiscard]] std::unordered_map<std::string, std::string> computedStyleOf(const web::Node* n) const {
        std::unordered_map<std::string, std::string> out;
        const web::ComputedStyle* s = styles_ ? styles_->styleOf(n) : nullptr;
        // Not rendered (display: none, or inside one): every property still has a value.
        static const web::ComputedStyle kUnrendered = [] {
            web::ComputedStyle u;
            u.display = web::Display::None;
            return u;
        }();
        if (!s) s = &kUnrendered;
        static const char* kDisplay[] = {"none", "inline", "block", "inline-block", "list-item", "flow-root", "table", "inline-table",
                                         "table-caption", "table-row-group", "table-row", "table-cell", "table-column", "flex",
                                         "inline-flex", "grid", "inline-grid", "contents"};
        const auto idx = static_cast<size_t>(s->display);
        out["display"] = idx < std::size(kDisplay) ? kDisplay[idx] : "block";
        out["visibility"] = s->hidden ? "hidden" : "visible";
        static const char* kPosition[] = {"static", "relative", "absolute", "fixed"};
        out["position"] = kPosition[static_cast<size_t>(s->position)];
        const auto rgb = [](web::Color c) {
            if (c.a == 255) return "rgb(" + std::to_string(c.r) + ", " + std::to_string(c.g) + ", " + std::to_string(c.b) + ")";
            char a[16];
            std::snprintf(a, sizeof a, "%g", c.a / 255.0);
            return "rgba(" + std::to_string(c.r) + ", " + std::to_string(c.g) + ", " + std::to_string(c.b) + ", " + a + ")";
        };
        out["color"] = rgb(s->color);
        out["background-color"] = rgb(s->background);
        const auto px = [](float v) {
            char b[32];
            std::snprintf(b, sizeof b, "%gpx", v);
            return std::string(b);
        };
        out["font-size"] = px(s->font_size);
        out["font-weight"] = std::to_string(s->font_weight);
        out["font-family"] = s->font_family;
        out["opacity"] = std::to_string(s->opacity).substr(0, 4);
        out["z-index"] = std::to_string(s->z_index);
        // Lengths as specified: px, or the percentage when that's what was said.
        const auto len = [&px](const web::Length& l) {
            if (l.is_auto) return std::string("auto");
            if (l.hasPercent() && l.px == 0) {
                char b[32];
                std::snprintf(b, sizeof b, "%g%%", l.percent);
                return std::string(b);
            }
            return px(l.px);
        };
        static const char* kSide[] = {"top", "right", "bottom", "left"};
        static const char* kBorderStyle[] = {"none", "hidden", "solid", "dashed", "dotted", "double", "groove", "ridge", "inset", "outset"};
        for (int i = 0; i < 4; ++i) {
            out[std::string("margin-") + kSide[i]] = s->margin[i].is_auto ? std::string("0px") : len(s->margin[i]);
            out[std::string("padding-") + kSide[i]] = len(s->padding[i]);
            out[std::string("border-") + kSide[i] + "-width"] = px(s->border_width[i]);
            out[std::string("border-") + kSide[i] + "-style"] = kBorderStyle[static_cast<size_t>(s->border_style[i])];
            out[std::string("border-") + kSide[i] + "-color"] = rgb(s->border_color[i]);
            out[kSide[i]] = len(s->offset[i]);
        }
        out["box-sizing"] = s->border_box ? "border-box" : "content-box";
        out["float"] = s->float_side == web::Float::Left ? "left" : s->float_side == web::Float::Right ? "right" : "none";
        out["overflow"] = s->overflow_clip ? "hidden" : "visible";
        out["line-height"] = s->line_height > 0 ? px(s->line_height) : std::string("normal");
        out["min-width"] = len(s->min_width);
        out["min-height"] = len(s->min_height);
        out["max-width"] = s->max_width.is_auto ? std::string("none") : len(s->max_width);
        out["max-height"] = s->max_height.is_auto ? std::string("none") : len(s->max_height);
        static const char* kWhiteSpace[] = {"normal", "pre", "nowrap", "pre-wrap", "pre-line"};
        out["white-space"] = kWhiteSpace[static_cast<size_t>(s->white_space)];
        static const char* kTextAlign[] = {"start", "right", "center"};
        out["text-align"] = kTextAlign[static_cast<size_t>(s->text_align)];
        static const char* kTransform[] = {"none", "uppercase", "lowercase", "capitalize"};
        out["text-transform"] = kTransform[static_cast<size_t>(s->text_transform)];
        out["text-indent"] = s->text_indent.is_auto ? std::string("0px") : len(s->text_indent);
        out["font-style"] = s->italic ? "italic" : "normal";
        out["letter-spacing"] = "normal";
        out["flex-direction"] = std::string(s->flex_column ? "column" : "row") + (s->flex_reverse ? "-reverse" : "");
        out["flex-wrap"] = s->flex_wrap ? "wrap" : "nowrap";
        static const char* kAlign[] = {"normal", "flex-start", "flex-end", "center", "stretch", "baseline", "space-between", "space-around", "space-evenly"};
        out["justify-content"] = kAlign[static_cast<size_t>(s->justify_content)];
        out["align-items"] = s->align_items == web::FlexAlign::Auto ? "normal" : kAlign[static_cast<size_t>(s->align_items)];
        out["align-self"] = s->align_self == web::FlexAlign::Auto ? "auto" : kAlign[static_cast<size_t>(s->align_self)];
        const auto num = [](float v) {
            char b[32];
            std::snprintf(b, sizeof b, "%g", v);
            return std::string(b);
        };
        out["flex-grow"] = num(s->flex_grow);
        out["flex-shrink"] = num(s->flex_shrink);
        out["flex-basis"] = len(s->flex_basis);
        if (const web::ElementBox* b = boxOf(n)) {
            // The used size: its content box, unless it sizes its border box.
            float w = b->w, h = b->h;
            if (!s->border_box) {
                for (int i : {1, 3}) w -= s->border_width[i] + (s->padding[i].hasPercent() ? 0 : s->padding[i].px);
                for (int i : {0, 2}) h -= s->border_width[i] + (s->padding[i].hasPercent() ? 0 : s->padding[i].px);
            }
            out["width"] = px(std::max(0.0f, w));
            out["height"] = px(std::max(0.0f, h));
        } else {
            out["width"] = out["height"] = "auto";
        }
        return out;
    }

    [[nodiscard]] const web::ElementBox* boxOf(const web::Node* n) const {
        for (const web::ElementBox& b : layout_.boxes)
            if (b.node == n) return &b;
        return nullptr;
    }

    // Where in a field's text a click at this x falls: the nearest character boundary.
    [[nodiscard]] size_t caretAt(const web::Node* control, float page_x) {
        const web::ElementBox* b = boxOf(control);
        const web::ComputedStyle* st = styles_ ? styles_->styleOf(control) : nullptr;
        if (!b || !st || !page_) return 0;
        const std::string value = page_->forms.value(control);
        const web::FontSpec font = fontFor(*st);
        const float x0 = b->x + st->border_width[web::Left] + st->padding[web::Left].resolve(0);
        size_t best = 0;
        float best_d = std::abs(page_x - x0);
        for (size_t i = 0; i < value.size();) {
            size_t j = i + 1;
            while (j < value.size() && (static_cast<unsigned char>(value[j]) & 0xC0) == 0x80) ++j;
            if (value[i] == '\n') break;
            const float d = std::abs(page_x - x0 - measurer_.width(std::string_view(value).substr(0, j), font));
            if (d < best_d) {
                best_d = d;
                best = j;
            }
            i = j;
        }
        return best;
    }

    // The list a <select> opens, drawn over the page.
    void openSelect(const web::Node* select) {
        popup_ = select;
        popup_highlight_ = page_ ? page_->forms.selected(select) : -1;
        popup_first_ = std::max(0, popup_highlight_ - kPopupRows + 1);
        dirty_paint_ = true;
    }
    void closeSelect() { popup_ = nullptr; }
    [[nodiscard]] const web::Node* openPopup() const { return popup_; }
    // Which option a point is on, or -1; -2 if it's not on the list at all.
    [[nodiscard]] int popupRowAt(float page_x, float page_y) const {
        const PopupGeometry g = popupGeometry();
        if (!g.rows || page_x < g.x || page_x >= g.x + g.w || page_y < g.y || page_y >= g.y + g.h) return -2;
        const int row = static_cast<int>((page_y - g.y) / g.row_h);
        return row < g.rows ? popup_first_ + row : -1;
    }
    void movePopupHighlight(int delta) {
        if (!popup_) return;
        const int count = static_cast<int>(web::optionsOf(popup_).size());
        if (count == 0) return;
        popup_highlight_ = std::clamp(popup_highlight_ + delta, 0, count - 1);
        if (popup_highlight_ < popup_first_) popup_first_ = popup_highlight_;
        if (popup_highlight_ >= popup_first_ + kPopupRows) popup_first_ = popup_highlight_ - kPopupRows + 1;
    }
    [[nodiscard]] int popupHighlight() const { return popup_highlight_; }

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
    static constexpr int kPopupRows = 12;

    [[nodiscard]] web::FontSpec fontFor(const web::ComputedStyle& st) const {
        web::FontSpec f;
        f.size = st.font_size;
        f.weight = st.font_weight;
        f.italic = st.italic;
        f.family = st.font_family;
        return f;
    }

    struct PopupGeometry {
        float x = 0, y = 0, w = 0, h = 0, row_h = 0;
        int rows = 0;
        web::FontSpec font;
    };
    [[nodiscard]] PopupGeometry popupGeometry() const {
        PopupGeometry g;
        if (!popup_) return g;
        const web::ElementBox* b = nullptr;
        for (const web::ElementBox& e : layout_.boxes)
            if (e.node == popup_) b = &e;
        const web::ComputedStyle* st = styles_ ? styles_->styleOf(popup_) : nullptr;
        if (!b || !st) return g;
        g.font = fontFor(*st);
        g.row_h = std::ceil(st->font_size * 1.4f);
        const int count = static_cast<int>(web::optionsOf(popup_).size());
        g.rows = std::min(count - popup_first_, kPopupRows);
        if (g.rows <= 0) return g;
        g.x = b->x;
        g.y = b->y + b->h;
        g.w = b->w;
        g.h = g.row_h * static_cast<float>(g.rows) + 2;
        return g;
    }

    void paintPopup(fbx::xrender::DisplayList& dl, Offset at) {
        PopupGeometry g = popupGeometry();
        if (!g.rows) return;
        const std::vector<const web::Node*> options = web::optionsOf(popup_);
        for (int i = 0; i < g.rows; ++i)
            g.w = std::max(g.w, measurer_.width(web::optionLabel(options[static_cast<size_t>(popup_first_ + i)]), g.font) + 16);
        const float x = g.x + at.x_, y = g.y + at.y_;
        fill(dl, x, y, g.w, g.h, {0x76, 0x76, 0x76, 255});
        fill(dl, x + 1, y + 1, g.w - 2, g.h - 2, {255, 255, 255, 255});
        for (int i = 0; i < g.rows; ++i) {
            const int index = popup_first_ + i;
            const float ry = y + 1 + g.row_h * static_cast<float>(i);
            const bool lit = index == popup_highlight_;
            const bool disabled = options[static_cast<size_t>(index)]->attr("disabled") != nullptr;
            if (lit) fill(dl, x + 1, ry, g.w - 2, g.row_h, {0x00, 0x75, 0xff, 255});
            web::PaintOp op;
            op.kind = web::PaintKind::Text;
            op.font = g.font;
            op.text = web::optionLabel(options[static_cast<size_t>(index)]);
            op.color = lit ? web::Color{255, 255, 255, 255} : disabled ? web::Color{0x99, 0x99, 0x99, 255} : web::Color{0, 0, 0, 255};
            const float baseline_top = ry + (g.row_h - (measurer_.ascent(g.font) + measurer_.descent(g.font))) / 2;
            text(dl, op, x + 6, baseline_top + measurer_.ascent(g.font) - 0.8f * g.font.size);
        }
    }

    [[nodiscard]] float viewportTop() const { return kToolbarHeight; }

    RenderNode* parentNode() const { return parent_.lock().get(); }

    void relayout(float width, float height) {
        if (!page_ || !page_->doc) {
            layout_ = {};
            text_y_.clear();
            return;
        }
        const Page& page = *page_;
        // A stylesheet linked since the page came (a script's, or a preload turned into one)
        // is fetched now and the page styled again when it's here.
        std::vector<std::string> missing_sheets;
        styles_ = std::make_unique<web::StyledDocument>(
            *page.doc, web::css::MediaContext{width, height},
            [&page, &missing_sheets](const std::string& href) {
                std::string url = web::resolveUrl(page.base, href);
                const auto it = page.sheets.find(url);
                if (it == page.sheets.end()) {
                    if (!page.sheets_requested.count(url)) missing_sheets.push_back(std::move(url));
                    return std::string{};
                }
                return it->second;
            },
            [this](const web::ComputedStyle& s) { return web::chWidth(measurer_, s); }, &page_->forms);
        const web::StyledDocument& styles = *styles_;
        if (!page.fonts_known) {
            page_->fonts_known = true;
            std::vector<web::WebFontFace> faces = styles.fontFaces();
            // Those in a <style> are still relative to the page.
            for (web::WebFontFace& f : faces)
                for (std::string& src : f.sources) src = web::resolveUrl(page.base, src);
            web::setPageWebFonts(std::move(faces));
            if (on_fonts_) on_fonts_(page_, web::wantedWebFonts(*page.doc, styles));
        }
        layout_ = web::layoutPage(*page.doc, styles, measurer_, &images_, width, height, &page_->forms);
        ++layout_gen_;
        std::vector<std::string> wanted;
        for (const std::string& src : layout_.images_wanted) {
            std::string url = web::resolveUrl(page.base, src);
            if (!page_->requested.count(url)) wanted.push_back(std::move(url));
        }
        if (!wanted.empty() && on_images_) on_images_(page_, std::move(wanted));
        if (!missing_sheets.empty() && on_sheets_) {
            for (const std::string& url : missing_sheets) page_->sheets_requested.insert(url);
            on_sheets_(page_, std::move(missing_sheets));
        }
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

    // One line's text, in the fonts it takes: usually there's only one.
    void text(fbx::xrender::DisplayList& dl, const web::PaintOp& op, float x, float y) {
        const std::vector<web::FontRun> runs = web::fontRuns(op.text, op.font);
        for (const web::FontRun& run : runs) {
            const std::string_view piece = std::string_view(op.text).substr(run.begin, run.end - run.begin);
            textRun(dl, op, piece, run.family, x, y);
            if (runs.size() > 1) x += measurer_.width(piece, op.font);
        }
    }

    static void textRun(fbx::xrender::DisplayList& dl, const web::PaintOp& op, std::string_view piece, const std::string& family, float x, float y) {
        fbx::shared::TextData td{};
        td.x_ = x;
        td.y_ = y;
        td.font_size_ = op.font.size;
        td.r_ = static_cast<float>(op.color.r) / 255;
        td.g_ = static_cast<float>(op.color.g) / 255;
        td.b_ = static_cast<float>(op.color.b) / 255;
        td.a_ = static_cast<float>(op.color.a) / 255;
        td.string_length_ = static_cast<uint32_t>(piece.size());
        td.font_weight_ = static_cast<uint16_t>(std::clamp((op.font.weight + 50) / 100 * 100, 100, 900));
        td.font_style_ = static_cast<uint8_t>(op.font.italic ? fbx::xrender::FontStyle::Italic : fbx::xrender::FontStyle::Normal);
        const size_t n = std::min(family.size(), sizeof(td.font_family_) - 1);
        std::memcpy(td.font_family_, family.data(), n);
        td.font_family_[n] = '\0';
        std::vector<uint8_t> payload(sizeof(td) + piece.size());
        std::memcpy(payload.data(), &td, sizeof(td));
        std::memcpy(payload.data() + sizeof(td), piece.data(), piece.size());
        dl.addCommand(fbx::shared::CommandType::DrawText, payload);
    }

    // Rasters we've drawn on the CPU (SVG, gradients, tinted masks), by what they show.
    struct CachedRaster {
        web::Raster raster;
        uint32_t id = 0;
        uint64_t used = 0;
        uint64_t frame = 0;   // the last paint that put its pixels in a display list
    };

    // How many pixels to draw something shown this big: all of them, within limits.
    static std::pair<uint32_t, uint32_t> rasterSize(float w, float h) {
        double pw = std::max(1.0, std::round(static_cast<double>(w))), ph = std::max(1.0, std::round(static_cast<double>(h)));
        const double side = static_cast<double>(web::kMaxRasterSide);
        const double fit = std::min({1.0, side / pw, side / ph, std::sqrt(static_cast<double>(web::kMaxRasterPixels) / (pw * ph))});
        return {static_cast<uint32_t>(std::max(1.0, std::floor(pw * fit))), static_cast<uint32_t>(std::max(1.0, std::floor(ph * fit)))};
    }

    const CachedRaster* cached(const std::string& key, const std::function<web::Raster()>& draw) {
        ++use_clock_;
        if (auto it = rasters_.find(key); it != rasters_.end()) {
            it->second.used = use_clock_;
            it->second.frame = paint_frame_;
            return it->second.raster.empty() ? nullptr : &it->second;
        }
        CachedRaster c;
        c.raster = draw();
        c.id = nextImageId();
        c.used = use_clock_;
        c.frame = paint_frame_;
        raster_pixels_ += static_cast<uint64_t>(c.raster.width) * c.raster.height;
        const CachedRaster& kept = rasters_[key] = std::move(c);
        // Past the budget, the longest unused go; never one this paint has already handed
        // to the display list, whose pixels the compositor has yet to read.
        while (raster_pixels_ > kMaxRasterCachePixels && rasters_.size() > 1) {
            auto oldest = rasters_.end();
            for (auto it = rasters_.begin(); it != rasters_.end(); ++it)
                if (it->second.frame != paint_frame_ && (oldest == rasters_.end() || it->second.used < oldest->second.used)) oldest = it;
            if (oldest == rasters_.end()) break;
            raster_pixels_ -= static_cast<uint64_t>(oldest->second.raster.width) * oldest->second.raster.height;
            rasters_.erase(oldest);
        }
        return kept.raster.empty() ? nullptr : &kept;
    }

    void drawRaster(fbx::xrender::DisplayList& dl, const CachedRaster& r, float x, float y, float w, float h) {
        if (group_) {
            groupRaster(r.raster, x, y, w, h);
            return;
        }
        fbx::shared::ImageData d{};
        d.x_ = x;
        d.y_ = y;
        d.width_ = w;
        d.height_ = h;
        d.texture_id_ = r.id;
        d.pixel_ptr_ = r.raster.rgba.data();
        d.pixel_width_ = r.raster.width;
        d.pixel_height_ = r.raster.height;
        dl.addCommand(fbx::shared::CommandType::DrawImage, {reinterpret_cast<const uint8_t*>(&d), sizeof(d)});
    }

    static std::string colorKey(web::Color c) {
        return std::to_string((static_cast<uint32_t>(c.r) << 24) | (static_cast<uint32_t>(c.g) << 16) | (static_cast<uint32_t>(c.b) << 8) | c.a);
    }

    static std::string gradientKey(const web::Gradient& g) {
        std::string k = std::to_string(static_cast<int>(g.kind)) + (g.repeating ? "r" : "") + "|" + std::to_string(g.angle) + "|" +
                        std::to_string(g.to_x) + "," + std::to_string(g.to_y) + "|" + std::to_string(g.center_x.px) + "," +
                        std::to_string(g.center_x.percent) + "," + std::to_string(g.center_y.px) + "," + std::to_string(g.center_y.percent) +
                        "|" + (g.circle ? "c" : "e") + std::to_string(static_cast<int>(g.extent)) + (g.explicit_size ? "s" : "") +
                        std::to_string(g.radius_x.px) + "," + std::to_string(g.radius_x.percent) + "," + std::to_string(g.radius_y.px) + "," +
                        std::to_string(g.radius_y.percent) + "|" + std::to_string(g.from);
        for (const web::GradientStop& st : g.stops)
            k += "|" + colorKey(st.color) + (st.has_position ? "@" + std::to_string(st.position.px) + "," + std::to_string(st.position.percent) : "");
        return k;
    }

    void gradient(fbx::xrender::DisplayList& dl, const web::PaintOp& op, float x, float y) {
        if (!op.gradient || op.w <= 0 || op.h <= 0) return;
        const auto [pw, ph] = rasterSize(op.w, op.h);
        const std::string key = "g:" + gradientKey(*op.gradient) + ":" + std::to_string(op.w) + "x" + std::to_string(op.h);
        const web::Gradient& g = *op.gradient;
        const float w = op.w, h = op.h;
        if (const CachedRaster* r = cached(key, [&, pw = pw, ph = ph] { return web::renderGradient(g, w, h, pw, ph); })) drawRaster(dl, *r, x, y, op.w, op.h);
    }

    static std::string boxKey(const web::BoxPaint& b) {
        std::string k = std::to_string(static_cast<int>(b.part));
        const auto f = [&](float v) { k += "," + std::to_string(v); };
        f(b.w); f(b.h); f(b.ox); f(b.oy);
        for (const auto& r : b.radii) { f(r[0]); f(r[1]); }
        for (int i = 0; i < 4; ++i) {
            f(b.border[i]);
            k += ":" + colorKey(b.colors[i]) + ":" + std::to_string(static_cast<int>(b.styles[i]));
        }
        k += "|" + colorKey(b.background);
        f(b.shadow.x); f(b.shadow.y); f(b.shadow.blur); f(b.shadow.spread);
        k += colorKey(b.shadow.color);
        return k;
    }

    void boxPaint(fbx::xrender::DisplayList& dl, const web::PaintOp& op, float x, float y) {
        if (!op.box || op.w <= 0 || op.h <= 0) return;
        // On whole pixels, its fraction moved inside it, so nothing is resampled.
        const float x0 = std::floor(x), y0 = std::floor(y);
        web::BoxPaint box = *op.box;
        box.ox += x - x0;
        box.oy += y - y0;
        const float w = std::ceil(op.w + x - x0), h = std::ceil(op.h + y - y0);
        const auto [pw, ph] = rasterSize(w, h);
        const std::string key = "b:" + boxKey(box) + ":" + std::to_string(pw) + "x" + std::to_string(ph);
        if (const CachedRaster* r = cached(key, [&, pw = pw, ph = ph] { return web::renderBoxPaint(box, w, h, pw, ph); })) drawRaster(dl, *r, x0, y0, w, h);
    }

    void inlineSvg(fbx::xrender::DisplayList& dl, const web::PaintOp& op, float x, float y) {
        if (!op.node || !styles_ || op.w <= 0 || op.h <= 0) return;
        const auto [pw, ph] = rasterSize(op.w, op.h);
        char ptr[32];
        std::snprintf(ptr, sizeof ptr, "%p", static_cast<const void*>(op.node));
        const std::string key = std::string("s:") + ptr + ":" + std::to_string(pw) + "x" + std::to_string(ph) + ":" + colorKey(op.color);
        const web::Node* node = op.node;
        const web::StyledDocument* styles = styles_.get();
        const web::Color color = op.color;
        if (const CachedRaster* r = cached(key, [&, pw = pw, ph = ph] { return web::renderSvg(*node, pw, ph, styles, color); })) drawRaster(dl, *r, x, y, op.w, op.h);
    }

    // A <canvas>'s pixels as its scripts left them: a new image (a new texture) each
    // time they change, the old one let go when it's no longer drawn.
    const LoadedImage* canvasImage(const web::Node* canvas) {
        if (!page_ || !page_->script || !canvas) return nullptr;
        uint64_t version = 0;
        const std::shared_ptr<const web::Raster> raster = page_->script->canvasImage(canvas, version);
        if (!raster || raster->empty()) return nullptr;
        auto& [shown_version, image] = canvas_images_[canvas];
        if (shown_version != version || image.width == 0) {
            shown_version = version;
            image.pixels = raster->rgba;
            image.width = raster->width;
            image.height = raster->height;
            image.id = nextImageId();
        }
        return &image;
    }

    // The part of a picture's pixels an op shows (object-fit: cover crops).
    static web::Raster croppedTo(web::Raster in, const web::PaintOp& op) {
        if (op.u0 <= 0 && op.v0 <= 0 && op.u1 >= 1 && op.v1 >= 1) return in;
        const auto px = [](float f, uint32_t n) { return std::min<uint32_t>(n, static_cast<uint32_t>(std::max(0.0f, std::round(f * static_cast<float>(n))))); };
        const uint32_t x0 = px(op.u0, in.width), x1 = std::max(x0 + 1, px(op.u1, in.width));
        const uint32_t y0 = px(op.v0, in.height), y1 = std::max(y0 + 1, px(op.v1, in.height));
        if (x1 > in.width || y1 > in.height) return in;
        web::Raster out;
        out.width = x1 - x0;
        out.height = y1 - y0;
        out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);
        for (uint32_t row = 0; row < out.height; ++row)
            std::memcpy(&out.rgba[static_cast<size_t>(row) * out.width * 4], &in.rgba[(static_cast<size_t>(y0 + row) * in.width + x0) * 4],
                        static_cast<size_t>(out.width) * 4);
        return out;
    }

    void image(fbx::xrender::DisplayList& dl, const web::PaintOp& op, float x, float y) {
        const LoadedImage* loaded = op.text == web::kCanvasSource ? canvasImage(op.node) : images_.find(op.text);
        if (!loaded) return;
        const bool cropped = op.u0 > 0 || op.v0 > 0 || op.u1 < 1 || op.v1 < 1;
        const bool filtered = op.filter && !op.filter->empty();
        if (loaded->svg || op.tint || op.radius > 0 || filtered || (cropped && group_)) {
            // Drawn for this size (an SVG), or only its alpha in a colour (a mask).
            // An SVG, or rounded corners, at the size shown; a mask at the image's own.
            const auto [pw, ph] = loaded->svg || op.radius > 0 || filtered ? rasterSize(op.w, op.h) : std::pair<uint32_t, uint32_t>{loaded->width, loaded->height};
            char ptr[32];
            std::snprintf(ptr, sizeof ptr, "%p", static_cast<const void*>(loaded));
            std::string filter_key;
            if (filtered)
                for (const web::Filter& f : *op.filter) filter_key += ":f" + std::to_string(static_cast<int>(f.kind)) + "=" + std::to_string(f.amount);
            const std::string crop_key = filter_key + (cropped ? ":c" + std::to_string(op.u0) + "," + std::to_string(op.v0) + "," + std::to_string(op.u1) + "," + std::to_string(op.v1) : std::string());
            const std::string key = std::string("i:") + ptr + ":" + std::to_string(pw) + "x" + std::to_string(ph) + crop_key + (op.tint ? ":" + colorKey(op.color) : "") +
                                    (op.radius > 0 ? ":r" + std::to_string(op.radius) + "," + std::to_string(op.radius_y) + "@" + std::to_string(op.w) + "x" + std::to_string(op.h) : "");
            const bool tint = op.tint;
            const float rx = op.radius, ry = op.radius_y, shown_w = op.w, shown_h = op.h;
            const web::Color color = op.color;
            const CachedRaster* r = cached(key, [&, pw = pw, ph = ph] {
                web::Raster out;
                if (loaded->svg) {
                    if (cropped) {
                        // The whole picture at the size it's drawn at, then the part shown.
                        const float sw = std::max(1.0f, static_cast<float>(pw) / std::max(0.001f, op.u1 - op.u0));
                        const float sh = std::max(1.0f, static_cast<float>(ph) / std::max(0.001f, op.v1 - op.v0));
                        out = croppedTo(loaded->svg->render(static_cast<uint32_t>(std::min(8192.0f, sw)), static_cast<uint32_t>(std::min(8192.0f, sh))), op);
                        if (out.width != pw || out.height != ph) out = web::resampled(out, pw, ph);
                    } else {
                        out = loaded->svg->render(pw, ph);
                    }
                } else {
                    out.width = loaded->width;
                    out.height = loaded->height;
                    out.rgba = loaded->pixels;
                    out = croppedTo(std::move(out), op);
                    if (out.width != pw || out.height != ph) out = web::resampled(out, pw, ph);
                }
                if (filtered && shown_w > 0) web::applyFilters(out, *op.filter, static_cast<float>(out.width) / shown_w);
                if (rx > 0 && shown_w > 0 && shown_h > 0)
                    web::roundCorners(out, rx * static_cast<float>(out.width) / shown_w, ry * static_cast<float>(out.height) / shown_h);
                if (tint)
                    for (size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
                        out.rgba[i] = color.r;
                        out.rgba[i + 1] = color.g;
                        out.rgba[i + 2] = color.b;
                        out.rgba[i + 3] = static_cast<uint8_t>(out.rgba[i + 3] * color.a / 255);
                    }
                return out;
            });
            if (r) drawRaster(dl, *r, x, y, op.w, op.h);
            return;
        }
        if (group_) {
            web::Raster r;
            r.width = loaded->width;
            r.height = loaded->height;
            r.rgba = loaded->pixels;
            groupRaster(r, x, y, op.w, op.h);
            return;
        }
        fbx::shared::ImageData d{};
        d.x_ = x;
        d.y_ = y;
        d.width_ = op.w;
        d.height_ = op.h;
        d.texture_id_ = loaded->id;
        d.pixel_ptr_ = loaded->pixels.data();
        d.pixel_width_ = loaded->width;
        d.pixel_height_ = loaded->height;
        d.uv_x0_ = op.u0;
        d.uv_y0_ = op.v0;
        d.uv_x1_ = op.u1;
        d.uv_y1_ = op.v1;
        dl.addCommand(fbx::shared::CommandType::DrawImage, {reinterpret_cast<const uint8_t*>(&d), sizeof(d)});
    }

    // ── Transforms ──
    // The compositor draws rectangles, text and images upright. What a rotation, scale or
    // skew holds, when it's no text, is drawn here on the CPU through the matrix, once,
    // and handed over as one image; with text in it, it's drawn untransformed.

    // Into the group being drawn: a raster shown at (x, y, w, h) on screen.
    void groupRaster(const web::Raster& r, float x, float y, float w, float h) {
        if (r.empty()) return;
        const web::Matrix place = web::Matrix::translate(x, y).after(web::Matrix::scaling(w / static_cast<float>(r.width), h / static_cast<float>(r.height)));
        group_->drawRaster(r, group_m_.after(place));
    }

    // The TransformEnd that closes the TransformBegin at `begin` (the list's end if none).
    [[nodiscard]] size_t transformEnd(size_t begin) const {
        int depth = 0;
        for (size_t k = begin; k < layout_.ops.size(); ++k) {
            if (layout_.ops[k].kind == web::PaintKind::TransformBegin) ++depth;
            else if (layout_.ops[k].kind == web::PaintKind::TransformEnd && --depth == 0) return k;
        }
        return layout_.ops.size();
    }

    // Each drawing op from `begin` to `end` with the matrix in force (page coordinates).
    template <typename F>
    void eachTransformed(size_t begin, size_t end, F&& f) const {
        std::vector<web::Matrix> stack;
        for (size_t k = begin; k < end && k < layout_.ops.size(); ++k) {
            const web::PaintOp& op = layout_.ops[k];
            if (op.kind == web::PaintKind::TransformBegin) stack.push_back(stack.empty() ? op.transform : stack.back().after(op.transform));
            else if (op.kind == web::PaintKind::TransformEnd) { if (!stack.empty()) stack.pop_back(); }
            else if (op.kind != web::PaintKind::ClipBegin && op.kind != web::PaintKind::ClipEnd && !stack.empty()) f(op, stack.back());
        }
    }

    // Draws the group from `begin` to `end`; false if it holds text (then it isn't drawn).
    bool drawTransformed(fbx::xrender::DisplayList& dl, size_t begin, size_t end, Offset at) {
        bool text = false;
        float l = INFINITY, t = INFINITY, r = -INFINITY, b = -INFINITY;
        eachTransformed(begin, end, [&](const web::PaintOp& op, const web::Matrix& m) {
            if (op.kind == web::PaintKind::Text) text = true;
            for (const web::Point p : {web::Point{op.x, op.y}, web::Point{op.x + op.w, op.y}, web::Point{op.x, op.y + op.h}, web::Point{op.x + op.w, op.y + op.h}}) {
                const web::Point q = m.apply(p);
                l = std::min(l, q.x);
                t = std::min(t, q.y);
                r = std::max(r, q.x);
                b = std::max(b, q.y);
            }
        });
        if (text) return false;
        if (!(r > l && b > t) || !std::isfinite(l) || !std::isfinite(t) || !std::isfinite(r) || !std::isfinite(b)) return true;
        l = std::floor(l);
        t = std::floor(t);
        r = std::ceil(r);
        b = std::ceil(b);
        const float y = t + at.y_;
        if (y > kMaxWindowHeight + kOffscreenMargin || y + (b - t) < -kOffscreenMargin) return true;
        const auto [pw, ph] = rasterSize(r - l, b - t);
        const std::string key = "t:" + std::to_string(layout_gen_) + ":" + std::to_string(begin) + ":" +
                                std::to_string(page_ ? page_->images.size() : 0) + ":" + std::to_string(pw) + "x" + std::to_string(ph);
        const web::Matrix to_canvas = web::Matrix::scaling(static_cast<float>(pw) / (r - l), static_cast<float>(ph) / (b - t)).after(web::Matrix::translate(-l, -t));
        const CachedRaster* drawn = cached(key, [&, pw = pw, ph = ph] {
            web::Canvas canvas(pw, ph);
            group_ = &canvas;
            eachTransformed(begin, end, [&](const web::PaintOp& op, const web::Matrix& m) {
                const web::Matrix page_to_canvas = to_canvas.after(m);
                if (op.kind == web::PaintKind::Rect) {
                    canvas.fill(web::transformed(web::roundedRect(op.x, op.y, op.w, op.h, op.radius, op.radius), page_to_canvas), false, web::Paint{op.color, nullptr, 1});
                    return;
                }
                // The helpers place things on screen: back to the page, then through.
                group_m_ = page_to_canvas.after(web::Matrix::translate(-at.x_, -at.y_));
                const float x = op.x + at.x_, oy = op.y + at.y_;
                switch (op.kind) {
                    case web::PaintKind::Image: image(dl, op, x, oy); break;
                    case web::PaintKind::Gradient: gradient(dl, op, x, oy); break;
                    case web::PaintKind::Svg: inlineSvg(dl, op, x, oy); break;
                    case web::PaintKind::Box: boxPaint(dl, op, x, oy); break;
                    default: break;
                }
            });
            group_ = nullptr;
            return canvas.toRaster();
        });
        if (drawn) drawRaster(dl, *drawn, l + at.x_, t + at.y_, r - l, b - t);
        return true;
    }

    web::Canvas* group_ = nullptr;   // a transformed group being drawn into
    web::Matrix group_m_;            // screen coordinates to its canvas
    uint64_t layout_gen_ = 0;

    std::shared_ptr<Page> page_;
    std::unordered_map<const web::Node*, std::pair<uint64_t, LoadedImage>> canvas_images_;
    std::function<void(const std::shared_ptr<Page>&, std::vector<web::WebFontFace>)> on_fonts_;
    std::function<void(const std::shared_ptr<Page>&, std::vector<std::string>)> on_images_;
    std::function<void(const std::shared_ptr<Page>&, std::vector<std::string>)> on_sheets_;
    std::unique_ptr<web::StyledDocument> styles_;   // the last layout's, which inline SVG is drawn with
    std::map<std::string, CachedRaster> rasters_;
    const web::Node* popup_ = nullptr;   // a <select> whose list is open
    int popup_highlight_ = -1, popup_first_ = 0;
    bool dirty_paint_ = false;
    uint64_t raster_pixels_ = 0, use_clock_ = 0, paint_frame_ = 0;
    PageImages images_;
    web::XrenderTextMeasurer measurer_;
    web::PageLayout layout_;
    std::vector<float> text_y_;
    float laid_out_width_ = -1, laid_out_height_ = -1;
    bool dirty_ = true;
};

// ── The browser ───────────────────────────────────────────────────────────────

class Browser {
    friend class WebPageElement;   // a click on a summary

public:
    explicit Browser(std::shared_ptr<RenderWebPage> node) : node_(std::move(node)) {
        node_->onFontsWanted([this](const std::shared_ptr<Page>& page, std::vector<web::WebFontFace> faces) { loadFonts(page, std::move(faces)); });
        node_->onImagesWanted([this](const std::shared_ptr<Page>& page, std::vector<std::string> urls) {
            // Not from inside layout: next turn of the loop.
            XApplication::post([this, page, urls = std::move(urls)]() mutable { loadImages(page, std::move(urls), generation_.load()); });
        });
        node_->onSheetsWanted([this](const std::shared_ptr<Page>& page, std::vector<std::string> urls) {
            const uint64_t mine = generation_.load();
            std::thread([this, page, urls = std::move(urls), mine] {
                // Each, with what it imports, then all of them into the page at once.
                std::vector<std::pair<std::string, std::string>> got_sheets;
                std::deque<std::string> queue(urls.begin(), urls.end());
                std::set<std::string> seen;
                while (!queue.empty() && got_sheets.size() < kMaxSheets && generation_.load() == mine) {
                    std::string url = std::move(queue.front());
                    queue.pop_front();
                    if (!seen.insert(url).second) continue;
                    Resource got = load(url, web::sameSite(page->url, url));
                    const bool ok = got.ok && got.http_status < 400;
                    std::string css = ok ? web::decodeToUtf8(got.body, got.content_type) : std::string{};
                    for (const std::string& ref : web::css::parseStylesheet(css).imports) queue.push_back(web::resolveUrl(url, ref));
                    got_sheets.emplace_back(std::move(url), ok ? std::move(css) : std::string("\x01" "failed"));
                }
                XApplication::post([this, page, got_sheets = std::move(got_sheets), mine] {
                    if (generation_.load() != mine) return;
                    for (const auto& [url, css] : got_sheets) {
                        const bool ok = css != "\x01" "failed";
                        if (!page->sheets.count(url)) page->sheets[url] = ok ? css : std::string{};
                        if (page->script) page->script->sheetLoaded(url, ok);
                    }
                    if (page->script) (void)page->script->takeDirty();
                    for (const auto& [url, css] : got_sheets)
                        if (css.find("@font-face") != std::string::npos) page->fonts_known = false;   // its fonts too
                    relayout_wanted_ = true;
                });
            }).detach();
        });
    }

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

    // Where we are, as the address bar would say it.
    [[nodiscard]] std::string currentUrl() const { return index_ < history_.size() ? history_[index_].url : std::string{}; }

    // A form submitted: GET goes where it points, POST sends its body there.
    void submit(const web::FormSubmission& sub) {
        if (sub.post) navigate(sub.url, Mode::New, sub);
        else navigate(sub.url, Mode::New);
    }

    void back() { travel(-1); }
    void forward() { travel(1); }
    void reload() {
        if (index_ < history_.size()) navigate(history_[index_].url, Mode::Reload);
    }

    // Once a frame: images arrive in bursts, and laying a page out for each would crawl.
    void onFrame() {
        // The page's event loop: timers and animation frames.
        if (Page* page = node_->page(); page && page->script) {
            if (page->script->busy()) page->script->tick();
            if (page->script->takeDirty()) node_->invalidate();
        }
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

    // A form's request is the next page.
    void navigate(const std::string& url, Mode mode, std::optional<web::FormSubmission> post = std::nullopt) {
        if (mode == Mode::New) pushHistory(url);
        showAddress(url);
        XApplication::setTitle("Loading...");
        const uint64_t mine = ++generation_;
        const float restore = mode == Mode::History ? history_[index_].scroll : (mode == Mode::Reload ? node_->scrollY() : 0);
        std::thread([this, url, mine, restore, post = std::move(post)] {
            auto page = std::make_shared<Page>();
            Resource got = post ? loadPost(*post) : load(url, true);
            if (generation_.load() != mine) return;
            page->url = got.ok ? got.final_url : url;
            if (got.ok) page->charset = web::detectCharset(got.body, got.content_type);
            page->doc = web::parseHtml(htmlFor(got, url), web::ParseOptions{.scripting = true});
            page->base = findBase(*page->doc, page->url);
            loadSheets(*page, generation_, mine);
            loadScripts(*page, generation_, mine);
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
        startScripts(page);
        // The stylesheets fetched with the page are here: "load" at their <link>s (a style
        // preload's onload is how loadCSS makes it a stylesheet).
        if (page->script) {
            for (const auto& [url, css] : page->sheets) page->script->sheetLoaded(url, true);
            if (page->script->takeDirty()) node_->refresh();
        }
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

    // The page's scripts run once it's shown, in its own world; what they ask of the
    // browser goes through these.
    void startScripts(const std::shared_ptr<Page>& page) {
        web::PageScript::Host host;
        host.url = page->url;
        host.base = page->base;
        int w = 0, h = 0;
        XApplication::windowSize(w, h);
        host.viewport_w = w > 0 ? static_cast<float>(w) : 1000;
        host.viewport_h = h > 0 ? static_cast<float>(h) - kToolbarHeight : 800;
        host.forms = &page->forms;
        host.console = [](std::string_view level, const std::string& text) { std::cerr << "[page " << level << "] " << text << "\n"; };
        host.navigate = [this](const std::string& url) { XApplication::post([this, url] { navigate(url, Mode::New); }); };
        host.rect = [this, raw = page.get()](const web::Node* n) -> std::optional<web::ElementRect> {
            // Laid out again once per change, not per question: a script asking for many
            // boxes or styles in one turn mustn't redo the whole page each time.
            if (raw->script && raw->script->takeDirty()) {
                node_->refresh();
                node_->invalidate();
            }
            if (const web::ElementBox* b = node_->boxOf(n)) return web::ElementRect{b->x, b->y, b->w, b->h};
            return std::nullopt;
        };
        host.computed_style = [this, raw = page.get()](const web::Node* n) {
            // Laid out again once per change, not per question: a script asking for many
            // boxes or styles in one turn mustn't redo the whole page each time.
            if (raw->script && raw->script->takeDirty()) {
                node_->refresh();
                node_->invalidate();
            }
            return node_->computedStyleOf(n);
        };
        Page* raw = page.get();
        host.activate = [this](const web::Node* n) { activateNode(n); };
        host.load_image = [this, page_weak = std::weak_ptr<Page>(page)](const std::string& url) {
            XApplication::post([this, page_weak, url] {
                std::shared_ptr<Page> p = page_weak.lock();
                if (!p || !p->script) return;
                // Here or given up on already: say so now (the loader won't again).
                if (p->images.count(url) || p->failed_images.count(url)) {
                    p->script->imageLoaded(url, p->images.count(url) > 0);
                    if (p->script->takeDirty()) node_->invalidate();
                    return;
                }
                loadImages(p, {url}, generation_.load());
            });
        };
        host.canvas_text = [](const std::string& text, const std::string& font, float scale, bool render) {
            return canvasText(text, font, scale, render);
        };
        // A loaded image's pixels for a canvas: an SVG drawn at its own size.
        host.image_pixels = [raw](const std::string& url) -> std::shared_ptr<const web::Raster> {
            Page* p = raw;   // (the page owns its script: it outlives this)
            const auto it = p->images.find(web::resolveUrl(p->base, url));
            if (it == p->images.end() || !it->second) return nullptr;
            const LoadedImage& img = *it->second;
            auto r = std::make_shared<web::Raster>();
            if (img.svg) {
                const web::SvgSize& z = img.svg->size();
                *r = img.svg->render(static_cast<uint32_t>(z.width.value_or(300)), static_cast<uint32_t>(z.height.value_or(150)));
            } else {
                r->width = img.width;
                r->height = img.height;
                r->rgba = img.pixels;
            }
            return r;
        };
        host.element_at = [this](float x, float y) -> web::Node* {
            web::Node* n = node_->nodeAt(x, y);
            while (n && !n->isElement()) n = n->parent;
            return n;
        };
        host.image_state = [page = page.get()](const std::string& url) {
            web::PageScript::Host::ImageState st;
            if (const auto it = page->images.find(url); it != page->images.end()) {
                st.done = st.ok = true;
                if (it->second->svg) {
                    const web::SvgSize& z = it->second->svg->size();
                    st.width = static_cast<uint32_t>(z.width.value_or(300));
                    st.height = static_cast<uint32_t>(z.height.value_or(150));
                } else {
                    st.width = it->second->width;
                    st.height = it->second->height;
                }
            } else if (page->failed_images.count(url)) {
                st.done = true;
            }
            if (std::getenv("XWEB_DEBUG_IMAGES")) std::fprintf(stderr, "[image] asked of %s: %s\n", url.c_str(), st.done ? (st.ok ? "here" : "failed") : "coming");
            return st;
        };
        host.submit = [this, raw](const web::Node* form) {
            if (auto sub = web::submitForm(form, nullptr, raw->forms, raw->base, raw->charset)) XApplication::post([this, sub = std::move(*sub)] { submit(sub); });
        };
        host.focus = [this, raw](const web::Node* n) {
            if (!web::isTypedInto(web::controlKind(n))) return;
            raw->forms.focused = n;
            raw->forms.caret = raw->forms.value(n).size();
            node_->invalidate();
        };
        host.scroll_to = [this](float, float y) { node_->scrollTo(y); };
        host.scroll_y = [this] { return node_->scrollY(); };
        // import() and anything else not fetched ahead: off the UI thread, back on it.
        std::weak_ptr<Page> weak = page;
        const std::string page_url = page->url, page_charset = page->charset;
        host.fetch_script = [this, weak, page_url, page_charset](const std::string& url, const std::string& charset,
                                                                  std::function<void(std::optional<std::string>)> done) {
            std::thread([this, weak, page_url, page_charset, url, charset, done = std::move(done)] {
                Resource got = load(url, web::sameSite(page_url, url));
                std::optional<std::string> body;
                // Not saying: as the script element says (jQuery's scriptCharset: Nate's
                // EUC-KR boxes), else as the page is.
                std::string type = got.content_type;
                if (web::detectCharset({}, type).empty()) {
                    const std::string& cs = !charset.empty() ? charset : page_charset;
                    type = "text/javascript; charset=" + (cs.empty() ? std::string("utf-8") : cs);
                }
                if (got.ok && got.http_status < 400 && got.body.size() <= (16u << 20)) body = web::decodeToUtf8(got.body, type);
                // XWEB_DEBUG_SCRIPTS=1: what each script fetch came to.
                if (std::getenv("XWEB_DEBUG_SCRIPTS"))
                    std::fprintf(stderr, "[script] %s: %s status %d, %zu bytes\n", url.c_str(), body ? "ok" : "FAILED", got.http_status, got.body.size());
                XApplication::post([this, weak, url, done, body = std::move(body)] {
                    std::shared_ptr<Page> p = weak.lock();
                    if (!p || !p->script) return;   // the page is gone
                    if (body) p->scripts[url] = *body;
                    done(body);
                    if (p->script->takeDirty()) node_->invalidate();
                });
            }).detach();
        };
        // fetch() and XMLHttpRequest. The broker can't show us a response's headers, so
        // there's no CORS check to make; instead: cookies only for the page's own origin,
        // and nothing on this machine or the local network for a page that isn't there.
        host.fetch = [this, weak, page_url](const web::PageScript::Host::Request& req, std::function<void(web::PageScript::Host::Response)> done) {
            web::PageScript::Host::Response refused;
            const web::Url target = web::parseUrl(req.url);
            const web::Url origin = web::parseUrl(page_url);
            const auto local = [](const std::string& authority) {
                std::string host = authority.substr(0, authority.rfind(':') == std::string::npos || authority.find(']') != std::string::npos ? authority.size() : authority.rfind(':'));
                if (host.starts_with("[")) return host == "[::1]" || host.starts_with("[fc") || host.starts_with("[fd") || host.starts_with("[fe80");
                if (host == "localhost" || host.ends_with(".localhost") || host.ends_with(".local")) return true;
                int a = -1, b = -1;
                if (std::sscanf(host.c_str(), "%d.%d.", &a, &b) == 2)
                    return a == 127 || a == 10 || a == 0 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31) || (a == 169 && b == 254);
                return false;
            };
            const bool same_origin = target.scheme == origin.scheme && target.authority == origin.authority;
            if (target.scheme != "http" && target.scheme != "https") {
                refused.error = "only http and https can be fetched";
            } else if (!same_origin && local(target.authority) && !local(origin.authority)) {
                refused.error = "a page on the web can't reach this machine or the local network";
            }
            if (!refused.error.empty()) {
                XApplication::post([weak, done, refused] {
                    if (weak.lock()) done(refused);
                });
                return;
            }
            (void)same_origin;
            std::thread([this, weak, req, done = std::move(done)] {
                // The fetch algorithm (script_fetch.cc) chose the headers and the cookies;
                // this only carries the request, method, headers and all.
                auto got = scriptWebBlocking(req.method, req.url, req.headers, req.body, req.cookies);
                web::PageScript::Host::Response r;
                r.ok = got.ok();
                r.status = static_cast<int>(got.http_status);
                r.url = got.final_url.empty() ? req.url : got.final_url;
                r.content_type = lowerCase(got.content_type);
                r.body = std::move(got.body);
                r.headers = std::move(got.headers);
                // XWEB_DEBUG_FETCH=1: what each page fetch came to.
                if (std::getenv("XWEB_DEBUG_FETCH"))
                    std::fprintf(stderr, "[fetch] %s %s: %s status %d, %zu bytes%s%s\n", req.method.c_str(), req.url.c_str(), r.ok ? "ok" : "FAILED", r.status,
                                 r.body.size(), req.body.empty() ? "" : " | sent: ", req.body.substr(0, 4000).c_str());
                if (!r.ok) r.error = got.status == fbx::shared::FetchStatus::Denied ? "this address isn't allowed"
                                     : got.status == fbx::shared::FetchStatus::TooLarge ? "the response is too large"
                                     : got.status == fbx::shared::FetchStatus::Unavailable ? "the connection service isn't running"
                                     : "the site can't be reached";
                XApplication::post([this, weak, done, r = std::move(r)] {
                    std::shared_ptr<Page> p = weak.lock();
                    if (!p || !p->script) return;
                    done(r);
                    if (p->script->takeDirty()) node_->invalidate();
                });
            }).detach();
        };
        page->script = std::make_unique<web::PageScript>(*page->doc, std::move(host));
        page->script->run([raw](const std::string& url) -> std::optional<std::string> {
            const auto it = raw->scripts.find(url);
            if (it == raw->scripts.end()) return std::nullopt;
            return it->second;
        });
        if (page->script->takeDirty()) node_->invalidate();
    }

    // A click's default action, for element.click() and the like: follow a link, tick a
    // box, send a form.
    // A click on a <details>'s summary opens or closes it.
    bool toggleDetails(web::Node* target) {
        Page* page = node_->page();
        web::Node* details = page ? web::detailsToggledBy(target) : nullptr;
        if (!details) return false;
        if (page->script) {
            page->script->toggle(details);
            (void)page->script->takeDirty();
        } else if (details->attr("open")) {
            std::erase_if(details->attrs, [](const web::Attribute& a) { return a.name == "open"; });
        } else {
            details->attrs.push_back({"open", ""});
        }
        node_->invalidate();
        return true;
    }

    void activateNode(const web::Node* n) {
        Page* page = node_->page();
        if (!page) return;
        if (toggleDetails(const_cast<web::Node*>(n))) return;
        for (const web::Node* a = n; a; a = a->parent)
            if (a->isHtml("a"))
                if (const std::string* href = a->attr("href")) {
                    const std::string h = *href;
                    XApplication::post([this, h] { follow(h); });
                    return;
                }
        switch (web::controlKind(n)) {
            case web::ControlKind::Checkbox:
            case web::ControlKind::Radio:
                page->forms.activate(n);
                node_->invalidate();
                break;
            case web::ControlKind::Submit:
            case web::ControlKind::Image:
                if (const web::Node* form = web::formOwner(n)) {
                    if (page->script && !page->script->submit(const_cast<web::Node*>(form), const_cast<web::Node*>(n))) return;
                    if (auto sub = web::submitForm(form, n, page->forms, page->base, page->charset)) XApplication::post([this, sub = std::move(*sub)] { submit(sub); });
                }
                break;
            default: break;
        }
    }

    void loadImages(const std::shared_ptr<Page>& page, std::vector<std::string> sources, uint64_t mine) {
        std::erase_if(sources, [&](const std::string& url) {
            return page->requested.size() >= kMaxImages || !page->requested.insert(url).second;
        });
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
                    Resource got = load(url, web::sameSite(page->url, url));
                    // XWEB_DEBUG_IMAGES=1: what each image came to, for finding why one's missing.
                    static const bool debug = std::getenv("XWEB_DEBUG_IMAGES") != nullptr;
                    // Said to the page's scripts, as an "error" at its <img>s.
                    const auto failed = [this, page, url, mine] {
                        XApplication::post([this, page, url, mine] {
                            if (generation_.load() != mine) return;
                            page->failed_images.insert(url);
                            relayout_wanted_ = true;   // now its alt text shows
                            if (page->script) {
                                page->script->imageLoaded(url, false);
                                (void)page->script->takeDirty();
                            }
                        });
                    };
                    if (!got.ok || got.http_status >= 400) {
                        if (debug) std::fprintf(stderr, "[image] %s: not loaded (status %d)\n", url.c_str(), got.http_status);
                        failed();
                        continue;
                    }
                    auto decoded = decodeImage(url, got.body, got.content_type);
                    if (debug)
                        std::fprintf(stderr, "[image] %s: %zu bytes %s, %s\n", url.c_str(), got.body.size(), got.content_type.c_str(),
                                     decoded ? (std::to_string(decoded->width) + "x" + std::to_string(decoded->height)).c_str() : "not decoded");
                    if (!decoded) {
                        failed();
                        continue;
                    }
                    auto image = std::make_shared<LoadedImage>(std::move(*decoded));
                    XApplication::post([this, page, url, image, mine] {
                        if (generation_.load() != mine) return;
                        const uint64_t pixels = static_cast<uint64_t>(image->width) * image->height;
                        if (page->pixels + pixels > kMaxPagePixels) {
                            if (std::getenv("XWEB_DEBUG_IMAGES")) std::fprintf(stderr, "[image] %s: over the page's pixel budget\n", url.c_str());
                            page->failed_images.insert(url);   // not coming: its alt text, then
                            relayout_wanted_ = true;
                            return;
                        }
                        page->pixels += pixels;
                        page->images[url] = image;
                        relayout_wanted_ = true;
                        // "load" at its <img>s: pages hide placeholders, start fades, measure.
                        if (page->script) {
                            page->script->imageLoaded(url, true);
                            (void)page->script->takeDirty();
                        }
                    });
                }
            }).detach();
        }
    }

    // In the background, text showing in another font until they come. A face has its
    // sources tried in order, until one really is a font.
    void loadFonts(const std::shared_ptr<Page>& page, std::vector<web::WebFontFace> faces) {
        const uint64_t mine = generation_.load();
        struct Work {
            std::mutex mutex;
            std::deque<std::vector<std::string>> queue;
        };
        auto work = std::make_shared<Work>();
        for (web::WebFontFace& f : faces) {
            const bool have = std::any_of(f.sources.begin(), f.sources.end(), [](const std::string& s) { return web::haveWebFontData(s); });
            if (!have) work->queue.push_back(std::move(f.sources));
        }
        if (work->queue.empty()) return;
        for (size_t i = 0; i < kFontWorkers; ++i) {
            std::thread([this, page, work, mine] {
                while (generation_.load() == mine) {
                    std::vector<std::string> sources;
                    {
                        std::lock_guard<std::mutex> lock(work->mutex);
                        if (work->queue.empty()) return;
                        sources = std::move(work->queue.front());
                        work->queue.pop_front();
                    }
                    for (const std::string& url : sources) {
                        Resource got = load(url, web::sameSite(page->url, url));
                        if (!got.ok || got.http_status >= 400) continue;
                        auto bytes = std::make_shared<const std::string>(std::move(got.body));
                        if (!web::webFontUsable(bytes)) continue;
                        XApplication::post([this, url, bytes, mine] {
                            if (!web::addWebFontData(url, bytes)) return;
                            if (generation_.load() == mine) relayout_wanted_ = true;
                        });
                        break;
                    }
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
        using fbx::shared::InputEventType;
        const auto at = node_->getAbsoluteOffset();
        const float x = static_cast<float>(event.value_a_) - at.x_;
        const float y = static_cast<float>(event.value_b_) - at.y_;
        Page* page = node_->page();
        if (event.type_ == InputEventType::MouseMove) {
            const web::Node* c = node_->controlAt(x, y);
            const web::ControlKind k = web::controlKind(c);
            node_->setCursor(web::isTypedInto(k) ? CursorShape::Text : (c || node_->linkAt(x, y)) ? CursorShape::Pointer : CursorShape::Default);
            return false;
        }
        if (event.type_ == InputEventType::MouseButton) {
            const bool down = event.char_value_ != 0;
            // An open list takes the click: an option picks it, anywhere else closes it.
            if (const web::Node* select = node_->openPopup(); select && page) {
                if (!down) return true;
                const int row = node_->popupRowAt(x, y);
                if (row >= 0 && !web::optionsOf(select)[static_cast<size_t>(row)]->attr("disabled")) {
                    page->forms.select(select, row);
                    if (page->script) page->script->change(const_cast<web::Node*>(select));
                }
                node_->closeSelect();
                changed();
                return true;
            }
            const web::Node* control = page ? node_->controlAt(x, y) : nullptr;
            const web::ControlKind kind = web::controlKind(control);
            if (!down && page && page->script) {
                // Scripts hear the click first, and may cancel what it would do.
                web::Node* target = node_->nodeAt(x, y);
                const bool go_on = page->script->click(target, x, y);
                if (page->script->takeDirty()) changed();
                if (!go_on) {
                    pressed_control_ = nullptr;
                    pressed_.clear();
                    return true;
                }
            }
            if (down) {
                pressed_control_ = control;
                if (web::isTypedInto(kind)) {
                    focusField(control, node_->caretAt(control, x));
                    return true;
                }
                blur();
                if (control) return true;
            } else if (control && control == pressed_control_) {
                pressed_control_ = nullptr;
                activate(control);
                return true;
            }
            pressed_control_ = nullptr;
            if (!down && browser_ && browser_->toggleDetails(node_->nodeAt(x, y))) {
                changed();
                return true;
            }
            return handleLinkClick(down, x, y);
        }
        if (!page) return false;
        if (const web::Node* select = node_->openPopup()) {
            if (event.type_ != InputEventType::KeyPress || event.value_b_ == 0) return true;
            constexpr int kUp = 103, kDown = 108, kEnter = 28, kEsc = 1;
            if (event.value_a_ == kUp || event.value_a_ == kDown) node_->movePopupHighlight(event.value_a_ == kUp ? -1 : 1);
            else if (event.value_a_ == kEnter) {
                if (node_->popupHighlight() >= 0) page->forms.select(select, node_->popupHighlight());
                node_->closeSelect();
            } else if (event.value_a_ == kEsc) node_->closeSelect();
            changed();
            return true;
        }
        const web::Node* field = page->forms.focused;
        if (page->script && event.type_ == InputEventType::KeyPress) {
            // keydown and keyup, to the field being typed into or the page.
            const bool down = event.value_b_ != 0;
            std::string key;
            switch (event.value_a_) {
                case 28: case 96: key = "Enter"; break;
                case 14: key = "Backspace"; break;
                case 15: key = "Tab"; break;
                case 1: key = "Escape"; break;
                case 111: key = "Delete"; break;
                case 103: key = "ArrowUp"; break;
                case 108: key = "ArrowDown"; break;
                case 105: key = "ArrowLeft"; break;
                case 106: key = "ArrowRight"; break;
                case 102: key = "Home"; break;
                case 107: key = "End"; break;
                case 104: key = "PageUp"; break;
                case 109: key = "PageDown"; break;
                case 57: key = " "; break;
                default:
                    if (event.char_value_ > 0 && event.char_value_ < 0x80) key = std::string(1, static_cast<char>(event.char_value_));
                    else if (event.char_value_ >= 0x80) key = utf8Of(static_cast<char32_t>(event.char_value_));
                    else key = "Unidentified";
            }
            int code = event.char_value_ >= 'a' && event.char_value_ <= 'z' ? event.char_value_ - 32 : event.char_value_;
            if (key == "Enter") code = 13;
            else if (key == "Escape") code = 27;
            else if (key == "Backspace") code = 8;
            else if (key == "Tab") code = 9;
            else if (key.starts_with("Arrow")) code = key == "ArrowLeft" ? 37 : key == "ArrowUp" ? 38 : key == "ArrowRight" ? 39 : 40;
            const bool go_on = page->script->key(const_cast<web::Node*>(field), down, key, code, event.modifiers_);
            if (page->script->takeDirty()) changed();
            if (!go_on && field) return true;
        }
        if (!field) return false;
        if (event.type_ == InputEventType::ImeCommit) {
            const uint32_t len = std::min(event.modifiers_, 8u);
            std::string committed(len, '\0');
            if (len > 0) std::memcpy(committed.data(), &event.value_a_, std::min(len, 4u));
            if (len > 4) std::memcpy(committed.data() + 4, &event.value_b_, len - 4u);
            edit([&](TextEditState& e) { e.insert(committed); });
            return true;
        }
        if (event.type_ != InputEventType::KeyPress || event.value_b_ == 0) return event.type_ == InputEventType::KeyPress;
        const bool ctrl = event.modifiers_ & 2, shift = event.modifiers_ & 1;
        const int kc = event.value_a_;
        const bool multiline = web::controlKind(field) == web::ControlKind::TextArea;
        if (kc == 15) {   // Tab: the next field (or the one before)
            focusNext(shift ? -1 : 1);
            return true;
        }
        if (kc == 28 || kc == 96) {
            if (multiline) edit([](TextEditState& e) { e.insert('\n'); });
            else submitFrom(field);
            return true;
        }
        if (ctrl && kc == 30) { edit([](TextEditState& e) { e.selectAll(); }); return true; }
        if (ctrl && kc == 46) {
            if (auto* bus = XApplication::getBus()) Clipboard::setString(*bus, page->forms.value(field));
            return true;
        }
        if (ctrl && kc == 47) {
            if (auto* bus = XApplication::getBus()) {
                std::weak_ptr<Element> self = shared_from_this();
                Clipboard::getString(*bus, [this, self](const std::string& pasted) {
                    if (self.lock()) edit([&](TextEditState& e) { e.insert(pasted); });
                });
            }
            return true;
        }
        switch (kc) {
            case 14: edit([](TextEditState& e) { e.backspace(); }); return true;
            case 111: edit([](TextEditState& e) { e.deleteForward(); }); return true;
            case 105: edit([&](TextEditState& e) { e.moveLeft(false); }); return true;
            case 106: edit([&](TextEditState& e) { e.moveRight(false); }); return true;
            case 102: edit([&](TextEditState& e) { e.moveHome(false); }); return true;
            case 107: edit([&](TextEditState& e) { e.moveEnd(false); }); return true;
            case 1: blur(); return true;   // Esc
            default: break;
        }
        if (event.char_value_ != 0 && !ctrl) {
            const uint32_t cp = static_cast<uint32_t>(event.char_value_);
            std::string utf8;
            if (cp < 0x80) utf8 += static_cast<char>(cp);
            else if (cp < 0x800) { utf8 += static_cast<char>(0xC0 | (cp >> 6)); utf8 += static_cast<char>(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { utf8 += static_cast<char>(0xE0 | (cp >> 12)); utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); utf8 += static_cast<char>(0x80 | (cp & 0x3F)); }
            else { utf8 += static_cast<char>(0xF0 | (cp >> 18)); utf8 += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); utf8 += static_cast<char>(0x80 | (cp & 0x3F)); }
            (void)shift;
            edit([&](TextEditState& e) { e.insert(utf8); });
            return true;
        }
        return true;   // a field being typed into keeps its keys
    }

    // Tab stays in the page while a field has the keyboard.
    [[nodiscard]] bool wantsTabKey() const noexcept override {
        const Page* page = node_->page();
        return page && page->forms.focused;
    }

private:
    bool handleLinkClick(bool down, float x, float y) {
        // A click: pressed and released on the same link.
        const std::string* link = node_->linkAt(x, y);
        if (down) {
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

    void changed() { node_->invalidate(); }

    // Keyboard focus to a field, and the input method with it (but not for a password).
    void focusField(const web::Node* field, size_t caret) {
        Page* page = node_->page();
        if (!page) return;
        page->forms.focused = field;
        page->forms.caret = std::min(caret, page->forms.value(field).size());
        XApplication::requestFocus(shared_from_this());
        if (auto* bus = XApplication::getBus()) {
            if (web::controlKind(field) == web::ControlKind::Password) {
                bus->Call("com.freebsdx.IME", static_cast<uint32_t>(fbx::shared::IMEOpcode::Deactivate), {}, [](const fbx::shared::XBusMessage&) {});
            } else {
                fbx::shared::IMEActivateRequest req{};
                bus->Call("com.freebsdx.IME", static_cast<uint32_t>(fbx::shared::IMEOpcode::Activate),
                          {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, [](const fbx::shared::XBusMessage&) {});
            }
        }
        changed();
    }

    void blur() {
        Page* page = node_->page();
        if (!page || !page->forms.focused) return;
        page->forms.focused = nullptr;
        if (auto* bus = XApplication::getBus())
            bus->Call("com.freebsdx.IME", static_cast<uint32_t>(fbx::shared::IMEOpcode::Deactivate), {}, [](const fbx::shared::XBusMessage&) {});
        changed();
    }

    // An edit to the focused field, through the toolkit's own editing rules.
    template <typename F>
    void edit(F&& f) {
        Page* page = node_->page();
        if (!page || !page->forms.focused) return;
        const web::Node* field = page->forms.focused;
        TextEditState e;
        e.text = page->forms.value(field);
        e.cursor = e.anchor = std::min(page->forms.caret, e.text.size());
        if (selecting_all_) e.anchor = 0;
        f(e);
        if (e.hasSelection() && e.anchor == 0 && e.cursor == e.text.size()) {
            selecting_all_ = true;   // Ctrl+A: the next edit replaces it all
        } else {
            selecting_all_ = false;
        }
        const bool really = page->forms.value(field) != e.text;
        page->forms.setValue(field, e.text);
        page->forms.caret = std::min(e.cursor, page->forms.value(field).size());
        if (really && page->script) page->script->input(const_cast<web::Node*>(field));
        changed();
    }

    // A click that does something to a control (its press already took the focus).
    void activate(const web::Node* control) {
        Page* page = node_->page();
        if (!page) return;
        switch (web::controlKind(control)) {
            case web::ControlKind::Checkbox:
            case web::ControlKind::Radio:
                page->forms.activate(control);
                if (page->script) {
                    page->script->input(const_cast<web::Node*>(control));
                    page->script->change(const_cast<web::Node*>(control));
                }
                changed();
                break;
            case web::ControlKind::Select:
                node_->openSelect(control);
                XApplication::requestFocus(shared_from_this());
                changed();
                break;
            case web::ControlKind::Submit:
            case web::ControlKind::Image:
                submit(web::formOwner(control), control);
                break;
            case web::ControlKind::Reset:
                if (const web::Node* form = web::formOwner(control)) {
                    page->forms.reset(form);
                    changed();
                }
                break;
            default: break;
        }
    }

    // Enter in a field: its form's first submit button, or the form alone if that's allowed.
    void submitFrom(const web::Node* field) {
        const web::Node* form = web::formOwner(field);
        if (!form) return;
        if (const web::Node* button = web::defaultButton(form)) {
            if (!web::isDisabled(button)) submit(form, button);
            return;
        }
        if (web::implicitSubmissionAllowed(form)) submit(form, nullptr);
    }

    void submit(const web::Node* form, const web::Node* submitter) {
        Page* page = node_->page();
        if (!page || !form) return;
        if (page->script && !page->script->submit(const_cast<web::Node*>(form), const_cast<web::Node*>(submitter))) {
            if (page->script->takeDirty()) changed();
            return;
        }
        auto sub = web::submitForm(form, submitter, page->forms, page->base, page->charset);
        if (!sub) return;
        Browser* browser = browser_;
        XApplication::post([browser, sub = std::move(*sub)] { browser->submit(sub); });
    }

    // Tab and Shift+Tab: the next field that takes typing, in document order.
    void focusNext(int direction) {
        Page* page = node_->page();
        if (!page || !page->doc) return;
        std::vector<const web::Node*> fields;
        std::vector<const web::Node*> todo{page->doc->root()};
        while (!todo.empty()) {
            const web::Node* n = todo.back();
            todo.pop_back();
            if (web::isTypedInto(web::controlKind(n)) && !web::isDisabled(n) && node_->boxOf(n)) fields.push_back(n);
            todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        }
        if (fields.empty()) return;
        const auto it = std::find(fields.begin(), fields.end(), page->forms.focused);
        long i = it == fields.end() ? 0 : (it - fields.begin()) + direction;
        if (i < 0 || i >= static_cast<long>(fields.size())) {
            blur();
            return;
        }
        focusField(fields[static_cast<size_t>(i)], page->forms.value(fields[static_cast<size_t>(i)]).size());
    }

    std::shared_ptr<RenderWebPage> node_;
    Browser* browser_;
    std::string pressed_;
    const web::Node* pressed_control_ = nullptr;
    bool selecting_all_ = false;
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

XBusClient* g_xbus = nullptr;   // the UI thread's connection, once it's up

// Asks the shell to open the page in the Browser's other engine. The Browser starts
// Chromium for a "chromium:" target; the shell only launches, it doesn't trust us more.
void openInChromium(const std::string& url) {
    const web::Url u = web::parseUrl(url);
    if (!g_xbus || (u.scheme != "http" && u.scheme != "https" && u.scheme != "file")) return;
    // A local file goes as its path, which is what the launcher takes for files.
    const std::string target = "chromium:" + (u.scheme == "file" ? percentDecoded(u.path) : url);
    fbx::shared::ShellLaunchRequest req{};
    if (target.size() >= sizeof(req.file_path_)) {
        XApplication::setTitle("This address is too long to hand to Chromium");
        return;
    }
    std::strncpy(req.app_id_, "com.freebsdx.browser", sizeof(req.app_id_) - 1);
    std::memcpy(req.file_path_, target.data(), target.size());
    g_xbus->Call("com.freebsdx.Shell", static_cast<uint32_t>(fbx::shared::ShellOpcode::LaunchWithFile),
                 {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, {});
}

}  // namespace

// The viewer, for xwebview's own main and for the Browser app's.
int webViewMain(int argc, char** argv) {
    // An exception nothing caught ends the app (the UI loop is noexcept): say which, so a
    // crash report has more to go on than SIGABRT.
    std::set_terminate([] {
        if (const std::exception_ptr e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& x) {
                std::fprintf(stderr, "[browser] uncaught %s: %s\n", typeid(x).name(), x.what());
            } catch (const web::js::Terminated& t) {
                std::fprintf(stderr, "[browser] a script stopped outside a task: %s\n", t.why.c_str());
            } catch (const web::js::Thrown&) {
                std::fprintf(stderr, "[browser] a script's exception got out of its task\n");
            } catch (...) {
                const std::type_info* type = abi::__cxa_current_exception_type();
                std::fprintf(stderr, "[browser] uncaught %s\n", type ? type->name() : "exception of another kind");
            }
        }
        std::abort();
    });
    std::string arg;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--open") continue;
        arg = argv[i];
        break;
    }
    const std::string start = arg.empty() ? std::string{} : startingAddress(arg);

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
    button("Open in Chromium", kChromiumButtonWidth, [] { openInChromium(browser.currentUrl()); });

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

    const int status = XApplication::Run(root, "/tmp/freebsdx.sock", false, false, [start, address_field = address](XBusClient& xbus) {
        g_xbus = &xbus;
        // The field takes the rest of the row, which only now has a known width.
        int window_w = 0, window_h = 0;
        XApplication::windowSize(window_w, window_h);
        if (window_w > 0) address_field->setFixedSize(static_cast<float>(window_w) - 56 - 72 - 64 - kChromiumButtonWidth - 6 * 6, 30);
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

#ifndef XWEB_VIEW_NO_MAIN
int main(int argc, char** argv) { return webViewMain(argc, argv); }
#endif
