// Byte streams for the remote engine's transport (remote_stream.h) that the tests and the harness share, never the library's: an in-memory pair, the TW_ENGINE_STREAM switch over it and, on POSIX, a socket pair whose writes really block.
#pragma once

#include "twidget/remote_stream.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace twidget {

/// One end of `memoryStreamPair`: reads what the other end wrote, in order, and writes what it reads.
class MemoryStream final : public SandboxRemoteStream {
public:
    /// One direction's bytes and who waits on them, under the pair's lock.
    struct Lane {
        std::string bytes;
        size_t head = 0;
        bool writerClosed = false;
        bool readerClosed = false;
        int readers = 0;
        int writers = 0;
        size_t size() const { return bytes.size() - head; }
    };
    /// What both ends share: a bounded lane each way under one lock.
    struct Pair {
        explicit Pair(size_t capacity) : capacity(std::max<size_t>(capacity, 1)) {}
        const size_t capacity;
        std::mutex mutex;
        std::condition_variable changed;
        bool shut = false;
        Lane lanes[2];
    };

    MemoryStream(std::shared_ptr<Pair> pair, int side) : pair_(std::move(pair)), in_(pair_->lanes[side]), out_(pair_->lanes[1 - side]) {}
    /// `close`, as a process that exits closes its end.
    ~MemoryStream() override { close(); }
    MemoryStream(const MemoryStream&) = delete;
    MemoryStream& operator=(const MemoryStream&) = delete;

    size_t read(char* out, size_t size) override {
        std::unique_lock<std::mutex> l(pair_->mutex);
        const auto over = [&] { return pair_->shut || in_.readerClosed || !size; };
        if (!over() && !in_.size() && !in_.writerClosed) {
            in_.readers++;
            pair_->changed.wait(l, [&] { return over() || in_.size() || in_.writerClosed; });
            in_.readers--;
        }
        if (over() || !in_.size()) return 0;
        const size_t n = std::min(size, in_.size());
        std::memcpy(out, in_.bytes.data() + in_.head, n);
        in_.head += n;
        if (in_.head == in_.bytes.size()) {
            in_.bytes.clear();
            in_.head = 0;
        } else if (in_.head >= pair_->capacity) {
            in_.bytes.erase(0, in_.head);
            in_.head = 0;
        }
        pair_->changed.notify_all();
        return n;
    }

    /// A write past the room left goes in parts, each as the other end's reads make room.
    bool write(std::string_view bytes) override {
        std::unique_lock<std::mutex> l(pair_->mutex);
        const auto over = [&] { return pair_->shut || out_.writerClosed || out_.readerClosed; };
        for (;;) {
            if (over()) return false;
            if (bytes.empty()) return true;
            if (out_.size() >= pair_->capacity) {
                out_.writers++;
                pair_->changed.wait(l, [&] { return over() || out_.size() < pair_->capacity; });
                out_.writers--;
                continue;
            }
            const size_t n = std::min(bytes.size(), pair_->capacity - out_.size());
            out_.bytes.append(bytes.data(), n);
            bytes.remove_prefix(n);
            pair_->changed.notify_all();
        }
    }

    /// Both ends at once: what is queued either way is dropped.
    void shutdown() override {
        {
            std::lock_guard<std::mutex> l(pair_->mutex);
            pair_->shut = true;
        }
        pair_->changed.notify_all();
    }

    /// This end is done: the other end reads what is left, then 0, and its writes fail, as after the peer closed a socket; this end's later calls fail.
    void close() {
        {
            std::lock_guard<std::mutex> l(pair_->mutex);
            out_.writerClosed = true;
            in_.readerClosed = true;
        }
        pair_->changed.notify_all();
    }

    /// Whether a `read` on this end waits for bytes now, for a test that has to see it blocked.
    bool readWaiting() {
        std::lock_guard<std::mutex> l(pair_->mutex);
        return in_.readers > 0;
    }
    /// Whether a `write` on this end waits for room now.
    bool writeWaiting() {
        std::lock_guard<std::mutex> l(pair_->mutex);
        return out_.writers > 0;
    }

private:
    const std::shared_ptr<Pair> pair_;
    Lane& in_;
    Lane& out_;
};

struct MemoryStreamPair {
    std::shared_ptr<MemoryStream> host;
    std::shared_ptr<MemoryStream> engine;
};

/// Two ends joined by a queue of `capacity` bytes each way (at least 1): a write waits while its queue is full and a read while it is empty, until the other end makes progress, closes its end or either end shuts the pair down.
inline MemoryStreamPair memoryStreamPair(size_t capacity) {
    auto pair = std::make_shared<MemoryStream::Pair>(capacity);
    return {std::make_shared<MemoryStream>(pair, 0), std::make_shared<MemoryStream>(pair, 1)};
}

