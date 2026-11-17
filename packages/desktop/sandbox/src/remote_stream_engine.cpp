// The engine process's end of a stream (twidget/remote_stream.h): the loop `SandboxRemoteEngineEnd::serve` runs, and remote_hosted.cpp's `serveHosting` through `detail::serveStream`, on the thread that reads, each runtime's server or hosted runtime writing its calls as frames through its sink. A translation unit of its own, so a host's program, which links the host's end (remote_stream.cpp), links no server or engine through it, and the engine-only helper's `serve` no host's end.
#include "twidget/remote_stream.h"

#include "remote_internal.h"
#include "remote_wire.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
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

/// What a query that gets no answer returns to the engine, as a client's dead engine answers it.
constexpr const char* kDeadQuery = "{\"error\":\"dead\"}";
/// The death of a runtime whose `widget.probe` cannot start, before the refusal itself (which names the thread and the OS error): its server or hosted runtime is stopped with it, as the host's `Stop` stops one.
constexpr const char* kProbeUnstarted = "the engine process could not start the widget's probe thread";
/// Why serving ends when `widget.reaper` cannot start, before the refusal itself: a closed runtime cannot be destroyed off the reader.
constexpr const char* kReaperUnstarted = "the engine process could not start the thread that destroys closed runtimes";
/// Bytes of the host's text a refusal quotes.
constexpr size_t kQuotedBytes = 64;

std::string_view message(const std::string& s) { return cut(s, SandboxEngine::kRemoteMessageLimit); }

std::string inQuotes(std::string_view s) { return "\"" + std::string(s.substr(0, kQuotedBytes)) + (s.size() > kQuotedBytes ? "...\"" : "\""); }

std::string undecodable(const Frame& f) { return kindOn(f) + " with a form that does not decode"; }

/// Why the engine's end refuses a first frame, "" for an opening it serves: another kind (`HostingOpening` too unless `hosting`), another version or build, a stamp past `kMaxStampBytes` or of more than one line (never cut into another stamp); a hosting opening's only stamp is the renderer's, its validator checks the whole bundle's own.
std::string refusalOf(const Frame& f, const std::string& build, bool hosting) {
    const bool hosted = hosting && f.kind == Kind::HostingOpening;
    if (f.kind != Kind::Opening && !hosted) return "kind " + std::to_string(int(f.kind)) + " first, not the opening";
    if (f.i64 != SandboxRemoteWire::kVersion) return "stream version " + std::to_string(f.i64) + ", this end's is " + std::to_string(SandboxRemoteWire::kVersion);
    if (f.str[0] != build) return "build " + inQuotes(f.str[0]) + ", this end's is " + inQuotes(build);
    using Stamp = std::pair<std::string_view, std::string_view>;
    const std::vector<Stamp> stamps = hosted ? std::vector<Stamp>{{"renderer", f.str[3]}} : std::vector<Stamp>{{"validator", f.str[2]}, {"renderer", f.str[3]}};
    for (const auto& [name, stamp] : stamps) {
        if (stamp.size() > SandboxRemoteWire::kMaxStampBytes)
            return "a " + std::string(name) + " stamp of " + std::to_string(stamp.size()) + " bytes, past the bound of " + std::to_string(SandboxRemoteWire::kMaxStampBytes);
        if (stamp.find('\n') != std::string_view::npos) return "a " + std::string(name) + " stamp of more than one line";
    }
    return {};
}

/// The runtimes' resources from the opening alone, no file read: the sandbox bundle, then the validator's and renderer's stamp lines for `BundleStamp`'s checks at boot, or a hosting opening's whole validator bundle and the renderer's stamp line.
std::shared_ptr<SandboxResources> resourcesOf(const Frame& f) {
    auto resources = std::make_shared<SandboxResources>();
    resources->sandboxBundle = std::string(f.str[1]);
    resources->validatorBundle = std::string(f.str[2]);
    resources->rendererBundle = std::string(f.str[3]);
    return resources;
}

/// The host kinds only a hosting stream takes: a hosted widget's start, its whole theme and its renderer's platform traffic.
bool hostedKind(Kind kind) { return kind == Kind::HostedStart || kind == Kind::HostedTheme || kind == Kind::Platform || kind == Kind::PlatformAnswer; }

