// The hosted mode's tests (remote_hosted.h: its coding, a hosted client over the fake channel, a hosted WidgetHost over the loopback, and a probe's order over the loopback and a hosting stream), in the suite after the byte stream's: `twidget-tests hosted_`.
#include "test_support.h"
#include "test_remote_support.h"
#include "test_remote_stream_support.h"

#include "twidget/bundle_stamp.h"
#include "twidget/intl.h"
#include "twidget/json.h"
#include "twidget/remote_engine.h"
#include "twidget/remote_hosted.h"
#include "twidget/sandbox_engine.h"
#include "twidget/sdk_policy.h"
#include "twidget/widget_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

// ───────────── the coding of the hosted forms ─────────────

static bool sameTheme(const WidgetTheme& a, const WidgetTheme& b) {
    const WidgetButtonTokens &x = a.button, &y = b.button;
    return a.colorScheme == b.colorScheme && a.params == b.params && x.bg == y.bg && x.text == y.text && x.radius == y.radius && x.font == y.font &&
           x.padding == y.padding && x.height == y.height && x.pressedOpacity == y.pressedOpacity && x.disabledOpacity == y.disabledOpacity &&
           a.text.fontSize == b.text.fontSize && a.text.lineHeight == b.text.lineHeight && a.text.padding == b.text.padding;
}

static std::string nested(int depth) { return std::string(size_t(depth), '[') + "0" + std::string(size_t(depth), ']'); }

/// A theme with every token set.
static WidgetTheme everyToken() {
    WidgetTheme t;
    t.colorScheme = "dark";
    t.params = {{"bg_color", "#112233"}, {"text_color", "#ffffff"}, {"not a key", "kept as given"}};
    t.button.bg = "#0088cc";
    t.button.text = "#ffffff";
    t.button.radius = "8px";
    t.button.font = "600 15px system-ui";
    t.button.padding = "6px 12px";
    t.button.height = "36px";
    t.button.pressedOpacity = 0.7;
    t.button.disabledOpacity = 0.4;
    t.text.fontSize = 16.5;
    t.text.lineHeight = 1.4;
    t.text.padding = "10px 14px";
    return t;
}

/// Every token and the start's members cross whole (a non-finite token as unset, a long label cut to the text bound); a form that is no object, has no string `colorScheme` (no decodable theme), passes 1 MiB or `kMaxDepth`, or is cut short is refused; members of another type are unset or keep their defaults.
TEST(hosted_coding_round_trips_and_refuses_damage) {
    using Coding = SandboxRemoteCoding;
    const WidgetTheme full = everyToken();
    const std::string wire = Coding::encodeTheme(full);
    std::optional<WidgetTheme> back = Coding::decodeTheme(wire);
    CHECK_MSG(back && sameTheme(*back, full), wire);
    const std::string plain = Coding::encodeTheme(WidgetTheme{});
    CHECK_MSG(plain == "{\"colorScheme\":\"light\",\"params\":{},\"button\":{},\"text\":{}}", plain);
    back = Coding::decodeTheme(plain);
    CHECK(back && sameTheme(*back, WidgetTheme{}));

    WidgetTheme odd = full;
    odd.button.pressedOpacity = std::nan("");
    odd.button.disabledOpacity = std::numeric_limits<double>::infinity();
    odd.text.fontSize = -std::numeric_limits<double>::infinity();
    back = Coding::decodeTheme(Coding::encodeTheme(odd));
    WidgetTheme dropped = full;
    dropped.button.pressedOpacity = dropped.button.disabledOpacity = dropped.text.fontSize = std::nullopt;
    CHECK_MSG(back && sameTheme(*back, dropped), Coding::encodeTheme(odd));

    // Members of another type are unset, a palette that is not all strings is none, a repeated palette key keeps its last value.
    back = Coding::decodeTheme("{\"colorScheme\":\"dark\",\"params\":{\"bg_color\":1},\"button\":{\"bg\":1,\"pressedOpacity\":\"0.5\",\"height\":null,\"radius\":\"4px\","
                               "\"disabledOpacity\":1e999},\"text\":[16]}");
    WidgetTheme expected;
    expected.colorScheme = "dark";
    expected.button.radius = "4px";
    CHECK(back && sameTheme(*back, expected));
    back = Coding::decodeTheme("{\"colorScheme\":\"custom\",\"params\":{\"bg_color\":\"#111111\",\"bg_color\":\"#222222\"},\"button\":\"x\",\"text\":{\"lineHeight\":1.2,\"padding\":3}}");
    expected = WidgetTheme{};
    expected.colorScheme = "custom";
    expected.params = {{"bg_color", "#222222"}};
    expected.text.lineHeight = 1.2;
    CHECK(back && sameTheme(*back, expected));

    for (const char* bad : {"{}", "{\"colorScheme\":1}", "{\"colorScheme\":null,\"params\":{}}", "[]", "\"light\"", "null", "", "{\"colorScheme\":\"light\""})
        CHECK_MSG(!Coding::decodeTheme(bad), bad);
    size_t prefixes = 0;
    for (size_t i = 0; i < wire.size(); i++) prefixes += Coding::decodeTheme(wire.substr(0, i)) ? 1 : 0;
    CHECK_MSG(prefixes == 0, std::to_string(prefixes) + " cut forms decoded");
    CHECK(Coding::decodeTheme(paddedTo(wire, SandboxEngine::kRemoteMessageLimit)) && !Coding::decodeTheme(paddedTo(wire, SandboxEngine::kRemoteMessageLimit + 1)));
    CHECK(Coding::decodeTheme("{\"colorScheme\":\"light\",\"x\":" + nested(Coding::kMaxDepth - 1) + "}"));
    CHECK(!Coding::decodeTheme("{\"colorScheme\":\"light\",\"x\":" + nested(Coding::kMaxDepth) + "}"));

    SandboxHostedStart start;
    start.theme = full;
    start.stoppedLabel = "Stopped by the host";
    start.validatedOps = true;
    const std::string startWire = Coding::encodeHostedStart(start);
    std::optional<SandboxHostedStart> started = Coding::decodeHostedStart(startWire);
    CHECK_MSG(started && sameTheme(started->theme, full) && started->stoppedLabel == start.stoppedLabel && started->validatedOps, startWire);
    const std::string defaults = Coding::encodeHostedStart(SandboxHostedStart{});
    CHECK_MSG(defaults == "{\"theme\":" + plain + ",\"stoppedLabel\":\"Widget stopped\",\"validatedOps\":false}", defaults);
    started = Coding::decodeHostedStart(defaults);
    CHECK(started && sameTheme(started->theme, WidgetTheme{}) && started->stoppedLabel == "Widget stopped" && !started->validatedOps);
    for (size_t extra : {size_t(0), size_t(1), size_t(100)}) {
        SandboxHostedStart labelled;
        labelled.stoppedLabel = std::string(SandboxEngine::kRemoteTextLimit + extra, 'l');
        started = Coding::decodeHostedStart(Coding::encodeHostedStart(labelled));
        CHECK_MSG(started && started->stoppedLabel == std::string(SandboxEngine::kRemoteTextLimit, 'l'), std::to_string(extra));
    }
    // A label or flag of another type keeps the default, a flag is true only when it is true.
    for (const char* odder : {"{\"theme\":{\"colorScheme\":\"light\"},\"stoppedLabel\":5,\"validatedOps\":\"true\"}", "{\"theme\":{\"colorScheme\":\"light\"},\"validatedOps\":1}",
                              "{\"theme\":{\"colorScheme\":\"light\"},\"stoppedLabel\":null,\"validatedOps\":null}"}) {
        started = Coding::decodeHostedStart(odder);
        CHECK_MSG(started && started->stoppedLabel == "Widget stopped" && !started->validatedOps, odder);
    }
    for (const char* bad : {"{}", "{\"theme\":{}}", "{\"theme\":\"light\",\"validatedOps\":true}", "{\"theme\":{\"colorScheme\":2}}", "[]", "null", "",
                            "{\"theme\":{\"colorScheme\":\"light\"}"})
        CHECK_MSG(!Coding::decodeHostedStart(bad), bad);
    prefixes = 0;
    for (size_t i = 0; i < startWire.size(); i++) prefixes += Coding::decodeHostedStart(startWire.substr(0, i)) ? 1 : 0;
    CHECK_MSG(prefixes == 0, std::to_string(prefixes) + " cut starts decoded");
    CHECK(Coding::decodeHostedStart(paddedTo(startWire, SandboxEngine::kRemoteMessageLimit)) && !Coding::decodeHostedStart(paddedTo(startWire, SandboxEngine::kRemoteMessageLimit + 1)));
    CHECK(Coding::decodeHostedStart("{\"theme\":{\"colorScheme\":\"light\",\"x\":" + nested(Coding::kMaxDepth - 2) + "}}"));
    CHECK(!Coding::decodeHostedStart("{\"theme\":{\"colorScheme\":\"light\",\"x\":" + nested(Coding::kMaxDepth - 1) + "}}"));
}

