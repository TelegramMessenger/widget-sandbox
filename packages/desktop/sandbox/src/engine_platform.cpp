// What the engine asks the operating system (twidget/engine_platform.h): the thread CPU clock with its calibration, the steady clock, the time zone and the CSPRNG, apart from Nova so an engine process whose end is in Rust links them alone.
#include "twidget/engine_platform.h"

#include "steady_clock.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#if (defined(_M_IX86) || defined(_M_X64)) && !defined(_M_ARM64EC) // ARM64EC code defines _M_X64 too
#include <intrin.h>
#define TW_TSC 1
#endif
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__ANDROID__)
#include <stdlib.h>
#include <time.h>
#else
#include <sys/random.h>
#include <time.h>
#endif

#include <algorithm>
#include <cstring>
#include <limits>

namespace twidget::platform {

#ifdef _WIN32
namespace {

constexpr double kFileTimeTicksPerSecond = 1e7;

struct ThreadClockSource {
    bool cycles = false;
    double ticksPerSecond = kFileTimeTicksPerSecond;
};

struct ThreadClock {
    double seconds = 0.;
    uint64_t count = 0;
    bool counting = false;
};

#ifdef TW_TSC
constexpr unsigned kHypervisorPresent = 1u << 31;
constexpr unsigned kInvariantTsc = 1u << 8;
constexpr unsigned kCreatePartitions = 1u << 0;
constexpr unsigned kNestedHypervisor = 1u << 12;
constexpr double kMinTscRate = 1e8;
constexpr double kMaxTscRate = 1e11;

struct CpuidLeaf {
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
};

CpuidLeaf readCpuid(unsigned leaf) {
    int registers[4] = {};
    __cpuid(registers, int(leaf));
    return {unsigned(registers[0]), unsigned(registers[1]), unsigned(registers[2]), unsigned(registers[3])};
}

bool onArm64Windows() {
    using Function = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto address = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
    const auto isWow64Process2 = reinterpret_cast<Function>(reinterpret_cast<void*>(address));
    if (!isWow64Process2) return false; // missing before Windows 10 1709, older than any ARM64 Windows
    auto process = USHORT(0);
    auto native = USHORT(0);
    return !isWow64Process2(GetCurrentProcess(), &process, &native) || native == IMAGE_FILE_MACHINE_ARM64;
}

// Hyper-V's root partition, not nested within Hyper-V, as Hyper-V's own leaves identify it:
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/feature-discovery
bool hyperVRootPartition() {
    if (readCpuid(0x40000000).eax < 0x40000004) return false;
    const auto signature = readCpuid(0x40000001).eax;
    if (std::memcmp(&signature, "Hv#1", sizeof(signature)) != 0) return false;
    if (!(readCpuid(0x40000003).ebx & kCreatePartitions)) return false;
    return !(readCpuid(0x40000004).eax & kNestedHypervisor);
}

double enumeratedTscRate() {
    if (onArm64Windows()) return 0.;
    const auto basic = readCpuid(0);
    const unsigned vendor[] = {basic.ebx, basic.edx, basic.ecx};
    if (std::memcmp(vendor, "GenuineIntel", sizeof(vendor)) != 0 || basic.eax < 0x15) return 0.;
    if (readCpuid(0x80000000).eax < 0x80000007) return 0.;
    if (!(readCpuid(0x80000007).edx & kInvariantTsc)) return 0.;
    if ((readCpuid(1).ecx & kHypervisorPresent) && !hyperVRootPartition()) return 0.;
    const auto tsc = readCpuid(0x15);
    const auto denominator = tsc.eax;
    const auto numerator = tsc.ebx;
    const auto crystalHz = tsc.ecx;
    if (!denominator || !numerator || !crystalHz) return 0.;
    const auto rate = double(crystalHz) * numerator / denominator;
    return (rate >= kMinTscRate && rate <= kMaxTscRate) ? rate : 0.;
}
#endif

const ThreadClockSource& threadClockSource() {
    static const ThreadClockSource source = [] {
#ifdef TW_TSC
        const auto rate = enumeratedTscRate();
        if (rate > 0.) return ThreadClockSource{true, rate};
#endif
        return ThreadClockSource();
    }();
    return source;
}

} // namespace
#endif

double threadCpuSeconds() {
#ifdef _WIN32
    const auto& source = threadClockSource();
    thread_local ThreadClock state;
    auto count = uint64_t(0);
    auto ok = false;
    if (source.cycles) {
        auto cycles = ULONG64(0);
        ok = QueryThreadCycleTime(GetCurrentThread(), &cycles) != FALSE;
        count = cycles;
    } else {
        FILETIME creation{}, exit{}, kernel{}, user{};
        ok = GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user) != FALSE;
        auto ticks = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        count = ticks(kernel) + ticks(user);
    }
    if (!ok) {
        state.counting = false;
        return state.seconds;
    }
    if (state.counting && count > state.count) state.seconds += double(count - state.count) / source.ticksPerSecond;
    state.count = count;
    state.counting = true;
    return state.seconds;
#else
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return double(t.tv_sec) + double(t.tv_nsec) / 1e9;
#endif
}

double steadySeconds() { return detail::steadySeconds(); }

int32_t timeZoneOffset(int64_t epochSeconds) {
#ifdef _WIN32
    (void)epochSeconds;
    TIME_ZONE_INFORMATION info{};
    const DWORD id = GetTimeZoneInformation(&info);
    if (id == TIME_ZONE_ID_INVALID) return 0;
    const LONG bias = info.Bias + ((id == TIME_ZONE_ID_DAYLIGHT) ? info.DaylightBias : 0);
    return int32_t(-bias * 60);
#else
    const auto clamped = std::clamp<int64_t>(epochSeconds, std::numeric_limits<time_t>::min(), std::numeric_limits<time_t>::max());
    const time_t seconds = time_t(clamped);
    struct tm local {};
    if (!localtime_r(&seconds, &local)) return 0;
    return int32_t(local.tm_gmtoff);
#endif
}

void randomBytes(uint8_t* buf, size_t n) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__ANDROID__)
    arc4random_buf(buf, n);
#elif defined(__linux__)
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(buf + got, n - got, 0);
        if (r <= 0) break;
        got += size_t(r);
    }
#elif defined(_WIN32)
    BCryptGenRandom(nullptr, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
#error "no CSPRNG for this platform"
#endif
}

} // namespace twidget::platform
