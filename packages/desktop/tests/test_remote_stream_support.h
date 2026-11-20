// What test_remote_stream.cpp (the framed transport's tests), test_remote_stream_hosted.cpp (a hosting stream's) and test_hosted.cpp (the hosted mode's, for a probe over a hosting stream) share: the documented wire and the engine process's scripted end written by hand (test_stream_frames.h), both scripted peers (`ScriptedEngine` that end with the runner's bounds) and the real ends over a memory pair, the sinks, delegates and hosts on them, the hostile-frame run, a child case on a socket and, on macOS, the checks of a transport thread the OS refuses to start.
#pragma once

#include "test_support.h"
#include "test_remote_support.h"
#include "test_thread_limit.h"

#include "../harness/stream_peers.h"
#include "test_stream_frames.h"
#include "twidget/intl.h"
#include "twidget/json.h"
#include "twidget/remote_engine.h"
#include "twidget/remote_hosted.h"
#include "twidget/remote_stream.h"
#include "twidget/widget_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

// ───────────── thread names ─────────────

/// The calling thread's name, "" where the tests read no thread names.
inline std::string currentThreadName() {
#if defined(__APPLE__) || defined(__linux__)
    char name[64] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    return name;
#else
    return {};
#endif
}

// ───────────── the engine process's end ─────────────

/// The build both ends of the tests' streams name.
inline const std::string kBuild = "twidget-tests";

/// A bundle's first line without its newline, all of it when it has none: what an opening carries of the validator and renderer bundles.
inline std::string firstLine(const std::string& bundle) { return bundle.substr(0, bundle.find('\n')); }

inline std::string openingBytes(int64_t version, std::string_view build, std::string_view bundle, std::string_view validatorStamp, std::string_view rendererStamp) {
    return FrameBytes(0, wire::Opening).i64(version).str(build).str(bundle).str(validatorStamp).str(rendererStamp).bytes();
}

/// The opening a host writes for `res`: its sandbox bundle, the first lines of its validator and renderer bundles.
inline std::string openingOf(const SandboxResources& res) {
    return openingBytes(1, kBuild, res.sandboxBundle, firstLine(res.validatorBundle), firstLine(res.rendererBundle));
}

/// An opening for cases that boot no runtime: a sandbox bundle of one byte, the validator's stamp and no renderer.
inline std::string smallOpening() { return openingBytes(1, kBuild, "s", firstLine(RES->validatorBundle), ""); }

inline std::string hostingOpeningBytes(int64_t version, std::string_view build, std::string_view bundle, std::string_view validatorBundle, std::string_view rendererStamp) {
    return FrameBytes(0, wire::HostingOpening).i64(version).str(build).str(bundle).str(validatorBundle).str(rendererStamp).bytes();
}

/// The opening a host writes for `res`'s hosted widgets: its sandbox and validator bundles (a given validator bundle in its place), the first line of its renderer bundle.
inline std::string hostingOpeningOf(const SandboxResources& res, std::optional<std::string> validator = std::nullopt) {
    return hostingOpeningBytes(1, kBuild, res.sandboxBundle, validator.value_or(res.validatorBundle), firstLine(res.rendererBundle));
}

/// A hosting opening for cases that build no widget: bundles of one byte and no renderer.
inline std::string smallHostingOpening() { return hostingOpeningBytes(1, kBuild, "s", "v", ""); }

inline std::string rectOf(int width) {
    const std::string w = std::to_string(width);
    return "{\"x\":0,\"y\":0,\"width\":" + w + ",\"height\":20,\"top\":0,\"left\":0,\"right\":" + w + ",\"bottom\":20}";
}

/// An engine frame as the tests read it, for the kinds they look at: its kind and its strings, a log's level only when it is not "log", a probe's answer after its sequence and a node count.
inline std::string describe(const ParsedFrame& f) {
    FieldsOf in(f);
    const auto str = [&] { return in.str().value_or("?"); };
    switch (f.kind) {
    case wire::Ready: return "ready:" + std::to_string(in.i64().value_or(-1));
    case wire::Refused: return "refused:" + str();
    case wire::Query: return "query:" + str();
    case wire::Intl: return "intl:" + str();
    case wire::Log: {
        const std::string level = str();
        return (level == "log" ? std::string("log:") : level + ":") + str();
    }
    case wire::Diag: {
        const std::string kind = str();
        return "diag:" + kind + ":" + str();
    }
    case wire::Died: return "died:" + str();
    case wire::ProbeAnswer: {
        const int64_t seq = in.i64().value_or(-1);
        return "probe:" + std::to_string(seq) + ":" + str();
    }
    case wire::Loaded: return "loaded";
    case wire::PlatformMessage: return "platform:" + str();
    case wire::PlatformRequest: return "platform-request:" + str();
    case wire::ValidatedOps: return "validated-ops";
    case wire::RendererReady: return "renderer-ready";
    case wire::Rendered: return "rendered";
    case wire::Nodes: return "nodes:" + std::to_string(in.i64().value_or(-1));
    default: return "kind " + std::to_string(int(f.kind));
    }
}

/// The engine's side of a pair as its end reads it: the bytes and the reads that came, so a test sees when a write of its own was read, and in how many reads, and the first frame as it came.
class WatchedStream final : public SandboxRemoteStream {
public:
    explicit WatchedStream(std::shared_ptr<SandboxRemoteStream> stream) : stream_(std::move(stream)) {}

    size_t read(char* out, size_t size) override {
        const size_t n = stream_->read(out, size);
        if (n) reads_++;
        keepFirst(std::string_view(out, n));
        bytes_ += n;
        return n;
    }
    bool write(std::string_view bytes) override { return stream_->write(bytes); }
    void shutdown() override { stream_->shutdown(); }

    size_t bytesRead() const { return bytes_; }
    /// Counted before the bytes, so it is current once `bytesRead` is.
    size_t reads() const { return reads_; }
    /// The first frame read, by the documented layout, once all of it was (a size past the opening's bound is kept to that bound).
    std::optional<ParsedFrame> firstFrame() {
        std::lock_guard<std::mutex> l(mutex_);
        if (first_.size() < 4) return std::nullopt;
        return frameAt(first_, uint32_t(wire::little(std::string_view(first_).substr(0, 4))));
    }

private:
    void keepFirst(std::string_view bytes) {
        std::lock_guard<std::mutex> l(mutex_);
        while (!bytes.empty()) {
            size_t whole = 4;
            if (first_.size() >= 4) whole += std::min<size_t>(wire::little(std::string_view(first_).substr(0, 4)), SandboxRemoteWire::kMaxOpeningBytes);
            if (first_.size() >= whole) return;
            const size_t take = std::min(bytes.size(), whole - first_.size());
            first_.append(bytes.substr(0, take));
            bytes.remove_prefix(take);
        }
    }

    const std::shared_ptr<SandboxRemoteStream> stream_;
    std::atomic<size_t> reads_{0};
    std::atomic<size_t> bytes_{0};
    std::mutex mutex_;
    std::string first_;
};

