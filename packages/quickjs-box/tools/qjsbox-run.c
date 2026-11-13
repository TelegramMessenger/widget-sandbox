// A command-line host for the QuickJS box: the same C a client compiles (src/), without Swift.
// Built by tools/build.sh in both memory modes. It checks the box on its own, runs scripts in it,
// and gives the numbers CLIENTS.md asks for.
//
//   qjsbox-run info
//   qjsbox-run selftest [sandbox-bundle.js sandbox-bundle.qbc]   every check below; exit 1 on a failure
//   qjsbox-run eval [--memory BYTES] FILE.js...                  each file as a script; prints the result and __host calls
//   qjsbox-run bench sandbox-bundle.js sandbox-bundle.qbc        boot and JS timings (CPU time, best of 7)
#include "qjsbox_host.h"
#include "qjsbox_build.h"   /* generated/: the module's stack size */
#include "qjsbox_testing.h" /* src/: the box's memory, to corrupt it */

#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NATIVE_STACK (6u << 20) /* what JSThread gives the engine of its 8 MB */

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { perror(path); exit(2); }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

// ---- a host for the tests: records what the box asked for --------------------------------------

typedef struct {
    qjsbox *box;
    int print;                 /* eval: print every __host call */
    int calls, words_calls, ops_calls;
    char last[256];            /* "kind|arg" of the last __host call */
    double words_sum;
    long ops_sum;
    size_t ops_strings;
    uintptr_t deepest;         /* lowest native stack address seen in a callback */
    int reentered;             /* what a call from inside a callback returned */
} host;

static void on_call(void *opaque, int32_t magic, int32_t argc, const qjsbox_arg *a, qjsbox_return *ret) {
    host *h = opaque;
    h->calls++;
    uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    if (!h->deepest || here < h->deepest) h->deepest = here;
    char kind[64] = "", arg[160] = "";
    if (argc > 0 && a[0].kind == QJSBOX_ARG_STRING) snprintf(kind, sizeof kind, "%.*s", (int)a[0].len, a[0].str);
    if (argc > 1 && a[1].kind == QJSBOX_ARG_STRING) snprintf(arg, sizeof arg, "%.*s", (int)a[1].len, a[1].str);
    if (argc > 1 && a[1].kind == QJSBOX_ARG_NUMBER) snprintf(arg, sizeof arg, "%g", a[1].num);
    snprintf(h->last, sizeof h->last, "%s|%s", kind, arg);
    if (h->print) printf("  __host[%d](%s, %s)\n", magic, kind, arg);
    if (!strcmp(kind, "null")) qjsbox_return_null(ret);
    else if (!strcmp(kind, "number")) qjsbox_return_number(ret, 42.5);
    else if (!strcmp(kind, "string")) qjsbox_return_string(ret, "caf\xc3\xa9 \xf0\x9f\x8e\x89", 10);
    else if (!strcmp(kind, "bytes")) qjsbox_return_bytes(ret, (const uint8_t *)"\x01\x02\x03\x00\xff", 5);
    else if (!strcmp(kind, "big")) { size_t n = 300000; char *s = malloc(n); memset(s, 'x', n); qjsbox_return_string(ret, s, n); free(s); }
    else if (!strcmp(kind, "echo")) qjsbox_return_string(ret, a[1].str ? a[1].str : "", a[1].len);
    else if (!strcmp(kind, "reenter")) { qjsbox_result r; h->reentered = qjsbox_eval(h->box, "1", 1, "reenter.js", false, &r); }
}
static void on_words(void *opaque, int32_t magic, double number, const uint8_t *words, size_t bytes, const char *text, size_t text_len) {
    (void)magic; (void)text;
    host *h = opaque;
    h->words_calls++;
    h->words_sum = number + (double)text_len;
    for (size_t i = 0; i + 8 <= bytes; i += 8) { double d; memcpy(&d, words + i, 8); h->words_sum += d; }
}
static void on_ops(void *opaque, int32_t magic, const uint8_t *words, size_t bytes, const char *strings, size_t strings_len) {
    (void)magic; (void)strings;
    host *h = opaque;
    h->ops_calls++;
    h->ops_sum = 0;
    h->ops_strings = strings_len;
    for (size_t i = 0; i + 4 <= bytes; i += 4) { int32_t v; memcpy(&v, words + i, 4); h->ops_sum += v; }
}

