// The host's end of a stream (twidget/remote_stream.h, `SandboxRemoteHostEnd`): its reader, writer and monitor, a channel per runtime and the refusing channel. The engine process's end is remote_stream_engine.cpp's, so a host's program links neither the engine's server nor an engine through this one.
#include "twidget/remote_stream.h"

#include "remote_wire.h"
#include "twidget/json.h"

#include <algorithm>
#include <atomic>
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

/// One host's message its renderer has yet to answer (SandboxHostedAnswers): its key, whether its time counts, the moment its writer admitted it and the host's time owed to its route by then.
struct Awaited {
    /// Untimed or timed by its route's visibility when sent; unresolved when sent before that was known (no `Visible` and no `HostedStart` before it), until the `HostedStart` decides by its config.
    enum class Timing : uint8_t { Untimed, Timed, Unresolved };

    uint32_t family = 0;
    std::optional<uint64_t> sequence;
    Timing timing = Timing::Untimed;
    Clock::time_point at;
    Clock::duration owedAt{};
};

/// What a hosting stream with the owner's SandboxHostedAnswers keeps of one route, under the connection's mutex: the host's messages its renderer has yet to answer, oldest first, the route's visibility and the host's own time (from a platform request's arrival until its answer was handed to the stream).
struct HostAccount {
    static constexpr size_t kNone = SIZE_MAX;

    std::deque<Awaited> awaited;
    /// Where in `awaited` its oldest timed message is (kNone: none), and how many there are unresolved.
    size_t oldestTimed = kNone;
    size_t unresolved = 0;
    /// The route's visibility, from its first `Visible` or its `HostedStart`, whichever the writer admitted first; none before either.
    std::optional<bool> visible;
    /// The writer admitted the route's `HostedStart`, where the engine process builds its renderer: only from then on is a renderer's message read (`Connection::accept`).
    bool started = false;
    /// The renderer answers nothing more (its `Last` message or its engine's `Died` came): nothing is kept for it again.
    bool halted = false;
    /// The host's time that ended, and since when the current one runs, for the request numbered `owingFor` of the `requests` that came.
    Clock::duration owed{};
    std::optional<Clock::time_point> owingSince;
    uint64_t requests = 0;
    uint64_t owingFor = 0;

    /// The host's time owed to the route up to `now`, the current one's elapsed part included.
    Clock::duration owedUntil(Clock::time_point now) const { return owed + (owingSince ? now - *owingSince : Clock::duration::zero()); }

    /// A message that expects its answer, admitted at `now`, timed by the visibility it was sent with.
    void expect(const SandboxHostedAnswers::Reading& reading, Clock::time_point now) {
        const Awaited::Timing timing = !reading.whenVisible ? Awaited::Timing::Timed
                                       : !visible           ? Awaited::Timing::Unresolved
                                       : *visible           ? Awaited::Timing::Timed
                                                            : Awaited::Timing::Untimed;
        if (timing == Awaited::Timing::Timed && oldestTimed == kNone) oldestTimed = awaited.size();
        if (timing == Awaited::Timing::Unresolved) unresolved++;
        awaited.push_back({reading.family, reading.sequence, timing, now, owedUntil(now)});
    }

    /// A visibility admitted, a `HostedStart`'s config with `start`: that one is the route's only when no `Visible` came before it, and it decides every message sent before either.
    void see(bool shown, bool start) {
        if (!start) {
            visible = shown;
            return;
        }
        if (!visible) visible = shown;
        if (!unresolved) return;
        for (Awaited& entry : awaited) {
            if (entry.timing == Awaited::Timing::Unresolved) entry.timing = shown ? Awaited::Timing::Timed : Awaited::Timing::Untimed;
        }
        unresolved = 0;
        oldestTimed = timedFrom(0);
    }

    /// Whether `reading` answers the oldest message kept: the same family and the same sequence, or none for both.
    bool answeredBy(const SandboxHostedAnswers::Reading& reading) const {
        return !awaited.empty() && awaited.front().family == reading.family && awaited.front().sequence == reading.sequence;
    }

    /// The oldest message settled; the search for the next timed one never passes an entry twice.
    void settle() {
        if (awaited.front().timing == Awaited::Timing::Unresolved) unresolved--;
        awaited.pop_front();
        if (oldestTimed == 0) oldestTimed = timedFrom(0);
        else if (oldestTimed != kNone) oldestTimed--;
    }

    /// Every message settled: none is awaited any more.
    void clear() {
        awaited.clear();
        oldestTimed = kNone;
        unresolved = 0;
    }

private:
    size_t timedFrom(size_t from) const {
        for (size_t i = from; i < awaited.size(); i++) {
            if (awaited[i].timing == Awaited::Timing::Timed) return i;
        }
        return kNone;
    }
};

/// A frame for the writer; a platform answer on an accounted route names that route and the request it answers (`route` 0: none), whose host time ends once the frame is written.
struct Queued {
    std::string frame;
    uint32_t route = 0;
    uint64_t request = 0;
};

/// What a host's frame changes in its route's account once the writer took it (`Connection::admit`): a platform message that expects its answer is kept from then; a visibility (a `Visible`'s, or with `start` a `HostedStart`'s config) holds for the messages after it.
struct Sent {
    std::optional<SandboxHostedAnswers::Reading> expects;
    std::optional<bool> visible;
    bool start = false;
};

