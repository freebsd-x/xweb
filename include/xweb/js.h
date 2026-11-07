#pragma once

// Our JavaScript engine, written by hand like the rest of xweb: a lexer and parser for
// the ES2020 language, and an interpreter that walks the tree it makes, with the
// standard library's core. Not yet: generators, async functions, RegExp, Date, Promise
// and the DOM, which the page's bindings will add.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbx::xweb::js {

class Object;
class Symbol;
class Runtime;
struct Env;

using JSString = std::shared_ptr<const std::u16string>;

// A JavaScript value.
class Value {
public:
    enum class Type : uint8_t { Undefined, Null, Boolean, Number, String, Symbol, Object, Empty };

    Value() = default;
    static Value undefined() { return {}; }
    static Value null() { Value v; v.type_ = Type::Null; return v; }
    static Value empty() { Value v; v.type_ = Type::Empty; return v; }   // an array's hole
    static Value boolean(bool b) { Value v; v.type_ = Type::Boolean; v.b_ = b; return v; }
    static Value number(double d) { Value v; v.type_ = Type::Number; v.n_ = d; return v; }
    static Value string(JSString s) { Value v; v.type_ = Type::String; v.s_ = std::move(s); return v; }
    static Value string(std::u16string s);
    static Value object(Object* o) { Value v; v.type_ = o ? Type::Object : Type::Null; v.o_ = o; return v; }
    static Value symbol(Symbol* s) { Value v; v.type_ = Type::Symbol; v.sym_ = s; return v; }

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool isUndefined() const noexcept { return type_ == Type::Undefined; }
    [[nodiscard]] bool isNull() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool isNullish() const noexcept { return type_ == Type::Undefined || type_ == Type::Null; }
    [[nodiscard]] bool isBoolean() const noexcept { return type_ == Type::Boolean; }
    [[nodiscard]] bool isNumber() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool isString() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool isSymbol() const noexcept { return type_ == Type::Symbol; }
    [[nodiscard]] bool isObject() const noexcept { return type_ == Type::Object; }
    [[nodiscard]] bool isEmpty() const noexcept { return type_ == Type::Empty; }

    [[nodiscard]] bool asBoolean() const noexcept { return b_; }
    [[nodiscard]] double asNumber() const noexcept { return n_; }
    [[nodiscard]] const std::u16string& asString() const noexcept { return *s_; }
    [[nodiscard]] const JSString& stringRef() const noexcept { return s_; }
    [[nodiscard]] Object* asObject() const noexcept { return o_; }
    [[nodiscard]] Symbol* asSymbol() const noexcept { return sym_; }

private:
    Type type_ = Type::Undefined;
    union {
        double n_ = 0;
        bool b_;
        Object* o_;
        Symbol* sym_;
    };
    JSString s_;
};

// A property's name: a string, or a symbol.
struct PropertyKey {
    std::u16string name;
    Symbol* symbol = nullptr;
    PropertyKey() = default;
    PropertyKey(std::u16string n) : name(std::move(n)) {}
    PropertyKey(const char16_t* n) : name(n) {}
    explicit PropertyKey(Symbol* s) : symbol(s) {}
    bool operator==(const PropertyKey&) const = default;
    [[nodiscard]] bool isSymbol() const noexcept { return symbol != nullptr; }
};

struct PropertyKeyHash {
    size_t operator()(const PropertyKey& k) const noexcept {
        return k.symbol ? std::hash<const void*>{}(k.symbol) : std::hash<std::u16string>{}(k.name);
    }
};

enum PropertyFlags : uint8_t { kWritable = 1, kEnumerable = 2, kConfigurable = 4, kAccessor = 8 };
inline constexpr uint8_t kDefaultFlags = kWritable | kEnumerable | kConfigurable;

struct Property {
    Value value;
    Object* getter = nullptr;
    Object* setter = nullptr;
    uint8_t flags = kDefaultFlags;
};

// Whatever the heap holds: objects, and symbols.
class Cell {
public:
    virtual ~Cell() = default;
    bool marked = false;
};

class Symbol : public Cell {
public:
    explicit Symbol(std::u16string d, bool has) : description(std::move(d)), has_description(has) {}
    std::u16string description;
    bool has_description;
};