static qjsbox *new_box(host *h, size_t memory) {
    memset(h, 0, sizeof *h);
    qjsbox_callbacks cb = {h, on_call, on_words, on_ops};
    qjsbox_options opt = {memory, NATIVE_STACK};
    const char *error = NULL;
    qjsbox *b = qjsbox_new(&opt, &cb, &error);
    if (!b) { fprintf(stderr, "qjsbox_new: %s\n", error); exit(2); }
    h->box = b;
    qjsbox_register(b, "__host", QJSBOX_FN_STRINGS, 0, 2);
    qjsbox_register(b, "__hostWords", QJSBOX_FN_WORDS, 1, 3);
    qjsbox_register(b, "__hostOps", QJSBOX_FN_OPS, 2, 2);
    return b;
}

static int ev(qjsbox *b, const char *code, qjsbox_result *r) { return qjsbox_eval(b, code, strlen(code), "test.js", false, r); }
static int is_string(const qjsbox_result *r, const char *want) { return r->kind == QJSBOX_RESULT_STRING && r->len == strlen(want) && !memcmp(r->str, want, r->len); }
static int has(const qjsbox_result *r, const char *part) { return r->kind == QJSBOX_RESULT_STRING && r->len && memmem(r->str, r->len, part, strlen(part)) != NULL; }
static double cpu(void) { return qjsbox_thread_cpu_time(); }
// The process's peak resident memory, in bytes (macOS reports bytes, Linux kilobytes).
static double peak_memory(void) {
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
    return (double)u.ru_maxrss;
#else
    return (double)u.ru_maxrss * 1024;
#endif
}

// ---- selftest --------------------------------------------------------------------------------

static int failures;
#define CHECK(name, cond) do { int ok_ = (cond); printf("%s  %s\n", ok_ ? "ok  " : "FAIL", name); if (!ok_) failures++; } while (0)

// What a child process does with the box, and how it ended.
static int child(void (*fn)(void), char *err, size_t err_cap) {
    int fds[2];
    if (pipe(fds)) return -1;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) { dup2(fds[1], 2); close(fds[0]); fn(); _exit(0); }
    close(fds[1]);
    ssize_t n = read(fds[0], err, err_cap - 1);
    err[n > 0 ? n : 0] = 0;
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) ? WTERMSIG(st) : -WEXITSTATUS(st);
}

// An engine whose memory is no longer its own: the host overwrites the box's heap, then runs a script.
// This stands in for an engine memory bug without shipping a way to cause one.
static void corrupted_engine(void) {
    alarm(20);
    host h;
    qjsbox *b = new_box(&h, 8 << 20);
    qjsbox_result r;
    ev(b, "var a = []; for (var i = 0; i < 1000; i++) a.push({i: i, s: 'x' + i}); a.length", &r);
    size_t size = 0;
    uint8_t *m = qjsbox_testing_memory(b, &size);
    size_t heap = QJSBOX_GUEST_STACK_BYTES + (256u << 10); /* past the module's stack and static data */
    memset(m + heap, 0xfb, size - heap);
    ev(b, "a.map(function (o) { return o.s + o.i; }).join()", &r);
    _exit(0);
}

