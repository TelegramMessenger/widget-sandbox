//! `renderer.h`: the renderer as the served host sees it, the JSON message contract of docs/protocol.md (host →
//! renderer: ops / query / listen / preventDefault / requestFrame / anim / theme / safeArea / policy / frozen;
//! renderer → host: ready / event / gesture / release / result / size / resize / frame / media / limit /
//! violation) behind a trait, and the paint limits a renderer reports through
//! [`RendererLink::limit_exceeded`](crate::RendererLink::limit_exceeded).
//!
//! Threading: the host calls every method on its host thread (the glue's `twidget-host`), and drops the
//! renderer on the thread that destroys the host once that thread was joined; the renderer's messages and
//! diagnostics (its [`RendererLink`](crate::RendererLink)) may come from any thread, from inside `send` for a
//! synchronous answer too, and the host serialises them.

use crate::served::RendererLink;

/// The paint limits a renderer enforces (packages/core/src/shared/paint-policy.ts): at most [`MAX_HEAVY_BOXES`]
/// heavy boxes at once, of which at most [`MAX_BACKDROP_BOXES`] with backdrop-filter.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WidgetPaintLimit {
    /// "paint-limit": more than `MAX_HEAVY_BOXES` heavy boxes; the count is the heavy boxes'.
    Paint,
    /// "backdrop-limit": more than `MAX_BACKDROP_BOXES` boxes with backdrop-filter; the count is theirs.
    Backdrop,
}

pub const MAX_HEAVY_BOXES: i32 = 16;
pub const MAX_BACKDROP_BOXES: i32 = 2;

pub fn paint_limit_code(code: WidgetPaintLimit) -> &'static str {
    match code {
        WidgetPaintLimit::Backdrop => "backdrop-limit",
        WidgetPaintLimit::Paint => "paint-limit",
    }
}

pub fn paint_limit_from_code(code: &[u8]) -> Option<WidgetPaintLimit> {
    match code {
        b"paint-limit" => Some(WidgetPaintLimit::Paint),
        b"backdrop-limit" => Some(WidgetPaintLimit::Backdrop),
        _ => None,
    }
}

/// The reason the host stops the widget with, the same on every platform (`paintLimitMessage` in
/// paint-policy.ts).
pub fn paint_limit_message(code: WidgetPaintLimit, count: i32) -> String {
    match code {
        WidgetPaintLimit::Backdrop => {
            format!("Paint limit exceeded: {count} boxes with backdrop-filter (max {MAX_BACKDROP_BOXES})")
        }
        WidgetPaintLimit::Paint => {
            format!("Paint limit exceeded: {count} boxes with shadows, filters or transparency (max {MAX_HEAVY_BOXES})")
        }
    }
}

/// One widget's renderer in the engine process (renderer.h's `Renderer`): whatever the embedder's factory
/// builds.
pub trait Renderer: Send + Sync {
    /// The host's ends of the renderer's messages and diagnostics (renderer.h's `onMessage` and `onDiag`, which
    /// the host sets once the factory returned): given once, on the thread that builds the host, before any other
    /// call; what the renderer reports before it goes nowhere.
    fn connect(&self, link: RendererLink);
    /// Starts the renderer; it announces itself with `{"type":"ready"}`.
    fn load(&self);
    /// Host → renderer message (JSON).
    fn send(&self, json: &[u8]);
    /// The chat theme changed (`prefers-color-scheme` inside the renderer).
    fn set_theme(&self, theme: &[u8]);
    /// Width of the bubble in CSS px changed.
    fn set_width(&self, width: f64);
    fn destroy(&self);
}
