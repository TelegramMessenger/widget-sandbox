#include "twidget/nova_context.h"

#include "steady_clock.h"
#include "twidget/engine_platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>

#include "nova_engine.h"

namespace twidget {
namespace {

constexpr size_t kMaxCompiledPerBundle = 4;

const uint8_t* bytesOf(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

std::string textOf(const nova_result& r) {
    return (r.bytes && r.len) ? std::string(reinterpret_cast<const char*>(r.bytes), r.len) : std::string();
}

bool isIdentifier(const std::string& name) {
    if (name.empty() || name.size() > 64 || (name[0] >= '0' && name[0] <= '9')) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
    });
}

/// The length of the well-formed UTF-8 sequence at `p`, 0 when there is none (overlong forms, surrogates and values past U+10FFFF are none).
size_t utf8SequenceLength(const unsigned char* p, size_t left) {
    if (p[0] < 0x80) return 1;
    auto tail = [&](size_t i) { return i < left && (p[i] & 0xC0) == 0x80; };
    if (p[0] >= 0xC2 && p[0] <= 0xDF) return tail(1) ? 2 : 0;
    if (p[0] >= 0xE0 && p[0] <= 0xEF) {
        if (!tail(1) || !tail(2)) return 0;
        if ((p[0] == 0xE0 && p[1] < 0xA0) || (p[0] == 0xED && p[1] > 0x9F)) return 0;
        return 3;
    }
    if (p[0] >= 0xF0 && p[0] <= 0xF4) {
        if (!tail(1) || !tail(2) || !tail(3)) return 0;
        if ((p[0] == 0xF0 && p[1] < 0x90) || (p[0] == 0xF4 && p[1] > 0x8F)) return 0;
        return 4;
    }
    return 0;
}

/// `text` with every byte that is not part of well-formed UTF-8 replaced by U+FFFD; nullopt when it is well-formed already.
std::optional<std::string> repairedUtf8(const std::string& text) {
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    std::optional<std::string> out;
    size_t kept = 0;
    for (size_t i = 0; i < text.size();) {
        const size_t length = utf8SequenceLength(p + i, text.size() - i);
        if (length) {
            i += length;
            continue;
        }
        if (!out) out.emplace();
        out->append(text, kept, i - kept);
        out->append("\xEF\xBF\xBD");
        kept = ++i;
    }
    if (out) out->append(text, kept, std::string::npos);
    return out;
}

/// Our own bundles compiled once per process (`nova_script_compile`): immutable code that holds no engine state, cannot be made from bytes and is never freed, which any context on any thread evaluates.
struct CompiledBundles {
    struct Entry {
        std::string source;
        nova_script* script = nullptr;
    };
    std::mutex mutex;
    std::map<std::string, std::vector<Entry>> entries;

    /// Null when `source` cannot be shared or does not compile: the caller evaluates it from source, which also reports the error.
    nova_script* script(const std::string& source, const std::string& filename) {
        std::lock_guard<std::mutex> lock(mutex);
        auto& list = entries[filename];
        for (const auto& entry : list) {
            if (entry.source == source) return entry.script;
        }
        if (list.size() >= kMaxCompiledPerBundle) return nullptr;
        nova_result error{};
        nova_script* compiled = nova_script_compile(bytesOf(source), source.size(), &error);
        nova_result_free(&error);
        list.push_back(Entry{source, compiled});
        return compiled;
    }
};

CompiledBundles& compiledBundles() {
    static auto* const result = new CompiledBundles();
    return *result;
}

} // namespace