/// The host's side of a stream written by hand from the documented wire, the engine's end serving the other side on a worker of the test's (`serveHosting` with `renderers` when they are given), with `lockdown` when one is given: each frame it reads is kept in order, and `answer` gives the frame that answers one (a query, an Intl request), written as soon as it was read.
class ScriptedHost {
public:
    using Answer = std::function<std::optional<std::string>(const ParsedFrame&)>;

    explicit ScriptedHost(Answer answer = nullptr, std::function<std::string()> lockdown = nullptr, HostedRendererFactory renderers = nullptr)
        : answer_(std::move(answer)), serving_([this, lockdown, renderers = std::move(renderers)] {
              return renderers ? serveHostingEngineEnd(*engine_, kBuild, lockdown, renderers) : serveEngineEnd(*engine_, kBuild, lockdown);
          }),
          reading_([this] { return read(); }) {}
    /// The pair shut down, the engine's end and the reader joined.
    ~ScriptedHost() {
        pair_.host->shutdown();
        serving_.finish(5);
        reading_.finish(5);
    }
    ScriptedHost(const ScriptedHost&) = delete;
    ScriptedHost& operator=(const ScriptedHost&) = delete;

    /// `bytes` in one write, after everything written before.
    bool send(std::string_view bytes) {
        std::lock_guard<std::mutex> l(writing_);
        if (!pair_.host->write(bytes)) return false;
        sent_ += bytes.size();
        return true;
    }
    size_t sent() const { return sent_; }
    /// As a host closes its socket: the engine's end reads what was written, then the end.
    void close() { pair_.host->close(); }
    /// Why the engine's end stopped serving, once it did within `seconds`.
    std::optional<std::string> served(double seconds) { return serving_.within(seconds); }
    WatchedStream& engineSide() { return *engine_; }

    /// The frames read so far on `runtime` (0: the connection), as `describe` gives them.
    std::vector<std::string> events(uint32_t runtime) {
        std::lock_guard<std::mutex> l(mutex_);
        std::vector<std::string> out;
        for (const ParsedFrame& f : frames_)
            if (f.runtime == runtime) out.push_back(describe(f));
        return out;
    }
    std::vector<std::string> eventsWith(uint32_t runtime, const std::string& prefix) {
        std::vector<std::string> out;
        for (const std::string& e : events(runtime))
            if (e.rfind(prefix, 0) == 0) out.push_back(e);
        return out;
    }
    bool saw(uint32_t runtime, const std::string& event) {
        const std::vector<std::string> all = events(runtime);
        return std::find(all.begin(), all.end(), event) != all.end();
    }
    size_t frameCount() {
        std::lock_guard<std::mutex> l(mutex_);
        return frames_.size();
    }
    /// A probe of `runtime` with the next sequence of its own, from the test's thread: its answer within 5 s, nullopt without one.
    std::optional<std::string> probe(uint32_t runtime, const std::string& code) {
        const int64_t seq = ++probes_[runtime];
        if (!send(FrameBytes(runtime, wire::Probe).i64(seq).str(code).bytes())) return std::nullopt;
        const std::string prefix = "probe:" + std::to_string(seq) + ":";
        std::optional<std::string> answer;
        eventually([&] {
            const std::vector<std::string> answers = eventsWith(runtime, prefix);
            if (!answers.empty()) answer = answers.front().substr(prefix.size());
            return answer.has_value();
        });
        return answer;
    }

private:
    bool read() {
        FrameStreamReader reader(*pair_.host);
        while (std::optional<ParsedFrame> frame = reader.next()) {
            const std::optional<std::string> reply = answer_ ? answer_(*frame) : std::nullopt;
            {
                std::lock_guard<std::mutex> l(mutex_);
                frames_.push_back(std::move(*frame));
            }
            if (reply) send(*reply);
        }
        return true;
    }

    MemoryStreamPair pair_ = memoryStreamPair(size_t(1) << 20);
    const std::shared_ptr<WatchedStream> engine_ = std::make_shared<WatchedStream>(pair_.engine);
    const Answer answer_;
    std::mutex writing_;
    std::atomic<size_t> sent_{0};
    std::mutex mutex_;
    std::vector<ParsedFrame> frames_;
    std::map<uint32_t, int64_t> probes_;
    Background<std::string> serving_;
    Background<bool> reading_;
};

/// Runtime 1's widget: three layout queries and an Intl request at boot, with the safe area it was given before its start.
inline const char* kQueriesA = "<p id='a'>a</p><p id='b'>b</p><p id='c'>c</p><script>var w = ['a', 'b', 'c'].map(function (id) { return document.getElementById(id).getBoundingClientRect().width; });"
                               "console.log('A ' + TelegramWidget.contentSafeAreaInset.bottom + ' ' + w.join(',') + ' ' + (1234.5).toLocaleString());</script>";
/// Runtime 2's widget: a layout query and an Intl request at boot, another query on a tap.
inline const char* kQueriesB = "<button id='b'>go</button><p id='p'>x</p><script>var p = document.getElementById('p');"
                               "console.log('B ' + p.getBoundingClientRect().width + ' ' + (1234.5).toLocaleString());"
                               "document.getElementById('b').onclick = function () { p.textContent = 'yy'; console.log('B again ' + p.getBoundingClientRect().width); };</script>";

/// The scripted host's answers: runtime 1's layout queries get widths from 77 up, the first with binary bytes, the second with empty ones and the rest with none; another runtime's from 33 up; Intl is "INTL-A" for runtime 1 and "INTL-B" for the others.
inline ScriptedHost::Answer layoutAnswers() {
    auto asked = std::make_shared<std::map<uint32_t, int>>();
    return [asked](const ParsedFrame& f) -> std::optional<std::string> {
        if (f.kind == wire::Intl) return FrameBytes(f.runtime, wire::IntlAnswer).str(f.runtime == 1 ? "INTL-A" : "INTL-B").bytes();
        if (f.kind != wire::Query) return std::nullopt;
        std::optional<Json> query = Json::parse(FieldsOf(f).str().value_or(""));
        if (!query || (*query)["q"].asString() != "rect") return FrameBytes(f.runtime, wire::QueryAnswer).str("null").boolean(0).str("").bytes();
        const int n = (*asked)[f.runtime]++;
        FrameBytes answer(f.runtime, wire::QueryAnswer);
        answer.str(rectOf((f.runtime == 1 ? 77 : 33) + n));
        if (f.runtime == 1 && n == 0) return answer.boolean(1).str(std::string("\x01\0\xFF", 3)).bytes();
        if (f.runtime == 1 && n == 1) return answer.boolean(1).str("").bytes();
        return answer.boolean(0).str("").bytes();
    };
}

// ───────────── the host's end ─────────────

/// Resources for a host's end whose engine boots no runtime: a sandbox bundle of one byte, the shipped validator (only its stamp crosses) and no renderer.
inline const SandboxResources& smallResources() {
    static const SandboxResources res = [] {
        SandboxResources r;
        r.sandboxBundle = "s";
        r.validatorBundle = RES->validatorBundle;
        return r;
    }();
    return res;
}

