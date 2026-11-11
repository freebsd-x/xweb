// A page's scripts: our engine bound to the page's tree. window, document, Node and
// Element, events and their dispatch, timers and animation frames, as the DOM and HTML
// standards describe them, the parts pages use most first.

#include "xweb/script.h"

#include "script_impl.h"

namespace fbx::xweb {

extern const char* const kScriptPrelude;   // script_prelude.cc

namespace {

// Node's constants: node types, and compareDocumentPosition's bits.
constexpr std::pair<const char16_t*, int> kNodeConstants[] = {
    {u"ELEMENT_NODE", 1}, {u"ATTRIBUTE_NODE", 2}, {u"TEXT_NODE", 3}, {u"CDATA_SECTION_NODE", 4}, {u"ENTITY_REFERENCE_NODE", 5},
    {u"ENTITY_NODE", 6}, {u"PROCESSING_INSTRUCTION_NODE", 7}, {u"COMMENT_NODE", 8}, {u"DOCUMENT_NODE", 9}, {u"DOCUMENT_TYPE_NODE", 10},
    {u"DOCUMENT_FRAGMENT_NODE", 11}, {u"NOTATION_NODE", 12},
    {u"DOCUMENT_POSITION_DISCONNECTED", 1}, {u"DOCUMENT_POSITION_PRECEDING", 2}, {u"DOCUMENT_POSITION_FOLLOWING", 4},
    {u"DOCUMENT_POSITION_CONTAINS", 8}, {u"DOCUMENT_POSITION_CONTAINED_BY", 16}, {u"DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC", 32},
};

}  // namespace



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
    // The same kind, name, attributes (in any order), text, and children, all the way down.
    method(p, u"isEqualNode", 1, [self](Runtime&, const Value& t, Args a) {
        const Node* x = self->thisNode(t);
        const Node* y = self->node(arg(a, 0));
        if (!y) return Value::boolean(false);
        std::vector<std::pair<const Node*, const Node*>> todo{{x, y}};
        size_t seen = 0;
        while (!todo.empty()) {
            if (++seen > 1'000'000) return Value::boolean(false);
            const auto [m, n] = todo.back();
            todo.pop_back();
            if (m->type != n->type || m->ns != n->ns || m->name != n->name || m->data != n->data) return Value::boolean(false);
            if (m->attrs.size() != n->attrs.size() || m->children.size() != n->children.size()) return Value::boolean(false);
            for (const Attribute& at : m->attrs) {
                const std::string* other = n->attr(at.name);
                if (!other || *other != at.value) return Value::boolean(false);
            }
            for (size_t i = 0; i < m->children.size(); ++i) todo.emplace_back(m->children[i], n->children[i]);
        }
        return Value::boolean(true);
    });
    method(p, u"isSameNode", 1, [self](Runtime&, const Value& t, Args a) {
        return Value::boolean(self->node(arg(a, 0)) == self->thisNode(t));
    });
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
    for (auto [name, value] : kNodeConstants) rt.put(p, name, Value::number(value), 0);
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
        // A NamedNodeMap's worth: Attr-like objects by index and by name.
        Node* el = self->thisElement(t);
        std::vector<Value> out;
        for (const auto& at : el->attrs) {
            Object* o = rt.newObject();
            rt.put(o, u"name", str(at.name), js::kEnumerable);
            rt.put(o, u"localName", str(at.name), js::kEnumerable);
            rt.put(o, u"nodeName", str(at.name), js::kEnumerable);
            rt.put(o, u"value", str(at.value), js::kEnumerable | js::kWritable);
            rt.put(o, u"nodeValue", str(at.value), js::kEnumerable);
            rt.put(o, u"nodeType", Value::number(2), js::kEnumerable);
            rt.put(o, u"specified", Value::boolean(true), js::kEnumerable);
            rt.put(o, u"namespaceURI", Value::null(), js::kEnumerable);
            rt.put(o, u"ownerElement", t, js::kEnumerable);
            out.push_back(Value::object(o));
        }
        Object* map = rt.newArray(std::vector<Value>(out));
        for (size_t i = 0; i < el->attrs.size(); ++i) {
            const std::u16string name = js::utf16(el->attrs[i].name);
            if (!map->own(PropertyKey(name))) rt.put(map, name, out[i], js::kConfigurable);
        }
        rt.method(map, u"getNamedItem", 1, [](Runtime& r, const Value& m, Args a) {
            const Value v = r.get(m, PropertyKey(*r.toString(arg(a, 0))));
            return v.isObject() ? v : Value::null();
        });
        rt.method(map, u"item", 1, [](Runtime& r, const Value& m, Args a) {
            const Value v = r.get(m, PropertyKey(Runtime::numberToString(r.toIntegerOrInfinity(arg(a, 0)))));
            return v.isObject() ? v : Value::null();
        });
        return Value::object(map);
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
        Node* el = self->thisElement(t);
        c->host = el;
        c->host_kind = kClassListWrapper;
        // Its tokens by index, as they are now (it's made afresh each time it's asked for).
        std::istringstream words(self->attr(el, "class"));
        std::string word;
        std::vector<std::string> seen;
        while (words >> word && seen.size() < 10000)
            if (std::find(seen.begin(), seen.end(), word) == seen.end()) seen.push_back(word);
        for (size_t i = 0; i < seen.size(); ++i) rt.put(c, PropertyKey(js::utf16(std::to_string(i))), str(seen[i]), js::kEnumerable);
        return Value::object(c);
    });
    accessor(p, u"isContentEditable", [](Runtime&, const Value&, Args) { return Value::boolean(false); });
    accessor(p, u"tabIndex", [self](Runtime&, const Value& t, Args) {
        Node* el = self->thisElement(t);
        const std::string v = self->attr(el, "tabindex");
        if (!v.empty()) return Value::number(std::atoi(v.c_str()));
        // Controls and links are in the tab order without asking.
        const bool focusable = el->isHtml("button") || el->isHtml("input") || el->isHtml("select") || el->isHtml("textarea") ||
                               el->isHtml("iframe") || el->isHtml("summary") || ((el->isHtml("a") || el->isHtml("area")) && el->attr("href"));
        return Value::number(focusable ? 0 : -1);
    }, [self](Runtime& rt, const Value& t, Args a) {
        const double n = rt.toNumber(arg(a, 0));
        self->setAttr(self->thisElement(t), "tabindex", std::to_string(std::isfinite(n) ? static_cast<long long>(n) : 0));
        return Value::undefined();
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
    // Each answers for the document it's asked of: the page's, or one a script made
    // (document.implementation.createHTMLDocument).
    accessor(p, u"documentElement", [self](Runtime&, const Value& t, Args) { return self->wrap(documentElementOf(self->docNode(t))); });
    accessor(p, u"head", [self](Runtime&, const Value& t, Args) { return self->wrap(childElementNamed(documentElementOf(self->docNode(t)), "head")); });
    accessor(p, u"body", [self](Runtime&, const Value& t, Args) { return self->wrap(childElementNamed(documentElementOf(self->docNode(t)), "body")); });
    accessor(p, u"title", [self](Runtime&, const Value& self_doc, Args) {
        std::vector<Node*> todo{self->docNode(self_doc)};
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
    }, [self](Runtime& rt, const Value& self_doc, Args a) {
        const std::string text = js::utf8(rt.toStringValue(arg(a, 0)));
        std::vector<Node*> todo{self->docNode(self_doc)};
        while (!todo.empty()) {
            Node* n = todo.back();
            todo.pop_back();
            if (n->isHtml("title")) {
                self->setText(n, text);
                return Value::undefined();
            }
            todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        }
        if (Node* head = childElementNamed(documentElementOf(self->docNode(self_doc)), "head")) {
            Node* t = self->doc.make(NodeType::Element);
            t->name = "title";
            head->append(t);
            self->setText(t, text);
        }
        return Value::undefined();
    });
    // document.implementation: documents of a script's own, apart from the page (jQuery
    // parses HTML in one). Nothing in them is shown or run.
    {
        Object* impl = rt.newObject();
        // DOMParser's work: a whole document from text, apart from the page (its scripts
        // don't run). XML types are read as HTML, which serves the SVG and markup pages pass.
        rt.method(impl, u"__parseDocument", 1, [self](Runtime& r, const Value&, Args a) {
            const std::string text = js::utf8(r.toStringValue(arg(a, 0)));
            if (text.size() > (32u << 20)) r.throwError("RangeError", "too much text to parse");
            const auto parsed = parseHtml(text);
            Node* d = self->doc.make(NodeType::Document);
            for (const Node* c : parsed->root()->children) d->append(self->clone(c, true));
            return self->wrap(d);
        });
        rt.method(impl, u"createHTMLDocument", 0, [self](Runtime& r, const Value&, Args a) {
            Node* d = self->doc.make(NodeType::Document);
            const auto element = [&](const char* tag, Node* parent) {
                Node* e = self->doc.make(NodeType::Element);
                e->ns = Namespace::Html;
                e->name = tag;
                parent->append(e);
                return e;
            };
            Node* dt = self->doc.make(NodeType::Doctype);
            dt->name = "html";
            d->append(dt);
            Node* html = element("html", d);
            Node* head = element("head", html);
            if (!arg(a, 0).isUndefined()) {
                Node* title = element("title", head);
                Node* text = self->doc.make(NodeType::Text);
                text->data = js::utf8(r.toStringValue(arg(a, 0)));
                title->append(text);
            }
            element("body", html);
            return self->wrap(d);
        });
        rt.method(impl, u"createDocument", 0, [self](Runtime&, const Value&, Args) {
            return self->wrap(self->doc.make(NodeType::Document));
        });
        rt.method(impl, u"hasFeature", 0, [](Runtime&, const Value&, Args) { return Value::boolean(true); });
        rt.put(p, u"implementation", Value::object(impl), js::kConfigurable);
    }
    // At a point of the window: the page's hit test, offset by how far it's scrolled.
    const auto at_point = [self](Runtime& rt, Args a) -> Node* {
        if (!self->host.element_at) return nullptr;
        const double x = rt.toNumber(arg(a, 0)), y = rt.toNumber(arg(a, 1));
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0) return nullptr;
        if (x > self->host.viewport_w || y > self->host.viewport_h) return nullptr;
        const float sy = self->host.scroll_y ? self->host.scroll_y() : 0;
        return self->host.element_at(static_cast<float>(x), static_cast<float>(y) + sy);
    };
    method(p, u"elementFromPoint", 2, [self, at_point](Runtime& rt, const Value&, Args a) { return self->wrap(at_point(rt, a)); });
    method(p, u"elementsFromPoint", 2, [self, at_point](Runtime& rt, const Value&, Args a) {
        std::vector<Node*> out;
        for (Node* n = at_point(rt, a); n; n = n->parent)
            if (n->isElement()) out.push_back(n);
        return self->list(out);
    });
    method(p, u"getElementById", 1, [self](Runtime& rt, const Value& self_doc, Args a) {
        const std::string id = js::utf8(rt.toStringValue(arg(a, 0)));
        std::vector<Node*> todo{self->docNode(self_doc)};
        while (!todo.empty()) {
            Node* n = todo.back();
            todo.pop_back();
            if (n->isElement())
                if (const std::string* v = n->attr("id"); v && *v == id) return self->wrap(n);
            todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        }
        return Value::null();
    });
    method(p, u"getElementsByName", 1, [self](Runtime& rt, const Value& self_doc, Args a) {
        const std::string want = js::utf8(rt.toStringValue(arg(a, 0)));
        std::vector<Node*> out;
        std::vector<Node*> todo{self->docNode(self_doc)};
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
        // A defined custom element is made by its class, at once; `is` asks for a
        // customized built-in.
        std::string is;
        if (arg(a, 1).isObject()) {
            const Value v = rt.get(arg(a, 1), u"is");
            if (!v.isUndefined()) is = js::utf8(rt.toStringValue(v));
        }
        const auto def = self->custom_definitions.find(is.empty() ? tag : is);
        if (def != self->custom_definitions.end() && def->second.local_name == tag) {
            const Value made = rt.construct(Value::object(def->second.ctor), {});
            Node* n = self->node(made);
            if (!n || !n->isElement()) rt.throwError("TypeError", "the custom element's constructor didn't make an element");
            return made;
        }
        Node* n = self->doc.make(NodeType::Element);
        n->name = tag;
        if (!is.empty()) n->attrs.push_back({"is", is});
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
        // One still waiting in the round that's running now is skipped too.
        for (auto& [fid, fn] : self->running_frames)
            if (fid == id) fn = Value::undefined();
        return Value::undefined();
    });
    method(g, u"requestIdleCallback", 1, addTimer(false));
    method(g, u"cancelIdleCallback", 1, clearTimer);
    // The browser's side of fetch() and XMLHttpRequest (script_fetch.cc), which the
    // prelude builds them on.
    method(g, u"__xwebFetch", 6, [self](Runtime&, const Value&, Args a) { return self->browserFetch(a); });
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
    for (const char16_t* name : {u"Event", u"CustomEvent", u"MouseEvent", u"KeyboardEvent", u"FocusEvent", u"InputEvent", u"UIEvent", u"PointerEvent",
                                 u"PromiseRejectionEvent", u"ErrorEvent", u"MessageEvent", u"ProgressEvent", u"PopStateEvent", u"HashChangeEvent",
                                 u"StorageEvent", u"TransitionEvent", u"AnimationEvent", u"WheelEvent", u"TouchEvent", u"SubmitEvent"}) {
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
        if (!proto->own(PropertyKey(rt.sym_to_string_tag))) rt.put(proto, PropertyKey(rt.sym_to_string_tag), Value::string(name), js::kConfigurable);
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
    // NodeList and HTMLCollection: our lists are arrays, so their prototype inherits
    // Array's; pages test instanceof and add forEach to it.
    proto_nodelist = rt.newObject(rt.array_prototype);
    method(proto_nodelist, u"item", 1, [](Runtime& r, const Value& t, Args a) {
        const Value v = r.get(t, PropertyKey(Runtime::numberToString(r.toIntegerOrInfinity(arg(a, 0)))));
        return v.isUndefined() ? Value::null() : v;
    });
    // HTMLCollection's lookup by id, then by name (on NodeLists too: they share a prototype).
    method(proto_nodelist, u"namedItem", 1, [self](Runtime& r, const Value& t, Args a) {
        const std::string key = js::utf8(r.toStringValue(arg(a, 0)));
        if (key.empty()) return Value::null();
        const double n = r.toNumber(r.get(t, u"length"));
        Value by_name = Value::null();
        for (double i = 0; i < n && i < 1e7; ++i) {
            const Value v = r.get(t, PropertyKey(Runtime::numberToString(i)));
            Node* e = self->node(v);
            if (!e || !e->isElement()) continue;
            if (const std::string* id = e->attr("id"); id && *id == key) return v;
            if (by_name.isNull())
                if (const std::string* nm = e->attr("name"); nm && *nm == key) by_name = v;
        }
        return by_name;
    });
    iface(u"NodeList", proto_nodelist);
    iface(u"HTMLCollection", proto_nodelist);
    iface(u"Node", proto_node);
    iface(u"Element", proto_element);
    iface(u"Document", proto_document);
    iface(u"HTMLDocument", proto_document);
    iface(u"Text", proto_text);
    iface(u"CharacterData", proto_text);
    iface(u"CSSStyleDeclaration", proto_style);
    iface(u"DOMTokenList", proto_classlist);
    // Node's constants on the interface too (ShadyDOM reads Node.DOCUMENT_FRAGMENT_NODE
    // for its roots' nodeType).
    for (auto [name, value] : kNodeConstants) rt.put(rt.get(Value::object(g), u"Node").asObject(), name, Value::number(value), 0);
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
    // "unhandledrejection" at the window; preventDefault() means it's been seen to.
    rt.unhandled_rejection = [this](const Value& promise, const Value& reason) {
        return !fire(nullptr, u"unhandledrejection", false, true, [&](Object* e) {
            rt.put(e, u"promise", promise, js::kEnumerable);
            rt.put(e, u"reason", reason, js::kEnumerable);
        });
    };
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
    installDom();
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
    // XWEB_PROBE=<expression>: for finding what a page is up to, its value 15 seconds on,
    // as a console line.
    if (const char* probe = std::getenv("XWEB_PROBE")) {
        std::string quoted = "\"";
        for (const char c : std::string(probe)) {
            if (c == '"' || c == '\\') quoted += '\\';
            if (c == '\n') { quoted += "\\n"; continue; }
            quoted += c;
        }
        quoted += "\"";
        (void)s.rt.evaluate("setTimeout(function () { try { console.log('probe: ' + String((0, eval)(" + quoted +
                                "))); } catch (e) { console.log('probe threw: ' + e); } }, 15000);",
                            "probe");
    }
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

void PageScript::imageLoaded(const std::string& url, bool ok) {
    Impl& s = *impl_;
    std::vector<Node*> imgs, todo{s.doc.root()};
    while (!todo.empty()) {
        Node* n = todo.back();
        todo.pop_back();
        if (n->isHtml("img"))
            if (const std::string* src = n->attr("src"); src && resolveUrl(s.host.base, *src) == url) imgs.push_back(n);
        todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
        if (n->shadow_root) todo.push_back(n->shadow_root);
    }
    for (Node* n : imgs) s.fire(n, ok ? u"load" : u"error", false, false);
    s.drainMicrotasks();
}

void PageScript::toggle(Node* details) {
    Impl& s = *impl_;
    if (!details) return;
    if (details->attr("open")) s.removeAttr(details, "open");
    else s.setAttr(details, "open", "");
    s.fire(details, u"toggle", false, false);
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
        s.running_frames.clear();
        s.running_frames.swap(s.frames);
        const Value stamp = Value::number(now);
        for (size_t i = 0; i < s.running_frames.size(); ++i) {
            const Value fn = s.running_frames[i].second;
            if (!s.rt.isCallable(fn)) continue;   // cancelled during this round
            s.task([&] { s.rt.call(fn, Value::undefined(), std::span<const Value>(&stamp, 1)); });
        }
        s.running_frames.clear();
        s.drainMicrotasks();
    }
    s.collect();
}

bool PageScript::busy() const { return !impl_->timers.empty() || !impl_->frames.empty(); }

bool PageScript::dirty() const { return impl_->dirty; }

bool PageScript::takeDirty() {
    const bool d = impl_->dirty;
    impl_->dirty = false;
    return d;
}

}  // namespace fbx::xweb
