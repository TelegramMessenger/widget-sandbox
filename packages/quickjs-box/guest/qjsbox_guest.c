// The guest half of the QuickJS box: the code that is compiled to wasm together with the engine
// (the fork's quickjs.c, dtoa.c, libregexp.c, libunicode.c and the Swift package's own qjs_shim.c)
// and then through wasm2c back to C. One instance of the module is one widget engine: one runtime,
// one context, in a linear memory of its own.
//
// Nothing but integers and doubles crosses the boundary. A JSValue never leaves the box: the host
// names a captured function by a slot number and gets results back as bytes.
//
// The rule that keeps the host side simple (docs/decisions.md, "QuickJS in a wasm2c box"): the host
// never calls into the box while one of its imports is running. So everything the native glue does
// from inside an engine callback (QuickJS.swift's hostTrampoline, opsTrampoline, wordsTrampoline:
// reading arguments, building the return value) happens here, and each host function is one import
// that receives (pointer, length) pairs and answers through a second import that copies the result
// into a buffer this side allocated.
//
// build.sh compiles this file and checks the module's import and export lists against the ones
// below, so a change of this surface is a deliberate one.
#include "qjs_shim.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IMPORT(name) __attribute__((import_module("host"), import_name(#name)))
#define EXPORT(name) __attribute__((export_name(#name)))

// ---- imports ---------------------------------------------------------------------------------

/* One argument of a host function, as hostTrampoline sees it. 24 bytes, read by the host field by field. */
typedef struct {
    uint32_t kind;    /* BOX_ARG_* */
    uint32_t len;     /* BOX_ARG_STRING: bytes at ptr, without the NUL that follows them */
    const char *ptr;
    uint32_t reserved;
    double num;       /* BOX_ARG_NUMBER */
} box_arg;
enum { BOX_ARG_OTHER = 0, BOX_ARG_STRING = 1, BOX_ARG_NUMBER = 2 };

/* What a host function returned (QuickJS.swift's HostReturn). 16 bytes, written by the host. */
typedef struct {
    double num;       /* BOX_RET_NUMBER */
    uint32_t len;     /* BOX_RET_STRING, BOX_RET_BYTES: bytes to fetch with host_take */
    uint32_t reserved;
} box_ret;
enum { BOX_RET_UNDEFINED = 0, BOX_RET_NULL = 1, BOX_RET_STRING = 2, BOX_RET_NUMBER = 3, BOX_RET_BYTES = 4 };

/* Nonzero stops the running script (the deadlines live on the host). */
IMPORT(interrupt) int32_t host_interrupt(void);
/* A registered function of kind BOX_FN_STRINGS was called. Returns a BOX_RET_* and fills *ret. */
IMPORT(call) int32_t host_call(int32_t magic, int32_t argc, const box_arg *args, box_ret *ret);
/* Copies the string or the bytes host_call announced into [dst, dst + len). */
IMPORT(take) void host_take(void *dst, uint32_t len);
/* BOX_FN_WORDS: name(number, Float64Array, text). The words and the text are valid during the call. */
IMPORT(words) void host_words(int32_t magic, double number, const uint8_t *words, uint32_t words_bytes, const char *text, uint32_t text_len);
/* BOX_FN_OPS: name(Int32Array, string). */
IMPORT(ops) void host_ops(int32_t magic, const uint8_t *words, uint32_t words_bytes, const char *strings, uint32_t strings_len);
/* Seconds east of UTC at `unix_seconds` in the host's time zone (a standalone wasm module has none). */
IMPORT(tz_offset) int32_t host_tz_offset(int64_t unix_seconds);

// ---- state -----------------------------------------------------------------------------------

static JSRuntime *g_rt;
static JSContext *g_ctx;

/* The result of the last entry: a value's string form, a number, an exception's description or a
   bytecode blob. The host reads it right after the call; the next entry releases it. */
enum { BOX_RESULT_UNDEFINED = 0, BOX_RESULT_STRING = 1, BOX_RESULT_NUMBER = 2 };
static int32_t g_result_kind;
static const char *g_result_ptr;
static uint32_t g_result_len;
static double g_result_num;
static const char *g_result_cstr;  /* owned: a JS_ToCStringLen2 string */
static char *g_result_heap;        /* owned: malloc */
static uint8_t *g_result_blob;     /* owned: qjs_compile_bytecode */

