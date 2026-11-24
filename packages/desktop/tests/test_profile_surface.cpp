// The bot's profile page's start and its changes through a WidgetHost in every fixture mode, as its renderer and its widget's realm get them, in the suite after test_main.cpp's (whose `sdk_profile_surface_layers_height_and_anchor` is the page's own case: the place, both layers, the height and the anchor); the engine process's twins are packages/rust's case of this name in crates/host/tests/hosted.rs and the realm's readings in crates/engine/tests/engine.rs's `sdk_profile_surface_layers_height_and_anchor`.
#include "test_support.h"

#include "twidget/json.h"
#include "twidget/widget_host.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

/// The shipped bundles, the sandbox's dispatcher wrapped so that the realm keeps `deviceInputs`: the device's edges of
/// the boot record (`boot:`) and each `deviceSafeArea` input (`input:`), as the engine wrote them for the widget. The
/// script is tests/fixtures/record-device-inputs.js, which packages/rust's twins include too: its leading newline
/// parts it from the bundle's last line, and its line ends are LF whatever a checkout made of them (core.autocrlf).
static std::shared_ptr<SandboxResources> recordingDeviceInputs() {
    std::string script = readFile(std::string(TW_FIXTURES) + "/record-device-inputs.js");
    script.erase(std::remove(script.begin(), script.end(), '\r'), script.end());
    auto res = std::make_shared<SandboxResources>(*RES);
    res->sandboxBundle += script;
    return res;
}

