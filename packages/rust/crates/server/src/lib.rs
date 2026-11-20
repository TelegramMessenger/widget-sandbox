//! The engine process's end of the remote engine (`remote_engine.h`, `remote_stream.h`): the wire forms both
//! processes code with, the server that runs one client-mode engine's widget for its channel, and the loop that
//! serves an engine stream's runtimes (`SandboxRemoteEngineEnd::serve`).

#![forbid(unsafe_code)]

pub mod coding;
mod serve;
mod server;

pub use coding::SandboxRemoteOutcome;
pub use serve::serve;
pub use server::{RemoteIntl, SandboxRemoteServer, SandboxRemoteSink};
