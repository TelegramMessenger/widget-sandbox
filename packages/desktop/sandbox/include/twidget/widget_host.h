// WidgetHost: one widget = one SandboxEngine (QuickJS on its own JSThread) +
// one TrustedValidator + one Renderer (whatever the factory builds — the
// Blitz prototype here, the Web App window on tdesktop). The only place where
// the three meet; nothing from the engine reaches the renderer without passing
// the validator. The C++ copy of src/host/widget-host.ts / WidgetHost.swift /
// WidgetHost.kt.
//
// Threading. A Qt-free library has no main thread, so the host owns a "host
// thread" that stands in for it: every renderer call, every diagnostics update
// and `onChange` / `onRenderOps` / `onAction` / `onStorageCommit` /
// `onGesture` / `onRequest` / `onCopyText` / `onWatchPublicData` (and the deprecated `onGestureClaim`
// and `onBridgeCall`) run there, in order. Engine
// callbacks hop from the engine thread; a layout query blocks the engine
// thread (≤ 3 s) until the renderer answers on the host thread. A Qt host
// would replace the thread with the UI thread (`crl::on_main`) one for one.
// `destroy()` and `keepRenderer()` are called from the owner's thread, never from the host thread.
#pragma once

#include "twidget/gesture_policy.h"
#include "twidget/renderer.h"
#include "twidget/sandbox_engine.h"
#include "twidget/trusted_validator.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace twidget {

// WidgetButtonTokens, WidgetTextTokens and WidgetTheme are in twidget/sandbox_engine.h (a hosted widget's start carries the theme).
// WidgetStorage, WidgetAction and StorageArea, and WidgetData, WidgetViewer, WidgetContext, WidgetRequest,
// WidgetRequestAnswer, WidgetRequestFailure and WidgetRequestOutcome are in twidget/sdk_policy.h (the engine checks with them).

/// Limits for the widget's QuickJS runtime (see SandboxConfig); mirrors WidgetHost.Options on iOS/Android.
struct WidgetHostOptions {
    size_t memoryLimitBytes = size_t(8) << 20;
    double cpuLimitMs = 500;
    double cpuQuotaMs = 300;
    double cpuQuotaWindowMs = 1000;
    double cpuKillAfterMs = 0;
    /// The host's main-thread work for this widget per window (layout queries + 25 µs per op + 2 µs per canvas command).
    double mainThreadQuotaMs = 100;
    int maxTimers = 64;
    /// Initial visibility; change it later with `setVisible`.
    bool visible = true;
    /// Device pixel ratio the engine boots with; the renderer's `size` report overrides it.
    double devicePixelRatio = 1;
    /// "strict" (default; anything but "compat") or "compat": the gesture gate, the 32 KB text caps and the text budgets,
    /// and the validator's counted caps, past which the widget stops (`diagnostics().dead` with the cap's reason, such
    /// as `Ops limit exceeded: 12000 operations in one batch (max 10000)`; docs/decisions.md, "Strict limits from the start").
    std::string policy = "strict";
    /// Opt-in sandbox features of docs/protocol.md, such as "webAnimations"; the renderer must support them.
    std::vector<std::string> features;
    /// The client's UI language, BCP 47 (`navigator.language`, the Intl default); `en-US` when malformed.
    std::string locale = std::string(sdk::DEFAULT_LOCALE);
    /// What the client draws over the widget (docs/sdk-v1.md §2.17, §5.12): for a widget with no caption below it the band of
    /// the message time at the bottom and its corner; zero (the default) with a caption below it. Pass it here, before the first
    /// render, so the first height already has the band; later only with the bubble's layout (`setContentSafeArea`).
    WidgetContentSafeArea contentSafeArea;
    /// Web Storage of this message as the client keeps it (docs/sdk-v1.md §5.4); an area over the
    /// 4 KB quota drops the whole preload (diagnostic `storage-rejected`, the widget starts empty).
    WidgetStorage storage;
    /// The bot's `chatData` and `publicData` as the message has them now (docs/sdk-v1.md §2.13); later
    /// edits go through `setData`. A field over 1024 bytes, with a NUL or malformed is null for the widget (`data-rejected`).
    WidgetData data;
    /// The message's flags from the server (`isCopy`, `canQuery`, `backgroundQuery`), the chat type and the viewer
    /// (docs/sdk-v1.md §4.4); fixed for the host's life. Without `canQuery` every request is refused. `preview`: an
    /// inline result not sent yet (§2.16): no input (the renderer's events and gesture claims are dropped), only
    /// `ready` and `setResizeAnchor` (`preview-blocked` for the rest), no storage preload or commits.
    WidgetContext context;
};

