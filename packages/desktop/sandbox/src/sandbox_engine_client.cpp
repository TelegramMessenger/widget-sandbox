// Client mode of SandboxEngine, the twin of SandboxEngine.swift's: no runtime here, the widget runs behind a SandboxRemoteChannel (remote_engine.h).
#include "twidget/sandbox_engine.h"

#include "twidget/json.h"
#include "twidget/remote_engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <future>
#include <string_view>
#include <utility>

namespace twidget {

namespace {

constexpr const char* kEngineGone = "the widget engine process ended";
constexpr const char* kEngineFlooded = "the widget engine process sent more than the host keeps queued";
/// The levels the shim's `console` sends; any other is "log".
constexpr std::array<std::string_view, 5> kLogLevels = {"log", "info", "warn", "error", "debug"};

/// A value made on a JSThread for a waiting caller (inline on that thread); the closure owns the promise alone, so one the thread drops breaks it, where `JSThread::sync` would wait forever.
template <class R>
class Posted {
public:
    template <class F>
    Posted(JSThread& thread, F fn) {
        if (thread.isCurrent()) {
            inline_ = std::move(fn);
            return;
        }
        auto promise = std::make_shared<std::promise<R>>();
        future_ = promise->get_future();
        thread.async([promise = std::move(promise), fn = std::move(fn)]() mutable { promise->set_value(fn()); });
    }

    /// Call it holding no lock the thread may need.
    std::optional<R> get() {
        if (inline_) return inline_();
        try {
            return future_.get();
        } catch (const std::future_error&) {
            return std::nullopt;
        }
    }

private:
    std::function<R()> inline_;
    std::future<R> future_;
};

QueryAnswer deadQuery() { return {"{\"error\":\"dead\"}", std::nullopt}; }

struct FactorySlot {
    std::mutex mutex;
    SandboxRemoteChannelFactory factory;
};

FactorySlot& factorySlot() {
    static FactorySlot slot;
    return slot;
}

} // namespace

// MARK: the sink

/// What the channel calls back, also after the engine is gone: the liveness lock only covers seeing the engine and queueing work, never a wait or a call out. What it queues is admitted first against `kRemoteQueuedBytes` and `kRemoteQueuedCalls`, before anything of the message is copied, and stays counted while the work its delegate queued with it waits (`remoteHold`).
class SandboxEngine::RemoteClient final : public SandboxRemoteSink {
public:
    explicit RemoteClient(std::shared_ptr<Liveness> life) : life_(std::move(life)) {}

    RemoteBacklog backlog() const { return {backlog_->bytes.load(), backlog_->calls.load()}; }
    /// A place for `bytes` of work a delegate made from the message being delivered (`remoteHold`), by `admit`'s rules.
    RemoteHold more(size_t bytes) { return admit(bytes); }

    // The engine's side sends a task's begin, that task's calls, then its end, never nested; a begin no delegate heard has no end here, and a death ends an open task.
    void remoteTaskStarted() override {
        if (Admitted admitted = admit(0)) {
            submit(std::move(admitted), [](SandboxEngine& engine) {
                if (engine.dead_ || engine.remoteTaskOpen_ || !engine.delegate_) return;
                engine.remoteTaskOpen_ = true;
                engine.delegate_->onTaskStarted(engine);
            });
        }
    }

    void remoteTaskCompleted() override {
        if (Admitted admitted = admit(0)) {
            submit(std::move(admitted), [](SandboxEngine& engine) {
                if (std::exchange(engine.remoteTaskOpen_, false) && engine.delegate_) engine.delegate_->onTaskCompleted(engine);
            });
        }
    }

    // What the delegate passes to the validator, as in process: only the size is checked here, before anything is copied.
    void remoteOps(const std::string& json) override {
        if (json.size() > kRemoteOpsLimit) return violation("an ops batch over 32 MB");
        if (Admitted admitted = admit(json.size())) deliver(std::move(admitted), [json](SandboxEngine& engine, SandboxEngineDelegate& delegate) { delegate.onOps(engine, json); });
    }

