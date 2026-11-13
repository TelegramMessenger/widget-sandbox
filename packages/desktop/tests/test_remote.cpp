// The remote engine's tests (remote_engine.h: the coding, a client-mode SandboxEngine, the server, the loopback, WidgetHost's factory), in test_main.cpp's suite after its own tests: `twidget-tests remote_`.
#include "test_support.h"

#include "../harness/recording_host.h"
#include "twidget/intl.h"
#include "twidget/json.h"
#include "twidget/remote_engine.h"
#include "twidget/sdk_policy.h"
#include "twidget/widget_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif

// ───────────── remote engine (remote_engine.h) ─────────────

/// The edits json_fuzz_never_crashes makes to its seeds, plus any byte, 1–3 per text: the same corpus on every run.
struct JsonDamage {
    uint64_t state;
    uint32_t next() { state = state * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(state >> 33); }
    std::string operator()(std::string text) {
        static const char alphabet[] = "{}[]\":,.-+eE0123456789truefalsn \\u/abcdef\x00\x80\xff";
        const int edits = 1 + int(next() % 3);
        for (int e = 0; e < edits && !text.empty(); e++) {
            size_t at = next() % text.size();
            switch (next() % 5) {
            case 0: text[at] = alphabet[next() % (sizeof(alphabet) - 1)]; break;
            case 1: text.erase(at, 1 + next() % 4); break;
            case 2: text.insert(at, 1, alphabet[next() % (sizeof(alphabet) - 1)]); break;
            case 3: text[at] = char(next() & 0xFF); break;
            default: text.resize(at); break;
            }
        }
        return text;
    }
};

/// Damages `wire` `rounds` times: nothing crashes, and what a decoder accepts encodes to a form that decodes back to itself. Returns how many it accepted.
template <class Decode, class Encode>
static int damageRoundTrips(const std::string& wire, int rounds, uint64_t seed, Decode decode, Encode encode) {
    JsonDamage damage{seed};
    int accepted = 0;
    for (int i = 0; i < rounds; i++) {
        auto decoded = decode(damage(wire));
        if (!decoded) continue;
        accepted++;
        const std::string again = encode(*decoded);
        auto twice = decode(again);
        CHECK_MSG(twice && encode(*twice) == again, again.substr(0, 200));
    }
    return accepted;
}

/// How many proper prefixes of `wire` decode: a cut message must give nothing.
template <class Decode>
static int decodablePrefixes(const std::string& wire, Decode decode) {
    int n = 0;
    for (size_t length = 0; length < wire.size(); length++) n += decode(std::string_view(wire).substr(0, length)).has_value();
    return n;
}

/// `wire` padded with spaces to exactly the message limit decodes, one byte more does not.
template <class Decode>
static bool boundedAtTheMessageLimit(const std::string& wire, Decode decode) {
    std::string padded = wire + std::string(SandboxEngine::kRemoteMessageLimit - wire.size(), ' ');
    const bool atLimit = decode(padded).has_value();
    padded.push_back(' ');
    return atLimit && !decode(padded).has_value();
}

/// No object in `v` names a member twice, so a reader that takes the first and one that takes the last see the same value.
static bool uniqueKeys(const Json& v) {
    if (v.isArray()) return std::all_of(v.arr().begin(), v.arr().end(), uniqueKeys);
    std::set<std::string> seen;
    for (const auto& [k, x] : v.obj())
        if (!seen.insert(k).second || !uniqueKeys(x)) return false;
    return true;
}

static std::string nestedArrays(int depth) { return std::string(size_t(depth), '[') + "0" + std::string(size_t(depth), ']'); }

static bool sameSafeArea(const WidgetContentSafeArea& a, const WidgetContentSafeArea& b) {
    return a.top == b.top && a.right == b.right && a.bottom == b.bottom && a.left == b.left && a.cornerLeft == b.cornerLeft && a.cornerRight == b.cornerRight;
}

/// The fields in which two configs differ, "" when none (SandboxConfig has no ==).
static std::string configDifferences(const SandboxConfig& a, const SandboxConfig& b) {
    std::string out;
    auto field = [&](const char* name, bool same) { if (!same) out += (out.empty() ? "" : ", ") + std::string(name); };
    field("memoryLimitBytes", a.memoryLimitBytes == b.memoryLimitBytes);
    field("cpuLimitMs", a.cpuLimitMs == b.cpuLimitMs);
    field("cpuQuotaMs", a.cpuQuotaMs == b.cpuQuotaMs);
    field("cpuQuotaWindowMs", a.cpuQuotaWindowMs == b.cpuQuotaWindowMs);
    field("cpuKillAfterMs", a.cpuKillAfterMs == b.cpuKillAfterMs);
    field("mainThreadQuotaMs", a.mainThreadQuotaMs == b.mainThreadQuotaMs);
    field("maxTimers", a.maxTimers == b.maxTimers);
    field("visible", a.visible == b.visible);
    field("innerWidth", a.innerWidth == b.innerWidth);
    field("innerHeight", a.innerHeight == b.innerHeight);
    field("devicePixelRatio", a.devicePixelRatio == b.devicePixelRatio);
    field("theme", a.theme == b.theme);
    field("themeParams", a.themeParams == b.themeParams);
    field("locale", a.locale == b.locale);
    field("contentSafeArea", sameSafeArea(a.contentSafeArea, b.contentSafeArea));
    field("storage", a.storage.local == b.storage.local && a.storage.session == b.storage.session);
    field("data", a.data == b.data);
    const WidgetContext &x = a.context, &y = b.context;
    field("context", x.isCopy == y.isCopy && x.canQuery == y.canQuery && x.backgroundQuery == y.backgroundQuery && x.preview == y.preview && x.wallet == y.wallet &&
                         x.chatType == y.chatType && x.viewer == y.viewer);
    field("policy", a.policy == b.policy);
    field("features", a.features == b.features);
    return out;
}

/// Every field away from its default, the strings with a NUL, a WTF-8 lone surrogate and invalid UTF-8 where a host may pass them.
static SandboxConfig everyConfigField() {
    SandboxConfig c;
    c.memoryLimitBytes = size_t(12) << 20;
    c.cpuLimitMs = 750.5;
    c.cpuQuotaMs = 250;
    c.cpuQuotaWindowMs = 2000;
    c.cpuKillAfterMs = 9000;
    c.mainThreadQuotaMs = 80.25;
    c.maxTimers = 12;
    c.visible = false;
    c.innerWidth = 320;
    c.innerHeight = 240.5;
    c.devicePixelRatio = 2.75;
    c.theme = "dark";
    c.themeParams = {{"bg_color", "#000000"}, {"text_color", "#FFFFFF"}};
    c.locale = "ru-RU";
    c.contentSafeArea = WidgetContentSafeArea{1, 2, 3.5, 4, 5, 6};
    c.storage.local = {{"a", "b"}, {"lone", "\xED\xA0\x80"}, {"bytes", "\xFF\xC3"}};
    c.storage.session = {{"nul", std::string("d\0e", 3)}};
    c.data = WidgetData{std::string("chat"), std::string("public\x01")};
    c.context.isCopy = c.context.canQuery = c.context.backgroundQuery = c.context.preview = c.context.wallet = true;
    c.context.chatType = "group";
    c.context.viewer = WidgetViewer{42, std::string("Ann"), std::string("Lee"), std::string("ann_lee"), true};
    c.policy = "compat";
    c.features = {"webAnimations", "binaryOps", "canvasWords"};
    return c;
}

/// Swift's testConfigRoundTrip and more: every field, the required members, wrong types, numbers at their destination types' ends, repeated keys, cuts, size, depth and damage.
TEST(remote_config_coding_round_trips_and_survives_damage) {
    using Coding = SandboxRemoteCoding;
    const SandboxConfig full = everyConfigField();
    const std::string wire = Coding::encodeConfig(full);
    auto decoded = Coding::decodeConfig(wire);
    CHECK_MSG(decoded && configDifferences(*decoded, full).empty(), decoded ? configDifferences(*decoded, full) : wire);
    auto parsed = Json::parse(wire);
    CHECK_MSG(parsed && parsed->isObject() && parsed->size() == 20 && uniqueKeys(*parsed), wire);
    auto defaults = Coding::decodeConfig(Coding::encodeConfig(SandboxConfig{}));
    CHECK(defaults && configDifferences(*defaults, SandboxConfig{}).empty() && !defaults->context.viewer && !defaults->data.chatData && !defaults->data.publicData);
    for (bool WidgetContext::*flag : {&WidgetContext::isCopy, &WidgetContext::canQuery, &WidgetContext::backgroundQuery, &WidgetContext::preview, &WidgetContext::wallet}) {
        SandboxConfig one;
        one.context.*flag = true;
        auto back = Coding::decodeConfig(Coding::encodeConfig(one));
        CHECK(back && configDifferences(*back, one).empty());
    }

    const std::string required = "\"innerWidth\":320,\"innerHeight\":240,\"devicePixelRatio\":2,\"theme\":\"dark\"";
    auto with = [&](const std::string& members) { return Coding::decodeConfig("{" + required + (members.empty() ? "" : ",") + members + "}"); };
    SandboxConfig minimal;
    minimal.innerWidth = 320;
    minimal.innerHeight = 240;
    minimal.devicePixelRatio = 2;
    minimal.theme = "dark";
    auto bare = with("");
    CHECK_MSG(bare && configDifferences(*bare, minimal).empty(), bare ? configDifferences(*bare, minimal) : "nothing");
    for (const char* broken : {"{\"innerHeight\":240,\"devicePixelRatio\":2,\"theme\":\"dark\"}", "{\"innerWidth\":\"320\",\"innerHeight\":240,\"devicePixelRatio\":2,\"theme\":\"dark\"}",
                               "{\"innerWidth\":1e999,\"innerHeight\":240,\"devicePixelRatio\":2,\"theme\":\"dark\"}", "{\"innerWidth\":320,\"innerHeight\":-1e999,\"devicePixelRatio\":2,\"theme\":\"dark\"}",
                               "{\"innerWidth\":320,\"innerHeight\":240,\"devicePixelRatio\":null,\"theme\":\"dark\"}", "{\"innerWidth\":320,\"innerHeight\":240,\"devicePixelRatio\":2,\"theme\":1}",
                               "{\"innerWidth\":320,\"innerHeight\":240,\"devicePixelRatio\":2}", "{}", "[]", "null", "\"dark\"", "", "{\"innerWidth\":320,"})
        CHECK_MSG(!Coding::decodeConfig(broken), broken);

    // Optional members of another type keep their defaults, as Swift's `as?` does; nesting within kMaxDepth is accepted in a member nobody reads.
    auto wrong = with("\"memoryLimitBytes\":\"1\",\"cpuLimitMs\":true,\"cpuQuotaMs\":null,\"cpuQuotaWindowMs\":[],\"cpuKillAfterMs\":{},\"mainThreadQuotaMs\":1e999,"
                      "\"maxTimers\":\"7\",\"visible\":0,\"themeParams\":{\"bg_color\":1},\"locale\":5,\"contentSafeArea\":[1],\"storage\":\"x\",\"data\":[],\"context\":1,"
                      "\"policy\":1,\"features\":[\"webAnimations\",1],\"unknown\":" + nestedArrays(Coding::kMaxDepth - 1));
    CHECK_MSG(wrong && configDifferences(*wrong, minimal).empty(), wrong ? configDifferences(*wrong, minimal) : "nothing");
    auto inner = with("\"storage\":{\"local\":{\"a\":1},\"session\":{\"b\":\"c\"}},\"data\":{\"chatData\":1,\"publicData\":\"p\"},\"contentSafeArea\":{\"top\":\"1\",\"bottom\":2},"
                      "\"context\":{\"isCopy\":1,\"canQuery\":true,\"chatType\":7,\"viewer\":{\"id\":\"42\"}}");
    CHECK(inner && inner->storage.local.empty() && inner->storage.session == (StorageArea{{"b", "c"}}) && !inner->data.chatData && inner->data.publicData == std::string("p") &&
          inner->contentSafeArea.top == 0 && inner->contentSafeArea.bottom == 2 && !inner->context.isCopy && inner->context.canQuery && inner->context.chatType.empty() &&
          !inner->context.viewer);
    CHECK(!with("\"unknown\":" + nestedArrays(Coding::kMaxDepth)) && !with("\"unknown\":" + nestedArrays(100000)));

    const int intMax = std::numeric_limits<int>::max(), intMin = std::numeric_limits<int>::min();
    for (auto [value, expect] : {std::pair<const char*, int>{"1e300", intMax}, {"-1e300", intMin}, {"2147483648", intMax}, {"2147483647.5", intMax}, {"-2147483649", intMin},
                                 {"2147483647", intMax}, {"12.9", 12}, {"-0.5", 0}, {"1e999", 64}, {"-1e999", 64}}) {
        auto c = with(std::string("\"maxTimers\":") + value);
        CHECK_MSG(c && c->maxTimers == expect, value);
    }
    const size_t sizeMax = std::numeric_limits<size_t>::max(), defaultMemory = SandboxConfig{}.memoryLimitBytes;
    const size_t exactCap = size_t(std::min<uint64_t>(uint64_t(1) << 53, sizeMax)), twoTo32 = size_t(std::min<uint64_t>(uint64_t(1) << 32, sizeMax));
    for (auto [value, expect] : {std::pair<const char*, size_t>{"9007199254740992", exactCap}, {"9007199254740994", exactCap}, {"1e300", exactCap}, {"4294967296", twoTo32},
                                 {"4294967295.5", size_t(4294967295u)}, {"1048576.75", size_t(1048576)}, {"0.5", size_t(0)}, {"-1", size_t(0)}, {"-1e300", size_t(0)},
                                 {"1e999", defaultMemory}, {"-1e999", defaultMemory}}) {
        auto c = with(std::string("\"memoryLimitBytes\":") + value);
        CHECK_MSG(c && c->memoryLimitBytes == expect, value);
    }
    // Viewer ids inside [-2^63, 2^63) exactly, truncated; anything else is no viewer.
    auto viewerId = [&](const std::string& id) -> std::optional<int64_t> {
        auto c = with("\"context\":{\"viewer\":{\"id\":" + id + ",\"first_name\":\"Ann\"}}");
        if (!c || !c->context.viewer) return std::nullopt;
        return c->context.viewer->id;
    };
    CHECK(viewerId("-9223372036854775808") == INT64_MIN && viewerId("9223372036854774784") == int64_t(9223372036854774784));
    CHECK(viewerId("9.2e18") == int64_t(9200000000000000000) && viewerId("42.9") == 42 && viewerId("-0.5") == 0);
    for (const char* bad : {"9223372036854775807", "9223372036854775808", "-9223372036854777856", "1e19", "1e999", "-1e999", "\"42\"", "null", "true"})
        CHECK_MSG(!viewerId(bad), bad);

    // A repeated member: the first for a scalar (Json::find), the last within a string map, as sdk::checkStorageArea reads one.
    auto twice = Coding::decodeConfig("{\"theme\":\"first\"," + required + ",\"theme\":\"last\",\"themeParams\":{\"bg_color\":\"#111111\",\"bg_color\":\"#222222\"},"
                                      "\"storage\":{\"local\":{\"k\":\"1\",\"k\":\"2\"},\"local\":{}}}");
    CHECK(twice && twice->theme == "first" && twice->themeParams == (std::map<std::string, std::string>{{"bg_color", "#222222"}}) &&
          twice->storage.local == (StorageArea{{"k", "2"}}));
    // Strings as the engine has them in process: an escaped lone surrogate stays WTF-8, so the SDK checks that refuse it still do and storage keeps it.
    auto strings = with("\"storage\":{\"local\":{\"s\":\"\\ud800\",\"n\":\"a\\u0000b\"}},\"data\":{\"chatData\":\"\\udc00\"},\"context\":{\"viewer\":{\"id\":7,\"first_name\":\"A\\ud800\"}}");
    CHECK(strings && strings->storage.local == (StorageArea{{"s", "\xED\xA0\x80"}, {"n", std::string("a\0b", 3)}}));
    CHECK(strings && strings->data.chatData == std::string("\xED\xB0\x80") && !sdk::checkWidgetDataValue(*strings->data.chatData) && sdk::checkWidgetDataValue("\xEF\xBF\xBD"));
    CHECK(strings && sdk::checkViewer(strings->context.viewer).rejected == std::vector<std::string>{"first_name"});

    CHECK(decodablePrefixes(wire, Coding::decodeConfig) == 0);
    CHECK(boundedAtTheMessageLimit(wire, Coding::decodeConfig));
    int accepted = damageRoundTrips(wire, 5000, 1, Coding::decodeConfig, Coding::encodeConfig);
    const std::string doubled = wire.substr(0, wire.size() - 1) + "," + wire.substr(1);
    auto first = Coding::decodeConfig(doubled);
    CHECK(first && configDifferences(*first, full).empty());
    accepted += damageRoundTrips(doubled, 2000, 2, Coding::decodeConfig, Coding::encodeConfig);
    CHECK_MSG(accepted > 100, std::to_string(accepted) + " damaged configs accepted");
}

/// Swift's testOutcomeAndStatsRoundTrip: every outcome and failure code (NotAllowed included), Swift's key precedence, and stats with validatorMs clamped into their types.
TEST(remote_outcome_and_stats_coding_round_trip) {
    using Coding = SandboxRemoteCoding;
    using Outcome = SandboxRemoteOutcome;
    using F = WidgetRequestFailure;
    const std::vector<Outcome> outcomes = {Outcome::answer(std::string("x")), Outcome::answer(std::nullopt), Outcome::answer(std::string()),
                                           Outcome::answer(std::string("a\0\xED\xA0\x80\xFF", 6)), Outcome::failed(F::Timeout), Outcome::failed(F::RateLimited),
                                           Outcome::failed(F::Failed), Outcome::failed(F::NotAllowed), Outcome::failed(F::Rejected), Outcome::failed(F::BadRequest),
                                           Outcome::unsupported()};
    int accepted = 0;
    uint64_t seed = 100;
    for (const Outcome& o : outcomes) {
        const std::string wire = Coding::encodeOutcome(o);
        auto back = Coding::decodeOutcome(wire);
        CHECK_MSG(back && *back == o, wire);
        CHECK_MSG(decodablePrefixes(wire, Coding::decodeOutcome) == 0 && boundedAtTheMessageLimit(wire, Coding::decodeOutcome), wire);
        accepted += damageRoundTrips(wire, 500, seed++, Coding::decodeOutcome, Coding::encodeOutcome);
    }
    CHECK_MSG(accepted > 0, std::to_string(accepted) + " damaged outcomes accepted");
    CHECK(Coding::encodeOutcome(Outcome::answer(std::string("x"))) == "{\"answer\":\"x\"}" && Coding::encodeOutcome(Outcome::answer(std::nullopt)) == "{\"answer\":null}");
    CHECK(Coding::encodeOutcome(Outcome::failed(F::NotAllowed)) == "{\"failure\":\"NOT_ALLOWED\"}" && Coding::encodeOutcome(Outcome::failed(F::Rejected)) == "{\"failure\":\"REJECTED\"}");
    CHECK(Coding::encodeOutcome(Outcome::unsupported()) == "{\"unsupported\":true}");
    CHECK(sdk::requestFailureCode(F::NotAllowed) == "NOT_ALLOWED" && sdk::walletFailureCode(F::NotAllowed) == "NOT_ALLOWED");
    CHECK(sdk::requestFailureCode(F::Rejected) == "FAILED" && sdk::requestFailureCode(F(42)) == "FAILED" && sdk::walletFailureCode(F(42)) == "FAILED");
    CHECK(Outcome::from(std::nullopt) == Outcome::unsupported() && Outcome::from(WidgetRequestOutcome{}) == Outcome::answer(std::nullopt));
    CHECK(Outcome::from(WidgetRequestOutcome{F::None, {std::string("r")}}) == Outcome::answer(std::string("r")));
    CHECK(Outcome::from(WidgetRequestOutcome{F::Timeout, {std::string("ignored")}}) == Outcome::failed(F::Timeout));

    // Swift's order: any `unsupported` member, then a string `failure` (an unknown code is NotAllowed), then any `answer`.
    auto decode = [](const std::string& text) { return Coding::decodeOutcome(text); };
    CHECK(decode("{\"answer\":\"a\",\"unsupported\":false}") == Outcome::unsupported() && decode("{\"unsupported\":null}") == Outcome::unsupported());
    CHECK(decode("{\"answer\":\"a\",\"failure\":\"TIMEOUT\"}") == Outcome::failed(F::Timeout) && decode("{\"failure\":1,\"answer\":\"a\"}") == Outcome::answer(std::string("a")));
    for (const char* code : {"NOPE", "timeout", "NONE", "", "INVALID_ANSWER", "BUSY", "NOT_ALLOWED"})
        CHECK_MSG(decode("{\"failure\":\"" + std::string(code) + "\"}") == Outcome::failed(F::NotAllowed), code);
    CHECK(decode("{\"answer\":5}") == Outcome::answer(std::nullopt) && decode("{\"answer\":\"a\",\"answer\":\"b\"}") == Outcome::answer(std::string("a")));
    CHECK(decode("{\"answer\":\"\\ud800\"}") == Outcome::answer(std::string("\xED\xA0\x80")) && !sdk::checkRequestAnswer(WidgetRequestAnswer{std::string("\xED\xA0\x80")}));
    for (const char* bad : {"{}", "[]", "null", "\"answer\"", "", "{\"answer\":\"a\"", "{\"other\":1}"}) CHECK_MSG(!decode(bad), bad);
    CHECK(!decode("{\"answer\":\"a\",\"x\":" + nestedArrays(Coding::kMaxDepth) + "}") && decode("{\"answer\":\"a\",\"x\":" + nestedArrays(Coding::kMaxDepth - 1) + "}"));

    SandboxStats stats;
    stats.jsMs = 12.5;
    stats.tasks = 3;
    stats.memoryBytes = size_t(1) << 22;
    stats.ops = 7;
    stats.queries = 9;
    stats.queryMs = 1.25;
    stats.queryCacheHits = 4;
    stats.missedMutations = 2;
    stats.throttledTasks = 5;
    stats.cpuWindowMs = 280.75;
    stats.validatorMs = 6.5;
    const std::vector<double> values = Coding::encodeStats(stats);
    CHECK(values.size() == Coding::kStatsFields && values == std::vector<double>({12.5, 3, 4194304, 7, 9, 1.25, 4, 2, 5, 280.75, 6.5}));
    auto back = Coding::decodeStats(values);
    CHECK(back && back->jsMs == 12.5 && back->tasks == 3 && back->memoryBytes == stats.memoryBytes && back->ops == 7 && back->queries == 9 && back->queryMs == 1.25 &&
          back->queryCacheHits == 4 && back->missedMutations == 2 && back->throttledTasks == 5 && back->cpuWindowMs == 280.75 && back->validatorMs == 6.5);
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    const size_t sizeMax = std::numeric_limits<size_t>::max();
    const size_t statsCap = size_t(std::min<uint64_t>(1000000000000000ull, sizeMax)), twoTo32 = size_t(std::min<uint64_t>(uint64_t(1) << 32, sizeMax));
    const int intMax = std::numeric_limits<int>::max();
    auto clamped = Coding::decodeStats({nan, -1, 1e300, 0, 0, inf, 0, 0, 0, -inf, 1e15 + 1});
    CHECK(clamped && clamped->jsMs == 0 && clamped->tasks == 0 && clamped->memoryBytes == statsCap && clamped->queryMs == 0 && clamped->cpuWindowMs == 0 && clamped->validatorMs == 1e15);
    auto ints = Coding::decodeStats({0.25, 1e300, 4294967296, 2147483647, 2147483648, 0, 2147483646.9, 3.7, -0.5, 1e15, 0});
    CHECK(ints && ints->jsMs == 0.25 && ints->tasks == intMax && ints->memoryBytes == twoTo32 && ints->ops == intMax && ints->queries == intMax &&
          ints->queryCacheHits == 2147483646 && ints->missedMutations == 3 && ints->throttledTasks == 0 && ints->cpuWindowMs == 1e15);
    auto fractions = Coding::decodeStats({0, inf, 4294967295.5, nan, -inf, 0, 0, 0, 0, 0, 0});
    CHECK(fractions && fractions->tasks == 0 && fractions->memoryBytes == size_t(4294967295u) && fractions->ops == 0 && fractions->queries == 0);
    for (int n : {0, 1, 10, 12}) CHECK_MSG(!Coding::decodeStats(std::vector<double>(size_t(n), 1.0)), std::to_string(n) + " numbers");
}

/// Swift's testCanvasCodingRoundTripsAndSurvivesDamage on what C++ carries instead: both wallet methods, the fixture's accepted calls, and the SDK check on every decode.
TEST(remote_wallet_coding_round_trips_and_survives_damage) {
    using Coding = SandboxRemoteCoding;
    const std::string friendly = "EQCD39VS5jcptHL8vMjEXrzGaRcCVYto7HUn4bpAOg8xqB2N", raw = "0:83dfd552e63729b472fcbcc8c45ebcc6691702558b68ec7527e1ba403a0f31a8";
    const std::string boc = "te6cckEBAQEADgAAGAAAAABIZWxsbyEhIQ5sm/I=";
    std::vector<WalletCall> calls(5);
    calls[0].transaction.validUntil = 1900000000;
    calls[0].transaction.network = "-239";
    calls[0].transaction.from = raw;
    calls[0].transaction.messages = {WalletMessage{friendly, "1000000000", boc, boc}, WalletMessage{"UQCD39VS5jcptHL8vMjEXrzGaRcCVYto7HUn4bpAOg8x-_9I", "5", std::nullopt, std::nullopt}};
    calls[0].data = std::string("pay:\0x", 6);
    calls[1].transaction.messages = {WalletMessage{friendly, "0", std::nullopt, std::nullopt}};
    calls[1].data = "m";
    for (size_t i = 2; i < 5; i++) calls[i].method = WalletCall::Method::SignData;
    calls[2].payload.type = "text";
    calls[2].payload.text = "I agree \xD0\xB6";
    calls[2].payload.network = "-3";
    calls[2].payload.from = friendly;
    calls[2].data = "sign:1";
    calls[3].payload.type = "binary";
    calls[3].payload.bytes = "AAEC";
    calls[3].data = "b";
    calls[4].payload.type = "cell";
    calls[4].payload.schema = "message#_ text:string = Message;";
    calls[4].payload.cell = boc;
    calls[4].data = "c";
    int accepted = 0;
    uint64_t seed = 200;
    for (const WalletCall& call : calls) {
        const std::string wire = Coding::encodeWallet(call);
        auto back = Coding::decodeWallet(wire);
        auto parsed = Json::parse(wire);
        CHECK_MSG(back && *back == call && parsed && uniqueKeys(*parsed) && (*parsed)[0].asNumber() == 1 && (*parsed)[1].asString() == call.methodName(), wire);
        CHECK_MSG(decodablePrefixes(wire, Coding::decodeWallet) == 0 && boundedAtTheMessageLimit(wire, Coding::decodeWallet), wire);
        accepted += damageRoundTrips(wire, 1000, seed++, Coding::decodeWallet, Coding::encodeWallet);
    }
    CHECK_MSG(accepted > 0, std::to_string(accepted) + " damaged wallet calls accepted");

    auto root = Json::parse(readFile(std::string(TW_FIXTURES) + "/sdk-policy.json"));
    CHECK_MSG(root.has_value(), "fixture missing — run pnpm build:desktop");
    int fixtureCalls = 0;
    auto roundTrips = [](const WalletCall& call) { auto back = Coding::decodeWallet(Coding::encodeWallet(call)); return back && *back == call; };
    if (root) {
        for (const auto& c : (*root)["walletTransactions"].arr()) {
            std::optional<WalletTransaction> tx = sdk::checkWalletTransaction(expandRepeat(c["input"]));
            if (!tx) continue;
            WalletCall call;
            call.transaction = std::move(*tx);
            call.data = "tx";
            fixtureCalls++;
            CHECK_MSG(roundTrips(call), c["note"].isString() ? c["note"].asString() : c["input"].dump().substr(0, 100));
        }
        for (const auto& c : (*root)["signDataPayloads"].arr()) {
            std::optional<SignDataPayload> payload = sdk::checkSignDataPayload(expandRepeat(c["input"]));
            if (!payload) continue;
            WalletCall call;
            call.method = WalletCall::Method::SignData;
            call.payload = std::move(*payload);
            call.data = "sign";
            fixtureCalls++;
            CHECK_MSG(roundTrips(call), c["note"].isString() ? c["note"].asString() : c["input"].dump().substr(0, 100));
        }
        for (const auto& c : (*root)["bridge"].arr()) {
            std::optional<sdk::BridgeCall> call = sdk::checkBridgeCall(c["method"], c["args"]);
            if (!call || !call->wallet) continue;
            fixtureCalls++;
            CHECK_MSG(roundTrips(call->wallet->call), c["args"].dump().substr(0, 100));
        }
    }
    CHECK_MSG(fixtureCalls >= 23, std::to_string(fixtureCalls) + " accepted fixture wallet calls");

    // A forged or broken call is nothing; an escaped lone surrogate stays one (the SDK refuses it) instead of becoming a valid U+FFFD.
    for (const std::string& bad : {std::string("[1,\"signData\",{},\"data\"]"), std::string("[1,\"signMessage\",{\"type\":\"text\",\"text\":\"x\"},\"d\"]"),
                                   std::string("[0,\"signData\",{\"type\":\"text\",\"text\":\"x\"},\"d\"]"), std::string("[\"1\",\"signData\",{\"type\":\"text\",\"text\":\"x\"},\"d\"]"),
                                   std::string("[1,\"signData\",{\"type\":\"text\",\"text\":\"x\"}]"), std::string("[1,\"signData\",{\"type\":\"text\",\"text\":\"x\\u0000\"},\"d\"]"),
                                   std::string("[1,\"signData\",{\"type\":\"text\",\"text\":\"\\ud800\"},\"d\"]"), std::string("[1,\"signData\",{\"type\":\"text\",\"text\":\"x\"},\"\\udc00\"]"),
                                   std::string("[1,\"signData\",{\"type\":\"text\",\"text\":\"\xED\xA0\x80\"},\"d\"]"),
                                   "[1,\"sendTransaction\",{\"messages\":[{\"address\":\"" + raw + "\",\"amount\":\"1\"}]},\"x\"]", std::string("{}"), std::string("null"),
                                   std::string("[]"), std::string(""), std::string(100000, '[')})
        CHECK_MSG(!Coding::decodeWallet(bad), bad.substr(0, 100));
    const std::string lone = "[1,\"signData\",{\"type\":\"text\",\"text\":\"a\\ud800\"},\"d\"]";
    auto replaced = Json::parse(lone);
    CHECK(replaced && sdk::checkBridgeCall(Json("wallet"), *replaced) && !Coding::decodeWallet(lone));
    // A repeated key reads as the SDK check reads it (the first), and its damaged forms stay bounded and stable.
    const std::string repeated = "[1,\"signData\",{\"type\":\"text\",\"text\":\"first\",\"text\":\"last\"},\"d\"]";
    auto dup = Coding::decodeWallet(repeated);
    CHECK(dup && dup->payload.text == std::string("first"));
    damageRoundTrips(repeated, 1000, seed, Coding::decodeWallet, Coding::encodeWallet);
}