struct WidgetDiagnostics {
    std::vector<std::pair<std::string, std::string>> logs;
    /// kind → (count, its first 50 details), at most WidgetHost::kMaxDiagKinds kinds: one of a new kind past them counts under "diag-kind-dropped", `<kind>: <detail>` its detail there.
    std::map<std::string, std::pair<int, std::vector<std::string>>> diags;
    /// The first 1000 actions `onAction` got.
    std::vector<WidgetAction> actions;
    /// Storage commits `onStorageCommit` got (one per area).
    int storageCommits = 0;
    /// The first 1000 bridge calls (method, args JSON), as the deprecated `onBridgeCall` gets them: the checked arguments re-serialized compact, a link's URL as the widget gave it; at most 276 bytes of method and arguments an entry, so 276 000 in all.
    std::vector<std::pair<std::string, std::string>> bridge;
    SandboxStats stats;
    int nodes = 0;
    bool loaded = false;
    /// Wall clock from WidgetHost creation to the renderer's `ready` and to `loaded`.
    std::optional<double> rendererReadyMs;
    std::optional<double> loadedMs;
    /// `loaded`: the widget's scripts ran; `rendered`: the renderer applied every op sent up to that point.
    bool rendered = false;
    std::optional<double> renderedMs;
    /// Fatal: the runtime is gone (memory, CPU, or a counted cap of the validator under strict). The renderer keeps the
    /// last state (frozen).
    std::optional<std::string> dead;
    /// Last uncaught widget error (non-fatal), cleared once a later task completes without one.
    std::optional<std::string> lastError;
    /// Document height in CSS px as the renderer reports it (clamped to [50, 600]).
    double height = 0;
    /// The edge of the bubble that stays put on screen when `height` changes (`TelegramWidget.setResizeAnchor`,
    /// docs/sdk-v1.md §2.12, since v0.1.3): "top" (the default: the bubble grows and shrinks downward, the content
    /// below it moves) or "bottom" (it grows upward, its bottom stays, the content above it moves). The widget's latest
    /// call; it is in the state before the size report that follows the same task's ops. No callback of its own: a
    /// change goes through `onChange`, read it there with the height.
    std::string resizeAnchor = std::string(sdk::DEFAULT_RESIZE_ANCHOR);
    /// Whether the widget watches its shared `public_data` (round 7, `TelegramWidget.watchPublicData()` /
    /// `unwatchPublicData()`): its latest call, false until then. Poll a shared widget's `public_data` only while it is on
    /// screen and this is true; a change also reaches `onWatchPublicData`. Not reset when the widget dies.
    bool watchesPublicData = false;
    /// Wall clock to the first `size` report that follows validated ops.
    std::optional<double> firstContentMs;

    int countDiag(const std::string& kind) const { auto it = diags.find(kind); return it == diags.end() ? 0 : it->second.first; }
    bool hasLog(const std::string& text) const { for (auto& l : logs) if (l.second == text) return true; return false; }
};

/// Host side of user activation: one physical pointer press (pointerdown, mousedown, pointerup, mouseup, click, dblclick)
/// is one gesture; keys never are. A click without a click count (detail 0, what Enter or Space on a focused control
/// produces) counts only as the end of an open press. Twin of GestureCounter in UserActivation.kt and user-activation.ts.
class GestureCounter {
public:
    /// A pointer press or release, or a click with a click count (detail > 0).
    static bool isUserActivation(const Json& ev);
    /// The gesture number for this renderer event, 0 when it is not a user activation.
    int gestureFor(const Json& ev);
    /// A primary press began (the renderer's `gesture` claim): what follows is a new gesture (user-activation.ts).
    void pressBegan() { pressOpen_ = false; lastType_.clear(); }

private:
    int number_ = 0;
    bool pressOpen_ = false;
    std::string lastType_;
};

class SandboxHostedChannel;
class SandboxHostedRuntime;
class WidgetHost;

/// What `WidgetHost::platformDelivery` gives inside a platform observer, a WidgetHost's or a kept renderer's: `hold` keeps the message or request being delivered counted against its client's queued bytes and calls (`SandboxEngine::kRemoteQueuedBytes`, `kRemoteQueuedCalls`) for as long as a copy of it lives, and `request` is a request's number (`SandboxHostedWait::admitted`; 0 for a message).
struct HostedPlatformDelivery {
    SandboxEngine::RemoteHold hold;
    uint64_t request = 0;
};

/// What a host keeps of a hosted widget past its WidgetHost (`WidgetHost::keepRenderer`): the engine process's renderer of that widget, without its engine or validator, reachable through its platform traffic alone, both ways under the live widget's bounds (`SandboxEngine::kRemotePlatformLimit`, the client's queued bytes and calls, one waiting request answered before it is due, as its client's SandboxHostedWait says when it has one). Nothing else crosses: no privileged call, grant, haptic, theme, data, safe area or probe. It owns the route (the hosted client) and one thread named `twidget-host` on which its observers run in order, the first only after the WidgetHost's last platform observer call. It ends at `release()` (the destructor's too), which the host asks for: no observer call after it; or without the host asking, at a death the host decides (a flood, a platform request unanswered or with no observer) or at the end of the transport, which `onEnded` reports. Every call from the owner's thread; never destroyed on its own thread.
class HostedKeptRenderer {
public:
    /// Set when the renderer is kept (`WidgetHost::keepRenderer`), each on the handle's thread; inside the two platform ones `WidgetHost::platformDelivery()` gives what they deliver.
    struct Observers {
        /// An opaque message of the renderer for the embedder's platform, in the order it sent them.
        std::function<void(const std::string& message)> onPlatformMessage;
        /// A request the renderer waits for, one at a time: call `respond` once, from any thread, before it is due (`SandboxEngine::kPlatformAnswerSeconds`, or as the client's SandboxHostedWait moved it), else the route ends (`onEnded`) and later calls are ignored; unset, the route ends at the first request.
        std::function<void(const std::string& message, std::function<void(std::string answer)> respond)> onPlatformRequest;
        /// The route ended without the host asking, once and last, with the reason (also `ended()`); never after `release()`.
        std::function<void(const std::string& reason)> onEnded;
    };

    /// `release()`.
    ~HostedKeptRenderer();
    HostedKeptRenderer(const HostedKeptRenderer&) = delete;
    HostedKeptRenderer& operator=(const HostedKeptRenderer&) = delete;

