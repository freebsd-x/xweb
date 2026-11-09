// ES modules (16.2): fetching a module graph through the embedder's loader, linking
// imports to exports as live bindings, and evaluating dependencies first. A module's
// body runs on a coroutine, so top-level await works.

#include <algorithm>
#include <map>
#include <new>
#include <set>
#include <unordered_map>

#include "coroutine.h"
#include "internal.h"

namespace fbx::xweb::js {

namespace {

using Args = std::span<const Value>;

enum class Status : uint8_t { Fetching, FetchFailed, Parsed, Linked, Evaluating, Evaluated, Errored };

struct GraphLoad;

struct ModuleRecord {
    std::string url;
    Status status = Status::Fetching;
    std::string error;                     // why fetching or parsing failed
    std::shared_ptr<Program> program;
    std::vector<std::string> requested;    // what it imports, resolved
    std::vector<std::shared_ptr<GraphLoad>> waiters;
    Env* env = nullptr;
    Object* ns = nullptr;
    Object* meta = nullptr;
    Object* holder = nullptr;              // owns the coroutine its body runs on
    Object* promise = nullptr;             // settles when it (and what it imports) has run
    Value failure;                         // what its evaluation threw
};

// One load of a graph: done when every module it reaches is fetched and parsed.
struct GraphLoad {
    std::string root;
    int pending = 0;
    bool finished = false;
    std::string error;
    std::set<ModuleRecord*> visited;
    std::function<void(const std::string& error)> done;
};

// The result of resolving an export by name.
struct Resolution {
    ModuleRecord* module = nullptr;
    std::u16string binding;
    bool is_namespace = false;
    bool ambiguous = false;
    explicit operator bool() const { return module != nullptr; }
};

std::u16string specifierOf(Node* n) { return n->str; }

}  // namespace

struct Runtime::Modules {
    std::unordered_map<std::string, std::unique_ptr<ModuleRecord>> records;
    std::vector<Object*> pending_imports;   // import() promises waiting on a load
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    size_t inline_count = 0;
};

void Runtime::ModulesDeleter::operator()(Modules* m) const {
    *m->alive = false;
    delete m;
}

Runtime::Modules& Runtime::modulesForLoader() {
    if (!modules_) modules_.reset(new Modules);
    return *modules_;
}

// ── URLs ──

namespace {

bool hasScheme(const std::string& s) {
    const size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    for (size_t i = 0; i < colon; ++i) {
        const char c = s[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.')) return false;
    }
    return std::isalpha(static_cast<unsigned char>(s[0])) != 0;
}

std::string removeDots(const std::string& path) {
    std::vector<std::string> out;
    size_t i = 0;
    const bool trailing = path.empty() || path.back() == '/' || (path.size() >= 2 && path.substr(path.size() - 2) == "/.") ||
                          (path.size() >= 3 && path.substr(path.size() - 3) == "/..");
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        const std::string seg = path.substr(i, j - i);
        if (seg == "..") {
            if (!out.empty()) out.pop_back();
        } else if (seg != "." && !seg.empty()) {
            out.push_back(seg);
        }
        i = j + 1;
    }
    std::string r;
    for (const auto& seg : out) r += "/" + seg;
    if (trailing || r.empty()) r += "/";
    return r;
}

}  // namespace

std::string Runtime::resolveSpecifier(const std::string& spec, const std::string& referrer) {
    if (spec.empty()) return {};
    if (hasScheme(spec)) return spec;
    const bool relative = spec.rfind("./", 0) == 0 || spec.rfind("../", 0) == 0 || spec[0] == '/';
    if (!relative) return {};   // a bare name ("lodash"): needs an import map
    if (!hasScheme(referrer)) return {};
    std::string base = referrer;
    if (const size_t h = base.find('#'); h != std::string::npos) base.resize(h);
    if (const size_t q = base.find('?'); q != std::string::npos) base.resize(q);
    const size_t scheme_end = base.find(':');
    std::string authority_part;   // "scheme://host"
    std::string path;
    if (base.compare(scheme_end, 3, "://") == 0) {
        const size_t host_start = scheme_end + 3;
        const size_t path_start = base.find('/', host_start);
        authority_part = base.substr(0, path_start == std::string::npos ? base.size() : path_start);
        path = path_start == std::string::npos ? "/" : base.substr(path_start);
    } else {
        authority_part = base.substr(0, scheme_end + 1);
        path = base.substr(scheme_end + 1);
    }
    if (spec.rfind("//", 0) == 0) return base.substr(0, scheme_end + 1) + spec;
    std::string query;
    std::string s = spec;
    if (const size_t h = s.find('#'); h != std::string::npos) s.resize(h);
    if (const size_t q = s.find('?'); q != std::string::npos) {
        query = s.substr(q);
        s.resize(q);
    }
    std::string joined;
    if (s[0] == '/') {
        joined = s;
    } else {
        const size_t slash = path.rfind('/');
        joined = (slash == std::string::npos ? std::string("/") : path.substr(0, slash + 1)) + s;
    }
    return authority_part + removeDots(joined) + query;
}

// ── Import maps ──

namespace {

// Just enough JSON for an import map: objects of strings, anything else skipped.
struct MiniJson {
    std::string_view s;
    size_t i = 0;
    bool ok = true;
    void space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool eat(char c) {
        space();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    bool string(std::string& out) {
        space();
        if (i >= s.size() || s[i] != '"') return ok = false;
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return ok = false;
            c = s[i++];
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (i + 4 > s.size()) return ok = false;
                    const unsigned cp = static_cast<unsigned>(std::stoul(std::string(s.substr(i, 4)), nullptr, 16));
                    i += 4;
                    out += utf8(std::u16string(1, static_cast<char16_t>(cp)));
                    break;
                }
                default: out += c;
            }
        }
        if (i >= s.size()) return ok = false;
        ++i;
        return true;
    }
    void skipValue(int depth = 0) {
        space();
        if (i >= s.size() || depth > 64) {
            ok = false;
            return;
        }
        if (s[i] == '"') {
            std::string dummy;
            string(dummy);
        } else if (s[i] == '{' || s[i] == '[') {
            const char close = s[i] == '{' ? '}' : ']';
            ++i;
            if (eat(close)) return;
            while (ok) {
                if (close == '}') {
                    std::string k;
                    if (!string(k) || !eat(':')) {
                        ok = false;
                        return;
                    }
                }
                skipValue(depth + 1);
                if (eat(close)) return;
                if (!eat(',')) ok = false;
            }
        } else {
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
        }
    }
    // An object of strings: each member handed over; other values skipped.
    template <typename F>
    bool object(F&& member) {
        if (!eat('{')) return ok = false;
        if (eat('}')) return true;
        while (ok) {
            std::string key;
            if (!string(key) || !eat(':')) return ok = false;
            member(key);
            if (eat('}')) return ok;
            if (!eat(',')) return ok = false;
        }
        return false;
    }
};