/// What a host's end tells its owner, in order with the name of the thread it came on and when it came: "runtimes:N" and "ended:reason".
class EndEvents {
public:
    /// Set (`holdAt` first) before the end reports `holdAt` open runtimes: that report waits on `hold`, on `widget.monitor` outside every lock of the end, as a monitor that does not run; the test opens it before the end goes.
    std::atomic<size_t> holdAt{0};
    std::atomic<Gate*> hold{nullptr};

    /// The end keeps them: this outlives it.
    SandboxRemoteHostEnd::Events events() {
        return {[this](size_t open) {
                    if (Gate* gate = hold.load(); gate && open == holdAt.load()) gate->wait();
                    record("runtimes:" + std::to_string(open));
                },
                [this](const std::string& why) { record("ended:" + why); }};
    }
    std::vector<std::string> all() {
        std::lock_guard<std::mutex> l(mutex_);
        return events_;
    }
    std::vector<std::string> threads() {
        std::lock_guard<std::mutex> l(mutex_);
        return threads_;
    }
    /// Why the stream ended, nullopt before `ended` came.
    std::optional<std::string> ended() {
        for (const std::string& e : all())
            if (e.rfind("ended:", 0) == 0) return e.substr(6);
        return std::nullopt;
    }
    /// When `ended` came, nullopt before it.
    std::optional<std::chrono::steady_clock::time_point> endedAt() {
        std::lock_guard<std::mutex> l(mutex_);
        for (size_t i = 0; i < events_.size(); i++)
            if (events_[i].rfind("ended:", 0) == 0) return times_[i];
        return std::nullopt;
    }
    /// The last count reported, 0 before one.
    size_t open() {
        size_t last = 0;
        for (const std::string& e : all())
            if (e.rfind("runtimes:", 0) == 0) last = size_t(std::stoul(e.substr(9)));
        return last;
    }

private:
    void record(const std::string& event) {
        std::lock_guard<std::mutex> l(mutex_);
        events_.push_back(event);
        threads_.push_back(currentThreadName());
        times_.push_back(std::chrono::steady_clock::now());
    }

    std::mutex mutex_;
    std::vector<std::string> events_;
    std::vector<std::string> threads_;
    std::vector<std::chrono::steady_clock::time_point> times_;
};

/// Whether `events` end as a stream's must: `ended` with `reason` once and last, right after a count of 0 when any count came.
inline bool endsOnce(const std::vector<std::string>& events, const std::string& reason) {
    if (events.empty() || events.back() != "ended:" + reason) return false;
    if (std::count_if(events.begin(), events.end(), [](const std::string& e) { return e.rfind("ended:", 0) == 0; }) != 1) return false;
    return events.size() == 1 || events[events.size() - 2] == "runtimes:0";
}

/// A bare sink on a host's channel, where the transport hands over: each call as text in order, with its thread; `remoteQuery` (a 77 px rect) and `remoteIntl` (`intlAnswer`) wait on `gate` while it is set, a hosted runtime's `remotePlatformRequest` (`platformAnswer`) on `platformGate` (only the request `platformGated` when that is set), a log `hold` on `logGate` (the reader's own delivery), and `remoteDisconnected` on `lost`, noted once it returns.
class StreamSink final : public SandboxRemoteSink {
public:
    /// Set before a synchronous call comes; the test opens it before the channel goes.
    Gate* gate = nullptr;
    /// Set before the loss comes; the test opens it before the stream or the channel goes.
    Gate* lost = nullptr;
    std::string intlAnswer = "INTL";
    /// Set before a platform request comes; the test opens it before the channel goes.
    Gate* platformGate = nullptr;
    /// Set with it, before the channel is attached: only a request of this text waits on it (empty: every request), so a test that holds a later request never touches the gate while an earlier one may read it.
    std::string platformGated;
    std::optional<std::string> platformAnswer = "ANSWER";
    /// Set before the log comes: the delivery of a log whose text is `hold` waits on it on `widget.reader`, outside every lock of the end, as a sink busy in a delivery; the test opens it before the stream goes.
    Gate* logGate = nullptr;

    void remoteTaskStarted() override { record("begin"); }
    void remoteTaskCompleted() override { record("end"); }
    void remoteOps(const std::string& json) override { record("ops:" + json); }
    QueryAnswer remoteQuery(const std::string& json) override {
        record("query:" + json);
        if (gate) gate->wait();
        record("query-done");
        return {rectOf(77), std::nullopt};
    }
    std::string remoteIntl(const std::string& requestJson) override {
        record("intl:" + requestJson);
        if (gate) gate->wait();
        record("intl-done");
        return intlAnswer;
    }
    void remoteListen(const std::string& json) override { record("listen:" + json); }
    void remotePreventDefault(const std::string& json) override { record("preventDefault:" + json); }
    void remoteAnim(const std::string& json) override { record("anim:" + json); }
    void remoteRequestsFrame() override { record("frame"); }
    void remoteLog(const std::string& level, const std::string& text) override {
        if (logGate && text == "hold") logGate->wait();
        record("log:" + level + ":" + text);
    }
    void remoteDiag(const std::string& kind, const std::string& detail) override { record("diag:" + kind + ":" + detail); }
    void remoteBridge(const std::string& method, const std::string& argsJson) override { record("bridge:" + method + ":" + argsJson); }
    void remoteCopyText(const std::string& text) override { record("copy:" + text); }
    void remoteRequest(int64_t id, const std::string& data, bool wallet) override { record("request:" + std::to_string(id) + ":" + data + (wallet ? ":wallet" : "")); }
    void remoteWallet(int64_t id, const std::string& payload) override { record("wallet:" + std::to_string(id) + ":" + payload); }
    void remoteStorage(const std::string& commitJson) override { record("storage:" + commitJson); }
    void remoteResizeAnchor(const std::string& edge) override { record("anchor:" + edge); }
    void remoteWatchesPublicData(bool on) override { record(on ? "watch:true" : "watch:false"); }
    void remoteLoaded() override { record("loaded"); }
    void remoteStats(const SandboxStats& stats) override { record("stats:" + std::to_string(stats.tasks)); }
    void remoteDied(const std::string& reason) override { record("died:" + reason); }
    void remoteDisconnected() override {
        if (lost) lost->wait();
        record("disconnected");
    }
    void remotePlatformMessage(const std::string& message) override { record("platform:" + message.substr(0, 64)); }
    std::optional<std::string> remotePlatformRequest(const std::string& message) override {
        record("platform-request:" + message.substr(0, 64));
        if (platformGate && (platformGated.empty() || message == platformGated)) platformGate->wait();
        record("platform-request-done");
        return platformAnswer;
    }
    void remoteValidatedOps(const std::string&) override { record("validated-ops"); }
    void remoteRendererReady() override { record("renderer-ready"); }
    void remoteRendered() override { record("rendered"); }
    void remoteNodes(int64_t nodes) override { record("nodes:" + std::to_string(nodes)); }

