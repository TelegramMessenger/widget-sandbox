// The engine process's side of a hosted widget (remote_hosted.h, desktop core only): SandboxHostedRuntime builds the widget's engine, a TrustedValidator and the embedder's renderer as a served WidgetHost and routes that engine's calls, keeps the renderer alone once that engine died until the host's `Close`, and the in-process loopback and the stream's `serveHosting` run over it. A translation unit of its own, so the engine-only transport (remote_engine.cpp, remote_stream.cpp) references no WidgetHost or validator.
#include "twidget/remote_hosted.h"

#include "remote_internal.h"
#include "twidget/remote_stream.h"
#include "twidget/widget_host.h"

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace twidget {

namespace {

/// A hosted renderer's platform over its runtime's sink: each message straight to it, the requests one at a time; nothing once the runtime closed.
class HostedPlatform final : public SandboxHostedPlatform {
public:
    explicit HostedPlatform(std::shared_ptr<SandboxRemoteSink> sink) : sink_(std::move(sink)) {}

    void send(const std::string& message) override {
        if (!closed_) sink_->remotePlatformMessage(message);
    }
    std::optional<std::string> request(const std::string& message) override {
        std::lock_guard<std::mutex> l(requesting_);
        if (closed_) return std::nullopt;
        return sink_->remotePlatformRequest(message);
    }
    /// From now on no message goes out, and every request, one waiting for its turn too, answers nullopt.
    void close() { closed_ = true; }

private:
    const std::shared_ptr<SandboxRemoteSink> sink_;
    std::mutex requesting_;
    std::atomic<bool> closed_{false};
};

constexpr const char* kNotBuilt = "the widget could not be built in the engine process: ";

} // namespace

/// One hosted widget on the engine's side: from its start a served WidgetHost (the glue between its engine, validator and renderer, as in process) whose engine has this as its delegate. The render calls go to the served host's own delegate methods, the engine's other calls to an unstarted SandboxRemoteServer that relays them to the sink and numbers its requests and wallet calls for `answer`; the glue's reports, the asked-for batches and the renderer's platform traffic go to the sink directly. The death and the load go through the served host's thread first, so they reach the sink after what the glue sent before them. At its engine's death (a limit of its own, the host's `stop`) the renderer stays, without engine or validator, until `close` and the owner's destruction: after the death's reports the served host's cut (`WidgetHost::keepRendererOnly`) frees them on the glue's thread, every input but the renderer's platform traffic is dropped and a probe answers "ERROR no runtime", while that traffic still crosses both ways.
class SandboxHostedRuntime final : public detail::HostedRuntime, public SandboxEngineDelegate {
public:
    SandboxHostedRuntime(std::shared_ptr<SandboxRemoteSink> sink, std::shared_ptr<SandboxResources> resources, HostedRendererFactory renderers)
        : sink_(std::move(sink)), resources_(std::move(resources)), renderers_(std::move(renderers)), forwarder_(sink_, resources_), platform_(std::make_shared<HostedPlatform>(sink_)) {}
    /// Closes, then destroys the served host, joining its threads while this delegate and the forwarder still exist.
    ~SandboxHostedRuntime() override;
    SandboxHostedRuntime(const SandboxHostedRuntime&) = delete;
    SandboxHostedRuntime& operator=(const SandboxHostedRuntime&) = delete;

    void start(const std::string& html, const SandboxConfig& config, const SandboxHostedStart& start) override;
    void theme(const WidgetTheme& theme) override {
        input([theme](WidgetHost& host) { host.setTheme(theme); });
    }
    void contentSafeArea(const WidgetContentSafeArea& area) override {
        input([area](WidgetHost& host) { host.setContentSafeArea(area); });
    }
    void setData(const WidgetData& data) override {
        input([data](WidgetHost& host) { host.setData(data); });
    }
    void setVisible(bool visible) override {
        input([visible](WidgetHost& host) { host.setVisible(visible); });
    }
    void stop(const std::string& reason) override {
        input([reason](WidgetHost& host) { host.engine_->stop(reason); });
    }
    void answer(int64_t id, const SandboxRemoteOutcome& outcome) override { forwarder_.answer(id, outcome); }
    std::string probe(const std::string& code) override;
    void platform(const std::string& message) override {
        input(
            [platform = platform_, message](WidgetHost& host) {
                host.host([platform, message] {
                    if (platform->onMessage) platform->onMessage(message);
                });
            },
            true);
    }
    void close() override;

    // SandboxEngineDelegate (the served engine's thread)
    void onOps(SandboxEngine& engine, const std::string& json) override { served().onOps(engine, json); }
    QueryAnswer onQuery(SandboxEngine& engine, const std::string& json) override { return served().onQuery(engine, json); }
    void onListen(SandboxEngine& engine, const std::string& json) override { served().onListen(engine, json); }
    void onPreventDefault(SandboxEngine& engine, const std::string& json) override { served().onPreventDefault(engine, json); }
    void onAnim(SandboxEngine& engine, const std::string& json) override { served().onAnim(engine, json); }
    void onRequestFrame(SandboxEngine& engine) override { served().onRequestFrame(engine); }
    void onLog(SandboxEngine& engine, const std::string& level, const std::string& text) override { forwarder_.onLog(engine, level, text); }
    void onDiag(SandboxEngine& engine, const std::string& kind, const std::string& detail) override { forwarder_.onDiag(engine, kind, detail); }
    void onBridgeAction(SandboxEngine& engine, const WidgetAction& action) override { forwarder_.onBridgeAction(engine, action); }
    void onBridge(SandboxEngine& engine, const std::string& method, const std::string& argsJson) override { forwarder_.onBridge(engine, method, argsJson); }
    void onBridgeCopyText(SandboxEngine& engine, const std::string& text) override { forwarder_.onBridgeCopyText(engine, text); }
    void onStorage(SandboxEngine& engine, const std::string& area, const StorageArea& values) override { forwarder_.onStorage(engine, area, values); }
    void onResizeAnchor(SandboxEngine& engine, const std::string& edge) override { forwarder_.onResizeAnchor(engine, edge); }
    void onPublicDataWatch(SandboxEngine& engine, bool on) override { forwarder_.onPublicDataWatch(engine, on); }
    void onBridgeRequest(SandboxEngine& engine, const WidgetRequest& request, WidgetRequestResponder respond) override {
        forwarder_.onBridgeRequest(engine, request, std::move(respond));
    }
    void onBridgeWallet(SandboxEngine& engine, const WalletCall& call, WidgetRequestResponder respond) override { forwarder_.onBridgeWallet(engine, call, std::move(respond)); }
    void onStats(SandboxEngine& engine, const SandboxStats& stats) override { forwarder_.onStats(engine, stats); }
    // The `Loaded` report after the loading tasks' batches and node counts, then the glue's render acknowledgement.
    void onLoaded(SandboxEngine& engine) override {
        WidgetHost& host = served();
        host.host([this, &engine] { forwarder_.onLoaded(engine); });
        host.onLoaded(engine);
    }
    // The death after the glue's reports before it (a validator's `limit` among them), then the renderer frozen with the host's `stoppedLabel`, then the cut that keeps the renderer alone: in the glue thread's order, so the death's reports and `frozen` come first.
    void onDied(SandboxEngine& engine, const std::string& reason) override {
        WidgetHost& host = served();
        host.host([this, &engine, reason] { forwarder_.onDied(engine, reason); });
        host.onDied(engine, reason);
        {
            std::lock_guard<std::mutex> l(mutex_);
            engineEnded_ = true;
        }
        host.host([&host] { host.keepRendererOnly(); });
    }

private:
    /// `call` with the served host: kept until it is built, dropped after `close` and, unless it is the renderer's platform traffic (`rendererTraffic`), once the engine ended; the lock only decides, the host is called after it.
    void input(std::function<void(WidgetHost&)> call, bool rendererTraffic = false);
    /// The served host for `start` (throws as WidgetHost's constructor does).
    std::unique_ptr<WidgetHost> build(const std::string& html, const SandboxConfig& config, const SandboxHostedStart& start);
    /// For the served engine's delegate calls, which come only once the host is built.
    WidgetHost& served() { return *served_.load(); }

    const std::shared_ptr<SandboxRemoteSink> sink_;
    const std::shared_ptr<SandboxResources> resources_;
    const HostedRendererFactory renderers_;
    /// Never started: the relay of the engine's own calls and the numbering of its requests and wallet calls.
    SandboxRemoteServer forwarder_;
    std::mutex mutex_;
    bool started_ = false;
    bool closed_ = false;
    /// The served engine died (`onDied`): only the renderer is kept.
    bool engineEnded_ = false;
    std::vector<std::function<void(WidgetHost&)>> waiting_;
    const std::shared_ptr<HostedPlatform> platform_;
    std::unique_ptr<WidgetHost> host_;
    /// `host_` as its engine's calls see it: set with it before the host starts and never cleared, so the calls its engine makes while the host is destroyed still find it.
    std::atomic<WidgetHost*> served_{nullptr};
};

SandboxHostedRuntime::~SandboxHostedRuntime() {
    close();
    std::unique_ptr<WidgetHost> host;
    {
        std::lock_guard<std::mutex> l(mutex_);
        host = std::move(host_);
    }
    // Outside the lock: the host's destructor joins its threads, whose last calls come here.
    host.reset();
}

std::unique_ptr<WidgetHost> SandboxHostedRuntime::build(const std::string& html, const SandboxConfig& config, const SandboxHostedStart& start) {
    RendererFactory renderers = [renderers = renderers_, platform = platform_](std::shared_ptr<SandboxResources> resources, const std::string& theme, double width) -> std::unique_ptr<Renderer> {
        if (!renderers) return nullptr;
        return renderers(std::move(resources), theme, width, platform);
    };
    const std::shared_ptr<SandboxRemoteSink> sink = sink_;
    WidgetHost::GlueReports reports;
    reports.diag = [sink](const std::string& kind, const std::string& detail) { sink->remoteDiag(kind, detail); };
    reports.rendererReady = [sink] { sink->remoteRendererReady(); };
    reports.rendered = [sink] { sink->remoteRendered(); };
    reports.nodes = [sink](int nodes) { sink->remoteNodes(nodes); };
    std::unique_ptr<WidgetHost> host(new WidgetHost(WidgetHost::ServedTag{}, html, start.theme, config, resources_, std::move(renderers), std::make_shared<detail::RemoteIntl>(sink_),
                                                    *this, std::move(reports)));
    host->stoppedLabel = start.stoppedLabel;
    if (start.validatedOps) host->onRenderOps = [sink](const std::string& json) { sink->remoteValidatedOps(json); };
    return host;
}

void SandboxHostedRuntime::start(const std::string& html, const SandboxConfig& config, const SandboxHostedStart& start) {
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (closed_ || started_) return;
        started_ = true;
    }
    std::unique_ptr<WidgetHost> built;
    std::string failure;
    try {
        built = build(html, config, start);
    } catch (const std::exception& e) {
        failure = e.what();
    } catch (...) {
        failure = "an exception of an unknown type";
    }
    if (!built) {
        {
            std::lock_guard<std::mutex> l(mutex_);
            closed_ = true;
            waiting_.clear();
        }
        platform_->close();
        forwarder_.close();
        sink_->remoteDied(kNotBuilt + failure);
        return;
    }
    WidgetHost& host = *built;
    std::vector<std::function<void(WidgetHost&)>> waiting;
    bool closed = false;
    {
        std::lock_guard<std::mutex> l(mutex_);
        served_ = &host;
        host_ = std::move(built);
        closed = closed_;
        waiting.swap(waiting_);
    }
    // Closed while it was built: it never starts and goes with the runtime.
    if (closed) return;
    for (auto& call : waiting) call(host);
    host.start();
}

