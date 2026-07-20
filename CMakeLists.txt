cmake_minimum_required(VERSION 3.20)
project(LeniaWallpaper CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(LeniaWallpaper WIN32
    src/main.cpp
    src/config.cpp
    src/renderer.cpp
    src/scheduler.cpp
    src/species.cpp
    src/tray.cpp
    src/util.cpp
    src/wallpaper_window.cpp
    src/app.rc
)

target_include_directories(LeniaWallpaper PRIVATE src)
target_compile_definitions(LeniaWallpaper PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(LeniaWallpaper PRIVATE /W4 /permissive-)
target_link_libraries(LeniaWallpaper PRIVATE
    d3d11 dxgi d3dcompiler d2d1 dwrite dwmapi wtsapi32 shell32 user32 gdi32 advapi32
)
