// The whole widget in another process (desktop core only; Swift and Kotlin have no hosted mode): the engine process hosts a widget's engine, a TrustedValidator and a renderer its embedder builds, so ops, layout queries, events and frame requests never leave it, while the host keeps a hosted client (`SandboxEngine::hostedClient`) that delivers the host's inputs, decides every privileged call again on the presses the host delivered and carries the renderer's traffic with its platform as opaque messages; when the widget's engine dies the engine process keeps its renderer, without engine or validator, its platform traffic still carried, until the host's `close` (a host that keeps it: `WidgetHost::keepRenderer`); the contract a transport and a renderer author implement (the engine-only one is remote_engine.h's).
#pragma once

#include "twidget/remote_engine.h"
#include "twidget/renderer.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace twidget {

/// Host → engine for a hosted client, with `SandboxRemoteChannel`'s rules (its dispatch thread, the host's order, no call but `probe` waits) and four calls of its own: a hosted client makes `attach`, `startHosted`, `hostedTheme`, `platform`, `kept`, `contentSafeArea`, `setData`, `setVisible`, `stop`, `answer`, `probe` and `close`, never `start`, `event`, `activationEvent`, `frame`, `viewport`, `resize`, `theme`, `pressBegan`, `pressEnded` or `chargeValidator` (the widget's own glue makes those inside the engine process); only inline virtuals are declared (no key function), so no translation unit depends on another for its vtable.
class SandboxHostedChannel : public SandboxRemoteChannel {
public:
    /// The widget, its configuration and its hosted start, once (the client's `startHosted`): the engine process builds the widget's engine, validator and renderer from them.
    virtual void startHosted(const std::string& html, const SandboxConfig& config, const SandboxHostedStart& start) = 0;
    /// The chat's whole theme changed (WidgetHost's `setTheme`), for the renderer and the widget in the engine process.
    virtual void hostedTheme(const WidgetTheme& theme) = 0;
    /// An opaque message of the host's platform for the widget's renderer, in order (`SandboxHostedPlatform::onMessage`).
    virtual void platform(const std::string& message) = 0;
    /// The host kept the widget's renderer (the client's `keepHosted`, once, after the widget died): from now on only the renderer's platform traffic crosses on this route, until `close`; a transport may count the route out of its live runtimes. The default does nothing.
    virtual void kept() {}
};

/// The renderer's side of its traffic with the host's platform, in the engine process (what a `HostedRendererFactory` gets for the renderer it builds): opaque to the framework both ways, each message at most `SandboxEngine::kRemotePlatformLimit` bytes (the host drops a longer message and answers a longer request "").
class SandboxHostedPlatform {
public:
    virtual ~SandboxHostedPlatform() = default;
    /// A message for the host's platform, from any thread; the host gets them in the order they were sent.
    virtual void send(const std::string& message) = 0;
    /// A request the renderer waits for, from any thread, the glue's host thread included (inside `onMessage` and inside the renderer calls the glue makes, while the widget lives or once its renderer is kept), but never inside the `HostedRendererFactory` (the thread that builds the renderer would wait for itself): the host's answer, given within `SandboxEngine::kPlatformAnswerSeconds` or the widget dies; a wait on the glue's host thread holds up only that widget's glue (its layout queries, its other platform messages, a probe), never another widget; one waits at a time per widget (a second waits here for the first), and nullopt once the runtime closed or the transport ended, also for a request that was waiting then.
    virtual std::optional<std::string> request(const std::string& message) = 0;
    /// The host's platform messages for the renderer, in order, on the glue's host thread (the thread every renderer call is made on, which a kept renderer keeps); set it when the renderer is built, before the factory returns.
    std::function<void(const std::string& message)> onMessage;
};

/// What the engine process's embedder gives `SandboxRemoteEngineEnd::serveHosting`: one hosted widget's renderer, as `RendererFactory` builds one, plus its platform; `resources` are the opening's (the sandbox and validator bundles with their bytecode, of the renderer bundle only its stamp line), `theme` the colour scheme, `width` the bubble's in CSS px. Called on the engine process's reader thread at the widget's hosted start; a factory that throws or returns nothing ends only that widget, its death reported to the host.
using HostedRendererFactory = std::function<std::unique_ptr<Renderer>(std::shared_ptr<SandboxResources> resources, const std::string& theme, double width,
                                                                      std::shared_ptr<SandboxHostedPlatform> platform)>;

namespace detail {
class HostedRuntime;
} // namespace detail

/// Both halves of a hosted widget in one process (tests, the harness, parity runs), the hosted twin of `SandboxLoopbackChannel`: `attach` builds the engine's side's runtime, which builds the widget's engine, validator and renderer (from `renderers`) at `startHosted` and sends what they report straight to the client's sink; the config, the hosted start, the theme, data, safe areas and outcomes cross through `SandboxRemoteCoding` as a transport's bytes would, and a form that does not decode is a lost connection (`remoteDisconnected`). The engine-only calls do nothing. `close` closes the runtime and keeps it, as the client may close from its dispatch thread while the runtime's engine waits for that thread: only this channel's destructor, on the owner's thread, destroys it.
class SandboxHostedLoopbackChannel final : public SandboxHostedChannel {
public:
    SandboxHostedLoopbackChannel(std::shared_ptr<SandboxResources> resources, HostedRendererFactory renderers);
    ~SandboxHostedLoopbackChannel() override;

    void attach(std::shared_ptr<SandboxRemoteSink> sink) override;
    void startHosted(const std::string& html, const SandboxConfig& config, const SandboxHostedStart& start) override;
    void hostedTheme(const WidgetTheme& theme) override;
    void platform(const std::string& message) override;
    void contentSafeArea(const WidgetContentSafeArea& area) override;
    void setData(const WidgetData& data) override;
    void setVisible(bool visible) override;
    void stop(const std::string& reason) override;
    void answer(int64_t id, const SandboxRemoteOutcome& outcome) override;
    std::string probe(const std::string& code) override;
    void close() override;
    void start(const std::string& html, const SandboxConfig& config) override;
    void event(const std::string& json) override;
    void activationEvent(const std::string& json, int gesture) override;
    void frame(double time) override;
    void viewport(double innerWidth, double innerHeight, double dpr) override;
    void resize(const std::string& entriesJson) override;
    void theme(const std::string& colorScheme, const std::map<std::string, std::string>& themeParams) override;
    void pressBegan() override;
    void pressEnded() override;
    void chargeValidator(double ms) override;

private:
    const std::shared_ptr<SandboxResources> resources_;
    const HostedRendererFactory renderers_;
    std::shared_ptr<SandboxRemoteSink> client_;
    std::unique_ptr<detail::HostedRuntime> runtime_;
};

} // namespace twidget
