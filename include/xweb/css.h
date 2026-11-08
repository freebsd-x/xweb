#pragma once

// Our CSS parser: the CSS Syntax 3 tokenizer and rule parsing, plus selectors,
// written by hand. As in the spec, what can't be parsed is dropped, not an error.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fbx::xweb::css {

enum class TokenType {
    Ident, Function, AtKeyword, Hash, String, BadString, Url, BadUrl, Delim, Number, Percentage, Dimension,
    Whitespace, Cdo, Cdc, Colon, Semicolon, Comma, LeftBracket, RightBracket, LeftParen, RightParen, LeftBrace,
    RightBrace, Eof,
};

struct Token {
    TokenType type = TokenType::Eof;
    std::string text;      // name, string value, unit; a Delim's one code point
    double number = 0;     // Number, Percentage, Dimension
    bool is_integer = false;
    bool is_id = false;    // Hash that would make a valid #id
    uint32_t begin = 0, end = 0;   // where it is in the source
};

[[nodiscard]] std::vector<Token> tokenize(std::string_view css);

struct Declaration {
    std::string name;            // lower case, except custom properties
    std::vector<Token> value;    // without the !important
    bool important = false;
};

[[nodiscard]] std::vector<Declaration> parseDeclarations(std::string_view css);

// ── Selectors ─────────────────────────────────────────────────────────────────

struct ComplexSelector;

enum class SimpleKind { Type, Universal, Id, Class, Attribute, PseudoClass, PseudoElement };
enum class AttrOp { Exists, Equals, Includes, DashMatch, Prefix, Suffix, Substring };

struct SimpleSelector {
    SimpleKind kind = SimpleKind::Universal;
    std::string name;            // lower case for Type, Attribute and pseudo names
    AttrOp op = AttrOp::Exists;
    std::string value;
    bool case_insensitive = false;
    int a = 0, b = 0;            // :nth-*(an+b)
    std::vector<ComplexSelector> args;   // :not() :is() :where()
};

enum class Combinator { Descendant, Child, NextSibling, SubsequentSibling };

struct CompoundSelector {
    std::vector<SimpleSelector> simple;
    Combinator combinator = Combinator::Descendant;   // how this joins to the one before it
};

struct Specificity {
    uint32_t ids = 0, classes = 0, types = 0;
    [[nodiscard]] uint64_t key() const noexcept {
        return (static_cast<uint64_t>(ids) << 40) | (static_cast<uint64_t>(classes) << 20) | types;
    }
};

struct ComplexSelector {
    std::vector<CompoundSelector> parts;   // leftmost first
    Specificity specificity;
};

// False if any part of the list is invalid: the whole rule is then dropped.
[[nodiscard]] bool parseSelectorList(const std::vector<Token>& tokens, std::vector<ComplexSelector>& out);

// ── Stylesheets ───────────────────────────────────────────────────────────────

struct Rule {
    std::vector<ComplexSelector> selectors;
    std::vector<Declaration> declarations;
    // Enclosing @media queries, all of which must match.
    std::vector<std::vector<Token>> media;
};

struct FontFaceRule {
    std::vector<Declaration> declarations;
    std::string unicode_range;   // as written: it doesn't survive being tokenized
    std::vector<std::vector<Token>> media;
};

struct Stylesheet {
    std::vector<Rule> rules;
    std::vector<FontFaceRule> font_faces;
    std::vector<std::string> imports;   // @import URLs, in order
};

[[nodiscard]] Stylesheet parseStylesheet(std::string_view css);

struct MediaContext {
    float width = 0, height = 0;
};

// One @media prelude, or a <link media=...>'s value once tokenized.
[[nodiscard]] bool mediaMatches(const std::vector<Token>& query, const MediaContext& context);
// What @supports and CSS.supports() answer for a condition ("(display: grid)").
[[nodiscard]] bool supportsCondition(std::string_view condition);

}  // namespace fbx::xweb::css