/// Swift's testBinaryPayloadCodingIsBounded on what C++ carries instead: the bot's data and the content safe area, round trips, wrong types, non-finite numbers, size and damage.
TEST(remote_data_and_safe_area_coding_round_trip_and_survive_damage) {
    using Coding = SandboxRemoteCoding;
    const std::vector<WidgetData> datas = {WidgetData{std::string("chat"), std::string("public")}, WidgetData{}, WidgetData{std::string(), std::nullopt},
                                           WidgetData{std::string("a\0b\x01", 4), std::string("\xED\xA0\x80\xFF")}, WidgetData{std::nullopt, std::string(2000, 'x')}};
    int accepted = 0;
    uint64_t seed = 300;
    for (const WidgetData& d : datas) {
        const std::string wire = Coding::encodeData(d);
        auto back = Coding::decodeData(wire);
        CHECK_MSG(back && *back == d, wire.substr(0, 100));
        CHECK_MSG(decodablePrefixes(wire, Coding::decodeData) == 0 && boundedAtTheMessageLimit(wire, Coding::decodeData), wire.substr(0, 100));
        accepted += damageRoundTrips(wire, 400, seed++, Coding::decodeData, Coding::encodeData);
    }
    CHECK(Coding::encodeData(datas[0]) == "{\"chatData\":\"chat\",\"publicData\":\"public\"}" && Coding::encodeData(WidgetData{}) == "{\"chatData\":null,\"publicData\":null}");
    CHECK(Coding::decodeData("{\"chatData\":1,\"publicData\":[\"x\"]}") == WidgetData{} && Coding::decodeData("{}") == WidgetData{});
    CHECK(Coding::decodeData("{\"chatData\":\"\\ud800\",\"chatData\":\"x\"}") == (WidgetData{std::string("\xED\xA0\x80"), std::nullopt}));
    for (const char* bad : {"[]", "null", "\"chatData\"", "", "{\"chatData\":\"a\"", "{\"chatData\":\"\x01\"}"}) CHECK_MSG(!Coding::decodeData(bad), bad);
    CHECK(!Coding::decodeData("{\"x\":" + nestedArrays(Coding::kMaxDepth) + "}") && Coding::decodeData("{\"x\":" + nestedArrays(Coding::kMaxDepth - 1) + "}"));

    const WidgetContentSafeArea area{1.5, 2, 3, 4, 5, 6};
    const std::string wire = Coding::encodeSafeArea(area);
    CHECK_MSG(wire == "{\"top\":1.5,\"right\":2,\"bottom\":3,\"left\":4,\"cornerLeft\":5,\"cornerRight\":6}", wire);
    auto back = Coding::decodeSafeArea(wire);
    CHECK(back && sameSafeArea(*back, area));
    CHECK(decodablePrefixes(wire, Coding::decodeSafeArea) == 0 && boundedAtTheMessageLimit(wire, Coding::decodeSafeArea));
    accepted += damageRoundTrips(wire, 2000, seed, Coding::decodeSafeArea, Coding::encodeSafeArea);
    CHECK_MSG(accepted > 0, std::to_string(accepted) + " damaged data and safe areas accepted");
    // Not finite is 0 (JSON has no NaN); a negative or huge side passes as given, the shim and the renderer bound it.
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    auto odd = Coding::decodeSafeArea(Coding::encodeSafeArea(WidgetContentSafeArea{nan, inf, -inf, -3, 0, 1e300}));
    CHECK(odd && sameSafeArea(*odd, WidgetContentSafeArea{0, 0, 0, -3, 0, 1e300}));
    auto wrong = Coding::decodeSafeArea("{\"top\":\"1\",\"right\":null,\"bottom\":true,\"left\":1e999,\"cornerLeft\":-1e999,\"cornerRight\":[7]}");
    CHECK(wrong && sameSafeArea(*wrong, WidgetContentSafeArea{}));
    auto empty = Coding::decodeSafeArea("{}");
    auto twice = Coding::decodeSafeArea("{\"top\":1,\"top\":2}");
    CHECK(empty && sameSafeArea(*empty, WidgetContentSafeArea{}) && twice && twice->top == 1);
    for (const char* bad : {"[]", "null", "1", "", "{\"top\":1"}) CHECK_MSG(!Coding::decodeSafeArea(bad), bad);
}

// ───────────── client-mode engine (SandboxEngine over a SandboxRemoteChannel) ─────────────

/// Swift's RemoteEngineTests.Channel: the test is the engine's side; each call with its arguments and its thread, under a lock.
class FakeRemoteChannel final : public SandboxRemoteChannel {
public:
    /// Called on the calling thread after a call is recorded, outside the lock; set it while no input is on its way.
    std::function<void(const std::string& call)> onCall;
    /// `close` reports a disconnection from inside, as a transport may.
    bool disconnectOnClose = false;
    /// `attach` reports a disconnection from inside, as a transport that cannot reach the engine's side may.
    bool disconnectOnAttach = false;
    std::string probeAnswer = "{\"ok\":true}";

    void attach(std::shared_ptr<SandboxRemoteSink> sink) override {
        {
            std::lock_guard<std::mutex> l(mutex_);
            sink_ = sink;
        }
        if (disconnectOnAttach) sink->remoteDisconnected();
    }
    void start(const std::string& html, const SandboxConfig& config) override { record("start:" + html + ":" + SandboxRemoteCoding::encodeConfig(config)); }
    void event(const std::string& json) override { record("event:" + json); }
    void activationEvent(const std::string& json, int gesture) override { record("activation:" + std::to_string(gesture) + ":" + json); }
    void frame(double time) override { record("frame:" + Json::number(time)); }
    void viewport(double w, double h, double dpr) override { record("viewport:" + Json::number(w) + "," + Json::number(h) + "," + Json::number(dpr)); }
    void resize(const std::string& entriesJson) override { record("resize:" + entriesJson); }
    void theme(const std::string& scheme, const std::map<std::string, std::string>& params) override {
        std::string call = "theme:" + scheme;
        for (const auto& [key, value] : params) call += ":" + key + "=" + value;
        record(call);
    }
    void contentSafeArea(const WidgetContentSafeArea& area) override { record("safeArea:" + SandboxRemoteCoding::encodeSafeArea(area)); }
    void setData(const WidgetData& data) override { record("data:" + SandboxRemoteCoding::encodeData(data)); }
    void setVisible(bool visible) override { record(visible ? "visible:true" : "visible:false"); }
    void pressBegan() override { record("press"); }
    void pressEnded() override { record("release"); }
    void chargeValidator(double ms) override { record("charge:" + Json::number(ms)); }
    void stop(const std::string& reason) override { record("stop:" + reason); }
    void answer(int64_t id, const SandboxRemoteOutcome& outcome) override {
        std::lock_guard<std::mutex> l(mutex_);
        answers_.emplace_back(id, outcome);
    }
    std::string probe(const std::string& code) override {
        record("probe:" + code);
        return probeAnswer;
    }
    void close() override {
        std::shared_ptr<SandboxRemoteSink> sink;
        {
            std::lock_guard<std::mutex> l(mutex_);
            closes_++;
            sink = sink_;
        }
        if (disconnectOnClose && sink) sink->remoteDisconnected();
    }

    std::shared_ptr<SandboxRemoteSink> sink() {
        std::lock_guard<std::mutex> l(mutex_);
        return sink_;
    }
    std::vector<std::string> calls() {
        std::lock_guard<std::mutex> l(mutex_);
        return calls_;
    }
    std::vector<std::thread::id> threads() {
        std::lock_guard<std::mutex> l(mutex_);
        return threads_;
    }
    std::vector<std::pair<int64_t, SandboxRemoteOutcome>> answers() {
        std::lock_guard<std::mutex> l(mutex_);
        return answers_;
    }
    int closes() {
        std::lock_guard<std::mutex> l(mutex_);
        return closes_;
    }

private:
    void record(const std::string& call) {
        {
            std::lock_guard<std::mutex> l(mutex_);
            calls_.push_back(call);
            threads_.push_back(std::this_thread::get_id());
        }
        if (onCall) onCall(call);
    }

    std::mutex mutex_;
    std::shared_ptr<SandboxRemoteSink> sink_;
    std::vector<std::string> calls_;
    std::vector<std::thread::id> threads_;
    std::vector<std::pair<int64_t, SandboxRemoteOutcome>> answers_;
    int closes_ = 0;
};

/// Swift's RemoteEngineTests.Delegate: what a client-mode engine hands its delegate, in order, under a lock.
class RemoteRecorder final : public SandboxEngineDelegate {
public:
    /// Runs inside `onQuery`, between its "query:…" and "query-done" events; set it while no query is on its way.
    std::function<void(SandboxEngine&)> duringQuery;
    /// Runs inside `onOps` after its event; set it while no ops are on their way.
    std::function<void(SandboxEngine&)> duringOps;
    /// Runs inside `onTaskStarted` after its event; set it on the engine's thread or while no task is on its way.
    std::function<void(SandboxEngine&)> atBegin;
    /// What `onQuery` answers; set it while no query is on its way.
    std::string queryAnswer = "{\"answer\":1}";

    void onOps(SandboxEngine& engine, const std::string&) override {
        record("ops");
        if (duringOps) duringOps(engine);
    }
    QueryAnswer onQuery(SandboxEngine& engine, const std::string& json) override {
        record("query:" + json);
        if (duringQuery) duringQuery(engine);
        record("query-done");
        return {queryAnswer, std::nullopt};
    }
    void onListen(SandboxEngine&, const std::string&) override { record("listen"); }
    void onPreventDefault(SandboxEngine&, const std::string&) override { record("preventDefault"); }
    void onAnim(SandboxEngine&, const std::string&) override { record("anim"); }
    void onRequestFrame(SandboxEngine&) override { record("frame"); }
    void onLog(SandboxEngine&, const std::string& level, const std::string& text) override { record("log:" + level + ":" + std::to_string(text.size())); }
    void onDiag(SandboxEngine&, const std::string& kind, const std::string& detail) override {
        record("diag:" + kind);
        std::lock_guard<std::mutex> l(mutex_);
        diags_.emplace_back(kind, detail);
    }
    void onBridgeAction(SandboxEngine&, const WidgetAction& action) override { record("action:" + action.toJson().dump()); }
    void onBridge(SandboxEngine&, const std::string& method, const std::string& argsJson) override { record("bridge:" + method + ":" + argsJson); }
    void onBridgeCopyText(SandboxEngine&, const std::string& text) override { record("copy:" + text); }
    void onStorage(SandboxEngine&, const std::string& area, const StorageArea& values) override {
        record("storage:" + area);
        std::lock_guard<std::mutex> l(mutex_);
        commits_.emplace_back(area, values);
    }
    void onBridgeRequest(SandboxEngine&, const WidgetRequest& request, WidgetRequestResponder respond) override {
        record("request:" + request.data + (request.wallet ? ":wallet" : ""));
        std::lock_guard<std::mutex> l(mutex_);
        responders_.push_back(std::move(respond));
    }
    void onBridgeWallet(SandboxEngine&, const WalletCall& call, WidgetRequestResponder respond) override {
        record("wallet:" + std::string(call.methodName()) + ":" + call.data);
        std::lock_guard<std::mutex> l(mutex_);
        walletCalls_.push_back(call);
        responders_.push_back(std::move(respond));
    }
    void onResizeAnchor(SandboxEngine&, const std::string& edge) override { record("anchor:" + edge); }
    void onPublicDataWatch(SandboxEngine&, bool on) override { record(on ? "watch:true" : "watch:false"); }
    void onLoaded(SandboxEngine&) override { record("loaded"); }
    void onStats(SandboxEngine&, const SandboxStats& stats) override {
        record("stats");
        std::lock_guard<std::mutex> l(mutex_);
        stats_.push_back(stats);
    }
    void onDied(SandboxEngine&, const std::string& reason) override { record("died:" + reason); }
    void onTaskStarted(SandboxEngine& engine) override {
        record("begin");
        if (atBegin) atBegin(engine);
    }
    void onTaskCompleted(SandboxEngine&) override { record("end"); }

    /// An event of the test's own (a provider's call) among the delegate's, with its thread.
    void record(const std::string& event) {
        std::lock_guard<std::mutex> l(mutex_);
        events_.push_back(event);
        threads_.push_back(std::this_thread::get_id());
    }

    std::vector<std::string> events() {
        std::lock_guard<std::mutex> l(mutex_);
        return events_;
    }
    std::vector<std::thread::id> threads() {
        std::lock_guard<std::mutex> l(mutex_);
        return threads_;
    }
    std::vector<std::pair<std::string, std::string>> diags() {
        std::lock_guard<std::mutex> l(mutex_);
        return diags_;
    }
    std::vector<SandboxStats> stats() {
        std::lock_guard<std::mutex> l(mutex_);
        return stats_;
    }
    std::vector<std::string> deaths() {
        std::vector<std::string> out;
        for (const std::string& e : events())
            if (e.rfind("died:", 0) == 0) out.push_back(e.substr(5));
        return out;
    }
    /// The events that start with `prefix`, in order.
    std::vector<std::string> eventsWith(const std::string& prefix) {
        std::vector<std::string> out;
        for (const std::string& e : events())
            if (e.rfind(prefix, 0) == 0) out.push_back(e);
        return out;
    }
    std::vector<std::pair<std::string, StorageArea>> commits() {
        std::lock_guard<std::mutex> l(mutex_);
        return commits_;
    }
    std::vector<WalletCall> walletCalls() {
        std::lock_guard<std::mutex> l(mutex_);
        return walletCalls_;
    }
    /// The responder of the `i`th request or wallet call the delegate got (one sequence for both), none before it came.
    std::optional<WidgetRequestResponder> responder(size_t i) {
        std::lock_guard<std::mutex> l(mutex_);
        if (i >= responders_.size()) return std::nullopt;
        return responders_[i];
    }
    /// Completes that call from the calling thread (nullopt: `unsupported()`); false when there is none.
    bool respond(size_t i, const std::optional<WidgetRequestOutcome>& outcome) {
        std::optional<WidgetRequestResponder> r = responder(i);
        if (!r) return false;
        if (outcome) (*r)(*outcome);
        else r->unsupported();
        return true;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> events_;
    std::vector<std::thread::id> threads_;
    std::vector<std::pair<std::string, std::string>> diags_;
    std::vector<SandboxStats> stats_;
    std::vector<std::pair<std::string, StorageArea>> commits_;
    std::vector<WalletCall> walletCalls_;
    std::vector<WidgetRequestResponder> responders_;
};

/// What RecordingIntl answers "alive" at once, unrecorded: on the dispatch thread, a probe of whether the client still has its engine ("" once its destructor let it go).
static const std::string kLivenessProbe = "{\"kind\":\"probe\"}";

/// A host's IntlProvider for a client: each request goes into `into` (set before the first) with its thread, then `during` runs (set it while no request is on its way) and `answer` comes back; `kLivenessProbe` is the exception.
class RecordingIntl final : public IntlProvider {
public:
    RemoteRecorder* into = nullptr;
    std::function<void()> during;
    std::string answer = "INTL";

    std::string format(const Json& request) override {
        if (request["kind"].asString() == "probe") return "alive";
        if (into) into->record("intl:" + request.dump());
        if (during) during();
        return answer;
    }
};

/// A wait a test opens from its own thread; `wait` gives up after 10 s, so a test that fails before opening it cannot strand the suite.
class Gate {
public:
    /// True when the test opened it in time.
    bool wait() {
        std::unique_lock<std::mutex> l(mutex_);
        waiters_++;
        cv_.notify_all();
        return cv_.wait_for(l, std::chrono::seconds(10), [&] { return open_; });
    }
    /// Until a thread waits on it, at most 5 s.
    bool waitedOn() {
        std::unique_lock<std::mutex> l(mutex_);
        return cv_.wait_for(l, std::chrono::seconds(5), [&] { return waiters_ > 0; });
    }
    void open() {
        {
            std::lock_guard<std::mutex> l(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int waiters_ = 0;
    bool open_ = false;
};

/// The exit status of a run a worker outlived its deadline in (`Background::finish`).
static constexpr int kWorkerOverranExit = 3;

/// A worker still running past its deadline may be using the test's fixtures: the process ends here, unwinding nothing.
[[noreturn]] static void stopTheRun(double seconds, const std::source_location& started) {
    fprintf(stderr, "  FAIL %s:%u: a worker still runs after %g s; the run stops here, with the fixtures it may use in place\n", started.file_name(), unsigned(started.line()), seconds);
    fflush(stdout);
    fflush(stderr);
    std::_Exit(kWorkerOverranExit);
}

/// `fn` on a thread the test owns, from now on: `within` looks for its value and gives up on nothing, `finish` waits for it to the end; the destructor finishes it too, so no fixture it uses goes first.
template <class R>
class Background {
public:
    /// The deadline of a worker the test did not finish: longer than a Gate's own 10 s.
    static constexpr double kDeadline = 15;

    explicit Background(std::function<R()> fn, std::source_location started = std::source_location::current())
        : started_(started), thread_([this, fn = std::move(fn)] { promise_.set_value(fn()); }) {}
    Background(const Background&) = delete;
    Background& operator=(const Background&) = delete;
    ~Background() {
        if (thread_.joinable()) finish(kDeadline);
    }

    /// Its value when it returns within `seconds`; nullopt while it still runs, which fails nothing: the worker stays the test's.
    std::optional<R> within(double seconds) {
        if (!value_ && future_.wait_for(std::chrono::duration<double>(seconds)) != std::future_status::ready) return std::nullopt;
        settle();
        return value_;
    }
    /// Its value, waited for up to `seconds`; past them the process ends at once (`stopTheRun`).
    R finish(double seconds) {
        if (!value_ && future_.wait_for(std::chrono::duration<double>(seconds)) != std::future_status::ready) stopTheRun(seconds, started_);
        settle();
        return *value_;
    }

private:
    void settle() {
        if (!value_) value_ = future_.get();
        if (thread_.joinable()) thread_.join();
    }

    std::source_location started_;
    std::promise<R> promise_;
    std::future<R> future_ = promise_.get_future();
    std::optional<R> value_;
    std::thread thread_;
};

static const std::string kEngineGone = "the widget engine process ended";

static SandboxConfig remoteConfig() {
    SandboxConfig c = testConfig();
    c.context.canQuery = true;
    return c;
}

static std::string startCall(const std::string& html, const SandboxConfig& config) { return "start:" + html + ":" + SandboxRemoteCoding::encodeConfig(config); }

static std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) out += (out.empty() ? "" : " | ") + item.substr(0, 120);
    return out;
}

/// Swift's `make`: a started client-mode engine over a fake channel, its recorder set first (the recorder outlives the engine).
struct RemoteRun {
    std::shared_ptr<FakeRemoteChannel> channel = std::make_shared<FakeRemoteChannel>();
    RemoteRecorder recorder;
    SandboxEngine engine;
    explicit RemoteRun(SandboxConfig config = remoteConfig(), bool withDelegate = true, bool start = true, std::shared_ptr<IntlProvider> intl = nullptr)
        : engine("<p>x</p>", std::move(config), channel, std::move(intl)) {
        if (withDelegate) engine.setDelegate(&recorder);
        if (start) engine.start();
        settle();
    }
    /// Swift's `settle`: everything queued on the dispatch thread so far has run.
    void settle() { engine.thread().sync([] {}); }
    SandboxRemoteSink& sink() { return *channel->sink(); }
    void tap(int gesture) {
        engine.activationEvent("{\"type\":\"click\"}", gesture);
        settle();
    }
};

/// `fn`'s value from a thread of its own, waited for up to `seconds`: a hang ends the run (`Background::finish`) instead of stalling it or unwinding what `fn` uses.
template <class R>
static R returnsWithin(double seconds, std::function<R()> fn, std::source_location started = std::source_location::current()) {
    Background<R> worker(std::move(fn), started);
    return worker.finish(seconds);
}

/// The default (no factory) back when a test that set one ends.
struct FactoryReset {
    ~FactoryReset() { SandboxEngine::setRemoteChannelFactory(nullptr); }
};

/// Swift's testInputsGoToTheChannelInOrder and more: every input with its arguments in the host's order on one dispatch thread (inputs before `start` keep their place, `start` goes once); `probe` asks from the caller's thread after them and delivers what came before its answer.
TEST(remote_inputs_reach_the_channel_in_order) {
    auto channel = std::make_shared<FakeRemoteChannel>();
    RemoteRecorder recorder;
    const SandboxConfig config = remoteConfig();
    SandboxEngine engine("<p>x</p>", config, channel);
    CHECK(engine.isRemote() && channel->sink());
    engine.setDelegate(&recorder);
    engine.contentSafeArea(WidgetContentSafeArea{1, 2, 3, 4, 5, 6});
    engine.setData(WidgetData{std::string("chat"), std::nullopt});
    engine.start();
    engine.start();
    engine.event("{\"type\":\"keydown\"}");
    engine.activationEvent("{\"type\":\"click\"}", 3);
    engine.frame(16.25);
    engine.viewport(320, 240.5, 2);
    engine.resize("[{\"id\":1}]");
    engine.theme("dark", {{"bg_color", "#000000"}, {"text_color", "#ffffff"}});
    engine.setVisible(false);
    engine.setVisible(false);
    engine.setVisible(true);
    engine.pressBegan();
    engine.pressEnded();
    engine.chargeValidator(12.5);
    engine.chargeValidator(0);
    engine.thread().sync([] {});
    const std::vector<std::string> expected = {
        "safeArea:{\"top\":1,\"right\":2,\"bottom\":3,\"left\":4,\"cornerLeft\":5,\"cornerRight\":6}",
        "data:{\"chatData\":\"chat\",\"publicData\":null}",
        startCall("<p>x</p>", config),
        "event:{\"type\":\"keydown\"}",
        "activation:3:{\"type\":\"click\"}",
        "frame:16.25",
        "viewport:320,240.5,2",
        "resize:[{\"id\":1}]",
        "theme:dark:bg_color=#000000:text_color=#ffffff",
        "visible:false",
        "visible:true",
        "press",
        "release",
        "charge:12.5",
    };
    std::vector<std::string> calls = channel->calls();
    CHECK_MSG(calls == expected, joined(calls));
    std::vector<std::thread::id> threads = channel->threads();
    const std::thread::id dispatch = threads.empty() ? std::thread::id() : threads[0];
    CHECK(dispatch != std::thread::id() && dispatch != std::this_thread::get_id());
    CHECK(std::all_of(threads.begin(), threads.end(), [&](std::thread::id id) { return id == dispatch; }));

    engine.event("{\"type\":\"keyup\"}");
    CHECK(engine.probe("1 + 1") == "{\"ok\":true}");
    calls = channel->calls();
    threads = channel->threads();
    CHECK_MSG(calls.size() == expected.size() + 2 && calls[expected.size()] == "event:{\"type\":\"keyup\"}" && calls.back() == "probe:1 + 1", joined(calls));
    CHECK(threads.back() == std::this_thread::get_id());
    channel->probeAnswer = std::string(SandboxEngine::kRemoteMessageLimit + 10, 'p');
    CHECK(engine.probe("2") == std::string(SandboxEngine::kRemoteMessageLimit, 'p'));
    CHECK(recorder.events().empty());
    // A death the engine's side reports while it answers reaches the delegate before probe returns.
    channel->probeAnswer = "{\"ok\":true}";
    channel->onCall = [&](const std::string& call) {
        if (call.rfind("probe:", 0) == 0) channel->sink()->remoteDied("said while probing");
    };
    CHECK(engine.probe("3") == "{\"ok\":true}");
    CHECK_MSG(recorder.events() == std::vector<std::string>{"died:said while probing"}, joined(recorder.events()));
    CHECK(engine.probe("4") == "ERROR no runtime" && channel->calls().back() == "probe:3");
}

/// Swift's testDisconnectKillsOnceAndSilencesTheRest: one death; nothing after it reaches the delegate or the channel, which closes once and is not told to stop.
TEST(remote_disconnect_kills_once_and_silences_the_rest) {
    RemoteRun r;
    r.sink().remoteDisconnected();
    r.sink().remoteDied("again");
    r.sink().remoteDisconnected();
    r.sink().remoteOps("[]");
    r.tap(1);
    r.sink().remoteCopyText("x");
    r.sink().remoteRequest(7, "{}", false);
    r.engine.event("{}");
    r.engine.setVisible(false);
    r.engine.pressBegan();
    r.engine.chargeValidator(3);
    r.engine.start();
    r.engine.terminate();
    r.engine.stop("later");
    r.settle();
    CHECK_MSG(r.recorder.events() == std::vector<std::string>{"died:" + kEngineGone}, joined(r.recorder.events()));
    CHECK_MSG(r.channel->calls() == std::vector<std::string>{startCall("<p>x</p>", remoteConfig())}, joined(r.channel->calls()));
    CHECK(r.channel->closes() == 1 && r.channel->answers().empty());
    CHECK(r.sink().remoteQuery("{}").json == "{\"error\":\"dead\"}");
    CHECK(r.engine.probe("1") == "ERROR no runtime");
}

/// Swift's testADeathBeforeTheDelegateReachesItLater: the death (bounded) waits for a delegate and reaches the first one set, once; with two set before a death, the later one hears it.
TEST(remote_death_before_the_delegate_reaches_it_later) {
    RemoteRecorder second;
    {
        RemoteRun r(remoteConfig(), false, false);
        r.sink().remoteDied("refused at open " + std::string(5000, 'x'));
        r.settle();
        CHECK(r.channel->closes() == 1 && r.channel->calls().empty());
        r.engine.setDelegate(&r.recorder);
        r.engine.setDelegate(&second);
        r.engine.setDelegate(&r.recorder);
        r.engine.start();
        r.settle();
        const std::vector<std::string> deaths = r.recorder.deaths();
        CHECK(deaths.size() == 1 && r.recorder.events().size() == 1);
        CHECK(!deaths.empty() && deaths[0].size() == SandboxEngine::kRemoteTextLimit && deaths[0].rfind("refused at open xxx", 0) == 0);
        CHECK(second.events().empty() && r.channel->calls().empty());
    }
    RemoteRecorder first;
    {
        RemoteRun r(remoteConfig(), false);
        r.engine.setDelegate(&first);
        r.engine.setDelegate(&r.recorder);
        r.sink().remoteDisconnected();
        r.settle();
        CHECK(first.events().empty() && r.recorder.deaths() == std::vector<std::string>{kEngineGone});
    }
}

/// Swift's testTerminateTellsTheEngineAndCloses: the engine's side hears the stop once, then the channel closes (a disconnection it reports from inside changes nothing); `stop` from a delegate callback acts at once, `terminate` after the callback.
TEST(remote_terminate_tells_the_engine_and_closes) {
    {
        RemoteRun r;
        r.channel->disconnectOnClose = true;
        r.engine.terminate();
        r.engine.terminate();
        r.engine.stop("later");
        r.engine.event("{}");
        r.settle();
        CHECK_MSG(r.recorder.events() == std::vector<std::string>{"died:terminated"}, joined(r.recorder.events()));
        CHECK_MSG((r.channel->calls() == std::vector<std::string>{startCall("<p>x</p>", remoteConfig()), "stop:terminated"}), joined(r.channel->calls()));
        CHECK(r.channel->closes() == 1);
    }
    {
        RemoteRun r;
        const std::string reason = "a counted cap " + std::string(5000, 'y');
        r.recorder.duringQuery = [&](SandboxEngine& engine) { engine.stop(reason); };
        CHECK(r.sink().remoteQuery("{\"q\":1}").json == "{\"answer\":1}");
        const std::string bounded = reason.substr(0, SandboxEngine::kRemoteTextLimit);
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"query:{\"q\":1}", "died:" + bounded, "query-done"}), joined(r.recorder.events()));
        CHECK((r.channel->calls() == std::vector<std::string>{startCall("<p>x</p>", remoteConfig()), "stop:" + bounded}) && r.channel->closes() == 1);
    }
    {
        RemoteRun r;
        r.recorder.duringQuery = [](SandboxEngine& engine) { engine.terminate(); };
        r.sink().remoteQuery("{}");
        r.settle();
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"query:{}", "query-done", "died:terminated"}), joined(r.recorder.events()));
    }
}

