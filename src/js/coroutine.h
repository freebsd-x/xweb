#pragma once

// Coroutines: a generator's or async function's body runs on a machine stack of its
// own, so `yield` and `await` can leave it mid-expression and come back later while
// the interpreter stays a plain tree walker. Internal to the engine.
//
// A suspended coroutine's frames hold values in C++ locals, which the collector finds
// by scanning its stack word by word for pointers to cells (conservatively), plus the
// value lists the interpreter registers (TempRoots) for what lives in heap buffers.
// A coroutine nothing reachable refers to any more is unwound (CoroutineUnwind runs
// its destructors, no JavaScript) before its cells are freed.

#include <deque>
#include <optional>
#include <vector>

#include "internal.h"

namespace fbx::xweb::js {

// Thrown at a suspension point to unwind a coroutine that's being dropped.
struct CoroutineUnwind {};
// generator.return(v) at a yield: a return from there, through any finally blocks.
struct GeneratorReturn {
    Value value;
};

enum class CoKind : uint8_t { Generator, AsyncFunction, AsyncGenerator, Module };
enum class CoState : uint8_t { Fresh, Running, Suspended, Done };
enum class ResumeMode : uint8_t { Next, Throw, Return, Unwind };

struct AsyncGenRequest {
    ResumeMode mode;
    Value value;
    Object* promise;
};

struct Coroutine {
    Coroutine(Runtime& rt, CoKind kind);
    ~Coroutine();
    Coroutine(const Coroutine&) = delete;
    Coroutine& operator=(const Coroutine&) = delete;

    Runtime& rt;
    CoKind kind;
    CoState state = CoState::Fresh;

    // What it runs: a function's body in the environment its call prepared, or a module.
    Object* function = nullptr;
    Env* env = nullptr;
    void* module = nullptr;            // a module record (modules.cc), for CoKind::Module
    Object* promise = nullptr;         // an async function's or module's result
    Object* holder = nullptr;          // the object that owns it (a generator, an async call's state)
    Object* on_fulfilled = nullptr;    // its await continuations, made once
    Object* on_rejected = nullptr;
    bool awaiting_return = false;      // a finished async generator settling a return(v)
    std::deque<AsyncGenRequest> requests;   // an async generator's next/throw/return calls

    // The hand-over between the coroutine and whoever resumes it.
    ResumeMode mode = ResumeMode::Next;
    Value transfer;                    // in: what next(v) sent; out: what it yielded or returned
    bool transfer_is_result = false;   // yield*: an inner iterator result, passed on as it is
    bool awaiting = false;             // suspended at an await, not a yield
    bool threw = false;                // ended by throwing `transfer`
    std::optional<Terminated> terminated;

    // The runtime's per-coroutine state, swapped in while it runs.
    std::vector<Env*> env_stack;
    std::vector<std::vector<Value>*> temp_lists;
    size_t call_depth = 0, eval_depth = 0;
    Coroutine* resumer = nullptr;

    // The machine stack.
    char* mem = nullptr;
    size_t mem_size = 0;
    char* stack_limit = nullptr;       // deeper than this is "too much recursion"
    void* sp = nullptr;                // its saved stack pointer while suspended
    void* caller_sp = nullptr;         // the resumer's, while it runs
    void* asan_fake = nullptr;
    const void* asan_caller_bottom = nullptr;
    size_t asan_caller_size = 0;
};

// Runs the coroutine until it suspends or ends. It must be Fresh or Suspended.
void resumeCoroutine(Coroutine& co);
// From inside the running coroutine: back to whoever resumed it. Returns when it's
// resumed again; then co.mode says how (an Unwind throws CoroutineUnwind here).
void suspendCoroutine(Coroutine& co);
// Unwinds a suspended coroutine's frames without running any JavaScript.
void unwindCoroutine(Coroutine& co);
// The stacks a running script is using (the current one and each resumer's), word by
// word, for a collection mid-script. False if they aren't as expected: don't collect.
bool scanActiveStacks(const Runtime& rt, const std::function<void(uintptr_t)>& word);
// The live part of a suspended coroutine's stack, for the collector.
void scanCoroutineStack(const Coroutine& co, const std::function<void(uintptr_t)>& word);

// The engine's own stack: JavaScript runs on a large one (only the pages it touches are
// committed), so deep recursion in real pages (React renders) has room. Whoever calls
// into the engine from outside gets switched onto it; calls from inside don't switch.
bool onEngineStack() noexcept;
// Deeper than this, on the engine's stack, is "too much recursion".
const char* engineStackLimit() noexcept;
void runOnEngineStack(const std::function<void()>& fn);

// What a coroutine runs (async.cc).
void runCoroutineBody(Coroutine& co);

// Keeps a heap buffer of values visible to the collector while a coroutine that's
// using it is suspended.
struct TempRoots {
    Runtime& rt;
    TempRoots(Runtime& r, std::vector<Value>& v) : rt(r) { rt.temp_lists.push_back(&v); }
    ~TempRoots() { rt.temp_lists.pop_back(); }
    TempRoots(const TempRoots&) = delete;
    TempRoots& operator=(const TempRoots&) = delete;
};

}  // namespace fbx::xweb::js
