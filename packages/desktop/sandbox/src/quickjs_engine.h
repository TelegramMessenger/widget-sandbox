// Internal: the engine behind QuickJSContext. Two engines sit behind this interface: quickjs-ng
// compiled into the process (quickjs_context.cpp, the default) and Nova behind its C interface
// (nova_engine.cpp). The rest of the core only ever sees QuickJSContext. Every method must be
// called from the thread that owns the context.
#pragma once

#include "twidget/quickjs_context.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace twidget {

/// What QuickJSContext forwards to whichever engine runs the code. The deadlines and the
/// `interrupted` / `uncatchable` flags stay in the owning QuickJSContext: an engine polls
/// `owner.pollInterrupt()` and sets `owner.uncatchable` when it reports an error.
class QuickJSEngine {
public:
    virtual ~QuickJSEngine() = default;

    virtual std::string description() const = 0;
    virtual size_t memoryUsed() const = 0;
    virtual void registerFunction(const std::string& name, QuickJSContext::HostFunction fn) = 0;
    virtual JSResult evaluate(const std::string& code, const std::string& filename, bool asModule) = 0;
    /// True when the bundle ran from code compiled once per process; false when it was evaluated as `evaluate` does.
    virtual bool evaluateBundle(const std::string& source, const std::string& filename) = 0;
    virtual JSResult evaluateBytecode(const std::vector<uint8_t>& blob, const std::string& source) = 0;
    virtual std::vector<uint8_t> compileBytecode(const std::string& code, const std::string& filename) = 0;
    virtual bool captureGlobalFunction(const std::string& name) = 0;
    virtual JSResult call(const std::string& name, const std::vector<std::string>& args) = 0;
    virtual bool hasPendingJob() const = 0;
    virtual bool executePendingJob(const std::function<void(const JSError&)>& onError) = 0;
};

/// The native engine (quickjs_context.cpp): quickjs-ng compiled into this process. Not in a build with
/// TW_NO_QUICKJS.
std::unique_ptr<QuickJSEngine> makeNativeQuickJS(QuickJSContext& owner, size_t memoryLimitBytes, size_t maxStackBytes);

/// Nova (nova_engine.cpp). Returns nullptr and sets `reason` when this build cannot run it: not
/// linked (no TW_NOVA_ENGINE), a library that does not enforce its limits, or an engine that could
/// not be created. The context then fails closed; it never falls back to QuickJS.
std::unique_ptr<QuickJSEngine> makeNovaEngine(QuickJSContext& owner, size_t memoryLimitBytes, size_t maxStackBytes, std::string& reason);
// novaAvailable() and novaDescription() are Nova's public queries, declared in
// twidget/quickjs_context.h and defined in nova_engine.cpp.

} // namespace twidget