/// `make` keeps engines in process without a factory (Debug's switch puts them behind the loopback); a host's factory wins, also over the switch, and gets the resources; a factory with no channel gives a client that died, never an engine in process.
TEST(remote_factory_selects_the_engine_and_fails_closed) {
    auto channel = std::make_shared<FakeRemoteChannel>();
    int built = 0;
    std::shared_ptr<SandboxResources> given;
    FactoryReset reset;
    {
        RecordingHost host("light", RES);
        std::unique_ptr<SandboxEngine> engine = SandboxEngine::make("<p>x</p><script>console.log('ran here')</script>", testConfig(), RES);
        CHECK(engine->isRemote() == loopbackSwitch());
        engine->setDelegate(&host);
        engine->start();
        CHECK(host.waitLoaded(20) && host.waitUntil([&] { return host.hasLog("ran here") && !host.died; }));
    }
    SandboxEngine::setRemoteChannelFactory([&](std::shared_ptr<SandboxResources> resources) -> std::shared_ptr<SandboxRemoteChannel> {
        built++;
        given = std::move(resources);
        return channel;
    });
    {
        RemoteRecorder recorder;
        std::unique_ptr<SandboxEngine> engine = SandboxEngine::make("<p>x</p>", remoteConfig(), RES);
        CHECK(engine->isRemote() && built == 1 && given == RES && channel->sink());
        engine->setDelegate(&recorder);
        engine->start();
        engine->thread().sync([] {});
        CHECK(channel->calls() == std::vector<std::string>{startCall("<p>x</p>", remoteConfig())} && recorder.events().empty());
    }
    CHECK(channel->closes() == 1);
    SandboxEngine::setRemoteChannelFactory([&](std::shared_ptr<SandboxResources>) {
        built++;
        return std::shared_ptr<SandboxRemoteChannel>();
    });
    {
        RemoteRecorder recorder;
        std::unique_ptr<SandboxEngine> engine = SandboxEngine::make("<p>x</p><script>console.log('ran here')</script>", remoteConfig(), RES);
        CHECK(engine->isRemote() && built == 2);
        engine->start();
        engine->event("{}");
        engine->setDelegate(&recorder);
        engine->thread().sync([] {});
        CHECK_MSG(recorder.events() == std::vector<std::string>{"died:" + kEngineGone}, joined(recorder.events()));
        CHECK(engine->probe("1") == "ERROR no runtime");
    }
    SandboxEngine::setRemoteChannelFactory(nullptr);
    CHECK(SandboxEngine::make("", testConfig(), RES)->isRemote() == loopbackSwitch() && built == 2);
}

/// A transport may keep the sink after the engine is gone: every call returns at once with a dead or empty answer and reaches nothing; a channel closed by the destructor may report its disconnection from inside.
TEST(remote_sink_calls_after_the_engine_are_ignored) {
    auto channel = std::make_shared<FakeRemoteChannel>();
    channel->disconnectOnClose = true;
    RemoteRecorder recorder;
    {
        SandboxEngine engine("<p>x</p>", remoteConfig(), channel);
        engine.setDelegate(&recorder);
        engine.start();
        engine.thread().sync([] {});
    }
    CHECK(channel->closes() == 1);
    std::shared_ptr<SandboxRemoteSink> sink = channel->sink();
    CHECK(sink != nullptr);
    if (!sink) return;
    sink->remoteTaskStarted();
    sink->remoteOps("[]");
    sink->remoteListen("[]");
    sink->remotePreventDefault("[]");
    sink->remoteAnim("[]");
    sink->remoteRequestsFrame();
    sink->remoteLog("log", "late");
    sink->remoteDiag("late", "late");
    sink->remoteBridge("openLink", "[\"https://example.com/\"]");
    sink->remoteCopyText("late");
    sink->remoteStorage("{\"local\":{}}");
    sink->remoteResizeAnchor("bottom");
    sink->remoteWatchesPublicData(true);
    sink->remoteLoaded();
    sink->remoteStats(SandboxStats{});
    sink->remoteTaskCompleted();
    sink->remoteRequest(1, "{}", false);
    sink->remoteWallet(2, "[]");
    CHECK(sink->remoteQuery("{}").json == "{\"error\":\"dead\"}");
    CHECK(sink->remoteIntl("{\"kind\":\"number\",\"value\":1}").empty());
    sink->remoteDied("late");
    sink->remoteDisconnected();
    CHECK_MSG(recorder.events().empty(), joined(recorder.events()));
    CHECK(channel->answers().empty() && channel->calls().size() == 1 && channel->closes() == 1);
}

/// A result the dispatch thread will never make, its queue stopped (WidgetHost's destructor joins it before the engine goes), still returns its caller with a dead answer: the closure owns its promise, and dropping it breaks the wait.
TEST(remote_results_dropped_by_a_stopped_queue_unblock_their_callers) {
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool blocked = false, released = false;
        RemoteRun r;
        r.channel->onCall = [&](const std::string& call) {
            if (call != "event:{\"block\":true}") return;
            std::unique_lock<std::mutex> l(mutex);
            blocked = true;
            cv.notify_all();
            cv.wait(l, [&] { return released; });
        };
        r.engine.event("{\"block\":true}");
        {
            std::unique_lock<std::mutex> l(mutex);
            CHECK(cv.wait_for(l, std::chrono::seconds(5), [&] { return blocked; }));
        }
        r.engine.thread().stop();
        // Released just before the query is queued: behind the stop it is cleared, after it refused; either way it returns.
        const std::string answer = returnsWithin<std::string>(5, [&] {
            {
                std::lock_guard<std::mutex> l(mutex);
                released = true;
            }
            cv.notify_all();
            return r.sink().remoteQuery("{}").json + "|" + r.sink().remoteIntl("{\"kind\":\"number\",\"value\":1}");
        });
        CHECK_MSG(answer == "{\"error\":\"dead\"}|", answer);
        CHECK(r.recorder.events().empty());
    }
    {
        RemoteRun r;
        r.engine.thread().join();
        const std::string query = returnsWithin<std::string>(5, [&] { return r.sink().remoteQuery("{}").json; });
        const std::string intl = returnsWithin<std::string>(5, [&] { return r.sink().remoteIntl("{\"kind\":\"number\",\"value\":1}"); });
        const std::string probe = returnsWithin<std::string>(5, [&] { return r.engine.probe("0"); });
        CHECK_MSG(query == "{\"error\":\"dead\"}", query);
        CHECK_MSG(intl.empty(), intl);
        CHECK_MSG(probe == "ERROR no runtime", probe);
        CHECK(r.recorder.events().empty());
    }
    // On the dispatch thread itself a value is made at once (a channel must not ask from inside a call, but it cannot hang the engine), and a probe from there is refused.
    {
        RemoteRun r;
        std::string inlineAnswer, probed;
        r.recorder.duringQuery = [&](SandboxEngine& engine) { probed = engine.probe("1"); };
        r.channel->onCall = [&](const std::string& call) {
            if (call == "event:{\"ask\":true}") inlineAnswer = r.sink().remoteQuery("{\"inline\":1}").json;
        };
        r.engine.event("{\"ask\":true}");
        r.settle();
        CHECK(inlineAnswer == "{\"answer\":1}" && probed == "ERROR no runtime");
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"query:{\"inline\":1}", "query-done"}), joined(r.recorder.events()));
        const std::vector<std::string> calls = r.channel->calls();
        CHECK(std::none_of(calls.begin(), calls.end(), [](const std::string& c) { return c.rfind("probe:", 0) == 0; }));
    }
}

static const std::string kNumberRequest = "{\"kind\":\"number\",\"locale\":\"en-US\",\"opts\":{},\"value\":1234.5}";

/// `json` padded with spaces to `size` bytes: the same JSON.
static std::string paddedTo(std::string json, size_t size) {
    json.resize(size, ' ');
    return json;
}

static bool has(const std::vector<std::string>& events, const std::string& event) { return std::find(events.begin(), events.end(), event) != events.end(); }

/// A delegate's task events among the rest: begins, ends and deaths.
static std::vector<std::string> taskEvents(RemoteRecorder& recorder) {
    std::vector<std::string> out;
    for (const std::string& e : recorder.events())
        if (e == "begin" || e == "end" || e.rfind("died:", 0) == 0) out.push_back(e);
    return out;
}

/// Swift's testOrderIsKeptAcrossQueriesAndTextIsBounded and more: a query and an Intl request wait for every call before them, every callback and the host's provider run on the dispatch thread, a log is cut where it arrives.
TEST(remote_order_is_kept_across_queries_and_text_is_bounded) {
    auto intl = std::make_shared<RecordingIntl>();
    RemoteRun r(remoteConfig(), true, true, intl);
    intl->into = &r.recorder;
    r.sink().remoteOps("[]");
    r.sink().remoteListen("{\"l\":1}");
    r.sink().remotePreventDefault("{\"p\":1}");
    r.sink().remoteAnim("{\"a\":1}");
    const QueryAnswer answer = r.sink().remoteQuery("{\"q\":\"rect\"}");
    r.sink().remoteLog("weird", std::string(100000, 'y'));
    const std::string formatted = r.sink().remoteIntl(kNumberRequest);
    r.sink().remoteRequestsFrame();
    r.sink().remoteLog("warn", std::string(SandboxEngine::kRemoteTextLimit, 'w'));
    r.settle();
    CHECK(answer.json == "{\"answer\":1}" && !answer.binary && formatted == "INTL");
    const std::vector<std::string> expected = {"ops", "listen", "preventDefault", "anim", "query:{\"q\":\"rect\"}", "query-done", "log:log:4000",
                                               "intl:" + Json::parse(kNumberRequest)->dump(), "frame", "log:warn:4000"};
    CHECK_MSG(r.recorder.events() == expected, joined(r.recorder.events()));
    const std::vector<std::thread::id> threads = r.recorder.threads();
    const std::vector<std::thread::id> channelThreads = r.channel->threads();
    CHECK(threads.size() == expected.size() && !channelThreads.empty() && threads[0] == channelThreads[0] && threads[0] != std::this_thread::get_id());
    CHECK(std::all_of(threads.begin(), threads.end(), [&](std::thread::id id) { return id == threads[0]; }));
    // Without a provider of the host's the client formats with BasicIntl.
    RemoteRun plain;
    const std::string basic = plain.sink().remoteIntl(kNumberRequest);
    CHECK_MSG(basic == "1,234.5", basic);
}

/// Swift's testHiddenWidgetGetsNoFrameRequests: the host's visibility decides whether a frame request reaches the delegate, from the start and after each change; a dead widget gets none.
TEST(remote_hidden_widget_gets_no_frame_requests) {
    SandboxConfig hidden = remoteConfig();
    hidden.visible = false;
    RemoteRun r(hidden);
    r.sink().remoteRequestsFrame();
    r.engine.setVisible(true);
    r.sink().remoteRequestsFrame();
    r.engine.setVisible(false);
    r.sink().remoteRequestsFrame();
    r.sink().remoteRequestsFrame();
    r.engine.setVisible(true);
    r.sink().remoteRequestsFrame();
    r.sink().remoteDisconnected();
    r.sink().remoteRequestsFrame();
    r.settle();
    CHECK_MSG((r.recorder.events() == std::vector<std::string>{"frame", "frame", "died:" + kEngineGone}), joined(r.recorder.events()));
}

/// Swift's testDeathInsideATaskStillEndsTheTask, and the engine's own tasks: a death inside a task reaches the delegate before that task's end, the end comes once and nothing of a later task comes; a stop from the begin runs none of the task's widget code.
TEST(remote_death_inside_a_task_still_ends_the_task) {
    {
        RemoteRun r;
        r.sink().remoteTaskStarted();
        r.sink().remoteOps("[]");
        r.sink().remoteDied("CPU");
        r.sink().remoteTaskCompleted();
        r.sink().remoteTaskStarted();
        r.sink().remoteTaskCompleted();
        r.settle();
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"begin", "ops", "died:CPU", "end"}), joined(r.recorder.events()));
    }
    {
        RemoteRun r;
        r.sink().remoteTaskStarted();
        r.sink().remoteDisconnected();
        r.sink().remoteTaskCompleted();
        r.settle();
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"begin", "died:" + kEngineGone, "end"}), joined(r.recorder.events()));
    }
    // The engine itself: its CPU limit stops the task of a click, after the two tasks of the boot.
    {
        SandboxConfig config = testConfig();
        config.cpuLimitMs = 200;
        RemoteRecorder recorder;
        SandboxEngine engine("<p id=\"p\">x</p><script>document.addEventListener('click', function () { for (;;) {} });</script>", config, RES);
        engine.setDelegate(&recorder);
        engine.start();
        CHECK(eventually([&] { return has(recorder.events(), "loaded"); }, 20));
        engine.event("{\"type\":\"click\",\"targetId\":2}");
        CHECK(eventually([&] { return !recorder.deaths().empty(); }, 10));
        engine.thread().sync([] {});
        const std::vector<std::string> tasks = taskEvents(recorder);
        CHECK_MSG(tasks.size() == 7 && std::vector<std::string>(tasks.begin(), tasks.begin() + 5) == (std::vector<std::string>{"begin", "end", "begin", "end", "begin"}) &&
                      tasks[5].rfind("died:CPU limit exceeded", 0) == 0 && tasks[6] == "end",
                  joined(tasks));
    }
    // A delegate that stops the engine from a task's begin.
    {
        RemoteRecorder recorder;
        SandboxEngine engine("<p id=\"p\">x</p><script>document.addEventListener('click', function () { console.log('clicked'); });</script>", testConfig(), RES);
        engine.setDelegate(&recorder);
        engine.start();
        CHECK(eventually([&] { return has(recorder.events(), "loaded"); }, 20));
        engine.event("{\"type\":\"click\",\"targetId\":2}");
        engine.thread().sync([&] { recorder.atBegin = [](SandboxEngine& stopped) { stopped.stop("stopped at the begin"); }; });
        engine.event("{\"type\":\"click\",\"targetId\":2}");
        engine.event("{\"type\":\"click\",\"targetId\":2}");
        engine.thread().sync([] {});
        const std::vector<std::string> events = recorder.events();
        CHECK_MSG(std::count(events.begin(), events.end(), "log:log:7") == 1, joined(events));
        const std::vector<std::string> tasks = taskEvents(recorder);
        CHECK_MSG(tasks.size() == 9 && std::vector<std::string>(tasks.begin() + 6, tasks.end()) == (std::vector<std::string>{"begin", "died:stopped at the begin", "end"}), joined(tasks));
        CHECK(engine.probe("1") == "ERROR no runtime");
    }
    // Stopped at the boot's begin: not even the shim's bundle runs (its `boot` diagnostic never comes), and the widget never loads.
    {
        RemoteRecorder recorder;
        recorder.atBegin = [](SandboxEngine& stopped) { stopped.stop("stopped at the begin"); };
        SandboxEngine engine("<script>console.log('ran')</script>", testConfig(), RES);
        engine.setDelegate(&recorder);
        engine.start();
        engine.thread().sync([] {});
        std::vector<std::string> events = recorder.events();
        events.erase(std::remove(events.begin(), events.end(), "stats"), events.end());
        CHECK_MSG((events == std::vector<std::string>{"begin", "died:stopped at the begin", "end"}), joined(events));
    }
}

/// The delegate hears a task's end only after it heard that task's begin: not for an end alone, a second begin, a begin before the delegate or given to an earlier one, nor a second end after a death.
TEST(remote_task_ends_match_the_begins_the_delegate_heard) {
    {
        RemoteRun r;
        r.sink().remoteTaskCompleted();
        r.sink().remoteTaskStarted();
        r.sink().remoteTaskStarted();
        r.sink().remoteTaskCompleted();
        r.sink().remoteTaskCompleted();
        r.sink().remoteTaskStarted();
        r.sink().remoteTaskCompleted();
        r.settle();
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"begin", "end", "begin", "end"}), joined(r.recorder.events()));
    }
    {
        RemoteRecorder first;
        RemoteRun r(remoteConfig(), false);
        r.sink().remoteTaskStarted();
        r.engine.setDelegate(&first);
        r.sink().remoteTaskCompleted();
        r.sink().remoteTaskStarted();
        r.engine.setDelegate(&r.recorder);
        r.engine.setDelegate(&r.recorder);
        r.sink().remoteTaskCompleted();
        r.sink().remoteTaskStarted();
        r.sink().remoteTaskCompleted();
        r.settle();
        CHECK_MSG(first.events() == std::vector<std::string>{"begin"}, joined(first.events()));
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"begin", "end"}), joined(r.recorder.events()));
    }
    {
        RemoteRun r(remoteConfig(), false);
        r.sink().remoteTaskStarted();
        r.sink().remoteDied("CPU");
        r.sink().remoteTaskCompleted();
        r.settle();
        r.engine.setDelegate(&r.recorder);
        r.sink().remoteTaskStarted();
        r.sink().remoteTaskCompleted();
        r.settle();
        CHECK_MSG(r.recorder.events() == std::vector<std::string>{"died:CPU"}, joined(r.recorder.events()));
    }
    {
        RemoteRun r;
        r.sink().remoteTaskStarted();
        r.settle();
        r.engine.stop("a counted cap");
        r.engine.terminate();
        r.sink().remoteTaskCompleted();
        r.sink().remoteDisconnected();
        r.settle();
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"begin", "died:a counted cap", "end"}), joined(r.recorder.events()));
        CHECK((r.channel->calls() == std::vector<std::string>{startCall("<p>x</p>", remoteConfig()), "stop:a counted cap"}) && r.channel->closes() == 1);
    }
    {
        RemoteRun r;
        r.recorder.atBegin = [](SandboxEngine& engine) { engine.stop("stopped at the begin"); };
        r.sink().remoteTaskStarted();
        r.sink().remoteOps("[]");
        r.sink().remoteTaskCompleted();
        r.settle();
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"begin", "died:stopped at the begin", "end"}), joined(r.recorder.events()));
        CHECK((r.channel->calls() == std::vector<std::string>{startCall("<p>x</p>", remoteConfig()), "stop:stopped at the begin"}) && r.channel->closes() == 1);
    }
}

/// Every message is measured where it arrives: at the limit it reaches the delegate or the host's provider, a byte over it does not (ops, listen, preventDefault and anim as `protocol-violation`, a query as `invalid query`, Intl as ""), and log and diagnostic texts are cut.
TEST(remote_messages_are_bounded_before_they_are_queued) {
    auto intl = std::make_shared<RecordingIntl>();
    RemoteRun r(remoteConfig(), true, true, intl);
    intl->into = &r.recorder;
    const size_t limit = SandboxEngine::kRemoteMessageLimit;
    r.sink().remoteOps(paddedTo("[]", SandboxEngine::kRemoteOpsLimit));
    r.sink().remoteOps(paddedTo("[]", SandboxEngine::kRemoteOpsLimit + 1));
    r.sink().remoteListen(paddedTo("{}", limit));
    r.sink().remoteListen(paddedTo("{}", limit + 1));
    r.sink().remotePreventDefault(paddedTo("{}", limit));
    r.sink().remotePreventDefault(paddedTo("{}", limit + 1));
    r.sink().remoteAnim(paddedTo("{}", limit));
    r.sink().remoteAnim(paddedTo("{}", limit + 1));
    const std::string query = paddedTo("{\"q\":1}", limit);
    CHECK(r.sink().remoteQuery(query).json == "{\"answer\":1}");
    CHECK(r.sink().remoteQuery(query + " ").json == "{\"error\":\"invalid query\"}");
    CHECK(r.sink().remoteIntl(paddedTo(kNumberRequest, limit)) == "INTL");
    for (const std::string& malformed : {paddedTo(kNumberRequest, limit + 1), std::string("{\"kind\":"), std::string("[]"), std::string("\"number\""), std::string()})
        CHECK_MSG(r.sink().remoteIntl(malformed).empty(), malformed.substr(0, 40));
    intl->answer = std::string(limit, 'n');
    CHECK(r.sink().remoteIntl(kNumberRequest).size() == limit);
    intl->answer = std::string(limit + 1, 'n');
    CHECK(r.sink().remoteIntl(kNumberRequest).empty());
    r.sink().remoteLog("log", std::string(SandboxEngine::kRemoteTextLimit + 1, 'l'));
    r.sink().remoteLog("trace", "t");
    const std::string kind(SandboxEngine::kRemoteNameLimit, 'k'), detail(SandboxEngine::kRemoteTextLimit, 'd');
    r.sink().remoteDiag(kind, detail);
    r.sink().remoteDiag(kind + "k", detail + "d");
    r.settle();
    const std::string asked = "intl:" + Json::parse(kNumberRequest)->dump();
    const std::vector<std::string> expected = {"ops", "diag:protocol-violation", "listen", "diag:protocol-violation", "preventDefault", "diag:protocol-violation", "anim",
                                               "diag:protocol-violation", "query:" + query, "query-done", asked, asked, asked, "log:log:4000", "log:log:1", "diag:" + kind,
                                               "diag:" + kind};
    CHECK_MSG(r.recorder.events() == expected, joined(r.recorder.events()));
    const std::vector<std::pair<std::string, std::string>> diags = {{"protocol-violation", "an ops batch over 32 MB"}, {"protocol-violation", "listen over 1 MB"},
                                                                    {"protocol-violation", "preventDefault over 1 MB"}, {"protocol-violation", "anim over 1 MB"},
                                                                    {kind, detail}, {kind, detail}};
    CHECK(r.recorder.diags() == diags);
}

/// Bridge calls, copies, commits, requests and wallet calls over their sizes are refused before they are queued: Swift's diagnostics (none in a preview for a copy or a commit) and NOT_ALLOWED answers, at most 20 reports per counter, protocol violations counted with the bridge's.
TEST(remote_oversized_calls_are_refused_and_reports_are_capped) {
    const std::string longName(SandboxEngine::kRemoteNameLimit + 1, 'm');
    const std::string bigCommit = paddedTo("{\"local\":{}}", sdk::MAX_STORAGE_COMMIT_BYTES + 1);
    {
        RemoteRun r;
        r.sink().remoteBridge(longName, "[]");
        r.sink().remoteBridge("openLink", paddedTo("[\"https://example.com/\"]", SandboxEngine::kRemoteMessageLimit + 1));
        r.sink().remoteCopyText(std::string(size_t(sdk::MAX_COPY_TEXT_LENGTH) + 1, 'c'));
        r.sink().remoteCopyText("\xED\xA0\x80");
        r.sink().remoteStorage(bigCommit);
        r.sink().remoteRequest(7, std::string(size_t(sdk::MAX_REQUEST_DATA_BYTES) + 1, 'r'), false);
        r.sink().remoteWallet(8, paddedTo("[1,\"signData\",{\"type\":\"text\",\"text\":\"x\"},\"d\"]", SandboxEngine::kRemoteMessageLimit + 1));
        for (int i = 0; i < 25; i++) r.sink().remoteResizeAnchor("left");
        for (int i = 0; i < 25; i++) r.sink().remoteStorage(bigCommit);
        r.settle();
        const std::vector<std::pair<std::string, std::string>> diags = r.recorder.diags();
        const std::vector<std::pair<std::string, std::string>> first = {{"bridge-rejected", longName.substr(0, 64) + ": rejected by the host (unknown method or malformed arguments)"},
                                                                         {"bridge-rejected", "openLink: rejected by the host (unknown method or malformed arguments)"},
                                                                         {"bridge-rejected", "copyText: rejected by the host"},
                                                                         {"bridge-rejected", "copyText: rejected by the host"},
                                                                         {"storage-rejected", "a storage commit the host refused"},
                                                                         {"protocol-violation", "resize anchor left"}};
        CHECK(diags.size() == 40 && std::equal(first.begin(), first.end(), diags.begin()));
        auto count = [&](const std::string& kind) { return std::count_if(diags.begin(), diags.end(), [&](const auto& d) { return d.first == kind; }); };
        CHECK(count("bridge-rejected") == 4 && count("protocol-violation") == 16 && count("storage-rejected") == 20);
        const auto answers = r.channel->answers();
        const SandboxRemoteOutcome refused = SandboxRemoteOutcome::failed(WidgetRequestFailure::NotAllowed);
        CHECK(answers.size() == 2 && answers[0].first == 7 && answers[0].second == refused && answers[1].first == 8 && answers[1].second == refused);
    }
    {
        SandboxConfig preview = remoteConfig();
        preview.context.preview = true;
        RemoteRun r(preview);
        r.sink().remoteCopyText(std::string(size_t(sdk::MAX_COPY_TEXT_LENGTH) + 1, 'c'));
        r.sink().remoteStorage(bigCommit);
        r.sink().remoteBridge(longName, "[]");
        r.sink().remoteResizeAnchor("left");
        r.settle();
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"diag:bridge-rejected", "diag:protocol-violation"}), joined(r.recorder.events()));
    }
}

static const std::string kEngineFlooded = "the widget engine process sent more than the host keeps queued";

/// `holder` is a client-mode SandboxEngine or a WidgetHost over one.
template <class Holder>
static bool backlogIs(Holder& holder, size_t bytes, size_t calls) {
    const SandboxEngine::RemoteBacklog b = holder.remoteBacklog();
    return b.bytes == bytes && b.calls == calls;
}

template <class Holder>
static std::string backlogText(Holder& holder) {
    const SandboxEngine::RemoteBacklog b = holder.remoteBacklog();
    return std::to_string(b.bytes) + " bytes, " + std::to_string(b.calls) + " calls";
}

/// Holds `engine`'s dispatch thread on `gate` (declared before the engine, so it outlives the task) until the test opens it: what the engine's side sends meanwhile waits in the client.
static bool holdDispatch(SandboxEngine& engine, Gate& gate) {
    engine.thread().async([&gate] { gate.wait(); });
    return gate.waitedOn();
}

/// While the delegate's thread is held, full ops batches fill the byte bound exactly; two bytes more kill the widget, once, after the two batches, and from then on the sink takes nothing (64 more full batches would hold 2 GiB) and answers at once without waiting for the held thread. The channel hears one stop and closes once; later calls change nothing.
TEST(remote_a_backlog_past_its_bytes_fails_the_widget_once) {
    Gate gate;
    RemoteRun r;
    r.channel->disconnectOnClose = true;
    SandboxRemoteSink& sink = r.sink();
    CHECK(backlogIs(r.engine, 0, 0) && SandboxEngine::kRemoteQueuedBytes == 2 * SandboxEngine::kRemoteOpsLimit);
    CHECK(holdDispatch(r.engine, gate));
    const std::string full = paddedTo("[]", SandboxEngine::kRemoteOpsLimit);
    sink.remoteOps(full);
    sink.remoteOps(full);
    CHECK_MSG(backlogIs(r.engine, SandboxEngine::kRemoteQueuedBytes, 2), backlogText(r.engine));
    sink.remoteOps("[]");
    CHECK_MSG(backlogIs(r.engine, SandboxEngine::kRemoteQueuedBytes, 2), backlogText(r.engine));
    for (int i = 0; i < 64; i++) sink.remoteOps(full);
    sink.remoteTaskStarted();
    sink.remoteLog("log", "after");
    sink.remoteListen(paddedTo("{}", SandboxEngine::kRemoteMessageLimit + 1));
    sink.remoteBridge(std::string(SandboxEngine::kRemoteNameLimit + 1, 'm'), "[]");
    sink.remoteStorage("{\"local\":{\"a\":\"b\"}}");
    sink.remoteRequest(9, "{}", false);
    sink.remoteDied("later");
    sink.remoteDisconnected();
    const std::string answers = returnsWithin<std::string>(5, [&] { return sink.remoteQuery("{}").json + "|" + sink.remoteIntl(kNumberRequest); });
    CHECK_MSG(answers == "{\"error\":\"dead\"}|", answers);
    CHECK_MSG(backlogIs(r.engine, SandboxEngine::kRemoteQueuedBytes, 2), backlogText(r.engine));
    CHECK(r.recorder.events().empty());
    gate.open();
    r.settle();
    CHECK_MSG((r.recorder.events() == std::vector<std::string>{"ops", "ops", "died:" + kEngineFlooded}), joined(r.recorder.events()));
    CHECK_MSG((r.channel->calls() == std::vector<std::string>{startCall("<p>x</p>", remoteConfig()), "stop:" + kEngineFlooded}), joined(r.channel->calls()));
    CHECK(r.channel->closes() == 1 && r.channel->answers().empty());
    CHECK_MSG(backlogIs(r.engine, 0, 0), backlogText(r.engine));
    sink.remoteOps("[]");
    sink.remoteLoaded();
    sink.remoteDisconnected();
    r.settle();
    CHECK(r.recorder.events().size() == 3 && backlogIs(r.engine, 0, 0) && r.channel->closes() == 1);
}

