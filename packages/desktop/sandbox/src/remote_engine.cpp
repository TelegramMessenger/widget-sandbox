// The engine's side of a remote engine (`SandboxRemoteServer`, which runs an in-process SandboxEngine for its channel) and the in-process loopback (`SandboxLoopbackChannel`); the wire forms they code with are remote_coding.cpp's.
#include "twidget/remote_engine.h"

#include "remote_internal.h"
#include "twidget/intl.h"
#include "twidget/json.h"

#include <exception>
#include <limits>
#include <utility>

namespace twidget {

namespace {

/// The death of a runtime whose engine cannot be built (its `widget.sandbox` cannot start), before the refusal itself (which names the thread and the OS error).
constexpr const char* kEngineUnstarted = "the widget engine could not start";

} // namespace

// MARK: the engine's side

SandboxRemoteServer::SandboxRemoteServer(std::shared_ptr<SandboxRemoteSink> sink, std::shared_ptr<SandboxResources> resources)
    : sink_(std::move(sink)), resources_(std::move(resources)) {}

SandboxRemoteServer::~SandboxRemoteServer() {
    close();
    std::unique_ptr<SandboxEngine> engine;
    {
        std::lock_guard<std::mutex> l(mutex_);
        engine = std::move(engine_);
    }
    // Joins the engine's thread, whose last tasks may still call this delegate and the sink.
    engine.reset();
}

void SandboxRemoteServer::start(const std::string& html, const SandboxConfig& config) {
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (closed_ || started_) return;
        started_ = true;
    }
    std::unique_ptr<SandboxEngine> engine;
    try {
        engine = std::make_unique<SandboxEngine>(html, config, resources_, std::make_shared<detail::RemoteIntl>(sink_));
    } catch (const std::exception& e) {
        // Closed as `close` closes it (nothing is outstanding before an engine), then the death outside the lock.
        {
            std::lock_guard<std::mutex> l(mutex_);
            closed_ = true;
            waiting_.clear();
        }
        sink_->remoteDied(std::string(kEngineUnstarted) + ": " + e.what());
        return;
    }
    engine->setDelegate(this);
    SandboxEngine& built = *engine;
    std::vector<std::function<void(SandboxEngine&)>> waiting;
    {
        std::lock_guard<std::mutex> l(mutex_);
        engine_ = std::move(engine);
        waiting.swap(waiting_);
    }
    for (auto& call : waiting) call(built);
    built.start();
}

void SandboxRemoteServer::input(std::function<void(SandboxEngine&)> call) {
    SandboxEngine* engine = nullptr;
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (closed_) return;
        if (!engine_) return waiting_.push_back(std::move(call));
        engine = engine_.get();
    }
    call(*engine);
}

void SandboxRemoteServer::event(const std::string& json) {
    input([json](SandboxEngine& engine) { engine.event(json); });
}
void SandboxRemoteServer::activationEvent(const std::string& json, int gesture) {
    input([json, gesture](SandboxEngine& engine) { engine.activationEvent(json, gesture); });
}
void SandboxRemoteServer::frame(double time) {
    input([time](SandboxEngine& engine) { engine.frame(time); });
}
void SandboxRemoteServer::viewport(double innerWidth, double innerHeight, double dpr) {
    input([innerWidth, innerHeight, dpr](SandboxEngine& engine) { engine.viewport(innerWidth, innerHeight, dpr); });
}
void SandboxRemoteServer::resize(const std::string& entriesJson) {
    input([entriesJson](SandboxEngine& engine) { engine.resize(entriesJson); });
}
void SandboxRemoteServer::theme(const std::string& colorScheme, const std::map<std::string, std::string>& themeParams) {
    input([colorScheme, themeParams](SandboxEngine& engine) { engine.theme(colorScheme, themeParams); });
}
void SandboxRemoteServer::contentSafeArea(const WidgetContentSafeArea& area) {
    input([area](SandboxEngine& engine) { engine.contentSafeArea(area); });
}
void SandboxRemoteServer::safeArea(const WidgetSafeArea& area) {
    input([area](SandboxEngine& engine) { engine.safeArea(area); });
}
void SandboxRemoteServer::setData(const WidgetData& data) {
    input([data](SandboxEngine& engine) { engine.setData(data); });
}
void SandboxRemoteServer::setVisible(bool visible) {
    input([visible](SandboxEngine& engine) { engine.setVisible(visible); });
}
void SandboxRemoteServer::pressBegan() {
    input([](SandboxEngine& engine) { engine.pressBegan(); });
}
void SandboxRemoteServer::pressEnded() {
    input([](SandboxEngine& engine) { engine.pressEnded(); });
}
void SandboxRemoteServer::chargeValidator(double ms) {
    input([ms](SandboxEngine& engine) { engine.chargeValidator(ms); });
}
void SandboxRemoteServer::stop(const std::string& reason) {
    input([reason](SandboxEngine& engine) { engine.stop(reason); });
}

