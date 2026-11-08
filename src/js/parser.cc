// The JavaScript parser: ECMAScript 2020 scripts, by recursive descent, into the tree
// the interpreter walks. Arrow functions and destructuring assignment are found the way
// the spec's cover grammars describe: parsed as expressions, then reinterpreted.

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

#include "ast.h"
#include "regexp.h"
#include "xweb/js.h"

namespace fbx::xweb::js {

namespace {

constexpr int kMaxNesting = 400;   // deeper than this is an error, not a crash

struct ParseError {
    std::string message;
    uint32_t line;
    uint32_t pos = 0;   // where in the source
};

class Parser {
public:
    Parser(std::u16string_view src, Program& prog, bool strict) : src_(src), lex_(src), p_(prog), strict_(strict) {}

    void setModule() { lex_.module = true; }
    void setEvalContext(const EvalContext& e) {
        if (e.new_target) non_arrow_depth_ = 1;
        super_property_ok_ = e.super_property;
        super_call_ok_ = e.super_call;
    }

    Node* program() {
        FunctionInfo* top = newFunction();
        if (p_.module) {
            strict_ = true;
            top->is_async = true;   // top-level await
        }
        top->strict = strict_;
        p_.top = top;
        fn_ = top;
        advance();
        Node* root = p_.make(N::Program, cur_.line);
        auto lexical = std::make_shared<std::vector<std::u16string>>();
        pushScope(lexical.get());
        directives(top);
        while (cur_.type != Tok::Eof) {
            top_item_ = true;
            root->kids.push_back(statementListItem());
        }
        top_item_ = false;
        if (p_.module) checkModuleExports(root, *lexical);
        popScope();
        if (!lex_.error.empty()) fail(lex_.error);
        root->lexical = lexical;
        root->fn = top;
        return root;
    }

private:
    // ── Tokens ──

    [[noreturn]] void fail(const std::string& message) { throw ParseError{message, cur_.line, cur_.begin}; }

    bool regexAfter(const Token& t) const {
        // `yield /re/` and `await /re/`: operators there, so a regex may follow.
        if (t.type == Tok::Identifier && !t.escaped && fn_ &&
            ((t.value == u"yield" && fn_->is_generator) || (t.value == u"await" && fn_->is_async)))
            return true;
        switch (t.type) {
            case Tok::Number: case Tok::String: case Tok::Regex: case Tok::Identifier: case Tok::PrivateName: return false;
            case Tok::Template: return !t.template_tail;
            case Tok::Keyword: return !(t.value == u"this" || t.value == u"super" || t.value == u"null" || t.value == u"true" || t.value == u"false");
            case Tok::Punct: return !(t.value == u")" || t.value == u"]" || t.value == u"}");
            default: return true;
        }
    }

    void advance() {
        const bool regex = started_ ? regexAfter(cur_) : true;
        started_ = true;
        prev_end_ = cur_.end;
        prev_line_ = cur_.line;
        cur_ = lex_.next(regex);
        if (cur_.type == Tok::Eof && !lex_.error.empty()) fail(lex_.error);
    }

    Token peek() {
        const size_t pos = lex_.pos();
        const uint32_t line = lex_.line();
        Token t = lex_.next(regexAfter(cur_));
        lex_.reset(pos, line);
        lex_.error.clear();
        return t;
    }

    [[nodiscard]] bool is(const char16_t* punct) const { return cur_.type == Tok::Punct && cur_.value == punct; }
    [[nodiscard]] bool isKw(const char16_t* kw) const { return cur_.type == Tok::Keyword && cur_.value == kw; }
    [[nodiscard]] bool isIdent(const char16_t* name) const { return cur_.type == Tok::Identifier && !cur_.escaped && cur_.value == name; }
    bool eat(const char16_t* punct) {
        if (!is(punct)) return false;
        advance();
        return true;
    }
    void expect(const char16_t* punct) {
        if (!eat(punct)) fail("expected '" + utf8(punct) + "' but found '" + utf8(cur_.value) + "'");
    }
    void semicolon() {
        if (eat(u";")) return;
        if (is(u"}") || cur_.type == Tok::Eof || cur_.newline_before) return;
        fail("missing ; before '" + utf8(cur_.value) + "'");
    }

    // Inside brackets and bodies, `in` is an operator again (a for head turns it off).
    struct InAllowed {
        Parser& p;
        bool saved;
        explicit InAllowed(Parser& parser) : p(parser), saved(parser.no_in_) { p.no_in_ = false; }
        ~InAllowed() { p.no_in_ = saved; }
    };

    struct Depth {
        Parser& p;
        explicit Depth(Parser& parser) : p(parser) {
            if (++p.depth_ > kMaxNesting) p.fail("too deeply nested");
        }
        ~Depth() { --p.depth_; }
    };

    FunctionInfo* newFunction() {
        p_.functions.emplace_back();
        return &p_.functions.back();
    }

    static bool strictReserved(const std::u16string& n) {
        for (const char16_t* w : {u"implements", u"interface", u"let", u"package", u"private", u"protected", u"public", u"static"})
            if (n == w) return true;
        return false;
    }

    // A name used as an identifier where it may not be one.
    void checkName(const std::u16string& name, bool binding) {
        if (name == u"yield" && (strict_ || fn_->is_generator)) fail("yield can't be a name here");
        if (name == u"await" && (p_.module || fn_->is_async)) fail("await can't be a name here");
        if (strict_ && strictReserved(name)) fail("'" + utf8(name) + "' is reserved in strict code");
        if (binding && strict_ && (name == u"eval" || name == u"arguments")) fail("can't bind eval or arguments in strict code");
    }

    // A var declaration: hoisted to its function, through every block on the way,
    // which mustn't have a let, const or class of that name.
    void addVarDecl(const std::u16string& name) {
        for (size_t i = lexical_.size(); i-- > var_stop_;) {
            const auto& lex = *lexical_[i];
            if (std::find(lex.begin(), lex.end(), name) != lex.end()) fail("redeclaration of '" + utf8(name) + "'");
            scope_vars_[i].push_back(name);
        }
        addVar(name);
    }

    void pushScope(std::vector<std::u16string>* lexical) {
        lexical_.push_back(lexical);
        scope_vars_.emplace_back();
    }
    void popScope() {
        lexical_.pop_back();
        scope_vars_.pop_back();
    }

    void addVar(const std::u16string& name) {
        auto& v = fn_->var_names;
        if (std::find(v.begin(), v.end(), name) == v.end()) v.push_back(name);
    }

    void addLexical(const std::u16string& name) {
        if (lexical_.empty()) return;
        auto& v = *lexical_.back();
        if (std::find(v.begin(), v.end(), name) != v.end()) fail("redeclaration of '" + utf8(name) + "'");
        if (const auto& vars = scope_vars_.back(); std::find(vars.begin(), vars.end(), name) != vars.end())
            fail("redeclaration of '" + utf8(name) + "'");
        if (name == u"let") fail("let can't be a lexically declared name");
        v.push_back(name);
    }

    // Names a pattern binds.
    void boundNames(Node* pattern, std::vector<std::u16string>& out) {
        if (!pattern) return;
        switch (pattern->kind) {
            case N::Identifier: out.push_back(pattern->str); break;
            case N::AssignPattern: boundNames(pattern->a, out); break;
            case N::Rest: boundNames(pattern->a, out); break;
            case N::ArrayPattern: for (Node* k : pattern->kids) boundNames(k, out); break;
            case N::ObjectPattern: for (Node* k : pattern->kids) boundNames(k->kind == N::PatternProperty ? k->b : k, out); break;
            default: break;
        }
    }

    bool directives(FunctionInfo* fn) {
        bool said = false;
        while (cur_.type == Tok::String) {
            const std::u16string_view raw = src_.substr(cur_.begin + 1, cur_.end - cur_.begin - 2);
            const Token t = peek();
            const bool alone = t.type == Tok::Eof || (t.type == Tok::Punct && (t.value == u";" || t.value == u"}")) || t.newline_before;
            if (!alone) break;
            if (raw == u"use strict") {
                fn->strict = true;
                strict_ = true;
                said = true;
            }
            // Leave the string to be parsed as the statement it also is.
            break;
        }
        return said;
    }

    // ── Statements ──

    Node* statementListItem() {
        if (isKw(u"function") || (isIdent(u"async") && peekIsFunctionAfterAsync())) return functionDeclaration();
        if (isKw(u"class")) return classDeclaration();
        if (isKw(u"const") || (isIdent(u"let") && letStartsDeclaration())) return lexicalDeclaration(true);
        return statement();
    }

    // ── Modules ──

    // A name in an import or export list: any identifier name, or a string.
    std::u16string moduleExportName(bool* was_string = nullptr) {
        if (cur_.type == Tok::String || cur_.type == Tok::Identifier || cur_.type == Tok::Keyword) {
            if (was_string) *was_string = cur_.type == Tok::String;
            std::u16string name = cur_.value;
            advance();
            return name;
        }
        fail("expected a name");
    }

    std::u16string moduleSpecifier() {
        if (!isIdent(u"from")) fail("expected from");
        advance();
        if (cur_.type != Tok::String) fail("expected a module specifier string");
        std::u16string spec = cur_.value;
        advance();
        // Import attributes (with { type: "json" }), read and set aside.
        if (isKw(u"with") || (isIdent(u"assert") && !cur_.newline_before)) {
            advance();
            expect(u"{");
            while (!eat(u"}")) {
                if (cur_.type == Tok::Eof) fail("unterminated import attributes");
                advance();
            }
        }
        return spec;
    }