/// The call bound the same way with small messages, rejection reports among them: exactly `kRemoteQueuedCalls` are held and delivered in order (the reports capped at 20, as always), the next one kills the widget once after them, and nothing after it is taken, not even a report.
TEST(remote_a_flood_of_small_messages_fails_the_widget_once) {
    Gate gate;
    RemoteRun r;
    SandboxRemoteSink& sink = r.sink();
    CHECK(holdDispatch(r.engine, gate));
    const size_t limit = SandboxEngine::kRemoteQueuedCalls;
    for (size_t i = 0; i < limit; i++) {
        switch (i % 4) {
        case 0: sink.remoteLog("log", "x"); break;
        case 1: sink.remoteResizeAnchor("left"); break;
        case 2: sink.remoteRequestsFrame(); break;
        default: sink.remoteBridge("nope", "[]"); break;
        }
    }
    const SandboxEngine::RemoteBacklog full = r.engine.remoteBacklog();
    CHECK_MSG(full.calls == limit && full.bytes > 0 && full.bytes < SandboxEngine::kRemoteQueuedBytes, backlogText(r.engine));
    sink.remoteLoaded();
    for (int i = 0; i < 1000; i++) {
        sink.remoteLog("log", "after");
        sink.remoteResizeAnchor("left");
    }
    const SandboxEngine::RemoteBacklog after = r.engine.remoteBacklog();
    CHECK_MSG(after.calls == full.calls && after.bytes == full.bytes, backlogText(r.engine));
    gate.open();
    r.settle();
    const std::vector<std::string> events = r.recorder.events();
    const auto count = [&](const std::string& event) { return size_t(std::count(events.begin(), events.end(), event)); };
    const size_t reports = std::count_if(events.begin(), events.end(), [](const std::string& e) { return e.rfind("diag:", 0) == 0; });
    CHECK(count("log:log:1") == limit / 4 && count("frame") == limit / 4 && reports == size_t(sdk::MAX_REJECTION_REPORTS));
    CHECK_MSG(events.size() == limit / 2 + reports + 1 && events.back() == "died:" + kEngineFlooded && count("loaded") == 0 && count("log:log:5") == 0, std::to_string(events.size()) + " events");
    CHECK(events.size() > 4 && events[0] == "log:log:1" && events[1] == "diag:protocol-violation" && events[2] == "frame" && events[3] == "diag:bridge-rejected");
    CHECK((r.channel->calls() == std::vector<std::string>{startCall("<p>x</p>", remoteConfig()), "stop:" + kEngineFlooded}) && r.channel->closes() == 1);
    CHECK(backlogIs(r.engine, 0, 0));
}

/// The control: a backlog at the bounds, not past them, is delivered in the engine's order with nothing dropped and nothing dying, and given back as it runs, so the same burst fits again; the sync calls then answer as usual.
TEST(remote_a_backlog_within_its_bounds_is_delivered_in_order_and_given_back) {
    Gate first, second;
    RemoteRun r;
    SandboxRemoteSink& sink = r.sink();
    const std::string full = paddedTo("[]", SandboxEngine::kRemoteOpsLimit);
    for (Gate* gate : {&first, &second}) {
        CHECK(holdDispatch(r.engine, *gate));
        sink.remoteTaskStarted();
        sink.remoteOps(full);
        sink.remoteRequestsFrame();
        sink.remoteOps(full);
        sink.remoteLoaded();
        sink.remoteTaskCompleted();
        CHECK_MSG(backlogIs(r.engine, SandboxEngine::kRemoteQueuedBytes, 6), backlogText(r.engine));
        gate->open();
        r.settle();
        CHECK_MSG(backlogIs(r.engine, 0, 0), backlogText(r.engine));
    }
    // Up to the call bound too: as many as it allows, held, then all delivered.
    Gate third;
    CHECK(holdDispatch(r.engine, third));
    for (size_t i = 0; i < SandboxEngine::kRemoteQueuedCalls; i++) sink.remoteRequestsFrame();
    CHECK_MSG(backlogIs(r.engine, 0, SandboxEngine::kRemoteQueuedCalls), backlogText(r.engine));
    third.open();
    r.settle();
    CHECK(sink.remoteQuery("{\"q\":1}").json == "{\"answer\":1}" && sink.remoteIntl(kNumberRequest) == "1,234.5");
    std::vector<std::string> expected;
    for (int round = 0; round < 2; round++) expected.insert(expected.end(), {"begin", "ops", "frame", "ops", "loaded", "end"});
    expected.insert(expected.end(), SandboxEngine::kRemoteQueuedCalls, "frame");
    expected.insert(expected.end(), {"query:{\"q\":1}", "query-done"});
    CHECK_MSG(r.recorder.events() == expected, std::to_string(r.recorder.events().size()) + " events");
    CHECK(r.recorder.deaths().empty() && r.channel->calls().size() == 1 && r.channel->closes() == 0 && backlogIs(r.engine, 0, 0));
}

/// What a stopped dispatch thread drops gives its place back as the thread clears it, and a call refused by the stopped thread at once; the destructor drains a full backlog in order (the accepted work, then the one death) and closes once, and later calls reach nothing.
TEST(remote_a_backlog_is_given_back_when_its_calls_are_dropped) {
    const std::string full = paddedTo("[]", SandboxEngine::kRemoteOpsLimit);
    {
        Gate gate;
        RemoteRun r;
        SandboxRemoteSink& sink = r.sink();
        CHECK(holdDispatch(r.engine, gate));
        r.engine.thread().stop();
        sink.remoteOps(full);
        sink.remoteLog("log", "x");
        CHECK_MSG(backlogIs(r.engine, SandboxEngine::kRemoteOpsLimit + 4, 2), backlogText(r.engine));
        gate.open();
        r.engine.thread().join();
        CHECK_MSG(backlogIs(r.engine, 0, 0), backlogText(r.engine));
        sink.remoteOps(full);
        sink.remoteLoaded();
        CHECK_MSG(backlogIs(r.engine, 0, 0), backlogText(r.engine));
        CHECK(r.recorder.events().empty());
    }
    {
        Gate gate;
        RemoteRecorder recorder;
        auto channel = std::make_shared<FakeRemoteChannel>();
        auto engine = std::make_unique<SandboxEngine>("<p>x</p>", remoteConfig(), channel);
        engine->setDelegate(&recorder);
        engine->start();
        std::shared_ptr<SandboxRemoteSink> sink = channel->sink();
        CHECK(holdDispatch(*engine, gate));
        for (int i = 0; i < 3; i++) sink->remoteOps(full);
        CHECK_MSG(backlogIs(*engine, SandboxEngine::kRemoteQueuedBytes, 2), backlogText(*engine));
        Background<bool> teardown([&] {
            engine.reset();
            return true;
        });
        gate.open();
        CHECK(teardown.finish(10));
        CHECK_MSG((recorder.events() == std::vector<std::string>{"ops", "ops", "died:" + kEngineFlooded}), joined(recorder.events()));
        // Closed by the death, then again (idempotent) by the destructor.
        CHECK((channel->calls() == std::vector<std::string>{startCall("<p>x</p>", remoteConfig()), "stop:" + kEngineFlooded}) && channel->closes() == 2);
        sink->remoteOps("[]");
        sink->remoteLoaded();
        CHECK(sink->remoteQuery("{}").json == "{\"error\":\"dead\"}");
        CHECK(recorder.events().size() == 3 && channel->closes() == 2);
    }
}

/// A delegate's own queue, as WidgetHost's host thread: work it queues with `remoteHold` keeps its message counted after the delegate call returned, and `remoteHold(bytes)` counts work of its own beside it. The place comes back when that work runs, when a stopped queue drops it or refuses it, and holds let go after the engine and its channel are gone touch nothing of theirs. Extra work past the bounds is refused and fails the widget once; an engine in process holds nothing.
TEST(remote_work_a_delegate_queues_keeps_its_message_counted) {
    std::vector<SandboxEngine::RemoteHold> outliving;
    {
        Gate first, second;
        JSThread queue("twidget-test-queue");
        RemoteRun r;
        SandboxRemoteSink& sink = r.sink();
        r.recorder.duringOps = [&](SandboxEngine& engine) {
            queue.async([hold = engine.remoteHold()] {});
            if (std::optional<SandboxEngine::RemoteHold> more = engine.remoteHold(10)) queue.async([hold = std::move(*more)] {});
        };
        queue.async([&first] { first.wait(); });
        CHECK(first.waitedOn());
        sink.remoteOps("[1]");
        r.settle();
        CHECK_MSG(backlogIs(r.engine, 3 + 10, 2), backlogText(r.engine));
        first.open();
        queue.sync([] {});
        CHECK_MSG(backlogIs(r.engine, 0, 0), backlogText(r.engine));
        queue.async([&second] { second.wait(); });
        CHECK(second.waitedOn());
        queue.stop();
        sink.remoteOps("[2]");
        r.settle();
        CHECK_MSG(backlogIs(r.engine, 3 + 10, 2), backlogText(r.engine));
        second.open();
        queue.join();
        CHECK_MSG(backlogIs(r.engine, 0, 0), backlogText(r.engine));
        sink.remoteOps("[3]");
        r.settle();
        CHECK_MSG(backlogIs(r.engine, 0, 0), backlogText(r.engine));
        r.recorder.duringOps = [&](SandboxEngine& engine) {
            outliving.push_back(engine.remoteHold());
            outliving.push_back(engine.remoteHold(10).value_or(nullptr));
        };
        sink.remoteOps("[4]");
        r.settle();
        CHECK_MSG(outliving.size() == 2 && outliving[0] && outliving[1] && backlogIs(r.engine, 3 + 10, 2), backlogText(r.engine));
        CHECK(r.recorder.eventsWith("ops").size() == 4 && r.recorder.deaths().empty());
    }
    outliving.clear();
    {
        RemoteRun r;
        bool refused = false;
        r.recorder.duringOps = [&](SandboxEngine& engine) { refused = !engine.remoteHold(SandboxEngine::kRemoteQueuedBytes); };
        r.sink().remoteOps("[1]");
        r.settle();
        r.settle();
        CHECK_MSG(refused && (r.recorder.events() == std::vector<std::string>{"ops", "died:" + kEngineFlooded}), joined(r.recorder.events()));
        CHECK(r.channel->calls().back() == "stop:" + kEngineFlooded && r.channel->closes() == 1 && backlogIs(r.engine, 0, 0));
        r.sink().remoteOps("[2]");
        r.settle();
        CHECK(r.recorder.events().size() == 2 && backlogIs(r.engine, 0, 0));
    }
    {
        RemoteRecorder recorder;
        std::mutex m;
        std::vector<bool> heldNothing;
        recorder.duringOps = [&](SandboxEngine& engine) {
            const std::optional<SandboxEngine::RemoteHold> more = engine.remoteHold(10);
            std::lock_guard<std::mutex> l(m);
            heldNothing.push_back(!engine.remoteHold() && more && !*more);
        };
        SandboxEngine local("<p>x</p>", testConfig(), RES);
        local.setDelegate(&recorder);
        local.start();
        CHECK(eventually([&] { return !recorder.eventsWith("ops").empty(); }));
        local.thread().sync([] {});
        std::lock_guard<std::mutex> l(m);
        CHECK(!local.isRemote() && !heldNothing.empty() && std::all_of(heldNothing.begin(), heldNothing.end(), [](bool b) { return b; }));
    }
}

/// Resize anchors pass `checkResizeAnchor` (also in a preview), a public-data watch reaches no preview, stats come bounded as the wire's numbers are whatever struct arrives, and after a death nothing more comes.
TEST(remote_anchors_watches_stats_and_loaded_follow_the_hosts_rules) {
    {
        RemoteRun r;
        r.sink().remoteLoaded();
        r.sink().remoteResizeAnchor("bottom");
        for (const std::string& bad : {std::string("Bottom"), std::string(), std::string("left"), std::string(" top"), std::string(100, 'x'), std::string("top\0", 4)})
            r.sink().remoteResizeAnchor(bad);
        r.sink().remoteResizeAnchor("top");
        r.sink().remoteWatchesPublicData(true);
        r.sink().remoteWatchesPublicData(false);
        SandboxStats hostile;
        hostile.jsMs = std::numeric_limits<double>::quiet_NaN();
        hostile.tasks = -5;
        hostile.memoryBytes = std::numeric_limits<size_t>::max();
        hostile.ops = std::numeric_limits<int>::max();
        hostile.queries = 3;
        hostile.queryMs = std::numeric_limits<double>::infinity();
        hostile.queryCacheHits = std::numeric_limits<int>::min();
        hostile.missedMutations = 2;
        hostile.throttledTasks = 1;
        hostile.cpuWindowMs = 1e300;
        hostile.validatorMs = -1;
        r.sink().remoteStats(hostile);
        r.settle();
        const std::vector<std::string> violations(6, "diag:protocol-violation");
        std::vector<std::string> expected = {"loaded", "anchor:bottom"};
        expected.insert(expected.end(), violations.begin(), violations.end());
        expected.insert(expected.end(), {"anchor:top", "watch:true", "watch:false", "stats"});
        CHECK_MSG(r.recorder.events() == expected, joined(r.recorder.events()));
        const std::vector<std::pair<std::string, std::string>> diags = r.recorder.diags();
        CHECK(diags.size() == 6 && diags[0].second == "resize anchor Bottom" && diags[1].second == "resize anchor " && diags[4].second == "resize anchor " + std::string(16, 'x') &&
              diags[5].second == std::string("resize anchor top\0", 18));
        const std::vector<SandboxStats> stats = r.recorder.stats();
        CHECK(stats.size() == 1);
        if (stats.size() == 1) {
            const SandboxStats& s = stats[0];
            CHECK(s.jsMs == 0 && s.tasks == 0 && s.ops == std::numeric_limits<int>::max() && s.queries == 3 && s.queryMs == 0 && s.queryCacheHits == 0);
            CHECK(double(s.memoryBytes) == std::min(1e15, double(std::numeric_limits<size_t>::max())));
            CHECK(s.missedMutations == 2 && s.throttledTasks == 1 && s.cpuWindowMs == 1e15 && s.validatorMs == 0);
        }
        r.sink().remoteDied("gone");
        r.sink().remoteLoaded();
        r.sink().remoteStats(SandboxStats{});
        r.sink().remoteResizeAnchor("bottom");
        r.sink().remoteResizeAnchor("left");
        r.sink().remoteWatchesPublicData(true);
        r.sink().remoteLog("log", "late");
        r.sink().remoteDiag("late", "late");
        r.sink().remoteOps("[]");
        r.sink().remoteListen(paddedTo("{}", SandboxEngine::kRemoteMessageLimit + 1));
        r.sink().remoteAnim("{}");
        r.sink().remoteRequestsFrame();
        r.sink().remoteTaskStarted();
        r.sink().remoteBridge(std::string(SandboxEngine::kRemoteNameLimit + 1, 'm'), "[]");
        r.sink().remoteCopyText("");
        r.sink().remoteStorage(paddedTo("{}", sdk::MAX_STORAGE_COMMIT_BYTES + 1));
        const std::string formatted = r.sink().remoteIntl(kNumberRequest);
        r.settle();
        expected.push_back("died:gone");
        CHECK_MSG(r.recorder.events() == expected, joined(r.recorder.events()));
        CHECK(formatted.empty() && r.channel->answers().empty());
    }
    {
        SandboxConfig preview = remoteConfig();
        preview.context.preview = true;
        RemoteRun r(preview);
        r.sink().remoteResizeAnchor("bottom");
        r.sink().remoteWatchesPublicData(true);
        r.sink().remoteLoaded();
        r.settle();
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"anchor:bottom", "loaded"}), joined(r.recorder.events()));
    }
}

/// An ops delegate stops the widget and the channel reports a disconnection from inside its close: a query sent after that batch, behind it in the queue, is answered dead without reaching the delegate, after one death and the task's end (one before it reaches the delegate). The same stop from inside a query's own callback, its caller provably waiting (the query was queued and runs), needs nothing that caller holds: the caller gets the callback's answer.
TEST(remote_a_stop_with_a_query_behind_it_answers_the_query_dead) {
    {
        RemoteRun r;
        r.channel->disconnectOnClose = true;
        r.recorder.duringOps = [](SandboxEngine& engine) { engine.stop("a counted cap"); };
        std::shared_ptr<SandboxRemoteSink> sink = r.channel->sink();
        const std::string answers = returnsWithin<std::string>(5, [sink] {
            const std::string before = sink->remoteQuery("{\"q\":0}").json;
            sink->remoteTaskStarted();
            sink->remoteOps("[]");
            return before + "|" + sink->remoteQuery("{\"q\":1}").json;
        });
        CHECK_MSG(answers == "{\"answer\":1}|{\"error\":\"dead\"}", answers);
        sink->remoteTaskCompleted();
        r.settle();
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"query:{\"q\":0}", "query-done", "begin", "ops", "died:a counted cap", "end"}), joined(r.recorder.events()));
        CHECK((r.channel->calls() == std::vector<std::string>{startCall("<p>x</p>", remoteConfig()), "stop:a counted cap"}) && r.channel->closes() == 1);
    }
    {
        Gate gate;
        RemoteRun r;
        r.channel->disconnectOnClose = true;
        r.recorder.duringQuery = [&](SandboxEngine& engine) {
            gate.wait();
            engine.stop("a counted cap");
        };
        std::shared_ptr<SandboxRemoteSink> sink = r.channel->sink();
        Background<std::string> transport([sink] { return sink->remoteQuery("{\"q\":2}").json; });
        // Held in that query's callback: the query was submitted and runs, so its caller is waiting for it.
        CHECK(gate.waitedOn() && !transport.within(0));
        gate.open();
        const std::string answer = transport.finish(5);
        CHECK_MSG(answer == "{\"answer\":1}", answer);
        r.settle();
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"query:{\"q\":2}", "died:a counted cap", "query-done"}), joined(r.recorder.events()));
        CHECK((r.channel->calls() == std::vector<std::string>{startCall("<p>x</p>", remoteConfig()), "stop:a counted cap"}) && r.channel->closes() == 1);
        CHECK(sink->remoteQuery("{}").json == "{\"error\":\"dead\"}");
    }
}

/// The owner destroys a client while its delegate answers a query, or its provider an Intl request: that callback, held until the destructor has let the engine go (a probe from the dispatch thread finds it before and not after), still answers its caller; the destructor waits for it and returns, and later sink calls reach nothing.
TEST(remote_teardown_waits_for_a_query_or_intl_in_flight) {
    for (const bool intlCase : {false, true}) {
        Gate gate;
        RemoteRecorder recorder;
        auto channel = std::make_shared<FakeRemoteChannel>();
        auto intl = std::make_shared<RecordingIntl>();
        std::shared_ptr<SandboxRemoteSink> sink;
        std::atomic<bool> entered{false}, foundBefore{false}, goneWhileHeld{false};
        const auto held = [&] {
            foundBefore = sink->remoteIntl(kLivenessProbe) == "alive";
            entered = true;
            goneWhileHeld = eventually([&] { return sink->remoteIntl(kLivenessProbe).empty(); });
            gate.wait();
        };
        intl->into = &recorder;
        intl->during = held;
        recorder.duringQuery = [&](SandboxEngine&) { held(); };
        auto engine = std::make_unique<SandboxEngine>("<p>x</p>", remoteConfig(), channel, intl);
        engine->setDelegate(&recorder);
        engine->start();
        engine->thread().sync([] {});
        sink = channel->sink();
        Background<std::string> call([sink, intlCase] { return intlCase ? sink->remoteIntl(kNumberRequest) : sink->remoteQuery("{\"q\":1}").json; });
        CHECK(eventually([&] { return entered.load(); }));
        Background<bool> teardown([&] {
            engine.reset();
            return true;
        });
        // The callback still held and the engine let go: neither its caller nor the destructor can be done.
        CHECK(gate.waitedOn() && foundBefore && goneWhileHeld && !call.within(0) && !teardown.within(0));
        gate.open();
        const std::string answer = call.finish(5);
        CHECK_MSG(answer == (intlCase ? "INTL" : "{\"answer\":1}"), answer);
        CHECK(teardown.finish(5));
        const std::vector<std::string> before = recorder.events();
        CHECK_MSG(before.size() == (intlCase ? 1u : 2u), joined(before));
        sink->remoteTaskStarted();
        sink->remoteOps("[]");
        sink->remoteLog("log", "late");
        sink->remoteLoaded();
        CHECK(sink->remoteQuery("{}").json == "{\"error\":\"dead\"}" && sink->remoteIntl(kNumberRequest).empty());
        CHECK_MSG(recorder.events() == before, joined(recorder.events()));
        CHECK(channel->closes() == 1);
    }
}

// ───────────── client mode: privileged calls decided on the host's own taps, visibility and flags ─────────────

using RemoteAnswers = std::vector<std::pair<int64_t, SandboxRemoteOutcome>>;
static const SandboxRemoteOutcome kRemoteNotAllowed = SandboxRemoteOutcome::failed(WidgetRequestFailure::NotAllowed);
static const SandboxRemoteOutcome kRemoteRateLimited = SandboxRemoteOutcome::failed(WidgetRequestFailure::RateLimited);

static std::string answersText(const RemoteAnswers& answers) {
    std::string out;
    for (const auto& [id, outcome] : answers) out += (out.empty() ? "" : " | ") + std::to_string(id) + "=" + SandboxRemoteCoding::encodeOutcome(outcome);
    return out;
}

/// A link call's arguments as the shim sends them.
static std::string linkArgs(const std::string& url) { return "[" + Json::quote(url) + "]"; }
/// The recorder's event for the link action of `method` (openLink or openTelegramLink) to `url`.
static std::string linkAction(const std::string& method, const std::string& url) { return "action:{\"kind\":\"" + method + "\",\"url\":" + Json::quote(url) + "}"; }

static WidgetRequestOutcome failedWith(WidgetRequestFailure failure) {
    WidgetRequestOutcome o;
    o.failure = failure;
    return o;
}

/// Swift's `WalletCall.signData(SignDataPayload(type: .text, text: "Sign me"), data: "data")`.
static WalletCall signMeCall() {
    WalletCall call;
    call.method = WalletCall::Method::SignData;
    call.payload.type = "text";
    call.payload.text = "Sign me";
    call.data = "data";
    return call;
}

static WalletCall sendTransactionCall() {
    WalletCall call;
    call.transaction.messages = {WalletMessage{"EQCD39VS5jcptHL8vMjEXrzGaRcCVYto7HUn4bpAOg8xqB2N", "1000", std::nullopt, std::nullopt}};
    call.data = "pay";
    return call;
}

/// Swift's testForgedLinkWithoutATapIsRefused: under strict a link or a copy goes nowhere (reported) without a tap this host delivered, whatever else either side sent; after a tap it goes, and only exactly "compat" needs none.
TEST(remote_forged_link_without_a_tap_is_refused) {
    {
        RemoteRun r;
        // No tap: the host's plain event, its presses, gestures 0 and below; the engine's side's own task, listener and ops.
        r.engine.event("{\"type\":\"click\"}");
        r.engine.pressBegan();
        r.engine.pressEnded();
        r.engine.activationEvent("{\"type\":\"click\"}", 0);
        r.engine.activationEvent("{\"type\":\"click\"}", -3);
        r.sink().remoteTaskStarted();
        r.sink().remoteListen("{\"type\":\"click\"}");
        r.sink().remoteOps("[{\"op\":\"click\"}]");
        r.sink().remoteBridge("openLink", linkArgs("https://example.com/"));
        r.sink().remoteBridge("openTelegramLink", linkArgs("https://t.me/durov"));
        r.sink().remoteCopyText("secret");
        r.sink().remoteTaskCompleted();
        r.settle();
        const std::vector<std::string> expected = {"begin", "listen", "ops", "diag:activation-blocked", "diag:activation-blocked", "diag:activation-blocked", "end"};
        CHECK_MSG(r.recorder.events() == expected, joined(r.recorder.events()));
        const std::vector<std::pair<std::string, std::string>> diags = r.recorder.diags();
        CHECK(diags.size() == 3 && diags[0].second == "openLink ignored by the host: no tap on the visible widget allows it" &&
              diags[2].second == "copyText ignored by the host: no tap on the visible widget allows it");
        r.tap(1);
        r.sink().remoteBridge("openLink", linkArgs("https://example.com/"));
        r.settle();
        CHECK_MSG(r.recorder.eventsWith("action:") == std::vector<std::string>{linkAction("openLink", "https://example.com/")}, joined(r.recorder.events()));
        CHECK(r.recorder.eventsWith("bridge:") == std::vector<std::string>{"bridge:openLink:[\"https://example.com/\"]"});
    }
    for (const char* policy : {"compat", "Compat", "", "lax"}) {
        SandboxConfig config = remoteConfig();
        config.policy = policy;
        RemoteRun r(config);
        r.sink().remoteBridge("openLink", linkArgs("https://example.com/"));
        r.sink().remoteCopyText("text");
        r.settle();
        const size_t allowed = std::string(policy) == "compat" ? 1 : 0;
        CHECK_MSG(r.recorder.eventsWith("action:").size() == allowed && r.recorder.eventsWith("copy:").size() == allowed, policy);
    }
}

/// Swift's testOneTapAllowsOnePrivilegedCall: one tap allows one link, copy, tapped request or tapped wallet call, whichever comes first; the rest of that tap's calls are refused, and the next tap allows the next one.
TEST(remote_one_tap_allows_one_privileged_call) {
    SandboxConfig config = remoteConfig();
    config.context.wallet = true;
    RemoteRun r(config);
    const WalletCall call = signMeCall();
    const std::string payload = SandboxRemoteCoding::encodeWallet(call);
    r.tap(1);
    r.sink().remoteBridge("openLink", linkArgs("https://example.com/"));
    r.sink().remoteBridge("openLink", linkArgs("https://example.com/again"));
    r.sink().remoteCopyText("secret");
    r.sink().remoteRequest(1, "after the link", false);
    r.sink().remoteWallet(2, payload);
    r.settle();
    const std::string link = linkAction("openLink", "https://example.com/");
    CHECK_MSG(r.recorder.eventsWith("action:") == std::vector<std::string>{link}, joined(r.recorder.events()));
    CHECK(r.recorder.eventsWith("copy:").empty() && r.recorder.eventsWith("request:").empty() && r.recorder.eventsWith("wallet:").empty());
    r.tap(2);
    r.sink().remoteCopyText("ok");
    r.sink().remoteBridge("openLink", linkArgs("https://example.com/late"));
    r.settle();
    CHECK(r.recorder.eventsWith("copy:") == std::vector<std::string>{"copy:ok"} && r.recorder.eventsWith("action:").size() == 1);
    // A tapped request and a tapped wallet call spend their taps too.
    r.tap(3);
    r.sink().remoteRequest(3, "tapped", false);
    r.sink().remoteCopyText("after the request");
    r.settle();
    CHECK(r.recorder.eventsWith("request:") == std::vector<std::string>{"request:tapped"} && r.recorder.eventsWith("copy:").size() == 1);
    CHECK(r.recorder.respond(0, failedWith(WidgetRequestFailure::Timeout)));
    r.tap(4);
    r.sink().remoteWallet(4, payload);
    r.sink().remoteBridge("openTelegramLink", linkArgs("https://t.me/durov"));
    r.settle();
    CHECK(r.recorder.eventsWith("wallet:") == std::vector<std::string>{"wallet:signData:data"} && r.recorder.walletCalls() == std::vector<WalletCall>{call});
    CHECK(r.recorder.eventsWith("action:").size() == 1);
    CHECK(r.recorder.respond(1, answered("signed")));
    r.tap(5);
    r.tap(6);
    r.sink().remoteCopyText("five");
    r.sink().remoteBridge("openTelegramLink", linkArgs("https://t.me/durov"));
    r.settle();
    CHECK(r.recorder.eventsWith("copy:") == (std::vector<std::string>{"copy:ok", "copy:five"}));
    CHECK_MSG(r.recorder.eventsWith("action:") == (std::vector<std::string>{link, linkAction("openTelegramLink", "https://t.me/durov")}), joined(r.recorder.events()));
    const RemoteAnswers expected = {{1, kRemoteRateLimited}, {2, kRemoteNotAllowed}, {3, SandboxRemoteOutcome::failed(WidgetRequestFailure::Timeout)}, {4, SandboxRemoteOutcome::answer("signed")}};
    CHECK_MSG(r.channel->answers() == expected, answersText(r.channel->answers()));
    CHECK(r.recorder.eventsWith("diag:activation-blocked").size() == 5 && r.recorder.eventsWith("diag:").size() == 5);
}

