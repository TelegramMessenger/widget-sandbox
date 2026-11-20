// The hosted mode's test switch `TW_ENGINE_HOSTED=loopback|stream` of twidget-tests and twidget-run, test and harness code like stream_peers.h, never the library's: every WidgetHost built with a renderer factory while no engine channel factory is set runs hosted (remote_hosted.h), its engine, validator and glue behind a hosted loopback or a stream of its own and its fixture's renderer here behind a proxy; and the canned renderer of twidget-run's hosted runs.
#pragma once

#include "recording_host.h"
#include "stream_peers.h"
#include "twidget/json.h"
#include "twidget/remote_hosted.h"
#include "twidget/remote_stream.h"
#include "twidget/widget_host.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace twidget {

/// A kind's name as `SandboxRemoteWire::Kind` spells it, "kind N" for a number it does not have.
inline std::string hostedKindName(uint8_t kind) {
    using K = SandboxRemoteWire::Kind;
    switch (K(kind)) {
    case K::Opening: return "Opening";
    case K::Open: return "Open";
    case K::Start: return "Start";
    case K::Event: return "Event";
    case K::Activation: return "Activation";
    case K::AnimationFrame: return "AnimationFrame";
    case K::Viewport: return "Viewport";
    case K::Resize: return "Resize";
    case K::Theme: return "Theme";
    case K::SafeArea: return "SafeArea";
    case K::Data: return "Data";
    case K::Visible: return "Visible";
    case K::PressBegan: return "PressBegan";
    case K::PressEnded: return "PressEnded";
    case K::ChargeValidator: return "ChargeValidator";
    case K::Stop: return "Stop";
    case K::Answer: return "Answer";
    case K::Probe: return "Probe";
    case K::QueryAnswer: return "QueryAnswer";
    case K::IntlAnswer: return "IntlAnswer";
    case K::Close: return "Close";
    case K::HostingOpening: return "HostingOpening";
    case K::HostedStart: return "HostedStart";
    case K::HostedTheme: return "HostedTheme";
    case K::Platform: return "Platform";
    case K::PlatformAnswer: return "PlatformAnswer";
    case K::Ready: return "Ready";
    case K::Refused: return "Refused";
    case K::TaskStarted: return "TaskStarted";
    case K::TaskCompleted: return "TaskCompleted";
    case K::Ops: return "Ops";
    case K::Query: return "Query";
    case K::Intl: return "Intl";
    case K::Listen: return "Listen";
    case K::PreventDefault: return "PreventDefault";
    case K::Anim: return "Anim";
    case K::RequestsFrame: return "RequestsFrame";
    case K::Log: return "Log";
    case K::Diag: return "Diag";
    case K::Bridge: return "Bridge";
    case K::CopyText: return "CopyText";
    case K::Request: return "Request";
    case K::Wallet: return "Wallet";
    case K::Storage: return "Storage";
    case K::ResizeAnchor: return "ResizeAnchor";
    case K::WatchesPublicData: return "WatchesPublicData";
    case K::Loaded: return "Loaded";
    case K::Stats: return "Stats";
    case K::Died: return "Died";
    case K::ProbeAnswer: return "ProbeAnswer";
    case K::PlatformMessage: return "PlatformMessage";
    case K::PlatformRequest: return "PlatformRequest";
    case K::ValidatedOps: return "ValidatedOps";
    case K::RendererReady: return "RendererReady";
    case K::Rendered: return "Rendered";
    case K::Nodes: return "Nodes";
    }
    return "kind " + std::to_string(kind);
}

/// The frames one hosted stream carried, by kind in both directions (a host's kinds are below 64, an engine's from 64), read from their headers alone; and whether its widget asked for the validated batches at its hosted start.
struct HostedStreamTally {
    std::array<std::atomic<size_t>, 256> frames{};
    std::atomic<bool> asked{false};
    /// A header whose size is below 5 stopped the count of its direction.
    std::atomic<bool> malformed{false};
};

