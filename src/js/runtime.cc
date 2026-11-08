// The interpreter: values, objects and the language's operations, and evaluation of
// the parser's tree, statement by statement and expression by expression.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

#include "ast.h"
#include "coroutine.h"
#include "internal.h"
#include "xweb/js.h"

namespace fbx::xweb::js {

// ── Strings ───────────────────────────────────────────────────────────────────

std::u16string utf16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else { out += u'�'; ++i; continue; }
        if (i + n > s.size()) { out += u'�'; break; }
        bool bad = false;
        for (size_t k = 1; k < n; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { bad = true; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (bad) { out += u'�'; ++i; continue; }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (cp >> 10));
            out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<char16_t>(cp);
        }
    }
    return out;
}

std::string utf8(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;   // a lone surrogate
        }
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    return out;
}

Value Value::string(std::u16string s) { return string(std::make_shared<const std::u16string>(std::move(s))); }

size_t ValueHash::operator()(const Value& v) const noexcept {
    switch (v.type()) {
        case Value::Type::String: return std::hash<std::u16string>{}(v.asString());
        case Value::Type::Number: {
            const double d = v.asNumber();
            if (d == 0) return 0;               // +0 and -0 are the same key
            if (std::isnan(d)) return 1;
            return std::hash<double>{}(d);
        }
        case Value::Type::Boolean: return v.asBoolean() ? 3 : 2;
        case Value::Type::Object: return std::hash<const void*>{}(v.asObject());
        case Value::Type::Symbol: return std::hash<const void*>{}(v.asSymbol());
        default: return static_cast<size_t>(v.type()) + 7;
    }
}

bool ValueSameZero::operator()(const Value& a, const Value& b) const noexcept { return Runtime::sameValueZero(a, b); }

namespace {

// A canonical array index ("0", "17"; not "01" or "4294967295"), or -1.
int64_t arrayIndex(const std::u16string& s) {
    if (s.empty() || s.size() > 10) return -1;
    if (s.size() > 1 && s[0] == u'0') return -1;
    uint64_t v = 0;
    for (char16_t c : s) {
        if (c < u'0' || c > u'9') return -1;
        v = v * 10 + (c - u'0');
    }
    return v < 4294967295ull ? static_cast<int64_t>(v) : -1;
}

std::u16string indexKey(uint64_t i) {
    std::u16string s;
    if (i == 0) return u"0";
    while (i) {
        s.insert(s.begin(), static_cast<char16_t>(u'0' + i % 10));
        i /= 10;
    }
    return s;
}

}  // namespace

// ── Objects ───────────────────────────────────────────────────────────────────

Object::~Object() = default;

Property* Object::own(const PropertyKey& key) {
    const auto it = index_.find(key);
    return it == index_.end() ? nullptr : &props_[it->second].second;
}

const Property* Object::own(const PropertyKey& key) const {
    const auto it = index_.find(key);
    return it == index_.end() ? nullptr : &props_[it->second].second;
}

Property& Object::define(const PropertyKey& key, Property p) {
    const auto it = index_.find(key);
    if (it != index_.end()) {
        props_[it->second].second = std::move(p);
        return props_[it->second].second;
    }
    index_[key] = props_.size();
    props_.emplace_back(key, std::move(p));
    return props_.back().second;
}

bool Object::remove(const PropertyKey& key) {
    const auto it = index_.find(key);
    if (it == index_.end()) return false;
    const size_t at = it->second;
    props_.erase(props_.begin() + static_cast<std::ptrdiff_t>(at));
    index_.erase(it);
    for (auto& [k, i] : index_)
        if (i > at) --i;
    return true;
}

