cmake_minimum_required(VERSION 3.24)

project(cppmcp VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(CPPMCP_BUILD_SHARED "Build the shared library" ON)
option(CPPMCP_BUILD_STATIC "Build the static library" ON)
option(CPPMCP_BUILD_TESTS "Build unit tests" ON)
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

if(CPPMCP_BUILD_TESTS)
    enable_testing()
    add_executable(cppmcp_core_tests
        tests/test_main.cpp
        tests/json_rpc_test.cpp
        tests/version_test.cpp
        tests/schema_contract_test.cpp
        tests/phase2_test.cpp
        tests/http_handler_test.cpp
        tests/cancellation_test.cpp
    )
    if(CPPMCP_BUILD_STATIC)
        target_link_libraries(cppmcp_core_tests PRIVATE cppmcp::core cppmcp::server cppmcp::client cppmcp::transport_stdio cppmcp::transport_http)
    else()
        target_link_libraries(cppmcp_core_tests PRIVATE cppmcp::core_shared)
    endif()
    target_compile_features(cppmcp_core_tests PRIVATE cxx_std_20)
    cppmcp_configure_warnings(cppmcp_core_tests)
    target_compile_definitions(cppmcp_core_tests PRIVATE
        CPPMCP_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/schema/2026-07-28/schema.json"
    )
    add_test(NAME cppmcp_core_tests COMMAND cppmcp_core_tests)
endif()
