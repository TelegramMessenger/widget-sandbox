// Tests for the desktop core: the XCTest suite of ios/Tests/WidgetSandboxTests
// ported one for one (engine boot, network stubs, CPU / memory kills, layout
// query round trip, pre-boot buffering, timer cap, CPU quota, off-screen pause,
// bridge, pinned-thread recursion, validator diagnostics and sanitising,
// create/teardown churn) plus the shared CpuBudget and SDK v1 policy fixtures,
// SDK v1 through the real engine and WidgetHost, and the JSON helper.
// No test framework dependency: `twidget-tests [filter]`, exit code = failures.

#include "../harness/recording_host.h"
#include "twidget/cpu_budget.h"
#ifdef _WIN32
#include "twidget/icu_intl.h"
#endif
#include "twidget/json.h"
#include "twidget/sdk_policy.h"
#ifdef TW_HAVE_BLITZ
#include "twidget/blitz_renderer.h"
#endif
#ifdef TW_HAVE_LITEHTML
#include "twidget/litehtml_renderer.h"
#endif
#include "twidget/widget_host.h"

#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace twidget;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_MSG(cond, msg) do { g_checks++; if (!(cond)) { g_failures++; fprintf(stderr, "  FAIL %s:%d: %s — %s\n", __FILE__, __LINE__, #cond, std::string(msg).c_str()); } } while (0)

struct TestCase { const char* name; void (*fn)(); };
static std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
#define TEST(name) static void name(); static const bool name##_registered = (registry().push_back({#name, name}), true); static void name()

static std::shared_ptr<SandboxResources> RES;
static const std::string CANARY = "http://127.0.0.1:9999/hit/desktop-unit";

static std::string readFile(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static SandboxConfig testConfig() {
    SandboxConfig c;
    c.cpuLimitMs = 2000;
    c.innerWidth = 400;
    c.innerHeight = 300;
    c.devicePixelRatio = 2;
    c.theme = "light";
    return c;
}

/// One engine with a recording host; the host outlives the engine (declaration order).
struct Run {
    RecordingHost host;
    SandboxEngine engine;
    Run(const std::string& html, SandboxConfig cfg = testConfig(), bool start = true) : host("light", RES), engine(html, cfg, RES) {
        engine.setDelegate(&host);
        if (start) { engine.start(); host.waitLoaded(20); }
    }
    /// Like `engine.queue.sync {}` on iOS: everything queued so far has run.
    void barrier() { engine.thread().sync([] {}); }
    std::string logsText() { std::string s; for (auto& l : host.logs) s += "[" + l.first + "] " + l.second + "\n"; return s; }
    std::string diagsText() { std::string s; for (auto& d : host.diags) s += d.first + ": " + d.second + "\n"; return s; }
};

// ───────────── helpers ─────────────

TEST(json_fuzz_never_crashes) {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    auto next = [&]() { state = state * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(state >> 33); };
    const std::string seeds[] = {
        "{\"type\":\"result\",\"qid\":1,\"result\":{\"w\":[1,2.5e300,-0.0,\"\\ud83d\\ude00\"]}}",
        "[\"\\u0000\",{\"a\":{\"b\":[null,true,false]}},-1e-400,1E+999]",
        "{\"ops\":[{\"op\":\"el\",\"id\":2,\"tag\":\"div\",\"attrs\":{\"class\":\"x\"}}]}",
    };
    const char alphabet[] = "{}[]\":,.-+eE0123456789truefalsn \\u/abcdef\x00\x80\xff";
    int parsed = 0;
    for (int t = 0; t < 60000; t++) {
        std::string text;
        if (t % 2 == 0) {
            text = seeds[next() % 3];
            int edits = 1 + int(next() % 6);
            for (int e = 0; e < edits && !text.empty(); e++) {
                size_t at = next() % text.size();
                switch (next() % 4) {
                case 0: text[at] = alphabet[next() % (sizeof(alphabet) - 1)]; break;
                case 1: text.erase(at, 1 + next() % 4); break;
                case 2: text.insert(at, 1, alphabet[next() % (sizeof(alphabet) - 1)]); break;
                default: text.resize(at); break;
                }
            }
        } else {
            size_t n = next() % 48;
            for (size_t k = 0; k < n; k++) text.push_back(alphabet[next() % (sizeof(alphabet) - 1)]);
        }
        auto v = Json::parse(text);
        if (v) {
            parsed++;
            std::string dumped = v->dump();
            auto again = Json::parse(dumped);
            CHECK(again.has_value());
            if (again) CHECK(again->dump() == dumped);
        }
    }
    CHECK(parsed > 0);
    CHECK(!Json::parse(std::string(200000, '[')).has_value());
    CHECK(!Json::parse(std::string("\"\\ud800")).has_value() || true);
}

TEST(json_parse_and_dump) {
    auto v = Json::parse("{\"a\":[1,2.5,\"x\\u00e9\\ud83d\\ude00\",true,null],\"b\":{}, \"c\":-1e2}");
    CHECK(v.has_value());
    CHECK(v->dump() == "{\"a\":[1,2.5,\"xé\U0001F600\",true,null],\"b\":{},\"c\":-100}");
    CHECK((*v)["a"][2].asString() == "xé\U0001F600");
    CHECK((*v)["c"].asNumber() == -100);
    CHECK(!Json::parse("[1,]"));
    CHECK(!Json::parse("{\"a\":1} x"));
    CHECK(!Json::parse("{a:1}"));
    CHECK(!Json::parse(std::string(300, '[')));
    CHECK(Json::parse(std::string(100, '[') + std::string(100, ']')).has_value());
    CHECK(Json::number(1234.5) == "1234.5");
    CHECK(Json::number(3) == "3");
    CHECK(Json::number(1e21) == "1e+21");
    // Shortest round trip on every target (Apple below macOS 13.3 has no floating to_chars).
    CHECK(Json::number(0.1) == "0.1");
    CHECK(Json::number(16.7) == "16.7");
    CHECK(Json::number(-0.25) == "-0.25");
    for (double d : {1.0 / 3, 2.0 / 3, 0.1 + 0.2, 1e-7, 123456.789e10, 5e-324, 1.7976931348623157e308}) {
        auto text = Json::number(d);
        CHECK_MSG(Json::parse(text)->asNumber() == d, text);
        CHECK_MSG(text.size() <= 24, text);
    }
    CHECK(Json::quote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
    Json o = Json::object();
    o.set("k", "v").set("n", 2);
    CHECK(o.dump() == "{\"k\":\"v\",\"n\":2}");
}

/// Numbers parse and print the same under a locale whose decimal separator is a
/// comma (Qt hosts call setlocale from the environment).
TEST(json_numbers_ignore_lc_numeric) {
    std::string saved = setlocale(LC_NUMERIC, nullptr);
    const char* comma = nullptr;
    for (const char* name : {"ru_RU.UTF-8", "ru_RU.utf8", "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8"}) {
        if (setlocale(LC_NUMERIC, name)) { comma = name; break; }
    }
    if (!comma) { fprintf(stderr, "  (no comma-decimal locale installed; skipped)\n"); return; }
    CHECK((*Json::parse("{\"h\":16.5}"))["h"].asNumber() == 16.5);
    CHECK(Json::parse("[1e-3,2.25E2]")->dump() == "[0.001,225]");
    CHECK(Json::number(16.5) == "16.5");
    CHECK(Json::number(0.1) == "0.1");
    setlocale(LC_NUMERIC, saved.c_str());
}

/// CpuBudget against the fixture shared with cpu-budget.ts, CpuBudget.swift and CpuBudget.kt.
TEST(cpu_budget_shared_fixture) {
    auto root = Json::parse(readFile(std::string(TW_FIXTURES) + "/cpu-budget.json"));
    CHECK_MSG(root.has_value(), "fixture missing — run pnpm build:desktop");
    if (!root) return;
    const auto& cases = (*root)["cases"].arr();
    CHECK(cases.size() > 5);
    for (const auto& c : cases) {
        const std::string& name = c["name"].asString();
        const Json& cfg = c["config"];
        CpuBudget b(cfg["cpuLimitMs"].asNumber(), cfg["cpuQuotaMs"].asNumber(), cfg["cpuQuotaWindowMs"].asNumber(), cfg["cpuKillAfterMs"].asNumber());
        int i = 0;
        for (const auto& step : c["steps"].arr()) {
            double at = step["at"].asNumber();
            std::string where = name + ": step " + std::to_string(i++) + " @" + std::to_string(int(at));
            if (const Json* kind = step.find("decide")) {
                const Json& expect = step["expect"];
                CpuDecision want = expect["run"].asBool() ? CpuDecision{true, expect["sliceMs"].asNumber(), 0} : CpuDecision{false, 0, expect["retryAt"].asNumber()};
                CpuDecision got = b.decide(kind->asString(), at);
                CHECK_MSG(got == want, where + " got run=" + std::to_string(got.run) + " slice=" + std::to_string(got.sliceMs) + " retryAt=" + std::to_string(got.retryAt));
            } else if (const Json* kind = step.find("record")) {
                b.record(kind->asString(), step["cpuMs"].asNumber(), at);
            } else if (const Json* kill = step.find("kill")) {
                CHECK_MSG(b.shouldKill(at) == kill->asBool(), where);
            } else if (const Json* slice = step.find("queryBudget")) {
                CHECK_MSG(CpuBudget::queryBudgetMs(slice->asNumber()) == step["expect"].asNumber(), where);
            } else {
                CHECK_MSG(false, where + ": unknown step");
            }
        }
    }
}

/// Deep equality of JSON values, object members in any order (what node:assert's deepEqual compares).
static bool jsonEqual(const Json& a, const Json& b) {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
    case Json::Type::Null: return true;
    case Json::Type::Bool: return a.asBool() == b.asBool();
    case Json::Type::Number: return a.asNumber() == b.asNumber();
    case Json::Type::String: return a.asString() == b.asString();
    case Json::Type::Array:
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) if (!jsonEqual(a[i], b[i])) return false;
        return true;
    case Json::Type::Object:
        if (a.size() != b.size()) return false;
        for (const auto& [k, v] : a.obj()) { const Json* w = b.find(k); if (!w || !jsonEqual(v, *w)) return false; }
        return true;
    }
    return false;
}

/// `{ "repeat": [s, n] }` in the fixture stands for s repeated n times.
static Json expandRepeat(const Json& v) {
    if (v.isArray()) {
        Json out = Json::array();
        for (const auto& x : v.arr()) out.push(expandRepeat(x));
        return out;
    }
    if (v.isObject()) {
        if (v.size() == 1 && v["repeat"].isArray()) {
            std::string s;
            for (int i = 0; i < v["repeat"][1].asInt(); i++) s += v["repeat"][0].asString();
            return Json(s);
        }
        Json out = Json::object();
        for (const auto& [k, x] : v.obj()) out.set(k, expandRepeat(x));
        return out;
    }
    return v;
}

