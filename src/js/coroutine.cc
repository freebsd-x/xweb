// Coroutines: separate machine stacks and a hand-written switch between them.

#include "coroutine.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <exception>
#include <functional>
#include <new>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FBX_JS_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define FBX_JS_ASAN 1
#endif

#ifdef FBX_JS_ASAN
extern "C" {
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old, size_t* size_old);
void __asan_unpoison_memory_region(void const volatile* addr, size_t size);
}
#endif

#if !defined(__x86_64__)
#error "xweb's coroutines have a context switch for x86-64 only so far"
#endif

// fbx_js_switch(save, to): saves the callee-saved registers and the floating-point
// control words on this stack, stores the stack pointer in *save, and resumes the
// stack `to` the same way. A fresh stack "returns" into the trampoline, which calls
// r13(r12) and never comes back.
extern "C" void fbx_js_switch(void** save, void* to);
extern "C" void fbx_js_trampoline();
asm(R"(
    .text
    .globl fbx_js_switch
    .type fbx_js_switch,@function
    .p2align 4
fbx_js_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $16, %rsp
    stmxcsr (%rsp)
    fnstcw 4(%rsp)
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    ldmxcsr (%rsp)
    fldcw 4(%rsp)
    addq $16, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
    .size fbx_js_switch,.-fbx_js_switch

    .globl fbx_js_trampoline
    .type fbx_js_trampoline,@function
    .p2align 4
fbx_js_trampoline:
    .cfi_startproc
    .cfi_undefined rip
    movq %r12, %rdi
    callq *%r13
    ud2
    .cfi_endproc
    .size fbx_js_trampoline,.-fbx_js_trampoline
)");

