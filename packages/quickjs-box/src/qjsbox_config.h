// How the generated C and WABT's runtime are compiled. Every translation unit of the target
// includes this file first: the generated files and the runtime are never compiled on their own,
// only through the wrappers in this directory. So the configuration cannot differ between them,
// and no build system has to pass a define to get it right.
#ifndef QJSBOX_CONFIG_H
#define QJSBOX_CONFIG_H

#include <stdint.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the QuickJS box is little-endian only"
#endif

// How the box's memory is fenced.
//   Guard pages (macOS): each box reserves 8 GiB of address space, and an access outside its
//   memory hits an unmapped page. Fastest. Nobody handles the fault: the process dies.
//   Bounds checks (everywhere else): every access is compared with the memory's size. No address
//   space to reserve, no signals, the same C. Slower.
// QJSBOX_BOUNDS_CHECKS=1 forces the second mode (the tests run both).
#ifndef QJSBOX_BOUNDS_CHECKS
#if defined(__APPLE__) && TARGET_OS_OSX && UINTPTR_MAX > 0xffffffffu
#define QJSBOX_BOUNDS_CHECKS 0
#else
#define QJSBOX_BOUNDS_CHECKS 1
#endif
#endif

#if QJSBOX_BOUNDS_CHECKS
#define WASM_RT_MEMCHECK_BOUNDS_CHECK 1
#define WASM_RT_MEMCHECK_GUARD_PAGES 0
#else
#define WASM_RT_USE_MMAP 1
#define WASM_RT_MEMCHECK_GUARD_PAGES 1
#define WASM_RT_MEMCHECK_BOUNDS_CHECK 0
#endif

// WABT's own SIGSEGV/SIGBUS handler is never installed: it claims every fault of the process and
// jumps out of it (docs/decisions.md, "QuickJS in a wasm2c box"). Nothing is installed in its place.
#define WASM_RT_SKIP_SIGNAL_RECOVERY 1

// A trap does not unwind: it ends the process with a reason (qjsbox_host.c). No setjmp anywhere.
#define WASM_RT_TRAP_HANDLER qjsbox_wasm_trap

// The stack. QuickJS limits its own recursion on the module's stack, and the host sets that limit
// from the native stack the thread has (qjsbox_js_stack_bytes). Past it there is the thread's guard
// page, as for the native engine; the generated code's frames are far smaller than a page, so it
// cannot be stepped over. WABT's per-call depth counter is left out: on Apple platforms it costs a
// thread-local access per call (10-20 % of the engine's speed).
#define WASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION 1

// 1 when this translation unit is optimized. An unoptimized box is 11-48 times slower than the
// optimized native engine, far past the CPU limits a widget lives under, so qjsbox_new refuses it.
// QJSBOX_ALLOW_UNOPTIMIZED=1 lifts that, to debug the box itself.
#if defined(__OPTIMIZE__)
#define QJSBOX_OPTIMIZED 1
#else
#define QJSBOX_OPTIMIZED 0
#endif

#endif
