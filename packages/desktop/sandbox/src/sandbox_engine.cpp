#include "twidget/sandbox_engine.h"

#include "twidget/json.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__ANDROID__)
#include <stdlib.h>
#elif defined(__linux__)
#include <sys/random.h>
#elif defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#endif

namespace twidget {

static constexpr double kInf = std::numeric_limits<double>::infinity();
static constexpr const char* kActivationPrefix = "activation:";
static constexpr const char* kHideHostGlobals =
    "(function(g){for(const n of ['__host','__hostBinary','__hostCanvas','__hostOps','__twDispatch'])if(!delete g[n])throw new Error(n);})(globalThis);";
static constexpr double kWallBackstopFactor = 10.0;
static constexpr double kWallBackstopMinMs = 5000.0;

double SandboxEngine::nowSeconds() { return QuickJSContext::now(); }
double SandboxEngine::nowMs() { return QuickJSContext::now() * 1000; }

static void randomBytes(uint8_t* buf, size_t n) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__ANDROID__)
    arc4random_buf(buf, n);
#elif defined(__linux__)
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(buf + got, n - got, 0);
        if (r <= 0) break;
        got += size_t(r);
    }
#elif defined(_WIN32)
    BCryptGenRandom(nullptr, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
#error "no CSPRNG for this platform"
#endif
}

static std::pair<std::string, std::string> splitPrefix(const std::string& s) {
    size_t i = s.find(':');
    if (i == std::string::npos) return {s, ""};
    return {s.substr(0, i), s.substr(i + 1)};
}

static int countOccurrences(const std::string& text, const char* needle) {
    int count = 0;
    size_t len = std::strlen(needle);
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + len)) count++;
    return count;
}

static std::string truncate(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n); }

SandboxEngine::SandboxEngine(std::string html, SandboxConfig config, std::shared_ptr<SandboxResources> resources, std::shared_ptr<IntlProvider> intl)
    : thread_("widget.sandbox"), html_(std::move(html)), config_(config), resources_(std::move(resources)),
      intl_(intl ? std::move(intl) : std::make_shared<BasicIntl>()), visible_(config.visible),
      budget_(config.cpuLimitMs, config.cpuQuotaMs, config.cpuQuotaWindowMs, config.cpuKillAfterMs) {}

SandboxEngine::~SandboxEngine() {
    thread_.join();
    // The thread is gone: nothing else touches the context, so it can be freed here.
    js_.reset();
}

// MARK: lifecycle

void SandboxEngine::start() {
    thread_.async([this] { boot(); });
}

void SandboxEngine::terminate() {
    thread_.async([this] { kill("terminated"); });
}

