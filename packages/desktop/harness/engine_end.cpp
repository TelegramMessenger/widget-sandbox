// The Rust end's callbacks for engine_end.h: the stream, the C++ engine's clocks, its time zone and its CSPRNG, and the lockdown; built into the tests and the harness with -DTW_RUST_ENGINE_END=ON.
#include "engine_end.h"

#include "twidget/nova_context.h"
#include "twidget_engine.h"

#include <chrono>
#include <cstdint>
#include <ctime>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <stdlib.h>
#else
#include <sys/random.h>
#endif

namespace twidget::detail {
namespace {

size_t readStream(void* context, uint8_t* out, size_t size) { return static_cast<SandboxRemoteStream*>(context)->read(reinterpret_cast<char*>(out), size); }

bool writeStream(void* context, const uint8_t* bytes, size_t size) {
    return static_cast<SandboxRemoteStream*>(context)->write(std::string_view(reinterpret_cast<const char*>(bytes), size));
}

void shutdownStream(void* context) { static_cast<SandboxRemoteStream*>(context)->shutdown(); }

double threadCpuSeconds(void*) { return NovaContext::threadCpuTime(); }

double steadySeconds(void*) { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

/// nova_context.cpp's: seconds east of UTC in the user's time zone, daylight saving time included.
int32_t timeZone(void*, int64_t epochSeconds) {
#ifdef _WIN32
    (void)epochSeconds;
    TIME_ZONE_INFORMATION info{};
    const DWORD id = GetTimeZoneInformation(&info);
    if (id == TIME_ZONE_ID_INVALID) return 0;
    const LONG bias = info.Bias + ((id == TIME_ZONE_ID_DAYLIGHT) ? info.DaylightBias : 0);
    return int32_t(-bias * 60);
#else
    const time_t seconds = time_t(epochSeconds);
    struct tm local {};
    if (!localtime_r(&seconds, &local)) return 0;
    return int32_t(local.tm_gmtoff);
#endif
}

/// sandbox_engine.cpp's `randomBytes`.
void randomBytes(void*, uint8_t* out, size_t size) {
#if defined(_WIN32)
    BCryptGenRandom(nullptr, out, ULONG(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    arc4random_buf(out, size);
#else
    size_t got = 0;
    while (got < size) {
        const ssize_t r = getrandom(out + got, size - got, 0);
        if (r <= 0) break;
        got += size_t(r);
    }
#endif
}

struct Lockdown {
    const std::function<std::string()>* lockdown;
    std::string why;
};

/// remote_stream_engine.cpp's: a lockdown that throws could not lock the process down.
const uint8_t* lockDown(void* context, size_t* size) {
    auto& l = *static_cast<Lockdown*>(context);
    try {
        l.why = (*l.lockdown)();
    } catch (...) {
        l.why = "the engine process could not lock itself down";
    }
    *size = l.why.size();
    return reinterpret_cast<const uint8_t*>(l.why.data());
}

} // namespace

std::string serveInRust(SandboxRemoteStream& stream, const std::string& build, const std::function<std::string()>& lockdown) {
    const twidget_engine_stream s{&stream, &readStream, &writeStream, &shutdownStream};
    const twidget_engine_platform p{nullptr, &threadCpuSeconds, &steadySeconds, &timeZone, &randomBytes};
    Lockdown context{&lockdown, {}};
    const twidget_engine_lockdown l{&context, &lockDown};
    size_t size = 0;
    uint8_t* reason = twidget_engine_serve(&s, &p, reinterpret_cast<const uint8_t*>(build.data()), build.size(), lockdown ? &l : nullptr, &size);
    std::string out(reinterpret_cast<const char*>(reason), size);
    twidget_engine_free(reason, size);
    return out;
}

} // namespace twidget::detail
