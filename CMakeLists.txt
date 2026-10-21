cmake_minimum_required(VERSION 3.20)
project(errt_fix LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the DLL does not depend on the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(errt_fix SHARED
    src/main.cpp
    src/hooks.cpp
    src/rt_cache.cpp
    src/pso_reuse.cpp
    src/vtidx.c
    src/dinput8.def
)
set_target_properties(errt_fix PROPERTIES OUTPUT_NAME dinput8)
target_compile_definitions(errt_fix PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(errt_fix PRIVATE /W3 /utf-8 /MP)
target_link_libraries(errt_fix PRIVATE d3d12 dxgi user32 psapi)
