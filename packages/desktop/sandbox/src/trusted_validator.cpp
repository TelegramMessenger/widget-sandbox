#include "twidget/trusted_validator.h"

#include "twidget/bundle_stamp.h"

#include <utility>
#include <vector>

namespace twidget {

// A validator's thread and context, with the bundle evaluated and its API flattened into globals: everything but
// `create(theme)`, the one step that is the widget's.
struct TrustedValidator::Core {
    JSThread thread{"widget.validator"};
    std::unique_ptr<QuickJSContext> js;
    // The engine the context runs on: a validator takes a core made ahead only for the engine it was asked for.
    QuickJSBackend backend = quickjsDefaultBackend();
    // The validator's thread only. Diagnostics from before a validator took the core wait in `early`.
    DiagSink onDiag;
    std::vector<std::pair<std::string, std::string>> early;
    std::mutex stoppedMutex;
    std::optional<std::string> stopped;
    CpuSink cpuSink;

    void diag(const std::string& kind, const std::string& detail) {
        if (onDiag) onDiag(kind, detail);
        else early.emplace_back(kind, detail);
    }
    ~Core() {
        thread.sync([&] { js.reset(); });
        thread.join();
    }
};

std::unique_ptr<TrustedValidator::Core> TrustedValidator::makeCore(const std::shared_ptr<SandboxResources>& resources, QuickJSBackend backend) {
    auto core = std::make_unique<Core>();
    Core* c = core.get();
    c->backend = backend;
    c->thread.sync([&] {
        c->js = std::make_unique<QuickJSContext>(size_t(256) << 20, JSThread::kJSStackLimit, backend);
        if (!c->js->available()) {
            c->diag("engine", "validator: " + c->js->unavailableReason());
            return;
        }
        c->js->registerFunction("__diag", [c](const std::vector<std::string>& args) {
            std::string kind = args.empty() ? "" : args[0], detail = args.size() > 1 ? args[1] : "";
            if (kind == "limit") {
                // Fatal: the owner reads it once the call returns and stops the widget (the bundle reports it once).
                std::lock_guard<std::mutex> l(c->stoppedMutex);
                if (!c->stopped) c->stopped = detail;
            }
            c->diag(kind, detail);
            return HostReturn::undefined();
        });
        // Bundles of another release are refused before anything runs (bundle_stamp.h): no bundle, every call
        // answers empty (`validator-error`).
        if (auto refusal = BundleStamp::refusal(*resources)) {
            c->diag("bundle-mismatch", *refusal);
            return;
        }
        // Precompiled when shipped and matching the source, else the source (same rule as the engine).
        bool fromBytecode = false;
        if (!c->js->runsBytecode()) {
            // Nova: from code compiled once per process (or from source when the bundle cannot be shared).
            c->js->evaluateBundle(resources->validatorBundle, "validator-bundle.js");
            fromBytecode = true;
        } else if (!resources->validatorBytecode.empty()) {
            try { c->js->evaluateBytecode(resources->validatorBytecode, resources->validatorBundle); fromBytecode = true; }
            catch (const JSError& e) { c->diag("bytecode", std::string("validator-bundle.js: falling back to source — ") + e.what()); }
        }
        if (!fromBytecode) c->js->evaluate(resources->validatorBundle, "validator-bundle.js");
        // Flatten the API into globals so QuickJSContext::call can reach it.
        c->js->evaluate(
            "globalThis.__v_processOps = (j) => __twValidator.processOps(j);"
            "globalThis.__v_validateQuery = (j) => __twValidator.validateQuery(j);"
            "globalThis.__v_validateListen = (j) => __twValidator.validateListen(j);"
            "globalThis.__v_validatePreventDefault = (j) => __twValidator.validatePreventDefault(j);"
            "globalThis.__v_validateAnim = (j) => __twValidator.validateAnim(j);"
            "globalThis.__v_setTheme = (t) => __twValidator.setTheme(t);"
            "globalThis.__v_rewriteMediaQuery = (q) => __twValidator.rewriteMediaQuery(q);"
            "globalThis.__v_nodeCount = () => __twValidator.nodeCount();"
            "globalThis.__v_markLoaded = () => { __twValidator.markLoaded(); return ''; };"
            "globalThis.__v_create = (t) => { __twValidator.create(t); return ''; };"
            "globalThis.__v_setPolicy = (p) => __twValidator.setPolicy ? __twValidator.setPolicy(p) : '';",
            "validator-glue.js");
    });
    return core;
}

namespace {
// The core made ahead (TrustedValidator::prepare) and the resources it is of.
std::mutex preparedMutex;
std::weak_ptr<SandboxResources> preparedFor;
}
std::unique_ptr<TrustedValidator::Core>& TrustedValidator::preparedCore() {
    static std::unique_ptr<Core> core;
    return core;
}

void TrustedValidator::prepare(std::shared_ptr<SandboxResources> resources, QuickJSBackend backend) {
    if (!resources) return;
    {
        std::lock_guard<std::mutex> l(preparedMutex);
        if (preparedCore() && preparedFor.lock() == resources && preparedCore()->backend == backend) return;
    }
    std::unique_ptr<Core> made;
    try {
        made = makeCore(resources, backend);
    } catch (...) {
        return; // the validator that would have taken it makes its own, and throws or reports as before
    }
    std::unique_ptr<Core> old;
    {
        std::lock_guard<std::mutex> l(preparedMutex);
        old = std::move(preparedCore());
        preparedCore() = std::move(made);
        preparedFor = resources;
    }
}

void TrustedValidator::discardPrepared() {
    std::unique_ptr<Core> old;
    std::lock_guard<std::mutex> l(preparedMutex);
    old = std::move(preparedCore());
}

TrustedValidator::TrustedValidator(const std::string& theme, std::shared_ptr<SandboxResources> resources, DiagSink onDiag, QuickJSBackend backend) {
    {
        std::lock_guard<std::mutex> l(preparedMutex);
        if (preparedCore() && preparedFor.lock() == resources && preparedCore()->backend == backend) core_ = std::move(preparedCore());
    }
    if (!core_) core_ = makeCore(resources, backend);
    core_->thread.sync([&] {
        core_->onDiag = std::move(onDiag);
        auto early = std::move(core_->early);
        core_->early.clear();
        // No bundle (another release's, or an engine this build cannot provide): nothing to create, every call
        // answers empty.
        bool unusable = false;
        for (const auto& [kind, detail] : early) {
            unusable |= kind == "bundle-mismatch" || kind == "engine";
            if (core_->onDiag) core_->onDiag(kind, detail);
        }
        if (!unusable) core_->js->call("__v_create", {theme});
    });
}

TrustedValidator::~TrustedValidator() = default;

std::string TrustedValidator::str(const char* name, const std::vector<std::string>& args) {
    return core_->thread.sync([&]() -> std::string {
        try {
            return core_->js->call(name, args).string.value_or("");
        } catch (const JSError& e) {
            if (core_->onDiag) core_->onDiag("validator-error", std::string(name) + ": " + e.what());
            return "";
        }
    });
}

void TrustedValidator::setCpuSink(CpuSink sink) {
    core_->thread.sync([&] { core_->cpuSink = std::move(sink); });
}

std::string TrustedValidator::metered(const char* name, const std::vector<std::string>& args) {
    return core_->thread.sync([&]() -> std::string {
        if (!core_->cpuSink) return str(name, args);
        double t0 = QuickJSContext::threadCpuTime();
        std::string out = str(name, args);
        core_->cpuSink((QuickJSContext::threadCpuTime() - t0) * 1000);
        return out;
    });
}

std::string TrustedValidator::processOps(const std::string& json) {
    std::string out = metered("__v_processOps", {json});
    return out.empty() ? "[]" : out;
}

std::optional<std::string> TrustedValidator::validateQuery(const std::string& json) {
    std::string out = metered("__v_validateQuery", {json});
    if (out.empty()) return std::nullopt;
    return out;
}

std::string TrustedValidator::validateListen(const std::string& json) { auto s = metered("__v_validateListen", {json}); return s.empty() ? "[]" : s; }
void TrustedValidator::setPolicy(const std::string& policy) { str("__v_setPolicy", {policy}); }
void TrustedValidator::markLoaded() { str("__v_markLoaded", {}); }
std::string TrustedValidator::validatePreventDefault(const std::string& json) { auto s = metered("__v_validatePreventDefault", {json}); return s.empty() ? "[]" : s; }
std::string TrustedValidator::validateAnim(const std::string& json) { auto s = metered("__v_validateAnim", {json}); return s.empty() ? "[]" : s; }
std::string TrustedValidator::setTheme(const std::string& theme) { auto s = str("__v_setTheme", {theme}); return s.empty() ? "[]" : s; }
std::string TrustedValidator::rewriteMediaQuery(const std::string& query) { return metered("__v_rewriteMediaQuery", {query}); }

std::optional<std::string> TrustedValidator::stopped() const {
    std::lock_guard<std::mutex> l(core_->stoppedMutex);
    return core_->stopped;
}

int TrustedValidator::nodeCount() {
    return core_->thread.sync([&]() -> int {
        try { return int(core_->js->call("__v_nodeCount").number.value_or(0)); } catch (const JSError&) { return 0; }
    });
}

} // namespace twidget