static void *selftest(void *arg) {
    char **files = arg;
    host h;
    qjsbox_result r;
    qjsbox *b = new_box(&h, 8 << 20);
    printf("box: QuickJS %s, %s, %s\n", qjsbox_quickjs_commit(), qjsbox_build_mode(), qjsbox_build_optimized() ? "optimized" : "NOT optimized");

    CHECK("a script's value", ev(b, "[1, 2, 3].map(function (x) { return x * 2; }).join('-')", &r) == QJSBOX_VALUE && is_string(&r, "2-4-6"));
    CHECK("a number", ev(b, "6 * 7", &r) == QJSBOX_VALUE && r.kind == QJSBOX_RESULT_NUMBER && r.num == 42);
    CHECK("an object is undefined to the host", ev(b, "({})", &r) == QJSBOX_VALUE && r.kind == QJSBOX_RESULT_UNDEFINED);
    CHECK("an exception with its stack", ev(b, "function f() { null.x; } f()", &r) == QJSBOX_EXCEPTION && has(&r, "TypeError") && has(&r, "at f (test.js"));
    CHECK("a string with a NUL and non-ASCII", ev(b, "'a\\0b\\u00e9\\ud83c\\udf89'", &r) == QJSBOX_VALUE && r.len == 9 && !memcmp(r.str, "a\0b\xc3\xa9\xf0\x9f\x8e\x89", 9));
    CHECK("a module", qjsbox_eval(b, "export const x = 1; globalThis.fromModule = x + 1;", 50, "m.js", true, &r) == QJSBOX_VALUE && ev(b, "fromModule", &r) == QJSBOX_VALUE && r.num == 2);
    CHECK("import() of anything fails", ev(b, "var imp = 'pending'; import('x').then(function () { imp = 'loaded'; }, function (e) { imp = 'refused'; }); 0", &r) == QJSBOX_VALUE);
    while (qjsbox_run_job(b, &r) > 0) {}
    CHECK("  ... and its rejection arrives as a job", ev(b, "imp", &r) == QJSBOX_VALUE && is_string(&r, "refused"));

    CHECK("host function: strings in", ev(b, "__host('log', 'caf\\u00e9'); 1", &r) == QJSBOX_VALUE && !strcmp(h.last, "log|caf\xc3\xa9"));
    CHECK("host function: a number argument", ev(b, "__host('n', 1.5); 1", &r) == QJSBOX_VALUE && !strcmp(h.last, "n|1.5"));
    CHECK("host function: undefined, null, number", ev(b, "[__host('x'), __host('null'), __host('number')].map(String).join()", &r) == QJSBOX_VALUE && is_string(&r, "undefined,null,42.5"));
    CHECK("host function: a string back", ev(b, "__host('string')", &r) == QJSBOX_VALUE && is_string(&r, "caf\xc3\xa9 \xf0\x9f\x8e\x89"));
    CHECK("host function: bytes back as an ArrayBuffer", ev(b, "Array.from(new Uint8Array(__host('bytes'))).join()", &r) == QJSBOX_VALUE && is_string(&r, "1,2,3,0,255"));
    CHECK("host function: 300 KB back", ev(b, "__host('big').length", &r) == QJSBOX_VALUE && r.num == 300000);
    CHECK("host function: 1 MB in and out", ev(b, "var s = 'ab'.repeat(500000); __host('echo', s) === s", &r) == QJSBOX_VALUE);
    CHECK("words function", ev(b, "__hostWords(3, new Float64Array([1.5, 2.5]), 'four'); 1", &r) == QJSBOX_VALUE && h.words_calls == 1 && h.words_sum == 3 + 4 + 4);
    CHECK("words function refuses another array type", ev(b, "__hostWords(3, new Int32Array(2), 'x'); 1", &r) == QJSBOX_VALUE && h.words_calls == 1);
    CHECK("ops function", ev(b, "__hostOps(new Int32Array([7, 2, -1]), 'strings'); 1", &r) == QJSBOX_VALUE && h.ops_calls == 1 && h.ops_sum == 8 && h.ops_strings == 7);

    int32_t slot = (ev(b, "globalThis.greet = function (a, b) { return a + ', ' + b + '!'; }; 0", &r), qjsbox_capture(b, "greet"));
    const char *args[] = {"hello", "w\0rld"};
    size_t lens[] = {5, 5};
    CHECK("capture and call", slot > 0 && qjsbox_call(b, slot, NULL, 2, args, lens, &r) == QJSBOX_VALUE && r.len == 13 && !memcmp(r.str, "hello, w\0rld!", 13));
    ev(b, "delete globalThis.greet", &r);
    CHECK("a captured function outlives its global", qjsbox_call(b, slot, NULL, 2, args, lens, &r) == QJSBOX_VALUE && qjsbox_call(b, 0, "greet", 0, NULL, NULL, &r) == QJSBOX_NOT_A_FUNCTION);
    qjsbox_release(b, slot);
    CHECK("capture of something that is not a function", qjsbox_capture(b, "Math") == 0);

    ev(b, "var order = []; Promise.resolve(1).then(function (v) { order.push('a' + v); }); Promise.reject(new Error('boom')).catch(function (e) { order.push(e.message); }); 0", &r);
    int jobs = 0;
    while (qjsbox_job_pending(b) && qjsbox_run_job(b, &r) > 0) jobs++;
    CHECK("promise jobs", jobs >= 2 && ev(b, "order.join()", &r) == QJSBOX_VALUE && is_string(&r, "a1,boom"));

    qjsbox_set_deadlines(b, INFINITY, cpu() + 0.05);
    double t0 = cpu();
    int st = ev(b, "for (;;) { try { for (;;) {} } catch (e) {} }", &r);
    double took = (cpu() - t0) * 1e3;
    CHECK("the CPU deadline interrupts a loop that catches everything", st == QJSBOX_EXCEPTION && has(&r, "interrupted") && qjsbox_interrupted(b) && took > 40 && took < 400);
    qjsbox_clear_interrupted(b);
    qjsbox_set_deadlines(b, INFINITY, INFINITY);
    CHECK("  ... and the engine goes on", ev(b, "1 + 1", &r) == QJSBOX_VALUE && r.num == 2);

    // The stack: QuickJS's own limit fires, and the native stack it took stays inside the budget.
    uintptr_t top = (uintptr_t)__builtin_frame_address(0);
#define P "if (++d === t) __host('deep'); "
    static const char *shapes[] = {
        "function f() { " P "f(); }",
        "function f() { " P "[1].forEach(f); }",
        "function f() { " P "return f.apply(null, [1, 2, 3]); }",
        "function f() { " P "new Proxy({}, { get: f }).x; }",
        "function f() { " P "JSON.stringify({ toJSON: f }); }",
        "function f() { " P "'x'.replace(/x/, f); }",
        "function f() { " P "[3, 1].sort(f); }",
        "function f() { " P "String({ toString: f }); }",
        "function f() { " P "eval('f()'); }",
        "function f() { " P "new f(); }",
        "function f() { " P "Reflect.apply(f, null, []); }",
        "function* g() { " P "yield* g(); } function f() { g().next(); }",
        "function f() { " P "return f.bind(null)(); }",
        "function f() { " P "Object.defineProperty({}, 'x', { get: f }).x; }",
        "function f() { " P "[1].map(f); }",
        "function f() { " P "`${{ toString: f }}`; }",
        "class A { constructor() { " P "new A(); } } function f() { new A(); }",
    };
    size_t worst = 0;
    int stack_ok = 1;
    for (size_t i = 0; i < sizeof shapes / sizeof *shapes; i++) {
        host h2;
        qjsbox *s = new_box(&h2, 8 << 20);
        char code[512];
        // The depth at which QuickJS stops; then again, with a host call two frames short of it.
        snprintf(code, sizeof code, "var d = 0, t = -1; %s; var kind = 'none'; try { f(); } catch (e) { kind = e.name; } var max = d; kind", shapes[i]);
        int range = ev(s, code, &r) == QJSBOX_VALUE && is_string(&r, "RangeError");
        ev(s, "d = 0; t = max - 2; try { f(); } catch (e) {} max", &r);
        size_t used = h2.deepest ? top - h2.deepest : 0;
        if (used > worst) worst = used;
        if (!range || !used || used > NATIVE_STACK) { stack_ok = 0; printf("      shape %zu: %s, %g frames, native stack %zu KiB\n", i, range ? "RangeError" : "no RangeError", r.num, used >> 10); }
        qjsbox_free(s);
    }
    printf("      deepest native stack over %zu recursion shapes: %zu KiB of the %u KiB budget (QuickJS's limit %zu KiB)\n", sizeof shapes / sizeof *shapes, worst >> 10,
           NATIVE_STACK >> 10, qjsbox_js_stack_bytes(NATIVE_STACK) >> 10);
    CHECK("runaway recursion ends in RangeError inside the native stack budget", stack_ok);

    // The memory limit is the memory's maximum.
    host hm;
    qjsbox *m = new_box(&hm, 8 << 20);
    st = ev(m, "var keep = [], n = 0; for (; n < 200; n++) keep.push(new Uint8Array(1 << 20).fill(1)); n", &r);
    size_t grown = qjsbox_linear_memory_bytes(m);
    CHECK("an 8 MB limit stops the heap (out of memory, memory within the limit)", st == QJSBOX_EXCEPTION && has(&r, "out of memory") && grown <= qjsbox_linear_memory_limit_bytes(m) && grown <= (10u << 20) + (2u << 20));
    printf("      linear memory %zu KiB, limit %zu KiB\n", grown >> 10, qjsbox_linear_memory_limit_bytes(m) >> 10);
    CHECK("  ... and the engine still answers", ev(m, "keep.length > 3 && keep.length < 10", &r) == QJSBOX_VALUE);
    qjsbox_set_memory_limit(m, 32 << 20);
    CHECK("a raised limit lets it grow again", ev(m, "for (var k = 0; k < 10; k++) keep.push(new Uint8Array(1 << 20)); keep.length", &r) == QJSBOX_VALUE && r.num > 13);
    qjsbox_free(m);

    // The host's time zone reaches Date (TZ is set by tools/selftest.sh).
    if (getenv("QJSBOX_EXPECT_TZ")) {
        ev(b, "[new Date(Date.UTC(2026, 0, 15, 12)).getTimezoneOffset(), new Date(Date.UTC(2026, 6, 15, 12)).getTimezoneOffset(), new Date(2026, 0, 15, 12).getHours()].join()", &r);
        printf("      winter offset, summer offset, local noon: %.*s (want %s)\n", (int)r.len, r.str, getenv("QJSBOX_EXPECT_TZ"));
        CHECK("Date follows the host's time zone, daylight saving included", is_string(&r, getenv("QJSBOX_EXPECT_TZ")));
    }

    if (files) {
        size_t sn, bn;
        char *src = read_file(files[0], &sn);
        char *qbc = read_file(files[1], &bn);
        const char *base = strrchr(files[0], '/') ? strrchr(files[0], '/') + 1 : files[0];
        host hb;
        qjsbox *c = new_box(&hb, 64 << 20);
        st = qjsbox_compile(c, src, sn, base, &r);
        CHECK("the shipped .qbc is byte for byte what the box compiles", st == QJSBOX_VALUE && r.len == bn && !memcmp(r.str, qbc, bn));
        qjsbox_register(c, "__hostBinary", QJSBOX_FN_STRINGS, 3, 0);
        st = qjsbox_eval_bytecode(c, (const uint8_t *)qbc, bn, src, sn, &r);
        CHECK("the shipped .qbc runs in the box", st == QJSBOX_VALUE && qjsbox_capture(c, "__twDispatch") > 0);
        qbc[bn / 2] ^= 1;
        src[sn - 1] ^= 1;
        CHECK("a .qbc of another source is refused", qjsbox_eval_bytecode(c, (const uint8_t *)qbc, bn, src, sn, &r) == QJSBOX_EXCEPTION && has(&r, "different source"));
        qjsbox_free(c);
    }

    // No engine code at teardown, and many boxes side by side.
    enum { N = 200 };
    static qjsbox *many[N];
    static host hs[N];
    double w0 = qjsbox_uptime(), m0 = peak_memory();
    for (int i = 0; i < N; i++) many[i] = new_box(&hs[i], 8 << 20);
    double each = (qjsbox_uptime() - w0) * 1e3 / N, memory_each = (peak_memory() - m0) / N;
    int distinct = 1;
    for (int i = 0; i < N; i++) {
        char code[64];
        snprintf(code, sizeof code, "globalThis.id = %d; id", i);
        ev(many[i], code, &r);
    }
    for (int i = 0; i < N; i++) { ev(many[i], "id", &r); if (r.num != i) distinct = 0; }
    for (int i = 0; i < N; i++) qjsbox_free(many[i]);
    printf("      %d boxes: %.3f ms and about %.0f KiB of resident memory each to create\n", N, each, memory_each / 1024);
    CHECK("200 boxes keep their own state", distinct);

#ifdef NDEBUG
    ev(b, "__host('reenter'); 1", &r);
    CHECK("a call from inside a callback is refused", h.reentered == QJSBOX_REFUSED);
#endif

    char err[600];
    int sig = child(corrupted_engine, err, sizeof err);
    printf("      corrupted heap: signal %d%s%s", sig, *err ? ", stderr: " : "\n", err);
    CHECK("an engine with a corrupted heap ends the process, it does not go on", sig == SIGABRT || sig == SIGSEGV || sig == SIGBUS);
    CHECK("  ... with a reason when it is a trap", (sig != SIGABRT) || strstr(err, "QuickJS box fault") != NULL);

    qjsbox_free(b);
    printf(failures ? "FAILED: %d\n" : "all checks passed\n", failures);
    return NULL;
}

