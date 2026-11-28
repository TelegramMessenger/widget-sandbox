//! widget-sandbox's CPU quota policy (`packages/desktop/sandbox/include/twidget/cpu_budget.h`,
//! `src/cpu_budget.cpp`; cpu-budget.ts and its Swift and Kotlin twins). A sliding window of
//! `cpu_quota_window_ms` in ten buckets; a widget may spend `cpu_quota_ms` of JS time per window, beyond
//! that deferrable tasks are postponed until the oldest busy bucket leaves the window, never dropped or
//! killed. Events always run. Every task gets the whole `cpu_limit_ms` slice; what it spends past the
//! quota is a debt (at most one more quota) that enters the window as its buckets leave. The engine keeps
//! two of them: the JS quota and the main-thread window (`ops_cost_ms`). Times are parameters, in ms on
//! one clock; checked against `packages/core/tests/fixtures/cpu-budget.json`.

#![forbid(unsafe_code)]

/// What a task may do now: run with `slice_ms`, or wait until `retry_at`.
#[derive(Clone, Copy, Debug)]
pub struct CpuDecision {
    pub run: bool,
    pub slice_ms: f64,
    pub retry_at: f64,
}

impl PartialEq for CpuDecision {
    fn eq(&self, o: &CpuDecision) -> bool {
        self.run == o.run && if self.run { self.slice_ms == o.slice_ms } else { self.retry_at == o.retry_at }
    }
}

/// `std::min`: `b` when it is less than `a`, else `a`.
fn min(a: f64, b: f64) -> f64 {
    if b < a { b } else { a }
}

/// `std::max`: `b` when `a` is less than it, else `a`.
fn max(a: f64, b: f64) -> f64 {
    if a < b { b } else { a }
}

const BUCKETS_F: f64 = CpuBudget::BUCKETS as f64;

/// One budget: the window's buckets, the debt and the throttling stretch.
#[derive(Clone, Debug)]
pub struct CpuBudget {
    cpu_limit_ms: f64,
    cpu_quota_ms: f64,
    window_ms: f64,
    cpu_kill_after_ms: f64,
    bucket_ms: f64,
    starts: [f64; CpuBudget::BUCKETS as usize],
    ms: [f64; CpuBudget::BUCKETS as usize],
    debt_ms: f64,
    settled: f64,
    throttled_since: f64,
    last_throttle_at: f64,
}

impl CpuBudget {
    pub const BUCKETS: i32 = 10;
    /// Layout queries the host may answer in one task (cached answers are free), whatever its slice.
    pub const QUERY_LIMIT: i32 = 128;
    /// What a task's layout queries may block the host in all: the hang guard behind the count.
    pub const QUERY_BUDGET_MS: f64 = 1000.0;
    /// Modelled main-thread cost of one op the glue forwards to the validator.
    pub const OP_COST_MS: f64 = 0.025;
    /// Modelled main-thread cost of one canvas command in an op.
    pub const CANVAS_COMMAND_COST_MS: f64 = 0.002;

    /// Kinds that may be postponed; `boot`, `event` and `visibility` always run.
    pub fn deferrable(kind: &[u8]) -> bool {
        matches!(kind, b"timer" | b"frame" | b"viewport" | b"resize" | b"theme" | b"bridge")
    }

