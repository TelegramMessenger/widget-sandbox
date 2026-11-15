#include "twidget/resources.h"

#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace twidget {

static std::optional<std::string> readSource(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string source = ss.str();
    // A checkout may give the bundles CRLF line ends (Git's core.autocrlf); their stamp and `.qbc` are of the LF source.
    if (source.find("\r\n") != std::string::npos) {
        size_t kept = 0;
        for (size_t i = 0; i < source.size(); i++) {
            if (source[i] != '\r' || i + 1 == source.size() || source[i + 1] != '\n') source[kept++] = source[i];
        }
        source.resize(kept);
    }
    return source;
}

static std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::shared_ptr<SandboxResources> SandboxResources::fromDirectory(const std::string& dir) {
    auto res = std::make_shared<SandboxResources>();
    auto need = [&](const char* name) {
        auto s = readSource(dir + "/" + name);
        if (!s) throw std::runtime_error(std::string("WidgetSandbox: missing resource ") + dir + "/" + name + " — run `pnpm build:desktop`");
        return *s;
    };
    res->sandboxBundle = need("sandbox-bundle.js");
    res->validatorBundle = need("validator-bundle.js");
    res->sandboxBytecode = readBytes(dir + "/sandbox-bundle.qbc");
    res->validatorBytecode = readBytes(dir + "/validator-bundle.qbc");
    res->rendererBundle = readSource(dir + "/renderer-bundle.js").value_or("");
    return res;
}

} // namespace twidget
