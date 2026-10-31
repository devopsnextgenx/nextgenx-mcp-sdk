#!/bin/bash
# test-example-servers.sh - Test the example servers built by build.sh
# This script is intended to be run after running build.sh with the --examples option.
# arguments:
#   --stdio: test the stdio transport server and client
#   --http: test the http transport server and client
#   --help: show this help message

usage() {
    cat <<'USAGE'
Usage: scripts/test-example-servers.sh [--stdio|--http]

Options:
  --stdio     Use stdio transport (default)
  --http      Use http transport
  --help      Show this help
USAGE
}

STDIO=OFF
HTTP=OFF

while [[ $# -gt 0 ]]; do
    case "$1" in
        --stdio) STDIO=ON ;;
        --http) HTTP=ON ;;
        --help)
            usage;
            exit 0
            ;;
        *)
            printf 'Unknown option: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

if [[ "$STDIO" == ON ]]; then
    cd build/linux
    ./cppmcp_stdio_client
elif [[ "$HTTP" == ON ]]; then
    cd build/linux
    ./cppmcp_http_server &
    sleep 1
    ./cppmcp_http_client --url http://localhost:8000/mcp
else
    printf 'No transport specified; use --stdio or --http\n' >&2
    usage >&2
    exit 2
fi

# cleanup http server on exit
if [[ "$HTTP" == ON ]]; then
    trap 'kill $! 2>/dev/null' EXIT
fi