// The standard library's core: Object, Function, Array, String, Number, Boolean,
// Symbol, Math, JSON, the Error types, Map, Set, iterators, the global functions and
// console. Written from the ECMAScript specification, the common paths first.

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "coroutine.h"
#include "internal.h"
#include "xweb/js.h"

namespace fbx::xweb::js {

namespace {

// ArraySpeciesCreate's length check, before a result is built element by element: past
// 2^32 - 1 it's a RangeError by the spec, and past the engine's limit on a dense array
// one too (rather than a C++ allocation the size of whatever `length` claims).
void checkNewArrayLength(Runtime& rt, uint64_t n) {
    if (n > 4294967295ull) rt.throwError("RangeError", "invalid array length");
    if (n > 100'000'000) rt.throwError("RangeError", "array too long");
}


using Args = std::span<const Value>;

Value arg(Args a, size_t i) { return i < a.size() ? a[i] : Value::undefined(); }

std::u16string keyString(const PropertyKey& k) { return k.isSymbol() ? u"Symbol(" + k.symbol->description + u")" : k.name; }

// Relative index arguments (slice, splice...): negative counts from the end.
uint64_t relative(Runtime& rt, const Value& v, uint64_t len, uint64_t if_undefined) {
    if (v.isUndefined()) return if_undefined;
    const double d = rt.toIntegerOrInfinity(v);
    if (d < 0) return d + static_cast<double>(len) < 0 ? 0 : static_cast<uint64_t>(d + static_cast<double>(len));
    return d > static_cast<double>(len) ? len : static_cast<uint64_t>(d);
}

Object* thisObject(Runtime& rt, const Value& t) { return rt.toObject(t); }

Value elementAt(Runtime& rt, Object* o, uint64_t i) {
    if (o->cls == ObjectClass::Array && i < o->elements.size() && !o->elements[i].isEmpty()) return o->elements[i];
    return rt.get(o, PropertyKey(Runtime::numberToString(static_cast<double>(i))), Value::object(o));
}

bool hasElement(Runtime& rt, Object* o, uint64_t i) {
    if (o->cls == ObjectClass::Array && i < o->elements.size()) return !o->elements[i].isEmpty() || rt.has(o->prototype, Runtime::numberToString(static_cast<double>(i)));
    return rt.has(o, PropertyKey(Runtime::numberToString(static_cast<double>(i))));
}

void setElement(Runtime& rt, Object* o, uint64_t i, const Value& v) {
    rt.set(Value::object(o), PropertyKey(Runtime::numberToString(static_cast<double>(i))), v, true);
}

void setLength(Runtime& rt, Object* o, uint64_t len) { rt.set(Value::object(o), u"length", Value::number(static_cast<double>(len)), true); }

Value callback(Runtime& rt, const Value& fn) {
    if (!rt.isCallable(fn)) rt.throwError("TypeError", "a callback must be a function");
    return fn;
}

// A property descriptor object, read.
Property toDescriptor(Runtime& rt, const Value& d, uint8_t& present) {
    if (!d.isObject()) rt.throwError("TypeError", "a property descriptor must be an object");
    Property p{Value::undefined(), nullptr, nullptr, 0};
    present = 0;
    Object* o = d.asObject();
    const auto flag = [&](const char16_t* name, uint8_t bit) {
        if (rt.has(o, name)) {
            present |= bit;
            if (rt.toBoolean(rt.get(d, name))) p.flags |= bit;
        }
    };
    flag(u"enumerable", kEnumerable);
    flag(u"configurable", kConfigurable);
    flag(u"writable", kWritable);
    if (rt.has(o, u"value")) {
        present |= 16;
        p.value = rt.get(d, u"value");
    }
    for (int k = 0; k < 2; ++k) {
        const char16_t* name = k == 0 ? u"get" : u"set";
        if (!rt.has(o, name)) continue;
        const Value f = rt.get(d, name);
        if (!f.isUndefined() && !rt.isCallable(f)) rt.throwError("TypeError", "a getter or setter must be a function");
        p.flags |= kAccessor;
        present |= 32;
        (k == 0 ? p.getter : p.setter) = f.isObject() ? f.asObject() : nullptr;
    }
    if ((p.flags & kAccessor) && (present & (16 | kWritable))) rt.throwError("TypeError", "a descriptor can't have both accessors and a value");
    return p;
}

void defineFromDescriptor(Runtime& rt, Object* o, const PropertyKey& key, const Value& desc) {
    if (o->cls == ObjectClass::Proxy) {
        if (!desc.isObject()) rt.throwError("TypeError", "a property descriptor must be an object");
        if (!proxyDefineOwnProperty(rt, o, key, desc)) rt.throwError("TypeError", "the proxy refused to define " + utf8(keyString(key)));
        return;
    }
    uint8_t present = 0;
    Property p = toDescriptor(rt, desc, present);
    // A typed array's element: only as a plain, writable, enumerable, configurable value.
    if (!key.isSymbol() && o->cls == ObjectClass::TypedArray) {
        double index;
        if (typedKey(key, index)) {
            if (!typedValidIndex(o, index)) rt.throwError("TypeError", "not a valid index of the typed array");
            if ((p.flags & kAccessor) || ((present & kConfigurable) && !(p.flags & kConfigurable)) ||
                ((present & kEnumerable) && !(p.flags & kEnumerable)) || ((present & kWritable) && !(p.flags & kWritable)))
                rt.throwError("TypeError", "a typed array's element can't be redefined that way");
            if (present & 16) typedSet(rt, o, index, p.value);
            return;
        }
    }
    // An array's element or length.
    if (!key.isSymbol() && o->cls == ObjectClass::Array) {
        if (key.name == u"length") {
            // length is a non-configurable data property: it can't become an accessor.
            if (p.flags & kAccessor) rt.throwError("TypeError", "an array's length can't become an accessor");
            // length: writable, not enumerable, not configurable; it can become read-only.
            if ((present & kConfigurable) && (p.flags & kConfigurable)) rt.throwError("TypeError", "an array's length can't be made configurable");
            if ((present & kEnumerable) && (p.flags & kEnumerable)) rt.throwError("TypeError", "an array's length can't be made enumerable");
            if (o->length_readonly) {
                if ((present & kWritable) && (p.flags & kWritable)) rt.throwError("TypeError", "a read-only length can't become writable again");
                if ((present & 16) && rt.toNumber(p.value) != static_cast<double>(o->elements.size())) rt.throwError("TypeError", "the array's length is read-only");
                return;
            }
            if (present & 16) {
                const double n = rt.toNumber(p.value);
                if (n != rt.toUint32(p.value)) rt.throwError("RangeError", "invalid array length");
                if (!rt.set(o, key, p.value, Value::object(o))) rt.throwError("TypeError", "the array has an element that can't be deleted");
            }
            if ((present & kWritable) && !(p.flags & kWritable)) o->length_readonly = true;
            return;
        }
        bool index = !key.name.empty() && key.name.size() < 10 && std::all_of(key.name.begin(), key.name.end(), [](char16_t c) { return c >= u'0' && c <= u'9'; }) &&
                     (key.name.size() == 1 || key.name[0] != u'0');
        if (index && o->length_readonly && std::stoul(utf8(key.name)) >= o->elements.size())
            rt.throwError("TypeError", "the array's length is read-only");
        // A plain element stays in the dense part: an existing one keeps its flags when the
        // descriptor leaves them out; a new one only if it asks for all three.
        const bool exists = index && std::stoul(utf8(key.name)) < o->elements.size() && !o->elements[std::stoul(utf8(key.name))].isEmpty();
        const uint8_t asked = present & (kWritable | kEnumerable | kConfigurable);
        // An accessor is never plain: it leaves the dense part, or get() would still
        // find the old value there before the getter.
        const bool plain = !(p.flags & kAccessor) &&
                           (exists ? (p.flags & asked) == asked : (asked == (kWritable | kEnumerable | kConfigurable) && (p.flags & kDefaultFlags) == kDefaultFlags));
        if (index && plain) {
            if (present & 16) rt.set(o, key, p.value, Value::object(o));
            else if (!exists && !(present & 16)) rt.set(o, key, Value::undefined(), Value::object(o));
            return;
        }
        if (index && exists) {
            // Moving out of the dense part: it keeps its value and its (all-true) flags first.
            const size_t i = std::stoul(utf8(key.name));
            o->define(key, {o->elements[i], nullptr, nullptr, kDefaultFlags});
            o->elements[i] = Value::empty();
        } else if (index && std::stoul(utf8(key.name)) >= o->elements.size()) {
            o->elements.resize(std::stoul(utf8(key.name)) + 1, Value::empty());
        }
        if (index) {
            // A special element: out of the dense part, into the property map.
            const size_t i = std::stoul(utf8(key.name));
            if (i < o->elements.size()) o->elements[i] = Value::empty();
        }
    }
    Property* existing = o->own(key);
    if (existing) {
        if (!(existing->flags & kConfigurable)) {
            const bool changing = ((present & kConfigurable) && (p.flags & kConfigurable)) ||
                                  ((present & kEnumerable) && ((p.flags ^ existing->flags) & kEnumerable)) ||
                                  ((existing->flags & kAccessor) != (p.flags & kAccessor) && (present & (16 | 32 | kWritable))) ||
                                  (!(existing->flags & kAccessor) && !(existing->flags & kWritable) &&
                                   (((present & kWritable) && (p.flags & kWritable)) || ((present & 16) && !Runtime::sameValue(p.value, existing->value))));
            if (changing) rt.throwError("TypeError", "can't redefine non-configurable property '" + utf8(keyString(key)) + "'");
        }
        Property merged = *existing;
        if (present & kEnumerable) merged.flags = (merged.flags & ~kEnumerable) | (p.flags & kEnumerable);
        if (present & kConfigurable) merged.flags = (merged.flags & ~kConfigurable) | (p.flags & kConfigurable);
        if (p.flags & kAccessor) {
            if (!(merged.flags & kAccessor)) merged = Property{Value::undefined(), nullptr, nullptr, static_cast<uint8_t>((merged.flags & (kEnumerable | kConfigurable)) | kAccessor)};
            if (present & 32) {
                const Value g = rt.get(desc, u"get"), s = rt.get(desc, u"set");
                if (rt.has(desc.asObject(), u"get")) merged.getter = g.isObject() ? g.asObject() : nullptr;
                if (rt.has(desc.asObject(), u"set")) merged.setter = s.isObject() ? s.asObject() : nullptr;
            }
        } else if (present & (16 | kWritable)) {
            if (merged.flags & kAccessor) merged = Property{Value::undefined(), nullptr, nullptr, static_cast<uint8_t>(merged.flags & (kEnumerable | kConfigurable))};
            if (present & 16) merged.value = p.value;
            if (present & kWritable) merged.flags = (merged.flags & ~kWritable) | (p.flags & kWritable);
        }
        *existing = merged;
        return;
    }
    if (!o->extensible) rt.throwError("TypeError", "can't add a property to a non-extensible object");
    o->define(key, p);
}

Value fromDescriptor(Runtime& rt, const Property& p) {
    Object* d = rt.newObject();
    if (p.flags & kAccessor) {
        rt.put(d, u"get", p.getter ? Value::object(p.getter) : Value::undefined(), kDefaultFlags);
        rt.put(d, u"set", p.setter ? Value::object(p.setter) : Value::undefined(), kDefaultFlags);
    } else {
        rt.put(d, u"value", p.value, kDefaultFlags);
        rt.put(d, u"writable", Value::boolean(p.flags & kWritable), kDefaultFlags);
    }
    rt.put(d, u"enumerable", Value::boolean(p.flags & kEnumerable), kDefaultFlags);
    rt.put(d, u"configurable", Value::boolean(p.flags & kConfigurable), kDefaultFlags);
    return Value::object(d);
}

// An own property, including an array's elements and a String's characters.
bool ownProperty(Runtime& rt, Object* o, const PropertyKey& key, Property& out) {
    if (o->cls == ObjectClass::Proxy) return proxyGetOwnProperty(rt, o, key, out);
    if (!key.isSymbol() && o->cls == ObjectClass::TypedArray) {
        double index;
        if (typedKey(key, index)) {
            if (!typedValidIndex(o, index)) return false;
            out = {typedGet(o, index), nullptr, nullptr, kDefaultFlags};
            return true;
        }
    }
    if (!key.isSymbol()) {
        if (o->cls == ObjectClass::Array && key.name == u"length") {
            out = {Value::number(static_cast<double>(o->elements.size())), nullptr, nullptr, static_cast<uint8_t>(o->length_readonly ? 0 : kWritable)};
            return true;
        }
        if (!key.name.empty() && std::all_of(key.name.begin(), key.name.end(), [](char16_t c) { return c >= u'0' && c <= u'9'; }) &&
            (key.name.size() == 1 || key.name[0] != u'0') && key.name.size() < 10) {
            const size_t i = std::stoul(utf8(key.name));
            if (i < o->elements.size() && !o->elements[i].isEmpty()) {
                out = {o->elements[i], nullptr, nullptr, static_cast<uint8_t>(o->extensible || !o->own(u"__frozen") ? kDefaultFlags : kEnumerable)};
                return true;
            }
            if (o->cls == ObjectClass::String && o->primitive.isString() && i < o->primitive.asString().size()) {
                out = {Value::string(std::u16string(1, o->primitive.asString()[i])), nullptr, nullptr, kEnumerable};
                return true;
            }
        }
        if (o->cls == ObjectClass::String && key.name == u"length") {
            out = {Value::number(static_cast<double>(o->primitive.asString().size())), nullptr, nullptr, 0};
            return true;
        }
    }
    if (const Property* p = o->own(key)) {
        out = *p;
        return true;
    }
    return false;
}

std::vector<PropertyKey> ownKeysOf(Runtime& rt, Object* o) {
    if (o->cls == ObjectClass::Proxy) return proxyOwnKeys(rt, o);
    std::vector<PropertyKey> keys = o->ownKeys();
    if (o->cls == ObjectClass::Array) keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(std::count_if(keys.begin(), keys.end(), [](const PropertyKey& k) {
                                                                    return !k.isSymbol() && !k.name.empty() && k.name[0] >= u'0' && k.name[0] <= u'9';
                                                                })),
                                                  PropertyKey(u"length"));
    if (o->cls == ObjectClass::String && o->primitive.isString()) {
        // A String's characters first, then length, then its other properties.
        std::vector<PropertyKey> front;
        for (size_t i = 0; i < o->primitive.asString().size(); ++i) front.emplace_back(Runtime::numberToString(static_cast<double>(i)));
        front.emplace_back(u"length");
        keys.insert(keys.begin(), front.begin(), front.end());
    }
    keys.erase(std::remove_if(keys.begin(), keys.end(), [](const PropertyKey& k) { return !k.isSymbol() && k.name == u"__frozen"; }), keys.end());
    return keys;
}

std::vector<PropertyKey> enumerableOwnKeys(Runtime& rt, Object* o) {
    std::vector<PropertyKey> out;
    for (const PropertyKey& k : ownKeysOf(rt, o)) {
        if (k.isSymbol()) continue;
        Property p;
        if (ownProperty(rt, o, k, p) && (p.flags & kEnumerable)) out.push_back(k);
    }
    return out;
}

}  // namespace

bool Runtime::getOwnProperty(Object* o, const PropertyKey& key, Property& out) { return ownProperty(*this, o, key, out); }
std::vector<PropertyKey> Runtime::ownPropertyKeys(Object* o) { return ownKeysOf(*this, o); }

bool Runtime::defineOwnProperty(Object* o, const PropertyKey& key, const Value& descriptor, bool throws) {
    if (o->cls == ObjectClass::Proxy) {
        if (!descriptor.isObject()) throwError("TypeError", "a property descriptor must be an object");
        const bool ok = proxyDefineOwnProperty(*this, o, key, descriptor);
        if (!ok && throws) throwError("TypeError", "the proxy refused to define " + utf8(keyString(key)));
        return ok;
    }
    if (throws) {
        defineFromDescriptor(*this, o, key, descriptor);
        return true;
    }
    // Reflect.defineProperty's way: false rather than an error for what can't be done.
    uint8_t present = 0;
    toDescriptor(*this, descriptor, present);   // a malformed descriptor still throws
    try {
        defineFromDescriptor(*this, o, key, descriptor);
    } catch (const Thrown&) {
        return false;
    }
    return true;
}

Property descriptorToProperty(Runtime& rt, const Value& desc) {
    uint8_t present = 0;
    return toDescriptor(rt, desc, present);
}

