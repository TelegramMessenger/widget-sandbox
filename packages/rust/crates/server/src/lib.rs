//! The engine process's end of the remote engine (`remote_engine.h`, `remote_stream.h`): the wire forms both
//! processes code with, the server that runs one client-mode engine's widget for its channel, and the loop that
//! serves an engine stream's runtimes (`SandboxRemoteEngineEnd::serve`) or a hosting stream's (`serveHosting`,
//! whose hosted runtimes `twidget_host` builds behind [`HostedRuntime`]).

#![forbid(unsafe_code)]

pub mod coding;
mod serve;
mod server;

pub use coding::SandboxRemoteOutcome;
pub use serve::{HostedRuntime, HostedRuntimes, serve, serve_hosting};
pub use server::{RemoteIntl, SandboxRemoteServer, SandboxRemoteSink};
