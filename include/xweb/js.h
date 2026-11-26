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

// A string value's characters, shared by every value holding them. Counted, not
// collected, and not atomically: a page's script values never leave its thread.
// (Was a std::shared_ptr: 16 bytes in every Value and its copies, numbers' too.)
class JSString {
public:
    JSString() noexcept = default;
    JSString(std::nullptr_t) noexcept {}
    JSString(const JSString& o) noexcept : r_(o.r_) { if (r_) ++r_->refs; }
    JSString(JSString&& o) noexcept : r_(o.r_) { o.r_ = nullptr; }
    JSString& operator=(const JSString& o) noexcept {
        if (o.r_) ++o.r_->refs;
        release();
        r_ = o.r_;
        return *this;
    }
    JSString& operator=(JSString&& o) noexcept {
        if (this != &o) {
            release();
            r_ = o.r_;
            o.r_ = nullptr;
        }
        return *this;
    }
    ~JSString() { release(); }

    static JSString make(std::u16string s) { JSString j; j.r_ = new Rep{1, std::move(s)}; return j; }
    static JSString make() { return make(std::u16string{}); }

    [[nodiscard]] const std::u16string& operator*() const noexcept { return r_->s; }
    [[nodiscard]] const std::u16string* operator->() const noexcept { return &r_->s; }
    [[nodiscard]] const std::u16string* get() const noexcept { return r_ ? &r_->s : nullptr; }
    explicit operator bool() const noexcept { return r_ != nullptr; }
    [[nodiscard]] long use_count() const noexcept { return r_ ? static_cast<long>(r_->refs) : 0; }
    bool operator==(const JSString& o) const noexcept { return r_ == o.r_; }
    bool operator!=(const JSString& o) const noexcept { return r_ != o.r_; }

private:
    friend class Value;
    struct Rep {
        uint32_t refs;
        std::u16string s;
    };
    void release() noexcept {
        if (r_ && --r_->refs == 0) delete r_;
        r_ = nullptr;
    }
    Rep* r_ = nullptr;
};

// A JavaScript value.
class Value {
public:
    enum class Type : uint8_t { Undefined, Null, Boolean, Number, String, Symbol, Object, Empty, BigInt };

    Value() noexcept = default;
    [[gnu::always_inline]] Value(const Value& o) noexcept : type_(o.type_), bits_(o.bits_) { if (counted()) ++s_->refs; }
    [[gnu::always_inline]] Value(Value&& o) noexcept : type_(o.type_), bits_(o.bits_) { o.type_ = Type::Undefined; }
    // (Numbers, objects and the like over the same: copied, no counts to keep. The
    // interpreter's every register move is one.)
    [[gnu::always_inline]] Value& operator=(const Value& o) noexcept {
        if (!counts(type_) && !counts(o.type_)) [[likely]] {
            type_ = o.type_;
            bits_ = o.bits_;
            return *this;
        }
        if (o.counted()) ++o.s_->refs;
        release();
        type_ = o.type_;
        bits_ = o.bits_;
        return *this;
    }
    [[gnu::always_inline]] Value& operator=(Value&& o) noexcept {
        if (this != &o) {
            if (counts(type_)) release();
            type_ = o.type_;
            bits_ = o.bits_;
            o.type_ = Type::Undefined;
        }
        return *this;
    }
    [[gnu::always_inline]] ~Value() { if (counts(type_)) release(); }
    static Value undefined() { return {}; }
    static Value null() { Value v; v.type_ = Type::Null; return v; }
    static Value empty() { Value v; v.type_ = Type::Empty; return v; }   // an array's hole
    static Value boolean(bool b) { Value v; v.type_ = Type::Boolean; v.b_ = b; return v; }
    static Value number(double d) { Value v; v.type_ = Type::Number; v.n_ = d; return v; }
    static Value string(JSString s) { Value v; v.type_ = Type::String; v.s_ = s.r_; s.r_ = nullptr; return v; }
    static Value string(std::u16string s);
    // A BigInt, its digits encoded (bigint.h) in the string slot.
    static Value bigint(JSString digits) { Value v; v.type_ = Type::BigInt; v.s_ = digits.r_; digits.r_ = nullptr; return v; }
    static Value object(Object* o) { Value v; v.type_ = o ? Type::Object : Type::Null; v.o_ = o; return v; }
    static Value symbol(Symbol* s) { Value v; v.type_ = Type::Symbol; v.sym_ = s; return v; }

