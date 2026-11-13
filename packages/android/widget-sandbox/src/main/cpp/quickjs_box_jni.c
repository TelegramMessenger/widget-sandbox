// JNI layer over the QuickJS box (packages/quickjs-box, through qjsbox_host.h)
// for BoxedQuickJS.kt. The box is the same fork commit as the native engine,
// compiled to wasm and back to C (wasm2c), running in a linear memory of its
// own; on Android it is always the bounds-check build. The twin of
// ios/.../QuickJSBoxed.swift.
//
// Strings cross the boundary as byte[] of real UTF-8 (JNI's jstring helpers
// speak modified UTF-8, which corrupts emoji). The scalar C surface of the box
// means no engine pointer ever reaches here; host functions receive their
// arguments as bytes inside the box, valid during the callback, and never call
// back into it. One box is used from one thread only (a JSThread), as the
// native engine is.
//
// The Kotlin deadline API is in CLOCK_BOOTTIME nanoseconds (wall) and thread
// CPU nanoseconds, like the native engine. The box checks its own deadlines in
// C against CLOCK_MONOTONIC and CLOCK_THREAD_CPUTIME_ID seconds, so the wall
// deadline is translated here from the one clock to the other; the CPU clock is
// the same (CLOCK_THREAD_CPUTIME_ID).

#include <jni.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <android/log.h>
#include "qjsbox_host.h"

#define TAG "twquickjsbox"
#define MAX_CAPTURED 16

typedef struct {
    qjsbox *box;
    JNIEnv *env;  // valid only while a native entry point is executing
    jobject self; // global ref to the owning BoxedQuickJS
    int64_t deadline_boot_ns; // the wall deadline the Kotlin side set (for the getter is not needed; kept for clarity)
    double wall_sec;          // the wall deadline in the box's clock (CLOCK_MONOTONIC seconds), INFINITY = off
    double cpu_sec;           // the CPU deadline in CLOCK_THREAD_CPUTIME_ID seconds, INFINITY = off
    struct { char *name; int32_t slot; } captured[MAX_CAPTURED];
    int captured_count;
} Box;

static jclass g_bytearray_class;    // [B
static jclass g_double_class;       // java/lang/Double
static jmethodID g_double_init;     // Double(double)
static jmethodID g_double_value;    // double doubleValue()
static jclass g_arraybuffer_class;  // HostReturn$ArrayBuffer
static jfieldID g_arraybuffer_bytes;
static jclass g_null_class;         // HostReturn$Null
static jclass g_exception_class;    // QuickJSException
static jmethodID g_dispatch;        // Object dispatch(int, byte[][])

static int64_t now_boottime_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static Box *box_of(jlong handle) { return (Box *)(intptr_t)handle; }

static jbyteArray bytes_from(JNIEnv *env, const char *s, size_t len) {
    jbyteArray a = (*env)->NewByteArray(env, (jsize)len);
    if (a && len) (*env)->SetByteArrayRegion(env, a, 0, (jsize)len, (const jbyte *)s);
    return a;
}

/* Raises QuickJSException in Java with the given UTF-8 bytes. The exception stays pending. */
static void raise_text_len(JNIEnv *env, const char *text, size_t len) {
    jstring s = NULL; // ThrowNew needs a C string; build one with a terminator.
    char *buf = (char *)malloc(len + 1);
    if (buf) {
        memcpy(buf, text, len);
        buf[len] = 0;
        (*env)->ThrowNew(env, g_exception_class, buf);
        free(buf);
    } else {
        (*env)->ThrowNew(env, g_exception_class, "out of memory");
    }
    (void)s;
}

static void raise_text(JNIEnv *env, const char *text) { raise_text_len(env, text, strlen(text)); }

/* The exception a call ended with (QJSBOX_EXCEPTION): the result's string is "name: message\nstack". */
static void raise_result_exception(JNIEnv *env, const qjsbox_result *r) {
    if (r->kind == QJSBOX_RESULT_STRING && r->str) raise_text_len(env, r->str, r->len);
    else raise_text(env, "unknown error");
}

