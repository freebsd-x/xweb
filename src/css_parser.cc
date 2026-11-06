// CSS Syntax 3 (tokenizer, rules, declarations) and Selectors 4 parsing.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "xweb/css.h"

namespace fbx::xweb::css {

namespace {

constexpr int kMaxBlockDepth = 64;   // nested blocks and :not(:is(...)) past this are dropped

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isHex(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool isNameStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (static_cast<unsigned char>(c) >= 0x80);
}
bool isName(char c) { return isNameStart(c) || isDigit(c) || c == '-'; }
bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
    return out;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Tokenizer {
public:
    explicit Tokenizer(std::string_view in) : in_(in) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        while (true) {
            Token t = next();
            if (t.type == TokenType::Eof) break;
            out.push_back(std::move(t));
        }
        return out;
    }

private:
    [[nodiscard]] char at(size_t off = 0) const { return pos_ + off < in_.size() ? in_[pos_ + off] : '\0'; }
    [[nodiscard]] bool done() const { return pos_ >= in_.size(); }

    [[nodiscard]] bool validEscape(size_t off = 0) const {
        return pos_ + off + 1 < in_.size() && at(off) == '\\' && at(off + 1) != '\n';
    }
    [[nodiscard]] bool startsIdent(size_t off = 0) const {
        const char c = at(off);
        if (pos_ + off >= in_.size()) return false;
        if (c == '-') return isNameStart(at(off + 1)) || at(off + 1) == '-' || validEscape(off + 1);
        return isNameStart(c) || validEscape(off);
    }
    [[nodiscard]] bool startsNumber() const {
        const char c = at();
        if (c == '+' || c == '-') return isDigit(at(1)) || (at(1) == '.' && isDigit(at(2)));
        if (c == '.') return isDigit(at(1));
        return isDigit(c);
    }

    void escape(std::string& out) {
        ++pos_;   // the backslash
        if (done()) {
            appendUtf8(out, 0xFFFD);
            return;
        }
        if (isHex(at())) {
            uint32_t cp = 0;
            for (int i = 0; i < 6 && isHex(at()); ++i) {
                const char c = at();
                cp = cp * 16 + static_cast<uint32_t>(isDigit(c) ? c - '0' : (c | 0x20) - 'a' + 10);
                ++pos_;
            }
            if (isWs(at())) ++pos_;
            appendUtf8(out, cp);
            return;
        }
        out += at();
        ++pos_;
    }

    std::string name() {
        std::string out;
        while (!done()) {
            if (isName(at())) {
                out += at();
                ++pos_;
            } else if (validEscape()) {
                escape(out);
            } else {
                break;
            }
        }
        return out;
    }

    void skipComments() {
        while (at() == '/' && at(1) == '*') {
            const size_t end = in_.find("*/", pos_ + 2);
            pos_ = end == std::string_view::npos ? in_.size() : end + 2;
        }
    }

    Token string(char quote) {
        Token t;
        t.type = TokenType::String;
        ++pos_;
        while (!done()) {
            const char c = at();
            if (c == quote) {
                ++pos_;
                return t;
            }
            if (c == '\n') {
                t.type = TokenType::BadString;
                return t;
            }
            if (c == '\\') {
                if (pos_ + 1 >= in_.size()) {
                    ++pos_;
                    continue;
                }
                if (at(1) == '\n') {
                    pos_ += 2;
                    continue;
                }
                escape(t.text);
                continue;
            }
            t.text += c;
            ++pos_;
        }
        return t;
    }

    Token number() {
        Token t;
        t.type = TokenType::Number;
        const size_t start = pos_;
        t.is_integer = true;
        if (at() == '+' || at() == '-') ++pos_;
        while (isDigit(at())) ++pos_;
        if (at() == '.' && isDigit(at(1))) {
            t.is_integer = false;
            ++pos_;
            while (isDigit(at())) ++pos_;
        }
        if ((at() == 'e' || at() == 'E') &&
            (isDigit(at(1)) || ((at(1) == '+' || at(1) == '-') && isDigit(at(2))))) {
            t.is_integer = false;
            pos_ += 2;
            while (isDigit(at())) ++pos_;
        }
        const std::string text(in_.substr(start, pos_ - start));
        t.number = std::strtod(text.c_str(), nullptr);
        if (!std::isfinite(t.number)) t.number = t.number > 0 ? 1e30 : -1e30;
        if (startsIdent()) {
            t.type = TokenType::Dimension;
            t.text = lower(name());
        } else if (at() == '%') {
            ++pos_;
            t.type = TokenType::Percentage;
        }
        return t;
    }

