cmake_minimum_required(VERSION 3.24)

project(cppmcp VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(CPPMCP_BUILD_SHARED "Build the shared library" ON)
option(CPPMCP_BUILD_STATIC "Build the static library" ON)
option(CPPMCP_BUILD_TESTS "Build unit tests" ON)
option(CPPMCP_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(CPPMCP_BUILD_EXAMPLES "Build the stdio and HTTP examples" ON)
option(CPPMCP_ENABLE_LEGACY "Enable legacy protocol support" ON)
option(CPPMCP_OFFLINE "Do not fetch dependencies from the network" OFF)
find_package(OpenSSL 3 QUIET)
if(NOT TARGET OpenSSL::Crypto)
    message(FATAL_ERROR "OpenSSL 3 Crypto is required for authenticated requestState support")
endif()
if(OpenSSL_FOUND)
    set(CPPMCP_TLS_DEFAULT ON)
else()
    set(CPPMCP_TLS_DEFAULT OFF)
endif()
option(CPPMCP_WITH_TLS "Build HTTP TLS support when OpenSSL 3 is available"
    ${CPPMCP_TLS_DEFAULT})
if(CPPMCP_WITH_TLS AND NOT OpenSSL_FOUND)
    message(FATAL_ERROR "CPPMCP_WITH_TLS requires OpenSSL 3")
endif()

if(NOT CPPMCP_BUILD_SHARED AND NOT CPPMCP_BUILD_STATIC)
    message(FATAL_ERROR "At least one of CPPMCP_BUILD_SHARED or CPPMCP_BUILD_STATIC must be enabled")
endif()

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
set(FETCHCONTENT_FULLY_DISCONNECTED "${CPPMCP_OFFLINE}")
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

set(JSONCONS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    jsoncons
    URL https://github.com/danielaparker/jsoncons/archive/refs/tags/v1.10.0.tar.gz
    URL_HASH SHA256=c829350a2eece662beb378143e2ab1c1cbb07d703687bf168626665a8a7d8bc1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(jsoncons)

set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    re2
    URL https://github.com/google/re2/archive/refs/tags/2023-03-01.tar.gz
    URL_HASH SHA256=7a9a4824958586980926a300b4717202485c4b4115ac031822e29aa4ef207e48
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(re2)
set_target_properties(re2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
set(JSONCONS_SOURCE_DIR "${jsoncons_SOURCE_DIR}")
include(cmake/patch_jsoncons_re2.cmake)

FetchContent_Declare(
    asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz
    URL_HASH SHA256=755bd7f85a4b269c67ae0ea254907c078d408cce8e1a352ad2ed664d233780e8
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(asio)

FetchContent_Declare(
    llhttp
    URL https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.2.1.tar.gz
    URL_HASH SHA256=3c163891446e529604b590f9ad097b2e98b5ef7e4d3ddcf1cf98b62ca668f23e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR cppmcp-source-only
)
FetchContent_MakeAvailable(llhttp)

set(CPPMCP_SOURCES
    src/auth.cpp
    src/json_rpc.cpp
    src/json_schema.cpp
    src/request_state.cpp
    src/uri_template.cpp
    src/version.cpp
    src/connection.cpp
)

# GCC's optimized libstdc++ regex implementation can trigger a false-positive
# maybe-uninitialized diagnostic in sanitizer builds. Keep strict warnings and
# avoid the diagnostic by compiling only this translation unit without O2.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_FLAGS MATCHES "-fsanitize=address")
    set_source_files_properties(src/json_schema.cpp PROPERTIES COMPILE_OPTIONS "-O0")
endif()

set(CPPMCP_SERVER_SOURCES
    src/server.cpp
    src/subscription_registry.cpp
)

set(CPPMCP_CLIENT_SOURCES
    src/client.cpp
)

set(CPPMCP_TRANSPORT_STDIO_SOURCES
    src/stdio.cpp
)

set(CPPMCP_TRANSPORT_HTTP_SOURCES
    src/network_policy.cpp
    src/jwt_verifier.cpp
    src/introspection_verifier.cpp
    src/oauth_metadata.cpp
    src/oauth_provider.cpp
    src/http_handler.cpp
    src/http_client.cpp
    src/http_server.cpp
    src/sse_parser.cpp
)

function(cppmcp_configure_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

function(cppmcp_configure_library target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${nlohmann_json_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${jsoncons_SOURCE_DIR}/include
    )
    if(CPPMCP_ENABLE_LEGACY)
        target_compile_definitions(${target} PUBLIC CPPMCP_ENABLE_LEGACY=1)
    else()
        target_compile_definitions(${target} PUBLIC CPPMCP_ENABLE_LEGACY=0)
    endif()
    set_target_properties(${target} PROPERTIES
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
    )
    cppmcp_configure_warnings(${target})
endfunction()

if(CPPMCP_BUILD_STATIC)
    add_library(cppmcp_llhttp OBJECT
        ${llhttp_SOURCE_DIR}/src/llhttp.c
        ${llhttp_SOURCE_DIR}/src/http.c
        ${llhttp_SOURCE_DIR}/src/api.c
    )
    target_include_directories(cppmcp_llhttp SYSTEM PRIVATE ${llhttp_SOURCE_DIR}/include)
    set_target_properties(cppmcp_llhttp PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

    add_library(cppmcp_core STATIC ${CPPMCP_SOURCES})
    add_library(cppmcp::core ALIAS cppmcp_core)
    cppmcp_configure_library(cppmcp_core)
    target_link_libraries(cppmcp_core PUBLIC OpenSSL::Crypto PRIVATE re2::re2)
    set_target_properties(cppmcp_core PROPERTIES EXPORT_NAME core)

    add_library(cppmcp_server STATIC ${CPPMCP_SERVER_SOURCES})
    add_library(cppmcp::server ALIAS cppmcp_server)
    cppmcp_configure_library(cppmcp_server)
    set_target_properties(cppmcp_server PROPERTIES EXPORT_NAME server)
    target_link_libraries(cppmcp_server PUBLIC cppmcp_core)

    add_library(cppmcp_client STATIC ${CPPMCP_CLIENT_SOURCES})
    add_library(cppmcp::client ALIAS cppmcp_client)
    cppmcp_configure_library(cppmcp_client)
    set_target_properties(cppmcp_client PROPERTIES EXPORT_NAME client)
    target_link_libraries(cppmcp_client PUBLIC cppmcp_core cppmcp_server)

    add_library(cppmcp_transport_stdio STATIC ${CPPMCP_TRANSPORT_STDIO_SOURCES})
    add_library(cppmcp::transport_stdio ALIAS cppmcp_transport_stdio)
    cppmcp_configure_library(cppmcp_transport_stdio)
    set_target_properties(cppmcp_transport_stdio PROPERTIES EXPORT_NAME transport_stdio)
    target_link_libraries(cppmcp_transport_stdio PUBLIC cppmcp_core)

    add_library(cppmcp_transport_http STATIC ${CPPMCP_TRANSPORT_HTTP_SOURCES})
    add_library(cppmcp::transport_http ALIAS cppmcp_transport_http)
    cppmcp_configure_library(cppmcp_transport_http)
    set_target_properties(cppmcp_transport_http PROPERTIES EXPORT_NAME transport_http)
    target_link_libraries(cppmcp_transport_http PUBLIC cppmcp_server cppmcp_client cppmcp_core)
    if(CPPMCP_WITH_TLS)
        target_compile_definitions(cppmcp_transport_http PUBLIC CPPMCP_WITH_TLS=1)
        target_link_libraries(cppmcp_transport_http PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    else()
        target_compile_definitions(cppmcp_transport_http PUBLIC CPPMCP_WITH_TLS=0)
    endif()
    target_sources(cppmcp_transport_http PRIVATE $<TARGET_OBJECTS:cppmcp_llhttp>)
    target_include_directories(cppmcp_transport_http SYSTEM PRIVATE
        ${llhttp_SOURCE_DIR}/include
    )
    target_include_directories(cppmcp_transport_http SYSTEM PRIVATE
        ${asio_SOURCE_DIR}/asio/include
    )
    # GCC reports libstdc++'s Asio TSan fences as errors despite instrumenting project code.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_FLAGS MATCHES "-fsanitize=thread")
        target_compile_options(cppmcp_transport_http PRIVATE -Wno-error=tsan)
    endif()
endif()

if(CPPMCP_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "CPPMCP_BUILD_FUZZERS requires Clang with libFuzzer")
    endif()
    if(NOT CPPMCP_BUILD_STATIC)
        message(FATAL_ERROR "CPPMCP_BUILD_FUZZERS requires CPPMCP_BUILD_STATIC=ON")
    endif()

    target_compile_options(cppmcp_llhttp PRIVATE
        -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer
    )
    foreach(fuzz_library IN ITEMS cppmcp_core cppmcp_server cppmcp_client
            cppmcp_transport_stdio cppmcp_transport_http)
        target_compile_options(${fuzz_library} PRIVATE
            -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
        target_link_options(${fuzz_library} INTERFACE
            "$<BUILD_INTERFACE:-fsanitize=address,undefined>")
    endforeach()

    function(cppmcp_add_fuzzer target source)
        add_executable(${target} ${source})
        target_compile_features(${target} PRIVATE cxx_std_20)
        target_compile_options(${target} PRIVATE
            -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
        cppmcp_configure_warnings(${target})
    endfunction()

    cppmcp_add_fuzzer(cppmcp_fuzz_mirrored_header fuzz/mirrored_header_fuzz.cpp)
    target_link_libraries(cppmcp_fuzz_mirrored_header PRIVATE cppmcp::transport_http)

    cppmcp_add_fuzzer(cppmcp_fuzz_mcp_name_param fuzz/mcp_name_param_fuzz.cpp)
    target_link_libraries(cppmcp_fuzz_mcp_name_param PRIVATE cppmcp::transport_http)

    cppmcp_add_fuzzer(cppmcp_fuzz_http_request fuzz/http_request_fuzz.cpp)
    target_include_directories(cppmcp_fuzz_http_request SYSTEM PRIVATE
        ${llhttp_SOURCE_DIR}/include
    )
    target_link_libraries(cppmcp_fuzz_http_request PRIVATE cppmcp::transport_http)

    cppmcp_add_fuzzer(cppmcp_fuzz_sse_parser fuzz/sse_parser_fuzz.cpp)
    target_link_libraries(cppmcp_fuzz_sse_parser PRIVATE cppmcp::transport_http)

    cppmcp_add_fuzzer(cppmcp_fuzz_www_authenticate fuzz/www_auth_fuzz.cpp)
    target_link_libraries(cppmcp_fuzz_www_authenticate PRIVATE cppmcp::transport_http)

    cppmcp_add_fuzzer(cppmcp_fuzz_json_schema fuzz/json_schema_fuzz.cpp)
    target_link_libraries(cppmcp_fuzz_json_schema PRIVATE cppmcp::core)

    cppmcp_add_fuzzer(cppmcp_fuzz_request_state fuzz/request_state_fuzz.cpp)
    target_link_libraries(cppmcp_fuzz_request_state PRIVATE cppmcp::core)

    cppmcp_add_fuzzer(cppmcp_fuzz_uri_template fuzz/uri_template_fuzz.cpp)
    target_include_directories(cppmcp_fuzz_uri_template PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/common)
    target_link_libraries(cppmcp_fuzz_uri_template PRIVATE cppmcp::server)

    cppmcp_add_fuzzer(cppmcp_fuzz_pagination_cursor fuzz/pagination_cursor_fuzz.cpp)
    target_include_directories(cppmcp_fuzz_pagination_cursor PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/common)
    target_link_libraries(cppmcp_fuzz_pagination_cursor PRIVATE cppmcp::server)

    cppmcp_add_fuzzer(cppmcp_fuzz_client_cache fuzz/client_cache_fuzz.cpp)
    target_link_libraries(cppmcp_fuzz_client_cache PRIVATE cppmcp::client)

    cppmcp_add_fuzzer(cppmcp_fuzz_file_sandbox fuzz/file_sandbox_fuzz.cpp)
    target_include_directories(cppmcp_fuzz_file_sandbox PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/common)
    target_link_libraries(cppmcp_fuzz_file_sandbox PRIVATE cppmcp::server)

    cppmcp_add_fuzzer(cppmcp_fuzz_subscription_filter fuzz/subscription_filter_fuzz.cpp)
    target_link_libraries(cppmcp_fuzz_subscription_filter PRIVATE cppmcp::transport_http)

    cppmcp_add_fuzzer(cppmcp_fuzz_jwt_parser fuzz/jwt_parser_fuzz.cpp)
    target_link_libraries(cppmcp_fuzz_jwt_parser PRIVATE cppmcp::transport_http)

    cppmcp_add_fuzzer(cppmcp_fuzz_oauth_metadata fuzz/oauth_metadata_fuzz.cpp)
    target_link_libraries(cppmcp_fuzz_oauth_metadata PRIVATE cppmcp::transport_http)

    cppmcp_add_fuzzer(cppmcp_fuzz_scope_set fuzz/scope_set_fuzz.cpp)
    target_link_libraries(cppmcp_fuzz_scope_set PRIVATE cppmcp::core)
endif()

if(CPPMCP_BUILD_SHARED)
    add_library(cppmcp_core_shared SHARED ${CPPMCP_SOURCES})
    add_library(cppmcp::core_shared ALIAS cppmcp_core_shared)
    cppmcp_configure_library(cppmcp_core_shared)
    target_link_libraries(cppmcp_core_shared PUBLIC OpenSSL::Crypto PRIVATE re2::re2)
    set_target_properties(cppmcp_core_shared PROPERTIES
        EXPORT_NAME core_shared
        OUTPUT_NAME cppmcp_core_shared
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${nlohmann_json_SOURCE_DIR}/include/nlohmann
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

set(CPPMCP_INSTALL_TARGETS)
if(CPPMCP_BUILD_STATIC)
    list(APPEND CPPMCP_INSTALL_TARGETS cppmcp_core cppmcp_server cppmcp_client cppmcp_transport_stdio cppmcp_transport_http)
endif()
if(CPPMCP_BUILD_SHARED)
    list(APPEND CPPMCP_INSTALL_TARGETS cppmcp_core_shared)
endif()

install(TARGETS ${CPPMCP_INSTALL_TARGETS}
    EXPORT cppmcpTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(EXPORT cppmcpTargets
    FILE cppmcpTargets.cmake
    NAMESPACE cppmcp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppmcp
)

configure_package_config_file(
    cmake/cppmcpConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cppmcpConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppmcp
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cppmcpConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/cppmcpConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/cppmcpConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppmcp
)
install(FILES "${re2_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/cppmcp"
    RENAME RE2-LICENSE
)

# ---------------------------------------------------------------------------
# Examples
#
#   examples/common/  shared headers (phase-4 tools, FeatureRegistry, client helpers, tours)
#   examples/stdio/   server.cpp, client.cpp
#   examples/http/    server.cpp, client.cpp, front_door.hpp, augmenting_exchange.hpp
#
# The example sources include their shared headers by relative path ("../common/...") and use
# std::jthread, so they only need Threads in addition to the cppmcp targets. The nlohmann_json
# include directory reaches them through the cppmcp library targets' PUBLIC include paths.
# examples/extras/ holds unbuilt reference material and is intentionally not listed here.
# ---------------------------------------------------------------------------
if(CPPMCP_BUILD_EXAMPLES)
    if(NOT CPPMCP_BUILD_STATIC)
        message(FATAL_ERROR "CPPMCP_BUILD_EXAMPLES requires CPPMCP_BUILD_STATIC=ON "
                            "(the server/client/transport libraries are static-only)")
    endif()
    find_package(Threads REQUIRED)

    function(cppmcp_add_example target source)
        add_executable(${target} ${source})
        target_compile_features(${target} PRIVATE cxx_std_20)
        set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
        target_link_libraries(${target} PRIVATE ${ARGN} Threads::Threads)
        cppmcp_configure_warnings(${target})
    endfunction()

    cppmcp_add_example(cppmcp_stdio_server examples/stdio/server.cpp
        cppmcp::server cppmcp::transport_stdio)
    cppmcp_add_example(cppmcp_stdio_client examples/stdio/client.cpp
        cppmcp::client cppmcp::transport_stdio)

    cppmcp_add_example(cppmcp_conformance_server examples/conformance_server.cpp
        cppmcp::server cppmcp::transport_http)

    # The HTTP example's front door (examples/http/front_door.hpp) uses POSIX sockets.
    if(NOT WIN32)
        cppmcp_add_example(cppmcp_http_server examples/http/server.cpp
            cppmcp::server cppmcp::transport_http)
        cppmcp_add_example(cppmcp_http_client examples/http/client.cpp
            cppmcp::client cppmcp::transport_http)
    else()
        message(STATUS "cppmcp: skipping HTTP examples (POSIX-only front door)")
    endif()
endif()

if(CPPMCP_BUILD_TESTS)
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    configure_file(tests/test_authserver.py
                   "${CMAKE_CURRENT_BINARY_DIR}/test_authserver" COPYONLY)
    file(CHMOD "${CMAKE_CURRENT_BINARY_DIR}/test_authserver"
         PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                    GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    FetchContent_Declare(
        json_schema_suite
        URL https://codeload.github.com/json-schema-org/JSON-Schema-Test-Suite/tar.gz/89e83a74014617ffbe79a56d83d14d054cd11417
        URL_HASH SHA256=70e238f62ee2d5143e9b292109dbf054f5055d31111ae20f75af754807c5323a
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(json_schema_suite)
    add_executable(cppmcp_json_schema_suite_tests tests/json_schema_suite_test.cpp)
    target_compile_features(cppmcp_json_schema_suite_tests PRIVATE cxx_std_20)
    cppmcp_configure_warnings(cppmcp_json_schema_suite_tests)
    if(CPPMCP_BUILD_STATIC)
        target_link_libraries(cppmcp_json_schema_suite_tests PRIVATE cppmcp::core)
    else()
        target_link_libraries(cppmcp_json_schema_suite_tests PRIVATE cppmcp::core_shared)
    endif()
    file(GLOB schema_suite_files CONFIGURE_DEPENDS
        "${json_schema_suite_SOURCE_DIR}/tests/draft2020-12/*.json")
    foreach(suite_file IN LISTS schema_suite_files)
        get_filename_component(suite_name "${suite_file}" NAME_WE)
        add_test(NAME cppmcp_json_schema_suite_${suite_name}
            COMMAND cppmcp_json_schema_suite_tests "${suite_file}")
        set_tests_properties(cppmcp_json_schema_suite_${suite_name}
            PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    endforeach()
    add_executable(cppmcp_core_tests
        tests/test_main.cpp
        tests/json_rpc_test.cpp
        tests/schema_validation_test.cpp
        tests/mrtr_test.cpp
        tests/version_test.cpp
        tests/schema_contract_test.cpp
        tests/phase2_test.cpp
        tests/http_handler_test.cpp
        tests/http_server_test.cpp
        tests/cancellation_test.cpp
        tests/sse_parser_test.cpp
        tests/www_auth_test.cpp
        tests/auth_phase6_test.cpp
        tests/subscription_registry_test.cpp
        tests/phase3d_test.cpp
        tests/phase5_test.cpp
    )
    if(CPPMCP_BUILD_STATIC)
        target_link_libraries(cppmcp_core_tests PRIVATE cppmcp::core cppmcp::server cppmcp::client cppmcp::transport_stdio cppmcp::transport_http)
    else()
        target_link_libraries(cppmcp_core_tests PRIVATE cppmcp::core_shared)
    endif()
    target_compile_features(cppmcp_core_tests PRIVATE cxx_std_20)
    cppmcp_configure_warnings(cppmcp_core_tests)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_FLAGS MATCHES "-fsanitize=thread")
        target_compile_options(cppmcp_core_tests PRIVATE -Wno-error=tsan)
    endif()
    target_include_directories(cppmcp_core_tests SYSTEM PRIVATE
        ${asio_SOURCE_DIR}/asio/include
    )
    target_include_directories(cppmcp_core_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/common
    )
    target_compile_definitions(cppmcp_core_tests PRIVATE
        CPPMCP_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/schema/2026-07-28/schema.json"
        CPPMCP_URI_TEMPLATE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/uritemplate-test"
    )
        foreach(test_name IN ITEMS
            json_rpc versions schema_contract schema_register tool_name_rule regex_redos_budget
            schema_limits output_schema
            schema_regex schema_references
            mrtr elicit request_state two_instance
            mrtr_client elicit_url mrtr_generic
            legacy_adapter
            http_progress_live
            http_legacy_elicit
            stdio_legacy_elicit
            request_state_subject
            progress logging
            phase2_memory phase2_stdio
            client_prompts client_resources client_completion client_cache client_resubscribe
            http_handler prm_handler_auth scope_hierarchy scope_single_challenge
            http_handler_mcp_param_stateless http_handler_mcp_param_types
            cancellation
            http_server_methods http_server_host_origin
            http_server_smuggling http_server_slowloris http_server_auth_route_bind
            http_server_allow_header http_server_unknown_route http_server_duplicate_host
            http_server_duplicate_origin http_server_invalid_origin
            http_server_bearer_auth http_server_bearer_reject
            http_server_missing_param_header http_server_duplicate_param_header
            http_server_unexpected_param_header http_server_optional_param_header
            http_server_sse_framing http_server_response_modes http_server_sse_keepalive
            http_server_slow_consumer http_server_disconnect_cancel http_server_sse_soak
            http_server_mcp_param_stateless
            resumability_reconnect resumability_retry session_tracking session_revocation
            session_identity dual_mode_legacy dual_mode_modern dual_mode_header_isolation
            dual_mode_runtime dual_mode_server_compat ttl_cache_scope ttl_server_window
            ttl_probe ttl_freshness expiry_reconnect expiry_cleanup expiry_session_timer
            expiry_legacy_timeout expiry_lifecycle expiry_guard sse_parser www_auth listen
            client_mrtr_prompt client_mrtr_resource
            sse_parser_cr_endings sse_parser_field_semantics sse_parser_id_persistence
            sse_parser_retry_digits sse_parser_truncated_dispatch sse_parser_size_limit
            www_auth_multi_challenge www_auth_quoted_escapes www_auth_fields
            listen_tools_filter listen_prompts_filter listen_resources_filter
            listen_resource_uri listen_fanout listen_overflow listen_global_limit
            listen_peer_limit listen_cleanup listen_shutdown listen_1000_subscribers
            listen_no_unrequested_types backoff_base backoff_cap backoff_jitter
            tls_min_version tls_missing_certificate hostname tls_expired_certificate
            tls_truncated_stream www_auth_listener_retry)
        add_test(NAME cppmcp_${test_name}_test COMMAND cppmcp_core_tests ${test_name})
    endforeach()
    foreach(test_name IN ITEMS
            prm_metadata_00 prm_metadata_01 prm_metadata_02 prm_metadata_03
            jwt_parser_00 jwt_parser_01 jwt_parser_02 jwt_parser_03 jwt_parser_04
            jwt_parser_05 jwt_parser_06 jwt_parser_07 jwt_parser_08 jwt_parser_09
            jwt_parser_10 jwt_parser_11 jwt_parser_12 jwt_parser_13 jwt_parser_14
            jwks_policy_00 jwks_policy_01 jwks_policy_02 jwks_policy_03
            introspection_config_00 introspection_config_01 introspection_config_02
            introspection_config_03 introspection_config_04
            scope_hierarchy_00 scope_hierarchy_01 scope_hierarchy_02 scope_hierarchy_03
            scope_single_challenge_00 scope_single_challenge_01
            scope_parser_00 scope_parser_01
            session_binding_00 session_binding_01 session_binding_02
            www_authenticate_00 www_authenticate_01 www_authenticate_02
            www_authenticate_03 www_authenticate_04
            oauth_discovery_00 oauth_discovery_01 oauth_discovery_02 oauth_discovery_03
            oauth_pkce_00 oauth_pkce_01 oauth_pkce_02
            oauth_iss_00 oauth_iss_01 oauth_iss_02 oauth_iss_03
            oauth_stepup_00 oauth_stepup_01 oauth_stepup_02
            oauth_refresh_00 oauth_refresh_01 oauth_refresh_02
            oauth_single_flight_00 oauth_single_flight_01
            oauth_store_issuer_00 oauth_store_issuer_01 oauth_store_issuer_02
            oauth_store_issuer_03
            oauth_ssrf_00 oauth_ssrf_01 oauth_ssrf_02 oauth_ssrf_03 oauth_ssrf_04
            token_passthrough_00 token_passthrough_01 token_redaction_00)
        add_test(NAME cppmcp_${test_name}_test COMMAND cppmcp_core_tests ${test_name})
    endforeach()
    foreach(test_name IN ITEMS
            uri_template template_match cursor file_sandbox symlink completion cache_hint
            registry_churn subscription_limit list_changed mrtr_prompt mrtr_resource
            template_ambiguous template_roundtrip cursor_stability tool_result_spoof
            uri_template_vectors server_builders
            mcp_name_uri_encoding
            uri_template_vector_01 uri_template_vector_02 uri_template_vector_03
            uri_template_vector_04 uri_template_vector_05 uri_template_vector_06
            uri_template_vector_07 uri_template_vector_08 uri_template_vector_09
            uri_template_vector_10 uri_template_vector_11 uri_template_vector_12
            uri_template_vector_13 uri_template_vector_14 uri_template_vector_15
            uri_template_vector_16 uri_template_vector_17 uri_template_vector_18
            uri_template_vector_19 uri_template_vector_20 uri_template_vector_21
            uri_template_vector_22 uri_template_vector_23 uri_template_vector_24
            uri_template_vector_25 uri_template_vector_26 uri_template_vector_27
            uri_template_vector_28 uri_template_vector_29 uri_template_vector_30
            uri_template_vector_31 uri_template_vector_32 uri_template_vector_33
            uri_template_vector_34 uri_template_vector_35 uri_template_vector_36
            uri_template_vector_37 uri_template_vector_38 uri_template_vector_39
            uri_template_vector_40 uri_template_vector_41 uri_template_vector_42
            uri_template_vector_43 uri_template_vector_44 uri_template_vector_45
            uri_template_vector_46 uri_template_vector_47 uri_template_vector_48)
        add_test(NAME cppmcp_phase5_${test_name}_test
            COMMAND cppmcp_core_tests phase5_${test_name})
    endforeach()
    set(uri_template_official_index 0)
    foreach(vector_file IN ITEMS spec-examples.json extended-tests.json)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/uritemplate-test/${vector_file}"
            uri_template_json)
        string(JSON suite_count LENGTH "${uri_template_json}")
        if(suite_count GREATER 0)
            math(EXPR suite_last "${suite_count} - 1")
            foreach(suite_index RANGE 0 ${suite_last})
                string(JSON suite_name MEMBER "${uri_template_json}" ${suite_index})
                string(JSON vector_count LENGTH "${uri_template_json}" "${suite_name}" testcases)
                if(vector_count GREATER 0)
                    math(EXPR vector_last "${vector_count} - 1")
                    foreach(vector_index RANGE 0 ${vector_last})
                        add_test(
                            NAME cppmcp_phase5_uri_template_official_vector_${uri_template_official_index}_test
                            COMMAND cppmcp_core_tests
                                phase5_uri_template_official_${uri_template_official_index})
                        math(EXPR uri_template_official_index
                            "${uri_template_official_index} + 1")
                    endforeach()
                endif()
            endforeach()
        endif()
    endforeach()
    add_test(NAME api_consumer COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_install.sh" "${CMAKE_BINARY_DIR}")
    set_tests_properties(api_consumer PROPERTIES TIMEOUT 180)
endif()
