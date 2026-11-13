#include "twidget/remote_engine.h"

#include "twidget/intl.h"
#include "twidget/json.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

namespace twidget {

// MARK: coding

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

// MARK: the engine's side

namespace {

/// The engine's IntlProvider on the engine's side: the host's provider formats each request through the synchronous `remoteIntl`.
class RemoteIntl final : public IntlProvider {
public:
    explicit RemoteIntl(std::shared_ptr<SandboxRemoteSink> sink) : sink_(std::move(sink)) {}
    std::string format(const Json& request) override { return sink_->remoteIntl(request.dump()); }

private:
    std::shared_ptr<SandboxRemoteSink> sink_;
};

} // namespace

SandboxRemoteServer::SandboxRemoteServer(std::shared_ptr<SandboxRemoteSink> sink, std::shared_ptr<SandboxResources> resources)
    : sink_(std::move(sink)), resources_(std::move(resources)) {}

SandboxRemoteServer::~SandboxRemoteServer() {
    close();
    std::unique_ptr<SandboxEngine> engine;
    {
        std::lock_guard<std::mutex> l(mutex_);
        engine = std::move(engine_);
    }
    // Joins the engine's thread, whose last tasks may still call this delegate and the sink.
    engine.reset();
}

void SandboxRemoteServer::start(const std::string& html, const SandboxConfig& config) {
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (closed_ || started_) return;
        started_ = true;
    }
    auto engine = std::make_unique<SandboxEngine>(html, config, resources_, std::make_shared<RemoteIntl>(sink_));
    engine->setDelegate(this);
    SandboxEngine& built = *engine;
    std::vector<std::function<void(SandboxEngine&)>> waiting;
    {
        std::lock_guard<std::mutex> l(mutex_);
        engine_ = std::move(engine);
        waiting.swap(waiting_);
    }
    for (auto& call : waiting) call(built);
    built.start();
}

void SandboxRemoteServer::input(std::function<void(SandboxEngine&)> call) {
    SandboxEngine* engine = nullptr;
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (closed_) return;
        if (!engine_) return waiting_.push_back(std::move(call));
        engine = engine_.get();
    }
    call(*engine);
}

void SandboxRemoteServer::event(const std::string& json) {
    input([json](SandboxEngine& engine) { engine.event(json); });
}
void SandboxRemoteServer::activationEvent(const std::string& json, int gesture) {
    input([json, gesture](SandboxEngine& engine) { engine.activationEvent(json, gesture); });
}
void SandboxRemoteServer::frame(double time) {
    input([time](SandboxEngine& engine) { engine.frame(time); });
}
void SandboxRemoteServer::viewport(double innerWidth, double innerHeight, double dpr) {
    input([innerWidth, innerHeight, dpr](SandboxEngine& engine) { engine.viewport(innerWidth, innerHeight, dpr); });
}
void SandboxRemoteServer::resize(const std::string& entriesJson) {
    input([entriesJson](SandboxEngine& engine) { engine.resize(entriesJson); });
}
void SandboxRemoteServer::theme(const std::string& colorScheme, const std::map<std::string, std::string>& themeParams) {
    input([colorScheme, themeParams](SandboxEngine& engine) { engine.theme(colorScheme, themeParams); });
}
void SandboxRemoteServer::contentSafeArea(const WidgetContentSafeArea& area) {
    input([area](SandboxEngine& engine) { engine.contentSafeArea(area); });
}
void SandboxRemoteServer::setData(const WidgetData& data) {
    input([data](SandboxEngine& engine) { engine.setData(data); });
}
void SandboxRemoteServer::setVisible(bool visible) {
    input([visible](SandboxEngine& engine) { engine.setVisible(visible); });
}
void SandboxRemoteServer::pressBegan() {
    input([](SandboxEngine& engine) { engine.pressBegan(); });
}
void SandboxRemoteServer::pressEnded() {
    input([](SandboxEngine& engine) { engine.pressEnded(); });
}
void SandboxRemoteServer::chargeValidator(double ms) {
    input([ms](SandboxEngine& engine) { engine.chargeValidator(ms); });
}
void SandboxRemoteServer::stop(const std::string& reason) {
    input([reason](SandboxEngine& engine) { engine.stop(reason); });
}

void SandboxRemoteServer::answer(int64_t id, const SandboxRemoteOutcome& outcome) {
    std::map<int64_t, WidgetRequestResponder>::node_type taken;
    {
        std::lock_guard<std::mutex> l(mutex_);
        taken = pending_.extract(id);
    }
    if (taken.empty()) return;
    const WidgetRequestResponder& respond = taken.mapped();
    switch (outcome.kind) {
    case SandboxRemoteOutcome::Kind::Answer: return respond(WidgetRequestOutcome{WidgetRequestFailure::None, WidgetRequestAnswer{outcome.result}});
    // A failure stays one: `None` here would read as an answer.
    case SandboxRemoteOutcome::Kind::Failure:
        return respond(WidgetRequestOutcome{outcome.failure == WidgetRequestFailure::None ? WidgetRequestFailure::Failed : outcome.failure, {}});
    case SandboxRemoteOutcome::Kind::Unsupported: return respond.unsupported();
    }
}

