//! The hosted mode's half of the C ABI: `twidget_engine_serve_hosting` serves a hosting stream whose widgets'
//! renderers the glue builds (`twidget_hosted_renderers`), each renderer a table of the glue's functions; the
//! renderer reaches its widget back through two opaque handles the factory is given, its platform
//! (`twidget_hosted_platform`, kept as long as the glue holds it) and its link (`twidget_hosted_link`, its
//! messages and diagnostics for the served host, valid until the renderer's `free` returned).

use std::ffi::c_void;
use std::sync::{Arc, Mutex};

use twidget_bundle_stamp::SandboxResources;
use twidget_host::{HostedPlatform, HostedRendererFactory, Renderer, RendererLink};

use crate::{Guard, Served, TwidgetEngineLockdown, TwidgetEnginePlatform, TwidgetEngineStream, leak, slice};

/// `size` bytes at `bytes` (null with 0 for none).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct TwidgetEngineBytes {
    pub bytes: *const u8,
    pub size: usize,
}

impl TwidgetEngineBytes {
    fn of(bytes: &[u8]) -> TwidgetEngineBytes {
        TwidgetEngineBytes { bytes: bytes.as_ptr(), size: bytes.len() }
    }
}

/// The hosting opening's resources a factory gets: the sandbox bundle, the whole validator bundle and the
/// renderer bundle's stamp line.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct TwidgetHostedResources {
    pub sandbox_bundle: TwidgetEngineBytes,
    pub validator_bundle: TwidgetEngineBytes,
    pub renderer_bundle: TwidgetEngineBytes,
}

/// One widget's renderer (renderer.h): `load`, `send`, `set_theme`, `set_width` and `destroy` as the served host
/// calls them, one at a time on its glue thread; `platform_message` the host's platform message for it on that
/// thread (`SandboxHostedPlatform::onMessage`); `free` once, after every other call returned, on any thread.
/// Any of them may be null.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct TwidgetHostedRenderer {
    pub context: *mut c_void,
    pub load: Option<unsafe extern "C" fn(context: *mut c_void)>,
    pub send: Option<unsafe extern "C" fn(context: *mut c_void, json: *const u8, size: usize)>,
    pub set_theme: Option<unsafe extern "C" fn(context: *mut c_void, theme: *const u8, size: usize)>,
    pub set_width: Option<unsafe extern "C" fn(context: *mut c_void, width: f64)>,
    pub destroy: Option<unsafe extern "C" fn(context: *mut c_void)>,
    pub platform_message: Option<unsafe extern "C" fn(context: *mut c_void, message: *const u8, size: usize)>,
    pub free: Option<unsafe extern "C" fn(context: *mut c_void)>,
}

/// `make` built nothing (`HostedRendererFactory` returned null).
pub const TWIDGET_HOSTED_BUILT_NOTHING: i32 = 0;
/// `make` filled `*renderer`.
pub const TWIDGET_HOSTED_RENDERER: i32 = 1;
/// `make` threw, `*error` what it threw.
pub const TWIDGET_HOSTED_THREW: i32 = 2;

/// The engine process's renderer factory (`HostedRendererFactory`): `make` is called on the serving thread at a
/// widget's hosted start with the opening's resources, the colour scheme, the bubble's width, the widget's
/// platform (the glue's to release once with `twidget_hosted_platform_release`, whatever `make` returns) and its
/// link (valid until the renderer's `free` returned, or until `make` returns when it built none). It returns
/// `TWIDGET_HOSTED_RENDERER` with `*renderer` filled, `TWIDGET_HOSTED_BUILT_NOTHING`, or `TWIDGET_HOSTED_THREW`
/// with `*error` the UTF-8 text it threw, which the glue keeps until its next `make` or the end of serving.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct TwidgetHostedRenderers {
    pub context: *mut c_void,
    #[allow(clippy::type_complexity)]
    pub make: Option<
        unsafe extern "C" fn(
            context: *mut c_void,
            resources: *const TwidgetHostedResources,
            theme: *const u8,
            theme_size: usize,
            width: f64,
            platform: *mut TwidgetHostedPlatform,
            link: *mut TwidgetHostedLink,
            renderer: *mut TwidgetHostedRenderer,
            error: *mut TwidgetEngineBytes,
        ) -> i32,
    >,
}

