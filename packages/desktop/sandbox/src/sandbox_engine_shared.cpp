// What every SandboxEngine runs, in process and in client mode: the constructor's shared part and the destructor, the entry points both modes take (each branching on the mode: a hosted client's refusal, the client's call, else the in-process half through `inProcess_`, sandbox_engine.cpp), the haptic gate's presses, the clock, the value types' JSON and the places' geometry (`surface::`). The in-process halves are reached only through that table, which the in-process constructor alone installs, so a program whose engines are all client-mode ones links neither them nor Nova.
#include "twidget/sandbox_engine.h"

#include "twidget/json.h"
#include "twidget/remote_engine.h"
#include "steady_clock.h"

#include <algorithm>
#include <cmath>

namespace twidget {

double SandboxEngine::nowSeconds() { return detail::steadySeconds(); }
double SandboxEngine::nowMs() { return nowSeconds() * 1000; }

SandboxEngine::SandboxEngine(std::string threadName, std::string html, SandboxConfig config, std::shared_ptr<SandboxResources> resources, std::shared_ptr<IntlProvider> intl)
    : thread_(std::move(threadName)), html_(std::move(html)), config_(config), resources_(std::move(resources)),
      intl_(intl ? std::move(intl) : std::make_shared<BasicIntl>()), visible_(config.visible) {
    liveness_->engine = this;
    // A preview (an inline result not sent yet) asks nothing whatever the flags say (docs/sdk-v1.md §2.16). Fixed from
    // here on, so other threads may read config_ without a lock.
    if (config_.context.preview) config_.context.isCopy = config_.context.canQuery = config_.context.backgroundQuery = false;
    // The wallet only where the widget may ask the bot (docs/sdk-v1.md §2.18).
    config_.context.wallet = config_.context.wallet && config_.context.canQuery;
}

SandboxEngine::~SandboxEngine() {
    {
        // A request's `respond` called from now on is ignored; one that holds the lock has posted its task, which runs before the thread stops.
        std::lock_guard<std::mutex> l(liveness_->mutex);
        liveness_->engine = nullptr;
    }
    thread_.join();
    // The thread is gone: nothing else touches the context, so it can be freed here.
    js_.reset();
    // A platform request waiting on its caller's thread returns now, and every later one at once, so the channel's threads (a stream's `widget.answer`, a loopback's glue) are free as it closes.
    if (client_) clientSettleWaiting();
    // From the owner's thread: a disconnection the channel reports while it closes meets the null liveness record. A hosted client's channel is the same one.
    if (remote_) remote_->close();
    remote_.reset();
    hosted_.reset();
}

void SandboxEngine::setDelegate(SandboxEngineDelegate* delegate) {
    if (client_) return clientSetDelegate(delegate);
    delegate_ = delegate;
}

// MARK: lifecycle

// A hosted client takes none of the engine-only inputs (`start`, `event`, `activationEvent`, `frame`, `viewport`, `resize`, `theme`, `pressBegan`, `pressEnded`, `chargeValidator`): no such call reaches its channel, records a grant or touches its haptic gate.
void SandboxEngine::start() {
    if (isHosted()) return;
    if (client_) return clientStart();
    (this->*inProcess_->start)();
}

void SandboxEngine::terminate() {
    if (client_) return clientKill("terminated", false);
    (this->*inProcess_->terminate)();
}

void SandboxEngine::stop(const std::string& reason) {
    if (client_) return clientKill(reason, true);
    (this->*inProcess_->stop)(reason);
}

// MARK: inputs from the host (all asynchronous)

// A preview takes no input (a tap on it selects the result, the client's); WidgetHost drops the renderer's events first.
void SandboxEngine::event(const std::string& json) {
    if (isHosted() || config_.context.preview) return;
    if (client_) return forward([json](SandboxRemoteChannel& remote) { remote.event(json); });
    (this->*inProcess_->event)(json);
}
void SandboxEngine::activationEvent(const std::string& json, int gesture) {
    if (isHosted() || config_.context.preview) return;
    if (client_) return clientActivationEvent(json, gesture);
    (this->*inProcess_->activationEvent)(json, gesture);
}

void SandboxEngine::frame(double time) {
    if (isHosted()) return;
    if (client_) return forward([time](SandboxRemoteChannel& remote) { remote.frame(time); });
    (this->*inProcess_->frame)(time);
}
void SandboxEngine::viewport(double innerWidth, double innerHeight, double dpr) {
    if (isHosted()) return;
    if (client_) return forward([innerWidth, innerHeight, dpr](SandboxRemoteChannel& remote) { remote.viewport(innerWidth, innerHeight, dpr); });
    (this->*inProcess_->viewport)(innerWidth, innerHeight, dpr);
}
void SandboxEngine::resize(const std::string& entriesJson) {
    if (isHosted()) return;
    if (client_) return forward([entriesJson](SandboxRemoteChannel& remote) { remote.resize(entriesJson); });
    (this->*inProcess_->resize)(entriesJson);
}
void SandboxEngine::theme(const std::string& colorScheme, const std::map<std::string, std::string>& themeParams) {
    if (isHosted()) return;
    if (client_) return forward([colorScheme, themeParams](SandboxRemoteChannel& remote) { remote.theme(colorScheme, themeParams); });
    (this->*inProcess_->theme)(colorScheme, themeParams);
}

Json WidgetContentSafeArea::json() const {
    Json out = Json::object();
    auto put = [&](const char* name, double v) { out.set(name, std::isfinite(v) ? v : 0.0); };
    put("top", top);
    put("right", right);
    put("bottom", bottom);
    put("left", left);
    put("cornerLeft", cornerLeft);
    put("cornerRight", cornerRight);
    return out;
}

Json WidgetSafeArea::json() const {
    Json out = Json::object();
    auto put = [&](const char* name, double v) { out.set(name, std::isfinite(v) ? v : 0.0); };
    put("top", top);
    put("right", right);
    put("bottom", bottom);
    put("left", left);
    return out;
}

namespace surface {
double checkViewportHeight(WidgetSurface s, double height) {
    if (s != WidgetSurface::Profile || !std::isfinite(height) || height <= 0 || height > kPageMaxHeight) return 0;
    return std::round(height * 100) / 100;
}

double height(WidgetSurface s, double content, double viewport) {
    double h = std::isfinite(content) ? content : 0;
    if (s == WidgetSurface::Message) return std::min(std::max(h, kMinHeight), kMessageMaxHeight);
    return std::min(std::max(h, std::max(kMinHeight, checkViewportHeight(s, viewport))), kPageMaxHeight);
}
} // namespace surface

namespace {

/// A theme token's key and its member, a string or a number.
template <class Tokens>
struct ThemeToken {
    const char* key;
    std::optional<std::string> Tokens::*text;
    std::optional<double> Tokens::*number;
};

/// The tokens in the order the renderer's theme message and the wire write them: the one list both the builders and the readers go through.
constexpr ThemeToken<WidgetButtonTokens> kButtonTokens[] = {
    {"bg", &WidgetButtonTokens::bg, nullptr},
    {"text", &WidgetButtonTokens::text, nullptr},
    {"radius", &WidgetButtonTokens::radius, nullptr},
    {"font", &WidgetButtonTokens::font, nullptr},
    {"padding", &WidgetButtonTokens::padding, nullptr},
    {"height", &WidgetButtonTokens::height, nullptr},
    {"pressedOpacity", nullptr, &WidgetButtonTokens::pressedOpacity},
    {"disabledOpacity", nullptr, &WidgetButtonTokens::disabledOpacity},
};
constexpr ThemeToken<WidgetTextTokens> kTextTokens[] = {
    {"fontSize", nullptr, &WidgetTextTokens::fontSize},
    {"lineHeight", nullptr, &WidgetTextTokens::lineHeight},
    {"padding", &WidgetTextTokens::padding, nullptr},
};

template <class Tokens, size_t N>
Json tokensJson(const Tokens& tokens, const ThemeToken<Tokens> (&keys)[N]) {
    Json out = Json::object();
    for (const ThemeToken<Tokens>& token : keys) {
        if (token.text) {
            if (const std::optional<std::string>& v = tokens.*token.text) out.set(token.key, *v);
        } else if (const std::optional<double>& v = tokens.*token.number; v && std::isfinite(*v)) {
            out.set(token.key, *v);
        }
    }
    return out;
}

template <class Tokens, size_t N>
Tokens tokensFrom(const Json& object, const ThemeToken<Tokens> (&keys)[N]) {
    Tokens tokens;
    if (!object.isObject()) return tokens;
    for (const ThemeToken<Tokens>& token : keys) {
        const Json& v = object[token.key];
        if (token.text) {
            if (v.isString()) tokens.*token.text = v.asString();
        } else if (v.isNumber() && std::isfinite(v.asNumber())) {
            tokens.*token.number = v.asNumber();
        }
    }
    return tokens;
}

} // namespace

Json WidgetButtonTokens::json() const { return tokensJson(*this, kButtonTokens); }
WidgetButtonTokens WidgetButtonTokens::from(const Json& object) { return tokensFrom(object, kButtonTokens); }
Json WidgetTextTokens::json() const { return tokensJson(*this, kTextTokens); }
WidgetTextTokens WidgetTextTokens::from(const Json& object) { return tokensFrom(object, kTextTokens); }

void SandboxEngine::contentSafeArea(const WidgetContentSafeArea& area) {
    if (client_) return forward([area](SandboxRemoteChannel& remote) { remote.contentSafeArea(area); });
    (this->*inProcess_->contentSafeArea)(area);
}

void SandboxEngine::safeArea(const WidgetSafeArea& area) {
    // Neither the config's wire form nor a remote channel carries the device's edges yet: a client-mode engine's widget has none.
    if (client_) return;
    (this->*inProcess_->safeArea)(area);
}

void SandboxEngine::setData(const WidgetData& data) {
    if (client_) return forward([data](SandboxRemoteChannel& remote) { remote.setData(data); });
    (this->*inProcess_->setData)(data);
}

// The haptic gate's touch (v0.2.2): stamped on the caller's thread with the clock `decide` reads, applied in order
// with the events the host sends after it (a tap's haptic sees its press). Gate state only: no task, never deferred.
void SandboxEngine::pressBegan() {
    if (isHosted()) return;
    if (client_) return clientPress(true);
    double at = nowMs();
    thread_.async([this, at] { haptics_.press(at); });
}
void SandboxEngine::pressEnded() {
    if (isHosted()) return;
    if (client_) return clientPress(false);
    double at = nowMs();
    thread_.async([this, at] { haptics_.release(at); });
}

void SandboxEngine::setVisible(bool v) {
    if (client_) return clientSetVisible(v);
    (this->*inProcess_->setVisible)(v);
}

std::string SandboxEngine::probe(const std::string& code) {
    if (client_) return clientProbe(code);
    return (this->*inProcess_->probe)(code);
}

void SandboxEngine::chargeValidator(double ms) {
    if (isHosted() || !(ms > 0)) return;
    if (client_) return forward([ms](SandboxRemoteChannel& remote) { remote.chargeValidator(ms); });
    (this->*inProcess_->chargeValidator)(ms);
}

} // namespace twidget