/* qjsbox_result (VALUE) → byte[] for strings, Double for numbers, null for everything else. */
static jobject result_to_java(JNIEnv *env, const qjsbox_result *r) {
    if (r->kind == QJSBOX_RESULT_STRING && r->str) return bytes_from(env, r->str, r->len);
    if (r->kind == QJSBOX_RESULT_NUMBER) return (*env)->NewObject(env, g_double_class, g_double_init, r->num);
    return NULL;
}

/* The status/result of an eval- or call-like entry point → a Java value, or a raised QuickJSException. */
static jobject take(JNIEnv *env, int status, const qjsbox_result *r, const char *name) {
    switch (status) {
    case QJSBOX_VALUE:
        return result_to_java(env, r);
    case QJSBOX_EXCEPTION:
        raise_result_exception(env, r);
        return NULL;
    case QJSBOX_NOT_A_FUNCTION: {
        char msg[160];
        snprintf(msg, sizeof msg, "%s is not a function", name ? name : "value");
        raise_text(env, msg);
        return NULL;
    }
    default: // QJSBOX_REFUSED
        raise_text(env, "a call into the boxed engine was refused");
        return NULL;
    }
}

/* The box's single host-function callback (QJSBOX_FN_STRINGS). Runs on the engine's thread during a call into the box,
   and must not call into it: everything it needs is in `args`. It mirrors host_trampoline in quickjs_jni.c. */
static void box_call(void *opaque, int32_t magic, int32_t argc, const qjsbox_arg *args, qjsbox_return *ret) {
    Box *b = (Box *)opaque;
    JNIEnv *env = b ? b->env : NULL;
    if (!env) return;
    if ((*env)->PushLocalFrame(env, 8 + argc) < 0) return;

    jobjectArray arr = (*env)->NewObjectArray(env, argc, g_bytearray_class, NULL);
    for (int i = 0; i < argc; i++) {
        jbyteArray a;
        if (args[i].kind == QJSBOX_ARG_STRING && args[i].str) {
            a = bytes_from(env, args[i].str, args[i].len);
        } else if (args[i].kind == QJSBOX_ARG_NUMBER) {
            // What the native engine would hand over: a number formatted to a string (JS_ToCStringLen).
            double d = args[i].num;
            char num[32];
            int n = (d == (double)(int64_t)d && d >= -9.007199254740992e15 && d <= 9.007199254740992e15)
                        ? snprintf(num, sizeof num, "%lld", (long long)(int64_t)d)
                        : snprintf(num, sizeof num, "%.17g", d);
            a = bytes_from(env, num, n > 0 ? (size_t)n : 0);
        } else {
            a = bytes_from(env, "", 0);
        }
        (*env)->SetObjectArrayElement(env, arr, i, a);
        (*env)->DeleteLocalRef(env, a);
    }

    jobject res = (*env)->CallObjectMethod(env, b->self, g_dispatch, (jint)magic, arr);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        // Leave the return undefined; dispatch catches its own throwables, so this is a JNI-side failure only.
    } else if (res == NULL) {
        // undefined: leave the box's return as it is (BOX_RET_UNDEFINED).
    } else if ((*env)->IsInstanceOf(env, res, g_bytearray_class)) {
        jsize len = (*env)->GetArrayLength(env, (jbyteArray)res);
        jbyte *p = (*env)->GetByteArrayElements(env, (jbyteArray)res, NULL);
        qjsbox_return_string(ret, (const char *)p, (size_t)len);
        (*env)->ReleaseByteArrayElements(env, (jbyteArray)res, p, JNI_ABORT);
    } else if ((*env)->IsInstanceOf(env, res, g_double_class)) {
        qjsbox_return_number(ret, (*env)->CallDoubleMethod(env, res, g_double_value));
    } else if ((*env)->IsInstanceOf(env, res, g_arraybuffer_class)) {
        jbyteArray bytes = (jbyteArray)(*env)->GetObjectField(env, res, g_arraybuffer_bytes);
        jsize len = bytes ? (*env)->GetArrayLength(env, bytes) : 0;
        jbyte *p = bytes ? (*env)->GetByteArrayElements(env, bytes, NULL) : NULL;
        qjsbox_return_bytes(ret, (const uint8_t *)p, (size_t)len);
        if (p) (*env)->ReleaseByteArrayElements(env, bytes, p, JNI_ABORT);
    } else if ((*env)->IsInstanceOf(env, res, g_null_class)) {
        qjsbox_return_null(ret);
    }
    (*env)->PopLocalFrame(env, NULL);
}

