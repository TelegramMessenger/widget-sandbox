// The remote engine over one byte stream per engine process, the byte-level twin of the loopback (remote_engine.h): the stream a host supplies, the frames on it, the host's end with a channel per runtime and the engine process's end; a hosting stream carries hosted widgets (remote_hosted.h) the same way.
#pragma once

#include "twidget/js_thread.h"
#include "twidget/remote_engine.h"
#include "twidget/remote_hosted.h"
#include "twidget/resources.h"
#include "twidget/sandbox_engine.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace twidget {

/// One duplex byte stream to an engine process, the OS boundary a host supplies (a POSIX socket, a Windows pipe): one reader and one writer at a time, each of which may block until `shutdown`; the owner destroys it only after both returned.
class SandboxRemoteStream {
public:
    virtual ~SandboxRemoteStream() = default;
    /// Blocks until at least one byte came, then copies up to `size` bytes to `out` and returns how many; 0 at the end of the stream, on an error and after `shutdown`.
    virtual size_t read(char* out, size_t size) = 0;
    /// Blocks until all of `bytes` is written: true, or false once the stream broke or was shut down, with how much of it went unknown.
    virtual bool write(std::string_view bytes) = 0;
    /// From any thread, also while a `read` or `write` blocks, idempotent and without waiting: a blocked `read` and `write` return, and every later one fails.
    virtual void shutdown() = 0;
};

/// The frames on a stream, all integers little-endian: a `u32` size of the bytes after it (at least 5), a `u32` runtime id (0 is the connection), a `u8` kind, then the kind's fields in order and nothing after them. A `str` is a `u32` length and that many raw bytes, an `i64` 8 bytes of two's complement, an `f64` the 8 bytes of its IEEE-754 pattern, a `bool` one byte of 0 or 1; the strings are the string protocol's (the `__host` JSON, `SandboxRemoteCoding`'s forms).
struct SandboxRemoteWire {
    /// The stream's version, in `Opening` and `Ready`.
    static constexpr int64_t kVersion = 1;
    /// The size any frame but an opening (`Opening`, `HostingOpening`) may give, in both directions: an ops batch with room for its frame.
    static constexpr size_t kMaxFrameBytes = SandboxEngine::kRemoteOpsLimit + (size_t(64) << 10);
    /// The size `Opening` and `HostingOpening` may give: the sandbox bundle, a hosting stream's validator bundle too.
    static constexpr size_t kMaxOpeningBytes = size_t(16) << 20;
    /// Bytes of a stamp line in `Opening` and `HostingOpening` (`BundleStamp`): a longer one is refused, never cut.
    static constexpr size_t kMaxStampBytes = 4096;

