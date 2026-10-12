// The renderer trust domain of docs/protocol.md on litehtml: validated
// RenderOps in, layout answers and WidgetEvents out, a DisplayList on request.
// The C++ twin of renderer-blitz/src/renderer.rs with one structural
// difference: litehtml has no DOM mutation API, so the ops maintain a shadow
// tree here, every batch serialises it to HTML and the document is parsed and
// laid out again (the render()→string model). Nothing here parses untrusted HTML
// (the string is generated from validated ops) or runs script.
//
// One Renderer per widget, every call on the platform's main thread (the glue
// guarantees it), so the document needs no lock. Coordinates are CSS px.
#pragma once

#include "backend.h"
#include "display_list.h"
#include "twidget/json.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace twlite {

class Container;

/// Timings of the last rebuild (serialise → parse+layout, per pass).
struct BuildStats {
    double serialize_ms = 0, parse_ms = 0;
    int passes = 0;
    size_t html_bytes = 0;
    int builds = 0;
};

class Renderer {
public:
    /// `backend` is owned by the platform layer and must outlive the renderer.
    Renderer(double width, double dpr, bool dark, Backend& backend);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Host → renderer message (parsed JSON). Returns renderer → host messages.
    std::vector<twidget::Json> receive(const twidget::Json& msg);
    void set_width(double width, std::vector<twidget::Json>& out);
    /// The chat theme changed: `prefers-color-scheme` inside the document follows.
    void set_theme(bool dark, std::vector<twidget::Json>& out);
    /// A touch from the host view, in CSS px. `action`: 0 down, 1 move, 2 up, 3 cancel.
    std::vector<twidget::Json> touch(int action, double x, double y, int pointer, bool primary);
    /// A backend that decodes images asynchronously (the web) reports the size of image `id` here;
    /// the document is rebuilt with it. Returns renderer → host messages (a `size`, when it changed).
    std::vector<twidget::Json> image_ready(int id, float w, float h);

    /// Viewport in CSS px (the height the document converged to) and in physical px.
    double width() const { return width_; }
    int viewport_height() const { return vp_height_; }
    double dpr() const { return dpr_; }
    uint32_t px_width() const;
    uint32_t px_height() const;
    bool dirty() const { return dirty_; }
    /// Records the current document into a display list (clears `dirty`).
    DisplayList paint();
    const BuildStats& stats() const { return stats_; }
    /// The HTML of the last build (diagnostics).
    const std::string& last_html() const { return html_; }

private:
    friend class Container;
    struct Node;
    struct Touch {
        double x0, y0, x, y;
        int64_t target;
        Rect target_rect;
        bool moved;
        bool primary;
        int pointer;
    };

    // ops → shadow tree
    void apply_op(const twidget::Json& op, std::vector<twidget::Json>& out);
    Node* node(int64_t id) const;
    void adopt(int64_t id, Node* n, const twidget::Json& attrs);
    void detach(Node* n);
    void forget(Node* n);
    // shadow tree → HTML → litehtml
    std::string serialize() const;
    void serialize_node(const Node* n, std::string& out) const;
    void rebuild();
    void report_size(std::vector<twidget::Json>& out);
    // queries & events
    twidget::Json answer(const twidget::Json& q);
    bool match_media(const std::string& query) const;
    int64_t widget_id_at(double x, double y, Rect* placement) const;
    bool wants_gesture(int64_t target) const;
    void emit_event(const char* type, int64_t target, const Rect& target_rect, const Touch& t, int action, std::vector<twidget::Json>& out);
    void freeze(const std::string& reason, bool fatal);

    std::unique_ptr<Container> container_;
    double width_;
    double dpr_;
    bool dark_;
    int vp_height_;
    double doc_height_ = 0;
    bool dirty_ = true;
    std::string html_;
    BuildStats stats_;
    std::vector<std::string> pending_violations_;

    /// Widget nodes by id. html/head/body are fixed nodes owned below and reachable through `aliases_` once adopted.
    std::unordered_map<int64_t, std::unique_ptr<Node>> nodes_;
    std::unordered_map<int64_t, Node*> aliases_;
    std::unique_ptr<Node> html_node_, head_node_, body_node_;
    std::unordered_set<std::string> interest_;
    std::unordered_set<std::string> prevent_;
    bool frozen_ = false;
    std::string frozen_reason_;
    bool frozen_fatal_ = false;
    std::string last_size_;
    std::unordered_map<int, Touch> touches_;
};

/// Rewrites `prefers-color-scheme` media queries into width queries litehtml can
/// evaluate (`(min-width:0px)` = always, `(min-width:999999px)` = never), since
/// litehtml knows no colour-scheme feature. Exposed for the unit test.
std::string rewrite_color_scheme(const std::string& css, bool dark);

} // namespace twlite
