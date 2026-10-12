#include "twidget/resources.h"

#include "twidget/json.h"

#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace twidget {

static std::optional<std::string> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string bytecodeName(const std::string& file) {
    return file.size() > 3 && file.compare(file.size() - 3, 3, ".js") == 0 ? file.substr(0, file.size() - 3) + ".qbc" : file + ".qbc";
}

std::shared_ptr<SandboxResources> SandboxResources::fromDirectory(const std::string& dir) {
    auto res = std::make_shared<SandboxResources>();
    res->dir_ = dir;
    auto need = [&](const char* name) {
        auto s = readFile(dir + "/" + name);
        if (!s) throw std::runtime_error(std::string("WidgetSandbox: missing resource ") + dir + "/" + name + " — run `pnpm build:desktop`");
        return *s;
    };
    res->sandboxBundle = need("sandbox-bundle.js");
    res->validatorBundle = need("validator-bundle.js");
    res->sandboxBytecode = readBytes(dir + "/sandbox-bundle.qbc");
    res->validatorBytecode = readBytes(dir + "/validator-bundle.qbc");
    res->rendererBundle = readFile(dir + "/renderer-bundle.js").value_or("");
    if (auto libs = readFile(dir + "/libraries.json")) {
        if (auto list = Json::parse(*libs)) {
            for (const auto& item : list->arr()) {
                BundledLibrary lib;
                lib.name = item["name"].asString();
                lib.file = item["file"].asString();
                if (lib.name.empty() || lib.file.empty()) continue;
                for (const auto& p : item["patterns"].arr()) {
                    try { lib.patterns.emplace_back(p.asString(), std::regex::ECMAScript | std::regex::icase); } catch (const std::regex_error&) {}
                }
                res->libraries.push_back(std::move(lib));
            }
        }
    }
    return res;
}

const BundledLibrary* SandboxResources::library(std::string_view url) const {
    std::string u(url);
    auto notSpace = [](unsigned char c) { return !(c == ' ' || c == '\t' || c == '\n' || c == '\r'); };
    while (!u.empty() && !notSpace((unsigned char)u.front())) u.erase(u.begin());
    while (!u.empty() && !notSpace((unsigned char)u.back())) u.pop_back();
    if (u.rfind("//", 0) == 0) u = "https:" + u;
    size_t cut = u.find_first_of("?#");
    if (cut != std::string::npos) u.resize(cut);
    for (const auto& lib : libraries)
        for (const auto& re : lib.patterns)
            if (std::regex_search(u, re)) return &lib;
    return nullptr;
}

std::optional<std::string> SandboxResources::librarySource(const BundledLibrary& lib) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(lib.file);
    if (it != cache_.end()) return it->second;
    auto s = readFile(dir_ + "/libs/" + lib.file);
    if (!s) return std::nullopt;
    cache_[lib.file] = *s;
    return *s;
}

std::vector<uint8_t> SandboxResources::libraryBytecode(const BundledLibrary& lib) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = bytecodeCache_.find(lib.file);
    if (it != bytecodeCache_.end()) return it->second;
    auto blob = readBytes(dir_ + "/libs/" + bytecodeName(lib.file));
    bytecodeCache_[lib.file] = blob;
    return blob;
}

} // namespace twidget