/// A widget's platform as the glue holds it (`std::shared_ptr<SandboxHostedPlatform>`): usable from any thread
/// until the glue releases it, also past its widget and past serving (then it sends nothing and answers none).
#[derive(Debug)]
pub struct TwidgetHostedPlatform {
    platform: Arc<HostedPlatform>,
}

/// A renderer's ends of `onMessage` and `onDiag`: empty until the served host connects the renderer, what the
/// renderer reports before going nowhere.
#[derive(Debug)]
pub struct TwidgetHostedLink {
    link: Mutex<Option<RendererLink>>,
}

impl TwidgetHostedLink {
    fn link(&self) -> Option<RendererLink> {
        self.link.lock().unwrap_or_else(std::sync::PoisonError::into_inner).clone()
    }
}

/// A glue's renderer table and whether it was freed, behind the serving guard.
struct CRendererTable {
    c: TwidgetHostedRenderer,
    guard: Arc<Guard>,
    freed: Mutex<bool>,
}

// SAFETY: the glue's contract (twidget_engine.h): the table's functions are called one at a time, on the thread
// the served host calls its renderer on, and `free` on any thread after the others; this crate calls them so.
unsafe impl Send for CRendererTable {}
// SAFETY: as above: shared between the host thread (the calls) and the platform's observer, which run on the
// same thread, and the drop, which the freed flag orders after them.
unsafe impl Sync for CRendererTable {}

impl CRendererTable {
    fn call(&self, f: Option<unsafe extern "C" fn(*mut c_void)>) {
        if let Some(f) = f {
            // SAFETY: the glue's function with its own context, while serving, before `free`.
            self.guard.run((), || unsafe { f(self.c.context) });
        }
    }

    fn call_bytes(&self, f: Option<unsafe extern "C" fn(*mut c_void, *const u8, usize)>, bytes: &[u8]) {
        if let Some(f) = f {
            // SAFETY: as above; `bytes` is readable for its length for the whole call.
            self.guard.run((), || unsafe { f(self.c.context, bytes.as_ptr(), bytes.len()) });
        }
    }

    /// The host's platform message, never once the renderer was freed.
    fn platform_message(&self, message: &[u8]) {
        let freed = self.freed.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        if !*freed {
            self.call_bytes(self.c.platform_message, message);
        }
    }
}

/// A glue's renderer as the served host holds it.
struct CRenderer {
    table: Arc<CRendererTable>,
    link: Arc<TwidgetHostedLink>,
    /// The glue's reference to `link`, given back after `free`.
    link_for_c: *const TwidgetHostedLink,
}

// SAFETY: `link_for_c` is only given back in `drop`; the table's own promise covers the rest.
unsafe impl Send for CRenderer {}
// SAFETY: as above.
unsafe impl Sync for CRenderer {}

impl Renderer for CRenderer {
    fn connect(&self, link: RendererLink) {
        *self.link.link.lock().unwrap_or_else(std::sync::PoisonError::into_inner) = Some(link);
    }
    fn load(&self) {
        self.table.call(self.table.c.load);
    }
    fn send(&self, json: &[u8]) {
        self.table.call_bytes(self.table.c.send, json);
    }
    fn set_theme(&self, theme: &[u8]) {
        self.table.call_bytes(self.table.c.set_theme, theme);
    }
    fn set_width(&self, width: f64) {
        if let Some(f) = self.table.c.set_width {
            // SAFETY: the glue's function with its own context, while serving, before `free`.
            self.table.guard.run((), || unsafe { f(self.table.c.context, width) });
        }
    }
    fn destroy(&self) {
        self.table.call(self.table.c.destroy);
    }
}

