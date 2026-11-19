// The hosted mode's tests (remote_hosted.h: its coding, a hosted client over the fake channel, a hosted WidgetHost over the loopback, and a probe's order over the loopback and a hosting stream), in the suite after the byte stream's: `twidget-tests hosted_`.
#include "test_support.h"
#include "test_remote_support.h"
#include "test_remote_stream_support.h"

#include "twidget/bundle_stamp.h"
#include "twidget/intl.h"
#include "twidget/js_thread.h"
#include "twidget/json.h"
#include "twidget/remote_engine.h"
#include "twidget/remote_hosted.h"
#include "twidget/sandbox_engine.h"
#include "twidget/sdk_policy.h"
#include "twidget/widget_host.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

/// A hosted client over a fake channel, its recorder set first (the recorder outlives the engine), hosted-started with `start` unless `begin` is false; with `wait` its platform requests are due as that record says.
struct HostedRun {
    std::shared_ptr<FakeRemoteChannel> channel = std::make_shared<FakeRemoteChannel>();
    RemoteRecorder recorder;
    std::unique_ptr<SandboxEngine> engine;
    explicit HostedRun(SandboxConfig config = hostedConfig(), SandboxHostedStart start = SandboxHostedStart(), bool begin = true, std::shared_ptr<SandboxHostedWait> wait = nullptr)
        : engine(SandboxEngine::hostedClient("<p>x</p>", std::move(config), channel, nullptr, std::move(wait))) {
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

/// A hosted client's grants come only from `pressDelivered` and `platformPress` (the latter only once the channel's writer admitted its message on the open route that was never kept, timed from its call; a refused down touches no haptic gate and its release opens no window): its `start()` and the other engine-only inputs reach the channel never (only `startHosted`, once), and neither they nor the engine's side's own word (a platform "press") allow a privileged call or open the haptic window; each delivered press allows one link, copy or tapped request within `kRemoteGrantWindowMs`, none while hidden or in a preview, and the newest `kRemoteMaxGrants` are kept.
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
        // A press and its message in one step (`platformPress`): a call queued before it finds no grant, the message goes out and the press allows one call after it; hidden, the message goes out and allows nothing; once the route closed, neither.
        HostedRun r;
        r.sink().remoteCopyText("before");
        r.engine->platformPress("down 1");
        r.sink().remoteCopyText("pressed");
        r.sink().remoteCopyText("again");
        r.engine->setVisible(false);
        r.engine->platformPress("down 2");
        r.engine->setVisible(true);
        r.sink().remoteCopyText("hidden press");
        r.settle();
        CHECK_MSG(callsWith(*r.channel, "platform:") == (std::vector<std::string>{"platform:down 1", "platform:down 2"}), joined(r.channel->calls()));
        CHECK_MSG(r.recorder.eventsWith("copy:") == std::vector<std::string>{"copy:pressed"}, joined(r.recorder.events()));
        CHECK_MSG(r.recorder.diags().size() == 3, joined(r.recorder.events()));
        r.sink().remoteDied("CPU limit exceeded");
        r.settle();
        r.engine->platformPress("down 3");
        r.settle();
        CHECK_MSG(callsWith(*r.channel, "platform:").size() == 2 && r.channel->closes() == 1, joined(r.channel->calls()));
    }
    {
        // The writer's word decides (`SandboxHostedChannel::platform`): a down it refused on the open route grants nothing; the next one it admits grants as before.
        HostedRun r;
        r.channel->admitsPlatform = false;
        r.engine->platformPress("refused down");
        r.sink().remoteCopyText("after the refusal");
        r.settle();
        CHECK_MSG(callsWith(*r.channel, "platform:") == std::vector<std::string>{"platform:refused down"}, joined(r.channel->calls()));
        CHECK_MSG(r.recorder.eventsWith("copy:").empty() && r.recorder.diags().size() == 1, joined(r.recorder.events()));
        r.channel->admitsPlatform = true;
        r.engine->platformPress("admitted down");
        r.sink().remoteCopyText("after the admission");
        r.sink().remoteCopyText("again");
        r.settle();
        CHECK_MSG(r.recorder.eventsWith("copy:") == std::vector<std::string>{"copy:after the admission"}, joined(r.recorder.events()));
        CHECK(r.recorder.deaths().empty());
    }
    {
        // Timed from the call, not from the job: a down whose job waited 1.2 s behind the dispatch thread grants nothing 2.3 s after its call; a fresh one grants at once.
        Gate gate;
        HostedRun r;
        r.engine->thread().async([&gate] { gate.wait(); });
        CHECK(gate.waitedOn());
        const auto called = std::chrono::steady_clock::now();
        r.engine->platformPress("down held behind the thread");
        sleepMs(1200);
        gate.open();
        r.settle();
        std::this_thread::sleep_until(called + std::chrono::milliseconds(2300));
        r.sink().remoteCopyText("2.3 s after the call");
        r.engine->platformPress("fresh down");
        r.sink().remoteCopyText("after the fresh down");
        r.settle();
        CHECK_MSG(r.recorder.eventsWith("copy:") == std::vector<std::string>{"copy:after the fresh down"}, joined(r.recorder.events()));
    }
    {
        // A refused down's haptics: no touch while it would be down and no 5 s window from its release; an admitted one opens both.
        HostedRun r;
        r.sink().remoteBridge("haptic", "[\"selection\"]");
        r.settle();
        CHECK(r.recorder.eventsWith("action:").size() == 1);
        sleepMs(int(sdk::HAPTIC_SERIES_MS) + 100);
        r.channel->admitsPlatform = false;
        r.engine->platformPress("refused down");
        r.sink().remoteBridge("haptic", "[\"impact\",\"soft\"]");
        r.settle();
        r.engine->releaseDelivered();
        r.sink().remoteBridge("haptic", "[\"notification\",\"success\"]");
        r.settle();
        CHECK_MSG(r.recorder.eventsWith("action:").size() == 1, joined(r.recorder.events()));
        r.channel->admitsPlatform = true;
        r.engine->platformPress("admitted down");
        r.sink().remoteBridge("haptic", "[\"impact\",\"soft\"]");
        r.settle();
        r.engine->releaseDelivered();
        r.sink().remoteBridge("haptic", "[\"notification\",\"success\"]");
        r.settle();
        const std::vector<std::string> actions = r.recorder.eventsWith("action:");
        CHECK_MSG(actions.size() == 3 && actions[1] == "action:{\"kind\":\"haptic\",\"haptic\":\"impact\",\"value\":\"soft\"}" &&
                      actions[2] == "action:{\"kind\":\"haptic\",\"haptic\":\"notification\",\"value\":\"success\"}",
                  joined(actions));
        CHECK_MSG(callsWith(*r.channel, "platform:") == (std::vector<std::string>{"platform:refused down", "platform:admitted down"}), joined(r.channel->calls()));
    }
    {
        // Kept before the press's job: the down still goes out on the kept route (its renderer's platform traffic) and grants nothing.
        RemoteRecorder kept;
        HostedRun r;
        r.sink().remoteRendererReady();
        r.settle();
        CHECK(r.engine->keepHosted(kept));
        r.engine->platformPress("down after the keep");
        r.engine->releaseDelivered();
        r.settle();
        CHECK_MSG(callsWith(*r.channel, "platform:") == std::vector<std::string>{"platform:down after the keep"}, joined(r.channel->calls()));
        CHECK_MSG(kept.events().empty(), joined(kept.events()));
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

static const std::string kUnanswered = "the host's platform answers no requests of the widget's renderer";

/// What a hosted runtime's engine's side sends is bounded by the client: the render calls a hosted widget keeps to itself are protocol violations before they are admitted, an engine-only client refuses the hosted calls, validated ops come only when asked for, node counts are clamped, a platform message or request past its bound is refused, a request is delivered on the dispatch thread and gets the delegate's answer, and `unanswered()` kills the widget once (`platform-timeout` first).
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
        // No answer: the diagnostic, then the widget dies once with that reason, the channel hears the stop and closes; later requests get none and reach no delegate.
        r.recorder.platformAnswer = std::nullopt;
        CHECK(!requestedFrom([&] { return r.sink().remotePlatformRequest("unanswered"); }));
        CHECK(!requestedFrom([&] { return r.sink().remotePlatformRequest("late"); }));
        r.settle();
        CHECK_MSG(r.recorder.deaths() == std::vector<std::string>{kUnanswered}, joined(r.recorder.events()));
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"hosted-request:ask", "diag:protocol-violation", "hosted-request:unanswered", "diag:platform-timeout", "died:" + kUnanswered}),
                  joined(r.recorder.events()));
        CHECK(r.recorder.diags().size() == 2 && r.recorder.diags()[1] == (std::pair<std::string, std::string>{"platform-timeout", kUnanswered}));
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

// ───────────── a kept route (SandboxEngine::keepHosted) ─────────────

static const std::string kFlooded = "the widget engine process sent more than the host keeps queued";
static const std::string kNoPlatform = "the host's platform answers no requests of the widget's renderer";
static const std::string kPlatformLate = "the host's platform did not answer the widget's renderer within 3 s";

/// `r`'s client kept with `kept` as its delegate once its renderer announced itself and a press was delivered; true when it kept.
static bool keptOver(HostedRun& r, RemoteRecorder& kept) {
    r.sink().remoteRendererReady();
    r.press();
    return r.engine->keepHosted(kept);
}

/// The dispatch thread of `r` held on `gate` until the test opens it.
static bool holdDispatch(HostedRun& r, Gate& gate) {
    r.engine->thread().async([&gate] { gate.wait(); });
    return gate.waitedOn();
}