    // As assigning number(d) and boolean(b), in place.
    void setNumber(double d) noexcept {
        if (counted()) release();
        type_ = Type::Number;
        n_ = d;
    }
    void setBoolean(bool b) noexcept {
        if (counted()) release();
        type_ = Type::Boolean;
        b_ = b;
    }

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
    [[nodiscard]] const std::u16string& bigintDigits() const noexcept { return s_->s; }

    [[nodiscard]] bool asBoolean() const noexcept { return b_; }
    [[nodiscard]] double asNumber() const noexcept { return n_; }
    [[nodiscard]] const std::u16string& asString() const noexcept { return s_->s; }
    // The characters, shared (another holder of them, counted).
    [[nodiscard]] JSString stringRef() const noexcept { JSString j; j.r_ = s_; if (s_) ++s_->refs; return j; }
    // Whether these are its characters' only holder (they may be changed in place).
    [[nodiscard]] bool stringUnshared() const noexcept { return s_ && s_->refs == 1; }
    [[nodiscard]] bool sameString(const Value& o) const noexcept { return s_ == o.s_; }
    [[nodiscard]] std::u16string& mutableString() noexcept { return s_->s; }
    [[nodiscard]] Object* asObject() const noexcept { return o_; }
    [[nodiscard]] Symbol* asSymbol() const noexcept { return sym_; }

private:
    [[nodiscard]] static bool counts(Type t) noexcept { return t == Type::String || t == Type::BigInt; }
    [[nodiscard]] bool counted() const noexcept { return counts(type_) && s_; }
    void release() noexcept {
        if (counted() && --s_->refs == 0) delete s_;
        type_ = Type::Undefined;
    }
    Type type_ = Type::Undefined;
    union {
        double n_ = 0;
        bool b_;
        Object* o_;
        Symbol* sym_;
        JSString::Rep* s_;   // a String's or BigInt's
        uint64_t bits_;      // any of them, for copying
    };
};
static_assert(sizeof(Value) == 16);

// A property's name: a string, or a symbol.
// A property's name, shared: an object's properties have the same one as the code that
// made them (a copy each was 80 bytes a property, and a heap allocation past ten letters).
class SharedName {
public:
    SharedName() = default;
    SharedName(std::u16string s) : s_(s.empty() ? JSString() : JSString::make(std::move(s))) {}
    SharedName(const char16_t* literal);
    explicit SharedName(JSString s) : s_(s && !s->empty() ? std::move(s) : JSString()) {}
    [[nodiscard]] const std::u16string& get() const noexcept {
        static const std::u16string kEmpty;
        return s_ ? *s_ : kEmpty;
    }
    operator const std::u16string&() const noexcept { return get(); }
    operator std::u16string_view() const noexcept { return get(); }
    // As a string value has it.
    [[nodiscard]] JSString str() const { return s_ ? s_ : JSString::make(); }
    [[nodiscard]] size_t size() const noexcept { return s_ ? s_->size() : 0; }
    [[nodiscard]] bool empty() const noexcept { return !s_; }
    char16_t operator[](size_t i) const noexcept { return (*s_)[i]; }
    [[nodiscard]] auto begin() const noexcept { return get().begin(); }
    [[nodiscard]] auto end() const noexcept { return get().end(); }
    bool operator==(const SharedName& o) const noexcept { return s_ == o.s_ || get() == o.get(); }
    bool operator==(const std::u16string& o) const noexcept { return get() == o; }
    bool operator==(const char16_t* o) const noexcept { return get() == o; }

private:
    JSString s_;
};