void SandboxHostedRuntime::input(std::function<void(WidgetHost&)> call, bool rendererTraffic) {
    WidgetHost* host = nullptr;
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (closed_ || (engineEnded_ && !rendererTraffic)) return;
        if (!host_) return waiting_.push_back(std::move(call));
        host = host_.get();
    }
    call(*host);
}

std::string SandboxHostedRuntime::probe(const std::string& code) {
    WidgetHost* host = nullptr;
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (!closed_ && !engineEnded_) host = host_.get();
    }
    if (!host) return "ERROR no runtime";
    // A barrier both ways, as in process: the inputs before the probe posted their engine work from the glue's thread first, and what the glue made of the engine's calls before it is on the sink before its answer.
    host->onHostThread([] {});
    std::string answer = host->probe(code);
    host->onHostThread([] {});
    return answer;
}

void SandboxHostedRuntime::close() {
    WidgetHost* host = nullptr;
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (std::exchange(closed_, true)) return;
        waiting_.clear();
        host = host_.get();
    }
    platform_->close();
    forwarder_.close();
    // A released engine (its death's cut) drops it.
    if (host) host->engine_->terminate();
}

namespace detail {

std::unique_ptr<HostedRuntime> makeHostedRuntime(std::shared_ptr<SandboxRemoteSink> sink, std::shared_ptr<SandboxResources> resources, HostedRendererFactory renderers) {
    return std::make_unique<SandboxHostedRuntime>(std::move(sink), std::move(resources), std::move(renderers));
}

} // namespace detail