// ───────────── a hosted client (SandboxEngine::hostedClient over a SandboxHostedChannel) ─────────────

static SandboxConfig hostedConfig() {
    SandboxConfig c = testConfig();
    c.context.canQuery = true;
    return c;
}

/// A hosted client over a fake channel, its recorder set first (the recorder outlives the engine), hosted-started with `start` unless `begin` is false.
struct HostedRun {
    std::shared_ptr<FakeRemoteChannel> channel = std::make_shared<FakeRemoteChannel>();
    RemoteRecorder recorder;
    std::unique_ptr<SandboxEngine> engine;
    explicit HostedRun(SandboxConfig config = hostedConfig(), SandboxHostedStart start = SandboxHostedStart(), bool begin = true)
        : engine(SandboxEngine::hostedClient("<p>x</p>", std::move(config), channel)) {
        engine->setDelegate(&recorder);
        if (begin) engine->startHosted(std::move(start));
        settle();
    }
    /// Everything queued on the dispatch thread so far has run.
    void settle() { engine->thread().sync([] {}); }
    SandboxRemoteSink& sink() { return *channel->sink(); }
    /// A primary press the embedder delivered, applied once the dispatch thread had it.
    void press() {
        engine->pressDelivered();
        settle();
    }
};

/// The calls of `channel` that only an engine-mode client makes.
static std::vector<std::string> engineOnlyCalls(FakeRemoteChannel& channel) {
    std::vector<std::string> out;
    for (const std::string& call : channel.calls()) {
        for (const char* prefix : {"start:", "event:", "activation:", "frame:", "viewport:", "resize:", "theme:", "charge:"})
            if (call.rfind(prefix, 0) == 0) out.push_back(call);
        if (call == "press" || call == "release") out.push_back(call);
    }
    return out;
}

/// The engine-only inputs on a hosted client: none reaches its channel, grants anything or touches its haptic gate.
static void engineOnlyInputs(SandboxEngine& engine) {
    engine.start();
    engine.event("{\"type\":\"click\"}");
    engine.activationEvent("{\"type\":\"pointerdown\"}", 7);
    engine.frame(16);
    engine.viewport(320, 200, 2);
    engine.resize("[]");
    engine.theme("dark", {{"bg_color", "#000000"}});
    engine.pressBegan();
    engine.pressEnded();
    engine.chargeValidator(5);
}

/// A hosted client's grants come only from `pressDelivered`: its `start()` and the other engine-only inputs reach the channel never (only `startHosted`, once), and neither they nor the engine's side's own word (a platform "press") allow a privileged call or open the haptic window; each delivered press allows one link, copy or tapped request within `kRemoteGrantWindowMs`, none while hidden or in a preview, and the newest `kRemoteMaxGrants` are kept.
TEST(hosted_client_grants_come_from_delivered_presses) {
    {
        HostedRun r(hostedConfig(), SandboxHostedStart(), false);
        CHECK(r.engine->isHosted() && r.engine->isRemote());
        r.engine->start();
        r.settle();
        CHECK_MSG(r.channel->calls().empty(), joined(r.channel->calls()));
        CHECK(r.engine->probe("1") == "ERROR no runtime");
        SandboxHostedStart start;
        start.theme.colorScheme = "dark";
        start.stoppedLabel = "Stopped";
        start.validatedOps = true;
        r.engine->startHosted(start);
        r.engine->startHosted(SandboxHostedStart());
        r.engine->start();
        r.settle();
        const std::string expected = "hostedStart:<p>x</p>:" + SandboxRemoteCoding::encodeConfig(hostedConfig()) + ":" + SandboxRemoteCoding::encodeHostedStart(start);
        CHECK_MSG((r.channel->calls() == std::vector<std::string>{expected}), joined(r.channel->calls()));
    }
    {
        HostedRun r;
        r.sink().remoteBridge("openLink", linkArgs("https://example.com/none"));
        r.sink().remotePlatformMessage("press");
        engineOnlyInputs(*r.engine);
        r.settle();
        r.sink().remoteBridge("openLink", linkArgs("https://example.com/after"));
        r.sink().remoteCopyText("after");
        r.settle();
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"diag:activation-blocked", "hosted-message:press", "diag:activation-blocked", "diag:activation-blocked"}),
                  joined(r.recorder.events()));
        const std::vector<std::pair<std::string, std::string>> diags = r.recorder.diags();
        CHECK(diags.size() == 3 && diags[0].second == "openLink ignored by the host: no tap on the visible widget allows it" &&
              diags[2].second == "copyText ignored by the host: no tap on the visible widget allows it");
        r.press();
        r.sink().remoteBridge("openLink", linkArgs("https://example.com/1"));
        r.sink().remoteBridge("openLink", linkArgs("https://example.com/2"));
        r.settle();
        CHECK_MSG(r.recorder.eventsWith("action:") == std::vector<std::string>{linkAction("openLink", "https://example.com/1")}, joined(r.recorder.events()));
        CHECK(r.recorder.eventsWith("bridge:") == std::vector<std::string>{"bridge:openLink:[\"https://example.com/1\"]"});
        // A copy and a tapped request spend a press too.
        r.press();
        r.sink().remoteCopyText("copied");
        r.sink().remoteCopyText("again");
        r.press();
        r.sink().remoteRequest(1, "tapped", false);
        r.sink().remoteCopyText("after the request");
        r.settle();
        CHECK_MSG(r.recorder.eventsWith("copy:") == std::vector<std::string>{"copy:copied"}, joined(r.recorder.events()));
        CHECK(r.recorder.eventsWith("request:") == std::vector<std::string>{"request:tapped"});
        CHECK(r.recorder.respond(0, answered("done")));
        r.settle();
        CHECK(r.channel->answers() == (std::vector<std::pair<int64_t, SandboxRemoteOutcome>>{{1, SandboxRemoteOutcome::answer("done")}}));
        // A press delivered while hidden allows nothing, also once the widget is back; the next visible one does.
        r.engine->setVisible(false);
        r.press();
        r.engine->setVisible(true);
        r.sink().remoteCopyText("hidden press");
        r.settle();
        CHECK(r.recorder.eventsWith("copy:").size() == 1);
        r.press();
        r.sink().remoteCopyText("visible press");
        r.settle();
        CHECK_MSG(r.recorder.eventsWith("copy:") == (std::vector<std::string>{"copy:copied", "copy:visible press"}), joined(r.recorder.events()));
        engineOnlyInputs(*r.engine);
        r.settle();
        CHECK_MSG(engineOnlyCalls(*r.channel).empty(), joined(engineOnlyCalls(*r.channel)));
        CHECK(r.recorder.deaths().empty());
    }
    {
        // A press older than the window allows nothing (`kept`: its oldest of 32 presses outlived it), and of 33 presses the oldest is dropped (`trimmed`: had a newer one gone, the expired oldest would leave 31).
        HostedRun kept, trimmed;
        const int max = int(SandboxEngine::kRemoteMaxGrants);
        kept.engine->pressDelivered();
        trimmed.engine->pressDelivered();
        sleepMs(int(SandboxEngine::kRemoteGrantWindowMs / 2) + 100);
        for (int i = 1; i < max; i++) kept.engine->pressDelivered();
        for (int i = 0; i < max; i++) trimmed.engine->pressDelivered();
        sleepMs(int(SandboxEngine::kRemoteGrantWindowMs / 2));
        for (int i = 0; i <= max; i++) {
            kept.sink().remoteCopyText("copy " + std::to_string(i));
            trimmed.sink().remoteCopyText("copy " + std::to_string(i));
        }
        kept.settle();
        trimmed.settle();
        CHECK_MSG(kept.recorder.eventsWith("copy:").size() == size_t(max - 1), std::to_string(kept.recorder.eventsWith("copy:").size()));
        CHECK_MSG(trimmed.recorder.eventsWith("copy:").size() == size_t(max), std::to_string(trimmed.recorder.eventsWith("copy:").size()));
    }
    {
        // A preview's press: a preview allows no privileged call at all, and its press is not even a grant.
        SandboxConfig preview = hostedConfig();
        preview.context.preview = true;
        HostedRun r(preview);
        r.press();
        r.sink().remoteBridge("openLink", linkArgs("https://example.com/"));
        r.sink().remoteCopyText("preview");
        r.settle();
        CHECK_MSG(r.recorder.eventsWith("action:").empty() && r.recorder.eventsWith("copy:").empty(), joined(r.recorder.events()));
    }
    {
        // Haptics: the boot's one series outside a touch, then nothing on the engine's side's word or the engine-only presses; a delivered press plays, and so does its release's window.
        HostedRun r;
        r.sink().remoteBridge("haptic", "[\"selection\"]");
        r.settle();
        CHECK(r.recorder.eventsWith("action:").size() == 1);
        sleepMs(int(sdk::HAPTIC_SERIES_MS) + 100);
        r.sink().remotePlatformMessage("press");
        r.engine->pressBegan();
        r.engine->activationEvent("{\"type\":\"pointerdown\"}", 3);
        r.sink().remoteBridge("haptic", "[\"impact\",\"soft\"]");
        r.settle();
        CHECK_MSG(r.recorder.eventsWith("action:").size() == 1, joined(r.recorder.events()));
        r.press();
        r.sink().remoteBridge("haptic", "[\"impact\",\"soft\"]");
        r.settle();
        r.engine->releaseDelivered();
        r.sink().remoteBridge("haptic", "[\"notification\",\"success\"]");
        r.settle();
        const std::vector<std::string> actions = r.recorder.eventsWith("action:");
        CHECK_MSG(actions.size() == 3 && actions[1] == "action:{\"kind\":\"haptic\",\"haptic\":\"impact\",\"value\":\"soft\"}" &&
                      actions[2] == "action:{\"kind\":\"haptic\",\"haptic\":\"notification\",\"value\":\"success\"}",
                  joined(actions));
        CHECK(r.recorder.diags().empty() && engineOnlyCalls(*r.channel).empty());
    }
}