bool urlLike(const std::string& s) { return s.rfind("./", 0) == 0 || s.rfind("../", 0) == 0 || s.rfind("/", 0) == 0; }

std::string normalizeKey(const std::string& key, const std::string& base) {
    if (urlLike(key) || hasScheme(key)) {
        const std::string u = Runtime::resolveSpecifier(key, base);
        if (!u.empty()) return u;
    }
    return key;
}

}  // namespace

bool ImportMap::add(std::string_view json, const std::string& base) {
    MiniJson j{json};
    const auto entries = [&](Entries& into) {
        return j.object([&](const std::string& key) {
            j.space();
            if (j.i < j.s.size() && j.s[j.i] == '"') {
                std::string value;
                if (!j.string(value)) return;
                const std::string target = Runtime::resolveSpecifier(value, base);
                if (target.empty()) return;
                into.emplace_back(normalizeKey(key, base), target);
            } else {
                j.skipValue();
            }
        });
    };
    const bool good = j.object([&](const std::string& key) {
        if (key == "imports") {
            entries(imports_);
        } else if (key == "scopes") {
            j.object([&](const std::string& scope) {
                Entries e;
                entries(e);
                scopes_.emplace_back(normalizeKey(scope, base), std::move(e));
            });
        } else {
            j.skipValue();
        }
    });
    const auto longest = [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); };
    std::stable_sort(imports_.begin(), imports_.end(), longest);
    std::stable_sort(scopes_.begin(), scopes_.end(), longest);
    for (auto& [scope, e] : scopes_) std::stable_sort(e.begin(), e.end(), longest);
    return good && j.ok;
}

std::string ImportMap::resolve(const std::string& spec, const std::string& referrer) const {
    const std::string as_url = (urlLike(spec) || hasScheme(spec)) ? Runtime::resolveSpecifier(spec, referrer) : std::string{};
    const std::string key = as_url.empty() ? spec : as_url;
    const auto lookup = [&](const Entries& e, std::string& out) {
        for (const auto& [k, v] : e) {
            if (k == key) {
                out = v;
                return true;
            }
            // "lib/": a prefix, the rest appended.
            if (!k.empty() && k.back() == '/' && v.size() && v.back() == '/' && key.rfind(k, 0) == 0) {
                out = v + key.substr(k.size());
                return true;
            }
        }
        return false;
    };
    std::string out;
    for (const auto& [scope, e] : scopes_)
        if ((scope == referrer || (scope.back() == '/' && referrer.rfind(scope, 0) == 0)) && lookup(e, out)) return out;
    if (lookup(imports_, out)) return out;
    return as_url;
}

