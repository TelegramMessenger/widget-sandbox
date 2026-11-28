// widget-sandbox's CpuBudget on stdin, for rust/crates/cpu_budget's tests to compare with the Rust port.
//
// Records (oracle_io.h): 'N' four f64 (cpuLimitMs, cpuQuotaMs, cpuQuotaWindowMs, cpuKillAfterMs) makes a new budget and
// gets no answer. On it: 'r' kind, f64 cpuMs, f64 now: record; 'u' f64 now: used -> f64; 'd' kind, f64 now: decide -> u8
// run, f64 sliceMs, f64 retryAt; 'k' f64 now: nothing more. Each of the four answers status 1 with its own fields, then
// throttledFor(now) as f64 and shouldKill(now) as u8. The static ones: 'q' i32 queries, f64 ms -> queryLimitHit; 'R' hit,
// kind -> queryLimitReason; 'o' f64 ops, f64 canvasCommands -> opsCostMs as f64; 'f' kind -> deferrable as the status.
#include "oracle_io.h"
#include "twidget/cpu_budget.h"

#include <memory>

using twidget::CpuBudget;

int main() {
    std::unique_ptr<CpuBudget> budget;
    return oracle::serve([&](int op, oracle::In& in) {
        oracle::Out out;
        double now = 0;
        switch (op) {
        case 'N': {
            const double limit = in.f64(), quota = in.f64(), window = in.f64(), kill = in.f64();
            budget = std::make_unique<CpuBudget>(limit, quota, window, kill);
            return true;
        }
        case 'r': {
            const std::string kind = in.str();
            const double cpu = in.f64();
            now = in.f64();
            budget->record(kind, cpu, now);
            break;
        }
        case 'u':
            now = in.f64();
            out.f64(budget->used(now));
            break;
        case 'd': {
            const std::string kind = in.str();
            now = in.f64();
            const twidget::CpuDecision d = budget->decide(kind, now);
            out.flag(d.run).f64(d.sliceMs).f64(d.retryAt);
            break;
        }
        case 'k': now = in.f64(); break;
        case 'q': {
            const int32_t queries = in.i32();
            oracle::answer(1, CpuBudget::queryLimitHit(queries, in.f64()));
            return true;
        }
        case 'R': {
            const std::string hit = in.str(), kind = in.str();
            oracle::answer(1, CpuBudget::queryLimitReason(hit, kind));
            return true;
        }
        case 'o': {
            const double ops = in.f64();
            oracle::answer(1, out.f64(CpuBudget::opsCostMs(ops, in.f64())).s);
            return true;
        }
        case 'f': oracle::answer(CpuBudget::deferrable(in.str()) ? 1 : 0, {}); return true;
        default: return false;
        }
        out.f64(budget->throttledFor(now)).flag(budget->shouldKill(now));
        oracle::answer(1, out.s);
        return true;
    });
}
