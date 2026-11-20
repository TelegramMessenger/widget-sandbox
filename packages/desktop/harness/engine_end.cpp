// The Rust end's callbacks for engine_end.h: the stream, the C++ engine's clocks, time zone and CSPRNG (engine_platform.h), and the lockdown; built into the tests and the harness with -DTW_RUST_ENGINE_END=ON.
#include "engine_end.h"

#include "twidget/engine_platform.h"
#include "twidget_engine.h"

#include <cstdint>

namespace twidget::detail {
namespace {

size_t readStream(void* context, uint8_t* out, size_t size) { return static_cast<SandboxRemoteStream*>(context)->read(reinterpret_cast<char*>(out), size); }

bool writeStream(void* context, const uint8_t* bytes, size_t size) {
    return static_cast<SandboxRemoteStream*>(context)->write(std::string_view(reinterpret_cast<const char*>(bytes), size));
}

void shutdownStream(void* context) { static_cast<SandboxRemoteStream*>(context)->shutdown(); }

double threadCpuSeconds(void*) { return platform::threadCpuSeconds(); }

double steadySeconds(void*) { return platform::steadySeconds(); }

int32_t timeZone(void*, int64_t epochSeconds) { return platform::timeZoneOffset(epochSeconds); }

void randomBytes(void*, uint8_t* out, size_t size) { platform::randomBytes(out, size); }

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
    std::string out = reason ? std::string(reinterpret_cast<const char*>(reason), size) : std::string();
    twidget_engine_free(reason, size);
    return out;
}

} // namespace twidget::detail