    std::u16string localName() {
        if (cur_.type != Tok::Identifier) fail("expected a name to bind");
        std::u16string n = cur_.value;
        if (n == u"await" || n == u"yield" || n == u"let") fail("'" + utf8(n) + "' can't be bound in a module");
        advance();
        return n;
    }

    Node* importSpec(const std::u16string& imported, const std::u16string& local) {
        if (std::find(import_names_.begin(), import_names_.end(), local) != import_names_.end()) fail("redeclaration of '" + utf8(local) + "'");
        import_names_.push_back(local);
        Node* sp = p_.make(N::ImportSpec, cur_.line);
        sp->str = imported;
        sp->str2 = local;
        return sp;
    }

    Node* importDeclaration() {
        Node* n = p_.make(N::ImportDecl, cur_.line);
        advance();   // import
        if (cur_.type == Tok::String) {
            n->str = cur_.value;
            advance();
            if (isKw(u"with")) {
                advance();
                expect(u"{");
                while (!eat(u"}")) {
                    if (cur_.type == Tok::Eof) fail("unterminated import attributes");
                    advance();
                }
            }
            semicolon();
            return n;
        }
        bool more = true;
        if (cur_.type == Tok::Identifier) {
            n->kids.push_back(importSpec(u"default", localName()));
            more = eat(u",");
        }
        if (more) {
            if (eat(u"*")) {
                if (!isIdent(u"as")) fail("expected as");
                advance();
                n->kids.push_back(importSpec(u"*", localName()));
            } else if (eat(u"{")) {
                while (!eat(u"}")) {
                    bool was_string = false;
                    const std::u16string imported = moduleExportName(&was_string);
                    std::u16string local = imported;
                    if (isIdent(u"as")) {
                        advance();
                        local = localName();
                    } else if (was_string || isKeywordName(imported)) {
                        fail("'" + utf8(imported) + "' needs `as` and a name to bind");
                    }
                    n->kids.push_back(importSpec(imported, local));
                    if (!is(u"}")) expect(u",");
                }
            } else {
                fail("expected an import list");
            }
        }
        n->str = moduleSpecifier();
        semicolon();
        return n;
    }

    static bool isKeywordName(const std::u16string& s) {
        static const char16_t* kWords[] = {u"break", u"case", u"catch", u"class", u"const", u"continue", u"debugger", u"default", u"delete",
                                           u"do", u"else", u"export", u"extends", u"finally", u"for", u"function", u"if", u"import", u"in",
                                           u"instanceof", u"new", u"return", u"super", u"switch", u"this", u"throw", u"try", u"typeof", u"var",
                                           u"void", u"while", u"with", u"null", u"true", u"false", u"enum"};
        for (const char16_t* k : kWords)
            if (s == k) return true;
        return false;
    }

    Node* exportDeclaration() {
        const uint32_t line = cur_.line;
        Node* n = p_.make(N::ExportDecl, line);
        advance();   // export
        if (eat(u"*")) {
            n->flags |= kNamespace;
            if (isIdent(u"as")) {
                advance();
                n->str2 = moduleExportName();
            }
            n->str = moduleSpecifier();
            semicolon();
            return n;
        }
        if (isKw(u"default")) {
            advance();
            n->flags |= kDefault;
            if (isKw(u"function") || (isIdent(u"async") && peekIsFunctionAfterAsync())) {
                // Named, it's a declaration (hoisted); anonymous, an expression named "default".
                Token t = peek();
                const bool named = isIdent(u"async") ? true : !(t.type == Tok::Punct);
                if (isIdent(u"async")) {
                    // async function NAME? Look past `function` and an optional `*`.
                    const size_t pos = lex_.pos();
                    const uint32_t lline = lex_.line();
                    Token a = lex_.next(false);
                    Token b = lex_.next(false);
                    if (b.type == Tok::Punct && b.value == u"*") b = lex_.next(false);
                    lex_.reset(pos, lline);
                    lex_.error.clear();
                    (void)a;
                    if (b.type == Tok::Punct) {
                        Node* f = functionExpression();
                        f->fn->name = u"default";
                        n->a = f;
                        n->flags |= kHoisted;
                        return n;
                    }
                    n->a = functionDeclaration();
                    return n;
                }
                if (t.type == Tok::Punct && t.value == u"*") {
                    // function* NAME?
                    const size_t pos = lex_.pos();
                    const uint32_t lline = lex_.line();
                    lex_.next(false);
                    Token b = lex_.next(false);
                    lex_.reset(pos, lline);
                    lex_.error.clear();
                    if (b.type == Tok::Punct) {
                        Node* f = functionExpression();
                        f->fn->name = u"default";
                        n->a = f;
                        n->flags |= kHoisted;
                        return n;
                    }
                    n->a = functionDeclaration();
                    return n;
                }
                if (named) {
                    n->a = functionDeclaration();
                } else {
                    Node* f = functionExpression();
                    f->fn->name = u"default";
                    n->a = f;
                    n->flags |= kHoisted;
                }
                return n;
            }
            if (isKw(u"class")) {
                const Token t = peek();
                if (t.type == Tok::Identifier) {
                    n->a = classDeclaration();
                    return n;
                }
            }
            Node* e = assignment();
            if ((e->kind == N::FunctionExpr || e->kind == N::Arrow || e->kind == N::ClassExpr) && e->fn && e->str.empty() && e->fn->name.empty())
                e->fn->name = u"default";
            n->a = e;
            semicolon();
            return n;
        }
        if (eat(u"{")) {
            while (!eat(u"}")) {
                Node* sp = p_.make(N::ExportSpec, cur_.line);
                sp->str = moduleExportName();
                sp->str2 = sp->str;
                if (isIdent(u"as")) {
                    advance();
                    sp->str2 = moduleExportName();
                }
                n->kids.push_back(sp);
                if (!is(u"}")) expect(u",");
            }
            if (isIdent(u"from")) n->str = moduleSpecifier();
            semicolon();
            return n;
        }
        if (isKw(u"var")) {
            n->a = variableDeclaration(kVar, true);
            semicolon();
        } else if (isKw(u"const") || (isIdent(u"let") && letStartsDeclaration())) {
            n->a = lexicalDeclaration(true);
        } else if (isKw(u"function") || (isIdent(u"async") && peekIsFunctionAfterAsync())) {
            n->a = functionDeclaration();
        } else if (isKw(u"class")) {
            n->a = classDeclaration();
        } else {
            fail("unexpected '" + utf8(cur_.value) + "' after export");
        }
        return n;
    }

    void checkModuleExports(Node* root, const std::vector<std::u16string>& lexical) {
        std::vector<std::u16string> declared = lexical;
        declared.insert(declared.end(), p_.top->var_names.begin(), p_.top->var_names.end());
        declared.insert(declared.end(), import_names_.begin(), import_names_.end());
        std::vector<std::u16string> exported;
        const auto add = [&](const std::u16string& name) {
            if (std::find(exported.begin(), exported.end(), name) != exported.end()) fail("'" + utf8(name) + "' is exported twice");
            exported.push_back(name);
        };
        for (Node* s : root->kids) {
            if (s->kind != N::ExportDecl) continue;
            if (s->flags & kDefault) {
                add(u"default");
                continue;
            }
            if (s->flags & kNamespace) {
                if (!s->str2.empty()) add(s->str2);
                continue;
            }
            if (s->a) {
                std::vector<std::u16string> names;
                if (s->a->kind == N::VarDecl) for (Node* d : s->a->kids) boundNames(d->a, names);
                else names.push_back(s->a->str);
                for (const auto& n : names) add(n);
                continue;
            }
            for (Node* sp : s->kids) {
                add(sp->str2);
                if (s->str.empty() && std::find(declared.begin(), declared.end(), sp->str) == declared.end())
                    fail("export of '" + utf8(sp->str) + "', which isn't declared");
            }
        }
    }

    bool peekIsFunctionAfterAsync() {
        const Token t = peek();
        return t.type == Tok::Keyword && t.value == u"function" && !t.newline_before;
    }

    bool letStartsDeclaration() {
        const Token t = peek();
        return t.type == Tok::Identifier || (t.type == Tok::Punct && (t.value == u"[" || t.value == u"{")) ||
               (t.type == Tok::Keyword && (t.value == u"yield" || t.value == u"await"));
    }

