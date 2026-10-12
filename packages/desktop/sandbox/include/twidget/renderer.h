// The renderer as WidgetHost sees it: the JSON message contract of
// docs/protocol.md (host → renderer: ops / query / listen / preventDefault /
// requestFrame / frozen; renderer → host: ready / event / gesture / result /
// size / resize / frame / media / violation) behind a C++ interface. The
// desktop copy of Renderer.swift / Renderer.kt: on tdesktop the product
// renderer would be the Web App window; the Blitz prototype
// (twidget/blitz_renderer.h) plugs in through WidgetHost's factory.
//
// Threading: WidgetHost calls every member on its host thread (the stand-in
// for the UI thread). `onMessage` may be invoked from any thread — from inside
// `send` for a synchronous answer, or from a renderer's own clock for frames —
// and WidgetHost serialises it.
#pragma once

#include "twidget/resources.h"

#include <functional>
#include <memory>
#include <string>

namespace twidget {

class Renderer {
public:
    virtual ~Renderer() = default;

    /// Renderer → host messages (JSON).
    std::function<void(const std::string& json)> onMessage;
    std::function<void(const std::string& kind, const std::string& detail)> onDiag;

    /// Starts the renderer; it announces itself with `{type:'ready'}`.
    virtual void load() = 0;
    /// Host → renderer message (JSON).
    virtual void send(const std::string& json) = 0;
    /// The chat theme changed (`prefers-color-scheme` inside the renderer).
    virtual void setTheme(const std::string& theme) = 0;
    /// Width of the bubble in CSS px changed.
    virtual void setWidth(double width) = 0;
    virtual void destroy() = 0;
};

/// Builds a renderer for one widget: the shared resources, the chat theme and the width in CSS px.
using RendererFactory = std::function<std::unique_ptr<Renderer>(std::shared_ptr<SandboxResources> resources, const std::string& theme, double width)>;

} // namespace twidget
