// The stream's documented wire written and read by hand (remote_stream.h's table, never the library's codec), and the engine process's end played with it against a real host's end over memory_stream.h's pair (`ScriptedEngineEnd`), test code with nothing but the standard library and the stream's interface: test_remote_stream_support.h's scripted peers use it (its `ScriptedEngine` is the test runner's wrapper of that end), and an embedder's own checks of a hosting stream (a self-check that plays the engine process's end) include it alone.
#pragma once

#include "../harness/memory_stream.h"
#include "twidget/remote_stream.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace twidget {

/// The wire as its table documents it, never through the library's codec: the kind numbers and little-endian integers.
namespace wire {

enum : uint8_t {
    Opening = 1, Open = 2, Start = 3, Event = 4, Activation = 5, AnimationFrame = 6, Viewport = 7, Resize = 8, Theme = 9, SafeArea = 10, Data = 11,
    Visible = 12, PressBegan = 13, PressEnded = 14, ChargeValidator = 15, Stop = 16, Answer = 17, Probe = 18, QueryAnswer = 19, IntlAnswer = 20,
    Close = 21, DeviceSafeArea = 22,
    Ready = 64, Refused = 65, TaskStarted = 66, TaskCompleted = 67, Ops = 68, Query = 69, Intl = 70, Listen = 71, PreventDefault = 72, Anim = 73,
    RequestsFrame = 74, Log = 75, Diag = 76, Bridge = 77, CopyText = 78, Request = 79, Wallet = 80, Storage = 81, ResizeAnchor = 82,
    WatchesPublicData = 83, Loaded = 84, Stats = 85, Died = 86, ProbeAnswer = 87,
    HostingOpening = 32, HostedStart = 33, HostedTheme = 34, Platform = 35, PlatformAnswer = 36, HostedViewportHeight = 37,
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

/// `v` in the fewest significant digits that read back as it, as `%g` writes them ("nan" and "inf" among them).
inline std::string shortest(double v) {
    char text[32] = {};
    for (int digits = 1; digits <= 17; digits++) {
        std::snprintf(text, sizeof(text), "%.*g", digits, v);
        if (std::strtod(text, nullptr) == v) break;
    }
    return text;
}

/// A host's frame as the tests read it, for the kinds they look at: its kind and its strings, a probe's code after its sequence, a viewport's height in its shortest digits.
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
    case wire::DeviceSafeArea: return "device-safe-area:" + str();
    case wire::HostingOpening: return "hosting-opening";
    case wire::HostedStart: return "hosted-start";
    case wire::HostedTheme: return "hosted-theme";
    case wire::Platform: return "platform:" + str();
    case wire::PlatformAnswer: return "platform-answer:" + str();
    case wire::HostedViewportHeight: {
        const std::optional<double> height = in.f64();
        return "hosted-viewport-height:" + (height ? shortest(*height) : std::string("?"));
    }
    default: return "kind " + std::to_string(int(f.kind));
    }
}

/// The engine process's end of a stream written by hand from the documented wire, against a real host's end it starts on a memory pair of `capacity` bytes each way (`startHosting` with the owner's `answers` when `hosting`, else `start`), with no test runner: a worker of its own keeps each frame the host's end writes, in order, the owner writes the engine's frames, and every wait has the owner's bound, so the owner tells a step that did not come in its own words. The owner's `Events` hear the end first; `ended` gives the end's reason after them. `finish` stops the host's end and joins the worker within a bound; the destructor finishes within `kFinish` and leaves a worker still reading past it to return on its own, with the state it shares, so nothing it uses goes first.
class ScriptedEngineEnd {
public:
    /// How `ready` went: the opening answered, no opening within its bound, or its answer not written.
    enum class Handshake { Answered, Unopened, Unwritten };

    /// How long the destructor waits for the worker.
    static constexpr std::chrono::seconds kFinish{5};

    ScriptedEngineEnd(const SandboxResources& resources, std::string build, SandboxRemoteHostEnd::Events events, const SandboxRemoteHostEnd::Limits& limits, bool hosting,
                      SandboxHostedAnswers answers = {}, size_t capacity = size_t(1) << 20)
        : state_(std::make_shared<State>(capacity)),
          hosting_(hosting),
          end_(hosting ? SandboxRemoteHostEnd::startHosting(state_->pair.host, resources, std::move(build), recorded(state_, std::move(events)), limits, std::move(answers))
                       : SandboxRemoteHostEnd::start(state_->pair.host, resources, std::move(build), recorded(state_, std::move(events)), limits)),
          reader_([state = state_] { read(*state); }) {}
    ~ScriptedEngineEnd() {
        if (!finish(kFinish)) reader_.detach();
    }
    ScriptedEngineEnd(const ScriptedEngineEnd&) = delete;
    ScriptedEngineEnd& operator=(const ScriptedEngineEnd&) = delete;

