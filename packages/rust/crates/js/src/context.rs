//! `nova_context.h`, `nova_context.cpp`: one Nova engine, one realm, string-based host functions, used only
//! from the thread that owns it. What the C++ reached through nova_embed_ffi's C interface is `nova_embed`'s
//! safe API here, with the C side's conversions kept: an argument string ends at its first NUL, a number is
//! `%lld` or `%.17g`, a copy the heap could not hold ends the script, and the error texts are the C++'s.
//!
//! The memory limit counts what nova_embed_ffi counts: the bytes of the blocks allocated on the engine's behalf,
//! on an account of nova-safe's metering allocator (`nova_meter`, the process's allocator: the helper's and
//! nova_embed_ffi's). The context enters the engine's account where nova_embed_ffi's C interface enters it, around
//! each call into the engine, and keeps the rest off it as the C++'s allocations are: the host's part of a host
//! function call runs on the uncounted account, only its answer is copied on the engine's (`nova_return_*`), and
//! what a call returns is copied off the engine's blocks as the C++ copies a result before freeing it. Without that
//! allocator (a test that does not install it) an account holds nothing and the engine goes by its own estimate.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::{Arc, Mutex, OnceLock};

use nova_embed::{ArgumentMode, Completion, Engine, EngineOptions, Error, Host, HostArg};
use nova_meter::Account;

/// What the context needs of the operating system, which safe Rust cannot ask itself: the embedder's.
pub trait Platform: Send + Sync {
    /// CPU seconds the calling thread has run, unaffected by how busy the machine is.
    fn thread_cpu_seconds(&self) -> f64;
    /// Seconds east of UTC of the user's time zone at `epoch_seconds`, daylight saving time included.
    fn time_zone_offset(&self, epoch_seconds: i64) -> i32;
    /// `bytes` filled from the system's CSPRNG.
    fn random(&self, bytes: &mut [u8]);
    /// The steady clock's seconds, `std::chrono::steady_clock` in the C++: the clock the engine's deadlines,
    /// its tasks' times and the widget's `now` are on, the same in the host's process (the embedder's); by
    /// default this process's [`now`].
    fn steady_seconds(&self) -> f64 {
        now()
    }
}

/// Monotonic seconds since the process's first call, the clock deadlines are measured on.
pub fn now() -> f64 {
    static START: OnceLock<std::time::Instant> = OnceLock::new();
    START.get_or_init(std::time::Instant::now).elapsed().as_secs_f64()
}

/// What a host function returns into the sandbox.
#[derive(Clone, Debug, PartialEq)]
pub enum HostReturn {
    Undefined,
    Null,
    String(Vec<u8>),
    Number(f64),
    ArrayBuffer(Vec<u8>),
    RangeError(Vec<u8>),
}

/// Why a host function could not answer, which ends the running script as the C++'s exceptions did.
#[derive(Clone, Debug, PartialEq)]
pub enum HostFailure {
    /// `std::bad_alloc`: "InternalError: out of memory".
    Memory,
    /// Any other exception: "InternalError: host function failed: <what>".
    Other(String),
}

pub type HostFunction = Box<dyn Fn(&[Vec<u8>]) -> Result<HostReturn, HostFailure>>;

/// The C++'s `JSError`: the text, and whether a script cannot catch it (out of memory, the interrupt, a host
/// function's failure, an engine fault).
#[derive(Clone, Debug, PartialEq)]
pub struct JsError {
    pub text: String,
    pub uncatchable: bool,
}

impl JsError {
    fn catchable(text: impl Into<String>) -> JsError {
        JsError { text: text.into(), uncatchable: false }
    }

    fn uncatchable(text: impl Into<String>) -> JsError {
        JsError { text: text.into(), uncatchable: true }
    }
}

/// The value of a finished call: a string, a number or nothing.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct JsResult {
    pub string: Option<Vec<u8>>,
    pub number: Option<f64>,
}

#[derive(Clone, Debug, PartialEq)]
enum Failure {
    None,
    Memory,
    Other,
}

/// The context's state its interrupt and host functions reach while the engine runs, and what the
/// embedder sets around a call: the deadlines.
pub struct Deadlines {
    /// Interrupt deadline in the platform's steady seconds; +inf disables it.
    pub deadline: Cell<f64>,
    /// Set by the interrupt when a deadline fired; reset by the caller.
    pub interrupted: Cell<bool>,
    /// Interrupt deadline in thread CPU seconds of the thread running JS; +inf disables it.
    pub cpu_deadline: Cell<f64>,
    failure: RefCell<Failure>,
    failure_text: RefCell<String>,
    functions: RefCell<Vec<Rc<HostFunction>>>,
}

