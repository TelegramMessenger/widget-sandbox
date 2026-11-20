// The in-memory byte stream pair of the remote engine's transport (remote_stream.h), test and harness code, never the library's, with nothing but the standard library and the stream's interface: stream_peers.h's switch runs over it, and a host's own checks of a stream (an embedder's self-check with an engine process's end played by hand) include it alone.
#pragma once

#include "twidget/remote_stream.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

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

} // namespace twidget