void SandboxRemoteServer::answer(int64_t id, const SandboxRemoteOutcome& outcome) {
    std::map<int64_t, WidgetRequestResponder>::node_type taken;
    {
        std::lock_guard<std::mutex> l(mutex_);
        taken = pending_.extract(id);
    }
    if (taken.empty()) return;
    const WidgetRequestResponder& respond = taken.mapped();
    switch (outcome.kind) {
    case SandboxRemoteOutcome::Kind::Answer: return respond(WidgetRequestOutcome{WidgetRequestFailure::None, WidgetRequestAnswer{outcome.result}});
    // A failure stays one: `None` here would read as an answer.
    case SandboxRemoteOutcome::Kind::Failure:
        return respond(WidgetRequestOutcome{outcome.failure == WidgetRequestFailure::None ? WidgetRequestFailure::Failed : outcome.failure, {}});
    case SandboxRemoteOutcome::Kind::Unsupported: return respond.unsupported();
    }
}

std::string SandboxRemoteServer::probe(const std::string& code) {
    SandboxEngine* engine = nullptr;
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (!closed_) engine = engine_.get();
    }
    return engine ? engine->probe(code) : "ERROR no runtime";
}

void SandboxRemoteServer::close() {
    std::map<int64_t, WidgetRequestResponder> open;
    SandboxEngine* engine = nullptr;
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (std::exchange(closed_, true)) return;
        waiting_.clear();
        open.swap(pending_);
        engine = engine_.get();
    }
    for (const auto& entry : open) entry.second.unsupported();
    if (engine) engine->terminate();
}

void SandboxRemoteServer::forward(WidgetRequestResponder respond, const std::function<void(int64_t id)>& send) {
    int64_t id = 0;
    {
        std::lock_guard<std::mutex> l(mutex_);
        // Every id is new: past the last one the calls are refused as after `close`.
        if (!closed_ && lastRequest_ < std::numeric_limits<int64_t>::max()) {
            id = ++lastRequest_;
            pending_.emplace(id, std::move(respond));
        }
    }
    if (!id) return respond.unsupported();
    send(id);
}

