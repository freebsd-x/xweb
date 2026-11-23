#pragma once

// The tree-walking interpreter's class, shared by runtime.cc and the bytecode VM (vm.cc).
// Internal to the engine.

#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.h"
#include "coroutine.h"
#include "internal.h"
#include "xweb/js.h"

namespace fbx::xweb::js {

// Near the end of the engine's stack (or, off it, of whatever stack this is: then only
// the counts guard it).
inline bool stackExhausted(const char* sp) {
    // On the engine stack (above its bottom) and below the limit (under its top).
    return sp < g_engine_stack_limit && sp > g_engine_stack_bottom;
}

// Too deep to go on, by the stack sp is really on: a coroutine's own, or the engine's.
// (Not by current_co alone: microtasks run on the engine stack while a coroutine is
// current, and were checked against the coroutine's limit, a stack they weren't on;
// recursion there ran into the engine stack's guard page: tistory.com.)
inline bool stackTooDeep(const Runtime& rt, const char* sp) {
    for (const Coroutine* co = rt.current_co; co; co = co->resumer)
        if (co->mem && sp >= co->mem && sp < co->mem + co->mem_size) return sp < co->stack_limit;
    return stackExhausted(sp);
}


// ── Environments and functions ────────────────────────────────────────────────

// Control flow out of statements.
enum class CT : uint8_t { Normal, Return, Break, Continue };
struct Completion {
    CT type = CT::Normal;
    Value value;
    const std::u16string* label = nullptr;
};

struct ShortCircuit {};   // an optional chain's nullish link

struct VmCode;
VmCode* vmCodeFor(FunctionInfo* info);   // compiled for the VM, or null (vm.cc)

class Interpreter {
public:
    // prog is held by reference: every caller's outlives the Interpreter (a function's
    // code, a local, a parameter), and a call shouldn't pay for a refcount round trip.
    explicit Interpreter(Runtime& rt, const std::shared_ptr<Program>& prog) : rt_(rt), prog_(prog) {}
    ~Interpreter() {
        if (!owns_scopes_) return;
        auto& stack = rt_.owned_stack;
        while (stack.size() > owned_base_) {
            Env* e = stack.back();
            stack.pop_back();
            if (rt_.env_pool.size() < kEnvPoolMax) {
                e->reuse(nullptr);
                rt_.env_pool.push_back(e);
            } else {
                delete e;
            }
        }
    }
    static constexpr size_t kEnvPoolMax = 512;
    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    Value runScript(Env* global_env) {
        Program& p = *prog_;
        // Hoisting: the script's var names and function declarations on the global object.
        Object* g = global_env->global_object;
        for (const auto& name : p.top->var_names)
            if (!rt_.has(g, name)) g->define(name, {Value::undefined(), nullptr, nullptr, kWritable | kEnumerable});
        for (Node* f : p.top->function_decls) {
            Object* fn = makeFunction(f->fn, global_env, nullptr);
            g->define(f->str, {Value::object(fn), nullptr, nullptr, kWritable | kEnumerable});
        }
        declareLexical(global_env, p.root->lexical);
        Value last;
        EnvScope scope(rt_, global_env);
        for (Node* s : p.root->kids) {
            Completion c = exec(s, global_env, p.top->strict);
            if (!c.value.isEmpty() && c.type == CT::Normal && s->kind != N::FunctionDecl) last = c.value;
        }
        return last;
    }

    Object* makeFunction(FunctionInfo* info, Env* env, Object* home);

    // A module's scope, before anything runs: its vars, its function declarations
    // (callable from modules that import them even before this one runs), its lexical
    // names in their dead zone, and the default export's slot.
    void instantiateModule(Env* env) {
        Program& p = *prog_;
        for (const auto& name : p.top->var_names)
            if (!env->vars.count(name)) env->vars[name] = Binding{};
        // Lexical names first: a module's top-level functions are among them, and are
        // initialized at once.
        declareLexical(env, p.root->lexical);
        for (Node* f : p.top->function_decls) env->vars[f->str] = Binding{Value::object(makeFunction(f->fn, env, nullptr))};
        for (Node* s : p.root->kids) {
            if (s->kind != N::ExportDecl || !(s->flags & kDefault) || !s->a || s->a->kind == N::FunctionDecl || s->a->kind == N::ClassDecl) continue;
            if (s->flags & kHoisted) env->vars[u"*default*"] = Binding{Value::object(makeFunction(s->a->fn, env, nullptr))};
            else env->vars[u"*default*"] = Binding{Value::undefined(), true, false};
        }
    }

