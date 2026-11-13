# QuickJS from the TelegramMessenger/quickjs fork, the git submodule at
# packages/ios/Sources/CQuickJS/quickjs (`git submodule update --init`), built for
# Android with the same qjs_shim.c plus a JNI layer written against the shim.
# One engine, one shim, one set of sources on every platform.
cmake_minimum_required(VERSION 3.22)
project(twquickjs C)

set(QJS "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../ios/Sources/CQuickJS")
if(NOT EXISTS "${QJS}/quickjs/quickjs.c")
    message(FATAL_ERROR "QuickJS sources not found at ${QJS}/quickjs: run git submodule update --init (packages/android must sit next to packages/ios)")
endif()

add_library(twquickjs SHARED
    "${QJS}/quickjs/quickjs.c"
    "${QJS}/quickjs/dtoa.c"
    "${QJS}/quickjs/libregexp.c"
    "${QJS}/quickjs/libunicode.c"
    "${QJS}/shim/qjs_shim.c"
    quickjs_jni.c)

target_include_directories(twquickjs PRIVATE "${QJS}/include" "${QJS}/quickjs")
target_compile_definitions(twquickjs PRIVATE _GNU_SOURCE QUICKJS_NG_BUILD)
# Always optimise the interpreter, also in debug builds: an -O0 quickjs-ng spends
# ~10 KB of C stack per JS call (the iOS lesson) and is several times slower.
target_compile_options(twquickjs PRIVATE -O2 -fvisibility=hidden -fno-strict-aliasing -Wno-unused-parameter)
target_link_options(twquickjs PRIVATE -Wl,--gc-sections)
target_link_libraries(twquickjs PRIVATE log m)

# ── The QuickJS box (opt-in; docs/decisions.md, "QuickJS in a wasm2c box") ──
# The same fork commit compiled to wasm and back to C (packages/quickjs-box/build.sh),
# so the engine runs in a linear memory of its own: a memory bug in it reads and
# writes that memory only, or ends the process. Bounds-check mode on Android (no
# guard pages: the 8 GiB reservation is not viable here and would clash with ART's
# own SIGSEGV use); qjsbox_config.h selects it automatically on a non-Apple target.
# Native QuickJS stays the default; this library is loaded only when a host opts in
# (BoxedQuickJS). Compile only the wrappers in src/ (each pulls in generated/ and
# wabt/ under qjsbox_config.h), with the box's three include paths (CLIENTS.md §2).
set(BOX "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../quickjs-box")
if(NOT EXISTS "${BOX}/src/qjsbox_host.c")
    message(FATAL_ERROR "QuickJS box sources not found at ${BOX}/src (packages/android must sit next to packages/quickjs-box)")
endif()
file(GLOB BOX_SRC "${BOX}/src/*.c")
add_library(twquickjsbox SHARED ${BOX_SRC} quickjs_box_jni.c)
target_include_directories(twquickjsbox PRIVATE "${BOX}/include" "${BOX}/generated" "${BOX}/wabt")
# The box refuses to start unoptimised (an -O0 box is 11-48x slower than native).
# Optimise in every configuration, as twquickjs does, and silence the generated code's noise.
target_compile_options(twquickjsbox PRIVATE -O2 -fvisibility=hidden -fno-strict-aliasing
    -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable)
target_link_options(twquickjsbox PRIVATE -Wl,--gc-sections)
target_link_libraries(twquickjsbox PRIVATE log m)
