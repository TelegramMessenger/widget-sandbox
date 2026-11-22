# twidget desktop core: the Nova sandbox engine, trusted validator, CPU budget and WidgetHost as a Qt-free C++20 library (for tdesktop or any Qt/C++ host), plus a console harness and tests.
# The engine is Nova (github.com/TelegramMessenger/nova-safe) behind the C interface of its nova_embed_ffi crate: -DTW_NOVA_ROOT=<a nova-safe checkout> builds it with cargo, or -DTW_NOVA_LIBRARY=<its static library> with -DTW_NOVA_INCLUDE=<the directory of nova_engine.h> links a built one.
# -DTW_BLITZ=ON adds the Blitz renderer prototype (the Rust crate in ../../apps/renderer-blitz built by cargo; dev tooling, see docs/blitz-experiment.md).
# Two libraries: twidget-sandbox-host, the host's side as objects (a hosted WidgetHost with its client and kept renderers, the stream's host end, the coding, the SDK and gesture policies, Intl, resources and threads; no engine, validator, in-process engine, local glue or engine end), and twidget-sandbox, the whole core: the engine and local side plus those objects, which every other program links.
# twidget-host-only links those objects alone, so its link proves the division.
cmake_minimum_required(VERSION 3.22)
project(twidget-desktop CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
if(MSVC)
    # The sources are UTF-8, as tdesktop compiles them; MSVC would read them in the system's code page.
    add_compile_options(/utf-8)
endif()

option(TW_BLITZ "Build the Blitz renderer prototype (needs rustup's cargo)" OFF)

set(TW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

set(TW_NOVA_ROOT "" CACHE PATH "A checkout of github.com/TelegramMessenger/nova-safe, built here with cargo")
set(TW_NOVA_LIBRARY "" CACHE FILEPATH "A built nova_embed_ffi static library, in place of TW_NOVA_ROOT")
set(TW_NOVA_INCLUDE "" CACHE PATH "The directory of nova_engine.h, with TW_NOVA_LIBRARY")
add_library(twnova STATIC IMPORTED)
if(TW_NOVA_LIBRARY)
    if(NOT EXISTS "${TW_NOVA_INCLUDE}/nova_engine.h")
        message(FATAL_ERROR "TW_NOVA_LIBRARY needs TW_NOVA_INCLUDE, the directory of nova_engine.h")
    endif()
    set_target_properties(twnova PROPERTIES IMPORTED_LOCATION "${TW_NOVA_LIBRARY}")
    target_include_directories(twnova INTERFACE "${TW_NOVA_INCLUDE}")
elseif(EXISTS "${TW_NOVA_ROOT}/nova_embed_ffi/include/nova_engine.h")
    find_program(TW_CARGO cargo HINTS /opt/homebrew/opt/rustup/bin "$ENV{HOME}/.cargo/bin")
    if(NOT TW_CARGO)
        message(FATAL_ERROR "TW_NOVA_ROOT needs cargo (rustup); none found in PATH, /opt/homebrew/opt/rustup/bin or ~/.cargo/bin")
    endif()
    set(TW_NOVA_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/nova-target")
    set(TW_NOVA_BUILT "${TW_NOVA_TARGET_DIR}/release/${CMAKE_STATIC_LIBRARY_PREFIX}nova_embed_ffi${CMAKE_STATIC_LIBRARY_SUFFIX}")
    file(GLOB_RECURSE TW_NOVA_SOURCES CONFIGURE_DEPENDS "${TW_NOVA_ROOT}/nova_embed_ffi/src/*.rs" "${TW_NOVA_ROOT}/nova_embed/src/*.rs" "${TW_NOVA_ROOT}/nova_vm/src/*.rs")
    add_custom_command(OUTPUT "${TW_NOVA_BUILT}"
        COMMAND "${TW_CARGO}" rustc --release --locked --package nova_embed_ffi --lib --crate-type staticlib --target-dir "${TW_NOVA_TARGET_DIR}"
        WORKING_DIRECTORY "${TW_NOVA_ROOT}"
        DEPENDS ${TW_NOVA_SOURCES} "${TW_NOVA_ROOT}/Cargo.toml" "${TW_NOVA_ROOT}/Cargo.lock"
        COMMENT "cargo build nova_embed_ffi (the first build takes about a minute)"
        VERBATIM)
    add_custom_target(twnova-cargo DEPENDS "${TW_NOVA_BUILT}")
    add_dependencies(twnova twnova-cargo)
    set_target_properties(twnova PROPERTIES IMPORTED_LOCATION "${TW_NOVA_BUILT}")
    target_include_directories(twnova INTERFACE "${TW_NOVA_ROOT}/nova_embed_ffi/include")
else()
    message(FATAL_ERROR "The Nova engine is not set: pass -DTW_NOVA_ROOT=<a checkout of github.com/TelegramMessenger/nova-safe>, or -DTW_NOVA_LIBRARY and -DTW_NOVA_INCLUDE for a built nova_embed_ffi")
endif()
# What `cargo rustc -- --print native-static-libs` lists for the crate.
if(WIN32)
    set_property(TARGET twnova APPEND PROPERTY INTERFACE_LINK_LIBRARIES advapi32 bcrypt kernel32 ntdll userenv ws2_32)
elseif(NOT APPLE)
    set_property(TARGET twnova APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${CMAKE_DL_LIBS} m)
endif()

find_package(Threads REQUIRED)

# The host's side: each of these links with the others and the system's libraries alone.
add_library(twidget-sandbox-host OBJECT
    sandbox/src/js_thread.cpp
    sandbox/src/sdk_policy.cpp
    sandbox/src/gesture_policy.cpp
    sandbox/src/intl.cpp
    sandbox/src/resources.cpp
    sandbox/src/remote_coding.cpp
    sandbox/src/remote_stream.cpp
    sandbox/src/sandbox_engine_shared.cpp
    sandbox/src/sandbox_engine_client.cpp
    sandbox/src/widget_host.cpp)
target_include_directories(twidget-sandbox-host PUBLIC sandbox/include)
target_link_libraries(twidget-sandbox-host PUBLIC Threads::Threads)
if(WIN32)
    target_compile_definitions(twidget-sandbox-host PRIVATE NOMINMAX)
endif()
if(NOT MSVC)
    target_compile_options(twidget-sandbox-host PRIVATE -Wall -Wextra)
endif()

add_library(twidget-sandbox STATIC
    sandbox/src/nova_context.cpp
    sandbox/src/cpu_budget.cpp
    sandbox/src/bundle_stamp.cpp
    sandbox/src/trusted_validator.cpp
    sandbox/src/sandbox_engine.cpp
    sandbox/src/remote_engine.cpp
    sandbox/src/remote_stream_engine.cpp
    sandbox/src/remote_hosted.cpp
    sandbox/src/widget_host_local.cpp
    $<TARGET_OBJECTS:twidget-sandbox-host>)
target_include_directories(twidget-sandbox PUBLIC sandbox/include)
target_link_libraries(twidget-sandbox PUBLIC twnova Threads::Threads)
if(WIN32)
    # Intl on the system's ICU (icu.dll, Windows 10 1903 and later).
    target_sources(twidget-sandbox PRIVATE sandbox/src/icu_intl.cpp)
    target_link_libraries(twidget-sandbox PUBLIC bcrypt icu)
    # windows.h's min and max macros would break std::min and std::max in the sources that include it.
    target_compile_definitions(twidget-sandbox PRIVATE NOMINMAX)
endif()
if(NOT MSVC)
    target_compile_options(twidget-sandbox PRIVATE -Wall -Wextra)
endif()

# ───────────── Blitz renderer prototype (optional) ─────────────
# The crate in ../../apps/renderer-blitz is built for the host triple by cargo (rustup's,
# like build-apple.sh and the Android module) and linked as a static library;
# twidget-renderer-blitz is the C++ Renderer over its C ABI.
if(TW_BLITZ)
    set(TW_BLITZ_CRATE "${TW_ROOT}/../apps/renderer-blitz")
    find_program(TW_CARGO cargo HINTS /opt/homebrew/opt/rustup/bin "$ENV{HOME}/.cargo/bin")
    find_program(TW_RUSTC rustc HINTS /opt/homebrew/opt/rustup/bin "$ENV{HOME}/.cargo/bin")
    if(NOT TW_CARGO OR NOT TW_RUSTC)
        message(FATAL_ERROR "TW_BLITZ needs cargo and rustc (rustup); none found in PATH, /opt/homebrew/opt/rustup/bin or ~/.cargo/bin")
    endif()
    if(NOT TW_RUST_TARGET)
        execute_process(COMMAND "${TW_RUSTC}" -vV OUTPUT_VARIABLE rustc_info OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "host: ([^\n]+)" _ "${rustc_info}")
        set(TW_RUST_TARGET "${CMAKE_MATCH_1}" CACHE STRING "Rust target triple the Blitz library is built for")
    endif()
    set(TW_BLITZ_LIB "${TW_BLITZ_CRATE}/target/${TW_RUST_TARGET}/release/${CMAKE_STATIC_LIBRARY_PREFIX}twidget_blitz${CMAKE_STATIC_LIBRARY_SUFFIX}")
    file(GLOB TW_BLITZ_SOURCES CONFIGURE_DEPENDS "${TW_BLITZ_CRATE}/src/*.rs" "${TW_BLITZ_CRATE}/include/*.h")
    add_custom_command(OUTPUT "${TW_BLITZ_LIB}"
        COMMAND "${TW_CARGO}" build --release --lib --target "${TW_RUST_TARGET}"
        WORKING_DIRECTORY "${TW_BLITZ_CRATE}"
        DEPENDS ${TW_BLITZ_SOURCES} "${TW_BLITZ_CRATE}/Cargo.toml" "${TW_BLITZ_CRATE}/Cargo.lock"
        COMMENT "cargo build renderer-blitz (${TW_RUST_TARGET}; the first build takes a minute or two)"
        VERBATIM)
    add_custom_target(twidget-blitz-cargo DEPENDS "${TW_BLITZ_LIB}")
    add_library(twidget-blitz-rs STATIC IMPORTED)
    set_target_properties(twidget-blitz-rs PROPERTIES IMPORTED_LOCATION "${TW_BLITZ_LIB}")
    add_dependencies(twidget-blitz-rs twidget-blitz-cargo)
    # What `cargo rustc -- --print native-static-libs` lists for the crate (system fonts via CoreText / fontconfig files).
    if(APPLE)
        set_property(TARGET twidget-blitz-rs PROPERTY INTERFACE_LINK_LIBRARIES "-framework Foundation" "-framework CoreText" "-framework CoreGraphics" "-framework CoreFoundation" objc iconv)
    elseif(UNIX)
        set_property(TARGET twidget-blitz-rs PROPERTY INTERFACE_LINK_LIBRARIES ${CMAKE_DL_LIBS} Threads::Threads m)
    else()
        message(FATAL_ERROR "TW_BLITZ: the Windows link line (ws2_32, userenv, bcrypt, ntdll, dwrite…) is not wired up yet")
    endif()

    add_library(twidget-renderer-blitz STATIC renderer/src/blitz_renderer.cpp)
    target_include_directories(twidget-renderer-blitz PUBLIC renderer/include "${TW_BLITZ_CRATE}/include")
    target_link_libraries(twidget-renderer-blitz PUBLIC twidget-sandbox twidget-blitz-rs)
    target_compile_definitions(twidget-renderer-blitz PUBLIC TW_HAVE_BLITZ=1)
    if(NOT MSVC)
        target_compile_options(twidget-renderer-blitz PRIVATE -Wall -Wextra)
    endif()
endif()

# ───────────── litehtml renderer prototype (optional) ─────────────
# The C++ core in ../../apps/renderer-litehtml (vendored litehtml, shadow tree re-parsed
# per batch, CoreText/CoreGraphics backend) built by its own CMakeLists and
# linked through its C ABI, the same shape as the Blitz one; macOS only until a
# non-Apple backend (QPainter on tdesktop) exists. See docs/litehtml-renderer.md.
if(TW_LITEHTML)
    if(NOT APPLE)
        message(FATAL_ERROR "TW_LITEHTML: only the CoreText/CoreGraphics backend exists so far (renderer-litehtml/src/apple); a QPainter backend is the tdesktop path")
    endif()
    add_subdirectory("${TW_ROOT}/../apps/renderer-litehtml" twidget-litehtml EXCLUDE_FROM_ALL)
    add_library(twidget-renderer-litehtml STATIC renderer/src/litehtml_renderer.cpp)
    target_include_directories(twidget-renderer-litehtml PUBLIC renderer/include)
    target_link_libraries(twidget-renderer-litehtml PUBLIC twidget-sandbox twidget_litehtml)
    target_compile_definitions(twidget-renderer-litehtml PUBLIC TW_HAVE_LITEHTML=1)
    if(NOT MSVC)
        target_compile_options(twidget-renderer-litehtml PRIVATE -Wall -Wextra)
    endif()
endif()

set(TW_WIDGET_JS "${CMAKE_CURRENT_SOURCE_DIR}/WidgetJS")
set(TW_CORPUS "${TW_ROOT}/../corpus")
set(TW_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")

add_executable(twidget-run harness/main.cpp)
target_link_libraries(twidget-run PRIVATE twidget-sandbox)
target_compile_definitions(twidget-run PRIVATE TW_WIDGET_JS="${TW_WIDGET_JS}" TW_CORPUS="${TW_CORPUS}")

# Built from the host's side alone: a hosted WidgetHost over a stream's host end with no engine end to reach.
add_executable(twidget-host-only harness/host_only.cpp)
target_link_libraries(twidget-host-only PRIVATE twidget-sandbox-host)
target_compile_definitions(twidget-host-only PRIVATE TW_WIDGET_JS="${TW_WIDGET_JS}")

add_executable(twidget-tests tests/test_main.cpp tests/test_render_ops.cpp tests/test_remote.cpp tests/test_remote_bridge.cpp tests/test_remote_stream.cpp tests/test_remote_stream_hosted.cpp tests/test_remote_stream_hosted_stop.cpp tests/test_hosted.cpp)
target_link_libraries(twidget-tests PRIVATE twidget-sandbox)
target_compile_definitions(twidget-tests PRIVATE TW_WIDGET_JS="${TW_WIDGET_JS}" TW_CORPUS="${TW_CORPUS}" TW_FIXTURES="${TW_FIXTURES}")

if(TW_BLITZ)
    target_link_libraries(twidget-run PRIVATE twidget-renderer-blitz)
    target_link_libraries(twidget-tests PRIVATE twidget-renderer-blitz)
endif()
if(TW_LITEHTML)
    target_link_libraries(twidget-run PRIVATE twidget-renderer-litehtml)
    target_link_libraries(twidget-tests PRIVATE twidget-renderer-litehtml)
endif()

enable_testing()
add_test(NAME sandbox COMMAND twidget-tests)