void SandboxRemoteServer::onTaskStarted(SandboxEngine&) { sink_->remoteTaskStarted(); }
void SandboxRemoteServer::onTaskCompleted(SandboxEngine&) { sink_->remoteTaskCompleted(); }
void SandboxRemoteServer::onOps(SandboxEngine&, const std::string& json) { sink_->remoteOps(json); }
QueryAnswer SandboxRemoteServer::onQuery(SandboxEngine&, const std::string& json) { return sink_->remoteQuery(json); }
void SandboxRemoteServer::onListen(SandboxEngine&, const std::string& json) { sink_->remoteListen(json); }
void SandboxRemoteServer::onPreventDefault(SandboxEngine&, const std::string& json) { sink_->remotePreventDefault(json); }
void SandboxRemoteServer::onAnim(SandboxEngine&, const std::string& json) { sink_->remoteAnim(json); }
void SandboxRemoteServer::onRequestFrame(SandboxEngine&) { sink_->remoteRequestsFrame(); }
void SandboxRemoteServer::onLog(SandboxEngine&, const std::string& level, const std::string& text) { sink_->remoteLog(level, text); }
void SandboxRemoteServer::onDiag(SandboxEngine&, const std::string& kind, const std::string& detail) { sink_->remoteDiag(kind, detail); }
// The raw call right after it is what the client checks again: an action goes once.
void SandboxRemoteServer::onBridgeAction(SandboxEngine&, const WidgetAction&) {}
void SandboxRemoteServer::onBridge(SandboxEngine&, const std::string& method, const std::string& argsJson) { sink_->remoteBridge(method, argsJson); }
void SandboxRemoteServer::onBridgeCopyText(SandboxEngine&, const std::string& text) { sink_->remoteCopyText(text); }
void SandboxRemoteServer::onStorage(SandboxEngine&, const std::string& area, const StorageArea& values) {
    sdk::StorageCommit commit;
    (area == "local" ? commit.local : commit.session) = values;
    sink_->remoteStorage(commit.toJson().dump());
}
void SandboxRemoteServer::onResizeAnchor(SandboxEngine&, const std::string& edge) { sink_->remoteResizeAnchor(edge); }
void SandboxRemoteServer::onPublicDataWatch(SandboxEngine&, bool on) { sink_->remoteWatchesPublicData(on); }
void SandboxRemoteServer::onBridgeRequest(SandboxEngine&, const WidgetRequest& request, WidgetRequestResponder respond) {
    forward(std::move(respond), [&](int64_t id) { sink_->remoteRequest(id, request.data, request.wallet); });
}
void SandboxRemoteServer::onBridgeWallet(SandboxEngine&, const WalletCall& call, WidgetRequestResponder respond) {
    forward(std::move(respond), [&](int64_t id) { sink_->remoteWallet(id, SandboxRemoteCoding::encodeWallet(call)); });
}
void SandboxRemoteServer::onLoaded(SandboxEngine&) { sink_->remoteLoaded(); }
void SandboxRemoteServer::onStats(SandboxEngine&, const SandboxStats& stats) { sink_->remoteStats(stats); }
void SandboxRemoteServer::onDied(SandboxEngine&, const std::string& reason) { sink_->remoteDied(reason); }

// MARK: the loopback

namespace detail {

/// The engine's side's sink in a loopback (`loopbackSink`): the client's, its stats coded on the way.
class LoopbackSink final : public SandboxRemoteSink {
public:
    explicit LoopbackSink(std::shared_ptr<SandboxRemoteSink> client) : client_(std::move(client)) {}

    void remoteTaskStarted() override { client_->remoteTaskStarted(); }
    void remoteTaskCompleted() override { client_->remoteTaskCompleted(); }
    void remoteOps(const std::string& json) override { client_->remoteOps(json); }
    QueryAnswer remoteQuery(const std::string& json) override { return client_->remoteQuery(json); }
    std::string remoteIntl(const std::string& requestJson) override { return client_->remoteIntl(requestJson); }
    void remoteListen(const std::string& json) override { client_->remoteListen(json); }
    void remotePreventDefault(const std::string& json) override { client_->remotePreventDefault(json); }
    void remoteAnim(const std::string& json) override { client_->remoteAnim(json); }
    void remoteRequestsFrame() override { client_->remoteRequestsFrame(); }
    void remoteLog(const std::string& level, const std::string& text) override { client_->remoteLog(level, text); }
    void remoteDiag(const std::string& kind, const std::string& detail) override { client_->remoteDiag(kind, detail); }
    void remoteBridge(const std::string& method, const std::string& argsJson) override { client_->remoteBridge(method, argsJson); }
    void remoteCopyText(const std::string& text) override { client_->remoteCopyText(text); }
    void remoteRequest(int64_t id, const std::string& data, bool wallet) override { client_->remoteRequest(id, data, wallet); }
    void remoteWallet(int64_t id, const std::string& payload) override { client_->remoteWallet(id, payload); }
    void remoteStorage(const std::string& commitJson) override { client_->remoteStorage(commitJson); }
    void remoteResizeAnchor(const std::string& edge) override { client_->remoteResizeAnchor(edge); }
    void remoteWatchesPublicData(bool on) override { client_->remoteWatchesPublicData(on); }
    void remoteLoaded() override { client_->remoteLoaded(); }
    void remoteStats(const SandboxStats& stats) override {
        if (std::optional<SandboxStats> coded = SandboxRemoteCoding::decodeStats(SandboxRemoteCoding::encodeStats(stats))) client_->remoteStats(*coded);
        else client_->remoteDisconnected();
    }
    void remoteDied(const std::string& reason) override { client_->remoteDied(reason); }
    void remoteDisconnected() override { client_->remoteDisconnected(); }
    void remotePlatformMessage(const std::string& message) override { client_->remotePlatformMessage(message); }
    std::optional<std::string> remotePlatformRequest(const std::string& message) override { return client_->remotePlatformRequest(message); }
    void remoteValidatedOps(const std::string& json) override { client_->remoteValidatedOps(json); }
    void remoteRendererReady() override { client_->remoteRendererReady(); }
    void remoteRendered() override { client_->remoteRendered(); }
    void remoteNodes(int64_t nodes) override { client_->remoteNodes(nodes); }

private:
    const std::shared_ptr<SandboxRemoteSink> client_;
};

std::shared_ptr<SandboxRemoteSink> loopbackSink(std::shared_ptr<SandboxRemoteSink> client) { return std::make_shared<LoopbackSink>(std::move(client)); }

} // namespace detail

