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

# Script interpreter (src/script): unit tests, game-data test, desktop tool tools/script_run.
gromada_add_test(script_test script_test.cpp)
gromada_add_test(script_data_test script_data_test.cpp)
add_executable(script_run ${PROJECT_SOURCE_DIR}/tools/script_run.cpp)
target_include_directories(script_run PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME script_run_demo
         COMMAND script_run --root ${CMAKE_CURRENT_SOURCE_DIR}/data/script --ticks 3 --trace demo.ini)
set_tests_properties(script_run_demo PROPERTIES PASS_REGULAR_EXPRESSION
    "\\[1\\] > main\\(\\)\n\\[1\\] Notify\\(100, \"begin\"\\)\n\\[2\\] > main\\(\\)\n\\[3\\] > main\\(\\)\n\\[3\\] > Report\\(20\\)\n\\[3\\] Notify\\(20, \"unit2\"\\)")