// ── Loading ──

namespace {

struct Loader {
    Runtime& rt;
    Runtime::Modules& m;

    ModuleRecord* find(const std::string& url) {
        const auto it = m.records.find(url);
        return it == m.records.end() ? nullptr : it->second.get();
    }

    // Parses a fetched source and works out what it imports.
    void parsed(ModuleRecord* rec, const std::string& source) {
        std::string error;
        std::shared_ptr<Program> prog(parse(utf16(source), error, true, true).release());
        if (!prog) {
            rec->status = Status::FetchFailed;
            rec->error = error + " (" + rec->url + ")";
            return;
        }
        prog->url = rec->url;
        rec->program = prog;
        for (Node* s : prog->root->kids) {
            if ((s->kind == N::ImportDecl || (s->kind == N::ExportDecl && !s->str.empty()))) {
                const std::string url = rt.import_map.resolve(utf8(specifierOf(s)), rec->url);
                if (url.empty()) {
                    rec->status = Status::FetchFailed;
                    rec->error = "can't resolve the module specifier '" + utf8(specifierOf(s)) + "' (" + rec->url + ")";
                    return;
                }
                if (std::find(rec->requested.begin(), rec->requested.end(), url) == rec->requested.end()) rec->requested.push_back(url);
            }
        }
        rec->status = Status::Parsed;
    }

    void finishIfDone(const std::shared_ptr<GraphLoad>& load) {
        if (load->pending > 0 || load->finished) return;
        load->finished = true;
        if (load->done) load->done(load->error);
    }

    void visitDeps(const std::shared_ptr<GraphLoad>& load, ModuleRecord* rec) {
        for (const std::string& url : rec->requested) visit(load, url);
    }

    void visit(const std::shared_ptr<GraphLoad>& load, const std::string& url) {
        ModuleRecord* rec = find(url);
        if (!rec) {
            auto owned = std::make_unique<ModuleRecord>();
            owned->url = url;
            rec = owned.get();
            m.records[url] = std::move(owned);
            rec->waiters.push_back(load);
            ++load->pending;
            fetch(rec);
            return;
        }
        if (!load->visited.insert(rec).second) return;
        if (rec->status == Status::Fetching) {
            rec->waiters.push_back(load);
            ++load->pending;
            return;
        }
        if (rec->status == Status::FetchFailed) {
            if (load->error.empty()) load->error = rec->error;
            return;
        }
        visitDeps(load, rec);
    }

    void fetch(ModuleRecord* rec) {
        if (!rt.moduleLoader()) {
            fetched(rec->url, ModuleSource{false, {}, "no module loader"});
            return;
        }
        std::weak_ptr<bool> alive = m.alive;
        Runtime* rtp = &rt;
        const std::string url = rec->url;
        rt.moduleLoader()(url, [alive, rtp, url](ModuleSource src) {
            if (alive.expired()) return;
            Loader{*rtp, rtp->modulesForLoader()}.fetched(url, std::move(src));
        });
    }

    void fetched(const std::string& url, ModuleSource src) {
        ModuleRecord* rec = find(url);
        if (!rec || rec->status != Status::Fetching) return;
        if (!src.ok) {
            rec->status = Status::FetchFailed;
            rec->error = "couldn't load the module " + url + (src.error.empty() ? "" : ": " + src.error);
        } else {
            parsed(rec, src.source);
        }
        std::vector<std::shared_ptr<GraphLoad>> waiters;
        waiters.swap(rec->waiters);
        for (const auto& load : waiters) {
            // Its dependencies are counted before it stops counting, so a loader that
            // answers at once can't finish the load early.
            load->visited.insert(rec);
            if (rec->status == Status::FetchFailed) {
                if (load->error.empty()) load->error = rec->error;
            } else {
                visitDeps(load, rec);
            }
            --load->pending;
            finishIfDone(load);
        }
    }
};

// ── Linking ──

struct Linker {
    Runtime& rt;
    Runtime::Modules& m;

    ModuleRecord* find(const std::string& url) {
        const auto it = m.records.find(url);
        return it == m.records.end() ? nullptr : it->second.get();
    }

    ModuleRecord* target(ModuleRecord* from, const std::u16string& spec) {
        return find(rt.import_map.resolve(utf8(spec), from->url));
    }

