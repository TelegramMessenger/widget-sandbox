// The framed byte transport's tests (remote_stream.h: the wire, the theme's coding, the streams the tests use, the engines' thread names, both ends and WidgetHost over them, the engine process's lockdown, hostile bytes, stuck and blocked work and teardown, a host process whose engines are all remote, threads the OS refuses to start), in the suite after the remote engine's: `twidget-tests remote_stream_`.
#include "test_support.h"
#include "test_remote_support.h"
#include "test_remote_stream_support.h"
#include "test_thread_limit.h"

#include "../harness/stream_peers.h"
#include "twidget/bundle_stamp.h"
#include "twidget/intl.h"
#include "twidget/json.h"
#include "twidget/remote_engine.h"
#include "twidget/remote_stream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// ───────────── the wire ─────────────

/// The header's kinds and bounds are the documented wire's: every number of the table, host to engine below 64, the frame, opening and stamp bounds and the version.
TEST(remote_stream_wire_follows_the_documented_table) {
    using K = SandboxRemoteWire::Kind;
    const std::vector<std::pair<K, uint8_t>> kinds = {
        {K::Opening, wire::Opening}, {K::Open, wire::Open}, {K::Start, wire::Start}, {K::Event, wire::Event}, {K::Activation, wire::Activation},
        {K::AnimationFrame, wire::AnimationFrame}, {K::Viewport, wire::Viewport}, {K::Resize, wire::Resize}, {K::Theme, wire::Theme},
        {K::SafeArea, wire::SafeArea}, {K::Data, wire::Data}, {K::Visible, wire::Visible}, {K::PressBegan, wire::PressBegan},
        {K::PressEnded, wire::PressEnded}, {K::ChargeValidator, wire::ChargeValidator}, {K::Stop, wire::Stop}, {K::Answer, wire::Answer},
        {K::Probe, wire::Probe}, {K::QueryAnswer, wire::QueryAnswer}, {K::IntlAnswer, wire::IntlAnswer}, {K::Close, wire::Close},
        {K::Ready, wire::Ready}, {K::Refused, wire::Refused}, {K::TaskStarted, wire::TaskStarted}, {K::TaskCompleted, wire::TaskCompleted},
        {K::Ops, wire::Ops}, {K::Query, wire::Query}, {K::Intl, wire::Intl}, {K::Listen, wire::Listen}, {K::PreventDefault, wire::PreventDefault},
        {K::Anim, wire::Anim}, {K::RequestsFrame, wire::RequestsFrame}, {K::Log, wire::Log}, {K::Diag, wire::Diag}, {K::Bridge, wire::Bridge},
        {K::CopyText, wire::CopyText}, {K::Request, wire::Request}, {K::Wallet, wire::Wallet}, {K::Storage, wire::Storage},
        {K::ResizeAnchor, wire::ResizeAnchor}, {K::WatchesPublicData, wire::WatchesPublicData}, {K::Loaded, wire::Loaded}, {K::Stats, wire::Stats},
        {K::Died, wire::Died}, {K::ProbeAnswer, wire::ProbeAnswer},
    };
    CHECK(kinds.size() == 45);
    for (const auto& [kind, number] : kinds) CHECK_MSG(uint8_t(kind) == number, "kind " + std::to_string(number) + " is " + std::to_string(uint8_t(kind)));
    CHECK(std::count_if(kinds.begin(), kinds.end(), [](const auto& k) { return k.second < 64; }) == 21);
    CHECK(SandboxRemoteWire::kVersion == 1);
    CHECK(SandboxRemoteWire::kMaxFrameBytes == (size_t(32) << 20) + (size_t(64) << 10) && SandboxRemoteWire::kMaxFrameBytes == SandboxEngine::kRemoteOpsLimit + 65536);
    CHECK(SandboxRemoteWire::kMaxOpeningBytes == size_t(16) << 20 && SandboxRemoteWire::kMaxStampBytes == 4096);
    CHECK(SandboxRemoteWire::kMaxFrameBytes <= UINT32_MAX && SandboxRemoteWire::kMaxOpeningBytes <= UINT32_MAX);
}

