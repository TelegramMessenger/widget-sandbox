// Console harness for the desktop core (the C++ twin of scripts/sandbox-node.ts).
//
//   twidget-run <widget.html> [--verbose] [--ops] [--events '<json>']... [--wait-ms N] [--probe EXPR]... [--theme dark]
//               [--theme-params '{"bg_color":"#212121",…}'] [--locale ru] [--storage '{"local":{…},"session":{…}}']
//                                 SDK v1 (docs/sdk-v1.md §3.1): the client's palette, UI language and Web Storage
//                                 preload; the run prints the actions (`onAction`), the storage commits and the
//                                 resize anchor when the widget called `setResizeAnchor`
//   twidget-run --attacks [DIR]   every attack file: nothing but data:/#fragment URLs may reach the validated ops
//   twidget-run --corpus DIR      smoke run of a corpus directory: loaded, alive, no uncaught errors
//   … --blitz [--png out.png | --png-dir DIR] [--click X,Y]... [--dpr N]
//                                 the same through WidgetHost + the Blitz renderer (-DTW_BLITZ=ON): real layout
//                                 answers, pointer events hit-tested by the document, a raster of the result
//
// Without --blitz, layout queries get canned answers (no renderer), so chart
// libraries lay out against a 100×20 box; what is checked is the sandbox, the
// validator and the limits — not pixels. With --blitz the validated ops go to
// the Rust document and a fetch attempt from it is counted as a leak too.

#include "png.h"
#include "recording_host.h"
#ifdef TW_HAVE_BLITZ
#include "twidget/blitz_renderer.h"
#endif
#ifdef TW_HAVE_LITEHTML
#include "twidget/litehtml_renderer.h"
#endif
#if defined(TW_HAVE_BLITZ) || defined(TW_HAVE_LITEHTML)
#include "twidget/widget_host.h"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace twidget;
namespace fs = std::filesystem;

static std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Options {
    std::string jsDir = TW_WIDGET_JS;
    std::string theme = "light";
    std::map<std::string, std::string> themeParams;
    std::string locale = std::string(sdk::DEFAULT_LOCALE);
    WidgetStorage storage;
    double width = 400;
    int waitMs = 1500;
    bool verbose = false;
    bool printOps = false;
    std::vector<std::string> events;
    std::vector<std::string> probes;
    // --blitz / --litehtml
    bool blitz = false;
    bool litehtml = false;
    double dpr = 1;
    std::string png, pngDir;
    std::vector<std::pair<double, double>> clicks;
};

struct RunOutcome {
    bool loaded = false;
    std::optional<std::string> died;
    int batches = 0, renderOps = 0, elements = 0, queries = 0, errors = 0;
    std::map<std::string, int> diagCounts;
    std::vector<std::string> leaks;
    std::vector<std::string> notes;
    std::vector<std::pair<std::string, std::string>> logs;
    std::vector<WidgetAction> actions;
    std::vector<std::pair<std::string, StorageArea>> storageCommits;
    /// The widget's latest `setResizeAnchor` ("top" until then) and, without a native renderer, each change in order.
    std::string resizeAnchor = std::string(sdk::DEFAULT_RESIZE_ANCHOR);
    std::vector<std::string> resizeAnchors;
    std::vector<std::string> probeResults;
    std::string renderOpsJson;
    SandboxStats stats;
    // --blitz
    double height = 0, paintMs = -1, initMs = 0, loadedMs = 0, renderedMs = 0;
    uint32_t frameW = 0, frameH = 0;
    std::vector<uint8_t> frame;
};

/// Anything in the validated ops that could make a renderer touch the network.
///
/// `data:` payloads are opaque to the renderer's network stack: an SVG loaded as
/// an image may not fetch external resources (SVG-as-image rules in every
/// engine), so a URL *inside* a data: URL is reported as a note, not a leak —
/// the browser attack suite confirms zero canary hits for exactly these files.
/// In CSS only `url()`-family functions and `@import` can trigger a fetch; a
/// string token holding "http://…" (e.g. in a custom property) is inert.
static bool mentionsNetwork(std::string l) {
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return l.find("127.0.0.1") != std::string::npos || l.find("http://") != std::string::npos || l.find("https://") != std::string::npos ||
           l.find("ws://") != std::string::npos || l.find("wss://") != std::string::npos || l.rfind("//", 0) == 0;
}