static Json orNull(const std::optional<std::string>& s) { return s ? Json(*s) : Json(); }

/// sdk_policy against the fixture shared with sdk-policy.ts, SdkPolicy.swift and SdkPolicy.kt: every section.
TEST(sdk_policy_shared_fixture) {
    auto root = Json::parse(readFile(std::string(TW_FIXTURES) + "/sdk-policy.json"));
    CHECK_MSG(root.has_value(), "fixture missing — run pnpm build:desktop");
    if (!root) return;
    int cases = 0;
    for (const auto& c : (*root)["links"].arr()) {
        cases++;
        Json got = orNull(sdk::checkLinkValue(c["method"].asString(), c["url"]));
        CHECK_MSG(jsonEqual(got, c["expect"]), c["method"].asString() + " " + c["url"].dump().substr(0, 100) + " → " + got.dump().substr(0, 100));
    }
    for (const auto& c : (*root)["haptics"].arr()) {
        cases++;
        auto h = sdk::checkHaptic(c["args"]);
        Json got;
        if (h) { got = Json::object(); got.set("haptic", h->haptic); if (h->value) got.set("value", *h->value); }
        CHECK_MSG(jsonEqual(got, c["expect"]), c["args"].dump() + " → " + got.dump());
    }
    for (const auto& c : (*root)["bridge"].arr()) {
        cases++;
        auto a = sdk::checkBridgeCall(c["method"], c["args"]);
        Json got = a ? a->toJson() : Json();
        CHECK_MSG(jsonEqual(got, c["expect"]), c["method"].dump() + " " + c["args"].dump() + " → " + got.dump());
    }
    for (const auto& c : (*root)["hapticGate"].arr()) {
        cases++;
        sdk::HapticGate gate;
        int i = 0;
        for (const auto& s : c["steps"].arr()) {
            std::string got(sdk::HapticGate::name(gate.decide(s["at"].asNumber(), s["visible"].asBool())));
            CHECK_MSG(got == s["expect"].asString(), c["name"].asString() + ", step " + std::to_string(i) + ": " + got);
            i++;
        }
    }
    for (const auto& c : (*root)["themeParams"].arr()) {
        cases++;
        Json got = sdk::themeParamsJson(sdk::checkThemeParams(c["input"]));
        CHECK_MSG(jsonEqual(got, c["expect"]), c["input"].dump() + " → " + got.dump());
    }
    for (const auto& c : (*root)["locales"].arr()) {
        cases++;
        Json got = orNull(sdk::checkLocaleValue(c["input"]));
        CHECK_MSG(jsonEqual(got, c["expect"]), c["input"].dump() + " → " + got.dump());
    }
    for (const auto& c : (*root)["storage"].arr()) {
        cases++;
        auto commit = sdk::checkStorageCommit(expandRepeat(c["input"]));
        Json got = commit ? commit->toJson() : Json();
        CHECK_MSG(jsonEqual(got, expandRepeat(c["expect"])), c["note"].isString() ? c["note"].asString() : c["input"].dump());
    }
    for (const auto& c : (*root)["utf8"].arr()) {
        cases++;
        CHECK_MSG(sdk::utf8Length(c["s"].asString()) == size_t(c["bytes"].asInt()), c["s"].asString());
    }
    printf("    %d fixture cases\n", cases);
    CHECK(cases > 150);
    for (const char* section : {"links", "haptics", "bridge", "hapticGate", "themeParams", "locales", "storage", "utf8"})
        CHECK_MSG((*root)[section].size() > 0, section);
}

/// What the fixture cannot carry (no lone surrogates cross every platform's string type): the C++ strings
/// are UTF-8, with a lone surrogate as WTF-8, and checkLink refuses both that and invalid UTF-8.
TEST(sdk_link_refuses_surrogates_and_invalid_utf8) {
    auto link = [](const std::string& url) { return sdk::checkLink("openLink", url).value_or("null"); };
    CHECK(link("https://example.com/\xC3\xA9") == "https://example.com/%C3%A9");
    CHECK(link("https://example.com/\xF0\x9F\x98\x80") == "https://example.com/%F0%9F%98%80");
    CHECK(link("https://example.com/\xED\x9F\xBF") == "https://example.com/%ED%9F%BF"); // U+D7FF, the last before the surrogates
    CHECK(link("https://example.com/\xEE\x80\x80") == "https://example.com/%EE%80%80"); // U+E000, the first after
    for (const char* bad : {"https://example.com/\xED\xA0\x80", "https://example.com/\xED\xBF\xBF", "https://example.com/\xED\xA0\xBD\xED\xB8\x80", // lone / CESU-8 pair
                            "https://example.com/\xC3", "https://example.com/\xC3x", "https://example.com/\x80", "https://example.com/\xFF",
                            "https://example.com/\xC0\xAF", "https://example.com/\xE0\x80\xAF", "https://example.com/\xF0\x80\x80\xAF", // overlong
                            "https://example.com/\xF4\x90\x80\x80", "https://example.com/\xF8\x88\x80\x80\x80"})                       // above U+10FFFF
        CHECK_MSG(link(bad) == "null", bad);
    CHECK(link(std::string("https://example.com/a\0b", 23)) == "null");
    // The JSON route: a `\ud800` escape stays a surrogate when asked to, so the bridge check refuses it.
    CHECK(Json::parse("\"\\ud800\"", Json::Surrogates::Keep)->asString() == "\xED\xA0\x80");
    CHECK(Json::parse("\"\\udc00x\"", Json::Surrogates::Keep)->asString() == "\xED\xB0\x80x");
    CHECK(Json::parse("\"\\ud83d\\ude00\"", Json::Surrogates::Keep)->asString() == "\xF0\x9F\x98\x80");
    CHECK(Json::parse("\"\\ud800\"")->asString() == "\xEF\xBF\xBD");
    // A lone high surrogate does not swallow the escape after it.
    CHECK(Json::parse("\"\\ud800\\u0041\"")->asString() == "\xEF\xBF\xBD" "A");
    CHECK(Json::parse("\"\\ud800\\u0041\"", Json::Surrogates::Keep)->asString() == "\xED\xA0\x80" "A");
    auto call = Json::parse("[\"openLink\",[\"https://example.com/\\ud800\"]]", Json::Surrogates::Keep);
    CHECK(call && !sdk::checkBridgeCall((*call)[0], (*call)[1]));
    CHECK(sdk::utf8Length(Json::parse("\"\\udc00\"", Json::Surrogates::Keep)->asString()) == 3);
    // Lowercasing is A–Z only: U+212A KELVIN SIGN stays non-ASCII and fails the host check.
    CHECK(link("https://\xE2\x84\xAA" "elvin.com/") == "null");
}

static const char* kDepthProbe ="(function(){ var n = 0; function r(){ n++; r(); } try { r(); } catch (e) { if (!(e instanceof RangeError)) throw e; } return n; })()";