std::vector<PropertyKey> Object::ownKeys() const {
    std::vector<PropertyKey> out;
    std::vector<std::pair<int64_t, PropertyKey>> indexed;
    if (cls == ObjectClass::TypedArray)
        for (size_t i = 0, n = typedLength(this); i < n; ++i) out.emplace_back(indexKey(i));
    for (size_t i = 0; i < elements.size(); ++i)
        if (!elements[i].isEmpty()) out.emplace_back(indexKey(i));
    for (const auto& [k, p] : props_)
        if (!k.isSymbol())
            if (const int64_t i = arrayIndex(k.name); i >= 0) indexed.emplace_back(i, k);
    std::sort(indexed.begin(), indexed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [i, k] : indexed) out.push_back(k);
    if (cls == ObjectClass::String && primitive.isString()) {
        // A String object's characters come first.
        std::vector<PropertyKey> chars;
        for (size_t i = 0; i < primitive.asString().size(); ++i) chars.emplace_back(indexKey(i));
        out.insert(out.begin(), chars.begin(), chars.end());
    }
    for (const auto& [k, p] : props_)
        if (!k.isSymbol() && arrayIndex(k.name) < 0) out.push_back(k);
    for (const auto& [k, p] : props_)
        if (k.isSymbol()) out.push_back(k);
    return out;
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

class Interpreter {
public:
    explicit Interpreter(Runtime& rt, std::shared_ptr<Program> prog) : rt_(rt), prog_(std::move(prog)) {}

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
        declareLexical(global_env, p.root->lexical.get());
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
        declareLexical(env, p.root->lexical.get());
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
        declareLexical(lex, p.root->lexical.get());
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

private:
    struct EnvScope {
        Runtime& rt;
        EnvScope(Runtime& r, Env* e) : rt(r) { rt.env_stack.push_back(e); }
        ~EnvScope() { rt.env_stack.pop_back(); }
    };

    Env* newEnv(Env* outer) { return rt_.allocate<Env>(outer); }

    void declareLexical(Env* env, const std::vector<std::u16string>* names) {
        if (!names) return;
        for (const auto& n : *names) env->vars[n] = Binding{Value::undefined(), true, false};
    }

    // ── Names ──

    // The binding a name means here. An import is followed to the exporting module's
    // binding (`imported` says it was one: those can't be assigned).
    Binding* findBinding(Env* env, const std::u16string& name, Env** where, bool* imported = nullptr) {
        for (Env* e = env; e; e = e->outer) {
            const auto it = e->vars.find(name);
            if (it != e->vars.end()) {
                if (where) *where = e;
                Binding* b = &it->second;
                if (imported) *imported = b->import_env != nullptr;
                for (int hops = 0; b->import_env; ++hops) {
                    const auto jt = b->import_env->vars.find(b->import_name);
                    if (jt == b->import_env->vars.end() || hops > 100) rt_.throwError("ReferenceError", "the import '" + utf8(name) + "' isn't there");
                    b = &jt->second;
                }
                return b;
            }
        }
        return nullptr;
    }

    Object* globalObject(Env* env) {
        for (Env* e = env; e; e = e->outer)
            if (e->global_object) return e->global_object;
        return rt_.global();
    }

    Value lookup(Env* env, const std::u16string& name) {
        if (Binding* b = findBinding(env, name, nullptr)) {
            if (!b->initialized) rt_.throwError("ReferenceError", "can't use '" + utf8(name) + "' before it's declared");
            return b->value;
        }
        Object* g = globalObject(env);
        if (name == u"undefined") return Value::undefined();
        if (rt_.has(g, name)) return rt_.get(g, name, Value::object(g));
        rt_.throwError("ReferenceError", utf8(name) + " is not defined");
    }

    void assignName(Env* env, const std::u16string& name, const Value& v, bool strict) {
        bool imported = false;
        if (Binding* b = findBinding(env, name, nullptr, &imported)) {
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
        if (strict && !rt_.has(g, name)) rt_.throwError("ReferenceError", utf8(name) + " is not defined");
        rt_.set(g, name, v, Value::object(g));
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

    Value eval(Node* n, Env* env, bool strict);
    Value evalCall(Node* n, Env* env, bool strict);
    Value evalNew(Node* n, Env* env, bool strict);
    Value evalAssign(Node* n, Env* env, bool strict);
    Value evalUpdate(Node* n, Env* env, bool strict);
    Value evalUnary(Node* n, Env* env, bool strict);
    Value evalObject(Node* n, Env* env, bool strict);
    Value evalArray(Node* n, Env* env, bool strict);
    Value evalTemplate(Node* n, Env* env, bool strict);
    Value evalTagged(Node* n, Env* env, bool strict);
    std::vector<Value> evalArgs(const std::vector<Node*>& args, Env* env, bool strict);
    void assignPattern(Node* pattern, const Value& v, Env* env, bool strict);
    Value superBase(Env* env);
    PropertyKey memberKey(Node* m, Env* env, bool strict);

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
    Value runBody(FunctionInfo* info, Env* env);
    Value runInEnv(FunctionInfo* info, Env* env) {
        EnvScope scope(rt_, env);
        return runBody(info, env);
    }
  private:
    Value binaryOp(Op op, const Value& l, const Value& r);

    Runtime& rt_;
    std::shared_ptr<Program> prog_;
    friend class Runtime;
};

// ── The runtime ───────────────────────────────────────────────────────────────

Runtime::~Runtime() {
    // Suspended coroutines' frames unwind first, while everything they refer to is there.
    for (size_t i = 0; i < cells_.size(); ++i)
        if (auto* o = dynamic_cast<Object*>(cells_[i]); o && o->co) unwindCoroutine(*o->co);
    for (Cell* c : cells_) delete c;
}

Object* Runtime::newObject(Object* proto) { return allocate<Object>(proto ? proto : object_prototype); }

Object* Runtime::newArray(std::vector<Value> elements) {
    Object* a = allocate<Object>(array_prototype, ObjectClass::Array);
    a->elements = std::move(elements);
    return a;
}

Object* Runtime::newFunction(std::u16string name, int length, NativeFunction fn, bool constructor) {
    Object* f = allocate<Object>(function_prototype, ObjectClass::Function);
    f->native = std::move(fn);
    f->constructor = constructor;
    f->define(u"length", {Value::number(length), nullptr, nullptr, kConfigurable});
    f->define(u"name", {Value::string(std::move(name)), nullptr, nullptr, kConfigurable});
    return f;
}

Symbol* Runtime::newSymbol(std::u16string description, bool has) { return allocate<Symbol>(std::move(description), has); }

void Runtime::put(Object* o, const PropertyKey& key, Value v, uint8_t flags) { o->define(key, {std::move(v), nullptr, nullptr, flags}); }

void Runtime::method(Object* o, const char16_t* name, int length, NativeFunction fn) {
    put(o, name, Value::object(newFunction(name, length, std::move(fn))));
}

Object* Runtime::newError(const char* kind, const std::string& message) {
    const auto it = error_prototypes.find(kind);
    Object* e = allocate<Object>(it != error_prototypes.end() ? it->second : error_prototype, ObjectClass::Error);
    put(e, u"message", Value::string(utf16(message)));
    const std::u16string stack = utf16(std::string(kind) + ": " + message);
    put(e, u"stack", Value::string(stack));
    return e;
}

void Runtime::throwError(const char* kind, const std::string& message) { throw Thrown{Value::object(newError(kind, message))}; }

void Runtime::step() {
    if (++steps_ > step_budget_) throw Terminated{"the script ran too long"};
}

bool Runtime::isCallable(const Value& v) const {
    if (!v.isObject()) return false;
    const Object* o = v.asObject();
    return o->cls == ObjectClass::Function;
}

bool Runtime::isConstructor(const Value& v) const {
    return isCallable(v) && v.asObject()->constructor;
}

// ── Property access ──

namespace {

// A String object's (or a string's) characters, as properties.
bool stringIndex(const Value& prim, const PropertyKey& key, Value& out) {
    if (key.isSymbol() || !prim.isString()) return false;
    const int64_t i = arrayIndex(key.name);
    const auto& s = prim.asString();
    if (i >= 0 && static_cast<size_t>(i) < s.size()) {
        out = Value::string(std::u16string(1, s[static_cast<size_t>(i)]));
        return true;
    }
    if (key.name == u"length") {
        out = Value::number(static_cast<double>(s.size()));
        return true;
    }
    return false;
}

}  // namespace

Value Runtime::get(Object* o, const PropertyKey& key, const Value& receiver) {
    for (Object* cur = o; cur; cur = cur->prototype) {
        if (!key.isSymbol()) {
            if (cur->cls == ObjectClass::TypedArray) {
                double index;
                if (typedKey(key, index)) return typedGet(cur, index);
            }
            if (cur->cls == ObjectClass::Array || !cur->elements.empty()) {
                if (key.name == u"length" && cur->cls == ObjectClass::Array) return Value::number(static_cast<double>(cur->elements.size()));
                const int64_t i = arrayIndex(key.name);
                if (i >= 0 && static_cast<size_t>(i) < cur->elements.size() && !cur->elements[static_cast<size_t>(i)].isEmpty())
                    return cur->elements[static_cast<size_t>(i)];
            }
            if (cur->cls == ObjectClass::String) {
                Value v;
                if (stringIndex(cur->primitive, key, v)) return v;
            }
        }
        if (const Property* p = cur->own(key)) {
            if (p->flags & kAccessor) return p->getter ? call(Value::object(p->getter), receiver, {}) : Value::undefined();
            return p->value;
        }
    }
    return Value::undefined();
}

Value Runtime::get(const Value& base, const PropertyKey& key) {
    switch (base.type()) {
        case Value::Type::Object: return get(base.asObject(), key, base);
        case Value::Type::String: {
            Value v;
            if (stringIndex(base, key, v)) return v;
            return get(string_prototype, key, base);
        }
        case Value::Type::Number: return get(number_prototype, key, base);
        case Value::Type::Boolean: return get(boolean_prototype, key, base);
        case Value::Type::Symbol: return get(symbol_prototype, key, base);
        default:
            throwError("TypeError", "can't read property '" + (key.isSymbol() ? std::string("symbol") : utf8(key.name)) + "' of " +
                                        (base.isNull() ? "null" : "undefined"));
    }
}

bool Runtime::has(Object* o, const PropertyKey& key) {
    for (Object* cur = o; cur; cur = cur->prototype) {
        if (!key.isSymbol()) {
            if (cur->cls == ObjectClass::TypedArray) {
                double index;
                if (typedKey(key, index)) return typedValidIndex(cur, index);
            }
            if (cur->cls == ObjectClass::Array && key.name == u"length") return true;
            const int64_t i = arrayIndex(key.name);
            if (i >= 0 && static_cast<size_t>(i) < cur->elements.size() && !cur->elements[static_cast<size_t>(i)].isEmpty()) return true;
            if (cur->cls == ObjectClass::String) {
                Value v;
                if (stringIndex(cur->primitive, key, v)) return true;
            }
        }
        if (cur->own(key)) return true;
    }
    return false;
}

bool Runtime::set(Object* o, const PropertyKey& key, const Value& v, const Value& receiver) {
    // A typed array's elements: never up the chain, never a new property.
    if (!key.isSymbol() && o->cls == ObjectClass::TypedArray) {
        double index;
        if (typedKey(key, index)) {
            if (receiver.isObject() && receiver.asObject() == o) {
                typedSet(*this, o, index, v);
                return true;
            }
            if (!typedValidIndex(o, index)) return true;
        }
    }
    // An array's elements and length.
    if (!key.isSymbol() && o->cls == ObjectClass::Array && receiver.isObject() && receiver.asObject() == o) {
        if (key.name == u"length") {
            const double n = toNumber(v);
            if (n < 0 || n != std::floor(n) || n > 4294967295.0) throwError("RangeError", "invalid array length");
            if (n > 100'000'000) throwError("RangeError", "array too long");
            o->elements.resize(static_cast<size_t>(n), Value::empty());
            return true;
        }
        const int64_t i = arrayIndex(key.name);
        if (i >= 0 && !o->own(key)) {
            if (!o->extensible && static_cast<size_t>(i) >= o->elements.size()) return false;
            if (static_cast<size_t>(i) >= o->elements.size()) {
                if (i > 100'000'000) throwError("RangeError", "array too long");
                o->elements.resize(static_cast<size_t>(i) + 1, Value::empty());
            }
            o->elements[static_cast<size_t>(i)] = v;
            return true;
        }
    }
    // A setter or a read-only property up the chain decides.
    for (Object* cur = o; cur; cur = cur->prototype) {
        if (const Property* p = cur->own(key)) {
            if (p->flags & kAccessor) {
                if (!p->setter) return false;
                call(Value::object(p->setter), receiver, std::span<const Value>(&v, 1));
                return true;
            }
            if (!(p->flags & kWritable)) return false;
            break;
        }
        if (cur->cls == ObjectClass::String && !key.isSymbol()) {
            Value tmp;
            if (stringIndex(cur->primitive, key, tmp)) return false;
        }
    }
    if (!receiver.isObject()) return false;
    Object* r = receiver.asObject();
    if (Property* own = r->own(key)) {
        if (own->flags & kAccessor || !(own->flags & kWritable)) return false;
        own->value = v;
        return true;
    }
    if (!r->extensible) return false;
    // An index or length reached through the chain: the array's own fast path above.
    // (Any other name is an ordinary property: going round again would never end.)
    if (!key.isSymbol() && r->cls == ObjectClass::Array && r != o && (arrayIndex(key.name) >= 0 || key.name == u"length"))
        return set(r, key, v, receiver);
    r->define(key, {v, nullptr, nullptr, kDefaultFlags});
    return true;
}

void Runtime::set(const Value& base, const PropertyKey& key, const Value& v, bool strict) {
    bool ok;
    if (base.isObject()) {
        ok = set(base.asObject(), key, v, base);
    } else if (base.isNullish()) {
        throwError("TypeError", "can't set property '" + (key.isSymbol() ? std::string("symbol") : utf8(key.name)) + "' of " +
                                    (base.isNull() ? "null" : "undefined"));
    } else {
        Object* proto = base.isString() ? string_prototype : base.isNumber() ? number_prototype : base.isBoolean() ? boolean_prototype : symbol_prototype;
        ok = set(proto, key, v, base);
    }
    if (!ok && strict) throwError("TypeError", "can't assign to read-only property '" + (key.isSymbol() ? std::string("symbol") : utf8(key.name)) + "'");
}

bool Runtime::deleteProperty(Object* o, const PropertyKey& key) {
    if (!key.isSymbol() && o->cls == ObjectClass::TypedArray) {
        double index;
        if (typedKey(key, index)) return !typedValidIndex(o, index);
    }
    if (!key.isSymbol()) {
        const int64_t i = arrayIndex(key.name);
        if (i >= 0 && static_cast<size_t>(i) < o->elements.size()) {
            if (!o->extensible && o->cls == ObjectClass::Array && !o->elements[static_cast<size_t>(i)].isEmpty()) {
                // A frozen array keeps its elements.
                if (o->own(u"__frozen")) return false;
            }
            o->elements[static_cast<size_t>(i)] = Value::empty();
            return true;
        }
        if (o->cls == ObjectClass::Array && key.name == u"length") return false;
    }
    const Property* p = o->own(key);
    if (!p) return true;
    if (!(p->flags & kConfigurable)) return false;
    o->remove(key);
    return true;
}

uint64_t Runtime::lengthOf(Object* o) {
    if (o->cls == ObjectClass::Array) return o->elements.size();
    const double n = toIntegerOrInfinity(get(o, u"length", Value::object(o)));
    if (n <= 0) return 0;
    return static_cast<uint64_t>(std::min(n, 9007199254740991.0));
}

// ── Conversions ──

bool Runtime::toBoolean(const Value& v) const {
    switch (v.type()) {
        case Value::Type::Boolean: return v.asBoolean();
        case Value::Type::Number: return v.asNumber() != 0 && !std::isnan(v.asNumber());
        case Value::Type::String: return !v.asString().empty();
        case Value::Type::Object: case Value::Type::Symbol: return true;
        default: return false;
    }
}

double Runtime::stringToNumber(std::u16string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (isJsSpace(s[b]) || isLineTerminator(s[b]))) ++b;
    while (e > b && (isJsSpace(s[e - 1]) || isLineTerminator(s[e - 1]))) --e;
    s = s.substr(b, e - b);
    if (s.empty()) return 0;
    if (s.size() > 2 && s[0] == u'0') {
        const char16_t k = static_cast<char16_t>(s[1] | 0x20);
        const int base = k == u'x' ? 16 : k == u'o' ? 8 : k == u'b' ? 2 : 0;
        if (base) {
            double v = 0;
            for (size_t i = 2; i < s.size(); ++i) {
                const char16_t c = s[i];
                int d = (c >= u'0' && c <= u'9') ? c - u'0' : (c >= u'a' && c <= u'f') ? c - u'a' + 10 : (c >= u'A' && c <= u'F') ? c - u'A' + 10 : 99;
                if (d >= base) return std::nan("");
                v = v * base + d;
            }
            return v;
        }
    }
    std::string ascii;
    size_t i = 0;
    if (s[0] == u'+' || s[0] == u'-') ascii += static_cast<char>(s[i++]);
    if (s.substr(i) == u"Infinity") return ascii == "-" ? -INFINITY : INFINITY;
    bool digits = false;
    while (i < s.size() && s[i] >= u'0' && s[i] <= u'9') { ascii += static_cast<char>(s[i++]); digits = true; }
    if (i < s.size() && s[i] == u'.') {
        ascii += '.';
        ++i;
        while (i < s.size() && s[i] >= u'0' && s[i] <= u'9') { ascii += static_cast<char>(s[i++]); digits = true; }
    }
    if (!digits) return std::nan("");
    if (i < s.size() && (s[i] == u'e' || s[i] == u'E')) {
        ascii += 'e';
        ++i;
        if (i < s.size() && (s[i] == u'+' || s[i] == u'-')) ascii += static_cast<char>(s[i++]);
        bool exp = false;
        while (i < s.size() && s[i] >= u'0' && s[i] <= u'9') { ascii += static_cast<char>(s[i++]); exp = true; }
        if (!exp) return std::nan("");
    }
    if (i != s.size()) return std::nan("");
    return std::strtod(ascii.c_str(), nullptr);
}

std::u16string Runtime::numberToString(double d, int radix) {
    if (std::isnan(d)) return u"NaN";
    if (d == 0) return u"0";
    if (std::isinf(d)) return d < 0 ? u"-Infinity" : u"Infinity";
    if (d < 0) return u"-" + numberToString(-d, radix);
    if (radix != 10) {
        std::u16string out;
        double ip = std::floor(d), fp = d - ip;
        if (ip == 0) out = u"0";
        while (ip >= 1) {
            const int digit = static_cast<int>(std::fmod(ip, radix));
            out.insert(out.begin(), static_cast<char16_t>(digit < 10 ? u'0' + digit : u'a' + digit - 10));
            ip = std::floor(ip / radix);
        }
        if (fp > 0) {
            out += u'.';
            for (int k = 0; k < 52 && fp > 0; ++k) {
                fp *= radix;
                const int digit = static_cast<int>(fp);
                fp -= digit;
                out += static_cast<char16_t>(digit < 10 ? u'0' + digit : u'a' + digit - 10);
            }
        }
        return out;
    }
    // The shortest digits that read back as d, placed as Number::toString says.
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::scientific);
    std::string sci(buf, res.ptr);
    const size_t epos = sci.find('e');
    std::string mant = sci.substr(0, epos);
    const int exp = std::stoi(sci.substr(epos + 1));
    mant.erase(std::remove(mant.begin(), mant.end(), '.'), mant.end());
    const int k = static_cast<int>(mant.size());
    const int n = exp + 1;
    std::string out;
    if (k <= n && n <= 21) {
        out = mant + std::string(static_cast<size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        out = mant.substr(0, static_cast<size_t>(n)) + "." + mant.substr(static_cast<size_t>(n));
    } else if (-6 < n && n <= 0) {
        out = "0." + std::string(static_cast<size_t>(-n), '0') + mant;
    } else {
        const int e = n - 1;
        out = mant.substr(0, 1);
        if (k > 1) out += "." + mant.substr(1);
        out += e >= 0 ? "e+" : "e-";
        out += std::to_string(std::abs(e));
    }
    return utf16(out);
}

Value Runtime::toPrimitive(const Value& v, const char* hint) {
    if (!v.isObject()) return v;
    // Symbol.toPrimitive, then valueOf/toString in the hint's order.
    const Value exotic = get(v, PropertyKey(sym_to_primitive));
    if (!exotic.isNullish()) {
        if (!isCallable(exotic)) throwError("TypeError", "Symbol.toPrimitive isn't a function");
        const Value h = Value::string(utf16(hint));
        const Value r = call(exotic, v, std::span<const Value>(&h, 1));
        if (r.isObject()) throwError("TypeError", "Symbol.toPrimitive returned an object");
        return r;
    }
    const bool string_first = std::strcmp(hint, "string") == 0;
    for (int pass = 0; pass < 2; ++pass) {
        const char16_t* name = (pass == 0) == string_first ? u"toString" : u"valueOf";
        const Value fn = get(v, name);
        if (isCallable(fn)) {
            const Value r = call(fn, v, {});
            if (!r.isObject()) return r;
        }
    }
    throwError("TypeError", "can't convert an object to a primitive value");
}

double Runtime::toNumber(const Value& v) {
    switch (v.type()) {
        case Value::Type::Number: return v.asNumber();
        case Value::Type::Undefined: return std::nan("");
        case Value::Type::Null: return 0;
        case Value::Type::Boolean: return v.asBoolean() ? 1 : 0;
        case Value::Type::String: return stringToNumber(v.asString());
        case Value::Type::Symbol: throwError("TypeError", "can't convert a symbol to a number");
        case Value::Type::Object: return toNumber(toPrimitive(v, "number"));
        default: return std::nan("");
    }
}

double Runtime::toIntegerOrInfinity(const Value& v) {
    const double d = toNumber(v);
    if (std::isnan(d) || d == 0) return 0;
    if (std::isinf(d)) return d;
    return std::trunc(d);
}

int32_t Runtime::toInt32(const Value& v) { return static_cast<int32_t>(toUint32(v)); }

uint32_t Runtime::toUint32(const Value& v) {
    const double d = toNumber(v);
    if (std::isnan(d) || std::isinf(d) || d == 0) return 0;
    const double m = std::fmod(std::trunc(d), 4294967296.0);
    return static_cast<uint32_t>(static_cast<int64_t>(m < 0 ? m + 4294967296.0 : m));
}

JSString Runtime::toString(const Value& v) {
    switch (v.type()) {
        case Value::Type::String: return v.stringRef();
        case Value::Type::Number: return std::make_shared<const std::u16string>(numberToString(v.asNumber()));
        case Value::Type::Undefined: return std::make_shared<const std::u16string>(u"undefined");
        case Value::Type::Null: return std::make_shared<const std::u16string>(u"null");
        case Value::Type::Boolean: return std::make_shared<const std::u16string>(v.asBoolean() ? u"true" : u"false");
        case Value::Type::Symbol: throwError("TypeError", "can't convert a symbol to a string");
        case Value::Type::Object: return toString(toPrimitive(v, "string"));
        default: return std::make_shared<const std::u16string>();
    }
}

Object* Runtime::toObject(const Value& v) {
    switch (v.type()) {
        case Value::Type::Object: return v.asObject();
        case Value::Type::String: { Object* o = allocate<Object>(string_prototype, ObjectClass::String); o->primitive = v; return o; }
        case Value::Type::Number: { Object* o = allocate<Object>(number_prototype, ObjectClass::Number); o->primitive = v; return o; }
        case Value::Type::Boolean: { Object* o = allocate<Object>(boolean_prototype, ObjectClass::Boolean); o->primitive = v; return o; }
        case Value::Type::Symbol: { Object* o = allocate<Object>(symbol_prototype, ObjectClass::Symbol); o->primitive = v; return o; }
        default: throwError("TypeError", std::string("can't convert ") + (v.isNull() ? "null" : "undefined") + " to an object");
    }
}

PropertyKey Runtime::toPropertyKey(const Value& v) {
    if (v.isSymbol()) return PropertyKey(v.asSymbol());
    if (v.isNumber()) {
        const double d = v.asNumber();
        if (d >= 0 && d < 4294967295.0 && d == std::floor(d)) return PropertyKey(indexKey(static_cast<uint64_t>(d)));
    }
    if (v.isString()) return PropertyKey(v.asString());
    const Value p = toPrimitive(v, "string");
    if (p.isSymbol()) return PropertyKey(p.asSymbol());
    return PropertyKey(*toString(p));
}

bool Runtime::strictEquals(const Value& a, const Value& b) {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
        case Value::Type::Undefined: case Value::Type::Null: return true;
        case Value::Type::Number: return a.asNumber() == b.asNumber();
        case Value::Type::String: return a.asString() == b.asString();
        case Value::Type::Boolean: return a.asBoolean() == b.asBoolean();
        case Value::Type::Object: return a.asObject() == b.asObject();
        case Value::Type::Symbol: return a.asSymbol() == b.asSymbol();
        default: return false;
    }
}

bool Runtime::sameValueZero(const Value& a, const Value& b) {
    if (a.isNumber() && b.isNumber() && std::isnan(a.asNumber()) && std::isnan(b.asNumber())) return true;
    return strictEquals(a, b);
}

bool Runtime::sameValue(const Value& a, const Value& b) {
    if (a.isNumber() && b.isNumber()) {
        const double x = a.asNumber(), y = b.asNumber();
        if (std::isnan(x) && std::isnan(y)) return true;
        if (x == 0 && y == 0) return std::signbit(x) == std::signbit(y);
        return x == y;
    }
    return strictEquals(a, b);
}

bool Runtime::looseEquals(const Value& a, const Value& b) {
    if (a.type() == b.type()) return strictEquals(a, b);
    if (a.isNullish() && b.isNullish()) return true;
    if (a.isNullish() || b.isNullish()) return false;
    if (a.isNumber() && b.isString()) return a.asNumber() == stringToNumber(b.asString());
    if (a.isString() && b.isNumber()) return stringToNumber(a.asString()) == b.asNumber();
    if (a.isBoolean()) return looseEquals(Value::number(a.asBoolean() ? 1 : 0), b);
    if (b.isBoolean()) return looseEquals(a, Value::number(b.asBoolean() ? 1 : 0));
    if ((a.isNumber() || a.isString() || a.isSymbol()) && b.isObject()) return looseEquals(a, toPrimitive(b));
    if (a.isObject() && (b.isNumber() || b.isString() || b.isSymbol())) return looseEquals(toPrimitive(a), b);
    return false;
}

Value Runtime::typeOf(const Value& v) {
    const char16_t* t = u"undefined";
    switch (v.type()) {
        case Value::Type::Null: t = u"object"; break;
        case Value::Type::Boolean: t = u"boolean"; break;
        case Value::Type::Number: t = u"number"; break;
        case Value::Type::String: t = u"string"; break;
        case Value::Type::Symbol: t = u"symbol"; break;
        case Value::Type::Object: t = isCallable(v) ? u"function" : u"object"; break;
        default: break;
    }
    return Value::string(t);
}

// ── Calls ──

Value Runtime::call(const Value& fnv, const Value& this_value, std::span<const Value> args) {
    if (!isCallable(fnv)) throwError("TypeError", "not a function");
    Object* f = fnv.asObject();
    if (call_depth >= kMaxCallDepth || (current_co && static_cast<char*>(__builtin_frame_address(0)) < current_co->stack_limit))
        throwError("RangeError", "too much recursion");
    ++call_depth;
    struct Down { size_t& d; ~Down() { --d; } } down{call_depth};
    step();
    if (f->bound_target) {
        std::vector<Value> all = f->bound_args;
        all.insert(all.end(), args.begin(), args.end());
        return call(Value::object(f->bound_target), f->bound_this, all);
    }
    if (f->native) return f->native(*this, this_value, args);
    if (f->code) {
        if (f->class_constructor) throwError("TypeError", "a class constructor must be called with new");
        Interpreter interp(*this, f->code->program);
        return interp.callScript(f, this_value, args, nullptr);
    }
    throwError("TypeError", "not a function");
}

Value Runtime::construct(const Value& fnv, std::span<const Value> args, Object* new_target) {
    if (!isConstructor(fnv)) throwError("TypeError", "not a constructor");
    Object* f = fnv.asObject();
    if (!new_target) new_target = f;
    if (call_depth >= kMaxCallDepth) throwError("RangeError", "too much recursion");
    ++call_depth;
    struct Down { size_t& d; ~Down() { --d; } } down{call_depth};
    step();
    if (f->bound_target) {
        std::vector<Value> all = f->bound_args;
        all.insert(all.end(), args.begin(), args.end());
        return construct(Value::object(f->bound_target), all, new_target == f ? f->bound_target : new_target);
    }
    if (f->native) {
        // A built-in constructor knows it's constructing by construct_target.
        construct_target = new_target;
        struct Reset { Object*& t; ~Reset() { t = nullptr; } } reset{construct_target};
        return f->native(*this, Value::empty(), args);
    }
    Interpreter interp(*this, f->code->program);
    return interp.callScript(f, Value::undefined(), args, new_target);
}

// ── Iteration ──

Object* Runtime::createIterResult(const Value& value, bool done) {
    Object* r = newObject();
    put(r, u"value", value, kDefaultFlags);
    put(r, u"done", Value::boolean(done), kDefaultFlags);
    return r;
}

std::vector<Value> Runtime::iterateToList(const Value& iterable) {
    std::vector<Value> out;
    // Arrays with the ordinary iterator, and strings, directly.
    if (iterable.isString()) {
        const auto& s = iterable.asString();
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] >= 0xD800 && s[i] <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
                out.push_back(Value::string(std::u16string(s.substr(i, 2))));
                ++i;
            } else {
                out.push_back(Value::string(std::u16string(1, s[i])));
            }
        }
        return out;
    }
    const Value method = iterable.isNullish() ? Value::undefined() : get(iterable, PropertyKey(sym_iterator));
    if (!isCallable(method)) throwError("TypeError", "not iterable");
    if (iterable.isObject() && iterable.asObject()->cls == ObjectClass::Array && method.asObject() == array_values_) {
        for (const Value& v : iterable.asObject()->elements) out.push_back(v.isEmpty() ? Value::undefined() : v);
        return out;
    }
    const Value it = call(method, iterable, {});
    if (!it.isObject()) throwError("TypeError", "an iterator must be an object");
    const Value next = get(it, u"next");
    while (true) {
        step();
        const Value r = call(next, it, {});
        if (!r.isObject()) throwError("TypeError", "an iterator result must be an object");
        if (toBoolean(get(r, u"done"))) break;
        out.push_back(get(r, u"value"));
        if (out.size() > 50'000'000) throwError("RangeError", "too many values");
    }
    return out;
}

