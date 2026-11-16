// What every WidgetHost runs (twidget/widget_host.h): the hosted constructor and the destructor, the inputs, the delegate calls both modes take, the hosted client's reports, the keep and HostedKeptRenderer. The local mode's halves (widget_host_local.cpp) are reached only through `glue_`, which the local and served constructors alone install, so a program whose hosts are all hosted ones links neither them, the validator nor QuickJS.
#include "twidget/widget_host.h"

#include "twidget/json.h"
#include "widget_host_internal.h"

#include <cstdint>
#include <stdexcept>

namespace twidget {

using detail::kBacklogLimitReason;
using detail::makeConfig;
using detail::normalized;

namespace {

/// Where a diagnostic of a new kind past WidgetHost::kMaxDiagKinds is counted.
constexpr const char* kDiagKindDropped = "diag-kind-dropped";

} // namespace

// The deprecated observers are members: their implicit construction and destruction here use nothing of them.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
WidgetHost::WidgetHost(std::string html, WidgetTheme theme, double width, WidgetHostOptions options, std::shared_ptr<SandboxHostedChannel> hosted, std::shared_ptr<IntlProvider> intl)
    : hostThread_("twidget-host"),
      html_(std::move(html)),
      theme_(theme.colorScheme == "dark" ? "dark" : "light"),
      widgetTheme_(normalized(std::move(theme))),
      config_(makeConfig(options, width, widgetTheme_)),
      hosting_{true, nullptr},
      validator_(nullptr, nullptr),
      engine_(SandboxEngine::hostedClient(html_, config_, std::move(hosted), std::move(intl))),
      viewportW_(width), viewportH_(300), viewportDpr_(options.devicePixelRatio) {}

WidgetHost::~WidgetHost() {
    destroy();
    // No delegate calls once the engine thread (a client's dispatch thread) is gone; the host thread drains while every member lives. A kept client is the handle's.
    if (engine_) engine_->thread().join();
    hostThread_.join();
}

#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

void WidgetHost::start() {
    host([this] {
        if (started_ || destroyed_) return;
        started_ = true;
        // On the client's dispatch thread before the engine's start below: what it kept (a death) comes now, nothing came before.
        if (engine_->isRemote()) engine_->setDelegate(this);
        // The engine process loads its renderer and boots the widget; validated batches only for an observer set by now.
        if (hosting_.hosted) return engine_->startHosted(SandboxHostedStart{widgetTheme_, stoppedLabel, bool(onRenderOps)}, keepsRenderer);
        (this->*glue_->start)();
    });
}

void WidgetHost::host(std::function<void()> fn, SandboxEngine::RemoteHold hold) {
    if (hostThread_.isCurrent()) fn();
    else if (!hold) hostThread_.async(std::move(fn));
    else hostThread_.async([fn = std::move(fn), hold = std::move(hold)] { fn(); });
}

void WidgetHost::engineWork(std::function<void()> fn, SandboxEngine::RemoteHold hold) {
    host([this, fn = std::move(fn)] { if (!engineCut_ && !rendererOnly_) fn(); }, std::move(hold));
}

double WidgetHost::elapsedMs() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - createdAt_).count();
}

void WidgetHost::setTheme(const WidgetTheme& theme) {
    // The host owns the theme: stylesheets are re-sanitised against the scheme (prefers-color-scheme,
    // color-scheme, light-dark()), the renderer gets the variables, the sandbox the scheme and the palette.
    host([this, theme = normalized(theme)] {
        if (destroyed_) return;
        theme_ = theme.colorScheme;
        widgetTheme_ = theme;
        if (hosting_.hosted) return engine_->hostedTheme(widgetTheme_);
        (this->*glue_->setTheme)();
    });
}

void WidgetHost::setContentSafeArea(const WidgetContentSafeArea& area) {
    host([this, area] {
        if (destroyed_ || rendererOnly_) return;
        config_.contentSafeArea = area;
        if (glue_) (this->*glue_->safeArea)(area);
        engine_->contentSafeArea(area);
    });
}

// After a keep the client is the handle's: the owner's inputs find none (`start`, `setTheme` and `setContentSafeArea` stop at `destroyed_` on the host thread first).
void WidgetHost::setVisible(bool visible) {
    if (engine_) engine_->setVisible(visible);
}
void WidgetHost::setData(const WidgetData& data) {
    if (engine_) engine_->setData(data);
}
void WidgetHost::event(const std::string& json) {
    if (engine_) engine_->event(json);
}
std::string WidgetHost::probe(const std::string& code) { return engine_ ? engine_->probe(code) : "ERROR no runtime"; }

