// The host's end of a stream (twidget/remote_stream.h, `SandboxRemoteHostEnd`): its reader, writer and monitor, a channel per runtime and the refusing channel. The engine process's end is remote_stream_engine.cpp's, so a host's program links neither the engine's server nor an engine through this one.
#include "twidget/remote_stream.h"

#include "remote_wire.h"
#include "twidget/json.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace twidget {

// The wire both ends share (remote_wire.h).
using detail::Codec;
using detail::cut;
using detail::Field;
using detail::Frame;
using detail::FrameReader;
using detail::Kind;
using detail::kindOn;
using detail::Side;

namespace {

constexpr const char* kNoRuntime = "ERROR no runtime";
constexpr const char* kProbeTimeout = "ERROR timeout";
constexpr const char* kHostStopped = "the host stopped the stream";
constexpr const char* kEngineClosed = "the engine process closed the stream";
constexpr const char* kWriteFailed = "a write to the stream failed";
/// The death of a runtime whose `widget.answer` cannot start, before the refusal itself (which names the thread and the OS error).
constexpr const char* kAnswerUnstarted = "the host could not start the thread that answers the widget";
/// Bytes of one write to the stream: `Limits::writeStallMs` bounds each such write.
constexpr size_t kWriteChunk = size_t(64) << 10;
/// The longest bound `Limits` takes, a year: its duration still fits the clock's.
constexpr double kLongestBoundMs = 365.0 * 24 * 3600 * 1000;
/// The stamp of a bundle whose first line is empty: no `BundleStamp` reads it, so its runtimes are refused at boot as in process, where "" would mean no renderer.
constexpr const char* kNoStamp = "/* no stamp */";

using Clock = std::chrono::steady_clock;

/// What the opening carries of the validator or renderer bundle: its first line, all of it without a newline, never cut (the engine's end refuses one past `kMaxStampBytes`).
std::string_view stampOf(const std::string& bundle) {
    const std::string_view line = std::string_view(bundle).substr(0, bundle.find('\n'));
    return line.empty() && !bundle.empty() ? std::string_view(kNoStamp) : line;
}

/// A bound of `Limits` in ms: `fallback` for one that is not positive and finite.
double boundMs(double ms, double fallback) { return std::isfinite(ms) && ms > 0 ? std::min(ms, kLongestBoundMs) : fallback; }

Clock::duration durationOf(double ms) { return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms)); }

/// The synchronous sink call a runtime's `widget.answer` makes: an engine's query or Intl request, or a hosted renderer's platform request.
enum class Pending : uint8_t { None, Query, Intl, Platform };

/// The engine kinds of the render glue, which a hosted runtime keeps inside the engine process.
bool renderKind(Kind kind) {
    switch (kind) {
    case Kind::Ops:
    case Kind::Query:
    case Kind::Listen:
    case Kind::PreventDefault:
    case Kind::Anim:
    case Kind::RequestsFrame: return true;
    default: return false;
    }
}

/// The engine kinds only a hosted runtime sends: its renderer's platform traffic, the batches its host asked for and its glue's reports.
bool hostedReport(Kind kind) {
    switch (kind) {
    case Kind::PlatformMessage:
    case Kind::PlatformRequest:
    case Kind::ValidatedOps:
    case Kind::RendererReady:
    case Kind::Rendered:
    case Kind::Nodes: return true;
    default: return false;
    }
}

/// One runtime on the host's side, made with its channel: the connection's mutex guards every field but the answer thread's three, which `answering` guards.
struct HostRoute {
    /// New until `attach`, Live while its id is registered, Leaving while its client hears that the stream is lost, then Gone.
    enum class State : uint8_t { New, Live, Leaving, Gone };

    State state = State::New;
    uint32_t id = 0;
    std::shared_ptr<SandboxRemoteSink> sink;
    /// A query or an Intl request in the host's hands, until its answer is queued: an engine runtime may send nothing but a probe's answer meanwhile, a hosted one no second Intl request.
    Pending pending = Pending::None;
    /// A hosted renderer's platform request in the host's hands, until its answer is queued or the client gave none: a second one meanwhile ends the stream.
    bool platformPending = false;
    /// Its client kept the renderer alone (`SandboxHostedChannel::kept`): not one of the open runtimes `Events::runtimes` counts, its frames still taken by the stream's rules.
    bool kept = false;
    /// The last probe sequence sent; the one `probe` waits for (0: none) and its answer.
    int64_t lastProbe = 0;
    int64_t waiting = 0;
    std::optional<std::string> probed;
    /// That answer when it came while `pending` or `platformPending`: published once neither is and the sinks' answers were queued, dropped when one cannot be.
    std::optional<std::string> deferred;
    std::condition_variable settled;

    std::mutex answering;
    /// Made at the runtime's first synchronous call; the channel's destructor takes and joins it, after which nothing is posted.
    std::unique_ptr<JSThread> answerer;
    bool retired = false;
};