// ── Garbage collection ──

void Runtime::collectGarbage() {
    std::vector<Cell*> todo;
    const auto markValue = [&](const Value& v) {
        if (v.isObject() && v.asObject()) todo.push_back(v.asObject());
        else if (v.isSymbol()) todo.push_back(v.asSymbol());
    };
    const auto markCell = [&](Cell* c) {
        if (c) todo.push_back(c);
    };
    markCell(global_);
    markCell(global_env_);
    for (Env* e : env_stack) markCell(e);
    for (const Value& v : extra_roots) markValue(v);
    for (const auto* list : temp_lists)
        for (const Value& v : *list) markValue(v);
    for (const Job& j : jobs_) { markValue(j.a); markValue(j.b); markValue(j.c); }
    for (Object* p : pending_rejections) markCell(p);
    markModules(todo);
    for (Object* o : {object_prototype, function_prototype, array_prototype, string_prototype, number_prototype, boolean_prototype,
                      symbol_prototype, error_prototype, iterator_prototype, array_iterator_prototype, map_prototype, set_prototype, array_values_,
                      regexp_prototype, regexp_string_iterator_prototype, regexp_exec, regexp_constructor, promise_prototype,
                      promise_constructor, promise_then, eval_function, array_buffer_prototype, typed_array_prototype, data_view_prototype, generator_prototype, async_generator_prototype, async_iterator_prototype,
                      generator_function_prototype, async_function_prototype, async_generator_function_prototype,
                      async_from_sync_iterator_prototype})
        markCell(o);
    for (auto& [k, o] : error_prototypes) markCell(o);
    for (Object* c : typed_array_constructors) markCell(c);
    for (Symbol* s : {sym_iterator, sym_to_primitive, sym_to_string_tag, sym_has_instance, sym_match, sym_match_all, sym_replace,
                      sym_search, sym_split, sym_species, sym_async_iterator})
        markCell(s);
    for (auto& [k, s] : symbol_registry) markCell(s);
    // Suspended coroutines' stacks are scanned for anything that looks like a cell.
    std::vector<Cell*> sorted;
    std::vector<Object*> weak_collections, weak_refs;
    const auto markCoroutine = [&](Coroutine& co) {
        markCell(co.function);
        markCell(co.env);
        markCell(co.promise);
        markCell(co.on_fulfilled);
        markCell(co.on_rejected);
        markValue(co.transfer);
        for (const AsyncGenRequest& r : co.requests) { markValue(r.value); markCell(r.promise); }
        if (co.state != CoState::Suspended) return;
        for (Env* e : co.env_stack) markCell(e);
        for (const auto* list : co.temp_lists)
            for (const Value& v : *list) markValue(v);
        if (sorted.empty()) {
            sorted = cells_;
            std::sort(sorted.begin(), sorted.end());
        }
        scanCoroutineStack(co, [&](uintptr_t w) {
            auto it = std::upper_bound(sorted.begin(), sorted.end(), w, [](uintptr_t x, Cell* c) { return x < reinterpret_cast<uintptr_t>(c); });
            if (it == sorted.begin()) return;
            Cell* c = *--it;
            if (w < reinterpret_cast<uintptr_t>(c) + c->cellSize()) todo.push_back(c);
        });
        if (co.module) markModule(co.module, todo);
    };
    const auto drain = [&] {
    while (!todo.empty()) {
        Cell* c = todo.back();
        todo.pop_back();
        if (c->marked) continue;
        c->marked = true;
        if (auto* e = dynamic_cast<Env*>(c)) {
            markCell(e->outer);
            for (auto& [n, b] : e->vars) markValue(b.value);
            markValue(e->this_value);
            markCell(e->function);
            markCell(e->home);
            markCell(e->new_target);
            markCell(e->global_object);
            for (auto& [n, b] : e->vars) markCell(b.import_env);
        } else if (auto* o = dynamic_cast<Object*>(c)) {
            markCell(o->prototype);
            for (const auto& k : o->ownKeys()) {
                if (k.symbol) markCell(k.symbol);
                if (const Property* p = o->own(k)) {
                    markValue(p->value);
                    markCell(p->getter);
                    markCell(p->setter);
                }
            }
            for (const Value& v : o->elements) markValue(v);
            markCell(o->bound_target);
            markValue(o->bound_this);
            for (const Value& v : o->bound_args) markValue(v);
            if (o->cls == ObjectClass::WeakMap || o->cls == ObjectClass::WeakSet) {
                weak_collections.push_back(o);   // its entries live only as long as their keys
            } else if (o->cls == ObjectClass::WeakRef) {
                weak_refs.push_back(o);
            } else {
                markValue(o->primitive);
                for (auto& [k, v] : o->entries) { markValue(k); markValue(v); }
            }
            markCell(o->target);
            for (const Value& v : o->slots) markValue(v);
            if (o->code) {
                markCell(o->code->closure);
                markCell(o->code->home);
                markCell(o->code->class_fields_home);
            }
            if (o->co) markCoroutine(*o->co);
        }
    }
    };
    drain();
    // Ephemerons: a weak collection's value is live when its key is, which can make
    // more keys live; round again until nothing changes.
    const auto reached = [](const Value& v) {
        if (v.isObject()) return v.asObject()->marked;
        if (v.isSymbol()) return v.asSymbol()->marked;
        return true;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < weak_collections.size(); ++i) {
            for (const auto& [k, v] : weak_collections[i]->entries) {
                if (k.isEmpty() || !reached(k) || reached(v)) continue;
                markValue(v);
                changed = true;
            }
        }
        drain();
    }
    for (Object* m : weak_collections) {
        std::vector<std::pair<Value, Value>> kept;
        for (auto& e : m->entries)
            if (!e.first.isEmpty() && reached(e.first)) kept.push_back(std::move(e));
        m->entries = std::move(kept);
        m->index.clear();
        for (size_t i = 0; i < m->entries.size(); ++i) m->index.emplace(m->entries[i].first, i);
    }
    for (Object* r : weak_refs)
        if (!reached(r->primitive)) r->primitive = Value::undefined();
    // Coroutines nothing reaches unwind now, before what their frames refer to goes.
    // Unwinding runs no JavaScript and makes nothing new.
    for (size_t i = 0; i < cells_.size(); ++i) {
        auto* o = dynamic_cast<Object*>(cells_[i]);
        if (o && !o->marked && o->co && o->co->state == CoState::Suspended) unwindCoroutine(*o->co);
    }
    std::vector<Cell*> live;
    live.reserve(cells_.size());
    for (Cell* c : cells_) {
        if (c->marked) {
            c->marked = false;
            live.push_back(c);
        } else {
            delete c;
        }
    }
    cells_.swap(live);
    allocations_ = cells_.size();
}