    /// An event of the test's own among the sink's (what a probe answered), with its thread.
    void record(const std::string& event) {
        std::lock_guard<std::mutex> l(mutex_);
        events_.push_back(event);
        threads_.emplace_back(std::this_thread::get_id(), currentThreadName());
    }
    std::vector<std::string> events() {
        std::lock_guard<std::mutex> l(mutex_);
        return events_;
    }
    bool saw(const std::string& event) {
        const std::vector<std::string> all = events();
        return std::find(all.begin(), all.end(), event) != all.end();
    }
    /// The thread of the first event that starts with `prefix` and its name, none before it came.
    std::pair<std::thread::id, std::string> threadOf(const std::string& prefix) {
        std::lock_guard<std::mutex> l(mutex_);
        for (size_t i = 0; i < events_.size(); i++)
            if (events_[i].rfind(prefix, 0) == 0) return threads_[i];
        return {};
    }

private:
    std::mutex mutex_;
    std::vector<std::string> events_;
    std::vector<std::pair<std::thread::id, std::string>> threads_;
};

/// What a transport sees of a client's sink: every call goes on to `inner`, a log after waiting at `gate` while one is set (`passed` counts the logs that went on), and a hosted runtime's platform request of the text `heldRequest` after waiting at `requestGate`, as a transport thread that lost the processor before it reached the client, and its answer back after waiting at `answerGate`, as one that lost it after the client answered; `requests()` notes each request's text as it comes and "<text>:returned" once `inner` returned it.
class HeldSink final : public SandboxRemoteSink {
public:
    explicit HeldSink(std::shared_ptr<SandboxRemoteSink> inner) : inner_(std::move(inner)) {}

    std::atomic<Gate*> gate{nullptr};
    std::atomic<int> passed{0};
    /// Set before the sink is attached; the test opens the gates before the channel goes: `requestGate` holds `heldRequest` on its way to the client, `answerGate` its answer on its way back to the stream, once the client returned it.
    std::string heldRequest;
    Gate* requestGate = nullptr;
    Gate* answerGate = nullptr;

    void remoteTaskStarted() override { inner_->remoteTaskStarted(); }
    void remoteTaskCompleted() override { inner_->remoteTaskCompleted(); }
    void remoteOps(const std::string& json) override { inner_->remoteOps(json); }
    QueryAnswer remoteQuery(const std::string& json) override { return inner_->remoteQuery(json); }
    std::string remoteIntl(const std::string& requestJson) override { return inner_->remoteIntl(requestJson); }
    void remoteListen(const std::string& json) override { inner_->remoteListen(json); }
    void remotePreventDefault(const std::string& json) override { inner_->remotePreventDefault(json); }
    void remoteAnim(const std::string& json) override { inner_->remoteAnim(json); }
    void remoteRequestsFrame() override { inner_->remoteRequestsFrame(); }
    void remoteLog(const std::string& level, const std::string& text) override {
        if (Gate* g = gate) g->wait();
        passed++;
        inner_->remoteLog(level, text);
    }
    void remoteDiag(const std::string& kind, const std::string& detail) override { inner_->remoteDiag(kind, detail); }
    void remoteBridge(const std::string& method, const std::string& argsJson) override { inner_->remoteBridge(method, argsJson); }
    void remoteCopyText(const std::string& text) override { inner_->remoteCopyText(text); }
    void remoteRequest(int64_t id, const std::string& data, bool wallet) override { inner_->remoteRequest(id, data, wallet); }
    void remoteWallet(int64_t id, const std::string& payload) override { inner_->remoteWallet(id, payload); }
    void remoteStorage(const std::string& commitJson) override { inner_->remoteStorage(commitJson); }
    void remoteResizeAnchor(const std::string& edge) override { inner_->remoteResizeAnchor(edge); }
    void remoteWatchesPublicData(bool on) override { inner_->remoteWatchesPublicData(on); }
    void remoteLoaded() override { inner_->remoteLoaded(); }
    void remoteStats(const SandboxStats& stats) override { inner_->remoteStats(stats); }
    void remoteDied(const std::string& reason) override { inner_->remoteDied(reason); }
    void remoteDisconnected() override { inner_->remoteDisconnected(); }
    void remotePlatformMessage(const std::string& message) override { inner_->remotePlatformMessage(message); }
    std::optional<std::string> remotePlatformRequest(const std::string& message) override {
        note(message);
        if (requestGate && message == heldRequest) requestGate->wait();
        std::optional<std::string> answer = inner_->remotePlatformRequest(message);
        note(message + ":returned");
        if (answerGate && message == heldRequest) answerGate->wait();
        return answer;
    }
    void remoteValidatedOps(const std::string& json) override { inner_->remoteValidatedOps(json); }
    void remoteRendererReady() override { inner_->remoteRendererReady(); }
    void remoteRendered() override { inner_->remoteRendered(); }
    void remoteNodes(int64_t nodes) override { inner_->remoteNodes(nodes); }

    std::vector<std::string> requests() {
        std::lock_guard<std::mutex> l(mutex_);
        return requests_;
    }

private:
    void note(const std::string& request) {
        std::lock_guard<std::mutex> l(mutex_);
        requests_.push_back(request);
    }

    const std::shared_ptr<SandboxRemoteSink> inner_;
    std::mutex mutex_;
    std::vector<std::string> requests_;
};

/// The engine process's side of a stream written by hand from the documented wire, against a real host's end (opened for hosted widgets with `hosting`, with the owner's `answers` when given): the test runner's wrapper of `ScriptedEngineEnd` (test_stream_frames.h), whose worker keeps each frame the host writes in order while the test writes the engine's frames, opened with the tests' resources and build, its end's reports in `EndEvents`, the opening waited for 5 s and a held reading on a `Gate`; the destructor stops the end, then joins the worker within 5 s, past which the run stops (`stopTheRun`), as a `Background` worker's would.
class ScriptedEngine {
public:
    explicit ScriptedEngine(const SandboxRemoteHostEnd::Limits& limits = {}, bool hosting = false, SandboxHostedAnswers answers = {})
        : end_(smallResources(), kBuild, events_.events(), limits, hosting, std::move(answers)) {}
    ~ScriptedEngine() {
        if (!end_.finish(std::chrono::seconds(5))) stopTheRun(5, std::source_location::current());
    }
    ScriptedEngine(const ScriptedEngine&) = delete;
    ScriptedEngine& operator=(const ScriptedEngine&) = delete;

