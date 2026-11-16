// Client mode of SandboxEngine, the twin of SandboxEngine.swift's: no runtime here, the widget runs behind a SandboxRemoteChannel (remote_engine.h); a hosted client's widget runs with its validator and renderer behind a SandboxHostedChannel (remote_hosted.h, desktop core only).
#include "twidget/sandbox_engine.h"

#include "twidget/json.h"
#include "twidget/remote_engine.h"
#include "twidget/remote_hosted.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>

namespace twidget {

namespace {

constexpr const char* kEngineGone = "the widget engine process ended";
constexpr const char* kEngineFlooded = "the widget engine process sent more than the host keeps queued";
/// The diagnostic and the deaths of a hosted widget whose renderer's platform request went unanswered.
constexpr const char* kPlatformTimeout = "platform-timeout";
constexpr const char* kPlatformLate = "the host's platform did not answer the widget's renderer within 3 s";
constexpr const char* kPlatformNone = "the host's platform answers no requests of the widget's renderer";
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

} // namespace

// MARK: the sink

/// What the channel calls back, also after the engine is gone: the liveness lock only covers seeing the engine and queueing work, never a wait or a call out. What it queues is admitted first against `kRemoteQueuedBytes` and `kRemoteQueuedCalls`, before anything of the message is copied, and stays counted while the work its delegate queued with it waits (`remoteHold`). A hosted client's engine's side keeps the render calls (ops, queries, listen, preventDefault, anim, frame requests) to itself, so each is a protocol violation here, refused before it is admitted; an engine-only client refuses a hosted runtime's calls the same way.
class SandboxEngine::RemoteClient final : public SandboxRemoteSink {
public:
    RemoteClient(std::shared_ptr<Liveness> life, bool hosted) : life_(std::move(life)), hosted_(hosted) {}

    bool hosted() const { return hosted_; }
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
        if (hosted_) return violation("an ops batch from a hosted engine process");
        if (json.size() > kRemoteOpsLimit) return violation("an ops batch over 32 MB");
        if (Admitted admitted = admit(json.size())) deliver(std::move(admitted), [json](SandboxEngine& engine, SandboxEngineDelegate& delegate) { delegate.onOps(engine, json); });
    }

    void remoteListen(const std::string& json) override {
        if (hosted_) return violation("listen from a hosted engine process");
        if (json.size() > kRemoteMessageLimit) return violation("listen over 1 MB");
        if (Admitted admitted = admit(json.size())) deliver(std::move(admitted), [json](SandboxEngine& engine, SandboxEngineDelegate& delegate) { delegate.onListen(engine, json); });
    }

    void remotePreventDefault(const std::string& json) override {
        if (hosted_) return violation("preventDefault from a hosted engine process");
        if (json.size() > kRemoteMessageLimit) return violation("preventDefault over 1 MB");
        if (Admitted admitted = admit(json.size())) deliver(std::move(admitted), [json](SandboxEngine& engine, SandboxEngineDelegate& delegate) { delegate.onPreventDefault(engine, json); });
    }

    void remoteAnim(const std::string& json) override {
        if (hosted_) return violation("anim from a hosted engine process");
        if (json.size() > kRemoteMessageLimit) return violation("anim over 1 MB");
        if (Admitted admitted = admit(json.size())) deliver(std::move(admitted), [json](SandboxEngine& engine, SandboxEngineDelegate& delegate) { delegate.onAnim(engine, json); });
    }

