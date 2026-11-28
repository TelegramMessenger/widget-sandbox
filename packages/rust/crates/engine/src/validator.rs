//! `trusted_validator.h`, `trusted_validator.cpp`: the validator / sanitizer (src/host/validator.ts) compiled
//! into validator-bundle.js and run in a trusted Nova context, a runtime that never runs widget code, on its own
//! thread (`widget.validator`; css-tree needs the 8 MB stack). One per widget; every call waits for its answer.
//!
//! Diagnostic `limit` is fatal: a counted cap was passed under strict, the batch came back empty and every later
//! one does too. The owner stops the widget with `stopped()` as the reason, as it does for memory or CPU.

use std::cell::RefCell;
use std::sync::{Arc, Mutex};

use twidget_bundle_stamp::{BundleStamp, SandboxResources};
use twidget_js::{HostReturn, JS_STACK_LIMIT, JsError, JsThread, NovaContext, Platform};

/// The diagnostics of the validator's bundle, `(kind, detail)`, on the validator's thread.
pub type DiagSink = Arc<dyn Fn(&[u8], &[u8]) + Send + Sync>;
/// The CPU time (ms, the validator thread's clock) of each call made for the widget's messages, on the
/// validator's thread.
pub type CpuSink = Box<dyn Fn(f64) + Send>;

const GLUE: &[u8] = b"globalThis.__v_processOps = (j) => __twValidator.processOps(j);\
globalThis.__v_validateQuery = (j) => __twValidator.validateQuery(j);\
globalThis.__v_validateListen = (j) => __twValidator.validateListen(j);\
globalThis.__v_validatePreventDefault = (j) => __twValidator.validatePreventDefault(j);\
globalThis.__v_validateAnim = (j) => __twValidator.validateAnim(j);\
globalThis.__v_setTheme = (t) => __twValidator.setTheme(t);\
globalThis.__v_rewriteMediaQuery = (q) => __twValidator.rewriteMediaQuery(q);\
globalThis.__v_nodeCount = () => __twValidator.nodeCount();\
globalThis.__v_markLoaded = () => { __twValidator.markLoaded(); return ''; };\
globalThis.__v_create = (t) => { __twValidator.create(t); return ''; };\
globalThis.__v_setPolicy = (p) => __twValidator.setPolicy ? __twValidator.setPolicy(p) : '';";

/// Why a validator could not be built: its thread did not start, or its bundle failed to load.
#[derive(Clone, Debug, PartialEq)]
pub enum ValidatorError {
    Thread(String),
    Js(JsError),
}

struct ValidatorState {
    js: RefCell<Option<NovaContext>>,
    cpu_sink: RefCell<Option<CpuSink>>,
}

/// One widget's validator.
pub struct TrustedValidator {
    thread: JsThread<ValidatorState>,
    on_diag: Option<DiagSink>,
    stopped: Arc<Mutex<Option<Vec<u8>>>>,
    platform: Arc<dyn Platform>,
}

impl std::fmt::Debug for TrustedValidator {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("TrustedValidator").field("thread", &self.thread).finish()
    }
}

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

impl TrustedValidator {
    /// The bundle's error when it fails to load, the thread's refusal when it cannot start.
    pub fn new(
        theme: &[u8],
        resources: Arc<SandboxResources>,
        on_diag: Option<DiagSink>,
        platform: Arc<dyn Platform>,
    ) -> Result<TrustedValidator, ValidatorError> {
        let thread = JsThread::new("widget.validator", || ValidatorState { js: RefCell::new(None), cpu_sink: RefCell::new(None) })
            .map_err(ValidatorError::Thread)?;
        let stopped = Arc::new(Mutex::new(None));
        let (sink, flag, theme, boot_platform) = (on_diag.clone(), Arc::clone(&stopped), theme.to_vec(), Arc::clone(&platform));
        let booted = thread.sync(move |s: &ValidatorState| -> Result<(), JsError> {
            // Kept local until the boot is over: a boot that fails frees the context here, on its own thread.
            let mut js = NovaContext::new(256 << 20, JS_STACK_LIMIT, boot_platform)?;
            let diag = sink.clone();
            js.register_function(
                "__diag",
                Box::new(move |args| {
                    let empty = Vec::new();
                    let (kind, detail) = (args.first().unwrap_or(&empty), args.get(1).unwrap_or(&empty));
                    if kind == b"limit" {
                        // Fatal: the owner reads it once the call returns and stops the widget.
                        let mut stopped = lock(&flag);
                        if stopped.is_none() {
                            *stopped = Some(detail.clone());
                        }
                    }
                    if let Some(d) = &diag {
                        d(kind, detail);
                    }
                    Ok(HostReturn::Undefined)
                }),
            )?;
            // Bundles of another release are refused before anything runs: no bundle, every call answers empty
            // (`validator-error`).
            if let Some(refusal) = BundleStamp::refusal_of(&resources) {
                if let Some(d) = &sink {
                    d(b"bundle-mismatch", &refusal);
                }
                *s.js.borrow_mut() = Some(js);
                return Ok(());
            }
            js.evaluate_bundle(&resources.validator_bundle, "validator-bundle.js")?;
            // Flatten the API into globals so `call` can reach it.
            js.evaluate(GLUE, false)?;
            js.call("__v_create", &[&theme])?;
            *s.js.borrow_mut() = Some(js);
            Ok(())
        });
        let validator = TrustedValidator { thread, on_diag, stopped, platform };
        match booted {
            Some(Ok(())) => Ok(validator),
            Some(Err(e)) => Err(ValidatorError::Js(e)),
            None => Err(ValidatorError::Thread("JSThread widget.validator: stopped".into())),
        }
    }