    // The names a module's own declarations export, and what they're called inside it.
    static void localExports(ModuleRecord* rec, std::vector<std::pair<std::u16string, std::u16string>>& out) {
        for (Node* s : rec->program->root->kids) {
            if (s->kind != N::ExportDecl || !s->str.empty() || (s->flags & kNamespace)) continue;
            if (s->flags & kDefault) {
                if (s->a && (s->a->kind == N::FunctionDecl || s->a->kind == N::ClassDecl)) out.emplace_back(u"default", s->a->str);
                else out.emplace_back(u"default", u"*default*");
                continue;
            }
            if (s->a) {
                if (s->a->kind == N::FunctionDecl || s->a->kind == N::ClassDecl) {
                    out.emplace_back(s->a->str, s->a->str);
                } else if (s->a->kind == N::VarDecl) {
                    std::vector<std::u16string> names;
                    for (Node* d : s->a->kids) boundNames(d->a, names);
                    for (const auto& n : names) out.emplace_back(n, n);
                }
                continue;
            }
            for (Node* sp : s->kids) out.emplace_back(sp->str2, sp->str);
        }
    }

    static void boundNames(Node* p, std::vector<std::u16string>& out) {
        if (!p) return;
        switch (p->kind) {
            case N::Identifier: out.push_back(p->str); break;
            case N::AssignPattern: case N::Rest: boundNames(p->a, out); break;
            case N::ArrayPattern: for (Node* k : p->kids) boundNames(k, out); break;
            case N::ObjectPattern: for (Node* k : p->kids) boundNames(k->kind == N::PatternProperty ? k->b : k, out); break;
            default: break;
        }
    }

    Resolution resolveExport(ModuleRecord* rec, const std::u16string& name, std::set<std::pair<ModuleRecord*, std::u16string>>& seen) {
        if (!rec || !rec->program) return {};
        if (!seen.insert({rec, name}).second) return {};   // a cycle: nothing here
        std::vector<std::pair<std::u16string, std::u16string>> locals;
        localExports(rec, locals);
        for (const auto& [exported, local] : locals) {
            if (exported != name) continue;
            // `import {a} from "m"; export {a}` is an indirect export of m's a.
            for (Node* s : rec->program->root->kids) {
                if (s->kind != N::ImportDecl) continue;
                for (Node* sp : s->kids) {
                    if (sp->str2 != local) continue;
                    ModuleRecord* t = target(rec, s->str);
                    if (sp->str == u"*") return Resolution{t, u"", true, false};
                    return resolveExport(t, sp->str, seen);
                }
            }
            return Resolution{rec, local, false, false};
        }
        for (Node* s : rec->program->root->kids) {
            if (s->kind != N::ExportDecl || s->str.empty()) continue;
            if (s->flags & kNamespace) {
                if (!s->str2.empty() && s->str2 == name) return Resolution{target(rec, s->str), u"", true, false};
                continue;
            }
            for (Node* sp : s->kids) {
                if (sp->str2 != name) continue;
                ModuleRecord* t = target(rec, s->str);
                if (sp->str == u"*") return Resolution{t, u"", true, false};
                return resolveExport(t, sp->str, seen);
            }
        }
        if (name == u"default") return {};
        Resolution star;
        for (Node* s : rec->program->root->kids) {
            if (s->kind != N::ExportDecl || s->str.empty() || !(s->flags & kNamespace) || !s->str2.empty()) continue;
            const Resolution r = resolveExport(target(rec, s->str), name, seen);
            if (r.ambiguous) return r;
            if (!r) continue;
            if (!star) star = r;
            else if (star.module != r.module || star.binding != r.binding || star.is_namespace != r.is_namespace) return Resolution{star.module, u"", false, true};
        }
        return star;
    }

    void exportedNames(ModuleRecord* rec, std::set<ModuleRecord*>& seen, std::vector<std::u16string>& out) {
        if (!rec || !rec->program || !seen.insert(rec).second) return;
        std::vector<std::pair<std::u16string, std::u16string>> locals;
        localExports(rec, locals);
        for (const auto& [exported, local] : locals) out.push_back(exported);
        for (Node* s : rec->program->root->kids) {
            if (s->kind != N::ExportDecl || s->str.empty()) continue;
            if (s->flags & kNamespace) {
                if (!s->str2.empty()) {
                    out.push_back(s->str2);
                } else {
                    std::vector<std::u16string> star;
                    exportedNames(target(rec, s->str), seen, star);
                    for (const auto& n : star)
                        if (n != u"default") out.push_back(n);
                }
                continue;
            }
            for (Node* sp : s->kids) out.push_back(sp->str2);
        }
    }

