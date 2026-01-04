cmake_minimum_required(VERSION 3.20)

# If building with the vcpkg toolchain, it may add an "applocal" post-build step
# that invokes `pwsh.exe` (PowerShell 7). This project only links system DLLs,
# so disable applocal dependency copying to avoid build errors on machines
# without PS7.
set(VCPKG_APPLOCAL_DEPS OFF CACHE BOOL "Disable vcpkg applocal deps" FORCE)

project(King LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(King WIN32
    src/main.cpp
    src/king_window.cpp
    src/king/render/d3d11/render_device_d3d11.cpp
    src/king/render/d3d11/render_system_d3d11.cpp
    src/king/systems/camera_system.cpp
    src/king/systems/lighting_system.cpp
    src/king/scene/camera.cpp
    src/king/scene/frustum.cpp
    src/king/time/time.cpp
    src/king/render/material.cpp
    src/king/render/shader.cpp
)

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
    target_compile_options(King PRIVATE /W4 /permissive-)
endif()
