// CPU quota policy: a line-for-line mirror of src/host/cpu-budget.ts (see the
// comments there) and CpuBudget.swift. A sliding window of `cpuQuotaWindowMs`
// in ten buckets; a widget may spend `cpuQuotaMs` of JS time per window,
// beyond that deferrable tasks are postponed until the oldest busy bucket
// leaves the window — never dropped or killed. Events always run with a floor
// of 50 ms.
//
// Pure and side-effect free; checked against tests/fixtures/cpu-budget.json
// (copied to src/androidTest/assets by `pnpm build:android`).

package org.telegram.widgetsandbox

import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min

sealed class CpuDecision {
    data class Run(val sliceMs: Double) : CpuDecision()
    data class Defer(val retryAt: Double) : CpuDecision()
}

class CpuBudget(
    private val cpuLimitMs: Double,
    private val cpuQuotaMs: Double,
    private val windowMs: Double,
    private val cpuKillAfterMs: Double,
) {
    companion object {
        const val BUCKETS = 10
        /** Minimum slice any task gets, so user input is handled even with the quota spent. */
        const val FLOOR_MS = 50.0
        /** Kinds that may be postponed. `boot`, `event` and `visibility` always run. */
        val DEFERRABLE = setOf("timer", "frame", "viewport", "resize", "theme", "bridge")
        /** A task's layout queries may block the host for at most this much, or the task's slice if that is longer. */
        const val QUERY_BUDGET_MIN_MS = 100.0
        /** Layout-query budget of one task with [sliceMs] of CPU (cpu-budget.ts `queryBudgetMs`): past it the task is interrupted and killed. */
        fun queryBudgetMs(sliceMs: Double): Double = maxOf(sliceMs, QUERY_BUDGET_MIN_MS)
    }

    constructor(config: SandboxConfig) : this(config.cpuLimitMs, config.cpuQuotaMs, config.cpuQuotaWindowMs, config.cpuKillAfterMs)

    private val bucketMs = max(1.0, windowMs / BUCKETS)
    private val starts = DoubleArray(BUCKETS) { Double.NEGATIVE_INFINITY }
    private val ms = DoubleArray(BUCKETS)
    private var throttledSince = -1.0
    private var lastThrottleAt = -1.0

    private fun bucketIndex(now: Double): Long = floor(now / bucketMs).toLong()
    private fun oldestStart(now: Double): Double = (bucketIndex(now) - BUCKETS + 1) * bucketMs

    /** Account `cpuMs` of widget JS time for a task that ended at `now` (ms). */
    fun record(@Suppress("UNUSED_PARAMETER") kind: String, cpuMs: Double, now: Double) {
        val b = bucketIndex(now)
        val i = (((b % BUCKETS) + BUCKETS) % BUCKETS).toInt()
        val start = b * bucketMs
        if (starts[i] != start) { starts[i] = start; ms[i] = 0.0 }
        if (cpuMs > 0) ms[i] += cpuMs // NaN and negatives are ignored
    }

    /** JS time used in the window ending now. */
    fun used(now: Double): Double {
        val oldest = oldestStart(now)
        var sum = 0.0
        for (i in 0 until BUCKETS) if (starts[i] >= oldest) sum += ms[i]
        return sum
    }

    /** Before a task: run it with this slice for the interrupt handler, or retry at `retryAt`. */
    fun decide(kind: String, now: Double): CpuDecision {
        if (kind == "boot") return CpuDecision.Run(cpuLimitMs)
        val remaining = cpuQuotaMs - used(now)
        if (kind in DEFERRABLE && remaining <= 0) {
            // A throttling stretch continues while deferrals are at most one window apart.
            if (throttledSince < 0 || now - lastThrottleAt > windowMs) throttledSince = now
            lastThrottleAt = now
            val oldestBusy = oldestStart(now)
            var oldest = Double.POSITIVE_INFINITY
            for (i in 0 until BUCKETS) if (ms[i] > 0 && starts[i] >= oldestBusy && starts[i] < oldest) oldest = starts[i]
            var retryAt = if (oldest == Double.POSITIVE_INFINITY) now + bucketMs else oldest + windowMs
            if (retryAt <= now) retryAt = now + bucketMs
            return CpuDecision.Defer(retryAt)
        }
        return CpuDecision.Run(min(cpuLimitMs, max(remaining, FLOOR_MS)))
    }

    /** Length of the current uninterrupted throttling stretch (0 when not throttled). */
    fun throttledFor(now: Double): Double {
        if (throttledSince < 0 || now - lastThrottleAt > windowMs) return 0.0
        return now - throttledSince
    }

    /** `cpuKillAfterMs` (0 = never): kill the runtime once throttling has lasted this long. */
    fun shouldKill(now: Double): Boolean = cpuKillAfterMs > 0 && throttledFor(now) >= cpuKillAfterMs
}