/// `fn`'s value from a thread of the test's, waited for up to 5 s (a hang ends the run instead of stalling it).
static std::optional<std::string> requestedFrom(const std::function<std::optional<std::string>()>& fn) {
    Background<std::optional<std::string>> worker(fn);
    return worker.finish(5);
}

static const std::string kUnanswered = "the host answered no request of the widget's renderer";

/// What a hosted runtime's engine's side sends is bounded by the client: the render calls a hosted widget keeps to itself are protocol violations before they are admitted, an engine-only client refuses the hosted calls, validated ops come only when asked for, node counts are clamped, a platform message or request past its bound is refused, a request gets the delegate's answer on the dispatch thread and no answer kills the widget once.
TEST(hosted_client_bounds_what_the_engine_end_sends) {
    {
        HostedRun r;
        r.sink().remoteOps("[]");
        r.sink().remoteListen("{}");
        r.sink().remotePreventDefault("{}");
        r.sink().remoteAnim("{}");
        r.sink().remoteRequestsFrame();
        const QueryAnswer answer = r.sink().remoteQuery("{\"q\":1}");
        r.settle();
        CHECK(answer.json == "{\"error\":\"invalid query\"}" && !answer.binary);
        const std::vector<std::pair<std::string, std::string>> expected = {{"protocol-violation", "an ops batch from a hosted engine process"},
                                                                           {"protocol-violation", "listen from a hosted engine process"},
                                                                           {"protocol-violation", "preventDefault from a hosted engine process"},
                                                                           {"protocol-violation", "anim from a hosted engine process"},
                                                                           {"protocol-violation", "a frame request from a hosted engine process"},
                                                                           {"protocol-violation", "a layout query from a hosted engine process"}};
        CHECK(r.recorder.diags() == expected);
        CHECK_MSG(r.recorder.events().size() == expected.size(), joined(r.recorder.events()));
        CHECK_MSG(backlogIs(*r.engine, 0, 0), backlogText(*r.engine));
    }
    {
        // Refused before it is admitted: while the dispatch thread is held only the report is counted, never the batch.
        Gate gate;
        HostedRun r;
        r.engine->thread().async([&gate] { gate.wait(); });
        CHECK(gate.waitedOn());
        r.sink().remoteOps(paddedTo("[]", SandboxEngine::kRemoteOpsLimit));
        const size_t report = std::string("an ops batch from a hosted engine process").size();
        CHECK_MSG(backlogIs(*r.engine, report, 1), backlogText(*r.engine));
        gate.open();
        r.settle();
        CHECK_MSG(backlogIs(*r.engine, 0, 0), backlogText(*r.engine));
    }
    {
        auto channel = std::make_shared<FakeRemoteChannel>();
        RemoteRecorder recorder;
        SandboxEngine engine("<p>x</p>", hostedConfig(), channel);
        engine.setDelegate(&recorder);
        engine.start();
        CHECK(engine.isRemote() && !engine.isHosted());
        SandboxRemoteSink& sink = *channel->sink();
        sink.remotePlatformMessage("m");
        CHECK(!requestedFrom([&] { return sink.remotePlatformRequest("r"); }));
        sink.remoteValidatedOps("[]");
        sink.remoteRendererReady();
        sink.remoteRendered();
        sink.remoteNodes(3);
        engine.startHosted(SandboxHostedStart());
        engine.hostedTheme(WidgetTheme{});
        engine.platform("p");
        engine.pressDelivered();
        engine.releaseDelivered();
        engine.thread().sync([] {});
        const std::vector<std::pair<std::string, std::string>> expected = {{"protocol-violation", "a platform message from an engine-only runtime"},
                                                                           {"protocol-violation", "a platform request from an engine-only runtime"},
                                                                           {"protocol-violation", "validated ops from an engine-only runtime"},
                                                                           {"protocol-violation", "a renderer's ready from an engine-only runtime"},
                                                                           {"protocol-violation", "a render report from an engine-only runtime"},
                                                                           {"protocol-violation", "a node count from an engine-only runtime"}};
        CHECK(recorder.diags() == expected);
        CHECK_MSG(recorder.events().size() == expected.size(), joined(recorder.events()));
        CHECK_MSG(channel->calls().size() == 1 && channel->calls()[0].rfind("start:", 0) == 0, joined(channel->calls()));
        CHECK(backlogIs(engine, 0, 0));
        // An engine with its own runtime takes none of a hosted client's inputs either.
        SandboxEngine local("<p>x</p>", testConfig(), RES);
        CHECK(!local.isRemote() && !local.isHosted());
        local.startHosted(SandboxHostedStart());
        local.hostedTheme(WidgetTheme{});
        local.platform("p");
        local.pressDelivered();
        local.releaseDelivered();
        CHECK(local.probe("1") == "ERROR no runtime");
    }
    {
        // Validated ops only after a start that asked for them; one past the ops bound is refused.
        HostedRun unasked(hostedConfig(), SandboxHostedStart(), false);
        unasked.sink().remoteValidatedOps("[]");
        unasked.engine->startHosted(SandboxHostedStart());
        unasked.sink().remoteValidatedOps("[]");
        unasked.settle();
        const std::pair<std::string, std::string> notAsked = {"protocol-violation", "validated ops the host did not ask for"};
        CHECK_MSG(unasked.recorder.diags() == (std::vector<std::pair<std::string, std::string>>{notAsked, notAsked}), joined(unasked.recorder.events()));
        CHECK(unasked.recorder.eventsWith("hosted-ops").empty());
        SandboxHostedStart ask;
        ask.validatedOps = true;
        HostedRun asked(hostedConfig(), ask);
        asked.sink().remoteValidatedOps("[{\"op\":\"el\",\"id\":2,\"tag\":\"p\"}]");
        asked.sink().remoteValidatedOps(paddedTo("[]", SandboxEngine::kRemoteOpsLimit + 1));
        asked.sink().remoteValidatedOps(paddedTo("[]", SandboxEngine::kRemoteOpsLimit));
        asked.settle();
        CHECK_MSG((asked.recorder.events() == std::vector<std::string>{"hosted-ops", "diag:protocol-violation", "hosted-ops"}), joined(asked.recorder.events()));
        CHECK(asked.recorder.diags() == (std::vector<std::pair<std::string, std::string>>{{"protocol-violation", "validated ops over 32 MB"}}));
        CHECK_MSG(backlogIs(*asked.engine, 0, 0), backlogText(*asked.engine));
    }
    {
        HostedRun r;
        r.sink().remoteRendererReady();
        r.sink().remoteNodes(-5);
        r.sink().remoteNodes(INT64_MAX);
        r.sink().remoteNodes(INT64_MIN);
        r.sink().remoteNodes(int64_t(INT_MAX) + 1);
        r.sink().remoteNodes(42);
        r.sink().remoteRendered();
        r.sink().remotePlatformMessage("hello");
        r.sink().remotePlatformMessage(std::string(SandboxEngine::kRemotePlatformLimit + 1, 'm'));
        r.sink().remotePlatformMessage(std::string(SandboxEngine::kRemotePlatformLimit, 'm'));
        r.settle();
        const std::vector<std::string> events = r.recorder.events();
        const std::string max = "nodes:" + std::to_string(INT_MAX);
        CHECK_MSG(events.size() == 10 && std::vector<std::string>(events.begin(), events.begin() + 9) ==
                                             (std::vector<std::string>{"renderer-ready", "nodes:0", max, "nodes:0", max, "nodes:42", "rendered", "hosted-message:hello", "diag:protocol-violation"}),
                  joined(events));
        CHECK(events.size() == 10 && events[9] == "hosted-message:" + std::string(SandboxEngine::kRemotePlatformLimit, 'm'));
        CHECK(r.recorder.diags() == (std::vector<std::pair<std::string, std::string>>{{"protocol-violation", "a platform message over 32 MB"}}));
        CHECK_MSG(backlogIs(*r.engine, 0, 0), backlogText(*r.engine));
    }
    {
        HostedRun r;
        const std::thread::id dispatch = r.engine->thread().sync([] { return std::this_thread::get_id(); });
        CHECK(requestedFrom([&] { return r.sink().remotePlatformRequest("ask"); }) == std::optional<std::string>("platform-answer"));
        CHECK(requestedFrom([&] { return r.sink().remotePlatformRequest(std::string(SandboxEngine::kRemotePlatformLimit + 1, 'r')); }) == std::optional<std::string>(""));
        r.settle();
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"hosted-request:ask", "diag:protocol-violation"}), joined(r.recorder.events()));
        CHECK(r.recorder.diags() == (std::vector<std::pair<std::string, std::string>>{{"protocol-violation", "a platform request over 32 MB"}}));
        CHECK(!r.recorder.threads().empty() && r.recorder.threads()[0] == dispatch);
        // No answer: the widget dies once with that reason, the channel hears the stop and closes; later requests get none and reach no delegate.
        r.recorder.platformAnswer = std::nullopt;
        CHECK(!requestedFrom([&] { return r.sink().remotePlatformRequest("unanswered"); }));
        CHECK(!requestedFrom([&] { return r.sink().remotePlatformRequest("late"); }));
        r.settle();
        CHECK_MSG(r.recorder.deaths() == std::vector<std::string>{kUnanswered}, joined(r.recorder.events()));
        CHECK(r.recorder.eventsWith("hosted-request:") == (std::vector<std::string>{"hosted-request:ask", "hosted-request:unanswered"}));
        CHECK_MSG(callsWith(*r.channel, "stop:") == std::vector<std::string>{"stop:" + kUnanswered} && r.channel->closes() == 1, joined(r.channel->calls()));
        CHECK_MSG(backlogIs(*r.engine, 0, 0), backlogText(*r.engine));
    }
    {
        // A delegate that stops the widget itself before it answers nothing (the host's own timeout): that death, once.
        HostedRun r;
        r.recorder.platformAnswer = std::nullopt;
        r.recorder.duringPlatformRequest = [](SandboxEngine& engine) { engine.stop("the host's own reason"); };
        CHECK(!requestedFrom([&] { return r.sink().remotePlatformRequest("ask"); }));
        r.settle();
        CHECK_MSG(r.recorder.deaths() == std::vector<std::string>{"the host's own reason"}, joined(r.recorder.events()));
        CHECK(callsWith(*r.channel, "stop:") == std::vector<std::string>{"stop:the host's own reason"} && r.channel->closes() == 1);
    }
    {
        // A hosted client built without a channel is dead from its first task, as a remote one is.
        RemoteRecorder recorder;
        std::unique_ptr<SandboxEngine> engine = SandboxEngine::hostedClient("<p>x</p>", hostedConfig(), nullptr);
        engine->setDelegate(&recorder);
        engine->startHosted(SandboxHostedStart());
        engine->pressDelivered();
        engine->thread().sync([] {});
        CHECK(engine->isHosted() && recorder.deaths() == std::vector<std::string>{kEngineGone});
        engine.reset();
    }
}