    /// An opaque message of the embedder's platform for the renderer, in order; nothing once the route ended or was released.
    void sendPlatform(std::string message);
    /// Whether the route ended without the host asking (`onEnded`), from any thread.
    bool ended() const { return ended_; }
    /// What the client holds now against its bounds, this handle's queued observer calls included (SandboxEngine::remoteBacklog); nothing once released (tests).
    SandboxEngine::RemoteBacklog remoteBacklog() const;
    /// Idempotent: the route closes (its client destroyed, the engine process told), queued observer calls are dropped, then the handle's thread ends.
    void release();

private:
    friend class WidgetHost;
    explicit HostedKeptRenderer(Observers observers);
    /// The client's delegate from the keep on: platform traffic to the observers, every other call dropped.
    class Delegate;
    SandboxEngineDelegate& delegate();
    /// The kept client, moved out of its WidgetHost on that host's thread; opens the start gate.
    void adopt(std::unique_ptr<SandboxEngine> client);
    /// The thread's first task returns: what the delegate queued before it runs (once).
    void openGate();

    JSThread thread_;
    /// The thread's first task waits for it: the observers start after `adopt` (or at a release without one).
    std::promise<void> gate_;
    bool gateOpen_ = false;
    const Observers observers_;
    std::unique_ptr<Delegate> delegate_;
    std::unique_ptr<SandboxEngine> client_;
    std::atomic<bool> ended_{false};
    std::atomic<bool> released_{false};
};

/// What a test or a harness gives `WidgetHost::setHostedChannelFactory`: the hosted channel of `host`, a WidgetHost being built with a renderer factory (remote_hosted.h; null: a widget that dies, "the widget engine process ended"). Called on the constructing thread with that host not built yet: keep the reference, call nothing on it before its constructor returned; `renderers`, `theme` (the colour scheme) and `width` are what the host would have built its renderer with.
using HostedChannelFactory = std::function<std::shared_ptr<SandboxHostedChannel>(WidgetHost& host, std::shared_ptr<SandboxResources> resources, const RendererFactory& renderers,
                                                                                 const std::string& theme, double width)>;

/// Local (the engine in process or behind `SandboxEngine::make`'s channel, the validator and the renderer here) or hosted (remote_hosted.h, desktop core only: the engine, the validator and the renderer in the engine process, a hosted client here that decides every privileged call on the presses the embedder delivered, `pressDelivered`, and carries the renderer's traffic with its platform, `sendPlatform` / `onPlatformMessage` / `onPlatformRequest`). In hosted mode `renderer()` throws, `event()`, `onGesture` and `onGestureClaim` do nothing, `height` and `firstContentMs` stay unset (the embedder's, from its renderer's platform traffic), and `onRenderOps` gets the engine process's validated batches only when set before `start()`.
class WidgetHost final : public SandboxEngineDelegate {
public:
    /// Throws JSError if the validator bundle fails to load, std::system_error when one of its threads cannot start (JSThread; those already started are joined first). Nothing runs until `start()`: set the observers and `stoppedLabel` before it; an engine behind a channel that failed while it was built (`SandboxEngine::make`) dies after it. Hosted, with neither validator nor renderer here, when a hook is set (`setHostedChannelFactory`) and no engine channel factory is (`SandboxEngine::remoteChannelFactorySet`).
    WidgetHost(std::string html, WidgetTheme theme, double width, WidgetHostOptions options, std::shared_ptr<SandboxResources> resources,
               RendererFactory rendererFactory, std::shared_ptr<IntlProvider> intl = nullptr);
    /// Hosted (remote_hosted.h): no validator and no renderer here, a hosted client over `hosted` (`SandboxEngine::hostedClient`; null: a widget that dies, "the widget engine process ended"), whatever the hook and the engine channel factory say; `intl` answers the widget's Intl requests on the client's dispatch thread; with `wait` its renderer's platform requests are due as that record says (SandboxHostedWait), also once kept, without it `kPlatformAnswerSeconds` after their arrival.
    WidgetHost(std::string html, WidgetTheme theme, double width, WidgetHostOptions options, std::shared_ptr<SandboxHostedChannel> hosted, std::shared_ptr<IntlProvider> intl = nullptr,
               std::shared_ptr<SandboxHostedWait> wait = nullptr);
    /// The API before SDK v1: a colour scheme only (the scheme's default palette, no button tokens).
    [[deprecated("pass a WidgetTheme (SDK v1)")]]
    WidgetHost(std::string html, std::string theme, double width, WidgetHostOptions options, std::shared_ptr<SandboxResources> resources,
               RendererFactory rendererFactory, std::shared_ptr<IntlProvider> intl = nullptr)
        : WidgetHost(std::move(html), WidgetTheme{std::move(theme), {}, {}, {}}, width, std::move(options), std::move(resources), std::move(rendererFactory), std::move(intl)) {}
    ~WidgetHost();
    WidgetHost(const WidgetHost&) = delete;
    WidgetHost& operator=(const WidgetHost&) = delete;

