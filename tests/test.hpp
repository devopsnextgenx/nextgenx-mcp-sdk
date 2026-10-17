#pragma once

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace cppmcp::test {

inline int failures = 0;

inline void check(bool condition, std::string_view expression, int line) {
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

}  // namespace cppmcp::test

#define CPPMCP_CHECK(condition) \
    ::cppmcp::test::check(static_cast<bool>(condition), #condition, __LINE__)