    void remoteListen(const std::string& json) override {
        if (json.size() > kRemoteMessageLimit) return violation("listen over 1 MB");
        if (Admitted admitted = admit(json.size())) deliver(std::move(admitted), [json](SandboxEngine& engine, SandboxEngineDelegate& delegate) { delegate.onListen(engine, json); });
    }

    void remotePreventDefault(const std::string& json) override {
        if (json.size() > kRemoteMessageLimit) return violation("preventDefault over 1 MB");
        if (Admitted admitted = admit(json.size())) deliver(std::move(admitted), [json](SandboxEngine& engine, SandboxEngineDelegate& delegate) { delegate.onPreventDefault(engine, json); });
    }

    void remoteAnim(const std::string& json) override {
        if (json.size() > kRemoteMessageLimit) return violation("anim over 1 MB");
        if (Admitted admitted = admit(json.size())) deliver(std::move(admitted), [json](SandboxEngine& engine, SandboxEngineDelegate& delegate) { delegate.onAnim(engine, json); });
    }

    void remoteRequestsFrame() override {
        if (Admitted admitted = admit(0)) {
            submit(std::move(admitted), [](SandboxEngine& engine) {
                if (!engine.dead_ && engine.visible_ && engine.delegate_) engine.delegate_->onRequestFrame(engine);
            });
        }
    }

    void remoteLog(const std::string& level, const std::string& text) override {
        std::string known = std::find(kLogLevels.begin(), kLogLevels.end(), level) != kLogLevels.end() ? level : "log";
        Admitted admitted = admit(known.size() + std::min(text.size(), kRemoteTextLimit));
        if (!admitted) return;
        deliver(std::move(admitted), [level = std::move(known), text = text.substr(0, kRemoteTextLimit)](SandboxEngine& engine, SandboxEngineDelegate& delegate) {
            delegate.onLog(engine, level, text);
        });
    }

    void remoteDiag(const std::string& kind, const std::string& detail) override {
        Admitted admitted = admit(std::min(kind.size(), kRemoteNameLimit) + std::min(detail.size(), kRemoteTextLimit));
        if (!admitted) return;
        deliver(std::move(admitted), [kind = kind.substr(0, kRemoteNameLimit), detail = detail.substr(0, kRemoteTextLimit)](SandboxEngine& engine, SandboxEngineDelegate& delegate) {
            delegate.onDiag(engine, kind, detail);
        });
    }

    // Checked as the engine checks it (lone surrogates kept for the SDK to refuse), only an action, then the preview, the host's tap and haptic gate; the raw arguments go on as they came.
    void remoteBridge(const std::string& method, const std::string& argsJson) override {
        const bool sized = method.size() <= kRemoteNameLimit && argsJson.size() <= kRemoteMessageLimit;
        Admitted admitted = admit(sized ? method.size() + argsJson.size() : kRemoteNameLimit);
        if (!admitted) return;
        std::optional<WidgetAction> action;
        if (sized) {
            std::optional<Json> args = Json::parse(argsJson, Json::Surrogates::Keep);
            std::optional<sdk::BridgeCall> checked = args ? sdk::checkBridgeCall(Json(method), *args) : std::nullopt;
            if (checked) action = std::move(checked->action);
        }
        if (!action) {
            return submit(std::move(admitted), [name = method.substr(0, kRemoteNameLimit)](SandboxEngine& engine) {
                engine.clientReport(engine.rejectedBridgeReported_, "bridge-rejected", name + ": rejected by the host (unknown method or malformed arguments)");
            });
        }
        submit(std::move(admitted), [method, argsJson, action = std::move(*action)](SandboxEngine& engine) {
            if (engine.dead_ || (engine.config_.context.preview && !sdk::allowedInPreview(method)) || !engine.clientActivation(method)) return;
            // Dropped silently: the engine's side reports its own `haptic-limit`.
            if (action.kind == WidgetAction::Haptic && engine.haptics_.decide(nowMs(), engine.visible_) != sdk::HapticGate::Decision::Play) return;
            if (!engine.delegate_) return;
            engine.delegate_->onBridgeAction(engine, action);
            engine.delegate_->onBridge(engine, method, argsJson);
        });
    }