    SandboxRemoteHostEnd& host() { return end_.host(); }
    EndEvents& events() { return events_; }
    /// `bytes` in one write, after everything written before.
    bool send(std::string_view bytes) { return end_.send(bytes); }
    /// Once the opening came, its answer.
    bool ready() { return end_.ready(std::chrono::seconds(5)) == ScriptedEngineEnd::Handshake::Answered; }
    /// As an engine process that exits: the host's end reads what was written, then the end of the stream.
    void close() { end_.close(); }
    /// As an engine process that stops reading: once the worker has the frame it is reading it reads none until `gate` opens, so the host's writer blocks once the pair is full; the test opens it before this goes.
    void holdReading(Gate* gate) { end_.holdReading(gate ? std::function<void()>([gate] { gate->wait(); }) : nullptr); }
    /// Whether the host's writer waits for room on the pair now.
    bool writeWaiting() { return end_.writeWaiting(); }

    /// The frames read so far on `runtime` (0: the connection), as `describeHost` gives them.
    std::vector<std::string> frames(uint32_t runtime) { return end_.frames(runtime); }
    bool saw(uint32_t runtime, const std::string& frame) { return end_.saw(runtime, frame); }
    /// The first frame the host's end wrote, as the documented layout reads it.
    std::optional<ParsedFrame> first() { return end_.first(); }

private:
    EndEvents events_;
    ScriptedEngineEnd end_;
};

/// A stream with the frames that crossed it counted by runtime and kind, by the documented layout, whatever pieces they come in: what was read (the other side's frames) and what was written (this side's); only each frame's size, runtime and kind are kept, its body skipped.
class CountingStream final : public SandboxRemoteStream {
public:
    explicit CountingStream(std::shared_ptr<SandboxRemoteStream> stream) : stream_(std::move(stream)) {}

    size_t read(char* out, size_t size) override {
        const size_t n = stream_->read(out, size);
        read_.add(std::string_view(out, n));
        return n;
    }
    /// Counted before it is written: a whole frame, as the engine's end writes them.
    bool write(std::string_view bytes) override {
        written_.add(bytes);
        return stream_->write(bytes);
    }
    void shutdown() override { stream_->shutdown(); }

    /// The frames of `kind` read so far, on `runtime` when one is given.
    size_t readOf(uint8_t kind, std::optional<uint32_t> runtime = std::nullopt) { return read_.count(kind, runtime); }
    size_t writtenOf(uint8_t kind, std::optional<uint32_t> runtime = std::nullopt) { return written_.count(kind, runtime); }

private:
    /// One direction's frames: a frame's first 9 bytes, then its body skipped; a size under 5 stops the count.
    class Tally {
    public:
        void add(std::string_view bytes) {
            std::lock_guard<std::mutex> l(mutex_);
            while (!bytes.empty() && !broken_) {
                if (skip_) {
                    const size_t n = std::min(skip_, bytes.size());
                    skip_ -= n;
                    bytes.remove_prefix(n);
                    continue;
                }
                const size_t take = std::min(kHead - head_.size(), bytes.size());
                head_.append(bytes.substr(0, take));
                bytes.remove_prefix(take);
                if (head_.size() < kHead) return;
                const uint32_t size = uint32_t(wire::little(std::string_view(head_).substr(0, 4)));
                if (size < 5) {
                    broken_ = true;
                    return;
                }
                counts_[{uint32_t(wire::little(std::string_view(head_).substr(4, 4))), uint8_t(head_[8])}]++;
                skip_ = size - 5;
                head_.clear();
            }
        }
        size_t count(uint8_t kind, std::optional<uint32_t> runtime) {
            std::lock_guard<std::mutex> l(mutex_);
            size_t n = 0;
            for (const auto& [key, count] : counts_)
                if (key.second == kind && (!runtime || key.first == *runtime)) n += count;
            return n;
        }

    private:
        static constexpr size_t kHead = 9;
        std::mutex mutex_;
        std::string head_;
        size_t skip_ = 0;
        bool broken_ = false;
        std::map<std::pair<uint32_t, uint8_t>, size_t> counts_;
    };

    const std::shared_ptr<SandboxRemoteStream> stream_;
    Tally read_;
    Tally written_;
};

/// A host's end opened with `resources` and the engine process's end over a memory pair, the engine's end serving on a worker of the test's, with `lockdown` when one is given; with `renderers`, a hosting stream (`startHosting`, `serveHosting`). The destructor stops the host's end, then joins the engine's.
struct StreamPeers {
    EndEvents events;
    MemoryStreamPair pair = memoryStreamPair(size_t(1) << 20);
    /// The engine's side as the engine's end reads it, the opening it got first.
    const std::shared_ptr<WatchedStream> engineSide = std::make_shared<WatchedStream>(pair.engine);
    /// The same side counted: the host's frames the engine's end read and its own it wrote.
    const std::shared_ptr<CountingStream> counted = std::make_shared<CountingStream>(engineSide);
    std::shared_ptr<SandboxRemoteHostEnd> end;
    Background<std::string> serving;

    explicit StreamPeers(const std::string& engineBuild = kBuild, const SandboxResources& resources = *RES, std::function<std::string()> lockdown = nullptr,
                         HostedRendererFactory renderers = nullptr)
        : end(renderers ? SandboxRemoteHostEnd::startHosting(pair.host, resources, kBuild, events.events(), {}) : SandboxRemoteHostEnd::start(pair.host, resources, kBuild, events.events(), {})),
          serving([this, engineBuild, lockdown, renderers] {
              return renderers ? serveHostingEngineEnd(*counted, engineBuild, lockdown, renderers) : serveEngineEnd(*counted, engineBuild, lockdown);
          }) {}
    ~StreamPeers() {
        end->stop();
        serving.finish(5);
    }
    StreamPeers(const StreamPeers&) = delete;
    StreamPeers& operator=(const StreamPeers&) = delete;
};

/// A client-mode engine's delegate over a stream: what it hands over, in order; a layout query answers a 77 px rect, after `gate` when it is set.
class StreamDelegate final : public SandboxEngineDelegate {
public:
    /// Set before a query comes; the test opens it before the engine goes.
    Gate* gate = nullptr;

    void onOps(SandboxEngine&, const std::string&) override { record("ops"); }
    QueryAnswer onQuery(SandboxEngine&, const std::string& json) override {
        record("query:" + json);
        if (gate) gate->wait();
        record("query-done");
        return {rectOf(77), std::nullopt};
    }
    void onListen(SandboxEngine&, const std::string&) override {}
    void onPreventDefault(SandboxEngine&, const std::string&) override {}
    void onRequestFrame(SandboxEngine&) override {}
    void onLog(SandboxEngine&, const std::string&, const std::string& text) override { record("log:" + text); }
    void onDiag(SandboxEngine&, const std::string& kind, const std::string& detail) override { record("diag:" + kind + ":" + detail); }
    void onLoaded(SandboxEngine&) override { record("loaded"); }
    void onStats(SandboxEngine&, const SandboxStats&) override {}
    void onDied(SandboxEngine&, const std::string& reason) override { record("died:" + reason); }
    void onTaskStarted(SandboxEngine&) override { record("begin"); }
    void onTaskCompleted(SandboxEngine&) override { record("end"); }

