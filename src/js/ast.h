#pragma once

// The parser's tree, and the lexer's tokens. Internal to the engine.

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xweb/js.h"

namespace fbx::xweb::js {

enum class Tok : uint8_t {
    Eof, Identifier, Keyword, Number, String, Template, Regex, Punct, PrivateName,
};

struct Token {
    Tok type = Tok::Eof;
    std::u16string value;     // identifier name, keyword, punctuator, cooked string
    std::u16string raw;       // a template part's raw text; a regex's flags
    double number = 0;
    bool newline_before = false;
    bool escaped = false;     // an identifier written with \u escapes (can't be a keyword)
    bool template_tail = false;   // a template part ending the literal
    bool cooked_ok = true;    // a template part with a valid escape everywhere
    bool legacy_octal = false;   // 0777: not in strict code
    bool bigint = false;         // 123n: `value` holds its digits
    uint32_t line = 1;
    uint32_t begin = 0, end = 0;
};

enum class N : uint8_t {
    // Statements
    Program, VarDecl, Declarator, FunctionDecl, ClassDecl, Return, If, For, ForIn, ForOf, While, DoWhile,
    Block, ExprStmt, Empty, Break, Continue, Throw, Try, Switch, Case, Labeled, Debugger, With,
    // Expressions
    Number, String, Template, TaggedTemplate, Regex, Identifier, This, Super, Array, Object, Property,
    FunctionExpr, Arrow, ClassExpr, Method, Unary, Update, Binary, Logical, Assign, Conditional, Call, New,
    Member, Sequence, Spread, NewTarget, Hole, Boolean, Null, Undefined, OptionalChain, Yield, Await,
    ImportCall, ImportMeta, BigInt,
    // Modules
    ImportDecl, ExportDecl, ImportSpec, ExportSpec,
    // Patterns
    ArrayPattern, ObjectPattern, PatternProperty, AssignPattern, Rest,
};

// Operators, as their own small numbers.
enum class Op : uint8_t {
    None,
    // Binary
    Add, Sub, Mul, Div, Mod, Exp, Shl, Shr, UShr, BitAnd, BitOr, BitXor,
    Eq, Ne, StrictEq, StrictNe, Lt, Gt, Le, Ge, In, InstanceOf,
    // Logical
    And, Or, Nullish,
    // Unary
    Neg, Plus, Not, BitNot, TypeOf, Void, Delete,
    // Update
    Inc, Dec,
};

struct FunctionInfo;

// A string most nodes don't have (a template part's raw text, a regex's flags, an
// import's local name): held out of line, so a node is 16 bytes smaller (a page's
// scripts are millions of nodes). Reads as a std::u16string, empty when there's none.
class RareString {
public:
    RareString() = default;
    RareString(const RareString& o) : s_(o.s_ ? std::make_unique<std::u16string>(*o.s_) : nullptr) {}
    RareString& operator=(const RareString& o) {
        s_ = o.s_ ? std::make_unique<std::u16string>(*o.s_) : nullptr;
        return *this;
    }
    RareString(RareString&&) noexcept = default;
    RareString& operator=(RareString&&) noexcept = default;
    RareString& operator=(std::u16string v) {
        if (v.empty()) s_.reset();
        else s_ = std::make_unique<std::u16string>(std::move(v));
        return *this;
    }
    [[nodiscard]] const std::u16string& get() const noexcept {
        static const std::u16string kEmpty;
        return s_ ? *s_ : kEmpty;
    }
    operator const std::u16string&() const noexcept { return get(); }
    [[nodiscard]] bool empty() const noexcept { return !s_ || s_->empty(); }
    friend bool operator==(const RareString& a, const std::u16string& b) { return a.get() == b; }
    friend bool operator==(const std::u16string& a, const RareString& b) { return a == b.get(); }
    friend bool operator!=(const RareString& a, const std::u16string& b) { return a.get() != b; }
    friend bool operator!=(const std::u16string& a, const RareString& b) { return a != b.get(); }

private:
    std::unique_ptr<std::u16string> s_;
};

inline constexpr uint32_t kGlobalSlot = 0xFFFFFFFFu;
// kGlobalPos + n: in none, the global object's property made n-th (as last found there).
inline constexpr uint32_t kGlobalPos = 0x80000000u;

struct Node {
    N kind;
    Op op = Op::None;
    uint32_t line = 0;
    uint32_t flags = 0;
    uint32_t offset = 0;          // where in its source it starts (a column, with Program::line_starts)
    std::u16string str;           // a name, a string's value
    RareString str2;              // a template's raw text, a regex's flags, an import's local name
    Node* a = nullptr;
    Node* b = nullptr;
    Node* c = nullptr;
    Node* d = nullptr;
    std::vector<Node*> kids;
    FunctionInfo* fn = nullptr;   // functions, arrows, methods, classes' constructors
    const std::vector<std::u16string>* lexical = nullptr;   // a block's let/const/class names (its Program keeps them)
    // A member access's key (a.b's "b"), made once: each access used to copy the name.
    std::unique_ptr<PropertyKey> key;
    std::unique_ptr<JSString> literal;   // a string literal's value, shared by each evaluation
    // A number literal's value, or an identifier's name hash for scope lookups (0 until
    // it's asked): no node is both, and these are millions of nodes.
    union {
        double number = 0;
        size_t name_hash;
    };
    // Where an identifier was found last: this many scopes out, at this slot there (or
    // kGlobalSlot: in none, so the global object's). -1 not yet, -2 never (a with).
    int32_t scope_hops = -1;
    uint32_t scope_slot = 0;
};

// Node flags.
enum : uint32_t {
    kLet = 1, kConst = 2, kVar = 4,           // VarDecl, ForIn/ForOf left side
    kComputed = 8, kOptional = 16,            // Member, Property, Call
    kPrefix = 32,                             // Update
    kStatic = 64, kGetter = 128, kSetter = 256, kShorthand = 512, kMethodProp = 1024, kSpreadProp = 2048,
    kParenthesized = 4096,
    kField = 8192,                            // a class field
    kAwait = 16384,                           // for await (… of …)
    kDefault = 32768,                         // export default
    kNamespace = 65536,                       // import * as ns, export * as ns
    kHoisted = 131072,                        // export default function () {}: made at link time
    kStaticBlock = 262144,                    // a class's static { ... } (with kStatic | kField)
    kCaptureKnown = 1u << 19, kCaptures = 1u << 20,   // For: whether its parts make functions or eval
};

struct FunctionInfo {
    std::u16string name;
    std::vector<Node*> params;        // patterns
    Node* body = nullptr;             // a Block, or an arrow's expression
    bool arrow = false;
    bool expression_body = false;
    bool strict = false;
    bool uses_arguments = false;      // reads `arguments`
    bool own_arguments = false;       // reads `name.arguments`, its own name's: the legacy
                                      // function.arguments, this call's while it runs
    bool uses_this = false;
    // Makes a function (or class, or method) or names `eval`: something may hold on
    // to its scope after the call. Without it, the call's scopes die with the call.
    bool scope_escapes = false;
    bool is_method = false;
    bool is_class_constructor = false;
    bool is_async = false;
    bool is_generator = false;
    bool derived = false;
    bool simple_params = true;
    int length = 0;                   // parameters before the first default or rest
    std::vector<std::u16string> var_names;   // `var` and parameter-free hoisting
    std::vector<size_t> var_hashes;          // their hashes, worked out at the first call
    std::vector<Node*> function_decls;       // hoisted with their values
    std::vector<Node*> fields;               // a class constructor's instance fields
    std::u16string source;                   // its text, for toString
};

// Where the statement running now is: its script's URL and line, for an error's stack.
// (One runtime runs at a time on a thread; a Program forgets itself here as it goes.)
struct ThrowSite {
    const std::string* url = nullptr;
    const std::vector<uint32_t>* line_starts = nullptr;
    uint32_t line = 0;
    uint32_t offset = 0;
};
inline thread_local ThrowSite t_throw_site;
inline ThrowSite& throwSite() noexcept { return t_throw_site; }
// The script functions being called, innermost last: each with where it was called from,
// for an error's stack (as V8 writes it, which pages and their tools read).
struct CallFrame {
    ThrowSite site;                   // the call, in its caller
    const std::u16string* name;       // the function called
};
inline thread_local std::vector<CallFrame> t_call_frames;
// "\n    at url:line:1" for where that is, or nothing: what an error's stack ends with.
std::u16string stackLine();

struct Program {
    Program() = default;
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
    ~Program() {
        if (throwSite().url == &url) {
            throwSite().url = nullptr;
            throwSite().line_starts = nullptr;
        }
    }
    std::string url;                 // a module's (or a script's) URL, for import() and import.meta
    std::vector<uint32_t> line_starts;   // the offset each line starts at, for columns
    uint32_t make_offset = 0;        // set by the parser: where the token being read starts
    bool module = false;
    std::deque<Node> nodes;
    std::vector<std::shared_ptr<const std::vector<std::u16string>>> lexicals;   // what nodes' `lexical` point to
    std::deque<FunctionInfo> functions;
    Node* root = nullptr;
    FunctionInfo* top = nullptr;   // the script itself, as a function: its hoisting
    Node* make(N kind, uint32_t line) {
        nodes.push_back(Node{kind});
        nodes.back().line = line;
        nodes.back().offset = make_offset;
        return &nodes.back();
    }
};

class Lexer {
public:
    explicit Lexer(std::u16string_view src) : s_(src) {}
    Token next(bool regex_allowed);
    // A template literal's next part, after the `}` that ended a substitution.
    Token templatePart();
    [[nodiscard]] uint32_t line() const noexcept { return line_; }
    [[nodiscard]] size_t pos() const noexcept { return i_; }
    void reset(size_t pos, uint32_t line) { i_ = pos; line_ = line; }
    std::string error;
    bool module = false;   // module code: no HTML-like comments

private:
    bool skipSpace(bool& newline);
    Token identifier(Token t);
    Token numberLiteral(Token t);
    Token stringLiteral(Token t, char16_t quote);
    Token templateLiteral(Token t);
    Token regexLiteral(Token t);
    bool escape(std::u16string& out, bool in_template, bool& ok);
    std::u16string_view s_;
    size_t i_ = 0;
    uint32_t line_ = 1;
};

// Parses a script. On a syntax error, `error` says what and where, and the result is null.
// Where eval code sits: inside a function, a method, a derived class's constructor.
struct EvalContext {
    bool new_target = false;
    bool super_property = false;
    bool super_call = false;
};
std::unique_ptr<Program> parse(std::u16string_view source, std::string& error, bool strict = false, bool module = false,
                               const EvalContext* eval = nullptr);

[[nodiscard]] bool isIdStart(char32_t c);
[[nodiscard]] bool isIdPart(char32_t c);
[[nodiscard]] bool isLineTerminator(char16_t c);
[[nodiscard]] bool isJsSpace(char16_t c);

}  // namespace fbx::xweb::js
