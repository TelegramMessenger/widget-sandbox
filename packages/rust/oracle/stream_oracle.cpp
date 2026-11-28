// src/remote_wire.h on stdin: the frame reader over byte runs and the codec, for the tests of rust/crates/stream to compare with.
//
// Each record is a u8 operation, a u32 length and its input; each answer a u8 status, a u32 length and bytes.
//   'r' FrameReader: u8 side that sends (0 host, 1 engine), u64 bound, u32 chunk (the most one read brings), then the stream's bytes
//       -> status 1 and, per frame, u8 1, u32 runtime, u8 kind, six strings (u32 length and bytes), eleven f64 bits, i64, u8 flag;
//          then u8 0 and the failure as a string ("" at the end on a frame's boundary)
//   'e' Codec::encode: u32 runtime, u8 kind, u64 bound, u8 count, each field u8 type ('s' u32 length and bytes, 'i' and 'f' 8 bytes,
//       'b' 1 byte) -> status 1 and the frame, or 0
#include "remote_wire.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace twidget;
using namespace twidget::detail;

namespace {

bool read_exact(void* to, size_t n) {
    return n == 0 || std::fread(to, 1, n, stdin) == n;
}

struct In {
    std::string_view s;
    size_t at = 0;
    uint8_t u8() { return at < s.size() ? uint8_t(s[at++]) : 0; }
    uint64_t u64(int bytes = 8) {
        uint64_t v = 0;
        for (int i = 0; i < bytes && at < s.size(); i++) v |= uint64_t(uint8_t(s[at++])) << (8 * i);
        return v;
    }
    std::string_view bytes(size_t n) {
        n = std::min(n, s.size() - at);
        auto out = s.substr(at, n);
        at += n;
        return out;
    }
};

struct Out {
    std::string s;
    void u8(uint8_t v) { s.push_back(char(v)); }
    void u64(uint64_t v, int bytes = 8) {
        for (int i = 0; i < bytes; i++) s.push_back(char(uint8_t(v >> (8 * i))));
    }
    void str(std::string_view t) {
        u64(t.size(), 4);
        s.append(t);
    }
};

void answer(uint8_t status, const std::string& bytes) {
    const uint32_t n = uint32_t(bytes.size());
    const unsigned char head[5] = {status, uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)};
    std::fwrite(head, 1, 5, stdout);
    if (n) std::fwrite(bytes.data(), 1, n, stdout);
}

struct ChunkStream final : SandboxRemoteStream {
    std::string_view bytes;
    size_t chunk = 1;
    size_t at = 0;
    size_t read(char* out, size_t size) override {
        const size_t n = std::min({size, chunk, bytes.size() - at});
        std::memcpy(out, bytes.data() + at, n);
        at += n;
        return n;
    }
    bool write(std::string_view) override { return false; }
    void shutdown() override {}
};

std::optional<std::string> encode_fields(uint32_t runtime, Kind kind, size_t bound, const std::vector<Field>& f) {
    switch (f.size()) {
    case 0: return Codec::encode(runtime, kind, bound, {});
    case 1: return Codec::encode(runtime, kind, bound, {f[0]});
    case 2: return Codec::encode(runtime, kind, bound, {f[0], f[1]});
    case 3: return Codec::encode(runtime, kind, bound, {f[0], f[1], f[2]});
    case 4: return Codec::encode(runtime, kind, bound, {f[0], f[1], f[2], f[3]});
    case 5: return Codec::encode(runtime, kind, bound, {f[0], f[1], f[2], f[3], f[4]});
    case 6: return Codec::encode(runtime, kind, bound, {f[0], f[1], f[2], f[3], f[4], f[5]});
    case 7: return Codec::encode(runtime, kind, bound, {f[0], f[1], f[2], f[3], f[4], f[5], f[6]});
    case 8: return Codec::encode(runtime, kind, bound, {f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]});
    case 9: return Codec::encode(runtime, kind, bound, {f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]});
    case 10: return Codec::encode(runtime, kind, bound, {f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]});
    case 11: return Codec::encode(runtime, kind, bound, {f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10]});
    case 12: return Codec::encode(runtime, kind, bound, {f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]});
    }
    return std::nullopt;
}

void run(int op, std::string_view input) {
    In in{input};
    Out o;
    if (op == 'r') {
        const Side from = in.u8() ? Side::Engine : Side::Host;
        const size_t bound = size_t(in.u64());
        ChunkStream stream;
        stream.chunk = std::max<size_t>(1, size_t(in.u64(4)));
        stream.bytes = input.substr(in.at);
        FrameReader reader(stream, from);
        while (reader.next(bound)) {
            const Frame& f = reader.frame();
            o.u8(1);
            o.u64(f.runtime, 4);
            o.u8(uint8_t(f.kind));
            for (const auto& s : f.str) o.str(s);
            for (double d : f.f64) {
                uint64_t bits;
                std::memcpy(&bits, &d, 8);
                o.u64(bits);
            }
            o.u64(uint64_t(f.i64));
            o.u8(f.flag ? 1 : 0);
        }
        o.u8(0);
        o.str(reader.failure());
        return answer(1, o.s);
    }
    if (op == 'e') {
        const uint32_t runtime = uint32_t(in.u64(4));
        const Kind kind = Kind(in.u8());
        const size_t bound = size_t(in.u64());
        const size_t count = in.u8();
        std::vector<std::string> keep(count);
        std::vector<Field> fields;
        for (size_t i = 0; i < count; i++) {
            const char type = char(in.u8());
            if (type == 's') {
                keep[i] = std::string(in.bytes(size_t(in.u64(4))));
                fields.emplace_back(std::string_view(keep[i]));
            } else if (type == 'i') {
                fields.emplace_back(int64_t(in.u64()));
            } else if (type == 'f') {
                const uint64_t bits = in.u64();
                double d;
                std::memcpy(&d, &bits, 8);
                fields.emplace_back(d);
            } else {
                fields.emplace_back(in.u8() != 0);
            }
        }
        auto frame = encode_fields(runtime, kind, bound, fields);
        if (!frame) return answer(0, {});
        return answer(1, *frame);
    }
    std::exit(3);
}

} // namespace

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::string bytes;
    for (int op; (op = std::fgetc(stdin)) != EOF;) {
        unsigned char b[4];
        if (!read_exact(b, 4)) return 2;
        const uint32_t n = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
        bytes.resize(n);
        if (!read_exact(bytes.data(), n)) return 2;
        run(op, bytes);
    }
    std::fflush(stdout);
    return 0;
}
