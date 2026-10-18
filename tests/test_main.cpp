#include "test.hpp"

#include <chrono>
#include <cstdlib>
#include <string>

#include "cppmcp/core/log.hpp"

void test_json_rpc();
void test_versions();
void test_schema_contract();
void test_phase2_memory_roundtrip();
void test_phase2_stdio_roundtrip();
void test_http_handler();
void test_cancellation_strategy();
void test_http_server_methods();
void test_http_server_host_origin();
void test_http_server_smuggling();
void test_http_server_slowloris();
void test_http_server_auth_route_bind();

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

int main(int argc, char** argv) {
    // Tests log at info by default; override with CPPMCP_LOG_LEVEL=warn|debug|trace
    // and optionally CPPMCP_LOG_FILE=/path/to/file.log
    cppmcp::core::log::configure(cppmcp::core::log::Level::info);

    if (argc > 2) {
        CPPMCP_LOG_ERROR("usage: cppmcp_core_tests [case-name]");
        return EXIT_FAILURE;
    }
    const std::string selected = argc == 2 ? argv[1] : "";
    bool found = selected.empty();
    const auto run_selected = [&](const char* name, auto test) {
        if (selected.empty() || selected == name) {
            found = true;
            run(name, test);
        }
    };
    run_selected("json_rpc", test_json_rpc);
    run_selected("versions", test_versions);
    run_selected("schema_contract", test_schema_contract);
    run_selected("phase2_memory", test_phase2_memory_roundtrip);
    run_selected("phase2_stdio", test_phase2_stdio_roundtrip);
    run_selected("http_handler", test_http_handler);
    run_selected("cancellation", test_cancellation_strategy);
    run_selected("http_server_methods", test_http_server_methods);
    run_selected("http_server_host_origin", test_http_server_host_origin);
    run_selected("http_server_smuggling", test_http_server_smuggling);
    run_selected("http_server_slowloris", test_http_server_slowloris);
    run_selected("http_server_auth_route_bind", test_http_server_auth_route_bind);
    if (!found) {
        CPPMCP_LOG_ERROR("unknown test case: " << selected);
        return EXIT_FAILURE;
    }

    if (cppmcp::test::failures != 0) {
        CPPMCP_LOG_ERROR(cppmcp::test::failures << " check(s) failed");
        return EXIT_FAILURE;
    }
    CPPMCP_LOG_INFO("All cppmcp foundation tests passed.");
    return EXIT_SUCCESS;
}