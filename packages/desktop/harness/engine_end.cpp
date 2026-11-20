// The Rust end's callbacks for engine_end.h: the stream, the C++ engine's clocks, time zone and CSPRNG (engine_platform.h), the lockdown, and a hosting stream's C++ renderers behind the C renderer factory; built into the tests and the harness with -DTW_RUST_ENGINE_END=ON.
#include "engine_end.h"

#include "twidget/engine_platform.h"
#include "twidget_engine.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

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

const uint8_t* bytesOf(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

std::string textOf(const uint8_t* bytes, size_t size) { return bytes ? std::string(reinterpret_cast<const char*>(bytes), size) : std::string(); }

std::string textOf(const twidget_engine_bytes& b) { return textOf(b.bytes, b.size); }

/// The served end's leftover: what a serve returned, freed.
std::string taken(uint8_t* reason, size_t size) {
    std::string out = textOf(reason, size);
    twidget_engine_free(reason, size);
    return out;
}

/// A hosted widget's platform over the Rust end's handle, released with its last owner (a renderer may keep it past its widget).
class RustHostedPlatform final : public SandboxHostedPlatform {
public:
    explicit RustHostedPlatform(twidget_hosted_platform* handle) : handle_(handle) {}
    ~RustHostedPlatform() override { twidget_hosted_platform_release(handle_); }
    RustHostedPlatform(const RustHostedPlatform&) = delete;
    RustHostedPlatform& operator=(const RustHostedPlatform&) = delete;

    void send(const std::string& message) override { twidget_hosted_platform_send(handle_, bytesOf(message), message.size()); }
    std::optional<std::string> request(const std::string& message) override {
        uint8_t* answer = nullptr;
        size_t size = 0;
        if (!twidget_hosted_platform_request(handle_, bytesOf(message), message.size(), &answer, &size)) return std::nullopt;
        return taken(answer, size);
    }

private:
    twidget_hosted_platform* const handle_;
};

/// One C++ renderer behind its C table, with the platform its host's messages reach it through.
struct RustHostedRenderer {
    std::unique_ptr<Renderer> renderer;
    std::shared_ptr<RustHostedPlatform> platform;
};

RustHostedRenderer& rendererOf(void* context) { return *static_cast<RustHostedRenderer*>(context); }

void rendererLoad(void* context) { rendererOf(context).renderer->load(); }

void rendererSend(void* context, const uint8_t* json, size_t size) { rendererOf(context).renderer->send(textOf(json, size)); }

void rendererSetTheme(void* context, const uint8_t* theme, size_t size) { rendererOf(context).renderer->setTheme(textOf(theme, size)); }

void rendererSetWidth(void* context, double width) { rendererOf(context).renderer->setWidth(width); }

void rendererDestroy(void* context) { rendererOf(context).renderer->destroy(); }

void rendererPlatformMessage(void* context, const uint8_t* message, size_t size) {
    const RustHostedPlatform& platform = *rendererOf(context).platform;
    if (platform.onMessage) platform.onMessage(textOf(message, size));
}

void rendererFree(void* context) { delete static_cast<RustHostedRenderer*>(context); }

/// The tests' HostedRendererFactory, and what its last throw said.
struct Renderers {
    const HostedRendererFactory* factory;
    std::string error;
};

/// remote_hosted.cpp's wrapper of the factory and WidgetHost's constructor around it: a factory that throws ends the widget with what it threw, "an exception of an unknown type" for anything but a std::exception.
int32_t makeRenderer(void* context, const twidget_hosted_resources* resources, const uint8_t* theme, size_t themeSize, double width, twidget_hosted_platform* handle,
                     twidget_hosted_link* link, twidget_hosted_renderer* out, twidget_engine_bytes* error) {
    auto& r = *static_cast<Renderers*>(context);
    // Owned at once: the handle is released with the platform's last owner, whatever the factory does.
    auto platform = std::make_shared<RustHostedPlatform>(handle);
    if (!*r.factory) return TWIDGET_HOSTED_BUILT_NOTHING;
    auto given = std::make_shared<SandboxResources>();
    given->sandboxBundle = textOf(resources->sandbox_bundle);
    given->validatorBundle = textOf(resources->validator_bundle);
    given->rendererBundle = textOf(resources->renderer_bundle);
    std::unique_ptr<Renderer> built;
    try {
        built = (*r.factory)(std::move(given), textOf(theme, themeSize), width, platform);
    } catch (const std::exception& e) {
        r.error = e.what();
        *error = twidget_engine_bytes{bytesOf(r.error), r.error.size()};
        return TWIDGET_HOSTED_THREW;
    } catch (...) {
        r.error = "an exception of an unknown type";
        *error = twidget_engine_bytes{bytesOf(r.error), r.error.size()};
        return TWIDGET_HOSTED_THREW;
    }
    if (!built) return TWIDGET_HOSTED_BUILT_NOTHING;
    built->onMessage = [link](const std::string& json) { twidget_hosted_renderer_message(link, bytesOf(json), json.size()); };
    built->onDiag = [link](const std::string& kind, const std::string& detail) { twidget_hosted_renderer_diag(link, bytesOf(kind), kind.size(), bytesOf(detail), detail.size()); };
    *out = twidget_hosted_renderer{new RustHostedRenderer{std::move(built), std::move(platform)}, &rendererLoad, &rendererSend, &rendererSetTheme, &rendererSetWidth,
                                   &rendererDestroy, &rendererPlatformMessage, &rendererFree};
    return TWIDGET_HOSTED_RENDERER;
}

} // namespace

std::string serveInRust(SandboxRemoteStream& stream, const std::string& build, const std::function<std::string()>& lockdown) {
    const twidget_engine_stream s{&stream, &readStream, &writeStream, &shutdownStream};
    const twidget_engine_platform p{nullptr, &threadCpuSeconds, &steadySeconds, &timeZone, &randomBytes};
    Lockdown context{&lockdown, {}};
    const twidget_engine_lockdown l{&context, &lockDown};
    size_t size = 0;
    uint8_t* reason = twidget_engine_serve(&s, &p, bytesOf(build), build.size(), lockdown ? &l : nullptr, &size);
    return taken(reason, size);
}

std::string serveHostingInRust(SandboxRemoteStream& stream, const std::string& build, const std::function<std::string()>& lockdown, const HostedRendererFactory& renderers) {
    const twidget_engine_stream s{&stream, &readStream, &writeStream, &shutdownStream};
    const twidget_engine_platform p{nullptr, &threadCpuSeconds, &steadySeconds, &timeZone, &randomBytes};
    Lockdown context{&lockdown, {}};
    const twidget_engine_lockdown l{&context, &lockDown};
    Renderers made{&renderers, {}};
    const twidget_hosted_renderers r{&made, &makeRenderer};
    size_t size = 0;
    uint8_t* reason = twidget_engine_serve_hosting(&s, &p, bytesOf(build), build.size(), lockdown ? &l : nullptr, renderers ? &r : nullptr, &size);
    return taken(reason, size);
}

} // namespace twidget::detail
