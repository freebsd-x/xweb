#pragma once

// Our JavaScript engine, written by hand like the rest of xweb: a lexer and parser for
// the ES2022 language, and an interpreter that walks the tree it makes, with the
// standard library's core, regular expressions, Promise, generators and async
// functions (on coroutines of their own), modules, weak collections, Date, typed arrays
// and eval. Not yet: BigInt, Proxy.

#include <cstdint>
#include <deque>
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
struct Coroutine;
namespace regex { struct Program; }

using JSString = std::shared_ptr<const std::u16string>;

// A JavaScript value.
class Value {
public:
    enum class Type : uint8_t { Undefined, Null, Boolean, Number, String, Symbol, Object, Empty, BigInt };

    Value() = default;
    static Value undefined() { return {}; }
    static Value null() { Value v; v.type_ = Type::Null; return v; }
    static Value empty() { Value v; v.type_ = Type::Empty; return v; }   // an array's hole
    static Value boolean(bool b) { Value v; v.type_ = Type::Boolean; v.b_ = b; return v; }
    static Value number(double d) { Value v; v.type_ = Type::Number; v.n_ = d; return v; }
    static Value string(JSString s) { Value v; v.type_ = Type::String; v.s_ = std::move(s); return v; }
    static Value string(std::u16string s);
    // A BigInt, its digits encoded (bigint.h) in the string slot.
    static Value bigint(JSString digits) { Value v; v.type_ = Type::BigInt; v.s_ = std::move(digits); return v; }
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
    [[nodiscard]] bool isBigInt() const noexcept { return type_ == Type::BigInt; }
    [[nodiscard]] const std::u16string& bigintDigits() const noexcept { return *s_; }

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
    // Its size, for the collector's scan of suspended coroutines' stacks.
    [[nodiscard]] virtual size_t cellSize() const noexcept { return sizeof(Cell); }
    bool marked = false;
};

class Symbol : public Cell {
public:
    explicit Symbol(std::u16string d, bool has) : description(std::move(d)), has_description(has) {}
    [[nodiscard]] size_t cellSize() const noexcept override { return sizeof(Symbol); }
    std::u16string description;
    bool has_description;
};

using NativeFunction = std::function<Value(Runtime&, const Value& this_value, std::span<const Value> args)>;

enum class ObjectClass : uint8_t {
    Ordinary, Array, Function, Error, Boolean, Number, String, Symbol, Arguments, Map, Set, Iterator, RegExp,
    Promise, Generator, AsyncGenerator, Internal, WeakMap, WeakSet, WeakRef, Date, ArrayBuffer, TypedArray, DataView, Proxy, BigInt,
};

// Map and Set keys: SameValueZero, hashed (strings by content, objects by identity).
struct ValueHash {
    size_t operator()(const Value& v) const noexcept;
};
struct ValueSameZero {
    bool operator()(const Value& a, const Value& b) const noexcept;
};

struct FunctionData;   // a script function's code and closure (runtime.cc)

// An ArrayBuffer's bytes, shared by the views on it.
struct BufferData {
    std::vector<uint8_t> bytes;
    bool detached = false;
    bool resizable = false;
    size_t max_length = 0;
};

class Object : public Cell {
public:
    explicit Object(Object* proto, ObjectClass cls = ObjectClass::Ordinary) : prototype(proto), cls(cls) {}
    ~Object() override;
    [[nodiscard]] size_t cellSize() const noexcept override { return sizeof(Object); }

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
    bool length_readonly = false;     // an array whose length was made non-writable
    bool derived = false;       // a class that extends another

    // Boolean, Number, String and Symbol wrappers' value; an iterator's state.
    Value primitive;
    std::vector<std::pair<Value, Value>> entries;   // Map and Set, in insertion order (deleted ones empty)
    std::unordered_map<Value, size_t, ValueHash, ValueSameZero> index;   // a key's place in entries
    size_t cursor = 0;
    Object* target = nullptr;   // an iterator's subject
    uint8_t iter_kind = 0;
    // A RegExp's compiled pattern; its source is `primitive`.
    std::shared_ptr<const regex::Program> regexp;
    // Internal slots: a built-in closure's captured values, a promise's reactions.
    std::vector<Value> slots;
    // A promise's state (pending, fulfilled, rejected); its result is `primitive`.
    uint8_t promise_state = 0;
    bool promise_handled = false;
    // An ArrayBuffer's bytes; a typed array's or DataView's window on its buffer (`target`):
    // its kind, where it starts, and its length (elements, or bytes for a DataView), or
    // all the rest of a resizable buffer when it tracks it.
    std::shared_ptr<BufferData> buffer;
    uint8_t view_kind = 0;
    bool view_tracking = false;
    size_t view_offset = 0;
    size_t view_length = 0;
    // A generator's or async function's suspended body.
    std::shared_ptr<Coroutine> co;

