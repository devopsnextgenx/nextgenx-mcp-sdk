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
void test_http_server_sse_framing();
void test_http_server_response_modes();
void test_http_server_sse_keepalive();
void test_http_server_slow_consumer();
void test_http_server_disconnect_cancel();
void test_http_server_sse_soak();
void test_http_server_mcp_param_stateless();
void test_http_handler_header_params_stateless();
void test_http_handler_header_params_primitive_types();
void test_http_server_allow_header();
void test_http_server_unknown_route();
void test_http_server_duplicate_host();
void test_http_server_duplicate_origin();
void test_http_server_invalid_origin();
void test_http_server_bearer_auth();
void test_http_server_bearer_reject();
void test_http_server_missing_param_header();
void test_http_server_duplicate_param_header();
void test_http_server_unexpected_param_header();
void test_http_server_optional_param_header();
void test_resumability_reconnect();
void test_resumability_retry();
void test_session_tracking();
void test_session_revocation();
void test_session_identity();
void test_dual_mode_legacy();
void test_dual_mode_modern();
void test_dual_mode_header_isolation();
void test_dual_mode_runtime();
void test_dual_mode_server_compat();
void test_ttl_cache_scope();
void test_ttl_server_window();
void test_ttl_probe();
void test_ttl_freshness();
void test_expiry_reconnect();
void test_expiry_cleanup();
void test_expiry_session_timer();
void test_expiry_legacy_timeout();
void test_expiry_lifecycle();
void test_expiry_guard();
void test_sse_parser();
void test_www_auth();
void test_listen_registry();

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
    run_selected("http_handler_mcp_param_stateless", test_http_handler_header_params_stateless);
    run_selected("http_handler_mcp_param_types",
                 test_http_handler_header_params_primitive_types);
    run_selected("cancellation", test_cancellation_strategy);
    run_selected("http_server_methods", test_http_server_methods);
    run_selected("http_server_host_origin", test_http_server_host_origin);
    run_selected("http_server_smuggling", test_http_server_smuggling);
    run_selected("http_server_slowloris", test_http_server_slowloris);
    run_selected("http_server_auth_route_bind", test_http_server_auth_route_bind);
    run_selected("http_server_allow_header", test_http_server_allow_header);
    run_selected("http_server_unknown_route", test_http_server_unknown_route);
    run_selected("http_server_duplicate_host", test_http_server_duplicate_host);
    run_selected("http_server_duplicate_origin", test_http_server_duplicate_origin);
    run_selected("http_server_invalid_origin", test_http_server_invalid_origin);
    run_selected("http_server_bearer_auth", test_http_server_bearer_auth);
    run_selected("http_server_bearer_reject", test_http_server_bearer_reject);
    run_selected("http_server_missing_param_header", test_http_server_missing_param_header);
    run_selected("http_server_duplicate_param_header", test_http_server_duplicate_param_header);
    run_selected("http_server_unexpected_param_header",
                 test_http_server_unexpected_param_header);
    run_selected("http_server_optional_param_header", test_http_server_optional_param_header);
    run_selected("http_server_sse_framing", test_http_server_sse_framing);
    run_selected("http_server_response_modes", test_http_server_response_modes);
    run_selected("http_server_sse_keepalive", test_http_server_sse_keepalive);
    run_selected("http_server_slow_consumer", test_http_server_slow_consumer);
    run_selected("http_server_disconnect_cancel", test_http_server_disconnect_cancel);
    run_selected("http_server_sse_soak", test_http_server_sse_soak);
    run_selected("http_server_mcp_param_stateless", test_http_server_mcp_param_stateless);
    run_selected("resumability_reconnect", test_resumability_reconnect);
    run_selected("resumability_retry", test_resumability_retry);
    run_selected("session_tracking", test_session_tracking);
    run_selected("session_revocation", test_session_revocation);
    run_selected("session_identity", test_session_identity);
    run_selected("dual_mode_legacy", test_dual_mode_legacy);
    run_selected("dual_mode_modern", test_dual_mode_modern);
    run_selected("dual_mode_header_isolation", test_dual_mode_header_isolation);
    run_selected("dual_mode_runtime", test_dual_mode_runtime);
    run_selected("dual_mode_server_compat", test_dual_mode_server_compat);
    run_selected("ttl_cache_scope", test_ttl_cache_scope);
    run_selected("ttl_server_window", test_ttl_server_window);
    run_selected("ttl_probe", test_ttl_probe);
    run_selected("ttl_freshness", test_ttl_freshness);
    run_selected("expiry_reconnect", test_expiry_reconnect);
    run_selected("expiry_cleanup", test_expiry_cleanup);
    run_selected("expiry_session_timer", test_expiry_session_timer);
    run_selected("expiry_legacy_timeout", test_expiry_legacy_timeout);
    run_selected("expiry_lifecycle", test_expiry_lifecycle);
    run_selected("expiry_guard", test_expiry_guard);
    run_selected("sse_parser", test_sse_parser);
    run_selected("www_auth", test_www_auth);
    run_selected("listen", test_listen_registry);
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