static void scanCssForLeaks(const std::string& where, const std::string& css, std::vector<std::string>& leaks) {
    std::string l = css;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (l.find("@import") != std::string::npos) leaks.push_back(where + ": @import survived: " + css.substr(0, 160));
    static const char* const fns[] = {"url(", "image-set(", "image(", "src(", "cross-fade("};
    for (const char* fn : fns) {
        size_t len = std::strlen(fn);
        for (size_t pos = l.find(fn); pos != std::string::npos; pos = l.find(fn, pos + len)) {
            if (pos > 0 && (std::isalnum((unsigned char)l[pos - 1]) || l[pos - 1] == '-')) continue; // e.g. "-webkit-image(" prefixes handled by their own match, "burl(" is not url(
            size_t a = pos + len;
            while (a < l.size() && (l[a] == ' ' || l[a] == '\t' || l[a] == '\n' || l[a] == '"' || l[a] == '\'')) a++;
            std::string arg = l.substr(a, 12);
            if (arg.rfind("data:", 0) == 0 || arg.rfind("#", 0) == 0) continue;
            if (std::strcmp(fn, "image-set(") == 0 && (arg.rfind("url(", 0) == 0 || arg.rfind("image(", 0) == 0)) continue; // nested, checked by its own match
            leaks.push_back(where + ": " + css.substr(pos, 160));
        }
    }
}

static void scanForLeaks(const Json& op, std::vector<std::string>& leaks, std::vector<std::string>& notes) {
    const std::string& kind = op["op"].asString();
    auto flagAttr = [&](const std::string& where, const std::string& value) {
        if (value.rfind("data:", 0) == 0) { if (mentionsNetwork(value)) notes.push_back(where + " (inert URL inside a data: payload): " + value.substr(0, 120)); return; }
        if (mentionsNetwork(value)) leaks.push_back(where + ": " + value.substr(0, 160));
    };
    if (kind == "el") {
        for (const auto& pair : op["attrs"].arr()) {
            const std::string& name = pair[0].asString();
            if (name == "style") scanCssForLeaks("el.style", pair[1].asString(), leaks);
            else flagAttr("el." + name, pair[1].asString());
        }
    } else if (kind == "attr") {
        if (op["name"].asString() == "style") scanCssForLeaks("attr style", op["value"].asString(), leaks);
        else flagAttr("attr " + op["name"].asString(), op["value"].asString());
    } else if (kind == "style") {
        scanCssForLeaks("style " + op["name"].asString(), op["value"].asString(), leaks);
    } else if (kind == "css") {
        scanCssForLeaks("css", op["text"].asString(), leaks);
    } else if (kind == "canvas") {
        for (const auto& cmd : op["cmds"].arr())
            for (const auto& a : cmd.arr()) if (a.isString()) { scanCssForLeaks("canvas", a.asString(), leaks); flagAttr("canvas", a.asString()); }
    }
}