void SandboxEngine::boot() {
    if (started_) return;
    started_ = true;
    js_ = std::make_unique<QuickJSContext>(config_.memoryLimitBytes, JSThread::kJSStackLimit);
    js_->registerFunction("__host", [this](const std::vector<std::string>& args) {
        return hostCall(args.empty() ? "" : args[0], args.size() > 1 ? args[1] : "");
    });
    js_->registerFunction("__hostBinary", [this](const std::vector<std::string>&) {
        if (!lastBinary_) return HostReturn::null();
        auto data = std::move(*lastBinary_);
        lastBinary_.reset();
        return HostReturn::arrayBuffer(std::move(data));
    });
    task("boot", config_.cpuLimitMs, [this] {
        double t0 = nowMs();
        // Generous one-off budget for our own bundle; its time is ours, not the widget's.
        js_->deadline = nowSeconds() + std::max(config_.cpuLimitMs, 5000.0) / 1000;
        js_->cpuDeadline = kInf;
        std::string how = evalBundle(resources_->sandboxBundle, resources_->sandboxBytecode, "sandbox-bundle.js");
        excludeMs_ = nowMs() - t0;
        // Fail closed: a runtime whose shim did not load completely (its network
        // stubs and DOM capture may be missing) never runs widget code.
        if (how == "failed") {
            if (!dead_) kill("sandbox bundle failed to load");
            return;
        }
        // The bundle holds private references to the host functions; widget code
        // must not reach them, nor the shim's dispatcher, through the global object.
        bool hidden = js_->captureGlobalFunction("__twDispatch");
        if (hidden) {
            try { js_->evaluate(kHideHostGlobals, "hide-host.js", false); } catch (const JSError&) { hidden = false; }
        }
        if (!hidden) {
            if (!dead_) kill("sandbox bundle failed to load");
            return;
        }
        if (delegate_) delegate_->onDiag(*this, "boot", "sandbox bundle eval " + std::to_string(int(excludeMs_)) + " ms (" + how + ")");
        resetDeadline();
        Json config = Json::object();
        config.set("memoryLimitBytes", double(config_.memoryLimitBytes));
        config.set("cpuLimitMs", config_.cpuLimitMs);
        config.set("cpuQuotaMs", config_.cpuQuotaMs);
        config.set("cpuQuotaWindowMs", config_.cpuQuotaWindowMs);
        config.set("cpuKillAfterMs", config_.cpuKillAfterMs);
        config.set("maxTimers", config_.maxTimers);
        config.set("visible", visible_);
        Json viewport = Json::object();
        viewport.set("innerWidth", config_.innerWidth);
        viewport.set("innerHeight", config_.innerHeight);
        viewport.set("dpr", config_.devicePixelRatio);
        config.set("viewport", viewport);
        config.set("theme", config_.theme);
        Json features = Json::array();
        for (const std::string& feature : config_.features) features.push(Json(feature));
        config.set("features", features);
        Json boot = Json::object();
        boot.set("html", html_);
        boot.set("config", config);
        callTw("boot", boot.dump());
    });
    task("boot", config_.cpuLimitMs, [this] { callTw("loaded", ""); });
    if (!dead_ && delegate_) delegate_->onLoaded(*this);
    postStats(true);
    auto queued = std::move(pendingBeforeBoot_);
    pendingBeforeBoot_.clear();
    for (auto& [kind, arg] : queued) if (!dead_) dispatch(kind, arg);
}

// MARK: inputs from the host (all asynchronous)

void SandboxEngine::event(const std::string& json) { enqueue("event", json); }
void SandboxEngine::activationEvent(const std::string& json, int gesture) { enqueue(kActivationPrefix + std::to_string(gesture), json); }
/// Widget clocks tick in 100 µs steps, like a browser's `performance.now()`, so they cannot time cache or pool effects of other widgets.
static double coarseNowMs(double ms) { return std::floor(ms / 0.1) * 0.1; }

void SandboxEngine::frame(double time) { enqueue("frame", Json::number(coarseNowMs(time))); }
void SandboxEngine::viewport(double innerWidth, double innerHeight, double dpr) {
    Json v = Json::object();
    v.set("innerWidth", innerWidth);
    v.set("innerHeight", innerHeight);
    v.set("dpr", dpr);
    enqueue("viewport", v.dump());
}
void SandboxEngine::resize(const std::string& entriesJson) { enqueue("resize", entriesJson); }
void SandboxEngine::theme(const std::string& theme) { enqueue("theme", Json::quote(theme)); }
void SandboxEngine::bridgeEvent(const std::string& event, const std::string& dataJson) {
    enqueue("bridge", "[" + Json::quote(event) + "," + (dataJson.empty() ? "null" : dataJson) + "]");
}

void SandboxEngine::setVisible(bool v) {
    thread_.async([this, v] {
        if (dead_ || visible_ == v) return;
        visible_ = v;
        if (!js_) return; // boot reads `visible_` for the shim
        dispatch("visibility", std::string("{\"visible\":") + (v ? "true" : "false") + "}");
        if (v) {
            if (frameWanted_) { frameWanted_ = false; if (delegate_) delegate_->onRequestFrame(*this); }
            if (!deferred_.empty()) wake();
        }
    });
}

std::string SandboxEngine::probe(const std::string& code) {
    return thread_.sync([&]() -> std::string {
        if (!js_ || dead_) return "ERROR no runtime";
        try {
            auto r = js_->evaluate("(function(){ try { return String(" + code + "); } catch (e) { return 'THREW ' + e; } })()", "probe.js");
            return r.string.value_or(r.number ? Json::number(*r.number) : std::string("undefined"));
        } catch (const JSError& e) {
            return std::string("ERROR ") + e.what();
        }
    });
}

