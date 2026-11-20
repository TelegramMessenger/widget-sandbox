// A hosting stream's tests (remote_stream.h's hosted kinds, `SandboxRemoteEngineEnd::serveHosting` and `SandboxRemoteHostEnd::startHosting`: the wire, the engine process's end and a hosted widget on it, the host's end with hosted WidgetHosts over real and lying ends, hostile frames, a flood, an unanswered request, a renderer's answer times and what the stream's end makes of each widget's death, renderers kept past their engines over real and lying ends, waits on the glue's thread, an engine process that reads no file, one that keeps only a kept renderer, a host process whose widgets are all hosted, threads the OS refuses to start), in the suite after the byte stream's other tests: `twidget-tests remote_stream_hosted_`.
#include "test_support.h"
#include "test_remote_support.h"
#include "test_remote_stream_support.h"

#include "../harness/stream_peers.h"
#include "twidget/bundle_stamp.h"
#include "twidget/json.h"
#include "twidget/remote_engine.h"
#include "twidget/remote_hosted.h"
#include "twidget/remote_stream.h"
#include "twidget/widget_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/resource.h>
#endif

// ───────────── the hosted mode on the engine process's end ─────────────

/// The HostedTestRenderers a hosting stream's engine end built, in the order its reader built them, and what its last factory call was given; each lives as long as its runtime, the factory as long as this.
class BuiltRenderers {
public:
    /// `configure` runs on each renderer before the factory returns it.
    HostedRendererFactory factory(std::function<void(HostedTestRenderer&)> configure = nullptr) {
        return [this, configure](std::shared_ptr<SandboxResources> resources, const std::string& theme, double width, std::shared_ptr<SandboxHostedPlatform> platform) -> std::unique_ptr<Renderer> {
            auto renderer = std::make_unique<HostedTestRenderer>(std::move(platform));
            if (configure) configure(*renderer);
            std::lock_guard<std::mutex> l(mutex_);
            built_.push_back(renderer.get());
            resources_ = std::move(resources);
            theme_ = theme;
            width_ = width;
            return renderer;
        };
    }
    size_t count() {
        std::lock_guard<std::mutex> l(mutex_);
        return built_.size();
    }
    /// The `i`th renderer built, null before it was.
    HostedTestRenderer* at(size_t i) {
        std::lock_guard<std::mutex> l(mutex_);
        return i < built_.size() ? built_[i] : nullptr;
    }
    std::shared_ptr<SandboxResources> resources() {
        std::lock_guard<std::mutex> l(mutex_);
        return resources_;
    }
    std::string theme() {
        std::lock_guard<std::mutex> l(mutex_);
        return theme_;
    }
    double width() {
        std::lock_guard<std::mutex> l(mutex_);
        return width_;
    }

private:
    std::mutex mutex_;
    std::vector<HostedTestRenderer*> built_;
    std::shared_ptr<SandboxResources> resources_;
    std::string theme_;
    double width_ = 0;
};

/// A hosted start that asks for the validated batches.
static SandboxHostedStart askingForOps() {
    SandboxHostedStart start;
    start.validatedOps = true;
    return start;
}

/// A hosted widget's start on `runtime`: `html` in `config`, with `start`'s theme, label and ask.
static std::string hostedStartFrame(uint32_t runtime, const std::string& html, const SandboxHostedStart& start = {}, const SandboxConfig& config = testConfig()) {
    return FrameBytes(runtime, wire::HostedStart).str(html).str(SandboxRemoteCoding::encodeConfig(config)).str(SandboxRemoteCoding::encodeHostedStart(start)).bytes();
}

/// One well-formed frame of each host kind an engine takes and a hosted widget makes inside the engine process.
static std::vector<std::pair<uint8_t, std::string>> engineOnlyFrames(uint32_t runtime) {
    return {
        {wire::Start, FrameBytes(runtime, wire::Start).str("<p>x</p>").str(SandboxRemoteCoding::encodeConfig(testConfig())).bytes()},
        {wire::Event, FrameBytes(runtime, wire::Event).str(kTapOnButton).bytes()},
        {wire::Activation, FrameBytes(runtime, wire::Activation).str(kTapOnButton).i64(1).bytes()},
        {wire::AnimationFrame, FrameBytes(runtime, wire::AnimationFrame).f64(16).bytes()},
        {wire::Viewport, FrameBytes(runtime, wire::Viewport).f64(300).f64(200).f64(2).bytes()},
        {wire::Resize, FrameBytes(runtime, wire::Resize).str("[]").bytes()},
        {wire::Theme, FrameBytes(runtime, wire::Theme).str("dark").str("{}").bytes()},
        {wire::PressBegan, FrameBytes(runtime, wire::PressBegan).bytes()},
        {wire::PressEnded, FrameBytes(runtime, wire::PressEnded).bytes()},
        {wire::ChargeValidator, FrameBytes(runtime, wire::ChargeValidator).f64(1).bytes()},
        {wire::QueryAnswer, FrameBytes(runtime, wire::QueryAnswer).str("{}").boolean(0).str("").bytes()},
    };
}

/// One well-formed frame of each host kind only a hosting stream takes.
static std::vector<std::pair<uint8_t, std::string>> hostedOnlyFrames(uint32_t runtime) {
    return {
        {wire::HostedStart, hostedStartFrame(runtime, "<p>x</p>")},
        {wire::HostedTheme, FrameBytes(runtime, wire::HostedTheme).str(SandboxRemoteCoding::encodeTheme(WidgetTheme{})).bytes()},
        {wire::Platform, FrameBytes(runtime, wire::Platform).str("say:x").bytes()},
        {wire::PlatformAnswer, FrameBytes(runtime, wire::PlatformAnswer).str("answer").bytes()},
    };
}

/// The hosting stream's kinds are the documented table's: the eleven numbers, the host's below 64; the engine's end serving a hosting stream still reads 0, 22, 63, 88 and 255 as unknown and the engine's new kinds as the other side's; `HostingOpening`'s fields read back by hand in their order, and a hosted widget's validator is built from its fourth (a bundle of another stamp is refused at its build).
TEST(remote_stream_hosted_wire_follows_the_documented_table) {
    using K = SandboxRemoteWire::Kind;
    const std::vector<std::pair<K, uint8_t>> kinds = {
        {K::HostingOpening, wire::HostingOpening}, {K::HostedStart, wire::HostedStart}, {K::HostedTheme, wire::HostedTheme}, {K::Platform, wire::Platform},
        {K::PlatformAnswer, wire::PlatformAnswer}, {K::PlatformMessage, wire::PlatformMessage}, {K::PlatformRequest, wire::PlatformRequest},
        {K::ValidatedOps, wire::ValidatedOps}, {K::RendererReady, wire::RendererReady}, {K::Rendered, wire::Rendered}, {K::Nodes, wire::Nodes},
    };
    CHECK(kinds.size() == 11);
    for (const auto& [kind, number] : kinds) CHECK_MSG(uint8_t(kind) == number, "kind " + std::to_string(number) + " is " + std::to_string(uint8_t(kind)));
    CHECK(std::count_if(kinds.begin(), kinds.end(), [](const auto& k) { return k.second < 64; }) == 5);

    std::vector<std::pair<std::string, std::string>> ends;
    for (const int kind : {0, 22, 63, 88, 255}) ends.push_back({FrameBytes(1, uint8_t(kind)).bytes(), "an unknown kind " + std::to_string(kind)});
    for (const int kind : {96, 97, 98, 99, 100, 101}) ends.push_back({FrameBytes(1, uint8_t(kind)).bytes(), "kind " + std::to_string(kind) + " from the other side"});
    BuiltRenderers built;
    for (const auto& [bytes, reason] : ends) {
        ScriptedHost host(nullptr, nullptr, built.factory());
        CHECK(host.send(smallHostingOpening()));
        CHECK_MSG(eventually([&] { return host.events(0) == std::vector<std::string>{"ready:1"}; }, 2), reason + ": " + joined(host.events(0)));
        CHECK(host.send(bytes));
        const std::optional<std::string> served = host.served(2);
        CHECK_MSG(served == reason, reason + ": " + served.value_or("still serving after 2 s"));
        CHECK_MSG(host.frameCount() == 1, reason + ": " + std::to_string(host.frameCount()) + " frames");
    }

    const std::string opening = hostingOpeningBytes(1, kBuild, std::string("sand\0box", 8), std::string("valid\1ator", 10), "stamp");
    const std::optional<ParsedFrame> frame = frameAt(opening, uint32_t(wire::little(std::string_view(opening).substr(0, 4))));
    CHECK(frame && frame->runtime == 0 && frame->kind == 32 && wire::little(std::string_view(opening).substr(0, 4)) == opening.size() - 4);
    if (frame) {
        FieldsOf in(*frame);
        CHECK(in.i64() == 1 && in.str() == kBuild && in.str() == std::string("sand\0box", 8) && in.str() == std::string("valid\1ator", 10) && in.str() == "stamp" && in.done());
    }

    const std::string other = BundleStamp{"9.9.9", 99, 1, 1}.line();
    const std::string refusal = BundleStamp::refusal(other, "validator-bundle.js").value_or("");
    const std::string restamped = other + RES->validatorBundle.substr(RES->validatorBundle.find('\n'));
    CHECK(!refusal.empty());
    struct Validator {
        const char* name;
        std::string opening;
        /// The diagnostic its build gives, "" for none.
        std::string diag;
        /// How the widget's start ends.
        std::string outcome;
    };
    const std::vector<Validator> validators = {
        {"another stamp", hostingOpeningOf(*RES, restamped), "diag:bundle-mismatch:" + refusal, "died:sandbox bundle failed to load"},
        {"the shipped validator", hostingOpeningOf(*RES), "", "rendered"},
    };
    for (const Validator& v : validators) {
        BuiltRenderers renderers;
        ScriptedHost host(nullptr, nullptr, renderers.factory());
        CHECK(host.send(v.opening + FrameBytes(1, wire::Open).bytes() + hostedStartFrame(1, "<p>checked</p>")));
        CHECK_MSG(eventually([&] { return (v.diag.empty() || !host.eventsWith(1, v.diag).empty()) && host.saw(1, v.outcome); }, 20), std::string(v.name) + ": " + joined(host.events(1)));
        const bool renders = v.outcome == "rendered", rendered = eventually([&] { return renderers.at(0) && renderers.at(0)->drew("checked"); }, renders ? 5 : 0.3);
        CHECK_MSG(rendered == renders, std::string(v.name) + (rendered ? ": rendered" : ": rendered nothing"));
        if (v.diag.empty()) CHECK_MSG(host.eventsWith(1, "diag:bundle-mismatch:").empty(), joined(host.events(1)));
        host.close();
        CHECK(host.served(5) == std::string());
    }
}

/// The engine's end opens a hosting stream as it opens a plain one: `HostingOpening` read whole within the opening's bound and the lockdown held between it and `Ready` (no Ready, no widget built, nothing booted until it returns; called once), its refusals by text and by exception, and the opening's own refusals (version, build, the renderer's stamp, a malformed frame) with no lockdown; plain `serve` refuses it as another first kind. Past the opening a hosting stream refuses either opening again, the engine's host kinds on any runtime and the hosted forms that do not decode, and drops what comes for a closed hosted runtime; an engine stream refuses the hosted kinds on `serve` and on `serveHosting`, which still serves a plain opening's engines.
TEST(remote_stream_hosted_engine_opening_and_lockdown) {
    using Wire = SandboxRemoteWire;
    BuiltRenderers built;
    const std::string opening = hostingOpeningOf(*RES);
    const std::optional<ParsedFrame> sent = frameAt(opening, uint32_t(opening.size() - 4));
    const std::string queued = opening + FrameBytes(1, wire::Open).bytes() + hostedStartFrame(1, "<script>console.log('ran')</script>");
    CHECK(opening.size() < Wire::kMaxOpeningBytes && opening.size() > RES->sandboxBundle.size() + RES->validatorBundle.size());
    {
        Gate held;
        std::atomic<int> calls{0};
        ScriptedHost host(nullptr, [&] {
            calls++;
            return held.wait() ? std::string() : std::string("the test never opened the gate");
        }, built.factory());
        CHECK(host.send(queued));
        CHECK(held.waitedOn());
        WatchedStream& engine = host.engineSide();
        const std::optional<ParsedFrame> first = engine.firstFrame();
        CHECK(sent && first && first->runtime == 0 && first->kind == wire::HostingOpening && first->fields == sent->fields);
        CHECK_MSG(engine.bytesRead() >= opening.size(), std::to_string(engine.bytesRead()) + " of " + std::to_string(opening.size()) + " bytes");
        CHECK(!host.served(0.3));
        CHECK_MSG(host.frameCount() == 0 && built.count() == 0, joined(host.events(0)) + " / " + joined(host.events(1)));
        held.open();
        CHECK_MSG(eventually([&] { return host.saw(1, "log:ran") && host.saw(1, "rendered"); }, 20), joined(host.events(1)));
        CHECK_MSG(host.events(0) == std::vector<std::string>{"ready:1"}, joined(host.events(0)));
        CHECK(built.count() == 1);
        host.close();
        CHECK(host.served(5) == std::string());
        CHECK(calls == 1);
    }

    std::atomic<int> calls{0};
    const auto refuses = [&] {
        calls++;
        return std::string("locked: no");
    };
    const auto throws = [&]() -> std::string {
        calls++;
        throw std::runtime_error("no sandbox here");
    };
    const std::vector<std::pair<std::function<std::string()>, std::string>> refusing = {{refuses, "locked: no"}, {throws, "the engine process could not lock itself down"}};
    for (const auto& [lockdown, reason] : refusing) {
        calls = 0;
        BuiltRenderers none;
        ScriptedHost host(nullptr, lockdown, none.factory());
        CHECK(host.send(queued));
        CHECK_MSG(host.served(5) == reason, reason);
        CHECK_MSG(eventually([&] { return host.events(0) == std::vector<std::string>{"refused:" + reason}; }), reason + ": " + joined(host.events(0)));
        CHECK_MSG(host.frameCount() == 1 && host.events(1).empty() && calls == 1 && none.count() == 0, reason + ": " + joined(host.events(1)));
    }

    const std::string longStamp(Wire::kMaxStampBytes + 1, 'r');
    struct Refused {
        const char* name;
        std::string bytes;
        std::string reason;
        bool hosting;
    };
    const std::vector<Refused> refused = {
        {"plain serve", opening, "kind 32 first, not the opening", false},
        {"another version", hostingOpeningBytes(2, kBuild, "s", "v", ""), "stream version 2, this end's is 1", true},
        {"another build", hostingOpeningBytes(1, "other", "s", "v", ""), "build \"other\", this end's is \"twidget-tests\"", true},
        {"another kind first", FrameBytes(1, wire::Open).bytes(), "kind 2 first, not the opening", true},
        {"the opening on a runtime", FrameBytes(3, wire::HostingOpening).i64(1).str(kBuild).str("s").str("v").str("").bytes(), "kind 32 on runtime 3, not the connection", true},
        {"a trailing byte", FrameBytes(0, wire::HostingOpening).i64(1).str(kBuild).str("s").str("v").str("").raw("x").bytes(), "kind 32 with bytes after its fields", true},
        {"a missing field", FrameBytes(0, wire::HostingOpening).i64(1).str(kBuild).str("s").str("v").bytes(), "kind 32 with a field past the frame's end", true},
        {"a size past the opening's bound, no body", wire::u32(uint32_t(Wire::kMaxOpeningBytes + 1)), "a frame of 16777217 bytes, past the bound of 16777216", true},
        {"a renderer stamp past its bound", hostingOpeningBytes(1, kBuild, "s", "v", longStamp), "a renderer stamp of 4097 bytes, past the bound of 4096", true},
        {"a renderer stamp of two lines", hostingOpeningBytes(1, kBuild, "s", "v", "r\nr"), "a renderer stamp of more than one line", true},
    };
    for (const Refused& v : refused) {
        calls = 0;
        ScriptedHost host(nullptr, [&] {
            calls++;
            return std::string();
        }, v.hosting ? built.factory() : nullptr);
        CHECK(host.send(v.bytes));
        const std::optional<std::string> reason = host.served(2);
        CHECK_MSG(reason == v.reason, std::string(v.name) + ": " + reason.value_or("still serving after 2 s"));
        CHECK_MSG(eventually([&] { return host.events(0) == std::vector<std::string>{"refused:" + v.reason}; }, 2), std::string(v.name) + ": " + joined(host.events(0)));
        CHECK_MSG(calls == 0, std::string(v.name) + ": called " + std::to_string(calls) + " times");
    }

    // A validator bundle is no stamp, so its first line may pass the stamps' bound; the renderer's stamp at its bound and none pass.
    for (const std::string& accepted : {hostingOpeningBytes(1, kBuild, "s", std::string(Wire::kMaxStampBytes + 1, 'v'), std::string(Wire::kMaxStampBytes, 'r')), smallHostingOpening()}) {
        calls = 0;
        ScriptedHost host(nullptr, [&] {
            calls++;
            return std::string();
        }, built.factory());
        CHECK(host.send(accepted));
        CHECK_MSG(eventually([&] { return host.events(0) == std::vector<std::string>{"ready:1"}; }, 2), joined(host.events(0)));
        host.close();
        CHECK(host.served(2) == std::string() && calls == 1);
    }

    const std::string open1 = FrameBytes(1, wire::Open).bytes(), close1 = FrameBytes(1, wire::Close).bytes();
    const std::string config = SandboxRemoteCoding::encodeConfig(testConfig()), hosted = SandboxRemoteCoding::encodeHostedStart(SandboxHostedStart{});
    std::string dropped = open1 + close1;
    for (const auto& [kind, bytes] : hostedOnlyFrames(1)) dropped += bytes;
    dropped += FrameBytes(1, wire::SafeArea).str(SandboxRemoteCoding::encodeSafeArea(WidgetContentSafeArea{})).bytes() +
               FrameBytes(1, wire::Data).str(SandboxRemoteCoding::encodeData(WidgetData{})).bytes() + FrameBytes(1, wire::Visible).boolean(1).bytes() +
               FrameBytes(1, wire::Stop).str("why").bytes() + FrameBytes(1, wire::Answer).i64(1).str(SandboxRemoteCoding::encodeOutcome(SandboxRemoteOutcome{})).bytes() +
               FrameBytes(1, wire::Probe).i64(1).str("1").bytes() + FrameBytes(1, wire::IntlAnswer).str("late").bytes();
    struct After {
        std::string name;
        std::string bytes;
        bool end;
        std::string reason;
    };
    std::vector<After> after = {
        {"a second hosting opening", smallHostingOpening(), false, "kind 32 after the opening"},
        {"a plain opening", smallOpening(), false, "kind 1 after the opening"},
        {"an engine kind on a runtime never opened", FrameBytes(5, wire::Event).str("{}").bytes(), false, "kind 4 on runtime 5, which a hosting stream does not take"},
        {"an engine kind on a closed runtime", open1 + close1 + FrameBytes(1, wire::Event).str("{}").bytes(), false, "kind 4 on runtime 1, which a hosting stream does not take"},
        {"a hosted kind on a runtime never opened", FrameBytes(5, wire::Platform).str("x").bytes(), false, "kind 35 on runtime 5, which was never opened"},
        {"a config that does not decode", open1 + FrameBytes(1, wire::HostedStart).str("<p>x</p>").str("{}").str(hosted).bytes(), false, "kind 33 on runtime 1 with a form that does not decode"},
        {"a hosted start that does not decode", open1 + FrameBytes(1, wire::HostedStart).str("<p>x</p>").str(config).str("{}").bytes(), false, "kind 33 on runtime 1 with a form that does not decode"},
        {"a theme that does not decode", open1 + FrameBytes(1, wire::HostedTheme).str("{\"params\":{}}").bytes(), false, "kind 34 on runtime 1 with a form that does not decode"},
        {"a safe area that does not decode", open1 + FrameBytes(1, wire::SafeArea).str("[]").bytes(), false, "kind 10 on runtime 1 with a form that does not decode"},
        {"data that does not decode", open1 + FrameBytes(1, wire::Data).str("null").bytes(), false, "kind 11 on runtime 1 with a form that does not decode"},
        {"an outcome that does not decode", open1 + FrameBytes(1, wire::Answer).i64(1).str("{}").bytes(), false, "kind 17 on runtime 1 with a form that does not decode"},
        {"a hosted start with a field missing", open1 + FrameBytes(1, wire::HostedStart).str("<p>x</p>").str(config).bytes(), false, "kind 33 with a field past the frame's end"},
        {"a platform message with a byte after it", open1 + FrameBytes(1, wire::Platform).str("x").raw("y").bytes(), false, "kind 35 with bytes after its fields"},
        {"a closed hosted runtime's frames", dropped, true, ""},
    };
    for (const auto& [kind, bytes] : engineOnlyFrames(1)) after.push_back({"engine kind " + std::to_string(kind), open1 + bytes, false, "kind " + std::to_string(kind) + " on runtime 1, which a hosting stream does not take"});
    for (const After& v : after) {
        ScriptedHost host(nullptr, nullptr, built.factory());
        CHECK(host.send(smallHostingOpening()));
        CHECK_MSG(eventually([&] { return host.events(0) == std::vector<std::string>{"ready:1"}; }, 2), v.name + ": " + joined(host.events(0)));
        CHECK(host.send(v.bytes));
        if (v.end) host.close();
        const std::optional<std::string> reason = host.served(2);
        CHECK_MSG(reason == v.reason, v.name + ": " + reason.value_or("still serving after 2 s"));
        CHECK_MSG(host.frameCount() == 1, v.name + ": " + std::to_string(host.frameCount()) + " frames");
    }
    CHECK(built.count() == 1);

    for (const bool hostingEnd : {false, true}) {
        for (const auto& [kind, bytes] : hostedOnlyFrames(1)) {
            const std::string reason = "kind " + std::to_string(kind) + " on runtime 1, which only a hosting stream takes";
            ScriptedHost host(nullptr, nullptr, hostingEnd ? built.factory() : nullptr);
            CHECK(host.send(smallOpening()));
            CHECK_MSG(eventually([&] { return host.events(0) == std::vector<std::string>{"ready:1"}; }, 2), reason + ": " + joined(host.events(0)));
            CHECK(host.send(open1 + bytes));
            const std::optional<std::string> served = host.served(2);
            CHECK_MSG(served == reason, std::string(hostingEnd ? "serveHosting: " : "serve: ") + served.value_or("still serving after 2 s"));
            CHECK_MSG(host.frameCount() == 1, reason + ": " + std::to_string(host.frameCount()) + " frames");
        }
    }

    // serveHosting serves a plain opening's engines as serve does, its lockdown at the same point, and builds no renderer.
    calls = 0;
    BuiltRenderers none;
    ScriptedHost host(layoutAnswers(), [&] {
        calls++;
        return std::string();
    }, none.factory());
    CHECK(host.send(openingOf(*RES) + open1 + FrameBytes(1, wire::Start).str(kQueriesB).str(config).bytes()));
    CHECK_MSG(eventually([&] { return host.saw(1, "log:B 77 INTL-A") && host.saw(1, "loaded"); }, 20), joined(host.events(1)));
    CHECK_MSG(host.events(0) == std::vector<std::string>{"ready:1"} && calls == 1 && none.count() == 0, joined(host.events(0)));
    host.close();
    CHECK(host.served(5) == std::string());
}

/// What a hosted widget does at boot and on a click, all of it inside the engine process: it draws, measures, listens, animates, asks for a frame and formats a number (an Intl request), and logs that with the safe area it was given before its start; a click prevents its default and formats another number.
static const char* kHostedWidget =
    "<button id='b'>go</button><p id='p'>hosted</p><script>var w = document.getElementById('p').getBoundingClientRect().width, n = (1234.5).toLocaleString();"
    "document.getElementById('b').addEventListener('click', function (e) { e.preventDefault(); console.log('m=' + (5678.5).toLocaleString()); });"
    "document.getElementById('p').animate([{ opacity: 0 }, { opacity: 1 }], { duration: 1000 }); requestAnimationFrame(function () {});"
    "console.log('n=' + n + ' ' + TelegramWidget.contentSafeAreaInset.bottom);</script>";

/// testConfig with Web Animations, so the widget's `animate` reaches its renderer.
static SandboxConfig animatedConfig() {
    SandboxConfig c = testConfig();
    c.features = {"webAnimations"};
    return c;
}

/// The frames of `runtime` that carry render traffic (ops, layout queries, listen, preventDefault, anim, frame requests), as `describe` gives them.
static std::vector<std::string> renderTraffic(ScriptedHost& host, uint32_t runtime) {
    std::vector<std::string> out;
    for (const std::string& e : host.events(runtime))
        if (e.rfind("query:", 0) == 0 || e == "kind 68" || e == "kind 71" || e == "kind 72" || e == "kind 73" || e == "kind 74") out.push_back(e);
    return out;
}

/// The engine's end serves a hosted widget whole: from `Open` and `HostedStart` (the safe area and data kept before it) its engine boots from the opening's sandbox bundle, compiled, its renderer (built from the opening's resources, the scheme and the width) announces itself, and the host hears `RendererReady`, then `Loaded`, then `Rendered`, its node counts and the batches it asked for, while every op, layout query, listener, preventDefault, animation and frame request stays with the engine end's renderer (control: the same widget as an engine on the same end sends them as frames). Platform messages cross both ways in order; the renderer's request is a `PlatformRequest` answered by a hand-written `PlatformAnswer`, a second one from another thread waits in the renderer (never a second frame), and the engine's Intl request and a platform request wait at once, each for its own answer. A second runtime that did not ask gets no batch, takes its theme, data and visibility and freezes with its label at a stop; `Close` makes a waiting request return nullopt and sends nothing more for its runtime. A platform message or request past its bound goes out cut to it and one byte more.
TEST(remote_stream_hosted_engine_end_serves_a_widget) {
    using Asked = Background<std::optional<std::string>>;
    // Before the host, so a request that a failed check left waiting ends with the host before its worker is finished.
    std::optional<Asked> first, second, both, never;
    std::atomic<bool> holdIntl{false};
    BuiltRenderers built;
    ScriptedHost host([&](const ParsedFrame& f) -> std::optional<std::string> {
        if (f.kind == wire::Intl && !holdIntl) return FrameBytes(f.runtime, wire::IntlAnswer).str("INTL-H").bytes();
        return std::nullopt;
    }, nullptr, built.factory());
    const std::string kept = FrameBytes(1, wire::SafeArea).str(SandboxRemoteCoding::encodeSafeArea(WidgetContentSafeArea{0, 0, 24, 0, 0, 0})).bytes() +
                             FrameBytes(1, wire::Data).str(SandboxRemoteCoding::encodeData(WidgetData{std::string("chat-h"), std::nullopt})).bytes();
    CHECK(host.send(hostingOpeningOf(*RES) + FrameBytes(1, wire::Open).bytes() + kept + hostedStartFrame(1, kHostedWidget, askingForOps(), animatedConfig())));
    CHECK_MSG(eventually([&] { return host.saw(1, "rendered") && host.saw(1, "log:n=INTL-H 24"); }, 20), joined(host.events(1)));
    HostedTestRenderer* r = built.at(0);
    CHECK(r && built.count() == 1);
    if (!r) return;

    const std::vector<std::string> events = host.events(1);
    const auto at = [&](const std::string& e) { return std::find(events.begin(), events.end(), e) - events.begin(); };
    CHECK_MSG(at("renderer-ready") < at("loaded") && at("loaded") < at("rendered") && at("rendered") < ptrdiff_t(events.size()), joined(events));
    const std::vector<std::string> nodes = host.eventsWith(1, "nodes:");
    CHECK_MSG(!nodes.empty() && std::atol(nodes.back().c_str() + 6) > 0, joined(nodes));
    CHECK(!host.eventsWith(1, "validated-ops").empty());
    const std::vector<std::string> boots = host.eventsWith(1, "diag:boot:sandbox bundle eval ");
    CHECK_MSG(boots.size() == 1 && boots[0].size() > 10 && boots[0].compare(boots[0].size() - 10, 10, "(compiled)") == 0, joined(boots));
    CHECK_MSG(host.eventsWith(1, "diag:protocol-violation:").empty(), joined(events));
    const std::shared_ptr<SandboxResources> res = built.resources();
    CHECK(res && res->sandboxBundle == RES->sandboxBundle && res->validatorBundle == RES->validatorBundle && res->rendererBundle == firstLine(RES->rendererBundle));
    CHECK(built.theme() == "light" && built.width() == 400);
    for (const char* type : {"ops", "query", "listen", "anim", "requestFrame"}) CHECK_MSG(!r->sent(type).empty(), type);
    CHECK(r->drew("hosted"));
    {
        std::lock_guard<std::mutex> l(r->mutex);
        CHECK_MSG(r->messages.size() > 2 && r->messages[0].rfind("{\"type\":\"theme\"", 0) == 0 && r->messages[1].find("\"bottom\":24") != std::string::npos,
                  r->messages.size() > 1 ? r->messages[1] : "");
    }
    CHECK_MSG(eventually([&] { return host.probe(1, "n + ' ' + TelegramWidget.chatData") == "INTL-H chat-h"; }), host.probe(1, "TelegramWidget.chatData").value_or("no answer"));

    CHECK(host.send(FrameBytes(1, wire::Platform).str("say:one").bytes() + FrameBytes(1, wire::Platform).str("say:two").bytes() + FrameBytes(1, wire::Platform).str("say:three").bytes()));
    const std::vector<std::string> said = {"platform:one", "platform:two", "platform:three"};
    CHECK_MSG(eventually([&] { return host.eventsWith(1, "platform:") == said; }), joined(host.eventsWith(1, "platform:")));
    CHECK(r->platformMessages() == (std::vector<std::string>{"say:one", "say:two", "say:three"}));

    const auto requests = [&] { return host.eventsWith(1, "platform-request:"); };
    CHECK(host.send(FrameBytes(1, wire::Platform).str("ask:q1").bytes()));
    CHECK_MSG(eventually([&] { return requests() == std::vector<std::string>{"platform-request:q1"}; }), joined(requests()));
    CHECK(host.send(FrameBytes(1, wire::PlatformAnswer).str("a1").bytes()));
    CHECK_MSG(eventually([&] { return host.saw(1, "platform:got:a1"); }), joined(host.eventsWith(1, "platform:")));

    // Two threads of the renderer's ask at once: one frame goes out, and the other request waits in the renderer until the first is answered.
    const std::shared_ptr<SandboxHostedPlatform> platform = r->platform;
    first.emplace([platform] { return platform->request("one"); });
    second.emplace([platform] { return platform->request("two"); });
    CHECK_MSG(eventually([&] { return requests().size() == 2; }), joined(requests()));
    sleepMs(300);
    CHECK_MSG(requests().size() == 2, joined(requests()));
    for (size_t answered = 0; answered < 2; answered++) {
        const std::vector<std::string> asked = requests();
        CHECK_MSG(asked.size() == 2 + answered, joined(asked));
        if (asked.size() != 2 + answered) break;
        CHECK(host.send(FrameBytes(1, wire::PlatformAnswer).str(asked.back().substr(17) + "!").bytes()));
        if (!answered) CHECK_MSG(eventually([&] { return requests().size() == 3; }), joined(requests()));
    }
    CHECK(first->within(5) == std::optional<std::string>("one!"));
    CHECK(second->within(5) == std::optional<std::string>("two!"));

    // The engine's Intl request (a click's) and a platform request wait at the same time; each gets its own answer, the platform's first.
    holdIntl = true;
    CHECK(host.send(FrameBytes(1, wire::Platform).str("untrusted-tap").bytes()));
    CHECK_MSG(eventually([&] { return host.eventsWith(1, "intl:").size() == 2; }), joined(host.events(1)));
    both.emplace([platform] { return platform->request("both"); });
    CHECK_MSG(eventually([&] { return host.saw(1, "platform-request:both"); }), joined(requests()));
    CHECK(host.send(FrameBytes(1, wire::PlatformAnswer).str("platform first").bytes()));
    CHECK(both->within(5) == std::optional<std::string>("platform first"));
    CHECK_MSG(host.eventsWith(1, "log:m=").empty(), joined(host.eventsWith(1, "log:")));
    CHECK(host.send(FrameBytes(1, wire::IntlAnswer).str("INTL-2").bytes()));
    CHECK_MSG(eventually([&] { return host.saw(1, "log:m=INTL-2"); }), joined(host.eventsWith(1, "log:")));
    holdIntl = false;
    CHECK_MSG(eventually([&] { return !r->sent("preventDefault").empty(); }), "no preventDefault");
    CHECK_MSG(renderTraffic(host, 1).empty(), joined(renderTraffic(host, 1)));

    // A second runtime that did not ask for the batches.
    SandboxHostedStart labelled;
    labelled.stoppedLabel = "Stopped here";
    CHECK(host.send(FrameBytes(2, wire::Open).bytes() + hostedStartFrame(2, "<p id='q'>quiet</p>", labelled)));
    CHECK_MSG(eventually([&] { return host.saw(2, "rendered"); }, 20), joined(host.events(2)));
    HostedTestRenderer* quiet = built.at(1);
    CHECK(quiet && quiet->drew("quiet"));
    CHECK_MSG(host.eventsWith(2, "validated-ops").empty() && !host.eventsWith(2, "nodes:").empty(), joined(host.events(2)));
    WidgetTheme dark;
    dark.colorScheme = "dark";
    dark.params = {{"bg_color", "#112233"}};
    CHECK(host.send(FrameBytes(2, wire::HostedTheme).str(SandboxRemoteCoding::encodeTheme(dark)).bytes() +
                    FrameBytes(2, wire::Data).str(SandboxRemoteCoding::encodeData(WidgetData{std::string("chat-q"), std::nullopt})).bytes() + FrameBytes(2, wire::Visible).boolean(0).bytes()));
    const std::string state = "TelegramWidget.colorScheme + ' ' + TelegramWidget.themeParams.bg_color + ' ' + TelegramWidget.chatData + ' ' + document.visibilityState";
    CHECK_MSG(eventually([&] { return host.probe(2, state) == "dark #112233 chat-q hidden"; }), host.probe(2, state).value_or("no answer"));
    if (quiet) {
        std::lock_guard<std::mutex> l(quiet->mutex);
        CHECK_MSG(std::find(quiet->themes.begin(), quiet->themes.end(), "dark") != quiet->themes.end(), joined(quiet->themes));
    }

    // Runtime 1 closes while its renderer waits: the request returns nullopt and nothing more goes out for it (runtime 2's later probe answer is behind whatever it sent before); runtime 2 goes on until its stop.
    never.emplace([platform] { return platform->request("never"); });
    CHECK_MSG(eventually([&] { return host.saw(1, "platform-request:never"); }), joined(requests()));
    CHECK(host.send(FrameBytes(1, wire::Close).bytes()));
    const std::optional<std::optional<std::string>> closed = never->within(5);
    CHECK_MSG(closed && !*closed, closed ? closed->value_or("nullopt") : "still waiting");
    CHECK(host.probe(2, "'alive'") == "alive");
    const size_t before = host.events(1).size();
    sleepMs(300);
    CHECK_MSG(host.events(1).size() == before && host.eventsWith(1, "died:").empty(), joined(host.events(1)));
    CHECK(host.send(FrameBytes(2, wire::Stop).str("stopped by the test").bytes()));
    CHECK_MSG(eventually([&] { return host.saw(2, "died:stopped by the test"); }), joined(host.events(2)));
    CHECK_MSG(quiet && eventually([&] {
                  const std::vector<Json> frozen = quiet->sent("frozen");
                  return frozen.size() == 1 && frozen[0]["reason"].asString() == "Stopped here";
              }),
              "not frozen with its label");
    host.close();
    CHECK(host.served(5) == std::string());

    {
        BuiltRenderers none;
        ScriptedHost engine(layoutAnswers(), nullptr, none.factory());
        CHECK(engine.send(openingOf(*RES) + FrameBytes(1, wire::Open).bytes() + FrameBytes(1, wire::Start).str(kHostedWidget).str(SandboxRemoteCoding::encodeConfig(animatedConfig())).bytes()));
        CHECK_MSG(eventually([&] { return engine.saw(1, "loaded"); }, 20), joined(engine.events(1)));
        CHECK(engine.send(FrameBytes(1, wire::Event).str(kTapOnButton).bytes()));
        const auto sends = [&] {
            const std::vector<std::string> traffic = renderTraffic(engine, 1);
            for (const char* kind : {"kind 68", "kind 71", "kind 72", "kind 73", "kind 74"})
                if (std::find(traffic.begin(), traffic.end(), kind) == traffic.end()) return false;
            return !engine.eventsWith(1, "query:").empty();
        };
        CHECK_MSG(eventually(sends), joined(renderTraffic(engine, 1)));
        CHECK(none.count() == 0 && engine.eventsWith(1, "renderer-ready").empty() && engine.eventsWith(1, "rendered").empty());
        engine.close();
        CHECK(engine.served(5) == std::string());
    }

    {
        std::optional<Asked> large;
        std::atomic<size_t> messageBytes{0}, requestBytes{0};
        BuiltRenderers big;
        ScriptedHost bounded([&](const ParsedFrame& f) -> std::optional<std::string> {
            if (f.kind == wire::PlatformMessage) messageBytes = FieldsOf(f).str().value_or("").size();
            if (f.kind != wire::PlatformRequest) return std::nullopt;
            requestBytes = FieldsOf(f).str().value_or("").size();
            return FrameBytes(f.runtime, wire::PlatformAnswer).str("cut").bytes();
        }, nullptr, big.factory());
        CHECK(bounded.send(hostingOpeningOf(*RES) + FrameBytes(1, wire::Open).bytes() + hostedStartFrame(1, "<p>x</p>")));
        CHECK_MSG(eventually([&] { return bounded.saw(1, "rendered"); }, 20), joined(bounded.events(1)));
        if (HostedTestRenderer* renderer = big.at(0)) {
            const std::shared_ptr<SandboxHostedPlatform> bigPlatform = renderer->platform;
            const size_t limit = SandboxEngine::kRemotePlatformLimit;
            bigPlatform->send(std::string(limit + 100, 'x'));
            CHECK_MSG(eventually([&] { return messageBytes == limit + 1; }, 10), std::to_string(messageBytes));
            large.emplace([bigPlatform, limit] { return bigPlatform->request(std::string(limit + 100, 'y')); });
            CHECK(large->within(10) == std::optional<std::string>("cut"));
            CHECK_MSG(requestBytes == limit + 1, std::to_string(requestBytes));
        } else {
            CHECK_MSG(false, "no renderer was built");
        }
        bounded.close();
        CHECK(bounded.served(5) == std::string());
    }
}

