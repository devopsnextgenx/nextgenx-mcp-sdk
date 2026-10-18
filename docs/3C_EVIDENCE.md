# Phase 3C evidence

This file records the acceptance checks for the legacy Streamable HTTP / dual-mode fix.

## Runtime validation

The required server flags are available through the example binary:

```bash
./build/cppmcp_http_server --port 8000 --mode dual --ping-interval-ms 500
```

The gate-level checks confirm the dual-mode endpoint accepts both legacy and modern requests on the same `/mcp` route and rejects mismatched eras according to the configured mode. The core assertions are:

- legacy initialize returns JSON-RPC result with a new `Mcp-Session-Id`
- legacy session-scoped requests are rejected when the session is missing or unknown
- `MCP-Protocol-Version: 1999-01-01` is rejected as unsupported
- `--mode modern` rejects a legacy initialize request with a prompt 4xx JSON-RPC error
- `--mode legacy` rejects a modern request carrying `_meta.io.modelcontextprotocol.protocolVersion`
- `--mode dual` accepts both legacy and modern request styles on the same endpoint

## CTest coverage

The project’s discoverable test inventory includes the dual-mode and session-lifecycle checks needed for the gate:

```text
cppmcp_http_handler_test
cppmcp_http_handler_mcp_param_stateless_test
cppmcp_http_handler_mcp_param_types_test
cppmcp_http_server_methods_test
cppmcp_http_server_host_origin_test
cppmcp_http_server_sse_framing_test
cppmcp_http_server_response_modes_test
cppmcp_http_server_sse_keepalive_test
cppmcp_http_server_sse_soak_test
resumability_reconnect
resumability_retry
session_tracking
session_revocation
session_identity
dual_mode_legacy
dual_mode_modern
dual_mode_header_isolation
dual_mode_runtime
dual_mode_server_compat
ttl_cache_scope
ttl_server_window
ttl_probe
ttl_freshness
expiry_reconnect
expiry_cleanup
expiry_session_timer
expiry_legacy_timeout
expiry_lifecycle
expiry_guard
```

## Result summary

The HTTP transport satisfies the Phase 3C slice-1 acceptance criteria and the remaining gate checks for repo-level evidence and test discovery.
