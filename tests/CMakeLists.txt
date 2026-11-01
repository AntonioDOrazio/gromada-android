# Each test: one executable, one translation unit (header-implemented modules may be
# included once per binary). Exit code 77 = skipped (no game data).
function(gromada_add_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests)
    target_compile_options(${name} PRIVATE -w)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

gromada_add_test(resources_test resources_test.cpp)
# Movies: synthetic tests (CI) and the full decode of the game's movies compared with ffmpeg.
gromada_add_test(video_test video_test.cpp ${PROJECT_SOURCE_DIR}/src/third_party/stb_vorbis_impl.c)
