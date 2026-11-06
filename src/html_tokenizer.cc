#include "html_tokenizer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace fbx::xweb::detail {

namespace {

constexpr std::string_view kReplacement = "\xEF\xBF\xBD";

struct NamedRef {
    const char* name;
    const char* value;
};
constexpr NamedRef kNamedRefs[] = {
#include "entities_table.inc"
};

bool isWs(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\f'; }
bool isAlpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
bool isHex(char c) noexcept { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

void appendUtf8(std::string& out, uint32_t cp) {
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

// The input stream's preprocessing: newlines normalized, invalid UTF-8 replaced
// (everything after this can trust the encoding), a leading BOM dropped.
std::string preprocess(std::string_view in) {
    if (in.substr(0, 3) == "\xEF\xBB\xBF") in.remove_prefix(3);
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        const auto c = static_cast<unsigned char>(in[i]);
        if (c == '\r') {
            out += '\n';
            i += (i + 1 < in.size() && in[i + 1] == '\n') ? 2 : 1;
            continue;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        int len = 0;
        uint32_t cp = 0, min = 0;
        if (c >= 0xC2 && c <= 0xDF) { len = 1; cp = c & 0x1F; min = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 2; cp = c & 0x0F; min = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 3; cp = c & 0x07; min = 0x10000; }
        bool ok = len > 0 && i + static_cast<size_t>(len) < in.size();
        for (int k = 1; ok && k <= len; ++k) {
            const auto cc = static_cast<unsigned char>(in[i + static_cast<size_t>(k)]);
            ok = (cc & 0xC0) == 0x80;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (ok && cp >= min && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) {
            out.append(in.substr(i, static_cast<size_t>(len) + 1));
            i += static_cast<size_t>(len) + 1;
        } else {
            out += kReplacement;
            ++i;
        }
    }
    return out;
}

// What numeric references to the C1 range really meant (windows-1252).
constexpr uint16_t kC1[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                              0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013,
                              0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};

const NamedRef* findNamed(std::string_view name) {
    const auto* end = std::end(kNamedRefs);
    const auto* it = std::lower_bound(std::begin(kNamedRefs), end, name,
                                      [](const NamedRef& r, std::string_view n) { return std::string_view(r.name) < n; });
    return (it != end && name == it->name) ? it : nullptr;
}

constexpr size_t kLongestRefName = 33;

}  // namespace

Tokenizer::Tokenizer(std::string_view input) : in_(preprocess(input)) {}

bool Tokenizer::startsWithNoCase(std::string_view word) const noexcept {
    if (in_.size() - pos_ < word.size()) return false;
    for (size_t i = 0; i < word.size(); ++i)
        if (lower(in_[pos_ + i]) != word[i]) return false;
    return true;
}

// pos_ is at "</". Is this the end tag of the element we're in?
bool Tokenizer::atAppropriateEndTag() const noexcept {
    if (last_start_tag_.empty()) return false;
    const size_t after = pos_ + 2 + last_start_tag_.size();
    if (after >= in_.size()) return false;
    for (size_t i = 0; i < last_start_tag_.size(); ++i)
        if (lower(in_[pos_ + 2 + i]) != last_start_tag_[i]) return false;
    const char c = in_[after];
    return isWs(c) || c == '/' || c == '>';
}

void Tokenizer::characterReference(std::string& out, bool in_attribute) {
    // pos_ is at '&'.
    const size_t start = pos_;
    ++pos_;
    if (eof()) { out += '&'; return; }
    if (in_[pos_] == '#') {
        ++pos_;
        const bool hex = !eof() && (in_[pos_] == 'x' || in_[pos_] == 'X');
        if (hex) ++pos_;
        const size_t digits = pos_;
        uint32_t cp = 0;
        while (!eof() && (hex ? isHex(in_[pos_]) : isDigit(in_[pos_]))) {
            const char c = lower(in_[pos_]);
            const uint32_t d = isDigit(c) ? static_cast<uint32_t>(c - '0') : static_cast<uint32_t>(c - 'a' + 10);
            if (cp <= 0x10FFFF) cp = cp * (hex ? 16 : 10) + d;
            ++pos_;
        }
        if (pos_ == digits) {   // "&#" with no digits stays as written
            out.append(in_, start, pos_ - start);
            return;
        }
        if (!eof() && in_[pos_] == ';') ++pos_;
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out += kReplacement;
            return;
        }
        if (cp >= 0x80 && cp <= 0x9F) cp = kC1[cp - 0x80];
        appendUtf8(out, cp);
        return;
    }
    // Named: the longest name in the table that the input starts with.
    size_t run = 0;
    while (pos_ + run < in_.size() && run < kLongestRefName &&
           (isAlpha(in_[pos_ + run]) || isDigit(in_[pos_ + run])))
        ++run;
    const size_t with_semicolon = (pos_ + run < in_.size() && in_[pos_ + run] == ';') ? run + 1 : run;
    for (size_t len = with_semicolon; len > 0; --len) {
        const NamedRef* ref = findNamed(std::string_view(in_).substr(pos_, len));
        if (!ref) continue;
        if (in_attribute && in_[pos_ + len - 1] != ';' && pos_ + len < in_.size()) {
            // In an attribute "?a=1&copy=2" is a URL, not a copyright sign.
            const char after = in_[pos_ + len];
            if (after == '=' || isAlpha(after) || isDigit(after)) break;
        }
        pos_ += len;
        out += ref->value;
        return;
    }
    out += '&';
}

void Tokenizer::readComment(Token& out) {
    // pos_ is just past "<!--".
    out.type = TokenType::Comment;
    out.data.clear();
    std::string_view rest = std::string_view(in_).substr(pos_);
    if (rest.substr(0, 1) == ">") { pos_ += 1; return; }
    if (rest.substr(0, 2) == "->") { pos_ += 2; return; }
    size_t end = 0, skip = 0;
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] != '-') continue;
        if (rest.substr(i, 3) == "-->") { end = i; skip = 3; break; }
        if (rest.substr(i, 4) == "--!>") { end = i; skip = 4; break; }
    }
    if (skip == 0) {   // ran out of input: what's there is the comment, less a partial closer
        end = rest.size();
        if (rest.ends_with("--!")) end -= 3;
        else if (rest.ends_with("--")) end -= 2;
        else if (rest.ends_with("-")) end -= 1;
        skip = rest.size() - end;
    }
    for (char c : rest.substr(0, end)) {
        if (c == '\0') out.data += kReplacement;
        else out.data += c;
    }
    pos_ += end + skip;
}

