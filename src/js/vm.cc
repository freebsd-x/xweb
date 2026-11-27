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
    OpNeedArguments, // d                   : made there, unless they have been (put off)
    OpArgLen,        // d a                 : arguments.length, not making them for it
    OpArgElem,       // d a k               : arguments[k]
    OpRestArgs,      // d from              : the arguments from `from` on, as an array
    OpJumpIfNotUndef,// s t
    OpCheckObjectCoercible, // s           : destructuring null or undefined is a TypeError
    OpIterGet,       // d s                 : an iterator record for s (as for-of and [a, b] = s)
    OpIterValue,     // d it                : its next value, or undefined when it's done
    OpIterNext,      // it d t              : its next value, or to t when it's done
    OpIterRest,      // d it                : what's left of it, as an array
    OpIterClose,     // it                  : closed, if it isn't done
    OpIterCloseFin,  // it kind             : at a finally: closed, its errors dropped if one is on its way
    OpObjRest,       // d s used            : s's own enumerable properties but the keys in the array `used`
    OpPushEnv,       // i                   : a block's scope (block_envs[i]'s names), for what closures see of it
    OpPopEnv,        //                     : back out of it
    OpCopyEnv,       //                     : a for (let ...) loop's next turn: a copy of this turn's
    OpSuperGet,      // d n k               : super.name (k none) or super[k], with this as receiver
    OpSuperSet,      // n k v
    OpSuperCall,     // d start argc arr    : super(...) in a derived constructor (arr: a spread's array, or none)
    OpNewTarget,     // d
    // d a b op: as OpBin, two numbers worked out here (anything else as OpBin does).
    OpBAdd,
    OpBSub,
    OpBMul,
    OpBBitAnd,
    OpBBitOr,
    OpBBitXor,
    OpBShl,
    OpBShr,
    OpBUShr,
    OpBLt,
    OpBGt,
    OpBLe,
    OpBGe,
    OpBStrictEq,
    OpBStrictNe,
    // d a imm op: the same with a whole number for b (imm its int32 bits): `x >>> 7`, `i < 16`,
    // `h | 0` without loading the number into a register first.
    OpIAdd,
    OpISub,
    OpIBitAnd,
    OpIBitOr,
    OpIBitXor,
    OpIShl,
    OpIShr,
    OpIUShr,
    OpILt,
    OpIGt,
    OpILe,
    OpIGe,
    OpIStrictEq,
    OpIStrictNe,
    // a b t (b a register) or a imm t (imm a whole number's int32 bits): to t when a OP b
    // is true (…T) or false (…F). A condition tested where it's worked out: no boolean
    // made to be tested by the next instruction.
    OpJRStrictEqT,
    OpJRStrictEqF,
    OpJRLtT,
    OpJRLtF,
    OpJRLeT,
    OpJRLeF,
    OpJRGtT,
    OpJRGtF,
    OpJRGeT,
    OpJRGeF,
    OpJIStrictEqT,
    OpJIStrictEqF,
    OpJILtT,
    OpJILtF,
    OpJILeT,
    OpJILeF,
    OpJIGtT,
    OpJIGtF,
    OpJIGeT,
    OpJIGeF,
    OpJumpIfNotNullish, // s t
    OpJumpIfNull,    // s t                 : s === null
    OpJumpIfNotNull, // s t
    OpReturnUndef,   //                     : return undefined
    OpYield,         // d s                 : a generator's yield: d is what next() sends
    OpYieldStar,     // d s                 : yield* s
    OpAwait,         // d s
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
        uint32_t env_depth = 0;   // how many block scopes deep its target is
        // A finally's: where it starts and its kind register, for a generator's return()
        // at a yield inside (it runs the finally, then returns).
        bool is_finally = false;
        uint32_t kind_reg = 0, fin_start = 0;
    };
    std::vector<Handler> handlers;   // innermost first
    // A member access's last find, by its node: the shape of the object it was read from
    // and where in it, or how far up the chain and those objects' shapes. Checked against
    // the live objects each time (never read through a pointer kept here).
    struct PropCache {
        const Shape* shape = nullptr;      // the receiver's
        const Shape* up1 = nullptr;        // its prototype's (when found there or further)
        const Shape* up2 = nullptr;        // that one's prototype's
        uint32_t depth = 0;                // 0: own, 1, 2
        uint32_t slot = 0;
        // A set that added the key: the receiver's shape after (shape before), its chain's.
        const Shape* added = nullptr;
        const Shape* chain[3] = {};
    };
    std::vector<PropCache> get_cache, set_cache;
    // As a constructor: how many properties its objects had by its end (room made for them).
    uint32_t ctor_props = 0;
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
    std::vector<std::vector<EnvName>> block_envs;
    // Per node: where a name found on the global object was (its place there, and the
    // scope generation it was found in).
    struct GlobalCache {
        uint64_t gen = 0;
        uint32_t pos = 0;
    };
    std::vector<GlobalCache> globals;
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
        if (f.is_generator && !f.simple_params) fail("generator with parameter patterns");   // (they're bound at the call)
        if (f.own_arguments) fail("f.arguments");
        if (f.params.size() > 255) fail("too many parameters");
        // What nested functions and classes may see of this one's variables: every name
        // used anywhere inside them (more than they need, never less).
        for (Node* p : f.params) survey(p, false);
        if (f.expression_body) survey(f.body, false);
        else
            for (Node* k : f.body->kids) survey(k, false);
        if (direct_eval_) fail("direct eval");
        code_->needs_env = makes_functions_;
        scopes_.emplace_back();
        scope_env_.push_back(-1);
        if (!f.simple_params) {
            params();
        } else {
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
            emit(OpInitCap, {capArg(l), static_cast<uint32_t>(i)});
        }
        }
        // arguments (not an arrow's: that's its enclosing function's, found by name).
        if (f.uses_arguments && !f.arrow && !scopes_.back().count(u"arguments")) {
            Local& l = declareVar(u"arguments");
            if (l.slot >= 0) {
                const uint32_t t = temp();
                emit(OpArguments, {t});
                emit(OpInitCap, {capArg(l), t});
                next_ = floor_;
            } else if (!f.is_generator && !f.is_async) {
                // Made when something but `arguments.length` and `arguments[i]` is asked of
                // them: mostly a loop copies them (what compilers make of `...args`).
                emit(OpEmpty, {l.reg});
                later_arguments_ = &l;
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
            if (l.slot >= 0) emit(OpInitCap, {capArg(l), t});
            else emit(OpMov, {l.reg, t});
            next_ = floor_;
        }
        floor_ = next_;
        max_ = std::max(max_, next_);
        if (f.expression_body) {
            site(f.body);   // (an arrow's errors are its own, not its caller's line)
            const uint32_t r = value(f.body);
            emit(OpReturn, {r});
        } else {
            enterLexical(f.body->lexical, f.body->kids, true);
            for (Node* s : f.body->kids) {
                if (s->kind == N::FunctionDecl) continue;   // made at the start
                stmt(s);
            }
            if (code_->nparams == 0 && next_ == 0) next_ = 1;
            emit(OpReturnUndef, {});
        }
        // A finally's jumps out, each on to the next finally or to where it was going.
        for (const Stub& s : stubs_) {
            code_->stubs.push_back(here());
            for (uint32_t i = 0; i < s.pops; ++i) emit(OpPopEnv, {});
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
        code_->globals.resize(code_->nodes.size());
        code_->nregs = std::max(max_, next_);
        if (code_->nregs == 0) code_->nregs = 1;
        return code_;
    }

private:
    struct Local {
        uint32_t reg;
        bool lexical;   // let, const: has a dead zone
        bool is_const;
        int slot = -1;  // kept in a scope (a nested function may see it): its place there
        uint32_t env_depth = 0;   // which scope: the function's (0), or a block's that deep
    };
    // A scope variable's operand: how many scopes out from where it's used, and its place.
    uint32_t capArg(const Local& l) const {
        const uint32_t hops = env_depth_ - l.env_depth;
        if (hops > 255 || l.slot > 0xFFFFFF) throw Unsupported{"scopes too deep"};
        return (hops << 24) | static_cast<uint32_t>(l.slot);
    }
    // Leaving block scopes down to depth `to`, on a jump away (the code after it is still
    // at this depth).
    void popEnvsKeep(uint32_t to) {
        for (uint32_t d = env_depth_; d > to; --d) emit(OpPopEnv, {});
    }

    // Parameters with patterns, defaults or a rest: argument i in register i, then each
    // bound in turn to its names, which are in their dead zone until then (`(a = b, b)`
    // throws).
    void params() {
        FunctionInfo& f = *info_;
        size_t positional = f.params.size();
        if (!f.params.empty() && f.params.back()->kind == N::Rest) --positional;
        next_ = static_cast<uint32_t>(positional);
        code_->nparams = next_;
        max_ = std::max(max_, next_);
        floor_ = next_;
        std::vector<std::u16string> names;
        for (Node* p : f.params) patternNames(p, names);
        Scope& sc = scopes_.back();
        for (const auto& name : names) {
            if (sc.count(name)) continue;
            Local l{next_++, true, false};
            if (captured(name)) {
                l.slot = envSlot(name, true, false);
            } else {
                emit(OpEmpty, {l.reg});
            }
            sc[name] = l;
        }
        floor_ = next_;
        max_ = std::max(max_, next_);
        for (size_t i = 0; i < f.params.size(); ++i) {
            Node* p = f.params[i];
            TempMark m(*this);
            if (p->kind == N::Rest) {
                const uint32_t x = temp();
                emit(OpRestArgs, {x, static_cast<uint32_t>(i)});
                bindPattern(p->a, x, Bind::Let);
                break;
            }
            bindPattern(p, static_cast<uint32_t>(i), Bind::Let);
        }
        // Bound: no dead zone from here on (a var of the same name shares it).
        for (auto& [name, l] : sc) l.lexical = false;
    }

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
        uint32_t pops = 0;   // block scopes to leave on the way
        Finally* next_finally = nullptr;
        uint32_t next_kind_reg = 0;
        int next_kind = 0;
        Label* target_label = nullptr;
    };
    struct Finally {
        uint32_t kind_reg, val_reg;
        uint32_t env_depth = 0;
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
        size_t cont_depth = SIZE_MAX;   // a continue's, when it's inside a finally of the loop's own (for-of)
        uint32_t env_depth = 0;          // block scopes at its break target
        uint32_t cont_env_depth = 0;     // and at its continue target
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

    const Local* findLocalAsIs(const std::u16string& name) const {
        for (size_t i = scopes_.size(); i-- > 0;)
            if (auto it = scopes_[i].find(name); it != scopes_[i].end()) return &it->second;
        return nullptr;
    }
    const Local* findLocal(const std::u16string& name) {
        const Local* l = findLocalAsIs(name);
        if (l && l == later_arguments_) emit(OpNeedArguments, {l->reg});
        return l;
    }
    // Whether an expression is `arguments`, put off.
    const Local* laterArguments(const Node* e) const {
        if (!later_arguments_ || e->kind != N::Identifier || e->str != u"arguments") return nullptr;
        const Local* l = findLocalAsIs(e->str);
        return l == later_arguments_ ? l : nullptr;
    }
    const Local* later_arguments_ = nullptr;

    // A block's let, const (and their dead zones): registers of their own, below any temp.
    // Returns the block scope it made (an index in block_envs; it's entered now), or -1.
    int enterLexical(const std::vector<std::u16string>* names, const std::vector<Node*>& stmts, bool top = false) {
        scopes_.emplace_back();
        scope_env_.push_back(-1);
        if (!names) return -1;
        std::unordered_set<std::u16string> consts;
        for (Node* s : stmts)
            if (s && s->kind == N::VarDecl && (s->flags & kConst))
                for (Node* d : s->kids) {
                    std::vector<std::u16string> names;
                    patternNames(d->a, names);
                    consts.insert(names.begin(), names.end());
                }
        for (const auto& name : *names) {
            const uint32_t r = next_++;
            max_ = std::max(max_, next_);
            floor_ = next_;
            Local l{r, true, consts.count(name) != 0};
            if (captured(name)) {
                if (top) {
                    l.slot = envSlot(name, true, l.is_const);   // in its dead zone from the start
                } else {
                    // A block's, seen by a closure: a scope of the block's own, made each
                    // time it's entered.
                    if (scope_env_.back() < 0) {
                        code_->needs_env = true;
                        code_->block_envs.emplace_back();
                        scope_env_.back() = static_cast<int>(code_->block_envs.size() - 1);
                    }
                    auto& list = code_->block_envs[static_cast<size_t>(scope_env_.back())];
                    l.slot = static_cast<int>(list.size());
                    l.env_depth = env_depth_ + 1;
                    list.push_back({name, true, l.is_const});
                }
            } else {
                emit(OpEmpty, {r});
            }
            scopes_.back()[name] = l;
        }
        if (scope_env_.back() >= 0) {
            emit(OpPushEnv, {static_cast<uint32_t>(scope_env_.back())});
            ++env_depth_;
        }
        return scope_env_.back();
    }
    // `popped`: its block scope was left already (a for-in/of's, entered each turn).
    void leaveLexical(bool popped = false) {
        if (scope_env_.back() >= 0 && !popped) {
            emit(OpPopEnv, {});
            --env_depth_;
        }
        scopes_.pop_back();
        scope_env_.pop_back();
    }
    // A for-in/of head's const names.
    void markConst(Node* n) {
        if (!(n->flags & kConst)) return;
        std::vector<std::u16string> names;
        patternNames(n->a, names);
        for (const auto& nm : names)
            if (auto it = scopes_.back().find(nm); it != scopes_.back().end()) it->second.is_const = true;
    }

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
                if (!n->a && finallies_.empty()) {
                    emit(OpReturnUndef, {});
                    return;
                }
                const uint32_t r = n->a ? value(n->a) : constUndefined();
                doReturn(r);
                return;
            }
            case N::If: {
                site(n);
                std::vector<uint32_t> to_else;
                condJump(n->a, false, to_else);
                stmt(n->b);
                if (n->c) {
                    const uint32_t to_end = jumpFwd(OpJump);
                    for (uint32_t at : to_else) patchHere(at);
                    stmt(n->c);
                    patchHere(to_end);
                } else {
                    for (uint32_t at : to_else) patchHere(at);
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
            case N::While: case N::DoWhile: case N::For: case N::ForIn: case N::ForOf: case N::Switch: loopLike(n, {}); return;
            case N::Labeled: {
                std::vector<std::u16string> labels{n->str};
                Node* body = n->a;
                while (body->kind == N::Labeled) {
                    labels.push_back(body->str);
                    body = body->a;
                }
                if (body->kind == N::While || body->kind == N::DoWhile || body->kind == N::For || body->kind == N::ForIn || body->kind == N::ForOf ||
                    body->kind == N::Switch) {
                    loopLike(body, labels);
                    return;
                }
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
        if (l.slot >= 0) emit(OpInitCap, {capArg(l), v});
        else if (l.reg != v) emit(OpMov, {l.reg, v});
    }

    void declare(Node* n) {
        const bool lexical = (n->flags & (kLet | kConst)) != 0;
        for (Node* d : n->kids) {
            if (!d->a) fail("declaration");
            if (d->a->kind != N::Identifier) {
                if (!d->b) fail("pattern without a value");
                TempMark m(*this);
                const uint32_t v = valueTemp(d->b);
                bindPattern(d->a, v, lexical ? Bind::Let : Bind::Var);
                continue;
            }
            const Local* l = findLocal(d->a->str);
            if (!l) fail("declared name not found");
            TempMark m(*this);
            if (l->slot >= 0) {
                if (!d->b && !lexical) continue;
                const uint32_t t = temp();
                if (d->b) expr(d->b, t);
                else emit(OpUndef, {t});
                if (lexical) emit(OpInitCap, {capArg(*l), t});
                else emit(OpStoreCap, {capArg(*l), t, node(d->a)});
                continue;
            }
            if (d->b) {
                expr(d->b, l->reg);
            } else if (lexical) {
                emit(OpUndef, {l->reg});
            }
            // Past its declaration, in the same block, it's surely made: read straight from its
            // register, no dead zone to check (as a var). Not in a switch's cases, which can
            // be jumped into past it.
            if (lexical && !scopes_.empty()) {
                const size_t at = scopes_.size() - 1;
                if (auto it = scopes_[at].find(d->a->str); it != scopes_[at].end() && &it->second == l &&
                    std::find(switch_scopes_.begin(), switch_scopes_.end(), at) == switch_scopes_.end())
                    it->second.lexical = false;
            }
        }
    }
    std::vector<size_t> switch_scopes_;   // the scopes of the switches being compiled

    // Loops and switches: their breaks, continues, and labels.
    void loopLike(Node* n, std::vector<std::u16string> labels) {
        Target t;
        t.labels = std::move(labels);
        t.loop = n->kind != N::Switch;
        t.brk = newLabel();
        t.cont = t.loop ? newLabel() : nullptr;
        t.finally_depth = finallies_.size();
        t.env_depth = t.cont_env_depth = env_depth_;
        switch (n->kind) {
            case N::While: {
                site(n);
                Label* top = newLabel();
                place(top);
                {
                    std::vector<uint32_t> out;
                    condJump(n->a, false, out);
                    for (uint32_t at : out) patchTo(at, t.brk);
                }
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
                {
                    std::vector<uint32_t> out;
                    condJump(n->b, true, out);
                    for (uint32_t at : out) patchTo(at, top);
                }
                place(t.brk);
                return;
            }
            case N::For: {
                site(n);
                const bool own = enterLexical(n->lexical, {n->a}) >= 0;
                t.env_depth = t.cont_env_depth = env_depth_;
                if (n->a) {
                    TempMark m(*this);
                    if (n->a->kind == N::VarDecl) declare(n->a);
                    else (void)value(n->a, true);
                }
                // Each turn its own copy of the loop's scope (closures made in one turn
                // keep that turn's values).
                if (own) emit(OpCopyEnv, {});
                Label* top = newLabel();
                place(top);
                if (n->b) {
                    std::vector<uint32_t> out;
                    condJump(n->b, false, out);
                    for (uint32_t at : out) patchTo(at, t.brk);
                }
                targets_.push_back(t);
                stmt(n->d);
                targets_.pop_back();
                place(t.cont);
                emit(OpLoop, {});
                if (own) emit(OpCopyEnv, {});
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
                if (n->a->kind != N::Identifier && decl == 0 && n->a->kind != N::Member && n->a->kind != N::ArrayPattern &&
                    n->a->kind != N::ObjectPattern)
                    fail("for-in target");
                // Its own scope for a let or const name, in its dead zone while the
                // subject is worked out.
                const int own = enterLexical(n->lexical, {});
                markConst(n);
                const uint32_t it = temp();
                floor_ = std::max(floor_, next_);   // (lives for the whole loop)
                {
                    TempMark m(*this);
                    emit(OpForInPrep, {it, value(n->b)});
                }
                // (Its scope, if it has one: the dead zone's while the subject was worked
                // out; then a new one each turn.)
                if (own >= 0) {
                    emit(OpPopEnv, {});
                    --env_depth_;
                }
                Label* top = newLabel();
                place(top);
                {
                    TempMark m(*this);
                    const uint32_t key = temp();
                    patchTo(jumpFwd(OpForInNext, {it, key}), t.brk);
                    if (own >= 0) {
                        emit(OpPushEnv, {static_cast<uint32_t>(own)});
                        ++env_depth_;
                    }
                    t.cont_env_depth = env_depth_;
                    bindPattern(n->a, key, (decl & (kLet | kConst)) ? Bind::Let : decl ? Bind::Var : Bind::Assign);
                }
                targets_.push_back(t);
                stmt(n->c);
                targets_.pop_back();
                place(t.cont);
                if (own >= 0) {
                    emit(OpPopEnv, {});
                    --env_depth_;
                }
                emit(OpLoop, {});
                patchTo(jumpFwd(OpJump), top);
                place(t.brk);
                leaveLexical(true);
                return;
            }
            case N::ForOf: {
                site(n);
                if (n->flags & kAwait) fail("for await");
                const uint32_t decl = n->flags & (kVar | kLet | kConst);
                const int own = enterLexical(n->lexical, {});
                markConst(n);
                const uint32_t it = temp();
                floor_ = std::max(floor_, next_);
                {
                    TempMark m(*this);
                    emit(OpIterGet, {it, value(n->b)});
                }
                if (own >= 0) {
                    emit(OpPopEnv, {});
                    --env_depth_;
                }
                // The loop inside a finally that closes the iterator when it's left early
                // (a break, a return, a throw); a break's target is past the finally.
                t.finally_depth = finallies_.size();
                t.cont_depth = t.finally_depth + 1;
                Finally* fin = beginFinally();
                const uint32_t start = here();
                Label* exhausted = newLabel();
                Label* top = newLabel();
                place(top);
                {
                    TempMark m(*this);
                    const uint32_t v = temp();
                    patchTo(jumpFwd(OpIterNext, {it, v}), exhausted);
                    if (own >= 0) {
                        emit(OpPushEnv, {static_cast<uint32_t>(own)});
                        ++env_depth_;
                    }
                    t.cont_env_depth = env_depth_;
                    bindPattern(n->a, v, (decl & (kLet | kConst)) ? Bind::Let : decl ? Bind::Var : Bind::Assign);
                }
                targets_.push_back(t);
                stmt(n->c);
                targets_.pop_back();
                place(t.cont);
                if (own >= 0) {
                    emit(OpPopEnv, {});
                    --env_depth_;
                }
                emit(OpLoop, {});
                patchTo(jumpFwd(OpJump), top);
                place(exhausted);
                const uint32_t end = here();
                finishFinally(fin, start, end, [&] { emit(OpIterCloseFin, {it, fin->kind_reg}); });
                place(t.brk);
                leaveLexical(true);
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
                switch_scopes_.push_back(scopes_.size() - 1);
                struct Popped {
                    std::vector<size_t>& v;
                    ~Popped() { v.pop_back(); }
                } popped{switch_scopes_};
                t.env_depth = env_depth_;   // (its break target is inside its scope, left after)
                std::vector<Label*> bodies;
                for (size_t i = 0; i < n->kids.size(); ++i) bodies.push_back(newLabel());
                for (size_t i = 0; i < n->kids.size(); ++i) {
                    Node* c = n->kids[i];
                    if (!c->a) continue;
                    TempMark m(*this);
                    if (smallInt(c->a)) {
                        patchTo(jumpFwd(OpJIStrictEqT, {d, static_cast<uint32_t>(static_cast<int32_t>(c->a->number))}), bodies[i]);
                    } else {
                        const uint32_t v = value(c->a);
                        patchTo(jumpFwd(OpJRStrictEqT, {d, v}), bodies[i]);
                    }
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
                place(t.brk);
                leaveLexical();
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
        const size_t depth = !is_break && t->cont_depth != SIZE_MAX ? t->cont_depth : t->finally_depth;
        const uint32_t dest_env = is_break ? t->env_depth : t->cont_env_depth;
        if (finallies_.size() == depth) {
            popEnvsKeep(dest_env);
            patchTo(jumpFwd(OpJump), dest);
            return;
        }
        // The chain from the outermost crossed finally inward: each stub hands on to the next.
        int next_kind = -1;
        Finally* next_f = nullptr;
        for (size_t i = depth; i < finallies_.size(); ++i) {
            Finally* f = finallies_[i];
            Stub s;
            if (next_f) {
                s.next_finally = next_f;
                s.next_kind_reg = next_f->kind_reg;
                s.next_kind = next_kind;
                s.pops = f->env_depth - next_f->env_depth;
            } else {
                s.target_label = dest;
                s.pops = f->env_depth - dest_env;
            }
            stubs_.push_back(s);
            next_kind = kFinJumps + static_cast<int>(stubs_.size() - 1);
            next_f = f;
        }
        // next_f is now the innermost finally, next_kind its kind.
        popEnvsKeep(next_f->env_depth);
        emit(OpInt, {next_f->kind_reg, static_cast<uint32_t>(next_kind)});
        next_f->starts.push_back(jumpFwd(OpJump));
    }

    void doReturn(uint32_t r) {
        if (finallies_.empty()) {
            emit(OpReturn, {r});
            return;
        }
        Finally* f = finallies_.back();
        popEnvsKeep(f->env_depth);
        emit(OpMov, {f->val_reg, r});
        emit(OpInt, {f->kind_reg, kFinReturn});
        f->starts.push_back(jumpFwd(OpJump));
    }

    Finally* beginFinally() {
        Finally* fin = new Finally;
        owned_finallies_.emplace_back(fin);
        fin->kind_reg = temp();
        fin->val_reg = temp();
        fin->env_depth = env_depth_;
        floor_ = std::max(floor_, next_);
        finallies_.push_back(fin);
        return fin;
    }
    // The finally's code after the region it guards [start, end): reached normally (the
    // code falls through to here), by a throw in the region, or by a jump to its start.
    template <typename Body>
    void finishFinally(Finally* fin, uint32_t start, uint32_t end, Body&& body) {
        finallies_.pop_back();
        // Leaving normally: through the finally with nothing pending.
        emit(OpInt, {fin->kind_reg, kFinNormal});
        const uint32_t to_fin = jumpFwd(OpJump);
        // A throw in the region: kept, through the finally, thrown again.
        const uint32_t throw_entry = here();
        emit(OpInt, {fin->kind_reg, kFinThrow});
        patchHere(to_fin);
        fin->start = here();
        {
            VmCode::Handler h{start, end, throw_entry, fin->val_reg, fin->env_depth};
            h.is_finally = true;
            h.kind_reg = fin->kind_reg;
            h.fin_start = fin->start;
            code_->handlers.push_back(h);
        }
        for (uint32_t s : fin->starts) code_->ops[s] = fin->start;
        fin->starts.clear();
        body();
        // Then on: as it was leaving. A return or jump out goes on through any outer
        // finally (the stubs for jumps were made with that one in mind).
        Finally* outer = finallies_.empty() ? nullptr : finallies_.back();
        if (outer) {
            emit(OpEndFinally, {fin->kind_reg, fin->val_reg, outer->kind_reg, outer->val_reg});
            outer->starts.push_back(here());
            code_->ops.push_back(kNone);
            code_->ops.push_back(fin->env_depth - outer->env_depth);
        } else {
            emit(OpEndFinally, {fin->kind_reg, fin->val_reg, kNone, kNone, kNone, 0});
        }
    }

    void tryStatement(Node* n) {
        Finally* fin = n->d ? beginFinally() : nullptr;
        const uint32_t try_start = here();
        stmt(n->a);
        uint32_t to_after = jumpFwd(OpJump);   // past the catch
        const uint32_t try_end = here();
        uint32_t catch_end = try_end;
        if (n->c) {
            const uint32_t exc = temp();
            floor_ = std::max(floor_, next_);
            const uint32_t handler = here();
            if (n->b && n->b->kind == N::Identifier && !captured(n->b->str)) {
                scopes_.emplace_back();
                scope_env_.push_back(-1);
                scopes_.back()[n->b->str] = Local{exc, false, false};
            } else if (n->b) {
                // A pattern, or a name a closure sees: its names in a scope of their own,
                // bound from the exception.
                std::vector<std::u16string> names;
                patternNames(n->b, names);
                enterLexical(&names, {});
                bindPattern(n->b, exc, Bind::Let);
            }
            stmt(n->c);
            if (n->b) leaveLexical();
            code_->handlers.push_back({try_start, try_end, handler, exc, env_depth_});
            catch_end = here();
        }
        patchHere(to_after);
        if (fin) finishFinally(fin, try_start, catch_end, [&] { stmt(n->d); });
    }

    // ── Patterns ──

    enum class Bind { Let, Var, Assign };   // a let/const/param declaration, a var, an assignment

    static void patternNames(Node* p, std::vector<std::u16string>& out) {
        if (!p) return;
        switch (p->kind) {
            case N::Identifier: out.push_back(p->str); return;
            case N::AssignPattern: case N::Rest: patternNames(p->a, out); return;
            case N::ArrayPattern: for (Node* k : p->kids) patternNames(k, out); return;
            case N::ObjectPattern:
                for (Node* k : p->kids) patternNames(k->kind == N::PatternProperty ? k->b : k->a, out);
                return;
            default: return;
        }
    }

    // A value (in register v) into a pattern's names, as a declaration or an assignment.
    void bindPattern(Node* p, uint32_t v, Bind mode) {
        if (++depth_ > 400) fail("nested too deeply");
        struct Out {
            int& d;
            ~Out() { --d; }
        } out{depth_};
        TempMark m(*this);
        switch (p->kind) {
            case N::Identifier: {
                if (mode == Bind::Let) {
                    const Local* l = findLocal(p->str);
                    if (!l) fail("pattern name");
                    initLocal(*l, v);
                } else {
                    storeName(p, v);
                }
                return;
            }
            case N::Member: {
                if (mode != Bind::Assign) fail("member in a declaration pattern");
                assignTo(p, v);
                return;
            }
            case N::AssignPattern: {
                const uint32_t x = temp();
                emit(OpMov, {x, v});
                const uint32_t skip = jumpFwd(OpJumpIfNotUndef, {x});
                expr(p->b, x);
                patchHere(skip);
                bindPattern(p->a, x, mode);
                return;
            }
            case N::ArrayPattern: {
                const uint32_t it = temp();
                floor_ = std::max(floor_, next_);   // (lives through the pattern)
                emit(OpIterGet, {it, v});
                // Read inside a finally that closes it however the reading ends: normally,
                // by a throw (its own errors dropped), or by a generator's return() at a
                // yield in a default or a target.
                Finally* fin = beginFinally();
                const uint32_t start = here();
                for (Node* k : p->kids) {
                    TempMark e(*this);
                    if (k && k->kind == N::Rest) {
                        Node* target = k->a;
                        uint32_t base = kNone, key = kNone;
                        if (mode == Bind::Assign && target->kind == N::Member) memberRef(target, base, key);
                        const uint32_t x = temp();
                        emit(OpIterRest, {x, it});
                        if (base != kNone) setMember(target, base, key, x);
                        else bindPattern(target, x, mode);
                        break;
                    }
                    Node* target = k;
                    Node* dflt = nullptr;
                    if (k && k->kind == N::AssignPattern) {
                        target = k->a;
                        dflt = k->b;
                    }
                    // An assignment's member target: its reference before the value (as the
                    // tree-walker orders it).
                    uint32_t base = kNone, key = kNone;
                    if (mode == Bind::Assign && target && target->kind == N::Member) memberRef(target, base, key);
                    const uint32_t x = temp();
                    emit(OpIterValue, {x, it});
                    if (!target) continue;
                    if (dflt) {
                        const uint32_t skip = jumpFwd(OpJumpIfNotUndef, {x});
                        expr(dflt, x);
                        patchHere(skip);
                    }
                    if (base != kNone) setMember(target, base, key, x);
                    else bindPattern(target, x, mode);
                }
                const uint32_t end = here();
                finishFinally(fin, start, end, [&] { emit(OpIterCloseFin, {it, fin->kind_reg}); });
                return;
            }
            case N::ObjectPattern: {
                emit(OpCheckObjectCoercible, {v});
                uint32_t used = kNone;
                for (Node* k : p->kids)
                    if (k->kind == N::Rest) {
                        used = temp();
                        emit(OpArrayNew, {used});
                    }
                for (Node* k : p->kids) {
                    TempMark e(*this);
                    if (k->kind == N::Rest) {
                        const uint32_t x = temp();
                        emit(OpObjRest, {x, v, used});
                        bindPattern(k->a, x, mode);
                        break;
                    }
                    if (k->kind != N::PatternProperty) fail("object pattern part");
                    const uint32_t x = temp();
                    if (k->flags & kComputed) {
                        const uint32_t key = temp();
                        expr(k->a, key);
                        if (used != kNone) emit(OpArrayPush, {used, key});
                        emit(OpGetElem, {x, v, key});
                    } else {
                        if (used != kNone) {
                            const uint32_t key = temp();
                            emit(OpConst, {key, constant(Value::string(k->str))});
                            emit(OpArrayPush, {used, key});
                        }
                        emit(OpGetNamed, {x, v, node(k)});
                    }
                    bindPattern(k->b, x, mode);
                }
                return;
            }
            default: fail("pattern");
        }
    }
    // A member target's object and key (computed), worked out now.
    void memberRef(Node* t, uint32_t& base, uint32_t& key) {
        if (t->a->kind == N::Super) fail("super");
        base = temp();
        expr(t->a, base);
        if (t->flags & kComputed) {
            key = temp();
            expr(t->b, key);
        }
    }
    void setMember(Node* t, uint32_t base, uint32_t key, uint32_t v) {
        if (key != kNone) emit(OpSetElem, {base, key, v});
        else emit(OpSetNamed, {base, node(t), v});
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
                std::vector<uint32_t> to_else;
                condJump(n->a, false, to_else);
                expr(n->b, dst);
                const uint32_t to_end = jumpFwd(OpJump);
                for (uint32_t at : to_else) patchHere(at);
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
                binWith(dst, l, n->b, n->op);
                return;
            }
            case N::Member: {
                TempMark m(*this);
                if (n->a->kind == N::Super) {
                    if (n->flags & kComputed) emit(OpLoadThis, {temp()});   // (this first: super[super()] throws)
                    const uint32_t k = (n->flags & kComputed) ? value(n->b) : kNone;
                    emit(OpSuperGet, {dst, node(n), k});
                    return;
                }
                if (const Local* args = (n->flags & kOptional) ? nullptr : laterArguments(n->a)) {
                    if (n->flags & kComputed) {
                        const uint32_t k = value(n->b);
                        emit(OpArgElem, {dst, args->reg, k});
                        return;
                    }
                    if (n->str == u"length") {
                        emit(OpArgLen, {dst, args->reg});
                        return;
                    }
                }
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
            case N::NewTarget: emit(OpNewTarget, {dst}); return;
            case N::Super: fail("super");
            case N::Yield: {
                TempMark m(*this);
                const uint32_t v = n->a ? value(n->a) : constUndefined();
                emit((n->flags & kSpreadProp) ? OpYieldStar : OpYield, {dst, v});
                return;
            }
            case N::Await: {
                TempMark m(*this);
                emit(OpAwait, {dst, value(n->a)});
                return;
            }
            case N::ImportCall: case N::ImportMeta: fail("import");
            default: fail("an expression the VM doesn't know");
        }
    }

    // A binary operator: its own instruction for the usual ones (two numbers worked out
    // in place), OpBin for the rest.
    // ── Conditions ──
    // Code that jumps when `c` is `when` (truthy or falsy) and goes on otherwise: the
    // jumps' target operands go in `out`. A comparison jumps as it compares; ! and
    // && / || become jumps, not booleans.
    static bool smallInt(const Node* b) {
        return b->kind == N::Number && b->number == std::trunc(b->number) && std::abs(b->number) < 2147483648.0 &&
               !(b->number == 0 && std::signbit(b->number));
    }
    void condJump(Node* c, bool when, std::vector<uint32_t>& out) {
        if (c->kind == N::Unary && c->op == Op::Not) {
            condJump(c->a, !when, out);
            return;
        }
        if (c->kind == N::Logical && (c->op == Op::And || c->op == Op::Or)) {
            // a && b is falsy if either is; a || b truthy if either is.
            const bool either = (c->op == Op::And) != when;
            if (either) {
                condJump(c->a, when, out);
                condJump(c->b, when, out);
            } else {
                std::vector<uint32_t> past;
                condJump(c->a, !when, past);
                condJump(c->b, when, out);
                for (uint32_t at : past) patchHere(at);
            }
            return;
        }
        TempMark m(*this);
        if (c->kind == N::Binary && (c->op == Op::Eq || c->op == Op::Ne) && c->b->kind == N::Null) {
            const uint32_t a = value(c->a);
            const bool nullish = when == (c->op == Op::Eq);
            out.push_back(jumpFwd(nullish ? OpJumpIfNullish : OpJumpIfNotNullish, {a}));
            return;
        }
        if (c->kind == N::Binary && (c->op == Op::StrictEq || c->op == Op::StrictNe) && c->b->kind == N::Null) {
            const uint32_t a = value(c->a);
            const bool is_null = when == (c->op == Op::StrictEq);
            out.push_back(jumpFwd(is_null ? OpJumpIfNull : OpJumpIfNotNull, {a}));
            return;
        }
        if (c->kind == N::Binary) {
            int which = -1;
            bool sense = when;
            switch (c->op) {
                case Op::StrictEq: which = 0; break;
                case Op::StrictNe: which = 0; sense = !when; break;
                case Op::Lt: which = 1; break;
                case Op::Le: which = 2; break;
                case Op::Gt: which = 3; break;
                case Op::Ge: which = 4; break;
                default: break;
            }
            if (which >= 0) {
                const uint32_t a = value(c->a, false, {c->b});
                const bool imm = smallInt(c->b);
                const uint32_t b = imm ? static_cast<uint32_t>(static_cast<int32_t>(c->b->number)) : value(c->b);
                const uint32_t first = imm ? OpJIStrictEqT : OpJRStrictEqT;
                out.push_back(jumpFwd(static_cast<Opc>(first + which * 2 + (sense ? 0 : 1)), {a, b}));
                return;
            }
        }
        const uint32_t v = value(c);
        out.push_back(jumpFwd(when ? OpJumpIfTrue : OpJumpIfFalse, {v}));
    }

    // a OP b, b an expression: a small whole number goes in the instruction.
    void binWith(uint32_t d, uint32_t a, Node* b, Op op) {
        if (b->kind == N::Number && b->number == std::trunc(b->number) && std::abs(b->number) < 2147483648.0 &&
            !(b->number == 0 && std::signbit(b->number))) {
            Opc c = OpCount;
            switch (op) {
                case Op::Add: c = OpIAdd; break;
                case Op::Sub: c = OpISub; break;
                case Op::BitAnd: c = OpIBitAnd; break;
                case Op::BitOr: c = OpIBitOr; break;
                case Op::BitXor: c = OpIBitXor; break;
                case Op::Shl: c = OpIShl; break;
                case Op::Shr: c = OpIShr; break;
                case Op::UShr: c = OpIUShr; break;
                case Op::Lt: c = OpILt; break;
                case Op::Gt: c = OpIGt; break;
                case Op::Le: c = OpILe; break;
                case Op::Ge: c = OpIGe; break;
                case Op::StrictEq: c = OpIStrictEq; break;
                case Op::StrictNe: c = OpIStrictNe; break;
                default: break;
            }
            if (c != OpCount) {
                emit(c, {d, a, static_cast<uint32_t>(static_cast<int32_t>(b->number)), static_cast<uint32_t>(op)});
                return;
            }
        }
        const uint32_t r = value(b);
        emitBin(d, a, r, op);
    }

    void emitBin(uint32_t d, uint32_t a, uint32_t b, Op op) {
        Opc c = OpBin;
        switch (op) {
            case Op::Add: c = OpBAdd; break;
            case Op::Sub: c = OpBSub; break;
            case Op::Mul: c = OpBMul; break;
            case Op::BitAnd: c = OpBBitAnd; break;
            case Op::BitOr: c = OpBBitOr; break;
            case Op::BitXor: c = OpBBitXor; break;
            case Op::Shl: c = OpBShl; break;
            case Op::Shr: c = OpBShr; break;
            case Op::UShr: c = OpBUShr; break;
            case Op::Lt: c = OpBLt; break;
            case Op::Gt: c = OpBGt; break;
            case Op::Le: c = OpBLe; break;
            case Op::Ge: c = OpBGe; break;
            case Op::StrictEq: c = OpBStrictEq; break;
            case Op::StrictNe: c = OpBStrictNe; break;
            default: break;
        }
        emit(c, {d, a, b, static_cast<uint32_t>(op)});
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
            if (l->slot >= 0) emit(OpLoadCap, {dst, capArg(*l), node(n)});
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
            TempMark m(*this);
            if (t->a->kind == N::Super) {
                const uint32_t k = (t->flags & kComputed) ? value(t->b) : kNone;
                emit(OpSuperSet, {node(t), k, v});
                return;
            }
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
                    emit(OpLoadCap, {x, capArg(*l), node(t)});   // (its dead zone first)
                    emit(OpThrowConst, {node(t)});
                    return;
                }
                emit(OpStoreCap, {capArg(*l), v, node(t)});
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
        if (n->op == Op::None && (t->kind == N::ArrayPattern || t->kind == N::ObjectPattern)) {
            TempMark m(*this);
            const uint32_t v = valueTemp(n->b);
            bindPattern(t, v, Bind::Assign);
            emit(OpMov, {dst, v});
            return;
        }
        if (t->kind != N::Identifier && t->kind != N::Member) fail("assignment pattern");
        if (t->kind == N::Member && t->a->kind == N::Super) {
            if (n->op != Op::None) fail("compound assignment to super.x");
            TempMark m(*this);
            if (t->flags & kComputed) emit(OpLoadThis, {temp()});   // (this first: super[super()] = 0 throws)
            const uint32_t k = (t->flags & kComputed) ? value(t->b, false, {n->b}) : kNone;
            const uint32_t kk = k != kNone && assigns(n->b) ? copyOf(k) : k;
            expr(n->b, dst);
            emit(OpSuperSet, {node(t), kk, dst});
            return;
        }
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
                emit(OpLoadCap, {old, capArg(*l), node(t)});
                const uint32_t r = value(n->b);
                emit(OpAppendCap, {dst, capArg(*l), old, r});
                return;
            }
            if (l && !l->lexical && l->slot < 0 && !logical) {
                if (n->op == Op::Add && !assigns(n->b)) {
                    const uint32_t r = value(n->b);
                    emit(OpAddInPlace, {l->reg, r});
                } else {
                    const uint32_t old = copyOf(l->reg);
                    binWith(l->reg, old, n->b, n->op);
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
            binWith(dst, dst, n->b, n->op);
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
            binWith(dst, dst, n->b, n->op);
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
        if (callee->kind == N::Super) {
            TempMark m(*this);
            if (hasSpread(n->kids)) {
                const uint32_t arr = temp();
                emit(OpArrayNew, {arr});
                for (Node* k : n->kids) {
                    TempMark e(*this);
                    if (k->kind == N::Spread) emit(OpArraySpread, {arr, value(k->a)});
                    else emit(OpArrayPush, {arr, value(k)});
                }
                emit(OpSuperCall, {dst, 0, 0, arr});
            } else {
                const uint32_t start = next_;
                for (size_t i = 0; i < n->kids.size(); ++i) (void)temp();
                for (size_t i = 0; i < n->kids.size(); ++i) expr(n->kids[i], start + static_cast<uint32_t>(i));
                emit(OpSuperCall, {dst, start, static_cast<uint32_t>(n->kids.size()), kNone});
            }
            return;
        }
        TempMark m(*this);
        const uint32_t fn = temp();
        const uint32_t self = temp();
        Label* saved_short = short_;
        Label* chain = nullptr;
        if (callee->kind == N::Member || (callee->kind == N::OptionalChain && callee->a->kind == N::Member && !(callee->flags & kParenthesized))) {
            Node* mem = callee->kind == N::Member ? callee : callee->a;
            if (mem->a->kind == N::Super) {
                // super.m(...): found on the home object's prototype, called on this.
                emit(OpLoadThis, {self});
                TempMark k(*this);
                const uint32_t key = (mem->flags & kComputed) ? value(mem->b) : kNone;
                emit(OpSuperGet, {fn, node(mem), key});
            } else {
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
        emit(OpNewObject, {o, static_cast<uint32_t>(n->kids.size())});
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
    std::vector<int> scope_env_;   // each scope's block scope (block_envs), or -1
    uint32_t env_depth_ = 0;
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

namespace {
// ToInt32 for a number this side of ±2^63 (not NaN or infinite): truncated, then its low
// 32 bits, as the tree-walker's binaryOp does; anything else goes the slow way.
inline bool int32Exact(double d) { return d > -9.2e18 && d < 9.2e18; }
inline int32_t toI32(double d) { return static_cast<int32_t>(static_cast<uint32_t>(static_cast<int64_t>(d))); }
bool vmProfiling() {
    static const bool on = std::getenv("XWEB_JS_PROFILE") != nullptr;
    return on;
}
}  // namespace

// A call from compiled code to compiled code, without Runtime::call's way round: no
// dispatch on the kind of function, no roots to register (the arguments are the
// caller's registers, on the stack), its frame on the call stack all the same.
Value Interpreter::callCompiled(Object* f, VmCode& code, const Value& this_value, std::span<const Value> args, Object* new_target) {
    if (rt_.call_depth >= Runtime::kMaxCallDepth) rt_.throwError("RangeError", "too much recursion");
    ++rt_.call_depth;
    struct Down {
        size_t& d;
        ~Down() { --d; }
    } down{rt_.call_depth};
    Interpreter callee(rt_, f->code->program);
    FunctionInfo* info = f->code->info;
    callee.owns_scopes_ = ownsScopes(info);
    callee.owned_base_ = rt_.owned_stack.size();
    struct Frame {
        size_t depth;
        explicit Frame(const std::u16string* name) : depth(t_call_frames.size()) {
            if (depth < 4096) t_call_frames.push_back({throwSite(), name});
        }
        ~Frame() {
            if (t_call_frames.size() > depth) t_call_frames.resize(depth);
        }
    } frame(&info->name);
    rt_.safePoint();
    return callee.runVm(f, code, this_value, args, new_target);
}

namespace {

// A key the shapes alone answer for on any object but a proxy: not an index, not "length",
// nothing a typed array would read as a number.
bool shapeKey(const PropertyKey& k) {
    if (k.isSymbol()) return true;
    const std::u16string& n = k.name;
    if (n.empty() || n == u"length" || n == u"Infinity" || n == u"NaN") return false;
    const char16_t c = n[0];
    return !(c >= u'0' && c <= u'9') && c != u'-';
}

// Where key is, from o up at most two prototypes, by shapes: the cache to keep.
bool fillGet(VmCode::PropCache& c, Object* o, const PropertyKey& key) {
    const size_t h = Object::keyHash(key);
    const Shape* shapes[3] = {};
    Object* cur = o;
    for (uint32_t d = 0; d < 3 && cur; ++d, cur = cur->prototype) {
        if (!cur->shape() || cur->ownLater()) return false;
        shapes[d] = cur->shape();
        if (const ptrdiff_t at = cur->shape()->find(key, h); at >= 0) {
            c.shape = shapes[0];
            c.up1 = shapes[1];
            c.up2 = shapes[2];
            c.depth = d;
            c.slot = static_cast<uint32_t>(at);
            return true;
        }
    }
    return false;
}

// The property a cache says o's key is, if it still is.
inline Property* cachedGet(const VmCode::PropCache& c, Object* o) {
    if (o->shape() != c.shape || !c.shape || o->ownLater()) return nullptr;
    Object* h = o;
    if (c.depth >= 1) {
        h = h->prototype;
        if (!h || h->shape() != c.up1 || h->ownLater()) return nullptr;
        if (c.depth == 2) {
            h = h->prototype;
            if (!h || h->shape() != c.up2 || h->ownLater()) return nullptr;
        }
    }
    return &h->slot(c.slot);
}

// A set that would add the key to o as a cache saw it before: o and its chain the same
// shapes (so nothing on the chain has the key: no setter, nothing read-only).
inline bool cachedAdd(const VmCode::PropCache& c, Object* o) {
    if (!c.added || o->shape() != c.added->parent() || !o->extensible || o->ownLater()) return false;
    // (The chain's shapes, then null where it ends: each one there has a shape.)
    Object* p = o->prototype;
    for (int i = 0; i < 3; ++i, p = p->prototype) {
        if (!p) return c.chain[i] == nullptr;
        if (!p->shape() || p->shape() != c.chain[i] || p->ownLater()) return false;
    }
    return p == nullptr;
}

}  // namespace

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

    // this: worked out the first time it's asked for. A derived class's constructor has
    // none until super() (which an arrow in it may call: then it's in the function's
    // scope, `fenv`, where the arrow sets it).
    Value this_value;
    bool this_ready = false;
    const bool derived_ctor = new_target && info->derived;
    bool this_init = !derived_ctor;
    Env* fenv = nullptr;
    const auto thisOf = [&]() -> Value {
        if (info->arrow) return thisValue(closure);   // (not kept: a super() may come yet)
        if (derived_ctor) {
            if (!(fenv ? fenv->this_initialized : this_init)) rt_.throwError("ReferenceError", "super() must be called before using this");
            return fenv ? fenv->this_value : this_value;
        }
        if (this_ready) return this_value;
        this_ready = true;
        if (strict) this_value = this_in;
        else if (this_in.isNullish() || this_in.isEmpty()) this_value = Value::object(rt_.global());
        else this_value = Value::object(rt_.toObject(this_in));
        return this_value;
    };
    if (new_target && !derived_ctor) {
        thread_local const PropertyKey key{u"prototype"};
        const Property* own = new_target->ownLater() ? nullptr : new_target->own(key);
        const Value proto = own && !(own->flags & kAccessor) ? own->value : rt_.get(Value::object(new_target), key);
        Object* obj = rt_.newObject(proto.isObject() ? proto.asObject() : rt_.object_prototype);
        if (code.ctor_props) obj->reserveProps(code.ctor_props);
        this_value = Value::object(obj);
        this_ready = true;
    }
    // A class's instance fields, on its new object (now, or at super() in a derived one).
    const auto initFields = [&](const Value& obj_value) {
        Object* obj = obj_value.asObject();
        for (Node* field : info->fields) {
            Env* fe = fieldEnv(f, obj_value);
            const PropertyKey key = (field->flags & kComputed) ? rt_.toPropertyKey(eval(field->a, fe, true)) : PropertyKey(field->str);
            const Value v = field->b ? eval(field->b, fe, true) : Value::undefined();
            obj->define(key, {v});
        }
    };
    if (new_target && !derived_ctor && info->is_class_constructor) initFields(this_value);
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
            if (derived_ctor) env->this_initialized = false;
            else env->this_value = thisOf();
            fenv = env;
        }
        env->vars.reserve(code.env_names.size());
        for (const VmCode::EnvName& e : code.env_names) {
            Binding b{Value::undefined(), !e.is_const, !e.lexical};
            env->vars.bind(e.name, VarMap::hashOf(e.name)) = b;
        }
        rt_.env_stack.back() = env;   // (the closure is reachable from it)
    }
    uint32_t edepth = 0;   // block scopes entered (env is that many in from the function's)
    // A scope variable: so many scopes out, at its place there.
    const auto cap = [&](uint32_t arg) -> Binding& {
        Env* e = env;
        for (uint32_t hops = arg >> 24; hops > 0; --hops) e = e->outer;
        return e->vars.entryAt(arg & 0xFFFFFF)->second;
    };
    // A name from outside, as lookup finds it; and if that was a data property of the
    // global object, with nothing in the way, where it was (cachedGlobal reads it from
    // there while no new declaration could have come in between).
    Object* const global = rt_.global();
    const bool plain_global = global->cls == ObjectClass::Ordinary || global->cls == ObjectClass::Function;
    const auto cachedGlobal = [&](uint32_t ni) -> const Value* {
        VmCode::GlobalCache& c = code.globals[ni];
        if (c.gen != rt_.scope_gen || !plain_global || !global->elements.empty()) return nullptr;
        Node* nm = code.nodes[ni];
        const Property* p = global->ownAt(c.pos, nameKey(nm), nameHash(nm));
        return p && !(p->flags & kAccessor) ? &p->value : nullptr;
    };
    const auto findGlobal = [&](uint32_t ni) -> Value {
        Node* nm = code.nodes[ni];
        Object* with = nullptr;
        if (plain_global && global->elements.empty() && !findBinding(closure, nm->str, nullptr, nullptr, &with, nameHash(nm), nm) && !with &&
            globalObject(closure) == global) {
            const ptrdiff_t at = global->ownPos(nameKey(nm), nameHash(nm));
            if (at >= 0)
                if (const Property* p = global->ownAt(static_cast<size_t>(at), nameKey(nm), nameHash(nm)); p && !(p->flags & kAccessor)) {
                    code.globals[ni] = {rt_.scope_gen, static_cast<uint32_t>(at)};
                    return p->value;
                }
        }
        return lookup(closure, nm->str, nameHash(nm), nullptr, nm);
    };
    const auto setEnv = [&](Env* e) {
        env = e;
        rt_.env_stack.back() = e;
    };
    // An iterator record, kept in a register as an internal object: the iterator and its
    // next method and a string's characters in its slots, an array read directly in
    // `target`, the place in `cursor`, done/async/wrapped in iter_kind's bits.
    const auto packIter = [&](Object* o, const IterRec& it) {
        o->slots.resize(3);
        o->slots[0] = it.iterator;
        o->slots[1] = it.next;
        o->slots[2] = it.string;
        o->target = it.array;
        o->cursor = it.index;
        o->iter_kind = static_cast<uint8_t>((it.done ? 1 : 0) | (it.async ? 2 : 0) | (it.sync_wrapped ? 4 : 0));
    };
    const auto unpackIter = [&](const Object* o) {
        IterRec it;
        it.iterator = o->slots[0];
        it.next = o->slots[1];
        it.string = o->slots[2];
        it.array = o->target;
        it.index = o->cursor;
        it.done = o->iter_kind & 1;
        it.async = o->iter_kind & 2;
        it.sync_wrapped = o->iter_kind & 4;
        return it;
    };

    const uint32_t* const ops = code.ops.data();
    Node* const* const nodes = code.nodes.data();
    const Value* const consts = code.consts.data();
    uint32_t pc = 0;
    uint32_t at = 0;   // where the instruction running now starts (for its handler)
    ThrowSite& site = throwSite();   // (this thread's: looked up once)

    const auto nameError = [&](Node* nm) {
        rt_.throwError("ReferenceError", "can't use '" + utf8(nm->str) + "' before it's declared");
    };
    const auto calleeName = [&](Node* call) {
        Node* c = call->a;
        if (c->kind == N::OptionalChain) c = c->a;
        return c->kind == N::Identifier ? utf8(c->str) : c->kind == N::Member && !(c->flags & kComputed) ? utf8(c->str) : std::string("expression");
    };

    const auto makeArguments = [&]() -> Object* {
        static const std::vector<PropertyKey> k_index = [] {
            std::vector<PropertyKey> out;
            for (int i = 0; i < 16; ++i) out.emplace_back(utf16(std::to_string(i)));
            return out;
        }();
        static const PropertyKey k_length(u"length"), k_callee(u"callee"), k_values(u"values"), k_caller(u"caller");
        Object* a = rt_.allocate<Object>(rt_.object_prototype, ObjectClass::Arguments);
        a->reserveProps(args.size() + 3);
        for (size_t i = 0; i < args.size(); ++i) a->defineNew(i < k_index.size() ? k_index[i] : PropertyKey(utf16(std::to_string(i))), {args[i]});
        a->defineNew(k_length, {Value::number(static_cast<double>(args.size())), nullptr, nullptr, kWritable | kConfigurable});
        a->defineNew(PropertyKey(rt_.sym_iterator), {rt_.get(Value::object(rt_.array_prototype), k_values), nullptr, nullptr, kWritable | kConfigurable});
        // callee: the function itself, in sloppy code with plain parameters (frameworks'
        // `o.extend = arguments.callee`); in strict code, %ThrowTypeError% both ways.
        if (!info->strict && info->simple_params) {
            a->defineNew(k_callee, {Value::object(f), nullptr, nullptr, kWritable | kConfigurable});
        } else if (const Property* caller = rt_.function_prototype->own(k_caller); caller && caller->getter) {
            a->defineNew(k_callee, {Value::undefined(), caller->getter, caller->getter, kAccessor});
        }
        return a;
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
                        const Node* n = nodes[o[1]];
                        site.url = &prog_->url;
                        site.line_starts = &prog_->line_starts;
                        site.line = n->line;
                        site.offset = n->offset;
                        pc += 2;
                        break;
                    }
                    case OpUndef: r[o[1]] = Value::undefined(); pc += 2; break;
                    case OpNull: r[o[1]] = Value::null(); pc += 2; break;
                    case OpTrue: r[o[1]].setBoolean(true); pc += 2; break;
                    case OpFalse: r[o[1]].setBoolean(false); pc += 2; break;
                    case OpEmpty: r[o[1]] = Value::empty(); pc += 2; break;
                    case OpInt: r[o[1]].setNumber(static_cast<double>(o[2])); pc += 3; break;
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
                        if (const Value* g = cachedGlobal(o[2])) r[o[1]] = *g;
                        else if (Binding* b = cachedBinding(closure, nm); b && b->initialized) r[o[1]] = b->value;
                        else r[o[1]] = findGlobal(o[2]);
                        pc += 3;
                        break;
                    }
                    case OpLoadCallee: {
                        Node* nm = nodes[o[3]];
                        if (const Value* g = cachedGlobal(o[3])) {
                            r[o[1]] = *g;
                            r[o[2]] = Value::undefined();
                        } else if (Binding* b = cachedBinding(closure, nm); b && b->initialized) {
                            r[o[1]] = b->value;
                            r[o[2]] = Value::undefined();
                        } else {
                            Object* with = nullptr;
                            if (!findBinding(closure, nm->str, nullptr, nullptr, &with) && with) {
                                r[o[2]] = Value::object(with);
                                r[o[1]] = rt_.get(r[o[2]], PropertyKey(nm->str));
                            } else {
                                r[o[2]] = Value::undefined();
                                r[o[1]] = findGlobal(o[3]);
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
                        if (findBinding(closure, nm->str, nullptr, nullptr, &with)) r[o[1]].setBoolean(false);
                        else if (with) r[o[1]].setBoolean(rt_.deleteProperty(with, nm->str));
                        else r[o[1]].setBoolean(rt_.deleteProperty(globalObject(closure), nm->str));
                        pc += 3;
                        break;
                    }
                    case OpLoadThis: r[o[1]] = thisOf(); pc += 2; break;
                    case OpGetNamed: {
                        Node* m = nodes[o[3]];
                        if (r[o[2]].isObject()) {
                            Object* obj = r[o[2]].asObject();
                            if (o[3] < code.get_cache.size())
                                if (const Property* p = cachedGet(code.get_cache[o[3]], obj)) {
                                    if (!(p->flags & kAccessor)) r[o[1]] = p->value;
                                    else r[o[1]] = p->getter ? rt_.call(Value::object(p->getter), Value::object(obj), {}) : Value::undefined();
                                    pc += 4;
                                    break;
                                }
                            if (!m->key) m->key = std::make_unique<PropertyKey>(m->str);
                            if (shapeKey(*m->key)) {
                                if (code.get_cache.size() < code.nodes.size()) code.get_cache.resize(code.nodes.size());
                                if (fillGet(code.get_cache[o[3]], obj, *m->key)) {
                                    const Property* p = cachedGet(code.get_cache[o[3]], obj);
                                    if (!(p->flags & kAccessor)) r[o[1]] = p->value;
                                    else r[o[1]] = p->getter ? rt_.call(Value::object(p->getter), Value::object(obj), {}) : Value::undefined();
                                    pc += 4;
                                    break;
                                }
                            }
                        }
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
                        Object* obj = r[o[1]].isObject() ? r[o[1]].asObject() : nullptr;
                        if (obj && o[2] < code.set_cache.size()) {
                            VmCode::PropCache& c = code.set_cache[o[2]];
                            if (c.shape && obj->shape() == c.shape && !obj->ownLater()) {
                                Property& p = obj->slot(c.slot);
                                if ((p.flags & (kAccessor | kWritable)) == kWritable) {
                                    p.value = r[o[3]];
                                    pc += 4;
                                    break;
                                }
                            }
                            if (cachedAdd(c, obj)) {
                                obj->addShaped(const_cast<Shape*>(c.added), Property{r[o[3]]});
                                pc += 4;
                                break;
                            }
                        }
                        if (!m->key) m->key = std::make_unique<PropertyKey>(m->str);
                        const Shape* before = obj ? obj->shape() : nullptr;
                        rt_.set(r[o[1]], *m->key, r[o[3]], strict);
                        // What it found, or what it added, kept for the next time.
                        if (obj && obj->shape() && !obj->ownLater() && shapeKey(*m->key) &&
                            (obj->cls == ObjectClass::Ordinary || obj->cls == ObjectClass::Function)) {
                            if (code.set_cache.size() < code.nodes.size()) code.set_cache.resize(code.nodes.size());
                            VmCode::PropCache& c = code.set_cache[o[2]];
                            const ptrdiff_t at = obj->shape()->find(*m->key, Object::keyHash(*m->key));
                            if (at >= 0 && before == obj->shape()) {
                                c.shape = obj->shape();
                                c.slot = static_cast<uint32_t>(at);
                            } else if (at >= 0 && before && obj->shape()->parent() == before &&
                                       static_cast<size_t>(at) + 1 == obj->shape()->count() && obj->slot(static_cast<size_t>(at)).flags == kDefaultFlags) {
                                bool ok = true;
                                Object* p = obj->prototype;
                                const Shape* chain[3] = {};
                                for (int i = 0; i < 3 && p; ++i, p = p->prototype) {
                                    if (!p->shape() || p->ownLater()) ok = false;
                                    chain[i] = p->shape();
                                }
                                if (ok && !p) {
                                    c.added = obj->shape();
                                    std::copy(chain, chain + 3, c.chain);
                                }
                            }
                        }
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
                        r[o[1]].setBoolean(ok);
                        pc += 4;
                        break;
                    }
                    case OpBin: r[o[1]] = binaryOp(static_cast<Op>(o[4]), r[o[2]], r[o[3]]); pc += 5; break;
                    case OpBAdd: {
                        const Value &a = r[o[2]], &b = r[o[3]];
                        if (a.isNumber() && b.isNumber()) {
                            const double x = a.asNumber(), y = b.asNumber();
                            r[o[1]].setNumber(x + y);
                        } else {
                            r[o[1]] = binaryOp(Op::Add, a, b);
                        }
                        pc += 5;
                        break;
                    }
                    case OpBSub: {
                        const Value &a = r[o[2]], &b = r[o[3]];
                        if (a.isNumber() && b.isNumber()) {
                            const double x = a.asNumber(), y = b.asNumber();
                            r[o[1]].setNumber(x - y);
                        } else {
                            r[o[1]] = binaryOp(Op::Sub, a, b);
                        }
                        pc += 5;
                        break;
                    }
                    case OpBMul: {
                        const Value &a = r[o[2]], &b = r[o[3]];
                        if (a.isNumber() && b.isNumber()) {
                            const double x = a.asNumber(), y = b.asNumber();
                            r[o[1]].setNumber(x * y);
                        } else {
                            r[o[1]] = binaryOp(Op::Mul, a, b);
                        }
                        pc += 5;
                        break;
                    }
                    case OpBLt: {
                        const Value &a = r[o[2]], &b = r[o[3]];
                        if (a.isNumber() && b.isNumber()) {
                            const double x = a.asNumber(), y = b.asNumber();
                            r[o[1]].setBoolean(x < y);
                        } else {
                            r[o[1]] = binaryOp(Op::Lt, a, b);
                        }
                        pc += 5;
                        break;
                    }
                    case OpBGt: {
                        const Value &a = r[o[2]], &b = r[o[3]];
                        if (a.isNumber() && b.isNumber()) {
                            const double x = a.asNumber(), y = b.asNumber();
                            r[o[1]].setBoolean(x > y);
                        } else {
                            r[o[1]] = binaryOp(Op::Gt, a, b);
                        }
                        pc += 5;
                        break;
                    }
                    case OpBLe: {
                        const Value &a = r[o[2]], &b = r[o[3]];
                        if (a.isNumber() && b.isNumber()) {
                            const double x = a.asNumber(), y = b.asNumber();
                            r[o[1]].setBoolean(x <= y);
                        } else {
                            r[o[1]] = binaryOp(Op::Le, a, b);
                        }
                        pc += 5;
                        break;
                    }
                    case OpBGe: {
                        const Value &a = r[o[2]], &b = r[o[3]];
                        if (a.isNumber() && b.isNumber()) {
                            const double x = a.asNumber(), y = b.asNumber();
                            r[o[1]].setBoolean(x >= y);
                        } else {
                            r[o[1]] = binaryOp(Op::Ge, a, b);
                        }
                        pc += 5;
                        break;
                    }
                    case OpBStrictEq: {
                        const Value &a = r[o[2]], &b = r[o[3]];
                        if (a.isNumber() && b.isNumber()) {
                            const double x = a.asNumber(), y = b.asNumber();
                            r[o[1]].setBoolean(x == y);
                        } else {
                            r[o[1]] = binaryOp(Op::StrictEq, a, b);
                        }
                        pc += 5;
                        break;
                    }
                    case OpBStrictNe: {
                        const Value &a = r[o[2]], &b = r[o[3]];
                        if (a.isNumber() && b.isNumber()) {
                            const double x = a.asNumber(), y = b.asNumber();
                            r[o[1]].setBoolean(x != y);
                        } else {
                            r[o[1]] = binaryOp(Op::StrictNe, a, b);
                        }
                        pc += 5;
                        break;
                    }
                    case OpBBitAnd: {
                        const Value &a = r[o[2]], &b = r[o[3]];
                        if (a.isNumber() && b.isNumber() && int32Exact(a.asNumber()) && int32Exact(b.asNumber())) {
                            const double x = a.asNumber(), y = b.asNumber();
                            r[o[1]].setNumber(static_cast<double>(toI32(x) & toI32(y)));
                        } else {
                            r[o[1]] = binaryOp(Op::BitAnd, a, b);
                        }
                        pc += 5;
                        break;
                    }
                    case OpBBitOr: {
                        const Value &a = r[o[2]], &b = r[o[3]];
                        if (a.isNumber() && b.isNumber() && int32Exact(a.asNumber()) && int32Exact(b.asNumber())) {
                            const double x = a.asNumber(), y = b.asNumber();
                            r[o[1]].setNumber(static_cast<double>(toI32(x) | toI32(y)));
                        } else {
                            r[o[1]] = binaryOp(Op::BitOr, a, b);
                        }
                        pc += 5;
                        break;
                    }
                    case OpBBitXor: {
                        const Value &a = r[o[2]], &b = r[o[3]];
                        if (a.isNumber() && b.isNumber() && int32Exact(a.asNumber()) && int32Exact(b.asNumber())) {
                            const double x = a.asNumber(), y = b.asNumber();
                            r[o[1]].setNumber(static_cast<double>(toI32(x) ^ toI32(y)));
                        } else {
                            r[o[1]] = binaryOp(Op::BitXor, a, b);
                        }
                        pc += 5;
                        break;
                    }
                    case OpBShl: {
                        const Value &a = r[o[2]], &b = r[o[3]];
                        if (a.isNumber() && b.isNumber() && int32Exact(a.asNumber()) && int32Exact(b.asNumber())) {
                            const double x = a.asNumber(), y = b.asNumber();
                            r[o[1]].setNumber(static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(toI32(x)) << (static_cast<uint32_t>(toI32(y)) & 31))));
                        } else {
                            r[o[1]] = binaryOp(Op::Shl, a, b);
                        }
                        pc += 5;
                        break;
                    }
                    case OpBShr: {
                        const Value &a = r[o[2]], &b = r[o[3]];
                        if (a.isNumber() && b.isNumber() && int32Exact(a.asNumber()) && int32Exact(b.asNumber())) {
                            const double x = a.asNumber(), y = b.asNumber();
                            r[o[1]].setNumber(static_cast<double>(toI32(x) >> (static_cast<uint32_t>(toI32(y)) & 31)));
                        } else {
                            r[o[1]] = binaryOp(Op::Shr, a, b);
                        }
                        pc += 5;
                        break;
                    }
                    case OpBUShr: {
                        const Value &a = r[o[2]], &b = r[o[3]];
                        if (a.isNumber() && b.isNumber() && int32Exact(a.asNumber()) && int32Exact(b.asNumber())) {
                            const uint32_t x = static_cast<uint32_t>(toI32(a.asNumber())), y = static_cast<uint32_t>(toI32(b.asNumber()));
                            r[o[1]].setNumber(static_cast<double>(x >> (y & 31)));
                        } else {
                            r[o[1]] = binaryOp(Op::UShr, a, b);
                        }
                        pc += 5;
                        break;
                    }

#define XWEB_IMM(OPC, OPER, COND, FAST)                                                  \
                    case OPC: {                                                          \
                        const Value& a = r[o[2]];                                        \
                        const int32_t yi = static_cast<int32_t>(o[3]);                   \
                        if (a.isNumber() COND) {                                         \
                            const double x = a.asNumber();                               \
                            const double y = static_cast<double>(yi);                    \
                            (void)y;                                                     \
                            FAST;                                                        \
                        } else {                                                         \
                            r[o[1]] = binaryOp(OPER, a, Value::number(static_cast<double>(yi))); \
                        }                                                                \
                        pc += 5;                                                         \
                        break;                                                           \
                    }
                    XWEB_IMM(OpIAdd, Op::Add, , r[o[1]].setNumber(x + y))
                    XWEB_IMM(OpISub, Op::Sub, , r[o[1]].setNumber(x - y))
                    XWEB_IMM(OpIBitAnd, Op::BitAnd, && int32Exact(a.asNumber()), r[o[1]].setNumber(static_cast<double>(toI32(x) & yi)))
                    XWEB_IMM(OpIBitOr, Op::BitOr, && int32Exact(a.asNumber()), r[o[1]].setNumber(static_cast<double>(toI32(x) | yi)))
                    XWEB_IMM(OpIBitXor, Op::BitXor, && int32Exact(a.asNumber()), r[o[1]].setNumber(static_cast<double>(toI32(x) ^ yi)))
                    XWEB_IMM(OpIShl, Op::Shl, && int32Exact(a.asNumber()),
                             r[o[1]].setNumber(static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(toI32(x)) << (static_cast<uint32_t>(yi) & 31)))))
                    XWEB_IMM(OpIShr, Op::Shr, && int32Exact(a.asNumber()), r[o[1]].setNumber(static_cast<double>(toI32(x) >> (static_cast<uint32_t>(yi) & 31))))
                    XWEB_IMM(OpIUShr, Op::UShr, && int32Exact(a.asNumber()),
                             r[o[1]].setNumber(static_cast<double>(static_cast<uint32_t>(toI32(x)) >> (static_cast<uint32_t>(yi) & 31))))
                    XWEB_IMM(OpILt, Op::Lt, , r[o[1]].setBoolean(x < y))
                    XWEB_IMM(OpIGt, Op::Gt, , r[o[1]].setBoolean(x > y))
                    XWEB_IMM(OpILe, Op::Le, , r[o[1]].setBoolean(x <= y))
                    XWEB_IMM(OpIGe, Op::Ge, , r[o[1]].setBoolean(x >= y))
#undef XWEB_IMM
                    // Strict equality with a number: only a number can be.
                    case OpIStrictEq: r[o[1]].setBoolean(r[o[2]].isNumber() && r[o[2]].asNumber() == static_cast<double>(static_cast<int32_t>(o[3]))); pc += 5; break;
                    case OpIStrictNe: r[o[1]].setBoolean(!(r[o[2]].isNumber() && r[o[2]].asNumber() == static_cast<double>(static_cast<int32_t>(o[3])))); pc += 5; break;

                    case OpNot: r[o[1]].setBoolean(!rt_.toBoolean(r[o[2]])); pc += 3; break;
                    case OpNeg: case OpBitNot: {
                        Value v = r[o[2]];
                        if (!v.isNumber()) v = rt_.toPrimitive(v, "number");
                        if (v.isBigInt()) {
                            const bigint::Big x = bigint::decode(v.bigintDigits());
                            r[o[1]] = Value::bigint(JSString::make(bigint::encode(o[0] == OpNeg ? bigint::negate(x) : bigint::bitNot(x))));
                        } else if (o[0] == OpNeg) {
                            r[o[1]].setNumber(-rt_.toNumber(v));
                        } else {
                            r[o[1]].setNumber(~rt_.toInt32(v));
                        }
                        pc += 3;
                        break;
                    }
                    case OpPlus: r[o[1]].setNumber(rt_.toNumber(r[o[2]])); pc += 3; break;
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
                            r[o[1]].setNumber(v.asNumber() + delta);
                        } else {
                            const Value p = v.isBigInt() ? v : rt_.toPrimitive(v, "number");
                            if (p.isBigInt()) {
                                const bigint::Big x = bigint::decode(p.bigintDigits());
                                r[o[1]] = Value::bigint(JSString::make(bigint::encode(bigint::add(x, bigint::fromInt64(o[0] == OpInc ? 1 : -1)))));
                            } else {
                                r[o[1]].setNumber(rt_.toNumber(p) + delta);
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
                    case OpJumpIfNotNullish: pc = r[o[1]].isNullish() ? pc + 3 : o[2]; break;
                    case OpJumpIfNull: pc = r[o[1]].isNull() ? o[2] : pc + 3; break;
                    case OpJumpIfNotNull: pc = r[o[1]].isNull() ? pc + 3 : o[2]; break;
                    case OpReturnUndef: result = Value::undefined(); goto done;
#define XWEB_JCMP(NAME, OPER, B, CMP, SLOW)                                               \
                    case OpJ##NAME##T: case OpJ##NAME##F: {                              \
                        const Value& a = r[o[1]];                                        \
                        const Value& bv = B;                                             \
                        bool t;                                                          \
                        if (a.isNumber() && bv.isNumber()) t = a.asNumber() CMP bv.asNumber(); \
                        else t = SLOW;                                                   \
                        pc = t == (o[0] == OpJ##NAME##T) ? o[3] : pc + 4;                \
                        break;                                                           \
                    }
                    XWEB_JCMP(RLt, Op::Lt, r[o[2]], <, rt_.toBoolean(binaryOp(Op::Lt, a, bv)))
                    XWEB_JCMP(RLe, Op::Le, r[o[2]], <=, rt_.toBoolean(binaryOp(Op::Le, a, bv)))
                    XWEB_JCMP(RGt, Op::Gt, r[o[2]], >, rt_.toBoolean(binaryOp(Op::Gt, a, bv)))
                    XWEB_JCMP(RGe, Op::Ge, r[o[2]], >=, rt_.toBoolean(binaryOp(Op::Ge, a, bv)))
#undef XWEB_JCMP
                    case OpJRStrictEqT: case OpJRStrictEqF: {
                        const Value& a = r[o[1]];
                        const Value& b = r[o[2]];
                        bool t;
                        if (a.isNumber() && b.isNumber()) t = a.asNumber() == b.asNumber();
                        else if (a.type() != b.type()) t = false;
                        else if (a.isObject()) t = a.asObject() == b.asObject();
                        else if (a.isString()) t = a.sameString(b) || a.asString() == b.asString();
                        else t = rt_.toBoolean(binaryOp(Op::StrictEq, a, b));
                        pc = t == (o[0] == OpJRStrictEqT) ? o[3] : pc + 4;
                        break;
                    }
                    case OpJIStrictEqT: case OpJIStrictEqF: {
                        const Value& a = r[o[1]];
                        const bool t = a.isNumber() && a.asNumber() == static_cast<double>(static_cast<int32_t>(o[2]));
                        pc = t == (o[0] == OpJIStrictEqT) ? o[3] : pc + 4;
                        break;
                    }
#define XWEB_JIMM(NAME, OPER, CMP)                                                       \
                    case OpJI##NAME##T: case OpJI##NAME##F: {                            \
                        const Value& a = r[o[1]];                                        \
                        const double y = static_cast<double>(static_cast<int32_t>(o[2])); \
                        bool t;                                                          \
                        if (a.isNumber()) t = a.asNumber() CMP y;                        \
                        else t = rt_.toBoolean(binaryOp(OPER, a, Value::number(y)));     \
                        pc = t == (o[0] == OpJI##NAME##T) ? o[3] : pc + 4;               \
                        break;                                                           \
                    }
                    XWEB_JIMM(Lt, Op::Lt, <)
                    XWEB_JIMM(Le, Op::Le, <=)
                    XWEB_JIMM(Gt, Op::Gt, >)
                    XWEB_JIMM(Ge, Op::Ge, >=)
#undef XWEB_JIMM

                    case OpCall: {
                        Node* call = nodes[o[6]];
                        here(call);
                        const Value& fn = r[o[2]];
                        // Another compiled function: straight into it.
                        if (fn.isObject()) {
                            Object* fo = fn.asObject();
                            if (fo->cls == ObjectClass::Function && fo->code && !fo->native && !fo->bound_target && !fo->class_constructor && !vmProfiling()) {
                                FunctionInfo* ci = fo->code->info;
                                if (!ci->is_async && !ci->is_generator)
                                    if (VmCode* cc = vmCodeFor(ci)) {
                                        Value v = callCompiled(fo, *cc, r[o[3]], std::span<const Value>(r + o[4], o[5]));
                                        r[o[1]] = std::move(v);
                                        pc += 7;
                                        break;
                                    }
                            }
                        }
                        // A built-in: called straight too (the arguments are registers, on the stack).
                        if (fn.isObject()) {
                            Object* fo = fn.asObject();
                            if (fo->cls == ObjectClass::Function && fo->native && !fo->bound_target) {
                                if (rt_.call_depth >= Runtime::kMaxCallDepth) rt_.throwError("RangeError", "too much recursion");
                                ++rt_.call_depth;
                                struct Down {
                                    Runtime& rt;
                                    Object* saved;
                                    ~Down() {
                                        --rt.call_depth;
                                        rt.construct_target = saved;
                                    }
                                } down{rt_, rt_.construct_target};
                                rt_.construct_target = nullptr;   // (called, not constructed)
                                const std::span<const Value> av(r + o[4], o[5]);
                                Value v;
                                if (fo->gc_safe) v = fo->native(rt_, r[o[3]], av);
                                else {
                                    Runtime::NoGc no_gc(rt_);
                                    v = fo->native(rt_, r[o[3]], av);
                                }
                                r[o[1]] = std::move(v);
                                pc += 7;
                                break;
                            }
                        }
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
                        // A compiled constructor: straight into it, as a call is.
                        if (Object* fo = fn.asObject(); fo->cls == ObjectClass::Function && fo->code && !fo->native && !fo->bound_target && !vmProfiling()) {
                            FunctionInfo* ci = fo->code->info;
                            if (!ci->is_async && !ci->is_generator)
                                if (VmCode* cc = vmCodeFor(ci)) {
                                    Value v = callCompiled(fo, *cc, Value::undefined(), std::span<const Value>(r + o[3], o[4]), fo);
                                    if (v.isObject())
                                        cc->ctor_props = std::max<uint32_t>(cc->ctor_props, std::min<uint32_t>(static_cast<uint32_t>(v.asObject()->shapedCount()), 32));
                                    r[o[1]] = std::move(v);
                                    pc += 6;
                                    break;
                                }
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
                    case OpNewObject: {
                        Object* obj = rt_.newObject();
                        obj->reserveProps(o[2]);
                        r[o[1]] = Value::object(obj);
                        pc += 3;
                        break;
                    }
                    case OpDefineNamed: {
                        Node* p = nodes[o[2]];
                        Object* obj = r[o[1]].asObject();
                        // A literal's next key: the same shape on from the same shape as the last time.
                        if (o[2] < code.set_cache.size()) {
                            const VmCode::PropCache& c = code.set_cache[o[2]];
                            if (c.added && obj->shape() == c.added->parent()) {
                                obj->addShaped(const_cast<Shape*>(c.added), Property{r[o[3]]});
                                pc += 4;
                                break;
                            }
                        }
                        if (!p->key) p->key = std::make_unique<PropertyKey>(p->str);
                        const Shape* before = obj->shape();
                        obj->define(*p->key, {r[o[3]]});
                        if (before && obj->shape() && obj->shape()->parent() == before && !obj->ownLater()) {
                            if (code.set_cache.size() < code.nodes.size()) code.set_cache.resize(code.nodes.size());
                            code.set_cache[o[2]].added = obj->shape();
                        }
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
                            pc += 7;
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
                            for (uint32_t i = 0; i < o[6]; ++i) {
                                setEnv(env->outer);
                                --edepth;
                            }
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
                                    it->elements.push_back(Value::string(k.name.str()));
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
                        if (!b.initialized) rt_.throwError("ReferenceError", "can't use a variable before it's declared");
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
                    case OpPushEnv: {
                        Env* b = rt_.allocate<Env>(env);
                        const auto& names = code.block_envs[o[1]];
                        b->vars.reserve(names.size());
                        for (const VmCode::EnvName& e : names)
                            b->vars.bind(e.name, VarMap::hashOf(e.name)) = Binding{Value::undefined(), !e.is_const, false};
                        setEnv(b);
                        ++edepth;
                        pc += 2;
                        break;
                    }
                    case OpPopEnv: setEnv(env->outer); --edepth; pc += 1; break;
                    case OpCopyEnv: {
                        Env* c = rt_.allocate<Env>(env->outer);
                        c->vars = env->vars;
                        setEnv(c);
                        pc += 1;
                        break;
                    }
                    case OpSuperGet: case OpSuperSet: {
                        const bool get = o[0] == OpSuperGet;
                        Node* m = nodes[get ? o[2] : o[1]];
                        const uint32_t kreg = get ? o[3] : o[2];
                        Object* base;
                        if (info->arrow) base = superObject(closure);
                        else {
                            if (!fd.home) rt_.throwError("SyntaxError", "super isn't allowed here");
                            base = fd.home->prototype;
                            if (!base) rt_.throwError("TypeError", "super has no prototype here");
                        }
                        const Value self = thisOf();
                        const PropertyKey key = kreg != kNone ? rt_.toPropertyKey(r[kreg]) : PropertyKey(m->str);
                        if (get) r[o[1]] = rt_.get(base, key, self);
                        else rt_.set(base, key, r[o[3]], self);
                        pc += 4;
                        break;
                    }
                    case OpSuperCall: {
                        // The parent constructor makes this; then this class's fields go on it.
                        Env* fe = nullptr;
                        Object* ctor = f;
                        Object* nt = new_target;
                        if (info->arrow) {
                            fe = functionEnv(closure);
                            while (fe && !fe->function && fe->outer) fe = functionEnv(fe->outer);
                            if (!fe || !fe->function || !fe->function->derived) rt_.throwError("SyntaxError", "super() is only allowed in a derived class's constructor");
                            ctor = fe->function;
                            nt = fe->new_target;
                        } else {
                            if (!derived_ctor) rt_.throwError("SyntaxError", "super() is only allowed in a derived class's constructor");
                            fe = fenv;
                        }
                        if (fe ? fe->this_initialized : this_init) rt_.throwError("ReferenceError", "super() was already called");
                        Value result;
                        if (o[4] != kNone) {
                            const std::vector<Value> list = r[o[4]].asObject()->elements;
                            result = rt_.construct(Value::object(ctor->prototype), list, nt);
                        } else {
                            result = rt_.construct(Value::object(ctor->prototype), std::span<const Value>(r + o[2], o[3]), nt);
                        }
                        if (fe) {
                            fe->this_value = result;
                            fe->this_initialized = true;
                        } else {
                            this_value = result;
                            this_init = true;
                        }
                        // This class's own fields, now there's an object for them.
                        FunctionInfo* cinfo = ctor->code->info;
                        Object* obj = result.asObject();
                        for (Node* field : cinfo->fields) {
                            Env* fe2 = fieldEnv(ctor, result);
                            const PropertyKey key = (field->flags & kComputed) ? rt_.toPropertyKey(eval(field->a, fe2, true)) : PropertyKey(field->str);
                            const Value v = field->b ? eval(field->b, fe2, true) : Value::undefined();
                            obj->define(key, {v});
                        }
                        r[o[1]] = result;
                        pc += 5;
                        break;
                    }
                    case OpYield: {
                        Value sent = generatorYield(rt_, r[o[2]]);
                        r[o[1]] = std::move(sent);
                        pc += 3;
                        break;
                    }
                    case OpYieldStar: {
                        Value got = generatorDelegate(rt_, r[o[2]]);
                        r[o[1]] = std::move(got);
                        pc += 3;
                        break;
                    }
                    case OpAwait: {
                        Value got = awaitValue(rt_, r[o[2]]);
                        r[o[1]] = std::move(got);
                        pc += 3;
                        break;
                    }
                    case OpNewTarget: {
                        if (info->arrow) {
                            Env* fe = functionEnv(closure);
                            r[o[1]] = fe && fe->new_target ? Value::object(fe->new_target) : Value::undefined();
                        } else {
                            r[o[1]] = new_target ? Value::object(new_target) : Value::undefined();
                        }
                        pc += 2;
                        break;
                    }
                    case OpRestArgs: {
                        std::vector<Value> rest;
                        for (size_t i = o[2]; i < args.size(); ++i) rest.push_back(args[i]);
                        r[o[1]] = Value::object(rt_.newArray(std::move(rest)));
                        pc += 3;
                        break;
                    }
                    case OpJumpIfNotUndef: pc = r[o[1]].isUndefined() ? pc + 3 : o[2]; break;
                    case OpCheckObjectCoercible: {
                        const Value& v = r[o[1]];
                        if (v.isNullish()) rt_.throwError("TypeError", "can't destructure " + std::string(v.isNull() ? "null" : "undefined"));
                        pc += 2;
                        break;
                    }
                    case OpIterGet: {
                        const IterRec it = getIterator(r[o[2]], false);
                        Object* h = rt_.allocate<Object>(nullptr, ObjectClass::Internal);
                        packIter(h, it);
                        r[o[1]] = Value::object(h);
                        pc += 3;
                        break;
                    }
                    case OpIterValue: case OpIterNext: {
                        const bool value_op = o[0] == OpIterValue;
                        Object* h = r[value_op ? o[2] : o[1]].asObject();
                        IterRec it = unpackIter(h);
                        Value got;
                        bool ok;
                        try {
                            ok = iterStep(it, got);
                        } catch (...) {
                            packIter(h, it);   // (left done by a failing next())
                            throw;
                        }
                        packIter(h, it);
                        if (value_op) {
                            r[o[1]] = ok ? std::move(got) : Value::undefined();
                            pc += 3;
                        } else if (ok) {
                            r[o[2]] = std::move(got);
                            pc += 4;
                        } else {
                            pc = o[3];
                        }
                        break;
                    }
                    case OpIterRest: {
                        Object* h = r[o[2]].asObject();
                        IterRec it = unpackIter(h);
                        std::vector<Value> items;
                        {
                            TempRoots roots(rt_, items);
                            Value x;
                            try {
                                while (iterStep(it, x)) items.push_back(x);
                            } catch (...) {
                                packIter(h, it);
                                throw;
                            }
                        }
                        packIter(h, it);
                        r[o[1]] = Value::object(rt_.newArray(std::move(items)));
                        pc += 3;
                        break;
                    }
                    case OpIterClose: case OpIterCloseFin: {
                        Object* h = r[o[1]].asObject();
                        bool suppress = false;
                        if (o[0] == OpIterCloseFin) suppress = o[2] == kNone || static_cast<int>(r[o[2]].asNumber()) == kFinThrow;
                        IterRec it = unpackIter(h);
                        try {
                            iterClose(it, suppress);
                        } catch (...) {
                            packIter(h, it);
                            throw;
                        }
                        packIter(h, it);
                        pc += o[0] == OpIterClose ? 2 : 3;
                        break;
                    }
                    case OpObjRest: {
                        const Value& value = r[o[2]];
                        const Object* used = r[o[3]].asObject();
                        std::vector<PropertyKey> keys;
                        for (const Value& k : used->elements) keys.push_back(rt_.toPropertyKey(k));
                        Object* rest = rt_.newObject();
                        Object* src = rt_.toObject(value);
                        for (const PropertyKey& key : rt_.ownPropertyKeys(src)) {
                            if (std::find(keys.begin(), keys.end(), key) != keys.end()) continue;
                            Property prop;
                            if (!rt_.getOwnProperty(src, key, prop) || !(prop.flags & kEnumerable)) continue;
                            rt_.put(rest, key, rt_.get(src, key, value), kDefaultFlags);
                        }
                        r[o[1]] = Value::object(rest);
                        pc += 4;
                        break;
                    }
                    case OpArguments: r[o[1]] = Value::object(makeArguments()); pc += 2; break;
                    case OpNeedArguments:
                        if (r[o[1]].isEmpty()) r[o[1]] = Value::object(makeArguments());
                        pc += 2;
                        break;
                    case OpArgLen: {
                        if (r[o[2]].isEmpty()) r[o[1]].setNumber(static_cast<double>(args.size()));
                        else r[o[1]] = rt_.get(r[o[2]], PropertyKey(u"length"));
                        pc += 3;
                        break;
                    }
                    case OpArgElem: {
                        const Value& k = r[o[3]];
                        if (r[o[2]].isEmpty()) {
                            const double d = k.isNumber() ? k.asNumber() : -1;
                            const auto at_arg = d >= 0 && d < static_cast<double>(args.size()) ? static_cast<size_t>(d) : args.size();
                            if (at_arg < args.size() && static_cast<double>(at_arg) == d) {
                                r[o[1]] = args[at_arg];
                                pc += 4;
                                break;
                            }
                            r[o[2]] = Value::object(makeArguments());
                        }
                        r[o[1]] = rt_.get(r[o[2]], rt_.toPropertyKey(k));
                        pc += 4;
                        break;
                    }
                    default: rt_.throwError("InternalError", "bad bytecode");
                }
            }
        } catch (const GeneratorReturn& ret) {
            // generator.return(v) at a yield: through the finally blocks around it, then
            // returned (as a return statement there would).
            const VmCode::Handler* h = nullptr;
            for (const VmCode::Handler& c : code.handlers)
                if (c.is_finally && at >= c.start && at < c.end) {
                    h = &c;
                    break;
                }
            if (!h) throw;
            r[h->reg] = ret.value;
            r[h->kind_reg] = Value::number(kFinReturn);
            pc = h->fin_start;
            while (edepth > h->env_depth) {
                setEnv(env->outer);
                --edepth;
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
            // Out of the block scopes entered since.
            while (edepth > h->env_depth) {
                setEnv(env->outer);
                --edepth;
            }
        }
        (void)caught;
    }
done:
    if (new_target) {
        if (result.isObject()) return result;
        if (info->derived && !result.isUndefined()) rt_.throwError("TypeError", "a derived constructor may only return an object or undefined");
        if (derived_ctor) {
            if (!(fenv ? fenv->this_initialized : this_init)) rt_.throwError("ReferenceError", "a derived constructor must call super()");
            return fenv ? fenv->this_value : this_value;
        }
        return this_value;
    }
    return result;
}

}  // namespace fbx::xweb::js
