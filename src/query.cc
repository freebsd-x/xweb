// Selector matching for the DOM's methods, walking siblings as it goes rather than
// from the cascade's prepared index.

#include "xweb/query.h"

#include <algorithm>
#include <cctype>

namespace fbx::xweb {

namespace {

using css::AttrOp;
using css::Combinator;
using css::ComplexSelector;
using css::CompoundSelector;
using css::SimpleKind;
using css::SimpleSelector;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool hasWord(const std::string& list, const std::string& word) {
    size_t i = 0;
    while (i < list.size()) {
        while (i < list.size() && std::isspace(static_cast<unsigned char>(list[i]))) ++i;
        const size_t b = i;
        while (i < list.size() && !std::isspace(static_cast<unsigned char>(list[i]))) ++i;
        if (i > b && list.compare(b, i - b, word) == 0 && i - b == word.size()) return true;
    }
    return false;
}

const Node* prevElement(const Node* el) {
    if (!el->parent) return nullptr;
    const Node* prev = nullptr;
    for (const Node* c : el->parent->children) {
        if (c == el) return prev;
        if (c->isElement()) prev = c;
    }
    return nullptr;
}

struct Position {
    int index = 0, count = 0, type_index = 0, type_count = 0;
};

Position positionOf(const Node* el) {
    Position p;
    if (!el->parent) return p;
    for (const Node* c : el->parent->children) {
        if (!c->isElement()) continue;
        ++p.count;
        if (c->name == el->name) ++p.type_count;
        if (c == el) {
            p.index = p.count;
            p.type_index = p.type_count;
        }
    }
    return p;
}

bool nth(int a, int b, int index) {
    if (a == 0) return index == b;
    const int diff = index - b;
    return diff % a == 0 && diff / a >= 0;
}

class Matcher {
public:
    bool complex(const Node* el, const ComplexSelector& sel) { return !sel.parts.empty() && from(el, sel, sel.parts.size() - 1); }

private:
    bool simple(const Node* el, const SimpleSelector& s) {
        switch (s.kind) {
            case SimpleKind::Universal: return true;
            case SimpleKind::Type: return el->name == s.name || (el->ns != Namespace::Html && lower(el->name) == s.name);
            case SimpleKind::Id: { const std::string* v = el->attr("id"); return v && *v == s.name; }
            case SimpleKind::Class: { const std::string* v = el->attr("class"); return v && hasWord(*v, s.name); }
            case SimpleKind::Attribute: {
                const std::string* found = el->attr(s.name);
                if (!found) return false;
                if (s.op == AttrOp::Exists) return true;
                std::string have = *found, want = s.value;
                if (s.case_insensitive) { have = lower(have); want = lower(want); }
                switch (s.op) {
                    case AttrOp::Equals: return have == want;
                    case AttrOp::Includes: return !want.empty() && hasWord(have, want);
                    case AttrOp::DashMatch: return have == want || have.starts_with(want + "-");
                    case AttrOp::Prefix: return !want.empty() && have.starts_with(want);
                    case AttrOp::Suffix: return !want.empty() && have.ends_with(want);
                    case AttrOp::Substring: return !want.empty() && have.find(want) != std::string::npos;
                    default: return true;
                }
            }
            case SimpleKind::PseudoClass: return pseudo(el, s);
            case SimpleKind::PseudoElement: return false;
        }
        return false;
    }