    // Eval code: let, const and class in a scope of its own (`lex`); var and function
    // declarations in the caller's variable scope (`var_env`, or the global object), or
    // in `lex` too when it's strict.
    Value runEval(Env* lex, Env* var_env, Object* var_object, bool strict) {
        Program& p = *prog_;
        for (const auto& name : p.top->var_names) {
            if (var_object) {
                if (!rt_.has(var_object, name)) var_object->define(name, {Value::undefined(), nullptr, nullptr, kDefaultFlags});
            } else if (!var_env->vars.count(name)) {
                var_env->vars[name] = Binding{};
            }
        }
        declareLexical(lex, p.root->lexical);
        for (Node* f : p.top->function_decls) {
            const Value fn = Value::object(makeFunction(f->fn, lex, nullptr));
            if (var_object) var_object->define(f->str, {fn, nullptr, nullptr, kDefaultFlags});
            else var_env->vars[f->str] = Binding{fn};
        }
        EnvScope scope(rt_, lex);
        Value last;
        for (Node* s : p.root->kids) {
            Completion c = exec(s, lex, strict);
            if (!c.value.isEmpty() && c.type == CT::Normal && s->kind != N::FunctionDecl) last = c.value;
        }
        return last;
    }

    // What eval code may use from where it's called.
    EvalContext evalContextOf(Env* env) {
        EvalContext ctx;
        Env* f = functionEnv(env);
        if (f && f->is_function && f->function) {
            ctx.new_target = true;
            ctx.super_property = f->home != nullptr;
            ctx.super_call = f->function->derived && f->function->code && f->function->code->info->is_class_constructor;
        } else if (f && f->field_init) {
            ctx.new_target = true;
            ctx.super_property = f->home != nullptr;
        }
        return ctx;
    }
    Env* varScopeOf(Env* env) {
        Env* f = functionEnv(env);
        return f;
    }

    void runModule(Env* env) {
        EnvScope scope(rt_, env);
        for (Node* s : prog_->root->kids) exec(s, env, true);
    }

    Value callScript(Object* f, const Value& this_value, std::span<const Value> args, Object* new_target);
    // A call run by the bytecode VM (vm.cc), its frame already on the call stack.
    Value runVm(Object* f, VmCode& code, const Value& this_value, std::span<const Value> args, Object* new_target);
    Value callCompiled(Object* f, VmCode& code, const Value& this_value, std::span<const Value> args, Object* new_target = nullptr);

private:
    struct EnvScope {
        Runtime& rt;
        EnvScope(Runtime& r, Env* e) : rt(r) { rt.env_stack.push_back(e); }
        ~EnvScope() { rt.env_stack.pop_back(); }
    };

    // A call whose scopes can't outlive it (ownsScopes) keeps them off the collected
    // heap: they're freed when the call's Interpreter goes, not at the next collection.
    Env* newEnv(Env* outer) {
        if (!owns_scopes_) return rt_.allocate<Env>(outer);
        Env* e;
        if (!rt_.env_pool.empty()) {
            e = rt_.env_pool.back();
            rt_.env_pool.pop_back();
            e->outer = outer;
        } else {
            e = new Env(outer);
        }
        rt_.owned_stack.push_back(e);
        return e;
    }
    // Nothing in the function can capture its scope: no nested function, class or
    // method, no direct eval, and no coroutine to keep it after a suspension. A class
    // constructor's instance fields are parsed as functions of their own but run in
    // its scope, so a constructor with fields keeps its scopes. (A collection only runs
    // between tasks, never while a call is on the stack.)
    static bool ownsScopes(const FunctionInfo* info) {
        static const bool off = std::getenv("XWEB_NO_OWNED_SCOPES") != nullptr;
        if (off) return false;
        return !info->scope_escapes && !info->is_async && !info->is_generator && info->fields.empty();
    }