struct PropertyKey {
    SharedName name;
    Symbol* symbol = nullptr;
    // Object::keyHash's answer, worked out the first time it's asked (0: not yet). A key
    // looked up again and again (a member access's, kept on its node) isn't hashed again.
    mutable size_t hash = 0;
    PropertyKey() = default;
    PropertyKey(std::u16string n) : name(std::move(n)) {}
    PropertyKey(const char16_t* n) : name(n) {}
    explicit PropertyKey(JSString n) : name(std::move(n)) {}
    explicit PropertyKey(Symbol* s) : symbol(s) {}
    bool operator==(const PropertyKey& o) const { return symbol == o.symbol && name == o.name; }
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

// The keys of objects made alike (the same keys added in the same order): shared, so
// that each object holds only its values, and a place a name was found at once is where
// it is in all of them (the interpreter's caches). A tree: each shape the one before it
// with one more key. Owned by the runtime's root; never freed before it.
class Shape {
public:
    Shape() = default;
    Shape(const Shape&) = delete;
    Shape& operator=(const Shape&) = delete;
    [[nodiscard]] uint32_t count() const noexcept { return count_; }
    [[nodiscard]] const PropertyKey& keyAt(size_t i) const { build(); return *keys_[i]; }
    // Where a key is (-1: not here). `hash`: Object::keyHash's.
    [[nodiscard]] ptrdiff_t find(const PropertyKey& key, size_t hash) const;
    // This one with `key` added; null when there'd be too many (then the object holds
    // its own keys).
    Shape* with(const PropertyKey& key, size_t hash);
    [[nodiscard]] const Shape* parent() const noexcept { return parent_; }
    static constexpr uint32_t kMaxKeys = 64;

private:
    void build() const;
    Shape* parent_ = nullptr;
    PropertyKey key_;     // the last added
    uint32_t count_ = 0;
    // Its keys in order, and an index of them when there are many: made when first asked.
    mutable std::vector<const PropertyKey*> keys_;
    mutable std::vector<uint32_t> index_;
    std::vector<std::pair<size_t, std::unique_ptr<Shape>>> next_;   // by the key's hash
};

// Whatever the heap holds: objects, and symbols.
class Cell {
public:
    virtual ~Cell() = default;
    // Its size, for the collector's scan of suspended coroutines' stacks.
    [[nodiscard]] virtual size_t cellSize() const noexcept { return sizeof(Cell); }
    bool marked = false;
    // Which kind it is, for the collector (no dynamic_cast per cell).
    enum class Kind : uint8_t { Other, Object, Symbol, Env };
    Kind cell_kind = Kind::Other;
};

class Symbol : public Cell {
public:
    explicit Symbol(std::u16string d, bool has) : description(std::move(d)), has_description(has) { cell_kind = Kind::Symbol; }
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

// A built-in's C++ (most objects have none): held out of line, 8 bytes in every object
// rather than a std::function's 48. Used as one: tested, called, assigned.
class NativeSlot {
public:
    NativeSlot() = default;
    template <typename F>
    NativeSlot& operator=(F&& f) {
        fn_ = std::make_unique<NativeFunction>(std::forward<F>(f));
        return *this;
    }
    explicit operator bool() const noexcept { return fn_ && *fn_; }
    Value operator()(Runtime& rt, const Value& this_value, std::span<const Value> args) const;
    operator const NativeFunction&() const noexcept {
        static const NativeFunction none;
        return fn_ ? *fn_ : none;
    }

private:
    std::unique_ptr<NativeFunction> fn_;
};

// A map only some objects need (a Map's or Set's index): made when first written,
// read as empty till then.
template <typename M>
class LazyMap {
public:
    using iterator = typename M::iterator;
    template <typename K>
    iterator find(const K& k) { return m_ ? m_->find(k) : none().find(k); }
    iterator end() { return m_ ? m_->end() : none().end(); }
    template <typename K>
    size_t count(const K& k) const { return m_ ? m_->count(k) : 0; }
    [[nodiscard]] size_t size() const noexcept { return m_ ? m_->size() : 0; }
    void clear() { if (m_) m_->clear(); }
    template <typename... A>
    auto emplace(A&&... a) { return get().emplace(std::forward<A>(a)...); }
    auto erase(iterator it) { return get().erase(it); }
    template <typename K>
    size_t erase(const K& k) { return m_ ? m_->erase(k) : 0; }

private:
    M& get() {
        if (!m_) m_ = std::make_unique<M>();
        return *m_;
    }
    static M& none() {
        static M empty;   // (never written: only found in)
        return empty;
    }
    std::unique_ptr<M> m_;
};

// An ArrayBuffer's bytes, shared by the views on it.
struct BufferData {
    std::vector<uint8_t> bytes;
    bool detached = false;
    bool resizable = false;
    size_t max_length = 0;
};

class Object : public Cell {
public:
    explicit Object(Object* proto, ObjectClass cls = ObjectClass::Ordinary) : prototype(proto), cls(cls) { cell_kind = Kind::Object; }
    ~Object() override;
    [[nodiscard]] size_t cellSize() const noexcept override { return sizeof(Object); }

