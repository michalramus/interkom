#pragma once

#include <cstdio>
#include <cstdlib>

// assert() that always runs, NDEBUG or not, so checks may have side effects.
#define CHECK(condition) ((condition) ? void(0) : check_failed(#condition, __FILE__, __LINE__))

[[noreturn]] inline void check_failed(const char* condition, const char* file, int line) {
    std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", file, line, condition);
    std::abort();
}
