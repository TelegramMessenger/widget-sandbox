// CPU quota policy (docs/decisions.md, Runtime limits): a sliding window of
// `cpuQuotaWindowMs` split into ten buckets; a widget may spend `cpuQuotaMs`
// of JS time per window. Beyond that, deferrable tasks (timers, frames, …) are
// postponed to the moment the oldest busy bucket leaves the window — they are
// never dropped or killed. Events always run, with a floor of EVENT_FLOOR_MS.
//
// The glue keeps two of them (docs/decisions.md, "Strict limits from the start"):
// the JS quota (`cpuQuotaMs`, JS time only) and the main-thread window
// (`mainThreadQuotaMs`): the host's own work for the widget, its layout queries
// and the modelled cost of the ops it forwards (`opsCostMs`). A deferrable task
// waits while either window is spent.
//
// Pure and side-effect free: the glue (worker.ts, the Node harness,
// CpuBudget.swift, CpuBudget.kt, cpu_budget.cpp) feeds it `now` and task durations and acts on
// the decisions. All four copies are checked against tests/fixtures/cpu-budget.json.

export type TaskKind = 'boot' | 'event' | 'visibility' | 'timer' | 'frame' | 'viewport' | 'resize' | 'theme' | 'bridge';

export interface CpuBudgetConfig {
  cpuLimitMs: number;
  cpuQuotaMs: number;
  cpuQuotaWindowMs: number;
  cpuKillAfterMs: number;
}

export type Decision =
  | { run: true; sliceMs: number }
  | { run: false; retryAt: number };

export const BUCKETS = 10;
/** Minimum slice any task gets, so user input is handled even with the quota spent. */
export const FLOOR_MS = 50;
/** Kinds that may be postponed. `boot`, `event` and `visibility` always run. */
export const DEFERRABLE: ReadonlySet<string> = new Set(['timer', 'frame', 'viewport', 'resize', 'theme', 'bridge']);
/** A task's layout queries may block the host for at most this much, whatever its slice (boot included). */
export const QUERY_BUDGET_MS = 100;
/** Layout-query budget of one task: the host answers queries synchronously, so a forced-layout loop would hold the renderer hostage without ever burning widget CPU; past this the task is interrupted and killed. */
export function queryBudgetMs(_sliceMs: number): number {
  return QUERY_BUDGET_MS;
}

/** Modelled main-thread cost of one op the glue forwards to the validator, and of one canvas command in it. */
export const OP_COST_MS = 0.025;
export const CANVAS_COMMAND_COST_MS = 0.002;
/** What a batch of `ops` ops with `canvasCommands` canvas commands costs the host's main thread: deterministic, from what the widget sent. */
export function opsCostMs(ops: number, canvasCommands: number): number {
  return ops * OP_COST_MS + canvasCommands * CANVAS_COMMAND_COST_MS;
}

export class CpuBudget {
  private readonly bucketMs: number;
  private readonly buckets: { start: number; ms: number }[] = [];
  private throttledSince = -1;
  private lastThrottleAt = -1;

  constructor(private readonly cfg: CpuBudgetConfig) {
    this.bucketMs = Math.max(1, cfg.cpuQuotaWindowMs / BUCKETS);
    for (let i = 0; i < BUCKETS; i++) this.buckets.push({ start: -Infinity, ms: 0 });
  }

  /** Account `cpuMs` of widget JS time for a task that ended at `now`. */
  record(_kind: TaskKind | string, cpuMs: number, now: number): void {
    const b = Math.floor(now / this.bucketMs);
    const slot = this.buckets[((b % BUCKETS) + BUCKETS) % BUCKETS];
    const start = b * this.bucketMs;
    if (slot.start !== start) { slot.start = start; slot.ms = 0; }
    if (cpuMs > 0) slot.ms += cpuMs; // NaN and negatives are ignored
  }

  /** JS time used in the window ending now (the ten bucket slots cover exactly one window). */
  used(now: number): number {
    const oldest = (Math.floor(now / this.bucketMs) - BUCKETS + 1) * this.bucketMs;
    let sum = 0;
    for (const s of this.buckets) if (s.start >= oldest) sum += s.ms;
    return sum;
  }

  /** Before a task: run it with this slice for the interrupt handler, or retry at `retryAt`. */
  decide(kind: TaskKind | string, now: number): Decision {
    if (kind === 'boot') return { run: true, sliceMs: this.cfg.cpuLimitMs };
    const remaining = this.cfg.cpuQuotaMs - this.used(now);
    if (DEFERRABLE.has(kind) && remaining <= 0) {
      // A throttling stretch continues while deferrals are at most one window apart.
      if (this.throttledSince < 0 || now - this.lastThrottleAt > this.cfg.cpuQuotaWindowMs) this.throttledSince = now;
      this.lastThrottleAt = now;
      const oldestBusy = (Math.floor(now / this.bucketMs) - BUCKETS + 1) * this.bucketMs;
      let oldest = Infinity;
      for (const s of this.buckets) if (s.ms > 0 && s.start >= oldestBusy && s.start < oldest) oldest = s.start;
      let retryAt = oldest === Infinity ? now + this.bucketMs : oldest + this.cfg.cpuQuotaWindowMs;
      if (retryAt <= now) retryAt = now + this.bucketMs;
      return { run: false, retryAt };
    }
    return { run: true, sliceMs: Math.min(this.cfg.cpuLimitMs, Math.max(remaining, FLOOR_MS)) };
  }

  /** Length of the current uninterrupted throttling stretch (0 when not throttled). */
  throttledFor(now: number): number {
    if (this.throttledSince < 0 || now - this.lastThrottleAt > this.cfg.cpuQuotaWindowMs) return 0;
    return now - this.throttledSince;
  }

  /** `cpuKillAfterMs` (0 = never): kill the runtime once throttling has lasted this long. */
  shouldKill(now: number): boolean {
    return this.cfg.cpuKillAfterMs > 0 && this.throttledFor(now) >= this.cfg.cpuKillAfterMs;
  }
}