/// A stream that counts the frames passing through it into a `HostedStreamTally`, both ways, and passes every byte on unchanged (its one reader and one writer at a time are the wrapped stream's).
class FrameCountingStream final : public SandboxRemoteStream {
public:
    FrameCountingStream(std::shared_ptr<SandboxRemoteStream> inner, std::shared_ptr<HostedStreamTally> tally) : inner_(std::move(inner)), tally_(std::move(tally)) {}

    size_t read(char* out, size_t size) override {
        const size_t n = inner_->read(out, size);
        in_.feed(out, n, *tally_);
        return n;
    }
    bool write(std::string_view bytes) override {
        out_.feed(bytes.data(), bytes.size(), *tally_);
        return inner_->write(bytes);
    }
    void shutdown() override { inner_->shutdown(); }

private:
    /// One direction's frame boundaries: a header's 9 bytes (size, runtime, kind) counted, the rest of its frame skipped.
    struct Headers {
        unsigned char header[9] = {};
        size_t have = 0;
        uint64_t skip = 0;
        bool broken = false;

        void feed(const char* data, size_t n, HostedStreamTally& tally) {
            while (n && !broken) {
                if (skip) {
                    const size_t k = size_t(std::min<uint64_t>(skip, n));
                    skip -= k;
                    data += k;
                    n -= k;
                    continue;
                }
                header[have++] = static_cast<unsigned char>(*data++);
                n--;
                if (have < sizeof header) continue;
                have = 0;
                const uint32_t size = uint32_t(header[0]) | uint32_t(header[1]) << 8 | uint32_t(header[2]) << 16 | uint32_t(header[3]) << 24;
                if (size < 5) {
                    broken = true;
                    tally.malformed = true;
                    return;
                }
                tally.frames[header[8]]++;
                skip = size - 5;
            }
        }
    };

    const std::shared_ptr<SandboxRemoteStream> inner_;
    const std::shared_ptr<HostedStreamTally> tally_;
    Headers in_;
    Headers out_;
};

/// One widget of the switch: its fixture renderer, built at once with the fixture's own factory (so the fixture holds it when the WidgetHost's constructor returns), and the routes around it. Its input messages (`event`, `gesture`, `release`) go through the WidgetHost as an embedder's would: a `gesture` is `pressDelivered`, a `release` `releaseDelivered`, a trusted event that begins a new gesture with no `gesture` before it both, and each then goes on as `sendPlatform` to the proxy, which hands it to the engine end's glue; every other message goes to that glue directly, as the engine process's own renderer would send it.
class HostedSession : public std::enable_shared_from_this<HostedSession> {
public:
    HostedSession(WidgetHost& host, std::unique_ptr<Renderer> fixture) : host_(&host), fixture_(std::move(fixture)) {
        fixture_->onMessage = [this](const std::string& json) { fromRenderer(json); };
        fixture_->onDiag = [this](const std::string& kind, const std::string& detail) {
            std::lock_guard<std::recursive_mutex> l(glueMutex_);
            if (glue_ && glue_->onDiag) glue_->onDiag(kind, detail);
        };
    }
    HostedSession(const HostedSession&) = delete;
    HostedSession& operator=(const HostedSession&) = delete;

    Renderer& fixture() { return *fixture_; }

    /// The engine end's renderer, once (the widget has one hosted start): a proxy over the fixture renderer and `platform`.
    std::unique_ptr<Renderer> proxy(std::shared_ptr<SandboxHostedPlatform> platform);

    /// A message as the engine process's renderer sends it to its glue (the proxy's `onMessage`, the served WidgetHost's): dropped while there is no glue.
    void toGlue(const std::string& json) {
        std::lock_guard<std::recursive_mutex> l(glueMutex_);
        if (glue_ && glue_->onMessage) glue_->onMessage(json);
    }

    /// From the proxy's construction to its `destroy` (or destructor): the glue the fixture's messages go to.
    void attachGlue(Renderer* proxy) {
        std::lock_guard<std::recursive_mutex> l(glueMutex_);
        glue_ = proxy;
    }
    void detachGlue(Renderer* proxy) {
        std::lock_guard<std::recursive_mutex> l(glueMutex_);
        if (glue_ == proxy) glue_ = nullptr;
    }