// ── Evaluating scripts ────────────────────────────────────────────────────────

Runtime::Result Runtime::evaluate(std::string_view source, std::string_view name) {
    Result result;
    std::string error;
    std::shared_ptr<Program> prog(parse(utf16(source), error).release());
    if (!prog) {
        result.error = error;
        (void)name;
        return result;
    }
    // Nested (a script run from an event a script dispatched): the outer one's state stays.
    const size_t saved_envs = env_stack.size(), saved_calls = call_depth, saved_eval = eval_depth;
    if (saved_envs == 0 && saved_calls == 0) steps_ = 0;
    prog->url = std::string(name);
    programs_.push_back(prog);
    try {
        Interpreter interp(*this, prog);
        result.value = interp.runScript(global_env_);
        result.ok = true;
    } catch (const Thrown& t) {
        try {
            result.error = describe(*this, t.value);
        } catch (...) {
            result.error = "Uncaught exception";
        }
    } catch (const Terminated& t) {
        result.error = "Stopped: " + t.why;
    }
    env_stack.resize(saved_envs);
    call_depth = saved_calls;
    eval_depth = saved_eval;
    return result;
}

std::string describe(Runtime& rt, const Value& thrown) {
    if (thrown.isObject() && thrown.asObject()->cls == ObjectClass::Error) {
        const Value name = rt.get(thrown, u"name");
        const Value msg = rt.get(thrown, u"message");
        std::string out = name.isString() ? utf8(name.asString()) : "Error";
        if (msg.isString() && !msg.asString().empty()) out += ": " + utf8(msg.asString());
        return out;
    }
    if (thrown.isSymbol()) return "Uncaught symbol";
    return "Uncaught " + utf8(*rt.toString(thrown));
}

// ── The interpreter proper ────────────────────────────────────────────────────

Object* Interpreter::makeFunction(FunctionInfo* info, Env* env, Object* home) {
    Object* f = rt_.allocate<Object>(rt_.function_prototype, ObjectClass::Function);
    f->code = std::make_shared<FunctionData>();
    f->code->info = info;
    f->code->closure = env;
    f->code->home = home;
    f->code->program = prog_;
    f->constructor = !info->arrow && !info->is_method && !info->is_async && !info->is_generator;
    f->define(u"length", {Value::number(info->length), nullptr, nullptr, kConfigurable});
    f->define(u"name", {Value::string(info->name), nullptr, nullptr, kConfigurable});
    // A sloppy function's own caller and arguments: null, as browsers have them.
    if (!info->strict && !info->arrow && !info->is_method && !info->is_async && !info->is_generator) {
        f->define(u"caller", {Value::null(), nullptr, nullptr, 0});
        f->define(u"arguments", {Value::null(), nullptr, nullptr, 0});
    }
    if (f->constructor) {
        Object* proto = rt_.newObject();
        proto->define(u"constructor", {Value::object(f), nullptr, nullptr, kWritable | kConfigurable});
        f->define(u"prototype", {Value::object(proto), nullptr, nullptr, kWritable});
    } else if (info->is_generator) {
        f->prototype = info->is_async ? rt_.async_generator_function_prototype : rt_.generator_function_prototype;
        Object* proto = rt_.newObject(info->is_async ? rt_.async_generator_prototype : rt_.generator_prototype);
        f->define(u"prototype", {Value::object(proto), nullptr, nullptr, kWritable});
    } else if (info->is_async) {
        f->prototype = rt_.async_function_prototype;
    }
    return f;
}

// ── Iteration ──

namespace {

// An abrupt completion caught on its way out, to be rethrown after cleanup (closing an
// iterator, a finally block) that must not run inside a C++ catch handler: a coroutine
// can't be suspended while one of its frames is handling an exception.
struct Abrupt {
    uint8_t kind = 0;   // 1 thrown, 2 a generator's return
    Value value;
    explicit operator bool() const { return kind != 0; }
    [[noreturn]] void rethrow() const {
        if (kind == 1) throw Thrown{value};
        throw GeneratorReturn{value};
    }
};

}  // namespace

#define FBX_CATCH_ABRUPT(a)                                         \
    catch (const Thrown& thrown_) { (a).kind = 1; (a).value = thrown_.value; } \
    catch (const GeneratorReturn& ret_) { (a).kind = 2; (a).value = ret_.value; }

Interpreter::IterRec Interpreter::getIterator(const Value& v, bool async) {
    IterRec it;
    it.async = async;
    if (async) {
        const Value m = v.isNullish() ? Value::undefined() : rt_.get(v, PropertyKey(rt_.sym_async_iterator));
        if (!m.isNullish()) {
            if (!rt_.isCallable(m)) rt_.throwError("TypeError", "Symbol.asyncIterator isn't a function");
            it.iterator = rt_.call(m, v, {});
            if (!it.iterator.isObject()) rt_.throwError("TypeError", "an async iterator must be an object");
            it.next = rt_.get(it.iterator, u"next");
            return it;
        }
        it.async = false;
        it.sync_wrapped = true;
    }
    if (v.isString()) {
        // Strings by code point, unless String.prototype[Symbol.iterator] was replaced.
        const Value m = rt_.get(Value::object(rt_.string_prototype), PropertyKey(rt_.sym_iterator));
        if (m.isObject() && m.asObject()->native && m.asObject()->own(u"name") &&
            m.asObject()->own(u"name")->value.isString() && m.asObject()->own(u"name")->value.asString() == u"[Symbol.iterator]") {
            it.string = v;
            return it;
        }
    }
    const Value m = v.isNullish() ? Value::undefined() : rt_.get(v, PropertyKey(rt_.sym_iterator));
    if (!rt_.isCallable(m)) rt_.throwError("TypeError", "not iterable");
    if (v.isObject() && v.asObject()->cls == ObjectClass::Array && m.asObject() == rt_.array_values_) {
        it.array = v.asObject();
        return it;
    }
    it.iterator = rt_.call(m, v, {});
    if (!it.iterator.isObject()) rt_.throwError("TypeError", "an iterator must be an object");
    it.next = rt_.get(it.iterator, u"next");
    return it;
}

// The next value, or false at the end. A failure in next() leaves it done (not closed).
bool Interpreter::iterStep(IterRec& it, Value& out) {
    if (it.done) return false;
    rt_.step();
    if (it.array) {
        if (it.index >= it.array->elements.size()) {
            it.done = true;
            return false;
        }
        const Value& e = it.array->elements[it.index++];
        out = e.isEmpty() ? Value::undefined() : e;
        if (it.sync_wrapped) out = awaitValue(rt_, out);
        return true;
    }
    if (it.string.isString()) {
        const auto& str = it.string.asString();
        if (it.index >= str.size()) {
            it.done = true;
            return false;
        }
        size_t n = 1;
        if (str[it.index] >= 0xD800 && str[it.index] <= 0xDBFF && it.index + 1 < str.size() && str[it.index + 1] >= 0xDC00 && str[it.index + 1] <= 0xDFFF) n = 2;
        out = Value::string(str.substr(it.index, n));
        it.index += n;
        if (it.sync_wrapped) out = awaitValue(rt_, out);
        return true;
    }
    it.done = true;
    Value r = rt_.call(it.next, it.iterator, {});
    if (it.async) r = awaitValue(rt_, r);
    if (!r.isObject()) rt_.throwError("TypeError", "an iterator result must be an object");
    if (rt_.toBoolean(rt_.get(r, u"done"))) return false;
    out = rt_.get(r, u"value");
    if (it.sync_wrapped) out = awaitValue(rt_, out);
    it.done = false;
    return true;
}

// IteratorClose: return(), if it has one. `suppress` for when an error is already on its way.
void Interpreter::iterClose(IterRec& it, bool suppress) {
    if (it.done || it.array || it.string.isString()) return;
    it.done = true;
    Abrupt error;
    Value r;
    try {
        const Value ret = rt_.get(it.iterator, u"return");
        if (ret.isNullish()) return;
        r = rt_.call(ret, it.iterator, {});
        if (it.async) r = awaitValue(rt_, r);
    }
    FBX_CATCH_ABRUPT(error)
    if (suppress) return;
    if (error) error.rethrow();
    if (!r.isObject()) rt_.throwError("TypeError", "an iterator's return() must give an object");
}

template <typename F>
void Interpreter::destructureArray(Node* p, const Value& value, F&& assign) {
    IterRec it = getIterator(value, false);
    Abrupt failed;
    try {
        for (size_t i = 0; i < p->kids.size(); ++i) {
            Node* k = p->kids[i];
            if (k && k->kind == N::Rest) {
                std::vector<Value> rest;
                {
                    TempRoots roots(rt_, rest);
                    Value v;
                    while (iterStep(it, v)) rest.push_back(v);
                }
                assign(k->a, Value::object(rt_.newArray(std::move(rest))));
                break;
            }
            Value v = Value::undefined();
            Value got;
            if (iterStep(it, got)) v = got;
            if (k) assign(k, v);
        }
    }
    FBX_CATCH_ABRUPT(failed)
    if (failed) {
        iterClose(it, failed.kind == 1);
        failed.rethrow();
    }
    iterClose(it, false);
}

void Interpreter::bindPattern(Node* p, const Value& value, Env* env, bool strict, uint32_t kind) {
    switch (p->kind) {
        case N::Identifier: {
            if (kind == kVar) {
                assignName(env, p->str, value, strict);
            } else {
                Binding& b = env->vars[p->str];
                b.value = value;
                b.initialized = true;
                b.is_mutable = kind != kConst;
            }
            return;
        }
        case N::AssignPattern: {
            Value v = value;
            if (v.isUndefined()) v = eval(p->b, env, strict);
            bindPattern(p->a, v, env, strict, kind);
            return;
        }
        case N::ArrayPattern:
            destructureArray(p, value, [&](Node* target, const Value& v) { bindPattern(target, v, env, strict, kind); });
            return;
        case N::ObjectPattern: {
            if (value.isNullish()) rt_.throwError("TypeError", "can't destructure " + std::string(value.isNull() ? "null" : "undefined"));
            std::vector<PropertyKey> used;
            for (Node* k : p->kids) {
                if (k->kind == N::Rest) {
                    Object* rest = rt_.newObject();
                    Object* src = rt_.toObject(value);
                    for (const PropertyKey& key : src->ownKeys()) {
                        if (std::find(used.begin(), used.end(), key) != used.end()) continue;
                        const Property* prop = src->own(key);
                        if (prop && !(prop->flags & kEnumerable)) continue;
                        rt_.put(rest, key, rt_.get(src, key, value), kDefaultFlags);
                    }
                    bindPattern(k->a, Value::object(rest), env, strict, kind);
                    break;
                }
                const PropertyKey key = (k->flags & kComputed) ? rt_.toPropertyKey(eval(k->a, env, strict)) : PropertyKey(k->str);
                used.push_back(key);
                bindPattern(k->b, rt_.get(value, key), env, strict, kind);
            }
            return;
        }
        default:
            // A member expression, in an assignment pattern.
            assignPattern(p, value, env, strict);
    }
}

