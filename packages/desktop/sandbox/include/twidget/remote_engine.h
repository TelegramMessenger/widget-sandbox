// The engine in another process, the C++ twin of RemoteEngine.swift: the channel and the sink between a client-mode engine and the engine's side, the server on that side, the in-process loopback and their wire coding.
#pragma once

#include "twidget/sandbox_engine.h"
#include "twidget/sdk_policy.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace twidget {

/// How the host settled a request or a wallet call the engine's side forwarded: the bot's answer, a failure (never `None`), or `Unsupported` (the host handles none).
struct SandboxRemoteOutcome {
    enum class Kind { Answer, Failure, Unsupported };
    Kind kind = Kind::Unsupported;
    /// `Answer` only: the bot's result, none for none.
    std::optional<std::string> result;
    /// `Failure` only.
    WidgetRequestFailure failure = WidgetRequestFailure::None;

    static SandboxRemoteOutcome answer(std::optional<std::string> result) { return {Kind::Answer, std::move(result), WidgetRequestFailure::None}; }
    static SandboxRemoteOutcome failed(WidgetRequestFailure failure) { return {Kind::Failure, std::nullopt, failure}; }
    static SandboxRemoteOutcome unsupported() { return {}; }
    /// A responder's outcome (`WidgetRequestResponder`): nullopt is `Unsupported`, a failure other than `None` is `Failure`, else the answer's result.
    static SandboxRemoteOutcome from(const std::optional<WidgetRequestOutcome>& outcome) {
        if (!outcome) return unsupported();
        if (outcome->failure != WidgetRequestFailure::None) return failed(outcome->failure);
        return answer(outcome->answer.result);
    }
    bool operator==(const SandboxRemoteOutcome& o) const {
        if (kind != o.kind) return false;
        if (kind == Kind::Answer) return result == o.result;
        return kind != Kind::Failure || failure == o.failure;
    }
};

class SandboxRemoteSink;

/// Host → engine for a client-mode engine: calls come from its dispatch thread in the host's order, a transport keeps it, and each but `probe` returns without waiting for the engine's side.
class SandboxRemoteChannel {
public:
    /// The owner destroys the channel on its own thread, never while another call on the same channel runs.
    virtual ~SandboxRemoteChannel() = default;
    /// Once, from the client's constructor, before any other call; the transport keeps the sink and may call it after the client is gone.
    virtual void attach(std::shared_ptr<SandboxRemoteSink> sink) = 0;
    /// The widget and its configuration, once (the client's `start()`).
    virtual void start(const std::string& html, const SandboxConfig& config) = 0;
    virtual void event(const std::string& json) = 0;
    virtual void activationEvent(const std::string& json, int gesture) = 0;
    virtual void frame(double time) = 0;
    virtual void viewport(double innerWidth, double innerHeight, double dpr) = 0;
    virtual void resize(const std::string& entriesJson) = 0;
    virtual void theme(const std::string& colorScheme, const std::map<std::string, std::string>& themeParams) = 0;
    virtual void contentSafeArea(const WidgetContentSafeArea& area) = 0;
    virtual void setData(const WidgetData& data) = 0;
    virtual void setVisible(bool visible) = 0;
    virtual void pressBegan() = 0;
    virtual void pressEnded() = 0;
    /// The host validator's time on this widget's messages (C++ only): the JS quota it counts against lives with the engine.
    virtual void chargeValidator(double ms) = 0;
    /// The host stops the widget (`SandboxEngine::stop`, `terminate`): the engine dies with `reason`.
    virtual void stop(const std::string& reason) = 0;
    /// The outcome of the request or wallet call `id` (`SandboxRemoteSink::remoteRequest`, `remoteWallet`).
    virtual void answer(int64_t id, const SandboxRemoteOutcome& outcome) = 0;
    /// The one synchronous call (`SandboxEngine::probe`): made from the owner's thread once earlier inputs are flushed, never from the dispatch thread, which a probe may need.
    virtual std::string probe(const std::string& code) = 0;
    /// Idempotent and may report `remoteDisconnected()` from inside; a loopback closes its server here but destroys it only in its own destructor, never on the dispatch thread.
    virtual void close() = 0;
};

