// What test_remote_stream.cpp (the framed transport's tests), test_remote_stream_hosted.cpp (a hosting stream's) and test_hosted.cpp (the hosted mode's, for a probe over a hosting stream) share: the documented wire written by hand, both scripted peers and the real ends over a memory pair, the sinks, delegates and hosts on them, the hostile-frame run and a child case on a socket.
#pragma once

#include "test_support.h"
#include "test_remote_support.h"

#include "../harness/stream_peers.h"
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

// ───────────── the documented wire, written by hand ─────────────

/// The wire as its table documents it, never through the library's codec: the kind numbers and little-endian integers.
namespace wire {

enum : uint8_t {
    Opening = 1, Open = 2, Start = 3, Event = 4, Activation = 5, AnimationFrame = 6, Viewport = 7, Resize = 8, Theme = 9, SafeArea = 10, Data = 11,
    Visible = 12, PressBegan = 13, PressEnded = 14, ChargeValidator = 15, Stop = 16, Answer = 17, Probe = 18, QueryAnswer = 19, IntlAnswer = 20,
    Close = 21,
    Ready = 64, Refused = 65, TaskStarted = 66, TaskCompleted = 67, Ops = 68, Query = 69, Intl = 70, Listen = 71, PreventDefault = 72, Anim = 73,
    RequestsFrame = 74, Log = 75, Diag = 76, Bridge = 77, CopyText = 78, Request = 79, Wallet = 80, Storage = 81, ResizeAnchor = 82,
    WatchesPublicData = 83, Loaded = 84, Stats = 85, Died = 86, ProbeAnswer = 87,
    HostingOpening = 32, HostedStart = 33, HostedTheme = 34, Platform = 35, PlatformAnswer = 36,
    PlatformMessage = 96, PlatformRequest = 97, ValidatedOps = 98, RendererReady = 99, Rendered = 100, Nodes = 101,
};

/// `v` as 4 bytes, the lowest first: a frame's size, a runtime id or a string's length (a size alone is a header with no body).
inline std::string u32(uint32_t v) {
    std::string out;
    for (int shift = 0; shift < 32; shift += 8) out.push_back(char(uint8_t(v >> shift)));
    return out;
}

inline std::string u64(uint64_t v) {
    std::string out;
    for (int shift = 0; shift < 64; shift += 8) out.push_back(char(uint8_t(v >> shift)));
    return out;
}

inline uint64_t little(std::string_view bytes) {
    uint64_t v = 0;
    for (size_t i = 0; i < bytes.size(); i++) v |= uint64_t(uint8_t(bytes[i])) << (8 * i);
    return v;
}

} // namespace wire

/// One frame built field by field as the documented wire lays it out; `raw` and a `boolean` of any byte make a damaged one, and `body` leaves the size out for a test that writes a size of its own.
class FrameBytes {
public:
    FrameBytes(uint32_t runtime, uint8_t kind) : body_(wire::u32(runtime)) { body_.push_back(char(kind)); }

    FrameBytes& str(std::string_view s) {
        body_ += wire::u32(uint32_t(s.size()));
        body_.append(s);
        return *this;
    }
    FrameBytes& i64(int64_t v) {
        body_ += wire::u64(uint64_t(v));
        return *this;
    }
    FrameBytes& f64(double v) {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        body_ += wire::u64(bits);
        return *this;
    }
    /// 0 or 1 for a bool, any other byte for a damaged one.
    FrameBytes& boolean(uint8_t byte) {
        body_.push_back(char(byte));
        return *this;
    }
    FrameBytes& raw(std::string_view bytes) {
        body_.append(bytes);
        return *this;
    }

    /// The size of the body, then the body.
    std::string bytes() const { return wire::u32(uint32_t(body_.size())) + body_; }
    const std::string& body() const { return body_; }

private:
    std::string body_;
};

