// The bytecode VM: a function's tree compiled once to a flat list of register
// instructions, then run in a loop instead of walked node by node.
//
// Where it pays: a function's own variables are registers (slots on the C++ stack, found
// by number), not names in a scope looked up by hash; a call makes no scope at all; and
// each step is one switch, not a recursive walk with a frame per node.
//
// What it runs, so far: functions with plain parameters, no super, no new.target, no
// direct eval, not generators or async. Anything else is the tree-walker's, decided once
// per function when it's first called. The two meet at calls (each side calls the other
// through Runtime::call) and at scopes: a variable a nested function or class may see
// lives in a real scope (an Env, as the tree-walker's), at a place known when compiled;
// the rest are registers. Names from outside are read and written by name, as the
// tree-walker does.
//
// XWEB_NO_VM=1: everything by the tree-walker. XWEB_VM_STATS=1: which functions weren't
// compiled, and why, as they're met.

#include <stdlib.h>   // alloca

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

#include "bigint.h"
#include "interpreter.h"

namespace fbx::xweb::js {

namespace {

enum Opc : uint32_t {
    OpSite,          // n                   : where errors are reported; a statement's step
    OpUndef,         // d
    OpNull,          // d
    OpTrue,          // d
    OpFalse,         // d
    OpEmpty,         // d                   : a let in its dead zone
    OpInt,           // d imm               : a small whole number
    OpConst,         // d k
    OpMov,           // d s
    OpLoadLex,       // d s n               : s, unless in its dead zone (n names it)
    OpCheckLex,      // s n                 : throws if s is in its dead zone
    OpThrowConst,    // n
    OpLoadFree,      // d n                 : a name from the closure's scopes, or a global
    OpLoadCallee,    // d this n            : the same, as a call's callee (a with's object as this)
    OpStoreFree,     // n s
    OpTypeofFree,    // d n
    OpDeleteFree,    // d n
    OpLoadThis,      // d
    OpGetNamed,      // d o n               : o.name (n a Member)
    OpGetElem,       // d o k
    OpSetNamed,      // o n v
    OpSetElem,       // o k v
    OpDeleteNamed,   // d o n
    OpDeleteElem,    // d o k
    OpBin,           // d a b op
    OpNot,           // d s
    OpNeg,           // d s
    OpPlus,          // d s
    OpBitNot,        // d s
    OpTypeof,        // d s
    OpToNumeric,     // d s
    OpInc,           // d s
    OpDec,           // d s
    OpAddInPlace,    // d s                 : d += s, a string appended where it is when it can be
    OpAppendFree,    // d n old r           : name += r for a closure's or global name, old read before r
    OpJump,          // t
    OpJumpIfTrue,    // s t
    OpJumpIfFalse,   // s t
    OpJumpIfNullish, // s t
    OpCall,          // d fn this start argc n
    OpCallArray,     // d fn this arr n     : arguments from an array (a spread)
    OpNew,           // d fn start argc n
    OpNewArray,      // arr start count  -> d is first
    OpArrayNew,      // d
    OpArrayPush,     // a s
    OpArraySpread,   // a s
    OpArrayHole,     // a
    OpNewObject,     // d
    OpDefineNamed,   // o n v               : n a Property
    OpDefineComputed,// o k v
    OpSetProto,      // o v
    OpObjSpread,     // o s
    OpTemplate,      // d n start
    OpRegex,         // d n
    OpThrow,         // s
    OpReturn,        // s
    OpEndFinally,    // kind val outer_kind outer_val outer_start
    OpForInPrep,     // d s
    OpForInNext,     // it d t
    OpLoop,          // (a loop's back edge)
    OpLoadCap,       // d slot n            : a variable kept in the function's scope
    OpStoreCap,      // slot s n
    OpInitCap,       // slot s              : its declaration (out of its dead zone)
    OpAppendCap,     // d slot old r        : slot += r, appended where it is when it can be
    OpClosure,       // d n                 : a function or arrow expression, or a declaration
    OpClass,         // d n
    OpDefineMethod,  // o n k               : a method, getter or setter (k a computed key, or none)
    OpArguments,     // d
    OpCount,
};

constexpr uint32_t kNone = 0xFFFFFFFFu;
// A finally's kinds of leaving: normally, by return, by a throw; then each jump out.
constexpr int kFinNormal = 0, kFinReturn = 1, kFinThrow = 2, kFinJumps = 3;

}  // namespace

struct VmCode {
    std::vector<uint32_t> ops;
    std::vector<Value> consts;
    std::vector<Node*> nodes;
    struct Handler {
        uint32_t start, end, target, reg;
    };
    std::vector<Handler> handlers;   // innermost first
    std::vector<uint32_t> stubs;     // where a finally's jumps out go on
    uint32_t nregs = 0;
    uint32_t nparams = 0;
    // The function's own scope, for what nested functions may see: made at each call
    // when there's anything to keep there, its names in this order.
    bool needs_env = false;
    struct EnvName {
        std::u16string name;
        bool lexical;
        bool is_const;
    };
    std::vector<EnvName> env_names;
};

namespace {

struct Unsupported {
    const char* why;
};

class Compiler {
public:
    explicit Compiler(FunctionInfo* info) : info_(info), code_(std::make_shared<VmCode>()) {}

    std::shared_ptr<VmCode> run() {
        FunctionInfo& f = *info_;
        if (f.is_async || f.is_generator) fail("async or generator");
        if (!f.simple_params) fail("parameter patterns or defaults");
        if (f.own_arguments) fail("f.arguments");
        if (f.is_class_constructor || f.derived || !f.fields.empty()) fail("class constructor");
        if (f.params.size() > 255) fail("too many parameters");
        // What nested functions and classes may see of this one's variables: every name
        // used anywhere inside them (more than they need, never less).
        if (f.expression_body) survey(f.body, false);
        else
            for (Node* k : f.body->kids) survey(k, false);
        if (direct_eval_) fail("direct eval");
        code_->needs_env = makes_functions_;
        scopes_.emplace_back();
        // Parameters first: argument i is register i. (A repeated name: the last one.)
        for (Node* p : f.params) {
            if (p->kind != N::Identifier) fail("parameter pattern");
            scopes_.back()[p->str] = Local{next_++, false, false};
        }
        code_->nparams = next_;
        floor_ = next_;
        // A parameter nested functions see: into the scope at the start.
        for (size_t i = 0; i < f.params.size(); ++i) {
            Local& l = scopes_.back()[f.params[i]->str];
            if (l.reg != i || !captured(f.params[i]->str)) continue;   // (a repeated name's earlier one)
            l.slot = envSlot(f.params[i]->str, false, false);
            emit(OpInitCap, {static_cast<uint32_t>(l.slot), static_cast<uint32_t>(i)});
        }
        // arguments (not an arrow's: that's its enclosing function's, found by name).
        if (f.uses_arguments && !f.arrow && !scopes_.back().count(u"arguments")) {
            Local& l = declareVar(u"arguments");
            if (l.slot >= 0) {
                const uint32_t t = temp();
                emit(OpArguments, {t});
                emit(OpInitCap, {static_cast<uint32_t>(l.slot), t});
                next_ = floor_;
            } else {
                emit(OpArguments, {l.reg});
            }
        }
        for (const auto& name : f.var_names)
            if (!scopes_.back().count(name)) (void)declareVar(name);
        // Function declarations, made now (over a var of the same name).
        for (Node* d : f.function_decls) {
            Local& l = scopes_.back().count(d->str) ? scopes_.back()[d->str] : declareVar(d->str);
            const uint32_t t = temp();
            emit(OpClosure, {t, node(d)});
            if (l.slot >= 0) emit(OpInitCap, {static_cast<uint32_t>(l.slot), t});
            else emit(OpMov, {l.reg, t});
            next_ = floor_;
        }
        floor_ = next_;
        max_ = std::max(max_, next_);
        if (f.expression_body) {
            const uint32_t r = value(f.body);
            emit(OpReturn, {r});
        } else {
            enterLexical(f.body->lexical, f.body->kids, true);
            for (Node* s : f.body->kids) {
                if (s->kind == N::FunctionDecl) continue;   // made at the start
                stmt(s);
            }
            emit(OpUndef, {0});   // (register 0 may be a parameter: the function is done with it)
            if (code_->nparams == 0 && next_ == 0) next_ = 1;
            emit(OpReturn, {0});
        }
        // A finally's jumps out, each on to the next finally or to where it was going.
        for (const Stub& s : stubs_) {
            code_->stubs.push_back(here());
            if (s.next_finally) {
                emit(OpInt, {s.next_kind_reg, static_cast<uint32_t>(s.next_kind)});
                if (s.next_finally->start == kNone) fail("finally not placed");
                jumpTo(OpJump, {}, s.next_finally->start);
            } else {
                patchTo(jumpFwd(OpJump), s.target_label);
            }
        }
        for (Label* l : labels_owned_) resolve(*l);
        if (max_ > 2000) fail("too many registers");
        code_->nregs = std::max(max_, next_);
        if (code_->nregs == 0) code_->nregs = 1;
        return code_;
    }

private:
    struct Local {
        uint32_t reg;
        bool lexical;   // let, const: has a dead zone
        bool is_const;
        int slot = -1;  // kept in the function's scope (a nested function may see it): its place there
    };

    // ── What nested functions see ──

