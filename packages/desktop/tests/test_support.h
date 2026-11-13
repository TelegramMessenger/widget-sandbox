// What test_main.cpp (the runner and the suite) and test_remote.cpp (the remote engine's tests) share: the registry, the checks and the helpers both use.
#pragma once

#include "twidget/json.h"
#include "twidget/renderer.h"
#include "twidget/resources.h"
#include "twidget/sandbox_engine.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace twidget;

extern int g_failures;
extern int g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_MSG(cond, msg) do { g_checks++; if (!(cond)) { g_failures++; fprintf(stderr, "  FAIL %s:%d: %s — %s\n", __FILE__, __LINE__, #cond, std::string(msg).c_str()); } } while (0)

struct TestCase { const char* name; void (*fn)(); };
/// The suite in registration order: each file's tests in that file's order, the files as they are linked (test_main.cpp first).
std::vector<TestCase>& registry();
#define TEST(name) static void name(); static const bool name##_registered = (registry().push_back({#name, name}), true); static void name()
/// Cases that run only alone in a child process (`twidget-tests --child <name>`), never in the suite: what a test watches end a process.
std::vector<TestCase>& childCases();
#define CHILD_CASE(name) static void name(); static const bool name##_child = (childCases().push_back({#name, name}), true); static void name()
/// This executable as it was started (`argv[0]`), for a test that runs a child case.
extern std::string g_self;

extern std::shared_ptr<SandboxResources> RES;

// test_main.cpp's helpers that the remote engine's tests use too.
std::string readFile(const std::string& p);
void sleepMs(int ms);
SandboxConfig testConfig();
bool loopbackSwitch();
Json expandRepeat(const Json& v);
extern const char* kRequester;
extern const std::string kTapOnButton;
WidgetRequestOutcome answered(std::optional<std::string> result);
WidgetContext messageContext(bool isCopy, bool canQuery, bool backgroundQuery, std::string chatType = "", std::optional<WidgetViewer> viewer = std::nullopt);
bool eventually(const std::function<bool()>& pred, double timeoutSeconds = 5);

/// A renderer without a document: announces itself, answers every query with `queryResult` (null), keeps what it was sent; tests inject its events.
struct ScriptedRenderer final : Renderer {
    void load() override { onMessage("{\"type\":\"ready\"}"); }
    void send(const std::string& json) override {
        {
            std::lock_guard<std::mutex> l(mutex);
            messages.push_back(json);
        }
        auto m = Json::parse(json);
        if (m && (*m)["type"].asString() == "query") onMessage("{\"type\":\"result\",\"qid\":" + std::to_string((*m)["qid"].asInt()) + ",\"result\":" + queryResult + "}");
    }
    /// Set before the host starts.
    std::string queryResult = "null";
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

RendererFactory scripted(ScriptedRenderer*& out);

/// Polls a probe until it gives `expected` (an answer may come from another thread); returns the last value.
template <typename Engine>
std::string probeUntil(Engine& engine, const std::string& code, const std::string& expected, double timeoutSeconds = 5) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(timeoutSeconds * 1000));
    std::string got = engine.probe(code);
    while (got != expected && std::chrono::steady_clock::now() < deadline) {
        sleepMs(10);
        got = engine.probe(code);
    }
    return got;
}