    Object* prototype;
    ObjectClass cls;
    bool extensible = true;

    // Own properties, in the order they were made.
    [[nodiscard]] Property* own(const PropertyKey& key);
    [[nodiscard]] const Property* own(const PropertyKey& key) const;
    // With the key's hash (keyHash) already worked out: a name looked up over and over.
    [[nodiscard]] const Property* ownHashed(const PropertyKey& key, size_t hash) const;
    // Where an own property is in the order they were made (-1: none), and the one there
    // if it's still that name's (a lookup's cached place, checked).
    [[nodiscard]] ptrdiff_t ownPos(const PropertyKey& key, size_t hash) const noexcept { return find(key, hash, true); }
    [[nodiscard]] Property* ownAt(size_t pos, const PropertyKey& key, size_t hash) noexcept;
    Property& define(const PropertyKey& key, Property p);
    // A property it surely hasn't yet (a new object's), added without looking for it first.
    Property& defineNew(const PropertyKey& key, Property p);
    // A script function's own (length, name, prototype...) are made when one's asked for, or
    // another's added, or they're listed: most functions are only called.
    void ownPropertiesLater(Runtime& rt) noexcept { later_ = &rt; }
    // Room for so many properties, made before they're added (one allocation, not several).
    void reserveProps(size_t n) {
        if (shape_) values_.reserve(n);
        else props_.reserve(n);
    }
    // Each own property, in order (no copy of the keys made, as ownKeys does).
    template <typename F>
    void forEachOwn(F&& f) const {
        if (shape_) {
            for (size_t i = 0; i < values_.size(); ++i) f(shape_->keyAt(i), values_[i]);
        } else {
            for (const auto& [k, p] : props_)
                if (p.flags != kTombstone) f(k, p);
        }
    }
    // A key's hash, worked out once and kept in it.
    [[nodiscard]] static size_t keyHash(const PropertyKey& k) noexcept;
    // Its shape (null: it holds its own keys, past many of them, or after a delete).
    [[nodiscard]] const Shape* shape() const noexcept { return shape_; }
    [[nodiscard]] Property& slot(size_t i) noexcept { return values_[i]; }
    void setShape(Shape* s) noexcept { shape_ = s; }
    // Shape s's own one more key (its next from this object's): its value added there.
    void addShaped(Shape* s, Property p) {
        shape_ = s;
        values_.push_back(std::move(p));
    }
    [[nodiscard]] size_t shapedCount() const noexcept { return values_.size(); }
    // Some of its own not made yet (a script function's length, name...).
    [[nodiscard]] bool ownLater() const noexcept { return later_ != nullptr; }
    bool remove(const PropertyKey& key);
    [[nodiscard]] std::vector<PropertyKey> ownKeys() const;   // indexes, strings, then symbols, as the spec orders them

    // An array's elements, kept apart for speed; holes are Value::empty().
    std::vector<Value> elements;

    // Functions.
    NativeSlot native;
    // A built-in that keeps nothing in its own heap buffers while it calls back into
    // JavaScript (or roots what it keeps): a collection may run inside it.
    bool gc_safe = false;
    std::shared_ptr<FunctionData> code;
    Object* bound_target = nullptr;
    // A bound function's this and leading arguments: out of line, as few objects are one
    // (every object carried them: an object was 512 bytes; a page keeps 250 000).
    struct BoundData {
        Value this_value;
        std::vector<Value> args;
    };
    std::unique_ptr<BoundData> bound;
    bool constructor = false;   // can be called with new
    bool class_constructor = false;   // can only be called with new
    bool length_readonly = false;     // an array whose length was made non-writable
    bool derived = false;       // a class that extends another

