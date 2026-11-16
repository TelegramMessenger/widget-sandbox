#include "twidget/quickjs_context.h"

#include "steady_clock.h"

#ifdef _WIN32
#include <windows.h>
#if (defined(_M_IX86) || defined(_M_X64)) && !defined(_M_ARM64EC) // ARM64EC code defines _M_X64 too
#include <intrin.h>
#define TW_TSC 1
#endif
#else
#include <time.h>
#endif

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" {
#include "qjs_shim.h"
}

namespace twidget {

struct QuickJSTrampoline {
    static JSValue call(JSContext* ctx, JSValue /*this*/, int argc, JSValue* argv, int magic) {
        auto* self = static_cast<QuickJSContext*>(JS_GetContextOpaque(ctx));
        if (!self || magic < 0 || size_t(magic) >= self->functions_.size()) return qjs_undefined();
        // The engine's C frames under this one cannot be unwound: nothing thrown here or by a host function may leave it.
        try {
            std::vector<std::string> args;
            args.reserve(size_t(argc));
            for (int i = 0; i < argc; i++) {
                JSValue v = argv[i];
                if (qjs_is_string(v)) {
                    struct Copied {
                        JSContext* ctx;
                        const char* c;
                        ~Copied() { if (c) qjs_free_cstring(ctx, c); }
                    } copied{ctx, qjs_to_cstring(ctx, v)};
                    if (!copied.c) return JS_ThrowOutOfMemory(ctx);
                    args.emplace_back(copied.c);
                } else if (qjs_is_number(v)) {
                    double d = qjs_to_float64(ctx, v);
                    char buf[64];
                    if (d == std::floor(d) && std::fabs(d) < 1e15) snprintf(buf, sizeof buf, "%lld", (long long)d);
                    else snprintf(buf, sizeof buf, "%.17g", d);
                    args.emplace_back(buf);
                } else {
                    args.emplace_back();
                }
            }
            HostReturn r = self->functions_[size_t(magic)](args);
            switch (r.kind) {
            case HostReturn::Kind::Undefined: return qjs_undefined();
            case HostReturn::Kind::Null: return qjs_null();
            case HostReturn::Kind::String: return qjs_new_string_len(ctx, r.str.data(), r.str.size());
            case HostReturn::Kind::Number: return qjs_new_float64(ctx, r.num);
            case HostReturn::Kind::ArrayBuffer: return qjs_new_array_buffer_copy(ctx, r.bytes.data(), r.bytes.size());
            case HostReturn::Kind::RangeError: return JS_ThrowRangeError(ctx, "%s", r.str.c_str());
            }
            return qjs_undefined();
        } catch (const std::bad_alloc&) {
            return JS_ThrowOutOfMemory(ctx);
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "host function failed: %s", e.what());
        } catch (...) {
            return JS_ThrowInternalError(ctx, "host function failed");
        }
    }

    static int interrupt(JSContext* /*ctx*/, void* opaque) {
        auto* self = static_cast<QuickJSContext*>(opaque);
        if (!self) return 0;
        if (QuickJSContext::now() > self->deadline || (self->cpuDeadline != std::numeric_limits<double>::infinity() && QuickJSContext::threadCpuTime() > self->cpuDeadline)) {
            self->interrupted = true;
            return 1;
        }
        return 0;
    }
};

double QuickJSContext::now() { return detail::steadySeconds(); }