impl std::fmt::Debug for Deadlines {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Deadlines")
            .field("deadline", &self.deadline.get())
            .field("interrupted", &self.interrupted.get())
            .field("cpu_deadline", &self.cpu_deadline.get())
            .finish()
    }
}

/// What nova_embed calls back. nova_embed keeps it on the engine's account, so it is five words, as nova_embed_ffi's
/// `CHost` (its two callbacks, the time zone, the account and the C++'s context): both ends count the same bytes.
struct NovaHost {
    shared: Rc<Deadlines>,
    account: Rc<Account>,
    platform: Arc<dyn Platform>,
    memory_limit: usize,
}

const _: () = assert!(size_of::<NovaHost>() == 5 * size_of::<usize>());

/// printf's `%lld` for an integral number below 1e15, else `%.17g`.
fn number_text(d: f64) -> Vec<u8> {
    if d == d.floor() && d.abs() < 1e15 {
        return (d as i64).to_string().into_bytes();
    }
    if d.is_nan() {
        return if d.is_sign_negative() { b"-nan".to_vec() } else { b"nan".to_vec() };
    }
    if d.is_infinite() {
        return if d < 0.0 { b"-inf".to_vec() } else { b"inf".to_vec() };
    }
    twidget_json::format_g(d, 17).into_bytes()
}

impl NovaHost {
    /// The C++'s trampoline: the host function's answer, `Undefined` when it may not run or failed. Nothing a host
    /// function does may leave here but its answer: a failure ends the script at the next poll.
    fn answer(&self, id: u32, args: Vec<HostArg>) -> HostReturn {
        let shared = &self.shared;
        if *shared.failure.borrow() != Failure::None {
            return HostReturn::Undefined;
        }
        let Some(function) = shared.functions.borrow().get(id as usize).cloned() else {
            return HostReturn::Undefined;
        };
        let limit = self.memory_limit;
        let mut list = Vec::with_capacity(args.len());
        for arg in args {
            let text = match arg {
                // A copy the engine's heap could not hold ends the script: the engine makes its copies outside that heap.
                HostArg::String(bytes) if limit != 0 && bytes.len() > limit => {
                    *shared.failure.borrow_mut() = Failure::Memory;
                    return HostReturn::Undefined;
                }
                HostArg::String(mut bytes) => {
                    if let Some(nul) = bytes.iter().position(|b| *b == 0) {
                        bytes.truncate(nul);
                    }
                    bytes
                }
                HostArg::Number(d) => number_text(d),
                _ => Vec::new(),
            };
            list.push(text);
        }
        match function(&list) {
            Err(HostFailure::Memory) => {
                *shared.failure.borrow_mut() = Failure::Memory;
                HostReturn::Undefined
            }
            Err(HostFailure::Other(what)) => {
                *shared.failure.borrow_mut() = Failure::Other;
                *shared.failure_text.borrow_mut() = format!("host function failed: {what}");
                HostReturn::Undefined
            }
            Ok(r) => {
                let size = match &r {
                    HostReturn::String(s) | HostReturn::RangeError(s) | HostReturn::ArrayBuffer(s) => s.len(),
                    _ => 0,
                };
                if limit != 0 && size > limit {
                    *shared.failure.borrow_mut() = Failure::Memory;
                    return HostReturn::Undefined;
                }
                r
            }
        }
    }
}

impl Host for NovaHost {
    // The host's part runs on the uncounted account, as the C++'s runs off the metered heap; its answer is copied on
    // the engine's account, where the call is, as nova_embed_ffi's `nova_return_*` copies it.
    fn call(&self, id: u32, args: Vec<HostArg>) -> nova_embed::HostReturn {
        let r = {
            let _host = Account::uncounted().enter();
            self.answer(id, args)
        };
        match r {
            HostReturn::Undefined => nova_embed::HostReturn::Undefined,
            HostReturn::Null => nova_embed::HostReturn::Null,
            HostReturn::Number(n) => nova_embed::HostReturn::Number(n),
            HostReturn::ArrayBuffer(b) => nova_embed::HostReturn::ArrayBuffer(b.as_slice().to_vec()),
            HostReturn::String(s) => match String::from_utf8(s.as_slice().to_vec()) {
                Ok(text) => nova_embed::HostReturn::String(text),
                Err(e) => nova_embed::HostReturn::StringBytes(e.into_bytes()),
            },
            HostReturn::RangeError(s) => nova_embed::HostReturn::RangeError(
                String::from_utf8(s.as_slice().to_vec()).unwrap_or_else(|e| String::from_utf8_lossy(e.as_bytes()).into_owned()),
            ),
        }
    }

