// Byte streams for the remote engine's transport (remote_stream.h) that the tests and the harness share, never the library's: an in-memory pair (memory_stream.h), the TW_ENGINE_STREAM switch over it and, on POSIX, a socket pair whose writes really block.
#pragma once

#include "memory_stream.h"
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
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace twidget {

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

    /// Waits for bytes outside `recv`: on macOS a blocking `recv` that starts to wait as the end comes (the peer's SHUT_WR, this end's own SHUT_RD) can sleep on with the end in its socket.
    size_t read(char* out, size_t size) override {
        if (shut_ || !size) return 0;
        Running running(readers_);
        for (;;) {
            const ssize_t n = ::recv(fd_, out, size, MSG_DONTWAIT);
            if (n > 0) return size_t(n);
            if (n == 0 || shut_) return 0;
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return 0;
            if (!waitReadable() || shut_) return 0;
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

    /// Until the socket has bytes or its end, with no timeout: true then and when a signal cut the wait, so `read` tries again; false when the wait failed or the descriptor is gone. `select` while the descriptor fits an `fd_set` (it still works where RLIMIT_NOFILE is 0, which fails every `poll`), `poll` past FD_SETSIZE.
    bool waitReadable() const {
        const int fd = fd_;
        if (fd < 0) return false;
        if (fd < FD_SETSIZE) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(fd, &set);
            if (::select(fd + 1, &set, nullptr, nullptr, nullptr) >= 0) return true;
        } else {
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, -1) >= 0) return true;
        }
        return errno == EINTR;
    }

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
