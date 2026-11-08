// HTML tree construction (WHATWG 13.2.6), written from the spec: insertion modes,
// the stack of open elements, active formatting elements with the adoption agency,
// foster parenting, templates and foreign content. Parse errors aren't reported:
// each has a defined recovery, and that's what runs.

#include "xweb/html_parser.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "html_tokenizer.h"

namespace fbx::xweb {

namespace {

using detail::Token;
using detail::Tokenizer;
using detail::TokenizerState;
using detail::TokenType;
using Names = std::initializer_list<std::string_view>;

bool in(std::string_view s, Names names) {
    return std::find(names.begin(), names.end(), s) != names.end();
}

bool isWsChar(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }
bool allWs(std::string_view s) { return std::all_of(s.begin(), s.end(), isWsChar); }

bool isSpecial(const Node* n) {
    if (n->ns == Namespace::Html)
        return in(n->name, {"address", "applet", "area", "article", "aside", "base", "basefont", "bgsound", "blockquote",
                            "body", "br", "button", "caption", "center", "col", "colgroup", "dd", "details", "dir",
                            "div", "dl", "dt", "embed", "fieldset", "figcaption", "figure", "footer", "form", "frame",
                            "frameset", "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hgroup", "hr", "html",
                            "iframe", "img", "input", "keygen", "li", "link", "listing", "main", "marquee", "menu",
                            "meta", "nav", "noembed", "noframes", "noscript", "object", "ol", "p", "param", "plaintext",
                            "pre", "script", "search", "section", "select", "source", "style", "summary", "table",
                            "tbody", "td", "template", "textarea", "tfoot", "th", "thead", "title", "tr", "track", "ul",
                            "wbr", "xmp"});
    if (n->ns == Namespace::MathMl) return in(n->name, {"mi", "mo", "mn", "ms", "mtext", "annotation-xml"});
    return in(n->name, {"foreignObject", "desc", "title"});
}

bool isMathTextPoint(const Node* n) {
    return n->ns == Namespace::MathMl && in(n->name, {"mi", "mo", "mn", "ms", "mtext"});
}

bool isHtmlIntegrationPoint(const Node* n) {
    if (n->ns == Namespace::Svg) return in(n->name, {"foreignObject", "desc", "title"});
    if (n->ns == Namespace::MathMl && n->name == "annotation-xml") {
        const std::string* enc = n->attr("encoding");
        if (!enc) return false;
        std::string lower = *enc;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
        return lower == "text/html" || lower == "application/xhtml+xml";
    }
    return false;
}

constexpr std::string_view kSvgTags[][2] = {
    {"altglyph", "altGlyph"}, {"altglyphdef", "altGlyphDef"}, {"altglyphitem", "altGlyphItem"},
    {"animatecolor", "animateColor"}, {"animatemotion", "animateMotion"}, {"animatetransform", "animateTransform"},
    {"clippath", "clipPath"}, {"feblend", "feBlend"}, {"fecolormatrix", "feColorMatrix"},
    {"fecomponenttransfer", "feComponentTransfer"}, {"fecomposite", "feComposite"},
    {"feconvolvematrix", "feConvolveMatrix"}, {"fediffuselighting", "feDiffuseLighting"},
    {"fedisplacementmap", "feDisplacementMap"}, {"fedistantlight", "feDistantLight"}, {"fedropshadow", "feDropShadow"},
    {"feflood", "feFlood"}, {"fefunca", "feFuncA"}, {"fefuncb", "feFuncB"}, {"fefuncg", "feFuncG"},
    {"fefuncr", "feFuncR"}, {"fegaussianblur", "feGaussianBlur"}, {"feimage", "feImage"}, {"femerge", "feMerge"},
    {"femergenode", "feMergeNode"}, {"femorphology", "feMorphology"}, {"feoffset", "feOffset"},
    {"fepointlight", "fePointLight"}, {"fespecularlighting", "feSpecularLighting"}, {"fespotlight", "feSpotLight"},
    {"fetile", "feTile"}, {"feturbulence", "feTurbulence"}, {"foreignobject", "foreignObject"},
    {"glyphref", "glyphRef"}, {"lineargradient", "linearGradient"}, {"radialgradient", "radialGradient"},
    {"textpath", "textPath"},
};

constexpr std::string_view kSvgAttrs[][2] = {
    {"attributename", "attributeName"}, {"attributetype", "attributeType"}, {"basefrequency", "baseFrequency"},
    {"baseprofile", "baseProfile"}, {"calcmode", "calcMode"}, {"clippathunits", "clipPathUnits"},
    {"diffuseconstant", "diffuseConstant"}, {"edgemode", "edgeMode"}, {"filterunits", "filterUnits"},
    {"glyphref", "glyphRef"}, {"gradienttransform", "gradientTransform"}, {"gradientunits", "gradientUnits"},
    {"kernelmatrix", "kernelMatrix"}, {"kernelunitlength", "kernelUnitLength"}, {"keypoints", "keyPoints"},
    {"keysplines", "keySplines"}, {"keytimes", "keyTimes"}, {"lengthadjust", "lengthAdjust"},
    {"limitingconeangle", "limitingConeAngle"}, {"markerheight", "markerHeight"}, {"markerunits", "markerUnits"},
    {"markerwidth", "markerWidth"}, {"maskcontentunits", "maskContentUnits"}, {"maskunits", "maskUnits"},
    {"numoctaves", "numOctaves"}, {"pathlength", "pathLength"}, {"patterncontentunits", "patternContentUnits"},
    {"patterntransform", "patternTransform"}, {"patternunits", "patternUnits"}, {"pointsatx", "pointsAtX"},
    {"pointsaty", "pointsAtY"}, {"pointsatz", "pointsAtZ"}, {"preservealpha", "preserveAlpha"},
    {"preserveaspectratio", "preserveAspectRatio"}, {"primitiveunits", "primitiveUnits"}, {"refx", "refX"},
    {"refy", "refY"}, {"repeatcount", "repeatCount"}, {"repeatdur", "repeatDur"},
    {"requiredextensions", "requiredExtensions"}, {"requiredfeatures", "requiredFeatures"},
    {"specularconstant", "specularConstant"}, {"specularexponent", "specularExponent"},
    {"spreadmethod", "spreadMethod"}, {"startoffset", "startOffset"}, {"stddeviation", "stdDeviation"},
    {"stitchtiles", "stitchTiles"}, {"surfacescale", "surfaceScale"}, {"systemlanguage", "systemLanguage"},
    {"tablevalues", "tableValues"}, {"targetx", "targetX"}, {"targety", "targetY"}, {"textlength", "textLength"},
    {"viewbox", "viewBox"}, {"viewtarget", "viewTarget"}, {"xchannelselector", "xChannelSelector"},
    {"ychannelselector", "yChannelSelector"}, {"zoomandpan", "zoomAndPan"},
};

// Foreign attributes keep their prefix in the serialized name, so nothing to do
// for them here beyond the SVG case fixes.
void adjustSvgAttrs(std::vector<Attribute>& attrs) {
    for (auto& a : attrs)
        for (const auto& row : kSvgAttrs)
            if (a.name == row[0]) {
                a.name = std::string(row[1]);
                break;
            }
}

void adjustMathAttrs(std::vector<Attribute>& attrs) {
    for (auto& a : attrs)
        if (a.name == "definitionurl") a.name = "definitionURL";
}

bool quirksPublicId(std::string pub) {
    std::transform(pub.begin(), pub.end(), pub.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
    static constexpr std::string_view exact[] = {"-//w3o//dtd w3 html strict 3.0//en//", "-/w3c/dtd html 4.0 transitional/en", "html"};
    if (std::find(std::begin(exact), std::end(exact), pub) != std::end(exact)) return true;
    static constexpr std::string_view prefix[] = {
        "+//silmaril//dtd html pro v0r11 19970101//", "-//as//dtd html 3.0 aswedit + extensions//",
        "-//advasoft ltd//dtd html 3.0 aswedit + extensions//", "-//ietf//dtd html", "-//metrius//dtd metrius presentational//",
        "-//microsoft//dtd internet explorer", "-//netscape comm. corp.//dtd", "-//o'reilly and associates//dtd html",
        "-//spyglass//dtd html 2.0 extended//", "-//sq//dtd html 2.0 hotmetal + extensions//",
        "-//sun microsystems corp.//dtd hotjava html//", "-//sun microsystems corp.//dtd hotjava strict html//",
        "-//w3c//dtd html 3", "-//w3c//dtd html 4.0 frameset//", "-//w3c//dtd html 4.0 transitional//",
        "-//w3c//dtd html experimental", "-//w3c//dtd w3 html//", "-//w3o//dtd w3 html 3.0//", "-//webtechs//dtd mozilla html",
    };
    return std::any_of(std::begin(prefix), std::end(prefix), [&](std::string_view p) { return pub.starts_with(p); });
}

enum class Mode {
    Initial, BeforeHtml, BeforeHead, InHead, InHeadNoscript, AfterHead, InBody, Text, InTable, InTableText,
    InCaption, InColumnGroup, InTableBody, InRow, InCell, InTemplate, AfterBody,
    InFrameset, AfterFrameset, AfterAfterBody, AfterAfterFrameset,
};

class TreeBuilder {
public:
    TreeBuilder(std::string_view html, ParseOptions options)
        : doc_(std::make_unique<Document>()), tok_(html), options_(options) {}

    // The fragment case's setup (13.4): a lone <html> on the stack and the context
    // element standing behind it for every decision that depends on where we are.
    void setFragmentContext(std::string_view name, Namespace ns) {
        context_ = doc_->make(NodeType::Element);
        context_->name = std::string(name);
        context_->ns = ns;
        if (ns == Namespace::Html) {
            if (name == "title" || name == "textarea") tok_.setState(TokenizerState::RcData);
            else if (in(name, {"style", "xmp", "iframe", "noembed", "noframes"})) tok_.setState(TokenizerState::RawText);
            else if (name == "script") tok_.setState(TokenizerState::ScriptData);
            else if (name == "noscript" && options_.scripting) tok_.setState(TokenizerState::RawText);
            else if (name == "plaintext") tok_.setState(TokenizerState::PlainText);
        }
        Node* html = createElement("html", {});
        doc_->root()->append(html);
        open_.push_back(html);
        if (context_->isHtml("template")) template_modes_.push_back(Mode::InTemplate);
        doc_->quirks = false;
        resetInsertionMode();
    }

    std::unique_ptr<Document> run() {
        Token t;
        do {
            // The tokenizer asks about the adjusted current node for CDATA.
            tok_.setCdataAllowed(!open_.empty() && adjustedCurrent()->ns != Namespace::Html);
            tok_.next(t);
            if (ignore_next_lf_) {
                ignore_next_lf_ = false;
                if (t.type == TokenType::Characters && !t.data.empty() && t.data[0] == '\n') {
                    t.data.erase(0, 1);
                    if (t.data.empty()) continue;
                }
            }
            dispatch(t);
        } while (t.type != TokenType::Eof);
        return std::move(doc_);
    }

private:
    // ── Dispatch ──────────────────────────────────────────────────────────────

    void dispatch(Token& t) {
        if (t.type == TokenType::Characters) {
            // Most modes treat whitespace apart, so hand over homogeneous runs.
            const std::string data = std::move(t.data);
            for (size_t i = 0; i < data.size();) {
                const bool ws = isWsChar(data[i]);
                size_t j = i + 1;
                while (j < data.size() && isWsChar(data[j]) == ws) ++j;
                Token run;
                run.type = TokenType::Characters;
                run.data = data.substr(i, j - i);
                route(run);
                i = j;
            }
            return;
        }
        route(t);
    }

    void route(Token& t) {
        if (useForeignRules(t)) foreign(t);
        else process(t);
    }

    bool useForeignRules(const Token& t) const {
        if (open_.empty()) return false;
        const Node* n = adjustedCurrent();
        if (n->ns == Namespace::Html) return false;
        const bool start = t.type == TokenType::StartTag;
        const bool chars = t.type == TokenType::Characters;
        if (isMathTextPoint(n) && ((start && t.name != "mglyph" && t.name != "malignmark") || chars)) return false;
        if (n->ns == Namespace::MathMl && n->name == "annotation-xml" && start && t.name == "svg") return false;
        if (isHtmlIntegrationPoint(n) && (start || chars)) return false;
        return t.type != TokenType::Eof;
    }

    void process(Token& t) {
        switch (mode_) {
            case Mode::Initial: initial(t); break;
            case Mode::BeforeHtml: beforeHtml(t); break;
            case Mode::BeforeHead: beforeHead(t); break;
            case Mode::InHead: inHead(t); break;
            case Mode::InHeadNoscript: inHeadNoscript(t); break;
            case Mode::AfterHead: afterHead(t); break;
            case Mode::InBody: inBody(t); break;
            case Mode::Text: text(t); break;
            case Mode::InTable: inTable(t); break;
            case Mode::InTableText: inTableText(t); break;
            case Mode::InCaption: inCaption(t); break;
            case Mode::InColumnGroup: inColumnGroup(t); break;
            case Mode::InTableBody: inTableBody(t); break;
            case Mode::InRow: inRow(t); break;
            case Mode::InCell: inCell(t); break;
            case Mode::InTemplate: inTemplate(t); break;
            case Mode::AfterBody: afterBody(t); break;
            case Mode::InFrameset: inFrameset(t); break;
            case Mode::AfterFrameset: afterFrameset(t); break;
            case Mode::AfterAfterBody: afterAfterBody(t); break;
            case Mode::AfterAfterFrameset: afterAfterFrameset(t); break;
        }
    }

    // ── Stack of open elements ────────────────────────────────────────────────

    [[nodiscard]] Node* current() const { return open_.back(); }
    [[nodiscard]] Node* adjustedCurrent() const { return (context_ && open_.size() == 1) ? context_ : open_.back(); }
    [[nodiscard]] bool currentIs(std::string_view tag) const { return !open_.empty() && current()->isHtml(tag); }
    [[nodiscard]] bool currentIsOneOf(Names names) const {
        return !open_.empty() && current()->ns == Namespace::Html && in(current()->name, names);
    }

    void pop() { if (!open_.empty()) open_.pop_back(); }

    void popUntil(std::string_view tag) {
        while (!open_.empty()) {
            const bool hit = current()->isHtml(tag);
            pop();
            if (hit) return;
        }
    }

    void popUntilOneOf(Names names) {
        while (!open_.empty()) {
            const bool hit = current()->ns == Namespace::Html && in(current()->name, names);
            pop();
            if (hit) return;
        }
    }

    [[nodiscard]] bool isOpen(const Node* n) const { return std::find(open_.begin(), open_.end(), n) != open_.end(); }
    void removeFromOpen(const Node* n) {
        auto it = std::find(open_.begin(), open_.end(), n);
        if (it != open_.end()) open_.erase(it);
    }

    [[nodiscard]] bool anyOpenIs(std::string_view tag) const {
        return std::any_of(open_.begin(), open_.end(), [&](const Node* n) { return n->isHtml(tag); });
    }

    enum class Scope { Default, ListItem, Button, Table, Select };

    [[nodiscard]] static bool stopsScope(const Node* n, Scope scope) {
        if (scope == Scope::Select) return !(n->ns == Namespace::Html && in(n->name, {"optgroup", "option"}));
        if (scope == Scope::Table) return n->ns == Namespace::Html && in(n->name, {"html", "table", "template"});
        if (n->ns == Namespace::Html) {
            if (in(n->name, {"applet", "caption", "html", "table", "td", "th", "marquee", "object", "select", "template"})) return true;
            if (scope == Scope::ListItem && in(n->name, {"ol", "ul"})) return true;
            if (scope == Scope::Button && n->name == "button") return true;
            return false;
        }
        if (n->ns == Namespace::MathMl) return in(n->name, {"mi", "mo", "mn", "ms", "mtext", "annotation-xml"});
        return in(n->name, {"foreignObject", "desc", "title"});
    }

    [[nodiscard]] bool hasInScope(std::string_view tag, Scope scope = Scope::Default) const {
        for (auto it = open_.rbegin(); it != open_.rend(); ++it) {
            if ((*it)->isHtml(tag)) return true;
            if (stopsScope(*it, scope)) return false;
        }
        return false;
    }

    [[nodiscard]] bool hasOneOfInScope(Names names, Scope scope = Scope::Default) const {
        for (auto it = open_.rbegin(); it != open_.rend(); ++it) {
            if ((*it)->ns == Namespace::Html && in((*it)->name, names)) return true;
            if (stopsScope(*it, scope)) return false;
        }
        return false;
    }

    [[nodiscard]] bool nodeInScope(const Node* n) const {
        for (auto it = open_.rbegin(); it != open_.rend(); ++it) {
            if (*it == n) return true;
            if (stopsScope(*it, Scope::Default)) return false;
        }
        return false;
    }

    void generateImpliedEndTags(std::string_view except = {}) {
        while (!open_.empty() && current()->ns == Namespace::Html &&
               in(current()->name, {"dd", "dt", "li", "optgroup", "option", "p", "rb", "rp", "rt", "rtc"}) &&
               current()->name != except)
            pop();
    }

    void generateAllImpliedEndTags() {
        while (!open_.empty() && current()->ns == Namespace::Html &&
               in(current()->name, {"caption", "colgroup", "dd", "dt", "li", "optgroup", "option", "p", "rb", "rp",
                                    "rt", "rtc", "tbody", "td", "tfoot", "th", "thead", "tr"}))
            pop();
    }

    void closePElement() {
        generateImpliedEndTags("p");
        popUntil("p");
    }

    void closePIfOpen() {
        if (hasInScope("p", Scope::Button)) closePElement();
    }

    // ── Insertion ─────────────────────────────────────────────────────────────

    struct Place {
        Node* parent;
        Node* before;
    };

    [[nodiscard]] static Node* contentOf(Node* n) { return n->template_content ? n->template_content : n; }

    [[nodiscard]] Place appropriatePlace(Node* target = nullptr) const {
        if (!target) target = current();
        if (foster_parenting_ && target->ns == Namespace::Html &&
            in(target->name, {"table", "tbody", "tfoot", "thead", "tr"})) {
            int last_template = -1, last_table = -1;
            for (int i = static_cast<int>(open_.size()) - 1; i >= 0; --i) {
                const auto at = static_cast<size_t>(i);
                if (last_template < 0 && open_[at]->isHtml("template")) last_template = i;
                if (last_table < 0 && open_[at]->isHtml("table")) last_table = i;
            }
            if (last_template >= 0 && (last_table < 0 || last_template > last_table))
                return {contentOf(open_[static_cast<size_t>(last_template)]), nullptr};
            if (last_table < 0) return {open_[0], nullptr};
            Node* table = open_[static_cast<size_t>(last_table)];
            if (table->parent) return {table->parent, table};
            return {open_[static_cast<size_t>(last_table) - 1], nullptr};
        }
        return {contentOf(target), nullptr};
    }

    Node* createElement(std::string name, std::vector<Attribute> attrs, Namespace ns = Namespace::Html) {
        Node* n = doc_->make(NodeType::Element);
        n->name = std::move(name);
        n->attrs = std::move(attrs);
        n->ns = ns;
        if (ns == Namespace::Html && n->name == "template") n->template_content = doc_->make(NodeType::Fragment);
        return n;
    }

    Node* insertElement(std::string name, std::vector<Attribute> attrs, Namespace ns = Namespace::Html) {
        const Place place = appropriatePlace();
        Node* n = createElement(std::move(name), std::move(attrs), ns);
        place.parent->insertBefore(n, place.before);
        // A page can't nest without limit: past the cap the element is still in the
        // document, but it doesn't become a parent.
        over_limit_ = open_.size() >= kMaxOpenElements ? n : nullptr;
        if (!over_limit_) open_.push_back(n);
        return n;
    }

    Node* insertHtml(const Token& t) { return insertElement(t.name, t.attrs); }

    void insertText(std::string_view s) {
        if (s.empty()) return;
        const Place place = appropriatePlace();
        if (place.parent->type == NodeType::Document) return;
        std::vector<Node*>& kids = place.parent->children;
        Node* prev = nullptr;
        if (place.before) {
            auto at = std::find(kids.begin(), kids.end(), place.before);
            if (at != kids.begin() && at != kids.end()) prev = *(at - 1);
        } else if (!kids.empty()) {
            prev = kids.back();
        }
        if (prev && prev->type == NodeType::Text) {
            prev->data.append(s);
            return;
        }
        Node* n = doc_->make(NodeType::Text);
        n->data = std::string(s);
        place.parent->insertBefore(n, place.before);
    }

    void insertComment(const Token& t, Node* parent = nullptr) {
        Node* n = doc_->make(NodeType::Comment);
        n->data = t.data;
        if (parent) {
            parent->append(n);
            return;
        }
        const Place place = appropriatePlace();
        place.parent->insertBefore(n, place.before);
    }

    void rawText(const Token& t, TokenizerState state) {
        insertHtml(t);
        tok_.setState(state);
        original_mode_ = mode_;
        mode_ = Mode::Text;
    }

    // An element that took no stack entry because of the cap needs no pop either.
    void popJustInserted() {
        if (over_limit_) over_limit_ = nullptr;
        else pop();
    }

    // ── Active formatting elements ────────────────────────────────────────────

    struct Formatting {
        Node* element;   // nullptr is a marker
    };

    void pushFormatting(Node* el) {
        // The Noah's Ark clause: at most three identical entries since the last marker.
        int same = 0;
        for (auto it = fmt_.rbegin(); it != fmt_.rend() && it->element; ++it) {
            const Node* o = it->element;
            if (o->name != el->name || o->ns != el->ns || o->attrs.size() != el->attrs.size()) continue;
            const bool equal = std::all_of(el->attrs.begin(), el->attrs.end(), [&](const Attribute& a) {
                const std::string* v = o->attr(a.name);
                return v && *v == a.value;
            });
            if (equal && ++same == 3) {
                fmt_.erase(std::next(it).base());
                break;
            }
        }
        fmt_.push_back({el});
    }

    void reconstructFormatting() {
        if (fmt_.empty()) return;
        if (!fmt_.back().element || isOpen(fmt_.back().element)) return;
        size_t i = fmt_.size() - 1;
        while (i > 0) {
            --i;
            if (!fmt_[i].element || isOpen(fmt_[i].element)) {
                ++i;
                break;
            }
        }
        for (; i < fmt_.size(); ++i) {
            const Node* old = fmt_[i].element;
            fmt_[i].element = insertElement(old->name, old->attrs, old->ns);
            over_limit_ = nullptr;
        }
    }

    void clearFormattingToMarker() {
        while (!fmt_.empty()) {
            const bool marker = !fmt_.back().element;
            fmt_.pop_back();
            if (marker) return;
        }
    }

    int formattingIndex(const Node* n) const {
        for (size_t i = fmt_.size(); i-- > 0;)
            if (fmt_[i].element == n) return static_cast<int>(i);
        return -1;
    }

    // ── The adoption agency ───────────────────────────────────────────────────

    // False means "act as for any other end tag".
    bool adoptionAgency(const std::string& subject) {
        if (currentIs(subject) && formattingIndex(current()) < 0) {
            pop();
            return true;
        }
        for (int outer = 0; outer < 8; ++outer) {
            int fi = -1;
            for (size_t i = fmt_.size(); i-- > 0;) {
                if (!fmt_[i].element) break;
                if (fmt_[i].element->name == subject) {
                    fi = static_cast<int>(i);
                    break;
                }
            }
            if (fi < 0) return false;
            Node* formatting = fmt_[static_cast<size_t>(fi)].element;
            if (!isOpen(formatting)) {
                fmt_.erase(fmt_.begin() + fi);
                return true;
            }
            if (!nodeInScope(formatting)) return true;

            const auto fpos = static_cast<size_t>(std::find(open_.begin(), open_.end(), formatting) - open_.begin());
            Node* furthest = nullptr;
            for (size_t i = fpos + 1; i < open_.size(); ++i) {
                if (isSpecial(open_[i])) {
                    furthest = open_[i];
                    break;
                }
            }
            if (!furthest) {
                while (current() != formatting) pop();
                pop();
                fmt_.erase(fmt_.begin() + fi);
                return true;
            }
            Node* common = open_[fpos - 1];
            int bookmark = fi;
            Node* node = furthest;
            Node* last = furthest;
            auto node_pos = static_cast<size_t>(std::find(open_.begin(), open_.end(), furthest) - open_.begin());
            for (int inner = 1;; ++inner) {
                --node_pos;
                node = open_[node_pos];
                if (node == formatting) break;
                int ni = formattingIndex(node);
                if (inner > 3 && ni >= 0) {
                    fmt_.erase(fmt_.begin() + ni);
                    if (ni < bookmark) --bookmark;
                    ni = -1;
                }
                if (ni < 0) {
                    open_.erase(open_.begin() + static_cast<std::ptrdiff_t>(node_pos));
                    continue;
                }
                Node* fresh = createElement(node->name, node->attrs, node->ns);
                fmt_[static_cast<size_t>(ni)].element = fresh;
                open_[node_pos] = fresh;
                node = fresh;
                if (last == furthest) bookmark = ni + 1;
                node->append(last);
                last = node;
            }
            const Place place = appropriatePlace(common);
            place.parent->insertBefore(last, place.before);

            Node* fresh = createElement(formatting->name, formatting->attrs, formatting->ns);
            const std::vector<Node*> moved = furthest->children;
            for (Node* c : moved) fresh->append(c);
            furthest->append(fresh);

            fi = formattingIndex(formatting);
            fmt_.erase(fmt_.begin() + fi);
            if (fi < bookmark) --bookmark;
            fmt_.insert(fmt_.begin() + std::min<int>(bookmark, static_cast<int>(fmt_.size())), {fresh});

            removeFromOpen(formatting);
            auto at = std::find(open_.begin(), open_.end(), furthest);
            open_.insert(at + 1, fresh);
        }
        return true;
    }

    // ── Foreign content ───────────────────────────────────────────────────────

    void leaveForeignContent() {
        while (!open_.empty() && current()->ns != Namespace::Html && !isMathTextPoint(current()) &&
               !isHtmlIntegrationPoint(current()))
            pop();
    }

    void foreign(Token& t) {
        switch (t.type) {
            case TokenType::Characters: {
                std::string fixed;
                bool counts = false;   // a NUL alone doesn't rule out a frameset
                for (char c : t.data) {
                    if (c == '\0') {
                        fixed += "\xEF\xBF\xBD";
                        continue;
                    }
                    fixed += c;
                    counts = counts || !isWsChar(c);
                }
                insertText(fixed);
                if (counts) frameset_ok_ = false;
                return;
            }
            case TokenType::Comment:
                insertComment(t);
                return;
            case TokenType::Doctype:
                return;
            case TokenType::StartTag: {
                const bool breakout =
                    in(t.name, {"b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl", "dt", "em",
                                "embed", "h1", "h2", "h3", "h4", "h5", "h6", "head", "hr", "i", "img", "li", "listing",
                                "menu", "meta", "nobr", "ol", "p", "pre", "ruby", "s", "small", "span", "strong",
                                "strike", "sub", "sup", "table", "tt", "u", "ul", "var"}) ||
                    (t.name == "font" && std::any_of(t.attrs.begin(), t.attrs.end(), [](const Attribute& a) {
                        return a.name == "color" || a.name == "face" || a.name == "size";
                    }));
                if (breakout) {
                    leaveForeignContent();
                    process(t);
                    return;
                }
                const Namespace ns = adjustedCurrent()->ns;
                std::string name = t.name;
                if (ns == Namespace::Svg) {
                    for (const auto& row : kSvgTags)
                        if (name == row[0]) {
                            name = std::string(row[1]);
                            break;
                        }
                    adjustSvgAttrs(t.attrs);
                } else {
                    adjustMathAttrs(t.attrs);
                }
                insertElement(std::move(name), t.attrs, ns);
                if (t.self_closing) popJustInserted();
                return;
            }
            case TokenType::EndTag: {
                if (t.name == "p" || t.name == "br") {
                    leaveForeignContent();
                    process(t);
                    return;
                }
                const auto lower = [](std::string s) {
                    std::transform(s.begin(), s.end(), s.begin(),
                                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
                    return s;
                };
                size_t i = open_.size() - 1;
                while (i > 0) {
                    if (lower(open_[i]->name) == t.name) {
                        open_.resize(i);
                        return;
                    }
                    --i;
                    if (open_[i]->ns == Namespace::Html) {
                        process(t);
                        return;
                    }
                }
                return;
            }
            case TokenType::Eof:
                return;
        }
    }

    void insertForeignFromBody(const Token& t, Namespace ns) {
        reconstructFormatting();
        std::vector<Attribute> attrs = t.attrs;
        if (ns == Namespace::Svg) adjustSvgAttrs(attrs);
        else adjustMathAttrs(attrs);
        insertElement(t.name, std::move(attrs), ns);
        if (t.self_closing) popJustInserted();
    }

    // ── Modes ─────────────────────────────────────────────────────────────────

    void initial(Token& t) {
        if (t.type == TokenType::Characters && allWs(t.data)) return;
        if (t.type == TokenType::Comment) {
            insertComment(t, doc_->root());
            return;
        }
        if (t.type == TokenType::Doctype) {
            Node* d = doc_->make(NodeType::Doctype);
            d->name = t.name;
            d->public_id = t.public_id;
            d->system_id = t.system_id;
            doc_->root()->append(d);
            std::string sys = t.system_id;
            std::transform(sys.begin(), sys.end(), sys.begin(),
                           [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
            doc_->quirks = t.force_quirks || t.name != "html" || (t.has_public && quirksPublicId(t.public_id)) ||
                           sys == "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd";
            if (!t.has_system && t.has_public) {
                std::string pub = t.public_id;
                std::transform(pub.begin(), pub.end(), pub.begin(),
                               [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
                if (pub.starts_with("-//w3c//dtd html 4.01 frameset//") ||
                    pub.starts_with("-//w3c//dtd html 4.01 transitional//"))
                    doc_->quirks = true;
            }
            mode_ = Mode::BeforeHtml;
            return;
        }
        doc_->quirks = true;
        mode_ = Mode::BeforeHtml;
        process(t);
    }

    void beforeHtml(Token& t) {
        if (t.type == TokenType::Doctype) return;
        if (t.type == TokenType::Comment) {
            insertComment(t, doc_->root());
            return;
        }
        if (t.type == TokenType::Characters && allWs(t.data)) return;
        if (t.type == TokenType::StartTag && t.name == "html") {
            Node* n = createElement("html", t.attrs);
            doc_->root()->append(n);
            open_.push_back(n);
            mode_ = Mode::BeforeHead;
            return;
        }
        if (t.type == TokenType::EndTag && !in(t.name, {"head", "body", "html", "br"})) return;
        Node* n = createElement("html", {});
        doc_->root()->append(n);
        open_.push_back(n);
        mode_ = Mode::BeforeHead;
        process(t);
    }

    void beforeHead(Token& t) {
        if (t.type == TokenType::Characters && allWs(t.data)) return;
        if (t.type == TokenType::Comment) {
            insertComment(t);
            return;
        }
        if (t.type == TokenType::Doctype) return;
        if (t.type == TokenType::StartTag && t.name == "html") {
            inBody(t);
            return;
        }
        if (t.type == TokenType::StartTag && t.name == "head") {
            head_ = insertHtml(t);
            mode_ = Mode::InHead;
            return;
        }
        if (t.type == TokenType::EndTag && !in(t.name, {"head", "body", "html", "br"})) return;
        head_ = insertElement("head", {});
        mode_ = Mode::InHead;
        process(t);
    }

    void inHead(Token& t) {
        if (t.type == TokenType::Characters && allWs(t.data)) {
            insertText(t.data);
            return;
        }
        if (t.type == TokenType::Comment) {
            insertComment(t);
            return;
        }
        if (t.type == TokenType::Doctype) return;
        if (t.type == TokenType::StartTag) {
            if (t.name == "html") {
                inBody(t);
                return;
            }
            if (in(t.name, {"base", "basefont", "bgsound", "link", "meta"})) {
                insertHtml(t);
                popJustInserted();
                return;
            }
            if (t.name == "title") {
                rawText(t, TokenizerState::RcData);
                return;
            }
            if (t.name == "noscript" && !options_.scripting) {
                insertHtml(t);
                mode_ = Mode::InHeadNoscript;
                return;
            }
            if (in(t.name, {"noscript", "noframes", "style"})) {
                rawText(t, TokenizerState::RawText);
                return;
            }
            if (t.name == "script") {
                rawText(t, TokenizerState::ScriptData);
                return;
            }
            if (t.name == "template") {
                insertHtml(t);
                fmt_.push_back({nullptr});
                frameset_ok_ = false;
                mode_ = Mode::InTemplate;
                template_modes_.push_back(Mode::InTemplate);
                return;
            }
            if (t.name == "head") return;
        }
        if (t.type == TokenType::EndTag) {
            if (t.name == "head") {
                pop();
                mode_ = Mode::AfterHead;
                return;
            }
            if (t.name == "template") {
                endTemplate();
                return;
            }
            if (!in(t.name, {"body", "html", "br"})) return;
        }
        pop();
        mode_ = Mode::AfterHead;
        process(t);
    }

    void endTemplate() {
        if (!anyOpenIs("template")) return;
        generateAllImpliedEndTags();
        popUntil("template");
        clearFormattingToMarker();
        if (!template_modes_.empty()) template_modes_.pop_back();
        resetInsertionMode();
    }

    void inHeadNoscript(Token& t) {
        if (t.type == TokenType::Doctype) return;
        if (t.type == TokenType::StartTag && t.name == "html") {
            inBody(t);
            return;
        }
        if (t.type == TokenType::EndTag && t.name == "noscript") {
            pop();
            mode_ = Mode::InHead;
            return;
        }
        if ((t.type == TokenType::Characters && allWs(t.data)) || t.type == TokenType::Comment ||
            (t.type == TokenType::StartTag && in(t.name, {"basefont", "bgsound", "link", "meta", "noframes", "style"}))) {
            inHead(t);
            return;
        }
        if (t.type == TokenType::StartTag && in(t.name, {"head", "noscript"})) return;
        if (t.type == TokenType::EndTag && t.name != "br") return;
        pop();
        mode_ = Mode::InHead;
        process(t);
    }

    void afterHead(Token& t) {
        if (t.type == TokenType::Characters && allWs(t.data)) {
            insertText(t.data);
            return;
        }
        if (t.type == TokenType::Comment) {
            insertComment(t);
            return;
        }
        if (t.type == TokenType::Doctype) return;
        if (t.type == TokenType::StartTag) {
            if (t.name == "html") {
                inBody(t);
                return;
            }
            if (t.name == "body") {
                insertHtml(t);
                frameset_ok_ = false;
                mode_ = Mode::InBody;
                return;
            }
            if (t.name == "frameset") {
                insertHtml(t);
                mode_ = Mode::InFrameset;
                return;
            }
            if (in(t.name, {"base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title"})) {
                open_.push_back(head_);
                inHead(t);
                removeFromOpen(head_);
                return;
            }
            if (t.name == "head") return;
        }
        if (t.type == TokenType::EndTag) {
            if (t.name == "template") {
                inHead(t);
                return;
            }
            if (!in(t.name, {"body", "html", "br"})) return;
        }
        insertElement("body", {});
        mode_ = Mode::InBody;
        process(t);
    }

    void text(Token& t) {
        if (t.type == TokenType::Characters) {
            insertText(t.data);
            return;
        }
        if (t.type == TokenType::Eof) {
            pop();
            mode_ = original_mode_;
            process(t);
            return;
        }
        if (t.type == TokenType::EndTag) {
            pop();
            mode_ = original_mode_;
        }
    }

    void inBodyChars(Token& t) {
        std::string data;
        data.reserve(t.data.size());
        for (char c : t.data)
            if (c != '\0') data += c;
        if (data.empty()) return;
        reconstructFormatting();
        insertText(data);
        if (!allWs(data)) frameset_ok_ = false;
    }

    void inBodyAnyOtherEnd(const std::string& name) {
        for (size_t i = open_.size(); i-- > 0;) {
            Node* n = open_[i];
            if (n->isHtml(name)) {
                generateImpliedEndTags(name);
                while (current() != n) pop();
                pop();
                return;
            }
            if (isSpecial(n)) return;
        }
    }

    void inBodyStart(Token& t) {
        const std::string& n = t.name;
        if (n == "html") {
            if (anyOpenIs("template") || open_.empty()) return;
            for (const auto& a : t.attrs)
                if (!open_[0]->attr(a.name)) open_[0]->attrs.push_back(a);
            return;
        }
        if (in(n, {"base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title"})) {
            inHead(t);
            return;
        }
        if (n == "body") {
            if (open_.size() < 2 || !open_[1]->isHtml("body") || anyOpenIs("template")) return;
            frameset_ok_ = false;
            for (const auto& a : t.attrs)
                if (!open_[1]->attr(a.name)) open_[1]->attrs.push_back(a);
            return;
        }
        if (n == "frameset") {
            if (open_.size() < 2 || !open_[1]->isHtml("body") || !frameset_ok_) return;
            Node* body = open_[1];
            if (body->parent) body->parent->remove(body);
            open_.resize(1);
            insertHtml(t);
            mode_ = Mode::InFrameset;
            return;
        }
        if (in(n, {"address", "article", "aside", "blockquote", "center", "details", "dialog", "dir", "div", "dl",
                   "fieldset", "figcaption", "figure", "footer", "header", "hgroup", "main", "menu", "nav", "ol", "p",
                   "search", "section", "summary", "ul"})) {
            closePIfOpen();
            insertHtml(t);
            return;
        }
        if (in(n, {"h1", "h2", "h3", "h4", "h5", "h6"})) {
            closePIfOpen();
            if (currentIsOneOf({"h1", "h2", "h3", "h4", "h5", "h6"})) pop();
            insertHtml(t);
            return;
        }
        if (in(n, {"pre", "listing"})) {
            closePIfOpen();
            insertHtml(t);
            ignore_next_lf_ = true;
            frameset_ok_ = false;
            return;
        }
        if (n == "form") {
            if (form_ && !anyOpenIs("template")) return;
            closePIfOpen();
            Node* f = insertHtml(t);
            if (!anyOpenIs("template")) form_ = f;
            return;
        }
        if (n == "li" || n == "dd" || n == "dt") {
            frameset_ok_ = false;
            for (size_t i = open_.size(); i-- > 0;) {
                const Node* node = open_[i];
                const bool hit = n == "li" ? node->isHtml("li") : (node->isHtml("dd") || node->isHtml("dt"));
                if (hit) {
                    generateImpliedEndTags(node->name);
                    popUntil(node->name);
                    break;
                }
                if (isSpecial(node) && !(node->ns == Namespace::Html && in(node->name, {"address", "div", "p"}))) break;
            }
            closePIfOpen();
            insertHtml(t);
            return;
        }
        if (n == "plaintext") {
            closePIfOpen();
            insertHtml(t);
            tok_.setState(TokenizerState::PlainText);
            return;
        }
        if (n == "button") {
            if (hasInScope("button")) {
                generateImpliedEndTags();
                popUntil("button");
            }
            reconstructFormatting();
            insertHtml(t);
            frameset_ok_ = false;
            return;
        }
        if (n == "a") {
            for (size_t i = fmt_.size(); i-- > 0;) {
                if (!fmt_[i].element) break;
                if (fmt_[i].element->isHtml("a")) {
                    Node* old = fmt_[i].element;
                    if (!adoptionAgency("a")) inBodyAnyOtherEnd("a");
                    const int at = formattingIndex(old);
                    if (at >= 0) fmt_.erase(fmt_.begin() + at);
                    removeFromOpen(old);
                    break;
                }
            }
            reconstructFormatting();
            pushFormatting(insertHtml(t));
            return;
        }
        if (in(n, {"b", "big", "code", "em", "font", "i", "s", "small", "strike", "strong", "tt", "u"})) {
            reconstructFormatting();
            pushFormatting(insertHtml(t));
            return;
        }
        if (n == "nobr") {
            reconstructFormatting();
            if (hasInScope("nobr")) {
                if (!adoptionAgency("nobr")) inBodyAnyOtherEnd("nobr");
                reconstructFormatting();
            }
            pushFormatting(insertHtml(t));
            return;
        }
        if (in(n, {"applet", "marquee", "object"})) {
            reconstructFormatting();
            insertHtml(t);
            fmt_.push_back({nullptr});
            frameset_ok_ = false;
            return;
        }
        if (n == "table") {
            if (!doc_->quirks) closePIfOpen();
            insertHtml(t);
            frameset_ok_ = false;
            mode_ = Mode::InTable;
            return;
        }
        if (in(n, {"area", "br", "embed", "img", "keygen", "wbr"})) {
            reconstructFormatting();
            insertHtml(t);
            popJustInserted();
            frameset_ok_ = false;
            return;
        }
        if (n == "input") {
            if (context_ && context_->isHtml("select")) return;
            if (hasInScope("select")) popUntil("select");
            reconstructFormatting();
            insertHtml(t);
            popJustInserted();
            const std::string* type = nullptr;
            for (const auto& a : t.attrs)
                if (a.name == "type") type = &a.value;
            std::string lower = type ? *type : "";
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
            if (lower != "hidden") frameset_ok_ = false;
            return;
        }
        if (in(n, {"param", "source", "track"})) {
            insertHtml(t);
            popJustInserted();
            return;
        }
        if (n == "hr") {
            closePIfOpen();
            if (hasInScope("select")) generateImpliedEndTags();
            insertHtml(t);
            popJustInserted();
            frameset_ok_ = false;
            return;
        }
        if (n == "image") {   // "Don't ask."
            t.name = "img";
            inBodyStart(t);
            return;
        }
        if (n == "textarea") {
            insertHtml(t);
            ignore_next_lf_ = true;
            tok_.setState(TokenizerState::RcData);
            original_mode_ = mode_;
            frameset_ok_ = false;
            mode_ = Mode::Text;
            return;
        }
        if (n == "xmp") {
            closePIfOpen();
            reconstructFormatting();
            frameset_ok_ = false;
            rawText(t, TokenizerState::RawText);
            return;
        }
        if (n == "iframe") {
            frameset_ok_ = false;
            rawText(t, TokenizerState::RawText);
            return;
        }
        if (n == "noembed" || (n == "noscript" && options_.scripting)) {
            rawText(t, TokenizerState::RawText);
            return;
        }
        // A select holds what it likes now (customizable select): no parser mode of its
        // own, only these rules so that options and nested selects still close.
        if (n == "select") {
            if (context_ && context_->isHtml("select")) return;
            if (hasInScope("select")) {
                popUntil("select");
                return;
            }
            reconstructFormatting();
            insertHtml(t);
            frameset_ok_ = false;
            return;
        }
        if (n == "option") {
            if (hasInScope("select")) generateImpliedEndTags("optgroup");
            else if (currentIs("option")) pop();
            reconstructFormatting();
            insertHtml(t);
            return;
        }
        if (n == "optgroup") {
            if (hasInScope("select")) generateImpliedEndTags();
            else if (currentIs("option")) pop();
            reconstructFormatting();
            insertHtml(t);
            return;
        }
        if (n == "rb" || n == "rtc") {
            if (hasInScope("ruby")) generateImpliedEndTags();
            insertHtml(t);
            return;
        }
        if (n == "rp" || n == "rt") {
            if (hasInScope("ruby")) generateImpliedEndTags("rtc");
            insertHtml(t);
            return;
        }
        if (n == "math") {
            insertForeignFromBody(t, Namespace::MathMl);
            return;
        }
        if (n == "svg") {
            insertForeignFromBody(t, Namespace::Svg);
            return;
        }
        if (in(n, {"caption", "col", "colgroup", "frame", "head", "tbody", "td", "tfoot", "th", "thead", "tr"})) return;
        reconstructFormatting();
        insertHtml(t);
    }

    void inBodyEnd(Token& t) {
        const std::string& n = t.name;
        if (n == "template") {
            inHead(t);
            return;
        }
        if (n == "body" || n == "html") {
            if (!hasInScope("body")) return;
            mode_ = Mode::AfterBody;
            if (n == "html") process(t);
            return;
        }
        if (in(n, {"address", "article", "aside", "blockquote", "button", "center", "details", "dialog", "dir", "div",
                   "dl", "fieldset", "figcaption", "figure", "footer", "header", "hgroup", "listing", "main", "menu",
                   "nav", "ol", "pre", "search", "section", "select", "summary", "ul"})) {
            if (!hasInScope(n)) return;
            generateImpliedEndTags();
            popUntil(n);
            return;
        }
        if (n == "form") {
            if (!anyOpenIs("template")) {
                Node* f = form_;
                form_ = nullptr;
                if (!f || !nodeInScope(f)) return;
                generateImpliedEndTags();
                removeFromOpen(f);
            } else {
                if (!hasInScope("form")) return;
                generateImpliedEndTags();
                popUntil("form");
            }
            return;
        }
        if (n == "p") {
            if (!hasInScope("p", Scope::Button)) insertElement("p", {});
            closePElement();
            return;
        }
        if (n == "li") {
            if (!hasInScope("li", Scope::ListItem)) return;
            generateImpliedEndTags("li");
            popUntil("li");
            return;
        }
        if (n == "dd" || n == "dt") {
            if (!hasInScope(n)) return;
            generateImpliedEndTags(n);
            popUntil(n);
            return;
        }
        if (in(n, {"h1", "h2", "h3", "h4", "h5", "h6"})) {
            if (!hasOneOfInScope({"h1", "h2", "h3", "h4", "h5", "h6"})) return;
            generateImpliedEndTags();
            popUntilOneOf({"h1", "h2", "h3", "h4", "h5", "h6"});
            return;
        }
        if (in(n, {"a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u"})) {
            if (!adoptionAgency(n)) inBodyAnyOtherEnd(n);
            return;
        }
        if (in(n, {"applet", "marquee", "object"})) {
            if (!hasInScope(n)) return;
            generateImpliedEndTags();
            popUntil(n);
            clearFormattingToMarker();
            return;
        }
        if (n == "br") {
            Token br;
            br.type = TokenType::StartTag;
            br.name = "br";
            inBodyStart(br);
            return;
        }
        inBodyAnyOtherEnd(n);
    }

    void inBody(Token& t) {
        switch (t.type) {
            case TokenType::Characters: inBodyChars(t); return;
            case TokenType::Comment: insertComment(t); return;
            case TokenType::Doctype: return;
            case TokenType::StartTag: inBodyStart(t); return;
            case TokenType::EndTag: inBodyEnd(t); return;
            case TokenType::Eof:
                if (!template_modes_.empty()) inTemplate(t);
                return;
        }
    }

    void clearStackToTableContext() {
        while (!currentIsOneOf({"table", "template", "html"})) pop();
    }
    void clearStackToTableBodyContext() {
        while (!currentIsOneOf({"tbody", "tfoot", "thead", "template", "html"})) pop();
    }
    void clearStackToRowContext() {
        while (!currentIsOneOf({"tr", "template", "html"})) pop();
    }

    void inTable(Token& t) {
        if (t.type == TokenType::Characters) {
            if (currentIsOneOf({"table", "tbody", "template", "tfoot", "thead", "tr"})) {
                pending_table_text_.clear();
                table_original_mode_ = mode_;
                mode_ = Mode::InTableText;
                inTableText(t);
                return;
            }
        } else if (t.type == TokenType::Comment) {
            insertComment(t);
            return;
        } else if (t.type == TokenType::Doctype) {
            return;
        } else if (t.type == TokenType::StartTag) {
            const std::string& n = t.name;
            if (n == "caption") {
                clearStackToTableContext();
                fmt_.push_back({nullptr});
                insertHtml(t);
                mode_ = Mode::InCaption;
                return;
            }
            if (n == "colgroup") {
                clearStackToTableContext();
                insertHtml(t);
                mode_ = Mode::InColumnGroup;
                return;
            }
            if (n == "col") {
                clearStackToTableContext();
                insertElement("colgroup", {});
                mode_ = Mode::InColumnGroup;
                process(t);
                return;
            }
            if (in(n, {"tbody", "tfoot", "thead"})) {
                clearStackToTableContext();
                insertHtml(t);
                mode_ = Mode::InTableBody;
                return;
            }
            if (in(n, {"td", "th", "tr"})) {
                clearStackToTableContext();
                insertElement("tbody", {});
                mode_ = Mode::InTableBody;
                process(t);
                return;
            }
            if (n == "table") {
                if (!hasInScope("table", Scope::Table)) return;
                popUntil("table");
                resetInsertionMode();
                process(t);
                return;
            }
            if (in(n, {"style", "script", "template"})) {
                inHead(t);
                return;
            }
            if (n == "input") {
                const std::string* type = nullptr;
                for (const auto& a : t.attrs)
                    if (a.name == "type") type = &a.value;
                std::string lower = type ? *type : "";
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
                if (lower == "hidden") {
                    insertHtml(t);
                    popJustInserted();
                    return;
                }
            }
            if (n == "form") {
                if (form_ || anyOpenIs("template")) return;
                form_ = insertHtml(t);
                popJustInserted();
                return;
            }
        } else if (t.type == TokenType::EndTag) {
            const std::string& n = t.name;
            if (n == "table") {
                if (!hasInScope("table", Scope::Table)) return;
                popUntil("table");
                resetInsertionMode();
                return;
            }
            if (in(n, {"body", "caption", "col", "colgroup", "html", "tbody", "td", "tfoot", "th", "thead", "tr"})) return;
            if (n == "template") {
                inHead(t);
                return;
            }
        } else {
            inBody(t);
            return;
        }
        // Anything else lands in front of the table.
        foster_parenting_ = true;
        inBody(t);
        foster_parenting_ = false;
    }

    void inTableText(Token& t) {
        if (t.type == TokenType::Characters) {
            for (char c : t.data)
                if (c != '\0') pending_table_text_ += c;
            return;
        }
        const std::string pending = std::move(pending_table_text_);
        pending_table_text_.clear();
        mode_ = table_original_mode_;
        if (!allWs(pending)) {
            foster_parenting_ = true;
            Token chars;
            chars.type = TokenType::Characters;
            chars.data = pending;
            inBodyChars(chars);
            foster_parenting_ = false;
        } else {
            insertText(pending);
        }
        process(t);
    }

    void inCaption(Token& t) {
        const bool closes = (t.type == TokenType::EndTag && t.name == "table") ||
                            (t.type == TokenType::StartTag &&
                             in(t.name, {"caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"}));
        if ((t.type == TokenType::EndTag && t.name == "caption") || closes) {
            if (!hasInScope("caption", Scope::Table)) return;
            generateImpliedEndTags();
            popUntil("caption");
            clearFormattingToMarker();
            mode_ = Mode::InTable;
            if (closes) process(t);
            return;
        }
        if (t.type == TokenType::EndTag &&
            in(t.name, {"body", "col", "colgroup", "html", "tbody", "td", "tfoot", "th", "thead", "tr"}))
            return;
        inBody(t);
    }

    void inColumnGroup(Token& t) {
        if (t.type == TokenType::Characters && allWs(t.data)) {
            insertText(t.data);
            return;
        }
        if (t.type == TokenType::Comment) {
            insertComment(t);
            return;
        }
        if (t.type == TokenType::Doctype) return;
        if (t.type == TokenType::StartTag) {
            if (t.name == "html") {
                inBody(t);
                return;
            }
            if (t.name == "col") {
                insertHtml(t);
                popJustInserted();
                return;
            }
            if (t.name == "template") {
                inHead(t);
                return;
            }
        }
        if (t.type == TokenType::EndTag) {
            if (t.name == "colgroup") {
                if (!currentIs("colgroup")) return;
                pop();
                mode_ = Mode::InTable;
                return;
            }
            if (t.name == "col") return;
            if (t.name == "template") {
                inHead(t);
                return;
            }
        }
        if (t.type == TokenType::Eof) {
            inBody(t);
            return;
        }
        if (!currentIs("colgroup")) return;
        pop();
        mode_ = Mode::InTable;
        process(t);
    }

    void inTableBody(Token& t) {
        if (t.type == TokenType::StartTag) {
            if (t.name == "tr") {
                clearStackToTableBodyContext();
                insertHtml(t);
                mode_ = Mode::InRow;
                return;
            }
            if (t.name == "th" || t.name == "td") {
                clearStackToTableBodyContext();
                insertElement("tr", {});
                mode_ = Mode::InRow;
                process(t);
                return;
            }
            if (in(t.name, {"caption", "col", "colgroup", "tbody", "tfoot", "thead"})) {
                if (!hasOneOfInScope({"tbody", "thead", "tfoot"}, Scope::Table)) return;
                clearStackToTableBodyContext();
                pop();
                mode_ = Mode::InTable;
                process(t);
                return;
            }
        } else if (t.type == TokenType::EndTag) {
            if (in(t.name, {"tbody", "tfoot", "thead"})) {
                if (!hasInScope(t.name, Scope::Table)) return;
                clearStackToTableBodyContext();
                pop();
                mode_ = Mode::InTable;
                return;
            }
            if (t.name == "table") {
                if (!hasOneOfInScope({"tbody", "thead", "tfoot"}, Scope::Table)) return;
                clearStackToTableBodyContext();
                pop();
                mode_ = Mode::InTable;
                process(t);
                return;
            }
            if (in(t.name, {"body", "caption", "col", "colgroup", "html", "td", "th", "tr"})) return;
        }
        inTable(t);
    }

    void inRow(Token& t) {
        const auto closeRow = [&] {
            if (!hasInScope("tr", Scope::Table)) return false;
            clearStackToRowContext();
            pop();
            mode_ = Mode::InTableBody;
            return true;
        };
        if (t.type == TokenType::StartTag) {
            if (t.name == "th" || t.name == "td") {
                clearStackToRowContext();
                insertHtml(t);
                mode_ = Mode::InCell;
                fmt_.push_back({nullptr});
                return;
            }
            if (in(t.name, {"caption", "col", "colgroup", "tbody", "tfoot", "thead", "tr"})) {
                if (closeRow()) process(t);
                return;
            }
        } else if (t.type == TokenType::EndTag) {
            if (t.name == "tr") {
                closeRow();
                return;
            }
            if (t.name == "table") {
                if (closeRow()) process(t);
                return;
            }
            if (in(t.name, {"tbody", "tfoot", "thead"})) {
                if (!hasInScope(t.name, Scope::Table)) return;
                if (closeRow()) process(t);
                return;
            }
            if (in(t.name, {"body", "caption", "col", "colgroup", "html", "td", "th"})) return;
        }
        inTable(t);
    }

    void closeCell() {
        generateImpliedEndTags();
        popUntilOneOf({"td", "th"});
        clearFormattingToMarker();
        mode_ = Mode::InRow;
    }

    void inCell(Token& t) {
        if (t.type == TokenType::EndTag) {
            if (t.name == "td" || t.name == "th") {
                if (!hasInScope(t.name, Scope::Table)) return;
                generateImpliedEndTags();
                popUntil(t.name);
                clearFormattingToMarker();
                mode_ = Mode::InRow;
                return;
            }
            if (in(t.name, {"body", "caption", "col", "colgroup", "html"})) return;
            if (in(t.name, {"table", "tbody", "tfoot", "thead", "tr"})) {
                if (!hasInScope(t.name, Scope::Table)) return;
                closeCell();
                process(t);
                return;
            }
        }
        if (t.type == TokenType::StartTag &&
            in(t.name, {"caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"})) {
            if (!hasOneOfInScope({"td", "th"}, Scope::Table)) return;
            closeCell();
            process(t);
            return;
        }
        inBody(t);
    }

    void inTemplate(Token& t) {
        switch (t.type) {
            case TokenType::Characters:
            case TokenType::Comment:
            case TokenType::Doctype:
                inBody(t);
                return;
            case TokenType::EndTag:
                if (t.name == "template") inHead(t);
                return;
            case TokenType::Eof:
                if (!anyOpenIs("template")) return;
                popUntil("template");
                clearFormattingToMarker();
                if (!template_modes_.empty()) template_modes_.pop_back();
                resetInsertionMode();
                process(t);
                return;
            case TokenType::StartTag: {
                const std::string& n = t.name;
                if (in(n, {"base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title"})) {
                    inHead(t);
                    return;
                }
                Mode next = Mode::InBody;
                if (in(n, {"caption", "colgroup", "tbody", "tfoot", "thead"})) next = Mode::InTable;
                else if (n == "col") next = Mode::InColumnGroup;
                else if (n == "tr") next = Mode::InTableBody;
                else if (n == "td" || n == "th") next = Mode::InRow;
                template_modes_.back() = next;
                mode_ = next;
                process(t);
                return;
            }
        }
    }

    void afterBody(Token& t) {
        if (t.type == TokenType::Characters && allWs(t.data)) {
            inBody(t);
            return;
        }
        if (t.type == TokenType::Comment) {
            insertComment(t, open_[0]);
            return;
        }
        if (t.type == TokenType::Doctype) return;
        if (t.type == TokenType::StartTag && t.name == "html") {
            inBody(t);
            return;
        }
        if (t.type == TokenType::EndTag && t.name == "html") {
            if (!context_) mode_ = Mode::AfterAfterBody;   // a fragment stays in its <html>
            return;
        }
        if (t.type == TokenType::Eof) return;
        mode_ = Mode::InBody;
        process(t);
    }

    void inFrameset(Token& t) {
        if (t.type == TokenType::Characters) {
            if (allWs(t.data)) insertText(t.data);
            return;
        }
        if (t.type == TokenType::Comment) {
            insertComment(t);
            return;
        }
        if (t.type == TokenType::StartTag) {
            if (t.name == "html") inBody(t);
            else if (t.name == "frameset") insertHtml(t);
            else if (t.name == "frame") {
                insertHtml(t);
                popJustInserted();
            } else if (t.name == "noframes") {
                inHead(t);
            }
            return;
        }
        if (t.type == TokenType::EndTag && t.name == "frameset") {
            if (open_.size() == 1) return;
            pop();
            if (!currentIs("frameset")) mode_ = Mode::AfterFrameset;
        }
    }

    void afterFrameset(Token& t) {
        if (t.type == TokenType::Characters) {
            if (allWs(t.data)) insertText(t.data);
            return;
        }
        if (t.type == TokenType::Comment) {
            insertComment(t);
            return;
        }
        if (t.type == TokenType::StartTag) {
            if (t.name == "html") inBody(t);
            else if (t.name == "noframes") inHead(t);
            return;
        }
        if (t.type == TokenType::EndTag && t.name == "html") mode_ = Mode::AfterAfterFrameset;
    }

    void afterAfterBody(Token& t) {
        if (t.type == TokenType::Comment) {
            insertComment(t, doc_->root());
            return;
        }
        if (t.type == TokenType::Doctype || (t.type == TokenType::Characters && allWs(t.data)) ||
            (t.type == TokenType::StartTag && t.name == "html")) {
            inBody(t);
            return;
        }
        if (t.type == TokenType::Eof) return;
        mode_ = Mode::InBody;
        process(t);
    }

    void afterAfterFrameset(Token& t) {
        if (t.type == TokenType::Comment) {
            insertComment(t, doc_->root());
            return;
        }
        if (t.type == TokenType::Doctype || (t.type == TokenType::Characters && allWs(t.data)) ||
            (t.type == TokenType::StartTag && t.name == "html")) {
            inBody(t);
            return;
        }
        if (t.type == TokenType::StartTag && t.name == "noframes") inHead(t);
    }

    void resetInsertionMode() {
        for (size_t i = open_.size(); i-- > 0;) {
            const bool last = i == 0;
            const Node* node = (last && context_) ? context_ : open_[i];
            if (node->ns != Namespace::Html) {
                if (last) { mode_ = Mode::InBody; return; }
                continue;
            }
            const std::string& n = node->name;
            if ((n == "td" || n == "th") && !last) { mode_ = Mode::InCell; return; }
            if (n == "tr") { mode_ = Mode::InRow; return; }
            if (in(n, {"tbody", "thead", "tfoot"})) { mode_ = Mode::InTableBody; return; }
            if (n == "caption") { mode_ = Mode::InCaption; return; }
            if (n == "colgroup") { mode_ = Mode::InColumnGroup; return; }
            if (n == "table") { mode_ = Mode::InTable; return; }
            if (n == "template") { mode_ = template_modes_.empty() ? Mode::InBody : template_modes_.back(); return; }
            if (n == "head" && !last) { mode_ = Mode::InHead; return; }
            if (n == "body") { mode_ = Mode::InBody; return; }
            if (n == "frameset") { mode_ = Mode::InFrameset; return; }
            if (n == "html") { mode_ = head_ ? Mode::AfterHead : Mode::BeforeHead; return; }
            if (last) { mode_ = Mode::InBody; return; }
        }
        mode_ = Mode::InBody;
    }

    std::unique_ptr<Document> doc_;
    Tokenizer tok_;
    ParseOptions options_;
    Mode mode_ = Mode::Initial;
    Mode original_mode_ = Mode::InBody;
    Mode table_original_mode_ = Mode::InTable;
    std::vector<Node*> open_;
    std::vector<Formatting> fmt_;
    std::vector<Mode> template_modes_;
    std::string pending_table_text_;
    Node* head_ = nullptr;
    Node* form_ = nullptr;
    Node* context_ = nullptr;
    Node* over_limit_ = nullptr;
    bool frameset_ok_ = true;
    bool foster_parenting_ = false;
    bool ignore_next_lf_ = false;
};

}  // namespace

std::unique_ptr<Document> parseHtml(std::string_view html, ParseOptions options) {
    std::unique_ptr<Document> doc = TreeBuilder(html, options).run();
    attachDeclarativeShadowRoots(*doc);
    return doc;
}

std::unique_ptr<Document> parseFragment(std::string_view html, std::string_view context, Namespace context_ns,
                                        ParseOptions options) {
    TreeBuilder builder(html, options);
    builder.setFragmentContext(context, context_ns);
    return builder.run();
}

}  // namespace fbx::xweb
