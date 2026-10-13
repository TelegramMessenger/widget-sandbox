// The sandbox engine: the C++ counterpart of src/host/worker.ts and of
// ios SandboxEngine.swift / android SandboxEngine.kt. Owns one QuickJS runtime
// per widget on its own JSThread, exposes exactly one capability to the
// untrusted code (`__host(kind, arg)` + `__hostBinary()`), enforces CPU and
// memory limits, and drives timers / frames / scripts.
//
// Everything that comes out of the context is UNTRUSTED: the delegate must
// pass it through TrustedValidator before it reaches any renderer.
#pragma once

#include "twidget/cpu_budget.h"
#include "twidget/intl.h"
#include "twidget/js_thread.h"
#include "twidget/quickjs_context.h"
#include "twidget/resources.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace twidget {

/// Mirrors `SandboxConfig` / `LIMIT_DEFAULTS` in src/shared/protocol.ts.
struct SandboxConfig {
    size_t memoryLimitBytes = size_t(64) << 20;
    /// Longest continuous run of widget JS in one task before the runtime is killed.
    double cpuLimitMs = 500;
    /// JS time the widget may use per `cpuQuotaWindowMs` (sliding window); beyond it timers and frames are deferred, never killed.
    double cpuQuotaMs = 300;
    double cpuQuotaWindowMs = 1000;
    /// Kill after this long of uninterrupted throttling; 0 disables (default).
    double cpuKillAfterMs = 0;
    /// Live timers a widget may hold; the shim throws RangeError beyond it, the engine refuses as a backstop.
    int maxTimers = 256;
    /// Initial visibility (see `setVisible`): hidden widgets get no frames and timers at 1 Hz.
    bool visible = true;
    double innerWidth = 400;
    double innerHeight = 300;
    double devicePixelRatio = 1;
    std::string theme = "light";
};

struct SandboxStats {
    double jsMs = 0;
    int tasks = 0;
    size_t memoryBytes = 0;
    int ops = 0;
    int queries = 0;
    double queryMs = 0;
    int queryCacheHits = 0;
    int missedMutations = 0;
    /// Tasks deferred by the CPU quota; JS time used in the current quota window at the time of the report.
    int throttledTasks = 0;
    double cpuWindowMs = 0;
};

struct QueryAnswer {
    std::string json;
    std::optional<std::vector<uint8_t>> binary;
};

class SandboxEngine;

/// All delegate calls happen on the engine's thread, never on the caller's.
/// `onQuery` is synchronous: it may block the engine thread (and must not block the UI thread).
class SandboxEngineDelegate {
public:
    virtual ~SandboxEngineDelegate() = default;
    virtual void onOps(SandboxEngine& engine, const std::string& json) = 0;
    virtual QueryAnswer onQuery(SandboxEngine& engine, const std::string& json) = 0;
    virtual void onListen(SandboxEngine& engine, const std::string& json) = 0;
    virtual void onPreventDefault(SandboxEngine& engine, const std::string& json) = 0;
    virtual void onRequestFrame(SandboxEngine& engine) = 0;
    virtual void onLog(SandboxEngine& engine, const std::string& level, const std::string& text) = 0;
    virtual void onDiag(SandboxEngine& engine, const std::string& kind, const std::string& detail) = 0;
    virtual void onBridge(SandboxEngine& engine, const std::string& method, const std::string& argsJson) = 0;
    virtual void onLoaded(SandboxEngine& engine) = 0;
    virtual void onStats(SandboxEngine& engine, const SandboxStats& stats) = 0;
    virtual void onDied(SandboxEngine& engine, const std::string& reason) = 0;
    virtual void onAnim(SandboxEngine&, const std::string&) {}
};

class SandboxEngine {
public:
    SandboxEngine(std::string html, SandboxConfig config, std::shared_ptr<SandboxResources> resources,
                  std::shared_ptr<IntlProvider> intl = nullptr);
    /// Stops the thread; outstanding delegate calls finish first. Safe from any thread but the engine's.
    ~SandboxEngine();
    SandboxEngine(const SandboxEngine&) = delete;
    SandboxEngine& operator=(const SandboxEngine&) = delete;

