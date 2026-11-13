#include "twidget/widget_host.h"

#include "twidget/json.h"
#include "twidget/quickjs_context.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace twidget {

namespace {

/// Small base64 decoder for `binaryBase64` in a renderer result; nullopt on malformed input.
std::optional<std::vector<uint8_t>> base64Decode(const std::string& text) {
    static const auto value = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(text.size() * 3 / 4);
    unsigned acc = 0;
    int bits = 0;
    for (unsigned char c : text) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        int v = value(c);
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | unsigned(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t((acc >> bits) & 0xFF));
        }
    }
    return out;
}

/// The theme as the host keeps it: "light" unless exactly "dark" (toWidgetTheme in sdk-policy.ts), the palette checked.
WidgetTheme normalized(WidgetTheme t) {
    t.colorScheme = t.colorScheme == "dark" ? "dark" : "light";
    t.params = sdk::checkThemeParams(t.params);
    return t;
}

SandboxConfig makeConfig(const WidgetHostOptions& o, double width, const WidgetTheme& theme) {
    SandboxConfig c;
    c.memoryLimitBytes = o.memoryLimitBytes;
    c.cpuLimitMs = o.cpuLimitMs;
    c.cpuQuotaMs = o.cpuQuotaMs;
    c.cpuQuotaWindowMs = o.cpuQuotaWindowMs;
    c.cpuKillAfterMs = o.cpuKillAfterMs;
    c.mainThreadQuotaMs = o.mainThreadQuotaMs;
    c.maxTimers = o.maxTimers;
    c.visible = o.visible;
    c.innerWidth = width;
    c.innerHeight = 300;
    c.devicePixelRatio = o.devicePixelRatio;
    c.theme = theme.colorScheme;
    c.themeParams = theme.params;
    c.locale = o.locale;
    c.contentSafeArea = o.contentSafeArea;
    c.storage = o.storage;
    c.data = o.data;
    c.context = o.context;
    c.policy = o.policy;
    c.features = o.features;
    return c;
}

// The shared bubble height bounds of v0.1.0 (docs/integration.md, "Bubble sizing").
constexpr double kMinHeight = 50, kMaxHeight = 600;

/// A hold on `engine`'s message for work that keeps `kept` bytes made of its `given`: what goes past them is counted as work of its own; nullopt when that does not fit.
std::optional<SandboxEngine::RemoteHold> keeping(SandboxEngine& engine, size_t kept, size_t given) {
    if (kept <= given) return engine.remoteHold();
    return engine.remoteHold(kept - given);
}

} // namespace

WidgetHost::WidgetHost(std::string html, WidgetTheme theme, double width, WidgetHostOptions options, std::shared_ptr<SandboxResources> resources,
                       RendererFactory rendererFactory, std::shared_ptr<IntlProvider> intl)
    : hostThread_("twidget-host"),
      html_(std::move(html)),
      theme_(theme.colorScheme == "dark" ? "dark" : "light"),
      widgetTheme_(normalized(std::move(theme))),
      options_(options),
      config_(makeConfig(options, width, widgetTheme_)),
      resources_(std::move(resources)),
      validator_(theme_, resources_, [this](const std::string& k, const std::string& d) { validatorDiag(k, d); }),
      renderer_(rendererFactory(resources_, theme_, width)),
      engine_(SandboxEngine::make(html_, config_, resources_, std::move(intl))),
      viewportW_(width), viewportH_(300), viewportDpr_(options.devicePixelRatio) {
    if (!renderer_) throw std::runtime_error("WidgetHost: the renderer factory returned nothing");
    if (isStrictPolicy(options.policy)) validator_.setPolicy("strict");
    // The validator's time on the widget's messages counts against its JS quota (docs/decisions.md, "The validator's time counts").
    validator_.setCpuSink([this](double ms) { engine_->chargeValidator(ms); });
    // A client-mode engine may have died already (no channel, a lost connection): it gets this host in start(), after the owner set onChange and stoppedLabel.
    if (!engine_->isRemote()) engine_->setDelegate(this);
    renderer_->onDiag = [this](const std::string& k, const std::string& d) { host([this, k, d] { diag(k, d); }); };
    // The widget boots once the renderer announced itself, so the very first layout queries have something to measure.
    renderer_->onMessage = [this](const std::string& json) { onRenderer(json); };
}