std::string SandboxRemoteServer::probe(const std::string& code) {
    SandboxEngine* engine = nullptr;
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (!closed_) engine = engine_.get();
    }
    return engine ? engine->probe(code) : "ERROR no runtime";
}

void SandboxRemoteServer::close() {
    std::map<int64_t, WidgetRequestResponder> open;
    SandboxEngine* engine = nullptr;
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (std::exchange(closed_, true)) return;
        waiting_.clear();
        open.swap(pending_);
        engine = engine_.get();
    }
    for (const auto& entry : open) entry.second.unsupported();
    if (engine) engine->terminate();
}

void SandboxRemoteServer::forward(WidgetRequestResponder respond, const std::function<void(int64_t id)>& send) {
    int64_t id = 0;
    {
        std::lock_guard<std::mutex> l(mutex_);
        // Every id is new: past the last one the calls are refused as after `close`.
        if (!closed_ && lastRequest_ < std::numeric_limits<int64_t>::max()) {
            id = ++lastRequest_;
            pending_.emplace(id, std::move(respond));
        }
    }
    if (!id) return respond.unsupported();
    send(id);
}

void SandboxRemoteServer::onTaskStarted(SandboxEngine&) { sink_->remoteTaskStarted(); }
void SandboxRemoteServer::onTaskCompleted(SandboxEngine&) { sink_->remoteTaskCompleted(); }
void SandboxRemoteServer::onOps(SandboxEngine&, const std::string& json) { sink_->remoteOps(json); }
QueryAnswer SandboxRemoteServer::onQuery(SandboxEngine&, const std::string& json) { return sink_->remoteQuery(json); }
void SandboxRemoteServer::onListen(SandboxEngine&, const std::string& json) { sink_->remoteListen(json); }
void SandboxRemoteServer::onPreventDefault(SandboxEngine&, const std::string& json) { sink_->remotePreventDefault(json); }
void SandboxRemoteServer::onAnim(SandboxEngine&, const std::string& json) { sink_->remoteAnim(json); }
void SandboxRemoteServer::onRequestFrame(SandboxEngine&) { sink_->remoteRequestsFrame(); }
void SandboxRemoteServer::onLog(SandboxEngine&, const std::string& level, const std::string& text) { sink_->remoteLog(level, text); }
void SandboxRemoteServer::onDiag(SandboxEngine&, const std::string& kind, const std::string& detail) { sink_->remoteDiag(kind, detail); }
// The raw call right after it is what the client checks again: an action goes once.
void SandboxRemoteServer::onBridgeAction(SandboxEngine&, const WidgetAction&) {}
void SandboxRemoteServer::onBridge(SandboxEngine&, const std::string& method, const std::string& argsJson) { sink_->remoteBridge(method, argsJson); }
void SandboxRemoteServer::onBridgeCopyText(SandboxEngine&, const std::string& text) { sink_->remoteCopyText(text); }
void SandboxRemoteServer::onStorage(SandboxEngine&, const std::string& area, const StorageArea& values) {
    sdk::StorageCommit commit;
    (area == "local" ? commit.local : commit.session) = values;
    sink_->remoteStorage(commit.toJson().dump());
}
void SandboxRemoteServer::onResizeAnchor(SandboxEngine&, const std::string& edge) { sink_->remoteResizeAnchor(edge); }
void SandboxRemoteServer::onPublicDataWatch(SandboxEngine&, bool on) { sink_->remoteWatchesPublicData(on); }
void SandboxRemoteServer::onBridgeRequest(SandboxEngine&, const WidgetRequest& request, WidgetRequestResponder respond) {
    forward(std::move(respond), [&](int64_t id) { sink_->remoteRequest(id, request.data, request.wallet); });
}
void SandboxRemoteServer::onBridgeWallet(SandboxEngine&, const WalletCall& call, WidgetRequestResponder respond) {
    forward(std::move(respond), [&](int64_t id) { sink_->remoteWallet(id, SandboxRemoteCoding::encodeWallet(call)); });
}
void SandboxRemoteServer::onLoaded(SandboxEngine&) { sink_->remoteLoaded(); }
void SandboxRemoteServer::onStats(SandboxEngine&, const SandboxStats& stats) { sink_->remoteStats(stats); }
void SandboxRemoteServer::onDied(SandboxEngine&, const std::string& reason) { sink_->remoteDied(reason); }

// MARK: the loopback

namespace {

/// The server's sink in a loopback: the client's, its stats coded on the way.
class LoopbackSink final : public SandboxRemoteSink {
public:
    explicit LoopbackSink(std::shared_ptr<SandboxRemoteSink> client) : client_(std::move(client)) {}

