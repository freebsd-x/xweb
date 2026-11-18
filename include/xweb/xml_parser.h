// A small XML parser: DOMParser's application/xml, text/xml and image/svg+xml.
#pragma once

#include <memory>
#include <string_view>

#include "xweb/dom.h"

namespace fbx::xweb {

// The document `xml` is, its elements in their namespaces (Node::xml), CDATA as text.
// Not well-formed: null, as DOMParser then gives a <parsererror> document.
[[nodiscard]] std::unique_ptr<Document> parseXml(std::string_view xml);

}  // namespace fbx::xweb
