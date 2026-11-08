// The JavaScript lexer: ECMAScript's input elements, read from UTF-16 source.

#include <cmath>
#include <cstdlib>

#include "ast.h"
#include "xweb/js.h"

namespace fbx::xweb::js {

bool isLineTerminator(char16_t c) { return c == u'\n' || c == u'\r' || c == 0x2028 || c == 0x2029; }

bool isJsSpace(char16_t c) {
    return c == u' ' || c == u'\t' || c == 0x0B || c == 0x0C || c == 0xA0 || c == 0xFEFF || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000;
}

bool isIdStart(char32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_';
    // Beyond ASCII: letters, near enough. Spaces and separators are what isn't.
    return !(c == 0xA0 || c == 0xFEFF || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
             c == 0x202F || c == 0x205F || c == 0x3000 || (c >= 0x2010 && c <= 0x205E));
}

bool isIdPart(char32_t c) {
    return isIdStart(c) || (c >= '0' && c <= '9') || c == 0x200C || c == 0x200D;
}

namespace {

int hexValue(char16_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void appendCodePoint(std::u16string& out, char32_t cp) {
    if (cp < 0x10000) {
        out += static_cast<char16_t>(cp);
    } else {
        cp -= 0x10000;
        out += static_cast<char16_t>(0xD800 + (cp >> 10));
        out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
    }
}

bool isKeyword(std::u16string_view w) {
    static const char16_t* kWords[] = {
        u"break", u"case", u"catch", u"class", u"const", u"continue", u"debugger", u"default", u"delete", u"do",
        u"else", u"export", u"extends", u"finally", u"for", u"function", u"if", u"import", u"in", u"instanceof",
        u"new", u"return", u"super", u"switch", u"this", u"throw", u"try", u"typeof", u"var", u"void", u"while",
        u"with", u"null", u"true", u"false", u"enum"};
    for (const char16_t* k : kWords)
        if (w == k) return true;
    return false;
}

}  // namespace

bool Lexer::skipSpace(bool& newline) {
    while (i_ < s_.size()) {
        const char16_t c = s_[i_];
        if (isJsSpace(c)) {
            ++i_;
        } else if (isLineTerminator(c)) {
            if (c == u'\r' && i_ + 1 < s_.size() && s_[i_ + 1] == u'\n') ++i_;
            ++i_;
            ++line_;
            newline = true;
        } else if (c == u'/' && i_ + 1 < s_.size() && s_[i_ + 1] == u'/') {
            while (i_ < s_.size() && !isLineTerminator(s_[i_])) ++i_;
        } else if (c == u'/' && i_ + 1 < s_.size() && s_[i_ + 1] == u'*') {
            i_ += 2;
            bool closed = false;
            while (i_ < s_.size()) {
                if (s_[i_] == u'*' && i_ + 1 < s_.size() && s_[i_ + 1] == u'/') {
                    i_ += 2;
                    closed = true;
                    break;
                }
                if (isLineTerminator(s_[i_])) {
                    if (s_[i_] == u'\r' && i_ + 1 < s_.size() && s_[i_ + 1] == u'\n') ++i_;
                    ++line_;
                    newline = true;
                }
                ++i_;
            }
            if (!closed) {
                error = "unterminated comment";
                return false;
            }
        } else if (c == u'<' && s_.substr(i_, 4) == u"<!--") {
            // HTML-like comments, as browsers still allow in scripts (not modules).
            if (module) {
                error = "HTML-like comments aren't allowed in modules";
                return false;
            }
            while (i_ < s_.size() && !isLineTerminator(s_[i_])) ++i_;
        } else if (c == u'-' && newline && s_.substr(i_, 3) == u"-->") {
            if (module) {
                error = "HTML-like comments aren't allowed in modules";
                return false;
            }
            while (i_ < s_.size() && !isLineTerminator(s_[i_])) ++i_;
        } else {
            break;
        }
    }
    return true;
}

Token Lexer::next(bool regex_allowed) {
    Token t;
    bool newline = i_ == 0;
    if (!skipSpace(newline)) {
        t.type = Tok::Eof;
        return t;
    }
    t.newline_before = newline;
    t.line = line_;
    t.begin = static_cast<uint32_t>(i_);
    if (i_ >= s_.size()) {
        t.type = Tok::Eof;
        t.end = t.begin;
        return t;
    }
    const char16_t c = s_[i_];
    // Identifiers, keywords and private names.
    if (c == u'#' && i_ + 1 < s_.size() && (isIdStart(s_[i_ + 1]) || s_[i_ + 1] == u'\\')) {
        ++i_;
        t = identifier(t);
        t.type = Tok::PrivateName;
        return t;
    }
    if (isIdStart(c) || c == u'\\' || (c >= 0xD800 && c <= 0xDBFF)) return identifier(t);
    if ((c >= u'0' && c <= u'9') || (c == u'.' && i_ + 1 < s_.size() && s_[i_ + 1] >= u'0' && s_[i_ + 1] <= u'9')) return numberLiteral(t);
    if (c == u'"' || c == u'\'') return stringLiteral(t, c);
    if (c == u'`') {
        ++i_;
        return templateLiteral(t);
    }
    if (c == u'/' && regex_allowed) return regexLiteral(t);
    // Punctuators, longest first.
    static const char16_t* kPuncts[] = {
        u">>>=", u"...", u"===", u"!==", u"**=", u"<<=", u">>=", u">>>", u"&&=", u"||=", u"?\?=",
        u"=>", u"==", u"!=", u"<=", u">=", u"&&", u"||", u"??", u"?.", u"++", u"--", u"+=", u"-=", u"*=", u"/=",
        u"%=", u"&=", u"|=", u"^=", u"<<", u">>", u"**",
        u"{", u"}", u"(", u")", u"[", u"]", u";", u",", u"<", u">", u"+", u"-", u"*", u"/", u"%", u"&", u"|",
        u"^", u"!", u"~", u"?", u":", u"=", u".", u"@"};
    for (const char16_t* p : kPuncts) {
        const std::u16string_view pv(p);
        if (s_.substr(i_, pv.size()) == pv) {
            // "?." followed by a digit is "?" then a number (a ? .5 : 1).
            if (pv == u"?." && i_ + 2 < s_.size() && s_[i_ + 2] >= u'0' && s_[i_ + 2] <= u'9') continue;
            t.type = Tok::Punct;
            t.value = pv;
            i_ += pv.size();
            t.end = static_cast<uint32_t>(i_);
            return t;
        }
    }
    error = "unexpected character";
    t.type = Tok::Eof;
    return t;
}

Token Lexer::identifier(Token t) {
    std::u16string name;
    while (i_ < s_.size()) {
        char32_t cp = s_[i_];
        size_t width = 1;
        if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < s_.size() && s_[i_ + 1] >= 0xDC00 && s_[i_ + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s_[i_ + 1] - 0xDC00);
            width = 2;
        }
        if (cp == u'\\') {
            // \uXXXX or \u{X...} in a name.
            if (i_ + 1 >= s_.size() || s_[i_ + 1] != u'u') {
                error = "invalid escape in identifier";
                break;
            }
            i_ += 2;
            char32_t v = 0;
            if (i_ < s_.size() && s_[i_] == u'{') {
                ++i_;
                int digits = 0;
                while (i_ < s_.size() && s_[i_] != u'}' && hexValue(s_[i_]) >= 0 && digits < 8) v = v * 16 + hexValue(s_[i_++]), ++digits;
                if (i_ >= s_.size() || s_[i_] != u'}' || v > 0x10FFFF || digits == 0) {
                    error = "invalid escape in identifier";
                    break;
                }
                ++i_;
            } else {
                for (int k = 0; k < 4; ++k) {
                    if (i_ >= s_.size() || hexValue(s_[i_]) < 0) {
                        error = "invalid escape in identifier";
                        break;
                    }
                    v = v * 16 + hexValue(s_[i_++]);
                }
            }
            if (!error.empty()) break;
            if (name.empty() ? !isIdStart(v) : !isIdPart(v)) {
                error = "invalid character in identifier";
                break;
            }
            t.escaped = true;
            appendCodePoint(name, v);
            continue;
        }
        if (name.empty() ? !isIdStart(cp) : !isIdPart(cp)) break;
        name.append(s_.substr(i_, width));
        i_ += width;
    }
    t.end = static_cast<uint32_t>(i_);
    t.value = std::move(name);
    t.type = !t.escaped && isKeyword(t.value) ? Tok::Keyword : Tok::Identifier;
    if (!error.empty()) t.type = Tok::Eof;
    return t;
}

Token Lexer::numberLiteral(Token t) {
    t.type = Tok::Number;
    const size_t start = i_;
    std::string digits;
    const auto readDigits = [&](auto ok) {
        bool any = false, last_sep = false;
        while (i_ < s_.size()) {
            const char16_t c = s_[i_];
            if (c == u'_' && any && !last_sep) {
                last_sep = true;
                ++i_;
                continue;
            }
            if (!ok(c)) break;
            digits += static_cast<char>(c);
            any = true;
            last_sep = false;
            ++i_;
        }
        if (last_sep) error = "numeric separator at the end of a number";
        return any;
    };
    const auto isDec = [](char16_t c) { return c >= u'0' && c <= u'9'; };
    if (s_[i_] == u'0' && i_ + 1 < s_.size() && (s_[i_ + 1] == u'x' || s_[i_ + 1] == u'X' || s_[i_ + 1] == u'o' || s_[i_ + 1] == u'O' ||
                                                 s_[i_ + 1] == u'b' || s_[i_ + 1] == u'B')) {
        const char16_t kind = static_cast<char16_t>(s_[i_ + 1] | 0x20);
        const int base = kind == u'x' ? 16 : kind == u'o' ? 8 : 2;
        i_ += 2;
        if (!readDigits([&](char16_t c) { const int v = hexValue(c); return v >= 0 && v < base; })) error = "missing digits";
        double v = 0;
        for (char d : digits) v = v * base + hexValue(d);
        t.number = v;
    } else if (s_[i_] == u'0' && i_ + 1 < s_.size() && isDec(s_[i_ + 1])) {
        // A legacy octal (or, with an 8 or 9 in it, decimal) literal.
        ++i_;
        readDigits(isDec);
        const bool octal = digits.find_first_of("89") == std::string::npos;
        double v = 0;
        for (char d : digits) v = v * (octal ? 8 : 10) + (d - '0');
        t.number = v;
        t.legacy_octal = true;
    } else {
        readDigits(isDec);
        if (i_ < s_.size() && s_[i_] == u'.') {
            digits += '.';
            ++i_;
            readDigits(isDec);
        }
        if (i_ < s_.size() && (s_[i_] == u'e' || s_[i_] == u'E')) {
            digits += 'e';
            ++i_;
            if (i_ < s_.size() && (s_[i_] == u'+' || s_[i_] == u'-')) digits += static_cast<char>(s_[i_++]);
            if (!readDigits(isDec)) error = "missing exponent";
        }
        t.number = std::strtod(digits.c_str(), nullptr);
    }
    if (i_ < s_.size() && s_[i_] == u'n') {
        // A BigInt: an integer, without a legacy octal's leading zero.
        const bool hex = s_.substr(start, 2) == u"0x" || s_.substr(start, 2) == u"0X";
        if (t.legacy_octal || digits.find('.') != std::string::npos || (!hex && digits.find('e') != std::string::npos))
            error = "a BigInt literal must be an integer";
        t.bigint = true;
        const std::u16string_view written = s_.substr(start, i_ - start);
        for (char16_t c : written)
            if (c != u'_') t.value += c;
        ++i_;
    }
    if (i_ < s_.size() && (isIdStart(s_[i_]) || isDec(s_[i_]))) error = "identifier starts right after a number";
    if (!error.empty()) t.type = Tok::Eof;
    t.end = static_cast<uint32_t>(i_);
    (void)start;
    return t;
}

bool Lexer::escape(std::u16string& out, bool in_template, bool& ok) {
    // At the character after the backslash.
    if (i_ >= s_.size()) return false;
    const char16_t c = s_[i_++];
    switch (c) {
        case u'n': out += u'\n'; return true;
        case u't': out += u'\t'; return true;
        case u'r': out += u'\r'; return true;
        case u'b': out += u'\b'; return true;
        case u'f': out += u'\f'; return true;
        case u'v': out += u'\v'; return true;
        case u'\r':
            if (i_ < s_.size() && s_[i_] == u'\n') ++i_;
            ++line_;
            return true;
        case u'\n':
        case 0x2028:
        case 0x2029:
            ++line_;
            return true;
        case u'x': {
            if (i_ + 1 < s_.size() && hexValue(s_[i_]) >= 0 && hexValue(s_[i_ + 1]) >= 0) {
                out += static_cast<char16_t>(hexValue(s_[i_]) * 16 + hexValue(s_[i_ + 1]));
                i_ += 2;
                return true;
            }
            ok = false;
            return in_template;
        }
        case u'u': {
            char32_t v = 0;
            if (i_ < s_.size() && s_[i_] == u'{') {
                ++i_;
                int digits = 0;
                while (i_ < s_.size() && hexValue(s_[i_]) >= 0) {
                    v = v * 16 + hexValue(s_[i_++]);
                    if (v > 0x10FFFF) break;
                    ++digits;
                }
                if (i_ >= s_.size() || s_[i_] != u'}' || digits == 0 || v > 0x10FFFF) {
                    ok = false;
                    return in_template;
                }
                ++i_;
            } else {
                for (int k = 0; k < 4; ++k) {
                    if (i_ >= s_.size() || hexValue(s_[i_]) < 0) {
                        ok = false;
                        return in_template;
                    }
                    v = v * 16 + hexValue(s_[i_++]);
                }
            }
            appendCodePoint(out, v);
            return true;
        }
        default:
            if (c >= u'0' && c <= u'7') {
                if (c == u'0' && !(i_ < s_.size() && s_[i_] >= u'0' && s_[i_] <= u'9')) {
                    out += u'\0';
                    return true;
                }
                if (in_template) {
                    ok = false;
                    return true;
                }
                // A legacy octal escape: up to three digits, value under 256.
                int v = c - u'0';
                for (int k = 0; k < 2 && i_ < s_.size() && s_[i_] >= u'0' && s_[i_] <= u'7' && v * 8 + (s_[i_] - u'0') < 256; ++k) v = v * 8 + (s_[i_++] - u'0');
                out += static_cast<char16_t>(v);
                return true;
            }
            if ((c == u'8' || c == u'9') && in_template) {
                ok = false;
                return true;
            }
            out += c;
            return true;
    }
}

Token Lexer::stringLiteral(Token t, char16_t quote) {
    t.type = Tok::String;
    ++i_;
    std::u16string value;
    while (true) {
        if (i_ >= s_.size()) {
            error = "unterminated string";
            t.type = Tok::Eof;
            return t;
        }
        const char16_t c = s_[i_];
        if (c == quote) {
            ++i_;
            break;
        }
        if (c == u'\n' || c == u'\r') {
            error = "unterminated string";
            t.type = Tok::Eof;
            return t;
        }
        if (c == u'\\') {
            ++i_;
            bool ok = true;
            if (!escape(value, false, ok) || !ok) {
                error = "invalid escape in string";
                t.type = Tok::Eof;
                return t;
            }
            continue;
        }
        value += c;
        ++i_;
    }
    t.value = std::move(value);
    t.end = static_cast<uint32_t>(i_);
    return t;
}

Token Lexer::templateLiteral(Token t) {
    // After the ` or the } that ends a substitution: up to ${ or the closing `.
    t.type = Tok::Template;
    std::u16string cooked, raw;
    bool ok = true;
    while (true) {
        if (i_ >= s_.size()) {
            error = "unterminated template";
            t.type = Tok::Eof;
            return t;
        }
        const char16_t c = s_[i_];
        if (c == u'`') {
            ++i_;
            t.template_tail = true;
            break;
        }
        if (c == u'$' && i_ + 1 < s_.size() && s_[i_ + 1] == u'{') {
            i_ += 2;
            break;
        }
        if (c == u'\\') {
            const size_t from = i_;
            ++i_;
            if (!escape(cooked, true, ok)) {
                error = "invalid escape in template";
                t.type = Tok::Eof;
                return t;
            }
            raw.append(s_.substr(from, i_ - from));
            continue;
        }
        if (c == u'\r') {
            // Line ends in a template are \n, raw or cooked.
            ++i_;
            if (i_ < s_.size() && s_[i_] == u'\n') ++i_;
            cooked += u'\n';
            raw += u'\n';
            ++line_;
            continue;
        }
        if (isLineTerminator(c)) ++line_;
        cooked += c;
        raw += c;
        ++i_;
    }
    t.value = std::move(cooked);
    t.raw = std::move(raw);
    t.cooked_ok = ok;
    t.end = static_cast<uint32_t>(i_);
    return t;
}

Token Lexer::templatePart() {
    Token t;
    t.line = line_;
    t.begin = static_cast<uint32_t>(i_);
    return templateLiteral(t);
}

Token Lexer::regexLiteral(Token t) {
    t.type = Tok::Regex;
    ++i_;
    std::u16string body;
    bool in_class = false;
    while (true) {
        if (i_ >= s_.size() || isLineTerminator(s_[i_])) {
            error = "unterminated regular expression";
            t.type = Tok::Eof;
            return t;
        }
        const char16_t c = s_[i_];
        if (c == u'\\') {
            body += c;
            ++i_;
            if (i_ >= s_.size() || isLineTerminator(s_[i_])) continue;
            body += s_[i_++];
            continue;
        }
        if (c == u'[') in_class = true;
        else if (c == u']') in_class = false;
        else if (c == u'/' && !in_class) {
            ++i_;
            break;
        }
        body += c;
        ++i_;
    }
    std::u16string flags;
    while (i_ < s_.size() && isIdPart(s_[i_])) flags += s_[i_++];
    t.value = std::move(body);
    t.raw = std::move(flags);
    t.end = static_cast<uint32_t>(i_);
    return t;
}

}  // namespace fbx::xweb::js
