/* The engine process's end in Rust (crates/engine_ffi): the C++ OS glue keeps the descriptors, the lockdown and the clocks, and serves an engine stream through these callbacks as SandboxRemoteEngineEnd::serve does, or a hosting stream with the renderers it builds as serveHosting does; the same library carries Nova's C interface (nova_engine.h). */
#ifndef TWIDGET_ENGINE_H
#define TWIDGET_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The stream to the host's process: `read` blocks until at least one byte came and returns how many it copied (0 at the end, on an error and after `shutdown`), `write` blocks until all bytes are written (false once broken or shut down), `shutdown` wakes both from any thread without waiting; all three may be called from any thread, a read and a write at once, until serving returns. */
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

/* Locks the process down once the opening was accepted, before Ready: why it could not (`*size` bytes the glue keeps until serving returns), or `*size` 0 when it did. */
typedef struct twidget_engine_lockdown {
    void* context;
    const uint8_t* (*lockdown)(void* context, size_t* size);
} twidget_engine_lockdown;

/* Serves one engine stream until it ends, on the calling thread: why it ended (`*reason_size` bytes to free with twidget_engine_free), or NULL and 0 at the end of the stream; `lockdown` may be NULL, and no callback runs after it returns. */
uint8_t* twidget_engine_serve(const twidget_engine_stream* stream, const twidget_engine_platform* platform, const uint8_t* build,
                              size_t build_size, const twidget_engine_lockdown* lockdown, size_t* reason_size);

/* Frees what twidget_engine_serve, twidget_engine_serve_hosting or twidget_hosted_platform_request returned; NULL does nothing. */
void twidget_engine_free(uint8_t* bytes, size_t size);

/* `size` bytes at `bytes` (NULL with 0 for none). */
typedef struct twidget_engine_bytes {
    const uint8_t* bytes;
    size_t size;
} twidget_engine_bytes;

/* A hosted widget's platform (SandboxHostedPlatform), opaque: the glue holds it from `make` until it releases it once with twidget_hosted_platform_release, from any thread, also past its widget and past serving (it then sends nothing and answers none). */
typedef struct twidget_hosted_platform twidget_hosted_platform;

/* A hosted renderer's way back to its widget (renderer.h's onMessage and onDiag), opaque: valid until the renderer's `free` returned, or until `make` returns when it built no renderer; what the renderer reports before the served host connects it goes nowhere. */
typedef struct twidget_hosted_link twidget_hosted_link;

/* The hosting opening's resources a factory gets: the sandbox bundle, the whole validator bundle and the renderer bundle's stamp line, valid during `make`. */
typedef struct twidget_hosted_resources {
    twidget_engine_bytes sandbox_bundle;
    twidget_engine_bytes validator_bundle;
    twidget_engine_bytes renderer_bundle;
} twidget_hosted_resources;

/* One widget's renderer (renderer.h): `load`, `send`, `set_theme`, `set_width` and `destroy` as the served host calls them, one at a time on its glue thread, `platform_message` the host's platform message for it on that thread (SandboxHostedPlatform::onMessage), `free` once after every other call returned, on any thread; any may be NULL. */
typedef struct twidget_hosted_renderer {
    void* context;
    void (*load)(void* context);
    void (*send)(void* context, const uint8_t* json, size_t size);
    void (*set_theme)(void* context, const uint8_t* theme, size_t size);
    void (*set_width)(void* context, double width);
    void (*destroy)(void* context);
    void (*platform_message)(void* context, const uint8_t* message, size_t size);
    void (*free)(void* context);
} twidget_hosted_renderer;

/* What `make` returns: it built nothing, it filled `*renderer`, or it threw with `*error` the UTF-8 text it threw (which the glue keeps until its next `make` or the end of serving). */
#define TWIDGET_HOSTED_BUILT_NOTHING 0
#define TWIDGET_HOSTED_RENDERER 1
#define TWIDGET_HOSTED_THREW 2

/* The engine process's renderer factory (HostedRendererFactory): `make` runs on the serving thread at a widget's hosted start with the opening's resources, the colour scheme, the bubble's width in CSS px, the widget's platform (the glue's to release whatever `make` returns) and its link, and returns one of the three above. */
typedef struct twidget_hosted_renderers {
    void* context;
    int32_t (*make)(void* context, const twidget_hosted_resources* resources, const uint8_t* theme, size_t theme_size, double width, twidget_hosted_platform* platform,
                    twidget_hosted_link* link, twidget_hosted_renderer* renderer, twidget_engine_bytes* error);
} twidget_hosted_renderers;

/* twidget_engine_serve for a stream whose opening may be a hosting one (serveHosting): a hosting opening's widgets run here with their engines, validators and the renderers `renderers` builds (NULL: every widget dies, its factory having built nothing), an engine opening's as twidget_engine_serve serves them; every renderer is freed before it returns. */
uint8_t* twidget_engine_serve_hosting(const twidget_engine_stream* stream, const twidget_engine_platform* platform, const uint8_t* build, size_t build_size,
                                      const twidget_engine_lockdown* lockdown, const twidget_hosted_renderers* renderers, size_t* reason_size);

/* A message of the renderer for the host's platform (SandboxHostedPlatform::send), from any thread. */
void twidget_hosted_platform_send(const twidget_hosted_platform* platform, const uint8_t* bytes, size_t size);

/* A request the renderer waits for (SandboxHostedPlatform::request), from any thread but never inside `make`: true with the host's answer (`*answer_size` bytes to free with twidget_engine_free, NULL for an empty one), false for none. */
bool twidget_hosted_platform_request(const twidget_hosted_platform* platform, const uint8_t* bytes, size_t size, uint8_t** answer, size_t* answer_size);

/* The glue lets go of a widget's platform, once per handle `make` gave it; NULL does nothing. */
void twidget_hosted_platform_release(twidget_hosted_platform* platform);

/* Renderer to host message (JSON, renderer.h's onMessage), from any thread, inside `send` too. */
void twidget_hosted_renderer_message(const twidget_hosted_link* link, const uint8_t* bytes, size_t size);

/* The renderer's diagnostic (renderer.h's onDiag), from any thread. */
void twidget_hosted_renderer_diag(const twidget_hosted_link* link, const uint8_t* kind, size_t kind_size, const uint8_t* detail, size_t detail_size);

#ifdef __cplusplus
}
#endif

#endif
