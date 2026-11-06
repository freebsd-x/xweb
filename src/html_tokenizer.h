#pragma once

// The HTML tokenizer (WHATWG 13.2.5). Private to the parser.

#include <string>
#include <string_view>
#include <vector>

#include "xweb/dom.h"

namespace fbx::xweb::detail {

enum class TokenType { Doctype, StartTag, EndTag, Comment, Characters, Eof };

struct Token {
    TokenType type = TokenType::Eof;
    std::string name;   // tag or doctype name
    std::string data;   // comment or characters
    std::vector<Attribute> attrs;
    bool self_closing = false;
    bool force_quirks = false;
    bool has_public = false, has_system = false;
    std::string public_id, system_id;
};

enum class TokenizerState { Data, RcData, RawText, ScriptData, PlainText };

class Tokenizer {
public:
    explicit Tokenizer(std::string_view input);

    void next(Token& out);
    void setState(TokenizerState state) noexcept { state_ = state; }
    void setCdataAllowed(bool allowed) noexcept { cdata_allowed_ = allowed; }

private:
    [[nodiscard]] bool eof() const noexcept { return pos_ >= in_.size(); }
    [[nodiscard]] bool startsWithNoCase(std::string_view word) const noexcept;
    [[nodiscard]] bool atAppropriateEndTag() const noexcept;

    void textRun(Token& out);
    bool tryTag(Token& out);
    void readEndTag(Token& out);
    bool readTag(Token& out);
    bool readAttributes(Token& out);
    void readMarkupDeclaration(Token& out);
    void readComment(Token& out);
    void readBogusComment(Token& out);
    void readDoctype(Token& out);
    void characterReference(std::string& out, bool in_attribute);

    std::string in_;
    size_t pos_ = 0;
    TokenizerState state_ = TokenizerState::Data;
    bool cdata_allowed_ = false;
    std::string last_start_tag_;
};

}  // namespace fbx::xweb::detail
