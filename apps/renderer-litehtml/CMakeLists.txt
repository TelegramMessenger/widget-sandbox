# twidget litehtml renderer (prototype): the renderer trust domain of
# docs/protocol.md on litehtml (vendored in third_party/litehtml, see its
# VERSION). No JavaScript, no network: it only consumes validated RenderOps.
# One core (src/renderer.cpp), two glues: a JNI shared library for Android
# (src/android/jni.cpp, built by android/renderer-litehtml through Gradle's
# externalNativeBuild) and a C-ABI static library for iOS/macOS
# (src/apple/ffi.cpp + include/twidget_litehtml.h, built by build-apple.sh from
# the demos' Xcode projects). On a Mac from a shell it also builds the host
# harness `twidget-litehtml-run` and the tests. See docs/litehtml-renderer.md.
cmake_minimum_required(VERSION 3.22)
project(twidget-litehtml C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(TW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
# twidget::Json, the desktop core's JSON helper (one copy, shared with packages/desktop)
set(TW_JSON_INCLUDE "${TW_ROOT}/../packages/desktop/sandbox/include")
if(NOT EXISTS "${TW_JSON_INCLUDE}/twidget/json.h")
    message(FATAL_ERROR "twidget/json.h not found at ${TW_JSON_INCLUDE}; apps/renderer-litehtml must sit next to packages/desktop")
endif()

# litehtml + its gumbo, unmodified; its clang-tidy pass is off (it treats warnings as errors).
set(LITEHTML_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LITEHTML_ENABLE_LINT OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/litehtml EXCLUDE_FROM_ALL)

add_library(twidget_litehtml_core STATIC src/renderer.cpp)
target_include_directories(twidget_litehtml_core PUBLIC src "${TW_JSON_INCLUDE}")
target_link_libraries(twidget_litehtml_core PUBLIC litehtml)
if(NOT MSVC)
    target_compile_options(twidget_litehtml_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(ANDROID)
    add_library(twidget_litehtml SHARED src/android/jni.cpp)
    target_link_libraries(twidget_litehtml PRIVATE twidget_litehtml_core log)
    target_compile_options(twidget_litehtml PRIVATE -fvisibility=hidden)
    target_link_options(twidget_litehtml PRIVATE -Wl,--gc-sections)
elseif(APPLE)
    add_library(twidget_litehtml STATIC src/apple/coregraphics.cpp src/apple/ffi.cpp)
    target_include_directories(twidget_litehtml PUBLIC include)
    target_link_libraries(twidget_litehtml PUBLIC twidget_litehtml_core "-framework CoreGraphics" "-framework CoreText" "-framework ImageIO" "-framework CoreFoundation")
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # Host tooling: messages.json → PNG, plus the tests (ctest).
        add_executable(twidget-litehtml-run tools/run.cpp)
        target_link_libraries(twidget-litehtml-run PRIVATE twidget_litehtml)
        add_executable(twidget-litehtml-tests tests/test_main.cpp)
        target_link_libraries(twidget-litehtml-tests PRIVATE twidget_litehtml)
        enable_testing()
        add_test(NAME litehtml-renderer COMMAND twidget-litehtml-tests)
    endif()
endif()