    fn should_interrupt(&self) -> bool {
        let _host = Account::uncounted().enter();
        let shared = &self.shared;
        if *shared.failure.borrow() != Failure::None {
            return true;
        }
        let cpu = shared.cpu_deadline.get();
        if self.platform.steady_seconds() > shared.deadline.get() || (cpu != f64::INFINITY && self.platform.thread_cpu_seconds() > cpu) {
            shared.interrupted.set(true);
            return true;
        }
        false
    }

    fn local_time_zone_offset(&self, epoch_seconds: i64) -> i32 {
        let _host = Account::uncounted().enter();
        self.platform.time_zone_offset(epoch_seconds)
    }

    // An account that holds nothing while its engine lives is not being counted: the process's allocator is not
    // nova_meter's.
    fn allocated_bytes(&self) -> Option<usize> {
        self.account.bytes().filter(|&bytes| bytes != 0)
    }

    // A host function's name, interned for the process's life: on no engine's account, as nova_embed_ffi keeps it.
    fn for_the_process(&self, work: &mut dyn FnMut()) {
        let _process = Account::uncounted().enter();
        work();
    }
}

const MAX_COMPILED_PER_BUNDLE: usize = 4;
const MAX_CALL_ARGS: usize = 64;

/// Our own bundles compiled once per process: immutable code that holds no engine state, which any context
/// on any thread evaluates; None for a source that could not be shared or compiled.
type Bundles = HashMap<String, Vec<(Vec<u8>, Option<Arc<nova_embed::CompiledScript>>)>>;

fn compiled_bundle(source: &[u8], filename: &str) -> Option<Arc<nova_embed::CompiledScript>> {
    static BUNDLES: OnceLock<Mutex<Bundles>> = OnceLock::new();
    let mut bundles = BUNDLES.get_or_init(|| Mutex::new(HashMap::new())).lock().unwrap_or_else(std::sync::PoisonError::into_inner);
    let list = bundles.entry(filename.to_string()).or_default();
    if let Some((_, script)) = list.iter().find(|(s, _)| s.as_slice() == source) {
        return script.clone();
    }
    if list.len() >= MAX_COMPILED_PER_BUNDLE {
        return None;
    }
    let compiled = std::str::from_utf8(source).ok().and_then(|text| nova_embed::CompiledScript::compile(text).ok()).map(Arc::new);
    list.push((source.to_vec(), compiled.clone()));
    compiled
}

/// The length of the well-formed UTF-8 sequence at `p`, 0 when there is none.
fn utf8_sequence_length(p: &[u8]) -> usize {
    let tail = |i: usize| p.get(i).is_some_and(|b| b & 0xC0 == 0x80);
    match p[0] {
        0x00..=0x7F => 1,
        0xC2..=0xDF => {
            if tail(1) {
                2
            } else {
                0
            }
        }
        0xE0..=0xEF => {
            if !tail(1) || !tail(2) || (p[0] == 0xE0 && p[1] < 0xA0) || (p[0] == 0xED && p[1] > 0x9F) {
                0
            } else {
                3
            }
        }
        0xF0..=0xF4 => {
            if !tail(1) || !tail(2) || !tail(3) || (p[0] == 0xF0 && p[1] < 0x90) || (p[0] == 0xF4 && p[1] > 0x8F) {
                0
            } else {
                4
            }
        }
        _ => 0,
    }
}

/// `text` with every byte that is not part of well-formed UTF-8 replaced by U+FFFD.
pub fn repaired_utf8(text: &[u8]) -> String {
    let mut out = Vec::with_capacity(text.len() + 8);
    let mut i = 0;
    while i < text.len() {
        let n = utf8_sequence_length(&text[i..]);
        if n != 0 {
            out.extend_from_slice(&text[i..i + n]);
            i += n;
        } else {
            out.extend_from_slice("\u{FFFD}".as_bytes());
            i += 1;
        }
    }
    String::from_utf8(out).expect("repaired text is UTF-8")
}