struct NovaTrampoline {
    // Nothing thrown here or by a host function may leave it: the engine's frames under this one cannot be unwound.
    static void call(void* context, uint32_t id, const nova_arg* args, size_t argc, nova_return* ret) {
        auto* self = static_cast<NovaContext*>(context);
        if (!self) return;
        if (self->failure_ != NovaContext::Failure::None || size_t(id) >= self->functions_.size()) return;
        try {
            std::vector<std::string> list;
            list.reserve(argc);
            for (size_t i = 0; i < argc; i++) {
                const nova_arg& arg = args[i];
                // A copy the engine's heap could not hold ends the script: the engine makes its copies outside that heap.
                if (self->memoryLimit_ && arg.len > self->memoryLimit_) throw std::bad_alloc();
                if (arg.kind == NOVA_VALUE_STRING) {
                    const auto* text = reinterpret_cast<const char*>(arg.bytes);
                    list.emplace_back(text ? text : "", text ? strnlen(text, arg.len) : 0);
                } else if (arg.kind == NOVA_VALUE_NUMBER) {
                    double d = arg.number;
                    char buf[64];
                    if (d == std::floor(d) && std::fabs(d) < 1e15) snprintf(buf, sizeof buf, "%lld", (long long)d);
                    else snprintf(buf, sizeof buf, "%.17g", d);
                    list.emplace_back(buf);
                } else {
                    list.emplace_back();
                }
            }
            HostReturn r = self->functions_[size_t(id)](list);
            if (self->memoryLimit_ && std::max(r.str.size(), r.bytes.size()) > self->memoryLimit_) throw std::bad_alloc();
            switch (r.kind) {
            case HostReturn::Kind::Undefined: break;
            case HostReturn::Kind::Null: nova_return_null(ret); break;
            case HostReturn::Kind::String: nova_return_string(ret, bytesOf(r.str), r.str.size()); break;
            case HostReturn::Kind::Number: nova_return_number(ret, r.num); break;
            case HostReturn::Kind::ArrayBuffer: nova_return_bytes(ret, r.bytes.data(), r.bytes.size()); break;
            case HostReturn::Kind::RangeError: nova_return_range_error(ret, bytesOf(r.str), r.str.size()); break;
            }
        } catch (const std::bad_alloc&) {
            self->failure_ = NovaContext::Failure::Memory;
        } catch (const std::exception& e) {
            self->failure_ = NovaContext::Failure::Other;
            try { self->failureText_ = std::string("host function failed: ") + e.what(); } catch (...) { self->failureText_.clear(); }
        } catch (...) {
            self->failure_ = NovaContext::Failure::Other;
            try { self->failureText_ = "host function failed"; } catch (...) { self->failureText_.clear(); }
        }
    }

    static bool interrupt(void* context) {
        auto* self = static_cast<NovaContext*>(context);
        if (!self) return false;
        if (self->failure_ != NovaContext::Failure::None) return true;
        if (NovaContext::now() > self->deadline || (self->cpuDeadline != std::numeric_limits<double>::infinity() && NovaContext::threadCpuTime() > self->cpuDeadline)) {
            self->interrupted = true;
            return true;
        }
        return false;
    }

    /// Seconds east of UTC in the user's time zone, daylight saving time included.
    static int32_t timeZone(void* /*context*/, int64_t epochSeconds) { return platform::timeZoneOffset(epochSeconds); }
};

double NovaContext::now() { return detail::steadySeconds(); }

double NovaContext::threadCpuTime() { return platform::threadCpuSeconds(); }

std::string NovaContext::engineVersion() {
    const char* version = nova_embed_version();
    return version ? version : "";
}

NovaContext::NovaContext(size_t memoryLimitBytes, size_t maxStackBytes) : memoryLimit_(memoryLimitBytes) {
    // Without all three a script that never ends is never stopped, or deep recursion overflows the thread's stack.
    constexpr uint32_t limits = NOVA_LIMIT_INTERRUPT | NOVA_LIMIT_HEAP | NOVA_LIMIT_STACK;
    if ((nova_embed_limits() & limits) != limits) throw JSError("this build of the Nova engine does not enforce its limits");
    nova_result error{};
    engine_ = nova_engine_new(memoryLimitBytes, maxStackBytes / 4 * 3, &NovaTrampoline::call, &NovaTrampoline::interrupt, this, &error);
    const std::string why = textOf(error);
    nova_result_free(&error);
    if (!engine_) throw JSError(why.empty() ? "the Nova engine could not be created" : why);
    // Out of memory ends the script wherever it happens: caught, a script would go on just under its limit.
    if (nova_engine_set_out_of_memory_fatal(engine_, true) != NOVA_OK) {
        nova_engine_free(engine_);
        throw JSError("the Nova engine did not take its out-of-memory mode");
    }
    // No module loader exists: `import` of anything fails inside the sandbox.
    nova_engine_set_time_zone(engine_, &NovaTrampoline::timeZone);
}

NovaContext::~NovaContext() {
    nova_engine_free(engine_);
}

size_t NovaContext::memoryUsed() const { return nova_engine_memory_used(engine_); }

void NovaContext::registerFunction(const std::string& name, HostFunction fn) {
    if (!isIdentifier(name)) throw JSError("a host function's name must be an identifier of at most 64 characters");
    const uint32_t id = uint32_t(functions_.size());
    functions_.push_back(std::move(fn));
    nova_result result{};
    const int status = nova_engine_register(engine_, bytesOf(name), name.size(), id, 2, NOVA_ARGS_VALUES, &result);
    const std::string why = textOf(result);
    nova_result_free(&result);
    if (status != NOVA_OK) throw JSError("host function " + name + ": " + why);
}