    // The embedder's: what a DOM wrapper stands for, and which kind of thing it is.
    void* host = nullptr;
    uint8_t host_kind = 0;

private:
    // In the order they were made. A removed one stays as a tombstone (flags
    // kTombstone) until enough have piled up to compact, so a delete is O(1).
    std::vector<std::pair<PropertyKey, Property>> props_;
    // Positions in props_ (plus one; 0 is empty), by key hash with linear probing.
    // Only once there are enough properties for a scan to be slow.
    std::vector<uint32_t> index_;
    uint32_t dead_ = 0;
    static constexpr uint8_t kTombstone = 0xFF;
    static constexpr size_t kScanMax = 8;
    [[nodiscard]] static size_t keyHash(const PropertyKey& k) noexcept;
    [[nodiscard]] ptrdiff_t find(const PropertyKey& key) const noexcept;
    void place(uint32_t pos) noexcept;
    void reindex();
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

// A job for the microtask queue: a promise reaction, a thenable to follow, or a callback.
struct Job {
    uint8_t kind = 0;
    Value a, b, c;
};

// A page's import maps (<script type="importmap">): bare names like "react", and
// URL prefixes, mapped to where they really are; scoped by the importing module's URL.
class ImportMap {
public:
    // Adds a map's JSON, its relative addresses read against `base`. False if it isn't a map.
    bool add(std::string_view json, const std::string& base);
    // A specifier resolved for a module at `referrer`: through the map, else as a URL
    // ("./a.js", "/a.js", "https://…"). Empty if it can't be (a bare name nobody mapped).
    [[nodiscard]] std::string resolve(const std::string& specifier, const std::string& referrer) const;
    [[nodiscard]] bool empty() const noexcept { return imports_.empty() && scopes_.empty(); }

private:
    using Entries = std::vector<std::pair<std::string, std::string>>;   // longest key first
    Entries imports_;
    std::vector<std::pair<std::string, Entries>> scopes_;              // longest prefix first
};

// The specifiers a module's import and export-from declarations name, in order, or
// nothing if it doesn't parse as a module. For loaders that fetch a graph ahead.
std::vector<std::string> moduleRequests(std::string_view source);

// What a module loader gives the runtime: the source at a URL, or an error.
struct ModuleSource {
    bool ok = false;
    std::string source;
    std::string error;
};

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

    // Modules. The loader fetches a module's source by its absolute URL and calls
    // `done` with it, now or later (from the embedder's event loop). Without one,
    // imports fail.
    using ModuleLoader = std::function<void(const std::string& url, std::function<void(ModuleSource)> done)>;
    void setModuleLoader(ModuleLoader loader) { module_loader_ = std::move(loader); }
    // Runs a module (and what it imports) whose own URL is `url`. It finishes when its
    // imports have loaded and any top-level await is done; `done` hears how it went.
    void evaluateModule(std::string_view source, const std::string& url, std::function<void(Result)> done = {});
    // How a specifier resolves against a referrer's URL ("./a.js", "/x.js", "https://...").
    static std::string resolveSpecifier(const std::string& specifier, const std::string& referrer);
    // What a classic script's import() resolves against when its own name isn't a URL.
    std::string base_url;
    // The page's import maps; every module specifier goes through them.
    ImportMap import_map;
    // The engine's own (modules.cc).
    struct Modules;
    struct ModulesDeleter { void operator()(Modules* m) const; };
    Modules& modulesForLoader();
    [[nodiscard]] const ModuleLoader& moduleLoader() const noexcept { return module_loader_; }
    [[nodiscard]] Env* globalEnv() const noexcept { return global_env_; }

