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
};

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
