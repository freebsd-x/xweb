#pragma once

// What the interpreter and the built-ins share. Internal to the engine.

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ast.h"
#include "xweb/js.h"

namespace fbx::xweb::js {

struct Binding {
    Value value;
    bool is_mutable = true;
    bool initialized = true;
    bool function_name = false;   // a named function expression's own name: sloppy writes are ignored
    // A module's import: a live view of `import_name` in another module's scope.
    Env* import_env = nullptr;
    std::u16string import_name;
};

// A scope's names. Most scopes hold a handful, found by a scan of stored hashes; a
// scope that grows past that gets an open-addressing index. Entries never move once made: a Binding* stays good while
// its scope lives, whatever is declared after it. A name is hashed once per lookup,
// however many scopes the lookup walks (find with the hash).
class VarMap {
public:
    struct Entry {
        std::u16string first;
        Binding second;
    };
    struct Slot {
        Entry e;
        size_t h = 0;
    };

    [[nodiscard]] static size_t hashOf(std::u16string_view s) noexcept {
        size_t h = 1469598103934665603ull;
        for (const char16_t c : s) h = (h ^ c) * 1099511628211ull;
        return h;
    }

    VarMap() = default;
    VarMap(const VarMap& o) { *this = o; }
    VarMap& operator=(const VarMap& o) {
        if (this == &o) return *this;
        slots_.clear();
        index_.clear();
        slots_.reserve(o.slots_.size());
        for (const auto& s : o.slots_) add(s->e.first, s->h).second = s->e.second;
        return *this;
    }

    [[nodiscard]] Entry* find(const std::u16string& name, size_t h) noexcept {
        if (index_.empty()) {
            for (const auto& s : slots_)
                if (s->h == h && s->e.first == name) return &s->e;
            return nullptr;
        }
        const size_t mask = index_.size() - 1;
        for (size_t i = h & mask;; i = (i + 1) & mask) {
            Slot* s = index_[i];
            if (!s) return nullptr;
            if (s->h == h && s->e.first == name) return &s->e;
        }
    }
    [[nodiscard]] Entry* find(const std::u16string& name) noexcept { return find(name, hashOf(name)); }
    [[nodiscard]] size_t count(const std::u16string& name) noexcept { return find(name) ? 1 : 0; }

    Binding& operator[](const std::u16string& name) {
        const size_t h = hashOf(name);
        if (Entry* e = find(name, h)) return e->second;
        return add(name, h).second;
    }

    [[nodiscard]] size_t size() const noexcept { return slots_.size(); }
    void reserve(size_t n) { slots_.reserve(n); }

    class iterator {
    public:
        explicit iterator(const std::unique_ptr<Slot>* p) : p_(p) {}
        Entry& operator*() const { return (*p_)->e; }
        iterator& operator++() { ++p_; return *this; }
        bool operator!=(const iterator& o) const { return p_ != o.p_; }
    private:
        const std::unique_ptr<Slot>* p_;
    };
    iterator begin() { return iterator(slots_.data()); }
    iterator end() { return iterator(slots_.data() + slots_.size()); }

private:
    static constexpr size_t kScanMax = 8;   // beyond this, keep an index

    Entry& add(const std::u16string& name, size_t h) {
        slots_.push_back(std::make_unique<Slot>());
        Slot* s = slots_.back().get();
        s->e.first = name;
        s->h = h;
        if (slots_.size() > kScanMax) {
            if (index_.size() < slots_.size() * 2) {
                size_t cap = 32;
                while (cap < slots_.size() * 4) cap *= 2;
                index_.assign(cap, nullptr);
                for (const auto& t : slots_) place(t.get());
            } else {
                place(s);
            }
        }
        return s->e;
    }
    void place(Slot* s) noexcept {
        const size_t mask = index_.size() - 1;
        size_t i = s->h & mask;
        while (index_[i]) i = (i + 1) & mask;
        index_[i] = s;
    }