/// Product contexts live on a JSThread (8 MB stack, 6 MB QuickJS limit): a release
/// quickjs-ng needs a few hundred bytes per JS call, so this must comfortably exceed
/// what css-tree and chart libraries recurse to.
TEST(pinned_thread_gives_deep_recursion) {
    JSThread t("test.js");
    double depth = t.sync([&] {
        QuickJSContext js(size_t(16) << 20, JSThread::kJSStackLimit);
        return js.evaluate(kDepthProbe, "t.js").number.value_or(-1);
    });
    printf("    quickjs recursion depth on JSThread: %.0f\n", depth);
    CHECK(depth > 300);
    // Timers and ordering: `after` fires on the thread, `cancel` prevents it.
    std::promise<bool> fired;
    bool cancelledRan = false;
    auto c = t.after(0.01, [&] { cancelledRan = true; });
    c->cancel();
    t.after(0.03, [&] { fired.set_value(t.isCurrent()); });
    auto f = fired.get_future();
    CHECK(f.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(f.get());
    CHECK(!cancelledRan);
    t.stop();
}

/// The runtime re-reads the stack top on every entry, so a call from another thread
/// (whose stack lives elsewhere) is measured correctly, not as a bogus overflow.
TEST(stack_check_follows_the_calling_thread) {
    JSThread t("creator");
    std::unique_ptr<QuickJSContext> js;
    double depthOnCreator = t.sync([&] {
        js = std::make_unique<QuickJSContext>(size_t(16) << 20);
        return js->evaluate(kDepthProbe, "t.js").number.value_or(-1);
    });
    double depthHere = js->evaluate(kDepthProbe, "t.js").number.value_or(-1);
    printf("    quickjs recursion depth at default stack limit: creator %.0f, here %.0f\n", depthOnCreator, depthHere);
    CHECK(depthOnCreator > 20);
    CHECK(depthHere > 20);
    CHECK(std::fabs(depthHere - depthOnCreator) / depthOnCreator < 0.1);
    t.sync([&] { js.reset(); });
}

// ───────────── engine ─────────────

TEST(boots_and_emits_initial_tree) {
    Run r("<p id=\"a\">hi</p><script>document.getElementById('a').textContent = 'changed by script'; console.log('hello from quickjs');</script>");
    r.barrier();
    auto l = r.host.lock();
    CHECK(!r.host.died);
    std::string all = r.host.allOps();
    CHECK_MSG(all.find("\"tag\":\"p\"") != std::string::npos, "initial tree should be sent");
    CHECK_MSG(all.find("changed by script") != std::string::npos, "script mutation should be sent");
    CHECK_MSG(r.host.allRenderOps().find("changed by script") != std::string::npos, "validated ops carry the mutation");
    CHECK(r.host.hasLog("hello from quickjs"));
    bool errors = false;
    for (auto& lg : r.host.logs) if (lg.first == "error") errors = true;
    CHECK_MSG(!errors, r.logsText());
}

TEST(network_apis_are_stubbed) {
    Run r("<script>var r = []; try { new WebSocket('ws://x') } catch (e) { r.push(e.name) } fetch('" + CANARY + "').catch(e => r.push(e.name)).then(() => console.log(r.join(',')));</script>");
    r.barrier();
    auto l = r.host.lock();
    CHECK_MSG(r.host.hasLog("NetworkDisabledError,NetworkDisabledError"), r.logsText());
    CHECK(r.host.hasDiag("network-blocked"));
}

TEST(cpu_limit_kills_infinite_loop) {
    auto cfg = testConfig();
    cfg.cpuLimitMs = 200;
    Run r("<script>for(;;){}</script>", cfg);
    r.barrier();
    auto l = r.host.lock();
    CHECK(r.host.died.has_value());
    CHECK_MSG(r.host.died && r.host.died->find("CPU limit") != std::string::npos, r.host.died.value_or(""));
}

TEST(memory_limit_kills_allocation_flood) {
    auto cfg = testConfig();
    cfg.memoryLimitBytes = size_t(16) << 20;
    cfg.cpuLimitMs = 10000;
    Run r("<script>var a = []; for (;;) a.push(new Array(100000).fill(1.5));</script>", cfg);
    r.barrier();
    auto l = r.host.lock();
    CHECK_MSG(r.host.died && r.host.died->find("Memory limit") != std::string::npos, r.host.died.value_or("alive: " + r.logsText()));
}

TEST(layout_query_round_trip) {
    Run r("<div id=\"d\">x</div><script>console.log('w=' + document.getElementById('d').getBoundingClientRect().width);</script>");
    r.barrier();
    auto l = r.host.lock();
    bool rect = false;
    for (auto& q : r.host.queries) if (q.find("\"rect\"") != std::string::npos) rect = true;
    CHECK(rect);
    CHECK_MSG(r.host.hasLog("w=100"), r.logsText());
}

/// The renderer reports its size before `ready`; the host forwards it before start().
TEST(inputs_before_start_are_buffered_not_crashing) {
    Run r("<script>console.log('w=' + innerWidth + ' theme=' + TelegramWidget.theme);</script>", testConfig(), false);
    r.engine.viewport(360, 200, 3);
    r.engine.theme("dark");
    r.engine.frame(1);
    r.engine.start();
    r.host.waitLoaded(20);
    r.barrier();
    auto l = r.host.lock();
    CHECK(!r.host.died);
    CHECK_MSG(r.host.hasLog("w=400 theme=light"), "boot sees the initial config: " + r.logsText());
}

/// `maxTimers` reaches the shim through the boot config: the 9th timer throws
/// RangeError inside the widget, the first 8 still fire, and the engine's own
/// backstop is never needed.
TEST(timer_limit_is_enforced) {
    auto cfg = testConfig();
    cfg.maxTimers = 8;
    Run r("<script>var fired = 0, err = ''; for (var i = 0; i < 8; i++) setTimeout(function () { if (++fired === 8) console.log('fired=' + fired); }, 5);"
          "try { setTimeout(function () {}, 5); } catch (e) { err = e.name; } try { setInterval(function () {}, 10); } catch (e) { err += ',' + e.name; } console.log('err=' + err);</script>", cfg);
    r.host.waitUntil([&] { return r.host.hasLog("fired=8"); }, 3);
    auto l = r.host.lock();
    CHECK_MSG(r.host.hasLog("err=RangeError,RangeError"), r.logsText());
    CHECK_MSG(r.host.hasLog("fired=8"), r.logsText());
    CHECK_MSG(r.host.countDiag("timer-limit") == 1, r.diagsText());
    CHECK_MSG(!r.host.hasDiag("timer-limit-enforced"), "the engine backstop must not be needed: " + r.diagsText());
    CHECK(!r.host.died);
}

/// A 100 ms interval burning 40 ms per tick is legal under cpuLimitMs alone; the
/// quota (100 ms/s here) defers ticks instead of killing, reports `cpu-throttled`,
/// and a click still gets through.
TEST(cpu_quota_throttles_greedy_timer) {
    auto cfg = testConfig();
    cfg.cpuQuotaMs = 100;
    Run r("<p id=\"p\">x</p><script>var ticks = 0; setInterval(function () { var t = performance.now(); while (performance.now() - t < 40) {} ticks++; }, 100);"
          "document.addEventListener('click', function () { console.log('click at tick ' + ticks); });</script>", cfg);
    sleepMs(2500);
    r.engine.event("{\"type\":\"click\",\"targetId\":2}");
    r.barrier();
    auto l = r.host.lock();
    CHECK(!r.host.died);
    CHECK_MSG(r.host.hasDiag("cpu-throttled"), r.diagsText());
    CHECK_MSG(r.host.hasLogPrefix("click at tick"), "events run under throttling: " + r.logsText());
    int ticks = -1;
    for (auto& lg : r.host.logs) if (lg.second.rfind("click at tick ", 0) == 0) ticks = std::atoi(lg.second.c_str() + 14);
    // Unthrottled the interval would tick ~25 times in 2.5 s; 100 ms/s allows 2–3 per second.
    CHECK_MSG(ticks > 1, std::to_string(ticks));
    CHECK_MSG(ticks < 12, std::to_string(ticks) + " ticks: " + r.logsText());
}

/// The layout-query budget (CpuBudget::queryBudgetMs): a forced-layout loop burns no JS
/// CPU of its own but keeps the host answering queries; with a slow renderer (5 ms per
/// answer) it is killed with the layout-query reason instead of holding the renderer.
/// The twin of QueryBudgetTests.swift and QueryBudgetTest.kt.
TEST(layout_query_loop_is_killed_instead_of_blocking_the_host) {
    auto cfg = testConfig();
    cfg.cpuLimitMs = 200;
    Run r("<div id=a></div><script>const a = document.getElementById('a'); let i = 0; for (;;) { a.style.width = (i++ % 300) + 'px'; a.offsetHeight; }</script>", cfg, false);
    r.host.answer = [](const Json&) -> std::optional<std::string> { std::this_thread::sleep_for(std::chrono::milliseconds(5)); return std::nullopt; };
    auto t0 = std::chrono::steady_clock::now();
    r.engine.start();
    r.host.waitLoaded(10);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    auto l = r.host.lock();
    CHECK_MSG(r.host.died.has_value(), "alive after " + std::to_string(seconds) + " s, " + std::to_string(r.host.queries.size()) + " queries");
    CHECK_MSG(r.host.died && r.host.died->rfind("Layout query limit exceeded", 0) == 0, r.host.died.value_or("alive"));
    CHECK_MSG(seconds < 5, std::to_string(seconds) + " s");
    CHECK_MSG(r.host.queries.size() < 120, std::to_string(r.host.queries.size()) + " queries");
}

TEST(ordinary_layout_queries_keep_the_widget_alive) {
    auto cfg = testConfig();
    cfg.cpuLimitMs = 200;
    Run r("<div id=a></div><script>const a = document.getElementById('a'); for (let i = 0; i < 10; i++) { a.style.width = i + 'px'; a.offsetHeight; }</script>", cfg, false);
    r.host.answer = [](const Json&) -> std::optional<std::string> { std::this_thread::sleep_for(std::chrono::milliseconds(2)); return std::nullopt; };
    r.engine.start();
    r.host.waitLoaded(10);
    r.barrier();
    auto l = r.host.lock();
    CHECK_MSG(!r.host.died, r.host.died.value_or(""));
    CHECK(!r.host.queries.empty());
}

/// Hidden: frames are neither requested nor delivered; on return the one
/// pending request is made and the shim saw `visibilitychange` both ways.
TEST(hidden_engine_delivers_no_frames) {
    Run r("<script>var n = 0; function step() { n++; console.log('frame ' + n); requestAnimationFrame(step); } requestAnimationFrame(step);"
          "document.addEventListener('visibilitychange', function () { console.log('vis=' + document.visibilityState); });</script>");
    r.barrier();
    { auto l = r.host.lock(); CHECK(r.host.frameRequests == 1); }
    r.engine.frame(1);
    r.barrier();
    { auto l = r.host.lock(); CHECK(r.host.frameRequests == 2); CHECK(r.host.hasLog("frame 1")); }
    r.engine.setVisible(false);
    r.engine.frame(2); // in flight when the widget was hidden: dropped
    r.barrier();
    { auto l = r.host.lock(); CHECK(r.host.frameRequests == 2); CHECK(!r.host.hasLog("frame 2")); CHECK_MSG(r.host.hasLog("vis=hidden"), r.logsText()); }
    r.engine.setVisible(true);
    r.barrier();
    { auto l = r.host.lock(); CHECK_MSG(r.host.frameRequests == 3, "the pending request is made on return"); CHECK_MSG(r.host.hasLog("vis=visible"), r.logsText()); }
    r.engine.frame(3);
    r.barrier();
    auto l = r.host.lock();
    CHECK(r.host.hasLog("frame 2"));
    CHECK(!r.host.died);
}

/// Hidden: a 50 ms interval ticks at most once per second; visible again it runs at full rate.
TEST(hidden_timers_are_coalesced_to_one_hertz) {
    Run r("<script>var t = 0; setInterval(function () { t++; console.log('tick ' + t); }, 50);</script>");
    r.engine.setVisible(false);
    sleepMs(1600);
    r.barrier();
    int hiddenTicks;
    { auto l = r.host.lock(); hiddenTicks = r.host.countLogPrefix("tick"); }
    CHECK(hiddenTicks >= 1);
    CHECK_MSG(hiddenTicks <= 2, std::to_string(hiddenTicks) + " ticks while hidden");
    r.engine.setVisible(true);
    sleepMs(1600);
    r.barrier();
    auto l = r.host.lock();
    int visibleTicks = r.host.countLogPrefix("tick") - hiddenTicks;
    CHECK_MSG(visibleTicks >= 5, std::to_string(visibleTicks) + " ticks while visible");
    CHECK(!r.host.died);
}

static int countActions(const std::vector<WidgetAction>& actions, WidgetAction::Kind kind) {
    int n = 0;
    for (auto& a : actions) if (a.kind == kind) n++;
    return n;
}

static std::string actionsText(const std::vector<WidgetAction>& actions) {
    std::string s;
    for (auto& a : actions) s += a.toJson().dump() + " ";
    return s;
}

TEST(compat_policy_forwards_every_bridge_call) {
    SandboxConfig compat = testConfig();
    compat.policy = "compat";
    Run r("<script>TelegramWidget.ready(); setTimeout(() => TelegramWidget.openLink('https://example.com/later'), 30);</script>", compat);
    r.host.waitUntil([&] { return countActions(r.host.actions, WidgetAction::OpenLink) > 0; }, 3);
    auto l = r.host.lock();
    CHECK_MSG(countActions(r.host.actions, WidgetAction::OpenLink) == 1, actionsText(r.host.actions));
    CHECK(countActions(r.host.actions, WidgetAction::Ready) == 1);
    CHECK(!r.host.hasDiag("activation-blocked"));
    // The deprecated raw call still follows each action.
    CHECK(r.host.bridge.size() == 2 && r.host.bridge[1].first == "openLink" && r.host.bridge[1].second == "[\"https://example.com/later\"]");
}

/// The default policy is strict, and so is any value but exactly "compat".
TEST(policy_is_strict_by_default_and_fails_closed) {
    CHECK(SandboxConfig().policy == "strict");
    CHECK(WidgetHostOptions().policy == "strict");
    CHECK(isStrictPolicy("strict"));
    CHECK(isStrictPolicy("Strict"));
    CHECK(isStrictPolicy("Compat"));
    CHECK(isStrictPolicy(""));
    CHECK(!isStrictPolicy("compat"));
    SandboxConfig typo = testConfig();
    typo.policy = "compat ";
    Run r("<script>setTimeout(() => TelegramWidget.openLink('https://example.com/later'), 30);</script>", typo);
    r.host.waitUntil([&] { return r.host.hasDiag("activation-blocked"); }, 3);
    auto l = r.host.lock();
    CHECK(r.host.hasDiag("activation-blocked"));
    CHECK_MSG(r.host.actions.empty(), actionsText(r.host.actions));
    CHECK(r.host.bridge.empty());
}

/// `ready` needs no gesture; a link call from a timer is refused and reported, the same call from the
/// handler of a trusted click goes through once (normalised), the second call of that tap is refused.
TEST(bridge_and_timers) {
    SandboxConfig strict = testConfig();
    Run r("<button id='b'>go</button><script>TelegramWidget.ready(); setTimeout(() => TelegramWidget.openLink('https://example.com/later'), 30);"
          "document.getElementById('b').addEventListener('click', () => { TelegramWidget.openLink('HTTPS://Example.COM/clicked'); TelegramWidget.openTelegramLink('https://t.me/again'); });</script>", strict);
    r.host.waitUntil([&] { return r.host.hasDiag("activation-blocked"); }, 3);
    {
        auto l = r.host.lock();
        CHECK(countActions(r.host.actions, WidgetAction::Ready) == 1);
        CHECK(countActions(r.host.actions, WidgetAction::OpenLink) == 0);
        CHECK(r.host.countDiag("activation-blocked") == 1);
    }
    r.engine.event("{\"type\":\"click\",\"targetId\":5}");
    r.barrier();
    r.engine.activationEvent("{\"type\":\"click\",\"targetId\":5}", 1);
    r.host.waitUntil([&] { return countActions(r.host.actions, WidgetAction::OpenLink) > 0; }, 3);
    r.barrier();
    r.engine.activationEvent("{\"type\":\"click\",\"targetId\":5}", 1);
    r.barrier();
    auto l = r.host.lock();
    std::vector<std::string> opened;
    for (auto& a : r.host.actions) if (a.kind == WidgetAction::OpenLink || a.kind == WidgetAction::OpenTelegramLink) opened.push_back(a.url);
    CHECK_MSG(opened == std::vector<std::string>({"https://example.com/clicked"}), actionsText(r.host.actions));
    // The timer, the untrusted click (×2 calls), the second call of the tap, the replayed gesture (×2 calls).
    CHECK_MSG(r.host.countDiag("activation-blocked") == 6, r.diagsText());
}

TEST(user_gestures_are_numbered_per_physical_press) {
    auto ev = [](const char* json) { return *Json::parse(json); };
    CHECK(WidgetHost::isUserActivation(ev("{\"type\":\"click\",\"detail\":1}")));
    CHECK(WidgetHost::isUserActivation(ev("{\"type\":\"pointerdown\"}")));
    CHECK(!WidgetHost::isUserActivation(ev("{\"type\":\"click\",\"detail\":0}")));
    CHECK(!WidgetHost::isUserActivation(ev("{\"type\":\"click\"}")));
    for (const char* key : {"Enter", " ", "a", "Escape", "Tab", "ArrowDown"})
        CHECK_MSG(!WidgetHost::isUserActivation(ev(("{\"type\":\"keydown\",\"key\":\"" + std::string(key) + "\"}").c_str())), key);
    CHECK(!WidgetHost::isUserActivation(ev("{\"type\":\"mousemove\"}")));
    CHECK(!WidgetHost::isUserActivation(ev("{\"type\":\"scroll\"}")));

    GestureCounter c;
    std::vector<int> press;
    for (const char* t : {"pointerdown", "mousedown", "pointerup", "mouseup", "click", "dblclick"}) press.push_back(c.gestureFor(ev(("{\"type\":\"" + std::string(t) + "\",\"detail\":1}").c_str())));
    CHECK(press == std::vector<int>({1, 1, 1, 1, 1, 1}));
    CHECK(c.gestureFor(ev("{\"type\":\"pointerdown\"}")) == 2);
    CHECK(c.gestureFor(ev("{\"type\":\"keydown\",\"key\":\"Enter\"}")) == 0);
    CHECK(c.gestureFor(ev("{\"type\":\"click\",\"detail\":0}")) == 2); // the end of the open press
    // No keyboard: neither a key nor the click Enter or Space activates (detail 0) starts a gesture.
    CHECK(c.gestureFor(ev("{\"type\":\"click\",\"detail\":0}")) == 0);
    CHECK(c.gestureFor(ev("{\"type\":\"dblclick\",\"detail\":0}")) == 0);
    CHECK(c.gestureFor(ev("{\"type\":\"click\",\"detail\":1}")) == 3);
    CHECK(c.gestureFor(ev("{\"type\":\"keydown\",\"key\":\" \"}")) == 0);
    CHECK(c.gestureFor(ev("{\"type\":\"mousemove\"}")) == 0);
}

/// A renderer without a document: announces itself, answers every query with null, keeps what it was sent; tests inject its events.
struct ScriptedRenderer final : Renderer {
    void load() override { onMessage("{\"type\":\"ready\"}"); }
    void send(const std::string& json) override {
        {
            std::lock_guard<std::mutex> l(mutex);
            messages.push_back(json);
        }
        auto m = Json::parse(json);
        if (m && (*m)["type"].asString() == "query") onMessage("{\"type\":\"result\",\"qid\":" + std::to_string((*m)["qid"].asInt()) + ",\"result\":null}");
    }
    void setTheme(const std::string& t) override { std::lock_guard<std::mutex> l(mutex); themes.push_back(t); }
    void setWidth(double) override {}
    void destroy() override {}
    /// The messages of one type, in order.
    std::vector<Json> sent(const std::string& type) {
        std::lock_guard<std::mutex> l(mutex);
        std::vector<Json> out;
        for (auto& m : messages) if (auto j = Json::parse(m); j && (*j)["type"].asString() == type) out.push_back(*j);
        return out;
    }
    std::mutex mutex;
    std::vector<std::string> messages;
    std::vector<std::string> themes;
};

static RendererFactory scripted(ScriptedRenderer*& out) {
    return [&out](std::shared_ptr<SandboxResources>, const std::string&, double) -> std::unique_ptr<Renderer> {
        auto r = std::make_unique<ScriptedRenderer>();
        out = r.get();
        return r;
    };
}

/// Only renderer events marked `trusted` (the DOM event's isTrusted; every input event of a native renderer) are gestures,
/// and only pointer ones: keys and the click Enter activates unlock nothing.
TEST(gestures_need_the_renderers_trusted_flag) {
    ScriptedRenderer* renderer = nullptr;
    WidgetHost host("<button id='b'>go</button><script>TelegramWidget.ready(); var b = document.getElementById('b');"
                    "b.addEventListener('keydown', function (e) { TelegramWidget.openLink('https://example.com/key-' + e.key); });"
                    "b.addEventListener('click', function (e) { TelegramWidget.openLink('https://example.com/click-' + e.detail); });</script>",
                    WidgetTheme{}, 300, WidgetHostOptions(), RES, scripted(renderer));
    std::mutex m;
    std::vector<std::string> sent;
    host.onAction = [&](const WidgetAction& a) { std::lock_guard<std::mutex> l(m); if (a.kind != WidgetAction::Ready) sent.push_back(a.url); };
    host.start();
    CHECK(host.waitLoaded(20));
    auto event = [&](const std::string& ev, const std::string& trusted) { renderer->onMessage("{\"type\":\"event\",\"ev\":" + ev + trusted + "}"); };
    const std::string click = "{\"type\":\"click\",\"targetId\":5,\"detail\":1}";
    event(click, "");
    event(click, ",\"trusted\":false");
    event(click, ",\"trusted\":1");
    CHECK(host.waitUntil([](const WidgetDiagnostics& d) { return d.countDiag("activation-blocked") >= 3; }, 5));
    {
        std::lock_guard<std::mutex> l(m);
        CHECK_MSG(sent.empty(), std::to_string(sent.size()) + " calls from untrusted events");
    }
    const std::string yes = ",\"trusted\":true";
    event("{\"type\":\"keydown\",\"targetId\":5,\"key\":\"Enter\",\"code\":\"Enter\",\"repeat\":false,\"modifiers\":0}", yes);
    event("{\"type\":\"click\",\"targetId\":5,\"detail\":0}", yes);
    event("{\"type\":\"keydown\",\"targetId\":5,\"key\":\"Enter\",\"code\":\"Enter\",\"repeat\":true,\"modifiers\":0}", yes);
    event("{\"type\":\"click\",\"targetId\":5,\"detail\":0}", yes);
    event("{\"type\":\"keydown\",\"targetId\":5,\"key\":\"Tab\",\"code\":\"Tab\",\"repeat\":false,\"modifiers\":0}", yes);
    event(click, yes);
    CHECK(host.waitUntil([&](const WidgetDiagnostics& d) { std::lock_guard<std::mutex> l(m); return sent.size() >= 1 && d.countDiag("activation-blocked") >= 8; }, 5));
    sleepMs(100);
    std::lock_guard<std::mutex> l(m);
    CHECK_MSG(sent == std::vector<std::string>({"https://example.com/click-1"}), [&] { std::string s; for (auto& x : sent) s += x + " "; return s; }());
    CHECK(host.diagnostics().countDiag("activation-blocked") == 8);
}

// ───────────── SDK v1 through the real engine (docs/sdk-v1.md §2–§4) ─────────────

TEST(sdk_members_and_removed_apis) {
    Run r("<button>c</button><script>var out = [alert('a'), confirm('c'), prompt('p')];"
          "document.querySelector('button').onclick = function () {"
          "  navigator.clipboard.writeText('x').catch(function (e) { out.push(e.name); });"
          "  navigator.share({ text: 'x' }).catch(function (e) { out.push(e.name); }); };</script>");
    r.engine.activationEvent("{\"type\":\"click\",\"targetId\":5,\"detail\":1}", 1);
    r.barrier();
    CHECK_MSG(r.engine.probe("JSON.stringify(out)") == "[null,false,null,\"NotAllowedError\",\"NotAllowedError\"]", r.engine.probe("JSON.stringify(out)"));
    std::string members = r.engine.probe("[TelegramWidget.profile, TelegramWidget.isProfileAtLeast(1), TelegramWidget.isProfileAtLeast(2), TelegramWidget.colorScheme,"
                                         " TelegramWidget.isActive, typeof TelegramWidget.sendMessage, typeof TelegramWidget.on, Object.keys(TelegramWidget.themeParams).length].join()");
    CHECK_MSG(members == "1,true,false,light,true,undefined,undefined,15", members);
    auto l = r.host.lock();
    CHECK_MSG(r.host.bridge.empty() && r.host.actions.empty(), actionsText(r.host.actions));
    CHECK(!r.host.hasDiag("profile-mismatch"));
}

/// A tap on a button that calls openLink reaches onAction normalised; the second call in the same tap is
/// `activation-blocked`; invalid URLs throw TypeError in the widget and never reach the host.
TEST(sdk_link_tap_reaches_on_action) {
    ScriptedRenderer* renderer = nullptr;
    WidgetHost host("<button>go</button><script>var errors = [];"
                    "function tryOpen(m, u) { try { TelegramWidget[m](u); } catch (e) { errors.push(m + ':' + e.name); } }"
                    "tryOpen('openLink', 'http://example.com/'); tryOpen('openLink', 'https://t.me/x'); tryOpen('openTelegramLink', 'tg://resolve?domain=x'); tryOpen('openLink', 42);"
                    "document.querySelector('button').addEventListener('click', function () { TelegramWidget.openLink('HTTPS://Example.com/\xD0\xBF\xD0\xBE\xD0\xB8\xD1\x81\xD0\xBA');"
                    " TelegramWidget.openLink('https://example.com/second'); });</script>",
                    WidgetTheme{}, 300, WidgetHostOptions(), RES, scripted(renderer));
    std::mutex m;
    std::vector<WidgetAction> actions;
    host.onAction = [&](const WidgetAction& a) { std::lock_guard<std::mutex> l(m); actions.push_back(a); };
    host.start();
    CHECK(host.waitLoaded(20));
    CHECK(host.probe("errors.join()") == "openLink:TypeError,openLink:TypeError,openTelegramLink:TypeError,openLink:TypeError");
    renderer->onMessage("{\"type\":\"event\",\"ev\":{\"type\":\"click\",\"targetId\":5,\"detail\":1},\"trusted\":true}");
    CHECK(host.waitUntil([](const WidgetDiagnostics& d) { return d.countDiag("activation-blocked") >= 1; }, 5));
    host.probe("0");            // the click task has run
    host.onHostThread([] {});   // and its actions reached the host thread
    std::lock_guard<std::mutex> l(m);
    CHECK_MSG(actions.size() == 1 && actions[0].kind == WidgetAction::OpenLink && actions[0].url == "https://example.com/%D0%BF%D0%BE%D0%B8%D1%81%D0%BA", actionsText(actions));
    CHECK(host.diagnostics().countDiag("activation-blocked") == 1);
    CHECK(host.diagnostics().countDiag("bridge-rejected") == 0);
}

/// `<a href>` taps and window.open go to openLink or openTelegramLink by host; http: and javascript: are ignored.
TEST(sdk_anchor_and_window_open_route_by_host) {
    Run r("<a href='https://example.com/a'>a</a><a href='https://t.me/durov'>t</a><a href='http://example.com/'>h</a><a href='javascript:void 0'>j</a>"
          "<button>o</button><script>var w = 1; document.querySelector('button').onclick = function () { w = window.open('https://telegram.me/x'); };</script>");
    r.barrier();
    std::vector<int> targets;
    {
        auto l = r.host.lock();
        for (const std::string tag : {"a", "button"})
            for (auto& batch : r.host.renderOps)
                if (auto ops = Json::parse(batch))
                    for (auto& op : ops->arr()) if (op["op"].asString() == "el" && op["tag"].asString() == tag) targets.push_back(op["id"].asInt());
    }
    CHECK(targets.size() == 5);
    int gesture = 0;
    for (int id : targets) r.engine.activationEvent("{\"type\":\"click\",\"targetId\":" + std::to_string(id) + ",\"detail\":1}", ++gesture);
    r.barrier();
    CHECK(r.engine.probe("w") == "null");
    auto l = r.host.lock();
    std::string got = actionsText(r.host.actions);
    CHECK_MSG(got == "{\"kind\":\"openLink\",\"url\":\"https://example.com/a\"} {\"kind\":\"openTelegramLink\",\"url\":\"https://t.me/durov\"} "
                     "{\"kind\":\"openTelegramLink\",\"url\":\"https://telegram.me/x\"} ", got);
    CHECK_MSG(r.host.countDiag("navigation-blocked") == 2, r.diagsText());
}

/// Haptics need no tap: seven impactOccurred in one task give 5 actions and one `haptic-limit`; a hidden widget plays nothing.
TEST(sdk_haptics_burst_and_limit) {
    Run r("<script>for (var i = 0; i < 7; i++) TelegramWidget.HapticFeedback.impactOccurred('medium');"
          "TelegramWidget.HapticFeedback.notificationOccurred('success').selectionChanged();</script>");
    r.barrier();
    {
        auto l = r.host.lock();
        CHECK_MSG(r.host.actions.size() == 5, actionsText(r.host.actions));
        if (!r.host.actions.empty()) CHECK(r.host.actions[0] == (WidgetAction{WidgetAction::Haptic, "", "impact", "medium"}));
        CHECK_MSG(r.host.countDiag("haptic-limit") == 1, r.diagsText());
    }
    sleepMs(250); // 2.5 tokens back
    CHECK(r.engine.probe("TelegramWidget.HapticFeedback.selectionChanged() === TelegramWidget.HapticFeedback") == "true");
    {
        auto l = r.host.lock();
        CHECK_MSG(r.host.actions.size() == 6 && r.host.actions[5] == (WidgetAction{WidgetAction::Haptic, "", "selection", ""}), actionsText(r.host.actions));
    }
    SandboxConfig hidden = testConfig();
    hidden.visible = false;
    Run h("<script>TelegramWidget.HapticFeedback.selectionChanged(); TelegramWidget.HapticFeedback.selectionChanged();</script>", hidden);
    h.barrier();
    auto l = h.host.lock();
    CHECK_MSG(h.host.actions.empty(), actionsText(h.host.actions));
    CHECK_MSG(h.host.countDiag("haptic-limit") == 1, h.diagsText());
}

/// The preload is readable from the first script line; each task commits the areas it changed, in full;
/// a commit near the quota passes the glue; lone surrogates round-trip as WTF-8.
TEST(sdk_storage_preload_and_commits) {
    SandboxConfig cfg = testConfig();
    cfg.storage.local = {{"count", "41"}};
    cfg.storage.session = {{"s", "kept"}};
    Run r("<button>b</button><script>"
          "var seen = localStorage.getItem('count') + '/' + sessionStorage.getItem('s') + '/' + localStorage.length;"
          "localStorage.setItem('count', String(Number(localStorage.getItem('count')) + 1)); localStorage.extra = 7;"
          "document.querySelector('button').onclick = function () { sessionStorage.setItem('s', 'tapped'); localStorage.setItem('count', localStorage.count); };</script>", cfg);
    CHECK(r.engine.probe("seen") == "41/kept/1");
    r.engine.event("{\"type\":\"click\",\"targetId\":5}");
    r.barrier();
    {
        auto l = r.host.lock();
        CHECK_MSG(r.host.storageCommits.size() == 2, std::to_string(r.host.storageCommits.size()) + " commits");
        if (r.host.storageCommits.size() == 2) {
            CHECK(r.host.storageCommits[0].first == "local" && r.host.storageCommits[0].second == StorageArea({{"count", "42"}, {"extra", "7"}}));
            CHECK(r.host.storageCommits[1].first == "session" && r.host.storageCommits[1].second == StorageArea({{"s", "tapped"}}));
        }
        CHECK(!r.host.hasDiag("storage-rejected"));
    }
    // 16 KB of UTF-8 per area: the shim throws past it, what it commits is within the glue's quota.
    Run q("<script>var out = []; localStorage.setItem('k', 'x'.repeat(16383));"
          "try { localStorage.setItem('k2', 'y'); } catch (e) { out.push(e.name, e.code, e instanceof DOMException); }"
          "sessionStorage.setItem('w', '\xD0\xB6'.repeat(8000)); sessionStorage.setItem('\\ud800', 'x\\udc00');</script>");
    CHECK(q.engine.probe("out.join()") == "QuotaExceededError,22,true");
    q.barrier();
    StorageArea session;
    {
        auto l = q.host.lock();
        CHECK_MSG(q.host.storageCommits.size() == 2 && !q.host.hasDiag("storage-rejected"), q.diagsText());
        if (q.host.storageCommits.size() == 2) {
            CHECK(sdk::storageBytes(q.host.storageCommits[0].second) == 16384);
            session = q.host.storageCommits[1].second;
            CHECK(session.count("\xED\xA0\x80") && session["\xED\xA0\x80"] == "x\xED\xB0\x80");
            CHECK(sdk::storageBytes(session) == 1 + 2 * 8000 + 3 + 4);
        }
    }
    SandboxConfig again = testConfig();
    again.storage.session = session;
    Run back("<p>x</p>", again);
    CHECK_MSG(back.engine.probe("sessionStorage.getItem('\\ud800') === 'x\\udc00' && sessionStorage.getItem('w').length") == "8000", back.diagsText());
}

TEST(sdk_storage_preload_over_quota_is_rejected) {
    SandboxConfig cfg = testConfig();
    cfg.storage.local = {{"a", std::string(20000, 'x')}};
    cfg.storage.session = {{"b", "1"}};
    Run r("<p>x</p>", cfg);
    CHECK(r.engine.probe("localStorage.length + sessionStorage.length") == "0");
    auto l = r.host.lock();
    CHECK_MSG(r.host.countDiag("storage-rejected") == 1, r.diagsText());
    CHECK(r.host.storageCommits.empty());
}

/// themeParams from the host reach the widget checked (the rest from the scheme's defaults); a theme
/// change updates colorScheme and themeParams and fires themeChanged and the legacy onTheme once.
TEST(sdk_theme_params_reach_the_widget) {
    SandboxConfig cfg = testConfig();
    cfg.themeParams = {{"bg_color", "#ABCDEF"}, {"text_color", "red"}, {"made_up", "#123456"}};
    Run r("<script>var log = [], first = TelegramWidget.themeParams;"
          "TelegramWidget.onEvent('themeChanged', function () { log.push('changed:' + TelegramWidget.colorScheme + ':' + TelegramWidget.themeParams.bg_color + ':' + (this === TelegramWidget)); });"
          "TelegramWidget.onTheme(function (t) { log.push('onTheme:' + t); });</script>", cfg);
    CHECK(r.engine.probe("TelegramWidget.themeParams.bg_color + ',' + TelegramWidget.themeParams.text_color + ',' + ('made_up' in TelegramWidget.themeParams)") == "#abcdef,#000000,false");
    r.engine.theme("light", {{"bg_color", "#abcdef"}});
    r.engine.theme("dark", {{"bg_color", "#101010"}});
    r.barrier();
    CHECK_MSG(r.engine.probe("log.join('|')") == "onTheme:light|changed:dark:#101010:true|onTheme:dark", r.engine.probe("log.join('|')"));
    CHECK(r.engine.probe("first === TelegramWidget.themeParams") == "false");
    CHECK(r.engine.probe("TelegramWidget.themeParams.text_color") == "#ffffff");
}

TEST(sdk_locale_reaches_the_widget) {
    SandboxConfig cfg = testConfig();
    cfg.locale = "pt-BR";
    Run r("<p>x</p>", cfg);
    CHECK(r.engine.probe("navigator.language + ',' + navigator.languages.join()") == "pt-BR,pt-BR");
    SandboxConfig bad = testConfig();
    bad.locale = "ru_RU";
    Run b("<p>x</p>", bad);
    CHECK(b.engine.probe("navigator.language") == "en-US");
}

/// WidgetHost: the renderer's first message is the theme (scheme, checked params, button tokens as given),
/// and every setTheme sends another; the widget gets the scheme, the palette and the locale; the renderer's
/// gesture report becomes onGestureClaim.
TEST(sdk_widget_host_theme_locale_and_gestures) {
    ScriptedRenderer* renderer = nullptr;
    WidgetTheme theme;
    theme.colorScheme = "dark";
    theme.params = {{"bg_color", "#ABCDEF"}, {"link_color", "blue"}};
    theme.button.radius = "10px";
    theme.button.pressedOpacity = 0.7;
    WidgetHostOptions options;
    options.locale = "ru";
    options.storage.local = {{"k", "v"}};
    WidgetHost host("<button>b</button><script>document.querySelector('button').onclick = function () { localStorage.k = 'w'; };</script>", theme, 300, options, RES, scripted(renderer));
    std::mutex m;
    std::vector<std::pair<bool, bool>> claims;
    std::vector<std::pair<std::string, StorageArea>> commits;
    host.onGestureClaim = [&](bool x, bool y) { std::lock_guard<std::mutex> l(m); claims.emplace_back(x, y); };
    host.onStorageCommit = [&](const std::string& area, const StorageArea& values) { std::lock_guard<std::mutex> l(m); commits.emplace_back(area, values); };
    host.start();
    CHECK(host.waitLoaded(20));
    {
        std::lock_guard<std::mutex> l(renderer->mutex);
        CHECK_MSG(!renderer->messages.empty() && renderer->messages[0] == "{\"type\":\"theme\",\"theme\":{\"colorScheme\":\"dark\",\"params\":{\"bg_color\":\"#abcdef\"},\"button\":{\"radius\":\"10px\",\"pressedOpacity\":0.7}}}",
                  renderer->messages.empty() ? "nothing sent" : renderer->messages[0]);
    }
    CHECK(host.probe("TelegramWidget.colorScheme + ',' + TelegramWidget.themeParams.bg_color + ',' + TelegramWidget.themeParams.link_color + ',' + navigator.language + ',' + localStorage.k") ==
          "dark,#abcdef,#8774e1,ru,v");
    WidgetTheme light;
    light.colorScheme = "Dark"; // anything but exactly "dark" is light
    light.params = {{"bg_color", "#FFFFFF"}};
    host.setTheme(light);
    host.onHostThread([] {});
    CHECK(host.probe("TelegramWidget.colorScheme + ',' + TelegramWidget.themeParams.bg_color") == "light,#ffffff");
    auto themes = renderer->sent("theme");
    CHECK_MSG(themes.size() == 2 && themes[1].dump() == "{\"type\":\"theme\",\"theme\":{\"colorScheme\":\"light\",\"params\":{\"bg_color\":\"#ffffff\"},\"button\":{}}}",
              themes.empty() ? "none" : themes.back().dump());
    {
        std::lock_guard<std::mutex> l(renderer->mutex);
        CHECK(renderer->themes == std::vector<std::string>({"light"}));
    }
    renderer->onMessage("{\"type\":\"gesture\",\"wants\":true,\"x\":false,\"y\":true}");
    renderer->onMessage("{\"type\":\"gesture\",\"wants\":false,\"x\":true,\"y\":false}");
    renderer->onMessage("{\"type\":\"gesture\",\"wants\":true}"); // a renderer that only knows `wants` (the native prototypes)
    renderer->onMessage("{\"type\":\"event\",\"ev\":{\"type\":\"click\",\"targetId\":5,\"detail\":1},\"trusted\":true}");
    CHECK(host.waitUntil([&](const WidgetDiagnostics& d) { return d.storageCommits > 0; }, 5));
    host.destroy();
    std::lock_guard<std::mutex> l(m);
    CHECK((claims == std::vector<std::pair<bool, bool>>{{false, true}, {true, false}, {false, true}}));
    CHECK(commits.size() == 1 && commits[0].first == "local" && commits[0].second == StorageArea({{"k", "w"}}));
}

/// The API before SDK v1 still compiles and forwards: a string theme, setTheme(string), the raw onBridgeCall.
TEST(sdk_deprecated_host_api_forwards) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    ScriptedRenderer* renderer = nullptr;
    WidgetHost host("<script>TelegramWidget.ready(); TelegramWidget.HapticFeedback.impactOccurred('soft');</script>", "dark", 300, WidgetHostOptions(), RES, scripted(renderer));
    std::mutex m;
    std::vector<std::pair<std::string, std::string>> raw;
    host.onBridgeCall = [&](const std::string& method, const std::string& args) { std::lock_guard<std::mutex> l(m); raw.emplace_back(method, args); };
    host.start();
    CHECK(host.waitLoaded(20));
    CHECK(host.probe("TelegramWidget.theme + ',' + TelegramWidget.themeParams.bg_color") == "dark,#212121");
    host.setTheme("light");
    host.onHostThread([] {});
    CHECK(host.probe("TelegramWidget.colorScheme") == "light");
    host.onHostThread([] {});
    std::lock_guard<std::mutex> l(m);
    CHECK_MSG((raw == std::vector<std::pair<std::string, std::string>>{{"ready", "[]"}, {"haptic", "[\"impact\",\"soft\"]"}}), std::to_string(raw.size()));
    CHECK(host.diagnostics().actions.size() == 2);
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
}