/// The test's own frames are the documented layout byte for byte (the size of what follows it, the runtime, the kind, each field little-endian) and read back whole from a run of frames, from pieces and over a stream.
TEST(remote_stream_frame_bytes_follow_the_documented_layout) {
    CHECK(FrameBytes(7, wire::Activation).str("{}").i64(-2).bytes() ==
          std::string("\x13\0\0\0" "\x07\0\0\0" "\x05" "\x02\0\0\0" "{}" "\xFE\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 23));
    CHECK(FrameBytes(1, wire::AnimationFrame).f64(1.5).bytes() == std::string("\x0D\0\0\0" "\x01\0\0\0" "\x06" "\0\0\0\0\0\0\xF8\x3F", 17));
    CHECK(FrameBytes(0x01020304, wire::Visible).boolean(1).bytes() == std::string("\x06\0\0\0" "\x04\x03\x02\x01" "\x0C" "\x01", 10));
    CHECK(FrameBytes(2, wire::Close).bytes() == std::string("\x05\0\0\0" "\x02\0\0\0" "\x15", 9) && FrameBytes(2, wire::Close).body().size() == 5);
    CHECK(wire::u32(UINT32_MAX) == "\xFF\xFF\xFF\xFF" && FrameBytes(0, wire::Ready).raw("x").body() == std::string("\0\0\0\0" "\x40" "x", 6));

    const std::string ready = FrameBytes(0, wire::Ready).i64(1).bytes();
    const std::string log = FrameBytes(9, wire::Log).str("warn").str(std::string("a\0b", 3)).bytes();
    const std::string viewport = FrameBytes(9, wire::Viewport).f64(-0.0).f64(1e300).f64(2.5).bytes();
    std::string rest;
    const std::vector<ParsedFrame> frames = parseFrames(ready + log + viewport + log.substr(0, 6), &rest);
    CHECK_MSG(frames.size() == 3 && rest == log.substr(0, 6), std::to_string(frames.size()) + " frames, " + std::to_string(rest.size()) + " bytes left");
    if (frames.size() == 3) {
        FieldsOf first(frames[0]);
        CHECK(frames[0].runtime == 0 && frames[0].kind == wire::Ready && first.i64() == 1 && first.done());
        FieldsOf second(frames[1]);
        CHECK(frames[1].runtime == 9 && frames[1].kind == wire::Log && second.str() == "warn" && second.str() == std::string("a\0b", 3) && second.done());
        FieldsOf third(frames[2]);
        std::optional<double> zero = third.f64();
        CHECK(zero && *zero == 0 && std::signbit(*zero) && third.f64() == 1e300 && third.f64() == 2.5 && third.done() && !third.f64() && !third.str());
    }
    CHECK(parseFrames(wire::u32(4) + std::string(4, '\0')).empty() && parseFrames(log.substr(0, log.size() - 1)).empty());

    const std::vector<std::string> pieces = fragments(ready + log, 3);
    std::string again;
    for (const std::string& piece : pieces) again += piece;
    CHECK(again == ready + log && pieces.size() == (ready.size() + log.size() + 2) / 3 && pieces.front().size() == 3);

    // Each piece a write of its own through a pair that holds fewer bytes than a frame, then the same frames in one write: the reader goes by the sizes.
    for (const bool inPieces : {true, false}) {
        MemoryStreamPair pair = memoryStreamPair(5);
        Background<bool> writer([&] {
            for (const std::string& piece : inPieces ? pieces : std::vector<std::string>{ready + log})
                if (!pair.engine->write(piece)) return false;
            pair.engine->close();
            return true;
        });
        FrameStreamReader reader(*pair.host);
        std::optional<ParsedFrame> a = reader.next(), b = reader.next(), end = reader.next();
        CHECK(a && a->kind == wire::Ready && b && b->kind == wire::Log && b->fields == log.substr(9) && !end && reader.pending().empty());
        CHECK(writer.within(5) == std::optional<bool>(true));
    }
}

// ───────────── the theme's coding ─────────────

/// The theme's palette crosses as one object of strings, as `encodeConfig` writes it: every byte of its keys and values back, the last value of a repeated key, nothing for another shape, a cut text or one past the message limit.
TEST(remote_stream_theme_params_coding_round_trips_and_refuses_damage) {
    using Coding = SandboxRemoteCoding;
    using Palette = std::map<std::string, std::string>;
    const std::vector<Palette> palettes = {
        {},
        {{"bg_color", "#ffffff"}, {"text_color", "#000000"}, {"button_color", "#3390ec"}},
        {{std::string("k\0y", 3), std::string("v\0\x01", 3)}, {"lone", "\xED\xA0\x80"}, {"bytes", "\xFF\xC3"}, {"quote\"\\", "line\nbreak"}, {"", ""}},
    };
    for (const Palette& palette : palettes) {
        const std::string wire = Coding::encodeThemeParams(palette);
        CHECK_MSG(Coding::decodeThemeParams(wire) == palette, wire);
        SandboxConfig config;
        config.themeParams = palette;
        std::optional<Json> coded = Json::parse(Coding::encodeConfig(config), Json::Surrogates::Keep);
        CHECK_MSG(coded && (*coded)["themeParams"].dump() == wire, wire);
        int prefixes = 0;
        for (size_t length = 0; length < wire.size(); length++) prefixes += Coding::decodeThemeParams(std::string_view(wire).substr(0, length)).has_value();
        CHECK_MSG(prefixes == 0, wire);
    }
    CHECK(Coding::encodeThemeParams({{"bg_color", "#ffffff"}}) == "{\"bg_color\":\"#ffffff\"}" && Coding::encodeThemeParams({}) == "{}");
    CHECK(Coding::decodeThemeParams("{\"a\":\"1\",\"a\":\"2\"}") == (Palette{{"a", "2"}}));
    CHECK(Coding::decodeThemeParams("{\"a\":1,\"a\":\"2\"}") == (Palette{{"a", "2"}}) && !Coding::decodeThemeParams("{\"a\":\"1\",\"a\":2}"));
    CHECK(Coding::decodeThemeParams(" {\"a\":\"\\ud800\"} ") == (Palette{{"a", "\xED\xA0\x80"}}));
    for (const char* bad : {"[]", "null", "\"bg_color\"", "1", "", "{\"bg_color\":1}", "{\"bg_color\":null}", "{\"bg_color\":[\"#fff\"]}",
                            "{\"bg_color\":{\"x\":\"y\"}}", "{\"bg_color\":\"#fff\"", "{\"bg_color\":\"#fff\"}x", "{\"bg_color\":\"\x01\"}"})
        CHECK_MSG(!Coding::decodeThemeParams(bad), bad);
    std::string padded = "{\"bg_color\":\"#ffffff\"}";
    padded.resize(SandboxEngine::kRemoteMessageLimit, ' ');
    const bool atLimit = Coding::decodeThemeParams(padded).has_value();
    padded.push_back(' ');
    CHECK(atLimit && !Coding::decodeThemeParams(padded));
}

// ───────────── the tests' streams ─────────────

/// The tests' in-memory pair is a stream: a write past its queue goes on in parts as the other end reads, a full write and an empty read wait until a shutdown of either end wakes them, nothing works after it, and a closed end leaves what it wrote to be read before the end.
TEST(remote_stream_memory_pair_bounds_its_queues_and_wakes_on_shutdown) {
    {
        MemoryStreamPair pair = memoryStreamPair(7);
        std::string sent;
        for (int i = 0; i < 1000; i++) sent.push_back(char('a' + i % 26));
        Background<bool> writer([&] { return pair.host->write(sent); });
        CHECK(eventually([&] { return pair.host->writeWaiting(); }));
        std::string got;
        size_t largest = 0;
        char buffer[64];
        while (got.size() < sent.size()) {
            const size_t n = pair.engine->read(buffer, sizeof(buffer));
            if (!n) break;
            largest = std::max(largest, n);
            got.append(buffer, n);
        }
        CHECK_MSG(got == sent && largest <= 7, std::to_string(got.size()) + " bytes, " + std::to_string(largest) + " at most in a read");
        CHECK(writer.within(5) == std::optional<bool>(true));
    }
    {
        MemoryStreamPair pair = memoryStreamPair(4);
        Background<bool> writer([&] { return pair.host->write("0123456789"); });
        Background<size_t> reader([&] {
            char b[1];
            return pair.host->read(b, sizeof(b));
        });
        CHECK(eventually([&] { return pair.host->writeWaiting() && pair.host->readWaiting(); }));
        pair.engine->shutdown();
        CHECK(writer.within(2) == std::optional<bool>(false));
        CHECK(reader.within(2) == std::optional<size_t>(0));
        char b[8];
        CHECK(pair.engine->read(b, sizeof(b)) == 0 && pair.host->read(b, sizeof(b)) == 0);
        CHECK(!pair.engine->write("x") && !pair.host->write("x") && !pair.host->write(""));
        pair.host->shutdown();
        CHECK(!pair.host->writeWaiting() && !pair.host->readWaiting());
    }
    {
        MemoryStreamPair pair = memoryStreamPair(16);
        CHECK(pair.host->write("abc"));
        pair.host->close();
        char b[8];
        CHECK(pair.engine->read(b, sizeof(b)) == 3 && std::string(b, 3) == "abc" && pair.engine->read(b, sizeof(b)) == 0);
        CHECK(!pair.engine->write("x") && !pair.host->write("x") && pair.host->read(b, sizeof(b)) == 0);
        MemoryStreamPair other = memoryStreamPair(16);
        Background<size_t> waiting([&] {
            char c[1];
            return other.engine->read(c, sizeof(c));
        });
        CHECK(eventually([&] { return other.engine->readWaiting(); }));
        other.host.reset();
        CHECK(waiting.within(2) == std::optional<size_t>(0));
    }
}

// ───────────── thread names ─────────────

#if defined(__APPLE__) || defined(__linux__)
/// The name of `thread`, read on it.
static std::string nameOf(JSThread& thread) {
    return thread.sync([] {
        char name[64] = {};
        pthread_getname_np(pthread_self(), name, sizeof(name));
        return std::string(name);
    });
}

/// A client-mode engine's dispatch thread is `widget.client`, also one that died at once without a channel; an engine with its own runtime keeps `widget.sandbox`.
TEST(remote_stream_client_engines_dispatch_on_widget_client) {
    SandboxEngine local("<p>x</p>", testConfig(), RES);
    SandboxEngine client("<p>x</p>", testConfig(), std::make_shared<FakeRemoteChannel>());
    SandboxEngine none("<p>x</p>", testConfig(), std::shared_ptr<SandboxRemoteChannel>());
    CHECK(!local.isRemote() && client.isRemote() && none.isRemote());
    const std::string names = nameOf(local.thread()) + " " + nameOf(client.thread()) + " " + nameOf(none.thread());
    CHECK_MSG(names == "widget.sandbox widget.client widget.client", names);
}
#endif

// ───────────── the engine process's end ─────────────

/// The engine's end checks the opening before it serves: another version or build, another kind first, a damaged or bodiless oversized first frame and a stamp past its bound or of more than one line are refused with the reason it returns; a stream that ends on a frame's boundary is clean, one cut inside a frame is not; after its Ready the frame bound replaces the opening's, a frame it cannot accept ends serving with its reason, and a closed runtime's well-formed frames are dropped.
TEST(remote_stream_engine_opening) {
    using Wire = SandboxRemoteWire;
    const std::string stamp = firstLine(RES->validatorBundle);
    CHECK(BundleStamp::parse(stamp) && stamp.find('\n') == std::string::npos && RES->validatorBundle.size() > stamp.size());
    const auto opening = [](int64_t version, std::string_view build, std::string_view validator, std::string_view renderer) { return openingBytes(version, build, "s", "", validator, renderer); };
    struct Vector {
        const char* name;
        std::string bytes;
        std::string reason;
    };

    const std::string longBuild(100000, 'b');
    const std::vector<Vector> refused = {
        {"another version", opening(2, kBuild, stamp, ""), "stream version 2, this end's is 1"},
        {"another build", opening(1, "other", stamp, ""), "build \"other\", this end's is \"twidget-tests\""},
        {"a long build", opening(1, longBuild, stamp, ""), "build \"" + std::string(64, 'b') + "...\", this end's is \"twidget-tests\""},
        {"another kind first", FrameBytes(1, wire::Open).bytes(), "kind 2 first, not the opening"},
        {"the engine's kind first", FrameBytes(0, wire::Ready).i64(1).bytes(), "kind 64 from the other side"},
        {"the opening on a runtime", FrameBytes(3, wire::Opening).i64(1).str(kBuild).str("s").str("").str(stamp).str("").bytes(), "kind 1 on runtime 3, not the connection"},
        {"a trailing byte", FrameBytes(0, wire::Opening).i64(1).str(kBuild).str("s").str("").str(stamp).str("").raw("x").bytes(), "kind 1 with bytes after its fields"},
        {"a missing field", FrameBytes(0, wire::Opening).i64(1).str(kBuild).str("s").str("").str(stamp).bytes(), "kind 1 with a field past the frame's end"},
        {"a size past the opening's bound, no body", wire::u32(uint32_t(Wire::kMaxOpeningBytes + 1)), "a frame of 16777217 bytes, past the bound of 16777216"},
        {"a frame's bound for the opening, no body", wire::u32(uint32_t(Wire::kMaxFrameBytes)), "a frame of 33619968 bytes, past the bound of 16777216"},
        {"the largest size, no body", wire::u32(UINT32_MAX), "a frame of 4294967295 bytes, past the bound of 16777216"},
        {"a size under 5", wire::u32(4), "a frame of 4 bytes, less than its runtime and kind"},
        {"a validator stamp past its bound", opening(1, kBuild, std::string(Wire::kMaxStampBytes + 1, 'v'), ""), "a validator stamp of 4097 bytes, past the bound of 4096"},
        {"a renderer stamp past its bound", opening(1, kBuild, stamp, std::string(Wire::kMaxStampBytes + 1, 'r')), "a renderer stamp of 4097 bytes, past the bound of 4096"},
        {"a validator stamp of two lines", opening(1, kBuild, stamp + "\n" + stamp, ""), "a validator stamp of more than one line"},
        {"a renderer stamp of two lines", opening(1, kBuild, stamp, stamp + "\n"), "a renderer stamp of more than one line"},
    };
    for (const Vector& v : refused) {
        ScriptedHost host;
        CHECK(host.send(v.bytes));
        const std::optional<std::string> reason = host.served(2);
        CHECK_MSG(reason == v.reason, std::string(v.name) + ": " + reason.value_or("still serving after 2 s"));
        CHECK_MSG(eventually([&] { return host.events(0) == std::vector<std::string>{"refused:" + v.reason}; }, 2), std::string(v.name) + ": " + joined(host.events(0)));
    }

    // Stamps at their bound, an empty one and no renderer pass the opening; the runtimes' boot checks what they say.
    for (const std::string& accepted : {opening(1, kBuild, std::string(Wire::kMaxStampBytes, 'v'), std::string(Wire::kMaxStampBytes, 'r')), opening(1, kBuild, "", ""), smallOpening()}) {
        ScriptedHost host;
        CHECK(host.send(accepted));
        CHECK_MSG(eventually([&] { return host.events(0) == std::vector<std::string>{"ready:1"}; }, 2), joined(host.events(0)));
        host.close();
        CHECK(host.served(2) == std::string());
    }

    const std::vector<Vector> ended = {
        {"nothing", "", ""},
        {"the end inside a size", wire::u32(9).substr(0, 2), "the stream ended inside a frame's size"},
        {"the end inside the opening", smallOpening().substr(0, 30), "the stream ended inside a frame"},
    };
    for (const Vector& v : ended) {
        ScriptedHost host;
        CHECK(host.send(v.bytes));
        host.close();
        const std::optional<std::string> reason = host.served(2);
        CHECK_MSG(reason == v.reason, std::string(v.name) + ": " + reason.value_or("still serving after 2 s"));
    }

    const std::string open1 = FrameBytes(1, wire::Open).bytes(), open2 = FrameBytes(2, wire::Open).bytes(), event1 = FrameBytes(1, wire::Event).str("{}").bytes();
    const std::string dropped = open1 + FrameBytes(1, wire::Close).bytes() + event1 + FrameBytes(1, wire::QueryAnswer).str("{}").boolean(1).str("b").bytes() +
                                FrameBytes(1, wire::IntlAnswer).str("late").bytes() + FrameBytes(1, wire::Probe).i64(1).str("1").bytes() + FrameBytes(1, wire::Start).str("<p>x</p>").str("{}").bytes();
    struct After {
        const char* name;
        std::string bytes;
        bool end;
        std::string reason;
    };
    const std::vector<After> after = {
        {"the end on a frame's boundary", "", true, ""},
        {"the end inside a frame", event1.substr(0, 9), true, "the stream ended inside a frame"},
        {"the end inside a size", event1.substr(0, 3), true, "the stream ended inside a frame's size"},
        {"a size past the opening's bound, within a frame's", wire::u32(uint32_t(Wire::kMaxOpeningBytes + 1)), true, "the stream ended inside a frame"},
        {"a size past a frame's bound, no body", wire::u32(uint32_t(Wire::kMaxFrameBytes + 1)), false, "a frame of 33619969 bytes, past the bound of 33619968"},
        {"a second opening", smallOpening(), false, "kind 1 after the opening"},
        {"a runtime never opened", FrameBytes(5, wire::Event).str("{}").bytes(), false, "kind 4 on runtime 5, which was never opened"},
        {"a runtime opened twice", open1 + open1, false, "kind 2 on runtime 1, not above the last opened runtime 1"},
        {"runtimes opened out of order", open2 + open1, false, "kind 2 on runtime 1, not above the last opened runtime 2"},
        {"a kind of the engine's", open1 + FrameBytes(1, wire::Log).str("log").str("x").bytes(), false, "kind 75 from the other side"},
        {"binary bytes without binary", open1 + FrameBytes(1, wire::QueryAnswer).str("{}").boolean(0).str("x").bytes(), false, "kind 19 with binary bytes but no binary"},
        {"a bool of 2", open1 + FrameBytes(1, wire::Visible).boolean(2).bytes(), false, "kind 12 with a bool of 2"},
        {"a config that does not decode", open1 + FrameBytes(1, wire::Start).str("<p>x</p>").str("{}").bytes(), false, "kind 3 on runtime 1 with a form that does not decode"},
        {"a palette that does not decode", open1 + FrameBytes(1, wire::Theme).str("dark").str("{\"bg_color\":1}").bytes(), false, "kind 9 on runtime 1 with a form that does not decode"},
        {"a safe area that does not decode", open1 + FrameBytes(1, wire::SafeArea).str("[]").bytes(), false, "kind 10 on runtime 1 with a form that does not decode"},
        {"data that does not decode", open1 + FrameBytes(1, wire::Data).str("null").bytes(), false, "kind 11 on runtime 1 with a form that does not decode"},
        {"an outcome that does not decode", open1 + FrameBytes(1, wire::Answer).i64(1).str("{}").bytes(), false, "kind 17 on runtime 1 with a form that does not decode"},
        {"a closed runtime's frames", dropped, true, ""},
        {"a closed runtime's damaged frame", open1 + FrameBytes(1, wire::Close).bytes() + FrameBytes(1, wire::Visible).boolean(2).bytes(), false, "kind 12 with a bool of 2"},
    };
    for (const After& v : after) {
        ScriptedHost host;
        CHECK(host.send(smallOpening()));
        CHECK_MSG(eventually([&] { return host.events(0) == std::vector<std::string>{"ready:1"}; }, 2), std::string(v.name) + ": " + joined(host.events(0)));
        CHECK(host.send(v.bytes));
        if (v.end) host.close();
        const std::optional<std::string> reason = host.served(2);
        CHECK_MSG(reason == v.reason, std::string(v.name) + ": " + reason.value_or("still serving after 2 s"));
        CHECK_MSG(host.frameCount() == 1, std::string(v.name) + ": " + std::to_string(host.frameCount()) + " frames");
    }
}

/// The engine's end runs real runtimes from the opening's bytes alone: two ids on one stream, several frames in one read and one frame over several, inputs before the start kept in order (a safe area, data, a palette coded for the wire), each runtime's layout queries and Intl requests answered by the host for it alone (an answer with binary bytes, with empty ones, with none), probes by sequence, and one runtime closed while the other goes on; without bytecode it boots from the source, and a validator stamp of another release is refused at boot as in process.
TEST(remote_stream_engine_scripted_host) {
    SandboxConfig config = testConfig();
    config.data = WidgetData{std::string("chat1"), std::nullopt};
    const std::string coded = SandboxRemoteCoding::encodeConfig(config);
    const auto started = [&](uint32_t runtime, const char* html) { return FrameBytes(runtime, wire::Start).str(html).str(coded).bytes(); };
    const auto booted = [](ScriptedHost& host, uint32_t runtime, const std::string& how) {
        const std::vector<std::string> boots = host.eventsWith(runtime, "diag:boot:sandbox bundle eval ");
        return boots.size() == 1 && boots[0].size() > how.size() && boots[0].compare(boots[0].size() - how.size(), how.size(), how) == 0;
    };
    CHECK(!RES->sandboxBytecode.empty());
    {
        ScriptedHost host(layoutAnswers());
        CHECK(host.send(openingOf(*RES, true)));
        CHECK_MSG(eventually([&] { return host.events(0) == std::vector<std::string>{"ready:1"}; }), joined(host.events(0)));
        WatchedStream& engine = host.engineSide();
        CHECK(eventually([&] { return engine.bytesRead() == host.sent(); }));
        const size_t reads = engine.reads();
        // Four frames in one write come in one read, then one frame in four writes, each read before the next is written: the reader goes by the sizes.
        const std::string batch = FrameBytes(1, wire::Open).bytes() + FrameBytes(2, wire::Open).bytes() +
                                  FrameBytes(1, wire::SafeArea).str(SandboxRemoteCoding::encodeSafeArea(WidgetContentSafeArea{0, 0, 24, 0, 0, 64})).bytes() +
                                  FrameBytes(2, wire::Data).str(SandboxRemoteCoding::encodeData(WidgetData{std::string("chat2"), std::nullopt})).bytes();
        CHECK(host.send(batch));
        CHECK(eventually([&] { return engine.bytesRead() == host.sent(); }));
        CHECK_MSG(engine.reads() == reads + 1, std::to_string(engine.reads() - reads) + " reads");
        const std::string theme = FrameBytes(1, wire::Theme).str("dark").str("{\"bg_color\":\"#112233\"}").bytes();
        const std::vector<std::string> pieces = {theme.substr(0, 2), theme.substr(2, 5), theme.substr(7, 6), theme.substr(13)};
        for (const std::string& piece : pieces) {
            CHECK(host.send(piece));
            CHECK(eventually([&] { return engine.bytesRead() == host.sent(); }));
        }
        CHECK_MSG(engine.reads() == reads + 1 + pieces.size(), std::to_string(engine.reads() - reads) + " reads");
        CHECK(host.send(started(1, kQueriesA) + started(2, kQueriesB)));

        CHECK_MSG(eventually([&] { return host.saw(1, "log:A 24 77,78,79 INTL-A"); }, 20), joined(host.events(1)));
        CHECK_MSG(eventually([&] { return host.saw(2, "log:B 33 INTL-B"); }, 20), joined(host.events(2)));
        for (const uint32_t runtime : {1u, 2u}) CHECK_MSG(booted(host, runtime, "(bytecode)") && host.eventsWith(runtime, "intl:").size() == 1, joined(host.events(runtime)));
        CHECK_MSG(host.eventsWith(1, "query:{\"q\":\"rect\"").size() == 3 && host.eventsWith(2, "query:{\"q\":\"rect\"").size() == 1, joined(host.events(1)) + " / " + joined(host.events(2)));
        CHECK(eventually([&] { return host.probe(1, "TelegramWidget.colorScheme + ' ' + TelegramWidget.themeParams.bg_color") == "dark #112233"; }));
        CHECK(eventually([&] { return host.probe(2, "TelegramWidget.chatData") == "chat2"; }));
        CHECK(host.probe(1, "w.join(',')") == "77,78,79" && host.probe(2, "typeof w") == "undefined");

        // Runtime 1 closes: what still comes for it is dropped, its late probe goes unanswered, its engine's death never goes out, and runtime 2 goes on.
        CHECK(host.send(FrameBytes(1, wire::Close).bytes() + FrameBytes(1, wire::Event).str(kTapOnButton).bytes() + FrameBytes(1, wire::Probe).i64(99).str("1").bytes() +
                        FrameBytes(1, wire::IntlAnswer).str("late").bytes()));
        CHECK(host.send(FrameBytes(2, wire::Event).str(kTapOnButton).bytes()));
        CHECK_MSG(eventually([&] { return host.saw(2, "log:B again 34"); }), joined(host.events(2)));
        CHECK(host.probe(2, "'alive'") == "alive");
        CHECK_MSG(host.eventsWith(1, "probe:99:").empty() && host.eventsWith(1, "died:").empty(), joined(host.events(1)));
        CHECK(host.eventsWith(2, "query:{\"q\":\"rect\"").size() == 2 && host.eventsWith(2, "died:").empty());
        host.close();
        CHECK(host.served(5) == std::string());
    }
    {
        ScriptedHost host(layoutAnswers());
        CHECK(host.send(openingBytes(1, kBuild, RES->sandboxBundle, "", firstLine(RES->validatorBundle), "") + FrameBytes(1, wire::Open).bytes() + started(1, kQueriesB)));
        CHECK_MSG(eventually([&] { return host.saw(1, "log:B 77 INTL-A"); }, 20), joined(host.events(1)));
        CHECK_MSG(booted(host, 1, "(source)"), joined(host.events(1)));
        host.close();
        CHECK(host.served(5) == std::string());
    }
    {
        const std::string other = BundleStamp{"9.9.9", 99, 1, 1}.line();
        const std::string refusal = BundleStamp::refusal(other, "validator-bundle.js").value_or("");
        ScriptedHost host;
        CHECK(host.send(openingBytes(1, kBuild, RES->sandboxBundle, "", other, firstLine(RES->rendererBundle)) + FrameBytes(1, wire::Open).bytes() + started(1, "<p>x</p>")));
        CHECK_MSG(eventually([&] { return host.saw(1, "died:sandbox bundle failed to load"); }, 20), joined(host.events(1)));
        CHECK_MSG(!refusal.empty() && host.saw(1, "diag:bundle-mismatch:" + refusal) && host.eventsWith(1, "diag:boot:").empty(), joined(host.events(1)));
        host.close();
        CHECK(host.served(5) == std::string());
    }
}

// ───────────── the host's end ─────────────

/// `end` stopped from a worker of the test's within `seconds` (a hang ends the run).
static bool stopsWithin(SandboxRemoteHostEnd& end, double seconds) {
    Background<bool> stopping([&] {
        end.stop();
        return true;
    });
    return stopping.finish(seconds);
}

/// The host's end bounds each write, not each call: an opening the engine's side never reads, and later one frame past what the stream holds, end the stream once the write bound passed with no other call; writes that each complete within the bound cancel their deadlines, also when the whole frame takes longer, and the stream goes on; the frame being written counts against the queue's bounds of bytes and frames until it is written.
TEST(remote_stream_host_bounds_each_write) {
    SandboxRemoteHostEnd::Limits limits;
    limits.writeStallMs = 300;
    const std::string stalled = "a write to the stream blocked for more than 300 ms";
    {
        EndEvents events;
        MemoryStreamPair pair = memoryStreamPair(16);
        const auto began = std::chrono::steady_clock::now();
        std::shared_ptr<SandboxRemoteHostEnd> end = SandboxRemoteHostEnd::start(pair.host, smallResources(), kBuild, events.events(), limits);
        CHECK(eventually([&] { return pair.host->writeWaiting(); }, 2));
        CHECK_MSG(eventually([&] { return events.ended().has_value(); }, 2), joined(events.all()));
        const double ms = msSince(began);
        CHECK_MSG(events.all() == std::vector<std::string>{"ended:" + stalled}, joined(events.all()));
        CHECK_MSG(ms >= 300 && ms < 2000, std::to_string(ms) + " ms");
        CHECK(stopsWithin(*end, 2) && !pair.host->writeWaiting());
    }
    {
        EndEvents events;
        MemoryStreamPair pair = memoryStreamPair(size_t(64) << 10);
        std::shared_ptr<SandboxRemoteHostEnd> end = SandboxRemoteHostEnd::start(pair.host, smallResources(), kBuild, events.events(), limits);
        FrameStreamReader engineSide(*pair.engine);
        const std::optional<ParsedFrame> opening = engineSide.next();
        CHECK(opening && opening->kind == wire::Opening && pair.engine->write(FrameBytes(0, wire::Ready).i64(1).bytes()));
        auto sink = std::make_shared<StreamSink>();
        std::shared_ptr<SandboxRemoteChannel> channel = end->channel();
        channel->attach(sink);
        const std::optional<ParsedFrame> open = engineSide.next();
        CHECK(open && open->kind == wire::Open && open->runtime == 1);
        CHECK(eventually([&] { return events.open() == 1; }));
        // One call past what the stream holds and none after it: the second of its writes blocks, and the bound alone ends the stream.
        channel->event(std::string(size_t(1) << 20, ' '));
        CHECK(eventually([&] { return pair.host->writeWaiting(); }, 2));
        CHECK_MSG(eventually([&] { return events.ended().has_value(); }, 2), joined(events.all()));
        CHECK_MSG(events.all() == (std::vector<std::string>{"runtimes:1", "runtimes:0", "ended:" + stalled}), joined(events.all()));
        CHECK_MSG(sink->events() == std::vector<std::string>{"disconnected"}, joined(sink->events()));
        CHECK(stopsWithin(*end, 2) && !pair.host->writeWaiting());
    }
    {
        limits.writeStallMs = 1000;
        EndEvents events;
        MemoryStreamPair pair = memoryStreamPair(size_t(64) << 10);
        std::shared_ptr<SandboxRemoteHostEnd> end = SandboxRemoteHostEnd::start(pair.host, smallResources(), kBuild, events.events(), limits);
        FrameStreamReader engineSide(*pair.engine);
        const std::optional<ParsedFrame> opening = engineSide.next();
        CHECK(opening && opening->kind == wire::Opening && pair.engine->write(FrameBytes(0, wire::Ready).i64(1).bytes()));
        auto sink = std::make_shared<StreamSink>();
        std::shared_ptr<SandboxRemoteChannel> channel = end->channel();
        channel->attach(sink);
        const std::optional<ParsedFrame> open = engineSide.next();
        CHECK(open && open->kind == wire::Open);
        // Six writes of 64 KiB, each waiting for the engine's side to read every 300 ms: each completes within the bound, the frame takes far longer.
        const size_t frameBytes = size_t(6) << 16;
        channel->event(std::string(frameBytes - 13, ' '));
        CHECK(eventually([&] { return pair.host->writeWaiting(); }, 2));
        const auto began = std::chrono::steady_clock::now();
        std::string got;
        std::vector<char> buffer(size_t(64) << 10);
        while (got.size() < frameBytes) {
            sleepMs(300);
            const size_t n = pair.engine->read(buffer.data(), std::min(buffer.size(), frameBytes - got.size()));
            if (!n) break;
            got.append(buffer.data(), n);
        }
        const double ms = msSince(began);
        CHECK_MSG(got.size() == frameBytes && parseFrames(got).size() == 1 && ms > 1000, std::to_string(got.size()) + " bytes in " + std::to_string(ms) + " ms");
        CHECK_MSG(!events.ended(), joined(events.all()));
        // Past any deadline of the writes that completed, the stream still goes on.
        sleepMs(1500);
        CHECK_MSG(!events.ended(), joined(events.all()));
        channel->event("{\"after\":1}");
        const std::optional<ParsedFrame> after = engineSide.next();
        CHECK(after && describeHost(*after) == "event:{\"after\":1}" && sink->events().empty());
        channel.reset();
        CHECK(stopsWithin(*end, 2));
        CHECK_MSG(endsOnce(events.all(), "the host stopped the stream"), joined(events.all()));
    }
    for (const bool bytes : {true, false}) {
        // The frame being written stays counted: a blocked one and what is queued behind it reach the bounds of bytes and of frames.
        SandboxRemoteHostEnd::Limits bounded;
        bounded.queuedBytes = size_t(3) << 20;
        EndEvents events;
        MemoryStreamPair pair = memoryStreamPair(size_t(64) << 10);
        std::shared_ptr<SandboxRemoteHostEnd> end = SandboxRemoteHostEnd::start(pair.host, smallResources(), kBuild, events.events(), bounded);
        FrameStreamReader engineSide(*pair.engine);
        const std::optional<ParsedFrame> opening = engineSide.next();
        CHECK(opening && opening->kind == wire::Opening && pair.engine->write(FrameBytes(0, wire::Ready).i64(1).bytes()));
        auto sink = std::make_shared<StreamSink>();
        std::shared_ptr<SandboxRemoteChannel> channel = end->channel();
        channel->attach(sink);
        const std::optional<ParsedFrame> open = engineSide.next();
        CHECK(open && open->kind == wire::Open);
        channel->event(std::string(size_t(2) << 20, ' '));
        CHECK(eventually([&] { return pair.host->writeWaiting(); }, 2));
        if (bytes) {
            channel->event(std::string(size_t(900) << 10, ' '));
        } else {
            for (size_t i = 1; i < SandboxEngine::kRemoteQueuedCalls; i++) channel->event("{}");
        }
        sleepMs(200);
        CHECK_MSG(!events.ended() && sink->events().empty(), joined(events.all()));
        channel->event(bytes ? std::string(size_t(200) << 10, ' ') : std::string("{}"));
        const std::string reason = bytes ? "more than 3145728 bytes queued for the stream" : "more than 16384 frames queued for the stream";
        CHECK_MSG(eventually([&] { return events.ended().has_value(); }, 2) && endsOnce(events.all(), reason), joined(events.all()));
        CHECK_MSG(sink->events() == std::vector<std::string>{"disconnected"}, joined(sink->events()));
        CHECK(stopsWithin(*end, 2));
    }
}

/// The widget the host's round trip runs for real: a layout query and an Intl request at boot.
static const char* kMeasureIntl = "<p id='p'>x</p><script>var w = document.getElementById('p').getBoundingClientRect().width; console.log('w=' + w + ' ' + (1234.5).toLocaleString());</script>";

/// A client-mode engine over the host's end runs a real engine behind the engine process's end: the opening's bundles boot it from their bytecode, its layout query and Intl request are answered in the host (the delegate, and the host's provider on the client's `widget.client`), a probe sees what it set, and the owner hears the count of runtimes, then at the stop the end once and last, all on `widget.monitor`.
TEST(remote_stream_host_round_trip) {
    auto intl = std::make_shared<StreamIntl>();
    StreamDelegate delegate;
    StreamPeers peers;
    {
        SandboxEngine engine(kMeasureIntl, testConfig(), peers.end->channel(), intl);
        CHECK(engine.isRemote());
        engine.setDelegate(&delegate);
        engine.start();
        CHECK_MSG(eventually([&] { return delegate.saw("log:w=77 INTL"); }, 20), joined(delegate.events()));
        CHECK(eventually([&] { return delegate.saw("loaded"); }));
        CHECK(engine.probe("w + 1") == "78");
        const std::vector<std::string> boots = delegate.eventsWith("diag:boot:");
        CHECK_MSG(boots.size() == 1 && boots[0].find("(bytecode)") != std::string::npos && delegate.eventsWith("query:").size() == 1, joined(delegate.events()));
        CHECK_MSG(eventually([&] { return peers.events.all() == std::vector<std::string>{"runtimes:1"}; }), joined(peers.events.all()));
#if defined(__APPLE__) || defined(__linux__)
        CHECK_MSG(intl->threads() == std::vector<std::string>{"widget.client"}, joined(intl->threads()));
#endif
        engine.terminate();
        CHECK(eventually([&] { return delegate.saw("died:terminated"); }));
        CHECK(engine.probe("1") == "ERROR no runtime");
    }
    CHECK_MSG(eventually([&] { return peers.events.all() == std::vector<std::string>{"runtimes:1", "runtimes:0"}; }), joined(peers.events.all()));
    CHECK(stopsWithin(*peers.end, 2));
    CHECK_MSG(endsOnce(peers.events.all(), "the host stopped the stream"), joined(peers.events.all()));
#if defined(__APPLE__) || defined(__linux__)
    const std::vector<std::string> threads = peers.events.threads();
    CHECK_MSG(std::all_of(threads.begin(), threads.end(), [](const std::string& name) { return name == "widget.monitor"; }), joined(threads));
#endif
    CHECK(peers.serving.within(5).has_value());
}

/// What refuses a widget refuses it with a reason and runs nothing: the refusing channel kills its client at `attach` with the reason (heard once a delegate is set) and answers probes "ERROR no runtime"; an engine process of another build or another stream version, one that refuses, and a runtime's frame before the opening was answered end the stream once with why, each client attached before hears that the stream is lost once and nothing after it (what followed in the same write included), and one attached after hears it at once.
TEST(remote_stream_host_refusals) {
    const std::string lost = "the widget engine process ended";
    {
        auto sink = std::make_shared<StreamSink>();
        std::shared_ptr<SandboxRemoteChannel> refusing = SandboxRemoteHostEnd::refusing("no engine process here");
        refusing->attach(sink);
        refusing->start("<p>x</p>", testConfig());
        refusing->event("{}");
        refusing->stop("x");
        CHECK(refusing->probe("1") == "ERROR no runtime");
        refusing->close();
        CHECK_MSG(sink->events() == std::vector<std::string>{"died:no engine process here"}, joined(sink->events()));
        StreamDelegate delegate;
        SandboxEngine engine("<script>console.log('ran here')</script>", testConfig(), SandboxRemoteHostEnd::refusing("no engine process here"));
        engine.thread().sync([] {});
        engine.setDelegate(&delegate);
        engine.start();
        engine.thread().sync([] {});
        CHECK_MSG(delegate.events() == std::vector<std::string>{"died:no engine process here"}, joined(delegate.events()));
        CHECK(engine.probe("1") == "ERROR no runtime");
    }
    {
        StreamPeers peers("another build");
        StreamDelegate early;
        SandboxEngine before("<p>x</p>", testConfig(), peers.end->channel());
        before.setDelegate(&early);
        before.start();
        const std::string why = "build \"twidget-tests\", this end's is \"another build\"";
        CHECK_MSG(eventually([&] { return peers.events.ended().has_value(); }), joined(peers.events.all()));
        CHECK_MSG(endsOnce(peers.events.all(), "the engine process refused the stream: " + why), joined(peers.events.all()));
        CHECK(peers.serving.within(5) == why);
        CHECK_MSG(eventually([&] { return early.saw("died:" + lost); }), joined(early.events()));
        StreamDelegate late;
        SandboxEngine after("<p>x</p>", testConfig(), peers.end->channel());
        after.setDelegate(&late);
        after.start();
        after.thread().sync([] {});
        before.thread().sync([] {});
        CHECK_MSG(late.events() == std::vector<std::string>{"died:" + lost}, joined(late.events()));
        CHECK_MSG(early.events() == std::vector<std::string>{"died:" + lost}, joined(early.events()));
    }
    const std::vector<std::pair<std::string, std::string>> answers = {
        {FrameBytes(0, wire::Ready).i64(2).bytes(), "stream version 2 from the engine process, this end's is 1"},
        {FrameBytes(0, wire::Refused).str("no room").bytes(), "the engine process refused the stream: no room"},
        {logFrame(1, "early"), "kind 75 on runtime 1 before the opening was answered"},
    };
    for (const auto& [answer, reason] : answers) {
        ScriptedEngine engine;
        auto sink = std::make_shared<StreamSink>();
        std::shared_ptr<SandboxRemoteChannel> channel = engine.host().channel();
        channel->attach(sink);
        CHECK(eventually([&] { return engine.saw(0, "opening") && engine.saw(1, "open"); }));
        CHECK(engine.send(answer + FrameBytes(0, wire::Ready).i64(1).bytes() + logFrame(1, "after")));
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }), reason + ": " + joined(engine.events().all()));
        CHECK_MSG(endsOnce(engine.events().all(), reason), reason + ": " + joined(engine.events().all()));
        CHECK_MSG(sink->events() == std::vector<std::string>{"disconnected"}, reason + ": " + joined(sink->events()));
        CHECK(channel->probe("1") == "ERROR no runtime");
        auto later = std::make_shared<StreamSink>();
        engine.host().channel()->attach(later);
        CHECK_MSG(later->events() == std::vector<std::string>{"disconnected"}, reason + ": " + joined(later->events()));
    }
}

/// The engine process's lockdown runs once between the accepted opening and `Ready`: while it is held, the opening was read whole and the runtime's open and start queued with it neither get the answer nor boot, which both come once it returns ""; a reason it returns, or the fixed one for an exception it throws, refuses the stream and boots nothing, and over a real host's end the stream ends with that refusal and a client attached while it was held dies with the lost stream, never loaded; a malformed opening, another build and a stream that ends before or inside the opening never call it.
TEST(remote_stream_engine_lockdown) {
    const std::string opening = openingOf(*RES, true);
    const std::optional<ParsedFrame> sent = frameAt(opening, uint32_t(opening.size() - 4));
    const std::string queued = opening + FrameBytes(1, wire::Open).bytes() +
                               FrameBytes(1, wire::Start).str("<script>console.log('ran')</script>").str(SandboxRemoteCoding::encodeConfig(testConfig())).bytes();
    {
        Gate held;
        std::atomic<int> calls{0};
        ScriptedHost host(nullptr, [&] {
            calls++;
            return held.wait() ? std::string() : std::string("the test never opened the gate");
        });
        CHECK(host.send(queued));
        CHECK(held.waitedOn());
        WatchedStream& engine = host.engineSide();
        const std::optional<ParsedFrame> first = engine.firstFrame();
        CHECK(sent && first && first->runtime == 0 && first->kind == wire::Opening && first->fields == sent->fields);
        CHECK_MSG(engine.bytesRead() >= opening.size(), std::to_string(engine.bytesRead()) + " of " + std::to_string(opening.size()) + " bytes");
        CHECK(!host.served(0.3));
        CHECK_MSG(host.frameCount() == 0, joined(host.events(0)) + " / " + joined(host.events(1)));
        held.open();
        CHECK_MSG(eventually([&] { return host.saw(1, "log:ran") && host.saw(1, "loaded"); }, 20), joined(host.events(1)));
        CHECK_MSG(host.events(0) == std::vector<std::string>{"ready:1"}, joined(host.events(0)));
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
        ScriptedHost host(nullptr, lockdown);
        CHECK(host.send(queued));
        CHECK_MSG(host.served(5) == reason, reason);
        CHECK_MSG(eventually([&] { return host.events(0) == std::vector<std::string>{"refused:" + reason}; }), reason + ": " + joined(host.events(0)));
        CHECK_MSG(host.frameCount() == 1 && host.events(1).empty() && calls == 1, reason + ": " + joined(host.events(1)));
    }

    {
        Gate attached;
        calls = 0;
        StreamPeers peers(kBuild, *RES, [&] {
            calls++;
            attached.wait();
            return std::string("locked: no");
        });
        CHECK(attached.waitedOn());
        StreamDelegate delegate;
        SandboxEngine client("<script>console.log('ran here')</script>", testConfig(), peers.end->channel());
        client.setDelegate(&delegate);
        client.start();
        client.thread().sync([] {});
        CHECK_MSG(eventually([&] { return peers.events.open() == 1; }), joined(peers.events.all()));
        CHECK_MSG(delegate.events().empty() && !peers.events.ended(), joined(delegate.events()) + " / " + joined(peers.events.all()));
        attached.open();
        CHECK_MSG(eventually([&] { return peers.events.ended().has_value(); }), joined(peers.events.all()));
        CHECK_MSG(endsOnce(peers.events.all(), "the engine process refused the stream: locked: no"), joined(peers.events.all()));
        CHECK(peers.serving.within(5) == std::string("locked: no"));
        CHECK_MSG(eventually([&] { return delegate.saw("died:" + kEngineGone); }), joined(delegate.events()));
        client.thread().sync([] {});
        CHECK_MSG(delegate.events() == std::vector<std::string>{"died:" + kEngineGone}, joined(delegate.events()));
        CHECK(calls == 1);
    }

    struct Uncalled {
        const char* name;
        std::string bytes;
        std::string reason;
    };
    const std::string stamp = firstLine(RES->validatorBundle);
    const std::vector<Uncalled> uncalled = {
        {"a malformed opening", FrameBytes(0, wire::Opening).i64(1).str(kBuild).str("s").str("").str(stamp).bytes(), "kind 1 with a field past the frame's end"},
        {"another build", openingBytes(1, "other", "s", "", stamp, ""), "build \"other\", this end's is \"twidget-tests\""},
        {"the end before the opening", "", ""},
        {"the end inside the opening", smallOpening().substr(0, 30), "the stream ended inside a frame"},
    };
    for (const Uncalled& v : uncalled) {
        calls = 0;
        ScriptedHost host(nullptr, [&] {
            calls++;
            return std::string();
        });
        CHECK(host.send(v.bytes));
        host.close();
        const std::optional<std::string> reason = host.served(2);
        CHECK_MSG(reason == v.reason, std::string(v.name) + ": " + reason.value_or("still serving after 2 s"));
        CHECK_MSG(calls == 0, std::string(v.name) + ": called " + std::to_string(calls) + " times");
    }
}