    /// An event of the test's own among the delegate's (what a probe answered).
    void record(const std::string& event) {
        std::lock_guard<std::mutex> l(mutex_);
        events_.push_back(event);
    }
    std::vector<std::string> events() {
        std::lock_guard<std::mutex> l(mutex_);
        return events_;
    }
    std::vector<std::string> eventsWith(const std::string& prefix) {
        std::vector<std::string> out;
        for (const std::string& e : events())
            if (e.rfind(prefix, 0) == 0) out.push_back(e);
        return out;
    }
    bool saw(const std::string& event) {
        const std::vector<std::string> all = events();
        return std::find(all.begin(), all.end(), event) != all.end();
    }

private:
    std::mutex mutex_;
    std::vector<std::string> events_;
};

/// The process the tests run in, 0 where they read none.
inline long processId() {
#ifndef _WIN32
    return long(getpid());
#else
    return 0;
#endif
}

/// The host's IntlProvider for a client over a stream: `answer` (set it before the first request), noting the process and the name of each thread it formats on.
class StreamIntl final : public IntlProvider {
public:
    std::string answer = "INTL";

    std::string format(const Json&) override {
        std::lock_guard<std::mutex> l(mutex_);
        threads_.push_back(currentThreadName());
        processes_.push_back(processId());
        return answer;
    }
    std::vector<std::string> threads() {
        std::lock_guard<std::mutex> l(mutex_);
        return threads_;
    }
    std::vector<long> processes() {
        std::lock_guard<std::mutex> l(mutex_);
        return processes_;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> threads_;
    std::vector<long> processes_;
};

/// The engine process's probe answer for `runtime`'s probe `seq`.
inline std::string probeAnswer(uint32_t runtime, int64_t seq, const std::string& text) { return FrameBytes(runtime, wire::ProbeAnswer).i64(seq).str(text).bytes(); }

inline std::string logFrame(uint32_t runtime, const std::string& text) { return FrameBytes(runtime, wire::Log).str("log").str(text).bytes(); }

inline double msSince(std::chrono::steady_clock::time_point at) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - at).count(); }

// ───────────── through WidgetHost ─────────────

/// A WidgetHost whose engine is a runtime on a stream (a factory set only while it is built gives it `end`'s channel), started: its renderer answers every layout query `offsetHeight` px tall and its Intl provider `intlAnswer`; what reached its callbacks, in order under a lock: each validated batch, each request and wallet call with its responder.
struct StreamHost {
    using Respond = std::function<void(const WidgetRequestOutcome&)>;
    const std::shared_ptr<StreamIntl> intl = std::make_shared<StreamIntl>();
    ScriptedRenderer* renderer = nullptr;
    /// Factory calls made for it.
    int asked = 0;
    std::unique_ptr<WidgetHost> host;

    StreamHost(SandboxRemoteHostEnd& end, const char* html, WidgetHostOptions options, int offsetHeight, std::string intlAnswer) {
        intl->answer = std::move(intlAnswer);
        {
            FactoryReset reset;
            SandboxEngine::setRemoteChannelFactory([this, &end](std::shared_ptr<SandboxResources>) -> std::shared_ptr<SandboxRemoteChannel> {
                asked++;
                return end.channel();
            });
            host = std::make_unique<WidgetHost>(html, WidgetTheme{}, 300, std::move(options), RES, scripted(renderer), intl);
        }
        const std::string h = std::to_string(offsetHeight);
        renderer->queryResult = "{\"x\":0,\"y\":0,\"width\":100,\"height\":" + h + ",\"offsetHeight\":" + h + "}";
        host->onRenderOps = [this](const std::string& ops) {
            std::lock_guard<std::mutex> l(mutex_);
            rendered_ += ops;
        };
        host->onRequest = [this](const WidgetRequest& request, Respond respond) { keep("request:" + request.data + (request.wallet ? ":wallet" : ""), std::move(respond)); };
        host->onWallet = [this](const WalletCall& call, Respond respond) { keep("wallet:" + std::string(call.methodName()) + ":" + call.data + ":" + call.payload.text.value_or(""), std::move(respond)); };
        host->start();
    }
    /// The WidgetHost goes first: it joins its threads while what its callbacks use still lives, a callback already running included.
    ~StreamHost() { host.reset(); }
    StreamHost(const StreamHost&) = delete;
    StreamHost& operator=(const StreamHost&) = delete;

    /// Loaded, and every op sent so far applied.
    bool shown() { return host->waitLoaded(20) && host->waitRendered(5); }
    std::string rendered() {
        std::lock_guard<std::mutex> l(mutex_);
        return rendered_;
    }
    /// Whether the validated batches carry `text` as a string of their own (a text node's).
    bool renders(const std::string& text) { return rendered().find(Json::quote(text)) != std::string::npos; }
    std::vector<std::string> calls() {
        std::lock_guard<std::mutex> l(mutex_);
        return calls_;
    }
    /// A trusted tap on the widget's first element through the renderer.
    void tap() { renderer->onMessage("{\"type\":\"event\",\"ev\":" + kTapOnButton + ",\"trusted\":true}"); }
    /// The `i`th call completed from the test's thread; false when it did not come.
    bool respond(size_t i, const WidgetRequestOutcome& outcome) {
        Respond responder;
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (i < responders_.size()) responder = responders_[i];
        }
        if (!responder) return false;
        responder(outcome);
        return true;
    }

private:
    void keep(const std::string& call, Respond respond) {
        std::lock_guard<std::mutex> l(mutex_);
        calls_.push_back(call);
        responders_.push_back(std::move(respond));
    }

    std::mutex mutex_;
    std::string rendered_;
    std::vector<std::string> calls_;
    std::vector<Respond> responders_;
};

/// The widget a WidgetHost runs over the stream: its layout answer and the host's Intl in its text, and on a tap a request for the wallet, then a signature on its grant, every outcome in `log`.
inline const char* kHostWidget =
    "<button id='b'>go</button><div id='d'>a</div><p id='n'>b</p><script>var d = document.getElementById('d'), log = []; d.style.height = '50px'; d.textContent = 'h:' + d.offsetHeight;"
    "document.getElementById('n').textContent = 'n:' + (1234.5).toLocaleString();"
    "document.getElementById('b').onclick = function () { TelegramWidget.request('ask', { wallet: true }).then(function (a) { log.push('ok:' + a.result);"
    "  return TelegramWidget.wallet.signData({ type: 'text', text: 'sign' }, 'w'); })"
    "  .then(function (b) { log.push('signed:' + b.result); }, function (e) { log.push('err:' + e.code); }); };</script>";

/// The boot diagnostics a WidgetHost got.
inline std::vector<std::string> bootsOf(const WidgetDiagnostics& d) {
    const auto it = d.diags.find("boot");
    return it == d.diags.end() ? std::vector<std::string>() : it->second.second;
}

/// Whether the one boot of `d` evaluated the sandbox bundle `how` ("(compiled)", "(source)").
inline bool bootedFrom(const WidgetDiagnostics& d, const std::string& how) {
    const std::vector<std::string> boots = bootsOf(d);
    return d.countDiag("boot") == 1 && boots.size() == 1 && boots[0].rfind("sandbox bundle eval ", 0) == 0 && boots[0].size() > how.size() &&
           boots[0].compare(boots[0].size() - how.size(), how.size(), how) == 0;
}