    void declareLexical(Env* env, const std::vector<std::u16string>* names) {
        if (!names) return;
        if (env->global_object && !names->empty()) ++rt_.scope_gen;
        for (const auto& n : *names) env->vars[n] = Binding{Value::undefined(), true, false};
    }

    // ── Names ──

    // The binding an identifier found last time, if it is surely that one still: the
    // common case of findBinding, small enough to be inlined where names are read and
    // written. (Nothing found: findBinding decides, searching if it must.)
    Binding* cachedBinding(Env* env, Node* site) {
        const int32_t hops = site->scope_hops;
        const uint32_t slot = site->scope_slot;
        if (hops < 0 || slot >= kGlobalPos || !site->name_hash) return nullptr;
        Env* e = env;
        for (int32_t k = hops; k > 0; --k) {
            if (e->eval_vars) return nullptr;
            e = e->outer;
            if (!e) return nullptr;
        }
        if (e->with_object) return nullptr;
        VarMap::Entry* it = e->vars.at(slot, site->name_hash, site->str);
        return it && !it->second.import_env ? &it->second : nullptr;
    }

    // The binding a name means here. An import is followed to the exporting module's
    // binding (`imported` says it was one: those can't be assigned).
    // `with_hit`: where a with statement's object has the name first (the binding is
    // then its property, and nothing is returned). Without it, with scopes are passed by.
    Binding* findBinding(Env* env, const std::u16string& name, Env** where, bool* imported = nullptr, Object** with_hit = nullptr,
                         size_t hash = 0, Node* site = nullptr) {
        const size_t h = hash ? hash : VarMap::hashOf(name);
        // Where this identifier was found last time. A scope's names are all made when
        // it is (only a direct eval adds any later), so the same code finds a name in
        // the same scope at the same place: checked, not searched.
        // (Unless a direct eval has since declared vars in a scope on the way: then it's
        // searched. Turning this off for the whole page at the first such eval made every
        // name a search on pages with one, 11st.co.kr's.)
        const bool cacheable = site && site->scope_hops != -2;
        if (cacheable && site->scope_hops >= 0) {
            Env* e = env;
            for (int32_t k = site->scope_hops; k > 0 && e; --k) {
                if (e->eval_vars) {
                    e = nullptr;
                    break;
                }
                e = e->outer;
            }
            if (e && !e->with_object) {
                if (site->scope_slot < kGlobalPos) {
                    if (VarMap::Entry* it = e->vars.at(site->scope_slot, h, name); it && !it->second.import_env) {
                        if (where) *where = e;
                        if (imported) *imported = false;
                        return &it->second;
                    }
                } else if (!e->outer && !e->vars.find(name, h)) {
                    return nullptr;   // the global object's, as last time
                }
            }
        }
        int32_t hops = 0;
        for (Env* e = env; e; e = e->outer, ++hops) {
            if (e->with_object) {
                if (site) site->scope_hops = -2;
                if (with_hit && withHas(e->with_object, name)) {
                    *with_hit = e->with_object;
                    return nullptr;
                }
                continue;
            }
            if (VarMap::Entry* it = e->vars.find(name, h)) {
                if (where) *where = e;
                Binding* b = &it->second;
                if (cacheable && !b->import_env && site->scope_hops != -2) {
                    site->scope_hops = hops;
                    site->scope_slot = VarMap::posOf(it);
                }
                if (imported) *imported = b->import_env != nullptr;
                for (int hops = 0; b->import_env; ++hops) {
                    VarMap::Entry* jt = b->import_env->vars.find(b->import_name);
                    if (!jt || hops > 100) rt_.throwError("ReferenceError", "the import '" + utf8(name) + "' isn't there");
                    b = &jt->second;
                }
                return b;
            }
        }
        if (cacheable && site->scope_hops != -2 && hops > 0) {
            site->scope_hops = hops - 1;   // the outermost scope, then the global object
            site->scope_slot = kGlobalSlot;
        }
        return nullptr;
    }

