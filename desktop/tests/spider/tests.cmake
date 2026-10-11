# Agent-owned test registration (Agent N, spider module).
ks_add_test(test_spider ${CMAKE_CURRENT_LIST_DIR}/test_spider.cpp)
target_compile_definitions(test_spider PRIVATE KS_SPIDER_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/../fixtures/spider")
ks_add_test(test_spider_rewrite ${CMAKE_CURRENT_LIST_DIR}/test_rewrite.cpp)
target_compile_definitions(test_spider_rewrite PRIVATE KS_SPIDER_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/../fixtures/spider")
ks_add_test(test_spider_live ${CMAKE_CURRENT_LIST_DIR}/test_spider_live.cpp)
target_compile_definitions(test_spider_live PRIVATE KS_TEST_OUT_DIR="${CMAKE_BINARY_DIR}")