impl Drop for CRenderer {
    fn drop(&mut self) {
        {
            let mut freed = self.table.freed.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
            *freed = true;
            self.table.call(self.table.c.free);
        }
        // SAFETY: the reference `Arc::into_raw` gave the glue, which `free` ended; given back once.
        drop(unsafe { Arc::from_raw(self.link_for_c) });
    }
}

/// The factory over the glue's table: each widget's platform and link handed over, its renderer wrapped.
fn factory(c: TwidgetHostedRenderers, guard: Arc<Guard>) -> HostedRendererFactory {
    struct Table(TwidgetHostedRenderers);
    // SAFETY: the glue's contract: `make` is called on the serving thread alone, while serving (the guard).
    unsafe impl Send for Table {}
    // SAFETY: as above.
    unsafe impl Sync for Table {}
    let table = Table(c);
    Arc::new(move |resources: Arc<SandboxResources>, theme: &[u8], width: f64, platform: Arc<HostedPlatform>| {
        let table = &table;
        let Some(make) = table.0.make else { return Ok(None) };
        let handle = Box::into_raw(Box::new(TwidgetHostedPlatform { platform: Arc::clone(&platform) }));
        let link = Arc::new(TwidgetHostedLink { link: Mutex::new(None) });
        let link_for_c = Arc::into_raw(Arc::clone(&link));
        let given = TwidgetHostedResources {
            sandbox_bundle: TwidgetEngineBytes::of(&resources.sandbox_bundle),
            validator_bundle: TwidgetEngineBytes::of(&resources.validator_bundle),
            renderer_bundle: TwidgetEngineBytes::of(&resources.renderer_bundle),
        };
        let mut built = TwidgetHostedRenderer {
            context: std::ptr::null_mut(),
            load: None,
            send: None,
            set_theme: None,
            set_width: None,
            destroy: None,
            platform_message: None,
            free: None,
        };
        let mut error = TwidgetEngineBytes { bytes: std::ptr::null(), size: 0 };
        let mut called = false;
        let made = guard.run(TWIDGET_HOSTED_BUILT_NOTHING, || {
            called = true;
            // SAFETY: the glue's factory with its own context, while serving; every pointer it gets is valid for
            // the call, the platform handle becomes its own and the link stays valid as the table says.
            unsafe {
                make(table.0.context, &given, theme.as_ptr(), theme.len(), width, handle, link_for_c.cast_mut(), &mut built, &mut error)
            }
        });
        // Serving ended before the call: the glue never got the handle.
        if !called {
            // SAFETY: `handle` came from `Box::into_raw` above and the glue never saw it.
            drop(unsafe { Box::from_raw(handle) });
        }
        if made == TWIDGET_HOSTED_RENDERER {
            let table = Arc::new(CRendererTable { c: built, guard: Arc::clone(&guard), freed: Mutex::new(false) });
            let observer = Arc::clone(&table);
            platform.set_on_message(move |message| observer.platform_message(message));
            return Ok(Some(Box::new(CRenderer { table, link, link_for_c }) as Box<dyn Renderer>));
        }
        // SAFETY: the glue's reference to the link ends when `make` returns without a renderer; given back once.
        drop(unsafe { Arc::from_raw(link_for_c) });
        if made == TWIDGET_HOSTED_THREW {
            // SAFETY: the glue's text, kept until its next `make`; copied at once.
            let what = unsafe { slice(error.bytes, error.size) };
            return Err(String::from_utf8_lossy(what).into_owned());
        }
        Ok(None)
    })
}