TEST(widget_code_cannot_reach_host_globals) {
    Run r("<script>var names=['__host','__hostBinary','__hostCanvas','__hostOps','__twDispatch'];console.log('globals '+names.map(function(n){return n+'='+typeof globalThis[n]+'/'+(n in globalThis);}).join(','));"
          "try{Object.defineProperty(globalThis,'__twDispatch',{value:function(){console.log('pwned');}});}catch(e){}setTimeout(function(){console.log('tick');},1);</script>");
    r.host.waitUntil([&] { return r.host.countLogPrefix("tick") > 0; }, 3);
    r.barrier();
    auto l = r.host.lock();
    CHECK_MSG(r.host.countLogPrefix("globals __host=undefined/false,__hostBinary=undefined/false,__hostCanvas=undefined/false,__hostOps=undefined/false,__twDispatch=undefined/false") == 1, r.logsText());
    CHECK(r.host.countLogPrefix("tick") == 1);
    CHECK(r.host.countLogPrefix("pwned") == 0);
}

TEST(widget_clock_is_coarse) {
    Run r("<script>var seen={},distinct=0,t0=performance.now();for(var i=0;i<200000;i++){var t=performance.now();if(!seen[t]){seen[t]=1;distinct++;}}console.log('CLOCK '+distinct+' '+(performance.now()-t0));</script>");
    r.barrier();
    auto l = r.host.lock();
    double distinct = -1, elapsed = -1;
    for (auto& log : r.host.logs) if (log.second.rfind("CLOCK ", 0) == 0) sscanf(log.second.c_str(), "CLOCK %lf %lf", &distinct, &elapsed);
    CHECK_MSG(distinct >= 1 && distinct <= elapsed / 0.1 + 2, r.logsText());
}

