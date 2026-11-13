// The host half of the QuickJS box (see qjsbox_host.h). Everything the box hands over is
// untrusted: an offset and a length are checked against the memory's current size before any byte
// is read or written, and a request that does not check out ends the process like a trap does.
#include "qjsbox_config.h"

#include "qjsbox_host.h"
#include "qjsbox_testing.h"

#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "qjsbox.h"       /* generated/: the module's C interface */
#include "qjsbox_build.h" /* generated/: what the module was built from */

// The import modules of the generated code. Each carries the box, so an import finds its state.
struct w2c_env { qjsbox *box; };
struct w2c_host { qjsbox *box; };
struct w2c_wasi__snapshot__preview1 { qjsbox *box; };

struct qjsbox_return {
    int32_t kind;   /* BOX_RET_* of the guest */
    double num;
    uint8_t *bytes; /* owned; reused from call to call */
    size_t len, cap;
};

struct qjsbox {
    w2c_qjsbox inst;
    struct w2c_env env;
    struct w2c_host host;
    struct w2c_wasi__snapshot__preview1 wasi;
    qjsbox_callbacks cb;
    bool entered;   /* a call into the box is running */
    bool in_import; /* ... and it is inside one of the host's callbacks */
    double uptime_deadline, cpu_deadline;
    bool interrupted;
    struct qjsbox_return ret;
    uint64_t initial_pages;
    char last_stderr[200]; /* the tail of what the engine wrote to stderr: an assertion's text, for the fault report */
    size_t last_stderr_len;
};

// The guest's constants (guest/qjsbox_guest.c).
enum { BOX_RET_UNDEFINED = 0, BOX_RET_NULL = 1, BOX_RET_STRING = 2, BOX_RET_NUMBER = 3, BOX_RET_BYTES = 4 };
enum { BOX_ARG_BYTES = 24, BOX_RET_BYTES_SIZE = 16 };

#if QJSBOX_NUM_OUTPUTS != 8
#error "generated/ has another number of translation units: update src/qjsbox_unit_*.c and Package.swift"
#endif

#define WASM_PAGE 65536u
// Native stack per byte of the stack QuickJS watches. Measured 3.5 (guard pages) to 5.2 (bounds
// checks) on plain calls; 8 leaves room for paths that were not measured.
#define QJSBOX_NATIVE_STACK_RATIO 8

// ---- faults ----------------------------------------------------------------------------------

static void (*g_fault_hook)(const char *reason);
static _Thread_local qjsbox *tl_box; /* the box this thread is inside, for the fault report only */

void qjsbox_set_fault_hook(void (*hook)(const char *reason)) { g_fault_hook = hook; }

