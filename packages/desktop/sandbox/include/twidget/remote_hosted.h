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

namespace detail {
/// The hosted client's side of `SandboxHostedChannel::death` (sandbox_engine_client.cpp).
struct HostedDeaths;
} // namespace detail

/// What a hosted channel's `decided()` reads, held apart from the channel (`SandboxHostedChannel::decidedView`): the transport keeps it in the same steps, under its own lock, that change what `decided()` reads, before anyone hears of them, and one end that decides several widgets turns all their views at once, so a reader that sees one of them decided by that end sees every other one it decided as decided too, whatever order it reads them in. It owns no channel, route, connection, sink or thread; only inline virtuals are declared (no key function).
class SandboxHostedDecidedView {
public:
    virtual ~SandboxHostedDecidedView() = default;
    /// What the channel's `decided()` reads as of the transport's last such step, from any thread, as often as one likes, with no lock of the transport's, also after the channel went.
    virtual bool decided() const = 0;
};

/// Host → engine for a hosted client, with `SandboxRemoteChannel`'s rules (its dispatch thread, the host's order, no call but `probe` waits) and four calls of its own: a hosted client makes `attach`, `startHosted`, `hostedTheme`, `platform`, `kept`, `contentSafeArea`, `setData`, `setVisible`, `stop`, `answer`, `probe` and `close`, never `start`, `event`, `activationEvent`, `frame`, `viewport`, `resize`, `theme`, `pressBegan`, `pressEnded` or `chargeValidator` (the widget's own glue makes those inside the engine process); only inline virtuals are declared (no key function), so no translation unit depends on another for its vtable.
class SandboxHostedChannel : public SandboxRemoteChannel {
public:
    /// What the first death its hosted client reported of the widget was to the transport (`death()`): only a hosting stream with its owner's SandboxHostedAnswers (remote_stream.h) decides one other than `Ordinary`, for the death its end brought, and only when that end was the widget's first death.
    enum class Death : uint8_t {
        /// The widget's own death, or an end that decided nothing of it: a death as any other (the widget's engine, a flood, a platform request left unanswered, the host's own stop of the widget before the end, `localStop`, the owner's stop of the stream, any other transport).
        Ordinary,
        /// The stream ended because this widget's renderer left a message of the host's unanswered past its answer time: the widget is the one that hung the engine process.
        Stalled,
        /// The stream ended for another widget or for no widget's fault while this one had nothing unanswered past its time (at a renderer's stall), nothing unanswered (at another end, when another widget had), or attached to a stream already ending so: not this widget's death.
        Spared,
    };

    /// The widget, its configuration and its hosted start, once (the client's `startHosted`): the engine process builds the widget's engine, validator and renderer from them.
    virtual void startHosted(const std::string& html, const SandboxConfig& config, const SandboxHostedStart& start) = 0;
    /// The chat's whole theme changed (WidgetHost's `setTheme`), for the renderer and the widget in the engine process.
    virtual void hostedTheme(const WidgetTheme& theme) = 0;
    /// An opaque message of the host's platform for the widget's renderer, in order (`SandboxHostedPlatform::onMessage`): whether the transport's writer admitted it for this live route (a stream's queue took its frame); false, and it never goes out, once the route is not live, closed or not yet attached, the stream ends, or the writer refuses it. A hosted client grants a press only on true (`SandboxEngine::platformPress`).
    virtual bool platform(const std::string& message) = 0;
    /// The host kept the widget's renderer (the client's `keepHosted`, once, after the widget died): from now on only the renderer's platform traffic crosses on this route, until `close`; a transport may count the route out of its live runtimes. The default does nothing.
    virtual void kept() {}

