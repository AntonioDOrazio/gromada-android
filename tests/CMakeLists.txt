# Each test: one executable, one translation unit (header-implemented modules may be
# included once per binary). Exit code 77 = skipped (no game data).
function(gromada_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE Threads::Threads)
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

# Differential oracle of the sound-effect system (issue #18): DirectSound calls of the original
# (vectors in the repo); with GROMADA_DATA also the fw.res descriptor table.
gromada_add_test(sound_oracle_test sound_oracle_test.cpp)
target_compile_definitions(sound_oracle_test PRIVATE GROMADA_ORACLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/oracle")

# Differential oracle (issue #11): reference math vs vectors from the original exe (no game data).
gromada_add_test(oracle_test oracle_test.cpp)
target_compile_definitions(oracle_test PRIVATE GROMADA_ORACLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/oracle")
# The engine's own code against the same vectors (issue #12), no game data.
gromada_add_test(engine_oracle_test engine_oracle_test.cpp)
target_compile_definitions(engine_oracle_test PRIVATE GROMADA_ORACLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/oracle")
# Simulation engine (src/game, issue #8): synthetic tests, data-backed level runs and the
# desktop driver tools/engine_run (screenshots + trace).
gromada_add_test(engine_test engine_test.cpp)
gromada_add_test(engine_data_test engine_data_test.cpp)
set_tests_properties(engine_data_test PROPERTIES TIMEOUT 1800)
# Rendering pipeline (issue #20): frame time per internal resolution / zoom, screenshots.
add_executable(render_bench ${PROJECT_SOURCE_DIR}/tools/render_bench.cpp
    ${PROJECT_SOURCE_DIR}/src/third_party/stb_vorbis_impl.c ${PROJECT_SOURCE_DIR}/src/third_party/dr_libs_impl.c)
target_include_directories(render_bench PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(render_bench PRIVATE Threads::Threads)
target_compile_options(render_bench PRIVATE -Wall -Wno-unknown-attributes -Wno-parentheses -Wno-sign-compare)
add_executable(engine_run ${PROJECT_SOURCE_DIR}/tools/engine_run.cpp)
target_include_directories(engine_run PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(engine_run PRIVATE -Wall -Wno-unknown-attributes -Wno-parentheses -Wno-sign-compare)
# Game-level trace of a demo replay (issue #13), compared with the emulated original by
# tools/oracle/compare_traces.py (docs/trace-oracle.md).
add_executable(game_trace ${PROJECT_SOURCE_DIR}/tools/game_trace.cpp
    ${PROJECT_SOURCE_DIR}/src/third_party/stb_vorbis_impl.c ${PROJECT_SOURCE_DIR}/src/third_party/dr_libs_impl.c)
target_include_directories(game_trace PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(game_trace PRIVATE Threads::Threads)
target_compile_options(game_trace PRIVATE -Wall -Wno-unknown-attributes -Wno-parentheses -Wno-sign-compare)
# The first 1500 frames of each demo against the emulated original (needs GROMADA_DATA and
# ORACLE_PYTHON, skipped otherwise; whole demos: tests/trace_oracle.sh ... 0).
add_test(NAME trace_oracle COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/trace_oracle.sh $<TARGET_FILE:game_trace>
         ${CMAKE_BINARY_DIR}/trace_oracle_out 1500)
set_tests_properties(trace_oracle PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
# The same with the rendering pipeline on (issue #20): drawn at internal resolution 2, zoom 2,
# smooth motion. Presentation only: the trace must not change.
add_test(NAME trace_oracle_presentation COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/trace_oracle.sh $<TARGET_FILE:game_trace>
         ${CMAKE_BINARY_DIR}/trace_oracle_presentation_out 1500 --present 2,2)
set_tests_properties(trace_oracle_presentation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
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

# Visual fidelity (issue #15): 16-bit colour tables, Effect(), gamma and putcadr against
# vectors from the original exe (tests/data/oracle/hicolor_*.json; no game data needed).
gromada_add_test(hicolor_test hicolor_test.cpp)
target_compile_definitions(hicolor_test PRIVATE GROMADA_ORACLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/oracle")
# Rendering pipeline (issue #20): scaled putcadr, drawView, view origin, smooth motion, input
# mapping through the zoom, internal-resolution composite (synthetic, no game data).
gromada_add_test(render_view_test render_view_test.cpp)
target_compile_definitions(render_view_test PRIVATE GROMADA_ORACLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/oracle")
# With the game data: every combination of internal resolution, zoom, aspect and frame rate on
# a level (picture, mapping, cursor), draw() == drawView() on real levels, save states with zoom.
gromada_add_test(render_data_test render_data_test.cpp
    ${PROJECT_SOURCE_DIR}/src/third_party/stb_vorbis_impl.c
    ${PROJECT_SOURCE_DIR}/src/third_party/dr_libs_impl.c)
set_tests_properties(render_data_test PROPERTIES TIMEOUT 1800)
# The core's output at 1x, zoom 1, smooth motion off is the one of main before the pipeline
# (frame hashes recorded with bedb79d).
add_test(NAME render_e2e COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/render_e2e.sh $<TARGET_FILE:harness>
         $<TARGET_FILE:gromada_libretro> ${CMAKE_BINARY_DIR}/render_e2e_out)
set_tests_properties(render_e2e PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
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
# Save states (issue #14): synthetic tests (format, engine, VM, mixer, mapper) and, with the
# game data, determinism of real levels, run-ahead, demos and corrupted states; plus the core
# end to end through the harness (tests/savestate_e2e.sh).
gromada_add_test(savestate_test savestate_test.cpp
    ${PROJECT_SOURCE_DIR}/src/third_party/stb_vorbis_impl.c
    ${PROJECT_SOURCE_DIR}/src/third_party/dr_libs_impl.c)
gromada_add_test(savestate_data_test savestate_data_test.cpp
    ${PROJECT_SOURCE_DIR}/src/third_party/stb_vorbis_impl.c
    ${PROJECT_SOURCE_DIR}/src/third_party/dr_libs_impl.c)
set_tests_properties(savestate_data_test PROPERTIES TIMEOUT 1800)
add_test(NAME savestate_e2e COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/savestate_e2e.sh $<TARGET_FILE:harness>
         $<TARGET_FILE:gromada_libretro> ${CMAKE_BINARY_DIR}/savestate_e2e_out)
add_test(NAME savestate_e2e_169_120fps COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/savestate_e2e.sh $<TARGET_FILE:harness>
         $<TARGET_FILE:gromada_libretro> ${CMAKE_BINARY_DIR}/savestate_e2e_169_out gromada_aspect=16:9 gromada_fps=120)
set_tests_properties(savestate_e2e savestate_e2e_169_120fps PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