    // Boolean, Number, String and Symbol wrappers' value; an iterator's state.
    Value primitive;
    std::vector<std::pair<Value, Value>> entries;   // Map and Set, in insertion order (deleted ones empty)
    LazyMap<std::unordered_map<Value, size_t, ValueHash, ValueSameZero>> index;   // a key's place in entries
    size_t cursor = 0;
    Object* target = nullptr;   // an iterator's subject
    uint8_t iter_kind = 0;
    // [[IsHTMLDDA]] (document.all): false as a boolean, typeof "undefined", == null.
    bool html_dda = false;
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
    // With a shape: its values, in the shape's order.
    Shape* shape_ = nullptr;
    std::vector<Property> values_;
    void ownKeysNow();   // from the shape into props_ (and no shape from then on)
    // Without one: in the order they were made. A removed one stays as a tombstone (flags
    // kTombstone) until enough have piled up to compact, so a delete is O(1).
    std::vector<std::pair<PropertyKey, Property>> props_;
    // Positions in props_ (plus one; 0 is empty), by key hash with linear probing.
    // Only once there are enough properties for a scan to be slow.
    std::vector<uint32_t> index_;
    uint32_t dead_ = 0;
    Runtime* later_ = nullptr;
    void makeOwn() const;   // those put off
    [[nodiscard]] static bool isFunctionsOwn(const PropertyKey& key) noexcept;
    static constexpr uint8_t kTombstone = 0xFF;
    static constexpr size_t kScanMax = 8;
    [[nodiscard]] ptrdiff_t find(const PropertyKey& key) const noexcept { return find(key, 0, false); }
    [[nodiscard]] ptrdiff_t find(const PropertyKey& key, size_t hash, bool hashed) const noexcept;
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

// A script or module parsed before it's run, on any thread: running that very source
// takes the parse instead of making its own (moduleRequests looks at it too). What's never
// run is let go as others come, or by forgetParsed.
void parseAhead(const std::string& source, bool module);
void forgetParsed();

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
        std::string where;    // and where, " (url:line)", if it was an error that knows
    };
    // Runs a script in the global scope. `want_value`: whether its completion value is
    // wanted (the result's value); a page's scripts don't need one, and keeping each
    // statement's held on to strings a script then couldn't grow in place.
    Result evaluate(std::string_view source, std::string_view name = "script", bool want_value = true);
    // Whether statements' completion values are kept: while running code whose value
    // someone wants (eval, a script asked for its result).
    bool want_completion = true;

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
    // A promise rejected with nobody handling it, first offered here (a page's
    // "unhandledrejection"); true: it was dealt with, nothing to report.
    std::function<bool(const Value& promise, const Value& reason)> unhandled_rejection;
    void reportError(const std::string& message) {
        if (error_reporter_) error_reporter_(message);
        else emitConsole("error", message);
    }
    // How much work one evaluate() may do before it's stopped.
    void setStepBudget(uint64_t steps) { step_budget_ = steps; }
    // Called now and then as a script runs, for whoever's kept waiting by it. It mustn't
    // run any, nor touch the page.
    std::function<void()> on_breathe;
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
    Object* array_constructor = nullptr;      // the intrinsic Array, and its [Symbol.species] getter:
    Object* array_species_getter = nullptr;   // a plain array's species, quickly
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
    Symbol* sym_is_concat_spreadable = nullptr;
    Symbol* sym_async_iterator = nullptr;
    Symbol* sym_unscopables = nullptr;
    std::unordered_map<std::u16string, Symbol*> symbol_registry;