    bool captured(const std::u16string& name) const { return seen_inside_.count(name) != 0; }
    int envSlot(const std::u16string& name, bool lexical, bool is_const) {
        code_->needs_env = true;
        code_->env_names.push_back({name, lexical, is_const});
        return static_cast<int>(code_->env_names.size() - 1);
    }
    // A var-scoped name (a var, arguments, a function declaration): a register, or a place
    // in the scope.
    Local& declareVar(const std::u16string& name) {
        Local& l = scopes_.back()[name];
        l = Local{next_++, false, false};
        if (captured(name)) l.slot = envSlot(name, false, false);
        floor_ = std::max(floor_, next_);
        max_ = std::max(max_, next_);
        return l;
    }
    // Every name used inside nested functions and classes; and whether there's a direct
    // eval anywhere (then any name may be read, by text: not compiled).
    void survey(Node* n, bool inside) {
        if (!n) return;
        if (++survey_depth_ > 2000) fail("nested too deeply");
        struct Out {
            int& d;
            ~Out() { --d; }
        } out{survey_depth_};
        const bool fn_like = n->fn || n->kind == N::ClassExpr || n->kind == N::ClassDecl;
        if (fn_like) makes_functions_ = true;
        const bool in = inside || fn_like;
        if (in && (n->kind == N::Identifier)) seen_inside_.insert(n->str);
        if (n->kind == N::Call && n->a && n->a->kind == N::Identifier && n->a->str == u"eval") direct_eval_ = true;
        // (A shorthand property's value is its name.)
        if (in && n->kind == N::Property && (n->flags & kShorthand)) seen_inside_.insert(n->str);
        survey(n->a, in);
        survey(n->b, in);
        survey(n->c, in);
        survey(n->d, in);
        for (Node* k : n->kids) survey(k, in);
        if (n->fn) {
            for (Node* p : n->fn->params) survey(p, true);
            if (n->fn->body) survey(n->fn->body, true);
            for (Node* fld : n->fn->fields) survey(fld, true);
            // A nested function that uses `arguments` or `this`: they're found in this
            // function's scope by name (arguments) or as its function scope (this).
            if (n->fn->uses_arguments) seen_inside_.insert(u"arguments");
        }
    }
    using Scope = std::unordered_map<std::u16string, Local>;

    // A place in the code jumps go to, known later: the operands to fill in when it is.
    struct Label {
        uint32_t at = kNone;
        std::vector<uint32_t> uses;
    };
    struct Finally;
    // A jump out through finally blocks: from a finally to the next, or to its target.
    struct Stub {
        Finally* next_finally = nullptr;
        uint32_t next_kind_reg = 0;
        int next_kind = 0;
        Label* target_label = nullptr;
    };
    struct Finally {
        uint32_t kind_reg, val_reg;
        std::vector<uint32_t> starts;   // jumps to its start, filled in when it's placed
        uint32_t start = kNone;
    };
    // A statement break and continue can leave: a loop, a switch, a labeled block.
    struct Target {
        std::vector<std::u16string> labels;
        bool loop = false;
        bool breakable = true;   // (a labeled block: only by its label)
        Label* brk = nullptr;
        Label* cont = nullptr;
        size_t finally_depth = 0;
    };

    [[noreturn]] void fail(const char* why) { throw Unsupported{why}; }

    // ── Code ──

    void emit(Opc op, std::initializer_list<uint32_t> args) {
        code_->ops.push_back(op);
        code_->ops.insert(code_->ops.end(), args.begin(), args.end());
    }
    uint32_t here() const { return static_cast<uint32_t>(code_->ops.size()); }
    uint32_t node(Node* n) {
        if (auto it = node_index_.find(n); it != node_index_.end()) return it->second;
        const auto i = static_cast<uint32_t>(code_->nodes.size());
        code_->nodes.push_back(n);
        node_index_[n] = i;
        return i;
    }
    uint32_t constant(Value v) {
        code_->consts.push_back(std::move(v));
        return static_cast<uint32_t>(code_->consts.size() - 1);
    }
    // A jump whose target comes later: where its target operand is.
    uint32_t jumpFwd(Opc op, std::initializer_list<uint32_t> args = {}) {
        emit(op, args);
        code_->ops.push_back(kNone);
        return here() - 1;
    }
    void jumpTo(Opc op, std::initializer_list<uint32_t> args, uint32_t target) {
        emit(op, args);
        code_->ops.push_back(target);
    }
    void patchHere(uint32_t at) { code_->ops[at] = here(); }
    Label* newLabel() {
        labels_owned_.push_back(new Label);
        owned_.emplace_back(labels_owned_.back());
        return labels_owned_.back();
    }
    void place(Label* l) { l->at = here(); }
    void patchTo(uint32_t at, Label* l) { l->uses.push_back(at); }
    void resolve(Label& l) {
        if (l.uses.empty()) return;
        if (l.at == kNone) fail("unplaced label");
        for (uint32_t u : l.uses) code_->ops[u] = l.at;
    }

    // ── Registers ──

    uint32_t temp() {
        const uint32_t r = next_++;
        max_ = std::max(max_, next_);
        return r;
    }
    struct TempMark {
        Compiler& c;
        uint32_t saved;
        explicit TempMark(Compiler& comp) : c(comp), saved(comp.next_) {}
        ~TempMark() { c.next_ = std::max(saved, c.floor_); }
    };

    const Local* findLocal(const std::u16string& name) const {
        for (size_t i = scopes_.size(); i-- > 0;)
            if (auto it = scopes_[i].find(name); it != scopes_[i].end()) return &it->second;
        return nullptr;
    }

    // A block's let, const (and their dead zones): registers of their own, below any temp.
    void enterLexical(const std::vector<std::u16string>* names, const std::vector<Node*>& stmts, bool top = false) {
        scopes_.emplace_back();
        if (!names) return;
        std::unordered_set<std::u16string> consts;
        for (Node* s : stmts)
            if (s && s->kind == N::VarDecl && (s->flags & kConst))
                for (Node* d : s->kids)
                    if (d->a && d->a->kind == N::Identifier) consts.insert(d->a->str);
        for (const auto& name : *names) {
            const uint32_t r = next_++;
            max_ = std::max(max_, next_);
            floor_ = next_;
            Local l{r, true, consts.count(name) != 0};
            if (captured(name)) {
                // (A block's, seen by a closure: each pass through the block, or each turn
                // of a loop, would need a scope of its own. Not yet.)
                if (!top) fail("a block's let or const seen by a nested function");
                l.slot = envSlot(name, true, l.is_const);   // in its dead zone from the start
            } else {
                emit(OpEmpty, {r});
            }
            scopes_.back()[name] = l;
        }
    }
    void leaveLexical() { scopes_.pop_back(); }

    // ── Statements ──

    void site(Node* n) { emit(OpSite, {node(n)}); }

    void stmt(Node* n) {
        if (++depth_ > 400) fail("nested too deeply");
        struct Out {
            int& d;
            ~Out() { --d; }
        } out{depth_};
        TempMark mark(*this);
        switch (n->kind) {
            case N::Empty: case N::Debugger: return;
            case N::ExprStmt: {
                site(n);
                TempMark m(*this);
                (void)value(n->a, true);
                return;
            }
            case N::VarDecl: site(n); declare(n); return;
            case N::Return: {
                site(n);
                const uint32_t r = n->a ? value(n->a) : constUndefined();
                doReturn(r);
                return;
            }
            case N::If: {
                site(n);
                const uint32_t c = value(n->a);
                const uint32_t to_else = jumpFwd(OpJumpIfFalse, {c});
                stmt(n->b);
                if (n->c) {
                    const uint32_t to_end = jumpFwd(OpJump);
                    patchHere(to_else);
                    stmt(n->c);
                    patchHere(to_end);
                } else {
                    patchHere(to_else);
                }
                return;
            }
            case N::Block: {
                enterLexical(n->lexical, n->kids);
                for (Node* k : n->kids) stmt(k);
                leaveLexical();
                return;
            }
            case N::Throw: {
                site(n);
                emit(OpThrow, {value(n->a)});
                return;
            }
            case N::While: case N::DoWhile: case N::For: case N::ForIn: case N::Switch: loopLike(n, {}); return;
            case N::Labeled: {
                std::vector<std::u16string> labels{n->str};
                Node* body = n->a;
                while (body->kind == N::Labeled) {
                    labels.push_back(body->str);
                    body = body->a;
                }
                if (body->kind == N::While || body->kind == N::DoWhile || body->kind == N::For || body->kind == N::ForIn || body->kind == N::Switch) {
                    loopLike(body, labels);
                    return;
                }
                if (body->kind == N::ForOf) fail("for-of");
                Target t;
                t.labels = labels;
                t.breakable = false;
                t.brk = newLabel();
                t.finally_depth = finallies_.size();
                targets_.push_back(t);
                stmt(body);
                targets_.pop_back();
                place(t.brk);
                return;
            }
            case N::Break: case N::Continue: site(n); jumpOut(n); return;
            case N::Try: tryStatement(n); return;
            case N::ForOf: fail("for-of");
            case N::With: fail("with");
            case N::FunctionDecl: fail("a function declared in a block");
            case N::ClassDecl: {
                site(n);
                const Local* l = findLocal(n->str);
                if (!l) fail("class name");
                const uint32_t t = temp();
                emit(OpClass, {t, node(n)});
                initLocal(*l, t);
                return;
            }
            default: fail("a statement the VM doesn't know");
        }
    }

    uint32_t constUndefined() {
        const uint32_t r = temp();
        emit(OpUndef, {r});
        return r;
    }

    // A declaration's value into its variable (out of its dead zone, for a let or const).
    void initLocal(const Local& l, uint32_t v) {
        if (l.slot >= 0) emit(OpInitCap, {static_cast<uint32_t>(l.slot), v});
        else if (l.reg != v) emit(OpMov, {l.reg, v});
    }

    void declare(Node* n) {
        const bool lexical = (n->flags & (kLet | kConst)) != 0;
        for (Node* d : n->kids) {
            if (!d->a || d->a->kind != N::Identifier) fail("declaration pattern");
            const Local* l = findLocal(d->a->str);
            if (!l) fail("declared name not found");
            TempMark m(*this);
            if (l->slot >= 0) {
                if (!d->b && !lexical) continue;
                const uint32_t t = temp();
                if (d->b) expr(d->b, t);
                else emit(OpUndef, {t});
                if (lexical) emit(OpInitCap, {static_cast<uint32_t>(l->slot), t});
                else emit(OpStoreCap, {static_cast<uint32_t>(l->slot), t, node(d->a)});
                continue;
            }
            if (d->b) {
                expr(d->b, l->reg);
            } else if (lexical) {
                emit(OpUndef, {l->reg});
            }
        }
    }