/// A kept route carries the renderer's platform traffic alone: the keep of a live widget is its death "terminated" through the old delegate and a `stop` the channel hears before `kept`, without a close; afterwards the engine's side's own death report closes nothing and no other call of it reaches either delegate (its Intl request answered "", its request no answer), while platform messages and requests reach the new delegate in order and the host's platform messages go out. A widget kept past its death (`keepAtDeath`) keeps its route open, its platform traffic still carried, and is kept with no stop; without it the death closes the route. Nothing is kept for a client never started, whose renderer never announced itself, that was flooded or lost its engine.
TEST(hosted_client_keeps_only_its_renderers_traffic) {
    {
        RemoteRecorder kept;
        HostedRun r;
        CHECK(keptOver(r, kept));
        const std::vector<std::string> atKeep = r.channel->calls();
        CHECK_MSG(atKeep.size() >= 3 && atKeep[atKeep.size() - 2] == "stop:terminated" && atKeep.back() == "kept" && r.channel->closes() == 0, joined(atKeep));
        CHECK_MSG(r.recorder.deaths() == std::vector<std::string>{"terminated"}, joined(r.recorder.events()));
        CHECK(!r.engine->keepHosted(kept));
        const std::vector<std::string> before = r.recorder.events();
        SandboxRemoteSink& sink = r.sink();
        sink.remoteDied("terminated");
        sink.remoteTaskStarted();
        sink.remoteBridge("openLink", linkArgs("https://example.com/kept"));
        sink.remoteBridge("haptic", "[\"selection\"]");
        sink.remoteCopyText("kept");
        sink.remoteStorage("{\"local\":{\"k\":\"v\"}}");
        sink.remoteRequest(1, "kept", false);
        sink.remoteWallet(2, "{}");
        sink.remoteResizeAnchor("bottom");
        sink.remoteWatchesPublicData(true);
        sink.remoteLoaded();
        sink.remoteStats(SandboxStats{});
        sink.remoteLog("log", "kept");
        sink.remoteDiag("kind", "kept");
        sink.remoteValidatedOps("[]");
        sink.remoteRendererReady();
        sink.remoteRendered();
        sink.remoteNodes(3);
        sink.remoteDied("later");
        sink.remoteTaskCompleted();
        CHECK(sink.remoteIntl("{\"kind\":\"number\",\"value\":1}").empty());
        sink.remotePlatformMessage("after");
        CHECK(requestedFrom([&] { return r.sink().remotePlatformRequest("asked"); }) == std::optional<std::string>("platform-answer"));
        sink.remotePlatformMessage("last");
        r.engine->platform("p");
        r.settle();
        CHECK_MSG(r.recorder.events() == before, joined(r.recorder.events()));
        CHECK_MSG((kept.events() == std::vector<std::string>{"hosted-message:after", "hosted-request:asked", "hosted-message:last"}), joined(kept.events()));
        const std::vector<std::string> calls = r.channel->calls();
        CHECK_MSG((std::vector<std::string>(calls.begin() + long(atKeep.size()), calls.end()) == std::vector<std::string>{"platform:p"}), joined(calls));
        CHECK(r.channel->answers().empty() && r.channel->closes() == 0);
        CHECK_MSG(backlogIs(*r.engine, 0, 0), backlogText(*r.engine));
    }
    {
        // Kept past its death: the route stays open and carries the platform traffic both ways, then the keep sends no stop.
        RemoteRecorder kept;
        HostedRun r(hostedConfig(), SandboxHostedStart(), false);
        r.engine->startHosted(SandboxHostedStart(), true);
        r.sink().remoteRendererReady();
        r.sink().remoteDied("CPU limit exceeded");
        r.sink().remotePlatformMessage("dead");
        CHECK(requestedFrom([&] { return r.sink().remotePlatformRequest("dead ask"); }) == std::optional<std::string>("platform-answer"));
        r.engine->platform("to the dead");
        r.settle();
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"renderer-ready", "died:CPU limit exceeded", "hosted-message:dead", "hosted-request:dead ask"}), joined(r.recorder.events()));
        CHECK(callsWith(*r.channel, "platform:") == std::vector<std::string>{"platform:to the dead"} && r.channel->closes() == 0);
        CHECK(r.engine->keepHosted(kept));
        CHECK_MSG(callsWith(*r.channel, "stop:").empty() && r.channel->calls().back() == "kept" && r.channel->closes() == 0, joined(r.channel->calls()));
        CHECK(r.recorder.deaths() == std::vector<std::string>{"CPU limit exceeded"} && kept.events().empty());
    }
    {
        // Without it the death closes the route: nothing more crosses, and nothing is kept.
        RemoteRecorder kept;
        HostedRun r;
        r.sink().remoteRendererReady();
        r.sink().remoteDied("CPU limit exceeded");
        r.settle();
        CHECK(r.channel->closes() == 1 && callsWith(*r.channel, "stop:").empty());
        CHECK(!r.engine->keepHosted(kept));
        r.sink().remotePlatformMessage("dead");
        CHECK(!requestedFrom([&] { return r.sink().remotePlatformRequest("dead ask"); }));
        r.engine->platform("to the dead");
        r.settle();
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"renderer-ready", "died:CPU limit exceeded"}), joined(r.recorder.events()));
        CHECK(callsWith(*r.channel, "platform:").empty() && kept.events().empty());
    }
    {
        RemoteRecorder kept;
        HostedRun unstarted(hostedConfig(), SandboxHostedStart(), false);
        unstarted.sink().remoteRendererReady();
        unstarted.settle();
        CHECK(!unstarted.engine->keepHosted(kept));
        HostedRun unready;
        CHECK(!unready.engine->keepHosted(kept));
        HostedRun flooded;
        flooded.sink().remoteRendererReady();
        Gate gate;
        CHECK(holdDispatch(flooded, gate));
        for (size_t i = 0; i <= SandboxEngine::kRemoteQueuedCalls; i++) flooded.sink().remoteLoaded();
        gate.open();
        flooded.settle();
        CHECK(flooded.recorder.deaths() == std::vector<std::string>{kFlooded});
        CHECK(!flooded.engine->keepHosted(kept));
        HostedRun lost;
        lost.sink().remoteRendererReady();
        lost.sink().remoteDisconnected();
        lost.settle();
        CHECK(!lost.engine->keepHosted(kept));
        CHECK(unstarted.recorder.deaths().empty() && unready.recorder.deaths().empty() && lost.recorder.deaths() == std::vector<std::string>{kEngineGone});
        CHECK(kept.events().empty());
        for (HostedRun* r : {&unstarted, &unready, &flooded, &lost}) CHECK_MSG(callsWith(*r->channel, "kept").empty(), joined(r->channel->calls()));
    }
}

/// The client ends a kept route itself when the engine's side floods it or loses it, or when the host's platform leaves a request unanswered (at once for `unanswered()`, 3 to 4 s after the request for a responder kept unanswered, a late answer ignored): the kept delegate hears `onHostedEnded` once with the reason, after the timeout's diagnostic, and the channel hears the close alone (its widget is dead). Destroying the client closes the route without `onHostedEnded`, and a request after it answers none at once. A second request while one waits answers none at once, a protocol violation on a live client and nothing more on a kept one, and the first is answered.
TEST(hosted_client_ends_a_kept_route_it_decides_to_end) {
    const auto secondsFrom = [](std::chrono::steady_clock::time_point at) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - at).count(); };
    {
        RemoteRecorder kept;
        HostedRun r;
        CHECK(keptOver(r, kept));
        Gate gate;
        CHECK(holdDispatch(r, gate));
        for (size_t i = 0; i <= SandboxEngine::kRemoteQueuedCalls; i++) r.sink().remoteLoaded();
        gate.open();
        r.settle();
        r.sink().remotePlatformMessage("after");
        r.settle();
        CHECK_MSG(kept.events() == std::vector<std::string>{"hosted-ended:" + kFlooded}, joined(kept.events()));
        CHECK_MSG(callsWith(*r.channel, "stop:") == std::vector<std::string>{"stop:terminated"} && r.channel->closes() == 1, joined(r.channel->calls()));
        CHECK_MSG(backlogIs(*r.engine, 0, 0), backlogText(*r.engine));
    }
    {
        RemoteRecorder kept;
        kept.platformAnswer = std::nullopt;
        HostedRun r;
        CHECK(keptOver(r, kept));
        CHECK(!requestedFrom([&] { return r.sink().remotePlatformRequest("none"); }));
        CHECK(!requestedFrom([&] { return r.sink().remotePlatformRequest("after"); }));
        r.settle();
        CHECK_MSG((kept.events() == std::vector<std::string>{"hosted-request:none", "diag:platform-timeout", "hosted-ended:" + kNoPlatform}), joined(kept.events()));
        CHECK(kept.diags() == (std::vector<std::pair<std::string, std::string>>{{"platform-timeout", kNoPlatform}}));
        CHECK_MSG(callsWith(*r.channel, "stop:") == std::vector<std::string>{"stop:terminated"} && r.channel->closes() == 1, joined(r.channel->calls()));
    }
    {
        RemoteRecorder kept;
        kept.holdPlatformRequests = true;
        HostedRun r;
        CHECK(keptOver(r, kept));
        const auto at = std::chrono::steady_clock::now();
        CHECK(!requestedFrom([&] { return r.sink().remotePlatformRequest("forever"); }));
        const double waited = secondsFrom(at);
        CHECK_MSG(waited >= SandboxEngine::kPlatformAnswerSeconds && waited < SandboxEngine::kPlatformAnswerSeconds + 1, std::to_string(waited));
        r.settle();
        std::optional<HostedRequestResponder> late = kept.platformResponder(0);
        CHECK(late.has_value());
        if (late) (*late)("too late");
        r.settle();
        CHECK_MSG((kept.events() == std::vector<std::string>{"hosted-request:forever", "diag:platform-timeout", "hosted-ended:" + kPlatformLate}), joined(kept.events()));
        CHECK(callsWith(*r.channel, "stop:") == std::vector<std::string>{"stop:terminated"} && r.channel->closes() == 1);
    }
    {
        RemoteRecorder kept;
        HostedRun r;
        CHECK(keptOver(r, kept));
        r.sink().remoteDisconnected();
        r.sink().remoteDisconnected();
        r.settle();
        CHECK_MSG(kept.events() == std::vector<std::string>{"hosted-ended:" + kEngineGone}, joined(kept.events()));
        CHECK(callsWith(*r.channel, "stop:") == std::vector<std::string>{"stop:terminated"} && r.channel->closes() == 1);
    }
    {
        // The handle's release: the client goes, the route closes once, nothing ends it, and the client's sink answers a later request at once.
        RemoteRecorder kept;
        HostedRun r;
        CHECK(keptOver(r, kept));
        const std::shared_ptr<SandboxRemoteSink> sink = r.channel->sink();
        r.engine.reset();
        CHECK(r.channel->closes() == 1 && callsWith(*r.channel, "stop:") == std::vector<std::string>{"stop:terminated"});
        const auto at = std::chrono::steady_clock::now();
        CHECK(!requestedFrom([&] { return sink->remotePlatformRequest("after the release"); }));
        CHECK_MSG(secondsFrom(at) < 1, std::to_string(secondsFrom(at)));
        CHECK_MSG(kept.events().empty(), joined(kept.events()));
    }
    for (bool keep : {false, true}) {
        RemoteRecorder kept;
        HostedRun r;
        if (keep) CHECK(keptOver(r, kept));
        RemoteRecorder& delegate = keep ? kept : r.recorder;
        delegate.holdPlatformRequests = true;
        const std::vector<std::string> before = r.recorder.events();
        Background<std::optional<std::string>> first([&] { return r.sink().remotePlatformRequest("first"); });
        CHECK(eventually([&] { return delegate.platformResponder(0).has_value(); }));
        const auto at = std::chrono::steady_clock::now();
        CHECK(!requestedFrom([&] { return r.sink().remotePlatformRequest("second"); }));
        CHECK_MSG(secondsFrom(at) < 1, std::to_string(secondsFrom(at)));
        if (std::optional<HostedRequestResponder> respond = delegate.platformResponder(0)) (*respond)("first answer");
        CHECK(first.finish(5) == std::optional<std::string>("first answer"));
        r.settle();
        CHECK_MSG(delegate.eventsWith("hosted-request:") == std::vector<std::string>{"hosted-request:first"}, joined(delegate.events()));
        if (keep) {
            CHECK_MSG(kept.events() == std::vector<std::string>{"hosted-request:first"} && r.recorder.events() == before, joined(kept.events()));
        } else {
            CHECK_MSG((r.recorder.events() == std::vector<std::string>{"hosted-request:first", "diag:protocol-violation"}), joined(r.recorder.events()));
            CHECK(r.recorder.diags() == (std::vector<std::pair<std::string, std::string>>{{"protocol-violation", "a second platform request while one waits"}}));
            CHECK(r.recorder.deaths().empty());
        }
        CHECK(r.channel->closes() == 0);
    }
}

// ───────────── the embedder's hold of a platform request (SandboxHostedWait) ─────────────

using WaitClock = SandboxHostedWait::Clock;

static WaitClock::duration secondsOf(double seconds) { return std::chrono::duration_cast<WaitClock::duration>(std::chrono::duration<double>(seconds)); }

static double between(WaitClock::time_point from, WaitClock::time_point to) { return std::chrono::duration<double>(to - from).count(); }

/// What one held platform request of a hosted client with a SandboxHostedWait came to (`heldRequest`): what the renderer got and when, its deadline once the delegate held it, the deadlines and moments `during` noted, and what the delegate heard.
struct HeldOutcome {
    std::optional<std::string> answer;
    WaitClock::time_point asked;
    WaitClock::time_point ended;
    std::optional<WaitClock::time_point> firstDue;
    std::vector<std::optional<WaitClock::time_point>> dues;
    std::vector<WaitClock::time_point> marks;
    std::optional<WaitClock::time_point> dueAfter;
    std::vector<std::string> deaths;
    std::vector<std::pair<std::string, std::string>> diags;
};

/// One platform request ("held") of a hosted client over a fake channel with a SandboxHostedWait, its delegate keeping the responder: `before` runs on the record before the request, `during` once the delegate holds it (it may answer through the responder); nothing answers it otherwise.
static HeldOutcome heldRequest(const std::function<void(SandboxHostedWait&)>& before, const std::function<void(SandboxHostedWait&, const HostedRequestResponder&, HeldOutcome&)>& during) {
    HeldOutcome out;
    auto wait = std::make_shared<SandboxHostedWait>();
    HostedRun r(hostedConfig(), SandboxHostedStart(), true, wait);
    r.recorder.holdPlatformRequests = true;
    if (before) before(*wait);
    out.asked = WaitClock::now();
    Background<std::optional<std::string>> asking([&] { return r.sink().remotePlatformRequest("held"); });
    std::optional<HostedRequestResponder> respond;
    eventually([&] { return (respond = r.recorder.platformResponder(0)).has_value(); });
    if (respond) {
        out.firstDue = wait->due(respond->request());
        if (during) during(*wait, *respond, out);
    }
    out.answer = asking.finish(40);
    out.ended = WaitClock::now();
    r.settle();
    if (respond) out.dueAfter = wait->due(respond->request());
    out.deaths = r.recorder.deaths();
    out.diags = r.recorder.diags();
    return out;
}