Value normalizeDescriptor(Runtime& rt, const Value& desc) {
    uint8_t present = 0;
    const Property p = toDescriptor(rt, desc, present);
    Object* d = rt.newObject();
    if (present & 16) rt.put(d, u"value", p.value, kDefaultFlags);
    if (present & kWritable) rt.put(d, u"writable", Value::boolean(p.flags & kWritable), kDefaultFlags);
    if (present & 32) {
        if (rt.has(desc.asObject(), u"get")) rt.put(d, u"get", p.getter ? Value::object(p.getter) : Value::undefined(), kDefaultFlags);
        if (rt.has(desc.asObject(), u"set")) rt.put(d, u"set", p.setter ? Value::object(p.setter) : Value::undefined(), kDefaultFlags);
    }
    if (present & kEnumerable) rt.put(d, u"enumerable", Value::boolean(p.flags & kEnumerable), kDefaultFlags);
    if (present & kConfigurable) rt.put(d, u"configurable", Value::boolean(p.flags & kConfigurable), kDefaultFlags);
    return Value::object(d);
}

Value propertyToDescriptor(Runtime& rt, const Property& p) { return fromDescriptor(rt, p); }

namespace {

// ── JSON ──

class JsonParser {
public:
    JsonParser(Runtime& rt, const std::u16string& s) : rt_(rt), s_(s) {}
    Value parse() {
        Value v = value(0);
        ws();
        if (i_ != s_.size()) fail();
        return v;
    }

private:
    [[noreturn]] void fail() { rt_.throwError("SyntaxError", "JSON.parse: unexpected input at position " + std::to_string(i_)); }
    void ws() {
        while (i_ < s_.size() && (s_[i_] == u' ' || s_[i_] == u'\t' || s_[i_] == u'\n' || s_[i_] == u'\r')) ++i_;
    }
    Value value(int depth) {
        if (depth > 512) rt_.throwError("RangeError", "JSON nested too deeply");
        ws();
        if (i_ >= s_.size()) fail();
        const char16_t c = s_[i_];
        if (c == u'{') {
            ++i_;
            Object* o = rt_.newObject();
            ws();
            if (i_ < s_.size() && s_[i_] == u'}') { ++i_; return Value::object(o); }
            while (true) {
                ws();
                if (i_ >= s_.size() || s_[i_] != u'"') fail();
                std::u16string k = string();
                ws();
                if (i_ >= s_.size() || s_[i_] != u':') fail();
                ++i_;
                const Value v = value(depth + 1);
                rt_.put(o, PropertyKey(k), v, kDefaultFlags);
                ws();
                if (i_ < s_.size() && s_[i_] == u',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == u'}') { ++i_; break; }
                fail();
            }
            return Value::object(o);
        }
        if (c == u'[') {
            ++i_;
            std::vector<Value> items;
            ws();
            if (i_ < s_.size() && s_[i_] == u']') { ++i_; return Value::object(rt_.newArray()); }
            while (true) {
                items.push_back(value(depth + 1));
                ws();
                if (i_ < s_.size() && s_[i_] == u',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == u']') { ++i_; break; }
                fail();
            }
            return Value::object(rt_.newArray(std::move(items)));
        }
        if (c == u'"') return Value::string(string());
        if (s_.compare(i_, 4, u"true") == 0) { i_ += 4; return Value::boolean(true); }
        if (s_.compare(i_, 5, u"false") == 0) { i_ += 5; return Value::boolean(false); }
        if (s_.compare(i_, 4, u"null") == 0) { i_ += 4; return Value::null(); }
        // A number, strictly as JSON writes it.
        const size_t start = i_;
        if (s_[i_] == u'-') ++i_;
        if (i_ >= s_.size()) fail();
        if (s_[i_] == u'0') ++i_;
        else if (s_[i_] >= u'1' && s_[i_] <= u'9') while (i_ < s_.size() && s_[i_] >= u'0' && s_[i_] <= u'9') ++i_;
        else fail();
        if (i_ < s_.size() && s_[i_] == u'.') {
            ++i_;
            if (i_ >= s_.size() || s_[i_] < u'0' || s_[i_] > u'9') fail();
            while (i_ < s_.size() && s_[i_] >= u'0' && s_[i_] <= u'9') ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == u'e' || s_[i_] == u'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == u'+' || s_[i_] == u'-')) ++i_;
            if (i_ >= s_.size() || s_[i_] < u'0' || s_[i_] > u'9') fail();
            while (i_ < s_.size() && s_[i_] >= u'0' && s_[i_] <= u'9') ++i_;
        }
        return Value::number(Runtime::stringToNumber(std::u16string_view(s_).substr(start, i_ - start)));
    }
    std::u16string string() {
        ++i_;
        std::u16string out;
        while (true) {
            if (i_ >= s_.size()) fail();
            const char16_t c = s_[i_++];
            if (c == u'"') break;
            if (c < 0x20) fail();
            if (c != u'\\') { out += c; continue; }
            if (i_ >= s_.size()) fail();
            const char16_t e = s_[i_++];
            switch (e) {
                case u'"': out += u'"'; break;
                case u'\\': out += u'\\'; break;
                case u'/': out += u'/'; break;
                case u'b': out += u'\b'; break;
                case u'f': out += u'\f'; break;
                case u'n': out += u'\n'; break;
                case u'r': out += u'\r'; break;
                case u't': out += u'\t'; break;
                case u'u': {
                    if (i_ + 4 > s_.size()) fail();
                    int v = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char16_t h = s_[i_++];
                        const int d = (h >= u'0' && h <= u'9') ? h - u'0' : (h >= u'a' && h <= u'f') ? h - u'a' + 10 : (h >= u'A' && h <= u'F') ? h - u'A' + 10 : -1;
                        if (d < 0) fail();
                        v = v * 16 + d;
                    }
                    out += static_cast<char16_t>(v);
                    break;
                }
                default: fail();
            }
        }
        return out;
    }
    Runtime& rt_;
    const std::u16string& s_;
    size_t i_ = 0;
};

std::u16string jsonQuote(const std::u16string& s) {
    std::u16string out = u"\"";
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < s.size(); ++i) {
        const char16_t c = s[i];
        switch (c) {
            case u'"': out += u"\\\""; break;
            case u'\\': out += u"\\\\"; break;
            case u'\b': out += u"\\b"; break;
            case u'\f': out += u"\\f"; break;
            case u'\n': out += u"\\n"; break;
            case u'\r': out += u"\\r"; break;
            case u'\t': out += u"\\t"; break;
            default: {
                const bool lone = (c >= 0xD800 && c <= 0xDBFF && !(i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)) ||
                                  (c >= 0xDC00 && c <= 0xDFFF && !(i > 0 && s[i - 1] >= 0xD800 && s[i - 1] <= 0xDBFF));
                if (c < 0x20 || lone) {
                    out += u"\\u";
                    for (int sh = 12; sh >= 0; sh -= 4) out += static_cast<char16_t>(hex[(c >> sh) & 15]);
                } else {
                    out += c;
                }
            }
        }
    }
    return out + u"\"";
}

struct JsonWriter {
    Runtime& rt;
    Value replacer_fn;
    std::vector<PropertyKey> allow;
    bool has_allow = false;
    std::u16string gap;
    std::vector<Object*> stack;

    bool write(const Value& holder, const PropertyKey& key, Value v, const std::u16string& indent, std::u16string& out) {
        if (v.isObject() || v.isBigInt()) {
            const Value to_json = rt.get(v, u"toJSON");
            if (rt.isCallable(to_json)) {
                const Value k = Value::string(key.name);
                v = rt.call(to_json, v, std::span<const Value>(&k, 1));
            }
        }
        if (!replacer_fn.isUndefined()) {
            const Value a[2] = {Value::string(key.name), v};
            v = rt.call(replacer_fn, holder, a);
        }
        if (v.isObject()) {
            Object* o = v.asObject();
            if (o->cls == ObjectClass::Number) v = Value::number(rt.toNumber(v));
            else if (o->cls == ObjectClass::String) v = Value::string(rt.toString(v));
            else if (o->cls == ObjectClass::Boolean || o->cls == ObjectClass::BigInt) v = o->primitive;
        }
        if (v.isBigInt()) rt.throwError("TypeError", "a BigInt can't be written as JSON");
        switch (v.type()) {
            case Value::Type::Null: out += u"null"; return true;
            case Value::Type::Boolean: out += v.asBoolean() ? u"true" : u"false"; return true;
            case Value::Type::String: out += jsonQuote(v.asString()); return true;
            case Value::Type::Number: out += std::isfinite(v.asNumber()) ? Runtime::numberToString(v.asNumber()) : u"null"; return true;
            case Value::Type::Object: break;
            default: return false;
        }
        if (rt.isCallable(v)) return false;
        Object* o = v.asObject();
        if (std::find(stack.begin(), stack.end(), o) != stack.end()) rt.throwError("TypeError", "JSON.stringify: a value refers to itself");
        if (stack.size() > 512) rt.throwError("RangeError", "JSON nested too deeply");
        stack.push_back(o);
        const std::u16string inner = indent + gap;
        if (rt.isArray(v)) {
            out += u'[';
            const uint64_t len = rt.lengthOf(o);
            for (uint64_t i = 0; i < len; ++i) {
                if (i) out += u',';
                if (!gap.empty()) out += u"\n" + inner;
                if (!write(v, PropertyKey(Runtime::numberToString(static_cast<double>(i))), elementAt(rt, o, i), inner, out)) out += u"null";
            }
            if (len && !gap.empty()) out += u"\n" + indent;
            out += u']';
        } else {
            out += u'{';
            bool any = false;
            const std::vector<PropertyKey> keys = has_allow ? allow : enumerableOwnKeys(rt, o);
            for (const PropertyKey& k : keys) {
                std::u16string piece = jsonQuote(k.name) + (gap.empty() ? u":" : u": ");
                if (!write(v, k, rt.get(o, k, v), inner, piece)) continue;
                if (any) out += u',';
                if (!gap.empty()) out += u"\n" + inner;
                out += piece;
                any = true;
            }
            if (any && !gap.empty()) out += u"\n" + indent;
            out += u'}';
        }
        stack.pop_back();
        return true;
    }
};

// ── Sorting ──

void sortValues(Runtime& rt, std::vector<Value>& items, const Value& cmp) {
    // undefined sorts last; the rest by the comparator, or by their strings.
    std::stable_sort(items.begin(), items.end(), [&](const Value& a, const Value& b) {
        if (a.isUndefined()) return false;
        if (b.isUndefined()) return true;
        if (!cmp.isUndefined()) {
            const Value args[2] = {a, b};
            const double r = rt.toNumber(rt.call(cmp, Value::undefined(), args));
            return r < 0;
        }
        return *rt.toString(a) < *rt.toString(b);
    });
}

std::u16string toCase(const std::u16string& s, bool upper) {
    std::u16string out = s;
    for (char16_t& c : out) {
        if (upper) {
            if (c >= u'a' && c <= u'z') c = static_cast<char16_t>(c - 32);
            else if (c >= 0xE0 && c <= 0xFE && c != 0xF7) c = static_cast<char16_t>(c - 32);
            else if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) c = static_cast<char16_t>(c - 32);
            else if (c >= 0x430 && c <= 0x44F) c = static_cast<char16_t>(c - 32);
        } else {
            if (c >= u'A' && c <= u'Z') c = static_cast<char16_t>(c + 32);
            else if (c >= 0xC0 && c <= 0xDE && c != 0xD7) c = static_cast<char16_t>(c + 32);
            else if (c >= 0x391 && c <= 0x3A9) c = static_cast<char16_t>(c + 32);
            else if (c >= 0x410 && c <= 0x42F) c = static_cast<char16_t>(c + 32);
        }
    }
    return out;
}

bool isWhite(char16_t c) { return isJsSpace(c) || isLineTerminator(c); }

std::u16string uriEncode(Runtime& rt, const std::u16string& s, const char* keep) {
    std::u16string out;
    static const char* hex = "0123456789ABCDEF";
    for (size_t i = 0; i < s.size(); ++i) {
        const char16_t c = s[i];
        if ((c < 128 && (std::isalnum(c) || std::strchr(keep, static_cast<char>(c)))) && c != 0) {
            out += c;
            continue;
        }
        char32_t cp = c;
        if (c >= 0xDC00 && c <= 0xDFFF) rt.throwError("URIError", "malformed URI sequence");
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= s.size() || s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF) rt.throwError("URIError", "malformed URI sequence");
            cp = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        }
        const std::string bytes = utf8(std::u16string(1, static_cast<char16_t>(0)).empty() ? u"" : u"");
        std::u16string one;
        if (cp >= 0x10000) {
            one += static_cast<char16_t>(0xD800 + ((cp - 0x10000) >> 10));
            one += static_cast<char16_t>(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
            one += static_cast<char16_t>(cp);
        }
        (void)bytes;
        for (unsigned char b : utf8(one)) {
            out += u'%';
            out += static_cast<char16_t>(hex[b >> 4]);
            out += static_cast<char16_t>(hex[b & 15]);
        }
    }
    return out;
}

std::u16string uriDecode(Runtime& rt, const std::u16string& s, const char* reserved) {
    std::string bytes;
    std::u16string out;
    const auto flush = [&] {
        if (bytes.empty()) return;
        const std::u16string decoded = utf16(bytes);
        if (decoded.find(u'�') != std::u16string::npos && bytes.find("\xEF\xBF\xBD") == std::string::npos) rt.throwError("URIError", "malformed URI sequence");
        out += decoded;
        bytes.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != u'%') {
            flush();
            out += s[i];
            continue;
        }
        const auto hexv = [](char16_t h) { return (h >= u'0' && h <= u'9') ? h - u'0' : (h >= u'a' && h <= u'f') ? h - u'a' + 10 : (h >= u'A' && h <= u'F') ? h - u'A' + 10 : -1; };
        if (i + 2 >= s.size() || hexv(s[i + 1]) < 0 || hexv(s[i + 2]) < 0) rt.throwError("URIError", "malformed URI sequence");
        const char b = static_cast<char>(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
        if (static_cast<unsigned char>(b) < 0x80 && std::strchr(reserved, b) && b != 0) {
            flush();
            out += s.substr(i, 3);
        } else {
            bytes += b;
        }
        i += 2;
    }
    flush();
    return out;
}

}  // namespace

// ── Setting up ────────────────────────────────────────────────────────────────

Runtime::Runtime() {
    object_prototype = allocate<Object>(nullptr);
    function_prototype = allocate<Object>(object_prototype, ObjectClass::Function);
    function_prototype->native = [](Runtime&, const Value&, Args) { return Value::undefined(); };
    global_ = allocate<Object>(object_prototype);
    global_env_ = allocate<Env>(nullptr);
    global_env_->global_object = global_;
    installBuiltins();
    // XWEB_GC_STRESS=n: collect mid-script whenever the heap has grown by n cells (1:
    // at nearly every safe point), to shake out anything the collector can't see.
    if (const char* stress = std::getenv("XWEB_GC_STRESS"); stress && *stress) setGcGrowth(std::strtoull(stress, nullptr, 10));
}