    Node* statement() {
        Depth guard(*this);
        const uint32_t line = cur_.line;
        const bool top_item = top_item_;
        top_item_ = false;
        const bool loop_body = loop_body_next_;
        loop_body_next_ = false;
        if (is(u"{")) return block();
        if (is(u";")) {
            advance();
            return p_.make(N::Empty, line);
        }
        if (cur_.type == Tok::Keyword) {
            const std::u16string& k = cur_.value;
            if (k == u"var") {
                Node* n = variableDeclaration(kVar, true);
                semicolon();
                return n;
            }
            if (k == u"if") return ifStatement();
            if (k == u"for") return forStatement();
            if (k == u"while") {
                advance();
                Node* n = p_.make(N::While, line);
                expect(u"(");
                n->a = expression();
                expect(u")");
                n->b = loopBody();
                return n;
            }
            if (k == u"do") {
                advance();
                Node* n = p_.make(N::DoWhile, line);
                n->a = loopBody();
                if (!isKw(u"while")) fail("expected while");
                advance();
                expect(u"(");
                n->b = expression();
                expect(u")");
                eat(u";");   // a do-while's semicolon is always optional
                return n;
            }
            if (k == u"return") {
                if (fn_ == p_.top) fail("return outside a function");
                advance();
                Node* n = p_.make(N::Return, line);
                if (!is(u";") && !is(u"}") && cur_.type != Tok::Eof && !cur_.newline_before) n->a = expression();
                semicolon();
                return n;
            }
            if (k == u"break" || k == u"continue") {
                const bool brk = k == u"break";
                advance();
                Node* n = p_.make(brk ? N::Break : N::Continue, line);
                if (cur_.type == Tok::Identifier && !cur_.newline_before) {
                    n->str = cur_.value;
                    if (std::find(labels_.begin(), labels_.end(), n->str) == labels_.end()) fail("no label '" + utf8(n->str) + "' here");
                    advance();
                } else if (brk ? (loop_depth_ == 0 && breakable_depth_ == 0) : loop_depth_ == 0) {
                    fail(brk ? "break outside a loop or switch" : "continue outside a loop");
                }
                if (!brk && loop_depth_ == 0) fail("continue outside a loop");
                semicolon();
                return n;
            }
            if (k == u"throw") {
                advance();
                if (cur_.newline_before) fail("no line break is allowed after throw");
                Node* n = p_.make(N::Throw, line);
                n->a = expression();
                semicolon();
                return n;
            }
            if (k == u"try") return tryStatement();
            if (k == u"switch") return switchStatement();
            if (k == u"debugger") {
                advance();
                semicolon();
                return p_.make(N::Debugger, line);
            }
            if (k == u"function") {
                // In sloppy code, a function declaration where only a statement may go.
                return functionDeclaration();
            }
            if (k == u"with") fail("with isn't supported");
            if (k == u"import" || k == u"export") {
                const Token t = peek();
                if (!(k == u"import" && t.type == Tok::Punct && (t.value == u"(" || t.value == u"."))) {
                    if (!p_.module) fail(utf8(k) + " is only valid in a module");
                    if (!top_item || fn_ != p_.top || lexical_.size() != 1) fail(utf8(k) + " is only valid at a module's top level");
                    return k == u"import" ? importDeclaration() : exportDeclaration();
                }
            }
        }
        // A label.
        if (cur_.type == Tok::Identifier) {
            const Token t = peek();
            if (t.type == Tok::Punct && t.value == u":") {
                Node* n = p_.make(N::Labeled, line);
                n->str = cur_.value;
                checkName(n->str, false);
                if (std::find(labels_.begin(), labels_.end(), n->str) != labels_.end()) fail("the label '" + utf8(n->str) + "' is already in use");
                advance();
                advance();
                labels_.push_back(n->str);
                if (isKw(u"function")) {
                    if (strict_ || loop_body) fail("a labelled function isn't allowed here");
                    n->a = functionDeclaration();
                } else {
                    loop_body_next_ = loop_body;
                    n->a = statement();
                }
                labels_.pop_back();
                return n;
            }
        }
        Node* n = p_.make(N::ExprStmt, line);
        n->a = expression();
        semicolon();
        return n;
    }

    Node* loopBody() {
        if (isKw(u"function") || isKw(u"class") || isKw(u"const") || (isIdent(u"let") && peek().type == Tok::Punct && peek().value == u"[") ||
            (isIdent(u"async") && peekIsFunctionAfterAsync()))
            fail("a declaration isn't allowed as a loop's body");
        ++loop_depth_;
        loop_body_next_ = true;
        Node* body = statement();
        --loop_depth_;
        return body;
    }

    Node* block() {
        Node* n = p_.make(N::Block, cur_.line);
        expect(u"{");
        auto lexical = std::make_shared<std::vector<std::u16string>>();
        pushScope(lexical.get());
        while (!is(u"}")) {
            if (cur_.type == Tok::Eof) fail("missing }");
            n->kids.push_back(statementListItem());
        }
        popScope();
        advance();
        if (!lexical->empty()) n->lexical = lexical;
        return n;
    }

    Node* variableDeclaration(uint32_t kind, bool allow_in) {
        Node* n = p_.make(N::VarDecl, cur_.line);
        n->flags = kind;
        advance();
        do {
            Node* d = p_.make(N::Declarator, cur_.line);
            d->a = bindingTarget();
            std::vector<std::u16string> names;
            boundNames(d->a, names);
            for (const auto& name : names) {
                if (name == u"let" && kind != kVar) fail("let can't be a name here");
                if (kind == kVar) addVarDecl(name);
                else addLexical(name);
            }
            if (eat(u"=")) {
                const bool saved = no_in_;
                no_in_ = !allow_in;
                d->b = assignment();
                no_in_ = saved;
                nameAnonymous(d->b, d->a);
            } else if (kind == kConst && allow_in) {
                fail("const needs a value");
            } else if (d->a->kind != N::Identifier && allow_in) {
                fail("a destructuring declaration needs a value");
            }
            n->kids.push_back(d);
        } while (eat(u","));
        return n;
    }

    Node* lexicalDeclaration(bool needs_semicolon) {
        const uint32_t kind = isKw(u"const") ? kConst : kLet;
        Node* n = variableDeclaration(kind, true);
        if (needs_semicolon) semicolon();
        return n;
    }

    // `function () {}` assigned to a name takes it: function names come from bindings.
    static void nameAnonymous(Node* value, Node* target) {
        if (!value || !target || target->kind != N::Identifier) return;
        if ((value->kind == N::FunctionExpr || value->kind == N::Arrow || value->kind == N::ClassExpr) && value->fn &&
            value->fn->name.empty() && value->str.empty())
            value->fn->name = target->str;
        if (value->kind == N::ClassExpr && value->str.empty() && value->fn) value->fn->name = target->str;
    }

    Node* ifStatement() {
        Node* n = p_.make(N::If, cur_.line);
        advance();
        expect(u"(");
        n->a = expression();
        expect(u")");
        n->b = isKw(u"function") ? functionDeclaration() : statement();
        if (isKw(u"else")) {
            advance();
            n->c = isKw(u"function") ? functionDeclaration() : statement();
        }
        return n;
    }

    Node* forStatement() {
        const uint32_t line = cur_.line;
        advance();
        bool for_await = false;
        if (isIdent(u"await")) {
            if (!fn_->is_async) fail("for await is only valid in async functions and modules");
            for_await = true;
            advance();
        }
        expect(u"(");
        // The loop's own scope, for let and const in its head.
        auto lexical = std::make_shared<std::vector<std::u16string>>();
        pushScope(lexical.get());
        Node* init = nullptr;
        uint32_t decl = 0;
        if (is(u";")) {
        } else if (isKw(u"var") || isKw(u"const") || (isIdent(u"let") && letStartsDeclaration())) {
            decl = isKw(u"var") ? kVar : isKw(u"const") ? kConst : kLet;
            init = variableDeclaration(decl, false);
        } else {
            const bool saved = no_in_;
            no_in_ = true;
            init = expression();
            no_in_ = saved;
        }
        Node* n = nullptr;
        if (init && (isKw(u"in") || isIdent(u"of"))) {
            const bool of = isIdent(u"of");
            n = p_.make(of ? N::ForOf : N::ForIn, line);
            n->flags = decl;
            if (decl) {
                if (init->kids.size() != 1) fail("a for-in or for-of declares one name");
                if (init->kids[0]->b && (of || decl != kVar || strict_ || init->kids[0]->a->kind != N::Identifier)) fail("a for-in or for-of name can't have a value");
                n->a = init->kids[0]->a;
            } else {
                n->a = toPattern(init, false);
            }
            advance();
            n->b = of ? assignment() : expression();
            expect(u")");
            n->c = loopBody();
        } else {
            n = p_.make(N::For, line);
            n->a = init;
            if (init && decl) {
                for (Node* d : init->kids)
                    if (!d->b && (decl == kConst || d->a->kind != N::Identifier)) fail("missing a value in the declaration");
            }
            expect(u";");
            if (!is(u";")) n->b = expression();
            expect(u";");
            if (!is(u")")) n->c = expression();
            expect(u")");
            n->d = loopBody();
        }
        popScope();
        if (!lexical->empty()) n->lexical = lexical;
        if (for_await) {
            if (n->kind != N::ForOf) fail("for await needs of");
            n->flags |= kAwait;
        }
        return n;
    }

    Node* tryStatement() {
        Node* n = p_.make(N::Try, cur_.line);
        advance();
        n->a = block();
        if (isKw(u"catch")) {
            advance();
            if (eat(u"(")) {
                n->b = bindingTarget();
                expect(u")");
            }
            n->c = block();
        }
        if (isKw(u"finally")) {
            advance();
            n->d = block();
        }
        if (!n->c && !n->d) fail("try needs catch or finally");
        return n;
    }