/// Engine → host, untrusted: the client checks every call again (its size before it is queued) and hands it to its delegate on its dispatch thread, in transport order.
class SandboxRemoteSink {
public:
    /// A transport may call the sink after the client is gone or closed the channel: such calls return at once and change nothing.
    virtual ~SandboxRemoteSink() = default;
    /// A task of the engine's side began (an event, a timer, a frame); `remoteTaskCompleted` ends it, also after a death inside it.
    virtual void remoteTaskStarted() = 0;
    virtual void remoteTaskCompleted() = 0;
    virtual void remoteOps(const std::string& json) = 0;
    /// Synchronous: the delegate's answer on the dispatch thread after every earlier call reached it, never on the renderer's or host's thread; never made from inside a channel call.
    virtual QueryAnswer remoteQuery(const std::string& json) = 0;
    /// Synchronous like `remoteQuery` (C++ only): the host's `IntlProvider` formats the shim's request object, "" for a malformed one.
    virtual std::string remoteIntl(const std::string& requestJson) = 0;
    virtual void remoteListen(const std::string& json) = 0;
    virtual void remotePreventDefault(const std::string& json) = 0;
    virtual void remoteAnim(const std::string& json) = 0;
    virtual void remoteRequestsFrame() = 0;
    virtual void remoteLog(const std::string& level, const std::string& text) = 0;
    virtual void remoteDiag(const std::string& kind, const std::string& detail) = 0;
    /// A bridge action's method and raw arguments (`SandboxEngineDelegate::onBridge`).
    virtual void remoteBridge(const std::string& method, const std::string& argsJson) = 0;
    virtual void remoteCopyText(const std::string& text) = 0;
    /// A request numbered by the engine's side, one counter with `remoteWallet`; the host settles it with `SandboxRemoteChannel::answer(id, …)`.
    virtual void remoteRequest(int64_t id, const std::string& data, bool wallet) = 0;
    /// A wallet call in `SandboxRemoteCoding::encodeWallet`'s form.
    virtual void remoteWallet(int64_t id, const std::string& payload) = 0;
    /// One area of a storage commit, `{"local":{…}}` or `{"session":{…}}` (`sdk::StorageCommit::toJson`).
    virtual void remoteStorage(const std::string& commitJson) = 0;
    virtual void remoteResizeAnchor(const std::string& edge) = 0;
    virtual void remoteWatchesPublicData(bool on) = 0;
    virtual void remoteLoaded() = 0;
    /// The engine's counters as it reports them; the client bounds them as `SandboxRemoteCoding::decodeStats` does.
    virtual void remoteStats(const SandboxStats& stats) = 0;
    virtual void remoteDied(const std::string& reason) = 0;
    /// The transport lost the engine's side or never reached it: the widget dies.
    virtual void remoteDisconnected() = 0;
};

/// The engine's side of one client-mode engine: a real `SandboxEngine` built at `start` with this as its delegate, each delegate call sent on to `sink` from the engine's thread in order (an action only as its raw `remoteBridge`), requests and wallet calls numbered from one counter for `answer`. `start`, the inputs, `answer` and `close` come one at a time in the host's order (a channel's thread) and never wait for the engine, which may be waiting for the host's answer to a query or an Intl request; `probe`, the one synchronous call, may come from another thread but never from the one answering those. The owner destroys the server on its own thread (never the engine's) after its last call.
class SandboxRemoteServer final : public SandboxEngineDelegate {
public:
    SandboxRemoteServer(std::shared_ptr<SandboxRemoteSink> sink, std::shared_ptr<SandboxResources> resources);
    /// Closes, then joins the engine while the sink and this delegate still exist.
    ~SandboxRemoteServer() override;
    SandboxRemoteServer(const SandboxRemoteServer&) = delete;
    SandboxRemoteServer& operator=(const SandboxRemoteServer&) = delete;

