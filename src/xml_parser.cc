// A small XML parser (see xml_parser.h): enough of XML 1.0 and Namespaces in XML for the
// documents pages parse themselves (OpenLayers' WMTS capabilities, RSS, SVG).

#include "xweb/xml_parser.h"

#include <map>
#include <vector>

namespace fbx::xweb {

namespace {

constexpr size_t kMaxDepth = 4096;

class XmlParser {
public:
    XmlParser(std::string_view s, Document& doc) : s_(s), doc_(doc) {}

    bool parse() {
        scopes_.push_back({{"xml", "http://www.w3.org/XML/1998/namespace"}});
        std::vector<Node*> open{doc_.root()};
        bool root_seen = false;
        while (i_ < s_.size()) {
            if (s_[i_] != '<') {
                const size_t start = i_;
                while (i_ < s_.size() && s_[i_] != '<') ++i_;
                std::string text;
                if (!decode(s_.substr(start, i_ - start), text)) return false;
                if (open.size() > 1) addText(open.back(), text);
                else if (text.find_first_not_of(" \t\r\n") != std::string::npos) return false;   // text outside the root
                continue;
            }
            if (starts("<?")) {
                const size_t end = s_.find("?>", i_);
                if (end == std::string_view::npos) return false;
                i_ = end + 2;
            } else if (starts("<!--")) {
                const size_t end = s_.find("-->", i_ + 4);
                if (end == std::string_view::npos) return false;
                Node* c = doc_.make(NodeType::Comment);
                c->data = std::string(s_.substr(i_ + 4, end - i_ - 4));
                open.back()->append(c);
                i_ = end + 3;
            } else if (starts("<![CDATA[")) {
                const size_t end = s_.find("]]>", i_ + 9);
                if (end == std::string_view::npos || open.size() < 2) return false;
                addText(open.back(), std::string(s_.substr(i_ + 9, end - i_ - 9)));
                i_ = end + 3;
            } else if (starts("<!DOCTYPE") || starts("<!doctype")) {
                // Skipped, internal subset and all.
                int depth = 0;
                for (; i_ < s_.size(); ++i_) {
                    if (s_[i_] == '[') ++depth;
                    else if (s_[i_] == ']') --depth;
                    else if (s_[i_] == '>' && depth <= 0) break;
                }
                ++i_;
            } else if (starts("</")) {
                i_ += 2;
                const std::string name = readName();
                skipSpace();
                if (i_ >= s_.size() || s_[i_] != '>' || open.size() < 2 || open.back()->name != name) return false;
                ++i_;
                open.pop_back();
                scopes_.pop_back();
            } else {
                ++i_;
                if (open.size() == 1 && root_seen) return false;   // a second root
                if (open.size() > kMaxDepth) return false;
                Node* e = doc_.make(NodeType::Element);
                e->name = readName();
                if (e->name.empty()) return false;
                std::map<std::string, std::string> declared;
                bool self_closing = false;
                while (true) {
                    skipSpace();
                    if (i_ >= s_.size()) return false;
                    if (s_[i_] == '>') { ++i_; break; }
                    if (starts("/>")) { i_ += 2; self_closing = true; break; }
                    Attribute a;
                    a.name = readName();
                    if (a.name.empty()) return false;
                    skipSpace();
                    if (i_ >= s_.size() || s_[i_] != '=') return false;
                    ++i_;
                    skipSpace();
                    if (i_ >= s_.size() || (s_[i_] != '"' && s_[i_] != '\'')) return false;
                    const char q = s_[i_++];
                    const size_t end = s_.find(q, i_);
                    if (end == std::string_view::npos) return false;
                    if (!decode(s_.substr(i_, end - i_), a.value)) return false;
                    i_ = end + 1;
                    if (a.name == "xmlns") declared[""] = a.value;
                    else if (a.name.starts_with("xmlns:")) declared[a.name.substr(6)] = a.value;
                    e->attrs.push_back(std::move(a));
                }
                scopes_.push_back(std::move(declared));
                auto x = std::make_shared<XmlName>();
                const size_t colon = e->name.find(':');
                x->prefix = colon == std::string::npos ? std::string() : e->name.substr(0, colon);
                x->local = colon == std::string::npos ? e->name : e->name.substr(colon + 1);
                x->uri = lookup(x->prefix);
                e->ns = x->uri == "http://www.w3.org/1999/xhtml" ? Namespace::Html
                        : x->uri == "http://www.w3.org/2000/svg" ? Namespace::Svg
                        : x->uri == "http://www.w3.org/1998/Math/MathML" ? Namespace::MathMl
                                                                          : Namespace::Other;
                e->xml = std::move(x);
                open.back()->append(e);
                if (open.size() == 1) root_seen = true;
                if (self_closing) scopes_.pop_back();
                else open.push_back(e);
            }
        }
        return open.size() == 1 && root_seen;
    }

private:
    bool starts(std::string_view p) const { return s_.substr(i_, p.size()) == p; }
    void skipSpace() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) ++i_;
    }
    std::string readName() {
        const size_t start = i_;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/' || c == '=' || c == '<' || c == '"' || c == '\'') break;
            ++i_;
        }
        return std::string(s_.substr(start, i_ - start));
    }
    std::string lookup(const std::string& prefix) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
            if (const auto f = it->find(prefix); f != it->end()) return f->second;
        return {};
    }
    void addText(Node* parent, std::string text) {
        if (text.empty()) return;
        if (!parent->children.empty() && parent->children.back()->type == NodeType::Text) {
            parent->children.back()->data += text;
            return;
        }
        Node* t = doc_.make(NodeType::Text);
        t->data = std::move(text);
        parent->append(t);
    }
    static void utf8(uint32_t cp, std::string& out) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    // Character and the five predefined entity references.
    static bool decode(std::string_view in, std::string& out) {
        out.reserve(out.size() + in.size());
        for (size_t k = 0; k < in.size(); ++k) {
            if (in[k] != '&') { out += in[k]; continue; }
            const size_t semi = in.find(';', k);
            if (semi == std::string_view::npos) return false;
            const std::string_view ref = in.substr(k + 1, semi - k - 1);
            if (ref == "lt") out += '<';
            else if (ref == "gt") out += '>';
            else if (ref == "amp") out += '&';
            else if (ref == "quot") out += '"';
            else if (ref == "apos") out += '\'';
            else if (ref.size() > 1 && ref[0] == '#') {
                const bool hex = ref[1] == 'x' || ref[1] == 'X';
                uint32_t cp = 0;
                const std::string_view digits = ref.substr(hex ? 2 : 1);
                if (digits.empty() || digits.size() > 8) return false;
                for (const char c : digits) {
                    const int d = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                    if (d < 0) return false;
                    cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(d);
                }
                if (cp == 0 || cp > 0x10FFFF) return false;
                utf8(cp, out);
            } else {
                return false;   // an entity a DTD would have defined
            }
            k = semi;
        }
        return true;
    }

    std::string_view s_;
    Document& doc_;
    size_t i_ = 0;
    std::vector<std::map<std::string, std::string>> scopes_;
};

}  // namespace

std::unique_ptr<Document> parseXml(std::string_view xml) {
    auto doc = std::make_unique<Document>();
    if (xml.starts_with("\xEF\xBB\xBF")) xml.remove_prefix(3);
    XmlParser p(xml, *doc);
    if (!p.parse()) return nullptr;
    return doc;
}

}  // namespace fbx::xweb