    Node* switchStatement() {
        struct Breakable {
            int& d;
            explicit Breakable(int& depth) : d(depth) { ++d; }
            ~Breakable() { --d; }
        } breakable(breakable_depth_);
        Node* n = p_.make(N::Switch, cur_.line);
        advance();
        expect(u"(");
        n->a = expression();
        expect(u")");
        expect(u"{");
        auto lexical = std::make_shared<std::vector<std::u16string>>();
        pushScope(lexical.get());
        bool seen_default = false;
        while (!eat(u"}")) {
            Node* c = p_.make(N::Case, cur_.line);
            if (isKw(u"case")) {
                advance();
                c->a = expression();
            } else if (isKw(u"default")) {
                if (seen_default) fail("more than one default in a switch");
                seen_default = true;
                advance();
            } else {
                fail("expected case or default");
            }
            expect(u":");
            while (!isKw(u"case") && !isKw(u"default") && !is(u"}")) {
                if (cur_.type == Tok::Eof) fail("missing }");
                c->kids.push_back(statementListItem());
            }
            n->kids.push_back(c);
        }
        popScope();
        if (!lexical->empty()) n->lexical = lexical;
        return n;
    }

    // ── Functions and classes ──

    Node* functionDeclaration() {
        const uint32_t line = cur_.line;
        const uint32_t begin = cur_.begin;
        bool is_async = false;
        if (isIdent(u"async")) {
            is_async = true;
            advance();
        }
        advance();   // function
        const bool generator = eat(u"*");
        if (cur_.type != Tok::Identifier && !(cur_.type == Tok::Keyword && (cur_.value == u"yield" || cur_.value == u"await"))) fail("a function declaration needs a name");
        checkName(cur_.value, true);
        Node* n = p_.make(N::FunctionDecl, line);
        n->str = cur_.value;
        advance();
        n->fn = functionRest(n->str, false, false, begin, is_async, generator);
        // Hoisted: in a function's body, with the var names; in a block, at the block's start.
        if (lexical_.size() <= base_lexical_depth_ + 1) {
            if (p_.module && fn_ == p_.top) {
                addLexical(n->str);   // a module's top-level functions are lexical: no duplicates
            } else {
                const auto& lex = *lexical_.back();
                if (std::find(lex.begin(), lex.end(), n->str) != lex.end()) fail("redeclaration of '" + utf8(n->str) + "'");
                scope_vars_.back().push_back(n->str);
            }
            addVar(n->str);
            fn_->function_decls.push_back(n);
        } else if (!strict_) {
            addVar(n->str);   // and visible to the function, as browsers do (Annex B; not in strict code)
        }
        return n;
    }

    // Parameters and body; the current token is the "(".
    FunctionInfo* functionRest(const std::u16string& name, bool method, bool arrow_unused, uint32_t begin, bool is_async = false, bool generator = false) {
        (void)arrow_unused;
        FunctionInfo* fn = newFunction();
        fn->name = name;
        fn->is_method = method;
        // Known before the body: it decides what `yield` and `await` mean there.
        fn->is_async = is_async;
        fn->is_generator = generator;
        FunctionInfo* outer = fn_;
        const bool outer_strict = strict_;
        const size_t outer_base = base_lexical_depth_;
        const bool derived_ctor = derived_ctor_next_;
        derived_ctor_next_ = false;
        FunctionContext context(*this, false, method, derived_ctor);
        fn_ = fn;
        fn->strict = strict_;
        auto params_scope = std::make_shared<std::vector<std::u16string>>();
        pushScope(params_scope.get());
        base_lexical_depth_ = lexical_.size();
        InAllowed in(*this);
        expect(u"(");
        in_params_ = true;
        formalParameters(fn, u")");
        in_params_ = false;
        expect(u")");
        functionBody(fn);
        popScope();
        base_lexical_depth_ = outer_base;
        fn_ = outer;
        strict_ = outer_strict;
        fn->source = std::u16string(src_.substr(begin, prev_end_ - begin));
        return fn;
    }

    void formalParameters(FunctionInfo* fn, const char16_t* close) {
        bool seen_default = false;
        while (!is(close)) {
            if (eat(u"...")) {
                Node* r = p_.make(N::Rest, cur_.line);
                r->a = bindingTarget();
                fn->params.push_back(r);
                fn->simple_params = false;
                if (!is(close)) fail("a rest parameter must be last");
                break;
            }
            Node* target = bindingTarget();
            if (target->kind != N::Identifier) fn->simple_params = false;
            if (eat(u"=")) {
                Node* ap = p_.make(N::AssignPattern, target->line);
                ap->a = target;
                ap->b = assignment();
                nameAnonymous(ap->b, target);
                target = ap;
                fn->simple_params = false;
                seen_default = true;
            }
            if (!seen_default && target->kind != N::Rest) ++fn->length;
            fn->params.push_back(target);
            if (!is(close)) expect(u",");
        }
    }

    void functionBody(FunctionInfo* fn) {
        expect(u"{");
        Node* body = p_.make(N::Block, cur_.line);
        auto lexical = std::make_shared<std::vector<std::u16string>>();
        pushScope(lexical.get());
        const size_t saved_stop = var_stop_;
        var_stop_ = lexical_.size() - 1;
        const bool saved_top = in_function_;
        in_function_ = true;
        if (directives(fn) && !fn->simple_params) fail("\"use strict\" isn't allowed in a function with non-simple parameters");
        // The parameters can't be declared again with let, const or class.
        {
            std::vector<std::u16string> params;
            for (Node* p : fn->params) boundNames(p, params);
            scope_vars_.back() = params;
        }
        while (!is(u"}")) {
            if (cur_.type == Tok::Eof) fail("missing } at the end of a function");
            body->kids.push_back(statementListItem());
        }
        in_function_ = saved_top;
        var_stop_ = saved_stop;
        popScope();
        advance();
        if (!lexical->empty()) body->lexical = lexical;
        fn->body = body;
        checkParams(fn);
    }

    // Duplicate parameters, and names a strict body forbids (it can make the function
    // strict after its parameters were read).
    void checkParams(FunctionInfo* fn) {
        std::vector<std::u16string> names;
        for (Node* p : fn->params) boundNames(p, names);
        for (size_t i = 0; i < names.size(); ++i) {
            if (fn->strict) {
                if (names[i] == u"eval" || names[i] == u"arguments") fail("can't bind eval or arguments in strict code");
                if (names[i] == u"yield" || strictReserved(names[i])) fail("'" + utf8(names[i]) + "' is reserved in strict code");
            }
            if ((fn->strict || !fn->simple_params || fn->arrow || fn->is_method) &&
                std::find(names.begin() + static_cast<std::ptrdiff_t>(i) + 1, names.end(), names[i]) != names.end())
                fail("duplicate parameter '" + utf8(names[i]) + "'");
        }
    }

    // What a function's body doesn't inherit from around it.
    struct FunctionContext {
        Parser& p;
        std::vector<std::u16string> labels;
        int loops, breakables, non_arrows;
        bool params, super_prop, super_call, top_item, loop_body;
        FunctionContext(Parser& parser, bool arrow, bool method, bool derived_ctor)
            : p(parser), labels(std::move(parser.labels_)), loops(parser.loop_depth_), breakables(parser.breakable_depth_),
              non_arrows(parser.non_arrow_depth_), params(parser.in_params_), super_prop(parser.super_property_ok_),
              super_call(parser.super_call_ok_), top_item(parser.top_item_), loop_body(parser.loop_body_next_) {
            p.labels_.clear();
            p.loop_depth_ = p.breakable_depth_ = 0;
            p.in_params_ = false;
            p.top_item_ = p.loop_body_next_ = false;
            if (!arrow) {
                ++p.non_arrow_depth_;
                p.super_property_ok_ = method;
                p.super_call_ok_ = derived_ctor;
            }
        }
        ~FunctionContext() {
            p.labels_ = std::move(labels);
            p.loop_depth_ = loops;
            p.breakable_depth_ = breakables;
            p.non_arrow_depth_ = non_arrows;
            p.in_params_ = params;
            p.super_property_ok_ = super_prop;
            p.super_call_ok_ = super_call;
            p.top_item_ = top_item;
            p.loop_body_next_ = loop_body;
        }
    };

    Node* classDeclaration() {
        const uint32_t line = cur_.line;
        const uint32_t begin = cur_.begin;
        advance();
        if (cur_.type != Tok::Identifier) fail("a class declaration needs a name");
        {
            const bool saved = strict_;
            strict_ = true;
            checkName(cur_.value, true);
            strict_ = saved;
        }
        Node* n = p_.make(N::ClassDecl, line);
        n->str = cur_.value;
        addLexical(n->str);
        advance();
        classTail(n, begin);
        return n;
    }