    Token urlRest() {
        Token t;
        t.type = TokenType::Url;
        while (isWs(at())) ++pos_;
        while (!done()) {
            const char c = at();
            if (c == ')') {
                ++pos_;
                return t;
            }
            if (isWs(c)) {
                while (isWs(at())) ++pos_;
                if (at() == ')' || done()) {
                    if (!done()) ++pos_;
                    return t;
                }
                return badUrl();
            }
            if (c == '"' || c == '\'' || c == '(') return badUrl();
            if (c == '\\') {
                if (!validEscape()) return badUrl();
                escape(t.text);
                continue;
            }
            t.text += c;
            ++pos_;
        }
        return t;
    }

    Token badUrl() {
        Token t;
        t.type = TokenType::BadUrl;
        while (!done() && at() != ')') {
            if (validEscape()) {
                std::string ignored;
                escape(ignored);
            } else {
                ++pos_;
            }
        }
        if (!done()) ++pos_;
        return t;
    }

    Token identLike() {
        Token t;
        std::string n = name();
        if (at() == '(') {
            ++pos_;
            if (lower(n) == "url") {
                size_t look = pos_;
                while (look < in_.size() && isWs(in_[look])) ++look;
                if (look >= in_.size() || (in_[look] != '"' && in_[look] != '\'')) return urlRest();
            }
            t.type = TokenType::Function;
            t.text = lower(n);
            return t;
        }
        t.type = TokenType::Ident;
        t.text = std::move(n);
        return t;
    }

    static Token simple(TokenType type) {
        Token t;
        t.type = type;
        return t;
    }

    Token next() {
        skipComments();
        if (done()) return {};
        const char c = at();
        if (isWs(c)) {
            while (isWs(at())) ++pos_;
            return simple(TokenType::Whitespace);
        }
        if (c == '"' || c == '\'') return string(c);
        if (c == '#') {
            if (isName(at(1)) || validEscape(1)) {
                ++pos_;
                Token t;
                t.type = TokenType::Hash;
                t.is_id = startsIdent();
                t.text = name();
                return t;
            }
        } else if (startsNumber()) {
            return number();
        } else if (c == '-' && at(1) == '-' && at(2) == '>') {
            pos_ += 3;
            return simple(TokenType::Cdc);
        } else if (c == '<' && at(1) == '!' && at(2) == '-' && at(3) == '-') {
            pos_ += 4;
            return simple(TokenType::Cdo);
        } else if (c == '@' && startsIdent(1)) {
            ++pos_;
            Token t;
            t.type = TokenType::AtKeyword;
            t.text = lower(name());
            return t;
        } else if (startsIdent()) {
            return identLike();
        }
        TokenType single = TokenType::Delim;
        switch (c) {
            case '(': single = TokenType::LeftParen; break;
            case ')': single = TokenType::RightParen; break;
            case ',': single = TokenType::Comma; break;
            case ':': single = TokenType::Colon; break;
            case ';': single = TokenType::Semicolon; break;
            case '[': single = TokenType::LeftBracket; break;
            case ']': single = TokenType::RightBracket; break;
            case '{': single = TokenType::LeftBrace; break;
            case '}': single = TokenType::RightBrace; break;
            default: break;
        }
        ++pos_;
        Token t;
        t.type = single;
        if (single == TokenType::Delim) t.text = std::string(1, c);
        return t;
    }

    std::string_view in_;
    size_t pos_ = 0;
};

// ── Token stream helpers ──────────────────────────────────────────────────────

class Stream {
public:
    explicit Stream(const std::vector<Token>& tokens, size_t begin = 0, size_t end = std::string::npos)
        : t_(tokens), pos_(begin), end_(std::min(end, tokens.size())) {}

    [[nodiscard]] bool done() const { return pos_ >= end_; }
    [[nodiscard]] const Token& peek() const { return t_[pos_]; }
    [[nodiscard]] TokenType type() const { return done() ? TokenType::Eof : t_[pos_].type; }
    const Token& take() { return t_[pos_++]; }
    void skipWs() {
        while (!done() && t_[pos_].type == TokenType::Whitespace) ++pos_;
    }
    [[nodiscard]] size_t pos() const { return pos_; }
    [[nodiscard]] size_t end() const { return end_; }
    [[nodiscard]] const std::vector<Token>& tokens() const { return t_; }
    void seek(size_t p) { pos_ = p; }