void SandboxEngine::enqueue(const std::string& kind, const std::string& arg) {
    thread_.async([this, kind, arg] {
        if (dead_) return;
        if (!js_) { pendingBeforeBoot_.emplace_back(kind, arg); return; }
        // A frame that arrives after the widget was hidden is dropped; the shim's callbacks wait for the next visible frame.
        if (kind == "frame" && !visible_) { frameWanted_ = true; return; }
        dispatch(kind, arg);
    });
}

/// Runs one task for an input (event, timer, frame, …) if the CPU quota
/// allows, otherwise defers it (returns false). Events and visibility always run.
bool SandboxEngine::dispatch(const std::string& kind, const std::string& arg) {
    if (dead_) return true;
    if (kind.rfind(kActivationPrefix, 0) == 0) {
        int gesture = std::atoi(kind.c_str() + std::strlen(kActivationPrefix));
        activeGesture_ = visible_ && gesture > consumedGesture_ ? gesture : 0;
        bool ran = dispatch("event", arg);
        activeGesture_ = 0;
        return ran;
    }
    double now = nowMs();
    CpuDecision d = budget_.decide(kind, now);
    if (!d.run) {
        if (budget_.shouldKill(now)) {
            char buf[256];
            snprintf(buf, sizeof buf, "CPU quota: throttled for %.1f s (%d ms per %d ms, cpuKillAfterMs=%d)", budget_.throttledFor(now) / 1000,
                     int(config_.cpuQuotaMs), int(config_.cpuQuotaWindowMs), int(config_.cpuKillAfterMs));
            kill(buf);
            return true;
        }
        deferTask(kind, arg);
        stats_.throttledTasks++;
        throttledSinceDiag_[kind]++;
        if (now - lastThrottleDiag_ >= 1000) {
            lastThrottleDiag_ = now;
            std::string parts;
            for (auto& [k, n] : throttledSinceDiag_) {
                if (!parts.empty()) parts += ", ";
                parts += std::to_string(n) + " " + k + (n == 1 ? "" : "s");
            }
            throttledSinceDiag_.clear();
            if (delegate_) delegate_->onDiag(*this, "cpu-throttled", "deferred " + parts + ": " + std::to_string(int(budget_.used(now))) + " ms of the " +
                                                                        std::to_string(int(config_.cpuQuotaMs)) + " ms per " + std::to_string(int(config_.cpuQuotaWindowMs)) + " ms budget used");
        }
        scheduleWake(d.retryAt / 1000);
        return false;
    }
    task(kind, d.sliceMs, [&] { callTw(kind, arg); });
    return true;
}

void SandboxEngine::deferTask(const std::string& kind, const std::string& arg) {
    if (kind == "frame") {
        for (auto& item : deferred_) if (item.kind == "frame") { item.arg = arg; return; }
    }
    if (kind == "timer") {
        for (auto& item : deferred_) if (item.kind == "timer" && item.arg == arg) return; // intervals do not accumulate missed ticks
    }
    deferred_.push_back({kind, arg});
}

void SandboxEngine::scheduleWake(double at) {
    if (wakeTimer_ && at >= wakeAt_) return;
    if (wakeTimer_) wakeTimer_->cancel();
    wakeAt_ = at;
    wakeTimer_ = thread_.after(std::max(0.0, at - nowSeconds()), [this] { wake(); });
}

/// Runs deferred tasks in order; stops (and re-queues the rest) if one is deferred again.
void SandboxEngine::wake() {
    wakeTimer_.reset();
    wakeAt_ = kInf;
    hiddenTickAt_ = 0;
    auto items = std::move(deferred_);
    deferred_.clear();
    for (size_t i = 0; i < items.size(); i++) {
        if (dead_) return;
        if (!dispatch(items[i].kind, items[i].arg)) {
            deferred_.insert(deferred_.begin(), items.begin() + long(i), items.end());
            return;
        }
    }
}

void SandboxEngine::onTimerFired(int id) {
    timers_.erase(id);
    if (!visible_) {
        deferTask("timer", std::to_string(id));
        if (hiddenTickAt_ == 0) hiddenTickAt_ = nowSeconds() + 1;
        scheduleWake(hiddenTickAt_);
        return;
    }
    dispatch("timer", std::to_string(id));
}

