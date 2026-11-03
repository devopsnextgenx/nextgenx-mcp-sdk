# Streamable HTTP quickstart

## Phase 4 Calls

Build/test in the mounted Ubuntu container as described in [README.md](../README.md).
The HTTP and stdio examples register the same schema/progress/input tools.
Output validation defaults to on:

```sh
./build/cppmcp_http_server --port 8000 --mode dual --validate-output on
./build/cppmcp_http_client --url http://127.0.0.1:8000/mcp --mode modern \
  --call structured_demo --args '{"city":"Paris"}'
./build/cppmcp_http_client --url http://127.0.0.1:8000/mcp --mode modern \
  --call progress_demo --args '{"steps":3,"delay_ms":100}' --progress
./build/cppmcp_http_client --url http://127.0.0.1:8000/mcp --mode modern \
  --call log_demo --args '{}' --log-level warning
```

`RESULT` includes structured JSON and, by default, the same JSON in a text block.
`PROGRESS` arrives before completion; `LOG` respects the requested threshold.
Wrong argument types return `isError: true`, unknown tools return `-32602`, and
`bad_output` returns `-32603` rather than its invalid value.

### Elicitation

```sh
./build/cppmcp_http_client --url http://127.0.0.1:8000/mcp --mode modern \
  --call delete_project --args '{"project":"alpha"}' \
  --elicit accept --elicit-content '{"confirm":true}'
./build/cppmcp_http_client --url http://127.0.0.1:8000/mcp --mode legacy \
  --call delete_project --args '{"project":"alpha"}' --elicit decline
```

The modern client handles `input_required`, fulfills ID-keyed input requests and
echoes `requestState` with the original arguments/metadata and a new RPC ID. The
legacy client answers a server-initiated request on the call's SSE stream. Accept
plus `confirm: true` completes deletion; decline/cancel does not. `loop_input`
exercises the round limit.

[mrtr_accept_response.json](../scripts/fixtures/mrtr_accept_response.json) uses
`__KEY__` for the server-assigned input ID. Do not guess IDs or manufacture a
state token. State is authenticated but readable and must not contain secrets.
For local cross-instance testing pass identical `--request-state-key` values;
the example also accepts `--request-state-key-file PATH` or reads
`CPPMCP_REQUEST_STATE_KEY` when neither CLI option is set. The file or
environment value must be exactly 64 hexadecimal characters (a final newline
in the file is ignored). Use protected secret storage and
`ServerOptions::request_state_keys` in applications; explicit CLI input takes
precedence over the key file, which takes precedence over the environment.

For C++ clients, set `CallToolOptions::on_elicit`, `max_rounds` and `timeout`.
Form support is advertised only with a handler. URL handling additionally needs
`allow_url_elicitation = true`; the callback must validate/present the destination.
Generic `on_input` callbacks use explicit `client_capabilities` for roots or
sampling. Legacy HTTP capabilities must also be declared in
`HttpClientOptions::client_capabilities` before `connect_http` initializes the
session. The CLI does this for `--elicit`.

Server handlers use `ToolContext::input_responses()`, `state()` and
`ToolResult::input_required(requests, private_state)`; the dispatcher authenticates
and validates continuations before invoking them. `ToolResult::structured` and
`error_text` distinguish output from execution errors. `report_progress` is
token-aware; notification limits/intervals are in `HttpHandlerOptions`. Legacy
input waits use `input_request_timeout` (30 seconds by default); configure client
and server deadlines for the application's user-interaction workflow.

Read [SECURITY.md](SECURITY.md) before deploying multi-user continuations. Bind
state to an authenticated subject and enforce idempotency or one-time nonces
for destructive operations.

## Transport Basics

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

For simple development, the example client accepts a static bearer token:

```sh
./build/cppmcp_http_client --url "$URL" --mode auto --token "$TOKEN"
```

Library users can supply an `IAuthProvider`. Its `apply(Headers&)` method adds
credentials to requests; `on_challenge(const AuthChallenge&)` receives parsed
Bearer challenge fields and can choose whether the client should retry. Use a
secret store or short-lived token provider in applications rather than embedding
tokens in source or shell history. Credential values are not included in client
logs or authentication error messages.

For OAuth, configure the HTTP server with a canonical public resource URI, an
authorization-server issuer, and (optionally) supported scopes:

```sh
./build/cppmcp_http_server --port 8000 \
  --auth-resource http://127.0.0.1:8000/mcp \
  --auth-issuer http://127.0.0.1:9000 \
  --auth-scopes-supported 'mcp:use projects:write'
```

Loopback HTTP is accepted only for local testing. Use HTTPS URIs for a real
deployment. The client discovers the server's Protected Resource Metadata from
the 401 challenge or well-known routes, then uses PKCE and the `resource`
indicator:

```sh
./build/cppmcp_http_client --url http://127.0.0.1:8000/mcp \
  --oauth --oauth-allow-insecure-loopback --oauth-launcher http-follow \
  --call add --args '{"a":2,"b":3}'
```