    // Past one component value (a whole block, if that's what starts here).
    void skipComponent() {
        if (done()) return;
        const TokenType open = t_[pos_].type;
        ++pos_;
        if (open != TokenType::LeftBrace && open != TokenType::LeftBracket && open != TokenType::LeftParen &&
            open != TokenType::Function)
            return;
        std::vector<TokenType> closers{closerOf(open)};
        while (!done() && !closers.empty()) {
            const TokenType ty = t_[pos_].type;
            ++pos_;
            if (ty == closers.back()) closers.pop_back();
            else if (ty == TokenType::LeftBrace || ty == TokenType::LeftBracket || ty == TokenType::LeftParen ||
                     ty == TokenType::Function)
                closers.push_back(closerOf(ty));
        }
    }

    [[nodiscard]] static TokenType closerOf(TokenType open) {
        if (open == TokenType::LeftBrace) return TokenType::RightBrace;
        if (open == TokenType::LeftBracket) return TokenType::RightBracket;
        return TokenType::RightParen;
    }

private:
    const std::vector<Token>& t_;
    size_t pos_, end_;
};

// ── Declarations ──────────────────────────────────────────────────────────────

void parseDeclarationsInto(const std::vector<Token>& tokens, size_t begin, size_t end, std::vector<Declaration>& out) {
    Stream s(tokens, begin, end);
    while (!s.done()) {
        s.skipWs();
        if (s.done()) break;
        if (s.type() == TokenType::Semicolon) {
            s.take();
            continue;
        }
        if (s.type() != TokenType::Ident) {   // bad declaration, or a nested rule: skip it
            while (!s.done() && s.type() != TokenType::Semicolon) {
                const bool block = s.type() == TokenType::LeftBrace;
                s.skipComponent();
                if (block) break;
            }
            continue;
        }
        const size_t start = s.pos();
        Declaration d;
        d.name = s.take().text;
        s.skipWs();
        if (s.type() != TokenType::Colon) {
            s.seek(start);
            while (!s.done() && s.type() != TokenType::Semicolon) {
                const bool block = s.type() == TokenType::LeftBrace;
                s.skipComponent();
                if (block) break;
            }
            continue;
        }
        s.take();
        if (!d.name.starts_with("--")) d.name = lower(d.name);
        s.skipWs();
        const size_t value_begin = s.pos();
        bool nested_rule = false;
        while (!s.done() && s.type() != TokenType::Semicolon) {
            if (s.type() == TokenType::LeftBrace && !d.name.starts_with("--")) nested_rule = true;
            s.skipComponent();
            if (nested_rule) break;
        }
        if (nested_rule) continue;
        size_t value_end = s.pos();
        while (value_end > value_begin && tokens[value_end - 1].type == TokenType::Whitespace) --value_end;
        // A trailing "! important"
        if (value_end > value_begin && tokens[value_end - 1].type == TokenType::Ident &&
            lower(tokens[value_end - 1].text) == "important") {
            size_t k = value_end - 1;
            while (k > value_begin && tokens[k - 1].type == TokenType::Whitespace) --k;
            if (k > value_begin && tokens[k - 1].type == TokenType::Delim && tokens[k - 1].text == "!") {
                d.important = true;
                value_end = k - 1;
                while (value_end > value_begin && tokens[value_end - 1].type == TokenType::Whitespace) --value_end;
            }
        }
        d.value.assign(tokens.begin() + static_cast<std::ptrdiff_t>(value_begin),
                       tokens.begin() + static_cast<std::ptrdiff_t>(value_end));
        out.push_back(std::move(d));
    }
}

// ── Selectors ─────────────────────────────────────────────────────────────────

class SelectorParser {
public:
    SelectorParser(const std::vector<Token>& t, size_t begin, size_t end, int depth) : s_(t, begin, end), depth_(depth) {}

