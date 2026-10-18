cmake_minimum_required(VERSION 3.24)
project(cppmcp_install_consumer LANGUAGES CXX)

find_package(cppmcp CONFIG REQUIRED)
add_executable(cppmcp_install_consumer main.cpp)
target_compile_features(cppmcp_install_consumer PRIVATE cxx_std_20)
target_link_libraries(cppmcp_install_consumer PRIVATE cppmcp::transport_http)
