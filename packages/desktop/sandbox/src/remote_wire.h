// The wire both ends of a stream read and write (twidget/remote_stream.h, `SandboxRemoteWire`): every kind's shape, the codec that sizes a frame against its bound before a byte of it is copied, the exact frame reader, and the cut and the naming both ends give a frame. Library sources only, the host's end (remote_stream.cpp) and the engine process's (remote_stream_engine.cpp); nothing under include/ names it.
#pragma once

#include "twidget/remote_engine.h"
#include "twidget/remote_stream.h"

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace twidget::detail {

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
    static constexpr size_t kStrings = 6;
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
        case Kind::Opening: return {true, H, true, "issss"};
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
        case Kind::HostingOpening: return {true, H, true, "issss"};
        case Kind::HostedStart: return {true, H, false, "sss"};
        case Kind::HostedTheme: return {true, H, false, "s"};
        case Kind::Platform: return {true, H, false, "s"};
        case Kind::PlatformAnswer: return {true, H, false, "s"};
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
        case Kind::PlatformMessage: return {true, E, false, "s"};
        case Kind::PlatformRequest: return {true, E, false, "s"};
        case Kind::ValidatedOps: return {true, E, false, "s"};
        case Kind::RendererReady: return {true, E, false, ""};
        case Kind::Rendered: return {true, E, false, ""};
        case Kind::Nodes: return {true, E, false, "i"};
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

/// An engine string cut to its limit and one byte more: still past the limit, so the client's own check gives its verdict over the loopback, and every frame fits `kMaxFrameBytes`.
inline std::string_view cut(std::string_view s, size_t limit) { return s.substr(0, limit + 1); }

inline std::string kindOn(const Frame& f) { return "kind " + std::to_string(int(f.kind)) + " on runtime " + std::to_string(f.runtime); }

} // namespace twidget::detail