void Tokenizer::readBogusComment(Token& out) {
    out.type = TokenType::Comment;
    out.data.clear();
    while (!eof() && in_[pos_] != '>') {
        if (in_[pos_] == '\0') out.data += kReplacement;
        else out.data += in_[pos_];
        ++pos_;
    }
    if (!eof()) ++pos_;
}

void Tokenizer::readDoctype(Token& out) {
    // pos_ is just past "<!doctype".
    out.type = TokenType::Doctype;
    out.name.clear();
    out.public_id.clear();
    out.system_id.clear();
    out.has_public = out.has_system = false;
    out.force_quirks = false;
    const auto skipWs = [&] { while (!eof() && isWs(in_[pos_])) ++pos_; };
    const auto bogus = [&] {
        out.force_quirks = true;
        while (!eof() && in_[pos_] != '>') ++pos_;
        if (!eof()) ++pos_;
    };
    // Returns false if the closing '>' or the end of input came first.
    const auto quoted = [&](std::string& into) {
        const char q = in_[pos_++];
        while (!eof() && in_[pos_] != q && in_[pos_] != '>') {
            if (in_[pos_] == '\0') into += kReplacement;
            else into += in_[pos_];
            ++pos_;
        }
        if (eof()) { out.force_quirks = true; return false; }
        if (in_[pos_] == '>') { out.force_quirks = true; ++pos_; return false; }
        ++pos_;
        return true;
    };
    skipWs();
    if (eof()) { out.force_quirks = true; return; }
    if (in_[pos_] == '>') { out.force_quirks = true; ++pos_; return; }
    while (!eof() && !isWs(in_[pos_]) && in_[pos_] != '>') {
        if (in_[pos_] == '\0') out.name += kReplacement;
        else out.name += lower(in_[pos_]);
        ++pos_;
    }
    skipWs();
    if (eof()) { out.force_quirks = true; return; }
    if (in_[pos_] == '>') { ++pos_; return; }
    if (startsWithNoCase("public")) {
        pos_ += 6;
        skipWs();
        if (eof()) { out.force_quirks = true; return; }
        if (in_[pos_] != '"' && in_[pos_] != '\'') { bogus(); return; }
        out.has_public = true;
        if (!quoted(out.public_id)) return;
        skipWs();
        if (eof()) { out.force_quirks = true; return; }
        if (in_[pos_] == '>') { ++pos_; return; }
        if (in_[pos_] != '"' && in_[pos_] != '\'') { bogus(); return; }
        out.has_system = true;
        if (!quoted(out.system_id)) return;
    } else if (startsWithNoCase("system")) {
        pos_ += 6;
        skipWs();
        if (eof()) { out.force_quirks = true; return; }
        if (in_[pos_] != '"' && in_[pos_] != '\'') { bogus(); return; }
        out.has_system = true;
        if (!quoted(out.system_id)) return;
    } else {
        bogus();
        return;
    }
    skipWs();
    if (eof()) { out.force_quirks = true; return; }
    // After the identifiers anything up to '>' is ignored, without forcing quirks.
    while (!eof() && in_[pos_] != '>') ++pos_;
    if (!eof()) ++pos_;
}