Renderer& WidgetHost::renderer() {
    if (!renderer_) throw std::logic_error("WidgetHost: a hosted widget's renderer is the engine process's");
    return *renderer_;
}

void WidgetHost::sendPlatform(std::string message) {
    if (hosting_.hosted && engine_) engine_->platform(std::move(message));
}

void WidgetHost::pressDelivered() {
    if (hosting_.hosted && engine_) engine_->pressDelivered();
}

void WidgetHost::releaseDelivered() {
    if (hosting_.hosted && engine_) engine_->releaseDelivered();
}

void WidgetHost::destroy() {
    if (destroyed_.exchange(true)) return;
    // Unblock an engine thread waiting for a renderer answer before the engine is stopped; a hosted client's waiting platform request answers none as its route closes.
    if (glue_) (this->*glue_->destroying)();
    engine_->terminate();
    hostThread_.sync([this] {
        if (glue_) (this->*glue_->destroyRenderer)();
        callbacksClosed_ = true;
    });
}

WidgetDiagnostics WidgetHost::diagnostics() const {
    std::lock_guard<std::mutex> l(mutex_);
    return diagnostics_;
}

bool WidgetHost::waitLoaded(double timeoutSeconds) {
    std::unique_lock<std::mutex> l(mutex_);
    return cv_.wait_for(l, std::chrono::duration<double>(timeoutSeconds), [&] { return diagnostics_.loaded || diagnostics_.dead.has_value(); });
}

bool WidgetHost::waitRendered(double timeoutSeconds) {
    std::unique_lock<std::mutex> l(mutex_);
    return cv_.wait_for(l, std::chrono::duration<double>(timeoutSeconds), [&] { return diagnostics_.rendered || diagnostics_.dead.has_value(); });
}

bool WidgetHost::waitUntil(const std::function<bool(const WidgetDiagnostics&)>& pred, double timeoutSeconds) {
    std::unique_lock<std::mutex> l(mutex_);
    return cv_.wait_for(l, std::chrono::duration<double>(timeoutSeconds), [&] { return pred(diagnostics_); });
}

// ───────────── engine → host (engine thread) ─────────────

// The render calls are the local glue's alone (`localOps` and the rest): a hosted client never makes them, and a hosted host has no glue.
void WidgetHost::onOps(SandboxEngine& engine, const std::string& json) {
    if (glue_) (this->*glue_->ops)(engine, json);
}

QueryAnswer WidgetHost::onQuery(SandboxEngine& engine, const std::string& json) {
    if (glue_) return (this->*glue_->query)(engine, json);
    return {"{\"error\":\"dead\"}", std::nullopt};
}

void WidgetHost::onListen(SandboxEngine& engine, const std::string& json) {
    if (glue_) (this->*glue_->listen)(engine, json);
}

void WidgetHost::onPreventDefault(SandboxEngine& engine, const std::string& json) {
    if (glue_) (this->*glue_->preventDefault)(engine, json);
}

void WidgetHost::onAnim(SandboxEngine& engine, const std::string& json) {
    if (glue_) (this->*glue_->anim)(engine, json);
}

void WidgetHost::onRequestFrame(SandboxEngine& engine) {
    if (glue_) (this->*glue_->requestFrame)(engine);
}

void WidgetHost::onLog(SandboxEngine& engine, const std::string& level, const std::string& text) {
    engineWork([this, level, text] {
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (diagnostics_.logs.size() < 5000) diagnostics_.logs.emplace_back(level, text);
            if (level == "error" && text.rfind("Uncaught ", 0) == 0) {
                diagnostics_.lastError = text;
                tasksAtError_ = diagnostics_.stats.tasks;
            }
        }
        if (level == "error" && text.rfind("Uncaught ", 0) == 0 && onError) onError(text);
        changed();
    }, engine.remoteHold());
}

void WidgetHost::onDiag(SandboxEngine& engine, const std::string& kind, const std::string& detail) {
    engineWork([this, kind, detail] { diag(kind, detail); }, engine.remoteHold());
}

/// Kept in the diagnostics; a widget can call ready() or play haptics for as long as it lives.
static constexpr size_t kMaxRecordedCalls = 1000;

void WidgetHost::onBridgeAction(SandboxEngine& engine, const WidgetAction& action) {
    engineWork([this, action] {
        if (destroyed_) return; // no link dialog for a widget that is going away
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (diagnostics_.actions.size() < kMaxRecordedCalls) diagnostics_.actions.push_back(action);
        }
        if (onAction) onAction(action);
        changed();
    }, engine.remoteHold());
}