    Object* namespaceOf(ModuleRecord* rec) {
        if (rec->ns) return rec->ns;
        Object* ns = rt.allocate<Object>(nullptr, ObjectClass::Internal);
        rec->ns = ns;
        std::set<ModuleRecord*> seen;
        std::vector<std::u16string> names;
        exportedNames(rec, seen, names);
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        for (const auto& name : names) {
            std::set<std::pair<ModuleRecord*, std::u16string>> s;
            const Resolution r = resolveExport(rec, name, s);
            if (!r || r.ambiguous) continue;
            if (r.is_namespace) {
                ns->define(name, {Value::object(namespaceOf(r.module)), nullptr, nullptr, kWritable | kEnumerable});
                continue;
            }
            // A live view of the binding.
            Object* getter = rt.newFunction(u"", 0, nullptr);
            getter->host = r.module;
            getter->slots = {Value::string(r.binding)};
            getter->native = [getter](Runtime& rt, const Value&, Args) -> Value {
                auto* mod = static_cast<ModuleRecord*>(getter->host);
                const std::u16string& b = getter->slots[0].asString();
                Env* env = mod->env;
                for (int hops = 0; env && hops < 100; ++hops) {
                    const VarMap::Entry* it = env->vars.find(b);
                    if (!it) break;
                    const Binding& bind = it->second;
                    if (bind.import_env) {
                        // Follow the chain by hand: imports of imports.
                        const VarMap::Entry* jt = bind.import_env->vars.find(bind.import_name);
                        if (!jt) break;
                        if (!jt->second.initialized) rt.throwError("ReferenceError", "'" + utf8(b) + "' isn't initialized yet");
                        if (!jt->second.import_env) return jt->second.value;
                        env = jt->second.import_env;
                        continue;
                    }
                    if (!bind.initialized) rt.throwError("ReferenceError", "'" + utf8(b) + "' isn't initialized yet");
                    return bind.value;
                }
                rt.throwError("ReferenceError", "'" + utf8(b) + "' isn't there");
            };
            ns->define(name, {Value::undefined(), getter, nullptr, kEnumerable | kAccessor});
        }
        ns->define(PropertyKey(rt.sym_to_string_tag), {Value::string(u"Module"), nullptr, nullptr, 0});
        ns->extensible = false;
        return ns;
    }

