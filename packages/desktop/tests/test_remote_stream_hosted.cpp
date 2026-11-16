// A hosting stream's tests (remote_stream.h's hosted kinds, `SandboxRemoteEngineEnd::serveHosting` and `SandboxRemoteHostEnd::startHosting`: the wire, the engine process's end and a hosted widget on it, the host's end with hosted WidgetHosts over real and lying ends, hostile frames, a flood, an unanswered request, renderers kept past their engines over real and lying ends, waits on the glue's thread, an engine process that reads no file, one that keeps only a kept renderer and a host process whose widgets are all hosted), in the suite after the byte stream's other tests: `twidget-tests remote_stream_hosted_`.
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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

/// The hosting stream's kinds are the documented table's: the eleven numbers, the host's below 64; the engine's end serving a hosting stream still reads 0, 22, 63, 88 and 255 as unknown and the engine's new kinds as the other side's; `HostingOpening`'s fields read back by hand in their order, and a hosted widget's validator is built from its fifth and sixth (a bundle of another stamp is refused at its build, bytecode that does not load falls back to the source).
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

    const std::string opening = hostingOpeningBytes(1, kBuild, "sandbox", std::string("\0\1", 2), "validator", "\2", "stamp");
    const std::optional<ParsedFrame> frame = frameAt(opening, uint32_t(wire::little(std::string_view(opening).substr(0, 4))));
    CHECK(frame && frame->runtime == 0 && frame->kind == 32 && wire::little(std::string_view(opening).substr(0, 4)) == opening.size() - 4);
    if (frame) {
        FieldsOf in(*frame);
        CHECK(in.i64() == 1 && in.str() == kBuild && in.str() == "sandbox" && in.str() == std::string("\0\1", 2) && in.str() == "validator" && in.str() == "\2" && in.str() == "stamp" && in.done());
    }

    const std::string other = BundleStamp{"9.9.9", 99, 1, 1}.line();
    const std::string refusal = BundleStamp::refusal(other, "validator-bundle.js").value_or("");
    const std::string restamped = other + RES->validatorBundle.substr(RES->validatorBundle.find('\n'));
    CHECK(!refusal.empty() && !RES->validatorBytecode.empty());
    struct Validator {
        const char* name;
        std::string opening;
        /// The diagnostic its build gives, "" for none.
        std::string diag;
        /// How the widget's start ends.
        std::string outcome;
    };
    const std::vector<Validator> validators = {
        {"another stamp", hostingOpeningOf(*RES, restamped, ""), "diag:bundle-mismatch:" + refusal, "died:sandbox bundle failed to load"},
        {"bytecode that does not load", hostingOpeningOf(*RES, std::nullopt, "not bytecode"), "diag:bytecode:validator-bundle.js: falling back to source", "rendered"},
        {"the shipped validator", hostingOpeningOf(*RES), "", "rendered"},
    };
    for (const Validator& v : validators) {
        BuiltRenderers renderers;
        ScriptedHost host(nullptr, nullptr, renderers.factory());
        CHECK(host.send(v.opening + FrameBytes(1, wire::Open).bytes() + hostedStartFrame(1, "<p>checked</p>")));
        CHECK_MSG(eventually([&] { return (v.diag.empty() || !host.eventsWith(1, v.diag).empty()) && host.saw(1, v.outcome); }, 20), std::string(v.name) + ": " + joined(host.events(1)));
        const bool renders = v.outcome == "rendered", rendered = eventually([&] { return renderers.at(0) && renderers.at(0)->drew("checked"); }, renders ? 5 : 0.3);
        CHECK_MSG(rendered == renders, std::string(v.name) + (rendered ? ": rendered" : ": rendered nothing"));
        if (v.diag.empty()) CHECK_MSG(host.eventsWith(1, "diag:bytecode:").empty() && host.eventsWith(1, "diag:bundle-mismatch:").empty(), joined(host.events(1)));
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
    CHECK(opening.size() < Wire::kMaxOpeningBytes && opening.size() > RES->validatorBundle.size() + RES->validatorBytecode.size());
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
        {"another version", hostingOpeningBytes(2, kBuild, "s", "", "v", "", ""), "stream version 2, this end's is 1", true},
        {"another build", hostingOpeningBytes(1, "other", "s", "", "v", "", ""), "build \"other\", this end's is \"twidget-tests\"", true},
        {"another kind first", FrameBytes(1, wire::Open).bytes(), "kind 2 first, not the opening", true},
        {"the opening on a runtime", FrameBytes(3, wire::HostingOpening).i64(1).str(kBuild).str("s").str("").str("v").str("").str("").bytes(), "kind 32 on runtime 3, not the connection", true},
        {"a trailing byte", FrameBytes(0, wire::HostingOpening).i64(1).str(kBuild).str("s").str("").str("v").str("").str("").raw("x").bytes(), "kind 32 with bytes after its fields", true},
        {"a missing field", FrameBytes(0, wire::HostingOpening).i64(1).str(kBuild).str("s").str("").str("v").str("").bytes(), "kind 32 with a field past the frame's end", true},
        {"a size past the opening's bound, no body", wire::u32(uint32_t(Wire::kMaxOpeningBytes + 1)), "a frame of 16777217 bytes, past the bound of 16777216", true},
        {"a renderer stamp past its bound", hostingOpeningBytes(1, kBuild, "s", "", "v", "", longStamp), "a renderer stamp of 4097 bytes, past the bound of 4096", true},
        {"a renderer stamp of two lines", hostingOpeningBytes(1, kBuild, "s", "", "v", "", "r\nr"), "a renderer stamp of more than one line", true},
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
    for (const std::string& accepted : {hostingOpeningBytes(1, kBuild, "s", "", std::string(Wire::kMaxStampBytes + 1, 'v'), "", std::string(Wire::kMaxStampBytes, 'r')), smallHostingOpening()}) {
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
    CHECK(host.send(openingOf(*RES, true) + open1 + FrameBytes(1, wire::Start).str(kQueriesB).str(config).bytes()));
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

/// The engine's end serves a hosted widget whole: from `Open` and `HostedStart` (the safe area and data kept before it) its engine boots from the opening's bytecode, its renderer (built from the opening's resources, the scheme and the width) announces itself, and the host hears `RendererReady`, then `Loaded`, then `Rendered`, its node counts and the batches it asked for, while every op, layout query, listener, preventDefault, animation and frame request stays with the engine end's renderer (control: the same widget as an engine on the same end sends them as frames). Platform messages cross both ways in order; the renderer's request is a `PlatformRequest` answered by a hand-written `PlatformAnswer`, a second one from another thread waits in the renderer (never a second frame), and the engine's Intl request and a platform request wait at once, each for its own answer. A second runtime that did not ask gets no batch, takes its theme, data and visibility and freezes with its label at a stop; `Close` makes a waiting request return nullopt and sends nothing more for its runtime. A platform message or request past its bound goes out cut to it and one byte more.
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
    CHECK_MSG(boots.size() == 1 && boots[0].size() > 10 && boots[0].compare(boots[0].size() - 10, 10, "(bytecode)") == 0, joined(boots));
    CHECK_MSG(host.eventsWith(1, "diag:bytecode:").empty() && host.eventsWith(1, "diag:protocol-violation:").empty(), joined(events));
    const std::shared_ptr<SandboxResources> res = built.resources();
    CHECK(res && res->sandboxBundle == RES->sandboxBundle && res->sandboxBytecode == RES->sandboxBytecode && res->validatorBundle == RES->validatorBundle &&
          res->validatorBytecode == RES->validatorBytecode && res->rendererBundle == firstLine(RES->rendererBundle));
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
        CHECK(engine.send(openingOf(*RES, true) + FrameBytes(1, wire::Open).bytes() + FrameBytes(1, wire::Start).str(kHostedWidget).str(SandboxRemoteCoding::encodeConfig(animatedConfig())).bytes()));
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

static bool died(const WidgetDiagnostics& d) { return d.dead.has_value(); }

/// The details a WidgetHost recorded for diagnostic `kind`.
static std::vector<std::string> diagsOf(const WidgetDiagnostics& d, const std::string& kind) {
    const auto it = d.diags.find(kind);
    return it == d.diags.end() ? std::vector<std::string>() : it->second.second;
}

/// How a hosted widget dies when its host's platform let its renderer's request go unanswered (WidgetHost's `kPlatformAnswerSeconds`).
static const std::string kPlatformLate = "the host's platform did not answer the widget's renderer within 3 s";

/// A hosted WidgetHost on `end`'s hosting stream (its `hostedChannel`), `width` px wide (what the engine end's renderer factory gets), started after `configure` ran on it: its Intl provider answers `intlAnswer`, its platform requests are answered "answer:<message>" (with `answering` false each responder is kept and never called); what reached its callbacks, in order under a lock: each validated batch (asked for before its start), platform message and platform request, each request and wallet call with its responder.
struct HostedStreamHost {
    using Respond = std::function<void(const WidgetRequestOutcome&)>;
    const std::shared_ptr<StreamIntl> intl = std::make_shared<StreamIntl>();
    std::unique_ptr<WidgetHost> host;

    HostedStreamHost(SandboxRemoteHostEnd& end, const char* html, WidgetHostOptions options, double width, std::string intlAnswer, bool answering = true,
                     const std::function<void(WidgetHost&)>& configure = nullptr) {
        intl->answer = std::move(intlAnswer);
        host = std::make_unique<WidgetHost>(html, WidgetTheme{}, width, std::move(options), end.hostedChannel(), intl);
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

/// A hosted WidgetHost over real ends (`startHosting`, `serveHosting` with a HostedTestRenderer): the hosting opening carries the sandbox and validator bundles with their bytecode and the renderer's stamp; the widget loads and renders in the engine end (its boot from the bytecode, its layout answer and the host's Intl, formatted here on `widget.client`, in the batches it asked for, its renderer ready and node count reported), a probe sees its state, a tap after `pressDelivered` gets the host's request callback and then the wallet's, platform messages cross both ways in order and the renderer's request gets the host's answer, and a theme reaches the widget. The counted stream carried the hosting kinds and none of `Start`, `Event`, `Activation`, `AnimationFrame`, `Viewport`, `Resize`, `Theme`, `PressBegan`, `PressEnded`, `ChargeValidator`, `QueryAnswer`, `Ops`, `Query`, `Listen`, `PreventDefault`, `Anim` or `RequestsFrame` (control: an engine-mode WidgetHost through the same counting sends `Start`, `Ops` and `Query` and gets `QueryAnswer`). Each end refuses the other mode's channel: a hosting end's `channel()` and an engine end's `hostedChannel()` kill their client with why, as `refusingHosted` does.
TEST(remote_stream_hosted_widget_host) {
    WidgetHostOptions privileged;
    privileged.context = messageContext(false, true, false, "private");
    privileged.context.wallet = true;
    const std::string bytecode(RES->sandboxBytecode.begin(), RES->sandboxBytecode.end()), checker(RES->validatorBytecode.begin(), RES->validatorBytecode.end());
    CHECK(!bytecode.empty() && !checker.empty());
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
        CHECK_MSG(bootedFrom(d, "(bytecode)"), joined(bootsOf(d)));
        CHECK_MSG(h.intl->processes() == std::vector<long>{processId()}, std::to_string(h.intl->processes().size()) + " requests");
#if defined(__APPLE__) || defined(__linux__)
        CHECK_MSG(h.intl->threads() == std::vector<std::string>{"widget.client"}, joined(h.intl->threads()));
#endif
        const std::vector<std::string> opening = hostingOpeningFields(peers.engineSide->firstFrame());
        CHECK_MSG((opening == std::vector<std::string>{"1", kBuild, RES->sandboxBundle, bytecode, RES->validatorBundle, checker, firstLine(RES->rendererBundle)}), std::to_string(opening.size()) + " fields");
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
    refusing->platform("x");
    CHECK(refusing->probe("1") == "ERROR no runtime");
    refusing->close();
    CHECK_MSG(sink->events() == std::vector<std::string>{"died:no hosting here"}, joined(sink->events()));
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
    add("the host's hosting opening", hostingOpeningBytes(1, kBuild, "s", "", "v", "", ""), "kind 32 from the other side", true);
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
    CHECK_MSG(eventually([&] { return engine.saw(1, "close"); }, 10), joined(engine.frames(1)));
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
    // Sampled while the reader admits, the backlog passes it by at most the one message the client counts while it checks it and gives back at once.
    const size_t tried = 1 + std::to_string(SandboxEngine::kRemoteQueuedCalls + 1).size();
    CHECK_MSG(maxCalls >= SandboxEngine::kRemoteQueuedCalls && maxCalls <= SandboxEngine::kRemoteQueuedCalls + 1 && maxBytes <= heldBytes + tried && heldBytes + tried < SandboxEngine::kRemoteQueuedBytes,
              std::to_string(maxBytes) + " bytes, " + std::to_string(maxCalls) + " calls");
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

#if defined(__APPLE__) || defined(__linux__)
/// `name` as the thread inventory lists it: Linux keeps a thread name's first 15 bytes.
static std::string listed(const std::string& name) {
#ifdef __linux__
    return name.substr(0, 15);
#else
    return name;
#endif
}
#endif

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
            // The engine end's cut follows the keep's stop: ask until the report comes after it.
            CHECK_MSG(eventually([&] {
                          kept = threadsThrough([&](std::string m) { handle->sendPlatform(std::move(m)); }, [&] { return heard.messages(); });
                          return !kept.empty() && countIn(kept, "widget.sandbox") == 0;
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