    // `extends` and the body; n->a is the superclass, n->kids the methods and fields,
    // n->fn the constructor.
    void classTail(Node* n, uint32_t begin) {
        const bool saved_strict = strict_;
        strict_ = true;   // a class's code is always strict
        if (isKw(u"extends")) {
            advance();
            n->a = leftHandSide();
        }
        expect(u"{");
        FunctionInfo* ctor = nullptr;
        while (!eat(u"}")) {
            if (eat(u";")) continue;
            Node* m = p_.make(N::Method, cur_.line);
            const uint32_t mbegin = cur_.begin;
            if (isIdent(u"static")) {
                const Token t = peek();
                if (!(t.type == Tok::Punct && (t.value == u"(" || t.value == u"="))) {
                    advance();
                    m->flags |= kStatic;
                    if (is(u"{")) fail("static blocks aren't supported yet");
                }
            }
            bool is_async = false, generator = false;
            if (isIdent(u"async")) {
                const Token t = peek();
                if (!(t.type == Tok::Punct && (t.value == u"(" || t.value == u"=")) && !t.newline_before) {
                    is_async = true;
                    advance();
                }
            }
            if (eat(u"*")) generator = true;
            if ((isIdent(u"get") || isIdent(u"set")) && !is_async && !generator) {
                const Token t = peek();
                if (!(t.type == Tok::Punct && (t.value == u"(" || t.value == u"=" || t.value == u";" || t.value == u"}"))) {
                    m->flags |= isIdent(u"get") ? kGetter : kSetter;
                    advance();
                }
            }
            propertyName(m);
            if (is(u"(")) {
                const bool is_ctor = !(m->flags & (kStatic | kComputed)) && m->str == u"constructor" && m->a == nullptr;
                derived_ctor_next_ = is_ctor && n->a != nullptr;
                FunctionInfo* f = functionRest(m->str, true, false, mbegin, is_async, generator);
                f->is_async = is_async;
                f->is_generator = generator;
                if (is_ctor) {
                    if (m->flags & (kGetter | kSetter) || is_async || generator) fail("a class constructor can't be a getter, setter, async or generator");
                    if (ctor) fail("a class has one constructor");
                    ctor = f;
                    continue;
                }
                m->fn = f;
                n->kids.push_back(m);
                continue;
            }
            // A field: `name = value;` or `name;`.
            m->flags |= kField;
            if (eat(u"=")) {
                FunctionInfo* init = newFunction();
                init->is_method = true;
                init->arrow = false;
                init->strict = true;
                FunctionInfo* outer = fn_;
                fn_ = init;
                m->b = assignment();
                fn_ = outer;
                m->fn = init;
            }
            semicolon();
            n->kids.push_back(m);
        }
        if (!ctor) {
            ctor = newFunction();
            ctor->body = p_.make(N::Block, n->line);
            ctor->strict = true;
            if (n->a) {
                // A derived class's default: constructor(...args) { super(...args); }
                Node* rest = p_.make(N::Rest, n->line);
                rest->a = p_.make(N::Identifier, n->line);
                rest->a->str = u"args";
                ctor->params.push_back(rest);
                ctor->simple_params = false;
                Node* call = p_.make(N::Call, n->line);
                call->a = p_.make(N::Super, n->line);
                Node* spread = p_.make(N::Spread, n->line);
                spread->a = p_.make(N::Identifier, n->line);
                spread->a->str = u"args";
                call->kids.push_back(spread);
                Node* stmt = p_.make(N::ExprStmt, n->line);
                stmt->a = call;
                ctor->body->kids.push_back(stmt);
            }
        }
        ctor->name = n->str;
        ctor->is_class_constructor = true;
        ctor->derived = n->a != nullptr;
        for (Node* m : n->kids)
            if ((m->flags & kField) && !(m->flags & kStatic)) ctor->fields.push_back(m);
        ctor->source = std::u16string(src_.substr(begin, prev_end_ - begin));
        n->fn = ctor;
        strict_ = saved_strict;
    }

    // A property's name, into n->str (or n->a for a computed one).
    void propertyName(Node* n) {
        if (eat(u"[")) {
            n->flags |= kComputed;
            n->a = assignment();
            expect(u"]");
            return;
        }
        if (cur_.type == Tok::Identifier || cur_.type == Tok::Keyword || cur_.type == Tok::String) {
            n->str = cur_.value;
        } else if (cur_.type == Tok::PrivateName) {
            n->str = u"#" + cur_.value;
        } else if (cur_.type == Tok::Number) {
            n->str = Runtime::numberToString(cur_.number);
        } else {
            fail("expected a property name");
        }
        advance();
    }

    // ── Patterns ──

    Node* bindingTarget() {
        Depth guard(*this);
        const uint32_t line = cur_.line;
        if (cur_.type == Tok::Identifier || (cur_.type == Tok::Keyword && (cur_.value == u"yield" || cur_.value == u"await"))) {
            Node* n = p_.make(N::Identifier, line);
            n->str = cur_.value;
            checkName(n->str, true);
            advance();
            return n;
        }
        if (is(u"[")) {
            advance();
            Node* n = p_.make(N::ArrayPattern, line);
            while (!eat(u"]")) {
                if (is(u",")) {
                    advance();
                    n->kids.push_back(nullptr);
                    continue;
                }
                if (eat(u"...")) {
                    Node* r = p_.make(N::Rest, cur_.line);
                    r->a = bindingTarget();
                    n->kids.push_back(r);
                    expect(u"]");
                    break;
                }
                Node* t = bindingElement();
                n->kids.push_back(t);
                if (!is(u"]")) expect(u",");
            }
            return n;
        }
        if (is(u"{")) {
            advance();
            Node* n = p_.make(N::ObjectPattern, line);
            while (!eat(u"}")) {
                if (eat(u"...")) {
                    Node* r = p_.make(N::Rest, cur_.line);
                    r->a = bindingTarget();
                    n->kids.push_back(r);
                    expect(u"}");
                    break;
                }
                Node* prop = p_.make(N::PatternProperty, cur_.line);
                const bool plain_name = cur_.type == Tok::Identifier;
                propertyName(prop);
                if (eat(u":")) {
                    prop->b = bindingElement();
                } else {
                    if (!plain_name) fail("expected : in a destructuring pattern");
                    Node* id = p_.make(N::Identifier, prop->line);
                    id->str = prop->str;
                    prop->b = id;
                    if (eat(u"=")) {
                        Node* ap = p_.make(N::AssignPattern, prop->line);
                        ap->a = id;
                        ap->b = assignment();
                        nameAnonymous(ap->b, id);
                        prop->b = ap;
                    }
                }
                n->kids.push_back(prop);
                if (!is(u"}")) expect(u",");
            }
            return n;
        }
        fail("expected a name or a destructuring pattern");
    }

    Node* bindingElement() {
        Node* t = bindingTarget();
        if (eat(u"=")) {
            Node* ap = p_.make(N::AssignPattern, t->line);
            ap->a = t;
            ap->b = assignment();
            nameAnonymous(ap->b, t);
            return ap;
        }
        return t;
    }

    // An expression reinterpreted as an assignment target (the cover grammar).
    Node* toPattern(Node* e, bool binding) {
        if (!e) fail("invalid assignment target");
        switch (e->kind) {
            case N::Identifier:
                if (strict_ && (e->str == u"eval" || e->str == u"arguments")) fail("can't assign to eval or arguments in strict code");
                return e;
            case N::Member:
                if (binding) fail("invalid destructuring target");
                if (e->flags & kOptional) fail("can't assign to an optional chain");
                return e;
            case N::Array: {
                if (e->flags & kParenthesized) fail("invalid assignment target");
                Node* p = p_.make(N::ArrayPattern, e->line);
                for (size_t i = 0; i < e->kids.size(); ++i) {
                    Node* k = e->kids[i];
                    if (!k || k->kind == N::Hole) {
                        p->kids.push_back(nullptr);
                    } else if (k->kind == N::Spread) {
                        if (i + 1 != e->kids.size() || (k->flags & kOptional)) fail("a rest element must be last");
                        if (k->a->kind == N::Assign) fail("a rest element can't have a default");
                        Node* r = p_.make(N::Rest, k->line);
                        r->a = toPattern(k->a, binding);
                        p->kids.push_back(r);
                    } else {
                        p->kids.push_back(toPattern(k, binding));
                    }
                }
                return p;
            }
            case N::Object: {
                if (e->flags & kParenthesized) fail("invalid assignment target");
                Node* p = p_.make(N::ObjectPattern, e->line);
                for (size_t i = 0; i < e->kids.size(); ++i) {
                    Node* prop = e->kids[i];
                    if (prop->flags & kSpreadProp) {
                        if (i + 1 != e->kids.size() || (prop->flags & kOptional)) fail("a rest property must be last");
                        Node* r = p_.make(N::Rest, prop->line);
                        r->a = toPattern(prop->b, binding);
                        p->kids.push_back(r);
                        continue;
                    }
                    if (prop->flags & (kGetter | kSetter | kMethodProp)) fail("invalid destructuring target");
                    Node* pp = p_.make(N::PatternProperty, prop->line);
                    pp->str = prop->str;
                    pp->a = prop->a;
                    pp->flags = prop->flags & kComputed;
                    pp->b = toPattern(prop->b, binding);
                    p->kids.push_back(pp);
                }
                return p;
            }
            case N::Assign:
                if (e->op != Op::None || (e->flags & kParenthesized)) fail("invalid destructuring target");
                {
                    Node* ap = p_.make(N::AssignPattern, e->line);
                    ap->a = toPattern(e->a, binding);
                    ap->b = e->b;
                    nameAnonymous(ap->b, ap->a);
                    return ap;
                }
            case N::AssignPattern: case N::ArrayPattern: case N::ObjectPattern:
                return e;   // converted already (a nested `{} = v` inside a literal)
            default:
                fail("invalid assignment target");
        }
    }

    // ── Expressions ──

    Node* expression() {
        Node* e = assignment();
        if (!is(u",")) return e;
        Node* seq = p_.make(N::Sequence, e->line);
        seq->kids.push_back(e);
        while (eat(u",")) seq->kids.push_back(assignment());
        return seq;
    }

