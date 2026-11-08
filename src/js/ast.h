#pragma once

// The parser's tree, and the lexer's tokens. Internal to the engine.

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
    uint32_t line = 1;
    uint32_t begin = 0, end = 0;
};

enum class N : uint8_t {
    // Statements
    Program, VarDecl, Declarator, FunctionDecl, ClassDecl, Return, If, For, ForIn, ForOf, While, DoWhile,
    Block, ExprStmt, Empty, Break, Continue, Throw, Try, Switch, Case, Labeled, Debugger,
    // Expressions
    Number, String, Template, TaggedTemplate, Regex, Identifier, This, Super, Array, Object, Property,
    FunctionExpr, Arrow, ClassExpr, Method, Unary, Update, Binary, Logical, Assign, Conditional, Call, New,
    Member, Sequence, Spread, NewTarget, Hole, Boolean, Null, Undefined, OptionalChain, Yield, Await,
    ImportCall, ImportMeta,
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

struct Node {
    N kind;
    Op op = Op::None;
    uint32_t line = 0;
    uint32_t flags = 0;
    double number = 0;
    std::u16string str;           // a name, a string's value
    std::u16string str2;          // a template's raw text, a regex's flags
    Node* a = nullptr;
    Node* b = nullptr;
    Node* c = nullptr;
    Node* d = nullptr;
    std::vector<Node*> kids;
    FunctionInfo* fn = nullptr;   // functions, arrows, methods, classes' constructors
    std::shared_ptr<const std::vector<std::u16string>> lexical;   // a block's let/const/class names
    std::shared_ptr<const std::vector<std::u16string>> cache;     // a tagged template's strings
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
};

struct FunctionInfo {
    std::u16string name;
    std::vector<Node*> params;        // patterns
    Node* body = nullptr;             // a Block, or an arrow's expression
    bool arrow = false;
    bool expression_body = false;
    bool strict = false;
    bool uses_arguments = false;      // reads `arguments`
    bool uses_this = false;
    bool is_method = false;
    bool is_class_constructor = false;
    bool is_async = false;
    bool is_generator = false;
    bool derived = false;
    bool simple_params = true;
    int length = 0;                   // parameters before the first default or rest
    std::vector<std::u16string> var_names;   // `var` and parameter-free hoisting
    std::vector<Node*> function_decls;       // hoisted with their values
    std::vector<Node*> fields;               // a class constructor's instance fields
    std::u16string source;                   // its text, for toString
};

struct Program {
    std::string url;                 // a module's (or a script's) URL, for import() and import.meta
    bool module = false;
    std::deque<Node> nodes;
    std::deque<FunctionInfo> functions;
    Node* root = nullptr;
    FunctionInfo* top = nullptr;   // the script itself, as a function: its hoisting
    Node* make(N kind, uint32_t line) {
        nodes.push_back(Node{kind});
        nodes.back().line = line;
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
