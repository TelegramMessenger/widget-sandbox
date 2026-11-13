// Internal: the engine behind QuickJSContext. Two builds of the same QuickJS fork sit behind this
// interface — the native engine compiled into the process (quickjs_context.cpp, the default
// everywhere) and the same commit inside a wasm2c box (quickjs_boxed.cpp; docs/decisions.md,
// "QuickJS in a wasm2c box"). The rest of the core only ever sees QuickJSContext. Every method must
// be called from the thread/queue that owns the context.
#pragma once

#include "twidget/quickjs_context.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace twidget {

/// What QuickJSContext forwards to whichever engine runs the widget's code.
class QuickJSEngine {
public:
    virtual ~QuickJSEngine() = default;

    // The interrupt deadlines (QuickJSContext::deadline / cpuDeadline). A write takes effect at
    // once, also while a task is on the stack: the native engine reads them in its interrupt
    // handler, the boxed engine pushes them into the box's host layer (it checks them in C).
    virtual double deadline() const = 0;
    virtual void setDeadline(double seconds) = 0;
    virtual double cpuDeadline() const = 0;
    virtual void setCpuDeadline(double seconds) = 0;
    virtual bool interrupted() const = 0;
    virtual void setInterrupted(bool value) = 0;

    virtual size_t memoryUsed() const = 0;
    virtual void registerFunction(const std::string& name, QuickJSContext::HostFunction fn) = 0;
    virtual JSResult evaluate(const std::string& code, const std::string& filename, bool asModule) = 0;
    virtual JSResult evaluateBytecode(const std::vector<uint8_t>& blob, const std::string& source) = 0;
    virtual std::vector<uint8_t> compileBytecode(const std::string& code, const std::string& filename) = 0;
    virtual bool captureGlobalFunction(const std::string& name) = 0;
    virtual JSResult call(const std::string& name, const std::vector<std::string>& args) = 0;
    virtual bool hasPendingJob() const = 0;
    virtual void executePendingJobs(const std::function<void(const JSError&)>& onError) = 0;
};

/// The native engine (quickjs_context.cpp): quickjs-ng compiled into this process.
std::unique_ptr<QuickJSEngine> makeNativeQuickJS(size_t memoryLimitBytes, size_t maxStackBytes);

/// The boxed engine (quickjs_boxed.cpp): the same commit inside a wasm2c box. Returns nullptr and
/// sets `reason` when this build cannot run a box — not compiled in (no -DTW_QUICKJS_BOX=ON), built
/// without optimization, or the box did not start. The context then fails closed; it never falls
/// back to the native engine.
std::unique_ptr<QuickJSEngine> makeBoxedQuickJS(size_t memoryLimitBytes, size_t maxStackBytes, std::string& reason);
// quickjsBoxAvailable() and quickjsBoxDescription() are the boxed engine's public queries, declared
// in twidget/quickjs_context.h and defined in quickjs_boxed.cpp.

} // namespace twidget