/// A probe's answer never overtakes what came before it, and only its own sequence answers it: one that comes while the runtime's query is in the host's hands is held until the sink's call returned (at a bare sink, and through a client, whose last barrier then finds the delegate's query done); past the probe bound it is "ERROR timeout" and its late answer is dropped, as a settled sequence's is while the next probe waits; a close or the end of the stream answers a waiting probe "ERROR no runtime", the end only once the client heard it; another runtime's frames, Intl and probes go on while one runtime's query is held, each runtime answered on a `widget.answer` of its own; and an answer held behind a query or Intl request whose own answer is never queued (its channel closed, a host form lost, the engine's side closed, the host's stop, an answer the stream cannot take) is dropped: the probe answers "ERROR no runtime", once its client heard the loss.
TEST(remote_stream_probe_order) {
    const std::string rectQuery = "{\"q\":\"rect\"}";
    const std::string query = FrameBytes(1, wire::Query).str(rectQuery).bytes();
    {
        ScriptedEngine engine;
        Gate gate;
        auto sink = std::make_shared<StreamSink>();
        sink->gate = &gate;
        std::shared_ptr<SandboxRemoteChannel> channel = engine.host().channel();
        channel->attach(sink);
        CHECK(engine.ready() && eventually([&] { return engine.saw(1, "open"); }));
        Background<std::string> probing([&] {
            const std::string answer = channel->probe("code");
            sink->record("probed:" + answer);
            return answer;
        });
        CHECK(eventually([&] { return engine.saw(1, "probe:1:code"); }));
        CHECK(engine.send(logFrame(1, "before") + query + probeAnswer(1, 1, "answered")));
        CHECK(gate.waitedOn());
        CHECK(!probing.within(0.3));
        gate.open();
        CHECK(probing.within(5) == std::optional<std::string>("answered"));
        CHECK_MSG(sink->events() == (std::vector<std::string>{"log:log:before", "query:" + rectQuery, "query-done", "probed:answered"}), joined(sink->events()));
        CHECK(eventually([&] { return engine.saw(1, "query-answer:" + rectOf(77)); }));
#if defined(__APPLE__) || defined(__linux__)
        CHECK_MSG(sink->threadOf("query:").second == "widget.answer" && sink->threadOf("log:").second == "widget.reader", sink->threadOf("query:").second);
#endif
        CHECK(!engine.events().ended());
    }
    {
        ScriptedEngine engine;
        StreamDelegate delegate;
        Gate gate;
        delegate.gate = &gate;
        SandboxEngine client("<p>x</p>", testConfig(), engine.host().channel());
        client.setDelegate(&delegate);
        client.start();
        CHECK(engine.ready() && eventually([&] { return engine.saw(1, "start"); }));
        Background<std::string> probing([&] {
            const std::string answer = client.probe("code");
            delegate.record("probed:" + answer);
            return answer;
        });
        CHECK(eventually([&] { return engine.saw(1, "probe:1:code"); }));
        CHECK(engine.send(logFrame(1, "before") + query + probeAnswer(1, 1, "answered")));
        CHECK(gate.waitedOn());
        CHECK(!probing.within(0.3));
        gate.open();
        CHECK(probing.within(5) == std::optional<std::string>("answered"));
        CHECK_MSG(delegate.events() == (std::vector<std::string>{"log:before", "query:" + rectQuery, "query-done", "probed:answered"}), joined(delegate.events()));
    }
    {
        SandboxRemoteHostEnd::Limits limits;
        limits.probeWaitMs = 300;
        ScriptedEngine engine(limits);
        auto sink = std::make_shared<StreamSink>();
        std::shared_ptr<SandboxRemoteChannel> channel = engine.host().channel();
        channel->attach(sink);
        CHECK(engine.ready() && eventually([&] { return engine.saw(1, "open"); }));
        const auto began = std::chrono::steady_clock::now();
        CHECK(channel->probe("first") == "ERROR timeout");
        const double ms = msSince(began);
        CHECK_MSG(ms >= 300 && ms < 1500, std::to_string(ms) + " ms");
        CHECK(eventually([&] { return engine.saw(1, "probe:1:first"); }));
        // Its answer comes late: dropped, and the stream goes on.
        CHECK(engine.send(probeAnswer(1, 1, "late") + logFrame(1, "alive")));
        CHECK_MSG(eventually([&] { return sink->saw("log:log:alive"); }), joined(sink->events()));
        CHECK(sink->events() == std::vector<std::string>{"log:log:alive"} && !engine.events().ended());
    }
    {
        ScriptedEngine engine;
        auto sink = std::make_shared<StreamSink>();
        std::shared_ptr<SandboxRemoteChannel> channel = engine.host().channel();
        channel->attach(sink);
        CHECK(engine.ready() && eventually([&] { return engine.saw(1, "open"); }));
        const auto probed = [&](const std::string& code, int64_t seq, const std::string& answers) {
            Background<std::string> probing([&] { return channel->probe(code); });
            CHECK(eventually([&] { return engine.saw(1, "probe:" + std::to_string(seq) + ":" + code); }));
            CHECK(engine.send(answers));
            return probing.within(5).value_or("still waiting");
        };
        CHECK(probed("one", 1, probeAnswer(1, 1, "1")) == "1");
        // While probe 2 waits, probe 1's repeated answer answers nothing; probe 2's own does, and its repeat is dropped.
        CHECK(probed("two", 2, probeAnswer(1, 1, "stale") + probeAnswer(1, 2, "fresh") + probeAnswer(1, 2, "again")) == "fresh");
        CHECK(probed("three", 3, probeAnswer(1, 3, "3")) == "3");
        CHECK(sink->events().empty() && !engine.events().ended());
    }
    {
        ScriptedEngine engine;
        auto sink = std::make_shared<StreamSink>();
        std::shared_ptr<SandboxRemoteChannel> channel = engine.host().channel();
        channel->attach(sink);
        CHECK(engine.ready() && eventually([&] { return engine.saw(1, "open"); }));
        Background<std::string> probing([&] {
            const std::string answer = channel->probe("held");
            sink->record("probed:" + answer);
            return answer;
        });
        CHECK(eventually([&] { return engine.saw(1, "probe:1:held"); }));
        channel->close();
        CHECK(probing.within(1) == std::optional<std::string>("ERROR no runtime"));
        CHECK(eventually([&] { return engine.saw(1, "close"); }));
        // The closed runtime's frames are dropped, its late answer among them, and the stream goes on for the next runtime.
        CHECK(engine.send(probeAnswer(1, 1, "late") + logFrame(1, "dropped")));
        auto next = std::make_shared<StreamSink>();
        std::shared_ptr<SandboxRemoteChannel> second = engine.host().channel();
        second->attach(next);
        CHECK(eventually([&] { return engine.saw(2, "open"); }));
        CHECK(engine.send(logFrame(2, "next")));
        CHECK_MSG(eventually([&] { return next->saw("log:log:next"); }), joined(next->events()));
        CHECK_MSG(sink->events() == std::vector<std::string>{"probed:ERROR no runtime"}, joined(sink->events()));
        CHECK(!engine.events().ended());
    }
    {
        ScriptedEngine engine;
        auto sink = std::make_shared<StreamSink>();
        std::shared_ptr<SandboxRemoteChannel> channel = engine.host().channel();
        channel->attach(sink);
        CHECK(engine.ready() && eventually([&] { return engine.saw(1, "open") && engine.events().open() == 1; }));
        Background<std::string> probing([&] {
            const std::string answer = channel->probe("held");
            sink->record("probed:" + answer);
            return answer;
        });
        CHECK(eventually([&] { return engine.saw(1, "probe:1:held"); }));
        engine.close();
        CHECK(probing.within(1) == std::optional<std::string>("ERROR no runtime"));
        CHECK_MSG(sink->events() == (std::vector<std::string>{"disconnected", "probed:ERROR no runtime"}), joined(sink->events()));
        CHECK(eventually([&] { return engine.events().ended().has_value(); }));
        CHECK_MSG(engine.events().all() == (std::vector<std::string>{"runtimes:1", "runtimes:0", "ended:the engine process closed the stream"}), joined(engine.events().all()));
    }
    {
        ScriptedEngine engine;
        Gate gate;
        auto a = std::make_shared<StreamSink>(), b = std::make_shared<StreamSink>();
        a->gate = &gate;
        std::shared_ptr<SandboxRemoteChannel> channelA = engine.host().channel(), channelB = engine.host().channel();
        channelA->attach(a);
        channelB->attach(b);
        CHECK(engine.ready() && eventually([&] { return engine.saw(1, "open") && engine.saw(2, "open"); }));
        CHECK(engine.send(query));
        CHECK(gate.waitedOn());
        Background<std::string> probingB([&] { return channelB->probe("b"); });
        CHECK(eventually([&] { return engine.saw(2, "probe:1:b"); }));
        CHECK(engine.send(logFrame(2, "b goes on") + FrameBytes(2, wire::Intl).str("{}").bytes() + probeAnswer(2, 1, "b answered")));
        CHECK(probingB.within(5) == std::optional<std::string>("b answered"));
        CHECK(eventually([&] { return engine.saw(2, "intl-answer:INTL"); }));
        CHECK_MSG(b->events() == (std::vector<std::string>{"log:log:b goes on", "intl:{}", "intl-done"}), joined(b->events()));
        CHECK_MSG(a->events() == std::vector<std::string>{"query:" + rectQuery} && !engine.saw(1, "query-answer:" + rectOf(77)), joined(a->events()));
        CHECK(a->threadOf("query:").first != b->threadOf("intl:").first);
        gate.open();
        CHECK(eventually([&] { return engine.saw(1, "query-answer:" + rectOf(77)); }));
        CHECK_MSG(a->events() == (std::vector<std::string>{"query:" + rectQuery, "query-done"}), joined(a->events()));
        CHECK(!engine.events().ended());
    }
    enum class Ending { EngineClosed, HostStopped, Unqueued, Lost, Closed };
    struct Cut {
        std::string name;
        Ending how;
        bool intl;
        /// Why the stream ends, "" when it goes on.
        std::string reason;
    };
    const std::vector<Cut> cuts = {
        {"a query, then the engine's side closed", Ending::EngineClosed, false, "the engine process closed the stream"},
        {"an Intl request, then the host's stop", Ending::HostStopped, true, "the host stopped the stream"},
        {"an Intl request whose answer the stream cannot take", Ending::Unqueued, true, "more than 4096 bytes queued for the stream"},
        {"a query, then a host form that does not decode", Ending::Lost, false, ""},
        {"a query, then its channel closed", Ending::Closed, false, ""},
    };
    for (const Cut& cut : cuts) {
        const std::string& name = cut.name;
        SandboxRemoteHostEnd::Limits limits;
        if (cut.how == Ending::Unqueued) limits.queuedBytes = 4096;
        Gate held, lost;
        ScriptedEngine engine(limits);
        auto neighbor = std::make_shared<StreamSink>(), sink = std::make_shared<StreamSink>();
        sink->gate = &held;
        sink->lost = &lost;
        if (cut.how == Ending::Unqueued) sink->intlAnswer = std::string(8192, 'i');
        std::shared_ptr<SandboxRemoteChannel> neighborChannel = engine.host().channel(), channel = engine.host().channel();
        neighborChannel->attach(neighbor);
        channel->attach(sink);
        CHECK_MSG(engine.ready() && eventually([&] { return engine.saw(1, "open") && engine.saw(2, "open"); }), name);
        Background<std::string> probing([&] {
            const std::string answer = channel->probe("code");
            sink->record("probed:" + answer);
            return answer;
        });
        CHECK_MSG(eventually([&] { return engine.saw(2, "probe:1:code"); }), name);
        const std::string asked = cut.intl ? FrameBytes(2, wire::Intl).str("{}").bytes() : FrameBytes(2, wire::Query).str(rectQuery).bytes();
        const std::string request = cut.intl ? "intl:{}" : "query:" + rectQuery, done = cut.intl ? "intl-done" : "query-done";
        // The neighbor's log comes after the probe's answer: once it is delivered, that answer is held behind the request.
        CHECK(engine.send(asked + probeAnswer(2, 1, "answered") + logFrame(1, "behind")));
        CHECK_MSG(held.waitedOn() && eventually([&] { return neighbor->saw("log:log:behind"); }), name + ": " + joined(neighbor->events()));
        if (cut.how == Ending::Closed) {
            channel->close();
            CHECK_MSG(probing.within(1) == std::optional<std::string>("ERROR no runtime"), name);
            held.open();
            CHECK_MSG(eventually([&] { return sink->saw(done); }), name);
        } else {
            // The client hears the loss at its gate, which the test holds while the sink's answer settles.
            Background<bool> cutting([&] {
                if (cut.how == Ending::EngineClosed) engine.close();
                if (cut.how == Ending::HostStopped) engine.host().stop();
                if (cut.how == Ending::Lost) channel->setData(WidgetData{std::string(SandboxEngine::kRemoteMessageLimit, 'x'), std::nullopt});
                return true;
            });
            // Unqueued: the sink's answer itself ends the stream.
            if (cut.how != Ending::Unqueued) CHECK_MSG(lost.waitedOn(), name);
            held.open();
            CHECK_MSG(eventually([&] { return sink->saw(done); }) && lost.waitedOn(), name + ": " + joined(sink->events()));
            CHECK_MSG(!probing.within(0.5), name + ": the probe answered " + probing.within(0).value_or("?") + " before its client heard the loss");
            lost.open();
            CHECK_MSG(probing.within(5) == std::optional<std::string>("ERROR no runtime"), name + ": " + joined(sink->events()));
            CHECK_MSG(cutting.finish(5), name);
        }
        // No call on the channel runs when it goes (a probe still waiting ends the run), and its `widget.answer` joined: whatever the sink's answer queued is queued by now.
        probing.finish(5);
        channel.reset();
        const std::vector<std::string> heard = cut.how == Ending::Closed ? std::vector<std::string>{request, "probed:ERROR no runtime", done} : std::vector<std::string>{request, done, "disconnected", "probed:ERROR no runtime"};
        CHECK_MSG(sink->events() == heard, name + ": " + joined(sink->events()));
        if (cut.reason.empty()) {
            // The neighbor's next frame comes after anything the closed runtime's answer could have queued.
            neighborChannel->event("{}");
            CHECK_MSG(eventually([&] { return engine.saw(1, "event:{}"); }) && engine.saw(2, "close"), name + ": " + joined(engine.frames(2)));
            CHECK_MSG(!engine.events().ended() && neighbor->events() == std::vector<std::string>{"log:log:behind"}, name + ": " + joined(engine.events().all()));
        } else {
            CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }) && endsOnce(engine.events().all(), cut.reason), name + ": " + joined(engine.events().all()));
            CHECK_MSG(neighbor->events() == (std::vector<std::string>{"log:log:behind", "disconnected"}), name + ": " + joined(neighbor->events()));
        }
        const std::vector<std::string> frames = engine.frames(2);
        CHECK_MSG(std::none_of(frames.begin(), frames.end(), [](const std::string& f) { return f.rfind("query-answer:", 0) == 0 || f.rfind("intl-answer:", 0) == 0; }), name + ": " + joined(frames));
    }
}

// ───────────── through WidgetHost ─────────────

/// The loopback's real-engine widget and recorder over the stream: inputs before the start (hidden, a safe area, data, a palette, a viewport) cross coded and keep their place before the boot, the delegate's layout answer and the host's Intl provider reach the widget, storage, a link, a request and the wallet call on its grant go through the client's checks and back by id, the validator's charge comes back in the stats and a terminate stops both halves; a probe after a tap returns with what its task sent delivered, every delegate call comes on the client's one dispatch thread, and the owner's count goes from 1 back to 0.
TEST(remote_stream_carries_calls) {
    auto intl = std::make_shared<RecordingIntl>();
    RemoteRecorder recorder;
    intl->into = &recorder;
    recorder.queryAnswer = kRect77;
    SandboxConfig config = walletConfig();
    config.data = WidgetData{std::string("chat1"), std::nullopt};
    StreamPeers peers;
    {
        SandboxEngine engine(kRealEngineWidget, config, peers.end->channel(), intl);
        CHECK(engine.isRemote());
        engine.setDelegate(&recorder);
        engine.setVisible(false);
        engine.contentSafeArea(WidgetContentSafeArea{0, 0, 24, 0, 0, 64});
        engine.setData(WidgetData{std::string("chat2"), std::nullopt});
        engine.theme("dark", {{"bg_color", "#112233"}});
        engine.viewport(320, 240.5, 2);
        engine.start();
        CHECK(eventually([&] { return has(recorder.events(), "loaded"); }, 20));
        const std::string booted = "[\"hidden\",24,\"chat1\",77,\"INTL\",\"chat:chat2\"]";
        CHECK_MSG(probeUntil(engine, "JSON.stringify(log)", booted) == booted, engine.probe("JSON.stringify(log)"));
        const std::string looks = engine.probe("[TelegramWidget.colorScheme, TelegramWidget.themeParams.bg_color, innerWidth, innerHeight, devicePixelRatio].join(' ')");
        CHECK_MSG(looks == "dark #112233 320 240.5 2", looks);
        CHECK_MSG(eventually([&] { return peers.events.all() == std::vector<std::string>{"runtimes:1"}; }), joined(peers.events.all()));
        engine.setVisible(true);
        // A probe's answer comes behind the tap's task: what the task sent has reached the delegate when it returns.
        engine.activationEvent(kTapOnButton, 1);
        CHECK(engine.probe("taps") == "1");
        const std::string link = linkAction("openLink", "https://example.com/");
        CHECK_MSG(has(recorder.events(), link), joined(recorder.events()));
        engine.activationEvent(kTapOnButton, 2);
        CHECK(engine.probe("taps") == "2");
        CHECK_MSG(recorder.responder(0).has_value(), joined(recorder.events()));
        CHECK(recorder.respond(0, answered(std::string("r"))));
        CHECK(eventually([&] { return recorder.responder(1).has_value(); }));
        CHECK(recorder.respond(1, answered(std::string("s"))));
        const std::string settled = booted.substr(0, booted.size() - 1) + ",\"ok:r\",\"signed:s\"]";
        CHECK_MSG(probeUntil(engine, "JSON.stringify(log)", settled) == settled, engine.probe("JSON.stringify(log)"));
        const std::vector<std::string> events = recorder.events();
        CHECK_MSG((recorder.eventsWith("request:") == std::vector<std::string>{"request:ask:wallet"} && recorder.eventsWith("wallet:") == std::vector<std::string>{"wallet:signData:w"}), joined(events));
        const std::vector<WalletCall> calls = recorder.walletCalls();
        CHECK(calls.size() == 1 && calls[0].payload.text == std::string("sign"));
        CHECK(recorder.eventsWith("bridge:") == std::vector<std::string>{"bridge:openLink:" + linkArgs("https://example.com/")});
        CHECK((recorder.commits() == std::vector<std::pair<std::string, StorageArea>>{{"local", StorageArea{{"k", "v"}}}}));
        CHECK(recorder.eventsWith("query:").size() == 1 && recorder.eventsWith("intl:").size() == 1);
        const std::thread::id dispatch = engine.thread().sync([] { return std::this_thread::get_id(); });
        const std::vector<std::thread::id> threads = recorder.threads();
        CHECK(!threads.empty() && dispatch != std::this_thread::get_id() && std::all_of(threads.begin(), threads.end(), [&](std::thread::id id) { return id == dispatch; }));
#if defined(__APPLE__) || defined(__linux__)
        CHECK(nameOf(engine.thread()) == "widget.client");
#endif
        engine.chargeValidator(7);
        CHECK(eventually([&] {
            const std::vector<SandboxStats> stats = recorder.stats();
            return !stats.empty() && stats.back().validatorMs == 7;
        }));
        engine.terminate();
        CHECK(eventually([&] { return !recorder.deaths().empty(); }));
        engine.thread().sync([] {});
        CHECK(recorder.deaths() == std::vector<std::string>{"terminated"} && engine.probe("1") == "ERROR no runtime");
    }
    CHECK_MSG(eventually([&] { return peers.events.all() == std::vector<std::string>{"runtimes:1", "runtimes:0"}; }), joined(peers.events.all()));
    CHECK(stopsWithin(*peers.end, 2));
    CHECK_MSG(endsOnce(peers.events.all(), "the host stopped the stream"), joined(peers.events.all()));
    CHECK(peers.serving.within(5).has_value());
}