#if defined(TW_HAVE_BLITZ) || defined(TW_HAVE_LITEHTML)
/// WidgetHost + a native renderer (BlitzRenderer or LitehtmlRenderer, the same owner contract): the pipeline
/// the demos run, with the native document answering layout queries.
template <typename NativeRenderer>
static RunOutcome runWidgetNative(const std::string& html, const Options& opts, std::shared_ptr<SandboxResources> res) {
    RunOutcome out;
    std::vector<std::string> renderOps;
    WidgetHostOptions ho;
    ho.devicePixelRatio = opts.dpr;
    ho.locale = opts.locale;
    ho.storage = opts.storage;
    NativeRenderer* blitz = nullptr;
    WidgetTheme theme{opts.theme, opts.themeParams, {}};
    WidgetHost host(html, theme, opts.width, ho, res, [&](std::shared_ptr<SandboxResources>, const std::string& scheme, double width) -> std::unique_ptr<Renderer> {
        auto r = std::make_unique<NativeRenderer>(scheme, width, opts.dpr);
        blitz = r.get();
        return r;
    });
    host.onRenderOps = [&](const std::string& clean) {
        renderOps.push_back(clean);
        if (opts.verbose) fprintf(stderr, "ops → %s\n", clean.size() > 2000 ? (clean.substr(0, 2000) + "…").c_str() : clean.c_str());
    };
    // On the host thread; read after destroy(), when the host thread has drained.
    host.onStorageCommit = [&](const std::string& area, const StorageArea& values) { out.storageCommits.emplace_back(area, values); };
    host.start();
    host.waitLoaded(30);
    host.waitRendered(5);
    for (const auto& ev : opts.events) host.event(ev);
    for (const auto& [x, y] : opts.clicks) blitz->click(x, y);
    std::this_thread::sleep_for(std::chrono::milliseconds(opts.waitMs));
    for (const auto& p : opts.probes) out.probeResults.push_back(host.probe(p));
    typename NativeRenderer::Frame frame = blitz->paint();
    out.initMs = blitz->initMs();
    WidgetDiagnostics d = host.diagnostics();
    host.destroy();

    out.loaded = d.loaded;
    out.died = d.dead;
    if (out.died && *out.died == "terminated") out.died.reset();
    out.batches = int(renderOps.size());
    for (const auto& b : renderOps) {
        if (auto arr = Json::parse(b)) {
            for (const auto& op : arr->arr()) {
                out.renderOps++;
                if (op["op"].asString() == "el") out.elements++;
                scanForLeaks(op, out.leaks, out.notes);
            }
        }
        out.renderOpsJson += b;
    }
    out.queries = d.stats.queries;
    for (const auto& [kind, entry] : d.diags) {
        out.diagCounts[kind] = entry.first;
        // The Rust side refuses every non-data: URL; with the validator in front it must never see one.
        if (kind == "renderer-violation")
            for (const auto& detail : entry.second) if (detail.rfind("fetch refused", 0) == 0) out.leaks.push_back("renderer " + detail);
    }
    for (const auto& lg : d.logs) if (lg.first == "error") out.errors++;
    out.logs = d.logs;
    out.actions = d.actions;
    out.resizeAnchor = d.resizeAnchor;
    out.stats = d.stats;
    out.height = d.height;
    out.loadedMs = d.loadedMs.value_or(0);
    out.renderedMs = d.renderedMs.value_or(0);
    out.paintMs = frame.paintMs;
    out.frameW = frame.width;
    out.frameH = frame.height;
    out.frame = std::move(frame.rgba);
    return out;
}
#endif

static RunOutcome runWidget(const std::string& html, const Options& opts, std::shared_ptr<SandboxResources> res, SandboxConfig config) {
#ifdef TW_HAVE_BLITZ
    if (opts.blitz) return runWidgetNative<BlitzRenderer>(html, opts, res);
#endif
#ifdef TW_HAVE_LITEHTML
    if (opts.litehtml) return runWidgetNative<LitehtmlRenderer>(html, opts, res);
#endif
    RunOutcome out;
    RecordingHost host(opts.theme, res, opts.verbose);
    {
        SandboxEngine engine(html, config, res);
        engine.setDelegate(&host);
        engine.start();
        host.waitLoaded(30);
        // An event with a numeric `gesture` is a trusted press (as the Node harness's `gesture: n`); the rest are untrusted.
        for (const auto& ev : opts.events) {
            auto parsed = Json::parse(ev);
            if (parsed && (*parsed)["gesture"].isNumber() && (*parsed)["gesture"].asInt() > 0) engine.activationEvent(ev, (*parsed)["gesture"].asInt());
            else engine.event(ev);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(opts.waitMs));
        for (const auto& p : opts.probes) out.probeResults.push_back(engine.probe(p));
        engine.terminate();
    }
    auto l = host.lock();
    out.loaded = host.loaded;
    out.died = host.died;
    if (out.died && *out.died == "terminated") out.died.reset();
    out.batches = int(host.ops.size());
    for (const auto& b : host.renderOps) {
        if (auto arr = Json::parse(b)) {
            for (const auto& op : arr->arr()) {
                out.renderOps++;
                if (op["op"].asString() == "el") out.elements++;
                scanForLeaks(op, out.leaks, out.notes);
            }
        }
        out.renderOpsJson += b;
    }
    out.queries = int(host.queries.size());
    for (const auto& d : host.diags) out.diagCounts[d.first]++;
    for (const auto& lg : host.logs) if (lg.first == "error") out.errors++;
    out.logs = host.logs;
    out.actions = host.actions;
    out.storageCommits = host.storageCommits;
    out.resizeAnchors = host.resizeAnchors;
    if (!host.resizeAnchors.empty()) out.resizeAnchor = host.resizeAnchors.back();
    out.stats = host.stats;
    return out;
}

