// RegExp objects, and the String methods that hand their work to them (match,
// matchAll, replace, replaceAll, search, split), as the specification has them
// (22.2.4 to 22.2.9, 22.1.3): through Symbol.match and friends, so a page's own
// matcher objects work too.

#include <algorithm>
#include <cmath>
#include <optional>

#include "internal.h"
#include "regexp.h"

namespace fbx::xweb::js {

namespace {

using Args = std::span<const Value>;

Value arg(Args a, size_t i) { return i < a.size() ? a[i] : Value::undefined(); }

Object* asRegExp(const Value& v) {
    if (v.isObject() && v.asObject()->cls == ObjectClass::RegExp && v.asObject()->regexp) return v.asObject();
    return nullptr;
}

Object* thisRegExp(Runtime& rt, const Value& t, const char* what) {
    Object* r = asRegExp(t);
    if (!r) rt.throwError("TypeError", std::string("RegExp.prototype.") + what + " called on something that isn't a RegExp");
    return r;
}

Object* thisObject(Runtime& rt, const Value& t, const char* what) {
    if (!t.isObject()) rt.throwError("TypeError", std::string("RegExp.prototype.") + what + " called on something that isn't an object");
    return t.asObject();
}

double toLength(Runtime& rt, const Value& v) {
    const double d = rt.toIntegerOrInfinity(v);
    return std::clamp(d, 0.0, 9007199254740991.0);
}

size_t advance(const std::u16string& s, size_t i, bool unicode) {
    if (!unicode || i + 1 >= s.size()) return i + 1;
    return s[i] >= 0xD800 && s[i] <= 0xDBFF && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF ? i + 2 : i + 1;
}

void setLastIndex(Runtime& rt, Object* r, double v) { rt.set(Value::object(r), u"lastIndex", Value::number(v), true); }

// The source as /…/ shows it: slashes and line breaks escaped, never empty.
std::u16string escapeSource(const std::u16string& src) {
    if (src.empty()) return u"(?:)";
    std::u16string out;
    bool in_class = false;
    for (size_t i = 0; i < src.size(); ++i) {
        const char16_t c = src[i];
        if (c == u'\\' && i + 1 < src.size()) {
            out += c;
            const char16_t n = src[++i];
            if (n == u'\n') out += u'n';
            else if (n == u'\r') out += u'r';
            else out += n;
            continue;
        }
        if (c == u'[') in_class = true;
        else if (c == u']') in_class = false;
        if (c == u'/' && !in_class) out += u"\\/";
        else if (c == u'\n') out += u"\\n";
        else if (c == u'\r') out += u"\\r";
        else if (c == 0x2028) out += u"\\u2028";
        else if (c == 0x2029) out += u"\\u2029";
        else out += c;
    }
    return out;
}

// RegExpInitialize: the pattern compiled (once per pattern and flags), lastIndex to 0.
void initialize(Runtime& rt, Object* r, const std::u16string& pattern, const std::u16string& flags_text) {
    const auto flags = regex::parseFlags(flags_text);
    if (!flags) rt.throwError("SyntaxError", "Invalid regular expression flags '" + utf8(flags_text) + "'");
    const std::u16string key = regex::flagsString(*flags) + u'/' + pattern;
    std::shared_ptr<const regex::Program> program;
    if (auto it = rt.regexp_cache.find(key); it != rt.regexp_cache.end()) program = it->second;
    if (!program) {
        std::string error;
        program = regex::compile(pattern, *flags, error);
        if (!program) rt.throwError("SyntaxError", "Invalid regular expression: /" + utf8(pattern) + "/" + utf8(flags_text) + ": " + error);
        if (rt.regexp_cache.size() >= 4096) rt.regexp_cache.clear();
        rt.regexp_cache.emplace(key, program);
    }
    r->regexp = std::move(program);
    r->primitive = Value::string(pattern);
    setLastIndex(rt, r, 0);
}

Object* allocRegExp(Runtime& rt, Object* proto) {
    Object* r = rt.allocate<Object>(proto, ObjectClass::RegExp);
    r->define(u"lastIndex", {Value::number(0), nullptr, nullptr, kWritable});
    return r;
}

// RegExpBuiltinExec: one match from lastIndex (or the start), as the array exec gives.
Value builtinExec(Runtime& rt, Object* r, const JSString& str) {
    const std::shared_ptr<const regex::Program> program = r->regexp;   // held: a callback can't free it mid-match
    const regex::Flags& f = regex::flagsOf(*program);
    const std::u16string& s = *str;
    double last = toLength(rt, rt.get(Value::object(r), u"lastIndex"));
    const bool keeps = f.global || f.sticky;
    if (!keeps) last = 0;
    std::vector<int> caps;
    if (last > static_cast<double>(s.size()) || !regex::exec(rt, *program, s, static_cast<size_t>(last), f.sticky, caps)) {
        if (keeps) setLastIndex(rt, r, 0);
        return Value::null();
    }
    if (keeps) setLastIndex(rt, r, caps[1]);
    const size_t groups = regex::groupCount(*program);
    std::vector<Value> items;
    items.reserve(groups + 1);
    for (size_t g = 0; g <= groups; ++g) {
        const int a = caps[2 * g], b = caps[2 * g + 1];
        items.push_back(a < 0 || b < 0 ? Value::undefined() : Value::string(s.substr(static_cast<size_t>(a), static_cast<size_t>(b - a))));
    }
    const auto& names = regex::groupNames(*program);
    Value groups_value = Value::undefined();
    if (!names.empty()) {
        Object* g = rt.newObject(nullptr);
        g->prototype = nullptr;
        for (const auto& [name, index] : names) g->define(name, {items[static_cast<size_t>(index)], nullptr, nullptr, kDefaultFlags});
        groups_value = Value::object(g);
    }
    Object* a = rt.newArray(std::move(items));
    a->define(u"index", {Value::number(caps[0]), nullptr, nullptr, kDefaultFlags});
    a->define(u"input", {Value::string(str), nullptr, nullptr, kDefaultFlags});
    a->define(u"groups", {groups_value, nullptr, nullptr, kDefaultFlags});
    if (f.has_indices) {
        std::vector<Value> pairs;
        for (size_t g = 0; g <= groups; ++g) {
            const int x = caps[2 * g], y = caps[2 * g + 1];
            pairs.push_back(x < 0 || y < 0 ? Value::undefined() : Value::object(rt.newArray({Value::number(x), Value::number(y)})));
        }
        Value index_groups = Value::undefined();
        if (!names.empty()) {
            Object* g = rt.newObject(nullptr);
            g->prototype = nullptr;
            for (const auto& [name, index] : names) g->define(name, {pairs[static_cast<size_t>(index)], nullptr, nullptr, kDefaultFlags});
            index_groups = Value::object(g);
        }
        Object* indices = rt.newArray(std::move(pairs));
        indices->define(u"groups", {index_groups, nullptr, nullptr, kDefaultFlags});
        a->define(u"indices", {Value::object(indices), nullptr, nullptr, kDefaultFlags});
    }
    rt.regexp_last_input = s;
    rt.regexp_last_caps = std::move(caps);
    return Value::object(a);
}

// RegExpExec: the object's own exec if it has one, else the built-in.
Value regExpExec(Runtime& rt, Object* r, const JSString& s) {
    const Value exec = rt.get(Value::object(r), u"exec");
    if (exec.isObject() && exec.asObject() == rt.regexp_exec && asRegExp(Value::object(r))) return builtinExec(rt, r, s);
    if (rt.isCallable(exec)) {
        const Value sv = Value::string(s);
        const Value result = rt.call(exec, Value::object(r), std::span<const Value>(&sv, 1));
        if (!result.isObject() && !result.isNull()) rt.throwError("TypeError", "exec must return an object or null");
        return result;
    }
    if (!asRegExp(Value::object(r))) rt.throwError("TypeError", "RegExp exec called on something that isn't a RegExp");
    return builtinExec(rt, r, s);
}

bool contains(const std::u16string& s, char16_t c) { return s.find(c) != std::u16string::npos; }

// GetSubstitution: $$, $&, $`, $', $n, $nn and $<name> in a replacement.
std::u16string substitute(Runtime& rt, const std::u16string& matched, const std::u16string& str, size_t position,
                          const std::vector<std::optional<std::u16string>>& captures, const Value& named, const std::u16string& tmpl) {
    std::u16string out;
    const size_t m = captures.size();
    for (size_t i = 0; i < tmpl.size(); ++i) {
        const char16_t c = tmpl[i];
        if (c != u'$' || i + 1 >= tmpl.size()) {
            out += c;
            continue;
        }
        const char16_t n = tmpl[i + 1];
        if (n == u'$') { out += u'$'; ++i; continue; }
        if (n == u'&') { out += matched; ++i; continue; }
        if (n == u'`') { out += str.substr(0, std::min(position, str.size())); ++i; continue; }
        if (n == u'\'') {
            const size_t tail = std::min(position + matched.size(), str.size());
            out += str.substr(tail);
            ++i;
            continue;
        }
        if (n >= u'0' && n <= u'9') {
            const size_t d1 = n - u'0';
            if (i + 2 < tmpl.size() && tmpl[i + 2] >= u'0' && tmpl[i + 2] <= u'9') {
                const size_t d2 = d1 * 10 + (tmpl[i + 2] - u'0');
                if (d2 >= 1 && d2 <= m) {
                    if (captures[d2 - 1]) out += *captures[d2 - 1];
                    i += 2;
                    continue;
                }
            }
            if (d1 >= 1 && d1 <= m) {
                if (captures[d1 - 1]) out += *captures[d1 - 1];
                ++i;
                continue;
            }
            out += c;
            continue;
        }
        if (n == u'<' && !named.isUndefined()) {
            const size_t close = tmpl.find(u'>', i + 2);
            if (close == std::u16string::npos) {
                out += c;
                continue;
            }
            const std::u16string name = tmpl.substr(i + 2, close - i - 2);
            const Value v = rt.get(named, PropertyKey(name));
            if (!v.isUndefined()) out += *rt.toString(v);
            i = close;
            continue;
        }
        out += c;
    }
    return out;
}

Value getMethod(Runtime& rt, const Value& v, Symbol* key) {
    const Value m = rt.get(v, PropertyKey(key));
    if (m.isNullish()) return Value::undefined();
    if (!rt.isCallable(m)) rt.throwError("TypeError", "a Symbol method must be a function");
    return m;
}

// SpeciesConstructor(o, %RegExp%).
Value speciesConstructor(Runtime& rt, Object* o, const Value& fallback) {
    const Value c = rt.get(Value::object(o), u"constructor");
    if (c.isUndefined()) return fallback;
    if (!c.isObject()) rt.throwError("TypeError", "a RegExp's constructor must be an object");
    const Value s = rt.get(c, PropertyKey(rt.sym_species));
    if (s.isNullish()) return fallback;
    if (!rt.isConstructor(s)) rt.throwError("TypeError", "Symbol.species must be a constructor");
    return s;
}

void requireCoercible(Runtime& rt, const Value& t, const char* what) {
    if (t.isNullish()) rt.throwError("TypeError", std::string("String.prototype.") + what + " called on null or undefined");
}

}  // namespace

bool Runtime::isRegExp(const Value& v) {
    if (!v.isObject()) return false;
    const Value m = get(v.asObject(), PropertyKey(sym_match), v);
    if (!m.isUndefined()) return toBoolean(m);
    return v.asObject()->cls == ObjectClass::RegExp;
}

Object* Runtime::newRegExp(const std::u16string& source, const std::u16string& flags) {
    Object* r = allocRegExp(*this, regexp_prototype);
    initialize(*this, r, source, flags);
    return r;
}

void Runtime::installRegExp() {
    Runtime& rt = *this;
    regexp_prototype = newObject();
    const auto getter = [&](Object* o, const PropertyKey& key, NativeFunction fn) {
        o->define(key, {Value::undefined(), newFunction(u"get " + key.name, 0, std::move(fn)), nullptr, kConfigurable | kAccessor});
    };

    // ── The constructor ──
    Object* ctor = newFunction(u"RegExp", 2, [](Runtime& rt, const Value&, Args a) -> Value {
        const Value pattern = arg(a, 0), flags = arg(a, 1);
        Object* new_target = rt.construct_target;
        const bool pattern_is_regexp = rt.isRegExp(pattern);
        if (!new_target && pattern_is_regexp && flags.isUndefined()) {
            const Value c = rt.get(pattern, u"constructor");
            if (c.isObject() && c.asObject() == rt.regexp_constructor) return pattern;
        }
        Value p, f;
        if (Object* r = asRegExp(pattern)) {
            p = r->primitive;
            f = flags.isUndefined() ? Value::string(regex::flagsString(regex::flagsOf(*r->regexp))) : flags;
        } else if (pattern_is_regexp) {
            p = rt.get(pattern, u"source");
            f = flags.isUndefined() ? rt.get(pattern, u"flags") : flags;
        } else {
            p = pattern;
            f = flags;
        }
        const std::u16string ps = p.isUndefined() ? u"" : *rt.toString(p);
        const std::u16string fs = f.isUndefined() ? u"" : *rt.toString(f);
        Object* proto = rt.regexp_prototype;
        if (new_target) {
            const Value pv = rt.get(Value::object(new_target), u"prototype");
            if (pv.isObject()) proto = pv.asObject();
        }
        Object* r = allocRegExp(rt, proto);
        initialize(rt, r, ps, fs);
        return Value::object(r);
    }, true);
    regexp_constructor = ctor;
    getter(ctor, PropertyKey(sym_species), [](Runtime&, const Value& t, Args) { return t; });
    ctor->own(PropertyKey(sym_species))->getter->define(u"name", {Value::string(u"get [Symbol.species]"), nullptr, nullptr, kConfigurable});
    put(ctor, u"prototype", Value::object(regexp_prototype), 0);
    put(regexp_prototype, u"constructor", Value::object(ctor));
    put(global_, u"RegExp", Value::object(ctor));

    // The legacy statics: RegExp.$1 and the rest, from the last match anywhere.
    const auto lastCapture = [](Runtime& rt, size_t g) -> Value {
        const auto& c = rt.regexp_last_caps;
        if (2 * g + 1 >= c.size() || c[2 * g] < 0) return Value::string(u"");
        return Value::string(rt.regexp_last_input.substr(static_cast<size_t>(c[2 * g]), static_cast<size_t>(c[2 * g + 1] - c[2 * g])));
    };
    for (size_t g = 1; g <= 9; ++g)
        getter(ctor, std::u16string(u"$") + static_cast<char16_t>(u'0' + g), [g, lastCapture](Runtime& rt, const Value&, Args) { return lastCapture(rt, g); });
    for (const char16_t* name : {u"input", u"$_"})
        getter(ctor, name, [](Runtime& rt, const Value&, Args) { return Value::string(rt.regexp_last_input); });
    for (const char16_t* name : {u"lastMatch", u"$&"})
        getter(ctor, name, [lastCapture](Runtime& rt, const Value&, Args) { return lastCapture(rt, 0); });
    for (const char16_t* name : {u"lastParen", u"$+"})
        getter(ctor, name, [lastCapture](Runtime& rt, const Value&, Args) {
            const size_t n = rt.regexp_last_caps.size() / 2;
            return n > 1 ? lastCapture(rt, n - 1) : Value::string(u"");
        });
    for (const char16_t* name : {u"leftContext", u"$`"})
        getter(ctor, name, [](Runtime& rt, const Value&, Args) {
            const auto& c = rt.regexp_last_caps;
            return Value::string(c.empty() ? u"" : rt.regexp_last_input.substr(0, static_cast<size_t>(c[0])));
        });
    for (const char16_t* name : {u"rightContext", u"$'"})
        getter(ctor, name, [](Runtime& rt, const Value&, Args) {
            const auto& c = rt.regexp_last_caps;
            return Value::string(c.empty() ? u"" : rt.regexp_last_input.substr(static_cast<size_t>(c[1])));
        });

    // ── The prototype's accessors ──
    struct FlagGetter { const char16_t* name; bool regex::Flags::*field; };
    static const FlagGetter kFlagGetters[] = {
        {u"hasIndices", &regex::Flags::has_indices}, {u"global", &regex::Flags::global}, {u"ignoreCase", &regex::Flags::ignore_case},
        {u"multiline", &regex::Flags::multiline}, {u"dotAll", &regex::Flags::dot_all}, {u"unicode", &regex::Flags::unicode},
        {u"unicodeSets", &regex::Flags::unicode_sets}, {u"sticky", &regex::Flags::sticky},
    };
    for (const FlagGetter& fg : kFlagGetters) {
        const auto field = fg.field;
        getter(regexp_prototype, fg.name, [field](Runtime& rt, const Value& t, Args) -> Value {
            if (t.isObject() && t.asObject() == rt.regexp_prototype) return Value::undefined();
            return Value::boolean(regex::flagsOf(*thisRegExp(rt, t, "flag getter")->regexp).*field);
        });
    }
    getter(regexp_prototype, u"flags", [](Runtime& rt, const Value& t, Args) {
        thisObject(rt, t, "flags");
        std::u16string out;
        static const std::pair<const char16_t*, char16_t> kOrder[] = {
            {u"hasIndices", u'd'}, {u"global", u'g'}, {u"ignoreCase", u'i'}, {u"multiline", u'm'},
            {u"dotAll", u's'}, {u"unicode", u'u'}, {u"unicodeSets", u'v'}, {u"sticky", u'y'},
        };
        for (const auto& [name, letter] : kOrder)
            if (rt.toBoolean(rt.get(t, name))) out += letter;
        return Value::string(std::move(out));
    });
    getter(regexp_prototype, u"source", [](Runtime& rt, const Value& t, Args) {
        if (t.isObject() && t.asObject() == rt.regexp_prototype) return Value::string(u"(?:)");
        return Value::string(escapeSource(thisRegExp(rt, t, "source")->primitive.asString()));
    });

    // ── The prototype's methods ──
    method(regexp_prototype, u"exec", 1, [](Runtime& rt, const Value& t, Args a) {
        Object* r = thisRegExp(rt, t, "exec");
        return builtinExec(rt, r, rt.toString(arg(a, 0)));
    });
    regexp_exec = get(regexp_prototype, u"exec", Value::object(regexp_prototype)).asObject();
    method(regexp_prototype, u"test", 1, [](Runtime& rt, const Value& t, Args a) {
        Object* r = thisObject(rt, t, "test");
        return Value::boolean(!regExpExec(rt, r, rt.toString(arg(a, 0))).isNull());
    });
    method(regexp_prototype, u"toString", 0, [](Runtime& rt, const Value& t, Args) {
        thisObject(rt, t, "toString");
        return Value::string(u"/" + *rt.toString(rt.get(t, u"source")) + u"/" + *rt.toString(rt.get(t, u"flags")));
    });
    method(regexp_prototype, u"compile", 2, [](Runtime& rt, const Value& t, Args a) {
        Object* r = thisRegExp(rt, t, "compile");
        std::u16string p, f;
        if (Object* src = asRegExp(arg(a, 0))) {
            if (!arg(a, 1).isUndefined()) rt.throwError("TypeError", "flags can't be given with a RegExp");
            p = src->primitive.asString();
            f = regex::flagsString(regex::flagsOf(*src->regexp));
        } else {
            p = arg(a, 0).isUndefined() ? u"" : *rt.toString(arg(a, 0));
            f = arg(a, 1).isUndefined() ? u"" : *rt.toString(arg(a, 1));
        }
        initialize(rt, r, p, f);
        return t;
    });

    put(regexp_prototype, PropertyKey(sym_match), Value::object(newFunction(u"[Symbol.match]", 1, [](Runtime& rt, const Value& t, Args a) -> Value {
        Object* r = thisObject(rt, t, "[Symbol.match]");
        const JSString s = rt.toString(arg(a, 0));
        const std::u16string flags = *rt.toString(rt.get(t, u"flags"));
        if (!contains(flags, u'g')) return regExpExec(rt, r, s);
        const bool unicode = contains(flags, u'u') || contains(flags, u'v');
        setLastIndex(rt, r, 0);
        std::vector<Value> found;
        while (true) {
            const Value result = regExpExec(rt, r, s);
            if (result.isNull()) return found.empty() ? Value::null() : Value::object(rt.newArray(std::move(found)));
            const JSString m = rt.toString(rt.get(result, u"0"));
            if (m->empty()) {
                const double i = toLength(rt, rt.get(t, u"lastIndex"));
                setLastIndex(rt, r, static_cast<double>(advance(*s, static_cast<size_t>(i), unicode)));
            }
            found.push_back(Value::string(m));
            rt.step();
        }
    })));

    put(regexp_prototype, PropertyKey(sym_search), Value::object(newFunction(u"[Symbol.search]", 1, [](Runtime& rt, const Value& t, Args a) -> Value {
        Object* r = thisObject(rt, t, "[Symbol.search]");
        const JSString s = rt.toString(arg(a, 0));
        const Value previous = rt.get(t, u"lastIndex");
        if (!Runtime::sameValue(previous, Value::number(0))) setLastIndex(rt, r, 0);
        const Value result = regExpExec(rt, r, s);
        if (!Runtime::sameValue(rt.get(t, u"lastIndex"), previous)) rt.set(t, u"lastIndex", previous, true);
        return result.isNull() ? Value::number(-1) : rt.get(result, u"index");
    })));

    put(regexp_prototype, PropertyKey(sym_replace), Value::object(newFunction(u"[Symbol.replace]", 2, [](Runtime& rt, const Value& t, Args a) -> Value {
        Object* r = thisObject(rt, t, "[Symbol.replace]");
        const JSString str = rt.toString(arg(a, 0));
        const std::u16string& s = *str;
        Value with = arg(a, 1);
        const bool functional = rt.isCallable(with);
        std::u16string tmpl;
        if (!functional) tmpl = *rt.toString(with);
        const std::u16string flags = *rt.toString(rt.get(t, u"flags"));
        const bool global = contains(flags, u'g');
        const bool unicode = contains(flags, u'u') || contains(flags, u'v');
        if (global) setLastIndex(rt, r, 0);
        std::vector<Value> results;
        while (true) {
            const Value result = regExpExec(rt, r, str);
            if (result.isNull()) break;
            results.push_back(result);   // no collection runs mid-script: held as they are
            if (!global) break;
            const JSString m = rt.toString(rt.get(result, u"0"));
            if (m->empty()) {
                const double i = toLength(rt, rt.get(t, u"lastIndex"));
                setLastIndex(rt, r, static_cast<double>(advance(s, static_cast<size_t>(i), unicode)));
            }
            rt.step();
        }
        std::u16string out;
        size_t next = 0;
        for (const Value& result : results) {
            Object* ro = rt.toObject(result);
            const uint64_t n = rt.lengthOf(ro);
            const size_t ncaps = n > 0 ? static_cast<size_t>(std::min<uint64_t>(n - 1, 1u << 20)) : 0;
            const std::u16string matched = *rt.toString(rt.get(result, u"0"));
            const double pos = std::clamp(rt.toIntegerOrInfinity(rt.get(result, u"index")), 0.0, static_cast<double>(s.size()));
            const size_t position = static_cast<size_t>(pos);
            std::vector<std::optional<std::u16string>> captures;
            std::vector<Value> capture_values;
            for (size_t k = 1; k <= ncaps; ++k) {
                Value c = rt.get(result, PropertyKey(Runtime::numberToString(static_cast<double>(k))));
                if (!c.isUndefined()) c = Value::string(rt.toString(c));
                capture_values.push_back(c);
                captures.push_back(c.isUndefined() ? std::nullopt : std::optional<std::u16string>(c.asString()));
            }
            Value named = rt.get(result, u"groups");
            std::u16string replacement;
            if (functional) {
                std::vector<Value> args;
                args.push_back(Value::string(matched));
                args.insert(args.end(), capture_values.begin(), capture_values.end());
                args.push_back(Value::number(static_cast<double>(position)));
                args.push_back(Value::string(str));
                if (!named.isUndefined()) args.push_back(named);
                replacement = *rt.toString(rt.call(with, Value::undefined(), args));
            } else {
                if (!named.isUndefined()) named = Value::object(rt.toObject(named));
                replacement = substitute(rt, matched, s, position, captures, named, tmpl);
            }
            if (position >= next) {
                out.append(s, next, position - next);
                out += replacement;
                next = position + matched.size();
            }
            if (out.size() > (1u << 29)) rt.throwError("RangeError", "string too long");
        }
        if (next < s.size()) out.append(s, next, std::u16string::npos);
        return Value::string(std::move(out));
    })));

    put(regexp_prototype, PropertyKey(sym_split), Value::object(newFunction(u"[Symbol.split]", 2, [](Runtime& rt, const Value& t, Args a) -> Value {
        Object* rx = thisObject(rt, t, "[Symbol.split]");
        const JSString str = rt.toString(arg(a, 0));
        const std::u16string& s = *str;
        const Value c = speciesConstructor(rt, rx, Value::object(rt.regexp_constructor));
        const std::u16string flags = *rt.toString(rt.get(t, u"flags"));
        const bool unicode = contains(flags, u'u') || contains(flags, u'v');
        const Value args[2] = {t, Value::string(contains(flags, u'y') ? flags : flags + u"y")};
        const Value made = rt.construct(c, args);
        if (!made.isObject()) rt.throwError("TypeError", "the splitter must be an object");
        Object* splitter = made.asObject();
        const uint32_t limit = arg(a, 1).isUndefined() ? 0xFFFFFFFFu : rt.toUint32(arg(a, 1));
        std::vector<Value> out;
        if (limit == 0) return Value::object(rt.newArray());
        const Value exec = rt.get(made, u"exec");
        if (exec.isObject() && exec.asObject() == rt.regexp_exec && asRegExp(made)) {
            // The built-in exec, untouched: the matcher directly, leftmost match from q
            // being what the spec's sticky try at each q comes to.
            const std::shared_ptr<const regex::Program> program = splitter->regexp;
            std::vector<int> caps;
            if (s.empty()) {
                if (regex::exec(rt, *program, s, 0, true, caps)) return Value::object(rt.newArray());
                return Value::object(rt.newArray({Value::string(str)}));
            }
            size_t p = 0, q = 0;
            const size_t groups = regex::groupCount(*program);
            while (q < s.size()) {
                if (!regex::exec(rt, *program, s, q, false, caps)) break;
                const size_t at = static_cast<size_t>(caps[0]);
                if (at >= s.size()) break;
                const size_t e = std::min(static_cast<size_t>(caps[1]), s.size());
                if (e == p) {
                    q = advance(s, at, unicode);
                    continue;
                }
                out.push_back(Value::string(s.substr(p, at - p)));
                if (out.size() == limit) return Value::object(rt.newArray(std::move(out)));
                p = e;
                for (size_t g = 1; g <= groups; ++g) {
                    const int x = caps[2 * g], y = caps[2 * g + 1];
                    out.push_back(x < 0 || y < 0 ? Value::undefined() : Value::string(s.substr(static_cast<size_t>(x), static_cast<size_t>(y - x))));
                    if (out.size() == limit) return Value::object(rt.newArray(std::move(out)));
                }
                q = p;
                rt.step();
            }
            out.push_back(Value::string(s.substr(p)));
            return Value::object(rt.newArray(std::move(out)));
        }
        // A page's own exec: the spec's loop, a sticky try at each position.
        if (s.empty()) {
            if (!regExpExec(rt, splitter, str).isNull()) return Value::object(rt.newArray());
            return Value::object(rt.newArray({Value::string(str)}));
        }
        size_t p = 0, q = 0;
        while (q < s.size()) {
            setLastIndex(rt, splitter, static_cast<double>(q));
            const Value z = regExpExec(rt, splitter, str);
            if (z.isNull()) {
                q = advance(s, q, unicode);
                continue;
            }
            const size_t e = static_cast<size_t>(std::min(toLength(rt, rt.get(made, u"lastIndex")), static_cast<double>(s.size())));
            if (e == p) {
                q = advance(s, q, unicode);
                continue;
            }
            out.push_back(Value::string(s.substr(p, q - p)));
            if (out.size() == limit) return Value::object(rt.newArray(std::move(out)));
            p = e;
            const double n = std::max(toLength(rt, rt.get(z, u"length")) - 1, 0.0);
            for (double i = 1; i <= n; ++i) {
                out.push_back(rt.get(z, PropertyKey(Runtime::numberToString(i))));
                if (out.size() == limit) return Value::object(rt.newArray(std::move(out)));
            }
            q = p;
            rt.step();
        }
        out.push_back(Value::string(s.substr(std::min(p, s.size()))));
        return Value::object(rt.newArray(std::move(out)));
    })));

    // matchAll's iterator.
    regexp_string_iterator_prototype = newObject(iterator_prototype);
    put(regexp_string_iterator_prototype, PropertyKey(sym_to_string_tag), Value::string(u"RegExp String Iterator"), kConfigurable);
    method(regexp_string_iterator_prototype, u"next", 0, [](Runtime& rt, const Value& t, Args) {
        if (!t.isObject() || t.asObject()->cls != ObjectClass::Iterator || !t.asObject()->target || !t.asObject()->primitive.isString())
            rt.throwError("TypeError", "not a RegExp String Iterator");
        Object* it = t.asObject();
        if (it->cursor) return Value::object(rt.createIterResult(Value::undefined(), true));
        const JSString s = it->primitive.stringRef();
        const Value match = regExpExec(rt, it->target, s);
        if (match.isNull()) {
            it->cursor = 1;
            return Value::object(rt.createIterResult(Value::undefined(), true));
        }
        if (it->iter_kind & 1) {
            if (rt.toString(rt.get(match, u"0"))->empty()) {
                const double i = toLength(rt, rt.get(Value::object(it->target), u"lastIndex"));
                setLastIndex(rt, it->target, static_cast<double>(advance(*s, static_cast<size_t>(i), it->iter_kind & 2)));
            }
        } else {
            it->cursor = 1;
        }
        return Value::object(rt.createIterResult(match, false));
    });
    put(regexp_prototype, PropertyKey(sym_match_all), Value::object(newFunction(u"[Symbol.matchAll]", 1, [](Runtime& rt, const Value& t, Args a) -> Value {
        Object* r = thisObject(rt, t, "[Symbol.matchAll]");
        const JSString s = rt.toString(arg(a, 0));
        const Value c = speciesConstructor(rt, r, Value::object(rt.regexp_constructor));
        const std::u16string flags = *rt.toString(rt.get(t, u"flags"));
        const Value args[2] = {t, Value::string(flags)};
        const Value made = rt.construct(c, args);
        if (!made.isObject()) rt.throwError("TypeError", "the matcher must be an object");
        setLastIndex(rt, made.asObject(), toLength(rt, rt.get(t, u"lastIndex")));
        Object* it = rt.allocate<Object>(rt.regexp_string_iterator_prototype, ObjectClass::Iterator);
        it->target = made.asObject();
        it->primitive = Value::string(s);
        it->iter_kind = static_cast<uint8_t>((contains(flags, u'g') ? 1 : 0) | (contains(flags, u'u') || contains(flags, u'v') ? 2 : 0));
        return Value::object(it);
    })));

    // ── String's methods that take a RegExp ──
    // Each hands an object's own Symbol method the work; a string or nothing becomes
    // a RegExp (match, matchAll, search) or keeps the plain-string path (replace, split).
    const auto viaSymbol = [](Runtime& rt, const Value& t, const Value& target, Symbol* sym, std::span<const Value> rest,
                              Value& out) -> bool {
        if (!target.isObject()) return false;   // a primitive's methods aren't asked (ES2023)
        const Value m = getMethod(rt, target, sym);
        if (m.isUndefined()) return false;
        std::vector<Value> args{t};
        args.insert(args.end(), rest.begin(), rest.end());
        out = rt.call(m, target, args);
        return true;
    };
    const auto createAndInvoke = [](Runtime& rt, const Value& t, const Value& pattern, const char16_t* flags, Symbol* sym) {
        const JSString s = rt.toString(t);
        Object* rx = rt.newRegExp(pattern.isUndefined() ? u"" : *rt.toString(pattern), flags);
        const Value sv = Value::string(s);
        return rt.call(rt.get(Value::object(rx), PropertyKey(sym)), Value::object(rx), std::span<const Value>(&sv, 1));
    };
    method(string_prototype, u"match", 1, [viaSymbol, createAndInvoke](Runtime& rt, const Value& t, Args a) {
        requireCoercible(rt, t, "match");
        Value out;
        if (viaSymbol(rt, t, arg(a, 0), rt.sym_match, {}, out)) return out;
        return createAndInvoke(rt, t, arg(a, 0), u"", rt.sym_match);
    });
    method(string_prototype, u"matchAll", 1, [viaSymbol, createAndInvoke](Runtime& rt, const Value& t, Args a) {
        requireCoercible(rt, t, "matchAll");
        const Value re = arg(a, 0);
        if (!re.isNullish() && rt.isRegExp(re)) {
            const Value flags = rt.get(re, u"flags");
            requireCoercible(rt, flags, "matchAll");
            if (!contains(*rt.toString(flags), u'g')) rt.throwError("TypeError", "matchAll must be called with a global RegExp");
        }
        Value out;
        if (viaSymbol(rt, t, re, rt.sym_match_all, {}, out)) return out;
        return createAndInvoke(rt, t, re, u"g", rt.sym_match_all);
    });
    method(string_prototype, u"search", 1, [viaSymbol, createAndInvoke](Runtime& rt, const Value& t, Args a) {
        requireCoercible(rt, t, "search");
        Value out;
        if (viaSymbol(rt, t, arg(a, 0), rt.sym_search, {}, out)) return out;
        return createAndInvoke(rt, t, arg(a, 0), u"", rt.sym_search);
    });
    for (const char16_t* name : {u"replace", u"replaceAll"}) {
        // The string path is the old method's own code, kept (its object goes).
        const NativeFunction plain = get(string_prototype, name, Value::object(string_prototype)).asObject()->native;
        const bool all = std::u16string_view(name) == u"replaceAll";
        method(string_prototype, name, 2, [viaSymbol, plain, all](Runtime& rt, const Value& t, Args a) {
            requireCoercible(rt, t, all ? "replaceAll" : "replace");
            const Value search = arg(a, 0);
            if (all && !search.isNullish() && rt.isRegExp(search)) {
                const Value flags = rt.get(search, u"flags");
                requireCoercible(rt, flags, "replaceAll");
                if (!contains(*rt.toString(flags), u'g')) rt.throwError("TypeError", "replaceAll must be called with a global RegExp");
            }
            Value out;
            const Value rest[1] = {arg(a, 1)};
            if (viaSymbol(rt, t, search, rt.sym_replace, rest, out)) return out;
            return plain(rt, t, a);
        });
    }
    {
        const NativeFunction plain = get(string_prototype, u"split", Value::object(string_prototype)).asObject()->native;
        method(string_prototype, u"split", 2, [viaSymbol, plain](Runtime& rt, const Value& t, Args a) {
            requireCoercible(rt, t, "split");
            Value out;
            const Value rest[1] = {arg(a, 1)};
            if (viaSymbol(rt, t, arg(a, 0), rt.sym_split, rest, out)) return out;
            return plain(rt, t, a);
        });
    }
    (void)rt;
}

}  // namespace fbx::xweb::js
