// What both WidgetHost sources use (twidget/widget_host.h): the theme and the config a host keeps, and the death of an engine past the renderer's backlog bounds. Library sources only, widget_host.cpp (what every host runs, the hosted mode) and widget_host_local.cpp (the local mode and the served host); nothing under include/ names it.
#pragma once

#include "twidget/sandbox_engine.h"
#include "twidget/sdk_policy.h"
#include "twidget/widget_host.h"

#include <utility>

namespace twidget::detail {

/// The theme as the host keeps it: "light" unless exactly "dark" (toWidgetTheme in sdk-policy.ts), the palette checked.
inline WidgetTheme normalized(WidgetTheme t) {
    t.colorScheme = t.colorScheme == "dark" ? "dark" : "light";
    t.params = sdk::checkThemeParams(t.params);
    return t;
}

/// `c` with the scheme and the palette of `theme`, a theme as the host keeps it.
inline SandboxConfig themed(SandboxConfig c, const WidgetTheme& theme) {
    c.theme = theme.colorScheme;
    c.themeParams = theme.params;
    return c;
}

inline SandboxConfig makeConfig(const WidgetHostOptions& o, double width, const WidgetTheme& theme) {
    SandboxConfig c;
    c.memoryLimitBytes = o.memoryLimitBytes;
    c.cpuLimitMs = o.cpuLimitMs;
    c.cpuQuotaMs = o.cpuQuotaMs;
    c.cpuQuotaWindowMs = o.cpuQuotaWindowMs;
    c.cpuKillAfterMs = o.cpuKillAfterMs;
    c.mainThreadQuotaMs = o.mainThreadQuotaMs;
    c.maxTimers = o.maxTimers;
    c.visible = o.visible;
    c.innerWidth = width;
    c.innerHeight = 300;
    c.devicePixelRatio = o.devicePixelRatio;
    c.locale = o.locale;
    c.contentSafeArea = o.contentSafeArea;
    c.safeArea = o.safeArea;
    c.storage = o.storage;
    c.data = o.data;
    c.context = o.context;
    c.policy = o.policy;
    c.features = o.features;
    return themed(std::move(c), theme);
}

/// The death of a client-mode engine whose renderer messages passed WidgetHost's backlog bounds (its diagnostic is widget_host_local.cpp's `kBacklogLimit`).
inline constexpr const char* kBacklogLimitReason = "the widget engine process sent more than the host keeps before its renderer is ready";

} // namespace twidget::detail