void Tokenizer::readMarkupDeclaration(Token& out) {
    // pos_ is just past "<!".
    if (in_.compare(pos_, 2, "--") == 0) {
        pos_ += 2;
        readComment(out);
    } else if (startsWithNoCase("doctype")) {
        pos_ += 7;
        readDoctype(out);
    } else if (cdata_allowed_ && in_.compare(pos_, 7, "[CDATA[") == 0) {
        pos_ += 7;
        const size_t end = in_.find("]]>", pos_);
        out.type = TokenType::Characters;
        out.data = in_.substr(pos_, end == std::string::npos ? std::string::npos : end - pos_);
        pos_ = end == std::string::npos ? in_.size() : end + 3;
    } else {
        readBogusComment(out);
    }
}

// True once past the tag's '>'; false if the input ended first, and the tag is dropped.
bool Tokenizer::readAttributes(Token& out) {
    while (true) {
        while (!eof() && isWs(in_[pos_])) ++pos_;
        if (eof()) return false;
        if (in_[pos_] == '>') { ++pos_; return true; }
        if (in_[pos_] == '/') {
            ++pos_;
            if (!eof() && in_[pos_] == '>') {
                out.self_closing = true;
                ++pos_;
                return true;
            }
            continue;
        }
        Attribute a;
        if (in_[pos_] == '=') {   // a stray '=' starts the name
            a.name += '=';
            ++pos_;
        }
        while (!eof() && !isWs(in_[pos_]) && in_[pos_] != '/' && in_[pos_] != '>' && in_[pos_] != '=') {
            if (in_[pos_] == '\0') a.name += kReplacement;
            else a.name += lower(in_[pos_]);
            ++pos_;
        }
        while (!eof() && isWs(in_[pos_])) ++pos_;
        if (!eof() && in_[pos_] == '=') {
            ++pos_;
            while (!eof() && isWs(in_[pos_])) ++pos_;
            if (!eof() && (in_[pos_] == '"' || in_[pos_] == '\'')) {
                const char q = in_[pos_++];
                while (!eof() && in_[pos_] != q) {
                    if (in_[pos_] == '&') { characterReference(a.value, true); continue; }
                    if (in_[pos_] == '\0') a.value += kReplacement;
                    else a.value += in_[pos_];
                    ++pos_;
                }
                if (!eof()) ++pos_;
            } else {
                while (!eof() && !isWs(in_[pos_]) && in_[pos_] != '>') {
                    if (in_[pos_] == '&') { characterReference(a.value, true); continue; }
                    if (in_[pos_] == '\0') a.value += kReplacement;
                    else a.value += in_[pos_];
                    ++pos_;
                }
            }
        }
        const bool duplicate = std::any_of(out.attrs.begin(), out.attrs.end(),
                                           [&](const Attribute& o) { return o.name == a.name; });
        if (!duplicate) out.attrs.push_back(std::move(a));
    }
}

// pos_ is at the name's first letter. False means the input ended inside the tag.
bool Tokenizer::readTag(Token& out) {
    out.name.clear();
    out.attrs.clear();
    out.self_closing = false;
    while (!eof() && !isWs(in_[pos_]) && in_[pos_] != '/' && in_[pos_] != '>') {
        if (in_[pos_] == '\0') out.name += kReplacement;
        else out.name += lower(in_[pos_]);
        ++pos_;
    }
    if (eof()) return false;
    return readAttributes(out);
}

void Tokenizer::readEndTag(Token& out) {
    pos_ += 2;
    out.type = TokenType::EndTag;
    if (!readTag(out)) { out.type = TokenType::Eof; return; }
    out.attrs.clear();
    out.self_closing = false;
}

