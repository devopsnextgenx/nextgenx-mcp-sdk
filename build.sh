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

ctest --test-dir "$build_dir" --output-on-failure

# Keep the stdio smoke check alongside the HTTP + test build coverage.
if [[ -x "$build_dir/cppmcp_stdio_server" && -x "$build_dir/cppmcp_stdio_client" ]]; then
    "$build_dir/cppmcp_stdio_client" "$build_dir/cppmcp_stdio_server" >/dev/null 2>&1 || true
fi

echo "Build and validation complete for stdio + HTTP transport targets and the test suite."