/// One engine and its realm.
pub struct NovaContext {
    engine: Engine,
    shared: Rc<Deadlines>,
    /// What the engine's allocations count on, entered around every call into the engine; after `engine`, so that it
    /// is given up only once the engine's blocks are freed.
    account: Rc<Account>,
    memory_limit: usize,
}

impl std::fmt::Debug for NovaContext {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("NovaContext").field("deadlines", &self.shared).finish()
    }
}

fn identifier(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 64
        && !name.as_bytes()[0].is_ascii_digit()
        && name.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_' || c == b'$')
}

impl NovaContext {
    /// `max_stack_bytes` must stay below the real stack of the thread that enters JS: the engine's JavaScript
    /// gets three quarters of it, counted from each call into the engine.
    pub fn new(memory_limit_bytes: usize, max_stack_bytes: usize, platform: Arc<dyn Platform>) -> Result<NovaContext, JsError> {
        let limits = nova_embed::LIMIT_INTERRUPT | nova_embed::LIMIT_HEAP | nova_embed::LIMIT_STACK;
        if nova_embed::ENFORCED_LIMITS & limits != limits {
            return Err(JsError::catchable("this build of the Nova engine does not enforce its limits"));
        }
        let shared = Rc::new(Deadlines {
            deadline: Cell::new(f64::INFINITY),
            interrupted: Cell::new(false),
            cpu_deadline: Cell::new(f64::INFINITY),
            failure: RefCell::new(Failure::None),
            failure_text: RefCell::new(String::new()),
            functions: RefCell::new(Vec::new()),
        });
        let options = EngineOptions { memory_limit: memory_limit_bytes, stack_limit: max_stack_bytes / 4 * 3 };
        let account = Rc::new(Account::new());
        let host = NovaHost { shared: Rc::clone(&shared), account: Rc::clone(&account), platform, memory_limit: memory_limit_bytes };
        let created = {
            let _engine = account.enter();
            Engine::new(options, Box::new(host))
        };
        let mut engine = created.map_err(|e| {
            let why = error_text(&e);
            JsError::catchable(if why.is_empty() { "the Nova engine could not be created".to_string() } else { why })
        })?;
        // Out of memory ends the script wherever it happens: caught, a script would go on just under its limit.
        let fatal = {
            let _engine = account.enter();
            engine.set_out_of_memory_fatal(true)
        };
        if fatal.is_err() {
            return Err(JsError::catchable("the Nova engine did not take its out-of-memory mode"));
        }
        Ok(NovaContext { engine, shared, account, memory_limit: memory_limit_bytes })
    }

    /// What the embedder sets around a call and its host functions read, shared with the interrupt.
    pub fn deadlines(&self) -> Rc<Deadlines> {
        Rc::clone(&self.shared)
    }

    /// The bytes the engine's allocations hold, or Nova's estimate of its heap when they are not counted.
    pub fn memory_used(&mut self) -> usize {
        let _engine = self.account.enter();
        self.engine.memory_used()
    }

    /// Exposes `name(...)` to the sandbox, `name` an identifier; arguments arrive as strings, at most eight.
    pub fn register_function(&mut self, name: &str, function: HostFunction) -> Result<(), JsError> {
        if !identifier(name) {
            return Err(JsError::catchable("a host function's name must be an identifier of at most 64 characters"));
        }
        let id = self.shared.functions.borrow().len() as u32;
        self.shared.functions.borrow_mut().push(Rc::new(function));
        let registered = {
            let _engine = self.account.enter();
            self.engine.register_function(name, id, 2, ArgumentMode::Values)
        };
        registered.map_err(|e| JsError::catchable(format!("host function {name}: {}", error_text(&e))))
    }