    // What is kept is the checked text, far under the cut admitted for it.
    void remoteCopyText(const std::string& text) override {
        Admitted admitted = admit(std::min(text.size(), kRemoteTextLimit));
        if (!admitted) return;
        submit(std::move(admitted), [checked = sdk::checkCopyText(text)](SandboxEngine& engine) {
            if (engine.dead_ || engine.config_.context.preview) return;
            if (!checked) return engine.clientReport(engine.rejectedBridgeReported_, "bridge-rejected", "copyText: rejected by the host");
            if (engine.clientActivation("copyText") && engine.delegate_) engine.delegate_->onBridgeCopyText(engine, *checked);
        });
    }

    // The engine's check again (the quota, last of a repeated key, strings as they came); a commit of neither area is refused too.
    void remoteStorage(const std::string& commitJson) override {
        const bool sized = commitJson.size() <= sdk::MAX_STORAGE_COMMIT_BYTES;
        Admitted admitted = admit(sized ? commitJson.size() : 0);
        if (!admitted) return;
        std::optional<sdk::StorageCommit> commit;
        if (sized) {
            std::optional<Json> parsed = Json::parse(commitJson, Json::Surrogates::Keep);
            if (parsed) commit = sdk::checkStorageCommit(*parsed);
            if (commit && !commit->local && !commit->session) commit.reset();
        }
        submit(std::move(admitted), [commit = std::move(commit)](SandboxEngine& engine) {
            if (engine.dead_ || engine.config_.context.preview) return;
            if (!commit) return engine.clientReport(engine.rejectedStorageReported_, "storage-rejected", "a storage commit the host refused");
            if (!engine.delegate_) return;
            if (commit->local) engine.delegate_->onStorage(engine, "local", *commit->local);
            if (commit->session) engine.delegate_->onStorage(engine, "session", *commit->session);
        });
    }

    void remoteResizeAnchor(const std::string& edge) override {
        std::optional<std::string> anchor = sdk::checkResizeAnchor(edge);
        if (!anchor) return violation("resize anchor " + edge.substr(0, 16));
        if (Admitted admitted = admit(anchor->size())) {
            deliver(std::move(admitted), [anchor = std::move(*anchor)](SandboxEngine& engine, SandboxEngineDelegate& delegate) { delegate.onResizeAnchor(engine, anchor); });
        }
    }

    void remoteWatchesPublicData(bool on) override {
        if (Admitted admitted = admit(0)) {
            submit(std::move(admitted), [on](SandboxEngine& engine) {
                if (!engine.dead_ && !engine.config_.context.preview && engine.delegate_) engine.delegate_->onPublicDataWatch(engine, on);
            });
        }
    }

    void remoteLoaded() override {
        if (Admitted admitted = admit(0)) deliver(std::move(admitted), [](SandboxEngine& engine, SandboxEngineDelegate& delegate) { delegate.onLoaded(engine); });
    }

    // A transport may hand over any struct: bounded as the wire's numbers are.
    void remoteStats(const SandboxStats& stats) override {
        Admitted admitted = admit(0);
        if (!admitted) return;
        deliver(std::move(admitted), [stats = SandboxRemoteCoding::decodeStats(SandboxRemoteCoding::encodeStats(stats)).value_or(SandboxStats{})](SandboxEngine& engine, SandboxEngineDelegate& delegate) {
            delegate.onStats(engine, stats);
        });
    }

    // Only what passes the SDK's checks is queued; a call that fails them is still answered (NOT_ALLOWED), so the engine's side never waits for it.
    void remoteRequest(int64_t id, const std::string& data, bool wallet) override {
        Admitted admitted = admit(std::min(data.size(), size_t(sdk::MAX_REQUEST_DATA_BYTES)));
        if (!admitted) return;
        std::optional<WidgetRequest> request;
        if (std::optional<std::string> checked = sdk::checkRequestData(data)) request = WidgetRequest{std::move(*checked), wallet};
        submit(std::move(admitted), [id, request = std::move(request)](SandboxEngine& engine) { engine.clientRequest(id, request, std::nullopt); });
    }

    void remoteWallet(int64_t id, const std::string& payload) override {
        Admitted admitted = admit(std::min(payload.size(), kRemoteMessageLimit));
        if (!admitted) return;
        submit(std::move(admitted), [id, call = SandboxRemoteCoding::decodeWallet(payload)](SandboxEngine& engine) { engine.clientRequest(id, std::nullopt, call); });
    }