// ───────────────────────── native methods ─────────────────────────

static jlong n_create(JNIEnv *env, jobject thiz, jlong memory_limit, jlong max_stack) {
    Box *b = (Box *)calloc(1, sizeof(Box));
    if (!b) { raise_text(env, "out of memory (box allocation)"); return 0; }
    b->wall_sec = INFINITY;
    b->cpu_sec = INFINITY;
    b->deadline_boot_ns = INT64_MAX;
    qjsbox_options options;
    options.memory_limit_bytes = memory_limit > 0 ? (size_t)memory_limit : 0;
    options.native_stack_bytes = max_stack > 0 ? (size_t)max_stack : 0;
    qjsbox_callbacks callbacks;
    callbacks.opaque = b;
    callbacks.call = box_call;
    callbacks.words = NULL; // the Android glue registers only string host functions (__host / __hostBinary)
    callbacks.ops = NULL;
    const char *error = NULL;
    b->box = qjsbox_new(&options, &callbacks, &error);
    if (!b->box) {
        free(b);
        raise_text(env, error ? error : "the QuickJS box could not be created");
        return 0;
    }
    b->self = (*env)->NewGlobalRef(env, thiz);
    return (jlong)(intptr_t)b;
}

static void n_destroy(JNIEnv *env, jobject thiz, jlong handle) {
    (void)thiz;
    Box *b = box_of(handle);
    if (!b) return;
    for (int i = 0; i < b->captured_count; i++) {
        if (b->captured[i].slot) qjsbox_release(b->box, b->captured[i].slot);
        free(b->captured[i].name);
    }
    qjsbox_free(b->box);
    if (b->self) (*env)->DeleteGlobalRef(env, b->self);
    free(b);
}

static void n_register(JNIEnv *env, jobject thiz, jlong handle, jstring name, jint magic) {
    (void)thiz;
    Box *b = box_of(handle);
    b->env = env;
    const char *n = (*env)->GetStringUTFChars(env, name, NULL);
    // length 2: name(kind, arg), as the native cfunction is declared.
    qjsbox_register(b->box, n, QJSBOX_FN_STRINGS, magic, 2);
    (*env)->ReleaseStringUTFChars(env, name, n);
    b->env = NULL;
}

static jobject n_eval(JNIEnv *env, jobject thiz, jlong handle, jbyteArray code, jstring filename, jboolean as_module) {
    (void)thiz;
    Box *b = box_of(handle);
    b->env = env;
    jsize len = (*env)->GetArrayLength(env, code);
    jbyte *buf = (*env)->GetByteArrayElements(env, code, NULL);
    const char *fn = (*env)->GetStringUTFChars(env, filename, NULL);
    qjsbox_result result;
    int status = qjsbox_eval(b->box, (const char *)buf, (size_t)len, fn, as_module ? true : false, &result);
    jobject out = take(env, status, &result, NULL);
    (*env)->ReleaseStringUTFChars(env, filename, fn);
    (*env)->ReleaseByteArrayElements(env, code, buf, JNI_ABORT);
    b->env = NULL;
    return out;
}

