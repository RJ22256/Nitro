cmake_minimum_required(VERSION 3.24)
project(Nitro LANGUAGES C CXX ASM_MASM RC)

# Configure twice:  -A x64   -> Nitro.exe, NitroHook.dll, NitroVk.dll
#                   -A Win32 -> NitroHook32.dll, NitroVk32.dll, NitroInject32.exe (for 32-bit games)
if(NOT MSVC)
    message(FATAL_ERROR "Build with Visual Studio 2022 (MSVC).")
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(NITRO_64 ON)
    set(NITRO_SUFFIX "")
else()
    set(NITRO_64 OFF)
    set(NITRO_SUFFIX "32")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the DLLs work inside any game without extra redistributables.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(NOT NITRO_64)
    set(CMAKE_ASM_MASM_FLAGS "${CMAKE_ASM_MASM_FLAGS} /safeseh")
endif()

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
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-add)
FetchContent_Declare(fidelityfx_fsr
    GIT_REPOSITORY https://github.com/GPUOpen-Effects/FidelityFX-FSR.git
    GIT_TAG v1.0.2
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR do-not-add)
FetchContent_MakeAvailable(minhook vulkan_headers imgui fidelityfx_fsr)
set(FSR_DIR ${fidelityfx_fsr_SOURCE_DIR}/ffx-fsr)

if(NITRO_64)
    set(HDE ${minhook_SOURCE_DIR}/src/hde/hde64.c)
else()
    set(HDE ${minhook_SOURCE_DIR}/src/hde/hde32.c)
endif()
add_library(minhook_static STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${HDE})
target_include_directories(minhook_static PUBLIC ${minhook_SOURCE_DIR}/include
                                          PRIVATE ${minhook_SOURCE_DIR}/src ${minhook_SOURCE_DIR}/src/hde)

# Dear ImGui + Nitro's look (used by the app and the in-game menu)
add_library(nitro_ui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    core/nitro_ui.cpp)
target_include_directories(nitro_ui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

set(NITRO_WARNINGS $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>)

# --- core: frame generation pipeline + shared helpers
add_library(nitro_core STATIC
    common/config.cpp
    common/shader_extract.cpp
    core/framegen.cpp
    core/blit.cpp
    core/bridge.cpp
    core/fsr.cpp
    core/runtime.cpp
    core/overlay.cpp
    core/log.cpp)
target_compile_options(nitro_core PRIVATE ${NITRO_WARNINGS})
target_include_directories(nitro_core SYSTEM PRIVATE ${FSR_DIR})
target_link_libraries(nitro_core PUBLIC nitro_ui minhook_static)

# --- NitroHook[32].dll: DX9/10/11/12 + OpenGL. Injected, or installed into the
# game folder as dxgi.dll / d3d9.dll / version.dll. Links no D3D/DXGI import
# libraries on purpose; everything is loaded at run time.
if(NITRO_64)
    set(THUNKS hook/proxy_thunks.asm)
else()
    set(THUNKS hook/proxy_thunks_x86.asm)
endif()
add_library(NitroHook SHARED
    hook/dllmain.cpp
    hook/dxgi_hooks.cpp
    hook/d3d9_hooks.cpp
    hook/gl_hooks.cpp
    ${THUNKS}
    hook/exports.def
    core/shaders/fsr.rc)
target_compile_options(NitroHook PRIVATE ${NITRO_WARNINGS})
target_include_directories(NitroHook PRIVATE ${FSR_DIR} ${CMAKE_SOURCE_DIR}/core/shaders)
target_link_libraries(NitroHook PRIVATE nitro_core minhook_static)

# --- NitroVk[32].dll: Vulkan layer
add_library(NitroVk SHARED
    vk/layer.cpp
    vk/exports.def
    core/shaders/fsr.rc)
target_compile_options(NitroVk PRIVATE ${NITRO_WARNINGS})
target_include_directories(NitroVk PRIVATE ${vulkan_headers_SOURCE_DIR}/include ${FSR_DIR}
                                           ${CMAKE_SOURCE_DIR}/core/shaders)
target_link_libraries(NitroVk PRIVATE nitro_core)
set(VK_MANIFEST NitroVk${NITRO_SUFFIX}.json)
add_custom_command(TARGET NitroVk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/vk/${VK_MANIFEST} ${DIST_DIR}/${VK_MANIFEST})

set(NITRO_TARGETS NitroHook NitroVk)
set_target_properties(NitroHook PROPERTIES OUTPUT_NAME NitroHook${NITRO_SUFFIX})
set_target_properties(NitroVk PROPERTIES OUTPUT_NAME NitroVk${NITRO_SUFFIX})

if(NITRO_64)
    # --- Nitro.exe: the app
    add_executable(Nitro WIN32
        launcher/main.cpp
        launcher/library.cpp
        launcher/nitro.rc
        common/config.cpp
        common/shader_extract.cpp
        common/inject.cpp)
    target_compile_options(Nitro PRIVATE ${NITRO_WARNINGS})
    target_link_libraries(Nitro PRIVATE nitro_ui d3d11 dxgi dwmapi comdlg32 psapi advapi32 shell32 ole32
                                        windowscodecs)
    add_dependencies(Nitro NitroHook NitroVk)
    list(APPEND NITRO_TARGETS Nitro)
else()
    # --- NitroInject32.exe: injects into 32-bit games for the 64-bit app
    add_executable(NitroInject32 WIN32
        launcher/inject32.cpp
        common/inject.cpp)
    target_compile_options(NitroInject32 PRIVATE ${NITRO_WARNINGS})
    target_link_libraries(NitroInject32 PRIVATE psapi shell32)
    list(APPEND NITRO_TARGETS NitroInject32)
endif()

foreach(t ${NITRO_TARGETS})
    set_target_properties(${t} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DIST_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${DIST_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${DIST_DIR})
endforeach()
