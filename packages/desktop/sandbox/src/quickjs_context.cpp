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

extern "C" {
#include "qjs_shim.h"
}

namespace twidget {

// ───────────── the native engine ─────────────

namespace {

class NativeQuickJS final : public QuickJSEngine {
public:
    NativeQuickJS(size_t memoryLimitBytes, size_t maxStackBytes) {
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

    double deadline() const override { return deadline_; }
    void setDeadline(double seconds) override { deadline_ = seconds; }
    double cpuDeadline() const override { return cpuDeadline_; }
    void setCpuDeadline(double seconds) override { cpuDeadline_ = seconds; }
    bool interrupted() const override { return interrupted_; }
    void setInterrupted(bool value) override { interrupted_ = value; }

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
        if (qjs_is_exception(v)) throw takeException();
        return readResult(v);
    }

    JSResult evaluateBytecode(const std::vector<uint8_t>& blob, const std::string& source) override {
        enter();
        JSValue v = qjs_eval_bytecode(context_, blob.data(), blob.size(), source.data(), source.size());
        if (qjs_is_exception(v)) throw takeException();
        return readResult(v);
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
        if (qjs_is_exception(r)) throw takeException();
        return readResult(r);
    }

    bool hasPendingJob() const override { return qjs_is_job_pending(runtime_); }

    void executePendingJobs(const std::function<void(const JSError&)>& onError) override {
        enter();
        for (int i = 0; i < 100; i++) {
            JSContext* ctx = nullptr;
            int r = qjs_execute_pending_job(runtime_, &ctx);
            if (r < 0) { onError(takeException()); continue; }
            if (r == 0) break;
        }
    }

private:
    struct Captured {
        std::string name;
        JSValue fn;
    };

    // QuickJS measures stack depth from the stack pointer it saw when the runtime
    // was created; re-read it on every entry (belt and braces — contexts are pinned
    // to a JSThread anyway).
    void enter() { JS_UpdateStackTop(runtime_); }

    JSError takeException() {
        char* c = qjs_take_exception_description(context_);
        if (!c) return JSError("unknown error");
        std::string text(c);
        free(c);
        return JSError(text);
    }

    JSResult readResult(JSValue v) {
        JSResult r;
        if (qjs_is_string(v)) {
            size_t length = 0;
            const char* c = JS_ToCStringLen2(context_, &length, v, false);
            r.string = c ? std::string(c, length) : std::string();
            if (c) JS_FreeCString(context_, c);
        } else if (qjs_is_number(v)) {
            r.number = qjs_to_float64(context_, v);
        }
        qjs_free_value(context_, v);
        return r;
    }

    static JSValue trampoline(JSContext* ctx, JSValue /*this*/, int argc, JSValue* argv, int magic) {
        auto* self = static_cast<NativeQuickJS*>(JS_GetContextOpaque(ctx));
        if (!self || magic < 0 || size_t(magic) >= self->functions_.size()) return qjs_undefined();
        std::vector<std::string> args;
        args.reserve(size_t(argc));
        for (int i = 0; i < argc; i++) {
            JSValue v = argv[i];
            if (qjs_is_string(v)) {
                const char* c = qjs_to_cstring(ctx, v);
                args.emplace_back(c ? c : "");
                if (c) qjs_free_cstring(ctx, c);
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
        }
        return qjs_undefined();
    }

    static int interrupt(JSContext* /*ctx*/, void* opaque) {
        auto* self = static_cast<NativeQuickJS*>(opaque);
        if (!self) return 0;
        if (QuickJSContext::now() > self->deadline_ ||
            (self->cpuDeadline_ != std::numeric_limits<double>::infinity() && QuickJSContext::threadCpuTime() > self->cpuDeadline_)) {
            self->interrupted_ = true;
            return 1;
        }
        return 0;
    }

    JSRuntime* runtime_ = nullptr;
    JSContext* context_ = nullptr;
    std::vector<QuickJSContext::HostFunction> functions_;
    std::vector<Captured*> captured_;
    double deadline_ = std::numeric_limits<double>::infinity();
    double cpuDeadline_ = std::numeric_limits<double>::infinity();
    bool interrupted_ = false;
};

/// The engine of a context that could not have one (a boxed engine this build cannot provide):
/// nothing runs, every evaluation and call throws with the reason (fail closed).
class UnavailableQuickJS final : public QuickJSEngine {
public:
    explicit UnavailableQuickJS(std::string reason) : reason_(std::move(reason)) {}

