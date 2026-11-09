#pragma once

// The document tree. A Document owns every node it ever made, so the parser can
// move nodes around (it has to) and nothing dangles or recurses on destruction.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fbx::xweb {

enum class NodeType { Document, Doctype, Element, Text, Comment, Fragment };
enum class Namespace { Html, Svg, MathMl };

struct Attribute {
    std::string name;
    std::string value;
};

class Node {
public:
    NodeType type = NodeType::Element;
    Namespace ns = Namespace::Html;
    std::string name;        // tag name (lower case in HTML), or the doctype's name
    std::string data;        // text or comment contents
    std::string public_id;   // doctype only
    std::string system_id;   // doctype only
    std::vector<Attribute> attrs;
    Node* parent = nullptr;
    std::vector<Node*> children;
    Node* template_content = nullptr;   // <template>'s children live here
    // Shadow DOM: a host's shadow root (a Fragment), and a shadow root's host.
    Node* shadow_root = nullptr;
    Node* shadow_host = nullptr;
    bool shadow_open = true;            // a shadow root's mode
    std::vector<std::string> adopted_css;   // a shadow root's adoptedStyleSheets, as text
    // A custom element's state: 0 none (or not yet looked at), 1 undefined (a custom
    // element name waiting for its definition), 2 custom (upgraded), 3 failed.
    uint8_t custom_state = 0;

    [[nodiscard]] bool isElement() const noexcept { return type == NodeType::Element; }
    [[nodiscard]] bool isHtml(std::string_view tag) const noexcept {
        return type == NodeType::Element && ns == Namespace::Html && name == tag;
    }
    [[nodiscard]] const std::string* attr(std::string_view attr_name) const noexcept;

    void append(Node* child);
    void insertBefore(Node* child, Node* before);   // before == nullptr appends
    void remove(Node* child);

    // All descendant text, in order. Doesn't recurse, so depth can't overflow it.
    [[nodiscard]] std::string textContent() const;

    // The shadow root this node's tree hangs from, or null if it's in the document's tree.
    [[nodiscard]] const Node* containingShadowRoot() const noexcept;
};

// The children a node has in the composed tree, the one that's rendered: a shadow host's
// are its shadow root's children; a <slot>'s are the host's children assigned to it (or
// its own, when none are); anyone else's are its own.
void composedChildren(const Node* n, std::vector<const Node*>& out);
// What a <slot> in a shadow tree has assigned: its host's children that name it (text
// and unnamed elements go to the unnamed slot), each to the first such slot.
void assignedNodes(const Node* slot, std::vector<const Node*>& out);
// The <details> a click on `target` opens or closes: the one whose summary it's in,
// unless a link or control nearer it takes the click. Null if none.
[[nodiscard]] Node* detailsToggledBy(Node* target) noexcept;
// <template shadowrootmode=open|closed> in a parsed document: its content becomes its
// parent's shadow root, as declarative shadow DOM has it.
void attachDeclarativeShadowRoots(class Document& doc);

class Document {
public:
    Document();
    [[nodiscard]] Node* root() const noexcept { return root_; }
    [[nodiscard]] Node* make(NodeType type);
    [[nodiscard]] Node* documentElement() const noexcept;
    [[nodiscard]] Node* body() const noexcept;
    [[nodiscard]] Node* head() const noexcept;
    bool quirks = false;

private:
    std::vector<std::unique_ptr<Node>> nodes_;
    Node* root_;
};

// HTML's serialization of a node's children (element.innerHTML).
[[nodiscard]] std::string serializeChildren(const Node& node);
// The whole document, as a browser would print it: doctype, then <html>.
[[nodiscard]] std::string serializeDocument(const Document& doc);

}  // namespace fbx::xweb
