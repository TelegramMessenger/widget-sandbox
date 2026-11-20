// What the stage oracles share: records `u8 op, u32 length, bytes` on stdin, answers `u8 status, u32 length, bytes` on
// stdout, and the little-endian fields inside them (a string is a u32 length and its bytes).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace oracle {

inline bool read_exact(void* to, size_t n) {
    return n == 0 || std::fread(to, 1, n, stdin) == n;
}

/// A record's bytes, read field by field; past the end every field is zero or empty.
struct In {
    std::string_view s;
    size_t at = 0;

    uint8_t u8() { return at < s.size() ? uint8_t(s[at++]) : 0; }
    bool flag() { return u8() != 0; }
    uint32_t u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4 && at < s.size(); i++) v |= uint32_t(uint8_t(s[at++])) << (8 * i);
        return v;
    }
    int32_t i32() { return int32_t(u32()); }
    uint64_t u64() {
        uint64_t v = 0;
        for (int i = 0; i < 8 && at < s.size(); i++) v |= uint64_t(uint8_t(s[at++])) << (8 * i);
        return v;
    }
    int64_t i64() { return int64_t(u64()); }
    double f64() {
        const uint64_t v = u64();
        double d;
        std::memcpy(&d, &v, 8);
        return d;
    }
    std::string str() {
        const size_t n = std::min<size_t>(u32(), s.size() - at);
        std::string out(s.substr(at, n));
        at += n;
        return out;
    }
    /// A u8 presence flag, then the string.
    std::optional<std::string> optStr() {
        const bool has = flag();
        std::string v = str();
        return has ? std::optional<std::string>(std::move(v)) : std::nullopt;
    }
};

/// An answer's bytes, built field by field.
struct Out {
    std::string s;

    Out& u8(uint8_t v) {
        s.push_back(char(v));
        return *this;
    }
    Out& flag(bool v) { return u8(v ? 1 : 0); }
    Out& u32(uint32_t v) {
        for (int i = 0; i < 4; i++) s.push_back(char(uint8_t(v >> (8 * i))));
        return *this;
    }
    Out& i32(int32_t v) { return u32(uint32_t(v)); }
    Out& u64(uint64_t v) {
        for (int i = 0; i < 8; i++) s.push_back(char(uint8_t(v >> (8 * i))));
        return *this;
    }
    Out& i64(int64_t v) { return u64(uint64_t(v)); }
    Out& f64(double d) {
        uint64_t v;
        std::memcpy(&v, &d, 8);
        return u64(v);
    }
    Out& str(std::string_view v) {
        u32(uint32_t(v.size()));
        s.append(v);
        return *this;
    }
    /// A u8 presence flag, then the string (empty when absent).
    Out& optStr(const std::optional<std::string>& v) {
        flag(v.has_value());
        return str(v ? std::string_view(*v) : std::string_view());
    }
};

inline void answer(uint8_t status, std::string_view bytes) {
    const uint32_t n = uint32_t(bytes.size());
    const unsigned char head[5] = {status, uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)};
    std::fwrite(head, 1, 5, stdout);
    if (n) std::fwrite(bytes.data(), 1, n, stdout);
}

/// Reads records until the end of stdin and hands each to `run(op, in)`, which answers it or not (a setting).
template <class Run>
int serve(Run run) {
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
        In in{bytes};
        if (!run(op, in)) return 3;
    }
    std::fflush(stdout);
    return 0;
}

} // namespace oracle
