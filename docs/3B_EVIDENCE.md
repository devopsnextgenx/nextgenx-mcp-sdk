# Phase 3B evidence

## Acceptance-gate curl transcripts

Captured by `scripts/verify_3b.sh --slice 5` (port and temporary log paths are
chosen dynamically by the script):

```text
-- transcript: server/discover
{"id":1,"jsonrpc":"2.0","result":{"capabilities":{"tools":{"listChanged":false}},"protocolVersions":["2026-07-28"],"serverInfo":{"name":"cppmcp-http-server","title":"cppmcp HTTP server","version":"0.1.0"}}}

-- transcript: tools/list
{"id":1,"jsonrpc":"2.0","result":{"cacheScope":"private","resultType":"complete","tools":[{"annotations":{"destructiveHint":true,"idempotentHint":true,"openWorldHint":false,"readOnlyHint":false},"description":"Add two numbers.","inputSchema":{"type":"object"},"name":"add","title":"Add"},{"annotations":{"destructiveHint":true,"idempotentHint":true,"openWorldHint":false,"readOnlyHint":false},"description":"Return the supplied value.","inputSchema":{"type":"object"},"name":"echo","title":"Echo"},{"annotations":{"destructiveHint":true,"idempotentHint":true,"openWorldHint":false,"readOnlyHint":fals
```

The gate limits printed JSON transcripts to 600 bytes. The `tools/list`
response continues with the remaining tools, including `weather_like` and its
`x-mcp-header` property; the gate separately asserts that annotation.

```text
-- transcript: tools/call add
{"id":1,"jsonrpc":"2.0","result":{"_meta":{"io.modelcontextprotocol/serverInfo":{"name":"cppmcp-http-server","title":"cppmcp HTTP server","version":"0.1.0"}},"content":[{"text":"3.0","type":"text"}],"resultType":"complete","structuredContent":{"result":3.0}}}

-- transcript: streaming call (emit_demo)
HTTP/1.1 200 OK
Content-Type: text/event-stream
Cache-Control: no-cache
X-Accel-Buffering: no
Transfer-Encoding: chunked
Connection: keep-alive

data: {"count":1,"total":3,"type":"progress"}

data: {"count":2,"total":3,"type":"progress"}

data: {"count":3,"total":3,"type":"progress"}

data: {"id":1,"jsonrpc":"2.0","result":{"_meta":{"io.modelcontextprotocol/serverInfo":{"name":"cppmcp-http-server","title":"cppmcp HTTP server","version":"0.1.0"}},"content":[{"text":"{\"count\":3}","type":"text"}],"resultType":"complete","structuredContent":{"count":3}}}
```

## CTest discovery and results

`ctest --test-dir build -N` discovered 31 independent cases:

```text
cppmcp_json_rpc_test
cppmcp_versions_test
cppmcp_schema_contract_test
cppmcp_phase2_memory_test
cppmcp_phase2_stdio_test
cppmcp_http_handler_test
cppmcp_http_handler_mcp_param_stateless_test
cppmcp_cancellation_test
cppmcp_http_server_methods_test
cppmcp_http_server_host_origin_test
cppmcp_http_server_smuggling_test
cppmcp_http_server_slowloris_test
cppmcp_http_server_auth_route_bind_test
cppmcp_http_server_allow_header_test
cppmcp_http_server_unknown_route_test
cppmcp_http_server_duplicate_host_test
cppmcp_http_server_duplicate_origin_test
cppmcp_http_server_invalid_origin_test
cppmcp_http_server_bearer_auth_test
cppmcp_http_server_bearer_reject_test
cppmcp_http_server_missing_param_header_test
cppmcp_http_server_duplicate_param_header_test
cppmcp_http_server_unexpected_param_header_test
cppmcp_http_server_optional_param_header_test
cppmcp_http_server_sse_framing_test
cppmcp_http_server_response_modes_test
cppmcp_http_server_sse_keepalive_test
cppmcp_http_server_slow_consumer_test
cppmcp_http_server_disconnect_cancel_test
cppmcp_http_server_sse_soak_test
cppmcp_http_server_mcp_param_stateless_test
```

`ctest --test-dir build --output-on-failure`:

```text
100% tests passed, 0 tests failed out of 31
Total Test time (real) = 6.04 sec
```

## Fuzz, install, and performance smoke

Clang 21.1.8 built all three libFuzzer targets. Each was run with
`-runs=10 -max_len=512 -timeout=2`:

```text
cppmcp_fuzz_mirrored_header: Done 10 runs in 0 second(s)
cppmcp_fuzz_mcp_name_param: Done 10 runs in 0 second(s)
cppmcp_fuzz_http_request: Done 10 runs in 0 second(s)
```

The installed-package consumer configured, linked against
`cppmcp::transport_http`, and ran successfully via
`bash scripts/verify_install.sh build`.

`python3 scripts/perf_smoke_http.py build/cppmcp_http_server`:

```text
perf smoke: requests=100 throughput=196.4 req/s median=5.09 ms p95=5.22 ms
```

This is a local smoke measurement, not a controlled benchmark.

## Workflow validation

`go run github.com/rhysd/actionlint/cmd/actionlint@v1.7.7 .github/workflows/ci.yml`
completed with exit status 0 and no diagnostics.

## Claude Code and MCP Inspector

Against a running `cppmcp_http_server` at a dynamically allocated loopback
port, with Claude's configuration isolated to a temporary directory:

```text
claude mcp add --transport http cppmcp-demo http://127.0.0.1:36611/mcp
Added HTTP MCP server cppmcp-demo with URL: http://127.0.0.1:36611/mcp to local config
claude_exit=0

npx --yes @modelcontextprotocol/inspector --cli http://127.0.0.1:36611/mcp --method tools/list
{"error":{"code":"error","message":"Error POSTing to endpoint: {\"error\":{\"code\":-32020,\"data\":{\"field\":\"MCP-Protocol-Version\"},\"message\":\"HTTP header does not match the JSON-RPC request: \"},\"id\":0,\"jsonrpc\":\"2.0\"}","status":400}}
inspector_exit=1
```

Claude Code accepted the server configuration; this command does not itself
connect to or validate the remote server. The Inspector made an HTTP request
but its `MCP-Protocol-Version` header did not mirror the request-body metadata,
so the server correctly rejected it with `HeaderMismatch`. Windows, GitHub
Actions, and the Inspector web UI were not run locally.

## Sanitizers

ASan/UBSan and TSan results from
`scripts/verify_3b.sh --slice 5 --sanitizers` will be appended after the final
gate run.