/// A real WidgetHost over the stream: its engine runs from the opening's bytes, which carry the sandbox bundle and its bytecode and only the stamp lines of the validator and renderer bundles that stay in the host; the widget's validated ops carry its layout answer and the host's Intl, formatted by the host's provider in this process on the client's `widget.client`; a tapped request and the wallet call on its grant reach the host's callbacks, and their answers the widget's state. Without bytecode it boots from the source, and a sandbox bundle of another release is refused at boot with the in-process verdict. The fixture destroyed while its WidgetHost runs one of its callbacks waits for that callback, which ends on the fixture's own storage.
TEST(remote_stream_widget_host) {
    WidgetHostOptions privileged;
    privileged.context = messageContext(false, true, false, "private");
    privileged.context.wallet = true;
    const std::string bytecode(RES->sandboxBytecode.begin(), RES->sandboxBytecode.end());
    const std::string validatorStamp = firstLine(RES->validatorBundle), rendererStamp = firstLine(RES->rendererBundle);
    CHECK(!bytecode.empty() && validatorStamp.size() < 200 && rendererStamp.size() < 200 && RES->validatorBundle.size() > 100000 && RES->rendererBundle.size() > 10000);
    {
        StreamPeers peers;
        StreamHost h(*peers.end, kHostWidget, privileged, 20, "INTL");
        CHECK(h.asked == 1);
        CHECK(h.shown());
        WidgetHost& host = *h.host;
        const WidgetDiagnostics d = host.diagnostics();
        CHECK_MSG(d.loaded && d.rendered && !d.dead, d.dead.value_or("alive"));
        CHECK_MSG(h.renders("h:20") && h.renders("n:INTL"), h.rendered().substr(0, 2000));
        CHECK(!h.renderer->sent("query").empty());
        CHECK_MSG(bootedFrom(d, "(bytecode)"), joined(bootsOf(d)));
        CHECK_MSG(h.intl->processes() == std::vector<long>{processId()}, std::to_string(h.intl->processes().size()) + " requests");
#if defined(__APPLE__) || defined(__linux__)
        CHECK_MSG(h.intl->threads() == std::vector<std::string>{"widget.client"}, joined(h.intl->threads()));
#endif
        const std::vector<std::string> opening = openingFields(peers.engineSide->firstFrame());
        CHECK_MSG((opening == std::vector<std::string>{"1", kBuild, RES->sandboxBundle, bytecode, validatorStamp, rendererStamp}), std::to_string(opening.size()) + " fields");
        CHECK(eventually([&] { return peers.events.open() == 1; }));

        h.tap();
        CHECK_MSG(eventually([&] { return h.calls() == std::vector<std::string>{"request:ask:wallet"}; }), joined(h.calls()));
        CHECK(h.respond(0, answered(std::string("r"))));
        CHECK_MSG(eventually([&] { return h.calls().size() == 2; }), joined(h.calls()));
        CHECK_MSG((h.calls() == std::vector<std::string>{"request:ask:wallet", "wallet:signData:w:sign"}), joined(h.calls()));
        CHECK(h.respond(1, answered(std::string("s"))));
        CHECK_MSG(probeUntil(host, "log.join(' ')", "ok:r signed:s") == "ok:r signed:s", host.probe("log.join(' ')"));
        CHECK_MSG(!host.diagnostics().dead && !peers.events.ended(), joined(peers.events.all()));
    }
    {
        SandboxResources source = *RES;
        source.sandboxBytecode.clear();
        StreamPeers peers(kBuild, source);
        StreamHost h(*peers.end, kHostWidget, privileged, 20, "INTL");
        CHECK(h.shown());
        const WidgetDiagnostics d = h.host->diagnostics();
        CHECK_MSG(h.renders("h:20") && h.renders("n:INTL") && !d.dead, h.rendered().substr(0, 2000));
        CHECK_MSG(bootedFrom(d, "(source)"), joined(bootsOf(d)));
        const std::vector<std::string> opening = openingFields(peers.engineSide->firstFrame());
        CHECK_MSG((opening == std::vector<std::string>{"1", kBuild, RES->sandboxBundle, "", validatorStamp, rendererStamp}), std::to_string(opening.size()) + " fields");
    }
    {
        const BundleStamp mine = BundleStamp::glue();
        SandboxResources other = *RES;
        other.sandboxBundle = BundleStamp{"9.9.9", mine.protocol + 1, mine.profile, mine.sdk}.line() + RES->sandboxBundle.substr(RES->sandboxBundle.find('\n'));
        const std::string refusal = BundleStamp::refusal(other).value_or("");
        CHECK_MSG(refusal.rfind("sandbox-bundle.js is widget-sandbox 9.9.9 ", 0) == 0, refusal);
        StreamPeers peers(kBuild, other);
        StreamHost h(*peers.end, "<script>console.log('ran here')</script>", WidgetHostOptions(), 20, "INTL");
        CHECK(h.host->waitUntil([](const WidgetDiagnostics& d) { return d.dead.has_value(); }, 20));
        settleHost(*h.host);
        const WidgetDiagnostics d = h.host->diagnostics();
        const auto mismatch = d.diags.find("bundle-mismatch");
        CHECK_MSG(d.dead == std::optional<std::string>("sandbox bundle failed to load") && !d.loaded && !d.hasLog("ran here") && d.countDiag("boot") == 0, d.dead.value_or("alive"));
        CHECK_MSG(mismatch != d.diags.end() && mismatch->second.first == 1 && mismatch->second.second == std::vector<std::string>{refusal}, refusal);
        const std::vector<std::string> opening = openingFields(peers.engineSide->firstFrame());
        CHECK_MSG(opening.size() == 6 && opening[2] == other.sandboxBundle, std::to_string(opening.size()) + " fields");
    }
    {
        StreamPeers peers;
        auto h = std::make_unique<StreamHost>(*peers.end, kHostWidget, privileged, 20, "INTL");
        CHECK(h->shown() && eventually([&] { return peers.events.open() == 1; }));
        WidgetHost& host = *h->host;
        Gate held;
        std::atomic<bool> kept{false};
        // The fixture's own request callback, held on the host thread before it records into the fixture.
        host.onHostThread([&] {
            host.onRequest = [&held, &kept, keep = host.onRequest](const WidgetRequest& request, StreamHost::Respond respond) {
                held.wait();
                keep(request, std::move(respond));
                kept = true;
            };
        });
        h->tap();
        CHECK(held.waitedOn());
        Background<bool> destroying([&] {
            h.reset();
            return true;
        });
        // The fixture's destruction reached its WidgetHost (its engine closed its runtime), which waits for the callback.
        CHECK_MSG(eventually([&] { return peers.events.open() == 0; }), joined(peers.events.all()));
        CHECK(!destroying.within(0.2) && !kept);
        held.open();
        CHECK(destroying.finish(5) && kept);
    }
}

/// A widget of several on one stream: its chat state (a letter), its renderer's layout answer and its host's Intl answer in its text; a tap asks for its letter, and a new chat state is logged.
static const char* kLetteredWidget =
    "<button id='b'>go</button><div id='d'>a</div><p id='n'>b</p><script>var d = document.getElementById('d'), log = []; d.style.height = '50px';"
    "d.textContent = 'h:' + TelegramWidget.chatData + ':' + d.offsetHeight; document.getElementById('n').textContent = 'n:' + (1234.5).toLocaleString();"
    "TelegramWidget.onEvent('chatDataChanged', function () { log.push('chat:' + TelegramWidget.chatData); });"
    "document.getElementById('b').onclick = function () { TelegramWidget.request('ask-' + TelegramWidget.chatData).then(function (a) { log.push('ok:' + a.result); },"
    "  function (e) { log.push('err:' + e.code); }); };</script>";

/// Several WidgetHosts on one stream each get only their own: their chat state, their renderer's layout answer and their provider's Intl answer in their validated ops, their own request and its answer; the owner's count follows each host made and destroyed, the others go on after one is destroyed and one made later joins them, and the stream ends once, after a last count of 0.
TEST(remote_stream_multiple_runtimes) {
    StreamPeers peers;
    std::map<std::string, std::unique_ptr<StreamHost>> hosts;
    std::map<std::string, int> heights;
    const auto make = [&](const std::string& letter, int height) {
        WidgetHostOptions options;
        options.context = messageContext(false, true, false, "private");
        options.data = WidgetData{letter, std::nullopt};
        heights[letter] = height;
        hosts[letter] = std::make_unique<StreamHost>(*peers.end, kLetteredWidget, options, height, "INTL-" + letter);
    };
    // Its own letter, height and Intl answer, and nothing of another letter's.
    const auto showsOnlyItsOwn = [&](const std::string& letter) {
        StreamHost& h = *hosts.at(letter);
        if (!h.shown() || !h.renders("h:" + letter + ":" + std::to_string(heights[letter])) || !h.renders("n:INTL-" + letter)) return false;
        const std::string rendered = h.rendered();
        for (const auto& [other, height] : heights)
            if (other != letter && (rendered.find("h:" + other + ":") != std::string::npos || rendered.find("INTL-" + other) != std::string::npos)) return false;
        return h.intl->threads().size() == 1 && h.asked == 1;
    };
    size_t open = 0;
    for (const auto& [letter, height] : std::vector<std::pair<std::string, int>>{{"A", 21}, {"B", 22}, {"C", 23}}) {
        make(letter, height);
        open++;
        CHECK_MSG(eventually([&] { return peers.events.open() == open; }), letter + ": " + joined(peers.events.all()));
    }
    for (const char* letter : {"A", "B", "C"}) CHECK_MSG(showsOnlyItsOwn(letter), std::string(letter) + ": " + hosts.at(letter)->rendered().substr(0, 2000));

    hosts.at("A")->tap();
    hosts.at("C")->tap();
    CHECK_MSG(eventually([&] { return hosts.at("A")->calls() == std::vector<std::string>{"request:ask-A"}; }), joined(hosts.at("A")->calls()));
    CHECK_MSG(eventually([&] { return hosts.at("C")->calls() == std::vector<std::string>{"request:ask-C"}; }), joined(hosts.at("C")->calls()));
    CHECK(hosts.at("C")->respond(0, answered(std::string("for-C"))) && hosts.at("A")->respond(0, answered(std::string("for-A"))));
    CHECK(probeUntil(*hosts.at("A")->host, "log.join(' ')", "ok:for-A") == "ok:for-A");
    CHECK(probeUntil(*hosts.at("C")->host, "log.join(' ')", "ok:for-C") == "ok:for-C");
    CHECK(hosts.at("B")->host->probe("log.join(' ')").empty() && hosts.at("B")->calls().empty());

    // B goes; A and C go on, and D joins them.
    hosts.erase("B");
    CHECK_MSG(eventually([&] { return peers.events.open() == 2; }), joined(peers.events.all()));
    hosts.at("A")->host->setData(WidgetData{std::string("A2"), std::nullopt});
    hosts.at("C")->host->setData(WidgetData{std::string("C2"), std::nullopt});
    CHECK(probeUntil(*hosts.at("A")->host, "log.join(' ')", "ok:for-A chat:A2") == "ok:for-A chat:A2");
    CHECK(probeUntil(*hosts.at("C")->host, "log.join(' ')", "ok:for-C chat:C2") == "ok:for-C chat:C2");
    make("D", 24);
    CHECK_MSG(eventually([&] { return peers.events.open() == 3; }), joined(peers.events.all()));
    CHECK_MSG(showsOnlyItsOwn("D"), hosts.at("D")->rendered().substr(0, 2000));
    hosts.at("D")->tap();
    CHECK_MSG(eventually([&] { return hosts.at("D")->calls() == std::vector<std::string>{"request:ask-D"}; }), joined(hosts.at("D")->calls()));
    CHECK(hosts.at("D")->respond(0, answered(std::string("for-D"))));
    CHECK(probeUntil(*hosts.at("D")->host, "log.join(' ')", "ok:for-D") == "ok:for-D");
    for (const char* letter : {"A", "C"}) CHECK(hosts.at(letter)->calls().size() == 1 && !hosts.at(letter)->host->diagnostics().dead);
    CHECK_MSG(!peers.events.ended(), joined(peers.events.all()));

    hosts.clear();
    CHECK_MSG(eventually([&] { return peers.events.open() == 0; }), joined(peers.events.all()));
    CHECK(stopsWithin(*peers.end, 2));
    CHECK_MSG(endsOnce(peers.events.all(), "the host stopped the stream"), joined(peers.events.all()));
    CHECK(peers.serving.within(5).has_value());
}

/// A WidgetHost given the transport's refusing channel runs no engine: before `start` nothing reaches it, its observer or its renderer, however long it is left; after it the widget dies once with the refusal's reason, frozen with the owner's label, never loaded, booted or run, and a probe answers "ERROR no runtime". Set at once, with no wait between, the same holds.
TEST(remote_stream_refusing_widget_host) {
    const std::string reason = "no engine process for widgets here";
    for (const bool waitFirst : {true, false}) {
        FactoryReset reset;
        std::atomic<int> asked{0};
        SandboxEngine::setRemoteChannelFactory([&](std::shared_ptr<SandboxResources>) {
            asked++;
            return SandboxRemoteHostEnd::refusing(reason);
        });
        ObservedChanges observed;
        ScriptedRenderer* renderer = nullptr;
        WidgetHost host("<script>console.log('ran here')</script>", WidgetTheme{}, 300, WidgetHostOptions(), RES, scripted(renderer));
        if (waitFirst) {
            settleHost(host);
            const WidgetDiagnostics before = host.diagnostics();
            CHECK_MSG(!before.dead && !before.loaded && before.logs.empty() && before.diags.empty(), before.dead.value_or("alive"));
            std::lock_guard<std::mutex> l(renderer->mutex);
            CHECK(renderer->messages.empty());
        }
        host.stoppedLabel = "stopped: refused";
        host.onChange = observed.observer();
        host.start();
        CHECK(host.waitUntil([](const WidgetDiagnostics& d) { return d.dead.has_value(); }, 5));
        settleHost(host);
        const WidgetDiagnostics after = host.diagnostics();
        CHECK_MSG(after.dead == reason && !after.loaded && after.logs.empty() && after.countDiag("boot") == 0, after.dead.value_or("alive"));
        const std::vector<Json> frozen = renderer->sent("frozen");
        CHECK_MSG(frozen.size() == 1 && frozen[0]["reason"].asString() == "stopped: refused" && frozen[0]["fatal"].asBool(), frozen.empty() ? "no frozen" : frozen[0].dump());
        const std::vector<WidgetDiagnostics> seen = observed.all();
        CHECK_MSG(seen.size() == 1 && seen[0].dead == reason, std::to_string(seen.size()) + " changes seen");
        CHECK(host.probe("1") == "ERROR no runtime" && asked == 1);
    }
}

/// A host form the stream cannot carry loses its runtime as over the loopback, and only it: a config without a finite viewport, data past the message limit and a request's answer past it each kill their client once with the lost connection, as the loopback's do; a neighboring runtime on the same stream goes on answering probes, the owner's count comes back to it alone and the stream never ends.
TEST(remote_stream_invalid_host_form) {
    SandboxConfig broken = testConfig();
    broken.innerWidth = std::numeric_limits<double>::quiet_NaN();
    SandboxConfig asking = testConfig();
    asking.context = messageContext(false, true, false, "private");
    struct Loss {
        std::string name;
        /// What its client heard: everything for a runtime that never ran, the deaths for one that did.
        std::vector<std::string> heard;
        std::function<std::vector<std::string>(std::shared_ptr<SandboxRemoteChannel>)> lose;
    };
    const std::vector<Loss> cases = {
        {"a config without a finite viewport", {"died:" + kEngineGone},
         [&](std::shared_ptr<SandboxRemoteChannel> channel) {
             RemoteRecorder recorder;
             SandboxEngine engine(kMeasure, broken, std::move(channel));
             engine.setDelegate(&recorder);
             engine.start();
             CHECK(eventually([&] { return !recorder.deaths().empty(); }));
             engine.thread().sync([] {});
             return recorder.events();
         }},
        {"data past the message limit", {kEngineGone},
         [&](std::shared_ptr<SandboxRemoteChannel> channel) {
             RemoteRecorder recorder;
             SandboxEngine engine(kMeasure, testConfig(), std::move(channel));
             engine.setDelegate(&recorder);
             engine.start();
             CHECK(eventually([&] { return has(recorder.events(), "loaded"); }, 20));
             engine.setData(WidgetData{std::string(SandboxEngine::kRemoteMessageLimit, 'x'), std::nullopt});
             CHECK(eventually([&] { return !recorder.deaths().empty(); }));
             engine.thread().sync([] {});
             return recorder.deaths();
         }},
        {"an answer past the message limit", {kEngineGone},
         [&](std::shared_ptr<SandboxRemoteChannel> channel) {
             RemoteRecorder recorder;
             SandboxEngine engine(kRequester, asking, std::move(channel));
             engine.setDelegate(&recorder);
             engine.start();
             CHECK(eventually([&] { return has(recorder.events(), "loaded"); }, 20));
             engine.activationEvent(kTapOnButton, 1);
             CHECK(eventually([&] { return recorder.responder(0).has_value(); }));
             CHECK(recorder.respond(0, answered(std::string(SandboxEngine::kRemoteMessageLimit, 'a'))));
             CHECK(eventually([&] { return !recorder.deaths().empty(); }));
             engine.thread().sync([] {});
             return recorder.deaths();
         }},
    };
    auto intl = std::make_shared<StreamIntl>();
    StreamDelegate delegate;
    StreamPeers peers;
    {
        SandboxEngine neighbor(kMeasureIntl, testConfig(), peers.end->channel(), intl);
        neighbor.setDelegate(&delegate);
        neighbor.start();
        CHECK_MSG(eventually([&] { return delegate.saw("log:w=77 INTL"); }, 20), joined(delegate.events()));
        CHECK(eventually([&] { return peers.events.open() == 1; }));
        int probes = 0;
        for (const auto& [name, heard, lose] : cases) {
            const std::vector<std::string> loopback = lose(std::make_shared<SandboxLoopbackChannel>(RES));
            const std::vector<std::string> stream = lose(peers.end->channel());
            CHECK_MSG(loopback == heard, name + ": " + joined(loopback));
            CHECK_MSG(stream == loopback, name + ": " + joined(stream) + " for the loopback's " + joined(loopback));
            CHECK_MSG(eventually([&] { return peers.events.open() == 1; }), name + ": " + joined(peers.events.all()));
            probes++;
            CHECK_MSG(neighbor.probe("w + " + std::to_string(probes)) == std::to_string(77 + probes), name);
            CHECK_MSG(!peers.events.ended() && !peers.serving.within(0), name + ": " + joined(peers.events.all()));
        }
        CHECK_MSG(delegate.eventsWith("died:").empty(), joined(delegate.events()));
    }
    CHECK(stopsWithin(*peers.end, 2));
    CHECK_MSG(endsOnce(peers.events.all(), "the host stopped the stream"), joined(peers.events.all()));
}

// ───────────── hostile bytes, stuck work and teardown ─────────────

static std::vector<Hostile> hostileInputs() {
    const uint32_t bound = uint32_t(SandboxRemoteWire::kMaxFrameBytes);
    const std::string past = " bytes, past the bound of " + std::to_string(bound);
    FrameBytes tenNumbers(1, wire::Stats);
    for (int i = 0; i < 10; i++) tenNumbers.f64(0);
    std::vector<Hostile> out;
    for (uint32_t size = 0; size < 5; size++)
        out.push_back({.name = "a size of " + std::to_string(size), .bytes = wire::u32(size), .reason = "a frame of " + std::to_string(size) + " bytes, less than its runtime and kind"});
    out.push_back({.name = "a size one past the bound, no body", .bytes = wire::u32(bound + 1), .reason = "a frame of " + std::to_string(bound + 1) + past, .bodiless = true});
    out.push_back({.name = "the largest size, no body", .bytes = wire::u32(UINT32_MAX), .reason = "a frame of 4294967295" + past, .bodiless = true});
    for (const int kind : {0, 22, 63, 88, 255}) out.push_back({.name = "kind " + std::to_string(kind), .bytes = bare(1, uint8_t(kind)), .reason = "an unknown kind " + std::to_string(kind)});
    const std::vector<Hostile> rest = {
        {.name = "the host's event", .bytes = FrameBytes(1, wire::Event).str("{}").bytes(), .reason = "kind 4 from the other side"},
        {.name = "the host's close", .bytes = bare(1, wire::Close), .reason = "kind 21 from the other side"},
        {.name = "the host's opening", .bytes = openingBytes(1, kBuild, "s", "", "", ""), .reason = "kind 1 from the other side"},
        {.name = "Ready on a runtime", .bytes = FrameBytes(1, wire::Ready).i64(1).bytes(), .reason = "kind 64 on runtime 1, not the connection"},
        {.name = "a log on the connection", .bytes = logFrame(0, "x"), .reason = "kind 75 on the connection, not a runtime"},
        {.name = "loaded on the connection", .bytes = bare(0, wire::Loaded), .reason = "kind 84 on the connection, not a runtime"},
        {.name = "a string longer than its frame", .bytes = FrameBytes(1, wire::Log).str("log").raw(wire::u32(1000)).raw("abc").bytes(), .reason = "kind 75 with a field past the frame's end"},
        {.name = "a string of the largest length", .bytes = FrameBytes(1, wire::Ops).raw(wire::u32(UINT32_MAX)).raw("[]").bytes(), .reason = "kind 68 with a field past the frame's end"},
        {.name = "a length cut short", .bytes = FrameBytes(1, wire::CopyText).raw(std::string("\x01\0", 2)).bytes(), .reason = "kind 78 with a field past the frame's end"},
        {.name = "a request without its bool", .bytes = FrameBytes(1, wire::Request).i64(1).str("d").bytes(), .reason = "kind 79 with a field past the frame's end"},
        {.name = "stats of ten numbers", .bytes = tenNumbers.bytes(), .reason = "kind 85 with a field past the frame's end"},
        {.name = "a byte after a log's fields", .bytes = FrameBytes(1, wire::Log).str("log").str("x").raw("x").bytes(), .reason = "kind 75 with bytes after its fields"},
        {.name = "a byte after loaded", .bytes = FrameBytes(1, wire::Loaded).raw(std::string(1, '\0')).bytes(), .reason = "kind 84 with bytes after its fields"},
        {.name = "a bool of 2", .bytes = FrameBytes(1, wire::WatchesPublicData).boolean(2).bytes(), .reason = "kind 83 with a bool of 2"},
        {.name = "a request's bool of 255", .bytes = FrameBytes(1, wire::Request).i64(1).str("d").boolean(255).bytes(), .reason = "kind 79 with a bool of 255"},
        {.name = "a probe answer of sequence 0", .bytes = probeAnswer(1, 0, "x"), .reason = "kind 87 with a sequence below 1"},
        {.name = "a probe answer of a negative sequence", .bytes = probeAnswer(1, -5, "x"), .reason = "kind 87 with a sequence below 1"},
        {.name = "a runtime never opened", .bytes = logFrame(3, "x"), .reason = "kind 75 on runtime 3, which was never opened"},
        {.name = "the largest runtime id", .bytes = logFrame(UINT32_MAX, "x"), .reason = "kind 75 on runtime 4294967295, which was never opened"},
        {.name = "a second Ready", .bytes = FrameBytes(0, wire::Ready).i64(1).bytes(), .reason = "kind 64 after the opening was answered"},
        {.name = "a refusal after Ready", .bytes = FrameBytes(0, wire::Refused).str("late").bytes(), .reason = "kind 65 after the opening was answered"},
        {.name = "a runtime's frame before Ready", .bytes = logFrame(1, "early"), .reason = "kind 75 on runtime 1 before the opening was answered", .first = true},
        {.name = "a refusal", .bytes = FrameBytes(0, wire::Refused).str("no room").bytes(), .reason = "the engine process refused the stream: no room", .first = true},
        {.name = "a long refusal", .bytes = FrameBytes(0, wire::Refused).str(std::string(5000, 'r')).bytes(),
         .reason = "the engine process refused the stream: " + std::string(SandboxEngine::kRemoteTextLimit, 'r'), .first = true},
        {.name = "another version", .bytes = FrameBytes(0, wire::Ready).i64(2).bytes(), .reason = "stream version 2 from the engine process, this end's is 1", .first = true},
        {.name = "version 0", .bytes = FrameBytes(0, wire::Ready).i64(0).bytes(), .reason = "stream version 0 from the engine process, this end's is 1", .first = true},
        {.name = "a query while a query is unanswered", .bytes = queryFrame(1, kRectQuery), .reason = "kind 69 on runtime 1 while its query is unanswered", .held = queryFrame(1, kRectQuery)},
        {.name = "Intl while a query is unanswered", .bytes = intlFrame(1, "{}"), .reason = "kind 70 on runtime 1 while its query is unanswered", .held = queryFrame(1, kRectQuery)},
        {.name = "a query while Intl is unanswered", .bytes = queryFrame(1, kRectQuery), .reason = "kind 69 on runtime 1 while its Intl request is unanswered", .held = intlFrame(1, "{}")},
        {.name = "a log while a query is unanswered", .bytes = logFrame(1, "x"), .reason = "kind 75 on runtime 1 while its query is unanswered", .held = queryFrame(1, kRectQuery)},
        {.name = "loaded while Intl is unanswered", .bytes = bare(1, wire::Loaded), .reason = "kind 84 on runtime 1 while its Intl request is unanswered", .held = intlFrame(1, "{}")},
        {.name = "a death while a query is unanswered", .bytes = FrameBytes(1, wire::Died).str("x").bytes(), .reason = "kind 86 on runtime 1 while its query is unanswered", .held = queryFrame(1, kRectQuery)},
        {.name = "a probe answer never asked for", .bytes = probeAnswer(1, 1, "x"), .reason = "kind 87 on runtime 1 for probe 1, which was never sent"},
        {.name = "a probe answer never asked for while a query is unanswered", .bytes = probeAnswer(1, 1, "x"), .reason = "kind 87 on runtime 1 for probe 1, which was never sent",
         .held = queryFrame(1, kRectQuery)},
        {.name = "a probe answer past the probe asked for", .bytes = probeAnswer(1, 2, "x"), .reason = "kind 87 on runtime 1 for probe 2, which was never sent", .probing = true},
    };
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

/// Every engine input the host's end cannot accept ends the whole stream with its stated reason, read from the size alone when that is out of bounds (no body sent, answered within 1 s), and never resynchronises: a size under 5 or past the frame bound, an unknown kind or one of the host's, a kind on the wrong side of the connection, a field past its frame, bytes after the fields, a bool other than 0 or 1, a sequence below 1, a runtime never opened, a frame before Ready, a second Ready or a refusal after it, a refusal or another version as the answer, a second synchronous request or any other output of a runtime while its query or Intl request is in the host's hands, and a probe's answer for a sequence never sent. A closed runtime's well-formed frames are dropped while the stream goes on; a damaged one still ends it.
TEST(remote_stream_hostile_frames) {
    for (const Hostile& v : hostileInputs()) hostileRun(v);

    ScriptedEngine engine;
    auto sink = std::make_shared<StreamSink>();
    std::shared_ptr<SandboxRemoteChannel> channel = engine.host().channel();
    channel->attach(sink);
    StreamDelegate delegate;
    SandboxEngine client("<p>x</p>", testConfig(), engine.host().channel());
    client.setDelegate(&delegate);
    CHECK(engine.ready() && engine.send(logFrame(1, "control-1") + bare(2, wire::TaskStarted) + logFrame(2, "control-2")));
    CHECK(eventually([&] { return sink->saw("log:log:control-1") && delegate.saw("log:control-2") && engine.events().open() == 2; }));
    channel->close();
    CHECK(eventually([&] { return engine.saw(1, "close") && engine.events().open() == 1; }));
    CHECK(engine.send(logFrame(1, "late") + queryFrame(1, kRectQuery) + intlFrame(1, "{}") + probeAnswer(1, 1, "late") + bare(1, wire::Loaded) +
                      FrameBytes(1, wire::Died).str("late").bytes() + logFrame(2, "after the close")));
    CHECK_MSG(eventually([&] { return delegate.saw("log:after the close"); }), joined(delegate.events()));
    CHECK_MSG(!engine.events().ended() && sink->events() == std::vector<std::string>{"log:log:control-1"}, joined(engine.events().all()) + " / " + joined(sink->events()));
    CHECK(engine.send(FrameBytes(1, wire::Log).str("log").str("x").raw("x").bytes() + logFrame(2, "trailing-2")));
    CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }, 2), joined(engine.events().all()));
    CHECK_MSG(endsOnce(engine.events().all(), "kind 75 with bytes after its fields"), joined(engine.events().all()));
    CHECK(eventually([&] { return delegate.saw("died:" + kEngineGone); }));
    client.thread().sync([] {});
    CHECK_MSG((delegate.events() == std::vector<std::string>{"begin", "log:control-2", "log:after the close", "died:" + kEngineGone, "end"}), joined(delegate.events()));
    CHECK_MSG(sink->events() == std::vector<std::string>{"log:log:control-1"}, joined(sink->events()));
}

