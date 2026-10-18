# HTTP interop evidence

## Existing server interop baseline

The Phase 3C environment reported successful real-client interop on
2026-10-17:

| Client | Command / setup | Result |
| --- | --- | --- |
| MCP Inspector CLI | `npx -y @modelcontextprotocol/inspector --cli "$URL" --transport http --method tools/list` against the dual-mode server | Connected and returned the tools list. |
| Claude Code | `claude mcp list` with the dual-mode server configured | `✔ Connected`. |

These are the previously observed Phase 3C results supplied as the Phase 3D
starting state; they are not a claim that either external client was rerun
during Phase 3D. This document will record fresh client results and exact
failures as the Phase 3D client and listen implementation becomes available.
