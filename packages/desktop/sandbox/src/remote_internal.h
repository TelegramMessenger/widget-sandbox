// What the engine side's translation units of the remote engine share without making it public: the engine's side's Intl provider over a sink (remote_engine.cpp's server, remote_hosted.cpp's runtime), the loopback's sink (remote_engine.cpp's, which remote_hosted.cpp's loopback uses too), the hosted runtime the engine's side drives (built in remote_hosted.cpp, driven by remote_stream_engine.cpp) and the stream's serving loop (remote_stream_engine.cpp's). Library sources only; nothing under include/ names it, and no source of the host's side (the client, the stream's host end, the coding) includes it.
#pragma once

#include "twidget/intl.h"
#include "twidget/json.h"
#include "twidget/remote_engine.h"
#include "twidget/remote_hosted.h"
#include "twidget/remote_stream.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace twidget::detail {

/// The engine's IntlProvider on the engine's side: the host's provider formats each request through the synchronous `remoteIntl`.
class RemoteIntl final : public IntlProvider {
public:
    explicit RemoteIntl(std::shared_ptr<SandboxRemoteSink> sink) : sink_(std::move(sink)) {}
    std::string format(const Json& request) override { return sink_->remoteIntl(request.dump()); }

private:
    std::shared_ptr<SandboxRemoteSink> sink_;
};

/// The engine's side's sink in a loopback: every call straight to `client`'s, stats coded on the way as a transport's bytes would be (a form that does not decode is a lost connection), a hosted runtime's calls too.
std::shared_ptr<SandboxRemoteSink> loopbackSink(std::shared_ptr<SandboxRemoteSink> client);

/// The engine's side of one hosted widget (remote_hosted.cpp's SandboxHostedRuntime, built by `makeHostedRuntime`), with `SandboxRemoteServer`'s rules: the calls but `probe` come one at a time in the host's order (a channel's thread) and never wait for the widget, the inputs before `start` are kept and those after `close` dropped, the owner destroys it on its own thread after its last call. Only inline pure virtuals, so a transport's translation unit drives it without referencing the one that builds it.
class HostedRuntime {
public:
    virtual ~HostedRuntime() = default;
    /// Builds the widget's engine, validator and renderer (the factory's, given the platform) and starts it, once, never after `close`; a build that fails reports the widget's death to the sink and closes the runtime.
    virtual void start(const std::string& html, const SandboxConfig& config, const SandboxHostedStart& start) = 0;
    virtual void theme(const WidgetTheme& theme) = 0;
    virtual void contentSafeArea(const WidgetContentSafeArea& area) = 0;
    /// The device's edges (the served host's `setSafeArea`).
    virtual void safeArea(const WidgetSafeArea& area) = 0;
    /// A profile page's viewport as the host checked it (the served host's `setViewportHeight`).
    virtual void viewportHeight(double height) = 0;
    virtual void setData(const WidgetData& data) = 0;
    virtual void setVisible(bool visible) = 0;
    /// The host stopped the widget: its engine dies with `reason`.
    virtual void stop(const std::string& reason) = 0;
    /// Settles the forwarded request or wallet call `id`, once.
    virtual void answer(int64_t id, const SandboxRemoteOutcome& outcome) = 0;
    /// The widget's probe, a barrier both ways: it runs after the engine work the inputs before it queued through the glue's thread, and what its glue sent before it is on the sink before it returns; "ERROR no runtime" before `start`, after `close` and once its engine died. Never from the thread answering the runtime's synchronous calls.
    virtual std::string probe(const std::string& code) = 0;
    /// A platform message for the renderer, on the glue's host thread in order (`SandboxHostedPlatform::onMessage`).
    virtual void platform(const std::string& message) = 0;
    /// The client is gone: the kept inputs dropped, the forwarded calls refused, the platform closed (its requests answer nullopt) and the engine terminated without waiting. Idempotent.
    virtual void close() = 0;
};

/// A hosted runtime that sends every call of its widget to `sink`, its widget built from `resources` with `renderers`' renderer.
std::unique_ptr<HostedRuntime> makeHostedRuntime(std::shared_ptr<SandboxRemoteSink> sink, std::shared_ptr<SandboxResources> resources, HostedRendererFactory renderers);

/// What a hosting stream's engine end makes for each runtime it opens, on its reader: the runtime sending its widget's calls to `sink`, its widget built from `resources` (the hosting opening's).
using HostedRuntimes = std::function<std::unique_ptr<HostedRuntime>(std::shared_ptr<SandboxRemoteSink> sink, std::shared_ptr<SandboxResources> resources)>;

/// `SandboxRemoteEngineEnd::serve` with `hosted` empty (a `HostingOpening` refused as another first kind), `serveHosting` with its runtimes: one loop in remote_stream_engine.cpp, which reaches a hosted runtime only through `hosted` and the interface above.
std::string serveStream(SandboxRemoteStream& stream, const std::string& build, const std::function<std::string()>& lockdown, const HostedRuntimes& hosted);

} // namespace twidget::detail