    /// Loads the renderer; the engine boots once the renderer announced itself (`ready`). Hosted: the hosted start, once, with the theme, `stoppedLabel` and whether `onRenderOps` is set (the ask for the validated batches); the engine process loads its renderer.
    void start();
    /// The chat's theme changed: stylesheets are re-sanitised against the scheme, the renderer gets the scheme and the variables, the widget gets `colorScheme`, `themeParams` and `themeChanged` in one task. Hosted: the whole theme to the engine process, which does all of that there.
    void setTheme(const WidgetTheme& theme);
    [[deprecated("pass a WidgetTheme (SDK v1)")]]
    void setTheme(const std::string& theme) { setTheme(WidgetTheme{theme, {}, {}, {}}); }
    /// Off-screen pause: no animation frames, timers at 1 Hz, `document.hidden` inside the widget.
    void setVisible(bool visible);
    /// The bot's data changed (an edit the client got from the server): the full state of both strings. The
    /// widget gets `chatDataChanged`, then `publicDataChanged`, for each that differs; a copy keeps no `chatData` (its
    /// publicData can change); a field over the limit is null (`data-rejected`).
    void setData(const WidgetData& data);
    /// What the client draws over the widget changed with the bubble's layout (docs/sdk-v1.md §2.17, §5.12): a caption
    /// added or removed by an edit, an "edited" mark that widens the time. The renderer gets the root variables and the
    /// widget `contentSafeAreaInset` and `contentSafeAreaChanged`, from this one call; never per frame.
    void setContentSafeArea(const WidgetContentSafeArea& area);
    /// A DOM event for the widget that did not come from the renderer (the harness's `--events`); nothing in hosted mode, whose input is the engine process's renderer's.
    void event(const std::string& json);
    /// Trusted diagnostic code evaluated in the widget context (tests / harness only).
    std::string probe(const std::string& code);
    /// Stops the engine and the renderer. Idempotent; not from the host thread. Storage commits already on their way reach `onStorageCommit` before it returns; no action and no storage commit comes after it. A platform request waiting for `onPlatformRequest` gets no answer and the widget dies "terminated".
    void destroy();
    /// Hosted only, from the owner's thread (never the host thread): ends the widget as `destroy()` does (a live one dies "terminated") but keeps its route, so the engine process's renderer stays reachable through the handle after this host is destroyed (HostedKeptRenderer). Everything the client handed this host before the keep reaches its own observers before this returns, the platform traffic and a waiting platform request among it (`respond` still works after the keep); from then on the handle's observers get the rest. Null, with the widget ended as `destroy()` ends it, when there is nothing to keep: a local host, a host never started, destroyed or kept already, a renderer that never announced itself, a route that ended (the transport, a flood, a platform request unanswered or with no `onPlatformRequest`), a death the engine process reported without `keepsRenderer`, or a handle whose thread cannot start (its `twidget-host`; nothing is thrown). This host's inputs do nothing afterwards.
    std::unique_ptr<HostedKeptRenderer> keepRenderer(HostedKeptRenderer::Observers observers);

    /// The renderer the factory built; its own API (touches, paints) is the owner's business. Throws `std::logic_error` in hosted mode, whose renderer is the engine process's.
    Renderer& renderer();
    /// Whether the widget runs hosted (remote_hosted.h), fixed at construction.
    bool isHosted() const { return hosting_.hosted; }
    /// For every later WidgetHost built with a renderer factory while no engine channel factory is set (`SandboxEngine::remoteChannelFactorySet`): it runs hosted over the channel this gives (tests and harnesses); an empty one (the default) builds local hosts. From any thread.
    static void setHostedChannelFactory(HostedChannelFactory factory);
    /// Hosted only (else nothing): an opaque message of the embedder's platform for the engine process's renderer, in order with the other inputs; none once the route closed (at the widget's death unless `keepsRenderer`, at `destroy()` or `keepRenderer()`).
    void sendPlatform(std::string message);
    /// Hosted only (else nothing): the embedder delivered a primary press to the widget's renderer (`SandboxEngine::pressDelivered`), the only source of the widget's grants and of its haptic touch: one privileged call within 2 s while visible.
    void pressDelivered();
    /// Hosted only (else nothing): `sendPlatform` for a message that delivers a primary press to the engine process's renderer, with that press (`pressDelivered`) in the same step (`SandboxEngine::platformPress`), timed from this call: the grant and the haptic touch exist only when the transport's writer admitted the message on the open, unkept route (`SandboxHostedChannel::platform`); nothing once the route closed, is ending or its writer refused it, and the route's close revokes the grant. It stays void: the client decides on its dispatch thread.
    void sendPlatformPress(std::string message);
    /// Hosted only (else nothing): that press ended (lifted or cancelled), the haptic touch window follows it (`SandboxEngine::releaseDelivered`), only for a press that counted.
    void releaseDelivered();
    /// How long a platform request of the engine process's renderer waits for `onPlatformRequest`'s `respond`, from its arrival or as the constructor's SandboxHostedWait moved it: then diagnostic `platform-timeout` and the widget dies.
    static constexpr double kPlatformAnswerSeconds = SandboxEngine::kPlatformAnswerSeconds;
    /// Inside `onPlatformMessage` or `onPlatformRequest`, a WidgetHost's or a kept renderer's (HostedKeptRenderer::Observers), on the thread that runs it, the first call on a handle's thread too: the message or request it delivers (HostedPlatformDelivery); empty anywhere else. Copy it before the observer returns: kept with the work it queues, the delivery stays counted against the client's bounds until that work ran or was dropped.
    static HostedPlatformDelivery platformDelivery();
    /// Hosted only (else nothing): a diagnostic of the embedder's own checks of the widget's platform traffic, as a local renderer's `onDiag` reports one: recorded on the host thread in the bounded `diagnostics().diags`, then `onChange`; nothing once `destroy()` or `keepRenderer()` returned.
    void platformDiag(const std::string& kind, const std::string& detail);
    /// Snapshot of the diagnostics (any thread).
    WidgetDiagnostics diagnostics() const;
    /// Runs `fn` on the host thread and waits for it (inline when already there).
    void onHostThread(const std::function<void()>& fn) { hostThread_.sync(fn); }
    /// What a client-mode engine holds now against its remote bounds, this host's queued work for it included (SandboxEngine::remoteBacklog), from the owner's thread once a keep may happen (it moves the engine); nothing in process or after a keep (tests).
    SandboxEngine::RemoteBacklog remoteBacklog() const { return engine_ ? engine_->remoteBacklog() : SandboxEngine::RemoteBacklog{}; }
    /// Bytes of a client-mode engine's renderer messages the host keeps until the renderer's `ready`, each as serialized with its wrapper: the message past this or `kRendererBacklogMessages` stops the widget once (diagnostic "backlog-limit"), what was kept is dropped and nothing the engine sends afterwards has an effect; the host's own messages always fit, an engine in process keeps nothing there.
    static constexpr size_t kRendererBacklogBytes = SandboxEngine::kRemoteQueuedBytes;
    /// Those messages counted, under the same rule.
    static constexpr size_t kRendererBacklogMessages = SandboxEngine::kRemoteQueuedCalls;
    /// Distinct kinds `diagnostics().diags` keeps, "diag-kind-dropped" among them.
    static constexpr size_t kMaxDiagKinds = 64;

