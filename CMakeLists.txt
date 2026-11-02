cmake_minimum_required(VERSION 3.22)
project(gromada_libretro VERSION 0.1.0 LANGUAGES C CXX)

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
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
    add_link_options(-stdlib=libc++)
endif()

add_library(gromada_libretro SHARED src/gromada_libretro.cpp
    src/third_party/stb_vorbis_impl.c src/third_party/dr_libs_impl.c)
target_include_directories(gromada_libretro PRIVATE src)
# The picture is drawn in bands on a few worker threads (issue #20, docs/rendering.md).
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(gromada_libretro PRIVATE Threads::Threads)
set(GROMADA_CXX_WARNINGS -Wall -Wno-unknown-attributes -Wno-deprecated-this-capture
    -Wno-defaulted-function-deleted -Wno-parentheses)
target_compile_options(gromada_libretro PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${GROMADA_CXX_WARNINGS}>"
    -fvisibility=hidden)
# stb_vorbis and dr_libs are third-party C: keep them quiet.
set_source_files_properties(src/third_party/stb_vorbis_impl.c src/third_party/dr_libs_impl.c
    PROPERTIES COMPILE_OPTIONS "-w")
set_target_properties(gromada_libretro PROPERTIES PREFIX "")

# The Indeo 3 decoder (src/video/indeo3.hpp) is LGPL-2.1-or-later: this option leaves it out.
option(GROMADA_NO_INDEO3 "Build without the LGPL Indeo 3 movie decoder" OFF)
if(GROMADA_NO_INDEO3)
    target_compile_definitions(gromada_libretro PRIVATE GROMADA_NO_INDEO3)
endif()

if(ANDROID)
    # RetroArch for Android loads cores named <name>_libretro_android.so
    # Configure with -DANDROID_STL=c++_static so the core carries its own libc++.
    set_target_properties(gromada_libretro PROPERTIES OUTPUT_NAME "gromada_libretro_android")
    target_link_options(gromada_libretro PRIVATE -Wl,--gc-sections -Wl,--exclude-libs,ALL)
endif()

option(GROMADA_BUILD_TESTS "Build unit tests" OFF)
if(GROMADA_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    add_subdirectory(tests)
    # Desktop preview of the UI module: screenshots of menus, a briefing and the credits.
    add_executable(ui_preview tools/ui_preview.cpp)
    target_include_directories(ui_preview PRIVATE src)
    target_compile_options(ui_preview PRIVATE "${GROMADA_CXX_WARNINGS}")
    add_test(NAME ui_preview_smoke COMMAND ui_preview --smoke ${CMAKE_BINARY_DIR}/ui_preview_out level=8 res=800)
    set_tests_properties(ui_preview_smoke PROPERTIES SKIP_RETURN_CODE 77)
    # Headless libretro frontend (screenshots, audio, scripted input): end-to-end tests.
    add_executable(harness tools/harness.cpp)
    target_link_libraries(harness PRIVATE ${CMAKE_DL_LIBS})
endif()
