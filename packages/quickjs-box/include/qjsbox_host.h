// The host half of the QuickJS box: one widget engine (a QuickJS runtime and context) inside a
// wasm linear memory of its own, behind an opaque handle. The engine is the pinned fork compiled to
// wasm and translated back to C (packages/quickjs-box/build.sh), so a memory bug in it reads and
// writes that memory only, or stops the process: it cannot reach the host's memory or another box.
//
// The surface is scalar: no JSValue and no engine pointer crosses it. Results come back as bytes
// that stay valid until the next call into the same box.
//
// Rules for the caller:
//  - One thread at a time per box (the engine's own thread, as with the native engine).
//  - No call into a box from inside one of its callbacks. The callbacks receive everything they
//    need, and return through qjsbox_return_*. A call made anyway is refused (QJSBOX_REFUSED) and
//    trips an assertion in a debug build. qjsbox_set_deadlines, qjsbox_interrupted and
//    qjsbox_clear_interrupted are the exception: they never enter the box.
//  - "Contain and crash": a fault inside the box (an access outside its memory, a bad indirect
//    call, an engine abort, a malformed request to the host) ends the process, as an engine crash
//    does without the box. The reason is written to stderr and passed to the fault hook first.
#ifndef QJSBOX_HOST_H
#define QJSBOX_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qjsbox qjsbox;

// ---- host functions (the box calling out) ----------------------------------------------------

/* One argument of a host function: a string as it is, a number, or anything else (empty). */
enum { QJSBOX_ARG_OTHER = 0, QJSBOX_ARG_STRING = 1, QJSBOX_ARG_NUMBER = 2 };
typedef struct {
    int32_t kind;
    const char *str; /* QJSBOX_ARG_STRING: `len` bytes of UTF-8 inside the box, valid during the callback */
    size_t len;
    double num;      /* QJSBOX_ARG_NUMBER */
} qjsbox_arg;

/* A host function's return value; undefined unless one of these is called during the callback. */
typedef struct qjsbox_return qjsbox_return;
void qjsbox_return_null(qjsbox_return *ret);
void qjsbox_return_number(qjsbox_return *ret, double value);
void qjsbox_return_string(qjsbox_return *ret, const char *utf8, size_t len); /* copied */
void qjsbox_return_bytes(qjsbox_return *ret, const uint8_t *bytes, size_t len); /* copied; an ArrayBuffer in JS */

/* How a registered function passes its arguments (qjsbox_register). */
enum {
    QJSBOX_FN_STRINGS = 0, /* name(...): `call`, at most QJSBOX_MAX_ARGS arguments */
    QJSBOX_FN_WORDS = 1,   /* name(number, Float64Array, string): `words` */
    QJSBOX_FN_OPS = 2,     /* name(Int32Array, string): `ops` */
};
#define QJSBOX_MAX_ARGS 8

/* Everything a callback receives points into the box and is valid until the callback returns. */
typedef struct {
    void *opaque;
    void (*call)(void *opaque, int32_t magic, int32_t argc, const qjsbox_arg *args, qjsbox_return *ret);
    void (*words)(void *opaque, int32_t magic, double number, const uint8_t *words, size_t words_bytes, const char *text, size_t text_len);
    void (*ops)(void *opaque, int32_t magic, const uint8_t *words, size_t words_bytes, const char *strings, size_t strings_len);
} qjsbox_callbacks;

// ---- lifetime --------------------------------------------------------------------------------

typedef struct {
    /* What the engine's heap may grow to; the box's memory stops growing there and the engine
       throws "out of memory", as JS_SetMemoryLimit makes the native engine do. */
    size_t memory_limit_bytes;
    /* Native stack the caller's thread can give the engine (JSThread: 6 MB of its 8 MB). The box
       derives QuickJS's own stack limit from it (qjsbox_js_stack_bytes). */
    size_t native_stack_bytes;
} qjsbox_options;

/* A new engine, or NULL with *error set to a static string (this build cannot run a box, or the
   engine did not start). */
qjsbox *qjsbox_new(const qjsbox_options *options, const qjsbox_callbacks *callbacks, const char **error);
/* Releases the box's memory. No engine code runs, whatever state the engine is in. */
void qjsbox_free(qjsbox *box);

// ---- calling in ------------------------------------------------------------------------------

