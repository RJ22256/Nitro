cmake_minimum_required(VERSION 3.24)
project(Nitro LANGUAGES C CXX ASM_MASM RC)

if(NOT MSVC)
    message(FATAL_ERROR "Build with Visual Studio 2022 (MSVC), x64.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Only x64 is supported: configure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the DLLs work inside any game without extra redistributables.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

set(DIST_DIR ${CMAKE_BINARY_DIR}/dist)

# --- third-party (downloaded at configure time; only sources/headers are used)
include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-add)
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.3.296
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-add)
FetchContent_MakeAvailable(minhook vulkan_headers)

add_library(minhook_static STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook_static PUBLIC ${minhook_SOURCE_DIR}/include
                                          PRIVATE ${minhook_SOURCE_DIR}/src ${minhook_SOURCE_DIR}/src/hde)

set(NITRO_WARNINGS $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>)

# --- core: frame generation pipeline + shared helpers
add_library(nitro_core STATIC
    common/config.cpp
    common/shader_extract.cpp
    core/framegen.cpp
    core/blit.cpp
    core/bridge.cpp
    core/runtime.cpp
    core/log.cpp)
target_compile_options(nitro_core PRIVATE ${NITRO_WARNINGS})

# --- NitroHook.dll: DX9/10/11/12 + OpenGL. Injected, or installed into the
# game folder as dxgi.dll / d3d9.dll / version.dll. Links no D3D/DXGI import
# libraries on purpose; everything is loaded at run time.
add_library(NitroHook SHARED
    hook/dllmain.cpp
    hook/dxgi_hooks.cpp
    hook/d3d9_hooks.cpp
    hook/gl_hooks.cpp
    hook/proxy_thunks.asm
    hook/exports.def)
target_compile_options(NitroHook PRIVATE ${NITRO_WARNINGS})
target_link_libraries(NitroHook PRIVATE nitro_core minhook_static)

# --- NitroVk.dll: Vulkan layer
add_library(NitroVk SHARED
    vk/layer.cpp
    vk/exports.def)
target_compile_options(NitroVk PRIVATE ${NITRO_WARNINGS})
target_include_directories(NitroVk PRIVATE ${vulkan_headers_SOURCE_DIR}/include)
target_link_libraries(NitroVk PRIVATE nitro_core)
add_custom_command(TARGET NitroVk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/vk/NitroVk.json ${DIST_DIR}/NitroVk.json)

# --- Nitro.exe: the app
add_executable(Nitro WIN32
    launcher/main.cpp
    launcher/nitro.rc
    common/config.cpp
    common/shader_extract.cpp)
target_compile_options(Nitro PRIVATE ${NITRO_WARNINGS})
target_link_libraries(Nitro PRIVATE comctl32 comdlg32 psapi advapi32)
add_dependencies(Nitro NitroHook NitroVk)

foreach(t NitroHook NitroVk Nitro)
    set_target_properties(${t} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DIST_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${DIST_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${DIST_DIR})
endforeach()