    /// What the widget's first death its hosted client reported was to the transport, from any thread: `Ordinary` until then and for every death but the transport's end, whose decision (`Death`) the client publishes (`publishEndedDeath`) on its dispatch thread before it reports that death to its delegate (or keeps it for a delegate set later), and which never changes afterwards. So the embedder reads it in the hosted WidgetHost's `onDeath`, through the very channel it gave that host's constructor (a weak reference: the client owns the channel), and gets the kind of the death it is being told about; a death of the widget's own reported earlier (a flood, a platform request left unanswered, a refused start, its engine's death, a counted stop that came before the end, the owner's keep) keeps `Ordinary` whatever the transport later decides, and so does a later end; a counted stop that comes after an end that decided the widget yields to it (`localStop`), and its client reports that end. The default, for a channel that decides nothing, is always `Ordinary`; a transport that decides (a hosting stream's channel) keeps the published value itself, so this class holds no state and a channel of the embedder's own keeps the copies and moves it had.
    virtual Death death() const { return Death::Ordinary; }
    /// Whether the transport already decided the widget's death, from any thread, changing nothing, also before its hosted client publishes that decision (`death()`) or reports the death: true once the transport's end took the widget into its decision (froze or settled its death, `Ordinary` included, which `death()` reads alike before and after its publication), from then on; true too once the client claimed a death of the widget's own first (a flood, a platform request left unanswered, its engine's death) or the host stopped the widget first (`localStop`), unless the owner's withdrawal then came before any end (`withdraw`'s `ended` false). False before either, for a widget the owner withdrew first, and after an end that decided nothing of it (the owner's stop of the stream). So an owner that counts the widget's deaths can hold back another run of the same widget while one's death is decided but not yet reported. A channel that decides nothing (the default, and a hosting stream's opened without its owner's SandboxHostedAnswers, remote_stream.h) reads false whatever stops or claims the widget; the base still holds no state.
    virtual bool decided() const { return false; }
    /// What `decided()` reads, as a view of its own (SandboxHostedDecidedView) the transport keeps in the same step that changes it, under its own lock and before anyone hears of that change (a claim of the widget's own, the client's or the host's `localStop`; the end that froze the route, or attached it to a stream already ending so; the owner's withdrawal, which clears a claim no end decided): so an owner that holds it reads `decided()` from any thread, as often as it likes, with no lock of the transport's, without the channel and after it went, a value that changes only in those steps. An end that freezes several routes turns their views together, once it froze them all: a reader never sees one route of that end decided and, later, another one it froze still undecided. The view owns no channel, route, connection, sink or thread, and it is the same view on every call: obtain it once. None for a channel that decides nothing (the default), whose `decided()` always reads false; a hosting stream opened without its owner's SandboxHostedAnswers gives one that stays false.
    virtual std::shared_ptr<const SandboxHostedDecidedView> decidedView() const { return nullptr; }

    /// What the owner's withdrawal found (`withdraw`).
    struct Withdrawal {
        /// The transport's end came first and took the widget into its decision, or found the widget's own death (a flood, a platform request left unanswered, the host's local stop) already claimed on its open route: the widget's death is the one it settles.
        bool ended = false;
        /// That death, once settled: its hosted client's first death when it was its own one (a flood, a platform request left unanswered, its engine's death: `Ordinary`) or the end, else, as cleanup takes the route from the client (the owner's stop, a keep, a close), the end's decision. None while it is still to settle: `settled` hears it then, once.
        std::optional<Death> death;
        /// The withdrawal came before any end, but a death of the widget's own was already claimed on its route (a flood, a platform request left unanswered, its engine's death, the host's `localStop`): read from the route in the same step that takes it out of the transport's decisions, so a claim its client made after the owner's last `decided()` is still told. Only the route's first withdrawal tells it, as `ended` false with no death settled and nothing for `settled` to hear. False for a route still undecided or already withdrawn, after an end that came first (`ended`), on every later withdrawal, and for a channel that decides nothing.
        bool own = false;
    };
    /// The owner takes the widget out of its transport's decisions (a host that retires it), once, from any thread, in one step with the transport's end: when the end has not decided the widget's death yet (`ended` false) it never will, nothing of the widget counts toward the transport's limits or blame from now on, and the route still carries what it carried (a kept renderer's traffic, for one), while `own` tells whether a death of the widget's own was claimed before that step; when the end came first the widget's death is the one it settled (`death`) or will settle, which `settled` then hears once, outside every lock of the transport's, on one of its threads or the client's, before the client reports that death to its delegate when it does. The default, for a channel that decides nothing: the withdrawal came first, with no claim.
    virtual Withdrawal withdraw(std::function<void(Death death)> /*settled*/) { return {}; }
    /// The host stops the widget of its own accord (its live platform end refused the widget's renderer for good, or its hosted client's counted `SandboxEngine::stop` on the client's dispatch thread), from any thread, in one step with the transport's end and before it tells anyone of that stop. None when no end took the widget into its decision before it: the stop is the widget's own death, claimed as a flood's is, so nothing of the widget is kept, timed or blamed from now on and an end that comes later finds it decided (`decided`; `withdraw` reads it `Ordinary`); none too when a death of the widget's own was claimed first or the owner withdrew the widget first, which stays as it was. The end's decision when that end came first, frozen or settled: it stands, unchanged (the hosted client still reports that end, which publishes it for `death`, a counted stop that yielded so delivering nothing more of the widget's own or of its renderer's platform traffic to the delegate until then and leaving that death to no later keep, terminate, stop or platform request, and a withdrawal that waits for it still hears it once). A later call finds the same winner, the end's decision as it then stands. It credits nothing the renderer owes and closes nothing. A channel that decides nothing (the default, and a hosting stream's opened without its owner's SandboxHostedAnswers): none, and nothing of the route changes.
    virtual std::optional<Death> localStop() { return std::nullopt; }

private:
    friend struct detail::HostedDeaths;
    /// The transport's end is the widget's first death (the hosted client, on its dispatch thread, once, before it reports that death): a transport that decides publishes what its end made of this route for `death` and settles it (`withdraw`). The default does nothing.
    virtual void publishEndedDeath() {}
    /// A death of the widget's own is its first death (a flood, a platform request left unanswered, its engine's death; the hosted client, on its dispatch thread, once, before it reports that death): a transport that decides keeps that claim in one step with its end, so a death its end already decided settles `Ordinary` (`withdraw`), unlike a later `localStop`, and before its end the widget counts toward its limits and blame no more. The default does nothing.
    virtual void publishOwnDeath() {}
};

