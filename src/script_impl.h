#pragma once

// A page's script world, shared by the files that bind it (script.cc: Node, Element,
// Document, window; script_dom.cc: element interfaces, custom elements, shadow roots,
// observers). Internal to xweb.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <unordered_map>

#include "xweb/css.h"
#include "xweb/html_parser.h"
#include "xweb/js.h"
#include "xweb/query.h"
#include "xweb/script.h"
#include "xweb/url.h"

namespace fbx::xweb {

using js::Object;
using js::PropertyKey;
using js::Runtime;
using js::Value;
using Args = std::span<const Value>;

namespace script_detail {

inline constexpr uint8_t kNodeWrapper = 1, kStyleWrapper = 2, kClassListWrapper = 3;
inline constexpr uint8_t kOwnedSheet = 4;   // a <style>'s or <link>'s CSSStyleSheet: host is the element
inline constexpr uint64_t kTaskBudget = 50'000'000;   // steps one script, event or timer may take
inline constexpr size_t kMaxTimers = 10'000;
inline constexpr int kMaxCloneDepth = 512;

inline Value arg(Args a, size_t i) { return i < a.size() ? a[i] : Value::undefined(); }
inline Value str(const std::string& s) { return Value::string(js::utf16(s)); }

inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// "background-color" from "backgroundColor", and back.
inline std::string cssName(const std::string& camel) {
    if (camel == "cssFloat") return "float";   // float was a reserved word once
    std::string out;
    for (char c : camel) {
        if (c >= 'A' && c <= 'Z') {
            out += '-';
            out += static_cast<char>(c + 32);
        } else {
            out += c;
        }
    }
    if (out.starts_with("webkit-") || out.starts_with("moz-")) out = "-" + out;
    return out;
}

inline std::string camelName(const std::string& css) {
    std::string out;
    bool up = false;
    for (size_t i = 0; i < css.size(); ++i) {
        const char c = css[i];
        if (c == '-') {
            up = i > 0;
            continue;
        }
        out += up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        up = false;
    }
    return out;
}

// A style attribute as its declarations, in order.
inline std::vector<std::pair<std::string, std::string>> parseStyle(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string cur;
    int depth = 0;
    char quote = 0;
    const auto flush = [&] {
        const size_t colon = cur.find(':');
        if (colon != std::string::npos) {
            auto trim = [](std::string s) {
                const size_t b = s.find_first_not_of(" \t\n\r");
                const size_t e = s.find_last_not_of(" \t\n\r");
                return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
            };
            const std::string name = lower(trim(cur.substr(0, colon)));
            const std::string value = trim(cur.substr(colon + 1));
            if (!name.empty()) {
                const auto it = std::find_if(out.begin(), out.end(), [&](const auto& p) { return p.first == name; });
                if (it != out.end()) it->second = value;
                else out.emplace_back(name, value);
            }
        }
        cur.clear();
    };
    for (char c : text) {
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(') {
            ++depth;
        } else if (c == ')') {
            depth = std::max(0, depth - 1);
        } else if (c == ';' && depth == 0) {
            flush();
            continue;
        }
        cur += c;
    }
    flush();
    return out;
}

inline std::string serializeStyle(const std::vector<std::pair<std::string, std::string>>& decls) {
    std::string out;
    for (const auto& [n, v] : decls) {
        if (!out.empty()) out += " ";
        out += n + ": " + v + ";";
    }
    return out;
}

inline std::string escapeAttr(const std::string& v) {
    std::string out;
    for (char c : v) {
        if (c == '&') out += "&amp;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

inline const char* kStyleProperties[] = {
    "cssFloat", "float", "transformOrigin", "transitionDuration", "transitionProperty", "transitionTimingFunction", "transitionDelay",
    "animationName", "animationDuration", "flexFlow", "gridTemplateAreas", "gridColumnStart", "gridColumnEnd", "gridRowStart",
    "gridRowEnd", "gridAutoFlow", "borderTopLeftRadius", "borderTopRightRadius", "borderBottomLeftRadius", "borderBottomRightRadius",
    "objectPosition", "overflowWrap", "translate", "rotate", "scale", "borderTopColor", "borderBottomColor", "borderLeftColor",
    "borderRightColor", "webkitTransform", "webkitTransition", "display", "position", "top", "right", "bottom", "left", "width", "height", "minWidth", "minHeight", "maxWidth",
    "maxHeight", "margin", "marginTop", "marginRight", "marginBottom", "marginLeft", "padding", "paddingTop",
    "paddingRight", "paddingBottom", "paddingLeft", "border", "borderTop", "borderRight", "borderBottom", "borderLeft",
    "borderWidth", "borderStyle", "borderColor", "borderRadius", "color", "background", "backgroundColor",
    "backgroundImage", "backgroundSize", "backgroundPosition", "backgroundRepeat", "font", "fontSize", "fontWeight",
    "fontFamily", "fontStyle", "lineHeight", "textAlign", "textDecoration", "textTransform", "whiteSpace", "overflow",
    "overflowX", "overflowY", "opacity", "visibility", "zIndex", "float", "clear", "verticalAlign", "boxShadow",
    "boxSizing", "flex", "flexDirection", "flexWrap", "flexGrow", "flexShrink", "flexBasis", "justifyContent",
    "alignItems", "alignSelf", "alignContent", "order", "gap", "rowGap", "columnGap", "grid", "gridTemplateColumns",
    "gridTemplateRows", "gridColumn", "gridRow", "gridArea", "transform", "transition", "animation", "cursor",
    "pointerEvents", "userSelect", "content", "listStyle", "listStyleType", "outline", "letterSpacing", "wordBreak",
    "textOverflow", "textIndent", "fill", "stroke", "filter", "inset", "aspectRatio", "objectFit", "tableLayout",
    "borderCollapse", "borderSpacing", "mask", "maskImage", "webkitMaskImage", "clipPath", "willChange"};

}  // namespace script_detail
using namespace script_detail;

struct PageScript::Impl {
    Impl(Document& d, Host h) : doc(d), host(std::move(h)), start(std::chrono::steady_clock::now()) {
        // What the page holds outside the engine, for every collection (including one
        // mid-script, while an event handler or timer runs).
        rt.host_roots = [this](std::vector<Value>& roots) { hostRoots(roots); };
    }

    void hostRoots(std::vector<Value>& roots) {
        for (auto& [n, w] : wrappers) roots.push_back(Value::object(w));
        domRoots(roots);
        for (const Timer& t : timers) {
            roots.push_back(t.fn);
            roots.insert(roots.end(), t.args.begin(), t.args.end());
        }
        for (auto& [id, fn] : frames) roots.push_back(fn);
        for (auto& [id, fn] : running_frames) roots.push_back(fn);
        for (Object* p : pending_fetches) roots.push_back(Value::object(p));
        for (Object* p : {proto_target, proto_node, proto_element, proto_document, proto_nodelist, proto_text, proto_event, proto_style, proto_classlist,
                          proto_html_element, proto_svg_element, proto_unknown, proto_fragment, proto_shadow_root})
            if (p) roots.push_back(Value::object(p));
        for (auto& [name, p] : interface_protos) roots.push_back(Value::object(p));
    }

    Runtime rt;
    Document& doc;
    Host host;
    std::chrono::steady_clock::time_point start;
    std::unordered_map<const Node*, Object*> wrappers;
    Object* proto_target = nullptr;
    Object* proto_node = nullptr;
    Object* proto_element = nullptr;
    Object* proto_document = nullptr;
    Object* proto_nodelist = nullptr;   // NodeList.prototype (and HTMLCollection's): an Array.prototype below
    Object* proto_text = nullptr;
    Object* proto_event = nullptr;
    Object* proto_style = nullptr;
    Object* proto_classlist = nullptr;
    js::Symbol* listeners = nullptr;   // where a target keeps its listeners
    js::Symbol* stop_flag = nullptr;
    js::Symbol* stop_now_flag = nullptr;
    bool dirty = false;
    std::string ready_state = "loading";
    Node* current_script = nullptr;
    bool running_scripts = false;
    size_t history_length = 1;

    struct Timer {
        int id;
        double due;
        double interval;
        bool repeat;
        Value fn;
        std::vector<Value> args;
    };
    std::vector<Timer> timers;
    std::vector<std::pair<int, Value>> frames;
    // This round's animation frame callbacks, while they run (the next round's go to
    // frames): a rooted place, as a collection may run inside any of them.
    std::vector<std::pair<int, Value>> running_frames;
    std::vector<Object*> pending_fetches;   // fetch promises waiting on the browser
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    int next_timer = 1;
    size_t live_after_gc = 0;

    static std::map<std::string, std::map<std::u16string, std::u16string>>& storage() {
        static std::map<std::string, std::map<std::u16string, std::u16string>> s;   // by origin, for this run
        return s;
    }

    double now() const { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }

    void report(const std::string& message) {
        if (host.console) host.console("error", message);
    }

    // Runs one task's call, reporting what escapes it.
    template <typename F>
    void task(F&& f) {
        const size_t envs = rt.env_stack.size(), calls = rt.call_depth, evals = rt.eval_depth;
        if (envs == 0 && calls == 0) rt.resetSteps();   // a new task, not one inside another
        try {
            f();
        } catch (const js::Thrown& t) {
            try {
                report("Uncaught " + js::describeValue(rt, t.value) + js::describeWhere(rt, t.value));
            } catch (...) {
                report("Uncaught exception");
            }
        } catch (const js::Terminated& t) {
            report("Script stopped: " + t.why);
        }
        rt.env_stack.resize(envs);
        rt.call_depth = calls;
        rt.eval_depth = evals;
    }

    // A microtask checkpoint: promise reactions, await continuations, queueMicrotask.
    // The engine skips it while script is still running (an event a script dispatched).
    void drainMicrotasks() {
        task([&] { rt.runJobs(); });
    }

    void collect() {
        if (rt.heapSize() < live_after_gc * 2 + 200'000) return;
        rt.collectGarbage();   // the page's own values come from host_roots
        live_after_gc = rt.heapSize();
    }

    // ── Wrappers ──

    Value wrap(const Node* n) {
        if (!n) return Value::null();
        if (const auto it = wrappers.find(n); it != wrappers.end()) return Value::object(it->second);
        Object* proto = protoFor(n);
        Object* w = rt.allocate<Object>(proto);
        w->host = const_cast<Node*>(n);
        w->host_kind = kNodeWrapper;
        wrappers[n] = w;
        return Value::object(w);
    }

    Node* node(const Value& v) {
        if (!v.isObject() || v.asObject()->host_kind != kNodeWrapper) return nullptr;
        return static_cast<Node*>(v.asObject()->host);
    }

    Node* thisNode(const Value& t) {
        Node* n = node(t);
        if (!n) rt.throwError("TypeError", "not a node");
        return n;
    }

    // The document a document method was called on: a script's own, or the page's.
    Node* docNode(const Value& t) {
        Node* n = node(t);
        return n && n->type == NodeType::Document ? n : doc.root();
    }
    static Node* documentElementOf(Node* d) {
        if (d)
            for (Node* c : d->children)
                if (c->isElement()) return c;
        return nullptr;
    }
    static Node* childElementNamed(Node* parent, std::string_view tag) {
        if (parent)
            for (Node* c : parent->children)
                if (c->isHtml(tag)) return c;
        return nullptr;
    }

    Node* thisElement(const Value& t) {
        Node* n = thisNode(t);
        if (!n->isElement()) rt.throwError("TypeError", "not an element");
        return n;
    }

    Value list(const std::vector<Node*>& nodes) {
        std::vector<Value> items;
        items.reserve(nodes.size());
        for (Node* n : nodes) items.push_back(wrap(n));
        Object* a = rt.newArray(std::move(items));
        if (proto_nodelist) a->prototype = proto_nodelist;   // instanceof NodeList; still an array (item(), namedItem() there)
        return Value::object(a);
    }

    void accessor(Object* o, const char16_t* name, js::NativeFunction get, js::NativeFunction set = nullptr) {
        js::Property p;
        p.flags = js::kConfigurable | js::kAccessor | js::kEnumerable;
        p.getter = rt.newFunction(std::u16string(u"get ") + name, 0, std::move(get));
        if (set) p.setter = rt.newFunction(std::u16string(u"set ") + name, 1, std::move(set));
        o->define(name, p);
    }

    void method(Object* o, const char16_t* name, int length, js::NativeFunction fn) { rt.method(o, name, length, std::move(fn)); }

    // ── Tree changes ──

    // Connected: in the document, or in a shadow tree whose host is (shadow-including).
    bool inDocument(const Node* n) const {
        for (int hops = 0; hops < 10'000; ++hops) {
            while (n->parent) n = n->parent;
            if (n->type == NodeType::Fragment && n->shadow_host) {
                n = n->shadow_host;
                continue;
            }
            return n == doc.root();
        }
        return false;
    }

    void insert(Node* parent, Node* child, Node* before) {
        ++dom_version;
        if (child == parent) rt.throwError("Error", "HierarchyRequestError: a node can't contain itself");
        for (const Node* p = parent; p; p = p->parent)
            if (p == child) rt.throwError("Error", "HierarchyRequestError: a node can't contain its ancestor");
        if (before && before->parent != parent) rt.throwError("Error", "NotFoundError: the reference node isn't a child");
        if (child->type == NodeType::Fragment) {
            if (child->shadow_host) rt.throwError("Error", "HierarchyRequestError: a shadow root can't be inserted");
            const std::vector<Node*> kids = child->children;
            for (Node* k : kids) insert(parent, k, before);
            return;
        }
        if (child->parent) detach(child);
        Node* prev = nullptr;
        if (before) {
            const auto it = std::find(parent->children.begin(), parent->children.end(), before);
            if (it != parent->children.begin()) prev = *(it - 1);
        } else if (!parent->children.empty()) {
            prev = parent->children.back();
        }
        parent->insertBefore(child, before);
        dirty = true;
        afterInsert(parent, child, prev, before);
    }

    void detach(Node* child) {
        ++dom_version;
        Node* parent = child->parent;
        if (!parent) return;
        const auto it = std::find(parent->children.begin(), parent->children.end(), child);
        Node* prev = it != parent->children.begin() && it != parent->children.end() ? *(it - 1) : nullptr;
        Node* next = it != parent->children.end() && it + 1 != parent->children.end() ? *(it + 1) : nullptr;
        const bool was_connected = inDocument(parent);
        parent->remove(child);
        dirty = true;
        afterRemove(parent, child, prev, next, was_connected);
    }

    Node* clone(const Node* src, bool deep, int depth = 0) {
        Node* n = doc.make(src->type);
        n->ns = src->ns;
        n->name = src->name;
        n->data = src->data;
        n->attrs = src->attrs;
        if (deep && depth < kMaxCloneDepth) {
            for (const Node* c : src->children) n->append(clone(c, true, depth + 1));
            // A <template>'s contents are copied with it (the loaders of web-component
            // polyfills check exactly this).
            if (src->template_content) {
                n->template_content = doc.make(NodeType::Fragment);
                for (const Node* c : src->template_content->children) n->template_content->append(clone(c, true, depth + 1));
            }
        }
        return n;
    }

    void setText(Node* n, const std::string& text) {
        if (!n->isElement() && n->type != NodeType::Fragment) {
            const std::string old = n->data;
            n->data = text;
            dirty = true;
            afterCharacterData(n, old);
            return;
        }
        while (!n->children.empty()) detach(n->children.back());
        if (!text.empty()) {
            Node* t = doc.make(NodeType::Text);
            t->data = text;
            insert(n, t, nullptr);
        }
        dirty = true;
    }

    void setInnerHtml(Node* n, const std::string& html) {
        auto frag = parseFragment(html, n->ns == Namespace::Html && !n->name.empty() ? n->name : "div");
        Node* target = n->template_content ? n->template_content : n;
        while (!target->children.empty()) detach(target->children.back());
        if (const Node* holder = frag->documentElement())
            for (const Node* c : holder->children) insert(target, clone(c, true), nullptr);
        dirty = true;
    }

    std::string outerHtml(const Node* n) {
        if (!n->isElement()) return n->type == NodeType::Text ? n->data : "";
        std::string out = "<" + n->name;
        for (const auto& a : n->attrs) out += " " + a.name + "=\"" + escapeAttr(a.value) + "\"";
        out += ">";
        static const char* kVoid[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"};
        if (std::any_of(std::begin(kVoid), std::end(kVoid), [&](const char* v) { return n->name == v; })) return out;
        return out + serializeChildren(*n) + "</" + n->name + ">";
    }

    void setAttr(Node* n, const std::string& name, const std::string& value) {
        for (auto& a : n->attrs)
            if (a.name == name) {
                const std::string old = a.value;
                a.value = value;
                dirty = true;
                afterAttr(n, name, &old);
                return;
            }
        n->attrs.push_back({name, value});
        dirty = true;
        afterAttr(n, name, nullptr);
    }

    void removeAttr(Node* n, const std::string& name) {
        const std::string* had = n->attr(name);
        if (!had) return;
        const std::string old = *had;
        const auto it = std::remove_if(n->attrs.begin(), n->attrs.end(), [&](const Attribute& a) { return a.name == name; });
        n->attrs.erase(it, n->attrs.end());
        dirty = true;
        afterAttr(n, name, &old);
    }

    std::string attr(const Node* n, const char* name) {
        const std::string* v = n->attr(name);
        return v ? *v : std::string{};
    }

    // ── Events ──

    Object* makeEvent(const std::u16string& type, bool bubbles, bool cancelable) {
        Object* e = rt.newObject(proto_event);
        rt.put(e, u"type", Value::string(type), js::kEnumerable);
        rt.put(e, u"bubbles", Value::boolean(bubbles), js::kEnumerable);
        rt.put(e, u"cancelable", Value::boolean(cancelable), js::kEnumerable);
        rt.put(e, u"defaultPrevented", Value::boolean(false), js::kEnumerable | js::kWritable);
        rt.put(e, u"target", Value::null(), js::kEnumerable | js::kWritable);
        rt.put(e, u"currentTarget", Value::null(), js::kEnumerable | js::kWritable);
        rt.put(e, u"eventPhase", Value::number(0), js::kEnumerable | js::kWritable);
        rt.put(e, u"timeStamp", Value::number(now()), js::kEnumerable);
        rt.put(e, u"isTrusted", Value::boolean(true), js::kEnumerable);
        return e;
    }

    Object* listenerList(Object* target, bool make) {
        js::Property* p = target->own(PropertyKey(listeners));
        if (p) return p->value.asObject();
        if (!make) return nullptr;
        Object* l = rt.newArray();
        target->define(PropertyKey(listeners), {Value::object(l), nullptr, nullptr, js::kWritable});
        return l;
    }

    void addListener(Object* target, const Value& type, const Value& fn, const Value& options) {
        if (fn.isNullish()) return;
        bool capture = false, once = false;
        if (options.isBoolean()) capture = options.asBoolean();
        else if (options.isObject()) {
            capture = rt.toBoolean(rt.get(options, u"capture"));
            once = rt.toBoolean(rt.get(options, u"once"));
        }
        Object* l = listenerList(target, true);
        const std::u16string t = rt.toStringValue(type);
        for (const Value& e : l->elements)
            if (e.isObject() && rt.toStringValue(rt.get(e, u"type")) == t && Runtime::strictEquals(rt.get(e, u"fn"), fn) &&
                rt.toBoolean(rt.get(e, u"capture")) == capture)
                return;
        Object* entry = rt.newObject();
        rt.put(entry, u"type", Value::string(t), js::kDefaultFlags);
        rt.put(entry, u"fn", fn, js::kDefaultFlags);
        rt.put(entry, u"capture", Value::boolean(capture), js::kDefaultFlags);
        rt.put(entry, u"once", Value::boolean(once), js::kDefaultFlags);
        if (l->elements.size() < 10'000) l->elements.push_back(Value::object(entry));
    }

    void removeListener(Object* target, const Value& type, const Value& fn, const Value& options) {
        Object* l = listenerList(target, false);
        if (!l) return;
        const bool capture = options.isBoolean() ? options.asBoolean() : options.isObject() && rt.toBoolean(rt.get(options, u"capture"));
        const std::u16string t = rt.toStringValue(type);
        std::erase_if(l->elements, [&](const Value& e) {
            return e.isObject() && rt.toStringValue(rt.get(e, u"type")) == t && Runtime::strictEquals(rt.get(e, u"fn"), fn) &&
                   rt.toBoolean(rt.get(e, u"capture")) == capture;
        });
    }

    // Calls the listeners on one target for this phase; false once propagation stops.
    void invoke(Object* target, Object* event, const std::u16string& type, int phase) {
        // window.event: the one being dispatched, while its listeners run.
        struct CurrentEvent {
            Runtime& rt;
            std::vector<Value> before;   // (a list the collector sees)
            CurrentEvent(Runtime& r, Object* e) : rt(r), before{r.get(Value::object(r.global()), u"event")} {
                rt.temp_lists.push_back(&before);
                rt.put(rt.global(), u"event", Value::object(e), js::kWritable | js::kConfigurable);
            }
            ~CurrentEvent() {
                rt.put(rt.global(), u"event", before[0], js::kWritable | js::kConfigurable);
                rt.temp_lists.pop_back();
            }
            CurrentEvent(const CurrentEvent&) = delete;
            CurrentEvent& operator=(const CurrentEvent&) = delete;
        } current(rt, event);
        rt.put(event, u"currentTarget", Value::object(target), js::kEnumerable | js::kWritable);
        rt.put(event, u"eventPhase", Value::number(phase), js::kEnumerable | js::kWritable);
        if (Object* l = listenerList(target, false)) {
            const std::vector<Value> snapshot = l->elements;
            for (const Value& e : snapshot) {
                if (!e.isObject() || rt.toStringValue(rt.get(e, u"type")) != type) continue;
                const bool capture = rt.toBoolean(rt.get(e, u"capture"));
                if ((phase == 1 && !capture) || (phase == 3 && capture)) continue;
                if (rt.toBoolean(rt.get(e, u"once"))) std::erase_if(l->elements, [&](const Value& x) { return Runtime::strictEquals(x, e); });
                const Value fn = rt.get(e, u"fn");
                const Value ev = Value::object(event);
                task([&] {
                    if (rt.isCallable(fn)) rt.call(fn, Value::object(target), std::span<const Value>(&ev, 1));
                    else if (fn.isObject()) rt.call(rt.get(fn, u"handleEvent"), fn, std::span<const Value>(&ev, 1));
                });
                if (event->own(PropertyKey(stop_now_flag))) return;
            }
        }
        // on<type>: a property set by script, or an attribute in the page.
        if (phase != 1) {
            const std::u16string on = u"on" + type;
            Value handler;
            if (const js::Property* p = target->own(PropertyKey(on)); p && !(p->flags & js::kAccessor)) handler = p->value;
            if (!rt.isCallable(handler) && target->host_kind == kNodeWrapper) {
                const Node* n = static_cast<Node*>(target->host);
                if (const std::string* code = n->attr(js::utf8(on))) {
                    task([&] {
                        const Runtime::Result r = rt.evaluate("(function (event) {\n" + *code + "\n})", "handler");
                        if (r.ok) handler = r.value;
                        else report(r.error);
                    });
                }
            } else if (!rt.isCallable(handler) && target == rt.global()) {
                // (window.onload etc. are plain properties of the global object)
            }
            if (target == rt.global())
                if (const js::Property* p = rt.global()->own(PropertyKey(on)); p && !(p->flags & js::kAccessor)) handler = p->value;
            if (rt.isCallable(handler)) {
                const Value ev = Value::object(event);
                task([&] {
                    const Value r = rt.call(handler, Value::object(target), std::span<const Value>(&ev, 1));
                    if (r.isBoolean() && !r.asBoolean()) preventDefault(event);
                });
            }
        }
    }

    void preventDefault(Object* event) {
        if (rt.toBoolean(rt.get(Value::object(event), u"cancelable"))) rt.put(event, u"defaultPrevented", Value::boolean(true), js::kEnumerable | js::kWritable);
    }

    // Dispatches through the target's ancestors and the window; true unless cancelled.
    bool dispatch(Object* target_obj, Object* event) {
        const std::u16string type = rt.toStringValue(rt.get(Value::object(event), u"type"));
        const bool bubbles = rt.toBoolean(rt.get(Value::object(event), u"bubbles"));
        rt.put(event, u"target", Value::object(target_obj), js::kEnumerable | js::kWritable);
        // The window's load says the document is its target (the legacy target override).
        if (target_obj == rt.global() && type == u"load") rt.put(event, u"target", wrap(doc.root()), js::kEnumerable | js::kWritable);
        event->remove(PropertyKey(stop_flag));
        event->remove(PropertyKey(stop_now_flag));
        std::vector<Object*> path;   // from the target outward
        path.push_back(target_obj);
        if (target_obj->host_kind == kNodeWrapper) {
            const Node* n = static_cast<Node*>(target_obj->host);
            // Up through shadow roots to their hosts: events from inside a shadow tree
            // reach the page around it.
            for (const Node* p = n->parent; p; p = p->parent ? p->parent : p->shadow_host) path.push_back(wrap(p).asObject());
            if (inDocument(n) && type != u"load") path.push_back(rt.global());
        }
        const auto stopped = [&] { return event->own(PropertyKey(stop_flag)) || event->own(PropertyKey(stop_now_flag)); };
        for (size_t i = path.size(); i-- > 1 && !stopped();) invoke(path[i], event, type, 1);
        if (!stopped()) invoke(path[0], event, type, 2);
        if (bubbles)
            for (size_t i = 1; i < path.size() && !stopped(); ++i) invoke(path[i], event, type, 3);
        rt.put(event, u"currentTarget", Value::null(), js::kEnumerable | js::kWritable);
        rt.put(event, u"eventPhase", Value::number(0), js::kEnumerable | js::kWritable);
        drainMicrotasks();
        return !rt.toBoolean(rt.get(Value::object(event), u"defaultPrevented"));
    }

    bool fire(const Node* target, const std::u16string& type, bool bubbles, bool cancelable,
              const std::function<void(Object*)>& more = nullptr) {
        Object* e = makeEvent(type, bubbles, cancelable);
        if (more) more(e);
        return dispatch(target ? wrap(target).asObject() : rt.global(), e);
    }

    // ── Scripts ──

    static std::string trimmed(const std::string& s) {
        const size_t b = s.find_first_not_of(" \t\n\r\f");
        const size_t e = s.find_last_not_of(" \t\n\r\f");
        return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
    }

    static bool isModule(const Node* s) {
        const std::string* type = s->attr("type");
        return type && lower(trimmed(*type)) == "module";
    }

    static bool runnable(const Node* s) {
        if (isModule(s)) return true;
        if (s->attr("nomodule")) return false;   // the fallback for browsers without modules
        const std::string* type = s->attr("type");
        if (!type) return true;
        const std::string t = lower(trimmed(*type));
        return t.empty() || t == "text/javascript" || t == "application/javascript" || t == "text/ecmascript" ||
               t == "application/x-javascript";
    }

    // Deferred: modules, and external classic scripts with defer or async. They run
    // once the document's parsed, in order.
    static bool deferred(const Node* s) {
        if (isModule(s)) return true;
        return s->attr("src") && (s->attr("defer") || s->attr("async"));
    }

    std::function<std::optional<std::string>(const std::string&)> source;
    std::vector<const Node*> ran;   // each script runs once

    void runScript(Node* s) {
        if (std::find(ran.begin(), ran.end(), s) != ran.end() || !runnable(s)) return;
        ran.push_back(s);
        if (const std::string* src = s->attr("src")) {
            const std::string url = resolveUrl(host.base, *src);
            std::optional<std::string> got = source ? source(url) : std::nullopt;
            if (got) {
                runScriptCode(s, std::move(*got), url);
                return;
            }
            // Not fetched ahead (a script added later, as bundlers load their chunks):
            // fetched now, and run when it comes, as browsers run inserted scripts.
            if (host.fetch_script && pending_script_fetches < kMaxPendingScriptFetches) {
                ++pending_script_fetches;
                host.fetch_script(url, [this, s, url](std::optional<std::string> body) {
                    --pending_script_fetches;
                    rt.setStepBudget(kTaskBudget);
                    if (!body) {
                        task([&] { fire(s, u"error", false, false); });
                    } else {
                        runScriptCode(s, std::move(*body), url);
                    }
                    drainMicrotasks();
                    dirty = true;
                });
                return;
            }
            fire(s, u"error", false, false);
            return;
        }
        runScriptCode(s, s->textContent(), "inline script");
    }

    static constexpr int kMaxPendingScriptFetches = 64;
    int pending_script_fetches = 0;

    void runScriptCode(Node* s, std::string code, const std::string& name) {
        Node* saved = current_script;
        rt.setStepBudget(kTaskBudget);
        if (isModule(s)) {
            // document.currentScript is null for modules. Its imports load through the
            // same fetch as scripts; it finishes when they've loaded and it's run.
            const std::string url = s->attr("src") ? name : host.url;
            const bool has_src = s->attr("src") != nullptr;
            current_script = nullptr;
            rt.evaluateModule(code, url, [this, name](Runtime::Result r) {
                if (!r.ok) report(r.error + (r.where.empty() ? " (" + name + ")" : r.where));
            });
            current_script = saved;
            drainMicrotasks();
            if (has_src) fire(s, u"load", false, false);
            return;
        }
        current_script = s;
        const Runtime::Result r = rt.evaluate(code, name);
        if (!r.ok) report(r.error + (r.where.empty() ? " (" + name + ")" : r.where));
        current_script = saved;
        drainMicrotasks();
        if (s->attr("src")) fire(s, u"load", false, false);
    }

    // Scripts that just came into the document (appendChild, innerHTML) run now.
    void scriptsInserted(Node* root) {
        // Even during the first pass: its list was made before any script ran, so one a
        // script adds is never in it. (Each runs once whichever way it's reached.)
        std::vector<Node*> found;
        std::vector<Node*> todo{root};
        while (!todo.empty()) {
            Node* n = todo.back();
            todo.pop_back();
            if (n->isHtml("script")) found.push_back(n);
            todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        }
        for (Node* s : found) runScript(s);
    }

    // ── Setting up ──

    // ── The DOM's newer parts (script_dom.cc) ──

    struct CustomDefinition {
        std::string name, local_name;   // local_name differs for a customized built-in (extends)
        Object* ctor = nullptr;
        Object* proto = nullptr;
        Value connected, disconnected, adopted, attribute_changed;
        std::vector<std::string> observed;
    };
    std::map<std::string, CustomDefinition> custom_definitions;
    std::map<std::string, Object*> when_defined;   // pending customElements.whenDefined promises
    // Elements being upgraded, innermost last, with their definition's constructor: each
    // definition has its own construction stack (one element's constructor may make others).
    std::vector<std::pair<Object*, Node*>> upgrade_candidates;
    struct ObserverState {
        Object* object = nullptr;     // the MutationObserver; its callback is slots[0]
        std::vector<Object*> records;
    };
    struct Registration {
        ObserverState* observer = nullptr;
        bool child_list = false, attributes = false, character_data = false, subtree = false;
        bool attribute_old_value = false, character_data_old_value = false;
        std::vector<std::string> attribute_filter;
        bool has_filter = false;
    };
    std::vector<std::unique_ptr<ObserverState>> observers;
    std::unordered_map<const Node*, std::vector<Registration>> registrations;
    bool mutation_delivery_queued = false;
    std::vector<std::pair<Node*, Object*>> adopters;   // shadow roots (or the document) and their adoptedStyleSheets arrays
    std::map<std::string, Object*> interface_protos;  // HTMLDivElement.prototype and the rest
    Object* proto_html_element = nullptr;
    Object* proto_svg_element = nullptr;
    Object* proto_unknown = nullptr;
    Object* proto_fragment = nullptr;
    Object* proto_shadow_root = nullptr;
    Object* proto_comment = nullptr;
    Object* proto_doctype = nullptr;
    Object* proto_mutation_observer = nullptr;

    Object* protoFor(const Node* n);
    void installDom();
    void domRoots(std::vector<Value>& roots);
    void afterInsert(Node* parent, Node* child, Node* prev, Node* next);
    void afterRemove(Node* parent, Node* child, Node* prev, Node* next, bool was_connected);
    void afterAttr(Node* n, const std::string& name, const std::string* old);
    void afterCharacterData(Node* n, const std::string& old);
    const CustomDefinition* definitionFor(const Node* el) const;
    void upgrade(Node* el, const CustomDefinition& def);
    void callReaction(Node* el, const Value& callback, std::vector<Value> args);
    void queueMutation(const char16_t* type, Node* target, const std::vector<Node*>& added, const std::vector<Node*>& removed,
                       Node* prev, Node* next, const std::string* attr_name, const std::string* old_value);

    Value browserFetch(Args a);   // script_fetch.cc

    // 2D canvases (script_canvas.cc): each <canvas> a script drew on, its pixels and clip.
    struct CanvasSurface {
        std::unique_ptr<Canvas> pixels;
        std::unique_ptr<Canvas> clip;   // coverage in its alpha; none: no clip
        uint64_t version = 1;
        mutable std::shared_ptr<const Raster> shown;   // pixels as a Raster, made when asked
        mutable uint64_t shown_version = 0;
    };
    std::unordered_map<const Node*, std::unique_ptr<CanvasSurface>> canvases;
    std::vector<Node*> script_images;   // <img>s a script gave a src, in the page or not
    uint64_t dom_version = 0;           // bumped by every change to the tree or an attribute: live lists recount
    CanvasSurface& surfaceOf(Node* canvas);
    void installCanvas();

    void install();
    void installNode();
    void installElement();
    void installDocument();
    void installWindow();
};

}  // namespace fbx::xweb