// ───────────── the hosted mode on the host's end ─────────────

/// A layout answer with a height, so a widget that measures (kHostWidget) shows `h:20`.
static const std::string kTallAnswer = "{\"x\":0,\"y\":0,\"width\":100,\"height\":20,\"offsetHeight\":20}";

/// An engine end's HostedTestRenderer that answers every layout query with `kTallAnswer`.
static void tall(HostedTestRenderer& renderer) { renderer.queryResult = kTallAnswer; }

static std::string platformMessageFrame(uint32_t runtime, const std::string& text) { return FrameBytes(runtime, wire::PlatformMessage).str(text).bytes(); }

static std::string platformRequestFrame(uint32_t runtime, const std::string& text) { return FrameBytes(runtime, wire::PlatformRequest).str(text).bytes(); }

/// A valid storage commit of the widget's local area from the engine process's side of `runtime`.
static std::string storageFrame(uint32_t runtime) { return FrameBytes(runtime, wire::Storage).str("{\"local\":{\"k\":\"v\"}}").bytes(); }

static bool died(const WidgetDiagnostics& d) { return d.dead.has_value(); }

/// The details a WidgetHost recorded for diagnostic `kind`.
static std::vector<std::string> diagsOf(const WidgetDiagnostics& d, const std::string& kind) {
    const auto it = d.diags.find(kind);
    return it == d.diags.end() ? std::vector<std::string>() : it->second.second;
}

/// How a hosted widget dies when its host's platform let its renderer's request go unanswered (WidgetHost's `kPlatformAnswerSeconds`).
static const std::string kPlatformLate = "the host's platform did not answer the widget's renderer within 3 s";

/// A hosted WidgetHost on `end`'s hosting stream (its `hostedChannel`), `width` px wide (what the engine end's renderer factory gets), started after `configure` ran on it, its platform requests due as `wait` says when given: its Intl provider answers `intlAnswer`, its platform requests are answered "answer:<message>" (with `answering` false each responder is kept and never called); what reached its callbacks, in order under a lock: each validated batch (asked for before its start), platform message and platform request, each request and wallet call with its responder.
struct HostedStreamHost {
    using Respond = std::function<void(const WidgetRequestOutcome&)>;
    const std::shared_ptr<StreamIntl> intl = std::make_shared<StreamIntl>();
    std::unique_ptr<WidgetHost> host;

    HostedStreamHost(SandboxRemoteHostEnd& end, const char* html, WidgetHostOptions options, double width, std::string intlAnswer, bool answering = true,
                     const std::function<void(WidgetHost&)>& configure = nullptr, std::shared_ptr<SandboxHostedWait> wait = nullptr) {
        intl->answer = std::move(intlAnswer);
        host = std::make_unique<WidgetHost>(html, WidgetTheme{}, width, std::move(options), end.hostedChannel(), intl, std::move(wait));
        host->onRenderOps = [this](const std::string& ops) {
            std::lock_guard<std::mutex> l(mutex_);
            rendered_ += ops;
        };
        host->onPlatformMessage = [this](const std::string& message) {
            std::lock_guard<std::mutex> l(mutex_);
            messages_.push_back(message);
        };
        host->onPlatformRequest = [this, answering](const std::string& message, std::function<void(std::string)> respond) {
            {
                std::lock_guard<std::mutex> l(mutex_);
                requests_.push_back(message);
                if (!answering) {
                    unanswered_.push_back(std::move(respond));
                    return;
                }
            }
            respond("answer:" + message);
        };
        host->onRequest = [this](const WidgetRequest& request, Respond respond) { keep("request:" + request.data + (request.wallet ? ":wallet" : ""), std::move(respond)); };
        host->onWallet = [this](const WalletCall& call, Respond respond) { keep("wallet:" + std::string(call.methodName()) + ":" + call.data + ":" + call.payload.text.value_or(""), std::move(respond)); };
        if (configure) configure(*host);
        host->start();
    }
    /// The WidgetHost goes first: it joins its threads while what its callbacks use still lives.
    ~HostedStreamHost() { host.reset(); }
    HostedStreamHost(const HostedStreamHost&) = delete;
    HostedStreamHost& operator=(const HostedStreamHost&) = delete;

    /// Loaded, and its first render reported by the engine end.
    bool shown() { return host->waitLoaded(20) && host->waitRendered(5); }
    std::string rendered() {
        std::lock_guard<std::mutex> l(mutex_);
        return rendered_;
    }
    /// Whether the validated batches carry `text` as a string of their own (a text node's).
    bool renders(const std::string& text) { return rendered().find(Json::quote(text)) != std::string::npos; }
    std::vector<std::string> messages() {
        std::lock_guard<std::mutex> l(mutex_);
        return messages_;
    }
    std::vector<std::string> requests() {
        std::lock_guard<std::mutex> l(mutex_);
        return requests_;
    }
    std::vector<std::string> calls() {
        std::lock_guard<std::mutex> l(mutex_);
        return calls_;
    }
    /// The `i`th request or wallet call completed from the test's thread; false when it did not come.
    bool respond(size_t i, const WidgetRequestOutcome& outcome) {
        Respond responder;
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (i < responders_.size()) responder = responders_[i];
        }
        if (!responder) return false;
        responder(outcome);
        return true;
    }
    /// The `i`th platform request's kept responder, empty before it came.
    std::function<void(std::string)> unanswered(size_t i) {
        std::lock_guard<std::mutex> l(mutex_);
        return i < unanswered_.size() ? unanswered_[i] : nullptr;
    }

private:
    void keep(const std::string& call, Respond respond) {
        std::lock_guard<std::mutex> l(mutex_);
        calls_.push_back(call);
        responders_.push_back(std::move(respond));
    }

    std::mutex mutex_;
    std::string rendered_;
    std::vector<std::string> messages_;
    std::vector<std::string> requests_;
    std::vector<std::string> calls_;
    std::vector<Respond> responders_;
    std::vector<std::function<void(std::string)>> unanswered_;
};

/// A hosted WidgetHost over real ends (`startHosting`, `serveHosting` with a HostedTestRenderer): the hosting opening carries the sandbox and validator bundles and the renderer's stamp; the widget loads and renders in the engine end (its boot from the bundle's compiled code, its layout answer and the host's Intl, formatted here on `widget.client`, in the batches it asked for, its renderer ready and node count reported), a probe sees its state, a tap after `pressDelivered` gets the host's request callback and then the wallet's, platform messages cross both ways in order and the renderer's request gets the host's answer, and a theme reaches the widget. The counted stream carried the hosting kinds and none of `Start`, `Event`, `Activation`, `AnimationFrame`, `Viewport`, `Resize`, `Theme`, `PressBegan`, `PressEnded`, `ChargeValidator`, `QueryAnswer`, `Ops`, `Query`, `Listen`, `PreventDefault`, `Anim` or `RequestsFrame` (control: an engine-mode WidgetHost through the same counting sends `Start`, `Ops` and `Query` and gets `QueryAnswer`). Each end refuses the other mode's channel: a hosting end's `channel()` and an engine end's `hostedChannel()` kill their client with why, as `refusingHosted` does.
TEST(remote_stream_hosted_widget_host) {
    WidgetHostOptions privileged;
    privileged.context = messageContext(false, true, false, "private");
    privileged.context.wallet = true;
    {
        BuiltRenderers built;
        StreamPeers peers(kBuild, *RES, nullptr, built.factory(tall));
        auto refused = std::make_shared<StreamSink>();
        peers.end->channel()->attach(refused);
        CHECK_MSG(refused->events() == std::vector<std::string>{"died:the stream was opened for hosted widgets"}, joined(refused->events()));
        HostedStreamHost h(*peers.end, kHostWidget, privileged, 300, "INTL-H");
        WidgetHost& host = *h.host;
        CHECK(host.isHosted());
        CHECK(h.shown());
        const WidgetDiagnostics d = host.diagnostics();
        CHECK_MSG(d.loaded && d.rendered && d.rendererReadyMs && d.nodes > 0 && d.height == 0 && !d.dead, d.dead.value_or("alive"));
        CHECK_MSG(d.countDiag("protocol-violation") == 0, joined(diagsOf(d, "protocol-violation")));
        CHECK_MSG(h.renders("h:20") && h.renders("n:INTL-H"), h.rendered().substr(0, 2000));
        CHECK_MSG(bootedFrom(d, "(compiled)"), joined(bootsOf(d)));
        CHECK_MSG(h.intl->processes() == std::vector<long>{processId()}, std::to_string(h.intl->processes().size()) + " requests");
#if defined(__APPLE__) || defined(__linux__)
        CHECK_MSG(h.intl->threads() == std::vector<std::string>{"widget.client"}, joined(h.intl->threads()));
#endif
        const std::vector<std::string> opening = hostingOpeningFields(peers.engineSide->firstFrame());
        CHECK_MSG((opening == std::vector<std::string>{"1", kBuild, RES->sandboxBundle, RES->validatorBundle, firstLine(RES->rendererBundle)}), std::to_string(opening.size()) + " fields");
        CHECK(host.probe("d.textContent + ' ' + document.getElementById('n').textContent") == "h:20 n:INTL-H");
        HostedTestRenderer* r = built.at(0);
        CHECK(r && built.count() == 1 && built.width() == 300);
        if (!r) return;
        CHECK(r->drew("h:20") && !r->sent("query").empty());

        // A tap the host delivered a press for: the request, then the wallet call on its grant, each answered here.
        host.pressDelivered();
        host.sendPlatform("tap");
        CHECK_MSG(eventually([&] { return h.calls() == std::vector<std::string>{"request:ask:wallet"}; }), joined(h.calls()));
        CHECK(h.respond(0, answered(std::string("r"))));
        CHECK_MSG(eventually([&] { return h.calls().size() == 2; }), joined(h.calls()));
        CHECK_MSG((h.calls() == std::vector<std::string>{"request:ask:wallet", "wallet:signData:w:sign"}), joined(h.calls()));
        CHECK(h.respond(1, answered(std::string("s"))));
        CHECK_MSG(probeUntil(host, "log.join(' ')", "ok:r signed:s") == "ok:r signed:s", host.probe("log.join(' ')"));

        // Platform traffic both ways, in order, and the renderer's request answered by the host.
        host.sendPlatform("say:one");
        host.sendPlatform("say:two");
        host.sendPlatform("ask:q1");
        CHECK_MSG(eventually([&] { return h.messages().size() == 3; }), joined(h.messages()));
        CHECK_MSG((h.messages() == std::vector<std::string>{"one", "two", "got:answer:q1"}), joined(h.messages()));
        CHECK(h.requests() == std::vector<std::string>{"q1"});
        CHECK_MSG((r->platformMessages() == std::vector<std::string>{"tap", "say:one", "say:two", "ask:q1"}), joined(r->platformMessages()));
        CHECK(r->answers() == std::vector<std::optional<std::string>>{std::string("answer:q1")});

        WidgetTheme dark;
        dark.colorScheme = "dark";
        host.setTheme(dark);
        CHECK(probeUntil(host, "TelegramWidget.colorScheme", "dark") == "dark");
        CHECK(eventually([&] { return r->sent("theme").size() == 2; }));
        settleHost(host);

        CountingStream& counted = *peers.counted;
        for (const uint8_t kind : {wire::Opening, wire::Start, wire::Event, wire::Activation, wire::AnimationFrame, wire::Viewport, wire::Resize, wire::Theme, wire::PressBegan, wire::PressEnded,
                                   wire::ChargeValidator, wire::QueryAnswer})
            CHECK_MSG(counted.readOf(kind) == 0, "host kind " + std::to_string(kind) + ": " + std::to_string(counted.readOf(kind)));
        for (const uint8_t kind : {wire::Ops, wire::Query, wire::Listen, wire::PreventDefault, wire::Anim, wire::RequestsFrame})
            CHECK_MSG(counted.writtenOf(kind) == 0, "engine kind " + std::to_string(kind) + ": " + std::to_string(counted.writtenOf(kind)));
        CHECK(counted.readOf(wire::HostingOpening, 0) == 1 && counted.writtenOf(wire::Ready, 0) == 1);
        const std::vector<std::pair<uint8_t, size_t>> hostKinds = {{wire::Open, 1}, {wire::HostedStart, 1}, {wire::HostedTheme, 1}, {wire::Platform, 4}, {wire::PlatformAnswer, 1}, {wire::Answer, 2}};
        for (const auto& [kind, count] : hostKinds) CHECK_MSG(counted.readOf(kind, 1) == count, "host kind " + std::to_string(kind) + ": " + std::to_string(counted.readOf(kind)));
        const std::vector<std::pair<uint8_t, size_t>> engineKinds = {{wire::RendererReady, 1}, {wire::Loaded, 1}, {wire::Rendered, 1}, {wire::PlatformMessage, 3},
                                                                     {wire::PlatformRequest, 1}, {wire::Request, 1}, {wire::Wallet, 1}};
        for (const auto& [kind, count] : engineKinds) CHECK_MSG(counted.writtenOf(kind, 1) == count, "engine kind " + std::to_string(kind) + ": " + std::to_string(counted.writtenOf(kind)));
        for (const uint8_t kind : {wire::Intl, wire::Nodes, wire::ValidatedOps, wire::Diag}) CHECK_MSG(counted.writtenOf(kind, 1) > 0, "engine kind " + std::to_string(kind));
        CHECK(counted.readOf(wire::Probe, 1) > 0 && counted.writtenOf(wire::ProbeAnswer, 1) == counted.readOf(wire::Probe, 1));
        CHECK_MSG(!host.diagnostics().dead && !peers.events.ended(), joined(peers.events.all()));
    }
    {
        StreamPeers peers;
        auto refused = std::make_shared<StreamSink>();
        peers.end->hostedChannel()->attach(refused);
        CHECK_MSG(refused->events() == std::vector<std::string>{"died:the stream was opened for engines only"}, joined(refused->events()));
        WidgetHost wrongMode("<p>x</p>", WidgetTheme{}, 300, WidgetHostOptions(), peers.end->hostedChannel());
        wrongMode.start();
        CHECK(wrongMode.waitUntil(died, 5) && wrongMode.diagnostics().dead == std::optional<std::string>("the stream was opened for engines only"));
        StreamHost h(*peers.end, kHostWidget, privileged, 20, "INTL");
        CHECK(h.shown() && h.renders("h:20"));
        CountingStream& counted = *peers.counted;
        CHECK_MSG(counted.writtenOf(wire::Ops) > 0 && counted.writtenOf(wire::Query) > 0 && counted.readOf(wire::QueryAnswer) > 0, std::to_string(counted.writtenOf(wire::Ops)) + " batches");
        CHECK(counted.readOf(wire::Opening) == 1 && counted.readOf(wire::Start) == 1 && counted.readOf(wire::HostingOpening) == 0 && counted.readOf(wire::HostedStart) == 0);
    }
    auto sink = std::make_shared<StreamSink>();
    std::shared_ptr<SandboxHostedChannel> refusing = SandboxRemoteHostEnd::refusingHosted("no hosting here");
    refusing->attach(sink);
    refusing->startHosted("<p>x</p>", testConfig(), SandboxHostedStart{});
    refusing->hostedTheme(WidgetTheme{});
    CHECK(!refusing->platform("x"));
    CHECK(refusing->probe("1") == "ERROR no runtime");
    refusing->close();
    CHECK_MSG(sink->events() == std::vector<std::string>{"died:no hosting here"}, joined(sink->events()));
}

/// A hosted channel's `platform` says whether the writer took the message, the word a hosted client's press waits for (`SandboxEngine::platformPress`): on a hosting stream false before `attach` and after `close`, true on a live route (a kept one too), false for a message the queue cannot hold (whose refusal ends the stream, as a queue past its bounds always did) and for every one once the stream ended; only the admitted ones reach the engine's end. The hosted loopback admits from `attach` until the client's `close`; a refusing channel never.
TEST(remote_stream_hosted_platform_tells_its_writer_admission) {
    {
        SandboxRemoteHostEnd::Limits bounded;
        bounded.queuedBytes = size_t(1) << 20;
        ScriptedEngine engine(bounded, true);
        CHECK(engine.ready());
        const std::shared_ptr<SandboxHostedChannel> early = engine.host().hostedChannel(), a = engine.host().hostedChannel(), b = engine.host().hostedChannel();
        CHECK(!early->platform("before attach"));
        auto sinkA = std::make_shared<StreamSink>(), sinkB = std::make_shared<StreamSink>();
        a->attach(sinkA);
        b->attach(sinkB);
        CHECK(a->platform("a live"));
        CHECK(b->platform("b live"));
        b->kept();
        CHECK(b->platform("b kept"));
        b->close();
        CHECK(!b->platform("b closed"));
        CHECK_MSG(eventually([&] { return engine.saw(2, "close") && engine.saw(1, "platform:a live"); }), joined(engine.frames(2)));
        CHECK(!engine.events().ended());
        // Past the queue's bound: refused, and the stream ends.
        CHECK(!a->platform(std::string(size_t(2) << 20, 'p')));
        const std::string reason = "more than 1048576 bytes queued for the stream";
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }, 2) && endsOnce(engine.events().all(), reason), joined(engine.events().all()));
        CHECK(!a->platform("after the end"));
        CHECK_MSG(eventually([&] { return sinkA->saw("disconnected"); }), joined(sinkA->events()));
        CHECK_MSG((engine.frames(1) == std::vector<std::string>{"open", "platform:a live"}), joined(engine.frames(1)));
        CHECK_MSG((engine.frames(2) == std::vector<std::string>{"open", "platform:b live", "platform:b kept", "close"}), joined(engine.frames(2)));
        CHECK(engine.frames(3).empty());
    }
    {
        SandboxHostedLoopbackChannel loopback(RES, nullptr);
        CHECK(!loopback.platform("before attach"));
        auto sink = std::make_shared<StreamSink>();
        loopback.attach(sink);
        CHECK(loopback.platform("attached"));
        loopback.close();
        CHECK(!loopback.platform("closed"));
        CHECK_MSG(sink->events().empty(), joined(sink->events()));
    }
}

// ───────────── a hosted renderer's answer times and each widget's death ─────────────

/// The tests' reading of hosted platform messages (SandboxHostedAnswers::read) on generic keys, `<what>:<family>[:<sequence>]`: the host's `ask` expects one answer and always counts time (an input event), its `draw` only when sent while visible (a frame request); the renderer's `reply` answers, and `stopped` is its last word. Anything else (a size, a theme, a report) is nothing the stream keeps, and a role sent by the other side counts as none.
static SandboxHostedAnswers::Reading answerKeys(std::string_view message) {
    using Role = SandboxHostedAnswers::Role;
    SandboxHostedAnswers::Reading reading;
    if (message == "stopped") {
        reading.role = Role::Last;
        return reading;
    }
    const size_t colon = message.find(':');
    const std::string_view what = message.substr(0, colon);
    if (colon == std::string_view::npos || (what != "ask" && what != "draw" && what != "reply")) return reading;
    const std::string_view key = message.substr(colon + 1);
    const size_t second = key.find(':');
    reading.role = what == "reply" ? Role::Answers : Role::Expects;
    reading.whenVisible = what == "draw";
    reading.family = uint32_t(std::stoul(std::string(key.substr(0, second))));
    if (second != std::string_view::npos) reading.sequence = std::stoull(std::string(key.substr(second + 1)));
    return reading;
}

/// How a hosting stream with the owner's SandboxHostedAnswers ends when a renderer's answer time ran out.
static const std::string kUnanswered = "a hosted renderer left a platform message unanswered for more than 3000 ms";

static std::string deathName(SandboxHostedChannel::Death death) {
    switch (death) {
    case SandboxHostedChannel::Death::Ordinary: return "ordinary";
    case SandboxHostedChannel::Death::Stalled: return "stalled";
    case SandboxHostedChannel::Death::Spared: return "spared";
    }
    return "?";
}

/// A hosted client on `engine`'s hosting stream, `runtime` there (a stream gives its routes their ids in the order they attach), with its delegate (the shared RemoteRecorder, each renderer request's responder kept and never called, so the request waits until its client settles it) set before any of its traffic, its Intl provider noting each request it formats (`intl`), and `startHosted` sent unless `started` is false; `channel` is the one it was given, whose `death()` the test reads as an embedder does.
struct AnsweringClient {
    AnsweringClient(ScriptedEngine& engine, uint32_t runtime, bool visible = true, bool started = true) : AnsweringClient(engine.host().hostedChannel(), runtime, visible, started) {}
    /// The same on `route`, the stream's own channel or one that passes on to it.
    AnsweringClient(std::shared_ptr<SandboxHostedChannel> route, uint32_t runtime, bool visible = true, bool started = true) : runtime(runtime), channel(std::move(route)) {
        // Before the client's channel is attached: no request can come first.
        delegate.holdPlatformRequests = true;
        SandboxConfig config = testConfig();
        config.visible = visible;
        client = SandboxEngine::hostedClient("<p>x</p>", config, channel, intl);
        client->setDelegate(&delegate);
        if (started) client->startHosted(SandboxHostedStart{});
        dispatch = client->thread().sync([] { return std::this_thread::get_id(); });
    }
    AnsweringClient(const AnsweringClient&) = delete;
    AnsweringClient& operator=(const AnsweringClient&) = delete;

    /// What the stream's end was to the widget, once its client reported a death within 5 s ("alive" without one).
    std::string death() { return eventually([&] { return !delegate.deaths().empty(); }) ? deathName(channel->death()) : "alive"; }
    /// Every call made so far ran on the client's dispatch thread: what it sent was admitted or refused.
    void settle() { client->thread().sync([] {}); }

    const uint32_t runtime;
    const std::shared_ptr<SandboxHostedChannel> channel;
    RemoteRecorder delegate;
    const std::shared_ptr<StreamIntl> intl = std::make_shared<StreamIntl>();
    std::unique_ptr<SandboxEngine> client;
    /// Its client's dispatch thread, read once at construction, before any test holds that thread: a callback that must run there compares against it on every platform, also while the client goes.
    std::thread::id dispatch;
};

/// What owners' withdrawals hear (`SandboxHostedChannel::withdraw`'s `settled`), from whatever thread runs each, in the order they came, under a lock: `<name>:<death>`, what the record's `note` reads as it runs, then " on its dispatch thread" or " off its dispatch thread" for one made for a client (`AnsweringClient::dispatch`, compared by thread identity). Declare it before the clients whose threads call it; `told` returns every record so far, so a case that expects none sees any that came.
class Withdrawals {
public:
    using Settled = std::function<void(SandboxHostedChannel::Death)>;

    /// For a withdrawal of `client`'s widget.
    Settled hearing(std::string name, const AnsweringClient& client, std::function<std::string()> note = {}) { return make(std::move(name), client.dispatch, std::move(note)); }
    /// For a channel no client of the test's was given (the embedder's own): every call is recorded all the same.
    Settled hearing(std::string name) { return make(std::move(name), std::nullopt, {}); }
    std::vector<std::string> told() {
        std::lock_guard<std::mutex> l(mutex_);
        return heard_;
    }

private:
    Settled make(std::string name, std::optional<std::thread::id> dispatch, std::function<std::string()> note) {
        return [this, name = std::move(name), dispatch, note = std::move(note)](SandboxHostedChannel::Death death) {
            std::string record = name + ":" + deathName(death) + (note ? note() : std::string());
            if (dispatch) record += std::this_thread::get_id() == *dispatch ? " on its dispatch thread" : " off its dispatch thread";
            std::lock_guard<std::mutex> l(mutex_);
            heard_.push_back(std::move(record));
        };
    }

    std::mutex mutex_;
    std::vector<std::string> heard_;
};

/// Opens each gate it was given as it goes: declared after the clients and streams whose threads may wait on them, so a thread that a failed check left waiting goes on before they join it.
class OpensAtExit {
public:
    OpensAtExit(std::initializer_list<Gate*> gates) : gates_(gates) {}
    ~OpensAtExit() {
        for (Gate* gate : gates_) gate->open();
    }
    OpensAtExit(const OpensAtExit&) = delete;
    OpensAtExit& operator=(const OpensAtExit&) = delete;

private:
    const std::vector<Gate*> gates_;
};

/// A hosted renderer's answer settles only the oldest message kept for its route with the same key, and a message counts time only when its route was visible as the writer took it. On one stream whose monitor does not run (its report of the routes waits) each route's messages are admitted, the engine's side answers some of them, and 3.2 s later one route's late answer ends the stream on the reader at once, before it is delivered. Stalled: an input behind an unanswered hidden frame, an input answered out of order with a frame, a reply of another family, stale, missing and repeated sequences, a renderer's report or its own `ask`, a frame sent visible and hidden after it, one sent before a visible start, an input whose renderer's answer and last word came before its start (no renderer said them: it is built at the start), the late answer's route, one whose platform request began after its message was due. Spared: a hidden frame, the same input answered in order, an input answered while a hidden frame younger than it waits, sequences answered in order, settings and a `reply` the host sent, a frame shown after it was sent, one sent before a hidden start, one sent after a hidden `Visible` that came before a visible start, a message 3 s younger than the others, an idle route. A renderer that said its last word keeps its ordinary death. The monitor, let go, reports that end once.
TEST(remote_stream_hosted_answers_settle_the_oldest_matching_message) {
    // The monitor waits on it: declared before the end that joins the monitor.
    Gate monitor;
    ScriptedEngine engine({}, true, {answerKeys});
    CHECK(engine.ready());
    struct Route {
        std::string name;
        std::string expected;
        std::unique_ptr<AnsweringClient> client;
    };
    std::vector<Route> routes;
    uint32_t next = 1;
    const auto add = [&](std::string name, std::string expected, bool visible = true, bool started = true) -> AnsweringClient& {
        routes.push_back({std::move(name), std::move(expected), std::make_unique<AnsweringClient>(engine, next++, visible, started)});
        return *routes.back().client;
    };
    AnsweringClient& hiddenAhead = add("hidden frame ahead", "stalled", false);
    AnsweringClient& hiddenAheadInOrder = add("hidden frame ahead, in order", "spared", false);
    AnsweringClient& hiddenFrame = add("hidden frame", "spared", false);
    AnsweringClient& inputThenHidden = add("input, then a hidden frame", "spared");
    AnsweringClient& inputThenFrame = add("input, then a frame", "stalled");
    AnsweringClient& otherFamily = add("another family", "stalled");
    AnsweringClient& sequences = add("stale, missing, repeated", "stalled");
    AnsweringClient& sequencesInOrder = add("sequences in order", "spared");
    AnsweringClient& settings = add("settings", "spared");
    AnsweringClient& unsolicited = add("the renderer's own", "stalled");
    AnsweringClient& shownAfter = add("shown after", "spared", false);
    AnsweringClient& hiddenAfter = add("hidden after", "stalled");
    AnsweringClient& beforeShownStart = add("before a visible start", "stalled", true, false);
    AnsweringClient& beforeHiddenStart = add("before a hidden start", "spared", false, false);
    AnsweringClient& hiddenBeforeStart = add("hidden before a visible start", "spared", true, false);
    AnsweringClient& wordsBeforeStart = add("its renderer's words before its start", "stalled", true, false);
    AnsweringClient& lastWord = add("last word", "ordinary");
    AnsweringClient& late = add("late answer", "stalled");
    AnsweringClient& owes = add("owes after its due", "stalled");
    AnsweringClient& younger = add("younger", "spared");
    add("idle", "spared");
    CHECK_MSG(eventually([&] { return engine.events().open() == routes.size(); }), joined(engine.events().all()));

    // The monitor held in its report of one more route, outside every lock of the end, before any time runs.
    engine.events().holdAt = routes.size() + 1;
    engine.events().hold = &monitor;
    auto barrier = std::make_shared<StreamSink>();
    const std::shared_ptr<SandboxHostedChannel> barrierRoute = engine.host().hostedChannel();
    barrierRoute->attach(barrier);
    const uint32_t barrierId = next++;
    CHECK_MSG(monitor.waitedOn(), joined(engine.events().all()));

    // Before its start: the host's input, then an answer and a last word on its route, all read before the start was admitted; then the start.
    wordsBeforeStart.client->platform("ask:1");
    wordsBeforeStart.settle();
    CHECK(engine.send(platformMessageFrame(wordsBeforeStart.runtime, "reply:1") + platformMessageFrame(wordsBeforeStart.runtime, "stopped") + logFrame(barrierId, "early")));
    CHECK_MSG(eventually([&] { return barrier->saw("log:log:early"); }), joined(barrier->events()));
    wordsBeforeStart.client->startHosted(SandboxHostedStart{});

    hiddenAhead.client->platform("draw:1:1");
    hiddenAhead.client->platform("ask:2");
    hiddenAheadInOrder.client->platform("draw:1:1");
    hiddenAheadInOrder.client->platform("ask:2");
    hiddenFrame.client->platform("draw:1:1");
    inputThenHidden.client->platform("ask:2");
    inputThenHidden.client->setVisible(false);
    inputThenHidden.client->platform("draw:1:1");
    inputThenFrame.client->platform("ask:2");
    inputThenFrame.client->platform("draw:1:1");
    otherFamily.client->platform("ask:2");
    sequences.client->platform("draw:1:5");
    sequences.client->platform("draw:1:6");
    sequencesInOrder.client->platform("draw:1:5");
    sequencesInOrder.client->platform("draw:1:6");
    for (const char* message : {"size:300", "theme:dark", "reply:1:1"}) settings.client->platform(message);
    unsolicited.client->platform("draw:1:1");
    shownAfter.client->platform("draw:1:1");
    shownAfter.client->setVisible(true);
    hiddenAfter.client->platform("draw:1:1");
    hiddenAfter.client->setVisible(false);
    beforeShownStart.client->platform("draw:1:1");
    beforeShownStart.client->startHosted(SandboxHostedStart{});
    beforeHiddenStart.client->platform("draw:1:1");
    beforeHiddenStart.client->startHosted(SandboxHostedStart{});
    hiddenBeforeStart.client->setVisible(false);
    hiddenBeforeStart.client->startHosted(SandboxHostedStart{});
    hiddenBeforeStart.client->platform("draw:1:1");
    for (AnsweringClient* c : {&lastWord, &late, &owes, &younger}) c->client->platform("ask:1");
    for (Route& route : routes) route.client->settle();
    const auto admitted = std::chrono::steady_clock::now();

    // The renderers' words, each after the messages it may answer were admitted, then a barrier: every one of them accepted.
    std::string words;
    const auto say = [&](const AnsweringClient& c, const std::string& message) { words += platformMessageFrame(c.runtime, message); };
    say(hiddenAhead, "reply:2");
    say(hiddenAheadInOrder, "reply:1:1");
    say(hiddenAheadInOrder, "reply:2");
    say(inputThenHidden, "reply:2");
    say(inputThenFrame, "reply:2");
    say(otherFamily, "reply:3");
    for (const char* reply : {"reply:1:4", "reply:1", "reply:1:5", "reply:1:5"}) say(sequences, reply);
    say(sequencesInOrder, "reply:1:5");
    say(sequencesInOrder, "reply:1:6");
    say(settings, "state:1");
    say(settings, "ask:4");
    say(unsolicited, "state:1:1");
    say(unsolicited, "ask:1:1");
    say(lastWord, "stopped");
    say(younger, "reply:1");
    CHECK(engine.send(words + logFrame(barrierId, "said")));
    CHECK_MSG(eventually([&] { return barrier->saw("log:log:said"); }), joined(barrier->events()));

    // 3.2 s after the last admission: a platform request that begins after its route's message was due, a younger message, then a late answer.
    std::this_thread::sleep_until(admitted + std::chrono::milliseconds(3200));
    CHECK(engine.send(platformRequestFrame(owes.runtime, "req")));
    CHECK_MSG(eventually([&] { return has(owes.delegate.events(), "hosted-request:req"); }), joined(owes.delegate.events()));
    younger.client->platform("ask:2");
    younger.settle();
    const auto answered = std::chrono::steady_clock::now();
    CHECK(engine.send(platformMessageFrame(late.runtime, "reply:1")));
    const auto allDied = [&] { return std::all_of(routes.begin(), routes.end(), [](const Route& r) { return !r.client->delegate.deaths().empty(); }); };
    const bool byReader = eventually(allDied, 2);
    const double ms = msSince(answered);
    CHECK_MSG(byReader && !engine.events().ended(), "the late answer ended nothing while the monitor was held: " + joined(engine.events().all()));
    monitor.open();
    engine.events().hold = nullptr;
    CHECK_MSG(eventually(allDied) && eventually([&] { return engine.events().ended().has_value(); }), joined(engine.events().all()));
    CHECK_MSG(endsOnce(engine.events().all(), kUnanswered), joined(engine.events().all()));
    CHECK_MSG(!has(late.delegate.events(), "hosted-message:reply:1") && has(lastWord.delegate.events(), "hosted-message:stopped"), joined(late.delegate.events()) + " / " + joined(lastWord.delegate.events()));
    std::string verdicts;
    for (Route& route : routes) {
        const std::string death = route.client->death();
        CHECK_MSG(death == route.expected, route.name + ": " + death + ", " + joined(route.client->delegate.events()));
        CHECK_MSG(route.client->delegate.deaths() == std::vector<std::string>{kEngineGone}, route.name + ": " + joined(route.client->delegate.deaths()));
        verdicts += (verdicts.empty() ? "" : ", ") + route.name + " " + death;
    }
    printf("    the late answer ended the stream in %.0f ms on the reader: %s\n", ms, verdicts.c_str());
    fflush(stdout);
}