/// The bot's profile page (docs/sdk-v1.md §2.19) from its start on, in every mode: a theme, a device layer and a viewport
/// changed before `start()` are what the start carries, once each (the engine, which kept the options' layer at its
/// construction, boots with the new one); the renderer gets the theme, the place, the content's layer, the device's and
/// the policy before any op, an empty batch counted; after the start each device, content and viewport change is
/// resent to it as it comes (two equal device ones twice, the widget hearing one), the viewport checked to two
/// decimals, and a size report gives the height with the viewport of that moment where the glue is this process's (not
/// hosted). A page's realm gets the device's edges as passed; in a bubble the engine gives the widget's realm four
/// zeros for them, at boot and on every change, and its renderer gets them as passed, with no viewport.
TEST(sdk_profile_surface_start_and_changes_reach_the_renderer_and_the_widget) {
    const auto joined = [](const std::vector<std::string>& items) {
        std::string out;
        for (const std::string& item : items) out += (out.empty() ? "" : " | ") + item;
        return out;
    };
    const auto typeOf = [](const std::string& message) {
        const std::optional<Json> m = Json::parse(message);
        return m ? (*m)["type"].asString() : std::string();
    };
    ScriptedRenderer* renderer = nullptr;
    WidgetHostOptions options;
    options.context.surface = WidgetSurface::Profile;
    options.context.chatType = "private"; // a page has no chat: null for the widget
    options.safeArea.top = 47; // replaced before the start
    options.contentSafeArea.top = 90;
    options.contentSafeArea.bottom = 150;
    options.contentSafeArea.left = 40;
    options.contentSafeArea.cornerRight = 64; // no corners on a page: 0 for the widget
    options.viewportHeight = 600; // replaced before the start
    WidgetHost host("<p>x</p><script>var log = [], clog = [], d = TelegramWidget.safeAreaInset, c = TelegramWidget.contentSafeAreaInset;"
                    "var place = [TelegramWidget.surface, d.top, d.bottom, d.left, c.top, c.bottom, c.left, c.cornerRight, String(TelegramWidget.chatType)].join(':');"
                    "TelegramWidget.onEvent('safeAreaChanged', function () { var a = TelegramWidget.safeAreaInset; log.push([a.top, a.left, a.right, a.bottom, Object.isFrozen(a), a !== d].join(':')); });"
                    "TelegramWidget.onEvent('contentSafeAreaChanged', function () { var a = TelegramWidget.contentSafeAreaInset;"
                    " clog.push([a.top, a.right, a.bottom, a.left, a.cornerLeft, a.cornerRight].join(':')); });"
                    "TelegramWidget.setResizeAnchor('bottom');</script>",
                    WidgetTheme{}, 390, options, recordingDeviceInputs(), scripted(renderer));
    WidgetTheme tinted;
    tinted.params = {{"bg_color", "#FFAA22"}};
    host.setTheme(tinted);
    WidgetSafeArea device;
    device.top = 120; // past the old ranges: as passed on a page
    device.bottom = 34;
    device.left = -5; // negative: 0 for the widget
    host.setSafeArea(device);
    host.setViewportHeight(844);
    host.start();
    CHECK(host.waitLoaded(20));
    CHECK(host.waitRendered(5));
    CHECK_MSG(host.probe("place") == "profile:120:34:0:90:150:40:0:null", host.probe("place"));
    const std::string edgesGiven = "{\"top\":120,\"right\":0,\"bottom\":34,\"left\":-5}";
    CHECK_MSG(host.probe("deviceInputs.join(' ')") == "boot:" + edgesGiven, host.probe("deviceInputs.join(' ')"));
    CHECK_MSG(host.probe("TelegramWidget.themeParams.bg_color") == "#ffaa22", host.probe("TelegramWidget.themeParams.bg_color"));
    // The renderer's messages of the place and both layers, in order, read under its lock behind the probe barrier.
    const auto layers = [&] {
        std::lock_guard<std::mutex> l(renderer->mutex);
        std::vector<std::string> out;
        for (const std::string& m : renderer->messages)
            if (const std::string type = typeOf(m); type == "surface" || type == "safeArea" || type == "deviceSafeArea") out.push_back(m);
        return out;
    };
    const std::string placed = "{\"type\":\"surface\",\"surface\":\"profile\",\"viewportHeight\":844}";
    const std::string content = "{\"type\":\"safeArea\",\"inset\":{\"top\":90,\"right\":0,\"bottom\":150,\"left\":40,\"cornerLeft\":0,\"cornerRight\":64}}";
    const std::string edges = "{\"type\":\"deviceSafeArea\",\"inset\":{\"top\":120,\"right\":0,\"bottom\":34,\"left\":-5}}";
    {
        // From the first message on: the theme set before the start once, as its first message, and no batch before the policy.
        std::lock_guard<std::mutex> l(renderer->mutex);
        std::vector<std::string> first;
        for (size_t i = 0; i < std::min<size_t>(renderer->messages.size(), 6); i++) first.push_back(typeOf(renderer->messages[i]));
        CHECK_MSG((first == std::vector<std::string>{"theme", "surface", "safeArea", "deviceSafeArea", "policy", "ops"}), joined(renderer->messages));
        CHECK_MSG(!renderer->messages.empty() && renderer->messages[0] == "{\"type\":\"theme\",\"theme\":{\"colorScheme\":\"light\",\"params\":{\"bg_color\":\"#ffaa22\"},\"button\":{},\"text\":{}}}",
                  joined(renderer->messages));
    }
    CHECK_MSG((layers() == std::vector<std::string>{placed, content, edges}), joined(layers()));
    // The scripted renderer reports no size of its own: a page's content of 120 px is its viewport tall.
    renderer->onMessage("{\"type\":\"size\",\"height\":120}");
    if (!host.isHosted()) CHECK_MSG(host.waitUntil([](const WidgetDiagnostics& d) { return d.height == 844; }, 5), std::to_string(host.diagnostics().height));
    WidgetSafeArea turned;
    turned.left = 59;
    turned.right = 59;
    turned.bottom = 21;
    host.setSafeArea(turned);
    host.setSafeArea(turned);
    WidgetContentSafeArea chrome;
    chrome.top = 80;
    chrome.bottom = 100;
    chrome.cornerRight = 64;
    host.setContentSafeArea(chrome);
    host.setViewportHeight(700.123);
    host.onHostThread([] {});
    CHECK_MSG(host.probe("log.join('|') + '#' + clog.join('|')") == "0:59:59:21:true:true#80:0:100:0:0:0", host.probe("log.join('|') + '#' + clog.join('|')"));
    const std::string turnedGiven = "{\"top\":0,\"right\":59,\"bottom\":21,\"left\":59}";
    CHECK_MSG(host.probe("deviceInputs.join(' ')") == "boot:" + edgesGiven + " input:" + turnedGiven + " input:" + turnedGiven, host.probe("deviceInputs.join(' ')"));
    const std::string turnedEdges = "{\"type\":\"deviceSafeArea\",\"inset\":" + turnedGiven + "}";
    const std::vector<std::string> resent = {placed, content, edges, turnedEdges, turnedEdges,
                                             "{\"type\":\"safeArea\",\"inset\":{\"top\":80,\"right\":0,\"bottom\":100,\"left\":0,\"cornerLeft\":0,\"cornerRight\":64}}",
                                             "{\"type\":\"surface\",\"surface\":\"profile\",\"viewportHeight\":700.12}"};
    CHECK_MSG(layers() == resent, joined(layers()));
    renderer->onMessage("{\"type\":\"size\",\"height\":120}");
    if (!host.isHosted()) CHECK_MSG(host.waitUntil([](const WidgetDiagnostics& d) { return d.height == 700.12; }, 5), std::to_string(host.diagnostics().height));
    host.destroy();
    ScriptedRenderer* bubbleRenderer = nullptr;
    WidgetHostOptions bubbleOptions;
    bubbleOptions.safeArea.top = 47;
    bubbleOptions.viewportHeight = 844; // a bubble has none
    WidgetHost bubble("<script>var s = TelegramWidget.surface + ':' + JSON.stringify(TelegramWidget.safeAreaInset);</script>", WidgetTheme{}, 300, bubbleOptions,
                      recordingDeviceInputs(), scripted(bubbleRenderer));
    bubble.start();
    CHECK(bubble.waitLoaded(20));
    const std::string none = "{\"top\":0,\"right\":0,\"bottom\":0,\"left\":0}";
    CHECK_MSG(bubble.probe("s") == "message:" + none, bubble.probe("s"));
    {
        std::lock_guard<std::mutex> l(bubbleRenderer->mutex);
        const std::vector<std::string>& sent = bubbleRenderer->messages;
        CHECK_MSG(sent.size() > 3 && sent[1] == "{\"type\":\"surface\",\"surface\":\"message\",\"viewportHeight\":0}" &&
                      sent[3] == "{\"type\":\"deviceSafeArea\",\"inset\":{\"top\":47,\"right\":0,\"bottom\":0,\"left\":0}}",
                  joined(sent));
    }
    // The edges never reach a bubble's realm, at boot or later; its renderer gets the change as passed.
    bubble.setSafeArea(turned);
    bubble.onHostThread([] {});
    CHECK_MSG(bubble.probe("deviceInputs.join(' ') + '#' + JSON.stringify(TelegramWidget.safeAreaInset)") == "boot:" + none + " input:" + none + "#" + none,
              bubble.probe("deviceInputs.join(' ')"));
    std::vector<std::string> bubbleEdges;
    for (const Json& m : bubbleRenderer->sent("deviceSafeArea")) bubbleEdges.push_back(m.dump());
    CHECK_MSG((bubbleEdges == std::vector<std::string>{"{\"type\":\"deviceSafeArea\",\"inset\":{\"top\":47,\"right\":0,\"bottom\":0,\"left\":0}}", turnedEdges}), joined(bubbleEdges));
}