/// The host kinds a hosting stream takes on a runtime: its own and those a hosted widget shares with an engine; the engine's start, input, theme, presses, validator time and query answers are made inside the engine process.
bool hostingTakes(Kind kind) {
    switch (kind) {
    case Kind::Open:
    case Kind::SafeArea:
    case Kind::Data:
    case Kind::Visible:
    case Kind::Stop:
    case Kind::Answer:
    case Kind::Probe:
    case Kind::IntlAnswer:
    case Kind::Close: return true;
    default: return hostedKind(kind);
    }
}

/// The engine's writes: whole frames, one at a time under one lock; `breakOff` takes no lock, so the end of the stream never waits for a write blocked inside it.
class Output {
public:
    explicit Output(SandboxRemoteStream& stream) : stream_(stream) {}

    /// A frame of `kind` on `runtime`, encoded before the lock: false once the stream broke, a failed write breaking it.
    bool send(uint32_t runtime, Kind kind, std::initializer_list<Field> fields) {
        if (broken_) return false;
        const std::optional<std::string> frame = Codec::encode(runtime, kind, SandboxRemoteWire::kMaxFrameBytes, fields);
        if (!frame) return false;
        std::lock_guard<std::mutex> l(writing_);
        if (broken_) return false;
        if (stream_.write(*frame)) return true;
        breakOff();
        return false;
    }
    /// Every later `send` fails, then the stream's shutdown wakes a write and the read blocked in it.
    void breakOff() {
        broken_ = true;
        stream_.shutdown();
    }

private:
    SandboxRemoteStream& stream_;
    std::mutex writing_;
    std::atomic<bool> broken_{false};
};

/// One synchronous request of a runtime's at a time (its engine's query or Intl request; a hosted renderer's platform request in a slot of its own): marked before it is written, filled by the reader with an answer of its kind only, given up once killed (the runtime closed or the stream ended). Its lock is never held across a write or a call out.
class SyncSlot {
public:
    enum class Wait : uint8_t { None, Query, Intl, Platform };

    /// Before the request is written: false once killed.
    bool begin(Wait kind) {
        std::lock_guard<std::mutex> l(mutex_);
        if (killed_) return false;
        waiting_ = kind;
        answer_.reset();
        return true;
    }
    /// The request was not written: nothing waits.
    void cancel() {
        std::lock_guard<std::mutex> l(mutex_);
        waiting_ = Wait::None;
    }
    /// The answer to the request `begin` marked, nullopt once killed without one.
    std::optional<QueryAnswer> wait() {
        std::unique_lock<std::mutex> l(mutex_);
        cv_.wait(l, [&] { return answer_ || killed_; });
        waiting_ = Wait::None;
        return std::exchange(answer_, std::nullopt);
    }
    /// The reader's: an answer for a request of `kind` that waits for one, dropped otherwise.
    void fill(Wait kind, QueryAnswer answer) {
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (killed_ || waiting_ != kind || answer_) return;
            answer_ = std::move(answer);
        }
        cv_.notify_all();
    }
    void kill() {
        {
            std::lock_guard<std::mutex> l(mutex_);
            killed_ = true;
        }
        cv_.notify_all();
    }
    bool killed() const { return killed_; }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    Wait waiting_ = Wait::None;
    std::optional<QueryAnswer> answer_;
    std::atomic<bool> killed_{false};
};

static_assert(SandboxRemoteCoding::kStatsFields == 11);

/// A runtime's sink on the engine's side: each call of its server's or hosted runtime's goes out as its frame, every string cut to its limit and one byte more; a query or an Intl request waits for its answer in its slot, a hosted renderer's platform request in the second one (the two may wait at once, from their own threads), and once killed nothing more goes out.
class EngineSink final : public SandboxRemoteSink {
public:
    EngineSink(uint32_t runtime, std::shared_ptr<Output> output) : runtime_(runtime), output_(std::move(output)) {}

