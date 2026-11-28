//! `js_thread.h`: an engine is pinned to one thread with a known, large stack (the engine counts its native
//! stack from each call into it): every engine gets a dedicated thread of [`STACK_SIZE`] bytes running a queue
//! of tasks and timers. The state the tasks share lives on the thread, made there, never crossing it.

use std::collections::{BTreeMap, VecDeque};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{JoinHandle, ThreadId};
use std::time::{Duration, Instant};

/// Real stack of the thread; a context's limits must stay below it.
pub const STACK_SIZE: usize = 8 << 20;
/// Safe stack limit for a context that only ever runs on a JS thread.
pub const JS_STACK_LIMIT: usize = 6 << 20;

/// A timer's handle: cancelled, it never starts.
#[derive(Debug, Default)]
pub struct Scheduled {
    cancelled: AtomicBool,
}

impl Scheduled {
    /// Safe from any thread; the task will not start after this returns.
    pub fn cancel(&self) {
        self.cancelled.store(true, Ordering::Release);
    }

    pub fn cancelled(&self) -> bool {
        self.cancelled.load(Ordering::Acquire)
    }
}

type Task<S> = Box<dyn FnOnce(&S) + Send>;

struct Timer<S> {
    handle: Arc<Scheduled>,
    task: Task<S>,
}

struct Queue<S> {
    tasks: VecDeque<Task<S>>,
    /// By deadline, then by the order they were set, as a multimap keeps equal keys.
    timers: BTreeMap<(Instant, u64), Timer<S>>,
    order: u64,
    /// The queue's size that sweeps out cancelled timers.
    sweep_at: usize,
    /// Set by the stop task itself, on the thread.
    stop_requested: bool,
    /// No more work accepted.
    stopping: bool,
}

struct Shared<S> {
    queue: Mutex<Queue<S>>,
    wake: Condvar,
}

/// One thread and its queue; `S` is the state its tasks get, made on the thread by the constructor's `init`.
pub struct JsThread<S: 'static> {
    shared: Arc<Shared<S>>,
    handle: Mutex<Option<JoinHandle<()>>>,
    id: ThreadId,
}

impl<S> std::fmt::Debug for JsThread<S> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("JsThread").field("id", &self.id).finish()
    }
}

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

impl<S: 'static> JsThread<S> {
    /// Starts the thread named `name`, makes its state there with `init` and waits until it runs. The
    /// thread's OS refusal comes back as `JSThread <name>: <why>`, and nothing is left behind then.
    pub fn new(name: &str, init: impl FnOnce() -> S + Send + 'static) -> Result<JsThread<S>, String> {
        JsThread::with_poster(name, move |_| init())
    }

    /// `new`, the state made with a [`Poster`] of its own thread, through which its tasks queue more work.
    pub fn with_poster(name: &str, init: impl FnOnce(Poster<S>) -> S + Send + 'static) -> Result<JsThread<S>, String> {
        let shared = Arc::new(Shared {
            queue: Mutex::new(Queue {
                tasks: VecDeque::new(),
                timers: BTreeMap::new(),
                order: 0,
                sweep_at: 64,
                stop_requested: false,
                stopping: false,
            }),
            wake: Condvar::new(),
        });
        let loop_shared = Arc::clone(&shared);
        let (started_tx, started_rx) = std::sync::mpsc::sync_channel::<()>(1);
        let handle = std::thread::Builder::new()
            .name(name.to_string())
            .stack_size(STACK_SIZE)
            .spawn(move || {
                let _ = started_tx.send(());
                let poster = Poster { shared: Arc::clone(&loop_shared), id: std::thread::current().id() };
                let state = init(poster);
                run(&loop_shared, &state);
            })
            .map_err(|e| format!("JSThread {name}: {e}"))?;
        let id = handle.thread().id();
        let _ = started_rx.recv();
        Ok(JsThread { shared, handle: Mutex::new(Some(handle)), id })
    }

    /// A handle that queues work on this thread from anywhere.
    pub fn poster(&self) -> Poster<S> {
        Poster { shared: Arc::clone(&self.shared), id: self.id }
    }

    pub fn is_current(&self) -> bool {
        std::thread::current().id() == self.id
    }

    fn post(&self, task: Task<S>) -> bool {
        self.shared.post(task)
    }

    /// Runs `task` on the thread after everything queued before it; dropped once stopped.
    pub fn post_task(&self, task: impl FnOnce(&S) + Send + 'static) {
        self.post(Box::new(task));
    }

    /// Runs `task` on the thread and waits for its result; None when the thread stopped before it ran. Never
    /// from the thread itself.
    pub fn sync<R: Send + 'static>(&self, task: impl FnOnce(&S) -> R + Send + 'static) -> Option<R> {
        debug_assert!(!self.is_current(), "sync from the thread itself");
        let (tx, rx) = std::sync::mpsc::sync_channel(1);
        if !self.post(Box::new(move |s| {
            let _ = tx.send(task(s));
        })) {
            return None;
        }
        rx.recv().ok()
    }

    /// Runs `task` on the thread after `seconds` unless cancelled.
    pub fn after(&self, seconds: f64, task: impl FnOnce(&S) + Send + 'static) -> Arc<Scheduled> {
        self.shared.after(seconds, Box::new(task))
    }

    /// Timers in the queue, cancelled ones not yet swept included.
    pub fn queued_timers(&self) -> usize {
        lock(&self.shared.queue).timers.len()
    }

    /// Stops the loop once queued work has run; the thread then exits. Idempotent.
    pub fn stop(&self) {
        let shared = Arc::clone(&self.shared);
        self.post(Box::new(move |_| lock(&shared.queue).stop_requested = true));
    }

    /// `stop` and wait for the thread to exit (nothing from the thread itself).
    pub fn join(&self) {
        self.stop();
        if self.is_current() {
            return;
        }
        let handle = lock(&self.handle).take();
        if let Some(handle) = handle {
            let _ = handle.join();
        }
    }
}