    double deadline() const override { return deadline_; }
    void setDeadline(double seconds) override { deadline_ = seconds; }
    double cpuDeadline() const override { return cpuDeadline_; }
    void setCpuDeadline(double seconds) override { cpuDeadline_ = seconds; }
    bool interrupted() const override { return false; }
    void setInterrupted(bool) override {}
    size_t memoryUsed() const override { return 0; }
    void registerFunction(const std::string&, QuickJSContext::HostFunction) override {}
    JSResult evaluate(const std::string&, const std::string&, bool) override { throw JSError(reason_); }
    JSResult evaluateBytecode(const std::vector<uint8_t>&, const std::string&) override { throw JSError(reason_); }
    std::vector<uint8_t> compileBytecode(const std::string&, const std::string&) override { throw JSError(reason_); }
    bool captureGlobalFunction(const std::string&) override { return false; }
    JSResult call(const std::string&, const std::vector<std::string>&) override { throw JSError(reason_); }
    bool hasPendingJob() const override { return false; }
    void executePendingJobs(const std::function<void(const JSError&)>&) override {}

private:
    std::string reason_;
    double deadline_ = std::numeric_limits<double>::infinity();
    double cpuDeadline_ = std::numeric_limits<double>::infinity();
};

} // namespace

std::unique_ptr<QuickJSEngine> makeNativeQuickJS(size_t memoryLimitBytes, size_t maxStackBytes) {
    return std::make_unique<NativeQuickJS>(memoryLimitBytes, maxStackBytes);
}

// ───────────── the facade ─────────────

QuickJSBackend quickjsDefaultBackend() {
    static const QuickJSBackend d = [] {
        const char* e = std::getenv("WIDGET_SANDBOX_ENGINE");
        return (e && std::string(e) == "boxed") ? QuickJSBackend::Boxed : QuickJSBackend::Native;
    }();
    return d;
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

QuickJSContext::QuickJSContext(size_t memoryLimitBytes, size_t maxStackBytes, QuickJSBackend backend)
    : backend_(backend) {
    if (backend == QuickJSBackend::Boxed) {
        std::string reason;
        engine_ = makeBoxedQuickJS(memoryLimitBytes, maxStackBytes, reason);
        if (!engine_) {
            unavailableReason_ = reason.empty() ? "the boxed QuickJS engine is not part of this build" : reason;
            engine_ = std::make_unique<UnavailableQuickJS>(unavailableReason_);
        }
    } else {
        engine_ = makeNativeQuickJS(memoryLimitBytes, maxStackBytes);
    }
}

QuickJSContext::~QuickJSContext() = default;

std::string QuickJSContext::engineDescription() const {
    if (backend_ == QuickJSBackend::Boxed) {
        std::string d = quickjsBoxDescription();
        return d.empty() ? std::string("boxed QuickJS") : d;
    }
    return "native QuickJS";
}

double QuickJSContext::deadline() const { return engine_->deadline(); }
void QuickJSContext::setDeadline(double seconds) { engine_->setDeadline(seconds); }
double QuickJSContext::cpuDeadline() const { return engine_->cpuDeadline(); }
void QuickJSContext::setCpuDeadline(double seconds) { engine_->setCpuDeadline(seconds); }
bool QuickJSContext::interrupted() const { return engine_->interrupted(); }
void QuickJSContext::setInterrupted(bool value) { engine_->setInterrupted(value); }

size_t QuickJSContext::memoryUsed() const { return engine_->memoryUsed(); }

void QuickJSContext::registerFunction(const std::string& name, HostFunction fn) {
    engine_->registerFunction(name, std::move(fn));
}

JSResult QuickJSContext::evaluate(const std::string& code, const std::string& filename, bool asModule) {
    return engine_->evaluate(code, filename, asModule);
}

JSResult QuickJSContext::evaluateBytecode(const std::vector<uint8_t>& blob, const std::string& source) {
    return engine_->evaluateBytecode(blob, source);
}

std::vector<uint8_t> QuickJSContext::compileBytecode(const std::string& code, const std::string& filename) {
    return engine_->compileBytecode(code, filename);
}

bool QuickJSContext::captureGlobalFunction(const std::string& name) {
    return engine_->captureGlobalFunction(name);
}

JSResult QuickJSContext::call(const std::string& name, const std::vector<std::string>& args) {
    return engine_->call(name, args);
}

bool QuickJSContext::hasPendingJob() const { return engine_->hasPendingJob(); }

void QuickJSContext::executePendingJobs(const std::function<void(const JSError&)>& onError) {
    engine_->executePendingJobs(onError);
}

} // namespace twidget