    void remoteTaskStarted() override { send(Kind::TaskStarted, {}); }
    void remoteTaskCompleted() override { send(Kind::TaskCompleted, {}); }
    void remoteOps(const std::string& json) override { send(Kind::Ops, {cut(json, SandboxEngine::kRemoteOpsLimit)}); }
    QueryAnswer remoteQuery(const std::string& json) override { return request(slot_, SyncSlot::Wait::Query, Kind::Query, message(json)).value_or(QueryAnswer{kDeadQuery, std::nullopt}); }
    std::string remoteIntl(const std::string& requestJson) override {
        std::optional<QueryAnswer> answer = request(slot_, SyncSlot::Wait::Intl, Kind::Intl, message(requestJson));
        return answer ? std::move(answer->json) : std::string();
    }
    void remoteListen(const std::string& json) override { send(Kind::Listen, {message(json)}); }
    void remotePreventDefault(const std::string& json) override { send(Kind::PreventDefault, {message(json)}); }
    void remoteAnim(const std::string& json) override { send(Kind::Anim, {message(json)}); }
    void remoteRequestsFrame() override { send(Kind::RequestsFrame, {}); }
    void remoteLog(const std::string& level, const std::string& text) override { send(Kind::Log, {message(level), message(text)}); }
    void remoteDiag(const std::string& kind, const std::string& detail) override { send(Kind::Diag, {message(kind), message(detail)}); }
    void remoteBridge(const std::string& method, const std::string& argsJson) override { send(Kind::Bridge, {message(method), message(argsJson)}); }
    void remoteCopyText(const std::string& text) override { send(Kind::CopyText, {message(text)}); }
    void remoteRequest(int64_t id, const std::string& data, bool wallet) override { send(Kind::Request, {id, message(data), wallet}); }
    void remoteWallet(int64_t id, const std::string& payload) override { send(Kind::Wallet, {id, message(payload)}); }
    void remoteStorage(const std::string& commitJson) override { send(Kind::Storage, {message(commitJson)}); }
    void remoteResizeAnchor(const std::string& edge) override { send(Kind::ResizeAnchor, {message(edge)}); }
    void remoteWatchesPublicData(bool on) override { send(Kind::WatchesPublicData, {on}); }
    void remoteLoaded() override { send(Kind::Loaded, {}); }
    void remoteStats(const SandboxStats& stats) override {
        const std::vector<double> v = SandboxRemoteCoding::encodeStats(stats);
        send(Kind::Stats, {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10]});
    }
    void remoteDied(const std::string& reason) override { send(Kind::Died, {message(reason)}); }
    /// No frame: a server never reports one, and the host's end tells its clients of a lost stream itself.
    void remoteDisconnected() override {}
    void remotePlatformMessage(const std::string& text) override { send(Kind::PlatformMessage, {platform(text)}); }
    std::optional<std::string> remotePlatformRequest(const std::string& text) override {
        std::optional<QueryAnswer> answer = request(platformSlot_, SyncSlot::Wait::Platform, Kind::PlatformRequest, platform(text));
        if (!answer) return std::nullopt;
        return std::move(answer->json);
    }
    void remoteValidatedOps(const std::string& json) override { send(Kind::ValidatedOps, {cut(json, SandboxEngine::kRemoteOpsLimit)}); }
    void remoteRendererReady() override { send(Kind::RendererReady, {}); }
    void remoteRendered() override { send(Kind::Rendered, {}); }
    void remoteNodes(int64_t nodes) override { send(Kind::Nodes, {nodes}); }

    /// The reader's: the host's answer to a request of `kind`, for the one this runtime waits for.
    void answer(SyncSlot::Wait kind, QueryAnswer answer) { slot_.fill(kind, std::move(answer)); }
    /// The reader's: the host's answer to the platform request this runtime's renderer waits for.
    void answerPlatform(std::string text) { platformSlot_.fill(SyncSlot::Wait::Platform, QueryAnswer{std::move(text), std::nullopt}); }
    /// A probe thread's: the server's answer to probe `seq`.
    void probed(int64_t seq, const std::string& text) { send(Kind::ProbeAnswer, {seq, message(text)}); }
    /// The runtime closed or the stream ended: a waiting request of either slot gets no answer, and nothing more goes out.
    void kill() {
        slot_.kill();
        platformSlot_.kill();
    }

