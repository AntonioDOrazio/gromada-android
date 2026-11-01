cmake_minimum_required(VERSION 3.22)
project(gromada_libretro VERSION 0.1.0 LANGUAGES CXX)

# Plain C++23 (no modules): builds with desktop clang + libc++ and with the Android NDK.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Desktop Linux: libc++ is required (libstdc++ 13 lacks <mdspan>).
if(NOT ANDROID AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
endif()

add_library(gromada_libretro SHARED src/gromada_libretro.cpp)
target_include_directories(gromada_libretro PRIVATE src)
target_compile_options(gromada_libretro PRIVATE -Wall -Wno-unknown-attributes -Wno-deprecated-this-capture
    -Wno-defaulted-function-deleted -Wno-parentheses -fvisibility=hidden)
set_target_properties(gromada_libretro PROPERTIES PREFIX "")

if(ANDROID)
    # RetroArch for Android loads cores named <name>_libretro_android.so
    # Configure with -DANDROID_STL=c++_static so the core carries its own libc++.
    set_target_properties(gromada_libretro PROPERTIES OUTPUT_NAME "gromada_libretro_android")
    target_link_options(gromada_libretro PRIVATE -Wl,--gc-sections -Wl,--exclude-libs,ALL)
endif()
