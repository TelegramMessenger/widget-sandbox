//! The C ABI between an engine process's C++ OS glue and its end in Rust (`include/twidget_engine.h`): the glue
//! keeps the descriptors, the lockdown and the clocks the OS gives, and hands `twidget_engine_serve` (or, for a
//! hosting stream whose widgets' renderers the glue builds, `twidget_engine_serve_hosting`) its stream and
//! platform as callbacks; everything past them is the safe crates. With the `nova-c-api` feature Nova's own C
//! interface (`nova_engine.h`) is in the same library, so a program that still drives Nova from C++ links one Rust
//! library.
//!
//! The one crate of the engine process with `unsafe`: the callbacks' raw pointers, the C buffers turned into
//! slices, and the promise that the glue's callbacks may be called from any thread. Every call into the glue
//! holds a guard that serving takes back before it returns, so none reaches the glue later.

#![deny(unsafe_op_in_unsafe_fn)]

mod hosted;

use std::ffi::c_void;
use std::sync::{Arc, Condvar, Mutex};

#[cfg(feature = "nova-c-api")]
pub use nova_embed_ffi;
use twidget_js::Platform;
use twidget_stream::Stream;

pub use hosted::{
    TWIDGET_HOSTED_BUILT_NOTHING, TWIDGET_HOSTED_RENDERER, TWIDGET_HOSTED_THREW, TwidgetEngineBytes, TwidgetHostedLink,
    TwidgetHostedPlatform, TwidgetHostedRenderer, TwidgetHostedRenderers, TwidgetHostedResources, twidget_engine_serve_hosting,
    twidget_hosted_platform_release, twidget_hosted_platform_request, twidget_hosted_platform_send, twidget_hosted_renderer_diag,
    twidget_hosted_renderer_message,
};

/// The stream to the host's process: `read` blocks until at least one byte came and returns how many it
/// copied (0 at the end, on an error and after `shutdown`), `write` blocks until all of the bytes are written
/// (false once broken or shut down), `shutdown` wakes both from any thread without waiting. All three may be
/// called from any thread, a read and a write at once.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct TwidgetEngineStream {
    pub context: *mut c_void,
    pub read: Option<unsafe extern "C" fn(context: *mut c_void, out: *mut u8, size: usize) -> usize>,
    pub write: Option<unsafe extern "C" fn(context: *mut c_void, bytes: *const u8, size: usize) -> bool>,
    pub shutdown: Option<unsafe extern "C" fn(context: *mut c_void)>,
}

/// What only the OS answers, from any thread: the calling thread's CPU seconds, the steady clock's seconds
/// (`std::chrono::steady_clock`, the host's too), the user's time zone (seconds east of UTC at a time) and
/// `size` bytes of the system's CSPRNG.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct TwidgetEnginePlatform {
    pub context: *mut c_void,
    pub thread_cpu_seconds: Option<unsafe extern "C" fn(context: *mut c_void) -> f64>,
    pub steady_seconds: Option<unsafe extern "C" fn(context: *mut c_void) -> f64>,
    pub time_zone_offset: Option<unsafe extern "C" fn(context: *mut c_void, epoch_seconds: i64) -> i32>,
    pub random: Option<unsafe extern "C" fn(context: *mut c_void, out: *mut u8, size: usize)>,
}

/// Locks the process down once the opening was accepted, before `Ready`: why it could not (`*size` bytes the
/// glue keeps until serving returns), or `*size` 0 when it did.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct TwidgetEngineLockdown {
    pub context: *mut c_void,
    pub lockdown: Option<unsafe extern "C" fn(context: *mut c_void, size: *mut usize) -> *const u8>,
}

/// Calls reach the glue only while serving: `close` waits for the calls running and turns every later one away.
/// A call may run inside another (a renderer's request writes to the stream), so entering never waits.
struct Guard {
    state: Mutex<GuardState>,
    idle: Condvar,
}

struct GuardState {
    open: bool,
    running: usize,
}

impl Guard {
    fn new() -> Arc<Guard> {
        Arc::new(Guard { state: Mutex::new(GuardState { open: true, running: 0 }), idle: Condvar::new() })
    }