/// A renderer that announces itself and keeps what it is sent but answers no layout query, as a stuck one: the query waits in its WidgetHost.
struct SilentRenderer final : Renderer {
    void load() override { onMessage("{\"type\":\"ready\"}"); }
    void send(const std::string& json) override {
        std::lock_guard<std::mutex> l(mutex);
        messages.push_back(json);
    }
    void setTheme(const std::string&) override {}
    void setWidth(double) override {}
    void destroy() override {}
    /// The widget's layout queries it got, all unanswered (the host's own, with no qid of the widget's, aside).
    size_t queries() {
        std::lock_guard<std::mutex> l(mutex);
        return size_t(std::count_if(messages.begin(), messages.end(), [](const std::string& m) {
            const Json message = Json::parse(m).value_or(Json());
            return message["type"].asString() == "query" && message["qid"].asInt() > 0;
        }));
    }
    /// A trusted tap on the widget's first element.
    void tap() { onMessage("{\"type\":\"event\",\"ev\":" + kTapOnButton + ",\"trusted\":true}"); }

    std::mutex mutex;
    std::vector<std::string> messages;
};

/// A WidgetHost on `end`'s stream (the factory set only while it is built) whose renderer, kept in `renderer`, is a SilentRenderer; not started.
static std::unique_ptr<WidgetHost> silentHost(SandboxRemoteHostEnd& end, const char* html, SilentRenderer*& renderer) {
    FactoryReset reset;
    SandboxEngine::setRemoteChannelFactory([&end](std::shared_ptr<SandboxResources>) -> std::shared_ptr<SandboxRemoteChannel> { return end.channel(); });
    return std::make_unique<WidgetHost>(html, WidgetTheme{}, 300, WidgetHostOptions(), RES, [&renderer](std::shared_ptr<SandboxResources>, const std::string&, double) -> std::unique_ptr<Renderer> {
        auto r = std::make_unique<SilentRenderer>();
        renderer = r.get();
        return r;
    });
}

/// A widget that measures on a tap and logs the width once its query is answered.
static const char* kMeasureOnTap =
    "<button id='b'>go</button><p id='p'>x</p><script>document.getElementById('b').onclick = function () { console.log('measured ' + document.getElementById('p').getBoundingClientRect().width); };</script>";

/// One WidgetHost's layout query that its renderer never answers holds no other runtime on the stream: while it waits, and before its host's own 3 s timeout, another WidgetHost on the same stream loads, renders its layout answer and Intl and answers a probe; the stuck one destroyed while it waits joins within 5 s, and the other goes on.
TEST(remote_stream_stuck_query) {
    StreamPeers peers;
    SilentRenderer* silent = nullptr;
    std::unique_ptr<WidgetHost> stuck = silentHost(*peers.end, kMeasureOnTap, silent);
    stuck->start();
    CHECK(stuck->waitLoaded(20) && !stuck->diagnostics().dead);
    CHECK(eventually([&] { return peers.events.open() == 1; }));
    CHECK(silent->queries() == 0);
    silent->tap();
    CHECK(eventually([&] { return silent->queries() == 1; }));
    const auto since = std::chrono::steady_clock::now();
    {
        StreamHost other(*peers.end, kHostWidget, WidgetHostOptions(), 20, "INTL-B");
        CHECK(other.shown());
        CHECK_MSG(other.renders("h:20") && other.renders("n:INTL-B") && !other.renderer->sent("query").empty(), other.rendered().substr(0, 2000));
        CHECK(other.host->probe("d.textContent") == "h:20");
        CHECK(eventually([&] { return peers.events.open() == 2; }));
        const double ms = msSince(since);
        CHECK_MSG(ms < 3000, std::to_string(ms) + " ms after the stuck query");
        CHECK_MSG(silent->queries() == 1 && stuck->diagnostics().logs.empty(), std::to_string(stuck->diagnostics().logs.size()) + " logs");

        Background<bool> destroying([&] {
            stuck.reset();
            return true;
        });
        CHECK(destroying.finish(5));
        CHECK_MSG(eventually([&] { return peers.events.open() == 1; }), joined(peers.events.all()));
        CHECK(other.host->probe("d.textContent + ' ' + (1 + 1)") == "h:20 2");
        CHECK_MSG(!other.host->diagnostics().dead && !peers.events.ended(), joined(peers.events.all()));
    }
    CHECK(eventually([&] { return peers.events.open() == 0; }));
    CHECK(stopsWithin(*peers.end, 2));
    CHECK_MSG(endsOnce(peers.events.all(), "the host stopped the stream"), joined(peers.events.all()));
    CHECK(peers.serving.within(5).has_value());
}

/// A host's end and the engine process's end joined by two sides of a pair (a memory pair's or a socket pair's), the engine's end serving on a worker of the test's; the destructor stops the host's end, then joins the engine's.
struct JoinedEnds {
    EndEvents events;
    const std::shared_ptr<SandboxRemoteStream> hostSide;
    const std::shared_ptr<SandboxRemoteStream> engineSide;
    std::shared_ptr<SandboxRemoteHostEnd> end;
    Background<std::string> serving;

    JoinedEnds(std::shared_ptr<SandboxRemoteStream> host, std::shared_ptr<SandboxRemoteStream> engine)
        : hostSide(std::move(host)), engineSide(std::move(engine)), end(SandboxRemoteHostEnd::start(hostSide, *RES, kBuild, events.events(), {})),
          serving([this] { return SandboxRemoteEngineEnd::serve(*engineSide, kBuild); }) {}
    ~JoinedEnds() {
        end->stop();
        serving.finish(5);
    }
    JoinedEnds(const JoinedEnds&) = delete;
    JoinedEnds& operator=(const JoinedEnds&) = delete;
};

/// A real runtime on `end`'s stream that measures and formats at boot, loaded: a neighbor that has to go on, or hear the end once.
struct Neighbor {
    StreamDelegate delegate;
    SandboxEngine engine;
    explicit Neighbor(SandboxRemoteHostEnd& end) : engine(kMeasureIntl, testConfig(), end.channel(), std::make_shared<StreamIntl>()) {
        engine.setDelegate(&delegate);
        engine.start();
    }
    bool loaded() { return eventually([&] { return delegate.saw("log:w=77 INTL"); }, 20); }
    size_t deaths() {
        engine.thread().sync([] {});
        return delegate.eventsWith("died:").size();
    }
};

/// A bare sink's runtime on `end`'s stream whose real engine waits in its boot's layout query, held at the sink's gate.
struct HeldQuery {
    Gate gate;
    const std::shared_ptr<StreamSink> sink = std::make_shared<StreamSink>();
    std::shared_ptr<SandboxRemoteChannel> channel;
    explicit HeldQuery(SandboxRemoteHostEnd& end) : channel(end.channel()) {
        sink->gate = &gate;
        channel->attach(sink);
        channel->start(kMeasure, testConfig());
    }
    /// The engine booted as far as its query, which waits at the gate.
    bool waiting() {
        const auto asked = [&] {
            const std::vector<std::string> events = sink->events();
            return std::any_of(events.begin(), events.end(), [](const std::string& e) { return e.rfind("query:", 0) == 0; });
        };
        return eventually(asked, 20) && gate.waitedOn();
    }
    /// The sink's gate opened, then the channel destroyed (it joins its `widget.answer`) within 5 s.
    bool release() {
        gate.open();
        Background<bool> releasing([&] {
            channel.reset();
            return true;
        });
        return releasing.finish(5);
    }
};

/// Every wait ends when its runtime closes or the stream ends, within its bound: a WidgetHost destroyed while its real engine waits in a layout query its renderer never answers, and a bare channel closed while its engine waits in the host's query and its probe waits for that engine (ERROR no runtime within 1 s), leave the neighbor running and the stream open; the engine's side of the stream gone (a memory pair's end closed, a socket's shut down) while a runtime's engine waits in the host's query, and while its probe waits, kills each open runtime once (the probe answers after its client heard it), reports 0 and then the end, and serve returns once its runtimes are gone. Clients and channels then go (joins within 5 s after the test's own gates opened) and the host's end stops within 2 s.
TEST(remote_stream_close_and_end_waits) {
    {
        StreamPeers peers;
        Neighbor neighbor(*peers.end);
        CHECK(neighbor.loaded());
        SilentRenderer* silent = nullptr;
        std::unique_ptr<WidgetHost> host = silentHost(*peers.end, kMeasure, silent);
        host->start();
        CHECK(eventually([&] { return silent->queries() == 1 && peers.events.open() == 2; }, 20));
        Background<bool> destroying([&] {
            host->destroy();
            return true;
        });
        CHECK(destroying.finish(5));
        CHECK_MSG(eventually([&] { return peers.events.open() == 1; }), joined(peers.events.all()));
        CHECK(neighbor.engine.probe("w") == "77");
        Background<bool> releasing([&] {
            host.reset();
            return true;
        });
        CHECK(releasing.finish(5));
        CHECK_MSG(!peers.events.ended() && !peers.serving.within(0) && neighbor.deaths() == 0, joined(peers.events.all()));
        CHECK(stopsWithin(*peers.end, 2) && peers.serving.within(5).has_value());
        CHECK_MSG(endsOnce(peers.events.all(), "the host stopped the stream") && neighbor.deaths() == 1, joined(peers.events.all()));
    }
    {
        StreamPeers peers;
        Neighbor neighbor(*peers.end);
        CHECK(neighbor.loaded());
        HeldQuery held(*peers.end);
        CHECK(held.waiting());
        Background<std::string> probing([&] { return held.channel->probe("w"); });
        CHECK(!probing.within(0.3));
        const auto closed = std::chrono::steady_clock::now();
        held.channel->close();
        CHECK(probing.within(1) == std::optional<std::string>("ERROR no runtime"));
        CHECK_MSG(msSince(closed) < 1000, std::to_string(msSince(closed)) + " ms");
        CHECK_MSG(eventually([&] { return peers.events.open() == 1; }), joined(peers.events.all()));
        CHECK_MSG(!held.sink->saw("disconnected"), joined(held.sink->events()));
        CHECK(held.release());
        CHECK(neighbor.engine.probe("w") == "77");
        CHECK_MSG(!peers.events.ended() && !peers.serving.within(0) && neighbor.deaths() == 0, joined(peers.events.all()));
        CHECK(stopsWithin(*peers.end, 2) && peers.serving.within(5).has_value());
        CHECK_MSG(endsOnce(peers.events.all(), "the host stopped the stream") && neighbor.deaths() == 1, joined(peers.events.all()));
    }
    for (const bool socket : {false, true}) {
        for (const bool probe : {false, true}) {
            const std::string name = std::string(socket ? "socket" : "memory") + (probe ? ", probing: " : ": ");
            std::shared_ptr<SandboxRemoteStream> hostSide, engineSide;
            std::function<void()> vanish;
            if (socket) {
#ifndef _WIN32
                SocketStreamPair pair = socketStreamPair();
                hostSide = pair.host;
                engineSide = pair.engine;
                vanish = [side = pair.engine] { side->shutdown(); };
#endif
            } else {
                MemoryStreamPair pair = memoryStreamPair(size_t(1) << 20);
                hostSide = pair.host;
                engineSide = pair.engine;
                vanish = [side = pair.engine] { side->close(); };
            }
            if (!hostSide || !engineSide) continue;
            JoinedEnds ends(hostSide, engineSide);
            Neighbor neighbor(*ends.end);
            CHECK_MSG(neighbor.loaded(), name + joined(neighbor.delegate.events()));
            HeldQuery held(*ends.end);
            CHECK_MSG(held.waiting(), name + joined(held.sink->events()));
            CHECK(eventually([&] { return ends.events.open() == 2; }));
            Background<std::string> probing([&] {
                if (!probe) return std::string();
                const std::string answer = held.channel->probe("w");
                held.sink->record("probed:" + answer);
                return answer;
            });
            if (probe) CHECK(!probing.within(0.3));
            vanish();
            CHECK_MSG(eventually([&] { return ends.events.ended().has_value(); }, 2), name + joined(ends.events.all()));
            CHECK_MSG(endsOnce(ends.events.all(), "the engine process closed the stream"), name + joined(ends.events.all()));
            if (probe) CHECK_MSG(probing.within(1) == std::optional<std::string>("ERROR no runtime"), name + joined(held.sink->events()));
            const std::vector<std::string> heard = held.sink->events();
            const auto at = std::find(heard.begin(), heard.end(), "disconnected");
            CHECK_MSG(std::count(heard.begin(), heard.end(), "disconnected") == 1 && at != heard.end() && at != heard.begin() && (at - 1)->rfind("query:", 0) == 0 &&
                          (probe ? at + 1 != heard.end() && *(at + 1) == "probed:ERROR no runtime" && at + 2 == heard.end() : at + 1 == heard.end()),
                      name + joined(heard));
            CHECK_MSG(eventually([&] { return neighbor.delegate.saw("died:" + kEngineGone); }), name + joined(neighbor.delegate.events()));
            CHECK_MSG(neighbor.deaths() == 1, name + joined(neighbor.delegate.events()));
            CHECK_MSG(ends.serving.within(5) == std::optional<std::string>(""), name + ends.serving.within(0).value_or("still serving"));
            CHECK(held.release());
            CHECK(stopsWithin(*ends.end, 2));
            CHECK_MSG(endsOnce(ends.events.all(), "the engine process closed the stream"), name + joined(ends.events.all()));
        }
    }
}

/// What a transport sees of a client's sink: every call goes on to `inner`, a log after waiting at `gate` while one is set; `passed` counts the logs that went on.
class HeldSink final : public SandboxRemoteSink {
public:
    explicit HeldSink(std::shared_ptr<SandboxRemoteSink> inner) : inner_(std::move(inner)) {}

    std::atomic<Gate*> gate{nullptr};
    std::atomic<int> passed{0};

    void remoteTaskStarted() override { inner_->remoteTaskStarted(); }
    void remoteTaskCompleted() override { inner_->remoteTaskCompleted(); }
    void remoteOps(const std::string& json) override { inner_->remoteOps(json); }
    QueryAnswer remoteQuery(const std::string& json) override { return inner_->remoteQuery(json); }
    std::string remoteIntl(const std::string& requestJson) override { return inner_->remoteIntl(requestJson); }
    void remoteListen(const std::string& json) override { inner_->remoteListen(json); }
    void remotePreventDefault(const std::string& json) override { inner_->remotePreventDefault(json); }
    void remoteAnim(const std::string& json) override { inner_->remoteAnim(json); }
    void remoteRequestsFrame() override { inner_->remoteRequestsFrame(); }
    void remoteLog(const std::string& level, const std::string& text) override {
        if (Gate* g = gate) g->wait();
        passed++;
        inner_->remoteLog(level, text);
    }
    void remoteDiag(const std::string& kind, const std::string& detail) override { inner_->remoteDiag(kind, detail); }
    void remoteBridge(const std::string& method, const std::string& argsJson) override { inner_->remoteBridge(method, argsJson); }
    void remoteCopyText(const std::string& text) override { inner_->remoteCopyText(text); }
    void remoteRequest(int64_t id, const std::string& data, bool wallet) override { inner_->remoteRequest(id, data, wallet); }
    void remoteWallet(int64_t id, const std::string& payload) override { inner_->remoteWallet(id, payload); }
    void remoteStorage(const std::string& commitJson) override { inner_->remoteStorage(commitJson); }
    void remoteResizeAnchor(const std::string& edge) override { inner_->remoteResizeAnchor(edge); }
    void remoteWatchesPublicData(bool on) override { inner_->remoteWatchesPublicData(on); }
    void remoteLoaded() override { inner_->remoteLoaded(); }
    void remoteStats(const SandboxStats& stats) override { inner_->remoteStats(stats); }
    void remoteDied(const std::string& reason) override { inner_->remoteDied(reason); }
    void remoteDisconnected() override { inner_->remoteDisconnected(); }

private:
    const std::shared_ptr<SandboxRemoteSink> inner_;
};

/// The counts a host's end reported, in order.
static std::vector<size_t> countsOf(const std::vector<std::string>& events) {
    std::vector<size_t> out;
    for (const std::string& e : events)
        if (e.rfind("runtimes:", 0) == 0) out.push_back(size_t(std::stoul(e.substr(9))));
    return out;
}

/// `events` end as `endsOnce` wants with one of `reasons`.
static bool endsOnceWith(const std::vector<std::string>& events, const std::vector<std::string>& reasons) {
    return std::any_of(reasons.begin(), reasons.end(), [&](const std::string& reason) { return endsOnce(events, reason); });
}

