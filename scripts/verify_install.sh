#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cmake --install "$BUILD" --prefix "$TMP/install"
cmake -S "$ROOT/tests/install_consumer" -B "$TMP/consumer" \
  -DCMAKE_PREFIX_PATH="$TMP/install"
cmake --build "$TMP/consumer" --parallel
"$TMP/consumer/cppmcp_install_consumer"