    /// Host to engine from 1, engine to host from 64, a hosting stream's own kinds from 32 and 96: a kind of the other direction is refused as the other side's. Each kind's fields in order.
    enum class Kind : uint8_t {
        /// On the connection, first: i64 version, str build, str sandboxBundle, str validatorStamp, str rendererStamp (each stamp the bundle's first line, "" for no renderer bundle).
        Opening = 1,
        /// A new runtime, its id above every earlier one's: no fields. The opening decides what it is, an engine after `Opening`, a hosted widget after `HostingOpening`.
        Open = 2,
        /// str html, str config (`SandboxRemoteCoding::encodeConfig`).
        Start = 3,
        /// str json.
        Event = 4,
        /// str json, i64 gesture (within `int`).
        Activation = 5,
        /// f64 time.
        AnimationFrame = 6,
        /// f64 innerWidth, f64 innerHeight, f64 dpr.
        Viewport = 7,
        /// str entriesJson.
        Resize = 8,
        /// str colorScheme, str params (`SandboxRemoteCoding::encodeThemeParams`).
        Theme = 9,
        /// str area (`SandboxRemoteCoding::encodeSafeArea`).
        SafeArea = 10,
        /// str data (`SandboxRemoteCoding::encodeData`).
        Data = 11,
        /// bool visible.
        Visible = 12,
        /// No fields.
        PressBegan = 13,
        /// No fields.
        PressEnded = 14,
        /// f64 ms.
        ChargeValidator = 15,
        /// str reason.
        Stop = 16,
        /// i64 id, str outcome (`SandboxRemoteCoding::encodeOutcome`).
        Answer = 17,
        /// i64 seq (from 1), str code.
        Probe = 18,
        /// str json, bool hasBinary, str binary ("" unless hasBinary).
        QueryAnswer = 19,
        /// str text.
        IntlAnswer = 20,
        /// The runtime closed: no fields.
        Close = 21,
        /// On the connection, first, opening a hosting stream (its runtimes are hosted widgets): i64 version, str build, str sandboxBundle, str validatorBundle, str rendererStamp (the renderer bundle's first line, "" for none).
        HostingOpening = 32,
        /// str html, str config (`SandboxRemoteCoding::encodeConfig`), str hosted (`SandboxRemoteCoding::encodeHostedStart`).
        HostedStart = 33,
        /// str theme (`SandboxRemoteCoding::encodeTheme`).
        HostedTheme = 34,
        /// str message, for the widget's renderer (`SandboxHostedPlatform::onMessage`).
        Platform = 35,
        /// str answer, to the runtime's `PlatformRequest`.
        PlatformAnswer = 36,
        /// On the connection, the answer to `Opening`: i64 version.
        Ready = 64,
        /// On the connection, the answer to `Opening`: str reason.
        Refused = 65,
        /// No fields.
        TaskStarted = 66,
        /// No fields.
        TaskCompleted = 67,
        /// str json.
        Ops = 68,
        /// str json; synchronous, answered by `QueryAnswer`.
        Query = 69,
        /// str requestJson; synchronous, answered by `IntlAnswer`.
        Intl = 70,
        /// str json.
        Listen = 71,
        /// str json.
        PreventDefault = 72,
        /// str json.
        Anim = 73,
        /// No fields.
        RequestsFrame = 74,
        /// str level, str text.
        Log = 75,
        /// str kind, str detail.
        Diag = 76,
        /// str method, str argsJson.
        Bridge = 77,
        /// str text.
        CopyText = 78,
        /// i64 id, str data, bool wallet.
        Request = 79,
        /// i64 id, str payload (`SandboxRemoteCoding::encodeWallet`).
        Wallet = 80,
        /// str commitJson.
        Storage = 81,
        /// str edge.
        ResizeAnchor = 82,
        /// bool on.
        WatchesPublicData = 83,
        /// No fields.
        Loaded = 84,
        /// 11 f64 in `SandboxRemoteCoding::encodeStats`' order.
        Stats = 85,
        /// str reason.
        Died = 86,
        /// i64 seq (from 1), str text.
        ProbeAnswer = 87,
        /// str message of the widget's renderer for the host's platform (`SandboxHostedPlatform::send`).
        PlatformMessage = 96,
        /// str message; synchronous, answered by `PlatformAnswer` (`SandboxHostedPlatform::request`).
        PlatformRequest = 97,
        /// str json, a batch the widget's validator passed, only after a `HostedStart` that asked for them.
        ValidatedOps = 98,
        /// The widget's renderer announced itself: no fields.
        RendererReady = 99,
        /// The loaded widget's first render: no fields.
        Rendered = 100,
        /// i64 nodes after a validated batch.
        Nodes = 101,
    };
};