static jobject n_eval_bytecode(JNIEnv *env, jobject thiz, jlong handle, jbyteArray blob, jbyteArray source) {
    (void)thiz;
    Box *b = box_of(handle);
    b->env = env;
    jsize blob_len = (*env)->GetArrayLength(env, blob), src_len = (*env)->GetArrayLength(env, source);
    jbyte *g = (*env)->GetByteArrayElements(env, blob, NULL);
    jbyte *s = (*env)->GetByteArrayElements(env, source, NULL);
    qjsbox_result result;
    int status = qjsbox_eval_bytecode(b->box, (const uint8_t *)g, (size_t)blob_len, (const char *)s, (size_t)src_len, &result);
    jobject out = take(env, status, &result, NULL);
    (*env)->ReleaseByteArrayElements(env, blob, g, JNI_ABORT);
    (*env)->ReleaseByteArrayElements(env, source, s, JNI_ABORT);
    b->env = NULL;
    return out;
}

static jbyteArray n_compile_bytecode(JNIEnv *env, jobject thiz, jlong handle, jbyteArray code, jstring filename) {
    (void)thiz;
    Box *b = box_of(handle);
    b->env = env;
    jsize len = (*env)->GetArrayLength(env, code);
    jbyte *buf = (*env)->GetByteArrayElements(env, code, NULL);
    const char *fn = (*env)->GetStringUTFChars(env, filename, NULL);
    qjsbox_result result;
    int status = qjsbox_compile(b->box, (const char *)buf, (size_t)len, fn, &result);
    jbyteArray out = NULL;
    if (status == QJSBOX_VALUE && result.kind == QJSBOX_RESULT_STRING && result.str) {
        out = bytes_from(env, result.str, result.len);
    } else if (status == QJSBOX_EXCEPTION) {
        raise_result_exception(env, &result);
    } else {
        raise_text(env, "the box could not compile the bytecode");
    }
    (*env)->ReleaseStringUTFChars(env, filename, fn);
    (*env)->ReleaseByteArrayElements(env, code, buf, JNI_ABORT);
    b->env = NULL;
    return out;
}

static int32_t capture_slot_for(Box *b, const char *name) {
    for (int i = 0; i < b->captured_count; i++)
        if (strcmp(b->captured[i].name, name) == 0) return b->captured[i].slot;
    return 0;
}

static jboolean n_capture_global_function(JNIEnv *env, jobject thiz, jlong handle, jstring name) {
    (void)thiz;
    Box *b = box_of(handle);
    b->env = env;
    const char *n = (*env)->GetStringUTFChars(env, name, NULL);
    int32_t slot = qjsbox_capture(b->box, n);
    jboolean ok = JNI_FALSE;
    if (slot != 0) {
        int i = 0;
        for (; i < b->captured_count; i++) {
            if (strcmp(b->captured[i].name, n) == 0) {
                if (b->captured[i].slot) qjsbox_release(b->box, b->captured[i].slot);
                b->captured[i].slot = slot;
                break;
            }
        }
        if (i == b->captured_count) {
            if (b->captured_count < MAX_CAPTURED) {
                b->captured[b->captured_count].name = strdup(n);
                b->captured[b->captured_count].slot = slot;
                b->captured_count++;
            } else {
                qjsbox_release(b->box, slot); // no room: drop it (should not happen; the glue captures one name)
                slot = 0;
            }
        }
        ok = slot != 0 ? JNI_TRUE : JNI_FALSE;
    }
    (*env)->ReleaseStringUTFChars(env, name, n);
    b->env = NULL;
    return ok;
}

