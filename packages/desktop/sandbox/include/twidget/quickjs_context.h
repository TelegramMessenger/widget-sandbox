// One QuickJS runtime and context behind one interface, whichever build of the engine runs it: the
// native one compiled into the process (the default everywhere) or the same fork commit inside a
// wasm2c box (docs/decisions.md, "QuickJS in a wasm2c box"). quickjs.h's JSValue macros are
// compound literals, not C++, so the native path goes through the same qjs_shim.c that iOS and
// Android use. One runtime, one context, string-based host functions. All methods must be called
// from the JSThread that owns the context.
#pragma once

#include <cstdint>
#include <cstdlib>
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
    enum class Kind { Undefined, Null, String, Number, ArrayBuffer };
    Kind kind = Kind::Undefined;
    std::string str;
    double num = 0;
    std::vector<uint8_t> bytes;

    static HostReturn undefined() { return {}; }
    static HostReturn null() { HostReturn r; r.kind = Kind::Null; return r; }
    static HostReturn string(std::string s) { HostReturn r; r.kind = Kind::String; r.str = std::move(s); return r; }
    static HostReturn number(double d) { HostReturn r; r.kind = Kind::Number; r.num = d; return r; }
    static HostReturn arrayBuffer(std::vector<uint8_t> b) { HostReturn r; r.kind = Kind::ArrayBuffer; r.bytes = std::move(b); return r; }
};

/// Result of calling a sandbox function from the host: string, number or nothing.
struct JSResult {
    std::optional<std::string> string;
    std::optional<double> number;
};

/// Which build of QuickJS runs a widget's code.
enum class QuickJSBackend {
    /// The engine compiled into the process. The default on every platform.
    Native,
    /// The same engine commit compiled to wasm and back to C, in a memory of its own: a memory bug
    /// in the engine reads and writes that memory only, or ends the process. Opt-in
    /// (-DTW_QUICKJS_BOX=ON); slower (JS about 1.4x with guard pages, 1.7x with bounds checks). The
    /// validator's engine stays native. Where this build cannot run it, a boxed context fails
    /// closed — every evaluation and call throws, it never falls back to the native engine.
    Boxed,
};

/// The backend of engines that do not name one: Native, unless `WIDGET_SANDBOX_ENGINE=boxed` is in
/// the environment (read once, as the Swift package's `QuickJSBackend.default` does). Direct
/// QuickJSContext users — the trusted validator above all — pass Native explicitly and are
/// unaffected; only the widget engines that take this default follow it.
QuickJSBackend quickjsDefaultBackend();

/// Whether this build can run a boxed engine (the box is compiled in — -DTW_QUICKJS_BOX=ON — and
/// optimized). A Boxed context on a build where this is false fails closed.
bool quickjsBoxAvailable();
/// "boxed QuickJS <commit> (<guard-pages|bounds-checks>)", or "" when the box is not in this build.
std::string quickjsBoxDescription();

class QuickJSEngine;

class QuickJSContext {
public:
    using HostFunction = std::function<HostReturn(const std::vector<std::string>&)>;

    /// `maxStackBytes` must stay below the real stack of the thread that enters JS (JSThread: 8 MB).
    /// The native engine only *detects* overflow against this limit; the boxed engine derives its
    /// own, smaller limit from it (the box's code uses several times more native stack than the
    /// stack QuickJS measures there).
    explicit QuickJSContext(size_t memoryLimitBytes, size_t maxStackBytes = size_t(384) << 10,
                            QuickJSBackend backend = QuickJSBackend::Native);
    ~QuickJSContext();
    QuickJSContext(const QuickJSContext&) = delete;
    QuickJSContext& operator=(const QuickJSContext&) = delete;

    /// Interrupt deadline in `now()` seconds; +inf disables it. A write takes effect at once, also
    /// while a task runs (the boxed engine checks it in its host layer, in C).
    double deadline() const;
    void setDeadline(double seconds);
    /// Interrupt deadline in `threadCpuTime()` seconds of the thread running JS; +inf disables it.
    double cpuDeadline() const;
    void setCpuDeadline(double seconds);
    /// Set by the interrupt handler when the deadline fired; reset by the caller.
    bool interrupted() const;
    void setInterrupted(bool value);

    /// Native, or boxed; and, when boxed, whether this build could actually give it one.
    QuickJSBackend backend() const { return backend_; }
    /// False when a boxed context could not be created (this build has no box, or it is unoptimized):
    /// every evaluation and call then throws JSError with `unavailableReason()`.
    bool available() const { return unavailableReason_.empty(); }
    const std::string& unavailableReason() const { return unavailableReason_; }
    /// "native QuickJS", or the boxed engine with the commit it was built from and how its memory is fenced.
    std::string engineDescription() const;

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
    /// Compiles `code` into the same container the build ships (tests).
    std::vector<uint8_t> compileBytecode(const std::string& code, const std::string& filename);
    /// Calls the global function `name` with string arguments.
    JSResult call(const std::string& name, const std::vector<std::string>& args = {});

    /// Keeps a private reference to the global function `name`; `call(name)` uses it from then on, even after the global is deleted.
    bool captureGlobalFunction(const std::string& name);

    bool hasPendingJob() const;
    /// Runs queued promise jobs. Errors thrown by jobs go to `onError` and do not stop the loop.
    void executePendingJobs(const std::function<void(const JSError&)>& onError);

private:
    QuickJSBackend backend_;
    std::string unavailableReason_;
    std::unique_ptr<QuickJSEngine> engine_;
};

} // namespace twidget
