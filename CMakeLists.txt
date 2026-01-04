cmake_minimum_required(VERSION 3.20)
project(King LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(King WIN32
    src/main.cpp
    src/king_window.cpp
    src/king/scene/camera.cpp
    src/king/scene/frustum.cpp
    src/king/render/material.cpp
    src/king/render/shader.cpp
)

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