    bool waitLoaded(double timeoutSeconds = 20);
    /// True once the post-load ack query came back from the renderer (or the widget died); hosted, once the engine process's glue reported it.
    bool waitRendered(double timeoutSeconds = 5);
    /// Polls `pred` on a diagnostics snapshot until true or timeout.
    bool waitUntil(const std::function<bool(const WidgetDiagnostics&)>& pred, double timeoutSeconds = 3);

    // Observers, all on the host thread.
    std::function<void(const WidgetDiagnostics&)> onChange;
    /// Every validated batch on its way to the renderer (what the harness scans for leaks). Hosted: the engine process's batches, only when set before `start()` (the hosted start asks for them then); set later, or unset, none.
    std::function<void(const std::string& renderOpsJson)> onRenderOps;
    /// What the widget asked the client to do, checked (URL rules, user activation, the haptic rate limit;
    /// docs/sdk-v1.md §3.2, §5.2, §5.3). At most one link action per pointer press.
    std::function<void(const WidgetAction& action)> onAction;
    /// The new full content of a Web Storage area, "local" or "session", at the end of each task that changed it
    /// (docs/sdk-v1.md §5.4). Within the quota; a lone surrogate of the widget's string arrives in its WTF-8 form.
    std::function<void(const std::string& area, const std::map<std::string, std::string>& values)> onStorageCommit;
    /// On every primary pointerdown (the renderer's `gesture` message, which also starts the haptic gate's touch; its
    /// `release` message ends it, v0.2.2), what of this press the widget claims with `touch-action` (docs/sdk-v1.md §5.5):
    /// horizontal drags (`x`) and holds (`hold`: no message menu, the widget gets `contextmenu`); taps and double taps
    /// are always the widget's, vertical drags always the chat's (`y` is always false since v0.1.3, whatever the
    /// renderer sends). Feed it to the GestureArbiter of the client's recognizers over this widget
    /// (twidget/gesture_policy.h). It arrives a few ms after the touch starts. A renderer that reports only `wants`
    /// (the Blitz and litehtml prototypes) claims nothing.
    std::function<void(const WidgetGestureClaim& claim)> onGesture;
    /// A request the widget sends to the bot (`TelegramWidget.request`, docs/sdk-v1.md §2.14), checked: the
    /// data (1–64 bytes), the message's `WidgetContext::canQuery`, a press (one request per press, shared with the links
    /// and copyText) or, without one, `backgroundQuery` (one when the widget appears on screen, then one per 5 s while
    /// visible; one made off screen waits for `setVisible(true)`), one in flight. Send it like a callback button's press and call `respond` once, from any thread, with the
    /// server's answer (`result`) or a failure (`Timeout`, `RateLimited`, `Failed`); later calls, and calls after
    /// `destroy()`, are ignored. Until then the
    /// widget's next request is BUSY. Read on the host thread when a request is delivered: unset then, the widget gets
    /// `NOT_ALLOWED` (reason `unsupported`).
    std::function<void(const WidgetRequest& request, std::function<void(const WidgetRequestOutcome&)> respond)> onRequest;
    /// A wallet call (`TelegramWidget.wallet`, docs/sdk-v1.md §2.18; checked: the transaction or the payload, a press or
    /// the grant, one call in flight with the requests). Only with `WidgetContext::wallet`. Show the client's own wallet
    /// window with the bot of the message; on confirmation send the transaction (or sign for `bot<bot_id>`), send the
    /// outcome to the bot as a query with `call.data` and call `respond` once, from any thread, with the bot's answer or
    /// a failure (`Rejected`: the user declined, `BadRequest`: the wallet refused it, or a request's). Read on the host
    /// thread at delivery: unset then, the widget gets `NOT_ALLOWED` (reason `unsupported`).
    std::function<void(const WalletCall& call, std::function<void(const WidgetRequestOutcome&)> respond)> onWallet;
    /// Copy this text to the clipboard and show a toast that says what was copied (`TelegramWidget.copyText`):
    /// 1–256 UTF-16 code units, well-formed, at most one privileged call per pointer press. Not an action.
    std::function<void(const std::string& text)> onCopyText;
    /// Whether the widget watches its shared `public_data` changed (round 7): poll it only while true and the widget is on
    /// screen. Called only on a change, after `diagnostics().watchesPublicData` took the new value. Not an action.
    std::function<void(bool on)> onWatchPublicData;
    /// Hosted only: an opaque message of the engine process's renderer for the embedder's platform (at most `SandboxEngine::kRemotePlatformLimit` bytes), in the order it sent them and in order with the widget's other reports; none after `destroy()` or `keepRenderer()` returned.
    std::function<void(const std::string& message)> onPlatformMessage;
    /// Hosted only: a request the engine process's renderer waits for (one at a time); call `respond` once, from any thread, before it is due (`kPlatformAnswerSeconds`, or as the constructor's SandboxHostedWait moved it), else the widget dies (diagnostic `platform-timeout`) and later calls are ignored. Read when the request arrives: unset then, the widget dies at once. The client waits for the answer on the request's own thread, never on its dispatch thread; still never wait for the widget in it: a `probe` from inside may wait for the very request it answers.
    std::function<void(const std::string& message, std::function<void(std::string answer)> respond)> onPlatformRequest;
    /// The widget's death as `diagnostics().dead` recorded it, once, on the host thread, right after that and `onChange`, before a local renderer gets `frozen`: its own limit, a validator cap, the client's own end of it (a lost engine process, a flood, a platform request left unanswered), in either mode. Never for the end the owner asked for (`destroy()`, `keepRenderer()`, a death that comes once either began), nor for a kept route's end (`HostedKeptRenderer::Observers::onEnded`). Set it before `start()`.
    std::function<void(const std::string& reason)> onDeath;
    /// Deprecated since v0.1.2 (`onGesture`): the claim's `x` and `y` (always false), right after `onGesture`.
    [[deprecated("use onGesture (v0.1.2)")]]
    std::function<void(bool x, bool y)> onGestureClaim;
    /// Deprecated since SDK v1 (`onAction`): the raw method and arguments JSON of each call `onAction` gets, right after it.
    [[deprecated("use onAction (SDK v1)")]]
    std::function<void(const std::string& method, const std::string& argsJson)> onBridgeCall;
    /// Non-fatal uncaught exception in widget code (also in `diagnostics().logs`).
    std::function<void(const std::string& text)> onError;
    /// Badge text shown by the renderer over a dead widget (user-facing, no error details).
    std::string stoppedLabel = "Widget stopped";
    /// Hosted only, read at `start()`: a death the engine process reports (a CPU or memory limit, a validator cap, the widget's own error) leaves the widget's route open, its renderer's platform traffic still carried to `sendPlatform` / `onPlatformMessage` / `onPlatformRequest` under the live bounds, until `keepRenderer()` takes it or `destroy()` closes it. Unset (the default), such a death closes the route at once and nothing more crosses.
    bool keepsRenderer = false;

