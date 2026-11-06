#include "xweb/dom.h"

#include <algorithm>
#include <utility>

namespace fbx::xweb {

const std::string* Node::attr(std::string_view attr_name) const noexcept {
    for (const auto& a : attrs)
        if (a.name == attr_name) return &a.value;
    return nullptr;
}

void Node::append(Node* child) { insertBefore(child, nullptr); }

void Node::insertBefore(Node* child, Node* before) {
    if (child->parent) child->parent->remove(child);
    child->parent = this;
    auto at = before ? std::find(children.begin(), children.end(), before) : children.end();
    children.insert(at, child);
}

void Node::remove(Node* child) {
    auto at = std::find(children.begin(), children.end(), child);
    if (at != children.end()) children.erase(at);
    child->parent = nullptr;
}

std::string Node::textContent() const {
    std::string out;
    std::vector<const Node*> todo(children.rbegin(), children.rend());
    while (!todo.empty()) {
        const Node* n = todo.back();
        todo.pop_back();
        if (n->type == NodeType::Text) out += n->data;
        todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
    }
    return out;
}

Document::Document() {
    root_ = make(NodeType::Document);
}

Node* Document::make(NodeType type) {
    nodes_.push_back(std::make_unique<Node>());
    nodes_.back()->type = type;
    return nodes_.back().get();
}

Node* Document::documentElement() const noexcept {
    for (Node* c : root_->children)
        if (c->isElement()) return c;
    return nullptr;
}

static Node* childNamed(const Node* parent, std::string_view tag) {
    if (parent)
        for (Node* c : parent->children)
            if (c->isHtml(tag)) return c;
    return nullptr;
}

Node* Document::body() const noexcept { return childNamed(documentElement(), "body"); }
Node* Document::head() const noexcept { return childNamed(documentElement(), "head"); }

// ── Serialization ─────────────────────────────────────────────────────────────

static bool isVoid(std::string_view t) {
    static constexpr std::string_view v[] = {"area", "base", "basefont", "bgsound", "br", "col", "embed", "frame",
                                             "hr", "img", "input", "keygen", "link", "meta", "param", "source",
                                             "track", "wbr"};
    return std::find(std::begin(v), std::end(v), t) != std::end(v);
}

static bool isLiteralText(std::string_view t) {
    static constexpr std::string_view v[] = {"style", "script", "xmp", "iframe", "noembed", "noframes", "plaintext"};
    return std::find(std::begin(v), std::end(v), t) != std::end(v);
}

static void escapeInto(std::string& out, std::string_view s, bool in_attribute) {
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '&') out += "&amp;";
        else if (static_cast<unsigned char>(c) == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) {
            out += "&nbsp;";
            ++i;
        }
        else if (in_attribute && c == '"') out += "&quot;";
        else if (!in_attribute && c == '<') out += "&lt;";
        else if (!in_attribute && c == '>') out += "&gt;";
        else out += c;
    }
}

// Explicit stack: a tree can end up deeper than the open-element limit (the
// adoption agency can nest), and a page mustn't be able to overflow ours.
static void serializeInto(std::string& out, const Node& top) {
    struct Item { const Node* node; bool closing; };
    std::vector<Item> todo;
    const auto pushChildren = [&](const Node& n) {
        const Node& from = n.template_content ? *n.template_content : n;
        for (auto it = from.children.rbegin(); it != from.children.rend(); ++it) todo.push_back({*it, false});
    };
    pushChildren(top);
    while (!todo.empty()) {
        const Item item = todo.back();
        todo.pop_back();
        const Node& n = *item.node;
        if (item.closing) {
            out += "</" + n.name + ">";
            continue;
        }
        switch (n.type) {
            case NodeType::Text: {
                const Node* p = n.parent;
                if (p && p->isElement() && p->ns == Namespace::Html && isLiteralText(p->name)) out += n.data;
                else escapeInto(out, n.data, false);
                break;
            }
            case NodeType::Comment:
                out += "<!--" + n.data + "-->";
                break;
            case NodeType::Doctype:
                out += "<!DOCTYPE " + n.name + ">";
                break;
            case NodeType::Element:
                out += "<" + n.name;
                for (const auto& a : n.attrs) {
                    out += " " + a.name + "=\"";
                    escapeInto(out, a.value, true);
                    out += "\"";
                }
                out += ">";
                if (n.ns == Namespace::Html && isVoid(n.name)) break;
                todo.push_back({&n, true});
                pushChildren(n);
                break;
            default:
                break;
        }
    }
}

std::string serializeChildren(const Node& node) {
    std::string out;
    serializeInto(out, node);
    return out;
}

std::string serializeDocument(const Document& doc) {
    return serializeChildren(*doc.root());
}

}  // namespace fbx::xweb
