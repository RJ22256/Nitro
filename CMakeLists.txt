cmake_minimum_required(VERSION 3.20)
project(Nitro LANGUAGES CXX ASM_MASM)

if(NOT MSVC)
    message(FATAL_ERROR "Build with Visual Studio 2022 (MSVC), x64.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Only x64 is supported: configure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the DLL works inside any game without extra redistributables.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/permissive->
                    $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)

set(DIST_DIR ${CMAKE_BINARY_DIR}/dist)

# Hook DLL: injected into the game, or installed into the game folder as dxgi.dll.
# It links no D3D/DXGI import libraries on purpose; everything is loaded at run
# time so it can safely pretend to be dxgi.dll.
add_library(NitroHook SHARED
    hook/dllmain.cpp
    hook/hooks.cpp
    hook/framegen.cpp
    hook/blit.cpp
    hook/log.cpp
    hook/proxy_thunks.asm
    hook/exports.def
    common/config.cpp
    common/shader_extract.cpp
)
set_target_properties(NitroHook PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DIST_DIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${DIST_DIR}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${DIST_DIR})

# Launcher GUI
add_executable(Nitro WIN32
    launcher/main.cpp
    common/config.cpp
    common/shader_extract.cpp
)
target_link_libraries(Nitro PRIVATE comctl32 comdlg32 psapi advapi32)
set_target_properties(Nitro PROPERTIES
    OUTPUT_NAME "Nitro"
    RUNTIME_OUTPUT_DIRECTORY ${DIST_DIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${DIST_DIR}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${DIST_DIR})
add_dependencies(Nitro NitroHook)
