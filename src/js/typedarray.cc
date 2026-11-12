// ArrayBuffer, the typed arrays and DataView (25.1 to 25.3, 23.2), and TextEncoder and
// TextDecoder from the Encoding Standard: bytes in a buffer, and windows on them that
// read and write numbers. BigInt64Array and BigUint64Array wait for BigInt.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "bigint.h"
#include "internal.h"
#include "xweb/encoding.h"

namespace fbx::xweb::js {

namespace {

using Args = std::span<const Value>;

Value arg(Args a, size_t i) { return i < a.size() ? a[i] : Value::undefined(); }

enum Kind : uint8_t { Int8, Uint8, Uint8C, Int16, Uint16, Int32, Uint32, Float32, Float64, BigInt64, BigUint64, kKinds };

const char16_t* const kNames[kKinds] = {u"Int8Array", u"Uint8Array", u"Uint8ClampedArray", u"Int16Array", u"Uint16Array",
                                        u"Int32Array", u"Uint32Array", u"Float32Array", u"Float64Array", u"BigInt64Array", u"BigUint64Array"};
const size_t kSizes[kKinds] = {1, 1, 1, 2, 2, 4, 4, 4, 8, 8, 8};

bool isBigKind(uint8_t kind) { return kind == BigInt64 || kind == BigUint64; }

Value bigValue(const bigint::Big& b) { return Value::bigint(std::make_shared<const std::u16string>(bigint::encode(b))); }

// ToBigInt.
Value toBigInt(Runtime& rt, const Value& v) {
    const Value p = rt.toPrimitive(v, "number");
    switch (p.type()) {
        case Value::Type::BigInt: return p;
        case Value::Type::Boolean: return bigValue(bigint::fromInt64(p.asBoolean() ? 1 : 0));
        case Value::Type::String: {
            const auto b = bigint::parse(p.asString());
            if (!b) rt.throwError("SyntaxError", "can't convert '" + utf8(p.asString()) + "' to a BigInt");
            return bigValue(*b);
        }
        default: rt.throwError("TypeError", "can't convert that to a BigInt");
    }
}

// What an element of this kind holds, converted from anything (ToNumber or ToBigInt).
Value toElement(Runtime& rt, uint8_t kind, const Value& v) {
    if (isBigKind(kind)) return toBigInt(rt, v);
    return Value::number(rt.toNumber(v));
}

constexpr size_t kMaxBytes = size_t{1} << 31;   // 2 GiB: more than a page should ask for

uint32_t toUint32(double d) {
    if (!std::isfinite(d)) return 0;
    d = std::trunc(d);
    d = std::fmod(d, 4294967296.0);
    if (d < 0) d += 4294967296.0;
    return static_cast<uint32_t>(d);
}

double readNumber(const uint8_t* p, uint8_t kind) {
    switch (kind) {
        case Int8: return static_cast<int8_t>(*p);
        case Uint8: case Uint8C: return *p;
        case Int16: { int16_t v; std::memcpy(&v, p, 2); return v; }
        case Uint16: { uint16_t v; std::memcpy(&v, p, 2); return v; }
        case Int32: { int32_t v; std::memcpy(&v, p, 4); return v; }
        case Uint32: { uint32_t v; std::memcpy(&v, p, 4); return v; }
        case Float32: { float v; std::memcpy(&v, p, 4); return v; }
        default: { double v; std::memcpy(&v, p, 8); return v; }
    }
}

void writeNumber(uint8_t* p, uint8_t kind, double d) {
    switch (kind) {
        case Int8: case Uint8: *p = static_cast<uint8_t>(toUint32(d)); break;
        case Uint8C: {
            const double c = std::isnan(d) ? 0 : std::clamp(d, 0.0, 255.0);
            *p = static_cast<uint8_t>(std::nearbyint(c));   // ties to even
            break;
        }
        case Int16: case Uint16: { const uint16_t v = static_cast<uint16_t>(toUint32(d)); std::memcpy(p, &v, 2); break; }
        case Int32: case Uint32: { const uint32_t v = toUint32(d); std::memcpy(p, &v, 4); break; }
        case Float32: { const float v = static_cast<float>(d); std::memcpy(p, &v, 4); break; }
        default: std::memcpy(p, &d, 8); break;
    }
}

BufferData* bufferOf(const Object* view) { return view->target && view->target->buffer ? view->target->buffer.get() : nullptr; }

bool detached(const Object* view) {
    const BufferData* b = bufferOf(view);
    return !b || b->detached;
}

// A view's byte length now, or 0 when it's out of its buffer's bounds.
size_t viewBytes(const Object* v, bool& out_of_bounds) {
    out_of_bounds = false;
    const BufferData* b = bufferOf(v);
    if (!b || b->detached) {
        out_of_bounds = true;
        return 0;
    }
    const size_t size = v->cls == ObjectClass::DataView ? 1 : kSizes[v->view_kind];
    if (v->view_offset > b->bytes.size()) {
        out_of_bounds = true;
        return 0;
    }
    if (v->view_tracking) {
        const size_t rest = b->bytes.size() - v->view_offset;
        return rest - rest % size;
    }
    const size_t bytes = v->view_length * size;
    if (v->view_offset + bytes > b->bytes.size()) {
        out_of_bounds = true;
        return 0;
    }
    return bytes;
}

Object* thisTyped(Runtime& rt, const Value& t, const char* what) {
    if (!t.isObject() || t.asObject()->cls != ObjectClass::TypedArray) rt.throwError("TypeError", std::string(what) + " called on something that isn't a typed array");
    return t.asObject();
}

// ValidateTypedArray: a typed array whose buffer is there and that's in its bounds.
Object* validTyped(Runtime& rt, const Value& t, const char* what) {
    Object* o = thisTyped(rt, t, what);
    bool oob;
    viewBytes(o, oob);
    if (oob) rt.throwError("TypeError", std::string(what) + ": the typed array's buffer is detached or too small");
    return o;
}

size_t toIndex(Runtime& rt, const Value& v) {
    if (v.isUndefined()) return 0;
    const double d = rt.toIntegerOrInfinity(v);
    if (d < 0 || d > 9007199254740991.0) rt.throwError("RangeError", "an invalid index or length");
    if (d > static_cast<double>(kMaxBytes)) rt.throwError("RangeError", "too large");
    return static_cast<size_t>(d);
}

// A relative index (negatives count from the end), clamped to [0, len].
size_t relative(Runtime& rt, const Value& v, size_t len, size_t if_undefined) {
    if (v.isUndefined()) return if_undefined;
    const double d = rt.toIntegerOrInfinity(v);
    if (d < 0) return d + static_cast<double>(len) < 0 ? 0 : static_cast<size_t>(d + static_cast<double>(len));
    return d > static_cast<double>(len) ? len : static_cast<size_t>(d);
}

Object* newBuffer(Runtime& rt, size_t bytes, Object* proto = nullptr) {
    if (bytes > kMaxBytes) rt.throwError("RangeError", "an ArrayBuffer that large can't be made");
    Object* b = rt.allocate<Object>(proto ? proto : rt.array_buffer_prototype, ObjectClass::ArrayBuffer);
    b->buffer = std::make_shared<BufferData>();
    b->buffer->bytes.assign(bytes, 0);
    return b;
}

Object* newTyped(Runtime& rt, uint8_t kind, Object* buffer, size_t offset, size_t length, bool tracking, Object* proto = nullptr) {
    if (!proto) proto = rt.get(Value::object(rt.typed_array_constructors[kind]), u"prototype").asObject();
    Object* t = rt.allocate<Object>(proto, ObjectClass::TypedArray);
    t->target = buffer;
    t->view_kind = kind;
    t->view_offset = offset;
    t->view_length = length;
    t->view_tracking = tracking;
    return t;
}

Object* newTypedOfLength(Runtime& rt, uint8_t kind, size_t length, Object* proto = nullptr) {
    if (length > kMaxBytes / kSizes[kind]) rt.throwError("RangeError", "a typed array that long can't be made");
    return newTyped(rt, kind, newBuffer(rt, length * kSizes[kind]), 0, length, false, proto);
}

uint8_t* elementPtr(const Object* t, size_t i) { return bufferOf(t)->bytes.data() + t->view_offset + i * kSizes[t->view_kind]; }

Value readValue(const uint8_t* p, uint8_t kind) {
    if (kind == BigInt64) { int64_t v; std::memcpy(&v, p, 8); return bigValue(bigint::fromInt64(v)); }
    if (kind == BigUint64) { uint64_t v; std::memcpy(&v, p, 8); return bigValue(bigint::fromUint64(v)); }
    return Value::number(readNumber(p, kind));
}

// `v` already converted for the kind (toElement).
void writeValue(uint8_t* p, uint8_t kind, const Value& v) {
    if (isBigKind(kind)) {
        const uint64_t bits = bigint::toUint64(bigint::decode(v.bigintDigits()));
        std::memcpy(p, &bits, 8);
        return;
    }
    writeNumber(p, kind, v.asNumber());
}

Value getV(const Object* t, size_t i) { return readValue(elementPtr(t, i), t->view_kind); }
void setV(Object* t, size_t i, const Value& v) { writeValue(elementPtr(t, i), t->view_kind, v); }

Value speciesConstructor(Runtime& rt, Object* o, Object* fallback) {
    const Value c = rt.get(Value::object(o), u"constructor");
    if (c.isUndefined()) return Value::object(fallback);
    if (!c.isObject()) rt.throwError("TypeError", "a constructor property isn't an object");
    const Value s = rt.get(c, PropertyKey(rt.sym_species));
    if (s.isNullish()) return Value::object(fallback);
    if (!rt.isConstructor(s)) rt.throwError("TypeError", "Symbol.species isn't a constructor");
    return s;
}

// TypedArrayCreate from a constructor, checked to be a typed array long enough.
Object* typedCreate(Runtime& rt, const Value& ctor, std::vector<Value> args, size_t min_length) {
    const Value made = rt.construct(ctor, args);
    if (!made.isObject() || made.asObject()->cls != ObjectClass::TypedArray) rt.throwError("TypeError", "the constructor didn't make a typed array");
    Object* t = made.asObject();
    bool oob;
    viewBytes(t, oob);
    if (oob) rt.throwError("TypeError", "the new typed array's buffer is detached");
    if (args.size() == 1 && args[0].isNumber() && typedLength(t) < min_length) rt.throwError("TypeError", "the new typed array is too short");
    return t;
}

Object* speciesCreate(Runtime& rt, Object* exemplar, std::vector<Value> args, size_t min_length = 0) {
    const Value c = speciesConstructor(rt, exemplar, rt.typed_array_constructors[exemplar->view_kind]);
    return typedCreate(rt, c, std::move(args), min_length);
}

// Copies values into a fresh typed array (from a list, an array-like or another typed array).
void fillFrom(Runtime& rt, Object* t, const std::vector<Value>& values) {
    for (size_t i = 0; i < values.size(); ++i) {
        const Value d = toElement(rt, t->view_kind, values[i]);
        if (i < typedLength(t)) setV(t, i, d);
    }
}

Value callback(Runtime& rt, const Value& fn, const char* what) {
    if (!rt.isCallable(fn)) rt.throwError("TypeError", std::string(what) + " needs a function");
    return fn;
}

// The number a sort comparator or the default order says.
bool lessThan(double x, double y) {
    if (std::isnan(y)) return !std::isnan(x);
    if (std::isnan(x)) return false;
    if (x == 0 && y == 0) return std::signbit(x) && !std::signbit(y);
    return x < y;
}

// ── Text ──

std::u16string decodeUtf8(Runtime& rt, const uint8_t* p, size_t n, bool fatal, bool ignore_bom) {
    std::u16string out;
    size_t i = 0;
    if (!ignore_bom && n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) i = 3;
    const auto bad = [&] {
        if (fatal) rt.throwError("TypeError", "the data isn't valid UTF-8");
        out += u'�';
    };
    while (i < n) {
        const uint8_t c = p[i];
        if (c < 0x80) {
            out += static_cast<char16_t>(c);
            ++i;
            continue;
        }
        int len = 0;
        uint32_t cp = 0, lower = 0x80, upper = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) { len = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 2; cp = c & 0x0F; if (c == 0xE0) lower = 0xA0; if (c == 0xED) upper = 0x9F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 3; cp = c & 0x07; if (c == 0xF0) lower = 0x90; if (c == 0xF4) upper = 0x8F; }
        else { bad(); ++i; continue; }
        size_t j = i + 1;
        bool ok = true;
        for (int k = 0; k < len; ++k, ++j) {
            if (j >= n || p[j] < lower || p[j] > upper) {
                ok = false;
                break;
            }
            lower = 0x80;
            upper = 0xBF;
            cp = (cp << 6) | (p[j] & 0x3F);
        }
        if (!ok) {
            bad();
            i = j;   // the maximal subpart is replaced once
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (cp >> 10));
            out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<char16_t>(cp);
        }
        i = j;
    }
    return out;
}