void Interpreter::assignPattern(Node* p, const Value& value, Env* env, bool strict) {
    switch (p->kind) {
        case N::Identifier: assignName(env, p->str, value, strict); return;
        case N::Member: {
            const Value base = p->a->kind == N::Super ? thisValue(env) : eval(p->a, env, strict);
            const PropertyKey key = memberKey(p, env, strict);
            if (p->a->kind == N::Super) {
                rt_.set(superBase(env).asObject(), key, value, base);
                return;
            }
            rt_.set(base, key, value, strict);
            return;
        }
        case N::AssignPattern: {
            Value v = value;
            if (v.isUndefined()) v = eval(p->b, env, strict);
            assignPattern(p->a, v, env, strict);
            return;
        }
        case N::ArrayPattern: {
            // As destructuring assignment orders it: a member target's reference is
            // worked out before the iterator is asked for its value.
            IterRec it = getIterator(value, false);
            Abrupt failed;
            try {
                for (Node* k : p->kids) {
                    Node* target = k;
                    Node* dflt = nullptr;
                    const bool rest = k && k->kind == N::Rest;
                    if (rest) target = k->a;
                    else if (k && k->kind == N::AssignPattern) { target = k->a; dflt = k->b; }
                    const bool member = target && target->kind == N::Member;
                    Value base;
                    PropertyKey key;
                    if (member) {
                        base = target->a->kind == N::Super ? thisValue(env) : eval(target->a, env, strict);
                        key = memberKey(target, env, strict);
                    }
                    Value v;
                    if (rest) {
                        std::vector<Value> items;
                        {
                            TempRoots roots(rt_, items);
                            Value x;
                            while (iterStep(it, x)) items.push_back(x);
                        }
                        v = Value::object(rt_.newArray(std::move(items)));
                    } else {
                        Value got;
                        if (iterStep(it, got)) v = got;
                    }
                    if (!target) continue;
                    if (dflt && v.isUndefined()) v = eval(dflt, env, strict);
                    if (member) {
                        if (target->a->kind == N::Super) rt_.set(superBase(env).asObject(), key, v, base);
                        else rt_.set(base, key, v, strict);
                    } else {
                        assignPattern(target, v, env, strict);
                    }
                    if (rest) break;
                }
            }
            FBX_CATCH_ABRUPT(failed)
            if (failed) {
                iterClose(it, failed.kind == 1);
                failed.rethrow();
            }
            iterClose(it, false);
            return;
        }
        case N::ObjectPattern: {
            if (value.isNullish()) rt_.throwError("TypeError", "can't destructure " + std::string(value.isNull() ? "null" : "undefined"));
            std::vector<PropertyKey> used;
            for (Node* k : p->kids) {
                if (k->kind == N::Rest) {
                    Object* rest = rt_.newObject();
                    Object* src = rt_.toObject(value);
                    for (const PropertyKey& key : src->ownKeys()) {
                        if (std::find(used.begin(), used.end(), key) != used.end()) continue;
                        rt_.put(rest, key, rt_.get(src, key, value), kDefaultFlags);
                    }
                    assignPattern(k->a, Value::object(rest), env, strict);
                    break;
                }
                const PropertyKey key = (k->flags & kComputed) ? rt_.toPropertyKey(eval(k->a, env, strict)) : PropertyKey(k->str);
                used.push_back(key);
                assignPattern(k->b, rt_.get(value, key), env, strict);
            }
            return;
        }
        default:
            rt_.throwError("SyntaxError", "invalid assignment target");
    }
}

Value Interpreter::callScript(Object* f, const Value& this_value, std::span<const Value> args, Object* new_target) {
    FunctionInfo* info = f->code->info;
    if (info->is_async || info->is_generator) {
        // The parameters are bound now; the body runs on a coroutine of its own. An
        // async function's failure here rejects its promise rather than throwing.
        Env* env = nullptr;
        if (info->is_async && !info->is_generator) {
            Abrupt failed;
            try {
                env = prepareCall(f, this_value, args, nullptr);
            }
            FBX_CATCH_ABRUPT(failed)
            if (failed) return rejectedPromise(rt_, failed.value);
        } else {
            env = prepareCall(f, this_value, args, nullptr);
        }
        return startCoroutineFunction(rt_, f, env);
    }
    Env* env = prepareCall(f, this_value, args, new_target);
    EnvScope scope(rt_, env);
    const Value result = runBody(info, env);
    if (new_target) {
        if (result.isObject()) return result;
        if (info->derived && !result.isUndefined()) rt_.throwError("TypeError", "a derived constructor may only return an object or undefined");
        if (!env->this_initialized) rt_.throwError("ReferenceError", "a derived constructor must call super()");
        return env->this_value;
    }
    return result;
}

Value Interpreter::runBody(FunctionInfo* info, Env* env) {
    if (info->expression_body) return eval(info->body, env, info->strict);
    declareLexical(env, info->body->lexical.get());
    Completion c = execBlock(info->body->kids, env, info->strict);
    return c.type == CT::Return ? c.value : Value::undefined();
}

Value evalCode(Runtime& rt, const std::u16string& source, Env* env, bool strict, bool direct) {
    Interpreter probe(rt, nullptr);
    EvalContext ctx;
    if (direct) ctx = probe.evalContextOf(env);
    else {
        env = rt.global_env_;
        strict = false;
    }
    // Parsed once per source (and context): eval in a loop doesn't parse again.
    std::u16string key = source;
    key += strict ? u"\x01s" : u"\x01l";
    key += ctx.new_target ? u'n' : u'-';
    key += ctx.super_property ? u'p' : u'-';
    key += ctx.super_call ? u'c' : u'-';
    std::shared_ptr<Program> prog;
    if (const auto it = rt.eval_cache_.find(key); it != rt.eval_cache_.end()) {
        prog = it->second;
    } else {
        std::string error;
        prog.reset(parse(source, error, strict, false, &ctx).release());
        if (!prog) {
            const std::string prefix = "SyntaxError: ";
            rt.throwError("SyntaxError", error.rfind(prefix, 0) == 0 ? error.substr(prefix.size()) : error);
        }
        prog->url = "eval";
        if (rt.eval_cache_.size() > 512) rt.eval_cache_.clear();
        rt.eval_cache_[key] = prog;
    }
    const bool code_strict = strict || prog->top->strict;
    Interpreter interp(rt, prog);
    Env* lex = rt.allocate<Env>(env);
    if (code_strict) return interp.runEval(lex, lex, nullptr, true);
    Env* var_env = direct ? interp.varScopeOf(env) : rt.global_env_;
    Object* var_object = var_env && var_env->global_object ? var_env->global_object : nullptr;
    if (!var_env) {
        var_env = rt.global_env_;
        var_object = rt.global();
    }
    return interp.runEval(lex, var_env, var_object, false);
}

Value dynamicFunction(Runtime& rt, std::span<const Value> args, const char* kind) {
    std::u16string params;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (i) params += u",";
        params += *rt.toString(args[i]);
    }
    const std::u16string body = args.empty() ? u"" : *rt.toString(args.back());
    const std::u16string source = u"(" + std::u16string(utf16(kind)) + u" anonymous(" + params + u"\n) {\n" + body + u"\n})";
    // It must be one function and nothing else: parameters or a body that close it
    // early (`}); more code; ({`) are a syntax error, not code that runs.
    {
        std::string error;
        std::unique_ptr<Program> p = parse(source, error);
        if (!p) rt.throwError("SyntaxError", error.rfind("SyntaxError: ", 0) == 0 ? error.substr(13) : error);
        if (p->root->kids.size() != 1 || p->root->kids[0]->kind != N::ExprStmt || p->root->kids[0]->a->kind != N::FunctionExpr)
            rt.throwError("SyntaxError", "the parameters or body of a function made from source close it early");
    }
    return evalCode(rt, source, nullptr, false, false);
}

void instantiateModuleScope(Runtime& rt, const std::shared_ptr<Program>& prog, Env* env) {
    Interpreter interp(rt, prog);
    interp.instantiateModule(env);
}

void runModuleStatements(Runtime& rt, const std::shared_ptr<Program>& prog, Env* env) {
    Interpreter interp(rt, prog);
    interp.runModule(env);
}

Value runFunctionBody(Runtime& rt, Object* f, Env* env) {
    Interpreter interp(rt, f->code->program);
    return interp.runInEnv(f->code->info, env);
}

Env* Interpreter::prepareCall(Object* f, const Value& this_value, std::span<const Value> args, Object* new_target) {
    FunctionData& code = *f->code;
    FunctionInfo* info = code.info;
    const bool strict = info->strict;
    Env* env = newEnv(code.closure);
    if (!info->arrow) {
        env->is_function = true;
        env->function = f;
        env->home = code.home;
        env->new_target = new_target;
        if (new_target) {
            if (info->derived) {
                env->this_initialized = false;
            } else {
                // The new object, from new.target's prototype.
                const Value proto = rt_.get(Value::object(new_target), u"prototype");
                Object* obj = rt_.newObject(proto.isObject() ? proto.asObject() : rt_.object_prototype);
                env->this_value = Value::object(obj);
            }
        } else if (strict) {
            env->this_value = this_value;
        } else if (this_value.isNullish() || this_value.isEmpty()) {
            env->this_value = Value::object(rt_.global());
        } else {
            env->this_value = Value::object(rt_.toObject(this_value));
        }
    }
    EnvScope scope(rt_, env);   // while the parameters' defaults run
    // Parameters.
    if (info->simple_params) {
        for (size_t i = 0; i < info->params.size(); ++i) env->vars[info->params[i]->str] = Binding{i < args.size() ? args[i] : Value::undefined()};
    }
    if (info->uses_arguments && !info->arrow) {
        Object* a = rt_.allocate<Object>(rt_.object_prototype, ObjectClass::Arguments);
        for (size_t i = 0; i < args.size(); ++i) a->define(PropertyKey(utf16(std::to_string(i))), {args[i]});
        a->define(u"length", {Value::number(static_cast<double>(args.size())), nullptr, nullptr, kWritable | kConfigurable});
        a->define(PropertyKey(rt_.sym_iterator), {rt_.get(Value::object(rt_.array_prototype), u"values"), nullptr, nullptr, kWritable | kConfigurable});
        if (!env->vars.count(u"arguments")) env->vars[u"arguments"] = Binding{Value::object(a)};
    }
    if (!info->simple_params) {
        // Every parameter is in its dead zone until its turn: `(a = b, b) => …` throws.
        std::vector<Node*> todo(info->params.begin(), info->params.end());
        while (!todo.empty()) {
            Node* p = todo.back();
            todo.pop_back();
            if (!p) continue;
            switch (p->kind) {
                case N::Identifier: env->vars[p->str] = Binding{Value::undefined(), true, false}; break;
                case N::AssignPattern: case N::Rest: todo.push_back(p->a); break;
                case N::ArrayPattern: todo.insert(todo.end(), p->kids.begin(), p->kids.end()); break;
                case N::ObjectPattern: for (Node* k : p->kids) todo.push_back(k->kind == N::PatternProperty ? k->b : k->a); break;
                default: break;
            }
        }
        for (size_t i = 0; i < info->params.size(); ++i) {
            Node* p = info->params[i];
            if (p->kind == N::Rest) {
                std::vector<Value> rest;
                for (size_t j = i; j < args.size(); ++j) rest.push_back(args[j]);
                bindPattern(p->a, Value::object(rt_.newArray(std::move(rest))), env, strict, kLet);
                break;
            }
            bindPattern(p, i < args.size() ? args[i] : Value::undefined(), env, strict, kLet);
        }
    }
    // Hoisting: var names (not over parameters), then function declarations.
    for (const auto& name : info->var_names)
        if (!env->vars.count(name)) env->vars[name] = Binding{};
    for (Node* d : info->function_decls) env->vars[d->str] = Binding{Value::object(makeFunction(d->fn, env, nullptr))};
    if (new_target && info->is_class_constructor && !info->derived) {
        Object* obj = env->this_value.asObject();
        for (Node* field : info->fields) {
            Env* fenv = newEnv(env);
            const PropertyKey key = (field->flags & kComputed) ? rt_.toPropertyKey(eval(field->a, env, true)) : PropertyKey(field->str);
            const Value v = field->b ? eval(field->b, fenv, true) : Value::undefined();
            obj->define(key, {v});
        }
    }
    return env;
}

// ── Statements ──

// How deep evaluation may nest in C++ before it's an error rather than a crash.
constexpr size_t kMaxEvalDepth = 2500;

struct EvalDepth {
    Runtime& rt;
    explicit EvalDepth(Runtime& r) : rt(r) {
        if (++rt.eval_depth > kMaxEvalDepth ||
            (rt.current_co && static_cast<char*>(__builtin_frame_address(0)) < rt.current_co->stack_limit)) {
            --rt.eval_depth;
            rt.throwError("RangeError", "too much recursion");
        }
    }
    ~EvalDepth() { --rt.eval_depth; }
};

void Interpreter::hoistBlockFunctions(const std::vector<Node*>& stmts, Env* env) {
    for (Node* s : stmts) {
        if (s->kind != N::FunctionDecl) continue;
        Object* fn = makeFunction(s->fn, env, nullptr);
        env->vars[s->str] = Binding{Value::object(fn)};
    }
}