    /// Before the WidgetHost goes (its client's channel is released inside its destructor, or kept past it by a kept renderer): no input reaches it from then on.
    void detachHost() {
        std::lock_guard<std::mutex> l(routeMutex_);
        host_ = nullptr;
    }

private:
    void fromRenderer(const std::string& json) {
        std::optional<Json> msg = Json::parse(json);
        const std::string type = msg ? (*msg)["type"].asString() : std::string();
        if (type != "event" && type != "gesture" && type != "release") return toGlue(json);
        std::lock_guard<std::mutex> l(routeMutex_);
        if (!host_) return;
        if (type == "gesture") {
            gestures_.pressBegan();
            claimed_ = true;
            host_->pressDelivered();
        } else if (type == "release") {
            claimed_ = false;
            host_->releaseDelivered();
        } else if (const Json* ev = msg->find("ev")) {
            // As WidgetHost numbers a renderer's events in process: any pointerdown begins a press, only a trusted event is a gesture.
            if ((*ev)["type"].isString() && (*ev)["type"].asString() == "pointerdown") gestures_.pressBegan();
            const int gesture = (*msg)["trusted"].asBool() ? gestures_.gestureFor(*ev) : 0;
            if (gesture > lastGesture_) {
                lastGesture_ = gesture;
                if (!std::exchange(claimed_, false)) {
                    host_->pressDelivered();
                    host_->releaseDelivered();
                }
            }
        }
        host_->sendPlatform(json);
    }

    std::mutex routeMutex_;
    WidgetHost* host_;
    GestureCounter gestures_;
    int lastGesture_ = 0;
    /// A `gesture` came and the press it began has no gesture number yet: the next new one is that press.
    bool claimed_ = false;
    /// Recursive: the glue answers a query from inside `send`, whose answer comes back here on the same thread.
    std::recursive_mutex glueMutex_;
    Renderer* glue_ = nullptr;
    bool proxied_ = false;
    std::unique_ptr<Renderer> fixture_;
};

/// The engine end's renderer of one switch widget: the glue's calls go to the fixture renderer, and the platform's messages (the inputs the WidgetHost delivered) to the glue as if the fixture renderer sent them.
class HostedProxyRenderer final : public Renderer {
public:
    HostedProxyRenderer(std::shared_ptr<HostedSession> session, std::shared_ptr<SandboxHostedPlatform> platform) : session_(std::move(session)), platform_(std::move(platform)) {
        session_->attachGlue(this);
        platform_->onMessage = [weak = std::weak_ptr<HostedSession>(session_)](const std::string& message) {
            if (std::shared_ptr<HostedSession> session = weak.lock()) session->toGlue(message);
        };
    }
    ~HostedProxyRenderer() override { session_->detachGlue(this); }

    void load() override { session_->fixture().load(); }
    void send(const std::string& json) override { session_->fixture().send(json); }
    void setTheme(const std::string& theme) override { session_->fixture().setTheme(theme); }
    void setWidth(double width) override { session_->fixture().setWidth(width); }
    void destroy() override {
        session_->fixture().destroy();
        session_->detachGlue(this);
    }

private:
    const std::shared_ptr<HostedSession> session_;
    const std::shared_ptr<SandboxHostedPlatform> platform_;
};

inline std::unique_ptr<Renderer> HostedSession::proxy(std::shared_ptr<SandboxHostedPlatform> platform) {
    {
        std::lock_guard<std::recursive_mutex> l(glueMutex_);
        if (std::exchange(proxied_, true)) return nullptr;
    }
    return std::make_unique<HostedProxyRenderer>(shared_from_this(), std::move(platform));
}