    SandboxRemoteHostEnd& host() { return *end_; }
    /// `bytes` in one write, after everything written before.
    bool send(std::string_view bytes) {
        std::lock_guard<std::mutex> l(writing_);
        return state_->pair.engine->write(bytes);
    }
    /// Once the opening (a hosting one on a hosting stream) came within `bound`, its answer: Ready of `version`.
    Handshake ready(std::chrono::milliseconds bound, int64_t version = 1) {
        if (!sawWithin(0, hosting_ ? "hosting-opening" : "opening", bound)) return Handshake::Unopened;
        return send(FrameBytes(0, wire::Ready).i64(version).bytes()) ? Handshake::Answered : Handshake::Unwritten;
    }
    /// As an engine process that exits: the host's end reads what was written, then the end of the stream.
    void close() { state_->pair.engine->close(); }
    /// As an engine process that stops reading: once the worker has the frame it is reading it calls `hold`, once, before it reads another, so the host's writer blocks once the pair is full while `hold` waits; the owner lets `hold` return before this goes.
    void holdReading(std::function<void()> hold) {
        std::lock_guard<std::mutex> l(state_->mutex);
        state_->hold = std::move(hold);
    }
    /// Whether the host's writer waits for room on the pair now.
    bool writeWaiting() { return state_->pair.host->writeWaiting(); }

    /// The frames read so far on `runtime` (0: the connection), as `describeHost` gives them.
    std::vector<std::string> frames(uint32_t runtime) {
        std::lock_guard<std::mutex> l(state_->mutex);
        std::vector<std::string> out;
        for (const ParsedFrame& f : state_->frames)
            if (f.runtime == runtime) out.push_back(describeHost(f));
        return out;
    }
    /// Whether `frame`, as `describeHost` gives it, was read on `runtime` so far.
    bool saw(uint32_t runtime, const std::string& frame) {
        std::lock_guard<std::mutex> l(state_->mutex);
        return state_->seen(runtime, frame);
    }
    /// `saw`, waited for up to `bound`.
    bool sawWithin(uint32_t runtime, const std::string& frame, std::chrono::milliseconds bound) {
        std::unique_lock<std::mutex> l(state_->mutex);
        return state_->changed.wait_for(l, bound, [&] { return state_->seen(runtime, frame); });
    }
    /// The first frame the host's end wrote, as the documented layout reads it.
    std::optional<ParsedFrame> first() {
        std::lock_guard<std::mutex> l(state_->mutex);
        return state_->frames.empty() ? std::nullopt : std::optional<ParsedFrame>(state_->frames.front());
    }
    /// Why the host's end said its stream ended, once it did within `bound`.
    std::optional<std::string> ended(std::chrono::milliseconds bound) {
        std::unique_lock<std::mutex> l(state_->mutex);
        state_->changed.wait_for(l, bound, [&] { return state_->ended.has_value(); });
        return state_->ended;
    }
    /// Stops the host's end (again: a no-op), then waits up to `bound` for the worker to return: true once it did and was joined, false while it still reads (a later call waits again).
    bool finish(std::chrono::milliseconds bound) {
        end_->stop();
        if (!reader_.joinable()) return true;
        {
            std::unique_lock<std::mutex> l(state_->mutex);
            if (!state_->changed.wait_for(l, bound, [&] { return !state_->reading; })) return false;
        }
        reader_.join();
        return true;
    }

private:
    /// What the worker shares with this end: the pair, the frames read, the end's reason and the hold, under one lock.
    struct State {
        explicit State(size_t capacity) : pair(memoryStreamPair(capacity)) {}

        bool seen(uint32_t runtime, const std::string& frame) const {
            for (const ParsedFrame& f : frames)
                if (f.runtime == runtime && describeHost(f) == frame) return true;
            return false;
        }

        MemoryStreamPair pair;
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<ParsedFrame> frames;
        std::optional<std::string> ended;
        std::function<void()> hold;
        bool reading = true;
    };

    /// `events`, its `ended` followed by the end's reason kept in `shared`.
    static SandboxRemoteHostEnd::Events recorded(std::shared_ptr<State> shared, SandboxRemoteHostEnd::Events events) {
        return {std::move(events.runtimes), [state = std::move(shared), owners = std::move(events.ended)](const std::string& why) {
                    if (owners) owners(why);
                    {
                        std::lock_guard<std::mutex> l(state->mutex);
                        if (!state->ended) state->ended = why;
                    }
                    state->changed.notify_all();
                }};
    }
    /// The worker: whole frames until the stream ends, each kept before the hold set for it.
    static void read(State& state) {
        FrameStreamReader reader(*state.pair.engine);
        while (std::optional<ParsedFrame> frame = reader.next()) {
            std::function<void()> hold;
            {
                std::lock_guard<std::mutex> l(state.mutex);
                state.frames.push_back(std::move(*frame));
                hold = std::exchange(state.hold, nullptr);
            }
            state.changed.notify_all();
            if (hold) hold();
        }
        {
            std::lock_guard<std::mutex> l(state.mutex);
            state.reading = false;
        }
        state.changed.notify_all();
    }

    const std::shared_ptr<State> state_;
    const bool hosting_;
    const std::shared_ptr<SandboxRemoteHostEnd> end_;
    std::mutex writing_;
    std::thread reader_;
};

} // namespace twidget