WidgetHost::~WidgetHost() {
    destroy();
    // No delegate calls once the engine thread (a client's dispatch thread) is gone; the host thread drains while every member lives.
    engine_->thread().join();
    hostThread_.join();
    validator_.setCpuSink(nullptr);
}

void WidgetHost::start() {
    host([this] {
        if (started_ || destroyed_) return;
        started_ = true;
        // On the client's dispatch thread before the engine's start below: what it kept (a death) comes now, nothing came before.
        if (engine_->isRemote()) engine_->setDelegate(this);
        // Queued until the renderer is ready, so the variables are its first message, before any ops.
        toRenderer(themeMessage());
        toRenderer(safeAreaMessage(config_.contentSafeArea));
        // Under strict the renderer counts heavy boxes and reports `limit` past them (docs/decisions.md "Heavy boxes"); compat counts nothing.
        toRenderer(std::string("{\"type\":\"policy\",\"policy\":\"") + (isStrictPolicy(options_.policy) ? "strict" : "compat") + "\"}");
        renderer_->load();
    });
}

std::string WidgetHost::themeMessage() const {
    const WidgetButtonTokens& b = widgetTheme_.button;
    Json button = Json::object();
    auto text = [&](const char* name, const std::optional<std::string>& v) { if (v) button.set(name, *v); };
    auto number = [&](const char* name, const std::optional<double>& v) { if (v) button.set(name, *v); };
    text("bg", b.bg);
    text("text", b.text);
    text("radius", b.radius);
    text("font", b.font);
    text("padding", b.padding);
    text("height", b.height);
    number("pressedOpacity", b.pressedOpacity);
    number("disabledOpacity", b.disabledOpacity);
    const WidgetTextTokens& x = widgetTheme_.text;
    Json text_tokens = Json::object();
    if (x.fontSize && std::isfinite(*x.fontSize)) text_tokens.set("fontSize", *x.fontSize);
    if (x.lineHeight && std::isfinite(*x.lineHeight)) text_tokens.set("lineHeight", *x.lineHeight);
    if (x.padding) text_tokens.set("padding", *x.padding);
    Json theme = Json::object();
    theme.set("colorScheme", widgetTheme_.colorScheme);
    theme.set("params", sdk::themeParamsJson(widgetTheme_.params));
    theme.set("button", std::move(button));
    theme.set("text", std::move(text_tokens));
    Json msg = Json::object();
    msg.set("type", "theme");
    msg.set("theme", std::move(theme));
    return msg.dump();
}

void WidgetHost::host(std::function<void()> fn, SandboxEngine::RemoteHold hold) {
    if (hostThread_.isCurrent()) fn();
    else if (!hold) hostThread_.async(std::move(fn));
    else hostThread_.async([fn = std::move(fn), hold = std::move(hold)] { fn(); });
}

void WidgetHost::validatorDiag(const std::string& kind, const std::string& detail) {
    SandboxEngine::RemoteHold hold;
    {
        // Held while the engine is read: the engine's thread clears `validating_` under it before it leaves the delegate call.
        std::lock_guard<std::mutex> l(validatingMutex_);
        if (validating_) {
            std::optional<SandboxEngine::RemoteHold> counted = validating_->remoteHold(kind.size() + detail.size());
            if (!counted) return;
            hold = std::move(*counted);
        }
    }
    host([this, kind, detail] { diag(kind, detail); }, std::move(hold));
}

template <class F>
auto WidgetHost::validated(SandboxEngine& engine, F validate) {
    {
        std::lock_guard<std::mutex> l(validatingMutex_);
        validating_ = &engine;
    }
    auto result = validate();
    std::lock_guard<std::mutex> l(validatingMutex_);
    validating_ = nullptr;
    return result;
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
        renderer_->setTheme(theme_);
        toRenderer(themeMessage());
        toRenderer("{\"type\":\"ops\",\"ops\":" + validator_.setTheme(theme_) + "}");
        engine_->theme(theme_, widgetTheme_.params);
    });
}