/// The opening's fields as the engine's end read them, by the documented layout: version, build, the sandbox bundle, the validator's and the renderer's stamps; empty when it is not a whole opening on the connection with nothing after its fields.
inline std::vector<std::string> openingFields(const std::optional<ParsedFrame>& frame) {
    if (!frame || frame->runtime != 0 || frame->kind != wire::Opening) return {};
    FieldsOf in(*frame);
    const std::optional<int64_t> version = in.i64();
    std::vector<std::string> out = {version ? std::to_string(*version) : "?"};
    for (int i = 0; i < 4; i++) out.push_back(in.str().value_or("?"));
    return in.done() ? out : std::vector<std::string>();
}

// ───────────── hostile bytes ─────────────

/// The engine's frame of `kind` on `runtime` with no fields.
inline std::string bare(uint32_t runtime, uint8_t kind) { return FrameBytes(runtime, kind).bytes(); }

inline std::string queryFrame(uint32_t runtime, const std::string& json) { return FrameBytes(runtime, wire::Query).str(json).bytes(); }

inline std::string intlFrame(uint32_t runtime, const std::string& json) { return FrameBytes(runtime, wire::Intl).str(json).bytes(); }

inline const std::string kRectQuery = "{\"q\":\"rect\"}";

/// One input of an engine process that `remote_stream_hostile_frames` (or `remote_stream_hosted_hostile_frames`) gives a fresh stream, and the reason the whole stream must end with.
struct Hostile {
    std::string name;
    std::string bytes;
    std::string reason;
    /// The answer to the opening itself, so no Ready comes before it.
    bool first = false;
    /// A size with nothing behind it, not even the marker.
    bool bodiless = false;
    /// Runtime 1's synchronous request, held at its sink's gate before `bytes`.
    std::string held;
    /// Runtime 1's probe, waiting before `bytes`.
    bool probing = false;
    /// On a hosting stream: runtime 1 a bare sink on a hosted channel, runtime 2 a hosted client.
    bool hosting = false;
};

/// A hosting opening's fields as the engine's end read them, by the documented layout: version, build, the sandbox bundle, the validator bundle, the renderer's stamp; empty when it is not a whole hosting opening on the connection with nothing after its fields.
inline std::vector<std::string> hostingOpeningFields(const std::optional<ParsedFrame>& frame) {
    if (!frame || frame->runtime != 0 || frame->kind != wire::HostingOpening) return {};
    FieldsOf in(*frame);
    const std::optional<int64_t> version = in.i64();
    std::vector<std::string> out = {version ? std::to_string(*version) : "?"};
    for (int i = 0; i < 4; i++) out.push_back(in.str().value_or("?"));
    return in.done() ? out : std::vector<std::string>();
}

/// `v` on a fresh stream of two runtimes (a hosting one with `v.hosting`), 1 at a bare sink and 2 a client (hosted on a hosting stream) whose task is open: the opening as the documented layout reads it, then (unless `v` answers it) Ready and a log for each; `v` with a marker for each runtime behind it in the same write ends the stream once with its reason within 2 s, each runtime hears it once and nothing after (the marker neither), the client closes its open task, and a channel attached later is lost at once.
inline void hostileRun(const Hostile& v) {
    const std::string name = v.name + ": ";
    ScriptedEngine engine({}, v.hosting);
    Gate gate;
    auto sink = std::make_shared<StreamSink>();
    sink->gate = &gate;
    sink->platformGate = &gate;
    std::shared_ptr<SandboxRemoteChannel> channel = v.hosting ? engine.host().hostedChannel() : engine.host().channel();
    channel->attach(sink);
    StreamDelegate delegate;
    std::unique_ptr<SandboxEngine> owned = v.hosting ? SandboxEngine::hostedClient("<p>x</p>", testConfig(), engine.host().hostedChannel())
                                                     : std::make_unique<SandboxEngine>("<p>x</p>", testConfig(), engine.host().channel());
    SandboxEngine& client = *owned;
    client.setDelegate(&delegate);
    CHECK_MSG(eventually([&] { return engine.saw(1, "open") && engine.saw(2, "open"); }), name + joined(engine.frames(0)));
    if (v.hosting) {
        const std::vector<std::string> opening = hostingOpeningFields(engine.first());
        CHECK_MSG((opening == std::vector<std::string>{"1", kBuild, "s", RES->validatorBundle, ""}), name + joined(opening));
    } else {
        const std::vector<std::string> opening = openingFields(engine.first());
        CHECK_MSG((opening == std::vector<std::string>{"1", kBuild, "s", firstLine(RES->validatorBundle), ""}), name + joined(opening));
    }
    std::vector<std::string> heard, told;
    if (!v.first) {
        CHECK(engine.ready() && engine.send(logFrame(1, "control-1") + bare(2, wire::TaskStarted) + logFrame(2, "control-2")));
        CHECK_MSG(eventually([&] { return sink->saw("log:log:control-1") && delegate.saw("log:control-2") && engine.events().open() == 2; }),
                  name + joined(sink->events()) + " / " + joined(delegate.events()) + " / " + joined(engine.events().all()));
        heard = {"log:log:control-1"};
        told = {"begin", "log:control-2"};
    }
    Background<std::string> probing([&] { return v.probing ? channel->probe("held") : std::string(); });
    if (v.probing) CHECK_MSG(eventually([&] { return engine.saw(1, "probe:1:held"); }), name + joined(engine.frames(1)));
    if (!v.held.empty()) {
        CHECK(engine.send(v.held));
        CHECK_MSG(gate.waitedOn(), name + joined(sink->events()));
        heard = sink->events();
    }
    const std::string marker = (v.first ? FrameBytes(0, wire::Ready).i64(1).bytes() : std::string()) + logFrame(1, "trailing-1") + logFrame(2, "trailing-2");
    const auto sent = std::chrono::steady_clock::now();
    CHECK(engine.send(v.bytes + (v.bodiless ? std::string() : marker)));
    CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }, 2), name + joined(engine.events().all()));
    const double ms = msSince(sent);
    CHECK_MSG(endsOnce(engine.events().all(), v.reason), name + joined(engine.events().all()));
    if (v.bodiless) CHECK_MSG(ms < 1000, name + std::to_string(ms) + " ms");
    heard.push_back("disconnected");
    CHECK_MSG(sink->events() == heard, name + joined(sink->events()));
    if (v.probing) CHECK_MSG(probing.within(2) == std::optional<std::string>("ERROR no runtime"), name + "the probe");
    CHECK_MSG(eventually([&] { return delegate.saw("died:" + kEngineGone); }), name + joined(delegate.events()));
    client.thread().sync([] {});
    told.push_back("died:" + kEngineGone);
    if (!v.first) told.push_back("end");
    CHECK_MSG(delegate.events() == told, name + joined(delegate.events()));
    auto later = std::make_shared<StreamSink>();
    (v.hosting ? engine.host().hostedChannel() : engine.host().channel())->attach(later);
    CHECK_MSG(later->events() == std::vector<std::string>{"disconnected"}, name + joined(later->events()));
    gate.open();
}