/// A frame read back by the documented layout: its runtime, its kind and the bytes of its fields.
struct ParsedFrame {
    uint32_t runtime = 0;
    uint8_t kind = 0;
    std::string fields;
};

/// A parsed frame's fields read in order; a read past the end is nullopt.
class FieldsOf {
public:
    explicit FieldsOf(const ParsedFrame& frame) : rest_(frame.fields) {}

    std::optional<std::string> str() {
        std::optional<uint64_t> length = take(4);
        if (!length || *length > rest_.size()) return std::nullopt;
        std::string out(rest_.substr(0, size_t(*length)));
        rest_.remove_prefix(size_t(*length));
        return out;
    }
    std::optional<int64_t> i64() {
        std::optional<uint64_t> v = take(8);
        return v ? std::optional<int64_t>(int64_t(*v)) : std::nullopt;
    }
    std::optional<double> f64() {
        std::optional<uint64_t> v = take(8);
        if (!v) return std::nullopt;
        double out = 0;
        std::memcpy(&out, &*v, sizeof(out));
        return out;
    }
    std::optional<uint8_t> byte() {
        std::optional<uint64_t> v = take(1);
        return v ? std::optional<uint8_t>(uint8_t(*v)) : std::nullopt;
    }
    bool done() const { return rest_.empty(); }

private:
    std::optional<uint64_t> take(size_t bytes) {
        if (rest_.size() < bytes) return std::nullopt;
        const uint64_t v = wire::little(rest_.substr(0, bytes));
        rest_.remove_prefix(bytes);
        return v;
    }

    std::string_view rest_;
};

/// The frame whose 4 + `size` bytes begin `bytes`; a size under 5 has no runtime and kind to give.
inline std::optional<ParsedFrame> frameAt(std::string_view bytes, uint32_t size) {
    if (size < 5 || bytes.size() < 4 + size_t(size)) return std::nullopt;
    return ParsedFrame{uint32_t(wire::little(bytes.substr(4, 4))), uint8_t(bytes[8]), std::string(bytes.substr(9, size_t(size) - 5))};
}

/// The whole frames at the front of `bytes`, by their sizes; what follows the last of them is left in `rest`.
inline std::vector<ParsedFrame> parseFrames(std::string_view bytes, std::string* rest = nullptr) {
    std::vector<ParsedFrame> out;
    while (bytes.size() >= 4) {
        const uint32_t size = uint32_t(wire::little(bytes.substr(0, 4)));
        std::optional<ParsedFrame> frame = frameAt(bytes, size);
        if (!frame) break;
        out.push_back(std::move(*frame));
        bytes.remove_prefix(4 + size_t(size));
    }
    if (rest) *rest = std::string(bytes);
    return out;
}

/// `bytes` in pieces of `piece` bytes, the last one shorter, for a peer that writes a frame in parts.
inline std::vector<std::string> fragments(std::string_view bytes, size_t piece) {
    std::vector<std::string> out;
    for (size_t at = 0; at < bytes.size(); at += piece) out.emplace_back(bytes.substr(at, piece));
    return out;
}

/// A test's side of a stream reading whole frames by the documented layout, whatever pieces they come in; a size past `limit` or under 5 ends it unread.
class FrameStreamReader {
public:
    explicit FrameStreamReader(SandboxRemoteStream& stream, size_t limit = size_t(64) << 20) : stream_(stream), limit_(limit) {}

    /// The next whole frame, or nullopt once the stream ended before one.
    std::optional<ParsedFrame> next() {
        for (;;) {
            if (pending_.size() >= 4) {
                const uint32_t size = uint32_t(wire::little(std::string_view(pending_).substr(0, 4)));
                if (size < 5 || size > limit_) return std::nullopt;
                if (std::optional<ParsedFrame> frame = frameAt(pending_, size)) {
                    pending_.erase(0, 4 + size_t(size));
                    return frame;
                }
            }
            char buffer[4096];
            const size_t got = stream_.read(buffer, sizeof(buffer));
            if (!got) return std::nullopt;
            pending_.append(buffer, got);
        }
    }
    /// What came after the last whole frame read.
    const std::string& pending() const { return pending_; }

private:
    SandboxRemoteStream& stream_;
    const size_t limit_;
    std::string pending_;
};

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

