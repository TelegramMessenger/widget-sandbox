// The local mode of WidgetHost and the served host (twidget/widget_host.h): their constructors (the validator, the renderer, the engine in process or behind `SandboxEngine::make`'s channel), the render glue between the engine, the validator and the renderer, and their halves of the calls both modes take, which widget_host.cpp reaches only through `kGlue`. A hosted host never installs that table, so a program whose hosts are all hosted ones links nothing of this file, the validator or the in-process engine.
#include "twidget/widget_host.h"

#include "twidget/json.h"
#include "twidget/quickjs_context.h"
#include "widget_host_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace twidget {

using detail::kBacklogLimitReason;
using detail::makeConfig;
using detail::normalized;
using detail::themed;

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

// The shared bubble height bounds of v0.1.0 (docs/integration.md, "Bubble sizing").
constexpr double kMinHeight = 50, kMaxHeight = 600;

/// The diagnostic of a client-mode engine whose renderer messages passed WidgetHost's backlog bounds (its death is `kBacklogLimitReason`).
constexpr const char* kBacklogLimit = "backlog-limit";

/// A hold on `engine`'s message for work that keeps `kept` bytes made of its `given`: what goes past them is counted as work of its own; nullopt when that does not fit.
std::optional<SandboxEngine::RemoteHold> keeping(SandboxEngine& engine, size_t kept, size_t given) {
    if (kept <= given) return engine.remoteHold();
    return engine.remoteHold(kept - given);
}

/// The hosted-mode hook (`WidgetHost::setHostedChannelFactory`).
struct HostedHook {
    std::mutex mutex;
    HostedChannelFactory factory;
};

HostedHook& hostedHook() {
    static HostedHook hook;
    return hook;
}

/// `WidgetHost::validator_`'s deleter: the CPU sink goes first, as the host's destructor cleared it before.
void freeValidator(TrustedValidator* validator) {
    validator->setCpuSink(nullptr);
    delete validator;
}

} // namespace

constinit const WidgetHost::Glue WidgetHost::kGlue = {
    &WidgetHost::localOps,
    &WidgetHost::localQuery,
    &WidgetHost::localListen,
    &WidgetHost::localPreventDefault,
    &WidgetHost::localAnim,
    &WidgetHost::localRequestFrame,
    &WidgetHost::localStart,
    &WidgetHost::localSetTheme,
    &WidgetHost::localSafeArea,
    &WidgetHost::localLoaded,
    &WidgetHost::localRenderAck,
    &WidgetHost::localDied,
    &WidgetHost::localDestroying,
    &WidgetHost::localDestroyRenderer,
};

// The deprecated observers are members: their implicit construction and destruction here use nothing of them.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
WidgetHost::WidgetHost(std::string html, WidgetTheme theme, double width, WidgetHostOptions options, std::shared_ptr<SandboxResources> resources,
                       RendererFactory rendererFactory, std::shared_ptr<IntlProvider> intl)
    : hostThread_("twidget-host"),
      html_(std::move(html)),
      theme_(theme.colorScheme == "dark" ? "dark" : "light"),
      widgetTheme_(normalized(std::move(theme))),
      config_(makeConfig(options, width, widgetTheme_)),
      resources_(std::move(resources)),
      hosting_(hostingFor(*this, resources_, rendererFactory, theme_, width)),
      glue_(hosting_.hosted ? nullptr : &kGlue),
      validator_(hosting_.hosted ? nullptr
                                 : new TrustedValidator(theme_, resources_, [this](const std::string& k, const std::string& d) { validatorDiag(k, d); }),
                 freeValidator),
      renderer_(hosting_.hosted ? nullptr : rendererFactory(resources_, theme_, width)),
      engine_(hosting_.hosted ? SandboxEngine::hostedClient(html_, config_, std::move(hosting_.channel), std::move(intl))
                              : SandboxEngine::make(html_, config_, resources_, std::move(intl))),
      viewportW_(width), viewportH_(300), viewportDpr_(options.devicePixelRatio) {
    if (!hosting_.hosted) wire(*this);
}