    bool list(std::vector<ComplexSelector>& out) {
        while (true) {
            ComplexSelector c;
            if (!complex(c)) return false;
            out.push_back(std::move(c));
            s_.skipWs();
            if (s_.done()) return true;
            if (s_.type() != TokenType::Comma) return false;
            s_.take();
        }
    }

private:
    bool complex(ComplexSelector& out) {
        s_.skipWs();
        Combinator next = Combinator::Descendant;
        while (true) {
            CompoundSelector part;
            part.combinator = next;
            if (!compound(part)) return false;
            out.parts.push_back(std::move(part));
            const bool had_ws = s_.type() == TokenType::Whitespace;
            s_.skipWs();
            if (s_.done() || s_.type() == TokenType::Comma) break;
            if (s_.type() == TokenType::Delim && (s_.peek().text == ">" || s_.peek().text == "+" || s_.peek().text == "~")) {
                const char c = s_.take().text[0];
                next = c == '>' ? Combinator::Child : c == '+' ? Combinator::NextSibling : Combinator::SubsequentSibling;
                s_.skipWs();
            } else if (had_ws) {
                next = Combinator::Descendant;
            } else {
                return false;
            }
        }
        out.specificity = specificityOf(out);
        return true;
    }

    static Specificity specificityOf(const ComplexSelector& c) {
        Specificity sp;
        for (const auto& part : c.parts) {
            for (const auto& ss : part.simple) {
                switch (ss.kind) {
                    case SimpleKind::Id: ++sp.ids; break;
                    case SimpleKind::Class:
                    case SimpleKind::Attribute: ++sp.classes; break;
                    case SimpleKind::Type:
                    case SimpleKind::PseudoElement: ++sp.types; break;
                    case SimpleKind::PseudoClass: {
                        if (ss.name == "where") break;
                        if (ss.name == "not" || ss.name == "is" || ss.name == "has") {
                            // Takes on the most specific of its arguments.
                            Specificity best;
                            for (const auto& a : ss.args)
                                if (a.specificity.key() > best.key()) best = a.specificity;
                            sp.ids += best.ids;
                            sp.classes += best.classes;
                            sp.types += best.types;
                            break;
                        }
                        ++sp.classes;
                        break;
                    }
                    case SimpleKind::Universal: break;
                }
            }
        }
        return sp;
    }

    bool compound(CompoundSelector& out) {
        // An optional type or universal first.
        if (s_.type() == TokenType::Ident || (s_.type() == TokenType::Delim && s_.peek().text == "*")) {
            SimpleSelector ss;
            if (s_.type() == TokenType::Ident) {
                ss.kind = SimpleKind::Type;
                ss.name = lower(s_.take().text);
            } else {
                s_.take();
                ss.kind = SimpleKind::Universal;
            }
            // Namespaces ("svg|rect") aren't supported, and neither is the rule.
            if (s_.type() == TokenType::Delim && s_.peek().text == "|") return false;
            out.simple.push_back(std::move(ss));
        }
        while (!s_.done()) {
            const TokenType ty = s_.type();
            if (ty == TokenType::Hash) {
                if (!s_.peek().is_id) return false;
                SimpleSelector ss;
                ss.kind = SimpleKind::Id;
                ss.name = s_.take().text;
                out.simple.push_back(std::move(ss));
            } else if (ty == TokenType::Delim && s_.peek().text == ".") {
                s_.take();
                if (s_.type() != TokenType::Ident) return false;
                SimpleSelector ss;
                ss.kind = SimpleKind::Class;
                ss.name = s_.take().text;
                out.simple.push_back(std::move(ss));
            } else if (ty == TokenType::LeftBracket) {
                if (!attribute(out)) return false;
            } else if (ty == TokenType::Colon) {
                if (!pseudo(out)) return false;
            } else {
                break;
            }
        }
        return !out.simple.empty();
    }

    bool attribute(CompoundSelector& out) {
        s_.take();
        s_.skipWs();
        if (s_.type() != TokenType::Ident) return false;
        SimpleSelector ss;
        ss.kind = SimpleKind::Attribute;
        ss.name = lower(s_.take().text);
        s_.skipWs();
        if (s_.type() == TokenType::RightBracket) {
            s_.take();
            out.simple.push_back(std::move(ss));
            return true;
        }
        if (s_.type() != TokenType::Delim) return false;
        const std::string op = s_.take().text;
        if (op == "=") {
            ss.op = AttrOp::Equals;
        } else {
            if (s_.type() != TokenType::Delim || s_.peek().text != "=") return false;
            s_.take();
            if (op == "~") ss.op = AttrOp::Includes;
            else if (op == "|") ss.op = AttrOp::DashMatch;
            else if (op == "^") ss.op = AttrOp::Prefix;
            else if (op == "$") ss.op = AttrOp::Suffix;
            else if (op == "*") ss.op = AttrOp::Substring;
            else return false;
        }
        s_.skipWs();
        if (s_.type() != TokenType::Ident && s_.type() != TokenType::String) return false;
        ss.value = s_.take().text;
        s_.skipWs();
        if (s_.type() == TokenType::Ident) {
            const std::string flag = lower(s_.take().text);
            if (flag == "i") ss.case_insensitive = true;
            else if (flag != "s") return false;
            s_.skipWs();
        }
        if (s_.type() != TokenType::RightBracket) return false;
        s_.take();
        out.simple.push_back(std::move(ss));
        return true;
    }

