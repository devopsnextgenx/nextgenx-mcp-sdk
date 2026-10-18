#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$repo_root/build"

cmake -S "$repo_root" -B "$build_dir" \
    -DCPPMCP_BUILD_SHARED=ON \
    -DCPPMCP_BUILD_STATIC=ON \
    -DCPPMCP_BUILD_TESTS=ON

cmake --build "$build_dir" --parallel \
    --target cppmcp_transport_http cppmcp_stdio_client cppmcp_stdio_server cppmcp_core_tests

printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{"name":"smoke","version":"0"},"io.modelcontextprotocol/clientCapabilities":{}}}}' \
  | "$build_dir/cppmcp_stdio_server" | head -n1 | jq -e '.result.tools | length > 0'

ctest --test-dir "$build_dir" --output-on-failure

echo "Build and validation complete for stdio + HTTP transport targets and the test suite."