    /// The value of a finished call, or its error as the C++ threw it. Its text is a copy, made off the engine's
    /// account, of what the engine returned, which is freed here, as the C++ copies a result and frees it: kept, the
    /// engine's block would count on the engine until the embedder let it go.
    fn finish(&mut self, result: Result<Completion, Error>) -> Result<JsResult, JsError> {
        let failure = std::mem::replace(&mut *self.shared.failure.borrow_mut(), Failure::None);
        match failure {
            Failure::Memory => return Err(JsError::uncatchable("InternalError: out of memory")),
            Failure::Other => return Err(JsError::uncatchable(format!("InternalError: {}", self.shared.failure_text.borrow()))),
            Failure::None => {}
        }
        match result {
            Ok(completion) => {
                let limit = self.memory_limit;
                match completion {
                    // A result the engine's heap could not hold is an error, as an argument of a host call is.
                    Completion::String(s) if limit != 0 && s.len() > limit => Err(JsError::uncatchable("InternalError: out of memory")),
                    Completion::String(s) => Ok(JsResult { string: Some(s.as_slice().to_vec()), number: None }),
                    Completion::Number(n) => Ok(JsResult { string: None, number: Some(n) }),
                    Completion::Undefined => Ok(JsResult::default()),
                }
            }
            Err(Error::Exception(text)) => Err(JsError::catchable(if text.is_empty() { "unknown error" } else { text.as_str() })),
            Err(Error::NotAFunction(name)) => Err(JsError::catchable(format!("{name} is not a function"))),
            Err(Error::Interrupted) => Err(JsError::uncatchable("InternalError: interrupted")),
            Err(Error::OutOfMemory) => Err(JsError::uncatchable("InternalError: out of memory")),
            Err(e) => {
                let text = error_text(&e);
                Err(JsError::uncatchable(format!("InternalError: {text}")))
            }
        }
    }

    pub fn evaluate(&mut self, code: &[u8], as_module: bool) -> Result<JsResult, JsError> {
        let repaired;
        let text = match std::str::from_utf8(code) {
            Ok(text) => text,
            // The engine takes well-formed UTF-8 only; a lone surrogate or a stray byte becomes U+FFFD.
            Err(_) => {
                repaired = repaired_utf8(code);
                &repaired
            }
        };
        let result = {
            let _engine = self.account.enter();
            self.engine.evaluate(text, as_module)
        };
        self.finish(result)
    }

    /// Runs one of our own bundles from code compiled once per process and shared by every context; false when
    /// it could not be shared and was evaluated from `source` instead. Never for widget code.
    pub fn evaluate_bundle(&mut self, source: &[u8], filename: &str) -> Result<bool, JsError> {
        match compiled_bundle(source, filename) {
            None => self.evaluate(source, false).map(|_| false),
            Some(script) => {
                let result = {
                    let _engine = self.account.enter();
                    self.engine.evaluate_compiled(&script)
                };
                self.finish(result).map(|_| true)
            }
        }
    }

    /// Calls the global function `name` with string arguments (as WTF-8, so lone surrogates go back as they were).
    pub fn call(&mut self, name: &str, args: &[&[u8]]) -> Result<JsResult, JsError> {
        if args.len() > MAX_CALL_ARGS {
            return Err(JsError::catchable(format!("more than {MAX_CALL_ARGS} arguments to {name}")));
        }
        let result = {
            let _engine = self.account.enter();
            // nova_embed_ffi lists the arguments it copied on the engine's account, for the length of the call.
            let args = args.to_vec();
            self.engine.call_with_bytes(name, &args)
        };
        self.finish(result)
    }

    /// Keeps a private reference to the global function `name`; `call(name)` uses it from then on.
    pub fn capture_global_function(&mut self, name: &str) -> bool {
        let result = {
            let _engine = self.account.enter();
            self.engine.capture_global_function(name)
        };
        if std::mem::replace(&mut *self.shared.failure.borrow_mut(), Failure::None) != Failure::None {
            return false;
        }
        matches!(result, Ok(true))
    }

    pub fn has_pending_job(&self) -> bool {
        let _engine = self.account.enter();
        self.engine.has_pending_job()
    }

    /// Runs one queued promise job, if any (false: the queue was empty); its error goes to `on_error`.
    pub fn execute_pending_job(&mut self, on_error: impl FnOnce(&JsError)) -> bool {
        let (has_job, result) = {
            let _engine = self.account.enter();
            (self.engine.has_pending_job(), self.engine.run_pending_job())
        };
        if !has_job && result.is_ok() {
            return false;
        }
        if let Err(e) = self.finish(result.map(|_| Completion::Undefined)) {
            on_error(&e);
        }
        true
    }
}

/// The text nova_embed_ffi writes for an error.
fn error_text(e: &Error) -> String {
    match e {
        Error::Exception(text) | Error::Dead(text) | Error::Invalid(text) => text.clone(),
        Error::NotAFunction(name) => name.clone(),
        other => other.to_string(),
    }
}
