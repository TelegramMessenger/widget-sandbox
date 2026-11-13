// What test_remote.cpp (the remote engine's tests), test_remote_bridge.cpp (the bridge calls a WidgetHost records from it) and test_remote_stream.cpp (the framed transport's) share: the fake channel, a WidgetHost over it, the tests' own workers, the recorders and the helpers they use.
#pragma once

#include "test_support.h"

#include "twidget/intl.h"
#include "twidget/json.h"
#include "twidget/remote_engine.h"
#include "twidget/sandbox_engine.h"
#include "twidget/widget_host.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/// Swift's RemoteEngineTests.Channel: the test is the engine's side; each call with its arguments and its thread, under a lock.
class FakeRemoteChannel final : public SandboxRemoteChannel {
public:
    /// Called on the calling thread after a call is recorded, outside the lock; set it while no input is on its way.
    std::function<void(const std::string& call)> onCall;
    /// `close` reports a disconnection from inside, as a transport may.
    bool disconnectOnClose = false;
    /// `attach` reports a disconnection from inside, as a transport that cannot reach the engine's side may.
    bool disconnectOnAttach = false;
    std::string probeAnswer = "{\"ok\":true}";

    void attach(std::shared_ptr<SandboxRemoteSink> sink) override {
        {
            std::lock_guard<std::mutex> l(mutex_);
            sink_ = sink;
        }
        if (disconnectOnAttach) sink->remoteDisconnected();
    }
    void start(const std::string& html, const SandboxConfig& config) override { record("start:" + html + ":" + SandboxRemoteCoding::encodeConfig(config)); }
    void event(const std::string& json) override { record("event:" + json); }
    void activationEvent(const std::string& json, int gesture) override { record("activation:" + std::to_string(gesture) + ":" + json); }
    void frame(double time) override { record("frame:" + Json::number(time)); }
    void viewport(double w, double h, double dpr) override { record("viewport:" + Json::number(w) + "," + Json::number(h) + "," + Json::number(dpr)); }
    void resize(const std::string& entriesJson) override { record("resize:" + entriesJson); }
    void theme(const std::string& scheme, const std::map<std::string, std::string>& params) override {
        std::string call = "theme:" + scheme;
        for (const auto& [key, value] : params) call += ":" + key + "=" + value;
        record(call);
    }
    void contentSafeArea(const WidgetContentSafeArea& area) override { record("safeArea:" + SandboxRemoteCoding::encodeSafeArea(area)); }
    void setData(const WidgetData& data) override { record("data:" + SandboxRemoteCoding::encodeData(data)); }
    void setVisible(bool visible) override { record(visible ? "visible:true" : "visible:false"); }
    void pressBegan() override { record("press"); }
    void pressEnded() override { record("release"); }
    void chargeValidator(double ms) override { record("charge:" + Json::number(ms)); }
    void stop(const std::string& reason) override { record("stop:" + reason); }
    void answer(int64_t id, const SandboxRemoteOutcome& outcome) override {
        std::lock_guard<std::mutex> l(mutex_);
        answers_.emplace_back(id, outcome);
    }
    std::string probe(const std::string& code) override {
        record("probe:" + code);
        return probeAnswer;
    }
    void close() override {
        std::shared_ptr<SandboxRemoteSink> sink;
        {
            std::lock_guard<std::mutex> l(mutex_);
            closes_++;
            sink = sink_;
        }
        if (disconnectOnClose && sink) sink->remoteDisconnected();
    }