private:
    static std::string_view platform(const std::string& s) { return cut(s, SandboxEngine::kRemotePlatformLimit); }

    bool send(Kind kind, std::initializer_list<Field> fields) { return !slot_.killed() && output_->send(runtime_, kind, fields); }

    /// Marked waiting in `slot` before its frame is written, so the reader finds it whenever the answer comes; nullopt once the runtime or the stream is gone.
    std::optional<QueryAnswer> request(SyncSlot& slot, SyncSlot::Wait wait, Kind kind, std::string_view text) {
        if (!slot.begin(wait)) return std::nullopt;
        if (!send(kind, {text})) {
            slot.cancel();
            return std::nullopt;
        }
        return slot.wait();
    }

    const uint32_t runtime_;
    const std::shared_ptr<Output> output_;
    SyncSlot slot_;
    SyncSlot platformSlot_;
};

/// One runtime on the engine's side: its server (an engine stream's) or its hosted runtime (a hosting stream's), the sink it writes through and, once probed, the thread its probes run on. The reader opens and closes it; `widget.reaper` destroys it, its probes' thread first, or the end of serving on the reader's thread once nothing waits for the reader.
struct Route {
    std::shared_ptr<EngineSink> sink;
    std::unique_ptr<SandboxRemoteServer> server;
    std::unique_ptr<detail::HostedRuntime> hosted;
    std::unique_ptr<JSThread> probes;

    /// A waiting request of either slot given up before the server or hosted runtime closes, so none of its threads waits for the reader any more. Idempotent.
    void close() {
        sink->kill();
        if (hosted) hosted->close();
        if (server) server->close();
    }
    /// After `close`, never on the runtime's own threads: the probes' thread joined, then the server or hosted runtime destroyed (joining theirs).
    void destroy() {
        probes.reset();
        server.reset();
        hosted.reset();
    }
};

/// `SandboxRemoteEngineEnd::serve` and `serveHosting` on the thread that reads, which alone keeps the runtimes by id and the reaper; the opening decides whether every runtime is an engine's server or a hosted widget's runtime.
class Serving {
public:
    Serving(SandboxRemoteStream& stream, std::string build, const std::function<std::string()>& lockdown, detail::HostedRuntimes hosted)
        : stream_(stream), build_(std::move(build)), lockdown_(lockdown), hosted_(std::move(hosted)), output_(std::make_shared<Output>(stream)) {}

    std::string run() {
        FrameReader reader(stream_, Side::Host);
        if (!reader.next(SandboxRemoteWire::kMaxOpeningBytes)) return refuse(reader.failure());
        if (std::string why = refusalOf(reader.frame(), build_, bool(hosted_)); !why.empty()) return refuse(why);
        hosting_ = reader.frame().kind == Kind::HostingOpening;
        resources_ = resourcesOf(reader.frame());
        if (lockdown_) {
            std::string why;
            try { why = lockdown_(); } catch (...) { why = "the engine process could not lock itself down"; }
            if (!why.empty()) return refuse(why);
        }
        output_->send(0, Kind::Ready, {SandboxRemoteWire::kVersion});
        std::string reason;
        while (reason.empty() && reader.next(SandboxRemoteWire::kMaxFrameBytes)) reason = dispatch(reader.frame());
        if (reason.empty()) reason = reader.failure();
        end();
        return reason;
    }

private:
    /// Before the opening is served: no runtime and no thread, and the stream is left as it is so the host can read the reason; "" (the stream ended before a frame) refuses nothing.
    std::string refuse(const std::string& why) {
        if (!why.empty()) output_->send(0, Kind::Refused, {why});
        return why;
    }

    /// One frame for the runtimes, in the host's order: why it cannot be accepted, "" when it was (a closed runtime's is dropped); a kind of the other mode ends serving whatever its runtime.
    std::string dispatch(const Frame& f) {
        if (f.kind == Kind::Opening || f.kind == Kind::HostingOpening) return "kind " + std::to_string(int(f.kind)) + " after the opening";
        if (hosting_ && !hostingTakes(f.kind)) return kindOn(f) + ", which a hosting stream does not take";
        if (!hosting_ && hostedKind(f.kind)) return kindOn(f) + ", which only a hosting stream takes";
        if (f.kind == Kind::Open) return open(f.runtime);
        const auto it = routes_.find(f.runtime);
        if (it == routes_.end()) return f.runtime > lastOpened_ ? kindOn(f) + ", which was never opened" : std::string();
        const std::shared_ptr<Route> route = it->second;
        switch (f.kind) {
        case Kind::Probe: probe(route, f.i64, std::string(f.str[0])); return {};
        case Kind::IntlAnswer: route->sink->answer(SyncSlot::Wait::Intl, QueryAnswer{std::string(f.str[0]), std::nullopt}); return {};
        case Kind::Close: {
            // A route the reaper could not take stays here for `end`.
            std::string why = retire(route);
            if (why.empty()) routes_.erase(it);
            return why;
        }
        default: break;
        }
        return hosting_ ? hostedInput(*route, f) : serverInput(*route, f);
    }

