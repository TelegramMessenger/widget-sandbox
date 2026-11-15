// The Nova engine: a JavaScript engine written in Rust whose crates forbid `unsafe` (the nova-safe
// fork), behind the C interface of its nova_embed_ffi crate (nova_engine.h), the only place with
// `unsafe`. No engine pointer or value reaches this file: scripts and arguments go in as UTF-8
// copies, and a host function receives copies of its arguments, valid during the call, and never
// calls back into the engine (the library refuses such a call).
//
// Nova does not run QuickJS bytecode: our bundles are compiled from source once per process
// (`nova_script_compile`) and every engine evaluates that code. A panic inside the engine is caught
// by the library and makes the engine dead; every later call fails and the widget stops.
//
// Compiled only into builds that link the library (TW_NOVA_ENGINE); elsewhere a Nova context fails
// closed (quickjs_context.cpp).
#include "quickjs_engine.h"

#ifdef TW_NOVA_ENGINE

#include "nova_engine.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <new>

namespace twidget {

namespace {

/// Whether a sequence of UTF-8 as Rust's `str` requires it starts at `p` (no overlong forms, no surrogates, nothing
/// past U+10FFFF); `length` is its length when it does.
bool utf8Step(const unsigned char* p, size_t left, size_t& length) {
    unsigned char c = p[0];
    if (c < 0x80) { length = 1; return true; }
    auto cont = [&](size_t i, unsigned char lo, unsigned char hi) { return i < left && p[i] >= lo && p[i] <= hi; };
    if (c >= 0xC2 && c <= 0xDF) {
        length = 2;
        return cont(1, 0x80, 0xBF);
    }
    if (c >= 0xE0 && c <= 0xEF) {
        length = 3;
        unsigned char lo = c == 0xE0 ? 0xA0 : 0x80, hi = c == 0xED ? 0x9F : 0xBF;
        return cont(1, lo, hi) && cont(2, 0x80, 0xBF);
    }
    if (c >= 0xF0 && c <= 0xF4) {
        length = 4;
        unsigned char lo = c == 0xF0 ? 0x90 : 0x80, hi = c == 0xF4 ? 0x8F : 0xBF;
        return cont(1, lo, hi) && cont(2, 0x80, 0xBF) && cont(3, 0x80, 0xBF);
    }
    length = 1;
    return false;
}

bool isUtf8(const std::string& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    for (size_t i = 0, n = s.size(); i < n;) {
        size_t length = 1;
        if (!utf8Step(p + i, n - i, length)) return false;
        i += length;
    }
    return true;
}

/// `s` with every byte that is not part of a UTF-8 sequence replaced by U+FFFD: QuickJS reads a source leniently,
/// Nova takes UTF-8 only.
std::string repairUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    for (size_t i = 0, n = s.size(); i < n;) {
        size_t length = 1;
        if (utf8Step(p + i, n - i, length)) {
            out.append(s, i, length);
            i += length;
        } else {
            out += "\xEF\xBF\xBD";
            i += 1;
        }
    }
    return out;
}

std::string text(const nova_result& r) {
    return r.bytes && r.len ? std::string(reinterpret_cast<const char*>(r.bytes), r.len) : std::string();
}

const uint8_t* bytesOf(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

/// Our own bundles compiled once per process and evaluated by every Nova engine: an engine then skips parsing and
/// compiling about 600 KB of JavaScript, most of its boot. A compiled script holds no engine state, is never freed and
/// cannot be made from bytes; only the package's bundles come here, never widget code. A bundle the engine cannot
/// share, or that does not compile, is evaluated from its source, which also reports the error.
class CompiledBundles {
public:
    static CompiledBundles& shared() {
        static CompiledBundles* instance = new CompiledBundles();
        return *instance;
    }

    /// The compiled script of `source`, compiling it the first time (other engines wait meanwhile rather than compile
    /// it too); nullptr when it cannot be shared or does not compile.
    const nova_script* script(const std::string& source, const std::string& filename) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entries = entries_[filename];
        for (const Entry& e : entries) if (e.source == source) return e.script;
        nova_result error{};
        nova_script* script = nova_script_compile(bytesOf(source), source.size(), &error);
        nova_result_free(&error);
        // A bundle is replaced only by another release's: a handful of entries per process at most.
        if (entries.size() >= 4) return script;
        entries.push_back(Entry{source, script});
        return script;
    }

private:
    struct Entry {
        std::string source;
        nova_script* script;
    };
    std::mutex mutex_;
    std::map<std::string, std::vector<Entry>> entries_;
};

bool enforcesLimits() {
    const uint32_t all = NOVA_LIMIT_INTERRUPT | NOVA_LIMIT_HEAP | NOVA_LIMIT_STACK;
    return (nova_embed_limits() & all) == all;
}

class NovaEngine final : public QuickJSEngine {
public:
    NovaEngine(QuickJSContext& owner, nova_engine* engine) : owner_(owner), engine_(engine) {}

