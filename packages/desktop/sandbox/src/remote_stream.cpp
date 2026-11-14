#include "twidget/remote_stream.h"

#include "twidget/json.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
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

// MARK: the wire

namespace {

using Kind = SandboxRemoteWire::Kind;

/// The side that sends a kind.
enum class Side : uint8_t { Host, Engine };

/// A kind's row of `SandboxRemoteWire`'s table: who sends it, whether on the connection (runtime 0) or on a runtime, and its fields in order, a letter each: `s` str, `i` i64, `f` f64, `b` bool.
struct Shape {
    bool known = false;
    Side from = Side::Host;
    bool connection = false;
    std::string_view fields;
};

/// One frame's fields, checked against its kind's shape: the strings in order, then the numbers; no kind has more than one `i64` or `bool`.
struct Frame {
    static constexpr size_t kStrings = 5;
    uint32_t runtime = 0;
    Kind kind = Kind::Opening;
    std::array<std::string_view, kStrings> str;
    std::array<double, SandboxRemoteCoding::kStatsFields> f64{};
    int64_t i64 = 0;
    bool flag = false;
};

/// One field of a frame to encode, typed as the wire types it: an integer of another type than `int64_t` does not convert.
struct Field {
    Field(std::string_view s) : type('s'), str(s) {}
    Field(const std::string& s) : type('s'), str(s) {}
    Field(const char* s) : type('s'), str(s) {}
    Field(int64_t v) : type('i'), bits(std::bit_cast<uint64_t>(v)) {}
    Field(double v) : type('f'), bits(std::bit_cast<uint64_t>(v)) {}
    Field(bool v) : type('b'), bits(v ? 1 : 0) {}

    char type;
    std::string_view str;
    uint64_t bits = 0;
};

/// The table both ends read and write frames by: one shape per kind, a frame sized against its bound before a byte of it is copied.
struct Codec {
    /// The size in front of a frame.
    static constexpr size_t kSizeBytes = 4;
    /// The runtime id and the kind, the least a frame's size counts.
    static constexpr size_t kHeadBytes = 5;

    static constexpr Shape shape(uint8_t kind) {
        constexpr Side H = Side::Host, E = Side::Engine;
        switch (Kind(kind)) {
        case Kind::Opening: return {true, H, true, "isssss"};
        case Kind::Open: return {true, H, false, ""};
        case Kind::Start: return {true, H, false, "ss"};
        case Kind::Event: return {true, H, false, "s"};
        case Kind::Activation: return {true, H, false, "si"};
        case Kind::AnimationFrame: return {true, H, false, "f"};
        case Kind::Viewport: return {true, H, false, "fff"};
        case Kind::Resize: return {true, H, false, "s"};
        case Kind::Theme: return {true, H, false, "ss"};
        case Kind::SafeArea: return {true, H, false, "s"};
        case Kind::Data: return {true, H, false, "s"};
        case Kind::Visible: return {true, H, false, "b"};
        case Kind::PressBegan: return {true, H, false, ""};
        case Kind::PressEnded: return {true, H, false, ""};
        case Kind::ChargeValidator: return {true, H, false, "f"};
        case Kind::Stop: return {true, H, false, "s"};
        case Kind::Answer: return {true, H, false, "is"};
        case Kind::Probe: return {true, H, false, "is"};
        case Kind::QueryAnswer: return {true, H, false, "sbs"};
        case Kind::IntlAnswer: return {true, H, false, "s"};
        case Kind::Close: return {true, H, false, ""};
        case Kind::Ready: return {true, E, true, "i"};
        case Kind::Refused: return {true, E, true, "s"};
        case Kind::TaskStarted: return {true, E, false, ""};
        case Kind::TaskCompleted: return {true, E, false, ""};
        case Kind::Ops: return {true, E, false, "s"};
        case Kind::Query: return {true, E, false, "s"};
        case Kind::Intl: return {true, E, false, "s"};
        case Kind::Listen: return {true, E, false, "s"};
        case Kind::PreventDefault: return {true, E, false, "s"};
        case Kind::Anim: return {true, E, false, "s"};
        case Kind::RequestsFrame: return {true, E, false, ""};
        case Kind::Log: return {true, E, false, "ss"};
        case Kind::Diag: return {true, E, false, "ss"};
        case Kind::Bridge: return {true, E, false, "ss"};
        case Kind::CopyText: return {true, E, false, "s"};
        case Kind::Request: return {true, E, false, "isb"};
        case Kind::Wallet: return {true, E, false, "is"};
        case Kind::Storage: return {true, E, false, "s"};
        case Kind::ResizeAnchor: return {true, E, false, "s"};
        case Kind::WatchesPublicData: return {true, E, false, "b"};
        case Kind::Loaded: return {true, E, false, ""};
        case Kind::Stats: return {true, E, false, "fffffffffff"};
        case Kind::Died: return {true, E, false, "s"};
        case Kind::ProbeAnswer: return {true, E, false, "is"};
        }
        return {};
    }

