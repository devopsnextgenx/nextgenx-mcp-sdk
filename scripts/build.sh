#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: scripts/build.sh <linux|macos|windows> [options]

Options:
  --deps       Fetch the pinned CMake dependencies (default)
  --no-deps    Configure offline using an existing FetchContent cache
  --shared     Build the shared library
  --static     Build the static library
  --package    Stage installable headers/libraries and create a tar.gz archive
  --help       Show this help

If neither --shared nor --static is specified, both library variants are built.
USAGE
}

if [[ $# -eq 1 && "$1" == --help ]]; then
    usage
    exit 0
fi

if [[ $# -lt 1 ]]; then
    usage >&2
    exit 2
fi

platform=$1
shift
case "$platform" in
    linux|macos|windows) ;;
    *)
        printf 'Unsupported platform: %s\n' "$platform" >&2
        usage >&2
        exit 2
        ;;
esac

case "$(uname -s)" in
    Linux*) host_platform=linux ;;
    Darwin*) host_platform=macos ;;
    MINGW*|MSYS*|CYGWIN*) host_platform=windows ;;
    *)
        printf 'Unable to identify native build platform with uname.\n' >&2
        exit 2
        ;;
esac
if [[ "$platform" != "$host_platform" ]]; then
    printf 'Requested %s build on %s host; this script currently builds natively only.\n' \
        "$platform" "$host_platform" >&2
    exit 2
fi

fetch_deps=ON
build_shared=
build_static=
package=OFF
while [[ $# -gt 0 ]]; do
    case "$1" in
        --deps) fetch_deps=ON ;;
        --no-deps) fetch_deps=OFF ;;
        --shared) build_shared=ON ;;
        --static) build_static=ON ;;
        --package) package=ON ;;
        --help) usage; exit 0 ;;
        *)
            printf 'Unknown option: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

if [[ -z "$build_shared" && -z "$build_static" ]]; then
    build_shared=ON
    build_static=ON
fi
build_shared=${build_shared:-OFF}
build_static=${build_static:-OFF}

root=$(cd "$(dirname "$0")/.." && pwd)
build_dir="$root/build/$platform"

cmake_args=(
    -S "$root"
    -B "$build_dir"
    "-DCPPMCP_BUILD_SHARED=$build_shared"
    "-DCPPMCP_BUILD_STATIC=$build_static"
    "-DCPPMCP_OFFLINE=$([[ "$fetch_deps" == ON ]] && printf OFF || printf ON)"
)

cmake "${cmake_args[@]}"
cmake --build "$build_dir" --config Release --parallel
ctest --test-dir "$build_dir" --build-config Release --output-on-failure

if [[ "$package" == ON ]]; then
    stage="$root/dist/stage-$platform"
    archive="$root/dist/cppmcp-$platform.tar.gz"
    if grep -q '^CMAKE_CONFIGURATION_TYPES:STRING=' "$build_dir/CMakeCache.txt"; then
        cmake --install "$build_dir" --config Release --prefix "$stage"
    else
        cmake --install "$build_dir" --prefix "$stage"
    fi
    mkdir -p "$root/dist"
    tar -czf "$archive" -C "$root/dist" "stage-$platform"
    printf 'Package created: %s\n' "$archive"
fi