using NativeFunction = std::function<Value(Runtime&, const Value& this_value, std::span<const Value> args)>;

enum class ObjectClass : uint8_t {
    Ordinary, Array, Function, Error, Boolean, Number, String, Symbol, Arguments, Map, Set, Iterator,
};

struct FunctionData;   // a script function's code and closure (runtime.cc)

class Object : public Cell {
public:
    explicit Object(Object* proto, ObjectClass cls = ObjectClass::Ordinary) : prototype(proto), cls(cls) {}
    ~Object() override;

    Object* prototype;
    ObjectClass cls;
    bool extensible = true;

    // Own properties, in the order they were made.
    [[nodiscard]] Property* own(const PropertyKey& key);
    [[nodiscard]] const Property* own(const PropertyKey& key) const;
    Property& define(const PropertyKey& key, Property p);
    bool remove(const PropertyKey& key);
    [[nodiscard]] std::vector<PropertyKey> ownKeys() const;   // indexes, strings, then symbols, as the spec orders them

    // An array's elements, kept apart for speed; holes are Value::empty().
    std::vector<Value> elements;

    // Functions.
    NativeFunction native;
    std::shared_ptr<FunctionData> code;
    Object* bound_target = nullptr;
    Value bound_this;
    std::vector<Value> bound_args;
    bool constructor = false;   // can be called with new
    bool class_constructor = false;   // can only be called with new
    bool derived = false;       // a class that extends another

    // Boolean, Number, String and Symbol wrappers' value; an iterator's state.
    Value primitive;
    std::vector<std::pair<Value, Value>> entries;   // Map and Set, in insertion order
    size_t cursor = 0;
    Object* target = nullptr;   // an iterator's subject
    uint8_t iter_kind = 0;

private:
    std::vector<std::pair<PropertyKey, Property>> props_;
    std::unordered_map<PropertyKey, size_t, PropertyKeyHash> index_;
};

// A thrown JavaScript value, as it travels through the C++ that runs it.
struct Thrown {
    Value value;
};

// The page ran too long, or out of memory or stack: stops everything, catch or not.
struct Terminated {
    std::string why;
};