/// How a hosting stream's owner reads its hosted widgets' opaque platform messages (`SandboxRemoteHostEnd::startHosting`), so the stream knows, per hosted route, which of the host's messages its renderer has yet to answer and for how long of the renderer's own time. Without `read` (the default, and every engine stream) the stream keeps nothing of the kind. With it, a host's message (`Platform`) read as `Expects` is kept from the moment its writer admitted it, which is when its time starts, until the renderer's first message (`PlatformMessage`) read as `Answers` with the same key while it is the route's oldest kept message; an answer with another key (another family, a missing, stale or repeated sequence) settles nothing, an answer on another route nothing of this one. A renderer's message read as `Last`, or its route's `Died`, settles every message kept for the route and nothing is kept for it again; so do the host's keep of the renderer alone (`SandboxHostedChannel::kept`), its `close` and the route's loss. A renderer's message is read only once its route's `HostedStart` was admitted (the engine process builds the renderer there): one that comes before it settles and halts nothing, and the host's messages sent before it are kept as any others. A message read `whenVisible` has its time counted only when it was sent while its route was visible: the `HostedStart`'s config, unless a `Visible` was admitted before it, then each `Visible` in the order the writer admitted them with the platform messages; one sent before either is judged by the `HostedStart`'s config; a later change of visibility neither starts nor stops the time of a message sent before it, and a message whose time is not counted is still kept, in its place. The host's own time is never the renderer's: from the arrival of a route's `PlatformRequest` until the writer handed its `PlatformAnswer` whole to the stream, or the renderer's next request showed that it arrived, no message of that route counts time, the ones sent meanwhile included, while every other route's time goes on; a request its client answers with none (`SandboxRemoteSink::remotePlatformRequest`'s nullopt), which no answer will ever end, loses its route if it is still live, as a host's form that does not decode does: its client hears that the stream is lost, after any death of its own it reported first, its `Close` goes out, and the stream and the other routes go on. A route with `SandboxEngine::kRemoteQueuedCalls` messages kept ends the stream at the next message that expects an answer, which is not sent, as a queue past its bounds does. Per message the stream keeps its key and two times, never the message. A timed message whose answer time (`SandboxEngine::kPlatformAnswerSeconds` of its renderer's own time, the host's left out) runs out ends the stream ("a hosted renderer left a platform message unanswered for more than 3000 ms"): `widget.monitor` sleeps until the earliest one, woken only for an earlier one, and a look more than a second after the wake it planned (a machine that slept, which the stream's steady clock counts, or a thread that did not run) concludes nothing and asks for a second look a second later; no look concludes before that one, and that one, late or not, judges each message with all the time it had by then and is spent, never asked for again in its place (`SandboxRemoteHostEnd::lookAtAnswers`, the monitor's and the reader's one rule); a frame that would settle a message whose time ran out (its answer, the renderer's last word, its engine's `Died`) ends the stream first, when a look at that moment would conclude. The stream's first end decides once, before any client hears it, what each open route's death is to its client (`SandboxHostedChannel::Death`, which the embedder reads through `SandboxHostedChannel::death`): at a run-out answer time the routes whose time ran out by then are `Stalled` and the others `Spared`; at any other end but the owner's `stop` (the engine process's end of the stream, a read, write or frame the stream cannot go on with, a write past its bound, a queue past its bounds) the routes with a message kept, timed or not, keep an `Ordinary` death and the others are `Spared`, or every one keeps it when none had one. A route kept, closed or lost alone, whose renderer halted, whose owner withdrew it first (`SandboxHostedChannel::withdraw`) or whose client or host claimed a death of the widget's own first (a flood, a platform request left unanswered, the host's own stop of the widget, `SandboxHostedChannel::localStop`, which its live platform end's refusal and its client's counted stop make: from that claim, in one step with the end, nothing of the route is kept, timed or blamed, and nothing it owed is credited, so that dead widget alone never ends the stream) takes no part and keeps its own death, which an end that finds the claimed route still open, neither kept nor halted, settles `Ordinary`; a local stop after the end leaves the end's decision as it is, and a counted stop then yields to that end; the owner's `stop` decides nothing; a hosted widget that attaches to a stream that ended otherwise is `Spared`. Each death the end decided settles once, as the end decided it when the client's first death is that end or cleanup takes the route from its client (a keep, a close) before it, `Ordinary` when a death of the widget's own came first (a loss alone leaves the choice to its client, which still hears it), and the owner's withdrawal that found it decided hears it then (`SandboxHostedChannel::Withdrawal`).
struct SandboxHostedAnswers {
    /// What one platform message is to the stream.
    enum class Role : uint8_t {
        /// Nothing the stream keeps an account of (a size, a theme, a report, a request of the renderer's).
        None,
        /// A host's message that its renderer answers with exactly one message of the same key, in the order the host sent them.
        Expects,
        /// A renderer's message that answers its route's oldest kept message of the same key.
        Answers,
        /// A renderer's last word: it answers nothing more (it stopped).
        Last,
    };
    /// One message as its owner reads it: its role and, for `Expects` and `Answers`, its key, a family of the owner's and, for a family that numbers its messages, their sequence; an expected message `whenVisible` counts time only when sent while visible (a frame request), any other one always (an input event).
    struct Reading {
        Role role = Role::None;
        uint32_t family = 0;
        std::optional<uint64_t> sequence;
        bool whenVisible = false;
    };
    /// Reads one message, a host's or a renderer's (a role of the other direction counts as `None`, as does a reading that throws), never one past `SandboxEngine::kRemotePlatformLimit`: pure and quick, called on the sending thread (a hosted client's `widget.client`) and on `widget.reader`, never under a lock of the stream.
    std::function<Reading(std::string_view message)> read;
};

