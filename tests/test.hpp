#pragma once

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "cppmcp/core/log.hpp"

namespace cppmcp::test {

inline int failures = 0;

inline void check(bool condition, std::string_view expression, const char* file, int line,
                  const char* function) {
    if (!condition) {
        ::cppmcp::core::log::write(::cppmcp::core::log::Level::error, file, line, function,
                                   std::string("CHECK FAILED: ") + std::string(expression));
        ++failures;
    }
}

}  // namespace cppmcp::test

#define CPPMCP_CHECK(condition) \
    ::cppmcp::test::check(static_cast<bool>(condition), #condition, __FILE__, __LINE__, CPPMCP_FUNC)