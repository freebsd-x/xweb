// The BigInt function and BigInt.prototype (21.2).

#include "bigint.h"
#include "internal.h"

namespace fbx::xweb::js {

namespace {

using Args = std::span<const Value>;

Value arg(Args a, size_t i) { return i < a.size() ? a[i] : Value::undefined(); }

Value bigValue(const bigint::Big& b) { return Value::bigint(std::make_shared<const std::u16string>(bigint::encode(b))); }

bigint::Big toBig(Runtime& rt, const Value& v) {
    const Value p = rt.toPrimitive(v, "number");
    switch (p.type()) {
        case Value::Type::BigInt: return bigint::decode(p.bigintDigits());
        case Value::Type::Boolean: return bigint::fromInt64(p.asBoolean() ? 1 : 0);
        case Value::Type::String: {
            const auto b = bigint::parse(p.asString());
            if (!b) rt.throwError("SyntaxError", "can't convert '" + utf8(p.asString()) + "' to a BigInt");
            return *b;
        }
        default: rt.throwError("TypeError", "can't convert that to a BigInt");
    }
}

bigint::Big thisBig(Runtime& rt, const Value& t, const char* what) {
    if (t.isBigInt()) return bigint::decode(t.bigintDigits());
    if (t.isObject() && t.asObject()->cls == ObjectClass::BigInt && t.asObject()->primitive.isBigInt())
        return bigint::decode(t.asObject()->primitive.bigintDigits());
    rt.throwError("TypeError", std::string("BigInt.prototype.") + what + " called on something that isn't a BigInt");
}

uint64_t toIndex(Runtime& rt, const Value& v) {
    const double d = rt.toIntegerOrInfinity(v);
    if (d < 0 || d > 9007199254740991.0) rt.throwError("RangeError", "an invalid number of bits");
    return static_cast<uint64_t>(d);
}

}  // namespace

void Runtime::installBigInt() {
    bigint_prototype = newObject();
    Object* ctor = newFunction(u"BigInt", 1, [](Runtime& rt, const Value&, Args a) {
        if (rt.construct_target) rt.throwError("TypeError", "BigInt isn't a constructor");
        const Value p = rt.toPrimitive(arg(a, 0), "number");
        if (p.isNumber()) {
            const auto b = bigint::fromDouble(p.asNumber());
            if (!b) rt.throwError("RangeError", "only an integer can become a BigInt");
            return bigValue(*b);
        }
        return bigValue(toBig(rt, p));
    }, true);   // marked constructible so `new BigInt` reaches the TypeError above
    put(ctor, u"prototype", Value::object(bigint_prototype), 0);
    put(bigint_prototype, u"constructor", Value::object(ctor));
    put(bigint_prototype, PropertyKey(sym_to_string_tag), Value::string(u"BigInt"), kConfigurable);
    put(global_, u"BigInt", Value::object(ctor));
    method(ctor, u"asIntN", 2, [](Runtime& rt, const Value&, Args a) {
        const uint64_t bits = toIndex(rt, arg(a, 0));
        return bigValue(bigint::asIntN(toBig(rt, arg(a, 1)), bits));
    });
    method(ctor, u"asUintN", 2, [](Runtime& rt, const Value&, Args a) {
        const uint64_t bits = toIndex(rt, arg(a, 0));
        const bigint::Big b = toBig(rt, arg(a, 1));
        if (b.neg && bits > bigint::kMaxBits) rt.throwError("RangeError", "Maximum BigInt size exceeded");
        return bigValue(bigint::asUintN(b, bits));
    });
    method(bigint_prototype, u"toString", 0, [](Runtime& rt, const Value& t, Args a) {
        const bigint::Big b = thisBig(rt, t, "toString");
        double radix = 10;
        if (!arg(a, 0).isUndefined()) radix = rt.toIntegerOrInfinity(arg(a, 0));
        if (radix < 2 || radix > 36) rt.throwError("RangeError", "toString's radix must be from 2 to 36");
        return Value::string(bigint::toString(b, static_cast<int>(radix)));
    });
    method(bigint_prototype, u"toLocaleString", 0, [](Runtime& rt, const Value& t, Args) {
        // en-US grouping, as Intl would give it.
        std::u16string s = bigint::toString(thisBig(rt, t, "toLocaleString"));
        const size_t start = s[0] == u'-' ? 1 : 0;
        for (size_t i = s.size(); i > start + 3; i -= 3) s.insert(i - 3, u",");
        return Value::string(s);
    });
    method(bigint_prototype, u"valueOf", 0, [](Runtime& rt, const Value& t, Args) { return bigValue(thisBig(rt, t, "valueOf")); });
}

double bigintToNumber(const Value& v) { return bigint::toDouble(bigint::decode(v.bigintDigits())); }

}  // namespace fbx::xweb::js
