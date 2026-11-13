// The boxed QuickJS engine: the same fork commit as the native one, compiled to wasm and translated
// back to C (packages/quickjs-box), behind the scalar C surface of qjsbox_host.h. One box is one
// runtime and one context in a linear memory of its own. No JSValue and no engine pointer reaches
// this file: host functions receive their arguments as bytes inside the box, valid during the call,
// and never call back into it (the box does the conversions the native trampoline does).
//
// "Contain and crash": a fault inside the box ends the process, as an engine crash does today
// (docs/decisions.md, "QuickJS in a wasm2c box"). What this file reports as JSError is only what it
// can refuse without the engine: a box that cannot be created, a call from inside a host function.
//
// This translation unit is always compiled. Without -DTW_QUICKJS_BOX=ON (the box's C is not in the
// build) makeBoxedQuickJS returns nullptr with a reason, and a Boxed context fails closed.

#include "quickjs_engine.h"

#include "twidget/quickjs_context.h"

#ifdef TW_QUICKJS_BOX

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>

extern "C" {
#include "qjsbox_host.h"
}

namespace twidget {
namespace {

class BoxedQuickJS final : public QuickJSEngine {
public:
    BoxedQuickJS(size_t memoryLimitBytes, size_t maxStackBytes) {
        qjsbox_options options{};
        options.memory_limit_bytes = memoryLimitBytes;
        options.native_stack_bytes = maxStackBytes;
        qjsbox_callbacks callbacks{};
        callbacks.opaque = this;
        callbacks.call = &onCallThunk;
        callbacks.words = &onWordsThunk;
        callbacks.ops = &onOpsThunk;
        const char* error = nullptr;
        box_ = qjsbox_new(&options, &callbacks, &error);
        if (!box_) throw JSError(error ? error : "the box could not be created");
    }

    ~BoxedQuickJS() override {
        // No engine code runs here: the box's memory is released whatever state the engine is in.
        qjsbox_free(box_);
    }

    // The deadlines are checked by the box's host layer, in C. A write takes effect at once, also
    // while a task is on the box's stack (qjsbox_set_deadlines never enters the box). The uptime
    // deadline is kept in this process's steady clock (QuickJSContext::now, which SandboxEngine
    // computes against) and translated to the box's own uptime clock each time it is pushed; the
    // CPU deadline is thread CPU time, the same clock on both sides, so it passes through.
    double deadline() const override { return deadline_; }
    void setDeadline(double seconds) override { deadline_ = seconds; pushDeadlines(); }
    double cpuDeadline() const override { return cpuDeadline_; }
    void setCpuDeadline(double seconds) override { cpuDeadline_ = seconds; pushDeadlines(); }
    bool interrupted() const override { return qjsbox_interrupted(box_); }
    void setInterrupted(bool value) override { if (!value) qjsbox_clear_interrupted(box_); }

    size_t memoryUsed() const override { return qjsbox_memory_used(box_); }

    void registerFunction(const std::string& name, QuickJSContext::HostFunction fn) override {
        int32_t magic = int32_t(functions_.size());
        functions_.push_back(std::move(fn));
        qjsbox_register(box_, name.c_str(), QJSBOX_FN_STRINGS, magic, 2);
    }

    JSResult evaluate(const std::string& code, const std::string& filename, bool asModule) override {
        qjsbox_result result{};
        int status = qjsbox_eval(box_, code.data(), code.size(), filename.c_str(), asModule, &result);
        return take(status, result);
    }

    JSResult evaluateBytecode(const std::vector<uint8_t>& blob, const std::string& source) override {
        qjsbox_result result{};
        int status = qjsbox_eval_bytecode(box_, blob.data(), blob.size(), source.data(), source.size(), &result);
        return take(status, result);
    }

    std::vector<uint8_t> compileBytecode(const std::string& code, const std::string& filename) override {
        qjsbox_result result{};
        int status = qjsbox_compile(box_, code.data(), code.size(), filename.c_str(), &result);
        if (status == QJSBOX_VALUE && result.kind == QJSBOX_RESULT_STRING && result.str) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(result.str);
            return std::vector<uint8_t>(p, p + result.len);
        }
        if (status == QJSBOX_EXCEPTION) throw JSError(text(result));
        throw JSError(kRefused);
    }

    bool captureGlobalFunction(const std::string& name) override {
        int32_t slot = qjsbox_capture(box_, name.c_str());
        if (slot == 0) return false;
        auto it = captured_.find(name);
        if (it != captured_.end()) { qjsbox_release(box_, it->second); it->second = slot; }
        else captured_.emplace(name, slot);
        return true;
    }

    JSResult call(const std::string& name, const std::vector<std::string>& args) override {
        if (args.size() > size_t(QJSBOX_MAX_ARGS)) {
            throw JSError("more than " + std::to_string(int(QJSBOX_MAX_ARGS)) + " arguments to " + name);
        }
        std::vector<const char*> pointers;
        std::vector<size_t> lengths;
        pointers.reserve(args.size());
        lengths.reserve(args.size());
        for (const auto& a : args) { pointers.push_back(a.data()); lengths.push_back(a.size()); }
        auto it = captured_.find(name);
        int32_t slot = it != captured_.end() ? it->second : 0;
        qjsbox_result result{};
        int status = qjsbox_call(box_, slot, name.c_str(), int(args.size()),
                                 pointers.empty() ? nullptr : pointers.data(),
                                 lengths.empty() ? nullptr : lengths.data(), &result);
        if (status == QJSBOX_NOT_A_FUNCTION) throw JSError(name + " is not a function");
        return take(status, result);
    }