inline std::string openingBytes(int64_t version, std::string_view build, std::string_view bundle, std::string_view bytecode, std::string_view validatorStamp, std::string_view rendererStamp) {
    return FrameBytes(0, wire::Opening).i64(version).str(build).str(bundle).str(bytecode).str(validatorStamp).str(rendererStamp).bytes();
}

/// The opening a host writes for `res`: its sandbox bundle with its bytecode (or without), the first lines of its validator and renderer bundles.
inline std::string openingOf(const SandboxResources& res, bool bytecode) {
    const std::string code(res.sandboxBytecode.begin(), res.sandboxBytecode.end());
    return openingBytes(1, kBuild, res.sandboxBundle, bytecode ? code : std::string(), firstLine(res.validatorBundle), firstLine(res.rendererBundle));
}

/// An opening for cases that boot no runtime: a sandbox bundle of one byte, the validator's stamp and no renderer.
inline std::string smallOpening() { return openingBytes(1, kBuild, "s", "", firstLine(RES->validatorBundle), ""); }

inline std::string hostingOpeningBytes(int64_t version, std::string_view build, std::string_view bundle, std::string_view bytecode, std::string_view validatorBundle,
                                       std::string_view validatorBytecode, std::string_view rendererStamp) {
    return FrameBytes(0, wire::HostingOpening).i64(version).str(build).str(bundle).str(bytecode).str(validatorBundle).str(validatorBytecode).str(rendererStamp).bytes();
}

/// The opening a host writes for `res`'s hosted widgets: its sandbox and validator bundles with their bytecode (a given validator bundle or bytecode in their place), the first line of its renderer bundle.
inline std::string hostingOpeningOf(const SandboxResources& res, std::optional<std::string> validator = std::nullopt, std::optional<std::string> validatorCode = std::nullopt) {
    const std::string code(res.sandboxBytecode.begin(), res.sandboxBytecode.end()), checker(res.validatorBytecode.begin(), res.validatorBytecode.end());
    return hostingOpeningBytes(1, kBuild, res.sandboxBundle, code, validator.value_or(res.validatorBundle), validatorCode.value_or(checker), firstLine(res.rendererBundle));
}

/// A hosting opening for cases that build no widget: bundles of one byte, no bytecode and no renderer.
inline std::string smallHostingOpening() { return hostingOpeningBytes(1, kBuild, "s", "", "v", "", ""); }

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
              return renderers ? SandboxRemoteEngineEnd::serveHosting(*engine_, kBuild, lockdown, renderers) : SandboxRemoteEngineEnd::serve(*engine_, kBuild, lockdown);
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

/// What a host's end tells its owner, in order with the name of the thread it came on: "runtimes:N" and "ended:reason".
class EndEvents {
public:
    /// The end keeps them: this outlives it.
    SandboxRemoteHostEnd::Events events() {
        return {[this](size_t open) { record("runtimes:" + std::to_string(open)); }, [this](const std::string& why) { record("ended:" + why); }};
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
    }

    std::mutex mutex_;
    std::vector<std::string> events_;
    std::vector<std::string> threads_;
};

/// Whether `events` end as a stream's must: `ended` with `reason` once and last, right after a count of 0 when any count came.
inline bool endsOnce(const std::vector<std::string>& events, const std::string& reason) {
    if (events.empty() || events.back() != "ended:" + reason) return false;
    if (std::count_if(events.begin(), events.end(), [](const std::string& e) { return e.rfind("ended:", 0) == 0; }) != 1) return false;
    return events.size() == 1 || events[events.size() - 2] == "runtimes:0";
}

