cmake_minimum_required(VERSION 3.20)

# If building with the vcpkg toolchain, it may add an "applocal" post-build step
# that invokes `pwsh.exe` (PowerShell 7). This project only links system DLLs,
# so disable applocal dependency copying to avoid build errors on machines
# without PS7.
set(VCPKG_APPLOCAL_DEPS OFF CACHE BOOL "Disable vcpkg applocal deps" FORCE)

# Also request vcpkg to avoid PowerShell Core (pwsh.exe) entirely.
set(VCPKG_USE_POWERSHELL_CORE OFF CACHE BOOL "Avoid pwsh.exe" FORCE)

# Extra safety: when using the Visual Studio generator with vcpkg integration,
# MSBuild can inject an applocal step that invokes pwsh.exe. Disable it via VS globals.
if (MSVC)
    if (DEFINED CMAKE_VS_GLOBALS)
        set(CMAKE_VS_GLOBALS "${CMAKE_VS_GLOBALS};VcpkgEnableApplocalDeps=false;VcpkgUsePowerShellCore=false")
    else()
        set(CMAKE_VS_GLOBALS "VcpkgEnableApplocalDeps=false;VcpkgUsePowerShellCore=false")
    endif()
endif()

project(King LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(King WIN32
    src/main.cpp
    src/king_window.cpp
    src/king/thread_config.cpp
    src/king/render/d3d11/render_device_d3d11.cpp
    src/king/render/d3d11/render_system_d3d11.cpp
    src/king/render/d3d11/fullscreen_pass_d3d11.cpp
    src/king/render/d3d11/post_process_d3d11.cpp
    src/king/render/d3d11/shader_program_d3d11.cpp
    src/king/render/d3d11/texture_manager_d3d11.cpp
    src/king/systems/camera_system.cpp
    src/king/systems/lighting_system.cpp
    src/king/scene/camera.cpp
    src/king/scene/frustum.cpp
    src/king/time/time.cpp
    src/king/render/material.cpp
    src/king/render/shader.cpp
    src/king/render/d3d11/shadows.cpp
    src/king/perf/perf_analyzer.cpp
    src/king/perf/gpu_profiler_d3d11.cpp
)

# Simple arrow-key terminal UI for editing thread_config.cfg.
add_executable(ThreadConfigCLI
    src/thread_config_cli.cpp
    src/king/thread_config.cpp
)

# Match the main target: do not run vcpkg applocal (can invoke pwsh.exe).
set_target_properties(ThreadConfigCLI PROPERTIES VCPKG_APPLOCAL_DEPS OFF)

target_include_directories(ThreadConfigCLI PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (MSVC)
    target_compile_options(ThreadConfigCLI PRIVATE /W4 /permissive- /FS)
endif()

# Also disable applocal on the target itself (the toolchain can run before this
# CMakeLists is evaluated, so setting the cache variable alone isn't always enough).
set_target_properties(King PROPERTIES VCPKG_APPLOCAL_DEPS OFF)

target_compile_definitions(King PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

target_include_directories(King PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(King PRIVATE cxx_std_17)

target_link_libraries(King PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dxguid
    user32
    gdi32
    ole32
)

# Helpful for MSVC users
if (MSVC)
    target_compile_options(King PRIVATE /W4 /permissive- /FS)
endif()