    QueryAnswer remoteQuery(const std::string& json) override {
        if (json.size() > kRemoteMessageLimit) return {"{\"error\":\"invalid query\"}", std::nullopt};
        Admitted admitted = admit(json.size());
        if (!admitted) return deadQuery();
        return await<QueryAnswer>(std::move(admitted), [json](SandboxEngine& engine) {
            if (engine.dead_ || !engine.delegate_) return deadQuery();
            return engine.delegate_->onQuery(engine, json);
        }).value_or(deadQuery());
    }

    std::string remoteIntl(const std::string& requestJson) override {
        if (requestJson.size() > kRemoteMessageLimit) return "";
        Admitted admitted = admit(requestJson.size());
        if (!admitted) return "";
        std::optional<Json> request = Json::parse(requestJson);
        if (!request || !request->isObject()) return "";
        std::string answer = await<std::string>(std::move(admitted), [request = std::move(*request)](SandboxEngine& engine) {
            return engine.dead_ ? std::string() : engine.intl_->format(request);
        }).value_or("");
        // It goes back as one message: a provider's answer over the limit is none.
        return answer.size() > kRemoteMessageLimit ? std::string() : answer;
    }

    void remoteDied(const std::string& reason) override {
        Admitted admitted = admit(std::min(reason.size(), kRemoteTextLimit));
        if (!admitted) return;
        submit(std::move(admitted), [reason = reason.substr(0, kRemoteTextLimit)](SandboxEngine& engine) { engine.remoteKill(reason, false); });
    }

    void remoteDisconnected() override {
        if (Admitted admitted = admit(0)) submit(std::move(admitted), [](SandboxEngine& engine) { engine.remoteKill(kEngineGone, false); });
    }

private:
    /// What is queued and has neither run nor been dropped; `overflowed` once a message did not fit.
    struct Backlog {
        std::atomic<size_t> bytes{0};
        std::atomic<size_t> calls{0};
        std::atomic<bool> overflowed{false};
    };
    /// One admitted message's share of the backlog, given back when all that holds it is gone: the queued call (run, or dropped by a stopped thread or a gone engine) and the work its delegate queued with it.
    struct Reservation {
        Reservation(std::shared_ptr<Backlog> backlog, size_t bytes) : backlog(std::move(backlog)), bytes(bytes) {}
        ~Reservation() {
            backlog->bytes -= bytes;
            backlog->calls--;
        }
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        const std::shared_ptr<Backlog> backlog;
        const size_t bytes;
    };
    using Admitted = std::shared_ptr<Reservation>;
    /// While it lives, `remoteHold` on the dispatch thread shares `admitted`; then again the message of the work it ran inside of.
    struct Delivering {
        Delivering(SandboxEngine& engine, RemoteHold admitted) : engine(engine), outer(std::exchange(engine.remoteDelivering_, std::move(admitted))) {}
        ~Delivering() { engine.remoteDelivering_ = std::move(outer); }
        Delivering(const Delivering&) = delete;
        Delivering& operator=(const Delivering&) = delete;
        SandboxEngine& engine;
        RemoteHold outer;
    };

    /// A place for one message of up to `bytes` that its queued call keeps, taken before any of it is copied and never waited for: none once a message did not fit; the first that does not fit kills the widget, after what came before it, and tells the engine's side to stop.
    Admitted admit(size_t bytes) {
        if (backlog_->overflowed) return nullptr;
        const size_t calls = ++backlog_->calls;
        const size_t total = backlog_->bytes += bytes;
        if (calls <= kRemoteQueuedCalls && total <= kRemoteQueuedBytes) return std::make_shared<Reservation>(backlog_, bytes);
        backlog_->bytes -= bytes;
        backlog_->calls--;
        if (!backlog_->overflowed.exchange(true)) submit(nullptr, [](SandboxEngine& engine) { engine.remoteKill(kEngineFlooded, true); });
        return nullptr;
    }