    /// The native stack Nova's JavaScript may use on a thread whose QuickJS limit is `maxStackBytes`: three quarters
    /// of it, so the frames between two of the engine's checks and the host's own stay inside the thread's stack.
    static size_t stackLimit(size_t maxStackBytes) { return maxStackBytes / 4 * 3; }

    static std::unique_ptr<QuickJSEngine> create(QuickJSContext& owner, size_t memoryLimitBytes, size_t maxStackBytes, std::string& reason) {
        if (!enforcesLimits()) {
            reason = "this build of the Nova engine does not enforce its limits (" + std::to_string(nova_embed_limits()) + " of 7)";
            return nullptr;
        }
        // The engine keeps the callbacks' context for its whole life: the NovaEngine object, made first.
        auto self = std::unique_ptr<NovaEngine>(new NovaEngine(owner, nullptr));
        nova_result error{};
        self->engine_ = nova_engine_new(memoryLimitBytes, stackLimit(maxStackBytes), &hostCall, &interrupt, self.get(), &error);
        std::string why = text(error);
        nova_result_free(&error);
        if (!self->engine_) {
            reason = why.empty() ? "the Nova engine could not be created" : why;
            return nullptr;
        }
        nova_engine_set_time_zone(self->engine_, &timeZoneOffset);
        return self;
    }

    ~NovaEngine() override { nova_engine_free(engine_); }

    std::string description() const override { return nova_embed_version(); }

    size_t memoryUsed() const override { return nova_engine_memory_used(engine_); }

    void registerFunction(const std::string& name, QuickJSContext::HostFunction fn) override {
        if (functions_.size() >= NOVA_MAX_HOST_FUNCTIONS) return;
        uint32_t id = uint32_t(functions_.size());
        functions_.push_back(std::move(fn));
        nova_engine_register(engine_, bytesOf(name), name.size(), id, 2, NOVA_ARGS_VALUES, nullptr);
    }

    JSResult evaluate(const std::string& code, const std::string& /*filename*/, bool asModule) override {
        enter();
        nova_result out{};
        int32_t status;
        if (isUtf8(code)) {
            status = nova_engine_eval(engine_, bytesOf(code), code.size(), asModule, &out);
        } else {
            std::string repaired = repairUtf8(code);
            status = nova_engine_eval(engine_, bytesOf(repaired), repaired.size(), asModule, &out);
        }
        return take(status, out);
    }

    bool evaluateBundle(const std::string& source, const std::string& filename) override {
        const nova_script* script = CompiledBundles::shared().script(source, filename);
        if (!script) {
            evaluate(source, filename, false);
            return false;
        }
        enter();
        nova_result out{};
        take(nova_engine_eval_script(engine_, script, &out), out);
        return true;
    }

    JSResult evaluateBytecode(const std::vector<uint8_t>&, const std::string&) override {
        throw JSError("the Nova engine does not run QuickJS bytecode");
    }

    std::vector<uint8_t> compileBytecode(const std::string&, const std::string&) override {
        throw JSError("the Nova engine does not compile QuickJS bytecode");
    }