/// The test switch `TW_ENGINE_HOSTED=loopback|stream` of twidget-tests and twidget-run, in any build: its factory, installed with `WidgetHost::setHostedChannelFactory`, gives every WidgetHost built with a renderer factory while no engine channel factory is set a hosted channel, a `SandboxHostedLoopbackChannel` (`loopback`) or a stream of its own (`stream`: a memory pair, `SandboxRemoteHostEnd::startHosting` and `serveHosting` on a worker of its own, the engine's side counted by kind); the fixture's renderer is built at once on the constructing thread and its input goes through the WidgetHost (`HostedSession`). A widget's stream ends when its client releases the channel (inside the WidgetHost's destructor, on the owner's thread): its end stopped, its worker joined.
class HostedSwitch {
public:
    enum class Mode { Off, Loopback, Stream, Invalid };
    /// The kinds that stay inside the engine process in the hosted mode: none of them may cross a hosted stream.
    static constexpr SandboxRemoteWire::Kind kEngineOnlyKinds[] = {
        SandboxRemoteWire::Kind::Start, SandboxRemoteWire::Kind::Event, SandboxRemoteWire::Kind::Activation, SandboxRemoteWire::Kind::AnimationFrame,
        SandboxRemoteWire::Kind::Viewport, SandboxRemoteWire::Kind::Resize, SandboxRemoteWire::Kind::Theme, SandboxRemoteWire::Kind::PressBegan,
        SandboxRemoteWire::Kind::PressEnded, SandboxRemoteWire::Kind::ChargeValidator, SandboxRemoteWire::Kind::QueryAnswer, SandboxRemoteWire::Kind::Ops,
        SandboxRemoteWire::Kind::Query, SandboxRemoteWire::Kind::Listen, SandboxRemoteWire::Kind::PreventDefault, SandboxRemoteWire::Kind::Anim,
        SandboxRemoteWire::Kind::RequestsFrame,
    };

    /// What the environment asks for: unset or empty is `Off`, "loopback" and "stream" their modes, anything else `Invalid`.
    static Mode requested() {
        const char* value = std::getenv("TW_ENGINE_HOSTED");
        if (!value || !*value) return Mode::Off;
        if (std::string_view(value) == "loopback") return Mode::Loopback;
        if (std::string_view(value) == "stream") return Mode::Stream;
        return Mode::Invalid;
    }
    /// Why the environment cannot run (a usage error, exit 2): another value, or the switch with `TW_ENGINE_LOOPBACK=1` or `TW_ENGINE_STREAM=1`; "" when it can.
    static std::string usageError() {
        const Mode mode = requested();
        if (mode == Mode::Invalid) return "TW_ENGINE_HOSTED takes loopback or stream";
        if (mode == Mode::Off) return "";
        for (const char* other : {"TW_ENGINE_LOOPBACK", "TW_ENGINE_STREAM"}) {
            const char* value = std::getenv(other);
            if (value && std::string_view(value) == "1") return std::string("TW_ENGINE_HOSTED does not combine with ") + other + "=1";
        }
        return "";
    }
    /// "behind the loopback" or "over the stream", as the summaries say it.
    static const char* where(Mode mode) { return mode == Mode::Stream ? "over the stream" : "behind the loopback"; }
    /// True on this thread while a switch's factory calls a fixture's renderer factory: the renderer it builds is a hosted widget's.
    static bool building() { return buildingHere(); }
    /// How many times a switch's factory ran on the calling thread so far: compared around a WidgetHost's construction, whether it went through the switch.
    static size_t madeHere() { return made(); }

    /// `mode` is `Loopback` or `Stream`; both ends of each stream name `build`.
    HostedSwitch(Mode mode, std::string build) : state_(std::make_shared<State>(mode, std::move(build))) {}
    /// `finish`.
    ~HostedSwitch() { finish(); }
    HostedSwitch(const HostedSwitch&) = delete;
    HostedSwitch& operator=(const HostedSwitch&) = delete;