/// `SandboxRemoteHostEnd::refusing`'s and `refusingHosted`'s channel: its client dies with the reason at `attach`, and no call reaches an engine.
class RefusingChannel final : public SandboxHostedChannel {
public:
    explicit RefusingChannel(std::string reason) : reason_(std::move(reason)) {}

    void attach(std::shared_ptr<SandboxRemoteSink> sink) override {
        if (sink) sink->remoteDied(reason_);
    }
    void startHosted(const std::string&, const SandboxConfig&, const SandboxHostedStart&) override {}
    void hostedTheme(const WidgetTheme&) override {}
    void platform(const std::string&) override {}
    void start(const std::string&, const SandboxConfig&) override {}
    void event(const std::string&) override {}
    void activationEvent(const std::string&, int) override {}
    void frame(double) override {}
    void viewport(double, double, double) override {}
    void resize(const std::string&) override {}
    void theme(const std::string&, const std::map<std::string, std::string>&) override {}
    void contentSafeArea(const WidgetContentSafeArea&) override {}
    void setData(const WidgetData&) override {}
    void setVisible(bool) override {}
    void pressBegan() override {}
    void pressEnded() override {}
    void chargeValidator(double) override {}
    void stop(const std::string&) override {}
    void answer(int64_t, const SandboxRemoteOutcome&) override {}
    std::string probe(const std::string&) override { return kNoRuntime; }
    void close() override {}

private:
    const std::string reason_;
};

} // namespace

/// What the reader, the writer, the monitor and every channel of one stream share, under one mutex; it owns no thread (`SandboxRemoteHostEnd` owns the three, each channel its `widget.answer`). The `...Locked` members need the mutex held, the others take it.
struct SandboxRemoteHostEnd::Connection : std::enable_shared_from_this<SandboxRemoteHostEnd::Connection> {
    class Channel;

    /// `hosting`: opened for hosted widgets (`startHosting`), every runtime of the stream a hosted one.
    Connection(std::shared_ptr<SandboxRemoteStream> stream, Events events, const Limits& limits, bool hosting)
        : stream_(*stream), events_(std::move(events)), probeWait_(durationOf(boundMs(limits.probeWaitMs, Limits{}.probeWaitMs))),
          stallMs_(boundMs(limits.writeStallMs, Limits{}.writeStallMs)), stall_(durationOf(stallMs_)), queueBytes_(limits.queuedBytes ? limits.queuedBytes : Limits{}.queuedBytes),
          hosting_(hosting), held_(std::move(stream)) {}

    bool hosting() const { return hosting_; }

    /// `widget.reader`'s loop: each frame checked, then given to its runtime in order, until the stream ends; then every open runtime's client hears it.
    void read() {
        FrameReader reader(stream_, Side::Engine);
        std::string why;
        while (why.empty()) {
            if (reader.next(SandboxRemoteWire::kMaxFrameBytes)) why = accept(reader.frame());
            else why = reader.failure().empty() ? kEngineClosed : reader.failure();
        }
        finish(why);
    }

    /// `widget.writer`'s loop: the queue in order, each frame in writes of `kWriteChunk` bytes, counted until it is written; at the end what is queued is dropped.
    void write() {
        for (;;) {
            std::string frame;
            {
                std::unique_lock<std::mutex> l(mutex_);
                work_.wait(l, [&] { return ending_ || !queue_.empty(); });
                if (ending_) break;
                frame = std::move(queue_.front());
                queue_.pop_front();
            }
            bool written = true;
            for (size_t at = 0; written && at < frame.size(); at += kWriteChunk) {
                {
                    std::lock_guard<std::mutex> l(mutex_);
                    if (ending_) break;
                    writing_ = true;
                    writingSince_ = Clock::now();
                    if (!timing_) watched_.notify_all();
                }
                written = stream_.write(std::string_view(frame).substr(at, kWriteChunk));
                std::lock_guard<std::mutex> l(mutex_);
                writing_ = false;
                if (!written) endLocked(kWriteFailed);
            }
            std::lock_guard<std::mutex> l(mutex_);
            queuedBytes_ -= frame.size();
            queuedFrames_--;
        }
        std::deque<std::string> dropped;
        std::lock_guard<std::mutex> l(mutex_);
        dropped.swap(queue_);
        queuedBytes_ = 0;
        queuedFrames_ = 0;
    }