/// The test switch `TW_ENGINE_STREAM=1` of twidget-tests and twidget-run, in any build: its factory gives every engine `SandboxEngine::make` builds a channel on a host's end, joined by a memory pair to the engine process's end serving on a worker of its own, one stream for each resources object it is given, which it keeps.
class StreamSwitch {
public:
    /// What a host's end ends with when the switch stops it.
    static constexpr std::string_view kStopped = "the host stopped the stream";

    /// Whether the environment asks for it.
    static bool requested() {
        const char* value = std::getenv("TW_ENGINE_STREAM");
        return value && std::string_view(value) == "1";
    }
    /// How many times a switch's factory ran on the calling thread so far: compared around a `make`, whether that engine's channel came from it.
    static size_t madeHere() { return made(); }

    /// Both ends of each of its streams name `build`.
    explicit StreamSwitch(std::string build) : state_(std::make_shared<State>(std::move(build))) {}
    /// `finish`.
    ~StreamSwitch() { finish(); }
    StreamSwitch(const StreamSwitch&) = delete;
    StreamSwitch& operator=(const StreamSwitch&) = delete;

    /// The factory to install with `SandboxEngine::setRemoteChannelFactory`; a copy left after `finish` gives refusing channels.
    SandboxRemoteChannelFactory factory() const {
        return [state = state_](std::shared_ptr<SandboxResources> resources) {
            made()++;
            return state->channel(std::move(resources));
        };
    }
    /// The first time: clears the global factory, then ends every stream at once (abortively: an engine's end may read a cut last frame), stops each host's end and joins each engine's end, on the calling thread. Every time: how each stream that ended otherwise ended, a host's end for another reason than this stop, an engine's end before it.
    std::vector<std::string> finish() { return state_->finish(); }
    /// The streams opened so far.
    size_t streams() const {
        std::lock_guard<std::mutex> l(state_->mutex);
        return state_->opened;
    }
    /// The channels the factory gave so far.
    size_t channels() const {
        std::lock_guard<std::mutex> l(state_->mutex);
        return state_->given;
    }

private:
    static size_t& made() {
        thread_local size_t calls = 0;
        return calls;
    }

    /// How the streams ended, from `widget.monitor` and the engine's workers: it owns no thread, so its last owner may be any thread.
    struct Ends {
        std::mutex mutex;
        bool finishing = false;
        std::vector<std::string> unexpected;

        void ended(size_t stream, const std::string& why) {
            std::lock_guard<std::mutex> l(mutex);
            if (why != kStopped) unexpected.push_back("stream " + std::to_string(stream) + " ended: " + why);
        }
        void served(size_t stream, const std::string& why) {
            std::lock_guard<std::mutex> l(mutex);
            if (!finishing) unexpected.push_back("the engine's end of stream " + std::to_string(stream) + " returned " + (why.empty() ? std::string("at the end of the stream") : why));
        }
        std::vector<std::string> all() {
            std::lock_guard<std::mutex> l(mutex);
            return unexpected;
        }
    };

    /// One stream: the resources it was opened for, kept so that no other object takes their address, its host's end and the worker its engine's end serves on.
    struct Session {
        std::shared_ptr<SandboxResources> resources;
        std::shared_ptr<SandboxRemoteHostEnd> end;
        std::thread serving;
    };

    struct State {
        explicit State(std::string build) : build(std::move(build)) {}

        std::shared_ptr<SandboxRemoteChannel> channel(std::shared_ptr<SandboxResources> resources) {
            std::lock_guard<std::mutex> l(mutex);
            if (finished) return SandboxRemoteHostEnd::refusing("the stream switch finished");
            if (!resources) return SandboxRemoteHostEnd::refusing("no resources to open a stream with");
            auto found = std::find_if(sessions.begin(), sessions.end(), [&](const std::unique_ptr<Session>& s) { return s->resources == resources; });
            if (found == sessions.end()) {
                const size_t stream = ++opened;
                MemoryStreamPair pair = memoryStreamPair(size_t(1) << 20);
                auto session = std::make_unique<Session>();
                session->resources = resources;
                session->end = SandboxRemoteHostEnd::start(pair.host, *resources, build, {nullptr, [ends = ends, stream](const std::string& why) { ends->ended(stream, why); }}, {});
                session->serving = std::thread([engine = pair.engine, build = build, ends = ends, stream] { ends->served(stream, SandboxRemoteEngineEnd::serve(*engine, build)); });
                found = sessions.insert(sessions.end(), std::move(session));
            }
            given++;
            return (*found)->end->channel();
        }

        std::vector<std::string> finish() {
            std::vector<std::unique_ptr<Session>> ending;
            {
                std::lock_guard<std::mutex> l(mutex);
                if (finished) return ends->all();
                finished = true;
                ending.swap(sessions);
            }
            SandboxEngine::setRemoteChannelFactory(nullptr);
            {
                std::lock_guard<std::mutex> l(ends->mutex);
                ends->finishing = true;
            }
            for (const std::unique_ptr<Session>& s : ending) s->end->stop();
            for (const std::unique_ptr<Session>& s : ending)
                if (s->serving.joinable()) s->serving.join();
            return ends->all();
        }