/// A bare sink on a host's channel, where the transport hands over: each call as text in order, with its thread; `remoteQuery` (a 77 px rect) and `remoteIntl` (`intlAnswer`) wait on `gate` while it is set, a hosted runtime's `remotePlatformRequest` (`platformAnswer`) on `platformGate`, and `remoteDisconnected` on `lost`, noted once it returns.
class StreamSink final : public SandboxRemoteSink {
public:
    /// Set before a synchronous call comes; the test opens it before the channel goes.
    Gate* gate = nullptr;
    /// Set before the loss comes; the test opens it before the stream or the channel goes.
    Gate* lost = nullptr;
    std::string intlAnswer = "INTL";
    /// Set before a platform request comes; the test opens it before the channel goes.
    Gate* platformGate = nullptr;
    std::optional<std::string> platformAnswer = "ANSWER";

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
    void remoteLog(const std::string& level, const std::string& text) override { record("log:" + level + ":" + text); }
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
        if (platformGate) platformGate->wait();
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

/// A host's frame as the tests read it, for the kinds they look at: its kind and its strings, a probe's code after its sequence.
inline std::string describeHost(const ParsedFrame& f) {
    FieldsOf in(f);
    const auto str = [&] { return in.str().value_or("?"); };
    switch (f.kind) {
    case wire::Opening: return "opening";
    case wire::Open: return "open";
    case wire::Close: return "close";
    case wire::Start: return "start";
    case wire::Event: return "event:" + str();
    case wire::Stop: return "stop:" + str();
    case wire::Probe: {
        const int64_t seq = in.i64().value_or(-1);
        return "probe:" + std::to_string(seq) + ":" + str();
    }
    case wire::QueryAnswer: return "query-answer:" + str();
    case wire::IntlAnswer: return "intl-answer:" + str();
    case wire::Answer: {
        const int64_t id = in.i64().value_or(-1);
        return "answer:" + std::to_string(id) + ":" + str();
    }
    case wire::HostingOpening: return "hosting-opening";
    case wire::HostedStart: return "hosted-start";
    case wire::HostedTheme: return "hosted-theme";
    case wire::Platform: return "platform:" + str();
    case wire::PlatformAnswer: return "platform-answer:" + str();
    default: return "kind " + std::to_string(int(f.kind));
    }
}

/// The engine process's side of a stream written by hand from the documented wire, against a real host's end (opened for hosted widgets with `hosting`): a worker of the test's keeps each frame the host writes in order, and the test writes the engine's frames; the destructor stops the end, then joins the worker.
class ScriptedEngine {
public:
    explicit ScriptedEngine(const SandboxRemoteHostEnd::Limits& limits = {}, bool hosting = false)
        : hosting_(hosting),
          end_(hosting ? SandboxRemoteHostEnd::startHosting(pair_.host, smallResources(), kBuild, events_.events(), limits)
                       : SandboxRemoteHostEnd::start(pair_.host, smallResources(), kBuild, events_.events(), limits)),
          reading_([this] { return read(); }) {}
    ~ScriptedEngine() {
        end_->stop();
        reading_.finish(5);
    }
    ScriptedEngine(const ScriptedEngine&) = delete;
    ScriptedEngine& operator=(const ScriptedEngine&) = delete;

    SandboxRemoteHostEnd& host() { return *end_; }
    EndEvents& events() { return events_; }
    /// `bytes` in one write, after everything written before.
    bool send(std::string_view bytes) {
        std::lock_guard<std::mutex> l(writing_);
        return pair_.engine->write(bytes);
    }
    /// Once the opening came, its answer.
    bool ready() { return eventually([&] { return saw(0, hosting_ ? "hosting-opening" : "opening"); }) && send(FrameBytes(0, wire::Ready).i64(1).bytes()); }
    /// As an engine process that exits: the host's end reads what was written, then the end of the stream.
    void close() { pair_.engine->close(); }

