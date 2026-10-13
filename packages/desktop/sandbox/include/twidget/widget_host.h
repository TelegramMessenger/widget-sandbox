// WidgetHost: one widget = one SandboxEngine (QuickJS on its own JSThread) +
// one TrustedValidator + one Renderer (whatever the factory builds — the
// Blitz prototype here, the Web App window on tdesktop). The only place where
// the three meet; nothing from the engine reaches the renderer without passing
// the validator. The C++ copy of src/host/widget-host.ts / WidgetHost.swift /
// WidgetHost.kt.
//
// Threading. A Qt-free library has no main thread, so the host owns a "host
// thread" that stands in for it: every renderer call, every diagnostics update
// and `onChange` / `onRenderOps` / `onBridgeCall` run there, in order. Engine
// callbacks hop from the engine thread; a layout query blocks the engine
// thread (≤ 3 s) until the renderer answers on the host thread. A Qt host
// would replace the thread with the UI thread (`crl::on_main`) one for one.
// `destroy()` is called from the owner's thread, never from the host thread.
#pragma once

#include "twidget/renderer.h"
#include "twidget/sandbox_engine.h"
#include "twidget/trusted_validator.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace twidget {

/// Limits for the widget's QuickJS runtime (see SandboxConfig); mirrors WidgetHost.Options on iOS/Android.
struct WidgetHostOptions {
    size_t memoryLimitBytes = size_t(64) << 20;
    double cpuLimitMs = 500;
    double cpuQuotaMs = 300;
    double cpuQuotaWindowMs = 1000;
    double cpuKillAfterMs = 0;
    int maxTimers = 256;
    /// Initial visibility; change it later with `setVisible`.
    bool visible = true;
    /// Device pixel ratio the engine boots with; the renderer's `size` report overrides it.
    double devicePixelRatio = 1;
};

struct WidgetDiagnostics {
    std::vector<std::pair<std::string, std::string>> logs;
    /// kind → (count, first details)
    std::map<std::string, std::pair<int, std::vector<std::string>>> diags;
    std::vector<std::pair<std::string, std::string>> bridge;
    SandboxStats stats;
    int nodes = 0;
    bool loaded = false;
    /// Wall clock from WidgetHost creation to the renderer's `ready` and to `loaded`.
    std::optional<double> rendererReadyMs;
    std::optional<double> loadedMs;
    /// `loaded`: the widget's scripts ran; `rendered`: the renderer applied every op sent up to that point.
    bool rendered = false;
    std::optional<double> renderedMs;
    /// Fatal: the runtime is gone. The renderer keeps the last state (frozen).
    std::optional<std::string> dead;
    /// Last uncaught widget error (non-fatal), cleared once a later task completes without one.
    std::optional<std::string> lastError;
    /// Document height in CSS px as the renderer reports it (clamped to [24, 900]).
    double height = 0;
    /// Wall clock to the first `size` report that follows validated ops.
    std::optional<double> firstContentMs;

    int countDiag(const std::string& kind) const { auto it = diags.find(kind); return it == diags.end() ? 0 : it->second.first; }
    bool hasLog(const std::string& text) const { for (auto& l : logs) if (l.second == text) return true; return false; }
};

class WidgetHost final : public SandboxEngineDelegate {
public:
    /// Throws JSError if the validator bundle fails to load. Nothing runs until `start()`.
    WidgetHost(std::string html, std::string theme, double width, WidgetHostOptions options, std::shared_ptr<SandboxResources> resources,
               RendererFactory rendererFactory, std::shared_ptr<IntlProvider> intl = nullptr);
    ~WidgetHost();
    WidgetHost(const WidgetHost&) = delete;
    WidgetHost& operator=(const WidgetHost&) = delete;

    /// Loads the renderer; the engine boots once the renderer announced itself (`ready`).
    void start();
    void setTheme(const std::string& theme);
    /// Off-screen pause: no animation frames, timers at 1 Hz, `document.hidden` inside the widget.
    void setVisible(bool visible);
    void bridgeEvent(const std::string& event, const std::string& dataJson = "null");
    /// A DOM event for the widget that did not come from the renderer (the harness's `--events`).
    void event(const std::string& json);
    /// Trusted diagnostic code evaluated in the widget context (tests / harness only).
    std::string probe(const std::string& code);
    /// Stops the engine and the renderer. Idempotent; not from the host thread.
    void destroy();