    /// Whether a task with `queries` answered layout queries that blocked the host `query_ms` in all is past
    /// its limit: "count", "time" or "" (none).
    pub fn query_limit_hit(queries: i32, query_ms: f64) -> &'static str {
        if queries > CpuBudget::QUERY_LIMIT {
            return "count";
        }
        if query_ms > CpuBudget::QUERY_BUDGET_MS {
            return "time";
        }
        ""
    }

    /// The reason a task stopped by `query_limit_hit` is killed with.
    pub fn query_limit_reason(hit: &[u8], kind: &[u8]) -> Vec<u8> {
        let head = if hit == b"count" {
            format!("Layout query limit exceeded: more than {} layout queries in one ", CpuBudget::QUERY_LIMIT)
        } else {
            format!(
                "Layout query limit exceeded: host queries blocked the renderer for more than {} ms in one ",
                CpuBudget::QUERY_BUDGET_MS as i32
            )
        };
        [head.as_bytes(), kind, b" task"].concat()
    }

    /// What a batch of `ops` ops with `canvas_commands` canvas commands costs the host's main thread.
    pub fn ops_cost_ms(ops: f64, canvas_commands: f64) -> f64 {
        ops * CpuBudget::OP_COST_MS + canvas_commands * CpuBudget::CANVAS_COMMAND_COST_MS
    }

    pub fn new(cpu_limit_ms: f64, cpu_quota_ms: f64, cpu_quota_window_ms: f64, cpu_kill_after_ms: f64) -> CpuBudget {
        CpuBudget {
            cpu_limit_ms,
            cpu_quota_ms,
            window_ms: cpu_quota_window_ms,
            cpu_kill_after_ms,
            bucket_ms: max(1.0, cpu_quota_window_ms / BUCKETS_F),
            starts: [f64::NEG_INFINITY; CpuBudget::BUCKETS as usize],
            ms: [0.0; CpuBudget::BUCKETS as usize],
            debt_ms: 0.0,
            settled: f64::NEG_INFINITY,
            throttled_since: -1.0,
            last_throttle_at: -1.0,
        }
    }

    /// Bucket `b`'s slot, emptied when it still holds an older bucket; a bucket that is not finite (no time
    /// the host's clock gives) takes slot 0.
    fn slot(&mut self, b: f64) -> usize {
        let i = (((b % BUCKETS_F) + BUCKETS_F) % BUCKETS_F) as i32 as usize;
        let start = b * self.bucket_ms;
        if self.starts[i] != start {
            self.starts[i] = start;
            self.ms[i] = 0.0;
        }
        i
    }

    /// Time in the window that ends with bucket `b`.
    fn window_at(&self, b: f64) -> f64 {
        let oldest = (b - BUCKETS_F + 1.0) * self.bucket_ms;
        let mut sum = 0.0;
        for i in 0..CpuBudget::BUCKETS as usize {
            if self.starts[i] >= oldest {
                sum += self.ms[i];
            }
        }
        sum
    }

    /// Moves the debt into the window, bucket by bucket since the last call, as the buckets that filled it leave.
    fn settle(&mut self, now: f64) {
        let cur = (now / self.bucket_ms).floor();
        if self.debt_ms > 0.0 {
            let last = min(cur, self.settled + 2.0 * BUCKETS_F);
            let mut b = self.settled + 1.0;
            while b <= last && self.debt_ms > 0.0 {
                let pay = min(self.debt_ms, self.cpu_quota_ms - self.window_at(b));
                if pay > 0.0 {
                    let i = self.slot(b);
                    self.ms[i] += pay;
                    self.debt_ms -= pay;
                }
                b += 1.0;
            }
            if last < cur {
                self.debt_ms = 0.0;
            }
        }
        if cur > self.settled {
            self.settled = cur;
        }
    }

    /// Accounts `cpu_ms` of widget JS time for a task that ended at `now`: into the window up to the quota,
    /// the rest is the debt. NaN and negatives are ignored.
    pub fn record(&mut self, _kind: &[u8], cpu_ms: f64, now: f64) {
        self.settle(now);
        let b = (now / self.bucket_ms).floor();
        let i = self.slot(b);
        if cpu_ms.is_nan() || cpu_ms <= 0.0 {
            return;
        }
        let into = min(cpu_ms, max(0.0, self.cpu_quota_ms - self.window_at(b) - self.debt_ms));
        self.ms[i] += into;
        self.debt_ms = min(self.cpu_quota_ms, self.debt_ms + cpu_ms - into);
    }

    fn oldest_start(&self, now: f64) -> f64 {
        ((now / self.bucket_ms).floor() - BUCKETS_F + 1.0) * self.bucket_ms
    }

    /// JS time charged now: the window ending now plus the debt.
    pub fn used(&mut self, now: f64) -> f64 {
        self.settle(now);
        self.window_at((now / self.bucket_ms).floor()) + self.debt_ms
    }

    /// Before a task: run it with the whole slice for the interrupt handler, or retry at `retry_at`.
    pub fn decide(&mut self, kind: &[u8], now: f64) -> CpuDecision {
        let remaining = self.cpu_quota_ms - self.used(now);
        if CpuBudget::deferrable(kind) && remaining <= 0.0 {
            if self.throttled_since < 0.0 || now - self.last_throttle_at > self.window_ms {
                self.throttled_since = now;
            }
            self.last_throttle_at = now;
            let oldest_busy = self.oldest_start(now);
            let mut oldest = f64::INFINITY;
            for i in 0..CpuBudget::BUCKETS as usize {
                if self.ms[i] > 0.0 && self.starts[i] >= oldest_busy && self.starts[i] < oldest {
                    oldest = self.starts[i];
                }
            }
            let mut retry_at = if oldest == f64::INFINITY { now + self.bucket_ms } else { oldest + self.window_ms };
            if retry_at <= now {
                retry_at = now + self.bucket_ms;
            }
            return CpuDecision { run: false, slice_ms: 0.0, retry_at };
        }
        CpuDecision { run: true, slice_ms: self.cpu_limit_ms, retry_at: 0.0 }
    }

    /// Length of the current uninterrupted throttling stretch (0 when not throttled).
    pub fn throttled_for(&self, now: f64) -> f64 {
        if self.throttled_since < 0.0 || now - self.last_throttle_at > self.window_ms {
            return 0.0;
        }
        now - self.throttled_since
    }

    /// `cpu_kill_after_ms` (0 = never): kill the runtime once throttling has lasted this long.
    pub fn should_kill(&self, now: f64) -> bool {
        self.cpu_kill_after_ms > 0.0 && self.throttled_for(now) >= self.cpu_kill_after_ms
    }
}