/// The host's own time is never a renderer's: from a platform request's arrival until its answer was written to the stream, its route's messages count no time, those sent meanwhile included, while every other route's time goes on. Four streams at once. Y: a message with 1.5 s of its time used, then a request held 3.2 s at its sink: the stream ends 1.5 s after the hold, neither at its end (all 4.7 s counted) nor 3 s after it (a new full time). X: the same with the message answered 1 s after the hold, which settles it, and a message sent 0.5 s into the hold, which then has its whole 3 s from the hold's end. W: a request answered at once while the writer is blocked (the engine's side reads nothing), its answer queued, and the renderer's next request, held 3.2 s, coming before that answer was written: the time stops from the first request until the second's answer was written, not until the first answer was queued or written. Z: while A's request is held, B's message, admitted while the writer is blocked, runs out 3 s after its admission: B is stalled and A, its message 3.5 s old with 1.5 s of its own time, spared.
TEST(remote_stream_hosted_answers_leave_out_the_hosts_own_time) {
    using Clock = std::chrono::steady_clock;
    const auto seconds = [](Clock::time_point from, Clock::time_point to) { return std::chrono::duration<double>(to - from).count(); };
    Gate xHold, yHold, wHold, wReading, zReading;
    ScriptedEngine x({}, true, {answerKeys}), y({}, true, {answerKeys}), w({}, true, {answerKeys}), z({}, true, {answerKeys});
    CHECK(x.ready() && y.ready() && w.ready() && z.ready());
    auto xSink = std::make_shared<StreamSink>(), ySink = std::make_shared<StreamSink>(), wSink = std::make_shared<StreamSink>();
    auto wBulk = std::make_shared<StreamSink>(), zBulk = std::make_shared<StreamSink>();
    xSink->platformGate = &xHold;
    ySink->platformGate = &yHold;
    // Chosen before W's route is attached: its first request is answered at once and only its second waits, so no failed check below leaves the test writing the gate while an answer thread may read it.
    wSink->platformGate = &wHold;
    wSink->platformGated = "second";
    const std::shared_ptr<SandboxHostedChannel> xRoute = x.host().hostedChannel(), yRoute = y.host().hostedChannel(), wRoute = w.host().hostedChannel();
    const std::shared_ptr<SandboxHostedChannel> wBulkRoute = w.host().hostedChannel(), zBulkRoute = z.host().hostedChannel();
    xRoute->attach(xSink);
    yRoute->attach(ySink);
    wRoute->attach(wSink);
    wBulkRoute->attach(wBulk);
    // A renderer's words are read only once its route started.
    for (SandboxHostedChannel* route : {xRoute.get(), yRoute.get(), wRoute.get()}) route->startHosted("<p>x</p>", testConfig(), SandboxHostedStart{});
    AnsweringClient zA(z, 1), zB(z, 2);
    zBulkRoute->attach(zBulk);
    CHECK_MSG(eventually([&] { return x.saw(1, "hosted-start") && y.saw(1, "hosted-start") && w.saw(1, "hosted-start") && w.saw(2, "open") && z.saw(1, "hosted-start") && z.saw(2, "hosted-start") && z.saw(3, "open"); }),
              joined(x.frames(1)) + " / " + joined(z.frames(1)) + " / " + joined(z.frames(2)));

    // 0 s: the first messages admitted.
    const Clock::time_point t0 = Clock::now();
    const auto at = [&](double s) { std::this_thread::sleep_until(t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s))); };
    CHECK(xRoute->platform("ask:1") && yRoute->platform("ask:1") && wRoute->platform("ask:1"));
    zA.client->platform("ask:1");
    zA.settle();
    const Clock::time_point first = Clock::now();
    // W's and Z's engine sides stop reading and a frame larger than the pair blocks each writer.
    const std::string bulk(size_t(2) << 20, 'b');
    w.holdReading(&wReading);
    z.holdReading(&zReading);
    CHECK(wBulkRoute->platform("x") && zBulkRoute->platform("x") && wBulkRoute->platform(bulk) && zBulkRoute->platform(bulk));
    CHECK_MSG(eventually([&] { return w.writeWaiting() && z.writeWaiting(); }, 2), "the writers were not blocked");
    // 0.3 s: W's renderer asks; its sink answers at once, behind the blocked write.
    at(0.3);
    CHECK(w.send(platformRequestFrame(1, "first")));
    const Clock::time_point wAsked = Clock::now();
    CHECK_MSG(eventually([&] { return wSink->saw("platform-request-done"); }), joined(wSink->events()));
    // 0.5 s: Z's B sends while Z's writer is blocked.
    at(0.5);
    zB.client->platform("ask:1");
    zB.settle();
    const Clock::time_point zSent = Clock::now();
    // 1.5 s: the renderers of X, Y, W (its second request, the first answer not yet written) and Z's A ask; each request waits.
    at(1.5);
    CHECK(x.send(platformRequestFrame(1, "held")) && y.send(platformRequestFrame(1, "held")) && w.send(platformRequestFrame(1, "second")) && z.send(platformRequestFrame(1, "held")));
    const Clock::time_point asked = Clock::now();
    CHECK_MSG(xHold.waitedOn() && yHold.waitedOn() && wHold.waitedOn() && eventually([&] { return has(zA.delegate.events(), "hosted-request:held"); }), joined(wSink->events()) + " / " + joined(zA.delegate.events()));
    // 1.8 s: W's engine side reads again: the first answer is written while the second request waits.
    at(1.8);
    wReading.open();
    CHECK_MSG(eventually([&] { return w.saw(1, "platform-answer:ANSWER"); }, 2), joined(w.frames(1)));
    // 2 s: X sends inside its hold.
    at(2.0);
    CHECK(xRoute->platform("ask:2"));
    // 4.7 s: the holds end; Z ended meanwhile.
    at(4.7);
    xHold.open();
    yHold.open();
    wHold.open();
    const Clock::time_point released = Clock::now();
    const bool zEnded = z.events().ended().has_value();
    // 5.7 s: X's first message answered, 0.5 s before its time would run out.
    at(5.7);
    CHECK(x.send(platformMessageFrame(1, "reply:1")));
    CHECK_MSG(eventually([&] { return x.events().ended() && y.events().ended() && w.events().ended() && z.events().ended(); }, 5),
              joined(x.events().all()) + " / " + joined(y.events().all()) + " / " + joined(w.events().all()) + " / " + joined(z.events().all()));
    zReading.open();
    for (ScriptedEngine* e : {&x, &y, &w, &z}) CHECK_MSG(endsOnce(e->events().all(), kUnanswered), joined(e->events().all()));
    const auto ended = [&](ScriptedEngine& e) { return e.events().endedAt().value_or(Clock::now()); };
    const double held = seconds(asked, released), wHeld = seconds(wAsked, released);
    // How much later than its message's time ran out (the host's left out) each stream ended; the alternatives are 1 s or more away.
    const double yLate = seconds(first, ended(y)) - 3 - held, xLate = seconds(released, ended(x)) - 3, wLate = seconds(first, ended(w)) - 3 - wHeld,
                 zLate = seconds(zSent, ended(z)) - 3;
    CHECK_MSG(yLate > -0.1 && yLate < 0.6, "Y ended " + std::to_string(seconds(released, ended(y))) + " s after its hold");
    CHECK_MSG(xLate > -0.1 && xLate < 0.6 && xSink->saw("platform:reply:1"), "X ended " + std::to_string(seconds(released, ended(x))) + " s after its hold");
    CHECK_MSG(wLate > -0.1 && wLate < 0.6, "W ended " + std::to_string(seconds(released, ended(w))) + " s after its second answer");
    CHECK_MSG(zEnded && zLate > -0.1 && zLate < 0.6, "Z ended " + std::to_string(seconds(zSent, ended(z))) + " s after B's message");
    CHECK_MSG(zB.death() == "stalled" && zA.death() == "spared", "B " + zB.death() + ", A " + zA.death() + ": " + joined(zA.delegate.events()));
    printf("    after its %.2f s hold: Y ended %.2f s on, X %.2f s on, W %.2f s on; Z %.2f s after B's message\n", held, seconds(released, ended(y)), seconds(released, ended(x)),
           seconds(released, ended(w)), seconds(zSent, ended(z)));
    fflush(stdout);
}

/// The stream's first end decides at once, under the connection's lock and before any client hears it, what each hosted widget's death is (`SandboxHostedChannel::death`, read once its client reported it), and nothing changes that afterwards. At the engine process's end of the stream: A's answer, read while A's client is held on its dispatch thread, leaves A spared though A's client had heard neither the answer nor the end when the end was reported; B's young unanswered message and H's hidden frame keep their ordinary deaths; an idle route is spared, a renderer that said its last word keeps its own death, and a widget attached afterwards is spared. With no message unanswered on a route that takes part (the others' settled by the renderer's last word, its engine's `Died`, the host's keep and close) every route keeps its ordinary death. An end from another thread while the reader is held in a delivery (a queue past its bound) is decided at that end: with the unanswered route closed or kept before the reader's fan-out the idle one is still spared, and with none unanswered it is ordinary. A route with `kRemoteQueuedCalls` messages unanswered ends the stream at the next one, which is not sent. The owner's stop decides nothing: every death ordinary, a widget attached afterwards too.
TEST(remote_stream_hosted_answers_decide_each_death_once_at_the_end) {
    const std::string closed = "the engine process closed the stream";
    {
        // A's dispatch thread waits on it: declared before the client that joins that thread.
        Gate held;
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        AnsweringClient a(engine, 1), b(engine, 2), hidden(engine, 3, false), idle(engine, 4), last(engine, 5);
        auto barrier = std::make_shared<StreamSink>();
        const std::shared_ptr<SandboxHostedChannel> barrierRoute = engine.host().hostedChannel();
        barrierRoute->attach(barrier);
        a.client->platform("ask:1");
        b.client->platform("ask:1");
        hidden.client->platform("draw:1:1");
        last.client->platform("ask:1");
        for (AnsweringClient* c : {&a, &b, &hidden, &last}) c->settle();
        a.client->thread().async([&held] { held.wait(); });
        CHECK(held.waitedOn());
        CHECK(engine.send(platformMessageFrame(1, "reply:1") + platformMessageFrame(5, "stopped") + logFrame(6, "said")));
        // Every frame of the host's written and every word of the engine's accepted: the reader alone ends the stream.
        CHECK_MSG(eventually([&] { return barrier->saw("log:log:said") && engine.saw(1, "platform:ask:1") && engine.saw(2, "platform:ask:1") && engine.saw(3, "platform:draw:1:1") && engine.saw(5, "platform:ask:1"); }),
                  joined(barrier->events()));
        engine.close();
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), closed), joined(engine.events().all()));
        CHECK_MSG(a.delegate.events().empty(), joined(a.delegate.events()));
        CHECK_MSG(b.death() == "ordinary" && hidden.death() == "ordinary" && idle.death() == "spared" && last.death() == "ordinary",
                  b.death() + " " + hidden.death() + " " + idle.death() + " " + last.death());
        held.open();
        CHECK_MSG(a.death() == "spared", a.death());
        CHECK_MSG((a.delegate.events() == std::vector<std::string>{"hosted-message:reply:1", "died:" + kEngineGone}), joined(a.delegate.events()));
        AnsweringClient attached(engine, 0);
        CHECK_MSG(attached.death() == "spared", attached.death());
    }
    {
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        AnsweringClient last(engine, 1), idle(engine, 2);
        auto dead = std::make_shared<StreamSink>(), kept = std::make_shared<StreamSink>(), gone = std::make_shared<StreamSink>();
        const std::shared_ptr<SandboxHostedChannel> deadRoute = engine.host().hostedChannel(), keptRoute = engine.host().hostedChannel(), goneRoute = engine.host().hostedChannel();
        deadRoute->attach(dead);
        keptRoute->attach(kept);
        goneRoute->attach(gone);
        last.client->platform("ask:1");
        last.settle();
        CHECK(deadRoute->platform("ask:1") && keptRoute->platform("ask:1") && goneRoute->platform("ask:1"));
        keptRoute->kept();
        goneRoute->close();
        CHECK(engine.send(platformMessageFrame(1, "stopped") + FrameBytes(3, wire::Died).str("its engine died").bytes()));
        CHECK_MSG(eventually([&] { return dead->saw("died:its engine died") && engine.saw(5, "close"); }), joined(dead->events()));
        engine.close();
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), closed), joined(engine.events().all()));
        CHECK_MSG(idle.death() == "ordinary" && last.death() == "ordinary", idle.death() + " " + last.death());
    }
    for (const std::string variant : {"closed", "kept", "none"}) {
        SandboxRemoteHostEnd::Limits bounded;
        bounded.queuedBytes = size_t(1) << 20;
        // The reader waits on it: declared before the end that joins the reader.
        Gate delivering;
        ScriptedEngine engine(bounded, true, {answerKeys});
        CHECK(engine.ready());
        auto busy = std::make_shared<StreamSink>(), waiting = std::make_shared<StreamSink>();
        busy->logGate = &delivering;
        const std::shared_ptr<SandboxHostedChannel> busyRoute = engine.host().hostedChannel(), waitingRoute = engine.host().hostedChannel();
        busyRoute->attach(busy);
        waitingRoute->attach(waiting);
        AnsweringClient idle(engine, 3);
        if (variant != "none") CHECK(waitingRoute->platform("ask:1"));
        CHECK(engine.send(logFrame(1, "hold")));
        CHECK_MSG(delivering.waitedOn(), variant + ": " + joined(busy->events()));
        // The queue past its bound from this thread, then the route that waited closed or kept, all while the reader is in that delivery.
        CHECK(!busyRoute->platform(std::string(size_t(2) << 20, 'p')));
        if (variant == "closed") waitingRoute->close();
        if (variant == "kept") waitingRoute->kept();
        CHECK_MSG(!engine.events().ended() && idle.delegate.deaths().empty(), variant + ": " + joined(engine.events().all()));
        delivering.open();
        const std::string reason = "more than 1048576 bytes queued for the stream";
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), reason), variant + ": " + joined(engine.events().all()));
        CHECK_MSG(idle.death() == (variant == "none" ? "ordinary" : "spared"), variant + ": " + idle.death());
        CHECK_MSG(eventually([&] { return busy->saw("disconnected") && (variant == "closed" || waiting->saw("disconnected")); }) && waiting->saw("disconnected") == (variant != "closed"),
                  variant + ": " + joined(waiting->events()));
    }
    {
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        auto sink = std::make_shared<StreamSink>();
        const std::shared_ptr<SandboxHostedChannel> route = engine.host().hostedChannel();
        route->attach(sink);
        AnsweringClient idle(engine, 2);
        // Hidden frames, which count no time, each written before the next 1024 are sent.
        route->setVisible(false);
        size_t admitted = 0;
        for (size_t i = 0; i < SandboxEngine::kRemoteQueuedCalls; i++) {
            if (route->platform("draw:1:" + std::to_string(i))) admitted++;
            if (i % 1024 == 1023) eventually([&] { return engine.frames(1).size() >= i + 3; });
        }
        CHECK_MSG(admitted == SandboxEngine::kRemoteQueuedCalls && !engine.events().ended(), std::to_string(admitted) + " admitted: " + joined(engine.events().all()));
        CHECK(!route->platform("draw:1:16384"));
        const std::string reason = "more than 16384 platform messages unanswered on runtime 1";
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), reason), joined(engine.events().all()));
        CHECK_MSG(idle.death() == "spared", idle.death());
        const std::vector<std::string> sent = engine.frames(1);
        CHECK_MSG(sent.size() == SandboxEngine::kRemoteQueuedCalls + 2 && sent.back() == "platform:draw:1:16383", std::to_string(sent.size()) + " frames, the last " + (sent.empty() ? std::string("(none)") : sent.back()));
    }
    {
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        AnsweringClient waiting(engine, 1), idle(engine, 2);
        waiting.client->platform("ask:1");
        waiting.settle();
        engine.host().stop();
        CHECK_MSG(endsOnce(engine.events().all(), "the host stopped the stream"), joined(engine.events().all()));
        CHECK_MSG(waiting.death() == "ordinary" && idle.death() == "ordinary", waiting.death() + " " + idle.death());
        AnsweringClient attached(engine, 0);
        CHECK_MSG(attached.death() == "ordinary", attached.death());
    }
}

/// A hosted channel of an embedder's own, `RefusingChannel`'s shape (remote_stream.cpp): the base holds no state of the death a transport decides, so it keeps the copies and moves it had and reports `Ordinary`.
class OwnHostedChannel final : public SandboxHostedChannel {
public:
    explicit OwnHostedChannel(std::string reason) : reason_(std::move(reason)) {}

    void attach(std::shared_ptr<SandboxRemoteSink> sink) override {
        if (sink) sink->remoteDied(reason_);
    }
    void startHosted(const std::string&, const SandboxConfig&, const SandboxHostedStart&) override {}
    void hostedTheme(const WidgetTheme&) override {}
    bool platform(const std::string&) override { return false; }
    void start(const std::string&, const SandboxConfig&) override {}
    void event(const std::string&) override {}
    void activationEvent(const std::string&, int) override {}
    void frame(double) override {}
    void viewport(double, double, double) override {}
    void resize(const std::string&) override {}
    void theme(const std::string&, const std::map<std::string, std::string>&) override {}
    void contentSafeArea(const WidgetContentSafeArea&) override {}
    void setData(const WidgetData&) override {}
    void setVisible(bool) override {}
    void pressBegan() override {}
    void pressEnded() override {}
    void chargeValidator(double) override {}
    void stop(const std::string&) override {}
    void answer(int64_t, const SandboxRemoteOutcome&) override {}
    std::string probe(const std::string&) override { return "ERROR no runtime"; }
    void close() override {}

private:
    std::string reason_;
};
static_assert(std::is_copy_constructible_v<OwnHostedChannel> && std::is_move_constructible_v<OwnHostedChannel>, "a channel of the embedder's own keeps its copies and moves");

/// A widget's first death keeps its own kind: a hosted channel's `death()` takes the transport's end's decision only when that end is the widget's first death. T's own death (its owner's stop, `terminate`), queued on T's dispatch thread behind a held job before the engine process's end of the stream, stays ordinary with its own reason once that thread runs, while G, held the same way with nothing queued, is spared: all idle, with B's message unanswered at that end. F's counted stop (`stop`), queued the same way, is decided on F's thread only after that end decided F: it yields to the end (`SandboxHostedChannel::localStop`), and F dies of that end, spared as G is. A hosted WidgetHost whose stream ended before `start()` hears that death once, at its start, in `onDeath`, which reads `death()` spared through a weak reference to the very channel it was given. A hosted client on a channel of the embedder's own, copied and moved first, dies with that channel's reason and reads `Ordinary`.
TEST(remote_stream_hosted_answers_keep_a_widgets_first_death) {
    // What the threads joined below use, declared before them: it outlives their joins on every exit, a failed check's too.
    Gate heldF, heldG, heldT;
    std::mutex mutex;
    std::vector<std::string> heard;
    ScriptedEngine engine({}, true, {answerKeys});
    CHECK(engine.ready());
    AnsweringClient b(engine, 1), f(engine, 2), g(engine, 3), t(engine, 4);
    const std::shared_ptr<SandboxHostedChannel> unstartedRoute = engine.host().hostedChannel();
    WidgetHost unstarted("<p>x</p>", WidgetTheme{}, 300, WidgetHostOptions(), unstartedRoute);
    CHECK_MSG(eventually([&] { return engine.saw(5, "open"); }), joined(engine.frames(0)));
    b.client->platform("ask:1");
    for (AnsweringClient* c : {&b, &f, &g, &t}) c->settle();
    f.client->thread().async([&heldF] { heldF.wait(); });
    g.client->thread().async([&heldG] { heldG.wait(); });
    t.client->thread().async([&heldT] { heldT.wait(); });
    CHECK(heldF.waitedOn() && heldG.waitedOn() && heldT.waitedOn() && eventually([&] { return engine.saw(1, "platform:ask:1"); }));
    f.client->stop("its own limit");
    t.client->terminate();
    engine.close();
    CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), "the engine process closed the stream"), joined(engine.events().all()));
    // The unstarted host's client took the end's decision with no delegate to tell yet.
    CHECK_MSG(eventually([&] { return unstartedRoute->death() == SandboxHostedChannel::Death::Spared; }), deathName(unstartedRoute->death()));
    CHECK_MSG(f.delegate.events().empty() && g.delegate.events().empty() && t.delegate.events().empty(), joined(f.delegate.events()) + " / " + joined(g.delegate.events()) + " / " + joined(t.delegate.events()));
    heldF.open();
    heldG.open();
    heldT.open();
    CHECK_MSG(t.death() == "ordinary" && t.delegate.deaths() == std::vector<std::string>{"terminated"}, t.death() + ": " + joined(t.delegate.events()));
    CHECK_MSG(f.death() == "spared" && f.delegate.deaths() == std::vector<std::string>{kEngineGone}, f.death() + ": " + joined(f.delegate.events()));
    CHECK_MSG(g.death() == "spared" && g.delegate.deaths() == std::vector<std::string>{kEngineGone}, g.death() + ": " + joined(g.delegate.events()));
    CHECK_MSG(b.death() == "ordinary", b.death());
    unstarted.onDeath = [&, route = std::weak_ptr<SandboxHostedChannel>(unstartedRoute)](const std::string& reason) {
        const std::shared_ptr<SandboxHostedChannel> channel = route.lock();
        std::lock_guard<std::mutex> l(mutex);
        heard.push_back(reason + ": " + (channel ? deathName(channel->death()) : "gone"));
    };
    unstarted.start();
    const auto told = [&] {
        std::lock_guard<std::mutex> l(mutex);
        return heard;
    };
    CHECK_MSG(eventually([&] { return !told().empty(); }), "no death at its start");
    sleepMs(200);
    CHECK_MSG(told() == std::vector<std::string>{kEngineGone + ": spared"}, joined(told()));
    CHECK_MSG(unstarted.diagnostics().dead == std::optional<std::string>(kEngineGone), unstarted.diagnostics().dead.value_or("alive"));

    OwnHostedChannel original("its own channel");
    OwnHostedChannel copied(original);
    auto own = std::make_shared<OwnHostedChannel>(std::move(copied));
    RemoteRecorder ownDelegate;
    std::unique_ptr<SandboxEngine> ownClient = SandboxEngine::hostedClient("<p>x</p>", testConfig(), own);
    ownClient->setDelegate(&ownDelegate);
    CHECK_MSG(eventually([&] { return !ownDelegate.deaths().empty(); }) && ownDelegate.deaths() == std::vector<std::string>{"its own channel"}, joined(ownDelegate.events()));
    CHECK_MSG(own->death() == SandboxHostedChannel::Death::Ordinary && original.death() == SandboxHostedChannel::Death::Ordinary, deathName(own->death()));
}

/// D1, the late look's rule (`SandboxRemoteHostEnd::lookAtAnswers`), at times the test gives it, as the monitor applies it after each wake (its planned wake spent, the second look kept) and as the reader asks it before a frame settles a message whose time ran out (only whether the stream ends): a message due at 3 s. An ordinary wake judges at once: nothing before the due, the end at it, also 0.9 s late. A first look 1.2 s late concludes nothing and asks for a second look at 5.2 s: the reader before it concludes nothing (an answer in that second is in time), and with the message answered the second look concludes nothing and is spent. With it unanswered, a look before the second one, itself late for an earlier planned wake (a write's), concludes nothing and moves nothing; the second look, itself 1.2 s late, judges the message with all its time and asks for none, and so does the reader at or after the second look's time with the monitor still asleep for it. After a spent second look a later late wake (another sleep) asks again, once.
TEST(remote_stream_hosted_answers_look_again_once_after_a_late_wake) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point origin = Clock::now();
    const auto at = [&](int ms) { return origin + std::chrono::milliseconds(ms); };
    const auto shown = [&](const std::optional<Clock::time_point>& t) {
        return t ? std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(*t - origin).count()) + " ms" : std::string("none");
    };
    /// The monitor's state between looks as the stream keeps it (`wake_`, `recheck_`), with the earliest answer time.
    struct Monitor {
        std::optional<Clock::time_point> due, wake, recheck;
    };
    const auto monitorLooks = [&](Monitor& m, int now) {
        const SandboxRemoteHostEnd::AnswerLook look = SandboxRemoteHostEnd::lookAtAnswers(m.due, m.wake, m.recheck, at(now));
        m.wake.reset();
        m.recheck = look.recheck;
        return look.expired;
    };
    const auto readerLooks = [&](const Monitor& m, int now) { return SandboxRemoteHostEnd::lookAtAnswers(m.due, m.wake, m.recheck, at(now)).expired; };
    {
        Monitor m{at(3000), at(3000), std::nullopt};
        CHECK(!readerLooks(m, 2900) && readerLooks(m, 3000));
        CHECK(!monitorLooks(m, 2900) && !m.recheck);
        m.wake = at(3000);
        CHECK(monitorLooks(m, 3000) && !m.recheck);
        Monitor slow{at(3000), at(3000), std::nullopt};
        CHECK_MSG(monitorLooks(slow, 3900) && !slow.recheck, "0.9 s after its wake: " + shown(slow.recheck));
    }
    {
        Monitor m{at(3000), at(3000), std::nullopt};
        CHECK_MSG(!monitorLooks(m, 4200) && m.recheck == at(5200), shown(m.recheck));
        m.wake = m.recheck;
        CHECK(!readerLooks(m, 4800));
        m.due.reset();
        CHECK_MSG(!monitorLooks(m, 5200) && !m.recheck, shown(m.recheck));
    }
    {
        Monitor m{at(3000), at(3000), std::nullopt};
        CHECK(!monitorLooks(m, 4200) && m.recheck == at(5200));
        m.wake = at(3500);
        CHECK_MSG(!monitorLooks(m, 5000) && m.recheck == at(5200), "a late look before the second one asked again at " + shown(m.recheck));
        m.wake = m.recheck;
        CHECK_MSG(monitorLooks(m, 6400) && !m.recheck, "the late second look asked again at " + shown(m.recheck));
    }
    {
        Monitor m{at(3000), at(3000), std::nullopt};
        CHECK(!monitorLooks(m, 4200));
        m.wake = m.recheck;
        CHECK(!readerLooks(m, 5100) && readerLooks(m, 5200) && readerLooks(m, 6400));
    }
    {
        Monitor m{std::nullopt, at(3000), std::nullopt};
        CHECK(!monitorLooks(m, 4200));
        m.wake = m.recheck;
        CHECK(!monitorLooks(m, 5200) && !m.recheck);
        m.due = at(8000);
        m.wake = at(8000);
        CHECK_MSG(!monitorLooks(m, 9500) && m.recheck == at(10500), shown(m.recheck));
        m.wake = m.recheck;
        CHECK(monitorLooks(m, 11000) && !m.recheck);
    }
}

/// A stream's hosted channel whose client's sink reaches the stream as a HeldSink (built at the client's attach, on the thread that builds the client, which reads it) that holds the platform request `heldRequest` at `requestGate` on its way to the client and, given `answerGate`, its answer there on its way back to the stream; every call goes on to `inner`, and every sink call to the client. A stream channel's publication of the end's decision is its own (`publishEndedDeath` reaches only the channel the client was given), so this one's `death()` is the base's `Ordinary` and `tookTheEnd` tells whether the client took the end for the widget's first death and asked for it. Its client's claim of a death of the widget's own (`publishOwnDeath`, private to the stream's channel too) reaches `inner` as the host's `localStop`, which takes a route the end has not decided yet as that claim does (an end that came first it leaves as it stands), and `decided`, `withdraw` and `localStop` are `inner`'s.
class HeldSinkChannel final : public SandboxHostedChannel {
public:
    HeldSinkChannel(std::shared_ptr<SandboxHostedChannel> inner, std::string heldRequest, Gate* requestGate, Gate* answerGate = nullptr)
        : inner_(std::move(inner)), heldRequest_(std::move(heldRequest)), requestGate_(requestGate), answerGate_(answerGate) {}

    /// The held sink's requests (`HeldSink::requests`), none before the client attached.
    std::vector<std::string> requests() const { return held_ ? held_->requests() : std::vector<std::string>(); }
    bool tookTheEnd() const { return tookTheEnd_; }

    void attach(std::shared_ptr<SandboxRemoteSink> sink) override {
        held_ = std::make_shared<HeldSink>(std::move(sink));
        held_->heldRequest = heldRequest_;
        held_->requestGate = requestGate_;
        held_->answerGate = answerGate_;
        inner_->attach(held_);
    }
    void startHosted(const std::string& html, const SandboxConfig& config, const SandboxHostedStart& start) override { inner_->startHosted(html, config, start); }
    void hostedTheme(const WidgetTheme& theme) override { inner_->hostedTheme(theme); }
    bool platform(const std::string& message) override { return inner_->platform(message); }
    void kept() override { inner_->kept(); }
    void start(const std::string& html, const SandboxConfig& config) override { inner_->start(html, config); }
    void event(const std::string& json) override { inner_->event(json); }
    void activationEvent(const std::string& json, int gesture) override { inner_->activationEvent(json, gesture); }
    void frame(double time) override { inner_->frame(time); }
    void viewport(double innerWidth, double innerHeight, double dpr) override { inner_->viewport(innerWidth, innerHeight, dpr); }
    void resize(const std::string& entriesJson) override { inner_->resize(entriesJson); }
    void theme(const std::string& colorScheme, const std::map<std::string, std::string>& themeParams) override { inner_->theme(colorScheme, themeParams); }
    void contentSafeArea(const WidgetContentSafeArea& area) override { inner_->contentSafeArea(area); }
    void setData(const WidgetData& data) override { inner_->setData(data); }
    void setVisible(bool visible) override { inner_->setVisible(visible); }
    void pressBegan() override { inner_->pressBegan(); }
    void pressEnded() override { inner_->pressEnded(); }
    void chargeValidator(double ms) override { inner_->chargeValidator(ms); }
    void stop(const std::string& reason) override { inner_->stop(reason); }
    void answer(int64_t id, const SandboxRemoteOutcome& outcome) override { inner_->answer(id, outcome); }
    std::string probe(const std::string& code) override { return inner_->probe(code); }
    void close() override { inner_->close(); }
    bool decided() const override { return inner_->decided(); }
    Withdrawal withdraw(std::function<void(Death death)> settled) override { return inner_->withdraw(std::move(settled)); }
    std::optional<Death> localStop() override { return inner_->localStop(); }

private:
    void publishEndedDeath() override { tookTheEnd_ = true; }
    void publishOwnDeath() override { inner_->localStop(); }

    const std::shared_ptr<SandboxHostedChannel> inner_;
    const std::string heldRequest_;
    Gate* const requestGate_;
    Gate* const answerGate_;
    std::shared_ptr<HeldSink> held_;
    std::atomic<bool> tookTheEnd_{false};
};

