// The remote engine over one byte stream per engine process, the byte-level twin of the loopback (remote_engine.h): the stream a host supplies, the frames on it, the host's end with a channel per runtime and the engine process's end.
#pragma once

#include "twidget/js_thread.h"
#include "twidget/remote_engine.h"
#include "twidget/resources.h"
#include "twidget/sandbox_engine.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace twidget {

/// One duplex byte stream to an engine process, the OS boundary a host supplies (a POSIX socket, a Windows pipe): one reader and one writer at a time, each of which may block until `shutdown`; the owner destroys it only after both returned.
class SandboxRemoteStream {
public:
    virtual ~SandboxRemoteStream() = default;
    /// Blocks until at least one byte came, then copies up to `size` bytes to `out` and returns how many; 0 at the end of the stream, on an error and after `shutdown`.
    virtual size_t read(char* out, size_t size) = 0;
    /// Blocks until all of `bytes` is written: true, or false once the stream broke or was shut down, with how much of it went unknown.
    virtual bool write(std::string_view bytes) = 0;
    /// From any thread, also while a `read` or `write` blocks, idempotent and without waiting: a blocked `read` and `write` return, and every later one fails.
    virtual void shutdown() = 0;
};

/// The frames on a stream, all integers little-endian: a `u32` size of the bytes after it (at least 5), a `u32` runtime id (0 is the connection), a `u8` kind, then the kind's fields in order and nothing after them. A `str` is a `u32` length and that many raw bytes, an `i64` 8 bytes of two's complement, an `f64` the 8 bytes of its IEEE-754 pattern, a `bool` one byte of 0 or 1; the strings are the string protocol's (the `__host` JSON, `SandboxRemoteCoding`'s forms).
struct SandboxRemoteWire {
    /// The stream's version, in `Opening` and `Ready`.
    static constexpr int64_t kVersion = 1;
    /// The size any frame but `Opening` may give, in both directions: an ops batch with room for its frame.
    static constexpr size_t kMaxFrameBytes = SandboxEngine::kRemoteOpsLimit + (size_t(64) << 10);
    /// The size `Opening` may give: the sandbox bundle and its bytecode.
    static constexpr size_t kMaxOpeningBytes = size_t(16) << 20;
    /// Bytes of a stamp line in `Opening` (`BundleStamp`): a longer one is refused, never cut.
    static constexpr size_t kMaxStampBytes = 4096;

    /// Host to engine from 1, engine to host from 64: a kind of the other direction is an unknown one. Each kind's fields in order.
    enum class Kind : uint8_t {
        /// On the connection, first: i64 version, str build, str sandboxBundle, str sandboxBytecode, str validatorStamp, str rendererStamp (each stamp the bundle's first line, "" for no renderer bundle).
        Opening = 1,
        /// A new runtime, its id above every earlier one's: no fields.
        Open = 2,
        /// str html, str config (`SandboxRemoteCoding::encodeConfig`).
        Start = 3,
        /// str json.
        Event = 4,
        /// str json, i64 gesture (within `int`).
        Activation = 5,
        /// f64 time.
        AnimationFrame = 6,
        /// f64 innerWidth, f64 innerHeight, f64 dpr.
        Viewport = 7,
        /// str entriesJson.
        Resize = 8,
        /// str colorScheme, str params (`SandboxRemoteCoding::encodeThemeParams`).
        Theme = 9,
        /// str area (`SandboxRemoteCoding::encodeSafeArea`).
        SafeArea = 10,
        /// str data (`SandboxRemoteCoding::encodeData`).
        Data = 11,
        /// bool visible.
        Visible = 12,
        /// No fields.
        PressBegan = 13,
        /// No fields.
        PressEnded = 14,
        /// f64 ms.
        ChargeValidator = 15,
        /// str reason.
        Stop = 16,
        /// i64 id, str outcome (`SandboxRemoteCoding::encodeOutcome`).
        Answer = 17,
        /// i64 seq (from 1), str code.
        Probe = 18,
        /// str json, bool hasBinary, str binary ("" unless hasBinary).
        QueryAnswer = 19,
        /// str text.
        IntlAnswer = 20,
        /// The runtime closed: no fields.
        Close = 21,
        /// On the connection, the answer to `Opening`: i64 version.
        Ready = 64,
        /// On the connection, the answer to `Opening`: str reason.
        Refused = 65,
        /// No fields.
        TaskStarted = 66,
        /// No fields.
        TaskCompleted = 67,
        /// str json.
        Ops = 68,
        /// str json; synchronous, answered by `QueryAnswer`.
        Query = 69,
        /// str requestJson; synchronous, answered by `IntlAnswer`.
        Intl = 70,
        /// str json.
        Listen = 71,
        /// str json.
        PreventDefault = 72,
        /// str json.
        Anim = 73,
        /// No fields.
        RequestsFrame = 74,
        /// str level, str text.
        Log = 75,
        /// str kind, str detail.
        Diag = 76,
        /// str method, str argsJson.
        Bridge = 77,
        /// str text.
        CopyText = 78,
        /// i64 id, str data, bool wallet.
        Request = 79,
        /// i64 id, str payload (`SandboxRemoteCoding::encodeWallet`).
        Wallet = 80,
        /// str commitJson.
        Storage = 81,
        /// str edge.
        ResizeAnchor = 82,
        /// bool on.
        WatchesPublicData = 83,
        /// No fields.
        Loaded = 84,
        /// 11 f64 in `SandboxRemoteCoding::encodeStats`' order.
        Stats = 85,
        /// str reason.
        Died = 86,
        /// i64 seq (from 1), str text.
        ProbeAnswer = 87,
    };
};