    void remoteTaskStarted() override { client_->remoteTaskStarted(); }
    void remoteTaskCompleted() override { client_->remoteTaskCompleted(); }
    void remoteOps(const std::string& json) override { client_->remoteOps(json); }
    QueryAnswer remoteQuery(const std::string& json) override { return client_->remoteQuery(json); }
    std::string remoteIntl(const std::string& requestJson) override { return client_->remoteIntl(requestJson); }
    void remoteListen(const std::string& json) override { client_->remoteListen(json); }
    void remotePreventDefault(const std::string& json) override { client_->remotePreventDefault(json); }
    void remoteAnim(const std::string& json) override { client_->remoteAnim(json); }
    void remoteRequestsFrame() override { client_->remoteRequestsFrame(); }
    void remoteLog(const std::string& level, const std::string& text) override { client_->remoteLog(level, text); }
    void remoteDiag(const std::string& kind, const std::string& detail) override { client_->remoteDiag(kind, detail); }
    void remoteBridge(const std::string& method, const std::string& argsJson) override { client_->remoteBridge(method, argsJson); }
    void remoteCopyText(const std::string& text) override { client_->remoteCopyText(text); }
    void remoteRequest(int64_t id, const std::string& data, bool wallet) override { client_->remoteRequest(id, data, wallet); }
    void remoteWallet(int64_t id, const std::string& payload) override { client_->remoteWallet(id, payload); }
    void remoteStorage(const std::string& commitJson) override { client_->remoteStorage(commitJson); }
    void remoteResizeAnchor(const std::string& edge) override { client_->remoteResizeAnchor(edge); }
    void remoteWatchesPublicData(bool on) override { client_->remoteWatchesPublicData(on); }
    void remoteLoaded() override { client_->remoteLoaded(); }
    void remoteStats(const SandboxStats& stats) override {
        if (std::optional<SandboxStats> coded = SandboxRemoteCoding::decodeStats(SandboxRemoteCoding::encodeStats(stats))) client_->remoteStats(*coded);
        else client_->remoteDisconnected();
    }
    void remoteDied(const std::string& reason) override { client_->remoteDied(reason); }
    void remoteDisconnected() override { client_->remoteDisconnected(); }

private:
    const std::shared_ptr<SandboxRemoteSink> client_;
};

} // namespace

SandboxLoopbackChannel::SandboxLoopbackChannel(std::shared_ptr<SandboxResources> resources) : resources_(std::move(resources)) {}

void SandboxLoopbackChannel::attach(std::shared_ptr<SandboxRemoteSink> sink) {
    client_ = std::move(sink);
    server_ = std::make_unique<SandboxRemoteServer>(std::make_shared<LoopbackSink>(client_), resources_);
}

void SandboxLoopbackChannel::start(const std::string& html, const SandboxConfig& config) {
    if (std::optional<SandboxConfig> coded = SandboxRemoteCoding::decodeConfig(SandboxRemoteCoding::encodeConfig(config))) server_->start(html, *coded);
    else client_->remoteDisconnected();
}
void SandboxLoopbackChannel::event(const std::string& json) { server_->event(json); }
void SandboxLoopbackChannel::activationEvent(const std::string& json, int gesture) { server_->activationEvent(json, gesture); }
void SandboxLoopbackChannel::frame(double time) { server_->frame(time); }
void SandboxLoopbackChannel::viewport(double innerWidth, double innerHeight, double dpr) { server_->viewport(innerWidth, innerHeight, dpr); }
void SandboxLoopbackChannel::resize(const std::string& entriesJson) { server_->resize(entriesJson); }
void SandboxLoopbackChannel::theme(const std::string& colorScheme, const std::map<std::string, std::string>& themeParams) { server_->theme(colorScheme, themeParams); }
void SandboxLoopbackChannel::contentSafeArea(const WidgetContentSafeArea& area) {
    if (std::optional<WidgetContentSafeArea> coded = SandboxRemoteCoding::decodeSafeArea(SandboxRemoteCoding::encodeSafeArea(area))) server_->contentSafeArea(*coded);
    else client_->remoteDisconnected();
}
void SandboxLoopbackChannel::setData(const WidgetData& data) {
    if (std::optional<WidgetData> coded = SandboxRemoteCoding::decodeData(SandboxRemoteCoding::encodeData(data))) server_->setData(*coded);
    else client_->remoteDisconnected();
}
void SandboxLoopbackChannel::setVisible(bool visible) { server_->setVisible(visible); }
void SandboxLoopbackChannel::pressBegan() { server_->pressBegan(); }
void SandboxLoopbackChannel::pressEnded() { server_->pressEnded(); }
void SandboxLoopbackChannel::chargeValidator(double ms) { server_->chargeValidator(ms); }
void SandboxLoopbackChannel::stop(const std::string& reason) { server_->stop(reason); }
void SandboxLoopbackChannel::answer(int64_t id, const SandboxRemoteOutcome& outcome) {
    if (std::optional<SandboxRemoteOutcome> coded = SandboxRemoteCoding::decodeOutcome(SandboxRemoteCoding::encodeOutcome(outcome))) server_->answer(id, *coded);
    else client_->remoteDisconnected();
}
std::string SandboxLoopbackChannel::probe(const std::string& code) { return server_->probe(code); }
void SandboxLoopbackChannel::close() { server_->close(); }

} // namespace twidget
