/* The engine process's end in Rust (crates/engine_ffi): the C++ OS glue keeps the descriptors, the lockdown and the clocks, and serves an engine stream through these callbacks as SandboxRemoteEngineEnd::serve does; the same library carries Nova's C interface (nova_engine.h). */
#ifndef TWIDGET_ENGINE_H
#define TWIDGET_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The stream to the host's process: `read` blocks until at least one byte came and returns how many it copied (0 at the end, on an error and after `shutdown`), `write` blocks until all bytes are written (false once broken or shut down), `shutdown` wakes both from any thread without waiting; all three may be called from any thread, a read and a write at once, until twidget_engine_serve returns. */
typedef struct twidget_engine_stream {
    void* context;
    size_t (*read)(void* context, uint8_t* out, size_t size);
    bool (*write)(void* context, const uint8_t* bytes, size_t size);
    void (*shutdown)(void* context);
} twidget_engine_stream;

/* What only the OS answers, from any thread: the calling thread's CPU seconds, std::chrono::steady_clock's seconds (the host's clock too), the user's time zone (seconds east of UTC at that time) and `size` bytes of the system's CSPRNG. */
typedef struct twidget_engine_platform {
    void* context;
    double (*thread_cpu_seconds)(void* context);
    double (*steady_seconds)(void* context);
    int32_t (*time_zone_offset)(void* context, int64_t epoch_seconds);
    void (*random)(void* context, uint8_t* out, size_t size);
} twidget_engine_platform;

/* Locks the process down once the opening was accepted, before Ready: why it could not (`*size` bytes the glue keeps until twidget_engine_serve returns), or `*size` 0 when it did. */
typedef struct twidget_engine_lockdown {
    void* context;
    const uint8_t* (*lockdown)(void* context, size_t* size);
} twidget_engine_lockdown;

/* Serves one engine stream until it ends, on the calling thread: why it ended (`*reason_size` bytes to free with twidget_engine_free), or NULL and 0 at the end of the stream; `lockdown` may be NULL, and no callback runs after it returns. */
uint8_t* twidget_engine_serve(const twidget_engine_stream* stream, const twidget_engine_platform* platform, const uint8_t* build,
                              size_t build_size, const twidget_engine_lockdown* lockdown, size_t* reason_size);

void twidget_engine_free(uint8_t* bytes, size_t size);

#ifdef __cplusplus
}
#endif

#endif
