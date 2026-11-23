//! The desktop core's sandbox engine in process (`sandbox_engine.h`, `sandbox_engine.cpp`): the widget's Nova
//! runtime on its own thread, the one capability its code gets, the limits, the values the host and the engine
//! exchange, and the trusted validator (`trusted_validator.h`).

#![forbid(unsafe_code)]

mod engine;
mod types;
mod validator;

pub use engine::{Engine, EngineDelegate, IntlProvider, SandboxEngine, WidgetRequestResponder};
pub use types::{
    QueryAnswer, SandboxConfig, SandboxHostedStart, SandboxStats, WidgetButtonTokens, WidgetContentSafeArea, WidgetSafeArea,
    WidgetTextTokens, WidgetTheme, is_strict_policy, surface,
};
pub use validator::{CpuSink, DiagSink, TrustedValidator, ValidatorError};