    // Promises and the microtask queue.
    Object* newPromise();
    // As the promise's resolve function would: a thenable is followed, anything else fulfils.
    void resolvePromise(Object* promise, const Value& resolution);
    void rejectPromise(Object* promise, const Value& reason);
    void enqueueJob(Job job) { jobs_.push_back(std::move(job)); }
    void enqueueMicrotask(const Value& fn) { jobs_.push_back(Job{2, fn, {}, {}}); }
    // Runs queued jobs until there are none; an uncaught error in one is reported to the
    // console and the rest still run. Rejections nobody handled are reported after.
    void runJobs();
    [[nodiscard]] bool hasPendingJobs() const noexcept { return !jobs_.empty(); }

    // console.log and friends: level is "log", "warn", "error" and so on.
    void setConsole(std::function<void(std::string_view level, const std::string& text)> sink) { console_ = std::move(sink); }
    // Where errors nothing caught go when no script is on the stack to throw to: a
    // failing microtask, a promise rejected with no handler. The console by default.
    void setErrorReporter(std::function<void(const std::string& message)> sink) { error_reporter_ = std::move(sink); }
    void reportError(const std::string& message) {
        if (error_reporter_) error_reporter_(message);
        else emitConsole("error", message);
    }
    // How much work one evaluate() may do before it's stopped.
    void setStepBudget(uint64_t steps) { step_budget_ = steps; }
    // A new task (an event, a timer) starts with the whole budget again.
    void resetSteps() { steps_ = 0; }

    [[nodiscard]] Object* global() const noexcept { return global_; }

    // For bindings: objects, functions and properties made from C++.
    Object* newObject(Object* proto = nullptr);
    Object* newArray(std::vector<Value> elements = {});
    Object* newFunction(std::u16string name, int length, NativeFunction fn, bool constructor = false);
    Object* newError(const char* kind, const std::string& message);
    Symbol* newSymbol(std::u16string description, bool has_description = true);
    // A RegExp, as /source/flags would make it; a bad pattern throws a SyntaxError.
    Object* newRegExp(const std::u16string& source, const std::u16string& flags);
    // Detaches an ArrayBuffer (its bytes go; views on it read as empty). False if it isn't one.
    bool detachArrayBuffer(const Value& buffer);
    // An ArrayBuffer holding these bytes; and the bytes an ArrayBuffer or a view on one holds.
    Object* newArrayBuffer(std::string_view bytes);
    bool bufferBytes(const Value& source, std::string& out);
    // IsRegExp: an object with a truthy Symbol.match, or else a RegExp.
    bool isRegExp(const Value& v);
    void put(Object* o, const PropertyKey& key, Value v, uint8_t flags = kWritable | kConfigurable);
    void method(Object* o, const char16_t* name, int length, NativeFunction fn);