    bool captureGlobalFunction(const std::string& name) override {
        enter();
        bool captured = false;
        int32_t status = nova_engine_capture(engine_, bytesOf(name), name.size(), &captured, nullptr);
        return status == NOVA_OK && captured;
    }

    JSResult call(const std::string& name, const std::vector<std::string>& args) override {
        if (args.size() > NOVA_MAX_CALL_ARGS) throw JSError("more than " + std::to_string(NOVA_MAX_CALL_ARGS) + " arguments to " + name);
        enter();
        std::vector<nova_str> argv;
        argv.reserve(args.size());
        for (const auto& a : args) argv.push_back(nova_str{a.empty() ? nullptr : bytesOf(a), a.size()});
        nova_result out{};
        int32_t status = nova_engine_call(engine_, bytesOf(name), name.size(), argv.data(), argv.size(), &out);
        return take(status, out);
    }

    bool hasPendingJob() const override { return nova_engine_has_pending_job(engine_); }

    bool executePendingJob(const std::function<void(const JSError&)>& onError) override {
        enter();
        bool ran = false;
        nova_result out{};
        int32_t status = nova_engine_run_job(engine_, &ran, &out);
        if (!ran) {
            nova_result_free(&out);
            return false;
        }
        if (status != NOVA_OK) {
            JSError failure = error(status, out);
            nova_result_free(&out);
            onError(failure);
        } else {
            nova_result_free(&out);
        }
        return true;
    }

private:
    void enter() {
        owner_.uncatchable = false;
        hostFailure_.clear();
        hostFailed_ = false;
    }

    /// What the native engine reports for the same failures: "name: message" for an exception the script could have
    /// caught, "InternalError: …" (and `uncatchable`) for the interrupt, the memory limit and a failed engine.
    JSError error(int32_t status, const nova_result& r) {
        std::string what = text(r);
        switch (status) {
        case NOVA_EXCEPTION:
            return JSError(what.empty() ? "unknown error" : what);
        case NOVA_NOT_A_FUNCTION:
            return JSError(what + " is not a function");
        case NOVA_INTERRUPTED:
            owner_.uncatchable = true;
            // A host function that failed asked for the stop (hostCall): not the CPU limit.
            if (!hostFailure_.empty()) return JSError("InternalError: " + hostFailure_);
            return JSError("InternalError: interrupted");
        case NOVA_OUT_OF_MEMORY:
            owner_.uncatchable = true;
            return JSError("InternalError: out of memory");
        default:
            owner_.uncatchable = true;
            return JSError("InternalError: the Nova engine failed (" + (what.empty() ? "status " + std::to_string(status) : what) + ")");
        }
    }

    JSResult take(int32_t status, nova_result& r) {
        struct Free {
            nova_result& r;
            ~Free() { nova_result_free(&r); }
        } free{r};
        if (status != NOVA_OK) throw error(status, r);
        if (hostFailed_) {
            // The script ended before the engine's next poll: the failure still ends the call, as it does natively.
            hostFailed_ = false;
            owner_.uncatchable = true;
            throw JSError("InternalError: " + (hostFailure_.empty() ? std::string("host function failed") : hostFailure_));
        }
        JSResult result;
        if (r.kind == NOVA_VALUE_STRING) result.string = text(r);
        else if (r.kind == NOVA_VALUE_NUMBER) result.number = r.number;
        return result;
    }

    // The three callbacks of the library. They run on the engine's thread, inside a call into it, and must not call
    // into it: everything they need arrives as arguments. Rust frames are under them: nothing may be thrown out.