    static Op assignOp(const std::u16string& v) {
        if (v == u"=") return Op::None;
        if (v == u"+=") return Op::Add;
        if (v == u"-=") return Op::Sub;
        if (v == u"*=") return Op::Mul;
        if (v == u"/=") return Op::Div;
        if (v == u"%=") return Op::Mod;
        if (v == u"**=") return Op::Exp;
        if (v == u"<<=") return Op::Shl;
        if (v == u">>=") return Op::Shr;
        if (v == u">>>=") return Op::UShr;
        if (v == u"&=") return Op::BitAnd;
        if (v == u"|=") return Op::BitOr;
        if (v == u"^=") return Op::BitXor;
        if (v == u"&&=") return Op::And;
        if (v == u"||=") return Op::Or;
        if (v == u"?\?=") return Op::Nullish;
        return Op::Eq;   // not an assignment
    }

    // Tries `(params) =>` at the current "(": the arrow, or null with nothing consumed.
    Node* tryArrowParams(bool is_async, uint32_t begin) {
        const size_t pos = lex_.pos();
        const uint32_t line = lex_.line();
        const Token saved = cur_;
        const uint32_t saved_end = prev_end_;
        const bool saved_started = started_;
        FunctionInfo* fn = newFunction();
        fn->arrow = true;
        fn->is_async = is_async;
        FunctionInfo* outer = fn_;
        try {
            fn_ = fn;
            fn->strict = strict_;
            advance();   // (
            const bool saved_params = in_params_;
            in_params_ = true;
            try {
                formalParameters(fn, u")");
            } catch (...) {
                in_params_ = saved_params;
                throw;
            }
            in_params_ = saved_params;
            expect(u")");
            if (!is(u"=>") || cur_.newline_before) throw ParseError{"not an arrow", 0};
        } catch (const ParseError&) {
            fn_ = outer;
            lex_.reset(pos, line);
            lex_.error.clear();
            cur_ = saved;
            prev_end_ = saved_end;
            started_ = saved_started;
            return nullptr;
        }
        fn_ = outer;
        return arrowBody(fn, begin);
    }

    Node* arrowBody(FunctionInfo* fn, uint32_t begin) {
        Node* n = p_.make(N::Arrow, cur_.line);
        expect(u"=>");
        FunctionContext context(*this, true, false, false);
        FunctionInfo* outer = fn_;
        const bool outer_strict = strict_;
        const size_t outer_base = base_lexical_depth_;
        fn_ = fn;
        auto params_scope = std::make_shared<std::vector<std::u16string>>();
        pushScope(params_scope.get());
        base_lexical_depth_ = lexical_.size();
        if (is(u"{")) {
            functionBody(fn);
        } else {
            fn->expression_body = true;
            const bool saved_in = no_in_;
            fn->body = assignment();
            no_in_ = saved_in;
            checkParams(fn);
        }
        popScope();
        base_lexical_depth_ = outer_base;
        fn_ = outer;
        strict_ = outer_strict;
        // An arrow's `arguments` and `this` are its enclosing function's.
        if (fn->uses_arguments) outer->uses_arguments = true;
        fn->source = std::u16string(src_.substr(begin, prev_end_ - begin));
        n->fn = fn;
        return n;
    }

    Node* assignment() {
        Depth guard(*this);
        const uint32_t line = cur_.line;
        const uint32_t begin = cur_.begin;
        // Arrows: `x =>`, `(…) =>`, `async x =>`, `async (…) =>`.
        if (cur_.type == Tok::Identifier || (cur_.type == Tok::Keyword && (cur_.value == u"yield" || cur_.value == u"await"))) {
            const Token t = peek();
            if (t.type == Tok::Punct && t.value == u"=>" && !t.newline_before) {
                FunctionInfo* fn = newFunction();
                fn->arrow = true;
                fn->strict = strict_;
                Node* id = p_.make(N::Identifier, line);
                id->str = cur_.value;
                fn->params.push_back(id);
                fn->length = 1;
                advance();
                return arrowBody(fn, begin);
            }
            if (isIdent(u"async") && !t.newline_before) {
                if (t.type == Tok::Identifier) {
                    advance();
                    FunctionInfo* fn = newFunction();
                    fn->arrow = true;
                    fn->is_async = true;
                    fn->strict = strict_;
                    Node* id = p_.make(N::Identifier, cur_.line);
                    id->str = cur_.value;
                    fn->params.push_back(id);
                    fn->length = 1;
                    advance();
                    if (!is(u"=>")) fail("expected =>");
                    return arrowBody(fn, begin);
                }
                if (t.type == Tok::Punct && t.value == u"(") {
                    const size_t pos = lex_.pos();
                    const uint32_t lline = lex_.line();
                    const Token saved = cur_;
                    const uint32_t saved_end = prev_end_;
                    advance();
                    if (Node* arrow = tryArrowParams(true, begin)) return arrow;
                    lex_.reset(pos, lline);
                    cur_ = saved;
                    prev_end_ = saved_end;
                }
            }
        }
        if (is(u"(")) {
            if (Node* arrow = tryArrowParams(false, begin)) return arrow;
        }
        if ((cur_.type == Tok::Identifier && cur_.value == u"yield") && fn_->is_generator) {
            if (cur_.escaped) fail("yield can't be written with escapes");
            if (in_params_) fail("yield isn't allowed in parameters");
            Node* y = p_.make(N::Yield, line);
            advance();
            if (eat(u"*")) y->flags |= kSpreadProp;
            if (!cur_.newline_before && !is(u")") && !is(u"]") && !is(u"}") && !is(u",") && !is(u";") && !is(u":") && cur_.type != Tok::Eof)
                y->a = assignment();
            return y;
        }
        Node* left = conditional();
        if (cur_.type == Tok::Punct) {
            const Op op = assignOp(cur_.value);
            if (op != Op::Eq) {
                Node* n = p_.make(N::Assign, line);
                n->op = op;
                if (op == Op::None && (left->kind == N::Array || left->kind == N::Object)) n->a = toPattern(left, false);
                else if (left->kind == N::Identifier || left->kind == N::Member) n->a = toPattern(left, false);
                else fail("invalid assignment target");
                advance();
                n->b = assignment();
                if (op == Op::None) nameAnonymous(n->b, n->a);
                return n;
            }
        }
        return left;
    }

    Node* conditional() {
        Node* test = binary(0);
        if (!is(u"?")) return test;
        Node* n = p_.make(N::Conditional, test->line);
        advance();
        n->a = test;
        const bool saved = no_in_;
        no_in_ = false;
        n->b = assignment();
        no_in_ = saved;
        expect(u":");
        n->c = assignment();
        return n;
    }

    // Binary operators by precedence (higher binds tighter).
    int precedence(Op& op, N& kind) const {
        kind = N::Binary;
        if (cur_.type == Tok::Keyword) {
            if (cur_.value == u"instanceof") { op = Op::InstanceOf; return 10; }
            if (cur_.value == u"in" && !no_in_) { op = Op::In; return 10; }
            return -1;
        }
        if (cur_.type != Tok::Punct) return -1;
        const std::u16string& v = cur_.value;
        if (v == u"??") { op = Op::Nullish; kind = N::Logical; return 1; }
        if (v == u"||") { op = Op::Or; kind = N::Logical; return 2; }
        if (v == u"&&") { op = Op::And; kind = N::Logical; return 3; }
        if (v == u"|") { op = Op::BitOr; return 4; }
        if (v == u"^") { op = Op::BitXor; return 5; }
        if (v == u"&") { op = Op::BitAnd; return 6; }
        if (v == u"==") { op = Op::Eq; return 7; }
        if (v == u"!=") { op = Op::Ne; return 7; }
        if (v == u"===") { op = Op::StrictEq; return 7; }
        if (v == u"!==") { op = Op::StrictNe; return 7; }
        if (v == u"<") { op = Op::Lt; return 10; }
        if (v == u">") { op = Op::Gt; return 10; }
        if (v == u"<=") { op = Op::Le; return 10; }
        if (v == u">=") { op = Op::Ge; return 10; }
        if (v == u"<<") { op = Op::Shl; return 11; }
        if (v == u">>") { op = Op::Shr; return 11; }
        if (v == u">>>") { op = Op::UShr; return 11; }
        if (v == u"+") { op = Op::Add; return 12; }
        if (v == u"-") { op = Op::Sub; return 12; }
        if (v == u"*") { op = Op::Mul; return 13; }
        if (v == u"/") { op = Op::Div; return 13; }
        if (v == u"%") { op = Op::Mod; return 13; }
        if (v == u"**") { op = Op::Exp; return 14; }
        return -1;
    }

    Node* binary(int min_prec) {
        Depth guard(*this);
        Node* left = unary();
        while (true) {
            Op op;
            N kind;
            const int prec = precedence(op, kind);
            if (prec < 0 || prec < min_prec) break;
            if (op == Op::Exp && left->kind == N::Unary && !(left->flags & kParenthesized)) fail("parenthesize a unary operand of **");
            const uint32_t line = cur_.line;
            advance();
            // ** is right-associative; the rest left.
            Node* right = binary(op == Op::Exp ? prec : prec + 1);
            // ?? doesn't mix with && or || without parentheses.
            if (kind == N::Logical) {
                const auto mixes = [&](Node* side) {
                    return side->kind == N::Logical && !(side->flags & kParenthesized) && ((op == Op::Nullish) != (side->op == Op::Nullish));
                };
                if (mixes(left) || mixes(right)) fail("?? can't be mixed with && or || without parentheses");
            }
            Node* n = p_.make(kind, line);
            n->op = op;
            n->a = left;
            n->b = right;
            left = n;
        }
        return left;
    }