/// A hosted client with a SandboxHostedWait: a request whose arrival a published hold covers is due 3 s after that hold's release, so an answer 4 s after it is returned and the widget lives; one dropped inside the hold ends 3 to 4 s after the release, one dropped outside any hold (or arriving while an input exemption that outlasts the release is in force) 3 to 4 s after its arrival, each with `platform-timeout` and the death once. Only a hold the request actually meets extends it: one arriving in an exemption that the release outlasts is due 3 s after its arrival until the exemption ends, then (the hold resumed) 3 s after the release, so dropped it ends 3 to 4 s after the release; admitted during the exemption it was never held and keeps its arrival's deadline, also when the admission or a renewal (moving its check, then ending it with an exemption that outlasts the release) comes while its waiter sleeps until that check; renewed every 200 ms across the release it is never held and ends 3 to 3.25 s after its arrival (not 5 s); an exemption that lapses between renewals while the release lies 2 s ahead makes it due 3 s after that release, which the later renewals keep. While it waits for its admission a later release moves it to 3 s after that release, a repeated or earlier one does not; a release 25 s ahead is a finite deadline that its repeat leaves alone, and the admission then makes it due at most 3 s from then, which nothing moves again; an unheld request's admission never restarts its 3 s (admitted at 1 s or at 2.9 s, it ends 3 s after its arrival), and an extended one admitted at 4 s keeps its release + 3 s. The admission of an older request's number, that request's responder and a release whose deadline the clock cannot hold change nothing for the next one, and the client's destruction settles a held request at once.
TEST(hosted_client_platform_requests_wait_for_the_embedders_hold) {
    const WaitClock::duration answerTime = secondsOf(SandboxEngine::kPlatformAnswerSeconds);
    const std::vector<std::pair<std::string, std::string>> lateDiag = {{"platform-timeout", kPlatformLate}};
    WaitClock::time_point heldRelease, insideRelease, exemptRelease, exemptAdmittedRelease, renewedRelease, gapRelease, extendedRelease;
    Background<HeldOutcome> heldPast([&] {
        return heldRequest([&](SandboxHostedWait& w) { w.hold(heldRelease = WaitClock::now() + secondsOf(2)); },
                           [](SandboxHostedWait&, const HostedRequestResponder& respond, HeldOutcome& out) {
                               std::this_thread::sleep_until(out.asked + secondsOf(4));
                               respond("held answer");
                           });
    });
    Background<HeldOutcome> droppedInside([&] { return heldRequest([&](SandboxHostedWait& w) { w.hold(insideRelease = WaitClock::now() + secondsOf(1.5)); }, nullptr); });
    Background<HeldOutcome> droppedOutside([] { return heldRequest(nullptr, nullptr); });
    Background<HeldOutcome> exempted([&] {
        return heldRequest([&](SandboxHostedWait& w) {
            w.exempt(WaitClock::now() + secondsOf(1));
            w.hold(exemptRelease = WaitClock::now() + secondsOf(2));
        }, [](SandboxHostedWait& w, const HostedRequestResponder& respond, HeldOutcome& out) {
            std::this_thread::sleep_until(out.asked + secondsOf(1.2));
            out.dues.push_back(w.due(respond.request()));
        });
    });
    Background<HeldOutcome> renewed([&] {
        return heldRequest([&](SandboxHostedWait& w) {
            w.exempt(WaitClock::now() + secondsOf(0.5));
            w.hold(renewedRelease = WaitClock::now() + secondsOf(2));
        }, [](SandboxHostedWait& w, const HostedRequestResponder& respond, HeldOutcome& out) {
            for (WaitClock::time_point at = WaitClock::now(); at < out.asked + secondsOf(2.6); at += secondsOf(0.2)) {
                std::this_thread::sleep_until(at);
                w.exempt(WaitClock::now() + secondsOf(0.5));
                out.dues.push_back(w.due(respond.request()));
            }
        });
    });
    Background<HeldOutcome> lapsed([&] {
        return heldRequest([&](SandboxHostedWait& w) {
            w.exempt(WaitClock::now() + secondsOf(0.2));
            w.hold(gapRelease = WaitClock::now() + secondsOf(2.2));
        }, [](SandboxHostedWait& w, const HostedRequestResponder& respond, HeldOutcome& out) {
            std::this_thread::sleep_until(out.asked + secondsOf(0.5));
            out.dues.push_back(w.due(respond.request()));
            for (WaitClock::time_point at = WaitClock::now(); at < out.asked + secondsOf(2.6); at += secondsOf(0.2)) {
                std::this_thread::sleep_until(at);
                w.exempt(WaitClock::now() + secondsOf(0.5));
            }
            out.dues.push_back(w.due(respond.request()));
        });
    });
    Background<HeldOutcome> exemptAdmitted([&] {
        return heldRequest([&](SandboxHostedWait& w) {
            w.exempt(WaitClock::now() + secondsOf(1));
            w.hold(exemptAdmittedRelease = WaitClock::now() + secondsOf(2));
        }, [](SandboxHostedWait& w, const HostedRequestResponder& respond, HeldOutcome& out) {
            out.marks.push_back(WaitClock::now());
            w.admitted(respond.request());
            out.dues.push_back(w.due(respond.request()));
        });
    });
    Background<HeldOutcome> asleepAdmitted([] {
        return heldRequest([](SandboxHostedWait& w) {
            w.exempt(WaitClock::now() + secondsOf(1));
            w.hold(WaitClock::now() + secondsOf(2));
        }, [](SandboxHostedWait& w, const HostedRequestResponder& respond, HeldOutcome& out) {
            std::this_thread::sleep_until(out.asked + secondsOf(0.4));
            out.dues.push_back(w.due(respond.request()));
            w.admitted(respond.request());
            out.dues.push_back(w.due(respond.request()));
        });
    });
    Background<HeldOutcome> asleepRenewed([] {
        return heldRequest([](SandboxHostedWait& w) {
            w.exempt(WaitClock::now() + secondsOf(0.5));
            w.hold(WaitClock::now() + secondsOf(2));
        }, [](SandboxHostedWait& w, const HostedRequestResponder& respond, HeldOutcome& out) {
            std::this_thread::sleep_until(out.asked + secondsOf(0.3));
            w.exempt(WaitClock::now() + secondsOf(0.5));
            out.dues.push_back(w.due(respond.request()));
            std::this_thread::sleep_until(out.asked + secondsOf(0.6));
            w.exempt(WaitClock::now() + secondsOf(2));
            out.dues.push_back(w.due(respond.request()));
            std::this_thread::sleep_until(out.asked + secondsOf(1));
            w.admitted(respond.request());
            out.dues.push_back(w.due(respond.request()));
        });
    });
    Background<HeldOutcome> exemptOutlasting([] {
        return heldRequest([](SandboxHostedWait& w) {
            w.exempt(WaitClock::now() + secondsOf(3));
            w.hold(WaitClock::now() + secondsOf(2));
        }, nullptr);
    });
    Background<HeldOutcome> extended([] {
        return heldRequest(nullptr, [](SandboxHostedWait& w, const HostedRequestResponder& respond, HeldOutcome& out) {
            const WaitClock::time_point release = WaitClock::now() + secondsOf(3);
            out.marks.push_back(release);
            w.hold(release);
            out.dues.push_back(w.due(respond.request()));
            w.hold(release);
            out.dues.push_back(w.due(respond.request()));
            w.hold(release - secondsOf(1));
            w.hold(WaitClock::now() - secondsOf(1));
            out.dues.push_back(w.due(respond.request()));
            std::this_thread::sleep_until(out.asked + secondsOf(4.5));
            respond("extended answer");
        });
    });
    Background<HeldOutcome> farAndAdmitted([] {
        return heldRequest(nullptr, [](SandboxHostedWait& w, const HostedRequestResponder& respond, HeldOutcome& out) {
            const WaitClock::time_point release = WaitClock::now() + secondsOf(25);
            out.marks.push_back(release);
            w.hold(release);
            out.dues.push_back(w.due(respond.request()));
            w.hold(release);
            out.dues.push_back(w.due(respond.request()));
            std::this_thread::sleep_until(out.asked + secondsOf(0.5));
            out.marks.push_back(WaitClock::now());
            w.admitted(respond.request());
            out.dues.push_back(w.due(respond.request()));
            w.admitted(respond.request());
            w.hold(release + secondsOf(10));
            out.dues.push_back(w.due(respond.request()));
        });
    });
    Background<HeldOutcome> unheldAdmitted([] {
        return heldRequest(nullptr, [](SandboxHostedWait& w, const HostedRequestResponder& respond, HeldOutcome& out) {
            std::this_thread::sleep_until(out.asked + secondsOf(1));
            w.admitted(respond.request());
            out.dues.push_back(w.due(respond.request()));
        });
    });
    Background<HeldOutcome> unheldLate([] {
        return heldRequest(nullptr, [](SandboxHostedWait& w, const HostedRequestResponder& respond, HeldOutcome& out) {
            std::this_thread::sleep_until(out.asked + secondsOf(2.9));
            w.admitted(respond.request());
            out.dues.push_back(w.due(respond.request()));
        });
    });
    Background<HeldOutcome> extendedAdmitted([&] {
        return heldRequest([&](SandboxHostedWait& w) { w.hold(extendedRelease = WaitClock::now() + secondsOf(2)); },
                           [](SandboxHostedWait& w, const HostedRequestResponder& respond, HeldOutcome& out) {
                               std::this_thread::sleep_until(out.asked + secondsOf(4));
                               w.admitted(respond.request());
                               out.dues.push_back(w.due(respond.request()));
                           });
    });

    const HeldOutcome a = heldPast.finish(40);
    CHECK_MSG(a.answer == std::optional<std::string>("held answer") && a.deaths.empty() && a.diags.empty(), a.answer.value_or("none") + " / " + joined(a.deaths));
    CHECK_MSG(between(a.asked, a.ended) >= 4 && between(a.asked, a.ended) < 5, std::to_string(between(a.asked, a.ended)));
    CHECK(a.firstDue == std::optional<WaitClock::time_point>(heldRelease + answerTime) && !a.dueAfter);
    printf("    held past 3 s: answered %.2f s after the request\n", between(a.asked, a.ended));

    const HeldOutcome b = droppedInside.finish(40);
    CHECK(!b.answer && b.deaths == std::vector<std::string>{kPlatformLate} && b.diags == lateDiag);
    CHECK(b.firstDue == std::optional<WaitClock::time_point>(insideRelease + answerTime));
    CHECK_MSG(between(insideRelease, b.ended) >= SandboxEngine::kPlatformAnswerSeconds && between(insideRelease, b.ended) < SandboxEngine::kPlatformAnswerSeconds + 1,
              std::to_string(between(insideRelease, b.ended)) + " s after the release");

    const HeldOutcome c = droppedOutside.finish(40), d = exemptOutlasting.finish(40);
    for (const HeldOutcome* plain : {&c, &d}) {
        CHECK(!plain->answer && plain->deaths == std::vector<std::string>{kPlatformLate} && plain->diags == lateDiag);
        CHECK_MSG(between(plain->asked, plain->ended) >= SandboxEngine::kPlatformAnswerSeconds && between(plain->asked, plain->ended) < SandboxEngine::kPlatformAnswerSeconds + 1,
                  std::to_string(between(plain->asked, plain->ended)));
        CHECK_MSG(plain->firstDue && between(plain->asked, *plain->firstDue) >= SandboxEngine::kPlatformAnswerSeconds && between(plain->asked, *plain->firstDue) < SandboxEngine::kPlatformAnswerSeconds + 0.5,
                  plain->firstDue ? std::to_string(between(plain->asked, *plain->firstDue)) : "none");
    }

    // Due 3 s after its arrival (read before the delegate got it), within `slack` of it.
    const auto arrivalDue = [](const HeldOutcome& o, double slack) {
        const bool fromArrival = o.firstDue && between(o.asked, *o.firstDue) >= SandboxEngine::kPlatformAnswerSeconds && between(o.asked, *o.firstDue) < SandboxEngine::kPlatformAnswerSeconds + slack;
        CHECK_MSG(fromArrival, o.firstDue ? std::to_string(between(o.asked, *o.firstDue)) + " s after the request" : "none");
    };

    // Arrived while input exempted the widget, behind a release that outlasts the exemption: not held then, so due 3 s after its arrival; once the exemption ended the hold resumed, so 3 s after that release.
    const HeldOutcome x = exempted.finish(40);
    CHECK(!x.answer && x.deaths == std::vector<std::string>{kPlatformLate} && x.diags == lateDiag);
    arrivalDue(x, 0.25);
    CHECK(x.dues.size() == 1 && x.dues[0] == std::optional<WaitClock::time_point>(exemptRelease + answerTime));
    CHECK_MSG(between(exemptRelease, x.ended) >= SandboxEngine::kPlatformAnswerSeconds && between(exemptRelease, x.ended) < SandboxEngine::kPlatformAnswerSeconds + 1,
              std::to_string(between(exemptRelease, x.ended)) + " s after the release");

    // The same request admitted during the exemption: never held, so its admission keeps its arrival's deadline (no fresh 3 s).
    const HeldOutcome y = exemptAdmitted.finish(40);
    CHECK(!y.answer && y.deaths == std::vector<std::string>{kPlatformLate});
    arrivalDue(y, 0.25);
    CHECK(y.marks.size() == 1 && y.dues.size() == 1 && y.dues[0] == y.firstDue);
    CHECK_MSG(between(y.asked, y.ended) >= SandboxEngine::kPlatformAnswerSeconds && between(y.asked, y.ended) < SandboxEngine::kPlatformAnswerSeconds + 1,
              std::to_string(between(y.asked, y.ended)) + " s after the request");

    // Exempt without a gap across the release, renewed every 200 ms: never held, so it ends 3 to 3.25 s after its arrival, every reading its arrival's deadline.
    const HeldOutcome ra = renewed.finish(40);
    CHECK(!ra.answer && ra.deaths == std::vector<std::string>{kPlatformLate} && ra.diags == lateDiag);
    arrivalDue(ra, 0.25);
    CHECK_MSG(ra.dues.size() >= 10 && std::all_of(ra.dues.begin(), ra.dues.end(), [&](const auto& due) { return due == ra.firstDue; }), std::to_string(ra.dues.size()) + " readings");
    CHECK_MSG(between(ra.asked, ra.ended) >= SandboxEngine::kPlatformAnswerSeconds && between(ra.asked, ra.ended) < SandboxEngine::kPlatformAnswerSeconds + 0.25,
              std::to_string(between(ra.asked, ra.ended)) + " s after the request");
    CHECK(between(ra.asked, renewedRelease) > 1.5);
    printf("    exempt through a release 2 s ahead: ended %.3f s after the request\n", between(ra.asked, ra.ended));

    // The exemption lapsed between renewals with the release 2 s ahead: held from then, so due 3 s after that release, kept by the renewals after it.
    const HeldOutcome rb = lapsed.finish(40);
    CHECK(!rb.answer && rb.deaths == std::vector<std::string>{kPlatformLate} && rb.diags == lateDiag);
    arrivalDue(rb, 0.25);
    CHECK(rb.dues.size() == 2 && rb.dues[0] == std::optional<WaitClock::time_point>(gapRelease + answerTime) && rb.dues[1] == rb.dues[0]);
    CHECK_MSG(between(gapRelease, rb.ended) >= SandboxEngine::kPlatformAnswerSeconds && between(gapRelease, rb.ended) < SandboxEngine::kPlatformAnswerSeconds + 0.5,
              std::to_string(between(gapRelease, rb.ended)) + " s after the release");
    printf("    exemption lapsed 2 s before the release: ended %.3f s after the request, %.3f s after the release\n", between(rb.asked, rb.ended), between(gapRelease, rb.ended));

    // The waiter asleep until its check (the end of an exemption the release outlasts) when the admission or a renewal comes: each ends or moves that check while the waiter waits for it, the request was never held, so it keeps and freezes its arrival's deadline.
    const HeldOutcome sa = asleepAdmitted.finish(40), sr = asleepRenewed.finish(40);
    for (const auto& [asleep, readings] : {std::pair{&sa, size_t(2)}, std::pair{&sr, size_t(3)}}) {
        CHECK(!asleep->answer && asleep->deaths == std::vector<std::string>{kPlatformLate} && asleep->diags == lateDiag);
        arrivalDue(*asleep, 0.25);
        CHECK_MSG(asleep->dues.size() == readings && std::all_of(asleep->dues.begin(), asleep->dues.end(), [&](const auto& due) { return due == asleep->firstDue; }), std::to_string(asleep->dues.size()) + " readings");
        CHECK_MSG(between(asleep->asked, asleep->ended) >= SandboxEngine::kPlatformAnswerSeconds && between(asleep->asked, asleep->ended) < SandboxEngine::kPlatformAnswerSeconds + 0.25,
                  std::to_string(between(asleep->asked, asleep->ended)) + " s after the request");
    }
    printf("    admitted, renewed and admitted while asleep on the check: ended %.3f and %.3f s after the request\n", between(sa.asked, sa.ended), between(sr.asked, sr.ended));

    const HeldOutcome e = extended.finish(40);
    CHECK_MSG(e.answer == std::optional<std::string>("extended answer") && e.deaths.empty(), e.answer.value_or("none") + " / " + joined(e.deaths));
    CHECK(e.marks.size() == 1 && e.dues.size() == 3);
    if (e.marks.size() == 1 && e.dues.size() == 3) {
        const std::optional<WaitClock::time_point> moved = e.marks[0] + answerTime;
        CHECK(e.dues[0] == moved && e.dues[1] == moved && e.dues[2] == moved);
        CHECK(e.firstDue && *e.firstDue < *moved);
    }

    const HeldOutcome f = farAndAdmitted.finish(40);
    CHECK(!f.answer && f.deaths == std::vector<std::string>{kPlatformLate});
    CHECK(f.marks.size() == 2 && f.dues.size() == 4);
    if (f.marks.size() == 2 && f.dues.size() == 4) {
        CHECK(f.dues[0] == std::optional<WaitClock::time_point>(f.marks[0] + answerTime) && f.dues[1] == f.dues[0]);
        CHECK_MSG(f.dues[2] && between(f.marks[1] + answerTime, *f.dues[2]) >= 0 && between(f.marks[1] + answerTime, *f.dues[2]) < 0.1,
                  f.dues[2] ? std::to_string(between(f.marks[1] + answerTime, *f.dues[2])) : "none");
        CHECK(f.dues[3] == f.dues[2]);
        CHECK_MSG(between(f.marks[1], f.ended) >= SandboxEngine::kPlatformAnswerSeconds && between(f.marks[1], f.ended) < SandboxEngine::kPlatformAnswerSeconds + 1,
                  std::to_string(between(f.marks[1], f.ended)) + " s after the admission");
    }

    const HeldOutcome g = unheldAdmitted.finish(40);
    CHECK(!g.answer && g.deaths == std::vector<std::string>{kPlatformLate} && g.dues.size() == 1 && g.dues[0] == g.firstDue);
    CHECK_MSG(between(g.asked, g.ended) >= SandboxEngine::kPlatformAnswerSeconds && between(g.asked, g.ended) < SandboxEngine::kPlatformAnswerSeconds + 1, std::to_string(between(g.asked, g.ended)));

    // Admitted at 2.9 s, unheld: frozen at its arrival's deadline, so it ends 3 to 3.25 s after its arrival, never 3 s after the admission.
    const HeldOutcome h = unheldLate.finish(40);
    CHECK(!h.answer && h.deaths == std::vector<std::string>{kPlatformLate} && h.dues.size() == 1 && h.dues[0] == h.firstDue);
    arrivalDue(h, 0.25);
    CHECK_MSG(between(h.asked, h.ended) >= SandboxEngine::kPlatformAnswerSeconds && between(h.asked, h.ended) < SandboxEngine::kPlatformAnswerSeconds + 0.25, std::to_string(between(h.asked, h.ended)));

    // Held at its arrival (due 3 s after a release 2 s on), admitted at 4 s: frozen at that release + 3 s, sooner than 3 s after the admission.
    const HeldOutcome k = extendedAdmitted.finish(40);
    CHECK(!k.answer && k.deaths == std::vector<std::string>{kPlatformLate});
    CHECK(k.firstDue == std::optional<WaitClock::time_point>(extendedRelease + answerTime) && k.dues.size() == 1 && k.dues[0] == k.firstDue);
    CHECK_MSG(between(extendedRelease, k.ended) >= SandboxEngine::kPlatformAnswerSeconds && between(extendedRelease, k.ended) < SandboxEngine::kPlatformAnswerSeconds + 0.5,
              std::to_string(between(extendedRelease, k.ended)) + " s after the release");

    {
        // An older request's number and responder are stale for the next one.
        auto wait = std::make_shared<SandboxHostedWait>();
        HostedRun r(hostedConfig(), SandboxHostedStart(), true, wait);
        r.recorder.holdPlatformRequests = true;
        Background<std::optional<std::string>> first([&] { return r.sink().remotePlatformRequest("first"); });
        CHECK(eventually([&] { return r.recorder.platformResponder(0).has_value(); }));
        const std::optional<HostedRequestResponder> old = r.recorder.platformResponder(0);
        if (!old) return;
        CHECK(wait->due(old->request()).has_value());
        (*old)("first answer");
        CHECK(first.finish(5) == std::optional<std::string>("first answer"));
        CHECK(!wait->due(old->request()));
        Background<std::optional<std::string>> second([&] { return r.sink().remotePlatformRequest("second"); });
        CHECK(eventually([&] { return r.recorder.platformResponder(1).has_value(); }));
        const std::optional<HostedRequestResponder> next = r.recorder.platformResponder(1);
        if (!next) return;
        CHECK(next->request() > old->request());
        const std::optional<WaitClock::time_point> due = wait->due(next->request());
        wait->admitted(old->request());
        // No deadline the clock cannot hold either.
        wait->hold(WaitClock::time_point::max());
        CHECK(due && wait->due(next->request()) == due);
        (*old)("stale");
        CHECK(!second.within(0.2));
        (*next)("second answer");
        CHECK(second.finish(5) == std::optional<std::string>("second answer"));
        r.settle();
        CHECK(r.recorder.deaths().empty());

        // Held 25 s more, then the client goes: the request is settled at once, unanswered, and its responder does nothing.
        wait->hold(WaitClock::now() + secondsOf(25));
        Background<std::optional<std::string>> third([&] { return r.sink().remotePlatformRequest("third"); });
        CHECK(eventually([&] { return r.recorder.platformResponder(2).has_value(); }));
        const std::optional<HostedRequestResponder> last = r.recorder.platformResponder(2);
        const WaitClock::time_point gone = WaitClock::now();
        r.engine.reset();
        CHECK(!third.finish(5));
        CHECK_MSG(between(gone, WaitClock::now()) < 1, std::to_string(between(gone, WaitClock::now())));
        if (last) {
            CHECK(!wait->due(last->request()));
            (*last)("after the client");
        }
        CHECK(r.recorder.deaths().empty());
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

/// A hosted WidgetHost over a loopback whose engine's side builds a HostedTestRenderer (`configure` runs on it first); `renderer` is that renderer once it announced itself; with `wait` its platform requests are due as that record says.
struct HostedWidget {
    HostedTestRenderer* renderer = nullptr;
    std::unique_ptr<WidgetHost> host;
    explicit HostedWidget(const std::string& html, WidgetTheme theme = WidgetTheme{}, WidgetHostOptions options = WidgetHostOptions(), std::shared_ptr<IntlProvider> intl = nullptr,
                          std::function<void(HostedTestRenderer&)> configure = nullptr, std::shared_ptr<SandboxResources> resources = RES, std::shared_ptr<SandboxHostedWait> wait = nullptr)
        : host(std::make_unique<WidgetHost>(html, std::move(theme), 300, std::move(options),
                                            std::make_shared<SandboxHostedLoopbackChannel>(std::move(resources), hostedTest(renderer, std::move(configure))), std::move(intl),
                                            std::move(wait))) {}
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

/// The tap sees the batch a theme change re-sanitises the stylesheets into: after `setTheme` the engine's side's validator re-themes them and that batch reaches the hosted host's `onRenderOps` too.
TEST(hosted_widget_host_gets_the_rethemed_batch) {
    std::mutex m;
    std::vector<std::string> asked;
    HostedWidget w("<style>@media (prefers-color-scheme: dark){p{color:red}}</style><p>x</p>");
    w.host->onRenderOps = [&](const std::string& ops) {
        std::lock_guard<std::mutex> l(m);
        asked.push_back(ops);
    };
    CHECK(w.begin());
    CHECK(eventually([&] { return w.renderer->drew("x"); }));
    w.settle();
    size_t before = 0;
    {
        std::lock_guard<std::mutex> l(m);
        before = asked.size();
    }
    WidgetTheme dark;
    dark.colorScheme = "dark";
    w.host->setTheme(dark);
    CHECK(eventually([&] {
        std::lock_guard<std::mutex> l(m);
        return asked.size() > before && asked.back().find("min-width: 0px") != std::string::npos;
    }));
    const WidgetDiagnostics d = w.host->diagnostics();
    CHECK_MSG(d.countDiag("protocol-violation") == 0 && !d.dead, d.dead.value_or("alive"));
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

/// A hosted host decides every privileged call on the presses its embedder delivered: a tap the engine's side's renderer reports (and the engine's side allows) is refused without a delivered press and allowed once when the tap's own message carried it (`sendPlatformPress`), for links, copies, requests and haptics (outside the boot's series only within a delivered press and the 5 s after its release); the theme, data, safe area and visibility reach the engine's side's renderer and the widget; a CPU kill and a validator's `limit` freeze that renderer with `stoppedLabel`, and its diagnostics (a renderer's violation too) reach the host; `renderer()` throws, `event()` reaches no widget and `onGesture` never runs.
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
    // A press-framed tap of the engine's side's renderer, which the engine's side allows, its message carrying the delivered press or not; settled once the widget handled it.
    const auto tap = [&](bool delivered) {
        if (delivered) w.host->sendPlatformPress("tap");
        else w.host->sendPlatform("tap");
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

// ───────────── a kept renderer over the loopback (WidgetHost::keepRenderer) ─────────────

/// The platform traffic one pair of observers heard (a WidgetHost's or a kept renderer's), in order, under a lock: its messages, its requests (each answered "answer:<request>" but `hold`, whose `respond` is kept) and its ends, with every call counted.
struct HeardPlatform {
    std::mutex mutex;
    std::vector<std::string> messages;
    std::vector<std::string> requests;
    std::vector<std::string> ends;
    std::vector<std::function<void(std::string)>> held;
    /// Set before a request of it can come.
    std::string hold;
    int calls = 0;

    void message(const std::string& m) {
        std::lock_guard<std::mutex> l(mutex);
        calls++;
        messages.push_back(m);
    }
    void request(const std::string& m, std::function<void(std::string)> respond) {
        {
            std::lock_guard<std::mutex> l(mutex);
            calls++;
            requests.push_back(m);
            if (!hold.empty() && m == hold) return held.push_back(std::move(respond));
        }
        respond("answer:" + m);
    }
    void ended(const std::string& reason) {
        std::lock_guard<std::mutex> l(mutex);
        calls++;
        ends.push_back(reason);
    }
    /// `host`'s platform observers.
    void observe(WidgetHost& host) {
        host.onPlatformMessage = [this](const std::string& m) { message(m); };
        host.onPlatformRequest = [this](const std::string& m, std::function<void(std::string)> respond) { request(m, std::move(respond)); };
    }
    /// A kept renderer's observers.
    HostedKeptRenderer::Observers observers() {
        HostedKeptRenderer::Observers o;
        o.onPlatformMessage = [this](const std::string& m) { message(m); };
        o.onPlatformRequest = [this](const std::string& m, std::function<void(std::string)> respond) { request(m, std::move(respond)); };
        o.onEnded = [this](const std::string& reason) { ended(reason); };
        return o;
    }
    /// The messages that begin with `prefix`, in order.
    std::vector<std::string> with(const std::string& prefix) {
        std::lock_guard<std::mutex> l(mutex);
        std::vector<std::string> out;
        for (const std::string& m : messages)
            if (m.rfind(prefix, 0) == 0) out.push_back(m);
        return out;
    }
    bool heard(const std::string& m) {
        std::lock_guard<std::mutex> l(mutex);
        return std::find(messages.begin(), messages.end(), m) != messages.end();
    }
    std::optional<std::function<void(std::string)>> firstHeld() {
        std::lock_guard<std::mutex> l(mutex);
        if (held.empty()) return std::nullopt;
        return held.front();
    }
    int count() {
        std::lock_guard<std::mutex> l(mutex);
        return calls;
    }
    std::vector<std::string> endsSoFar() {
        std::lock_guard<std::mutex> l(mutex);
        return ends;
    }
};

#if defined(__APPLE__) || defined(__linux__)
/// `name` as this process's thread inventory lists it: Linux keeps a thread name's first 15 bytes.
static std::string listedThread(const std::string& name) {
#ifdef __linux__
    return name.substr(0, 15);
#else
    return name;
#endif
}
#endif

/// A renderer's `inventory` in process: this process's `widget.sandbox` and `widget.validator` threads as "<sandbox> <validator>", "unlisted" where the tests read no thread names.
static std::string engineThreads() {
#if defined(__APPLE__) || defined(__linux__)
    if (std::optional<std::map<std::string, int>> names = threadNames()) return std::to_string(countOf(*names, "widget.sandbox")) + " " + std::to_string(countOf(*names, listedThread("widget.validator")));
#endif
    return "unlisted";
}

/// The renderer's next `threads` report, asked through `send` (a WidgetHost's or a kept renderer's `sendPlatform`) and heard by `heard`; "" when none came within 5 s.
static std::string inventoryThrough(const std::function<void(std::string)>& send, HeardPlatform& heard) {
    const size_t before = heard.with("threads:").size();
    send("threads");
    std::string report;
    eventually([&] {
        const std::vector<std::string> all = heard.with("threads:");
        if (all.size() <= before) return false;
        report = all[before].substr(8);
        return true;
    });
    return report;
}

/// "<sandbox - 1> <validator - 1>" of an `engineThreads` report: one engine and one validator fewer; "" for a report without counts.
static std::string oneFewer(const std::string& report) {
    int sandbox = 0, validator = 0;
    if (std::sscanf(report.c_str(), "%d %d", &sandbox, &validator) != 2) return {};
    return std::to_string(sandbox - 1) + " " + std::to_string(validator - 1);
}

/// "b:0" .. "b:<n - 1>", what a renderer's `burst:<n>` sends.
static std::vector<std::string> burst(int n) {
    std::vector<std::string> out;
    for (int i = 0; i < n; i++) out.push_back("b:" + std::to_string(i));
    return out;
}

/// A widget that dies of its CPU limit soon after it loaded.
static const char* kDyingWidget = "<p>kept</p><script>setTimeout(function () { for (;;) {} }, 300);</script>";

/// Both kinds of kept renderer over the loopback: one its host stopped (`keepRenderer` on a live widget) and one dead of its own CPU limit (`keepsRenderer`): once kept and its WidgetHost destroyed, the handle's platform traffic goes both ways in order (20 messages each way), a request asked on the renderer's own thread and one asked on the glue's host thread are answered, the engine process holds one `widget.sandbox` and one `widget.validator` thread fewer than while the widget lived, the handle has not ended, and its `release()` destroys the renderer at once, with no observer call after it. Across the stopped one's keep nothing is lost or reordered: of a burst of 200 sent just before it the WidgetHost's observer hears a first part and the handle the rest, and a request waiting at the keep is answered after it. A twin destroyed without a keep loses its renderer at once, and a burst without a keep reaches the WidgetHost whole.
TEST(hosted_widget_host_keeps_a_stopped_and_a_dead_renderer) {
    {
        auto gone = std::make_shared<std::atomic<bool>>(false);
        HostedWidget twin("<p>kept</p>", WidgetTheme{}, WidgetHostOptions(), nullptr, [gone](HostedTestRenderer& r) { r.gone = gone; });
        CHECK(twin.begin());
        twin.host.reset();
        CHECK(*gone);
    }
    for (const bool dead : {false, true}) {
        const std::string kind = dead ? "dead: " : "stopped: ";
        auto gone = std::make_shared<std::atomic<bool>>(false);
        HostedWidget w(dead ? kDyingWidget : "<p>kept</p>", WidgetTheme{}, WidgetHostOptions(), nullptr, [gone](HostedTestRenderer& r) {
            r.gone = gone;
            r.inventory = engineThreads;
        });
        HeardPlatform before;
        before.hold = "edge";
        before.observe(*w.host);
        w.host->keepsRenderer = dead;
        w.host->start();
        // While the widget lives (before the dying one's timer runs): its engine and validator are there.
        const std::string live = inventoryThrough([&](std::string m) { w.host->sendPlatform(std::move(m)); }, before);
        CHECK_MSG(!live.empty(), kind + "no inventory");
        CHECK_MSG(w.host->waitLoaded(20) && w.host->waitRendered(5), kind + w.host->diagnostics().dead.value_or("not shown"));
        HeardPlatform after;
        after.hold = "edge";
        std::unique_ptr<HostedKeptRenderer> kept;
        if (dead) {
            CHECK(w.host->waitUntil(isDead, 10));
            const std::string reason = w.host->diagnostics().dead.value_or("");
            CHECK_MSG(reason.rfind("CPU limit exceeded", 0) == 0, reason);
            // Kept past its death: the WidgetHost still has its renderer's traffic.
            w.host->sendPlatform("say:past the death");
            CHECK(eventually([&] { return before.heard("past the death"); }));
            kept = w.host->keepRenderer(after.observers());
            CHECK_MSG(kept, kind + "nothing kept");
        } else {
            CHECK(!w.host->diagnostics().dead);
            w.host->sendPlatform("burst:200");
            CHECK(eventually([&] { return before.with("b:").size() == 200; }));
            CHECK_MSG(before.with("b:") == burst(200), kind + firstDifference(before.with("b:"), burst(200)));
            {
                std::lock_guard<std::mutex> l(before.mutex);
                before.messages.clear();
            }
            // The keep's boundary: a burst and a request on the glue's thread just before it.
            w.host->sendPlatform("burst:200");
            w.host->sendPlatform("wait:edge");
            kept = w.host->keepRenderer(after.observers());
            CHECK_MSG(kept, kind + "nothing kept");
            const std::vector<std::string> first = before.with("b:");
            const int beforeCalls = before.count();
            std::optional<std::function<void(std::string)>> respond;
            CHECK(eventually([&] {
                respond = before.firstHeld();
                if (!respond) respond = after.firstHeld();
                return respond.has_value();
            }));
            if (respond) (*respond)("late");
            CHECK(eventually([&] { return after.heard("waited:late"); }));
            CHECK(eventually([&] { return first.size() + after.with("b:").size() >= 200; }));
            std::vector<std::string> both = first;
            for (const std::string& b : after.with("b:")) both.push_back(b);
            CHECK_MSG(both == burst(200), kind + std::to_string(first.size()) + " before the keep; " + firstDifference(both, burst(200)));
            printf("    %sthe burst of 200 at the keep: %zu to the WidgetHost, %zu to the handle\n", kind.c_str(), first.size(), both.size() - first.size());
            CHECK_MSG(before.count() == beforeCalls, kind + "the WidgetHost's observers heard more after the keep");
        }
        if (!kept) continue;
        CHECK(w.host->diagnostics().dead.has_value());
        w.host.reset();
        std::vector<std::string> said;
        for (int i = 1; i <= 20; i++) {
            said.push_back(std::to_string(i));
            kept->sendPlatform("say:" + std::to_string(i));
        }
        const auto heardSaid = [&] {
            std::vector<std::string> out;
            for (const std::string& m : after.with(""))
                if (!m.empty() && std::isdigit((unsigned char)m[0])) out.push_back(m);
            return out;
        };
        CHECK(eventually([&] { return heardSaid().size() >= said.size(); }));
        CHECK_MSG(heardSaid() == said, kind + joined(heardSaid()));
        kept->sendPlatform("ask:on its thread");
        CHECK_MSG(eventually([&] { return after.heard("got:answer:on its thread"); }), kind + joined(after.with("")));
        kept->sendPlatform("wait:on the glue's");
        CHECK_MSG(eventually([&] { return after.heard("waited:answer:on the glue's"); }), kind + joined(after.with("")));
        if (live != "unlisted") {
            const std::string fewer = oneFewer(live);
            std::string now;
            CHECK_MSG(eventually([&] {
                now = inventoryThrough([&](std::string m) { kept->sendPlatform(std::move(m)); }, after);
                return now == fewer;
            }), kind + "live " + live + ", kept " + now);
            printf("    %swidget.sandbox and widget.validator threads: %s while live, %s while kept\n", kind.c_str(), live.c_str(), now.c_str());
        }
        CHECK(!kept->ended() && after.endsSoFar().empty());
        CHECK(!*gone);
        kept->release();
        CHECK_MSG(*gone, kind + "the renderer outlived the release");
        const int calls = after.count();
        kept->sendPlatform("say:after the release");
        sleepMs(100);
        CHECK(after.count() == calls && !kept->ended());
    }
}

/// Nothing is kept, and the widget ends as `destroy()` ends it, when there is nothing to keep: a local host (no hook, its renderer not a fixture's, so the hosted switch neither hosts nor counts it), a hosted host never started, one destroyed, one dead of its own without `keepsRenderer`, and, with `keepsRenderer`, one the host killed for a platform request left unanswered 3 s and one with no `onPlatformRequest`; each renderer goes with its WidgetHost.
TEST(hosted_widget_host_keeps_nothing_it_cannot_keep) {
    {
        WidgetHost::setHostedChannelFactory(nullptr);
        struct HookBack {
            ~HookBack() { restoreHostedFactory(); }
        } hookBack;
        WidgetHost local("<p>local</p>", WidgetTheme{}, 300, WidgetHostOptions(), RES,
                         [](std::shared_ptr<SandboxResources>, const std::string&, double) -> std::unique_ptr<Renderer> { return std::make_unique<ScriptedRenderer>(); });
        CHECK(!local.isHosted());
        local.start();
        CHECK(local.waitLoaded(20));
        CHECK(!local.keepRenderer(HostedKeptRenderer::Observers{}));
        CHECK(local.waitUntil(isDead, 5) && local.diagnostics().dead == std::optional<std::string>("terminated"));
        CHECK(local.probe("1") == "ERROR no runtime");
    }
    {
        HostedWidget unstarted("<p>x</p>");
        CHECK(!unstarted.host->keepRenderer(HostedKeptRenderer::Observers{}));
        unstarted.host->start();
        CHECK(unstarted.host->probe("1") == "ERROR no runtime");
    }
    const auto keptNothing = [](HostedWidget& w, const std::shared_ptr<std::atomic<bool>>& gone, const std::string& kind) {
        HeardPlatform after;
        CHECK_MSG(!w.host->keepRenderer(after.observers()), kind);
        CHECK_MSG(w.host->diagnostics().dead.has_value(), kind);
        w.host.reset();
        CHECK_MSG(*gone, kind + ": the renderer outlived its WidgetHost");
        CHECK_MSG(after.count() == 0, kind);
    };
    const auto withGone = [](const std::shared_ptr<std::atomic<bool>>& gone) { return [gone](HostedTestRenderer& r) { r.gone = gone; }; };
    {
        auto gone = std::make_shared<std::atomic<bool>>(false);
        HostedWidget destroyed("<p>x</p>", WidgetTheme{}, WidgetHostOptions(), nullptr, withGone(gone));
        CHECK(destroyed.begin());
        destroyed.host->destroy();
        CHECK(destroyed.host->waitUntil(isDead, 2) && destroyed.host->diagnostics().dead == std::optional<std::string>("terminated"));
        keptNothing(destroyed, gone, "destroyed");
    }
    {
        auto gone = std::make_shared<std::atomic<bool>>(false);
        HostedWidget dying(kDyingWidget, WidgetTheme{}, WidgetHostOptions(), nullptr, withGone(gone));
        CHECK(dying.begin());
        CHECK(dying.host->waitUntil(isDead, 10));
        CHECK_MSG(dying.host->diagnostics().dead.value_or("").rfind("CPU limit exceeded", 0) == 0, dying.host->diagnostics().dead.value_or("alive"));
        keptNothing(dying, gone, "dead without keepsRenderer");
    }
    {
        auto gone = std::make_shared<std::atomic<bool>>(false);
        HostedWidget unanswered("<p>x</p>", WidgetTheme{}, WidgetHostOptions(), nullptr, withGone(gone));
        HeardPlatform heard;
        heard.hold = "forever";
        heard.observe(*unanswered.host);
        unanswered.host->keepsRenderer = true;
        CHECK(unanswered.begin());
        unanswered.host->sendPlatform("ask:forever");
        CHECK(unanswered.host->waitUntil(isDead, 6));
        CHECK_MSG(unanswered.host->diagnostics().dead == std::optional<std::string>(kPlatformLate), unanswered.host->diagnostics().dead.value_or("alive"));
        keptNothing(unanswered, gone, "unanswered 3 s");
    }
    {
        auto gone = std::make_shared<std::atomic<bool>>(false);
        HostedWidget unobserved("<p>x</p>", WidgetTheme{}, WidgetHostOptions(), nullptr, withGone(gone));
        unobserved.host->keepsRenderer = true;
        CHECK(unobserved.begin());
        unobserved.host->sendPlatform("ask:anyone");
        CHECK(unobserved.host->waitUntil(isDead, 3));
        CHECK_MSG(unobserved.host->diagnostics().dead == std::optional<std::string>(kNoPlatform), unobserved.host->diagnostics().dead.value_or("alive"));
        keptNothing(unobserved, gone, "no onPlatformRequest");
    }
}

/// The platform deliveries an observer copies (`WidgetHost::platformDelivery`), in order under a lock: what each held and the message it delivered, and how many observer calls found none.
struct KeptDeliveries {
    std::mutex mutex;
    std::vector<HostedPlatformDelivery> deliveries;
    std::vector<std::string> messages;
    int empty = 0;

    void keep(const std::string& message) {
        const HostedPlatformDelivery delivery = WidgetHost::platformDelivery();
        std::lock_guard<std::mutex> l(mutex);
        if (!delivery.hold) empty++;
        deliveries.push_back(delivery);
        messages.push_back(message);
    }
    /// What the kept copies hold: their messages' bytes and their count.
    SandboxEngine::RemoteBacklog held() {
        std::lock_guard<std::mutex> l(mutex);
        SandboxEngine::RemoteBacklog out;
        for (const std::string& m : messages) out.bytes += m.size();
        out.calls = messages.size();
        return out;
    }
    void drop() {
        std::lock_guard<std::mutex> l(mutex);
        deliveries.clear();
        messages.clear();
    }
    bool heard(const std::string& m) {
        std::lock_guard<std::mutex> l(mutex);
        return std::find(messages.begin(), messages.end(), m) != messages.end();
    }
    size_t with(const std::string& prefix) {
        std::lock_guard<std::mutex> l(mutex);
        return size_t(std::count_if(messages.begin(), messages.end(), [&](const std::string& m) { return m.rfind(prefix, 0) == 0; }));
    }
    int unheld() {
        std::lock_guard<std::mutex> l(mutex);
        return empty;
    }
};

/// What a platform observer delivers stays counted while its copy lives (`WidgetHost::platformDelivery`), on a live WidgetHost and across its keep: empty outside an observer; inside one a message's hold, or a request's hold and its number, which names the request waiting in the client's SandboxHostedWait. Kept copies keep their messages counted against the client's bounds after the observers returned, exactly those bytes and calls; across the keep the handle's backlog (the same client's) counts what the live observer kept and what the handle's observers keep (each of their calls, the first ones, maybe made before `keepRenderer` returned, among them, with a hold of its own); dropped, it comes back to 0.
TEST(hosted_platform_deliveries_stay_counted_across_the_keep) {
    const HostedPlatformDelivery outside = WidgetHost::platformDelivery();
    CHECK(!outside.hold && outside.request == 0);
    KeptDeliveries live, kept;
    std::mutex m;
    uint64_t number = 0;
    bool waiting = false;
    auto wait = std::make_shared<SandboxHostedWait>();
    HostedWidget w("<p>x</p>", WidgetTheme{}, WidgetHostOptions(), nullptr, nullptr, RES, wait);
    w.host->onPlatformMessage = [&](const std::string& message) { live.keep(message); };
    w.host->onPlatformRequest = [&](const std::string& message, std::function<void(std::string)> respond) {
        const HostedPlatformDelivery delivery = WidgetHost::platformDelivery();
        {
            std::lock_guard<std::mutex> l(m);
            number = delivery.request;
            waiting = wait->due(delivery.request).has_value();
        }
        live.keep(message);
        respond("answer:" + message);
    };
    CHECK(w.begin());
    // The widget's own reports (its stats among them) pass through and go: in the end nothing is held.
    CHECK_MSG(eventually([&] { return backlogIs(*w.host, 0, 0); }), backlogText(*w.host));
    w.host->sendPlatform("say:one");
    w.host->sendPlatform("say:two");
    w.host->sendPlatform("ask:q");
    CHECK(eventually([&] { return live.heard("got:answer:q"); }));
    // "one", "two", the request "q" and "got:answer:q", each still counted by its copy.
    const SandboxEngine::RemoteBacklog held = live.held();
    CHECK_MSG(held.calls == 4 && eventually([&] { return backlogIs(*w.host, held.bytes, held.calls); }), backlogText(*w.host) + " for " + std::to_string(held.bytes) + " bytes kept");
    {
        std::lock_guard<std::mutex> l(m);
        CHECK(number > 0 && waiting);
    }
    live.drop();
    CHECK_MSG(eventually([&] { return backlogIs(*w.host, 0, 0); }), backlogText(*w.host));

    // A burst just before the keep: a first part to the WidgetHost's observer, the rest to the handle's.
    w.host->sendPlatform("say:before the keep");
    CHECK(eventually([&] { return live.heard("before the keep"); }));
    w.host->sendPlatform("burst:100");
    HostedKeptRenderer::Observers observers;
    observers.onPlatformMessage = [&](const std::string& message) { kept.keep(message); };
    std::unique_ptr<HostedKeptRenderer> handle = w.host->keepRenderer(std::move(observers));
    CHECK(handle);
    if (!handle) return;
    CHECK(eventually([&] { return live.with("b:") + kept.with("b:") == 100; }));
    const auto both = [&] {
        const SandboxEngine::RemoteBacklog a = live.held(), b = kept.held();
        return SandboxEngine::RemoteBacklog{a.bytes + b.bytes, a.calls + b.calls};
    };
    CHECK_MSG(eventually([&] { return backlogIs(*handle, both().bytes, both().calls); }), backlogText(*handle) + " for " + std::to_string(both().bytes) + " bytes kept");
    printf("    the burst of 100 at the keep: %zu to the WidgetHost, %zu to the handle\n", live.with("b:"), kept.with("b:"));
    w.host.reset();
    handle->sendPlatform("say:kept");
    CHECK(eventually([&] { return kept.heard("kept"); }));
    CHECK(live.unheld() == 0 && kept.unheld() == 0);
    CHECK_MSG(eventually([&] { return backlogIs(*handle, both().bytes, both().calls); }), backlogText(*handle));
    live.drop();
    CHECK_MSG(eventually([&] { return backlogIs(*handle, kept.held().bytes, kept.held().calls); }), backlogText(*handle));
    kept.drop();
    CHECK_MSG(eventually([&] { return backlogIs(*handle, 0, 0); }), backlogText(*handle));
    CHECK(!handle->ended());
    handle->release();
}

/// The deaths `onDeath` heard, under a lock: each reason, and whether every one came on the host thread, with `diagnostics().dead` already that reason and (`frozenYet`, when given) before the renderer got `frozen`.
struct HeardDeaths {
    std::mutex mutex;
    std::vector<std::string> reasons;
    bool fine = true;

    void watch(WidgetHost& host, std::function<bool()> frozenYet = nullptr) {
        std::thread::id thread;
        host.onHostThread([&] { thread = std::this_thread::get_id(); });
        host.onDeath = [this, &host, thread, frozenYet = std::move(frozenYet)](const std::string& reason) {
            const bool recorded = host.diagnostics().dead == std::optional<std::string>(reason);
            const bool frozen = frozenYet && frozenYet();
            std::lock_guard<std::mutex> l(mutex);
            reasons.push_back(reason);
            fine = fine && recorded && !frozen && std::this_thread::get_id() == thread;
        };
    }
    std::vector<std::string> all() {
        std::lock_guard<std::mutex> l(mutex);
        return reasons;
    }
    bool allFine() {
        std::lock_guard<std::mutex> l(mutex);
        return fine;
    }
};

/// `onDeath` hears each death once, on the host thread, once `diagnostics().dead` has it: a hosted widget's own limit, the client's own end of it (a platform request with no `onPlatformRequest`) and, with `keepsRenderer`, a death before the keep, whose renderer is then kept; never the end `destroy()` makes or the keep of a live widget. A local widget's own limit too, before its renderer gets `frozen`.
TEST(hosted_widget_host_tells_its_death_once) {
    {
        HeardDeaths heard;
        HostedWidget dying(kDyingWidget);
        heard.watch(*dying.host);
        CHECK(dying.begin());
        CHECK(dying.host->waitUntil(isDead, 10));
        dying.settle();
        const std::string reason = dying.host->diagnostics().dead.value_or("");
        CHECK_MSG(reason.rfind("CPU limit exceeded", 0) == 0, reason);
        CHECK_MSG(heard.all() == std::vector<std::string>{reason} && heard.allFine(), joined(heard.all()));
        dying.host->destroy();
        dying.settle();
        CHECK(heard.all().size() == 1);
    }
    {
        HeardDeaths heard;
        HostedWidget unobserved("<p>x</p>");
        heard.watch(*unobserved.host);
        CHECK(unobserved.begin());
        unobserved.host->sendPlatform("ask:anyone");
        CHECK(unobserved.host->waitUntil(isDead, 3));
        unobserved.settle();
        CHECK_MSG(heard.all() == std::vector<std::string>{kNoPlatform} && heard.allFine(), joined(heard.all()));
    }
    {
        HeardDeaths heard;
        HostedWidget destroyed("<p>x</p>");
        heard.watch(*destroyed.host);
        CHECK(destroyed.begin());
        destroyed.host->destroy();
        CHECK(destroyed.host->waitUntil(isDead, 2) && destroyed.host->diagnostics().dead == std::optional<std::string>("terminated"));
        destroyed.settle();
        CHECK_MSG(heard.all().empty(), joined(heard.all()));
    }
    for (const bool dead : {true, false}) {
        HeardDeaths heard;
        HostedWidget w(dead ? kDyingWidget : "<p>x</p>");
        heard.watch(*w.host);
        w.host->keepsRenderer = dead;
        CHECK(w.begin());
        if (dead) CHECK(w.host->waitUntil(isDead, 10));
        w.settle();
        const std::vector<std::string> before = heard.all();
        CHECK_MSG(before.size() == (dead ? 1u : 0u) && heard.allFine(), joined(before));
        HeardPlatform after;
        std::unique_ptr<HostedKeptRenderer> kept = w.host->keepRenderer(after.observers());
        CHECK(kept);
        CHECK(w.host->diagnostics().dead.has_value());
        w.host.reset();
        CHECK_MSG(heard.all() == before, joined(heard.all()));
        if (kept) kept->release();
    }
    {
        // A local widget: not a fixture's renderer, so the hosted switch leaves it in process.
        WidgetHost::setHostedChannelFactory(nullptr);
        struct HookBack {
            ~HookBack() { restoreHostedFactory(); }
        } hookBack;
        std::atomic<ScriptedRenderer*> renderer{nullptr};
        WidgetHost local(kDyingWidget, WidgetTheme{}, 300, WidgetHostOptions(), RES, [&](std::shared_ptr<SandboxResources>, const std::string&, double) -> std::unique_ptr<Renderer> {
            auto made = std::make_unique<ScriptedRenderer>();
            renderer = made.get();
            return made;
        });
        CHECK(!local.isHosted() && renderer.load());
        HeardDeaths heard;
        heard.watch(local, [&] { return !renderer.load()->sent("frozen").empty(); });
        local.start();
        CHECK(local.waitUntil(isDead, 10));
        settleHost(local);
        const std::string reason = local.diagnostics().dead.value_or("");
        CHECK_MSG(reason.rfind("CPU limit exceeded", 0) == 0, reason);
        CHECK_MSG(heard.all() == std::vector<std::string>{reason} && heard.allFine(), joined(heard.all()));
        CHECK(!renderer.load()->sent("frozen").empty());
    }
}

/// The embedder's own diagnostics of a hosted widget (`platformDiag`) land as a local renderer's do: in the diagnostics on the host thread, `onChange` after each, within `kMaxDiagKinds` kinds; nothing once the host was destroyed, and nothing on a local host.
TEST(hosted_widget_host_takes_the_embedders_diagnostics) {
    ObservedChanges changes;
    HostedWidget w("<p>x</p>");
    w.host->onChange = changes.observer();
    CHECK(w.begin());
    w.settle();
    const size_t seen = changes.all().size();
    w.host->platformDiag("paint-bounds", "a list past its bounds");
    w.settle();
    WidgetDiagnostics d = w.host->diagnostics();
    CHECK(d.countDiag("paint-bounds") == 1 && d.diags["paint-bounds"].second == std::vector<std::string>{"a list past its bounds"});
    CHECK(changes.all().size() > seen && changes.all().back().countDiag("paint-bounds") == 1);
    for (int i = 0; i < 80; i++) w.host->platformDiag("kind-" + std::to_string(i), "x");
    w.settle();
    d = w.host->diagnostics();
    CHECK_MSG(d.diags.size() <= WidgetHost::kMaxDiagKinds && d.countDiag("diag-kind-dropped") > 0, std::to_string(d.diags.size()) + " kinds");
    w.host->destroy();
    w.host->platformDiag("after", "the destroy");
    w.host->onHostThread([] {});
    CHECK(w.host->diagnostics().countDiag("after") == 0);

    WidgetHost::setHostedChannelFactory(nullptr);
    struct HookBack {
        ~HookBack() { restoreHostedFactory(); }
    } hookBack;
    WidgetHost local("<p>local</p>", WidgetTheme{}, 300, WidgetHostOptions(), RES,
                     [](std::shared_ptr<SandboxResources>, const std::string&, double) -> std::unique_ptr<Renderer> { return std::make_unique<ScriptedRenderer>(); });
    CHECK(!local.isHosted());
    local.platformDiag("paint-bounds", "local");
    local.onHostThread([] {});
    CHECK(local.diagnostics().countDiag("paint-bounds") == 0);
}

/// A hosted renderer may wait for its platform on the glue's host thread over the loopback: a request inside `onMessage` (`wait:`) and one inside the glue's call that hands it each batch (`askOnOps`) are answered while the widget, which makes layout queries, loads and renders; and the same `wait:` once the renderer is kept.
TEST(hosted_renderer_waits_on_the_glue_thread) {
    HostedWidget w(kWholeWidget, WidgetTheme{}, WidgetHostOptions(), nullptr, [](HostedTestRenderer& r) { r.askOnOps = true; });
    HeardPlatform heard;
    heard.observe(*w.host);
    CHECK(w.begin());
    HostedTestRenderer& r = *w.renderer;
    const std::vector<Json> queries = r.sent("query");
    CHECK(std::any_of(queries.begin(), queries.end(), [](const Json& q) { return q["qid"].asInt() > 0; }));
    CHECK(r.drew("hello"));
    const size_t batches = r.sent("ops").size();
    CHECK(eventually([&] { return heard.with("laid:").size() >= batches; }));
    const std::vector<std::string> laid = heard.with("laid:");
    for (size_t i = 0; i < laid.size(); i++) {
        const std::string n = std::to_string(i + 1);
        CHECK_MSG(laid[i] == "laid:" + n + ":answer:laid:" + n, laid[i]);
    }
    CHECK(!laid.empty());
    w.host->sendPlatform("wait:live");
    CHECK_MSG(eventually([&] { return heard.heard("waited:answer:live"); }), joined(heard.with("")));
    CHECK(!w.host->diagnostics().dead);
    HeardPlatform after;
    std::unique_ptr<HostedKeptRenderer> kept = w.host->keepRenderer(after.observers());
    CHECK(kept);
    if (!kept) return;
    w.host.reset();
    kept->sendPlatform("wait:kept");
    CHECK_MSG(eventually([&] { return after.heard("waited:answer:kept"); }), joined(after.with("")));
    CHECK(!kept->ended());
    kept->release();
}

/// When `pred` first held, polled for up to `seconds`; nullopt when it never did.
static std::optional<WaitClock::time_point> seenWithin(const std::function<bool()>& pred, double seconds) {
    const WaitClock::time_point until = WaitClock::now() + secondsOf(seconds);
    for (;;) {
        if (pred()) return WaitClock::now();
        if (WaitClock::now() >= until) return std::nullopt;
        sleepMs(5);
    }
}

/// How a served layout query went (`servedQuery`): the widths the widget logged and when the first came after the trigger, its `got` once settled ("" once dead), its death and when, whether the measure began while its glue waited inside a held platform request, and for a widget that lives the query time its stats charged (`SandboxStats::queryMs`, -1 until its query was counted).
struct ServedQuery {
    std::vector<std::string> widths;
    std::optional<double> widthAfter;
    std::string got;
    std::optional<std::string> dead;
    std::optional<double> deathAfter;
    bool measuredDuringWait = false;
    double queryMs = -1;
};

/// The served queries of `hosted_served_query_leaves_out_its_glues_platform_waits`: the glue asks its platform inside the query, at once as the query reaches it, and the host answers 4 s later (`RealWait`); the glue waits inside a held request when the widget's timer measures (`WaitingAtStart`); the glue is slow 3.5 s with no request (`SlowGlue`); the same while its renderer's own thread waits for a held request (`Unrelated`); the glue answers at once with no request (`NoWait`, the control).
enum class QueryCase { RealWait, WaitingAtStart, SlowGlue, Unrelated, NoWait };

/// One hosted widget over the loopback with a SandboxHostedWait whose renderer answers its layout queries with a 77 px rect, measured as `kind` says; the host holds the request the case names (the glue's `query:`, the glue's `held`, the renderer thread's `far`) under a published hold and answers every other at once.
static ServedQuery servedQuery(QueryCase kind) {
    ServedQuery out;
    std::mutex m;
    std::vector<std::function<void(std::string)>> held;
    const std::string heldPrefix = kind == QueryCase::RealWait ? "query:" : kind == QueryCase::WaitingAtStart ? "held" : "far";
    const auto heldOne = [&]() -> std::function<void(std::string)> {
        std::lock_guard<std::mutex> l(m);
        return held.empty() ? nullptr : held.front();
    };
    auto wait = std::make_shared<SandboxHostedWait>();
    HostedWidget w(kind == QueryCase::WaitingAtStart ? kMeasuredLater : kMeasuredOnTap, WidgetTheme{}, WidgetHostOptions(), nullptr,
                   [](HostedTestRenderer& r) { r.queryResult = kRect77; }, RES, wait);
    w.host->onPlatformRequest = [&](const std::string& message, std::function<void(std::string)> respond) {
        {
            std::lock_guard<std::mutex> l(m);
            if (message.rfind(heldPrefix, 0) == 0) return held.push_back(std::move(respond));
        }
        respond("answer:" + message);
    };
    if (!w.begin()) return out;
    const auto widths = [&] { return widthsLogged(w.host->diagnostics()); };
    WaitClock::time_point trigger = WaitClock::now();
    switch (kind) {
    case QueryCase::RealWait:
        w.renderer->askOnQuery = true;
        wait->hold(WaitClock::now() + secondsOf(2));
        trigger = WaitClock::now();
        w.host->sendPlatform("tap");
        seenWithin([&] { return heldOne() != nullptr; }, 5);
        std::this_thread::sleep_until(trigger + secondsOf(4));
        if (const auto respond = heldOne()) respond("measured");
        break;
    case QueryCase::WaitingAtStart: {
        wait->hold(WaitClock::now() + secondsOf(2.5));
        w.host->sendPlatform("wait:held");
        seenWithin([&] { return heldOne() != nullptr; }, 5);
        trigger = WaitClock::now();
        // `measuring` comes on the engine's own path, so it shows the measure began behind the glue's wait.
        out.measuredDuringWait = seenWithin([&] { return w.host->diagnostics().hasLog("measuring"); }, 4.5).has_value();
        std::this_thread::sleep_until(trigger + secondsOf(5));
        if (const auto respond = heldOne()) respond("released");
        break;
    }
    case QueryCase::Unrelated:
        wait->hold(WaitClock::now() + secondsOf(3));
        w.host->sendPlatform("ask:far");
        seenWithin([&] { return heldOne() != nullptr; }, 5);
        [[fallthrough]];
    case QueryCase::SlowGlue:
        w.renderer->queryStallMs = 3500;
        trigger = WaitClock::now();
        w.host->sendPlatform("tap");
        break;
    case QueryCase::NoWait:
        trigger = WaitClock::now();
        w.host->sendPlatform("tap");
        break;
    }
    const bool dies = kind == QueryCase::SlowGlue || kind == QueryCase::Unrelated;
    if (const std::optional<WaitClock::time_point> at = seenWithin([&] { return !widths().empty(); }, dies ? 4 : 10)) out.widthAfter = between(trigger, *at);
    if (dies) {
        if (const std::optional<WaitClock::time_point> at = seenWithin([&] { return w.host->diagnostics().dead.has_value(); }, 10)) out.deathAfter = between(trigger, *at);
    }
    w.settle();
    out.got = w.host->probe("got.join(',')");
    // A query whose time passed the budget stops the widget at its task's end, which comes before this.
    sleepMs(100);
    w.settle();
    out.widths = widths();
    out.dead = w.host->diagnostics().dead;
    // The stats after the measuring task, which the engine reports at its end (at most 200 ms later).
    if (!dies && seenWithin([&] { return w.host->diagnostics().stats.queries >= 1; }, 5)) out.queryMs = w.host->diagnostics().stats.queryMs;
    return out;
}

/// A served host's layout query gets 3 s of its glue's own time, its glue thread's waits inside its renderer's platform requests left out of them and of the widget's query budget: a query whose glue asks its platform as soon as the query reaches it and is answered 4 s later under a published hold gets its renderer's 77 px, and so does one that begins while the glue waits inside a held request (the measure's own log comes during that wait), each widget living on and charged no more query time than a query whose glue answers at once with no request (the control: well under the 1 s budget, as are they); a glue slow 3.5 s with no request of its own, alone or while its renderer's own thread waits for a held request, answers the query the timeout instead (no `w=77`, a width logged, if at all, before the glue's answer could come) and the widget stops at its query budget.
TEST(hosted_served_query_leaves_out_its_glues_platform_waits) {
    Background<ServedQuery> realWait([] { return servedQuery(QueryCase::RealWait); });
    Background<ServedQuery> waitingAtStart([] { return servedQuery(QueryCase::WaitingAtStart); });
    Background<ServedQuery> slowGlue([] { return servedQuery(QueryCase::SlowGlue); });
    Background<ServedQuery> unrelated([] { return servedQuery(QueryCase::Unrelated); });
    Background<ServedQuery> noWait([] { return servedQuery(QueryCase::NoWait); });
    const ServedQuery control = noWait.finish(40);
    CHECK_MSG(control.widths == std::vector<std::string>{"w=77"} && control.got == "77" && !control.dead,
              joined(control.widths) + " / " + control.got + " / " + control.dead.value_or("alive"));
    CHECK_MSG(control.queryMs >= 0 && control.queryMs < 500, std::to_string(control.queryMs));
    const ServedQuery a = realWait.finish(40);
    CHECK_MSG(a.widths == std::vector<std::string>{"w=77"} && a.got == "77" && !a.dead, joined(a.widths) + " / " + a.got + " / " + a.dead.value_or("alive"));
    CHECK_MSG(a.widthAfter && *a.widthAfter >= 4 && *a.widthAfter < 5, a.widthAfter ? std::to_string(*a.widthAfter) : "no width");
    const ServedQuery b = waitingAtStart.finish(40);
    CHECK_MSG(b.measuredDuringWait && b.widths == std::vector<std::string>{"w=77"} && b.got == "77" && !b.dead, joined(b.widths) + " / " + b.got + " / " + b.dead.value_or("alive"));
    for (const auto& [name, run] : std::vector<std::pair<std::string, ServedQuery>>{{"4 s platform wait", a}, {"begun while the glue waits", b}})
        CHECK_MSG(run.queryMs >= 0 && run.queryMs < 500 && run.queryMs <= control.queryMs + 250,
                  name + ": " + std::to_string(run.queryMs) + " ms charged, the control " + std::to_string(control.queryMs) + " ms");
    if (a.widthAfter) printf("    the query behind a 4 s platform wait: its width %.2f s after the tap\n", *a.widthAfter);
    printf("    query time charged: %.1f ms behind the 4 s platform wait, %.1f ms begun while the glue waits, %.1f ms with no wait\n", a.queryMs, b.queryMs, control.queryMs);
    for (const auto& [name, run] : std::vector<std::pair<std::string, ServedQuery>>{{"slow glue", slowGlue.finish(40)}, {"unrelated request", unrelated.finish(40)}}) {
        CHECK_MSG(run.dead && run.dead->rfind("Layout query limit exceeded: host queries blocked the renderer", 0) == 0, name + ": " + run.dead.value_or("alive"));
        CHECK_MSG(std::find(run.widths.begin(), run.widths.end(), "w=77") == run.widths.end(), name + ": " + joined(run.widths));
        CHECK_MSG(run.widths.size() <= 1 && (run.widths.empty() || (run.widthAfter && *run.widthAfter >= 3 && *run.widthAfter < 3.5)),
                  name + ": " + joined(run.widths) + (run.widthAfter ? " after " + std::to_string(*run.widthAfter) : ""));
        CHECK_MSG(run.deathAfter && *run.deathAfter >= 3, name + ": " + (run.deathAfter ? std::to_string(*run.deathAfter) : "no death"));
        printf("    %s: %s, dead %.2f s after the tap\n", name.c_str(), run.widths.empty() ? "no width logged" : (run.widths[0] + " " + std::to_string(run.widthAfter.value_or(-1)) + " s after the tap").c_str(),
               run.deathAfter.value_or(-1));
    }
}

/// A thread's request of a kept platform, the thread one with a joined thread's id (`requestFromRecycledId`): how many threads it took, whether one got that id, and that one's answer and how long it took.
struct RecycledRequest {
    int threads = 0;
    bool matched = false;
    std::optional<std::string> answer = "unset";
    double seconds = -1;
};

/// From threads with a JSThread's stack (as a glue's), one after another, at most 64, until one has `id`, a joined thread's id that the system hands out again: that thread's `platform.request`.
static RecycledRequest requestFromRecycledId(SandboxHostedPlatform& platform, std::thread::id id) {
    RecycledRequest out;
    while (out.threads < 64 && !out.matched) {
        out.threads++;
        JSThread thread("hosted.recycled");
        thread.sync([&] {
            if (std::this_thread::get_id() != id) return;
            out.matched = true;
            const auto at = std::chrono::steady_clock::now();
            out.answer = platform.request("recycled");
            out.seconds = secondsSince(at);
        });
    }
    return out;
}

/// A renderer may keep its platform past its widget and call it from any thread. Over a hosted loopback whose sink holds the served glue's own request (`wait:held`), the runtime closed and destroyed on a thread of its own: a request of another thread answers none at once instead of waiting for the held one's turn; the destruction waits for the held request alone, which still gets its answer; and from then on, in the renderer's own destructor once the glue thread was joined and after the runtime is gone, a request answers none at once, also from a thread that got the glue thread's id again, and never reaches the sink. Before all that a request of another thread is the sink's as usual.
TEST(hosted_kept_platform_reaches_no_host_after_close_and_join) {
    auto sink = std::make_shared<StreamSink>();
    Gate held;
    std::shared_ptr<SandboxHostedPlatform> kept;
    std::thread::id glue;
    RecycledRequest inDestructor;
    HostedTestRenderer* renderer = nullptr;
    auto channel = std::make_unique<SandboxHostedLoopbackChannel>(RES, hostedTest(renderer, [&](HostedTestRenderer& r) {
        kept = r.platform;
        r.inventory = [&glue] {
            glue = std::this_thread::get_id();
            return std::string("glue");
        };
        r.onDestroyed = [&] { inDestructor = requestFromRecycledId(*kept, glue); };
    }));
    channel->attach(sink);
    channel->startHosted("<p>x</p>", testConfig(), SandboxHostedStart{});
    CHECK(eventually([&] { return sink->saw("rendered"); }, 20));
    channel->platform("threads");
    CHECK(eventually([&] { return sink->saw("platform:threads:glue"); }));
    CHECK(kept && glue != std::thread::id());
    if (!kept || glue == std::thread::id()) return;
    CHECK(kept->request("unrelated") == std::optional<std::string>("ANSWER"));
    sink->platformGate = &held;
    channel->platform("wait:held");
    CHECK(held.waitedOn());
    Background<bool> destroying([&] {
        channel.reset();
        return true;
    });
    sleepMs(200);
    CHECK_MSG(!destroying.within(0), "the runtime was destroyed while its glue thread waited inside a request");
    Background<std::pair<std::optional<std::string>, double>> closedCall([&] {
        const auto at = std::chrono::steady_clock::now();
        std::optional<std::string> answer = kept->request("closed");
        return std::make_pair(answer, secondsSince(at));
    });
    const std::optional<std::pair<std::optional<std::string>, double>> closed = closedCall.within(1);
    CHECK_MSG(closed && !closed->first && closed->second < 0.5, closed ? closed->first.value_or("none") + " after " + std::to_string(closed->second) + " s" : "still waiting after 1 s");
    sleepMs(100);
    held.open();
    CHECK(destroying.finish(10));
    closedCall.finish(5);
    if (inDestructor.matched) {
        CHECK_MSG(!inDestructor.answer && inDestructor.seconds >= 0 && inDestructor.seconds < 0.5,
                  inDestructor.answer.value_or("none") + " after " + std::to_string(inDestructor.seconds) + " s");
    }
    const auto at = std::chrono::steady_clock::now();
    CHECK(!kept->request("after"));
    CHECK(secondsSince(at) < 0.5);
    const RecycledRequest after = requestFromRecycledId(*kept, glue);
    if (after.matched) CHECK_MSG(!after.answer && after.seconds >= 0 && after.seconds < 0.5, after.answer.value_or("none") + " after " + std::to_string(after.seconds) + " s");
    const std::vector<std::string> events = sink->events();
    const auto count = [&](const std::string& event) { return std::count(events.begin(), events.end(), event); };
    CHECK(count("platform-request:unrelated") == 1 && count("platform-request:held") == 1 && count("platform-request-done") == 2);
    for (const std::string& unreached : {"platform-request:closed", "platform-request:recycled", "platform-request:after", "platform:waited:ANSWER"})
        CHECK_MSG(count(unreached) == 0, unreached);
    printf("    the glue thread's id again: %s in the renderer's destructor (%d threads), %s after the runtime (%d threads)\n", inDestructor.matched ? "got" : "not got",
           inDestructor.threads, after.matched ? "got" : "not got", after.threads);
    kept.reset();
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

/// A probe whose task the engine's thread drops at its stop answers "ERROR no runtime" instead of waiting for good, as one in flight when the engine process stops a hosted widget's dead engine (`SandboxEngine::release`): an in-process engine's thread held on a gate, its stop queued behind the hold, then the probe's task behind the stop, then the gate opened.
TEST(hosted_probe_answers_when_its_engine_thread_stops_under_it) {
    Gate holding;
    SandboxEngine engine("<p>x</p>", testConfig(), RES);
    engine.thread().async([&] { holding.wait(); });
    CHECK(holding.waitedOn());
    engine.thread().stop();
    Background<std::string> probing([&] { return engine.probe("1"); });
    sleepMs(100);
    holding.open();
    CHECK_MSG(probing.within(1).has_value(), "the probe still waits 1 s after its engine's thread stopped");
    const std::string answer = probing.finish(5);
    CHECK_MSG(answer == "ERROR no runtime", answer);
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