    bool pseudo(const Node* el, const SimpleSelector& s) {
        const std::string& n = s.name;
        if (n == "not") { for (const auto& a : s.args) if (complex(el, a)) return false; return true; }
        if (n == "is" || n == "where" || n == "matches") { for (const auto& a : s.args) if (complex(el, a)) return true; return false; }
        if (n == css::kHasAnchor) return el == has_anchor_;
        if (n == "has") {
            // Each selector relative to this element: looked for among its descendants, or
            // (led by + or ~) its later siblings and theirs.
            const Node* saved = has_anchor_;
            has_anchor_ = el;
            bool hit = false;
            for (const auto& a : s.args) {
                if (a.parts.size() < 2 || hit) continue;
                const bool siblings = a.parts[1].combinator == Combinator::NextSibling || a.parts[1].combinator == Combinator::SubsequentSibling;
                std::vector<const Node*> todo;
                if (siblings) {
                    if (el->parent) {
                        bool after = false;
                        for (const Node* c : el->parent->children) {
                            if (after) todo.push_back(c);
                            if (c == el) after = true;
                        }
                    }
                } else {
                    todo.assign(el->children.begin(), el->children.end());
                }
                while (!todo.empty() && !hit && fuel_ > 0) {
                    const Node* c = todo.back();
                    todo.pop_back();
                    if (c->isElement() && complex(c, a)) hit = true;
                    todo.insert(todo.end(), c->children.begin(), c->children.end());
                }
            }
            has_anchor_ = saved;
            return hit;
        }
        if (n == "root") return el->parent && el->parent->type == NodeType::Document;
        if (n == "defined") return !(el->ns == Namespace::Html && el->name.find('-') != std::string::npos && el->custom_state != 2);
        if (n == "host") return false;   // from outside a shadow tree, nothing is its host
        if (n == "scope") return false;
        const Position p = positionOf(el);
        if (n == "first-child") return p.index == 1;
        if (n == "last-child") return p.index == p.count;
        if (n == "only-child") return p.count == 1;
        if (n == "nth-child") return nth(s.a, s.b, p.index);
        if (n == "nth-last-child") return nth(s.a, s.b, p.count - p.index + 1);
        if (n == "first-of-type") return p.type_index == 1;
        if (n == "last-of-type") return p.type_index == p.type_count;
        if (n == "only-of-type") return p.type_count == 1;
        if (n == "nth-of-type") return nth(s.a, s.b, p.type_index);
        if (n == "nth-last-of-type") return nth(s.a, s.b, p.type_count - p.type_index + 1);
        if (n == "empty") return std::none_of(el->children.begin(), el->children.end(), [](const Node* c) { return c->isElement() || (c->type == NodeType::Text && !c->data.empty()); });
        if (n == "link" || n == "any-link") return (el->isHtml("a") || el->isHtml("area")) && el->attr("href");
        if (n == "checked") return el->attr("checked") || el->attr("selected");
        if (n == "disabled") return el->attr("disabled") != nullptr;
        if (n == "enabled") return (el->isHtml("input") || el->isHtml("button") || el->isHtml("select") || el->isHtml("textarea")) && !el->attr("disabled");
        return false;
    }

    bool compound(const Node* el, const CompoundSelector& c) {
        if (--fuel_ < 0) return false;
        for (const SimpleSelector& s : c.simple)
            if (!simple(el, s)) return false;
        return true;
    }

    bool from(const Node* el, const ComplexSelector& sel, size_t part) {
        if (!compound(el, sel.parts[part])) return false;
        if (part == 0) return true;
        switch (sel.parts[part].combinator) {
            case Combinator::Descendant:
                for (const Node* p = el->parent; p && p->isElement(); p = p->parent)
                    if (from(p, sel, part - 1)) return true;
                return false;
            case Combinator::Child: return el->parent && el->parent->isElement() && from(el->parent, sel, part - 1);
            case Combinator::NextSibling: { const Node* p = prevElement(el); return p && from(p, sel, part - 1); }
            case Combinator::SubsequentSibling:
                for (const Node* p = prevElement(el); p; p = prevElement(p))
                    if (from(p, sel, part - 1)) return true;
                return false;
        }
        return false;
    }

    long fuel_ = 1'000'000;   // a selector can't make one match take forever
    const Node* has_anchor_ = nullptr;   // the element a :has() is being asked about
};

}  // namespace

std::optional<std::vector<css::ComplexSelector>> parseSelectors(std::string_view text) {
    std::vector<css::Token> tokens = css::tokenize(text);
    while (!tokens.empty() && (tokens.back().type == css::TokenType::Eof || tokens.back().type == css::TokenType::Whitespace)) tokens.pop_back();
    std::vector<css::ComplexSelector> out;
    if (tokens.empty() || !css::parseSelectorList(tokens, out)) return std::nullopt;
    return out;
}

bool matchesSelectors(const Node* el, const std::vector<css::ComplexSelector>& selectors) {
    if (!el || !el->isElement()) return false;
    Matcher m;
    for (const auto& s : selectors)
        if (m.complex(el, s)) return true;
    return false;
}

std::vector<Node*> querySelectorAll(Node* root, const std::vector<css::ComplexSelector>& selectors, bool first) {
    std::vector<Node*> out;
    std::vector<Node*> todo(root->children.rbegin(), root->children.rend());
    while (!todo.empty()) {
        Node* n = todo.back();
        todo.pop_back();
        if (n->isElement() && matchesSelectors(n, selectors)) {
            out.push_back(n);
            if (first) break;
        }
        todo.insert(todo.end(), n->children.rbegin(), n->children.rend());
    }
    return out;
}

}  // namespace fbx::xweb