Completion Interpreter::execBlock(const std::vector<Node*>& stmts, Env* env, bool strict) {
    Completion last;
    last.value = Value::empty();
    for (Node* s : stmts) {
        Completion c = exec(s, env, strict);
        if (c.type != CT::Normal) {
            if (c.value.isEmpty()) c.value = last.value;
            return c;
        }
        if (!c.value.isEmpty()) last.value = c.value;
    }
    return last;
}

void Interpreter::declare(Node* decl, Env* env, bool strict) {
    const uint32_t kind = decl->flags & (kVar | kLet | kConst);
    for (Node* d : decl->kids) {
        if (kind == kVar) {
            if (!d->b) continue;
            const Value v = eval(d->b, env, strict);
            bindPattern(d->a, v, env, strict, kVar);
        } else {
            const Value v = d->b ? eval(d->b, env, strict) : Value::undefined();
            bindPattern(d->a, v, env, strict, kind);
        }
    }
}

Completion Interpreter::exec(Node* n, Env* env, bool strict) {
    rt_.step();
    EvalDepth depth(rt_);
    switch (n->kind) {
        case N::ExprStmt: return {CT::Normal, eval(n->a, env, strict)};
        case N::VarDecl: declare(n, env, strict); return {CT::Normal, Value::empty()};
        case N::FunctionDecl: {
            // Hoisted already at its function's start; a block's at the block's start.
            // Annex B: the name is also the enclosing function's var.
            Env* where = nullptr;
            if (strict) return {CT::Normal, Value::empty()};
            if (Binding* b = findBinding(env, n->str, &where)) {
                Env* fenv = functionEnv(env);
                if (fenv && fenv != where && fenv->vars.count(n->str)) fenv->vars[n->str].value = b->value;
                else if (fenv && fenv->global_object && fenv != where) rt_.set(fenv->global_object, n->str, b->value, Value::object(fenv->global_object));
            }
            return {CT::Normal, Value::empty()};
        }
        case N::ClassDecl: {
            Object* c = classDefinition(n, env);
            Binding& b = env->vars[n->str];
            b.value = Value::object(c);
            b.initialized = true;
            return {CT::Normal, Value::empty()};
        }
        case N::Return: return {CT::Return, n->a ? eval(n->a, env, strict) : Value::undefined()};
        case N::If: {
            if (rt_.toBoolean(eval(n->a, env, strict))) return exec(n->b, env, strict);
            if (n->c) return exec(n->c, env, strict);
            return {CT::Normal, Value::undefined()};
        }
        case N::Block: {
            if (!n->lexical && std::none_of(n->kids.begin(), n->kids.end(), [](Node* k) { return k->kind == N::FunctionDecl; }))
                return execBlock(n->kids, env, strict);
            Env* block = newEnv(env);
            declareLexical(block, n->lexical.get());
            hoistBlockFunctions(n->kids, block);
            EnvScope scope(rt_, block);
            return execBlock(n->kids, block, strict);
        }
        case N::Empty: case N::Debugger: return {CT::Normal, Value::empty()};
        case N::Throw: throw Thrown{eval(n->a, env, strict)};
        case N::Try: return tryStatement(n, env, strict);
        case N::Break: return {CT::Break, Value::empty(), n->str.empty() ? nullptr : &n->str};
        case N::Continue: return {CT::Continue, Value::empty(), n->str.empty() ? nullptr : &n->str};
        case N::Labeled: {
            Node* body = n->a;
            Completion c;
            if (body->kind == N::For) c = forStatement(body, env, strict, &n->str);
            else if (body->kind == N::ForIn || body->kind == N::ForOf) c = forInOf(body, env, strict, &n->str);
            else if (body->kind == N::Switch) c = switchStatement(body, env, strict, &n->str);
            else if (body->kind == N::While || body->kind == N::DoWhile) {
                // Labeled while loops go through the general path with the label.
                bool stop = false;
                Value last;
                if (body->kind == N::DoWhile) {
                    do {
                        Completion bc = execLoopBody(body->a, env, strict, &n->str, stop, last);
                        if (stop) return bc;
                    } while (rt_.toBoolean(eval(body->b, env, strict)));
                } else {
                    while (rt_.toBoolean(eval(body->a, env, strict))) {
                        Completion bc = execLoopBody(body->b, env, strict, &n->str, stop, last);
                        if (stop) return bc;
                    }
                }
                return {CT::Normal, last};
            } else c = exec(body, env, strict);
            if (c.type == CT::Break && c.label && *c.label == n->str) return {CT::Normal, c.value};
            return c;
        }
        case N::While: {
            bool stop = false;
            Value last;
            while (rt_.toBoolean(eval(n->a, env, strict))) {
                Completion c = execLoopBody(n->b, env, strict, nullptr, stop, last);
                if (stop) return c;
            }
            return {CT::Normal, last};
        }
        case N::DoWhile: {
            bool stop = false;
            Value last;
            do {
                Completion c = execLoopBody(n->a, env, strict, nullptr, stop, last);
                if (stop) return c;
            } while (rt_.toBoolean(eval(n->b, env, strict)));
            return {CT::Normal, last};
        }
        case N::For: return forStatement(n, env, strict, nullptr);
        case N::ForIn: case N::ForOf: return forInOf(n, env, strict, nullptr);
        case N::Switch: return switchStatement(n, env, strict, nullptr);
        case N::ImportDecl: return {CT::Normal, Value::empty()};   // bound when the module was linked
        case N::ExportDecl: {
            if (!n->a) return {CT::Normal, Value::empty()};
            if (n->a->kind == N::VarDecl || n->a->kind == N::FunctionDecl || n->a->kind == N::ClassDecl) return exec(n->a, env, strict);
            if (n->flags & kHoisted) return {CT::Normal, Value::empty()};   // made when the module was linked
            // export default <expression>
            const Value v = eval(n->a, env, strict);
            Binding& b = env->vars[u"*default*"];
            b.value = v;
            b.initialized = true;
            return {CT::Normal, Value::empty()};
        }
        default: rt_.throwError("SyntaxError", "unexpected statement");
    }
}

// A loop body's completion: `stop` when the loop should end (with that completion).
Completion Interpreter::execLoopBody(Node* body, Env* env, bool strict, const std::u16string* label, bool& stop, Value& last) {
    Completion c = exec(body, env, strict);
    if (!c.value.isEmpty()) last = c.value;
    stop = false;
    if (c.type == CT::Break) {
        stop = true;
        if (!c.label || (label && *c.label == *label)) return {CT::Normal, last};
        return c;
    }
    if (c.type == CT::Continue) {
        if (!c.label || (label && *c.label == *label)) return {CT::Normal, last};
        stop = true;
        return c;
    }
    if (c.type == CT::Return) {
        stop = true;
        return c;
    }
    return c;
}

Completion Interpreter::forStatement(Node* n, Env* env, bool strict, const std::u16string* label) {
    Env* loop = env;
    const bool per_iteration = n->a && n->a->kind == N::VarDecl && (n->a->flags & (kLet | kConst));
    if (n->lexical) {
        loop = newEnv(env);
        declareLexical(loop, n->lexical.get());
    }
    EnvScope scope(rt_, loop);
    if (n->a) {
        if (n->a->kind == N::VarDecl) declare(n->a, loop, strict);
        else eval(n->a, loop, strict);
    }
    Value last;
    bool stop = false;
    while (true) {
        // let: each iteration its own copy, so closures see that iteration's value.
        Env* iter = loop;
        if (per_iteration) {
            iter = newEnv(env);
            iter->vars = loop->vars;
        }
        if (n->b && !rt_.toBoolean(eval(n->b, iter, strict))) break;
        Completion c = execLoopBody(n->d, iter, strict, label, stop, last);
        if (stop) return c;
        if (per_iteration) {
            Env* next = newEnv(env);
            next->vars = iter->vars;
            loop = next;
        }
        if (n->c) eval(n->c, loop, strict);
    }
    return {CT::Normal, last};
}