    Node* unary() {
        Depth guard(*this);
        const uint32_t line = cur_.line;
        Op op = Op::None;
        if (cur_.type == Tok::Punct) {
            const std::u16string& v = cur_.value;
            if (v == u"!") op = Op::Not;
            else if (v == u"-") op = Op::Neg;
            else if (v == u"+") op = Op::Plus;
            else if (v == u"~") op = Op::BitNot;
            else if (v == u"++" || v == u"--") {
                const Op u = v == u"++" ? Op::Inc : Op::Dec;
                advance();
                Node* n = p_.make(N::Update, line);
                n->op = u;
                n->flags = kPrefix;
                n->a = unary();
                if (n->a->kind != N::Identifier && n->a->kind != N::Member) fail("invalid operand for ++ or --");
                toPattern(n->a, false);
                return n;
            }
        } else if (cur_.type == Tok::Keyword) {
            if (cur_.value == u"typeof") op = Op::TypeOf;
            else if (cur_.value == u"void") op = Op::Void;
            else if (cur_.value == u"delete") op = Op::Delete;
        } else if (cur_.type == Tok::Identifier && cur_.value == u"await" && fn_->is_async) {
            if (cur_.escaped) fail("await can't be written with escapes");
            if (in_params_) fail("await isn't allowed in parameters");
            advance();
            Node* n = p_.make(N::Await, line);
            n->a = unary();
            return n;
        }
        if (op != Op::None) {
            advance();
            Node* n = p_.make(N::Unary, line);
            n->op = op;
            n->a = unary();
            if (op == Op::Delete && strict_ && n->a->kind == N::Identifier) fail("can't delete a name in strict code");
            return n;
        }
        Node* e = leftHandSide();
        // Postfix ++ and --, on the same line.
        if (cur_.type == Tok::Punct && (cur_.value == u"++" || cur_.value == u"--") && !cur_.newline_before) {
            if (e->kind != N::Identifier && e->kind != N::Member) fail("invalid operand for ++ or --");
            toPattern(e, false);
            Node* n = p_.make(N::Update, line);
            n->op = cur_.value == u"++" ? Op::Inc : Op::Dec;
            n->a = e;
            advance();
            return n;
        }
        return e;
    }

    std::vector<Node*> arguments() {
        InAllowed in(*this);
        std::vector<Node*> args;
        expect(u"(");
        while (!is(u")")) {
            if (eat(u"...")) {
                Node* s = p_.make(N::Spread, cur_.line);
                s->a = assignment();
                args.push_back(s);
            } else {
                args.push_back(assignment());
            }
            if (!is(u")")) expect(u",");
        }
        advance();
        return args;
    }

    Node* leftHandSide() {
        Depth guard(*this);
        const uint32_t line = cur_.line;
        Node* e;
        if (isKw(u"new")) {
            advance();
            if (eat(u".")) {
                if (!isIdent(u"target")) fail("expected new.target");
                if (non_arrow_depth_ == 0) fail("new.target is only valid in functions");
                advance();
                e = p_.make(N::NewTarget, line);
            } else {
                Node* callee = memberOnly();
                Node* n = p_.make(N::New, line);
                n->a = callee;
                if (is(u"(")) n->kids = arguments();
                e = n;
            }
        } else if (isKw(u"super")) {
            advance();
            e = p_.make(N::Super, line);
            if (!is(u"(") && !is(u".") && !is(u"[")) fail("super must be called or have a property read");
            if (is(u"(") ? !super_call_ok_ : !super_property_ok_) fail("super isn't allowed here");
        } else if (isKw(u"import")) {
            advance();
            if (eat(u".")) {
                if (!isIdent(u"meta")) fail("expected import.meta");
                if (!p_.module) fail("import.meta is only valid in a module");
                advance();
                e = p_.make(N::ImportMeta, line);
            } else {
                expect(u"(");
                e = p_.make(N::ImportCall, line);
                e->a = assignment();
                if (eat(u",") && !is(u")")) {
                    e->b = assignment();   // options ({with: …}): read, not used
                    eat(u",");
                }
                expect(u")");
            }
        } else {
            e = primary();
        }
        return callTail(e, true);
    }

    // `new X.y(...)`: member accesses, no calls.
    Node* memberOnly() {
        Node* e;
        if (isKw(u"new")) {
            const uint32_t line = cur_.line;
            advance();
            if (eat(u".")) {
                if (!isIdent(u"target")) fail("expected new.target");
                if (non_arrow_depth_ == 0) fail("new.target is only valid in functions");
                advance();
                e = p_.make(N::NewTarget, line);
            } else {
                Node* n = p_.make(N::New, line);
                n->a = memberOnly();
                if (is(u"(")) n->kids = arguments();
                e = n;
            }
        } else if (isKw(u"super")) {
            e = p_.make(N::Super, cur_.line);
            advance();
            if (!super_property_ok_) fail("super isn't allowed here");
        } else {
            e = primary();
        }
        return callTail(e, false);
    }

    Node* callTail(Node* e, bool calls) {
        bool optional_chain = false;
        while (true) {
            const uint32_t line = cur_.line;
            if (is(u".")) {
                advance();
                Node* m = p_.make(N::Member, line);
                m->a = e;
                if (cur_.type == Tok::PrivateName) m->str = u"#" + cur_.value;
                else if (cur_.type == Tok::Identifier || cur_.type == Tok::Keyword) m->str = cur_.value;
                else fail("expected a property name after .");
                advance();
                e = m;
            } else if (is(u"?.")) {
                if (!calls) fail("optional chaining isn't allowed in new");
                advance();
                optional_chain = true;
                if (is(u"(")) {
                    Node* c = p_.make(N::Call, line);
                    c->a = e;
                    c->flags |= kOptional;
                    c->kids = arguments();
                    e = c;
                } else if (eat(u"[")) {
                    Node* m = p_.make(N::Member, line);
                    m->a = e;
                    m->flags |= kComputed | kOptional;
                    m->b = expression();
                    expect(u"]");
                    e = m;
                } else {
                    Node* m = p_.make(N::Member, line);
                    m->a = e;
                    m->flags |= kOptional;
                    if (cur_.type == Tok::Identifier || cur_.type == Tok::Keyword) m->str = cur_.value;
                    else if (cur_.type == Tok::PrivateName) m->str = u"#" + cur_.value;
                    else fail("expected a property name after ?.");
                    advance();
                    e = m;
                }
            } else if (is(u"[")) {
                advance();
                Node* m = p_.make(N::Member, line);
                m->a = e;
                m->flags |= kComputed;
                const bool saved = no_in_;
                no_in_ = false;
                m->b = expression();
                no_in_ = saved;
                expect(u"]");
                e = m;
            } else if (calls && is(u"(")) {
                Node* c = p_.make(N::Call, line);
                c->a = e;
                c->kids = arguments();
                e = c;
            } else if (cur_.type == Tok::Template) {
                if (optional_chain) fail("a tagged template can't follow an optional chain");
                Node* t = p_.make(N::TaggedTemplate, line);
                t->a = e;
                t->b = templateLiteral(true);
                e = t;
            } else {
                break;
            }
        }
        if (optional_chain) {
            Node* wrap = p_.make(N::OptionalChain, e->line);
            wrap->a = e;
            return wrap;
        }
        return e;
    }

    Node* templateLiteral(bool tagged) {
        Node* n = p_.make(N::Template, cur_.line);
        while (true) {
            if (cur_.type != Tok::Template) fail("unterminated template");
            if (!cur_.cooked_ok && !tagged) fail("invalid escape in template");
            Node* part = p_.make(N::String, cur_.line);
            part->str = cur_.value;
            part->str2 = cur_.raw;
            if (!cur_.cooked_ok) part->flags |= kOptional;   // cooked is undefined
            n->kids.push_back(part);
            if (cur_.template_tail) {
                advance();
                break;
            }
            advance();
            n->kids.push_back(expression());
            if (!is(u"}")) fail("expected } in template");
            cur_ = lex_.templatePart();
            if (cur_.type == Tok::Eof && !lex_.error.empty()) fail(lex_.error);
        }
        return n;
    }

