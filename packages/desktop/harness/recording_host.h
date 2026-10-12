// A fake host for the harness and the tests: records everything the engine
// emits, runs every ops batch through the TrustedValidator (so what it keeps is
// exactly what a renderer would receive) and answers layout queries with the
// same canned values as scripts/sandbox-node.ts.
#pragma once

#include "twidget/json.h"
#include "twidget/sandbox_engine.h"
#include "twidget/trusted_validator.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace twidget {

class RecordingHost : public SandboxEngineDelegate {
public:
    RecordingHost(const std::string& theme, std::shared_ptr<SandboxResources> resources, bool verbose = false)
        : verbose_(verbose), validator_(theme, std::move(resources), [this](const std::string& k, const std::string& d) { diag("validator:" + k, d); }) {}

    // Recorded state (read under lock()).
    std::vector<std::string> ops;        // raw batches from the sandbox (untrusted)
    std::vector<std::string> renderOps;  // validated RenderOps per batch (JSON arrays)
    std::vector<std::pair<std::string, std::string>> logs;
    std::vector<std::pair<std::string, std::string>> diags;
    std::vector<std::string> queries;
    std::vector<std::pair<std::string, std::string>> bridge;
    std::optional<std::string> died;
    int frameRequests = 0;
    bool loaded = false;
    SandboxStats stats;
    /// Custom query answers (clean query → answer JSON); nullopt falls back to the canned ones.
    std::function<std::optional<std::string>(const Json&)> answer;

    std::unique_lock<std::mutex> lock() { return std::unique_lock<std::mutex>(mutex_); }
    TrustedValidator& validator() { return validator_; }

    /// True once `loaded` or `died` was reported.
    bool waitLoaded(double timeoutSeconds = 20) {
        std::unique_lock<std::mutex> l(mutex_);
        return cv_.wait_for(l, std::chrono::duration<double>(timeoutSeconds), [&] { return loaded || died.has_value(); });
    }