    /// An engine stream's input for its runtime's server, which keeps what comes before its start.
    std::string serverInput(Route& route, const Frame& f) {
        SandboxRemoteServer& server = *route.server;
        const auto text = [&](size_t i) { return std::string(f.str[i]); };
        switch (f.kind) {
        case Kind::Start:
            if (std::optional<SandboxConfig> config = SandboxRemoteCoding::decodeConfig(f.str[1])) server.start(text(0), *config);
            else return undecodable(f);
            break;
        case Kind::Event: server.event(text(0)); break;
        case Kind::Activation: server.activationEvent(text(0), int(f.i64)); break;
        case Kind::AnimationFrame: server.frame(f.f64[0]); break;
        case Kind::Viewport: server.viewport(f.f64[0], f.f64[1], f.f64[2]); break;
        case Kind::Resize: server.resize(text(0)); break;
        case Kind::Theme:
            if (std::optional<std::map<std::string, std::string>> params = SandboxRemoteCoding::decodeThemeParams(f.str[1])) server.theme(text(0), *params);
            else return undecodable(f);
            break;
        case Kind::SafeArea:
            if (std::optional<WidgetContentSafeArea> area = SandboxRemoteCoding::decodeSafeArea(f.str[0])) server.contentSafeArea(*area);
            else return undecodable(f);
            break;
        case Kind::Data:
            if (std::optional<WidgetData> data = SandboxRemoteCoding::decodeData(f.str[0])) server.setData(*data);
            else return undecodable(f);
            break;
        case Kind::Visible: server.setVisible(f.flag); break;
        case Kind::PressBegan: server.pressBegan(); break;
        case Kind::PressEnded: server.pressEnded(); break;
        case Kind::ChargeValidator: server.chargeValidator(f.f64[0]); break;
        case Kind::Stop: server.stop(text(0)); break;
        case Kind::Answer:
            if (std::optional<SandboxRemoteOutcome> outcome = SandboxRemoteCoding::decodeOutcome(f.str[0])) server.answer(f.i64, *outcome);
            else return undecodable(f);
            break;
        case Kind::QueryAnswer: {
            std::optional<std::vector<uint8_t>> binary;
            if (f.flag) binary.emplace(f.str[1].begin(), f.str[1].end());
            route.sink->answer(SyncSlot::Wait::Query, QueryAnswer{text(0), std::move(binary)});
            break;
        }
        default: break;
        }
        return {};
    }

    /// A hosting stream's input for its runtime, which keeps what comes before its start; the start builds the widget's engine, validator and renderer here, on the reader.
    std::string hostedInput(Route& route, const Frame& f) {
        detail::HostedRuntime& runtime = *route.hosted;
        const auto text = [&](size_t i) { return std::string(f.str[i]); };
        switch (f.kind) {
        case Kind::HostedStart: {
            std::optional<SandboxConfig> config = SandboxRemoteCoding::decodeConfig(f.str[1]);
            std::optional<SandboxHostedStart> start = SandboxRemoteCoding::decodeHostedStart(f.str[2]);
            if (!config || !start) return undecodable(f);
            runtime.start(text(0), *config, *start);
            break;
        }
        case Kind::HostedTheme:
            if (std::optional<WidgetTheme> theme = SandboxRemoteCoding::decodeTheme(f.str[0])) runtime.theme(*theme);
            else return undecodable(f);
            break;
        case Kind::Platform: runtime.platform(text(0)); break;
        case Kind::PlatformAnswer: route.sink->answerPlatform(text(0)); break;
        case Kind::SafeArea:
            if (std::optional<WidgetContentSafeArea> area = SandboxRemoteCoding::decodeSafeArea(f.str[0])) runtime.contentSafeArea(*area);
            else return undecodable(f);
            break;
        case Kind::Data:
            if (std::optional<WidgetData> data = SandboxRemoteCoding::decodeData(f.str[0])) runtime.setData(*data);
            else return undecodable(f);
            break;
        case Kind::Visible: runtime.setVisible(f.flag); break;
        case Kind::Stop: runtime.stop(text(0)); break;
        case Kind::Answer:
            if (std::optional<SandboxRemoteOutcome> outcome = SandboxRemoteCoding::decodeOutcome(f.str[0])) runtime.answer(f.i64, *outcome);
            else return undecodable(f);
            break;
        default: break;
        }
        return {};
    }

