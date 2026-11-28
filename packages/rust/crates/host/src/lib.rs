//! The engine process's half of a hosted widget (`remote_hosted.h`, desktop core only): a widget's engine, its
//! trusted validator and its renderer all in the engine process, as the macOS helper runs them. The hosted
//! runtime (`SandboxHostedRuntime`, remote_hosted.cpp) builds them as a served `WidgetHost` (widget_host.h's
//! `ServedTag` constructor and widget_host_local.cpp's glue, what widget_host.cpp runs of it), routes that
//! engine's calls, keeps the renderer alone once that engine died, and carries the renderer's traffic with the
//! host's platform; [`serve_hosting`] serves a hosting stream's runtimes with them.
//!
//! The renderer is the embedder's ([`Renderer`], renderer.h's contract), built by its [`HostedRendererFactory`]
//! with the widget's [`HostedPlatform`]. The host's side of a hosted widget (the hosted client, a kept renderer's
//! handle, the platform wait) stays in the host's process and is not here.

#![forbid(unsafe_code)]

mod hosted;
mod renderer;
mod served;

pub use hosted::{HostedPlatform, HostedRendererFactory, SandboxHostedRuntime, serve_hosting};
pub use renderer::{
    MAX_BACKDROP_BOXES, MAX_HEAVY_BOXES, Renderer, WidgetPaintLimit, paint_limit_code, paint_limit_from_code, paint_limit_message,
};
pub use served::{GestureCounter, GlueDiagnostics, RENDERER_BACKLOG_BYTES, RENDERER_BACKLOG_MESSAGES, RendererLink, base64_decode};
