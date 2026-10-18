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

For a non-loopback deployment, configure TLS or authentication. The
`--allow-insecure-remote` switch is an explicit opt-in for deployments that
deliberately use neither. Configure allowed hosts/origins and the proxy to
preserve `Mcp-*`, `MCP-Protocol-Version`, `Origin`, and `Authorization`
headers, forward POST bodies without rewriting them, and disable response
buffering for `text/event-stream`.

## MCP clients

Build and use the bundled HTTP client against modern, legacy, or dual-mode
servers:

```sh
cmake --build build --target cppmcp_http_client
./build/cppmcp_http_client --url http://127.0.0.1:8000/mcp --mode auto
```

`--mode modern` requires `server/discover`, `--mode legacy` requires a
session-based legacy server, and `--mode auto` probes modern before falling
back to legacy. The client lists tools and calls `add`. To use a bearer token,
pass `--token "$TOKEN"`; it is sent in the `Authorization` header and is not
printed by the client:

```sh
./build/cppmcp_http_client --url "$URL" --mode auto --token "$TOKEN"
```

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

## Notifications with `subscriptions/listen`

Modern clients open a long-lived POST stream with the notification types they
want to receive. The supplied request fixture includes the required request
metadata and opts into tool-list changes:

```bash
./build/cppmcp_http_server --port 8000 --mode dual --notify-interval-ms 5000
curl -N --max-time 20 \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: subscriptions/listen' \
  --data-binary @scripts/fixtures/listen_request.json \
  http://127.0.0.1:8000/mcp
```

The first `data:` event acknowledges the accepted filters. Subsequent
`notifications/tools/list_changed` events carry the subscription ID in
`params._meta["io.modelcontextprotocol/subscriptionId"]`. The server's
`--notify-interval-ms` option is for exercising the example; applications can
instead call `Server::notify_tools_list_changed()` when their tool list changes.
The bundled client can consume and reconnect a stream:

```sh
./build/cppmcp_http_client --url "$URL" --mode auto --listen --duration-ms 30000
```

It prints `NOTIFY`, `STREAM_LOST`, and `RECONNECTED` lines as events arrive.
After reconnection, re-list tools because the client cannot infer changes that
occurred while the stream was down.

## TLS

OpenSSL 3 is detected by CMake; TLS is enabled by default when it is available.
Generate a local CA and a server certificate with both DNS and IP subject
alternative names:

```sh
mkdir -p tls-local
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout tls-local/ca.key -out tls-local/ca.pem -days 30 \
  -subj '/CN=cppmcp local test CA'
openssl req -newkey rsa:2048 -nodes \
  -keyout tls-local/server.key -out tls-local/server.csr \
  -subj '/CN=localhost'
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n' > tls-local/server.ext
openssl x509 -req -in tls-local/server.csr \
  -CA tls-local/ca.pem -CAkey tls-local/ca.key -CAcreateserial \
  -out tls-local/server.pem -days 30 -extfile tls-local/server.ext
```

Start the example server with TLS and connect using the CA bundle:

```sh
./build/cppmcp_http_server --bind 127.0.0.1 --port 8000 \
  --tls-cert tls-local/server.pem --tls-key tls-local/server.key
./build/cppmcp_http_client --url https://localhost:8000/mcp \
  --mode auto --cacert tls-local/ca.pem
```

The client verifies the certificate chain and the requested DNS name or IP
address by default. Do not disable verification in production. For remote
deployments, configure TLS either on this server or on a trusted reverse proxy.
If TLS terminates at a proxy, ensure the backend is inaccessible to untrusted
networks and disable proxy buffering for SSE responses.

## Authentication hook

The example client accepts a static bearer token on the command line:

```sh
./build/cppmcp_http_client --url "$URL" --mode auto --token "$TOKEN"
```

Library users can supply an `IAuthProvider`. Its `apply(Headers&)` method adds
credentials to requests; `on_challenge(const AuthChallenge&)` receives parsed
Bearer challenge fields and can choose whether the client should retry. Use a
secret store or short-lived token provider in applications rather than embedding
tokens in source or shell history. Credential values are not included in client
logs or authentication error messages.