    std::shared_ptr<SandboxRemoteSink> sink() {
        std::lock_guard<std::mutex> l(mutex_);
        return sink_;
    }
    std::vector<std::string> calls() {
        std::lock_guard<std::mutex> l(mutex_);
        return calls_;
    }
    std::vector<std::thread::id> threads() {
        std::lock_guard<std::mutex> l(mutex_);
        return threads_;
    }
    std::vector<std::pair<int64_t, SandboxRemoteOutcome>> answers() {
        std::lock_guard<std::mutex> l(mutex_);
        return answers_;
    }
    int closes() {
        std::lock_guard<std::mutex> l(mutex_);
        return closes_;
    }

private:
    void record(const std::string& call) {
        {
            std::lock_guard<std::mutex> l(mutex_);
            calls_.push_back(call);
            threads_.push_back(std::this_thread::get_id());
        }
        if (onCall) onCall(call);
    }

    std::mutex mutex_;
    std::shared_ptr<SandboxRemoteSink> sink_;
    std::vector<std::string> calls_;
    std::vector<std::thread::id> threads_;
    std::vector<std::pair<int64_t, SandboxRemoteOutcome>> answers_;
    int closes_ = 0;
};

/// A wait a test opens from its own thread; `wait` gives up after 10 s, so a test that fails before opening it cannot strand the suite.
class Gate {
public:
    /// True when the test opened it in time.
    bool wait() {
        std::unique_lock<std::mutex> l(mutex_);
        waiters_++;
        cv_.notify_all();
        return cv_.wait_for(l, std::chrono::seconds(10), [&] { return open_; });
    }
    /// Until a thread waits on it, at most 5 s.
    bool waitedOn() {
        std::unique_lock<std::mutex> l(mutex_);
        return cv_.wait_for(l, std::chrono::seconds(5), [&] { return waiters_ > 0; });
    }
    void open() {
        {
            std::lock_guard<std::mutex> l(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int waiters_ = 0;
    bool open_ = false;
};

/// The exit status of a run a worker outlived its deadline in (`Background::finish`).
inline constexpr int kWorkerOverranExit = 3;

/// A worker still running past its deadline may be using the test's fixtures: the process ends here, unwinding nothing.
[[noreturn]] inline void stopTheRun(double seconds, const std::source_location& started) {
    fprintf(stderr, "  FAIL %s:%u: a worker still runs after %g s; the run stops here, with the fixtures it may use in place\n", started.file_name(), unsigned(started.line()), seconds);
    fflush(stdout);
    fflush(stderr);
    std::_Exit(kWorkerOverranExit);
}

/// `fn` on a thread the test owns, from now on: `within` looks for its value and gives up on nothing, `finish` waits for it to the end; the destructor finishes it too, so no fixture it uses goes first.
template <class R>
class Background {
public:
    /// The deadline of a worker the test did not finish: longer than a Gate's own 10 s.
    static constexpr double kDeadline = 15;

    explicit Background(std::function<R()> fn, std::source_location started = std::source_location::current())
        : started_(started), thread_([this, fn = std::move(fn)] { promise_.set_value(fn()); }) {}
    Background(const Background&) = delete;
    Background& operator=(const Background&) = delete;
    ~Background() {
        if (thread_.joinable()) finish(kDeadline);
    }

    /// Its value when it returns within `seconds`; nullopt while it still runs, which fails nothing: the worker stays the test's.
    std::optional<R> within(double seconds) {
        if (!value_ && future_.wait_for(std::chrono::duration<double>(seconds)) != std::future_status::ready) return std::nullopt;
        settle();
        return value_;
    }
    /// Its value, waited for up to `seconds`; past them the process ends at once (`stopTheRun`).
    R finish(double seconds) {
        if (!value_ && future_.wait_for(std::chrono::duration<double>(seconds)) != std::future_status::ready) stopTheRun(seconds, started_);
        settle();
        return *value_;
    }

private:
    void settle() {
        if (!value_) value_ = future_.get();
        if (thread_.joinable()) thread_.join();
    }

    std::source_location started_;
    std::promise<R> promise_;
    std::future<R> future_ = promise_.get_future();
    std::optional<R> value_;
    std::thread thread_;
};

inline std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) out += (out.empty() ? "" : " | ") + item.substr(0, 120);
    return out;
}

/// The run's baseline factory (none, or the stream switch's) back when a test that set one ends.
struct FactoryReset {
    ~FactoryReset() { restoreRemoteFactory(); }
};

/// `json` padded with spaces to `size` bytes: the same JSON.
inline std::string paddedTo(std::string json, size_t size) {
    json.resize(size, ' ');
    return json;
}

/// `holder` is a client-mode SandboxEngine or a WidgetHost over one.
template <class Holder>
bool backlogIs(Holder& holder, size_t bytes, size_t calls) {
    const SandboxEngine::RemoteBacklog b = holder.remoteBacklog();
    return b.bytes == bytes && b.calls == calls;
}

template <class Holder>
std::string backlogText(Holder& holder) {
    const SandboxEngine::RemoteBacklog b = holder.remoteBacklog();
    return std::to_string(b.bytes) + " bytes, " + std::to_string(b.calls) + " calls";
}

/// A link call's arguments as the shim sends them.
inline std::string linkArgs(const std::string& url) { return "[" + Json::quote(url) + "]"; }
/// The recorder's event for the link action of `method` (openLink or openTelegramLink) to `url`.
inline std::string linkAction(const std::string& method, const std::string& url) { return "action:{\"kind\":\"" + method + "\",\"url\":" + Json::quote(url) + "}"; }

/// Whether the client already sent the engine's side its `start`.
inline bool startedOver(FakeRemoteChannel& channel) {
    const std::vector<std::string> calls = channel.calls();
    return !calls.empty() && calls[0].rfind("start:", 0) == 0;
}

/// Every call a host's client-mode engine queued has reached the delegate, and the host thread has run what that sent it (a started, live widget's probe flushes the dispatch thread; a dead or unstarted one's too).
inline void settleHost(WidgetHost& host) {
    host.probe("0");
    host.onHostThread([] {});
}

/// A WidgetHost over a fake channel, started (its delegate is in once the channel heard `start`); without `ready` its renderer stays quiet until `announce`, and the delegate is in once the client took it, the engine not started.
struct RemoteHostRun {
    FactoryReset reset;
    std::shared_ptr<FakeRemoteChannel> channel = std::make_shared<FakeRemoteChannel>();
    ScriptedRenderer* renderer = nullptr;
    std::optional<WidgetHost> host;
    /// `observe` sets the host's observers before it starts.
    explicit RemoteHostRun(const std::function<void(WidgetHost&)>& observe = nullptr, bool ready = true, const WidgetHostOptions& options = WidgetHostOptions()) {
        SandboxEngine::setRemoteChannelFactory([channel = channel](std::shared_ptr<SandboxResources>) -> std::shared_ptr<SandboxRemoteChannel> { return channel; });
        host.emplace("<p>x</p>", WidgetTheme{}, 300, options, RES, scripted(renderer));
        renderer->readyOnLoad = ready;
        if (observe) observe(*host);
        host->start();
        if (ready) {
            CHECK(eventually([&] { return startedOver(*channel); }));
            return;
        }
        // start()'s closure handed the delegate to the client; an unstarted client's probe answers after its dispatch thread took it, asking the channel nothing.
        host->onHostThread([] {});
        CHECK(host->probe("0") == "ERROR no runtime");
    }
    SandboxRemoteSink& sink() { return *channel->sink(); }
    /// The quiet renderer's `ready`, then the host settled: what it kept sent, the engine started unless it was cut.
    void announce() {
        renderer->onMessage("{\"type\":\"ready\"}");
        host->onHostThread([] {});
        settleHost(*host);
    }
    /// The types of what the renderer got, in order.
    std::vector<std::string> sentTypes() {
        std::lock_guard<std::mutex> l(renderer->mutex);
        std::vector<std::string> out;
        for (const std::string& m : renderer->messages) out.push_back(Json::parse(m).value_or(Json())["type"].asString());
        return out;
    }
};

/// `host`'s thread held on a gate from a worker of the test's until `release`, as a slow or stuck host: what the host queues meanwhile waits, while the client's dispatch thread runs on. Declared after the host, so it lets go first.
struct HeldHost {
    Gate gate;
    Background<bool> worker;
    explicit HeldHost(WidgetHost& host) : worker([this, &host] {
        host.onHostThread([this] { gate.wait(); });
        return true;
    }) {}
    bool held() { return gate.waitedOn(); }
    bool release() {
        gate.open();
        return worker.finish(5);
    }
};

/// The calls of `channel` that begin with `prefix`, in order.
inline std::vector<std::string> callsWith(FakeRemoteChannel& channel, const std::string& prefix) {
    std::vector<std::string> out;
    for (const std::string& c : channel.calls())
        if (c.rfind(prefix, 0) == 0) out.push_back(c);
    return out;
}

/// A trusted tap through `r`'s renderer, a grant once the client's dispatch thread had it.
inline void tapThrough(RemoteHostRun& r) {
    r.renderer->onMessage("{\"type\":\"event\",\"ev\":" + kTapOnButton + ",\"trusted\":true}");
    r.host->onHostThread([] {});
    r.host->probe("0");
}

/// A primary press through `r`'s renderer (its `gesture`), down on the client's haptic gate once its dispatch thread had it.
inline void pressThrough(RemoteHostRun& r) {
    r.renderer->onMessage("{\"type\":\"gesture\"}");
    r.host->onHostThread([] {});
    r.host->probe("0");
}

/// The sizes of `got` and `expected` and their first difference, for a failure.
inline std::string firstDifference(const std::vector<std::string>& got, const std::vector<std::string>& expected) {
    size_t i = 0;
    while (i < got.size() && i < expected.size() && got[i] == expected[i]) i++;
    return std::to_string(got.size()) + " entries for " + std::to_string(expected.size()) + ", first difference at " + std::to_string(i) + ": " +
           (i < got.size() ? got[i].substr(0, 200) : "(none)") + " for " + (i < expected.size() ? expected[i].substr(0, 200) : "(none)");
}

/// Swift's RemoteEngineTests.Delegate: what a client-mode engine hands its delegate, in order, under a lock.
class RemoteRecorder final : public SandboxEngineDelegate {
public:
    /// Runs inside `onQuery`, between its "query:…" and "query-done" events; set it while no query is on its way.
    std::function<void(SandboxEngine&)> duringQuery;
    /// Runs inside `onOps` after its event; set it while no ops are on their way.
    std::function<void(SandboxEngine&)> duringOps;
    /// Runs inside `onTaskStarted` after its event; set it on the engine's thread or while no task is on its way.
    std::function<void(SandboxEngine&)> atBegin;
    /// What `onQuery` answers; set it while no query is on its way.
    std::string queryAnswer = "{\"answer\":1}";