/// Attaching, closing and the end of the stream race on one lock, and whichever comes first: each client attached by the end hears the loss exactly once, from the reader when it registered first and from its own attach when it came after (which then gets no id), no count passes those registered, and the owner hears the end once and last after a count of 0; a close racing the end leaves each client one loss at most; an answer racing its runtime's close goes out before that close or not at all; and a sink call the reader already made goes on into a client that is gone with its channel, changing nothing, while what follows for that runtime is dropped and the stream goes on.
TEST(remote_stream_races) {
    constexpr int kRounds = 8, kRacers = 6, kEarly = 2;
    const std::vector<std::string> engineEnded = {"the engine process closed the stream", "a write to the stream failed"};
    const auto ender = [&](ScriptedEngine& engine, int round) {
        if (round % 2) engine.close();
        else engine.host().stop();
    };
    const auto reasons = [&](int round) { return round % 2 ? engineEnded : std::vector<std::string>{"the host stopped the stream"}; };
    for (int round = 0; round < kRounds; round++) {
        const std::string name = "attach, round " + std::to_string(round) + ": ";
        ScriptedEngine engine;
        CHECK(engine.ready());
        std::vector<std::shared_ptr<StreamSink>> sinks;
        std::vector<std::shared_ptr<SandboxRemoteChannel>> channels;
        for (int i = 0; i < kEarly + kRacers; i++) {
            sinks.push_back(std::make_shared<StreamSink>());
            channels.push_back(engine.host().channel());
        }
        for (int i = 0; i < kEarly; i++) channels[i]->attach(sinks[i]);
        CHECK(eventually([&] { return engine.events().open() == size_t(kEarly); }));
        std::vector<std::thread::id> attachers(kEarly + kRacers);
        {
            Gate go;
            std::vector<std::unique_ptr<Background<bool>>> racers;
            for (int i = kEarly; i < kEarly + kRacers; i++) {
                racers.push_back(std::make_unique<Background<bool>>([&, i] {
                    go.wait();
                    attachers[i] = std::this_thread::get_id();
                    channels[i]->attach(sinks[i]);
                    return true;
                }));
            }
            racers.push_back(std::make_unique<Background<bool>>([&] {
                go.wait();
                ender(engine, round);
                return true;
            }));
            CHECK(go.waitedOn());
            go.open();
            for (const auto& racer : racers) CHECK(racer->finish(5));
        }
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }, 2), name + joined(engine.events().all()));
        size_t registered = 0;
        for (int i = 0; i < kEarly + kRacers; i++) {
            CHECK_MSG(sinks[i]->events() == std::vector<std::string>{"disconnected"}, name + std::to_string(i) + ": " + joined(sinks[i]->events()));
            if (i < kEarly || sinks[i]->threadOf("disconnected").first != attachers[i]) registered++;
        }
        const std::vector<size_t> counts = countsOf(engine.events().all());
        CHECK_MSG(registered >= size_t(kEarly) && std::all_of(counts.begin(), counts.end(), [&](size_t n) { return n <= registered; }), name + std::to_string(registered) + " registered, " + joined(engine.events().all()));
        CHECK_MSG(!engine.saw(uint32_t(registered + 1), "open"), name + std::to_string(registered) + " registered");
        CHECK_MSG(endsOnceWith(engine.events().all(), reasons(round)), name + joined(engine.events().all()));
    }
    for (int round = 0; round < kRounds; round++) {
        const std::string name = "close, round " + std::to_string(round) + ": ";
        ScriptedEngine engine;
        CHECK(engine.ready());
        std::vector<std::shared_ptr<StreamSink>> sinks;
        std::vector<std::shared_ptr<SandboxRemoteChannel>> channels;
        for (int i = 0; i < kRacers; i++) {
            sinks.push_back(std::make_shared<StreamSink>());
            channels.push_back(engine.host().channel());
            channels.back()->attach(sinks.back());
        }
        CHECK(eventually([&] { return engine.events().open() == size_t(kRacers); }));
        {
            Gate go;
            std::vector<std::unique_ptr<Background<bool>>> racers;
            for (int i = 0; i < kRacers; i++) {
                racers.push_back(std::make_unique<Background<bool>>([&, i] {
                    go.wait();
                    channels[i]->close();
                    return true;
                }));
            }
            racers.push_back(std::make_unique<Background<bool>>([&] {
                go.wait();
                ender(engine, round);
                return true;
            }));
            CHECK(go.waitedOn());
            go.open();
            for (const auto& racer : racers) CHECK(racer->finish(5));
        }
        CHECK_MSG(eventually([&] { return engine.events().ended().has_value(); }, 2), name + joined(engine.events().all()));
        for (int i = 0; i < kRacers; i++) {
            const std::vector<std::string> heard = sinks[i]->events();
            CHECK_MSG(heard.empty() || heard == std::vector<std::string>{"disconnected"}, name + std::to_string(i) + ": " + joined(heard));
        }
        CHECK_MSG(endsOnceWith(engine.events().all(), reasons(round)), name + joined(engine.events().all()));
    }
    {
        ScriptedEngine engine;
        CHECK(engine.ready());
        for (int round = 0; round < kRounds; round++) {
            const uint32_t id = uint32_t(round + 1);
            Gate held;
            auto sink = std::make_shared<StreamSink>();
            sink->gate = &held;
            std::shared_ptr<SandboxRemoteChannel> channel = engine.host().channel();
            channel->attach(sink);
            CHECK(eventually([&] { return engine.saw(id, "open"); }));
            CHECK(engine.send(queryFrame(id, kRectQuery)));
            CHECK(held.waitedOn());
            {
                Gate go;
                Background<bool> answering([&] {
                    go.wait();
                    held.open();
                    return true;
                });
                // Every other round the close waits for the sink's call to return, racing the answer's commit rather than the call.
                Background<bool> closing([&] {
                    go.wait();
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (round % 2 && !sink->saw("query-done") && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
                    channel->close();
                    return true;
                });
                CHECK(go.waitedOn());
                go.open();
                CHECK(answering.finish(5) && closing.finish(5));
            }
            channel.reset();
            CHECK(eventually([&] { return engine.saw(id, "close"); }));
            CHECK_MSG((sink->events() == std::vector<std::string>{"query:" + kRectQuery, "query-done"}), joined(sink->events()));
        }
        // A runtime opened last: once its Open came, everything queued before it came too.
        auto last = std::make_shared<StreamSink>();
        std::shared_ptr<SandboxRemoteChannel> lastChannel = engine.host().channel();
        lastChannel->attach(last);
        CHECK(eventually([&] { return engine.saw(uint32_t(kRounds + 1), "open"); }));
        for (uint32_t id = 1; id <= uint32_t(kRounds); id++) {
            const std::vector<std::string> frames = engine.frames(id);
            CHECK_MSG(frames == (std::vector<std::string>{"open", "query-answer:" + rectOf(77), "close"}) || frames == (std::vector<std::string>{"open", "close"}),
                      std::to_string(id) + ": " + joined(frames));
        }
        CHECK_MSG(!engine.events().ended() && last->events().empty(), joined(engine.events().all()));
    }
    {
        ScriptedEngine engine;
        CHECK(engine.ready());
        Gate gate;
        auto fake = std::make_shared<FakeRemoteChannel>();
        StreamDelegate delegate;
        auto client = std::make_unique<SandboxEngine>("<p>x</p>", testConfig(), fake);
        client->setDelegate(&delegate);
        auto held = std::make_shared<HeldSink>(fake->sink());
        std::shared_ptr<SandboxRemoteChannel> channel = engine.host().channel();
        channel->attach(held);
        auto other = std::make_shared<StreamSink>();
        std::shared_ptr<SandboxRemoteChannel> otherChannel = engine.host().channel();
        otherChannel->attach(other);
        CHECK(eventually([&] { return engine.saw(1, "open") && engine.saw(2, "open"); }));
        CHECK(engine.send(logFrame(1, "on time")));
        CHECK_MSG(eventually([&] { return delegate.saw("log:on time"); }), joined(delegate.events()));
        held->gate = &gate;
        CHECK(engine.send(logFrame(1, "late") + logFrame(1, "dropped") + logFrame(2, "behind")));
        CHECK(gate.waitedOn());
        client.reset();
        channel->close();
        channel.reset();
        gate.open();
        CHECK_MSG(eventually([&] { return other->saw("log:log:behind"); }), joined(other->events()));
        CHECK_MSG(held->passed == 2 && delegate.events() == std::vector<std::string>{"log:on time"}, std::to_string(held->passed) + " passed, " + joined(delegate.events()));
        CHECK_MSG(!engine.events().ended() && other->events() == std::vector<std::string>{"log:log:behind"}, joined(engine.events().all()));
    }
}

#ifndef _WIN32
/// A host's end on a socket pair of small buffers whose engine's side the test reads itself, until it reads no more.
struct StoppedReader {
    EndEvents events;
    SocketStreamPair pair;
    std::shared_ptr<SandboxRemoteHostEnd> end;
    std::optional<FrameStreamReader> engineSide;

    StoppedReader(SocketStreamPair sockets, const SandboxRemoteHostEnd::Limits& limits, const SandboxResources& resources = smallResources()) : pair(std::move(sockets)) {
        if (!pair.host || !pair.engine) return;
        end = SandboxRemoteHostEnd::start(pair.host, resources, kBuild, events.events(), limits);
        engineSide.emplace(*pair.engine);
    }
    ~StoppedReader() {
        if (end) end->stop();
    }
    StoppedReader(const StoppedReader&) = delete;
    StoppedReader& operator=(const StoppedReader&) = delete;

    /// The opening read and answered.
    bool ready() {
        if (!engineSide) return false;
        const std::optional<ParsedFrame> opening = engineSide->next();
        return opening && opening->kind == wire::Opening && pair.engine->write(FrameBytes(0, wire::Ready).i64(1).bytes());
    }
    /// The next frame the host's end wrote, as `describeHost` gives it.
    std::string next() {
        const std::optional<ParsedFrame> frame = engineSide ? engineSide->next() : std::nullopt;
        return frame ? describeHost(*frame) : "none";
    }
    /// The host's writer in the socket with a frame past what it holds, which this side does not read.
    bool blocked() { return eventually([&] { return pair.host->writeBlocked(); }, 2); }
    /// The host's writer out of the socket.
    bool woke() { return eventually([&] { return !pair.host->writing(); }, 2); }
};

/// A host's end whose engine's side stops reading, on a real socket pair whose writes block: the tests' socket pair itself blocks a write past its room, a shutdown of that end wakes the write and a read waiting on it, the peer reads what came before, then the end, and a write to a closed peer fails without a signal. After Ready, one event past what the socket holds leaves a write outstanding: with a 300 ms bound the end comes with no other call, its shutdown wakes the writer, and the client hears it once; an opening never read ends the same way; with the default bounds, channel calls, the client's dispatch barrier and a probe's first barrier return while the write stays blocked, and a stop wakes it and joins within 2 s; a blocked frame counts against the bounds of bytes and frames; and the engine's side closing its descriptor ends the stream, idle with the end of the stream, blocked by whichever of the reader and the writer sees it first.
TEST(remote_stream_stopped_reader) {
    const std::string lost = "died:" + kEngineGone;
    const std::string event(size_t(1) << 20, ' ');
    {
        SocketStreamPair pair = socketStreamPair(4096);
        CHECK(pair.host && pair.engine);
        if (!pair.host) return;
        Background<bool> writer([&] { return pair.host->write(event); });
        CHECK(eventually([&] { return pair.host->writeBlocked(); }, 2));
        Background<size_t> reader([&] {
            char b[16];
            return pair.host->read(b, sizeof(b));
        });
        CHECK(eventually([&] { return pair.host->readWaiting(); }, 2));
        pair.host->shutdown();
        CHECK(writer.within(2) == std::optional<bool>(false));
        CHECK(reader.within(2) == std::optional<size_t>(0));
        char b[64];
        CHECK(!pair.host->write("x") && pair.host->read(b, sizeof(b)) == 0);
        pair.host->shutdown();
        size_t got = 0;
        while (const size_t n = pair.engine->read(b, sizeof(b))) got += n;
        CHECK_MSG(got > 0 && got < event.size(), std::to_string(got) + " bytes before the end");
        SocketStreamPair closed = socketStreamPair();
        CHECK(closed.engine->write("abc"));
        closed.engine->close();
        CHECK(closed.host->read(b, sizeof(b)) == 3 && std::string(b, 3) == "abc" && closed.host->read(b, sizeof(b)) == 0);
        CHECK(!closed.host->write("after the peer closed"));
    }
    SandboxRemoteHostEnd::Limits quick;
    quick.writeStallMs = 300;
    const std::string stalled = "a write to the stream blocked for more than 300 ms";
    {
        StoppedReader r(socketStreamPair(4096), quick);
        CHECK(r.ready());
        StreamDelegate delegate;
        SandboxEngine client("<p>x</p>", testConfig(), r.end->channel());
        client.setDelegate(&delegate);
        client.start();
        CHECK(r.next() == "open" && r.next() == "start" && eventually([&] { return r.events.open() == 1; }));
        client.event(event);
        CHECK(r.blocked());
        const auto blocked = std::chrono::steady_clock::now();
        CHECK_MSG(eventually([&] { return r.events.ended().has_value(); }, 2), joined(r.events.all()));
        CHECK_MSG(msSince(blocked) < 2000, std::to_string(msSince(blocked)) + " ms");
        CHECK_MSG(r.events.all() == (std::vector<std::string>{"runtimes:1", "runtimes:0", "ended:" + stalled}), joined(r.events.all()));
        CHECK(r.woke());
        CHECK(eventually([&] { return delegate.saw(lost); }));
        CHECK(stopsWithin(*r.end, 2));
        client.thread().sync([] {});
        CHECK_MSG(delegate.eventsWith("died:").size() == 1, joined(delegate.events()));
    }
    {
        SandboxResources large = smallResources();
        large.sandboxBundle = std::string(size_t(1) << 20, 's');
        StoppedReader r(socketStreamPair(4096), quick, large);
        CHECK(r.end && r.blocked());
        CHECK_MSG(eventually([&] { return r.events.ended().has_value(); }, 2), joined(r.events.all()));
        CHECK_MSG(r.events.all() == std::vector<std::string>{"ended:" + stalled}, joined(r.events.all()));
        CHECK(r.woke() && stopsWithin(*r.end, 2));
    }
    {
        StoppedReader r(socketStreamPair(4096), {});
        CHECK(r.ready());
        StreamDelegate delegate;
        SandboxEngine client("<p>x</p>", testConfig(), r.end->channel());
        client.setDelegate(&delegate);
        client.start();
        CHECK(r.next() == "open" && r.next() == "start");
        client.event(event);
        CHECK(r.blocked());
        Background<bool> calls([&] {
            client.event("{}");
            client.setVisible(false);
            client.frame(1);
            client.thread().sync([] {});
            return true;
        });
        CHECK(calls.within(2) == std::optional<bool>(true));
        CHECK_MSG(r.pair.host->writeBlocked() && !r.events.ended(), joined(r.events.all()));
        Background<std::string> probing([&] { return client.probe("1"); });
        CHECK(!probing.within(0.3));
        CHECK_MSG(r.pair.host->writeBlocked() && !r.events.ended(), joined(r.events.all()));
        CHECK(stopsWithin(*r.end, 2));
        CHECK(r.woke());
        CHECK(probing.within(2) == std::optional<std::string>("ERROR no runtime"));
        CHECK_MSG(endsOnce(r.events.all(), "the host stopped the stream"), joined(r.events.all()));
        CHECK(eventually([&] { return delegate.saw(lost); }));
    }
    for (const bool bytes : {true, false}) {
        SandboxRemoteHostEnd::Limits bounded;
        bounded.queuedBytes = size_t(3) << 20;
        StoppedReader r(socketStreamPair(4096), bounded);
        CHECK(r.ready());
        auto sink = std::make_shared<StreamSink>();
        std::shared_ptr<SandboxRemoteChannel> channel = r.end->channel();
        channel->attach(sink);
        CHECK(r.next() == "open");
        channel->event(std::string(size_t(2) << 20, ' '));
        CHECK(r.blocked());
        if (bytes) {
            channel->event(std::string(size_t(900) << 10, ' '));
        } else {
            for (size_t i = 1; i < SandboxEngine::kRemoteQueuedCalls; i++) channel->event("{}");
        }
        CHECK_MSG(!r.events.ended() && sink->events().empty() && r.pair.host->writeBlocked(), joined(r.events.all()));
        channel->event(bytes ? std::string(size_t(200) << 10, ' ') : std::string("{}"));
        const std::string reason = bytes ? "more than 3145728 bytes queued for the stream" : "more than 16384 frames queued for the stream";
        CHECK_MSG(eventually([&] { return r.events.ended().has_value(); }, 2) && endsOnce(r.events.all(), reason), joined(r.events.all()));
        CHECK_MSG(sink->events() == std::vector<std::string>{"disconnected"}, joined(sink->events()));
        CHECK(r.woke() && stopsWithin(*r.end, 2));
    }
    for (const bool writing : {false, true}) {
        StoppedReader r(socketStreamPair(4096), {});
        CHECK(r.ready());
        StreamDelegate delegate;
        SandboxEngine client("<p>x</p>", testConfig(), r.end->channel());
        client.setDelegate(&delegate);
        client.start();
        CHECK(r.next() == "open" && r.next() == "start");
        if (writing) {
            client.event(event);
            CHECK(r.blocked());
        }
        r.pair.engine->close();
        CHECK_MSG(eventually([&] { return r.events.ended().has_value(); }, 2), joined(r.events.all()));
        const std::vector<std::string> reasons = writing ? std::vector<std::string>{"the engine process closed the stream", "a write to the stream failed"} : std::vector<std::string>{"the engine process closed the stream"};
        CHECK_MSG(endsOnceWith(r.events.all(), reasons), joined(r.events.all()));
        CHECK(r.woke() && eventually([&] { return delegate.saw(lost); }));
        CHECK(stopsWithin(*r.end, 2));
        client.thread().sync([] {});
        CHECK_MSG(delegate.eventsWith("died:").size() == 1, joined(delegate.events()));
    }
}

/// A widget that logs far more at once than a socket holds.
static const char* kFlood = "<script>var s = 'x'.repeat(3000); for (var i = 0; i < 400; i++) console.log(i + s);</script>";

/// The engine process's end whose runtime blocks in a write, its host reading no more, still returns when the host ends the stream: by a half close, which leaves that write blocked so only the end's own shutdown, before it takes the writer's lock, can wake it; or by closing its socket. Serve, and with it every runtime, probe and reaper thread it started, is done within 5 s.
TEST(remote_stream_engine_blocked_write_shutdown) {
    const std::string coded = SandboxRemoteCoding::encodeConfig(testConfig());
    for (const bool halfClose : {true, false}) {
        const std::string name = halfClose ? "half close: " : "close: ";
        SocketStreamPair pair = socketStreamPair(4096);
        CHECK(pair.host && pair.engine);
        if (!pair.host) continue;
        Background<std::string> serving([&] { return SandboxRemoteEngineEnd::serve(*pair.engine, kBuild); });
        CHECK(pair.host->write(openingOf(*RES, true)));
        FrameStreamReader hostSide(*pair.host);
        const std::optional<ParsedFrame> ready = hostSide.next();
        CHECK(ready && describe(*ready) == "ready:1");
        CHECK(pair.host->write(FrameBytes(1, wire::Open).bytes() + FrameBytes(1, wire::Start).str(kFlood).str(coded).bytes() + FrameBytes(1, wire::Probe).i64(1).str("i").bytes()));
        CHECK_MSG(eventually([&] { return pair.engine->writeBlocked(); }, 20), name + "the engine's write never blocked");
        const auto began = std::chrono::steady_clock::now();
        if (halfClose) pair.host->finishWriting();
        else pair.host->close();
        CHECK_MSG(serving.within(5) == std::optional<std::string>(""), name + serving.within(0).value_or("still serving"));
        CHECK_MSG(msSince(began) < 5000 && !pair.engine->writing(), name + std::to_string(msSince(began)) + " ms");
        pair.host->shutdown();
    }
}