    // SandboxEngineDelegate (engine thread)
    void onOps(SandboxEngine&, const std::string& json) override;
    QueryAnswer onQuery(SandboxEngine&, const std::string& json) override;
    void onListen(SandboxEngine&, const std::string& json) override;
    void onPreventDefault(SandboxEngine&, const std::string& json) override;
    void onAnim(SandboxEngine&, const std::string& json) override;
    void onRequestFrame(SandboxEngine&) override;
    void onLog(SandboxEngine&, const std::string& level, const std::string& text) override;
    void onDiag(SandboxEngine&, const std::string& kind, const std::string& detail) override;
    void onBridgeAction(SandboxEngine&, const WidgetAction& action) override;
    void onResizeAnchor(SandboxEngine&, const std::string& edge) override;
    void onPublicDataWatch(SandboxEngine&, bool on) override;
    void onStorage(SandboxEngine&, const std::string& area, const StorageArea& values) override;
    void onBridgeRequest(SandboxEngine&, const WidgetRequest& request, WidgetRequestResponder respond) override;
    void onBridgeWallet(SandboxEngine&, const WalletCall& call, WidgetRequestResponder respond) override;
    void onBridgeCopyText(SandboxEngine&, const std::string& text) override;
    void onBridge(SandboxEngine&, const std::string& method, const std::string& argsJson) override;
    void onLoaded(SandboxEngine&) override;
    void onStats(SandboxEngine&, const SandboxStats& stats) override;
    void onDied(SandboxEngine&, const std::string& reason) override;
    // A hosted client's reports (its dispatch thread): platform messages and requests, the asked-for batches, the engine process's renderer ready, rendered and its node count.
    void onHostedMessage(SandboxEngine&, const std::string& message) override;
    void onHostedRequest(SandboxEngine&, const std::string& message, HostedRequestResponder respond) override;
    void onHostedOps(SandboxEngine&, const std::string& json) override;
    void onHostedRendererReady(SandboxEngine&) override;
    void onHostedRendered(SandboxEngine&) override;
    void onHostedNodes(SandboxEngine&, int nodes) override;