    /// The renderer the factory built; its own API (touches, paints) is the owner's business.
    Renderer& renderer() { return *renderer_; }
    /// Snapshot of the diagnostics (any thread).
    WidgetDiagnostics diagnostics() const;
    /// Runs `fn` on the host thread and waits for it (inline when already there).
    void onHostThread(const std::function<void()>& fn) { hostThread_.sync(fn); }

    bool waitLoaded(double timeoutSeconds = 20);
    /// True once the post-load ack query came back from the renderer (or the widget died).
    bool waitRendered(double timeoutSeconds = 5);
    /// Polls `pred` on a diagnostics snapshot until true or timeout.
    bool waitUntil(const std::function<bool(const WidgetDiagnostics&)>& pred, double timeoutSeconds = 3);

    // Observers, all on the host thread.
    std::function<void(const WidgetDiagnostics&)> onChange;
    /// Every validated batch on its way to the renderer (what the harness scans for leaks).
    std::function<void(const std::string& renderOpsJson)> onRenderOps;
    std::function<void(const std::string& method, const std::string& argsJson)> onBridgeCall;
    /// Non-fatal uncaught exception in widget code (also in `diagnostics().logs`).
    std::function<void(const std::string& text)> onError;
    /// Badge text shown by the renderer over a dead widget (user-facing, no error details).
    std::string stoppedLabel = "Widget stopped";

    // SandboxEngineDelegate (engine thread)
    void onOps(SandboxEngine&, const std::string& json) override;
    QueryAnswer onQuery(SandboxEngine&, const std::string& json) override;
    void onListen(SandboxEngine&, const std::string& json) override;
    void onPreventDefault(SandboxEngine&, const std::string& json) override;
    void onAnim(SandboxEngine&, const std::string& json) override;
    void onRequestFrame(SandboxEngine&) override;
    void onLog(SandboxEngine&, const std::string& level, const std::string& text) override;
    void onDiag(SandboxEngine&, const std::string& kind, const std::string& detail) override;
    void onBridge(SandboxEngine&, const std::string& method, const std::string& argsJson) override;
    void onLoaded(SandboxEngine&) override;
    void onStats(SandboxEngine&, const SandboxStats& stats) override;
    void onDied(SandboxEngine&, const std::string& reason) override;

private:
    struct PendingQuery {
        int id = 0;
        bool done = false;
        std::string json = "null";
        std::optional<std::vector<uint8_t>> binary;
    };

    void onRenderer(const std::string& json);
    void handleRendererMessage(const Json& msg);
    void toRenderer(const std::string& json);
    void resolveQuery(const std::string& json, std::optional<std::vector<uint8_t>> binary);
    void diag(const std::string& kind, const std::string& detail);
    void changed();
    double elapsedMs() const;
    /// Runs `fn` on the host thread (inline when already there).
    void host(std::function<void()> fn);

    /// qid of the host's own post-load ack query; engine queries count up from 1.
    static constexpr int kRenderAckQid = -1;

    JSThread hostThread_;
    std::string html_;
    std::string theme_;
    WidgetHostOptions options_;
    SandboxConfig config_;
    std::shared_ptr<SandboxResources> resources_;
    TrustedValidator validator_;
    std::unique_ptr<Renderer> renderer_;
    SandboxEngine engine_;

    mutable std::mutex mutex_;              // diagnostics_, waiters
    std::condition_variable cv_;
    WidgetDiagnostics diagnostics_;

    // Host-thread state.
    bool rendererReady_ = false;
    bool engineStarted_ = false;
    bool started_ = false;
    std::vector<std::string> pendingToRenderer_;
    double viewportW_, viewportH_, viewportDpr_;
    int qid_ = 0;
    int opsSent_ = 0;
    int tasksAtError_ = 0;

    std::atomic<bool> destroyed_{false};
    std::mutex queryMutex_;
    std::condition_variable queryCv_;
    std::shared_ptr<PendingQuery> pendingQuery_;
    const std::chrono::steady_clock::time_point createdAt_ = std::chrono::steady_clock::now();
};

} // namespace twidget
