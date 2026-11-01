cmake_minimum_required(VERSION 3.20)
project(cppmcp_api_consumer LANGUAGES CXX)

find_package(cppmcp CONFIG REQUIRED)

add_executable(cppmcp_api_consumer main.cpp)
target_compile_features(cppmcp_api_consumer PRIVATE cxx_std_20)
target_link_libraries(cppmcp_api_consumer PRIVATE cppmcp::server cppmcp::client cppmcp::core)