void WidgetHost::setContentSafeArea(const WidgetContentSafeArea& area) {
    host([this, area] {
        if (destroyed_) return;
        config_.contentSafeArea = area;
        if (started_) toRenderer(safeAreaMessage(area));
        engine_->contentSafeArea(area);
    });
}

/// `{"type":"safeArea","inset":{…}}` (docs/sdk-v1.md §3.4): the renderer core checks the values and sets the variables.
std::string WidgetHost::safeAreaMessage(const WidgetContentSafeArea& area) {
    Json msg = Json::object();
    msg.set("type", "safeArea");
    msg.set("inset", area.json());
    return msg.dump();
}

void WidgetHost::setVisible(bool visible) { engine_->setVisible(visible); }
void WidgetHost::setData(const WidgetData& data) { engine_->setData(data); }
void WidgetHost::event(const std::string& json) { engine_->event(json); }
std::string WidgetHost::probe(const std::string& code) { return engine_->probe(code); }

void WidgetHost::destroy() {
    if (destroyed_.exchange(true)) return;
    // Unblock an engine thread waiting for a renderer answer before the engine is stopped.
    resolveQuery("{\"error\":\"dead\"}", std::nullopt);
    engine_->terminate();
    hostThread_.sync([this] {
        renderer_->destroy();
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

void WidgetHost::onOps(SandboxEngine& engine, const std::string& json) {
    // Validation on the validator's thread (synchronous from here), the renderer call on the host thread.
    std::string clean = validated(engine, [&] { return validator_.processOps(json); });
    // A counted cap passed under strict (the validator's `limit`): the widget stops as it does for memory or CPU
    // (onDied: the reason in the diagnostics, the renderer frozen on its last render; this batch came back empty).
    if (std::optional<std::string> reason = validator_.stopped()) return engine.stop(*reason);
    int nodes = validator_.nodeCount();
    std::optional<SandboxEngine::RemoteHold> hold = keeping(engine, clean.size(), json.size());
    if (!hold) return;
    host([this, clean = std::move(clean), nodes] {
        if (destroyed_) return;
        if (clean != "[]") {
            toRenderer("{\"type\":\"ops\",\"ops\":" + clean + "}");
            opsSent_++;
            if (onRenderOps) onRenderOps(clean);
        }
        std::lock_guard<std::mutex> l(mutex_);
        diagnostics_.nodes = nodes;
    }, std::move(*hold));
}

QueryAnswer WidgetHost::onQuery(SandboxEngine& engine, const std::string& json) {
    if (destroyed_) return {"{\"error\":\"dead\"}", std::nullopt};
    auto pending = std::make_shared<PendingQuery>();
    {
        std::lock_guard<std::mutex> l(queryMutex_);
        pendingQuery_ = pending;
    }
    std::optional<std::string> clean = validated(engine, [&] {
        std::optional<std::string> valid = validator_.validateQuery(json);
        if (valid && valid->find("\"matchMedia\"") != std::string::npos) {
            if (auto q = Json::parse(*valid); q && (*q)["query"].isString()) {
                Json rewritten = *q;
                rewritten.set("query", validator_.rewriteMediaQuery((*q)["query"].asString()));
                valid = rewritten.dump();
            }
        }
        return valid;
    });
    std::optional<SandboxEngine::RemoteHold> hold = keeping(engine, clean ? clean->size() : 0, json.size());
    if (!hold) {
        std::lock_guard<std::mutex> l(queryMutex_);
        if (pendingQuery_ == pending) pendingQuery_.reset();
        return {"{\"error\":\"dead\"}", std::nullopt};
    }
    host([this, pending, clean] {
        if (destroyed_) return resolveQuery("{\"error\":\"dead\"}", std::nullopt);
        if (!clean) return resolveQuery("{\"error\":\"invalid query\"}", std::nullopt);
        qid_++;
        {
            std::lock_guard<std::mutex> l(queryMutex_);
            pending->id = qid_;
        }
        toRenderer("{\"type\":\"query\",\"qid\":" + std::to_string(qid_) + ",\"query\":" + *clean + "}");
    }, std::move(*hold));
    std::unique_lock<std::mutex> l(queryMutex_);
    if (!queryCv_.wait_for(l, std::chrono::seconds(3), [&] { return pending->done; })) {
        if (pendingQuery_ == pending) pendingQuery_.reset();
        return {"{\"error\":\"timeout\"}", std::nullopt};
    }
    return {pending->json, pending->binary};
}

void WidgetHost::resolveQuery(const std::string& json, std::optional<std::vector<uint8_t>> binary) {
    std::lock_guard<std::mutex> l(queryMutex_);
    auto p = std::move(pendingQuery_);
    pendingQuery_.reset();
    if (!p) return;
    p->json = json;
    p->binary = std::move(binary);
    p->done = true;
    queryCv_.notify_all();
}

void WidgetHost::onListen(SandboxEngine& engine, const std::string& json) {
    std::string types = validated(engine, [&] { return validator_.validateListen(json); });
    std::optional<SandboxEngine::RemoteHold> hold = keeping(engine, types.size(), json.size());
    if (!hold) return;
    host([this, types] { toRenderer("{\"type\":\"listen\",\"types\":" + types + "}"); }, std::move(*hold));
}

void WidgetHost::onPreventDefault(SandboxEngine& engine, const std::string& json) {
    std::string keys = validated(engine, [&] { return validator_.validatePreventDefault(json); });
    std::optional<SandboxEngine::RemoteHold> hold = keeping(engine, keys.size(), json.size());
    if (!hold) return;
    host([this, keys] { toRenderer("{\"type\":\"preventDefault\",\"keys\":" + keys + "}"); }, std::move(*hold));
}

void WidgetHost::onAnim(SandboxEngine& engine, const std::string& json) {
    std::string anims = validated(engine, [&] { return validator_.validateAnim(json); });
    if (anims == "[]") return;
    std::optional<SandboxEngine::RemoteHold> hold = keeping(engine, anims.size(), json.size());
    if (!hold) return;
    host([this, anims] {
        char now[64];
        std::snprintf(now, sizeof now, "%.17g", QuickJSContext::now() * 1000);
        toRenderer(std::string("{\"type\":\"anim\",\"now\":") + now + ",\"anims\":" + anims + "}");
    }, std::move(*hold));
}

void WidgetHost::onRequestFrame(SandboxEngine& engine) {
    host([this] { toRenderer("{\"type\":\"requestFrame\"}"); }, engine.remoteHold());
}

void WidgetHost::onLog(SandboxEngine& engine, const std::string& level, const std::string& text) {
    host([this, level, text] {
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
    host([this, kind, detail] { diag(kind, detail); }, engine.remoteHold());
}

/// Kept in the diagnostics; a widget can call ready() or play haptics for as long as it lives.
static constexpr size_t kMaxRecordedCalls = 1000;

void WidgetHost::onBridgeAction(SandboxEngine& engine, const WidgetAction& action) {
    host([this, action] {
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
    host([this, edge] {
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
    host([this, on] {
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
    host([this, area, values] {
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
    host([this, request, respond = std::move(respond)] {
        if (destroyed_) return; // the engine is going away: nothing waits for the answer
        if (!onRequest) return respond.unsupported();
        // `respond` is the engine's: from any thread, once, ignored once the engine is dead or destroyed.
        onRequest(request, respond);
    }, engine.remoteHold());
}

void WidgetHost::onBridgeWallet(SandboxEngine& engine, const WalletCall& call, WidgetRequestResponder respond) {
    // As onBridgeRequest: the client side is decided at delivery, on the host thread where `onWallet` lives.
    host([this, call, respond = std::move(respond)] {
        if (destroyed_) return;
        if (!onWallet) return respond.unsupported();
        onWallet(call, respond);
    }, engine.remoteHold());
}

void WidgetHost::onBridgeCopyText(SandboxEngine& engine, const std::string& text) {
    host([this, text] {
        if (destroyed_) return; // no toast for a widget that is going away
        if (onCopyText) onCopyText(text);
    }, engine.remoteHold());
}

void WidgetHost::onBridge(SandboxEngine& engine, const std::string& method, const std::string& argsJson) {
    // A second copy of the call, next to its action's.
    std::optional<SandboxEngine::RemoteHold> hold = engine.remoteHold(method.size() + argsJson.size());
    if (!hold) return;
    host([this, method, argsJson] {
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
    // On the engine thread after the loading tasks' ops were validated: text changes are rate-limited from here on.
    validator_.markLoaded();
    double loadedMs = elapsedMs();
    // Through the host thread after the ops of the loading tasks; then one trusted query to the renderer:
    // its answer comes after every size report those ops produced.
    host([this, loadedMs] {
        {
            std::lock_guard<std::mutex> l(mutex_);
            diagnostics_.loaded = true;
            diagnostics_.loadedMs = loadedMs;
        }
        changed();
        toRenderer("{\"type\":\"query\",\"qid\":" + std::to_string(kRenderAckQid) + ",\"query\":{\"q\":\"viewport\"}}");
    }, engine.remoteHold());
}

void WidgetHost::onStats(SandboxEngine& engine, const SandboxStats& stats) {
    host([this, stats] {
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
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (diagnostics_.dead) return;
            diagnostics_.dead = reason;
        }
        changed();
        resolveQuery("{\"error\":\"dead\"}", std::nullopt);
        // The renderer stays: last render as fallback, badge from the renderer, no more events or frames.
        if (reason != "terminated") {
            Json frozen = Json::object();
            frozen.set("type", "frozen");
            frozen.set("reason", stoppedLabel);
            frozen.set("fatal", true);
            toRenderer(frozen.dump());
        }
    }, engine.remoteHold());
}

// ───────────── renderer → host (host thread) ─────────────

void WidgetHost::onRenderer(const std::string& json) {
    host([this, json] {
        if (destroyed_) return;
        if (auto msg = Json::parse(json)) handleRendererMessage(*msg);
    });
}

void WidgetHost::handleRendererMessage(const Json& msg) {
    const std::string& type = msg["type"].asString();
    if (type == "ready") {
        if (rendererReady_) return;
        rendererReady_ = true;
        {
            std::lock_guard<std::mutex> l(mutex_);
            diagnostics_.rendererReadyMs = elapsedMs();
        }
        auto queued = std::move(pendingToRenderer_);
        pendingToRenderer_.clear();
        for (const auto& m : queued) renderer_->send(m);
        if (!engineStarted_) {
            engineStarted_ = true;
            engine_->start();
        }
    } else if (type == "event") {
        // A preview takes no input: a tap on it selects the result (the client's), and the widget gets nothing (§2.16).
        if (options_.context.preview) return;
        if (const Json* ev = msg.find("ev")) {
            // Any pointerdown begins a press, trusted or not: a second finger's is neither trusted nor claimed (`gesture`
            // is the primary pointer's), and without this its release continued the press before it.
            if ((*ev)["type"].isString() && (*ev)["type"].asString() == "pointerdown") gestures_.pressBegan();
            // Only input the renderer saw as real (the DOM event's isTrusted; every input event of a native renderer) is a gesture.
            if (int gesture = msg["trusted"].asBool() ? gestures_.gestureFor(*ev) : 0) engine_->activationEvent(ev->dump(), gesture);
            else engine_->event(ev->dump());
        }
    } else if (type == "result") {
        if (!msg["qid"].isNumber()) return;
        int id = msg["qid"].asInt();
        if (id == kRenderAckQid) {
            bool first = false;
            {
                std::lock_guard<std::mutex> l(mutex_);
                if (!diagnostics_.rendered) {
                    diagnostics_.rendered = true;
                    diagnostics_.renderedMs = elapsedMs();
                    first = true;
                }
            }
            if (first) changed();
            return;
        }
        {
            std::lock_guard<std::mutex> l(queryMutex_);
            if (!pendingQuery_ || pendingQuery_->id != id) return;
        }
        std::optional<std::vector<uint8_t>> binary;
        if (msg["binaryBase64"].isString()) binary = base64Decode(msg["binaryBase64"].asString());
        const Json* result = msg.find("result");
        resolveQuery(result ? result->dump() : "null", std::move(binary));
    } else if (type == "size") {
        double height = std::min(std::max(msg["height"].asNumber(0), kMinHeight), kMaxHeight);
        bool notify = false;
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (!diagnostics_.firstContentMs && opsSent_ > 0) {
                diagnostics_.firstContentMs = elapsedMs();
                notify = true;
            }
            if (height != diagnostics_.height) {
                diagnostics_.height = height;
                notify = true;
            }
        }
        if (notify) changed();
        double w = msg["innerWidth"].asNumber(viewportW_), h = msg["innerHeight"].asNumber(viewportH_), dpr = msg["dpr"].asNumber(viewportDpr_);
        if (w != viewportW_ || h != viewportH_ || dpr != viewportDpr_) {
            viewportW_ = w;
            viewportH_ = h;
            viewportDpr_ = dpr;
            engine_->viewport(w, h, dpr);
        }
    } else if (type == "resize") {
        if (const Json* entries = msg.find("entries")) engine_->resize(entries->dump());
    } else if (type == "frame") {
        engine_->frame(msg["time"].asNumber(0));
    } else if (type == "media") {
        // OS-level change inside the renderer: the host theme is authoritative, just re-notify.
        engine_->theme(theme_, widgetTheme_.params);
    } else if (type == "violation") {
        diag("renderer-violation", msg["detail"].asString());
    } else if (type == "limit") {
        // Past a paint limit (16 heavy boxes, 2 with backdrop-filter; Renderer::limitExceeded): the renderer drew the frame
        // without the effects, and the widget stops as for memory or CPU (onDied: the reason in the diagnostics, the
        // renderer frozen). compat counts nothing.
        std::optional<WidgetPaintLimit> limit = msg["code"].isString() ? paintLimitFromCode(msg["code"].asString()) : std::nullopt;
        double count = msg["count"].isNumber() ? msg["count"].asNumber() : -1;
        if (!limit || !std::isfinite(count) || count < 0) return diag("renderer-violation", "a limit report with an unknown code or no count");
        if (!isStrictPolicy(options_.policy)) return;
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (diagnostics_.dead) return;
        }
        std::string reason = paintLimitMessage(*limit, int(count));
        diag(paintLimitCode(*limit), reason);
        engine_->stop(reason);
    } else if (type == "release") {
        // The primary press ended (pointerup or pointercancel; renderer-made): the haptic gate's 5 s touch window starts.
        engine_->pressEnded();
    } else if (type == "gesture") {
        // A primary press began (renderer-made: the widget cannot fake it): haptics are free while it lasts, and 5 s
        // after. Before the preview check: harmless, a preview gets no haptics anyway.
        engine_->pressBegan();
        gestures_.pressBegan();
        if (options_.context.preview) return; // no input, so nothing to claim: the press is the client's
        // docs/sdk-v1.md §5.5, gesture_policy.h. The renderer core sends x and hold (and `y` / `wants`, always false
        // since v0.1.3); vertical drags are the chat's, so the claim's `y` is false whatever a renderer sends. A
        // renderer that sends only `wants` (the Blitz and litehtml prototypes) claims nothing. Anything missing or
        // not a boolean is false.
        WidgetGestureClaim claim;
        claim.x = msg["x"].asBool();
        claim.y = false;
        claim.hold = msg["hold"].asBool();
        if (onGesture) onGesture(claim);
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        if (onGestureClaim) onGestureClaim(claim.x, claim.y);
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
    }
}

void WidgetHost::toRenderer(const std::string& json) {
    if (destroyed_) return;
    if (rendererReady_) renderer_->send(json);
    else pendingToRenderer_.push_back(json);
}

void WidgetHost::diag(const std::string& kind, const std::string& detail) {
    {
        std::lock_guard<std::mutex> l(mutex_);
        auto& entry = diagnostics_.diags[kind];
        entry.first++;
        if (entry.second.size() < 50) entry.second.push_back(detail);
    }
    changed();
}

void WidgetHost::changed() {
    cv_.notify_all();
    if (onChange) onChange(diagnostics());
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