    /// A runtime whose id is above every earlier one, its server or hosted runtime not started.
    std::string open(uint32_t runtime) {
        if (runtime <= lastOpened_) return "kind 2 on runtime " + std::to_string(runtime) + ", not above the last opened runtime " + std::to_string(lastOpened_);
        lastOpened_ = runtime;
        auto route = std::make_shared<Route>();
        route->sink = std::make_shared<EngineSink>(runtime, output_);
        if (hosting_) route->hosted = hosted_(route->sink, resources_);
        else route->server = std::make_unique<SandboxRemoteServer>(route->sink, resources_);
        routes_.emplace(runtime, std::move(route));
        return {};
    }

    /// On the runtime's `widget.probe` thread, made at its first probe, so the reader goes on answering the queries the probe may wait for. A runtime whose `widget.probe` cannot start is stopped with why (`kProbeUnstarted`, then the refusal) as the host's `Stop` stops it, its renderer kept until `Close`, and the probe answered "ERROR no runtime" at once; a later probe tries the thread again.
    void probe(const std::shared_ptr<Route>& route, int64_t seq, std::string code) {
        if (!route->probes) {
            try {
                route->probes = std::make_unique<JSThread>("widget.probe");
            } catch (const std::exception& e) {
                const std::string why = std::string(kProbeUnstarted) + ": " + e.what();
                if (route->hosted) route->hosted->stop(why);
                else route->server->stop(why);
                route->sink->probed(seq, "ERROR no runtime");
                return;
            }
        }
        route->probes->async([route, seq, code = std::move(code)] { route->sink->probed(seq, route->hosted ? route->hosted->probe(code) : route->server->probe(code)); });
    }

    /// In the host's order, the route closed, then destroyed on `widget.reaper`, never on the reader or the runtime's own threads: "", or why serving cannot go on when the reaper cannot start (`kReaperUnstarted`, then the refusal), the route left to `end`.
    std::string retire(std::shared_ptr<Route> route) {
        route->close();
        if (!reaper_) {
            try {
                reaper_ = std::make_unique<JSThread>("widget.reaper");
            } catch (const std::exception& e) {
                return std::string(kReaperUnstarted) + ": " + e.what();
            }
        }
        reaper_->async([route = std::move(route)] { route->destroy(); });
        return {};
    }

    /// The writes stop and a blocked one wakes before any lock is taken, then every runtime left is closed before anything is joined (none of their threads waits for the reader any more), the reaper drained if one was made, and the rest destroyed on this thread: the end starts no thread.
    void end() {
        output_->breakOff();
        for (const auto& entry : routes_) entry.second->close();
        if (reaper_) reaper_->join();
        reaper_.reset();
        for (const auto& entry : routes_) entry.second->destroy();
        routes_.clear();
    }

    SandboxRemoteStream& stream_;
    const std::string build_;
    const std::function<std::string()>& lockdown_;
    /// Empty for `serve`, which refuses a hosting opening.
    const detail::HostedRuntimes hosted_;
    const std::shared_ptr<Output> output_;
    std::shared_ptr<SandboxResources> resources_;
    /// Whether the opening was `HostingOpening`.
    bool hosting_ = false;
    std::map<uint32_t, std::shared_ptr<Route>> routes_;
    uint32_t lastOpened_ = 0;
    std::unique_ptr<JSThread> reaper_;
};

} // namespace

std::string detail::serveStream(SandboxRemoteStream& stream, const std::string& build, const std::function<std::string()>& lockdown, const HostedRuntimes& hosted) {
    return Serving(stream, build, lockdown, hosted).run();
}

std::string SandboxRemoteEngineEnd::serve(SandboxRemoteStream& stream, const std::string& build, const std::function<std::string()>& lockdown) { return detail::serveStream(stream, build, lockdown, {}); }

} // namespace twidget