Use `--oauth-launcher print` for a headless authorization-code flow. For
pre-registration, provide `--oauth-client-id`; put any client secret in a
protected file and use `--oauth-client-secret-file`. `--oauth-dcr` enables the
deprecated DCR fallback, and `--oauth-cimd-url` supplies a CIMD URL. The
`--oauth-store-file` option persists issuer-keyed registrations and tokens;
protect the file and consider the library's encrypted `FileTokenStore` for
applications. Never put tokens or client secrets in a URI or logs.

The OAuth provider retries after authentication and performs bounded scope
step-up after an insufficient-scope challenge. See [AUTH.md](AUTH.md) for
server builder examples, scope hierarchy, listing modes, storage details, and
deployment guidance. Stdio does not use this OAuth flow.

## Prompts, resources, and completion

The shared HTTP and stdio demo registry exposes `summarize`, `code_review`, and
`ask_name`; static resources include `demo://readme`, `demo://image`, and
`demo://counter`. It also registers the
`demo://users/{id}/profile{?format}` resource template. The same methods are
available over modern, legacy, and dual HTTP modes:

```bash
post prompts/list '' "$(body prompts/list '')" | jq
post prompts/get summarize \
  "$(body prompts/get '\"name\":\"summarize\",\"arguments\":{\"text\":\"hello\"},')" | jq
post resources/list '' "$(body resources/list '')" | jq
post resources/templates/list '' "$(body resources/templates/list '')" | jq
post resources/read 'demo://readme' \
  "$(body resources/read '\"uri\":\"demo://readme\",')" | jq
```

`Mcp-Name` mirrors the prompt name for `prompts/get` and the URI for
`resources/read`; its value uses the same safe encoding as other mirrored MCP
headers. Completion can use already-resolved prompt arguments:

```bash
post completion/complete '' \
  "$(body completion/complete '\"ref\":{\"type\":\"ref/prompt\",\"name\":\"code_review\"},\"argument\":{\"name\":\"framework\",\"value\":\"d\"},\"context\":{\"arguments\":{\"language\":\"python\"}},')" | jq
```

To see pagination, restart the example with `--page-size 2`. Each list method
returns an opaque `nextCursor` when another page is available; pass that exact
cursor in the next request's params. Cursors are scoped to the list kind and
registry instance.

The C++ `Client` exposes matching feature methods and cursor aggregation:

```cpp
const auto prompt_page = client.list_prompts();
const auto every_prompt = client.list_all_prompts();
const auto prompt = client.get_prompt("summarize", {{"text", "hello"}});
const auto resources = client.list_all_resources();
const auto resource = client.read_resource("demo://readme");
```

`list_all_prompts()` and `list_all_resources()` follow opaque cursors and reject
repeated or malformed continuation cursors. Successful responses with a
positive `ttlMs` are cached in the client's bounded `ClientCache`; resource
updates and prompt/resource list-change notifications invalidate that cache.
`Client::subscribe_resource()` retains subscriptions across a fresh
`connect_http()` and re-establishes the corresponding stream/RPC subscription.

Prompt and resource handlers can also request input. The bundled client drives
the modern `requestState` continuation automatically when an elicitation action
is supplied:

```bash
./build/cppmcp_http_client --url "$URL" --mode modern \
  --request prompts/get --params '{"name":"ask_name"}' \
  --elicit accept --elicit-content '{"name":"Ada"}'
```

For a curl-only round trip, send the initial modern request with elicitation
capability metadata, then echo the returned `requestState` unchanged and attach
the response under the input-request ID:

```bash
ask='{"jsonrpc":"2.0","id":11,"method":"prompts/get","params":{"name":"ask_name","_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{"name":"curl","version":"1"},"io.modelcontextprotocol/clientCapabilities":{"elicitation":{"form":{}}}}}}'
first="$(curl -sS -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: prompts/get' -H 'Mcp-Name: ask_name' \
  --data-binary "$ask" "$URL")"
state="$(jq -r '.result.requestState' <<<"$first")"
retry="$(jq --arg state "$state" \
  '.id=12 | .params.requestState=$state |
   .params.inputResponses={"name":{"action":"accept","content":{"name":"Ada"}}}' \
  <<<"$ask")"
curl -sS -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: prompts/get' -H 'Mcp-Name: ask_name' \
  --data-binary "$retry" "$URL" | jq
```

The retry uses a new JSON-RPC ID and preserves the original method, prompt name
and metadata. Do not construct or edit the signed state token.

Resource update subscriptions and list-change notifications use the modern
`subscriptions/listen` request:

```bash
./build/cppmcp_http_server --port 8000 --mode dual --tick-ms 1000
curl -N --max-time 10 \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: subscriptions/listen' \
  --data-binary @scripts/fixtures/listen_resources_request.json "$URL"
```

The acknowledgement comes first. Subsequent resource-update events are limited
to the accepted `resourceSubscriptions`; list-change events are sent only for
types requested by the client. The bundled client can maintain the resource
filter and listen across reconnects:

```bash
./build/cppmcp_http_client --url "$URL" --mode auto --listen \
  --subscribe-resource demo://counter --duration-ms 30000
```

With `--resource-root DIR`, the file template `demo://files/{+path}` reads
bounded files beneath `DIR`. See [SECURITY.md](SECURITY.md) for path and
symlink restrictions.