/// The host's end of one stream: it opens the stream with the sandbox bundle, hands out a channel per runtime for `SandboxEngine::setRemoteChannelFactory` (or, opened for hosted widgets, per hosted widget) and gives each frame of the engine process to its runtime's sink. A frame it cannot accept ends the whole stream, and every open runtime's client hears `remoteDisconnected` once: on an engine stream any frame of a runtime but a probe's answer while its query or Intl request is in the host's hands, and a hosted runtime's kinds; on a hosting stream a runtime's render frames (`Ops`, `Query`, `Listen`, `PreventDefault`, `Anim`, `RequestsFrame`), a second Intl request or a second platform request while the first is in the host's hands (its other frames are delivered meanwhile, as its engine, glue and renderer send on their own). Its threads: `widget.reader` reads, `widget.writer` alone blocks in writes, `widget.monitor` bounds those writes and the hosted renderers' answer times (SandboxHostedAnswers) and reports to the owner, and each channel's `widget.answer` answers its runtime's queries, Intl requests and a hosted renderer's platform requests through the client, one at a time in the order they came. A runtime whose `widget.answer` cannot start is lost alone: its client hears why (the thread's refusal) as its death, then `remoteDisconnected`, its `Close` goes out, and the stream and the other runtimes go on.
class SandboxRemoteHostEnd {
public:
    /// What the owner hears, from `widget.monitor` and outside every lock of this end; an empty one is skipped. Each returns at once and posts what it does, never stopping, destroying or calling into this end.
    struct Events {
        /// How many runtimes are open now, a hosted route whose renderer its client kept (`SandboxHostedChannel::kept`) not among them: changes close together may come as their last count, and a stream that ends with runtimes open reports 0 before `ended`. The stream's lifetime stays the owner's: kept routes neither hold it open nor end it.
        std::function<void(size_t open)> runtimes;
        /// Once and last: why the stream ended.
        std::function<void(const std::string& reason)> ended;
    };
    /// The bounds of one stream, each positive and finite; another value is its default.
    struct Limits {
        /// How long a channel's `probe` waits for its answer ("ERROR timeout" past it); the client's barriers around the call wait for its dispatch thread as in process.
        double probeWaitMs = 10000;
        /// How long one write to the stream (at most 64 KiB of a frame) may block before the stream ends, without another call needed.
        double writeStallMs = 10000;
        /// Bytes of frames kept to write, the one being written included; past them, or past `SandboxEngine::kRemoteQueuedCalls` frames, the stream ends.
        size_t queuedBytes = size_t(64) << 20;
    };

