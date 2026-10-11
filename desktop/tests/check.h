// Tiny test helper. Each test file is one executable with main().
#pragma once
#include <cstdio>
#include <string>
namespace kstest {
inline int failures = 0;
inline int checks = 0;
}
#define CHECK(cond) do { ++kstest::checks; if (!(cond)) { ++kstest::failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++kstest::checks; auto _a = (a); auto _b = (b); if (!(_a == _b)) { ++kstest::failures; std::fprintf(stderr, "FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)
#define TEST_MAIN_END() do { std::printf("%d checks, %d failures\n", kstest::checks, kstest::failures); return kstest::failures ? 1 : 0; } while (0)
