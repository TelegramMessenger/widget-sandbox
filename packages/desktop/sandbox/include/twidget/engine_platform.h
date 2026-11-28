// What the engine asks the operating system, one definition for the engine in process (nova_context.cpp, sandbox_engine.cpp) and for an engine process whose end is in Rust (its glue hands these to twidget_engine_serve); it links without Nova.
#pragma once

#include <cstddef>
#include <cstdint>

namespace twidget::platform {

/// CPU seconds the calling thread has run, unaffected by how busy the machine is (`NovaContext::threadCpuTime`).
double threadCpuSeconds();
/// std::chrono::steady_clock's seconds since its epoch, the engine's clock and the host's.
double steadySeconds();
/// Seconds east of UTC in the user's time zone at `epochSeconds`, daylight saving time included.
int32_t timeZoneOffset(int64_t epochSeconds);
/// `size` bytes of the system's CSPRNG.
void randomBytes(uint8_t* out, size_t size);

} // namespace twidget::platform