WidgetHost::WidgetHost(ServedTag, std::string html, WidgetTheme theme, SandboxConfig config, std::shared_ptr<SandboxResources> resources, RendererFactory renderers,
                       std::shared_ptr<IntlProvider> intl, SandboxEngineDelegate& engineDelegate, GlueReports reports)
    : reports_(std::move(reports)),
      hostThread_("twidget-host"),
      html_(std::move(html)),
      theme_(theme.colorScheme == "dark" ? "dark" : "light"),
      widgetTheme_(normalized(std::move(theme))),
      config_(themed(std::move(config), widgetTheme_)),
      resources_(std::move(resources)),
      glue_(&kGlue),
      validator_(new TrustedValidator(theme_, resources_, [this](const std::string& k, const std::string& d) { validatorDiag(k, d); }), freeValidator),
      renderer_(renderers(resources_, theme_, config_.innerWidth)),
      engine_(std::make_unique<SandboxEngine>(html_, config_, resources_, std::move(intl))),
      viewportW_(config_.innerWidth), viewportH_(config_.innerHeight), viewportDpr_(config_.devicePixelRatio) {
    wire(engineDelegate);
}

#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

WidgetHost::Hosting WidgetHost::hostingFor(WidgetHost& host, const std::shared_ptr<SandboxResources>& resources, const RendererFactory& renderers, const std::string& theme, double width) {
    HostedChannelFactory factory;
    {
        HostedHook& hook = hostedHook();
        std::lock_guard<std::mutex> l(hook.mutex);
        factory = hook.factory;
    }
    // Tests of the remote engine mode set their engine factory first: they keep testing that mode.
    if (!factory || SandboxEngine::remoteChannelFactorySet()) return {};
    return {true, factory(host, resources, renderers, theme, width)};
}

void WidgetHost::setHostedChannelFactory(HostedChannelFactory factory) {
    HostedHook& hook = hostedHook();
    std::lock_guard<std::mutex> l(hook.mutex);
    std::swap(hook.factory, factory);
}

void WidgetHost::wire(SandboxEngineDelegate& engineDelegate) {
    if (!renderer_) throw std::runtime_error("WidgetHost: the renderer factory returned nothing");
    if (isStrictPolicy(config_.policy)) validator_->setPolicy("strict");
    // The validator's time on the widget's messages counts against its JS quota (docs/decisions.md, "The validator's time counts").
    validator_->setCpuSink([this](double ms) { engine_->chargeValidator(ms); });
    // A client-mode engine may have died already (no channel, a lost connection): it gets this host in start(), after the owner set onChange and stoppedLabel.
    if (!engine_->isRemote()) engine_->setDelegate(&engineDelegate);
    renderer_->onDiag = [this](const std::string& k, const std::string& d) {
        host([this, k, d] {
            if (!rendererOnly_) diag(k, d);
        });
    };
    // The widget boots once the renderer announced itself, so the very first layout queries have something to measure.
    renderer_->onMessage = [this](const std::string& json) { onRenderer(json); };
}

void WidgetHost::localStart() {
    // Queued until the renderer is ready, so the variables are its first message, before any ops.
    toRenderer(themeMessage());
    toRenderer(safeAreaMessage(config_.contentSafeArea));
    // Under strict the renderer counts heavy boxes and reports `limit` past them (docs/decisions.md "Heavy boxes"); compat counts nothing.
    toRenderer(std::string("{\"type\":\"policy\",\"policy\":\"") + (isStrictPolicy(config_.policy) ? "strict" : "compat") + "\"}");
    renderer_->load();
}

std::string WidgetHost::themeMessage() const {
    Json theme = Json::object();
    theme.set("colorScheme", widgetTheme_.colorScheme);
    theme.set("params", sdk::themeParamsJson(widgetTheme_.params));
    theme.set("button", widgetTheme_.button.json());
    theme.set("text", widgetTheme_.text.json());
    Json msg = Json::object();
    msg.set("type", "theme");
    msg.set("theme", std::move(theme));
    return msg.dump();
}