    void remoteRequestsFrame() override {
        if (hosted_) return violation("a frame request from a hosted engine process");
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

    // Checked as the engine checks it (lone surrogates kept for the SDK to refuse), only an action, then the preview, the host's tap and haptic gate; what goes on is the checked arguments re-serialized, as in process, far under the cut admitted for them.
    void remoteBridge(const std::string& method, const std::string& argsJson) override {
        const bool sized = method.size() <= kRemoteNameLimit && argsJson.size() <= kRemoteMessageLimit;
        Admitted admitted = admit(sized ? method.size() + argsJson.size() : kRemoteNameLimit);
        if (!admitted) return;
        std::optional<WidgetAction> action;
        std::string checkedArgs;
        if (sized) {
            std::optional<Json> args = Json::parse(argsJson, Json::Surrogates::Keep);
            std::optional<sdk::BridgeCall> checked = args ? sdk::checkBridgeCall(Json(method), *args) : std::nullopt;
            if (checked && checked->action) {
                action = std::move(checked->action);
                checkedArgs = args->dump();
            }
        }
        if (!action) {
            return submit(std::move(admitted), [name = method.substr(0, kRemoteNameLimit)](SandboxEngine& engine) {
                engine.clientReport(engine.rejectedBridgeReported_, "bridge-rejected", name + ": rejected by the host (unknown method or malformed arguments)");
            });
        }
        submit(std::move(admitted), [method, checkedArgs = std::move(checkedArgs), action = std::move(*action)](SandboxEngine& engine) {
            if (engine.dead_ || (engine.config_.context.preview && !sdk::allowedInPreview(method)) || !engine.clientActivation(method)) return;
            // Dropped silently: the engine's side reports its own `haptic-limit`.
            if (action.kind == WidgetAction::Haptic && engine.haptics_.decide(nowMs(), engine.visible_) != sdk::HapticGate::Decision::Play) return;
            if (!engine.delegate_) return;
            engine.delegate_->onBridgeAction(engine, action);
            engine.delegate_->onBridge(engine, method, checkedArgs);
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
        if (hosted_) {
            violation("a layout query from a hosted engine process");
            return {"{\"error\":\"invalid query\"}", std::nullopt};
        }
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

    // A death a widget kept past it (`keepAtDeath_`) or a kept route hears closes nothing: the engine's side's own report of a keep's stop, and any later one, has no effect there.
    void remoteDied(const std::string& reason) override {
        Admitted admitted = admit(std::min(reason.size(), kRemoteTextLimit));
        if (!admitted) return;
        submit(std::move(admitted), [reason = reason.substr(0, kRemoteTextLimit)](SandboxEngine& engine) {
            engine.remoteDeath(reason);
            if (!engine.keepAtDeath_ && !engine.kept_) engine.closeRoute(reason, false, false);
        });
    }

    void remoteDisconnected() override {
        if (Admitted admitted = admit(0)) submit(std::move(admitted), [](SandboxEngine& engine) { engine.remoteKill(kEngineGone, false, true); });
    }

    // A hosted runtime's calls: sized first (a message past its bound is dropped, a request past it answered ""), then admitted as any other. The renderer's platform traffic is the route's, not the widget's: it is carried while the route is open, past the widget's death on a route kept open.
    void remotePlatformMessage(const std::string& message) override {
        if (!hosted_) return violation("a platform message from an engine-only runtime");
        if (message.size() > kRemotePlatformLimit) return violation("a platform message over 32 MB");
        if (Admitted admitted = admit(message.size())) {
            submit(std::move(admitted), [message](SandboxEngine& engine) {
                if (engine.routeOpen_ && engine.delegate_) engine.delegate_->onHostedMessage(engine, message);
            });
        }
    }

    // Delivered on the dispatch thread in order with the other calls, then waited for here, on the caller's thread (the transport's), up to `kPlatformAnswerSeconds` from its arrival: the dispatch thread never waits for the host's platform, so a keep (`keepHosted`) can switch the delegate while a request waits and the request survives it. One at a time; none at once once the route closed or the client is going.
    std::optional<std::string> remotePlatformRequest(const std::string& message) override {
        if (!hosted_) {
            violation("a platform request from an engine-only runtime");
            return std::nullopt;
        }
        if (message.size() > kRemotePlatformLimit) {
            violation("a platform request over 32 MB");
            return std::string();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(kPlatformAnswerSeconds));
        auto call = std::make_shared<PlatformCall>();
        bool second = false;
        {
            // The closed check and the publish in one step: a close settles this call or the request answers none here.
            std::lock_guard<std::mutex> l(platformMutex_);
            if (requestsClosed_) return std::nullopt;
            second = waiting_ != nullptr;
            if (!second) waiting_ = call;
        }
        if (second) {
            violation("a second platform request while one waits");
            return std::nullopt;
        }
        Admitted admitted = admit(message.size());
        if (!admitted) {
            finished(call);
            return std::nullopt;
        }
        submit(std::move(admitted), [call, message](SandboxEngine& engine) {
            if (!engine.routeOpen_ || !engine.delegate_) return call->settle(std::nullopt, false);
            engine.delegate_->onHostedRequest(engine, message, hostedResponder([call](std::optional<std::string> answer) {
                const bool none = !answer;
                call->settle(std::move(answer), none);
            }));
        });
        std::optional<std::string> answer;
        bool none = false, late = false;
        {
            std::unique_lock<std::mutex> l(call->mutex);
            late = !call->cv.wait_until(l, deadline, [&] { return call->settled; });
            // A `respond` from now on is late: ignored.
            call->settled = true;
            answer = std::move(call->answer);
            none = call->none;
        }
        finished(call);
        if (answer) return answer;
        // Settled by a close: no kill, the route is ending already.
        if (late || none) submit(nullptr, [reason = late ? kPlatformLate : kPlatformNone](SandboxEngine& engine) { engine.platformKill(reason); });
        return std::nullopt;
    }

    /// The route closed or the client is going: the waiting platform request answers none at once, without a kill, and so does every later one.
    void settleWaiting() {
        std::shared_ptr<PlatformCall> call;
        {
            std::lock_guard<std::mutex> l(platformMutex_);
            requestsClosed_ = true;
            call = std::move(waiting_);
            waiting_.reset();
        }
        if (call) call->settle(std::nullopt, false);
    }

    // Only for a host that asked at its hosted start (`hostedOps_`, set on the dispatch thread before anything after the start is delivered).
    void remoteValidatedOps(const std::string& json) override {
        if (!hosted_) return violation("validated ops from an engine-only runtime");
        if (json.size() > kRemoteOpsLimit) return violation("validated ops over 32 MB");
        Admitted admitted = admit(json.size());
        if (!admitted) return;
        submit(std::move(admitted), [json](SandboxEngine& engine) {
            if (!engine.hostedOps_) return engine.clientReport(engine.rejectedBridgeReported_, "protocol-violation", "validated ops the host did not ask for");
            if (!engine.dead_ && engine.delegate_) engine.delegate_->onHostedOps(engine, json);
        });
    }

    // Noted also for a dead widget (`hostedReady_`): a renderer that announced itself is one a host may keep.
    void remoteRendererReady() override {
        if (!hosted_) return violation("a renderer's ready from an engine-only runtime");
        if (Admitted admitted = admit(0)) {
            submit(std::move(admitted), [](SandboxEngine& engine) {
                engine.hostedReady_ = true;
                if (!engine.dead_ && engine.delegate_) engine.delegate_->onHostedRendererReady(engine);
            });
        }
    }

    void remoteRendered() override {
        if (!hosted_) return violation("a render report from an engine-only runtime");
        if (Admitted admitted = admit(0)) deliver(std::move(admitted), [](SandboxEngine& engine, SandboxEngineDelegate& delegate) { delegate.onHostedRendered(engine); });
    }

    void remoteNodes(int64_t nodes) override {
        if (!hosted_) return violation("a node count from an engine-only runtime");
        const int bounded = int(std::clamp<int64_t>(nodes, 0, std::numeric_limits<int>::max()));
        if (Admitted admitted = admit(0)) deliver(std::move(admitted), [bounded](SandboxEngine& engine, SandboxEngineDelegate& delegate) { delegate.onHostedNodes(engine, bounded); });
    }

private:
    /// A platform request waiting for the host's platform on its caller's thread: settled once, by its responder, a close, or its waiter at the timeout.
    struct PlatformCall {
        std::mutex mutex;
        std::condition_variable cv;
        bool settled = false;
        std::optional<std::string> answer;
        /// Settled by `unanswered()`: no platform for it.
        bool none = false;
        /// Once; later calls are ignored.
        void settle(std::optional<std::string> value, bool unanswered) {
            {
                std::lock_guard<std::mutex> l(mutex);
                if (settled) return;
                settled = true;
                answer = std::move(value);
                none = unanswered;
            }
            cv.notify_all();
        }
    };
    /// `call` is no longer the waiting one.
    void finished(const std::shared_ptr<PlatformCall>& call) {
        std::lock_guard<std::mutex> l(platformMutex_);
        if (waiting_ == call) waiting_.reset();
    }

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
        if (!backlog_->overflowed.exchange(true)) submit(nullptr, [](SandboxEngine& engine) { engine.remoteKill(kEngineFlooded, true, true); });
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

    /// `fn`'s value from the dispatch thread after everything queued before it, holding `admitted` while `fn` runs (`remoteHold` shares it) and giving it back as `fn` returns, before its value is published, so a caller with the value no longer finds its call counted unless work the delegate queued still shares it; nullopt once the engine is gone or its thread stopped.
    template <class R, class F>
    std::optional<R> await(Admitted admitted, F fn) {
        std::optional<Posted<R>> posted;
        {
            std::lock_guard<std::mutex> l(life_->mutex);
            SandboxEngine* engine = life_->engine;
            if (!engine) return std::nullopt;
            posted.emplace(engine->thread_, [engine, admitted = std::move(admitted), fn = std::move(fn)]() mutable {
                Delivering delivering(*engine, std::move(admitted));
                return fn(*engine);
            });
        }
        return posted->get();
    }

    std::shared_ptr<Liveness> life_;
    const bool hosted_;
    const std::shared_ptr<Backlog> backlog_ = std::make_shared<Backlog>();
    /// The platform request waiting now, and whether requests are closed (`settleWaiting`).
    std::mutex platformMutex_;
    std::shared_ptr<PlatformCall> waiting_;
    bool requestsClosed_ = false;
};

// MARK: construction

SandboxEngine::SandboxEngine(std::string html, SandboxConfig config, std::shared_ptr<SandboxRemoteChannel> remote, std::shared_ptr<IntlProvider> intl)
    : SandboxEngine("widget.client", std::move(html), std::move(config), std::shared_ptr<SandboxResources>(), std::move(intl)) {
    attachClient(std::move(remote), false);
}

SandboxEngine::SandboxEngine(HostedTag, std::string html, SandboxConfig config, std::shared_ptr<SandboxHostedChannel> hosted, std::shared_ptr<IntlProvider> intl)
    : SandboxEngine("widget.client", std::move(html), std::move(config), std::shared_ptr<SandboxResources>(), std::move(intl)) {
    hosted_ = hosted;
    attachClient(std::move(hosted), true);
}

void SandboxEngine::attachClient(std::shared_ptr<SandboxRemoteChannel> remote, bool hosted) {
    remote_ = std::move(remote);
    client_ = std::make_shared<RemoteClient>(liveness_, hosted);
    // Before anything the channel delivers: its calls run on the dispatch thread after this.
    routeOpen_ = bool(remote_);
    // A host's factory without a channel: dead from the first task, never in process.
    if (remote_) remote_->attach(client_);
    else thread_.async([this] { remoteKill(kEngineGone, false, false); });
}

std::unique_ptr<SandboxEngine> SandboxEngine::hostedClient(std::string html, SandboxConfig config, std::shared_ptr<SandboxHostedChannel> hosted, std::shared_ptr<IntlProvider> intl) {
    return std::unique_ptr<SandboxEngine>(new SandboxEngine(HostedTag{}, std::move(html), std::move(config), std::move(hosted), std::move(intl)));
}

bool SandboxEngine::isHosted() const { return client_ && client_->hosted(); }

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
    if (atOnce && thread_.isCurrent()) return remoteKill(reason, true, false);
    thread_.async([this, reason] { remoteKill(reason, true, false); });
}

void SandboxEngine::clientActivationEvent(const std::string& json, int gesture) {
    const double at = nowMs();
    thread_.async([this, json, gesture, at] {
        if (dead_) return;
        grant(gesture, at);
        remote_->activationEvent(json, gesture);
    });
}

void SandboxEngine::grant(int gesture, double atMs) {
    const auto same = [&](const Grant& kept) { return kept.gesture == gesture; };
    if (!visible_ || gesture <= consumedGesture_ || std::any_of(grants_.begin(), grants_.end(), same)) return;
    const auto later = std::upper_bound(grants_.begin(), grants_.end(), gesture, [](int g, const Grant& kept) { return g < kept.gesture; });
    grants_.insert(later, Grant{gesture, atMs});
    if (grants_.size() > kRemoteMaxGrants) grants_.erase(grants_.begin(), grants_.end() - long(kRemoteMaxGrants));
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

// MARK: a hosted client's own inputs (the host's order, on the dispatch thread)

void SandboxEngine::startHosted(SandboxHostedStart start, bool keepAtDeath) {
    if (!hosted_) return;
    thread_.async([this, start = std::move(start), keepAtDeath] {
        if (dead_ || started_) return;
        started_ = true;
        hostedOps_ = start.validatedOps;
        keepAtDeath_ = keepAtDeath;
        hosted_->startHosted(html_, config_, start);
    });
}

void SandboxEngine::hostedTheme(const WidgetTheme& theme) {
    if (!hosted_) return;
    thread_.async([this, theme] {
        if (!dead_) hosted_->hostedTheme(theme);
    });
}

void SandboxEngine::platform(std::string message) {
    if (!hosted_) return;
    thread_.async([this, message = std::move(message)] {
        if (routeOpen_) hosted_->platform(message);
    });
}

// The old delegate hears a live widget's death before the switch, the new one nothing of it; what the old one queued with its own threads is its business (WidgetHost::keepRenderer drains it).
bool SandboxEngine::keepHosted(SandboxEngineDelegate& kept) {
    if (!hosted_) return false;
    return Posted<bool>(thread_, [this, &kept] {
        if (!started_ || !routeOpen_ || !hostedReady_ || kept_) return false;
        if (!dead_) {
            remoteDeath("terminated");
            remote_->stop("terminated");
        }
        grants_.clear();
        remoteTaskOpen_ = false;
        delegate_ = &kept;
        kept_ = true;
        hosted_->kept();
        return true;
    }).get().value_or(false);
}

// The press is the grant and the touch at once (packages/desktop/README.md, "Who decides"); stamped here with the clock the gates read, as `clientPress` does.
void SandboxEngine::pressDelivered() {
    if (!hosted_) return;
    const double at = nowMs();
    thread_.async([this, at] {
        if (dead_ || config_.context.preview) return;
        haptics_.press(at);
        if (visible_) grant(++hostedGesture_, at);
    });
}

void SandboxEngine::releaseDelivered() {
    if (!hosted_) return;
    const double at = nowMs();
    thread_.async([this, at] {
        if (!dead_ && !config_.context.preview) haptics_.release(at);
    });
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

void SandboxEngine::remoteKill(const std::string& reason, bool hostDecided, bool ended) {
    const bool lived = remoteDeath(reason);
    // A dead route (a kept one, or one kept open past its death) closes with the close alone: its engine is gone already.
    closeRoute(reason, hostDecided && lived, ended);
}

bool SandboxEngine::remoteDeath(const std::string& reason) {
    if (dead_) return false;
    dead_ = true;
    const std::string bounded = reason.substr(0, kRemoteTextLimit);
    const bool taskOpen = std::exchange(remoteTaskOpen_, false);
    if (delegate_) {
        delegate_->onDied(*this, bounded);
        // The engine's own end of that task reaches no delegate after the death.
        if (taskOpen) delegate_->onTaskCompleted(*this);
    } else {
        pendingDeath_ = bounded;
    }
    return true;
}

void SandboxEngine::closeRoute(const std::string& reason, bool stop, bool ended) {
    if (!routeOpen_) return;
    routeOpen_ = false;
    clientSettleWaiting();
    const std::string bounded = reason.substr(0, kRemoteTextLimit);
    if (stop) remote_->stop(bounded);
    remote_->close();
    if (kept_ && ended && delegate_) delegate_->onHostedEnded(*this, bounded);
}

void SandboxEngine::platformKill(const std::string& reason) {
    if (!routeOpen_) return;
    if (delegate_) delegate_->onDiag(*this, kPlatformTimeout, reason);
    remoteKill(reason, true, true);
}

void SandboxEngine::clientSettleWaiting() { client_->settleWaiting(); }

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