void Runtime::installBuiltins() {
    Runtime& rt = *this;
    const auto ctor = [&](const char16_t* name, int length, NativeFunction fn, Object* proto) {
        Object* c = newFunction(name, length, std::move(fn), true);
        put(c, u"prototype", Value::object(proto), 0);
        put(proto, u"constructor", Value::object(c));
        put(global_, name, Value::object(c));
        return c;
    };
    const auto getter = [&](Object* o, const PropertyKey& key, NativeFunction fn) {
        o->define(key, {Value::undefined(), newFunction(u"get", 0, std::move(fn)), nullptr, kConfigurable | kAccessor});
    };

    // Symbols first: everything else names them.
    sym_iterator = newSymbol(u"Symbol.iterator");
    sym_to_primitive = newSymbol(u"Symbol.toPrimitive");
    sym_to_string_tag = newSymbol(u"Symbol.toStringTag");
    sym_has_instance = newSymbol(u"Symbol.hasInstance");

    put(global_, u"globalThis", Value::object(global_));
    put(global_, u"NaN", Value::number(std::nan("")), 0);
    put(global_, u"Infinity", Value::number(INFINITY), 0);
    put(global_, u"undefined", Value::undefined(), 0);

    // ── Object ──
    Object* object_ctor = ctor(u"Object", 1, [](Runtime& rt, const Value&, Args a) {
        const Value v = arg(a, 0);
        if (v.isNullish()) return Value::object(rt.newObject());
        return Value::object(rt.toObject(v));
    }, object_prototype);
    method(object_ctor, u"keys", 1, [](Runtime& rt, const Value&, Args a) {
        Object* o = rt.toObject(arg(a, 0));
        std::vector<Value> out;
        for (const PropertyKey& k : enumerableOwnKeys(rt, o)) out.push_back(Value::string(k.name));
        return Value::object(rt.newArray(std::move(out)));
    });
    method(object_ctor, u"values", 1, [](Runtime& rt, const Value&, Args a) {
        Object* o = rt.toObject(arg(a, 0));
        std::vector<Value> out;
        for (const PropertyKey& k : enumerableOwnKeys(rt, o)) out.push_back(rt.get(o, k, Value::object(o)));
        return Value::object(rt.newArray(std::move(out)));
    });
    method(object_ctor, u"entries", 1, [](Runtime& rt, const Value&, Args a) {
        Object* o = rt.toObject(arg(a, 0));
        std::vector<Value> out;
        for (const PropertyKey& k : enumerableOwnKeys(rt, o))
            out.push_back(Value::object(rt.newArray({Value::string(k.name), rt.get(o, k, Value::object(o))})));
        return Value::object(rt.newArray(std::move(out)));
    });
    method(object_ctor, u"fromEntries", 1, [](Runtime& rt, const Value&, Args a) {
        Object* o = rt.newObject();
        for (const Value& e : rt.iterateToList(arg(a, 0))) {
            if (!e.isObject()) rt.throwError("TypeError", "an entry must be an object");
            rt.put(o, rt.toPropertyKey(rt.get(e, u"0")), rt.get(e, u"1"), kDefaultFlags);
        }
        return Value::object(o);
    });
    method(object_ctor, u"assign", 2, [](Runtime& rt, const Value&, Args a) {
        Object* target = rt.toObject(arg(a, 0));
        for (size_t i = 1; i < a.size(); ++i) {
            if (a[i].isNullish()) continue;
            Object* src = rt.toObject(a[i]);
            for (const PropertyKey& k : ownKeysOf(rt, src)) {
                Property p;
                if (ownProperty(rt, src, k, p) && (p.flags & kEnumerable)) rt.set(Value::object(target), k, rt.get(src, k, a[i]), true);
            }
        }
        return Value::object(target);
    });
    method(object_ctor, u"create", 2, [](Runtime& rt, const Value&, Args a) {
        const Value p = arg(a, 0);
        if (!p.isObject() && !p.isNull()) rt.throwError("TypeError", "Object.create needs an object or null");
        Object* o = rt.allocate<Object>(p.isObject() ? p.asObject() : nullptr);
        if (!arg(a, 1).isUndefined()) {
            Object* props = rt.toObject(arg(a, 1));
            for (const PropertyKey& k : enumerableOwnKeys(rt, props)) defineFromDescriptor(rt, o, k, rt.get(props, k, Value::object(props)));
        }
        return Value::object(o);
    });
    method(object_ctor, u"defineProperty", 3, [](Runtime& rt, const Value&, Args a) {
        if (!arg(a, 0).isObject()) rt.throwError("TypeError", "Object.defineProperty needs an object");
        defineFromDescriptor(rt, arg(a, 0).asObject(), rt.toPropertyKey(arg(a, 1)), arg(a, 2));
        return arg(a, 0);
    });
    method(object_ctor, u"defineProperties", 2, [](Runtime& rt, const Value&, Args a) {
        if (!arg(a, 0).isObject()) rt.throwError("TypeError", "Object.defineProperties needs an object");
        Object* props = rt.toObject(arg(a, 1));
        for (const PropertyKey& k : enumerableOwnKeys(rt, props)) defineFromDescriptor(rt, arg(a, 0).asObject(), k, rt.get(props, k, Value::object(props)));
        return arg(a, 0);
    });
    method(object_ctor, u"getOwnPropertyNames", 1, [](Runtime& rt, const Value&, Args a) {
        Object* o = rt.toObject(arg(a, 0));
        std::vector<Value> out;
        for (const PropertyKey& k : ownKeysOf(rt, o))
            if (!k.isSymbol()) out.push_back(Value::string(k.name));
        return Value::object(rt.newArray(std::move(out)));
    });
    method(object_ctor, u"getOwnPropertySymbols", 1, [](Runtime& rt, const Value&, Args a) {
        Object* o = rt.toObject(arg(a, 0));
        std::vector<Value> out;
        for (const PropertyKey& k : ownKeysOf(rt, o))
            if (k.isSymbol()) out.push_back(Value::symbol(k.symbol));
        return Value::object(rt.newArray(std::move(out)));
    });
    method(object_ctor, u"getOwnPropertyDescriptor", 2, [](Runtime& rt, const Value&, Args a) {
        Object* o = rt.toObject(arg(a, 0));
        Property p;
        if (!ownProperty(rt, o, rt.toPropertyKey(arg(a, 1)), p)) return Value::undefined();
        return fromDescriptor(rt, p);
    });
    method(object_ctor, u"getOwnPropertyDescriptors", 1, [](Runtime& rt, const Value&, Args a) {
        Object* o = rt.toObject(arg(a, 0));
        Object* out = rt.newObject();
        for (const PropertyKey& k : ownKeysOf(rt, o)) {
            Property p;
            if (ownProperty(rt, o, k, p)) rt.put(out, k, fromDescriptor(rt, p), kDefaultFlags);
        }
        return Value::object(out);
    });
    method(object_ctor, u"getPrototypeOf", 1, [](Runtime& rt, const Value&, Args a) {
        Object* o = rt.toObject(arg(a, 0));
        Object* p = rt.getPrototypeOf(o);
        return p ? Value::object(p) : Value::null();
    });
    method(object_ctor, u"setPrototypeOf", 2, [](Runtime& rt, const Value&, Args a) {
        const Value p = arg(a, 1);
        if (arg(a, 0).isNullish()) rt.throwError("TypeError", "Object.setPrototypeOf called on null or undefined");
        if (!p.isObject() && !p.isNull()) rt.throwError("TypeError", "a prototype must be an object or null");
        if (!arg(a, 0).isObject()) return arg(a, 0);
        if (!rt.setPrototypeOf(arg(a, 0).asObject(), p.isObject() ? p.asObject() : nullptr))
            rt.throwError("TypeError", "the prototype can't be set (a cycle, or the object isn't extensible)");
        return arg(a, 0);
    });
    method(object_ctor, u"is", 2, [](Runtime&, const Value&, Args a) { return Value::boolean(Runtime::sameValue(arg(a, 0), arg(a, 1))); });
    const auto freezeLike = [](bool freeze) {
        return [freeze](Runtime& rt, const Value&, Args a) {
            if (!arg(a, 0).isObject()) return arg(a, 0);
            Object* o = arg(a, 0).asObject();
            if (o->cls == ObjectClass::Proxy) {
                // SetIntegrityLevel, through the proxy's handler.
                if (!rt.preventExtensions(o)) rt.throwError("TypeError", "the proxy refused to become non-extensible");
                for (const PropertyKey& k : rt.ownPropertyKeys(o)) {
                    Object* d = rt.newObject();
                    rt.put(d, u"configurable", Value::boolean(false), kDefaultFlags);
                    if (freeze) {
                        Property cur;
                        if (rt.getOwnProperty(o, k, cur) && !(cur.flags & kAccessor)) rt.put(d, u"writable", Value::boolean(false), kDefaultFlags);
                    }
                    rt.defineOwnProperty(o, k, Value::object(d), true);
                }
                return arg(a, 0);
            }
            o->extensible = false;
            if (o->cls == ObjectClass::Array && freeze) rt.put(o, u"__frozen", Value::boolean(true), 0);
            for (const PropertyKey& k : o->ownKeys()) {
                if (Property* p = o->own(k)) {
                    p->flags &= ~kConfigurable;
                    if (freeze && !(p->flags & kAccessor)) p->flags &= ~kWritable;
                }
            }
            if (freeze && o->cls != ObjectClass::Array) {
                // Elements of an array-like non-array stay as properties already.
            }
            return arg(a, 0);
        };
    };
    method(object_ctor, u"freeze", 1, freezeLike(true));
    method(object_ctor, u"seal", 1, freezeLike(false));
    method(object_ctor, u"preventExtensions", 1, [](Runtime& rt, const Value&, Args a) {
        if (arg(a, 0).isObject() && !rt.preventExtensions(arg(a, 0).asObject())) rt.throwError("TypeError", "the object refused to become non-extensible");
        return arg(a, 0);
    });
    method(object_ctor, u"isExtensible", 1, [](Runtime& rt, const Value&, Args a) { return Value::boolean(arg(a, 0).isObject() && rt.isExtensible(arg(a, 0).asObject())); });
    method(object_ctor, u"isFrozen", 1, [](Runtime& rt, const Value&, Args a) {
        if (!arg(a, 0).isObject()) return Value::boolean(true);
        Object* o = arg(a, 0).asObject();
        if (rt.isExtensible(o)) return Value::boolean(false);
        if (o->cls == ObjectClass::Proxy) {
            for (const PropertyKey& k : rt.ownPropertyKeys(o)) {
                Property p;
                if (rt.getOwnProperty(o, k, p) && ((p.flags & kConfigurable) || (!(p.flags & kAccessor) && (p.flags & kWritable)))) return Value::boolean(false);
            }
            return Value::boolean(true);
        }
        if (o->cls == ObjectClass::Array && !o->elements.empty() && !o->own(u"__frozen")) return Value::boolean(false);
        for (const PropertyKey& k : o->ownKeys()) {
            Property p;
            if (ownProperty(rt, o, k, p) && ((p.flags & kConfigurable) || (!(p.flags & kAccessor) && (p.flags & kWritable)))) return Value::boolean(false);
        }
        return Value::boolean(true);
    });
    method(object_ctor, u"isSealed", 1, [](Runtime& rt, const Value&, Args a) {
        if (!arg(a, 0).isObject()) return Value::boolean(true);
        Object* o = arg(a, 0).asObject();
        if (rt.isExtensible(o)) return Value::boolean(false);
        if (o->cls == ObjectClass::Proxy) {
            for (const PropertyKey& k : rt.ownPropertyKeys(o)) {
                Property p;
                if (rt.getOwnProperty(o, k, p) && (p.flags & kConfigurable)) return Value::boolean(false);
            }
            return Value::boolean(true);
        }
        for (const PropertyKey& k : o->ownKeys())
            if (const Property* p = o->own(k); p && (p->flags & kConfigurable)) return Value::boolean(false);
        return Value::boolean(true);
    });

    method(object_prototype, u"hasOwnProperty", 1, [](Runtime& rt, const Value& t, Args a) {
        const PropertyKey k = rt.toPropertyKey(arg(a, 0));
        Property p;
        return Value::boolean(ownProperty(rt, thisObject(rt, t), k, p));
    });
    method(object_ctor, u"hasOwn", 2, [](Runtime& rt, const Value&, Args a) {
        Property p;
        Object* o = rt.toObject(arg(a, 0));
        return Value::boolean(ownProperty(rt, o, rt.toPropertyKey(arg(a, 1)), p));
    });
    method(object_prototype, u"isPrototypeOf", 1, [](Runtime& rt, const Value& t, Args a) {
        if (!arg(a, 0).isObject()) return Value::boolean(false);
        Object* self = thisObject(rt, t);
        for (Object* o = rt.getPrototypeOf(arg(a, 0).asObject()); o; o = rt.getPrototypeOf(o))
            if (o == self) return Value::boolean(true);
        return Value::boolean(false);
    });
    method(object_prototype, u"propertyIsEnumerable", 1, [](Runtime& rt, const Value& t, Args a) {
        Property p;
        return Value::boolean(ownProperty(rt, thisObject(rt, t), rt.toPropertyKey(arg(a, 0)), p) && (p.flags & kEnumerable));
    });
    method(object_prototype, u"toString", 0, [](Runtime& rt, const Value& t, Args) {
        if (t.isUndefined()) return Value::string(u"[object Undefined]");
        if (t.isNull()) return Value::string(u"[object Null]");
        Object* o = rt.toObject(t);
        const Value tag = rt.get(o, PropertyKey(rt.sym_to_string_tag), t);
        if (tag.isString()) return Value::string(u"[object " + tag.asString() + u"]");
        const char16_t* name = u"Object";
        if (rt.isArray(Value::object(o))) name = u"Array";
        else if (o->cls == ObjectClass::Proxy && rt.isCallable(Value::object(o))) name = u"Function";
        else switch (o->cls) {
            case ObjectClass::Array: name = u"Array"; break;
            case ObjectClass::Function: name = u"Function"; break;
            case ObjectClass::Error: name = u"Error"; break;
            case ObjectClass::Boolean: name = u"Boolean"; break;
            case ObjectClass::Number: name = u"Number"; break;
            case ObjectClass::String: name = u"String"; break;
            case ObjectClass::Arguments: name = u"Arguments"; break;
            case ObjectClass::Date: name = u"Date"; break;
            case ObjectClass::RegExp: name = u"RegExp"; break;
            default: break;
        }
        return Value::string(std::u16string(u"[object ") + name + u"]");
    });
    method(object_prototype, u"toLocaleString", 0, [](Runtime& rt, const Value& t, Args) { return rt.call(rt.get(t, u"toString"), t, {}); });
    method(object_prototype, u"valueOf", 0, [](Runtime& rt, const Value& t, Args) { return Value::object(rt.toObject(t)); });
    object_prototype->define(u"__proto__", {Value::undefined(),
        newFunction(u"get __proto__", 0, [](Runtime& rt, const Value& t, Args) {
            Object* o = rt.toObject(t);
            Object* p = rt.getPrototypeOf(o);
            return p ? Value::object(p) : Value::null();
        }),
        newFunction(u"set __proto__", 1, [](Runtime& rt, const Value& t, Args a) {
            if (t.isNullish()) rt.throwError("TypeError", "set __proto__ called on null or undefined");
            const Value p = arg(a, 0);
            if (t.isObject() && (p.isObject() || p.isNull()) && !rt.setPrototypeOf(t.asObject(), p.isObject() ? p.asObject() : nullptr))
                rt.throwError("TypeError", "the prototype can't be set");
            return Value::undefined();
        }),
        kConfigurable | kAccessor});

    // ── Function ──
    // Functions from source, in the global scope; and eval (a direct call in calls).
    ctor(u"Function", 1, [](Runtime& rt, const Value&, Args a) { return dynamicFunction(rt, a, "function"); }, function_prototype);
    eval_function = newFunction(u"eval", 1, [](Runtime& rt, const Value&, Args a) {
        if (a.empty() || !a[0].isString()) return arg(a, 0);
        return evalCode(rt, a[0].asString(), nullptr, false, false);
    });
    put(global_, u"eval", Value::object(eval_function));
    put(function_prototype, u"length", Value::number(0), kConfigurable);
    put(function_prototype, u"name", Value::string(u""), kConfigurable);
    // %ThrowTypeError%: caller and arguments, which strict code may not reach for.
    {
        Object* thrower = newFunction(u"", 0, [](Runtime& rt, const Value&, Args) -> Value {
            rt.throwError("TypeError", "'caller' and 'arguments' can't be used on strict functions or their calls");
        });
        thrower->extensible = false;
        for (const char16_t* name : {u"caller", u"arguments"})
            function_prototype->define(name, {Value::undefined(), thrower, thrower, kConfigurable | kAccessor});
    }
    method(function_prototype, u"call", 1, [](Runtime& rt, const Value& t, Args a) {
        return rt.call(t, arg(a, 0), a.size() > 1 ? a.subspan(1) : Args{});
    });
    method(function_prototype, u"apply", 2, [](Runtime& rt, const Value& t, Args a) {
        std::vector<Value> list;
        if (!arg(a, 1).isNullish()) {
            if (!arg(a, 1).isObject()) rt.throwError("TypeError", "apply's arguments must be an array-like object");
            Object* o = arg(a, 1).asObject();
            const uint64_t n = rt.lengthOf(o);
            if (n > 1'000'000) rt.throwError("RangeError", "too many arguments");
            for (uint64_t i = 0; i < n; ++i) list.push_back(elementAt(rt, o, i));
        }
        return rt.call(t, arg(a, 0), list);
    });
    method(function_prototype, u"bind", 1, [](Runtime& rt, const Value& t, Args a) {
        if (!rt.isCallable(t)) rt.throwError("TypeError", "bind needs a function");
        Object* target = t.asObject();
        Object* b = rt.allocate<Object>(target->prototype, ObjectClass::Function);
        b->bound_target = target;
        b->bound_this = arg(a, 0);
        if (a.size() > 1) b->bound_args.assign(a.begin() + 1, a.end());
        b->constructor = target->constructor;
        const double len = std::max(0.0, rt.toIntegerOrInfinity(rt.get(t, u"length")) - static_cast<double>(b->bound_args.size()));
        rt.put(b, u"length", Value::number(len), kConfigurable);
        const Value name = rt.get(t, u"name");
        rt.put(b, u"name", Value::string(u"bound " + (name.isString() ? name.asString() : std::u16string())), kConfigurable);
        return Value::object(b);
    });
    method(function_prototype, u"toString", 0, [](Runtime& rt, const Value& t, Args) {
        if (!rt.isCallable(t)) rt.throwError("TypeError", "Function.prototype.toString needs a function");
        Object* f = t.asObject();
        if (f->code && !f->code->info->source.empty()) return Value::string(f->code->info->source);
        const Value name = rt.get(t, u"name");
        return Value::string(u"function " + (name.isString() ? name.asString() : std::u16string()) + u"() { [native code] }");
    });
    function_prototype->define(PropertyKey(sym_has_instance), {Value::object(newFunction(u"[Symbol.hasInstance]", 1, [](Runtime& rt, const Value& t, Args a) {
        if (!rt.isCallable(t) || !arg(a, 0).isObject()) return Value::boolean(false);
        Object* f = t.asObject();
        while (f->bound_target) f = f->bound_target;
        const Value proto = rt.get(Value::object(f), u"prototype");
        if (!proto.isObject()) rt.throwError("TypeError", "the function's prototype isn't an object");
        for (Object* o = rt.getPrototypeOf(arg(a, 0).asObject()); o; o = rt.getPrototypeOf(o))
            if (o == proto.asObject()) return Value::boolean(true);
        return Value::boolean(false);
    })), nullptr, nullptr, 0});

    // ── Errors ──
    error_prototype = newObject();
    const auto errorCtor = [&](const char* kind, Object* proto, Object* parent_ctor) {
        const std::string k = kind;
        Object* c = ctor(utf16(kind).c_str(), 1, [k](Runtime& rt, const Value&, Args a) {
            Object* nt = rt.construct_target;
            Object* proto = nullptr;
            if (nt) {
                const Value p = rt.get(Value::object(nt), u"prototype");
                if (p.isObject()) proto = p.asObject();
            }
            const auto it = rt.error_prototypes.find(k);
            Object* e = rt.allocate<Object>(proto ? proto : it->second, ObjectClass::Error);
            if (!arg(a, 0).isUndefined()) rt.put(e, u"message", Value::string(rt.toString(arg(a, 0))));
            const Value opts = arg(a, 1);
            if (opts.isObject() && rt.has(opts.asObject(), u"cause")) rt.put(e, u"cause", rt.get(opts, u"cause"));
            rt.put(e, u"stack", Value::string(utf16(k) + (arg(a, 0).isUndefined() ? u"" : u": " + *rt.toString(arg(a, 0))) + stackLine()));
            return Value::object(e);
        }, proto);
        if (parent_ctor) c->prototype = parent_ctor;
        put(proto, u"name", Value::string(utf16(kind)));
        put(proto, u"message", Value::string(u""));
        error_prototypes[kind] = proto;
        return c;
    };
    Object* error_ctor = errorCtor("Error", error_prototype, nullptr);
    method(error_prototype, u"toString", 0, [](Runtime& rt, const Value& t, Args) {
        if (!t.isObject()) rt.throwError("TypeError", "Error.prototype.toString needs an object");
        const Value n = rt.get(t, u"name"), m = rt.get(t, u"message");
        const std::u16string name = n.isUndefined() ? u"Error" : *rt.toString(n);
        const std::u16string msg = m.isUndefined() ? u"" : *rt.toString(m);
        if (name.empty()) return Value::string(msg);
        if (msg.empty()) return Value::string(name);
        return Value::string(name + u": " + msg);
    });
    for (const char* kind : {"TypeError", "RangeError", "SyntaxError", "ReferenceError", "EvalError", "URIError"})
        errorCtor(kind, allocate<Object>(error_prototype), error_ctor);

    // ── Symbol ──
    symbol_prototype = newObject();
    Object* symbol_ctor = ctor(u"Symbol", 0, [](Runtime& rt, const Value&, Args a) -> Value {
        if (rt.construct_target) rt.throwError("TypeError", "Symbol isn't a constructor");
        const Value d = arg(a, 0);
        return Value::symbol(rt.newSymbol(d.isUndefined() ? u"" : *rt.toString(d), !d.isUndefined()));
    }, symbol_prototype);
    symbol_ctor->constructor = true;   // so `new Symbol` reaches the TypeError above
    put(symbol_ctor, u"iterator", Value::symbol(sym_iterator), 0);
    put(symbol_ctor, u"toPrimitive", Value::symbol(sym_to_primitive), 0);
    put(symbol_ctor, u"toStringTag", Value::symbol(sym_to_string_tag), 0);
    put(symbol_ctor, u"hasInstance", Value::symbol(sym_has_instance), 0);
    sym_match = newSymbol(u"Symbol.match");
    sym_match_all = newSymbol(u"Symbol.matchAll");
    sym_replace = newSymbol(u"Symbol.replace");
    sym_search = newSymbol(u"Symbol.search");
    sym_split = newSymbol(u"Symbol.split");
    put(symbol_ctor, u"match", Value::symbol(sym_match), 0);
    put(symbol_ctor, u"matchAll", Value::symbol(sym_match_all), 0);
    put(symbol_ctor, u"replace", Value::symbol(sym_replace), 0);
    put(symbol_ctor, u"search", Value::symbol(sym_search), 0);
    put(symbol_ctor, u"split", Value::symbol(sym_split), 0);
    sym_species = newSymbol(u"Symbol.species");
    put(symbol_ctor, u"species", Value::symbol(sym_species), 0);
    sym_async_iterator = newSymbol(u"Symbol.asyncIterator");
    put(symbol_ctor, u"asyncIterator", Value::symbol(sym_async_iterator), 0);
    sym_unscopables = newSymbol(u"Symbol.unscopables");
    put(symbol_ctor, u"unscopables", Value::symbol(sym_unscopables), 0);
    put(symbol_ctor, u"isConcatSpreadable", Value::symbol(newSymbol(u"Symbol.isConcatSpreadable")), 0);
    method(symbol_ctor, u"for", 1, [](Runtime& rt, const Value&, Args a) {
        const std::u16string key = *rt.toString(arg(a, 0));
        auto& s = rt.symbol_registry[key];
        if (!s) s = rt.newSymbol(key);
        return Value::symbol(s);
    });
    method(symbol_ctor, u"keyFor", 1, [](Runtime& rt, const Value&, Args a) {
        if (!arg(a, 0).isSymbol()) rt.throwError("TypeError", "Symbol.keyFor needs a symbol");
        for (auto& [k, s] : rt.symbol_registry)
            if (s == arg(a, 0).asSymbol()) return Value::string(k);
        return Value::undefined();
    });
    const auto thisSymbol = [](Runtime& rt, const Value& t) -> Symbol* {
        if (t.isSymbol()) return t.asSymbol();
        if (t.isObject() && t.asObject()->cls == ObjectClass::Symbol) return t.asObject()->primitive.asSymbol();
        rt.throwError("TypeError", "not a symbol");
    };
    method(symbol_prototype, u"toString", 0, [thisSymbol](Runtime& rt, const Value& t, Args) { return Value::string(u"Symbol(" + thisSymbol(rt, t)->description + u")"); });
    method(symbol_prototype, u"valueOf", 0, [thisSymbol](Runtime& rt, const Value& t, Args) { return Value::symbol(thisSymbol(rt, t)); });
    getter(symbol_prototype, u"description", [thisSymbol](Runtime& rt, const Value& t, Args) {
        Symbol* s = thisSymbol(rt, t);
        return s->has_description ? Value::string(s->description) : Value::undefined();
    });
    put(symbol_prototype, PropertyKey(sym_to_string_tag), Value::string(u"Symbol"), kConfigurable);

    // ── Boolean ──
    boolean_prototype = allocate<Object>(object_prototype, ObjectClass::Boolean);
    boolean_prototype->primitive = Value::boolean(false);
    ctor(u"Boolean", 1, [](Runtime& rt, const Value&, Args a) {
        const Value b = Value::boolean(rt.toBoolean(arg(a, 0)));
        if (!rt.construct_target) return b;
        Object* o = rt.toObject(b);
        return Value::object(o);
    }, boolean_prototype);
    const auto thisBool = [](Runtime& rt, const Value& t) {
        if (t.isBoolean()) return t.asBoolean();
        if (t.isObject() && t.asObject()->cls == ObjectClass::Boolean) return t.asObject()->primitive.asBoolean();
        rt.throwError("TypeError", "not a boolean");
    };
    method(boolean_prototype, u"toString", 0, [thisBool](Runtime& rt, const Value& t, Args) { return Value::string(thisBool(rt, t) ? u"true" : u"false"); });
    method(boolean_prototype, u"valueOf", 0, [thisBool](Runtime& rt, const Value& t, Args) { return Value::boolean(thisBool(rt, t)); });

    // ── Number ──
    number_prototype = allocate<Object>(object_prototype, ObjectClass::Number);
    number_prototype->primitive = Value::number(0);
    Object* number_ctor = ctor(u"Number", 1, [](Runtime& rt, const Value&, Args a) {
        // Number(1n) is the one place a BigInt becomes a number.
        Value prim = a.empty() ? Value::number(0) : rt.toPrimitive(a[0], "number");
        const Value n = Value::number(prim.isBigInt() ? bigintToNumber(prim) : rt.toNumber(prim));
        if (!rt.construct_target) return n;
        return Value::object(rt.toObject(n));
    }, number_prototype);
    put(number_ctor, u"MAX_SAFE_INTEGER", Value::number(9007199254740991.0), 0);
    put(number_ctor, u"MIN_SAFE_INTEGER", Value::number(-9007199254740991.0), 0);
    put(number_ctor, u"MAX_VALUE", Value::number(std::numeric_limits<double>::max()), 0);
    put(number_ctor, u"MIN_VALUE", Value::number(std::numeric_limits<double>::denorm_min()), 0);
    put(number_ctor, u"EPSILON", Value::number(std::numeric_limits<double>::epsilon()), 0);
    put(number_ctor, u"POSITIVE_INFINITY", Value::number(INFINITY), 0);
    put(number_ctor, u"NEGATIVE_INFINITY", Value::number(-INFINITY), 0);
    put(number_ctor, u"NaN", Value::number(std::nan("")), 0);
    method(number_ctor, u"isNaN", 1, [](Runtime&, const Value&, Args a) { return Value::boolean(arg(a, 0).isNumber() && std::isnan(arg(a, 0).asNumber())); });
    method(number_ctor, u"isFinite", 1, [](Runtime&, const Value&, Args a) { return Value::boolean(arg(a, 0).isNumber() && std::isfinite(arg(a, 0).asNumber())); });
    method(number_ctor, u"isInteger", 1, [](Runtime&, const Value&, Args a) {
        const Value v = arg(a, 0);
        return Value::boolean(v.isNumber() && std::isfinite(v.asNumber()) && std::trunc(v.asNumber()) == v.asNumber());
    });
    method(number_ctor, u"isSafeInteger", 1, [](Runtime&, const Value&, Args a) {
        const Value v = arg(a, 0);
        return Value::boolean(v.isNumber() && std::isfinite(v.asNumber()) && std::trunc(v.asNumber()) == v.asNumber() && std::fabs(v.asNumber()) <= 9007199254740991.0);
    });
    const auto thisNumber = [](Runtime& rt, const Value& t) {
        if (t.isNumber()) return t.asNumber();
        if (t.isObject() && t.asObject()->cls == ObjectClass::Number) return t.asObject()->primitive.asNumber();
        rt.throwError("TypeError", "not a number");
    };
    method(number_prototype, u"toString", 1, [thisNumber](Runtime& rt, const Value& t, Args a) {
        const double d = thisNumber(rt, t);
        int radix = 10;
        if (!arg(a, 0).isUndefined()) {
            const double r = rt.toIntegerOrInfinity(arg(a, 0));
            if (r < 2 || r > 36) rt.throwError("RangeError", "radix must be between 2 and 36");
            radix = static_cast<int>(r);
        }
        return Value::string(Runtime::numberToString(d, radix));
    });
    method(number_prototype, u"toLocaleString", 0, [thisNumber](Runtime& rt, const Value& t, Args) {
        // en-US grouping, up to three decimals.
        const double d = thisNumber(rt, t);
        if (!std::isfinite(d)) return Value::string(Runtime::numberToString(d));
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.3f", std::fabs(d));
        std::string s = buf;
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
        const size_t dot = s.find('.');
        std::string ip = s.substr(0, dot), fp = dot == std::string::npos ? "" : s.substr(dot);
        for (int i = static_cast<int>(ip.size()) - 3; i > 0; i -= 3) ip.insert(static_cast<size_t>(i), ",");
        return Value::string(utf16((d < 0 ? "-" : "") + ip + fp));
    });
    method(number_prototype, u"valueOf", 0, [thisNumber](Runtime& rt, const Value& t, Args) { return Value::number(thisNumber(rt, t)); });
    method(number_prototype, u"toFixed", 1, [thisNumber](Runtime& rt, const Value& t, Args a) {
        const double d = thisNumber(rt, t);
        const double f = rt.toIntegerOrInfinity(arg(a, 0));
        if (f < 0 || f > 100) rt.throwError("RangeError", "toFixed digits must be between 0 and 100");
        if (std::isnan(d)) return Value::string(u"NaN");
        if (std::fabs(d) >= 1e21) return Value::string(Runtime::numberToString(d));
        char buf[512];
        std::snprintf(buf, sizeof buf, "%.*f", static_cast<int>(f), d);
        std::string s = buf;
        if (s.size() > 1 && s[0] == '-' && std::all_of(s.begin() + 1, s.end(), [](char c) { return c == '0' || c == '.'; })) s.erase(0, 1);
        return Value::string(utf16(s));
    });
    method(number_prototype, u"toPrecision", 1, [thisNumber](Runtime& rt, const Value& t, Args a) {
        const double d = thisNumber(rt, t);
        if (arg(a, 0).isUndefined()) return Value::string(Runtime::numberToString(d));
        const double p = rt.toIntegerOrInfinity(arg(a, 0));
        if (!std::isfinite(d)) return Value::string(Runtime::numberToString(d));
        if (p < 1 || p > 100) rt.throwError("RangeError", "toPrecision digits must be between 1 and 100");
        char buf[512];
        const int e = d == 0 ? 0 : static_cast<int>(std::floor(std::log10(std::fabs(d))));
        if (e < -6 || e >= p) std::snprintf(buf, sizeof buf, "%.*e", static_cast<int>(p) - 1, d);
        else std::snprintf(buf, sizeof buf, "%.*f", std::max(0, static_cast<int>(p) - 1 - e), d);
        std::string s = buf;
        // C writes e+05; JS writes e+5.
        if (const size_t ep = s.find('e'); ep != std::string::npos) {
            std::string exp = s.substr(ep + 2);
            while (exp.size() > 1 && exp[0] == '0') exp.erase(0, 1);
            s = s.substr(0, ep + 2) + exp;
        }
        return Value::string(utf16(s));
    });
    method(number_prototype, u"toExponential", 1, [thisNumber](Runtime& rt, const Value& t, Args a) {
        const double d = thisNumber(rt, t);
        if (!std::isfinite(d)) return Value::string(Runtime::numberToString(d));
        char buf[512];
        if (arg(a, 0).isUndefined()) {
            const std::u16string s = Runtime::numberToString(d);
            std::snprintf(buf, sizeof buf, "%e", d);
        } else {
            const double f = rt.toIntegerOrInfinity(arg(a, 0));
            if (f < 0 || f > 100) rt.throwError("RangeError", "toExponential digits must be between 0 and 100");
            std::snprintf(buf, sizeof buf, "%.*e", static_cast<int>(f), d);
        }
        std::string s = buf;
        if (const size_t ep = s.find('e'); ep != std::string::npos) {
            std::string exp = s.substr(ep + 2);
            while (exp.size() > 1 && exp[0] == '0') exp.erase(0, 1);
            s = s.substr(0, ep + 2) + exp;
        }
        return Value::string(utf16(s));
    });

    // ── Global functions ──
    const auto parse_float = [](Runtime& rt, const Value&, Args a) {
        const std::u16string s = *rt.toString(arg(a, 0));
        size_t i = 0;
        while (i < s.size() && isWhite(s[i])) ++i;
        // The longest prefix that's a decimal literal.
        size_t best = 0;
        double value = std::nan("");
        for (size_t end = s.size(); end > i; --end) {
            const std::u16string_view piece = std::u16string_view(s).substr(i, end - i);
            if (piece.find_first_of(u"xXoObB") != std::u16string_view::npos) continue;
            const double d = Runtime::stringToNumber(piece);
            if (!std::isnan(d) && (piece.back() != u'.' || piece.size() > 1)) {
                best = end;
                value = d;
                break;
            }
            if (end - i > 400) end = i + 401;
        }
        (void)best;
        return Value::number(value);
    };
    const auto parse_int = [](Runtime& rt, const Value&, Args a) {
        const std::u16string s = *rt.toString(arg(a, 0));
        size_t i = 0;
        while (i < s.size() && isWhite(s[i])) ++i;
        bool neg = false;
        if (i < s.size() && (s[i] == u'+' || s[i] == u'-')) neg = s[i++] == u'-';
        int radix = rt.toInt32(arg(a, 1));
        bool strip = true;
        if (radix != 0) {
            if (radix < 2 || radix > 36) return Value::number(std::nan(""));
            if (radix != 16) strip = false;
        } else {
            radix = 10;
        }
        if (strip && i + 1 < s.size() && s[i] == u'0' && (s[i + 1] == u'x' || s[i + 1] == u'X')) {
            i += 2;
            radix = 16;
        }
        double v = 0;
        bool any = false;
        for (; i < s.size(); ++i) {
            const char16_t c = s[i];
            const int d = (c >= u'0' && c <= u'9') ? c - u'0' : (c >= u'a' && c <= u'z') ? c - u'a' + 10 : (c >= u'A' && c <= u'Z') ? c - u'A' + 10 : 99;
            if (d >= radix) break;
            v = v * radix + d;
            any = true;
        }
        if (!any) return Value::number(std::nan(""));
        return Value::number(neg ? -v : v);
    };
    method(global_, u"parseFloat", 1, parse_float);
    method(global_, u"parseInt", 2, parse_int);
    put(number_ctor, u"parseFloat", get(global_, u"parseFloat", Value::object(global_)));
    put(number_ctor, u"parseInt", get(global_, u"parseInt", Value::object(global_)));
    method(global_, u"isNaN", 1, [](Runtime& rt, const Value&, Args a) { return Value::boolean(std::isnan(rt.toNumber(arg(a, 0)))); });
    method(global_, u"isFinite", 1, [](Runtime& rt, const Value&, Args a) { return Value::boolean(std::isfinite(rt.toNumber(arg(a, 0)))); });
    method(global_, u"encodeURIComponent", 1, [](Runtime& rt, const Value&, Args a) { return Value::string(uriEncode(rt, *rt.toString(arg(a, 0)), "-_.!~*'()")); });
    method(global_, u"encodeURI", 1, [](Runtime& rt, const Value&, Args a) { return Value::string(uriEncode(rt, *rt.toString(arg(a, 0)), "-_.!~*'();/?:@&=+$,#")); });
    method(global_, u"decodeURIComponent", 1, [](Runtime& rt, const Value&, Args a) { return Value::string(uriDecode(rt, *rt.toString(arg(a, 0)), "")); });
    method(global_, u"decodeURI", 1, [](Runtime& rt, const Value&, Args a) { return Value::string(uriDecode(rt, *rt.toString(arg(a, 0)), ";/?:@&=+$,#")); });
    // Annex B's escape and unescape: %XX for the first 256 code units, %uXXXX beyond.
    method(global_, u"escape", 1, [](Runtime& rt, const Value&, Args a) {
        const std::u16string& in = *rt.toString(arg(a, 0));
        static const char16_t* const hex = u"0123456789ABCDEF";
        std::u16string out;
        for (const char16_t c : in) {
            if ((c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9') || c == u'@' || c == u'*' || c == u'_' ||
                c == u'+' || c == u'-' || c == u'.' || c == u'/') {
                out += c;
            } else if (c < 256) {
                out += u'%'; out += hex[c >> 4]; out += hex[c & 15];
            } else {
                out += u"%u"; out += hex[c >> 12]; out += hex[(c >> 8) & 15]; out += hex[(c >> 4) & 15]; out += hex[c & 15];
            }
        }
        return Value::string(std::move(out));
    });
    method(global_, u"unescape", 1, [](Runtime& rt, const Value&, Args a) {
        const std::u16string& in = *rt.toString(arg(a, 0));
        const auto digit = [](char16_t c) -> int {
            if (c >= u'0' && c <= u'9') return c - u'0';
            if (c >= u'a' && c <= u'f') return c - u'a' + 10;
            if (c >= u'A' && c <= u'F') return c - u'A' + 10;
            return -1;
        };
        std::u16string out;
        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i] == u'%') {
                if (i + 5 < in.size() && in[i + 1] == u'u') {
                    const int d[4] = {digit(in[i + 2]), digit(in[i + 3]), digit(in[i + 4]), digit(in[i + 5])};
                    if (d[0] >= 0 && d[1] >= 0 && d[2] >= 0 && d[3] >= 0) {
                        out += static_cast<char16_t>((d[0] << 12) | (d[1] << 8) | (d[2] << 4) | d[3]);
                        i += 5;
                        continue;
                    }
                }
                if (i + 2 < in.size()) {
                    const int h = digit(in[i + 1]), l = digit(in[i + 2]);
                    if (h >= 0 && l >= 0) {
                        out += static_cast<char16_t>((h << 4) | l);
                        i += 2;
                        continue;
                    }
                }
            }
            out += in[i];
        }
        return Value::string(std::move(out));
    });

    // ── Math ──
    Object* math = newObject();
    put(global_, u"Math", Value::object(math));
    put(math, PropertyKey(sym_to_string_tag), Value::string(u"Math"), kConfigurable);
    put(math, u"PI", Value::number(M_PI), 0);
    put(math, u"E", Value::number(M_E), 0);
    put(math, u"LN2", Value::number(M_LN2), 0);
    put(math, u"LN10", Value::number(M_LN10), 0);
    put(math, u"LOG2E", Value::number(M_LOG2E), 0);
    put(math, u"LOG10E", Value::number(M_LOG10E), 0);
    put(math, u"SQRT2", Value::number(M_SQRT2), 0);
    put(math, u"SQRT1_2", Value::number(M_SQRT1_2), 0);
    const auto unary = [&](const char16_t* name, double (*f)(double)) {
        method(math, name, 1, [f](Runtime& rt, const Value&, Args a) { return Value::number(f(rt.toNumber(arg(a, 0)))); });
    };
    unary(u"abs", [](double x) { return std::fabs(x); });
    unary(u"floor", [](double x) { return std::floor(x); });
    unary(u"ceil", [](double x) { return std::ceil(x); });
    unary(u"trunc", [](double x) { return std::trunc(x); });
    unary(u"sqrt", [](double x) { return std::sqrt(x); });
    unary(u"cbrt", [](double x) { return std::cbrt(x); });
    unary(u"sin", [](double x) { return std::sin(x); });
    unary(u"cos", [](double x) { return std::cos(x); });
    unary(u"tan", [](double x) { return std::tan(x); });
    unary(u"asin", [](double x) { return std::asin(x); });
    unary(u"acos", [](double x) { return std::acos(x); });
    unary(u"atan", [](double x) { return std::atan(x); });
    unary(u"sinh", [](double x) { return std::sinh(x); });
    unary(u"cosh", [](double x) { return std::cosh(x); });
    unary(u"tanh", [](double x) { return std::tanh(x); });
    unary(u"asinh", [](double x) { return std::asinh(x); });
    unary(u"acosh", [](double x) { return std::acosh(x); });
    unary(u"atanh", [](double x) { return std::atanh(x); });
    unary(u"exp", [](double x) { return std::exp(x); });
    unary(u"expm1", [](double x) { return std::expm1(x); });
    unary(u"log", [](double x) { return std::log(x); });
    unary(u"log2", [](double x) { return std::log2(x); });
    unary(u"log10", [](double x) { return std::log10(x); });
    unary(u"log1p", [](double x) { return std::log1p(x); });
    unary(u"fround", [](double x) { return static_cast<double>(static_cast<float>(x)); });
    unary(u"sign", [](double x) { return std::isnan(x) ? x : x > 0 ? 1.0 : x < 0 ? -1.0 : x; });
    unary(u"round", [](double x) {
        if (!std::isfinite(x) || x == 0) return x;
        if (x > 0 && x < 0.5) return 0.0;
        if (x < 0 && x >= -0.5) return -0.0;
        return std::floor(x + 0.5);
    });
    unary(u"clz32", [](double x) {
        const double m = std::fmod(std::trunc(std::isfinite(x) ? x : 0), 4294967296.0);
        uint32_t u = static_cast<uint32_t>(static_cast<int64_t>(m < 0 ? m + 4294967296.0 : m));
        if (u == 0) return 32.0;
        int n = 0;
        while (!(u & 0x80000000u)) { u <<= 1; ++n; }
        return static_cast<double>(n);
    });
    method(math, u"atan2", 2, [](Runtime& rt, const Value&, Args a) { return Value::number(std::atan2(rt.toNumber(arg(a, 0)), rt.toNumber(arg(a, 1)))); });
    method(math, u"pow", 2, [](Runtime& rt, const Value&, Args a) {
        const double x = rt.toNumber(arg(a, 0)), y = rt.toNumber(arg(a, 1));
        if (std::isnan(y)) return Value::number(std::nan(""));
        if (y == 0) return Value::number(1);
        if ((x == 1 || x == -1) && std::isinf(y)) return Value::number(std::nan(""));
        return Value::number(std::pow(x, y));
    });
    method(math, u"imul", 2, [](Runtime& rt, const Value&, Args a) {
        return Value::number(static_cast<int32_t>(static_cast<uint32_t>(rt.toInt32(arg(a, 0))) * static_cast<uint32_t>(rt.toInt32(arg(a, 1)))));
    });
    method(math, u"hypot", 2, [](Runtime& rt, const Value&, Args a) {
        double sum = 0;
        bool inf = false, nan = false;
        for (const Value& v : a) {
            const double d = rt.toNumber(v);
            if (std::isinf(d)) inf = true;
            if (std::isnan(d)) nan = true;
            sum += d * d;
        }
        if (inf) return Value::number(INFINITY);
        if (nan) return Value::number(std::nan(""));
        return Value::number(std::sqrt(sum));
    });
    const auto minmax = [](bool is_max) {
        return [is_max](Runtime& rt, const Value&, Args a) {
            double r = is_max ? -INFINITY : INFINITY;
            bool nan = false;
            for (const Value& v : a) {
                const double d = rt.toNumber(v);
                if (std::isnan(d)) nan = true;
                else if (is_max ? (d > r || (d == 0 && r == 0 && !std::signbit(d))) : (d < r || (d == 0 && r == 0 && std::signbit(d)))) r = d;
            }
            return Value::number(nan ? std::nan("") : r);
        };
    };
    method(math, u"max", 2, minmax(true));
    method(math, u"min", 2, minmax(false));
    method(math, u"random", 0, [](Runtime&, const Value&, Args) {
        static std::mt19937_64 rng{std::random_device{}()};
        return Value::number(std::uniform_real_distribution<double>(0, 1)(rng));
    });

    // ── Iterators ──
    iterator_prototype = newObject();
    iterator_prototype->define(PropertyKey(sym_iterator), {Value::object(newFunction(u"[Symbol.iterator]", 0, [](Runtime&, const Value& t, Args) { return t; })), nullptr, nullptr, kWritable | kConfigurable});
    array_iterator_prototype = newObject(iterator_prototype);
    put(array_iterator_prototype, PropertyKey(sym_to_string_tag), Value::string(u"Array Iterator"), kConfigurable);
    // iter_kind: 0 values, 1 keys, 2 entries (arrays); 3 a string's code points; 4-6 a
    // Map's values, keys, entries; 7 a Set's values.
    method(array_iterator_prototype, u"next", 0, [](Runtime& rt, const Value& t, Args) {
        if (!t.isObject() || t.asObject()->cls != ObjectClass::Iterator) rt.throwError("TypeError", "not an iterator");
        Object* it = t.asObject();
        if (!it->target && !it->primitive.isString()) return Value::object(rt.createIterResult(Value::undefined(), true));
        if (it->iter_kind == 3) {
            const auto& s = it->primitive.asString();
            if (it->cursor >= s.size()) {
                it->primitive = Value::undefined();
                return Value::object(rt.createIterResult(Value::undefined(), true));
            }
            size_t n = 1;
            if (s[it->cursor] >= 0xD800 && s[it->cursor] <= 0xDBFF && it->cursor + 1 < s.size() && s[it->cursor + 1] >= 0xDC00 && s[it->cursor + 1] <= 0xDFFF) n = 2;
            const Value v = Value::string(s.substr(it->cursor, n));
            it->cursor += n;
            return Value::object(rt.createIterResult(v, false));
        }
        Object* target = it->target;
        if (it->iter_kind >= 4) {
            // Skip deleted entries (marked empty).
            while (it->cursor < target->entries.size() && target->entries[it->cursor].first.isEmpty()) ++it->cursor;
            if (it->cursor >= target->entries.size()) {
                it->target = nullptr;
                return Value::object(rt.createIterResult(Value::undefined(), true));
            }
            const auto& e = target->entries[it->cursor++];
            Value v = it->iter_kind == 4 ? e.second : it->iter_kind == 5 ? e.first : it->iter_kind == 7 ? e.first : Value::object(rt.newArray({e.first, it->iter_kind == 6 && target->cls == ObjectClass::Set ? e.first : e.second}));
            return Value::object(rt.createIterResult(v, false));
        }
        const uint64_t len = rt.lengthOf(target);
        if (it->cursor >= len) {
            it->target = nullptr;
            return Value::object(rt.createIterResult(Value::undefined(), true));
        }
        const uint64_t i = it->cursor++;
        Value v;
        if (it->iter_kind == 1) v = Value::number(static_cast<double>(i));
        else if (it->iter_kind == 0) v = elementAt(rt, target, i);
        else v = Value::object(rt.newArray({Value::number(static_cast<double>(i)), elementAt(rt, target, i)}));
        return Value::object(rt.createIterResult(v, false));
    });
    const auto makeIterator = [](Runtime& rt, Object* target, uint8_t kind) {
        Object* it = rt.allocate<Object>(rt.array_iterator_prototype, ObjectClass::Iterator);
        it->target = target;
        it->iter_kind = kind;
        return Value::object(it);
    };

    // ── Array ──
    array_prototype = allocate<Object>(object_prototype, ObjectClass::Array);
    Object* array_ctor = ctor(u"Array", 1, [](Runtime& rt, const Value&, Args a) {
        Object* arr;
        if (a.size() == 1 && a[0].isNumber()) {
            const double n = a[0].asNumber();
            if (n < 0 || n != std::floor(n) || n > 4294967295.0) rt.throwError("RangeError", "invalid array length");
            if (n > 100'000'000) rt.throwError("RangeError", "array too long");
            arr = rt.newArray(std::vector<Value>(static_cast<size_t>(n), Value::empty()));
        } else {
            arr = rt.newArray(std::vector<Value>(a.begin(), a.end()));
        }
        // A subclass's instance: its prototype from new.target.
        if (Object* nt = rt.construct_target) {
            const Value p = rt.get(Value::object(nt), u"prototype");
            if (p.isObject()) arr->prototype = p.asObject();
        }
        return Value::object(arr);
    }, array_prototype);
    method(array_ctor, u"isArray", 1, [](Runtime& rt, const Value&, Args a) { return Value::boolean(rt.isArray(arg(a, 0))); });
    method(array_ctor, u"of", 0, [](Runtime& rt, const Value&, Args a) { return Value::object(rt.newArray(std::vector<Value>(a.begin(), a.end()))); });
    method(array_ctor, u"from", 1, [](Runtime& rt, const Value&, Args a) {
        const Value src = arg(a, 0);
        const Value fn = arg(a, 1);
        if (!fn.isUndefined()) callback(rt, fn);
        std::vector<Value> items;
        if (src.isNullish()) rt.throwError("TypeError", "Array.from needs an array-like or iterable");
        const Value iter = src.isString() ? Value::object(rt.function_prototype) : rt.get(src, PropertyKey(rt.sym_iterator));
        // Each value mapped as it comes: a mapFn that throws stops an endless iterator
        // (and closes it).
        const auto add = [&](const Value& v) {
            if (fn.isUndefined()) {
                items.push_back(v);
            } else {
                const Value args[2] = {v, Value::number(static_cast<double>(items.size()))};
                items.push_back(rt.call(fn, arg(a, 2), args));
            }
            if (items.size() > 100'000'000) rt.throwError("RangeError", "array too long");
        };
        if (!iter.isNullish()) {
            rt.iterate(src, add);
        } else {
            Object* o = rt.toObject(src);
            const uint64_t n = rt.lengthOf(o);
            if (n > 100'000'000) rt.throwError("RangeError", "array too long");
            for (uint64_t i = 0; i < n; ++i) add(elementAt(rt, o, i));
        }
        return Value::object(rt.newArray(std::move(items)));
    });
    method(array_prototype, u"push", 1, [](Runtime& rt, const Value& t, Args a) {
        Object* o = thisObject(rt, t);
        if (o->cls == ObjectClass::Array && o->extensible) {
            if (o->elements.size() + a.size() > 100'000'000) rt.throwError("RangeError", "array too long");
            o->elements.insert(o->elements.end(), a.begin(), a.end());
            return Value::number(static_cast<double>(o->elements.size()));
        }
        uint64_t n = rt.lengthOf(o);
        for (const Value& v : a) setElement(rt, o, n++, v);
        setLength(rt, o, n);
        return Value::number(static_cast<double>(n));
    });
    method(array_prototype, u"pop", 0, [](Runtime& rt, const Value& t, Args) {
        Object* o = thisObject(rt, t);
        if (o->cls == ObjectClass::Array) {
            if (o->elements.empty()) return Value::undefined();
            Value v = o->elements.back();
            o->elements.pop_back();
            return v.isEmpty() ? Value::undefined() : v;
        }
        const uint64_t n = rt.lengthOf(o);
        if (n == 0) {
            setLength(rt, o, 0);
            return Value::undefined();
        }
        const Value v = elementAt(rt, o, n - 1);
        rt.deleteProperty(o, Runtime::numberToString(static_cast<double>(n - 1)));
        setLength(rt, o, n - 1);
        return v;
    });
    method(array_prototype, u"shift", 0, [](Runtime& rt, const Value& t, Args) {
        Object* o = thisObject(rt, t);
        if (o->cls == ObjectClass::Array) {
            if (o->elements.empty()) return Value::undefined();
            Value v = o->elements.front();
            o->elements.erase(o->elements.begin());
            return v.isEmpty() ? Value::undefined() : v;
        }
        return Value::undefined();
    });
    method(array_prototype, u"unshift", 1, [](Runtime& rt, const Value& t, Args a) {
        Object* o = thisObject(rt, t);
        if (o->cls == ObjectClass::Array) {
            o->elements.insert(o->elements.begin(), a.begin(), a.end());
            return Value::number(static_cast<double>(o->elements.size()));
        }
        return Value::number(static_cast<double>(rt.lengthOf(o)));
    });
    method(array_prototype, u"slice", 2, [](Runtime& rt, const Value& t, Args a) {
        Object* o = thisObject(rt, t);
        const uint64_t len = rt.lengthOf(o);
        const uint64_t b = relative(rt, arg(a, 0), len, 0), e = relative(rt, arg(a, 1), len, len);
        if (e > b) checkNewArrayLength(rt, e - b);
        std::vector<Value> out;
        for (uint64_t i = b; i < e; ++i) out.push_back(hasElement(rt, o, i) ? elementAt(rt, o, i) : Value::empty());
        return Value::object(rt.newArray(std::move(out)));
    });
    method(array_prototype, u"splice", 2, [](Runtime& rt, const Value& t, Args a) {
        Object* o = thisObject(rt, t);
        if (o->cls != ObjectClass::Array) {
            // Anything with a length (Baidu's polyfills splice array-likes): the spec's steps,
            // element by element.
            const uint64_t len = rt.lengthOf(o);
            const uint64_t start = relative(rt, arg(a, 0), len, 0);
            uint64_t count = 0;
            if (a.size() == 1) count = len - start;
            else if (a.size() > 1) count = static_cast<uint64_t>(std::clamp(rt.toIntegerOrInfinity(a[1]), 0.0, static_cast<double>(len - start)));
            const uint64_t items = a.size() > 2 ? a.size() - 2 : 0;
            if (len - count + items > 9007199254740991ull) rt.throwError("TypeError", "the length would be too long");
            checkNewArrayLength(rt, count);
            std::vector<Value> removed;
            for (uint64_t k = 0; k < count; ++k) removed.push_back(hasElement(rt, o, start + k) ? elementAt(rt, o, start + k) : Value::empty());
            const auto key = [](uint64_t i) { return Runtime::numberToString(static_cast<double>(i)); };
            if (items < count) {
                for (uint64_t k = start; k < len - count; ++k) {
                    if (hasElement(rt, o, k + count)) setElement(rt, o, k + items, elementAt(rt, o, k + count));
                    else rt.deleteProperty(o, key(k + items));
                }
                for (uint64_t k = len; k > len - count + items; --k) rt.deleteProperty(o, key(k - 1));
            } else if (items > count) {
                for (uint64_t k = len - count; k > start; --k) {
                    if (hasElement(rt, o, k + count - 1)) setElement(rt, o, k + items - 1, elementAt(rt, o, k + count - 1));
                    else rt.deleteProperty(o, key(k + items - 1));
                }
            }
            for (uint64_t k = 0; k < items; ++k) setElement(rt, o, start + k, a[2 + k]);
            setLength(rt, o, len - count + items);
            return Value::object(rt.newArray(std::move(removed)));
        }
        const uint64_t len = o->elements.size();
        const uint64_t start = relative(rt, arg(a, 0), len, 0);
        uint64_t count = 0;
        if (a.size() == 1) count = len - start;
        else if (a.size() > 1) count = static_cast<uint64_t>(std::clamp(rt.toIntegerOrInfinity(a[1]), 0.0, static_cast<double>(len - start)));
        std::vector<Value> removed(o->elements.begin() + static_cast<std::ptrdiff_t>(start), o->elements.begin() + static_cast<std::ptrdiff_t>(start + count));
        o->elements.erase(o->elements.begin() + static_cast<std::ptrdiff_t>(start), o->elements.begin() + static_cast<std::ptrdiff_t>(start + count));
        if (a.size() > 2) o->elements.insert(o->elements.begin() + static_cast<std::ptrdiff_t>(start), a.begin() + 2, a.end());
        return Value::object(rt.newArray(std::move(removed)));
    });
    method(array_prototype, u"concat", 1, [](Runtime& rt, const Value& t, Args a) {
        std::vector<Value> out;
        const auto add = [&](const Value& v) {
            if (rt.isArray(v)) {
                Object* o = v.asObject();
                const uint64_t n = rt.lengthOf(o);
                for (uint64_t i = 0; i < n; ++i) out.push_back(hasElement(rt, o, i) ? elementAt(rt, o, i) : Value::empty());
            } else {
                out.push_back(v);
            }
            if (out.size() > 100'000'000) rt.throwError("RangeError", "array too long");
        };
        add(Value::object(thisObject(rt, t)));
        for (const Value& v : a) add(v);
        return Value::object(rt.newArray(std::move(out)));
    });
    method(array_prototype, u"join", 1, [](Runtime& rt, const Value& t, Args a) {
        Object* o = thisObject(rt, t);
        static thread_local std::vector<Object*> joining;   // a cycle joins as ""
        if (std::find(joining.begin(), joining.end(), o) != joining.end()) return Value::string(u"");
        joining.push_back(o);
        struct Pop { ~Pop() { joining.pop_back(); } } pop;
        const std::u16string sep = arg(a, 0).isUndefined() ? u"," : *rt.toString(arg(a, 0));
        const uint64_t n = rt.lengthOf(o);
        std::u16string out;
        for (uint64_t i = 0; i < n; ++i) {
            if (i) out += sep;
            const Value v = elementAt(rt, o, i);
            if (!v.isNullish()) out += *rt.toString(v);
            if (out.size() > (1u << 29)) rt.throwError("RangeError", "string too long");
        }
        return Value::string(std::move(out));
    });
    method(array_prototype, u"toString", 0, [](Runtime& rt, const Value& t, Args) {
        const Value join = rt.get(t, u"join");
        if (rt.isCallable(join)) return rt.call(join, t, {});
        return rt.call(rt.get(Value::object(rt.object_prototype), u"toString"), t, {});
    });
    method(array_prototype, u"toLocaleString", 0, [](Runtime& rt, const Value& t, Args) { return rt.call(rt.get(t, u"join"), t, {}); });
    method(array_prototype, u"reverse", 0, [](Runtime& rt, const Value& t, Args) {
        Object* o = thisObject(rt, t);
        if (o->cls == ObjectClass::Array) std::reverse(o->elements.begin(), o->elements.end());
        return Value::object(o);
    });
    const auto search = [](bool from_end, bool includes) {
        return [from_end, includes](Runtime& rt, const Value& t, Args a) {
            Object* o = thisObject(rt, t);
            const int64_t len = static_cast<int64_t>(rt.lengthOf(o));
            if (len == 0) return includes ? Value::boolean(false) : Value::number(-1);
            int64_t i;
            if (from_end) {
                i = a.size() > 1 ? static_cast<int64_t>(std::max(-1.0, std::min(rt.toIntegerOrInfinity(a[1]), static_cast<double>(len - 1)))) : len - 1;
                if (a.size() > 1 && rt.toIntegerOrInfinity(a[1]) < 0) i = len + static_cast<int64_t>(std::max(rt.toIntegerOrInfinity(a[1]), -static_cast<double>(len) - 1));
                for (; i >= 0; --i)
                    if (hasElement(rt, o, static_cast<uint64_t>(i)) && Runtime::strictEquals(elementAt(rt, o, static_cast<uint64_t>(i)), arg(a, 0))) return Value::number(static_cast<double>(i));
                return Value::number(-1);
            }
            i = static_cast<int64_t>(relative(rt, arg(a, 1), static_cast<uint64_t>(len), 0));
            for (; i < len; ++i) {
                if (includes) {
                    if (Runtime::sameValueZero(elementAt(rt, o, static_cast<uint64_t>(i)), arg(a, 0))) return Value::boolean(true);
                } else if (hasElement(rt, o, static_cast<uint64_t>(i)) && Runtime::strictEquals(elementAt(rt, o, static_cast<uint64_t>(i)), arg(a, 0))) {
                    return Value::number(static_cast<double>(i));
                }
            }
            return includes ? Value::boolean(false) : Value::number(-1);
        };
    };
    method(array_prototype, u"indexOf", 1, search(false, false));
    method(array_prototype, u"lastIndexOf", 1, search(true, false));
    method(array_prototype, u"includes", 1, search(false, true));
    // The callback family: each takes (value, index, array).
    enum class Each { ForEach, Map, Filter, Some, Every, Find, FindIndex, FindLast, FindLastIndex };
    const auto each = [](Each kind) {
        return [kind](Runtime& rt, const Value& t, Args a) {
            Object* o = thisObject(rt, t);
            const uint64_t len = rt.lengthOf(o);
            const Value fn = callback(rt, arg(a, 0));
            const Value self = arg(a, 1);
            std::vector<Value> out;
            TempRoots roots(rt, out);   // results so far, while the callback runs
            if (kind == Each::Map) {
                checkNewArrayLength(rt, len);
                out.assign(len, Value::empty());
            }
            const bool backwards = kind == Each::FindLast || kind == Each::FindLastIndex;
            for (uint64_t step = 0; step < len; ++step) {
                const uint64_t i = backwards ? len - 1 - step : step;
                const bool finding = kind == Each::Find || kind == Each::FindIndex || kind == Each::FindLast || kind == Each::FindLastIndex;
                if (!finding && !hasElement(rt, o, i)) continue;
                const Value v = elementAt(rt, o, i);
                const Value args[3] = {v, Value::number(static_cast<double>(i)), Value::object(o)};
                const Value r = rt.call(fn, self, args);
                switch (kind) {
                    case Each::Map: out[i] = r; break;
                    case Each::Filter: if (rt.toBoolean(r)) out.push_back(v); break;
                    case Each::Some: if (rt.toBoolean(r)) return Value::boolean(true); break;
                    case Each::Every: if (!rt.toBoolean(r)) return Value::boolean(false); break;
                    case Each::Find: case Each::FindLast: if (rt.toBoolean(r)) return v; break;
                    case Each::FindIndex: case Each::FindLastIndex: if (rt.toBoolean(r)) return Value::number(static_cast<double>(i)); break;
                    default: break;
                }
            }
            switch (kind) {
                case Each::Map: case Each::Filter: return Value::object(rt.newArray(std::move(out)));
                case Each::Some: return Value::boolean(false);
                case Each::Every: return Value::boolean(true);
                case Each::FindIndex: case Each::FindLastIndex: return Value::number(-1);
                default: return Value::undefined();
            }
        };
    };
    method(array_prototype, u"forEach", 1, each(Each::ForEach));
    method(array_prototype, u"map", 1, each(Each::Map));
    method(array_prototype, u"filter", 1, each(Each::Filter));
    method(array_prototype, u"some", 1, each(Each::Some));
    method(array_prototype, u"every", 1, each(Each::Every));
    method(array_prototype, u"find", 1, each(Each::Find));
    method(array_prototype, u"findIndex", 1, each(Each::FindIndex));
    method(array_prototype, u"findLast", 1, each(Each::FindLast));
    method(array_prototype, u"findLastIndex", 1, each(Each::FindLastIndex));
    const auto reduce = [](bool right) {
        return [right](Runtime& rt, const Value& t, Args a) {
            Object* o = thisObject(rt, t);
            const uint64_t len = rt.lengthOf(o);
            const Value fn = callback(rt, arg(a, 0));
            uint64_t k = 0;
            Value acc;
            bool have = a.size() > 1;
            if (have) acc = a[1];
            for (; k < len; ++k) {
                const uint64_t i = right ? len - 1 - k : k;
                if (!hasElement(rt, o, i)) continue;
                const Value v = elementAt(rt, o, i);
                if (!have) {
                    acc = v;
                    have = true;
                    continue;
                }
                const Value args[4] = {acc, v, Value::number(static_cast<double>(i)), Value::object(o)};
                acc = rt.call(fn, Value::undefined(), args);
            }
            if (!have) rt.throwError("TypeError", "reduce of an empty array with no initial value");
            return acc;
        };
    };
    method(array_prototype, u"reduce", 1, reduce(false));
    method(array_prototype, u"reduceRight", 1, reduce(true));
    // These keep nothing the collector can't see while they call back (their results in
    // TempRoots, their arguments in the call's roots): a long loop inside one can collect.
    for (const char16_t* name : {u"forEach", u"map", u"filter", u"some", u"every", u"find", u"findIndex", u"findLast", u"findLastIndex", u"reduce", u"reduceRight"})
        get(Value::object(array_prototype), name).asObject()->gc_safe = true;
    for (const char16_t* name : {u"call", u"apply"}) get(Value::object(function_prototype), name).asObject()->gc_safe = true;
    method(array_prototype, u"sort", 1, [](Runtime& rt, const Value& t, Args a) {
        Object* o = thisObject(rt, t);
        const Value cmp = arg(a, 0);
        if (!cmp.isUndefined()) callback(rt, cmp);
        if (o->cls != ObjectClass::Array) return Value::object(o);
        std::vector<Value> items;
        size_t holes = 0;
        for (const Value& v : o->elements) {
            if (v.isEmpty()) ++holes;
            else items.push_back(v);
        }
        sortValues(rt, items, cmp);
        items.insert(items.end(), holes, Value::empty());
        o->elements = std::move(items);
        return Value::object(o);
    });
    method(array_prototype, u"fill", 1, [](Runtime& rt, const Value& t, Args a) {
        Object* o = thisObject(rt, t);
        const uint64_t len = rt.lengthOf(o);
        const uint64_t b = relative(rt, arg(a, 1), len, 0), e = relative(rt, arg(a, 2), len, len);
        for (uint64_t i = b; i < e; ++i) setElement(rt, o, i, arg(a, 0));
        return Value::object(o);
    });
    method(array_prototype, u"at", 1, [](Runtime& rt, const Value& t, Args a) {
        Object* o = thisObject(rt, t);
        const double len = static_cast<double>(rt.lengthOf(o));
        double i = rt.toIntegerOrInfinity(arg(a, 0));
        if (i < 0) i += len;
        if (i < 0 || i >= len) return Value::undefined();
        return elementAt(rt, o, static_cast<uint64_t>(i));
    });
    const auto flatten = [](Runtime& rt, Object* o, double depth, std::vector<Value>& out, auto& self) -> void {
        const uint64_t len = rt.lengthOf(o);
        for (uint64_t i = 0; i < len; ++i) {
            if (!hasElement(rt, o, i)) continue;
            const Value v = elementAt(rt, o, i);
            if (depth > 0 && v.isObject() && v.asObject()->cls == ObjectClass::Array) self(rt, v.asObject(), depth - 1, out, self);
            else out.push_back(v);
            if (out.size() > 100'000'000) rt.throwError("RangeError", "array too long");
        }
    };
    method(array_prototype, u"flat", 0, [flatten](Runtime& rt, const Value& t, Args a) {
        std::vector<Value> out;
        const double depth = arg(a, 0).isUndefined() ? 1 : rt.toIntegerOrInfinity(arg(a, 0));
        flatten(rt, thisObject(rt, t), depth, out, flatten);
        return Value::object(rt.newArray(std::move(out)));
    });
    method(array_prototype, u"flatMap", 1, [](Runtime& rt, const Value& t, Args a) {
        Object* o = thisObject(rt, t);
        const Value fn = callback(rt, arg(a, 0));
        std::vector<Value> out;
        const uint64_t len = rt.lengthOf(o);
        for (uint64_t i = 0; i < len; ++i) {
            if (!hasElement(rt, o, i)) continue;
            const Value args[3] = {elementAt(rt, o, i), Value::number(static_cast<double>(i)), Value::object(o)};
            const Value r = rt.call(fn, arg(a, 1), args);
            if (r.isObject() && r.asObject()->cls == ObjectClass::Array) {
                for (uint64_t j = 0; j < rt.lengthOf(r.asObject()); ++j) out.push_back(elementAt(rt, r.asObject(), j));
            } else {
                out.push_back(r);
            }
        }
        return Value::object(rt.newArray(std::move(out)));
    });
    method(array_prototype, u"keys", 0, [makeIterator](Runtime& rt, const Value& t, Args) { return makeIterator(rt, thisObject(rt, t), 1); });
    method(array_prototype, u"entries", 0, [makeIterator](Runtime& rt, const Value& t, Args) { return makeIterator(rt, thisObject(rt, t), 2); });
    method(array_prototype, u"values", 0, [makeIterator](Runtime& rt, const Value& t, Args) { return makeIterator(rt, thisObject(rt, t), 0); });
    array_values_ = get(array_prototype, u"values", Value::object(array_prototype)).asObject();
    put(array_prototype, PropertyKey(sym_iterator), Value::object(array_values_));

    // ── String ──
    string_prototype = allocate<Object>(object_prototype, ObjectClass::String);
    string_prototype->primitive = Value::string(u"");
    Object* string_ctor = ctor(u"String", 1, [](Runtime& rt, const Value&, Args a) {
        Value s;
        if (a.empty()) s = Value::string(u"");
        else if (a[0].isSymbol() && !rt.construct_target) s = Value::string(u"Symbol(" + a[0].asSymbol()->description + u")");
        else s = Value::string(rt.toString(a[0]));
        if (!rt.construct_target) return s;
        return Value::object(rt.toObject(s));
    }, string_prototype);
    method(string_ctor, u"fromCharCode", 1, [](Runtime& rt, const Value&, Args a) {
        std::u16string s;
        for (const Value& v : a) s += static_cast<char16_t>(rt.toUint32(v) & 0xFFFF);
        return Value::string(std::move(s));
    });
    method(string_ctor, u"fromCodePoint", 1, [](Runtime& rt, const Value&, Args a) {
        std::u16string s;
        for (const Value& v : a) {
            const double d = rt.toNumber(v);
            if (d < 0 || d > 0x10FFFF || d != std::floor(d)) rt.throwError("RangeError", "invalid code point");
            const auto cp = static_cast<char32_t>(d);
            if (cp >= 0x10000) {
                s += static_cast<char16_t>(0xD800 + ((cp - 0x10000) >> 10));
                s += static_cast<char16_t>(0xDC00 + ((cp - 0x10000) & 0x3FF));
            } else {
                s += static_cast<char16_t>(cp);
            }
        }
        return Value::string(std::move(s));
    });
    method(string_ctor, u"raw", 1, [](Runtime& rt, const Value&, Args a) {
        Object* strings = rt.toObject(arg(a, 0));
        Object* raw = rt.toObject(rt.get(strings, u"raw", Value::object(strings)));
        const uint64_t n = rt.lengthOf(raw);
        std::u16string out;
        for (uint64_t i = 0; i < n; ++i) {
            out += *rt.toString(elementAt(rt, raw, i));
            if (i + 1 < n && i + 1 < a.size()) out += *rt.toString(a[i + 1]);
        }
        return Value::string(std::move(out));
    });
    const auto thisString = [](Runtime& rt, const Value& t) -> std::u16string {
        if (t.isString()) return t.asString();
        if (t.isNullish()) rt.throwError("TypeError", "String.prototype method called on null or undefined");
        return *rt.toString(t);
    };
    method(string_prototype, u"toString", 0, [](Runtime& rt, const Value& t, Args) {
        if (t.isString()) return t;
        if (t.isObject() && t.asObject()->cls == ObjectClass::String) return t.asObject()->primitive;
        rt.throwError("TypeError", "not a string");
    });
    put(string_prototype, u"valueOf", get(string_prototype, u"toString", Value::object(string_prototype)));
    method(string_prototype, u"charAt", 1, [thisString](Runtime& rt, const Value& t, Args a) {
        const std::u16string s = thisString(rt, t);
        const double i = rt.toIntegerOrInfinity(arg(a, 0));
        return Value::string(i >= 0 && i < static_cast<double>(s.size()) ? std::u16string(1, s[static_cast<size_t>(i)]) : u"");
    });
    method(string_prototype, u"charCodeAt", 1, [thisString](Runtime& rt, const Value& t, Args a) {
        const std::u16string s = thisString(rt, t);
        const double i = rt.toIntegerOrInfinity(arg(a, 0));
        return Value::number(i >= 0 && i < static_cast<double>(s.size()) ? s[static_cast<size_t>(i)] : std::nan(""));
    });
    method(string_prototype, u"codePointAt", 1, [thisString](Runtime& rt, const Value& t, Args a) {
        const std::u16string s = thisString(rt, t);
        const double d = rt.toIntegerOrInfinity(arg(a, 0));
        if (d < 0 || d >= static_cast<double>(s.size())) return Value::undefined();
        const size_t i = static_cast<size_t>(d);
        const char16_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)
            return Value::number(0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00));
        return Value::number(c);
    });
    method(string_prototype, u"at", 1, [thisString](Runtime& rt, const Value& t, Args a) {
        const std::u16string s = thisString(rt, t);
        double i = rt.toIntegerOrInfinity(arg(a, 0));
        if (i < 0) i += static_cast<double>(s.size());
        if (i < 0 || i >= static_cast<double>(s.size())) return Value::undefined();
        return Value::string(std::u16string(1, s[static_cast<size_t>(i)]));
    });
    method(string_prototype, u"indexOf", 1, [thisString](Runtime& rt, const Value& t, Args a) {
        const std::u16string s = thisString(rt, t), needle = *rt.toString(arg(a, 0));
        const double from = std::clamp(rt.toIntegerOrInfinity(arg(a, 1)), 0.0, static_cast<double>(s.size()));
        const size_t at = s.find(needle, static_cast<size_t>(from));
        return Value::number(at == std::u16string::npos ? -1 : static_cast<double>(at));
    });
    method(string_prototype, u"lastIndexOf", 1, [thisString](Runtime& rt, const Value& t, Args a) {
        const std::u16string s = thisString(rt, t), needle = *rt.toString(arg(a, 0));
        const double n = rt.toNumber(arg(a, 1));
        const size_t from = std::isnan(n) ? std::u16string::npos : static_cast<size_t>(std::clamp(std::trunc(n), 0.0, static_cast<double>(s.size())));
        const size_t at = s.rfind(needle, from);
        return Value::number(at == std::u16string::npos ? -1 : static_cast<double>(at));
    });
    const auto noRegex = [](Runtime& rt, const Value& v) {
        if (rt.isRegExp(v)) rt.throwError("TypeError", "a regular expression isn't allowed here");
    };
    method(string_prototype, u"includes", 1, [thisString, noRegex](Runtime& rt, const Value& t, Args a) {
        noRegex(rt, arg(a, 0));
        const std::u16string s = thisString(rt, t), needle = *rt.toString(arg(a, 0));
        const double from = std::clamp(rt.toIntegerOrInfinity(arg(a, 1)), 0.0, static_cast<double>(s.size()));
        return Value::boolean(s.find(needle, static_cast<size_t>(from)) != std::u16string::npos);
    });
    method(string_prototype, u"startsWith", 1, [thisString, noRegex](Runtime& rt, const Value& t, Args a) {
        noRegex(rt, arg(a, 0));
        const std::u16string s = thisString(rt, t), needle = *rt.toString(arg(a, 0));
        const size_t from = static_cast<size_t>(std::clamp(rt.toIntegerOrInfinity(arg(a, 1)), 0.0, static_cast<double>(s.size())));
        return Value::boolean(s.compare(from, needle.size(), needle) == 0 && from + needle.size() <= s.size());
    });
    method(string_prototype, u"endsWith", 1, [thisString, noRegex](Runtime& rt, const Value& t, Args a) {
        noRegex(rt, arg(a, 0));
        const std::u16string s = thisString(rt, t), needle = *rt.toString(arg(a, 0));
        const size_t end = arg(a, 1).isUndefined() ? s.size() : static_cast<size_t>(std::clamp(rt.toIntegerOrInfinity(arg(a, 1)), 0.0, static_cast<double>(s.size())));
        return Value::boolean(needle.size() <= end && s.compare(end - needle.size(), needle.size(), needle) == 0);
    });
    method(string_prototype, u"slice", 2, [thisString](Runtime& rt, const Value& t, Args a) {
        const std::u16string s = thisString(rt, t);
        const uint64_t b = relative(rt, arg(a, 0), s.size(), 0), e = relative(rt, arg(a, 1), s.size(), s.size());
        return Value::string(b < e ? s.substr(b, e - b) : u"");
    });
    method(string_prototype, u"substring", 2, [thisString](Runtime& rt, const Value& t, Args a) {
        const std::u16string s = thisString(rt, t);
        const double len = static_cast<double>(s.size());
        double b = std::clamp(rt.toIntegerOrInfinity(arg(a, 0)), 0.0, len);
        double e = arg(a, 1).isUndefined() ? len : std::clamp(rt.toIntegerOrInfinity(arg(a, 1)), 0.0, len);
        if (b > e) std::swap(b, e);
        return Value::string(s.substr(static_cast<size_t>(b), static_cast<size_t>(e - b)));
    });
    method(string_prototype, u"substr", 2, [thisString](Runtime& rt, const Value& t, Args a) {
        const std::u16string s = thisString(rt, t);
        const uint64_t b = relative(rt, arg(a, 0), s.size(), 0);
        const double n = arg(a, 1).isUndefined() ? static_cast<double>(s.size()) : std::clamp(rt.toIntegerOrInfinity(arg(a, 1)), 0.0, static_cast<double>(s.size()));
        return Value::string(s.substr(b, static_cast<size_t>(n)));
    });
    method(string_prototype, u"toUpperCase", 0, [thisString](Runtime& rt, const Value& t, Args) { return Value::string(toCase(thisString(rt, t), true)); });
    method(string_prototype, u"toLowerCase", 0, [thisString](Runtime& rt, const Value& t, Args) { return Value::string(toCase(thisString(rt, t), false)); });
    method(string_prototype, u"toLocaleUpperCase", 0, [thisString](Runtime& rt, const Value& t, Args) { return Value::string(toCase(thisString(rt, t), true)); });
    method(string_prototype, u"toLocaleLowerCase", 0, [thisString](Runtime& rt, const Value& t, Args) { return Value::string(toCase(thisString(rt, t), false)); });
    const auto trimmer = [thisString](bool start, bool end) {
        return [thisString, start, end](Runtime& rt, const Value& t, Args) {
            const std::u16string s = thisString(rt, t);
            size_t b = 0, e = s.size();
            if (start) while (b < e && isWhite(s[b])) ++b;
            if (end) while (e > b && isWhite(s[e - 1])) --e;
            return Value::string(s.substr(b, e - b));
        };
    };
    method(string_prototype, u"trim", 0, trimmer(true, true));
    method(string_prototype, u"trimStart", 0, trimmer(true, false));
    method(string_prototype, u"trimEnd", 0, trimmer(false, true));
    put(string_prototype, u"trimLeft", get(string_prototype, u"trimStart", Value::object(string_prototype)));
    put(string_prototype, u"trimRight", get(string_prototype, u"trimEnd", Value::object(string_prototype)));
    method(string_prototype, u"split", 2, [thisString](Runtime& rt, const Value& t, Args a) {
        const std::u16string s = thisString(rt, t);
        const uint32_t limit = arg(a, 1).isUndefined() ? 0xFFFFFFFFu : rt.toUint32(arg(a, 1));
        std::vector<Value> out;
        const std::u16string sep = arg(a, 0).isUndefined() ? u"" : *rt.toString(arg(a, 0));
        if (limit == 0) return Value::object(rt.newArray());
        if (arg(a, 0).isUndefined()) return Value::object(rt.newArray({Value::string(s)}));
        if (sep.empty()) {
            for (size_t i = 0; i < s.size() && out.size() < limit; ++i) out.push_back(Value::string(std::u16string(1, s[i])));
            return Value::object(rt.newArray(std::move(out)));
        }
        size_t start = 0;
        while (out.size() < limit) {
            const size_t at = s.find(sep, start);
            if (at == std::u16string::npos) {
                out.push_back(Value::string(s.substr(start)));
                break;
            }
            out.push_back(Value::string(s.substr(start, at - start)));
            start = at + sep.size();
        }
        return Value::object(rt.newArray(std::move(out)));
    });
    const auto replacer = [thisString](bool all) {
        return [thisString, all](Runtime& rt, const Value& t, Args a) {
            const std::u16string s = thisString(rt, t);
            const std::u16string pat = *rt.toString(arg(a, 0));
            const Value with = arg(a, 1);
            const bool fn = rt.isCallable(with);
            const std::u16string tmpl = fn ? u"" : *rt.toString(with);
            std::u16string out;
            size_t start = 0;
            while (true) {
                const size_t at = pat.empty() && start > s.size() ? std::u16string::npos : s.find(pat, start);
                if (at == std::u16string::npos) break;
                out += s.substr(start, at - start);
                if (fn) {
                    const Value args[3] = {Value::string(pat), Value::number(static_cast<double>(at)), Value::string(s)};
                    out += *rt.toString(rt.call(with, Value::undefined(), args));
                } else {
                    // $$, $&, $` and $' in the replacement.
                    for (size_t i = 0; i < tmpl.size(); ++i) {
                        if (tmpl[i] == u'$' && i + 1 < tmpl.size()) {
                            const char16_t n = tmpl[i + 1];
                            if (n == u'$') { out += u'$'; ++i; continue; }
                            if (n == u'&') { out += pat; ++i; continue; }
                            if (n == u'`') { out += s.substr(0, at); ++i; continue; }
                            if (n == u'\'') { out += s.substr(at + pat.size()); ++i; continue; }
                        }
                        out += tmpl[i];
                    }
                }
                start = at + pat.size();
                if (pat.empty()) {
                    if (at < s.size()) out += s[at];
                    start = at + 1;
                }
                if (!all) break;
                if (out.size() > (1u << 29)) rt.throwError("RangeError", "string too long");
            }
            if (start <= s.size()) out += s.substr(start);
            return Value::string(std::move(out));
        };
    };
    method(string_prototype, u"replace", 2, replacer(false));
    method(string_prototype, u"replaceAll", 2, replacer(true));
    method(string_prototype, u"repeat", 1, [thisString](Runtime& rt, const Value& t, Args a) {
        const std::u16string s = thisString(rt, t);
        const double n = rt.toIntegerOrInfinity(arg(a, 0));
        if (n < 0 || std::isinf(n)) rt.throwError("RangeError", "invalid repeat count");
        if (s.size() * n > (1u << 29)) rt.throwError("RangeError", "string too long");
        std::u16string out;
        for (int i = 0; i < static_cast<int>(n); ++i) out += s;
        return Value::string(std::move(out));
    });
    const auto padder = [thisString](bool start) {
        return [thisString, start](Runtime& rt, const Value& t, Args a) {
            const std::u16string s = thisString(rt, t);
            const double target = rt.toIntegerOrInfinity(arg(a, 0));
            const std::u16string fill = arg(a, 1).isUndefined() ? u" " : *rt.toString(arg(a, 1));
            if (target <= static_cast<double>(s.size()) || fill.empty()) return Value::string(s);
            if (target > (1u << 29)) rt.throwError("RangeError", "string too long");
            std::u16string pad;
            while (pad.size() < static_cast<size_t>(target) - s.size()) pad += fill;
            pad.resize(static_cast<size_t>(target) - s.size());
            return Value::string(start ? pad + s : s + pad);
        };
    };
    method(string_prototype, u"padStart", 2, padder(true));
    method(string_prototype, u"padEnd", 2, padder(false));
    method(string_prototype, u"concat", 1, [thisString](Runtime& rt, const Value& t, Args a) {
        std::u16string s = thisString(rt, t);
        for (const Value& v : a) s += *rt.toString(v);
        return Value::string(std::move(s));
    });
    method(string_prototype, u"localeCompare", 1, [thisString](Runtime& rt, const Value& t, Args a) {
        const std::u16string s = thisString(rt, t), o = *rt.toString(arg(a, 0));
        return Value::number(s < o ? -1 : s > o ? 1 : 0);
    });
    method(string_prototype, u"normalize", 0, [thisString](Runtime& rt, const Value& t, Args) { return Value::string(thisString(rt, t)); });
    method(string_prototype, u"codePoints", 0, [thisString](Runtime& rt, const Value& t, Args) { return Value::string(thisString(rt, t)); });
    string_prototype->remove(u"codePoints");
    put(string_prototype, PropertyKey(sym_iterator), Value::object(newFunction(u"[Symbol.iterator]", 0, [thisString](Runtime& rt, const Value& t, Args) {
        Object* it = rt.allocate<Object>(rt.array_iterator_prototype, ObjectClass::Iterator);
        it->primitive = Value::string(thisString(rt, t));
        it->iter_kind = 3;
        return Value::object(it);
    })));

    // ── Map and Set ──
    map_prototype = newObject();
    set_prototype = newObject();
    const auto collectionCtor = [&](const char16_t* name, Object* proto, ObjectClass cls) {
        return ctor(name, 0, [proto, cls](Runtime& rt, const Value&, Args a) -> Value {
            if (!rt.construct_target) rt.throwError("TypeError", "a Map or Set must be made with new");
            Object* nt = rt.construct_target;
            const Value p = rt.get(Value::object(nt), u"prototype");
            Object* m = rt.allocate<Object>(p.isObject() ? p.asObject() : proto, cls);
            if (!arg(a, 0).isNullish()) {
                const Value adder = rt.get(Value::object(m), cls == ObjectClass::Map ? u"set" : u"add");
                if (!rt.isCallable(adder)) rt.throwError("TypeError", cls == ObjectClass::Map ? "Map.prototype.set isn't a function" : "Set.prototype.add isn't a function");
                rt.iterate(arg(a, 0), [&](const Value& e) {
                    if (cls == ObjectClass::Map) {
                        if (!e.isObject()) rt.throwError("TypeError", "a Map entry must be an object");
                        const Value kv[2] = {rt.get(e, u"0"), rt.get(e, u"1")};
                        rt.call(adder, Value::object(m), kv);
                    } else {
                        rt.call(adder, Value::object(m), std::span<const Value>(&e, 1));
                    }
                });
            }
            return Value::object(m);
        }, proto);
    };
    collectionCtor(u"Map", map_prototype, ObjectClass::Map);
    collectionCtor(u"Set", set_prototype, ObjectClass::Set);
    const auto thisCollection = [](Runtime& rt, const Value& t, ObjectClass cls) {
        if (!t.isObject() || t.asObject()->cls != cls) rt.throwError("TypeError", cls == ObjectClass::Map ? "not a Map" : "not a Set");
        return t.asObject();
    };
    const auto findEntry = [](Object* m, const Value& k) -> std::pair<Value, Value>* {
        const auto it = m->index.find(k);
        return it == m->index.end() ? nullptr : &m->entries[it->second];
    };
    const auto normalizeKey = [](const Value& k) { return k.isNumber() && k.asNumber() == 0 ? Value::number(0) : k; };
    for (int which = 0; which < 2; ++which) {
        const ObjectClass cls = which == 0 ? ObjectClass::Map : ObjectClass::Set;
        Object* proto = which == 0 ? map_prototype : set_prototype;
        put(proto, PropertyKey(sym_to_string_tag), Value::string(which == 0 ? u"Map" : u"Set"), kConfigurable);
        method(proto, u"has", 1, [=](Runtime& rt, const Value& t, Args a) { return Value::boolean(findEntry(thisCollection(rt, t, cls), arg(a, 0)) != nullptr); });
        method(proto, u"delete", 1, [=](Runtime& rt, const Value& t, Args a) {
            Object* m = thisCollection(rt, t, cls);
            auto* e = findEntry(m, arg(a, 0));
            if (!e) return Value::boolean(false);
            m->index.erase(e->first);
            e->first = Value::empty();   // a hole: iterators in progress skip it
            e->second = Value::undefined();
            return Value::boolean(true);
        });
        method(proto, u"clear", 0, [=](Runtime& rt, const Value& t, Args) {
            Object* m = thisCollection(rt, t, cls);
            for (auto& e : m->entries) {
                e.first = Value::empty();
                e.second = Value::undefined();
            }
            m->index.clear();
            return Value::undefined();
        });
        getter(proto, u"size", [=](Runtime& rt, const Value& t, Args) {
            return Value::number(static_cast<double>(thisCollection(rt, t, cls)->index.size()));
        });
        method(proto, u"forEach", 1, [=](Runtime& rt, const Value& t, Args a) {
            Object* m = thisCollection(rt, t, cls);
            const Value fn = callback(rt, arg(a, 0));
            for (size_t i = 0; i < m->entries.size(); ++i) {
                if (m->entries[i].first.isEmpty()) continue;
                const Value k = m->entries[i].first, v = cls == ObjectClass::Map ? m->entries[i].second : k;
                const Value args[3] = {v, k, t};
                rt.call(fn, arg(a, 1), args);
            }
            return Value::undefined();
        });
        const auto iter = [=](uint8_t kind) {
            return [=](Runtime& rt, const Value& t, Args) {
                Object* it = rt.allocate<Object>(rt.array_iterator_prototype, ObjectClass::Iterator);
                it->target = thisCollection(rt, t, cls);
                it->iter_kind = kind;
                return Value::object(it);
            };
        };
        if (which == 0) {
            method(proto, u"get", 1, [=](Runtime& rt, const Value& t, Args a) {
                auto* e = findEntry(thisCollection(rt, t, cls), arg(a, 0));
                return e ? e->second : Value::undefined();
            });
            method(proto, u"set", 2, [=](Runtime& rt, const Value& t, Args a) {
                Object* m = thisCollection(rt, t, cls);
                if (auto* e = findEntry(m, arg(a, 0))) {
                    e->second = arg(a, 1);
                } else {
                    m->index.emplace(normalizeKey(arg(a, 0)), m->entries.size());
                    m->entries.emplace_back(normalizeKey(arg(a, 0)), arg(a, 1));
                }
                return t;
            });
            method(proto, u"values", 0, iter(4));
            method(proto, u"keys", 0, iter(5));
            method(proto, u"entries", 0, iter(6));
            put(proto, PropertyKey(sym_iterator), get(proto, u"entries", Value::object(proto)));
        } else {
            method(proto, u"add", 1, [=](Runtime& rt, const Value& t, Args a) {
                Object* m = thisCollection(rt, t, cls);
                if (!findEntry(m, arg(a, 0))) {
                    m->index.emplace(normalizeKey(arg(a, 0)), m->entries.size());
                    m->entries.emplace_back(normalizeKey(arg(a, 0)), Value::undefined());
                }
                return t;
            });
            method(proto, u"values", 0, iter(7));
            put(proto, u"keys", get(proto, u"values", Value::object(proto)));
            method(proto, u"entries", 0, iter(6));
            put(proto, PropertyKey(sym_iterator), get(proto, u"values", Value::object(proto)));
        }
    }

    // ── WeakMap, WeakSet, WeakRef, FinalizationRegistry ──
    // Held weakly by the collector: an entry goes when nothing else keeps its key.
    {
        const auto canBeHeldWeakly = [](Runtime& rt, const Value& v) {
            return v.isObject() || (v.isSymbol() && [&] {
                for (auto& [k, s] : rt.symbol_registry)
                    if (s == v.asSymbol()) return false;
                return true;
            }());
        };
        const auto weakCtor = [&](const char16_t* name, ObjectClass cls) {
            Object* proto = newObject();
            put(proto, PropertyKey(sym_to_string_tag), Value::string(name), kConfigurable);
            Object* c = ctor(name, 0, [proto, cls](Runtime& rt, const Value&, Args a) -> Value {
                Object* nt = rt.construct_target;
                if (!nt) rt.throwError("TypeError", "it must be made with new");
                const Value p = rt.get(Value::object(nt), u"prototype");
                Object* m = rt.allocate<Object>(p.isObject() ? p.asObject() : proto, cls);
                if (!arg(a, 0).isNullish() && cls != ObjectClass::WeakRef) {
                    const Value adder = rt.get(Value::object(m), cls == ObjectClass::WeakMap ? u"set" : u"add");
                    for (const Value& e : rt.iterateToList(arg(a, 0))) {
                        if (cls == ObjectClass::WeakMap) {
                            if (!e.isObject()) rt.throwError("TypeError", "a WeakMap entry must be an object");
                            const Value kv[2] = {rt.get(e, u"0"), rt.get(e, u"1")};
                            rt.call(adder, Value::object(m), kv);
                        } else {
                            rt.call(adder, Value::object(m), std::span<const Value>(&e, 1));
                        }
                    }
                }
                return Value::object(m);
            }, proto);
            return std::make_pair(c, proto);
        };
        const auto thisWeak = [](Runtime& rt, const Value& t, ObjectClass cls) {
            if (!t.isObject() || t.asObject()->cls != cls) rt.throwError("TypeError", "the wrong kind of object for this method");
            return t.asObject();
        };
        auto [wm_ctor, wm] = weakCtor(u"WeakMap", ObjectClass::WeakMap);
        auto [ws_ctor, ws] = weakCtor(u"WeakSet", ObjectClass::WeakSet);
        (void)wm_ctor;
        (void)ws_ctor;
        for (Object* proto : {wm, ws}) {
            const ObjectClass cls = proto == wm ? ObjectClass::WeakMap : ObjectClass::WeakSet;
            method(proto, u"has", 1, [=](Runtime& rt, const Value& t, Args a) {
                return Value::boolean(thisWeak(rt, t, cls)->index.count(arg(a, 0)) != 0);
            });
            method(proto, u"delete", 1, [=](Runtime& rt, const Value& t, Args a) {
                Object* m = thisWeak(rt, t, cls);
                const auto it = m->index.find(arg(a, 0));
                if (it == m->index.end()) return Value::boolean(false);
                m->entries[it->second] = {Value::empty(), Value::undefined()};
                m->index.erase(it);
                return Value::boolean(true);
            });
        }
        method(wm, u"get", 1, [=](Runtime& rt, const Value& t, Args a) {
            Object* m = thisWeak(rt, t, ObjectClass::WeakMap);
            const auto it = m->index.find(arg(a, 0));
            return it == m->index.end() ? Value::undefined() : m->entries[it->second].second;
        });
        method(wm, u"set", 2, [=](Runtime& rt, const Value& t, Args a) {
            Object* m = thisWeak(rt, t, ObjectClass::WeakMap);
            if (!canBeHeldWeakly(rt, arg(a, 0))) rt.throwError("TypeError", "a WeakMap key must be an object or a symbol");
            if (const auto it = m->index.find(arg(a, 0)); it != m->index.end()) {
                m->entries[it->second].second = arg(a, 1);
            } else {
                m->index.emplace(arg(a, 0), m->entries.size());
                m->entries.emplace_back(arg(a, 0), arg(a, 1));
            }
            return t;
        });
        method(ws, u"add", 1, [=](Runtime& rt, const Value& t, Args a) {
            Object* m = thisWeak(rt, t, ObjectClass::WeakSet);
            if (!canBeHeldWeakly(rt, arg(a, 0))) rt.throwError("TypeError", "a WeakSet value must be an object or a symbol");
            if (!m->index.count(arg(a, 0))) {
                m->index.emplace(arg(a, 0), m->entries.size());
                m->entries.emplace_back(arg(a, 0), Value::undefined());
            }
            return t;
        });
        // WeakRef: its target is `primitive`, which the collector doesn't count as a reference.
        Object* wr = newObject();
        put(wr, PropertyKey(sym_to_string_tag), Value::string(u"WeakRef"), kConfigurable);
        ctor(u"WeakRef", 1, [wr, canBeHeldWeakly](Runtime& rt, const Value&, Args a) -> Value {
            Object* nt = rt.construct_target;
            if (!nt) rt.throwError("TypeError", "WeakRef must be made with new");
            if (!canBeHeldWeakly(rt, arg(a, 0))) rt.throwError("TypeError", "a WeakRef's target must be an object or a symbol");
            const Value p = rt.get(Value::object(nt), u"prototype");
            Object* r = rt.allocate<Object>(p.isObject() ? p.asObject() : wr, ObjectClass::WeakRef);
            r->primitive = arg(a, 0);
            return Value::object(r);
        }, wr);
        method(wr, u"deref", 0, [=](Runtime& rt, const Value& t, Args) { return thisWeak(rt, t, ObjectClass::WeakRef)->primitive; });
        // FinalizationRegistry: registrations are kept, but cleanup callbacks never run
        // (the specification allows a host never to call them).
        Object* fr = newObject();
        put(fr, PropertyKey(sym_to_string_tag), Value::string(u"FinalizationRegistry"), kConfigurable);
        ctor(u"FinalizationRegistry", 1, [fr](Runtime& rt, const Value&, Args a) -> Value {
            Object* nt = rt.construct_target;
            if (!nt) rt.throwError("TypeError", "FinalizationRegistry must be made with new");
            if (!rt.isCallable(arg(a, 0))) rt.throwError("TypeError", "FinalizationRegistry needs a cleanup function");
            const Value p = rt.get(Value::object(nt), u"prototype");
            return Value::object(rt.allocate<Object>(p.isObject() ? p.asObject() : fr));
        }, fr);
        method(fr, u"register", 2, [=](Runtime& rt, const Value&, Args a) {
            if (!canBeHeldWeakly(rt, arg(a, 0))) rt.throwError("TypeError", "the target must be an object or a symbol");
            return Value::undefined();
        });
        method(fr, u"unregister", 1, [](Runtime&, const Value&, Args) { return Value::boolean(false); });
    }

    // ── JSON ──
    Object* json = newObject();
    put(global_, u"JSON", Value::object(json));
    put(json, PropertyKey(sym_to_string_tag), Value::string(u"JSON"), kConfigurable);
    method(json, u"parse", 2, [](Runtime& rt, const Value&, Args a) {
        const std::u16string text = *rt.toString(arg(a, 0));
        Value v = JsonParser(rt, text).parse();
        const Value reviver = arg(a, 1);
        if (!rt.isCallable(reviver)) return v;
        // The reviver walks the result bottom up.
        const auto walk = [&](const Value& holder, const PropertyKey& key, auto& self) -> Value {
            const Value val = rt.get(holder, key);
            if (val.isObject()) {
                Object* o = val.asObject();
                std::vector<PropertyKey> keys;
                if (rt.isArray(val)) for (uint64_t i = 0; i < rt.lengthOf(o); ++i) keys.emplace_back(Runtime::numberToString(static_cast<double>(i)));
                else keys = enumerableOwnKeys(rt, o);
                for (const PropertyKey& k : keys) {
                    const Value nv = self(val, k, self);
                    if (nv.isUndefined()) rt.deleteProperty(o, k);
                    else rt.set(val, k, nv, false);
                }
            }
            const Value args[2] = {Value::string(key.name), val};
            return rt.call(reviver, holder, args);
        };
        Object* root = rt.newObject();
        rt.put(root, u"", v, kDefaultFlags);
        return walk(Value::object(root), PropertyKey(u""), walk);
    });
    method(json, u"stringify", 3, [](Runtime& rt, const Value&, Args a) {
        JsonWriter w{rt};
        const Value replacer = arg(a, 1);
        if (rt.isCallable(replacer)) {
            w.replacer_fn = replacer;
        } else if (replacer.isObject() && replacer.asObject()->cls == ObjectClass::Array) {
            w.has_allow = true;
            for (uint64_t i = 0; i < rt.lengthOf(replacer.asObject()); ++i) {
                const Value k = elementAt(rt, replacer.asObject(), i);
                if (k.isString() || k.isNumber()) {
                    const PropertyKey key(*rt.toString(k));
                    if (std::find(w.allow.begin(), w.allow.end(), key) == w.allow.end()) w.allow.push_back(key);
                }
            }
        }
        Value space = arg(a, 2);
        if (space.isObject()) space = space.asObject()->cls == ObjectClass::Number ? Value::number(rt.toNumber(space)) : space.asObject()->cls == ObjectClass::String ? Value::string(rt.toString(space)) : space;
        if (space.isNumber()) w.gap = std::u16string(static_cast<size_t>(std::clamp(rt.toIntegerOrInfinity(space), 0.0, 10.0)), u' ');
        else if (space.isString()) w.gap = space.asString().substr(0, 10);
        Object* root = rt.newObject();
        rt.put(root, u"", arg(a, 0), kDefaultFlags);
        std::u16string out;
        if (!w.write(Value::object(root), PropertyKey(u""), arg(a, 0), u"", out)) return Value::undefined();
        return Value::string(std::move(out));
    });

    // ── console ──
    Object* console = newObject();
    put(global_, u"console", Value::object(console));
    for (const char16_t* level : {u"log", u"info", u"warn", u"error", u"debug", u"trace"}) {
        const std::string lv = utf8(level);
        method(console, level, 0, [lv](Runtime& rt, const Value&, Args a) {
            std::string line;
            for (size_t i = 0; i < a.size(); ++i) {
                if (i) line += ' ';
                const Value& v = a[i];
                if (v.isString()) line += utf8(v.asString());
                else if (v.isSymbol()) line += "Symbol(" + utf8(v.asSymbol()->description) + ")";
                else if (v.isBigInt()) line += utf8(*rt.toString(v)) + "n";
                else if (v.isObject() && v.asObject()->cls == ObjectClass::Error) line += describe(rt, v) + describeWhere(rt, v);
                else if (v.isObject() && v.asObject()->cls == ObjectClass::RegExp) line += utf8(*rt.toString(v));
                else if (v.isObject() && !rt.isCallable(v)) {
                    // As JSON where it can be; [object …] where it can't.
                    try {
                        const Value js = rt.call(rt.get(rt.get(Value::object(rt.global()), u"JSON"), u"stringify"), Value::undefined(), std::span<const Value>(&v, 1));
                        line += js.isString() ? utf8(js.asString()) : "undefined";
                    } catch (const Thrown&) {
                        line += utf8(*rt.toString(v));
                    }
                } else {
                    line += utf8(*rt.toString(v));
                }
            }
            rt.emitConsole(lv, line);
            return Value::undefined();
        });
    }
    installRegExp();
    installPromise();
    installDate();
    installTypedArrays();
    installProxyAndReflect();
    installBigInt();
    installGenerators();
    (void)rt;
}

}  // namespace fbx::xweb::js