    /// Builds the engine (directly, never through `SandboxEngine::make`), gives it the inputs that came before in their order for its own pre-boot rules, then starts it; once, never after `close`.
    void start(const std::string& html, const SandboxConfig& config);
    // The engine's inputs: kept until `start`, dropped after `close`.
    void event(const std::string& json);
    void activationEvent(const std::string& json, int gesture);
    void frame(double time);
    void viewport(double innerWidth, double innerHeight, double dpr);
    void resize(const std::string& entriesJson);
    void theme(const std::string& colorScheme, const std::map<std::string, std::string>& themeParams);
    void contentSafeArea(const WidgetContentSafeArea& area);
    void setData(const WidgetData& data);
    void setVisible(bool visible);
    void pressBegan();
    void pressEnded();
    void chargeValidator(double ms);
    void stop(const std::string& reason);
    /// Settles the outstanding request or wallet call `id`, once; an id that is not outstanding (unknown, settled, refused by `close`) changes nothing.
    void answer(int64_t id, const SandboxRemoteOutcome& outcome);
    /// The engine's `probe`, waiting for its thread; "ERROR no runtime" before `start` and after `close`.
    std::string probe(const std::string& code);
    /// The client is gone: the waiting inputs are dropped, the outstanding calls refused (`unsupported`) and the engine terminated without waiting for it (it may be waiting for the sink); it lives until the destructor. Idempotent.
    void close();

    // SandboxEngineDelegate (the engine's thread)
    void onTaskStarted(SandboxEngine&) override;
    void onTaskCompleted(SandboxEngine&) override;
    void onOps(SandboxEngine&, const std::string& json) override;
    QueryAnswer onQuery(SandboxEngine&, const std::string& json) override;
    void onListen(SandboxEngine&, const std::string& json) override;
    void onPreventDefault(SandboxEngine&, const std::string& json) override;
    void onAnim(SandboxEngine&, const std::string& json) override;
    void onRequestFrame(SandboxEngine&) override;
    void onLog(SandboxEngine&, const std::string& level, const std::string& text) override;
    void onDiag(SandboxEngine&, const std::string& kind, const std::string& detail) override;
    void onBridgeAction(SandboxEngine&, const WidgetAction& action) override;
    void onBridge(SandboxEngine&, const std::string& method, const std::string& argsJson) override;
    void onBridgeCopyText(SandboxEngine&, const std::string& text) override;
    void onStorage(SandboxEngine&, const std::string& area, const StorageArea& values) override;
    void onResizeAnchor(SandboxEngine&, const std::string& edge) override;
    void onPublicDataWatch(SandboxEngine&, bool on) override;
    void onBridgeRequest(SandboxEngine&, const WidgetRequest& request, WidgetRequestResponder respond) override;
    void onBridgeWallet(SandboxEngine&, const WalletCall& call, WidgetRequestResponder respond) override;
    void onLoaded(SandboxEngine&) override;
    void onStats(SandboxEngine&, const SandboxStats& stats) override;
    void onDied(SandboxEngine&, const std::string& reason) override;

private:
    /// `call` with the engine, kept until `start` and dropped after `close`; the lock only decides, the engine is called after it.
    void input(std::function<void(SandboxEngine&)> call);
    /// Numbers `respond` and keeps it for `answer`, then `send`s the number; refused (`unsupported`) after `close`.
    void forward(WidgetRequestResponder respond, const std::function<void(int64_t id)>& send);

    const std::shared_ptr<SandboxRemoteSink> sink_;
    const std::shared_ptr<SandboxResources> resources_;
    std::mutex mutex_;
    bool started_ = false;
    bool closed_ = false;
    std::vector<std::function<void(SandboxEngine&)>> waiting_;
    std::unique_ptr<SandboxEngine> engine_;
    int64_t lastRequest_ = 0;
    std::map<int64_t, WidgetRequestResponder> pending_;
};

/// Both halves in one process (tests, the harness, parity runs): each call goes straight to a `SandboxRemoteServer` built at `attach`, whose sink reaches the client's; config, data, safe areas and outcomes cross through `SandboxRemoteCoding`, wallet calls and storage in their sink forms and stats through `encodeStats`/`decodeStats` as a transport's bytes would, and a form that does not decode is a lost connection (`remoteDisconnected`). `close` closes the server and keeps it, as the client may close from its dispatch thread while the server's engine waits for that thread: only this channel's destructor, on the owner's thread, destroys it.
class SandboxLoopbackChannel final : public SandboxRemoteChannel {
public:
    explicit SandboxLoopbackChannel(std::shared_ptr<SandboxResources> resources);

