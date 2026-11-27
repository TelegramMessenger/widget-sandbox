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

/// Every hosted client's platform requests are numbered from one counter, so a number names one request in the process (`SandboxHostedWait::admitted`).
std::atomic<uint64_t> platformRequests{0};

} // namespace

// MARK: a platform request's deadline

namespace detail {

/// A platform request waiting for the host's platform on its caller's thread: settled once, by its responder, a close, or its waiter at its deadline. Due `kPlatformAnswerSeconds` after its arrival unless its client's SandboxHostedWait moves that, always under `mutex`, the waiter woken at each move to read it again.
struct PlatformCall {
    using Clock = std::chrono::steady_clock;
    static Clock::duration answerTime() { return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(SandboxEngine::kPlatformAnswerSeconds)); }

    PlatformCall(uint64_t number, Clock::time_point arrival) : number(number), arrival(arrival), deadline(arrival + answerTime()) {}

    const uint64_t number;
    const Clock::time_point arrival;
    std::mutex mutex;
    std::condition_variable cv;
    Clock::time_point deadline;
    /// The end of the input exemption that keeps a later release from holding it now (SandboxHostedWait): the waiter wakes then too and reads the record again (`HostedWaits::recheck`), a renewal moving it; none while nothing would hold it.
    std::optional<Clock::time_point> check;
    /// The embedder's platform took it (SandboxHostedWait::admitted): no release moves its deadline again.
    bool admitted = false;
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
    /// Under `mutex`: due at `at`, the waiter woken when that is a change.
    void dueAt(Clock::time_point at) {
        if (at == deadline) return;
        deadline = at;
        cv.notify_all();
    }
    /// Under `mutex`: read again at `at` (none: never), the waiter woken when that is a change.
    void checkAt(std::optional<Clock::time_point> at) {
        if (at == check) return;
        check = at;
        cv.notify_all();
    }
};

/// What a hosted client tells its SandboxHostedWait, under the record's lock and then the call's, never the other way round.
struct HostedWaits {
    /// The one rule for the waiting `call` at `now`, under both locks, while it is neither admitted nor settled: the meter holds the widget now (a release ahead and no exemption in force) before the call is due, or held it since its `check` passed with no renewal before it (the exemption ended with the release still ahead, which a check never set past the deadline makes before the call was due), so it is due `kPlatformAnswerSeconds` after that release if that is later; an exemption in force that ends before the deadline with the release past its end is read again at that end; otherwise nothing holds it and its deadline stays. An exemption renewed before it ends never extends it, and while one is in force the deadline never grows, so a check stays before it.
    static void apply(SandboxHostedWait& wait, PlatformCall& call, PlatformCall::Clock::time_point now) {
        if (call.settled || call.admitted || !wait.release_) return call.checkAt(std::nullopt);
        const PlatformCall::Clock::time_point release = *wait.release_;
        const std::optional<PlatformCall::Clock::time_point> exemption = wait.exemptUntil_;
        const bool exempt = exemption && *exemption > now;
        const bool lapsed = call.check && *call.check <= now;
        const bool held = !exempt && release > now && now < call.deadline;
        if (lapsed || held) call.dueAt(std::max(call.deadline, release + PlatformCall::answerTime()));
        call.checkAt(exempt && release > *exemption && *exemption < call.deadline ? exemption : std::nullopt);
    }
    /// `call` waits from now on, the newest request (an older one no longer moves): the rule at its arrival, so it is due after the release of a hold in force then; one that arrives in an exemption is only read again at the exemption's end.
    static void began(SandboxHostedWait& wait, const std::shared_ptr<PlatformCall>& call) {
        std::lock_guard<std::mutex> l(wait.mutex_);
        wait.waiting_ = call;
        std::lock_guard<std::mutex> c(call->mutex);
        apply(wait, *call, call->arrival);
    }
    /// `call`'s waiter at its `check`, holding no lock: the rule again, now, so a renewed exemption moves the check and an ended one extends the deadline; a call no longer the record's is never read again.
    static void recheck(SandboxHostedWait& wait, const std::shared_ptr<PlatformCall>& call) {
        std::lock_guard<std::mutex> l(wait.mutex_);
        std::lock_guard<std::mutex> c(call->mutex);
        if (wait.waiting_ == call) apply(wait, *call, PlatformCall::Clock::now());
        else call->checkAt(std::nullopt);
    }
    /// The record changed (`hold`, `exempt`): the rule for the call waiting now, if any.
    static void changed(SandboxHostedWait& wait) {
        if (!wait.waiting_) return;
        std::lock_guard<std::mutex> c(wait.waiting_->mutex);
        apply(wait, *wait.waiting_, PlatformCall::Clock::now());
    }
    /// `call`'s caller no longer waits.
    static void ended(SandboxHostedWait& wait, const std::shared_ptr<PlatformCall>& call) {
        std::lock_guard<std::mutex> l(wait.mutex_);
        if (wait.waiting_ == call) wait.waiting_.reset();
    }
};