    void onOps(SandboxEngine& engine, const std::string&) override {
        record("ops");
        if (duringOps) duringOps(engine);
    }
    QueryAnswer onQuery(SandboxEngine& engine, const std::string& json) override {
        record("query:" + json);
        if (duringQuery) duringQuery(engine);
        record("query-done");
        return {queryAnswer, std::nullopt};
    }
    void onListen(SandboxEngine&, const std::string&) override { record("listen"); }
    void onPreventDefault(SandboxEngine&, const std::string&) override { record("preventDefault"); }
    void onAnim(SandboxEngine&, const std::string&) override { record("anim"); }
    void onRequestFrame(SandboxEngine&) override { record("frame"); }
    void onLog(SandboxEngine&, const std::string& level, const std::string& text) override { record("log:" + level + ":" + std::to_string(text.size())); }
    void onDiag(SandboxEngine&, const std::string& kind, const std::string& detail) override {
        record("diag:" + kind);
        std::lock_guard<std::mutex> l(mutex_);
        diags_.emplace_back(kind, detail);
    }
    void onBridgeAction(SandboxEngine&, const WidgetAction& action) override { record("action:" + action.toJson().dump()); }
    void onBridge(SandboxEngine&, const std::string& method, const std::string& argsJson) override { record("bridge:" + method + ":" + argsJson); }
    void onBridgeCopyText(SandboxEngine&, const std::string& text) override { record("copy:" + text); }
    void onStorage(SandboxEngine&, const std::string& area, const StorageArea& values) override {
        record("storage:" + area);
        std::lock_guard<std::mutex> l(mutex_);
        commits_.emplace_back(area, values);
    }
    void onBridgeRequest(SandboxEngine&, const WidgetRequest& request, WidgetRequestResponder respond) override {
        record("request:" + request.data + (request.wallet ? ":wallet" : ""));
        std::lock_guard<std::mutex> l(mutex_);
        responders_.push_back(std::move(respond));
    }
    void onBridgeWallet(SandboxEngine&, const WalletCall& call, WidgetRequestResponder respond) override {
        record("wallet:" + std::string(call.methodName()) + ":" + call.data);
        std::lock_guard<std::mutex> l(mutex_);
        walletCalls_.push_back(call);
        responders_.push_back(std::move(respond));
    }
    void onResizeAnchor(SandboxEngine&, const std::string& edge) override { record("anchor:" + edge); }
    void onPublicDataWatch(SandboxEngine&, bool on) override { record(on ? "watch:true" : "watch:false"); }
    void onLoaded(SandboxEngine&) override { record("loaded"); }
    void onStats(SandboxEngine&, const SandboxStats& stats) override {
        record("stats");
        std::lock_guard<std::mutex> l(mutex_);
        stats_.push_back(stats);
    }
    void onDied(SandboxEngine&, const std::string& reason) override { record("died:" + reason); }
    void onTaskStarted(SandboxEngine& engine) override {
        record("begin");
        if (atBegin) atBegin(engine);
    }
    void onTaskCompleted(SandboxEngine&) override { record("end"); }

