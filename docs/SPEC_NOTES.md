# MCP specification notes

This document records the source-of-truth review for the first cppmcp phase. The
specification, schema, and extension are versioned separately; each implementation
phase must re-check the relevant sources rather than assuming this snapshot is
still current.

## Sources reviewed

- [MCP specification 2026-07-28](https://modelcontextprotocol.io/specification/2026-07-28)
- [2026-07-28 changelog](https://modelcontextprotocol.io/specification/2026-07-28/changelog)
- [MCP specification 2025-11-25](https://modelcontextprotocol.io/specification/2025-11-25)
- [2026-07-28 schema.json](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2026-07-28/schema.json), snapshotted at [schema.json](../schema/2026-07-28/schema.json)
- [2026-07-28 schema.ts](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2026-07-28/schema.ts)
- [MCP Apps overview](https://modelcontextprotocol.io/docs/extensions/apps) and [MCP Apps extension specification](https://github.com/modelcontextprotocol/ext-apps/blob/main/specification/2026-01-26/apps.mdx)
- [MCP authorization](https://modelcontextprotocol.io/specification/2026-07-28/basic/authorization)
- [RFC 9728](https://datatracker.ietf.org/doc/html/rfc9728), [RFC 8414](https://datatracker.ietf.org/doc/html/rfc8414), [RFC 8707](https://datatracker.ietf.org/doc/html/rfc8707), [RFC 9207](https://datatracker.ietf.org/doc/html/rfc9207), [RFC 7636](https://datatracker.ietf.org/doc/html/rfc7636), [OAuth 2.1 draft](https://datatracker.ietf.org/doc/html/draft-ietf-oauth-v2-1), [RFC 7662](https://datatracker.ietf.org/doc/html/rfc7662), and [RFC 7591](https://datatracker.ietf.org/doc/html/rfc7591)
- [MCP conformance suite](https://github.com/modelcontextprotocol/conformance)

## Protocol requirements that shape the implementation

### Modern protocol: 2026-07-28 and later

- The protocol is stateless. There is no `initialize` handshake, negotiated
  connection session, `Mcp-Session-Id`, or server-initiated JSON-RPC request.
- Every request carries `io.modelcontextprotocol/protocolVersion` and
  `io.modelcontextprotocol/clientCapabilities` in `_meta`. Clients should also
  identify themselves there; servers should return their identity in result
  `_meta`. A server must implement `server/discover`.
- Modern version compatibility is decided independently per request. A server
  rejects an unsupported version with `UnsupportedProtocolVersionError`.
- Error codes reserved by MCP are `-32020` (`HeaderMismatchError`), `-32021`
  (`MissingRequiredClientCapabilityError`), and `-32022`
  (`UnsupportedProtocolVersionError`). Resource-not-found is `-32602`; `-32002`
  is not valid for a modern server.
- Results include `resultType` (`complete` or `input_required`). A client
  interoperating with an older server treats a missing value as `complete`.
  Multi Round-Trip Requests (MRTR) return input requests and are continued by
  retrying the original operation with input responses and request state.
- Opt-in change events use `subscriptions/listen`; request-scoped progress and
  logging notifications travel on the response stream for their originating
  request. `notifications/message` is forbidden unless that request supplied
  `io.modelcontextprotocol/logLevel`.
- Modern Streamable HTTP uses POST to one endpoint. `Mcp-Method` and `Mcp-Name`
  are required and mirrored metadata must agree with the body. Modern SSE has
  no event IDs or replay / `Last-Event-ID` semantics; a lost request stream is
  retried as a new request with a new ID.
- Cacheable list/read results carry `ttlMs` and `cacheScope`. Tool listing
  should be deterministic. `inputSchema` and `outputSchema` permit JSON Schema
  2020-12; reference resolution and composition must be bounded. `structuredContent`
  is not restricted to an object.
- Capabilities can advertise extensions. Trace context (`traceparent`,
  `tracestate`, and `baggage`) is carried in `_meta`.

### Legacy protocol: 2025-11-25 and earlier

- Legacy peers use `initialize` / `notifications/initialized`, negotiate a
  protocol version at connection setup, and may use session-bearing Streamable
  HTTP, server-initiated requests, ping, HTTP GET streams, and SSE replay.
- The compatibility layer must preserve the same application handlers while
  adapting wire behavior. Support for individual historic versions must be
  verified against their own schema and transport rules.
- The modern changelog deprecates features including roots, sampling, logging,
  ping, and the elicitation completion notification. Their presence and
  lifecycle must be checked feature-by-feature; “deprecated” does not by itself
  mean a feature disappeared from every current schema.

### Authorization

- HTTP authorization is optional for an MCP implementation. When used, MCP
  servers act as OAuth resource servers and must expose Protected Resource
  Metadata (RFC 9728). Clients use that metadata to discover authorization
  servers and support OAuth Authorization Server Metadata (RFC 8414) and
  OpenID Connect discovery.
- Client registration preference is Client ID Metadata Documents, then
  pre-registration, then Dynamic Client Registration (RFC 7591) as a
  deprecated compatibility fallback. Authorization Code with PKCE, a
  resource indicator (RFC 8707), and issuer validation (RFC 9207) are part of
  the authorization flow.
- stdio credentials come from the environment instead of the HTTP OAuth flow.

### MCP Apps

- MCP Apps is an extension, not part of the core protocol. Its resource URI is
  declared as `_meta.ui.resourceUri`; the older flat `ui/resourceUri` key is
  deprecated. The current HTML profile is `text/html;profile=mcp-app`.
- UI metadata communicates CSP/domain allowances and requested sandbox
  permissions. Hosts enforce CSP, must not assume requested permissions are
  granted, and enforce `visibility` for model- and app-originated tool calls.
- UI support is negotiated via the extension capability, and a tool still needs
  useful non-UI output for hosts that do not support the extension.

## Prompt discrepancies and resolution

1. The 2026 error enum is not a general allocation to use freely: the schema
   assigns exactly `-32020`, `-32021`, and `-32022` to the errors above. The
   project uses those exact codes and names.
2. The prompt describes all deprecated roots / sampling / logging behavior as
   legacy-only. The current specification's deprecated-features registry still
   defines roots, with its current-era request pattern using MRTR. Following the
   live spec takes precedence: any feature retained by the current schema will
   be implemented according to its current wire form, and deprecated status
   will be made explicit in the API. Phase 1 does not implement these features.
3. The prompt's description of extension negotiation as initialization-based
   is only accurate for legacy peers. Modern protocol metadata and capabilities
   are per request; current extension behavior must not depend on an
   initialization handshake.
4. The prompt permits compatibility probes on stdio. The modern stdio rules
   define probing with `server/discover` before other calls for dual-era
   clients; an unsupported-version error means “modern but different version,”
   not “legacy.” Only an unrecognized error or timeout should trigger legacy
   initialization.
5. The current protocol allows extension result types and arbitrary JSON
   `structuredContent`; the C++ API must not hard-code results to a fixed set
   of content shapes.

## Phase 1 scope

The first phase establishes the C++20/CMake foundation, a pinned JSON
dependency, the 2026 schema snapshot, strict JSON-RPC message parsing and
serialization, protocol error codes, version-era identification and highest
common-version selection, a schema-contract test, CI scaffolding, and a
cross-platform Bash build/package entry point. It does not implement a
transport, a full schema validator, server/client dispatch, or protocol
features beyond the core message and version primitives.

The schema snapshot is the official `schema/2026-07-28/schema.json`; its SHA-256
is `e8c8ae56f7dd8eab465fb2740db54042761c4f9e0e19b23b98504cec27f4c91d`.

## Streamable HTTP review

The HTTP handler work re-read the versioned transport and lifecycle material:

- [2026-07-28 Streamable HTTP](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http)
- [2025-11-25 transports](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
- [2025-03-26 transports](https://modelcontextprotocol.io/specification/2025-03-26/basic/transports)
- [2025-11-25 lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)
- [2026-07-28 versioning and compatibility](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning)

The current revision differs materially from the legacy transport:

- Modern Streamable HTTP is POST-only and has no session ID, GET event
  endpoint, DELETE termination, `Last-Event-ID`, or stream replay. Legacy
  2025-03-26 through 2025-11-25 retains those session and resumption rules.
- Current requests require `MCP-Protocol-Version` to match
  `_meta["io.modelcontextprotocol/protocolVersion"]`, and require `Mcp-Method`.
  `Mcp-Name` mirrors `params.name` for `tools/call` and `prompts/get`, and
  `params.uri` for `resources/read`. Request `_meta` also requires an object
  at `io.modelcontextprotocol/clientCapabilities`. Header names are
  case-insensitive.
- `Mcp-Name` and annotated `Mcp-Param-*` values use the case-sensitive
  `=?base64?...?=` sentinel for values that are not safe plain ASCII, and for
  plain values that themselves look like that sentinel.
- Missing, malformed, or mismatched mirrored headers use HTTP `400` with
  JSON-RPC error `-32020` (`HeaderMismatch`); unsupported versions use HTTP
  `400` with error `-32022` and `data.requested` / `data.supported`, as defined
  by the current schema snapshot.
- The current transport page describes `202 Accepted` for an accepted
  notification POST, but also states that the 2026-07-28 core defines no
  client-to-server notifications over Streamable HTTP. The implementation
  rejects such notifications rather than silently accepting unimplemented
  methods. JSON-RPC responses are not valid client POST bodies in this
  revision.
- Request-scoped SSE can carry related notifications followed by the final
  response; `subscriptions/listen` is a distinct long-lived stream for opted-in
  change notifications. Modern streams do not support event IDs or replay.

The current implementation validates the modern request metadata and header
mirrors, returns JSON or a final-response SSE event, and rejects unsupported
methods. It does not yet implement the socket adapter, `subscriptions/listen`,
tool parameter header annotations, legacy sessions, or the HTTP client; those
behaviors must not be inferred from the in-process handler.