    fn run<R>(&self, closed: R, call: impl FnOnce() -> R) -> R {
        {
            let mut st = self.state.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
            if !st.open {
                return closed;
            }
            st.running += 1;
        }
        let result = call();
        let mut st = self.state.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        st.running -= 1;
        if st.running == 0 {
            self.idle.notify_all();
        }
        result
    }

    fn close(&self) {
        let mut st = self.state.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        st.open = false;
        while st.running != 0 {
            st = self.idle.wait(st).unwrap_or_else(std::sync::PoisonError::into_inner);
        }
    }
}

struct CStream {
    c: TwidgetEngineStream,
    guard: Arc<Guard>,
}

// SAFETY: the glue's contract (twidget_engine.h): its callbacks and their context may be used from any thread
// until serving returns, which the guard enforces.
unsafe impl Send for CStream {}
// SAFETY: as above, a read and a write at once included.
unsafe impl Sync for CStream {}

impl Stream for CStream {
    fn read(&self, out: &mut [u8]) -> usize {
        let Some(read) = self.c.read else { return 0 };
        // SAFETY: `out` is writable for its length; the glue copies at most that many bytes.
        self.guard.run(0, || unsafe { read(self.c.context, out.as_mut_ptr(), out.len()) }).min(out.len())
    }

    fn write(&self, bytes: &[u8]) -> bool {
        let Some(write) = self.c.write else { return false };
        // SAFETY: `bytes` is readable for its length for the whole call.
        self.guard.run(false, || unsafe { write(self.c.context, bytes.as_ptr(), bytes.len()) })
    }

    fn shutdown(&self) {
        if let Some(shutdown) = self.c.shutdown {
            // SAFETY: the glue's own context, while serving.
            self.guard.run((), || unsafe { shutdown(self.c.context) });
        }
    }
}

struct CPlatform {
    c: TwidgetEnginePlatform,
    guard: Arc<Guard>,
}

// SAFETY: the glue's contract: the platform's callbacks may be called from any thread while serving.
unsafe impl Send for CPlatform {}
// SAFETY: as above.
unsafe impl Sync for CPlatform {}

impl Platform for CPlatform {
    fn thread_cpu_seconds(&self) -> f64 {
        let Some(f) = self.c.thread_cpu_seconds else { return self.steady_seconds() };
        // SAFETY: a call with the glue's own context, while serving.
        self.guard.run(0.0, || unsafe { f(self.c.context) })
    }

    fn steady_seconds(&self) -> f64 {
        let Some(f) = self.c.steady_seconds else { return twidget_js::now() };
        // SAFETY: as above.
        self.guard.run(0.0, || unsafe { f(self.c.context) })
    }

    fn time_zone_offset(&self, epoch_seconds: i64) -> i32 {
        let Some(f) = self.c.time_zone_offset else { return 0 };
        // SAFETY: as above.
        self.guard.run(0, || unsafe { f(self.c.context, epoch_seconds) })
    }

    fn random(&self, bytes: &mut [u8]) {
        let Some(f) = self.c.random else { return bytes.fill(0) };
        // SAFETY: `bytes` is writable for its length; the glue fills exactly that many.
        self.guard.run((), || unsafe { f(self.c.context, bytes.as_mut_ptr(), bytes.len()) });
    }
}

/// `bytes` as a Rust slice; empty for a null pointer.
///
/// # Safety
/// `bytes` is null or readable for `size` bytes for `'a`.
unsafe fn slice<'a>(bytes: *const u8, size: usize) -> &'a [u8] {
    if bytes.is_null() || size == 0 {
        return &[];
    }
    // SAFETY: the caller's promise.
    unsafe { std::slice::from_raw_parts(bytes, size) }
}

/// What serving needs from the caller's tables, read once and copied: the stream, the platform and the lockdown
/// as safe objects behind one guard.
struct Served {
    guard: Arc<Guard>,
    stream: Arc<dyn Stream>,
    platform: Arc<dyn Platform>,
    build: Vec<u8>,
    lockdown: Option<TwidgetEngineLockdown>,
}