void WidgetHost::onResizeAnchor(SandboxEngine& engine, const std::string& edge) {
    // Not an action: the host keeps the latest edge next to the height (docs/sdk-v1.md §2.12). The engine calls this
    // during the task, before that task's onOps, and both hop to the host thread in order; the renderer's size report
    // for those ops comes after them, so the anchor is in the state before the height it anchors. An equal value
    // changes nothing (no onChange).
    engineWork([this, edge] {
        if (destroyed_) return;
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (diagnostics_.resizeAnchor == edge) return;
            diagnostics_.resizeAnchor = edge;
        }
        changed();
    }, engine.remoteHold());
}

void WidgetHost::onPublicDataWatch(SandboxEngine& engine, bool on) {
    // Like the resize anchor: host state, reported on a change only (the shim already sends only changes).
    engineWork([this, on] {
        if (destroyed_) return;
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (diagnostics_.watchesPublicData == on) return;
            diagnostics_.watchesPublicData = on;
        }
        if (onWatchPublicData) onWatchPublicData(on);
        changed();
    }, engine.remoteHold());
}

void WidgetHost::onStorage(SandboxEngine& engine, const std::string& area, const StorageArea& values) {
    engineWork([this, area, values] {
        if (callbacksClosed_) return;
        {
            std::lock_guard<std::mutex> l(mutex_);
            diagnostics_.storageCommits++;
        }
        if (onStorageCommit) onStorageCommit(area, values);
        changed();
    }, engine.remoteHold());
}

void WidgetHost::onBridgeRequest(SandboxEngine& engine, const WidgetRequest& request, WidgetRequestResponder respond) {
    // The client side is decided here, at delivery, on the host thread where `onRequest` lives.
    engineWork([this, request, respond = std::move(respond)] {
        if (destroyed_) return; // the engine is going away: nothing waits for the answer
        if (!onRequest) return respond.unsupported();
        // `respond` is the engine's: from any thread, once, ignored once the engine is dead or destroyed.
        onRequest(request, respond);
    }, engine.remoteHold());
}

void WidgetHost::onBridgeWallet(SandboxEngine& engine, const WalletCall& call, WidgetRequestResponder respond) {
    // As onBridgeRequest: the client side is decided at delivery, on the host thread where `onWallet` lives.
    engineWork([this, call, respond = std::move(respond)] {
        if (destroyed_) return;
        if (!onWallet) return respond.unsupported();
        onWallet(call, respond);
    }, engine.remoteHold());
}

void WidgetHost::onBridgeCopyText(SandboxEngine& engine, const std::string& text) {
    engineWork([this, text] {
        if (destroyed_) return; // no toast for a widget that is going away
        if (onCopyText) onCopyText(text);
    }, engine.remoteHold());
}

void WidgetHost::onBridge(SandboxEngine& engine, const std::string& method, const std::string& argsJson) {
    // A second copy of the call, next to its action's.
    std::optional<SandboxEngine::RemoteHold> hold = engine.remoteHold(method.size() + argsJson.size());
    if (!hold) return;
    engineWork([this, method, argsJson] {
        if (destroyed_) return;
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (diagnostics_.bridge.size() < kMaxRecordedCalls) diagnostics_.bridge.emplace_back(method, argsJson);
        }
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        if (onBridgeCall) onBridgeCall(method, argsJson);
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
        changed();
    }, std::move(*hold));
}

void WidgetHost::onLoaded(SandboxEngine& engine) {
    if (glue_) (this->*glue_->loaded)();
    double loadedMs = elapsedMs();
    // Through the host thread after the ops of the loading tasks; then one trusted query to the renderer:
    // its answer comes after every size report those ops produced.
    engineWork([this, loadedMs] {
        {
            std::lock_guard<std::mutex> l(mutex_);
            diagnostics_.loaded = true;
            diagnostics_.loadedMs = loadedMs;
        }
        changed();
        if (glue_) (this->*glue_->renderAck)();
    }, engine.remoteHold());
}

void WidgetHost::onStats(SandboxEngine& engine, const SandboxStats& stats) {
    engineWork([this, stats] {
        {
            std::lock_guard<std::mutex> l(mutex_);
            diagnostics_.stats = stats;
            // Widened: an engine in another process may report any count, `INT_MAX` too.
            if (diagnostics_.lastError && int64_t(stats.tasks) > int64_t(tasksAtError_) + 1) diagnostics_.lastError.reset();
        }
        changed();
    }, engine.remoteHold());
}