    /// `widget.monitor`'s loop: ends the stream when one write outlasts its bound, with no other call needed, and alone reports to the owner outside the lock: the open count (kept routes left out) as it changes, then the end once and last.
    void watch() {
        size_t reported = 0;
        std::unique_lock<std::mutex> l(mutex_);
        for (;;) {
            if (writing_ && !ending_ && Clock::now() - writingSince_ >= stall_) {
                endLocked("a write to the stream blocked for more than " + Json::number(stallMs_) + " ms");
                continue;
            }
            if (const size_t live = liveLocked(); live != reported) {
                reported = live;
                l.unlock();
                if (events_.runtimes) events_.runtimes(reported);
                l.lock();
                continue;
            }
            if (finalized_) {
                const std::string why = reason_;
                l.unlock();
                if (events_.ended) events_.ended(why);
                return;
            }
            timing_ = writing_ && !ending_;
            if (timing_) watched_.wait_until(l, writingSince_ + stall_);
            else watched_.wait(l);
            timing_ = false;
        }
    }

    /// The opening, the first frame queued: the sandbox bundle and its bytecode, and only the other bundles' stamps; a hosting stream's (`HostingOpening`) the validator bundle and its bytecode too, the renderer's stamp alone.
    void open(const SandboxResources& r, const std::string& build) {
        const auto bytes = [](const std::vector<uint8_t>& code) { return std::string_view(reinterpret_cast<const char*>(code.data()), code.size()); };
        constexpr int64_t version = SandboxRemoteWire::kVersion;
        constexpr size_t bound = SandboxRemoteWire::kMaxOpeningBytes;
        std::optional<std::string> opening =
            hosting_ ? Codec::encode(0, Kind::HostingOpening, bound, {version, build, r.sandboxBundle, bytes(r.sandboxBytecode), r.validatorBundle, bytes(r.validatorBytecode), stampOf(r.rendererBundle)})
                     : Codec::encode(0, Kind::Opening, bound, {version, build, r.sandboxBundle, bytes(r.sandboxBytecode), stampOf(r.validatorBundle), stampOf(r.rendererBundle)});
        std::lock_guard<std::mutex> l(mutex_);
        if (opening) queueLocked(std::move(*opening));
        else endLocked("an opening past the bound of " + std::to_string(SandboxRemoteWire::kMaxOpeningBytes) + " bytes");
    }

    void end(const std::string& why) {
        std::lock_guard<std::mutex> l(mutex_);
        endLocked(why);
    }

    /// After the threads joined: the stream goes, outside the lock.
    void release() {
        std::shared_ptr<SandboxRemoteStream> held;
        std::lock_guard<std::mutex> l(mutex_);
        held = std::move(held_);
    }

    /// `route`'s id and its `Open` in one step, so the engine's end sees the ids rise in the order they were given; without them (the stream ended, every id given) its client hears at once that the stream is lost.
    void attach(const std::shared_ptr<HostRoute>& route, std::shared_ptr<SandboxRemoteSink> sink) {
        if (!sink) return;
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (route->state != HostRoute::State::New) return;
            route->sink = sink;
            if (lastId_ < UINT32_MAX) {
                std::optional<std::string> open = Codec::encode(lastId_ + 1, Kind::Open, SandboxRemoteWire::kMaxFrameBytes, {});
                if (open && queueLocked(std::move(*open))) {
                    route->id = ++lastId_;
                    route->state = HostRoute::State::Live;
                    routes_.emplace(route->id, route);
                    watched_.notify_all();
                    return;
                }
            }
            route->state = HostRoute::State::Gone;
        }
        sink->remoteDisconnected();
    }

    /// `route`'s id while a frame of it may still go out.
    std::optional<uint32_t> liveId(HostRoute& route) {
        std::lock_guard<std::mutex> l(mutex_);
        if (route.state != HostRoute::State::Live || ending_) return std::nullopt;
        return route.id;
    }

    /// A frame of `route`'s encoded outside the lock, queued only while the route and the stream still are.
    void admit(HostRoute& route, std::string frame) {
        std::lock_guard<std::mutex> l(mutex_);
        if (route.state == HostRoute::State::Live) queueLocked(std::move(frame));
    }

    /// The channel's `kept`: a live route counts no more among the open runtimes, and the monitor reports the new count.
    void keep(HostRoute& route) {
        std::lock_guard<std::mutex> l(mutex_);
        if (route.state != HostRoute::State::Live || std::exchange(route.kept, true)) return;
        watched_.notify_all();
    }

    /// The channel's `close`: the route goes with its `Close` and a waiting probe wakes; its client, which closes, hears nothing.
    void close(HostRoute& route) {
        std::lock_guard<std::mutex> l(mutex_);
        switch (route.state) {
        case HostRoute::State::New: route.state = HostRoute::State::Gone; break;
        case HostRoute::State::Live:
            removeLocked(route);
            route.state = HostRoute::State::Gone;
            route.settled.notify_all();
            break;
        default: break;
        }
    }

    /// A form of the host's that does not decode or fit a frame, or a runtime whose `widget.answer` cannot start: the route goes as `close` lets it go, and its client hears that the stream is lost, as over the loopback, after `died` as its runtime's death when one is given.
    void lose(const std::shared_ptr<HostRoute>& route, const std::optional<std::string>& died = std::nullopt) {
        std::shared_ptr<SandboxRemoteSink> sink;
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (route->state != HostRoute::State::Live) return;
            removeLocked(*route);
            sink = route->sink;
        }
        if (died) sink->remoteDied(*died);
        sink->remoteDisconnected();
        std::lock_guard<std::mutex> l(mutex_);
        route->state = HostRoute::State::Gone;
        route->settled.notify_all();
    }

    /// The channel's `probe`: the next sequence's `Probe`, then its answer within the bound ("ERROR timeout" past it), "ERROR no runtime" once the route closed or the stream ended and its client heard it.
    std::string probe(const std::shared_ptr<HostRoute>& route, const std::string& code) {
        uint32_t id = 0;
        int64_t seq = 0;
        {
            std::unique_lock<std::mutex> l(mutex_);
            if (route->state != HostRoute::State::Live || ending_) return settledLocked(l, *route);
            // Sequences never wrap: past the last one the runtime answers no probe.
            if (route->lastProbe == std::numeric_limits<int64_t>::max()) return kNoRuntime;
            id = route->id;
            seq = ++route->lastProbe;
        }
        std::optional<std::string> frame = Codec::encode(id, Kind::Probe, SandboxRemoteWire::kMaxFrameBytes, {seq, code});
        if (!frame) {
            lose(route);
            return kNoRuntime;
        }
        std::unique_lock<std::mutex> l(mutex_);
        if (route->state != HostRoute::State::Live || !queueLocked(std::move(*frame))) return settledLocked(l, *route);
        route->waiting = seq;
        route->settled.wait_for(l, probeWait_, [&] { return route->probed || route->state == HostRoute::State::Gone; });
        std::string answer = route->probed ? std::move(*route->probed) : route->state == HostRoute::State::Live && !ending_ ? kProbeTimeout : kNoRuntime;
        route->waiting = 0;
        route->probed.reset();
        route->deferred.reset();
        return answer;
    }