/// A route's view of what `decided` reads (SandboxHostedChannel::decidedView), owned apart from the route and its connection: `HostRoute::become` stores what its part reads, in the same step that changes it, and a part the stream's first end froze reads that end's publication, one flag of its connection's that every route it froze shares (`Connection::blameLocked` raises it once it froze them all), so no reader sees one of those routes decided and, later, another one still undecided.
class RouteDecidedView final : public SandboxHostedDecidedView {
public:
    /// What the part reads: `No` while Undecided or Withdrawn, `Yes` while Own or Settled, `AtEnd` while Frozen (the end's publication).
    enum class Reading : uint8_t { No, Yes, AtEnd };

    explicit RouteDecidedView(std::shared_ptr<const std::atomic<bool>> published) : published_(std::move(published)) {}

    bool decided() const override {
        const Reading reading = reading_.load();
        return reading == Reading::Yes || (reading == Reading::AtEnd && published_->load());
    }
    void store(Reading reading) { reading_.store(reading); }

private:
    const std::shared_ptr<const std::atomic<bool>> published_;
    std::atomic<Reading> reading_{Reading::No};
};

/// One runtime on the host's side, made with its channel: the connection's mutex guards every field but the answer thread's three, which `answering` guards.
struct HostRoute {
    /// `published`: its connection's flag of the first end's decision (`RouteDecidedView`).
    explicit HostRoute(std::shared_ptr<const std::atomic<bool>> published) : decidedView(std::make_shared<RouteDecidedView>(std::move(published))) {}

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
    /// Made with its channel on a stream with the owner's SandboxHostedAnswers, none on any other, and never replaced: what its renderer has yet to answer.
    std::unique_ptr<HostAccount> account;
    /// Whether the stream's first end took part in its client's death (`SandboxHostedChannel::withdraw`; `decided` while Own, Frozen or Settled): Undecided until the end froze it, the owner withdrew the route first or its client or host claimed a death of the widget's own first (Own: nothing of the route is kept, timed or blamed again, and an end that finds it still open and neither kept nor halted settles it as that death); a frozen death settles once, at its client's first death or as cleanup takes the route from its client.
    enum class Part : uint8_t { Undecided, Withdrawn, Own, Frozen, Settled };
    Part part = Part::Undecided;
    /// What `decided` reads of `part` (SandboxHostedChannel::decidedView), stored in the same step that changes it (`become`) and owned apart from the route, so its channel's owner reads it with no lock and after the route went.
    const std::shared_ptr<RouteDecidedView> decidedView;
    /// A death of the widget's own was claimed before any end decided the route's (Own): Settled then holds that claim, not an end's decision (`Connection::localStop`).
    bool claimed = false;
    /// What the stream's first end made of its client's death (`SandboxHostedChannel::death`), decided before any client hears the end, never after: `Ordinary` for a route the end decided nothing of; once settled, the death it settled as.
    SandboxHostedChannel::Death death = SandboxHostedChannel::Death::Ordinary;
    /// The owner's withdrawal that found the death frozen, until it settles.
    std::function<void(SandboxHostedChannel::Death)> hears;
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

    /// Whether `part` decides its client's death (`SandboxHostedChannel::decided`).
    static bool decides(Part part) { return part == Part::Own || part == Part::Frozen || part == Part::Settled; }
    /// `part` becomes `next`, under the connection's mutex, and `decidedView` tells it in the same step: Frozen, which only an end makes, as that end's publication.
    void become(Part next) {
        using Reading = RouteDecidedView::Reading;
        part = next;
        decidedView->store(next == Part::Frozen ? Reading::AtEnd : decides(next) ? Reading::Yes : Reading::No);
    }
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
    bool platform(const std::string&) override { return false; }
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

    /// `hosting`: opened for hosted widgets (`startHosting`), every runtime of the stream a hosted one, with the owner's `answers` when it gave them.
    Connection(std::shared_ptr<SandboxRemoteStream> stream, Events events, const Limits& limits, bool hosting, SandboxHostedAnswers answers)
        : stream_(*stream), events_(std::move(events)), probeWait_(durationOf(boundMs(limits.probeWaitMs, Limits{}.probeWaitMs))),
          stallMs_(boundMs(limits.writeStallMs, Limits{}.writeStallMs)), stall_(durationOf(stallMs_)), queueBytes_(limits.queuedBytes ? limits.queuedBytes : Limits{}.queuedBytes),
          hosting_(hosting), answers_(hosting ? std::move(answers) : SandboxHostedAnswers{}), held_(std::move(stream)) {}

    bool hosting() const { return hosting_; }
    /// Keeps what each route's renderer has yet to answer: a hosting stream given the owner's SandboxHostedAnswers.
    bool accounting() const { return bool(answers_.read); }
    /// What every route's view reads while its part is Frozen (`RouteDecidedView`): the first end's decision, published once it froze every route it took (`blameLocked`).
    std::shared_ptr<const std::atomic<bool>> published() const { return published_; }

