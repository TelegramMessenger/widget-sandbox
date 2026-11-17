// Thin C++ wrapper over the Nova engine (TelegramMessenger/nova-safe) through the C interface of its nova_embed_ffi crate (nova_engine.h): one engine, one realm, string-based host functions; scripts, arguments and results cross as copies, and all methods must be called from the JSThread that owns the context.
#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

struct nova_engine;

namespace twidget {

class JSError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Value returned by a host function into the sandbox.
struct HostReturn {
    enum class Kind { Undefined, Null, String, Number, ArrayBuffer, RangeError };
    Kind kind = Kind::Undefined;
    std::string str;
    double num = 0;
    std::vector<uint8_t> bytes;

    static HostReturn undefined() { return {}; }
    static HostReturn null() { HostReturn r; r.kind = Kind::Null; return r; }
    static HostReturn string(std::string s) { HostReturn r; r.kind = Kind::String; r.str = std::move(s); return r; }
    static HostReturn number(double d) { HostReturn r; r.kind = Kind::Number; r.num = d; return r; }
    static HostReturn arrayBuffer(std::vector<uint8_t> b) { HostReturn r; r.kind = Kind::ArrayBuffer; r.bytes = std::move(b); return r; }
    static HostReturn rangeError(std::string message) { HostReturn r; r.kind = Kind::RangeError; r.str = std::move(message); return r; }
};

/// Result of calling a sandbox function from the host: string, number or nothing.
struct JSResult {
    std::optional<std::string> string;
    std::optional<double> number;
};

class NovaContext {
public:
    using HostFunction = std::function<HostReturn(const std::vector<std::string>&)>;

    /// "nova_embed <version> (nova <commit>)".
    static std::string engineVersion();

    /// `maxStackBytes` must stay below the real stack of the thread that enters JS (JSThread: 8 MB); the engine's JavaScript gets three quarters of it, counted from each call into the engine. Throws JSError when the engine cannot be created or does not enforce its interrupt, heap and stack limits.
    explicit NovaContext(size_t memoryLimitBytes, size_t maxStackBytes = size_t(384) << 10);
    ~NovaContext();
    NovaContext(const NovaContext&) = delete;
    NovaContext& operator=(const NovaContext&) = delete;

    /// Interrupt deadline in `now()` seconds; +inf disables it.
    double deadline = std::numeric_limits<double>::infinity();
    /// Set by the interrupt handler when the deadline fired; reset by the caller.
    bool interrupted = false;
    /// Whether the error the last call threw is one script cannot catch (out of memory, the interrupt, a host function's failure, an engine fault).
    bool uncatchable = false;

    /// Interrupt deadline in `threadCpuTime()` seconds of the thread running JS; +inf disables it.
    double cpuDeadline = std::numeric_limits<double>::infinity();

    /// Monotonic seconds (steady clock), the clock `deadline` is measured on.
    static double now();
    /// CPU seconds consumed by the calling thread; unaffected by how busy the machine is.
    static double threadCpuTime();

    size_t memoryUsed() const;

    /// Exposes `name(...)` to the sandbox, `name` an identifier. Arguments arrive as strings (numbers are formatted), at most eight of them.
    void registerFunction(const std::string& name, HostFunction fn);

    JSResult evaluate(const std::string& code, const std::string& filename, bool asModule = false);
    /// Runs one of our own bundles from code compiled once per process and shared by every context; false when it could not be shared and was evaluated from `source` instead. Never for widget code.
    bool evaluateBundle(const std::string& source, const std::string& filename);
    /// Calls the global function `name` with string arguments.
    JSResult call(const std::string& name, const std::vector<std::string>& args = {});

    /// Keeps a private reference to the global function `name`; `call(name)` uses it from then on, even after the global is deleted.
    bool captureGlobalFunction(const std::string& name);

    bool hasPendingJob() const;
    /// Runs one queued promise job, if any (false: the queue was empty). An error thrown by the job goes to `onError`.
    bool executePendingJob(const std::function<void(const JSError&)>& onError);

private:
    enum class Failure { None, Memory, Other };

    void enter();
    /// The value of a finished call, or its error thrown as JSError; frees `result`.
    JSResult finish(int status, void* result);

    nova_engine* engine_ = nullptr;
    size_t memoryLimit_ = 0;
    std::vector<HostFunction> functions_;
    /// The RangeError a host function asked for, until the script's side of the function throws it.
    std::optional<std::string> thrown_;
    /// A host function threw: nothing more reaches the host and the running script is ended.
    Failure failure_ = Failure::None;
    std::string failureText_;
    friend struct NovaTrampoline;
};

} // namespace twidget