Completion Interpreter::forInOf(Node* n, Env* env, bool strict, const std::u16string* label) {
    const uint32_t decl = n->flags & (kVar | kLet | kConst);
    // The right side sees the loop's names in their dead zone.
    Env* tdz = env;
    if (decl & (kLet | kConst)) {
        tdz = newEnv(env);
        std::vector<std::u16string> names;
        std::vector<Node*> todo{n->a};
        while (!todo.empty()) {
            Node* p = todo.back();
            todo.pop_back();
            if (!p) continue;
            if (p->kind == N::Identifier) tdz->vars[p->str] = Binding{Value::undefined(), true, false};
            else if (p->kind == N::AssignPattern || p->kind == N::Rest) todo.push_back(p->a);
            else if (p->kind == N::ArrayPattern) todo.insert(todo.end(), p->kids.begin(), p->kids.end());
            else if (p->kind == N::ObjectPattern) for (Node* k : p->kids) todo.push_back(k->kind == N::PatternProperty ? k->b : k->a);
        }
    }
    const Value subject = eval(n->b, tdz, strict);
    if (n->kind == N::ForOf) return forOf(n, env, strict, label, subject, decl);
    std::vector<Value> items;
    if (n->kind == N::ForIn) {
        if (subject.isNullish()) return {CT::Normal, Value::undefined()};
        // Enumerable string keys, own then inherited, each name once.
        Object* o = rt_.toObject(subject);
        std::vector<PropertyKey> seen;
        for (Object* cur = o; cur; cur = cur->prototype) {
            for (const PropertyKey& k : cur->ownKeys()) {
                if (k.isSymbol() || std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
                seen.push_back(k);
                const Property* p = cur->own(k);
                if (p && !(p->flags & kEnumerable)) continue;
                items.push_back(Value::string(k.name));
            }
        }
    }
    Value last;
    bool stop = false;
    for (const Value& item : items) {
        Env* iter = env;
        if (decl & (kLet | kConst)) {
            iter = newEnv(env);
            bindPattern(n->a, item, iter, strict, decl);
        } else if (decl == kVar) {
            bindPattern(n->a, item, env, strict, kVar);
        } else {
            assignPattern(n->a, item, env, strict);
        }
        EnvScope scope(rt_, iter);
        Completion c = execLoopBody(n->c, iter, strict, label, stop, last);
        if (stop) return c;
    }
    return {CT::Normal, last};
}

// for (… of …) and for await (… of …): one value at a time, and the iterator closed
// when the loop is left early.
Completion Interpreter::forOf(Node* n, Env* env, bool strict, const std::u16string* label, const Value& subject, uint32_t decl) {
    IterRec it = getIterator(subject, (n->flags & kAwait) != 0);
    Value last;
    bool stop = false;
    while (true) {
        Value item;
        if (!iterStep(it, item)) break;
        Completion c;
        Abrupt abrupt;
        try {
            Env* iter = env;
            if (decl & (kLet | kConst)) {
                iter = newEnv(env);
                bindPattern(n->a, item, iter, strict, decl);
            } else if (decl == kVar) {
                bindPattern(n->a, item, env, strict, kVar);
            } else {
                assignPattern(n->a, item, env, strict);
            }
            EnvScope scope(rt_, iter);
            c = execLoopBody(n->c, iter, strict, label, stop, last);
        }
        FBX_CATCH_ABRUPT(abrupt)
        if (abrupt) {
            iterClose(it, abrupt.kind == 1);
            abrupt.rethrow();
        }
        if (stop) {
            iterClose(it, false);
            return c;
        }
    }
    return {CT::Normal, last};
}

Completion Interpreter::tryStatement(Node* n, Env* env, bool strict) {
    // The catch and finally blocks run after the C++ handler has finished: a yield or
    // await in them suspends the coroutine, which mustn't happen mid-handler.
    Completion result;
    Abrupt abrupt;
    try {
        result = exec(n->a, env, strict);
    }
    FBX_CATCH_ABRUPT(abrupt)
    if (abrupt.kind == 1 && n->c) {
        const Value thrown = abrupt.value;
        abrupt = Abrupt{};
        try {
            Env* catch_env = newEnv(env);
            EnvScope scope(rt_, catch_env);
            if (n->b) bindPattern(n->b, thrown, catch_env, strict, kLet);
            result = exec(n->c, catch_env, strict);
        }
        FBX_CATCH_ABRUPT(abrupt)
    }
    if (n->d) {
        Completion f = exec(n->d, env, strict);
        if (f.type != CT::Normal) return f;   // finally's own break, continue or return wins
    }
    if (abrupt) abrupt.rethrow();
    return result;
}

Completion Interpreter::switchStatement(Node* n, Env* env, bool strict, const std::u16string* label) {
    const Value d = eval(n->a, env, strict);
    Env* block = newEnv(env);
    declareLexical(block, n->lexical.get());
    for (Node* c : n->kids) hoistBlockFunctions(c->kids, block);
    EnvScope scope(rt_, block);
    size_t start = n->kids.size();
    for (size_t i = 0; i < n->kids.size() && start == n->kids.size(); ++i)
        if (n->kids[i]->a && Runtime::strictEquals(d, eval(n->kids[i]->a, block, strict))) start = i;
    if (start == n->kids.size())
        for (size_t i = 0; i < n->kids.size(); ++i)
            if (!n->kids[i]->a) start = i;
    Value last;
    for (size_t i = start; i < n->kids.size(); ++i) {
        Completion c = execBlock(n->kids[i]->kids, block, strict);
        if (!c.value.isEmpty()) last = c.value;
        if (c.type == CT::Break && (!c.label || (label && *c.label == *label))) return {CT::Normal, last};
        if (c.type != CT::Normal) return c;
    }
    return {CT::Normal, last};
}

Object* Interpreter::classDefinition(Node* n, Env* env) {
    Env* class_env = newEnv(env);
    const std::u16string& name = n->str.empty() ? n->fn->name : n->str;
    if (!n->str.empty()) class_env->vars[n->str] = Binding{Value::undefined(), false, false};
    Object* parent_proto = rt_.object_prototype;
    Object* parent_ctor = rt_.function_prototype;
    if (n->a) {
        const Value super_class = eval(n->a, class_env, true);
        if (super_class.isNull()) {
            parent_proto = nullptr;
        } else {
            if (!rt_.isConstructor(super_class)) rt_.throwError("TypeError", "a class can only extend a constructor or null");
            parent_ctor = super_class.asObject();
            const Value pp = rt_.get(super_class, u"prototype");
            if (!pp.isObject() && !pp.isNull()) rt_.throwError("TypeError", "the superclass's prototype must be an object or null");
            parent_proto = pp.isObject() ? pp.asObject() : nullptr;
        }
    }
    Object* proto = rt_.allocate<Object>(parent_proto);
    Object* ctor = makeFunction(n->fn, class_env, proto);
    ctor->prototype = parent_ctor;
    ctor->class_constructor = true;
    ctor->constructor = true;
    ctor->derived = n->a != nullptr;
    ctor->define(u"prototype", {Value::object(proto), nullptr, nullptr, 0});
    ctor->define(u"name", {Value::string(name), nullptr, nullptr, kConfigurable});
    proto->define(u"constructor", {Value::object(ctor), nullptr, nullptr, kWritable | kConfigurable});
    for (Node* m : n->kids) {
        Object* target = (m->flags & kStatic) ? ctor : proto;
        const PropertyKey key = (m->flags & kComputed) ? rt_.toPropertyKey(eval(m->a, class_env, true)) : PropertyKey(m->str);
        if (m->flags & kField) {
            if (m->flags & kStatic) {
                Env* fenv = newEnv(class_env);
                fenv->is_function = true;
                fenv->this_value = Value::object(ctor);
                fenv->home = ctor;
                const Value v = m->b ? eval(m->b, fenv, true) : Value::undefined();
                ctor->define(key, {v});
            }
            continue;
        }
        Object* fn = makeFunction(m->fn, class_env, target);
        if (m->flags & (kGetter | kSetter)) {
            Property* existing = target->own(key);
            Property p = existing && (existing->flags & kAccessor) ? *existing : Property{Value::undefined(), nullptr, nullptr, kConfigurable | kAccessor};
            if (m->flags & kGetter) p.getter = fn;
            else p.setter = fn;
            target->define(key, p);
        } else {
            target->define(key, {Value::object(fn), nullptr, nullptr, kWritable | kConfigurable});
        }
    }
    if (!n->str.empty()) {
        class_env->vars[n->str].value = Value::object(ctor);
        class_env->vars[n->str].initialized = true;
    }
    return ctor;
}

// ── Expressions ──

PropertyKey Interpreter::memberKey(Node* m, Env* env, bool strict) {
    if (m->flags & kComputed) return rt_.toPropertyKey(eval(m->b, env, strict));
    return PropertyKey(m->str);
}

Value Interpreter::superBase(Env* env) {
    Env* f = functionEnv(env);
    // Arrows look through to their enclosing method.
    while (f && !f->home && f->outer) f = functionEnv(f->outer);
    if (!f || !f->home) rt_.throwError("SyntaxError", "super isn't allowed here");
    return Value::object(f->home->prototype);
}

std::vector<Value> Interpreter::evalArgs(const std::vector<Node*>& args, Env* env, bool strict) {
    std::vector<Value> out;
    out.reserve(args.size());
    TempRoots roots(rt_, out);
    for (Node* a : args) {
        if (a->kind == N::Spread) {
            const std::vector<Value> items = rt_.iterateToList(eval(a->a, env, strict));
            out.insert(out.end(), items.begin(), items.end());
        } else {
            out.push_back(eval(a, env, strict));
        }
    }
    return out;
}

Value Interpreter::eval(Node* n, Env* env, bool strict) {
    EvalDepth depth(rt_);
    switch (n->kind) {
        case N::Number: return Value::number(n->number);
        case N::String: return Value::string(n->str);
        case N::Boolean: return Value::boolean(n->flags != 0);
        case N::Null: return Value::null();
        case N::Identifier: return lookup(env, n->str);
        case N::This: return thisValue(env);
        case N::Template: return evalTemplate(n, env, strict);
        case N::TaggedTemplate: return evalTagged(n, env, strict);
        case N::Regex: return Value::object(rt_.newRegExp(n->str, n->str2));
        case N::Array: return evalArray(n, env, strict);
        case N::Object: return evalObject(n, env, strict);
        case N::FunctionExpr: {
            if (n->str.empty()) return Value::object(makeFunction(n->fn, env, nullptr));
            // A named function expression sees its own name.
            Env* named = newEnv(env);
            Object* f = makeFunction(n->fn, named, nullptr);
            Binding self{Value::object(f), false, true};
            self.function_name = true;
            named->vars[n->str] = self;
            return Value::object(f);
        }
        case N::Arrow: return Value::object(makeFunction(n->fn, env, nullptr));
        case N::ClassExpr: return Value::object(classDefinition(n, env));
        case N::Unary: return evalUnary(n, env, strict);
        case N::Update: return evalUpdate(n, env, strict);
        case N::Assign: return evalAssign(n, env, strict);
        case N::Conditional: return rt_.toBoolean(eval(n->a, env, strict)) ? eval(n->b, env, strict) : eval(n->c, env, strict);
        case N::Sequence: {
            Value v;
            for (Node* k : n->kids) v = eval(k, env, strict);
            return v;
        }
        case N::Logical: {
            const Value l = eval(n->a, env, strict);
            if (n->op == Op::And) return rt_.toBoolean(l) ? eval(n->b, env, strict) : l;
            if (n->op == Op::Or) return rt_.toBoolean(l) ? l : eval(n->b, env, strict);
            return l.isNullish() ? eval(n->b, env, strict) : l;
        }
        case N::Binary: {
            // A long left-leaning chain (a + b + c + ...) in a loop, not by recursion.
            std::vector<Node*> spine;
            Node* left = n;
            while (left->kind == N::Binary && spine.size() < 1'000'000) {
                spine.push_back(left);
                left = left->a;
            }
            Value acc = eval(left, env, strict);
            for (auto it = spine.rbegin(); it != spine.rend(); ++it) {
                const Value r = eval((*it)->b, env, strict);
                acc = binaryOp((*it)->op, acc, r);
            }
            return acc;
        }
        case N::Member: {
            if (n->a->kind == N::Super) return rt_.get(superBase(env).asObject(), memberKey(n, env, strict), thisValue(env));
            const Value base = eval(n->a, env, strict);
            if ((n->flags & kOptional) && base.isNullish()) throw ShortCircuit{};
            return rt_.get(base, memberKey(n, env, strict));
        }
        case N::OptionalChain: {
            try {
                return eval(n->a, env, strict);
            } catch (const ShortCircuit&) {
                return Value::undefined();
            }
        }
        case N::Call: return evalCall(n, env, strict);
        case N::New: return evalNew(n, env, strict);
        case N::NewTarget: {
            Env* f = functionEnv(env);
            return f && f->new_target ? Value::object(f->new_target) : Value::undefined();
        }
        case N::Yield: {
            const Value v = n->a ? eval(n->a, env, strict) : Value::undefined();
            if (n->flags & kSpreadProp) return generatorDelegate(rt_, v);
            return generatorYield(rt_, v);
        }
        case N::Await: return awaitValue(rt_, eval(n->a, env, strict));
        case N::ImportCall: {
            const Value spec = eval(n->a, env, strict);
            return dynamicImport(rt_, spec, prog_->url);
        }
        case N::ImportMeta: return Value::object(importMeta(rt_, prog_->url));
        case N::Spread: rt_.throwError("SyntaxError", "unexpected ...");
        default: rt_.throwError("SyntaxError", "unexpected expression");
    }
}

namespace {

// Abstract relational comparison: true, false, or undefined (NaN) as -1.
int lessThan(Runtime& rt, const Value& a, const Value& b, bool left_first) {
    Value pa, pb;
    if (left_first) {
        pa = rt.toPrimitive(a, "number");
        pb = rt.toPrimitive(b, "number");
    } else {
        pb = rt.toPrimitive(b, "number");
        pa = rt.toPrimitive(a, "number");
    }
    if (pa.isString() && pb.isString()) return pa.asString() < pb.asString() ? 1 : 0;
    const double x = rt.toNumber(pa), y = rt.toNumber(pb);
    if (std::isnan(x) || std::isnan(y)) return -1;
    return x < y ? 1 : 0;
}

double jsPow(double x, double y) {
    if (std::isnan(y)) return std::nan("");
    if (y == 0) return 1;
    if ((x == 1 || x == -1) && std::isinf(y)) return std::nan("");
    return std::pow(x, y);
}

}  // namespace

Value Interpreter::binaryOp(Op op, const Value& l, const Value& r) {
    switch (op) {
        case Op::Add: {
            if (l.isNumber() && r.isNumber()) return Value::number(l.asNumber() + r.asNumber());
            if (l.isString() && r.isString()) {
                if (l.asString().size() + r.asString().size() > (1u << 29)) rt_.throwError("RangeError", "string too long");
                return Value::string(l.asString() + r.asString());
            }
            const Value pl = rt_.toPrimitive(l), pr = rt_.toPrimitive(r);
            if (pl.isString() || pr.isString()) {
                const JSString a = rt_.toString(pl), b = rt_.toString(pr);
                if (a->size() + b->size() > (1u << 29)) rt_.throwError("RangeError", "string too long");
                return Value::string(*a + *b);
            }
            return Value::number(rt_.toNumber(pl) + rt_.toNumber(pr));
        }
        case Op::Sub: return Value::number(rt_.toNumber(l) - rt_.toNumber(r));
        case Op::Mul: return Value::number(rt_.toNumber(l) * rt_.toNumber(r));
        case Op::Div: return Value::number(rt_.toNumber(l) / rt_.toNumber(r));
        case Op::Mod: {
            const double a = rt_.toNumber(l), b = rt_.toNumber(r);
            return Value::number(std::fmod(a, b));
        }
        case Op::Exp: return Value::number(jsPow(rt_.toNumber(l), rt_.toNumber(r)));
        case Op::Shl: return Value::number(static_cast<int32_t>(static_cast<uint32_t>(rt_.toInt32(l)) << (rt_.toUint32(r) & 31)));
        case Op::Shr: return Value::number(rt_.toInt32(l) >> (rt_.toUint32(r) & 31));
        case Op::UShr: return Value::number(rt_.toUint32(l) >> (rt_.toUint32(r) & 31));
        case Op::BitAnd: return Value::number(rt_.toInt32(l) & rt_.toInt32(r));
        case Op::BitOr: return Value::number(rt_.toInt32(l) | rt_.toInt32(r));
        case Op::BitXor: return Value::number(rt_.toInt32(l) ^ rt_.toInt32(r));
        case Op::Eq: return Value::boolean(rt_.looseEquals(l, r));
        case Op::Ne: return Value::boolean(!rt_.looseEquals(l, r));
        case Op::StrictEq: return Value::boolean(Runtime::strictEquals(l, r));
        case Op::StrictNe: return Value::boolean(!Runtime::strictEquals(l, r));
        case Op::Lt: return Value::boolean(lessThan(rt_, l, r, true) == 1);
        case Op::Gt: return Value::boolean(lessThan(rt_, r, l, false) == 1);
        case Op::Le: { const int c = lessThan(rt_, r, l, false); return Value::boolean(c == 0); }
        case Op::Ge: { const int c = lessThan(rt_, l, r, true); return Value::boolean(c == 0); }
        case Op::In: {
            if (!r.isObject()) rt_.throwError("TypeError", "'in' needs an object on its right");
            return Value::boolean(rt_.has(r.asObject(), rt_.toPropertyKey(l)));
        }
        case Op::InstanceOf: {
            if (!r.isObject()) rt_.throwError("TypeError", "the right side of instanceof isn't an object");
            const Value custom = rt_.get(r, PropertyKey(rt_.sym_has_instance));
            if (!custom.isNullish()) return Value::boolean(rt_.toBoolean(rt_.call(custom, r, std::span<const Value>(&l, 1))));
            if (!rt_.isCallable(r)) rt_.throwError("TypeError", "the right side of instanceof isn't callable");
            Object* f = r.asObject();
            while (f->bound_target) f = f->bound_target;
            if (!l.isObject()) return Value::boolean(false);
            const Value proto = rt_.get(Value::object(f), u"prototype");
            if (!proto.isObject()) rt_.throwError("TypeError", "the function's prototype isn't an object");
            for (Object* o = l.asObject()->prototype; o; o = o->prototype)
                if (o == proto.asObject()) return Value::boolean(true);
            return Value::boolean(false);
        }
        default: rt_.throwError("SyntaxError", "unknown operator");
    }
}

Value Interpreter::evalTemplate(Node* n, Env* env, bool strict) {
    std::u16string out;
    for (size_t i = 0; i < n->kids.size(); ++i) {
        if (i % 2 == 0) out += n->kids[i]->str;
        else {
            const Value v = eval(n->kids[i], env, strict);
            if (v.isSymbol()) rt_.throwError("TypeError", "can't convert a symbol to a string");
            out += *rt_.toString(v);
        }
    }
    return Value::string(std::move(out));
}

Value Interpreter::evalTagged(Node* n, Env* env, bool strict) {
    Value this_value;
    Value fn;
    std::vector<Value> args;
    TempRoots roots(rt_, args);
    if (n->a->kind == N::Member) {
        this_value = eval(n->a->a, env, strict);
        fn = rt_.get(this_value, memberKey(n->a, env, strict));
    } else {
        fn = eval(n->a, env, strict);
    }
    Node* t = n->b;
    std::vector<Value> cooked, raw;
    for (size_t i = 0; i < t->kids.size(); i += 2) {
        cooked.push_back((t->kids[i]->flags & kOptional) ? Value::undefined() : Value::string(t->kids[i]->str));
        raw.push_back(Value::string(t->kids[i]->str2));
    }
    Object* strings = rt_.newArray(std::move(cooked));
    Object* raw_arr = rt_.newArray(std::move(raw));
    strings->define(u"raw", {Value::object(raw_arr), nullptr, nullptr, 0});
    args.push_back(Value::object(strings));
    for (size_t i = 1; i < t->kids.size(); i += 2) args.push_back(eval(t->kids[i], env, strict));
    return rt_.call(fn, this_value, args);
}

Value Interpreter::evalArray(Node* n, Env* env, bool strict) {
    std::vector<Value> items;
    items.reserve(n->kids.size());
    TempRoots roots(rt_, items);
    for (Node* k : n->kids) {
        if (k->kind == N::Hole) items.push_back(Value::empty());
        else if (k->kind == N::Spread) {
            const std::vector<Value> spread = rt_.iterateToList(eval(k->a, env, strict));
            items.insert(items.end(), spread.begin(), spread.end());
        } else {
            items.push_back(eval(k, env, strict));
        }
    }
    return Value::object(rt_.newArray(std::move(items)));
}

Value Interpreter::evalObject(Node* n, Env* env, bool strict) {
    Object* o = rt_.newObject();
    for (Node* p : n->kids) {
        if (p->flags & kSpreadProp) {
            const Value src = eval(p->b, env, strict);
            if (src.isNullish()) continue;
            Object* so = rt_.toObject(src);
            for (const PropertyKey& k : so->ownKeys()) {
                const Property* prop = so->own(k);
                if (prop && !(prop->flags & kEnumerable)) continue;
                o->define(k, {rt_.get(so, k, src)});
            }
            continue;
        }
        const PropertyKey key = (p->flags & kComputed) ? rt_.toPropertyKey(eval(p->a, env, strict)) : PropertyKey(p->str);
        if (p->flags & (kGetter | kSetter)) {
            Object* fn = makeFunction(p->fn, env, o);
            Property* existing = o->own(key);
            Property prop = existing && (existing->flags & kAccessor) ? *existing : Property{Value::undefined(), nullptr, nullptr, kEnumerable | kConfigurable | kAccessor};
            if (p->flags & kGetter) prop.getter = fn;
            else prop.setter = fn;
            o->define(key, prop);
            continue;
        }
        if (p->flags & kMethodProp) {
            o->define(key, {Value::object(makeFunction(p->fn, env, o))});
            continue;
        }
        if (!(p->flags & kComputed) && p->str == u"__proto__" && !(p->flags & kShorthand)) {
            const Value v = eval(p->b, env, strict);
            if (v.isObject()) o->prototype = v.asObject();
            else if (v.isNull()) o->prototype = nullptr;
            continue;
        }
        Value v = eval(p->b, env, strict);
        if (v.isObject() && v.asObject()->code && (p->flags & kComputed)) {
            // `[key]: function () {}` takes the key as its name.
            Property* nm = v.asObject()->own(u"name");
            if (nm && nm->value.isString() && nm->value.asString().empty())
                nm->value = Value::string(key.isSymbol() ? u"[" + key.symbol->description + u"]" : key.name);
        }
        o->define(key, {v});
    }
    return Value::object(o);
}

Value Interpreter::evalCall(Node* n, Env* env, bool strict) {
    Node* callee = n->a;
    Value this_value = Value::undefined();
    Value fn;
    if (callee->kind == N::Super) {
        // super(...): the parent constructor makes `this`.
        Env* f = functionEnv(env);
        while (f && !f->function && f->outer) f = functionEnv(f->outer);
        if (!f || !f->function || !f->function->derived) rt_.throwError("SyntaxError", "super() is only allowed in a derived class's constructor");
        if (f->this_initialized) rt_.throwError("ReferenceError", "super() was already called");
        Object* parent = f->function->prototype;
        const std::vector<Value> args = evalArgs(n->kids, env, strict);
        const Value result = rt_.construct(Value::object(parent), args, f->new_target);
        f->this_value = result;
        f->this_initialized = true;
        // This class's own fields, now there's an object for them.
        FunctionInfo* info = f->function->code->info;
        Object* obj = result.asObject();
        for (Node* field : info->fields) {
            Env* fenv = newEnv(f);
            const PropertyKey key = (field->flags & kComputed) ? rt_.toPropertyKey(eval(field->a, f, true)) : PropertyKey(field->str);
            const Value v = field->b ? eval(field->b, fenv, true) : Value::undefined();
            obj->define(key, {v});
        }
        return result;
    }
    if (callee->kind == N::Member) {
        if (callee->a->kind == N::Super) {
            this_value = thisValue(env);
            fn = rt_.get(superBase(env).asObject(), memberKey(callee, env, strict), this_value);
        } else {
            this_value = eval(callee->a, env, strict);
            if ((callee->flags & kOptional) && this_value.isNullish()) throw ShortCircuit{};
            fn = rt_.get(this_value, memberKey(callee, env, strict));
        }
    } else if (callee->kind == N::OptionalChain && callee->a->kind == N::Member && !(callee->flags & kParenthesized)) {
        try {
            Node* m = callee->a;
            this_value = eval(m->a, env, strict);
            if ((m->flags & kOptional) && this_value.isNullish()) throw ShortCircuit{};
            fn = rt_.get(this_value, memberKey(m, env, strict));
        } catch (const ShortCircuit&) {
            return Value::undefined();
        }
    } else {
        fn = eval(callee, env, strict);
    }
    if ((n->flags & kOptional) && fn.isNullish()) throw ShortCircuit{};
    const std::vector<Value> args = evalArgs(n->kids, env, strict);
    // eval(...) by that name, meaning the real eval: code in this very scope.
    if (callee->kind == N::Identifier && callee->str == u"eval" && fn.isObject() && fn.asObject() == rt_.eval_function) {
        if (args.empty() || !args[0].isString()) return args.empty() ? Value::undefined() : args[0];
        return evalCode(rt_, args[0].asString(), env, strict, true);
    }
    if (!rt_.isCallable(fn)) {
        std::string what = callee->kind == N::Identifier ? utf8(callee->str)
                           : callee->kind == N::Member && !(callee->flags & kComputed) ? utf8(callee->str) : std::string("expression");
        rt_.throwError("TypeError", what + " is not a function");
    }
    return rt_.call(fn, this_value, args);
}

Value Interpreter::evalNew(Node* n, Env* env, bool strict) {
    const Value fn = eval(n->a, env, strict);
    const std::vector<Value> args = evalArgs(n->kids, env, strict);
    if (!rt_.isConstructor(fn)) {
        std::string what = n->a->kind == N::Identifier ? utf8(n->a->str) : n->a->kind == N::Member && !(n->a->flags & kComputed) ? utf8(n->a->str) : "expression";
        rt_.throwError("TypeError", what + " is not a constructor");
    }
    return rt_.construct(fn, args);
}

Value Interpreter::evalUnary(Node* n, Env* env, bool strict) {
    switch (n->op) {
        case Op::TypeOf: {
            if (n->a->kind == N::Identifier) {
                // typeof an undeclared name is "undefined", not an error.
                if (!findBinding(env, n->a->str, nullptr) && !rt_.has(globalObject(env), n->a->str)) return Value::string(u"undefined");
            }
            return rt_.typeOf(eval(n->a, env, strict));
        }
        case Op::Delete: {
            Node* t = n->a;
            if (t->kind == N::OptionalChain) t = t->a;
            if (t->kind == N::Member) {
                if (t->a->kind == N::Super) rt_.throwError("ReferenceError", "can't delete a super property");
                const Value base = eval(t->a, env, strict);
                if ((t->flags & kOptional) && base.isNullish()) return Value::boolean(true);
                const PropertyKey key = memberKey(t, env, strict);
                Object* o = rt_.toObject(base);
                const bool ok = rt_.deleteProperty(o, key);
                if (!ok && strict) rt_.throwError("TypeError", "can't delete property '" + (key.isSymbol() ? std::string("symbol") : utf8(key.name)) + "'");
                return Value::boolean(ok);
            }
            if (t->kind == N::Identifier) {
                if (findBinding(env, t->str, nullptr)) return Value::boolean(false);
                Object* g = globalObject(env);
                return Value::boolean(rt_.deleteProperty(g, t->str));
            }
            eval(t, env, strict);
            return Value::boolean(true);
        }
        case Op::Void: eval(n->a, env, strict); return Value::undefined();
        case Op::Not: return Value::boolean(!rt_.toBoolean(eval(n->a, env, strict)));
        case Op::Neg: return Value::number(-rt_.toNumber(eval(n->a, env, strict)));
        case Op::Plus: return Value::number(rt_.toNumber(eval(n->a, env, strict)));
        case Op::BitNot: return Value::number(~rt_.toInt32(eval(n->a, env, strict)));
        default: rt_.throwError("SyntaxError", "unknown unary operator");
    }
}

Value Interpreter::evalUpdate(Node* n, Env* env, bool strict) {
    Node* t = n->a;
    const double delta = n->op == Op::Inc ? 1 : -1;
    if (t->kind == N::Identifier) {
        const double old = rt_.toNumber(lookup(env, t->str));
        assignName(env, t->str, Value::number(old + delta), strict);
        return Value::number((n->flags & kPrefix) ? old + delta : old);
    }
    const Value base = t->a->kind == N::Super ? thisValue(env) : eval(t->a, env, strict);
    const PropertyKey key = memberKey(t, env, strict);
    const double old = rt_.toNumber(t->a->kind == N::Super ? rt_.get(superBase(env).asObject(), key, base) : rt_.get(base, key));
    rt_.set(base, key, Value::number(old + delta), strict);
    return Value::number((n->flags & kPrefix) ? old + delta : old);
}

Value Interpreter::evalAssign(Node* n, Env* env, bool strict) {
    Node* t = n->a;
    if (n->op == Op::None) {
        if (t->kind == N::Identifier) {
            // Resolve the name first: a TDZ name errs before the value is computed.
            const Value v = eval(n->b, env, strict);
            assignName(env, t->str, v, strict);
            return v;
        }
        if (t->kind == N::Member) {
            const Value base = t->a->kind == N::Super ? thisValue(env) : eval(t->a, env, strict);
            const PropertyKey key = memberKey(t, env, strict);
            const Value v = eval(n->b, env, strict);
            if (t->a->kind == N::Super) {
                rt_.set(superBase(env).asObject(), key, v, base);
                return v;
            }
            rt_.set(base, key, v, strict);
            return v;
        }
        const Value v = eval(n->b, env, strict);
        assignPattern(t, v, env, strict);
        return v;
    }
    // Compound: read, operate, write. &&=, ||= and ??= only write when they must.
    Value base;
    PropertyKey key;
    Value old;
    if (t->kind == N::Identifier) {
        old = lookup(env, t->str);
    } else {
        base = eval(t->a, env, strict);
        key = memberKey(t, env, strict);
        old = rt_.get(base, key);
    }
    const auto write = [&](const Value& v) {
        if (t->kind == N::Identifier) assignName(env, t->str, v, strict);
        else rt_.set(base, key, v, strict);
        return v;
    };
    if (n->op == Op::And) return rt_.toBoolean(old) ? write(eval(n->b, env, strict)) : old;
    if (n->op == Op::Or) return rt_.toBoolean(old) ? old : write(eval(n->b, env, strict));
    if (n->op == Op::Nullish) return old.isNullish() ? write(eval(n->b, env, strict)) : old;
    const Value r = eval(n->b, env, strict);
    return write(binaryOp(n->op, old, r));
}

}  // namespace fbx::xweb::js