/// The renderer's side of its traffic with the host's platform, in the engine process (what a `HostedRendererFactory` gets for the renderer it builds): opaque to the framework both ways, each message at most `SandboxEngine::kRemotePlatformLimit` bytes (the host drops a longer message and answers a longer request "").
class SandboxHostedPlatform {
public:
    virtual ~SandboxHostedPlatform() = default;
    /// A message for the host's platform, from any thread; the host gets them in the order they were sent.
    virtual void send(const std::string& message) = 0;
    /// A request the renderer waits for, from any thread, the glue's host thread included (inside `onMessage` and inside the renderer calls the glue makes, while the widget lives or once its renderer is kept), but never inside the `HostedRendererFactory` (the thread that builds the renderer would wait for itself): the host's answer, given before it is due (`SandboxEngine::kPlatformAnswerSeconds` after its arrival, or as the host's SandboxHostedWait moved it) or the widget dies; a wait on the glue's host thread holds up only that widget's glue (its layout queries, its other platform messages, a probe), never another widget; one waits at a time per widget (a second waits here for the first), and nullopt once the runtime closed or the transport ended, also for a request that was waiting then, and at once for one made after the close: a renderer may keep its platform past its widget.
    virtual std::optional<std::string> request(const std::string& message) = 0;
    /// The host's platform messages for the renderer, in order, on the glue's host thread (the thread every renderer call is made on, which a kept renderer keeps); set it when the renderer is built, before the factory returns.
    std::function<void(const std::string& message)> onMessage;
};

/// What the engine process's embedder gives `SandboxRemoteEngineEnd::serveHosting`: one hosted widget's renderer, as `RendererFactory` builds one, plus its platform; `resources` are the opening's (the sandbox and validator bundles, of the renderer bundle only its stamp line), `theme` the colour scheme, `width` the bubble's in CSS px. Called on the engine process's reader thread at the widget's hosted start; a factory that throws or returns nothing ends only that widget, its death reported to the host.
using HostedRendererFactory = std::function<std::unique_ptr<Renderer>(std::shared_ptr<SandboxResources> resources, const std::string& theme, double width,
                                                                      std::shared_ptr<SandboxHostedPlatform> platform)>;

namespace detail {
class HostedRuntime;
} // namespace detail

/// Both halves of a hosted widget in one process (tests, the harness, parity runs), the hosted twin of `SandboxLoopbackChannel`: `attach` builds the engine's side's runtime, which builds the widget's engine, validator and renderer (from `renderers`) at `startHosted` and sends what they report straight to the client's sink; the config, the hosted start, the theme, data, safe areas and outcomes cross through `SandboxRemoteCoding` as a transport's bytes would, and a form that does not decode is a lost connection (`remoteDisconnected`). The engine-only calls do nothing. `platform` hands the message to the runtime at once, admitted until the client's `close` (no writer refuses here). `close` closes the runtime and keeps it, as the client may close from its dispatch thread while the runtime's engine waits for that thread: only this channel's destructor, on the owner's thread, destroys it.
class SandboxHostedLoopbackChannel final : public SandboxHostedChannel {
public:
    SandboxHostedLoopbackChannel(std::shared_ptr<SandboxResources> resources, HostedRendererFactory renderers);
    ~SandboxHostedLoopbackChannel() override;

    void attach(std::shared_ptr<SandboxRemoteSink> sink) override;
    void startHosted(const std::string& html, const SandboxConfig& config, const SandboxHostedStart& start) override;
    void hostedTheme(const WidgetTheme& theme) override;
    bool platform(const std::string& message) override;
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
    /// The client's `close` came (its dispatch thread makes every call, its destructor the last after joining it): `platform` admits nothing more.
    bool closed_ = false;
};

} // namespace twidget
