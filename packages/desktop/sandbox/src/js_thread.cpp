#include "twidget/js_thread.h"

#include <cstring>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace twidget {

// A thread of the reserve tells the JSThread it ran that its loop is over here: the JSThread may be destroyed as soon
// as its join() sees it, so the thread keeps this alive (not the JSThread) for the last thing it touches.
struct JSThread::Done {
    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
};

#ifndef _WIN32
namespace {
void nameThisThread(const std::string& name) {
#if defined(__APPLE__)
    pthread_setname_np(name.c_str());
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#endif
}

// The threads that wait to be taken, and the JSThreads handed to them. Never destroyed: its threads live as long as
// the process.
struct Reserve {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<JSThread*> handed;
    size_t waiting = 0;
};
Reserve& reserveState() {
    static Reserve* state = new Reserve;
    return *state;
}
} // namespace

void JSThread::reserve(size_t count) {
    for (size_t i = 0; i < count; i++) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, kStackSize);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t thread;
        int error = pthread_create(&thread, &attr, &JSThread::parked, nullptr);
        pthread_attr_destroy(&attr);
        if (error != 0) break;
    }
}

size_t JSThread::reserved() {
    Reserve& r = reserveState();
    std::lock_guard<std::mutex> lock(r.mutex);
    return r.waiting;
}

void* JSThread::parked(void*) {
    Reserve& r = reserveState();
    for (;;) {
        nameThisThread("widget-spare");
        JSThread* self;
        {
            std::unique_lock<std::mutex> lock(r.mutex);
            r.waiting++;
            r.cv.wait(lock, [&] { return !r.handed.empty(); });
            self = r.handed.front();
            r.handed.pop_front();
        }
        self->adopt();
    }
    return nullptr;
}

void JSThread::adopt() {
    nameThisThread(name_);
    id_.store(std::this_thread::get_id());
    std::shared_ptr<Done> done = done_;
    started_->set_value();
    run();
    std::lock_guard<std::mutex> lock(done->mutex);
    done->finished = true;
    done->cv.notify_all();
}
#else
void JSThread::reserve(size_t) {}
size_t JSThread::reserved() { return 0; }
void* JSThread::parked(void*) { return nullptr; }
void JSThread::adopt() {}
#endif

JSThread::JSThread(std::string name) : name_(std::move(name)) {
    std::promise<void> started;
    auto startedFuture = started.get_future();
#ifndef _WIN32
    {
        Reserve& r = reserveState();
        std::unique_lock<std::mutex> lock(r.mutex);
        if (r.waiting > 0) {
            r.waiting--;
            started_ = &started;
            done_ = std::make_shared<Done>();
            r.handed.push_back(this);
            lock.unlock();
            r.cv.notify_all();
            startedFuture.wait();
            return;
        }
    }
#endif
    struct Boot { JSThread* self; std::promise<void>* started; };
    auto* boot = new Boot{this, &started};
#ifdef _WIN32
    auto* h = new HANDLE;
    *h = (HANDLE)_beginthreadex(nullptr, (unsigned)kStackSize, [](void* p) -> unsigned {
        auto* b = static_cast<Boot*>(p);
        b->self->id_.store(std::this_thread::get_id());
        b->started->set_value();
        JSThread* self = b->self;
        delete b;
        self->run();
        return 0;
    }, boot, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    handle_ = h;
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kStackSize);
    auto* t = new pthread_t;
    int error = pthread_create(t, &attr, &JSThread::entry, boot);
    pthread_attr_destroy(&attr);
    if (error != 0) {
        delete t;
        delete boot;
        throw std::system_error(error, std::generic_category(), "JSThread " + name_ + ": no thread");
    }
    handle_ = t;
#endif
    startedFuture.wait();
}

void* JSThread::entry(void* p) {
    struct Boot { JSThread* self; std::promise<void>* started; };
    auto* b = static_cast<Boot*>(p);
    JSThread* self = b->self;
#if defined(__APPLE__)
    pthread_setname_np(self->name_.c_str());
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), self->name_.substr(0, 15).c_str());
#endif
    self->id_.store(std::this_thread::get_id());
    b->started->set_value();
    delete b;
    self->run();
    return nullptr;
}

JSThread::~JSThread() {
    join();
    if (!handle_) return;
#ifdef _WIN32
    delete static_cast<HANDLE*>(handle_);
#else
    delete static_cast<pthread_t*>(handle_);
#endif
}

bool JSThread::post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return false;
        tasks_.push_back(std::move(fn));
    }
    cv_.notify_one();
    return true;
}

void JSThread::async(std::function<void()> fn) { post(std::move(fn)); }

std::shared_ptr<JSThread::Scheduled> JSThread::after(double seconds, std::function<void()> fn) {
    auto handle = std::make_shared<Scheduled>();
    auto at = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds < 0 ? 0 : seconds));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) { handle->cancel(); return handle; }
        // A cancelled timer waits here until its time, up to 24 h for a widget's: once the queue has doubled since the
        // last sweep they are swept out, so a widget setting and clearing timers in a loop cannot grow it (Kotlin's
        // removeCallbacks, Swift's Timer.invalidate).
        if (timers_.size() >= sweepAt_) {
            std::erase_if(timers_, [](const auto& e) { return e.second.handle->cancelled(); });
            sweepAt_ = std::max<size_t>(64, timers_.size() * 2);
        }
        timers_.emplace(at, Timer{handle, std::move(fn)});
    }
    cv_.notify_one();
    return handle;
}

size_t JSThread::queuedTimers() {
    std::lock_guard<std::mutex> lock(mutex_);
    return timers_.size();
}

void JSThread::stop() {
    // Like CFRunLoopStop performed as a block: everything queued before runs first.
    bool posted = post([this] { stopRequested_ = true; });
    if (!posted) return;
}

void JSThread::join() {
    stop();
    if (isCurrent()) return; // cannot join ourselves; the loop exits after this task
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (joined_) return;
        joined_ = true;
    }
    if (done_) {
        std::unique_lock<std::mutex> lock(done_->mutex);
        done_->cv.wait(lock, [&] { return done_->finished; });
        return;
    }
#ifdef _WIN32
    HANDLE h = *static_cast<HANDLE*>(handle_);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
#else
    pthread_join(*static_cast<pthread_t*>(handle_), nullptr);
#endif
}

void JSThread::run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (stopRequested_) {
                    stopping_ = true;
                    for (auto& [_, t] : timers_) t.handle->cancel();
                    timers_.clear();
                    tasks_.clear();
                    return;
                }
                if (!tasks_.empty()) {
                    task = std::move(tasks_.front());
                    tasks_.pop_front();
                    break;
                }
                auto now = std::chrono::steady_clock::now();
                if (!timers_.empty() && timers_.begin()->first <= now) {
                    auto it = timers_.begin();
                    Timer t = std::move(it->second);
                    timers_.erase(it);
                    if (!t.handle->cancelled()) task = std::move(t.fn);
                    if (task) break;
                    continue;
                }
                if (timers_.empty()) cv_.wait(lock);
                else cv_.wait_until(lock, timers_.begin()->first);
            }
        }
        task();
    }
}

} // namespace twidget
