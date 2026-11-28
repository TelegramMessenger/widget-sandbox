// `SandboxRemoteCoding` (twidget/remote_engine.h): the wire forms both processes write and read, their numbers bounded and their text parsed as the engine has it in process. A translation unit of its own, so a host's program (the client and the host's end of a stream) links the forms without the engine's server or the loopback.
#include "twidget/remote_engine.h"

#include "twidget/json.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

namespace twidget {

namespace {

/// 2^N for an N-bit integer type, the first number past its range and exact as a double (`double(max)` itself may round up to it).
template <class T>
constexpr double rangeEnd() { return double(std::numeric_limits<T>::max() / 2 + 1) * 2.0; }

/// A JSON number is a double: the wire's integers are exact up to 2^53.
constexpr double kExactIntegers = 9007199254740992.0;
constexpr double kStatsMax = 1e15;

/// A number truncated into an integer type that cannot be negative here: 0 below 0, the type's largest value past its range.
template <class T>
T truncatedTo(double v) {
    if (!(v > 0)) return T(0);
    return v >= rangeEnd<T>() ? std::numeric_limits<T>::max() : T(v);
}

/// Untrusted text: refused over the message limit before it is parsed; an escaped lone surrogate stays WTF-8, the bytes the engine has in process.
std::optional<Json> parse(std::string_view text) {
    if (text.size() > SandboxEngine::kRemoteMessageLimit) return std::nullopt;
    return Json::parse(text, Json::Surrogates::Keep, SandboxRemoteCoding::kMaxDepth);
}

std::optional<Json> parseObject(std::string_view text) {
    std::optional<Json> v = parse(text);
    if (!v || !v->isObject()) return std::nullopt;
    return v;
}

/// A member that is a finite number; with a repeated key the first, as `Json::find` reads every scalar member here.
std::optional<double> finiteNumber(const Json& object, std::string_view key) {
    const Json* v = object.find(key);
    if (!v || !v->isNumber() || !std::isfinite(v->asNumber())) return std::nullopt;
    return v->asNumber();
}

std::optional<std::string> stringOf(const Json& v) { return v.isString() ? std::optional<std::string>(v.asString()) : std::nullopt; }

Json orNull(const std::optional<std::string>& s) { return s ? Json(*s) : Json(); }

std::optional<std::vector<std::string>> stringList(const Json& v) {
    if (!v.isArray()) return std::nullopt;
    std::vector<std::string> out;
    for (const Json& s : v.arr()) {
        if (!s.isString()) return std::nullopt;
        out.push_back(s.asString());
    }
    return out;
}

/// An object of strings, or nullopt; a repeated key counts with its last value, as `sdk::checkStorageArea` reads a storage area.
std::optional<std::map<std::string, std::string>> stringMap(const Json& v) {
    if (!v.isObject()) return std::nullopt;
    std::map<std::string, const Json*> last;
    for (const auto& [k, s] : v.obj()) last[k] = &s;
    std::map<std::string, std::string> out;
    for (const auto& [k, s] : last) {
        if (!s->isString()) return std::nullopt;
        out.emplace(k, s->asString());
    }
    return out;
}

Json stringMapJson(const std::map<std::string, std::string>& values) {
    Json o = Json::object();
    for (const auto& [k, v] : values) o.set(k, v);
    return o;
}

Json dataJson(const WidgetData& data) {
    Json o = Json::object();
    o.set("chatData", orNull(data.chatData)).set("publicData", orNull(data.publicData));
    return o;
}

WidgetData dataOf(const Json& o) { return WidgetData{stringOf(o["chatData"]), stringOf(o["publicData"])}; }

WidgetContentSafeArea safeAreaOf(const Json& o) {
    auto side = [&](std::string_view key) { return finiteNumber(o, key).value_or(0); };
    return WidgetContentSafeArea{side("top"), side("right"), side("bottom"), side("left"), side("cornerLeft"), side("cornerRight")};
}

/// A finite number inside `int64_t`'s range, truncated toward zero; the range is checked exactly, [-2^63, 2^63).
std::optional<int64_t> int64Of(const Json& v) {
    constexpr double end = rangeEnd<int64_t>();
    const double n = v.asNumber();
    if (!v.isNumber() || !std::isfinite(n) || n < -end || n >= end) return std::nullopt;
    return int64_t(n);
}

Json contextJson(const WidgetContext& c) {
    Json o = Json::object();
    o.set("isCopy", c.isCopy).set("canQuery", c.canQuery).set("backgroundQuery", c.backgroundQuery).set("preview", c.preview).set("wallet", c.wallet);
    o.set("chatType", c.chatType);
    if (c.viewer) o.set("viewer", sdk::viewerJson(*c.viewer));
    return o;
}

/// `SandboxRemoteCoding::encodeTheme`'s object: the scheme and the palette always, the tokens as the renderer's theme message has them.
Json themeJson(const WidgetTheme& t) {
    Json o = Json::object();
    o.set("colorScheme", t.colorScheme).set("params", stringMapJson(t.params)).set("button", t.button.json()).set("text", t.text.json());
    return o;
}

/// `SandboxRemoteCoding::decodeTheme`'s reading of a parsed value.
std::optional<WidgetTheme> themeOf(const Json& o) {
    std::optional<std::string> scheme = o.isObject() ? stringOf(o["colorScheme"]) : std::nullopt;
    if (!scheme) return std::nullopt;
    WidgetTheme t;
    t.colorScheme = std::move(*scheme);
    if (std::optional<std::map<std::string, std::string>> params = stringMap(o["params"])) t.params = std::move(*params);
    t.button = WidgetButtonTokens::from(o["button"]);
    t.text = WidgetTextTokens::from(o["text"]);
    return t;
}

/// A fresh context, as Swift's decoder builds one: each flag a bool or false, the chat type a string or none, a viewer only with a usable `id`.
WidgetContext contextOf(const Json& o) {
    WidgetContext c;
    c.isCopy = o["isCopy"].asBool();
    c.canQuery = o["canQuery"].asBool();
    c.backgroundQuery = o["backgroundQuery"].asBool();
    c.preview = o["preview"].asBool();
    c.wallet = o["wallet"].asBool();
    c.chatType = stringOf(o["chatType"]).value_or("");
    const Json& viewer = o["viewer"];
    if (std::optional<int64_t> id = viewer.isObject() ? int64Of(viewer["id"]) : std::nullopt) {
        WidgetViewer v;
        v.id = *id;
        v.firstName = stringOf(viewer["first_name"]);
        v.lastName = stringOf(viewer["last_name"]);
        v.username = stringOf(viewer["username"]);
        v.isPremium = viewer["is_premium"].asBool();
        c.viewer = std::move(v);
    }
    return c;
}

} // namespace

std::string SandboxRemoteCoding::encodeConfig(const SandboxConfig& c) {
    Json features = Json::array();
    for (const std::string& feature : c.features) features.push(feature);
    Json o = Json::object();
    o.set("memoryLimitBytes", c.memoryLimitBytes).set("cpuLimitMs", c.cpuLimitMs).set("cpuQuotaMs", c.cpuQuotaMs).set("cpuQuotaWindowMs", c.cpuQuotaWindowMs);
    o.set("cpuKillAfterMs", c.cpuKillAfterMs).set("mainThreadQuotaMs", c.mainThreadQuotaMs).set("maxTimers", c.maxTimers).set("visible", c.visible);
    o.set("innerWidth", c.innerWidth).set("innerHeight", c.innerHeight).set("devicePixelRatio", c.devicePixelRatio).set("theme", c.theme);
    o.set("themeParams", stringMapJson(c.themeParams)).set("locale", c.locale).set("contentSafeArea", c.contentSafeArea.json());
    o.set("storage", sdk::StorageCommit{c.storage.local, c.storage.session}.toJson()).set("data", dataJson(c.data)).set("context", contextJson(c.context));
    o.set("policy", c.policy).set("features", std::move(features));
    return o.dump();
}

std::optional<SandboxConfig> SandboxRemoteCoding::decodeConfig(std::string_view text) {
    std::optional<Json> o = parseObject(text);
    if (!o) return std::nullopt;
    std::optional<double> width = finiteNumber(*o, "innerWidth"), height = finiteNumber(*o, "innerHeight"), dpr = finiteNumber(*o, "devicePixelRatio");
    std::optional<std::string> theme = stringOf((*o)["theme"]);
    if (!width || !height || !dpr || !theme) return std::nullopt;
    SandboxConfig c;
    c.innerWidth = *width;
    c.innerHeight = *height;
    c.devicePixelRatio = *dpr;
    c.theme = std::move(*theme);
    // The rest is optional as in Swift's decodeConfig: absent, of another type or a non-finite number keeps the default (as `Json::asInt` does).
    if (std::optional<double> v = finiteNumber(*o, "memoryLimitBytes")) c.memoryLimitBytes = truncatedTo<size_t>(std::min(*v, kExactIntegers));
    for (auto [key, field] : {std::pair{"cpuLimitMs", &c.cpuLimitMs}, {"cpuQuotaMs", &c.cpuQuotaMs}, {"cpuQuotaWindowMs", &c.cpuQuotaWindowMs},
                              {"cpuKillAfterMs", &c.cpuKillAfterMs}, {"mainThreadQuotaMs", &c.mainThreadQuotaMs}})
        if (std::optional<double> v = finiteNumber(*o, key)) *field = *v;
    c.maxTimers = (*o)["maxTimers"].asInt(c.maxTimers);
    if (const Json& v = (*o)["visible"]; v.isBool()) c.visible = v.asBool();
    if (std::optional<std::map<std::string, std::string>> v = stringMap((*o)["themeParams"])) c.themeParams = std::move(*v);
    if (std::optional<std::string> v = stringOf((*o)["locale"])) c.locale = std::move(*v);
    if (const Json& v = (*o)["contentSafeArea"]; v.isObject()) c.contentSafeArea = safeAreaOf(v);
    // Each area an object of strings or empty (Swift's `?? [:]`); the quota stays the receiving engine's check, which drops an over-quota preload with its diagnostic.
    if (const Json& v = (*o)["storage"]; v.isObject()) c.storage = WidgetStorage{stringMap(v["local"]).value_or(StorageArea{}), stringMap(v["session"]).value_or(StorageArea{})};
    if (const Json& v = (*o)["data"]; v.isObject()) c.data = dataOf(v);
    if (const Json& v = (*o)["context"]; v.isObject()) c.context = contextOf(v);
    if (std::optional<std::string> v = stringOf((*o)["policy"])) c.policy = std::move(*v);
    if (std::optional<std::vector<std::string>> v = stringList((*o)["features"])) c.features = std::move(*v);
    return c;
}

std::string SandboxRemoteCoding::encodeSafeArea(const WidgetContentSafeArea& area) { return area.json().dump(); }

std::optional<WidgetContentSafeArea> SandboxRemoteCoding::decodeSafeArea(std::string_view text) {
    std::optional<Json> o = parseObject(text);
    if (!o) return std::nullopt;
    return safeAreaOf(*o);
}

std::string SandboxRemoteCoding::encodeData(const WidgetData& data) { return dataJson(data).dump(); }

std::optional<WidgetData> SandboxRemoteCoding::decodeData(std::string_view text) {
    std::optional<Json> o = parseObject(text);
    if (!o) return std::nullopt;
    return dataOf(*o);
}

std::string SandboxRemoteCoding::encodeThemeParams(const std::map<std::string, std::string>& params) { return stringMapJson(params).dump(); }

std::optional<std::map<std::string, std::string>> SandboxRemoteCoding::decodeThemeParams(std::string_view text) {
    std::optional<Json> o = parseObject(text);
    if (!o) return std::nullopt;
    return stringMap(*o);
}

std::string SandboxRemoteCoding::encodeTheme(const WidgetTheme& theme) { return themeJson(theme).dump(); }

std::optional<WidgetTheme> SandboxRemoteCoding::decodeTheme(std::string_view text) {
    std::optional<Json> o = parseObject(text);
    if (!o) return std::nullopt;
    return themeOf(*o);
}

std::string SandboxRemoteCoding::encodeHostedStart(const SandboxHostedStart& start) {
    Json o = Json::object();
    o.set("theme", themeJson(start.theme)).set("stoppedLabel", start.stoppedLabel).set("validatedOps", start.validatedOps);
    return o.dump();
}

std::optional<SandboxHostedStart> SandboxRemoteCoding::decodeHostedStart(std::string_view text) {
    std::optional<Json> o = parseObject(text);
    if (!o) return std::nullopt;
    std::optional<WidgetTheme> theme = themeOf((*o)["theme"]);
    if (!theme) return std::nullopt;
    SandboxHostedStart start;
    start.theme = std::move(*theme);
    if (std::optional<std::string> label = stringOf((*o)["stoppedLabel"])) start.stoppedLabel = label->substr(0, SandboxEngine::kRemoteTextLimit);
    start.validatedOps = (*o)["validatedOps"].asBool();
    return start;
}

std::string SandboxRemoteCoding::encodeOutcome(const SandboxRemoteOutcome& outcome) {
    Json o = Json::object();
    switch (outcome.kind) {
    case SandboxRemoteOutcome::Kind::Answer: o.set("answer", orNull(outcome.result)); break;
    case SandboxRemoteOutcome::Kind::Failure: o.set("failure", sdk::walletFailureCode(outcome.failure)); break;
    case SandboxRemoteOutcome::Kind::Unsupported: o.set("unsupported", true); break;
    }
    return o.dump();
}

std::optional<SandboxRemoteOutcome> SandboxRemoteCoding::decodeOutcome(std::string_view text) {
    std::optional<Json> o = parseObject(text);
    if (!o) return std::nullopt;
    if (o->find("unsupported")) return SandboxRemoteOutcome::unsupported();
    if (const Json* failure = o->find("failure"); failure && failure->isString()) {
        for (WidgetRequestFailure known : {WidgetRequestFailure::Timeout, WidgetRequestFailure::RateLimited, WidgetRequestFailure::Failed,
                                           WidgetRequestFailure::Rejected, WidgetRequestFailure::BadRequest})
            if (sdk::walletFailureCode(known) == failure->asString()) return SandboxRemoteOutcome::failed(known);
        return SandboxRemoteOutcome::failed(WidgetRequestFailure::NotAllowed);
    }
    if (const Json* answer = o->find("answer")) return SandboxRemoteOutcome::answer(stringOf(*answer));
    return std::nullopt;
}

std::string SandboxRemoteCoding::encodeWallet(const WalletCall& call) {
    Json args = Json::array();
    args.push(1).push(call.methodName()).push(call.method == WalletCall::Method::SignData ? call.payload.toJson() : call.transaction.toJson()).push(call.data);
    return args.dump();
}

std::optional<WalletCall> SandboxRemoteCoding::decodeWallet(std::string_view text) {
    std::optional<Json> args = parse(text);
    if (!args) return std::nullopt;
    std::optional<sdk::BridgeCall> call = sdk::checkBridgeCall(Json("wallet"), *args);
    if (!call || !call->wallet) return std::nullopt;
    return std::move(call->wallet->call);
}

std::vector<double> SandboxRemoteCoding::encodeStats(const SandboxStats& s) {
    return {s.jsMs, double(s.tasks), double(s.memoryBytes), double(s.ops), double(s.queries), s.queryMs, double(s.queryCacheHits), double(s.missedMutations),
            double(s.throttledTasks), s.cpuWindowMs, s.validatorMs};
}

std::optional<SandboxStats> SandboxRemoteCoding::decodeStats(const std::vector<double>& values) {
    if (values.size() != kStatsFields) return std::nullopt;
    std::array<double, kStatsFields> v;
    for (size_t i = 0; i < kStatsFields; i++) v[i] = std::isfinite(values[i]) && values[i] > 0 ? std::min(values[i], kStatsMax) : 0.0;
    SandboxStats s;
    s.jsMs = v[0];
    s.tasks = truncatedTo<int>(v[1]);
    s.memoryBytes = truncatedTo<size_t>(v[2]);
    s.ops = truncatedTo<int>(v[3]);
    s.queries = truncatedTo<int>(v[4]);
    s.queryMs = v[5];
    s.queryCacheHits = truncatedTo<int>(v[6]);
    s.missedMutations = truncatedTo<int>(v[7]);
    s.throttledTasks = truncatedTo<int>(v[8]);
    s.cpuWindowMs = v[9];
    s.validatorMs = v[10];
    return s;
}

} // namespace twidget