impl<S> Shared<S> {
    fn post(&self, task: Task<S>) -> bool {
        {
            let mut q = lock(&self.queue);
            if q.stopping {
                return false;
            }
            q.tasks.push_back(task);
        }
        self.wake.notify_one();
        true
    }

    fn after(&self, seconds: f64, task: Task<S>) -> Arc<Scheduled> {
        let handle = Arc::new(Scheduled::default());
        let delay = if seconds < 0.0 || seconds.is_nan() { 0.0 } else { seconds };
        let at = Instant::now() + Duration::try_from_secs_f64(delay).unwrap_or(Duration::MAX / 2);
        {
            let mut q = lock(&self.queue);
            if q.stopping {
                handle.cancel();
                return handle;
            }
            // A cancelled timer waits here until its time: once the queue has doubled since the last sweep they
            // are swept out, so a widget setting and clearing timers in a loop cannot grow it.
            if q.timers.len() >= q.sweep_at {
                q.timers.retain(|_, t| !t.handle.cancelled());
                q.sweep_at = 64.max(q.timers.len() * 2);
            }
            let order = q.order;
            q.order += 1;
            q.timers.insert((at, order), Timer { handle: Arc::clone(&handle), task });
        }
        self.wake.notify_one();
        handle
    }
}

/// What queues work on a [`JsThread`] without owning it: its state's own handle to its thread (timers set by
/// the code running there), or a completion's from any thread. Work queued once the thread stopped is dropped.
pub struct Poster<S: 'static> {
    shared: Arc<Shared<S>>,
    id: ThreadId,
}

impl<S> Clone for Poster<S> {
    fn clone(&self) -> Self {
        Poster { shared: Arc::clone(&self.shared), id: self.id }
    }
}

impl<S> std::fmt::Debug for Poster<S> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Poster").field("id", &self.id).finish()
    }
}

impl<S: 'static> Poster<S> {
    /// Runs `task` on the thread after everything queued before it: false (and dropped) once stopped.
    pub fn post_task(&self, task: impl FnOnce(&S) + Send + 'static) -> bool {
        self.shared.post(Box::new(task))
    }

    /// Runs `task` on the thread after `seconds` unless cancelled.
    pub fn after(&self, seconds: f64, task: impl FnOnce(&S) + Send + 'static) -> Arc<Scheduled> {
        self.shared.after(seconds, Box::new(task))
    }

    pub fn is_current(&self) -> bool {
        std::thread::current().id() == self.id
    }
}

impl<S: 'static> Drop for JsThread<S> {
    fn drop(&mut self) {
        self.join();
    }
}

fn run<S>(shared: &Shared<S>, state: &S) {
    loop {
        let task = {
            let mut q = lock(&shared.queue);
            loop {
                if q.stop_requested {
                    q.stopping = true;
                    for t in q.timers.values() {
                        t.handle.cancel();
                    }
                    q.timers.clear();
                    q.tasks.clear();
                    return;
                }
                if let Some(task) = q.tasks.pop_front() {
                    break task;
                }
                let now = Instant::now();
                let first = q.timers.keys().next().copied();
                match first {
                    Some(key) if key.0 <= now => {
                        let t = q.timers.remove(&key).expect("the first timer");
                        if !t.handle.cancelled() {
                            break t.task;
                        }
                    }
                    Some(key) => {
                        let wait = key.0 - now;
                        q = shared.wake.wait_timeout(q, wait).unwrap_or_else(std::sync::PoisonError::into_inner).0;
                    }
                    None => q = shared.wake.wait(q).unwrap_or_else(std::sync::PoisonError::into_inner),
                }
            }
        };
        // As an exception that leaves a C++ JSThread task terminates the process: nothing of a broken engine
        // lingers, and whoever waits on it (a probe, the host's stream) sees the process end.
        if std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| task(state))).is_err() {
            std::process::abort();
        }
    }
}