/// The stream's end reaches a hosted client that holds exactly `kRemoteQueuedCalls` messages as the end, not as a flood: A's dispatch thread is held while the engine process sends A that many platform messages, every one legal, and then ends the stream with only B's message unanswered; let go, A hears every message, then the end, spared, as an idle widget is. With one message more the overflow came first: A dies flooded, its own death, `Ordinary` whatever the end decided. So it does when the message past the bound is a platform request A's `widget.answer` hands to A's client only after the reader's fan-out queued the end for A (held on its way to the client, as a thread that lost the processor), before A's thread ran that end: A dies flooded, the end's decision (spared) never asked for, and the request never reaches A's delegate.
TEST(remote_stream_hosted_answers_end_reaches_a_full_client) {
    const std::string flooded = "the widget engine process sent more than the host keeps queued";
    enum class Leg { Exact, Over, RequestAfterTheEnd };
    for (const Leg leg : {Leg::Exact, Leg::Over, Leg::RequestAfterTheEnd}) {
        const std::string variant = leg == Leg::Exact ? "exactly at the bound" : leg == Leg::Over ? "one past the bound" : "a request past the bound after the end";
        const bool over = leg != Leg::Exact, crossing = leg == Leg::RequestAfterTheEnd;
        // A's dispatch thread and A's `widget.answer` wait on them: declared before the client and the stream that join those threads.
        Gate held, requestHeld;
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        const std::shared_ptr<HeldSinkChannel> heldRoute = crossing ? std::make_shared<HeldSinkChannel>(engine.host().hostedChannel(), "second", &requestHeld) : nullptr;
        const std::shared_ptr<SandboxHostedChannel> aRoute = heldRoute ? std::shared_ptr<SandboxHostedChannel>(heldRoute) : engine.host().hostedChannel();
        AnsweringClient a(aRoute, 1), b(engine, 2);
        auto barrier = std::make_shared<StreamSink>();
        const std::shared_ptr<SandboxHostedChannel> barrierRoute = engine.host().hostedChannel();
        barrierRoute->attach(barrier);
        b.client->platform("ask:1");
        b.settle();
        a.settle();
        a.client->thread().async([&held] { held.wait(); });
        CHECK(held.waitedOn());
        const size_t count = SandboxEngine::kRemoteQueuedCalls + (leg == Leg::Over ? 1 : 0);
        std::string words;
        for (size_t i = 0; i < count; i++) words += platformMessageFrame(1, "s");
        if (crossing) words += platformRequestFrame(1, "second");
        CHECK(engine.send(words + logFrame(3, "said")));
        CHECK_MSG(eventually([&] { return barrier->saw("log:log:said"); }), variant + ": " + joined(barrier->events()));
        CHECK_MSG(a.client->remoteBacklog().calls == SandboxEngine::kRemoteQueuedCalls, variant + ": " + std::to_string(a.client->remoteBacklog().calls) + " held");
        if (crossing) CHECK_MSG(eventually([&] { return has(heldRoute->requests(), "second"); }), variant + ": " + joined(heldRoute->requests()));
        engine.close();
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), "the engine process closed the stream"), variant + ": " + joined(engine.events().all()));
        if (crossing) {
            // The reader's fan-out queued the end for A behind its held thread: now A's `widget.answer` hands the request over, past the bound.
            requestHeld.open();
            CHECK_MSG(eventually([&] { return has(heldRoute->requests(), "second:returned"); }), variant + ": " + joined(heldRoute->requests()));
        }
        CHECK_MSG(b.death() == "ordinary" && a.delegate.events().empty(), variant + ": B " + b.death() + ", A " + joined(a.delegate.events()));
        held.open();
        CHECK_MSG(a.death() == (over ? "ordinary" : "spared") && !(heldRoute && heldRoute->tookTheEnd()), variant + ": " + a.death() + (heldRoute && heldRoute->tookTheEnd() ? ", the end's decision asked for" : ""));
        const size_t heard = a.delegate.eventsWith("hosted-message:").size();
        CHECK_MSG(heard == SandboxEngine::kRemoteQueuedCalls && a.delegate.deaths() == std::vector<std::string>{over ? flooded : kEngineGone} && a.delegate.eventsWith("hosted-request:").empty(),
                  variant + ": " + std::to_string(heard) + " messages, " + joined(a.delegate.deaths()) + " " + joined(a.delegate.eventsWith("hosted-request:")));
        CHECK_MSG(eventually([&] { return a.client->remoteBacklog().calls == 0; }), variant + ": " + std::to_string(a.client->remoteBacklog().calls) + " still held");
    }
}

/// A rejection past the bound and the end's choice are one step each (`admit`, `remoteDisconnected`), so a rejection in the middle of its step when the end chooses is that flood, never lost between the two. A's dispatch thread is held with exactly `kRemoteQueuedCalls` legal messages queued; a platform request past the bound reaches A's client on A's `widget.answer`, whose rejection is held in the middle of its step (`SandboxEngine::setRemoteRejectionStep`: rejected, the overflow not yet claimed), with the reader past it at a later runtime's message and then at the engine process's end, which is queued for A behind them all. Let go, A's dispatch thread hands every message over and reaches the end's choice, which reports itself on that thread as it takes the step's lock (the seam's `choosing`, the attempt seen, not guessed from the time): for half a second from that report the choice, which waits for the held step, delivers no death and its job does not finish, where a choice that read the overflow outside the step would have killed A of the end at once. Once the step is let go the overflow is claimed and A dies flooded, its own death, its channel's `death()` `Ordinary` (the end's decision for idle A, spared, never taken), the request never reaches A's delegate, B keeps its ordinary death and every place is given back; neither gate ran out of its own time.
TEST(remote_stream_hosted_answers_end_and_a_rejection_in_its_step) {
    const std::string flooded = "the widget engine process sent more than the host keeps queued";
    // A's dispatch thread and A's rejection wait on them, and their threads report here: declared before the client and the stream that join those threads.
    Gate held, step;
    std::atomic<bool> heldOpened{false}, stepOpened{false}, chose{false}, choseOnDispatch{false}, choiceDone{false};
    ScriptedEngine engine({}, true, {answerKeys});
    CHECK(engine.ready());
    AnsweringClient a(engine, 1), b(engine, 2);
    // Before the engine process sends A anything and before the stream can end: the step for the rejecting thread, the choice's report for A's dispatch thread.
    SandboxEngine& aClient = *a.client;
    aClient.setRemoteRejectionStep([&step, &stepOpened] { stepOpened = step.wait(); },
                                   [&aClient, &chose, &choseOnDispatch] {
                                       choseOnDispatch = aClient.thread().isCurrent();
                                       chose = true;
                                   });
    auto barrier = std::make_shared<StreamSink>();
    const std::shared_ptr<SandboxHostedChannel> barrierRoute = engine.host().hostedChannel();
    barrierRoute->attach(barrier);
    b.client->platform("ask:1");
    b.settle();
    a.settle();
    a.client->thread().async([&held, &heldOpened] { heldOpened = held.wait(); });
    CHECK(held.waitedOn());
    std::string words;
    for (size_t i = 0; i < SandboxEngine::kRemoteQueuedCalls; i++) words += platformMessageFrame(1, "s");
    CHECK(engine.send(words + platformRequestFrame(1, "second") + logFrame(3, "said")));
    // The rejection held before the overflow's claim, the reader past it at the later runtime's message, then at the end.
    CHECK_MSG(step.waitedOn() && eventually([&] { return barrier->saw("log:log:said"); }), joined(barrier->events()));
    CHECK_MSG(a.client->remoteBacklog().calls == SandboxEngine::kRemoteQueuedCalls, std::to_string(a.client->remoteBacklog().calls) + " held");
    engine.close();
    CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), "the engine process closed the stream"), joined(engine.events().all()));
    CHECK_MSG(!chose && a.delegate.events().empty(), joined(a.delegate.events()));
    // The reader's fan-out queued the end for A behind its held thread: let go, the thread hands over every message, then comes to the end's choice.
    held.open();
    CHECK_MSG(eventually([&] { return chose.load(); }), "the end's choice never came, " + std::to_string(a.delegate.eventsWith("hosted-message:").size()) + " messages heard");
    CHECK_MSG(choseOnDispatch && a.delegate.eventsWith("hosted-message:").size() == SandboxEngine::kRemoteQueuedCalls,
              std::string(choseOnDispatch ? "" : "chosen off the dispatch thread, ") + std::to_string(a.delegate.eventsWith("hosted-message:").size()) + " messages heard");
    // From the choice's report, its chance to finish: behind it on A's thread, a job that tells it did.
    a.client->thread().async([&choiceDone] { choiceDone = true; });
    const bool finished = eventually([&] { return choiceDone.load() || !a.delegate.deaths().empty(); }, 0.5);
    CHECK_MSG(!finished, "the end chose while the rejection was in its step: " + joined(a.delegate.deaths()) + (choiceDone ? ", its job done" : ""));
    step.open();
    CHECK_MSG(eventually([&] { return !a.delegate.deaths().empty(); }), joined(a.delegate.eventsWith("died:")));
    CHECK_MSG(a.delegate.deaths() == std::vector<std::string>{flooded} && a.channel->death() == SandboxHostedChannel::Death::Ordinary,
              joined(a.delegate.deaths()) + ", " + deathName(a.channel->death()));
    CHECK_MSG(a.delegate.eventsWith("hosted-request:").empty() && b.death() == "ordinary", joined(a.delegate.eventsWith("hosted-request:")) + " B " + b.death());
    CHECK_MSG(eventually([&] { return a.client->remoteBacklog().calls == 0; }), std::to_string(a.client->remoteBacklog().calls) + " still held");
    CHECK_MSG(eventually([&] { return choiceDone.load(); }) && heldOpened && stepOpened, std::string(heldOpened ? "" : "A's thread's gate ran out, ") + (stepOpened ? "" : "the rejection's gate ran out, ") + (choiceDone ? "" : "the choice's job never finished"));
}

/// The owner's withdrawal (`SandboxHostedChannel::withdraw`) is one step with the stream's first end. Before the end: A, withdrawn with a timed message unanswered, is out of the stream's decisions, so 3.2 s later nothing ended, a later message of A's is still carried, and at the engine process's end B, idle, keeps its ordinary death (no route with a message unanswered took part), while A's withdrawal hears nothing. After the end: a withdrawal of a route whose death settled reads it at once (A's ordinary, a message unanswered; B's spared; one attached after the end, spared). One whose client's dispatch thread has not run the end yet hears its decision later, once, on that thread, before the delegate hears the death; one whose owner's stop ran on that thread before the end hears the end's decision as the route goes; one whose own flood came before the end hears `Ordinary`. A channel that decides nothing (the embedder's own) has the withdrawal first and calls nothing back.
TEST(remote_stream_hosted_answers_withdraw_in_one_step_with_the_end) {
    using Death = SandboxHostedChannel::Death;
    const std::string closed = "the engine process closed the stream";
    const std::string flooded = "the widget engine process sent more than the host keeps queued";
    // What the withdrawals hear, from the clients' threads, declared before them.
    Withdrawals withdrawals;
    {
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        AnsweringClient a(engine, 1), b(engine, 2);
        a.client->platform("ask:1");
        a.settle();
        CHECK_MSG(eventually([&] { return engine.saw(1, "platform:ask:1"); }), joined(engine.frames(1)));
        const SandboxHostedChannel::Withdrawal withdrawn = a.channel->withdraw(withdrawals.hearing("a", a));
        CHECK(!withdrawn.ended && !withdrawn.death);
        sleepMs(3200);
        CHECK_MSG(!engine.events().ended(), "a withdrawn route's time counted: " + joined(engine.events().all()));
        a.client->platform("ask:2");
        a.settle();
        CHECK_MSG(eventually([&] { return engine.saw(1, "platform:ask:2"); }), joined(engine.frames(1)));
        engine.close();
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), closed), joined(engine.events().all()));
        CHECK_MSG(a.death() == "ordinary" && b.death() == "ordinary", a.death() + " " + b.death());
        const SandboxHostedChannel::Withdrawal again = a.channel->withdraw(withdrawals.hearing("a again", a));
        CHECK(!again.ended && !again.death);
        CHECK_MSG(withdrawals.told().empty(), joined(withdrawals.told()));
    }
    {
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        AnsweringClient a(engine, 1), b(engine, 2);
        a.client->platform("ask:1");
        a.settle();
        CHECK_MSG(eventually([&] { return engine.saw(1, "platform:ask:1"); }), joined(engine.frames(1)));
        engine.close();
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }), joined(engine.events().all()));
        CHECK_MSG(a.death() == "ordinary" && b.death() == "spared", a.death() + " " + b.death());
        const SandboxHostedChannel::Withdrawal wa = a.channel->withdraw(withdrawals.hearing("a", a)), wb = b.channel->withdraw(withdrawals.hearing("b", b));
        CHECK(wa.ended && wa.death == std::optional<Death>(Death::Ordinary) && wb.ended && wb.death == std::optional<Death>(Death::Spared));
        AnsweringClient attached(engine, 0);
        CHECK_MSG(attached.death() == "spared", attached.death());
        const SandboxHostedChannel::Withdrawal late = attached.channel->withdraw(withdrawals.hearing("attached", attached));
        CHECK(late.ended && late.death == std::optional<Death>(Death::Spared));
        CHECK_MSG(withdrawals.told().empty(), joined(withdrawals.told()));
    }
    {
        // B's, C's and D's dispatch threads wait on them: declared before the clients that join those threads.
        Gate heldB, heldC, heldD;
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        AnsweringClient a(engine, 1), b(engine, 2), c(engine, 3), d(engine, 4);
        auto barrier = std::make_shared<StreamSink>();
        const std::shared_ptr<SandboxHostedChannel> barrierRoute = engine.host().hostedChannel();
        barrierRoute->attach(barrier);
        a.client->platform("ask:1");
        for (AnsweringClient* client : {&a, &b, &c, &d}) client->settle();
        b.client->thread().async([&heldB] { heldB.wait(); });
        c.client->thread().async([&heldC] { heldC.wait(); });
        d.client->thread().async([&heldD] { heldD.wait(); });
        CHECK(heldB.waitedOn() && heldC.waitedOn() && heldD.waitedOn());
        // C's owner stops it, queued on C's thread before the end; D floods, its kill queued on D's thread before the end.
        c.client->terminate();
        std::string words;
        for (size_t i = 0; i <= SandboxEngine::kRemoteQueuedCalls; i++) words += platformMessageFrame(4, "s");
        CHECK(engine.send(words + logFrame(5, "said")));
        CHECK_MSG(eventually([&] { return barrier->saw("log:log:said") && engine.saw(1, "platform:ask:1"); }), joined(barrier->events()));
        engine.close();
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), closed), joined(engine.events().all()));
        const SandboxHostedChannel::Withdrawal wb = b.channel->withdraw(withdrawals.hearing("b", b, [&b] { return std::string(b.delegate.deaths().empty() ? " before its death" : " after its death"); }));
        const SandboxHostedChannel::Withdrawal wc = c.channel->withdraw(withdrawals.hearing("c", c));
        const SandboxHostedChannel::Withdrawal wd = d.channel->withdraw(withdrawals.hearing("d", d));
        CHECK(wb.ended && !wb.death && wc.ended && !wc.death && wd.ended && !wd.death);
        CHECK_MSG(withdrawals.told().empty() && a.death() == "ordinary", joined(withdrawals.told()) + " " + a.death());
        heldB.open();
        heldC.open();
        heldD.open();
        CHECK_MSG(b.death() == "spared" && b.delegate.deaths() == std::vector<std::string>{kEngineGone}, b.death() + ": " + joined(b.delegate.events()));
        CHECK_MSG(c.death() == "ordinary" && c.delegate.deaths() == std::vector<std::string>{"terminated"}, c.death() + ": " + joined(c.delegate.events()));
        CHECK_MSG(d.death() == "ordinary" && d.delegate.deaths() == std::vector<std::string>{flooded}, d.death() + ": " + joined(d.delegate.deaths()));
        CHECK_MSG(eventually([&] { return withdrawals.told().size() == 3; }), joined(withdrawals.told()));
        sleepMs(100);
        std::vector<std::string> all = withdrawals.told();
        std::sort(all.begin(), all.end());
        CHECK_MSG((all == std::vector<std::string>{"b:spared before its death on its dispatch thread", "c:spared on its dispatch thread", "d:ordinary on its dispatch thread"}), joined(all));
    }
    // A recorder of its own, declared before the channel: the withdrawal that comes first calls nothing back.
    Withdrawals ownHeard;
    OwnHostedChannel own("its own channel");
    const SandboxHostedChannel::Withdrawal first = own.withdraw(ownHeard.hearing("own"));
    CHECK_MSG(!first.ended && !first.death && ownHeard.told().empty(), std::string(first.ended ? "ended " : "first ") + (first.death ? deathName(*first.death) : "none") + ": " + joined(ownHeard.told()));
}

/// A death of the widget's own that its client reported first stays its death through the stream's end and the route's cleanup, `SandboxHostedChannel::withdraw` agreeing with `death()` on it, and that dead widget alone never ends its siblings' stream. X: A floods with a timed message unanswered (the overflow claimed at the reader, the kill behind A's held dispatch thread, the message sent just before that thread goes on), and its delegate holds that death before its client closes the route: 3.4 s after the message nothing ended, A's withdrawal comes first (no end decided it) and its `death()` is `Ordinary`; let go, A's route closes and idle B's message still goes out. Y: idle A floods the same way, then B sends a timed message, whose time runs out while A's delegate still holds A's death: B is stalled, A's withdrawal reads `Ordinary` at once, as its `death()` does, not the spared an idle route would be. Z: the end (a queue past its bound from this thread while the reader is held in a delivery) comes while A's platform request waits for its delegate and A's flood waits behind A's held dispatch thread; the delegate then answers none, so A's `widget.answer` loses the still open route before the reader's fan-out, a loss its client still hears: A's flood comes first on its thread, and A's withdrawal and `death()` read `Ordinary`, not the end's spared, while B keeps its ordinary death. P: A's renderer's platform request, which A's delegate answers with none (no platform), is A's own death claimed before its diagnostic (`platform-timeout`) reaches that delegate: with A's timed input unanswered, A's delegate holding the diagnostic and A's `widget.answer`, its client's answer given, held before it loses the route, A reads decided, and at the engine process's end idle B, the only route that takes part, keeps its ordinary death (not the spared beside a route with a message unanswered) and A's withdrawal reads `Ordinary` at once; let go, A dies of its platform once.
TEST(remote_stream_hosted_answers_claim_an_own_death_in_one_step_with_the_end) {
    using Death = SandboxHostedChannel::Death;
    using Clock = std::chrono::steady_clock;
    const std::string flooded = "the widget engine process sent more than the host keeps queued";
    const auto overflowing = [] {
        std::string words;
        for (size_t i = 0; i <= SandboxEngine::kRemoteQueuedCalls; i++) words += platformMessageFrame(1, "s");
        return words;
    }();
    // What the withdrawals hear, from the clients' threads, declared before them.
    Withdrawals withdrawals;
    {
        // A's dispatch thread and A's death wait on them in each stream: declared before the clients that join those threads.
        Gate xHeld, xDying, yHeld, yDying;
        ScriptedEngine x({}, true, {answerKeys}), y({}, true, {answerKeys});
        CHECK(x.ready() && y.ready());
        AnsweringClient xa(x, 1), xb(x, 2), ya(y, 1), yb(y, 2);
        auto xBarrier = std::make_shared<StreamSink>(), yBarrier = std::make_shared<StreamSink>();
        const std::shared_ptr<SandboxHostedChannel> xBarrierRoute = x.host().hostedChannel(), yBarrierRoute = y.host().hostedChannel();
        xBarrierRoute->attach(xBarrier);
        yBarrierRoute->attach(yBarrier);
        for (AnsweringClient* c : {&xa, &xb, &ya, &yb}) c->settle();
        // Each A's flood waits behind its held dispatch thread, its overflow already claimed at the reader; its delegate's hold on that death is in place before the hold of its thread, so before anything can queue the flood.
        const auto queueFlood = [&](ScriptedEngine& engine, AnsweringClient& a, StreamSink& barrier, Gate& held, Gate& dying) {
            a.delegate.duringDeath = [&dying](SandboxEngine&) { dying.wait(); };
            a.client->thread().async([&held] { held.wait(); });
            CHECK(held.waitedOn());
            CHECK(engine.send(overflowing + logFrame(3, "flooded")));
            CHECK_MSG(eventually([&] { return barrier.saw("log:log:flooded"); }), joined(barrier.events()));
        };
        queueFlood(x, xa, *xBarrier, xHeld, xDying);
        queueFlood(y, ya, *yBarrier, yHeld, yDying);
        // X's A sends its timed message (on its route, its client's thread held), then each A's client claims its flood and its delegate holds that death before the route closes.
        const Clock::time_point sent = Clock::now();
        CHECK(xa.channel->platform("ask:1"));
        xHeld.open();
        yHeld.open();
        CHECK_MSG(xDying.waitedOn() && xa.delegate.deaths() == std::vector<std::string>{flooded}, joined(xa.delegate.eventsWith("died:")));
        CHECK_MSG(yDying.waitedOn() && ya.delegate.deaths() == std::vector<std::string>{flooded}, joined(ya.delegate.eventsWith("died:")));
        const double claimed = msSince(sent);
        CHECK_MSG(claimed < 2500, "the claims took " + std::to_string(claimed) + " ms");
        // Y's B sends once Y's A is dead; its time runs out while A's delegate still holds that death.
        yb.client->platform("ask:1");
        yb.settle();
        CHECK_MSG(eventually([&] { return y.events().ended().has_value(); }) && endsOnce(y.events().all(), kUnanswered), joined(y.events().all()));
        std::this_thread::sleep_until(sent + std::chrono::milliseconds(3400));
        CHECK_MSG(!x.events().ended(), "dead A's message ended its stream: " + joined(x.events().all()));
        CHECK_MSG(yb.death() == "stalled", yb.death());
        const SandboxHostedChannel::Withdrawal xw = xa.channel->withdraw(withdrawals.hearing("x", xa)), yw = ya.channel->withdraw(withdrawals.hearing("y", ya));
        CHECK_MSG(!xw.ended && !xw.death && xa.channel->death() == Death::Ordinary, std::string(xw.ended ? "ended " : "first ") + (xw.death ? deathName(*xw.death) : "none"));
        CHECK_MSG(yw.ended && yw.death == std::optional<Death>(Death::Ordinary) && ya.channel->death() == Death::Ordinary,
                  std::string(yw.ended ? "ended " : "first ") + (yw.death ? deathName(*yw.death) : "none") + ", death() " + deathName(ya.channel->death()));
        xDying.open();
        yDying.open();
        CHECK_MSG(eventually([&] { return x.saw(1, "close"); }), joined(x.frames(1)));
        xb.client->platform("size:1");
        xb.settle();
        CHECK_MSG(eventually([&] { return x.saw(2, "platform:size:1"); }) && !x.events().ended() && xb.delegate.deaths().empty(), joined(x.events().all()));
        const SandboxHostedChannel::Withdrawal again = ya.channel->withdraw(withdrawals.hearing("y again", ya));
        CHECK(again.ended && again.death == std::optional<Death>(Death::Ordinary));
        CHECK_MSG(withdrawals.told().empty(), joined(withdrawals.told()));
    }
    {
        SandboxRemoteHostEnd::Limits bounded;
        bounded.queuedBytes = size_t(1) << 20;
        // The reader and A's dispatch thread wait on them: declared before the stream and the client that join those threads.
        Gate delivering, held;
        ScriptedEngine engine(bounded, true, {answerKeys});
        CHECK(engine.ready());
        auto busy = std::make_shared<StreamSink>();
        busy->logGate = &delivering;
        const std::shared_ptr<SandboxHostedChannel> busyRoute = engine.host().hostedChannel();
        busyRoute->attach(busy);
        AnsweringClient a(engine, 2), b(engine, 3);
        b.client->platform("ask:1");
        b.settle();
        // A's renderer asks; its delegate keeps the responder.
        CHECK(engine.send(platformRequestFrame(2, "asks")));
        CHECK_MSG(eventually([&] { return has(a.delegate.events(), "hosted-request:asks"); }), joined(a.delegate.events()));
        a.client->thread().async([&held] { held.wait(); });
        CHECK(held.waitedOn());
        std::string words;
        for (size_t i = 0; i <= SandboxEngine::kRemoteQueuedCalls; i++) words += platformMessageFrame(2, "s");
        CHECK(engine.send(words + logFrame(1, "hold")));
        CHECK_MSG(delivering.waitedOn() && a.client->remoteBacklog().calls == SandboxEngine::kRemoteQueuedCalls, std::to_string(a.client->remoteBacklog().calls) + " held");
        // The end from this thread, the reader still in that delivery: A, idle, is frozen spared.
        CHECK(!busyRoute->platform(std::string(size_t(2) << 20, 'p')));
        // None for A's request: A's `widget.answer` loses the route, still live, which a probe then finds gone.
        std::optional<HostedRequestResponder> responder = a.delegate.platformResponder(0);
        CHECK(responder.has_value());
        if (responder) responder->unanswered();
        Background<std::string> probing([&] { return a.channel->probe("x"); });
        CHECK_MSG(probing.within(2) == std::optional<std::string>("ERROR no runtime"), "A's route was not lost");
        held.open();
        CHECK_MSG(eventually([&] { return !a.delegate.deaths().empty(); }), joined(a.delegate.eventsWith("died:")));
        CHECK_MSG(a.delegate.deaths() == std::vector<std::string>{flooded} && a.channel->death() == Death::Ordinary, joined(a.delegate.deaths()) + ", " + deathName(a.channel->death()));
        const SandboxHostedChannel::Withdrawal wa = a.channel->withdraw(withdrawals.hearing("z", a));
        CHECK_MSG(wa.ended && wa.death == std::optional<Death>(Death::Ordinary), std::string(wa.ended ? "ended " : "first ") + (wa.death ? deathName(*wa.death) : "none"));
        delivering.open();
        const std::string reason = "more than 1048576 bytes queued for the stream";
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), reason), joined(engine.events().all()));
        CHECK_MSG(b.death() == "ordinary", b.death());
        probing.finish(15);
        CHECK_MSG(withdrawals.told().empty(), joined(withdrawals.told()));
    }
    {
        const std::string none = "the host's platform answers no requests of the widget's renderer";
        // A's `widget.answer`, its client's answer given, and A's diagnostic wait on them: declared before the stream and the clients that join those threads.
        Gate answering, diagnosing;
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        const auto heldRoute = std::make_shared<HeldSinkChannel>(engine.host().hostedChannel(), "none", nullptr, &answering);
        AnsweringClient a(heldRoute, 1), b(engine, 2);
        const OpensAtExit opens{&answering, &diagnosing};
        a.delegate.holdPlatformRequests = false;
        a.delegate.platformAnswer = std::nullopt;
        a.delegate.duringDiag = [&diagnosing](SandboxEngine&, const std::string& kind) {
            if (kind == "platform-timeout") diagnosing.wait();
        };
        a.client->platform("ask:1");
        for (AnsweringClient* c : {&a, &b}) c->settle();
        CHECK_MSG(eventually([&] { return engine.saw(1, "platform:ask:1"); }), joined(engine.frames(1)));
        // A's renderer asks and A's delegate has no platform for it: A's delegate holds that diagnostic while A's `widget.answer`, the answer given, has yet to lose A's route.
        CHECK(engine.send(platformRequestFrame(1, "none")));
        CHECK_MSG(diagnosing.waitedOn() && answering.waitedOn() && has(heldRoute->requests(), "none:returned"), joined(heldRoute->requests()) + " / " + joined(a.delegate.events()));
        CHECK_MSG(a.channel->decided() && a.delegate.deaths().empty(), std::string(a.channel->decided() ? "decided: " : "undecided: ") + joined(a.delegate.events()));
        // The engine process's end with A's input still kept on its open route: A takes no part, so idle B is the only route that does, and keeps its ordinary death.
        engine.close();
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), "the engine process closed the stream"), joined(engine.events().all()));
        CHECK_MSG(b.death() == "ordinary", b.death());
        const SandboxHostedChannel::Withdrawal wa = a.channel->withdraw(withdrawals.hearing("p", a));
        CHECK_MSG(wa.ended && wa.death == std::optional<Death>(Death::Ordinary), std::string(wa.ended ? "ended " : "first ") + (wa.death ? deathName(*wa.death) : "none"));
        diagnosing.open();
        answering.open();
        CHECK_MSG(eventually([&] { return !a.delegate.deaths().empty(); }), joined(a.delegate.events()));
        CHECK_MSG(a.delegate.deaths() == std::vector<std::string>{none} && a.delegate.eventsWith("diag:platform-timeout").size() == 1, joined(a.delegate.events()));
        CHECK_MSG(withdrawals.told().empty(), joined(withdrawals.told()));
    }
}

/// What a local stop found (`SandboxHostedChannel::localStop`) or a withdrawal read, "none" for none.
static std::string deathOrNone(const std::optional<SandboxHostedChannel::Death>& death) { return death ? deathName(*death) : "none"; }

/// The host's own stop of a widget (`SandboxHostedChannel::localStop`, which a live platform end that refuses the widget's renderer makes from its own thread, here the test's) is one step with the stream's first end. X0, the control: A's frame request admitted while A is visible, its renderer's reply of another sequence (a valid key that answers nothing) heard and no stop: the stream ends 3 s after the admission, A stalled and idle B spared. X: the same, then, A's client held as a stop's notification that stalls, the host stops A within 2.5 s: 3.4 s after the admission nothing ended, B's input sent past A's time is answered on the same stream, A's route still carries and nothing closed it, and a second stop finds the first one's; let go, at the engine process's end with B's input unanswered A keeps its own ordinary death (not the spared of an idle route), which its withdrawal reads too, and a stop still finds the first one's. Y, the end first: A's frame request runs out while A's client is held before it heard that end, A stalled; the stop then finds that decision and changes nothing, twice: the withdrawal registered before it hears stalled once, A's client publishes stalled and dies of the end once, B is spared, and a stop afterwards still finds stalled. Z: idle A stopped before the engine process's end, with B's input unanswered and idle C not stopped: A ordinary, its withdrawal too, C spared, both withdrawals read at once and neither called back, also once the clients and the stream went. A channel that decides nothing (the embedder's own, a refusing one) has the stop first.
TEST(remote_stream_hosted_answers_stop_a_widget_locally_in_one_step_with_the_end) {
    using Death = SandboxHostedChannel::Death;
    using Clock = std::chrono::steady_clock;
    const std::string closed = "the engine process closed the stream";
    // What the withdrawals hear, from the clients' threads, declared before them.
    Withdrawals withdrawals;
    {
        // X's and Y's A dispatch threads wait on them: declared before the clients that join those threads.
        Gate xHeld, yHeld;
        ScriptedEngine x0({}, true, {answerKeys}), x({}, true, {answerKeys}), y({}, true, {answerKeys});
        CHECK(x0.ready() && x.ready() && y.ready());
        AnsweringClient x0a(x0, 1), x0b(x0, 2), xa(x, 1), xb(x, 2), ya(y, 1), yb(y, 2);
        const OpensAtExit opens{&xHeld, &yHeld};
        for (AnsweringClient* c : {&x0a, &x0b, &xa, &xb, &ya, &yb}) c->settle();
        // Each A's frame request admitted while A is visible; X0's and X's renderers reply with another sequence, which their clients hear and which answers nothing.
        const Clock::time_point sent = Clock::now();
        CHECK(x0a.channel->platform("draw:7:1") && xa.channel->platform("draw:7:1") && ya.channel->platform("draw:7:1"));
        CHECK(x0.send(platformMessageFrame(1, "reply:7:2")) && x.send(platformMessageFrame(1, "reply:7:2")));
        CHECK_MSG(eventually([&] { return has(x0a.delegate.events(), "hosted-message:reply:7:2") && has(xa.delegate.events(), "hosted-message:reply:7:2"); }),
                  joined(x0a.delegate.events()) + " / " + joined(xa.delegate.events()));
        // Y's A held before it hears anything more; X's A held as a stop's notification that stalls, then X's host stops A.
        ya.client->thread().async([&yHeld] { yHeld.wait(); });
        xa.client->thread().async([&xHeld] { xHeld.wait(); });
        CHECK(yHeld.waitedOn() && xHeld.waitedOn());
        const std::optional<Death> stopped = xa.channel->localStop();
        const double stoppedAfter = msSince(sent);
        CHECK_MSG(!stopped && stoppedAfter < 2500 && xa.channel->decided() && xa.channel->death() == Death::Ordinary, deathOrNone(stopped) + " after " + std::to_string(stoppedAfter) + " ms");
        CHECK(xb.channel->platform("ask:1") && x.send(platformMessageFrame(2, "reply:1")));
        CHECK_MSG(eventually([&] { return has(xb.delegate.events(), "hosted-message:reply:1"); }), joined(xb.delegate.events()));
        // X0 and Y end at A's time.
        CHECK_MSG(eventually([&] { return x0.events().ended() && y.events().ended(); }) && endsOnce(x0.events().all(), kUnanswered) && endsOnce(y.events().all(), kUnanswered),
                  joined(x0.events().all()) + " / " + joined(y.events().all()));
        const double x0Ended = std::chrono::duration<double, std::milli>(x0.events().endedAt().value_or(sent) - sent).count();
        CHECK_MSG(x0Ended >= 3000 && x0a.death() == "stalled" && x0b.death() == "spared", "X0 ended " + std::to_string(x0Ended) + " ms after the request: A " + x0a.death() + ", B " + x0b.death());
        // Y: the end decided A first; the stop changes nothing of that, the withdrawal waiting for it kept.
        const SandboxHostedChannel::Withdrawal yw = ya.channel->withdraw(withdrawals.hearing("y", ya));
        const std::optional<Death> yStopped = ya.channel->localStop(), yAgain = ya.channel->localStop();
        CHECK_MSG(yw.ended && !yw.death && yStopped == std::optional<Death>(Death::Stalled) && yAgain == yStopped && ya.channel->decided(), deathOrNone(yStopped) + ", again " + deathOrNone(yAgain));
        CHECK_MSG(ya.channel->death() == Death::Ordinary && ya.delegate.deaths().empty() && withdrawals.told().empty(), deathName(ya.channel->death()) + ": " + joined(ya.delegate.events()) + " " + joined(withdrawals.told()));
        // 3.4 s: X's A owes nothing: no end, B answered past A's time, A's route still carries.
        std::this_thread::sleep_until(sent + std::chrono::milliseconds(3400));
        CHECK_MSG(!x.events().ended(), "a stopped widget's frame request ended its stream: " + joined(x.events().all()));
        CHECK(xb.channel->platform("ask:2") && x.send(platformMessageFrame(2, "reply:2")) && xa.channel->platform("size:1"));
        CHECK_MSG(eventually([&] { return has(xb.delegate.events(), "hosted-message:reply:2") && x.saw(1, "platform:size:1"); }) && !x.events().ended() && !x.saw(1, "close"),
                  joined(x.events().all()) + " / " + joined(x.frames(1)));
        const std::optional<Death> xAgain = xa.channel->localStop();
        CHECK_MSG(!xAgain && xa.delegate.deaths().empty(), deathOrNone(xAgain) + ": " + joined(xa.delegate.events()));
        // Y let go: A's client publishes the end's decision and dies of it once, the withdrawal hearing it once.
        yHeld.open();
        CHECK_MSG(ya.death() == "stalled" && ya.delegate.deaths() == std::vector<std::string>{kEngineGone} && yb.death() == "spared", ya.death() + ": " + joined(ya.delegate.events()) + ", B " + yb.death());
        CHECK_MSG(eventually([&] { return !withdrawals.told().empty(); }) && ya.channel->localStop() == std::optional<Death>(Death::Stalled), joined(withdrawals.told()));
        // X let go, then the engine process's end with B's input unanswered: A's own death stands.
        xHeld.open();
        xa.settle();
        CHECK(xb.channel->platform("ask:3"));
        CHECK_MSG(eventually([&] { return x.saw(2, "platform:ask:3"); }), joined(x.frames(2)));
        x.close();
        CHECK_MSG(eventually([&] { return x.events().ended().has_value(); }) && endsOnce(x.events().all(), closed), joined(x.events().all()));
        CHECK_MSG(xa.death() == "ordinary" && xa.delegate.deaths() == std::vector<std::string>{kEngineGone} && xb.death() == "ordinary", xa.death() + ": " + joined(xa.delegate.events()) + ", B " + xb.death());
        const SandboxHostedChannel::Withdrawal xw = xa.channel->withdraw(withdrawals.hearing("x", xa));
        const std::optional<Death> xLast = xa.channel->localStop();
        CHECK_MSG(xw.ended && xw.death == std::optional<Death>(Death::Ordinary) && !xLast, std::string(xw.ended ? "ended " : "first ") + deathOrNone(xw.death) + ", then " + deathOrNone(xLast));
        sleepMs(100);
        CHECK_MSG(withdrawals.told() == std::vector<std::string>{"y:stalled on its dispatch thread"}, joined(withdrawals.told()));
    }
    {
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        AnsweringClient a(engine, 1), b(engine, 2), c(engine, 3);
        for (AnsweringClient* client : {&a, &b, &c}) client->settle();
        const std::optional<Death> stopped = a.channel->localStop();
        CHECK_MSG(!stopped && a.channel->decided() && !c.channel->decided(), deathOrNone(stopped));
        CHECK(b.channel->platform("ask:1"));
        CHECK_MSG(eventually([&] { return engine.saw(2, "platform:ask:1"); }), joined(engine.frames(2)));
        engine.close();
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), closed), joined(engine.events().all()));
        CHECK_MSG(a.death() == "ordinary" && b.death() == "ordinary" && c.death() == "spared", a.death() + " " + b.death() + " " + c.death());
        const SandboxHostedChannel::Withdrawal wa = a.channel->withdraw(withdrawals.hearing("za", a)), wc = c.channel->withdraw(withdrawals.hearing("zc", c));
        CHECK_MSG(wa.ended && wa.death == std::optional<Death>(Death::Ordinary) && wc.ended && wc.death == std::optional<Death>(Death::Spared), deathOrNone(wa.death) + " " + deathOrNone(wc.death));
        CHECK(!a.channel->localStop() && c.channel->localStop() == std::optional<Death>(Death::Spared));
    }
    // Each death settled when withdrawn (X's A, Z's A and C) was read at once: none of those callbacks was ever called, then or as the clients, their channels and the streams went.
    CHECK_MSG(withdrawals.told() == std::vector<std::string>{"y:stalled on its dispatch thread"}, joined(withdrawals.told()));
    OwnHostedChannel own("its own channel");
    CHECK(!own.localStop() && !own.decided());
    CHECK(!SandboxRemoteHostEnd::refusingHosted("refused")->localStop());
}

