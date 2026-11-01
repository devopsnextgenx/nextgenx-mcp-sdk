#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CXX_FLAGS="$(sed -n 's/^CMAKE_CXX_FLAGS:STRING=//p' "$BUILD/CMakeCache.txt")"
EXE_LINKER_FLAGS="$(sed -n 's/^CMAKE_EXE_LINKER_FLAGS:STRING=//p' "$BUILD/CMakeCache.txt")"

cmake --install "$BUILD" --prefix "$TMP/install"
cmake -S "$ROOT/tests/install_consumer" -B "$TMP/consumer" \
  -DCMAKE_PREFIX_PATH="$TMP/install" \
  -DCMAKE_CXX_FLAGS:STRING="$CXX_FLAGS" \
  -DCMAKE_EXE_LINKER_FLAGS:STRING="$EXE_LINKER_FLAGS"
cmake --build "$TMP/consumer" --parallel
"$TMP/consumer/cppmcp_install_consumer"

cmake -S "$ROOT/tests/api_consumer" -B "$TMP/api-consumer" \
  -DCMAKE_PREFIX_PATH="$TMP/install" \
  -DCMAKE_CXX_FLAGS:STRING="$CXX_FLAGS" \
  -DCMAKE_EXE_LINKER_FLAGS:STRING="$EXE_LINKER_FLAGS"
cmake --build "$TMP/api-consumer" --parallel
"$TMP/api-consumer/cppmcp_api_consumer"