    /// Whether every kind's fields fit a `Frame`.
    static constexpr bool shapesFit() {
        for (int kind = 0; kind <= UINT8_MAX; kind++) {
            const std::string_view fields = shape(uint8_t(kind)).fields;
            const auto count = [&](char type) { return size_t(std::count(fields.begin(), fields.end(), type)); };
            if (count('s') > Frame::kStrings || count('f') > SandboxRemoteCoding::kStatsFields || count('i') > 1 || count('b') > 1) return false;
        }
        return true;
    }

    /// A whole frame of `kind` on `runtime`, size first, or nullopt when its size would pass `bound` or `fields` are not the kind's.
    static std::optional<std::string> encode(uint32_t runtime, Kind kind, size_t bound, std::initializer_list<Field> fields) {
        const Shape s = shape(uint8_t(kind));
        if (!s.known || fields.size() != s.fields.size()) return std::nullopt;
        bound = std::min<size_t>(bound, UINT32_MAX);
        if (bound < kHeadBytes) return std::nullopt;
        // Every addition is checked against what is left of the bound, so nothing wraps.
        size_t size = kHeadBytes;
        const auto add = [&](size_t bytes) {
            if (bytes > bound - size) return false;
            size += bytes;
            return true;
        };
        size_t i = 0;
        for (const Field& f : fields) {
            if (f.type != s.fields[i++]) return std::nullopt;
            const bool fits = f.type == 's' ? add(4) && add(f.str.size()) : add(f.type == 'b' ? 1 : 8);
            if (!fits) return std::nullopt;
        }
        std::string out;
        out.reserve(kSizeBytes + size);
        putU32(out, uint32_t(size));
        putU32(out, runtime);
        out.push_back(char(uint8_t(kind)));
        for (const Field& f : fields) {
            switch (f.type) {
            case 's':
                putU32(out, uint32_t(f.str.size()));
                out.append(f.str);
                break;
            case 'b': out.push_back(char(uint8_t(f.bits))); break;
            default: putU64(out, f.bits); break;
            }
        }
        return out;
    }

    static void putU32(std::string& out, uint32_t v) {
        for (int shift = 0; shift < 32; shift += 8) out.push_back(char(uint8_t(v >> shift)));
    }
    static void putU64(std::string& out, uint64_t v) {
        for (int shift = 0; shift < 64; shift += 8) out.push_back(char(uint8_t(v >> shift)));
    }
};

static_assert(Codec::shapesFit());

/// A frame's body read field by field, each against what is left of it.
class FieldReader {
public:
    explicit FieldReader(std::string_view body) : rest_(body) {}

    bool u8(uint8_t& v) {
        if (rest_.empty()) return false;
        v = uint8_t(rest_[0]);
        rest_.remove_prefix(1);
        return true;
    }
    bool u32(uint32_t& v) { return little(v); }
    bool u64(uint64_t& v) { return little(v); }
    /// A length, then that many bytes, viewed where they are.
    bool str(std::string_view& v) {
        uint32_t length = 0;
        if (!u32(length) || length > rest_.size()) return false;
        v = rest_.substr(0, length);
        rest_.remove_prefix(length);
        return true;
    }
    bool done() const { return rest_.empty(); }

private:
    template <class U>
    bool little(U& v) {
        if (rest_.size() < sizeof(U)) return false;
        v = 0;
        for (size_t i = 0; i < sizeof(U); i++) v |= U(uint8_t(rest_[i])) << (8 * i);
        rest_.remove_prefix(sizeof(U));
        return true;
    }