    bool hasPendingJob() const override { return qjsbox_job_pending(box_); }

    void executePendingJobs(const std::function<void(const JSError&)>& onError) override {
        for (int i = 0; i < 100; i++) {
            qjsbox_result result{};
            int r = qjsbox_run_job(box_, &result);
            if (r < 0) { onError(JSError(text(result))); continue; }
            if (r == 0) break;
        }
    }

private:
    static constexpr const char* kRefused =
        "a call into the boxed engine was refused (it was made from inside one of its host functions)";

    void pushDeadlines() {
        double uptimeDeadline = std::isinf(deadline_) ? deadline_
                                                      : qjsbox_uptime() + (deadline_ - QuickJSContext::now());
        qjsbox_set_deadlines(box_, uptimeDeadline, cpuDeadline_);
    }

    static std::string text(const qjsbox_result& r) {
        if (r.kind == QJSBOX_RESULT_STRING && r.str) return std::string(r.str, r.len);
        return "unknown error";
    }

    /// The value of a call that ended with `status` (mirrors the native readResult), or its exception.
    JSResult take(int status, const qjsbox_result& r) {
        JSResult out;
        switch (status) {
        case QJSBOX_VALUE:
            if (r.kind == QJSBOX_RESULT_STRING && r.str) out.string = std::string(r.str, r.len);
            else if (r.kind == QJSBOX_RESULT_NUMBER) out.number = r.num;
            return out;
        case QJSBOX_EXCEPTION:
            throw JSError(text(r));
        default: // QJSBOX_REFUSED and anything unexpected
            throw JSError(kRefused);
        }
    }

    // ── the box's callbacks (run on the engine thread, inside a call into the box) ──

    void onCall(int32_t magic, int32_t argc, const qjsbox_arg* args, qjsbox_return* ret) {
        if (magic < 0 || size_t(magic) >= functions_.size()) return;
        std::vector<std::string> strings;
        strings.reserve(size_t(argc));
        for (int i = 0; i < argc; i++) {
            const qjsbox_arg& a = args[i];
            if (a.kind == QJSBOX_ARG_STRING && a.str) {
                // The native engine takes a JS string as a C string: up to the first NUL.
                size_t len = 0;
                while (len < a.len && a.str[len] != '\0') len++;
                strings.emplace_back(a.str, len);
            } else if (a.kind == QJSBOX_ARG_NUMBER) {
                double d = a.num;
                char buf[64];
                if (d == std::floor(d) && std::fabs(d) < 1e15) snprintf(buf, sizeof buf, "%lld", (long long)d);
                else snprintf(buf, sizeof buf, "%.17g", d);
                strings.emplace_back(buf);
            } else {
                strings.emplace_back();
            }
        }
        HostReturn r = functions_[size_t(magic)](strings);
        switch (r.kind) {
        case HostReturn::Kind::Undefined: break;
        case HostReturn::Kind::Null: qjsbox_return_null(ret); break;
        case HostReturn::Kind::Number: qjsbox_return_number(ret, r.num); break;
        case HostReturn::Kind::String: qjsbox_return_string(ret, r.str.data(), r.str.size()); break;
        case HostReturn::Kind::ArrayBuffer: qjsbox_return_bytes(ret, r.bytes.data(), r.bytes.size()); break;
        }
    }

    static void onCallThunk(void* opaque, int32_t magic, int32_t argc, const qjsbox_arg* args, qjsbox_return* ret) {
        if (opaque) static_cast<BoxedQuickJS*>(opaque)->onCall(magic, argc, args, ret);
    }
    // The C++ core registers only string functions; the words/ops fast paths are the Swift binary-ops
    // optimization and are never reached here.
    static void onWordsThunk(void*, int32_t, double, const uint8_t*, size_t, const char*, size_t) {}
    static void onOpsThunk(void*, int32_t, const uint8_t*, size_t, const char*, size_t) {}

    qjsbox* box_ = nullptr;
    std::vector<QuickJSContext::HostFunction> functions_;
    std::map<std::string, int32_t> captured_;
    double deadline_ = std::numeric_limits<double>::infinity();
    double cpuDeadline_ = std::numeric_limits<double>::infinity();
};

} // namespace

std::unique_ptr<QuickJSEngine> makeBoxedQuickJS(size_t memoryLimitBytes, size_t maxStackBytes, std::string& reason) {
    try {
        return std::make_unique<BoxedQuickJS>(memoryLimitBytes, maxStackBytes);
    } catch (const JSError& e) {
        reason = e.what();
        return nullptr;
    }
}

bool quickjsBoxAvailable() { return qjsbox_build_optimized(); }

std::string quickjsBoxDescription() {
    return std::string("boxed QuickJS ") + qjsbox_quickjs_commit() + " (" + qjsbox_build_mode() + ")";
}

} // namespace twidget

#else // TW_QUICKJS_BOX

namespace twidget {

std::unique_ptr<QuickJSEngine> makeBoxedQuickJS(size_t, size_t, std::string& reason) {
    reason = "the boxed QuickJS engine is not part of this build (build packages/desktop with -DTW_QUICKJS_BOX=ON)";
    return nullptr;
}

bool quickjsBoxAvailable() { return false; }

std::string quickjsBoxDescription() { return ""; }

} // namespace twidget

#endif // TW_QUICKJS_BOX
