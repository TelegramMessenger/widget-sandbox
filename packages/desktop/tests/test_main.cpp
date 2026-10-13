// Tests for the desktop core: the XCTest suite of ios/Tests/WidgetSandboxTests
// ported one for one (engine boot, network stubs, CPU / memory kills, layout
// query round trip, pre-boot buffering, timer cap, CPU quota, off-screen pause,
// bridge, pinned-thread recursion, validator diagnostics and sanitising,
// create/teardown churn) plus the shared CpuBudget fixture and the JSON helper.
// No test framework dependency: `twidget-tests [filter]`, exit code = failures.

#include "../harness/recording_host.h"
#include "twidget/cpu_budget.h"
#include "twidget/json.h"
#ifdef TW_HAVE_BLITZ
#include "twidget/blitz_renderer.h"
#endif
#ifdef TW_HAVE_LITEHTML
#include "twidget/litehtml_renderer.h"
#endif
#if defined(TW_HAVE_BLITZ) || defined(TW_HAVE_LITEHTML)
#include "twidget/widget_host.h"
#endif

#include <chrono>
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
    CHECK(Json::quote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
    Json o = Json::object();
    o.set("k", "v").set("n", 2);
    CHECK(o.dump() == "{\"k\":\"v\",\"n\":2}");
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
            } else {
                CHECK_MSG(false, where + ": unknown step");
            }
        }
    }
}

static const char* kDepthProbe = "(function(){ var n = 0; function r(){ n++; r(); } try { r(); } catch (e) { if (!(e instanceof RangeError)) throw e; } return n; })()";

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

/// `ready` needs no gesture; a privileged call from a timer is refused and reported,
/// the same call from the handler of a trusted click goes through once.
TEST(bridge_and_timers) {
    Run r("<button id='b'>go</button><script>TelegramWidget.ready(); setTimeout(() => TelegramWidget.sendMessage('later'), 30);"
          "document.getElementById('b').addEventListener('click', () => { TelegramWidget.sendMessage('clicked'); TelegramWidget.sendMessage('again'); });</script>");
    r.host.waitUntil([&] { for (auto& d : r.host.diags) if (d.first == "activation-blocked") return true; return false; }, 3);
    {
        auto l = r.host.lock();
        bool ready = false, sent = false;
        for (auto& b : r.host.bridge) { if (b.first == "ready") ready = true; if (b.first == "sendMessage") sent = true; }
        CHECK(ready);
        CHECK(!sent);
    }
    r.engine.event("{\"type\":\"click\",\"targetId\":5}");
    r.barrier();
    r.engine.activationEvent("{\"type\":\"click\",\"targetId\":5}", 1);
    r.host.waitUntil([&] { for (auto& b : r.host.bridge) if (b.first == "sendMessage") return true; return false; }, 3);
    r.barrier();
    r.engine.activationEvent("{\"type\":\"click\",\"targetId\":5}", 1);
    r.barrier();
    auto l = r.host.lock();
    std::vector<std::string> sent;
    for (auto& b : r.host.bridge) if (b.first == "sendMessage") sent.push_back(b.second);
    CHECK_MSG(sent.size() == 1 && sent[0].find("clicked") != std::string::npos, std::to_string(sent.size()) + " sendMessage calls");
}

#if defined(TW_HAVE_BLITZ) || defined(TW_HAVE_LITEHTML)
TEST(user_gestures_are_numbered_per_physical_press) {
    auto ev = [](const char* json) { return *Json::parse(json); };
    CHECK(WidgetHost::isUserActivation(ev("{\"type\":\"click\"}")));
    CHECK(WidgetHost::isUserActivation(ev("{\"type\":\"keydown\",\"key\":\"a\"}")));
    CHECK(!WidgetHost::isUserActivation(ev("{\"type\":\"keydown\",\"key\":\"Escape\"}")));
    CHECK(!WidgetHost::isUserActivation(ev("{\"type\":\"keydown\",\"key\":\"a\",\"repeat\":true}")));
    CHECK(!WidgetHost::isUserActivation(ev("{\"type\":\"keydown\",\"key\":\"c\",\"modifiers\":4}")));
    CHECK(!WidgetHost::isUserActivation(ev("{\"type\":\"mousemove\"}")));
    CHECK(!WidgetHost::isUserActivation(ev("{\"type\":\"scroll\"}")));
}
#endif

