#!/bin/bash

usage() {
    cat <<'USAGE'
Usage: scripts/start-mcp-inspector.sh [--stdio|--http]

Options:
  --stdio     Use stdio transport (default)
  --http      Use http transport
  --help      Show this help
USAGE
}

stdio=ON
http=OFF
while [[ $# -gt 0 ]]; do
    case "$1" in
        --stdio) stdio=ON; http=OFF ;;
        --http) stdio=OFF; http=ON ;;
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

if [[ "$stdio" == ON ]]; then
    CLIENT_PORT=7284 SERVER_PORT=7287 MCP_SANDBOX_PORT=7285 MCP_APP_ORIGIN_PORT=7288 \
        npx @modelcontextprotocol/inspector ./build/linux/cppmcp_stdio_server
elif [[ "$http" == ON ]]; then
    ./build/linux/cppmcp_http_server &
    sleep 1
    npx @modelcontextprotocol/inspector --server-url http://localhost:8000/mcp --transport http
else
    printf 'No transport specified; use --stdio or --http\n' >&2
    usage >&2
    exit 2
fi

# cleanup http server on exit
if [[ "$http" == ON ]]; then
    trap 'kill $! 2>/dev/null' EXIT
fi