    /// Starts this end's threads on `stream` and queues the opening: `resources`' sandbox bundle, and only the stamp lines of its validator and renderer bundles, which stay in the host. `build` is the host's build, which the engine process's end must share. Throws std::system_error when one of its threads cannot start (JSThread): those already started are joined, nothing was written to the stream and `Events` hear nothing.
    static std::shared_ptr<SandboxRemoteHostEnd> start(std::shared_ptr<SandboxRemoteStream> stream, const SandboxResources& resources, std::string build, Events events, Limits limits);
    /// `start` for hosted widgets (remote_hosted.h), served by `SandboxRemoteEngineEnd::serveHosting`: the opening (`HostingOpening`) carries `resources`' sandbox and validator bundles and only the stamp line of its renderer bundle, every runtime is a hosted widget on `hostedChannel`, and `channel` refuses. With `answers` the stream keeps what each hosted renderer has yet to answer, bounds its answer time and decides at its end what each hosted widget's death is (SandboxHostedAnswers); without them, nothing. Throws as `start` does.
    static std::shared_ptr<SandboxRemoteHostEnd> startHosting(std::shared_ptr<SandboxRemoteStream> stream, const SandboxResources& resources, std::string build, Events events, Limits limits,
                                                              SandboxHostedAnswers answers = {});
    /// `stop`, on the owner's thread: the last reference is never dropped on a thread of this end's (inside `Events`).
    ~SandboxRemoteHostEnd();
    SandboxRemoteHostEnd(const SandboxRemoteHostEnd&) = delete;
    SandboxRemoteHostEnd& operator=(const SandboxRemoteHostEnd&) = delete;

    /// A channel to a new runtime on this stream, which runs the opening's bundles whatever resources the factory was given; from any thread, without waiting. Once the stream ended, one whose `attach` reports `remoteDisconnected`; on an end `startHosting` opened, `refusing("the stream was opened for hosted widgets")`.
    std::shared_ptr<SandboxRemoteChannel> channel();
    /// A hosted widget's channel to a new runtime on a stream `startHosting` opened (WidgetHost's hosted constructor, `SandboxEngine::hostedClient`), as `channel` gives one; on an end `start` opened, `refusingHosted("the stream was opened for engines only")`.
    std::shared_ptr<SandboxHostedChannel> hostedChannel();
    /// Ends the stream at once (what is queued is dropped, and the engine process may see a cut frame), joins `widget.reader`, `widget.writer` and `widget.monitor`, then lets the stream go; `ended` came before it returns. Idempotent, on the owner's thread. A channel joins its `widget.answer` when it is destroyed; no delegate or provider code that already runs is cut short. When the stream's first end, the one `ended` reports, was no `stop` (the engine process's end of it, a refusal before `Ready` among them, a read, write or frame the stream cannot go on with, a write past its bound, a queue past its bounds, an answer time that ran out), the instant of that end on `std::chrono::steady_clock` (for an answer time that ran out, the instant a look judged it run out, at which that end decided the hosted widgets' deaths), taken once in the same step as that end, under the stream's lock: also while `ended` has yet to report that end, and the same at every later call. None when this call ended the stream, which decides nothing of the hosted widgets' deaths (SandboxHostedAnswers), or an earlier `stop` did; the destructor's own call reads nothing.
    std::optional<std::chrono::steady_clock::time_point> stop();

    /// A channel for a host that has no engine process: `attach` reports `remoteDied(reason)` from inside, `probe` answers "ERROR no runtime" and every other call does nothing.
    static std::shared_ptr<SandboxRemoteChannel> refusing(std::string reason);
    /// `refusing` for a hosted client: its hosted calls do nothing too.
    static std::shared_ptr<SandboxHostedChannel> refusingHosted(std::string reason);

