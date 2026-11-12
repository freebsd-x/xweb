#pragma once

// A page's scripts: its <script>s run on our engine, with window, document, elements,
// events and timers bound to the page's own tree. One per page, on one thread.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xweb/dom.h"
#include "xweb/forms.h"
#include "xweb/paint.h"

namespace fbx::xweb {

struct ElementRect {
    float x = 0, y = 0, w = 0, h = 0;   // page coordinates
};

class PageScript {
public:
    // What the page's scripts can ask of the browser around them.
    struct Host {
        std::string url;
        std::string base;
        float viewport_w = 1000, viewport_h = 800;
        FormState* forms = nullptr;
        std::function<void(std::string_view level, const std::string& text)> console;
        std::function<void(const std::string& url)> navigate;
        std::function<std::optional<ElementRect>(const Node*)> rect;
        // The element shown at a point of the page (not the window); null if none.
        std::function<Node*(float page_x, float page_y)> element_at;
        // For 2D canvases. Text in a CSS font at `scale` times its size: its coverage (in
        // the alpha of `mask`, left edge at the pen, top `ascent` above the baseline) when
        // `render`, and its metrics either way, in unscaled px. Null: no fonts here.
        struct CanvasText {
            Raster mask;
            float advance = 0, ascent = 0, descent = 0;
        };
        std::function<std::optional<CanvasText>(const std::string& text, const std::string& font, float scale, bool render)> canvas_text;
        // Load this image (an <img> a script set a src on): imageLoaded() says when, even
        // when it was here already.
        std::function<void(const std::string& url)> load_image;
        // A loaded image's pixels, by its URL (null if it isn't loaded).
        std::function<std::shared_ptr<const Raster>(const std::string& url)> image_pixels;
        // An image's state by its absolute URL: still coming, or here (with its size), or failed.
        struct ImageState {
            bool done = false, ok = false;
            uint32_t width = 0, height = 0;
        };
        std::function<ImageState(const std::string& url)> image_state;
        std::function<std::unordered_map<std::string, std::string>(const Node*)> computed_style;
        std::function<void(const Node*)> activate;     // a click's default action (a link, a checkbox, a submit button)
        std::function<void(const Node*)> submit;       // form.submit(): no event, straight off
        std::function<void(const Node*)> focus;
        std::function<void(float x, float y)> scroll_to;
        std::function<float()> scroll_y;
        // A script the page wants that wasn't fetched ahead (import(), a module's
        // import): fetched now, its text (or nothing) handed back later on this thread.
        // `charset`: what to read it as if the response doesn't say (a script's charset
        // attribute; "utf-8" for modules; empty: as the page is).
        std::function<void(const std::string& url, const std::string& charset, std::function<void(std::optional<std::string>)> done)> fetch_script;
        // fetch() and XMLHttpRequest: the request, and later on this thread its response.
        // The browser decides what a page may reach (see the viewer's policy).
        struct Request {
            std::string url;            // absolute
            std::string method = "GET";
            std::string content_type;
            std::string body;
            std::vector<std::pair<std::string, std::string>> headers;   // what the page may send, and Origin/Referer
            bool cookies = false;       // send and keep cookies (decided by the fetch algorithm)
        };
        struct Response {
            bool ok = false;            // a response came back (any status)
            int status = 0;
            std::string url;            // after redirects
            std::string content_type;
            std::string body;
            std::vector<std::pair<std::string, std::string>> headers;   // lower-case names; never Set-Cookie
            std::string error;          // when not ok: why
        };
        std::function<void(const Request&, std::function<void(Response)> done)> fetch;
    };

    PageScript(Document& doc, Host host);
    ~PageScript();
    PageScript(const PageScript&) = delete;
    PageScript& operator=(const PageScript&) = delete;

    // Runs the page's scripts in document order, then DOMContentLoaded and load.
    // `source` gives an external script's text by its absolute address.
    void run(const std::function<std::optional<std::string>(const std::string& url)>& source);

    // The user's doing, as events. Each returns false if a script cancelled the default.
    bool click(Node* target, float page_x, float page_y);
    bool submit(Node* form, Node* submitter);
    void input(Node* control);
    void change(Node* control);
    bool key(Node* target, bool down, const std::string& key, int key_code, uint32_t modifiers);
    void resized(float w, float h);
    void scrolled();
    // Opens or closes a <details>, as clicking its summary does, and says so with "toggle".
    void toggle(Node* details);
    // An image has come (or failed): "load" (or "error") at each <img> showing it.
    void imageLoaded(const std::string& url, bool ok);
    // A stylesheet has come (or failed): "load" (or "error") at each <link> that names it,
    // as a stylesheet or a style preload (loadCSS's onload switches rel to stylesheet).
    void sheetLoaded(const std::string& url, bool ok);

    // The event loop: due timers, then animation frames, then microtasks.
    void tick();
    [[nodiscard]] bool busy() const;   // timers or frames waiting
    // Whether the tree or a control changed since last asked: time to lay out again.
    bool takeDirty();
    // The same, without forgetting it (a script asking where something is now).
    [[nodiscard]] bool dirty() const;
    // A <canvas>'s pixels as its scripts drew them, and a number that changes whenever
    // they do (null: nothing was drawn on it).
    [[nodiscard]] std::shared_ptr<const Raster> canvasImage(const Node* canvas, uint64_t& version) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fbx::xweb