std::string encodeUtf8(const std::u16string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    return out;
}

// The bytes a BufferSource names: an ArrayBuffer, or a view's window on one.
std::pair<const uint8_t*, size_t> bytesOf(Runtime& rt, const Value& v) {
    if (v.isUndefined()) return {nullptr, 0};
    if (v.isObject()) {
        Object* o = v.asObject();
        if (o->cls == ObjectClass::ArrayBuffer && o->buffer) {
            if (o->buffer->detached) return {nullptr, 0};
            return {o->buffer->bytes.data(), o->buffer->bytes.size()};
        }
        if (o->cls == ObjectClass::TypedArray || o->cls == ObjectClass::DataView) {
            bool oob;
            const size_t n = viewBytes(o, oob);
            if (oob) return {nullptr, 0};
            return {bufferOf(o)->bytes.data() + o->view_offset, n};
        }
    }
    rt.throwError("TypeError", "not an ArrayBuffer or a view of one");
}

}  // namespace

// ── The runtime's hooks ──

bool typedKey(const PropertyKey& key, double& index) {
    const std::u16string& s = key.name;
    if (s.empty()) return false;
    const char16_t c = s[0];
    if (!(c == u'-' || (c >= u'0' && c <= u'9') || c == u'I' || c == u'N')) return false;
    if (s == u"-0") {
        index = -0.0;
        return true;
    }
    const double d = Runtime::stringToNumber(s);
    if (Runtime::numberToString(d) != s) return false;
    index = d;
    return true;
}

