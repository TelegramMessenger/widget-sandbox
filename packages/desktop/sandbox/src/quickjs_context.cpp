#include "twidget/quickjs_context.h"

#include "quickjs_engine.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

// TW_NO_QUICKJS: a build without quickjs-ng (Android: Nova is the one engine there). A Native context then fails
// closed, as a Nova one does where Nova is not linked, and the default backend is Nova.
#ifndef TW_NO_QUICKJS
extern "C" {
#include "qjs_shim.h"
}
#endif

namespace twidget {

// ───────────── the native engine ─────────────

namespace {

#ifndef TW_NO_QUICKJS
class NativeQuickJS final : public QuickJSEngine {
public:
    NativeQuickJS(QuickJSContext& owner, size_t memoryLimitBytes, size_t maxStackBytes) : owner_(owner) {
        runtime_ = JS_NewRuntime();
        JS_SetMemoryLimit(runtime_, memoryLimitBytes);
        JS_SetMaxStackSize(runtime_, maxStackBytes);
        context_ = JS_NewContext(runtime_);
        JS_SetContextOpaque(context_, this);
        // No module loader is installed: `import()` of anything fails inside the sandbox.
        JS_SetInterruptHandler(runtime_, &interrupt, this);
    }

    ~NativeQuickJS() override {
        for (Captured* c : captured_) {
            qjs_free_value(context_, c->fn);
            delete c;
        }
        JS_FreeContext(context_);
        JS_FreeRuntime(runtime_);
    }

    std::string description() const override { return "native QuickJS"; }

    size_t memoryUsed() const override { return qjs_memory_used(runtime_); }

    void registerFunction(const std::string& name, QuickJSContext::HostFunction fn) override {
        int magic = int(functions_.size());
        functions_.push_back(std::move(fn));
        JSValue f = qjs_new_cfunction_magic(context_, &trampoline, name.c_str(), 2, magic);
        qjs_set_global_prop(context_, name.c_str(), f); // takes ownership of f
    }

    JSResult evaluate(const std::string& code, const std::string& filename, bool asModule) override {
        enter();
        JSValue v = qjs_eval(context_, code.c_str(), code.size(), filename.c_str(), asModule);
        JSResult r;
        if (qjs_is_exception(v) || !readResult(context_, v, r)) throw takeException();
        return r;
    }

    bool evaluateBundle(const std::string& source, const std::string& filename) override {
        evaluate(source, filename, false);
        return false;
    }

    JSResult evaluateBytecode(const std::vector<uint8_t>& blob, const std::string& source) override {
        enter();
        JSValue v = qjs_eval_bytecode(context_, blob.data(), blob.size(), source.data(), source.size());
        JSResult r;
        if (qjs_is_exception(v) || !readResult(context_, v, r)) throw takeException();
        return r;
    }

    std::vector<uint8_t> compileBytecode(const std::string& code, const std::string& filename) override {
        enter();
        size_t len = 0;
        uint8_t* blob = qjs_compile_bytecode(context_, code.data(), code.size(), filename.c_str(), &len);
        if (!blob) throw takeException();
        std::vector<uint8_t> out(blob, blob + len);
        qjs_free_bytecode(context_, blob);
        return out;
    }