/// `--theme-params` / `--storage`: a JSON object of strings (the engine checks keys, colours and the quota).
static bool stringMap(const Json& v, std::map<std::string, std::string>& out) {
    if (!v.isObject()) return false;
    for (const auto& [k, x] : v.obj()) {
        if (!x.isString()) return false;
        out[k] = x.asString();
    }
    return true;
}

static std::string diagSummary(const std::map<std::string, int>& counts) {
    std::string s;
    for (const auto& [k, n] : counts) {
        if (k == "boot" || k == "library") continue;
        if (!s.empty()) s += ", ";
        s += k + (n > 1 ? "×" + std::to_string(n) : "");
    }
    return s;
}

/// `--png` / `--png-dir`: the Blitz raster of a run, if any; returns what was written for the report.
static std::string writePng(const RunOutcome& r, const std::string& path) {
    if (path.empty() || r.frame.empty()) return "";
    fs::create_directories(fs::path(path).parent_path().empty() ? fs::path(".") : fs::path(path).parent_path());
    return png::write(path, r.frameW, r.frameH, r.frame) ? path : "(png write failed: " + path + ")";
}

static std::vector<fs::path> htmlFiles(const fs::path& dir) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) if (e.path().extension() == ".html") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    return files;
}

int main(int argc, char** argv) {
    Options opts;
    std::string file, attacksDir, corpusDir;
    bool attacks = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--verbose") opts.verbose = true;
        else if (a == "--ops") opts.printOps = true;
        else if (a == "--events") opts.events.push_back(next());
        else if (a == "--probe" || a == "--eval") opts.probes.push_back(next());
        else if (a == "--wait-ms") opts.waitMs = std::atoi(next().c_str());
        else if (a == "--theme") opts.theme = next();
        else if (a == "--theme-params") {
            auto v = Json::parse(next());
            if (!v || !stringMap(*v, opts.themeParams)) { fprintf(stderr, "--theme-params wants a JSON object of strings\n"); return 2; }
        }
        else if (a == "--locale") opts.locale = next();
        else if (a == "--storage") {
            auto v = Json::parse(next());
            bool ok = v && v->isObject();
            for (const auto& [area, values] : ok ? v->obj() : Json::Object{})
                ok = ok && (area == "local" || area == "session") && stringMap(values, area == "local" ? opts.storage.local : opts.storage.session);
            if (!ok) { fprintf(stderr, "--storage wants {\"local\": {…}, \"session\": {…}} with string values\n"); return 2; }
        }
        else if (a == "--width") opts.width = std::atof(next().c_str());
        else if (a == "--js-dir") opts.jsDir = next();
        else if (a == "--blitz") opts.blitz = true;
        else if (a == "--litehtml") opts.litehtml = true;
        else if (a == "--dpr") opts.dpr = std::atof(next().c_str());
        else if (a == "--png") opts.png = next();
        else if (a == "--png-dir") opts.pngDir = next();
        else if (a == "--click") { std::string v = next(); size_t c = v.find(','); if (c == std::string::npos) { fprintf(stderr, "--click wants X,Y\n"); return 2; } opts.clicks.emplace_back(std::atof(v.substr(0, c).c_str()), std::atof(v.substr(c + 1).c_str())); }
        else if (a == "--attacks") { attacks = true; if (i + 1 < argc && argv[i + 1][0] != '-') attacksDir = argv[++i]; }
        else if (a == "--corpus") corpusDir = next();
        else if (!a.empty() && a[0] == '-') { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
        else file = a;
    }
    if (file.empty() && !attacks && corpusDir.empty()) {
        fprintf(stderr, "usage: twidget-run <widget.html> [--verbose] [--ops] [--events JSON] [--wait-ms N] [--probe EXPR]\n"
                        "                   [--theme dark] [--theme-params JSON] [--locale TAG] [--storage JSON]   (an event with \"gesture\": n is a trusted press)\n"
                        "       twidget-run --attacks [DIR] | --corpus DIR\n"
                        "       … --blitz | --litehtml [--png FILE | --png-dir DIR] [--click X,Y] [--dpr N]   (built with -DTW_BLITZ=ON / -DTW_LITEHTML=ON)\n");
        return 2;
    }
#ifndef TW_HAVE_BLITZ
    if (opts.blitz) { fprintf(stderr, "--blitz needs a build with -DTW_BLITZ=ON (rustup's cargo; see packages/desktop/README.md)\n"); return 2; }
#endif
#ifndef TW_HAVE_LITEHTML
    if (opts.litehtml) { fprintf(stderr, "--litehtml needs a build with -DTW_LITEHTML=ON (macOS; see packages/desktop/README.md)\n"); return 2; }
#endif
    if (opts.blitz && opts.litehtml) { fprintf(stderr, "--blitz and --litehtml are exclusive\n"); return 2; }
    std::shared_ptr<SandboxResources> res;
    try { res = SandboxResources::fromDirectory(opts.jsDir); } catch (const std::exception& e) { fprintf(stderr, "%s\n", e.what()); return 2; }
    SandboxConfig config;
    config.innerWidth = opts.width;
    config.theme = opts.theme;
    config.themeParams = opts.themeParams;
    config.locale = opts.locale;
    config.storage = opts.storage;

    if (attacks) {
        fs::path dir = attacksDir.empty() ? fs::path(TW_CORPUS) / "attacks" : fs::path(attacksDir);
        auto files = htmlFiles(dir);
        int leaks = 0, dead = 0;
        printf("%-40s %-8s %s\n", "attack", "status", "diagnostics");
        for (const auto& f : files) {
            // One click so click-triggered vectors fire too (targetId 2 = <html>; events bubble from the document).
            Options o = opts;
            o.events.push_back("{\"type\":\"click\",\"targetId\":2,\"clientX\":5,\"clientY\":5}");
            RunOutcome r = runWidget(readFile(f), o, res, config);
            std::string status = !r.leaks.empty() ? "LEAK" : r.died ? "dead" : "ok";
            if (!r.leaks.empty()) leaks++;
            if (r.died) dead++;
            std::string extra = r.died ? " — " + r.died->substr(0, 90) : "";
            printf("%-40s %-8s %s%s\n", f.filename().string().c_str(), status.c_str(), diagSummary(r.diagCounts).c_str(), extra.c_str());
            for (const auto& leak : r.leaks) printf("    LEAK %s\n", leak.c_str());
            for (const auto& note : r.notes) printf("    note %s\n", note.c_str());
            if (!opts.pngDir.empty()) writePng(r, (fs::path(opts.pngDir) / (f.stem().string() + ".png")).string());
            fflush(stdout);
        }
        printf("\n%zu attacks, %d with URLs leaking into validated ops%s, %d runtimes killed by limits\n", files.size(), leaks,
               (opts.blitz || opts.litehtml) ? " or fetched by the renderer" : "", dead);
        return leaks ? 1 : 0;
    }

    if (!corpusDir.empty()) {
        auto files = htmlFiles(corpusDir);
        int pass = 0;
        const bool native = opts.blitz || opts.litehtml;
        if (native) printf("%-32s %-6s %6s %6s %5s %4s %6s %6s %7s  %s\n", "widget", "status", "ops", "els", "qry", "err", "height", "paint", "loaded", "diagnostics");
        else printf("%-32s %-6s %6s %6s %5s %4s  %s\n", "widget", "status", "ops", "els", "qry", "err", "diagnostics");
        for (const auto& f : files) {
            RunOutcome r = runWidget(readFile(f), opts, res, config);
            bool ok = r.loaded && !r.died && r.errors == 0 && r.leaks.empty();
            if (ok) pass++;
            std::string status = !r.leaks.empty() ? "LEAK" : r.died ? "dead" : r.errors ? "errors" : "ok";
            if (native)
                printf("%-32s %-6s %6d %6d %5d %4d %6.0f %5.1fms %5.0fms  %s%s\n", f.filename().string().c_str(), status.c_str(), r.renderOps, r.elements, r.queries, r.errors,
                       r.height, r.paintMs, r.loadedMs, diagSummary(r.diagCounts).c_str(), r.died ? (" — " + r.died->substr(0, 80)).c_str() : "");
            else
                printf("%-32s %-6s %6d %6d %5d %4d  %s%s\n", f.filename().string().c_str(), status.c_str(), r.renderOps, r.elements, r.queries, r.errors,
                       diagSummary(r.diagCounts).c_str(), r.died ? (" — " + r.died->substr(0, 80)).c_str() : "");
            if (!opts.pngDir.empty()) writePng(r, (fs::path(opts.pngDir) / (f.stem().string() + ".png")).string());
            if (opts.verbose) for (const auto& lg : r.logs) if (lg.first == "error") printf("    %s\n", lg.second.substr(0, 300).c_str());
            fflush(stdout);
        }
        printf("\n%d / %zu loaded without errors\n", pass, files.size());
        return pass == int(files.size()) ? 0 : 1;
    }

    RunOutcome r = runWidget(readFile(file), opts, res, config);
    for (size_t i = 0; i < r.probeResults.size(); i++) printf("probe[%zu] %s → %s\n", i, opts.probes[i].c_str(), r.probeResults[i].c_str());
    if (opts.printOps) printf("%s\n", r.renderOpsJson.c_str());
    printf("\nbatches %d, render ops %d (%d elements), queries %d, tasks %d, js %.0f ms, memory %zu KB%s\n", r.batches, r.renderOps, r.elements, r.queries,
           r.stats.tasks, r.stats.jsMs, r.stats.memoryBytes / 1024, r.died ? (" — DEAD: " + *r.died).c_str() : "");
    if (opts.blitz || opts.litehtml) {
        printf("%-6s height %.0f px, frame %u×%u px, paint %.1f ms, init %.1f ms, loaded %.0f ms, rendered %.0f ms\n", opts.blitz ? "blitz" : "lite", r.height, r.frameW, r.frameH, r.paintMs, r.initMs, r.loadedMs, r.renderedMs);
        std::string written = writePng(r, opts.png);
        if (!written.empty()) printf("png    %s\n", written.c_str());
    }
    for (const auto& [k, n] : r.diagCounts) printf("diag  %s ×%d\n", k.c_str(), n);
    for (const auto& lg : r.logs) printf("[%s] %s\n", lg.first.c_str(), lg.second.c_str());
    for (const auto& a : r.actions) printf("action %s\n", a.toJson().dump().c_str());
    if (!r.resizeAnchors.empty() || r.resizeAnchor != sdk::DEFAULT_RESIZE_ANCHOR) {
        std::string calls;
        for (const auto& e : r.resizeAnchors) calls += (calls.empty() ? "" : " → ") + e;
        printf("resize anchor: %s%s: the %s edge of the bubble stays put when the height changes\n", r.resizeAnchor.c_str(),
               calls.empty() ? "" : (" (setResizeAnchor: " + calls + ")").c_str(), r.resizeAnchor.c_str());
    }
    for (const auto& [area, values] : r.storageCommits) {
        Json v = Json::object();
        for (const auto& [k, x] : values) v.set(k, x);
        std::string text = v.dump();
        printf("storage %s %s\n", area.c_str(), text.size() > 500 ? (text.substr(0, 500) + "…").c_str() : text.c_str());
    }
    for (const auto& leak : r.leaks) printf("LEAK %s\n", leak.c_str());
    return r.leaks.empty() ? 0 : 1;
}