std::u16string utf16(std::string_view utf8);
std::string utf8(std::u16string_view utf16);

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    struct Result {
        bool ok = false;
        Value value;          // the last statement's, as eval would give
        std::string error;    // for a failure: what was thrown, as text
    };
    // Runs a script in the global scope.
    Result evaluate(std::string_view source, std::string_view name = "script");

    // console.log and friends: level is "log", "warn", "error" and so on.
    void setConsole(std::function<void(std::string_view level, const std::string& text)> sink) { console_ = std::move(sink); }
    // How much work one evaluate() may do before it's stopped.
    void setStepBudget(uint64_t steps) { step_budget_ = steps; }

    [[nodiscard]] Object* global() const noexcept { return global_; }

    // For bindings: objects, functions and properties made from C++.
    Object* newObject(Object* proto = nullptr);
    Object* newArray(std::vector<Value> elements = {});
    Object* newFunction(std::u16string name, int length, NativeFunction fn, bool constructor = false);
    Object* newError(const char* kind, const std::string& message);
    Symbol* newSymbol(std::u16string description, bool has_description = true);
    void put(Object* o, const PropertyKey& key, Value v, uint8_t flags = kWritable | kConfigurable);
    void method(Object* o, const char16_t* name, int length, NativeFunction fn);

    // The language's operations, for bindings and the built-ins.
    Value get(const Value& base, const PropertyKey& key);
    Value get(Object* o, const PropertyKey& key, const Value& receiver);
    void set(const Value& base, const PropertyKey& key, const Value& v, bool strict = true);
    bool set(Object* o, const PropertyKey& key, const Value& v, const Value& receiver);
    bool has(Object* o, const PropertyKey& key);
    bool deleteProperty(Object* o, const PropertyKey& key);
    Value call(const Value& fn, const Value& this_value, std::span<const Value> args);
    Value construct(const Value& fn, std::span<const Value> args, Object* new_target = nullptr);
    [[nodiscard]] bool isCallable(const Value& v) const;
    [[nodiscard]] bool isConstructor(const Value& v) const;

    [[nodiscard]] bool toBoolean(const Value& v) const;
    double toNumber(const Value& v);
    double toIntegerOrInfinity(const Value& v);
    int32_t toInt32(const Value& v);
    uint32_t toUint32(const Value& v);
    JSString toString(const Value& v);
    std::u16string toStringValue(const Value& v) { return *toString(v); }
    Object* toObject(const Value& v);
    Value toPrimitive(const Value& v, const char* hint = "default");
    PropertyKey toPropertyKey(const Value& v);
    uint64_t lengthOf(Object* o);
    [[nodiscard]] static bool strictEquals(const Value& a, const Value& b);
    bool looseEquals(const Value& a, const Value& b);
    [[nodiscard]] static bool sameValue(const Value& a, const Value& b);
    [[nodiscard]] static bool sameValueZero(const Value& a, const Value& b);
    static std::u16string numberToString(double d, int radix = 10);
    static double stringToNumber(std::u16string_view s);
    Value typeOf(const Value& v);

    // Throws a new error of this kind ("TypeError", "RangeError"...), as the script would.
    [[noreturn]] void throwError(const char* kind, const std::string& message);

    // Iteration, through Symbol.iterator.
    std::vector<Value> iterateToList(const Value& iterable);
    Object* createIterResult(const Value& value, bool done);

    // Well-known objects and symbols.
    Object* object_prototype = nullptr;
    Object* function_prototype = nullptr;
    Object* array_prototype = nullptr;
    Object* string_prototype = nullptr;
    Object* number_prototype = nullptr;
    Object* boolean_prototype = nullptr;
    Object* symbol_prototype = nullptr;
    Object* error_prototype = nullptr;
    Object* iterator_prototype = nullptr;
    Object* array_iterator_prototype = nullptr;
    Object* map_prototype = nullptr;
    Object* set_prototype = nullptr;
    std::unordered_map<std::string, Object*> error_prototypes;
    Symbol* sym_iterator = nullptr;
    Symbol* sym_to_primitive = nullptr;
    Symbol* sym_to_string_tag = nullptr;
    Symbol* sym_has_instance = nullptr;
    std::unordered_map<std::u16string, Symbol*> symbol_registry;

    // Heap.
    template <typename T, typename... A>
    T* allocate(A&&... args) {
        if (++allocations_ > kMaxCells) throw Terminated{"out of memory"};
        auto* c = new T(std::forward<A>(args)...);
        cells_.push_back(c);
        return c;
    }
    // Frees what nothing reachable refers to. Only between scripts: values held in C++
    // locals while a script runs aren't roots.
    void collectGarbage();
    [[nodiscard]] size_t heapSize() const noexcept { return cells_.size(); }

    // The interpreter's own (runtime.cc).
    void step();
    std::vector<Env*> env_stack;
    size_t call_depth = 0;
    size_t eval_depth = 0;   // C++ recursion in the interpreter
    std::vector<Value> extra_roots;   // values bindings keep while a script runs
    Object* construct_target = nullptr;   // while a built-in constructor runs under new
    Object* array_values_ = nullptr;      // Array.prototype.values, for the fast path
    void emitConsole(std::string_view level, const std::string& text) {
        if (console_) console_(level, text);
    }

    static constexpr size_t kMaxCallDepth = 300;
    static constexpr size_t kMaxCells = 20'000'000;

private:
    void installBuiltins();
    std::vector<Cell*> cells_;
    size_t allocations_ = 0;
    std::vector<std::shared_ptr<struct Program>> programs_;   // every script's tree, kept for its functions
    Object* global_ = nullptr;
    Env* global_env_ = nullptr;
    std::function<void(std::string_view, const std::string&)> console_;
    uint64_t step_budget_ = 200'000'000;
    uint64_t steps_ = 0;
    friend class Interpreter;
    friend struct Builtins;
};

// What was thrown, as a line of text: "TypeError: x is not a function".
std::string describe(Runtime& rt, const Value& thrown);

}  // namespace fbx::xweb::js