/* Functions the host keeps a private reference to (QuickJSContext.captureGlobalFunction). */
#define BOX_SLOTS 16
static JSValue g_slots[BOX_SLOTS];
static uint8_t g_slot_used[BOX_SLOTS];

static void result_clear(void) {
    if (g_result_cstr) { JS_FreeCString(g_ctx, g_result_cstr); g_result_cstr = NULL; }
    if (g_result_heap) { free(g_result_heap); g_result_heap = NULL; }
    if (g_result_blob) { qjs_free_bytecode(g_ctx, g_result_blob); g_result_blob = NULL; }
    g_result_kind = BOX_RESULT_UNDEFINED;
    g_result_ptr = NULL;
    g_result_len = 0;
    g_result_num = 0;
}

/* Every entry starts here: QuickJS measures its stack from the pointer it sees now. */
static void enter(void) {
    result_clear();
    JS_UpdateStackTop(g_rt);
}

enum { BOX_VALUE = 0, BOX_EXCEPTION = 1, BOX_NOT_A_FUNCTION = 2 };

/* The pending exception as "name: message\nstack" (qjs_take_exception_description). */
static int32_t take_exception(void) {
    g_result_heap = qjs_take_exception_description(g_ctx);
    g_result_kind = BOX_RESULT_STRING;
    g_result_ptr = g_result_heap ? g_result_heap : "unknown error";
    g_result_len = (uint32_t)strlen(g_result_ptr);
    return BOX_EXCEPTION;
}

/* QuickJSContext.readResult: a string, a number, or undefined for anything else. Consumes v. */
static int32_t take_value(JSValue v) {
    if (qjs_is_exception(v)) return take_exception();
    if (qjs_is_string(v)) {
        size_t n = 0;
        g_result_cstr = JS_ToCStringLen2(g_ctx, &n, v, false);
        if (g_result_cstr) { g_result_kind = BOX_RESULT_STRING; g_result_ptr = g_result_cstr; g_result_len = (uint32_t)n; }
    } else if (qjs_is_number(v)) {
        g_result_kind = BOX_RESULT_NUMBER;
        g_result_num = qjs_to_float64(g_ctx, v);
    }
    qjs_free_value(g_ctx, v);
    return BOX_VALUE;
}

// ---- host functions --------------------------------------------------------------------------

static int on_interrupt(JSContext *ctx, void *opaque) {
    (void)ctx; (void)opaque;
    return host_interrupt();
}

/* The host's own handlers read two arguments at most; more than this many are not passed on. */
#define BOX_MAX_ARGS 8

/* hostTrampoline: strings as they are, numbers as numbers (the host formats them), anything else empty. */
static JSValue strings_trampoline(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)this_val;
    box_arg args[BOX_MAX_ARGS];
    if (argc > BOX_MAX_ARGS) argc = BOX_MAX_ARGS;
    for (int i = 0; i < argc; i++) {
        args[i].kind = BOX_ARG_OTHER;
        args[i].len = 0;
        args[i].ptr = NULL;
        args[i].reserved = 0;
        args[i].num = 0;
        if (qjs_is_string(argv[i])) {
            size_t n = 0;
            const char *s = JS_ToCStringLen(ctx, &n, argv[i]);
            if (s) { args[i].kind = BOX_ARG_STRING; args[i].ptr = s; args[i].len = (uint32_t)n; }
        } else if (qjs_is_number(argv[i])) {
            args[i].kind = BOX_ARG_NUMBER;
            args[i].num = qjs_to_float64(ctx, argv[i]);
        }
    }
    box_ret ret = {0, 0, 0};
    int32_t kind = host_call(magic, argc, args, &ret);
    for (int i = 0; i < argc; i++) {
        if (args[i].kind == BOX_ARG_STRING) JS_FreeCString(ctx, args[i].ptr);
    }
    switch (kind) {
    case BOX_RET_NULL:
        return qjs_null();
    case BOX_RET_NUMBER:
        return qjs_new_float64(ctx, ret.num);
    case BOX_RET_STRING:
    case BOX_RET_BYTES: {
        uint8_t *buf = malloc(ret.len ? ret.len : 1);
        if (!buf) return JS_ThrowOutOfMemory(ctx);
        host_take(buf, ret.len);
        JSValue v = kind == BOX_RET_STRING ? qjs_new_string_len(ctx, (const char *)buf, ret.len)
                                           : qjs_new_array_buffer_copy(ctx, buf, ret.len);
        free(buf);
        return v;
    }
    default:
        return qjs_undefined();
    }
}

