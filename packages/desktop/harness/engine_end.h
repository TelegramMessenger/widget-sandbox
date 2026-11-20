// The engine process's end the tests and the harness serve an engine stream with: the library's (SandboxRemoteEngineEnd::serve), or with TW_ENGINE_END=rust the one in Rust (../rust, crates/engine_ffi) of a build configured with -DTW_RUST_ENGINE_END=ON; test and harness code, never the library's.
#pragma once

#include "twidget/remote_stream.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>

namespace twidget {

/// Whether the environment asks for the engine's end in Rust.
inline bool rustEngineEndRequested() {
    const char* value = std::getenv("TW_ENGINE_END");
    return value && std::string_view(value) == "rust";
}

#ifdef TW_RUST_ENGINE_END
namespace detail {
/// The Rust end's `twidget_engine_serve` with the C++ engine's clocks, time zone and CSPRNG (engine_end.cpp).
std::string serveInRust(SandboxRemoteStream& stream, const std::string& build, const std::function<std::string()>& lockdown);
} // namespace detail
#endif

/// `SandboxRemoteEngineEnd::serve`, or the Rust end's with TW_ENGINE_END=rust (a build without it exits 2).
inline std::string serveEngineEnd(SandboxRemoteStream& stream, const std::string& build, const std::function<std::string()>& lockdown = {}) {
    if (rustEngineEndRequested()) {
#ifdef TW_RUST_ENGINE_END
        return detail::serveInRust(stream, build, lockdown);
#else
        std::fprintf(stderr, "TW_ENGINE_END=rust needs a build configured with -DTW_RUST_ENGINE_END=ON\n");
        std::exit(2);
#endif
    }
    return SandboxRemoteEngineEnd::serve(stream, build, lockdown);
}

} // namespace twidget