    Mode mode() const { return state_->mode; }
    /// The hook to install with `WidgetHost::setHostedChannelFactory`; a copy left after `finish` gives refusing channels.
    HostedChannelFactory factory() const {
        return [state = state_](WidgetHost& host, std::shared_ptr<SandboxResources> resources, const RendererFactory& renderers, const std::string& theme, double width) {
            made()++;
            return state->channel(host, std::move(resources), renderers, theme, width);
        };
    }
    /// The first time: clears the hook, then stops the streams of widgets still alive and joins their workers, on the calling thread. Every time: how each stream that ended otherwise ended, a host's end for another reason than its stop, an engine's end before it.
    std::vector<std::string> finish() { return state_->finish(); }
    /// The widgets it made so far.
    size_t widgets() const {
        std::lock_guard<std::mutex> l(state_->mutex);
        return state_->widgets;
    }
    /// The streams it opened so far (one per widget under `stream`).
    size_t streams() const {
        std::lock_guard<std::mutex> l(state_->mutex);
        return state_->tallies.size();
    }
    /// What its streams carried against the mode's rules: an engine-only kind (`kEngineOnlyKinds`), `ValidatedOps` on a stream whose widget did not ask, a malformed header; one line each.
    std::vector<std::string> trafficFaults() const {
        std::vector<std::string> faults;
        const std::vector<std::shared_ptr<HostedStreamTally>> tallies = state_->allTallies();
        for (size_t i = 0; i < tallies.size(); i++) {
            const HostedStreamTally& t = *tallies[i];
            const std::string stream = "stream " + std::to_string(i + 1) + ": ";
            for (SandboxRemoteWire::Kind kind : kEngineOnlyKinds)
                if (const size_t n = t.frames[uint8_t(kind)]) faults.push_back(stream + std::to_string(n) + " " + hostedKindName(uint8_t(kind)) + ", which stays in the engine process");
            if (const size_t n = t.frames[uint8_t(SandboxRemoteWire::Kind::ValidatedOps)]; n && !t.asked) faults.push_back(stream + std::to_string(n) + " ValidatedOps its widget did not ask for");
            if (t.malformed) faults.push_back(stream + "a frame header with a size below 5");
        }
        return faults;
    }
    /// Every kind its streams carried with its count, summed over the streams, and how many of them asked for the validated batches.
    std::string trafficText() const {
        const std::vector<std::shared_ptr<HostedStreamTally>> tallies = state_->allTallies();
        std::map<int, size_t> sums;
        size_t asked = 0;
        for (const std::shared_ptr<HostedStreamTally>& t : tallies) {
            for (size_t k = 0; k < t->frames.size(); k++)
                if (const size_t n = t->frames[k]) sums[int(k)] += n;
            if (t->asked) asked++;
        }
        std::string text;
        for (const auto& [kind, n] : sums) text += (text.empty() ? "" : ", ") + hostedKindName(uint8_t(kind)) + " " + std::to_string(n);
        size_t engineOnly = 0;
        for (SandboxRemoteWire::Kind kind : kEngineOnlyKinds) engineOnly += sums.count(int(kind)) ? sums[int(kind)] : 0;
        return (text.empty() ? std::string("no frames") : text) + "; " + std::to_string(engineOnly) + " of the engine-only kinds; validated ops asked on " + std::to_string(asked) + " of " +
               std::to_string(tallies.size()) + " streams";
    }

private:
    static size_t& made() {
        thread_local size_t calls = 0;
        return calls;
    }
    static bool& buildingHere() {
        thread_local bool building = false;
        return building;
    }
    /// `building()` while it lives, also when the factory throws.
    struct Building {
        Building() { buildingHere() = true; }
        ~Building() { buildingHere() = false; }
    };

    /// How the streams ended, from `widget.monitor` and the workers: it owns no thread, so its last owner may be any thread.
    struct Ends {
        std::mutex mutex;
        std::vector<std::string> unexpected;

        void ended(size_t stream, const std::string& why) {
            std::lock_guard<std::mutex> l(mutex);
            if (why != StreamSwitch::kStopped) unexpected.push_back("stream " + std::to_string(stream) + " ended: " + why);
        }
        void served(size_t stream, const std::string& why) {
            std::lock_guard<std::mutex> l(mutex);
            unexpected.push_back("the engine's end of stream " + std::to_string(stream) + " returned " + (why.empty() ? std::string("at the end of the stream") : why) + " before its stop");
        }
        std::vector<std::string> all() {
            std::lock_guard<std::mutex> l(mutex);
            return unexpected;
        }
    };