// ───────────── a hosted WidgetHost over the loopback (SandboxHostedLoopbackChannel) ─────────────

/// The calling thread's name, "" where the tests read no thread names.
static std::string threadNameHere() {
#if defined(__APPLE__) || defined(__linux__)
    char name[64] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    return name;
#else
    return {};
#endif
}

/// A host's Intl provider that answers "INTL" and keeps the name of each thread it was asked on.
class NamedIntl final : public IntlProvider {
public:
    std::string format(const Json&) override {
        std::lock_guard<std::mutex> l(mutex_);
        threads_.push_back(threadNameHere());
        return "INTL";
    }
    std::vector<std::string> threads() {
        std::lock_guard<std::mutex> l(mutex_);
        return threads_;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> threads_;
};

/// A hosted WidgetHost over a loopback whose engine's side builds a HostedTestRenderer (`configure` runs on it first); `renderer` is that renderer once it announced itself.
struct HostedWidget {
    HostedTestRenderer* renderer = nullptr;
    std::unique_ptr<WidgetHost> host;
    explicit HostedWidget(const std::string& html, WidgetTheme theme = WidgetTheme{}, WidgetHostOptions options = WidgetHostOptions(), std::shared_ptr<IntlProvider> intl = nullptr,
                          std::function<void(HostedTestRenderer&)> configure = nullptr, std::shared_ptr<SandboxResources> resources = RES)
        : host(std::make_unique<WidgetHost>(html, std::move(theme), 300, std::move(options),
                                            std::make_shared<SandboxHostedLoopbackChannel>(std::move(resources), hostedTest(renderer, std::move(configure))), std::move(intl))) {}
    HostedWidget(const HostedWidget&) = delete;
    HostedWidget& operator=(const HostedWidget&) = delete;
    /// Started, loaded and rendered on the engine's side.
    bool begin() {
        host->start();
        return host->waitLoaded(20) && host->waitRendered(5) && !host->diagnostics().dead;
    }
    /// What the client queued has reached the host, and the host thread ran what that sent it.
    void settle() { settleHost(*host); }
};

static bool isDead(const WidgetDiagnostics& d) { return d.dead.has_value(); }

static double secondsSince(std::chrono::steady_clock::time_point at) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - at).count(); }