/// `twidget_engine_serve` for a stream whose opening may be a hosting one (`SandboxRemoteEngineEnd::serveHosting`):
/// a hosting opening's widgets run here with their engines, validators and the renderers `renderers` builds (null:
/// every widget dies, its factory having built nothing); an engine opening's are served as `twidget_engine_serve`
/// serves them.
///
/// # Safety
/// As `twidget_engine_serve`'s, and `renderers` is null or a valid table whose `make` may be called on this
/// thread, and whose renderers' functions as `TwidgetHostedRenderer` says, until this returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn twidget_engine_serve_hosting(
    stream: *const TwidgetEngineStream,
    platform: *const TwidgetEnginePlatform,
    build: *const u8,
    build_size: usize,
    lockdown: *const TwidgetEngineLockdown,
    renderers: *const TwidgetHostedRenderers,
    reason_size: *mut usize,
) -> *mut u8 {
    // SAFETY: the caller's promises, read once and copied.
    let (served, renderers) = unsafe { (Served::new(stream, platform, build, build_size, lockdown), renderers.as_ref().copied()) };
    let renderers = renderers.map(|c| factory(c, Arc::clone(&served.guard)));
    // SAFETY: the caller's promises, passed on.
    unsafe {
        served.run(reason_size, move |stream, build, lockdown, platform| {
            twidget_host::serve_hosting(stream, build, lockdown, platform, renderers)
        })
    }
}

/// A message of the renderer for the host's platform (`SandboxHostedPlatform::send`), from any thread.
///
/// # Safety
/// `platform` is a handle the glue still holds; `bytes` is readable for `size` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn twidget_hosted_platform_send(platform: *const TwidgetHostedPlatform, bytes: *const u8, size: usize) {
    // SAFETY: the caller's promises.
    let (platform, bytes) = unsafe { (&*platform, slice(bytes, size)) };
    platform.platform.send(bytes);
}

/// A request the renderer waits for (`SandboxHostedPlatform::request`), from any thread but never inside `make`:
/// true with the host's answer (`*answer_size` bytes to free with `twidget_engine_free`, null for an empty one),
/// false for none.
///
/// # Safety
/// `platform` is a handle the glue still holds; `bytes` is readable for `size` bytes; `answer` and `answer_size`
/// are writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn twidget_hosted_platform_request(
    platform: *const TwidgetHostedPlatform,
    bytes: *const u8,
    size: usize,
    answer: *mut *mut u8,
    answer_size: *mut usize,
) -> bool {
    // SAFETY: the caller's promises.
    let (platform, bytes) = unsafe { (&*platform, slice(bytes, size)) };
    match platform.platform.request(bytes) {
        Some(text) => {
            // SAFETY: the caller's writable places.
            unsafe { *answer = leak(text, answer_size) };
            true
        }
        None => {
            // SAFETY: as above.
            unsafe {
                *answer = std::ptr::null_mut();
                *answer_size = 0;
            }
            false
        }
    }
}

/// The glue lets go of a widget's platform, once per handle `make` gave it.
///
/// # Safety
/// `platform` is such a handle, released once; null does nothing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn twidget_hosted_platform_release(platform: *mut TwidgetHostedPlatform) {
    if platform.is_null() {
        return;
    }
    // SAFETY: the box `make`'s caller leaked for the glue, given back once.
    drop(unsafe { Box::from_raw(platform) });
}

/// Renderer → host message (JSON, renderer.h's `onMessage`), from any thread, inside `send` too.
///
/// # Safety
/// `link` is valid (until the renderer's `free` returned); `bytes` is readable for `size` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn twidget_hosted_renderer_message(link: *const TwidgetHostedLink, bytes: *const u8, size: usize) {
    // SAFETY: the caller's promises.
    let (link, bytes) = unsafe { (&*link, slice(bytes, size)) };
    if let Some(link) = link.link() {
        link.message(bytes);
    }
}

/// The renderer's diagnostic (renderer.h's `onDiag`), from any thread.
///
/// # Safety
/// `link` is valid (until the renderer's `free` returned); `kind` and `detail` are readable for their sizes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn twidget_hosted_renderer_diag(
    link: *const TwidgetHostedLink,
    kind: *const u8,
    kind_size: usize,
    detail: *const u8,
    detail_size: usize,
) {
    // SAFETY: the caller's promises.
    let (link, kind, detail) = unsafe { (&*link, slice(kind, kind_size), slice(detail, detail_size)) };
    if let Some(link) = link.link() {
        link.diag(kind, detail);
    }
}