// MARK: the stream

std::string SandboxRemoteEngineEnd::serveHosting(SandboxRemoteStream& stream, const std::string& build, const std::function<std::string()>& lockdown, HostedRendererFactory renderers) {
    return detail::serveStream(stream, build, lockdown, [renderers = std::move(renderers)](std::shared_ptr<SandboxRemoteSink> sink, std::shared_ptr<SandboxResources> resources) {
        return detail::makeHostedRuntime(std::move(sink), std::move(resources), renderers);
    });
}

// MARK: the loopback

SandboxHostedLoopbackChannel::SandboxHostedLoopbackChannel(std::shared_ptr<SandboxResources> resources, HostedRendererFactory renderers)
    : resources_(std::move(resources)), renderers_(std::move(renderers)) {}

SandboxHostedLoopbackChannel::~SandboxHostedLoopbackChannel() = default;

void SandboxHostedLoopbackChannel::attach(std::shared_ptr<SandboxRemoteSink> sink) {
    client_ = std::move(sink);
    runtime_ = detail::makeHostedRuntime(detail::loopbackSink(client_), resources_, renderers_);
}

void SandboxHostedLoopbackChannel::startHosted(const std::string& html, const SandboxConfig& config, const SandboxHostedStart& start) {
    std::optional<SandboxConfig> coded = SandboxRemoteCoding::decodeConfig(SandboxRemoteCoding::encodeConfig(config));
    std::optional<SandboxHostedStart> hosted = SandboxRemoteCoding::decodeHostedStart(SandboxRemoteCoding::encodeHostedStart(start));
    if (coded && hosted) runtime_->start(html, *coded, *hosted);
    else client_->remoteDisconnected();
}
void SandboxHostedLoopbackChannel::hostedTheme(const WidgetTheme& theme) {
    if (std::optional<WidgetTheme> coded = SandboxRemoteCoding::decodeTheme(SandboxRemoteCoding::encodeTheme(theme))) runtime_->theme(*coded);
    else client_->remoteDisconnected();
}
void SandboxHostedLoopbackChannel::platform(const std::string& message) { runtime_->platform(message); }
void SandboxHostedLoopbackChannel::contentSafeArea(const WidgetContentSafeArea& area) {
    if (std::optional<WidgetContentSafeArea> coded = SandboxRemoteCoding::decodeSafeArea(SandboxRemoteCoding::encodeSafeArea(area))) runtime_->contentSafeArea(*coded);
    else client_->remoteDisconnected();
}
void SandboxHostedLoopbackChannel::setData(const WidgetData& data) {
    if (std::optional<WidgetData> coded = SandboxRemoteCoding::decodeData(SandboxRemoteCoding::encodeData(data))) runtime_->setData(*coded);
    else client_->remoteDisconnected();
}
void SandboxHostedLoopbackChannel::setVisible(bool visible) { runtime_->setVisible(visible); }
void SandboxHostedLoopbackChannel::stop(const std::string& reason) { runtime_->stop(reason); }
void SandboxHostedLoopbackChannel::answer(int64_t id, const SandboxRemoteOutcome& outcome) {
    if (std::optional<SandboxRemoteOutcome> coded = SandboxRemoteCoding::decodeOutcome(SandboxRemoteCoding::encodeOutcome(outcome))) runtime_->answer(id, *coded);
    else client_->remoteDisconnected();
}
std::string SandboxHostedLoopbackChannel::probe(const std::string& code) { return runtime_->probe(code); }
void SandboxHostedLoopbackChannel::close() { runtime_->close(); }

// The engine-only calls, which a hosted client never makes: the widget's own glue makes them inside the runtime.
void SandboxHostedLoopbackChannel::start(const std::string&, const SandboxConfig&) {}
void SandboxHostedLoopbackChannel::event(const std::string&) {}
void SandboxHostedLoopbackChannel::activationEvent(const std::string&, int) {}
void SandboxHostedLoopbackChannel::frame(double) {}
void SandboxHostedLoopbackChannel::viewport(double, double, double) {}
void SandboxHostedLoopbackChannel::resize(const std::string&) {}
void SandboxHostedLoopbackChannel::theme(const std::string&, const std::map<std::string, std::string>&) {}
void SandboxHostedLoopbackChannel::pressBegan() {}
void SandboxHostedLoopbackChannel::pressEnded() {}
void SandboxHostedLoopbackChannel::chargeValidator(double) {}

} // namespace twidget