void WidgetHost::onDied(SandboxEngine& engine, const std::string& reason) {
    host([this, reason] {
        // After the cut a death that won the race on the engine's thread (a "terminated" the engine sent, a disconnect) is the cut's.
        const std::string effective = engineCut_ ? kBacklogLimitReason : reason;
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (diagnostics_.dead) return;
            diagnostics_.dead = effective;
        }
        changed();
        if (glue_) (this->*glue_->died)(effective);
    }, engine.remoteHold());
}

void WidgetHost::diag(const std::string& kind, const std::string& detail) {
    {
        std::lock_guard<std::mutex> l(mutex_);
        auto& diags = diagnostics_.diags;
        auto it = diags.find(kind);
        bool folded = false;
        if (it == diags.end()) {
            // A new kind leaves the last place to the overflow kind until that kind is there; the overflow kind takes it.
            const size_t reserved = (kind == kDiagKindDropped || diags.count(kDiagKindDropped)) ? 0 : 1;
            folded = diags.size() + reserved >= kMaxDiagKinds;
            it = diags.try_emplace(folded ? std::string(kDiagKindDropped) : kind).first;
        }
        auto& entry = it->second;
        entry.first++;
        if (entry.second.size() < 50) entry.second.push_back(folded ? kind + ": " + detail : detail);
    }
    if (reports_.diag) reports_.diag(kind, detail);
    changed();
}

void WidgetHost::changed() {
    cv_.notify_all();
    if (onChange) onChange(diagnostics());
}

// ───────────── a hosted client's reports (its dispatch thread) ─────────────

void WidgetHost::onHostedMessage(SandboxEngine& engine, const std::string& message) {
    engineWork([this, message] {
        if (!platformClosed() && onPlatformMessage) onPlatformMessage(message);
    }, engine.remoteHold());
}

void WidgetHost::onHostedOps(SandboxEngine& engine, const std::string& json) {
    engineWork([this, json] {
        if (!destroyed_ && onRenderOps) onRenderOps(json);
    }, engine.remoteHold());
}

void WidgetHost::onHostedRendererReady(SandboxEngine& engine) {
    engineWork([this] {
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (diagnostics_.rendererReadyMs) return;
            diagnostics_.rendererReadyMs = elapsedMs();
        }
        changed();
    }, engine.remoteHold());
}

void WidgetHost::onHostedRendered(SandboxEngine& engine) {
    engineWork([this] {
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (diagnostics_.rendered) return;
            diagnostics_.rendered = true;
            diagnostics_.renderedMs = elapsedMs();
        }
        changed();
    }, engine.remoteHold());
}

void WidgetHost::onHostedNodes(SandboxEngine& engine, int nodes) {
    engineWork([this, nodes] {
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (diagnostics_.nodes == nodes) return;
            diagnostics_.nodes = nodes;
        }
        changed();
    }, engine.remoteHold());
}

// The client waits for the answer on the request's own thread: this only hands it to `onPlatformRequest` on the host thread, whose `respond` holds only the request. One that comes once the platform observers are closed is left to the route's close (`destroy()`, a keep that keeps nothing), so it ends the widget as that close does.
void WidgetHost::onHostedRequest(SandboxEngine& engine, const std::string& message, HostedRequestResponder respond) {
    host([this, message, respond] {
        if (platformClosed()) return;
        if (!onPlatformRequest) return respond.unanswered();
        onPlatformRequest(message, [respond](std::string answer) { respond(std::move(answer)); });
    }, engine.remoteHold());
}

// The client goes on the host thread, after everything it handed this host before the switch (the platform traffic among it, `keeping_`), so each platform message reaches exactly one observer, this host's first.
std::unique_ptr<HostedKeptRenderer> WidgetHost::keepRenderer(HostedKeptRenderer::Observers observers) {
    if (!hosting_.hosted) {
        destroy();
        return nullptr;
    }
    keeping_ = true;
    if (destroyed_.exchange(true)) return nullptr;
    std::unique_ptr<HostedKeptRenderer> kept(new HostedKeptRenderer(std::move(observers)));
    if (!engine_->keepHosted(kept->delegate())) {
        // Nothing to keep: the widget ends as destroy() ends it; the handle opens its gate before it joins its thread.
        engine_->terminate();
        hostThread_.sync([this] { callbacksClosed_ = true; });
        return nullptr;
    }
    hostThread_.sync([&] {
        callbacksClosed_ = true;
        kept->adopt(std::move(engine_));
    });
    return kept;
}

// ───────────── a kept renderer (HostedKeptRenderer) ─────────────

/// Its platform traffic to the observers on the handle's thread, in order, each queued call keeping its message counted; every other call is dropped (the client delivers none of them past the keep but a layout query's refusal).
class HostedKeptRenderer::Delegate final : public SandboxEngineDelegate {
public:
    explicit Delegate(HostedKeptRenderer& kept) : kept_(kept) {}