    // Scopes for every module reached, then their imports bound to the exports.
    std::string link(ModuleRecord* root) {
        std::vector<ModuleRecord*> order;
        std::set<ModuleRecord*> seen;
        std::vector<ModuleRecord*> todo{root};
        while (!todo.empty()) {
            ModuleRecord* r = todo.back();
            todo.pop_back();
            if (!r || !seen.insert(r).second) continue;
            if (r->status == Status::FetchFailed) return r->error;
            order.push_back(r);
            for (const auto& u : r->requested) todo.push_back(find(u));
        }
        for (ModuleRecord* r : order) {
            if (r->status != Status::Parsed) continue;
            r->env = rt.allocate<Env>(rt.globalEnv());
            r->env->is_function = true;   // its `this` is undefined
            r->env->this_value = Value::undefined();
            instantiateModuleScope(rt, r->program, r->env);
        }
        for (ModuleRecord* r : order) {
            if (r->status != Status::Parsed) continue;
            for (Node* s : r->program->root->kids) {
                if (s->kind != N::ImportDecl) continue;
                ModuleRecord* t = target(r, s->str);
                if (!t) return "a module it imports is missing (" + r->url + ")";
                for (Node* sp : s->kids) {
                    if (sp->str == u"*") {
                        r->env->vars[sp->str2] = Binding{Value::object(namespaceOf(t)), false, true};
                        continue;
                    }
                    std::set<std::pair<ModuleRecord*, std::u16string>> s2;
                    const Resolution res = resolveExport(t, sp->str, s2);
                    if (res.ambiguous) return "SyntaxError: the export '" + utf8(sp->str) + "' of " + t->url + " is ambiguous";
                    if (!res) return "SyntaxError: " + t->url + " doesn't export '" + utf8(sp->str) + "' (imported by " + r->url + ")";
                    if (res.is_namespace) {
                        r->env->vars[sp->str2] = Binding{Value::object(namespaceOf(res.module)), false, true};
                        continue;
                    }
                    Binding b;
                    b.is_mutable = false;
                    b.import_env = res.module->env;
                    b.import_name = res.binding;
                    r->env->vars[sp->str2] = b;
                }
            }
            // Re-exports must resolve too.
            for (Node* s : r->program->root->kids) {
                if (s->kind != N::ExportDecl || s->str.empty() || (s->flags & kNamespace)) continue;
                for (Node* sp : s->kids) {
                    if (sp->str == u"*") continue;
                    std::set<std::pair<ModuleRecord*, std::u16string>> s2;
                    const Resolution res = resolveExport(target(r, s->str), sp->str, s2);
                    if (!res || res.ambiguous) return "SyntaxError: '" + utf8(sp->str) + "' can't be re-exported from " + r->url;
                }
            }
            r->status = Status::Linked;
        }
        return {};
    }
};

// ── Evaluating ──

// Runs a graph's modules dependencies first, one after another; a module that awaits
// at its top level holds up the ones after it until it's done.
struct Sequence {
    std::vector<ModuleRecord*> order;
    size_t next = 0;
    Object* promise = nullptr;   // the whole graph's
};

void postOrder(Runtime::Modules& m, ModuleRecord* r, std::set<ModuleRecord*>& seen, std::vector<ModuleRecord*>& out) {
    if (!r || !seen.insert(r).second) return;
    for (const auto& u : r->requested) {
        const auto it = m.records.find(u);
        if (it != m.records.end()) postOrder(m, it->second.get(), seen, out);
    }
    out.push_back(r);
}

void step(Runtime& rt, Object* seq_holder);

Value continueSequence(Runtime& rt, Object* self, const Value&, Args a, bool ok) {
    Object* seq_holder = self->slots[0].asObject();
    auto* seq = static_cast<Sequence*>(seq_holder->host);
    if (!seq) return Value::undefined();
    if (!ok) {
        rt.rejectPromise(seq->promise, a.empty() ? Value::undefined() : a[0]);
        delete seq;
        seq_holder->host = nullptr;
        return Value::undefined();
    }
    step(rt, seq_holder);
    return Value::undefined();
}

Object* continuation(Runtime& rt, Object* seq_holder, bool ok) {
    Object* f = rt.newFunction(u"", 1, nullptr);
    f->slots = {Value::object(seq_holder)};
    if (ok) f->native = [f](Runtime& rt, const Value& t, Args a) { return continueSequence(rt, f, t, a, true); };
    else f->native = [f](Runtime& rt, const Value& t, Args a) { return continueSequence(rt, f, t, a, false); };
    return f;
}

void waitOn(Runtime& rt, Object* seq_holder, Object* promise) {
    Object* on_ok = continuation(rt, seq_holder, true);
    Object* on_err = continuation(rt, seq_holder, false);
    const Value then = rt.get(Value::object(promise), u"then");
    (void)then;
    // Directly, not through a page's replaced then.
    const Value fns[2] = {Value::object(on_ok), Value::object(on_err)};
    rt.call(Value::object(rt.promise_then), Value::object(promise), fns);
}

void step(Runtime& rt, Object* seq_holder) {
    auto* seq = static_cast<Sequence*>(seq_holder->host);
    while (seq->next < seq->order.size()) {
        ModuleRecord* r = seq->order[seq->next++];
        if (r->status == Status::Errored) {
            rt.rejectPromise(seq->promise, r->failure);
            delete seq;
            seq_holder->host = nullptr;
            return;
        }
        if (r->status == Status::Evaluated) continue;
        if (r->status == Status::Evaluating) {
            // Running already (another graph started it, or a cycle): wait for it.
            if (r->promise && r->promise->promise_state == 1) continue;
            if (r->promise && r->promise->promise_state == 0 && r->holder && r->holder->co && r->holder->co->state != CoState::Running) {
                waitOn(rt, seq_holder, r->promise);
                return;
            }
            continue;   // a cycle back to one that's running now: go on, as the spec does
        }
        if (r->status != Status::Linked) continue;
        r->status = Status::Evaluating;
        r->promise = rt.newPromise();
        r->holder = rt.allocate<Object>(nullptr, ObjectClass::Internal);
        r->holder->co = std::make_shared<Coroutine>(rt, CoKind::Module);
        Coroutine& co = *r->holder->co;
        co.module = r;
        co.holder = r->holder;
        co.promise = r->promise;
        co.env = r->env;
        resumeCoroutine(co);
        if (co.terminated) {
            const Terminated t = *co.terminated;
            co.terminated.reset();
            throw t;
        }
        if (co.state == CoState::Done) {
            if (co.threw) {
                r->status = Status::Errored;
                r->failure = co.transfer;
                rt.rejectPromise(r->promise, co.transfer);
                rt.rejectPromise(seq->promise, co.transfer);
                r->promise->promise_handled = true;
                delete seq;
                seq_holder->host = nullptr;
                return;
            }
            r->status = Status::Evaluated;
            rt.resolvePromise(r->promise, Value::undefined());
            continue;
        }
        // It's awaiting at its top level: carry on once it's done.
        waitOn(rt, seq_holder, r->promise);
        return;
    }
    rt.resolvePromise(seq->promise, Value::undefined());
    delete seq;
    seq_holder->host = nullptr;
}

// Evaluates a linked graph; the promise settles when it's all run.
Object* evaluateGraph(Runtime& rt, Runtime::Modules& m, ModuleRecord* root) {
    auto* seq = new Sequence;
    std::set<ModuleRecord*> seen;
    postOrder(m, root, seen, seq->order);
    seq->promise = rt.newPromise();
    Object* promise = seq->promise;
    Object* seq_holder = rt.allocate<Object>(nullptr, ObjectClass::Internal);
    seq_holder->host = seq;
    seq_holder->slots = {Value::object(promise)};
    step(rt, seq_holder);
    return promise;
}

}  // namespace