/// Its layout query, listener, frame request and Intl call at boot, and a text to find in its batches.
static const char* kWholeWidget = "<p id='p'>hello</p><button id='b'>go</button><script>"
                                  "var w = document.getElementById('p').getBoundingClientRect().width, n = (1234.5).toLocaleString();"
                                  "document.getElementById('b').addEventListener('click', function () {});"
                                  "requestAnimationFrame(function () {});</script>";

/// The whole widget runs on the engine's side of a hosted loopback: its ops, layout query, listener and frame request reach that side's renderer (the theme with every token first, then the safe area and the policy), the host hears it loaded and rendered, its node count and its renderer's ready, gets the validated batches it asked for before start, formats its Intl requests on the client's dispatch thread and sees no protocol violation; its height stays 0. The hook builds such a host from a renderer factory while no engine channel factory is set, else a local one, and a hook without a channel gives a widget that dies.
TEST(hosted_loopback_runs_the_whole_widget_in_the_engine_end) {
    WidgetHostOptions options;
    options.contentSafeArea.bottom = 24;
    options.contentSafeArea.cornerRight = 72;
    auto intl = std::make_shared<NamedIntl>();
    HostedWidget w(kWholeWidget, everyToken(), options, intl);
    std::mutex m;
    std::vector<std::string> batches;
    w.host->onRenderOps = [&](const std::string& ops) {
        std::lock_guard<std::mutex> l(m);
        batches.push_back(ops);
    };
    CHECK(w.host->isHosted());
    CHECK(w.begin());
    w.settle();
    WidgetDiagnostics d = w.host->diagnostics();
    CHECK_MSG(d.loaded && d.rendered && d.loadedMs && d.renderedMs && d.rendererReadyMs && !d.dead, d.dead.value_or("alive"));
    CHECK_MSG(d.nodes > 0, std::to_string(d.nodes));
    CHECK(d.height == 0 && !d.firstContentMs);
    CHECK_MSG(d.countDiag("protocol-violation") == 0, joined(d.diags["protocol-violation"].second));
    {
        std::lock_guard<std::mutex> l(m);
        std::string all;
        for (const std::string& b : batches) all += b;
        CHECK_MSG(!batches.empty() && all.find("hello") != std::string::npos, all.substr(0, 200));
    }
    HostedTestRenderer& r = *w.renderer;
    const std::string theme = "{\"type\":\"theme\",\"theme\":{\"colorScheme\":\"dark\",\"params\":{\"bg_color\":\"#112233\",\"text_color\":\"#ffffff\"},\"button\":{\"bg\":\"#0088cc\","
                              "\"text\":\"#ffffff\",\"radius\":\"8px\",\"font\":\"600 15px system-ui\",\"padding\":\"6px 12px\",\"height\":\"36px\",\"pressedOpacity\":0.7,"
                              "\"disabledOpacity\":0.4},\"text\":{\"fontSize\":16.5,\"lineHeight\":1.4,\"padding\":\"10px 14px\"}}}";
    {
        std::lock_guard<std::mutex> l(r.mutex);
        CHECK_MSG(r.messages.size() > 3 && r.messages[0] == theme, r.messages.empty() ? "nothing sent" : r.messages[0]);
        CHECK_MSG(r.messages.size() > 3 && r.messages[1] == "{\"type\":\"safeArea\",\"inset\":{\"top\":0,\"right\":0,\"bottom\":24,\"left\":0,\"cornerLeft\":0,\"cornerRight\":72}}",
                  r.messages.size() > 1 ? r.messages[1] : "");
        CHECK_MSG(r.messages.size() > 3 && r.messages[2] == "{\"type\":\"policy\",\"policy\":\"strict\"}", r.messages.size() > 2 ? r.messages[2] : "");
    }
    for (const char* type : {"ops", "listen", "requestFrame"}) CHECK_MSG(!r.sent(type).empty(), type);
    const std::vector<Json> queries = r.sent("query");
    CHECK(std::any_of(queries.begin(), queries.end(), [](const Json& q) { return q["qid"].asInt() > 0; }));
    CHECK(r.drew("hello"));
    CHECK_MSG(w.host->probe("n") == "INTL", w.host->probe("n"));
    const std::vector<std::string> threads = intl->threads();
    CHECK(!threads.empty());
#if defined(__APPLE__) || defined(__linux__)
    for (const std::string& name : threads) CHECK_MSG(name == "widget.client", name);
#endif

    // The hook (`WidgetHost::setHostedChannelFactory`).
    FactoryReset reset;
    SandboxEngine::setRemoteChannelFactory(nullptr);
    HostedTestRenderer* hooked = nullptr;
    ScriptedRenderer* local = nullptr;
    int calls = 0;
    std::string scheme;
    double width = 0;
    bool giveChannel = true;
    struct HookReset {
        ~HookReset() { WidgetHost::setHostedChannelFactory(nullptr); }
    } hookReset;
    WidgetHost::setHostedChannelFactory([&](WidgetHost&, std::shared_ptr<SandboxResources> resources, const RendererFactory&, const std::string& colorScheme,
                                            double bubble) -> std::shared_ptr<SandboxHostedChannel> {
        calls++;
        scheme = colorScheme;
        width = bubble;
        if (!giveChannel) return nullptr;
        return std::make_shared<SandboxHostedLoopbackChannel>(std::move(resources), hostedTest(hooked));
    });
    {
        WidgetTheme dark;
        dark.colorScheme = "dark";
        WidgetHost viaHook("<p>hooked</p>", dark, 280, WidgetHostOptions(), RES, scripted(local));
        CHECK(viaHook.isHosted() && calls == 1 && scheme == "dark" && width == 280 && !local);
        viaHook.start();
        CHECK(viaHook.waitLoaded(20) && viaHook.waitRendered(5));
        CHECK(hooked && hooked->drew("hooked"));
    }
    SandboxEngine::setRemoteChannelFactory([](std::shared_ptr<SandboxResources> resources) -> std::shared_ptr<SandboxRemoteChannel> { return std::make_shared<SandboxLoopbackChannel>(resources); });
    {
        WidgetHost viaEngineFactory("<p>x</p>", WidgetTheme{}, 300, WidgetHostOptions(), RES, scripted(local));
        CHECK(!viaEngineFactory.isHosted() && calls == 1 && local);
    }
    SandboxEngine::setRemoteChannelFactory(nullptr);
    giveChannel = false;
    {
        WidgetHost lost("<p>x</p>", WidgetTheme{}, 300, WidgetHostOptions(), RES, scripted(local));
        lost.start();
        CHECK(lost.isHosted() && calls == 2);
        CHECK(lost.waitUntil(isDead, 5) && lost.diagnostics().dead == std::optional<std::string>(kEngineGone));
    }
}

