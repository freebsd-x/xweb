// Proxy and Reflect (28.1, 28.2): an object whose every internal operation goes through a
// handler's trap, or on to its target when the handler has none; the checks that keep a
// handler from lying about what the target can't change; and Reflect, the same operations
// as functions. The object's internal methods dispatch here when they meet a proxy.

#include "internal.h"

namespace fbx::xweb::js {

namespace {

using Args = std::span<const Value>;

Value arg(Args a, size_t i) { return i < a.size() ? a[i] : Value::undefined(); }

Value keyValue(const PropertyKey& k) { return k.isSymbol() ? Value::symbol(k.symbol) : Value::string(k.name); }

std::string keyText(const PropertyKey& k) { return k.isSymbol() ? std::string("a symbol") : "'" + utf8(k.name) + "'"; }

// The handler and target, or a TypeError for a revoked proxy.
std::pair<Object*, Object*> parts(Runtime& rt, Object* p) {
    if (!p->target || p->slots.empty() || !p->slots[0].isObject()) rt.throwError("TypeError", "the proxy has been revoked");
    return {p->slots[0].asObject(), p->target};
}

Value trap(Runtime& rt, Object* handler, const char16_t* name) {
    const Value t = rt.get(Value::object(handler), name);
    if (t.isNullish()) return Value::undefined();
    if (!rt.isCallable(t)) rt.throwError("TypeError", "the proxy handler's " + utf8(name) + " isn't a function");
    return t;
}

Value call(Runtime& rt, const Value& fn, Object* handler, std::vector<Value> args) { return rt.call(fn, Value::object(handler), args); }

bool dataNonConfigurable(const Property& p) { return !(p.flags & kAccessor) && !(p.flags & kConfigurable); }

// IsCompatiblePropertyDescriptor: could `desc` (a normalized descriptor object) be applied
// to a property that's `current`?
bool compatible(Runtime& rt, const Property& current, const Value& desc) {
    Object* d = desc.asObject();
    const Property asked = descriptorToProperty(rt, desc);
    const bool has_conf = rt.has(d, u"configurable"), has_enum = rt.has(d, u"enumerable");
    const bool has_value = rt.has(d, u"value"), has_writable = rt.has(d, u"writable");
    const bool has_get = rt.has(d, u"get"), has_set = rt.has(d, u"set");
    if (current.flags & kConfigurable) return true;
    if (has_conf && (asked.flags & kConfigurable)) return false;
    if (has_enum && ((asked.flags ^ current.flags) & kEnumerable)) return false;
    const bool generic = !has_value && !has_writable && !has_get && !has_set;
    if (generic) return true;
    const bool asked_accessor = has_get || has_set;
    if (asked_accessor != ((current.flags & kAccessor) != 0)) return false;
    if (!asked_accessor) {
        if (!(current.flags & kWritable)) {
            if (has_writable && (asked.flags & kWritable)) return false;
            if (has_value && !Runtime::sameValue(asked.value, current.value)) return false;
        }
        return true;
    }
    if (has_get && asked.getter != current.getter) return false;
    if (has_set && asked.setter != current.setter) return false;
    return true;
}

}  // namespace

// ── The proxy's internal methods ──

Object* proxyGetPrototypeOf(Runtime& rt, Object* p) {
    auto [handler, target] = parts(rt, p);
    const Value t = trap(rt, handler, u"getPrototypeOf");
    if (t.isUndefined()) return rt.getPrototypeOf(target);
    const Value r = call(rt, t, handler, {Value::object(target)});
    if (!r.isObject() && !r.isNull()) rt.throwError("TypeError", "getPrototypeOf trap gave something that isn't an object or null");
    Object* proto = r.isObject() ? r.asObject() : nullptr;
    if (!rt.isExtensible(target) && proto != rt.getPrototypeOf(target))
        rt.throwError("TypeError", "getPrototypeOf trap disagrees with a non-extensible target");
    return proto;
}

bool proxySetPrototypeOf(Runtime& rt, Object* p, Object* proto) {
    auto [handler, target] = parts(rt, p);
    const Value t = trap(rt, handler, u"setPrototypeOf");
    if (t.isUndefined()) return rt.setPrototypeOf(target, proto);
    const bool ok = rt.toBoolean(call(rt, t, handler, {Value::object(target), proto ? Value::object(proto) : Value::null()}));
    if (!ok) return false;
    if (!rt.isExtensible(target) && proto != rt.getPrototypeOf(target))
        rt.throwError("TypeError", "setPrototypeOf trap claims to change a non-extensible target");
    return true;
}

bool proxyIsExtensible(Runtime& rt, Object* p) {
    auto [handler, target] = parts(rt, p);
    const Value t = trap(rt, handler, u"isExtensible");
    if (t.isUndefined()) return rt.isExtensible(target);
    const bool r = rt.toBoolean(call(rt, t, handler, {Value::object(target)}));
    if (r != rt.isExtensible(target)) rt.throwError("TypeError", "isExtensible trap disagrees with the target");
    return r;
}

bool proxyPreventExtensions(Runtime& rt, Object* p) {
    auto [handler, target] = parts(rt, p);
    const Value t = trap(rt, handler, u"preventExtensions");
    if (t.isUndefined()) return rt.preventExtensions(target);
    const bool r = rt.toBoolean(call(rt, t, handler, {Value::object(target)}));
    if (r && rt.isExtensible(target)) rt.throwError("TypeError", "preventExtensions trap claims success but the target is still extensible");
    return r;
}

bool proxyGetOwnProperty(Runtime& rt, Object* p, const PropertyKey& key, Property& out) {
    auto [handler, target] = parts(rt, p);
    const Value t = trap(rt, handler, u"getOwnPropertyDescriptor");
    if (t.isUndefined()) return rt.getOwnProperty(target, key, out);
    const Value r = call(rt, t, handler, {Value::object(target), keyValue(key)});
    if (!r.isObject() && !r.isUndefined()) rt.throwError("TypeError", "getOwnPropertyDescriptor trap gave something that isn't an object or undefined");
    Property target_prop;
    const bool target_has = rt.getOwnProperty(target, key, target_prop);
    if (r.isUndefined()) {
        if (target_has && !(target_prop.flags & kConfigurable))
            rt.throwError("TypeError", "getOwnPropertyDescriptor trap hides the non-configurable property " + keyText(key));
        if (target_has && !rt.isExtensible(target)) rt.throwError("TypeError", "getOwnPropertyDescriptor trap hides a property of a non-extensible target");
        return false;
    }
    if (!target_has && !rt.isExtensible(target)) rt.throwError("TypeError", "getOwnPropertyDescriptor trap reports a property a non-extensible target doesn't have");
    out = descriptorToProperty(rt, r);
    // Completed as a property descriptor is.
    if (!(out.flags & kAccessor) && out.value.isEmpty()) out.value = Value::undefined();
    if (!(out.flags & kConfigurable)) {
        if (!target_has || (target_prop.flags & kConfigurable))
            rt.throwError("TypeError", "getOwnPropertyDescriptor trap reports " + keyText(key) + " as non-configurable but the target's isn't");
        if (!(out.flags & kAccessor) && !(out.flags & kWritable) && !(target_prop.flags & kAccessor) && (target_prop.flags & kWritable))
            rt.throwError("TypeError", "getOwnPropertyDescriptor trap reports " + keyText(key) + " as read-only but the target's is writable");
    }
    return true;
}

bool proxyDefineOwnProperty(Runtime& rt, Object* p, const PropertyKey& key, const Value& desc) {
    auto [handler, target] = parts(rt, p);
    const Value t = trap(rt, handler, u"defineProperty");
    if (t.isUndefined()) return rt.defineOwnProperty(target, key, desc, false);
    // The trap sees a fresh descriptor object, as ToPropertyDescriptor/FromPropertyDescriptor make it.
    const Value normalized = normalizeDescriptor(rt, desc);
    if (!rt.toBoolean(call(rt, t, handler, {Value::object(target), keyValue(key), normalized}))) return false;
    const Property asked = descriptorToProperty(rt, normalized);
    const bool asks_non_configurable = rt.has(normalized.asObject(), u"configurable") && !(asked.flags & kConfigurable);
    Property target_prop;
    const bool target_has = rt.getOwnProperty(target, key, target_prop);
    if (!target_has) {
        if (!rt.isExtensible(target)) rt.throwError("TypeError", "defineProperty trap adds a property to a non-extensible target");
        if (asks_non_configurable) rt.throwError("TypeError", "defineProperty trap defines a non-configurable property the target doesn't have");
    } else {
        if (!compatible(rt, target_prop, normalized)) rt.throwError("TypeError", "defineProperty trap claims a change the target's " + keyText(key) + " can't take");
        if (asks_non_configurable && (target_prop.flags & kConfigurable))
            rt.throwError("TypeError", "defineProperty trap claims " + keyText(key) + " is non-configurable but the target's isn't");
        if (dataNonConfigurable(target_prop) && (target_prop.flags & kWritable) && rt.has(normalized.asObject(), u"writable") && !(asked.flags & kWritable))
            rt.throwError("TypeError", "defineProperty trap claims " + keyText(key) + " is read-only but the target's is writable");
    }
    return true;
}

bool proxyHas(Runtime& rt, Object* p, const PropertyKey& key) {
    auto [handler, target] = parts(rt, p);
    const Value t = trap(rt, handler, u"has");
    if (t.isUndefined()) return rt.has(target, key);
    const bool r = rt.toBoolean(call(rt, t, handler, {Value::object(target), keyValue(key)}));
    if (!r) {
        Property target_prop;
        if (rt.getOwnProperty(target, key, target_prop)) {
            if (!(target_prop.flags & kConfigurable)) rt.throwError("TypeError", "has trap hides the non-configurable property " + keyText(key));
            if (!rt.isExtensible(target)) rt.throwError("TypeError", "has trap hides a property of a non-extensible target");
        }
    }
    return r;
}

Value proxyGet(Runtime& rt, Object* p, const PropertyKey& key, const Value& receiver) {
    auto [handler, target] = parts(rt, p);
    const Value t = trap(rt, handler, u"get");
    if (t.isUndefined()) return rt.get(target, key, receiver);
    const Value r = call(rt, t, handler, {Value::object(target), keyValue(key), receiver});
    Property target_prop;
    if (rt.getOwnProperty(target, key, target_prop) && !(target_prop.flags & kConfigurable)) {
        if (!(target_prop.flags & kAccessor) && !(target_prop.flags & kWritable) && !Runtime::sameValue(r, target_prop.value))
            rt.throwError("TypeError", "get trap reports a different value for the read-only, non-configurable " + keyText(key));
        if ((target_prop.flags & kAccessor) && !target_prop.getter && !r.isUndefined())
            rt.throwError("TypeError", "get trap reports a value for " + keyText(key) + ", which has no getter on the target");
    }
    return r;
}

bool proxySet(Runtime& rt, Object* p, const PropertyKey& key, const Value& v, const Value& receiver) {
    auto [handler, target] = parts(rt, p);
    const Value t = trap(rt, handler, u"set");
    if (t.isUndefined()) return rt.set(target, key, v, receiver);
    if (!rt.toBoolean(call(rt, t, handler, {Value::object(target), keyValue(key), v, receiver}))) return false;
    Property target_prop;
    if (rt.getOwnProperty(target, key, target_prop) && !(target_prop.flags & kConfigurable)) {
        if (!(target_prop.flags & kAccessor) && !(target_prop.flags & kWritable) && !Runtime::sameValue(v, target_prop.value))
            rt.throwError("TypeError", "set trap claims to change the read-only, non-configurable " + keyText(key));
        if ((target_prop.flags & kAccessor) && !target_prop.setter)
            rt.throwError("TypeError", "set trap claims to set " + keyText(key) + ", which has no setter on the target");
    }
    return true;
}

bool proxyDelete(Runtime& rt, Object* p, const PropertyKey& key) {
    auto [handler, target] = parts(rt, p);
    const Value t = trap(rt, handler, u"deleteProperty");
    if (t.isUndefined()) return rt.deleteProperty(target, key);
    if (!rt.toBoolean(call(rt, t, handler, {Value::object(target), keyValue(key)}))) return false;
    Property target_prop;
    if (rt.getOwnProperty(target, key, target_prop)) {
        if (!(target_prop.flags & kConfigurable)) rt.throwError("TypeError", "deleteProperty trap claims to delete the non-configurable " + keyText(key));
        if (!rt.isExtensible(target)) rt.throwError("TypeError", "deleteProperty trap claims to delete a property of a non-extensible target");
    }
    return true;
}

std::vector<PropertyKey> proxyOwnKeys(Runtime& rt, Object* p) {
    auto [handler, target] = parts(rt, p);
    const Value t = trap(rt, handler, u"ownKeys");
    if (t.isUndefined()) return rt.ownPropertyKeys(target);
    const Value r = call(rt, t, handler, {Value::object(target)});
    if (!r.isObject()) rt.throwError("TypeError", "ownKeys trap gave something that isn't an object");
    std::vector<PropertyKey> keys;
    Object* list = r.asObject();
    const uint64_t n = rt.lengthOf(list);
    if (n > 10'000'000) rt.throwError("RangeError", "ownKeys trap gave too many keys");
    for (uint64_t i = 0; i < n; ++i) {
        const Value k = rt.get(list, PropertyKey(Runtime::numberToString(static_cast<double>(i))), r);
        if (!k.isString() && !k.isSymbol()) rt.throwError("TypeError", "ownKeys trap gave a key that isn't a string or a symbol");
        const PropertyKey key = k.isSymbol() ? PropertyKey(k.asSymbol()) : PropertyKey(k.asString());
        if (std::find(keys.begin(), keys.end(), key) != keys.end()) rt.throwError("TypeError", "ownKeys trap gave a key twice");
        keys.push_back(key);
    }
    // Everything the target can't lose must be there; a non-extensible target's keys exactly.
    const bool extensible = rt.isExtensible(target);
    std::vector<PropertyKey> target_keys = rt.ownPropertyKeys(target);
    for (const PropertyKey& k : target_keys) {
        Property tp;
        const bool has = rt.getOwnProperty(target, k, tp);
        const bool listed = std::find(keys.begin(), keys.end(), k) != keys.end();
        if (has && !(tp.flags & kConfigurable) && !listed) rt.throwError("TypeError", "ownKeys trap leaves out the non-configurable " + keyText(k));
        if (!extensible && !listed) rt.throwError("TypeError", "ownKeys trap leaves out a key of a non-extensible target");
    }
    if (!extensible)
        for (const PropertyKey& k : keys)
            if (std::find(target_keys.begin(), target_keys.end(), k) == target_keys.end())
                rt.throwError("TypeError", "ownKeys trap adds a key to a non-extensible target");
    return keys;
}

Value proxyCall(Runtime& rt, Object* p, const Value& this_value, std::span<const Value> args) {
    auto [handler, target] = parts(rt, p);
    const Value t = trap(rt, handler, u"apply");
    if (t.isUndefined()) return rt.call(Value::object(target), this_value, args);
    return call(rt, t, handler, {Value::object(target), this_value, Value::object(rt.newArray(std::vector<Value>(args.begin(), args.end())))});
}

Value proxyConstruct(Runtime& rt, Object* p, std::span<const Value> args, Object* new_target) {
    auto [handler, target] = parts(rt, p);
    const Value t = trap(rt, handler, u"construct");
    if (t.isUndefined()) return rt.construct(Value::object(target), args, new_target == p ? target : new_target);
    const Value r = call(rt, t, handler, {Value::object(target), Value::object(rt.newArray(std::vector<Value>(args.begin(), args.end()))),
                                          Value::object(new_target)});
    if (!r.isObject()) rt.throwError("TypeError", "construct trap gave something that isn't an object");
    return r;
}

// ── Generic internal methods, a proxy's or an ordinary object's ──

Object* Runtime::getPrototypeOf(Object* o) {
    if (o->cls == ObjectClass::Proxy) return proxyGetPrototypeOf(*this, o);
    return o->prototype;
}

bool Runtime::setPrototypeOf(Object* o, Object* proto) {
    if (o->cls == ObjectClass::Proxy) return proxySetPrototypeOf(*this, o, proto);
    if (o->prototype == proto) return true;
    if (!o->extensible) return false;
    // No cycles (a proxy in the chain ends the check, as the spec has it).
    for (Object* c = proto; c; c = c->prototype) {
        if (c == o) return false;
        if (c->cls == ObjectClass::Proxy) break;
    }
    // The global object and Object.prototype keep theirs (immutable prototype exotic).
    if (o == object_prototype) return false;
    o->prototype = proto;
    return true;
}

bool Runtime::isExtensible(Object* o) {
    if (o->cls == ObjectClass::Proxy) return proxyIsExtensible(*this, o);
    return o->extensible;
}

bool Runtime::preventExtensions(Object* o) {
    if (o->cls == ObjectClass::Proxy) return proxyPreventExtensions(*this, o);
    o->extensible = false;
    return true;
}

bool Runtime::isArray(const Value& v) {
    if (!v.isObject()) return false;
    Object* o = v.asObject();
    for (int hops = 0; o && o->cls == ObjectClass::Proxy && hops < 100'000; ++hops) {
        if (!o->target) throwError("TypeError", "the proxy has been revoked");
        o = o->target;
    }
    return o && o->cls == ObjectClass::Array;
}

// ── Installing ──

void Runtime::installProxyAndReflect() {
    // Proxy and Proxy.revocable.
    const auto make = [](Runtime& rt, const Value& target, const Value& handler) {
        if (!target.isObject() || !handler.isObject()) rt.throwError("TypeError", "a Proxy needs a target object and a handler object");
        Object* p = rt.allocate<Object>(nullptr, ObjectClass::Proxy);
        p->target = target.asObject();
        p->slots = {handler};
        if (rt.isCallable(target)) {
            p->iter_kind = 1;   // callable
            p->constructor = rt.isConstructor(target);
        }
        return p;
    };
    Object* proxy = newFunction(u"Proxy", 2, [make](Runtime& rt, const Value&, Args a) -> Value {
        if (!rt.construct_target) rt.throwError("TypeError", "Proxy must be called with new");
        return Value::object(make(rt, arg(a, 0), arg(a, 1)));
    }, true);
    method(proxy, u"revocable", 2, [make](Runtime& rt, const Value&, Args a) {
        Object* p = make(rt, arg(a, 0), arg(a, 1));
        Object* revoke = rt.newFunction(u"", 0, nullptr);
        revoke->slots = {Value::object(p)};
        revoke->native = [revoke](Runtime&, const Value&, Args) {
            if (!revoke->slots.empty() && revoke->slots[0].isObject()) {
                Object* target = revoke->slots[0].asObject();
                target->target = nullptr;
                target->slots.clear();
            }
            revoke->slots.clear();
            return Value::undefined();
        };
        Object* r = rt.newObject();
        rt.put(r, u"proxy", Value::object(p), kDefaultFlags);
        rt.put(r, u"revoke", Value::object(revoke), kDefaultFlags);
        return Value::object(r);
    });
    put(global_, u"Proxy", Value::object(proxy));

    // Reflect: each internal method as a function.
    Object* reflect = newObject();
    put(reflect, PropertyKey(sym_to_string_tag), Value::string(u"Reflect"), kConfigurable);
    put(global_, u"Reflect", Value::object(reflect));
    const auto target = [](Runtime& rt, const Value& v, const char* what) {
        if (!v.isObject()) rt.throwError("TypeError", std::string("Reflect.") + what + " needs an object");
        return v.asObject();
    };
    method(reflect, u"apply", 3, [](Runtime& rt, const Value&, Args a) {
        if (!rt.isCallable(arg(a, 0))) rt.throwError("TypeError", "Reflect.apply needs a function");
        if (!arg(a, 2).isObject()) rt.throwError("TypeError", "Reflect.apply's arguments must be an array-like object");
        Object* list = arg(a, 2).asObject();
        std::vector<Value> args;
        const uint64_t n = rt.lengthOf(list);
        for (uint64_t i = 0; i < n; ++i) args.push_back(rt.get(list, PropertyKey(Runtime::numberToString(static_cast<double>(i))), arg(a, 2)));
        return rt.call(arg(a, 0), arg(a, 1), args);
    });
    method(reflect, u"construct", 2, [](Runtime& rt, const Value&, Args a) {
        if (!rt.isConstructor(arg(a, 0))) rt.throwError("TypeError", "Reflect.construct needs a constructor");
        const Value nt = a.size() > 2 ? a[2] : arg(a, 0);
        if (!rt.isConstructor(nt)) rt.throwError("TypeError", "Reflect.construct's newTarget must be a constructor");
        if (!arg(a, 1).isObject()) rt.throwError("TypeError", "Reflect.construct's arguments must be an array-like object");
        Object* list = arg(a, 1).asObject();
        std::vector<Value> args;
        const uint64_t n = rt.lengthOf(list);
        for (uint64_t i = 0; i < n; ++i) args.push_back(rt.get(list, PropertyKey(Runtime::numberToString(static_cast<double>(i))), arg(a, 1)));
        return rt.construct(arg(a, 0), args, nt.asObject());
    });
    method(reflect, u"defineProperty", 3, [target](Runtime& rt, const Value&, Args a) {
        Object* o = target(rt, arg(a, 0), "defineProperty");
        const PropertyKey key = rt.toPropertyKey(arg(a, 1));
        if (!arg(a, 2).isObject()) rt.throwError("TypeError", "a property descriptor must be an object");
        return Value::boolean(rt.defineOwnProperty(o, key, arg(a, 2), false));
    });
    method(reflect, u"deleteProperty", 2, [target](Runtime& rt, const Value&, Args a) {
        Object* o = target(rt, arg(a, 0), "deleteProperty");
        return Value::boolean(rt.deleteProperty(o, rt.toPropertyKey(arg(a, 1))));
    });
    method(reflect, u"get", 2, [target](Runtime& rt, const Value&, Args a) {
        Object* o = target(rt, arg(a, 0), "get");
        return rt.get(o, rt.toPropertyKey(arg(a, 1)), a.size() > 2 ? a[2] : arg(a, 0));
    });
    method(reflect, u"getOwnPropertyDescriptor", 2, [target](Runtime& rt, const Value&, Args a) {
        Object* o = target(rt, arg(a, 0), "getOwnPropertyDescriptor");
        Property p;
        if (!rt.getOwnProperty(o, rt.toPropertyKey(arg(a, 1)), p)) return Value::undefined();
        return propertyToDescriptor(rt, p);
    });
    method(reflect, u"getPrototypeOf", 1, [target](Runtime& rt, const Value&, Args a) {
        Object* p = rt.getPrototypeOf(target(rt, arg(a, 0), "getPrototypeOf"));
        return p ? Value::object(p) : Value::null();
    });
    method(reflect, u"has", 2, [target](Runtime& rt, const Value&, Args a) {
        Object* o = target(rt, arg(a, 0), "has");
        return Value::boolean(rt.has(o, rt.toPropertyKey(arg(a, 1))));
    });
    method(reflect, u"isExtensible", 1, [target](Runtime& rt, const Value&, Args a) {
        return Value::boolean(rt.isExtensible(target(rt, arg(a, 0), "isExtensible")));
    });
    method(reflect, u"ownKeys", 1, [target](Runtime& rt, const Value&, Args a) {
        std::vector<Value> out;
        for (const PropertyKey& k : rt.ownPropertyKeys(target(rt, arg(a, 0), "ownKeys"))) out.push_back(keyValue(k));
        return Value::object(rt.newArray(std::move(out)));
    });
    method(reflect, u"preventExtensions", 1, [target](Runtime& rt, const Value&, Args a) {
        return Value::boolean(rt.preventExtensions(target(rt, arg(a, 0), "preventExtensions")));
    });
    method(reflect, u"set", 3, [target](Runtime& rt, const Value&, Args a) {
        Object* o = target(rt, arg(a, 0), "set");
        return Value::boolean(rt.set(o, rt.toPropertyKey(arg(a, 1)), arg(a, 2), a.size() > 3 ? a[3] : arg(a, 0)));
    });
    // Object.groupBy and Map.groupBy.
    const auto groupBy = [](Runtime& rt, const Value& items, const Value& fn, bool property_keys, std::vector<std::pair<Value, std::vector<Value>>>& groups) {
        if (items.isNullish()) rt.throwError("TypeError", "groupBy needs something iterable");
        if (!rt.isCallable(fn)) rt.throwError("TypeError", "groupBy needs a callback");
        const std::vector<Value> list = rt.iterateToList(items);
        for (size_t i = 0; i < list.size(); ++i) {
            const Value args[2] = {list[i], Value::number(static_cast<double>(i))};
            Value key = rt.call(fn, Value::undefined(), args);
            if (property_keys) {
                const PropertyKey pk = rt.toPropertyKey(key);
                key = keyValue(pk);
            } else if (key.isNumber() && key.asNumber() == 0) {
                key = Value::number(0);
            }
            auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return Runtime::sameValue(g.first, key); });
            if (it == groups.end()) groups.push_back({key, {list[i]}});
            else it->second.push_back(list[i]);
        }
    };
    {
        const Value object_ctor = get(Value::object(global_), u"Object");
        method(object_ctor.asObject(), u"groupBy", 2, [groupBy](Runtime& rt, const Value&, Args a) {
            std::vector<std::pair<Value, std::vector<Value>>> groups;
            groupBy(rt, arg(a, 0), arg(a, 1), true, groups);
            Object* out = rt.allocate<Object>(nullptr);
            for (auto& [k, v] : groups) rt.put(out, rt.toPropertyKey(k), Value::object(rt.newArray(std::move(v))), kDefaultFlags);
            return Value::object(out);
        });
        const Value map_ctor = get(Value::object(global_), u"Map");
        method(map_ctor.asObject(), u"groupBy", 2, [groupBy](Runtime& rt, const Value&, Args a) {
            std::vector<std::pair<Value, std::vector<Value>>> groups;
            groupBy(rt, arg(a, 0), arg(a, 1), false, groups);
            const Value map = rt.construct(rt.get(Value::object(rt.global()), u"Map"), {});
            const Value set = rt.get(map, u"set");
            for (auto& [k, v] : groups) {
                const Value args[2] = {k, Value::object(rt.newArray(std::move(v)))};
                rt.call(set, map, args);
            }
            return map;
        });
    }
    // __defineGetter__ and the other legacy accessor helpers on Object.prototype.
    for (int setter = 0; setter < 2; ++setter) {
        method(object_prototype, setter ? u"__defineSetter__" : u"__defineGetter__", 2, [setter](Runtime& rt, const Value& t, Args a) {
            Object* o = rt.toObject(t);
            if (!rt.isCallable(arg(a, 1))) rt.throwError("TypeError", "the accessor must be a function");
            Object* d = rt.newObject();
            rt.put(d, setter ? u"set" : u"get", arg(a, 1), kDefaultFlags);
            rt.put(d, u"enumerable", Value::boolean(true), kDefaultFlags);
            rt.put(d, u"configurable", Value::boolean(true), kDefaultFlags);
            rt.defineOwnProperty(o, rt.toPropertyKey(arg(a, 0)), Value::object(d), true);
            return Value::undefined();
        });
        method(object_prototype, setter ? u"__lookupSetter__" : u"__lookupGetter__", 1, [setter](Runtime& rt, const Value& t, Args a) {
            Object* o = rt.toObject(t);
            const PropertyKey key = rt.toPropertyKey(arg(a, 0));
            for (Object* cur = o; cur; cur = rt.getPrototypeOf(cur)) {
                Property p;
                if (!rt.getOwnProperty(cur, key, p)) continue;
                if (!(p.flags & kAccessor)) return Value::undefined();
                Object* f = setter ? p.setter : p.getter;
                return f ? Value::object(f) : Value::undefined();
            }
            return Value::undefined();
        });
    }

    method(reflect, u"setPrototypeOf", 2, [target](Runtime& rt, const Value&, Args a) {
        Object* o = target(rt, arg(a, 0), "setPrototypeOf");
        if (!arg(a, 1).isObject() && !arg(a, 1).isNull()) rt.throwError("TypeError", "a prototype must be an object or null");
        return Value::boolean(rt.setPrototypeOf(o, arg(a, 1).isObject() ? arg(a, 1).asObject() : nullptr));
    });
}

}  // namespace fbx::xweb::js