    void attach(std::shared_ptr<SandboxRemoteSink> sink) override;
    void start(const std::string& html, const SandboxConfig& config) override;
    void event(const std::string& json) override;
    void activationEvent(const std::string& json, int gesture) override;
    void frame(double time) override;
    void viewport(double innerWidth, double innerHeight, double dpr) override;
    void resize(const std::string& entriesJson) override;
    void theme(const std::string& colorScheme, const std::map<std::string, std::string>& themeParams) override;
    void contentSafeArea(const WidgetContentSafeArea& area) override;
    void setData(const WidgetData& data) override;
    void setVisible(bool visible) override;
    void pressBegan() override;
    void pressEnded() override;
    void chargeValidator(double ms) override;
    void stop(const std::string& reason) override;
    void answer(int64_t id, const SandboxRemoteOutcome& outcome) override;
    std::string probe(const std::string& code) override;
    void close() override;

private:
    std::shared_ptr<SandboxResources> resources_;
    std::shared_ptr<SandboxRemoteSink> client_;
    std::unique_ptr<SandboxRemoteServer> server_;
};

/// Wire forms between the processes, Swift's SandboxRemoteCoding without its canvas and binary-op coders; JSON is written by `Json::dump` (bytes from 0x20 up raw).
struct SandboxRemoteCoding {
    /// Nesting a decoder accepts (the deepest form, a wallet call's message, has 4); every decoder also refuses text over `SandboxEngine::kRemoteMessageLimit` bytes unparsed.
    static constexpr int kMaxDepth = 8;
    static constexpr size_t kStatsFields = 11;

    /// Every `SandboxConfig` field as one JSON object (a non-finite number is written as null).
    static std::string encodeConfig(const SandboxConfig& config);
    /// Nullopt without finite numbers `innerWidth`, `innerHeight`, `devicePixelRatio` and a string `theme`; any other member absent or of another type keeps its default.
    static std::optional<SandboxConfig> decodeConfig(std::string_view text);
    static std::string encodeSafeArea(const WidgetContentSafeArea& area);
    /// An object, or nullopt; each side a finite number, else 0.
    static std::optional<WidgetContentSafeArea> decodeSafeArea(std::string_view text);
    /// `{"chatData": text | null, "publicData": text | null}`.
    static std::string encodeData(const WidgetData& data);
    /// An object, or nullopt; a member that is not a string is none.
    static std::optional<WidgetData> decodeData(std::string_view text);
    /// `{"answer": text | null}`, `{"failure": code}` (`sdk::walletFailureCode`) or `{"unsupported": true}`.
    static std::string encodeOutcome(const SandboxRemoteOutcome& outcome);
    /// An object, or nullopt: any `unsupported` member first, then a string `failure` (an unknown code is `NotAllowed`), then any `answer` (a result only when a string).
    static std::optional<SandboxRemoteOutcome> decodeOutcome(std::string_view text);
    /// `[1, method, transaction | payload, data]`: the arguments of a `wallet` bridge call; the real id travels apart.
    static std::string encodeWallet(const WalletCall& call);
    /// The call as `sdk::checkBridgeCall("wallet", …)` checks it, or nullopt.
    static std::optional<WalletCall> decodeWallet(std::string_view text);
    /// `[jsMs, tasks, memoryBytes, ops, queries, queryMs, queryCacheHits, missedMutations, throttledTasks, cpuWindowMs, validatorMs]`; Swift sends the first 10.
    static std::vector<double> encodeStats(const SandboxStats& stats);
    /// Exactly 11 numbers, or nullopt; a non-finite one is 0, a finite one is kept within [0, 1e15] and then within its field's type.
    static std::optional<SandboxStats> decodeStats(const std::vector<double>& values);
};

} // namespace twidget