// ───────────── a child case on a socket ─────────────

#ifndef _WIN32
/// The descriptor a child case run by `runChildOn` finds its stream on.
inline constexpr int kChildStreamFd = 3;

#if defined(__APPLE__) || defined(__linux__)
/// How many threads of this process have each name now (Apple: the task's threads; Linux: /proc/self/task), nullopt when they cannot be listed; a thread gone while they are read is left out.
inline std::optional<std::map<std::string, int>> threadNames() {
    std::map<std::string, int> names;
#ifdef __APPLE__
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return std::nullopt;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        thread_extended_info_data_t info = {};
        mach_msg_type_number_t size = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(threads[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info), &size) == KERN_SUCCESS) names[info.pth_name]++;
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), vm_address_t(threads), vm_size_t(count * sizeof(thread_act_t)));
#else
    std::error_code error;
    for (const std::filesystem::directory_entry& task : std::filesystem::directory_iterator("/proc/self/task", error)) {
        std::string name = readFile((task.path() / "comm").string());
        if (!name.empty() && name.back() == '\n') name.pop_back();
        names[name]++;
    }
    if (error) return std::nullopt;
#endif
    if (names.empty()) return std::nullopt;
    return names;
}

/// `names` as "name×count" in name order, unnamed threads left out.
inline std::string namesText(const std::map<std::string, int>& names) {
    std::string out;
    for (const auto& [name, count] : names)
        if (!name.empty()) out += (out.empty() ? "" : " ") + name + "×" + std::to_string(count);
    return out;
}

inline int countOf(const std::map<std::string, int>& names, const std::string& name) {
    const auto it = names.find(name);
    return it == names.end() ? 0 : it->second;
}
#endif

/// What a child case run on a socket gave: its exit status (128 + the signal that ended it; -1 when it outlived its deadline and was killed, -2 when it never started, -3 when it was lost) and its output.
struct ChildRun {
    int status = -2;
    std::string output;
};

/// `twidget-tests --child <name>` with a duplicate of `stream`'s descriptor as its descriptor 3 and its output in a file, waited for at most `seconds` (killed and reaped past them); this process's copy of `stream` closes once the child has its own.
inline ChildRun runChildOn(const std::string& name, SocketStream& stream, double seconds) {
    ChildRun run;
    const std::filesystem::path log = std::filesystem::temp_directory_path() / ("twidget-tests-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
    const int spare = ::fcntl(stream.descriptor(), F_DUPFD_CLOEXEC, 10);
    if (spare < 0) {
        stream.close();
        run.output = std::string("no descriptor to hand over: ") + std::strerror(errno);
        return run;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    posix_spawn_file_actions_adddup2(&actions, spare, kChildStreamFd);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    std::string self = g_self, flag = "--child", child = name;
    char* argv[] = {self.data(), flag.data(), child.data(), nullptr};
    pid_t pid = -1;
    const int failed = posix_spawn(&pid, self.c_str(), &actions, &attributes, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    ::close(spare);
    stream.close();
    if (failed) {
        run.output = std::string("posix_spawn: ") + std::strerror(failed);
        return run;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    int status = 0;
    for (;;) {
        const pid_t reaped = ::waitpid(pid, &status, WNOHANG);
        if (reaped == pid) {
            run.status = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -3;
            break;
        }
        if (reaped < 0 && errno != EINTR) {
            run.status = -3;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            run.status = -1;
            break;
        }
        sleepMs(20);
    }
    run.output = readFile(log.string());
    std::error_code ignored;
    std::filesystem::remove(log, ignored);
    return run;
}
#endif

#ifdef __APPLE__
// ───────────── a transport thread the OS refuses to start (macOS: the per-process thread limit) ─────────────

/// How the host's end words the death of a runtime whose `widget.answer` cannot start, before the refusal itself.
inline const std::string kAnswerUnstarted = "the host could not start the thread that answers the widget";
/// How the engine's end words the death of a runtime whose engine cannot be built (its `widget.sandbox` cannot start), before the refusal itself.
inline const std::string kEngineUnstarted = "the widget engine could not start";
/// How the engine's end words the death of a runtime whose `widget.probe` cannot start, before the refusal itself.
inline const std::string kProbeUnstarted = "the engine process could not start the widget's probe thread";
/// How the engine's end words why it stops serving when its `widget.reaper` cannot start, before the refusal itself.
inline const std::string kReaperUnstarted = "the engine process could not start the thread that destroys closed runtimes";

/// Whether `reason` is `context`'s report of a refused `thread`: the context, then the refusal of that thread with the OS's EAGAIN.
inline bool unstarted(const std::string& reason, const std::string& context, const std::string& thread) {
    return reason.rfind(context + ": ", 0) == 0 && reason.find("JSThread " + thread + ": ") != std::string::npos &&
           reason.find(std::generic_category().message(EAGAIN)) != std::string::npos;
}

/// `items` joined whole, for a row that must show a reason in full.
inline std::string whole(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) out += (out.empty() ? "" : " | ") + item;
    return out;
}

/// The thread allowance filled for one leg: empty, or what went wrong, printed with the named threads made before it.
inline std::string filledFor(ThreadAllowance& allowance, const std::string& leg, const std::map<std::string, int>& before) {
    const std::string filled = allowance.fill();
    CHECK_MSG(filled.empty(), leg + ": " + filled);
    if (filled.empty()) printf("    %s: full at %d threads, %zu of them parked; before it %s\n", leg.c_str(), allowance.full(), allowance.parked(), namesText(before).c_str());
    fflush(stdout);
    return filled;
}

/// A child case's legs, then the task's threads back at their count before them: every thread a leg made ended.
inline void legsLeaveNoThread(const std::function<void()>& legs) {
    const std::optional<int> alone = taskThreads();
    CHECK_MSG(alone.has_value(), "the task's threads cannot be listed");
    if (!alone) return;
    legs();
    const std::optional<int> after = threadsSettleAt(*alone);
    printf("    every thread of the legs ended: %s threads, %d before them\n", countText(after).c_str(), *alone);
    fflush(stdout);
    CHECK_MSG(after == alone, countText(after) + " threads, " + std::to_string(*alone) + " before the legs");
}

/// Runs child case `name` in a fresh process (`runChild`) for at most `seconds` and prints what it printed: the test fails unless it exits 0.
inline void childPasses(const std::string& name, int seconds) {
    const ChildExit child = runChild(name, seconds);
    printf("%s", child.output.c_str());
    if (!child.output.empty() && child.output.back() != '\n') printf("\n");
    fflush(stdout);
    CHECK_MSG(child.ok, "the child (twidget-tests --child " + name + ") " + child.ending);
}
#endif