    std::string_view rest_;
};

/// The frames one side of a stream sends, read exactly: the size first, checked against its bound before the body is read or room made for it, then every field against its kind's shape. A buffer of at most one frame (and one read's room) holds what came; after a false `next` the stream is done.
class FrameReader {
public:
    FrameReader(SandboxRemoteStream& stream, Side from) : stream_(stream), from_(from) {}

    /// The next frame, true; false at the end of the stream on a frame's boundary (`failure()` empty) or with why it cannot go on.
    bool next(size_t bound) {
        failure_.clear();
        if (!fill(Codec::kSizeBytes)) {
            if (end_ != begin_) failure_ = "the stream ended inside a frame's size";
            return false;
        }
        uint32_t size = 0;
        FieldReader(std::string_view(buffer_.data() + begin_, Codec::kSizeBytes)).u32(size);
        if (size < Codec::kHeadBytes) return fail("a frame of " + std::to_string(size) + " bytes, less than its runtime and kind");
        if (size > bound) return fail("a frame of " + std::to_string(size) + " bytes, past the bound of " + std::to_string(bound));
        if (!fill(Codec::kSizeBytes + size_t(size))) return fail("the stream ended inside a frame");
        const std::string_view body(buffer_.data() + begin_ + Codec::kSizeBytes, size);
        begin_ += Codec::kSizeBytes + size_t(size);
        return decode(body);
    }
    /// Valid until the next `next`: its strings view this reader's buffer.
    const Frame& frame() const { return frame_; }
    const std::string& failure() const { return failure_; }

private:
    /// What one read may bring past the frame being read, and the buffer kept between frames.
    static constexpr size_t kReadChunk = size_t(64) << 10;

    bool fail(std::string why) {
        failure_ = std::move(why);
        return false;
    }

    /// Until `bytes` unread bytes are here, false when the stream ends first; room is made for `bytes` only, never for what a size has not yet been checked for.
    bool fill(size_t bytes) {
        if (end_ - begin_ >= bytes) return true;
        const size_t rest = end_ - begin_;
        if (begin_ && rest) std::memmove(buffer_.data(), buffer_.data() + begin_, rest);
        begin_ = 0;
        end_ = rest;
        // The room a large frame took is given back before the next one.
        if (buffer_.size() > kReadChunk && std::max(rest, bytes) <= kReadChunk) {
            buffer_.resize(kReadChunk);
            buffer_.shrink_to_fit();
        }
        if (buffer_.size() < bytes) buffer_.resize(std::max(bytes, kReadChunk));
        while (end_ < bytes) {
            const size_t room = buffer_.size() - end_;
            const size_t got = std::min(stream_.read(buffer_.data() + end_, room), room);
            if (!got) return false;
            end_ += got;
        }
        return true;
    }

    bool decode(std::string_view body) {
        FieldReader in(body);
        Frame frame;
        uint8_t kind = 0;
        in.u32(frame.runtime);
        in.u8(kind);
        const Shape s = Codec::shape(kind);
        const auto bad = [&](const std::string& what) { return fail("kind " + std::to_string(kind) + " " + what); };
        if (!s.known) return fail("an unknown kind " + std::to_string(kind));
        if (s.from != from_) return bad("from the other side");
        if (s.connection && frame.runtime) return bad("on runtime " + std::to_string(frame.runtime) + ", not the connection");
        if (!s.connection && !frame.runtime) return bad("on the connection, not a runtime");
        frame.kind = Kind(kind);
        size_t strs = 0, numbers = 0;
        for (const char type : s.fields) {
            bool read = false;
            uint64_t bits = 0;
            uint8_t byte = 0;
            switch (type) {
            case 's': read = in.str(frame.str[strs++]); break;
            case 'i':
                read = in.u64(bits);
                frame.i64 = std::bit_cast<int64_t>(bits);
                break;
            case 'f':
                read = in.u64(bits);
                frame.f64[numbers++] = std::bit_cast<double>(bits);
                break;
            default:
                read = in.u8(byte);
                if (read && byte > 1) return bad("with a bool of " + std::to_string(byte));
                frame.flag = byte == 1;
                break;
            }
            if (!read) return bad("with a field past the frame's end");
        }
        if (!in.done()) return bad("with bytes after its fields");
        switch (frame.kind) {
        case Kind::Activation:
            if (frame.i64 < INT_MIN || frame.i64 > INT_MAX) return bad("with a gesture outside int");
            break;
        case Kind::Probe:
        case Kind::ProbeAnswer:
            if (frame.i64 < 1) return bad("with a sequence below 1");
            break;
        case Kind::QueryAnswer:
            if (!frame.flag && !frame.str[1].empty()) return bad("with binary bytes but no binary");
            break;
        default: break;
        }
        frame_ = frame;
        return true;
    }