    // Heap.
    template <typename T, typename... A>
    T* allocate(A&&... args) {
        if (++allocations_ > kMaxCells) throw Terminated{"out of memory"};
        auto* c = new T(std::forward<A>(args)...);
        if constexpr (std::is_same_v<T, Object>)
            if (c->cls != ObjectClass::Proxy) c->setShape(&root_shape_);
        cells_.push_back(c);
        return c;
    }
    // Frees what nothing reachable refers to. Between scripts, or at a safe point while
    // one runs (safePoint): then the running stacks are scanned for cells
    // (conservatively), along with each call's function, this and arguments, the
    // interpreter's heap buffers (TempRoots) and the embedder's (host_roots).
    void collectGarbage();
    [[nodiscard]] size_t heapSize() const noexcept { return cells_.size(); }
    // Called by the interpreter where a collection may run mid-script (a loop's next
    // round, a call's start): it does once the heap has grown enough, unless C++ that
    // holds values where the collector can't see them is on the stack (NoGc).
    void safePoint() {
        if (cells_.size() >= gc_next_ && gc_inhibit == 0) collectMidScript();
    }
    // While one lives, no collection runs mid-script: a built-in that keeps values in
    // its own heap buffers while calling back into JavaScript holds one.
    struct NoGc {
        Runtime& rt;
        explicit NoGc(Runtime& r) : rt(r) { ++rt.gc_inhibit; }
        ~NoGc() { --rt.gc_inhibit; }
        NoGc(const NoGc&) = delete;
        NoGc& operator=(const NoGc&) = delete;
    };
    size_t gc_inhibit = 0;
    // The embedder's values (wrappers, timers, pending callbacks), for every collection.
    std::function<void(std::vector<Value>&)> host_roots;
    // How many collections ran mid-script (for tests), and the smallest heap growth that
    // starts one (lowered by XWEB_GC_STRESS to collect at nearly every safe point).
    size_t mid_script_collections = 0;
    size_t gc_min_growth = 500'000;
    void setGcGrowth(size_t cells) {
        gc_min_growth = cells ? cells : 1;
        gc_next_ = cells_.size() + gc_min_growth;
    }
    // What each call in progress was given: kept alive while it runs, whoever holds it.
    struct CallRoots {
        const Value* fn;
        const Value* self;
        const Value* args;
        size_t count;
    };
    std::vector<CallRoots> call_roots;
    // Scopes a call keeps off the collected heap (they die with it): roots while it runs.
    // Calls nest, so their owned scopes are one stack: each call's on top of its caller's.
    std::vector<Env*> owned_stack;
    // Owned scopes done with, kept to be made again: a call made two or three heap
    // allocations for its scope alone, freed as it returned.
    std::vector<Env*> env_pool;

    // The interpreter's own (runtime.cc).
    void step() {
        if ((++steps_ & 0x3FFF) == 0 || steps_ > step_budget_) stepSlow();
    }
    void stepSlow();
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
    // A tagged template's strings array, made once per call site: lit and friends
    // recognise a template by that identity, and remake the DOM when it changes.
    std::unordered_map<const void*, Object*> template_objects;
    // Changes whenever a name may newly stand between code and a global it found before:
    // a script's top-level let/const/class, a direct eval's var. (The VM's global name
    // caches hold while it doesn't.)
    uint64_t scope_gen = 1;
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
    size_t cells_sorted_ = 0;   // so many of cells_, from the start, by address
    size_t allocations_ = 0;
    size_t gc_next_ = 500'000;   // the heap size at which a safe point collects
    void collectMidScript();
    void collect(bool mid_script);
    std::vector<std::shared_ptr<struct Program>> programs_;   // every script's tree, kept for its functions
    std::unordered_map<std::u16string, std::shared_ptr<struct Program>> eval_cache_;   // eval'd source, parsed once
    friend Value evalCode(Runtime& rt, const std::u16string& source, Env* env, bool strict, bool direct);
    Object* global_ = nullptr;
    Env* global_env_ = nullptr;
    std::function<void(std::string_view, const std::string&)> console_;
    std::function<void(const std::string&)> error_reporter_;
    uint64_t step_budget_ = 200'000'000;
    Shape root_shape_;   // (before anything with a shape goes; see allocate)
    uint64_t steps_ = 0;
    friend class Interpreter;
    friend struct Builtins;
};

// What was thrown, as a line of text: "TypeError: x is not a function".
std::string describe(Runtime& rt, const Value& thrown);
// What was thrown, for a line after "Uncaught ": an error's name and message, an
// object's message or a glimpse of its properties, a value as a string.
std::string describeValue(Runtime& rt, const Value& thrown);
// Where an error was made, " (url:line)" from its stack, or "".
std::string describeWhere(Runtime& rt, const Value& thrown);

}  // namespace fbx::xweb::js
