# Lead-owned. Includes each module's test registration.
set(KS_TEST_INCLUDE ${PROJECT_SOURCE_DIR}/tests)
# Helper: ks_add_test(<name> <source...>) builds an executable linked to ks_core and registers it.
function(ks_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE ks_core)
    target_include_directories(${name} PRIVATE ${KS_TEST_INCLUDE})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "KS_FIXTURES=${PROJECT_SOURCE_DIR}/../tests/fixtures;KS_DATA_DIR=${CMAKE_BINARY_DIR}/testdata/${name}")
endfunction()
# Core sources are part of ks_core via the module lists; add them here so ks_core always has util.
include(${PROJECT_SOURCE_DIR}/tests/extract/tests.cmake)
include(${PROJECT_SOURCE_DIR}/tests/output/tests.cmake)
include(${PROJECT_SOURCE_DIR}/tests/net/tests.cmake)
include(${PROJECT_SOURCE_DIR}/tests/spider/tests.cmake)