/// What a hosted client tells its channel of the death it reports (`SandboxHostedChannel::death`).
struct HostedDeaths {
    /// The transport's end is the widget's first death: what the end made of it, published before the death is reported.
    static void ended(SandboxHostedChannel& channel) { channel.publishEndedDeath(); }
    /// A death of the widget's own is its first death, published before it is reported.
    static void own(SandboxHostedChannel& channel) { channel.publishOwnDeath(); }
};

} // namespace detail

void SandboxHostedWait::hold(Clock::time_point release) {
    if (release > Clock::time_point::max() - detail::PlatformCall::answerTime()) return;
    std::lock_guard<std::mutex> l(mutex_);
    if (release_ && release <= *release_) return;
    release_ = release;
    detail::HostedWaits::changed(*this);
}

void SandboxHostedWait::exempt(Clock::time_point until) {
    std::lock_guard<std::mutex> l(mutex_);
    if (exemptUntil_ && until <= *exemptUntil_) return;
    exemptUntil_ = until;
    detail::HostedWaits::changed(*this);
}

void SandboxHostedWait::admitted(uint64_t request) {
    std::lock_guard<std::mutex> l(mutex_);
    if (!waiting_ || waiting_->number != request) return;
    std::lock_guard<std::mutex> c(waiting_->mutex);
    if (waiting_->settled || waiting_->admitted) return;
    // A hold it met up to now still counts: an exemption that ended before its waiter woke to read it.
    const Clock::time_point now = Clock::now();
    detail::HostedWaits::apply(*this, *waiting_, now);
    waiting_->admitted = true;
    waiting_->checkAt(std::nullopt);
    waiting_->dueAt(std::min(waiting_->deadline, now + detail::PlatformCall::answerTime()));
}

std::optional<SandboxHostedWait::Clock::time_point> SandboxHostedWait::due(uint64_t request) const {
    std::lock_guard<std::mutex> l(mutex_);
    if (!waiting_ || waiting_->number != request) return std::nullopt;
    std::lock_guard<std::mutex> c(waiting_->mutex);
    if (waiting_->settled) return std::nullopt;
    return waiting_->deadline;
}

// MARK: the sink

/// What the channel calls back, also after the engine is gone: the liveness lock only covers seeing the engine and queueing work, never a wait or a call out. What it queues of the engine's side's is admitted first against `kRemoteQueuedBytes` and `kRemoteQueuedCalls`, before anything of the message is copied, and stays counted while the work its delegate queued with it waits (`remoteHold`); the transport's end (`remoteDisconnected`), the transport's word and not the engine's side's, is queued once without taking a place, so a backlog at its bounds never turns it into a flood, and it defers to a flood claimed before it runs. A hosted client's engine's side keeps the render calls (ops, queries, listen, preventDefault, anim, frame requests) to itself, so each is a protocol violation here, refused before it is admitted; an engine-only client refuses a hosted runtime's calls the same way.
class SandboxEngine::RemoteClient final : public SandboxRemoteSink {
public:
    RemoteClient(std::shared_ptr<Liveness> life, bool hosted, std::shared_ptr<SandboxHostedWait> wait) : life_(std::move(life)), hosted_(hosted), wait_(std::move(wait)) {}

