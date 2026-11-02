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
gromada_add_test(demo_file_test demo_file_test.cpp)

# Script compiler and VM (src/script): unit tests, game-data test, desktop tool tools/script_run,
# byte-by-byte comparison with the Python replica of the original compiler (needs python3 and
# the game data; skipped otherwise).
gromada_add_test(script_test script_test.cpp)
gromada_add_test(script_data_test script_data_test.cpp)
add_executable(script_run ${PROJECT_SOURCE_DIR}/tools/script_run.cpp)
target_include_directories(script_run PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME script_run_demo
         COMMAND script_run --root ${CMAKE_CURRENT_SOURCE_DIR}/data/script --ticks 3 --trace demo.ini)
set_tests_properties(script_run_demo PROPERTIES PASS_REGULAR_EXPRESSION
    "\\[1\\] > main\\(\\)\n\\[1\\] Notify\\(100, \"begin\"\\)\n\\[2\\] > main\\(\\)\n\\[3\\] > main\\(\\)\n\\[3\\] > Report\\(20\\)\n\\[3\\] Notify\\(20, \"unit2\"\\)")
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME script_layout_compare
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/script_layout_compare.py $<TARGET_FILE:script_run>)
    set_tests_properties(script_layout_compare PROPERTIES SKIP_RETURN_CODE 77)
endif()
gromada_add_test(ui_text_test ui_text_test.cpp)
gromada_add_test(ui_data_test ui_data_test.cpp)
gromada_add_test(input_mapper_test input_mapper_test.cpp)
gromada_add_test(cursor_file_test cursor_file_test.cpp)
# Movies: synthetic tests (CI) and the full decode of the game's movies compared with ffmpeg.
gromada_add_test(video_test video_test.cpp ${PROJECT_SOURCE_DIR}/src/third_party/stb_vorbis_impl.c
    ${PROJECT_SOURCE_DIR}/src/third_party/dr_libs_impl.c)
gromada_add_test(video_data_test video_data_test.cpp ${PROJECT_SOURCE_DIR}/src/third_party/stb_vorbis_impl.c
    ${PROJECT_SOURCE_DIR}/src/third_party/dr_libs_impl.c)

# Music decoders (FLAC/WAV/OGG) through the Mixer, with synthetic files.
gromada_add_test(music_test music_test.cpp
    ${PROJECT_SOURCE_DIR}/src/third_party/stb_vorbis_impl.c
    ${PROJECT_SOURCE_DIR}/src/third_party/dr_libs_impl.c)
target_compile_definitions(music_test PRIVATE GROMADA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