// ---- eval, bench -----------------------------------------------------------------------------

static void eval_files(int argc, char **argv) {
    size_t memory = 8 << 20;
    host h;
    qjsbox *b = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--memory") && i + 1 < argc) { memory = (size_t)atol(argv[++i]); continue; }
        if (!b) { b = new_box(&h, memory); h.print = 1; }
        size_t n;
        char *src = read_file(argv[i], &n);
        qjsbox_result r;
        int st = qjsbox_eval(b, src, n, argv[i], false, &r);
        while (qjsbox_run_job(b, NULL) > 0) {}
        if (r.kind == QJSBOX_RESULT_NUMBER) printf("%s: %s %g\n", argv[i], st ? "exception" : "value", r.num);
        else printf("%s: %s %.*s\n", argv[i], st ? "exception" : "value", (int)r.len, r.kind == QJSBOX_RESULT_STRING ? r.str : "");
        free(src);
    }
    printf("alive: linear memory %zu KiB\n", b ? qjsbox_linear_memory_bytes(b) >> 10 : 0);
}

static void bench(char **files) {
    size_t sn, bn;
    char *src = read_file(files[0], &sn);
    char *qbc = read_file(files[1], &bn);
    host h;
    qjsbox_result r;
    double best = 1e9;
    printf("mode\t%s\n", qjsbox_build_mode());
    for (int i = 0; i < 7; i++) {
        double t0 = cpu();
        qjsbox *b = new_box(&h, 8 << 20);
        qjsbox_register(b, "__hostBinary", QJSBOX_FN_STRINGS, 3, 0);
        qjsbox_eval_bytecode(b, (const uint8_t *)qbc, bn, src, sn, &r);
        double t = (cpu() - t0) * 1e3;
        if (t < best) best = t;
        qjsbox_free(b);
    }
    printf("boot: new box + sandbox bundle from .qbc\t%.2f ms\n", best);
    static const char *names[] = {"call-heavy: fib(30)", "property/array loop", "string building + split/join", "JSON parse/stringify", "regexp exec loop", "Map/Set churn", "typed array math", "closures + array methods"};
    static const char *codes[] = {
        "function fib(n){ return n < 2 ? n : fib(n-1) + fib(n-2); } fib(30)",
        "var o = []; for (var i = 0; i < 300000; i++) o.push({x: i, y: i * 2}); var s = 0; for (var k = 0; k < 6; k++) for (var i = 0; i < o.length; i++) s += o[i].x + o[i].y; s",
        "var s = ''; for (var i = 0; i < 200000; i++) s += (i % 10); var p = s.split('5').join('-'); for (var k = 0; k < 20; k++) p = p.replace('-', '+'); p.length",
        "var d = []; for (var i = 0; i < 20000; i++) d.push({id: i, name: 'item' + i, tags: ['a', 'b', i % 7], v: i / 3}); var t = 0; for (var k = 0; k < 6; k++) t += JSON.parse(JSON.stringify(d)).length; t",
        "var re = /(\\d+)-(\\w+)@([a-z]+)\\.com/g, txt = ''; for (var i = 0; i < 20000; i++) txt += i + '-user' + i + '@example.com '; var c = 0; for (var k = 0; k < 4; k++) { re.lastIndex = 0; while (re.exec(txt)) c++; } c",
        "var m = new Map(), st = new Set(); for (var i = 0; i < 200000; i++) { m.set('k' + (i % 5000), i); st.add(i % 3000); } var q = 0; m.forEach(function (v) { q += v; }); q + st.size",
        "var a = new Float64Array(200000); for (var k = 0; k < 10; k++) for (var i = 0; i < a.length; i++) a[i] = Math.sqrt(i) * 1.0001 + a[(i + 1) % a.length]; a[1000]",
        "var arr = []; for (var i = 0; i < 100000; i++) arr.push(i); var z = 0; for (var k = 0; k < 6; k++) z += arr.map(function (x) { return x * 2; }).filter(function (x) { return x % 3 === 0; }).reduce(function (a, b) { return a + b; }, 0); z",
    };
    for (size_t i = 0; i < sizeof codes / sizeof *codes; i++) {
        best = 1e9;
        for (int k = 0; k < 7; k++) {
            qjsbox *b = new_box(&h, 64 << 20);
            double t0 = cpu();
            ev(b, codes[i], &r);
            double t = (cpu() - t0) * 1e3;
            if (t < best) best = t;
            qjsbox_free(b);
        }
        printf("%s\t%.2f ms\n", names[i], best);
    }
    qjsbox *b = new_box(&h, 8 << 20);
    ev(b, "function tick() { return 1; }", &r);
    int32_t slot = qjsbox_capture(b, "tick");
    double t0 = cpu();
    for (int i = 0; i < 200000; i++) qjsbox_call(b, slot, NULL, 0, NULL, NULL, &r);
    printf("host -> box call\t%.3f us\n", (cpu() - t0) * 1e6 / 200000);
    t0 = cpu();
    ev(b, "for (var i = 0; i < 300000; i++) __host('noop', 'x'); 1", &r);
    printf("box -> host call\t%.3f us\n", (cpu() - t0) * 1e6 / 300000);
    qjsbox_free(b);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 2 && !strcmp(argv[1], "info")) {
        printf("QuickJS %s, %s, %s\n", qjsbox_quickjs_commit(), qjsbox_build_mode(), qjsbox_build_optimized() ? "optimized" : "NOT optimized");
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "selftest")) {
        // Like JSThread: the engine on a thread of its own with an 8 MB stack.
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u << 20);
        pthread_t t;
        pthread_create(&t, &attr, selftest, argc >= 4 ? argv + 2 : NULL);
        pthread_join(t, NULL);
        return failures ? 1 : 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "eval")) { eval_files(argc - 2, argv + 2); return 0; }
    if (argc >= 4 && !strcmp(argv[1], "bench")) { bench(argv + 2); return 0; }
    fprintf(stderr, "usage: qjsbox-run info | selftest [bundle.js bundle.qbc] | eval [--memory BYTES] FILE.js... | bench bundle.js bundle.qbc\n");
    return 2;
}