TEST(widget_code_cannot_reach_host_globals) {
    Run r("<script>var names=['__host','__hostBinary','__twDispatch'];console.log('globals '+names.map(function(n){return n+'='+typeof globalThis[n]+'/'+(n in globalThis);}).join(','));"
          "try{Object.defineProperty(globalThis,'__twDispatch',{value:function(){console.log('pwned');}});}catch(e){}setTimeout(function(){console.log('tick');},1);</script>");
    r.host.waitUntil([&] { return r.host.countLogPrefix("tick") > 0; }, 3);
    r.barrier();
    auto l = r.host.lock();
    CHECK_MSG(r.host.countLogPrefix("globals __host=undefined/false,__hostBinary=undefined/false,__twDispatch=undefined/false") == 1, r.logsText());
    CHECK(r.host.countLogPrefix("tick") == 1);
    CHECK(r.host.countLogPrefix("pwned") == 0);
}

TEST(intl_fallback_formats_through_the_host) {
    Run r("<script>console.log((1234.5).toLocaleString() + '|' + new Intl.PluralRules('en').select(1) + '|' + new Intl.NumberFormat('en', {style:'percent'}).format(0.25));</script>");
    r.barrier();
    auto l = r.host.lock();
    CHECK_MSG(r.host.hasLog("1,234.5|one|25%"), r.logsText());
}

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

TEST(shipped_bytecode_is_used_for_bundles_and_libraries) {
    CHECK_MSG(!RES->sandboxBytecode.empty() && !RES->validatorBytecode.empty(), "pnpm build:desktop ships .qbc next to the bundles");
    Run r("<canvas id=\"c\"></canvas><script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script><script>console.log(typeof Chart);</script>");
    r.barrier();
    auto l = r.host.lock();
    CHECK(!r.host.died);
    bool bootFromBytecode = false, libFromBytecode = false, fallback = false;
    for (auto& d : r.host.diags) {
        if (d.first == "boot" && d.second.find("(bytecode)") != std::string::npos) bootFromBytecode = true;
        if (d.first == "library" && d.second.find("bytecode") != std::string::npos) libFromBytecode = true;
        if (d.first == "bytecode") fallback = true;
    }
    CHECK_MSG(bootFromBytecode && libFromBytecode && !fallback, r.diagsText());
    CHECK(r.host.hasLog("function"));
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
        : host(html, "light", 400, [] { WidgetHostOptions o; o.devicePixelRatio = 2; return o; }(), RES,
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

/// A pointer from the host view is hit-tested by the document and reaches the widget's listener as a click.
TEST(blitz_click_reaches_the_widget) {
    BlitzRun r("<button id=\"b\" style=\"display:block;width:200px;height:60px;margin:0\">go</button>"
               "<script>document.getElementById('b').addEventListener('click', function (e) { console.log('clicked ' + e.target.id); });</script>");
    r.blitz->click(20, 20);
    CHECK_MSG(r.host.waitUntil([](const WidgetDiagnostics& d) { return d.hasLog("clicked b"); }, 3), "click reached the widget");
    r.blitz->click(300, 200);
    sleepMs(200);
    auto d = r.host.diagnostics();
    int clicks = 0;
    for (auto& l : d.logs) if (l.second == "clicked b") clicks++;
    CHECK_MSG(clicks == 1, "a click outside the button must not fire it: " + std::to_string(clicks));
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
        : host(html, "light", 400, [] { WidgetHostOptions o; o.devicePixelRatio = 2; return o; }(), RES,
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

/// A pointer is hit-tested by litehtml's render tree and reaches the widget's listener as a click.
TEST(litehtml_click_reaches_the_widget) {
    LitehtmlRun r("<button id=\"b\" style=\"display:block;width:200px;height:60px;margin:0\">go</button>"
                  "<script>document.getElementById('b').addEventListener('click', function (e) { console.log('clicked ' + e.target.id); });</script>");
    r.lite->click(20, 20);
    CHECK_MSG(r.host.waitUntil([](const WidgetDiagnostics& d) { return d.hasLog("clicked b"); }, 3), "click reached the widget");
    r.lite->click(300, 200);
    sleepMs(200);
    auto d = r.host.diagnostics();
    int clicks = 0;
    for (auto& l : d.logs) if (l.second == "clicked b") clicks++;
    CHECK_MSG(clicks == 1, "a click outside the button must not fire it: " + std::to_string(clicks));
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