    /// One widget's stream: its host's end and the worker its engine's end serves on, stopped once by its channel or by `finish`.
    struct Stream {
        size_t number = 0;
        std::shared_ptr<HostedStreamTally> tally = std::make_shared<HostedStreamTally>();
        std::shared_ptr<SandboxRemoteHostEnd> end;
        std::thread serving;
        /// Set before the end is stopped: the worker's return after it is the stop's.
        std::shared_ptr<std::atomic<bool>> stopping = std::make_shared<std::atomic<bool>>(false);
        std::mutex mutex;
        bool stopped = false;

        void stop() {
            std::lock_guard<std::mutex> l(mutex);
            if (std::exchange(stopped, true)) return;
            *stopping = true;
            end->stop();
            if (serving.joinable()) serving.join();
        }
    };

    /// The channel a WidgetHost gets: the hosted loopback's or the stream's, every call passed on; at its end, inside the client's destructor on the owner's thread, the router lets the host go, the engine end's widget is destroyed and a stream is stopped with its worker joined.
    class Channel final : public SandboxHostedChannel {
    public:
        Channel(std::shared_ptr<SandboxHostedChannel> inner, std::shared_ptr<HostedSession> session, std::shared_ptr<Stream> stream)
            : inner_(std::move(inner)), session_(std::move(session)), stream_(std::move(stream)) {}
        ~Channel() override {
            session_->detachHost();
            inner_.reset();
            if (stream_) stream_->stop();
        }

        void attach(std::shared_ptr<SandboxRemoteSink> sink) override { inner_->attach(std::move(sink)); }
        void startHosted(const std::string& html, const SandboxConfig& config, const SandboxHostedStart& start) override {
            if (stream_) stream_->tally->asked = start.validatedOps;
            inner_->startHosted(html, config, start);
        }
        void hostedTheme(const WidgetTheme& theme) override { inner_->hostedTheme(theme); }
        bool platform(const std::string& message) override { return inner_->platform(message); }
        // The client is the kept renderer's from now on and its WidgetHost may go first: no fixture input reaches that host any more.
        void kept() override {
            session_->detachHost();
            inner_->kept();
        }
        void start(const std::string& html, const SandboxConfig& config) override { inner_->start(html, config); }
        void event(const std::string& json) override { inner_->event(json); }
        void activationEvent(const std::string& json, int gesture) override { inner_->activationEvent(json, gesture); }
        void frame(double time) override { inner_->frame(time); }
        void viewport(double innerWidth, double innerHeight, double dpr) override { inner_->viewport(innerWidth, innerHeight, dpr); }
        void resize(const std::string& entriesJson) override { inner_->resize(entriesJson); }
        void theme(const std::string& colorScheme, const std::map<std::string, std::string>& themeParams) override { inner_->theme(colorScheme, themeParams); }
        void contentSafeArea(const WidgetContentSafeArea& area) override { inner_->contentSafeArea(area); }
        void setData(const WidgetData& data) override { inner_->setData(data); }
        void setVisible(bool visible) override { inner_->setVisible(visible); }
        void pressBegan() override { inner_->pressBegan(); }
        void pressEnded() override { inner_->pressEnded(); }
        void chargeValidator(double ms) override { inner_->chargeValidator(ms); }
        void stop(const std::string& reason) override { inner_->stop(reason); }
        void answer(int64_t id, const SandboxRemoteOutcome& outcome) override { inner_->answer(id, outcome); }
        std::string probe(const std::string& code) override { return inner_->probe(code); }
        void close() override { inner_->close(); }

    private:
        std::shared_ptr<SandboxHostedChannel> inner_;
        const std::shared_ptr<HostedSession> session_;
        const std::shared_ptr<Stream> stream_;
    };

    struct State {
        State(Mode mode, std::string build) : mode(mode), build(std::move(build)) {}