private:
    /// One checked frame of the engine's, in its order: why the stream cannot go on, "" when it was given to its runtime or dropped (its runtime closed); a kind of the other mode ends the stream whatever its runtime.
    std::string accept(const Frame& f) {
        // What a synchronous call or a probe keeps is copied before the lock, cut as the engine's end cuts it.
        std::string kept;
        if (f.kind == Kind::Query || f.kind == Kind::Intl || f.kind == Kind::ProbeAnswer) kept = std::string(cut(f.str[0], SandboxEngine::kRemoteMessageLimit));
        if (f.kind == Kind::PlatformRequest) kept = std::string(cut(f.str[0], SandboxEngine::kRemotePlatformLimit));
        const bool synchronous = f.kind == Kind::Query || f.kind == Kind::Intl || f.kind == Kind::PlatformRequest;
        std::shared_ptr<HostRoute> route;
        std::shared_ptr<SandboxRemoteSink> sink;
        uint32_t id = 0;
        {
            std::lock_guard<std::mutex> l(mutex_);
            // The end stops delivery at once, also of frames already read.
            if (ending_) return reason_;
            if (!f.runtime) return connectionLocked(f);
            if (!ready_) return kindOn(f) + " before the opening was answered";
            if (hosting_ && renderKind(f.kind)) return kindOn(f) + ", which a hosted runtime never sends";
            if (!hosting_ && hostedReport(f.kind)) return kindOn(f) + ", which only a hosted runtime sends";
            const auto it = routes_.find(f.runtime);
            if (it == routes_.end()) return f.runtime > lastId_ ? kindOn(f) + ", which was never opened" : std::string();
            route = it->second;
            if (f.kind == Kind::ProbeAnswer) return probedLocked(*route, f, std::move(kept));
            if (std::string why = pendingLocked(*route, f); !why.empty()) return why;
            if (f.kind == Kind::Query) route->pending = Pending::Query;
            if (f.kind == Kind::Intl) route->pending = Pending::Intl;
            if (f.kind == Kind::PlatformRequest) route->platformPending = true;
            sink = route->sink;
            id = route->id;
        }
        if (synchronous) post(route, std::move(sink), id, f.kind, std::move(kept));
        else deliver(*sink, f);
        return {};
    }

    /// Why `f` cannot come while a request of its runtime is in the host's hands, "" when it can: an engine's one producer waits for its answer, so it sends nothing then; a hosted runtime's engine, glue and renderer send on their own, so only a second request of the same kind is refused.
    std::string pendingLocked(const HostRoute& route, const Frame& f) const {
        if (!hosting_) {
            if (route.pending == Pending::None) return {};
            return kindOn(f) + (route.pending == Pending::Query ? " while its query is unanswered" : " while its Intl request is unanswered");
        }
        if (f.kind == Kind::Intl && route.pending == Pending::Intl) return kindOn(f) + " while its Intl request is unanswered";
        if (f.kind == Kind::PlatformRequest && route.platformPending) return kindOn(f) + " while its platform request is unanswered";
        return {};
    }

    /// Ready, once and of this version, or Refused, both only as the first frame.
    std::string connectionLocked(const Frame& f) {
        if (ready_) return "kind " + std::to_string(int(f.kind)) + " after the opening was answered";
        if (f.kind == Kind::Refused) return "the engine process refused the stream: " + std::string(f.str[0].substr(0, SandboxEngine::kRemoteTextLimit));
        if (f.i64 != SandboxRemoteWire::kVersion) return "stream version " + std::to_string(f.i64) + " from the engine process, this end's is " + std::to_string(SandboxRemoteWire::kVersion);
        ready_ = true;
        return {};
    }

    /// A probe's answer: for a sequence never sent the stream ends; a late or repeated one is dropped and answers no other wait; one that comes while a synchronous call is in the host's hands waits for it (for both, while a hosted runtime has two).
    std::string probedLocked(HostRoute& route, const Frame& f, std::string answer) {
        if (f.i64 > route.lastProbe) return kindOn(f) + " for probe " + std::to_string(f.i64) + ", which was never sent";
        if (f.i64 != route.waiting || route.probed || route.deferred) return {};
        if (route.pending != Pending::None || route.platformPending) {
            route.deferred = std::move(answer);
            return {};
        }
        route.probed = std::move(answer);
        route.settled.notify_all();
        return {};
    }

    /// An asynchronous sink call, its strings cut as the engine's end cuts them (the client's verdicts are the same past the cut).
    static void deliver(SandboxRemoteSink& sink, const Frame& f) {
        const auto text = [&](size_t i) { return std::string(cut(f.str[i], SandboxEngine::kRemoteMessageLimit)); };
        switch (f.kind) {
        case Kind::TaskStarted: sink.remoteTaskStarted(); break;
        case Kind::TaskCompleted: sink.remoteTaskCompleted(); break;
        case Kind::Ops: sink.remoteOps(std::string(cut(f.str[0], SandboxEngine::kRemoteOpsLimit))); break;
        case Kind::Listen: sink.remoteListen(text(0)); break;
        case Kind::PreventDefault: sink.remotePreventDefault(text(0)); break;
        case Kind::Anim: sink.remoteAnim(text(0)); break;
        case Kind::RequestsFrame: sink.remoteRequestsFrame(); break;
        case Kind::Log: sink.remoteLog(text(0), text(1)); break;
        case Kind::Diag: sink.remoteDiag(text(0), text(1)); break;
        case Kind::Bridge: sink.remoteBridge(text(0), text(1)); break;
        case Kind::CopyText: sink.remoteCopyText(text(0)); break;
        case Kind::Request: sink.remoteRequest(f.i64, text(0), f.flag); break;
        case Kind::Wallet: sink.remoteWallet(f.i64, text(0)); break;
        case Kind::Storage: sink.remoteStorage(text(0)); break;
        case Kind::ResizeAnchor: sink.remoteResizeAnchor(text(0)); break;
        case Kind::WatchesPublicData: sink.remoteWatchesPublicData(f.flag); break;
        case Kind::Loaded: sink.remoteLoaded(); break;
        case Kind::Stats:
            if (std::optional<SandboxStats> stats = SandboxRemoteCoding::decodeStats(std::vector<double>(f.f64.begin(), f.f64.end()))) sink.remoteStats(*stats);
            break;
        case Kind::Died: sink.remoteDied(text(0)); break;
        case Kind::PlatformMessage: sink.remotePlatformMessage(std::string(cut(f.str[0], SandboxEngine::kRemotePlatformLimit))); break;
        case Kind::ValidatedOps: sink.remoteValidatedOps(std::string(cut(f.str[0], SandboxEngine::kRemoteOpsLimit))); break;
        case Kind::RendererReady: sink.remoteRendererReady(); break;
        case Kind::Rendered: sink.remoteRendered(); break;
        case Kind::Nodes: sink.remoteNodes(f.i64); break;
        default: break;
        }
    }

    /// A query, an Intl request or a platform request to the route's `widget.answer`, made at its first, so the reader goes on for the other runtimes while the client answers; dropped once the channel is destroyed. A platform request the client answers with none (it stopped the widget, which closes the runtime) gets no frame. A route whose `widget.answer` cannot start is lost with why (`kAnswerUnstarted`, then the refusal), which its client hears as its runtime's death; the stream and the other routes go on.
    void post(const std::shared_ptr<HostRoute>& route, std::shared_ptr<SandboxRemoteSink> sink, uint32_t id, Kind kind, std::string request) {
        std::unique_lock<std::mutex> l(route->answering);
        if (route->retired) return;
        if (!route->answerer) {
            try {
                route->answerer = std::make_unique<JSThread>("widget.answer");
            } catch (const std::exception& e) {
                // The client hears the loss outside every lock of this end.
                l.unlock();
                lose(route, std::string(kAnswerUnstarted) + ": " + e.what());
                return;
            }
        }
        route->answerer->async([self = shared_from_this(), route, sink = std::move(sink), id, kind, request = std::move(request)] {
            std::optional<std::string> frame;
            bool lost = false;
            Pending pending = Pending::Query;
            if (kind == Kind::Query) {
                const QueryAnswer answer = sink->remoteQuery(request);
                const std::string_view binary = answer.binary ? std::string_view(reinterpret_cast<const char*>(answer.binary->data()), answer.binary->size()) : std::string_view();
                frame = Codec::encode(id, Kind::QueryAnswer, SandboxRemoteWire::kMaxFrameBytes, {answer.json, answer.binary.has_value(), binary});
                lost = !frame;
            } else if (kind == Kind::Intl) {
                pending = Pending::Intl;
                frame = Codec::encode(id, Kind::IntlAnswer, SandboxRemoteWire::kMaxFrameBytes, {sink->remoteIntl(request)});
                lost = !frame;
            } else {
                pending = Pending::Platform;
                if (const std::optional<std::string> answer = sink->remotePlatformRequest(request)) {
                    frame = Codec::encode(id, Kind::PlatformAnswer, SandboxRemoteWire::kMaxFrameBytes, {*answer});
                    lost = !frame;
                }
            }
            self->answered(route, pending, std::move(frame), lost);
        });
    }

    /// The sink's answer to a request of `kind` queued (none: no frame) and the request settled in one step, then a probe's answer that waited, only once neither request is in the host's hands and every answer was queued; an answer past the frame bound (`lost`) loses the route.
    void answered(const std::shared_ptr<HostRoute>& route, Pending kind, std::optional<std::string> frame, bool lost) {
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (kind == Pending::Platform) route->platformPending = false;
            else route->pending = Pending::None;
            if (!lost) {
                // Not queued (the route closed, lost or leaving with the stream, or an answer the stream cannot take): the probe's answer is dropped, and the probe answers as the route's end does, once its client heard any loss.
                const bool queued = route->state == HostRoute::State::Live && (!frame || queueLocked(std::move(*frame)));
                if (!queued) route->deferred.reset();
                else if (route->deferred && route->pending == Pending::None && !route->platformPending) {
                    route->probed = std::exchange(route->deferred, std::nullopt);
                    route->settled.notify_all();
                }
                return;
            }
            route->deferred.reset();
        }
        lose(route);
    }

    /// The reader's last step: the stream ends (its first reason stays), each open runtime's client hears it once, then their probes wake, then the monitor may report the end.
    void finish(const std::string& why) {
        std::vector<std::pair<std::shared_ptr<HostRoute>, std::shared_ptr<SandboxRemoteSink>>> open;
        {
            std::lock_guard<std::mutex> l(mutex_);
            endLocked(why);
            for (const auto& [id, route] : routes_) {
                route->state = HostRoute::State::Leaving;
                open.emplace_back(route, route->sink);
            }
            routes_.clear();
            watched_.notify_all();
        }
        // Before the probes wake: a client's barrier after its probe finds the death queued.
        for (const auto& entry : open) entry.second->remoteDisconnected();
        std::lock_guard<std::mutex> l(mutex_);
        for (const auto& entry : open) {
            entry.first->state = HostRoute::State::Gone;
            entry.first->settled.notify_all();
        }
        finalized_ = true;
        watched_.notify_all();
    }

    /// The first reason wins; the stream's shutdown wakes a blocked write and read without waiting, and nothing more is queued or delivered.
    void endLocked(std::string why) {
        if (ending_) return;
        ending_ = true;
        reason_ = std::move(why);
        if (held_) held_->shutdown();
        work_.notify_all();
        watched_.notify_all();
    }

    /// Counted against the bounds, the frame being written included: past them the stream ends instead.
    bool queueLocked(std::string frame) {
        if (ending_) return false;
        if (queuedFrames_ >= SandboxEngine::kRemoteQueuedCalls) {
            endLocked("more than " + std::to_string(SandboxEngine::kRemoteQueuedCalls) + " frames queued for the stream");
            return false;
        }
        if (frame.size() > queueBytes_ - queuedBytes_) {
            endLocked("more than " + std::to_string(queueBytes_) + " bytes queued for the stream");
            return false;
        }
        queuedBytes_ += frame.size();
        queuedFrames_++;
        queue_.push_back(std::move(frame));
        work_.notify_one();
        return true;
    }

    /// A live route out of the map, its `Close` queued while the stream goes on.
    void removeLocked(HostRoute& route) {
        routes_.erase(route.id);
        route.state = HostRoute::State::Leaving;
        if (std::optional<std::string> close = Codec::encode(route.id, Kind::Close, SandboxRemoteWire::kMaxFrameBytes, {})) queueLocked(std::move(*close));
        watched_.notify_all();
    }

    /// The open routes but the kept ones: the runtimes `Events::runtimes` reports.
    size_t liveLocked() const {
        return size_t(std::count_if(routes_.begin(), routes_.end(), [](const auto& entry) { return !entry.second->kept; }));
    }

    /// "ERROR no runtime" for a route that is not live, once its client heard why (at most a probe's wait).
    std::string settledLocked(std::unique_lock<std::mutex>& l, HostRoute& route) {
        route.settled.wait_for(l, probeWait_, [&] { return route.state == HostRoute::State::New || route.state == HostRoute::State::Gone; });
        return kNoRuntime;
    }

    /// Used by the reader and the writer alone, both joined before `release`.
    SandboxRemoteStream& stream_;
    const Events events_;
    const Clock::duration probeWait_;
    const double stallMs_;
    const Clock::duration stall_;
    const size_t queueBytes_;
    /// Opened for hosted widgets: every runtime is a hosted one, and `accept` takes its kinds.
    const bool hosting_;

    std::mutex mutex_;
    /// The writer waits on it for frames, the monitor for the rest.
    std::condition_variable work_;
    std::condition_variable watched_;
    std::shared_ptr<SandboxRemoteStream> held_;
    bool ending_ = false;
    std::string reason_;
    bool ready_ = false;
    /// Every open runtime's client heard the end: the monitor reports it.
    bool finalized_ = false;
    std::map<uint32_t, std::shared_ptr<HostRoute>> routes_;
    uint32_t lastId_ = 0;
    std::deque<std::string> queue_;
    size_t queuedBytes_ = 0;
    size_t queuedFrames_ = 0;
    /// One write blocks since `writingSince_`; the monitor's wait is timed while `timing_`.
    bool writing_ = false;
    Clock::time_point writingSince_;
    bool timing_ = false;
};