impl Served {
    /// # Safety
    /// As `twidget_engine_serve`'s.
    unsafe fn new(
        stream: *const TwidgetEngineStream,
        platform: *const TwidgetEnginePlatform,
        build: *const u8,
        build_size: usize,
        lockdown: *const TwidgetEngineLockdown,
    ) -> Served {
        // SAFETY: the caller's promises, read once and copied.
        let (stream, platform, build, lockdown) =
            unsafe { (*stream, *platform, slice(build, build_size).to_vec(), lockdown.as_ref().copied()) };
        let guard = Guard::new();
        let c_stream: Arc<dyn Stream> = Arc::new(CStream { c: stream, guard: Arc::clone(&guard) });
        let c_platform: Arc<dyn Platform> = Arc::new(CPlatform { c: platform, guard: Arc::clone(&guard) });
        Served { guard, stream: c_stream, platform: c_platform, build, lockdown }
    }

    /// Serves with `serve` given the stream, the build, the lockdown and the platform; once it returned no
    /// callback runs, and the reason is handed over as `twidget_engine_free` frees it.
    ///
    /// # Safety
    /// `reason_size` is writable; the lockdown table's promises hold until this returns.
    unsafe fn run(
        self,
        reason_size: *mut usize,
        serve: impl FnOnce(Arc<dyn Stream>, &[u8], Option<&dyn Fn() -> Vec<u8>>, Arc<dyn Platform>) -> Vec<u8>,
    ) -> *mut u8 {
        let lock = self.lockdown.and_then(|l| {
            l.lockdown.map(|f| {
                move || {
                    let mut size = 0usize;
                    // SAFETY: the glue's lockdown with its own context, on the serving thread; its text stays valid
                    // until serving returns, and is copied at once.
                    unsafe {
                        let text = f(l.context, &mut size);
                        slice(text, size).to_vec()
                    }
                }
            })
        });
        let reason = serve(self.stream, &self.build, lock.as_ref().map(|f| f as &dyn Fn() -> Vec<u8>), self.platform);
        self.guard.close();
        // SAFETY: the caller's writable size.
        unsafe { leak(reason, reason_size) }
    }
}

/// `bytes` handed to the glue, `*size` bytes it frees with `twidget_engine_free`; null with 0 for none.
///
/// # Safety
/// `size` is writable.
unsafe fn leak(bytes: Vec<u8>, size: *mut usize) -> *mut u8 {
    let mut bytes = bytes.into_boxed_slice();
    // SAFETY: the caller's writable size.
    unsafe { *size = bytes.len() };
    if bytes.is_empty() {
        return std::ptr::null_mut();
    }
    let p = bytes.as_mut_ptr();
    std::mem::forget(bytes);
    p
}

/// Serves one engine stream until it ends, on the calling thread (`SandboxRemoteEngineEnd::serve`): why it
/// ended, `*reason_size` bytes the caller frees with `twidget_engine_free`, or null with 0 at the end of the
/// stream. Every runtime is destroyed and no callback runs once it returned.
///
/// # Safety
/// `stream` and `platform` point to valid tables whose callbacks may be called from any thread until this
/// returns; `build` is readable for `build_size` bytes; `lockdown` is null or a valid table; `reason_size` is
/// writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn twidget_engine_serve(
    stream: *const TwidgetEngineStream,
    platform: *const TwidgetEnginePlatform,
    build: *const u8,
    build_size: usize,
    lockdown: *const TwidgetEngineLockdown,
    reason_size: *mut usize,
) -> *mut u8 {
    // SAFETY: the caller's promises, passed on.
    unsafe { Served::new(stream, platform, build, build_size, lockdown).run(reason_size, twidget_server::serve) }
}

/// Frees what `twidget_engine_serve`, `twidget_engine_serve_hosting` or `twidget_hosted_platform_request`
/// returned.
///
/// # Safety
/// `bytes` and `size` are exactly a buffer one of them returned, freed once; null does nothing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn twidget_engine_free(bytes: *mut u8, size: usize) {
    if bytes.is_null() {
        return;
    }
    // SAFETY: the box `leak` leaked, of that length.
    drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(bytes, size)) });
}