        std::shared_ptr<SandboxHostedChannel> channel(WidgetHost& host, std::shared_ptr<SandboxResources> resources, const RendererFactory& renderers, const std::string& theme, double width) {
            {
                std::lock_guard<std::mutex> l(mutex);
                if (finished) return SandboxRemoteHostEnd::refusingHosted("the hosted switch finished");
            }
            if (!resources) return SandboxRemoteHostEnd::refusingHosted("no resources to open a hosted widget with");
            std::unique_ptr<Renderer> fixture;
            {
                Building building;
                fixture = renderers(resources, theme, width);
            }
            // What WidgetHost's local constructor throws for it.
            if (!fixture) throw std::runtime_error("WidgetHost: the renderer factory returned nothing");
            auto session = std::make_shared<HostedSession>(host, std::move(fixture));
            HostedRendererFactory proxies = [session](std::shared_ptr<SandboxResources>, const std::string&, double, std::shared_ptr<SandboxHostedPlatform> platform) {
                return session->proxy(std::move(platform));
            };
            if (mode != Mode::Stream) {
                {
                    std::lock_guard<std::mutex> l(mutex);
                    widgets++;
                }
                return std::make_shared<Channel>(std::make_shared<SandboxHostedLoopbackChannel>(std::move(resources), std::move(proxies)), std::move(session), nullptr);
            }
            auto stream = std::make_shared<Stream>();
            {
                std::lock_guard<std::mutex> l(mutex);
                widgets++;
                tallies.push_back(stream->tally);
                stream->number = tallies.size();
                live.push_back(stream);
            }
            MemoryStreamPair pair = memoryStreamPair(size_t(1) << 20);
            const size_t number = stream->number;
            stream->end = SandboxRemoteHostEnd::startHosting(pair.host, *resources, build, {nullptr, [ends = ends, number](const std::string& why) { ends->ended(number, why); }}, {});
            auto counted = std::make_shared<FrameCountingStream>(pair.engine, stream->tally);
            stream->serving = std::thread([counted, build = build, proxies = std::move(proxies), ends = ends, stopping = stream->stopping, number] {
                const std::string why = serveHostingEngineEnd(*counted, build, {}, proxies);
                if (!*stopping) ends->served(number, why);
            });
            return std::make_shared<Channel>(stream->end->hostedChannel(), std::move(session), std::move(stream));
        }

        std::vector<std::shared_ptr<HostedStreamTally>> allTallies() {
            std::lock_guard<std::mutex> l(mutex);
            return tallies;
        }

        std::vector<std::string> finish() {
            std::vector<std::weak_ptr<Stream>> left;
            {
                std::lock_guard<std::mutex> l(mutex);
                if (finished) return ends->all();
                finished = true;
                left.swap(live);
            }
            WidgetHost::setHostedChannelFactory(nullptr);
            for (const std::weak_ptr<Stream>& s : left)
                if (std::shared_ptr<Stream> stream = s.lock()) stream->stop();
            return ends->all();
        }

        const Mode mode;
        const std::string build;
        const std::shared_ptr<Ends> ends = std::make_shared<Ends>();
        std::mutex mutex;
        bool finished = false;
        size_t widgets = 0;
        std::vector<std::shared_ptr<HostedStreamTally>> tallies;
        std::vector<std::weak_ptr<Stream>> live;
    };

    const std::shared_ptr<State> state_;
};

/// twidget-run's renderer under the hosted switch (the fixture renderer behind the switch's proxy): it announces `ready` on load and answers every `query` with `RecordingHost::cannedAnswer`, as RecordingHost answers in process; it never answers a frame request and draws nothing.
struct CannedRenderer final : Renderer {
    void load() override { onMessage("{\"type\":\"ready\"}"); }
    void send(const std::string& json) override {
        // Only a query wants an answer: an ops batch is never parsed here.
        if (json.rfind("{\"type\":\"query\"", 0) != 0) return;
        std::optional<Json> m = Json::parse(json);
        if (!m) return;
        const Json* query = m->find("query");
        onMessage("{\"type\":\"result\",\"qid\":" + std::to_string((*m)["qid"].asInt()) + ",\"result\":" + (query ? RecordingHost::cannedAnswer(*query) : std::string("null")) + "}");
    }
    void setTheme(const std::string&) override {}
    void setWidth(double) override {}
    void destroy() override {}
};

} // namespace twidget
