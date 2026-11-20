//! The desktop core's runtime under the engine: a dedicated thread per engine (`js_thread.h`) and the Nova
//! context on it (`nova_context.h`), calling nova-safe's `nova_embed` directly. What only the operating
//! system can answer (a thread's CPU time, the time zone, randomness) the embedder gives as a [`Platform`].

#![forbid(unsafe_code)]

mod context;
mod thread;

pub use context::{Deadlines, HostFailure, HostFunction, HostReturn, JsError, JsResult, NovaContext, Platform, now, repaired_utf8};
pub use thread::{JS_STACK_LIMIT, JsThread, STACK_SIZE, Scheduled};