    /// `work` on the dispatch thread after everything queued before it, holding `admitted` until it has run or is dropped; `remoteHold` shares it while `work` runs.
    void submit(Admitted admitted, std::function<void(SandboxEngine&)> work) {
        std::lock_guard<std::mutex> l(life_->mutex);
        if (SandboxEngine* engine = life_->engine) {
            engine->thread_.async([engine, admitted = std::move(admitted), work = std::move(work)] {
                Delivering delivering(*engine, admitted);
                work(*engine);
            });
        }
    }

    /// `call` with the delegate after everything queued before it, unless the widget died or has none.
    template <class F>
    void deliver(Admitted admitted, F call) {
        submit(std::move(admitted), [call = std::move(call)](SandboxEngine& engine) {
            if (!engine.dead_ && engine.delegate_) call(engine, *engine.delegate_);
        });
    }

    /// Counted with the bridge's rejections, as in Swift.
    void violation(std::string detail) {
        Admitted admitted = admit(detail.size());
        if (!admitted) return;
        submit(std::move(admitted), [detail = std::move(detail)](SandboxEngine& engine) { engine.clientReport(engine.rejectedBridgeReported_, "protocol-violation", detail); });
    }

    /// `fn`'s value from the dispatch thread after everything queued before it, holding `admitted` until then (`remoteHold` shares it while `fn` runs); nullopt once the engine is gone or its thread stopped.
    template <class R, class F>
    std::optional<R> await(Admitted admitted, F fn) {
        std::optional<Posted<R>> posted;
        {
            std::lock_guard<std::mutex> l(life_->mutex);
            SandboxEngine* engine = life_->engine;
            if (!engine) return std::nullopt;
            posted.emplace(engine->thread_, [engine, admitted = std::move(admitted), fn = std::move(fn)] {
                Delivering delivering(*engine, admitted);
                return fn(*engine);
            });
        }
        return posted->get();
    }