    // "an+b" from a :nth-child(...) argument, as text: its tokens split it oddly.
    static bool parseNth(std::string text, int& a, int& b) {
        text.erase(std::remove_if(text.begin(), text.end(), isWs), text.end());
        text = lower(text);
        if (text == "odd") { a = 2; b = 1; return true; }
        if (text == "even") { a = 2; b = 0; return true; }
        const size_t n = text.find('n');
        const auto toInt = [](std::string_view v, int& into) {
            if (v.starts_with('+')) v.remove_prefix(1);
            const auto r = std::from_chars(v.data(), v.data() + v.size(), into);
            return r.ec == std::errc{} && r.ptr == v.data() + v.size();
        };
        if (n == std::string::npos) {
            a = 0;
            return toInt(text, b);
        }
        const std::string_view coeff = std::string_view(text).substr(0, n);
        if (coeff.empty() || coeff == "+") a = 1;
        else if (coeff == "-") a = -1;
        else if (!toInt(coeff, a)) return false;
        const std::string_view rest = std::string_view(text).substr(n + 1);
        if (rest.empty()) {
            b = 0;
            return true;
        }
        return toInt(rest, b);
    }

    bool pseudo(CompoundSelector& out) {
        s_.take();
        bool element = false;
        if (s_.type() == TokenType::Colon) {
            s_.take();
            element = true;
        }
        SimpleSelector ss;
        ss.kind = element ? SimpleKind::PseudoElement : SimpleKind::PseudoClass;
        if (s_.type() == TokenType::Ident) {
            ss.name = lower(s_.take().text);
            static constexpr std::string_view legacy[] = {"before", "after", "first-line", "first-letter"};
            if (!element && std::find(std::begin(legacy), std::end(legacy), ss.name) != std::end(legacy))
                ss.kind = SimpleKind::PseudoElement;
            out.simple.push_back(std::move(ss));
            return true;
        }
        if (s_.type() != TokenType::Function) return false;
        ss.name = s_.peek().text;
        const size_t fn = s_.pos();
        s_.skipComponent();
        const size_t arg_begin = fn + 1;
        const size_t arg_end = s_.pos() - 1;
        const auto& tokens = s_.tokens();
        if (depth_ >= kMaxBlockDepth) return false;
        if (ss.name == "not" || ss.name == "is" || ss.name == "where" || ss.name == "matches" ||
            ss.name == "-webkit-any" || ss.name == "-moz-any") {
            if (ss.name != "not" && ss.name != "where") ss.name = "is";
            SelectorParser inner(tokens, arg_begin, arg_end, depth_ + 1);
            if (!inner.list(ss.args)) {
                if (ss.name == "not") return false;
                ss.args.clear();   // :is() and :where() forgive: this matches nothing
            }
        } else if (ss.name.starts_with("nth-")) {
            std::string text;
            for (size_t i = arg_begin; i < arg_end && i < tokens.size(); ++i) {
                const Token& t = tokens[i];
                if (t.type == TokenType::Ident) {
                    if (lower(t.text) == "of") return false;   // "of S" isn't supported
                    text += t.text;
                } else if (t.type == TokenType::Number) {
                    if (t.number >= 0 && !text.empty() && text.back() != '+' && text.back() != '-') text += "+";
                    text += std::to_string(static_cast<int>(t.number));
                } else if (t.type == TokenType::Dimension) {
                    text += std::to_string(static_cast<int>(t.number)) + t.text;
                } else if (t.type == TokenType::Delim) {
                    text += t.text;
                }
            }
            if (!parseNth(text, ss.a, ss.b)) return false;
        } else if (ss.name == "lang" || ss.name == "dir") {
            if (arg_begin < arg_end && arg_begin < tokens.size()) ss.value = tokens[arg_begin].text;
        } else {
            return false;   // :has() and anything else we can't evaluate: drop the rule
        }
        out.simple.push_back(std::move(ss));
        return true;
    }