/// A hosted client's counted stop (`SandboxEngine::stop`) is the widget's own death in one step with the stream's first end, decided on the client's dispatch thread before its delegate hears it (`SandboxHostedChannel::localStop`), and it yields to an end that decided the widget first. X: A's input admitted and left unanswered, A's client stops it from another thread and its delegate holds that death inside `onDied`, before the route closes: the stop was decided within 2.5 s, 3.4 s after the admission nothing ended, B's input sent past A's time is answered on the same stream, A's withdrawal comes first (no end decided it) and its `death()` is `Ordinary`; let go, A's route takes the stop and closes and B still carries. R, the end first: a storage commit of A's engine's side reaches A's delegate while A lives; then a job ahead of the end on A's dispatch thread waits while A's frame request runs out (A stalled, idle B spared) and the same commit is queued behind that job, then the job stops A inline: no death inside that job and nothing published, the commit queued after the stop reaches no delegate, then A's client publishes stalled and dies of the end once, the withdrawal registered before hears stalled once, on A's dispatch thread, B is spared, and a stop queued after that death changes nothing. Z: idle A stopped inline on its dispatch thread, its delegate holding that death, then the engine process's end with B's input unanswered and idle C not stopped: A's withdrawal reads its own ordinary death (not the spared of an idle route), B is ordinary and C spared; let go, A's client reported its stop alone. T: the owner's `terminate` claims nothing: while its delegate holds that death, with A's input unanswered, A is not decided. O: on a hosting stream opened without the owner's answers the same counted stop of a widget with an input sent decides nothing, before and after the stop ran (`decided()` false, `death()` `Ordinary`), where a stream opened with them decides it (`Ordinary`); the owner's withdrawal comes first on both, no end having decided either.
TEST(remote_stream_hosted_answers_claim_a_counted_stop_in_one_step_with_the_end) {
    using Death = SandboxHostedChannel::Death;
    using Clock = std::chrono::steady_clock;
    const std::string counted = "a counted stop";
    const std::string closed = "the engine process closed the stream";
    // What the withdrawals hear, from the clients' threads, declared before them.
    Withdrawals withdrawals;
    {
        // What R's job saw right after its inline stop, then the gates X's A death and R's A dispatch thread wait on: declared before the clients that join those threads.
        std::atomic<bool> stoppedInline{false};
        std::vector<std::string> deathsInline;
        Death publishedInline = Death::Spared;
        Gate xDying, rHeld;
        ScriptedEngine x({}, true, {answerKeys}), r({}, true, {answerKeys});
        CHECK(x.ready() && r.ready());
        AnsweringClient xa(x, 1), xb(x, 2), ra(r, 1), rb(r, 2);
        const OpensAtExit opens{&xDying, &rHeld};
        for (AnsweringClient* c : {&xa, &xb, &ra, &rb}) c->settle();
        xa.delegate.duringDeath = [&xDying](SandboxEngine&) { xDying.wait(); };
        // R's A, live: a storage commit of its engine's side reaches its delegate.
        CHECK(r.send(storageFrame(1)));
        CHECK_MSG(eventually([&] { return ra.delegate.eventsWith("storage:").size() == 1; }), joined(ra.delegate.events()));
        // R's job, ahead of any end on A's dispatch thread: let go, it stops A inline and notes what that did at once.
        ra.client->thread().async([&] {
            rHeld.wait();
            ra.client->stop(counted);
            deathsInline = ra.delegate.deaths();
            publishedInline = ra.channel->death();
            stoppedInline = true;
        });
        CHECK(rHeld.waitedOn());
        // X's input and R's frame request admitted; X's A stopped from this thread.
        const Clock::time_point sent = Clock::now();
        CHECK(xa.channel->platform("ask:1") && ra.channel->platform("draw:7:1"));
        // The same commit for R's A, queued behind that job, so after its stop and before the end's kill: admitted and not yet run.
        CHECK(r.send(storageFrame(1)));
        CHECK_MSG(eventually([&] { return ra.client->remoteBacklog().calls == 1; }), std::to_string(ra.client->remoteBacklog().calls) + " held");
        CHECK_MSG(eventually([&] { return x.saw(1, "platform:ask:1"); }), joined(x.frames(1)));
        xa.client->stop(counted);
        CHECK_MSG(xDying.waitedOn() && xa.delegate.deaths() == std::vector<std::string>{counted}, joined(xa.delegate.eventsWith("died:")));
        const double claimed = msSince(sent);
        CHECK_MSG(claimed < 2500 && xa.channel->decided() && xa.channel->death() == Death::Ordinary, std::string(xa.channel->decided() ? "decided" : "undecided") + " after " + std::to_string(claimed) + " ms");
        // R ends at A's time while A's client, held before that end, has its stop still to come.
        CHECK_MSG(eventually([&] { return r.events().ended().has_value(); }) && endsOnce(r.events().all(), kUnanswered), joined(r.events().all()));
        CHECK_MSG(ra.delegate.deaths().empty() && ra.channel->decided() && ra.channel->death() == Death::Ordinary, joined(ra.delegate.events()));
        const SandboxHostedChannel::Withdrawal rw = ra.channel->withdraw(withdrawals.hearing("r", ra));
        CHECK(rw.ended && !rw.death);
        rHeld.open();
        const bool inlineDone = eventually([&] { return stoppedInline.load(); });
        CHECK_MSG(inlineDone && deathsInline.empty() && publishedInline == Death::Ordinary, inlineDone ? "inside the job: " + joined(deathsInline) + ", " + deathName(publishedInline) : std::string("the job never stopped A"));
        CHECK_MSG(ra.death() == "stalled" && ra.delegate.deaths() == std::vector<std::string>{kEngineGone} && rb.death() == "spared", ra.death() + ": " + joined(ra.delegate.events()) + ", B " + rb.death());
        // The commit queued after the stop ran and reached nothing: the live one alone was stored.
        CHECK_MSG(eventually([&] { return ra.client->remoteBacklog().calls == 0; }) && ra.delegate.eventsWith("storage:") == std::vector<std::string>{"storage:local"},
                  joined(ra.delegate.events()) + ", " + std::to_string(ra.client->remoteBacklog().calls) + " held");
        CHECK_MSG(eventually([&] { return withdrawals.told().size() == 1; }) && !r.saw(1, "stop:" + counted), joined(withdrawals.told()) + " / " + joined(r.frames(1)));
        // Behind the end's kill: a widget dead already.
        ra.client->stop("later");
        ra.settle();
        CHECK_MSG(ra.delegate.deaths() == std::vector<std::string>{kEngineGone} && ra.channel->death() == Death::Stalled && ra.channel->localStop() == std::optional<Death>(Death::Stalled), joined(ra.delegate.events()));
        // 3.4 s: X's A owes nothing while its delegate holds its death.
        std::this_thread::sleep_until(sent + std::chrono::milliseconds(3400));
        CHECK_MSG(!x.events().ended(), "a stopped widget's input ended its stream: " + joined(x.events().all()));
        CHECK(xb.channel->platform("ask:1") && x.send(platformMessageFrame(2, "reply:1")));
        CHECK_MSG(eventually([&] { return has(xb.delegate.events(), "hosted-message:reply:1"); }) && !x.events().ended() && !x.saw(1, "close"), joined(x.events().all()) + " / " + joined(x.frames(1)));
        const SandboxHostedChannel::Withdrawal xw = xa.channel->withdraw(withdrawals.hearing("x", xa));
        CHECK_MSG(!xw.ended && !xw.death && xa.channel->death() == Death::Ordinary, std::string(xw.ended ? "ended " : "first ") + deathOrNone(xw.death));
        xDying.open();
        CHECK_MSG(eventually([&] { return x.saw(1, "stop:" + counted) && x.saw(1, "close"); }), joined(x.frames(1)));
        xb.client->platform("size:1");
        xb.settle();
        CHECK_MSG(eventually([&] { return x.saw(2, "platform:size:1"); }) && !x.events().ended() && xb.delegate.deaths().empty() && xa.delegate.deaths() == std::vector<std::string>{counted},
                  joined(x.events().all()) + " / " + joined(xa.delegate.deaths()));
        CHECK_MSG(withdrawals.told() == std::vector<std::string>{"r:stalled on its dispatch thread"}, joined(withdrawals.told()));
    }
    {
        // A's death waits on it: declared before the client that joins A's dispatch thread.
        Gate dying;
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        AnsweringClient a(engine, 1), b(engine, 2), c(engine, 3);
        const OpensAtExit opens{&dying};
        for (AnsweringClient* client : {&a, &b, &c}) client->settle();
        a.delegate.duringDeath = [&dying](SandboxEngine&) { dying.wait(); };
        a.client->thread().async([&a, &counted] { a.client->stop(counted); });
        CHECK_MSG(dying.waitedOn() && a.delegate.deaths() == std::vector<std::string>{counted} && a.channel->decided() && !c.channel->decided(), joined(a.delegate.events()));
        CHECK(b.channel->platform("ask:1"));
        CHECK_MSG(eventually([&] { return engine.saw(2, "platform:ask:1"); }), joined(engine.frames(2)));
        engine.close();
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), closed), joined(engine.events().all()));
        const SandboxHostedChannel::Withdrawal wa = a.channel->withdraw(withdrawals.hearing("za", a));
        CHECK_MSG(wa.ended && wa.death == std::optional<Death>(Death::Ordinary) && a.channel->death() == Death::Ordinary, std::string(wa.ended ? "ended " : "first ") + deathOrNone(wa.death));
        CHECK_MSG(b.death() == "ordinary" && c.death() == "spared", b.death() + " " + c.death());
        dying.open();
        a.settle();
        CHECK_MSG(a.death() == "ordinary" && a.delegate.deaths() == std::vector<std::string>{counted}, a.death() + ": " + joined(a.delegate.events()));
    }
    {
        // A's death waits on it: declared before the client that joins A's dispatch thread.
        Gate dying;
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        AnsweringClient a(engine, 1);
        const OpensAtExit opens{&dying};
        a.settle();
        a.delegate.duringDeath = [&dying](SandboxEngine&) { dying.wait(); };
        CHECK(a.channel->platform("ask:1"));
        a.client->terminate();
        CHECK_MSG(dying.waitedOn() && a.delegate.deaths() == std::vector<std::string>{"terminated"} && !a.channel->decided(), joined(a.delegate.events()));
        dying.open();
        CHECK_MSG(eventually([&] { return engine.saw(1, "close"); }) && !engine.events().ended() && !a.channel->decided(), joined(engine.frames(1)));
    }
    {
        // O: a hosting stream opened without the owner's answers (`startHosting`'s default) decides nothing, the same counted stop included; one opened with them decides it.
        ScriptedEngine plain({}, true), opted({}, true, {answerKeys});
        CHECK(plain.ready() && opted.ready());
        AnsweringClient p(plain, 1), o(opted, 1);
        for (AnsweringClient* client : {&p, &o}) {
            client->client->platform("ask:1");
            client->settle();
        }
        CHECK_MSG(eventually([&] { return plain.saw(1, "platform:ask:1") && opted.saw(1, "platform:ask:1"); }), joined(plain.frames(1)) + " / " + joined(opted.frames(1)));
        CHECK(!p.channel->decided() && !o.channel->decided() && p.channel->death() == Death::Ordinary && o.channel->death() == Death::Ordinary);
        p.client->stop(counted);
        o.client->stop(counted);
        p.settle();
        o.settle();
        CHECK_MSG(p.delegate.deaths() == std::vector<std::string>{counted} && !p.channel->decided() && p.channel->death() == Death::Ordinary,
                  std::string(p.channel->decided() ? "decided" : "undecided") + ", " + deathName(p.channel->death()) + ": " + joined(p.delegate.deaths()));
        CHECK_MSG(o.delegate.deaths() == std::vector<std::string>{counted} && o.channel->decided() && o.channel->death() == Death::Ordinary,
                  std::string(o.channel->decided() ? "decided" : "undecided") + ", " + deathName(o.channel->death()) + ": " + joined(o.delegate.deaths()));
        const SandboxHostedChannel::Withdrawal pw = p.channel->withdraw(withdrawals.hearing("op", p)), ow = o.channel->withdraw(withdrawals.hearing("oo", o));
        // Both withdrawals come first, no end having decided either (the opted-in claim was before any end).
        CHECK_MSG(!pw.ended && !pw.death && !ow.ended && !ow.death && !p.channel->localStop() && !p.channel->decided(),
                  std::string(pw.ended ? "ended " : "first ") + deathOrNone(pw.death) + ", " + (ow.ended ? "ended " : "first ") + deathOrNone(ow.death));
        CHECK_MSG(withdrawals.told() == std::vector<std::string>{"r:stalled on its dispatch thread"}, joined(withdrawals.told()));
    }
}

/// A counted stop that yielded to the stream's end (`SandboxEngine::stop` after an end that decided the widget first) leaves the widget's death to that end's kill, and until that kill its client is as at the stop's own death. S, the watchdog's end while the reader is held in a delivery of an earlier route's, so the routes stay open and no client hears that end: A's, K's and T's inputs run out (A's renderer's platform request came after its input was due, its responder kept; K's renderer announced itself), idle E's renderer's request is kept too, and each is stopped once that end decided it, nothing dying then. K's owner tries to keep its renderer and T's owner terminates it: the keep fails and nothing dies. Then A's and E's responders decline: each refusal's kill is queued on its client before its route, still open, is lost for the answer it never gets, so each client runs that kill before it hears the loss; with the reader still held A and E die of that loss once, with no `platform-timeout` diagnostic, their channels' `death()` the end's decision (A stalled, E spared). Let go, K and T die of the end once, stalled, K's delegate hearing that death and the one it offered nothing. Q, the engine process's end with B's input unanswered, its fan-out held at an earlier route before it reaches the clients: M's renderer's live message and request reach its delegate and I's live Intl request its provider; then another message and request of M's renderer and another Intl request of I's are admitted behind a job on each one's dispatch thread that, let go once that end decided both, stops it inline. Each queued call then runs on its client's thread, whenever its worker handed it over, while the client is yielded and before the end's kill: none of them reaches the delegate or the provider (the request answers none, with no kill) and nothing dies. Let go, M and I die of the end once, spared, and B is ordinary.
TEST(remote_stream_hosted_answers_leave_a_yielded_stop_to_the_ends_kill) {
    using Clock = std::chrono::steady_clock;
    const std::string counted = "a counted stop";
    {
        // The monitor's report and the reader's delivery at Z wait on them, and K's offered delegate must outlive its client: declared before the stream and the clients.
        Gate monitor, delivering;
        RemoteRecorder offered;
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        // A monitor that does not run until the test lets it look at the answer times, and Z, the route before the clients, whose log delivery holds the reader.
        engine.events().holdAt = 1;
        engine.events().hold = &monitor;
        const std::shared_ptr<SandboxHostedChannel> zRoute = engine.host().hostedChannel();
        auto z = std::make_shared<StreamSink>();
        z->logGate = &delivering;
        zRoute->attach(z);
        CHECK(monitor.waitedOn());
        AnsweringClient a(engine, 2), k(engine, 3), t(engine, 4), e(engine, 5);
        const OpensAtExit opens{&monitor, &delivering};
        for (AnsweringClient* c : {&a, &k, &t, &e}) c->settle();
        CHECK(engine.send(bare(3, wire::RendererReady)));
        CHECK_MSG(eventually([&] { return has(k.delegate.events(), "renderer-ready"); }), joined(k.delegate.events()));
        // A's, K's and T's inputs admitted; past their time A's and E's renderers ask the host's platform, which keeps both responders; then the reader waits in a delivery of Z's.
        const Clock::time_point sent = Clock::now();
        CHECK(a.channel->platform("ask:1") && k.channel->platform("ask:1") && t.channel->platform("ask:1"));
        std::this_thread::sleep_until(sent + std::chrono::milliseconds(3150));
        CHECK(engine.send(platformRequestFrame(2, "a asks") + platformRequestFrame(5, "e asks") + logFrame(1, "hold")));
        CHECK_MSG(eventually([&] { return a.delegate.platformResponder(0) && e.delegate.platformResponder(0); }) && delivering.waitedOn(), joined(a.delegate.events()) + " / " + joined(e.delegate.events()));
        // The monitor looks: the end decides every route, which stays open while the reader, held, has yet to tell any client of that end.
        monitor.open();
        CHECK_MSG(eventually([&] { return a.channel->decided() && k.channel->decided() && t.channel->decided() && e.channel->decided(); }), "a route the end did not decide");
        for (AnsweringClient* c : {&a, &k, &t, &e}) {
            c->client->stop(counted);
            c->settle();
        }
        // Each stop yielded; now a keep and a terminate before the end's kill.
        const bool kept = k.client->keepHosted(offered);
        t.client->terminate();
        t.settle();
        CHECK_MSG(!kept && k.delegate.deaths().empty() && t.delegate.deaths().empty(), std::string(kept ? "kept, " : "") + "K " + joined(k.delegate.events()) + " / T " + joined(t.delegate.events()));
        // A's and E's responders decline while the reader is still held: each worker queues its refusal's kill, then gives no answer on its route, still open, which loses that route, so the client hears the loss behind the kill.
        for (AnsweringClient* c : {&a, &e}) {
            std::optional<HostedRequestResponder> responder = c->delegate.platformResponder(0);
            if (responder) responder->unanswered();
        }
        CHECK_MSG(eventually([&] { return !a.delegate.deaths().empty() && !e.delegate.deaths().empty(); }), "A " + joined(a.delegate.events()) + " / E " + joined(e.delegate.events()));
        CHECK_MSG(a.death() == "stalled" && a.delegate.deaths() == std::vector<std::string>{kEngineGone} && a.delegate.eventsWith("diag:").empty(), a.death() + ": " + joined(a.delegate.events()));
        CHECK_MSG(e.death() == "spared" && e.delegate.deaths() == std::vector<std::string>{kEngineGone} && e.delegate.eventsWith("diag:").empty(), e.death() + ": " + joined(e.delegate.events()));
        for (AnsweringClient* c : {&k, &t}) c->settle();
        CHECK_MSG(!engine.events().ended() && k.delegate.deaths().empty() && t.delegate.deaths().empty(), joined(engine.events().all()) + " / K " + joined(k.delegate.events()) + " / T " + joined(t.delegate.events()));
        // Let go: the reader tells K and T of the end.
        delivering.open();
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), kUnanswered), joined(engine.events().all()));
        CHECK_MSG(k.death() == "stalled" && k.delegate.deaths() == std::vector<std::string>{kEngineGone} && offered.events().empty(), k.death() + ": " + joined(k.delegate.events()) + ", offered " + joined(offered.events()));
        CHECK_MSG(t.death() == "stalled" && t.delegate.deaths() == std::vector<std::string>{kEngineGone}, t.death() + ": " + joined(t.delegate.events()));
        for (AnsweringClient* c : {&a, &e}) c->settle();
        CHECK_MSG(a.delegate.deaths() == std::vector<std::string>{kEngineGone} && e.delegate.deaths() == std::vector<std::string>{kEngineGone} && a.delegate.eventsWith("diag:").empty() && e.delegate.eventsWith("diag:").empty(),
                  "A " + joined(a.delegate.events()) + " / E " + joined(e.delegate.events()));
    }
    {
        // The reader's fan-out at Z and M's and I's dispatch threads wait on them, and the jobs on those threads report here: declared before the stream and the clients that join those threads.
        Gate fanout, mHeld, iHeld;
        std::atomic<bool> mStopped{false}, iStopped{false};
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        // A fan-out that stops at Z, the route before the clients.
        const std::shared_ptr<SandboxHostedChannel> zRoute = engine.host().hostedChannel();
        auto z = std::make_shared<StreamSink>();
        z->lost = &fanout;
        zRoute->attach(z);
        AnsweringClient m(engine, 2), i(engine, 3), b(engine, 4);
        const OpensAtExit opens{&fanout, &mHeld, &iHeld};
        m.delegate.holdPlatformRequests = false;
        b.client->platform("ask:1");
        for (AnsweringClient* c : {&m, &i, &b}) c->settle();
        CHECK_MSG(eventually([&] { return engine.saw(4, "platform:ask:1"); }), joined(engine.frames(4)));
        // Live: M's renderer's message and request reach its delegate, which answers the request, and I's Intl request reaches its provider.
        CHECK(engine.send(platformMessageFrame(2, "live") + platformRequestFrame(2, "live") + intlFrame(3, "{\"live\":1}")));
        CHECK_MSG(eventually([&] { return engine.saw(2, "platform-answer:platform-answer") && engine.saw(3, "intl-answer:INTL"); }), joined(engine.frames(2)) + " / " + joined(engine.frames(3)));
        CHECK_MSG(has(m.delegate.events(), "hosted-message:live") && has(m.delegate.events(), "hosted-request:live") && i.intl->threads().size() == 1, joined(m.delegate.events()));
        // Jobs on M's and I's dispatch threads: let go, each stops its widget inline.
        m.client->thread().async([&] {
            mHeld.wait();
            m.client->stop(counted);
            mStopped = true;
        });
        i.client->thread().async([&] {
            iHeld.wait();
            i.client->stop(counted);
            iStopped = true;
        });
        CHECK(mHeld.waitedOn() && iHeld.waitedOn());
        // Admitted behind those jobs, each handed over to its client's thread whenever its worker gets to it.
        CHECK(engine.send(platformMessageFrame(2, "queued") + platformRequestFrame(2, "queued") + intlFrame(3, "{\"queued\":1}")));
        CHECK_MSG(eventually([&] { return m.client->remoteBacklog().calls == 2 && i.client->remoteBacklog().calls == 1; }),
                  std::to_string(m.client->remoteBacklog().calls) + " and " + std::to_string(i.client->remoteBacklog().calls) + " held");
        // The engine process's end decides M and I while its fan-out waits at Z, before any client hears it.
        engine.close();
        CHECK_MSG(fanout.waitedOn() && m.channel->decided() && i.channel->decided(), joined(engine.events().all()));
        // Let go, each job's stop yields to that end; what was queued runs while both clients are yielded and before either hears the end: none of it reaches the delegate or the provider (the request answers none, with no kill) and nothing dies.
        mHeld.open();
        iHeld.open();
        CHECK_MSG(eventually([&] { return mStopped && iStopped; }), "a job never stopped its widget");
        CHECK_MSG(eventually([&] { return m.client->remoteBacklog().calls == 0 && i.client->remoteBacklog().calls == 0; }),
                  std::to_string(m.client->remoteBacklog().calls) + " and " + std::to_string(i.client->remoteBacklog().calls) + " still held");
        CHECK_MSG(m.delegate.eventsWith("hosted-") == (std::vector<std::string>{"hosted-message:live", "hosted-request:live"}) && m.delegate.eventsWith("diag:").empty() && m.delegate.deaths().empty()
                      && i.intl->threads().size() == 1 && i.delegate.deaths().empty() && !engine.events().ended(),
                  joined(m.delegate.events()) + " / " + std::to_string(i.intl->threads().size()) + " formatted: " + joined(i.delegate.events()));
        // Let go: the reader tells M, I and B of the end, and each dies of it once.
        fanout.open();
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), "the engine process closed the stream"), joined(engine.events().all()));
        CHECK_MSG(m.death() == "spared" && i.death() == "spared" && b.death() == "ordinary", m.death() + " " + i.death() + " " + b.death());
        CHECK_MSG(m.delegate.eventsWith("hosted-") == (std::vector<std::string>{"hosted-message:live", "hosted-request:live"}) && m.delegate.deaths() == std::vector<std::string>{kEngineGone}
                      && m.delegate.eventsWith("diag:").empty(),
                  joined(m.delegate.events()));
        CHECK_MSG(i.intl->threads().size() == 1 && i.delegate.deaths() == std::vector<std::string>{kEngineGone}, std::to_string(i.intl->threads().size()) + " formatted: " + joined(i.delegate.events()));
    }
}

/// `SandboxHostedChannel::decided` tells, changing nothing, that the stream already decided a widget's death before its client published or reported it, where `death()` cannot: an `Ordinary` death reads alike before and after its publication. Live routes read false, and so does C once its owner withdrew it. At the engine process's end A (a message unanswered) and idle B, each held on its dispatch thread, read decided while neither client ran the end: no death reported, both `death()` still `Ordinary`; C, withdrawn before that end, still reads false. Let go, A is ordinary and B spared, both still decided. The owner's stop decides nothing: both routes read false after their deaths. D's flood, its kill held behind D's dispatch thread, reads false until D's client claims it, then decided while D's delegate holds that death, with `death()` `Ordinary` and the stream still going. A channel of the embedder's own reads false.
TEST(remote_stream_hosted_answers_tell_a_decided_death_before_it_is_published) {
    using Death = SandboxHostedChannel::Death;
    const std::string flooded = "the widget engine process sent more than the host keeps queued";
    {
        // A's and B's dispatch threads wait on them: declared before the clients that join those threads.
        Gate heldA, heldB;
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        AnsweringClient a(engine, 1), b(engine, 2), c(engine, 3);
        a.client->platform("ask:1");
        c.client->platform("ask:1");
        for (AnsweringClient* client : {&a, &b, &c}) client->settle();
        CHECK_MSG(eventually([&] { return engine.saw(1, "platform:ask:1") && engine.saw(3, "platform:ask:1"); }), joined(engine.frames(1)) + " / " + joined(engine.frames(3)));
        CHECK(!a.channel->decided() && !b.channel->decided() && !c.channel->decided());
        const SandboxHostedChannel::Withdrawal withdrawn = c.channel->withdraw([](Death) {});
        CHECK(!withdrawn.ended && !withdrawn.death && !c.channel->decided());
        a.client->thread().async([&heldA] { heldA.wait(); });
        b.client->thread().async([&heldB] { heldB.wait(); });
        CHECK(heldA.waitedOn() && heldB.waitedOn());
        engine.close();
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), "the engine process closed the stream"), joined(engine.events().all()));
        CHECK_MSG(a.delegate.deaths().empty() && b.delegate.deaths().empty(), joined(a.delegate.deaths()) + " / " + joined(b.delegate.deaths()));
        CHECK_MSG(a.channel->decided() && b.channel->decided() && a.channel->death() == Death::Ordinary && b.channel->death() == Death::Ordinary,
                  std::string(a.channel->decided() ? "A decided " : "A undecided ") + deathName(a.channel->death()) + (b.channel->decided() ? ", B decided " : ", B undecided ") + deathName(b.channel->death()));
        CHECK(!c.channel->decided());
        heldA.open();
        heldB.open();
        CHECK_MSG(a.death() == "ordinary" && b.death() == "spared", a.death() + " " + b.death());
        CHECK(a.channel->decided() && b.channel->decided() && !c.channel->decided());
    }
    {
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        AnsweringClient waiting(engine, 1), idle(engine, 2);
        waiting.client->platform("ask:1");
        waiting.settle();
        engine.host().stop();
        CHECK_MSG(waiting.death() == "ordinary" && idle.death() == "ordinary", waiting.death() + " " + idle.death());
        CHECK(!waiting.channel->decided() && !idle.channel->decided());
    }
    {
        // D's dispatch thread and D's death wait on them: declared before the client that joins that thread.
        Gate held, dying;
        ScriptedEngine engine({}, true, {answerKeys});
        CHECK(engine.ready());
        AnsweringClient d(engine, 1);
        auto barrier = std::make_shared<StreamSink>();
        const std::shared_ptr<SandboxHostedChannel> barrierRoute = engine.host().hostedChannel();
        barrierRoute->attach(barrier);
        // D's delegate holds its death from before the hold of its thread, so before anything can queue the flood.
        d.delegate.duringDeath = [&dying](SandboxEngine&) { dying.wait(); };
        d.settle();
        d.client->thread().async([&held] { held.wait(); });
        CHECK(held.waitedOn());
        std::string words;
        for (size_t i = 0; i <= SandboxEngine::kRemoteQueuedCalls; i++) words += platformMessageFrame(1, "s");
        CHECK(engine.send(words + logFrame(2, "flooded")));
        CHECK_MSG(eventually([&] { return barrier->saw("log:log:flooded"); }), joined(barrier->events()));
        CHECK(!d.channel->decided());
        held.open();
        CHECK_MSG(dying.waitedOn() && d.delegate.deaths() == std::vector<std::string>{flooded}, joined(d.delegate.eventsWith("died:")));
        CHECK_MSG(d.channel->decided() && d.channel->death() == Death::Ordinary && !engine.events().ended(), deathName(d.channel->death()) + ": " + joined(engine.events().all()));
        dying.open();
    }
    OwnHostedChannel own("its own channel");
    CHECK(!own.decided());
}

/// A platform request its client answers with none loses its accounted route at once, which no answer would ever end: its host time would never stop and no later message of the route would ever be timed. U, a hosted WidgetHost not yet started (no delegate to ask), and a bare started route whose sink answers none, each with a timed message kept: the engine process's request on each closes that route (`Close`), its client hears that the stream is lost, a later message is refused, and the stream and its idle sibling go on; U, started, hears that death once, `Ordinary`. N, whose delegate answers that it has no platform, dies of that first (`platform-timeout`, its own reason, `Ordinary`), and its route closes once.
TEST(remote_stream_hosted_answers_lose_a_route_left_without_an_answer) {
    std::mutex mutex;
    std::vector<std::string> heard;
    ScriptedEngine engine({}, true, {answerKeys});
    CHECK(engine.ready());
    const std::shared_ptr<SandboxHostedChannel> unstartedRoute = engine.host().hostedChannel();
    WidgetHost unstarted("<p>x</p>", WidgetTheme{}, 300, WidgetHostOptions(), unstartedRoute);
    auto bare = std::make_shared<StreamSink>();
    bare->platformAnswer = std::nullopt;
    const std::shared_ptr<SandboxHostedChannel> bareRoute = engine.host().hostedChannel();
    bareRoute->attach(bare);
    bareRoute->startHosted("<p>x</p>", testConfig(), SandboxHostedStart{});
    AnsweringClient none(engine, 3), sibling(engine, 4);
    none.delegate.holdPlatformRequests = false;
    none.delegate.platformAnswer = std::nullopt;
    CHECK(unstartedRoute->platform("ask:1") && bareRoute->platform("ask:1"));
    none.client->platform("ask:1");
    sibling.client->platform("size:1");
    none.settle();
    sibling.settle();
    CHECK_MSG(eventually([&] { return engine.saw(1, "platform:ask:1") && engine.saw(2, "platform:ask:1") && engine.saw(3, "platform:ask:1") && engine.saw(4, "platform:size:1"); }), joined(engine.frames(2)));
    CHECK(engine.send(platformRequestFrame(1, "early") + platformRequestFrame(2, "none") + platformRequestFrame(3, "none")));
    CHECK_MSG(eventually([&] { return engine.saw(1, "close") && engine.saw(2, "close") && engine.saw(3, "close"); }), joined(engine.frames(1)) + " / " + joined(engine.frames(2)) + " / " + joined(engine.frames(3)));
    CHECK_MSG(eventually([&] { return bare->saw("disconnected"); }) && !bareRoute->platform("ask:2"), joined(bare->events()));
    CHECK_MSG(!unstartedRoute->platform("ask:2") && !engine.events().ended(), joined(engine.events().all()));
    CHECK_MSG(none.death() == "ordinary" && none.delegate.deaths() == std::vector<std::string>{"the host's platform answers no requests of the widget's renderer"} && has(none.delegate.events(), "diag:platform-timeout"),
              joined(none.delegate.events()));
    const auto closes = [&](uint32_t runtime) {
        const std::vector<std::string> frames = engine.frames(runtime);
        return std::count(frames.begin(), frames.end(), "close");
    };
    sibling.client->platform("size:2");
    sibling.settle();
    CHECK_MSG(eventually([&] { return engine.saw(4, "platform:size:2"); }) && sibling.delegate.deaths().empty() && closes(3) == 1, joined(engine.frames(3)));
    unstarted.onDeath = [&, route = std::weak_ptr<SandboxHostedChannel>(unstartedRoute)](const std::string& reason) {
        const std::shared_ptr<SandboxHostedChannel> channel = route.lock();
        std::lock_guard<std::mutex> l(mutex);
        heard.push_back(reason + ": " + (channel ? deathName(channel->death()) : "gone"));
    };
    unstarted.start();
    const auto told = [&] {
        std::lock_guard<std::mutex> l(mutex);
        return heard;
    };
    CHECK_MSG(eventually([&] { return !told().empty(); }), "no death at its start");
    sleepMs(200);
    CHECK_MSG(told() == std::vector<std::string>{kEngineGone + ": ordinary"}, joined(told()));
}

/// The bridge call of runtime 1 an engine end writes by hand.
static std::string bridgeFrame(const std::string& method, const std::string& args) { return FrameBytes(1, wire::Bridge).str(method).str(args).bytes(); }