/// The validated-ops tap: a hosted host that set `onRenderOps` before `start()` gets every batch the engine's side's validator passed, in order; set after start, or unset, it gets none, and the engine's side sends none (no protocol violation).
TEST(hosted_widget_host_gets_validated_ops_only_when_it_asked) {
    const char* html = "<p id='p'>first</p><script>setTimeout(function () { document.getElementById('p').textContent = 'second'; }, 30);</script>";
    std::mutex m;
    std::vector<std::string> asked, late;
    HostedWidget before(html), after(html), unset(html);
    before.host->onRenderOps = [&](const std::string& ops) {
        std::lock_guard<std::mutex> l(m);
        asked.push_back(ops);
    };
    after.host->start();
    after.host->onHostThread([&] {
        after.host->onRenderOps = [&](const std::string& ops) {
            std::lock_guard<std::mutex> l(m);
            late.push_back(ops);
        };
    });
    for (HostedWidget* w : {&before, &after, &unset}) {
        CHECK(w->begin());
        CHECK(eventually([&] { return w->renderer->drew("second"); }));
        w->settle();
        const WidgetDiagnostics d = w->host->diagnostics();
        CHECK_MSG(d.countDiag("protocol-violation") == 0 && !d.dead, d.dead.value_or("alive"));
    }
    std::lock_guard<std::mutex> l(m);
    std::string all;
    for (const std::string& b : asked) all += b;
    const size_t first = all.find("first"), second = all.find("second");
    CHECK_MSG(first != std::string::npos && second != std::string::npos && first < second, all.substr(0, 300));
    CHECK_MSG(late.empty(), std::to_string(late.size()) + " batches after start");
}

/// Its haptics, links, copies and requests (`mode`), and the inputs it reports in `log`.
static const char* kPressWidget =
    "<button id='b'>go</button><script>var mode = 'haptic', log = [], clicks = 0, H = TelegramWidget.HapticFeedback; H.selectionChanged();"
    "TelegramWidget.onEvent('chatDataChanged', function () { log.push('chat:' + TelegramWidget.chatData); });"
    "TelegramWidget.onEvent('themeChanged', function () { log.push('theme:' + TelegramWidget.colorScheme); });"
    "TelegramWidget.onEvent('contentSafeAreaChanged', function () { log.push('inset:' + TelegramWidget.contentSafeAreaInset.bottom); });"
    "document.addEventListener('visibilitychange', function () { log.push('visibility:' + document.visibilityState); });"
    "document.getElementById('b').addEventListener('click', function () { clicks++;"
    "  if (mode === 'haptic') H.impactOccurred('light');"
    "  else if (mode === 'link') TelegramWidget.openLink('https://example.com/' + clicks);"
    "  else if (mode === 'copy') TelegramWidget.copyText('copied ' + clicks);"
    "  else TelegramWidget.request('tap').then(function (a) { log.push('ok:' + a.result); }, function (e) { log.push('err:' + e.code); }); });</script>";

