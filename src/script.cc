// A page's scripts: our engine bound to the page's tree. window, document, Node and
// Element, events and their dispatch, timers and animation frames, as the DOM and HTML
// standards describe them, the parts pages use most first.

#include "xweb/script.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

#include "xweb/css.h"
#include "xweb/html_parser.h"
#include "xweb/js.h"
#include "xweb/query.h"
#include "xweb/url.h"

namespace fbx::xweb {

extern const char* const kScriptPrelude;   // script_prelude.cc


using js::Object;
using js::PropertyKey;
using js::Runtime;
using js::Value;
using Args = std::span<const Value>;

namespace {

constexpr uint8_t kNodeWrapper = 1, kStyleWrapper = 2, kClassListWrapper = 3;
constexpr uint64_t kTaskBudget = 50'000'000;   // steps one script, event or timer may take
constexpr size_t kMaxTimers = 10'000;
constexpr int kMaxCloneDepth = 512;

Value arg(Args a, size_t i) { return i < a.size() ? a[i] : Value::undefined(); }
Value str(const std::string& s) { return Value::string(js::utf16(s)); }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// "background-color" from "backgroundColor", and back.
std::string cssName(const std::string& camel) {
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

std::string camelName(const std::string& css) {
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
std::vector<std::pair<std::string, std::string>> parseStyle(const std::string& text) {
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

std::string serializeStyle(const std::vector<std::pair<std::string, std::string>>& decls) {
    std::string out;
    for (const auto& [n, v] : decls) {
        if (!out.empty()) out += " ";
        out += n + ": " + v + ";";
    }
    return out;
}

std::string escapeAttr(const std::string& v) {
    std::string out;
    for (char c : v) {
        if (c == '&') out += "&amp;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

const char* kStyleProperties[] = {
    "display", "position", "top", "right", "bottom", "left", "width", "height", "minWidth", "minHeight", "maxWidth",
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

}  // namespace

struct PageScript::Impl {
    Impl(Document& d, Host h) : doc(d), host(std::move(h)), start(std::chrono::steady_clock::now()) {}

    Runtime rt;
    Document& doc;
    Host host;
    std::chrono::steady_clock::time_point start;
    std::unordered_map<const Node*, Object*> wrappers;
    Object* proto_target = nullptr;
    Object* proto_node = nullptr;
    Object* proto_element = nullptr;
    Object* proto_document = nullptr;
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
                report("Uncaught " + js::describe(rt, t.value));
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
        rt.extra_roots.clear();
        for (auto& [n, w] : wrappers) rt.extra_roots.push_back(Value::object(w));
        for (const Timer& t : timers) {
            rt.extra_roots.push_back(t.fn);
            rt.extra_roots.insert(rt.extra_roots.end(), t.args.begin(), t.args.end());
        }
        for (auto& [id, fn] : frames) rt.extra_roots.push_back(fn);
        rt.collectGarbage();
        rt.extra_roots.clear();
        live_after_gc = rt.heapSize();
    }

    // ── Wrappers ──

    Value wrap(const Node* n) {
        if (!n) return Value::null();
        if (const auto it = wrappers.find(n); it != wrappers.end()) return Value::object(it->second);
        Object* proto = n->type == NodeType::Document ? proto_document : n->isElement() ? proto_element : proto_text;
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
        rt.method(a, u"item", 1, [this](Runtime& r, const Value& t, Args args) {
            const Value v = r.get(t, PropertyKey(Runtime::numberToString(r.toIntegerOrInfinity(arg(args, 0)))));
            return v.isUndefined() ? Value::null() : v;
        });
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

    bool inDocument(const Node* n) const {
        while (n->parent) n = n->parent;
        return n == doc.root();
    }

    void insert(Node* parent, Node* child, Node* before) {
        if (child == parent) rt.throwError("Error", "HierarchyRequestError: a node can't contain itself");
        for (const Node* p = parent; p; p = p->parent)
            if (p == child) rt.throwError("Error", "HierarchyRequestError: a node can't contain its ancestor");
        if (before && before->parent != parent) rt.throwError("Error", "NotFoundError: the reference node isn't a child");
        if (child->type == NodeType::Fragment) {
            const std::vector<Node*> kids = child->children;
            for (Node* k : kids) insert(parent, k, before);
            return;
        }
        parent->insertBefore(child, before);
        dirty = true;
        if (inDocument(parent)) scriptsInserted(child);
    }

    void detach(Node* child) {
        if (child->parent) {
            child->parent->remove(child);
            dirty = true;
        }
    }

    Node* clone(const Node* src, bool deep, int depth = 0) {
        Node* n = doc.make(src->type);
        n->ns = src->ns;
        n->name = src->name;
        n->data = src->data;
        n->attrs = src->attrs;
        if (deep && depth < kMaxCloneDepth)
            for (const Node* c : src->children) n->append(clone(c, true, depth + 1));
        return n;
    }

    void setText(Node* n, const std::string& text) {
        if (!n->isElement() && n->type != NodeType::Fragment) {
            n->data = text;
            dirty = true;
            return;
        }
        while (!n->children.empty()) n->remove(n->children.back());
        if (!text.empty()) {
            Node* t = doc.make(NodeType::Text);
            t->data = text;
            n->append(t);
        }
        dirty = true;
    }

    void setInnerHtml(Node* n, const std::string& html) {
        auto frag = parseFragment(html, n->ns == Namespace::Html ? n->name : "div");
        while (!n->children.empty()) n->remove(n->children.back());
        if (const Node* holder = frag->documentElement())
            for (const Node* c : holder->children) n->append(clone(c, true));
        dirty = true;
        if (inDocument(n)) scriptsInserted(n);
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
                a.value = value;
                dirty = true;
                return;
            }
        n->attrs.push_back({name, value});
        dirty = true;
    }

    void removeAttr(Node* n, const std::string& name) {
        const auto it = std::remove_if(n->attrs.begin(), n->attrs.end(), [&](const Attribute& a) { return a.name == name; });
        if (it != n->attrs.end()) {
            n->attrs.erase(it, n->attrs.end());
            dirty = true;
        }
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
        event->remove(PropertyKey(stop_flag));
        event->remove(PropertyKey(stop_now_flag));
        std::vector<Object*> path;   // from the target outward
        path.push_back(target_obj);
        if (target_obj->host_kind == kNodeWrapper) {
            const Node* n = static_cast<Node*>(target_obj->host);
            for (const Node* p = n->parent; p; p = p->parent) path.push_back(wrap(p).asObject());
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
        std::string code;
        std::string name = "inline script";
        if (const std::string* src = s->attr("src")) {
            const std::string url = resolveUrl(host.base, *src);
            name = url;
            std::optional<std::string> got = source ? source(url) : std::nullopt;
            if (!got) {
                fire(s, u"error", false, false);
                return;
            }
            code = std::move(*got);
        } else {
            code = s->textContent();
        }
        Node* saved = current_script;
        rt.setStepBudget(kTaskBudget);
        if (isModule(s)) {
            // document.currentScript is null for modules. Its imports load through the
            // same fetch as scripts; it finishes when they've loaded and it's run.
            const std::string url = s->attr("src") ? name : host.url;
            const bool has_src = s->attr("src") != nullptr;
            current_script = nullptr;
            rt.evaluateModule(code, url, [this, name](Runtime::Result r) {
                if (!r.ok) report(r.error + " (" + name + ")");
            });
            current_script = saved;
            drainMicrotasks();
            if (has_src) fire(s, u"load", false, false);
            return;
        }
        current_script = s;
        const Runtime::Result r = rt.evaluate(code, name);
        if (!r.ok) report(r.error + " (" + name + ")");
        current_script = saved;
        drainMicrotasks();
        if (s->attr("src")) fire(s, u"load", false, false);
    }

    // Scripts that just came into the document (appendChild, innerHTML) run now.
    void scriptsInserted(Node* root) {
        if (running_scripts && ready_state == "loading") return;   // the initial pass will get them
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

    void install();
    void installNode();
    void installElement();
    void installDocument();
    void installWindow();
};

// ── Node ──────────────────────────────────────────────────────────────────────

void PageScript::Impl::installNode() {
    Impl* self = this;
    Object* p = proto_node;
    accessor(p, u"nodeType", [self](Runtime&, const Value& t, Args) {
        const Node* n = self->thisNode(t);
        switch (n->type) {
            case NodeType::Element: return Value::number(1);
            case NodeType::Text: return Value::number(3);
            case NodeType::Comment: return Value::number(8);
            case NodeType::Document: return Value::number(9);
            case NodeType::Doctype: return Value::number(10);
            case NodeType::Fragment: return Value::number(11);
        }
        return Value::number(1);
    });
    accessor(p, u"nodeName", [self](Runtime&, const Value& t, Args) {
        const Node* n = self->thisNode(t);
        switch (n->type) {
            case NodeType::Element: return str(n->ns == Namespace::Html ? upper(n->name) : n->name);
            case NodeType::Text: return str("#text");
            case NodeType::Comment: return str("#comment");
            case NodeType::Document: return str("#document");
            case NodeType::Fragment: return str("#document-fragment");
            default: return str(n->name);
        }
    });
    accessor(p, u"nodeValue", [self](Runtime&, const Value& t, Args) {
        const Node* n = self->thisNode(t);
        return n->type == NodeType::Text || n->type == NodeType::Comment ? str(n->data) : Value::null();
    }, [self](Runtime& rt, const Value& t, Args a) {
        Node* n = self->thisNode(t);
        if (n->type == NodeType::Text || n->type == NodeType::Comment) self->setText(n, js::utf8(rt.toStringValue(arg(a, 0))));
        return Value::undefined();
    });
    accessor(p, u"textContent", [self](Runtime&, const Value& t, Args) {
        const Node* n = self->thisNode(t);
        if (n->type == NodeType::Document) return Value::null();
        return str(n->isElement() || n->type == NodeType::Fragment ? n->textContent() : n->data);
    }, [self](Runtime& rt, const Value& t, Args a) {
        self->setText(self->thisNode(t), arg(a, 0).isNull() ? std::string{} : js::utf8(rt.toStringValue(arg(a, 0))));
        return Value::undefined();
    });
    accessor(p, u"parentNode", [self](Runtime&, const Value& t, Args) { return self->wrap(self->thisNode(t)->parent); });
    accessor(p, u"parentElement", [self](Runtime&, const Value& t, Args) {
        const Node* par = self->thisNode(t)->parent;
        return par && par->isElement() ? self->wrap(par) : Value::null();
    });
    accessor(p, u"childNodes", [self](Runtime&, const Value& t, Args) { return self->list(self->thisNode(t)->children); });
    accessor(p, u"firstChild", [self](Runtime&, const Value& t, Args) {
        const Node* n = self->thisNode(t);
        return n->children.empty() ? Value::null() : self->wrap(n->children.front());
    });
    accessor(p, u"lastChild", [self](Runtime&, const Value& t, Args) {
        const Node* n = self->thisNode(t);
        return n->children.empty() ? Value::null() : self->wrap(n->children.back());
    });
    const auto sibling = [self](int dir, bool elements) {
        return [self, dir, elements](Runtime&, const Value& t, Args) {
            const Node* n = self->thisNode(t);
            if (!n->parent) return Value::null();
            const auto& kids = n->parent->children;
            auto it = std::find(kids.begin(), kids.end(), n);
            while (true) {
                if (dir < 0) {
                    if (it == kids.begin()) return Value::null();
                    --it;
                } else {
                    ++it;
                    if (it == kids.end()) return Value::null();
                }
                if (!elements || (*it)->isElement()) return self->wrap(*it);
            }
        };
    };
    accessor(p, u"nextSibling", sibling(1, false));
    accessor(p, u"previousSibling", sibling(-1, false));
    accessor(p, u"ownerDocument", [self](Runtime&, const Value& t, Args) {
        const Node* n = self->thisNode(t);
        return n->type == NodeType::Document ? Value::null() : self->wrap(self->doc.root());
    });
    accessor(p, u"isConnected", [self](Runtime&, const Value& t, Args) { return Value::boolean(self->inDocument(self->thisNode(t))); });
    method(p, u"hasChildNodes", 0, [self](Runtime&, const Value& t, Args) { return Value::boolean(!self->thisNode(t)->children.empty()); });
    method(p, u"appendChild", 1, [self](Runtime& rt, const Value& t, Args a) {
        Node* c = self->node(arg(a, 0));
        if (!c) rt.throwError("TypeError", "appendChild needs a node");
        self->insert(self->thisNode(t), c, nullptr);
        return arg(a, 0);
    });
    method(p, u"insertBefore", 2, [self](Runtime& rt, const Value& t, Args a) {
        Node* c = self->node(arg(a, 0));
        if (!c) rt.throwError("TypeError", "insertBefore needs a node");
        self->insert(self->thisNode(t), c, self->node(arg(a, 1)));
        return arg(a, 0);
    });
    method(p, u"removeChild", 1, [self](Runtime& rt, const Value& t, Args a) {
        Node* c = self->node(arg(a, 0));
        if (!c || c->parent != self->thisNode(t)) rt.throwError("Error", "NotFoundError: not a child of this node");
        self->detach(c);
        return arg(a, 0);
    });
    method(p, u"replaceChild", 2, [self](Runtime& rt, const Value& t, Args a) {
        Node* parent = self->thisNode(t);
        Node* fresh = self->node(arg(a, 0));
        Node* old = self->node(arg(a, 1));
        if (!fresh || !old || old->parent != parent) rt.throwError("Error", "NotFoundError: not a child of this node");
        if (fresh != old) {
            self->insert(parent, fresh, old);
            self->detach(old);
        }
        return arg(a, 1);
    });
    method(p, u"cloneNode", 1, [self](Runtime& rt, const Value& t, Args a) { return self->wrap(self->clone(self->thisNode(t), rt.toBoolean(arg(a, 0)))); });
    method(p, u"contains", 1, [self](Runtime&, const Value& t, Args a) {
        const Node* n = self->thisNode(t);
        for (const Node* c = self->node(arg(a, 0)); c; c = c->parent)
            if (c == n) return Value::boolean(true);
        return Value::boolean(false);
    });
    method(p, u"getRootNode", 0, [self](Runtime&, const Value& t, Args) {
        const Node* n = self->thisNode(t);
        while (n->parent) n = n->parent;
        return self->wrap(n);
    });
    method(p, u"normalize", 0, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    for (auto [name, value] : std::initializer_list<std::pair<const char16_t*, int>>{
             {u"ELEMENT_NODE", 1}, {u"TEXT_NODE", 3}, {u"COMMENT_NODE", 8}, {u"DOCUMENT_NODE", 9}, {u"DOCUMENT_FRAGMENT_NODE", 11}})
        rt.put(p, name, Value::number(value), 0);
    // Text and comments: data and length.
    accessor(proto_text, u"data", [self](Runtime&, const Value& t, Args) { return str(self->thisNode(t)->data); },
             [self](Runtime& rt, const Value& t, Args a) {
                 self->setText(self->thisNode(t), js::utf8(rt.toStringValue(arg(a, 0))));
                 return Value::undefined();
             });
    accessor(proto_text, u"length", [self](Runtime&, const Value& t, Args) { return Value::number(static_cast<double>(js::utf16(self->thisNode(t)->data).size())); });
    accessor(proto_text, u"wholeText", [self](Runtime&, const Value& t, Args) { return str(self->thisNode(t)->data); });
    // ChildNode and ParentNode, for elements, text and fragments alike.
    const auto toNodes = [self](Runtime& rt, Args a) {
        std::vector<Node*> out;
        for (const Value& v : a) {
            if (Node* n = self->node(v)) {
                out.push_back(n);
            } else {
                Node* tn = self->doc.make(NodeType::Text);
                tn->data = js::utf8(rt.toStringValue(v));
                out.push_back(tn);
            }
        }
        return out;
    };
    for (Object* o : {proto_node}) {
        method(o, u"remove", 0, [self](Runtime&, const Value& t, Args) {
            self->detach(self->thisNode(t));
            return Value::undefined();
        });
        method(o, u"before", 0, [self, toNodes](Runtime& rt, const Value& t, Args a) {
            Node* n = self->thisNode(t);
            if (!n->parent) return Value::undefined();
            for (Node* x : toNodes(rt, a)) self->insert(n->parent, x, n);
            return Value::undefined();
        });
        method(o, u"after", 0, [self, toNodes](Runtime& rt, const Value& t, Args a) {
            Node* n = self->thisNode(t);
            if (!n->parent) return Value::undefined();
            Node* parent = n->parent;
            auto it = std::find(parent->children.begin(), parent->children.end(), n);
            Node* next = (it + 1) == parent->children.end() ? nullptr : *(it + 1);
            for (Node* x : toNodes(rt, a)) self->insert(parent, x, next);
            return Value::undefined();
        });
        method(o, u"replaceWith", 0, [self, toNodes](Runtime& rt, const Value& t, Args a) {
            Node* n = self->thisNode(t);
            if (!n->parent) return Value::undefined();
            Node* parent = n->parent;
            for (Node* x : toNodes(rt, a)) self->insert(parent, x, n);
            self->detach(n);
            return Value::undefined();
        });
        method(o, u"append", 0, [self, toNodes](Runtime& rt, const Value& t, Args a) {
            Node* n = self->thisNode(t);
            for (Node* x : toNodes(rt, a)) self->insert(n, x, nullptr);
            return Value::undefined();
        });
        method(o, u"prepend", 0, [self, toNodes](Runtime& rt, const Value& t, Args a) {
            Node* n = self->thisNode(t);
            Node* first = n->children.empty() ? nullptr : n->children.front();
            for (Node* x : toNodes(rt, a)) self->insert(n, x, first);
            return Value::undefined();
        });
        method(o, u"replaceChildren", 0, [self, toNodes](Runtime& rt, const Value& t, Args a) {
            Node* n = self->thisNode(t);
            while (!n->children.empty()) self->detach(n->children.back());
            for (Node* x : toNodes(rt, a)) self->insert(n, x, nullptr);
            return Value::undefined();
        });
        accessor(o, u"children", [self](Runtime&, const Value& t, Args) {
            std::vector<Node*> out;
            for (Node* c : self->thisNode(t)->children)
                if (c->isElement()) out.push_back(c);
            return self->list(out);
        });
        accessor(o, u"childElementCount", [self](Runtime&, const Value& t, Args) {
            const auto& k = self->thisNode(t)->children;
            return Value::number(static_cast<double>(std::count_if(k.begin(), k.end(), [](const Node* c) { return c->isElement(); })));
        });
        accessor(o, u"firstElementChild", [self](Runtime&, const Value& t, Args) {
            for (Node* c : self->thisNode(t)->children)
                if (c->isElement()) return self->wrap(c);
            return Value::null();
        });
        accessor(o, u"lastElementChild", [self](Runtime&, const Value& t, Args) {
            const auto& k = self->thisNode(t)->children;
            for (auto it = k.rbegin(); it != k.rend(); ++it)
                if ((*it)->isElement()) return self->wrap(*it);
            return Value::null();
        });
        accessor(o, u"nextElementSibling", sibling(1, true));
        accessor(o, u"previousElementSibling", sibling(-1, true));
        method(o, u"querySelector", 1, [self](Runtime& rt, const Value& t, Args a) {
            const auto sel = parseSelectors(js::utf8(rt.toStringValue(arg(a, 0))));
            if (!sel) rt.throwError("SyntaxError", "'" + js::utf8(rt.toStringValue(arg(a, 0))) + "' isn't a valid selector");
            const auto found = querySelectorAll(self->thisNode(t), *sel, true);
            return found.empty() ? Value::null() : self->wrap(found[0]);
        });
        method(o, u"querySelectorAll", 1, [self](Runtime& rt, const Value& t, Args a) {
            const auto sel = parseSelectors(js::utf8(rt.toStringValue(arg(a, 0))));
            if (!sel) rt.throwError("SyntaxError", "'" + js::utf8(rt.toStringValue(arg(a, 0))) + "' isn't a valid selector");
            return self->list(querySelectorAll(self->thisNode(t), *sel));
        });
        const auto byWalk = [self](std::function<bool(const Node*, const std::string&)> keep) {
            return [self, keep](Runtime& rt, const Value& t, Args a) {
                const std::string want = js::utf8(rt.toStringValue(arg(a, 0)));
                std::vector<Node*> out;
                std::vector<Node*> todo(self->thisNode(t)->children.rbegin(), self->thisNode(t)->children.rend());
                while (!todo.empty()) {
                    Node* n = todo.back();
                    todo.pop_back();
                    if (n->isElement() && keep(n, want)) out.push_back(n);
                    todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
                }
                return self->list(out);
            };
        };
        method(o, u"getElementsByTagName", 1, byWalk([](const Node* n, const std::string& w) { return w == "*" || n->name == lower(w); }));
        method(o, u"getElementsByClassName", 1, byWalk([](const Node* n, const std::string& w) {
            const std::string* c = n->attr("class");
            if (!c) return false;
            // Every class asked for.
            size_t i = 0;
            while (i < w.size()) {
                while (i < w.size() && w[i] == ' ') ++i;
                const size_t b = i;
                while (i < w.size() && w[i] != ' ') ++i;
                if (i == b) break;
                const std::string one = w.substr(b, i - b);
                const std::string padded = " " + *c + " ";
                if (padded.find(" " + one + " ") == std::string::npos) return false;
            }
            return true;
        }));
    }
}

// ── Element ───────────────────────────────────────────────────────────────────

void PageScript::Impl::installElement() {
    Impl* self = this;
    Object* p = proto_element;
    accessor(p, u"tagName", [self](Runtime&, const Value& t, Args) {
        const Node* n = self->thisElement(t);
        return str(n->ns == Namespace::Html ? upper(n->name) : n->name);
    });
    accessor(p, u"localName", [self](Runtime&, const Value& t, Args) { return str(self->thisElement(t)->name); });
    // Attributes reflected as properties.
    for (const char16_t* name : {u"id", u"title", u"lang", u"dir", u"name", u"placeholder", u"alt", u"rel", u"target", u"role", u"slot"}) {
        const std::string a = js::utf8(name);
        accessor(p, name, [self, a](Runtime&, const Value& t, Args) { return str(self->attr(self->thisElement(t), a.c_str())); },
                 [self, a](Runtime& rt, const Value& t, Args v) {
                     self->setAttr(self->thisElement(t), a, js::utf8(rt.toStringValue(arg(v, 0))));
                     return Value::undefined();
                 });
    }
    accessor(p, u"className", [self](Runtime&, const Value& t, Args) { return str(self->attr(self->thisElement(t), "class")); },
             [self](Runtime& rt, const Value& t, Args v) {
                 self->setAttr(self->thisElement(t), "class", js::utf8(rt.toStringValue(arg(v, 0))));
                 return Value::undefined();
             });
    for (const char16_t* name : {u"hidden", u"disabled", u"required", u"readOnly", u"multiple", u"autofocus"}) {
        const std::string a = lower(js::utf8(name));
        accessor(p, name, [self, a](Runtime&, const Value& t, Args) { return Value::boolean(self->thisElement(t)->attr(a) != nullptr); },
                 [self, a](Runtime& rt, const Value& t, Args v) {
                     if (rt.toBoolean(arg(v, 0))) self->setAttr(self->thisElement(t), a, "");
                     else self->removeAttr(self->thisElement(t), a);
                     return Value::undefined();
                 });
    }
    for (const char16_t* name : {u"href", u"src", u"action"}) {
        const std::string a = js::utf8(name);
        accessor(p, name, [self, a](Runtime&, const Value& t, Args) {
            const std::string* v = self->thisElement(t)->attr(a);
            return v ? str(resolveUrl(self->host.base, *v)) : str("");
        }, [self, a](Runtime& rt, const Value& t, Args v) {
            self->setAttr(self->thisElement(t), a, js::utf8(rt.toStringValue(arg(v, 0))));
            return Value::undefined();
        });
    }
    method(p, u"getAttribute", 1, [self](Runtime& rt, const Value& t, Args a) {
        const std::string* v = self->thisElement(t)->attr(lower(js::utf8(rt.toStringValue(arg(a, 0)))));
        return v ? str(*v) : Value::null();
    });
    method(p, u"setAttribute", 2, [self](Runtime& rt, const Value& t, Args a) {
        self->setAttr(self->thisElement(t), lower(js::utf8(rt.toStringValue(arg(a, 0)))), js::utf8(rt.toStringValue(arg(a, 1))));
        return Value::undefined();
    });
    method(p, u"removeAttribute", 1, [self](Runtime& rt, const Value& t, Args a) {
        self->removeAttr(self->thisElement(t), lower(js::utf8(rt.toStringValue(arg(a, 0)))));
        return Value::undefined();
    });
    method(p, u"hasAttribute", 1, [self](Runtime& rt, const Value& t, Args a) {
        return Value::boolean(self->thisElement(t)->attr(lower(js::utf8(rt.toStringValue(arg(a, 0))))) != nullptr);
    });
    method(p, u"toggleAttribute", 1, [self](Runtime& rt, const Value& t, Args a) {
        Node* n = self->thisElement(t);
        const std::string name = lower(js::utf8(rt.toStringValue(arg(a, 0))));
        const bool on = a.size() > 1 ? rt.toBoolean(a[1]) : !n->attr(name);
        if (on) { if (!n->attr(name)) self->setAttr(n, name, ""); }
        else self->removeAttr(n, name);
        return Value::boolean(on);
    });
    method(p, u"getAttributeNames", 0, [self](Runtime& rt, const Value& t, Args) {
        std::vector<Value> out;
        for (const auto& at : self->thisElement(t)->attrs) out.push_back(str(at.name));
        return Value::object(rt.newArray(std::move(out)));
    });
    accessor(p, u"attributes", [self](Runtime& rt, const Value& t, Args) {
        std::vector<Value> out;
        for (const auto& at : self->thisElement(t)->attrs) {
            Object* o = rt.newObject();
            rt.put(o, u"name", str(at.name), js::kEnumerable);
            rt.put(o, u"value", str(at.value), js::kEnumerable);
            out.push_back(Value::object(o));
        }
        return Value::object(rt.newArray(std::move(out)));
    });
    accessor(p, u"dataset", [self](Runtime& rt, const Value& t, Args) {
        // A copy of the data-* attributes: reading works; writing goes nowhere yet.
        Object* o = rt.newObject();
        for (const auto& at : self->thisElement(t)->attrs)
            if (at.name.starts_with("data-")) rt.put(o, js::utf16(camelName(at.name.substr(5))), str(at.value), js::kDefaultFlags);
        return Value::object(o);
    });
    accessor(p, u"innerHTML", [self](Runtime&, const Value& t, Args) { return str(serializeChildren(*self->thisElement(t))); },
             [self](Runtime& rt, const Value& t, Args a) {
                 self->setInnerHtml(self->thisElement(t), js::utf8(rt.toStringValue(arg(a, 0))));
                 return Value::undefined();
             });
    accessor(p, u"outerHTML", [self](Runtime&, const Value& t, Args) { return str(self->outerHtml(self->thisElement(t))); });
    accessor(p, u"innerText", [self](Runtime&, const Value& t, Args) { return str(self->thisElement(t)->textContent()); },
             [self](Runtime& rt, const Value& t, Args a) {
                 self->setText(self->thisElement(t), js::utf8(rt.toStringValue(arg(a, 0))));
                 return Value::undefined();
             });
    method(p, u"insertAdjacentHTML", 2, [self](Runtime& rt, const Value& t, Args a) {
        Node* n = self->thisElement(t);
        const std::string where = lower(js::utf8(rt.toStringValue(arg(a, 0))));
        auto frag = parseFragment(js::utf8(rt.toStringValue(arg(a, 1))), "div");
        std::vector<Node*> made;
        if (const Node* holder = frag->documentElement())
            for (const Node* c : holder->children) made.push_back(self->clone(c, true));
        Node* parent = where == "beforebegin" || where == "afterend" ? n->parent : n;
        if (!parent) return Value::undefined();
        Node* before = nullptr;
        if (where == "beforebegin") before = n;
        else if (where == "afterbegin") before = n->children.empty() ? nullptr : n->children.front();
        else if (where == "afterend") {
            auto it = std::find(parent->children.begin(), parent->children.end(), n);
            before = (it + 1) == parent->children.end() ? nullptr : *(it + 1);
        }
        for (Node* m : made) self->insert(parent, m, before);
        return Value::undefined();
    });
    method(p, u"matches", 1, [self](Runtime& rt, const Value& t, Args a) {
        const auto sel = parseSelectors(js::utf8(rt.toStringValue(arg(a, 0))));
        if (!sel) rt.throwError("SyntaxError", "invalid selector");
        return Value::boolean(matchesSelectors(self->thisElement(t), *sel));
    });
    method(p, u"closest", 1, [self](Runtime& rt, const Value& t, Args a) {
        const auto sel = parseSelectors(js::utf8(rt.toStringValue(arg(a, 0))));
        if (!sel) rt.throwError("SyntaxError", "invalid selector");
        for (const Node* n = self->thisElement(t); n && n->isElement(); n = n->parent)
            if (matchesSelectors(n, *sel)) return self->wrap(n);
        return Value::null();
    });
    // Geometry, from the last layout.
    const auto rectOf = [self](const Value& t) {
        const Node* n = self->thisElement(t);
        ElementRect r;
        if (self->host.rect)
            if (auto got = self->host.rect(n)) r = *got;
        return r;
    };
    method(p, u"getBoundingClientRect", 0, [self, rectOf](Runtime& rt, const Value& t, Args) {
        const ElementRect r = rectOf(t);
        const float sy = self->host.scroll_y ? self->host.scroll_y() : 0;
        Object* o = rt.newObject();
        rt.put(o, u"x", Value::number(r.x), js::kDefaultFlags);
        rt.put(o, u"y", Value::number(r.y - sy), js::kDefaultFlags);
        rt.put(o, u"left", Value::number(r.x), js::kDefaultFlags);
        rt.put(o, u"top", Value::number(r.y - sy), js::kDefaultFlags);
        rt.put(o, u"width", Value::number(r.w), js::kDefaultFlags);
        rt.put(o, u"height", Value::number(r.h), js::kDefaultFlags);
        rt.put(o, u"right", Value::number(r.x + r.w), js::kDefaultFlags);
        rt.put(o, u"bottom", Value::number(r.y - sy + r.h), js::kDefaultFlags);
        return Value::object(o);
    });
    method(p, u"getClientRects", 0, [self](Runtime& rt, const Value& t, Args) {
        const Value r = rt.call(rt.get(t, u"getBoundingClientRect"), t, {});
        return Value::object(rt.newArray({r}));
    });
    accessor(p, u"offsetWidth", [rectOf](Runtime&, const Value& t, Args) { return Value::number(std::round(rectOf(t).w)); });
    accessor(p, u"offsetHeight", [rectOf](Runtime&, const Value& t, Args) { return Value::number(std::round(rectOf(t).h)); });
    accessor(p, u"offsetLeft", [rectOf](Runtime&, const Value& t, Args) { return Value::number(std::round(rectOf(t).x)); });
    accessor(p, u"offsetTop", [rectOf](Runtime&, const Value& t, Args) { return Value::number(std::round(rectOf(t).y)); });
    accessor(p, u"clientWidth", [rectOf](Runtime&, const Value& t, Args) { return Value::number(std::round(rectOf(t).w)); });
    accessor(p, u"clientHeight", [rectOf](Runtime&, const Value& t, Args) { return Value::number(std::round(rectOf(t).h)); });
    accessor(p, u"scrollWidth", [rectOf](Runtime&, const Value& t, Args) { return Value::number(std::round(rectOf(t).w)); });
    accessor(p, u"scrollHeight", [rectOf](Runtime&, const Value& t, Args) { return Value::number(std::round(rectOf(t).h)); });
    accessor(p, u"offsetParent", [self](Runtime&, const Value& t, Args) { return self->wrap(self->doc.body()); });
    accessor(p, u"scrollTop", [](Runtime&, const Value&, Args) { return Value::number(0); }, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    accessor(p, u"scrollLeft", [](Runtime&, const Value&, Args) { return Value::number(0); }, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    method(p, u"scrollIntoView", 0, [self, rectOf](Runtime&, const Value& t, Args) {
        if (self->host.scroll_to) self->host.scroll_to(0, rectOf(t).y);
        return Value::undefined();
    });
    method(p, u"focus", 0, [self](Runtime&, const Value& t, Args) {
        if (self->host.focus) self->host.focus(self->thisElement(t));
        return Value::undefined();
    });
    method(p, u"blur", 0, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    method(p, u"click", 0, [self](Runtime&, const Value& t, Args) {
        Node* n = self->thisElement(t);
        if (self->fire(n, u"click", true, true) && self->host.activate) self->host.activate(n);
        return Value::undefined();
    });
    // Form controls, through the page's form state.
    accessor(p, u"value", [self](Runtime&, const Value& t, Args) {
        const Node* n = self->thisElement(t);
        const ControlKind k = controlKind(n);
        if (n->isHtml("option")) return str(optionValue(n));
        if (k == ControlKind::Select) {
            const int i = self->host.forms ? self->host.forms->selected(n) : -1;
            const auto opts = optionsOf(n);
            return i >= 0 ? str(optionValue(opts[static_cast<size_t>(i)])) : str("");
        }
        if (k == ControlKind::None) return Value::undefined();
        if (k == ControlKind::Checkbox || k == ControlKind::Radio) return str(self->attr(n, "value").empty() ? "on" : self->attr(n, "value"));
        return str(self->host.forms ? self->host.forms->value(n) : self->attr(n, "value"));
    }, [self](Runtime& rt, const Value& t, Args a) {
        Node* n = self->thisElement(t);
        const std::string v = js::utf8(rt.toStringValue(arg(a, 0)));
        const ControlKind k = controlKind(n);
        if (k == ControlKind::Select && self->host.forms) {
            const auto opts = optionsOf(n);
            int pick = -1;
            for (size_t i = 0; i < opts.size(); ++i)
                if (optionValue(opts[i]) == v) { pick = static_cast<int>(i); break; }
            self->host.forms->select(n, pick);
        } else if (isTypedInto(k) && self->host.forms) {
            self->host.forms->setValue(n, v);
            if (self->host.forms->focused == n) self->host.forms->caret = self->host.forms->value(n).size();
        } else {
            self->setAttr(n, "value", v);
        }
        self->dirty = true;
        return Value::undefined();
    });
    accessor(p, u"checked", [self](Runtime&, const Value& t, Args) {
        const Node* n = self->thisElement(t);
        return Value::boolean(self->host.forms ? self->host.forms->checked(n) : n->attr("checked") != nullptr);
    }, [self](Runtime& rt, const Value& t, Args a) {
        if (self->host.forms) self->host.forms->setChecked(self->thisElement(t), rt.toBoolean(arg(a, 0)));
        self->dirty = true;
        return Value::undefined();
    });
    accessor(p, u"selectedIndex", [self](Runtime&, const Value& t, Args) {
        return Value::number(self->host.forms ? self->host.forms->selected(self->thisElement(t)) : -1);
    }, [self](Runtime& rt, const Value& t, Args a) {
        if (self->host.forms) self->host.forms->select(self->thisElement(t), static_cast<int>(rt.toIntegerOrInfinity(arg(a, 0))));
        self->dirty = true;
        return Value::undefined();
    });
    accessor(p, u"options", [self](Runtime&, const Value& t, Args) {
        std::vector<Node*> nodes;
        for (const Node* o : optionsOf(self->thisElement(t))) nodes.push_back(const_cast<Node*>(o));
        return self->list(nodes);
    });
    accessor(p, u"type", [self](Runtime&, const Value& t, Args) {
        const Node* n = self->thisElement(t);
        if (n->isHtml("select")) return str(n->attr("multiple") ? "select-multiple" : "select-one");
        if (n->isHtml("textarea")) return str("textarea");
        const std::string v = lower(self->attr(n, "type"));
        if (n->isHtml("button")) return str(v.empty() ? "submit" : v);
        return str(v.empty() && n->isHtml("input") ? "text" : v);
    }, [self](Runtime& rt, const Value& t, Args a) {
        self->setAttr(self->thisElement(t), "type", js::utf8(rt.toStringValue(arg(a, 0))));
        return Value::undefined();
    });
    accessor(p, u"form", [self](Runtime&, const Value& t, Args) { return self->wrap(formOwner(self->thisElement(t))); });
    accessor(p, u"elements", [self](Runtime&, const Value& t, Args) {
        const Node* f = self->thisElement(t);
        std::vector<Node*> out;
        std::vector<Node*> todo{self->doc.root()};
        while (!todo.empty()) {
            Node* n = todo.back();
            todo.pop_back();
            if (controlKind(n) != ControlKind::None && formOwner(n) == f) out.push_back(n);
            todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        }
        return self->list(out);
    });
    method(p, u"submit", 0, [self](Runtime&, const Value& t, Args) {
        if (self->host.submit) self->host.submit(self->thisElement(t));
        return Value::undefined();
    });
    method(p, u"requestSubmit", 0, [self](Runtime&, const Value& t, Args) {
        Node* f = self->thisElement(t);
        if (self->fire(f, u"submit", true, true) && self->host.submit) self->host.submit(f);
        return Value::undefined();
    });
    method(p, u"reset", 0, [self](Runtime&, const Value& t, Args) {
        if (self->host.forms) self->host.forms->reset(self->thisElement(t));
        self->dirty = true;
        return Value::undefined();
    });
    // style and classList: objects that read and write the element's attributes.
    accessor(p, u"style", [self](Runtime& rt, const Value& t, Args) {
        Object* s = rt.allocate<Object>(self->proto_style);
        s->host = self->thisElement(t);
        s->host_kind = kStyleWrapper;
        return Value::object(s);
    }, [self](Runtime& rt, const Value& t, Args a) {
        self->setAttr(self->thisElement(t), "style", js::utf8(rt.toStringValue(arg(a, 0))));
        return Value::undefined();
    });
    accessor(p, u"classList", [self](Runtime& rt, const Value& t, Args) {
        Object* c = rt.allocate<Object>(self->proto_classlist);
        c->host = self->thisElement(t);
        c->host_kind = kClassListWrapper;
        return Value::object(c);
    });
    accessor(p, u"isContentEditable", [](Runtime&, const Value&, Args) { return Value::boolean(false); });
    accessor(p, u"tabIndex", [self](Runtime&, const Value& t, Args) {
        const std::string v = self->attr(self->thisElement(t), "tabindex");
        return Value::number(v.empty() ? -1 : std::atoi(v.c_str()));
    });

    // CSSStyleDeclaration.
    Object* st = proto_style;
    const auto styleNode = [self](const Value& t) -> Node* {
        if (!t.isObject() || t.asObject()->host_kind != kStyleWrapper) self->rt.throwError("TypeError", "not a style");
        return static_cast<Node*>(t.asObject()->host);
    };
    const auto getProp = [self, styleNode](const Value& t, const std::string& name) {
        for (const auto& [n, v] : parseStyle(self->attr(styleNode(t), "style")))
            if (n == name) return v;
        return std::string{};
    };
    const auto setProp = [self, styleNode](const Value& t, const std::string& name, const std::string& value) {
        Node* n = styleNode(t);
        auto decls = parseStyle(self->attr(n, "style"));
        const auto it = std::find_if(decls.begin(), decls.end(), [&](const auto& d) { return d.first == name; });
        if (value.empty()) {
            if (it != decls.end()) decls.erase(it);
        } else if (it != decls.end()) {
            it->second = value;
        } else {
            decls.emplace_back(name, value);
        }
        self->setAttr(n, "style", serializeStyle(decls));
    };
    for (const char* prop : kStyleProperties) {
        const std::string css = cssName(prop);
        accessor(st, js::utf16(prop).c_str(), [getProp, css](Runtime&, const Value& t, Args) { return str(getProp(t, css)); },
                 [setProp, css](Runtime& rt, const Value& t, Args a) {
                     const Value v = arg(a, 0);
                     std::string text = v.isNullish() ? "" : js::utf8(rt.toStringValue(v));
                     // A bare number for a length: browsers ignore it; so do we, except 0.
                     setProp(t, css, text);
                     return Value::undefined();
                 });
    }
    method(st, u"getPropertyValue", 1, [getProp](Runtime& rt, const Value& t, Args a) { return str(getProp(t, lower(js::utf8(rt.toStringValue(arg(a, 0)))))); });
    method(st, u"setProperty", 2, [setProp](Runtime& rt, const Value& t, Args a) {
        const std::string name = js::utf8(rt.toStringValue(arg(a, 0)));
        setProp(t, name.starts_with("--") ? name : lower(name), arg(a, 1).isNullish() ? "" : js::utf8(rt.toStringValue(arg(a, 1))));
        return Value::undefined();
    });
    method(st, u"removeProperty", 1, [getProp, setProp](Runtime& rt, const Value& t, Args a) {
        const std::string name = lower(js::utf8(rt.toStringValue(arg(a, 0))));
        const std::string old = getProp(t, name);
        setProp(t, name, "");
        return str(old);
    });
    accessor(st, u"cssText", [self, styleNode](Runtime&, const Value& t, Args) { return str(self->attr(styleNode(t), "style")); },
             [self, styleNode](Runtime& rt, const Value& t, Args a) {
                 self->setAttr(styleNode(t), "style", js::utf8(rt.toStringValue(arg(a, 0))));
                 return Value::undefined();
             });

    // DOMTokenList, for classList.
    Object* cl = proto_classlist;
    const auto classNode = [self](const Value& t) -> Node* {
        if (!t.isObject() || t.asObject()->host_kind != kClassListWrapper) self->rt.throwError("TypeError", "not a class list");
        return static_cast<Node*>(t.asObject()->host);
    };
    const auto classes = [self](Node* n) {
        std::vector<std::string> out;
        const std::string c = self->attr(n, "class");
        size_t i = 0;
        while (i < c.size()) {
            while (i < c.size() && std::isspace(static_cast<unsigned char>(c[i]))) ++i;
            const size_t b = i;
            while (i < c.size() && !std::isspace(static_cast<unsigned char>(c[i]))) ++i;
            if (i > b && std::find(out.begin(), out.end(), c.substr(b, i - b)) == out.end()) out.push_back(c.substr(b, i - b));
        }
        return out;
    };
    const auto store = [self](Node* n, const std::vector<std::string>& list) {
        std::string s;
        for (const auto& c : list) s += (s.empty() ? "" : " ") + c;
        self->setAttr(n, "class", s);
    };
    method(cl, u"add", 1, [classNode, classes, store](Runtime& rt, const Value& t, Args a) {
        Node* n = classNode(t);
        auto list = classes(n);
        for (const Value& v : a) {
            const std::string c = js::utf8(rt.toStringValue(v));
            if (std::find(list.begin(), list.end(), c) == list.end()) list.push_back(c);
        }
        store(n, list);
        return Value::undefined();
    });
    method(cl, u"remove", 1, [classNode, classes, store](Runtime& rt, const Value& t, Args a) {
        Node* n = classNode(t);
        auto list = classes(n);
        for (const Value& v : a) std::erase(list, js::utf8(rt.toStringValue(v)));
        store(n, list);
        return Value::undefined();
    });
    method(cl, u"toggle", 1, [classNode, classes, store](Runtime& rt, const Value& t, Args a) {
        Node* n = classNode(t);
        auto list = classes(n);
        const std::string c = js::utf8(rt.toStringValue(arg(a, 0)));
        const bool has = std::find(list.begin(), list.end(), c) != list.end();
        const bool want = a.size() > 1 ? rt.toBoolean(a[1]) : !has;
        if (want && !has) list.push_back(c);
        if (!want && has) std::erase(list, c);
        store(n, list);
        return Value::boolean(want);
    });
    method(cl, u"contains", 1, [classNode, classes](Runtime& rt, const Value& t, Args a) {
        const auto list = classes(classNode(t));
        return Value::boolean(std::find(list.begin(), list.end(), js::utf8(rt.toStringValue(arg(a, 0)))) != list.end());
    });
    method(cl, u"replace", 2, [classNode, classes, store](Runtime& rt, const Value& t, Args a) {
        Node* n = classNode(t);
        auto list = classes(n);
        const auto it = std::find(list.begin(), list.end(), js::utf8(rt.toStringValue(arg(a, 0))));
        if (it == list.end()) return Value::boolean(false);
        *it = js::utf8(rt.toStringValue(arg(a, 1)));
        store(n, list);
        return Value::boolean(true);
    });
    method(cl, u"item", 1, [classNode, classes](Runtime& rt, const Value& t, Args a) {
        const auto list = classes(classNode(t));
        const double i = rt.toIntegerOrInfinity(arg(a, 0));
        return i >= 0 && i < static_cast<double>(list.size()) ? str(list[static_cast<size_t>(i)]) : Value::null();
    });
    accessor(cl, u"length", [classNode, classes](Runtime&, const Value& t, Args) { return Value::number(static_cast<double>(classes(classNode(t)).size())); });
    accessor(cl, u"value", [self, classNode](Runtime&, const Value& t, Args) { return str(self->attr(classNode(t), "class")); });
    method(cl, u"toString", 0, [self, classNode](Runtime&, const Value& t, Args) { return str(self->attr(classNode(t), "class")); });
}

// ── Document ──────────────────────────────────────────────────────────────────

void PageScript::Impl::installDocument() {
    Impl* self = this;
    Object* p = proto_document;
    accessor(p, u"documentElement", [self](Runtime&, const Value&, Args) { return self->wrap(self->doc.documentElement()); });
    accessor(p, u"head", [self](Runtime&, const Value&, Args) { return self->wrap(self->doc.head()); });
    accessor(p, u"body", [self](Runtime&, const Value&, Args) { return self->wrap(self->doc.body()); });
    accessor(p, u"title", [self](Runtime&, const Value&, Args) {
        std::vector<Node*> todo{self->doc.root()};
        while (!todo.empty()) {
            Node* n = todo.back();
            todo.pop_back();
            if (n->isHtml("title")) {
                std::string t = n->textContent();
                // Whitespace collapsed, as the title is shown.
                std::string out;
                bool sp = false;
                for (char c : t) {
                    if (std::isspace(static_cast<unsigned char>(c))) { sp = !out.empty(); continue; }
                    if (sp) out += ' ';
                    sp = false;
                    out += c;
                }
                return str(out);
            }
            todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        }
        return str("");
    }, [self](Runtime& rt, const Value&, Args a) {
        const std::string text = js::utf8(rt.toStringValue(arg(a, 0)));
        std::vector<Node*> todo{self->doc.root()};
        while (!todo.empty()) {
            Node* n = todo.back();
            todo.pop_back();
            if (n->isHtml("title")) {
                self->setText(n, text);
                return Value::undefined();
            }
            todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        }
        if (Node* head = self->doc.head()) {
            Node* t = self->doc.make(NodeType::Element);
            t->name = "title";
            head->append(t);
            self->setText(t, text);
        }
        return Value::undefined();
    });
    method(p, u"getElementById", 1, [self](Runtime& rt, const Value&, Args a) {
        const std::string id = js::utf8(rt.toStringValue(arg(a, 0)));
        std::vector<Node*> todo{self->doc.root()};
        while (!todo.empty()) {
            Node* n = todo.back();
            todo.pop_back();
            if (n->isElement())
                if (const std::string* v = n->attr("id"); v && *v == id) return self->wrap(n);
            todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        }
        return Value::null();
    });
    method(p, u"getElementsByName", 1, [self](Runtime& rt, const Value&, Args a) {
        const std::string want = js::utf8(rt.toStringValue(arg(a, 0)));
        std::vector<Node*> out;
        std::vector<Node*> todo{self->doc.root()};
        while (!todo.empty()) {
            Node* n = todo.back();
            todo.pop_back();
            if (n->isElement())
                if (const std::string* v = n->attr("name"); v && *v == want) out.push_back(n);
            todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        }
        return self->list(out);
    });
    method(p, u"createElement", 1, [self](Runtime& rt, const Value&, Args a) {
        const std::string tag = lower(js::utf8(rt.toStringValue(arg(a, 0))));
        if (tag.empty() || tag.find_first_of(" <>/\"'=") != std::string::npos) rt.throwError("Error", "InvalidCharacterError: invalid tag name");
        Node* n = self->doc.make(NodeType::Element);
        n->name = tag;
        return self->wrap(n);
    });
    method(p, u"createElementNS", 2, [self](Runtime& rt, const Value&, Args a) {
        const std::string ns = js::utf8(rt.toStringValue(arg(a, 0)));
        Node* n = self->doc.make(NodeType::Element);
        n->name = js::utf8(rt.toStringValue(arg(a, 1)));
        n->ns = ns == "http://www.w3.org/2000/svg" ? Namespace::Svg : ns == "http://www.w3.org/1998/Math/MathML" ? Namespace::MathMl : Namespace::Html;
        if (n->ns == Namespace::Html) n->name = lower(n->name);
        return self->wrap(n);
    });
    method(p, u"createTextNode", 1, [self](Runtime& rt, const Value&, Args a) {
        Node* n = self->doc.make(NodeType::Text);
        n->data = js::utf8(rt.toStringValue(arg(a, 0)));
        return self->wrap(n);
    });
    method(p, u"createComment", 1, [self](Runtime& rt, const Value&, Args a) {
        Node* n = self->doc.make(NodeType::Comment);
        n->data = js::utf8(rt.toStringValue(arg(a, 0)));
        return self->wrap(n);
    });
    method(p, u"createDocumentFragment", 0, [self](Runtime&, const Value&, Args) { return self->wrap(self->doc.make(NodeType::Fragment)); });
    method(p, u"createEvent", 1, [self](Runtime&, const Value&, Args) {
        Object* e = self->makeEvent(u"", false, false);
        self->rt.method(e, u"initEvent", 3, [](Runtime& rt, const Value& t, Args a) {
            Object* o = t.asObject();
            rt.put(o, u"type", Value::string(rt.toString(arg(a, 0))), js::kEnumerable);
            rt.put(o, u"bubbles", Value::boolean(rt.toBoolean(arg(a, 1))), js::kEnumerable);
            rt.put(o, u"cancelable", Value::boolean(rt.toBoolean(arg(a, 2))), js::kEnumerable);
            return Value::undefined();
        });
        return Value::object(e);
    });
    accessor(p, u"readyState", [self](Runtime&, const Value&, Args) { return str(self->ready_state); });
    accessor(p, u"URL", [self](Runtime&, const Value&, Args) { return str(self->host.url); });
    accessor(p, u"documentURI", [self](Runtime&, const Value&, Args) { return str(self->host.url); });
    accessor(p, u"baseURI", [self](Runtime&, const Value&, Args) { return str(self->host.base); });
    accessor(p, u"referrer", [](Runtime&, const Value&, Args) { return str(""); });
    accessor(p, u"domain", [self](Runtime&, const Value&, Args) {
        const Url u = parseUrl(self->host.url);
        return str(u.authority.substr(0, u.authority.find(':')));
    });
    accessor(p, u"cookie", [](Runtime&, const Value&, Args) { return str(""); }, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    accessor(p, u"characterSet", [](Runtime&, const Value&, Args) { return str("UTF-8"); });
    accessor(p, u"charset", [](Runtime&, const Value&, Args) { return str("UTF-8"); });
    accessor(p, u"compatMode", [self](Runtime&, const Value&, Args) { return str(self->doc.quirks ? "BackCompat" : "CSS1Compat"); });
    accessor(p, u"contentType", [](Runtime&, const Value&, Args) { return str("text/html"); });
    accessor(p, u"visibilityState", [](Runtime&, const Value&, Args) { return str("visible"); });
    accessor(p, u"hidden", [](Runtime&, const Value&, Args) { return Value::boolean(false); });
    accessor(p, u"defaultView", [self](Runtime&, const Value&, Args) { return Value::object(self->rt.global()); });
    accessor(p, u"currentScript", [self](Runtime&, const Value&, Args) { return self->wrap(self->current_script); });
    accessor(p, u"activeElement", [self](Runtime&, const Value&, Args) {
        if (self->host.forms && self->host.forms->focused) return self->wrap(self->host.forms->focused);
        return self->wrap(self->doc.body());
    });
    method(p, u"hasFocus", 0, [](Runtime&, const Value&, Args) { return Value::boolean(true); });
    const auto collection = [self](std::function<bool(const Node*)> keep) {
        return [self, keep](Runtime&, const Value&, Args) {
            std::vector<Node*> out;
            std::vector<Node*> todo{self->doc.root()};
            while (!todo.empty()) {
                Node* n = todo.back();
                todo.pop_back();
                if (n->isElement() && keep(n)) out.push_back(n);
                todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
            }
            return self->list(out);
        };
    };
    accessor(p, u"forms", collection([](const Node* n) { return n->isHtml("form"); }));
    accessor(p, u"images", collection([](const Node* n) { return n->isHtml("img"); }));
    accessor(p, u"links", collection([](const Node* n) { return (n->isHtml("a") || n->isHtml("area")) && n->attr("href"); }));
    accessor(p, u"scripts", collection([](const Node* n) { return n->isHtml("script"); }));
    // document.write: what it writes goes at the end of the body.
    const auto write = [self](bool newline) {
        return [self, newline](Runtime& rt, const Value&, Args a) {
            std::string html;
            for (const Value& v : a) html += js::utf8(rt.toStringValue(v));
            if (newline) html += "\n";
            Node* target = self->current_script && self->current_script->parent ? self->current_script->parent : self->doc.body();
            if (!target) return Value::undefined();
            auto frag = parseFragment(html, target->name);
            if (const Node* holder = frag->documentElement()) {
                Node* after = nullptr;
                if (self->current_script && self->current_script->parent == target) {
                    auto it = std::find(target->children.begin(), target->children.end(), self->current_script);
                    after = (it + 1) == target->children.end() ? nullptr : *(it + 1);
                }
                for (const Node* c : holder->children) self->insert(target, self->clone(c, true), after);
            }
            return Value::undefined();
        };
    };
    method(p, u"write", 1, write(false));
    method(p, u"writeln", 1, write(true));
    method(p, u"open", 0, [](Runtime&, const Value& t, Args) { return t; });
    method(p, u"close", 0, [](Runtime&, const Value&, Args) { return Value::undefined(); });
}

// ── window ────────────────────────────────────────────────────────────────────

void PageScript::Impl::installWindow() {
    Impl* self = this;
    Object* g = rt.global();
    rt.put(g, u"window", Value::object(g));
    rt.put(g, u"self", Value::object(g));
    rt.put(g, u"top", Value::object(g));
    rt.put(g, u"parent", Value::object(g));
    rt.put(g, u"frames", Value::object(g));
    rt.put(g, u"document", wrap(doc.root()), 0);
    rt.put(g, u"name", str(""));
    rt.put(g, u"closed", Value::boolean(false));
    rt.put(g, u"isSecureContext", Value::boolean(parseUrl(host.url).scheme == "https"));
    rt.put(g, u"origin", str([&] {
        const Url u = parseUrl(host.url);
        return u.scheme + "://" + u.authority;
    }()));
    accessor(g, u"innerWidth", [self](Runtime&, const Value&, Args) { return Value::number(self->host.viewport_w); });
    accessor(g, u"innerHeight", [self](Runtime&, const Value&, Args) { return Value::number(self->host.viewport_h); });
    accessor(g, u"outerWidth", [self](Runtime&, const Value&, Args) { return Value::number(self->host.viewport_w); });
    accessor(g, u"outerHeight", [self](Runtime&, const Value&, Args) { return Value::number(self->host.viewport_h); });
    rt.put(g, u"devicePixelRatio", Value::number(1));
    const auto scrollY = [self](Runtime&, const Value&, Args) { return Value::number(self->host.scroll_y ? self->host.scroll_y() : 0); };
    accessor(g, u"scrollY", scrollY);
    accessor(g, u"pageYOffset", scrollY);
    accessor(g, u"scrollX", [](Runtime&, const Value&, Args) { return Value::number(0); });
    accessor(g, u"pageXOffset", [](Runtime&, const Value&, Args) { return Value::number(0); });
    const auto scrollTo = [self](bool by) {
        return [self, by](Runtime& rt, const Value&, Args a) {
            double x = 0, y = 0;
            if (arg(a, 0).isObject()) {
                x = rt.toNumber(rt.get(arg(a, 0), u"left"));
                y = rt.toNumber(rt.get(arg(a, 0), u"top"));
            } else {
                x = rt.toNumber(arg(a, 0));
                y = rt.toNumber(arg(a, 1));
            }
            if (std::isnan(y)) y = 0;
            if (by && self->host.scroll_y) y += self->host.scroll_y();
            if (self->host.scroll_to) self->host.scroll_to(static_cast<float>(std::isnan(x) ? 0 : x), static_cast<float>(y));
            return Value::undefined();
        };
    };
    method(g, u"scrollTo", 2, scrollTo(false));
    method(g, u"scroll", 2, scrollTo(false));
    method(g, u"scrollBy", 2, scrollTo(true));
    Object* screen = rt.newObject();
    accessor(screen, u"width", [self](Runtime&, const Value&, Args) { return Value::number(self->host.viewport_w); });
    accessor(screen, u"height", [self](Runtime&, const Value&, Args) { return Value::number(self->host.viewport_h); });
    rt.put(screen, u"colorDepth", Value::number(24));
    rt.put(g, u"screen", Value::object(screen));

    // location
    Object* loc = rt.newObject();
    const auto part = [self](std::function<std::string(const Url&)> f) {
        return [self, f](Runtime&, const Value&, Args) { return str(f(parseUrl(self->host.url))); };
    };
    accessor(loc, u"href", part([](const Url& u) { return u.toString(); }), [self](Runtime& rt, const Value&, Args a) {
        if (self->host.navigate) self->host.navigate(resolveUrl(self->host.base, js::utf8(rt.toStringValue(arg(a, 0)))));
        return Value::undefined();
    });
    accessor(loc, u"protocol", part([](const Url& u) { return u.scheme + ":"; }));
    accessor(loc, u"host", part([](const Url& u) { return u.authority; }));
    accessor(loc, u"hostname", part([](const Url& u) { return u.authority.substr(0, u.authority.find(':')); }));
    accessor(loc, u"port", part([](const Url& u) { const size_t c = u.authority.find(':'); return c == std::string::npos ? std::string{} : u.authority.substr(c + 1); }));
    accessor(loc, u"pathname", part([](const Url& u) { return u.path.empty() ? std::string("/") : u.path; }));
    accessor(loc, u"search", part([](const Url& u) { return u.has_query && !u.query.empty() ? "?" + u.query : std::string{}; }));
    accessor(loc, u"hash", part([](const Url& u) { return u.has_fragment && !u.fragment.empty() ? "#" + u.fragment : std::string{}; }));
    accessor(loc, u"origin", part([](const Url& u) { return u.scheme + "://" + u.authority; }));
    const auto go = [self](Runtime& rt, const Value&, Args a) {
        if (self->host.navigate) self->host.navigate(resolveUrl(self->host.base, js::utf8(rt.toStringValue(arg(a, 0)))));
        return Value::undefined();
    };
    method(loc, u"assign", 1, go);
    method(loc, u"replace", 1, go);
    method(loc, u"reload", 0, [self](Runtime&, const Value&, Args) {
        if (self->host.navigate) self->host.navigate(self->host.url);
        return Value::undefined();
    });
    method(loc, u"toString", 0, part([](const Url& u) { return u.toString(); }));
    accessor(g, u"location", [loc](Runtime&, const Value&, Args) { return Value::object(loc); }, [self](Runtime& rt, const Value&, Args a) {
        if (self->host.navigate) self->host.navigate(resolveUrl(self->host.base, js::utf8(rt.toStringValue(arg(a, 0)))));
        return Value::undefined();
    });
    rt.put(proto_document, u"location", Value::object(loc));

    // history: the address changes without loading (single-page sites); back is a no-op.
    Object* history = rt.newObject();
    const auto push = [self](bool add) {
        return [self, add](Runtime& rt, const Value&, Args a) {
            if (!arg(a, 2).isNullish()) self->host.url = resolveUrl(self->host.url, js::utf8(rt.toStringValue(arg(a, 2))));
            if (add) ++self->history_length;
            return Value::undefined();
        };
    };
    method(history, u"pushState", 3, push(true));
    method(history, u"replaceState", 3, push(false));
    method(history, u"back", 0, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    method(history, u"forward", 0, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    method(history, u"go", 0, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    accessor(history, u"length", [self](Runtime&, const Value&, Args) { return Value::number(static_cast<double>(self->history_length)); });
    rt.put(history, u"state", Value::null());
    rt.put(g, u"history", Value::object(history));

    // navigator
    Object* nav = rt.newObject();
    rt.put(nav, u"userAgent", str("Mozilla/5.0 (X11; FreeBSD amd64) AppleWebKit/537.36 (KHTML, like Gecko) xweb/1.0"), js::kEnumerable);
    rt.put(nav, u"appName", str("Netscape"), js::kEnumerable);
    rt.put(nav, u"appVersion", str("5.0 (X11)"), js::kEnumerable);
    rt.put(nav, u"platform", str("FreeBSD amd64"), js::kEnumerable);
    rt.put(nav, u"vendor", str(""), js::kEnumerable);
    rt.put(nav, u"language", str("en-US"), js::kEnumerable);
    rt.put(nav, u"languages", Value::object(rt.newArray({str("en-US"), str("en")})), js::kEnumerable);
    rt.put(nav, u"onLine", Value::boolean(true), js::kEnumerable);
    rt.put(nav, u"cookieEnabled", Value::boolean(false), js::kEnumerable);
    rt.put(nav, u"hardwareConcurrency", Value::number(4), js::kEnumerable);
    rt.put(nav, u"maxTouchPoints", Value::number(0), js::kEnumerable);
    rt.put(nav, u"webdriver", Value::boolean(false), js::kEnumerable);
    rt.put(g, u"navigator", Value::object(nav));

    // Timers.
    const auto addTimer = [self](bool repeat) {
        return [self, repeat](Runtime& rt, const Value&, Args a) {
            Value fn = arg(a, 0);
            if (!rt.isCallable(fn)) {
                // A string of code.
                const Runtime::Result r = rt.evaluate("(function () {\n" + js::utf8(rt.toStringValue(fn)) + "\n})", "timer");
                if (!r.ok) return Value::number(0);
                fn = r.value;
            }
            if (self->timers.size() >= kMaxTimers) rt.throwError("RangeError", "too many timers");
            double delay = rt.toNumber(arg(a, 1));
            if (std::isnan(delay) || delay < 0) delay = 0;
            Timer t{self->next_timer++, self->now() + delay, std::max(delay, 4.0), repeat, fn, {}};
            if (a.size() > 2) t.args.assign(a.begin() + 2, a.end());
            self->timers.push_back(std::move(t));
            return Value::number(self->timers.back().id);
        };
    };
    method(g, u"setTimeout", 2, addTimer(false));
    method(g, u"setInterval", 2, addTimer(true));
    const auto clearTimer = [self](Runtime& rt, const Value&, Args a) {
        const int id = static_cast<int>(rt.toIntegerOrInfinity(arg(a, 0)));
        std::erase_if(self->timers, [id](const Timer& t) { return t.id == id; });
        return Value::undefined();
    };
    method(g, u"clearTimeout", 1, clearTimer);
    method(g, u"clearInterval", 1, clearTimer);
    method(g, u"requestAnimationFrame", 1, [self](Runtime& rt, const Value&, Args a) {
        if (!rt.isCallable(arg(a, 0))) rt.throwError("TypeError", "requestAnimationFrame needs a function");
        if (self->frames.size() >= kMaxTimers) rt.throwError("RangeError", "too many frames requested");
        self->frames.emplace_back(self->next_timer++, arg(a, 0));
        return Value::number(self->frames.back().first);
    });
    method(g, u"cancelAnimationFrame", 1, [self](Runtime& rt, const Value&, Args a) {
        const int id = static_cast<int>(rt.toIntegerOrInfinity(arg(a, 0)));
        std::erase_if(self->frames, [id](const auto& f) { return f.first == id; });
        return Value::undefined();
    });
    method(g, u"requestIdleCallback", 1, addTimer(false));
    method(g, u"cancelIdleCallback", 1, clearTimer);
    method(g, u"queueMicrotask", 1, [self](Runtime& rt, const Value&, Args a) {
        if (!rt.isCallable(arg(a, 0))) rt.throwError("TypeError", "queueMicrotask needs a function");
        rt.enqueueMicrotask(arg(a, 0));
        return Value::undefined();
    });
    method(g, u"alert", 1, [self](Runtime& rt, const Value&, Args a) {
        if (self->host.console) self->host.console("info", "alert: " + js::utf8(rt.toStringValue(arg(a, 0))));
        return Value::undefined();
    });
    method(g, u"confirm", 1, [](Runtime&, const Value&, Args) { return Value::boolean(false); });
    method(g, u"prompt", 1, [](Runtime&, const Value&, Args) { return Value::null(); });
    method(g, u"print", 0, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    method(g, u"focus", 0, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    method(g, u"blur", 0, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    method(g, u"open", 1, [](Runtime&, const Value&, Args) { return Value::null(); });
    method(g, u"postMessage", 1, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    Object* perf = rt.newObject();
    method(perf, u"now", 0, [self](Runtime&, const Value&, Args) { return Value::number(self->now()); });
    method(perf, u"mark", 1, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    method(perf, u"measure", 1, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    method(perf, u"getEntriesByType", 1, [](Runtime& rt, const Value&, Args) { return Value::object(rt.newArray()); });
    method(perf, u"getEntriesByName", 1, [](Runtime& rt, const Value&, Args) { return Value::object(rt.newArray()); });
    rt.put(perf, u"timeOrigin", Value::number(static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())));
    rt.put(g, u"performance", Value::object(perf));

    // getComputedStyle, from the last layout's styles.
    method(g, u"getComputedStyle", 1, [self](Runtime& rt, const Value&, Args a) {
        Node* n = self->node(arg(a, 0));
        if (!n || !n->isElement()) rt.throwError("TypeError", "getComputedStyle needs an element");
        Object* o = rt.newObject();
        const auto values = self->host.computed_style ? self->host.computed_style(n) : std::unordered_map<std::string, std::string>{};
        for (const auto& [k, v] : values) {
            rt.put(o, js::utf16(k), str(v), js::kEnumerable);
            rt.put(o, js::utf16(camelName(k)), str(v), js::kEnumerable);
        }
        Object* holder = o;
        rt.method(o, u"getPropertyValue", 1, [holder](Runtime& r, const Value&, Args args) {
            const Value v = r.get(holder, PropertyKey(r.toStringValue(arg(args, 0))), Value::object(holder));
            return v.isUndefined() ? str("") : v;
        });
        return Value::object(o);
    });
    method(g, u"matchMedia", 1, [self](Runtime& rt, const Value&, Args a) {
        const std::string q = js::utf8(rt.toStringValue(arg(a, 0)));
        Object* m = rt.newObject();
        rt.put(m, u"media", str(q), js::kEnumerable);
        rt.put(m, u"matches", Value::boolean(css::mediaMatches(css::tokenize(q), {self->host.viewport_w, self->host.viewport_h})), js::kEnumerable);
        for (const char16_t* name : {u"addListener", u"removeListener", u"addEventListener", u"removeEventListener"})
            rt.method(m, name, 1, [](Runtime&, const Value&, Args) { return Value::undefined(); });
        return Value::object(m);
    });
    // atob and btoa: Latin-1 strings and base64.
    method(g, u"btoa", 1, [](Runtime& rt, const Value&, Args a) {
        static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const std::u16string s = rt.toStringValue(arg(a, 0));
        std::string bytes;
        for (char16_t c : s) {
            if (c > 255) rt.throwError("Error", "InvalidCharacterError: btoa takes Latin-1 only");
            bytes += static_cast<char>(c);
        }
        std::string out;
        for (size_t i = 0; i < bytes.size(); i += 3) {
            const uint32_t v = (static_cast<unsigned char>(bytes[i]) << 16) | (i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) << 8 : 0) |
                               (i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0);
            out += k[v >> 18];
            out += k[(v >> 12) & 63];
            out += i + 1 < bytes.size() ? k[(v >> 6) & 63] : '=';
            out += i + 2 < bytes.size() ? k[v & 63] : '=';
        }
        return str(out);
    });
    method(g, u"atob", 1, [](Runtime& rt, const Value&, Args a) {
        const std::u16string s = rt.toStringValue(arg(a, 0));
        std::u16string out;
        uint32_t acc = 0;
        int bits = 0;
        for (char16_t c : s) {
            int v;
            if (c >= u'A' && c <= u'Z') v = c - u'A';
            else if (c >= u'a' && c <= u'z') v = c - u'a' + 26;
            else if (c >= u'0' && c <= u'9') v = c - u'0' + 52;
            else if (c == u'+') v = 62;
            else if (c == u'/') v = 63;
            else if (c == u'=' || c == u' ' || c == u'\n' || c == u'\t' || c == u'\r') continue;
            else rt.throwError("Error", "InvalidCharacterError: not base64");
            acc = (acc << 6) | static_cast<uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out += static_cast<char16_t>((acc >> bits) & 0xFF);
            }
        }
        return Value::string(std::move(out));
    });
    // Storage: per origin, for as long as the browser runs.
    const std::string origin = [&] {
        const Url u = parseUrl(host.url);
        return u.scheme + "://" + u.authority;
    }();
    for (const char16_t* name : {u"localStorage", u"sessionStorage"}) {
        Object* s = rt.newObject();
        const std::string key = origin + (name[0] == u's' ? "#session" : "");
        method(s, u"getItem", 1, [key](Runtime& rt, const Value&, Args a) {
            auto& m = Impl::storage()[key];
            const auto it = m.find(rt.toStringValue(arg(a, 0)));
            return it == m.end() ? Value::null() : Value::string(it->second);
        });
        method(s, u"setItem", 2, [key](Runtime& rt, const Value&, Args a) {
            auto& m = Impl::storage()[key];
            if (m.size() > 10'000) rt.throwError("Error", "QuotaExceededError");
            m[rt.toStringValue(arg(a, 0))] = rt.toStringValue(arg(a, 1));
            return Value::undefined();
        });
        method(s, u"removeItem", 1, [key](Runtime& rt, const Value&, Args a) {
            Impl::storage()[key].erase(rt.toStringValue(arg(a, 0)));
            return Value::undefined();
        });
        method(s, u"clear", 0, [key](Runtime&, const Value&, Args) {
            Impl::storage()[key].clear();
            return Value::undefined();
        });
        method(s, u"key", 1, [key](Runtime& rt, const Value&, Args a) {
            auto& m = Impl::storage()[key];
            const double i = rt.toIntegerOrInfinity(arg(a, 0));
            if (i < 0 || i >= static_cast<double>(m.size())) return Value::null();
            auto it = m.begin();
            std::advance(it, static_cast<long>(i));
            return Value::string(it->first);
        });
        accessor(s, u"length", [key](Runtime&, const Value&, Args) { return Value::number(static_cast<double>(Impl::storage()[key].size())); });
        rt.put(g, name, Value::object(s));
    }

    // Event targets: window and nodes.
    for (Object* o : {proto_target, g}) {
        method(o, u"addEventListener", 2, [self](Runtime&, const Value& t, Args a) {
            Object* target = t.isObject() ? t.asObject() : self->rt.global();
            self->addListener(target, arg(a, 0), arg(a, 1), arg(a, 2));
            return Value::undefined();
        });
        method(o, u"removeEventListener", 2, [self](Runtime&, const Value& t, Args a) {
            Object* target = t.isObject() ? t.asObject() : self->rt.global();
            self->removeListener(target, arg(a, 0), arg(a, 1), arg(a, 2));
            return Value::undefined();
        });
        method(o, u"dispatchEvent", 1, [self](Runtime& rt, const Value& t, Args a) {
            if (!arg(a, 0).isObject()) rt.throwError("TypeError", "dispatchEvent needs an event");
            Object* target = t.isObject() ? t.asObject() : self->rt.global();
            Object* ev = arg(a, 0).asObject();
            rt.put(ev, u"isTrusted", Value::boolean(false), js::kEnumerable);
            return Value::boolean(self->dispatch(target, ev));
        });
    }
    // Event and friends.
    method(proto_event, u"preventDefault", 0, [self](Runtime&, const Value& t, Args) {
        if (t.isObject()) self->preventDefault(t.asObject());
        return Value::undefined();
    });
    method(proto_event, u"stopPropagation", 0, [self](Runtime&, const Value& t, Args) {
        if (t.isObject()) t.asObject()->define(PropertyKey(self->stop_flag), {Value::boolean(true)});
        return Value::undefined();
    });
    method(proto_event, u"stopImmediatePropagation", 0, [self](Runtime&, const Value& t, Args) {
        if (t.isObject()) t.asObject()->define(PropertyKey(self->stop_now_flag), {Value::boolean(true)});
        return Value::undefined();
    });
    method(proto_event, u"composedPath", 0, [](Runtime& rt, const Value&, Args) { return Value::object(rt.newArray()); });
    for (const char16_t* name : {u"Event", u"CustomEvent", u"MouseEvent", u"KeyboardEvent", u"FocusEvent", u"InputEvent", u"UIEvent", u"PointerEvent"}) {
        Object* c = rt.newFunction(name, 1, [self](Runtime& rt, const Value&, Args a) {
            Object* e = self->makeEvent(rt.toStringValue(arg(a, 0)), false, false);
            rt.put(e, u"isTrusted", Value::boolean(false), js::kEnumerable);
            if (arg(a, 1).isObject()) {
                Object* init = rt.toObject(arg(a, 1));
                for (const PropertyKey& k : init->ownKeys()) rt.put(e, k, rt.get(init, k, arg(a, 1)), js::kEnumerable | js::kWritable);
            }
            return Value::object(e);
        }, true);
        rt.put(c, u"prototype", Value::object(proto_event), 0);
        rt.put(g, name, Value::object(c));
    }
    // Constructors for instanceof checks; they can't be called.
    const auto iface = [&](const char16_t* name, Object* proto) {
        Object* c = rt.newFunction(name, 0, [](Runtime& rt, const Value&, Args) -> Value { rt.throwError("TypeError", "Illegal constructor"); }, true);
        rt.put(c, u"prototype", Value::object(proto), 0);
        rt.put(proto, u"constructor", Value::object(c));
        rt.put(g, name, Value::object(c));
    };
    {
        // new EventTarget(), and classes that extend it, get the same listener machinery.
        Object* c = rt.newFunction(u"EventTarget", 0, [this](Runtime& rt, const Value&, Args) -> Value {
            Object* nt = rt.construct_target;
            if (!nt) rt.throwError("TypeError", "EventTarget must be made with new");
            const Value p = rt.get(Value::object(nt), u"prototype");
            return Value::object(rt.newObject(p.isObject() ? p.asObject() : proto_target));
        }, true);
        rt.put(c, u"prototype", Value::object(proto_target), 0);
        rt.put(proto_target, u"constructor", Value::object(c));
        rt.put(g, u"EventTarget", Value::object(c));
    }
    iface(u"Node", proto_node);
    iface(u"Element", proto_element);
    iface(u"HTMLElement", proto_element);
    iface(u"Document", proto_document);
    iface(u"HTMLDocument", proto_document);
    iface(u"Text", proto_text);
    iface(u"CharacterData", proto_text);
    iface(u"CSSStyleDeclaration", proto_style);
    iface(u"DOMTokenList", proto_classlist);
    for (auto [name, value] : std::initializer_list<std::pair<const char16_t*, int>>{{u"ELEMENT_NODE", 1}, {u"TEXT_NODE", 3}, {u"COMMENT_NODE", 8}, {u"DOCUMENT_NODE", 9}})
        rt.put(rt.get(Value::object(g), u"Node").asObject(), name, Value::number(value), 0);
}

void PageScript::Impl::install() {
    listeners = rt.newSymbol(u"listeners");
    stop_flag = rt.newSymbol(u"stop");
    stop_now_flag = rt.newSymbol(u"stopNow");
    proto_target = rt.newObject();
    proto_node = rt.newObject(proto_target);
    proto_element = rt.newObject(proto_node);
    proto_document = rt.newObject(proto_node);
    proto_text = rt.newObject(proto_node);
    proto_event = rt.newObject();
    proto_style = rt.newObject();
    proto_classlist = rt.newObject();
    rt.setConsole([this](std::string_view level, const std::string& text) {
        if (host.console) host.console(level, text);
    });
    rt.setErrorReporter([this](const std::string& message) { report(message); });
    rt.base_url = host.base;
    rt.setModuleLoader([this](const std::string& url, std::function<void(js::ModuleSource)> done) {
        if (std::optional<std::string> got = source ? source(url) : std::nullopt) {
            done(js::ModuleSource{true, std::move(*got), {}});
            return;
        }
        if (!host.fetch_script) {
            done(js::ModuleSource{false, {}, "couldn't fetch it"});
            return;
        }
        // Not fetched ahead: ask for it, and carry on as a task of its own when it comes.
        host.fetch_script(url, [this, done](std::optional<std::string> got) {
            js::ModuleSource src;
            if (got) {
                src.ok = true;
                src.source = std::move(*got);
            } else {
                src.error = "couldn't fetch it";
            }
            rt.setStepBudget(kTaskBudget);
            task([&] { done(std::move(src)); });
            drainMicrotasks();
            dirty = true;
        });
    });
    installNode();
    installElement();
    installDocument();
    installWindow();
    // What's written in JavaScript itself (URL, URLSearchParams).
    const Runtime::Result prelude = rt.evaluate(kScriptPrelude, "prelude");
    if (!prelude.ok) report("the script prelude failed: " + prelude.error);
}

// ── The public face ───────────────────────────────────────────────────────────

PageScript::PageScript(Document& doc, Host host) : impl_(std::make_unique<Impl>(doc, std::move(host))) { impl_->install(); }

PageScript::~PageScript() = default;

void PageScript::run(const std::function<std::optional<std::string>(const std::string&)>& source) {
    Impl& s = *impl_;
    s.source = source;
    s.running_scripts = true;
    std::vector<Node*> scripts;
    std::vector<Node*> todo{s.doc.root()};
    while (!todo.empty()) {
        Node* n = todo.back();
        todo.pop_back();
        if (n->isHtml("script")) scripts.push_back(n);
        todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
    }
    // Import maps first: every module specifier goes through them.
    for (Node* n : scripts) {
        const std::string* type = n->attr("type");
        if (type && lower(Impl::trimmed(*type)) == "importmap" && !s.rt.import_map.add(n->textContent(), s.host.base))
            s.report("an import map that isn't valid JSON was ignored");
    }
    // Parser-blocking scripts in order, then the deferred ones (modules, defer, async).
    for (Node* n : scripts)
        if (!Impl::deferred(n)) s.runScript(n);
    for (Node* n : scripts)
        if (Impl::deferred(n)) s.runScript(n);
    s.running_scripts = false;
    s.ready_state = "interactive";
    s.fire(s.doc.root(), u"readystatechange", false, false);
    s.fire(s.doc.root(), u"DOMContentLoaded", true, false);
    s.ready_state = "complete";
    s.fire(s.doc.root(), u"readystatechange", false, false);
    s.fire(nullptr, u"load", false, false);
    s.collect();
}

bool PageScript::click(Node* target, float page_x, float page_y) {
    Impl& s = *impl_;
    if (!target) return true;
    const float sy = s.host.scroll_y ? s.host.scroll_y() : 0;
    const auto more = [&](Object* e) {
        for (auto [k, v] : std::initializer_list<std::pair<const char16_t*, double>>{
                 {u"clientX", page_x}, {u"clientY", page_y - sy}, {u"pageX", page_x}, {u"pageY", page_y}, {u"screenX", page_x}, {u"screenY", page_y - sy},
                 {u"button", 0}, {u"buttons", 1}, {u"detail", 1}})
            s.rt.put(e, k, Value::number(v), js::kEnumerable);
        for (const char16_t* k : {u"ctrlKey", u"shiftKey", u"altKey", u"metaKey"}) s.rt.put(e, k, Value::boolean(false), js::kEnumerable);
    };
    s.fire(target, u"mousedown", true, true, more);
    s.fire(target, u"mouseup", true, true, more);
    return s.fire(target, u"click", true, true, more);
}

bool PageScript::submit(Node* form, Node* submitter) {
    Impl& s = *impl_;
    return s.fire(form, u"submit", true, true, [&](Object* e) { s.rt.put(e, u"submitter", s.wrap(submitter), js::kEnumerable); });
}

void PageScript::input(Node* control) { impl_->fire(control, u"input", true, false); }
void PageScript::change(Node* control) { impl_->fire(control, u"change", true, false); }

bool PageScript::key(Node* target, bool down, const std::string& key, int key_code, uint32_t modifiers) {
    Impl& s = *impl_;
    if (!target) target = s.doc.body() ? s.doc.body() : s.doc.root();
    return s.fire(target, down ? u"keydown" : u"keyup", true, true, [&](Object* e) {
        s.rt.put(e, u"key", str(key), js::kEnumerable);
        s.rt.put(e, u"code", str(key.size() == 1 && std::isalpha(static_cast<unsigned char>(key[0])) ? "Key" + upper(key) : key), js::kEnumerable);
        s.rt.put(e, u"keyCode", Value::number(key_code), js::kEnumerable);
        s.rt.put(e, u"which", Value::number(key_code), js::kEnumerable);
        s.rt.put(e, u"shiftKey", Value::boolean(modifiers & 1), js::kEnumerable);
        s.rt.put(e, u"ctrlKey", Value::boolean(modifiers & 2), js::kEnumerable);
        s.rt.put(e, u"altKey", Value::boolean(modifiers & 4), js::kEnumerable);
        s.rt.put(e, u"metaKey", Value::boolean(modifiers & 8), js::kEnumerable);
        s.rt.put(e, u"repeat", Value::boolean(false), js::kEnumerable);
    });
}

void PageScript::resized(float w, float h) {
    impl_->host.viewport_w = w;
    impl_->host.viewport_h = h;
    impl_->fire(nullptr, u"resize", false, false);
}

void PageScript::scrolled() { impl_->fire(nullptr, u"scroll", false, false); }

void PageScript::tick() {
    Impl& s = *impl_;
    const double now = s.now();
    // Due timers, earliest first; at most so many a tick, so the page can't starve the window.
    for (int ran = 0; ran < 200; ++ran) {
        auto it = std::min_element(s.timers.begin(), s.timers.end(), [](const Impl::Timer& a, const Impl::Timer& b) {
            return a.due < b.due || (a.due == b.due && a.id < b.id);
        });
        if (it == s.timers.end() || it->due > now) break;
        Impl::Timer t = *it;
        if (t.repeat) it->due = now + t.interval;
        else s.timers.erase(it);
        s.task([&] { s.rt.call(t.fn, Value::object(s.rt.global()), t.args); });
        s.drainMicrotasks();
    }
    if (!s.frames.empty()) {
        std::vector<std::pair<int, Value>> frames;
        frames.swap(s.frames);
        const Value stamp = Value::number(now);
        for (auto& [id, fn] : frames) s.task([&] { s.rt.call(fn, Value::undefined(), std::span<const Value>(&stamp, 1)); });
        s.drainMicrotasks();
    }
    s.collect();
}

bool PageScript::busy() const { return !impl_->timers.empty() || !impl_->frames.empty(); }

bool PageScript::takeDirty() {
    const bool d = impl_->dirty;
    impl_->dirty = false;
    return d;
}

}  // namespace fbx::xweb