    /// Renderer events that count as a user gesture: clicks and presses of Enter or Space (not a repeat, not a Command shortcut).
    static bool isUserActivation(const Json& ev) { return GestureCounter::isUserActivation(ev); }

private:
    /// Builds the served WidgetHost of a hosted widget in the engine process (remote_hosted.cpp) and routes its engine's calls.
    friend class SandboxHostedRuntime;
    /// What the served host's glue reports as it happens (each a call when set; empty in the local and hosted modes): a diagnostic it recorded, the renderer's first `ready`, the first render acknowledgement, the node count after a batch when it changed.
    struct GlueReports {
        std::function<void(const std::string& kind, const std::string& detail)> diag;
        std::function<void()> rendererReady;
        std::function<void()> rendered;
        std::function<void(int nodes)> nodes;
    };
    struct ServedTag {};
    /// The served host (the engine process's, SandboxHostedRuntime's): local, from the host's `config` as its hosted start carried it (`innerWidth` the renderer's width) with the scheme and the palette of `theme`, its engine built here directly (never `SandboxEngine::make`) with `engineDelegate` as its delegate, its glue's reports to `reports`.
    WidgetHost(ServedTag, std::string html, WidgetTheme theme, SandboxConfig config, std::shared_ptr<SandboxResources> resources, RendererFactory renderers,
               std::shared_ptr<IntlProvider> intl, SandboxEngineDelegate& engineDelegate, GlueReports reports);
    /// Whether the widget is hosted, and the hook's channel until the constructor hands it to the engine (which then holds the only reference).
    struct Hosting {
        bool hosted = false;
        std::shared_ptr<SandboxHostedChannel> channel;
    };
    /// The renderer factory constructor's mode: hosted over the hook's channel while a hook is set and no engine channel factory is, else local.
    static Hosting hostingFor(WidgetHost& host, const std::shared_ptr<SandboxResources>& resources, const RendererFactory& renderers, const std::string& theme, double width);
    /// The local and served constructors' body: the validator's policy and CPU sink, the engine's delegate (an engine in process now), the renderer's callbacks.
    void wire(SandboxEngineDelegate& engineDelegate);
    /// The served host's cut once its engine died (SandboxHostedRuntime), on the host thread after the death's reports, once and never after `destroy()`: from then on the renderer's messages, the engine's queued work and every input stop at the glue, a waiting layout query answers "dead", the engine is released (`SandboxEngine::release`: its thread and QuickJS context go) and the validator destroyed (its thread and context), so only the renderer, its platform and this thread stay.
    void keepRendererOnly();
    /// Host thread: the platform observers get nothing more; while keeping, only once `keepRenderer` handed the route over, so what came before the keep still reaches them.
    bool platformClosed() const { return keeping_ ? callbacksClosed_ : bool(destroyed_); }
    /// A served host's glue thread inside its renderer's platform requests as one reading: `at`, the total time up to `at` (a wait in progress included) and whether one is in progress then.
    struct GlueWaited {
        std::chrono::steady_clock::time_point at;
        std::chrono::steady_clock::duration total{};
        bool waiting = false;
    };
    /// The served host's reading (SandboxHostedRuntime sets it before `start()`): a layout query's 3 s leave that time out. Empty in the local and hosted modes, whose queries keep their 3 s.
    std::function<GlueWaited()> glueWaits_;
    /// `glueWaits_`'s reading, or now and nothing without one.
    GlueWaited glueWaited() const;
    /// SandboxHostedRuntime, on the glue thread, outside its accounting: a wait inside a platform request ended, so a layout query waiting through it reads again; never once the runtime's destructor stopped watching that thread, before this host goes (its `destroy()` answers such a query).
    void glueWaitEnded();

    /// Fed only the events the renderer marks `trusted`.
    GestureCounter gestures_;
    struct PendingQuery {
        int id = 0;
        bool done = false;
        std::string json = "null";
        std::optional<std::vector<uint8_t>> binary;
        /// A served host's glue waits in its renderer's platform requests in all, as they stood when it was answered (`glueWaits_`).
        std::chrono::steady_clock::duration glueWaited{};
    };
    /// A message the renderer gets at its `ready`; the engine's count against the backlog bounds and are dropped at the cut.
    struct PendingMessage {
        std::string json;
        bool fromEngine = false;
    };

    void onRenderer(const std::string& json);
    void handleRendererMessage(const Json& msg);
    void toRenderer(const std::string& json);
    /// `toRenderer` for the engine's message, kept within the backlog bounds before `ready`: false when it is not sent or kept (destroyed, cut, the renderer kept alone, or the message that cuts).
    bool engineToRenderer(std::string json);
    /// `{"type":"theme","theme":{"colorScheme","params","button"}}` of the current theme (docs/sdk-v1.md §3.4).
    std::string themeMessage() const;
    /// `{"type":"safeArea","inset":{…}}` (docs/sdk-v1.md §3.4).
    static std::string safeAreaMessage(const WidgetContentSafeArea& area);
    void resolveQuery(const std::string& json, std::optional<std::vector<uint8_t>> binary);
    void diag(const std::string& kind, const std::string& detail);
    void changed();
    double elapsedMs() const;
    /// Runs `fn` on the host thread (inline when already there); queued, it keeps `hold` until it has run or is dropped.
    void host(std::function<void()> fn, SandboxEngine::RemoteHold hold = nullptr);
    /// `host` for the work an engine's message queued: skipped once the engine is cut or the renderer kept alone (`keepRendererOnly`), `hold` kept until it has run or is dropped either way.
    void engineWork(std::function<void()> fn, SandboxEngine::RemoteHold hold);
    /// The validator's diagnostic, on its thread: inside `validated` it is counted as work of its own of that engine's message, and dropped when that does not fit.
    void validatorDiag(const std::string& kind, const std::string& detail);
    /// `validate`'s value on the engine's thread, what the validator reports meanwhile counted with `engine`'s message (`validatorDiag`).
    template <class F>
    auto validated(SandboxEngine& engine, F validate);