    /// The frames read so far on `runtime` (0: the connection), as `describeHost` gives them.
    std::vector<std::string> frames(uint32_t runtime) {
        std::lock_guard<std::mutex> l(mutex_);
        std::vector<std::string> out;
        for (const ParsedFrame& f : frames_)
            if (f.runtime == runtime) out.push_back(describeHost(f));
        return out;
    }
    bool saw(uint32_t runtime, const std::string& frame) {
        const std::vector<std::string> all = frames(runtime);
        return std::find(all.begin(), all.end(), frame) != all.end();
    }
    /// The first frame the host's end wrote, as the documented layout reads it.
    std::optional<ParsedFrame> first() {
        std::lock_guard<std::mutex> l(mutex_);
        return frames_.empty() ? std::nullopt : std::optional<ParsedFrame>(frames_.front());
    }

private:
    bool read() {
        FrameStreamReader reader(*pair_.engine);
        while (std::optional<ParsedFrame> frame = reader.next()) {
            std::lock_guard<std::mutex> l(mutex_);
            frames_.push_back(std::move(*frame));
        }
        return true;
    }

    EndEvents events_;
    MemoryStreamPair pair_ = memoryStreamPair(size_t(1) << 20);
    const bool hosting_;
    std::shared_ptr<SandboxRemoteHostEnd> end_;
    std::mutex writing_;
    std::mutex mutex_;
    std::vector<ParsedFrame> frames_;
    Background<bool> reading_;
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
              return renderers ? SandboxRemoteEngineEnd::serveHosting(*counted, engineBuild, lockdown, renderers) : SandboxRemoteEngineEnd::serve(*counted, engineBuild, lockdown);
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

/// Whether the one boot of `d` evaluated the sandbox bundle `how` ("(bytecode)", "(source)").
inline bool bootedFrom(const WidgetDiagnostics& d, const std::string& how) {
    const std::vector<std::string> boots = bootsOf(d);
    return d.countDiag("boot") == 1 && boots.size() == 1 && boots[0].rfind("sandbox bundle eval ", 0) == 0 && boots[0].size() > how.size() &&
           boots[0].compare(boots[0].size() - how.size(), how.size(), how) == 0;
}

/// The opening's fields as the engine's end read them, by the documented layout: version, build, the sandbox bundle and its bytecode, the validator's and the renderer's stamps; empty when it is not a whole opening on the connection with nothing after its fields.
inline std::vector<std::string> openingFields(const std::optional<ParsedFrame>& frame) {
    if (!frame || frame->runtime != 0 || frame->kind != wire::Opening) return {};
    FieldsOf in(*frame);
    const std::optional<int64_t> version = in.i64();
    std::vector<std::string> out = {version ? std::to_string(*version) : "?"};
    for (int i = 0; i < 5; i++) out.push_back(in.str().value_or("?"));
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

/// A hosting opening's fields as the engine's end read them, by the documented layout: version, build, the sandbox bundle and its bytecode, the validator bundle and its bytecode, the renderer's stamp; empty when it is not a whole hosting opening on the connection with nothing after its fields.
inline std::vector<std::string> hostingOpeningFields(const std::optional<ParsedFrame>& frame) {
    if (!frame || frame->runtime != 0 || frame->kind != wire::HostingOpening) return {};
    FieldsOf in(*frame);
    const std::optional<int64_t> version = in.i64();
    std::vector<std::string> out = {version ? std::to_string(*version) : "?"};
    for (int i = 0; i < 6; i++) out.push_back(in.str().value_or("?"));
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
        CHECK_MSG((opening == std::vector<std::string>{"1", kBuild, "s", "", RES->validatorBundle, "", ""}), name + joined(opening));
    } else {
        const std::vector<std::string> opening = openingFields(engine.first());
        CHECK_MSG((opening == std::vector<std::string>{"1", kBuild, "s", "", firstLine(RES->validatorBundle), ""}), name + joined(opening));
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