    // Whether a with statement's object supplies this name: it has it, and its
    // Symbol.unscopables doesn't hide it.
    bool withHas(Object* o, const std::u16string& name) {
        const PropertyKey key(name);
        if (!rt_.has(o, key)) return false;
        const Value hidden = rt_.get(Value::object(o), PropertyKey(rt_.sym_unscopables));
        return !(hidden.isObject() && rt_.toBoolean(rt_.get(hidden, key)));
    }

    Object* globalObject(Env* env) {
        for (Env* e = env; e; e = e->outer)
            if (e->global_object) return e->global_object;
        return rt_.global();
    }

    // An identifier's name hash, worked out once.
    static size_t nameHash(Node* n) {
        if (!n->name_hash) n->name_hash = VarMap::hashOf(n->str);
        return n->name_hash;
    }

    // An identifier's name as a property key, made once (globals are looked up by it).
    static const PropertyKey& nameKey(Node* n) {
        if (!n->key) n->key = std::make_unique<PropertyKey>(n->str);
        return *n->key;
    }

    Value lookup(Env* env, const std::u16string& name, size_t hash = 0, const PropertyKey* name_key = nullptr, Node* site = nullptr) {
        Object* with = nullptr;
        if (Binding* b = findBinding(env, name, nullptr, nullptr, &with, hash, site)) {
            if (!b->initialized) rt_.throwError("ReferenceError", "can't use '" + utf8(name) + "' before it's declared");
            return b->value;
        }
        if (with) return rt_.get(Value::object(with), PropertyKey(name));
        Object* g = globalObject(env);
        std::optional<PropertyKey> made;
        const PropertyKey& key = name_key ? *name_key : site ? nameKey(site) : made.emplace(name);
        // The usual case, in one lookup: an ordinary global's own data property is
        // what has() then get() would find first. (A name's hash is its key's.) Where it
        // was last time, if it's there still (a script's top-level vars, each use).
        if (g->cls != ObjectClass::Proxy && g->cls != ObjectClass::TypedArray && g->cls != ObjectClass::String && g->elements.empty() && hash) {
            const bool cached = site && site->scope_slot >= kGlobalPos && site->scope_slot != kGlobalSlot;
            if (cached)
                if (const Property* p = g->ownAt(site->scope_slot - kGlobalPos, key, hash); p && !(p->flags & kAccessor)) return p->value;
            const ptrdiff_t at = g->ownPos(key, hash);
            if (at >= 0) {
                const Property* p = g->ownAt(static_cast<size_t>(at), key, hash);
                if (p && !(p->flags & kAccessor)) {
                    if (site && site->scope_slot >= kGlobalPos && static_cast<uint64_t>(at) < kGlobalSlot - kGlobalPos)
                        site->scope_slot = kGlobalPos + static_cast<uint32_t>(at);
                    return p->value;
                }
            }
        }
        if (name == u"undefined") return Value::undefined();
        if (rt_.has(g, key)) return rt_.get(g, key, Value::object(g));
        rt_.throwError("ReferenceError", utf8(name) + " is not defined");
    }