    /// Polls `pred` (called under the lock) until true or timeout.
    bool waitUntil(const std::function<bool()>& pred, double timeoutSeconds = 3) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(timeoutSeconds));
        for (;;) {
            { std::lock_guard<std::mutex> l(mutex_); if (pred()) return true; }
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    bool hasLog(const std::string& text) const { for (auto& l : logs) if (l.second == text) return true; return false; }
    bool hasLogPrefix(const std::string& prefix) const { for (auto& l : logs) if (l.second.rfind(prefix, 0) == 0) return true; return false; }
    int countLogPrefix(const std::string& prefix) const { int n = 0; for (auto& l : logs) if (l.second.rfind(prefix, 0) == 0) n++; return n; }
    int countDiag(const std::string& kind) const { int n = 0; for (auto& d : diags) if (d.first == kind) n++; return n; }
    bool hasDiag(const std::string& kind) const { return countDiag(kind) > 0; }
    std::string allOps() const { std::string s; for (auto& o : ops) s += o; return s; }
    std::string allRenderOps() const { std::string s; for (auto& o : renderOps) s += o; return s; }

    // SandboxEngineDelegate (engine thread)
    void onOps(SandboxEngine&, const std::string& json) override {
        std::string clean = validator_.processOps(json);
        std::lock_guard<std::mutex> l(mutex_);
        ops.push_back(json);
        renderOps.push_back(clean);
        if (verbose_) fprintf(stderr, "ops → %s\n", clean.size() > 2000 ? (clean.substr(0, 2000) + "…").c_str() : clean.c_str());
    }
    QueryAnswer onQuery(SandboxEngine&, const std::string& json) override {
        { std::lock_guard<std::mutex> l(mutex_); queries.push_back(json); }
        auto clean = validator_.validateQuery(json);
        if (!clean) return {"{\"error\":\"invalid query\"}", std::nullopt};
        auto q = Json::parse(*clean);
        if (!q) return {"{\"error\":\"invalid query\"}", std::nullopt};
        std::optional<std::string> custom = answer ? answer(*q) : std::nullopt;
        std::string out = custom ? *custom : cannedAnswer(*q);
        if (verbose_) fprintf(stderr, "query %s → %s\n", clean->c_str(), out.c_str());
        return {out, std::nullopt};
    }
    void onListen(SandboxEngine&, const std::string& json) override { (void)validator_.validateListen(json); }
    void onPreventDefault(SandboxEngine&, const std::string& json) override { (void)validator_.validatePreventDefault(json); }
    void onRequestFrame(SandboxEngine&) override { std::lock_guard<std::mutex> l(mutex_); frameRequests++; }
    void onLog(SandboxEngine&, const std::string& level, const std::string& text) override {
        std::lock_guard<std::mutex> l(mutex_);
        logs.emplace_back(level, text);
        if (verbose_) fprintf(stderr, "[%s] %s\n", level.c_str(), text.c_str());
    }
    void onDiag(SandboxEngine&, const std::string& kind, const std::string& detail) override { diag(kind, detail); }
    void onBridge(SandboxEngine&, const std::string& method, const std::string& argsJson) override {
        std::lock_guard<std::mutex> l(mutex_);
        bridge.emplace_back(method, argsJson);
        if (verbose_) fprintf(stderr, "[bridge] %s %s\n", method.c_str(), argsJson.c_str());
    }
    void onLoaded(SandboxEngine&) override { { std::lock_guard<std::mutex> l(mutex_); loaded = true; } cv_.notify_all(); }
    void onStats(SandboxEngine&, const SandboxStats& s) override { std::lock_guard<std::mutex> l(mutex_); stats = s; }
    void onDied(SandboxEngine&, const std::string& reason) override {
        { std::lock_guard<std::mutex> l(mutex_); died = reason; if (verbose_) fprintf(stderr, "[dead] %s\n", reason.c_str()); }
        cv_.notify_all();
    }

    static std::string cannedAnswer(const Json& q) {
        const std::string& kind = q["q"].asString();
        if (kind == "rect") return "{\"x\":0,\"y\":0,\"width\":100,\"height\":20,\"top\":0,\"left\":0,\"right\":100,\"bottom\":20}";
        if (kind == "rects") return "[{\"x\":0,\"y\":0,\"width\":100,\"height\":20,\"top\":0,\"left\":0,\"right\":100,\"bottom\":20}]";
        if (kind == "box") return "{\"offsetWidth\":100,\"offsetHeight\":20,\"offsetTop\":0,\"offsetLeft\":0,\"offsetParent\":0,\"clientWidth\":100,\"clientHeight\":20,\"clientTop\":0,\"clientLeft\":0,\"scrollWidth\":100,\"scrollHeight\":20,\"scrollTop\":0,\"scrollLeft\":0}";
        if (kind == "cs") return q["prop"].isString() ? "\"\"" : "{\"display\":\"block\",\"width\":\"100px\",\"height\":\"20px\",\"font-size\":\"16px\"}";
        if (kind == "svg") {
            const std::string& m = q["method"].asString();
            if (m == "getBBox") return "{\"x\":0,\"y\":0,\"width\":50,\"height\":12}";
            if (m.find("CTM") != std::string::npos) return "{\"a\":1,\"b\":0,\"c\":0,\"d\":1,\"e\":0,\"f\":0}";
            if (m == "getPointAtLength") return "{\"x\":0,\"y\":0}";
            return "42";
        }
        if (kind == "measureText") return "{\"width\":" + std::to_string(q["text"].asString().size() * 6) + ",\"actualBoundingBoxAscent\":8,\"actualBoundingBoxDescent\":2}";
        if (kind == "matchMedia") return "{\"matches\":false}";
        if (kind == "viewport") return "{\"innerWidth\":400,\"innerHeight\":300,\"dpr\":1}";
        if (kind == "validity") return "{\"valid\":true,\"message\":\"\"}";
        if (kind == "elementFromPoint") return "0";
        if (kind == "canvasCall") return q["method"].asString() == "toDataURL" ? "\"data:,\"" : "false";
        if (kind == "imageData") return "{\"width\":0,\"height\":0}";
        return "null";
    }

private:
    void diag(const std::string& kind, const std::string& detail) {
        std::lock_guard<std::mutex> l(mutex_);
        diags.emplace_back(kind, detail);
        if (verbose_) fprintf(stderr, "[diag] %s — %s\n", kind.c_str(), detail.c_str());
    }

    bool verbose_;
    std::mutex mutex_;
    std::condition_variable cv_;
    TrustedValidator validator_;
};

} // namespace twidget