size_t typedLength(const Object* ta) {
    bool oob;
    const size_t bytes = viewBytes(ta, oob);
    return oob ? 0 : bytes / kSizes[ta->view_kind];
}

bool typedValidIndex(const Object* ta, double index) {
    if (detached(ta) || index != std::trunc(index) || std::isinf(index) || (index == 0 && std::signbit(index)) || index < 0) return false;
    return index < static_cast<double>(typedLength(ta));
}

Value typedGet(const Object* ta, double index) {
    if (!typedValidIndex(ta, index)) return Value::undefined();
    return getV(ta, static_cast<size_t>(index));
}

bool typedSet(Runtime& rt, Object* ta, double index, const Value& v) {
    const Value d = toElement(rt, ta->view_kind, v);   // even for an index that isn't there
    if (!typedValidIndex(ta, index)) return false;
    setV(ta, static_cast<size_t>(index), d);
    return true;
}

bool Runtime::detachArrayBuffer(const Value& v) {
    if (!v.isObject() || v.asObject()->cls != ObjectClass::ArrayBuffer || !v.asObject()->buffer) return false;
    BufferData& b = *v.asObject()->buffer;
    b.bytes = {};
    b.detached = true;
    return true;
}

Object* Runtime::newArrayBuffer(std::string_view bytes) {
    Object* b = newBuffer(*this, bytes.size());
    if (!bytes.empty()) std::memcpy(b->buffer->bytes.data(), bytes.data(), bytes.size());
    return b;
}

bool Runtime::bufferBytes(const Value& source, std::string& out) {
    if (!source.isObject()) return false;
    const ObjectClass c = source.asObject()->cls;
    if (c != ObjectClass::ArrayBuffer && c != ObjectClass::TypedArray && c != ObjectClass::DataView) return false;
    const auto [p, n] = bytesOf(*this, source);
    out.assign(reinterpret_cast<const char*>(p), n);
    return true;
}

// ── Installing ──