    bool captureGlobalFunction(const std::string& name) override {
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

    JSResult call(const std::string& name, const std::vector<std::string>& args) override {
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

    bool hasPendingJob() const override { return qjs_is_job_pending(runtime_); }

    bool executePendingJob(const std::function<void(const JSError&)>& onError) override {
        enter();
        JSContext* ctx = nullptr;
        int r = qjs_execute_pending_job(runtime_, &ctx);
        if (r < 0) onError(takeException());
        return r != 0;
    }

private:
    struct Captured {
        std::string name;
        JSValue fn;
    };

    static JSValue trampoline(JSContext* ctx, JSValue /*this*/, int argc, JSValue* argv, int magic) {
        auto* self = static_cast<NativeQuickJS*>(JS_GetContextOpaque(ctx));
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
        auto* self = static_cast<NativeQuickJS*>(opaque);
        return self && self->owner_.pollInterrupt() ? 1 : 0;
    }

    // QuickJS measures stack depth from the stack pointer it saw when the runtime
    // was created; re-read it on every entry (belt and braces — contexts are pinned
    // to a JSThread anyway).
    void enter() {
        JS_UpdateStackTop(runtime_);
        owner_.uncatchable = false;
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

    JSError takeException() {
        JSValue e = JS_GetException(context_);
        owner_.uncatchable = JS_IsUncatchableError(e);
        if (!owner_.uncatchable) {
            JS_Throw(context_, e);
            char* c = qjs_take_exception_description(context_);
            std::string text(c ? c : "unknown error");
            free(c);
            if (!JS_HasException(context_)) return JSError(text);
            e = JS_GetException(context_);
            owner_.uncatchable = JS_IsUncatchableError(e);
            if (!owner_.uncatchable) {
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

    QuickJSContext& owner_;
    JSRuntime* runtime_;
    JSContext* context_;
    std::vector<QuickJSContext::HostFunction> functions_;
    std::vector<Captured*> captured_;
};

#endif // TW_NO_QUICKJS

/// The engine of a context whose backend this build cannot run: nothing evaluates, every call fails with the reason.
class UnavailableEngine final : public QuickJSEngine {
public:
    explicit UnavailableEngine(std::string reason) : reason_(std::move(reason)) {}
    std::string description() const override { return "unavailable"; }
    size_t memoryUsed() const override { return 0; }
    void registerFunction(const std::string&, QuickJSContext::HostFunction) override {}
    JSResult evaluate(const std::string&, const std::string&, bool) override { throw JSError(reason_); }
    bool evaluateBundle(const std::string&, const std::string&) override { throw JSError(reason_); }
    JSResult evaluateBytecode(const std::vector<uint8_t>&, const std::string&) override { throw JSError(reason_); }
    std::vector<uint8_t> compileBytecode(const std::string&, const std::string&) override { throw JSError(reason_); }
    bool captureGlobalFunction(const std::string&) override { return false; }
    JSResult call(const std::string&, const std::vector<std::string>&) override { throw JSError(reason_); }
    bool hasPendingJob() const override { return false; }
    bool executePendingJob(const std::function<void(const JSError&)>&) override { return false; }

private:
    std::string reason_;
};

} // namespace

#ifndef TW_NO_QUICKJS
std::unique_ptr<QuickJSEngine> makeNativeQuickJS(QuickJSContext& owner, size_t memoryLimitBytes, size_t maxStackBytes) {
    return std::make_unique<NativeQuickJS>(owner, memoryLimitBytes, maxStackBytes);
}
#endif

bool quickjsAvailable() {
#ifdef TW_NO_QUICKJS
    return false;
#else
    return true;
#endif
}

// ───────────── QuickJSContext ─────────────

QuickJSBackend quickjsDefaultBackend() {
    if (!quickjsAvailable()) return QuickJSBackend::Nova;
    static const QuickJSBackend backend = [] {
        const char* name = std::getenv("WIDGET_SANDBOX_ENGINE");
        return name && std::strcmp(name, "nova") == 0 ? QuickJSBackend::Nova : QuickJSBackend::Native;
    }();
    return backend;
}

double QuickJSContext::now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

double QuickJSContext::threadCpuTime() {
#ifdef _WIN32
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return now();
    auto ticks = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return double(ticks(kernel) + ticks(user)) / 1e7;
#else
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return double(t.tv_sec) + double(t.tv_nsec) / 1e9;
#endif
}

QuickJSContext::QuickJSContext(size_t memoryLimitBytes, size_t maxStackBytes, QuickJSBackend backend) : backend_(backend) {
    if (backend == QuickJSBackend::Nova) {
        engine_ = makeNovaEngine(*this, memoryLimitBytes, maxStackBytes, unavailableReason_);
        if (!engine_) {
            if (unavailableReason_.empty()) unavailableReason_ = "the Nova engine could not be created";
            engine_ = std::make_unique<UnavailableEngine>(unavailableReason_);
        }
    } else {
#ifdef TW_NO_QUICKJS
        (void)memoryLimitBytes;
        (void)maxStackBytes;
        unavailableReason_ = "QuickJS is not in this build";
        engine_ = std::make_unique<UnavailableEngine>(unavailableReason_);
#else
        engine_ = makeNativeQuickJS(*this, memoryLimitBytes, maxStackBytes);
#endif
    }
}

QuickJSContext::~QuickJSContext() = default;

bool QuickJSContext::pollInterrupt() {
    if (now() > deadline || (cpuDeadline != std::numeric_limits<double>::infinity() && threadCpuTime() > cpuDeadline)) {
        interrupted = true;
        return true;
    }
    return false;
}

std::string QuickJSContext::engineDescription() const { return engine_->description(); }

size_t QuickJSContext::memoryUsed() const { return engine_->memoryUsed(); }

void QuickJSContext::registerFunction(const std::string& name, HostFunction fn) { engine_->registerFunction(name, std::move(fn)); }

JSResult QuickJSContext::evaluate(const std::string& code, const std::string& filename, bool asModule) {
    return engine_->evaluate(code, filename, asModule);
}

bool QuickJSContext::evaluateBundle(const std::string& source, const std::string& filename) {
    return engine_->evaluateBundle(source, filename);
}

JSResult QuickJSContext::evaluateBytecode(const std::vector<uint8_t>& blob, const std::string& source) {
    return engine_->evaluateBytecode(blob, source);
}

std::vector<uint8_t> QuickJSContext::compileBytecode(const std::string& code, const std::string& filename) {
    return engine_->compileBytecode(code, filename);
}

bool QuickJSContext::captureGlobalFunction(const std::string& name) { return engine_->captureGlobalFunction(name); }

JSResult QuickJSContext::call(const std::string& name, const std::vector<std::string>& args) { return engine_->call(name, args); }

bool QuickJSContext::hasPendingJob() const { return engine_->hasPendingJob(); }

bool QuickJSContext::executePendingJob(const std::function<void(const JSError&)>& onError) {
    return engine_->executePendingJob(onError);
}

} // namespace twidget