// MARK: the single capability

HostReturn SandboxEngine::hostCall(const std::string& kind, const std::string& arg) {
    if (kind == "ops") {
        stats_.ops += countOccurrences(arg, "\"op\":");
        if (delegate_) delegate_->onOps(*this, arg);
        return HostReturn::undefined();
    }
    if (kind == "query") return HostReturn::string(syncQuery(arg));
    if (kind == "now") return HostReturn::number(coarseNowMs(nowMs()));
    if (kind == "timer") {
        auto pair = Json::parse(arg);
        if (!pair || !pair->isArray() || pair->size() != 2 || !(*pair)[0].isNumber()) return HostReturn::undefined();
        int id = (*pair)[0].asInt();
        double delay = std::min(std::max(0.0, (*pair)[1].asNumber()) / 1000, kMaxTimerDelaySeconds);
        if (!visible_) delay = std::max(delay, 1.0); // hidden: 1 Hz, like a background tab
        if (int(timers_.size()) >= config_.maxTimers) {
            // Backstop: the shim should have thrown RangeError before asking. Reaching this means a shim bug (or a tampered shim).
            if (!timerLimitReported_) {
                timerLimitReported_ = true;
                if (delegate_) delegate_->onDiag(*this, "timer-limit-enforced", "engine refused a timer beyond maxTimers=" + std::to_string(config_.maxTimers) + "; the shim did not stop it");
            }
            return HostReturn::undefined();
        }
        timers_[id] = thread_.after(delay, [this, id] {
            if (dead_) return;
            onTimerFired(id);
        });
        return HostReturn::undefined();
    }
    if (kind == "clearTimer") {
        char* end = nullptr;
        long id = std::strtol(arg.c_str(), &end, 10);
        if (end && *end == '\0') {
            auto it = timers_.find(int(id));
            if (it != timers_.end()) { it->second->cancel(); timers_.erase(it); }
        }
        return HostReturn::undefined();
    }
    if (kind == "raf") {
        if (!visible_) { frameWanted_ = true; return HostReturn::undefined(); } // one frame is requested when the widget becomes visible again
        if (delegate_) delegate_->onRequestFrame(*this);
        return HostReturn::undefined();
    }
    if (kind == "log") {
        auto [level, text] = splitPrefix(arg);
        if (delegate_) delegate_->onLog(*this, level, truncate(text, 4000));
        return HostReturn::undefined();
    }
    if (kind == "diag") {
        auto [k, detail] = splitPrefix(arg);
        if (delegate_) delegate_->onDiag(*this, truncate(k, 64), truncate(detail, 4000));
        return HostReturn::undefined();
    }
    if (kind == "listen") { if (delegate_) delegate_->onListen(*this, arg); return HostReturn::undefined(); }
    if (kind == "pd") { if (delegate_) delegate_->onPreventDefault(*this, arg); return HostReturn::undefined(); }
    if (kind == "anim") { if (delegate_) delegate_->onAnim(*this, arg); return HostReturn::undefined(); }
    if (kind == "bridge") {
        auto pair = Json::parse(arg);
        if (!pair || !pair->isArray() || pair->size() != 2 || !pair->arr()[0].isString()) return HostReturn::undefined();
        std::string name = truncate(pair->arr()[0].asString(), 64);
        if (isStrictPolicy(config_.policy) && name != "ready") {
            if (!(activeGesture_ > consumedGesture_ && visible_)) {
                if (blockedBridgeReported_ < 20 && delegate_) {
                    blockedBridgeReported_++;
                    delegate_->onDiag(*this, "activation-blocked", name + " ignored: it needs a user click or key press on the visible widget and runs in that event's handler (called from the " + currentKind_ + " task)");
                }
                return HostReturn::undefined();
            }
            consumedGesture_ = activeGesture_;
            activeGesture_ = 0;
        }
        if (delegate_) delegate_->onBridge(*this, name, pair->arr()[1].dump());
        return HostReturn::undefined();
    }
    if (kind == "intl") {
        auto req = Json::parse(arg);
        if (!req || !req->isObject()) return HostReturn::string("");
        return HostReturn::string(intl_->format(*req));
    }
    if (kind == "random") {
        long n = std::strtol(arg.c_str(), nullptr, 10);
        n = std::min(std::max(n, 0L), 65536L);
        std::vector<uint8_t> bytes(static_cast<size_t>(n), 0);
        if (n > 0) randomBytes(bytes.data(), bytes.size());
        std::string out = "[";
        for (size_t i = 0; i < bytes.size(); i++) { if (i) out.push_back(','); out += std::to_string(bytes[i]); }
        out.push_back(']');
        return HostReturn::string(out);
    }
    if (kind == "stat") {
        if (auto s = Json::parse(arg); s && s->isObject()) {
            stats_.queryCacheHits = (*s)["cacheHits"].asInt();
            stats_.missedMutations = (*s)["missed"].asInt();
        }
        return HostReturn::undefined();
    }
    return HostReturn::undefined();
}

