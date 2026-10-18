# CMake generated Testfile for 
# Source directory: /home/kira/git/devopsnextgenx/nextgenx-mcp-sdk
# Build directory: /home/kira/git/devopsnextgenx/nextgenx-mcp-sdk/build-warn
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[cppmcp_core_tests]=] "/home/kira/git/devopsnextgenx/nextgenx-mcp-sdk/build-warn/cppmcp_core_tests")
set_tests_properties([=[cppmcp_core_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/kira/git/devopsnextgenx/nextgenx-mcp-sdk/CMakeLists.txt;185;add_test;/home/kira/git/devopsnextgenx/nextgenx-mcp-sdk/CMakeLists.txt;0;")
subdirs("_deps/nlohmann_json-build")