    // Each name its own node, so an Entry never moves.
    std::vector<std::unique_ptr<Slot>> slots_;
    std::vector<Slot*> index_;
};

struct Env : Cell {
    explicit Env(Env* o) : outer(o) {}
    [[nodiscard]] size_t cellSize() const noexcept override { return sizeof(Env); }
    Env* outer;
    VarMap vars;
    // A function's own: what `this`, `super` and `new.target` mean inside it.
    bool is_function = false;
    Value this_value;
    bool this_initialized = true;
    Object* function = nullptr;
    Object* home = nullptr;
    Object* new_target = nullptr;
    // The global scope's: its var names are the global object's properties.
    Object* global_object = nullptr;
    // A with statement's: its names are this object's properties (it has no vars).
    Object* with_object = nullptr;
};

struct FunctionData {
    FunctionInfo* info = nullptr;
    Env* closure = nullptr;
    Object* home = nullptr;          // a method's object, for super
    std::shared_ptr<Program> program;   // keeps the tree alive
    Object* class_fields_home = nullptr;
};

// Runs a script function's body in the environment a call prepared (runtime.cc): for a
// generator's or async function's coroutine.
Value runFunctionBody(Runtime& rt, Object* f, Env* env);

// yield, yield* and await, from the interpreter (async.cc).
Value generatorYield(Runtime& rt, const Value& v);
Value generatorDelegate(Runtime& rt, const Value& iterable);
Value awaitValue(Runtime& rt, const Value& v);
// A generator or async function's call, once its environment is ready (async.cc).
Value startCoroutineFunction(Runtime& rt, Object* f, Env* env);
Value rejectedPromise(Runtime& rt, const Value& reason);

// A BigInt's value as the nearest number, for Number(1n) (bigint_builtins.cc).
double bigintToNumber(const Value& v);

// A proxy's internal methods (proxy.cc).
Object* proxyGetPrototypeOf(Runtime& rt, Object* p);
bool proxySetPrototypeOf(Runtime& rt, Object* p, Object* proto);
bool proxyIsExtensible(Runtime& rt, Object* p);
bool proxyPreventExtensions(Runtime& rt, Object* p);
bool proxyGetOwnProperty(Runtime& rt, Object* p, const PropertyKey& key, Property& out);
bool proxyDefineOwnProperty(Runtime& rt, Object* p, const PropertyKey& key, const Value& desc);
bool proxyHas(Runtime& rt, Object* p, const PropertyKey& key);
Value proxyGet(Runtime& rt, Object* p, const PropertyKey& key, const Value& receiver);
bool proxySet(Runtime& rt, Object* p, const PropertyKey& key, const Value& v, const Value& receiver);
bool proxyDelete(Runtime& rt, Object* p, const PropertyKey& key);
std::vector<PropertyKey> proxyOwnKeys(Runtime& rt, Object* p);
Value proxyCall(Runtime& rt, Object* p, const Value& this_value, std::span<const Value> args);
Value proxyConstruct(Runtime& rt, Object* p, std::span<const Value> args, Object* new_target);

// Property descriptors (builtins.cc): an object read as one; a fresh object with only what
// a descriptor object said (ToPropertyDescriptor then FromPropertyDescriptor); a property
// as a descriptor object.
Property descriptorToProperty(Runtime& rt, const Value& desc);
Value normalizeDescriptor(Runtime& rt, const Value& desc);
Value propertyToDescriptor(Runtime& rt, const Property& p);

// Typed arrays' integer-indexed elements (typedarray.cc). A key that's a canonical
// numeric string is the typed array's business, valid index or not.
bool typedKey(const PropertyKey& key, double& index);
size_t typedLength(const Object* ta);
bool typedValidIndex(const Object* ta, double index);
Value typedGet(const Object* ta, double index);
bool typedSet(Runtime& rt, Object* ta, double index, const Value& v);

// eval: direct (in the caller's scope, `env`) or indirect (the global scope); and the
// Function constructors' functions made from source (runtime.cc).
Value evalCode(Runtime& rt, const std::u16string& source, Env* env, bool strict, bool direct);
Value dynamicFunction(Runtime& rt, std::span<const Value> args, const char* kind);

// A module's scope set up, and its statements run (runtime.cc).
void instantiateModuleScope(Runtime& rt, const std::shared_ptr<Program>& prog, Env* env);
void runModuleStatements(Runtime& rt, const std::shared_ptr<Program>& prog, Env* env);

// A module's body, run in its coroutine (modules.cc).
void runModuleBody(Runtime& rt, Coroutine& co);
void markModule(void* record, std::vector<Cell*>& todo);
// import.meta for the module at this URL (modules.cc).
Object* importMeta(Runtime& rt, const std::string& url);
// import(), from the interpreter: a promise for the namespace (modules.cc).
Value dynamicImport(Runtime& rt, const Value& specifier, const std::string& referrer);

}  // namespace fbx::xweb::js