        const std::string build;
        const std::shared_ptr<Ends> ends = std::make_shared<Ends>();
        std::mutex mutex;
        bool finished = false;
        size_t opened = 0;
        size_t given = 0;
        std::vector<std::unique_ptr<Session>> sessions;
    };

    const std::shared_ptr<State> state_;
};

#ifndef _WIN32
/// One end of `socketStreamPair`, the descriptor its own until it closes: an interrupted call goes on, a short write writes the rest, a write to a gone peer fails without SIGPIPE, and `shutdown` is the socket's for both directions.
class SocketStream final : public SandboxRemoteStream {
public:
    explicit SocketStream(int fd) : fd_(fd) {}
    ~SocketStream() override { close(); }
    SocketStream(const SocketStream&) = delete;
    SocketStream& operator=(const SocketStream&) = delete;

    size_t read(char* out, size_t size) override {
        if (shut_ || !size) return 0;
        Running running(readers_);
        for (;;) {
            const ssize_t n = ::recv(fd_, out, size, 0);
            if (n > 0) return size_t(n);
            if (n < 0 && errno == EINTR && !shut_) continue;
            return 0;
        }
    }

    bool write(std::string_view bytes) override {
        Running running(writers_);
        while (!bytes.empty()) {
            if (shut_) return false;
            left_ = bytes.size();
            const ssize_t n = ::send(fd_, bytes.data(), bytes.size(), kSendFlags);
            left_ = 0;
            if (n > 0) bytes.remove_prefix(size_t(n));
            else if (n < 0 && errno == EINTR) continue;
            else return false;
        }
        return true;
    }

    /// Each direction on its own: once the peer ended its writes, macOS refuses SHUT_RDWR whole and leaves a blocked write blocked.
    void shutdown() override {
        shut_ = true;
        ::shutdown(fd_, SHUT_WR);
        ::shutdown(fd_, SHUT_RD);
    }

    /// As a process that exits: the descriptor closes, the peer reads what is left, then the end, and its writes fail; only while no other call of this end runs.
    void close() {
        shut_ = true;
        if (const int fd = fd_.exchange(-1); fd >= 0) ::close(fd);
    }
    /// A half close: the peer reads what was written, then the end, while this end may still read and the peer's writes go on filling it.
    void finishWriting() { ::shutdown(fd_, SHUT_WR); }
    /// The descriptor, -1 once closed: for a test that hands a duplicate of it to a child process.
    int descriptor() const { return fd_; }

    /// Whether a `read` of this end runs now.
    bool readWaiting() const { return readers_ > 0; }
    /// Whether a `write` of this end runs now.
    bool writing() const { return writers_ > 0; }
    /// Whether a `write` of this end is in the socket with bytes of it left and the socket takes none now: with a peer that reads nothing and more to write than the socket holds, that write is blocked.
    bool writeBlocked() const {
        if (!writers_ || !left_) return false;
        pollfd p{fd_, POLLOUT, 0};
        return ::poll(&p, 1, 0) == 0;
    }

private:
#ifdef MSG_NOSIGNAL
    static constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    static constexpr int kSendFlags = 0;
#endif

    /// Counts a call of this end while it runs.
    struct Running {
        explicit Running(std::atomic<int>& count) : count(count) { count++; }
        ~Running() { count--; }
        std::atomic<int>& count;
    };

    std::atomic<int> fd_;
    std::atomic<bool> shut_{false};
    std::atomic<int> readers_{0};
    std::atomic<int> writers_{0};
    std::atomic<size_t> left_{0};
};

struct SocketStreamPair {
    std::shared_ptr<SocketStream> host;
    std::shared_ptr<SocketStream> engine;
};

/// A connected `socketpair(AF_UNIX, SOCK_STREAM)`, both ends close-on-exec and without SIGPIPE; `buffer` above 0 asks for that many bytes of send and receive room on each end (the OS may round it). Null ends when the OS gives no such pair.
inline SocketStreamPair socketStreamPair(int buffer = 0) {
    int fds[2] = {-1, -1};
#ifdef SOCK_CLOEXEC
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) return {};
#else
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return {};
#endif
    SocketStreamPair pair{std::make_shared<SocketStream>(fds[0]), std::make_shared<SocketStream>(fds[1])};
    for (const int fd : fds) {
        bool set = true;
#ifndef SOCK_CLOEXEC
        set = set && ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
#endif
#ifdef SO_NOSIGPIPE
        const int on = 1;
        set = set && ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) == 0;
#endif
        if (buffer > 0) set = set && ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)) == 0 && ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer)) == 0;
        if (!set) return {};
    }
    return pair;
}
#endif

} // namespace twidget