static jobject n_call(JNIEnv *env, jobject thiz, jlong handle, jstring name, jobjectArray args) {
    (void)thiz;
    Box *b = box_of(handle);
    b->env = env;
    const char *n = (*env)->GetStringUTFChars(env, name, NULL);
    int argc = args ? (*env)->GetArrayLength(env, args) : 0;
    if (argc > QJSBOX_MAX_ARGS) {
        char msg[96];
        snprintf(msg, sizeof msg, "more than %d arguments to %s", QJSBOX_MAX_ARGS, n);
        (*env)->ReleaseStringUTFChars(env, name, n);
        raise_text(env, msg);
        b->env = NULL;
        return NULL;
    }
    jbyteArray held[QJSBOX_MAX_ARGS];
    const char *argv[QJSBOX_MAX_ARGS];
    size_t lens[QJSBOX_MAX_ARGS];
    for (int i = 0; i < argc; i++) {
        held[i] = (jbyteArray)(*env)->GetObjectArrayElement(env, args, i);
        lens[i] = (size_t)(*env)->GetArrayLength(env, held[i]);
        argv[i] = (const char *)(*env)->GetByteArrayElements(env, held[i], NULL);
    }
    qjsbox_result result;
    int status = qjsbox_call(b->box, capture_slot_for(b, n), n, argc, argv, lens, &result);
    jobject out = take(env, status, &result, n);
    for (int i = 0; i < argc; i++) {
        (*env)->ReleaseByteArrayElements(env, held[i], (jbyte *)argv[i], JNI_ABORT);
        (*env)->DeleteLocalRef(env, held[i]);
    }
    (*env)->ReleaseStringUTFChars(env, name, n);
    b->env = NULL;
    return out;
}

static jboolean n_is_job_pending(JNIEnv *env, jobject thiz, jlong handle) {
    (void)env; (void)thiz;
    return qjsbox_job_pending(box_of(handle)->box) ? JNI_TRUE : JNI_FALSE;
}

/* >0 a job ran, 0 none pending; throws QuickJSException when the job failed. */
static jint n_execute_pending_job(JNIEnv *env, jobject thiz, jlong handle) {
    (void)thiz;
    Box *b = box_of(handle);
    b->env = env;
    qjsbox_result result;
    int r = qjsbox_run_job(b->box, &result);
    if (r < 0) raise_result_exception(env, &result);
    b->env = NULL;
    return r;
}

static jlong n_memory_used(JNIEnv *env, jobject thiz, jlong handle) {
    (void)env; (void)thiz;
    return (jlong)qjsbox_memory_used(box_of(handle)->box);
}

static void apply_deadlines(Box *b) { qjsbox_set_deadlines(b->box, b->wall_sec, b->cpu_sec); }

static void n_set_deadline(JNIEnv *env, jobject thiz, jlong handle, jlong boot_ns) {
    (void)env; (void)thiz;
    Box *b = box_of(handle);
    b->deadline_boot_ns = boot_ns;
    // Translate the wall deadline from the Kotlin clock (CLOCK_BOOTTIME) to the box's (CLOCK_MONOTONIC via qjsbox_uptime).
    b->wall_sec = boot_ns == INT64_MAX ? INFINITY : qjsbox_uptime() + (double)(boot_ns - now_boottime_ns()) / 1e9;
    apply_deadlines(b);
}

static void n_set_cpu_deadline(JNIEnv *env, jobject thiz, jlong handle, jlong cpu_ns) {
    (void)env; (void)thiz;
    Box *b = box_of(handle);
    // The CPU clock is the same (CLOCK_THREAD_CPUTIME_ID). Long.MIN_VALUE forces an immediate interrupt.
    b->cpu_sec = cpu_ns == INT64_MAX ? INFINITY : (cpu_ns == INT64_MIN ? -1.0 : (double)cpu_ns / 1e9);
    apply_deadlines(b);
}

static jboolean n_interrupted(JNIEnv *env, jobject thiz, jlong handle) {
    (void)env; (void)thiz;
    return qjsbox_interrupted(box_of(handle)->box) ? JNI_TRUE : JNI_FALSE;
}

static void n_reset_interrupted(JNIEnv *env, jobject thiz, jlong handle) {
    (void)env; (void)thiz;
    qjsbox_clear_interrupted(box_of(handle)->box);
}

static jboolean n_optimized(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    return qjsbox_build_optimized() ? JNI_TRUE : JNI_FALSE;
}

static jstring n_build_mode(JNIEnv *env, jclass clazz) {
    (void)clazz;
    return (*env)->NewStringUTF(env, qjsbox_build_mode());
}

