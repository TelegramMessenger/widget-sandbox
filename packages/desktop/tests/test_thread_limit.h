// What test_main.cpp (the JSThread owners' tests) and the transport's tests (test_remote_stream.cpp and test_remote_stream_hosted.cpp, through test_remote_stream_support.h) share for a thread the OS refuses to start (macOS: the per-process thread limit): the allowance that uses it up, the task's thread count, a refusal checked and a child case run in a fresh process; test_main.cpp defines the functions.
#pragma once

#ifdef __APPLE__
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <string>

/// The task's threads as Mach counts them, nullopt when they cannot be listed; every port and the array are given back.
std::optional<int> taskThreads();

/// Polls `taskThreads()` until it is `want`, for at most 2 s (a joined thread leaves the count a little later): the last count read.
std::optional<int> threadsSettleAt(int want);

std::string countText(const std::optional<int>& count);

/// This process's thread allowance taken up by parked threads on the smallest stacks: `leave(k)` frees exactly k of it, `fill()` takes it back, and every parked thread ends with this object.
class ThreadAllowance {
public:
    ThreadAllowance() = default;
    ThreadAllowance(const ThreadAllowance&) = delete;
    ThreadAllowance& operator=(const ThreadAllowance&) = delete;
    ~ThreadAllowance() {
        // Wake them all, then join: waking and joining one at a time takes twice as long.
        for (Slot& slot : slots_) wake(slot);
        for (Slot& slot : slots_) pthread_join(slot.thread, nullptr);
    }

    /// Parks threads until `pthread_create` refuses one with EAGAIN (at most 65536): the first fill sets `full()`, a later one must reach it again. Empty, or what went wrong.
    std::string fill() {
        pthread_attr_t attributes;
        if (const int error = pthread_attr_init(&attributes)) return std::string("pthread_attr_init: ") + std::strerror(error);
        std::string failure;
        if (const int error = pthread_attr_setstacksize(&attributes, PTHREAD_STACK_MIN)) failure = std::string("pthread_attr_setstacksize: ") + std::strerror(error);
        int refused = 0;
        for (int tries = 0; failure.empty() && !refused; tries++) {
            if (tries == 65536) {
                failure = "no thread refused in 65536";
                break;
            }
            Slot& slot = slots_.emplace_back();
            refused = pthread_create(&slot.thread, &attributes, &ThreadAllowance::park, &slot);
            if (refused) slots_.pop_back();
        }
        pthread_attr_destroy(&attributes);
        if (failure.empty() && refused != EAGAIN) failure = std::string("pthread_create refused a thread with ") + std::strerror(refused) + ", not EAGAIN";
        if (!failure.empty()) return failure;
        const std::optional<int> count = taskThreads();
        if (!count) return "the task's threads cannot be listed";
        if (!full_) full_ = *count;
        else if (*count != full_) return "full at " + std::to_string(*count) + " threads, the first time at " + std::to_string(full_);
        return {};
    }

    /// Ends the `k` threads parked last and waits until the task counts `full() - k` threads: empty, or what went wrong.
    std::string leave(int k) {
        if (k > int(slots_.size())) return "only " + std::to_string(slots_.size()) + " threads are parked";
        for (int i = 0; i < k; i++) wake(slots_[slots_.size() - 1 - i]);
        for (int i = 0; i < k; i++) {
            pthread_join(slots_.back().thread, nullptr);
            slots_.pop_back();
        }
        const std::optional<int> count = threadsSettleAt(full_ - k);
        if (count != full_ - k) return "the task counts " + countText(count) + " threads, not " + std::to_string(full_ - k);
        return {};
    }

    int full() const { return full_; }
    size_t parked() const { return slots_.size(); }

private:
    struct Slot {
        std::mutex mutex;
        std::condition_variable cv;
        bool stop = false;
        pthread_t thread{};
    };

    static void* park(void* p) {
        Slot& slot = *static_cast<Slot*>(p);
        std::unique_lock<std::mutex> lock(slot.mutex);
        slot.cv.wait(lock, [&] { return slot.stop; });
        return nullptr;
    }

    static void wake(Slot& slot) {
        {
            std::lock_guard<std::mutex> lock(slot.mutex);
            slot.stop = true;
        }
        slot.cv.notify_one();
    }

    std::deque<Slot> slots_;
    int full_ = 0;
};

/// With `k` threads free, `construct` must throw std::system_error EAGAIN from the start of `thread` within 1 s and the task's count must come back; then the allowance is full again. False when the child cannot go on.
bool expectNoStart(ThreadAllowance& allowance, const std::string& owner, int k, const std::string& thread, const std::function<void()>& construct);

/// How a child case run in a fresh process ended: `ok` when it exited with 0, else `ending` says how; `output` is what it printed.
struct ChildExit {
    bool ok = false;
    std::string ending;
    std::string output;
};

/// `twidget-tests --child <name>` with its output in a temporary file and no `TW_ENGINE_` switch in its environment (its owners are the in-process ones in every mode the suite runs in), waited for at most `seconds`: past them it is killed and reaped.
ChildExit runChild(const std::string& name, int seconds);
#endif