    // Loops and switches: their breaks, continues, and labels.
    void loopLike(Node* n, std::vector<std::u16string> labels) {
        Target t;
        t.labels = std::move(labels);
        t.loop = n->kind != N::Switch;
        t.brk = newLabel();
        t.cont = t.loop ? newLabel() : nullptr;
        t.finally_depth = finallies_.size();
        switch (n->kind) {
            case N::While: {
                site(n);
                Label* top = newLabel();
                place(top);
                const uint32_t c = valueTemp(n->a);
                patchTo(jumpFwd(OpJumpIfFalse, {c}), t.brk);
                targets_.push_back(t);
                stmt(n->b);
                targets_.pop_back();
                place(t.cont);
                emit(OpLoop, {});
                patchTo(jumpFwd(OpJump), top);
                place(t.brk);
                return;
            }
            case N::DoWhile: {
                Label* top = newLabel();
                place(top);
                targets_.push_back(t);
                stmt(n->a);
                targets_.pop_back();
                place(t.cont);
                emit(OpLoop, {});
                const uint32_t c = valueTemp(n->b);
                patchTo(jumpFwd(OpJumpIfTrue, {c}), top);
                place(t.brk);
                return;
            }
            case N::For: {
                site(n);
                enterLexical(n->lexical, {n->a});
                if (n->a) {
                    TempMark m(*this);
                    if (n->a->kind == N::VarDecl) declare(n->a);
                    else (void)value(n->a, true);
                }
                Label* top = newLabel();
                place(top);
                if (n->b) {
                    TempMark m(*this);
                    const uint32_t c = value(n->b);
                    patchTo(jumpFwd(OpJumpIfFalse, {c}), t.brk);
                }
                targets_.push_back(t);
                stmt(n->d);
                targets_.pop_back();
                place(t.cont);
                emit(OpLoop, {});
                if (n->c) {
                    TempMark m(*this);
                    (void)value(n->c, true);
                }
                patchTo(jumpFwd(OpJump), top);
                place(t.brk);
                leaveLexical();
                return;
            }
            case N::ForIn: {
                site(n);
                const uint32_t decl = n->flags & (kVar | kLet | kConst);
                if (n->a->kind != N::Identifier && !(decl == 0 && n->a->kind == N::Member)) fail("for-in pattern");
                // Its own scope for a let or const name, in its dead zone while the
                // subject is worked out.
                enterLexical(n->lexical, {});
                if ((decl & kConst) && n->a->kind == N::Identifier)
                    if (auto it = scopes_.back().find(n->a->str); it != scopes_.back().end()) it->second.is_const = true;
                const uint32_t it = temp();
                floor_ = std::max(floor_, next_);   // (lives for the whole loop)
                {
                    TempMark m(*this);
                    emit(OpForInPrep, {it, value(n->b)});
                }
                Label* top = newLabel();
                place(top);
                {
                    TempMark m(*this);
                    const uint32_t key = temp();
                    patchTo(jumpFwd(OpForInNext, {it, key}), t.brk);
                    if (decl & (kLet | kConst)) {
                        const Local* l = findLocal(n->a->str);
                        if (!l) fail("for-in name");
                        emit(OpMov, {l->reg, key});   // its declaration: out of its dead zone
                    } else {
                        assignTo(n->a, key);
                    }
                }
                targets_.push_back(t);
                stmt(n->c);
                targets_.pop_back();
                place(t.cont);
                emit(OpLoop, {});
                patchTo(jumpFwd(OpJump), top);
                place(t.brk);
                leaveLexical();
                return;
            }
            case N::Switch: {
                site(n);
                const uint32_t d = temp();
                floor_ = std::max(floor_, next_);
                expr(n->a, d);
                std::vector<Node*> all;
                for (Node* c : n->kids) all.insert(all.end(), c->kids.begin(), c->kids.end());
                enterLexical(n->lexical, all);
                std::vector<Label*> bodies;
                for (size_t i = 0; i < n->kids.size(); ++i) bodies.push_back(newLabel());
                for (size_t i = 0; i < n->kids.size(); ++i) {
                    Node* c = n->kids[i];
                    if (!c->a) continue;
                    TempMark m(*this);
                    const uint32_t v = value(c->a);
                    const uint32_t eq = temp();
                    emit(OpBin, {eq, d, v, static_cast<uint32_t>(Op::StrictEq)});
                    patchTo(jumpFwd(OpJumpIfTrue, {eq}), bodies[i]);
                }
                Label* none = t.brk;
                for (size_t i = 0; i < n->kids.size(); ++i)
                    if (!n->kids[i]->a) none = bodies[i];
                patchTo(jumpFwd(OpJump), none);
                targets_.push_back(t);
                for (size_t i = 0; i < n->kids.size(); ++i) {
                    place(bodies[i]);
                    for (Node* s : n->kids[i]->kids) stmt(s);
                }
                targets_.pop_back();
                leaveLexical();
                place(t.brk);
                return;
            }
            default: fail("loop");
        }
    }

    // break and continue: to their target, through any finally blocks on the way.
    void jumpOut(Node* n) {
        const bool is_break = n->kind == N::Break;
        Target* t = nullptr;
        for (size_t i = targets_.size(); i-- > 0;) {
            Target& c = targets_[i];
            if (n->str.empty()) {
                if (is_break ? c.breakable : c.loop) {
                    t = &c;
                    break;
                }
            } else if (std::find(c.labels.begin(), c.labels.end(), n->str) != c.labels.end()) {
                if (!is_break && !c.loop) fail("continue to a non-loop");
                t = &c;
                break;
            }
        }
        if (!t) fail("break or continue target");
        Label* dest = is_break ? t->brk : t->cont;
        if (finallies_.size() == t->finally_depth) {
            patchTo(jumpFwd(OpJump), dest);
            return;
        }
        // The chain from the outermost crossed finally inward: each stub hands on to the next.
        int next_kind = -1;
        Finally* next_f = nullptr;
        for (size_t i = t->finally_depth; i < finallies_.size(); ++i) {
            Finally* f = finallies_[i];
            Stub s;
            if (next_f) {
                s.next_finally = next_f;
                s.next_kind_reg = next_f->kind_reg;
                s.next_kind = next_kind;
            } else {
                s.target_label = dest;
            }
            stubs_.push_back(s);
            next_kind = kFinJumps + static_cast<int>(stubs_.size() - 1);
            next_f = f;
        }
        // next_f is now the innermost finally, next_kind its kind.
        emit(OpInt, {next_f->kind_reg, static_cast<uint32_t>(next_kind)});
        next_f->starts.push_back(jumpFwd(OpJump));
    }

    void doReturn(uint32_t r) {
        if (finallies_.empty()) {
            emit(OpReturn, {r});
            return;
        }
        Finally* f = finallies_.back();
        emit(OpMov, {f->val_reg, r});
        emit(OpInt, {f->kind_reg, kFinReturn});
        f->starts.push_back(jumpFwd(OpJump));
    }

    void tryStatement(Node* n) {
        Finally* fin = nullptr;
        if (n->d) {
            fin = new Finally;
            owned_finallies_.emplace_back(fin);
            fin->kind_reg = temp();
            fin->val_reg = temp();
            floor_ = std::max(floor_, next_);
            finallies_.push_back(fin);
        }
        const uint32_t try_start = here();
        stmt(n->a);
        uint32_t to_after = jumpFwd(OpJump);   // past the catch
        const uint32_t try_end = here();
        uint32_t catch_end = try_end;
        if (n->c) {
            const uint32_t exc = temp();
            floor_ = std::max(floor_, next_);
            const uint32_t handler = here();
            if (n->b) {
                if (n->b->kind != N::Identifier) fail("catch pattern");
                if (captured(n->b->str)) fail("a catch name seen by a nested function");
                scopes_.emplace_back();
                scopes_.back()[n->b->str] = Local{exc, false, false};
            }
            stmt(n->c);
            if (n->b) scopes_.pop_back();
            code_->handlers.push_back({try_start, try_end, handler, exc});
            catch_end = here();
        }
        patchHere(to_after);
        if (fin) {
            finallies_.pop_back();
            // Leaving normally: through the finally with nothing pending.
            emit(OpInt, {fin->kind_reg, kFinNormal});
            const uint32_t to_fin = jumpFwd(OpJump);
            // A throw in the try or the catch: kept, through the finally, thrown again.
            const uint32_t throw_entry = here();
            emit(OpInt, {fin->kind_reg, kFinThrow});
            code_->handlers.push_back({try_start, catch_end, throw_entry, fin->val_reg});
            patchHere(to_fin);
            fin->start = here();
            for (uint32_t s : fin->starts) code_->ops[s] = fin->start;
            fin->starts.clear();
            stmt(n->d);
            // Then on: as it was leaving. A return or jump out goes on through any outer
            // finally (the stubs for jumps were made with that one in mind).
            Finally* outer = finallies_.empty() ? nullptr : finallies_.back();
            if (outer) {
                emit(OpEndFinally, {fin->kind_reg, fin->val_reg, outer->kind_reg, outer->val_reg});
                outer->starts.push_back(here());
                code_->ops.push_back(kNone);
            } else {
                emit(OpEndFinally, {fin->kind_reg, fin->val_reg, kNone, kNone, kNone});
            }
        }
    }

    // ── Expressions ──

    // Whether evaluating it may assign a variable (then a variable read before it is
    // copied first, not read later from its register).
    bool assigns(Node* n) {
        if (!n) return false;
        if (auto it = assigns_.find(n); it != assigns_.end()) return it->second;
        bool r = n->kind == N::Assign || n->kind == N::Update ||
                 (n->kind == N::Unary && n->op == Op::Delete);
        if (!r) r = assigns(n->a) || assigns(n->b) || assigns(n->c) || assigns(n->d);
        if (!r)
            for (Node* k : n->kids)
                if (assigns(k)) {
                    r = true;
                    break;
                }
        assigns_[n] = r;
        return r;
    }