#ifdef _WIN32
namespace {

constexpr double kFileTimeTicksPerSecond = 1e7;

struct ThreadClockSource {
    bool cycles = false;
    double ticksPerSecond = kFileTimeTicksPerSecond;
};

struct ThreadClock {
    double seconds = 0.;
    uint64_t count = 0;
    bool counting = false;
};

#ifdef TW_TSC
constexpr unsigned kHypervisorPresent = 1u << 31;
constexpr unsigned kInvariantTsc = 1u << 8;
constexpr unsigned kCreatePartitions = 1u << 0;
constexpr unsigned kNestedHypervisor = 1u << 12;
constexpr double kMinTscRate = 1e8;
constexpr double kMaxTscRate = 1e11;

struct CpuidLeaf {
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
};

CpuidLeaf readCpuid(unsigned leaf) {
    int registers[4] = {};
    __cpuid(registers, int(leaf));
    return {unsigned(registers[0]), unsigned(registers[1]), unsigned(registers[2]), unsigned(registers[3])};
}

bool onArm64Windows() {
    using Function = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto address = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
    const auto isWow64Process2 = reinterpret_cast<Function>(reinterpret_cast<void*>(address));
    if (!isWow64Process2) return false; // missing before Windows 10 1709, older than any ARM64 Windows
    auto process = USHORT(0);
    auto native = USHORT(0);
    return !isWow64Process2(GetCurrentProcess(), &process, &native) || native == IMAGE_FILE_MACHINE_ARM64;
}

// Hyper-V's root partition, not nested within Hyper-V, as Hyper-V's own leaves identify it:
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/feature-discovery
bool hyperVRootPartition() {
    if (readCpuid(0x40000000).eax < 0x40000004) return false;
    const auto signature = readCpuid(0x40000001).eax;
    if (std::memcmp(&signature, "Hv#1", sizeof(signature)) != 0) return false;
    if (!(readCpuid(0x40000003).ebx & kCreatePartitions)) return false;
    return !(readCpuid(0x40000004).eax & kNestedHypervisor);
}

double enumeratedTscRate() {
    if (onArm64Windows()) return 0.;
    const auto basic = readCpuid(0);
    const unsigned vendor[] = {basic.ebx, basic.edx, basic.ecx};
    if (std::memcmp(vendor, "GenuineIntel", sizeof(vendor)) != 0 || basic.eax < 0x15) return 0.;
    if (readCpuid(0x80000000).eax < 0x80000007) return 0.;
    if (!(readCpuid(0x80000007).edx & kInvariantTsc)) return 0.;
    if ((readCpuid(1).ecx & kHypervisorPresent) && !hyperVRootPartition()) return 0.;
    const auto tsc = readCpuid(0x15);
    const auto denominator = tsc.eax;
    const auto numerator = tsc.ebx;
    const auto crystalHz = tsc.ecx;
    if (!denominator || !numerator || !crystalHz) return 0.;
    const auto rate = double(crystalHz) * numerator / denominator;
    return (rate >= kMinTscRate && rate <= kMaxTscRate) ? rate : 0.;
}
#endif

const ThreadClockSource& threadClockSource() {
    static const ThreadClockSource source = [] {
#ifdef TW_TSC
        const auto rate = enumeratedTscRate();
        if (rate > 0.) return ThreadClockSource{true, rate};
#endif
        return ThreadClockSource();
    }();
    return source;
}

} // namespace
#endif

double QuickJSContext::threadCpuTime() {
#ifdef _WIN32
    const auto& source = threadClockSource();
    thread_local ThreadClock state;
    auto count = uint64_t(0);
    auto ok = false;
    if (source.cycles) {
        auto cycles = ULONG64(0);
        ok = QueryThreadCycleTime(GetCurrentThread(), &cycles) != FALSE;
        count = cycles;
    } else {
        FILETIME creation{}, exit{}, kernel{}, user{};
        ok = GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user) != FALSE;
        auto ticks = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        count = ticks(kernel) + ticks(user);
    }
    if (!ok) {
        state.counting = false;
        return state.seconds;
    }
    if (state.counting && count > state.count) state.seconds += double(count - state.count) / source.ticksPerSecond;
    state.count = count;
    state.counting = true;
    return state.seconds;
#else
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return double(t.tv_sec) + double(t.tv_nsec) / 1e9;
#endif
}

QuickJSContext::QuickJSContext(size_t memoryLimitBytes, size_t maxStackBytes) {
    runtime_ = JS_NewRuntime();
    JS_SetMemoryLimit(runtime_, memoryLimitBytes);
    JS_SetMaxStackSize(runtime_, maxStackBytes);
    context_ = JS_NewContext(runtime_);
    JS_SetContextOpaque(context_, this);
    // No module loader is installed: `import()` of anything fails inside the sandbox.
    JS_SetInterruptHandler(runtime_, &QuickJSTrampoline::interrupt, this);
}

struct QuickJSContext::Captured {
    std::string name;
    JSValue fn;
};

QuickJSContext::~QuickJSContext() {
    for (Captured* c : captured_) {
        qjs_free_value(context_, c->fn);
        delete c;
    }
    JS_FreeContext(context_);
    JS_FreeRuntime(runtime_);
}

size_t QuickJSContext::memoryUsed() const { return qjs_memory_used(runtime_); }

void QuickJSContext::registerFunction(const std::string& name, HostFunction fn) {
    int magic = int(functions_.size());
    functions_.push_back(std::move(fn));
    JSValue f = qjs_new_cfunction_magic(context_, &QuickJSTrampoline::call, name.c_str(), 2, magic);
    qjs_set_global_prop(context_, name.c_str(), f); // takes ownership of f
}

// QuickJS measures stack depth from the stack pointer it saw when the runtime
// was created; re-read it on every entry (belt and braces — contexts are pinned
// to a JSThread anyway).
void QuickJSContext::enter() {
    JS_UpdateStackTop(runtime_);
    uncatchable = false;
}