void Runtime::installTypedArrays() {
    const auto getter = [&](Object* o, const PropertyKey& key, const char16_t* name, NativeFunction fn) {
        Object* f = newFunction(std::u16string(u"get ") + name, 0, std::move(fn));
        o->define(key, {Value::undefined(), f, nullptr, kConfigurable | kAccessor});
    };
    const auto speciesGetter = [&](Object* c) {
        Object* f = newFunction(u"get [Symbol.species]", 0, [](Runtime&, const Value& t, Args) { return t; });
        c->define(PropertyKey(sym_species), {Value::undefined(), f, nullptr, kConfigurable | kAccessor});
    };

    // ── ArrayBuffer ──
    array_buffer_prototype = newObject();
    Object* ab = newFunction(u"ArrayBuffer", 1, [](Runtime& rt, const Value&, Args a) -> Value {
        Object* nt = rt.construct_target;
        if (!nt) rt.throwError("TypeError", "ArrayBuffer must be made with new");
        const size_t len = toIndex(rt, arg(a, 0));
        size_t max = 0;
        bool resizable = false;
        if (arg(a, 1).isObject()) {
            const Value m = rt.get(arg(a, 1), u"maxByteLength");
            if (!m.isUndefined()) {
                max = toIndex(rt, m);
                resizable = true;
                if (len > max) rt.throwError("RangeError", "the length is over the maximum");
            }
        }
        const Value p = rt.get(Value::object(nt), u"prototype");
        Object* b = newBuffer(rt, len, p.isObject() ? p.asObject() : rt.array_buffer_prototype);
        b->buffer->resizable = resizable;
        b->buffer->max_length = max;
        return Value::object(b);
    }, true);
    put(ab, u"prototype", Value::object(array_buffer_prototype), 0);
    put(array_buffer_prototype, u"constructor", Value::object(ab));
    put(array_buffer_prototype, PropertyKey(sym_to_string_tag), Value::string(u"ArrayBuffer"), kConfigurable);
    put(global_, u"ArrayBuffer", Value::object(ab));
    speciesGetter(ab);
    method(ab, u"isView", 1, [](Runtime&, const Value&, Args a) {
        const Value v = arg(a, 0);
        return Value::boolean(v.isObject() && (v.asObject()->cls == ObjectClass::TypedArray || v.asObject()->cls == ObjectClass::DataView));
    });
    const auto thisBuffer = [](Runtime& rt, const Value& t, const char* what) {
        if (!t.isObject() || t.asObject()->cls != ObjectClass::ArrayBuffer || !t.asObject()->buffer)
            rt.throwError("TypeError", std::string("ArrayBuffer.prototype.") + what + " called on something that isn't an ArrayBuffer");
        return t.asObject();
    };
    getter(array_buffer_prototype, u"byteLength", u"byteLength", [thisBuffer](Runtime& rt, const Value& t, Args) {
        Object* b = thisBuffer(rt, t, "byteLength");
        return Value::number(b->buffer->detached ? 0 : static_cast<double>(b->buffer->bytes.size()));
    });
    getter(array_buffer_prototype, u"maxByteLength", u"maxByteLength", [thisBuffer](Runtime& rt, const Value& t, Args) {
        Object* b = thisBuffer(rt, t, "maxByteLength");
        if (b->buffer->detached) return Value::number(0);
        return Value::number(static_cast<double>(b->buffer->resizable ? b->buffer->max_length : b->buffer->bytes.size()));
    });
    getter(array_buffer_prototype, u"resizable", u"resizable", [thisBuffer](Runtime& rt, const Value& t, Args) {
        return Value::boolean(thisBuffer(rt, t, "resizable")->buffer->resizable);
    });
    getter(array_buffer_prototype, u"detached", u"detached", [thisBuffer](Runtime& rt, const Value& t, Args) {
        return Value::boolean(thisBuffer(rt, t, "detached")->buffer->detached);
    });
    method(array_buffer_prototype, u"slice", 2, [thisBuffer](Runtime& rt, const Value& t, Args a) {
        Object* b = thisBuffer(rt, t, "slice");
        if (b->buffer->detached) rt.throwError("TypeError", "the ArrayBuffer is detached");
        const size_t len = b->buffer->bytes.size();
        const size_t first = relative(rt, arg(a, 0), len, 0);
        const size_t last = relative(rt, arg(a, 1), len, len);
        const size_t n = last > first ? last - first : 0;
        const Value c = speciesConstructor(rt, b, rt.get(Value::object(rt.global()), u"ArrayBuffer").asObject());
        const Value nv = Value::number(static_cast<double>(n));
        const Value made = rt.construct(c, std::span<const Value>(&nv, 1));
        if (!made.isObject() || made.asObject()->cls != ObjectClass::ArrayBuffer || made.asObject()->buffer->detached)
            rt.throwError("TypeError", "the species constructor didn't make an ArrayBuffer");
        Object* out = made.asObject();
        if (out == b) rt.throwError("TypeError", "the species constructor gave back the same buffer");
        if (out->buffer->bytes.size() < n) rt.throwError("TypeError", "the new ArrayBuffer is too small");
        if (b->buffer->detached) rt.throwError("TypeError", "the ArrayBuffer was detached");
        const size_t now_len = b->buffer->bytes.size();
        const size_t copy = first < now_len ? std::min(n, now_len - first) : 0;
        if (copy) std::memcpy(out->buffer->bytes.data(), b->buffer->bytes.data() + first, copy);
        return made;
    });
    method(array_buffer_prototype, u"resize", 1, [thisBuffer](Runtime& rt, const Value& t, Args a) {
        Object* b = thisBuffer(rt, t, "resize");
        if (!b->buffer->resizable) rt.throwError("TypeError", "the ArrayBuffer isn't resizable");
        const size_t n = toIndex(rt, arg(a, 0));
        if (b->buffer->detached) rt.throwError("TypeError", "the ArrayBuffer is detached");
        if (n > b->buffer->max_length) rt.throwError("RangeError", "over the ArrayBuffer's maximum length");
        b->buffer->bytes.resize(n, 0);
        return Value::undefined();
    });
    const auto transfer = [thisBuffer](bool fixed) {
        return [thisBuffer, fixed](Runtime& rt, const Value& t, Args a) {
            Object* b = thisBuffer(rt, t, "transfer");
            const size_t n = arg(a, 0).isUndefined() ? (b->buffer->detached ? 0 : b->buffer->bytes.size()) : toIndex(rt, arg(a, 0));
            if (b->buffer->detached) rt.throwError("TypeError", "the ArrayBuffer is detached");
            Object* out = newBuffer(rt, 0);
            out->buffer->bytes = std::move(b->buffer->bytes);
            out->buffer->bytes.resize(n, 0);
            if (!fixed && b->buffer->resizable) {
                out->buffer->resizable = true;
                out->buffer->max_length = b->buffer->max_length;
                if (n > out->buffer->max_length) rt.throwError("RangeError", "over the ArrayBuffer's maximum length");
            }
            b->buffer->bytes = {};
            b->buffer->detached = true;
            return Value::object(out);
        };
    };
    method(array_buffer_prototype, u"transfer", 0, transfer(false));
    method(array_buffer_prototype, u"transferToFixedLength", 0, transfer(true));

    // ── %TypedArray% ──
    typed_array_prototype = newObject();
    Object* ta = newFunction(u"TypedArray", 0, [](Runtime& rt, const Value&, Args) -> Value {
        rt.throwError("TypeError", "TypedArray is abstract: use Uint8Array and the rest");
    }, true);
    put(ta, u"prototype", Value::object(typed_array_prototype), 0);
    put(typed_array_prototype, u"constructor", Value::object(ta));
    speciesGetter(ta);
    Object* P = typed_array_prototype;
    getter(P, u"buffer", u"buffer", [](Runtime& rt, const Value& t, Args) { return Value::object(thisTyped(rt, t, "buffer")->target); });
    getter(P, u"byteLength", u"byteLength", [](Runtime& rt, const Value& t, Args) {
        Object* o = thisTyped(rt, t, "byteLength");
        bool oob;
        return Value::number(static_cast<double>(viewBytes(o, oob)));
    });
    getter(P, u"byteOffset", u"byteOffset", [](Runtime& rt, const Value& t, Args) {
        Object* o = thisTyped(rt, t, "byteOffset");
        bool oob;
        viewBytes(o, oob);
        return Value::number(oob ? 0 : static_cast<double>(o->view_offset));
    });
    getter(P, u"length", u"length", [](Runtime& rt, const Value& t, Args) { return Value::number(static_cast<double>(typedLength(thisTyped(rt, t, "length")))); });
    getter(P, PropertyKey(sym_to_string_tag), u"[Symbol.toStringTag]", [](Runtime&, const Value& t, Args) {
        if (!t.isObject() || t.asObject()->cls != ObjectClass::TypedArray) return Value::undefined();
        return Value::string(kNames[t.asObject()->view_kind]);
    });

    // The methods that share Array's generic code exactly.
    for (const char16_t* name : {u"join", u"indexOf", u"lastIndexOf", u"includes", u"find", u"findIndex", u"findLast", u"findLastIndex",
                                 u"every", u"some", u"forEach", u"reduce", u"reduceRight", u"at", u"toLocaleString"}) {
        const Value generic = get(Value::object(array_prototype), name);
        const std::string what = "%TypedArray%.prototype." + utf8(name);
        method(P, name, static_cast<int>(toNumber(get(generic, u"length"))), [generic, what](Runtime& rt, const Value& t, Args a) {
            validTyped(rt, t, what.c_str());
            return rt.call(generic, t, a);
        });
    }
    put(P, u"toString", get(Value::object(array_prototype), u"toString"));
    method(P, u"values", 0, [](Runtime& rt, const Value& t, Args) {
        Object* o = validTyped(rt, t, "values");
        Object* it = rt.allocate<Object>(rt.array_iterator_prototype, ObjectClass::Iterator);
        it->target = o;
        it->iter_kind = 0;
        return Value::object(it);
    });
    method(P, u"keys", 0, [](Runtime& rt, const Value& t, Args) {
        Object* it = rt.allocate<Object>(rt.array_iterator_prototype, ObjectClass::Iterator);
        it->target = validTyped(rt, t, "keys");
        it->iter_kind = 1;
        return Value::object(it);
    });
    method(P, u"entries", 0, [](Runtime& rt, const Value& t, Args) {
        Object* it = rt.allocate<Object>(rt.array_iterator_prototype, ObjectClass::Iterator);
        it->target = validTyped(rt, t, "entries");
        it->iter_kind = 2;
        return Value::object(it);
    });
    put(P, PropertyKey(sym_iterator), get(Value::object(P), u"values"));

    method(P, u"fill", 1, [](Runtime& rt, const Value& t, Args a) {
        Object* o = validTyped(rt, t, "fill");
        const size_t len = typedLength(o);
        const Value v = toElement(rt, o->view_kind, arg(a, 0));
        const size_t first = relative(rt, arg(a, 1), len, 0);
        const size_t last = relative(rt, arg(a, 2), len, len);
        validTyped(rt, t, "fill");
        const size_t now = typedLength(o);
        for (size_t i = first; i < last && i < now; ++i) setV(o, i, v);
        return t;
    });
    method(P, u"set", 1, [](Runtime& rt, const Value& t, Args a) {
        Object* o = thisTyped(rt, t, "set");
        const double off = rt.toIntegerOrInfinity(arg(a, 1));
        if (off < 0) rt.throwError("RangeError", "the offset can't be negative");
        bool oob;
        viewBytes(o, oob);
        if (oob) rt.throwError("TypeError", "the typed array's buffer is detached");
        const size_t len = typedLength(o);
        const Value src = arg(a, 0);
        if (src.isObject() && src.asObject()->cls == ObjectClass::TypedArray) {
            Object* s = src.asObject();
            if (detached(s)) rt.throwError("TypeError", "the source's buffer is detached");
            const size_t n = typedLength(s);
            if (off + static_cast<double>(n) > static_cast<double>(len)) rt.throwError("RangeError", "the source doesn't fit");
            if (isBigKind(s->view_kind) != isBigKind(o->view_kind)) rt.throwError("TypeError", "BigInt and number typed arrays don't mix");
            std::vector<Value> vals(n);   // read first: they may share a buffer
            for (size_t i = 0; i < n; ++i) vals[i] = getV(s, i);
            for (size_t i = 0; i < n; ++i) setV(o, static_cast<size_t>(off) + i, vals[i]);
            return Value::undefined();
        }
        Object* s = rt.toObject(src);
        const uint64_t n = rt.lengthOf(s);
        if (off + static_cast<double>(n) > static_cast<double>(len)) rt.throwError("RangeError", "the source doesn't fit");
        for (uint64_t i = 0; i < n; ++i) {
            typedSet(rt, o, off + static_cast<double>(i), rt.get(s, PropertyKey(Runtime::numberToString(static_cast<double>(i))), Value::object(s)));
        }
        return Value::undefined();
    });
    method(P, u"subarray", 2, [](Runtime& rt, const Value& t, Args a) {
        Object* o = thisTyped(rt, t, "subarray");
        const size_t len = typedLength(o);
        const size_t first = relative(rt, arg(a, 0), len, 0);
        const bool to_end = o->view_tracking && arg(a, 1).isUndefined();
        const size_t last = relative(rt, arg(a, 1), len, len);
        std::vector<Value> args{Value::object(o->target), Value::number(static_cast<double>(o->view_offset + first * kSizes[o->view_kind]))};
        if (!to_end) args.push_back(Value::number(static_cast<double>(last > first ? last - first : 0)));
        return Value::object(speciesCreate(rt, o, std::move(args)));
    });
    method(P, u"slice", 2, [](Runtime& rt, const Value& t, Args a) {
        Object* o = validTyped(rt, t, "slice");
        const size_t len = typedLength(o);
        const size_t first = relative(rt, arg(a, 0), len, 0);
        const size_t last = relative(rt, arg(a, 1), len, len);
        const size_t n = last > first ? last - first : 0;
        Object* out = speciesCreate(rt, o, {Value::number(static_cast<double>(n))}, n);
        if (n) {
            validTyped(rt, t, "slice");
            const size_t now = typedLength(o);
            for (size_t i = 0; i < n && first + i < now; ++i) setV(out, i, getV(o, first + i));
        }
        return Value::object(out);
    });
    method(P, u"map", 1, [](Runtime& rt, const Value& t, Args a) {
        Object* o = validTyped(rt, t, "map");
        const Value fn = callback(rt, arg(a, 0), "map");
        const size_t len = typedLength(o);
        Object* out = speciesCreate(rt, o, {Value::number(static_cast<double>(len))}, len);
        for (size_t i = 0; i < len; ++i) {
            const Value args[3] = {typedGet(o, static_cast<double>(i)), Value::number(static_cast<double>(i)), t};
            typedSet(rt, out, static_cast<double>(i), rt.call(fn, arg(a, 1), args));
        }
        return Value::object(out);
    });
    method(P, u"filter", 1, [](Runtime& rt, const Value& t, Args a) {
        Object* o = validTyped(rt, t, "filter");
        const Value fn = callback(rt, arg(a, 0), "filter");
        const size_t len = typedLength(o);
        std::vector<Value> kept;
        for (size_t i = 0; i < len; ++i) {
            const Value v = typedGet(o, static_cast<double>(i));
            const Value args[3] = {v, Value::number(static_cast<double>(i)), t};
            if (rt.toBoolean(rt.call(fn, arg(a, 1), args))) kept.push_back(v);
        }
        Object* out = speciesCreate(rt, o, {Value::number(static_cast<double>(kept.size()))}, kept.size());
        fillFrom(rt, out, kept);
        return Value::object(out);
    });
    method(P, u"reverse", 0, [](Runtime& rt, const Value& t, Args) {
        Object* o = validTyped(rt, t, "reverse");
        const size_t len = typedLength(o);
        for (size_t i = 0; i < len / 2; ++i) {
            const Value x = getV(o, i), y = getV(o, len - 1 - i);
            setV(o, i, y);
            setV(o, len - 1 - i, x);
        }
        return t;
    });
    method(P, u"copyWithin", 2, [](Runtime& rt, const Value& t, Args a) {
        Object* o = validTyped(rt, t, "copyWithin");
        const size_t len = typedLength(o);
        const size_t to = relative(rt, arg(a, 0), len, 0);
        const size_t from = relative(rt, arg(a, 1), len, 0);
        const size_t last = relative(rt, arg(a, 2), len, len);
        size_t count = last > from ? std::min(last - from, len - to) : 0;
        if (count) {
            validTyped(rt, t, "copyWithin");
            const size_t now = typedLength(o);
            if (to + count > now) count = to < now ? now - to : 0;
            if (from + count > now) count = from < now ? now - from : 0;
            const size_t size = kSizes[o->view_kind];
            if (count) std::memmove(elementPtr(o, to), elementPtr(o, from), count * size);
        }
        return t;
    });
    const auto sortValues = [](Runtime& rt, Object* o, const Value& cmp) {
        const size_t len = typedLength(o);
        std::vector<Value> v(len);
        for (size_t i = 0; i < len; ++i) v[i] = getV(o, i);
        if (cmp.isUndefined()) {
            if (isBigKind(o->view_kind))
                std::stable_sort(v.begin(), v.end(), [](const Value& x, const Value& y) {
                    return bigint::compare(bigint::decode(x.bigintDigits()), bigint::decode(y.bigintDigits())) < 0;
                });
            else
                std::stable_sort(v.begin(), v.end(), [](const Value& x, const Value& y) { return lessThan(x.asNumber(), y.asNumber()); });
        } else {
            std::stable_sort(v.begin(), v.end(), [&](const Value& x, const Value& y) {
                const Value args[2] = {x, y};
                const double r = rt.toNumber(rt.call(cmp, Value::undefined(), args));
                return r < 0;
            });
        }
        return v;
    };
    method(P, u"sort", 1, [sortValues](Runtime& rt, const Value& t, Args a) {
        if (!arg(a, 0).isUndefined() && !rt.isCallable(arg(a, 0))) rt.throwError("TypeError", "sort's comparator must be a function");
        Object* o = validTyped(rt, t, "sort");
        const std::vector<Value> v = sortValues(rt, o, arg(a, 0));
        const size_t now = typedLength(o);
        for (size_t i = 0; i < v.size() && i < now; ++i) setV(o, i, v[i]);
        return t;
    });
    method(P, u"toSorted", 1, [sortValues](Runtime& rt, const Value& t, Args a) {
        if (!arg(a, 0).isUndefined() && !rt.isCallable(arg(a, 0))) rt.throwError("TypeError", "toSorted's comparator must be a function");
        Object* o = validTyped(rt, t, "toSorted");
        Object* out = newTypedOfLength(rt, o->view_kind, typedLength(o));
        const std::vector<Value> v = sortValues(rt, o, arg(a, 0));
        for (size_t i = 0; i < v.size(); ++i) setV(out, i, v[i]);
        return Value::object(out);
    });
    method(P, u"toReversed", 0, [](Runtime& rt, const Value& t, Args) {
        Object* o = validTyped(rt, t, "toReversed");
        const size_t len = typedLength(o);
        Object* out = newTypedOfLength(rt, o->view_kind, len);
        for (size_t i = 0; i < len; ++i) setV(out, i, getV(o, len - 1 - i));
        return Value::object(out);
    });
    method(P, u"with", 2, [](Runtime& rt, const Value& t, Args a) {
        Object* o = validTyped(rt, t, "with");
        const size_t len = typedLength(o);
        const double rel = rt.toIntegerOrInfinity(arg(a, 0));
        const double index = rel >= 0 ? rel : static_cast<double>(len) + rel;
        const Value v = toElement(rt, o->view_kind, arg(a, 1));
        if (!typedValidIndex(o, index)) rt.throwError("RangeError", "an index out of range");
        Object* out = newTypedOfLength(rt, o->view_kind, len);
        for (size_t i = 0; i < len; ++i) setV(out, i, i == static_cast<size_t>(index) ? v : getV(o, i));
        return Value::object(out);
    });

    // TypedArray.from and of.
    method(ta, u"from", 1, [](Runtime& rt, const Value& t, Args a) {
        if (!rt.isConstructor(t)) rt.throwError("TypeError", "TypedArray.from needs a constructor as this");
        const Value fn = arg(a, 1);
        if (!fn.isUndefined() && !rt.isCallable(fn)) rt.throwError("TypeError", "the map function must be a function");
        const Value src = arg(a, 0);
        std::vector<Value> values;
        const Value iter = src.isNullish() ? Value::undefined() : rt.get(src, PropertyKey(rt.sym_iterator));
        if (!iter.isNullish()) {
            values = rt.iterateToList(src);
        } else {
            Object* s = rt.toObject(src);
            const uint64_t n = rt.lengthOf(s);
            for (uint64_t i = 0; i < n; ++i) values.push_back(rt.get(s, PropertyKey(Runtime::numberToString(static_cast<double>(i))), Value::object(s)));
        }
        Object* out = typedCreate(rt, t, {Value::number(static_cast<double>(values.size()))}, values.size());
        for (size_t i = 0; i < values.size(); ++i) {
            Value v = values[i];
            if (!fn.isUndefined()) {
                const Value args[2] = {v, Value::number(static_cast<double>(i))};
                v = rt.call(fn, arg(a, 2), args);
            }
            typedSet(rt, out, static_cast<double>(i), v);
        }
        return Value::object(out);
    });
    method(ta, u"of", 0, [](Runtime& rt, const Value& t, Args a) {
        if (!rt.isConstructor(t)) rt.throwError("TypeError", "TypedArray.of needs a constructor as this");
        Object* out = typedCreate(rt, t, {Value::number(static_cast<double>(a.size()))}, a.size());
        for (size_t i = 0; i < a.size(); ++i) typedSet(rt, out, static_cast<double>(i), a[i]);
        return Value::object(out);
    });

    // ── The concrete kinds ──
    for (uint8_t kind = 0; kind < kKinds; ++kind) {
        Object* proto = newObject(typed_array_prototype);
        Object* c = newFunction(kNames[kind], 3, [kind, proto](Runtime& rt, const Value&, Args a) -> Value {
            Object* nt = rt.construct_target;
            if (!nt) rt.throwError("TypeError", utf8(kNames[kind]) + " must be made with new");
            const Value pv = rt.get(Value::object(nt), u"prototype");
            Object* p = pv.isObject() ? pv.asObject() : proto;
            const size_t size = kSizes[kind];
            const Value first = arg(a, 0);
            if (!first.isObject()) return Value::object(newTypedOfLength(rt, kind, toIndex(rt, first), p));
            Object* src = first.asObject();
            if (src->cls == ObjectClass::ArrayBuffer && src->buffer) {
                const size_t offset = toIndex(rt, arg(a, 1));
                if (offset % size) rt.throwError("RangeError", "the offset must be a multiple of the element size");
                size_t length = 0;
                const bool has_length = !arg(a, 2).isUndefined();
                if (has_length) length = toIndex(rt, arg(a, 2));
                if (src->buffer->detached) rt.throwError("TypeError", "the ArrayBuffer is detached");
                const size_t buf_len = src->buffer->bytes.size();
                if (!has_length && src->buffer->resizable) {
                    if (offset > buf_len) rt.throwError("RangeError", "the offset is past the buffer's end");
                    return Value::object(newTyped(rt, kind, src, offset, 0, true, p));
                }
                if (!has_length) {
                    if (buf_len % size) rt.throwError("RangeError", "the buffer's length must be a multiple of the element size");
                    if (offset > buf_len) rt.throwError("RangeError", "the offset is past the buffer's end");
                    length = (buf_len - offset) / size;
                } else if (offset + length * size > buf_len) {
                    rt.throwError("RangeError", "the typed array doesn't fit in its buffer");
                }
                return Value::object(newTyped(rt, kind, src, offset, length, false, p));
            }
            if (src->cls == ObjectClass::TypedArray) {
                if (detached(src)) rt.throwError("TypeError", "the source's buffer is detached");
                if (isBigKind(src->view_kind) != isBigKind(kind)) rt.throwError("TypeError", "BigInt and number typed arrays don't mix");
                const size_t n = typedLength(src);
                Object* out = newTypedOfLength(rt, kind, n, p);
                for (size_t i = 0; i < n; ++i) setV(out, i, getV(src, i));
                return Value::object(out);
            }
            std::vector<Value> values;
            const Value iter = rt.get(first, PropertyKey(rt.sym_iterator));
            if (!iter.isNullish()) {
                if (!rt.isCallable(iter)) rt.throwError("TypeError", "Symbol.iterator isn't a function");
                values = rt.iterateToList(first);
            } else {
                const uint64_t n = rt.lengthOf(src);
                if (n > kMaxBytes) rt.throwError("RangeError", "too long");
                for (uint64_t i = 0; i < n; ++i) values.push_back(rt.get(src, PropertyKey(Runtime::numberToString(static_cast<double>(i))), first));
            }
            Object* out = newTypedOfLength(rt, kind, values.size(), p);
            fillFrom(rt, out, values);
            return Value::object(out);
        }, true);
        c->prototype = ta;
        put(c, u"prototype", Value::object(proto), 0);
        put(c, u"BYTES_PER_ELEMENT", Value::number(static_cast<double>(kSizes[kind])), 0);
        put(proto, u"constructor", Value::object(c));
        put(proto, u"BYTES_PER_ELEMENT", Value::number(static_cast<double>(kSizes[kind])), 0);
        put(global_, kNames[kind], Value::object(c));
        typed_array_constructors[kind] = c;
    }

    // ── DataView ──
    data_view_prototype = newObject();
    Object* dv = newFunction(u"DataView", 1, [](Runtime& rt, const Value&, Args a) -> Value {
        Object* nt = rt.construct_target;
        if (!nt) rt.throwError("TypeError", "DataView must be made with new");
        const Value b = arg(a, 0);
        if (!b.isObject() || b.asObject()->cls != ObjectClass::ArrayBuffer || !b.asObject()->buffer) rt.throwError("TypeError", "DataView needs an ArrayBuffer");
        Object* buf = b.asObject();
        const size_t offset = toIndex(rt, arg(a, 1));
        if (buf->buffer->detached) rt.throwError("TypeError", "the ArrayBuffer is detached");
        const size_t len = buf->buffer->bytes.size();
        if (offset > len) rt.throwError("RangeError", "the offset is past the buffer's end");
        const bool tracking = arg(a, 2).isUndefined() && buf->buffer->resizable;
        size_t view_len = len - offset;
        if (!arg(a, 2).isUndefined()) {
            view_len = toIndex(rt, arg(a, 2));
            if (offset + view_len > len) rt.throwError("RangeError", "the view doesn't fit in its buffer");
        }
        const Value p = rt.get(Value::object(nt), u"prototype");
        if (buf->buffer->detached) rt.throwError("TypeError", "the ArrayBuffer is detached");
        Object* v = rt.allocate<Object>(p.isObject() ? p.asObject() : rt.data_view_prototype, ObjectClass::DataView);
        v->target = buf;
        v->view_offset = offset;
        v->view_length = view_len;
        v->view_tracking = tracking;
        return Value::object(v);
    }, true);
    put(dv, u"prototype", Value::object(data_view_prototype), 0);
    put(data_view_prototype, u"constructor", Value::object(dv));
    put(data_view_prototype, PropertyKey(sym_to_string_tag), Value::string(u"DataView"), kConfigurable);
    put(global_, u"DataView", Value::object(dv));
    const auto thisView = [](Runtime& rt, const Value& t) {
        if (!t.isObject() || t.asObject()->cls != ObjectClass::DataView) rt.throwError("TypeError", "not a DataView");
        return t.asObject();
    };
    getter(data_view_prototype, u"buffer", u"buffer", [thisView](Runtime& rt, const Value& t, Args) { return Value::object(thisView(rt, t)->target); });
    getter(data_view_prototype, u"byteLength", u"byteLength", [thisView](Runtime& rt, const Value& t, Args) {
        bool oob;
        Object* v = thisView(rt, t);
        if (detached(v)) return Value::number(0);
        const size_t n = viewBytes(v, oob);
        if (oob) rt.throwError("TypeError", "the DataView's buffer is detached or too small");
        return Value::number(static_cast<double>(n));
    });
    getter(data_view_prototype, u"byteOffset", u"byteOffset", [thisView](Runtime& rt, const Value& t, Args) {
        Object* v = thisView(rt, t);
        bool oob;
        viewBytes(v, oob);
        if (oob) rt.throwError("TypeError", "the DataView's buffer is detached or too small");
        return Value::number(static_cast<double>(v->view_offset));
    });
    static const std::pair<const char16_t*, uint8_t> kViewTypes[] = {{u"Int8", Int8}, {u"Uint8", Uint8}, {u"Int16", Int16}, {u"Uint16", Uint16},
                                                                     {u"Int32", Int32}, {u"Uint32", Uint32}, {u"Float32", Float32}, {u"Float64", Float64}};
    for (const uint8_t k : {uint8_t(BigInt64), uint8_t(BigUint64)}) {
        const std::u16string name = k == BigInt64 ? u"BigInt64" : u"BigUint64";
        method(data_view_prototype, (u"get" + name).c_str(), 1, [thisView, k](Runtime& rt, const Value& t, Args a) {
            Object* v = thisView(rt, t);
            const size_t index = toIndex(rt, arg(a, 0));
            const bool little = rt.toBoolean(arg(a, 1));
            bool oob;
            const size_t n = viewBytes(v, oob);
            if (oob) rt.throwError("TypeError", "the DataView's buffer is detached or too small");
            if (index + 8 > n) rt.throwError("RangeError", "an offset outside the DataView");
            uint8_t bytes[8];
            std::memcpy(bytes, bufferOf(v)->bytes.data() + v->view_offset + index, 8);
            if (!little) std::reverse(bytes, bytes + 8);
            return readValue(bytes, k);
        });
        method(data_view_prototype, (u"set" + name).c_str(), 2, [thisView, k](Runtime& rt, const Value& t, Args a) {
            Object* v = thisView(rt, t);
            const size_t index = toIndex(rt, arg(a, 0));
            const Value b = toBigInt(rt, arg(a, 1));
            const bool little = rt.toBoolean(arg(a, 2));
            bool oob;
            const size_t n = viewBytes(v, oob);
            if (oob) rt.throwError("TypeError", "the DataView's buffer is detached or too small");
            if (index + 8 > n) rt.throwError("RangeError", "an offset outside the DataView");
            uint8_t bytes[8];
            writeValue(bytes, k, b);
            if (!little) std::reverse(bytes, bytes + 8);
            std::memcpy(bufferOf(v)->bytes.data() + v->view_offset + index, bytes, 8);
            return Value::undefined();
        });
    }
    for (const auto& [name, kind] : kViewTypes) {
        const uint8_t k = kind;
        method(data_view_prototype, (std::u16string(u"get") + name).c_str(), 1, [thisView, k](Runtime& rt, const Value& t, Args a) {
            Object* v = thisView(rt, t);
            const size_t index = toIndex(rt, arg(a, 0));
            const bool little = rt.toBoolean(arg(a, 1));
            bool oob;
            const size_t n = viewBytes(v, oob);
            if (oob) rt.throwError("TypeError", "the DataView's buffer is detached or too small");
            if (index + kSizes[k] > n) rt.throwError("RangeError", "an offset outside the DataView");
            uint8_t bytes[8];
            std::memcpy(bytes, bufferOf(v)->bytes.data() + v->view_offset + index, kSizes[k]);
            if (!little) std::reverse(bytes, bytes + kSizes[k]);
            return Value::number(readNumber(bytes, k));
        });
        method(data_view_prototype, (std::u16string(u"set") + name).c_str(), 2, [thisView, k](Runtime& rt, const Value& t, Args a) {
            Object* v = thisView(rt, t);
            const size_t index = toIndex(rt, arg(a, 0));
            const double d = rt.toNumber(arg(a, 1));
            const bool little = rt.toBoolean(arg(a, 2));
            bool oob;
            const size_t n = viewBytes(v, oob);
            if (oob) rt.throwError("TypeError", "the DataView's buffer is detached or too small");
            if (index + kSizes[k] > n) rt.throwError("RangeError", "an offset outside the DataView");
            uint8_t bytes[8];
            writeNumber(bytes, k, d);
            if (!little) std::reverse(bytes, bytes + kSizes[k]);
            std::memcpy(bufferOf(v)->bytes.data() + v->view_offset + index, bytes, kSizes[k]);
            return Value::undefined();
        });
    }

    // ── TextEncoder, TextDecoder ──
    Object* te_proto = newObject();
    Object* te = newFunction(u"TextEncoder", 0, [te_proto](Runtime& rt, const Value&, Args) -> Value {
        Object* nt = rt.construct_target;
        if (!nt) rt.throwError("TypeError", "TextEncoder must be made with new");
        const Value p = rt.get(Value::object(nt), u"prototype");
        return Value::object(rt.newObject(p.isObject() ? p.asObject() : te_proto));
    }, true);
    put(te, u"prototype", Value::object(te_proto), 0);
    put(te_proto, u"constructor", Value::object(te));
    put(global_, u"TextEncoder", Value::object(te));
    getter(te_proto, u"encoding", u"encoding", [](Runtime&, const Value&, Args) { return Value::string(u"utf-8"); });
    method(te_proto, u"encode", 0, [](Runtime& rt, const Value&, Args a) {
        const std::string bytes = arg(a, 0).isUndefined() ? std::string() : encodeUtf8(*rt.toString(arg(a, 0)));
        Object* out = newTypedOfLength(rt, Uint8, bytes.size());
        if (!bytes.empty()) std::memcpy(elementPtr(out, 0), bytes.data(), bytes.size());
        return Value::object(out);
    });
    method(te_proto, u"encodeInto", 2, [](Runtime& rt, const Value&, Args a) {
        const std::u16string s = *rt.toString(arg(a, 0));
        const Value dest = arg(a, 1);
        if (!dest.isObject() || dest.asObject()->cls != ObjectClass::TypedArray || dest.asObject()->view_kind != Uint8)
            rt.throwError("TypeError", "encodeInto needs a Uint8Array");
        Object* d = dest.asObject();
        const size_t room = typedLength(d);
        size_t read = 0, written = 0;
        for (size_t i = 0; i < s.size();) {
            const size_t step = (s[i] >= 0xD800 && s[i] <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) ? 2 : 1;
            const std::string bytes = encodeUtf8(s.substr(i, step));
            if (written + bytes.size() > room) break;
            std::memcpy(elementPtr(d, written), bytes.data(), bytes.size());
            written += bytes.size();
            read += step;
            i += step;
        }
        Object* r = rt.newObject();
        rt.put(r, u"read", Value::number(static_cast<double>(read)), kDefaultFlags);
        rt.put(r, u"written", Value::number(static_cast<double>(written)), kDefaultFlags);
        return Value::object(r);
    });

    Object* td_proto = newObject();
    // A decoder's encoding is `primitive` ("utf-8", "utf-16le", "utf-16be", "windows-1252", "euc-kr");
    // its fatal and ignoreBOM flags are `cursor` bits 1 and 2.
    Object* td = newFunction(u"TextDecoder", 0, [td_proto](Runtime& rt, const Value&, Args a) -> Value {
        Object* nt = rt.construct_target;
        if (!nt) rt.throwError("TypeError", "TextDecoder must be made with new");
        std::string label = arg(a, 0).isUndefined() ? "utf-8" : utf8(*rt.toString(arg(a, 0)));
        while (!label.empty() && std::isspace(static_cast<unsigned char>(label.back()))) label.pop_back();
        while (!label.empty() && std::isspace(static_cast<unsigned char>(label.front()))) label.erase(label.begin());
        for (char& c : label) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::u16string enc;
        if (label == "utf-8" || label == "utf8" || label == "unicode-1-1-utf-8") enc = u"utf-8";
        else if (label == "utf-16le" || label == "utf-16" || label == "ucs-2" || label == "unicode") enc = u"utf-16le";
        else if (label == "utf-16be") enc = u"utf-16be";
        else if (label == "windows-1252" || label == "latin1" || label == "iso-8859-1" || label == "ascii" || label == "us-ascii" || label == "l1" || label == "cp1252")
            enc = u"windows-1252";
        else if (isEucKrLabel(label)) enc = u"euc-kr";
        else rt.throwError("RangeError", "the encoding '" + label + "' isn't supported");
        const Value p = rt.get(Value::object(nt), u"prototype");
        Object* d = rt.newObject(p.isObject() ? p.asObject() : td_proto);
        d->primitive = Value::string(enc);
        if (arg(a, 1).isObject()) {
            if (rt.toBoolean(rt.get(arg(a, 1), u"fatal"))) d->cursor |= 1;
            if (rt.toBoolean(rt.get(arg(a, 1), u"ignoreBOM"))) d->cursor |= 2;
        }
        return Value::object(d);
    }, true);
    put(td, u"prototype", Value::object(td_proto), 0);
    put(td_proto, u"constructor", Value::object(td));
    put(global_, u"TextDecoder", Value::object(td));
    const auto thisDecoder = [](Runtime& rt, const Value& t) {
        if (!t.isObject() || !t.asObject()->primitive.isString()) rt.throwError("TypeError", "not a TextDecoder");
        return t.asObject();
    };
    getter(td_proto, u"encoding", u"encoding", [thisDecoder](Runtime& rt, const Value& t, Args) { return thisDecoder(rt, t)->primitive; });
    getter(td_proto, u"fatal", u"fatal", [thisDecoder](Runtime& rt, const Value& t, Args) { return Value::boolean(thisDecoder(rt, t)->cursor & 1); });
    getter(td_proto, u"ignoreBOM", u"ignoreBOM", [thisDecoder](Runtime& rt, const Value& t, Args) { return Value::boolean(thisDecoder(rt, t)->cursor & 2); });
    method(td_proto, u"decode", 0, [thisDecoder](Runtime& rt, const Value& t, Args a) {
        Object* d = thisDecoder(rt, t);
        const auto [p, n] = bytesOf(rt, arg(a, 0));
        const std::u16string& enc = d->primitive.asString();
        const bool fatal = d->cursor & 1, ignore_bom = d->cursor & 2;
        if (enc == u"utf-8") return Value::string(decodeUtf8(rt, p, n, fatal, ignore_bom));
        if (enc == u"euc-kr") {
            const std::string text = eucKrToUtf8(std::string_view(reinterpret_cast<const char*>(p), n));
            if (fatal && text.find("\xEF\xBF\xBD") != std::string::npos) rt.throwError("TypeError", "the data isn't valid EUC-KR");
            return Value::string(utf16(text));
        }
        std::u16string out;
        if (enc == u"windows-1252") {
            static const char16_t k80[32] = {0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F,
                                             0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178};
            for (size_t i = 0; i < n; ++i) out += p[i] >= 0x80 && p[i] < 0xA0 ? k80[p[i] - 0x80] : static_cast<char16_t>(p[i]);
            return Value::string(out);
        }
        const bool big = enc == u"utf-16be";
        size_t i = 0;
        if (!ignore_bom && n >= 2 && ((big && p[0] == 0xFE && p[1] == 0xFF) || (!big && p[0] == 0xFF && p[1] == 0xFE))) i = 2;
        for (; i + 1 < n; i += 2) out += static_cast<char16_t>(big ? (p[i] << 8) | p[i + 1] : (p[i + 1] << 8) | p[i]);
        if (i < n) {
            if (fatal) rt.throwError("TypeError", "the data isn't valid UTF-16");
            out += u'�';
        }
        return Value::string(out);
    });
}

}  // namespace fbx::xweb::js
