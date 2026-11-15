// One JavaScript engine and context behind one interface, whichever engine runs it: quickjs-ng
// compiled into the process (the default), or Nova (the nova-safe fork: a JavaScript engine in Rust
// whose crates forbid `unsafe`, behind the small C interface of its nova_embed_ffi crate). The
// native path goes through the same qjs_shim.c that iOS and Android use (quickjs.h's JSValue macros
// are compound literals, not C++). One runtime, one context, string-based host functions. All
// methods must be called from the JSThread that owns the context.
#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

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

/// Which engine runs a context's code.
enum class QuickJSBackend {
    /// quickjs-ng compiled into the process. The default.
    Native,
    /// Nova (github.com/macosswift/nova-safe): every engine crate is built with
    /// `#![forbid(unsafe_code)]`, so no script can reach memory corruption in the engine. Opt-in
    /// (-DTW_NOVA=ON, or a client that links libnova_embed_ffi.a and defines TW_NOVA_ENGINE);
    /// slower than QuickJS, and it runs our bundles from source compiled once per process, never
    /// the `.qbc`. Where this build cannot run it, a Nova context fails closed: every evaluation
    /// and call throws, it never falls back to QuickJS.
    Nova,
};

/// The backend of engines and validators that do not name one: Native, unless
/// `WIDGET_SANDBOX_ENGINE=nova` is in the environment (read once).
QuickJSBackend quickjsDefaultBackend();

/// Whether this build can run Nova: the library is linked and enforces its interrupt, heap limit
/// and stack guard (`nova_embed_limits`). A Nova context on a build where this is false fails closed.
bool novaAvailable();
/// "nova_embed <version> (nova <commit>)", or "" when Nova is not in this build.
std::string novaDescription();

class QuickJSEngine;

class QuickJSContext {
public:
    using HostFunction = std::function<HostReturn(const std::vector<std::string>&)>;

    /// `maxStackBytes` must stay below the real stack of the thread that enters
    /// JS (JSThread: 8 MB). QuickJS only *detects* overflow against this limit; Nova
    /// takes three quarters of it for the native stack its JavaScript may use.
    explicit QuickJSContext(size_t memoryLimitBytes, size_t maxStackBytes = size_t(384) << 10,
                            QuickJSBackend backend = QuickJSBackend::Native);
    ~QuickJSContext();
    QuickJSContext(const QuickJSContext&) = delete;
    QuickJSContext& operator=(const QuickJSContext&) = delete;

    /// Interrupt deadline in `now()` seconds; +inf disables it.
    double deadline = std::numeric_limits<double>::infinity();
    /// Set by the interrupt handler when the deadline fired; reset by the caller.
    bool interrupted = false;
    /// Whether the error the last call threw is one script cannot catch (out of memory, the interrupt, any InternalError).
    bool uncatchable = false;

    /// Interrupt deadline in `threadCpuTime()` seconds of the thread running JS; +inf disables it.
    double cpuDeadline = std::numeric_limits<double>::infinity();

    QuickJSBackend backend() const { return backend_; }
    /// False when a Nova context could not be created (this build has no Nova, or its library does not enforce its
    /// limits): every evaluation and call then throws JSError with `unavailableReason()`.
    bool available() const { return unavailableReason_.empty(); }
    const std::string& unavailableReason() const { return unavailableReason_; }
    /// "native QuickJS", or Nova's version with the commit it was built from.
    std::string engineDescription() const;
    /// Whether this engine runs the precompiled `.qbc` bundles (QuickJS bytecode); Nova evaluates their source.
    bool runsBytecode() const { return backend_ == QuickJSBackend::Native; }

    /// What the engine's interrupt polls: true, and `interrupted` set, once a deadline has passed.
    bool pollInterrupt();

    /// Monotonic seconds (steady clock), the clock `deadline` is measured on.
    static double now();
    /// CPU seconds consumed by the calling thread; unaffected by how busy the machine is.
    static double threadCpuTime();

    size_t memoryUsed() const;

    /// Exposes `name(...)` to the sandbox. Arguments arrive as strings (numbers are formatted).
    void registerFunction(const std::string& name, HostFunction fn);

    JSResult evaluate(const std::string& code, const std::string& filename, bool asModule = false);
    /// Runs one of our own precompiled bundles (`.qbc`, see qjs_shim.h), checked against
    /// the source it was built from; throws JSError on any mismatch so the caller can
    /// fall back to `evaluate`. Never for widget code: bytecode is not verified.
    JSResult evaluateBytecode(const std::vector<uint8_t>& blob, const std::string& source);
    /// Evaluates one of our own bundles from its source. An engine that can share compiled code between its instances
    /// (Nova) compiles each bundle once per process and evaluates that: true. Otherwise as `evaluate`: false. Never for
    /// widget code.
    bool evaluateBundle(const std::string& source, const std::string& filename);
    /// Compiles `code` into the same container the build ships (tests).
    std::vector<uint8_t> compileBytecode(const std::string& code, const std::string& filename);
    /// Calls the global function `name` with string arguments.
    JSResult call(const std::string& name, const std::vector<std::string>& args = {});

    /// Keeps a private reference to the global function `name`; `call(name)` uses it from then on, even after the global is deleted.
    bool captureGlobalFunction(const std::string& name);

    bool hasPendingJob() const;
    /// Runs one queued promise job, if any (false: the queue was empty). An error thrown by the job goes to `onError`.
    bool executePendingJob(const std::function<void(const JSError&)>& onError);

private:
    QuickJSBackend backend_;
    std::string unavailableReason_;
    std::unique_ptr<QuickJSEngine> engine_;
};

} // namespace twidget