    /// An event of the test's own (a provider's call) among the delegate's, with its thread.
    void record(const std::string& event) {
        std::lock_guard<std::mutex> l(mutex_);
        events_.push_back(event);
        threads_.push_back(std::this_thread::get_id());
    }

    std::vector<std::string> events() {
        std::lock_guard<std::mutex> l(mutex_);
        return events_;
    }
    std::vector<std::thread::id> threads() {
        std::lock_guard<std::mutex> l(mutex_);
        return threads_;
    }
    std::vector<std::pair<std::string, std::string>> diags() {
        std::lock_guard<std::mutex> l(mutex_);
        return diags_;
    }
    std::vector<SandboxStats> stats() {
        std::lock_guard<std::mutex> l(mutex_);
        return stats_;
    }
    std::vector<std::string> deaths() {
        std::vector<std::string> out;
        for (const std::string& e : events())
            if (e.rfind("died:", 0) == 0) out.push_back(e.substr(5));
        return out;
    }
    /// The events that start with `prefix`, in order.
    std::vector<std::string> eventsWith(const std::string& prefix) {
        std::vector<std::string> out;
        for (const std::string& e : events())
            if (e.rfind(prefix, 0) == 0) out.push_back(e);
        return out;
    }
    std::vector<std::pair<std::string, StorageArea>> commits() {
        std::lock_guard<std::mutex> l(mutex_);
        return commits_;
    }
    std::vector<WalletCall> walletCalls() {
        std::lock_guard<std::mutex> l(mutex_);
        return walletCalls_;
    }
    /// The responder of the `i`th request or wallet call the delegate got (one sequence for both), none before it came.
    std::optional<WidgetRequestResponder> responder(size_t i) {
        std::lock_guard<std::mutex> l(mutex_);
        if (i >= responders_.size()) return std::nullopt;
        return responders_[i];
    }
    /// Completes that call from the calling thread (nullopt: `unsupported()`); false when there is none.
    bool respond(size_t i, const std::optional<WidgetRequestOutcome>& outcome) {
        std::optional<WidgetRequestResponder> r = responder(i);
        if (!r) return false;
        if (outcome) (*r)(*outcome);
        else r->unsupported();
        return true;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> events_;
    std::vector<std::thread::id> threads_;
    std::vector<std::pair<std::string, std::string>> diags_;
    std::vector<SandboxStats> stats_;
    std::vector<std::pair<std::string, StorageArea>> commits_;
    std::vector<WalletCall> walletCalls_;
    std::vector<WidgetRequestResponder> responders_;
};

/// What RecordingIntl answers "alive" at once, unrecorded: on the dispatch thread, a probe of whether the client still has its engine ("" once its destructor let it go).
inline const std::string kLivenessProbe = "{\"kind\":\"probe\"}";

/// A host's IntlProvider for a client: each request goes into `into` (set before the first) with its thread, then `during` runs (set it while no request is on its way) and `answer` comes back; `kLivenessProbe` is the exception.
class RecordingIntl final : public IntlProvider {
public:
    RemoteRecorder* into = nullptr;
    std::function<void()> during;
    std::string answer = "INTL";