/// Swift's testEachTapAllowsOneCallEvenWhenTheCallsArriveLater: each tap the calls come after allows one, a repeated gesture once, one up to a spent one none; the oldest live tap goes first, and only the newest 32 are kept.
TEST(remote_each_tap_allows_one_call_even_when_the_calls_arrive_later) {
    {
        RemoteRun r;
        r.tap(1);
        r.tap(2);
        r.tap(2);
        for (const char* n : {"1", "2", "3"}) r.sink().remoteBridge("openLink", linkArgs(std::string("https://example.com/") + n));
        r.settle();
        CHECK_MSG(r.recorder.eventsWith("action:") == (std::vector<std::string>{linkAction("openLink", "https://example.com/1"), linkAction("openLink", "https://example.com/2")}),
                  joined(r.recorder.events()));
        r.tap(1);
        r.sink().remoteBridge("openLink", linkArgs("https://example.com/old"));
        r.settle();
        CHECK_MSG(r.recorder.eventsWith("action:").size() == 2, "a gesture older than a spent one grants nothing");
        // Spending 5 before 10 leaves room for 7, which a newest-first rule would have spent past.
        r.tap(10);
        r.tap(5);
        r.sink().remoteCopyText("a");
        r.tap(7);
        r.sink().remoteCopyText("b");
        r.sink().remoteCopyText("c");
        r.sink().remoteCopyText("d");
        r.settle();
        CHECK_MSG(r.recorder.eventsWith("copy:") == (std::vector<std::string>{"copy:a", "copy:b", "copy:c"}), joined(r.recorder.events()));
    }
    {
        RemoteRun r;
        const int max = int(SandboxEngine::kRemoteMaxGrants), taps = max + 8;
        for (int g = 1; g <= taps; g++) r.tap(g);
        for (int i = 0; i < taps; i++) r.sink().remoteCopyText("copy " + std::to_string(i));
        r.settle();
        std::vector<std::string> copies = r.recorder.eventsWith("copy:");
        CHECK_MSG(copies.size() == size_t(max) && copies.back() == "copy:copy " + std::to_string(max - 1), std::to_string(copies.size()));
        // The newest 32 were kept and spent: a repeat of one of them allows nothing (it would, had the oldest been kept), a newer tap one call.
        r.tap(taps - 5);
        r.sink().remoteCopyText("kept and spent");
        r.tap(taps + 1);
        r.sink().remoteCopyText("newer");
        r.settle();
        copies = r.recorder.eventsWith("copy:");
        CHECK_MSG(copies.size() == size_t(max) + 1 && copies.back() == "copy:newer", joined(copies));
    }
}

/// Swift's testTapOnHiddenWidgetGrantsNothing: a tap delivered while the widget is hidden allows nothing, also once it appears; while hidden no tap allows a call; hidden from the start the same.
TEST(remote_tap_on_a_hidden_widget_grants_nothing) {
    {
        RemoteRun r;
        r.engine.setVisible(false);
        r.tap(1);
        r.engine.setVisible(true);
        r.sink().remoteBridge("openTelegramLink", linkArgs("https://t.me/durov"));
        r.settle();
        CHECK_MSG(r.recorder.eventsWith("action:").empty() && r.recorder.eventsWith("diag:") == std::vector<std::string>{"diag:activation-blocked"}, joined(r.recorder.events()));
        r.tap(2);
        r.sink().remoteBridge("openTelegramLink", linkArgs("https://t.me/durov"));
        r.settle();
        CHECK(r.recorder.eventsWith("action:") == std::vector<std::string>{linkAction("openTelegramLink", "https://t.me/durov")});
        r.tap(3);
        r.engine.setVisible(false);
        r.sink().remoteCopyText("hidden");
        r.sink().remoteRequest(1, "hidden", false);
        r.settle();
        CHECK(r.recorder.eventsWith("copy:").empty() && r.recorder.eventsWith("request:").empty());
        CHECK_MSG(r.channel->answers() == (RemoteAnswers{{1, kRemoteRateLimited}}), answersText(r.channel->answers()));
    }
    {
        SandboxConfig hidden = remoteConfig();
        hidden.visible = false;
        RemoteRun r(hidden);
        r.tap(1);
        r.engine.setVisible(true);
        r.sink().remoteCopyText("x");
        r.settle();
        CHECK(r.recorder.eventsWith("copy:").empty());
        r.tap(2);
        r.sink().remoteCopyText("y");
        r.settle();
        CHECK(r.recorder.eventsWith("copy:") == std::vector<std::string>{"copy:y"});
    }
}

/// Swift's testGrantExpires on the real clock: a tap allows a call for `kRemoteGrantWindowMs`, then no link, copy or request, and a new tap allows one again.
TEST(remote_grant_expires) {
    RemoteRun r;
    r.tap(1);
    r.sink().remoteCopyText("now");
    r.settle();
    CHECK(r.recorder.eventsWith("copy:") == std::vector<std::string>{"copy:now"});
    r.tap(2);
    r.tap(3);
    sleepMs(int(SandboxEngine::kRemoteGrantWindowMs) + 100);
    r.sink().remoteCopyText("late");
    r.sink().remoteBridge("openLink", linkArgs("https://example.com/"));
    r.sink().remoteRequest(1, "late", false);
    r.settle();
    CHECK(r.recorder.eventsWith("copy:").size() == 1 && r.recorder.eventsWith("action:").empty() && r.recorder.eventsWith("request:").empty());
    CHECK_MSG(r.channel->answers() == (RemoteAnswers{{1, kRemoteRateLimited}}), answersText(r.channel->answers()));
    r.tap(4);
    r.sink().remoteCopyText("again");
    r.settle();
    CHECK(r.recorder.eventsWith("copy:") == (std::vector<std::string>{"copy:now", "copy:again"}));
}

/// Swift's testMalformedBridgeCallsAreRejected and more: whatever fails the SDK's check, or is not an action, is reported and spends no tap; escaped lone surrogates stay lone; an action passes with its raw arguments; copies keep the SDK's bounds.
TEST(remote_malformed_bridge_calls_are_rejected) {
    {
        RemoteRun r;
        const std::vector<std::pair<std::string, std::string>> malformed = {
            {"openLink", "[\"javascript:alert(1)\"]"},
            {"openLink", "not json"},
            {"exec", "[]"},
            {"copyText", "[\"x\"]"},
            {"request", "[1,\"x\"]"},
            {"setResizeAnchor", "[\"bottom\"]"},
            {"watchPublicData", "[true]"},
            {"wallet", "[1,\"signData\",{\"type\":\"text\",\"text\":\"x\"},\"d\"]"},
            {"openLink", "[123]"},
            {"openLink", "{\"0\":\"https://example.com/\"}"},
            {"openLink", "\"https://example.com/\""},
            {"openLink", "[\"https://example.com/\",true]"},
            {"openLink", "[\"https://example.com/\\u0000\"]"},
            {"openLink", "[\"https://example.com/\\ud800\"]"},
            {"openLink", "[\"https://example.com/\xED\xA0\x80\"]"},
            {"openLink", "[\"https://example.com/\xFF\"]"},
            {"OpenLink", "[\"https://example.com/\"]"},
            {std::string("openLink\0", 9), "[\"https://example.com/\"]"},
            {"openLink", std::string(200, '[') + "\"https://example.com/\"" + std::string(200, ']')},
        };
        r.tap(1);
        for (const auto& [method, args] : malformed) r.sink().remoteBridge(method, args);
        r.sink().remoteBridge("ready", "[]");
        const std::string padded = paddedTo("[\"https://example.com/ok\"]", SandboxEngine::kRemoteMessageLimit);
        r.sink().remoteBridge("openLink", padded);
        r.sink().remoteBridge("openLink", linkArgs("https://example.com/second"));
        r.settle();
        std::vector<std::string> expected(malformed.size(), "diag:bridge-rejected");
        expected.insert(expected.end(), {"action:{\"kind\":\"ready\"}", "bridge:ready:[]", linkAction("openLink", "https://example.com/ok"), "bridge:openLink:" + padded, "diag:activation-blocked"});
        CHECK_MSG(r.recorder.events() == expected, joined(r.recorder.events()));
        const std::vector<std::pair<std::string, std::string>> diags = r.recorder.diags();
        CHECK(diags.size() == malformed.size() + 1 && diags[2].second == "exec: rejected by the host (unknown method or malformed arguments)" &&
              diags[17].second == std::string("openLink\0", 9) + ": rejected by the host (unknown method or malformed arguments)");
        // Read with replacement the escaped lone surrogate would make a valid link (U+FFFD): the client keeps it and refuses.
        auto replaced = Json::parse(malformed[13].second);
        std::optional<sdk::BridgeCall> laundered = replaced ? sdk::checkBridgeCall(Json("openLink"), *replaced) : std::nullopt;
        CHECK(laundered && laundered->action);
    }
    {
        RemoteRun r;
        const std::string emoji = "\xF0\x9F\x98\x80";
        std::string emoji128;
        for (int i = 0; i < 128; i++) emoji128 += emoji;
        r.tap(1);
        for (const std::string& bad : {std::string(), std::string("a\xFF"), std::string("\xED\xB0\x80"), emoji128 + emoji, std::string(size_t(sdk::MAX_COPY_TEXT_LENGTH) + 1, 'c')})
            r.sink().remoteCopyText(bad);
        r.sink().remoteCopyText(emoji128);
        r.tap(2);
        r.sink().remoteCopyText(std::string(size_t(sdk::MAX_COPY_TEXT_LENGTH), 'a'));
        r.tap(3);
        r.sink().remoteCopyText("PROMO-2026 \xE2\x9C\x93");
        r.settle();
        std::vector<std::string> expected(5, "diag:bridge-rejected");
        expected.insert(expected.end(), {"copy:" + emoji128, "copy:" + std::string(size_t(sdk::MAX_COPY_TEXT_LENGTH), 'a'), "copy:PROMO-2026 \xE2\x9C\x93"});
        CHECK_MSG(r.recorder.events() == expected, joined(r.recorder.events()));
    }
}

/// Swift's testRequestWithoutCanQueryIsRefusedAndSettles: without canQuery a request or wallet call is refused NOT_ALLOWED on its id (the host's wallet flag notwithstanding) and spends no tap; so is malformed data where requests are allowed; with no delegate an allowed one is unsupported and frees the gate.
TEST(remote_request_without_can_query_is_refused_and_settles) {
    const std::string payload = SandboxRemoteCoding::encodeWallet(signMeCall());
    {
        SandboxConfig config = remoteConfig();
        config.context.canQuery = false;
        config.context.backgroundQuery = true;
        config.context.wallet = true;
        RemoteRun r(config);
        r.tap(1);
        r.sink().remoteRequest(7, "x", false);
        r.sink().remoteRequest(8, "x", true);
        r.sink().remoteWallet(9, payload);
        r.sink().remoteBridge("openLink", linkArgs("https://example.com/"));
        r.settle();
        CHECK(r.recorder.eventsWith("request:").empty() && r.recorder.eventsWith("wallet:").empty());
        CHECK_MSG(r.channel->answers() == (RemoteAnswers{{7, kRemoteNotAllowed}, {8, kRemoteNotAllowed}, {9, kRemoteNotAllowed}}), answersText(r.channel->answers()));
        CHECK(r.recorder.eventsWith("action:") == std::vector<std::string>{linkAction("openLink", "https://example.com/")});
    }
    {
        RemoteRun r;
        r.tap(1);
        int64_t id = 10;
        for (const std::string& data : {std::string(), std::string("a\xFF"), std::string("\xED\xA0\x80"), std::string(size_t(sdk::MAX_REQUEST_DATA_BYTES) + 1, 'r')})
            r.sink().remoteRequest(id++, data, false);
        r.sink().remoteWallet(id++, "not a wallet call");
        const std::string longest(size_t(sdk::MAX_REQUEST_DATA_BYTES), 'd');
        r.sink().remoteRequest(id, longest, false);
        r.settle();
        CHECK(r.recorder.eventsWith("request:") == std::vector<std::string>{"request:" + longest});
        const RemoteAnswers expected = {{10, kRemoteNotAllowed}, {11, kRemoteNotAllowed}, {12, kRemoteNotAllowed}, {13, kRemoteNotAllowed}, {14, kRemoteNotAllowed}};
        CHECK_MSG(r.channel->answers() == expected, answersText(r.channel->answers()));
    }
    {
        RemoteRun r(remoteConfig(), false);
        r.tap(1);
        r.sink().remoteRequest(1, "x", false);
        r.settle();
        r.tap(2);
        r.sink().remoteRequest(2, "y", false);
        // Without a delegate the responder answers through the dispatch thread: the second settle runs what the first one let it post.
        r.settle();
        r.settle();
        const SandboxRemoteOutcome unsupported = SandboxRemoteOutcome::unsupported();
        CHECK_MSG(r.channel->answers() == (RemoteAnswers{{1, unsupported}, {2, unsupported}}), answersText(r.channel->answers()));
    }
}

/// Swift's testForegroundRequestNeedsATapAndOneInFlight: without a tap RATE_LIMITED, a tapped one goes, a second is refused while it is out; its responder answers once (copies share the once), frees the gate, and after a death or the engine's destruction reaches nothing.
TEST(remote_foreground_request_needs_a_tap_and_one_in_flight) {
    {
        RemoteRun r;
        r.sink().remoteRequest(1, "no tap", false);
        r.settle();
        CHECK(r.recorder.eventsWith("request:").empty() && r.channel->answers() == (RemoteAnswers{{1, kRemoteRateLimited}}));
        r.tap(1);
        r.sink().remoteRequest(2, "tapped", false);
        r.sink().remoteRequest(3, "second", false);
        r.settle();
        CHECK(r.recorder.eventsWith("request:") == std::vector<std::string>{"request:tapped"});
        CHECK_MSG(!r.channel->answers().empty() && r.channel->answers().back() == (std::pair<int64_t, SandboxRemoteOutcome>{3, kRemoteNotAllowed}), answersText(r.channel->answers()));
        std::optional<WidgetRequestResponder> respond = r.recorder.responder(0);
        CHECK(respond.has_value());
        if (respond) {
            (*respond)(answered("done"));
            (*respond)(answered("again"));
            respond->unsupported();
        }
        CHECK(r.recorder.respond(0, failedWith(WidgetRequestFailure::Timeout)));
        r.settle();
        const RemoteAnswers expected = {{1, kRemoteRateLimited}, {3, kRemoteNotAllowed}, {2, SandboxRemoteOutcome::answer("done")}};
        CHECK_MSG(r.channel->answers() == expected, answersText(r.channel->answers()));
        r.tap(2);
        r.sink().remoteRequest(4, "next", false);
        r.settle();
        CHECK(r.recorder.eventsWith("request:") == (std::vector<std::string>{"request:tapped", "request:next"}));
        r.sink().remoteDied("gone");
        r.settle();
        CHECK(r.recorder.respond(1, answered("late")));
        r.settle();
        CHECK_MSG(r.channel->answers() == expected, answersText(r.channel->answers()));
    }
    {
        std::shared_ptr<FakeRemoteChannel> channel;
        std::optional<WidgetRequestResponder> late;
        {
            RemoteRun r;
            channel = r.channel;
            r.tap(1);
            r.sink().remoteRequest(1, "x", false);
            r.settle();
            late = r.recorder.responder(0);
        }
        CHECK(late.has_value());
        if (late) (*late)(answered("after"));
        CHECK(channel->answers().empty());
    }
    // Answered on another thread while the owner destroys the engine: once or not at all, never after it is gone.
    {
        auto run = std::make_unique<RemoteRun>();
        std::shared_ptr<FakeRemoteChannel> channel = run->channel;
        run->tap(1);
        run->sink().remoteRequest(1, "x", false);
        run->settle();
        std::optional<WidgetRequestResponder> respond = run->recorder.responder(0);
        CHECK(respond.has_value());
        if (respond) {
            std::thread answering([respond] { (*respond)(answered("racing")); });
            run.reset();
            answering.join();
        }
        const RemoteAnswers answers = channel->answers();
        CHECK_MSG(answers.empty() || answers == (RemoteAnswers{{1, SandboxRemoteOutcome::answer("racing")}}), answersText(answers));
    }
}

/// Swift's testBackgroundRequestsNeedTheFlagAndAreRateLimited: with backgroundQuery one goes without a tap, the next within 5 s is RATE_LIMITED until the widget appears again, a hidden one is RATE_LIMITED, and one for the wallet still needs a tap.
TEST(remote_background_requests_need_the_flag_and_are_rate_limited) {
    {
        SandboxConfig config = remoteConfig();
        config.context.backgroundQuery = true;
        RemoteRun r(config);
        r.sink().remoteRequest(1, "a", false);
        r.settle();
        CHECK(r.recorder.eventsWith("request:") == std::vector<std::string>{"request:a"});
        CHECK(r.recorder.respond(0, answered(std::nullopt)));
        r.settle();
        r.sink().remoteRequest(2, "b", false);
        r.settle();
        CHECK(r.recorder.eventsWith("request:").size() == 1);
        r.engine.setVisible(false);
        r.engine.setVisible(true);
        r.settle();
        r.sink().remoteRequest(3, "c", false);
        r.settle();
        CHECK(r.recorder.eventsWith("request:") == (std::vector<std::string>{"request:a", "request:c"}));
        r.engine.setVisible(false);
        r.settle();
        CHECK(r.recorder.respond(1, failedWith(WidgetRequestFailure::Timeout)));
        r.sink().remoteRequest(4, "hidden", false);
        r.settle();
        CHECK(r.recorder.eventsWith("request:").size() == 2);
        const RemoteAnswers expected = {{1, SandboxRemoteOutcome::answer(std::nullopt)}, {2, kRemoteRateLimited}, {3, SandboxRemoteOutcome::failed(WidgetRequestFailure::Timeout)},
                                        {4, kRemoteRateLimited}};
        CHECK_MSG(r.channel->answers() == expected, answersText(r.channel->answers()));
    }
    {
        SandboxConfig config = remoteConfig();
        config.context.backgroundQuery = true;
        config.context.wallet = true;
        config.visible = false;
        RemoteRun r(config);
        r.sink().remoteRequest(1, "hidden", false);
        r.engine.setVisible(true);
        r.sink().remoteRequest(2, "wallet", true);
        r.sink().remoteRequest(3, "shown", false);
        r.settle();
        CHECK(r.recorder.eventsWith("request:") == std::vector<std::string>{"request:shown"});
        CHECK_MSG(r.channel->answers() == (RemoteAnswers{{1, kRemoteRateLimited}, {2, kRemoteNotAllowed}}), answersText(r.channel->answers()));
    }
}

/// Swift's testWalletRequestAndFollowUpUseHostPermissionAndGrant: the host's wallet flag and taps decide; a checked answer to a tapped call grants one wallet call, a failed, invalid or unsupported one nothing; requests and wallet calls share one slot.
TEST(remote_wallet_request_and_follow_up_use_the_hosts_permission_and_grant) {
    const WalletCall call = signMeCall();
    const std::string payload = SandboxRemoteCoding::encodeWallet(call);
    SandboxConfig walletConfig = remoteConfig();
    walletConfig.context.backgroundQuery = true;
    walletConfig.context.wallet = true;
    {
        RemoteRun r(walletConfig);
        r.sink().remoteRequest(1, "wallet", true);
        r.sink().remoteWallet(2, payload);
        r.settle();
        CHECK(r.recorder.eventsWith("request:").empty() && r.recorder.eventsWith("wallet:").empty());
        r.tap(1);
        r.sink().remoteRequest(3, "wallet", true);
        r.settle();
        CHECK(r.recorder.eventsWith("request:") == std::vector<std::string>{"request:wallet:wallet"});
        CHECK(r.recorder.respond(0, answered("grant")));
        r.settle();
        r.sink().remoteWallet(4, "[1,\"signData\",{},\"data\"]");
        r.sink().remoteWallet(5, payload);
        r.sink().remoteRequest(6, "busy", false);
        r.settle();
        CHECK(r.recorder.walletCalls() == std::vector<WalletCall>{call} && r.recorder.eventsWith("request:").size() == 1);
        CHECK(r.recorder.respond(1, failedWith(WidgetRequestFailure::Rejected)));
        r.settle();
        r.sink().remoteWallet(7, payload);
        r.settle();
        CHECK(r.recorder.walletCalls().size() == 1);
        const RemoteAnswers expected = {{1, kRemoteNotAllowed},
                                        {2, kRemoteNotAllowed},
                                        {3, SandboxRemoteOutcome::answer("grant")},
                                        {4, kRemoteNotAllowed},
                                        {6, kRemoteNotAllowed},
                                        {5, SandboxRemoteOutcome::failed(WidgetRequestFailure::Rejected)},
                                        {7, kRemoteNotAllowed}};
        CHECK_MSG(r.channel->answers() == expected, answersText(r.channel->answers()));
    }
    // Without the host's wallet flag a tap allows no wallet request or call, and stays for a link.
    {
        SandboxConfig noWallet = walletConfig;
        noWallet.context.wallet = false;
        RemoteRun r(noWallet);
        r.tap(1);
        r.sink().remoteRequest(1, "wallet", true);
        r.sink().remoteWallet(2, payload);
        r.sink().remoteBridge("openLink", linkArgs("https://example.com/"));
        r.settle();
        CHECK(r.recorder.eventsWith("request:").empty() && r.recorder.eventsWith("wallet:").empty() && r.recorder.eventsWith("action:").size() == 1);
        CHECK_MSG(r.channel->answers() == (RemoteAnswers{{1, kRemoteNotAllowed}, {2, kRemoteNotAllowed}}), answersText(r.channel->answers()));
    }
    // The outcome goes back as it came (the engine's side checks an answer for the widget); only a checked answer brings the grant, here for a sendTransaction.
    const WalletCall transaction = sendTransactionCall();
    const std::vector<std::optional<WidgetRequestOutcome>> outcomes = {failedWith(WidgetRequestFailure::Timeout), answered(std::string("a\0b", 3)),
                                                                        answered(std::string(size_t(sdk::MAX_REQUEST_RESULT_BYTES) + 1, 'x')), std::nullopt, answered("ok")};
    for (size_t i = 0; i < outcomes.size(); i++) {
        RemoteRun r(walletConfig);
        r.tap(1);
        r.sink().remoteRequest(1, "wallet", true);
        r.settle();
        CHECK(r.recorder.respond(0, outcomes[i]));
        r.settle();
        r.sink().remoteWallet(2, SandboxRemoteCoding::encodeWallet(transaction));
        r.settle();
        const bool granted = i + 1 == outcomes.size();
        CHECK_MSG(r.recorder.walletCalls() == (granted ? std::vector<WalletCall>{transaction} : std::vector<WalletCall>{}), std::to_string(i));
        RemoteAnswers expected = {{1, SandboxRemoteOutcome::from(outcomes[i])}};
        if (!granted) expected.emplace_back(2, kRemoteNotAllowed);
        CHECK_MSG(r.channel->answers() == expected, std::to_string(i) + ": " + answersText(r.channel->answers()));
    }
}

/// Swift's testPreviewTakesNoActionsOrStorage: a preview takes no input and makes no link, haptic, copy, commit, watch, request or wallet call even with flags and a policy that would allow them; `ready` and the anchor pass, and the same calls pass without the preview flag.
TEST(remote_preview_takes_no_actions_or_storage) {
    SandboxConfig preview = remoteConfig();
    preview.policy = "compat";
    preview.context.preview = true;
    preview.context.backgroundQuery = true;
    preview.context.wallet = true;
    const std::string payload = SandboxRemoteCoding::encodeWallet(signMeCall());
    const auto forge = [&](RemoteRun& r) {
        r.tap(1);
        r.engine.event("{\"type\":\"click\"}");
        r.sink().remoteBridge("openLink", linkArgs("https://example.com/"));
        r.sink().remoteBridge("haptic", "[\"selection\"]");
        r.sink().remoteCopyText("secret");
        r.sink().remoteStorage("{\"local\":{\"a\":\"b\"}}");
        r.sink().remoteWatchesPublicData(true);
        r.sink().remoteRequest(1, "x", false);
        r.sink().remoteRequest(2, "w", true);
        r.sink().remoteWallet(3, payload);
        r.sink().remoteResizeAnchor("bottom");
        r.sink().remoteBridge("ready", "[]");
        r.settle();
    };
    {
        RemoteRun r(preview);
        forge(r);
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"anchor:bottom", "action:{\"kind\":\"ready\"}", "bridge:ready:[]"}), joined(r.recorder.events()));
        CHECK_MSG(r.channel->answers() == (RemoteAnswers{{1, kRemoteNotAllowed}, {2, kRemoteNotAllowed}, {3, kRemoteNotAllowed}}), answersText(r.channel->answers()));
        const std::vector<std::string> calls = r.channel->calls();
        CHECK_MSG(calls.size() == 1 && calls[0].rfind("start:", 0) == 0, joined(calls));
    }
    {
        SandboxConfig normal = preview;
        normal.context.preview = false;
        RemoteRun r(normal);
        forge(r);
        const std::vector<std::string> expected = {linkAction("openLink", "https://example.com/"), "bridge:openLink:[\"https://example.com/\"]",
                                                   "action:{\"kind\":\"haptic\",\"haptic\":\"selection\"}", "bridge:haptic:[\"selection\"]", "copy:secret", "storage:local", "watch:true",
                                                   "request:x", "anchor:bottom", "action:{\"kind\":\"ready\"}", "bridge:ready:[]"};
        CHECK_MSG(r.recorder.events() == expected, joined(r.recorder.events()));
        CHECK_MSG(r.channel->answers() == (RemoteAnswers{{2, kRemoteNotAllowed}, {3, kRemoteNotAllowed}}), answersText(r.channel->answers()));
    }
}

/// Swift's testStorageIsCheckedAgain: a commit passes the SDK's check again (the quota, objects of strings, last of a repeated key, lone surrogates kept) with at least one area, local before session; anything else is reported.
TEST(remote_storage_is_checked_again) {
    RemoteRun r;
    const std::string quota(size_t(sdk::STORAGE_QUOTA_BYTES) - 1, 'v');
    const std::string lone = "{\"session\":{\"k\":\"\\ud800\",\"n\":\"\\u0000\"}}";
    r.sink().remoteStorage("{\"local\":{\"a\":\"b\"}}");
    r.sink().remoteStorage("{\"local\":{\"a\":\"" + std::string(10000, 'x') + "\"}}");
    r.sink().remoteStorage("[1,2]");
    r.sink().remoteStorage("{\"session\":{\"s\":\"1\"},\"local\":{\"l\":\"2\"}}");
    r.sink().remoteStorage("{\"local\":{}}");
    r.sink().remoteStorage("{\"local\":{\"a\":\"1\",\"a\":\"2\"}}");
    r.sink().remoteStorage(lone);
    r.sink().remoteStorage("{\"local\":{\"q\":\"" + quota + "\"}}");
    for (const std::string& bad : {"{\"local\":{\"q\":\"" + quota + "v\"}}", std::string("{}"), std::string("{\"cookies\":{}}"), std::string("{\"local\":{\"a\":1}}"),
                                   std::string("{\"local\":{\"a\":\"b\"},\"session\":[]}"), std::string("not json"), std::string("{\"local\":{\"a\":\"b\"}")})
        r.sink().remoteStorage(bad);
    r.settle();
    const std::vector<std::pair<std::string, StorageArea>> expected = {
        {"local", StorageArea{{"a", "b"}}},
        {"local", StorageArea{{"l", "2"}}},
        {"session", StorageArea{{"s", "1"}}},
        {"local", StorageArea{}},
        {"local", StorageArea{{"a", "2"}}},
        {"session", StorageArea{{"k", "\xED\xA0\x80"}, {"n", std::string(1, '\0')}}},
        {"local", StorageArea{{"q", quota}}},
    };
    CHECK_MSG(r.recorder.commits() == expected, joined(r.recorder.events()));
    const std::vector<std::pair<std::string, std::string>> diags = r.recorder.diags();
    CHECK_MSG(diags.size() == 9 && std::all_of(diags.begin(), diags.end(), [](const auto& d) { return d.first == "storage-rejected" && d.second == "a storage commit the host refused"; }),
              joined(r.recorder.events()));
    // Read with replacement the lone surrogate would be stored as U+FFFD.
    auto replaced = Json::parse(lone);
    std::optional<sdk::StorageCommit> laundered = replaced ? sdk::checkStorageCommit(*replaced) : std::nullopt;
    CHECK(laundered && laundered->session && laundered->session->at("k") == "\xEF\xBF\xBD");
}

