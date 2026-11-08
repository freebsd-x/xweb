// Promise, the microtask queue, generators, async functions and async generators, as
// the specification has them (27.2 to 27.7). Generators and async functions run on
// coroutines (coroutine.h): yield and await suspend the body's own machine stack.

#include <cmath>

#include "coroutine.h"
#include "internal.h"

namespace fbx::xweb::js {

namespace {

using Args = std::span<const Value>;

Value arg(Args a, size_t i) { return i < a.size() ? a[i] : Value::undefined(); }

enum JobKind : uint8_t { kReactionJob = 0, kThenableJob = 1, kCallbackJob = 2 };
enum PromiseState : uint8_t { kPending = 0, kFulfilled = 1, kRejected = 2 };

Object* internalObject(Runtime& rt) { return rt.allocate<Object>(nullptr, ObjectClass::Internal); }

// A built-in function whose code reads what it captured from its own slots, where the
// collector sees them.
using ClosureFn = Value (*)(Runtime&, Object* self, const Value& this_value, Args args);
Object* closure(Runtime& rt, const char16_t* name, int length, std::vector<Value> slots, ClosureFn fn) {
    Object* f = rt.newFunction(name, length, nullptr);
    f->slots = std::move(slots);
    f->native = [f, fn](Runtime& r, const Value& t, Args a) { return fn(r, f, t, a); };
    return f;
}

bool isPromise(const Value& v) { return v.isObject() && v.asObject()->cls == ObjectClass::Promise; }

// An error caught on its way out, to be dealt with after the C++ handler is done.
struct Caught {
    bool threw = false;
    Value value;
};
#define FBX_CATCH_THROWN(c) catch (const Thrown& thrown_) { (c).threw = true; (c).value = thrown_.value; }

// ── Promise internals ──

void triggerReactions(Runtime& rt, Object* p, uint8_t state, const Value& argument) {
    std::vector<Value> reactions;
    reactions.swap(p->slots);
    for (const Value& r : reactions) rt.enqueueJob(Job{kReactionJob, r, Value::number(state), argument});
}

void fulfill(Runtime& rt, Object* p, const Value& v) {
    if (p->promise_state != kPending) return;
    p->promise_state = kFulfilled;
    p->primitive = v;
    triggerReactions(rt, p, kFulfilled, v);
}

// The resolve and reject functions handed to an executor or a thenable, sharing
// whether either has been called.
std::pair<Object*, Object*> resolvingFunctions(Runtime& rt, Object* p) {
    Object* flag = internalObject(rt);
    Object* res = closure(rt, u"", 1, {Value::object(p), Value::object(flag)}, [](Runtime& rt, Object* self, const Value&, Args a) {
        Object* flag = self->slots[1].asObject();
        if (flag->cursor) return Value::undefined();
        flag->cursor = 1;
        rt.resolvePromise(self->slots[0].asObject(), arg(a, 0));
        return Value::undefined();
    });
    Object* rej = closure(rt, u"", 1, {Value::object(p), Value::object(flag)}, [](Runtime& rt, Object* self, const Value&, Args a) {
        Object* flag = self->slots[1].asObject();
        if (flag->cursor) return Value::undefined();
        flag->cursor = 1;
        rt.rejectPromise(self->slots[0].asObject(), arg(a, 0));
        return Value::undefined();
    });
    return {res, rej};
}

// A promise and the way to settle it. With the built-in constructor and no functions
// asked for, it's settled from C++ directly (resolve and reject stay undefined).
struct Capability {
    Value promise, resolve, reject;
};

Capability newCapability(Runtime& rt, const Value& c, bool functions) {
    if (c.isObject() && c.asObject() == rt.promise_constructor) {
        Object* p = rt.newPromise();
        if (!functions) return {Value::object(p), {}, {}};
        auto [res, rej] = resolvingFunctions(rt, p);
        return {Value::object(p), Value::object(res), Value::object(rej)};
    }
    if (!rt.isConstructor(c)) rt.throwError("TypeError", "not a promise constructor");
    Object* executor = closure(rt, u"", 2, {Value::undefined(), Value::undefined()}, [](Runtime& rt, Object* self, const Value&, Args a) {
        if (!self->slots[0].isUndefined() || !self->slots[1].isUndefined()) rt.throwError("TypeError", "a promise capability's executor was called twice");
        self->slots[0] = arg(a, 0);
        self->slots[1] = arg(a, 1);
        return Value::undefined();
    });
    const Value ev = Value::object(executor);
    const Value promise = rt.construct(c, std::span<const Value>(&ev, 1));
    if (!rt.isCallable(executor->slots[0]) || !rt.isCallable(executor->slots[1])) rt.throwError("TypeError", "a promise constructor didn't give resolve and reject functions");
    return {promise, executor->slots[0], executor->slots[1]};
}

void settle(Runtime& rt, const Capability& cap, bool ok, const Value& v) {
    if (cap.resolve.isUndefined()) {
        if (!cap.promise.isObject()) return;
        if (ok) rt.resolvePromise(cap.promise.asObject(), v);
        else rt.rejectPromise(cap.promise.asObject(), v);
        return;
    }
    rt.call(ok ? cap.resolve : cap.reject, Value::undefined(), std::span<const Value>(&v, 1));
}

// PerformPromiseThen: a reaction record [capability promise, resolve, reject, on fulfilled, on rejected].
void performThen(Runtime& rt, Object* p, const Value& on_fulfilled, const Value& on_rejected, const Capability* cap) {
    Object* r = internalObject(rt);
    r->slots = {cap ? cap->promise : Value::undefined(), cap ? cap->resolve : Value::undefined(), cap ? cap->reject : Value::undefined(),
                rt.isCallable(on_fulfilled) ? on_fulfilled : Value::undefined(), rt.isCallable(on_rejected) ? on_rejected : Value::undefined()};
    if (p->promise_state == kPending) {
        p->slots.push_back(Value::object(r));
    } else {
        if (p->promise_state == kRejected && !p->promise_handled) {
            auto& pr = rt.pending_rejections;
            pr.erase(std::remove(pr.begin(), pr.end(), p), pr.end());
        }
        rt.enqueueJob(Job{kReactionJob, Value::object(r), Value::number(p->promise_state), p->primitive});
    }
    p->promise_handled = true;
}

// PromiseResolve(C, x): x itself when it's already such a promise.
Value promiseResolve(Runtime& rt, const Value& c, const Value& x) {
    if (isPromise(x)) {
        const Value ctor = rt.get(x, u"constructor");
        if (Runtime::sameValue(ctor, c)) return x;
    }
    const Capability cap = newCapability(rt, c, false);
    settle(rt, cap, true, x);
    return cap.promise;
}

Object* thisPromise(Runtime& rt, const Value& t, const char* what) {
    if (!isPromise(t)) rt.throwError("TypeError", std::string("Promise.prototype.") + what + " called on something that isn't a promise");
    return t.asObject();
}

Value speciesConstructor(Runtime& rt, const Value& o, Object* fallback) {
    const Value c = rt.get(o, u"constructor");
    if (c.isUndefined()) return Value::object(fallback);
    if (!c.isObject()) rt.throwError("TypeError", "a constructor property isn't an object");
    const Value s = rt.get(c, PropertyKey(rt.sym_species));
    if (s.isNullish()) return Value::object(fallback);
    if (rt.isConstructor(s)) return s;
    rt.throwError("TypeError", "Symbol.species isn't a constructor");
}

Value invoke(Runtime& rt, const Value& o, const char16_t* name, std::vector<Value> args) {
    const Value f = rt.get(o, name);
    return rt.call(f, o, args);
}

Value thenOf(Runtime& rt, const Value& promise, const Value& on_fulfilled, const Value& on_rejected) {
    Object* p = promise.asObject();
    const Value c = speciesConstructor(rt, promise, rt.promise_constructor);
    const Capability cap = newCapability(rt, c, false);
    performThen(rt, p, on_fulfilled, on_rejected, &cap);
    return cap.promise;
}

// Iterating for the combinators, closing the iterator if something goes wrong midway.
struct NativeIter {
    Value iterator, next;
    bool done = false;
};

NativeIter getIter(Runtime& rt, const Value& v) {
    const Value m = v.isNullish() ? Value::undefined() : rt.get(v, PropertyKey(rt.sym_iterator));
    if (!rt.isCallable(m)) rt.throwError("TypeError", "not iterable");
    NativeIter it;
    it.iterator = rt.call(m, v, {});
    if (!it.iterator.isObject()) rt.throwError("TypeError", "an iterator must be an object");
    it.next = rt.get(it.iterator, u"next");
    return it;
}

bool iterNext(Runtime& rt, NativeIter& it, Value& out) {
    it.done = true;
    const Value r = rt.call(it.next, it.iterator, {});
    if (!r.isObject()) rt.throwError("TypeError", "an iterator result must be an object");
    if (rt.toBoolean(rt.get(r, u"done"))) return false;
    out = rt.get(r, u"value");
    it.done = false;
    return true;
}

void iterCloseQuietly(Runtime& rt, NativeIter& it) {
    if (it.done) return;
    it.done = true;
    try {
        const Value ret = rt.get(it.iterator, u"return");
        if (!ret.isNullish()) rt.call(ret, it.iterator, {});
    } catch (const Thrown&) {
    }
}

Object* newAggregateError(Runtime& rt, Object* errors) {
    const auto it = rt.error_prototypes.find("AggregateError");
    Object* e = rt.allocate<Object>(it != rt.error_prototypes.end() ? it->second : rt.error_prototype, ObjectClass::Error);
    rt.put(e, u"message", Value::string(u"All promises were rejected"));
    rt.put(e, u"errors", Value::object(errors));
    return e;
}

// Promise.all, allSettled, any and race share their loop; `each` hooks one value's promise up.
enum class Combinator { All, AllSettled, Any, Race };

Value combinator(Runtime& rt, const Value& c, const Value& iterable, Combinator kind) {
    const Capability cap = newCapability(rt, c, true);
    Caught failed;
    NativeIter it;
    bool have_iter = false;
    try {
        const Value resolve_fn = rt.get(c, u"resolve");
        if (!rt.isCallable(resolve_fn)) rt.throwError("TypeError", "the constructor's resolve isn't a function");
        it = getIter(rt, iterable);
        have_iter = true;
        Object* values = rt.newArray();
        Object* remaining = internalObject(rt);
        remaining->cursor = 1;
        size_t index = 0;
        Value v;
        while (iterNext(rt, it, v)) {
            const Value next_promise = rt.call(resolve_fn, c, std::span<const Value>(&v, 1));
            if (kind == Combinator::Race) {
                invoke(rt, next_promise, u"then", {cap.resolve, cap.reject});
                continue;
            }
            values->elements.push_back(Value::undefined());
            ++remaining->cursor;
            Object* called = internalObject(rt);
            const std::vector<Value> slots{Value::object(values), Value::number(static_cast<double>(index)), Value::object(remaining),
                                           cap.resolve, cap.reject, Value::object(called)};
            const auto element = [&](const char16_t* name, ClosureFn fn) { return Value::object(closure(rt, name, 1, slots, fn)); };
            // slots: values, index, remaining, resolve, reject, already-called.
            Value on_ok, on_err;
            if (kind == Combinator::All) {
                on_ok = element(u"", [](Runtime& rt, Object* self, const Value&, Args a) {
                    Object* called = self->slots[5].asObject();
                    if (called->cursor) return Value::undefined();
                    called->cursor = 1;
                    Object* values = self->slots[0].asObject();
                    values->elements[static_cast<size_t>(self->slots[1].asNumber())] = arg(a, 0);
                    if (--self->slots[2].asObject()->cursor == 0) {
                        const Value all = Value::object(values);
                        rt.call(self->slots[3], Value::undefined(), std::span<const Value>(&all, 1));
                    }
                    return Value::undefined();
                });
                on_err = cap.reject;
            } else if (kind == Combinator::AllSettled) {
                const auto settled = [](Runtime& rt, Object* self, Args a, bool ok) {
                    Object* called = self->slots[5].asObject();
                    if (called->cursor) return Value::undefined();
                    called->cursor = 1;
                    Object* entry = rt.newObject();
                    rt.put(entry, u"status", Value::string(ok ? u"fulfilled" : u"rejected"), kDefaultFlags);
                    rt.put(entry, ok ? u"value" : u"reason", arg(a, 0), kDefaultFlags);
                    Object* values = self->slots[0].asObject();
                    values->elements[static_cast<size_t>(self->slots[1].asNumber())] = Value::object(entry);
                    if (--self->slots[2].asObject()->cursor == 0) {
                        const Value all = Value::object(values);
                        rt.call(self->slots[3], Value::undefined(), std::span<const Value>(&all, 1));
                    }
                    return Value::undefined();
                };
                on_ok = element(u"", [](Runtime& rt, Object* self, const Value&, Args a) {
                    return decltype(settled)()(rt, self, a, true);
                });
                on_err = element(u"", [](Runtime& rt, Object* self, const Value&, Args a) {
                    return decltype(settled)()(rt, self, a, false);
                });
            } else {
                on_ok = cap.resolve;
                on_err = element(u"", [](Runtime& rt, Object* self, const Value&, Args a) {
                    Object* called = self->slots[5].asObject();
                    if (called->cursor) return Value::undefined();
                    called->cursor = 1;
                    Object* errors = self->slots[0].asObject();
                    errors->elements[static_cast<size_t>(self->slots[1].asNumber())] = arg(a, 0);
                    if (--self->slots[2].asObject()->cursor == 0) {
                        const Value e = Value::object(newAggregateError(rt, errors));
                        rt.call(self->slots[4], Value::undefined(), std::span<const Value>(&e, 1));
                    }
                    return Value::undefined();
                });
            }
            invoke(rt, next_promise, u"then", {on_ok, on_err});
            ++index;
        }
        if (kind != Combinator::Race && --remaining->cursor == 0) {
            if (kind == Combinator::Any) {
                const Value e = Value::object(newAggregateError(rt, values));
                rt.call(cap.reject, Value::undefined(), std::span<const Value>(&e, 1));
            } else {
                const Value all = Value::object(values);
                rt.call(cap.resolve, Value::undefined(), std::span<const Value>(&all, 1));
            }
        }
    }
    FBX_CATCH_THROWN(failed)
    if (failed.threw) {
        if (have_iter) iterCloseQuietly(rt, it);
        rt.call(cap.reject, Value::undefined(), std::span<const Value>(&failed.value, 1));
    }
    return cap.promise;
}

// ── Coroutine-run functions ──

Value takeResumption(Coroutine& co) {
    const Value v = co.transfer;
    co.transfer = Value::undefined();
    switch (co.mode) {
        case ResumeMode::Throw: throw Thrown{v};
        case ResumeMode::Return: throw GeneratorReturn{v};
        default: return v;
    }
}

void rethrowTermination(Coroutine& co) {
    if (co.terminated) {
        const Terminated t = *co.terminated;
        co.terminated.reset();
        throw t;
    }
}

void releaseBody(Coroutine& co) {
    co.function = nullptr;
    co.env = nullptr;
}

void continueAsync(Runtime& rt, Object* holder);

// An await's continuation: the awaited promise settled.
Value resumeFromAwait(Runtime& rt, Object* self, ResumeMode mode, const Value& v) {
    Object* holder = self->slots[0].asObject();
    Coroutine& co = *holder->co;
    if (co.state != CoState::Suspended || !co.awaiting) return Value::undefined();
    co.awaiting = false;
    co.mode = mode;
    co.transfer = v;
    resumeCoroutine(co);
    continueAsync(rt, holder);
    return Value::undefined();
}

Object* iterResult(Runtime& rt, const Value& v, bool done) { return rt.createIterResult(v, done); }

void asyncGenDrain(Runtime& rt, Object* holder);

// After an async body ran to a suspension or its end: settle what its end settles.
void continueAsync(Runtime& rt, Object* holder) {
    Coroutine& co = *holder->co;
    rethrowTermination(co);
    if (co.kind == CoKind::AsyncGenerator) {
        if (co.state == CoState::Done && !co.requests.empty() && co.function) {
            // Its body just ended: that settles the request it was serving.
            const AsyncGenRequest req = co.requests.front();
            co.requests.pop_front();
            const Value result = co.transfer;
            const bool threw = co.threw;
            co.transfer = Value::undefined();
            co.threw = false;
            releaseBody(co);
            if (threw) rt.rejectPromise(req.promise, result);
            else rt.resolvePromise(req.promise, Value::object(iterResult(rt, result, true)));
        }
        asyncGenDrain(rt, holder);
        return;
    }
    if (co.state != CoState::Done) return;
    releaseBody(co);
    if (!co.promise) return;
    if (co.threw) rt.rejectPromise(co.promise, co.transfer);
    else rt.resolvePromise(co.promise, co.transfer);
    co.transfer = Value::undefined();
}

// The requests a suspended or finished async generator can serve now.
void asyncGenDrain(Runtime& rt, Object* holder) {
    Coroutine& co = *holder->co;
    while (co.state != CoState::Running && !co.awaiting && !co.awaiting_return && !co.requests.empty()) {
        AsyncGenRequest& req = co.requests.front();
        if (co.state == CoState::Fresh && req.mode != ResumeMode::Next) {
            co.state = CoState::Done;
            releaseBody(co);
        }
        if (co.state == CoState::Done) {
            if (req.mode == ResumeMode::Next) {
                Object* p = req.promise;
                co.requests.pop_front();
                rt.resolvePromise(p, Value::object(iterResult(rt, Value::undefined(), true)));
                continue;
            }
            if (req.mode == ResumeMode::Throw) {
                Object* p = req.promise;
                const Value v = req.value;
                co.requests.pop_front();
                rt.rejectPromise(p, v);
                continue;
            }
            // return(v) on a finished generator: v is awaited, then it's {value: v, done: true}.
            co.awaiting_return = true;
            Caught failed;
            Value awaited;
            try {
                awaited = promiseResolve(rt, Value::object(rt.promise_constructor), req.value);
            }
            FBX_CATCH_THROWN(failed)
            if (failed.threw) {
                co.awaiting_return = false;
                Object* p = req.promise;
                co.requests.pop_front();
                rt.rejectPromise(p, failed.value);
                continue;
            }
            const auto done = [](Runtime& rt, Object* self, const Value&, Args a, bool ok) {
                Object* holder = self->slots[0].asObject();
                Coroutine& co = *holder->co;
                co.awaiting_return = false;
                if (!co.requests.empty()) {
                    Object* p = co.requests.front().promise;
                    co.requests.pop_front();
                    if (ok) rt.resolvePromise(p, Value::object(iterResult(rt, arg(a, 0), true)));
                    else rt.rejectPromise(p, arg(a, 0));
                }
                asyncGenDrain(rt, holder);
                return Value::undefined();
            };
            Object* on_ok = closure(rt, u"", 1, {Value::object(holder)}, [](Runtime& rt, Object* self, const Value& t, Args a) {
                return decltype(done)()(rt, self, t, a, true);
            });
            Object* on_err = closure(rt, u"", 1, {Value::object(holder)}, [](Runtime& rt, Object* self, const Value& t, Args a) {
                return decltype(done)()(rt, self, t, a, false);
            });
            performThen(rt, awaited.asObject(), Value::object(on_ok), Value::object(on_err), nullptr);
            return;
        }
        // Fresh (a next) or suspended at a yield: run it with this request.
        co.mode = req.mode;
        co.transfer = req.value;
        resumeCoroutine(co);
        rethrowTermination(co);
        if (co.state == CoState::Done && co.function) {
            const AsyncGenRequest r = co.requests.front();
            co.requests.pop_front();
            const Value result = co.transfer;
            const bool threw = co.threw;
            co.transfer = Value::undefined();
            co.threw = false;
            releaseBody(co);
            if (threw) rt.rejectPromise(r.promise, result);
            else rt.resolvePromise(r.promise, Value::object(iterResult(rt, result, true)));
        }
    }
}

Value asyncGenEnqueue(Runtime& rt, const Value& g, ResumeMode mode, const Value& v) {
    Object* promise = rt.newPromise();
    if (!g.isObject() || g.asObject()->cls != ObjectClass::AsyncGenerator || !g.asObject()->co) {
        rt.rejectPromise(promise, Value::object(rt.newError("TypeError", "not an async generator")));
        return Value::object(promise);
    }
    Object* holder = g.asObject();
    holder->co->requests.push_back(AsyncGenRequest{mode, v, promise});
    asyncGenDrain(rt, holder);
    return Value::object(promise);
}

// A sync generator's next, return or throw.
Value generatorResume(Runtime& rt, const Value& g, ResumeMode mode, const Value& v) {
    if (!g.isObject() || g.asObject()->cls != ObjectClass::Generator || !g.asObject()->co) rt.throwError("TypeError", "not a generator");
    Coroutine& co = *g.asObject()->co;
    if (co.state == CoState::Running) rt.throwError("TypeError", "the generator is already running");
    if (co.state == CoState::Fresh && mode != ResumeMode::Next) {
        co.state = CoState::Done;
        releaseBody(co);
    }
    if (co.state == CoState::Done) {
        if (mode == ResumeMode::Throw) throw Thrown{v};
        return Value::object(iterResult(rt, mode == ResumeMode::Return ? v : Value::undefined(), true));
    }
    co.mode = mode;
    co.transfer = v;
    resumeCoroutine(co);
    rethrowTermination(co);
    Value out = co.transfer;
    co.transfer = Value::undefined();
    if (co.state == CoState::Done) {
        releaseBody(co);
        if (co.threw) {
            co.threw = false;
            throw Thrown{out};
        }
        return Value::object(iterResult(rt, out, true));
    }
    if (co.transfer_is_result) return out;
    return Value::object(iterResult(rt, out, false));
}

// An async generator's yield, once the value's awaited: settles the request it's
// serving, then carries on with the next one if there is one, or suspends.
std::pair<ResumeMode, Value> asyncGenYieldRaw(Runtime& rt, Coroutine& co, const Value& v) {
    if (!co.requests.empty()) {
        Object* p = co.requests.front().promise;
        co.requests.pop_front();
        rt.resolvePromise(p, Value::object(iterResult(rt, v, false)));
    }
    if (co.requests.empty()) {
        co.awaiting = false;
        suspendCoroutine(co);
    }
    const AsyncGenRequest& req = co.requests.front();
    ResumeMode mode = req.mode;
    Value value = req.value;
    co.transfer = Value::undefined();
    if (mode == ResumeMode::Return) {
        Caught failed;
        try {
            value = awaitValue(rt, value);
        }
        FBX_CATCH_THROWN(failed)
        if (failed.threw) {
            mode = ResumeMode::Throw;
            value = failed.value;
        }
    }
    return {mode, value};
}

Value getMethod(Runtime& rt, const Value& o, const char16_t* name) {
    const Value m = rt.get(o, name);
    if (m.isNullish()) return Value::undefined();
    if (!rt.isCallable(m)) rt.throwError("TypeError", "an iterator's method isn't a function");
    return m;
}

}  // namespace

// ── The runtime's side ──

Object* Runtime::newPromise() { return allocate<Object>(promise_prototype, ObjectClass::Promise); }

void Runtime::resolvePromise(Object* p, const Value& x) {
    if (p->promise_state != kPending) return;
    if (x.isObject() && x.asObject() == p) {
        rejectPromise(p, Value::object(newError("TypeError", "a promise can't be resolved with itself")));
        return;
    }
    if (!x.isObject()) {
        fulfill(*this, p, x);
        return;
    }
    Caught failed;
    Value then;
    try {
        then = get(x, u"then");
    }
    FBX_CATCH_THROWN(failed)
    if (failed.threw) {
        rejectPromise(p, failed.value);
        return;
    }
    if (!isCallable(then)) {
        fulfill(*this, p, x);
        return;
    }
    enqueueJob(Job{kThenableJob, Value::object(p), x, then});
}

void Runtime::rejectPromise(Object* p, const Value& reason) {
    if (p->promise_state != kPending) return;
    p->promise_state = kRejected;
    p->primitive = reason;
    if (!p->promise_handled) pending_rejections.push_back(p);
    triggerReactions(*this, p, kRejected, reason);
}

void Runtime::runJobs() {
    // Only at a checkpoint: never while script is running (a nested event dispatch).
    if (call_depth > 0 || current_co) return;
    size_t ran = 0;
    while (!jobs_.empty()) {
        Job j = std::move(jobs_.front());
        jobs_.pop_front();
        if (++ran > 10'000'000) throw Terminated{"too many microtasks"};
        Caught failed;
        try {
            switch (j.kind) {
                case kReactionJob: {
                    Object* r = j.a.asObject();
                    const uint8_t state = static_cast<uint8_t>(j.b.asNumber());
                    const Value handler = state == kFulfilled ? r->slots[3] : r->slots[4];
                    Value result = j.c;
                    bool ok = state == kFulfilled;
                    if (!handler.isUndefined()) {
                        Caught threw;
                        try {
                            result = call(handler, Value::undefined(), std::span<const Value>(&j.c, 1));
                            ok = true;
                        }
                        FBX_CATCH_THROWN(threw)
                        if (threw.threw) {
                            result = threw.value;
                            ok = false;
                        }
                    }
                    settle(*this, Capability{r->slots[0], r->slots[1], r->slots[2]}, ok, result);
                    break;
                }
                case kThenableJob: {
                    auto [res, rej] = resolvingFunctions(*this, j.a.asObject());
                    const Value fns[2] = {Value::object(res), Value::object(rej)};
                    Caught threw;
                    try {
                        call(j.c, j.b, fns);
                    }
                    FBX_CATCH_THROWN(threw)
                    if (threw.threw) call(fns[1], Value::undefined(), std::span<const Value>(&threw.value, 1));
                    break;
                }
                default: call(j.a, Value::undefined(), {}); break;
            }
        }
        FBX_CATCH_THROWN(failed)
        if (failed.threw) {
            std::string message = "Uncaught exception";
            try {
                message = "Uncaught " + describe(*this, failed.value);
            } catch (const Thrown&) {
            }
            reportError(message);
        }
    }
    // Rejections still unhandled now that everything ran.
    std::vector<Object*> rejected;
    rejected.swap(pending_rejections);
    for (Object* p : rejected) {
        if (p->promise_handled) continue;
        std::string message = "Uncaught (in promise)";
        try {
            message = "Uncaught (in promise) " + describe(*this, p->primitive);
        } catch (const Thrown&) {
        }
        reportError(message);
    }
}

Value rejectedPromise(Runtime& rt, const Value& reason) {
    Object* p = rt.newPromise();
    rt.rejectPromise(p, reason);
    return Value::object(p);
}

// ── Coroutine-run functions, the interpreter's side ──

Value startCoroutineFunction(Runtime& rt, Object* f, Env* env) {
    FunctionInfo* info = f->code->info;
    if (info->is_generator) {
        const Value proto = rt.get(Value::object(f), u"prototype");
        Object* fallback = info->is_async ? rt.async_generator_prototype : rt.generator_prototype;
        Object* g = rt.allocate<Object>(proto.isObject() ? proto.asObject() : fallback, info->is_async ? ObjectClass::AsyncGenerator : ObjectClass::Generator);
        g->co = std::make_shared<Coroutine>(rt, info->is_async ? CoKind::AsyncGenerator : CoKind::Generator);
        g->co->function = f;
        g->co->env = env;
        g->co->holder = g;
        return Value::object(g);
    }
    // An async function: its body runs now, until its first await.
    Object* holder = internalObject(rt);
    holder->co = std::make_shared<Coroutine>(rt, CoKind::AsyncFunction);
    Coroutine& co = *holder->co;
    co.function = f;
    co.env = env;
    co.holder = holder;
    co.promise = rt.newPromise();
    Object* promise = co.promise;
    resumeCoroutine(co);
    continueAsync(rt, holder);
    return Value::object(promise);
}

void runCoroutineBody(Coroutine& co) {
    if (co.kind == CoKind::Module) {
        runModuleBody(co.rt, co);
        co.transfer = Value::undefined();
        return;
    }
    co.transfer = runFunctionBody(co.rt, co.function, co.env);
}

Value generatorYield(Runtime& rt, const Value& v) {
    Coroutine* co = rt.current_co;
    if (!co) rt.throwError("SyntaxError", "yield outside a generator");
    if (co->kind == CoKind::AsyncGenerator) {
        const Value awaited = awaitValue(rt, v);
        auto [mode, value] = asyncGenYieldRaw(rt, *co, awaited);
        co->mode = mode;
        co->transfer = value;
        return takeResumption(*co);
    }
    co->transfer = v;
    co->transfer_is_result = false;
    suspendCoroutine(*co);
    return takeResumption(*co);
}

Value awaitValue(Runtime& rt, const Value& v) {
    Coroutine* co = rt.current_co;
    if (!co || co->kind == CoKind::Generator) rt.throwError("SyntaxError", "await is only valid in async functions and modules");
    const Value p = promiseResolve(rt, Value::object(rt.promise_constructor), v);
    if (!co->on_fulfilled) {
        co->on_fulfilled = closure(rt, u"", 1, {Value::object(co->holder)}, [](Runtime& rt, Object* self, const Value&, Args a) {
            return resumeFromAwait(rt, self, ResumeMode::Next, arg(a, 0));
        });
        co->on_rejected = closure(rt, u"", 1, {Value::object(co->holder)}, [](Runtime& rt, Object* self, const Value&, Args a) {
            return resumeFromAwait(rt, self, ResumeMode::Throw, arg(a, 0));
        });
    }
    performThen(rt, p.asObject(), Value::object(co->on_fulfilled), Value::object(co->on_rejected), nullptr);
    co->awaiting = true;
    suspendCoroutine(*co);
    co->awaiting = false;
    const Value got = co->transfer;
    co->transfer = Value::undefined();
    if (co->mode == ResumeMode::Throw) throw Thrown{got};
    return got;
}

// yield*: hands next, throw and return through to an inner iterator until it's done.
Value generatorDelegate(Runtime& rt, const Value& iterable) {
    Coroutine* cop = rt.current_co;
    if (!cop) rt.throwError("SyntaxError", "yield outside a generator");
    Coroutine& co = *cop;
    const bool async = co.kind == CoKind::AsyncGenerator;
    Value iterator, next;
    bool sync_wrapped = false;
    {
        Value m;
        if (async) {
            m = iterable.isNullish() ? Value::undefined() : rt.get(iterable, PropertyKey(rt.sym_async_iterator));
            if (m.isNullish()) sync_wrapped = true;
        }
        if (!async || sync_wrapped) m = iterable.isNullish() ? Value::undefined() : rt.get(iterable, PropertyKey(rt.sym_iterator));
        if (!rt.isCallable(m)) rt.throwError("TypeError", "yield* needs something iterable");
        iterator = rt.call(m, iterable, {});
        if (!iterator.isObject()) rt.throwError("TypeError", "an iterator must be an object");
        next = rt.get(iterator, u"next");
    }
    // A step's result, awaited where an async generator awaits it.
    const auto settleResult = [&](Value r) {
        if (async && !sync_wrapped) r = awaitValue(rt, r);
        if (!r.isObject()) rt.throwError("TypeError", "an iterator result must be an object");
        return r;
    };
    ResumeMode mode = ResumeMode::Next;
    Value received;
    while (true) {
        Value r;
        if (mode == ResumeMode::Next) {
            r = settleResult(rt.call(next, iterator, std::span<const Value>(&received, 1)));
        } else if (mode == ResumeMode::Throw) {
            const Value throw_m = getMethod(rt, iterator, u"throw");
            if (throw_m.isUndefined()) {
                // No throw(): close it, then it's a protocol error.
                const Value ret = getMethod(rt, iterator, u"return");
                if (!ret.isUndefined()) {
                    Value rr = rt.call(ret, iterator, {});
                    if (async && !sync_wrapped) rr = awaitValue(rt, rr);
                    if (!rr.isObject()) rt.throwError("TypeError", "an iterator's return() must give an object");
                }
                rt.throwError("TypeError", "the iterator yield* is delegating to has no throw method");
            }
            r = settleResult(rt.call(throw_m, iterator, std::span<const Value>(&received, 1)));
        } else {
            const Value ret = getMethod(rt, iterator, u"return");
            if (ret.isUndefined()) {
                Value v = received;
                if (async) v = awaitValue(rt, v);
                throw GeneratorReturn{v};
            }
            r = settleResult(rt.call(ret, iterator, std::span<const Value>(&received, 1)));
            if (rt.toBoolean(rt.get(r, u"done"))) {
                Value v = rt.get(r, u"value");
                if (async) v = awaitValue(rt, v);
                throw GeneratorReturn{v};
            }
        }
        if (mode != ResumeMode::Return && rt.toBoolean(rt.get(r, u"done"))) {
            Value v = rt.get(r, u"value");
            if (sync_wrapped) v = awaitValue(rt, v);
            return v;
        }
        if (async) {
            Value v = rt.get(r, u"value");
            if (sync_wrapped) v = awaitValue(rt, v);
            auto [m, value] = asyncGenYieldRaw(rt, co, v);
            mode = m;
            received = value;
        } else {
            co.transfer = r;
            co.transfer_is_result = true;
            suspendCoroutine(co);
            co.transfer_is_result = false;
            mode = co.mode;
            received = co.transfer;
            co.transfer = Value::undefined();
        }
    }
}

// ── Installing ──

void Runtime::installPromise() {
    const auto fn = [&](Object* o, const char16_t* name, int length, NativeFunction f) { method(o, name, length, std::move(f)); };

    // AggregateError, which Promise.any rejects with.
    {
        Object* proto = allocate<Object>(error_prototype);
        Object* c = newFunction(u"AggregateError", 2, [](Runtime& rt, const Value&, Args a) {
            Object* nt = rt.construct_target;
            Object* proto = rt.error_prototypes["AggregateError"];
            if (nt) {
                const Value p = rt.get(Value::object(nt), u"prototype");
                if (p.isObject()) proto = p.asObject();
            }
            Object* e = rt.allocate<Object>(proto, ObjectClass::Error);
            if (!arg(a, 1).isUndefined()) rt.put(e, u"message", Value::string(rt.toString(arg(a, 1))));
            const Value opts = arg(a, 2);
            if (opts.isObject() && rt.has(opts.asObject(), u"cause")) rt.put(e, u"cause", rt.get(opts, u"cause"));
            rt.put(e, u"errors", Value::object(rt.newArray(rt.iterateToList(arg(a, 0)))));
            return Value::object(e);
        }, true);
        const Value err = get(Value::object(global_), u"Error");
        if (err.isObject()) c->prototype = err.asObject();
        put(c, u"prototype", Value::object(proto), 0);
        put(proto, u"constructor", Value::object(c));
        put(proto, u"name", Value::string(u"AggregateError"));
        put(proto, u"message", Value::string(u""));
        error_prototypes["AggregateError"] = proto;
        put(global_, u"AggregateError", Value::object(c));
    }

    promise_prototype = newObject();
    Object* ctor = newFunction(u"Promise", 1, [](Runtime& rt, const Value&, Args a) -> Value {
        Object* nt = rt.construct_target;
        if (!nt) rt.throwError("TypeError", "Promise must be called with new");
        const Value executor = arg(a, 0);
        if (!rt.isCallable(executor)) rt.throwError("TypeError", "Promise needs an executor function");
        const Value proto = rt.get(Value::object(nt), u"prototype");
        Object* p = rt.allocate<Object>(proto.isObject() ? proto.asObject() : rt.promise_prototype, ObjectClass::Promise);
        auto [res, rej] = resolvingFunctions(rt, p);
        const Value fns[2] = {Value::object(res), Value::object(rej)};
        Caught failed;
        try {
            rt.call(executor, Value::undefined(), fns);
        }
        FBX_CATCH_THROWN(failed)
        if (failed.threw) rt.call(fns[1], Value::undefined(), std::span<const Value>(&failed.value, 1));
        return Value::object(p);
    }, true);
    promise_constructor = ctor;
    put(ctor, u"prototype", Value::object(promise_prototype), 0);
    put(promise_prototype, u"constructor", Value::object(ctor));
    put(promise_prototype, PropertyKey(sym_to_string_tag), Value::string(u"Promise"), kConfigurable);
    put(global_, u"Promise", Value::object(ctor));
    {
        Object* g = newFunction(u"get [Symbol.species]", 0, [](Runtime&, const Value& t, Args) { return t; });
        ctor->define(PropertyKey(sym_species), {Value::undefined(), g, nullptr, kConfigurable | kAccessor});
    }

    fn(promise_prototype, u"then", 2, [](Runtime& rt, const Value& t, Args a) {
        thisPromise(rt, t, "then");
        return thenOf(rt, t, arg(a, 0), arg(a, 1));
    });
    promise_then = promise_prototype->own(u"then")->value.asObject();
    fn(promise_prototype, u"catch", 1, [](Runtime& rt, const Value& t, Args a) {
        return invoke(rt, t, u"then", {Value::undefined(), arg(a, 0)});
    });
    fn(promise_prototype, u"finally", 1, [](Runtime& rt, const Value& t, Args a) {
        if (!t.isObject()) rt.throwError("TypeError", "Promise.prototype.finally called on something that isn't an object");
        const Value c = speciesConstructor(rt, t, rt.promise_constructor);
        const Value on_finally = arg(a, 0);
        if (!rt.isCallable(on_finally)) return invoke(rt, t, u"then", {on_finally, on_finally});
        Object* then_finally = closure(rt, u"", 1, {on_finally, c}, [](Runtime& rt, Object* self, const Value&, Args a) {
            const Value result = rt.call(self->slots[0], Value::undefined(), {});
            const Value p = promiseResolve(rt, self->slots[1], result);
            Object* thunk = closure(rt, u"", 0, {arg(a, 0)}, [](Runtime&, Object* self, const Value&, Args) { return self->slots[0]; });
            return invoke(rt, p, u"then", {Value::object(thunk)});
        });
        Object* catch_finally = closure(rt, u"", 1, {on_finally, c}, [](Runtime& rt, Object* self, const Value&, Args a) {
            const Value result = rt.call(self->slots[0], Value::undefined(), {});
            const Value p = promiseResolve(rt, self->slots[1], result);
            Object* thrower = closure(rt, u"", 0, {arg(a, 0)}, [](Runtime&, Object* self, const Value&, Args) -> Value { throw Thrown{self->slots[0]}; });
            return invoke(rt, p, u"then", {Value::object(thrower)});
        });
        return invoke(rt, t, u"then", {Value::object(then_finally), Value::object(catch_finally)});
    });

    fn(ctor, u"resolve", 1, [](Runtime& rt, const Value& t, Args a) {
        if (!t.isObject()) rt.throwError("TypeError", "Promise.resolve called on something that isn't an object");
        return promiseResolve(rt, t, arg(a, 0));
    });
    fn(ctor, u"reject", 1, [](Runtime& rt, const Value& t, Args a) {
        const Capability cap = newCapability(rt, t, false);
        settle(rt, cap, false, arg(a, 0));
        return cap.promise;
    });
    fn(ctor, u"withResolvers", 0, [](Runtime& rt, const Value& t, Args) {
        const Capability cap = newCapability(rt, t, true);
        Object* o = rt.newObject();
        rt.put(o, u"promise", cap.promise, kDefaultFlags);
        rt.put(o, u"resolve", cap.resolve, kDefaultFlags);
        rt.put(o, u"reject", cap.reject, kDefaultFlags);
        return Value::object(o);
    });
    fn(ctor, u"try", 1, [](Runtime& rt, const Value& t, Args a) {
        if (!t.isObject()) rt.throwError("TypeError", "Promise.try called on something that isn't an object");
        const Capability cap = newCapability(rt, t, false);
        Caught failed;
        Value result;
        try {
            result = rt.call(arg(a, 0), Value::undefined(), a.size() > 1 ? a.subspan(1) : Args{});
        }
        FBX_CATCH_THROWN(failed)
        settle(rt, cap, !failed.threw, failed.threw ? failed.value : result);
        return cap.promise;
    });
    fn(ctor, u"all", 1, [](Runtime& rt, const Value& t, Args a) { return combinator(rt, t, arg(a, 0), Combinator::All); });
    fn(ctor, u"allSettled", 1, [](Runtime& rt, const Value& t, Args a) { return combinator(rt, t, arg(a, 0), Combinator::AllSettled); });
    fn(ctor, u"any", 1, [](Runtime& rt, const Value& t, Args a) { return combinator(rt, t, arg(a, 0), Combinator::Any); });
    fn(ctor, u"race", 1, [](Runtime& rt, const Value& t, Args a) { return combinator(rt, t, arg(a, 0), Combinator::Race); });
}

void Runtime::installGenerators() {
    const auto tag = [&](Object* o, const char16_t* name) { put(o, PropertyKey(sym_to_string_tag), Value::string(name), kConfigurable); };
    const auto link = [&](Object* fn_proto, Object* proto) {
        // GeneratorFunction.prototype.prototype and its constructor, both read-only.
        put(fn_proto, u"prototype", Value::object(proto), kConfigurable);
        put(proto, u"constructor", Value::object(fn_proto), kConfigurable);
    };

    generator_prototype = newObject(iterator_prototype);
    method(generator_prototype, u"next", 1, [](Runtime& rt, const Value& t, Args a) { return generatorResume(rt, t, ResumeMode::Next, arg(a, 0)); });
    method(generator_prototype, u"return", 1, [](Runtime& rt, const Value& t, Args a) { return generatorResume(rt, t, ResumeMode::Return, arg(a, 0)); });
    method(generator_prototype, u"throw", 1, [](Runtime& rt, const Value& t, Args a) { return generatorResume(rt, t, ResumeMode::Throw, arg(a, 0)); });
    tag(generator_prototype, u"Generator");
    generator_function_prototype = newObject(function_prototype);
    tag(generator_function_prototype, u"GeneratorFunction");
    link(generator_function_prototype, generator_prototype);

    async_iterator_prototype = newObject();
    async_iterator_prototype->define(PropertyKey(sym_async_iterator),
                                     {Value::object(newFunction(u"[Symbol.asyncIterator]", 0, [](Runtime&, const Value& t, Args) { return t; })),
                                      nullptr, nullptr, kWritable | kConfigurable});
    async_generator_prototype = newObject(async_iterator_prototype);
    method(async_generator_prototype, u"next", 1, [](Runtime& rt, const Value& t, Args a) { return asyncGenEnqueue(rt, t, ResumeMode::Next, arg(a, 0)); });
    method(async_generator_prototype, u"return", 1, [](Runtime& rt, const Value& t, Args a) { return asyncGenEnqueue(rt, t, ResumeMode::Return, arg(a, 0)); });
    method(async_generator_prototype, u"throw", 1, [](Runtime& rt, const Value& t, Args a) { return asyncGenEnqueue(rt, t, ResumeMode::Throw, arg(a, 0)); });
    tag(async_generator_prototype, u"AsyncGenerator");
    async_generator_function_prototype = newObject(function_prototype);
    tag(async_generator_function_prototype, u"AsyncGeneratorFunction");
    link(async_generator_function_prototype, async_generator_prototype);

    async_function_prototype = newObject(function_prototype);
    tag(async_function_prototype, u"AsyncFunction");

    // Their constructors, which code finds through a function's prototype.
    const Value function_ctor = get(Value::object(global_), u"Function");
    const auto dynamicCtor = [&](const char16_t* name, Object* fn_proto) {
        const char* kind = name == std::u16string_view(u"GeneratorFunction") ? "function*"
                           : name == std::u16string_view(u"AsyncGeneratorFunction") ? "async function*" : "async function";
        Object* c = newFunction(name, 1, [kind](Runtime& rt, const Value&, Args a) { return dynamicFunction(rt, a, kind); }, true);
        if (function_ctor.isObject()) c->prototype = function_ctor.asObject();
        put(c, u"prototype", Value::object(fn_proto), 0);
        put(fn_proto, u"constructor", Value::object(c), kConfigurable);
    };
    dynamicCtor(u"GeneratorFunction", generator_function_prototype);
    dynamicCtor(u"AsyncGeneratorFunction", async_generator_function_prototype);
    dynamicCtor(u"AsyncFunction", async_function_prototype);
}

}  // namespace fbx::xweb::js