/// A lying engine end against a hosted WidgetHost (ScriptedEngine on a hosting stream): after the boot's one haptic series, a privileged call with no press delivered is refused (`activation-blocked`, no action); a press the engine end reports (a platform message saying so) then a link and a haptic: the link refused, the haptic dropped; after `pressDelivered` exactly one link passes. The press, the host's `event()` and the rest write no `Event`, `Activation`, `PressBegan` or `PressEnded` frame. The host's own kinds sent by the engine end (`PressBegan`, `Activation`) end the stream as the other side's.
TEST(remote_stream_hosted_lying_engine_end) {
    {
        ScriptedEngine engine({}, true);
        WidgetHostOptions options;
        options.context = messageContext(false, true, false, "private");
        std::mutex m;
        std::vector<WidgetAction> actions;
        std::vector<std::string> heard;
        WidgetHost host("<p>x</p>", WidgetTheme{}, 300, options, engine.host().hostedChannel());
        host.onAction = [&](const WidgetAction& a) {
            std::lock_guard<std::mutex> l(m);
            actions.push_back(a);
        };
        host.onPlatformMessage = [&](const std::string& message) {
            std::lock_guard<std::mutex> l(m);
            heard.push_back(message);
        };
        host.start();
        CHECK_MSG(eventually([&] { return engine.saw(1, "hosted-start"); }), joined(engine.frames(1)));
        CHECK(engine.ready());
        int marks = 0;
        // What the engine end wrote before a log of its own has reached the host's observers once that log is in the diagnostics.
        const auto settled = [&] {
            const std::string mark = "mark-" + std::to_string(++marks);
            CHECK(engine.send(logFrame(1, mark)));
            CHECK_MSG(host.waitUntil([&](const WidgetDiagnostics& d) { return d.hasLog(mark); }, 5), mark);
            host.onHostThread([] {});
        };
        const auto counted = [&](WidgetAction::Kind kind) {
            std::lock_guard<std::mutex> l(m);
            return std::count_if(actions.begin(), actions.end(), [&](const WidgetAction& a) { return a.kind == kind; });
        };
        CHECK(engine.send(bridgeFrame("haptic", "[\"selection\"]")));
        settled();
        CHECK_MSG(counted(WidgetAction::Haptic) == 1, std::to_string(counted(WidgetAction::Haptic)));
        sleepMs(int(sdk::HAPTIC_SERIES_MS) + 100);
        CHECK(engine.send(bridgeFrame("openLink", linkArgs("https://example.com/none"))));
        settled();
        CHECK(engine.send(platformMessageFrame(1, "press") + bridgeFrame("openLink", linkArgs("https://example.com/reported")) + bridgeFrame("haptic", "[\"impact\",\"soft\"]")));
        settled();
        CHECK(counted(WidgetAction::OpenLink) == 0 && counted(WidgetAction::Haptic) == 1);
        host.pressDelivered();
        CHECK(engine.send(bridgeFrame("openLink", linkArgs("https://example.com/1")) + bridgeFrame("openLink", linkArgs("https://example.com/2"))));
        settled();
        {
            std::lock_guard<std::mutex> l(m);
            std::vector<std::string> links;
            for (const WidgetAction& a : actions)
                if (a.kind == WidgetAction::OpenLink) links.push_back(a.url);
            CHECK_MSG(links == std::vector<std::string>{"https://example.com/1"}, joined(links));
            CHECK(heard == std::vector<std::string>{"press"});
        }
        const std::string blocked = "openLink ignored by the host: no tap on the visible widget allows it";
        const WidgetDiagnostics d = host.diagnostics();
        CHECK_MSG((diagsOf(d, "activation-blocked") == std::vector<std::string>{blocked, blocked, blocked}), joined(diagsOf(d, "activation-blocked")));

        host.event(kTapOnButton);
        settled();
        for (const std::string& frame : engine.frames(1))
            CHECK_MSG(frame.rfind("event:", 0) != 0 && frame != "kind 5" && frame != "kind 13" && frame != "kind 14", frame);
        CHECK_MSG(!engine.events().ended() && !host.diagnostics().dead, joined(engine.events().all()));
    }
    const std::vector<std::pair<std::string, std::string>> ends = {
        {bare(1, wire::PressBegan), "kind 13 from the other side"},
        {FrameBytes(1, wire::Activation).str(kTapOnButton).i64(1).bytes(), "kind 5 from the other side"},
    };
    for (const auto& [bytes, reason] : ends) {
        ScriptedEngine engine({}, true);
        WidgetHost host("<p>x</p>", WidgetTheme{}, 300, WidgetHostOptions(), engine.host().hostedChannel());
        host.start();
        CHECK(eventually([&] { return engine.saw(1, "hosted-start"); }));
        CHECK(engine.ready() && engine.send(bytes));
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }, 2), reason + ": " + joined(engine.events().all()));
        CHECK_MSG(endsOnce(engine.events().all(), reason), reason + ": " + joined(engine.events().all()));
        CHECK(host.waitUntil(died, 5) && host.diagnostics().dead == std::optional<std::string>(kEngineGone));
    }
}

/// What a hosting stream's host end cannot accept, each ending the whole stream with its reason (`hostileRun` on a hosting stream: runtime 1 a bare sink, 2 a hosted client): a second platform request or Intl request while the first is in the host's hands, a size past the frame bound, a field cut short or bytes after the fields of a hosted kind, the host's hosting kinds from the engine's side, and the render glue's kinds (`Ops`, `Query`, `Listen`, `PreventDefault`, `Anim`, `RequestsFrame`), which a hosted runtime keeps in the engine process, on any runtime and while a request waits; on an engine stream, the six hosted kinds.
static std::vector<Hostile> hostedHostileInputs() {
    const uint32_t bound = uint32_t(SandboxRemoteWire::kMaxFrameBytes);
    const std::string never = ", which a hosted runtime never sends";
    std::vector<Hostile> out;
    const auto add = [&](std::string name, std::string bytes, std::string reason, bool hosting, std::string held = {}) {
        Hostile v;
        v.name = std::move(name);
        v.bytes = std::move(bytes);
        v.reason = std::move(reason);
        v.held = std::move(held);
        v.hosting = hosting;
        out.push_back(std::move(v));
    };
    add("a second platform request while one is unanswered", platformRequestFrame(1, "second"), "kind 97 on runtime 1 while its platform request is unanswered", true,
        platformRequestFrame(1, "first"));
    add("Intl while Intl is unanswered", intlFrame(1, "{}"), "kind 70 on runtime 1 while its Intl request is unanswered", true, intlFrame(1, "{}"));
    add("a platform message's size past the bound, no body", wire::u32(bound + 1) + wire::u32(1) + std::string(1, char(wire::PlatformMessage)),
        "a frame of " + std::to_string(bound + 1) + " bytes, past the bound of " + std::to_string(bound), true);
    out.back().bodiless = true;
    add("a node count cut short", FrameBytes(1, wire::Nodes).raw(std::string(4, '\0')).bytes(), "kind 101 with a field past the frame's end", true);
    add("a byte after renderer ready", FrameBytes(1, wire::RendererReady).raw("x").bytes(), "kind 99 with bytes after its fields", true);
    add("a platform request longer than its frame", FrameBytes(1, wire::PlatformRequest).raw(wire::u32(1000)).raw("abc").bytes(), "kind 97 with a field past the frame's end", true);
    add("the host's hosting opening", hostingOpeningBytes(1, kBuild, "s", "v", ""), "kind 32 from the other side", true);
    add("the host's platform message", FrameBytes(1, wire::Platform).str("x").bytes(), "kind 35 from the other side", true);
    add("ops while a platform request is unanswered", FrameBytes(1, wire::Ops).str("[]").bytes(), "kind 68 on runtime 1" + never, true, platformRequestFrame(1, "first"));
    add("ops on a runtime never opened", FrameBytes(3, wire::Ops).str("[]").bytes(), "kind 68 on runtime 3" + never, true);
    const std::vector<std::pair<uint8_t, std::string>> render = {
        {wire::Ops, FrameBytes(1, wire::Ops).str("[]").bytes()},
        {wire::Query, queryFrame(1, kRectQuery)},
        {wire::Listen, FrameBytes(1, wire::Listen).str("{}").bytes()},
        {wire::PreventDefault, FrameBytes(1, wire::PreventDefault).str("{}").bytes()},
        {wire::Anim, FrameBytes(1, wire::Anim).str("{}").bytes()},
        {wire::RequestsFrame, bare(1, wire::RequestsFrame)},
    };
    for (const auto& [kind, bytes] : render) add("kind " + std::to_string(kind) + " on a hosted runtime", bytes, "kind " + std::to_string(kind) + " on runtime 1" + never, true);
    const std::vector<std::pair<uint8_t, std::string>> hosted = {
        {wire::PlatformMessage, platformMessageFrame(1, "x")},
        {wire::PlatformRequest, platformRequestFrame(1, "x")},
        {wire::ValidatedOps, FrameBytes(1, wire::ValidatedOps).str("[]").bytes()},
        {wire::RendererReady, bare(1, wire::RendererReady)},
        {wire::Rendered, bare(1, wire::Rendered)},
        {wire::Nodes, FrameBytes(1, wire::Nodes).i64(3).bytes()},
    };
    for (const auto& [kind, bytes] : hosted) add("kind " + std::to_string(kind) + " on an engine stream", bytes, "kind " + std::to_string(kind) + " on runtime 1, which only a hosted runtime sends", false);
    return out;
}

/// Every input of `hostedHostileInputs` ends its stream once with its reason within 2 s (a size past the bound within 1 s), both runtimes hear it once and nothing after. What a hosting stream takes goes on: a platform message past `kRemotePlatformLimit` within the frame bound and a batch the host did not ask for are the client's protocol violations; while a runtime's platform request is in the host's hands its log and its engine's Intl request are taken, and so are another runtime's frames, and a probe's answer that came meanwhile waits until both requests were answered; a closed hosted runtime's well-formed frames are dropped, and a damaged one still ends the stream.
TEST(remote_stream_hosted_hostile_frames) {
    for (const Hostile& v : hostedHostileInputs()) hostileRun(v);

    ScriptedEngine engine({}, true);
    Gate platformGate, intlGate;
    auto sink = std::make_shared<StreamSink>();
    sink->gate = &intlGate;
    sink->platformGate = &platformGate;
    std::shared_ptr<SandboxHostedChannel> channel = engine.host().hostedChannel();
    channel->attach(sink);
    StreamDelegate delegate;
    std::unique_ptr<SandboxEngine> client = SandboxEngine::hostedClient("<p>x</p>", testConfig(), engine.host().hostedChannel());
    client->setDelegate(&delegate);
    CHECK(eventually([&] { return engine.saw(1, "open") && engine.saw(2, "open"); }));
    CHECK(engine.ready() && engine.send(logFrame(1, "control-1") + logFrame(2, "control-2")));
    CHECK(eventually([&] { return sink->saw("log:log:control-1") && delegate.saw("log:control-2"); }));

    const size_t limit = SandboxEngine::kRemotePlatformLimit;
    CHECK(engine.send(platformMessageFrame(2, std::string(limit + 1, 'p')) + FrameBytes(2, wire::ValidatedOps).str("[]").bytes() + logFrame(2, "after the violations")));
    CHECK_MSG(eventually([&] { return delegate.saw("log:after the violations"); }, 10), joined(delegate.events()));
    const std::vector<std::string> violations = {"diag:protocol-violation:a platform message over 32 MB", "diag:protocol-violation:validated ops the host did not ask for"};
    CHECK_MSG(delegate.eventsWith("diag:protocol-violation:") == violations, joined(delegate.events()));

    Background<std::string> probing([&] { return channel->probe("held"); });
    CHECK(eventually([&] { return engine.saw(1, "probe:1:held"); }));
    CHECK(engine.send(platformRequestFrame(1, "waits")));
    CHECK(platformGate.waitedOn());
    CHECK(engine.send(logFrame(1, "while the platform waits") + intlFrame(1, "{}") + probeAnswer(1, 1, "held") + logFrame(2, "meanwhile")));
    CHECK_MSG(eventually([&] { return sink->saw("log:log:while the platform waits") && delegate.saw("log:meanwhile"); }), joined(sink->events()));
    CHECK(!probing.within(0.3));
    platformGate.open();
    CHECK_MSG(eventually([&] { return engine.saw(1, "platform-answer:ANSWER"); }), joined(engine.frames(1)));
    CHECK(intlGate.waitedOn());
    CHECK(engine.send(logFrame(1, "while Intl waits")));
    CHECK(eventually([&] { return sink->saw("log:log:while Intl waits"); }));
    CHECK(!probing.within(0.3));
    intlGate.open();
    CHECK(probing.within(5) == std::optional<std::string>("held"));
    CHECK_MSG(eventually([&] { return engine.saw(1, "intl-answer:INTL"); }), joined(engine.frames(1)));
    const std::vector<std::string> expected = {"log:log:control-1", "platform-request:waits", "log:log:while the platform waits", "platform-request-done", "intl:{}", "log:log:while Intl waits", "intl-done"};
    CHECK_MSG(sink->events() == expected, firstDifference(sink->events(), expected));

    channel->close();
    CHECK(eventually([&] { return engine.saw(1, "close") && engine.events().open() == 1; }));
    CHECK(engine.send(platformMessageFrame(1, "late") + platformRequestFrame(1, "late") + FrameBytes(1, wire::ValidatedOps).str("[]").bytes() + bare(1, wire::RendererReady) +
                      bare(1, wire::Rendered) + FrameBytes(1, wire::Nodes).i64(3).bytes() + logFrame(1, "late") + intlFrame(1, "{}") + probeAnswer(1, 1, "late") + logFrame(2, "after the close")));
    CHECK_MSG(eventually([&] { return delegate.saw("log:after the close"); }), joined(delegate.events()));
    CHECK_MSG(!engine.events().ended() && sink->events() == expected, joined(engine.events().all()) + " / " + joined(sink->events()));
    CHECK(engine.send(FrameBytes(1, wire::Nodes).raw("x").bytes() + logFrame(2, "trailing-2")));
    CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }, 2), joined(engine.events().all()));
    CHECK_MSG(endsOnce(engine.events().all(), "kind 101 with a field past the frame's end"), joined(engine.events().all()));
    CHECK(eventually([&] { return delegate.saw("died:" + kEngineGone); }));
    client->thread().sync([] {});
    CHECK_MSG(!delegate.saw("log:trailing-2") && sink->events() == expected, joined(delegate.events()));
}

/// A lying engine end floods a hosted WidgetHost with platform messages while the host's thread is held, as a stuck embedder's: each message the client took stays counted until the host ran its work, so at the bound the widget dies flooded once (one `Stop` with that reason, then `Close`), the client holds exactly `kRemoteQueuedCalls` messages, far under `kRemoteQueuedBytes` (`remoteBacklog()`, sampled throughout, passes that only by the one message being checked and given back at once), and the host's end keeps reading (another runtime's logs on the same stream arrive while the host is held, the stream goes on); released, the backlog returns to 0 and the host hears the death and exactly the messages the client took.
TEST(remote_stream_hosted_platform_flood_stays_within_the_bounds) {
    const std::string flooded = "the widget engine process sent more than the host keeps queued";
    ScriptedEngine engine({}, true);
    std::atomic<size_t> heard{0};
    WidgetHost host("<p>x</p>", WidgetTheme{}, 300, WidgetHostOptions(), engine.host().hostedChannel());
    host.onPlatformMessage = [&](const std::string&) { heard++; };
    auto other = std::make_shared<StreamSink>();
    std::shared_ptr<SandboxHostedChannel> otherChannel = engine.host().hostedChannel();
    otherChannel->attach(other);
    host.start();
    CHECK(eventually([&] { return engine.saw(1, "hosted-start") && engine.saw(2, "open"); }));
    CHECK(engine.ready());
    HeldHost held(host);
    CHECK(held.held());
    std::atomic<bool> flooding{true};
    std::atomic<size_t> maxBytes{0}, maxCalls{0};
    Background<bool> watching([&] {
        while (flooding) {
            const SandboxEngine::RemoteBacklog b = host.remoteBacklog();
            maxBytes = std::max(maxBytes.load(), b.bytes);
            maxCalls = std::max(maxCalls.load(), b.calls);
            std::this_thread::yield();
        }
        return true;
    });
    const size_t total = SandboxEngine::kRemoteQueuedCalls + 256;
    std::string batch;
    size_t logs = 0;
    for (size_t i = 1; i <= total; i++) {
        batch += platformMessageFrame(1, "m" + std::to_string(i));
        if (i % 2048 && i != total) continue;
        batch += logFrame(2, "during " + std::to_string(++logs));
        CHECK(engine.send(batch));
        batch.clear();
    }
    CHECK_MSG(eventually([&] { return other->saw("log:log:during " + std::to_string(logs)); }, 10), joined(other->events()));
    CHECK_MSG(eventually([&] { return engine.saw(1, "close") && engine.events().open() == 1; }, 10), joined(engine.frames(1)) + " / " + joined(engine.events().all()));
    std::vector<std::string> stops;
    for (const std::string& frame : engine.frames(1))
        if (frame.rfind("stop:", 0) == 0) stops.push_back(frame);
    CHECK_MSG(stops == std::vector<std::string>{"stop:" + flooded}, joined(stops));
    for (size_t i = 1; i <= logs; i++) CHECK_MSG(other->saw("log:log:during " + std::to_string(i)), std::to_string(i));
    CHECK_MSG(!engine.events().ended() && engine.events().open() == 1 && heard == 0, joined(engine.events().all()));
    flooding = false;
    CHECK(watching.finish(5));
    // Held at the bound: exactly the first `kRemoteQueuedCalls` messages, the next refused and the widget killed.
    size_t heldBytes = 0;
    for (size_t i = 1; i <= SandboxEngine::kRemoteQueuedCalls; i++) heldBytes += 1 + std::to_string(i).size();
    CHECK_MSG(backlogIs(host, heldBytes, SandboxEngine::kRemoteQueuedCalls), backlogText(host));
    // Sampled while the reader admits, the backlog passes it by at most the one message the client counts while it checks it and gives back at once. The samples bound it from above only: the watcher need not run between the fill and its stop, so the fill is the exact reading above.
    const size_t tried = 1 + std::to_string(SandboxEngine::kRemoteQueuedCalls + 1).size();
    CHECK_MSG(maxCalls <= SandboxEngine::kRemoteQueuedCalls + 1 && maxBytes <= heldBytes + tried && heldBytes + tried < SandboxEngine::kRemoteQueuedBytes,
              std::to_string(maxBytes) + " bytes, " + std::to_string(maxCalls) + " calls");
    printf("    sampled while the reader admitted: at most %zu calls, %zu bytes\n", maxCalls.load(), maxBytes.load());
    CHECK(held.release());
    CHECK_MSG(eventually([&] { return backlogIs(host, 0, 0); }, 10), backlogText(host));
    CHECK(host.waitUntil(died, 5) && host.diagnostics().dead == std::optional<std::string>(flooded));
    host.onHostThread([] {});
    CHECK_MSG(heard == SandboxEngine::kRemoteQueuedCalls, std::to_string(heard));
    CHECK_MSG(!engine.events().ended(), joined(engine.events().all()));
}

/// Three hosted WidgetHosts on one stream between real ends: A's renderer asks as it is built and A's host keeps the responder and never answers; while A's request waits, B and C load, render, answer probes and exchange platform messages, and B's own request is answered, all within the 3 s; A dies 3 to 4 s after its request with `platform-timeout`, the engine end gets A's `Stop` and `Close` (none for B or C) and A's renderer's request returns nullopt, a late answer changing nothing; B and C go on and the stream does not end. A fourth widget whose renderer factory throws ends with that reason alone.
TEST(remote_stream_hosted_unanswered_request_kills_only_its_runtime) {
    using Asked = Background<std::optional<std::string>>;
    std::mutex m;
    std::optional<Asked> asked;
    std::chrono::steady_clock::time_point askedAt;
    BuiltRenderers built;
    const HostedRendererFactory renderers = built.factory(tall);
    StreamPeers peers(kBuild, *RES, nullptr,
                      [&](std::shared_ptr<SandboxResources> resources, const std::string& theme, double width, std::shared_ptr<SandboxHostedPlatform> platform) -> std::unique_ptr<Renderer> {
                          if (width == 333) throw std::runtime_error("no renderer for this widget");
                          std::unique_ptr<Renderer> renderer = renderers(std::move(resources), theme, width, platform);
                          if (width == 301) {
                              // A's renderer asks from a thread of its own, never inside the factory.
                              std::lock_guard<std::mutex> l(m);
                              askedAt = std::chrono::steady_clock::now();
                              asked.emplace([platform] { return platform->request("at load"); });
                          }
                          return renderer;
                      });
    const WidgetHostOptions options;
    HostedStreamHost a(*peers.end, kHostWidget, options, 301, "INTL-A", false);
    HostedStreamHost b(*peers.end, kHostWidget, options, 302, "INTL-B");
    HostedStreamHost c(*peers.end, kHostWidget, options, 303, "INTL-C");
    CHECK(eventually([&] {
        std::lock_guard<std::mutex> l(m);
        return asked.has_value();
    }, 20));
    std::chrono::steady_clock::time_point since;
    {
        std::lock_guard<std::mutex> l(m);
        since = askedAt;
    }
    CHECK(b.shown() && c.shown());
    CHECK_MSG(b.renders("n:INTL-B") && c.renders("n:INTL-C"), b.rendered().substr(0, 500));
    CHECK(b.host->probe("d.textContent") == "h:20" && c.host->probe("d.textContent") == "h:20");
    b.host->sendPlatform("say:b");
    c.host->sendPlatform("say:c");
    b.host->sendPlatform("ask:b");
    CHECK_MSG(eventually([&] { return b.messages() == std::vector<std::string>{"b", "got:answer:b"} && c.messages() == std::vector<std::string>{"c"}; }),
              joined(b.messages()) + " / " + joined(c.messages()));
    const double meanwhile = msSince(since);
    CHECK_MSG(meanwhile < 3000 && !a.host->diagnostics().dead, std::to_string(meanwhile) + " ms after A's request");
    CHECK_MSG(eventually([&] { return a.requests() == std::vector<std::string>{"at load"}; }), joined(a.requests()));

    CHECK(a.host->waitUntil(died, 6));
    const double waited = msSince(since) / 1000;
    CHECK_MSG(waited >= WidgetHost::kPlatformAnswerSeconds && waited < WidgetHost::kPlatformAnswerSeconds + 1, std::to_string(waited) + " s");
    const WidgetDiagnostics d = a.host->diagnostics();
    CHECK_MSG(d.dead == std::optional<std::string>(kPlatformLate), d.dead.value_or("alive"));
    CHECK(diagsOf(d, "platform-timeout") == std::vector<std::string>{kPlatformLate});
    CountingStream& counted = *peers.counted;
    CHECK_MSG(eventually([&] { return counted.readOf(wire::Stop, 1) == 1 && counted.readOf(wire::Close, 1) == 1; }),
              std::to_string(counted.readOf(wire::Stop, 1)) + " stops, " + std::to_string(counted.readOf(wire::Close, 1)) + " closes");
    const std::optional<std::optional<std::string>> got = asked->within(5);
    CHECK_MSG(got && !*got, got ? got->value_or("nullopt") : "still waiting");
    if (const std::function<void(std::string)> late = a.unanswered(0)) late("too late");
    settleHost(*a.host);
    CHECK(a.host->diagnostics().dead == std::optional<std::string>(kPlatformLate));

    CHECK(b.host->probe("1 + 1") == "2" && c.host->probe("2 + 2") == "4");
    c.host->sendPlatform("say:c2");
    CHECK_MSG(eventually([&] { return c.messages() == std::vector<std::string>{"c", "c2"}; }), joined(c.messages()));
    for (const uint32_t runtime : {2u, 3u}) CHECK(counted.readOf(wire::Stop, runtime) == 0 && counted.readOf(wire::Close, runtime) == 0);
    CHECK(eventually([&] { return peers.events.open() == 2; }));
    CHECK_MSG(!b.host->diagnostics().dead && !c.host->diagnostics().dead && !peers.events.ended(), joined(peers.events.all()));

    {
        HostedStreamHost failing(*peers.end, "<p>x</p>", options, 333, "INTL-D");
        CHECK(failing.host->waitUntil(died, 20));
        CHECK_MSG(failing.host->diagnostics().dead == std::optional<std::string>("the widget could not be built in the engine process: no renderer for this widget"),
                  failing.host->diagnostics().dead.value_or("alive"));
    }
    CHECK(b.host->probe("'still'") == "still" && c.host->probe("'still'") == "still");
    CHECK_MSG(!b.host->diagnostics().dead && !c.host->diagnostics().dead && !peers.events.ended(), joined(peers.events.all()));
    CHECK(built.count() == 3);
}

// ───────────── a kept renderer over the stream (WidgetHost::keepRenderer) ─────────────

/// How a hosted widget dies when the engine process sent more than its client keeps queued.
static const std::string kFloodedReason = "the widget engine process sent more than the host keeps queued";

/// What a kept renderer's observers heard, in order under a lock: "message:<m>", "request:<m>" and "ended:<reason>" (and when that came). A request is answered "answer:<m>" unless it is `hold`, whose `respond` is kept for the test; the message `block` waits on `gate` on the handle's thread before it is noted.
struct KeptObservers {
    /// Set before `observers()` is handed over.
    std::string hold;
    std::string block;
    Gate* gate = nullptr;

    HostedKeptRenderer::Observers observers() {
        HostedKeptRenderer::Observers o;
        o.onPlatformMessage = [this](const std::string& message) {
            if (gate && message == block) gate->wait();
            std::lock_guard<std::mutex> l(mutex_);
            events_.push_back("message:" + message);
        };
        o.onPlatformRequest = [this](const std::string& message, std::function<void(std::string)> respond) {
            {
                std::lock_guard<std::mutex> l(mutex_);
                events_.push_back("request:" + message);
                if (!hold.empty() && message == hold) return held_.push_back(std::move(respond));
            }
            respond("answer:" + message);
        };
        o.onEnded = [this](const std::string& reason) {
            std::lock_guard<std::mutex> l(mutex_);
            events_.push_back("ended:" + reason);
            endedAt_ = std::chrono::steady_clock::now();
        };
        return o;
    }
    std::vector<std::string> events() {
        std::lock_guard<std::mutex> l(mutex_);
        return events_;
    }
    /// The messages heard that begin with `prefix`, in order.
    std::vector<std::string> messages(const std::string& prefix = "") {
        std::vector<std::string> out;
        for (const std::string& e : events())
            if (e.rfind("message:" + prefix, 0) == 0) out.push_back(e.substr(8));
        return out;
    }
    bool heard(const std::string& message) { return has(events(), "message:" + message); }
    std::vector<std::string> ends() {
        std::vector<std::string> out;
        for (const std::string& e : events())
            if (e.rfind("ended:", 0) == 0) out.push_back(e.substr(6));
        return out;
    }
    /// When the first `onEnded` came, nullopt before it.
    std::optional<std::chrono::steady_clock::time_point> endedAt() {
        std::lock_guard<std::mutex> l(mutex_);
        return endedAt_;
    }
    /// The `i`th kept responder, empty before it came.
    std::function<void(std::string)> held(size_t i) {
        std::lock_guard<std::mutex> l(mutex_);
        return i < held_.size() ? held_[i] : nullptr;
    }
    size_t count() {
        std::lock_guard<std::mutex> l(mutex_);
        return events_.size();
    }

private:
    std::mutex mutex_;
    std::vector<std::string> events_;
    std::vector<std::function<void(std::string)>> held_;
    std::optional<std::chrono::steady_clock::time_point> endedAt_;
};

/// A widget that dies of its CPU limit soon after it loaded.
static const char* kDiesSoon = "<p>kept</p><script>setTimeout(function () { for (;;) {} }, 300);</script>";

/// The host's frames of `runtime` the engine's side of `counted` read, of every kind.
static size_t hostFramesOf(CountingStream& counted, uint32_t runtime) {
    size_t n = 0;
    for (int kind = 0; kind <= UINT8_MAX; kind++) n += counted.readOf(uint8_t(kind), runtime);
    return n;
}

/// A kept renderer's traffic both ways through its handle: 20 `say:<tag><i>` heard back in order, an `ask:` the renderer makes on a thread of its own and a `wait:` it makes on the glue's thread, each answered through the handle; "" when all came within 5 s, else what was heard.
static std::string keptTraffic(HostedKeptRenderer& kept, KeptObservers& heard, const std::string& tag) {
    std::vector<std::string> said;
    for (int i = 1; i <= 20; i++) {
        said.push_back(tag + std::to_string(i));
        kept.sendPlatform("say:" + said.back());
    }
    kept.sendPlatform("ask:" + tag + "-thread");
    kept.sendPlatform("wait:" + tag + "-glue");
    const bool came = eventually([&] { return heard.messages(tag).size() >= said.size() && heard.heard("got:answer:" + tag + "-thread") && heard.heard("waited:answer:" + tag + "-glue"); });
    return came && heard.messages(tag) == said ? std::string() : tag + ": " + joined(heard.events());
}

/// A stopped and a dead kept renderer on one stream between real ends: one its host stopped (`keepRenderer` on a live widget, `Stop` once) and one dead of its own CPU limit (`keepsRenderer`: the WidgetHost still has its traffic past the death and the route still counts); each keep takes one runtime out of `Events::runtimes` while the stream goes on. With both WidgetHosts gone each handle carries 20 messages each way in order and gets a request of the renderer's own thread and one of the glue's thread answered; no `Close` crosses for either until its release, exactly one after it and no frame of that runtime after that, its renderer goes, and the other still answers. Then the engine's side closes its end, as an exiting engine process: the other handle hears `onEnded("the widget engine process ended")` once and last, its renderer goes, and the released one hears nothing.
TEST(remote_stream_hosted_kept_renderers) {
    const auto stoppedGone = std::make_shared<std::atomic<bool>>(false), deadGone = std::make_shared<std::atomic<bool>>(false);
    std::atomic<int> made{0};
    BuiltRenderers built;
    StreamPeers peers(kBuild, *RES, nullptr, built.factory([&](HostedTestRenderer& r) { r.gone = made++ ? deadGone : stoppedGone; }));
    CountingStream& counted = *peers.counted;
    KeptObservers stoppedHeard, deadHeard;
    std::unique_ptr<HostedKeptRenderer> stoppedKept, deadKept;
    {
        // One after the other, so the stopped widget is runtime 1 and its renderer the first built.
        HostedStreamHost stopped(*peers.end, "<p>stopped</p>", WidgetHostOptions(), 300, "INTL");
        CHECK(stopped.shown());
        HostedStreamHost dead(*peers.end, kDiesSoon, WidgetHostOptions(), 300, "INTL", true, [](WidgetHost& h) { h.keepsRenderer = true; });
        CHECK(dead.shown());
        CHECK_MSG(eventually([&] { return peers.events.open() == 2; }), joined(peers.events.all()));
        CHECK(dead.host->waitUntil(died, 10));
        const std::string reason = dead.host->diagnostics().dead.value_or("");
        CHECK_MSG(reason.rfind("CPU limit exceeded", 0) == 0, reason);
        dead.host->sendPlatform("say:past the death");
        CHECK_MSG(eventually([&] { return has(dead.messages(), "past the death"); }), joined(dead.messages()));
        CHECK_MSG(peers.events.open() == 2 && counted.readOf(wire::Close) == 0, joined(peers.events.all()));
        stoppedKept = stopped.host->keepRenderer(stoppedHeard.observers());
        CHECK(stoppedKept);
        CHECK_MSG(eventually([&] { return peers.events.open() == 1; }), joined(peers.events.all()));
        deadKept = dead.host->keepRenderer(deadHeard.observers());
        CHECK(deadKept);
        CHECK_MSG(eventually([&] { return peers.events.open() == 0; }), joined(peers.events.all()));
    }
    if (!stoppedKept || !deadKept) return;
    CHECK_MSG(eventually([&] { return counted.readOf(wire::Stop, 1) == 1; }), std::to_string(counted.readOf(wire::Stop, 1)) + " stops");
    const std::string stoppedTraffic = keptTraffic(*stoppedKept, stoppedHeard, "s");
    CHECK_MSG(stoppedTraffic.empty(), stoppedTraffic);
    const std::string deadTraffic = keptTraffic(*deadKept, deadHeard, "d");
    CHECK_MSG(deadTraffic.empty(), deadTraffic);
    CHECK_MSG(counted.readOf(wire::Close) == 0 && counted.readOf(wire::Stop, 2) == 0, std::to_string(counted.readOf(wire::Close)) + " closes");
    CHECK(!*stoppedGone && !*deadGone && !stoppedKept->ended() && !deadKept->ended());
    CHECK_MSG(!peers.events.ended() && peers.events.open() == 0, joined(peers.events.all()));
    CHECK(built.count() == 2);

    stoppedKept->release();
    CHECK_MSG(eventually([&] { return counted.readOf(wire::Close, 1) == 1; }), std::to_string(counted.readOf(wire::Close, 1)) + " closes");
    CHECK_MSG(eventually([&] { return stoppedGone->load(); }), "the released renderer is still in the engine end");
    const size_t closedAt = hostFramesOf(counted, 1);
    const size_t heardAt = stoppedHeard.count();
    stoppedKept->sendPlatform("say:after the release");
    const std::string otherTraffic = keptTraffic(*deadKept, deadHeard, "e");
    CHECK_MSG(otherTraffic.empty(), otherTraffic);
    CHECK_MSG(counted.readOf(wire::Close, 1) == 1 && hostFramesOf(counted, 1) == closedAt, std::to_string(hostFramesOf(counted, 1)) + " frames for " + std::to_string(closedAt));
    CHECK(counted.readOf(wire::Close, 2) == 0 && stoppedHeard.count() == heardAt && !*deadGone);

    peers.pair.engine->close();
    CHECK_MSG(eventually([&] { return !deadHeard.ends().empty(); }), joined(deadHeard.events()));
    CHECK_MSG(deadHeard.ends() == std::vector<std::string>{kEngineGone} && deadHeard.events().back() == "ended:" + kEngineGone, joined(deadHeard.events()));
    CHECK(deadKept->ended() && peers.events.ended().has_value());
    CHECK_MSG(eventually([&] { return deadGone->load(); }), "the other renderer outlived its engine process's end");
    deadKept->release();
    CHECK_MSG(deadHeard.ends().size() == 1 && stoppedHeard.ends().empty() && !stoppedKept->ended(), joined(stoppedHeard.events()));
}

/// Seconds from `from` to now.
static double secondsSince(std::chrono::steady_clock::time_point from) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count(); }

