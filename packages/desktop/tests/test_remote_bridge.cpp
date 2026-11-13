// The remote engine's tests of the bridge calls a WidgetHost records from it (`diagnostics().bridge`, the deprecated `onBridgeCall`), in the suite after test_remote.cpp's: `twidget-tests remote_`.
#include "test_support.h"
#include "test_remote_support.h"

#include "twidget/json.h"
#include "twidget/remote_engine.h"
#include "twidget/sdk_policy.h"
#include "twidget/widget_host.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/// The longest link the SDK takes: `sdk::MAX_LINK_LENGTH` bytes to a Telegram host.
static const std::string kLongestLink = "https://t.me/" + std::string(size_t(sdk::MAX_LINK_LENGTH) - 13, 'a');
/// The most `diagnostics().bridge` keeps of one call, method and arguments: the longest method with the longest link in its array.
static const size_t kBridgeEntryBound = std::string("openTelegramLink").size() + size_t(sdk::MAX_LINK_LENGTH) + 4;
static const std::string kReadyAction = "action:{\"kind\":\"ready\"}";

/// What a host's `onAction` and deprecated `onBridgeCall` got, in order, under a lock (both run on the host thread); `observe` sets them before the host starts.
struct HeardBridge {
    void observe(WidgetHost& host) {
        host.onAction = [this](const WidgetAction& action) {
            std::lock_guard<std::mutex> l(mutex_);
            actions_.push_back("action:" + action.toJson().dump());
        };
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        host.onBridgeCall = [this](const std::string& method, const std::string& args) {
            std::lock_guard<std::mutex> l(mutex_);
            calls_.emplace_back(method, args);
        };
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
    }
    std::vector<std::string> actions() {
        std::lock_guard<std::mutex> l(mutex_);
        return actions_;
    }
    std::vector<std::pair<std::string, std::string>> calls() {
        std::lock_guard<std::mutex> l(mutex_);
        return calls_;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> actions_;
    std::vector<std::pair<std::string, std::string>> calls_;
};

/// Recorded actions as `HeardBridge` writes them.
static std::vector<std::string> actionEvents(const std::vector<WidgetAction>& actions) {
    std::vector<std::string> out;
    for (const WidgetAction& action : actions) out.push_back("action:" + action.toJson().dump());
    return out;
}

/// Bridge calls as `method:args`, for comparing them whole and for `firstDifference`, which clips what it prints.
static std::vector<std::string> bridgeEntries(const std::vector<std::pair<std::string, std::string>>& calls) {
    std::vector<std::string> out;
    for (const auto& [method, args] : calls) out.push_back(method + ":" + args);
    return out;
}

/// The largest and the total of `calls`' method and argument bytes, as the documented bound counts them.
static std::pair<size_t, size_t> bridgeBytes(const std::vector<std::pair<std::string, std::string>>& calls) {
    size_t most = 0, total = 0;
    for (const auto& [method, args] : calls) {
        most = std::max(most, method.size() + args.size());
        total += method.size() + args.size();
    }
    return {most, total};
}

/// A hostile engine's side pads valid bridge calls with whitespace up to the message limit, through a real WidgetHost: each takes effect as the same call unpadded, and what the host records and hands the deprecated callback is its checked arguments re-serialized. While the host is held, a call's own place still counts what came, the host's copy beside its action only the checked `[]`. Every entry stays within 276 bytes of method and arguments (the longest link reaches them), the first 1000 of 1024 calls within 276 000, and past them every action and callback still runs; a ready a byte past the limit and a padded link with every tap spent are still refused, and nothing dies.
TEST(remote_widget_host_keeps_padded_bridge_calls_to_their_checked_arguments) {
    using Diags = std::map<std::string, std::pair<int, std::vector<std::string>>>;
    HeardBridge heard;
    RemoteHostRun r([&](WidgetHost& host) { heard.observe(host); });
    WidgetHost& host = *r.host;
    SandboxRemoteSink& sink = r.sink();
    const size_t limit = SandboxEngine::kRemoteMessageLimit, recorded = 1000, accepted = 1024;
    const std::string okLink = "https://example.com/ok";
    // Built before the taps, whose grants last 2 s; the padded ready has all four kinds of JSON whitespace.
    const std::string ready = paddedTo("[ \n\t\r]", limit), impact = paddedTo("[\"impact\",\"soft\"]", limit), link = paddedTo(linkArgs(okLink), limit);
    const std::string longest = paddedTo(linkArgs(kLongestLink), limit), overLimit = paddedTo("[]", limit + 1), volume = paddedTo("[]", 1024);
    CHECK(kBridgeEntryBound == 276 && kLongestLink.size() == size_t(sdk::MAX_LINK_LENGTH) && ready.size() == limit && longest.size() == limit);
    {
        HeldHost held(host);
        CHECK(held.held());
        sink.remoteBridge("ready", "[]");
        sink.remoteBridge("ready", ready);
        // The dispatch thread only: both calls ran there, and the host work they queued waits with each call's own place.
        CHECK(host.probe("0") == r.channel->probeAnswer);
        CHECK_MSG(backlogIs(host, (5 + 2) + 7 + (5 + limit) + 7, 4), backlogText(host));
        CHECK(held.release());
    }
    settleHost(host);
    CHECK_MSG(backlogIs(host, 0, 0), backlogText(host));
    pressThrough(r);
    sink.remoteBridge("haptic", "[\"impact\",\"soft\"]");
    sink.remoteBridge("haptic", impact);
    tapThrough(r);
    sink.remoteBridge("openLink", linkArgs(okLink));
    tapThrough(r);
    sink.remoteBridge("openLink", link);
    tapThrough(r);
    sink.remoteBridge("openTelegramLink", longest);
    sink.remoteBridge("ready", overLimit);
    sink.remoteBridge("openLink", link);
    settleHost(host);
    const std::string impactAction = "action:{\"kind\":\"haptic\",\"haptic\":\"impact\",\"value\":\"soft\"}";
    const std::vector<std::string> checkedActions = {kReadyAction, kReadyAction, impactAction, impactAction, linkAction("openLink", okLink), linkAction("openLink", okLink),
                                                     linkAction("openTelegramLink", kLongestLink)};
    const std::vector<std::string> checkedCalls = {"ready:[]", "ready:[]", "haptic:[\"impact\",\"soft\"]", "haptic:[\"impact\",\"soft\"]", "openLink:[\"https://example.com/ok\"]",
                                                   "openLink:[\"https://example.com/ok\"]", "openTelegramLink:[\"" + kLongestLink + "\"]"};
    const Diags controls = {{"activation-blocked", {1, {"openLink ignored by the host: no tap on the visible widget allows it"}}},
                            {"bridge-rejected", {1, {"ready: rejected by the host (unknown method or malformed arguments)"}}}};
    {
        const WidgetDiagnostics d = host.diagnostics();
        const std::vector<std::string> kept = bridgeEntries(d.bridge), heardCalls = bridgeEntries(heard.calls()), keptActions = actionEvents(d.actions), heardActions = heard.actions();
        CHECK_MSG(kept == checkedCalls && heardCalls == checkedCalls, firstDifference(kept, checkedCalls) + "; heard " + firstDifference(heardCalls, checkedCalls));
        CHECK_MSG(keptActions == checkedActions && heardActions == checkedActions, firstDifference(keptActions, checkedActions) + "; heard " + firstDifference(heardActions, checkedActions));
        const auto [most, total] = bridgeBytes(d.bridge);
        const size_t last = d.bridge.empty() ? 0 : d.bridge.back().first.size() + d.bridge.back().second.size();
        CHECK_MSG(last == kBridgeEntryBound && most == kBridgeEntryBound, std::to_string(last) + " bytes last, " + std::to_string(most) + " at most, " + std::to_string(total) + " in all");
        CHECK_MSG(d.diags == controls && !d.dead, std::to_string(d.diags.size()) + " kinds, " + d.dead.value_or("alive"));
    }
    for (size_t i = checkedCalls.size(); i < accepted; i++) {
        sink.remoteBridge("ready", volume);
        if (i % 32 == 31) settleHost(host);
    }
    settleHost(host);
    std::vector<std::string> allCalls = checkedCalls, allActions = checkedActions;
    allCalls.resize(accepted, "ready:[]");
    allActions.resize(accepted, kReadyAction);
    const std::vector<std::string> firstCalls(allCalls.begin(), allCalls.begin() + std::ptrdiff_t(recorded)), firstActions(allActions.begin(), allActions.begin() + std::ptrdiff_t(recorded));
    const WidgetDiagnostics d = host.diagnostics();
    const auto [most, total] = bridgeBytes(d.bridge);
    CHECK_MSG(most <= kBridgeEntryBound && total <= recorded * kBridgeEntryBound, std::to_string(most) + " bytes at most, " + std::to_string(total) + " in all, in " + std::to_string(d.bridge.size()) + " entries");
    CHECK_MSG(d.bridge.size() == recorded && d.actions.size() == recorded, std::to_string(d.bridge.size()) + " entries, " + std::to_string(d.actions.size()) + " actions");
    const std::vector<std::string> kept = bridgeEntries(d.bridge), heardCalls = bridgeEntries(heard.calls()), keptActions = actionEvents(d.actions), heardActions = heard.actions();
    CHECK_MSG(kept == firstCalls && keptActions == firstActions, firstDifference(kept, firstCalls) + "; actions " + firstDifference(keptActions, firstActions));
    CHECK_MSG(heardCalls == allCalls && heardActions == allActions, firstDifference(heardCalls, allCalls) + "; actions " + firstDifference(heardActions, allActions));
    CHECK_MSG(d.diags == controls && !d.dead, std::to_string(d.diags.size()) + " kinds, " + d.dead.value_or("alive"));
    CHECK_MSG(backlogIs(host, 0, 0), backlogText(host));
    CHECK_MSG(r.channel->closes() == 0 && callsWith(*r.channel, "stop:").empty(), joined(callsWith(*r.channel, "stop:")));
}

/// Ordinary calls from a client-mode engine's side, through a real WidgetHost, are recorded and reach the deprecated callback with their method and arguments of the JSON value that was sent: byte for byte when they came compact, compact when they came spaced or escaped. A link keeps the URL as the widget gave it (an upper-case scheme and host, raw UTF-8) where its action has the normalised one, and the longest link is recorded at exactly 276 bytes.
TEST(remote_widget_host_records_ordinary_bridge_calls_as_they_were_sent) {
    struct Sent {
        std::string method, args, recorded, action;
    };
    const std::string cafe = "[\"HTTPS://Example.COM/caf\xC3\xA9" "?q=1#top\"]", longest = "[\"" + kLongestLink + "\"]";
    const std::vector<Sent> sent = {
        {"ready", "[]", "[]", kReadyAction},
        {"haptic", "[\"selection\"]", "[\"selection\"]", "action:{\"kind\":\"haptic\",\"haptic\":\"selection\"}"},
        {"haptic", "[\"notification\",\"warning\"]", "[\"notification\",\"warning\"]", "action:{\"kind\":\"haptic\",\"haptic\":\"notification\",\"value\":\"warning\"}"},
        {"haptic", "[ \"impact\" ,\t\"rigid\"\n]", "[\"impact\",\"rigid\"]", "action:{\"kind\":\"haptic\",\"haptic\":\"impact\",\"value\":\"rigid\"}"},
        {"openLink", cafe, cafe, linkAction("openLink", "https://example.com/caf%C3%A9?q=1#top")},
        {"openLink", "[\"https:\\/\\/example.com\\/esc\\u0061ped\"]", "[\"https://example.com/escaped\"]", linkAction("openLink", "https://example.com/escaped")},
        {"openTelegramLink", longest, longest, linkAction("openTelegramLink", kLongestLink)},
    };
    // The JSON values this surface has: arrays of strings.
    const auto sameStrings = [](const std::string& a, const std::string& b) {
        const std::optional<Json> x = Json::parse(a, Json::Surrogates::Keep), y = Json::parse(b, Json::Surrogates::Keep);
        if (!x || !y || !x->isArray() || !y->isArray() || x->size() != y->size()) return false;
        for (size_t i = 0; i < x->size(); i++)
            if (!(*x)[i].isString() || !(*y)[i].isString() || (*x)[i].asString() != (*y)[i].asString()) return false;
        return true;
    };
    HeardBridge heard;
    RemoteHostRun r([&](WidgetHost& host) { heard.observe(host); });
    WidgetHost& host = *r.host;
    pressThrough(r);
    for (const Sent& call : sent) {
        if (call.method != "ready" && call.method != "haptic") tapThrough(r);
        r.sink().remoteBridge(call.method, call.args);
    }
    settleHost(host);
    const WidgetDiagnostics d = host.diagnostics();
    CHECK_MSG(d.bridge.size() == sent.size(), std::to_string(d.bridge.size()) + " entries");
    std::vector<std::string> recorded, actions;
    for (size_t i = 0; i < sent.size(); i++) {
        recorded.push_back(sent[i].method + ":" + sent[i].recorded);
        actions.push_back(sent[i].action);
        if (i >= d.bridge.size()) continue;
        const auto& [method, args] = d.bridge[i];
        CHECK_MSG(method == sent[i].method && sameStrings(args, sent[i].args), sent[i].method + " " + std::to_string(i) + ": " + args.substr(0, 80));
    }
    const std::vector<std::string> kept = bridgeEntries(d.bridge), heardCalls = bridgeEntries(heard.calls()), keptActions = actionEvents(d.actions), heardActions = heard.actions();
    CHECK_MSG(kept == recorded && heardCalls == recorded, firstDifference(kept, recorded) + "; heard " + firstDifference(heardCalls, recorded));
    CHECK_MSG(keptActions == actions && heardActions == actions, firstDifference(keptActions, actions) + "; heard " + firstDifference(heardActions, actions));
    const size_t last = d.bridge.empty() ? 0 : d.bridge.back().first.size() + d.bridge.back().second.size();
    CHECK_MSG(last == kBridgeEntryBound, std::to_string(last) + " bytes");
    CHECK_MSG(d.diags.empty() && !d.dead, std::to_string(d.diags.size()) + " kinds, " + d.dead.value_or("alive"));
}