// The end of the process. Not inlined, so a crash report shows the frame.
__attribute__((noinline, noreturn, cold)) static void qjsbox_fault(const char *reason) {
    char line[512];
    qjsbox *b = tl_box;
    int n = snprintf(line, sizeof line, "widget-sandbox: QuickJS box fault: %s (QuickJS %s, %s)%s%.*s\n", reason, QJSBOX_QUICKJS_COMMIT,
                     QJSBOX_BOUNDS_CHECKS ? "bounds-checks" : "guard-pages", b && b->last_stderr_len ? "; engine said: " : "",
                     b ? (int)b->last_stderr_len : 0, b ? b->last_stderr : "");
    if (n > 0) (void)!write(STDERR_FILENO, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    if (g_fault_hook) g_fault_hook(reason);
    abort();
}

// One function per trap, so the reason is in the backtrace too.
__attribute__((noinline, noreturn, cold)) static void qjsbox_trap_out_of_bounds(void) { qjsbox_fault("trap: out-of-bounds access in the box's memory or table"); }
__attribute__((noinline, noreturn, cold)) static void qjsbox_trap_call_indirect(void) { qjsbox_fault("trap: invalid indirect call"); }
__attribute__((noinline, noreturn, cold)) static void qjsbox_trap_unreachable(void) { qjsbox_fault("trap: unreachable code (the engine aborted)"); }
__attribute__((noinline, noreturn, cold)) static void qjsbox_trap_arithmetic(void) { qjsbox_fault("trap: integer overflow, division by zero or invalid conversion"); }
__attribute__((noinline, noreturn, cold)) static void qjsbox_trap_exhaustion(void) { qjsbox_fault("trap: call stack exhausted"); }
__attribute__((noinline, noreturn, cold)) static void qjsbox_trap_other(void) { qjsbox_fault("trap"); }
__attribute__((noinline, noreturn, cold)) static void qjsbox_bad_request(void) { qjsbox_fault("the engine passed the host a pointer or a length outside its memory"); }

// WASM_RT_TRAP_HANDLER (qjsbox_config.h): what the generated code calls instead of unwinding.
__attribute__((noreturn)) void qjsbox_wasm_trap(wasm_rt_trap_t code);
void qjsbox_wasm_trap(wasm_rt_trap_t code) {
    switch (code) {
    case WASM_RT_TRAP_OOB: qjsbox_trap_out_of_bounds();
    case WASM_RT_TRAP_CALL_INDIRECT: qjsbox_trap_call_indirect();
    case WASM_RT_TRAP_UNREACHABLE: qjsbox_trap_unreachable();
    case WASM_RT_TRAP_INT_OVERFLOW:
    case WASM_RT_TRAP_DIV_BY_ZERO:
    case WASM_RT_TRAP_INVALID_CONVERSION: qjsbox_trap_arithmetic();
#if !WASM_RT_MERGED_OOB_AND_EXHAUSTION_TRAPS
    case WASM_RT_TRAP_EXHAUSTION: qjsbox_trap_exhaustion();
#endif
    default: qjsbox_trap_other();
    }
}

// ---- the box's memory ------------------------------------------------------------------------

static inline wasm_rt_memory_t *memory(qjsbox *b) { return &b->inst.w2c_memory; }

// `len` bytes at `ptr` inside the box, or the end of the process.
static inline uint8_t *span(qjsbox *b, uint32_t ptr, uint64_t len) {
    wasm_rt_memory_t *m = memory(b);
    if ((uint64_t)ptr + len > m->size) qjsbox_bad_request();
    return m->data + ptr;
}
static inline uint32_t get_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

// ---- entering the box ------------------------------------------------------------------------

// Every call into the engine is bracketed by these. There is never more than one on the stack:
// a call from inside a callback would put host frames between two runs of engine code.
static bool box_enter(qjsbox *b) {
    if (!b) return false;
    assert(!b->in_import && "qjsbox: called from inside one of the box's callbacks");
    assert(!b->entered && "qjsbox: already inside this box");
    if (b->in_import || b->entered) return false;
    b->entered = true;
    tl_box = b;
    return true;
}
static void box_leave(qjsbox *b) {
    b->entered = false;
    tl_box = NULL;
}

static void result_none(qjsbox_result *r) {
    if (!r) return;
    r->kind = QJSBOX_RESULT_UNDEFINED;
    r->str = NULL;
    r->len = 0;
    r->num = 0;
}
static void read_result(qjsbox *b, qjsbox_result *r) {
    if (!r) return;
    r->kind = (int32_t)w2c_qjsbox_box_result_kind(&b->inst);
    if (r->kind == QJSBOX_RESULT_STRING) {
        uint32_t len = w2c_qjsbox_box_result_len(&b->inst);
        r->str = (const char *)span(b, w2c_qjsbox_box_result_ptr(&b->inst), len);
        r->len = len;
    } else if (r->kind == QJSBOX_RESULT_NUMBER) {
        r->num = w2c_qjsbox_box_result_number(&b->inst);
    } else {
        r->kind = QJSBOX_RESULT_UNDEFINED;
    }
}
// The host could not place its own data in the box: the memory is at its limit.
static int out_of_memory(qjsbox_result *r) {
    static const char text[] = "InternalError: out of memory";
    if (r) { r->kind = QJSBOX_RESULT_STRING; r->str = text; r->len = sizeof text - 1; r->num = 0; }
    return QJSBOX_EXCEPTION;
}

// Copies `len` bytes into a new block inside the box, with a NUL after them. 0 when it has no room.
static uint32_t put(qjsbox *b, const void *bytes, size_t len) {
    if (len >= QJSBOX_MAXIMUM_MEMORY_BYTES) return 0;
    uint32_t p = w2c_qjsbox_box_alloc(&b->inst, (uint32_t)len + 1);
    if (!p) return 0;
    uint8_t *d = span(b, p, (uint64_t)len + 1);
    if (len) memcpy(d, bytes, len);
    d[len] = 0;
    return p;
}
static void drop(qjsbox *b, uint32_t p) { if (p) w2c_qjsbox_box_free(&b->inst, p); }

// ---- lifetime --------------------------------------------------------------------------------

static pthread_once_t g_runtime_once = PTHREAD_ONCE_INIT;
static void runtime_init(void) { wasm_rt_init(); } /* installs nothing in this configuration */

size_t qjsbox_js_stack_bytes(size_t native_stack_bytes) {
    size_t limit = native_stack_bytes / QJSBOX_NATIVE_STACK_RATIO;
    size_t most = QJSBOX_GUEST_STACK_BYTES / 4 * 3; /* the rest is for the engine's C frames between two checks */
    if (limit > most) limit = most;
    if (limit < 32u << 10) limit = 32u << 10;
    return limit;
}

static void set_memory_ceiling(qjsbox *b, size_t bytes) {
    uint64_t module_max = QJSBOX_MAXIMUM_MEMORY_BYTES / WASM_PAGE;
    uint64_t pages = b->initial_pages + ((uint64_t)bytes + WASM_PAGE - 1) / WASM_PAGE;
    memory(b)->max_pages = pages < module_max ? pages : module_max;
}

qjsbox *qjsbox_new(const qjsbox_options *options, const qjsbox_callbacks *callbacks, const char **error) {
    const char *ignored;
    if (!error) error = &ignored;
    *error = NULL;
#if !QJSBOX_OPTIMIZED && !defined(QJSBOX_ALLOW_UNOPTIMIZED)
    *error = "the QuickJS box was compiled without optimization (compile CQuickJSBox with -O2 in every configuration)";
    return NULL;
#endif
    pthread_once(&g_runtime_once, runtime_init);
    qjsbox *b = calloc(1, sizeof *b);
    if (!b) { *error = "out of memory"; return NULL; }
    b->env.box = b;
    b->host.box = b;
    b->wasi.box = b;
    if (callbacks) b->cb = *callbacks;
    b->uptime_deadline = INFINITY;
    b->cpu_deadline = INFINITY;
    b->entered = true;
    tl_box = b;
    wasm2c_qjsbox_instantiate(&b->inst, &b->env, &b->host, &b->wasi);
    b->initial_pages = memory(b)->pages;
    w2c_qjsbox_0x5Finitialize(&b->inst);
    size_t native_stack = options && options->native_stack_bytes ? options->native_stack_bytes : 384u << 10;
    uint32_t started = w2c_qjsbox_box_init(&b->inst, (uint32_t)qjsbox_js_stack_bytes(native_stack));
    box_leave(b);
    if (started != 0) {
        qjsbox_free(b);
        *error = "the engine did not start inside the box";
        return NULL;
    }
    qjsbox_set_memory_limit(b, options && options->memory_limit_bytes ? options->memory_limit_bytes : 8u << 20);
    return b;
}

void qjsbox_free(qjsbox *b) {
    if (!b) return;
    assert(!b->entered && !b->in_import && "qjsbox_free from inside the box");
    wasm2c_qjsbox_free(&b->inst);
    free(b->ret.bytes);
    free(b);
}

// ---- calling in ------------------------------------------------------------------------------

bool qjsbox_register(qjsbox *b, const char *name, int kind, int32_t magic, int32_t length) {
    if (!box_enter(b)) return false;
    uint32_t n = put(b, name, strlen(name));
    bool ok = n && w2c_qjsbox_box_register(&b->inst, n, (uint32_t)kind, (uint32_t)magic, (uint32_t)length) == 0;
    drop(b, n);
    box_leave(b);
    return ok;
}

int qjsbox_eval(qjsbox *b, const char *code, size_t len, const char *filename, bool as_module, qjsbox_result *result) {
    result_none(result);
    if (!box_enter(b)) return QJSBOX_REFUSED;
    uint32_t c = put(b, code, len), f = put(b, filename, strlen(filename));
    int status;
    if (c && f) {
        status = (int)w2c_qjsbox_box_eval(&b->inst, c, (uint32_t)len, f, as_module ? 1 : 0);
        read_result(b, result);
    } else {
        status = out_of_memory(result);
    }
    drop(b, c);
    drop(b, f);
    box_leave(b);
    return status;
}

int qjsbox_eval_bytecode(qjsbox *b, const uint8_t *blob, size_t blob_len, const char *source, size_t source_len, qjsbox_result *result) {
    result_none(result);
    if (!box_enter(b)) return QJSBOX_REFUSED;
    uint32_t g = put(b, blob, blob_len), s = put(b, source, source_len);
    int status;
    if (g && s) {
        status = (int)w2c_qjsbox_box_eval_bytecode(&b->inst, g, (uint32_t)blob_len, s, (uint32_t)source_len);
        read_result(b, result);
    } else {
        status = out_of_memory(result);
    }
    drop(b, g);
    drop(b, s);
    box_leave(b);
    return status;
}

int qjsbox_compile(qjsbox *b, const char *code, size_t len, const char *filename, qjsbox_result *result) {
    result_none(result);
    if (!box_enter(b)) return QJSBOX_REFUSED;
    uint32_t c = put(b, code, len), f = put(b, filename, strlen(filename));
    int status;
    if (c && f) {
        status = (int)w2c_qjsbox_box_compile(&b->inst, c, (uint32_t)len, f);
        read_result(b, result);
    } else {
        status = out_of_memory(result);
    }
    drop(b, c);
    drop(b, f);
    box_leave(b);
    return status;
}

int32_t qjsbox_capture(qjsbox *b, const char *name) {
    if (!box_enter(b)) return 0;
    uint32_t n = put(b, name, strlen(name));
    int32_t slot = n ? (int32_t)w2c_qjsbox_box_capture(&b->inst, n) : 0;
    drop(b, n);
    box_leave(b);
    return slot;
}

void qjsbox_release(qjsbox *b, int32_t slot) {
    if (!box_enter(b)) return;
    w2c_qjsbox_box_release(&b->inst, (uint32_t)slot);
    box_leave(b);
}

int qjsbox_call(qjsbox *b, int32_t slot, const char *name, int argc, const char *const *args, const size_t *lens, qjsbox_result *result) {
    result_none(result);
    if (argc < 0 || argc > QJSBOX_MAX_ARGS) return QJSBOX_NOT_A_FUNCTION;
    if (!box_enter(b)) return QJSBOX_REFUSED;
    // One block: the pointers, the lengths, then the strings, each with a NUL after it.
    uint64_t total = (uint64_t)argc * 8;
    for (int i = 0; i < argc; i++) total += (uint64_t)lens[i] + 1;
    uint32_t block = total < QJSBOX_MAXIMUM_MEMORY_BYTES ? w2c_qjsbox_box_alloc(&b->inst, (uint32_t)total) : 0;
    uint32_t n = put(b, name ? name : "", name ? strlen(name) : 0);
    int status;
    if (block && n) {
        uint8_t *d = span(b, block, total);
        uint32_t at = (uint32_t)argc * 8;
        for (int i = 0; i < argc; i++) {
            put_u32(d + i * 4, block + at);
            put_u32(d + argc * 4 + i * 4, (uint32_t)lens[i]);
            if (lens[i]) memcpy(d + at, args[i], lens[i]);
            d[at + lens[i]] = 0;
            at += (uint32_t)lens[i] + 1;
        }
        status = (int)w2c_qjsbox_box_call(&b->inst, (uint32_t)slot, n, (uint32_t)argc, block, block + (uint32_t)argc * 4);
        read_result(b, result);
    } else {
        status = out_of_memory(result);
    }
    drop(b, block);
    drop(b, n);
    box_leave(b);
    return status;
}

bool qjsbox_job_pending(qjsbox *b) {
    if (!box_enter(b)) return false;
    bool pending = w2c_qjsbox_box_job_pending(&b->inst) != 0;
    box_leave(b);
    return pending;
}

int qjsbox_run_job(qjsbox *b, qjsbox_result *result) {
    result_none(result);
    if (!box_enter(b)) return 0;
    int r = (int)(int32_t)w2c_qjsbox_box_run_job(&b->inst);
    if (r < 0) read_result(b, result);
    box_leave(b);
    return r;
}

// ---- limits ----------------------------------------------------------------------------------

double qjsbox_uptime(void) {
#if defined(__APPLE__)
    return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; /* ProcessInfo.systemUptime */
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
#endif
}

double qjsbox_thread_cpu_time(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

void qjsbox_set_deadlines(qjsbox *b, double uptime_deadline, double cpu_deadline) {
    if (!b) return;
    b->uptime_deadline = uptime_deadline;
    b->cpu_deadline = cpu_deadline;
}
bool qjsbox_interrupted(qjsbox *b) { return b && b->interrupted; }
void qjsbox_clear_interrupted(qjsbox *b) { if (b) b->interrupted = false; }

void qjsbox_set_memory_limit(qjsbox *b, size_t bytes) {
    if (!box_enter(b)) return;
    set_memory_ceiling(b, bytes);
    w2c_qjsbox_box_set_memory_limit(&b->inst, bytes < UINT32_MAX ? (uint32_t)bytes : UINT32_MAX);
    box_leave(b);
}

size_t qjsbox_memory_used(qjsbox *b) {
    if (!box_enter(b)) return 0;
    size_t used = w2c_qjsbox_box_memory_used(&b->inst);
    box_leave(b);
    return used;
}

void qjsbox_memory_usage(qjsbox *b, int64_t out[11]) {
    memset(out, 0, 11 * sizeof(int64_t));
    if (!box_enter(b)) return;
    uint32_t p = w2c_qjsbox_box_alloc(&b->inst, 11 * 8);
    if (p) {
        w2c_qjsbox_box_memory_usage(&b->inst, p);
        memcpy(out, span(b, p, 11 * 8), 11 * 8);
        drop(b, p);
    }
    box_leave(b);
}

void qjsbox_collect_garbage(qjsbox *b) {
    if (!box_enter(b)) return;
    w2c_qjsbox_box_gc(&b->inst);
    box_leave(b);
}

size_t qjsbox_linear_memory_bytes(qjsbox *b) { return b ? (size_t)memory(b)->size : 0; }
size_t qjsbox_linear_memory_limit_bytes(qjsbox *b) { return b ? (size_t)(memory(b)->max_pages * WASM_PAGE) : 0; }

// For the tests (qjsbox_testing.h): the box's memory itself, to play an engine that corrupted it.
uint8_t *qjsbox_testing_memory(qjsbox *b, size_t *size) {
    *size = (size_t)memory(b)->size;
    return memory(b)->data;
}

const char *qjsbox_build_mode(void) { return QJSBOX_BOUNDS_CHECKS ? "bounds-checks" : "guard-pages"; }
bool qjsbox_build_optimized(void) { return QJSBOX_OPTIMIZED != 0; }
const char *qjsbox_quickjs_commit(void) { return QJSBOX_QUICKJS_COMMIT; }

// ---- return values of host functions ---------------------------------------------------------

static void return_copy(qjsbox_return *r, int32_t kind, const void *bytes, size_t len) {
    if (len >= QJSBOX_MAXIMUM_MEMORY_BYTES) { r->kind = BOX_RET_UNDEFINED; return; } /* could never fit in the box */
    if (len > r->cap) {
        uint8_t *grown = realloc(r->bytes, len);
        if (!grown) { r->kind = BOX_RET_UNDEFINED; return; }
        r->bytes = grown;
        r->cap = len;
    }
    if (len) memcpy(r->bytes, bytes, len);
    r->len = len;
    r->kind = kind;
}
void qjsbox_return_null(qjsbox_return *r) { r->kind = BOX_RET_NULL; }
void qjsbox_return_number(qjsbox_return *r, double value) { r->kind = BOX_RET_NUMBER; r->num = value; }
void qjsbox_return_string(qjsbox_return *r, const char *utf8, size_t len) { return_copy(r, BOX_RET_STRING, utf8, len); }
void qjsbox_return_bytes(qjsbox_return *r, const uint8_t *bytes, size_t len) { return_copy(r, BOX_RET_BYTES, bytes, len); }

// ---- imports: the engine calling out ---------------------------------------------------------

// The script's deadlines. Plain numbers and two clocks: no callback, nothing of the host's runs.
u32 w2c_host_interrupt(struct w2c_host *h) {
    qjsbox *b = h->box;
    if (qjsbox_uptime() > b->uptime_deadline || (b->cpu_deadline != INFINITY && qjsbox_thread_cpu_time() > b->cpu_deadline)) {
        b->interrupted = true;
        return 1;
    }
    return 0;
}

u32 w2c_host_call(struct w2c_host *h, u32 magic, u32 argc, u32 args_ptr, u32 ret_ptr) {
    qjsbox *b = h->box;
    if (argc > QJSBOX_MAX_ARGS) qjsbox_bad_request();
    const uint8_t *raw = span(b, args_ptr, (uint64_t)argc * BOX_ARG_BYTES);
    qjsbox_arg args[QJSBOX_MAX_ARGS];
    for (u32 i = 0; i < argc; i++) {
        const uint8_t *a = raw + i * BOX_ARG_BYTES;
        args[i].kind = QJSBOX_ARG_OTHER;
        args[i].str = NULL;
        args[i].len = 0;
        args[i].num = 0;
        u32 kind = get_u32(a);
        if (kind == QJSBOX_ARG_STRING) {
            u32 len = get_u32(a + 4);
            args[i].kind = QJSBOX_ARG_STRING;
            args[i].str = (const char *)span(b, get_u32(a + 8), len);
            args[i].len = len;
        } else if (kind == QJSBOX_ARG_NUMBER) {
            args[i].kind = QJSBOX_ARG_NUMBER;
            memcpy(&args[i].num, a + 16, 8);
        }
    }
    b->ret.kind = BOX_RET_UNDEFINED;
    b->ret.num = 0;
    b->ret.len = 0;
    if (b->cb.call) {
        b->in_import = true;
        b->cb.call(b->cb.opaque, (int32_t)magic, (int32_t)argc, args, &b->ret);
        b->in_import = false;
    }
    uint8_t *ret = span(b, ret_ptr, BOX_RET_BYTES_SIZE);
    memcpy(ret, &b->ret.num, 8);
    put_u32(ret + 8, b->ret.kind == BOX_RET_STRING || b->ret.kind == BOX_RET_BYTES ? (u32)b->ret.len : 0);
    put_u32(ret + 12, 0);
    return (u32)b->ret.kind;
}

// The string or the bytes the last host function returned, into the buffer the engine allocated for them.
void w2c_host_take(struct w2c_host *h, u32 dst, u32 len) {
    qjsbox *b = h->box;
    if ((b->ret.kind != BOX_RET_STRING && b->ret.kind != BOX_RET_BYTES) || len != b->ret.len) qjsbox_bad_request();
    uint8_t *d = span(b, dst, len);
    if (len) memcpy(d, b->ret.bytes, len);
    b->ret.kind = BOX_RET_UNDEFINED;
}

void w2c_host_words(struct w2c_host *h, u32 magic, f64 number, u32 words, u32 words_bytes, u32 text, u32 text_len) {
    qjsbox *b = h->box;
    const uint8_t *w = span(b, words, words_bytes);
    const char *t = (const char *)span(b, text, text_len);
    if (!b->cb.words) return;
    b->in_import = true;
    b->cb.words(b->cb.opaque, (int32_t)magic, number, w, words_bytes, t, text_len);
    b->in_import = false;
}

void w2c_host_ops(struct w2c_host *h, u32 magic, u32 words, u32 words_bytes, u32 strings, u32 strings_len) {
    qjsbox *b = h->box;
    const uint8_t *w = span(b, words, words_bytes);
    const char *s = (const char *)span(b, strings, strings_len);
    if (!b->cb.ops) return;
    b->in_import = true;
    b->cb.ops(b->cb.opaque, (int32_t)magic, w, words_bytes, s, strings_len);
    b->in_import = false;
}

// Seconds east of UTC at that moment, in the host's time zone: what the native engine reads from localtime_r.
u32 w2c_host_tz_offset(struct w2c_host *h, u64 unix_seconds) {
    (void)h;
    time_t t = (time_t)(int64_t)unix_seconds;
    struct tm tm;
    if (!localtime_r(&t, &tm)) return 0;
    return (u32)(int32_t)tm.tm_gmtoff;
}

// ---- imports: what is left of the C library's needs ------------------------------------------

void w2c_env_emscripten_notify_memory_growth(struct w2c_env *e, u32 memory_index) {
    (void)e;
    (void)memory_index;
}

enum { WASI_ESUCCESS = 0, WASI_EBADF = 8, WASI_EINVAL = 28 };

// Date.now() and the engine's monotonic clock.
u32 w2c_wasi__snapshot__preview1_clock_time_get(struct w2c_wasi__snapshot__preview1 *w, u32 clock_id, u64 precision, u32 out) {
    (void)precision;
    qjsbox *b = w->box;
    uint8_t *p = span(b, out, 8);
    struct timespec t;
    if (clock_id == 0) clock_gettime(CLOCK_REALTIME, &t);
    else if (clock_id == 1) clock_gettime(CLOCK_MONOTONIC, &t);
    else return WASI_EINVAL;
    uint64_t ns = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
    memcpy(p, &ns, 8);
    return WASI_ESUCCESS;
}

// The engine's stdout and stderr go nowhere. The tail of stderr is kept for the fault report: when
// the engine aborts on an assertion, its text is the last thing written.
u32 w2c_wasi__snapshot__preview1_fd_write(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 iovs, u32 iovs_len, u32 nwritten) {
    qjsbox *b = w->box;
    if (iovs_len > 64) qjsbox_bad_request();
    const uint8_t *v = span(b, iovs, (uint64_t)iovs_len * 8);
    uint8_t *out = span(b, nwritten, 4);
    u32 total = 0;
    for (u32 i = 0; i < iovs_len; i++) {
        u32 len = get_u32(v + i * 8 + 4);
        const uint8_t *bytes = span(b, get_u32(v + i * 8), len);
        total += len;
        if (fd != 2) continue;
        for (u32 k = len > sizeof b->last_stderr ? len - (u32)sizeof b->last_stderr : 0; k < len; k++) {
            if (b->last_stderr_len == sizeof b->last_stderr) {
                memmove(b->last_stderr, b->last_stderr + 1, sizeof b->last_stderr - 1);
                b->last_stderr_len--;
            }
            uint8_t c = bytes[k];
            b->last_stderr[b->last_stderr_len++] = c >= 32 && c < 127 ? (char)c : ' ';
        }
    }
    put_u32(out, total);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_close(struct w2c_wasi__snapshot__preview1 *w, u32 fd) {
    (void)w;
    (void)fd;
    return WASI_EBADF;
}

u32 w2c_wasi__snapshot__preview1_fd_seek(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u64 offset, u32 whence, u32 out) {
    (void)w;
    (void)fd;
    (void)offset;
    (void)whence;
    (void)out;
    return WASI_EBADF;
}