enum {
    QJSBOX_VALUE = 0,          /* the result is the value */
    QJSBOX_EXCEPTION = 1,      /* the result is the exception: "name: message\nstack" */
    QJSBOX_NOT_A_FUNCTION = 2, /* qjsbox_call: nothing to call */
    QJSBOX_REFUSED = 3,        /* called from inside one of the box's callbacks, or on a NULL box */
};
enum { QJSBOX_RESULT_UNDEFINED = 0, QJSBOX_RESULT_STRING = 1, QJSBOX_RESULT_NUMBER = 2 };
typedef struct {
    int32_t kind;
    const char *str; /* QJSBOX_RESULT_STRING: `len` bytes, valid until the next call into this box */
    size_t len;
    double num;      /* QJSBOX_RESULT_NUMBER */
} qjsbox_result;

/* A global function `name` the scripts can call; it reaches the callback of its kind with `magic`. */
bool qjsbox_register(qjsbox *box, const char *name, int kind, int32_t magic, int32_t length);

int qjsbox_eval(qjsbox *box, const char *code, size_t len, const char *filename, bool as_module, qjsbox_result *result);
/* One of the host's own precompiled bundles (.qbc, the TWQB container of qjs_shim.h), checked against its source. */
int qjsbox_eval_bytecode(qjsbox *box, const uint8_t *blob, size_t blob_len, const char *source, size_t source_len, qjsbox_result *result);
/* Compiles `code` into that container; the blob is the result's bytes. */
int qjsbox_compile(qjsbox *box, const char *code, size_t len, const char *filename, qjsbox_result *result);

/* A private reference to the global function `name`: a slot for qjsbox_call, or 0 when it is not a function. */
int32_t qjsbox_capture(qjsbox *box, const char *name);
void qjsbox_release(qjsbox *box, int32_t slot);
/* Calls the function in `slot`, or the global `name` when slot is 0, with string arguments (at most QJSBOX_MAX_ARGS). */
int qjsbox_call(qjsbox *box, int32_t slot, const char *name, int argc, const char *const *args, const size_t *lens, qjsbox_result *result);

bool qjsbox_job_pending(qjsbox *box);
/* One queued promise job: > 0 ran, 0 none, < 0 it threw (the result is the exception). */
int qjsbox_run_job(qjsbox *box, qjsbox_result *result);

// ---- limits ----------------------------------------------------------------------------------

/* The running script is interrupted once the clock passes `uptime_deadline` (seconds of
   qjsbox_uptime) or the calling thread's CPU time passes `cpu_deadline` (seconds of
   qjsbox_thread_cpu_time; INFINITY disables it). Checked on the host, in C. */
void qjsbox_set_deadlines(qjsbox *box, double uptime_deadline, double cpu_deadline);
bool qjsbox_interrupted(qjsbox *box);
void qjsbox_clear_interrupted(qjsbox *box);
/* The clocks the deadlines are compared with: ProcessInfo.systemUptime on Apple platforms, the
   monotonic clock elsewhere; and CLOCK_THREAD_CPUTIME_ID. */
double qjsbox_uptime(void);
double qjsbox_thread_cpu_time(void);

void qjsbox_set_memory_limit(qjsbox *box, size_t bytes);
size_t qjsbox_memory_used(qjsbox *box);
/* JS_ComputeMemoryUsage: malloc, used, atoms, strings, objects, properties, shapes, functions, bytecode, pc2line, binary. */
void qjsbox_memory_usage(qjsbox *box, int64_t out[11]);
void qjsbox_collect_garbage(qjsbox *box);
/* The box's linear memory right now, and the size it may reach. */
size_t qjsbox_linear_memory_bytes(qjsbox *box);
size_t qjsbox_linear_memory_limit_bytes(qjsbox *box);
/* QuickJS's own stack limit for a thread that gives the engine `native_stack_bytes`. */
size_t qjsbox_js_stack_bytes(size_t native_stack_bytes);

// ---- the build -------------------------------------------------------------------------------

/* "guard-pages" (macOS) or "bounds-checks" (everywhere else, or QJSBOX_BOUNDS_CHECKS=1). */
const char *qjsbox_build_mode(void);
/* False when the box was compiled without optimization: qjsbox_new then refuses (11-48x slower than native). */
bool qjsbox_build_optimized(void);
/* The first 7 hex digits of the QuickJS fork commit the module was built from. */
const char *qjsbox_quickjs_commit(void);

/* Called with the reason right before a fault ends the process (to log it or annotate a crash report). */
void qjsbox_set_fault_hook(void (*hook)(const char *reason));

#ifdef __cplusplus
}
#endif
#endif