    void onOps(SandboxEngine&, const std::string&) override {}
    QueryAnswer onQuery(SandboxEngine&, const std::string&) override { return {"{\"error\":\"dead\"}", std::nullopt}; }
    void onListen(SandboxEngine&, const std::string&) override {}
    void onPreventDefault(SandboxEngine&, const std::string&) override {}
    void onRequestFrame(SandboxEngine&) override {}
    void onLog(SandboxEngine&, const std::string&, const std::string&) override {}
    void onDiag(SandboxEngine&, const std::string&, const std::string&) override {}
    void onLoaded(SandboxEngine&) override {}
    void onStats(SandboxEngine&, const SandboxStats&) override {}
    void onDied(SandboxEngine&, const std::string&) override {}

    void onHostedMessage(SandboxEngine& engine, const std::string& message) override {
        kept_.thread_.async([&kept = kept_, message, hold = engine.remoteHold()] {
            if (!kept.released_ && kept.observers_.onPlatformMessage) kept.observers_.onPlatformMessage(message);
        });
    }
    void onHostedRequest(SandboxEngine& engine, const std::string& message, HostedRequestResponder respond) override {
        kept_.thread_.async([&kept = kept_, message, respond, hold = engine.remoteHold()] {
            if (kept.released_ || !kept.observers_.onPlatformRequest) return respond.unanswered();
            kept.observers_.onPlatformRequest(message, [respond](std::string answer) { respond(std::move(answer)); });
        });
    }
    void onHostedEnded(SandboxEngine&, const std::string& reason) override {
        kept_.ended_ = true;
        kept_.thread_.async([&kept = kept_, reason] {
            if (!kept.released_ && kept.observers_.onEnded) kept.observers_.onEnded(reason);
        });
    }

private:
    HostedKeptRenderer& kept_;
};

HostedKeptRenderer::HostedKeptRenderer(Observers observers) : thread_("twidget-host"), observers_(std::move(observers)), delegate_(std::make_unique<Delegate>(*this)) {
    thread_.async([gate = gate_.get_future().share()] { gate.wait(); });
}

HostedKeptRenderer::~HostedKeptRenderer() { release(); }

SandboxEngineDelegate& HostedKeptRenderer::delegate() { return *delegate_; }

void HostedKeptRenderer::adopt(std::unique_ptr<SandboxEngine> client) {
    client_ = std::move(client);
    openGate();
}

void HostedKeptRenderer::openGate() {
    if (std::exchange(gateOpen_, true)) return;
    gate_.set_value();
}

void HostedKeptRenderer::sendPlatform(std::string message) {
    if (client_) client_->platform(std::move(message));
}

SandboxEngine::RemoteBacklog HostedKeptRenderer::remoteBacklog() const { return client_ ? client_->remoteBacklog() : SandboxEngine::RemoteBacklog{}; }

// The client first (its dispatch thread joined, the engine process told by its destructor), so nothing is queued here after the join.
void HostedKeptRenderer::release() {
    if (released_.exchange(true)) return;
    client_.reset();
    openGate();
    thread_.join();
}

bool GestureCounter::isUserActivation(const Json& ev) {
    if (!ev.isObject()) return false;
    const std::string& type = ev["type"].asString();
    // Pointer presses only: widgets get no keyboard (docs/decisions.md, "Profile v1").
    static const char* types[] = {"click", "dblclick", "mousedown", "mouseup", "pointerdown", "pointerup"};
    bool known = false;
    for (const char* t : types) known = known || type == t;
    if (!known) return false;
    // A click made by a pointer has a click count; the click Enter or Space activates has detail 0.
    if (type == "click" || type == "dblclick") return ev["detail"].asNumber() > 0;
    return true;
}

int GestureCounter::gestureFor(const Json& ev) {
    if (!ev.isObject()) return 0;
    const std::string& type = ev["type"].asString();
    bool continues = false;
    if (type == "mousedown") continues = lastType_ == "pointerdown";
    else if (type == "pointerup" || type == "mouseup" || type == "click") continues = pressOpen_;
    else if (type == "dblclick") continues = lastType_ == "click";
    else if (type != "pointerdown") return 0;
    if (!continues && !isUserActivation(ev)) {
        // A keyboard click ends any chain: a dblclick after it is not the pointer's.
        pressOpen_ = false;
        lastType_.clear();
        return 0;
    }
    if (!continues) number_++;
    pressOpen_ = type != "click" && type != "dblclick";
    lastType_ = type;
    return number_;
}

} // namespace twidget