    std::string format(const Json& request) override {
        if (request["kind"].asString() == "probe") return "alive";
        if (into) into->record("intl:" + request.dump());
        if (during) during();
        return answer;
    }
};

inline const std::string kEngineGone = "the widget engine process ended";

inline bool has(const std::vector<std::string>& events, const std::string& event) { return std::find(events.begin(), events.end(), event) != events.end(); }

inline const std::string kRect77 = "{\"x\":0,\"y\":0,\"width\":77,\"height\":20,\"top\":0,\"left\":0,\"right\":77,\"bottom\":20}";

inline const char* kMeasure = "<p id='p'>x</p><script>console.log('w=' + document.getElementById('p').getBoundingClientRect().width);</script>";

inline SandboxConfig walletConfig() {
    SandboxConfig c = testConfig();
    c.context = messageContext(false, true, false, "private");
    c.context.wallet = true;
    return c;
}

/// The widget the loopback and the stream run behind a client: what it saw at boot (hidden, the safe area, the chat state, its button's width from a layout query, Intl), each chat state change, a storage commit, a link on the first tap and on the next a request for the wallet, then a signature on its grant, every outcome logged.
inline const char* kRealEngineWidget =
    "<button id='b'>go</button><script>var log = [document.visibilityState, TelegramWidget.contentSafeAreaInset.bottom, TelegramWidget.chatData,"
    "  document.getElementById('b').getBoundingClientRect().width, (1234.5).toLocaleString()], taps = 0;"
    "TelegramWidget.onEvent('chatDataChanged', function () { log.push('chat:' + TelegramWidget.chatData); });"
    "localStorage.setItem('k', 'v');"
    "document.getElementById('b').onclick = function () { if (++taps === 1) return TelegramWidget.openLink('https://example.com/');"
    "  TelegramWidget.request('ask', { wallet: true }).then(function (a) { log.push('ok:' + a.result);"
    "    return TelegramWidget.wallet.signData({ type: 'text', text: 'sign' }, 'w'); })"
    "  .then(function (b) { log.push('signed:' + b.result); }, function (e) { log.push('err:' + e.code); }); };</script>";

/// The diagnostics a WidgetHost's observer saw, in order, under a lock (`onChange` runs on the host thread).
struct ObservedChanges {
    std::mutex mutex;
    std::vector<WidgetDiagnostics> seen;
    std::function<void(const WidgetDiagnostics&)> observer() {
        return [this](const WidgetDiagnostics& d) {
            std::lock_guard<std::mutex> l(mutex);
            seen.push_back(d);
        };
    }
    std::vector<WidgetDiagnostics> all() {
        std::lock_guard<std::mutex> l(mutex);
        return seen;
    }
};