    // The language's operations, for bindings and the built-ins.
    Value get(const Value& base, const PropertyKey& key);
    Value get(Object* o, const PropertyKey& key, const Value& receiver);
    void set(const Value& base, const PropertyKey& key, const Value& v, bool strict = true);
    bool set(Object* o, const PropertyKey& key, const Value& v, const Value& receiver);
    bool has(Object* o, const PropertyKey& key);
    bool deleteProperty(Object* o, const PropertyKey& key);
    // The rest of an object's internal methods, a proxy's going through its handler.
    bool getOwnProperty(Object* o, const PropertyKey& key, Property& out);
    std::vector<PropertyKey> ownPropertyKeys(Object* o);
    // From a descriptor object; false (or a TypeError, when `throws`) if it can't be done.
    bool defineOwnProperty(Object* o, const PropertyKey& key, const Value& descriptor, bool throws = true);
    Object* getPrototypeOf(Object* o);
    bool setPrototypeOf(Object* o, Object* proto);
    bool isExtensible(Object* o);
    bool preventExtensions(Object* o);
    // IsArray: an Array, or a proxy for one.
    bool isArray(const Value& v);
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
    // One value at a time, as the spec's loops over an iterator go: an endless iterator
    // is fine as long as `each` stops it by throwing, and the iterator is closed (its
    // return()) when `each` throws, the original exception winning.
    void iterate(const Value& iterable, const std::function<void(const Value&)>& each);
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
    Object* regexp_prototype = nullptr;
    Object* regexp_string_iterator_prototype = nullptr;
    Object* regexp_exec = nullptr;   // RegExp.prototype.exec, for the fast path
    Object* regexp_constructor = nullptr;
    Object* promise_prototype = nullptr;
    Object* bigint_prototype = nullptr;
    Object* promise_constructor = nullptr;
    Object* promise_then = nullptr;   // Promise.prototype.then, as installed
    Object* generator_prototype = nullptr;              // %GeneratorPrototype%
    Object* async_generator_prototype = nullptr;        // %AsyncGeneratorPrototype%
    Object* async_iterator_prototype = nullptr;
    Object* generator_function_prototype = nullptr;
    Object* async_function_prototype = nullptr;
    Object* async_generator_function_prototype = nullptr;
    Object* async_from_sync_iterator_prototype = nullptr;
    Object* array_buffer_prototype = nullptr;
    Object* typed_array_prototype = nullptr;     // %TypedArray%.prototype
    Object* typed_array_constructors[11] = {};   // Int8Array … Float64Array, BigInt64Array, BigUint64Array
    Object* data_view_prototype = nullptr;
    Object* eval_function = nullptr;   // the global eval, which a direct call recognizes
    std::unordered_map<std::string, Object*> error_prototypes;
    Symbol* sym_iterator = nullptr;
    Symbol* sym_to_primitive = nullptr;
    Symbol* sym_to_string_tag = nullptr;
    Symbol* sym_has_instance = nullptr;
    Symbol* sym_match = nullptr;
    Symbol* sym_match_all = nullptr;
    Symbol* sym_replace = nullptr;
    Symbol* sym_search = nullptr;
    Symbol* sym_split = nullptr;
    Symbol* sym_species = nullptr;
    Symbol* sym_async_iterator = nullptr;
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
    // Many steps at once (the regex matcher, which counts its own).
    void steps(uint64_t n) {
        steps_ += n;
        if (steps_ > step_budget_) throw Terminated{"the script ran too long"};
    }
    std::vector<Env*> env_stack;
    // The coroutine running now (a generator's or async function's body), if any.
    Coroutine* current_co = nullptr;
    // Heap buffers of values the interpreter is using (TempRoots), for the collector.
    std::vector<std::vector<Value>*> temp_lists;
    // Promises rejected with no handler yet, reported after the jobs run.
    std::vector<Object*> pending_rejections;
    size_t call_depth = 0;
    size_t eval_depth = 0;   // C++ recursion in the interpreter
    std::vector<Value> extra_roots;   // values bindings keep while a script runs
    Object* construct_target = nullptr;   // while a built-in constructor runs under new
    Object* array_values_ = nullptr;      // Array.prototype.values, for the fast path
    // The last successful match, for the legacy RegExp.$1 and friends.
    std::u16string regexp_last_input;
    std::vector<int> regexp_last_caps;
    // Compiled patterns by flags and source: a loop's literal compiles once.
    std::unordered_map<std::u16string, std::shared_ptr<const regex::Program>> regexp_cache;
    void emitConsole(std::string_view level, const std::string& text) {
        if (console_) console_(level, text);
    }

    static constexpr size_t kMaxCallDepth = 10'000;   // the stack itself is checked too
    static constexpr size_t kMaxCells = 20'000'000;

private:
    void installBuiltins();
    void installRegExp();
    void installPromise();
    void installDate();
    void installTypedArrays();
    void installProxyAndReflect();
    void installBigInt();
    void installGenerators();
    void markModules(std::vector<Cell*>& todo);
    std::deque<Job> jobs_;
    ModuleLoader module_loader_;
    std::unique_ptr<Modules, ModulesDeleter> modules_;
    std::vector<Cell*> cells_;
    size_t allocations_ = 0;
    std::vector<std::shared_ptr<struct Program>> programs_;   // every script's tree, kept for its functions
    std::unordered_map<std::u16string, std::shared_ptr<struct Program>> eval_cache_;   // eval'd source, parsed once
    friend Value evalCode(Runtime& rt, const std::u16string& source, Env* env, bool strict, bool direct);
    Object* global_ = nullptr;
    Env* global_env_ = nullptr;
    std::function<void(std::string_view, const std::string&)> console_;
    std::function<void(const std::string&)> error_reporter_;
    uint64_t step_budget_ = 200'000'000;
    uint64_t steps_ = 0;
    friend class Interpreter;
    friend struct Builtins;
};

// What was thrown, as a line of text: "TypeError: x is not a function".
std::string describe(Runtime& rt, const Value& thrown);

}  // namespace fbx::xweb::js
