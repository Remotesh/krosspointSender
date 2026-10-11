# Agent-owned test registration (Agent F, output module).
ks_add_test(test_output ${CMAKE_CURRENT_LIST_DIR}/test_output.cpp)
target_compile_definitions(test_output PRIVATE KS_TEST_OUT_DIR="${CMAKE_BINARY_DIR}")
# Agent N: spider pack EPUB builder and the dl/ext fixes.
ks_add_test(test_pack_epub ${CMAKE_CURRENT_LIST_DIR}/test_pack_epub.cpp)
target_compile_definitions(test_pack_epub PRIVATE KS_TEST_OUT_DIR="${CMAKE_BINARY_DIR}")
