#include "twidget/trusted_validator.h"

namespace twidget {

TrustedValidator::TrustedValidator(const std::string& theme, std::shared_ptr<SandboxResources> resources, DiagSink onDiag)
    : thread_("widget.validator"), onDiag_(std::move(onDiag)) {
    thread_.sync([&] {
        js_ = std::make_unique<QuickJSContext>(size_t(256) << 20, JSThread::kJSStackLimit);
        js_->registerFunction("__diag", [this](const std::vector<std::string>& args) {
            if (onDiag_) onDiag_(args.empty() ? "" : args[0], args.size() > 1 ? args[1] : "");
            return HostReturn::undefined();
        });
        // Precompiled when shipped and matching the source, else the source (same rule as the engine).
        bool fromBytecode = false;
        if (!resources->validatorBytecode.empty()) {
            try { js_->evaluateBytecode(resources->validatorBytecode, resources->validatorBundle); fromBytecode = true; }
            catch (const JSError& e) { if (onDiag_) onDiag_("bytecode", std::string("validator-bundle.js: falling back to source — ") + e.what()); }
        }
        if (!fromBytecode) js_->evaluate(resources->validatorBundle, "validator-bundle.js");
        // Flatten the API into globals so QuickJSContext::call can reach it.
        js_->evaluate(
            "globalThis.__v_processOps = (j) => __twValidator.processOps(j);"
            "globalThis.__v_validateQuery = (j) => __twValidator.validateQuery(j);"
            "globalThis.__v_validateListen = (j) => __twValidator.validateListen(j);"
            "globalThis.__v_validatePreventDefault = (j) => __twValidator.validatePreventDefault(j);"
            "globalThis.__v_setTheme = (t) => __twValidator.setTheme(t);"
            "globalThis.__v_rewriteMediaQuery = (q) => __twValidator.rewriteMediaQuery(q);"
            "globalThis.__v_nodeCount = () => __twValidator.nodeCount();"
            "globalThis.__v_create = (t) => { __twValidator.create(t); return ''; };",
            "validator-glue.js");
        js_->call("__v_create", {theme});
    });
}

TrustedValidator::~TrustedValidator() {
    thread_.sync([&] { js_.reset(); });
    thread_.join();
}

std::string TrustedValidator::str(const char* name, const std::vector<std::string>& args) {
    return thread_.sync([&]() -> std::string {
        try {
            return js_->call(name, args).string.value_or("");
        } catch (const JSError& e) {
            if (onDiag_) onDiag_("validator-error", std::string(name) + ": " + e.what());
            return "";
        }
    });
}

std::string TrustedValidator::processOps(const std::string& json) {
    std::string out = str("__v_processOps", {json});
    return out.empty() ? "[]" : out;
}

std::optional<std::string> TrustedValidator::validateQuery(const std::string& json) {
    std::string out = str("__v_validateQuery", {json});
    if (out.empty()) return std::nullopt;
    return out;
}

std::string TrustedValidator::validateListen(const std::string& json) { auto s = str("__v_validateListen", {json}); return s.empty() ? "[]" : s; }
std::string TrustedValidator::validatePreventDefault(const std::string& json) { auto s = str("__v_validatePreventDefault", {json}); return s.empty() ? "[]" : s; }
std::string TrustedValidator::setTheme(const std::string& theme) { auto s = str("__v_setTheme", {theme}); return s.empty() ? "[]" : s; }
std::string TrustedValidator::rewriteMediaQuery(const std::string& query) { return str("__v_rewriteMediaQuery", {query}); }
int TrustedValidator::nodeCount() {
    return thread_.sync([&]() -> int {
        try { return int(js_->call("__v_nodeCount").number.value_or(0)); } catch (const JSError&) { return 0; }
    });
}

} // namespace twidget