/// A runtime's channel on the stream, an engine's or a hosted widget's (the opening decides which, so no call checks it): each call goes out as its frame, its forms coded as the loopback codes them (one that does not decode again, or a frame past the bound, loses the runtime); the destructor, on the owner's thread, closes and joins `widget.answer`.
class SandboxRemoteHostEnd::Connection::Channel final : public SandboxHostedChannel {
public:
    explicit Channel(std::shared_ptr<Connection> connection) : connection_(std::move(connection)) {}
    ~Channel() override {
        connection_->close(*route_);
        std::unique_ptr<JSThread> answerer;
        {
            std::lock_guard<std::mutex> l(route_->answering);
            route_->retired = true;
            answerer = std::move(route_->answerer);
        }
        // What it still has runs first, each answer dropped for a closed route.
        answerer.reset();
    }
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    void attach(std::shared_ptr<SandboxRemoteSink> sink) override { connection_->attach(route_, std::move(sink)); }
    void start(const std::string& html, const SandboxConfig& config) override {
        const std::string coded = SandboxRemoteCoding::encodeConfig(config);
        if (SandboxRemoteCoding::decodeConfig(coded)) send(Kind::Start, {html, coded});
        else connection_->lose(route_);
    }
    void event(const std::string& json) override { send(Kind::Event, {json}); }
    void activationEvent(const std::string& json, int gesture) override { send(Kind::Activation, {json, int64_t(gesture)}); }
    void frame(double time) override { send(Kind::AnimationFrame, {time}); }
    void viewport(double innerWidth, double innerHeight, double dpr) override { send(Kind::Viewport, {innerWidth, innerHeight, dpr}); }
    void resize(const std::string& entriesJson) override { send(Kind::Resize, {entriesJson}); }
    void theme(const std::string& colorScheme, const std::map<std::string, std::string>& themeParams) override {
        const std::string coded = SandboxRemoteCoding::encodeThemeParams(themeParams);
        if (SandboxRemoteCoding::decodeThemeParams(coded)) send(Kind::Theme, {colorScheme, coded});
        else connection_->lose(route_);
    }
    void contentSafeArea(const WidgetContentSafeArea& area) override {
        const std::string coded = SandboxRemoteCoding::encodeSafeArea(area);
        if (SandboxRemoteCoding::decodeSafeArea(coded)) send(Kind::SafeArea, {coded});
        else connection_->lose(route_);
    }
    void setData(const WidgetData& data) override {
        const std::string coded = SandboxRemoteCoding::encodeData(data);
        if (SandboxRemoteCoding::decodeData(coded)) send(Kind::Data, {coded});
        else connection_->lose(route_);
    }
    void setVisible(bool visible) override { send(Kind::Visible, {visible}); }
    void pressBegan() override { send(Kind::PressBegan, {}); }
    void pressEnded() override { send(Kind::PressEnded, {}); }
    void chargeValidator(double ms) override { send(Kind::ChargeValidator, {ms}); }
    void stop(const std::string& reason) override { send(Kind::Stop, {reason}); }
    void answer(int64_t id, const SandboxRemoteOutcome& outcome) override {
        const std::string coded = SandboxRemoteCoding::encodeOutcome(outcome);
        if (SandboxRemoteCoding::decodeOutcome(coded)) send(Kind::Answer, {id, coded});
        else connection_->lose(route_);
    }
    std::string probe(const std::string& code) override { return connection_->probe(route_, code); }
    void close() override { connection_->close(*route_); }
    void startHosted(const std::string& html, const SandboxConfig& config, const SandboxHostedStart& start) override {
        const std::string coded = SandboxRemoteCoding::encodeConfig(config), hosted = SandboxRemoteCoding::encodeHostedStart(start);
        if (SandboxRemoteCoding::decodeConfig(coded) && SandboxRemoteCoding::decodeHostedStart(hosted)) send(Kind::HostedStart, {html, coded, hosted});
        else connection_->lose(route_);
    }
    void hostedTheme(const WidgetTheme& theme) override {
        const std::string coded = SandboxRemoteCoding::encodeTheme(theme);
        if (SandboxRemoteCoding::decodeTheme(coded)) send(Kind::HostedTheme, {coded});
        else connection_->lose(route_);
    }
    void platform(const std::string& message) override { send(Kind::Platform, {message}); }
    void kept() override { connection_->keep(*route_); }

private:
    /// Encoded outside the lock once the route is live, then admitted only if it still is.
    void send(Kind kind, std::initializer_list<Field> fields) {
        const std::optional<uint32_t> id = connection_->liveId(*route_);
        if (!id) return;
        std::optional<std::string> frame = Codec::encode(*id, kind, SandboxRemoteWire::kMaxFrameBytes, fields);
        if (frame) connection_->admit(*route_, std::move(*frame));
        else connection_->lose(route_);
    }

