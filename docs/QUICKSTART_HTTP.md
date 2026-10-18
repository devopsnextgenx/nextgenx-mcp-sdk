# Streamable HTTP quickstart

This guide uses the same endpoint and request headers as
[`scripts/verify_3b.sh`](../scripts/verify_3b.sh). It assumes the project has
been built with CMake and `curl`, `jq`, and Python 3 are installed.

Legacy mode uses a session-bearing request flow:

```bash
URL="http://127.0.0.1:8000/mcp"
INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"verify","version":"0"}}}'
curl -sS -D - -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d "$INIT" "$URL"
```

A successful legacy initialize response includes a `Mcp-Session-Id` header and a
JSON-RPC result payload. Later requests reuse that session ID and the legacy
protocol version header:

```bash
curl -sS -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'Mcp-Session-Id: <session-id>' \
  -H 'MCP-Protocol-Version: 2025-11-25' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
  "$URL"
```

The example server also supports modern, dual, and legacy runtime modes:

```bash
./build/cppmcp_http_server --port 8000 --mode modern
./build/cppmcp_http_server --port 8000 --mode legacy
./build/cppmcp_http_server --port 8000 --mode dual --ping-interval-ms 500
```

## Start the example

```sh
./build/cppmcp_http_server --port 8000
```

In another shell, use the gate's protocol metadata and request helper:

```bash
URL="http://127.0.0.1:8000/mcp"
META='"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{"name":"verify","version":"0"},"io.modelcontextprotocol/clientCapabilities":{}}'
body() { printf '{"jsonrpc":"2.0","id":1,"method":"%s","params":{%s%s}}' "$1" "$2" "$META"; }
H=(-H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" -H "MCP-Protocol-Version: 2026-07-28")
post() { local method="$1" name="$2" payload="$3"; shift 3
  local extra=(-H "Mcp-Method: $method"); [[ -n "$name" ]] && extra+=(-H "Mcp-Name: $name")
  curl -sS -m 15 "${H[@]}" "${extra[@]}" "$@" -d "$payload" "$URL"; }
```

The first calls match the gate:

```bash
post server/discover '' "$(body server/discover '')" | jq
post tools/list '' "$(body tools/list '')" | jq
post tools/call add "$(body tools/call '"name":"add","arguments":{"a":1,"b":2},')" | jq
```

The server exposes `echo`, `add`, `slow_count`, `emit_demo`, and
`weather_like`. `weather_like` requires a `city` string. The HTTP header
`Mcp-Param-city` mirrors that argument:

```bash
curl -sS -m 15 "${H[@]}" \
  -H 'Mcp-Method: tools/call' -H 'Mcp-Name: weather_like' \
  -H 'Mcp-Param-city: Paris' \
  -d "$(body tools/call '"name":"weather_like","arguments":{"city":"Paris"},')" "$URL" | jq
```

Mirrored headers use the same encoding as `Mcp-Name`; non-ASCII values and
values that look like the encoded sentinel are represented with the
`=?base64?...?=` form. The JSON argument remains authoritative. This
deliberately mismatched request is rejected with HTTP 400 and JSON-RPC error
`-32020`:

```bash
curl -i -m 15 "${H[@]}" \
  -H 'Mcp-Method: tools/call' -H 'Mcp-Name: weather_like' \
  -H 'Mcp-Param-city: London' \
  -d "$(body tools/call '"name":"weather_like","arguments":{"city":"Paris"},')" "$URL"
```

## Stream a tool response

Request-scoped events switch automatic mode to SSE. This is the same request
used by the acceptance gate:

```bash
curl -sN -m 15 -D /tmp/cppmcp-sse.headers "${H[@]}" \
  -H 'Mcp-Method: tools/call' -H 'Mcp-Name: emit_demo' \
  -d "$(body tools/call '"name":"emit_demo","arguments":{"count":3},')" "$URL"
```

The event stream carries progress events and ends with the final JSON-RPC
result. Closing the response connection requests cancellation for that tool.

## Bearer token

Stop the first server and launch with a token:

```sh
./build/cppmcp_http_server --port 8000 --token 'replace-with-a-local-secret'
```

Unauthenticated requests receive HTTP 401. Send the bearer token in the
standard `Authorization` header:

```bash
TOKEN='replace-with-a-local-secret'
curl -sS -m 15 "${H[@]}" -H "Authorization: Bearer $TOKEN" \
  -H 'Mcp-Method: tools/list' \
  -d "$(body tools/list '')" "$URL" | jq
```

For a non-loopback deployment, the listener also requires
`--allow-non-loopback`. Configure allowed hosts/origins and place TLS at a
trusted reverse proxy. The proxy must preserve `Mcp-*`, `MCP-Protocol-Version`,
`Origin`, and `Authorization` headers, forward POST bodies without rewriting
them, and disable response buffering for `text/event-stream`. Do not expose an
unauthenticated listener to an untrusted network.

## MCP clients

With Claude Code installed, add the running endpoint:

```sh
claude mcp add --transport http cppmcp-demo http://127.0.0.1:8000/mcp
```

The MCP Inspector web UI can be started with:

```sh
npx @modelcontextprotocol/inspector
```

Choose Streamable HTTP, enter `http://127.0.0.1:8000/mcp`, then connect and
inspect `tools/list` and `tools/call`. Its CLI can run the tools-list request
without opening the UI:

```sh
npx @modelcontextprotocol/inspector --cli \
  http://127.0.0.1:8000/mcp --method tools/list
```

Interop depends on the client supporting the current stateless Streamable HTTP
request shape and protocol version. See [3B_EVIDENCE.md](3B_EVIDENCE.md) for
the recorded tool attempts and their results.