    Stream s_;
    int depth_;
};

// ── Rules ─────────────────────────────────────────────────────────────────────

class SheetParser {
public:
    explicit SheetParser(const std::vector<Token>& tokens) : tokens_(tokens) {}

    void parseRange(size_t begin, size_t end, std::vector<std::vector<Token>>& media, Stylesheet& out, int depth) {
        Stream s(tokens_, begin, end);
        while (!s.done()) {
            const TokenType ty = s.type();
            if (ty == TokenType::Whitespace || ty == TokenType::Cdo || ty == TokenType::Cdc) {
                s.take();
            } else if (ty == TokenType::AtKeyword) {
                atRule(s, media, out, depth);
            } else {
                qualifiedRule(s, media, out);
            }
        }
    }

private:
    // Finds this at-rule's prelude end and block, if any.
    static bool findBlock(Stream& s, size_t& block_begin, size_t& block_end) {
        while (!s.done()) {
            if (s.type() == TokenType::Semicolon) {
                s.take();
                return false;
            }
            if (s.type() == TokenType::LeftBrace) {
                block_begin = s.pos() + 1;
                s.skipComponent();
                block_end = s.pos() > block_begin ? s.pos() - 1 : block_begin;
                return true;
            }
            s.skipComponent();
        }
        return false;
    }

    void atRule(Stream& s, std::vector<std::vector<Token>>& media, Stylesheet& out, int depth) {
        const std::string name = s.take().text;
        const size_t prelude = s.pos();
        size_t begin = 0, end = 0;
        const bool has_block = findBlock(s, begin, end);
        const size_t prelude_end = has_block ? begin - 1 : s.pos();
        const auto preludeTokens = [&] {
            size_t e = prelude_end;
            if (!has_block && e > prelude && tokens_[e - 1].type == TokenType::Semicolon) --e;
            return std::vector<Token>(tokens_.begin() + static_cast<std::ptrdiff_t>(prelude),
                                      tokens_.begin() + static_cast<std::ptrdiff_t>(e));
        };
        if (name == "import" && !has_block) {
            for (const Token& t : preludeTokens()) {
                if (t.type == TokenType::String || t.type == TokenType::Url) {
                    out.imports.push_back(t.text);
                    break;
                }
            }
            return;
        }
        if (!has_block || depth >= kMaxBlockDepth) return;
        if (name == "media") {
            media.push_back(preludeTokens());
            parseRange(begin, end, media, out, depth + 1);
            media.pop_back();
        } else if (name == "supports") {
            // We can't honestly answer most queries yet. A "not" is usually a
            // fallback for browsers lacking something; the rest we try to obey.
            const auto prelude_toks = preludeTokens();
            const bool negated = std::any_of(prelude_toks.begin(), prelude_toks.end(), [](const Token& t) {
                return t.type == TokenType::Ident && lower(t.text) == "not";
            });
            if (!negated) parseRange(begin, end, media, out, depth + 1);
        } else if (name == "layer" || name == "container" || name == "scope") {
            // Rules inside still apply, without the layer's or container's conditions.
            if (name == "layer") parseRange(begin, end, media, out, depth + 1);
        }
        // @font-face, @keyframes, @page, @counter-style...: nothing uses them yet.
    }

    void qualifiedRule(Stream& s, std::vector<std::vector<Token>>& media, Stylesheet& out) {
        const size_t begin = s.pos();
        while (!s.done() && s.type() != TokenType::LeftBrace) s.skipComponent();
        if (s.done()) return;   // no block: not a rule
        const size_t prelude_end = s.pos();
        const size_t block = s.pos() + 1;
        s.skipComponent();
        const size_t block_end = s.pos() > block ? s.pos() - 1 : block;
        Rule rule;
        SelectorParser selectors(tokens_, begin, prelude_end, 0);
        if (!selectors.list(rule.selectors)) return;
        parseDeclarationsInto(tokens_, block, block_end, rule.declarations);
        if (rule.declarations.empty()) return;
        rule.media = media;
        out.rules.push_back(std::move(rule));
    }

    const std::vector<Token>& tokens_;
};

}  // namespace