    void assignName(Env* env, const std::u16string& name, const Value& v, bool strict, Node* site = nullptr) {
        if (site)
            if (Binding* b = cachedBinding(env, site); b && b->initialized && b->is_mutable) {
                b->value = v;
                return;
            }
        bool imported = false;
        Object* with = nullptr;
        Binding* b = site ? findBinding(env, name, nullptr, &imported, &with, nameHash(site), site) : findBinding(env, name, nullptr, &imported, &with);
        if (with) {
            rt_.set(with, PropertyKey(name), v, Value::object(with));
            return;
        }
        if (b) {
            if (imported) rt_.throwError("TypeError", "assignment to the import '" + utf8(name) + "'");
            if (!b->initialized) rt_.throwError("ReferenceError", "can't use '" + utf8(name) + "' before it's declared");
            if (!b->is_mutable) {
                if (b->function_name && !strict) return;
                rt_.throwError("TypeError", "assignment to constant '" + utf8(name) + "'");
            }
            b->value = v;
            return;
        }
        Object* g = globalObject(env);
        // A global var where it was last time (as lookup), or found once: set there.
        if (site && site->scope_slot >= kGlobalPos && (g->cls == ObjectClass::Ordinary || g->cls == ObjectClass::Function)) {
            const size_t h = nameHash(site);
            Property* p = site->scope_slot != kGlobalSlot ? g->ownAt(site->scope_slot - kGlobalPos, nameKey(site), h) : nullptr;
            if (!p) {
                const ptrdiff_t at = g->ownPos(nameKey(site), h);
                p = at >= 0 ? g->ownAt(static_cast<size_t>(at), nameKey(site), h) : nullptr;
                if (p && static_cast<uint64_t>(at) < kGlobalSlot - kGlobalPos) site->scope_slot = kGlobalPos + static_cast<uint32_t>(at);
            }
            if (p && !(p->flags & kAccessor) && (p->flags & kWritable)) {
                p->value = v;
                return;
            }
        }
        if (strict && !rt_.has(g, name)) rt_.throwError("ReferenceError", utf8(name) + " is not defined");
        rt_.set(g, site ? nameKey(site) : PropertyKey(name), v, Value::object(g));
    }

    // Declares a pattern's names in env with these values (let/const/params/catch).
    void bindPattern(Node* pattern, const Value& value, Env* env, bool strict, uint32_t kind);

    // ── this, super, new.target ──

    Env* functionEnv(Env* env) {
        for (Env* e = env; e; e = e->outer)
            if (e->is_function || e->global_object) return e;
        return nullptr;
    }

    Value thisValue(Env* env) {
        Env* f = functionEnv(env);
        if (!f) return Value::undefined();
        if (f->global_object) return Value::object(f->global_object);
        if (!f->this_initialized) rt_.throwError("ReferenceError", "super() must be called before using this");
        return f->this_value;
    }

    // ── Statements ──

    Completion exec(Node* n, Env* env, bool strict);
    Completion execBlock(const std::vector<Node*>& stmts, Env* env, bool strict);
    void hoistBlockFunctions(const std::vector<Node*>& stmts, Env* env);
    Completion execLoopBody(Node* body, Env* env, bool strict, const std::u16string* label, bool& stop, Value& last);
    Completion forStatement(Node* n, Env* env, bool strict, const std::u16string* label);
    Completion forInOf(Node* n, Env* env, bool strict, const std::u16string* label);
    Completion forOf(Node* n, Env* env, bool strict, const std::u16string* label, const Value& subject, uint32_t decl);
    Completion tryStatement(Node* n, Env* env, bool strict);
    Completion switchStatement(Node* n, Env* env, bool strict, const std::u16string* label);
    void declare(Node* decl, Env* env, bool strict);
    Object* classDefinition(Node* n, Env* env);

    // ── Expressions ──

    // The leaves here, where every evaluation comes: the rest (evalNode) has a big
    // function's prologue and stack frame, which numbers and names needn't pay.
    Value eval(Node* n, Env* env, bool strict) {
        switch (n->kind) {
            case N::Number: return Value::number(n->number);
            case N::Identifier:
                if (Binding* b = cachedBinding(env, n); b && b->initialized) return b->value;
                return lookup(env, n->str, nameHash(n), nullptr, n);   // (its key made if a global's)
            default: return evalNode(n, env, strict);
        }
    }
    Value evalNode(Node* n, Env* env, bool strict);
    Value evalCall(Node* n, Env* env, bool strict);
    Value evalNew(Node* n, Env* env, bool strict);
    Value evalAssign(Node* n, Env* env, bool strict);
    Value evalUpdate(Node* n, Env* env, bool strict);
    Value evalUnary(Node* n, Env* env, bool strict);
    Value evalObject(Node* n, Env* env, bool strict);
    Value evalArray(Node* n, Env* env, bool strict);
    Value evalTemplate(Node* n, Env* env, bool strict);
    Value evalTagged(Node* n, Env* env, bool strict);
    void here(const Node* n);
    std::vector<Value> evalArgs(const std::vector<Node*>& args, Env* env, bool strict);
    void assignPattern(Node* pattern, const Value& v, Env* env, bool strict);
    Value superBase(Env* env);
    // super.x's object: a home object whose prototype is null has none (TypeError, as
    // ToObject(null) would be).
    Object* superObject(Env* env) {
        Object* o = superBase(env).asObject();
        if (!o) rt_.throwError("TypeError", "super has no prototype here");
        return o;
    }
    // A member access's key: a.b's from the node (kept there), a[x]'s made into `scratch`.
    // (`scratch`, a temporary by default, lives to the end of the caller's full expression.)
    const PropertyKey& memberKey(Node* m, Env* env, bool strict, PropertyKey&& scratch = PropertyKey{});

