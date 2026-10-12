// The JavaScript shipped with the core (built by `pnpm build:desktop` into
// desktop/WidgetJS): the three bundles, the bundled libraries and the CDN URL
// patterns that map to them (same resolution as src/host/loader.ts).
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace twidget {

struct BundledLibrary {
    std::string name;
    std::string file;
    std::vector<std::regex> patterns;
};

class SandboxResources {
public:
    /// Reads `dir`/sandbox-bundle.js, validator-bundle.js, renderer-bundle.js
    /// (optional) and libraries.json. Throws std::runtime_error when a bundle is missing.
    static std::shared_ptr<SandboxResources> fromDirectory(const std::string& dir);

    std::string sandboxBundle;
    std::string validatorBundle;
    std::string rendererBundle;
    /// Precompiled bytecode of the bundles (`.qbc` next to the `.js`, built together; empty when not shipped).
    std::vector<uint8_t> sandboxBytecode;
    std::vector<uint8_t> validatorBytecode;
    std::vector<BundledLibrary> libraries;

    /// Known CDN URL → bundled copy; anything else → nullptr.
    const BundledLibrary* library(std::string_view url) const;
    /// Source of a bundled library (`dir`/libs/<file>), cached.
    std::optional<std::string> librarySource(const BundledLibrary& lib) const;
    /// Precompiled bytecode of a bundled library (`dir`/libs/<file>.qbc), cached; empty when not shipped.
    std::vector<uint8_t> libraryBytecode(const BundledLibrary& lib) const;

private:
    std::string dir_;
    mutable std::mutex mutex_;
    mutable std::map<std::string, std::string> cache_;
    mutable std::map<std::string, std::vector<uint8_t>> bytecodeCache_;
};

} // namespace twidget