    fn str(&self, name: &'static str, args: Vec<Vec<u8>>) -> Vec<u8> {
        let on_diag = self.on_diag.clone();
        self.thread.sync(move |s: &ValidatorState| call(s, name, &args, on_diag.as_ref())).unwrap_or_default()
    }

    /// `str` measured for the CPU sink.
    fn metered(&self, name: &'static str, args: Vec<Vec<u8>>) -> Vec<u8> {
        let on_diag = self.on_diag.clone();
        let platform = Arc::clone(&self.platform);
        self.thread
            .sync(move |s: &ValidatorState| {
                if s.cpu_sink.borrow().is_none() {
                    return call(s, name, &args, on_diag.as_ref());
                }
                let t0 = platform.thread_cpu_seconds();
                let out = call(s, name, &args, on_diag.as_ref());
                let ms = (platform.thread_cpu_seconds() - t0) * 1000.0;
                if let Some(sink) = s.cpu_sink.borrow().as_ref() {
                    sink(ms);
                }
                out
            })
            .unwrap_or_default()
    }

    /// Untrusted ops batch (JSON) → validated RenderOps (JSON array; "[]" if nothing survived).
    pub fn process_ops(&self, json: &[u8]) -> Vec<u8> {
        or_empty_list(self.metered("__v_processOps", vec![json.to_vec()]))
    }

    /// Untrusted query → clean query JSON, or none if rejected.
    pub fn validate_query(&self, json: &[u8]) -> Option<Vec<u8>> {
        Some(self.metered("__v_validateQuery", vec![json.to_vec()])).filter(|s| !s.is_empty())
    }

    pub fn validate_listen(&self, json: &[u8]) -> Vec<u8> {
        or_empty_list(self.metered("__v_validateListen", vec![json.to_vec()]))
    }

    pub fn validate_prevent_default(&self, json: &[u8]) -> Vec<u8> {
        or_empty_list(self.metered("__v_validatePreventDefault", vec![json.to_vec()]))
    }

    pub fn set_policy(&self, policy: &[u8]) {
        self.str("__v_setPolicy", vec![policy.to_vec()]);
    }

    pub fn validate_anim(&self, json: &[u8]) -> Vec<u8> {
        or_empty_list(self.metered("__v_validateAnim", vec![json.to_vec()]))
    }

    /// RenderOps re-styling the document for the new theme.
    pub fn set_theme(&self, theme: &[u8]) -> Vec<u8> {
        or_empty_list(self.str("__v_setTheme", vec![theme.to_vec()]))
    }

    pub fn rewrite_media_query(&self, query: &[u8]) -> Vec<u8> {
        self.metered("__v_rewriteMediaQuery", vec![query.to_vec()])
    }

    pub fn node_count(&self) -> i32 {
        self.thread
            .sync(|s: &ValidatorState| {
                let mut js = s.js.borrow_mut();
                match js.as_mut().map(|js| js.call("__v_nodeCount", &[])) {
                    Some(Ok(r)) => r.number.unwrap_or(0.0) as i32,
                    _ => 0,
                }
            })
            .unwrap_or(0)
    }

    /// The loading tasks are over (their ops are in): text changes are rate-limited from now on.
    pub fn mark_loaded(&self) {
        self.str("__v_markLoaded", Vec::new());
    }

    /// Set once a counted cap was passed under strict (the first `limit` diagnostic's detail): the reason the
    /// owner stops the widget with. Read it after the call that passed the cap returns.
    pub fn stopped(&self) -> Option<Vec<u8>> {
        lock(&self.stopped).clone()
    }

    /// The sink of the CPU time of each call made for the widget's messages: `process_ops`, `validate_query`,
    /// `rewrite_media_query`, `validate_listen`, `validate_prevent_default`, `validate_anim`. None: nothing is
    /// measured.
    pub fn set_cpu_sink(&self, sink: Option<CpuSink>) {
        self.thread.sync(move |s: &ValidatorState| *s.cpu_sink.borrow_mut() = sink);
    }
}

impl Drop for TrustedValidator {
    fn drop(&mut self) {
        self.thread.sync(|s: &ValidatorState| *s.js.borrow_mut() = None);
        self.thread.join();
    }
}

fn or_empty_list(s: Vec<u8>) -> Vec<u8> {
    if s.is_empty() { b"[]".to_vec() } else { s }
}

fn call(s: &ValidatorState, name: &str, args: &[Vec<u8>], on_diag: Option<&DiagSink>) -> Vec<u8> {
    let argv: Vec<&[u8]> = args.iter().map(Vec::as_slice).collect();
    let r = {
        let mut js = s.js.borrow_mut();
        match js.as_mut() {
            Some(js) => js.call(name, &argv),
            None => return Vec::new(),
        }
    };
    match r {
        Ok(r) => r.string.unwrap_or_default(),
        Err(e) => {
            if let Some(d) = on_diag {
                d(b"validator-error", format!("{name}: {}", e.text).as_bytes());
            }
            Vec::new()
        }
    }
}