void NovaContext::enter() {
    uncatchable = false;
}

JSResult NovaContext::finish(int status, void* opaque) {
    auto& result = *static_cast<nova_result*>(opaque);
    struct Freed {
        nova_result& result;
        ~Freed() { nova_result_free(&result); }
    } freed{result};
    if (failure_ != Failure::None) {
        const bool memory = (failure_ == Failure::Memory);
        failure_ = Failure::None;
        uncatchable = true;
        throw JSError(memory ? std::string("InternalError: out of memory") : "InternalError: " + failureText_);
    }
    switch (status) {
    case NOVA_OK: {
        // A result the engine's heap could not hold is an error, as an argument of a host call is.
        if (memoryLimit_ && result.len > memoryLimit_) {
            uncatchable = true;
            throw JSError("InternalError: out of memory");
        }
        JSResult r;
        if (result.kind == NOVA_VALUE_STRING) r.string = textOf(result);
        else if (result.kind == NOVA_VALUE_NUMBER) r.number = result.number;
        return r;
    }
    case NOVA_EXCEPTION: {
        const std::string text = textOf(result);
        throw JSError(text.empty() ? "unknown error" : text);
    }
    case NOVA_NOT_A_FUNCTION:
        throw JSError(textOf(result) + " is not a function");
    case NOVA_INTERRUPTED:
        uncatchable = true;
        throw JSError("InternalError: interrupted");
    case NOVA_OUT_OF_MEMORY:
        uncatchable = true;
        throw JSError("InternalError: out of memory");
    default: {
        uncatchable = true;
        const std::string text = textOf(result);
        throw JSError("InternalError: " + (text.empty() ? "the engine failed (status " + std::to_string(status) + ")" : text));
    }
    }
}

JSResult NovaContext::evaluate(const std::string& code, const std::string& /*filename*/, bool asModule) {
    enter();
    nova_result result{};
    int status = nova_engine_eval(engine_, bytesOf(code), code.size(), asModule, &result);
    if (status == NOVA_INVALID) {
        // The engine takes well-formed UTF-8 only; a lone surrogate or a stray byte becomes U+FFFD.
        if (const auto repaired = repairedUtf8(code)) {
            nova_result_free(&result);
            status = nova_engine_eval(engine_, bytesOf(*repaired), repaired->size(), asModule, &result);
        }
    }
    return finish(status, &result);
}

bool NovaContext::evaluateBundle(const std::string& source, const std::string& filename) {
    nova_script* script = compiledBundles().script(source, filename);
    if (!script) {
        evaluate(source, filename, false);
        return false;
    }
    enter();
    nova_result result{};
    finish(nova_engine_eval_script(engine_, script, &result), &result);
    return true;
}

bool NovaContext::captureGlobalFunction(const std::string& name) {
    enter();
    bool captured = false;
    nova_result result{};
    const int status = nova_engine_capture(engine_, bytesOf(name), name.size(), &captured, &result);
    nova_result_free(&result);
    if (failure_ != Failure::None) {
        failure_ = Failure::None;
        return false;
    }
    return status == NOVA_OK && captured;
}

JSResult NovaContext::call(const std::string& name, const std::vector<std::string>& args) {
    enter();
    if (args.size() > NOVA_MAX_CALL_ARGS) throw JSError("more than " + std::to_string(NOVA_MAX_CALL_ARGS) + " arguments to " + name);
    std::vector<nova_str> list;
    list.reserve(args.size());
    for (const auto& arg : args) list.push_back(nova_str{arg.empty() ? nullptr : bytesOf(arg), arg.size()});
    nova_result result{};
    return finish(nova_engine_call(engine_, bytesOf(name), name.size(), list.data(), list.size(), &result), &result);
}

bool NovaContext::hasPendingJob() const { return nova_engine_has_pending_job(engine_); }

bool NovaContext::executePendingJob(const std::function<void(const JSError&)>& onError) {
    enter();
    bool ran = false;
    nova_result result{};
    const int status = nova_engine_run_job(engine_, &ran, &result);
    if (!ran) {
        nova_result_free(&result);
        return false;
    }
    try {
        finish(status, &result);
    } catch (const JSError& e) {
        onError(e);
    }
    return true;
}

} // namespace twidget