/* wordsTrampoline: name(number, words: Float64Array, text). */
static JSValue words_trampoline(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)this_val;
    if (argc < 3 || !qjs_is_number(argv[0]) || !qjs_is_string(argv[2])) return qjs_undefined();
    uint8_t *words = NULL;
    size_t bytes = 0;
    if (qjs_typed_array_bytes(ctx, argv[1], &words, &bytes) != JS_TYPED_ARRAY_FLOAT64 || !words || bytes % 8 != 0) return qjs_undefined();
    size_t n = 0;
    const char *text = JS_ToCStringLen2(ctx, &n, argv[2], false);
    if (!text) return qjs_undefined();
    host_words(magic, qjs_to_float64(ctx, argv[0]), words, (uint32_t)bytes, text, (uint32_t)n);
    JS_FreeCString(ctx, text);
    return qjs_undefined();
}

/* opsTrampoline: name(words: Int32Array, strings: string). */
static JSValue ops_trampoline(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)this_val;
    if (argc < 2 || !qjs_is_string(argv[1])) return qjs_undefined();
    uint8_t *words = NULL;
    size_t bytes = 0;
    if (qjs_typed_array_bytes(ctx, argv[0], &words, &bytes) != JS_TYPED_ARRAY_INT32 || !words || bytes % 4 != 0) return qjs_undefined();
    size_t n = 0;
    const char *strings = JS_ToCStringLen2(ctx, &n, argv[1], false);
    if (!strings) return qjs_undefined();
    host_ops(magic, words, (uint32_t)bytes, strings, (uint32_t)n);
    JS_FreeCString(ctx, strings);
    return qjs_undefined();
}

/* The engine asks libc for the local time zone in one place, getTimezoneOffset(), and reads only
   tm_gmtoff. build.sh compiles the engine with -Dlocaltime_r=qjsbox_localtime_r, so that call lands here. */
struct tm *qjsbox_localtime_r(const time_t *restrict t, struct tm *restrict tm) {
    memset(tm, 0, sizeof *tm);
    tm->tm_gmtoff = host_tz_offset((int64_t)*t);
    return tm;
}

// ---- exports ---------------------------------------------------------------------------------

/* Once per instance. `js_stack_bytes` is QuickJS's own stack limit, measured on this module's stack. */
EXPORT(box_init) int32_t box_init(uint32_t js_stack_bytes) {
    if (g_rt) return -1;
    g_rt = JS_NewRuntime();
    if (!g_rt) return -2;
    JS_SetMaxStackSize(g_rt, js_stack_bytes);
    g_ctx = JS_NewContext(g_rt);
    if (!g_ctx) return -3;
    /* No module loader is installed: import() of anything fails, as in the native glue. */
    JS_SetInterruptHandler(g_rt, on_interrupt, NULL);
    return 0;
}

/* The cheap first check for one huge allocation. The real limit is the memory's maximum, which the
   host sets: this build of QuickJS cannot measure its blocks (docs/decisions.md, "The memory limit
   is the wasm memory's maximum"). */
EXPORT(box_set_memory_limit) void box_set_memory_limit(uint32_t bytes) { JS_SetMemoryLimit(g_rt, bytes); }

EXPORT(box_alloc) void *box_alloc(uint32_t bytes) { return malloc(bytes ? bytes : 1); }
EXPORT(box_free) void box_free(void *p) { free(p); }

enum { BOX_FN_STRINGS = 0, BOX_FN_WORDS = 1, BOX_FN_OPS = 2 };

/* A global function `name` that reaches the host with `magic`. 0 on success. */
EXPORT(box_register) int32_t box_register(const char *name, int32_t kind, int32_t magic, int32_t length) {
    enter();
    JSCFunctionMagic *fn = kind == BOX_FN_WORDS ? words_trampoline : kind == BOX_FN_OPS ? ops_trampoline : strings_trampoline;
    JSValue f = qjs_new_cfunction_magic(g_ctx, fn, name, length, magic);
    return qjs_set_global_prop(g_ctx, name, f) < 0 ? -1 : 0; /* takes ownership of f */
}

/* `code[len]` must be NUL (JS_Eval reads it). */
EXPORT(box_eval) int32_t box_eval(const char *code, uint32_t len, const char *filename, int32_t as_module) {
    enter();
    return take_value(qjs_eval(g_ctx, code, len, filename, as_module != 0));
}

