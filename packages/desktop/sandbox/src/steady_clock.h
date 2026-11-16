// The engine's clock, one definition for the engine's deadlines (`SandboxEngine::nowSeconds`, sandbox_engine_shared.cpp) and the QuickJS interrupt handler that enforces them (`QuickJSContext::now`, quickjs_context.cpp), so the two never disagree; inline, so the host's side reads it without linking QuickJS. Library sources only; nothing under include/ names it.
#pragma once

#include <chrono>

namespace twidget::detail {

/// The steady clock in seconds since its epoch.
inline double steadySeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

} // namespace twidget::detail