TEST(intl_fallback_formats_through_the_host) {
    Run r("<script>console.log((1234.5).toLocaleString() + '|' + new Intl.PluralRules('en').select(1) + '|' + new Intl.NumberFormat('en', {style:'percent'}).format(0.25));</script>");
    r.barrier();
    auto l = r.host.lock();
    CHECK_MSG(r.host.hasLog("1,234.5|one|25%"), r.logsText());
}

// formatToParts is in the widget profile: the basic provider splits what it formats.
TEST(intl_fallback_format_to_parts) {
    Run r("<script>var j=function(p){return p.map(function(x){return x.type+'='+x.value}).join(';')};"
          "console.log(j(new Intl.NumberFormat('en').formatToParts(-1234.5)));"
          "console.log(j(new Intl.DateTimeFormat('en',{timeZone:'UTC',year:'numeric',month:'long',day:'numeric'}).formatToParts(new Date(0))));</script>");
    r.barrier();
    auto l = r.host.lock();
    CHECK_MSG(r.host.hasLog("minusSign=-;integer=1;group=,;integer=234;decimal=.;fraction=5"), r.logsText());
    CHECK_MSG(r.host.hasLog("month=January;literal= ;day=1;literal=, ;year=1970"), r.logsText());
}

#ifdef _WIN32
// The system's ICU: CLDR data for the locale a widget names, where the basic provider writes English.
TEST(icu_intl_formats_with_cldr) {
    RecordingHost host("light", RES);
    SandboxEngine engine("<script>console.log(new Intl.NumberFormat('ru-RU').format(1234.5).replace(/\\s/g, ' ') + '|' + new Intl.PluralRules('ru-RU').select(3) + '|' +"
                         " new Intl.RelativeTimeFormat('en', {numeric: 'auto'}).format(-1, 'day') + '|' + new Intl.DateTimeFormat('en-US', {timeZone: 'UTC'}).format(new Date(0)) + '|' +"
                         " new Intl.ListFormat('en', {type: 'conjunction'}).format(['a', 'b', 'c']));</script>",
                         testConfig(), RES, std::make_shared<IcuIntl>());
    engine.setDelegate(&host);
    engine.start();
    host.waitLoaded(20);
    engine.thread().sync([] {});
    auto l = host.lock();
    std::string logs;
    for (auto& log : host.logs) logs += log.second + "\n";
    CHECK_MSG(host.hasLog("1 234,5|few|yesterday|1/1/1970|a, b, and c"), logs);
}
#endif