static jstring n_quickjs_commit(JNIEnv *env, jclass clazz) {
    (void)clazz;
    return (*env)->NewStringUTF(env, qjsbox_quickjs_commit());
}

static const JNINativeMethod METHODS[] = {
    {"nativeCreate", "(JJ)J", (void *)n_create},
    {"nativeDestroy", "(J)V", (void *)n_destroy},
    {"nativeRegisterFunction", "(JLjava/lang/String;I)V", (void *)n_register},
    {"nativeEval", "(J[BLjava/lang/String;Z)Ljava/lang/Object;", (void *)n_eval},
    {"nativeEvalBytecode", "(J[B[B)Ljava/lang/Object;", (void *)n_eval_bytecode},
    {"nativeCompileBytecode", "(J[BLjava/lang/String;)[B", (void *)n_compile_bytecode},
    {"nativeCall", "(JLjava/lang/String;[[B)Ljava/lang/Object;", (void *)n_call},
    {"nativeIsJobPending", "(J)Z", (void *)n_is_job_pending},
    {"nativeExecutePendingJob", "(J)I", (void *)n_execute_pending_job},
    {"nativeMemoryUsed", "(J)J", (void *)n_memory_used},
    {"nativeSetDeadline", "(JJ)V", (void *)n_set_deadline},
    {"nativeSetCpuDeadline", "(JJ)V", (void *)n_set_cpu_deadline},
    {"nativeCaptureGlobalFunction", "(JLjava/lang/String;)Z", (void *)n_capture_global_function},
    {"nativeInterrupted", "(J)Z", (void *)n_interrupted},
    {"nativeResetInterrupted", "(J)V", (void *)n_reset_interrupted},
};

static const JNINativeMethod STATIC_METHODS[] = {
    {"nativeOptimized", "()Z", (void *)n_optimized},
    {"nativeBuildMode", "()Ljava/lang/String;", (void *)n_build_mode},
    {"nativeQuickJSCommit", "()Ljava/lang/String;", (void *)n_quickjs_commit},
};

static jclass global_class(JNIEnv *env, const char *name) {
    jclass local = (*env)->FindClass(env, name);
    if (!local) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "class not found: %s", name);
        return NULL;
    }
    jclass g = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    return g;
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass box_class = (*env)->FindClass(env, "org/telegram/widgetsandbox/BoxedQuickJS");
    if (!box_class) return JNI_ERR;
    if ((*env)->RegisterNatives(env, box_class, METHODS, sizeof METHODS / sizeof METHODS[0]) != JNI_OK) return JNI_ERR;
    if ((*env)->RegisterNatives(env, box_class, STATIC_METHODS, sizeof STATIC_METHODS / sizeof STATIC_METHODS[0]) != JNI_OK) return JNI_ERR;
    g_dispatch = (*env)->GetMethodID(env, box_class, "dispatch", "(I[[B)Ljava/lang/Object;");
    (*env)->DeleteLocalRef(env, box_class);
    g_bytearray_class = global_class(env, "[B");
    g_double_class = global_class(env, "java/lang/Double");
    g_arraybuffer_class = global_class(env, "org/telegram/widgetsandbox/HostReturn$ArrayBuffer");
    g_null_class = global_class(env, "org/telegram/widgetsandbox/HostReturn$Null");
    g_exception_class = global_class(env, "org/telegram/widgetsandbox/QuickJSException");
    if (!g_dispatch || !g_bytearray_class || !g_double_class || !g_arraybuffer_class || !g_null_class || !g_exception_class) return JNI_ERR;
    g_double_init = (*env)->GetMethodID(env, g_double_class, "<init>", "(D)V");
    g_double_value = (*env)->GetMethodID(env, g_double_class, "doubleValue", "()D");
    g_arraybuffer_bytes = (*env)->GetFieldID(env, g_arraybuffer_class, "bytes", "[B");
    if (!g_double_init || !g_double_value || !g_arraybuffer_bytes) return JNI_ERR;
    return JNI_VERSION_1_6;
}