    // A register holding n's value: a variable's own when it's that (and nothing after it
    // in `later` may change it first), else a new temporary.
    uint32_t value(Node* n, bool discard = false, std::initializer_list<Node*> later = {}) {
        if (n->kind == N::Identifier) {
            if (const Local* l = findLocal(n->str); l && !l->lexical && l->slot < 0) {
                bool safe = true;
                for (Node* k : later)
                    if (assigns(k)) safe = false;
                if (safe) return l->reg;
            }
        }
        (void)discard;
        const uint32_t r = temp();
        expr(n, r);
        return r;
    }
    uint32_t valueTemp(Node* n) {
        const uint32_t r = temp();
        expr(n, r);
        return r;
    }

    void expr(Node* n, uint32_t dst) {
        if (++depth_ > 400) fail("nested too deeply");
        struct Out {
            int& d;
            ~Out() { --d; }
        } out{depth_};
        switch (n->kind) {
            case N::Number: emitNumber(dst, n->number); return;
            case N::String: emit(OpConst, {dst, constant(Value::string(n->str))}); return;
            case N::Boolean: emit((n->flags & 1) ? OpTrue : OpFalse, {dst}); return;
            case N::Null: emit(OpNull, {dst}); return;
            case N::Undefined: emit(OpUndef, {dst}); return;
            case N::BigInt: {
                const auto b = bigint::parseLiteral(n->str);
                if (!b) fail("bigint literal");
                emit(OpConst, {dst, constant(Value::bigint(JSString::make(bigint::encode(*b))))});
                return;
            }
            case N::Identifier: loadName(n, dst); return;
            case N::This: emit(OpLoadThis, {dst}); return;
            case N::Template: {
                TempMark m(*this);
                std::vector<Node*> subs;
                for (size_t i = 1; i < n->kids.size(); i += 2) subs.push_back(n->kids[i]);
                const uint32_t start = next_;
                for (size_t i = 0; i < subs.size(); ++i) (void)temp();
                for (size_t i = 0; i < subs.size(); ++i) expr(subs[i], start + static_cast<uint32_t>(i));
                emit(OpTemplate, {dst, node(n), start});
                return;
            }
            case N::Regex: emit(OpRegex, {dst, node(n)}); return;
            case N::Array: arrayLiteral(n, dst); return;
            case N::Object: objectLiteral(n, dst); return;
            case N::Unary: unary(n, dst); return;
            case N::Update: update(n, dst); return;
            case N::Assign: assign(n, dst); return;
            case N::Conditional: {
                TempMark m(*this);
                const uint32_t c = value(n->a);
                const uint32_t to_else = jumpFwd(OpJumpIfFalse, {c});
                expr(n->b, dst);
                const uint32_t to_end = jumpFwd(OpJump);
                patchHere(to_else);
                expr(n->c, dst);
                patchHere(to_end);
                return;
            }
            case N::Sequence: {
                for (size_t i = 0; i + 1 < n->kids.size(); ++i) {
                    TempMark m(*this);
                    (void)value(n->kids[i], true);
                }
                expr(n->kids.back(), dst);
                return;
            }
            case N::Logical: {
                // The left side into dst, kept if it decides; else the right side there.
                // (Not straight into a variable's register: the right side may read it.)
                if (dst < floor_) {
                    TempMark m(*this);
                    const uint32_t t = temp();
                    expr(n, t);
                    emit(OpMov, {dst, t});
                    return;
                }
                expr(n->a, dst);
                uint32_t skip;
                if (n->op == Op::And) skip = jumpFwd(OpJumpIfFalse, {dst});
                else if (n->op == Op::Or) skip = jumpFwd(OpJumpIfTrue, {dst});
                else {
                    const uint32_t take = jumpFwd(OpJumpIfNullish, {dst});
                    skip = jumpFwd(OpJump);
                    patchHere(take);
                }
                {
                    TempMark m(*this);
                    expr(n->b, dst);
                }
                patchHere(skip);
                return;
            }
            case N::Binary: {
                TempMark m(*this);
                const uint32_t l = value(n->a, false, {n->b});
                const uint32_t r = value(n->b);
                emit(OpBin, {dst, l, r, static_cast<uint32_t>(n->op)});
                return;
            }
            case N::Member: {
                if (n->a->kind == N::Super) fail("super");
                TempMark m(*this);
                const uint32_t o = value(n->a, false, {n->b});
                if (n->flags & kOptional) shortIfNullish(o);
                if (n->flags & kComputed) {
                    const uint32_t k = value(n->b);
                    emit(OpGetElem, {dst, o, k});
                } else {
                    emit(OpGetNamed, {dst, o, node(n)});
                }
                return;
            }
            case N::OptionalChain: {
                Label* saved = short_;
                Label* sc = newLabel();
                short_ = sc;
                expr(n->a, dst);
                short_ = saved;
                const uint32_t to_end = jumpFwd(OpJump);
                place(sc);
                emit(OpUndef, {dst});
                patchHere(to_end);
                return;
            }
            case N::Call: call(n, dst); return;
            case N::New: {
                TempMark m(*this);
                const uint32_t fn = value(n->a, false, {});
                const uint32_t fn_kept = assignsAny(n->kids) ? copyOf(fn) : fn;
                if (hasSpread(n->kids)) fail("new with spread");
                const uint32_t start = next_;
                for (size_t i = 0; i < n->kids.size(); ++i) (void)temp();
                for (size_t i = 0; i < n->kids.size(); ++i) expr(n->kids[i], start + static_cast<uint32_t>(i));
                emit(OpNew, {dst, fn_kept, start, static_cast<uint32_t>(n->kids.size()), node(n)});
                return;
            }
            case N::FunctionExpr: case N::Arrow: emit(OpClosure, {dst, node(n)}); return;
            case N::ClassExpr: emit(OpClass, {dst, node(n)}); return;
            case N::TaggedTemplate: fail("tagged template");
            case N::Super: case N::NewTarget: fail("super or new.target");
            case N::Yield: case N::Await: fail("yield or await");
            case N::ImportCall: case N::ImportMeta: fail("import");
            default: fail("an expression the VM doesn't know");
        }
    }

    bool assignsAny(const std::vector<Node*>& ks) {
        for (Node* k : ks)
            if (assigns(k)) return true;
        return false;
    }
    static bool hasSpread(const std::vector<Node*>& ks) {
        for (Node* k : ks)
            if (k->kind == N::Spread) return true;
        return false;
    }
    uint32_t copyOf(uint32_t r) {
        const uint32_t t = temp();
        emit(OpMov, {t, r});
        return t;
    }

    void emitNumber(uint32_t dst, double d) {
        if (d >= 0 && d < 1e9 && d == std::floor(d) && !(d == 0 && std::signbit(d))) emit(OpInt, {dst, static_cast<uint32_t>(d)});
        else emit(OpConst, {dst, constant(Value::number(d))});
    }

    void shortIfNullish(uint32_t r) {
        if (!short_) fail("optional link outside a chain");
        patchTo(jumpFwd(OpJumpIfNullish, {r}), short_);
    }

    void loadName(Node* n, uint32_t dst) {
        if (const Local* l = findLocal(n->str)) {
            if (l->slot >= 0) emit(OpLoadCap, {dst, static_cast<uint32_t>(l->slot), node(n)});
            else if (l->lexical) emit(OpLoadLex, {dst, l->reg, node(n)});
            else if (l->reg != dst) emit(OpMov, {dst, l->reg});
            return;
        }
        if (n->str == u"arguments" && !info_->arrow) fail("arguments");
        emit(OpLoadFree, {dst, node(n)});
    }

    // A value into a name or a member: a for-in's key, an assignment's target.
    void assignTo(Node* t, uint32_t v) {
        if (t->kind == N::Identifier) {
            storeName(t, v);
            return;
        }
        if (t->kind == N::Member) {
            if (t->a->kind == N::Super) fail("super");
            TempMark m(*this);
            const uint32_t o = value(t->a);
            if (t->flags & kComputed) emit(OpSetElem, {o, value(t->b), v});
            else emit(OpSetNamed, {o, node(t), v});
            return;
        }
        fail("assignment pattern");
    }

    void storeName(Node* t, uint32_t v) {
        if (const Local* l = findLocal(t->str)) {
            if (l->slot >= 0) {
                if (l->is_const) {
                    const uint32_t x = temp();
                    emit(OpLoadCap, {x, static_cast<uint32_t>(l->slot), node(t)});   // (its dead zone first)
                    emit(OpThrowConst, {node(t)});
                    return;
                }
                emit(OpStoreCap, {static_cast<uint32_t>(l->slot), v, node(t)});
                return;
            }
            if (l->is_const) {
                emit(OpCheckLex, {l->reg, node(t)});
                emit(OpThrowConst, {node(t)});
                return;
            }
            if (l->lexical) emit(OpCheckLex, {l->reg, node(t)});
            if (l->reg != v) emit(OpMov, {l->reg, v});
            return;
        }
        if (t->str == u"arguments" && !info_->arrow) fail("arguments");
        emit(OpStoreFree, {node(t), v});
    }

