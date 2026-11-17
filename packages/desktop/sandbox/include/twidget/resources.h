// The JavaScript shipped with the core (built by `pnpm build:desktop` into desktop/WidgetJS): the three bundles.
#pragma once

#include <memory>
#include <string>

namespace twidget {

class SandboxResources {
public:
    /// Reads `dir`/sandbox-bundle.js, validator-bundle.js and renderer-bundle.js (optional) with each CRLF pair as LF (a lone CR and LF input as they are). Throws std::runtime_error when the sandbox or validator bundle is missing.
    static std::shared_ptr<SandboxResources> fromDirectory(const std::string& dir);

    std::string sandboxBundle;
    std::string validatorBundle;
    std::string rendererBundle;
};

} // namespace twidget