/// Hosted WidgetHosts with a SandboxHostedWait on one stream between real ends: A's renderer asks on a thread of its own under a published hold and A's host answers 4 s later: the renderer gets that answer and A goes on; C's request, under a hold released 1.5 s on, is never answered and C dies 3 to 4 s after that release with `platform-timeout` (`Stop` and `Close` for C alone, a late answer changing nothing); B, with no record, exchanges its traffic meanwhile and goes on. Kept, A's route keeps its client and so its record: its renderer's request under a new hold, answered through the handle 4 s later, gets that answer and the route goes on.
TEST(remote_stream_hosted_held_request_outlives_three_seconds) {
    using namespace std::chrono_literals;
    BuiltRenderers built;
    StreamPeers peers(kBuild, *RES, nullptr, built.factory(tall));
    const auto waitA = std::make_shared<SandboxHostedWait>(), waitC = std::make_shared<SandboxHostedWait>();
    HostedStreamHost a(*peers.end, kHostWidget, WidgetHostOptions(), 301, "INTL-A", false, nullptr, waitA);
    HostedStreamHost b(*peers.end, kHostWidget, WidgetHostOptions(), 302, "INTL-B");
    HostedStreamHost c(*peers.end, kHostWidget, WidgetHostOptions(), 303, "INTL-C", false, nullptr, waitC);
    CHECK(a.shown() && b.shown() && c.shown());
    const auto heldAt = std::chrono::steady_clock::now();
    waitA->hold(heldAt + 2s);
    const auto releaseC = heldAt + 1500ms;
    waitC->hold(releaseC);
    a.host->sendPlatform("ask:held");
    c.host->sendPlatform("ask:dropped");
    CHECK_MSG(eventually([&] { return a.requests() == std::vector<std::string>{"held"} && c.requests() == std::vector<std::string>{"dropped"}; }), joined(a.requests()) + " / " + joined(c.requests()));
    b.host->sendPlatform("say:b");
    b.host->sendPlatform("ask:b");
    CHECK_MSG(eventually([&] { return b.messages() == std::vector<std::string>{"b", "got:answer:b"}; }), joined(b.messages()));
    std::this_thread::sleep_until(heldAt + 4s);
    CHECK_MSG(!a.host->diagnostics().dead, a.host->diagnostics().dead.value_or(""));
    if (const std::function<void(std::string)> respond = a.unanswered(0)) respond("held answer");
    CHECK_MSG(eventually([&] { return a.messages() == std::vector<std::string>{"got:held answer"}; }), joined(a.messages()));

    CHECK(c.host->waitUntil(died, 6));
    const double afterRelease = std::chrono::duration<double>(std::chrono::steady_clock::now() - releaseC).count();
    CHECK_MSG(afterRelease >= WidgetHost::kPlatformAnswerSeconds && afterRelease < WidgetHost::kPlatformAnswerSeconds + 1, std::to_string(afterRelease) + " s after its release");
    const WidgetDiagnostics d = c.host->diagnostics();
    CHECK_MSG(d.dead == std::optional<std::string>(kPlatformLate) && diagsOf(d, "platform-timeout") == std::vector<std::string>{kPlatformLate}, d.dead.value_or("alive"));
    CountingStream& counted = *peers.counted;
    CHECK_MSG(eventually([&] { return counted.readOf(wire::Stop, 3) == 1 && counted.readOf(wire::Close, 3) == 1; }), std::to_string(counted.readOf(wire::Close, 3)) + " closes");
    if (const std::function<void(std::string)> late = c.unanswered(0)) late("too late");
    settleHost(*c.host);
    CHECK(c.host->diagnostics().dead == std::optional<std::string>(kPlatformLate));
    for (const uint32_t runtime : {1u, 2u}) CHECK(counted.readOf(wire::Stop, runtime) == 0 && counted.readOf(wire::Close, runtime) == 0);
    CHECK_MSG(!a.host->diagnostics().dead && !b.host->diagnostics().dead && !peers.events.ended(), joined(peers.events.all()));
    printf("    A's request answered 4 s after it under its hold; C's dropped one ended it %.2f s after its release\n", afterRelease);

    KeptObservers heard;
    heard.hold = "kept ask";
    std::unique_ptr<HostedKeptRenderer> kept = a.host->keepRenderer(heard.observers());
    CHECK(kept);
    if (!kept) return;
    const auto keptAt = std::chrono::steady_clock::now();
    waitA->hold(keptAt + 2s);
    kept->sendPlatform("ask:kept ask");
    CHECK_MSG(eventually([&] { return heard.held(0) != nullptr; }), joined(heard.events()));
    std::this_thread::sleep_until(keptAt + 4s);
    CHECK_MSG(!kept->ended() && heard.ends().empty(), joined(heard.events()));
    if (const std::function<void(std::string)> respond = heard.held(0)) respond("kept answer");
    CHECK_MSG(eventually([&] { return heard.heard("got:kept answer"); }), joined(heard.events()));
    CHECK_MSG(!kept->ended() && heard.ends().empty() && secondsSince(keptAt) >= 4, joined(heard.events()));
    kept->release();
}

/// A served host's layout query over real ends leaves its glue's platform wait out of its 3 s and of the widget's query budget: A's glue asks its platform inside the query a tap makes and A's host answers 4 s later under a published hold, and A's widget gets its renderer's 77 px and lives; C's glue is slow 3.5 s inside the same query with no request of its own, and C's widget gets no 77 and stops at its query budget; B, on the same stream, goes on meanwhile.
TEST(remote_stream_hosted_served_query_leaves_out_its_glues_platform_wait) {
    using namespace std::chrono_literals;
    StreamPeers peers(kBuild, *RES, nullptr, [](std::shared_ptr<SandboxResources>, const std::string&, double width, std::shared_ptr<SandboxHostedPlatform> platform) -> std::unique_ptr<Renderer> {
        auto renderer = std::make_unique<HostedTestRenderer>(std::move(platform));
        renderer->queryResult = width == 302 ? kTallAnswer : kRect77;
        renderer->askOnQuery = width == 301;
        renderer->queryStallMs = width == 303 ? 3500 : 0;
        return renderer;
    });
    const auto waitA = std::make_shared<SandboxHostedWait>();
    HostedStreamHost a(*peers.end, kMeasuredOnTap, WidgetHostOptions(), 301, "INTL-A", false, nullptr, waitA);
    HostedStreamHost b(*peers.end, kHostWidget, WidgetHostOptions(), 302, "INTL-B");
    HostedStreamHost c(*peers.end, kMeasuredOnTap, WidgetHostOptions(), 303, "INTL-C");
    CHECK(a.shown() && b.shown() && c.shown());
    waitA->hold(std::chrono::steady_clock::now() + 2s);
    const auto tapped = std::chrono::steady_clock::now();
    a.host->sendPlatform("tap");
    c.host->sendPlatform("tap");
    CHECK_MSG(eventually([&] { return a.requests().size() == 1 && a.requests()[0].rfind("query:", 0) == 0; }), joined(a.requests()));
    b.host->sendPlatform("say:meanwhile");
    CHECK_MSG(eventually([&] { return b.messages() == std::vector<std::string>{"meanwhile"}; }), joined(b.messages()));
    CHECK(b.host->probe("d.textContent") == "h:20");
    std::this_thread::sleep_until(tapped + 4s);
    if (const std::function<void(std::string)> respond = a.unanswered(0)) respond("measured");
    CHECK_MSG(eventually([&] { return widthsLogged(a.host->diagnostics()) == std::vector<std::string>{"w=77"}; }), joined(widthsLogged(a.host->diagnostics())));
    const double answeredAfter = secondsSince(tapped);
    CHECK(probeUntil(*a.host, "got.join(',')", "77") == "77");
    settleHost(*a.host);
    CHECK_MSG(!a.host->diagnostics().dead && answeredAfter >= 4, a.host->diagnostics().dead.value_or("alive") + ", " + std::to_string(answeredAfter) + " s");

    CHECK(c.host->waitUntil(died, 10));
    const std::string reason = c.host->diagnostics().dead.value_or("");
    CHECK_MSG(reason.rfind("Layout query limit exceeded: host queries blocked the renderer", 0) == 0, reason);
    const std::vector<std::string> widths = widthsLogged(c.host->diagnostics());
    CHECK_MSG(std::find(widths.begin(), widths.end(), "w=77") == widths.end(), joined(widths));
    CHECK_MSG(!b.host->diagnostics().dead && !peers.events.ended(), joined(peers.events.all()));
    printf("    A's query behind its 4 s platform wait answered %.2f s after the tap; C's slow glue: %s\n", answeredAfter, widths.empty() ? "no width logged" : widths[0].c_str());
}

/// The bridge call of runtime `runtime` an engine end writes by hand.
static std::string bridgeOn(uint32_t runtime, const std::string& method, const std::string& args) { return FrameBytes(runtime, wire::Bridge).str(method).str(args).bytes(); }

/// One frame of each kind a hosted runtime may send on `runtime` but its renderer's platform traffic, ending with an Intl request: the calls a kept route takes and drops.
static std::string keptRouteJunk(uint32_t runtime) {
    FrameBytes stats(runtime, wire::Stats);
    for (size_t i = 0; i < SandboxRemoteCoding::kStatsFields; i++) stats.f64(0);
    return bridgeOn(runtime, "openLink", linkArgs("https://example.com/kept")) + bridgeOn(runtime, "haptic", "[\"selection\"]") + FrameBytes(runtime, wire::CopyText).str("kept").bytes() +
           FrameBytes(runtime, wire::Request).i64(1).str("kept").boolean(0).bytes() + FrameBytes(runtime, wire::Wallet).i64(2).str("{}").bytes() +
           FrameBytes(runtime, wire::Storage).str("{\"local\":{\"k\":\"v\"}}").bytes() + FrameBytes(runtime, wire::ResizeAnchor).str("bottom").bytes() +
           FrameBytes(runtime, wire::WatchesPublicData).boolean(1).bytes() + bare(runtime, wire::Loaded) + stats.bytes() + logFrame(runtime, "kept") +
           FrameBytes(runtime, wire::Diag).str("kind").str("kept").bytes() + FrameBytes(runtime, wire::ValidatedOps).str("[]").bytes() + bare(runtime, wire::RendererReady) +
           bare(runtime, wire::Rendered) + FrameBytes(runtime, wire::Nodes).i64(3).bytes() + FrameBytes(runtime, wire::Died).str("later").bytes() + bare(runtime, wire::TaskStarted) +
           bare(runtime, wire::TaskCompleted) + intlFrame(runtime, "{\"kind\":\"number\",\"value\":1}");
}

/// The host's frames of `runtime` that begin with `prefix`, as `describeHost` gives them.
static std::vector<std::string> framesWith(ScriptedEngine& engine, uint32_t runtime, const std::string& prefix) {
    std::vector<std::string> out;
    for (const std::string& frame : engine.frames(runtime))
        if (frame.rfind(prefix, 0) == 0) out.push_back(frame);
    return out;
}

/// A lying engine end against kept routes (ScriptedEngine on a hosting stream): three hosted WidgetHosts with `keepsRenderer` whose renderers announced themselves and which then, by the engine end's word, died are kept, with no `Stop` or `Close` at the keep, and destroyed. On a kept route every kind a hosted runtime may send but its platform traffic has no effect (the bridge's link and haptic, copyText, a request, a wallet call, storage, the resize anchor, the public data watch, loaded, stats, a log, a diagnostic, validated ops, renderer ready, rendered, nodes, a death, a task) and the host writes no `Answer`, `Stop` or `Close` for it; its Intl request is answered "", the handle hears only the platform message after them and the backlog returns to 0; a platform message past `kRemotePlatformLimit` within the frame bound is dropped and the next one heard. A request the handle leaves unanswered ends that route 3 to 4 s later (`onEnded` with the 3 s reason, `Close` alone: its widget is dead, and no answer for a late `respond`) while the second runtime still hears its messages; a flood on the third while its handle's thread is held ends that route once (`Close` alone, the backlog within the bounds and then 0, `onEnded` flooded after every message the client took), the stream going on for the second; a second waiting request on the second ends the stream as on a live route, its handle hears `onEnded("the widget engine process ended")`, and no handle hears a second end. On a fresh stream a malformed frame on a kept route ends it: that kept handle hears the same, and a live widget with `keepsRenderer` killed by it keeps nothing.
TEST(remote_stream_hosted_kept_route_against_a_lying_engine_end) {
    const std::string lied = "CPU limit exceeded (by the engine end's word)";
    ScriptedEngine engine({}, true);
    Gate holding;
    KeptObservers heard[3];
    heard[0].hold = "never";
    heard[1].hold = "first";
    heard[2].block = "hold";
    heard[2].gate = &holding;
    std::unique_ptr<HostedKeptRenderer> kept[3];
    {
        std::vector<std::unique_ptr<WidgetHost>> hosts;
        for (int i = 0; i < 3; i++) {
            hosts.push_back(std::make_unique<WidgetHost>("<p>x</p>", WidgetTheme{}, 300, WidgetHostOptions(), engine.host().hostedChannel()));
            hosts.back()->keepsRenderer = true;
            hosts.back()->start();
        }
        CHECK_MSG(eventually([&] { return engine.saw(1, "hosted-start") && engine.saw(2, "hosted-start") && engine.saw(3, "hosted-start"); }), joined(engine.frames(3)));
        CHECK(engine.ready());
        std::string born;
        for (uint32_t runtime = 1; runtime <= 3; runtime++) born += bare(runtime, wire::RendererReady) + FrameBytes(runtime, wire::Died).str(lied).bytes();
        CHECK(engine.send(born));
        for (int i = 0; i < 3; i++) {
            CHECK_MSG(hosts[i]->waitUntil(died, 5) && hosts[i]->diagnostics().dead == std::optional<std::string>(lied), hosts[i]->diagnostics().dead.value_or("alive"));
            kept[i] = hosts[i]->keepRenderer(heard[i].observers());
            CHECK_MSG(kept[i], "runtime " + std::to_string(i + 1) + " kept nothing");
        }
    }
    if (!kept[0] || !kept[1] || !kept[2]) {
        holding.open();
        return;
    }

    CHECK(engine.send(keptRouteJunk(1)));
    CHECK_MSG(eventually([&] { return engine.saw(1, "intl-answer:"); }), joined(engine.frames(1)));
    CHECK(engine.send(platformMessageFrame(1, "only this")));
    CHECK_MSG(eventually([&] { return !heard[0].events().empty(); }), "nothing heard");
    CHECK_MSG(heard[0].events() == std::vector<std::string>{"message:only this"}, joined(heard[0].events()));
    CHECK_MSG(eventually([&] { return backlogIs(*kept[0], 0, 0); }), backlogText(*kept[0]));
    for (uint32_t runtime = 1; runtime <= 3; runtime++)
        CHECK_MSG(framesWith(engine, runtime, "stop:").empty() && framesWith(engine, runtime, "close").empty() && framesWith(engine, runtime, "answer:").empty(), joined(engine.frames(runtime)));
    const size_t limit = SandboxEngine::kRemotePlatformLimit;
    CHECK(engine.send(platformMessageFrame(1, std::string(limit + 1, 'p')) + platformMessageFrame(1, "next")));
    CHECK_MSG(eventually([&] { return heard[0].heard("next"); }, 10), joined(heard[0].events()));
    CHECK_MSG((heard[0].messages() == std::vector<std::string>{"only this", "next"}), joined(heard[0].events()));

    // An unanswered request ends its route alone, after 3 s; the second runtime is heard meanwhile.
    const auto asked = std::chrono::steady_clock::now();
    CHECK(engine.send(platformRequestFrame(1, "never")));
    CHECK_MSG(eventually([&] { return has(heard[0].events(), "request:never"); }), joined(heard[0].events()));
    for (int i = 1; i <= 3; i++) {
        const std::string message = "meanwhile " + std::to_string(i);
        CHECK(engine.send(platformMessageFrame(2, message)));
        CHECK_MSG(eventually([&] { return heard[1].heard(message); }), joined(heard[1].events()));
    }
    const double meanwhile = msSince(asked);
    CHECK_MSG(meanwhile < 3000 && !kept[0]->ended(), std::to_string(meanwhile) + " ms");
    CHECK_MSG(eventually([&] { return heard[0].endedAt().has_value(); }, 6), joined(heard[0].events()));
    const double waited = std::chrono::duration<double>(heard[0].endedAt().value_or(asked) - asked).count();
    CHECK_MSG(waited >= SandboxEngine::kPlatformAnswerSeconds && waited < SandboxEngine::kPlatformAnswerSeconds + 1, std::to_string(waited) + " s");
    printf("    the kept route's unanswered request ended it %.2f s after it\n", waited);
    CHECK_MSG(heard[0].ends() == std::vector<std::string>{kPlatformLate} && kept[0]->ended(), joined(heard[0].events()));
    CHECK_MSG(eventually([&] { return framesWith(engine, 1, "close").size() == 1; }), joined(engine.frames(1)));
    if (const std::function<void(std::string)> late = heard[0].held(0)) late("too late");
    CHECK_MSG(!kept[1]->ended() && !kept[2]->ended() && heard[1].ends().empty(), joined(heard[1].events()));

    // A flood on the third while its handle's thread is held.
    CHECK(engine.send(platformMessageFrame(3, "hold")));
    CHECK(holding.waitedOn());
    std::atomic<bool> flooding{true};
    std::atomic<size_t> maxBytes{0}, maxCalls{0};
    Background<bool> watching([&] {
        while (flooding) {
            const SandboxEngine::RemoteBacklog b = kept[2]->remoteBacklog();
            maxBytes = std::max(maxBytes.load(), b.bytes);
            maxCalls = std::max(maxCalls.load(), b.calls);
            std::this_thread::yield();
        }
        return true;
    });
    const size_t total = SandboxEngine::kRemoteQueuedCalls + 256;
    std::string batch;
    size_t during = 0;
    for (size_t i = 1; i <= total; i++) {
        batch += platformMessageFrame(3, "f" + std::to_string(i));
        if (i % 2048 && i != total) continue;
        batch += platformMessageFrame(2, "during " + std::to_string(++during));
        CHECK(engine.send(batch));
        batch.clear();
    }
    CHECK_MSG(eventually([&] { return heard[1].heard("during " + std::to_string(during)); }, 10), joined(heard[1].messages("during ")));
    CHECK_MSG(eventually([&] { return !framesWith(engine, 3, "close").empty(); }, 10), joined(engine.frames(3)));
    flooding = false;
    CHECK(watching.finish(5));
    std::vector<std::string> duringAll;
    for (size_t i = 1; i <= during; i++) duringAll.push_back("during " + std::to_string(i));
    CHECK_MSG(heard[1].messages("during ") == duringAll, firstDifference(heard[1].messages("during "), duringAll));
    CHECK_MSG(framesWith(engine, 3, "close").size() == 1 && framesWith(engine, 3, "stop:").empty() && kept[2]->ended(), joined(engine.frames(3)));
    // Held at the bound: "hold" and the first messages after it, `kRemoteQueuedCalls` in all; the next refused, the route ended.
    size_t heldBytes = std::string("hold").size();
    for (size_t i = 1; i < SandboxEngine::kRemoteQueuedCalls; i++) heldBytes += 1 + std::to_string(i).size();
    const size_t tried = 1 + std::to_string(SandboxEngine::kRemoteQueuedCalls).size();
    CHECK_MSG(backlogIs(*kept[2], heldBytes, SandboxEngine::kRemoteQueuedCalls), backlogText(*kept[2]));
    CHECK_MSG(maxCalls <= SandboxEngine::kRemoteQueuedCalls + 1 && maxBytes <= heldBytes + tried && heldBytes + tried < SandboxEngine::kRemoteQueuedBytes,
              std::to_string(maxBytes) + " bytes, " + std::to_string(maxCalls) + " calls");
    CHECK_MSG(heard[2].ends().empty(), joined(heard[2].ends()));
    holding.open();
    CHECK_MSG(eventually([&] { return backlogIs(*kept[2], 0, 0); }, 10), backlogText(*kept[2]));
    CHECK_MSG(eventually([&] { return !heard[2].ends().empty(); }, 10), "no end heard");
    const std::vector<std::string> flooded = heard[2].events();
    CHECK_MSG(flooded.size() == SandboxEngine::kRemoteQueuedCalls + 1 && flooded.front() == "message:hold" && flooded.back() == "ended:" + kFloodedReason && heard[2].ends().size() == 1,
              std::to_string(flooded.size()) + " events, the last " + (flooded.empty() ? std::string() : flooded.back()));
    CHECK_MSG(!engine.events().ended(), joined(engine.events().all()));
    CHECK(engine.send(platformMessageFrame(2, "after the flood")));
    CHECK_MSG(eventually([&] { return heard[1].heard("after the flood"); }), joined(heard[1].messages("after")));

    // A second waiting request on the second ends the stream, as on a live route.
    CHECK(engine.send(platformRequestFrame(2, "first")));
    CHECK_MSG(eventually([&] { return has(heard[1].events(), "request:first"); }), joined(heard[1].events()));
    CHECK(engine.send(platformRequestFrame(2, "second")));
    CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }, 2), joined(engine.events().all()));
    CHECK_MSG(endsOnce(engine.events().all(), "kind 97 on runtime 2 while its platform request is unanswered"), joined(engine.events().all()));
    CHECK_MSG(eventually([&] { return !heard[1].ends().empty(); }), joined(heard[1].events()));
    CHECK_MSG(heard[1].ends() == std::vector<std::string>{kEngineGone} && heard[1].events().back() == "ended:" + kEngineGone && kept[1]->ended(), joined(heard[1].events()));
    if (const std::function<void(std::string)> late = heard[1].held(0)) late("too late");
    for (KeptObservers& h : heard) CHECK_MSG(h.ends().size() == 1, joined(h.ends()));
    for (uint32_t runtime = 1; runtime <= 3; runtime++)
        CHECK_MSG(framesWith(engine, runtime, "platform-answer:").empty() && framesWith(engine, runtime, "answer:").empty() && framesWith(engine, runtime, "stop:").empty(), joined(engine.frames(runtime)));
    for (std::unique_ptr<HostedKeptRenderer>& k : kept) k->release();
    for (KeptObservers& h : heard) CHECK_MSG(h.ends().size() == 1, joined(h.ends()));

    // A malformed frame on the kept route of a fresh stream: that route learns the end, and the live widget it killed keeps nothing.
    ScriptedEngine fresh({}, true);
    KeptObservers keptHeard, liveHeard;
    std::unique_ptr<HostedKeptRenderer> handle;
    auto dying = std::make_unique<WidgetHost>("<p>x</p>", WidgetTheme{}, 300, WidgetHostOptions(), fresh.host().hostedChannel());
    WidgetHost live("<p>x</p>", WidgetTheme{}, 300, WidgetHostOptions(), fresh.host().hostedChannel());
    dying->keepsRenderer = live.keepsRenderer = true;
    dying->start();
    live.start();
    CHECK_MSG(eventually([&] { return fresh.saw(1, "hosted-start") && fresh.saw(2, "hosted-start"); }), joined(fresh.frames(2)));
    CHECK(fresh.ready());
    CHECK(fresh.send(bare(1, wire::RendererReady) + FrameBytes(1, wire::Died).str(lied).bytes() + bare(2, wire::RendererReady)));
    CHECK(dying->waitUntil(died, 5));
    CHECK(live.waitUntil([](const WidgetDiagnostics& d) { return d.rendererReadyMs.has_value(); }, 5) && !live.diagnostics().dead);
    handle = dying->keepRenderer(keptHeard.observers());
    CHECK(handle);
    dying.reset();
    CHECK(fresh.send(FrameBytes(1, wire::Nodes).raw("x").bytes()));
    CHECK_MSG(eventually([&] { return fresh.events().ended().has_value(); }, 2), joined(fresh.events().all()));
    CHECK_MSG(endsOnce(fresh.events().all(), "kind 101 with a field past the frame's end"), joined(fresh.events().all()));
    CHECK_MSG(eventually([&] { return !keptHeard.ends().empty(); }), joined(keptHeard.events()));
    CHECK_MSG(keptHeard.ends() == std::vector<std::string>{kEngineGone} && handle && handle->ended(), joined(keptHeard.events()));
    CHECK_MSG(live.waitUntil(died, 5) && live.diagnostics().dead == std::optional<std::string>(kEngineGone), live.diagnostics().dead.value_or("alive"));
    CHECK(!live.keepRenderer(liveHeard.observers()));
    CHECK(liveHeard.count() == 0);
    if (handle) handle->release();
}

/// What an engine end's renderer did with its platform, kept by the test past the renderer: each message it sent and each request's answer, in order, under a lock.
struct PlatformTap {
    std::mutex mutex;
    std::vector<std::string> sent;
    std::vector<std::optional<std::string>> answers;

    bool hasSent(const std::string& message) {
        std::lock_guard<std::mutex> l(mutex);
        return std::find(sent.begin(), sent.end(), message) != sent.end();
    }
    std::vector<std::optional<std::string>> answered() {
        std::lock_guard<std::mutex> l(mutex);
        return answers;
    }
};

/// A renderer's platform that tells its tap what the renderer sends and what its requests got, and passes every call on to the runtime's platform (whose messages it hands to its own `onMessage`); it goes with its renderer, once the glue's thread, the only one that calls `onMessage`, has ended.
class TappedPlatform final : public SandboxHostedPlatform {
public:
    TappedPlatform(std::shared_ptr<SandboxHostedPlatform> platform, std::shared_ptr<PlatformTap> tap) : platform_(std::move(platform)), tap_(std::move(tap)) {
        platform_->onMessage = [this](const std::string& message) {
            if (onMessage) onMessage(message);
        };
    }
    ~TappedPlatform() override { platform_->onMessage = nullptr; }
    TappedPlatform(const TappedPlatform&) = delete;
    TappedPlatform& operator=(const TappedPlatform&) = delete;

    void send(const std::string& message) override {
        {
            std::lock_guard<std::mutex> l(tap_->mutex);
            tap_->sent.push_back(message);
        }
        platform_->send(message);
    }
    std::optional<std::string> request(const std::string& message) override {
        std::optional<std::string> answer = platform_->request(message);
        {
            std::lock_guard<std::mutex> l(tap_->mutex);
            tap_->answers.push_back(answer);
        }
        return answer;
    }

private:
    const std::shared_ptr<SandboxHostedPlatform> platform_;
    const std::shared_ptr<PlatformTap> tap_;
};

/// A glue thread's wait holds up no other widget: two hosted widgets on one stream between real ends. A's renderer waits on its glue's thread inside `onMessage` (`wait:`) for a request A's host keeps unanswered; meanwhile B, built after that, whose renderer also asks inside the glue's call that hands it each batch (`askOnOps`), loads, renders with its layout answer and Intl, answers a probe and exchanges a `say:` and a `wait:` both ways, all within 3 s of A's request. A dies 3 to 4 s after it with `platform-timeout`, its renderer's request on the glue's thread returns nullopt (it sends `waited:none`) and the renderer goes; `Stop` and `Close` crossed for A only, a late answer changes nothing, B goes on and the stream does not end.
TEST(remote_stream_hosted_glue_thread_waits_hold_up_no_other_widget) {
    const auto tapA = std::make_shared<PlatformTap>(), tapB = std::make_shared<PlatformTap>();
    const auto goneA = std::make_shared<std::atomic<bool>>(false), goneB = std::make_shared<std::atomic<bool>>(false);
    StreamPeers peers(kBuild, *RES, nullptr, [&](std::shared_ptr<SandboxResources>, const std::string&, double width, std::shared_ptr<SandboxHostedPlatform> platform) -> std::unique_ptr<Renderer> {
        const bool a = width == 301;
        auto renderer = std::make_unique<HostedTestRenderer>(std::make_shared<TappedPlatform>(std::move(platform), a ? tapA : tapB));
        tall(*renderer);
        renderer->askOnOps = !a;
        renderer->gone = a ? goneA : goneB;
        return renderer;
    });
    CountingStream& counted = *peers.counted;
    HostedStreamHost a(*peers.end, kHostWidget, WidgetHostOptions(), 301, "INTL-A", false);
    CHECK(a.shown() && a.renders("h:20"));
    const auto since = std::chrono::steady_clock::now();
    a.host->sendPlatform("wait:held");
    CHECK_MSG(eventually([&] { return a.requests() == std::vector<std::string>{"held"}; }), joined(a.requests()));

    HostedStreamHost b(*peers.end, kHostWidget, WidgetHostOptions(), 302, "INTL-B");
    CHECK(b.shown());
    CHECK_MSG(b.renders("h:20") && b.renders("n:INTL-B"), b.rendered().substr(0, 500));
    CHECK(b.host->probe("d.textContent") == "h:20");
    b.host->sendPlatform("say:b");
    b.host->sendPlatform("wait:b");
    CHECK_MSG(eventually([&] { return has(b.messages(), "b") && has(b.messages(), "waited:answer:b"); }), joined(b.messages()));
    const double meanwhile = msSince(since);
    CHECK_MSG(meanwhile < 3000 && !a.host->diagnostics().dead, std::to_string(meanwhile) + " ms after A's request");
    size_t laid = 0;
    for (const std::string& m : b.messages()) {
        if (m.rfind("laid:", 0) != 0) continue;
        const std::string n = std::to_string(++laid);
        CHECK_MSG(m == "laid:" + n + ":answer:laid:" + n, m);
    }
    CHECK_MSG(laid > 0, joined(b.messages()));

    CHECK(a.host->waitUntil(died, 6));
    const double waited = msSince(since) / 1000;
    CHECK_MSG(waited >= WidgetHost::kPlatformAnswerSeconds && waited < WidgetHost::kPlatformAnswerSeconds + 1, std::to_string(waited) + " s");
    printf("    B shown and answered %.0f ms after A's request; A dead %.2f s after it\n", meanwhile, waited);
    const WidgetDiagnostics d = a.host->diagnostics();
    CHECK_MSG(d.dead == std::optional<std::string>(kPlatformLate), d.dead.value_or("alive"));
    CHECK(diagsOf(d, "platform-timeout") == std::vector<std::string>{kPlatformLate});
    CHECK_MSG(eventually([&] { return tapA->hasSent("waited:none"); }), "A's renderer is still waiting");
    CHECK(tapA->answered() == std::vector<std::optional<std::string>>{std::nullopt});
    CHECK_MSG(eventually([&] { return goneA->load(); }), "A's renderer is still in the engine end");
    CHECK_MSG(eventually([&] { return counted.readOf(wire::Stop, 1) == 1 && counted.readOf(wire::Close, 1) == 1; }),
              std::to_string(counted.readOf(wire::Stop, 1)) + " stops, " + std::to_string(counted.readOf(wire::Close, 1)) + " closes");
    if (const std::function<void(std::string)> late = a.unanswered(0)) late("too late");
    settleHost(*a.host);
    CHECK(a.host->diagnostics().dead == std::optional<std::string>(kPlatformLate));

    CHECK(b.host->probe("1 + 1") == "2");
    b.host->sendPlatform("say:b2");
    CHECK_MSG(eventually([&] { return has(b.messages(), "b2"); }), joined(b.messages()));
    CHECK(counted.readOf(wire::Stop, 2) == 0 && counted.readOf(wire::Close, 2) == 0 && !*goneB);
    CHECK(eventually([&] { return peers.events.open() == 1; }));
    CHECK_MSG(!b.host->diagnostics().dead && !peers.events.ended(), joined(peers.events.all()));
}

#ifndef _WIN32
/// The lines of a child's output its case printed for the parent (indented by four spaces), each with its newline.
static std::string childLines(const std::string& output) {
    std::string lines;
    for (size_t at = 0; at < output.size();) {
        const size_t end = std::min(output.find('\n', at), output.size());
        const std::string line = output.substr(at, end - at);
        if (line.rfind("    ", 0) == 0) lines += line + "\n";
        at = end + 1;
    }
    return lines;
}

/// The child of `remote_stream_hosted_engine_process_reads_no_file`, the engine process of a hosting stream on the socket it inherited: `serveHosting` with HostedTestRenderers (layout answers `kTallAnswer`), its lockdown counting its calls and leaving it no descriptor to open (`RLIMIT_NOFILE` {0, 0}, after which an open of /dev/null must fail). It prints the lockdown's calls and the Ready frames written before and after it, the renderers it built and how many of them before it, and why serving ended.
CHILD_CASE(remote_stream_hosted_engine_process) {
    const auto stream = std::make_shared<CountingStream>(std::make_shared<SocketStream>(kChildStreamFd));
    std::atomic<int> lockdowns{0}, built{0}, early{0};
    std::atomic<size_t> readyBefore{0};
    const std::string served = SandboxRemoteEngineEnd::serveHosting(
        *stream, kBuild,
        [&] {
            lockdowns++;
            readyBefore = stream->writtenOf(wire::Ready);
            const rlimit none = {0, 0};
            if (::setrlimit(RLIMIT_NOFILE, &none) != 0) return std::string("setrlimit: ") + std::strerror(errno);
            const int fd = ::open("/dev/null", O_RDONLY);
            if (fd >= 0) {
                ::close(fd);
                return std::string("a descriptor could still be opened");
            }
            printf("    locked down: no descriptor to open (%s)\n", std::strerror(errno));
            fflush(stdout);
            return std::string();
        },
        [&](std::shared_ptr<SandboxResources>, const std::string&, double, std::shared_ptr<SandboxHostedPlatform> platform) -> std::unique_ptr<Renderer> {
            if (!lockdowns) early++;
            built++;
            auto renderer = std::make_unique<HostedTestRenderer>(std::move(platform));
            tall(*renderer);
            return renderer;
        });
    printf("    lockdown calls %d, Ready frames before it %zu and after serving %zu, renderers %d, %d of them before it; served until \"%s\"\n", lockdowns.load(), readyBefore.load(),
           stream->writtenOf(wire::Ready), built.load(), early.load(), served.c_str());
    fflush(stdout);
    CHECK(lockdowns == 1 && readyBefore == 0 && stream->writtenOf(wire::Ready) == 1 && early == 0 && built > 0);
}

/// The engine process of a hosting stream reads no file: a fresh child of the suite's (`remote_stream_hosted_engine_process`) serves `serveHosting` on its descriptor 3 and, locked down once before its Ready and before any widget, has no descriptor left to open, while two hosted WidgetHosts here load and render and their taps get the widget's new text in the validated batches they asked for, so the child built each validator, engine and renderer from the opening's bytes alone; it exits 0. Control: an opening whose validator bundle carries another release's stamp gives that widget `bundle-mismatch` with the in-process refusal and no render: the child used the bytes it was given, not a file.
TEST(remote_stream_hosted_engine_process_reads_no_file) {
    const char* html = "<button id='b'>go</button><p id='p'>before</p><script>document.getElementById('b').onclick = function () { document.getElementById('p').textContent = 'tapped'; };</script>";
    for (const bool restamped : {false, true}) {
        const std::string name = restamped ? "another validator stamp: " : "the shipped bundles: ";
        SandboxResources resources = *RES;
        if (restamped) {
            const BundleStamp mine = BundleStamp::glue();
            resources.validatorBundle = BundleStamp{"9.9.9", mine.protocol, mine.profile + 1, mine.sdk}.line() + RES->validatorBundle.substr(RES->validatorBundle.find('\n'));
        }
        const std::string refusal = BundleStamp::refusal(resources).value_or("");
        CHECK_MSG(restamped == !refusal.empty(), name + refusal);
        SocketStreamPair pair = socketStreamPair();
        CHECK(pair.host && pair.engine);
        if (!pair.host || !pair.engine) return;
        Background<ChildRun> child([&] { return runChildOn("remote_stream_hosted_engine_process", *pair.engine, 60); });
        EndEvents events;
        const std::shared_ptr<SandboxRemoteHostEnd> end = SandboxRemoteHostEnd::startHosting(pair.host, resources, kBuild, events.events(), {});
        const size_t widgets = restamped ? 1 : 2;
        {
            std::vector<std::unique_ptr<HostedStreamHost>> hosts;
            for (size_t i = 0; i < widgets; i++) hosts.push_back(std::make_unique<HostedStreamHost>(*end, html, WidgetHostOptions(), 300, "INTL"));
            for (const std::unique_ptr<HostedStreamHost>& h : hosts) {
                if (restamped) {
                    CHECK_MSG(h->host->waitUntil(died, 30), name + "still alive");
                    settleHost(*h->host);
                    const WidgetDiagnostics d = h->host->diagnostics();
                    const std::vector<std::string> mismatches = diagsOf(d, "bundle-mismatch");
                    CHECK_MSG(!mismatches.empty() && std::all_of(mismatches.begin(), mismatches.end(), [&](const std::string& m) { return m == refusal; }), name + joined(mismatches));
                    CHECK_MSG(d.nodes == 0 && h->rendered().empty() && !d.rendered, name + h->rendered().substr(0, 200));
                    continue;
                }
                CHECK_MSG(h->shown() && h->renders("before"), name + h->rendered().substr(0, 500));
                h->host->sendPlatform("tap");
                CHECK_MSG(eventually([&] { return h->renders("tapped"); }, 10), name + h->rendered().substr(0, 500));
            }
        }
        CHECK_MSG(eventually([&] { return events.open() == 0; }), name + joined(events.all()));
        end->stop();
        const ChildRun run = child.finish(60);
        const std::string lines = childLines(run.output);
        printf("%s", lines.c_str());
        CHECK_MSG(run.status == 0, name + std::to_string(run.status) + ": " + run.output);
        const std::string report = "    lockdown calls 1, Ready frames before it 0 and after serving 1, renderers " + std::to_string(widgets) + ", 0 of them before it;";
        CHECK_MSG(lines.find("    locked down: no descriptor to open") != std::string::npos && lines.find(report) != std::string::npos, name + lines);
    }
}