    void assign(Node* n, uint32_t dst) {
        Node* t = n->a;
        if (t->kind != N::Identifier && t->kind != N::Member) fail("assignment pattern");
        if (t->kind == N::Member && t->a->kind == N::Super) fail("super");
        TempMark m(*this);
        if (n->op == Op::None) {
            if (t->kind == N::Identifier) {
                expr(n->b, dst);
                storeName(t, dst);
                return;
            }
            const uint32_t o = value(t->a, false, {t->b, n->b});
            if (t->flags & kComputed) {
                const uint32_t k = value(t->b, false, {n->b});
                expr(n->b, dst);
                emit(OpSetElem, {o, k, dst});
            } else {
                expr(n->b, dst);
                emit(OpSetNamed, {o, node(t), dst});
            }
            return;
        }
        const bool logical = n->op == Op::And || n->op == Op::Or || n->op == Op::Nullish;
        if (t->kind == N::Identifier) {
            const Local* l = findLocal(t->str);
            if (l && l->slot >= 0 && !l->is_const && n->op == Op::Add) {
                const uint32_t old = temp();
                emit(OpLoadCap, {old, static_cast<uint32_t>(l->slot), node(t)});
                const uint32_t r = value(n->b);
                emit(OpAppendCap, {dst, static_cast<uint32_t>(l->slot), old, r});
                return;
            }
            if (l && !l->lexical && l->slot < 0 && !logical) {
                if (n->op == Op::Add && !assigns(n->b)) {
                    const uint32_t r = value(n->b);
                    emit(OpAddInPlace, {l->reg, r});
                } else {
                    const uint32_t old = copyOf(l->reg);
                    const uint32_t r = value(n->b);
                    emit(OpBin, {l->reg, old, r, static_cast<uint32_t>(n->op)});
                }
                if (dst != l->reg) emit(OpMov, {dst, l->reg});
                return;
            }
            if (!l && n->op == Op::Add) {
                if (t->str == u"arguments" && !info_->arrow) fail("arguments");
                const uint32_t old = temp();
                emit(OpLoadFree, {old, node(t)});
                const uint32_t r = value(n->b);
                emit(OpAppendFree, {dst, node(t), old, r});
                return;
            }
            loadName(t, dst);
            if (logical) {
                const uint32_t skip = logicalSkip(n->op, dst);
                expr(n->b, dst);
                storeName(t, dst);
                patchHere(skip);
                return;
            }
            const uint32_t r = value(n->b);
            emit(OpBin, {dst, dst, r, static_cast<uint32_t>(n->op)});
            storeName(t, dst);
            return;
        }
        // A member: its object and key once, read, operate, write.
        const uint32_t o = value(t->a, false, {t->b, n->b});
        uint32_t k = kNone;
        if (t->flags & kComputed) {
            k = temp();
            expr(t->b, k);
            emit(OpGetElem, {dst, o, k});
        } else {
            emit(OpGetNamed, {dst, o, node(t)});
        }
        uint32_t skip = kNone;
        if (logical) {
            skip = logicalSkip(n->op, dst);
            expr(n->b, dst);
        } else {
            const uint32_t r = value(n->b);
            emit(OpBin, {dst, dst, r, static_cast<uint32_t>(n->op)});
        }
        if (k != kNone) emit(OpSetElem, {o, k, dst});
        else emit(OpSetNamed, {o, node(t), dst});
        if (skip != kNone) patchHere(skip);
    }
    // &&= ||= ??=: past the write when the current value decides.
    uint32_t logicalSkip(Op op, uint32_t cur) {
        if (op == Op::And) return jumpFwd(OpJumpIfFalse, {cur});
        if (op == Op::Or) return jumpFwd(OpJumpIfTrue, {cur});
        const uint32_t take = jumpFwd(OpJumpIfNullish, {cur});
        const uint32_t skip = jumpFwd(OpJump);
        patchHere(take);
        return skip;
    }

    void update(Node* n, uint32_t dst) {
        Node* t = n->a;
        const bool prefix = (n->flags & kPrefix) != 0;
        const Opc step = n->op == Op::Inc ? OpInc : OpDec;
        TempMark m(*this);
        if (t->kind == N::Identifier) {
            const Local* l = findLocal(t->str);
            if (l && !l->is_const && l->slot < 0) {
                if (l->lexical) emit(OpCheckLex, {l->reg, node(t)});
                if (prefix) {
                    emit(step, {l->reg, l->reg});
                    if (dst != l->reg) emit(OpMov, {dst, l->reg});
                } else {
                    const uint32_t old = dst == l->reg ? temp() : dst;
                    emit(OpToNumeric, {old, l->reg});
                    emit(step, {l->reg, old});
                    if (old != dst) emit(OpMov, {dst, old});
                }
                return;
            }
            const uint32_t cur = temp();
            loadName(t, cur);
            const uint32_t old = temp();
            emit(OpToNumeric, {old, cur});
            emit(step, {cur, old});
            storeName(t, cur);
            emit(OpMov, {dst, prefix ? cur : old});
            return;
        }
        if (t->kind != N::Member || t->a->kind == N::Super) fail("update target");
        const uint32_t o = value(t->a, false, {t->b});
        uint32_t k = kNone;
        const uint32_t cur = temp();
        if (t->flags & kComputed) {
            k = value(t->b);
            emit(OpGetElem, {cur, o, k});
        } else {
            emit(OpGetNamed, {cur, o, node(t)});
        }
        const uint32_t old = temp();
        emit(OpToNumeric, {old, cur});
        emit(step, {cur, old});
        if (k != kNone) emit(OpSetElem, {o, k, cur});
        else emit(OpSetNamed, {o, node(t), cur});
        emit(OpMov, {dst, prefix ? cur : old});
    }

    void unary(Node* n, uint32_t dst) {
        TempMark m(*this);
        switch (n->op) {
            case Op::TypeOf:
                if (n->a->kind == N::Identifier && !findLocal(n->a->str)) {
                    if (n->a->str == u"arguments" && !info_->arrow) fail("arguments");
                    emit(OpTypeofFree, {dst, node(n->a)});
                    return;
                }
                emit(OpTypeof, {dst, value(n->a)});
                return;
            case Op::Void: (void)value(n->a, true); emit(OpUndef, {dst}); return;
            case Op::Not: emit(OpNot, {dst, value(n->a)}); return;
            case Op::Neg: emit(OpNeg, {dst, value(n->a)}); return;
            case Op::Plus: emit(OpPlus, {dst, value(n->a)}); return;
            case Op::BitNot: emit(OpBitNot, {dst, value(n->a)}); return;
            case Op::Delete: {
                Node* t = n->a;
                if (t->kind == N::OptionalChain) fail("delete of a chain");
                if (t->kind == N::Member) {
                    if (t->a->kind == N::Super) fail("super");
                    const uint32_t o = value(t->a, false, {t->b});
                    if (t->flags & kOptional) fail("delete a?.b");
                    if (t->flags & kComputed) emit(OpDeleteElem, {dst, o, value(t->b)});
                    else emit(OpDeleteNamed, {dst, o, node(t)});
                    return;
                }
                if (t->kind == N::Identifier) {
                    if (findLocal(t->str)) emit(OpFalse, {dst});   // a declared variable: not deleted
                    else emit(OpDeleteFree, {dst, node(t)});
                    return;
                }
                (void)value(t, true);
                emit(OpTrue, {dst});
                return;
            }
            default: fail("unary operator");
        }
    }

    void call(Node* n, uint32_t dst) {
        Node* callee = n->a;
        if (callee->kind == N::Super) fail("super()");
        TempMark m(*this);
        const uint32_t fn = temp();
        const uint32_t self = temp();
        Label* saved_short = short_;
        Label* chain = nullptr;
        if (callee->kind == N::Member || (callee->kind == N::OptionalChain && callee->a->kind == N::Member && !(callee->flags & kParenthesized))) {
            Node* mem = callee->kind == N::Member ? callee : callee->a;
            if (mem->a->kind == N::Super) fail("super.f()");
            if (callee->kind == N::OptionalChain) {
                chain = newLabel();
                short_ = chain;
            }
            expr(mem->a, self);
            if (mem->flags & kOptional) shortIfNullish(self);
            if (mem->flags & kComputed) {
                TempMark k(*this);
                emit(OpGetElem, {fn, self, value(mem->b)});
            } else {
                emit(OpGetNamed, {fn, self, node(mem)});
            }
        } else if (callee->kind == N::Identifier && !(callee->flags & kParenthesized) && !findLocal(callee->str)) {
            if (callee->str == u"arguments" && !info_->arrow) fail("arguments");
            emit(OpLoadCallee, {fn, self, node(callee)});
        } else {
            expr(callee, fn);
            emit(OpUndef, {self});
        }
        if (n->flags & kOptional) shortIfNullish(fn);
        if (hasSpread(n->kids)) {
            const uint32_t arr = temp();
            emit(OpArrayNew, {arr});
            for (Node* k : n->kids) {
                TempMark e(*this);
                if (k->kind == N::Spread) emit(OpArraySpread, {arr, value(k->a)});
                else emit(OpArrayPush, {arr, value(k)});
            }
            emit(OpCallArray, {dst, fn, self, arr, node(n)});
        } else {
            const uint32_t start = next_;
            for (size_t i = 0; i < n->kids.size(); ++i) (void)temp();
            for (size_t i = 0; i < n->kids.size(); ++i) expr(n->kids[i], start + static_cast<uint32_t>(i));
            emit(OpCall, {dst, fn, self, start, static_cast<uint32_t>(n->kids.size()), node(n)});
        }
        if (chain) {
            short_ = saved_short;
            const uint32_t to_end = jumpFwd(OpJump);
            place(chain);
            emit(OpUndef, {dst});
            patchHere(to_end);
        }
    }

    void arrayLiteral(Node* n, uint32_t dst) {
        TempMark m(*this);
        bool simple = true;
        for (Node* k : n->kids)
            if (k->kind == N::Spread || k->kind == N::Hole) simple = false;
        if (simple && n->kids.size() <= 64) {
            const uint32_t start = next_;
            for (size_t i = 0; i < n->kids.size(); ++i) (void)temp();
            for (size_t i = 0; i < n->kids.size(); ++i) expr(n->kids[i], start + static_cast<uint32_t>(i));
            emit(OpNewArray, {dst, start, static_cast<uint32_t>(n->kids.size())});
            return;
        }
        const uint32_t a = temp();
        emit(OpArrayNew, {a});
        for (Node* k : n->kids) {
            TempMark e(*this);
            if (k->kind == N::Hole) emit(OpArrayHole, {a});
            else if (k->kind == N::Spread) emit(OpArraySpread, {a, value(k->a)});
            else emit(OpArrayPush, {a, value(k)});
        }
        emit(OpMov, {dst, a});
    }