/* One of the host's own precompiled bundles, in the TWQB container (qjs_shim.h). */
EXPORT(box_eval_bytecode) int32_t box_eval_bytecode(const uint8_t *blob, uint32_t blob_len, const char *source, uint32_t source_len) {
    enter();
    return take_value(qjs_eval_bytecode(g_ctx, blob, blob_len, source, source_len));
}

/* Compiles `code` into that container; the blob is the result's bytes. */
EXPORT(box_compile) int32_t box_compile(const char *code, uint32_t len, const char *filename) {
    enter();
    size_t n = 0;
    g_result_blob = qjs_compile_bytecode(g_ctx, code, len, filename, &n);
    if (!g_result_blob) return take_exception();
    g_result_kind = BOX_RESULT_STRING;
    g_result_ptr = (const char *)g_result_blob;
    g_result_len = (uint32_t)n;
    return BOX_VALUE;
}

/* Keeps a reference to the global function `name`: its slot (1…), or 0 when it is not a function. */
EXPORT(box_capture) int32_t box_capture(const char *name) {
    enter();
    JSValue fn = qjs_get_global_prop(g_ctx, name);
    if (qjs_is_function(g_ctx, fn)) {
        for (int i = 0; i < BOX_SLOTS; i++) {
            if (!g_slot_used[i]) { g_slot_used[i] = 1; g_slots[i] = fn; return i + 1; }
        }
    }
    qjs_free_value(g_ctx, fn);
    return 0;
}

EXPORT(box_release) void box_release(int32_t slot) {
    enter();
    if (slot < 1 || slot > BOX_SLOTS || !g_slot_used[slot - 1]) return;
    g_slot_used[slot - 1] = 0;
    qjs_free_value(g_ctx, g_slots[slot - 1]);
}

/* Calls the function in `slot`, or the global `name` when slot is 0, with string arguments. */
EXPORT(box_call) int32_t box_call(int32_t slot, const char *name, int32_t argc, const char *const *args, const uint32_t *lens) {
    enter();
    int captured = slot >= 1 && slot <= BOX_SLOTS && g_slot_used[slot - 1];
    JSValue fn = captured ? g_slots[slot - 1] : qjs_get_global_prop(g_ctx, name);
    int32_t status = BOX_NOT_A_FUNCTION;
    if (qjs_is_function(g_ctx, fn) && argc >= 0 && argc <= BOX_MAX_ARGS) {
        JSValue argv[BOX_MAX_ARGS];
        for (int i = 0; i < argc; i++) argv[i] = qjs_new_string_len(g_ctx, args[i], lens[i]);
        JSValue r = qjs_call(g_ctx, fn, argc, argv);
        for (int i = 0; i < argc; i++) qjs_free_value(g_ctx, argv[i]);
        status = take_value(r);
    }
    if (!captured) qjs_free_value(g_ctx, fn);
    return status;
}

EXPORT(box_result_kind) int32_t box_result_kind(void) { return g_result_kind; }
EXPORT(box_result_ptr) const char *box_result_ptr(void) { return g_result_ptr; }
EXPORT(box_result_len) uint32_t box_result_len(void) { return g_result_len; }
EXPORT(box_result_number) double box_result_number(void) { return g_result_num; }

EXPORT(box_job_pending) int32_t box_job_pending(void) { return qjs_is_job_pending(g_rt); }

/* One queued promise job: > 0 ran, 0 none, < 0 it threw (the description is the result). */
EXPORT(box_run_job) int32_t box_run_job(void) {
    enter();
    JSContext *ctx = NULL;
    int r = qjs_execute_pending_job(g_rt, &ctx);
    if (r < 0) take_exception();
    return r;
}

EXPORT(box_memory_used) uint32_t box_memory_used(void) { return (uint32_t)qjs_memory_used(g_rt); }

/* JS_ComputeMemoryUsage, the eleven numbers QuickJSContext.memoryUsage reports, in its order. */
EXPORT(box_memory_usage) void box_memory_usage(int64_t *out) {
    JSMemoryUsage u;
    JS_ComputeMemoryUsage(g_rt, &u);
    out[0] = u.malloc_size; out[1] = u.memory_used_size; out[2] = u.atom_size; out[3] = u.str_size;
    out[4] = u.obj_size; out[5] = u.prop_size; out[6] = u.shape_size; out[7] = u.js_func_size;
    out[8] = u.js_func_code_size; out[9] = u.js_func_pc2line_size; out[10] = u.binary_object_size;
}

EXPORT(box_gc) void box_gc(void) { JS_RunGC(g_rt); }