// A module body's coroutine: when its statements finish (or throw), its record says so.
void runModuleBody(Runtime& rt, Coroutine& co) {
    auto* rec = static_cast<ModuleRecord*>(co.module);
    runModuleStatements(rt, rec->program, rec->env);
}

void markModule(void* record, std::vector<Cell*>& todo) {
    auto* r = static_cast<ModuleRecord*>(record);
    for (Cell* c : std::initializer_list<Cell*>{r->env, r->ns, r->meta, r->holder, r->promise})
        if (c) todo.push_back(c);
    if (r->failure.isObject()) todo.push_back(r->failure.asObject());
}

void Runtime::markModules(std::vector<Cell*>& todo) {
    if (!modules_) return;
    for (auto& [url, r] : modules_->records) markModule(r.get(), todo);
    for (Object* p : modules_->pending_imports) todo.push_back(p);
}

Object* importMeta(Runtime& rt, const std::string& url) {
    ModuleRecord* rec = nullptr;
    auto& m = rt.modulesForLoader();
    if (const auto it = m.records.find(url); it != m.records.end()) rec = it->second.get();
    if (rec && rec->meta) return rec->meta;
    Object* meta = rt.allocate<Object>(nullptr, ObjectClass::Ordinary);
    std::string shown = url;
    if (shown.find("#<inline") != std::string::npos) shown.resize(shown.find("#<inline"));
    rt.put(meta, u"url", Value::string(utf16(shown)), kDefaultFlags);
    const std::string base = url;
    Object* resolve = rt.newFunction(u"resolve", 1, nullptr);
    resolve->slots = {Value::string(utf16(base))};
    resolve->native = [resolve](Runtime& rt, const Value&, Args a) {
        const std::string spec = utf8(*rt.toString(a.empty() ? Value::undefined() : a[0]));
        const std::string r = rt.import_map.resolve(spec, utf8(resolve->slots[0].asString()));
        if (r.empty()) rt.throwError("TypeError", "can't resolve the module specifier '" + spec + "'");
        return Value::string(utf16(r));
    };
    rt.put(meta, u"resolve", Value::object(resolve), kDefaultFlags);
    if (rec) rec->meta = meta;
    return meta;
}

namespace {

// Loads, links and evaluates the graph at `url`, then calls `then` with the root's
// record, or with an error (as a thrown value).
void loadLinkEvaluate(Runtime& rt, const std::string& url, std::function<void(ModuleRecord*, Object* evaluated, const Value& error)> then) {
    auto& m = rt.modulesForLoader();
    auto load = std::make_shared<GraphLoad>();
    load->root = url;
    Runtime* rtp = &rt;
    std::weak_ptr<bool> alive = m.alive;
    load->done = [rtp, alive, url, then](const std::string& error) {
        if (alive.expired()) return;
        Runtime& rt = *rtp;
        auto& m = rt.modulesForLoader();
        ModuleRecord* root = m.records.count(url) ? m.records[url].get() : nullptr;
        if (!error.empty() || !root) {
            then(nullptr, nullptr, Value::object(rt.newError(error.rfind("SyntaxError", 0) == 0 ? "SyntaxError" : "TypeError", error.empty() ? "module failed to load" : error)));
            return;
        }
        Linker linker{rt, m};
        const std::string link_error = linker.link(root);
        if (!link_error.empty()) {
            const bool syntax = link_error.rfind("SyntaxError: ", 0) == 0;
            then(nullptr, nullptr, Value::object(rt.newError(syntax ? "SyntaxError" : "TypeError", syntax ? link_error.substr(13) : link_error)));
            return;
        }
        Object* evaluated = evaluateGraph(rt, m, root);
        then(root, evaluated, Value::undefined());
    };
    ++load->pending;   // until the root's own walk is done
    Loader{rt, m}.visit(load, url);
    --load->pending;
    Loader{rt, m}.finishIfDone(load);
}

}  // namespace

std::vector<std::string> moduleRequests(std::string_view source) {
    std::string error;
    std::unique_ptr<Program> prog = parse(utf16(source), error, true, true);
    std::vector<std::string> out;
    if (!prog) return out;
    for (Node* s : prog->root->kids) {
        if (s->kind == N::ImportDecl || (s->kind == N::ExportDecl && !s->str.empty())) {
            std::string spec = utf8(s->str);
            if (std::find(out.begin(), out.end(), spec) == out.end()) out.push_back(std::move(spec));
        }
    }
    return out;
}

