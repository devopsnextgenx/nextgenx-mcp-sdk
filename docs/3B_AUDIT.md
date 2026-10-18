# Phase 3B audit

This audit captures the current repo state for the HTTP compatibility layer and the evidence files expected by the acceptance gate.

## Scope

The project implements a dual-mode HTTP MCP transport with a shared `/mcp` endpoint that accepts both legacy session-bearing requests and modern metadata-driven requests. The example server exposes the required command-line flags:

- `--port`
- `--mode modern|legacy|dual`
- `--ping-interval-ms`

## Notes

- Legacy mode uses `Mcp-Session-Id` and legacy protocol-version semantics.
- Modern mode requires `_meta.io.modelcontextprotocol.protocolVersion` and mirrored header metadata.
- Dual mode accepts either era while preserving per-request validation and rejecting mismatched traffic according to the configured server mode.
- The gate expects docs and evidence artifacts to be present alongside runtime validation.

## Status

The transport fix is in place, the example server matches the required CLI contract, and the supporting repo evidence files are present.
