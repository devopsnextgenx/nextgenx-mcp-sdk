# HTTP interop evidence

## Existing server interop baseline

The Phase 3C environment reported successful real-client interop on
2026-10-17:

| Client | Command / setup | Result |
| --- | --- | --- |
| MCP Inspector CLI | `npx -y @modelcontextprotocol/inspector --cli "$URL" --transport http --method tools/list` against the dual-mode server | Connected and returned the tools list. |
| Claude Code | `claude mcp list` with the dual-mode server configured | `✔ Connected`. |

These are the previously observed Phase 3C results supplied as the Phase 3D
starting state.

## Phase 3D runs

Ran on 2026-10-18 with the completed HTTP client, modern/legacy/dual server,
and TLS support. The complete acceptance command was:

```sh
EVERYTHING_PORT=<free localhost port> bash scripts/verify_3d.sh \
  --slice 4 --sanitizers --interop
```

The port was selected with Python's `socket.bind(("127.0.0.1", 0))` because
the gate's default reference-server port, 3001, was already occupied. The
gate's bounded `npx` and CLI invocations produced these results:

| Interop | Command / setup | Result |
| --- | --- | --- |
| Inspector CLI -> our dual server | `npx -y @modelcontextprotocol/inspector --cli "$URL" --transport http --method tools/list` | Exit 0; gate reported `PASS: Inspector CLI -> our dual server`. |
| Our client -> TypeScript `server-everything` | `PORT=<free port> npx -y @modelcontextprotocol/server-everything streamableHttp`; `build/cppmcp_http_client --url "http://127.0.0.1:$PORT/mcp" --mode auto` | Exit 0; negotiated legacy and listed the reference server's tools. This server does not expose `add`, so the example client lists tools without attempting that optional example-only call. |
| TypeScript SDK client -> our dual server | A Node client loaded `Client` and `StreamableHTTPClientTransport` from `@modelcontextprotocol/sdk` via `npx -y -p @modelcontextprotocol/sdk`; it connected to a dynamically allocated local dual server and called `listTools()`. | Exit 0; `TS_SDK_CONNECTED tools=5`. No npm dependency was added to the repository. |
| Claude Code -> our dual server | Gate configured `cppmcp-verify` with `claude mcp add --transport http cppmcp-verify "$URL"` and checked `claude mcp list`. | `cppmcp-verify: http://127.0.0.1:33959/mcp (HTTP) - ✔ Connected`; gate passed. |

The final gate summary was `RESULT: 63 passed, 0 failed, 0 warnings`. Both
ASan+UBSan and TSan completed with zero findings. The initial interop attempt
without `EVERYTHING_PORT` encountered the unrelated service on port 3001; the
free-port rerun above passed.