    /// The owner's reading of a platform message (SandboxHostedAnswers::read), outside every lock: `None` without one, for a message past `kRemotePlatformLimit` (its client drops one from the engine's side) and for a reading that throws.
    SandboxHostedAnswers::Reading readOf(std::string_view message) const {
        if (!answers_.read || message.size() > SandboxEngine::kRemotePlatformLimit) return {};
        try {
            return answers_.read(message);
        } catch (...) {
            return {};
        }
    }

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
            Queued queued;
            {
                std::unique_lock<std::mutex> l(mutex_);
                work_.wait(l, [&] { return ending_ || !queue_.empty(); });
                if (ending_) break;
                queued = std::move(queue_.front());
                queue_.pop_front();
            }
            const std::string& frame = queued.frame;
            bool written = true;
            size_t at = 0;
            for (; written && at < frame.size(); at += kWriteChunk) {
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
            if (written && at >= frame.size()) handedLocked(queued);
            queuedBytes_ -= frame.size();
            queuedFrames_--;
        }
        std::deque<Queued> dropped;
        std::lock_guard<std::mutex> l(mutex_);
        dropped.swap(queue_);
        queuedBytes_ = 0;
        queuedFrames_ = 0;
    }

    /// `widget.monitor`'s loop: ends the stream when one write outlasts its bound, or on an accounting stream when a hosted renderer's timed message outlasts its answer time (`lookLocked`), with no other call needed, and alone reports to the owner outside the lock: the open count (kept routes left out) as it changes, then the end once and last. It sleeps until the earlier of the two (`wakeLocked`), woken only for an earlier one: no poll, no timer per route.
    void watch() {
        size_t reported = 0;
        std::unique_lock<std::mutex> l(mutex_);
        for (;;) {
            if (writing_ && !ending_ && Clock::now() - writingSince_ >= stall_) {
                endLocked("a write to the stream blocked for more than " + Json::number(stallMs_) + " ms");
                continue;
            }
            if (!ending_ && accounting() && lookLocked()) continue;
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
            const std::optional<Clock::time_point> wake = wakeLocked();
            wake_ = wake;
            // A write that starts from now on outlasts its bound after this wake at the earliest: the writer need not wake the monitor.
            timing_ = wake && *wake <= Clock::now() + stall_;
            if (wake) watched_.wait_until(l, *wake);
            else watched_.wait(l);
            timing_ = false;
        }
    }

    /// The opening, the first frame queued: the sandbox bundle, and only the other bundles' stamps; a hosting stream's (`HostingOpening`) the validator bundle too, the renderer's stamp alone.
    void open(const SandboxResources& r, const std::string& build) {
        constexpr int64_t version = SandboxRemoteWire::kVersion;
        constexpr size_t bound = SandboxRemoteWire::kMaxOpeningBytes;
        std::optional<std::string> opening =
            hosting_ ? Codec::encode(0, Kind::HostingOpening, bound, {version, build, r.sandboxBundle, r.validatorBundle, stampOf(r.rendererBundle)})
                     : Codec::encode(0, Kind::Opening, bound, {version, build, r.sandboxBundle, stampOf(r.validatorBundle), stampOf(r.rendererBundle)});
        std::lock_guard<std::mutex> l(mutex_);
        if (opening) queueLocked(std::move(*opening));
        else endLocked("an opening past the bound of " + std::to_string(SandboxRemoteWire::kMaxOpeningBytes) + " bytes");
    }

    /// The owner's stop (`SandboxRemoteHostEnd::stop`): the stream ends, deciding nothing of its routes' deaths; the instant of a first end that was no stop.
    std::optional<Clock::time_point> stop() {
        std::lock_guard<std::mutex> l(mutex_);
        endLocked(kHostStopped, End::Stopped);
        return stopped_ ? std::nullopt : std::optional<Clock::time_point>(endedAt_);
    }

    /// After the threads joined: the stream goes, outside the lock.
    void release() {
        std::shared_ptr<SandboxRemoteStream> held;
        std::lock_guard<std::mutex> l(mutex_);
        held = std::move(held_);
    }

    /// `route`'s id and its `Open` in one step, so the engine's end sees the ids rise in the order they were given; without them (the stream ended, every id given) its client hears at once that the stream is lost, a death an accounting stream that ended without the owner's stop spares (`blameLocked`).
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
            if (ending_ && !stopped_ && accounting() && route->part == HostRoute::Part::Undecided) {
                route->death = SandboxHostedChannel::Death::Spared;
                route->become(HostRoute::Part::Frozen);
            }
        }
        sink->remoteDisconnected();
    }

    /// `route`'s id while a frame of it may still go out.
    std::optional<uint32_t> liveId(HostRoute& route) {
        std::lock_guard<std::mutex> l(mutex_);
        if (route.state != HostRoute::State::Live || ending_) return std::nullopt;
        return route.id;
    }

    /// A frame of `route`'s encoded outside the lock, queued only while the route and the stream still are: whether the writer took it (false for a route no longer live, a stream ending, or a queue past its bounds, which ends the stream). On an accounted route what it changes (`sent`) holds from the moment the writer took it, in the order of the route's frames: a message that expects its answer is kept from then on a route that is neither kept nor halted, and one past `kRemoteQueuedCalls` kept ends the stream instead.
    bool admit(HostRoute& route, std::string frame, const Sent& sent = {}) {
        std::lock_guard<std::mutex> l(mutex_);
        if (route.state != HostRoute::State::Live) return false;
        HostAccount* const account = route.account.get();
        const bool expects = account && sent.expects && !route.kept && !account->halted && route.part == HostRoute::Part::Undecided;
        if (expects && account->awaited.size() >= SandboxEngine::kRemoteQueuedCalls) {
            endLocked("more than " + std::to_string(SandboxEngine::kRemoteQueuedCalls) + " platform messages unanswered on runtime " + std::to_string(route.id));
            return false;
        }
        if (!queueLocked(std::move(frame))) return false;
        if (account && sent.start) account->started = true;
        if (account && sent.visible) account->see(*sent.visible, sent.start);
        if (expects) account->expect(*sent.expects, Clock::now());
        // The `HostedStart` may time messages sent before it.
        if (expects || (account && sent.start)) wakeForLocked(route);
        return true;
    }

    /// The client's first death is the end: `route`'s frozen death settles as the end decided it (`SandboxHostedChannel::withdraw`); what that death is to the client (`SandboxHostedChannel::death`): the end's decision, or `Ordinary` for a route the end decided nothing of.
    SandboxHostedChannel::Death settle(HostRoute& route) {
        std::function<void()> heard;
        SandboxHostedChannel::Death death = SandboxHostedChannel::Death::Ordinary;
        {
            std::lock_guard<std::mutex> l(mutex_);
            heard = settleLocked(route);
            death = route.death;
        }
        if (heard) heard();
        return death;
    }

    /// The client's first death is the widget's own (a flood, a platform request left unanswered, its engine's death), in one step with the end: a death the end froze settles `Ordinary`; before the end decided the route's death the claim is kept (`Part::Own`) and the route's watchdog debt goes with it, so nothing of the route is kept, timed or blamed again, and that dead widget alone never ends its siblings' stream.
    void claim(HostRoute& route) {
        std::function<void()> heard;
        {
            std::lock_guard<std::mutex> l(mutex_);
            claimLocked(route);
            heard = settleLocked(route, SandboxHostedChannel::Death::Ordinary);
        }
        if (heard) heard();
    }

    /// The channel's `localStop` (SandboxHostedChannel::localStop), from any thread, in one step with the end: before the end decided the route's death the widget's own death is claimed as `claim` claims it, and none comes back; when the end froze the route first, its decision as frozen or settled now, which nothing here changes (no settlement, a waiting withdrawal kept); none when a death of the widget's own was claimed first or the owner withdrew the route first.
    std::optional<SandboxHostedChannel::Death> localStop(HostRoute& route) {
        std::lock_guard<std::mutex> l(mutex_);
        claimLocked(route);
        const bool ended = route.part == HostRoute::Part::Frozen || (route.part == HostRoute::Part::Settled && !route.claimed);
        return ended ? std::optional<SandboxHostedChannel::Death>(route.death) : std::nullopt;
    }

    /// The channel's `withdraw` (SandboxHostedChannel::withdraw): before the end decided the route's death (an own death claimed first included, which the withdrawal tells as `own` from the part it replaces) the route takes no part in it from now on, nothing of it kept, timed or blamed; after, its death as settled, or none with `settled` kept to hear it.
    SandboxHostedChannel::Withdrawal withdraw(HostRoute& route, std::function<void(SandboxHostedChannel::Death)> settled) {
        std::lock_guard<std::mutex> l(mutex_);
        switch (route.part) {
        case HostRoute::Part::Undecided:
        case HostRoute::Part::Own: {
            SandboxHostedChannel::Withdrawal result;
            result.own = route.part == HostRoute::Part::Own;
            route.become(HostRoute::Part::Withdrawn);
            if (route.account) route.account->clear();
            return result;
        }
        case HostRoute::Part::Withdrawn: return {};
        case HostRoute::Part::Frozen:
            route.hears = std::move(settled);
            return {true, std::nullopt};
        case HostRoute::Part::Settled: return {true, route.death};
        }
        return {};
    }

    /// The channel's `decided` (SandboxHostedChannel::decided), changing nothing: the end froze or settled the route's death, or its client or host claimed a death of the widget's own first and no withdrawal came first on it.
    bool decided(const HostRoute& route) {
        std::lock_guard<std::mutex> l(mutex_);
        return HostRoute::decides(route.part);
    }

    /// The channel's `kept`: a live route counts no more among the open runtimes, and the monitor reports the new count; nothing its renderer has yet to answer is awaited any more; a death the end froze settles.
    void keep(HostRoute& route) {
        std::function<void()> heard;
        {
            std::lock_guard<std::mutex> l(mutex_);
            heard = settleLocked(route);
            if (route.state == HostRoute::State::Live && !std::exchange(route.kept, true)) {
                if (route.account) route.account->clear();
                watched_.notify_all();
            }
        }
        if (heard) heard();
    }

    /// The channel's `close`: the route goes with its `Close` and a waiting probe wakes; its client, which closes, hears nothing; a death the end froze settles.
    void close(HostRoute& route) {
        std::function<void()> heard;
        {
            std::lock_guard<std::mutex> l(mutex_);
            heard = settleLocked(route);
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
        if (heard) heard();
    }

    /// A form of the host's that does not decode or fit a frame, or a runtime whose `widget.answer` cannot start: the route goes as `close` lets it go, and its client hears that the stream is lost, as over the loopback, after `died` as its runtime's death when one is given. A death the end froze stays for its client to settle (its first death, an own one queued before this loss included), since the client still hears this one.
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
        // The owner's reading of a renderer's message, outside the lock; it counts only for a route whose renderer was built (`HostAccount::started`).
        const SandboxHostedAnswers::Reading reading = f.kind == Kind::PlatformMessage ? readOf(f.str[0]) : SandboxHostedAnswers::Reading{};
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
            HostAccount* const account = route->account.get();
            // Before the route's `HostedStart` there is no renderer to answer or to stop: its words settle and halt nothing.
            const Heard heard = account ? heardOf(*account, f.kind, account->started ? reading : SandboxHostedAnswers::Reading{}) : Heard::Nothing;
            if (expiredBeforeLocked(*route, heard)) return reason_;
            if (f.kind == Kind::Query) route->pending = Pending::Query;
            if (f.kind == Kind::Intl) route->pending = Pending::Intl;
            if (f.kind == Kind::PlatformRequest) route->platformPending = true;
            if (account) heardLocked(*account, heard);
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

    /// What a frame of a hosted runtime's, accepted for its sink, does to its route's account, decided once under the lock before the sink hears it (`heardOf`), then checked against the answer times (`expiredBeforeLocked`) and applied (`heardLocked`) as decided.
    enum class Heard : uint8_t {
        /// Nothing kept changes (a message read as none, an answer with another key than the oldest message's, any other kind).
        Nothing,
        /// A platform request: the host's time starts, or goes on when the previous answer is still on its way (the request shows that it arrived).
        Request,
        /// An answer with the oldest kept message's key: that message alone settles.
        Oldest,
        /// The renderer's last word or its engine's death: every message settles, and nothing is kept again.
        All,
    };

    /// What `kind` with its owner's `reading` does to `account` as it stands now (`Heard`).
    static Heard heardOf(const HostAccount& account, Kind kind, const SandboxHostedAnswers::Reading& reading) {
        if (kind == Kind::PlatformRequest) return Heard::Request;
        if (kind == Kind::Died || reading.role == SandboxHostedAnswers::Role::Last) return Heard::All;
        if (reading.role == SandboxHostedAnswers::Role::Answers && account.answeredBy(reading)) return Heard::Oldest;
        return Heard::Nothing;
    }

    /// `heard`, as `heardOf` decided it, applied to `account`.
    static void heardLocked(HostAccount& account, Heard heard) {
        switch (heard) {
        case Heard::Request:
            account.owingFor = ++account.requests;
            if (!account.owingSince) account.owingSince = Clock::now();
            break;
        case Heard::All:
            account.halted = true;
            account.clear();
            break;
        case Heard::Oldest: account.settle(); break;
        case Heard::Nothing: break;
        }
    }

    /// Before `heard` settles `route`'s oldest timed message, whose answer time may have run out (every message settling, or the oldest one when it is the oldest timed one; an untimed one ahead of it settles alone): the stream ends as the monitor's look would have ended it, when a look at this moment concludes so (`lookAtAnswers` with this route's answer time, the second look left to the monitor), so a late frame never erases what that look would see. Whether it ended.
    bool expiredBeforeLocked(const HostRoute& route, Heard heard) {
        const HostAccount* const account = route.account.get();
        if (!account || account->oldestTimed == HostAccount::kNone) return false;
        const bool settles = heard == Heard::All || (heard == Heard::Oldest && account->oldestTimed == 0);
        if (!settles) return false;
        const Clock::time_point now = Clock::now();
        if (!lookAtAnswers(dueLocked(route), wake_, recheck_, now).expired) return false;
        expireLocked(now);
        return true;
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

    /// A query, an Intl request or a platform request to the route's `widget.answer`, made at its first, so the reader goes on for the other runtimes while the client answers; dropped once the channel is destroyed. A platform request the client answers with none (it stopped the widget, which closes the runtime, or it had no delegate to ask) gets no frame, and on an accounted route that is still live it loses the route (`answered`). A route whose `widget.answer` cannot start is lost with why (`kAnswerUnstarted`, then the refusal), which its client hears as its runtime's death; the stream and the other routes go on.
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

    /// The sink's answer to a request of `kind` queued (none: no frame) and the request settled in one step, then a probe's answer that waited, only once neither request is in the host's hands and every answer was queued; an answer past the frame bound (`lost`) loses the route, and so does a platform request answered with none on an accounted route still live: its renderer waits for an answer that never comes and the host's time it started would never end (`dueLocked`), so the route goes, its client hearing it after any death of its own it queued before giving no answer.
    void answered(const std::shared_ptr<HostRoute>& route, Pending kind, std::optional<std::string> frame, bool lost) {
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (kind == Pending::Platform) route->platformPending = false;
            else route->pending = Pending::None;
            const bool unanswered = kind == Pending::Platform && !frame && route->account && route->state == HostRoute::State::Live;
            if (!lost && !unanswered) {
                // A platform answer's host time ends once the writer handed it to the stream (`handedLocked`).
                const bool handing = kind == Pending::Platform && route->account;
                // Not queued (the route closed, lost or leaving with the stream, or an answer the stream cannot take): the probe's answer is dropped, and the probe answers as the route's end does, once its client heard any loss.
                const bool queued = route->state == HostRoute::State::Live
                                    && (!frame || queueLocked(std::move(*frame), handing ? route->id : 0, handing ? route->account->owingFor : 0));
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

    /// Why the stream ends, as far as its routes' deaths go: a renderer's answer time that ran out by the end's instant (`expireLocked`), the owner's stop, or any other end (the reader's, a write that failed or stalled, a queue past its bounds, a frame the stream cannot take).
    enum class End : uint8_t { Lost, Unanswered, Stopped };

    /// The first reason wins, with its instant (`endedAt_`: for an `Unanswered` end `at`, the instant its look concluded, otherwise now), and with it, on an accounting stream, the decision of every open route's death at that same instant (`blameLocked`), before the shutdown and before any client hears the end; the stream's shutdown wakes a blocked write and read without waiting, and nothing more is queued or delivered.
    void endLocked(std::string why, End end = End::Lost, Clock::time_point at = {}) {
        if (ending_) return;
        ending_ = true;
        endedAt_ = end == End::Unanswered ? at : Clock::now();
        reason_ = std::move(why);
        stopped_ = end == End::Stopped;
        if (accounting() && !stopped_) blameLocked(end == End::Unanswered, endedAt_);
        if (held_) held_->shutdown();
        work_.notify_all();
        watched_.notify_all();
    }

    /// A timed message's answer time ran out by `at`, as the monitor's look or a frame that would settle it saw: the stream ends, its routes whose time ran out by then stalled.
    void expireLocked(Clock::time_point at) {
        endLocked("a hosted renderer left a platform message unanswered for more than " + Json::number(SandboxEngine::kPlatformAnswerSeconds * 1000) + " ms", End::Unanswered, at);
    }

    /// The first end's decision of the open routes' deaths, each route's only one (`HostRoute::death`; `finish` only hands it out, whatever closes or keeps a route meanwhile). Only an active route takes part, one neither kept, halted (its renderer's last word, its engine's `Died`), withdrawn by its owner (`withdraw`) nor dead of its own (`claim`, `localStop`): the others keep their own death (`Ordinary`), and one whose client claimed a death of its own while neither kept nor halted settles as that death, which the end finds already decided; each one that takes part is frozen, to settle once (`settleLocked`). With `unanswered`, the active routes whose answer time ran out by `at` stalled and every other active one spared, the ones with younger messages unanswered among them; at any other end, the active routes with a message unanswered (timed or not) keep an ordinary death and the others are spared, unless none had one, when every active route keeps it. The frozen routes' views turn together, at the publication after the last of them froze.
    void blameLocked(bool unanswered, Clock::time_point at) {
        const auto active = [](const HostRoute& route) { return route.account && !route.kept && !route.account->halted && route.part == HostRoute::Part::Undecided; };
        const bool awaited = std::any_of(routes_.begin(), routes_.end(), [&](const auto& entry) { return active(*entry.second) && !entry.second->account->awaited.empty(); });
        for (const auto& [id, route] : routes_) {
            if (route->part == HostRoute::Part::Own && !route->kept && !(route->account && route->account->halted)) route->become(HostRoute::Part::Settled);
            if (!active(*route)) continue;
            route->become(HostRoute::Part::Frozen);
            if (unanswered) route->death = overdueLocked(*route, at) ? SandboxHostedChannel::Death::Stalled : SandboxHostedChannel::Death::Spared;
            else if (awaited && route->account->awaited.empty()) route->death = SandboxHostedChannel::Death::Spared;
        }
        published_->store(true);
    }

    /// `route`'s frozen death settles, once, as frozen or `as`: what tells the owner's withdrawal that waits for it, to call once the lock is released (empty: nothing to tell).
    static std::function<void()> settleLocked(HostRoute& route, std::optional<SandboxHostedChannel::Death> as = std::nullopt) {
        if (route.part != HostRoute::Part::Frozen) return {};
        route.become(HostRoute::Part::Settled);
        if (as) route.death = *as;
        if (!route.hears) return {};
        return [hears = std::exchange(route.hears, nullptr), death = route.death] { hears(death); };
    }

    /// A death of the widget's own claimed while the end has not decided `route`'s (`claim`, `localStop`): it takes no part in the end's decision from now on (Own) and nothing its renderer owes is kept any more, none of it credited; any other route stays as it is, and so does every route of a stream without the owner's SandboxHostedAnswers (no account), whose end decides nothing.
    static void claimLocked(HostRoute& route) {
        if (!route.account || route.part != HostRoute::Part::Undecided) return;
        route.become(HostRoute::Part::Own);
        route.claimed = true;
        route.account->clear();
    }

    /// Counted against the bounds, the frame being written included: past them the stream ends instead. A platform answer of an accounted route names it and the request it answers (`handedLocked`).
    bool queueLocked(std::string frame, uint32_t answering = 0, uint64_t request = 0) {
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
        queue_.push_back({std::move(frame), answering, request});
        work_.notify_one();
        return true;
    }

    /// The writer handed `queued` whole to the stream: when it is the answer its route's host time waits for, that time ends now; one the renderer's next request already took over goes on.
    void handedLocked(const Queued& queued) {
        if (!queued.route) return;
        const auto it = routes_.find(queued.route);
        if (it == routes_.end() || !it->second->account) return;
        HostAccount& account = *it->second->account;
        if (!account.owingSince || account.owingFor != queued.request) return;
        account.owed += Clock::now() - *account.owingSince;
        account.owingSince.reset();
        wakeForLocked(*it->second);
    }

    /// A live route out of the map, its `Close` queued while the stream goes on, and nothing its renderer has yet to answer awaited any more.
    void removeLocked(HostRoute& route) {
        routes_.erase(route.id);
        route.state = HostRoute::State::Leaving;
        if (route.account) route.account->clear();
        if (std::optional<std::string> close = Codec::encode(route.id, Kind::Close, SandboxRemoteWire::kMaxFrameBytes, {})) queueLocked(std::move(*close));
        watched_.notify_all();
    }

    /// The open routes but the kept ones: the runtimes `Events::runtimes` reports.
    size_t liveLocked() const {
        return size_t(std::count_if(routes_.begin(), routes_.end(), [](const auto& entry) { return !entry.second->kept; }));
    }

    /// When `route`'s oldest timed message has used its answer time, the host's time since its admission left out: none while none is kept, and none while the host owes the route time, unless that message was due before that time began, which no later wait of the host's undoes.
    std::optional<Clock::time_point> dueLocked(const HostRoute& route) const {
        const HostAccount* const account = route.account.get();
        if (!account || account->oldestTimed == HostAccount::kNone) return std::nullopt;
        const Awaited& oldest = account->awaited[account->oldestTimed];
        const Clock::time_point due = oldest.at + answerWait_ + (account->owed - oldest.owedAt);
        if (account->owingSince && due > *account->owingSince) return std::nullopt;
        return due;
    }

    /// Whether `route`'s oldest timed message has used its answer time by `now` (`dueLocked`).
    bool overdueLocked(const HostRoute& route, Clock::time_point now) const {
        const std::optional<Clock::time_point> due = dueLocked(route);
        return due && *due <= now;
    }

    /// The earliest of the open routes' `dueLocked`, none when none is due.
    std::optional<Clock::time_point> earliestDueLocked() const {
        std::optional<Clock::time_point> earliest;
        for (const auto& entry : routes_) {
            const std::optional<Clock::time_point> due = dueLocked(*entry.second);
            if (due && (!earliest || *due < *earliest)) earliest = due;
        }
        return earliest;
    }

    /// The monitor's look at the answer times after each wake, from the same `dueLocked` as its wake and the end's decision, by the one rule (`lookAtAnswers`): when one ran out by now the stream ends (true); a look more than `kLateLook` after the wake the monitor planned concludes nothing and asks for a second look that long after; no look concludes before that one, which, late or not, judges every message with all the time it had and is spent.
    bool lookLocked() {
        const Clock::time_point now = Clock::now();
        const AnswerLook look = lookAtAnswers(earliestDueLocked(), wake_, recheck_, now);
        wake_.reset();
        recheck_ = look.recheck;
        if (!look.expired) return false;
        expireLocked(now);
        return true;
    }

    /// When the monitor wakes next: the current write's bound or, on an accounting stream, the second look a late one asked for or else the earliest answer time, whichever comes first; none while there is neither.
    std::optional<Clock::time_point> wakeLocked() const {
        std::optional<Clock::time_point> wake;
        if (writing_ && !ending_) wake = writingSince_ + stall_;
        if (ending_ || !accounting()) return wake;
        const std::optional<Clock::time_point> look = recheck_ ? recheck_ : earliestDueLocked();
        if (look && (!wake || *look < *wake)) wake = look;
        return wake;
    }

    /// `route`'s answer time may come earlier now (a message timed, the host's wait ended): the monitor wakes only when it comes before the wake it planned, so a renderer that answers in time costs no wake of its own.
    void wakeForLocked(const HostRoute& route) {
        const std::optional<Clock::time_point> due = dueLocked(route);
        if (due && (!wake_ || *due < *wake_)) watched_.notify_all();
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
    /// The owner's reading of the hosted widgets' platform messages (none on an engine stream), and how long a renderer may leave a timed one unanswered.
    const SandboxHostedAnswers answers_;
    const Clock::duration answerWait_ = durationOf(SandboxEngine::kPlatformAnswerSeconds * 1000);
    /// The first end's decision is published to the views of the routes it froze (`blameLocked`, once): shared with each route's view, owned apart from the connection.
    const std::shared_ptr<std::atomic<bool>> published_ = std::make_shared<std::atomic<bool>>(false);

    std::mutex mutex_;
    /// The writer waits on it for frames, the monitor for the rest.
    std::condition_variable work_;
    std::condition_variable watched_;
    std::shared_ptr<SandboxRemoteStream> held_;
    bool ending_ = false;
    /// The first end's instant (`endLocked`), one value for the routes and the owner: the one `blameLocked` decided their deaths at, and the one the owner's `stop` tells unless it was that end.
    Clock::time_point endedAt_;
    std::string reason_;
    /// The owner's stop ended it (`End::Stopped`): no route's death was decided.
    bool stopped_ = false;
    bool ready_ = false;
    /// Every open runtime's client heard the end: the monitor reports it.
    bool finalized_ = false;
    std::map<uint32_t, std::shared_ptr<HostRoute>> routes_;
    uint32_t lastId_ = 0;
    std::deque<Queued> queue_;
    size_t queuedBytes_ = 0;
    size_t queuedFrames_ = 0;
    /// One write blocks since `writingSince_`; while `timing_` the monitor wakes before a write that starts now could outlast its bound.
    bool writing_ = false;
    Clock::time_point writingSince_;
    bool timing_ = false;
    /// The wake the monitor planned for its wait (none: it waits until woken), until its look at the answer times after it (`lookLocked`); after a late look, when its second one comes, until a look at or after it spent it.
    std::optional<Clock::time_point> wake_;
    std::optional<Clock::time_point> recheck_;
};

/// A runtime's channel on the stream, an engine's or a hosted widget's (the opening decides which, so no call checks it): each call goes out as its frame, its forms coded as the loopback codes them (one that does not decode again, or a frame past the bound, loses the runtime); the destructor, on the owner's thread, closes and joins `widget.answer`.
class SandboxRemoteHostEnd::Connection::Channel final : public SandboxHostedChannel {
public:
    explicit Channel(std::shared_ptr<Connection> connection) : connection_(std::move(connection)) {
        if (connection_->accounting()) route_->account = std::make_unique<HostAccount>();
    }
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
    void setVisible(bool visible) override { send(Kind::Visible, {visible}, {.visible = visible}); }
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
        if (SandboxRemoteCoding::decodeConfig(coded) && SandboxRemoteCoding::decodeHostedStart(hosted)) send(Kind::HostedStart, {html, coded, hosted}, {.visible = config.visible, .start = true});
        else connection_->lose(route_);
    }
    void hostedTheme(const WidgetTheme& theme) override {
        const std::string coded = SandboxRemoteCoding::encodeTheme(theme);
        if (SandboxRemoteCoding::decodeTheme(coded)) send(Kind::HostedTheme, {coded});
        else connection_->lose(route_);
    }
    bool platform(const std::string& message) override {
        Sent sent;
        if (const SandboxHostedAnswers::Reading reading = connection_->readOf(message); reading.role == SandboxHostedAnswers::Role::Expects) sent.expects = reading;
        return send(Kind::Platform, {message}, sent);
    }
    void kept() override { connection_->keep(*route_); }
    Death death() const override { return death_.load(); }
    Withdrawal withdraw(std::function<void(Death death)> settled) override { return connection_->withdraw(*route_, std::move(settled)); }
    bool decided() const override { return connection_->decided(*route_); }
    /// The route's own view of what `decided` reads (`HostRoute::become` keeps it), shared with the owner past the route.
    std::shared_ptr<const SandboxHostedDecidedView> decidedView() const override { return route_->decidedView; }
    /// The host's own stop in one step with the end (`Connection::localStop`): the widget's own death claimed, or the decision of an end that came first, left as it is.
    std::optional<Death> localStop() override { return connection_->localStop(*route_); }

private:
    /// The stream's first end's decision of this route's death (`Connection::blameLocked`; `Ordinary` for a route it decided nothing of), published when its client reports that end as the widget's first death, which settles it.
    void publishEndedDeath() override { death_.store(connection_->settle(*route_)); }
    /// The widget's own death came first: a death the end froze settles `Ordinary`, and one before the end is claimed (`Connection::claim`).
    void publishOwnDeath() override { connection_->claim(*route_); }

    /// Encoded outside the lock once the route is live, then admitted only if it still is, with what it changes in the route's account (`Connection::admit`): whether the writer took the frame (false too for one past the frame bound, which loses the runtime).
    bool send(Kind kind, std::initializer_list<Field> fields, const Sent& sent = {}) {
        const std::optional<uint32_t> id = connection_->liveId(*route_);
        if (!id) return false;
        std::optional<std::string> frame = Codec::encode(*id, kind, SandboxRemoteWire::kMaxFrameBytes, fields);
        if (frame) return connection_->admit(*route_, std::move(*frame), sent);
        connection_->lose(route_);
        return false;
    }

    const std::shared_ptr<Connection> connection_;
    const std::shared_ptr<HostRoute> route_ = std::make_shared<HostRoute>(connection_->published());
    /// What `death` reads: `Ordinary` until the client published its first death's decision.
    std::atomic<Death> death_{Death::Ordinary};
};

SandboxRemoteHostEnd::SandboxRemoteHostEnd(std::shared_ptr<Connection> connection)
    : connection_(std::move(connection)), reader_(std::make_unique<JSThread>("widget.reader")), writer_(std::make_unique<JSThread>("widget.writer")),
      monitor_(std::make_unique<JSThread>("widget.monitor")) {
    writer_->async([connection = connection_] { connection->write(); });
    monitor_->async([connection = connection_] { connection->watch(); });
    reader_->async([connection = connection_] { connection->read(); });
}

std::shared_ptr<SandboxRemoteHostEnd> SandboxRemoteHostEnd::start(std::shared_ptr<SandboxRemoteStream> stream, const SandboxResources& resources, std::string build, Events events, Limits limits) {
    auto connection = std::make_shared<Connection>(std::move(stream), std::move(events), limits, false, SandboxHostedAnswers{});
    connection->open(resources, build);
    return std::shared_ptr<SandboxRemoteHostEnd>(new SandboxRemoteHostEnd(std::move(connection)));
}

std::shared_ptr<SandboxRemoteHostEnd> SandboxRemoteHostEnd::startHosting(std::shared_ptr<SandboxRemoteStream> stream, const SandboxResources& resources, std::string build, Events events, Limits limits,
                                                                         SandboxHostedAnswers answers) {
    auto connection = std::make_shared<Connection>(std::move(stream), std::move(events), limits, true, std::move(answers));
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

std::optional<Clock::time_point> SandboxRemoteHostEnd::stop() {
    const std::optional<Clock::time_point> endedAt = connection_->stop();
    reader_->join();
    writer_->join();
    monitor_->join();
    connection_->release();
    return endedAt;
}

SandboxRemoteHostEnd::AnswerLook SandboxRemoteHostEnd::lookAtAnswers(std::optional<Clock::time_point> due, std::optional<Clock::time_point> wake, std::optional<Clock::time_point> recheck,
                                                                     Clock::time_point now) {
    const bool ranOut = due && *due <= now;
    // The second look a late one asked for decides first, however late it comes itself.
    if (recheck) return now < *recheck ? AnswerLook{false, recheck} : AnswerLook{ranOut, std::nullopt};
    if (wake && now - *wake > kLateLook) return {false, now + kLateLook};
    return {ranOut, std::nullopt};
}

std::shared_ptr<SandboxRemoteChannel> SandboxRemoteHostEnd::refusing(std::string reason) { return std::make_shared<RefusingChannel>(std::move(reason)); }

std::shared_ptr<SandboxHostedChannel> SandboxRemoteHostEnd::refusingHosted(std::string reason) { return std::make_shared<RefusingChannel>(std::move(reason)); }

} // namespace twidget