/// Blocks the engine thread until the host answers (the host must never wait for the engine).
std::string SandboxEngine::syncQuery(const std::string& json) {
    if (!delegate_) return "{\"error\":\"no host\"}";
    double t0 = nowSeconds();
    QueryAnswer answer = delegate_->onQuery(*this, json);
    double dt = nowSeconds() - t0;
    js_->deadline += dt; // time spent waiting is not widget CPU time
    stats_.queries++;
    stats_.queryMs += dt * 1000;
    lastBinary_ = std::move(answer.binary);
    return answer.json;
}

// MARK: running code

void SandboxEngine::kill(const std::string& reason) {
    if (dead_) return;
    dead_ = true;
    pendingBeforeBoot_.clear();
    for (auto& [_, t] : timers_) t->cancel();
    timers_.clear();
    deferred_.clear();
    if (wakeTimer_) { wakeTimer_->cancel(); wakeTimer_.reset(); }
    if (trailingStats_) { trailingStats_->cancel(); trailingStats_.reset(); }
    if (delegate_) delegate_->onDied(*this, reason);
}

void SandboxEngine::handleError(const std::string& text, const std::string& where) {
    if (js_ && js_->interrupted) {
        // The slice is cpuLimitMs unless the CPU quota shrank it; say so, the fix differs (infinite loop vs. a widget that was already over budget).
        std::string quota;
        if (currentSliceMs_ < config_.cpuLimitMs) {
            quota = "; CPU quota: " + std::to_string(int(std::max(0.0, config_.cpuQuotaMs - budget_.used(nowMs())))) + " ms of " + std::to_string(int(config_.cpuQuotaMs)) +
                    " ms per " + std::to_string(int(config_.cpuQuotaWindowMs)) + " ms left, so this " + currentKind_ + " task got " + std::to_string(int(currentSliceMs_)) + " ms";
        }
        return kill("CPU limit exceeded: more than " + std::to_string(int(currentSliceMs_)) + " ms of continuous execution (" + where + quota + ")");
    }
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (lower.find("out of memory") != std::string::npos) return kill("Memory limit exceeded (" + std::to_string(config_.memoryLimitBytes >> 20) + " MB)");
    if (delegate_) delegate_->onLog(*this, "error", truncate("Uncaught " + text, 4000));
}

void SandboxEngine::drainJobs() {
    js_->executePendingJobs([this](const JSError& e) { handleError(e.what(), "promise job"); });
}

std::optional<std::string> SandboxEngine::callTw(const std::string& kind, const std::string& arg) {
    if (dead_) return std::nullopt;
    try {
        return js_->call("__twDispatch", {kind, arg}).string;
    } catch (const JSError& e) {
        handleError(e.what(), kind);
        return std::nullopt;
    }
}

void SandboxEngine::settle() {
    for (int i = 0; i < 20 && !dead_; i++) {
        drainJobs();
        if (dead_) return;
        auto more = callTw("endTick", "");
        if (!(more && *more == "1") && !js_->hasPendingJob()) break;
    }
}