/// A hosted host decides every privileged call on the presses its embedder delivered: a tap the engine's side's renderer reports (and the engine's side allows) is refused without `pressDelivered` and allowed once with it, for links, copies, requests and haptics (outside the boot's series only within a delivered press and the 5 s after its release); the theme, data, safe area and visibility reach the engine's side's renderer and the widget; a CPU kill and a validator's `limit` freeze that renderer with `stoppedLabel`, and its diagnostics (a renderer's violation too) reach the host; `renderer()` throws, `event()` reaches no widget and `onGesture` never runs.
TEST(hosted_widget_host_decides_privileged_calls_on_its_own_presses) {
    WidgetHostOptions options;
    options.context = messageContext(false, true, false, "private");
    HostedWidget w(kPressWidget, WidgetTheme{}, options);
    std::mutex m;
    std::vector<WidgetAction> actions;
    std::vector<std::string> copies, requests;
    int claims = 0;
    w.host->onAction = [&](const WidgetAction& a) {
        std::lock_guard<std::mutex> l(m);
        actions.push_back(a);
    };
    w.host->onCopyText = [&](const std::string& text) {
        std::lock_guard<std::mutex> l(m);
        copies.push_back(text);
    };
    w.host->onRequest = [&](const WidgetRequest& q, std::function<void(const WidgetRequestOutcome&)> respond) {
        {
            std::lock_guard<std::mutex> l(m);
            requests.push_back(q.data);
        }
        respond(answered("yes"));
    };
    w.host->onGesture = [&](const WidgetGestureClaim&) {
        std::lock_guard<std::mutex> l(m);
        claims++;
    };
    CHECK(w.begin());
    int clicks = 0;
    // A press-framed tap of the engine's side's renderer, which the engine's side allows; settled once the widget handled it.
    const auto tap = [&](bool delivered) {
        if (delivered) w.host->pressDelivered();
        w.host->sendPlatform("tap");
        const std::string expected = std::to_string(++clicks);
        CHECK_MSG(probeUntil(*w.host, "clicks", expected) == expected, w.host->probe("clicks"));
        w.settle();
    };
    const auto counted = [&](WidgetAction::Kind kind) {
        std::lock_guard<std::mutex> l(m);
        return std::count_if(actions.begin(), actions.end(), [&](const WidgetAction& a) { return a.kind == kind; });
    };
    sleepMs(int(sdk::HAPTIC_SERIES_MS) + 100);
    tap(false);
    CHECK_MSG(counted(WidgetAction::Haptic) == 1, std::to_string(counted(WidgetAction::Haptic)));
    tap(true);
    CHECK(counted(WidgetAction::Haptic) == 2);
    w.host->releaseDelivered();
    tap(false);
    CHECK_MSG(counted(WidgetAction::Haptic) == 3, std::to_string(counted(WidgetAction::Haptic)));
    // The haptics' press allows no link any more.
    sleepMs(int(SandboxEngine::kRemoteGrantWindowMs) + 100);
    w.host->probe("mode = 'link'");
    tap(false);
    tap(true);
    tap(false);
    w.host->probe("mode = 'copy'");
    tap(false);
    tap(true);
    w.host->probe("mode = 'ask'");
    tap(false);
    tap(true);
    const std::string answeredLog = "err:RATE_LIMITED ok:yes";
    CHECK_MSG(probeUntil(*w.host, "log.join(' ')", answeredLog) == answeredLog, w.host->probe("log.join(' ')"));
    w.settle();
    {
        std::lock_guard<std::mutex> l(m);
        std::vector<std::string> links;
        for (const WidgetAction& a : actions)
            if (a.kind == WidgetAction::OpenLink) links.push_back(a.url);
        CHECK_MSG(links == std::vector<std::string>{"https://example.com/5"}, joined(links));
        CHECK_MSG(copies == std::vector<std::string>{"copied 8"}, joined(copies));
        CHECK(requests == std::vector<std::string>{"tap"});
    }
    WidgetDiagnostics d = w.host->diagnostics();
    const std::vector<std::string> blocked = {"openLink ignored by the host: no tap on the visible widget allows it", "openLink ignored by the host: no tap on the visible widget allows it",
                                              "copyText ignored by the host: no tap on the visible widget allows it"};
    CHECK_MSG(d.diags["activation-blocked"].second == blocked, joined(d.diags["activation-blocked"].second));

    // The host's inputs reach the engine's side.
    WidgetTheme dark = everyToken();
    w.host->setTheme(dark);
    w.host->setData({std::string("v2"), std::nullopt});
    WidgetContentSafeArea area;
    area.bottom = 30;
    w.host->setContentSafeArea(area);
    w.host->setVisible(false);
    const std::string all = "['theme:dark', 'chat:v2', 'inset:30', 'visibility:hidden'].every(function (e) { return log.indexOf(e) >= 0; })";
    CHECK_MSG(probeUntil(*w.host, all, "true") == "true", w.host->probe("log.join(' ')"));
    w.host->setVisible(true);
    CHECK(eventually([&] { return w.renderer->sent("theme").size() == 2 && w.renderer->sent("safeArea").size() == 2; }));
    CHECK_MSG(w.renderer->sent("theme")[1]["theme"]["button"]["radius"].asString() == "8px", w.renderer->sent("theme")[1].dump());
    CHECK(w.renderer->sent("safeArea")[1]["inset"]["bottom"].asNumber() == 30);
    {
        std::lock_guard<std::mutex> l(w.renderer->mutex);
        CHECK(w.renderer->themes == std::vector<std::string>{"dark"});
    }

    // No renderer, event or gesture claim on the host's side; the engine's side's renderer's violation reaches its diagnostics.
    bool threw = false;
    try {
        w.host->renderer();
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
    w.host->event(kTapOnButton);
    w.settle();
    CHECK(w.host->probe("clicks") == std::to_string(clicks));
    w.renderer->onMessage("{\"type\":\"violation\",\"detail\":\"from the engine's side\"}");
    CHECK(w.host->waitUntil([](const WidgetDiagnostics& x) { return x.countDiag("renderer-violation") == 1; }, 5));
    d = w.host->diagnostics();
    CHECK(d.diags["renderer-violation"].second == std::vector<std::string>{"from the engine's side"});
    {
        std::lock_guard<std::mutex> l(m);
        CHECK_MSG(claims == 0, std::to_string(claims));
    }

    // Deaths in the engine's side: its renderer frozen with the host's label.
    {
        HostedWidget busy("<p>x</p><script>setTimeout(function () { for (;;) {} }, 20);</script>");
        busy.host->stoppedLabel = "Stopped by the test";
        busy.host->start();
        CHECK(busy.host->waitUntil(isDead, 10));
        const std::string dead = busy.host->diagnostics().dead.value_or("");
        CHECK_MSG(dead.rfind("CPU limit exceeded", 0) == 0, dead);
        CHECK(eventually([&] { return busy.renderer->sent("frozen").size() == 1; }));
        const Json frozen = busy.renderer->sent("frozen")[0];
        CHECK_MSG(frozen["reason"].asString() == "Stopped by the test" && frozen["fatal"].asBool(), frozen.dump());
    }
    {
        HostedWidget capped("<p id=p>start</p><script>setTimeout(function () { document.getElementById('p').textContent = 'x'.repeat(40000); }, 10);</script>");
        capped.host->start();
        CHECK(capped.host->waitUntil(isDead, 10));
        WidgetDiagnostics c = capped.host->diagnostics();
        CHECK_MSG(c.dead == std::optional<std::string>("Text limit exceeded: a text value of 40000 characters (max 32768)"), c.dead.value_or("alive"));
        CHECK_MSG(c.countDiag("limit") == 1 && c.diags["limit"].second[0] == *c.dead, std::to_string(c.countDiag("limit")));
        CHECK(eventually([&] { return capped.renderer->sent("frozen").size() == 1; }));
        CHECK(capped.renderer->sent("frozen")[0]["reason"].asString() == "Widget stopped");
        CHECK(!capped.renderer->drew(std::string(64, 'x')));
    }
}

/// Its anchor and its text change in one task, after its first batch.
static const char* kAnchoredWidget = "<p id='p'>x</p><script>setTimeout(function () { TelegramWidget.setResizeAnchor('bottom');"
                                     " document.getElementById('p').textContent = 'anchored'; }, 30);</script>";

/// Platform traffic both ways, in order, on the host thread and in order with the widget's other reports (the resize anchor); a request answered through `onPlatformRequest`; one left unanswered kills the widget 3 to 4 s after it (`platform-timeout`), the renderer's request gets none and a late `respond` changes nothing; without an `onPlatformRequest` the widget dies at once; `destroy()` while a request waits returns within 1 s.
TEST(hosted_widget_host_platform_traffic_and_requests) {
    std::mutex m;
    std::vector<std::pair<std::string, std::string>> heard;
    bool onHost = true;
    bool answer = true;
    std::function<void(std::string)> kept;
    HostedWidget w(kAnchoredWidget, WidgetTheme{}, WidgetHostOptions(), nullptr, [](HostedTestRenderer& r) { r.echoOps = true; });
    std::thread::id hostThread;
    w.host->onHostThread([&] { hostThread = std::this_thread::get_id(); });
    w.host->onPlatformMessage = [&](const std::string& message) {
        const std::string anchor = w.host->diagnostics().resizeAnchor;
        std::lock_guard<std::mutex> l(m);
        onHost = onHost && std::this_thread::get_id() == hostThread;
        heard.emplace_back(message, anchor);
    };
    w.host->onPlatformRequest = [&](const std::string& message, std::function<void(std::string)> respond) {
        std::lock_guard<std::mutex> l(m);
        onHost = onHost && std::this_thread::get_id() == hostThread;
        if (answer) return respond("answer:" + message);
        kept = std::move(respond);
    };
    const auto said = [&] {
        std::lock_guard<std::mutex> l(m);
        std::vector<std::string> out;
        for (const auto& [message, anchor] : heard)
            if (message.rfind("ops:", 0) != 0) out.push_back(message);
        return out;
    };
    CHECK(w.begin());
    // The batch of the anchor's task is heard after the anchor.
    CHECK(eventually([&] { return w.renderer->drew("anchored"); }));
    const std::vector<Json> batches = w.renderer->sent("ops");
    size_t anchored = 0;
    while (anchored < batches.size() && batches[anchored].dump().find("anchored") == std::string::npos) anchored++;
    const std::string echo = "ops:" + std::to_string(anchored + 1);
    CHECK(eventually([&] {
        std::lock_guard<std::mutex> l(m);
        return std::any_of(heard.begin(), heard.end(), [&](const auto& h) { return h.first == echo; });
    }));
    {
        std::lock_guard<std::mutex> l(m);
        std::string order;
        for (const auto& [message, anchor] : heard)
            if (message.rfind("ops:", 0) == 0) order += message + "=" + anchor + " ";
        CHECK_MSG(order.find(echo + "=bottom ") != std::string::npos, order);
    }
    std::vector<std::string> expected, sent;
    for (int i = 1; i <= 20; i++) {
        expected.push_back(std::to_string(i));
        sent.push_back("say:" + std::to_string(i));
        w.host->sendPlatform("say:" + std::to_string(i));
    }
    CHECK(eventually([&] { return said().size() == expected.size(); }));
    CHECK_MSG(said() == expected, joined(said()));
    CHECK_MSG(w.renderer->platformMessages() == sent, joined(w.renderer->platformMessages()));
    w.host->sendPlatform("ask:q1");
    expected.push_back("got:answer:q1");
    CHECK(eventually([&] { return said().size() == expected.size(); }));
    CHECK_MSG(said() == expected, joined(said()));
    CHECK(w.renderer->answers() == std::vector<std::optional<std::string>>{std::string("answer:q1")});

    // Unanswered: dead 3 to 4 s after the request with its reason, the renderer's request answered none.
    {
        std::lock_guard<std::mutex> l(m);
        answer = false;
    }
    const auto askedAt = std::chrono::steady_clock::now();
    w.host->sendPlatform("ask:q2");
    CHECK(w.host->waitUntil(isDead, 6));
    const double waited = secondsSince(askedAt);
    CHECK_MSG(waited >= WidgetHost::kPlatformAnswerSeconds && waited < WidgetHost::kPlatformAnswerSeconds + 1, std::to_string(waited));
    WidgetDiagnostics d = w.host->diagnostics();
    const std::string late = "the host's platform did not answer the widget's renderer within 3 s";
    CHECK_MSG(d.dead == std::optional<std::string>(late), d.dead.value_or("alive"));
    CHECK(d.diags["platform-timeout"].second == std::vector<std::string>{late});
    CHECK(eventually([&] { return w.renderer->answers().size() == 2; }));
    CHECK(w.renderer->answers() == (std::vector<std::optional<std::string>>{std::string("answer:q1"), std::nullopt}));
    std::function<void(std::string)> respond;
    {
        std::lock_guard<std::mutex> l(m);
        respond = kept;
    }
    CHECK(bool(respond));
    if (respond) respond("too late");
    w.settle();
    sleepMs(50);
    CHECK(w.renderer->answers().size() == 2 && w.host->diagnostics().dead == std::optional<std::string>(late));
    {
        std::lock_guard<std::mutex> l(m);
        CHECK(onHost);
    }

    // No `onPlatformRequest`: dead at once.
    {
        HostedWidget none("<p>x</p>");
        CHECK(none.begin());
        const auto at = std::chrono::steady_clock::now();
        none.host->sendPlatform("ask:anyone");
        CHECK(none.host->waitUntil(isDead, 3));
        CHECK_MSG(secondsSince(at) < 1, std::to_string(secondsSince(at)));
        WidgetDiagnostics n = none.host->diagnostics();
        CHECK_MSG(n.dead == std::optional<std::string>("the host's platform answers no requests of the widget's renderer"), n.dead.value_or("alive"));
        CHECK(n.countDiag("platform-timeout") == 1);
        CHECK(eventually([&] { return none.renderer->answers() == std::vector<std::optional<std::string>>{std::nullopt}; }));
    }

    // destroy() while a request waits.
    {
        std::atomic<bool> asked{false};
        HostedWidget waiting("<p>x</p>");
        waiting.host->onPlatformRequest = [&](const std::string&, std::function<void(std::string)> keep) {
            std::lock_guard<std::mutex> l(m);
            kept = std::move(keep);
            asked = true;
        };
        CHECK(waiting.begin());
        waiting.host->sendPlatform("ask:forever");
        CHECK(eventually([&] { return asked.load(); }));
        const auto at = std::chrono::steady_clock::now();
        waiting.host->destroy();
        CHECK_MSG(secondsSince(at) < 1, std::to_string(secondsSince(at)));
        CHECK(waiting.host->waitUntil(isDead, 2) && waiting.host->diagnostics().dead == std::optional<std::string>("terminated"));
        CHECK(waiting.host->diagnostics().countDiag("platform-timeout") == 0);
        CHECK(eventually([&] { return waiting.renderer->answers() == std::vector<std::optional<std::string>>{std::nullopt}; }));
        std::function<void(std::string)> after;
        {
            std::lock_guard<std::mutex> l(m);
            after = kept;
        }
        if (after) after("after destroy");
    }
}

/// The widget's scheme and bottom inset, as a probe reads them.
static const char* kSchemeAndInset = "TelegramWidget.colorScheme + ':' + TelegramWidget.contentSafeAreaInset.bottom";

/// A hosted probe is a barrier behind the inputs before it, as in process, over the loopback and over a hosting stream: with the engine's side's glue thread held 200 ms by a platform message (`stall`), a safe area and a theme sent after it, which that thread relays to the engine, and then `onHostThread`, the first probe already sees the new inset and scheme.
TEST(hosted_probe_comes_after_the_inputs_before_it) {
    WidgetContentSafeArea area;
    area.bottom = 30;
    WidgetTheme dark;
    dark.colorScheme = "dark";
    const auto probed = [&](WidgetHost& host, const std::string& transport) {
        host.start();
        CHECK_MSG(host.waitLoaded(20) && host.waitRendered(5) && !host.diagnostics().dead, transport + ": " + host.diagnostics().dead.value_or("not shown"));
        CHECK_MSG(host.probe(kSchemeAndInset) == "light:0", transport + ": " + host.probe(kSchemeAndInset));
        host.sendPlatform("stall");
        host.setContentSafeArea(area);
        host.setTheme(dark);
        host.onHostThread([] {});
        const std::string seen = host.probe(kSchemeAndInset);
        CHECK_MSG(seen == "dark:30", transport + ": " + seen);
    };
    {
        HostedWidget w("<p>x</p>");
        probed(*w.host, "loopback");
    }
    {
        HostedTestRenderer* renderer = nullptr;
        StreamPeers peers(kBuild, *RES, nullptr, hostedTest(renderer));
        WidgetHost host("<p>x</p>", WidgetTheme{}, 300, WidgetHostOptions(), peers.end->hostedChannel());
        probed(host, "stream");
    }
}

/// `source` with its first line replaced by `line`.
static std::string restampedWith(const std::string& source, const std::string& line) {
    const size_t nl = source.find('\n');
    return line + "\n" + (nl == std::string::npos ? std::string() : source.substr(nl + 1));
}

/// A renderer factory that returns nothing or throws, and resources whose validator bundle is another release's, each end only that widget, with the reason reported to the host; the last never draws, and its `bundle-mismatch` comes from the bytes the engine's side was given.
TEST(hosted_engine_end_build_failures_stop_their_runtime) {
    const std::string notBuilt = "the widget could not be built in the engine process: ";
    const std::vector<std::pair<HostedRendererFactory, std::string>> factories = {
        {[](std::shared_ptr<SandboxResources>, const std::string&, double, std::shared_ptr<SandboxHostedPlatform>) -> std::unique_ptr<Renderer> { return nullptr; },
         "WidgetHost: the renderer factory returned nothing"},
        {[](std::shared_ptr<SandboxResources>, const std::string&, double, std::shared_ptr<SandboxHostedPlatform>) -> std::unique_ptr<Renderer> {
             throw std::runtime_error("no renderer in this process");
         },
         "no renderer in this process"},
    };
    for (const auto& [factory, what] : factories) {
        WidgetHost host("<p>x</p>", WidgetTheme{}, 300, WidgetHostOptions(), std::make_shared<SandboxHostedLoopbackChannel>(RES, factory));
        host.start();
        CHECK(host.waitUntil(isDead, 10));
        const WidgetDiagnostics d = host.diagnostics();
        CHECK_MSG(d.dead == std::optional<std::string>(notBuilt + what), d.dead.value_or("alive"));
        CHECK(!d.loaded && !d.rendererReadyMs);
        CHECK(host.probe("1") == "ERROR no runtime");
    }
    const BundleStamp mine = BundleStamp::glue();
    auto other = std::make_shared<SandboxResources>(*RES);
    other->validatorBundle = restampedWith(other->validatorBundle, BundleStamp{"9.9.9", mine.protocol, mine.profile + 1, mine.sdk}.line());
    const std::string refusal = BundleStamp::refusal(*other).value_or("");
    CHECK(refusal.rfind("validator-bundle.js is ", 0) == 0);
    HostedWidget stale("<p>never drawn</p>", WidgetTheme{}, WidgetHostOptions(), nullptr, nullptr, other);
    std::mutex m;
    size_t batches = 0;
    stale.host->onRenderOps = [&](const std::string&) {
        std::lock_guard<std::mutex> l(m);
        batches++;
    };
    stale.host->start();
    CHECK(stale.host->waitUntil(isDead, 20));
    stale.settle();
    WidgetDiagnostics d = stale.host->diagnostics();
    CHECK_MSG(d.countDiag("bundle-mismatch") >= 1, d.dead.value_or("alive"));
    for (const std::string& detail : d.diags["bundle-mismatch"].second) CHECK_MSG(detail == refusal, detail);
    CHECK(d.nodes == 0);
    CHECK(stale.renderer && stale.renderer->sent("ops").empty() && !stale.renderer->drew("never drawn"));
    std::lock_guard<std::mutex> l(m);
    CHECK(batches == 0);
}