    SandboxRemoteStream& stream_;
    const Side from_;
    /// Bytes [begin_, end_) came and are not read yet; `frame_`'s strings view what is before `begin_`.
    std::string buffer_;
    size_t begin_ = 0;
    size_t end_ = 0;
    Frame frame_;
    std::string failure_;
};

} // namespace

// MARK: the engine process's end

namespace {

/// What a query that gets no answer returns to the engine, as a client's dead engine answers it.
constexpr const char* kDeadQuery = "{\"error\":\"dead\"}";
/// Bytes of the host's text a refusal quotes.
constexpr size_t kQuotedBytes = 64;

/// An engine string cut to its limit and one byte more: still past the limit, so the client's own check gives its verdict over the loopback, and every frame fits `kMaxFrameBytes`.
std::string_view cut(std::string_view s, size_t limit) { return s.substr(0, limit + 1); }

std::string_view message(const std::string& s) { return cut(s, SandboxEngine::kRemoteMessageLimit); }

std::string inQuotes(std::string_view s) { return "\"" + std::string(s.substr(0, kQuotedBytes)) + (s.size() > kQuotedBytes ? "...\"" : "\""); }

std::string kindOn(const Frame& f) { return "kind " + std::to_string(int(f.kind)) + " on runtime " + std::to_string(f.runtime); }

std::string undecodable(const Frame& f) { return kindOn(f) + " with a form that does not decode"; }

/// Why the engine's end refuses a first frame, "" for an opening it serves: another kind, another version or build, a stamp past `kMaxStampBytes` or of more than one line (never cut into another stamp).
std::string refusalOf(const Frame& f, const std::string& build) {
    if (f.kind != Kind::Opening) return "kind " + std::to_string(int(f.kind)) + " first, not the opening";
    if (f.i64 != SandboxRemoteWire::kVersion) return "stream version " + std::to_string(f.i64) + ", this end's is " + std::to_string(SandboxRemoteWire::kVersion);
    if (f.str[0] != build) return "build " + inQuotes(f.str[0]) + ", this end's is " + inQuotes(build);
    for (const auto& [name, stamp] : {std::pair<std::string_view, std::string_view>{"validator", f.str[3]}, {"renderer", f.str[4]}}) {
        if (stamp.size() > SandboxRemoteWire::kMaxStampBytes)
            return "a " + std::string(name) + " stamp of " + std::to_string(stamp.size()) + " bytes, past the bound of " + std::to_string(SandboxRemoteWire::kMaxStampBytes);
        if (stamp.find('\n') != std::string_view::npos) return "a " + std::string(name) + " stamp of more than one line";
    }
    return {};
}

/// The runtimes' resources from the opening alone, no file read: the sandbox bundle and its bytecode, and the validator's and renderer's stamp lines for `BundleStamp`'s checks at boot.
std::shared_ptr<SandboxResources> resourcesOf(const Frame& f) {
    auto resources = std::make_shared<SandboxResources>();
    resources->sandboxBundle = std::string(f.str[1]);
    resources->sandboxBytecode.assign(f.str[2].begin(), f.str[2].end());
    resources->validatorBundle = std::string(f.str[3]);
    resources->rendererBundle = std::string(f.str[4]);
    return resources;
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

/// One runtime's synchronous request at a time: marked before it is written, filled by the reader with an answer of its kind only, given up once killed (the runtime closed or the stream ended). Its lock is never held across a write or a call out.
class SyncSlot {
public:
    enum class Wait : uint8_t { None, Query, Intl };

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

/// A runtime's sink on the engine's side: each call of its server's goes out as its frame, every string cut to its limit and one byte more; a query or an Intl request waits for its answer in its slot, and once killed nothing more goes out.
class EngineSink final : public SandboxRemoteSink {
public:
    EngineSink(uint32_t runtime, std::shared_ptr<Output> output) : runtime_(runtime), output_(std::move(output)) {}

    void remoteTaskStarted() override { send(Kind::TaskStarted, {}); }
    void remoteTaskCompleted() override { send(Kind::TaskCompleted, {}); }
    void remoteOps(const std::string& json) override { send(Kind::Ops, {cut(json, SandboxEngine::kRemoteOpsLimit)}); }
    QueryAnswer remoteQuery(const std::string& json) override { return request(SyncSlot::Wait::Query, Kind::Query, json).value_or(QueryAnswer{kDeadQuery, std::nullopt}); }
    std::string remoteIntl(const std::string& requestJson) override {
        std::optional<QueryAnswer> answer = request(SyncSlot::Wait::Intl, Kind::Intl, requestJson);
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

    /// The reader's: the host's answer to a request of `kind`, for the one this runtime waits for.
    void answer(SyncSlot::Wait kind, QueryAnswer answer) { slot_.fill(kind, std::move(answer)); }
    /// A probe thread's: the server's answer to probe `seq`.
    void probed(int64_t seq, const std::string& text) { send(Kind::ProbeAnswer, {seq, message(text)}); }
    /// The runtime closed or the stream ended: a waiting request gets no answer, and nothing more goes out.
    void kill() { slot_.kill(); }

private:
    bool send(Kind kind, std::initializer_list<Field> fields) { return !slot_.killed() && output_->send(runtime_, kind, fields); }

    /// Marked waiting before its frame is written, so the reader finds it whenever the answer comes; nullopt once the runtime or the stream is gone.
    std::optional<QueryAnswer> request(SyncSlot::Wait wait, Kind kind, const std::string& text) {
        if (!slot_.begin(wait)) return std::nullopt;
        if (!send(kind, {message(text)})) {
            slot_.cancel();
            return std::nullopt;
        }
        return slot_.wait();
    }

    const uint32_t runtime_;
    const std::shared_ptr<Output> output_;
    SyncSlot slot_;
};

/// One runtime on the engine's side: its server, the sink the server writes through and, once probed, the thread its probes run on. The reader opens and closes it; `widget.reaper` destroys it, its probes' thread first.
struct Route {
    std::shared_ptr<EngineSink> sink;
    std::unique_ptr<SandboxRemoteServer> server;
    std::unique_ptr<JSThread> probes;
};

/// `SandboxRemoteEngineEnd::serve` on the thread that reads, which alone keeps the runtimes by id and the reaper.
class Serving {
public:
    Serving(SandboxRemoteStream& stream, std::string build, const std::function<std::string()>& lockdown)
        : stream_(stream), build_(std::move(build)), lockdown_(lockdown), output_(std::make_shared<Output>(stream)) {}

    std::string run() {
        FrameReader reader(stream_, Side::Host);
        if (!reader.next(SandboxRemoteWire::kMaxOpeningBytes)) return refuse(reader.failure());
        if (std::string why = refusalOf(reader.frame(), build_); !why.empty()) return refuse(why);
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

    /// One frame for the runtimes, in the host's order: why it cannot be accepted, "" when it was (a closed runtime's is dropped).
    std::string dispatch(const Frame& f) {
        if (f.kind == Kind::Opening) return "kind 1 after the opening";
        if (f.kind == Kind::Open) return open(f.runtime);
        const auto it = routes_.find(f.runtime);
        if (it == routes_.end()) return f.runtime > lastOpened_ ? kindOn(f) + ", which was never opened" : std::string();
        const std::shared_ptr<Route> route = it->second;
        SandboxRemoteServer& server = *route->server;
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
        case Kind::Probe: probe(route, f.i64, text(0)); break;
        case Kind::QueryAnswer: {
            std::optional<std::vector<uint8_t>> binary;
            if (f.flag) binary.emplace(f.str[1].begin(), f.str[1].end());
            route->sink->answer(SyncSlot::Wait::Query, QueryAnswer{text(0), std::move(binary)});
            break;
        }
        case Kind::IntlAnswer: route->sink->answer(SyncSlot::Wait::Intl, QueryAnswer{text(0), std::nullopt}); break;
        case Kind::Close:
            routes_.erase(it);
            retire(route);
            break;
        default: break;
        }
        return {};
    }

    /// A runtime whose id is above every earlier one, its server not started.
    std::string open(uint32_t runtime) {
        if (runtime <= lastOpened_) return "kind 2 on runtime " + std::to_string(runtime) + ", not above the last opened runtime " + std::to_string(lastOpened_);
        lastOpened_ = runtime;
        auto route = std::make_shared<Route>();
        route->sink = std::make_shared<EngineSink>(runtime, output_);
        route->server = std::make_unique<SandboxRemoteServer>(route->sink, resources_);
        routes_.emplace(runtime, std::move(route));
        return {};
    }

    /// On the runtime's `widget.probe` thread, made at its first probe, so the reader goes on answering the queries the probe may wait for.
    void probe(const std::shared_ptr<Route>& route, int64_t seq, std::string code) {
        if (!route->probes) route->probes = std::make_unique<JSThread>("widget.probe");
        route->probes->async([route, seq, code = std::move(code)] { route->sink->probed(seq, route->server->probe(code)); });
    }

    /// In the host's order, a waiting request given up before the server closes; `widget.reaper` joins the probes' thread, then destroys the server, never on the reader or the runtime's own thread.
    void retire(std::shared_ptr<Route> route) {
        route->sink->kill();
        route->server->close();
        if (!reaper_) reaper_ = std::make_unique<JSThread>("widget.reaper");
        reaper_->async([route = std::move(route)] {
            route->probes.reset();
            route->server.reset();
        });
    }

    /// The writes stop and a blocked one wakes before any lock is taken, then every runtime is retired and the reaper drained.
    void end() {
        output_->breakOff();
        for (const auto& entry : routes_) retire(entry.second);
        routes_.clear();
        if (reaper_) reaper_->join();
        reaper_.reset();
    }

    SandboxRemoteStream& stream_;
    const std::string build_;
    const std::function<std::string()>& lockdown_;
    const std::shared_ptr<Output> output_;
    std::shared_ptr<SandboxResources> resources_;
    std::map<uint32_t, std::shared_ptr<Route>> routes_;
    uint32_t lastOpened_ = 0;
    std::unique_ptr<JSThread> reaper_;
};

} // namespace

std::string SandboxRemoteEngineEnd::serve(SandboxRemoteStream& stream, const std::string& build, const std::function<std::string()>& lockdown) { return Serving(stream, build, lockdown).run(); }

// MARK: the host's end

namespace {

constexpr const char* kNoRuntime = "ERROR no runtime";
constexpr const char* kProbeTimeout = "ERROR timeout";
constexpr const char* kHostStopped = "the host stopped the stream";
constexpr const char* kEngineClosed = "the engine process closed the stream";
constexpr const char* kWriteFailed = "a write to the stream failed";
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

/// The synchronous sink call a runtime's `widget.answer` makes.
enum class Pending : uint8_t { None, Query, Intl };

/// One runtime on the host's side, made with its channel: the connection's mutex guards every field but the answer thread's three, which `answering` guards.
struct HostRoute {
    /// New until `attach`, Live while its id is registered, Leaving while its client hears that the stream is lost, then Gone.
    enum class State : uint8_t { New, Live, Leaving, Gone };

    State state = State::New;
    uint32_t id = 0;
    std::shared_ptr<SandboxRemoteSink> sink;
    /// A query or an Intl request in the host's hands: until its answer is queued, the runtime may send nothing but a probe's answer.
    Pending pending = Pending::None;
    /// The last probe sequence sent; the one `probe` waits for (0: none) and its answer.
    int64_t lastProbe = 0;
    int64_t waiting = 0;
    std::optional<std::string> probed;
    /// That answer when it came while `pending`: published once the sink's answer was queued, dropped when it cannot be.
    std::optional<std::string> deferred;
    std::condition_variable settled;

    std::mutex answering;
    /// Made at the runtime's first synchronous call; the channel's destructor takes and joins it, after which nothing is posted.
    std::unique_ptr<JSThread> answerer;
    bool retired = false;
};

/// `SandboxRemoteHostEnd::refusing`'s channel: its client dies with the reason at `attach`, and no call reaches an engine.
class RefusingChannel final : public SandboxRemoteChannel {
public:
    explicit RefusingChannel(std::string reason) : reason_(std::move(reason)) {}

    void attach(std::shared_ptr<SandboxRemoteSink> sink) override {
        if (sink) sink->remoteDied(reason_);
    }
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

    Connection(std::shared_ptr<SandboxRemoteStream> stream, Events events, const Limits& limits)
        : stream_(*stream), events_(std::move(events)), probeWait_(durationOf(boundMs(limits.probeWaitMs, Limits{}.probeWaitMs))),
          stallMs_(boundMs(limits.writeStallMs, Limits{}.writeStallMs)), stall_(durationOf(stallMs_)), queueBytes_(limits.queuedBytes ? limits.queuedBytes : Limits{}.queuedBytes),
          held_(std::move(stream)) {}

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

    /// `widget.monitor`'s loop: ends the stream when one write outlasts its bound, with no other call needed, and alone reports to the owner outside the lock: the open count as it changes, then the end once and last.
    void watch() {
        size_t reported = 0;
        std::unique_lock<std::mutex> l(mutex_);
        for (;;) {
            if (writing_ && !ending_ && Clock::now() - writingSince_ >= stall_) {
                endLocked("a write to the stream blocked for more than " + Json::number(stallMs_) + " ms");
                continue;
            }
            if (routes_.size() != reported) {
                reported = routes_.size();
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

    /// The opening, the first frame queued: the sandbox bundle and its bytecode, and only the other bundles' stamps.
    void open(const SandboxResources& r, const std::string& build) {
        const std::string_view bytecode(reinterpret_cast<const char*>(r.sandboxBytecode.data()), r.sandboxBytecode.size());
        std::optional<std::string> opening = Codec::encode(0, Kind::Opening, SandboxRemoteWire::kMaxOpeningBytes,
                                                           {SandboxRemoteWire::kVersion, build, r.sandboxBundle, bytecode, stampOf(r.validatorBundle), stampOf(r.rendererBundle)});
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

    /// A form of the host's that does not decode or fit a frame: the route goes as `close` lets it go, and its client hears that the stream is lost, as over the loopback.
    void lose(const std::shared_ptr<HostRoute>& route) {
        std::shared_ptr<SandboxRemoteSink> sink;
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (route->state != HostRoute::State::Live) return;
            removeLocked(*route);
            sink = route->sink;
        }
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
    /// One checked frame of the engine's, in its order: why the stream cannot go on, "" when it was given to its runtime or dropped (its runtime closed).
    std::string accept(const Frame& f) {
        // What a synchronous call or a probe keeps is copied before the lock, cut as the engine's end cuts it.
        std::string kept;
        if (f.kind == Kind::Query || f.kind == Kind::Intl || f.kind == Kind::ProbeAnswer) kept = std::string(cut(f.str[0], SandboxEngine::kRemoteMessageLimit));
        std::shared_ptr<HostRoute> route;
        std::shared_ptr<SandboxRemoteSink> sink;
        uint32_t id = 0;
        {
            std::lock_guard<std::mutex> l(mutex_);
            // The end stops delivery at once, also of frames already read.
            if (ending_) return reason_;
            if (!f.runtime) return connectionLocked(f);
            if (!ready_) return kindOn(f) + " before the opening was answered";
            const auto it = routes_.find(f.runtime);
            if (it == routes_.end()) return f.runtime > lastId_ ? kindOn(f) + ", which was never opened" : std::string();
            route = it->second;
            if (f.kind == Kind::ProbeAnswer) return probedLocked(*route, f, std::move(kept));
            if (route->pending != Pending::None) return kindOn(f) + (route->pending == Pending::Query ? " while its query is unanswered" : " while its Intl request is unanswered");
            if (f.kind == Kind::Query) route->pending = Pending::Query;
            if (f.kind == Kind::Intl) route->pending = Pending::Intl;
            sink = route->sink;
            id = route->id;
        }
        if (f.kind == Kind::Query || f.kind == Kind::Intl) post(route, std::move(sink), id, f.kind, std::move(kept));
        else deliver(*sink, f);
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

    /// A probe's answer: for a sequence never sent the stream ends; a late or repeated one is dropped and answers no other wait; one that comes while a synchronous call is in the host's hands waits for it.
    std::string probedLocked(HostRoute& route, const Frame& f, std::string answer) {
        if (f.i64 > route.lastProbe) return kindOn(f) + " for probe " + std::to_string(f.i64) + ", which was never sent";
        if (f.i64 != route.waiting || route.probed || route.deferred) return {};
        if (route.pending != Pending::None) {
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
        default: break;
        }
    }

    /// A query or an Intl request to the route's `widget.answer`, made at its first, so the reader goes on for the other runtimes while the client answers; dropped once the channel is destroyed.
    void post(const std::shared_ptr<HostRoute>& route, std::shared_ptr<SandboxRemoteSink> sink, uint32_t id, Kind kind, std::string request) {
        std::lock_guard<std::mutex> l(route->answering);
        if (route->retired) return;
        if (!route->answerer) route->answerer = std::make_unique<JSThread>("widget.answer");
        route->answerer->async([self = shared_from_this(), route, sink = std::move(sink), id, kind, request = std::move(request)] {
            std::optional<std::string> frame;
            if (kind == Kind::Query) {
                const QueryAnswer answer = sink->remoteQuery(request);
                const std::string_view binary = answer.binary ? std::string_view(reinterpret_cast<const char*>(answer.binary->data()), answer.binary->size()) : std::string_view();
                frame = Codec::encode(id, Kind::QueryAnswer, SandboxRemoteWire::kMaxFrameBytes, {answer.json, answer.binary.has_value(), binary});
            } else {
                frame = Codec::encode(id, Kind::IntlAnswer, SandboxRemoteWire::kMaxFrameBytes, {sink->remoteIntl(request)});
            }
            self->answered(route, std::move(frame));
        });
    }

    /// The sink's answer queued and the request settled in one step, then a probe's answer that waited for it, only once that answer was queued; an answer past the frame bound loses the route.
    void answered(const std::shared_ptr<HostRoute>& route, std::optional<std::string> frame) {
        {
            std::lock_guard<std::mutex> l(mutex_);
            route->pending = Pending::None;
            std::optional<std::string> deferred = std::move(route->deferred);
            route->deferred.reset();
            if (frame) {
                // Not queued (the route closed, lost or leaving with the stream, or an answer the stream cannot take): the probe's answer is dropped, and the probe answers as the route's end does, once its client heard any loss.
                if (route->state == HostRoute::State::Live && queueLocked(std::move(*frame)) && deferred) {
                    route->probed = std::move(deferred);
                    route->settled.notify_all();
                }
                return;
            }
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

/// A runtime's channel on the stream: each call goes out as its frame, its forms coded as the loopback codes them (one that does not decode again, or a frame past the bound, loses the runtime); the destructor, on the owner's thread, closes and joins `widget.answer`.
class SandboxRemoteHostEnd::Connection::Channel final : public SandboxRemoteChannel {
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
    auto connection = std::make_shared<Connection>(std::move(stream), std::move(events), limits);
    connection->open(resources, build);
    return std::shared_ptr<SandboxRemoteHostEnd>(new SandboxRemoteHostEnd(std::move(connection)));
}

SandboxRemoteHostEnd::~SandboxRemoteHostEnd() { stop(); }

std::shared_ptr<SandboxRemoteChannel> SandboxRemoteHostEnd::channel() { return std::make_shared<Connection::Channel>(connection_); }

void SandboxRemoteHostEnd::stop() {
    connection_->end(kHostStopped);
    reader_->join();
    writer_->join();
    monitor_->join();
    connection_->release();
}

std::shared_ptr<SandboxRemoteChannel> SandboxRemoteHostEnd::refusing(std::string reason) { return std::make_shared<RefusingChannel>(std::move(reason)); }

} // namespace twidget