/// `name` as the thread inventory lists it: Linux keeps a thread name's first 15 bytes.
static std::string listed(const std::string& name) {
#ifdef __linux__
    return name.substr(0, 15);
#else
    return name;
#endif
}

/// The child of `remote_stream_hosted_thread_process`, a host whose widgets are all hosted, on the stream it inherited: three hosted WidgetHosts load and render with their Intl formatted here on `widget.client`; this process then has no `widget.validator` and no `widget.sandbox` thread, three `twidget-host`, three `widget.client`, three `widget.answer` and the transport's own, and one local WidgetHost adds exactly one of each of the first two (the inventory sees them when they exist). It prints its process id and both inventories of names.
CHILD_CASE(remote_stream_hosted_host_process) {
    EndEvents events;
    const std::shared_ptr<SandboxRemoteHostEnd> end = SandboxRemoteHostEnd::startHosting(std::make_shared<SocketStream>(kChildStreamFd), *RES, kBuild, events.events(), {});
    printf("    child %ld\n", processId());
    {
        std::vector<std::unique_ptr<HostedStreamHost>> hosts;
        for (const char* letter : {"A", "B", "C"}) hosts.push_back(std::make_unique<HostedStreamHost>(*end, kHostWidget, WidgetHostOptions(), 300, std::string("INTL-") + letter));
        for (const std::unique_ptr<HostedStreamHost>& h : hosts) {
            const std::string answer = h->intl->answer;
            CHECK_MSG(h->host->isHosted() && h->shown() && h->renders("h:20") && h->renders("n:" + answer), answer + ": " + h->rendered().substr(0, 500));
#if defined(__APPLE__) || defined(__linux__)
            CHECK_MSG(h->intl->threads() == std::vector<std::string>{"widget.client"}, answer + ": " + joined(h->intl->threads()));
#endif
        }
        CHECK_MSG(eventually([&] { return events.open() == 3; }), joined(events.all()));
#if defined(__APPLE__) || defined(__linux__)
        const std::optional<std::map<std::string, int>> hosted = threadNames();
        CHECK_MSG(hosted.has_value(), "this process's threads could not be listed");
        if (hosted) {
            printf("    hosted only: %s\n", namesText(*hosted).c_str());
            CHECK_MSG(countOf(*hosted, listed("widget.validator")) == 0 && countOf(*hosted, "widget.sandbox") == 0, namesText(*hosted));
            CHECK_MSG(countOf(*hosted, "twidget-host") == 3 && countOf(*hosted, "widget.client") == 3 && countOf(*hosted, "widget.answer") == 3, namesText(*hosted));
            CHECK_MSG(countOf(*hosted, "widget.reader") == 1 && countOf(*hosted, "widget.writer") == 1 && countOf(*hosted, "widget.monitor") == 1, namesText(*hosted));
        }
        {
            ScriptedRenderer* renderer = nullptr;
            WidgetHost local("<p>x</p>", WidgetTheme{}, 300, WidgetHostOptions(), RES, scripted(renderer));
            local.start();
            CHECK(!local.isHosted() && local.waitLoaded(20));
            const std::optional<std::map<std::string, int>> mixed = threadNames();
            CHECK_MSG(mixed.has_value(), "this process's threads could not be listed");
            if (mixed) {
                printf("    with a local widget: %s\n", namesText(*mixed).c_str());
                CHECK_MSG(countOf(*mixed, listed("widget.validator")) == 1 && countOf(*mixed, "widget.sandbox") == 1 && countOf(*mixed, "twidget-host") == 4, namesText(*mixed));
            }
        }
#else
        printf("    thread names: not listed on this platform\n");
#endif
        fflush(stdout);
    }
    CHECK_MSG(eventually([&] { return events.open() == 0; }), joined(events.all()));
    end->stop();
    CHECK_MSG(endsOnce(events.all(), "the host stopped the stream"), joined(events.all()));
}

/// A host process whose widgets are all hosted has no validator and no engine thread: a fresh child of the suite's (`remote_stream_hosted_host_process`) gets one end of a socket pair as its descriptor 3 while this process serves `serveHosting` on the other, where the child's three widgets' engines, validators and renderers run; the child's checks pass (its Intl in its own providers, its inventories), it exits 0 within 60 s with its process id and both inventories, this process built its three renderers, and serving returns within 5 s of the child's exit.
TEST(remote_stream_hosted_thread_process) {
    SocketStreamPair pair = socketStreamPair();
    CHECK(pair.host && pair.engine);
    if (!pair.host || !pair.engine) return;
    BuiltRenderers built;
    Background<std::string> serving([&] { return SandboxRemoteEngineEnd::serveHosting(*pair.engine, kBuild, {}, built.factory(tall)); });
    const ChildRun child = runChildOn("remote_stream_hosted_host_process", *pair.host, 60);
    CHECK_MSG(serving.within(5).has_value(), "serveHosting still runs 5 s after the child ended");
    const std::string lines = childLines(child.output);
    printf("%s", lines.c_str());
    CHECK_MSG(child.status == 0, std::to_string(child.status) + ": " + child.output);
    const size_t named = lines.find("    child ");
    const long childId = named == std::string::npos ? 0 : std::atol(lines.c_str() + named + 10);
    CHECK_MSG(childId > 0 && childId != processId(), lines);
#if defined(__APPLE__) || defined(__linux__)
    CHECK_MSG(lines.find("    hosted only: ") != std::string::npos && lines.find("    with a local widget: ") != std::string::npos, lines);
#endif
    CHECK(built.count() == 3);
}

/// This process's threads as `namesText` gives them, "unlisted" where the tests read no thread names.
static std::string threadInventory() {
#if defined(__APPLE__) || defined(__linux__)
    if (const std::optional<std::map<std::string, int>> names = threadNames()) return namesText(*names);
#endif
    return "unlisted";
}

/// The count `names` (`namesText`'s "name×count" list) gives `name`, 0 when it lists none.
static int countIn(const std::string& names, const std::string& name) {
    const std::string key = name + "×";
    for (size_t at = 0; at < names.size();) {
        const size_t end = std::min(names.find(' ', at), names.size());
        const std::string item = names.substr(at, end - at);
        if (item.rfind(key, 0) == 0) return std::atoi(item.c_str() + key.size());
        at = end + 1;
    }
    return 0;
}

/// The renderer's next `threads` report, asked through `send` and read from `messages` (what its host heard, in order); "" when none came within 5 s.
static std::string threadsThrough(const std::function<void(std::string)>& send, const std::function<std::vector<std::string>()>& messages) {
    const auto reports = [&] {
        std::vector<std::string> out;
        for (const std::string& m : messages())
            if (m.rfind("threads:", 0) == 0) out.push_back(m.substr(8));
        return out;
    };
    const size_t before = reports().size();
    send("threads");
    std::string report;
    eventually([&] {
        const std::vector<std::string> all = reports();
        if (all.size() <= before) return false;
        report = all[before];
        return true;
    });
    return report;
}

/// The child of `remote_stream_hosted_kept_engine_process_keeps_only_the_renderer`, an engine process serving `serveHosting` on the socket it inherited: its HostedTestRenderers (layout answers `kTallAnswer`) answer `threads` with this process's thread inventory, each report printed as it goes. It prints the renderers it built and why serving ended.
CHILD_CASE(remote_stream_hosted_kept_engine_process) {
    const auto stream = std::make_shared<SocketStream>(kChildStreamFd);
    std::atomic<int> built{0};
    const std::string served = SandboxRemoteEngineEnd::serveHosting(*stream, kBuild, nullptr,
                                                                    [&](std::shared_ptr<SandboxResources>, const std::string&, double, std::shared_ptr<SandboxHostedPlatform> platform) -> std::unique_ptr<Renderer> {
                                                                        built++;
                                                                        auto renderer = std::make_unique<HostedTestRenderer>(std::move(platform));
                                                                        tall(*renderer);
                                                                        renderer->inventory = [] {
                                                                            const std::string names = threadInventory();
                                                                            printf("    threads: %s\n", names.c_str());
                                                                            fflush(stdout);
                                                                            return names;
                                                                        };
                                                                        return renderer;
                                                                    });
    printf("    renderers %d; served until \"%s\"\n", built.load(), served.c_str());
    fflush(stdout);
    CHECK(built == 1);
}

/// A kept renderer leaves its engine process neither its engine nor its validator: a fresh child of the suite's (`remote_stream_hosted_kept_engine_process`) serves `serveHosting` on its descriptor 3 for one hosted WidgetHost here with `keepsRenderer`. While the widget lives, its renderer there reports one `widget.sandbox`, one `widget.validator` and one `twidget-host` thread in that process; once `keepRenderer` kept it and the WidgetHost is gone, the same renderer, asked through the handle, reports no `widget.sandbox` and no `widget.validator` and still its one `twidget-host`. Released, the route goes, and the child exits 0, having printed its inventories.
TEST(remote_stream_hosted_kept_engine_process_keeps_only_the_renderer) {
    SocketStreamPair pair = socketStreamPair();
    CHECK(pair.host && pair.engine);
    if (!pair.host || !pair.engine) return;
    Background<ChildRun> child([&] { return runChildOn("remote_stream_hosted_kept_engine_process", *pair.engine, 60); });
    EndEvents events;
    const std::shared_ptr<SandboxRemoteHostEnd> end = SandboxRemoteHostEnd::startHosting(pair.host, *RES, kBuild, events.events(), {});
    std::string live, kept;
    {
        KeptObservers heard;
        std::unique_ptr<HostedKeptRenderer> handle;
        {
            HostedStreamHost h(*end, "<p>kept</p>", WidgetHostOptions(), 300, "INTL", true, [](WidgetHost& w) { w.keepsRenderer = true; });
            CHECK_MSG(h.shown(), h.host->diagnostics().dead.value_or("not shown"));
            live = threadsThrough([&](std::string m) { h.host->sendPlatform(std::move(m)); }, [&] { return h.messages(); });
            handle = h.host->keepRenderer(heard.observers());
            CHECK(handle);
        }
        if (handle) {
            // The engine end's cut follows the keep's stop and joins the engine's thread, then the validator's: ask until a report comes after both left the inventory.
            CHECK_MSG(eventually([&] {
                          kept = threadsThrough([&](std::string m) { handle->sendPlatform(std::move(m)); }, [&] { return heard.messages(); });
                          return !kept.empty() && countIn(kept, "widget.sandbox") == 0 && countIn(kept, listed("widget.validator")) == 0;
                      }, 10),
                      kept);
            CHECK(!handle->ended());
            handle->release();
        }
    }
    CHECK_MSG(eventually([&] { return events.open() == 0; }), joined(events.all()));
    end->stop();
    const ChildRun run = child.finish(60);
    const std::string lines = childLines(run.output);
    printf("%s", lines.c_str());
    CHECK_MSG(run.status == 0, std::to_string(run.status) + ": " + run.output);
    CHECK_MSG(lines.find("    renderers 1; served until ") != std::string::npos, lines);
#if defined(__APPLE__) || defined(__linux__)
    CHECK_MSG(countIn(live, "widget.sandbox") == 1 && countIn(live, listed("widget.validator")) == 1 && countIn(live, "twidget-host") == 1, "live: " + live);
    CHECK_MSG(countIn(kept, "widget.sandbox") == 0 && countIn(kept, listed("widget.validator")) == 0 && countIn(kept, "twidget-host") == 1, "kept: " + kept);
    CHECK_MSG(lines.find("    threads: " + live + "\n") != std::string::npos && lines.find("    threads: " + kept + "\n") != std::string::npos, lines);
#else
    CHECK_MSG(live == "unlisted" && kept == "unlisted", live + " / " + kept);
#endif
}
#endif

// ───────────── threads the OS refuses to start (macOS: the per-process thread limit) ─────────────

#ifdef __APPLE__
/// How the engine's end words the death of a hosted widget it could not build, before why (here the refusal of one of its threads).
static const std::string kNotBuilt = "the widget could not be built in the engine process";

/// The engine's end of a hosting stream while no thread can start but those a leg leaves: the reaper (runtime 1 opened and closed), B (runtime 2, its probe thread made) and D (runtime 3, never probed), each with its glue's, validator's, renderer's and engine's threads, and every worker are made before the fill. With 0, 1, 2 and 3 threads free a hosted start (runtimes 4 to 7) is refused at its served host's `twidget-host`, its `widget.validator`, its renderer's thread (the test renderer's `hosted.asker`, inside the renderer factory) and its engine's `widget.sandbox`: its one death names that thread within 2 s; the task's thread count comes back to what the leg left with the same named threads (a renderer it built is gone), and the allowance is filled again. Then D's first probe answers "ERROR no runtime" and D is stopped with the refusal of its `widget.probe` within 2 s, in either order, its renderer still carrying its platform messages until its `Close`, then gone. B's probe, platform message and platform request still go through after each, B's threads and the reaper are all that is left, and the engine's end goes on serving.
static void hostedUnbuiltOnScriptedStream() {
    const std::string leg = "hosting stream";
    std::mutex m;
    // Whether each renderer built was destroyed, in the order the reader built them.
    std::vector<std::shared_ptr<std::atomic<bool>>> gone;
    const auto goneAt = [&](size_t i) {
        std::lock_guard<std::mutex> l(m);
        return i < gone.size() && gone[i]->load();
    };
    BuiltRenderers built;
    ScriptedHost host([](const ParsedFrame& f) -> std::optional<std::string> {
        if (f.kind != wire::PlatformRequest) return std::nullopt;
        return FrameBytes(f.runtime, wire::PlatformAnswer).str("answer:" + FieldsOf(f).str().value_or("")).bytes();
    }, nullptr, built.factory([&](HostedTestRenderer& r) {
        std::lock_guard<std::mutex> l(m);
        r.gone = gone.emplace_back(std::make_shared<std::atomic<bool>>(false));
    }));
    const auto open = [](uint32_t runtime) { return FrameBytes(runtime, wire::Open).bytes(); };
    CHECK(host.send(hostingOpeningOf(*RES)));
    CHECK_MSG(eventually([&] { return host.events(0) == std::vector<std::string>{"ready:1"}; }), leg + ": " + joined(host.events(0)));
    // Runtime 1 opened and closed makes the reaper; B is runtime 2 and D runtime 3.
    CHECK(host.send(open(1) + FrameBytes(1, wire::Close).bytes() + open(2) + hostedStartFrame(2, "<p id='p'>b</p>") + open(3) + hostedStartFrame(3, "<p>d</p>")));
    CHECK_MSG(eventually([&] { return host.saw(2, "rendered") && host.saw(3, "rendered"); }, 20), leg + ": " + joined(host.events(2)) + " / " + joined(host.events(3)));
    CHECK_MSG(built.count() == 2, leg + ": " + joined(host.events(2)) + " / " + joined(host.events(3)));
    int rounds = 0;
    std::string heard;
    // B's probe sets its text on its probe thread; its renderer sends a platform message and asks a platform request the host answers.
    const auto bGoesOn = [&] {
        const std::string n = std::to_string(++rounds);
        const std::optional<std::string> probed = host.probe(2, "document.getElementById('p').textContent = 'b" + n + "'");
        const bool sent = host.send(FrameBytes(2, wire::Platform).str("say:s" + n).bytes() + FrameBytes(2, wire::Platform).str("wait:w" + n).bytes());
        const bool answered = eventually([&] { return host.saw(2, "platform:s" + n) && host.saw(2, "platform:waited:answer:w" + n); }, 2);
        heard = "probed \"" + probed.value_or("nothing") + "\", " + (answered ? "its platform message and request answered" : joined(host.eventsWith(2, "platform")));
        return probed == std::optional<std::string>("b" + n) && sent && answered;
    };
    CHECK_MSG(bGoesOn(), leg + ": B " + heard);
    std::map<std::string, int> before;
    CHECK_MSG(eventually([&] {
        before = threadNames().value_or(std::map<std::string, int>());
        return countOf(before, "widget.reaper") == 1 && countOf(before, "twidget-host") == 2 && countOf(before, "widget.validator") == 2 && countOf(before, "hosted.asker") == 2 &&
               countOf(before, "widget.sandbox") == 2 && countOf(before, "widget.probe") == 1;
    }), leg + ": " + namesText(before));
    {
        ThreadAllowance allowance;
        if (!filledFor(allowance, leg, before).empty()) return;
        struct Refused {
            int free;
            const char* thread;
        };
        // In the served host's order: its glue's thread, its validator, its renderer from the factory, its engine.
        const Refused refused[] = {{0, "twidget-host"}, {1, "widget.validator"}, {2, "hosted.asker"}, {3, "widget.sandbox"}};
        uint32_t runtime = 4;
        for (const Refused& r : refused) {
            const std::string what = leg + ", " + std::to_string(r.free) + " free";
            const std::string left = allowance.leave(r.free);
            CHECK_MSG(left.empty(), what + ": " + left);
            if (!left.empty()) return;
            const size_t renderers = built.count();
            printf("    %s: runtime %u's hosted start, whose %s cannot start\n", what.c_str(), runtime, r.thread);
            fflush(stdout);
            const auto sent = std::chrono::steady_clock::now();
            CHECK(host.send(open(runtime) + hostedStartFrame(runtime, "<p>a</p>")));
            std::vector<std::string> died;
            const bool dies = eventually([&] { return !(died = host.eventsWith(runtime, "died:")).empty(); }, 2);
            const double ms = msSince(sent);
            const int leftAt = allowance.full() - r.free;
            const std::optional<int> after = threadsSettleAt(leftAt);
            const std::map<std::string, int> names = threadNames().value_or(std::map<std::string, int>());
            const std::vector<std::string> events = host.events(runtime);
            printf("    %s: runtime %u heard %s in %.1f ms; the task back at %s threads (%d with %d free)\n", what.c_str(), runtime, whole(events).c_str(), ms, countText(after).c_str(), leftAt, r.free);
            fflush(stdout);
            CHECK_MSG(dies && died.size() == 1 && unstarted(died[0].substr(5), kNotBuilt, r.thread), what + ": " + whole(events));
            CHECK_MSG(after == leftAt && namesText(names) == namesText(before), what + ": " + countText(after) + " threads, not " + std::to_string(leftAt) + "; " + namesText(names));
            // Only the engine's refusal comes after the renderer was built, which went with the rest.
            const size_t made = r.free == 3 ? 1 : 0;
            CHECK_MSG(built.count() == renderers + made && (!made || goneAt(renderers)), what + ": " + std::to_string(built.count() - renderers) + " renderers built");
            const std::string filled = allowance.fill();
            CHECK_MSG(filled.empty(), what + ": " + filled);
            if (!filled.empty()) return;
            CHECK_MSG(bGoesOn(), what + ": B " + heard);
            runtime++;
        }

        printf("    %s: D's first probe, whose widget.probe cannot start\n", leg.c_str());
        fflush(stdout);
        const auto asked = std::chrono::steady_clock::now();
        const std::optional<std::string> probed = host.probe(3, "1 + 1");
        const double answered = msSince(asked);
        std::vector<std::string> stopped;
        const bool stops = eventually([&] { return !(stopped = host.eventsWith(3, "died:")).empty(); }, 2);
        const double ended = msSince(asked);
        printf("    %s: D's probe answered \"%s\" in %.1f ms; D heard %s within %.1f ms\n", leg.c_str(), probed.value_or("nothing").c_str(), answered, whole(stopped).c_str(), ended);
        fflush(stdout);
        CHECK_MSG(probed == std::optional<std::string>("ERROR no runtime") && answered < 2000, leg + ": D's probe " + probed.value_or("unanswered") + " in " + std::to_string(answered) + " ms");
        CHECK_MSG(stops && stopped.size() == 1 && unstarted(stopped[0].substr(5), kProbeUnstarted, "widget.probe"), leg + ": " + whole(stopped));
        // Stopped as `Stop` stops it: its renderer stays, its platform traffic still crossing, until its `Close`.
        CHECK(host.send(FrameBytes(3, wire::Platform).str("say:kept").bytes()));
        CHECK_MSG(eventually([&] { return host.saw(3, "platform:kept"); }, 2), leg + ": " + joined(host.events(3)));
        CHECK_MSG(!goneAt(1), leg + ": D's renderer went before its Close");
        CHECK_MSG(bGoesOn(), leg + ": B " + heard);
        CHECK(host.send(FrameBytes(3, wire::Close).bytes()));
        CHECK_MSG(eventually([&] { return goneAt(1); }, 5), leg + ": D's renderer is still in the engine's end after its Close");
        std::map<std::string, int> after;
        CHECK_MSG(eventually([&] {
            after = threadNames().value_or(std::map<std::string, int>());
            return countOf(after, "twidget-host") == 1 && countOf(after, "widget.validator") == 1 && countOf(after, "hosted.asker") == 1 && countOf(after, "widget.sandbox") == 1;
        }, 5), leg + ": " + namesText(after));
        CHECK_MSG(countOf(after, "widget.probe") == 1 && countOf(after, "widget.reaper") == 1 && host.eventsWith(3, "died:") == stopped, leg + ": " + namesText(after) + "; " + whole(host.events(3)));
        CHECK_MSG(bGoesOn(), leg + ": B " + heard);
        printf("    %s: D's renderer carried its platform message until its Close, then went; B %s; after them %s\n", leg.c_str(), heard.c_str(), namesText(after).c_str());
        fflush(stdout);
        const std::optional<std::string> served = host.served(0);
        CHECK_MSG(!served, leg + ": the engine's end stopped serving: " + served.value_or(""));
    }
}

/// The child of `remote_stream_hosted_threads_that_cannot_start_stop_only_their_widget`, alone in a fresh process: `twidget-tests --child remote_stream_hosted_widgets_at_the_thread_limit`.
CHILD_CASE(remote_stream_hosted_widgets_at_the_thread_limit) {
    legsLeaveNoThread([] { hostedUnbuiltOnScriptedStream(); });
}

/// Both real ends of a hosting stream while no thread can start: the engine's end's reaper (a hosted channel opened and closed), live A (runtime 2), A2 (runtime 3, dead of its own CPU limit, `keepsRenderer`) and B (runtime 4, its probe thread and its host's `widget.answer` made by a probe and a `wait:` request) with all their threads, and every worker are made before the fill. A's `keepRenderer`, with no thread free for its handle's `twidget-host`, returns null within 1 s and throws nothing: A dies "terminated", the engine's end reads one `Stop` and one `Close` of A's, A's renderer there goes and its route no longer counts. With A's threads in the engine's end gone and the allowance filled again, A2's keep returns null the same way with one `Close` and no `Stop`, its death unchanged. Neither handle's observers hear anything, B's platform request and probe are still answered after each, and the stream goes on.
static void keepUnstartedOverRealEnds() {
    const std::string leg = "both ends, a keep";
    // A's, A2's and B's renderers in the engine's end, built in that order: whether each was destroyed.
    const std::vector<std::shared_ptr<std::atomic<bool>>> gone = {std::make_shared<std::atomic<bool>>(false), std::make_shared<std::atomic<bool>>(false),
                                                                  std::make_shared<std::atomic<bool>>(false)};
    std::atomic<size_t> made{0};
    BuiltRenderers built;
    StreamPeers peers(kBuild, *RES, nullptr, built.factory([&](HostedTestRenderer& r) {
        if (const size_t i = made++; i < gone.size()) r.gone = gone[i];
    }));
    CountingStream& counted = *peers.counted;
    {
        auto sink = std::make_shared<StreamSink>();
        const std::shared_ptr<SandboxHostedChannel> channel = peers.end->hostedChannel();
        channel->attach(sink);
        channel->close();
    }
    CHECK_MSG(eventually([&] {
        const std::optional<std::map<std::string, int>> names = threadNames();
        return names && countOf(*names, "widget.reaper") == 1;
    }), leg + ": no reaper after runtime 1 closed");
    // One after the other, so A is runtime 2, A2 runtime 3 and B runtime 4, their renderers built in that order.
    HostedStreamHost a(*peers.end, "<p>a</p>", WidgetHostOptions(), 300, "INTL");
    CHECK(a.shown());
    HostedStreamHost a2(*peers.end, kDiesSoon, WidgetHostOptions(), 300, "INTL", true, [](WidgetHost& h) { h.keepsRenderer = true; });
    CHECK(a2.shown());
    HostedStreamHost b(*peers.end, "<p>b</p>", WidgetHostOptions(), 300, "INTL");
    CHECK(b.shown());
    CHECK(a2.host->waitUntil(died, 10));
    const std::string deathA2 = a2.host->diagnostics().dead.value_or("");
    CHECK_MSG(deathA2.rfind("CPU limit exceeded", 0) == 0, leg + ": " + deathA2);
    int rounds = 0;
    // B's renderer asks a platform request its host answers (on the glue's thread, no thread of the test's), and B answers a probe.
    const auto bAnswers = [&] {
        const std::string n = "b" + std::to_string(++rounds);
        b.host->sendPlatform("wait:" + n);
        const bool probed = b.host->probe("1 + 1") == "2";
        return eventually([&] { return has(b.messages(), "waited:answer:" + n); }, 2) && probed;
    };
    CHECK_MSG(bAnswers(), leg + ": B " + joined(b.messages()));
    CHECK_MSG(eventually([&] { return peers.events.open() == 3; }), leg + ": " + joined(peers.events.all()));
    std::map<std::string, int> before;
    CHECK_MSG(eventually([&] {
        before = threadNames().value_or(std::map<std::string, int>());
        return countOf(before, "widget.sandbox") == 2 && countOf(before, "widget.validator") == 2 && countOf(before, "hosted.asker") == 3 && countOf(before, "twidget-host") == 6 &&
               countOf(before, "widget.client") == 3 && countOf(before, "widget.probe") == 1 && countOf(before, "widget.answer") == 1 && countOf(before, "widget.reaper") == 1;
    }), leg + ": " + namesText(before));
    {
        ThreadAllowance allowance;
        if (!filledFor(allowance, leg, before).empty()) return;
        KeptObservers heardA, heardA2;
        printf("    %s: live A's keep, whose handle's twidget-host cannot start\n", leg.c_str());
        fflush(stdout);
        auto asked = std::chrono::steady_clock::now();
        std::unique_ptr<HostedKeptRenderer> kept = a.host->keepRenderer(heardA.observers());
        double ms = msSince(asked);
        const bool diedA = a.host->waitUntil(died, 2);
        const std::string deathA = a.host->diagnostics().dead.value_or("alive");
        const bool closedA = eventually([&] { return counted.readOf(wire::Close, 2) == 1; }, 5);
        const bool goneA = eventually([&] { return gone[0]->load(); }, 5);
        printf("    %s: A's keep returned %s in %.1f ms; A %s; the engine's end read %zu Stop and %zu Close of A's, its renderer %s\n", leg.c_str(), kept ? "a handle" : "null", ms,
               diedA ? ("died: " + deathA).c_str() : "lives", counted.readOf(wire::Stop, 2), counted.readOf(wire::Close, 2), goneA ? "gone" : "still there");
        fflush(stdout);
        CHECK_MSG(!kept && ms < 1000, leg + ": A's keep " + (kept ? "kept" : "returned null") + " in " + std::to_string(ms) + " ms");
        CHECK_MSG(diedA && deathA == "terminated", leg + ": A " + deathA);
        CHECK_MSG(closedA && counted.readOf(wire::Stop, 2) == 1, leg + ": " + std::to_string(counted.readOf(wire::Stop, 2)) + " stops, " + std::to_string(counted.readOf(wire::Close, 2)) + " closes");
        CHECK_MSG(goneA, leg + ": A's renderer is still in the engine's end");
        CHECK_MSG(eventually([&] { return peers.events.open() == 2; }), leg + ": " + joined(peers.events.all()));
        // A's threads in the engine's end went with it: once they are gone the allowance is filled again for A2's keep.
        std::map<std::string, int> freed;
        CHECK_MSG(eventually([&] {
            freed = threadNames().value_or(std::map<std::string, int>());
            return countOf(freed, "widget.sandbox") == 1 && countOf(freed, "widget.validator") == 1 && countOf(freed, "hosted.asker") == 2 && countOf(freed, "twidget-host") == 5;
        }, 5), leg + ": " + namesText(freed));
        CHECK_MSG(bAnswers(), leg + ": B " + joined(b.messages()));
        if (!filledFor(allowance, leg + ", A's keep settled", freed).empty()) return;

        printf("    %s: dead A2's keep, whose handle's twidget-host cannot start\n", leg.c_str());
        fflush(stdout);
        asked = std::chrono::steady_clock::now();
        kept = a2.host->keepRenderer(heardA2.observers());
        ms = msSince(asked);
        const bool closedA2 = eventually([&] { return counted.readOf(wire::Close, 3) == 1; }, 5);
        const bool goneA2 = eventually([&] { return gone[1]->load(); }, 5);
        printf("    %s: A2's keep returned %s in %.1f ms; A2 died: %s; the engine's end read %zu Stop and %zu Close of A2's, its renderer %s\n", leg.c_str(), kept ? "a handle" : "null", ms,
               a2.host->diagnostics().dead.value_or("alive").c_str(), counted.readOf(wire::Stop, 3), counted.readOf(wire::Close, 3), goneA2 ? "gone" : "still there");
        fflush(stdout);
        CHECK_MSG(!kept && ms < 1000, leg + ": A2's keep " + (kept ? "kept" : "returned null") + " in " + std::to_string(ms) + " ms");
        CHECK_MSG(closedA2 && counted.readOf(wire::Stop, 3) == 0, leg + ": " + std::to_string(counted.readOf(wire::Stop, 3)) + " stops, " + std::to_string(counted.readOf(wire::Close, 3)) + " closes");
        CHECK_MSG(goneA2, leg + ": A2's renderer is still in the engine's end");
        CHECK_MSG(a2.host->diagnostics().dead == std::optional<std::string>(deathA2), leg + ": A2 " + a2.host->diagnostics().dead.value_or("alive"));
        CHECK_MSG(eventually([&] { return peers.events.open() == 1; }), leg + ": " + joined(peers.events.all()));
        CHECK_MSG(heardA.count() == 0 && heardA2.count() == 0, leg + ": " + joined(heardA.events()) + " / " + joined(heardA2.events()));
        CHECK_MSG(bAnswers(), leg + ": B " + joined(b.messages()));
        CHECK_MSG(counted.readOf(wire::Stop, 4) == 0 && counted.readOf(wire::Close, 4) == 0 && !gone[2]->load() && !b.host->diagnostics().dead, leg + ": B was stopped");
        CHECK_MSG(!peers.events.ended() && !peers.serving.within(0), leg + ": " + joined(peers.events.all()));
        const std::map<std::string, int> after = threadNames().value_or(std::map<std::string, int>());
        printf("    %s: B answered after both keeps; the host's end heard %s; after them %s\n", leg.c_str(), joined(peers.events.all()).c_str(), namesText(after).c_str());
        fflush(stdout);
    }
}

/// The child of `remote_stream_hosted_keep_whose_thread_cannot_start_keeps_nothing`, alone in a fresh process: `twidget-tests --child remote_stream_hosted_keeps_at_the_thread_limit`.
CHILD_CASE(remote_stream_hosted_keeps_at_the_thread_limit) {
    legsLeaveNoThread([] { keepUnstartedOverRealEnds(); });
}
#endif

/// A hosted widget whose threads in the engine's end cannot start dies alone with the reason: a fresh child (`--child remote_stream_hosted_widgets_at_the_thread_limit`) uses up its thread allowance, every other thread made before, on a hosting stream. With 0, 1, 2 and 3 threads free a hosted start is refused at its served host's `twidget-host`, `widget.validator`, its renderer's own thread (inside the renderer factory) and `widget.sandbox`: its one death names that thread within 2 s (the engine's end's context, `JSThread <thread>`, EAGAIN), and nothing it built is left. A live widget's first probe, whose `widget.probe` cannot start, answers "ERROR no runtime" and stops it with the refusal, its renderer still carrying platform messages until its `Close`, then gone. Another widget's probe, platform message and platform request go through after each, and the engine's end goes on serving. The child exits 0 within 120 s, or it is killed and the test fails.
TEST(remote_stream_hosted_threads_that_cannot_start_stop_only_their_widget) {
#ifdef __APPLE__
    childPasses("remote_stream_hosted_widgets_at_the_thread_limit", 120);
#else
    printf("  not run on this platform: this test uses macOS's per-process thread limit\n");
#endif
}

/// A keep whose handle's thread cannot start keeps nothing: a fresh child (`--child remote_stream_hosted_keeps_at_the_thread_limit`) uses up its thread allowance, every other thread made before, with hosted WidgetHosts over both real ends. A live widget's `keepRenderer` returns null within 1 s and throws nothing, the widget dying "terminated" with one `Stop` and one `Close` and its renderer gone from the engine's end; then, with the allowance full again, a dead one's with `keepsRenderer` returns null with one `Close` and no `Stop`. Neither handle's observers hear anything, a third widget's platform request and probe are still answered, and the stream goes on. The child exits 0 within 60 s, or it is killed and the test fails.
TEST(remote_stream_hosted_keep_whose_thread_cannot_start_keeps_nothing) {
#ifdef __APPLE__
    childPasses("remote_stream_hosted_keeps_at_the_thread_limit", 60);
#else
    printf("  not run on this platform: this test uses macOS's per-process thread limit\n");
#endif
}