SandboxLoopbackChannel::SandboxLoopbackChannel(std::shared_ptr<SandboxResources> resources) : resources_(std::move(resources)) {}

void SandboxLoopbackChannel::attach(std::shared_ptr<SandboxRemoteSink> sink) {
    client_ = std::move(sink);
    server_ = std::make_unique<SandboxRemoteServer>(detail::loopbackSink(client_), resources_);
}

void SandboxLoopbackChannel::start(const std::string& html, const SandboxConfig& config) {
    if (std::optional<SandboxConfig> coded = SandboxRemoteCoding::decodeConfig(SandboxRemoteCoding::encodeConfig(config))) server_->start(html, *coded);
    else client_->remoteDisconnected();
}
void SandboxLoopbackChannel::event(const std::string& json) { server_->event(json); }
void SandboxLoopbackChannel::activationEvent(const std::string& json, int gesture) { server_->activationEvent(json, gesture); }
void SandboxLoopbackChannel::frame(double time) { server_->frame(time); }
void SandboxLoopbackChannel::viewport(double innerWidth, double innerHeight, double dpr) { server_->viewport(innerWidth, innerHeight, dpr); }
void SandboxLoopbackChannel::resize(const std::string& entriesJson) { server_->resize(entriesJson); }
void SandboxLoopbackChannel::theme(const std::string& colorScheme, const std::map<std::string, std::string>& themeParams) { server_->theme(colorScheme, themeParams); }
void SandboxLoopbackChannel::contentSafeArea(const WidgetContentSafeArea& area) {
    if (std::optional<WidgetContentSafeArea> coded = SandboxRemoteCoding::decodeSafeArea(SandboxRemoteCoding::encodeSafeArea(area))) server_->contentSafeArea(*coded);
    else client_->remoteDisconnected();
}
void SandboxLoopbackChannel::safeArea(const WidgetSafeArea& area) {
    if (std::optional<WidgetSafeArea> coded = SandboxRemoteCoding::decodeDeviceSafeArea(SandboxRemoteCoding::encodeDeviceSafeArea(area))) server_->safeArea(*coded);
    else client_->remoteDisconnected();
}
void SandboxLoopbackChannel::setData(const WidgetData& data) {
    if (std::optional<WidgetData> coded = SandboxRemoteCoding::decodeData(SandboxRemoteCoding::encodeData(data))) server_->setData(*coded);
    else client_->remoteDisconnected();
}
void SandboxLoopbackChannel::setVisible(bool visible) { server_->setVisible(visible); }
void SandboxLoopbackChannel::pressBegan() { server_->pressBegan(); }
void SandboxLoopbackChannel::pressEnded() { server_->pressEnded(); }
void SandboxLoopbackChannel::chargeValidator(double ms) { server_->chargeValidator(ms); }
void SandboxLoopbackChannel::stop(const std::string& reason) { server_->stop(reason); }
void SandboxLoopbackChannel::answer(int64_t id, const SandboxRemoteOutcome& outcome) {
    if (std::optional<SandboxRemoteOutcome> coded = SandboxRemoteCoding::decodeOutcome(SandboxRemoteCoding::encodeOutcome(outcome))) server_->answer(id, *coded);
    else client_->remoteDisconnected();
}
std::string SandboxLoopbackChannel::probe(const std::string& code) { return server_->probe(code); }
void SandboxLoopbackChannel::close() { server_->close(); }

} // namespace twidget