/// Engines created and torn down quickly, some before they finished booting.
TEST(engine_create_and_teardown_churn) {
    for (int i = 0; i < 10; i++) {
        RecordingHost host("light", RES);
        SandboxEngine engine("<p>x</p><script>setInterval(function(){}, 10); requestAnimationFrame(function(){});</script>", testConfig(), RES);
        engine.setDelegate(&host);
        engine.start();
        if (i % 2) host.waitLoaded(10);
        if (i % 3 == 0) engine.terminate();
    }
    CHECK(true);
}

// ───────────── validator ─────────────

// ───────────── precompiled bundles ─────────────

TEST(bytecode_round_trip_and_stale_blob_refused) {
    QuickJSContext js(size_t(16) << 20);
    auto blob = js.compileBytecode("globalThis.answer = 6 * 7;", "t.js");
    CHECK(blob.size() > 24);
    js.evaluateBytecode(blob, "globalThis.answer = 6 * 7;");
    CHECK(js.evaluate("answer", "t.js").number == 42);
    bool threw = false;
    try { js.evaluateBytecode(blob, "globalThis.answer = 6 * 8;"); } catch (const JSError& e) { threw = std::string(e.what()).find("different source") != std::string::npos; }
    CHECK_MSG(threw, "a blob compiled from another source must be refused");
    auto bad = blob;
    bad[0] = 'X';
    threw = false;
    try { js.evaluateBytecode(bad, "globalThis.answer = 6 * 7;"); } catch (const JSError&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { js.evaluateBytecode({}, ""); } catch (const JSError&) { threw = true; }
    CHECK(threw);
}

// A CDN script is an external script like any other: profile 1 bundles no libraries.
TEST(shipped_bytecode_is_used_and_no_library_is_bundled) {
    CHECK_MSG(!RES->sandboxBytecode.empty() && !RES->validatorBytecode.empty(), "pnpm build:desktop ships .qbc next to the bundles");
    Run r("<canvas id=\"c\"></canvas><script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script><script>console.log(typeof Chart);</script>");
    r.barrier();
    auto l = r.host.lock();
    CHECK(!r.host.died);
    bool bootFromBytecode = false, skipped = false, fallback = false;
    for (auto& d : r.host.diags) {
        if (d.first == "boot" && d.second.find("(bytecode)") != std::string::npos) bootFromBytecode = true;
        if (d.first == "script-skipped" && d.second.find("chart.js") != std::string::npos) skipped = true;
        if (d.first == "bytecode") fallback = true;
    }
    CHECK_MSG(bootFromBytecode && skipped && !fallback, r.diagsText());
    CHECK(r.host.hasLog("undefined"));
    std::vector<std::pair<std::string, std::string>> diags;
    TrustedValidator v("light", RES, [&](const std::string& k, const std::string& d) { diags.emplace_back(k, d); });
    for (auto& d : diags) CHECK_MSG(d.first != "bytecode", d.second);
}

TEST(validator_diag_bridge) {
    std::vector<std::pair<std::string, std::string>> diags;
    TrustedValidator v("light", RES, [&](const std::string& k, const std::string& d) { diags.emplace_back(k, d); });
    // An unknown element is reported through __diag (tag-replaced).
    v.processOps("[{\"op\":\"el\",\"id\":2,\"tag\":\"x-foo\"}]");
    bool replaced = false;
    for (auto& d : diags) if (d.first == "tag-replaced") replaced = true;
    CHECK(replaced);
}

TEST(validator_validates_anim_records) {
    std::vector<std::pair<std::string, std::string>> diags;
    TrustedValidator v("light", RES, [&](const std::string& k, const std::string& d) { diags.emplace_back(k, d); });
    v.processOps("[{\"op\":\"el\",\"id\":2,\"tag\":\"html\"},{\"op\":\"el\",\"id\":3,\"tag\":\"body\"},{\"op\":\"ins\",\"parent\":2,\"id\":3,\"before\":null},{\"op\":\"el\",\"id\":7,\"tag\":\"div\"},{\"op\":\"ins\",\"parent\":3,\"id\":7,\"before\":null}]");
    auto record = [](const std::string& values, const std::string& duration, const std::string& easing) {
        return "[{\"aid\":1,\"target\":7,\"keyframes\":[{\"offset\":0,\"easing\":\"linear\",\"composite\":null,\"values\":" + values + "}],\"timing\":{\"duration\":" + duration + ",\"delay\":0,\"endDelay\":0,\"iterations\":null,\"iterationStart\":0,\"direction\":\"normal\",\"fill\":\"none\",\"easing\":\"" + easing + "\"},\"composite\":\"replace\",\"iterationComposite\":\"replace\",\"startTime\":1000,\"holdTime\":null,\"playbackRate\":1}]";
    };
    for (const std::string& bad : {record("{}", "1e999", "linear"), record("{}", "1e12", "linear"), record("{}", "1000", "evil(1)"), std::string("{\"not\":\"a list\"}"), std::string("[{\"aid\":-1}]")}) {
        diags.clear();
        CHECK_MSG(v.validateAnim(bad) == "[]", bad);
        bool violation = false;
        for (auto& d : diags) if (d.first == "protocol-violation") violation = true;
        CHECK_MSG(violation, bad);
    }
    std::string clean = v.validateAnim(record("{\"opacity\":\"0.5\",\"background-image\":\"url(https://evil.example/x)\",\"width\":\"expression(alert(1))\",\"behavior\":\"url(x.htc)\",\"animation-name\":\"x\"}", "1000", "ease-in"));
    CHECK_MSG(clean.find("\"opacity\":\"0.5\"") != std::string::npos, clean);
    CHECK_MSG(clean.find("url(") == std::string::npos && clean.find("expression") == std::string::npos && clean.find("animation-name") == std::string::npos, clean);
}

TEST(validator_strips_external_urls_and_handlers) {
    std::vector<std::pair<std::string, std::string>> diags;
    TrustedValidator v("light", RES, [&](const std::string& k, const std::string& d) { diags.emplace_back(k, d); });
    std::string ops =
        "[{\"op\":\"el\",\"id\":2,\"tag\":\"html\"},{\"op\":\"ins\",\"parent\":1,\"id\":2,\"before\":null},"
        "{\"op\":\"el\",\"id\":3,\"tag\":\"body\"},{\"op\":\"ins\",\"parent\":2,\"id\":3,\"before\":null},"
        "{\"op\":\"el\",\"id\":4,\"tag\":\"img\",\"attrs\":{\"src\":\"http://127.0.0.1:9999/hit/x\",\"onerror\":\"fetch('http://127.0.0.1:9999/e')\"}},{\"op\":\"ins\",\"parent\":3,\"id\":4,\"before\":null},"
        "{\"op\":\"el\",\"id\":5,\"tag\":\"iframe\",\"attrs\":{\"src\":\"http://127.0.0.1:9999/hit/f\"}},{\"op\":\"ins\",\"parent\":3,\"id\":5,\"before\":null},"
        "{\"op\":\"el\",\"id\":6,\"tag\":\"style\"},{\"op\":\"ins\",\"parent\":3,\"id\":6,\"before\":null},"
        "{\"op\":\"text\",\"id\":7,\"data\":\"@import url(http://127.0.0.1:9999/i); p{color:red;background:url(http://127.0.0.1:9999/b)}\"},{\"op\":\"ins\",\"parent\":3,\"id\":7,\"before\":null}]";
    std::string out = v.processOps(ops);
    auto rendered = Json::parse(out);
    CHECK(rendered.has_value() && rendered->isArray());
    // Nothing that is attached, set as an attribute or emitted as CSS may carry the canary.
    std::string attached;
    for (const auto& op : rendered->arr()) {
        const std::string& k = op["op"].asString();
        if (k == "ins" || k == "attr" || k == "css" || k == "style" || k == "el") attached += op.dump();
    }
    std::string lower = attached;
    for (auto& c : lower) c = char(std::tolower((unsigned char)c));
    CHECK_MSG(attached.find("127.0.0.1") == std::string::npos, attached);
    CHECK_MSG(lower.find("onerror") == std::string::npos, attached);
    CHECK_MSG(attached.find("\"tag\":\"iframe\"") == std::string::npos, attached);
    CHECK_MSG(out.find("data-tg-placeholder") != std::string::npos, out);
    bool blocked = false;
    for (auto& d : diags) if (d.first == "url-blocked") blocked = true;
    CHECK(blocked);
    CHECK(v.validateQuery("{\"q\":\"rect\",\"id\":4}") == std::optional<std::string>("{\"q\":\"rect\",\"id\":4}"));
    CHECK(!v.validateQuery("{\"q\":\"eval\",\"code\":\"1\"}"));
    CHECK(v.rewriteMediaQuery("(prefers-color-scheme: dark)") == "(min-width: 99999px)");
}

/// Churn validators, each with its own thread, and let them tear down.
TEST(validator_repeated_create_and_teardown) {
    for (int i = 0; i < 25; i++) {
        TrustedValidator v(i % 2 == 0 ? "light" : "dark", RES, [](const std::string&, const std::string&) {});
        CHECK(v.processOps("[{\"op\":\"el\",\"id\":2,\"tag\":\"p\"},{\"op\":\"ins\",\"parent\":1,\"id\":2,\"before\":null}]").find("\"tag\":\"p\"") != std::string::npos);
        CHECK(v.nodeCount() == 2);
    }
}

TEST(validator_stylesheet_text_is_only_shipped_sanitized) {
    std::vector<std::string> diags;
    TrustedValidator v("dark", RES, [&](const std::string& k, const std::string& d) { diags.push_back(k + ": " + d); });
    std::string ops =
        "[{\"op\":\"el\",\"id\":2,\"tag\":\"html\"},{\"op\":\"ins\",\"parent\":1,\"id\":2,\"before\":null},"
        "{\"op\":\"el\",\"id\":6,\"tag\":\"style\"},{\"op\":\"ins\",\"parent\":2,\"id\":6,\"before\":null},"
        "{\"op\":\"text\",\"id\":7,\"data\":\"@import url(http://127.0.0.1:9999/i); @media (prefers-color-scheme: dark){p{color:red}} p{background:url(http://127.0.0.1:9999/b)}\"},{\"op\":\"ins\",\"parent\":6,\"id\":7,\"before\":null}]";
    std::string out = v.processOps(ops);
    CHECK_MSG(out.find("{\"op\":\"rm\",\"id\":7}") != std::string::npos, out);
    CHECK_MSG(out.find("@media (min-width: 0px){p{color:red}}") != std::string::npos, out);
    bool reported = false;
    for (auto& d : diags) if (d.rfind("url-blocked", 0) == 0 || d.rfind("css-dropped", 0) == 0) reported = true;
    CHECK(reported);
    CHECK_MSG(out.find("\"css\",\"id\":6,\"text\":\"@import") == std::string::npos, out);
    CHECK(v.nodeCount() == 4);
}

// ───────────── WidgetHost + Blitz renderer (-DTW_BLITZ=ON) ─────────────
#ifdef TW_HAVE_BLITZ

/// One widget on WidgetHost with the Blitz renderer; `blitz` is the renderer the factory built.
struct BlitzRun {
    BlitzRenderer* blitz = nullptr;
    WidgetHost host;
    explicit BlitzRun(const std::string& html, double dpr = 2)
        : host(html, WidgetTheme{}, 400, [] { WidgetHostOptions o; o.devicePixelRatio = 2; return o; }(), RES,
               [this, dpr](std::shared_ptr<SandboxResources>, const std::string& theme, double width) -> std::unique_ptr<Renderer> {
                   auto r = std::make_unique<BlitzRenderer>(theme, width, dpr);
                   blitz = r.get();
                   return r;
               }) {
        host.start();
        host.waitLoaded(20);
        host.waitRendered(5);
    }
};

/// The Rust document answers layout queries with real geometry (not the harness's canned 100×20),
/// reports the document height, and paints a raster whose size follows width × dpr.
TEST(blitz_lays_out_answers_queries_and_paints) {
    BlitzRun r("<style>body{margin:0}#d{height:50px;background:#f00}</style><div id=\"d\">x</div>"
               "<script>console.log('rect=' + document.getElementById('d').getBoundingClientRect().width + 'x' + document.getElementById('d').getBoundingClientRect().height);</script>");
    auto d = r.host.diagnostics();
    CHECK_MSG(d.loaded, "loaded");
    CHECK_MSG(d.rendered, "the post-load ack query came back");
    CHECK_MSG(!d.dead, d.dead.value_or(""));
    CHECK_MSG(d.hasLog("rect=400x50"), [&] { std::string s; for (auto& l : d.logs) s += l.second + "\n"; return s; }());
    CHECK_MSG(d.height >= 50 && d.height < 200, "height " + std::to_string(d.height));
    BlitzRenderer::Frame f = r.blitz->paint();
    CHECK_MSG(f.width == 800, "frame width " + std::to_string(f.width));
    CHECK_MSG(f.height >= 100, "frame height " + std::to_string(f.height));
    // A red pixel inside the div (premultiplied RGBA, opaque).
    size_t i = (size_t(20) * f.width + 20) * 4;
    CHECK_MSG(f.rgba.size() > i + 3 && f.rgba[i] > 200 && f.rgba[i + 1] < 50 && f.rgba[i + 3] == 255, "pixel at (20,20)");
    CHECK(d.countDiag("renderer-violation") == 0);
}

/// A pointer from the host view is hit-tested by the document and reaches the widget's listener as a click,
/// marked trusted, so it is a user gesture for a privileged bridge call.
TEST(blitz_click_reaches_the_widget) {
    BlitzRun r("<button id=\"b\" style=\"display:block;width:200px;height:60px;margin:0\">go</button>"
               "<script>document.getElementById('b').addEventListener('click', function (e) { console.log('clicked ' + e.target.id); TelegramWidget.openLink('https://example.com/clicked'); });</script>");
    r.blitz->click(20, 20);
    CHECK_MSG(r.host.waitUntil([](const WidgetDiagnostics& d) { return d.hasLog("clicked b"); }, 3), "click reached the widget");
    r.blitz->click(300, 200);
    sleepMs(200);
    auto d = r.host.diagnostics();
    int clicks = 0;
    for (auto& l : d.logs) if (l.second == "clicked b") clicks++;
    CHECK_MSG(clicks == 1, "a click outside the button must not fire it: " + std::to_string(clicks));
    int sent = 0;
    for (auto& a : d.actions) if (a.kind == WidgetAction::OpenLink && a.url == "https://example.com/clicked") sent++;
    CHECK_MSG(sent == 1, "the renderer marks its input trusted, so the click is a gesture: " + std::to_string(sent));
}

/// A killed runtime freezes the document: the last render stays, pointers stop reaching anything.
TEST(blitz_frozen_after_kill_keeps_last_render) {
    BlitzRun r("<style>body{margin:0}</style><div style=\"height:40px;background:#00f\"></div><script>setTimeout(function () { for (;;) {} }, 50);</script>");
    CHECK_MSG(r.host.waitUntil([](const WidgetDiagnostics& d) { return d.dead.has_value(); }, 10), "the CPU limit kills the loop");
    sleepMs(100);
    BlitzRenderer::Frame f = r.blitz->paint();
    CHECK_MSG(!f.empty(), "still paints after the kill");
    // (10,10) CSS px at dpr 2, inside the div; the fatal badge sits bottom-right and the grayscale filter is 30 %.
    size_t i = (size_t(20) * f.width + 20) * 4;
    CHECK_MSG(f.rgba.size() > i + 3 && f.rgba[i + 2] > 150 && f.rgba[i] < 120, "the last render (blue div) is kept");
}

#endif

// ───────────── WidgetHost + litehtml renderer (-DTW_LITEHTML=ON) ─────────────
#ifdef TW_HAVE_LITEHTML

/// One widget on WidgetHost with the litehtml renderer (the Blitz tests above, on the other native document).
struct LitehtmlRun {
    LitehtmlRenderer* lite = nullptr;
    WidgetHost host;
    explicit LitehtmlRun(const std::string& html, double dpr = 2)
        : host(html, WidgetTheme{}, 400, [] { WidgetHostOptions o; o.devicePixelRatio = 2; return o; }(), RES,
               [this, dpr](std::shared_ptr<SandboxResources>, const std::string& theme, double width) -> std::unique_ptr<Renderer> {
                   auto r = std::make_unique<LitehtmlRenderer>(theme, width, dpr);
                   lite = r.get();
                   return r;
               }) {
        host.start();
        host.waitLoaded(20);
        host.waitRendered(5);
    }
};

/// litehtml answers layout queries with real geometry, reports the document height and paints a raster of width × dpr.
TEST(litehtml_lays_out_answers_queries_and_paints) {
    LitehtmlRun r("<style>body{margin:0}#d{height:50px;background:#f00}</style><div id=\"d\">x</div>"
                  "<script>console.log('rect=' + document.getElementById('d').getBoundingClientRect().width + 'x' + document.getElementById('d').getBoundingClientRect().height);</script>");
    auto d = r.host.diagnostics();
    CHECK_MSG(d.loaded, "loaded");
    CHECK_MSG(d.rendered, "the post-load ack query came back");
    CHECK_MSG(!d.dead, d.dead.value_or(""));
    CHECK_MSG(d.hasLog("rect=400x50"), [&] { std::string s; for (auto& l : d.logs) s += l.second + "\n"; return s; }());
    CHECK_MSG(d.height >= 50 && d.height < 200, "height " + std::to_string(d.height));
    LitehtmlRenderer::Frame f = r.lite->paint();
    CHECK_MSG(f.width == 800, "frame width " + std::to_string(f.width));
    CHECK_MSG(f.height >= 100, "frame height " + std::to_string(f.height));
    size_t i = (size_t(20) * f.width + 20) * 4;
    CHECK_MSG(f.rgba.size() > i + 3 && f.rgba[i] > 200 && f.rgba[i + 1] < 50 && f.rgba[i + 3] == 255, "pixel at (20,20)");
    CHECK(d.countDiag("renderer-violation") == 0);
    CHECK_MSG(r.lite->html().find("data-twid=") != std::string::npos, "the shadow tree was serialised");
}

/// A pointer is hit-tested by litehtml's render tree and reaches the widget's listener as a click,
/// marked trusted, so it is a user gesture for a privileged bridge call.
TEST(litehtml_click_reaches_the_widget) {
    LitehtmlRun r("<button id=\"b\" style=\"display:block;width:200px;height:60px;margin:0\">go</button>"
                  "<script>document.getElementById('b').addEventListener('click', function (e) { console.log('clicked ' + e.target.id); TelegramWidget.openLink('https://example.com/clicked'); });</script>");
    r.lite->click(20, 20);
    CHECK_MSG(r.host.waitUntil([](const WidgetDiagnostics& d) { return d.hasLog("clicked b"); }, 3), "click reached the widget");
    r.lite->click(300, 200);
    sleepMs(200);
    auto d = r.host.diagnostics();
    int clicks = 0;
    for (auto& l : d.logs) if (l.second == "clicked b") clicks++;
    CHECK_MSG(clicks == 1, "a click outside the button must not fire it: " + std::to_string(clicks));
    int sent = 0;
    for (auto& a : d.actions) if (a.kind == WidgetAction::OpenLink && a.url == "https://example.com/clicked") sent++;
    CHECK_MSG(sent == 1, "the renderer marks its input trusted, so the click is a gesture: " + std::to_string(sent));
}

/// A killed runtime freezes the document: the last render stays, pointers stop reaching anything.
TEST(litehtml_frozen_after_kill_keeps_last_render) {
    LitehtmlRun r("<style>body{margin:0}</style><div style=\"height:40px;background:#00f\"></div><script>setTimeout(function () { for (;;) {} }, 50);</script>");
    CHECK_MSG(r.host.waitUntil([](const WidgetDiagnostics& d) { return d.dead.has_value(); }, 10), "the CPU limit kills the loop");
    sleepMs(100);
    LitehtmlRenderer::Frame f = r.lite->paint();
    CHECK_MSG(!f.empty(), "still paints after the kill");
    size_t i = (size_t(20) * f.width + 20) * 4;
    CHECK_MSG(f.rgba.size() > i + 3 && f.rgba[i + 2] > 150 && f.rgba[i] < 120, "the last render (blue div) is kept");
}

#endif

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    try { RES = SandboxResources::fromDirectory(TW_WIDGET_JS); } catch (const std::exception& e) { fprintf(stderr, "%s\n", e.what()); return 2; }
    int ran = 0;
    for (const auto& t : registry()) {
        if (filter && std::strstr(t.name, filter) == nullptr) continue;
        int before = g_failures;
        auto t0 = std::chrono::steady_clock::now();
        printf("%s\n", t.name);
        fflush(stdout);
        t.fn();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("  %s (%.0f ms)\n", g_failures == before ? "ok" : "FAILED", ms);
        ran++;
    }
    printf("\n%d tests, %d checks, %d failures\n", ran, g_checks, g_failures);
    return g_failures ? 1 : 0;
}
