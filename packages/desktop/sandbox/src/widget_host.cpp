#include "twidget/widget_host.h"

#include "twidget/json.h"

#include <algorithm>
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

SandboxConfig makeConfig(const WidgetHostOptions& o, double width, const std::string& theme) {
    SandboxConfig c;
    c.memoryLimitBytes = o.memoryLimitBytes;
    c.cpuLimitMs = o.cpuLimitMs;
    c.cpuQuotaMs = o.cpuQuotaMs;
    c.cpuQuotaWindowMs = o.cpuQuotaWindowMs;
    c.cpuKillAfterMs = o.cpuKillAfterMs;
    c.maxTimers = o.maxTimers;
    c.visible = o.visible;
    c.innerWidth = width;
    c.innerHeight = 300;
    c.devicePixelRatio = o.devicePixelRatio;
    c.theme = theme;
    return c;
}

constexpr double kMinHeight = 24, kMaxHeight = 900;

} // namespace

WidgetHost::WidgetHost(std::string html, std::string theme, double width, WidgetHostOptions options, std::shared_ptr<SandboxResources> resources,
                       RendererFactory rendererFactory, std::shared_ptr<IntlProvider> intl)
    : hostThread_("twidget-host"),
      html_(std::move(html)),
      theme_(std::move(theme)),
      options_(options),
      config_(makeConfig(options, width, theme_)),
      resources_(std::move(resources)),
      validator_(theme_, resources_, [this](const std::string& k, const std::string& d) { host([this, k, d] { diag(k, d); }); }),
      renderer_(rendererFactory(resources_, theme_, width)),
      engine_(html_, config_, resources_, std::move(intl)),
      viewportW_(width), viewportH_(300), viewportDpr_(options.devicePixelRatio) {
    if (!renderer_) throw std::runtime_error("WidgetHost: the renderer factory returned nothing");
    engine_.setDelegate(this);
    renderer_->onDiag = [this](const std::string& k, const std::string& d) { host([this, k, d] { diag(k, d); }); };
    // The widget boots once the renderer announced itself, so the very first layout queries have something to measure.
    renderer_->onMessage = [this](const std::string& json) { onRenderer(json); };
}

WidgetHost::~WidgetHost() {
    destroy();
    // No more delegate calls once the engine thread is gone; then the host thread drains its queue
    // while every member is still alive, and only then do the members go.
    engine_.thread().join();
    hostThread_.join();
}

void WidgetHost::start() {
    host([this] {
        if (started_ || destroyed_) return;
        started_ = true;
        renderer_->load();
    });
}

void WidgetHost::host(std::function<void()> fn) {
    if (hostThread_.isCurrent()) fn();
    else hostThread_.async(std::move(fn));
}

double WidgetHost::elapsedMs() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - createdAt_).count();
}

void WidgetHost::setTheme(const std::string& theme) {
    host([this, theme] {
        if (destroyed_) return;
        theme_ = theme;
        renderer_->setTheme(theme);
        toRenderer("{\"type\":\"ops\",\"ops\":" + validator_.setTheme(theme) + "}");
        engine_.theme(theme);
    });
}

void WidgetHost::setVisible(bool visible) { engine_.setVisible(visible); }
void WidgetHost::bridgeEvent(const std::string& event, const std::string& dataJson) { engine_.bridgeEvent(event, dataJson); }
void WidgetHost::event(const std::string& json) { engine_.event(json); }
std::string WidgetHost::probe(const std::string& code) { return engine_.probe(code); }

void WidgetHost::destroy() {
    if (destroyed_.exchange(true)) return;
    // Unblock an engine thread waiting for a renderer answer before the engine is stopped.
    resolveQuery("{\"error\":\"dead\"}", std::nullopt);
    engine_.terminate();
    hostThread_.sync([this] { renderer_->destroy(); });
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

void WidgetHost::onOps(SandboxEngine&, const std::string& json) {
    // Validation on the validator's thread (synchronous from here), the renderer call on the host thread.
    std::string clean = validator_.processOps(json);
    int nodes = validator_.nodeCount();
    host([this, clean = std::move(clean), nodes] {
        if (destroyed_) return;
        if (clean != "[]") {
            toRenderer("{\"type\":\"ops\",\"ops\":" + clean + "}");
            opsSent_++;
            if (onRenderOps) onRenderOps(clean);
        }
        std::lock_guard<std::mutex> l(mutex_);
        diagnostics_.nodes = nodes;
    });
}

QueryAnswer WidgetHost::onQuery(SandboxEngine&, const std::string& json) {
    if (destroyed_) return {"{\"error\":\"dead\"}", std::nullopt};
    auto pending = std::make_shared<PendingQuery>();
    {
        std::lock_guard<std::mutex> l(queryMutex_);
        pendingQuery_ = pending;
    }
    std::optional<std::string> clean = validator_.validateQuery(json);
    if (clean && clean->find("\"matchMedia\"") != std::string::npos) {
        if (auto q = Json::parse(*clean); q && (*q)["query"].isString()) {
            Json rewritten = *q;
            rewritten.set("query", validator_.rewriteMediaQuery((*q)["query"].asString()));
            clean = rewritten.dump();
        }
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
    });
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

void WidgetHost::onListen(SandboxEngine&, const std::string& json) {
    std::string types = validator_.validateListen(json);
    host([this, types] { toRenderer("{\"type\":\"listen\",\"types\":" + types + "}"); });
}

void WidgetHost::onPreventDefault(SandboxEngine&, const std::string& json) {
    std::string keys = validator_.validatePreventDefault(json);
    host([this, keys] { toRenderer("{\"type\":\"preventDefault\",\"keys\":" + keys + "}"); });
}

void WidgetHost::onRequestFrame(SandboxEngine&) {
    host([this] { toRenderer("{\"type\":\"requestFrame\"}"); });
}

void WidgetHost::onLog(SandboxEngine&, const std::string& level, const std::string& text) {
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
    });
}

void WidgetHost::onDiag(SandboxEngine&, const std::string& kind, const std::string& detail) {
    host([this, kind, detail] { diag(kind, detail); });
}

void WidgetHost::onBridge(SandboxEngine&, const std::string& method, const std::string& argsJson) {
    host([this, method, argsJson] {
        {
            std::lock_guard<std::mutex> l(mutex_);
            diagnostics_.bridge.emplace_back(method, argsJson);
        }
        if (onBridgeCall) onBridgeCall(method, argsJson);
        changed();
    });
}

void WidgetHost::onLoaded(SandboxEngine&) {
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
    });
}

void WidgetHost::onStats(SandboxEngine&, const SandboxStats& stats) {
    host([this, stats] {
        {
            std::lock_guard<std::mutex> l(mutex_);
            diagnostics_.stats = stats;
            if (diagnostics_.lastError && stats.tasks > tasksAtError_ + 1) diagnostics_.lastError.reset();
        }
        changed();
    });
}

void WidgetHost::onDied(SandboxEngine&, const std::string& reason) {
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
    });
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
            engine_.start();
        }
    } else if (type == "event") {
        if (const Json* ev = msg.find("ev")) engine_.event(ev->dump());
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
            engine_.viewport(w, h, dpr);
        }
    } else if (type == "resize") {
        if (const Json* entries = msg.find("entries")) engine_.resize(entries->dump());
    } else if (type == "frame") {
        engine_.frame(msg["time"].asNumber(0));
    } else if (type == "media") {
        engine_.theme(theme_);
    } else if (type == "violation") {
        diag("renderer-violation", msg["detail"].asString());
    }
    // "gesture": the scroll-vs-widget decision of a list; no list here.
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

} // namespace twidget