    bool hosted() const { return hosted_; }
    RemoteBacklog backlog() const { return {backlog_->bytes.load(), backlog_->calls.load()}; }
    /// `SandboxEngine::setRemoteRejectionStep`: `step` under the step's lock, `choosing` by a job on the dispatch thread, the only thread that reads it, so the end's choice queued later finds it without that lock.
    void rejectionStep(std::function<void()> step, std::function<void()> choosing) {
        {
            std::lock_guard<std::mutex> l(backlog_->deciding);
            backlog_->rejecting = std::move(step);
        }
        submit(nullptr, [backlog = backlog_, choosing = std::move(choosing)](SandboxEngine&) { backlog->choosing = choosing; });
    }
    /// A place for `bytes` of work a delegate made from the message being delivered (`remoteHold`), by `admit`'s rules.
    RemoteHold more(size_t bytes) { return admit(bytes); }
    /// On the dispatch thread: a counted stop of the live hosted widget yielded to the transport's end that decided it first (`SandboxEngine::clientKill`): that end's kill, which this client queues when the transport tells it, reports the widget's death with that decision, and until then the client is as at the stop's own death (`yielded`): nothing of the widget's own reaches the delegate or its Intl provider (`live`), its renderer's platform traffic goes nowhere (`carries`), so no platform request of it kills it and no keep takes it, and a later stop or terminate leaves its death to that kill.
    void yield() { yielded_ = true; }
    /// On the dispatch thread: a counted stop of `engine`'s widget yielded to the transport's end (`yield`), from then on; once that end's kill ran the widget is dead as well.
    static bool yielded(const SandboxEngine& engine) { return engine.client_->yielded_; }
    /// On the dispatch thread: what the engine's side sends of the widget's own (its batches, storage, bridge calls, requests, Intl requests, reports, its task's begin) may still reach `engine`'s delegate or its Intl provider: the widget has not died and no counted stop of it yielded to an end (`yielded`). Its death, its task's end and its renderer's platform traffic, the route's, have rules of their own.
    static bool live(const SandboxEngine& engine) { return !engine.dead_ && !yielded(engine); }
    /// On the dispatch thread: `engine`'s route still carries its renderer's platform traffic (the delivery of its messages and requests, a request's kill, a keep): it is open, a kept one and one kept open past the widget's death included, and no counted stop yielded on it (`yielded`), whose end's kill closes it.
    static bool carries(const SandboxEngine& engine) { return engine.routeOpen_ && !yielded(engine); }