    /// Set before start(); the pointer must outlive the engine.
    void setDelegate(SandboxEngineDelegate* delegate) { delegate_ = delegate; }
    JSThread& thread() { return thread_; }

    void start();
    void terminate();

    // Inputs from the host (all asynchronous).
    void event(const std::string& json);
    /// A real user input event from the renderer (press, click, key press); every event of one physical gesture carries the same increasing `gesture` number.
    void activationEvent(const std::string& json, int gesture);
    void frame(double time);
    void viewport(double innerWidth, double innerHeight, double dpr);
    void resize(const std::string& entriesJson);
    void theme(const std::string& theme);
    void bridgeEvent(const std::string& event, const std::string& dataJson);
    /// The host knows whether the widget is on screen; the sandbox does not.
    void setVisible(bool visible);

    /// Evaluates trusted diagnostic code in the widget context (tests only). Returns the string value or the error text.
    std::string probe(const std::string& code);

private:
    struct Deferred { std::string kind; std::string arg; };

    void boot();
    void enqueue(const std::string& kind, const std::string& arg);
    bool dispatch(const std::string& kind, const std::string& arg);
    void deferTask(const std::string& kind, const std::string& arg);
    void scheduleWake(double atSeconds);
    void wake();
    void onTimerFired(int id);
    HostReturn hostCall(const std::string& kind, const std::string& arg);
    std::string syncQuery(const std::string& json);
    void kill(const std::string& reason);
    void handleError(const std::string& text, const std::string& where);
    void drainJobs();
    std::optional<std::string> callTw(const std::string& kind, const std::string& arg);
    void settle();
    void evalScript(const std::string& code, const std::string& filename, bool asModule = false);
    /// One of our own bundles: the precompiled `.qbc` when shipped and matching the source,
    /// else the source. Returns "bytecode" or "source" for the diag.
    std::string evalBundle(const std::string& source, const std::vector<uint8_t>& bytecode, const std::string& filename);
    void pumpScripts();
    void resetDeadline();
    void task(const std::string& kind, double sliceMs, const std::function<void()>& body);
    void postStats(bool force);
    static double nowMs();
    static double nowSeconds();

    JSThread thread_;
    SandboxEngineDelegate* delegate_ = nullptr;
    std::string html_;
    SandboxConfig config_;
    std::shared_ptr<SandboxResources> resources_;
    std::shared_ptr<IntlProvider> intl_;
    std::unique_ptr<QuickJSContext> js_;
    bool dead_ = false;
    bool started_ = false;
    SandboxStats stats_;
    std::map<int, std::shared_ptr<JSThread::Scheduled>> timers_;
    std::optional<std::vector<uint8_t>> lastBinary_;
    double lastStatsPost_ = 0;
    double lastMemCheck_ = 0;
    bool timerLimitReported_ = false;
    static constexpr double kMaxTimerDelaySeconds = 86400; // MAX_TIMER_DELAY_MS

    // Visibility and the deferred-task queue (mirrors src/host/worker.ts).
    bool visible_;
    bool frameWanted_ = false;
    std::vector<Deferred> deferred_;
    std::shared_ptr<JSThread::Scheduled> wakeTimer_;
    double wakeAt_ = 0;
    double hiddenTickAt_ = 0;

    // CPU quota: decide before a task, record after it.
    CpuBudget budget_;
    double currentSliceMs_ = 0;
    std::string currentKind_ = "boot";
    double excludeMs_ = 0;
    double lastThrottleDiag_ = -1e300;
    std::map<std::string, int> throttledSinceDiag_;
    std::vector<std::pair<std::string, std::string>> pendingBeforeBoot_;
    std::shared_ptr<JSThread::Scheduled> trailingStats_;

    // User activation: the gesture whose event task is running (0 outside one); one gesture unlocks one privileged bridge call.
    int activeGesture_ = 0;
    int consumedGesture_ = 0;
    int blockedBridgeReported_ = 0;
};

} // namespace twidget