namespace fbx::xweb::js {

namespace {

#ifdef FBX_JS_ASAN
constexpr size_t kStackSize = 32u << 20;
constexpr size_t kStackReserve = 1u << 20;   // left for natives below the recursion check
constexpr size_t kEngineStackSize = 60u << 20;   // under ASan's 64 MB: deeper, it stops unpoisoning when exceptions unwind
#else
constexpr size_t kStackSize = 16u << 20;   // virtual: only the pages a body touches are committed
constexpr size_t kStackReserve = 256u << 10;
constexpr size_t kEngineStackSize = 512u << 20;
#endif
constexpr size_t kPoolMax = 32;
constexpr size_t kMaxLiveStacks = 50'000;   // 16 MB of address space each, little of it committed
thread_local size_t live_stacks = 0;   // stacks handed out and not given back

size_t pageSize() {
    static const size_t p = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return p;
}

// Stacks are kept for reuse: async code makes and ends coroutines all the time.
struct StackPool {
    std::vector<char*> free;
    ~StackPool() {
        for (char* m : free) munmap(m, kStackSize);
    }
};
thread_local StackPool pool;

char* takeStack() {
    ++live_stacks;
    if (!pool.free.empty()) {
        char* m = pool.free.back();
        pool.free.pop_back();
#ifdef FBX_JS_ASAN
        __asan_unpoison_memory_region(m + pageSize(), kStackSize - pageSize());
#endif
        return m;
    }
    void* m = mmap(nullptr, kStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) {
        --live_stacks;
        return nullptr;
    }
    // A guard page at the bottom: running off the end faults instead of writing over memory.
    mprotect(m, pageSize(), PROT_NONE);
    return static_cast<char*>(m);
}

void giveStack(char* m) {
    --live_stacks;
    if (pool.free.size() >= kPoolMax) {
        munmap(m, kStackSize);
        return;
    }
    // The pages it touched go back to the system; the mapping stays.
    madvise(m + pageSize(), kStackSize - pageSize(), MADV_FREE);
    pool.free.push_back(m);
}

[[noreturn]] void entry(Coroutine* co);

void swapState(Runtime& rt, Coroutine& co) {
    std::swap(rt.env_stack, co.env_stack);
    std::swap(rt.temp_lists, co.temp_lists);
}

}  // namespace

Coroutine::Coroutine(Runtime& r, CoKind k) : rt(r), kind(k) {}

Coroutine::~Coroutine() {
    // A suspended one should have been unwound (the collector and ~Runtime do); if not,
    // its frames' destructors never run, which leaks but is safe.
    if (mem) giveStack(mem);
}

void resumeCoroutine(Coroutine& co) {
    Runtime& rt = co.rt;
    if (co.state == CoState::Fresh) {
        // A stack is real work and memory: charged as such, and only so many at once.
        rt.steps(100);
        if (live_stacks >= kMaxLiveStacks) rt.throwError("RangeError", "too many suspended generators and async functions");
        co.mem = takeStack();
        if (!co.mem) throw Terminated{"out of memory for a coroutine's stack"};
        co.mem_size = kStackSize;
        co.stack_limit = co.mem + pageSize() + kStackReserve;
        // The first switch "returns" into the trampoline with r12 = co, r13 = entry.
        char* top = co.mem + kStackSize;
        uintptr_t after_ret = (reinterpret_cast<uintptr_t>(top) - 64) & ~uintptr_t{15};
        auto* frame = reinterpret_cast<uint64_t*>(after_ret - 72);
        std::memset(frame, 0, 72);
        const uint32_t mxcsr = 0x1F80;
        const uint16_t fpucw = 0x037F;
        std::memcpy(reinterpret_cast<char*>(frame), &mxcsr, 4);
        std::memcpy(reinterpret_cast<char*>(frame) + 4, &fpucw, 2);
        frame[2] = 0;                                              // r15
        frame[3] = 0;                                              // r14
        frame[4] = reinterpret_cast<uint64_t>(&entry);             // r13
        frame[5] = reinterpret_cast<uint64_t>(&co);                // r12
        frame[6] = 0;                                              // rbx
        frame[7] = 0;                                              // rbp
        frame[8] = reinterpret_cast<uint64_t>(&fbx_js_trampoline); // return address
        co.sp = frame;
    }
    co.state = CoState::Running;
    co.resumer = rt.current_co;
    rt.current_co = &co;
    swapState(rt, co);
    const size_t base_calls = rt.call_depth, base_evals = rt.eval_depth;
    rt.call_depth += co.call_depth;
    rt.eval_depth += co.eval_depth;
#ifdef FBX_JS_ASAN
    void* fake = nullptr;
    __sanitizer_start_switch_fiber(&fake, co.mem + pageSize(), kStackSize - pageSize());
#endif
    fbx_js_switch(&co.caller_sp, co.sp);
#ifdef FBX_JS_ASAN
    __sanitizer_finish_switch_fiber(fake, nullptr, nullptr);
#endif
    co.call_depth = rt.call_depth - base_calls;
    co.eval_depth = rt.eval_depth - base_evals;
    rt.call_depth = base_calls;
    rt.eval_depth = base_evals;
    swapState(rt, co);
    rt.current_co = co.resumer;
    co.resumer = nullptr;
    if (co.state == CoState::Done) {
        // Its frames are gone: the stack can serve the next one.
        giveStack(co.mem);
        co.mem = nullptr;
        co.sp = nullptr;
        co.env_stack.clear();
        co.temp_lists.clear();
    } else {
        co.state = CoState::Suspended;
    }
}

void suspendCoroutine(Coroutine& co) {
#ifdef FBX_JS_ASAN
    __sanitizer_start_switch_fiber(&co.asan_fake, co.asan_caller_bottom, co.asan_caller_size);
#endif
    fbx_js_switch(&co.sp, co.caller_sp);
#ifdef FBX_JS_ASAN
    __sanitizer_finish_switch_fiber(co.asan_fake, &co.asan_caller_bottom, &co.asan_caller_size);
#endif
    if (co.mode == ResumeMode::Unwind) throw CoroutineUnwind{};
}

void unwindCoroutine(Coroutine& co) {
    if (co.state == CoState::Fresh) {
        co.state = CoState::Done;
        return;
    }
    if (co.state != CoState::Suspended) return;
    co.mode = ResumeMode::Unwind;
    co.transfer = Value::undefined();
    resumeCoroutine(co);
}

// ── The engine's stack ──

namespace {

struct EngineStack {
    char* mem = nullptr;
    void* caller_sp = nullptr;
    const std::function<void()>* fn = nullptr;
    std::exception_ptr error;
    const void* asan_caller_bottom = nullptr;
    size_t asan_caller_size = 0;
};
thread_local EngineStack engine;

[[noreturn]] void engineEntry(EngineStack* e) {
#ifdef FBX_JS_ASAN
    __sanitizer_finish_switch_fiber(nullptr, &e->asan_caller_bottom, &e->asan_caller_size);
#endif
    try {
        (*e->fn)();
    } catch (...) {
        e->error = std::current_exception();
    }
#ifdef FBX_JS_ASAN
    __sanitizer_start_switch_fiber(nullptr, e->asan_caller_bottom, e->asan_caller_size);
#endif
    void* dead = nullptr;
    fbx_js_switch(&dead, e->caller_sp);
    __builtin_trap();
}

}  // namespace

bool onEngineStack() noexcept {
    const char* sp = static_cast<const char*>(__builtin_frame_address(0));
    return engine.mem && sp > engine.mem && sp < engine.mem + kEngineStackSize;
}

const char* engineStackLimit() noexcept { return engine.mem ? engine.mem + pageSize() + kStackReserve * 4 : nullptr; }

void runOnEngineStack(const std::function<void()>& fn) {
    if (!engine.mem) {
        void* m = mmap(nullptr, kEngineStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) {
            fn();   // no room for it: run where we are, with the old limits
            return;
        }
        mprotect(m, pageSize(), PROT_NONE);
        engine.mem = static_cast<char*>(m);
    }
    // A fresh frame at the top each time: nothing stays on it between runs.
    char* top = engine.mem + kEngineStackSize;
    uintptr_t after_ret = (reinterpret_cast<uintptr_t>(top) - 64) & ~uintptr_t{15};
    auto* frame = reinterpret_cast<uint64_t*>(after_ret - 72);
    std::memset(frame, 0, 72);
    const uint32_t mxcsr = 0x1F80;
    const uint16_t fpucw = 0x037F;
    std::memcpy(reinterpret_cast<char*>(frame), &mxcsr, 4);
    std::memcpy(reinterpret_cast<char*>(frame) + 4, &fpucw, 2);
    frame[4] = reinterpret_cast<uint64_t>(&engineEntry);
    frame[5] = reinterpret_cast<uint64_t>(&engine);
    frame[8] = reinterpret_cast<uint64_t>(&fbx_js_trampoline);
    const std::function<void()>* saved_fn = engine.fn;
    void* saved_caller = engine.caller_sp;
    engine.fn = &fn;
    engine.error = nullptr;
#ifdef FBX_JS_ASAN
    void* fake = nullptr;
    __sanitizer_start_switch_fiber(&fake, engine.mem + pageSize(), kEngineStackSize - pageSize());
#endif
    fbx_js_switch(&engine.caller_sp, frame);
#ifdef FBX_JS_ASAN
    __sanitizer_finish_switch_fiber(fake, nullptr, nullptr);
#endif
    std::exception_ptr error = engine.error;
    engine.error = nullptr;
    engine.fn = saved_fn;
    engine.caller_sp = saved_caller;
    if (error) std::rethrow_exception(error);
}

__attribute__((no_sanitize("address"))) void scanCoroutineStack(const Coroutine& co, const std::function<void(uintptr_t)>& word) {
    if (co.state != CoState::Suspended || !co.sp || !co.mem) return;
    const auto* p = static_cast<const uintptr_t*>(co.sp);
    const auto* end = reinterpret_cast<const uintptr_t*>(co.mem + co.mem_size);
    for (; p < end; ++p) word(*p);
}

namespace {

[[noreturn]] void entry(Coroutine* co) {
#ifdef FBX_JS_ASAN
    __sanitizer_finish_switch_fiber(nullptr, &co->asan_caller_bottom, &co->asan_caller_size);
#endif
    try {
        if (co->mode == ResumeMode::Unwind) throw CoroutineUnwind{};
        runCoroutineBody(*co);
    } catch (const Thrown& t) {
        co->threw = true;
        co->transfer = t.value;
    } catch (const Terminated& t) {
        co->terminated = t;
    } catch (const CoroutineUnwind&) {
    } catch (const GeneratorReturn& r) {
        co->transfer = r.value;
    } catch (const std::bad_alloc&) {
        co->terminated = Terminated{"out of memory"};
    } catch (...) {
        co->terminated = Terminated{"internal error in a coroutine"};
    }
    co->state = CoState::Done;
#ifdef FBX_JS_ASAN
    __sanitizer_start_switch_fiber(nullptr, co->asan_caller_bottom, co->asan_caller_size);
#endif
    void* dead = nullptr;
    fbx_js_switch(&dead, co->caller_sp);
    __builtin_trap();
}

}  // namespace

}  // namespace fbx::xweb::js
