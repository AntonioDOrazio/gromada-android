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
# Controls on the gameplay (issue #16): move orders, follow camera, weapons (synthetic).
gromada_add_test(controls_test controls_test.cpp)
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

# Sound-effect channels of the original (issue #17) and the Mixer's looping voices.
gromada_add_test(sound_channels_test sound_channels_test.cpp
    ${PROJECT_SOURCE_DIR}/src/third_party/stb_vorbis_impl.c
    ${PROJECT_SOURCE_DIR}/src/third_party/dr_libs_impl.c)

# Differential oracle (issue #11): reference math vs vectors from the original exe (no game data).
gromada_add_test(oracle_test oracle_test.cpp)
target_compile_definitions(oracle_test PRIVATE GROMADA_ORACLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/oracle")
# Simulation engine (src/game, issue #8): synthetic tests, data-backed level runs and the
# desktop driver tools/engine_run (screenshots + trace).
gromada_add_test(engine_test engine_test.cpp)
gromada_add_test(engine_data_test engine_data_test.cpp)
set_tests_properties(engine_data_test PROPERTIES TIMEOUT 1800)
add_executable(engine_run ${PROJECT_SOURCE_DIR}/tools/engine_run.cpp)
target_include_directories(engine_run PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(engine_run PRIVATE -Wall -Wno-unknown-attributes -Wno-parentheses -Wno-sign-compare)
# Game session (src/game/session*.hpp): synthetic tests (mock engine, synthetic scripts) and
# the whole flow of the game + the three demos on the original data (provisional engine).
gromada_add_test(session_test session_test.cpp
    ${PROJECT_SOURCE_DIR}/src/third_party/stb_vorbis_impl.c
    ${PROJECT_SOURCE_DIR}/src/third_party/dr_libs_impl.c)
gromada_add_test(session_data_test session_data_test.cpp
    ${PROJECT_SOURCE_DIR}/src/third_party/stb_vorbis_impl.c
    ${PROJECT_SOURCE_DIR}/src/third_party/dr_libs_impl.c)

# The core end to end through the headless frontend (needs the game data, else skipped).
add_test(NAME core_e2e COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/core_e2e.sh $<TARGET_FILE:harness>
         $<TARGET_FILE:gromada_libretro> ${CMAKE_BINARY_DIR}/core_e2e_out)
add_test(NAME core_e2e_169_120fps COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/core_e2e.sh $<TARGET_FILE:harness>
         $<TARGET_FILE:gromada_libretro> ${CMAKE_BINARY_DIR}/core_e2e_169_out gromada_aspect=16:9 gromada_fps=120)
set_tests_properties(core_e2e core_e2e_169_120fps PROPERTIES SKIP_RETURN_CODE 77)
# Vehicle engine sounds loop instead of restarting every animation cycle (issue #17).
add_test(NAME core_sfx_loop COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/core_sfx_loop.sh $<TARGET_FILE:harness>
         $<TARGET_FILE:gromada_libretro> ${CMAKE_BINARY_DIR}/core_sfx_loop_out)
add_test(NAME core_sfx_loop_120fps COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/core_sfx_loop.sh $<TARGET_FILE:harness>
         $<TARGET_FILE:gromada_libretro> ${CMAKE_BINARY_DIR}/core_sfx_loop_120_out gromada_fps=120)
set_tests_properties(core_sfx_loop core_sfx_loop_120fps PROPERTIES SKIP_RETURN_CODE 77)

# Controls on the real gameplay (issue #16): direct scheme drives to a point and hits a target,
# menus by pad only and by touch only; 60/120 fps x 4:3/16:9 (needs the game data).
foreach(mode pad touch)
    foreach(cfg "60;4:3" "120;4:3" "60;16:9" "120;16:9")
        list(GET cfg 0 fps)
        list(GET cfg 1 aspect)
        string(REPLACE ":" "" tag "${aspect}")
        set(name controls_e2e_${mode}_${fps}fps_${tag})
        add_test(NAME ${name} COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/controls_e2e.sh $<TARGET_FILE:harness>
                 $<TARGET_FILE:gromada_libretro> ${CMAKE_BINARY_DIR}/${name}_out ${mode}
                 gromada_fps=${fps} gromada_aspect=${aspect})
        set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
endforeach()
