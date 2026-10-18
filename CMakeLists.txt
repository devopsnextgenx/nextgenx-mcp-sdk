cmake_minimum_required(VERSION 3.24)

project(cppmcp VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(CPPMCP_BUILD_SHARED "Build the shared library" ON)
option(CPPMCP_BUILD_STATIC "Build the static library" ON)
option(CPPMCP_BUILD_TESTS "Build unit tests" ON)
option(CPPMCP_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(CPPMCP_ENABLE_LEGACY "Enable legacy protocol support" ON)
option(CPPMCP_OFFLINE "Do not fetch dependencies from the network" OFF)

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
    src/json_rpc.cpp
    src/version.cpp
    src/connection.cpp
)

set(CPPMCP_SERVER_SOURCES
    src/server.cpp
)

set(CPPMCP_CLIENT_SOURCES
    src/client.cpp
)

set(CPPMCP_TRANSPORT_STDIO_SOURCES
    src/stdio.cpp
)

set(CPPMCP_TRANSPORT_HTTP_SOURCES
    src/http_handler.cpp
    src/http_server.cpp
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
    target_link_libraries(cppmcp_transport_http PUBLIC cppmcp_server cppmcp_core)
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
endif()

if(CPPMCP_BUILD_SHARED)
    add_library(cppmcp_core_shared SHARED ${CPPMCP_SOURCES})
    add_library(cppmcp::core_shared ALIAS cppmcp_core_shared)
    cppmcp_configure_library(cppmcp_core_shared)
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

add_executable(cppmcp_stdio_server examples/stdio_server.cpp)
target_link_libraries(cppmcp_stdio_server PRIVATE cppmcp::server cppmcp::transport_stdio)
target_compile_features(cppmcp_stdio_server PRIVATE cxx_std_20)
cppmcp_configure_warnings(cppmcp_stdio_server)

add_executable(cppmcp_stdio_client examples/stdio_client.cpp)
target_link_libraries(cppmcp_stdio_client PRIVATE cppmcp::client cppmcp::transport_stdio)
target_compile_features(cppmcp_stdio_client PRIVATE cxx_std_20)
cppmcp_configure_warnings(cppmcp_stdio_client)

add_executable(cppmcp_http_server examples/http_server/main.cpp)
target_link_libraries(cppmcp_http_server PRIVATE cppmcp::server cppmcp::transport_http)
target_compile_features(cppmcp_http_server PRIVATE cxx_std_20)
cppmcp_configure_warnings(cppmcp_http_server)

if(CPPMCP_BUILD_TESTS)
    enable_testing()
    add_executable(cppmcp_core_tests
        tests/test_main.cpp
        tests/json_rpc_test.cpp
        tests/version_test.cpp
        tests/schema_contract_test.cpp
        tests/phase2_test.cpp
        tests/http_handler_test.cpp
        tests/http_server_test.cpp
        tests/cancellation_test.cpp
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
    target_compile_definitions(cppmcp_core_tests PRIVATE
        CPPMCP_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/schema/2026-07-28/schema.json"
    )
    foreach(test_name IN ITEMS
            json_rpc versions schema_contract phase2_memory phase2_stdio
            http_handler http_handler_mcp_param_stateless http_handler_mcp_param_types
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
            expiry_legacy_timeout expiry_lifecycle expiry_guard)
        add_test(NAME cppmcp_${test_name}_test COMMAND cppmcp_core_tests ${test_name})
    endforeach()
endif()