// pos_ is at '<' in the data state. False means it's just a '<'.
bool Tokenizer::tryTag(Token& out) {
    if (pos_ + 1 >= in_.size()) return false;
    const char c = in_[pos_ + 1];
    if (isAlpha(c)) {
        ++pos_;
        out.type = TokenType::StartTag;
        if (!readTag(out)) { out.type = TokenType::Eof; return true; }
        last_start_tag_ = out.name;
        return true;
    }
    if (c == '!') {
        pos_ += 2;
        readMarkupDeclaration(out);
        return true;
    }
    if (c == '?') {
        ++pos_;
        readBogusComment(out);
        return true;
    }
    if (c == '/') {
        if (pos_ + 2 >= in_.size()) return false;
        const char d = in_[pos_ + 2];
        if (isAlpha(d)) {
            readEndTag(out);
            return true;
        }
        if (d == '>') {   // "</>" is dropped
            pos_ += 3;
            out.type = TokenType::Comment;
            out.name = "\x01skip";
            return true;
        }
        pos_ += 2;
        readBogusComment(out);
        return true;
    }
    return false;
}

void Tokenizer::textRun(Token& out) {
    out.type = TokenType::Characters;
    out.data.clear();
    const bool refs = state_ == TokenizerState::Data || state_ == TokenizerState::RcData;
    enum { Normal, Escaped, DoubleEscaped } script = Normal;
    while (!eof()) {
        const char c = in_[pos_];
        if (c == '&' && refs) {
            characterReference(out.data, false);
            continue;
        }
        if (c == '\0' && state_ != TokenizerState::Data) {
            out.data += kReplacement;
            ++pos_;
            continue;
        }
        if (c == '-' && state_ == TokenizerState::ScriptData && script != Normal &&
            in_.compare(pos_, 3, "-->") == 0) {
            script = Normal;
            out.data += "-->";
            pos_ += 3;
            continue;
        }
        if (c != '<' || state_ == TokenizerState::PlainText) {
            out.data += c;
            ++pos_;
            continue;
        }
        if (state_ == TokenizerState::Data) return;   // the caller looks at the tag
        if (state_ == TokenizerState::ScriptData) {
            if (script == Normal && in_.compare(pos_, 4, "<!--") == 0) {
                script = Escaped;
                out.data += "<!";
                pos_ += 2;   // the dashes count towards a closing "-->", so "<!-->" closes
                continue;
            }
            const auto wordEnds = [&](size_t at) {
                return at < in_.size() && (isWs(in_[at]) || in_[at] == '/' || in_[at] == '>');
            };
            if (script != Normal) {
                ++pos_;
                const bool opener = startsWithNoCase("script") && wordEnds(pos_ + 6);
                --pos_;
                if (opener && script == Escaped) {
                    script = DoubleEscaped;
                    out.data += in_.substr(pos_, 7);
                    pos_ += 7;
                    continue;
                }
            }
            if (script == DoubleEscaped && in_.compare(pos_, 2, "</") == 0) {
                pos_ += 2;
                const bool closer = startsWithNoCase("script") && wordEnds(pos_ + 6);
                pos_ -= 2;
                if (closer) {
                    script = Escaped;
                    out.data += in_.substr(pos_, 8);
                    pos_ += 8;
                    continue;
                }
                out.data += c;
                ++pos_;
                continue;
            }
        }
        if (in_.compare(pos_, 2, "</") == 0 && atAppropriateEndTag()) return;
        out.data += c;
        ++pos_;
    }
}

void Tokenizer::next(Token& out) {
    while (true) {
        out.type = TokenType::Eof;
        out.name.clear();
        if (eof()) return;
        if (state_ == TokenizerState::Data) {
            if (in_[pos_] == '<') {
                if (tryTag(out)) {
                    if (out.type == TokenType::Comment && out.name == "\x01skip") continue;
                    return;
                }
                out.type = TokenType::Characters;
                out.data = "<";
                ++pos_;
                return;
            }
            textRun(out);
            return;
        }
        if (state_ != TokenizerState::PlainText && in_.compare(pos_, 2, "</") == 0 && atAppropriateEndTag()) {
            readEndTag(out);
            state_ = TokenizerState::Data;
            return;
        }
        textRun(out);
        return;
    }
}

}  // namespace fbx::xweb::detail
