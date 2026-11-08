// xjs [--module] file.js... — runs scripts on our JavaScript engine, console output to
// stdout and stderr. A .mjs file, or any file after --module, runs as a module (its
// imports load from files). Promise jobs run after each script, then timers
// (setTimeout) until there are none. Exit status 1 if a script threw or failed to
// parse, or a microtask threw.

#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "xweb/js.h"

namespace {

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

std::string fileUrl(const std::string& path) {
    char buf[PATH_MAX];
    if (realpath(path.c_str(), buf)) return std::string("file://") + buf;
    return "file://" + path;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace fbx::xweb::js;
    Runtime rt;
    int status = 0;
    rt.setConsole([](std::string_view level, const std::string& text) {
        (level == "error" || level == "warn" ? std::cerr : std::cout) << text << "\n";
    });
    rt.setErrorReporter([&status](const std::string& message) {
        std::cerr << message << "\n";
        // A rejection nobody handled is reported, as browsers do, but isn't a failure.
        if (message.rfind("Uncaught (in promise)", 0) != 0) status = 1;
    });
    rt.setModuleLoader([](const std::string& url, std::function<void(ModuleSource)> done) {
        ModuleSource src;
        if (url.rfind("file://", 0) != 0) {
            src.error = "only file: URLs load here";
        } else if (readFile(url.substr(7), src.source)) {
            src.ok = true;
        } else {
            src.error = "can't read it";
        }
        done(std::move(src));
    });

    // setTimeout and clearTimeout, run in order of when they're due once scripts are done.
    struct Timer {
        int id;
        double due;
        uint64_t seq;
        Value fn;
        std::vector<Value> args;
    };
    std::vector<Timer> timers;
    int next_id = 1;
    uint64_t seq = 0;
    double now = 0;
    rt.method(rt.global(), u"setTimeout", 2, [&](Runtime& r, const Value&, std::span<const Value> a) {
        const double delay = a.size() > 1 ? std::max(0.0, r.toNumber(a[1])) : 0;
        Timer t{next_id++, now + delay, seq++, a.empty() ? Value::undefined() : a[0], {}};
        if (a.size() > 2) t.args.assign(a.begin() + 2, a.end());
        timers.push_back(std::move(t));
        return Value::number(timers.back().id);
    });
    rt.method(rt.global(), u"clearTimeout", 1, [&](Runtime& r, const Value&, std::span<const Value> a) {
        const int id = a.empty() ? 0 : static_cast<int>(r.toNumber(a[0]));
        timers.erase(std::remove_if(timers.begin(), timers.end(), [id](const Timer& t) { return t.id == id; }), timers.end());
        return Value::undefined();
    });
    // $262, the host hooks test262 asks a shell for.
    {
        Object* host = rt.newObject();
        rt.method(host, u"detachArrayBuffer", 1, [](Runtime& r, const Value&, std::span<const Value> a) {
            if (a.empty() || !r.detachArrayBuffer(a[0])) r.throwError("TypeError", "not an ArrayBuffer");
            return Value::null();
        });
        rt.method(host, u"evalScript", 1, [](Runtime& r, const Value&, std::span<const Value> a) {
            const Runtime::Result res = r.evaluate(utf8(*r.toString(a.empty() ? Value::undefined() : a[0])), "evalScript");
            if (!res.ok) r.throwError(res.error.rfind("SyntaxError", 0) == 0 ? "SyntaxError" : "Error", res.error);
            return res.value;
        });
        rt.method(host, u"gc", 0, [](Runtime&, const Value&, std::span<const Value>) { return Value::undefined(); });
        rt.put(host, u"global", Value::object(rt.global()));
        rt.put(rt.global(), u"$262", Value::object(host));
    }
    // print(), as other engines' shells have it.
    rt.method(rt.global(), u"print", 1, [](Runtime& r, const Value&, std::span<const Value> a) {
        std::string line;
        for (size_t i = 0; i < a.size(); ++i) line += (i ? " " : "") + utf8(*r.toString(a[i]));
        std::cout << line << "\n";
        return Value::undefined();
    });
    rt.method(rt.global(), u"queueMicrotask", 1, [](Runtime& r, const Value&, std::span<const Value> a) {
        if (a.empty() || !r.isCallable(a[0])) r.throwError("TypeError", "queueMicrotask needs a function");
        r.enqueueMicrotask(a[0]);
        return Value::undefined();
    });
    const auto runJobs = [&] {
        try {
            rt.runJobs();
        } catch (const Terminated& t) {
            std::cerr << "Stopped: " << t.why << "\n";
            status = 1;
        }
    };

    bool module = false;
    for (int i = 1; i < argc; ++i) {
        const std::string path = argv[i];
        if (path == "--module") {
            module = true;
            continue;
        }
        std::string src;
        if (!readFile(path, src)) {
            std::cerr << "can't read " << path << "\n";
            return 2;
        }
        if (module || (path.size() > 4 && path.substr(path.size() - 4) == ".mjs")) {
            rt.evaluateModule(src, fileUrl(path), [&status, path](Runtime::Result r) {
                if (!r.ok) {
                    std::cerr << path << ": " << r.error << "\n";
                    status = 1;
                }
            });
        } else {
            rt.base_url = fileUrl(path);
            const Runtime::Result r = rt.evaluate(src, rt.base_url);
            if (!r.ok) {
                std::cerr << path << ": " << r.error << "\n";
                status = 1;
            }
        }
        runJobs();
    }
    // The event loop: timers in order, each followed by its jobs.
    while (!timers.empty()) {
        const auto it = std::min_element(timers.begin(), timers.end(), [](const Timer& a, const Timer& b) {
            return a.due != b.due ? a.due < b.due : a.seq < b.seq;
        });
        Timer t = std::move(*it);
        timers.erase(it);
        now = std::max(now, t.due);
        rt.resetSteps();
        try {
            rt.call(t.fn, Value::undefined(), t.args);
        } catch (const Thrown& e) {
            std::cerr << "Uncaught " << describe(rt, e.value) << "\n";
            status = 1;
        } catch (const Terminated& e) {
            std::cerr << "Stopped: " << e.why << "\n";
            status = 1;
        }
        runJobs();
        // Between tasks, as a page's event loop does: suspended coroutines' stacks
        // are scanned, and the ones nothing reaches are unwound. Pending timers' values
        // are roots.
        rt.extra_roots.clear();
        for (const Timer& pending : timers) {
            rt.extra_roots.push_back(pending.fn);
            rt.extra_roots.insert(rt.extra_roots.end(), pending.args.begin(), pending.args.end());
        }
        rt.collectGarbage();
        rt.extra_roots.clear();
    }
    rt.collectGarbage();
    return status;
}