/// False when a string could not be copied out: the engine's out of memory error is pending. Frees `v` either way.
static bool readResult(JSContext* ctx, JSValue v, JSResult& r) {
    bool copied = true;
    if (qjs_is_string(v)) {
        size_t length = 0;
        const char* c = JS_ToCStringLen2(ctx, &length, v, false);
        copied = c != nullptr;
        if (c) r.string = std::string(c, length);
        if (c) JS_FreeCString(ctx, c);
    } else if (qjs_is_number(v)) {
        r.number = qjs_to_float64(ctx, v);
    }
    qjs_free_value(ctx, v);
    return copied;
}

JSError QuickJSContext::takeException() {
    JSValue e = JS_GetException(context_);
    uncatchable = JS_IsUncatchableError(e);
    if (!uncatchable) {
        JS_Throw(context_, e);
        char* c = qjs_take_exception_description(context_);
        std::string text(c ? c : "unknown error");
        free(c);
        if (!JS_HasException(context_)) return JSError(text);
        e = JS_GetException(context_);
        uncatchable = JS_IsUncatchableError(e);
        if (!uncatchable) {
            qjs_free_value(context_, e);
            return JSError(text);
        }
    }
    // Thrown again it is uncatchable again: reading its message runs no getter of the widget's.
    JS_Throw(context_, qjs_dup_value(context_, e));
    JSResult message;
    readResult(context_, JS_GetPropertyStr(context_, e, "message"), message);
    qjs_free_value(context_, JS_GetException(context_));
    qjs_free_value(context_, e);
    return JSError("InternalError: " + message.string.value_or(""));
}

JSResult QuickJSContext::evaluate(const std::string& code, const std::string& filename, bool asModule) {
    enter();
    JSValue v = qjs_eval(context_, code.c_str(), code.size(), filename.c_str(), asModule);
    JSResult r;
    if (qjs_is_exception(v) || !readResult(context_, v, r)) throw takeException();
    return r;
}

JSResult QuickJSContext::evaluateBytecode(const std::vector<uint8_t>& blob, const std::string& source) {
    enter();
    JSValue v = qjs_eval_bytecode(context_, blob.data(), blob.size(), source.data(), source.size());
    JSResult r;
    if (qjs_is_exception(v) || !readResult(context_, v, r)) throw takeException();
    return r;
}

std::vector<uint8_t> QuickJSContext::compileBytecode(const std::string& code, const std::string& filename) {
    enter();
    size_t len = 0;
    uint8_t* blob = qjs_compile_bytecode(context_, code.data(), code.size(), filename.c_str(), &len);
    if (!blob) throw takeException();
    std::vector<uint8_t> out(blob, blob + len);
    qjs_free_bytecode(context_, blob);
    return out;
}

bool QuickJSContext::captureGlobalFunction(const std::string& name) {
    enter();
    JSValue fn = qjs_get_global_prop(context_, name.c_str());
    if (!qjs_is_function(context_, fn)) {
        qjs_free_value(context_, fn);
        return false;
    }
    for (Captured* c : captured_) {
        if (c->name == name) {
            qjs_free_value(context_, c->fn);
            c->fn = fn;
            return true;
        }
    }
    captured_.push_back(new Captured{name, fn});
    return true;
}

JSResult QuickJSContext::call(const std::string& name, const std::vector<std::string>& args) {
    enter();
    const JSValue* kept = nullptr;
    for (Captured* c : captured_) if (c->name == name) kept = &c->fn;
    JSValue fn = kept ? qjs_dup_value(context_, *kept) : qjs_get_global_prop(context_, name.c_str());
    if (!qjs_is_function(context_, fn)) {
        qjs_free_value(context_, fn);
        throw JSError(name + " is not a function");
    }
    std::vector<JSValue> argv;
    argv.reserve(args.size());
    for (const auto& a : args) argv.push_back(qjs_new_string_len(context_, a.data(), a.size()));
    JSValue r = qjs_call(context_, fn, int(argv.size()), argv.data());
    for (auto& v : argv) qjs_free_value(context_, v);
    qjs_free_value(context_, fn);
    JSResult result;
    if (qjs_is_exception(r) || !readResult(context_, r, result)) throw takeException();
    return result;
}

bool QuickJSContext::hasPendingJob() const { return qjs_is_job_pending(runtime_); }

bool QuickJSContext::executePendingJob(const std::function<void(const JSError&)>& onError) {
    enter();
    JSContext* ctx = nullptr;
    int r = qjs_execute_pending_job(runtime_, &ctx);
    if (r < 0) onError(takeException());
    return r != 0;
}

} // namespace twidget