    void objectLiteral(Node* n, uint32_t dst) {
        TempMark m(*this);
        const uint32_t o = temp();
        emit(OpNewObject, {o});
        for (Node* p : n->kids) {
            TempMark e(*this);
            if (p->flags & kSpreadProp) {
                emit(OpObjSpread, {o, value(p->b)});
                continue;
            }
            if (p->flags & (kGetter | kSetter | kMethodProp)) {
                uint32_t k = kNone;
                if (p->flags & kComputed) {
                    k = temp();
                    expr(p->a, k);
                }
                emit(OpDefineMethod, {o, node(p), k});
                continue;
            }
            if (p->flags & kComputed) {
                const uint32_t k = temp();
                expr(p->a, k);
                const uint32_t v = value(p->b);
                emit(OpDefineComputed, {o, k, v});
                continue;
            }
            if (p->str == u"__proto__" && !(p->flags & kShorthand)) {
                emit(OpSetProto, {o, value(p->b)});
                continue;
            }
            emit(OpDefineNamed, {o, node(p), value(p->b)});
        }
        emit(OpMov, {dst, o});
    }

    FunctionInfo* info_;
    std::shared_ptr<VmCode> code_;
    std::vector<Scope> scopes_;
    uint32_t next_ = 0, max_ = 0, floor_ = 0;
    int depth_ = 0;
    std::unordered_map<Node*, uint32_t> node_index_;
    std::unordered_map<Node*, bool> assigns_;
    std::vector<Label*> labels_owned_;
    std::vector<std::unique_ptr<Label>> owned_;
    std::vector<std::unique_ptr<Finally>> owned_finallies_;
    std::vector<Finally*> finallies_;
    std::vector<Stub> stubs_;
    std::vector<Target> targets_;
    Label* short_ = nullptr;
    std::unordered_set<std::u16string> seen_inside_;
    bool makes_functions_ = false;
    bool direct_eval_ = false;
    int survey_depth_ = 0;
};

bool vmOff() {
    static const bool off = std::getenv("XWEB_NO_VM") != nullptr;
    return off;
}
bool vmStats() {
    static const bool on = std::getenv("XWEB_VM_STATS") != nullptr;
    return on;
}

}  // namespace

VmCode* vmCodeFor(FunctionInfo* info) {
    if (info->vm_state == 1) return info->vm.get();
    if (info->vm_state == 2) return nullptr;
    info->vm_state = 2;
    if (vmOff()) return nullptr;
    Compiler c(info);
    try {
        info->vm = c.run();
        info->vm_state = 1;
        return info->vm.get();
    } catch (const Unsupported& u) {
        if (vmStats()) {
            std::string text = utf8(std::u16string_view(info->source).substr(0, 80));
            for (char& ch : text)
                if (ch == '\n') ch = ' ';
            std::fprintf(stderr, "[vm] not compiled (%s): %s\n", u.why, text.c_str());
        }
        return nullptr;
    }
}

// ── Running ──

Value Interpreter::runVm(Object* f, VmCode& code, const Value& this_in, std::span<const Value> args, Object* new_target) {
    // As deep as the tree-walker lets calls go, and no deeper than the stack.
    struct Depth {
        Runtime& rt;
        explicit Depth(Runtime& x) : rt(x) {
            if (++rt.eval_depth > 1'000'000 || stackTooDeep(rt, static_cast<char*>(__builtin_frame_address(0)))) {
                --rt.eval_depth;
                rt.throwError("RangeError", "too much recursion");
            }
        }
        ~Depth() { --rt.eval_depth; }
    } depth(rt_);
    FunctionData& fd = *f->code;
    FunctionInfo* info = fd.info;
    Env* const closure = fd.closure;
    rt_.env_stack.push_back(closure);   // (a root while it runs, as a call's scope is)
    struct EnvPop {
        Runtime& rt;
        ~EnvPop() { rt.env_stack.pop_back(); }
    } env_pop{rt_};
    const bool strict = info->strict;
    // The registers, on this stack: a collection mid-script finds what they hold there.
    const uint32_t n = code.nregs;
    Value* const r = static_cast<Value*>(alloca(sizeof(Value) * n));
    for (uint32_t i = 0; i < n; ++i) new (&r[i]) Value();
    struct Regs {
        Value* r;
        uint32_t n;
        ~Regs() {
            for (uint32_t i = 0; i < n; ++i) r[i].~Value();
        }
    } regs_guard{r, n};
    const size_t na = std::min<size_t>(args.size(), code.nparams);
    for (size_t i = 0; i < na; ++i) r[i] = args[i];

    // this: worked out the first time it's asked for.
    Value this_value;
    bool this_ready = false;
    const auto thisOf = [&]() -> const Value& {
        if (this_ready) return this_value;
        this_ready = true;
        if (info->arrow) this_value = thisValue(closure);
        else if (strict) this_value = this_in;
        else if (this_in.isNullish() || this_in.isEmpty()) this_value = Value::object(rt_.global());
        else this_value = Value::object(rt_.toObject(this_in));
        return this_value;
    };
    if (new_target) {
        const Value proto = rt_.get(Value::object(new_target), u"prototype");
        this_value = Value::object(rt_.newObject(proto.isObject() ? proto.asObject() : rt_.object_prototype));
        this_ready = true;
    }
    // Its own scope, when nested functions may see into it: what they see of `this`,
    // new.target and super (as the tree-walker's function scope), and the variables they
    // may read, in their dead zone if they're let or const.
    Env* env = closure;
    if (code.needs_env) {
        env = rt_.allocate<Env>(closure);
        if (!info->arrow) {
            env->is_function = true;
            env->function = f;
            env->home = fd.home;
            env->new_target = new_target;
            env->this_value = thisOf();
        }
        env->vars.reserve(code.env_names.size());
        for (const VmCode::EnvName& e : code.env_names) {
            Binding b{Value::undefined(), !e.is_const, !e.lexical};
            env->vars.bind(e.name, VarMap::hashOf(e.name)) = b;
        }
        rt_.env_stack.back() = env;   // (the closure is reachable from it)
    }
    const auto cap = [&](uint32_t slot) -> Binding& { return env->vars.entryAt(slot)->second; };

    const uint32_t* const ops = code.ops.data();
    Node* const* const nodes = code.nodes.data();
    const Value* const consts = code.consts.data();
    uint32_t pc = 0;
    uint32_t at = 0;   // where the instruction running now starts (for its handler)

    const auto nameError = [&](Node* nm) {
        rt_.throwError("ReferenceError", "can't use '" + utf8(nm->str) + "' before it's declared");
    };
    const auto calleeName = [&](Node* call) {
        Node* c = call->a;
        if (c->kind == N::OptionalChain) c = c->a;
        return c->kind == N::Identifier ? utf8(c->str) : c->kind == N::Member && !(c->flags & kComputed) ? utf8(c->str) : std::string("expression");
    };

    Value result;
    for (;;) {
        bool caught = false;
        Value exception;
        try {
            for (;;) {
                at = pc;
                const uint32_t* o = ops + pc;
                switch (static_cast<Opc>(o[0])) {
                    case OpSite: {
                        rt_.step();
                        here(nodes[o[1]]);
                        pc += 2;
                        break;
                    }
                    case OpUndef: r[o[1]] = Value::undefined(); pc += 2; break;
                    case OpNull: r[o[1]] = Value::null(); pc += 2; break;
                    case OpTrue: r[o[1]] = Value::boolean(true); pc += 2; break;
                    case OpFalse: r[o[1]] = Value::boolean(false); pc += 2; break;
                    case OpEmpty: r[o[1]] = Value::empty(); pc += 2; break;
                    case OpInt: r[o[1]] = Value::number(static_cast<double>(o[2])); pc += 3; break;
                    case OpConst: r[o[1]] = consts[o[2]]; pc += 3; break;
                    case OpMov: r[o[1]] = r[o[2]]; pc += 3; break;
                    case OpLoadLex: {
                        if (r[o[2]].isEmpty()) nameError(nodes[o[3]]);
                        r[o[1]] = r[o[2]];
                        pc += 4;
                        break;
                    }
                    case OpCheckLex: {
                        if (r[o[1]].isEmpty()) nameError(nodes[o[2]]);
                        pc += 3;
                        break;
                    }
                    case OpThrowConst: rt_.throwError("TypeError", "assignment to constant '" + utf8(nodes[o[1]]->str) + "'"); pc += 2; break;
                    case OpLoadFree: {
                        Node* nm = nodes[o[2]];
                        if (Binding* b = cachedBinding(closure, nm); b && b->initialized) r[o[1]] = b->value;
                        else r[o[1]] = lookup(closure, nm->str, nameHash(nm), nullptr, nm);
                        pc += 3;
                        break;
                    }
                    case OpLoadCallee: {
                        Node* nm = nodes[o[3]];
                        if (Binding* b = cachedBinding(closure, nm); b && b->initialized) {
                            r[o[1]] = b->value;
                            r[o[2]] = Value::undefined();
                        } else {
                            Object* with = nullptr;
                            if (!findBinding(closure, nm->str, nullptr, nullptr, &with) && with) {
                                r[o[2]] = Value::object(with);
                                r[o[1]] = rt_.get(r[o[2]], PropertyKey(nm->str));
                            } else {
                                r[o[2]] = Value::undefined();
                                r[o[1]] = lookup(closure, nm->str, nameHash(nm), nullptr, nm);
                            }
                        }
                        pc += 4;
                        break;
                    }
                    case OpStoreFree: {
                        Node* nm = nodes[o[1]];
                        assignName(closure, nm->str, r[o[2]], strict, nm);
                        pc += 3;
                        break;
                    }
                    case OpTypeofFree: {
                        Node* nm = nodes[o[2]];
                        Object* with = nullptr;
                        if (!findBinding(closure, nm->str, nullptr, nullptr, &with) && !with && !rt_.has(globalObject(closure), nm->str))
                            r[o[1]] = Value::string(u"undefined");
                        else
                            r[o[1]] = rt_.typeOf(lookup(closure, nm->str, nameHash(nm), nullptr, nm));
                        pc += 3;
                        break;
                    }
                    case OpDeleteFree: {
                        Node* nm = nodes[o[2]];
                        Object* with = nullptr;
                        if (findBinding(closure, nm->str, nullptr, nullptr, &with)) r[o[1]] = Value::boolean(false);
                        else if (with) r[o[1]] = Value::boolean(rt_.deleteProperty(with, nm->str));
                        else r[o[1]] = Value::boolean(rt_.deleteProperty(globalObject(closure), nm->str));
                        pc += 3;
                        break;
                    }
                    case OpLoadThis: r[o[1]] = thisOf(); pc += 2; break;
                    case OpGetNamed: {
                        Node* m = nodes[o[3]];
                        if (!m->key) m->key = std::make_unique<PropertyKey>(m->str);
                        r[o[1]] = rt_.get(r[o[2]], *m->key);
                        pc += 4;
                        break;
                    }
                    case OpGetElem: {
                        const Value& base = r[o[2]];
                        const Value& k = r[o[3]];
                        if (k.isNumber() && base.isObject() && base.asObject()->cls == ObjectClass::Array) {
                            const Object* a = base.asObject();
                            const double d = k.asNumber();
                            if (d >= 0 && d < static_cast<double>(a->elements.size())) {
                                const auto i = static_cast<size_t>(d);
                                if (static_cast<double>(i) == d && !a->elements[i].isEmpty()) {
                                    r[o[1]] = a->elements[i];
                                    pc += 4;
                                    break;
                                }
                            }
                        }
                        r[o[1]] = rt_.get(base, rt_.toPropertyKey(k));
                        pc += 4;
                        break;
                    }
                    case OpSetNamed: {
                        Node* m = nodes[o[2]];
                        if (!m->key) m->key = std::make_unique<PropertyKey>(m->str);
                        rt_.set(r[o[1]], *m->key, r[o[3]], strict);
                        pc += 4;
                        break;
                    }
                    case OpSetElem: {
                        const Value& base = r[o[1]];
                        const Value& k = r[o[2]];
                        if (k.isNumber() && base.isObject() && base.asObject()->cls == ObjectClass::Array) {
                            Object* a = base.asObject();
                            const double d = k.asNumber();
                            if (a->extensible && d >= 0 && d < static_cast<double>(a->elements.size())) {
                                const auto i = static_cast<size_t>(d);
                                if (static_cast<double>(i) == d && !a->elements[i].isEmpty()) {
                                    a->elements[i] = r[o[3]];
                                    pc += 4;
                                    break;
                                }
                            }
                        }
                        rt_.set(base, rt_.toPropertyKey(k), r[o[3]], strict);
                        pc += 4;
                        break;
                    }
                    case OpDeleteNamed: case OpDeleteElem: {
                        PropertyKey key;
                        if (o[0] == OpDeleteNamed) key = PropertyKey(nodes[o[3]]->str);
                        else key = rt_.toPropertyKey(r[o[3]]);
                        Object* obj = rt_.toObject(r[o[2]]);
                        const bool ok = rt_.deleteProperty(obj, key);
                        if (!ok && strict) rt_.throwError("TypeError", "can't delete property '" + (key.isSymbol() ? std::string("symbol") : utf8(key.name)) + "'");
                        r[o[1]] = Value::boolean(ok);
                        pc += 4;
                        break;
                    }
                    case OpBin: r[o[1]] = binaryOp(static_cast<Op>(o[4]), r[o[2]], r[o[3]]); pc += 5; break;
                    case OpNot: r[o[1]] = Value::boolean(!rt_.toBoolean(r[o[2]])); pc += 3; break;
                    case OpNeg: case OpBitNot: {
                        Value v = r[o[2]];
                        if (!v.isNumber()) v = rt_.toPrimitive(v, "number");
                        if (v.isBigInt()) {
                            const bigint::Big x = bigint::decode(v.bigintDigits());
                            r[o[1]] = Value::bigint(JSString::make(bigint::encode(o[0] == OpNeg ? bigint::negate(x) : bigint::bitNot(x))));
                        } else if (o[0] == OpNeg) {
                            r[o[1]] = Value::number(-rt_.toNumber(v));
                        } else {
                            r[o[1]] = Value::number(~rt_.toInt32(v));
                        }
                        pc += 3;
                        break;
                    }
                    case OpPlus: r[o[1]] = Value::number(rt_.toNumber(r[o[2]])); pc += 3; break;
                    case OpTypeof: r[o[1]] = rt_.typeOf(r[o[2]]); pc += 3; break;
                    case OpToNumeric: {
                        const Value& v = r[o[2]];
                        if (v.isNumber()) r[o[1]] = v;
                        else {
                            const Value p = rt_.toPrimitive(v, "number");
                            r[o[1]] = p.isBigInt() ? p : Value::number(rt_.toNumber(p));
                        }
                        pc += 3;
                        break;
                    }
                    case OpInc: case OpDec: {
                        const Value& v = r[o[2]];
                        const double delta = o[0] == OpInc ? 1 : -1;
                        if (v.isNumber()) {
                            r[o[1]] = Value::number(v.asNumber() + delta);
                        } else {
                            const Value p = v.isBigInt() ? v : rt_.toPrimitive(v, "number");
                            if (p.isBigInt()) {
                                const bigint::Big x = bigint::decode(p.bigintDigits());
                                r[o[1]] = Value::bigint(JSString::make(bigint::encode(bigint::add(x, bigint::fromInt64(o[0] == OpInc ? 1 : -1)))));
                            } else {
                                r[o[1]] = Value::number(rt_.toNumber(p) + delta);
                            }
                        }
                        pc += 3;
                        break;
                    }
                    case OpAddInPlace: {
                        Value& d = r[o[1]];
                        const Value& s = r[o[2]];
                        if (d.isNumber() && s.isNumber()) {
                            d = Value::number(d.asNumber() + s.asNumber());
                        } else if (d.isString() && s.isString() && d.stringUnshared()) {
                            std::u16string& mine = d.mutableString();
                            const std::u16string& add = s.asString();
                            if (mine.size() + add.size() > (1u << 29)) rt_.throwError("RangeError", "string too long");
                            if (&mine == &add) mine += std::u16string(add);
                            else mine += add;
                        } else {
                            d = binaryOp(Op::Add, d, s);
                        }
                        pc += 3;
                        break;
                    }
                    case OpAppendFree: {
                        // As the tree-walker's name += string: appended where the name's value
                        // is when nothing else holds it (else a quadratic copy each time).
                        Node* nm = nodes[o[2]];
                        Value& old = r[o[3]];
                        const Value& rhs = r[o[4]];
                        Env* where = nullptr;
                        Object* with = nullptr;
                        Binding* b = findBinding(closure, nm->str, &where, nullptr, &with, nameHash(nm), nm);
                        if (b && !with && b->is_mutable && b->initialized && !b->function_name && rhs.isString() && b->value.isString() &&
                            b->value.sameString(old)) {
                            old = Value();
                            if (b->value.stringUnshared()) {
                                std::u16string& mine = b->value.mutableString();
                                const std::u16string& add = rhs.asString();
                                if (mine.size() + add.size() > (1u << 29)) rt_.throwError("RangeError", "string too long");
                                if (&mine == &add) mine += std::u16string(add);
                                else mine += add;
                                r[o[1]] = b->value;
                                pc += 5;
                                break;
                            }
                            old = b->value;
                        }
                        const Value v = binaryOp(Op::Add, old, rhs);
                        assignName(closure, nm->str, v, strict, nm);
                        r[o[1]] = v;
                        pc += 5;
                        break;
                    }
                    case OpJump: pc = o[1]; break;
                    case OpJumpIfTrue: pc = rt_.toBoolean(r[o[1]]) ? o[2] : pc + 3; break;
                    case OpJumpIfFalse: pc = rt_.toBoolean(r[o[1]]) ? pc + 3 : o[2]; break;
                    case OpJumpIfNullish: pc = r[o[1]].isNullish() ? o[2] : pc + 3; break;
                    case OpCall: {
                        Node* call = nodes[o[6]];
                        here(call);
                        const Value& fn = r[o[2]];
                        if (!rt_.isCallable(fn)) rt_.throwError("TypeError", calleeName(call) + " is not a function");
                        Value v = rt_.call(fn, r[o[3]], std::span<const Value>(r + o[4], o[5]));
                        r[o[1]] = std::move(v);
                        pc += 7;
                        break;
                    }
                    case OpCallArray: {
                        Node* call = nodes[o[5]];
                        here(call);
                        const Value& fn = r[o[2]];
                        if (!rt_.isCallable(fn)) rt_.throwError("TypeError", calleeName(call) + " is not a function");
                        const Object* a = r[o[4]].asObject();
                        const std::vector<Value> list = a->elements;   // (the call may change the array)
                        Value v = rt_.call(fn, r[o[3]], list);
                        r[o[1]] = std::move(v);
                        pc += 6;
                        break;
                    }
                    case OpNew: {
                        Node* nw = nodes[o[5]];
                        here(nw);
                        const Value& fn = r[o[2]];
                        if (!rt_.isConstructor(fn)) {
                            Node* c = nw->a;
                            std::string what = c->kind == N::Identifier ? utf8(c->str) : c->kind == N::Member && !(c->flags & kComputed) ? utf8(c->str) : "expression";
                            rt_.throwError("TypeError", what + " is not a constructor");
                        }
                        Value v = rt_.construct(fn, std::span<const Value>(r + o[3], o[4]));
                        r[o[1]] = std::move(v);
                        pc += 6;
                        break;
                    }
                    case OpNewArray: {
                        std::vector<Value> items(r + o[2], r + o[2] + o[3]);
                        r[o[1]] = Value::object(rt_.newArray(std::move(items)));
                        pc += 4;
                        break;
                    }
                    case OpArrayNew: r[o[1]] = Value::object(rt_.newArray({})); pc += 2; break;
                    case OpArrayPush: r[o[1]].asObject()->elements.push_back(r[o[2]]); pc += 3; break;
                    case OpArrayHole: r[o[1]].asObject()->elements.push_back(Value::empty()); pc += 2; break;
                    case OpArraySpread: {
                        const std::vector<Value> items = rt_.iterateToList(r[o[2]]);
                        auto& el = r[o[1]].asObject()->elements;
                        el.insert(el.end(), items.begin(), items.end());
                        pc += 3;
                        break;
                    }
                    case OpNewObject: r[o[1]] = Value::object(rt_.newObject()); pc += 2; break;
                    case OpDefineNamed: {
                        Node* p = nodes[o[2]];
                        if (!p->key) p->key = std::make_unique<PropertyKey>(p->str);
                        r[o[1]].asObject()->define(*p->key, {r[o[3]]});
                        pc += 4;
                        break;
                    }
                    case OpDefineComputed: {
                        const PropertyKey key = rt_.toPropertyKey(r[o[2]]);
                        r[o[1]].asObject()->define(key, {r[o[3]]});
                        pc += 4;
                        break;
                    }
                    case OpSetProto: {
                        const Value& v = r[o[2]];
                        Object* obj = r[o[1]].asObject();
                        if (v.isObject()) obj->prototype = v.asObject();
                        else if (v.isNull()) obj->prototype = nullptr;
                        pc += 3;
                        break;
                    }
                    case OpObjSpread: {
                        const Value src = r[o[2]];
                        if (!src.isNullish()) {
                            Object* obj = r[o[1]].asObject();
                            Object* so = rt_.toObject(src);
                            for (const PropertyKey& k : rt_.ownPropertyKeys(so)) {
                                Property prop;
                                if (!rt_.getOwnProperty(so, k, prop) || !(prop.flags & kEnumerable)) continue;
                                obj->define(k, {rt_.get(so, k, src)});
                            }
                        }
                        pc += 3;
                        break;
                    }
                    case OpTemplate: {
                        Node* t = nodes[o[2]];
                        std::u16string out;
                        uint32_t sub = o[3];
                        for (size_t i = 0; i < t->kids.size(); ++i) {
                            if (i % 2 == 0) {
                                out += t->kids[i]->str;
                            } else {
                                const Value& v = r[sub++];
                                if (v.isSymbol()) rt_.throwError("TypeError", "can't convert a symbol to a string");
                                if (v.isString()) out += v.asString();
                                else out += *rt_.toString(v);
                            }
                        }
                        r[o[1]] = Value::string(std::move(out));
                        pc += 4;
                        break;
                    }
                    case OpRegex: {
                        Node* rx = nodes[o[2]];
                        r[o[1]] = Value::object(rt_.newRegExp(rx->str, rx->str2));
                        pc += 3;
                        break;
                    }
                    case OpThrow: throw Thrown{r[o[1]]};
                    case OpReturn: {
                        result = r[o[1]];
                        goto done;
                    }
                    case OpEndFinally: {
                        const int kind = static_cast<int>(r[o[1]].asNumber());
                        if (kind == kFinNormal) {
                            pc += 6;
                            break;
                        }
                        if (kind == kFinThrow) throw Thrown{r[o[2]]};
                        if (kind == kFinReturn) {
                            if (o[3] == kNone) {
                                result = r[o[2]];
                                goto done;
                            }
                            r[o[4]] = r[o[2]];
                            r[o[3]] = Value::number(kFinReturn);
                            pc = o[5];
                            break;
                        }
                        pc = code.stubs[static_cast<size_t>(kind - kFinJumps)];
                        break;
                    }
                    case OpForInPrep: {
                        const Value subject = r[o[2]];
                        Object* it = rt_.allocate<Object>(nullptr, ObjectClass::Internal);
                        if (!subject.isNullish()) {
                            // Enumerable string keys, own then inherited, each name once.
                            Object* obj = rt_.toObject(subject);
                            std::unordered_set<std::u16string> seen;
                            for (Object* cur = obj; cur; cur = rt_.getPrototypeOf(cur)) {
                                for (const PropertyKey& k : rt_.ownPropertyKeys(cur)) {
                                    if (k.isSymbol() || !seen.insert(k.name).second) continue;
                                    Property p;
                                    if (!rt_.getOwnProperty(cur, k, p) || !(p.flags & kEnumerable)) continue;
                                    it->elements.push_back(Value::string(k.name));
                                }
                            }
                        }
                        r[o[1]] = Value::object(it);
                        pc += 3;
                        break;
                    }
                    case OpForInNext: {
                        Object* it = r[o[1]].asObject();
                        if (it->cursor >= it->elements.size()) {
                            pc = o[3];
                            break;
                        }
                        r[o[2]] = it->elements[it->cursor++];
                        pc += 4;
                        break;
                    }
                    case OpLoop: rt_.safePoint(); pc += 1; break;
                    case OpLoadCap: {
                        const Binding& b = cap(o[2]);
                        if (!b.initialized) nameError(nodes[o[3]]);
                        r[o[1]] = b.value;
                        pc += 4;
                        break;
                    }
                    case OpStoreCap: {
                        Binding& b = cap(o[1]);
                        if (!b.initialized) nameError(nodes[o[3]]);
                        b.value = r[o[2]];
                        pc += 4;
                        break;
                    }
                    case OpInitCap: {
                        Binding& b = cap(o[1]);
                        b.value = r[o[2]];
                        b.initialized = true;
                        pc += 3;
                        break;
                    }
                    case OpAppendCap: {
                        Binding& b = cap(o[2]);
                        Value& old = r[o[3]];
                        const Value& rhs = r[o[4]];
                        if (!b.initialized) rt_.throwError("ReferenceError", "can't use '" + utf8(code.env_names[o[2]].name) + "' before it's declared");
                        if (rhs.isString() && b.value.isString() && b.value.sameString(old)) {
                            old = Value();
                            if (b.value.stringUnshared()) {
                                std::u16string& mine = b.value.mutableString();
                                const std::u16string& add = rhs.asString();
                                if (mine.size() + add.size() > (1u << 29)) rt_.throwError("RangeError", "string too long");
                                if (&mine == &add) mine += std::u16string(add);
                                else mine += add;
                                r[o[1]] = b.value;
                                pc += 5;
                                break;
                            }
                            old = b.value;
                        }
                        Value v = binaryOp(Op::Add, old, rhs);
                        Binding& again = cap(o[2]);   // (entries don't move: the same one)
                        again.value = v;
                        r[o[1]] = std::move(v);
                        pc += 5;
                        break;
                    }
                    case OpClosure: {
                        Node* fnode = nodes[o[2]];
                        if (fnode->kind == N::FunctionExpr && !fnode->str.empty()) {
                            // A named function expression sees its own name.
                            Env* named = rt_.allocate<Env>(env);
                            Object* fn = makeFunction(fnode->fn, named, nullptr);
                            Binding self{Value::object(fn), false, true};
                            self.function_name = true;
                            named->vars[fnode->str] = self;
                            r[o[1]] = Value::object(fn);
                        } else {
                            r[o[1]] = Value::object(makeFunction(fnode->fn, env, nullptr));
                        }
                        pc += 3;
                        break;
                    }
                    case OpClass: r[o[1]] = Value::object(classDefinition(nodes[o[2]], env)); pc += 3; break;
                    case OpDefineMethod: {
                        Node* p = nodes[o[2]];
                        Object* obj = r[o[1]].asObject();
                        const PropertyKey key = o[3] != kNone ? rt_.toPropertyKey(r[o[3]]) : PropertyKey(p->str);
                        Object* fn = makeFunction(p->fn, env, obj);
                        if (p->flags & (kGetter | kSetter)) {
                            Property* existing = obj->own(key);
                            Property prop = existing && (existing->flags & kAccessor)
                                                ? *existing
                                                : Property{Value::undefined(), nullptr, nullptr, kEnumerable | kConfigurable | kAccessor};
                            if (p->flags & kGetter) prop.getter = fn;
                            else prop.setter = fn;
                            obj->define(key, prop);
                        } else {
                            obj->define(key, {Value::object(fn)});
                        }
                        pc += 4;
                        break;
                    }
                    case OpArguments: {
                        Object* a = rt_.allocate<Object>(rt_.object_prototype, ObjectClass::Arguments);
                        for (size_t i = 0; i < args.size(); ++i) a->define(PropertyKey(utf16(std::to_string(i))), {args[i]});
                        a->define(u"length", {Value::number(static_cast<double>(args.size())), nullptr, nullptr, kWritable | kConfigurable});
                        a->define(PropertyKey(rt_.sym_iterator), {rt_.get(Value::object(rt_.array_prototype), u"values"), nullptr, nullptr, kWritable | kConfigurable});
                        if (!info->strict && info->simple_params) {
                            a->define(u"callee", {Value::object(f), nullptr, nullptr, kWritable | kConfigurable});
                        } else if (const Property* caller = rt_.function_prototype->own(PropertyKey(u"caller")); caller && caller->getter) {
                            a->define(u"callee", {Value::undefined(), caller->getter, caller->getter, kAccessor});
                        }
                        r[o[1]] = Value::object(a);
                        pc += 2;
                        break;
                    }
                    default: rt_.throwError("InternalError", "bad bytecode");
                }
            }
        } catch (const Thrown& t) {
            // The innermost handler around the instruction that threw, if any.
            const VmCode::Handler* h = nullptr;
            for (const VmCode::Handler& c : code.handlers)
                if (at >= c.start && at < c.end) {
                    h = &c;
                    break;
                }
            if (!h) throw;
            exception = t.value;
            pc = h->target;
            caught = true;
            r[h->reg] = exception;
        }
        (void)caught;
    }
done:
    if (new_target) {
        if (result.isObject()) return result;
        return this_value;
    }
    return result;
}

}  // namespace fbx::xweb::js