    /// What one look at the hosted renderers' answer times concludes (`lookAtAnswers`).
    struct AnswerLook {
        /// An answer time ran out by the look: the stream ends.
        bool expired = false;
        /// The second look asked for after this one: none, the one asked for before (still ahead), or a new one `kLateLook` from the look.
        std::optional<std::chrono::steady_clock::time_point> recheck;
    };
    /// How long after the wake the monitor planned a look at the answer times may come and still conclude (SandboxHostedAnswers).
    static constexpr std::chrono::seconds kLateLook{1};
    /// The stream's one rule for a look at the answer times at `now` (SandboxHostedAnswers), pure, so its tests give it their own times: `due` is when the earliest answer time looked at runs out (none: none runs), `wake` the wake the monitor planned (none: it waits until woken, or it is running), `recheck` the second look a late one asked for. A second look asked for decides first: before it nothing is concluded and it stays; at it or after it, however late that look is, `due` is judged with all the time it had (at `now` or before, the stream ends) and the second look is spent. Without one, a look more than `kLateLook` after `wake` concludes nothing and asks for one `kLateLook` from `now`; any other judges `due`. `widget.monitor` takes all of it after each wake, with the earliest answer time of every route; `widget.reader`, before a frame settles a message whose time ran out, only `expired`, with that route's, leaving the second look to the monitor.
    static AnswerLook lookAtAnswers(std::optional<std::chrono::steady_clock::time_point> due, std::optional<std::chrono::steady_clock::time_point> wake,
                                    std::optional<std::chrono::steady_clock::time_point> recheck, std::chrono::steady_clock::time_point now);

private:
    struct Connection;
    explicit SandboxRemoteHostEnd(std::shared_ptr<Connection> connection);

    const std::shared_ptr<Connection> connection_;
    std::unique_ptr<JSThread> reader_;
    std::unique_ptr<JSThread> writer_;
    std::unique_ptr<JSThread> monitor_;
};

/// The engine process's end of one stream: no Qt, no file read, so a host may run it before Qt starts.
struct SandboxRemoteEngineEnd {
    /// Serves `stream` from the calling thread, its reader: checks the opening (a malformed one, another version or another build than `build` is refused, a `HostingOpening` too), builds the runtimes' resources from its bundles, keeps a `SandboxRemoteServer` per runtime id on its own `widget.sandbox` thread and writes their frames under one lock; a runtime's probes run on its `widget.probe` thread and closed runtimes are destroyed on `widget.reaper`. A runtime whose `widget.sandbox` cannot start dies with why (`SandboxRemoteServer::start`), and one whose `widget.probe` cannot start is stopped with why as `Stop` stops it, that probe answered "ERROR no runtime"; the stream and the other runtimes go on. A `widget.reaper` that cannot start ends serving with why. At the end every runtime still open is closed before any thread is joined, then destroyed on this thread, so ending needs no thread. Returns why the stream ended, "" at its end on a frame's boundary, once every runtime and thread it started is gone; a `shutdown` of `stream` from another thread ends it. A set `lockdown` is called once on this thread after the opening is accepted and the resources are built, before `Ready` and before any runtime exists (never for a refused opening or a stream that ends before one): "" goes on serving, any other text refuses the stream with it, and an exception refuses it with "the engine process could not lock itself down".
    static std::string serve(SandboxRemoteStream& stream, const std::string& build, const std::function<std::string()>& lockdown = {});
    /// `serve`, also for a hosting stream: a first `HostingOpening` is checked as `Opening` is (version, build, the renderer's stamp) and gives the runtimes' resources its sandbox and validator bundles, no file read, with `lockdown` called at the same point; every runtime of such a stream is a hosted widget, whose engine (on `widget.sandbox`), `TrustedValidator` (on `widget.validator`), glue (on `twidget-host`) and renderer from `renderers` are built at its `HostedStart` on this thread, which the factory must never make wait for the host (no `SandboxHostedPlatform::request` inside it). Its probe and reaper threads, and what their refusals do, are `serve`'s: a hosted widget stopped because its `widget.probe` cannot start keeps its renderer until `Close`, as at any death of its engine. On a hosting stream the engine's host kinds (`Start`, `Event`, `Activation`, `AnimationFrame`, `Viewport`, `Resize`, `Theme`, `PressBegan`, `PressEnded`, `ChargeValidator`, `QueryAnswer`) end serving, as the hosted kinds do on an engine stream. Defined in remote_hosted.cpp, so a helper that calls only `serve` links no WidgetHost or validator.
    static std::string serveHosting(SandboxRemoteStream& stream, const std::string& build, const std::function<std::string()>& lockdown, HostedRendererFactory renderers);
};

} // namespace twidget
