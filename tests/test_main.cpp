#include "test.hpp"

#include <chrono>
#include <cstdlib>

#include "cppmcp/core/log.hpp"

void test_json_rpc();
void test_versions();
void test_schema_contract();
void test_phase2_memory_roundtrip();
void test_phase2_stdio_roundtrip();
void test_http_handler();
void test_cancellation_strategy();

namespace {

template <typename Fn>
void run(const char* name, Fn fn) {
    CPPMCP_LOG_INFO("running " << name);
    const auto start = std::chrono::steady_clock::now();
    const int failures_before = cppmcp::test::failures;
    fn();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    if (cppmcp::test::failures == failures_before) {
        CPPMCP_LOG_INFO(name << " passed in " << ms << " ms");
    } else {
        CPPMCP_LOG_ERROR(name << " FAILED in " << ms << " ms");
    }
}

}  // namespace

int main() {
    // Tests log at info by default; override with CPPMCP_LOG_LEVEL=warn|debug|trace
    // and optionally CPPMCP_LOG_FILE=/path/to/file.log
    cppmcp::core::log::configure(cppmcp::core::log::Level::info);

    run("test_json_rpc", test_json_rpc);
    run("test_versions", test_versions);
    run("test_schema_contract", test_schema_contract);
    run("test_phase2_memory_roundtrip", test_phase2_memory_roundtrip);
    run("test_phase2_stdio_roundtrip", test_phase2_stdio_roundtrip);
    run("test_http_handler", test_http_handler);
    run("test_cancellation_strategy", test_cancellation_strategy);

    if (cppmcp::test::failures != 0) {
        CPPMCP_LOG_ERROR(cppmcp::test::failures << " check(s) failed");
        return EXIT_FAILURE;
    }
    CPPMCP_LOG_INFO("All cppmcp foundation tests passed.");
    return EXIT_SUCCESS;
}