/// A socket end's read that starts to wait just as its end comes returns 0 all the same: in 2000 rounds, each on a fresh pair, a reader thread starts a read of the engine's end and, 0 to 1000 ns later (the same series every run), the host's end shuts down (odd rounds) or the engine's end itself (even rounds), as a host end's stop ends the engine process's read and wakes its own reader. A blocking `recv` on macOS slept through such an end now and then; a round still reading after 2 s stops the run.
TEST(remote_stream_socket_read_sees_an_end_that_comes_as_it_starts_to_wait) {
    constexpr int kRounds = 2000;
    constexpr uint64_t kLongestDelayNs = 1000;
    std::atomic<SocketStream*> reading{nullptr};
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    std::atomic<size_t> got{0};
    std::atomic<bool> over{false};
    Background<bool> reader([&] {
        char b[64];
        for (int round = 1;; round++) {
            while (started < round && !over) {}
            if (over) return true;
            got = reading.load()->read(b, sizeof(b));
            finished = round;
        }
    });
    uint64_t seed = 88172645463325252ull;
    for (int round = 1; round <= kRounds; round++) {
        SocketStreamPair pair = socketStreamPair();
        CHECK(pair.host && pair.engine);
        if (!pair.host) break;
        const bool peer = round % 2;
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        const auto delay = std::chrono::nanoseconds(seed % (kLongestDelayNs + 1));
        reading = pair.engine.get();
        started = round;
        const auto until = std::chrono::steady_clock::now() + delay;
        while (std::chrono::steady_clock::now() < until) {}
        if (peer) pair.host->shutdown();
        else pair.engine->shutdown();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (finished != round && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        const std::string name = "round " + std::to_string(round) + " (" + (peer ? "the peer's" : "its own") + " shutdown " + std::to_string(delay.count()) + " ns after the reader's start): ";
        const bool ended = finished == round;
        CHECK_MSG(ended, name + "the read still waits 2 s later");
        if (!ended) stopTheRun(2, std::source_location::current());
        CHECK_MSG(got.load() == 0, name + std::to_string(got.load()) + " bytes read");
    }
    over = true;
    CHECK(reader.finish(5));
}
#endif

/// A widget whose media query is past the message limit (the shim sends it uncut) next to one within it.
static const char* kLongMediaQuery = "<script>var big = matchMedia('(min-width: ' + 'x'.repeat(1100000) + 'px)').matches, small = matchMedia('(min-width: 1px)').matches;</script>";

static const std::string kNumberIntl = "{\"kind\":\"number\",\"locale\":\"en-US\",\"opts\":{},\"value\":1234.5}";

/// A payload past its limit gets over the stream exactly what it gets over the loopback, and kills nothing: a real engine's media query past the message limit, which the shim does not cut, reaches no delegate and answers as an invalid query while the widget and its neighbor go on; engine frames at a limit, one byte past it and far past it (all within the frame bound, the last cut by the host's end) get the client's own verdicts, as over a fake channel: delivered, a protocol violation, an invalid query, an Intl answer of "", a cut text, an oversized request refused NOT_ALLOWED; and the stream never ends.
TEST(remote_stream_payload_limit) {
    {
        const auto run = [](std::shared_ptr<SandboxRemoteChannel> channel) {
            RemoteRecorder recorder;
            recorder.queryAnswer = "{\"matches\":true}";
            SandboxEngine engine(kLongMediaQuery, testConfig(), std::move(channel));
            engine.setDelegate(&recorder);
            engine.start();
            CHECK(eventually([&] { return has(recorder.events(), "loaded"); }, 20));
            const std::string seen = engine.probe("big + ' ' + small");
            engine.thread().sync([] {});
            std::vector<std::string> out = recorder.eventsWith("query:");
            out.insert(out.begin(), seen);
            for (const auto& [kind, detail] : recorder.diags())
                if (kind != "boot") out.push_back(kind + ":" + detail);
            for (const std::string& death : recorder.deaths()) out.push_back("died:" + death);
            return out;
        };
        StreamPeers peers;
        Neighbor neighbor(*peers.end);
        CHECK(neighbor.loaded());
        const std::vector<std::string> loopback = run(std::make_shared<SandboxLoopbackChannel>(RES));
        const std::vector<std::string> stream = run(peers.end->channel());
        CHECK_MSG((loopback == std::vector<std::string>{"false true", "query:{\"q\":\"matchMedia\",\"query\":\"(min-width: 1px)\"}"}), joined(loopback));
        CHECK_MSG(stream == loopback, joined(stream) + " for the loopback's " + joined(loopback));
        CHECK(neighbor.engine.probe("w") == "77");
        CHECK_MSG(!peers.events.ended() && neighbor.deaths() == 0, joined(peers.events.all()));
    }
    {
        ScriptedEngine engine;
        auto intl = std::make_shared<RecordingIntl>();
        RemoteRecorder recorder;
        intl->into = &recorder;
        SandboxEngine client("<p>x</p>", testConfig(), engine.host().channel(), intl);
        client.setDelegate(&recorder);
        auto neighbor = std::make_shared<StreamSink>();
        std::shared_ptr<SandboxRemoteChannel> next = engine.host().channel();
        next->attach(neighbor);
        CHECK(engine.ready() && eventually([&] { return engine.saw(1, "open") && engine.saw(2, "open"); }));
        const size_t limit = SandboxEngine::kRemoteMessageLimit, ops = SandboxEngine::kRemoteOpsLimit, far = 4096;
        const auto one = [](uint8_t kind, const std::string& text) { return FrameBytes(1, kind).str(text).bytes(); };
        for (const size_t size : {ops, ops + 1, ops + far}) CHECK(engine.send(one(wire::Ops, paddedTo("[]", size))));
        for (const uint8_t kind : {uint8_t(wire::Listen), uint8_t(wire::PreventDefault), uint8_t(wire::Anim)})
            for (const size_t size : {limit, limit + 1, limit + far}) CHECK(engine.send(one(kind, paddedTo("{}", size))));
        const std::string kind(SandboxEngine::kRemoteNameLimit, 'k'), detail(SandboxEngine::kRemoteTextLimit, 'd');
        CHECK(engine.send(FrameBytes(1, wire::Log).str("log").str(std::string(SandboxEngine::kRemoteTextLimit + 1, 'l')).bytes() + FrameBytes(1, wire::Log).str("trace").str("t").bytes() +
                          FrameBytes(1, wire::Log).str("log").str(std::string(limit + far, 'l')).bytes() + FrameBytes(1, wire::Diag).str(kind).str(detail).bytes() +
                          FrameBytes(1, wire::Diag).str(kind + std::string(limit, 'k')).str(detail + std::string(limit, 'd')).bytes() +
                          FrameBytes(1, wire::Request).i64(7).str(std::string(size_t(sdk::MAX_REQUEST_DATA_BYTES) + 1, 'r')).boolean(0).bytes()));
        const auto answers = [&] {
            std::vector<std::string> out;
            for (const std::string& f : engine.frames(1))
                if (f.rfind("query-answer:", 0) == 0 || f.rfind("intl-answer:", 0) == 0) out.push_back(f);
            return out;
        };
        const auto ask = [&](const std::string& frame) {
            const size_t before = answers().size();
            std::string got = "no answer";
            if (!engine.send(frame)) return got;
            eventually([&] {
                const std::vector<std::string> all = answers();
                if (all.size() > before) got = all[before];
                return all.size() > before;
            });
            return got;
        };
        const std::string query = paddedTo("{\"q\":1}", limit), invalid = "query-answer:{\"error\":\"invalid query\"}";
        CHECK(ask(queryFrame(1, query)) == "query-answer:{\"answer\":1}");
        CHECK(ask(queryFrame(1, query + " ")) == invalid);
        CHECK(ask(queryFrame(1, query + std::string(far, ' '))) == invalid);
        CHECK(ask(intlFrame(1, paddedTo(kNumberIntl, limit))) == "intl-answer:INTL");
        CHECK(ask(intlFrame(1, paddedTo(kNumberIntl, limit + 1))) == "intl-answer:");
        CHECK(ask(intlFrame(1, paddedTo(kNumberIntl, limit + far))) == "intl-answer:");
        intl->answer = std::string(limit, 'n');
        CHECK(ask(intlFrame(1, kNumberIntl)) == "intl-answer:" + std::string(limit, 'n'));
        intl->answer = std::string(limit + 1, 'n');
        CHECK(ask(intlFrame(1, kNumberIntl)) == "intl-answer:");
        const std::string refused = "answer:7:" + SandboxRemoteCoding::encodeOutcome(SandboxRemoteOutcome::failed(WidgetRequestFailure::NotAllowed));
        CHECK_MSG(eventually([&] { return engine.saw(1, refused); }), joined(engine.frames(1)));
        CHECK(engine.send(logFrame(2, "neighbor")));
        CHECK(eventually([&] { return neighbor->saw("log:log:neighbor"); }));
        client.thread().sync([] {});
        const std::string asked = "intl:" + Json::parse(kNumberIntl)->dump(), violation = "diag:protocol-violation";
        const std::vector<std::string> expected = {"ops", violation, violation, "listen", violation, violation, "preventDefault", violation, violation, "anim", violation, violation,
                                                   "log:log:4000", "log:log:1", "log:log:4000", "diag:" + kind, "diag:" + kind, "query:" + query, "query-done", asked, asked, asked};
        CHECK_MSG(recorder.events() == expected, firstDifference(recorder.events(), expected));
        const std::pair<std::string, std::string> ops32 = {"protocol-violation", "an ops batch over 32 MB"};
        const std::vector<std::pair<std::string, std::string>> diags = {ops32, ops32, {"protocol-violation", "listen over 1 MB"}, {"protocol-violation", "listen over 1 MB"},
                                                                        {"protocol-violation", "preventDefault over 1 MB"}, {"protocol-violation", "preventDefault over 1 MB"},
                                                                        {"protocol-violation", "anim over 1 MB"}, {"protocol-violation", "anim over 1 MB"}, {kind, detail}, {kind, detail}};
        CHECK(recorder.diags() == diags);
        CHECK_MSG(!engine.events().ended() && recorder.deaths().empty() && neighbor->events() == std::vector<std::string>{"log:log:neighbor"}, joined(engine.events().all()));
    }
}

// ───────────── a host process whose engines are all remote ─────────────

#ifndef _WIN32
/// The child of `remote_stream_thread_process`, a host whose engines are all remote, on the stream it inherited: three WidgetHosts load and render with their Intl formatted by their providers here, on `widget.client`; this process then has no `widget.sandbox` thread, three `widget.client` and the transport's own, and one engine of its own adds exactly one `widget.sandbox`. It prints its process id and both inventories of names.
CHILD_CASE(remote_stream_host_process) {
    EndEvents events;
    const std::shared_ptr<SandboxRemoteHostEnd> end = SandboxRemoteHostEnd::start(std::make_shared<SocketStream>(kChildStreamFd), *RES, kBuild, events.events(), {});
    printf("    child %ld\n", processId());
    {
        std::vector<std::unique_ptr<StreamHost>> hosts;
        for (const char* letter : {"A", "B", "C"}) hosts.push_back(std::make_unique<StreamHost>(*end, kHostWidget, WidgetHostOptions(), 20, std::string("INTL-") + letter));
        for (const std::unique_ptr<StreamHost>& h : hosts) {
            const std::string answer = h->intl->answer;
            CHECK_MSG(h->shown() && h->renders("h:20") && h->renders("n:" + answer), answer + ": " + h->rendered().substr(0, 500));
            CHECK_MSG(h->intl->processes() == std::vector<long>{processId()}, answer + ": " + std::to_string(h->intl->processes().size()) + " requests");
#if defined(__APPLE__) || defined(__linux__)
            CHECK_MSG(h->intl->threads() == std::vector<std::string>{"widget.client"}, answer + ": " + joined(h->intl->threads()));
#endif
        }
        CHECK_MSG(eventually([&] { return events.open() == 3; }), joined(events.all()));
#if defined(__APPLE__) || defined(__linux__)
        const std::optional<std::map<std::string, int>> remote = threadNames();
        CHECK_MSG(remote.has_value(), "this process's threads could not be listed");
        if (remote) {
            printf("    remote only: %s\n", namesText(*remote).c_str());
            CHECK_MSG(countOf(*remote, "widget.sandbox") == 0 && countOf(*remote, "widget.client") == 3, namesText(*remote));
            CHECK_MSG(countOf(*remote, "widget.reader") == 1 && countOf(*remote, "widget.writer") == 1 && countOf(*remote, "widget.monitor") == 1 && countOf(*remote, "widget.answer") == 3,
                      namesText(*remote));
        }
        {
            StreamDelegate delegate;
            SandboxEngine local("<p>x</p>", testConfig(), RES);
            local.setDelegate(&delegate);
            local.start();
            CHECK(eventually([&] { return delegate.saw("loaded"); }, 20));
            const std::optional<std::map<std::string, int>> mixed = threadNames();
            CHECK_MSG(mixed.has_value(), "this process's threads could not be listed");
            if (mixed) {
                printf("    with a local engine: %s\n", namesText(*mixed).c_str());
                CHECK_MSG(countOf(*mixed, "widget.sandbox") == 1 && countOf(*mixed, "widget.client") == 3, namesText(*mixed));
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

/// A host process whose engines are all remote has no `widget.sandbox` thread: a fresh child of the suite's (`remote_stream_host_process`) gets one end of a socket pair as its descriptor 3 while this process serves the engine process's end on the other, where the child's three WidgetHosts' engines run; the child's checks pass (its Intl in its own providers, its names), it exits 0 within 60 s with its process id and name inventories, and serve returns within 5 s of its exit.
TEST(remote_stream_thread_process) {
    SocketStreamPair pair = socketStreamPair();
    CHECK(pair.host && pair.engine);
    if (!pair.host || !pair.engine) return;
    Background<std::string> serving([&] { return SandboxRemoteEngineEnd::serve(*pair.engine, kBuild); });
    const ChildRun child = runChildOn("remote_stream_host_process", *pair.host, 60);
    CHECK_MSG(serving.within(5).has_value(), "serve still runs 5 s after the child ended");
    std::string lines;
    for (size_t at = 0; at < child.output.size();) {
        const size_t end = std::min(child.output.find('\n', at), child.output.size());
        const std::string line = child.output.substr(at, end - at);
        if (line.rfind("    ", 0) == 0) lines += line + "\n";
        at = end + 1;
    }
    printf("%s", lines.c_str());
    CHECK_MSG(child.status == 0, std::to_string(child.status) + ": " + child.output);
    const size_t named = lines.find("    child ");
    const long childId = named == std::string::npos ? 0 : std::atol(lines.c_str() + named + 10);
    CHECK_MSG(childId > 0 && childId != processId(), lines);
#if defined(__APPLE__) || defined(__linux__)
    CHECK_MSG(lines.find("    remote only: ") != std::string::npos && lines.find("    with a local engine: ") != std::string::npos, lines);
#endif
}
#endif

// ───────────── threads the OS refuses to start (macOS: the per-process thread limit) ─────────────

#ifdef __APPLE__
/// The host's end on `stream` for engines that boot nothing (`smallResources`), opened by `startHosting` with `hosting`, else by `start`.
static std::shared_ptr<SandboxRemoteHostEnd> openedEnd(bool hosting, std::shared_ptr<MemoryStream> stream, EndEvents& events) {
    return hosting ? SandboxRemoteHostEnd::startHosting(std::move(stream), smallResources(), kBuild, events.events(), {})
                   : SandboxRemoteHostEnd::start(std::move(stream), smallResources(), kBuild, events.events(), {});
}

/// The child of `remote_stream_host_end_whose_threads_cannot_start_fails_promptly`, alone in a fresh process: `twidget-tests --child remote_stream_host_end_at_the_thread_limit`.
CHILD_CASE(remote_stream_host_end_at_the_thread_limit) {
    const std::optional<int> alone = taskThreads();
    CHECK_MSG(alone.has_value(), "the task's threads cannot be listed");
    if (!alone) return;
    {
        ThreadAllowance allowance;
        const auto start = std::chrono::steady_clock::now();
        const std::string filled = allowance.fill();
        CHECK_MSG(filled.empty(), filled);
        if (!filled.empty()) return;
        printf("    full: %d threads, %zu of them parked, in %.0f ms\n", allowance.full(), allowance.parked(), msSince(start));
        fflush(stdout);
        CHECK_MSG(allowance.parked() >= 3, std::to_string(allowance.parked()) + " threads parked");
        if (allowance.parked() < 3) return;
        const char* threads[] = {"widget.reader", "widget.writer", "widget.monitor"};
        for (const bool hosting : {false, true}) {
            const std::string opener = hosting ? "startHosting" : "start";
            for (int k = 0; k < 3; k++) {
                EndEvents events;
                const MemoryStreamPair pair = memoryStreamPair(size_t(1) << 20);
                std::shared_ptr<SandboxRemoteHostEnd> end;
                const bool refused = expectNoStart(allowance, opener, k, threads[k], [&] { end = openedEnd(hosting, pair.host, events); });
                // Closed, not shut down: what the end queued stays readable, and none of it may have been written.
                pair.host->close();
                std::string written;
                char buffer[4096];
                while (const size_t n = pair.engine->read(buffer, sizeof(buffer))) written.append(buffer, n);
                printf("    %s, %d free: %zu bytes on the stream, %zu events\n", opener.c_str(), k, written.size(), events.all().size());
                fflush(stdout);
                CHECK_MSG(!end && written.empty() && events.all().empty(), opener + ", " + std::to_string(k) + " free: " + std::to_string(written.size()) + " bytes, events " + joined(events.all()));
                if (!refused) return;
            }
        }
        // The control: with three free each opener starts its three threads, writes its opening and stops as usual.
        const std::string left = allowance.leave(3);
        CHECK_MSG(left.empty(), left);
        if (!left.empty()) return;
        const int before = allowance.full() - 3;
        for (const bool hosting : {false, true}) {
            const std::string opener = hosting ? "startHosting" : "start";
            EndEvents events;
            const MemoryStreamPair pair = memoryStreamPair(size_t(1) << 20);
            std::string failure;
            std::optional<int> during;
            std::string first = "none";
            try {
                const std::shared_ptr<SandboxRemoteHostEnd> end = openedEnd(hosting, pair.host, events);
                during = taskThreads();
                FrameStreamReader engineSide(*pair.engine);
                if (const std::optional<ParsedFrame> frame = engineSide.next()) first = describeHost(*frame);
                end->stop();
            } catch (const std::exception& e) {
                failure = e.what();
            }
            const std::optional<int> after = threadsSettleAt(before);
            printf("    %s, 3 free: %s, first frame %s, events %s; %d threads before, %s while it lived, %s after\n", opener.c_str(),
                   failure.empty() ? "started" : ("\"" + failure + "\"").c_str(), first.c_str(), joined(events.all()).c_str(), before, countText(during).c_str(), countText(after).c_str());
            fflush(stdout);
            CHECK_MSG(failure.empty(), opener + ": " + failure);
            CHECK_MSG(during == before + 3, opener + ": " + std::to_string(before) + " threads before, " + countText(during) + " while it lived");
            CHECK_MSG(first == (hosting ? "hosting-opening" : "opening"), opener + ": " + first);
            CHECK_MSG(endsOnce(events.all(), "the host stopped the stream"), opener + ": " + joined(events.all()));
            CHECK_MSG(after == before, opener + ": " + std::to_string(before) + " threads before, " + countText(after) + " after");
        }
    }
    const std::optional<int> after = threadsSettleAt(*alone);
    printf("    every parked thread ended: %s threads, %d before the fill\n", countText(after).c_str(), *alone);
    fflush(stdout);
    CHECK_MSG(after == alone, countText(after) + " threads, " + std::to_string(*alone) + " before the fill");
}

/// Runtime A's first synchronous request on a scripted engine's stream, a query (or with `hosting` a hosting stream's platform request), while no thread can start: B's `widget.answer`, A's waiting probe and every worker are made before the fill. A's client hears the refusal as its death, then the loss, its probe answers "ERROR no runtime" within 2 s, its `Close` goes out once and no answer of its; B answers its next request and an Intl request, no named thread came or went, and the stream goes on.
static void answerUnstartedOnScriptedStream(bool hosting) {
    const std::string leg = hosting ? "hosting stream, a platform request" : "engine stream, a query";
    SandboxRemoteHostEnd::Limits limits;
    // A's probe waits through the fill.
    limits.probeWaitMs = 60000;
    ScriptedEngine engine(limits, hosting);
    auto a = std::make_shared<StreamSink>(), b = std::make_shared<StreamSink>();
    const std::shared_ptr<SandboxRemoteChannel> channelA = hosting ? engine.host().hostedChannel() : engine.host().channel();
    const std::shared_ptr<SandboxRemoteChannel> channelB = hosting ? engine.host().hostedChannel() : engine.host().channel();
    channelA->attach(a);
    channelB->attach(b);
    CHECK_MSG(engine.ready() && eventually([&] { return engine.saw(1, "open") && engine.saw(2, "open") && engine.events().open() == 2; }), leg + ": " + joined(engine.events().all()));
    const auto request = [&](uint32_t runtime) { return hosting ? FrameBytes(runtime, wire::PlatformRequest).str("asks").bytes() : queryFrame(runtime, kRectQuery); };
    const std::string answer = hosting ? "platform-answer:ANSWER" : "query-answer:" + rectOf(77);
    const auto answers = [&](uint32_t runtime, const std::string& frame) {
        const std::vector<std::string> frames = engine.frames(runtime);
        return std::count(frames.begin(), frames.end(), frame);
    };
    CHECK(engine.send(request(2)));
    CHECK_MSG(eventually([&] { return answers(2, answer) == 1; }), leg + ": " + joined(engine.frames(2)));
    Background<std::string> probing([&] {
        const std::string answered = channelA->probe("held");
        a->record("probed:" + answered);
        return answered;
    });
    CHECK_MSG(eventually([&] { return engine.saw(1, "probe:1:held"); }), leg + ": " + joined(engine.frames(1)));
    const std::map<std::string, int> before = threadNames().value_or(std::map<std::string, int>());
    CHECK_MSG(countOf(before, "widget.answer") == 1, leg + ": " + namesText(before));
    {
        ThreadAllowance allowance;
        if (!filledFor(allowance, leg, before).empty()) {
            channelA->close();
            return;
        }
        printf("    %s: A's first request, whose widget.answer cannot start\n", leg.c_str());
        fflush(stdout);
        const auto sent = std::chrono::steady_clock::now();
        CHECK(engine.send(request(1)));
        const std::optional<std::string> probed = probing.within(2);
        const double ms = msSince(sent);
        const std::vector<std::string> heard = a->events();
        printf("    %s: A heard %s in %.1f ms\n", leg.c_str(), whole(heard).c_str(), ms);
        fflush(stdout);
        CHECK_MSG(probed == std::optional<std::string>("ERROR no runtime"), leg + ": the probe " + probed.value_or("still waits"));
        CHECK_MSG(heard.size() == 3 && heard[0].rfind("died:", 0) == 0 && unstarted(heard[0].substr(5), kAnswerUnstarted, "widget.answer") && heard[1] == "disconnected" && heard[2] == "probed:ERROR no runtime",
                  leg + ": " + whole(heard));
        CHECK_MSG(eventually([&] { return engine.saw(1, "close"); }, 2), leg + ": " + joined(engine.frames(1)));
        CHECK(engine.send(request(2)));
        CHECK_MSG(eventually([&] { return answers(2, answer) == 2; }, 2), leg + ": " + joined(engine.frames(2)));
        CHECK(engine.send(intlFrame(2, "{}")));
        CHECK_MSG(eventually([&] { return engine.saw(2, "intl-answer:INTL"); }, 2), leg + ": " + joined(engine.frames(2)));
        // A's Close was queued before B's answers.
        CHECK_MSG((engine.frames(1) == std::vector<std::string>{"open", "probe:1:held", "close"}), leg + ": " + joined(engine.frames(1)));
        const std::vector<std::string> done = hosting ? std::vector<std::string>{"platform-request:asks", "platform-request-done", "platform-request:asks", "platform-request-done", "intl:{}", "intl-done"}
                                                      : std::vector<std::string>{"query:" + kRectQuery, "query-done", "query:" + kRectQuery, "query-done", "intl:{}", "intl-done"};
        CHECK_MSG(b->events() == done && a->events() == heard, leg + ": B " + joined(b->events()) + " / A " + whole(a->events()));
        CHECK_MSG(eventually([&] { return engine.events().open() == 1; }) && !engine.events().ended(), leg + ": " + joined(engine.events().all()));
        const std::map<std::string, int> after = threadNames().value_or(std::map<std::string, int>());
        printf("    %s: A's frames %s; B answered, the stream goes on; after it %s\n", leg.c_str(), joined(engine.frames(1)).c_str(), namesText(after).c_str());
        fflush(stdout);
        CHECK_MSG(namesText(after) == namesText(before), leg + ": " + namesText(before) + " before, " + namesText(after) + " after");
    }
}

/// Runtime A's first layout query over both real ends, asked by its WidgetHost's probe while no thread can start: the engine's end's reaper, A's and B's `widget.probe`, B's `widget.answer` (its boot asked) and every worker are made before the fill, A's boot asked nothing. The probe answers "ERROR no runtime" within 2 s, A dies with the refusal, its `Close` reaches the engine's end once with no answer before it and its engine and probe threads are reaped; B's layout query and Intl request are still answered, and the stream goes on.
static void answerUnstartedOverRealEnds() {
    const std::string leg = "both ends, a WidgetHost's query";
    StreamPeers peers;
    {
        auto sink = std::make_shared<StreamSink>();
        const std::shared_ptr<SandboxRemoteChannel> channel = peers.end->channel();
        channel->attach(sink);
        channel->close();
    }
    const auto named = [](const std::string& name) {
        const std::optional<std::map<std::string, int>> names = threadNames();
        return names ? countOf(*names, name) : -1;
    };
    CHECK_MSG(eventually([&] { return named("widget.reaper") == 1; }), leg + ": no reaper after runtime 1 closed");
    auto a = std::make_unique<StreamHost>(*peers.end, "<p id='p'>a</p>", WidgetHostOptions(), 20, "INTL-A");
    CHECK_MSG(a->shown() && named("widget.answer") == 0, leg + ": A asked at its boot");
    StreamHost b(*peers.end, kHostWidget, WidgetHostOptions(), 20, "INTL-B");
    CHECK_MSG(b.shown() && b.renders("h:20") && b.renders("n:INTL-B"), b.rendered().substr(0, 500));
    CHECK(a->host->probe("1 + 1") == "2" && b.host->probe("d.textContent") == "h:20");
    const std::map<std::string, int> before = threadNames().value_or(std::map<std::string, int>());
    CHECK_MSG(countOf(before, "widget.answer") == 1 && countOf(before, "widget.probe") == 2 && countOf(before, "widget.sandbox") == 2 && countOf(before, "widget.reaper") == 1,
              leg + ": " + namesText(before));
    CountingStream& counted = *peers.counted;
    const auto dead = [](const WidgetDiagnostics& d) { return d.dead.has_value(); };
    {
        ThreadAllowance allowance;
        if (!filledFor(allowance, leg, before).empty()) return;
        printf("    %s: A's first request, whose widget.answer cannot start\n", leg.c_str());
        fflush(stdout);
        const auto asked = std::chrono::steady_clock::now();
        const std::string got = a->host->probe("document.getElementById('p').getBoundingClientRect().width");
        const double ms = msSince(asked);
        const bool died = a->host->waitUntil(dead, 2);
        const std::string reason = a->host->diagnostics().dead.value_or("alive");
        printf("    %s: A's probe answered \"%s\" in %.1f ms; A %s\n", leg.c_str(), got.c_str(), ms, died ? ("died: " + reason).c_str() : "lives");
        fflush(stdout);
        CHECK_MSG(got == "ERROR no runtime" && ms < 2000, leg + ": " + got + " in " + std::to_string(ms) + " ms");
        CHECK_MSG(died && unstarted(reason, kAnswerUnstarted, "widget.answer"), leg + ": " + reason);
        CHECK_MSG(eventually([&] { return counted.readOf(wire::Close, 2) == 1; }, 2), leg + ": " + std::to_string(counted.readOf(wire::Close, 2)) + " closes");
        std::map<std::string, int> after;
        CHECK_MSG(eventually([&] {
            after = threadNames().value_or(std::map<std::string, int>());
            return countOf(after, "widget.sandbox") == 1 && countOf(after, "widget.probe") == 1;
        }, 5), leg + ": " + namesText(after));
        CHECK_MSG(countOf(after, "widget.answer") == 1 && countOf(after, "widget.reaper") == 1 && countOf(after, "widget.client") == countOf(before, "widget.client"), leg + ": " + namesText(after));
        const size_t queries = counted.readOf(wire::QueryAnswer, 3), intl = counted.readOf(wire::IntlAnswer, 3);
        const std::string again = b.host->probe("(d.textContent = 'again', d.offsetHeight + ' ' + (4321.5).toLocaleString())");
        printf("    %s: the engine's end read %zu Close and %zu query answers of A's; B's probe answered \"%s\" (%zu query and %zu Intl answers before it, %zu and %zu after); after A %s\n", leg.c_str(),
               counted.readOf(wire::Close, 2), counted.readOf(wire::QueryAnswer, 2), again.c_str(), queries, intl, counted.readOf(wire::QueryAnswer, 3), counted.readOf(wire::IntlAnswer, 3),
               namesText(after).c_str());
        fflush(stdout);
        CHECK_MSG(again == "20 INTL-B" && counted.readOf(wire::QueryAnswer, 3) > queries && counted.readOf(wire::IntlAnswer, 3) > intl, leg + ": " + again);
        CHECK_MSG(counted.readOf(wire::QueryAnswer, 2) == 0, leg + ": " + std::to_string(counted.readOf(wire::QueryAnswer, 2)) + " query answers for A");
        CHECK_MSG(eventually([&] { return peers.events.open() == 1; }) && !peers.events.ended() && !peers.serving.within(0) && !b.host->diagnostics().dead, leg + ": " + joined(peers.events.all()));
    }
    a.reset();
    CHECK(b.host->probe("'still'") == "still");
    CHECK_MSG(counted.readOf(wire::Close, 2) == 1, leg + ": " + std::to_string(counted.readOf(wire::Close, 2)) + " closes once A was destroyed");
}

/// The child of `remote_stream_answer_thread_that_cannot_start_stops_only_its_runtime`, alone in a fresh process: `twidget-tests --child remote_stream_answers_at_the_thread_limit`.
CHILD_CASE(remote_stream_answers_at_the_thread_limit) {
    legsLeaveNoThread([] {
        answerUnstartedOnScriptedStream(false);
        answerUnstartedOnScriptedStream(true);
        answerUnstartedOverRealEnds();
    });
}

/// The engine's end on a scripted host's engine stream while no thread can start: the reaper (runtime 1 opened and closed), B's engine and probe thread (runtime 2), D's engine (runtime 4, never probed) and every worker are made before the fill. A's start (runtime 3, a safe area kept before it) is its death with the refusal of its `widget.sandbox` within 2 s, and an event and a second start after it are dropped with no second death, nothing of A's run or loaded; D's first probe answers "ERROR no runtime" and D dies with the refusal of its `widget.probe` within 2 s, in either order, and a second probe answers the same with no second death; B's probe still runs a layout query and an Intl request the host answers, no named thread came or went, and the engine's end goes on serving. With the threads free again, A's and D's `Close` reap D's engine and B goes on.
static void engineUnstartedOnScriptedStream() {
    const std::string leg = "engine stream";
    ScriptedHost host(layoutAnswers());
    const std::string coded = SandboxRemoteCoding::encodeConfig(testConfig());
    const auto open = [](uint32_t runtime) { return FrameBytes(runtime, wire::Open).bytes(); };
    const auto started = [&](uint32_t runtime, const char* html) { return FrameBytes(runtime, wire::Start).str(html).str(coded).bytes(); };
    const char* widgetA = "<script>console.log('A ran')</script>";
    std::string measured;
    // B's probe asks a layout query and an Intl request, `number` a new one each time so the shim asks again: the next of runtime 2's widths and its Intl answer.
    const auto measuresB = [&](const std::string& number) {
        const size_t queries = host.eventsWith(2, "query:").size(), intl = host.eventsWith(2, "intl:").size();
        measured = host.probe(2, "(p.textContent = '" + number + "', p.getBoundingClientRect().width + ' ' + (" + number + ").toLocaleString())").value_or("no answer");
        return measured == std::to_string(33 + queries) + " INTL-B" && host.eventsWith(2, "query:").size() == queries + 1 && host.eventsWith(2, "intl:").size() == intl + 1;
    };
    CHECK(host.send(openingOf(*RES, true)));
    CHECK_MSG(eventually([&] { return host.events(0) == std::vector<std::string>{"ready:1"}; }), leg + ": " + joined(host.events(0)));
    // Runtime 1 opened and closed makes the reaper; B is runtime 2, A 3 and D 4.
    CHECK(host.send(open(1) + FrameBytes(1, wire::Close).bytes() + open(2) + started(2, kQueriesB) + open(3) +
                    FrameBytes(3, wire::SafeArea).str(SandboxRemoteCoding::encodeSafeArea(WidgetContentSafeArea{0, 0, 24, 0, 0, 64})).bytes() + open(4) + started(4, "<p>d</p>")));
    CHECK_MSG(eventually([&] { return host.saw(2, "log:B 33 INTL-B") && host.saw(4, "loaded"); }, 20), leg + ": " + joined(host.events(2)) + " / " + joined(host.events(4)));
    CHECK(host.probe(2, "p.textContent") == std::optional<std::string>("x"));
    std::map<std::string, int> before;
    CHECK_MSG(eventually([&] {
        before = threadNames().value_or(std::map<std::string, int>());
        return countOf(before, "widget.reaper") == 1 && countOf(before, "widget.sandbox") == 2 && countOf(before, "widget.probe") == 1;
    }), leg + ": " + namesText(before));
    {
        ThreadAllowance allowance;
        if (!filledFor(allowance, leg, before).empty()) return;
        printf("    %s: A's start, whose widget.sandbox cannot start\n", leg.c_str());
        fflush(stdout);
        const auto sent = std::chrono::steady_clock::now();
        CHECK(host.send(started(3, widgetA)));
        std::vector<std::string> died;
        const bool dies = eventually([&] { return !(died = host.eventsWith(3, "died:")).empty(); }, 2);
        printf("    %s: A heard %s in %.1f ms\n", leg.c_str(), whole(died).c_str(), msSince(sent));
        fflush(stdout);
        CHECK_MSG(dies && died.size() == 1 && unstarted(died[0].substr(5), kEngineUnstarted, "widget.sandbox"), leg + ": " + whole(died));
        // What comes for A after its death is dropped: the second start builds nothing, so it is refused nothing.
        CHECK(host.send(FrameBytes(3, wire::Event).str(kTapOnButton).bytes() + started(3, widgetA)));
        CHECK_MSG(measuresB("4321.5"), leg + ": B's probe " + measured);
        CHECK_MSG(host.events(3) == died, leg + ": " + whole(host.events(3)));

        printf("    %s: D's first probe, whose widget.probe cannot start\n", leg.c_str());
        fflush(stdout);
        const auto asked = std::chrono::steady_clock::now();
        const std::optional<std::string> probed = host.probe(4, "1 + 1");
        const double answered = msSince(asked);
        std::vector<std::string> stopped;
        const bool stops = eventually([&] { return !(stopped = host.eventsWith(4, "died:")).empty(); }, 2);
        const double ended = msSince(asked);
        const std::vector<std::string> framesD = host.events(4);
        const auto answerAt = std::find(framesD.begin(), framesD.end(), "probe:1:" + probed.value_or("")), deathAt = std::find_if(framesD.begin(), framesD.end(), [](const std::string& e) {
            return e.rfind("died:", 0) == 0;
        });
        printf("    %s: D's probe answered \"%s\" in %.1f ms; D heard %s within %.1f ms, %s\n", leg.c_str(), probed.value_or("nothing").c_str(), answered, whole(stopped).c_str(), ended,
               answerAt < deathAt ? "after the answer" : "before the answer");
        fflush(stdout);
        CHECK_MSG(probed == std::optional<std::string>("ERROR no runtime") && answered < 2000, leg + ": D's probe " + probed.value_or("unanswered") + " in " + std::to_string(answered) + " ms");
        CHECK_MSG(stops && stopped.size() == 1 && unstarted(stopped[0].substr(5), kProbeUnstarted, "widget.probe"), leg + ": " + whole(stopped));
        // A second probe tries the thread again and answers as a dead runtime's, with no second death.
        CHECK(host.probe(4, "2 + 2") == std::optional<std::string>("ERROR no runtime"));
        CHECK_MSG(measuresB("5432.5"), leg + ": B's probe " + measured);
        CHECK_MSG(host.eventsWith(4, "died:") == stopped && !host.saw(3, "loaded") && !host.saw(3, "log:A ran"), leg + ": D " + whole(host.eventsWith(4, "died:")) + " / A " + whole(host.events(3)));
        const std::map<std::string, int> after = threadNames().value_or(std::map<std::string, int>());
        printf("    %s: B's probe answered \"%s\"; after them %s\n", leg.c_str(), measured.c_str(), namesText(after).c_str());
        fflush(stdout);
        CHECK_MSG(namesText(after) == namesText(before), leg + ": " + namesText(before) + " before, " + namesText(after) + " after");
        const std::optional<std::string> served = host.served(0);
        CHECK_MSG(!served, leg + ": the engine's end stopped serving: " + served.value_or(""));
    }
    CHECK(host.send(FrameBytes(3, wire::Close).bytes() + FrameBytes(4, wire::Close).bytes()));
    std::map<std::string, int> reaped;
    CHECK_MSG(eventually([&] {
        reaped = threadNames().value_or(std::map<std::string, int>());
        return countOf(reaped, "widget.sandbox") == 1 && countOf(reaped, "widget.probe") == 1;
    }), leg + ": " + namesText(reaped));
    CHECK(host.probe(2, "'still'") == std::optional<std::string>("still"));
}

/// Behind the in-process loopback while no thread can start: A's and B's `widget.client` and B's engine are made before the fill, B loaded. A's start (its visibility kept before it) is its death with the refusal of its `widget.sandbox` within 2 s on its client's delegate, nothing of A's run or loaded, and its probe answers "ERROR no runtime"; B's probe still runs a layout query and an Intl request its client answers, and no named thread came or went.
static void engineUnstartedBehindLoopback() {
    const std::string leg = "loopback";
    StreamDelegate delegateA, delegateB;
    const auto intl = std::make_shared<StreamIntl>();
    SandboxEngine b(kMeasureIntl, testConfig(), std::make_shared<SandboxLoopbackChannel>(RES), intl);
    b.setDelegate(&delegateB);
    b.start();
    CHECK_MSG(eventually([&] { return delegateB.saw("log:w=77 INTL") && delegateB.saw("loaded"); }, 20), leg + ": " + joined(delegateB.events()));
    SandboxEngine a("<script>console.log('A ran')</script>", testConfig(), std::make_shared<SandboxLoopbackChannel>(RES));
    a.setDelegate(&delegateA);
    a.setVisible(false);
    a.thread().sync([] {});
    const std::map<std::string, int> before = threadNames().value_or(std::map<std::string, int>());
    CHECK_MSG(countOf(before, "widget.client") == 2 && countOf(before, "widget.sandbox") == 1, leg + ": " + namesText(before));
    {
        ThreadAllowance allowance;
        if (!filledFor(allowance, leg, before).empty()) return;
        printf("    %s: A's start, whose widget.sandbox cannot start\n", leg.c_str());
        fflush(stdout);
        const auto sent = std::chrono::steady_clock::now();
        a.start();
        std::vector<std::string> died;
        const bool dies = eventually([&] { return !(died = delegateA.eventsWith("died:")).empty(); }, 2);
        const double ms = msSince(sent);
        const std::string probed = a.probe("1 + 1");
        printf("    %s: A heard %s in %.1f ms; its probe answered \"%s\"\n", leg.c_str(), whole(died).c_str(), ms, probed.c_str());
        fflush(stdout);
        CHECK_MSG(dies && died.size() == 1 && unstarted(died[0].substr(5), kEngineUnstarted, "widget.sandbox"), leg + ": " + whole(died));
        CHECK_MSG(probed == "ERROR no runtime" && delegateA.events() == died, leg + ": " + probed + " / " + whole(delegateA.events()));
        const size_t queries = delegateB.eventsWith("query:").size(), formatted = intl->threads().size();
        const std::string measured = b.probe("(document.getElementById('p').textContent = 'again', document.getElementById('p').getBoundingClientRect().width + ' ' + (4321.5).toLocaleString())");
        const std::map<std::string, int> after = threadNames().value_or(std::map<std::string, int>());
        printf("    %s: B's probe answered \"%s\"; after them %s\n", leg.c_str(), measured.c_str(), namesText(after).c_str());
        fflush(stdout);
        CHECK_MSG(measured == "77 INTL" && delegateB.eventsWith("query:").size() == queries + 1 && intl->threads().size() == formatted + 1, leg + ": B's probe " + measured);
        CHECK_MSG(namesText(after) == namesText(before), leg + ": " + namesText(before) + " before, " + namesText(after) + " after");
    }
}

/// The child of `remote_stream_engine_threads_that_cannot_start_stop_only_their_runtime`, alone in a fresh process: `twidget-tests --child remote_stream_engines_at_the_thread_limit`.
CHILD_CASE(remote_stream_engines_at_the_thread_limit) {
    legsLeaveNoThread([] {
        engineUnstartedOnScriptedStream();
        engineUnstartedBehindLoopback();
    });
}

/// Both real ends while the engine's end has no reaper and no thread can start: A's and B's client engines (their channels on the host's end, A's kept), their engines and probe threads in the engine's end and every worker are made before the fill, and A's kept channel closes while A's client lives (destroying A would free its thread for the reaper). The engine's end stops serving within 2 s with why (its context, the refusal of `widget.reaper`, EAGAIN), every engine and probe thread of its gone; the host's end hears the engine process close the stream, and B dies of it.
static void reaperUnstartedAtClose() {
    const std::string leg = "a Close with no reaper";
    StreamPeers peers;
    StreamDelegate delegateA, delegateB;
    const std::shared_ptr<SandboxRemoteChannel> channelA = peers.end->channel();
    SandboxEngine a("<p>a</p>", testConfig(), channelA);
    SandboxEngine b("<p>b</p>", testConfig(), peers.end->channel());
    a.setDelegate(&delegateA);
    b.setDelegate(&delegateB);
    a.start();
    b.start();
    CHECK_MSG(eventually([&] { return delegateA.saw("loaded") && delegateB.saw("loaded"); }, 20), leg + ": " + joined(delegateA.events()) + " / " + joined(delegateB.events()));
    CHECK(a.probe("1 + 1") == "2" && b.probe("2 + 2") == "4");
    CHECK_MSG(eventually([&] { return peers.events.open() == 2; }), leg + ": " + joined(peers.events.all()));
    const std::map<std::string, int> before = threadNames().value_or(std::map<std::string, int>());
    CHECK_MSG(countOf(before, "widget.sandbox") == 2 && countOf(before, "widget.probe") == 2 && countOf(before, "widget.reaper") == 0 && countOf(before, "widget.answer") == 0,
              leg + ": " + namesText(before));
    {
        ThreadAllowance allowance;
        if (!filledFor(allowance, leg, before).empty()) return;
        printf("    %s: A's channel closes, and the engine's end has no widget.reaper\n", leg.c_str());
        fflush(stdout);
        const auto closed = std::chrono::steady_clock::now();
        channelA->close();
        const std::optional<std::string> reason = peers.serving.within(2);
        const double ms = msSince(closed);
        std::map<std::string, int> after;
        const bool gone = eventually([&] {
            after = threadNames().value_or(std::map<std::string, int>());
            return countOf(after, "widget.sandbox") == 0 && countOf(after, "widget.probe") == 0 && countOf(after, "widget.reaper") == 0;
        }, 2);
        printf("    %s: the engine's end stopped serving in %.1f ms: \"%s\"; after it %s\n", leg.c_str(), ms, reason.value_or("still serving").c_str(), namesText(after).c_str());
        fflush(stdout);
        CHECK_MSG(reason && unstarted(*reason, kReaperUnstarted, "widget.reaper"), leg + ": " + reason.value_or("still serving after 2 s"));
        CHECK_MSG(gone, leg + ": " + namesText(after));
        CHECK_MSG(eventually([&] { return peers.events.ended().has_value(); }, 2) && endsOnce(peers.events.all(), "the engine process closed the stream"), leg + ": " + joined(peers.events.all()));
        CHECK_MSG(eventually([&] { return delegateB.saw("died:the widget engine process ended"); }, 2), leg + ": " + joined(delegateB.events()));
        printf("    %s: the host's end heard %s; B %s\n", leg.c_str(), joined(peers.events.all()).c_str(), whole(delegateB.eventsWith("died:")).c_str());
        fflush(stdout);
    }
}

/// The child of `remote_stream_reaper_that_cannot_start_ends_serving_with_a_reason`'s first leg, alone in a fresh process: `twidget-tests --child remote_stream_reaper_at_the_thread_limit`.
CHILD_CASE(remote_stream_reaper_at_the_thread_limit) {
    legsLeaveNoThread([] { reaperUnstartedAtClose(); });
}

/// The end of the stream while the engine's end has no reaper, a probe waits for a layout query nobody answers and no thread can start: the host's side is the test's own (frames written and read on its thread, no worker of the test's that ends with the stream), the engine's end serves on a worker made before the fill, runtime 1's engine and probe thread and runtime 2's engine exist, and runtime 1's probe waits for its query. The host closes its endpoint: the engine's end stops serving within 5 s with "" (the end on a frame's boundary), every engine and probe thread of its gone, and it never needed a reaper.
static void endWithNoReaper() {
    const std::string leg = "the end with no reaper";
    MemoryStreamPair pair = memoryStreamPair(size_t(1) << 20);
    Background<std::string> serving([&] { return SandboxRemoteEngineEnd::serve(*pair.engine, kBuild); });
    FrameStreamReader reader(*pair.host);
    std::map<uint32_t, std::vector<std::string>> seen;
    // Reads on until `runtime` sent a frame that `describe`s as starting with `prefix`; false once the stream ended before it.
    const auto readUntil = [&](uint32_t runtime, const std::string& prefix) {
        const auto came = [&] {
            const std::vector<std::string>& frames = seen[runtime];
            return std::any_of(frames.begin(), frames.end(), [&](const std::string& e) { return e.rfind(prefix, 0) == 0; });
        };
        while (!came()) {
            const std::optional<ParsedFrame> frame = reader.next();
            if (!frame) return false;
            seen[frame->runtime].push_back(describe(*frame));
        }
        return true;
    };
    const std::string coded = SandboxRemoteCoding::encodeConfig(testConfig());
    CHECK(pair.host->write(openingOf(*RES, true) + FrameBytes(1, wire::Open).bytes() + FrameBytes(1, wire::Start).str("<p id='p'>x</p>").str(coded).bytes() + FrameBytes(2, wire::Open).bytes() +
                           FrameBytes(2, wire::Start).str("<p>y</p>").str(coded).bytes()));
    CHECK_MSG(readUntil(0, "ready:") && readUntil(1, "loaded") && readUntil(2, "loaded"), leg + ": " + joined(seen[1]) + " / " + joined(seen[2]));
    CHECK(pair.host->write(FrameBytes(1, wire::Probe).i64(1).str("document.getElementById('p').getBoundingClientRect().width").bytes()));
    CHECK_MSG(readUntil(1, "query:"), leg + ": " + joined(seen[1]));
    const std::map<std::string, int> before = threadNames().value_or(std::map<std::string, int>());
    CHECK_MSG(countOf(before, "widget.sandbox") == 2 && countOf(before, "widget.probe") == 1 && countOf(before, "widget.reaper") == 0, leg + ": " + namesText(before));
    {
        ThreadAllowance allowance;
        if (!filledFor(allowance, leg, before).empty()) {
            pair.host->shutdown();
            return;
        }
        printf("    %s: the host closes its endpoint while runtime 1's probe waits for its query\n", leg.c_str());
        fflush(stdout);
        const auto closed = std::chrono::steady_clock::now();
        pair.host->close();
        const std::optional<std::string> reason = serving.within(5);
        const double ms = msSince(closed);
        std::map<std::string, int> after;
        const bool gone = eventually([&] {
            after = threadNames().value_or(std::map<std::string, int>());
            return countOf(after, "widget.sandbox") == 0 && countOf(after, "widget.probe") == 0 && countOf(after, "widget.reaper") == 0;
        }, 2);
        printf("    %s: the engine's end stopped serving in %.1f ms: %s; after it %s\n", leg.c_str(), ms, reason ? ("\"" + *reason + "\"").c_str() : "still serving",
               after.empty() || namesText(after).empty() ? "no named thread" : namesText(after).c_str());
        fflush(stdout);
        CHECK_MSG(reason == std::optional<std::string>(""), leg + ": " + reason.value_or("still serving after 5 s"));
        CHECK_MSG(gone, leg + ": " + namesText(after));
    }
}

/// The child of `remote_stream_reaper_that_cannot_start_ends_serving_with_a_reason`'s second leg, alone in a fresh process: `twidget-tests --child remote_stream_end_at_the_thread_limit`.
CHILD_CASE(remote_stream_end_at_the_thread_limit) {
    legsLeaveNoThread([] { endWithNoReaper(); });
}
#endif

/// A host's end whose threads cannot start fails at once and leaves nothing behind: a fresh child (`--child remote_stream_host_end_at_the_thread_limit`) uses up its thread allowance; `start` and `startHosting` with 0, 1 and 2 threads free (`widget.reader`, `widget.writer`, `widget.monitor` refused in turn) each throw std::system_error EAGAIN naming that thread within 1 s, the task's thread count comes back, nothing of the queued opening reaches the stream (its endpoint closed, not shut down, the other end reads 0 bytes) and `Events` hear nothing; with 3 free each opener writes its opening and stops as usual with its three threads joined. The child exits 0 within 30 s, or it is killed and the test fails.
TEST(remote_stream_host_end_whose_threads_cannot_start_fails_promptly) {
#ifdef __APPLE__
    childPasses("remote_stream_host_end_at_the_thread_limit", 30);
#else
    printf("  not run on this platform: this test uses macOS's per-process thread limit\n");
#endif
}

/// A runtime whose `widget.answer` cannot start dies alone with the reason: a fresh child (`--child remote_stream_answers_at_the_thread_limit`) uses up its thread allowance right before runtime A's first synchronous request, with B's answer thread, A's probe and every worker made before. On a scripted engine's stream (a query) and a hosting stream (a platform request) A's client hears the refusal (the host's context, `JSThread widget.answer`, EAGAIN) as its death, then the loss; its waiting probe answers "ERROR no runtime" within 2 s, its `Close` goes out once and no answer. Over both real ends a WidgetHost's probe that asks A's first layout query answers "ERROR no runtime" within 2 s, A dies with the refusal, and the engine's end reaps its engine and probe threads. B's requests are still answered and the stream goes on in each. The child exits 0 within 120 s, or it is killed and the test fails.
TEST(remote_stream_answer_thread_that_cannot_start_stops_only_its_runtime) {
#ifdef __APPLE__
    childPasses("remote_stream_answers_at_the_thread_limit", 120);
#else
    printf("  not run on this platform: this test uses macOS's per-process thread limit\n");
#endif
}

/// A runtime whose engine or probe thread cannot start dies alone with the reason: a fresh child (`--child remote_stream_engines_at_the_thread_limit`) uses up its thread allowance, every other thread made before. On a scripted host's engine stream runtime A's start is its death with the refusal (the engine's end's context, `JSThread widget.sandbox`, EAGAIN) within 2 s, what came for A before and after it dropped with no second death; D's first probe answers "ERROR no runtime" and D is stopped with the refusal of its `widget.probe` within 2 s, a second probe the same with no second death; B's probe still runs a layout query and an Intl request, and the engine's end goes on serving. Behind the in-process loopback A's start is its death with the same refusal within 2 s and its probe answers "ERROR no runtime", while B still runs a layout query and an Intl request. The child exits 0 within 120 s, or it is killed and the test fails.
TEST(remote_stream_engine_threads_that_cannot_start_stop_only_their_runtime) {
#ifdef __APPLE__
    childPasses("remote_stream_engines_at_the_thread_limit", 120);
#else
    printf("  not run on this platform: this test uses macOS's per-process thread limit\n");
#endif
}

/// A `widget.reaper` that cannot start ends serving with why, and the end of serving never needs one: fresh children use up their thread allowance, every other thread made before. In one (`--child remote_stream_reaper_at_the_thread_limit`) a `Close` over both real ends, with two live client engines and no reaper yet, ends serving within 2 s with the refusal (the engine's end's context, `JSThread widget.reaper`, EAGAIN), every engine and probe thread of the engine's end gone, the host's end hearing the engine process close the stream and the other runtime dying of it. In the other (`--child remote_stream_end_at_the_thread_limit`) the end of the stream, with a probe waiting for a query nobody answers and no reaper, ends serving within 5 s with "" and every engine and probe thread gone. Each child exits 0 within 60 s, or it is killed and the test fails.
TEST(remote_stream_reaper_that_cannot_start_ends_serving_with_a_reason) {
#ifdef __APPLE__
    childPasses("remote_stream_reaper_at_the_thread_limit", 60);
    childPasses("remote_stream_end_at_the_thread_limit", 60);
#else
    printf("  not run on this platform: this test uses macOS's per-process thread limit\n");
#endif
}