/// Swift's testHapticsPassTheHostGate: the host's own HapticGate on its presses and visibility bounds a forged burst, silently (the engine's side reports its own `haptic-limit`); a press lets haptics play again, a hidden widget plays none.
TEST(remote_haptics_pass_the_host_gate) {
    {
        RemoteRun r;
        for (int i = 0; i < 40; i++) r.sink().remoteBridge("haptic", "[\"selection\"]");
        r.settle();
        const size_t burst = r.recorder.eventsWith("action:").size();
        CHECK_MSG(burst > 0 && burst < 40, std::to_string(burst));
        CHECK(r.recorder.eventsWith("bridge:").size() == burst && r.recorder.diags().empty());
        // That series over, nothing plays outside a touch; the press and the 5 s after its release let haptics play, with their arguments as they came.
        sleepMs(int(sdk::HAPTIC_SERIES_MS) + 100);
        r.sink().remoteBridge("haptic", "[\"impact\",\"soft\"]");
        r.settle();
        CHECK(r.recorder.eventsWith("action:").size() == burst);
        r.engine.pressBegan();
        r.sink().remoteBridge("haptic", "[ \"impact\" , \"soft\" ]");
        r.settle();
        r.engine.pressEnded();
        r.sink().remoteBridge("haptic", "[\"notification\",\"success\"]");
        r.settle();
        const std::vector<std::string> actions = r.recorder.eventsWith("action:"), raw = r.recorder.eventsWith("bridge:");
        CHECK_MSG(actions.size() == burst + 2 && actions[burst] == "action:{\"kind\":\"haptic\",\"haptic\":\"impact\",\"value\":\"soft\"}" &&
                      actions.back() == "action:{\"kind\":\"haptic\",\"haptic\":\"notification\",\"value\":\"success\"}",
                  joined(actions));
        CHECK(raw.size() == burst + 2 && raw[burst] == "bridge:haptic:[ \"impact\" , \"soft\" ]");
        r.engine.setVisible(false);
        r.engine.pressBegan();
        r.sink().remoteBridge("haptic", "[\"selection\"]");
        r.settle();
        CHECK(r.recorder.eventsWith("action:").size() == burst + 2 && r.recorder.diags().empty());
    }
    {
        SandboxConfig hidden = remoteConfig();
        hidden.visible = false;
        RemoteRun r(hidden);
        r.engine.pressBegan();
        for (int i = 0; i < 10; i++) r.sink().remoteBridge("haptic", "[\"selection\"]");
        r.sink().remoteBridge("haptic", "[\"impact\",\"huge\"]");
        r.sink().remoteBridge("haptic", "[\"selection\",\"x\"]");
        r.settle();
        CHECK_MSG((r.recorder.events() == std::vector<std::string>{"diag:bridge-rejected", "diag:bridge-rejected"}), joined(r.recorder.events()));
    }
}

// ───────────── the engine's side (SandboxRemoteServer) and the loopback ─────────────

static const std::string kRect77 = "{\"x\":0,\"y\":0,\"width\":77,\"height\":20,\"top\":0,\"left\":0,\"right\":77,\"bottom\":20}";

/// What a SandboxRemoteServer sends its sink, as text in order with its thread; `remoteQuery` runs `duringQuery` (set it while no query is on its way) and answers a 77 px rect, `remoteIntl` answers "INTL".
class RecordingSink final : public SandboxRemoteSink {
public:
    std::function<void()> duringQuery;

    void remoteTaskStarted() override { record("begin"); }
    void remoteTaskCompleted() override { record("end"); }
    void remoteOps(const std::string&) override { record("ops"); }
    QueryAnswer remoteQuery(const std::string& json) override {
        std::optional<Json> query = Json::parse(json);
        record("query:" + (query ? (*query)["q"].asString() : std::string("?")));
        if (duringQuery) duringQuery();
        return {kRect77, std::nullopt};
    }
    std::string remoteIntl(const std::string& requestJson) override {
        record("intl:" + requestJson);
        return "INTL";
    }
    void remoteListen(const std::string&) override { record("listen"); }
    void remotePreventDefault(const std::string&) override { record("preventDefault"); }
    void remoteAnim(const std::string&) override { record("anim"); }
    void remoteRequestsFrame() override { record("frame"); }
    void remoteLog(const std::string&, const std::string& text) override { record("log:" + text); }
    void remoteDiag(const std::string& kind, const std::string& detail) override { record("diag:" + kind + ":" + detail); }
    void remoteBridge(const std::string& method, const std::string& argsJson) override { record("bridge:" + method + ":" + argsJson); }
    void remoteCopyText(const std::string& text) override { record("copy:" + text); }
    void remoteRequest(int64_t id, const std::string& data, bool wallet) override { record("request:" + std::to_string(id) + ":" + data + (wallet ? ":wallet" : "")); }
    void remoteWallet(int64_t id, const std::string& payload) override {
        std::optional<WalletCall> call = SandboxRemoteCoding::decodeWallet(payload);
        record("wallet:" + std::to_string(id) + ":" + (call ? std::string(call->methodName()) + ":" + call->payload.text.value_or("") + ":" + call->data : "undecodable"));
    }
    void remoteStorage(const std::string& commitJson) override { record("storage:" + commitJson); }
    void remoteResizeAnchor(const std::string& edge) override { record("anchor:" + edge); }
    void remoteWatchesPublicData(bool on) override { record(on ? "watch:true" : "watch:false"); }
    void remoteLoaded() override { record("loaded"); }
    void remoteStats(const SandboxStats& stats) override {
        record("stats");
        std::lock_guard<std::mutex> l(mutex_);
        stats_.push_back(stats);
    }
    void remoteDied(const std::string& reason) override { record("died:" + reason); }
    void remoteDisconnected() override { record("disconnected"); }

    std::vector<std::string> events() {
        std::lock_guard<std::mutex> l(mutex_);
        return events_;
    }
    std::vector<std::string> eventsWith(const std::string& prefix) {
        std::vector<std::string> out;
        for (const std::string& e : events())
            if (e.rfind(prefix, 0) == 0) out.push_back(e);
        return out;
    }
    /// The thread of the first event that starts with `prefix`, none before it came.
    std::thread::id threadOf(const std::string& prefix) {
        std::lock_guard<std::mutex> l(mutex_);
        for (size_t i = 0; i < events_.size(); i++)
            if (events_[i].rfind(prefix, 0) == 0) return threads_[i];
        return {};
    }
    std::vector<SandboxStats> stats() {
        std::lock_guard<std::mutex> l(mutex_);
        return stats_;
    }

private:
    void record(const std::string& event) {
        std::lock_guard<std::mutex> l(mutex_);
        events_.push_back(event);
        threads_.push_back(std::this_thread::get_id());
    }

    std::mutex mutex_;
    std::vector<std::string> events_;
    std::vector<std::thread::id> threads_;
    std::vector<SandboxStats> stats_;
};

/// Where `event` is in `events`, or `events.size()`.
static size_t indexOf(const std::vector<std::string>& events, const std::string& event) { return size_t(std::find(events.begin(), events.end(), event) - events.begin()); }

/// Each tap asks for the wallet, then signs on the answer's grant; every outcome is logged, so the sink sees it too.
static const char* kAskThenSign =
    "<button id='b'>go</button><script>var log = [], taps = 0; function note(s) { log.push(s); console.log(s); }"
    "document.getElementById('b').onclick = function () { var k = ++taps;"
    "  TelegramWidget.request('ask' + k, { wallet: true }).then(function (a) { note('ok' + k + ':' + a.result);"
    "    return TelegramWidget.wallet.signData({ type: 'text', text: 'sign' + k }, 'w' + k); })"
    "  .then(function (b) { note('signed' + k + ':' + b.result); }, function (e) { note('err' + k + ':' + e.code); }); };</script>";

static const char* kMeasure = "<p id='p'>x</p><script>console.log('w=' + document.getElementById('p').getBoundingClientRect().width);</script>";

static SandboxConfig walletConfig() {
    SandboxConfig c = testConfig();
    c.context = messageContext(false, true, false, "private");
    c.context.wallet = true;
    return c;
}

/// The real engine numbers its requests and wallet calls from one counter; an answer settles only the outstanding call of its id, once (a stray, early, repeated or late one changes nothing), and `close` refuses the call still out.
TEST(remote_server_numbers_requests_and_wallet_calls_from_one_counter) {
    using Outcome = SandboxRemoteOutcome;
    auto sink = std::make_shared<RecordingSink>();
    SandboxRemoteServer server(sink, RES);
    server.start(kAskThenSign, walletConfig());
    CHECK(eventually([&] { return has(sink->events(), "loaded"); }, 20));
    server.activationEvent(kTapOnButton, 1);
    CHECK(eventually([&] { return !sink->eventsWith("request:").empty(); }));
    for (int64_t stray : {int64_t(2), int64_t(0), int64_t(-1), std::numeric_limits<int64_t>::max()}) server.answer(stray, Outcome::answer(std::string("stray")));
    CHECK(server.probe("log.join(' ')").empty());
    server.answer(1, Outcome::answer(std::string("r1")));
    CHECK(server.probe("log.join(' ')") == "ok1:r1");
    CHECK_MSG((sink->eventsWith("wallet:") == std::vector<std::string>{"wallet:2:signData:sign1:w1"}), joined(sink->events()));
    server.answer(1, Outcome::failed(WidgetRequestFailure::Timeout));
    server.answer(2, Outcome::answer(std::string("s1")));
    server.answer(2, Outcome::failed(WidgetRequestFailure::Rejected));
    CHECK(server.probe("log.join(' ')") == "ok1:r1 signed1:s1");
    server.activationEvent(kTapOnButton, 2);
    CHECK(eventually([&] { return sink->eventsWith("request:").size() == 2; }));
    server.answer(4, Outcome::answer(std::string("ahead")));
    CHECK(server.probe("log.join(' ')") == "ok1:r1 signed1:s1");
    server.close();
    CHECK(eventually([&] { return !sink->eventsWith("died:").empty(); }));
    server.answer(3, Outcome::answer(std::string("late")));
    server.close();
    CHECK(server.probe("1") == "ERROR no runtime");
    const std::vector<std::string> events = sink->events();
    CHECK_MSG((sink->eventsWith("request:") == std::vector<std::string>{"request:1:ask1:wallet", "request:3:ask2:wallet"}), joined(events));
    CHECK_MSG((sink->eventsWith("log:") == std::vector<std::string>{"log:ok1:r1", "log:signed1:s1", "log:err2:NOT_ALLOWED"}), joined(events));
    CHECK(sink->eventsWith("died:") == std::vector<std::string>{"died:terminated"} && indexOf(events, "log:err2:NOT_ALLOWED") < indexOf(events, "died:terminated"));
    CHECK(has(events, "diag:request-blocked:request refused: the client handles no requests (onRequest)"));
    CHECK(sink->eventsWith("bridge:").empty() && sink->eventsWith("wallet:").size() == 1);
}

/// Inputs before `start` wait and reach the engine in order before it boots, so its own pre-boot rules decide them (hidden, the safe area, data and an event after the boot); a second start is ignored; Intl is answered through the sink while the widget runs; storage goes one area at a time; the validator's charge comes back in the stats; nothing runs after or without a start once closed.
TEST(remote_server_keeps_inputs_until_start_and_drops_them_after_close) {
    const char* html = "<button id='b'>go</button><script>var log = [document.visibilityState, TelegramWidget.contentSafeAreaInset.bottom, TelegramWidget.chatData, (1234.5).toLocaleString()];"
                       "TelegramWidget.onEvent('chatDataChanged', function () { log.push('chat:' + TelegramWidget.chatData); });"
                       "document.getElementById('b').addEventListener('click', function () { log.push('click'); });"
                       "localStorage.setItem('k', 'v'); sessionStorage.setItem('s', 'w');</script>";
    SandboxConfig config = testConfig();
    config.data = WidgetData{std::string("chat1"), std::nullopt};
    {
        auto sink = std::make_shared<RecordingSink>();
        SandboxRemoteServer server(sink, RES);
        server.setVisible(false);
        server.contentSafeArea(WidgetContentSafeArea{0, 0, 24, 0, 0, 64});
        server.setData(WidgetData{std::string("chat2"), std::nullopt});
        server.event(kTapOnButton);
        server.chargeValidator(5);
        server.answer(1, SandboxRemoteOutcome::answer(std::string("early")));
        CHECK(server.probe("1") == "ERROR no runtime" && sink->events().empty());
        server.start(html, config);
        server.start("<script>var log = ['second'];</script>", config);
        CHECK(eventually([&] { return has(sink->events(), "loaded"); }, 20));
        const std::string booted = "[\"hidden\",24,\"chat1\",\"INTL\",\"chat:chat2\",\"click\"]";
        CHECK_MSG(probeUntil(server, "JSON.stringify(log)", booted) == booted, server.probe("JSON.stringify(log)"));
        const std::vector<std::string> intl = sink->eventsWith("intl:");
        CHECK_MSG(intl.size() == 1 && intl[0].find("\"kind\":\"number\"") != std::string::npos && intl[0].find("1234.5") != std::string::npos, joined(intl));
        CHECK(sink->threadOf("intl:") == sink->threadOf("loaded") && sink->threadOf("intl:") != std::this_thread::get_id());
        CHECK_MSG((sink->eventsWith("storage:") == std::vector<std::string>{"storage:{\"local\":{\"k\":\"v\"}}", "storage:{\"session\":{\"s\":\"w\"}}"}), joined(sink->events()));
        const std::vector<SandboxStats> stats = sink->stats();
        CHECK(!stats.empty() && std::all_of(stats.begin(), stats.end(), [](const SandboxStats& s) { return s.validatorMs == 5; }) && stats.back().tasks >= 2);
        server.close();
        server.close();
        server.event(kTapOnButton);
        CHECK(server.probe("log.length") == "ERROR no runtime");
        CHECK(eventually([&] { return !sink->eventsWith("died:").empty(); }));
        CHECK(sink->eventsWith("died:") == std::vector<std::string>{"died:terminated"});
    }
    auto quiet = std::make_shared<RecordingSink>();
    {
        SandboxRemoteServer server(quiet, RES);
        server.setVisible(false);
        server.event(kTapOnButton);
        server.chargeValidator(5);
        server.close();
        server.start(html, config);
        server.event(kTapOnButton);
        server.close();
        CHECK(server.probe("1") == "ERROR no runtime");
    }
    CHECK_MSG(quiet->events().empty(), joined(quiet->events()));
}

/// `close` returns while the engine waits for the sink's answer to a query (a client may close from the thread that answers it); the destructor waits for that answer and the engine's end before the sink and the delegate go.
TEST(remote_server_close_and_destruction_while_the_engine_waits_for_a_query) {
    {
        Gate gate;
        auto sink = std::make_shared<RecordingSink>();
        sink->duringQuery = [&] { gate.wait(); };
        SandboxRemoteServer server(sink, RES);
        server.start(kMeasure, testConfig());
        CHECK(gate.waitedOn());
        CHECK(returnsWithin<bool>(5, [&] {
            server.close();
            return true;
        }));
        gate.open();
        CHECK(eventually([&] { return !sink->eventsWith("died:").empty(); }));
        const std::vector<std::string> events = sink->events();
        CHECK_MSG(indexOf(events, "query:rect") < indexOf(events, "log:w=77") && indexOf(events, "log:w=77") < indexOf(events, "died:terminated"), joined(events));
    }
    {
        Gate gate;
        auto sink = std::make_shared<RecordingSink>();
        sink->duringQuery = [&] { gate.wait(); };
        auto server = std::make_unique<SandboxRemoteServer>(sink, RES);
        server->start(kMeasure, testConfig());
        CHECK(gate.waitedOn());
        Background<bool> teardown([&] {
            server.reset();
            return true;
        });
        CHECK(!teardown.within(0.2));
        gate.open();
        CHECK(teardown.finish(5));
        const std::vector<std::string> events = sink->events();
        CHECK_MSG(indexOf(events, "log:w=77") < indexOf(events, "died:terminated") && sink->eventsWith("died:").size() == 1, joined(events));
    }
}

