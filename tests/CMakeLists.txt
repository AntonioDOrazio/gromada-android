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
