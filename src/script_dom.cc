// The DOM's newer parts, bound to the page's tree: an interface per element (HTMLDivElement
// and the rest, for instanceof and for the members only some elements have), custom
// elements and their reactions, shadow roots and slots, MutationObserver, constructible
// stylesheets, DocumentFragment and <template>'s content.

#include "script_impl.h"

#include <stdlib.h>

namespace fbx::xweb {

namespace {

// HTML elements' interfaces, by tag. Those not here are HTMLElement (or, for names that
// aren't HTML's, HTMLUnknownElement).
const std::pair<const char*, const char*> kInterfaces[] = {
    {"a", "HTMLAnchorElement"}, {"area", "HTMLAreaElement"}, {"audio", "HTMLAudioElement"}, {"br", "HTMLBRElement"},
    {"base", "HTMLBaseElement"}, {"body", "HTMLBodyElement"}, {"button", "HTMLButtonElement"}, {"canvas", "HTMLCanvasElement"},
    {"dl", "HTMLDListElement"}, {"data", "HTMLDataElement"}, {"datalist", "HTMLDataListElement"}, {"details", "HTMLDetailsElement"},
    {"dialog", "HTMLDialogElement"}, {"div", "HTMLDivElement"}, {"embed", "HTMLEmbedElement"}, {"fieldset", "HTMLFieldSetElement"},
    {"form", "HTMLFormElement"}, {"h1", "HTMLHeadingElement"}, {"h2", "HTMLHeadingElement"}, {"h3", "HTMLHeadingElement"},
    {"h4", "HTMLHeadingElement"}, {"h5", "HTMLHeadingElement"}, {"h6", "HTMLHeadingElement"}, {"head", "HTMLHeadElement"},
    {"hr", "HTMLHRElement"}, {"html", "HTMLHtmlElement"}, {"iframe", "HTMLIFrameElement"}, {"img", "HTMLImageElement"},
    {"input", "HTMLInputElement"}, {"li", "HTMLLIElement"}, {"label", "HTMLLabelElement"}, {"legend", "HTMLLegendElement"},
    {"link", "HTMLLinkElement"}, {"map", "HTMLMapElement"}, {"menu", "HTMLMenuElement"}, {"meta", "HTMLMetaElement"},
    {"meter", "HTMLMeterElement"}, {"del", "HTMLModElement"}, {"ins", "HTMLModElement"}, {"object", "HTMLObjectElement"},
    {"ol", "HTMLOListElement"}, {"optgroup", "HTMLOptGroupElement"}, {"option", "HTMLOptionElement"}, {"output", "HTMLOutputElement"},
    {"p", "HTMLParagraphElement"}, {"param", "HTMLParamElement"}, {"picture", "HTMLPictureElement"}, {"pre", "HTMLPreElement"},
    {"listing", "HTMLPreElement"}, {"xmp", "HTMLPreElement"}, {"progress", "HTMLProgressElement"}, {"q", "HTMLQuoteElement"},
    {"blockquote", "HTMLQuoteElement"}, {"script", "HTMLScriptElement"}, {"select", "HTMLSelectElement"}, {"slot", "HTMLSlotElement"},
    {"source", "HTMLSourceElement"}, {"span", "HTMLSpanElement"}, {"style", "HTMLStyleElement"}, {"caption", "HTMLTableCaptionElement"},
    {"td", "HTMLTableCellElement"}, {"th", "HTMLTableCellElement"}, {"col", "HTMLTableColElement"}, {"colgroup", "HTMLTableColElement"},
    {"table", "HTMLTableElement"}, {"tr", "HTMLTableRowElement"}, {"thead", "HTMLTableSectionElement"}, {"tbody", "HTMLTableSectionElement"},
    {"tfoot", "HTMLTableSectionElement"}, {"template", "HTMLTemplateElement"}, {"textarea", "HTMLTextAreaElement"},
    {"time", "HTMLTimeElement"}, {"title", "HTMLTitleElement"}, {"track", "HTMLTrackElement"}, {"ul", "HTMLUListElement"},
    {"video", "HTMLVideoElement"},
};

// Tags that are HTML's own and simply HTMLElement.
const char* const kPlainHtml[] = {"abbr", "address", "article", "aside", "b", "bdi", "bdo", "cite", "code", "dd", "dfn", "dt", "em",
                                  "figcaption", "figure", "footer", "header", "hgroup", "i", "kbd", "main", "mark", "nav", "noscript",
                                  "rp", "rt", "ruby", "s", "samp", "search", "section", "small", "strong", "sub", "summary", "sup",
                                  "u", "var", "wbr", "acronym", "basefont", "big", "center", "nobr", "noembed", "noframes", "plaintext",
                                  "rb", "rtc", "strike", "tt"};

bool validCustomName(const std::string& n) {
    if (n.empty() || !(n[0] >= 'a' && n[0] <= 'z') || n.find('-') == std::string::npos) return false;
    for (char c : n)
        if (c >= 'A' && c <= 'Z') return false;
    for (const char* reserved : {"annotation-xml", "color-profile", "font-face", "font-face-src", "font-face-uri", "font-face-format",
                                 "font-face-name", "missing-glyph"})
        if (n == reserved) return false;
    return true;
}

[[noreturn]] void domError(Runtime& rt, const char* name, const std::string& message) {
    // A DOMException (the prelude's), or an Error by that name before it's there.
    const Value ctor = rt.get(Value::object(rt.global()), u"DOMException");
    if (rt.isConstructor(ctor)) {
        const Value args[2] = {str(message), str(name)};
        throw js::Thrown{rt.construct(ctor, args)};
    }
    Object* e = rt.newError("Error", message);
    rt.put(e, u"name", Value::string(js::utf16(name)), js::kWritable | js::kConfigurable);
    throw js::Thrown{Value::object(e)};
}

// Every element under `root` (itself too), shadow trees included, in tree order.
void shadowIncluding(Node* root, std::vector<Node*>& out) {
    std::vector<Node*> todo{root};
    while (!todo.empty() && out.size() < 1'000'000) {
        Node* n = todo.back();
        todo.pop_back();
        if (n->isElement()) out.push_back(n);
        todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        if (n->shadow_root) todo.push_back(n->shadow_root);
    }
}

}  // namespace

// ── MutationObserver's bookkeeping ──

void PageScript::Impl::domRoots(std::vector<Value>& roots) {
    for (auto& [name, def] : custom_definitions) {
        roots.push_back(Value::object(def.ctor));
        roots.push_back(Value::object(def.proto));
        for (const Value& v : {def.connected, def.disconnected, def.adopted, def.attribute_changed}) roots.push_back(v);
    }
    for (auto& [name, p] : when_defined) roots.push_back(Value::object(p));
    for (auto& o : observers) {
        roots.push_back(Value::object(o->object));
        for (Object* r : o->records) roots.push_back(Value::object(r));
    }
    for (auto& [node, arr] : adopters) roots.push_back(Value::object(arr));
}

Object* PageScript::Impl::protoFor(const Node* n) {
    switch (n->type) {
        case NodeType::Document: return proto_document;
        case NodeType::Text: return proto_text;
        case NodeType::Comment: return proto_comment ? proto_comment : proto_text;
        case NodeType::Doctype: return proto_doctype ? proto_doctype : proto_node;
        case NodeType::Fragment: return n->shadow_host ? proto_shadow_root : proto_fragment ? proto_fragment : proto_node;
        default: break;
    }
    if (!proto_html_element) return proto_element;
    if (n->ns == Namespace::Svg) {
        const auto it = interface_protos.find(n->name == "svg" ? "SVGSVGElement" : "SVGElement");
        return it != interface_protos.end() ? it->second : proto_svg_element;
    }
    if (n->ns != Namespace::Html) return proto_element;
    for (const auto& [tag, iface] : kInterfaces)
        if (n->name == tag) return interface_protos[iface];
    if (validCustomName(n->name)) return proto_html_element;
    for (const char* tag : kPlainHtml)
        if (n->name == tag) return proto_html_element;
    return proto_unknown;
}

// ── Custom elements ──

const PageScript::Impl::CustomDefinition* PageScript::Impl::definitionFor(const Node* el) const {
    if (!el->isElement() || el->ns != Namespace::Html) return nullptr;
    if (const auto it = custom_definitions.find(el->name); it != custom_definitions.end() && it->second.local_name == el->name) return &it->second;
    if (const std::string* is = el->attr("is"))
        if (const auto it = custom_definitions.find(*is); it != custom_definitions.end() && it->second.local_name == el->name) return &it->second;
    return nullptr;
}

void PageScript::Impl::callReaction(Node* el, const Value& callback, std::vector<Value> args) {
    if (!rt.isCallable(callback)) return;
    try {
        rt.call(callback, wrap(el), args);
    } catch (const js::Thrown& t) {
        // A reaction's error is reported, not thrown at whoever changed the tree.
        std::string message = "Uncaught exception";
        try {
            message = "Uncaught " + js::describeValue(rt, t.value) + js::describeWhere(rt, t.value);
        } catch (const js::Thrown&) {
        }
        report(message);
    }
}

void PageScript::Impl::upgrade(Node* el, const CustomDefinition& def) {
    if (el->custom_state == 2 || el->custom_state == 3) return;
    if (upgrade_candidates.size() > 64) return;
    upgrade_candidates.emplace_back(def.ctor, el);
    Value made;
    std::string failure;
    try {
        made = rt.construct(Value::object(def.ctor), {});
    } catch (const js::Thrown& t) {
        try {
            failure = "Uncaught " + js::describeValue(rt, t.value) + js::describeWhere(rt, t.value);
        } catch (const js::Thrown&) {
            failure = "Uncaught exception";
        }
    }
    upgrade_candidates.pop_back();
    if (failure.empty() && !(made.isObject() && made.asObject() == wrap(el).asObject()))
        failure = "TypeError: the custom element's constructor didn't give back the element being upgraded";
    if (!failure.empty()) {
        el->custom_state = 3;
        report(failure + " (<" + def.name + ">)");
        return;
    }
    el->custom_state = 2;
    dirty = true;
    for (const std::string& name : def.observed)
        if (const std::string* v = el->attr(name)) callReaction(el, def.attribute_changed, {str(name), Value::null(), str(*v)});
    if (inDocument(el)) callReaction(el, def.connected, {});
}

void PageScript::Impl::afterInsert(Node* parent, Node* child, Node* prev, Node* next) {
    queueMutation(u"childList", parent, {child}, {}, prev, next, nullptr, nullptr);
    if (!inDocument(parent)) return;
    scriptsInserted(child);
    if (custom_definitions.empty()) return;
    std::vector<Node*> els;
    shadowIncluding(child, els);
    for (Node* el : els) {
        if (!inDocument(el)) continue;   // a reaction moved it out again
        if (el->custom_state == 2) {
            if (const CustomDefinition* def = definitionFor(el)) callReaction(el, def->connected, {});
        } else if (const CustomDefinition* def = definitionFor(el)) {
            upgrade(el, *def);
        }
    }
}

void PageScript::Impl::afterRemove(Node* parent, Node* child, Node* prev, Node* next, bool was_connected) {
    queueMutation(u"childList", parent, {}, {child}, prev, next, nullptr, nullptr);
    if (!was_connected || custom_definitions.empty()) return;
    std::vector<Node*> els;
    shadowIncluding(child, els);
    for (Node* el : els)
        if (el->custom_state == 2)
            if (const CustomDefinition* def = definitionFor(el)) callReaction(el, def->disconnected, {});
}

void PageScript::Impl::afterAttr(Node* n, const std::string& name, const std::string* old) {
    queueMutation(u"attributes", n, {}, {}, nullptr, nullptr, &name, old);
    // An image a script points somewhere loads whether or not it's in the page (new
    // Image() for a canvas, preloading): its load event comes when it has.
    if (name == "src" && n->isHtml("img") && host.load_image)
        if (const std::string* src = n->attr("src"); src && !src->empty()) {
            if (std::find(script_images.begin(), script_images.end(), n) == script_images.end() && script_images.size() < 10000)
                script_images.push_back(n);
            host.load_image(resolveUrl(host.base, *src));
        }
    if (n->custom_state != 2) return;
    const CustomDefinition* def = definitionFor(n);
    if (!def || std::find(def->observed.begin(), def->observed.end(), name) == def->observed.end()) return;
    const std::string* now = n->attr(name);
    callReaction(n, def->attribute_changed, {str(name), old ? str(*old) : Value::null(), now ? str(*now) : Value::null()});
}

void PageScript::Impl::afterCharacterData(Node* n, const std::string& old) {
    queueMutation(u"characterData", n, {}, {}, nullptr, nullptr, nullptr, &old);
}

// ── Mutation records ──

void PageScript::Impl::queueMutation(const char16_t* type, Node* target, const std::vector<Node*>& added, const std::vector<Node*>& removed,
                                     Node* prev, Node* next, const std::string* attr_name, const std::string* old_value) {
    if (registrations.empty()) return;
    const std::u16string kind = type;
    // Who's interested, and whether they want the old value.
    std::vector<std::pair<ObserverState*, bool>> interested;
    for (const Node* n = target; n; n = n->parent) {
        const auto it = registrations.find(n);
        if (it == registrations.end()) continue;
        for (const Registration& r : it->second) {
            if (n != target && !r.subtree) continue;
            bool old = false;
            if (kind == u"attributes") {
                if (!r.attributes) continue;
                if (r.has_filter && std::find(r.attribute_filter.begin(), r.attribute_filter.end(), *attr_name) == r.attribute_filter.end()) continue;
                old = r.attribute_old_value;
            } else if (kind == u"characterData") {
                if (!r.character_data) continue;
                old = r.character_data_old_value;
            } else if (!r.child_list) {
                continue;
            }
            auto found = std::find_if(interested.begin(), interested.end(), [&](const auto& p) { return p.first == r.observer; });
            if (found == interested.end()) interested.emplace_back(r.observer, old);
            else found->second = found->second || old;
        }
    }
    if (interested.empty()) return;
    for (auto& [obs, want_old] : interested) {
        if (obs->records.size() > 100'000) continue;
        Object* rec = rt.newObject();
        const auto set = [&](const char16_t* k, Value v) { rt.put(rec, k, std::move(v), js::kEnumerable | js::kConfigurable); };
        set(u"type", Value::string(kind));
        set(u"target", wrap(target));
        std::vector<Node*> a = added, r = removed;
        set(u"addedNodes", list(a));
        set(u"removedNodes", list(r));
        set(u"previousSibling", wrap(prev));
        set(u"nextSibling", wrap(next));
        set(u"attributeName", attr_name ? str(*attr_name) : Value::null());
        set(u"attributeNamespace", Value::null());
        set(u"oldValue", want_old && old_value ? str(*old_value) : Value::null());
        obs->records.push_back(rec);
    }
    if (mutation_delivery_queued) return;
    mutation_delivery_queued = true;
    // Delivered at the next microtask checkpoint, all records of an observer together.
    Impl* self = this;
    Object* deliver = rt.newFunction(u"", 0, [self](Runtime& rt, const Value&, Args) {
        self->mutation_delivery_queued = false;
        std::vector<ObserverState*> todo;
        for (auto& o : self->observers) todo.push_back(o.get());
        for (ObserverState* o : todo) {
            if (o->records.empty()) continue;
            std::vector<Value> recs;
            for (Object* r : o->records) recs.push_back(Value::object(r));
            o->records.clear();
            const Value args[2] = {Value::object(rt.newArray(std::move(recs))), Value::object(o->object)};
            try {
                rt.call(o->object->slots[0], Value::object(o->object), args);
            } catch (const js::Thrown& t) {
                std::string message = "Uncaught exception";
                try {
                    message = "Uncaught " + js::describeValue(rt, t.value) + js::describeWhere(rt, t.value);
                } catch (const js::Thrown&) {
                }
                self->report(message);
            }
        }
        return Value::undefined();
    });
    rt.enqueueMicrotask(Value::object(deliver));
}

// ── Installing ──

void PageScript::Impl::installDom() {
    Impl* self = this;
    Object* g = rt.global();
    const auto illegal = [&](const char16_t* name, Object* proto, Object* parent_ctor) {
        Object* c = rt.newFunction(name, 0, [](Runtime& rt, const Value&, Args) -> Value { rt.throwError("TypeError", "Illegal constructor"); }, true);
        rt.put(c, u"prototype", Value::object(proto), 0);
        rt.put(proto, PropertyKey(rt.sym_to_string_tag), Value::string(name), js::kConfigurable);
        rt.put(proto, u"constructor", Value::object(c));
        if (parent_ctor) c->prototype = parent_ctor;
        rt.put(g, name, Value::object(c));
        return c;
    };
    const Value element_ctor = rt.get(Value::object(g), u"Element");
    const Value node_ctor = rt.get(Value::object(g), u"Node");

    // HTMLElement and every HTML interface: constructible only by a custom element's
    // class, through super().
    const auto htmlConstruct = [self](Runtime& rt, const Value&, Args) -> Value {
        Object* nt = rt.construct_target;
        if (!nt) rt.throwError("TypeError", "Illegal constructor: use document.createElement");
        const CustomDefinition* def = nullptr;
        for (auto& [name, d] : self->custom_definitions)
            if (d.ctor == nt) def = &d;
        if (!def) rt.throwError("TypeError", "Illegal constructor: this class isn't a defined custom element");
        const Value pv = rt.get(Value::object(nt), u"prototype");
        Object* proto = pv.isObject() ? pv.asObject() : self->proto_html_element;
        // This definition's construction stack: the innermost element it's upgrading.
        auto mine = std::find_if(self->upgrade_candidates.rbegin(), self->upgrade_candidates.rend(),
                                 [&](const auto& c) { return c.first == def->ctor; });
        if (mine != self->upgrade_candidates.rend()) {
            if (!mine->second) rt.throwError("TypeError", "the custom element was already constructed");
            Node* el = mine->second;
            mine->second = nullptr;   // constructed: a second super() fails
            Object* w = self->wrap(el).asObject();
            w->prototype = proto;
            return Value::object(w);
        }
        Node* n = self->doc.make(NodeType::Element);
        n->name = def->local_name;
        if (def->local_name != def->name) n->attrs.push_back({"is", def->name});
        n->custom_state = 2;
        Object* w = self->wrap(n).asObject();
        w->prototype = proto;
        return Value::object(w);
    };
    const auto htmlCtor = [&](const char16_t* name, Object* proto, Object* parent_ctor) {
        Object* c = rt.newFunction(name, 0, htmlConstruct, true);
        rt.put(c, u"prototype", Value::object(proto), 0);
        rt.put(proto, PropertyKey(rt.sym_to_string_tag), Value::string(name), js::kConfigurable);
        rt.put(proto, u"constructor", Value::object(c));
        if (parent_ctor) c->prototype = parent_ctor;
        rt.put(g, name, Value::object(c));
        return c;
    };
    proto_html_element = rt.newObject(proto_element);
    Object* html_ctor = htmlCtor(u"HTMLElement", proto_html_element, element_ctor.isObject() ? element_ctor.asObject() : nullptr);
    proto_unknown = rt.newObject(proto_html_element);
    htmlCtor(u"HTMLUnknownElement", proto_unknown, html_ctor);
    Object* media = rt.newObject(proto_html_element);
    Object* media_ctor = htmlCtor(u"HTMLMediaElement", media, html_ctor);
    for (const auto& [tag, iface] : kInterfaces) {
        if (interface_protos.count(iface)) continue;
        const std::string n = iface;
        const bool is_media = n == "HTMLAudioElement" || n == "HTMLVideoElement";
        Object* p = rt.newObject(is_media ? media : proto_html_element);
        interface_protos[iface] = p;
        htmlCtor(js::utf16(iface).c_str(), p, is_media ? media_ctor : html_ctor);
    }
    proto_svg_element = rt.newObject(proto_element);
    Object* svg_ctor = illegal(u"SVGElement", proto_svg_element, element_ctor.isObject() ? element_ctor.asObject() : nullptr);
    for (const char* iface : {"SVGGraphicsElement", "SVGSVGElement"}) {
        Object* p = rt.newObject(proto_svg_element);
        interface_protos[iface] = p;
        illegal(js::utf16(iface).c_str(), p, svg_ctor);
    }
    // Comment, DocumentType, DocumentFragment, ShadowRoot.
    proto_comment = rt.newObject(proto_text->prototype ? proto_text->prototype : proto_node);
    illegal(u"Comment", proto_comment, nullptr);
    accessor(proto_comment, u"data", [self](Runtime&, const Value& t, Args) { return str(self->thisNode(t)->data); },
             [self](Runtime& rt, const Value& t, Args a) {
                 self->setText(self->thisNode(t), js::utf8(rt.toStringValue(arg(a, 0))));
                 return Value::undefined();
             });
    // Nodes our HTML parser never makes, but polyfills patch their prototypes.
    illegal(u"CDATASection", rt.newObject(proto_text), nullptr);
    illegal(u"ProcessingInstruction", rt.newObject(proto_text->prototype ? proto_text->prototype : proto_node), nullptr);
    proto_doctype = rt.newObject(proto_node);
    illegal(u"DocumentType", proto_doctype, nullptr);
    accessor(proto_doctype, u"name", [self](Runtime&, const Value& t, Args) { return str(self->thisNode(t)->name); });
    accessor(proto_doctype, u"publicId", [self](Runtime&, const Value& t, Args) { return str(self->thisNode(t)->public_id); });
    accessor(proto_doctype, u"systemId", [self](Runtime&, const Value& t, Args) { return str(self->thisNode(t)->system_id); });
    proto_fragment = rt.newObject(proto_node);
    {
        Object* c = rt.newFunction(u"DocumentFragment", 0, [self](Runtime&, const Value&, Args) {
            return self->wrap(self->doc.make(NodeType::Fragment));
        }, true);
        rt.put(c, u"prototype", Value::object(proto_fragment), 0);
        rt.put(proto_fragment, PropertyKey(rt.sym_to_string_tag), Value::string(u"DocumentFragment"), js::kConfigurable);
        rt.put(proto_fragment, u"constructor", Value::object(c));
        if (node_ctor.isObject()) c->prototype = node_ctor.asObject();
        rt.put(g, u"DocumentFragment", Value::object(c));
    }
    method(proto_fragment, u"getElementById", 1, [self](Runtime& rt, const Value& t, Args a) {
        const std::string id = js::utf8(rt.toStringValue(arg(a, 0)));
        std::vector<Node*> todo(self->thisNode(t)->children.rbegin(), self->thisNode(t)->children.rend());
        while (!todo.empty()) {
            Node* n = todo.back();
            todo.pop_back();
            if (const std::string* v = n->attr("id"); v && *v == id && n->isElement()) return self->wrap(n);
            todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        }
        return Value::null();
    });
    proto_shadow_root = rt.newObject(proto_fragment);
    illegal(u"ShadowRoot", proto_shadow_root, nullptr);
    accessor(proto_shadow_root, u"host", [self](Runtime&, const Value& t, Args) { return self->wrap(self->thisNode(t)->shadow_host); });
    accessor(proto_shadow_root, u"mode", [self](Runtime&, const Value& t, Args) { return str(self->thisNode(t)->shadow_open ? "open" : "closed"); });
    accessor(proto_shadow_root, u"delegatesFocus", [](Runtime&, const Value&, Args) { return Value::boolean(false); });
    accessor(proto_shadow_root, u"slotAssignment", [](Runtime&, const Value&, Args) { return str("named"); });
    accessor(proto_shadow_root, u"activeElement", [](Runtime&, const Value&, Args) { return Value::null(); });
    accessor(proto_shadow_root, u"innerHTML", [self](Runtime&, const Value& t, Args) { return str(serializeChildren(*self->thisNode(t))); },
             [self](Runtime& rt, const Value& t, Args a) {
                 self->setInnerHtml(self->thisNode(t), js::utf8(rt.toStringValue(arg(a, 0))));
                 return Value::undefined();
             });

    // attachShadow and shadowRoot.
    method(proto_element, u"attachShadow", 1, [self](Runtime& rt, const Value& t, Args a) {
        Node* el = self->thisElement(t);
        static const char* const kHosts[] = {"article", "aside", "blockquote", "body", "div", "footer", "h1", "h2", "h3", "h4", "h5", "h6",
                                             "header", "main", "nav", "p", "section", "span"};
        const bool ok = el->ns == Namespace::Html &&
                        (validCustomName(el->name) || std::any_of(std::begin(kHosts), std::end(kHosts), [&](const char* h) { return el->name == h; }));
        if (!ok) domError(rt, "NotSupportedError", "this element can't have a shadow root");
        if (el->shadow_root) domError(rt, "NotSupportedError", "the element already has a shadow root");
        if (!arg(a, 0).isObject()) rt.throwError("TypeError", "attachShadow needs {mode}");
        const std::string mode = js::utf8(rt.toStringValue(rt.get(arg(a, 0), u"mode")));
        if (mode != "open" && mode != "closed") rt.throwError("TypeError", "a shadow root's mode is open or closed");
        Node* root = self->doc.make(NodeType::Fragment);
        root->shadow_host = el;
        root->shadow_open = mode == "open";
        el->shadow_root = root;
        self->dirty = true;
        return self->wrap(root);
    });
    accessor(proto_element, u"shadowRoot", [self](Runtime&, const Value& t, Args) {
        const Node* el = self->thisElement(t);
        return el->shadow_root && el->shadow_root->shadow_open ? self->wrap(el->shadow_root) : Value::null();
    });
    accessor(proto_element, u"slot", [self](Runtime&, const Value& t, Args) { return str(self->attr(self->thisElement(t), "slot")); },
             [self](Runtime& rt, const Value& t, Args a) {
                 self->setAttr(self->thisElement(t), "slot", js::utf8(rt.toStringValue(arg(a, 0))));
                 return Value::undefined();
             });
    accessor(proto_node, u"assignedSlot", [self](Runtime&, const Value& t, Args) {
        const Node* n = self->thisNode(t);
        if (!n->parent || !n->parent->shadow_root || !n->parent->shadow_root->shadow_open) return Value::null();
        std::vector<Node*> els;
        shadowIncluding(n->parent->shadow_root, els);
        for (Node* s : els) {
            if (!s->isHtml("slot")) continue;
            std::vector<const Node*> assigned;
            assignedNodes(s, assigned);
            if (std::find(assigned.begin(), assigned.end(), n) != assigned.end()) return self->wrap(s);
        }
        return Value::null();
    });
    {
        Object* slot = interface_protos["HTMLSlotElement"];
        accessor(slot, u"name", [self](Runtime&, const Value& t, Args) { return str(self->attr(self->thisElement(t), "name")); },
                 [self](Runtime& rt, const Value& t, Args a) {
                     self->setAttr(self->thisElement(t), "name", js::utf8(rt.toStringValue(arg(a, 0))));
                     return Value::undefined();
                 });
        const auto assigned = [self](bool elements_only) {
            return [self, elements_only](Runtime& rt, const Value& t, Args a) {
                Node* s = self->thisElement(t);
                const bool flatten = arg(a, 0).isObject() && rt.toBoolean(rt.get(arg(a, 0), u"flatten"));
                std::vector<const Node*> got;
                assignedNodes(s, got);
                if (got.empty() && flatten) got.assign(s->children.begin(), s->children.end());
                std::vector<Node*> out;
                for (const Node* n : got)
                    if (!elements_only || n->isElement()) out.push_back(const_cast<Node*>(n));
                return self->list(out);
            };
        };
        method(slot, u"assignedNodes", 0, assigned(false));
        method(slot, u"assignedElements", 0, assigned(true));
        method(slot, u"assign", 0, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    }
    // <template>'s content.
    accessor(interface_protos["HTMLTemplateElement"], u"content", [self](Runtime&, const Value& t, Args) {
        Node* el = self->thisElement(t);
        if (!el->template_content) {
            el->template_content = self->doc.make(NodeType::Fragment);
            const std::vector<Node*> kids = el->children;
            for (Node* k : kids) el->template_content->append(k);
        }
        return self->wrap(el->template_content);
    });
    // <dialog>.
    {
        Object* d = interface_protos["HTMLDialogElement"];
        accessor(d, u"open", [self](Runtime&, const Value& t, Args) { return Value::boolean(self->thisElement(t)->attr("open") != nullptr); },
                 [self](Runtime& rt, const Value& t, Args a) {
                     if (rt.toBoolean(arg(a, 0))) self->setAttr(self->thisElement(t), "open", "");
                     else self->removeAttr(self->thisElement(t), "open");
                     return Value::undefined();
                 });
        rt.put(d, u"returnValue", str(""), js::kWritable | js::kConfigurable | js::kEnumerable);
        for (const char16_t* name : {u"show", u"showModal"})
            method(d, name, 0, [self](Runtime&, const Value& t, Args) {
                self->setAttr(self->thisElement(t), "open", "");
                return Value::undefined();
            });
        method(d, u"close", 1, [self](Runtime& rt, const Value& t, Args a) {
            Node* el = self->thisElement(t);
            if (!el->attr("open")) return Value::undefined();
            if (!arg(a, 0).isUndefined()) rt.put(t.asObject(), u"returnValue", Value::string(rt.toString(arg(a, 0))), js::kWritable | js::kConfigurable | js::kEnumerable);
            self->removeAttr(el, "open");
            self->fire(el, u"close", false, false);
            return Value::undefined();
        });
    }
    // Form controls' members belong to the controls, not to every element: on Element
    // they'd be in the way of a custom element's own (Lit's `options`, a `type`, a `form`).
    {
        const std::pair<const char16_t*, std::vector<const char*>> owners[] = {
            {u"value", {"HTMLInputElement", "HTMLSelectElement", "HTMLTextAreaElement", "HTMLOptionElement", "HTMLButtonElement",
                        "HTMLDataElement", "HTMLOutputElement", "HTMLParamElement", "HTMLMeterElement", "HTMLProgressElement"}},
            {u"checked", {"HTMLInputElement"}},
            {u"selectedIndex", {"HTMLSelectElement"}},
            {u"options", {"HTMLSelectElement", "HTMLDataListElement"}},
            {u"type", {"HTMLInputElement", "HTMLButtonElement", "HTMLSelectElement", "HTMLTextAreaElement", "HTMLScriptElement",
                       "HTMLLinkElement", "HTMLStyleElement", "HTMLOListElement", "HTMLSourceElement", "HTMLEmbedElement",
                       "HTMLObjectElement", "HTMLOutputElement", "HTMLAnchorElement"}},
            {u"form", {"HTMLInputElement", "HTMLSelectElement", "HTMLTextAreaElement", "HTMLButtonElement", "HTMLFieldSetElement",
                       "HTMLOutputElement", "HTMLObjectElement", "HTMLLabelElement", "HTMLLegendElement", "HTMLOptionElement"}},
            {u"elements", {"HTMLFormElement", "HTMLFieldSetElement"}},
            {u"submit", {"HTMLFormElement"}},
            {u"requestSubmit", {"HTMLFormElement"}},
            {u"reset", {"HTMLFormElement"}},
        };
        for (const auto& [name, ifaces] : owners) {
            const js::Property* own = proto_element->own(PropertyKey(name));
            if (!own) continue;
            const js::Property kept = *own;
            for (const char* iface : ifaces)
                if (Object* proto = interface_protos[iface]; proto && !proto->own(PropertyKey(name))) proto->define(PropertyKey(name), kept);
            proto_element->remove(PropertyKey(name));
        }
    }
    // ParentNode's members belong to elements, documents and fragments, not to every
    // node: pages tell text from elements by asking for querySelectorAll (Reddit does).
    for (const char16_t* name : {u"children", u"childElementCount", u"firstElementChild", u"lastElementChild", u"append",
                                 u"prepend", u"replaceChildren", u"querySelector", u"querySelectorAll",
                                 u"getElementsByTagName", u"getElementsByClassName"}) {
        const js::Property* own = proto_node->own(PropertyKey(name));
        if (!own) continue;
        const js::Property kept = *own;
        const bool fragments_too = std::u16string_view(name).substr(0, 10) != u"getElement";
        for (Object* proto : {proto_element, proto_document, fragments_too ? proto_fragment : nullptr})
            if (proto && !proto->own(PropertyKey(name))) proto->define(PropertyKey(name), kept);
        proto_node->remove(PropertyKey(name));
    }
    // <details>'s open.
    accessor(interface_protos["HTMLDetailsElement"], u"open", [self](Runtime&, const Value& t, Args) { return Value::boolean(self->thisElement(t)->attr("open") != nullptr); },
             [self](Runtime& rt, const Value& t, Args a) {
                 if (rt.toBoolean(arg(a, 0))) self->setAttr(self->thisElement(t), "open", "");
                 else self->removeAttr(self->thisElement(t), "open");
                 return Value::undefined();
             });
    // Media, canvas and images: as far as a page needs them not to fail.
    rt.put(media, u"paused", Value::boolean(true), js::kWritable | js::kConfigurable);
    rt.put(media, u"muted", Value::boolean(false), js::kWritable | js::kConfigurable);
    rt.put(media, u"volume", Value::number(1), js::kWritable | js::kConfigurable);
    rt.put(media, u"currentTime", Value::number(0), js::kWritable | js::kConfigurable);
    rt.put(media, u"duration", Value::number(NAN), js::kWritable | js::kConfigurable);
    rt.put(media, u"readyState", Value::number(0), js::kWritable | js::kConfigurable);
    method(media, u"play", 0, [](Runtime& rt, const Value&, Args) {
        Object* e = rt.newError("Error", "media can't play here yet");
        rt.put(e, u"name", str("NotSupportedError"), js::kWritable | js::kConfigurable);
        Object* p = rt.newPromise();
        rt.rejectPromise(p, Value::object(e));
        return Value::object(p);
    });
    method(media, u"pause", 0, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    method(media, u"load", 0, [](Runtime&, const Value&, Args) { return Value::undefined(); });
    method(media, u"canPlayType", 1, [](Runtime&, const Value&, Args) { return str(""); });
    {
        Object* canvas = interface_protos["HTMLCanvasElement"];
        method(canvas, u"getContext", 1, [](Runtime&, const Value&, Args) { return Value::null(); });
        method(canvas, u"toDataURL", 0, [](Runtime&, const Value&, Args) { return str("data:,"); });
        Object* img = interface_protos["HTMLImageElement"];
        // What the page's loader says of the image, by the URL it's showing.
        const auto state = [self](const Value& t) {
            const std::string* v = self->thisElement(t)->attr("src");
            if (!v || v->empty() || !self->host.image_state) return PageScript::Host::ImageState{true, false, 0, 0};
            return self->host.image_state(resolveUrl(self->host.base, *v));
        };
        accessor(img, u"complete", [state](Runtime&, const Value& t, Args) { return Value::boolean(state(t).done); });
        accessor(img, u"naturalWidth", [state](Runtime&, const Value& t, Args) { return Value::number(state(t).width); });
        accessor(img, u"naturalHeight", [state](Runtime&, const Value& t, Args) { return Value::number(state(t).height); });
        accessor(img, u"currentSrc", [self](Runtime&, const Value& t, Args) {
            const std::string* v = self->thisElement(t)->attr("src");
            return str(v ? resolveUrl(self->host.base, *v) : std::string{});
        });
        method(img, u"decode", 0, [](Runtime& rt, const Value&, Args) {
            Object* p = rt.newPromise();
            rt.resolvePromise(p, Value::undefined());
            return Value::object(p);
        });
        accessor(interface_protos["HTMLIFrameElement"], u"contentWindow", [](Runtime&, const Value&, Args) { return Value::null(); });
        accessor(interface_protos["HTMLIFrameElement"], u"contentDocument", [](Runtime&, const Value&, Args) { return Value::null(); });
    }

    // Small parts pages expect everywhere.
    method(proto_element, u"hasAttributes", 0, [self](Runtime&, const Value& t, Args) { return Value::boolean(!self->thisElement(t)->attrs.empty()); });
    for (const char16_t* name : {u"scrollTo", u"scroll", u"scrollBy"})
        method(proto_element, name, 0, [self, name](Runtime& rt, const Value& t, Args a) {
            Node* el = self->thisElement(t);
            if (!self->doc.body() || (el != self->doc.body() && el != self->doc.documentElement())) return Value::undefined();
            double y = 0;
            if (arg(a, 0).isObject()) y = rt.toNumber(rt.get(arg(a, 0), u"top"));
            else if (a.size() > 1) y = rt.toNumber(a[1]);
            if (std::u16string_view(name) == u"scrollBy" && self->host.scroll_y) y += self->host.scroll_y();
            if (self->host.scroll_to && std::isfinite(y)) self->host.scroll_to(0, static_cast<float>(y));
            return Value::undefined();
        });
    method(proto_document, u"importNode", 1, [self](Runtime& rt, const Value&, Args a) {
        Node* n = self->node(arg(a, 0));
        if (!n || n->type == NodeType::Document) domError(rt, "NotSupportedError", "that node can't be imported");
        return self->wrap(self->clone(n, rt.toBoolean(arg(a, 1))));
    });
    method(proto_document, u"adoptNode", 1, [self](Runtime& rt, const Value&, Args a) {
        Node* n = self->node(arg(a, 0));
        if (!n || n->type == NodeType::Document) domError(rt, "NotSupportedError", "that node can't be adopted");
        self->detach(n);
        return arg(a, 0);
    });
    {
        Object* css = rt.newObject();
        rt.put(css, PropertyKey(rt.sym_to_string_tag), str("CSS"), js::kConfigurable);
        method(css, u"supports", 1, [](Runtime& rt, const Value&, Args a) {
            std::string cond = js::utf8(rt.toStringValue(arg(a, 0)));
            if (a.size() > 1) cond = "(" + cond + ": " + js::utf8(rt.toStringValue(a[1])) + ")";
            else if (cond.find('(') == std::string::npos) cond = "(" + cond + ")";
            return Value::boolean(css::supportsCondition(cond));
        });
        method(css, u"escape", 1, [](Runtime& rt, const Value&, Args a) {
            const std::u16string s = rt.toStringValue(arg(a, 0));
            std::u16string out;
            for (size_t i = 0; i < s.size(); ++i) {
                const char16_t c = s[i];
                const auto hex = [&](char16_t ch) {
                    static const char* d = "0123456789abcdef";
                    std::u16string h;
                    for (uint32_t v = ch; v; v >>= 4) h.insert(h.begin(), static_cast<char16_t>(d[v & 15]));
                    out += u"\\" + (h.empty() ? std::u16string(u"0") : h) + u" ";
                };
                if (c == 0) out += u"\uFFFD";
                else if ((c >= 1 && c <= 0x1F) || c == 0x7F || (i == 0 && c >= u'0' && c <= u'9') || (i == 1 && c >= u'0' && c <= u'9' && s[0] == u'-')) hex(c);
                else if (i == 0 && c == u'-' && s.size() == 1) out += u"\\-";
                else if (c >= 0x80 || c == u'-' || c == u'_' || (c >= u'0' && c <= u'9') || (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z')) out += c;
                else { out += u'\\'; out += c; }
            }
            return Value::string(out);
        });
        rt.put(g, u"CSS", Value::object(css));
    }
    {
        const Value nav = rt.get(Value::object(g), u"navigator");
        if (nav.isObject())
            method(nav.asObject(), u"sendBeacon", 2, [](Runtime& rt, const Value&, Args a) {
                // Sent as a fetch POST that nobody waits for.
                const Value fetch = rt.get(Value::object(rt.global()), u"fetch");
                if (!rt.isCallable(fetch)) return Value::boolean(false);
                Object* init = rt.newObject();
                rt.put(init, u"method", str("POST"), js::kDefaultFlags);
                if (a.size() > 1) rt.put(init, u"body", a[1], js::kDefaultFlags);
                const Value args[2] = {arg(a, 0), Value::object(init)};
                const Value p = rt.call(fetch, Value::undefined(), args);
                if (p.isObject()) p.asObject()->promise_handled = true;
                return Value::boolean(true);
            });
    }

    // crypto: random values from the system (arc4random, which the sandbox allows).
    {
        Object* crypto = rt.newObject();
        rt.put(crypto, PropertyKey(rt.sym_to_string_tag), str("Crypto"), js::kConfigurable);
        method(crypto, u"getRandomValues", 1, [](Runtime& rt, const Value&, Args a) {
            const Value v = arg(a, 0);
            std::string probe;
            if (!v.isObject() || v.asObject()->cls != js::ObjectClass::TypedArray || !rt.bufferBytes(v, probe))
                rt.throwError("TypeError", "getRandomValues needs an integer typed array");
            if (probe.size() > 65536) domError(rt, "QuotaExceededError", "getRandomValues takes at most 65536 bytes");
            Object* ta = v.asObject();
            uint8_t* p = ta->target->buffer->bytes.data() + ta->view_offset;
            arc4random_buf(p, probe.size());
            return v;
        });
        method(crypto, u"randomUUID", 0, [](Runtime&, const Value&, Args) {
            uint8_t b[16];
            arc4random_buf(b, sizeof b);
            b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);
            b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);
            static const char* hex = "0123456789abcdef";
            std::string out;
            for (int i = 0; i < 16; ++i) {
                if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
                out += hex[b[i] >> 4];
                out += hex[b[i] & 15];
            }
            return str(out);
        });
        rt.put(g, u"crypto", Value::object(crypto));
    }

    // customElements.
    Object* registry_proto = rt.newObject();
    illegal(u"CustomElementRegistry", registry_proto, nullptr);
    Object* registry = rt.newObject(registry_proto);
    rt.put(g, u"customElements", Value::object(registry));
    method(registry, u"define", 2, [self](Runtime& rt, const Value&, Args a) {
        const std::string name = js::utf8(rt.toStringValue(arg(a, 0)));
        const Value ctor = arg(a, 1);
        if (!rt.isConstructor(ctor)) rt.throwError("TypeError", "customElements.define needs a class");
        if (!validCustomName(name)) domError(rt, "SyntaxError", "'" + name + "' isn't a valid custom element name");
        if (self->custom_definitions.count(name)) domError(rt, "NotSupportedError", "'" + name + "' is already defined");
        for (auto& [n, d] : self->custom_definitions)
            if (d.ctor == ctor.asObject()) domError(rt, "NotSupportedError", "this class is already defined as '" + n + "'");
        CustomDefinition def;
        def.name = name;
        def.local_name = name;
        if (arg(a, 2).isObject()) {
            const Value ext = rt.get(arg(a, 2), u"extends");
            if (!ext.isUndefined()) {
                const std::string e = js::utf8(rt.toStringValue(ext));
                if (validCustomName(e)) domError(rt, "NotSupportedError", "a custom element can't extend another custom element");
                def.local_name = e;
            }
        }
        def.ctor = ctor.asObject();
        const Value proto = rt.get(ctor, u"prototype");
        if (!proto.isObject()) rt.throwError("TypeError", "the class's prototype isn't an object");
        def.proto = proto.asObject();
        def.connected = rt.get(proto, u"connectedCallback");
        def.disconnected = rt.get(proto, u"disconnectedCallback");
        def.adopted = rt.get(proto, u"adoptedCallback");
        def.attribute_changed = rt.get(proto, u"attributeChangedCallback");
        if (rt.isCallable(def.attribute_changed)) {
            const Value observed = rt.get(ctor, u"observedAttributes");
            if (!observed.isUndefined())
                for (const Value& v : rt.iterateToList(observed)) def.observed.push_back(js::utf8(rt.toStringValue(v)));
        }
        self->custom_definitions[name] = def;
        const CustomDefinition& kept = self->custom_definitions[name];
        // Upgrade what's already in the document, in tree order.
        std::vector<Node*> els;
        shadowIncluding(self->doc.root(), els);
        for (Node* el : els)
            if (self->definitionFor(el) == &kept) self->upgrade(el, kept);
        if (const auto it = self->when_defined.find(name); it != self->when_defined.end()) {
            Object* p = it->second;
            self->when_defined.erase(it);
            rt.resolvePromise(p, ctor);
        }
        return Value::undefined();
    });
    method(registry, u"get", 1, [self](Runtime& rt, const Value&, Args a) {
        const auto it = self->custom_definitions.find(js::utf8(rt.toStringValue(arg(a, 0))));
        return it == self->custom_definitions.end() ? Value::undefined() : Value::object(it->second.ctor);
    });
    method(registry, u"getName", 1, [self](Runtime&, const Value&, Args a) {
        for (auto& [n, d] : self->custom_definitions)
            if (arg(a, 0).isObject() && d.ctor == arg(a, 0).asObject()) return str(n);
        return Value::null();
    });
    method(registry, u"whenDefined", 1, [self](Runtime& rt, const Value&, Args a) {
        const std::string name = js::utf8(rt.toStringValue(arg(a, 0)));
        Object* p = rt.newPromise();
        if (!validCustomName(name)) {
            Object* e = rt.newError("Error", "'" + name + "' isn't a valid custom element name");
            rt.put(e, u"name", str("SyntaxError"), js::kWritable | js::kConfigurable);
            rt.rejectPromise(p, Value::object(e));
            return Value::object(p);
        }
        if (const auto it = self->custom_definitions.find(name); it != self->custom_definitions.end()) {
            rt.resolvePromise(p, Value::object(it->second.ctor));
            return Value::object(p);
        }
        Object*& pending = self->when_defined[name];
        if (!pending) pending = p;
        return Value::object(pending);
    });
    method(registry, u"upgrade", 1, [self](Runtime&, const Value&, Args a) {
        if (Node* root = self->node(arg(a, 0))) {
            std::vector<Node*> els;
            shadowIncluding(root, els);
            for (Node* el : els)
                if (const CustomDefinition* d = self->definitionFor(el)) self->upgrade(el, *d);
        }
        return Value::undefined();
    });

    // MutationObserver.
    proto_mutation_observer = rt.newObject();
    {
        Object* c = rt.newFunction(u"MutationObserver", 1, [self](Runtime& rt, const Value&, Args a) {
            Object* nt = rt.construct_target;
            if (!nt) rt.throwError("TypeError", "MutationObserver must be made with new");
            if (!rt.isCallable(arg(a, 0))) rt.throwError("TypeError", "MutationObserver needs a callback");
            const Value pv = rt.get(Value::object(nt), u"prototype");
            Object* o = rt.newObject(pv.isObject() ? pv.asObject() : self->proto_mutation_observer);
            o->slots = {arg(a, 0)};
            auto state = std::make_unique<ObserverState>();
            state->object = o;
            self->observers.push_back(std::move(state));
            return Value::object(o);
        }, true);
        rt.put(c, u"prototype", Value::object(proto_mutation_observer), 0);
        rt.put(proto_mutation_observer, PropertyKey(rt.sym_to_string_tag), Value::string(u"MutationObserver"), js::kConfigurable);
        rt.put(proto_mutation_observer, u"constructor", Value::object(c));
        rt.put(g, u"MutationObserver", Value::object(c));
        rt.put(g, u"WebKitMutationObserver", Value::object(c));
    }
    const auto stateOf = [self](Runtime& rt, const Value& t) -> ObserverState* {
        if (t.isObject())
            for (auto& o : self->observers)
                if (o->object == t.asObject()) return o.get();
        rt.throwError("TypeError", "not a MutationObserver");
    };
    method(proto_mutation_observer, u"observe", 2, [self, stateOf](Runtime& rt, const Value& t, Args a) {
        ObserverState* st = stateOf(rt, t);
        Node* target = self->node(arg(a, 0));
        if (!target) rt.throwError("TypeError", "observe needs a node");
        Registration r;
        r.observer = st;
        const Value opts = arg(a, 1);
        const auto flag = [&](const char16_t* k, bool& into) {
            if (!opts.isObject()) return false;
            const Value v = rt.get(opts, k);
            if (v.isUndefined()) return false;
            into = rt.toBoolean(v);
            return true;
        };
        flag(u"childList", r.child_list);
        flag(u"subtree", r.subtree);
        const bool has_attributes = flag(u"attributes", r.attributes);
        const bool has_cdata = flag(u"characterData", r.character_data);
        if (flag(u"attributeOldValue", r.attribute_old_value) && !has_attributes) r.attributes = true;
        if (flag(u"characterDataOldValue", r.character_data_old_value) && !has_cdata) r.character_data = true;
        if (opts.isObject()) {
            const Value filter = rt.get(opts, u"attributeFilter");
            if (!filter.isUndefined()) {
                for (const Value& v : rt.iterateToList(filter)) r.attribute_filter.push_back(js::utf8(rt.toStringValue(v)));
                r.has_filter = true;
                if (!has_attributes) r.attributes = true;
            }
        }
        if (!r.child_list && !r.attributes && !r.character_data)
            rt.throwError("TypeError", "observe needs childList, attributes or characterData");
        auto& list = self->registrations[target];
        for (Registration& existing : list)
            if (existing.observer == st) {
                existing = r;
                return Value::undefined();
            }
        list.push_back(r);
        return Value::undefined();
    });
    method(proto_mutation_observer, u"disconnect", 0, [self, stateOf](Runtime& rt, const Value& t, Args) {
        ObserverState* st = stateOf(rt, t);
        for (auto it = self->registrations.begin(); it != self->registrations.end();) {
            auto& v = it->second;
            v.erase(std::remove_if(v.begin(), v.end(), [&](const Registration& r) { return r.observer == st; }), v.end());
            it = v.empty() ? self->registrations.erase(it) : std::next(it);
        }
        st->records.clear();
        return Value::undefined();
    });
    method(proto_mutation_observer, u"takeRecords", 0, [stateOf](Runtime& rt, const Value& t, Args) {
        ObserverState* st = stateOf(rt, t);
        std::vector<Value> recs;
        for (Object* r : st->records) recs.push_back(Value::object(r));
        st->records.clear();
        return Value::object(rt.newArray(std::move(recs)));
    });

    // Constructible stylesheets, and adoptedStyleSheets on shadow roots and the document.
    Object* sheet_proto = rt.newObject();
    {
        Object* c = rt.newFunction(u"CSSStyleSheet", 0, [sheet_proto](Runtime& rt, const Value&, Args) {
            Object* nt = rt.construct_target;
            if (!nt) rt.throwError("TypeError", "CSSStyleSheet must be made with new");
            const Value pv = rt.get(Value::object(nt), u"prototype");
            Object* s = rt.newObject(pv.isObject() ? pv.asObject() : sheet_proto);
            s->primitive = Value::string(u"");
            return Value::object(s);
        }, true);
        rt.put(c, u"prototype", Value::object(sheet_proto), 0);
        rt.put(sheet_proto, u"constructor", Value::object(c));
        rt.put(g, u"CSSStyleSheet", Value::object(c));
    }
    const auto resync = [self] {
        // Every adopter's sheets as text again: a sheet's change shows wherever it's adopted.
        for (auto& [node, arr] : self->adopters) {
            node->adopted_css.clear();
            for (const Value& v : arr->elements)
                if (v.isObject() && v.asObject()->primitive.isString()) node->adopted_css.push_back(js::utf8(v.asObject()->primitive.asString()));
        }
        self->dirty = true;
    };
    method(sheet_proto, u"replaceSync", 1, [resync](Runtime& rt, const Value& t, Args a) {
        if (!t.isObject()) rt.throwError("TypeError", "not a CSSStyleSheet");
        t.asObject()->primitive = Value::string(rt.toString(arg(a, 0)));
        resync();
        return Value::undefined();
    });
    method(sheet_proto, u"replace", 1, [resync](Runtime& rt, const Value& t, Args a) {
        if (!t.isObject()) rt.throwError("TypeError", "not a CSSStyleSheet");
        t.asObject()->primitive = Value::string(rt.toString(arg(a, 0)));
        resync();
        Object* p = rt.newPromise();
        rt.resolvePromise(p, t);
        return Value::object(p);
    });
    // An element's sheet keeps the rules added to it on the element, where styling reads them.
    const auto owner = [](const Value& t) -> Node* {
        return t.isObject() && t.asObject()->host_kind == kOwnedSheet ? static_cast<Node*>(t.asObject()->host) : nullptr;
    };
    method(sheet_proto, u"insertRule", 1, [self, resync, owner](Runtime& rt, const Value& t, Args a) {
        if (!t.isObject()) rt.throwError("TypeError", "not a CSSStyleSheet");
        const std::string rule = js::utf8(rt.toStringValue(arg(a, 0)));
        if (Node* n = owner(t)) {
            const double at = arg(a, 1).isUndefined() ? 0 : rt.toIntegerOrInfinity(arg(a, 1));
            if (at < 0 || at > static_cast<double>(n->adopted_css.size())) rt.throwError("Error", "IndexSizeError: no such rule index");
            if (n->adopted_css.size() >= 100000) rt.throwError("Error", "QuotaExceededError: too many rules");
            n->adopted_css.insert(n->adopted_css.begin() + static_cast<ptrdiff_t>(at), rule);
            self->dirty = true;
            return Value::number(at);
        }
        Object* s = t.asObject();
        const std::u16string old = s->primitive.isString() ? s->primitive.asString() : u"";
        s->primitive = Value::string(old + u"\n" + js::utf16(rule));
        resync();
        return Value::number(0);
    });
    method(sheet_proto, u"deleteRule", 1, [self, owner](Runtime& rt, const Value& t, Args a) {
        Node* n = owner(t);
        if (!n) return Value::undefined();
        const double at = rt.toIntegerOrInfinity(arg(a, 0));
        if (at < 0 || at >= static_cast<double>(n->adopted_css.size())) rt.throwError("Error", "IndexSizeError: no such rule index");
        n->adopted_css.erase(n->adopted_css.begin() + static_cast<ptrdiff_t>(at));
        self->dirty = true;
        return Value::undefined();
    });
    accessor(sheet_proto, u"cssRules", [owner](Runtime& rt, const Value& t, Args) {
        std::vector<Value> rules;
        const auto add = [&](std::u16string_view text) {
            while (!text.empty() && (text.front() == u' ' || text.front() == u'\n' || text.front() == u'\t' || text.front() == u'\r')) text.remove_prefix(1);
            if (text.empty() || rules.size() >= 100000) return;
            Object* r = rt.newObject();
            rt.put(r, u"cssText", Value::string(std::u16string(text)), js::kEnumerable);
            rules.push_back(Value::object(r));
        };
        if (Node* n = owner(t)) {
            for (const std::string& css : n->adopted_css) add(js::utf16(css));
        } else if (t.isObject() && t.asObject()->primitive.isString()) {
            // A constructed sheet's text, rule by rule: split where a top-level block or
            // statement ends (strings and comments don't count).
            const std::u16string& css = t.asObject()->primitive.asString();
            size_t start = 0;
            int depth = 0;
            char16_t quote = 0;
            for (size_t i = 0; i < css.size(); ++i) {
                const char16_t c = css[i];
                if (quote) {
                    if (c == u'\\') ++i;
                    else if (c == quote) quote = 0;
                    continue;
                }
                if (c == u'/' && i + 1 < css.size() && css[i + 1] == u'*') {
                    const size_t end = css.find(u"*/", i + 2);
                    if (end == std::u16string::npos) break;
                    if (depth == 0 && start == i) start = end + 2;
                    i = end + 1;
                    continue;
                }
                if (c == u'"' || c == u'\'') quote = c;
                else if (c == u'{') ++depth;
                else if (c == u'}' && depth > 0 && --depth == 0) { add(std::u16string_view(css).substr(start, i + 1 - start)); start = i + 1; }
                else if (c == u';' && depth == 0) { add(std::u16string_view(css).substr(start, i + 1 - start)); start = i + 1; }
            }
        }
        return Value::object(rt.newArray(std::move(rules)));
    });
    accessor(sheet_proto, u"ownerNode", [self, owner](Runtime&, const Value& t, Args) { return self->wrap(owner(t)); });
    // <style>.sheet and <link rel=stylesheet>.sheet, one each, kept on the element's wrapper.
    const auto sheetOf = [self, sheet_proto](Runtime& rt, Node* n) -> Value {
        if (!n) return Value::null();
        if (n->isHtml("link")) {
            const std::string* rel = n->attr("rel");
            if (!rel || lower(*rel).find("stylesheet") == std::string::npos) return Value::null();
        } else if (!n->isHtml("style")) {
            return Value::null();
        }
        const Value w = self->wrap(n);
        const PropertyKey key(u"\u0000sheet");
        if (const js::Property* p = w.asObject()->own(key)) return p->value;
        Object* s = rt.newObject(sheet_proto);
        s->host = n;
        s->host_kind = kOwnedSheet;
        rt.put(w.asObject(), key, Value::object(s), 0);
        return Value::object(s);
    };
    for (const char* iface : {"HTMLStyleElement", "HTMLLinkElement"})
        if (Object* proto = interface_protos[iface])
            accessor(proto, u"sheet", [self, sheetOf](Runtime& rt, const Value& t, Args) { return sheetOf(rt, self->thisElement(t)); });
    accessor(proto_document, u"styleSheets", [self, sheetOf](Runtime& rt, const Value&, Args) {
        std::vector<Value> sheets;
        std::vector<Node*> todo{self->doc.root()};
        while (!todo.empty() && sheets.size() < 10000) {
            Node* n = todo.back();
            todo.pop_back();
            if (n->isHtml("style") || n->isHtml("link")) {
                const Value s = sheetOf(rt, n);
                if (s.isObject()) sheets.push_back(s);
            }
            todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        }
        Object* list = rt.newArray(std::move(sheets));
        rt.method(list, u"item", 1, [](Runtime& r, const Value& t, Args a) {
            const Value v = r.get(t, PropertyKey(js::utf16(std::to_string(static_cast<long long>(r.toIntegerOrInfinity(arg(a, 0)))))));
            return v.isUndefined() ? Value::null() : v;
        });
        return Value::object(list);
    });
    const auto adoptedAccessor = [self, resync](Object* o, bool document) {
        self->accessor(o, u"adoptedStyleSheets", [self, document](Runtime& rt, const Value& t, Args) {
            Node* n = document ? self->doc.root() : self->thisNode(t);
            for (auto& [node, arr] : self->adopters)
                if (node == n) return Value::object(arr);
            Object* arr = rt.newArray();
            self->adopters.emplace_back(n, arr);
            return Value::object(arr);
        }, [self, document, resync](Runtime& rt, const Value& t, Args a) {
            Node* n = document ? self->doc.root() : self->thisNode(t);
            Object* arr = rt.newArray(rt.iterateToList(arg(a, 0)));
            bool found = false;
            for (auto& [node, existing] : self->adopters)
                if (node == n) {
                    existing = arr;
                    found = true;
                }
            if (!found) self->adopters.emplace_back(n, arr);
            resync();
            return Value::undefined();
        });
    };
    adoptedAccessor(proto_shadow_root, false);
    adoptedAccessor(proto_document, true);
}

}  // namespace fbx::xweb