    // ── Iteration, one value at a time (for-of, destructuring) ──
    struct IterRec {
        Value iterator, next;
        Object* array = nullptr;   // an ordinary array with its own iterator: read directly
        Value string;              // a string: by code points, directly
        size_t index = 0;
        bool done = false;
        bool async = false;        // for await: results are awaited
        bool sync_wrapped = false; // for await over a sync iterator: values are awaited
    };
    IterRec getIterator(const Value& v, bool async);
    bool iterStep(IterRec& it, Value& out);
    void iterClose(IterRec& it, bool suppress);
    template <typename F>
    void destructureArray(Node* p, const Value& value, F&& assign);

  public:
    Env* prepareCall(Object* f, const Value& this_value, std::span<const Value> args, Object* new_target);
    Env* fieldEnv(Object* ctor, const Value& this_value);
    Value runBody(FunctionInfo* info, Env* env);
    Value runInEnv(FunctionInfo* info, Env* env) {
        EnvScope scope(rt_, env);
        return runBody(info, env);
    }
  private:
    // Two numbers, inline (a loop's condition, arithmetic, a hash's bit twiddling);
    // anything else, binaryOpSlow.
    Value binaryOp(Op op, const Value& l, const Value& r) {
        if (l.isNumber() && r.isNumber()) {
            const double x = l.asNumber(), y = r.asNumber();
            switch (op) {
                case Op::Lt: return Value::boolean(x < y);
                case Op::Gt: return Value::boolean(x > y);
                case Op::Le: return Value::boolean(x <= y);
                case Op::Ge: return Value::boolean(x >= y);
                case Op::Eq: case Op::StrictEq: return Value::boolean(x == y);
                case Op::Ne: case Op::StrictNe: return Value::boolean(x != y);
                case Op::Add: return Value::number(x + y);
                case Op::Sub: return Value::number(x - y);
                case Op::Mul: return Value::number(x * y);
                case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl: case Op::Shr: case Op::UShr: {
                    if (!(x > -9.2e18 && x < 9.2e18 && y > -9.2e18 && y < 9.2e18)) break;
                    const auto a = static_cast<uint32_t>(static_cast<int64_t>(x));
                    const auto b = static_cast<uint32_t>(static_cast<int64_t>(y));
                    switch (op) {
                        case Op::BitAnd: return Value::number(static_cast<int32_t>(a & b));
                        case Op::BitOr: return Value::number(static_cast<int32_t>(a | b));
                        case Op::BitXor: return Value::number(static_cast<int32_t>(a ^ b));
                        case Op::Shl: return Value::number(static_cast<int32_t>(a << (b & 31)));
                        case Op::Shr: return Value::number(static_cast<int32_t>(a) >> (b & 31));
                        default: return Value::number(a >> (b & 31));
                    }
                }
                default: break;
            }
        }
        return binaryOpSlow(op, l, r);
    }
    Value binaryOpSlow(Op op, const Value& l, const Value& r);

    Runtime& rt_;
    const std::shared_ptr<Program>& prog_;
    bool owns_scopes_ = false;
    size_t owned_base_ = 0;   // where this call's own scopes start in rt_.owned_stack
    struct ReusedScope {
        Env* env = nullptr;   // one of this call's owned scopes
        bool busy = false;
    };
    std::unordered_map<const Node*, ReusedScope> block_scopes_;
    friend class Runtime;
};

}  // namespace fbx::xweb::js