    /// The local mode's halves of the calls both modes take (widget_host_local.cpp), each run from its call (widget_host.cpp) through `glue_`: the six render calls whole, then `start()`'s and `setTheme`'s past the hosted branch, `setContentSafeArea`'s renderer message, `onLoaded`'s validator mark (engine thread) and render acknowledgement (host thread), `onDied`'s once the death is recorded, `destroy()`'s before the engine stops and its renderer's end on the host thread.
    void localOps(SandboxEngine& engine, const std::string& json);
    QueryAnswer localQuery(SandboxEngine& engine, const std::string& json);
    void localListen(SandboxEngine& engine, const std::string& json);
    void localPreventDefault(SandboxEngine& engine, const std::string& json);
    void localAnim(SandboxEngine& engine, const std::string& json);
    void localRequestFrame(SandboxEngine& engine);
    void localStart();
    void localSetTheme();
    void localSafeArea(const WidgetContentSafeArea& area);
    void localLoaded();
    void localRenderAck();
    void localDied(const std::string& effective);
    void localDestroying();
    void localDestroyRenderer();
    /// Those halves as one table, constant-initialised in widget_host_local.cpp (`kGlue`): the calls reach them only through it, so a program whose hosts are all hosted ones links neither them nor the validator.
    struct Glue {
        void (WidgetHost::*ops)(SandboxEngine& engine, const std::string& json);
        QueryAnswer (WidgetHost::*query)(SandboxEngine& engine, const std::string& json);
        void (WidgetHost::*listen)(SandboxEngine& engine, const std::string& json);
        void (WidgetHost::*preventDefault)(SandboxEngine& engine, const std::string& json);
        void (WidgetHost::*anim)(SandboxEngine& engine, const std::string& json);
        void (WidgetHost::*requestFrame)(SandboxEngine& engine);
        void (WidgetHost::*start)();
        void (WidgetHost::*setTheme)();
        void (WidgetHost::*safeArea)(const WidgetContentSafeArea& area);
        void (WidgetHost::*loaded)();
        void (WidgetHost::*renderAck)();
        void (WidgetHost::*died)(const std::string& effective);
        void (WidgetHost::*destroying)();
        void (WidgetHost::*destroyRenderer)();
    };
    static const Glue kGlue;

    /// qid of the host's own post-load ack query; engine queries count up from 1.
    static constexpr int kRenderAckQid = -1;

    // Before the threads and the validator: a diagnostic the validator reports while it is built (`bundle-mismatch`, `bytecode`) reaches `diag` on the host thread at once, which may run it as late as its final join.
    mutable std::mutex mutex_;              // diagnostics_, waiters
    std::condition_variable cv_;
    WidgetDiagnostics diagnostics_;
    /// Where the served host's `diag` reports too: the served constructor initializes it first.
    const GlueReports reports_;

    JSThread hostThread_;
    std::string html_;
    /// The colour scheme ("light" | "dark") and the whole theme with its palette checked (host thread after start).
    std::string theme_;
    WidgetTheme widgetTheme_;
    /// What the engine boots with, made from the options (a served host's from its hosted start's): the host reads its policy and preview flag here.
    SandboxConfig config_;
    std::shared_ptr<SandboxResources> resources_;
    /// Fixed at construction: only `hosted` is read after it.
    Hosting hosting_;
    /// `&kGlue`, set by the local and served constructors alone; null in hosted mode, whose client makes none of the render calls and whose other calls have no local half to run.
    const Glue* glue_ = nullptr;
    /// The engine the validator works for in `validated`, under `validatingMutex_`; before the validator, which may report while it is built.
    std::mutex validatingMutex_;
    SandboxEngine* validating_ = nullptr;
    /// Null in hosted mode, as `renderer_` is. The local constructors give it its deleter (widget_host_local.cpp: the CPU sink cleared, then deleted), so only that file names the validator's destructor; none without one.
    std::unique_ptr<TrustedValidator, void (*)(TrustedValidator*)> validator_;
    std::unique_ptr<Renderer> renderer_;
    /// `SandboxEngine::make`'s: in process, or a client of an engine behind the host's channel factory; a hosted client (`SandboxEngine::hostedClient`) in hosted mode; the served host's own, in process.
    std::unique_ptr<SandboxEngine> engine_;

    // Host-thread state.
    bool rendererReady_ = false;
    bool engineStarted_ = false;
    bool started_ = false;
    /// Set at the end of destroy(): storage commits queued before it still reach `onStorageCommit` (the
    /// widget's last state is worth keeping); none reaches the owner once destroy() has returned.
    bool callbacksClosed_ = false;
    std::vector<PendingMessage> pendingToRenderer_;
    /// Bytes and count of the engine's messages in `pendingToRenderer_`, within `kRendererBacklogBytes` and `kRendererBacklogMessages`.
    size_t backlogBytes_ = 0;
    size_t backlogMessages_ = 0;
    /// Set once, when the engine's message passed those bounds: nothing the engine sends afterwards has an effect.
    bool engineCut_ = false;
    double viewportW_, viewportH_, viewportDpr_;
    int qid_ = 0;
    int opsSent_ = 0;
    int tasksAtError_ = 0;

    std::atomic<bool> destroyed_{false};
    /// Set by `keepRenderer` before `destroyed_`: the platform observers keep what came before the keep (`platformClosed`).
    std::atomic<bool> keeping_{false};
    /// Set by the served host's cut (`keepRendererOnly`) on the host thread, read there and, at a layout query's entry, on the engine's thread.
    std::atomic<bool> rendererOnly_{false};
    std::mutex queryMutex_;
    std::condition_variable queryCv_;
    std::shared_ptr<PendingQuery> pendingQuery_;
    const std::chrono::steady_clock::time_point createdAt_ = std::chrono::steady_clock::now();
};

} // namespace twidget
