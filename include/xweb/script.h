#pragma once

// A page's scripts: its <script>s run on our engine, with window, document, elements,
// events and timers bound to the page's own tree. One per page, on one thread.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "xweb/dom.h"
#include "xweb/forms.h"

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
        std::function<std::unordered_map<std::string, std::string>(const Node*)> computed_style;
        std::function<void(const Node*)> activate;     // a click's default action (a link, a checkbox, a submit button)
        std::function<void(const Node*)> submit;       // form.submit(): no event, straight off
        std::function<void(const Node*)> focus;
        std::function<void(float x, float y)> scroll_to;
        std::function<float()> scroll_y;
        // A script the page wants that wasn't fetched ahead (import(), a module's
        // import): fetched now, its text (or nothing) handed back later on this thread.
        std::function<void(const std::string& url, std::function<void(std::optional<std::string>)> done)> fetch_script;
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

    // The event loop: due timers, then animation frames, then microtasks.
    void tick();
    [[nodiscard]] bool busy() const;   // timers or frames waiting
    // Whether the tree or a control changed since last asked: time to lay out again.
    bool takeDirty();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fbx::xweb