    std::shared_ptr<Liveness> life_;
    const std::shared_ptr<Backlog> backlog_ = std::make_shared<Backlog>();
};

// MARK: construction

SandboxEngine::SandboxEngine(std::string html, SandboxConfig config, std::shared_ptr<SandboxRemoteChannel> remote, std::shared_ptr<IntlProvider> intl)
    : SandboxEngine(std::move(html), std::move(config), std::shared_ptr<SandboxResources>(), std::move(intl)) {
    remote_ = std::move(remote);
    client_ = std::make_shared<RemoteClient>(liveness_);
    // A host's factory without a channel: dead from the first task, never in process.
    if (remote_) remote_->attach(client_);
    else thread_.async([this] { remoteKill(kEngineGone, false); });
}

std::unique_ptr<SandboxEngine> SandboxEngine::make(std::string html, SandboxConfig config, std::shared_ptr<SandboxResources> resources, std::shared_ptr<IntlProvider> intl) {
    SandboxRemoteChannelFactory factory;
    {
        FactorySlot& slot = factorySlot();
        std::lock_guard<std::mutex> l(slot.mutex);
        factory = slot.factory;
    }
    if (factory) return std::make_unique<SandboxEngine>(std::move(html), std::move(config), factory(std::move(resources)), std::move(intl));
#ifndef NDEBUG
    // Debug only, for parity runs: without a factory every engine a host builds runs behind the in-process loopback.
    if (const char* loopback = std::getenv("TW_ENGINE_LOOPBACK"); loopback && std::string_view(loopback) == "1")
        return std::make_unique<SandboxEngine>(std::move(html), std::move(config), std::make_shared<SandboxLoopbackChannel>(std::move(resources)), std::move(intl));
#endif
    return std::make_unique<SandboxEngine>(std::move(html), std::move(config), std::move(resources), std::move(intl));
}

void SandboxEngine::setRemoteChannelFactory(SandboxRemoteChannelFactory factory) {
    FactorySlot& slot = factorySlot();
    std::lock_guard<std::mutex> l(slot.mutex);
    std::swap(slot.factory, factory);
}

SandboxEngine::RemoteBacklog SandboxEngine::remoteBacklog() const { return client_ ? client_->backlog() : RemoteBacklog{}; }

SandboxEngine::RemoteHold SandboxEngine::remoteHold() const { return remoteDelivering_; }

std::optional<SandboxEngine::RemoteHold> SandboxEngine::remoteHold(size_t bytes) {
    if (!client_ || !remoteDelivering_) return RemoteHold();
    RemoteHold more = client_->more(bytes);
    if (!more) return std::nullopt;
    return std::make_shared<std::pair<RemoteHold, RemoteHold>>(remoteDelivering_, std::move(more));
}

// MARK: inputs (the host's order, on the dispatch thread)

void SandboxEngine::forward(std::function<void(SandboxRemoteChannel&)> call) {
    thread_.async([this, call = std::move(call)] {
        if (!dead_) call(*remote_);
    });
}

void SandboxEngine::clientStart() {
    thread_.async([this] {
        if (dead_ || started_) return;
        started_ = true;
        remote_->start(html_, config_);
    });
}

void SandboxEngine::clientKill(const std::string& reason, bool atOnce) {
    if (atOnce && thread_.isCurrent()) return remoteKill(reason, true);
    thread_.async([this, reason] { remoteKill(reason, true); });
}

void SandboxEngine::clientActivationEvent(const std::string& json, int gesture) {
    const double at = nowMs();
    thread_.async([this, json, gesture, at] {
        if (dead_) return;
        const auto same = [&](const Grant& grant) { return grant.gesture == gesture; };
        if (visible_ && gesture > consumedGesture_ && std::none_of(grants_.begin(), grants_.end(), same)) {
            const auto later = std::upper_bound(grants_.begin(), grants_.end(), gesture, [](int g, const Grant& grant) { return g < grant.gesture; });
            grants_.insert(later, Grant{gesture, at});
            if (grants_.size() > kRemoteMaxGrants) grants_.erase(grants_.begin(), grants_.end() - long(kRemoteMaxGrants));
        }
        remote_->activationEvent(json, gesture);
    });
}

void SandboxEngine::clientSetVisible(bool v) {
    thread_.async([this, v] {
        if (dead_ || visible_ == v) return;
        visible_ = v;
        // On screen again: the next background request may go out at once, as in process.
        if (v) requests_.show(nowMs());
        remote_->setVisible(v);
    });
}

void SandboxEngine::clientPress(bool began) {
    const double at = nowMs();
    thread_.async([this, began, at] {
        if (dead_) return;
        if (began) {
            haptics_.press(at);
            remote_->pressBegan();
        } else {
            haptics_.release(at);
            remote_->pressEnded();
        }
    });
}

void SandboxEngine::clientSetDelegate(SandboxEngineDelegate* delegate) {
    auto set = [this, delegate] {
        // An open task's begin went to the delegate before: another one never hears an end without its begin.
        if (delegate != delegate_) remoteTaskOpen_ = false;
        delegate_ = delegate;
        deliverPendingDeath();
    };
    if (thread_.isCurrent()) set();
    else thread_.async(set);
}

std::string SandboxEngine::clientProbe(const std::string& code) {
    // The engine's side may need the dispatch thread to answer (a layout query): it never waits for itself.
    if (thread_.isCurrent()) return "ERROR no runtime";
    if (!Posted<bool>(thread_, [this] { return !dead_ && started_; }).get().value_or(false)) return "ERROR no runtime";
    std::string answer = remote_->probe(code);
    answer.resize(std::min(answer.size(), kRemoteMessageLimit));
    // What the engine's side sent before its answer reaches the delegate before this returns, as in process.
    Posted<bool>(thread_, [] { return true; }).get();
    return answer;
}

// MARK: privileged calls (decided on the host's own taps, visibility and flags)

bool SandboxEngine::liveGrants() {
    const double now = nowMs();
    const auto spent = [&](const Grant& grant) { return now - grant.atMs > kRemoteGrantWindowMs || grant.gesture <= consumedGesture_; };
    grants_.erase(std::remove_if(grants_.begin(), grants_.end(), spent), grants_.end());
    return visible_ && !grants_.empty();
}

void SandboxEngine::spendGrant() {
    if (grants_.empty()) return;
    consumedGesture_ = grants_.front().gesture;
    grants_.erase(grants_.begin());
}

bool SandboxEngine::clientActivation(const std::string& method) {
    if (!isStrictPolicy(config_.policy) || !sdk::isPrivilegedMethod(method)) return true;
    if (!liveGrants()) {
        clientReport(blockedBridgeReported_, "activation-blocked", method + " ignored by the host: no tap on the visible widget allows it");
        return false;
    }
    spendGrant();
    return true;
}

// Swift's clientRequest with its refusal codes; the in-process engine's own refusals, with their reasons, already happened on the engine's side.
void SandboxEngine::clientRequest(int64_t id, const std::optional<WidgetRequest>& request, const std::optional<WalletCall>& wallet) {
    if (dead_) return;
    const auto refuse = [&](WidgetRequestFailure failure) { remote_->answer(id, SandboxRemoteOutcome::failed(failure)); };
    // The host's flags as the constructor normalized them: a preview asks nothing, the wallet only with canQuery.
    const WidgetContext& c = config_.context;
    if ((!request && !wallet) || c.preview || !c.canQuery || requests_.busy()) return refuse(WidgetRequestFailure::NotAllowed);
    const bool strict = isStrictPolicy(config_.policy);
    const bool pressed = !strict || liveGrants();
    sdk::RequestVerdict verdict;
    if (request) {
        // The engine's side holds a hidden widget's background request until the host shows it: one that comes hidden is not held here.
        if (!visible_) return refuse(WidgetRequestFailure::RateLimited);
        verdict = requests_.decide(nowMs(), {pressed, visible_, c.canQuery, c.backgroundQuery, request->wallet, c.wallet});
        if (verdict == sdk::RequestVerdict::NoActivation && !request->wallet) return refuse(WidgetRequestFailure::RateLimited);
    } else {
        verdict = requests_.decideWallet(nowMs(), {pressed, visible_, c.canQuery, c.wallet});
    }
    if (std::optional<std::string_view> error = sdk::requestRefusal(verdict))
        return refuse(*error == "RATE_LIMITED" ? WidgetRequestFailure::RateLimited : WidgetRequestFailure::NotAllowed);
    if (verdict == sdk::RequestVerdict::Foreground && strict) spendGrant();
    // The in-process responder's once and liveness; the outcome goes back as it came, and only a checked answer brings the wallet grant.
    auto once = std::make_shared<std::atomic<bool>>(false);
    WidgetRequestResponder respond([life = liveness_, once, id](const std::optional<WidgetRequestOutcome>& outcome) {
        if (once->exchange(true)) return;
        std::lock_guard<std::mutex> l(life->mutex);
        SandboxEngine* engine = life->engine;
        if (!engine) return;
        engine->thread_.async([engine, id, outcome] {
            if (engine->dead_) return;
            if (outcome && outcome->failure == WidgetRequestFailure::None && sdk::checkRequestAnswer(outcome->answer)) engine->requests_.finish(nowMs());
            else engine->requests_.finish();
            engine->remote_->answer(id, SandboxRemoteOutcome::from(outcome));
        });
    });
    if (!delegate_) return respond.unsupported();
    if (request) delegate_->onBridgeRequest(*this, *request, std::move(respond));
    else delegate_->onBridgeWallet(*this, *wallet, std::move(respond));
}

// MARK: death

void SandboxEngine::remoteKill(const std::string& reason, bool tellRemote) {
    if (dead_) return;
    dead_ = true;
    const std::string bounded = reason.substr(0, kRemoteTextLimit);
    const bool taskOpen = std::exchange(remoteTaskOpen_, false);
    if (delegate_) {
        delegate_->onDied(*this, bounded);
        // The engine's own end of that task cannot come through a closed channel.
        if (taskOpen) delegate_->onTaskCompleted(*this);
    } else {
        pendingDeath_ = bounded;
    }
    if (!remote_) return;
    if (tellRemote) remote_->stop(bounded);
    remote_->close();
}

void SandboxEngine::clientReport(int& counter, const char* kind, const std::string& detail) {
    if (dead_ || !delegate_ || counter >= sdk::MAX_REJECTION_REPORTS) return;
    counter++;
    delegate_->onDiag(*this, kind, detail);
}

void SandboxEngine::deliverPendingDeath() {
    if (!pendingDeath_ || !delegate_) return;
    const std::string reason = std::move(*pendingDeath_);
    pendingDeath_.reset();
    delegate_->onDied(*this, reason);
}

} // namespace twidget