    const std::shared_ptr<Connection> connection_;
    const std::shared_ptr<HostRoute> route_ = std::make_shared<HostRoute>();
};

SandboxRemoteHostEnd::SandboxRemoteHostEnd(std::shared_ptr<Connection> connection)
    : connection_(std::move(connection)), reader_(std::make_unique<JSThread>("widget.reader")), writer_(std::make_unique<JSThread>("widget.writer")),
      monitor_(std::make_unique<JSThread>("widget.monitor")) {
    writer_->async([connection = connection_] { connection->write(); });
    monitor_->async([connection = connection_] { connection->watch(); });
    reader_->async([connection = connection_] { connection->read(); });
}

std::shared_ptr<SandboxRemoteHostEnd> SandboxRemoteHostEnd::start(std::shared_ptr<SandboxRemoteStream> stream, const SandboxResources& resources, std::string build, Events events, Limits limits) {
    auto connection = std::make_shared<Connection>(std::move(stream), std::move(events), limits, false);
    connection->open(resources, build);
    return std::shared_ptr<SandboxRemoteHostEnd>(new SandboxRemoteHostEnd(std::move(connection)));
}

std::shared_ptr<SandboxRemoteHostEnd> SandboxRemoteHostEnd::startHosting(std::shared_ptr<SandboxRemoteStream> stream, const SandboxResources& resources, std::string build, Events events, Limits limits) {
    auto connection = std::make_shared<Connection>(std::move(stream), std::move(events), limits, true);
    connection->open(resources, build);
    return std::shared_ptr<SandboxRemoteHostEnd>(new SandboxRemoteHostEnd(std::move(connection)));
}

SandboxRemoteHostEnd::~SandboxRemoteHostEnd() { stop(); }

std::shared_ptr<SandboxRemoteChannel> SandboxRemoteHostEnd::channel() {
    if (connection_->hosting()) return refusing("the stream was opened for hosted widgets");
    return std::make_shared<Connection::Channel>(connection_);
}

std::shared_ptr<SandboxHostedChannel> SandboxRemoteHostEnd::hostedChannel() {
    if (!connection_->hosting()) return refusingHosted("the stream was opened for engines only");
    return std::make_shared<Connection::Channel>(connection_);
}

void SandboxRemoteHostEnd::stop() {
    connection_->end(kHostStopped);
    reader_->join();
    writer_->join();
    monitor_->join();
    connection_->release();
}

std::shared_ptr<SandboxRemoteChannel> SandboxRemoteHostEnd::refusing(std::string reason) { return std::make_shared<RefusingChannel>(std::move(reason)); }

std::shared_ptr<SandboxHostedChannel> SandboxRemoteHostEnd::refusingHosted(std::string reason) { return std::make_shared<RefusingChannel>(std::move(reason)); }

} // namespace twidget