/// A client over the loopback runs the real engine: inputs before start pass the coders and the engine's pre-boot rules, the delegate's layout answer and the host's Intl provider reach the widget, storage, a link, a request and the wallet call on its grant go through the client's checks and back by id, the validator's charge comes back in the stats, and a terminate stops both halves.
TEST(remote_loopback_runs_the_real_engine_behind_a_client) {
    const char* html = "<button id='b'>go</button><script>var log = [document.visibilityState, TelegramWidget.contentSafeAreaInset.bottom, TelegramWidget.chatData,"
                       "  document.getElementById('b').getBoundingClientRect().width, (1234.5).toLocaleString()], taps = 0;"
                       "TelegramWidget.onEvent('chatDataChanged', function () { log.push('chat:' + TelegramWidget.chatData); });"
                       "localStorage.setItem('k', 'v');"
                       "document.getElementById('b').onclick = function () { if (++taps === 1) return TelegramWidget.openLink('https://example.com/');"
                       "  TelegramWidget.request('ask', { wallet: true }).then(function (a) { log.push('ok:' + a.result);"
                       "    return TelegramWidget.wallet.signData({ type: 'text', text: 'sign' }, 'w'); })"
                       "  .then(function (b) { log.push('signed:' + b.result); }, function (e) { log.push('err:' + e.code); }); };</script>";
    auto intl = std::make_shared<RecordingIntl>();
    RemoteRecorder recorder;
    intl->into = &recorder;
    recorder.queryAnswer = kRect77;
    SandboxConfig config = walletConfig();
    config.data = WidgetData{std::string("chat1"), std::nullopt};
    SandboxEngine engine(html, config, std::make_shared<SandboxLoopbackChannel>(RES), intl);
    CHECK(engine.isRemote());
    engine.setDelegate(&recorder);
    engine.setVisible(false);
    engine.contentSafeArea(WidgetContentSafeArea{0, 0, 24, 0, 0, 64});
    engine.setData(WidgetData{std::string("chat2"), std::nullopt});
    engine.start();
    CHECK(eventually([&] { return has(recorder.events(), "loaded"); }, 20));
    const std::string booted = "[\"hidden\",24,\"chat1\",77,\"INTL\",\"chat:chat2\"]";
    CHECK_MSG(probeUntil(engine, "JSON.stringify(log)", booted) == booted, engine.probe("JSON.stringify(log)"));
    engine.setVisible(true);
    engine.activationEvent(kTapOnButton, 1);
    const std::string link = linkAction("openLink", "https://example.com/");
    CHECK_MSG(eventually([&] { return has(recorder.events(), link); }), joined(recorder.events()));
    engine.activationEvent(kTapOnButton, 2);
    CHECK(eventually([&] { return recorder.responder(0).has_value(); }));
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
    const std::vector<std::thread::id> threads = recorder.threads();
    CHECK(!threads.empty() && threads[0] != std::this_thread::get_id() && std::all_of(threads.begin(), threads.end(), [&](std::thread::id id) { return id == threads[0]; }));
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

/// The loopback's lifecycle: a stop from the delegate's query answer closes from the dispatch thread while the server's engine waits for it; the owner destroys a client whose dispatch thread is busy with the query's predecessor (the query delivered or answered dead); a form that does not decode (a config without finite viewport, data or an answer over the message limit) is a lost connection.
TEST(remote_loopback_closes_on_the_dispatch_thread_and_tears_down_on_the_owners) {
    {
        RemoteRecorder recorder;
        recorder.duringQuery = [](SandboxEngine& engine) { engine.stop("a counted cap"); };
        auto engine = std::make_unique<SandboxEngine>(kMeasure, testConfig(), std::make_shared<SandboxLoopbackChannel>(RES));
        engine->setDelegate(&recorder);
        engine->start();
        CHECK(eventually([&] { return !recorder.deaths().empty(); }, 20));
        engine->thread().sync([] {});
        const std::vector<std::string> events = recorder.events();
        const std::vector<std::string> queries = recorder.eventsWith("query:");
        CHECK_MSG(queries.size() == 1 && indexOf(events, queries[0]) < indexOf(events, "died:a counted cap") && indexOf(events, "died:a counted cap") < indexOf(events, "query-done") &&
                      events.back() == "query-done" && recorder.deaths().size() == 1,
                  joined(events));
        CHECK(returnsWithin<bool>(5, [&] {
            engine.reset();
            return true;
        }));
    }
    {
        Gate gate;
        std::atomic<bool> held{false}, destroying{false};
        RemoteRecorder recorder;
        recorder.duringOps = [&](SandboxEngine&) {
            if (!held.exchange(true)) gate.wait();
        };
        auto engine = std::make_unique<SandboxEngine>(kMeasure, testConfig(), std::make_shared<SandboxLoopbackChannel>(RES));
        engine->setDelegate(&recorder);
        engine->start();
        CHECK(gate.waitedOn());
        Background<bool> teardown([&] {
            destroying = true;
            engine.reset();
            return true;
        });
        CHECK(eventually([&] { return destroying.load(); }));
        gate.open();
        CHECK(teardown.finish(5));
        const std::vector<std::string> events = recorder.events();
        const size_t asked = recorder.eventsWith("query:").size(), done = recorder.eventsWith("query-done").size();
        CHECK_MSG(has(events, "ops") && asked == done && asked <= 1 && recorder.deaths().empty(), joined(events));
    }
    SandboxConfig broken = testConfig();
    broken.innerWidth = std::numeric_limits<double>::quiet_NaN();
    {
        RemoteRecorder recorder;
        SandboxEngine engine(kMeasure, broken, std::make_shared<SandboxLoopbackChannel>(RES));
        engine.setDelegate(&recorder);
        engine.start();
        CHECK(eventually([&] { return !recorder.deaths().empty(); }));
        engine.thread().sync([] {});
        CHECK_MSG(recorder.events() == std::vector<std::string>{"died:" + kEngineGone}, joined(recorder.events()));
    }
    {
        RemoteRecorder recorder;
        SandboxEngine engine(kMeasure, testConfig(), std::make_shared<SandboxLoopbackChannel>(RES));
        engine.setDelegate(&recorder);
        engine.start();
        CHECK(eventually([&] { return has(recorder.events(), "loaded"); }, 20));
        engine.setData(WidgetData{std::string(SandboxEngine::kRemoteMessageLimit, 'x'), std::nullopt});
        CHECK(eventually([&] { return !recorder.deaths().empty(); }));
        CHECK(recorder.deaths() == std::vector<std::string>{kEngineGone});
    }
    {
        SandboxConfig asking = testConfig();
        asking.context = messageContext(false, true, false, "private");
        RemoteRecorder recorder;
        SandboxEngine engine(kRequester, asking, std::make_shared<SandboxLoopbackChannel>(RES));
        engine.setDelegate(&recorder);
        engine.start();
        CHECK(eventually([&] { return has(recorder.events(), "loaded"); }, 20));
        engine.activationEvent(kTapOnButton, 1);
        CHECK(eventually([&] { return recorder.responder(0).has_value(); }));
        CHECK(recorder.respond(0, answered(std::string(SandboxEngine::kRemoteMessageLimit, 'a'))));
        CHECK(eventually([&] { return !recorder.deaths().empty(); }));
        CHECK(recorder.deaths() == std::vector<std::string>{kEngineGone});
    }
}

/// A host factory's channel: the loopback, noting the thread its calls come from (the client's dispatch thread) at `start`.
class NotingLoopback final : public SandboxRemoteChannel {
public:
    explicit NotingLoopback(std::shared_ptr<SandboxResources> resources) : loopback_(std::move(resources)) {}
    std::thread::id startedOn() {
        std::lock_guard<std::mutex> l(mutex_);
        return startedOn_;
    }

    void attach(std::shared_ptr<SandboxRemoteSink> sink) override { loopback_.attach(std::move(sink)); }
    void start(const std::string& html, const SandboxConfig& config) override {
        {
            std::lock_guard<std::mutex> l(mutex_);
            startedOn_ = std::this_thread::get_id();
        }
        loopback_.start(html, config);
    }
    void event(const std::string& json) override { loopback_.event(json); }
    void activationEvent(const std::string& json, int gesture) override { loopback_.activationEvent(json, gesture); }
    void frame(double time) override { loopback_.frame(time); }
    void viewport(double w, double h, double dpr) override { loopback_.viewport(w, h, dpr); }
    void resize(const std::string& entriesJson) override { loopback_.resize(entriesJson); }
    void theme(const std::string& scheme, const std::map<std::string, std::string>& params) override { loopback_.theme(scheme, params); }
    void contentSafeArea(const WidgetContentSafeArea& area) override { loopback_.contentSafeArea(area); }
    void setData(const WidgetData& data) override { loopback_.setData(data); }
    void setVisible(bool visible) override { loopback_.setVisible(visible); }
    void pressBegan() override { loopback_.pressBegan(); }
    void pressEnded() override { loopback_.pressEnded(); }
    void chargeValidator(double ms) override { loopback_.chargeValidator(ms); }
    void stop(const std::string& reason) override { loopback_.stop(reason); }
    void answer(int64_t id, const SandboxRemoteOutcome& outcome) override { loopback_.answer(id, outcome); }
    std::string probe(const std::string& code) override { return loopback_.probe(code); }
    void close() override { loopback_.close(); }

private:
    std::mutex mutex_;
    std::thread::id startedOn_;
    SandboxLoopbackChannel loopback_;
};

/// Swift's testLoopbackRunsAWidgetThroughTheHost: WidgetHost asks a host's factory for its engine once (with Debug's switch set too: the factory wins) and runs the widget behind the loopback; its DOM change, the renderer's layout answer and the host's Intl provider, called on the client's dispatch thread, come back as validated ops. A factory without a channel gives a host whose widget died; with none set none is asked.
TEST(remote_loopback_runs_a_widget_through_widget_host) {
    FactoryReset reset;
    std::mutex m;
    int built = 0;
    std::shared_ptr<NotingLoopback> channel;
    SandboxEngine::setRemoteChannelFactory([&](std::shared_ptr<SandboxResources> resources) -> std::shared_ptr<SandboxRemoteChannel> {
        auto made = std::make_shared<NotingLoopback>(std::move(resources));
        std::lock_guard<std::mutex> l(m);
        built++;
        channel = made;
        return made;
    });
    {
        RemoteRecorder provided;
        auto intl = std::make_shared<RecordingIntl>();
        intl->into = &provided;
        ScriptedRenderer* renderer = nullptr;
        WidgetHost host("<div id='d'>a</div><p id='n'>b</p><script>var d = document.getElementById('d'); d.style.height = '50px'; d.textContent = 'h:' + d.offsetHeight;"
                        "document.getElementById('n').textContent = 'n:' + (1234.5).toLocaleString();</script>",
                        WidgetTheme{}, 300, WidgetHostOptions(), RES, scripted(renderer), intl);
        renderer->queryResult = "{\"x\":0,\"y\":0,\"width\":100,\"height\":20,\"offsetHeight\":20}";
        std::string rendered;
        host.onRenderOps = [&](const std::string& ops) {
            std::lock_guard<std::mutex> l(m);
            rendered += ops;
        };
        host.start();
        CHECK(host.waitLoaded(20) && host.waitRendered(5));
        const WidgetDiagnostics d = host.diagnostics();
        CHECK_MSG(d.loaded && d.rendered && !d.dead, d.dead.value_or("alive"));
        std::thread::id hostThread;
        host.onHostThread([&] { hostThread = std::this_thread::get_id(); });
        std::thread::id dispatch;
        {
            std::lock_guard<std::mutex> l(m);
            CHECK_MSG(built == 1 && channel, std::to_string(built) + " channels asked for");
            CHECK_MSG(rendered.find("\"h:20\"") != std::string::npos && rendered.find("\"n:INTL\"") != std::string::npos, rendered.substr(0, 2000));
            if (channel) dispatch = channel->startedOn();
        }
        const std::vector<std::string> asked = provided.events();
        const std::vector<std::thread::id> on = provided.threads();
        CHECK_MSG(asked.size() == 1 && asked[0].find("\"kind\":\"number\"") != std::string::npos && asked[0].find("1234.5") != std::string::npos, joined(asked));
        CHECK(dispatch != std::thread::id() && dispatch != hostThread && dispatch != std::this_thread::get_id());
        CHECK(!on.empty() && std::all_of(on.begin(), on.end(), [&](std::thread::id id) { return id == dispatch; }));
    }
    SandboxEngine::setRemoteChannelFactory([&](std::shared_ptr<SandboxResources>) {
        std::lock_guard<std::mutex> l(m);
        built++;
        return std::shared_ptr<SandboxRemoteChannel>();
    });
    {
        ScriptedRenderer* renderer = nullptr;
        WidgetHost refused("<script>console.log('ran here')</script>", WidgetTheme{}, 300, WidgetHostOptions(), RES, scripted(renderer));
        refused.start();
        CHECK(refused.waitUntil([](const WidgetDiagnostics& d) { return d.dead.has_value(); }, 5));
        const WidgetDiagnostics d = refused.diagnostics();
        CHECK_MSG(d.dead == std::optional<std::string>(kEngineGone) && !d.loaded && !d.hasLog("ran here"), d.dead.value_or("alive"));
        CHECK(refused.probe("1") == "ERROR no runtime");
    }
    SandboxEngine::setRemoteChannelFactory(nullptr);
    {
        ScriptedRenderer* renderer = nullptr;
        WidgetHost plain("<script>console.log('ran here')</script>", WidgetTheme{}, 300, WidgetHostOptions(), RES, scripted(renderer));
        plain.start();
        CHECK(plain.waitLoaded(20) && plain.waitUntil([](const WidgetDiagnostics& d) { return d.hasLog("ran here"); }, 5));
        CHECK(!plain.diagnostics().dead);
    }
    std::lock_guard<std::mutex> l(m);
    CHECK_MSG(built == 2, std::to_string(built) + " channels asked for");
}

/// Whether the client already sent the engine's side its `start`.
static bool startedOver(FakeRemoteChannel& channel) {
    const std::vector<std::string> calls = channel.calls();
    return !calls.empty() && calls[0].rfind("start:", 0) == 0;
}

/// Every call a host's client-mode engine queued has reached the delegate, and the host thread has run what that sent it (a started, live widget's probe flushes the dispatch thread; a dead or unstarted one's too).
static void settleHost(WidgetHost& host) {
    host.probe("0");
    host.onHostThread([] {});
}

/// The diagnostics a WidgetHost's observer saw, in order, under a lock (`onChange` runs on the host thread).
struct ObservedChanges {
    std::mutex mutex;
    std::vector<WidgetDiagnostics> seen;
    std::function<void(const WidgetDiagnostics&)> observer() {
        return [this](const WidgetDiagnostics& d) {
            std::lock_guard<std::mutex> l(mutex);
            seen.push_back(d);
        };
    }
    std::vector<WidgetDiagnostics> all() {
        std::lock_guard<std::mutex> l(mutex);
        return seen;
    }
};

/// A hostile engine's task counts reach WidgetHost's clearing of the last error (an `Uncaught` one stays until a task after the next): at the top of `int` the comparison does not overflow (UBSan), and small counts clear it as before.
TEST(remote_widget_host_compares_hostile_task_counts_without_overflow) {
    FactoryReset reset;
    auto channel = std::make_shared<FakeRemoteChannel>();
    SandboxEngine::setRemoteChannelFactory([&](std::shared_ptr<SandboxResources>) -> std::shared_ptr<SandboxRemoteChannel> { return channel; });
    ScriptedRenderer* renderer = nullptr;
    WidgetHost host("<p>x</p>", WidgetTheme{}, 300, WidgetHostOptions(), RES, scripted(renderer));
    host.start();
    CHECK(eventually([&] { return startedOver(*channel); }));
    SandboxRemoteSink& sink = *channel->sink();
    const auto stats = [&](int tasks) {
        SandboxStats s;
        s.tasks = tasks;
        sink.remoteStats(s);
    };
    const auto lastError = [&] {
        settleHost(host);
        return host.diagnostics().lastError;
    };
    const int top = std::numeric_limits<int>::max();
    stats(top);
    sink.remoteLog("error", "Uncaught x");
    stats(top);
    CHECK(lastError() == std::optional<std::string>("Uncaught x") && host.diagnostics().stats.tasks == top);
    stats(3);
    sink.remoteLog("error", "Uncaught y");
    stats(4);
    CHECK(lastError() == std::optional<std::string>("Uncaught y"));
    stats(5);
    CHECK(!lastError());
    CHECK(!host.diagnostics().dead);
}

/// WidgetHost's "nothing runs until start()" with an engine behind a channel that failed already: the factory gave none, or the channel reported its disconnection while it attached. Before start nothing reaches the host, its observer or the renderer, however long it is left; the observer and stop label set then (as Telegram's Build sets them) are what the one death meets after start. Set at once, with no wait between, the same holds (TSan: no access races the owner's writes).
TEST(remote_widget_host_hears_a_failed_setup_only_after_start) {
    FactoryReset reset;
    for (const bool noChannel : {true, false}) {
        for (const bool waitFirst : {true, false}) {
            auto channel = std::make_shared<FakeRemoteChannel>();
            channel->disconnectOnAttach = true;
            std::atomic<int> asked{0};
            SandboxEngine::setRemoteChannelFactory([&](std::shared_ptr<SandboxResources>) -> std::shared_ptr<SandboxRemoteChannel> {
                asked++;
                return noChannel ? nullptr : channel;
            });
            const std::string label = std::string("stopped: ") + (noChannel ? "no channel" : "lost at attach");
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
            host.stoppedLabel = label;
            host.onChange = observed.observer();
            host.start();
            CHECK(host.waitUntil([](const WidgetDiagnostics& d) { return d.dead.has_value(); }, 5));
            settleHost(host);
            const WidgetDiagnostics after = host.diagnostics();
            CHECK_MSG(after.dead == std::optional<std::string>(kEngineGone) && !after.loaded && !after.hasLog("ran here"), after.dead.value_or("alive"));
            const std::vector<Json> frozen = renderer->sent("frozen");
            CHECK_MSG(frozen.size() == 1 && frozen[0]["reason"].asString() == label && frozen[0]["fatal"].asBool(), frozen.empty() ? "no frozen" : frozen[0].dump());
            const std::vector<WidgetDiagnostics> seen = observed.all();
            CHECK_MSG(seen.size() == 1 && seen[0].dead == std::optional<std::string>(kEngineGone), std::to_string(seen.size()) + " changes seen");
            CHECK(asked == 1 && channel->calls().empty() && channel->closes() == (noChannel ? 0 : 1));
        }
    }
}

/// The control: a channel that works. What its transport sends before start reaches nothing (only a death would be kept); after start the widget runs, the owner's observer hears it, and nothing dies.
TEST(remote_widget_host_starts_a_working_channel_after_its_setup) {
    FactoryReset reset;
    auto channel = std::make_shared<FakeRemoteChannel>();
    SandboxEngine::setRemoteChannelFactory([&](std::shared_ptr<SandboxResources>) -> std::shared_ptr<SandboxRemoteChannel> { return channel; });
    ObservedChanges observed;
    ScriptedRenderer* renderer = nullptr;
    WidgetHost host("<p>x</p>", WidgetTheme{}, 300, WidgetHostOptions(), RES, scripted(renderer));
    SandboxRemoteSink& sink = *channel->sink();
    sink.remoteLog("error", "Uncaught early");
    sink.remoteDiag("early", "before start");
    sink.remoteLoaded();
    settleHost(host);
    {
        const WidgetDiagnostics before = host.diagnostics();
        CHECK(!before.dead && !before.loaded && before.logs.empty() && before.diags.empty() && channel->calls().empty());
        std::lock_guard<std::mutex> l(renderer->mutex);
        CHECK(renderer->messages.empty());
    }
    host.stoppedLabel = "stopped: working";
    host.onChange = observed.observer();
    host.start();
    CHECK(eventually([&] { return startedOver(*channel); }));
    sink.remoteLog("log", "after start");
    sink.remoteLoaded();
    CHECK(host.waitLoaded(5));
    settleHost(host);
    const WidgetDiagnostics after = host.diagnostics();
    CHECK_MSG(after.loaded && !after.dead && after.hasLog("after start") && !after.hasLog("Uncaught early") && after.countDiag("early") == 0, after.dead.value_or("alive"));
    const std::vector<WidgetDiagnostics> seen = observed.all();
    CHECK(!seen.empty() && seen.back().loaded && std::none_of(seen.begin(), seen.end(), [](const WidgetDiagnostics& d) { return d.dead.has_value(); }));
    CHECK(renderer->sent("frozen").empty() && !renderer->sent("theme").empty());
}

/// A WidgetHost over a fake channel, started (its delegate is in once the channel heard `start`); without `ready` its renderer stays quiet until `announce`, and the delegate is in once the client took it, the engine not started.
struct RemoteHostRun {
    FactoryReset reset;
    std::shared_ptr<FakeRemoteChannel> channel = std::make_shared<FakeRemoteChannel>();
    ScriptedRenderer* renderer = nullptr;
    std::optional<WidgetHost> host;
    /// `observe` sets the host's observers before it starts.
    explicit RemoteHostRun(const std::function<void(WidgetHost&)>& observe = nullptr, bool ready = true, const WidgetHostOptions& options = WidgetHostOptions()) {
        SandboxEngine::setRemoteChannelFactory([channel = channel](std::shared_ptr<SandboxResources>) -> std::shared_ptr<SandboxRemoteChannel> { return channel; });
        host.emplace("<p>x</p>", WidgetTheme{}, 300, options, RES, scripted(renderer));
        renderer->readyOnLoad = ready;
        if (observe) observe(*host);
        host->start();
        if (ready) {
            CHECK(eventually([&] { return startedOver(*channel); }));
            return;
        }
        // start()'s closure handed the delegate to the client; an unstarted client's probe answers after its dispatch thread took it, asking the channel nothing.
        host->onHostThread([] {});
        CHECK(host->probe("0") == "ERROR no runtime");
    }
    SandboxRemoteSink& sink() { return *channel->sink(); }
    /// The quiet renderer's `ready`, then the host settled: what it kept sent, the engine started unless it was cut.
    void announce() {
        renderer->onMessage("{\"type\":\"ready\"}");
        host->onHostThread([] {});
        settleHost(*host);
    }
    /// The types of what the renderer got, in order.
    std::vector<std::string> sentTypes() {
        std::lock_guard<std::mutex> l(renderer->mutex);
        std::vector<std::string> out;
        for (const std::string& m : renderer->messages) out.push_back(Json::parse(m).value_or(Json())["type"].asString());
        return out;
    }
};

/// `host`'s thread held on a gate from a worker of the test's until `release`, as a slow or stuck host: what the host queues meanwhile waits, while the client's dispatch thread runs on. Declared after the host, so it lets go first.
struct HeldHost {
    Gate gate;
    Background<bool> worker;
    explicit HeldHost(WidgetHost& host) : worker([this, &host] {
        host.onHostThread([this] { gate.wait(); });
        return true;
    }) {}
    bool held() { return gate.waitedOn(); }
    bool release() {
        gate.open();
        return worker.finish(5);
    }
};

/// The engine's side stopped once, last, as a flood ends it.
static bool stoppedAsFlooded(FakeRemoteChannel& channel) {
    const std::vector<std::string> calls = channel.calls();
    return !calls.empty() && calls.back() == "stop:" + kEngineFlooded && std::count_if(calls.begin(), calls.end(), [](const std::string& c) { return c.rfind("stop:", 0) == 0; }) == 1;
}

/// With the host's thread held and the client's dispatch thread free, what WidgetHost queued for the engine's messages stays counted until it runs. Bursts of 1000 legal 4000-byte logs, each under both bounds and followed by an Intl answer (it comes from the dispatch thread after every earlier message and needs no host), pile up in the host's queue; frame requests and diagnostics count as calls. At the call bound, the barrier's own call included, Intl still answers; the frame request past it fails the widget once: the engine's side is stopped and closed while the host is held, its death waiting behind the accepted work, queries and Intl are refused at once and nothing more is taken. Let go, the host delivers that work in order, then the death, and gives every place back.
TEST(remote_widget_host_keeps_what_it_queued_counted_until_it_runs) {
    RemoteHostRun r;
    WidgetHost& host = *r.host;
    SandboxRemoteSink& sink = r.sink();
    HeldHost held(host);
    CHECK(held.held());
    const std::string text(SandboxEngine::kRemoteTextLimit, 'x');
    const size_t logBytes = 3 + text.size(), bursts = 16, burst = 1000;
    for (size_t b = 1; b <= bursts; b++) {
        for (size_t i = 0; i < burst; i++) sink.remoteLog("log", text);
        const std::string answer = sink.remoteIntl(kNumberRequest);
        CHECK_MSG(answer == "1,234.5" && backlogIs(host, b * burst * logBytes, b * burst), answer + ", " + backlogText(host));
    }
    const size_t small = SandboxEngine::kRemoteQueuedCalls - bursts * burst - 1;
    for (size_t i = 0; i < small; i++) {
        if (i % 2) sink.remoteRequestsFrame();
        else sink.remoteDiag("d", "");
    }
    const size_t diags = (small + 1) / 2, frames = small / 2, heldBytes = bursts * burst * logBytes + diags;
    CHECK(sink.remoteIntl(kNumberRequest) == "1,234.5");
    CHECK_MSG(backlogIs(host, heldBytes, SandboxEngine::kRemoteQueuedCalls - 1), backlogText(host));
    sink.remoteRequestsFrame();
    CHECK_MSG(backlogIs(host, heldBytes, SandboxEngine::kRemoteQueuedCalls), backlogText(host));
    sink.remoteRequestsFrame();
    CHECK(eventually([&] { return r.channel->closes() == 1; }));
    CHECK_MSG(stoppedAsFlooded(*r.channel), joined(r.channel->calls()));
    CHECK(!host.diagnostics().dead);
    const std::string refused = returnsWithin<std::string>(5, [&] { return sink.remoteQuery("{}").json + "|" + sink.remoteIntl(kNumberRequest); });
    CHECK_MSG(refused == "{\"error\":\"dead\"}|", refused);
    sink.remoteLog("log", text);
    sink.remoteRequestsFrame();
    sink.remoteOps("[]");
    sink.remoteDisconnected();
    CHECK_MSG(backlogIs(host, heldBytes, SandboxEngine::kRemoteQueuedCalls), backlogText(host));
    CHECK(held.release());
    settleHost(host);
    const WidgetDiagnostics d = host.diagnostics();
    CHECK_MSG(d.dead == std::optional<std::string>(kEngineFlooded) && d.logs.size() == 5000 && d.countDiag("d") == int(diags), d.dead.value_or("alive"));
    const std::vector<std::string> sent = r.sentTypes();
    CHECK_MSG(size_t(std::count(sent.begin(), sent.end(), "requestFrame")) == frames + 1 && !sent.empty() && sent.back() == "frozen" && std::count(sent.begin(), sent.end(), "frozen") == 1,
              std::to_string(sent.size()) + " renderer messages");
    CHECK_MSG(backlogIs(host, 0, 0), backlogText(host));
    CHECK(r.channel->closes() == 1);
}

/// The same by bytes, through the validator: an ops batch it refuses op by op keeps its own bytes and each diagnostic it reported (counted on their own, as calls); full batches fill the byte bound to the barrier's last byte, and one byte past it fails the widget once while the host is held. Let go, the host has the 3000 diagnostics and the death, and gives every place back.
TEST(remote_widget_host_counts_held_ops_and_their_diagnostics_by_their_bytes) {
    RemoteHostRun r;
    WidgetHost& host = *r.host;
    SandboxRemoteSink& sink = r.sink();
    HeldHost held(host);
    CHECK(held.held());
    const int rejected = 3000;
    std::string batch = "[";
    for (int i = 0; i < rejected; i++) batch += i ? ",{}" : "{}";
    batch += "]";
    const size_t perDiag = std::string("protocol-violation").size() + std::string("rejected op {}").size();
    sink.remoteOps(batch);
    CHECK(sink.remoteIntl(kNumberRequest) == "1,234.5");
    size_t heldBytes = batch.size() + rejected * perDiag;
    CHECK_MSG(backlogIs(host, heldBytes, rejected + 1), backlogText(host));
    sink.remoteOps(paddedTo("[]", SandboxEngine::kRemoteOpsLimit));
    CHECK(sink.remoteIntl(kNumberRequest) == "1,234.5");
    heldBytes += SandboxEngine::kRemoteOpsLimit;
    CHECK_MSG(backlogIs(host, heldBytes, rejected + 2), backlogText(host));
    const size_t rest = SandboxEngine::kRemoteQueuedBytes - heldBytes - kNumberRequest.size();
    sink.remoteOps(paddedTo("[]", rest));
    CHECK(sink.remoteIntl(kNumberRequest) == "1,234.5");
    heldBytes += rest;
    CHECK_MSG(backlogIs(host, heldBytes, rejected + 3), backlogText(host));
    sink.remoteDiag("k", std::string(kNumberRequest.size(), 'x'));
    CHECK(eventually([&] { return r.channel->closes() == 1; }));
    CHECK_MSG(stoppedAsFlooded(*r.channel), joined(r.channel->calls()));
    CHECK(!host.diagnostics().dead);
    const std::string refused = returnsWithin<std::string>(5, [&] { return sink.remoteQuery("{}").json + "|" + sink.remoteIntl(kNumberRequest); });
    CHECK_MSG(refused == "{\"error\":\"dead\"}|", refused);
    sink.remoteOps(batch);
    CHECK_MSG(backlogIs(host, heldBytes, rejected + 3), backlogText(host));
    CHECK(held.release());
    settleHost(host);
    const WidgetDiagnostics d = host.diagnostics();
    CHECK_MSG(d.dead == std::optional<std::string>(kEngineFlooded) && d.countDiag("protocol-violation") == rejected && d.countDiag("k") == 0, d.dead.value_or("alive") + ", " + std::to_string(d.countDiag("protocol-violation")));
    CHECK_MSG(backlogIs(host, 0, 0), backlogText(host));
}

/// The control, below the bounds: with the host held, a round (an `Uncaught` log, a ready call with its raw copy, one storage commit of both areas, a diagnostic, the first time a query that times out while it waits behind them, frame requests up to the call bound but the barrier's call, a last `Uncaught` log) is counted as it is queued; let go, it comes in the engine's order, nothing dies, and every place is given back, so the same round fits again. Work queued before `destroy()` and run after it, doing nothing, gives its place back too.
TEST(remote_widget_host_gives_back_what_it_held_as_it_runs) {
    std::mutex m;
    std::vector<std::string> order;
    const auto note = [&](const std::string& event) {
        std::lock_guard<std::mutex> l(m);
        order.push_back(event);
    };
    RemoteHostRun r([&](WidgetHost& host) {
        host.onError = [&](const std::string& text) { note(text); };
        host.onAction = [&](const WidgetAction& action) { note("action:" + action.toJson()["kind"].asString()); };
        host.onStorageCommit = [&](const std::string& area, const std::map<std::string, std::string>&) { note("storage:" + area); };
    });
    WidgetHost& host = *r.host;
    SandboxRemoteSink& sink = r.sink();
    const std::string commit = "{\"local\":{\"a\":\"b\"},\"session\":{\"c\":\"d\"}}", query = "{\"q\":\"viewport\"}";
    size_t framesBefore = 0;
    for (int round = 0; round < 2; round++) {
        {
            std::lock_guard<std::mutex> l(m);
            order.clear();
        }
        HeldHost held(host);
        CHECK(held.held());
        sink.remoteLog("error", "Uncaught first");
        sink.remoteBridge("ready", "[]");
        sink.remoteStorage(commit);
        sink.remoteDiag("d", "x");
        size_t bytes = (5 + 14) + 2 * (5 + 2) + commit.size() + 2, calls = 5;
        if (round == 0) {
            CHECK(sink.remoteQuery(query).json == "{\"error\":\"timeout\"}");
            bytes += query.size();
            calls++;
        }
        const size_t frames = SandboxEngine::kRemoteQueuedCalls - 1 - calls - 1;
        for (size_t i = 0; i < frames; i++) sink.remoteRequestsFrame();
        sink.remoteLog("error", "Uncaught last");
        bytes += 5 + 13;
        calls += frames + 1;
        CHECK(sink.remoteIntl(kNumberRequest) == "1,234.5");
        CHECK_MSG(backlogIs(host, bytes, calls) && calls == SandboxEngine::kRemoteQueuedCalls - 1, backlogText(host));
        CHECK(held.release());
        settleHost(host);
        CHECK_MSG(backlogIs(host, 0, 0), backlogText(host));
        const WidgetDiagnostics d = host.diagnostics();
        CHECK_MSG(!d.dead && d.actions.size() == size_t(round + 1) && d.bridge.size() == size_t(round + 1) && d.storageCommits == 2 * (round + 1) && d.countDiag("d") == round + 1, d.dead.value_or("alive"));
        {
            std::lock_guard<std::mutex> l(m);
            CHECK_MSG((order == std::vector<std::string>{"Uncaught first", "action:ready", "storage:local", "storage:session", "Uncaught last"}), joined(order));
        }
        const std::vector<std::string> sent = r.sentTypes();
        const size_t framesNow = size_t(std::count(sent.begin(), sent.end(), "requestFrame"));
        CHECK_MSG(framesNow == framesBefore + frames && std::count(sent.begin(), sent.end(), "query") == 1, std::to_string(framesNow) + " frame requests");
        framesBefore = framesNow;
    }
    CHECK(r.channel->closes() == 0 && r.channel->calls().size() >= 1 && !stoppedAsFlooded(*r.channel));
    HeldHost held(host);
    CHECK(held.held());
    for (int i = 0; i < 100; i++) sink.remoteRequestsFrame();
    sink.remoteLog("log", "before destroy");
    CHECK(sink.remoteIntl(kNumberRequest) == "1,234.5");
    CHECK_MSG(backlogIs(host, 3 + 14, 101), backlogText(host));
    Background<bool> destroying([&] {
        host.destroy();
        return true;
    });
    CHECK(held.release());
    CHECK(destroying.finish(5));
    host.onHostThread([] {});
    CHECK_MSG(backlogIs(host, 0, 0), backlogText(host));
    CHECK(host.diagnostics().hasLog("before destroy"));
}

static const std::string kBacklogLimitReason = "the widget engine process sent more than the host keeps before its renderer is ready";
/// The engine's side's paragraph (id 2), then as the validator sends it on.
static const std::string kParagraph = "[{\"op\":\"el\",\"id\":2,\"tag\":\"p\"}]";
static const std::string kParagraphRendered = "[{\"op\":\"el\",\"id\":2,\"tag\":\"p\",\"attrs\":[]}]";
/// A CSS animation of that paragraph, valid as it comes.
static const std::string kParagraphAnim =
    "[{\"aid\":1,\"target\":2,\"css\":{\"kind\":\"animation\",\"name\":\"spin\",\"origin\":0},\"startTime\":null,\"holdTime\":null,\"playbackRate\":1,\"cancelled\":false}]";

static bool isDead(const WidgetDiagnostics& d) { return d.dead.has_value(); }

/// `frames` frame requests from `r`'s engine, the host settled after every 1000 so the client's own bounds never fill.
static void sendFrames(RemoteHostRun& r, size_t frames) {
    for (size_t i = 1; i <= frames; i++) {
        r.sink().remoteRequestsFrame();
        if (i % 1000 == 0) settleHost(*r.host);
    }
    settleHost(*r.host);
}

/// The calls of `channel` that begin with `prefix`, in order.
static std::vector<std::string> callsWith(FakeRemoteChannel& channel, const std::string& prefix) {
    std::vector<std::string> out;
    for (const std::string& c : channel.calls())
        if (c.rfind(prefix, 0) == 0) out.push_back(c);
    return out;
}

/// The engine's side was told to stop once, for the backlog, and never to start.
static bool stoppedForTheBacklog(FakeRemoteChannel& channel) {
    return callsWith(channel, "stop:") == std::vector<std::string>{"stop:" + kBacklogLimitReason} && callsWith(channel, "start:").empty();
}

/// A trusted tap through `r`'s renderer, a grant once the client's dispatch thread had it.
static void tapThrough(RemoteHostRun& r) {
    r.renderer->onMessage("{\"type\":\"event\",\"ev\":" + kTapOnButton + ",\"trusted\":true}");
    r.host->onHostThread([] {});
    r.host->probe("0");
}

/// A renderer message as its type and what tells it from the others of that type (an anim's clock left out).
static std::string described(const std::string& message) {
    const Json m = Json::parse(message).value_or(Json());
    const std::string& type = m["type"].asString();
    if (type == "theme") return "theme:" + m["theme"]["colorScheme"].asString() + m["theme"]["params"].dump();
    if (type == "safeArea") return "safeArea:" + m["inset"].dump();
    if (type == "policy") return "policy:" + m["policy"].asString();
    if (type == "ops") return "ops:" + m["ops"].dump();
    if (type == "listen") return "listen:" + m["types"].dump();
    if (type == "preventDefault") return "preventDefault:" + m["keys"].dump();
    if (type == "anim") return "anim:" + m["anims"].dump();
    if (type == "query") return "query:" + m["qid"].dump() + m["query"].dump();
    if (type == "frozen") return "frozen:" + m["reason"].asString();
    return type;
}

/// What `r`'s renderer got, `described`, a run of one message as one entry with its count ("requestFrame x3").
static std::vector<std::string> sentRuns(RemoteHostRun& r) {
    std::vector<std::string> messages;
    {
        std::lock_guard<std::mutex> l(r.renderer->mutex);
        messages = r.renderer->messages;
    }
    std::vector<std::pair<std::string, size_t>> runs;
    for (const std::string& m : messages) {
        std::string d = described(m);
        if (!runs.empty() && runs.back().first == d) runs.back().second++;
        else runs.emplace_back(std::move(d), 1);
    }
    std::vector<std::string> out;
    for (const auto& [d, n] : runs) out.push_back(n == 1 ? d : d + " x" + std::to_string(n));
    return out;
}

/// What a quiet renderer's host queued at start, before anything else: the default theme, no safe area, strict.
static std::vector<std::string> startMessages() { return {"theme:light{}", "safeArea:" + WidgetContentSafeArea{}.json().dump(), "policy:strict"}; }

/// The sizes of `got` and `expected` and their first difference, for a failure.
static std::string firstDifference(const std::vector<std::string>& got, const std::vector<std::string>& expected) {
    size_t i = 0;
    while (i < got.size() && i < expected.size() && got[i] == expected[i]) i++;
    return std::to_string(got.size()) + " entries for " + std::to_string(expected.size()) + ", first difference at " + std::to_string(i) + ": " +
           (i < got.size() ? got[i].substr(0, 200) : "(none)") + " for " + (i < expected.size() ? expected[i].substr(0, 200) : "(none)");
}

/// A quiet renderer's host keeps what a client-mode engine sends before `ready` up to exactly the message bound (validated ops, listen, preventDefault, an anim, the loaded ack, frame requests), the owner's safe area and theme among them and at the bound too, and nothing dies; at `ready` the renderer gets the host's messages, then all of it in its order, the ack marks the widget rendered, and the engine's later output goes on at once.
TEST(remote_widget_host_keeps_an_early_engines_messages_in_order_up_to_the_bound) {
    std::mutex m;
    std::vector<std::string> rendered;
    RemoteHostRun r([&](WidgetHost& host) {
        host.onRenderOps = [&](const std::string& ops) {
            std::lock_guard<std::mutex> l(m);
            rendered.push_back(ops);
        };
    }, false);
    WidgetHost& host = *r.host;
    SandboxRemoteSink& sink = r.sink();
    const WidgetContentSafeArea first{1, 2, 3, 4, 5, 6}, last{7, 8, 9, 10, 11, 12};
    WidgetTheme dark, tinted;
    dark.colorScheme = "dark";
    tinted.params = {{"bg_color", "#123456"}};
    sink.remoteOps(kParagraph);
    settleHost(host);
    host.setContentSafeArea(first);
    sink.remoteListen("[\"click\"]");
    sink.remotePreventDefault("[\"2:click\"]");
    sink.remoteAnim(kParagraphAnim);
    sink.remoteLoaded();
    const size_t frames = WidgetHost::kRendererBacklogMessages - 5, early = frames / 2;
    sendFrames(r, early);
    host.setTheme(dark);
    sendFrames(r, frames - early);
    host.setTheme(tinted);
    host.setContentSafeArea(last);
    settleHost(host);
    const WidgetDiagnostics before = host.diagnostics();
    CHECK_MSG(!before.dead && before.diags.empty() && before.loaded && !before.rendered, before.dead.value_or("alive"));
    CHECK(r.sentTypes().empty() && callsWith(*r.channel, "start:").empty());
    r.announce();
    std::vector<std::string> expected = startMessages();
    expected.insert(expected.end(), {"ops:" + kParagraphRendered, "safeArea:" + first.json().dump(), "listen:[\"click\"]", "preventDefault:[\"2:click\"]", "anim:" + kParagraphAnim,
                                     "query:-1{\"q\":\"viewport\"}", "requestFrame x" + std::to_string(early), "theme:dark{}", "ops:[]",
                                     "requestFrame x" + std::to_string(frames - early), "theme:light{\"bg_color\":\"#123456\"}", "ops:[]", "safeArea:" + last.json().dump()});
    std::vector<std::string> sent = sentRuns(r);
    CHECK_MSG(sent == expected, firstDifference(sent, expected));
    const WidgetDiagnostics at = host.diagnostics();
    CHECK_MSG(at.rendered && !at.dead && at.diags.empty(), at.dead.value_or("alive"));
    CHECK_MSG(callsWith(*r.channel, "start:").size() == 1, joined(r.channel->calls()));
    const std::string later = "[{\"op\":\"text\",\"id\":3,\"data\":\"later\"}]";
    sink.remoteRequestsFrame();
    sink.remoteOps(later);
    settleHost(host);
    expected.insert(expected.end(), {"requestFrame", "ops:" + later});
    sent = sentRuns(r);
    CHECK_MSG(sent == expected, firstDifference(sent, expected));
    CHECK(!host.diagnostics().dead);
    std::lock_guard<std::mutex> l(m);
    CHECK_MSG((rendered == std::vector<std::string>{kParagraphRendered, later}), joined(rendered));
}

/// The bytes alone bound it, through the strict validator before `loaded`: after a paragraph, batches of its class, 8000 characters a value (under strict's attribute cap, and no text its budgets count), each validated as it came and given back before the next, fill what the host keeps, wrappers included, to exactly the byte bound in under a hundred messages, and the widget lives; a small batch more stops it once, never reported to `onRenderOps`, with the backlog's diagnostic and death, one stop and one close, and a late `ready` gets the host's messages and the badge only.
TEST(remote_widget_host_bounds_an_early_engines_messages_by_their_bytes) {
    const size_t wrapper = std::string("{\"type\":\"ops\",\"ops\":}").size();
    std::mutex m;
    size_t keptBytes = 0, batches = 0, lastOps = 0;
    RemoteHostRun r([&](WidgetHost& host) {
        host.onRenderOps = [&](const std::string& ops) {
            std::lock_guard<std::mutex> l(m);
            keptBytes += wrapper + ops.size();
            batches++;
            lastOps = ops.size();
        };
    }, false);
    WidgetHost& host = *r.host;
    SandboxRemoteSink& sink = r.sink();
    const size_t value = 8000, perBatch = 128;
    const auto op = [](size_t length) { return "{\"op\":\"attr\",\"id\":2,\"name\":\"class\",\"value\":\"" + std::string(length, 'x') + "\"}"; };
    const auto batchOf = [&](size_t full, std::optional<size_t> tail) {
        std::string out = "[";
        for (size_t i = 0; i < full; i++) out += (i ? "," : "") + op(value);
        if (tail) out += (full ? "," : "") + op(*tail);
        return out + "]";
    };
    size_t sent = 0, count = 0;
    bool sizesMatch = true;
    const auto send = [&](const std::string& ops, size_t validated) {
        sink.remoteOps(ops);
        settleHost(host);
        sent += wrapper + validated;
        count++;
        std::lock_guard<std::mutex> l(m);
        sizesMatch = sizesMatch && lastOps == validated && batches == count;
    };
    send(kParagraph, kParagraphRendered.size());
    const std::string batch = batchOf(perBatch, std::nullopt);
    while (sent + wrapper + batch.size() <= WidgetHost::kRendererBacklogBytes) send(batch, batch.size());
    const size_t rest = WidgetHost::kRendererBacklogBytes - sent, perOp = op(value).size() + 1, tailBase = wrapper + 2 + op(0).size();
    const size_t whole = (rest - tailBase) / perOp;
    const std::string last = batchOf(whole, rest - tailBase - whole * perOp);
    CHECK_MSG(wrapper + last.size() == rest, std::to_string(last.size()) + " for " + std::to_string(rest));
    send(last, last.size());
    {
        std::lock_guard<std::mutex> l(m);
        CHECK_MSG(sizesMatch && keptBytes == WidgetHost::kRendererBacklogBytes && batches * 100 < WidgetHost::kRendererBacklogMessages,
                  std::to_string(keptBytes) + " bytes in " + std::to_string(batches) + " batches");
    }
    const WidgetDiagnostics atBound = host.diagnostics();
    CHECK_MSG(!atBound.dead && atBound.diags.empty() && callsWith(*r.channel, "stop:").empty(), atBound.dead.value_or("alive"));
    sink.remoteOps("[{\"op\":\"attr\",\"id\":2,\"name\":\"class\",\"value\":\"y\"}]");
    CHECK(host.waitUntil(isDead, 5));
    settleHost(host);
    const WidgetDiagnostics d = host.diagnostics();
    const auto limit = d.diags.find("backlog-limit");
    CHECK_MSG(d.dead == std::optional<std::string>(kBacklogLimitReason) && d.diags.size() == 1 && limit != d.diags.end() &&
                  limit->second == (std::pair<int, std::vector<std::string>>{1, {kBacklogLimitReason}}),
              d.dead.value_or("alive"));
    CHECK_MSG(stoppedForTheBacklog(*r.channel) && r.channel->closes() == 1, joined(callsWith(*r.channel, "stop:")));
    {
        std::lock_guard<std::mutex> l(m);
        CHECK_MSG(batches == count, std::to_string(batches) + " batches reported");
    }
    CHECK_MSG(backlogIs(host, 0, 0), backlogText(host));
    r.announce();
    std::vector<std::string> expected = startMessages();
    expected.push_back("frozen:Widget stopped");
    const std::vector<std::string> late = sentRuns(r);
    CHECK_MSG(late == expected, firstDifference(late, expected));
    CHECK(r.channel->closes() == 1 && callsWith(*r.channel, "start:").empty());
}

/// Past the bound nothing the engine sent after the message that passed it has an effect, also when the client delivered it before the host ran the cut: with the host held behind that frame request, validated ops, listen, preventDefault, an anim, a frame request, `loaded`, an `Uncaught` and a plain log, a diagnostic, a refused op and a malformed listen (the validator's diagnostics), a raw `ready`, a commit of both areas, an anchor, a watch, stats and a tapped copy reach the host's queue (an Intl answer after them), and let go, none of it lands, every place is given back, later calls change nothing, and a late `ready` gets the host's messages, the badge, then the owner's theme and safe area without re-themed ops. The control: the same round within the bound lands in full.
TEST(remote_widget_host_lets_nothing_queued_after_the_cut_take_effect) {
    const size_t roundMessages = 7;
    for (const bool cut : {false, true}) {
        std::mutex m;
        std::vector<std::string> effects;
        const auto note = [&](const std::string& effect) {
            std::lock_guard<std::mutex> l(m);
            effects.push_back(effect);
        };
        ObservedChanges observed;
        RemoteHostRun r([&](WidgetHost& host) {
            host.onChange = observed.observer();
            host.onRenderOps = [&](const std::string& ops) { note("ops:" + ops); };
            host.onError = [&](const std::string& text) { note("error:" + text); };
            host.onAction = [&](const WidgetAction& action) { note("action:" + action.toJson()["kind"].asString()); };
            host.onStorageCommit = [&](const std::string& area, const std::map<std::string, std::string>&) { note("storage:" + area); };
            host.onWatchPublicData = [&](bool on) { note(on ? "watch:true" : "watch:false"); };
            host.onCopyText = [&](const std::string& text) { note("copy:" + text); };
        }, false);
        WidgetHost& host = *r.host;
        SandboxRemoteSink& sink = r.sink();
        sink.remoteOps(kParagraph);
        sendFrames(r, WidgetHost::kRendererBacklogMessages - 1 - (cut ? 0 : roundMessages));
        const int nodes = host.diagnostics().nodes;
        tapThrough(r);
        {
            HeldHost held(host);
            CHECK(held.held());
            if (cut) sink.remoteRequestsFrame();
            sink.remoteOps("[{\"op\":\"text\",\"id\":3,\"data\":\"after\"}]");
            sink.remoteListen("[\"click\"]");
            sink.remotePreventDefault("[\"2:click\"]");
            sink.remoteAnim(kParagraphAnim);
            sink.remoteRequestsFrame();
            sink.remoteLoaded();
            sink.remoteLog("error", "Uncaught after");
            sink.remoteLog("log", "plain after");
            sink.remoteDiag("after", "direct");
            sink.remoteOps("[{}]");
            sink.remoteListen("{");
            sink.remoteBridge("ready", "[]");
            sink.remoteStorage("{\"local\":{\"a\":\"b\"},\"session\":{\"c\":\"d\"}}");
            sink.remoteResizeAnchor("bottom");
            sink.remoteWatchesPublicData(true);
            SandboxStats stats;
            stats.tasks = 7;
            sink.remoteStats(stats);
            sink.remoteCopyText("copied");
            CHECK(sink.remoteIntl(kNumberRequest) == "1,234.5");
            CHECK_MSG(host.remoteBacklog().calls > 0 && !host.diagnostics().dead, backlogText(host));
            CHECK(held.release());
        }
        if (cut) CHECK(host.waitUntil(isDead, 5));
        settleHost(host);
        CHECK_MSG(backlogIs(host, 0, 0), backlogText(host));
        const WidgetDiagnostics d = host.diagnostics();
        std::string kinds;
        for (const auto& [kind, entry] : d.diags) kinds += kind + "=" + std::to_string(entry.first) + " ";
        if (!cut) {
            CHECK_MSG(!d.dead && d.diags.size() == 3 && d.countDiag("after") == 1 && d.countDiag("protocol-violation") == 1 && d.countDiag("validator-error") == 1, kinds);
            CHECK(d.logs.size() == 2 && d.actions.size() == 1 && d.bridge.size() == 1 && d.storageCommits == 2 && d.resizeAnchor == "bottom" && d.watchesPublicData);
            CHECK(d.stats.tasks == 7 && d.loaded && d.nodes == nodes + 1);
            std::lock_guard<std::mutex> l(m);
            const std::vector<std::string> landed = {"ops:" + kParagraphRendered, "ops:[{\"op\":\"text\",\"id\":3,\"data\":\"after\"}]", "error:Uncaught after", "action:ready",
                                                     "storage:local", "storage:session", "watch:true", "copy:copied"};
            CHECK_MSG(effects == landed, joined(effects));
            continue;
        }
        CHECK_MSG(d.dead == std::optional<std::string>(kBacklogLimitReason) && d.diags.size() == 1 && d.countDiag("backlog-limit") == 1, kinds + d.dead.value_or("alive"));
        CHECK(d.logs.empty() && !d.lastError && d.actions.empty() && d.bridge.empty() && d.storageCommits == 0 && d.resizeAnchor == "top" && !d.watchesPublicData);
        CHECK(d.stats.tasks == 0 && !d.loaded && d.nodes == nodes);
        const std::vector<WidgetDiagnostics> seen = observed.all();
        CHECK_MSG(seen.size() == 2 && !seen[0].dead && seen[0].countDiag("backlog-limit") == 1 && seen[1].dead == d.dead, std::to_string(seen.size()) + " changes seen");
        CHECK_MSG(stoppedForTheBacklog(*r.channel) && r.channel->closes() == 1, joined(callsWith(*r.channel, "stop:")));
        CHECK(r.sentTypes().empty());
        sink.remoteOps("[{\"op\":\"text\",\"id\":4,\"data\":\"late\"}]");
        sink.remoteLog("error", "Uncaught late");
        sink.remoteDiag("late", "x");
        sink.remoteBridge("ready", "[]");
        sink.remoteRequestsFrame();
        sink.remoteLoaded();
        sink.remoteDied("again");
        settleHost(host);
        const WidgetDiagnostics after = host.diagnostics();
        CHECK(after.diags == d.diags && after.logs.empty() && after.actions.empty() && after.dead == d.dead && !after.loaded && observed.all().size() == 2 && r.channel->closes() == 1);
        WidgetTheme dark;
        dark.colorScheme = "dark";
        const WidgetContentSafeArea area{1, 2, 3, 4, 5, 6};
        host.setTheme(dark);
        host.setContentSafeArea(area);
        r.announce();
        std::vector<std::string> expected = startMessages();
        expected.insert(expected.end(), {"frozen:Widget stopped", "theme:dark{}", "safeArea:" + area.json().dump()});
        const std::vector<std::string> late = sentRuns(r);
        CHECK_MSG(late == expected, firstDifference(late, expected));
        {
            std::lock_guard<std::mutex> l(r.renderer->mutex);
            CHECK(r.renderer->themes == std::vector<std::string>{"dark"});
        }
        CHECK(callsWith(*r.channel, "start:").empty());
        std::lock_guard<std::mutex> l(m);
        CHECK_MSG(effects == std::vector<std::string>{"ops:" + kParagraphRendered}, joined(effects));
    }
}

/// A message's request and wallet flags (a group's) under strict.
static WidgetHostOptions privilegedOptions() {
    WidgetHostOptions options;
    options.context = messageContext(false, true, false, "group");
    options.context.wallet = true;
    return options;
}

/// A quiet renderer's host with `privilegedOptions` and its engine's messages at the backlog's message bound; what `onRequest` and `onWallet` get, their responders kept.
struct PrivilegedHostRun {
    using Respond = std::function<void(const WidgetRequestOutcome&)>;
    std::mutex mutex;
    std::vector<std::string> got;
    std::vector<Respond> responders;
    RemoteHostRun r;
    PrivilegedHostRun()
        : r([this](WidgetHost& host) {
              host.onRequest = [this](const WidgetRequest& request, Respond respond) { keep("request:" + request.data, std::move(respond)); };
              host.onWallet = [this](const WalletCall& call, Respond respond) { keep("wallet:" + call.data, std::move(respond)); };
          }, false, privilegedOptions()) {
        sendFrames(r, WidgetHost::kRendererBacklogMessages);
    }
    void keep(const std::string& call, Respond respond) {
        std::lock_guard<std::mutex> l(mutex);
        got.push_back(call);
        responders.push_back(std::move(respond));
    }
    std::vector<std::string> calls() {
        std::lock_guard<std::mutex> l(mutex);
        return got;
    }
    /// After a tap, with the host held: the frame request past the bound when `cut`, then `call`, which the client hands over unrefused (an Intl answer after it, nothing more answered on the wire); then let go and settled.
    void handOver(bool cut, const std::function<void(SandboxRemoteSink&)>& call) {
        tapThrough(r);
        const size_t answered = r.channel->answers().size();
        HeldHost held(*r.host);
        CHECK(held.held());
        if (cut) r.sink().remoteRequestsFrame();
        call(r.sink());
        CHECK(r.sink().remoteIntl(kNumberRequest) == "1,234.5");
        CHECK_MSG(r.channel->answers().size() == answered, answersText(r.channel->answers()));
        CHECK(held.release());
        if (cut) CHECK(r.host->waitUntil(isDead, 5));
        settleHost(*r.host);
    }
    /// Completes the `i`th call from the test's thread; the client has sent its answer on when this returns.
    void respond(size_t i, const WidgetRequestOutcome& outcome) {
        Respond responder;
        {
            std::lock_guard<std::mutex> l(mutex);
            if (i < responders.size()) responder = responders[i];
        }
        CHECK(responder != nullptr);
        if (responder) responder(outcome);
        settleHost(*r.host);
    }
};

/// The privileged calls the cut stops, with the real flags and taps: within the bound a tapped request, and once it is answered a tapped wallet call, each handed over while the host is held, reach `onRequest` and `onWallet`; past it the same request, or the same wallet call on a host of its own (a fresh grant, nothing in flight), handed over while the host is held behind the frame request that passes the bound, reaches neither, and nothing answers it.
TEST(remote_widget_host_hands_no_privileged_call_over_after_the_cut) {
    const WalletCall wallet = signMeCall();
    const std::string payload = SandboxRemoteCoding::encodeWallet(wallet);
    {
        PrivilegedHostRun p;
        p.handOver(false, [](SandboxRemoteSink& sink) { sink.remoteRequest(1, "ask", false); });
        CHECK_MSG(p.calls() == std::vector<std::string>{"request:ask"}, joined(p.calls()));
        p.respond(0, answered(std::string("thanks")));
        p.handOver(false, [&](SandboxRemoteSink& sink) { sink.remoteWallet(2, payload); });
        CHECK_MSG((p.calls() == std::vector<std::string>{"request:ask", "wallet:" + wallet.data}), joined(p.calls()));
        p.respond(1, answered(std::string("signed")));
        const RemoteAnswers expected = {{1, SandboxRemoteOutcome::answer("thanks")}, {2, SandboxRemoteOutcome::answer("signed")}};
        CHECK_MSG(p.r.channel->answers() == expected, answersText(p.r.channel->answers()));
        const WidgetDiagnostics d = p.r.host->diagnostics();
        CHECK_MSG(!d.dead && d.diags.empty(), d.dead.value_or("alive"));
    }
    for (const bool walletCall : {false, true}) {
        PrivilegedHostRun p;
        p.handOver(true, [&](SandboxRemoteSink& sink) {
            if (walletCall) sink.remoteWallet(1, payload);
            else sink.remoteRequest(1, "ask", false);
        });
        const WidgetDiagnostics d = p.r.host->diagnostics();
        CHECK_MSG(p.calls().empty() && d.dead == std::optional<std::string>(kBacklogLimitReason), joined(p.calls()) + d.dead.value_or("alive"));
        CHECK_MSG(p.r.channel->answers().empty() && stoppedForTheBacklog(*p.r.channel), answersText(p.r.channel->answers()));
    }
}

/// A layout query that passes the message bound of a quiet renderer's host is answered dead at once, never after its 3 s timeout: the cut resolves it, the stop it posts runs behind it, the renderer gets no query, the owner's theme (its re-themed ops queued before the cut) and safe area stay for `ready`, and every place in the client is given back.
TEST(remote_widget_host_answers_a_query_past_the_bound_dead_at_once) {
    RemoteHostRun r(nullptr, false);
    WidgetHost& host = *r.host;
    sendFrames(r, WidgetHost::kRendererBacklogMessages);
    WidgetTheme dark;
    dark.colorScheme = "dark";
    const WidgetContentSafeArea area{1, 2, 3, 4, 5, 6};
    host.setTheme(dark);
    host.setContentSafeArea(area);
    host.onHostThread([] {});
    const std::string answer = returnsWithin<std::string>(5, [&] { return r.sink().remoteQuery("{\"q\":\"viewport\"}").json; });
    CHECK_MSG(answer == "{\"error\":\"dead\"}", answer);
    CHECK(host.waitUntil(isDead, 5));
    settleHost(host);
    const WidgetDiagnostics d = host.diagnostics();
    CHECK_MSG(d.dead == std::optional<std::string>(kBacklogLimitReason) && d.diags.size() == 1 && d.countDiag("backlog-limit") == 1, d.dead.value_or("alive"));
    CHECK(eventually([&] { return backlogIs(host, 0, 0); }));
    CHECK_MSG(stoppedForTheBacklog(*r.channel) && r.channel->closes() == 1, joined(callsWith(*r.channel, "stop:")));
    r.announce();
    std::vector<std::string> expected = startMessages();
    expected.insert(expected.end(), {"theme:dark{}", "ops:[]", "safeArea:" + area.json().dump(), "frozen:Widget stopped"});
    const std::vector<std::string> late = sentRuns(r);
    CHECK_MSG(late == expected, firstDifference(late, expected));
}

/// A death that wins on the client's thread while the host is held behind the message that passes the bound, a `terminated` from the engine's side or a lost connection, is the cut's once the host runs: the backlog's reason, the badge at a late `ready`, one close and no start; the stop the cut asks for finds the engine dead and adds nothing.
TEST(remote_widget_host_keeps_the_cuts_death_over_one_that_won_the_race) {
    for (const bool disconnect : {false, true}) {
        RemoteHostRun r(nullptr, false);
        WidgetHost& host = *r.host;
        SandboxRemoteSink& sink = r.sink();
        sendFrames(r, WidgetHost::kRendererBacklogMessages);
        HeldHost held(host);
        CHECK(held.held());
        sink.remoteRequestsFrame();
        if (disconnect) sink.remoteDisconnected();
        else sink.remoteDied("terminated");
        CHECK(host.probe("0") == "ERROR no runtime");
        CHECK(sink.remoteIntl(kNumberRequest).empty());
        CHECK(r.channel->closes() == 1 && callsWith(*r.channel, "stop:").empty() && !host.diagnostics().dead);
        CHECK(held.release());
        CHECK(host.waitUntil(isDead, 5));
        settleHost(host);
        const WidgetDiagnostics d = host.diagnostics();
        CHECK_MSG(d.dead == std::optional<std::string>(kBacklogLimitReason) && d.countDiag("backlog-limit") == 1, d.dead.value_or("alive"));
        r.announce();
        std::vector<std::string> expected = startMessages();
        expected.push_back("frozen:Widget stopped");
        const std::vector<std::string> late = sentRuns(r);
        CHECK_MSG(late == expected, firstDifference(late, expected));
        CHECK_MSG(r.channel->closes() == 1 && callsWith(*r.channel, "stop:").empty() && callsWith(*r.channel, "start:").empty(), joined(r.channel->calls()));
    }
}

/// An observer of the cut's diagnostic that announces the renderer at once, inside the cut: the renderer gets only the host's messages (the engine's were dropped already), the engine is never started, and the death the cut asked for still brings the badge once.
TEST(remote_widget_host_starts_no_engine_at_a_ready_inside_the_cut) {
    std::atomic<bool> announced{false};
    RemoteHostRun r([&](WidgetHost& host) {
        host.onChange = [&announced, &host](const WidgetDiagnostics& d) {
            if (d.countDiag("backlog-limit") && !announced.exchange(true)) host.renderer().onMessage("{\"type\":\"ready\"}");
        };
    }, false);
    WidgetHost& host = *r.host;
    sendFrames(r, WidgetHost::kRendererBacklogMessages + 1);
    CHECK(host.waitUntil(isDead, 5));
    settleHost(host);
    const WidgetDiagnostics d = host.diagnostics();
    CHECK_MSG(announced && d.rendererReadyMs && d.dead == std::optional<std::string>(kBacklogLimitReason) && d.countDiag("backlog-limit") == 1, d.dead.value_or("alive"));
    std::vector<std::string> expected = startMessages();
    expected.push_back("frozen:Widget stopped");
    const std::vector<std::string> sent = sentRuns(r);
    CHECK_MSG(sent == expected, firstDifference(sent, expected));
    CHECK_MSG(stoppedForTheBacklog(*r.channel) && r.channel->closes() == 1, joined(r.channel->calls()));
}

/// A flood of distinct diagnostic kinds from the engine's side of a working host: `diagnostics().diags` never holds more than `kMaxDiagKinds`; the kinds that came first keep counting with their first 50 details, an ordinary one sent 65 times and an early one sent again among them; every later kind, the renderer's own `renderer-violation` too, counts under "diag-kind-dropped" with its first 50 as `<kind>: <detail>`. The same when the engine's side sent "diag-kind-dropped" first: that is then the overflow's one place, and no second one is kept free.
TEST(remote_widget_host_caps_the_kinds_of_its_diagnostics) {
    using Entry = std::pair<int, std::vector<std::string>>;
    const size_t cap = WidgetHost::kMaxDiagKinds, details = 50, flood = 3 * cap, ordinary = 60;
    const auto kind = [](size_t i) { return "k" + std::to_string(i); };
    const auto detail = [](size_t i) { return "d" + std::to_string(i); };
    for (const bool overflowFirst : {false, true}) {
        std::mutex m;
        size_t most = 0;
        RemoteHostRun r([&](WidgetHost& host) {
            host.onChange = [&](const WidgetDiagnostics& d) {
                std::lock_guard<std::mutex> l(m);
                most = std::max(most, d.diags.size());
            };
        });
        WidgetHost& host = *r.host;
        SandboxRemoteSink& sink = r.sink();
        CHECK(host.diagnostics().diags.empty());
        if (overflowFirst) sink.remoteDiag("diag-kind-dropped", "from the engine");
        for (size_t i = 0; i < ordinary; i++) sink.remoteDiag("ordinary", "o" + std::to_string(i));
        const size_t retained = cap - 2, early = retained + 8;
        for (size_t i = 0; i < early; i++) sink.remoteDiag(kind(i), detail(i));
        settleHost(host);
        r.renderer->onMessage("{\"type\":\"violation\",\"detail\":\"from the renderer\"}");
        host.onHostThread([] {});
        for (size_t i = early; i < flood; i++) sink.remoteDiag(kind(i), detail(i));
        for (size_t i = ordinary; i < ordinary + 5; i++) sink.remoteDiag("ordinary", "o" + std::to_string(i));
        sink.remoteDiag(kind(0), "again");
        settleHost(host);
        const WidgetDiagnostics d = host.diagnostics();
        const auto entry = [&](const std::string& k) {
            auto it = d.diags.find(k);
            return it == d.diags.end() ? Entry{} : it->second;
        };
        std::vector<std::string> firsts, dropped;
        for (size_t i = 0; i < details; i++) firsts.push_back("o" + std::to_string(i));
        if (overflowFirst) dropped.push_back("from the engine");
        for (size_t i = retained; i < early; i++) dropped.push_back(kind(i) + ": " + detail(i));
        dropped.push_back("renderer-violation: from the renderer");
        for (size_t i = early; dropped.size() < details; i++) dropped.push_back(kind(i) + ": " + detail(i));
        CHECK_MSG(d.diags.size() == cap, std::to_string(d.diags.size()) + " kinds");
        CHECK(entry("ordinary") == Entry(int(ordinary + 5), firsts));
        CHECK(entry(kind(0)) == (Entry{2, std::vector<std::string>{detail(0), "again"}}));
        CHECK(entry(kind(retained - 1)) == (Entry{1, std::vector<std::string>{detail(retained - 1)}}) && d.diags.count(kind(retained)) == 0 && d.diags.count("renderer-violation") == 0);
        const Entry overflow = entry("diag-kind-dropped");
        CHECK_MSG(overflow.first == int(flood - retained + 1 + (overflowFirst ? 1 : 0)) && overflow.second == dropped, std::to_string(overflow.first) + ": " + firstDifference(overflow.second, dropped));
        std::lock_guard<std::mutex> l(m);
        CHECK_MSG(most == cap, std::to_string(most) + " kinds at most");
    }
}

/// With the kinds full, and the overflow's details too, the message that passes the backlog's bound still stops a quiet renderer's host for the backlog: its diagnostic is one more under "diag-kind-dropped", the kinds stay at the cap, and `dead` names the backlog.
TEST(remote_widget_host_dies_of_its_backlog_with_its_diagnostic_kinds_full) {
    const size_t cap = WidgetHost::kMaxDiagKinds, flood = 3 * cap;
    std::mutex m;
    size_t most = 0;
    RemoteHostRun r([&](WidgetHost& host) {
        host.onChange = [&](const WidgetDiagnostics& d) {
            std::lock_guard<std::mutex> l(m);
            most = std::max(most, d.diags.size());
        };
    }, false);
    WidgetHost& host = *r.host;
    for (size_t i = 0; i < flood; i++) r.sink().remoteDiag("k" + std::to_string(i), "d" + std::to_string(i));
    settleHost(host);
    const WidgetDiagnostics full = host.diagnostics();
    const auto before = full.diags.find("diag-kind-dropped");
    const bool saturated = before != full.diags.end() && before->second.first == int(flood - (cap - 1)) && before->second.second.size() == 50;
    CHECK_MSG(full.diags.size() == cap && saturated, std::to_string(full.diags.size()) + " kinds");
    sendFrames(r, WidgetHost::kRendererBacklogMessages + 1);
    CHECK(host.waitUntil(isDead, 5));
    settleHost(host);
    const WidgetDiagnostics d = host.diagnostics();
    const auto after = d.diags.find("diag-kind-dropped");
    CHECK_MSG(d.dead == std::optional<std::string>(kBacklogLimitReason) && d.diags.size() == cap && d.countDiag("backlog-limit") == 0, d.dead.value_or("alive"));
    CHECK(saturated && after != d.diags.end() && after->second.first == before->second.first + 1 && after->second.second == before->second.second);
    CHECK_MSG(stoppedForTheBacklog(*r.channel) && r.channel->closes() == 1, joined(callsWith(*r.channel, "stop:")));
    std::lock_guard<std::mutex> l(m);
    CHECK_MSG(most == cap, std::to_string(most) + " kinds at most");
}

// ───────────── the tests' own workers (Background) ─────────────

/// A fixture whose end shows in a child's output.
struct NotedFixture {
    int value = 1;
    ~NotedFixture() {
        fprintf(stderr, "fixture destroyed\n");
        value = 0;
    }
};

/// A worker held `holdMs`, then reading a fixture of the case's, finished with `deadline`: past it the process must end inside `finish`, the fixture and the line after it untouched.
static void finishAWorkerHeld(int holdMs, double deadline) {
    NotedFixture fixture;
    Background<int> worker([&fixture, holdMs] {
        sleepMs(holdMs);
        return fixture.value;
    });
    if (!worker.within(0)) fprintf(stderr, "still running\n");
    CHECK(worker.finish(deadline) == 1);
    fprintf(stderr, "after the deadline\n");
}

CHILD_CASE(worker_held_past_its_deadline) { finishAWorkerHeld(8000, 0.25); }
CHILD_CASE(worker_back_within_its_deadline) { finishAWorkerHeld(50, 5); }

/// The exit status and the output (stdout and stderr) of `twidget-tests --child <name>`; each child case ends on its own, so this wait does too.
static std::pair<int, std::string> runChild(const std::string& name) {
    const std::filesystem::path log = std::filesystem::temp_directory_path() / ("twidget-tests-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
    std::string command = "\"" + g_self + "\" --child " + name + " > \"" + log.string() + "\" 2>&1";
#ifdef _WIN32
    // cmd /c drops the outer pair of quotes.
    command = "\"" + command + "\"";
    const int status = std::system(command.c_str());
#else
    const int result = std::system(command.c_str());
    const int status = WIFEXITED(result) ? WEXITSTATUS(result) : -1;
#endif
    std::string output = readFile(log.string());
    std::error_code ignored;
    std::filesystem::remove(log, ignored);
    return {status, output};
}

/// A worker past its deadline ends the run at once, unwinding nothing it may use: run alone in a child, a worker held 8 s and finished with 0.25 s ends the process with the run's stop status after a look that gave up on nothing, before its fixture's destructor and the line after `finish`, and long before it would wake; back in time, the same case ends normally with both.
TEST(remote_a_worker_past_its_deadline_stops_the_run_with_its_fixtures_in_place) {
    const auto started = std::chrono::steady_clock::now();
    const auto [status, output] = runChild("worker_held_past_its_deadline");
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK_MSG(status == kWorkerOverranExit, std::to_string(status) + ": " + output);
    CHECK_MSG(output.find("still running\n") != std::string::npos && output.find("a worker still runs after 0.25 s; the run stops here") != std::string::npos, output);
    CHECK_MSG(output.find("fixture destroyed") == std::string::npos && output.find("after the deadline") == std::string::npos, output);
    CHECK_MSG(seconds < 8, std::to_string(seconds) + " s");
    const auto [backStatus, backOutput] = runChild("worker_back_within_its_deadline");
    const size_t after = backOutput.find("after the deadline\n"), destroyed = backOutput.find("fixture destroyed\n");
    CHECK_MSG(backStatus == 0 && after != std::string::npos && destroyed != std::string::npos && after < destroyed && backOutput.find("FAIL") == std::string::npos,
              std::to_string(backStatus) + ": " + backOutput);
}