std::string SandboxEngine::evalBundle(const std::string& source, const std::vector<uint8_t>& bytecode, const std::string& filename) {
    if (!bytecode.empty()) {
        try {
            js_->evaluateBytecode(bytecode, source);
            return "bytecode";
        } catch (const JSError& e) {
            std::string msg = e.what();
            if (delegate_) delegate_->onDiag(*this, "bytecode", filename + ": falling back to source — " + msg.substr(0, msg.find('\n')));
        }
    }
    try {
        js_->evaluate(source, filename, false);
    } catch (const JSError& e) {
        handleError(e.what(), filename);
        return "failed";
    }
    return "source";
}

void SandboxEngine::evalScript(const std::string& code, const std::string& filename, bool asModule) {
    try { js_->evaluate(code, filename, asModule); } catch (const JSError& e) { handleError(e.what(), filename); }
}

void SandboxEngine::pumpScripts() {
    for (int i = 0; i < 1000 && !dead_; i++) {
        auto next = callTw("nextScript", "");
        if (!next || next->empty()) break;
        auto s = Json::parse(*next);
        if (!s || !s->isObject()) break;
        int id = (*s)["id"].asInt();
        bool ok = true;
        resetDeadline();
        if (const Json* src = s->find("src"); src && src->isString()) {
            const BundledLibrary* lib = resources_->library(src->asString());
            std::optional<std::string> source = lib ? resources_->librarySource(*lib) : std::nullopt;
            if (lib && source) {
                double t0 = nowMs();
                std::string how = evalBundle(*source, resources_->libraryBytecode(*lib), lib->file);
                if (delegate_) delegate_->onDiag(*this, "library", src->asString() + " → bundled " + lib->name + " (eval " + std::to_string(int(nowMs() - t0)) + " ms, " + how + ")");
            } else {
                ok = false;
                if (delegate_) delegate_->onDiag(*this, "script-skipped", "external script not available: " + truncate(src->asString(), 200));
            }
        } else if (const Json* code = s->find("code"); code && code->isString()) {
            evalScript(code->asString(), "inline-script-" + std::to_string(id) + ".js", (*s)["type"].asString() == "module");
        }
        if (dead_) return;
        settle();
        callTw("scriptDone", "[" + std::to_string(id) + "," + (ok ? "true" : "false") + "]");
        settle();
    }
}

/// The slice is measured in the JS thread's CPU time, so a busy machine does not
/// kill widgets; wall-clock time is only a backstop for a thread that never runs.
void SandboxEngine::resetDeadline() {
    js_->interrupted = false;
    js_->cpuDeadline = QuickJSContext::threadCpuTime() + currentSliceMs_ / 1000;
    js_->deadline = nowSeconds() + std::max(currentSliceMs_ * kWallBackstopFactor, kWallBackstopMinMs) / 1000;
}

/// One task of `kind` with `sliceMs` of continuous execution; its JS time is recorded against the quota.
void SandboxEngine::task(const std::string& kind, double sliceMs, const std::function<void()>& body) {
    if (dead_) return;
    currentKind_ = kind;
    currentSliceMs_ = sliceMs;
    excludeMs_ = 0;
    double c0 = QuickJSContext::threadCpuTime();
    resetDeadline();
    body();
    if (!dead_) settle();
    if (!dead_) pumpScripts();
    stats_.tasks++;
    double cpuMs = (QuickJSContext::threadCpuTime() - c0) * 1000 - excludeMs_;
    stats_.jsMs += cpuMs;
    budget_.record(kind, cpuMs, nowMs());
    stats_.cpuWindowMs = budget_.used(nowMs());
    double now = nowSeconds();
    if (now - lastMemCheck_ > 1 && !dead_) {
        lastMemCheck_ = now;
        stats_.memoryBytes = js_->memoryUsed();
    }
    postStats(false);
}

/// At most one stats message per 200 ms, but the last state always gets out.
void SandboxEngine::postStats(bool force) {
    double now = nowSeconds();
    if (force || now - lastStatsPost_ > 0.2) {
        lastStatsPost_ = now;
        if (trailingStats_) { trailingStats_->cancel(); trailingStats_.reset(); }
        if (delegate_) delegate_->onStats(*this, stats_);
    } else if (!trailingStats_) {
        trailingStats_ = thread_.after(0.2, [this] {
            trailingStats_.reset();
            postStats(true);
        });
    }
}

} // namespace twidget