void WidgetHost::validatorDiag(const std::string& kind, const std::string& detail) {
    SandboxEngine::RemoteHold hold;
    bool fromEngine = false;
    {
        // Held while the engine is read: the engine's thread clears `validating_` under it before it leaves the delegate call.
        std::lock_guard<std::mutex> l(validatingMutex_);
        if (validating_) {
            fromEngine = true;
            std::optional<SandboxEngine::RemoteHold> counted = validating_->remoteHold(kind.size() + detail.size());
            if (!counted) return;
            hold = std::move(*counted);
        }
    }
    std::function<void()> report = [this, kind, detail] { diag(kind, detail); };
    if (fromEngine) engineWork(std::move(report), std::move(hold));
    else host(std::move(report));
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

void WidgetHost::localSetTheme() {
    // A served host kept for its renderer alone has no validator or engine to re-theme.
    if (rendererOnly_) return;
    renderer_->setTheme(theme_);
    toRenderer(themeMessage());
    // Past the cut the renderer never gets the nodes the re-themed stylesheets would name.
    if (!engineCut_) {
        // A validated batch too: `onRenderOps` (the harness's leak scan) sees every batch the renderer gets.
        const std::string clean = validator_->setTheme(theme_);
        toRenderer("{\"type\":\"ops\",\"ops\":" + clean + "}");
        if (clean != "[]" && onRenderOps) onRenderOps(clean);
    }
    engine_->theme(theme_, widgetTheme_.params);
}

void WidgetHost::localSafeArea(const WidgetContentSafeArea& area) {
    if (started_) toRenderer(safeAreaMessage(area));
}

/// `{"type":"safeArea","inset":{…}}` (docs/sdk-v1.md §3.4): the renderer core checks the values and sets the variables.
std::string WidgetHost::safeAreaMessage(const WidgetContentSafeArea& area) {
    Json msg = Json::object();
    msg.set("type", "safeArea");
    msg.set("inset", area.json());
    return msg.dump();
}

void WidgetHost::localDestroying() {
    resolveQuery("{\"error\":\"dead\"}", std::nullopt);
}

void WidgetHost::localDestroyRenderer() {
    if (renderer_) renderer_->destroy();
}

// The engine is dead, so its thread never waits for this one again (no layout query after the death; one before it is answered here): the join cannot hang, and once it returned nothing calls the validator but this thread, which destroys it.
void WidgetHost::keepRendererOnly() {
    if (destroyed_ || rendererOnly_) return;
    rendererOnly_ = true;
    resolveQuery("{\"error\":\"dead\"}", std::nullopt);
    engine_->release();
    if (validator_) {
        validator_->setCpuSink(nullptr);
        validator_.reset();
    }
}

// ───────────── engine → host (engine thread) ─────────────

void WidgetHost::localOps(SandboxEngine& engine, const std::string& json) {
    // Validation on the validator's thread (synchronous from here), the renderer call on the host thread.
    std::string clean = validated(engine, [&] { return validator_->processOps(json); });
    // A counted cap passed under strict (the validator's `limit`): the widget stops as it does for memory or CPU
    // (onDied: the reason in the diagnostics, the renderer frozen on its last render; this batch came back empty).
    if (std::optional<std::string> reason = validator_->stopped()) return engine.stop(*reason);
    int nodes = validator_->nodeCount();
    std::optional<SandboxEngine::RemoteHold> hold = keeping(engine, clean.size(), json.size());
    if (!hold) return;
    engineWork([this, clean = std::move(clean), nodes] {
        if (destroyed_) return;
        if (clean != "[]") {
            if (!engineToRenderer("{\"type\":\"ops\",\"ops\":" + clean + "}")) return;
            opsSent_++;
            if (onRenderOps) onRenderOps(clean);
        }
        int before = 0;
        {
            std::lock_guard<std::mutex> l(mutex_);
            before = std::exchange(diagnostics_.nodes, nodes);
        }
        if (nodes != before && reports_.nodes) reports_.nodes(nodes);
    }, std::move(*hold));
}

QueryAnswer WidgetHost::localQuery(SandboxEngine& engine, const std::string& json) {
    if (destroyed_ || rendererOnly_) return {"{\"error\":\"dead\"}", std::nullopt};
    auto pending = std::make_shared<PendingQuery>();
    {
        std::lock_guard<std::mutex> l(queryMutex_);
        pendingQuery_ = pending;
    }
    std::optional<std::string> clean = validated(engine, [&] {
        std::optional<std::string> valid = validator_->validateQuery(json);
        if (valid && valid->find("\"matchMedia\"") != std::string::npos) {
            if (auto q = Json::parse(*valid); q && (*q)["query"].isString()) {
                Json rewritten = *q;
                rewritten.set("query", validator_->rewriteMediaQuery((*q)["query"].asString()));
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
    // 3 s of the glue's own time, read before the glue can run this query: a served host's glue thread waiting inside its renderer's platform requests from here on (one ahead of this query too) is the embedder's time, left out here and in the widget's query budget, however late this thread waits.
    const GlueWaited first = glueWaited();
    host([this, pending, clean] {
        if (destroyed_ || engineCut_ || rendererOnly_) return resolveQuery("{\"error\":\"dead\"}", std::nullopt);
        if (!clean) return resolveQuery("{\"error\":\"invalid query\"}", std::nullopt);
        qid_++;
        {
            std::lock_guard<std::mutex> l(queryMutex_);
            pending->id = qid_;
        }
        // Not kept: the cut, or destroy(), resolved this query and no longer waits for its qid.
        engineToRenderer("{\"type\":\"query\",\"qid\":" + std::to_string(qid_) + ",\"query\":" + *clean + "}");
    }, std::move(*hold));
    const auto platformSeconds = [&](std::chrono::steady_clock::duration total) {
        return std::chrono::duration<double>(std::max(total - first.total, std::chrono::steady_clock::duration::zero())).count();
    };
    std::unique_lock<std::mutex> l(queryMutex_);
    while (!pending->done) {
        const GlueWaited now = glueWaited();
        const auto deadline = first.at + std::chrono::seconds(3) + (now.total - first.total);
        if (now.at >= deadline) {
            if (pendingQuery_ == pending) pendingQuery_.reset();
            // Its 3 s of the glue's own time are charged, the platform waits in it are not, as for an answer.
            return {"{\"error\":\"timeout\"}", std::nullopt, platformSeconds(now.total)};
        }
        // A wait in progress moves the deadline with it: its end (`glueWaitEnded`) or the answer wakes this.
        if (now.waiting) queryCv_.wait(l);
        else queryCv_.wait_until(l, deadline);
    }
    // Up to the answer's own reading (`resolveQuery`), whenever this thread woke.
    return {pending->json, pending->binary, platformSeconds(pending->glueWaited)};
}

WidgetHost::GlueWaited WidgetHost::glueWaited() const { return glueWaits_ ? glueWaits_() : GlueWaited{std::chrono::steady_clock::now()}; }

void WidgetHost::glueWaitEnded() {
    // Under the query's lock, so a query between its reading and its wait cannot miss it.
    {
        std::lock_guard<std::mutex> l(queryMutex_);
    }
    queryCv_.notify_all();
}

void WidgetHost::resolveQuery(const std::string& json, std::optional<std::vector<uint8_t>> binary) {
    std::lock_guard<std::mutex> l(queryMutex_);
    auto p = std::move(pendingQuery_);
    pendingQuery_.reset();
    if (!p) return;
    p->json = json;
    p->binary = std::move(binary);
    p->glueWaited = glueWaited().total;
    p->done = true;
    queryCv_.notify_all();
}

void WidgetHost::localListen(SandboxEngine& engine, const std::string& json) {
    std::string types = validated(engine, [&] { return validator_->validateListen(json); });
    std::optional<SandboxEngine::RemoteHold> hold = keeping(engine, types.size(), json.size());
    if (!hold) return;
    engineWork([this, types] { engineToRenderer("{\"type\":\"listen\",\"types\":" + types + "}"); }, std::move(*hold));
}

void WidgetHost::localPreventDefault(SandboxEngine& engine, const std::string& json) {
    std::string keys = validated(engine, [&] { return validator_->validatePreventDefault(json); });
    std::optional<SandboxEngine::RemoteHold> hold = keeping(engine, keys.size(), json.size());
    if (!hold) return;
    engineWork([this, keys] { engineToRenderer("{\"type\":\"preventDefault\",\"keys\":" + keys + "}"); }, std::move(*hold));
}

void WidgetHost::localAnim(SandboxEngine& engine, const std::string& json) {
    std::string anims = validated(engine, [&] { return validator_->validateAnim(json); });
    if (anims == "[]") return;
    std::optional<SandboxEngine::RemoteHold> hold = keeping(engine, anims.size(), json.size());
    if (!hold) return;
    engineWork([this, anims] {
        char now[64];
        std::snprintf(now, sizeof now, "%.17g", QuickJSContext::now() * 1000);
        engineToRenderer(std::string("{\"type\":\"anim\",\"now\":") + now + ",\"anims\":" + anims + "}");
    }, std::move(*hold));
}

void WidgetHost::localRequestFrame(SandboxEngine& engine) {
    engineWork([this] { engineToRenderer("{\"type\":\"requestFrame\"}"); }, engine.remoteHold());
}

void WidgetHost::localLoaded() {
    // On the engine thread after the loading tasks' ops were validated: text changes are rate-limited from here on.
    if (validator_) validator_->markLoaded();
}

void WidgetHost::localRenderAck() {
    engineToRenderer("{\"type\":\"query\",\"qid\":" + std::to_string(kRenderAckQid) + ",\"query\":{\"q\":\"viewport\"}}");
}

void WidgetHost::localDied(const std::string& effective) {
    resolveQuery("{\"error\":\"dead\"}", std::nullopt);
    // The renderer stays: last render as fallback, badge from the renderer, no more events or frames.
    if (effective != "terminated") {
        Json frozen = Json::object();
        frozen.set("type", "frozen");
        frozen.set("reason", stoppedLabel);
        frozen.set("fatal", true);
        toRenderer(frozen.dump());
    }
}

// ───────────── renderer → host (host thread) ─────────────

void WidgetHost::onRenderer(const std::string& json) {
    host([this, json] {
        if (destroyed_ || rendererOnly_) return;
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
        if (reports_.rendererReady) reports_.rendererReady();
        std::vector<PendingMessage> queued = std::move(pendingToRenderer_);
        pendingToRenderer_.clear();
        backlogBytes_ = 0;
        backlogMessages_ = 0;
        for (const PendingMessage& m : queued) renderer_->send(m.json);
        // Never once cut: an observer of the cut's diagnostic may announce the renderer before the stop is posted.
        if (!engineStarted_ && !engineCut_) {
            engineStarted_ = true;
            engine_->start();
        }
    } else if (type == "event") {
        // A preview takes no input: a tap on it selects the result (the client's), and the widget gets nothing (§2.16).
        if (config_.context.preview) return;
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
            if (!first) return;
            changed();
            if (reports_.rendered) reports_.rendered();
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
        if (!isStrictPolicy(config_.policy)) return;
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
        if (config_.context.preview) return; // no input, so nothing to claim: the press is the client's
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

// Neither has a renderer to reach in hosted mode: the safe area's message, `frozen` and the render acknowledgement are the engine process's.
void WidgetHost::toRenderer(const std::string& json) {
    if (destroyed_ || rendererOnly_ || !renderer_) return;
    if (rendererReady_) renderer_->send(json);
    else pendingToRenderer_.push_back(PendingMessage{json, false});
}

bool WidgetHost::engineToRenderer(std::string json) {
    if (destroyed_ || engineCut_ || rendererOnly_ || !renderer_) return false;
    if (rendererReady_) {
        renderer_->send(json);
        return true;
    }
    if (backlogMessages_ < kRendererBacklogMessages && json.size() <= kRendererBacklogBytes - backlogBytes_) {
        backlogBytes_ += json.size();
        backlogMessages_++;
        pendingToRenderer_.push_back(PendingMessage{std::move(json), true});
        return true;
    }
    engineCut_ = true;
    std::vector<PendingMessage> kept;
    for (PendingMessage& m : pendingToRenderer_) {
        if (!m.fromEngine) kept.push_back(std::move(m));
    }
    pendingToRenderer_ = std::move(kept);
    backlogBytes_ = 0;
    backlogMessages_ = 0;
    opsSent_ = 0;
    diag(kBacklogLimit, kBacklogLimitReason);
    // A query waiting, or the one whose message passed the bounds, would hold the engine's thread for its 3 s before the stop runs.
    resolveQuery("{\"error\":\"dead\"}", std::nullopt);
    engine_->stop(kBacklogLimitReason);
    return false;
}

} // namespace twidget