    // The engine's side sends a task's begin, that task's calls, then its end, never nested; a begin no delegate heard has no end here, and a death ends an open task.
    void remoteTaskStarted() override {
        if (Admitted admitted = admit(0)) {
            submit(std::move(admitted), [](SandboxEngine& engine) {
                if (!live(engine) || engine.remoteTaskOpen_ || !engine.delegate_) return;
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
                if (live(engine) && engine.visible_ && engine.delegate_) engine.delegate_->onRequestFrame(engine);
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
            if (!live(engine) || (engine.config_.context.preview && !sdk::allowedInPreview(method)) || !engine.clientActivation(method)) return;
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
            if (!live(engine) || engine.config_.context.preview) return;
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
            if (!live(engine) || engine.config_.context.preview) return;
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
            // A profile page never moves when its height changes, whatever the engine's side reports: the host's own place decides, as the engine's check does in process.
            deliver(std::move(admitted), [anchor = std::move(*anchor)](SandboxEngine& engine, SandboxEngineDelegate& delegate) {
                if (surface::keepsResizeAnchor(engine.config_.context.surface)) delegate.onResizeAnchor(engine, anchor);
            });
        }
    }

    void remoteWatchesPublicData(bool on) override {
        if (Admitted admitted = admit(0)) {
            submit(std::move(admitted), [on](SandboxEngine& engine) {
                if (live(engine) && !engine.config_.context.preview && engine.delegate_) engine.delegate_->onPublicDataWatch(engine, on);
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
            if (!live(engine) || !engine.delegate_) return deadQuery();
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
            return live(engine) ? engine.intl_->format(request) : std::string();
        }).value_or("");
        // It goes back as one message: a provider's answer over the limit is none.
        return answer.size() > kRemoteMessageLimit ? std::string() : answer;
    }

    // A death a widget kept past it (`keepAtDeath_`) or a kept route hears closes nothing: the engine's side's own report of a keep's stop, and any later one, has no effect there.
    void remoteDied(const std::string& reason) override {
        Admitted admitted = admit(std::min(reason.size(), kRemoteTextLimit));
        if (!admitted) return;
        submit(std::move(admitted), [reason = reason.substr(0, kRemoteTextLimit)](SandboxEngine& engine) {
            ownDeath(engine);
            engine.remoteDeath(reason);
            if (!engine.keepAtDeath_ && !engine.kept_) engine.closeRoute(reason, false, false);
        });
    }

    // Once, in order after what came before it, without a place of the engine's side's traffic: a backlog of exactly `kRemoteQueuedCalls` legal messages still hears the end as the end. An overflow claimed by the time the end runs on the dispatch thread came of the engine's side's traffic before the end (the reader's, a route's `widget.answer`, a delegate's `remoteHold`), so the end defers to it: the widget dies of that flood, its own death with its reason, whether the overflow's kill was queued before the end or after it (a `widget.answer` crossing the reader's fan-out). The end's choice and a rejection of a message are one step each under the backlog's `deciding` (`admit`): a message the admission found past the bounds before the end chose is that flood, one it finds past them after the choice is not. Otherwise a hosted widget's channel tells what the end was to it (`SandboxHostedChannel::death`), only for the widget's first death: one reported before it (its own, a flood, a platform request left unanswered, a keep) stays as it was.
    void remoteDisconnected() override {
        if (disconnected_.exchange(true)) return;
        submit(nullptr, [backlog = backlog_](SandboxEngine& engine) {
            if (backlog->flooded()) return flooded(engine);
            if (engine.hosted_ && !engine.dead_) detail::HostedDeaths::ended(*engine.hosted_);
            engine.remoteKill(kEngineGone, false, true);
        });
    }

    // A hosted runtime's calls: sized first (a message past its bound is dropped, a request past it answered ""), then admitted as any other. The renderer's platform traffic is the route's, not the widget's: it is carried while the route is open, past the widget's death on a route kept open, and not once a counted stop yielded to the transport's end (`carries`).
    void remotePlatformMessage(const std::string& message) override {
        if (!hosted_) return violation("a platform message from an engine-only runtime");
        if (message.size() > kRemotePlatformLimit) return violation("a platform message over 32 MB");
        if (Admitted admitted = admit(message.size())) {
            submit(std::move(admitted), [message](SandboxEngine& engine) {
                if (carries(engine) && engine.delegate_) engine.delegate_->onHostedMessage(engine, message);
            });
        }
    }

    // Delivered on the dispatch thread in order with the other calls, then waited for here, on the caller's thread (the transport's), until it is due (`kPlatformAnswerSeconds` from its arrival, or as `wait_` moves it, the waiter also waking at the end of an exemption a release outlasts to ask the record again): the dispatch thread never waits for the host's platform, so a keep (`keepHosted`) can switch the delegate while a request waits and the request survives it. One at a time; none at once once the route closed or the client is going, and none without a kill, as at a close, for one whose turn on the dispatch thread comes once the route no longer carries it (`carries`).
    std::optional<std::string> remotePlatformRequest(const std::string& message) override {
        if (!hosted_) {
            violation("a platform request from an engine-only runtime");
            return std::nullopt;
        }
        if (message.size() > kRemotePlatformLimit) {
            violation("a platform request over 32 MB");
            return std::string();
        }
        const auto arrival = std::chrono::steady_clock::now();
        std::shared_ptr<detail::PlatformCall> call;
        bool second = false;
        {
            // The closed check and the publish in one step: a close settles this call or the request answers none here.
            std::lock_guard<std::mutex> l(platformMutex_);
            if (requestsClosed_) return std::nullopt;
            second = waiting_ != nullptr;
            if (!second) waiting_ = call = std::make_shared<detail::PlatformCall>(++platformRequests, arrival);
        }
        if (second) {
            violation("a second platform request while one waits");
            return std::nullopt;
        }
        // Before the delegate can hear it: a release or an admission from now on is this request's, and none of an older one's moves it.
        if (wait_) detail::HostedWaits::began(*wait_, call);
        Admitted admitted = admit(message.size());
        if (!admitted) {
            finished(call);
            return std::nullopt;
        }
        submit(std::move(admitted), [call, message](SandboxEngine& engine) {
            if (!carries(engine) || !engine.delegate_) return call->settle(std::nullopt, false);
            engine.delegate_->onHostedRequest(engine, message, hostedResponder([call](std::optional<std::string> answer) {
                const bool none = !answer;
                call->settle(std::move(answer), none);
            }, call->number));
        });
        std::optional<std::string> answer;
        bool none = false, late = false;
        {
            std::unique_lock<std::mutex> l(call->mutex);
            while (!call->settled) {
                // Read again at every wake: a hold, an exemption or the admission may have moved either.
                const auto now = std::chrono::steady_clock::now();
                if (wait_ && call->check && now >= *call->check) {
                    // Before the deadline: a hold that resumed at the check may have moved it past now.
                    l.unlock();
                    detail::HostedWaits::recheck(*wait_, call);
                    l.lock();
                    continue;
                }
                if (now >= call->deadline) {
                    late = true;
                    break;
                }
                // A copy taken under the lock: `wait_until` reads its time again once it has the lock back, when an admission or a renewal may have ended the `check` it would refer to.
                const auto wake = call->check ? std::min(call->deadline, *call->check) : call->deadline;
                call->cv.wait_until(l, wake);
            }
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
        std::shared_ptr<detail::PlatformCall> call;
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
            if (live(engine) && engine.delegate_) engine.delegate_->onHostedOps(engine, json);
        });
    }

    // Noted also for a dead widget (`hostedReady_`): a renderer that announced itself is one a host may keep.
    void remoteRendererReady() override {
        if (!hosted_) return violation("a renderer's ready from an engine-only runtime");
        if (Admitted admitted = admit(0)) {
            submit(std::move(admitted), [](SandboxEngine& engine) {
                engine.hostedReady_ = true;
                if (live(engine) && engine.delegate_) engine.delegate_->onHostedRendererReady(engine);
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
    /// `call` is no longer the waiting one.
    void finished(const std::shared_ptr<detail::PlatformCall>& call) {
        {
            std::lock_guard<std::mutex> l(platformMutex_);
            if (waiting_ == call) waiting_.reset();
        }
        if (wait_) detail::HostedWaits::ended(*wait_, call);
    }

    /// What is queued and has neither run nor been dropped; `overflowed` once a message did not fit, claimed once, from any thread, its rejection and claim one step under `deciding` (`admit`), which the end's choice takes too (`flooded`).
    struct Backlog {
        std::atomic<size_t> bytes{0};
        std::atomic<size_t> calls{0};
        std::atomic<bool> overflowed{false};
        std::mutex deciding;
        /// Runs in a rejection's step before its claim (`SandboxEngine::setRemoteRejectionStep`, tests), under `deciding`.
        std::function<void()> rejecting;
        /// Runs as the end's choice takes `deciding` (`SandboxEngine::setRemoteRejectionStep`, tests); set and read on the dispatch thread only.
        std::function<void()> choosing;

        /// The end's choice, on the dispatch thread: whether a rejection claimed the overflow before it, none of them in the middle of its step.
        bool flooded() {
            if (choosing) choosing();
            std::lock_guard<std::mutex> l(deciding);
            return overflowed;
        }
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

    /// A place for one message of up to `bytes` that its queued call keeps, taken before any of it is copied and never waited for: none once a message did not fit; the first that does not fit kills the widget, after what came before it, and tells the engine's side to stop (the end, should it run first, kills it the same way: `remoteDisconnected`). The bounds' check, the rejection and the overflow's claim are one step under `deciding`, the kill queued outside it.
    Admitted admit(size_t bytes) {
        {
            std::lock_guard<std::mutex> l(backlog_->deciding);
            if (backlog_->overflowed) return nullptr;
            const size_t calls = ++backlog_->calls;
            const size_t total = backlog_->bytes += bytes;
            if (calls <= kRemoteQueuedCalls && total <= kRemoteQueuedBytes) return std::make_shared<Reservation>(backlog_, bytes);
            backlog_->bytes -= bytes;
            backlog_->calls--;
            if (backlog_->rejecting) backlog_->rejecting();
            backlog_->overflowed = true;
        }
        submit(nullptr, flooded);
        return nullptr;
    }

    /// On the dispatch thread: the widget's death of its flood, its own (a hosted channel's `death()` stays `Ordinary`), once whichever of the overflow's kill and the end runs first.
    static void flooded(SandboxEngine& engine) {
        ownDeath(engine);
        engine.remoteKill(kEngineFlooded, true, true);
    }

    /// On the dispatch thread, before a death of the widget's own is reported: its hosted channel hears it is the first one (`SandboxHostedChannel::withdraw`).
    static void ownDeath(SandboxEngine& engine) {
        if (engine.hosted_ && !engine.dead_) detail::HostedDeaths::own(*engine.hosted_);
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
            if (live(engine) && engine.delegate_) call(engine, *engine.delegate_);
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
    /// What moves its platform requests' deadlines (SandboxHostedWait); none: each is due `kPlatformAnswerSeconds` after its arrival.
    const std::shared_ptr<SandboxHostedWait> wait_;
    const std::shared_ptr<Backlog> backlog_ = std::make_shared<Backlog>();
    /// The transport's end was queued (`remoteDisconnected`): a later one queues nothing.
    std::atomic<bool> disconnected_{false};
    /// Dispatch thread only: `yield`.
    bool yielded_ = false;
    /// The platform request waiting now, and whether requests are closed (`settleWaiting`).
    std::mutex platformMutex_;
    std::shared_ptr<detail::PlatformCall> waiting_;
    bool requestsClosed_ = false;
};

// MARK: construction

SandboxEngine::SandboxEngine(std::string html, SandboxConfig config, std::shared_ptr<SandboxRemoteChannel> remote, std::shared_ptr<IntlProvider> intl)
    : SandboxEngine("widget.client", std::move(html), std::move(config), std::shared_ptr<SandboxResources>(), std::move(intl)) {
    attachClient(std::move(remote), false);
}

SandboxEngine::SandboxEngine(HostedTag, std::string html, SandboxConfig config, std::shared_ptr<SandboxHostedChannel> hosted, std::shared_ptr<IntlProvider> intl,
                             std::shared_ptr<SandboxHostedWait> wait)
    : SandboxEngine("widget.client", std::move(html), std::move(config), std::shared_ptr<SandboxResources>(), std::move(intl)) {
    hosted_ = hosted;
    attachClient(std::move(hosted), true, std::move(wait));
}

void SandboxEngine::attachClient(std::shared_ptr<SandboxRemoteChannel> remote, bool hosted, std::shared_ptr<SandboxHostedWait> wait) {
    remote_ = std::move(remote);
    client_ = std::make_shared<RemoteClient>(liveness_, hosted, std::move(wait));
    // Before anything the channel delivers: its calls run on the dispatch thread after this.
    routeOpen_ = bool(remote_);
    // A host's factory without a channel: dead from the first task, never in process.
    if (remote_) remote_->attach(client_);
    else thread_.async([this] { remoteKill(kEngineGone, false, false); });
}

std::unique_ptr<SandboxEngine> SandboxEngine::hostedClient(std::string html, SandboxConfig config, std::shared_ptr<SandboxHostedChannel> hosted, std::shared_ptr<IntlProvider> intl,
                                                           std::shared_ptr<SandboxHostedWait> wait) {
    return std::unique_ptr<SandboxEngine>(new SandboxEngine(HostedTag{}, std::move(html), std::move(config), std::move(hosted), std::move(intl), std::move(wait)));
}

bool SandboxEngine::isHosted() const { return client_ && client_->hosted(); }

SandboxEngine::RemoteBacklog SandboxEngine::remoteBacklog() const { return client_ ? client_->backlog() : RemoteBacklog{}; }

void SandboxEngine::setRemoteRejectionStep(std::function<void()> step, std::function<void()> choosing) {
    if (client_) client_->rejectionStep(std::move(step), std::move(choosing));
}

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

void SandboxEngine::clientKill(const std::string& reason, bool counted) {
    auto kill = [this, reason, counted] {
        // A stop that yielded left the widget's death to the end's kill: a later stop or terminate takes nothing from it.
        if (RemoteClient::yielded(*this)) return;
        // An end that decided the live widget first kills it with that decision, its kill queued behind this one; until then nothing of the widget's reaches the delegate.
        if (counted && hosted_ && !dead_ && hosted_->localStop()) {
            client_->yield();
            return;
        }
        remoteKill(reason, true, false);
    };
    if (counted && thread_.isCurrent()) return kill();
    thread_.async(std::move(kill));
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

void SandboxEngine::hostedViewportHeight(double height) {
    if (!hosted_) return;
    thread_.async([this, height] {
        if (!dead_) hosted_->hostedViewportHeight(height);
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
        if (!started_ || !RemoteClient::carries(*this) || !hostedReady_ || kept_) return false;
        if (!dead_) {
            remoteDeath("terminated");
            remote_->stop("terminated");
        }
        grants_.clear();
        pressAdmitted_ = false;
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
        pressAdmitted_ = true;
        haptics_.press(at);
        if (visible_) grant(++hostedGesture_, at);
    });
}

// The press and its message in one job, stamped at the call: a grant never exists before the transport's writer took the message, nor for one a closed, ending or kept route or a refusing writer dropped.
void SandboxEngine::platformPress(std::string message) {
    if (!hosted_) return;
    const double at = nowMs();
    thread_.async([this, message = std::move(message), at] {
        if (!routeOpen_ || !hosted_->platform(message)) return;
        if (dead_ || kept_ || config_.context.preview) return;
        pressAdmitted_ = true;
        haptics_.press(at);
        if (visible_) grant(++hostedGesture_, at);
    });
}

// Only the end of a press that counted: a refused down leaves no touch window to its release.
void SandboxEngine::releaseDelivered() {
    if (!hosted_) return;
    const double at = nowMs();
    thread_.async([this, at] {
        if (std::exchange(pressAdmitted_, false) && !dead_ && !config_.context.preview) haptics_.release(at);
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
    if (!RemoteClient::live(*this)) return;
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
    // Background or not is this gate's verdict: the engine's side sends only the data and the wallet flag.
    if (request) delegate_->onBridgeRequest(*this, WidgetRequest{.data = request->data, .wallet = request->wallet, .background = verdict == sdk::RequestVerdict::Background}, std::move(respond));
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
    grants_.clear();
    pressAdmitted_ = false;
    clientSettleWaiting();
    const std::string bounded = reason.substr(0, kRemoteTextLimit);
    if (stop) remote_->stop(bounded);
    remote_->close();
    if (kept_ && ended && delegate_) delegate_->onHostedEnded(*this, bounded);
}

void SandboxEngine::platformKill(const std::string& reason) {
    // A stop that yielded left the widget's death to the end's kill: no claim, no diagnostic and no death of this request's.
    if (!RemoteClient::carries(*this)) return;
    // Before the delegate can stall: an end meanwhile finds the widget's own death claimed.
    if (hosted_ && !dead_) detail::HostedDeaths::own(*hosted_);
    if (delegate_) delegate_->onDiag(*this, kPlatformTimeout, reason);
    remoteKill(reason, true, true);
}

void SandboxEngine::clientSettleWaiting() { client_->settleWaiting(); }

void SandboxEngine::clientReport(int& counter, const char* kind, const std::string& detail) {
    if (!RemoteClient::live(*this) || !delegate_ || counter >= sdk::MAX_REJECTION_REPORTS) return;
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