    Node* primary() {
        Depth guard(*this);
        const uint32_t line = cur_.line;
        const uint32_t begin = cur_.begin;
        switch (cur_.type) {
            case Tok::Number: {
                if (cur_.legacy_octal && strict_) fail("octal literals aren't allowed in strict code");
                Node* n = p_.make(N::Number, line);
                n->number = cur_.number;
                advance();
                return n;
            }
            case Tok::String: {
                Node* n = p_.make(N::String, line);
                n->str = cur_.value;
                advance();
                return n;
            }
            case Tok::Template:
                return templateLiteral(false);
            case Tok::Regex: {
                // A bad pattern is an early error: the script doesn't run at all.
                const auto flags = regex::parseFlags(cur_.raw);
                if (!flags) fail("invalid regular expression flags");
                std::string why;
                if (!regex::compile(cur_.value, *flags, why)) fail("invalid regular expression: " + why);
                Node* n = p_.make(N::Regex, line);
                n->str = cur_.value;
                n->str2 = cur_.raw;
                advance();
                return n;
            }
            case Tok::Identifier: {
                if (isIdent(u"async") && peekIsFunctionAfterAsync()) return functionExpression();
                checkName(cur_.value, false);
                Node* n = p_.make(N::Identifier, line);
                n->str = cur_.value;
                if (n->str == u"arguments") markArguments();
                advance();
                return n;
            }
            case Tok::Keyword: {
                const std::u16string& k = cur_.value;
                if (k == u"this") {
                    advance();
                    return p_.make(N::This, line);
                }
                if (k == u"null") {
                    advance();
                    return p_.make(N::Null, line);
                }
                if (k == u"true" || k == u"false") {
                    Node* n = p_.make(N::Boolean, line);
                    n->flags = k == u"true";
                    advance();
                    return n;
                }
                if (k == u"function") return functionExpression();
                if (k == u"class") {
                    advance();
                    Node* n = p_.make(N::ClassExpr, line);
                    if (cur_.type == Tok::Identifier) {
                        n->str = cur_.value;
                        advance();
                    }
                    classTail(n, begin);
                    return n;
                }
                break;
            }
            case Tok::PrivateName: {
                // `#x in obj`
                Node* n = p_.make(N::String, line);
                n->str = u"#" + cur_.value;
                advance();
                return n;
            }
            case Tok::Punct: {
                if (is(u"(")) {
                    advance();
                    const bool saved = no_in_;
                    no_in_ = false;
                    Node* e = expression();
                    no_in_ = saved;
                    expect(u")");
                    e->flags |= kParenthesized;
                    return e;
                }
                if (is(u"[")) return arrayLiteral();
                if (is(u"{")) return objectLiteral();
                break;
            }
            default:
                break;
        }
        if (cur_.type == Tok::Eof) fail("unexpected end of script");
        fail("unexpected '" + utf8(cur_.value) + "'");
    }

    void markArguments() {
        // The nearest function that isn't an arrow owns `arguments`.
        fn_->uses_arguments = true;
    }

    Node* functionExpression() {
        const uint32_t line = cur_.line;
        const uint32_t begin = cur_.begin;
        bool is_async = false;
        if (isIdent(u"async")) {
            is_async = true;
            advance();
        }
        advance();   // function
        const bool generator = eat(u"*");
        Node* n = p_.make(N::FunctionExpr, line);
        if (cur_.type == Tok::Identifier || (cur_.type == Tok::Keyword && (cur_.value == u"yield" || cur_.value == u"await"))) {
            // An expression's own name follows its own rules for yield and await.
            if (generator && cur_.value == u"yield") fail("yield can't name a generator expression");
            if (is_async && cur_.value == u"await") fail("await can't name an async function expression");
            if (cur_.value == u"yield" && strict_) fail("yield can't be a name here");
            if (strict_ && (cur_.value == u"eval" || cur_.value == u"arguments" || strictReserved(cur_.value))) fail("'" + utf8(cur_.value) + "' can't be a name here");
            if (cur_.value == u"await" && p_.module) fail("await can't be a name here");
            n->str = cur_.value;
            advance();
        }
        n->fn = functionRest(n->str, false, false, begin, is_async, generator);
        return n;
    }

    Node* arrayLiteral() {
        InAllowed in(*this);
        Node* n = p_.make(N::Array, cur_.line);
        advance();
        while (!eat(u"]")) {
            if (is(u",")) {
                advance();
                n->kids.push_back(p_.make(N::Hole, cur_.line));
                continue;
            }
            if (eat(u"...")) {
                Node* s = p_.make(N::Spread, cur_.line);
                s->a = assignment();
                n->kids.push_back(s);
                if (is(u",")) s->flags |= kOptional;   // a comma after: not a rest element
            } else {
                n->kids.push_back(assignment());
            }
            if (!is(u"]")) expect(u",");
        }
        return n;
    }

    Node* objectLiteral() {
        InAllowed in(*this);
        Node* n = p_.make(N::Object, cur_.line);
        advance();
        while (!eat(u"}")) {
            Node* prop = p_.make(N::Property, cur_.line);
            const uint32_t pbegin = cur_.begin;
            if (eat(u"...")) {
                prop->flags |= kSpreadProp;
                prop->b = assignment();
                n->kids.push_back(prop);
                if (is(u",")) prop->flags |= kOptional;
                if (!is(u"}")) expect(u",");
                continue;
            }
            bool is_async = false, generator = false;
            if (isIdent(u"async")) {
                const Token t = peek();
                if (!(t.type == Tok::Punct && (t.value == u"(" || t.value == u":" || t.value == u"," || t.value == u"}" || t.value == u"=")) && !t.newline_before) {
                    is_async = true;
                    advance();
                }
            }
            if (eat(u"*")) generator = true;
            if ((isIdent(u"get") || isIdent(u"set")) && !is_async && !generator) {
                const Token t = peek();
                if (!(t.type == Tok::Punct && (t.value == u"(" || t.value == u":" || t.value == u"," || t.value == u"}" || t.value == u"="))) {
                    prop->flags |= isIdent(u"get") ? kGetter : kSetter;
                    advance();
                }
            }
            const bool plain_name = cur_.type == Tok::Identifier || (cur_.type == Tok::Keyword && (cur_.value == u"yield" || cur_.value == u"await"));
            propertyName(prop);
            if (is(u"(")) {
                prop->flags |= kMethodProp;
                prop->fn = functionRest(prop->str, true, false, pbegin, is_async, generator);
                prop->fn->is_async = is_async;
                prop->fn->is_generator = generator;
                Node* f = p_.make(N::FunctionExpr, prop->line);
                f->fn = prop->fn;
                prop->b = f;
            } else if (prop->flags & (kGetter | kSetter)) {
                fail("a getter or setter needs ()");
            } else if (eat(u":")) {
                prop->b = assignment();
                if (!(prop->flags & kComputed)) {
                    Node* target = p_.make(N::Identifier, prop->line);
                    target->str = prop->str;
                    nameAnonymous(prop->b, target);
                }
            } else if (plain_name && !(prop->flags & kComputed)) {
                // Shorthand `{a}`, or `{a = 1}` that only a pattern may have.
                prop->flags |= kShorthand;
                Node* id = p_.make(N::Identifier, prop->line);
                id->str = prop->str;
                if (id->str == u"arguments") markArguments();
                if (is(u"=")) {
                    advance();
                    Node* as = p_.make(N::Assign, prop->line);
                    as->a = id;
                    as->b = assignment();
                    prop->b = as;
                    cover_init_ = true;
                } else {
                    prop->b = id;
                }
            } else {
                fail("expected : after a property name");
            }
            n->kids.push_back(prop);
            if (!is(u"}")) expect(u",");
        }
        return n;
    }

    std::u16string_view src_;
    Lexer lex_;
    Program& p_;
    Token cur_;
    bool started_ = false;
    uint32_t prev_end_ = 0;
    uint32_t prev_line_ = 1;
    bool strict_;
    bool no_in_ = false;
    bool in_function_ = false;
    bool cover_init_ = false;
    int depth_ = 0;
    FunctionInfo* fn_ = nullptr;
    std::vector<std::vector<std::u16string>*> lexical_;
    size_t base_lexical_depth_ = 1;
    std::vector<std::u16string> import_names_;
    // Early errors' bookkeeping.
    bool top_item_ = false;            // the statement now is a module's top-level item
    bool loop_body_next_ = false;      // the statement now is a loop's body
    bool in_params_ = false;           // in a function's parameters (no yield or await expressions)
    int loop_depth_ = 0;
    int breakable_depth_ = 0;
    std::vector<std::u16string> labels_;
    bool super_property_ok_ = false;   // in a method (or an arrow in one)
    bool super_call_ok_ = false;       // in a derived class's constructor
    bool derived_ctor_next_ = false;
    int non_arrow_depth_ = 0;          // functions that aren't arrows: new.target is allowed
    size_t var_stop_ = 0;              // the lexical scope a var declaration hoists to
    std::vector<std::vector<std::u16string>> scope_vars_;   // per scope in lexical_: var names hoisted through it
};

}  // namespace

std::unique_ptr<Program> parse(std::u16string_view source, std::string& error, bool strict, bool module, const EvalContext* eval) {
    auto prog = std::make_unique<Program>();
    prog->module = module;
    Parser parser(source, *prog, strict);
    if (module) parser.setModule();
    if (eval) parser.setEvalContext(*eval);
    try {
        prog->root = parser.program();
    } catch (const ParseError& e) {
        // On a long (minified) line, the column too.
        size_t start = e.pos < source.size() ? e.pos : source.size();
        while (start > 0 && source[start - 1] != u'\n') --start;
        size_t end = start;
        while (end < source.size() && source[end] != u'\n') ++end;
        std::string where = "line " + std::to_string(e.line);
        if (end - start > 500) where += ", column " + std::to_string(e.pos - start + 1);
        error = "SyntaxError: " + e.message + " (" + where + ")";
        return nullptr;
    }
    return prog;
}

}  // namespace fbx::xweb::js
