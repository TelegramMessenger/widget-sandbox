# The C++ the Rust port is compared with, built from widget-sandbox's own sources: one small program per
# stage that reads cases on stdin and writes what the C++ makes of them on stdout (twidget's rust/PLAN.md, T4).
cmake_minimum_required(VERSION 3.20)
project(widget_sandbox_oracle CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(WS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(WS_SANDBOX "${WS_ROOT}/packages/desktop/sandbox")

if (MSVC)
    add_compile_options(/utf-8 /W3 /fp:precise)
else()
    # Rust never fuses a*b+c; the C++ must not either for the two to be compared bit for bit.
    add_compile_options(-ffp-contract=off)
endif()

add_executable(json_oracle json_oracle.cpp)
target_include_directories(json_oracle PRIVATE "${WS_SANDBOX}/include")

add_executable(stream_oracle stream_oracle.cpp)
target_include_directories(stream_oracle PRIVATE "${WS_SANDBOX}/include" "${WS_SANDBOX}/src")

# Stage S1: the desktop core's pure policies, each program built from its C++ source.
set(WS_SANDBOX_SRC "${WS_SANDBOX}/src")
foreach(policy cpu_budget sdk_policy gesture_policy bundle_stamp)
    add_executable(${policy}_oracle ${policy}_oracle.cpp "${WS_SANDBOX_SRC}/${policy}.cpp")
    target_include_directories(${policy}_oracle PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${WS_SANDBOX}/include" "${WS_SANDBOX_SRC}")
endforeach()
target_sources(bundle_stamp_oracle PRIVATE "${WS_SANDBOX_SRC}/resources.cpp")