std::vector<Token> tokenize(std::string_view css) { return Tokenizer(css).run(); }

std::vector<Declaration> parseDeclarations(std::string_view css) {
    const std::vector<Token> tokens = tokenize(css);
    std::vector<Declaration> out;
    parseDeclarationsInto(tokens, 0, tokens.size(), out);
    return out;
}

bool parseSelectorList(const std::vector<Token>& tokens, std::vector<ComplexSelector>& out) {
    return SelectorParser(tokens, 0, tokens.size(), 0).list(out);
}

Stylesheet parseStylesheet(std::string_view css) {
    const std::vector<Token> tokens = tokenize(css);
    Stylesheet out;
    std::vector<std::vector<Token>> media;
    SheetParser(tokens).parseRange(0, tokens.size(), media, out, 0);
    return out;
}

// ── Media queries ─────────────────────────────────────────────────────────────

namespace {

float lengthPx(const Token& t) {
    if (t.type == TokenType::Number) return static_cast<float>(t.number);
    if (t.type != TokenType::Dimension) return -1;
    const auto v = static_cast<float>(t.number);
    if (t.text == "px") return v;
    if (t.text == "em" || t.text == "rem") return v * 16.0f;
    if (t.text == "pt") return v * 96.0f / 72.0f;
    if (t.text == "in") return v * 96.0f;
    if (t.text == "cm") return v * 96.0f / 2.54f;
    return -1;
}

bool featureMatches(const std::string& name, const Token* value, const MediaContext& c) {
    const auto compare = [&](float actual) {
        if (!value) return true;
        const float want = lengthPx(*value);
        if (want < 0) return false;
        if (name.starts_with("min-")) return actual >= want;
        if (name.starts_with("max-")) return actual <= want;
        return actual == want;
    };
    if (name == "width" || name == "min-width" || name == "max-width" || name == "device-width" ||
        name == "min-device-width" || name == "max-device-width")
        return compare(c.width);
    if (name == "height" || name == "min-height" || name == "max-height" || name == "device-height" ||
        name == "min-device-height" || name == "max-device-height")
        return compare(c.height);
    const std::string v = value && value->type == TokenType::Ident ? lower(value->text) : "";
    if (name == "prefers-color-scheme") return v == "light";
    if (name == "prefers-reduced-motion") return v == "no-preference";
    if (name == "hover" || name == "any-hover") return v == "hover" || !value;
    if (name == "pointer" || name == "any-pointer") return v == "fine" || !value;
    if (name == "orientation") return v == (c.width >= c.height ? "landscape" : "portrait");
    if (name == "color") return true;
    return false;
}

}  // namespace

bool mediaMatches(const std::vector<Token>& query, const MediaContext& context) {
    // A comma-separated list: any one that matches will do.
    bool any = false;
    bool saw_part = false;
    bool ok = true, negate = false, valid = true;
    const auto finish = [&] {
        if (saw_part && valid && (ok != negate)) any = true;
        ok = true;
        negate = false;
        valid = true;
        saw_part = false;
    };
    Stream s(query);
    while (!s.done()) {
        const Token& t = s.peek();
        if (t.type == TokenType::Comma) {
            s.take();
            finish();
        } else if (t.type == TokenType::Whitespace) {
            s.take();
        } else if (t.type == TokenType::Ident) {
            const std::string word = lower(t.text);
            s.take();
            saw_part = true;
            if (word == "not") negate = true;
            else if (word == "only" || word == "and") {}
            else if (word == "screen" || word == "all") {}
            else ok = false;   // print, speech, and the unknown
        } else if (t.type == TokenType::LeftParen) {
            const size_t open = s.pos();
            s.skipComponent();
            saw_part = true;
            std::string name;
            const Token* value = nullptr;
            for (size_t i = open + 1; i + 1 < s.pos(); ++i) {
                const Token& inner = query[i];
                if (inner.type == TokenType::Whitespace || inner.type == TokenType::Colon) continue;
                if (name.empty() && inner.type == TokenType::Ident) name = lower(inner.text);
                else if (!value) value = &inner;
            }
            if (!featureMatches(name, value, context)) ok = false;
        } else {
            s.skipComponent();
            valid = false;   // syntax we don't read: "not all", per the spec
            saw_part = true;
        }
    }
    finish();
    return any || query.empty();
}

}  // namespace fbx::xweb::css
