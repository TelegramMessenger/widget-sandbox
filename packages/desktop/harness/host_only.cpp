// twidget-host-only, built from the host's side alone (CMakeLists.txt's twidget-sandbox-host objects: no engine, TrustedValidator, in-process engine, local glue or engine end), so its link proves that no host-side source needs the rest; it runs a hosted WidgetHost that keeps its renderer over a hosting stream whose engine's end is gone (none can link here) and passes when that widget ran hosted, died "the widget engine process ended" and kept nothing, while the hosted suites prove the same objects' behaviour.
#include "stream_peers.h"
#include "twidget/remote_stream.h"
#include "twidget/resources.h"
#include "twidget/widget_host.h"

#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>

using namespace twidget;

int main() {
    std::shared_ptr<SandboxResources> resources;
    try {
        resources = SandboxResources::fromDirectory(TW_WIDGET_JS);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "host-only: %s\n", e.what());
        return 1;
    }
    // The engine's end closes as an engine process that exited closes its own.
    MemoryStreamPair pair = memoryStreamPair(size_t(1) << 20);
    pair.engine->close();
    std::shared_ptr<SandboxRemoteHostEnd> end = SandboxRemoteHostEnd::startHosting(pair.host, *resources, "twidget-host-only", {}, {});
    bool hosted = false;
    std::string dead = "none";
    bool kept = false;
    {
        WidgetHost host("<p>host-only</p>", WidgetTheme{"light", {}, {}, {}}, 300, WidgetHostOptions{}, end->hostedChannel());
        host.keepsRenderer = true;
        host.onPlatformMessage = [](const std::string&) {};
        host.onPlatformRequest = [](const std::string&, std::function<void(std::string answer)> respond) { respond(""); };
        hosted = host.isHosted();
        host.start();
        host.waitUntil([](const WidgetDiagnostics& d) { return d.dead.has_value(); }, 10);
        if (std::optional<std::string> reason = host.diagnostics().dead) dead = *reason;
        host.sendPlatform("host-only");
        // Null: the route ended with the stream, so there is no renderer to keep.
        kept = host.keepRenderer({}) != nullptr;
    }
    end->stop();
    std::printf("host-only: hosted=%d dead=%s kept=%d\n", int(hosted), dead.c_str(), int(kept));
    return hosted && dead == "the widget engine process ended" && !kept ? 0 : 1;
}
