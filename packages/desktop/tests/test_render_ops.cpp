// The validated batches a WidgetHost's renderer gets and `onRenderOps` sees (the harness's leak scan reads that observer), in the suite after test_main.cpp's; the hosted twin is `hosted_widget_host_gets_the_rethemed_batch` in test_hosted.cpp.
#include "test_support.h"

#include "twidget/widget_host.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

/// WidgetHost: the batch a setTheme re-sanitises the stylesheets into reaches `onRenderOps` too, exactly as the renderer gets it, so the harness's leak scan sees every validated batch; an empty one reaches neither observer as a batch.
TEST(sdk_widget_host_rethemed_batch_reaches_on_render_ops) {
    InProcessWidgets inProcess;
    ScriptedRenderer* renderer = nullptr;
    WidgetTheme dark;
    dark.colorScheme = "dark";
    WidgetHost host("<style>@media (prefers-color-scheme: dark){p{color:red}}</style><p>x</p>", dark, 300, WidgetHostOptions{}, RES, scripted(renderer));
    std::mutex m;
    std::vector<std::string> batches;
    host.onRenderOps = [&](const std::string& ops) { std::lock_guard<std::mutex> l(m); batches.push_back(ops); };
    host.start();
    CHECK(host.waitLoaded(20));
    host.onHostThread([] {});
    size_t before = 0;
    {
        std::lock_guard<std::mutex> l(m);
        before = batches.size();
    }
    CHECK(before > 0);
    WidgetTheme light;
    host.setTheme(light);
    host.onHostThread([] {});
    std::string last;
    {
        std::lock_guard<std::mutex> l(renderer->mutex);
        for (auto& message : renderer->messages) if (message.rfind("{\"type\":\"ops\"", 0) == 0) last = message;
    }
    std::lock_guard<std::mutex> l(m);
    CHECK_MSG(batches.size() == before + 1, std::to_string(batches.size()) + " batches, " + std::to_string(before) + " before");
    CHECK_MSG(!batches.empty() && last == "{\"type\":\"ops\",\"ops\":" + batches.back() + "}", last);
    CHECK_MSG(!batches.empty() && batches.back().find("min-width: 99999px") != std::string::npos, batches.empty() ? "none" : batches.back());
    host.destroy();
}