    /// What the native trampoline builds: strings up to their first NUL, numbers formatted, the rest empty.
    static void hostCall(void* context, uint32_t id, const nova_arg* argv, size_t argc, nova_return* ret) {
        auto* self = static_cast<NovaEngine*>(context);
        if (!self || id >= self->functions_.size()) return;
        try {
            std::vector<std::string> args;
            args.reserve(argc);
            for (size_t i = 0; i < argc; i++) {
                const nova_arg& a = argv[i];
                if (a.kind == NOVA_VALUE_STRING) {
                    const char* p = reinterpret_cast<const char*>(a.bytes);
                    size_t n = p ? a.len : 0;
                    if (const void* nul = n ? std::memchr(p, 0, n) : nullptr) n = size_t(static_cast<const char*>(nul) - p);
                    args.emplace_back(p ? p : "", n);
                } else if (a.kind == NOVA_VALUE_NUMBER) {
                    double d = a.number;
                    char buf[64];
                    if (d == std::floor(d) && std::fabs(d) < 1e15) snprintf(buf, sizeof buf, "%lld", (long long)d);
                    else snprintf(buf, sizeof buf, "%.17g", d);
                    args.emplace_back(buf);
                } else {
                    args.emplace_back();
                }
            }
            HostReturn r = self->functions_[id](args);
            switch (r.kind) {
            case HostReturn::Kind::Undefined: break;
            case HostReturn::Kind::Null: nova_return_null(ret); break;
            case HostReturn::Kind::String: nova_return_string(ret, bytesOf(r.str), r.str.size()); break;
            case HostReturn::Kind::Number: nova_return_number(ret, r.num); break;
            case HostReturn::Kind::ArrayBuffer: nova_return_bytes(ret, r.bytes.data(), r.bytes.size()); break;
            case HostReturn::Kind::RangeError: nova_return_range_error(ret, bytesOf(r.str), r.str.size()); break;
            }
        } catch (const std::bad_alloc&) {
            self->failHost("out of memory");
        } catch (const std::exception& e) {
            self->failHost(std::string("host function failed: ") + e.what());
        } catch (...) {
            self->failHost("host function failed");
        }
    }

    /// The native engine throws an uncatchable error out of a host function that failed. A Nova host function can
    /// only return: the script goes on with `undefined` until the engine's next interrupt poll, which ends it.
    void failHost(const char* why) noexcept {
        try { hostFailure_ = why; } catch (...) { hostFailed_ = true; }
        hostFailed_ = true;
    }
    void failHost(const std::string& why) noexcept { failHost(why.c_str()); }

    static bool interrupt(void* context) {
        auto* self = static_cast<NovaEngine*>(context);
        if (!self) return false;
        if (self->hostFailed_) {
            self->hostFailed_ = false;
            if (self->hostFailure_.empty()) self->hostFailure_ = "host function failed";
            return true;
        }
        return self->owner_.pollInterrupt();
    }

    /// Seconds east of UTC at that instant in the user's time zone, daylight saving time included: what QuickJS reads
    /// from localtime_r.
    static int32_t timeZoneOffset(void* /*context*/, int64_t epochSeconds) {
#ifdef _WIN32
        time_t t = time_t(epochSeconds);
        struct tm local{}, utc{};
        if (localtime_s(&local, &t) != 0 || gmtime_s(&utc, &t) != 0) return 0;
        local.tm_isdst = 0;
        utc.tm_isdst = 0;
        return int32_t(_mkgmtime(&local) - _mkgmtime(&utc));
#else
        time_t t = time_t(epochSeconds);
        struct tm local{};
        if (!localtime_r(&t, &local)) return 0;
        return int32_t(local.tm_gmtoff);
#endif
    }

    QuickJSContext& owner_;
    nova_engine* engine_;
    std::vector<QuickJSContext::HostFunction> functions_;
    std::string hostFailure_;
    bool hostFailed_ = false;
};

} // namespace

bool novaAvailable() { return enforcesLimits(); }

std::string novaDescription() { return nova_embed_version(); }

std::unique_ptr<QuickJSEngine> makeNovaEngine(QuickJSContext& owner, size_t memoryLimitBytes, size_t maxStackBytes, std::string& reason) {
    return NovaEngine::create(owner, memoryLimitBytes, maxStackBytes, reason);
}

} // namespace twidget

#else // !TW_NOVA_ENGINE

namespace twidget {

bool novaAvailable() { return false; }

std::string novaDescription() { return {}; }

std::unique_ptr<QuickJSEngine> makeNovaEngine(QuickJSContext&, size_t, size_t, std::string& reason) {
    reason = "the Nova engine is not part of this build";
    return nullptr;
}

} // namespace twidget

#endif