/// The host's end of one stream: it opens the stream with the sandbox bundle, hands out a channel per runtime for `SandboxEngine::setRemoteChannelFactory` and gives each frame of the engine process to its runtime's sink. A frame it cannot accept ends the whole stream, and every open runtime's client hears `remoteDisconnected` once. Its threads: `widget.reader` reads, `widget.writer` alone blocks in writes, `widget.monitor` bounds those writes and reports to the owner, and each channel's `widget.answer` answers its runtime's queries and Intl requests through the client.
class SandboxRemoteHostEnd {
public:
    /// What the owner hears, from `widget.monitor` and outside every lock of this end; an empty one is skipped. Each returns at once and posts what it does, never stopping, destroying or calling into this end.
    struct Events {
        /// How many runtimes are open now: changes close together may come as their last count, and a stream that ends with runtimes open reports 0 before `ended`.
        std::function<void(size_t open)> runtimes;
        /// Once and last: why the stream ended.
        std::function<void(const std::string& reason)> ended;
    };
    /// The bounds of one stream, each positive and finite; another value is its default.
    struct Limits {
        /// How long a channel's `probe` waits for its answer ("ERROR timeout" past it); the client's barriers around the call wait for its dispatch thread as in process.
        double probeWaitMs = 10000;
        /// How long one write to the stream (at most 64 KiB of a frame) may block before the stream ends, without another call needed.
        double writeStallMs = 10000;
        /// Bytes of frames kept to write, the one being written included; past them, or past `SandboxEngine::kRemoteQueuedCalls` frames, the stream ends.
        size_t queuedBytes = size_t(64) << 20;
    };

    /// Starts this end's threads on `stream` and queues the opening: `resources`' sandbox bundle and its bytecode, and only the stamp lines of its validator and renderer bundles, which stay in the host. `build` is the host's build, which the engine process's end must share.
    static std::shared_ptr<SandboxRemoteHostEnd> start(std::shared_ptr<SandboxRemoteStream> stream, const SandboxResources& resources, std::string build, Events events, Limits limits);
    /// `stop`, on the owner's thread: the last reference is never dropped on a thread of this end's (inside `Events`).
    ~SandboxRemoteHostEnd();
    SandboxRemoteHostEnd(const SandboxRemoteHostEnd&) = delete;
    SandboxRemoteHostEnd& operator=(const SandboxRemoteHostEnd&) = delete;

    /// A channel to a new runtime on this stream, which runs the opening's bundles whatever resources the factory was given; from any thread, without waiting. Once the stream ended, one whose `attach` reports `remoteDisconnected`.
    std::shared_ptr<SandboxRemoteChannel> channel();
    /// Ends the stream at once (what is queued is dropped, and the engine process may see a cut frame), joins `widget.reader`, `widget.writer` and `widget.monitor`, then lets the stream go; `ended` came before it returns. Idempotent, on the owner's thread. A channel joins its `widget.answer` when it is destroyed; no delegate or provider code that already runs is cut short.
    void stop();

    /// A channel for a host that has no engine process: `attach` reports `remoteDied(reason)` from inside, `probe` answers "ERROR no runtime" and every other call does nothing.
    static std::shared_ptr<SandboxRemoteChannel> refusing(std::string reason);

private:
    struct Connection;
    explicit SandboxRemoteHostEnd(std::shared_ptr<Connection> connection);

    const std::shared_ptr<Connection> connection_;
    std::unique_ptr<JSThread> reader_;
    std::unique_ptr<JSThread> writer_;
    std::unique_ptr<JSThread> monitor_;
};

/// The engine process's end of one stream: no Qt, no file read, so a host may run it before Qt starts.
struct SandboxRemoteEngineEnd {
    /// Serves `stream` from the calling thread, its reader: checks the opening (a malformed one, another version or another build than `build` is refused), builds the runtimes' resources from its bundles, keeps a `SandboxRemoteServer` per runtime id on its own `widget.sandbox` thread and writes their frames under one lock; a runtime's probes run on its `widget.probe` thread and closed runtimes are destroyed on `widget.reaper`. Returns why the stream ended, "" at its end on a frame's boundary, once every runtime and thread it started is gone; a `shutdown` of `stream` from another thread ends it.
    static std::string serve(SandboxRemoteStream& stream, const std::string& build);
};

} // namespace twidget