void Runtime::evaluateModule(std::string_view source, const std::string& url, std::function<void(Result)> done) {
    if (!current_co && !onEngineStack()) {
        runOnEngineStack([&] { evaluateModule(source, url, done); });
        return;
    }
    auto& m = modulesForLoader();
    // Inline modules each get a record of their own, keyed apart from the page's URL.
    std::string key = url;
    if (m.records.count(key)) key = url + "#<inline " + std::to_string(++m.inline_count) + ">";
    auto owned = std::make_unique<ModuleRecord>();
    owned->url = key;
    ModuleRecord* rec = owned.get();
    m.records[key] = std::move(owned);
    Loader{*this, m}.parsed(rec, std::string(source));
    const auto report = [this, done](const Value& error) {
        Result r;
        r.ok = false;
        try {
            r.error = describe(*this, error);
        } catch (const Thrown&) {
            r.error = "Uncaught exception";
        }
        if (done) done(r);
    };
    if (rec->status == Status::FetchFailed) {
        Result r;
        r.error = rec->error;
        if (done) done(r);
        return;
    }
    const bool fresh = env_stack.empty() && call_depth == 0;
    if (fresh) resetSteps();
    try {
        loadLinkEvaluate(*this, key, [this, done, report](ModuleRecord*, Object* evaluated, const Value& error) {
            if (!evaluated) {
                report(error);
                return;
            }
            // Report when it settles.
            Object* on_ok = newFunction(u"", 1, [done](Runtime&, const Value&, Args) {
                Result r;
                r.ok = true;
                if (done) done(r);
                return Value::undefined();
            });
            Object* on_err = newFunction(u"", 1, [report](Runtime&, const Value&, Args a) {
                report(a.empty() ? Value::undefined() : a[0]);
                return Value::undefined();
            });
            const Value fns[2] = {Value::object(on_ok), Value::object(on_err)};
            call(Value::object(promise_then), Value::object(evaluated), fns);
        });
    } catch (const Thrown& t) {
        report(t.value);
    } catch (const Terminated& t) {
        Result r;
        r.error = "Stopped: " + t.why;
        if (done) done(r);
    } catch (const std::bad_alloc&) {
        Result r;
        r.error = "Stopped: out of memory";
        if (done) done(r);
    }
}

Value dynamicImport(Runtime& rt, const Value& specifier, const std::string& referrer) {
    Object* promise = rt.newPromise();
    std::string spec;
    try {
        spec = utf8(*rt.toString(specifier));
    } catch (const Thrown& t) {
        rt.rejectPromise(promise, t.value);
        return Value::object(promise);
    }
    std::string base = referrer;
    if (Runtime::resolveSpecifier("./x", base).empty()) base = rt.base_url;
    const std::string url = rt.import_map.resolve(spec, base);
    if (url.empty()) {
        rt.rejectPromise(promise, Value::object(rt.newError("TypeError", "can't resolve the module specifier '" + spec + "'")));
        return Value::object(promise);
    }
    // Kept as a root while its graph loads (the loader may answer later).
    auto& m = rt.modulesForLoader();
    m.pending_imports.push_back(promise);
    Runtime* rtp = &rt;
    loadLinkEvaluate(rt, url, [rtp, promise](ModuleRecord* root, Object* evaluated, const Value& error) {
        Runtime& rt = *rtp;
        auto& m = rt.modulesForLoader();
        auto& pend = m.pending_imports;
        pend.erase(std::remove(pend.begin(), pend.end(), promise), pend.end());
        if (!evaluated) {
            rt.rejectPromise(promise, error);
            return;
        }
        // Once it's run, the promise gets its namespace.
        Object* on_ok = rt.newFunction(u"", 1, nullptr);
        on_ok->slots = {Value::object(promise)};
        on_ok->host = root;
        on_ok->native = [on_ok](Runtime& rt, const Value&, Args) {
            Linker linker{rt, rt.modulesForLoader()};
            rt.resolvePromise(on_ok->slots[0].asObject(), Value::object(linker.namespaceOf(static_cast<ModuleRecord*>(on_ok->host))));
            return Value::undefined();
        };
        Object* on_err = rt.newFunction(u"", 1, nullptr);
        on_err->slots = {Value::object(promise)};
        on_err->native = [on_err](Runtime& rt, const Value&, Args a) {
            rt.rejectPromise(on_err->slots[0].asObject(), a.empty() ? Value::undefined() : a[0]);
            return Value::undefined();
        };
        const Value fns[2] = {Value::object(on_ok), Value::object(on_err)};
        rt.call(Value::object(rt.promise_then), Value::object(evaluated), fns);
    });
    return Value::object(promise);
}

}  // namespace fbx::xweb::js
