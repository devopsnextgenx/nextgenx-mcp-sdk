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
- Legacy Streamable HTTP retains session-bearing setup and `Last-Event-ID`
  semantics for resumed SSE streams, while the modern path is stateless and
  cannot replay messages by event ID.
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

The versioned transport page states: “The Streamable HTTP transport mirrors
selected JSON-RPC body fields into HTTP headers so that intermediaries (load
balancers, gateways, observability tooling) can route and inspect requests
without parsing the body.” The body remains authoritative: for mirrored
fields, mismatches are rejected with `400 Bad Request` and `HeaderMismatch`.
The linked [tool-definition extension](https://modelcontextprotocol.io/specification/2026-07-28/server/tools#x-mcp-header)
states: “The `x-mcp-header` property specifies the name portion used to
construct the header name `Mcp-Param-{name}`.”
The implementation applies that rule to `Mcp-Method`, `Mcp-Name`, and declared
`Mcp-Param-*` values. For tool parameters, `x-mcp-header` stores the suffix
used to form the `Mcp-Param-{name}` header. The implementation supports string,
integer, and boolean properties, converts primitive arguments to the
specification's text representation, and requires exactly one matching header
when the property is supplied (and for required properties). Mirrored integer
values are restricted to JavaScript's safe integer range.

## Phase 4 specification review

The relevant 2026-07-28 pages were re-read: [server/tools](https://modelcontextprotocol.io/specification/2026-07-28/server/tools), [basic/patterns/mrtr](https://modelcontextprotocol.io/specification/2026-07-28/basic/patterns/mrtr), [client/elicitation](https://modelcontextprotocol.io/specification/2026-07-28/client/elicitation), [basic/utilities/progress](https://modelcontextprotocol.io/specification/2026-07-28/basic/utilities/progress), [server/utilities/logging](https://modelcontextprotocol.io/specification/2026-07-28/server/utilities/logging), and [basic/versioning](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning). The 2025-11-25 equivalents were checked for tools, elicitation, progress, logging, and lifecycle. The versioned schema used for field names is [schema.json](../schema/2026-07-28/schema.json).

The 2026 tools page states: “This can be any JSON value (object, array, string, number, boolean, or null) that conforms to the tool's `outputSchema` if one is defined.” It also says: “For backwards compatibility, a tool that returns structured content SHOULD also return the serialized JSON in a TextContent block.” The implementation preserves the value without wrapping scalars and mirrors structured values into text by default.

The same page states: “Servers MUST provide structured results that conform to this schema.” Invalid arguments for a known tool are therefore treated as tool execution errors (`isError: true`) so the caller can self-correct; unknown tools use the specification's invalid-params protocol error (`-32602`). A server output that violates its declared schema is an internal defect: this implementation returns a generic `-32603` and records details only in the server log.

The 2026 MRTR page defines `inputRequests` as a map of server-assigned IDs to request objects and `inputResponses` as responses keyed by those same IDs. It says: “If a client request contains a `requestState` field, servers MUST treat `requestState` as an attacker-controlled input.” It further recommends binding integrity-protected state to the authenticated principal, expiry, method, and salient request parameters. Clients must echo the exact state and use a different JSON-RPC ID for the retry. At least one of `inputRequests` or `requestState` must be present in an input-required result.

The elicitation capability is per-request in 2026 (`_meta.io.modelcontextprotocol/clientCapabilities`) and initialization-negotiated in 2025-11-25. The 2026 page states: “Servers MUST NOT use form mode elicitation to request sensitive information such as passwords, API keys, access tokens, or payment credentials” and “Servers MUST use URL mode for interactions involving such sensitive information.” The implementation restricts form schemas to the schema-defined flat primitive subset.

Progress tokens must be strings or integers, and the 2026 progress page says: “The `progress` value MUST increase with each notification.” Notifications must only reference a token from an active request and stop when that request completes. Modern progress and logging notifications stay on the response stream for their request.

The 2026 logging page says a server “MUST NOT emit `notifications/message` for a request that does not include” `io.modelcontextprotocol/logLevel`; with a requested level, messages are scoped to that request's response stream. Logging is deprecated in 2026-07-28 but remains in the schema. The implementation retains the API as deprecated and keeps legacy `logging/setLevel` separate from modern request metadata.

The 2026 Subscriptions page permits a successful completion response when the server closes a `subscriptions/listen` stream itself; see the quote and gate-window clarification in [3D_EVIDENCE.md](3D_EVIDENCE.md).

The original Phase 3B HTTP server included the Asio/llhttp socket listener,
JSON responses, request-scoped SSE, cancellation on disconnect, hardening and
authentication/route seams. Phase 3D adds the incremental SSE parser and HTTP
client, modern `subscriptions/listen`, legacy GET-stream notifications,
reconnecting client subscriptions, and optional OpenSSL TLS support. Prompt and
resource notifications remain hooks only until their corresponding features are
implemented.

## Phase 3D transport and subscriptions review (2026-10-18)

The 2026-07-28 Streamable HTTP and Subscriptions pages, and the 2025-11-25 and
2025-03-26 transport overviews were re-read alongside
`schema/2026-07-28/schema.json`. The versioned Streamable HTTP pages for 2025
are contained in their respective transport overview pages; the split
`/streamable-http` URLs are not published for those versions.

The 2026-07-28 Streamable HTTP page says:

> “Every JSON-RPC message sent from the client MUST be a new HTTP POST request
> to the MCP endpoint.”

> “The client MUST include an `Accept` header listing both
> `application/json` and `text/event-stream` as supported content types.”

> “If the body is a JSON-RPC request, the server MUST return either
> `Content-Type: application/json` (a single JSON object) or
> `Content-Type: text/event-stream` (an SSE response stream). The client MUST
> support both.”

The same page says:

> “Long-lived notification streams are obtained by sending a
> [`subscriptions/listen`][subscriptions-listen] request.”

> “Resumable SSE streams via `Last-Event-ID` are not supported.”

The 2026-07-28 Subscriptions page says:

> “The server MUST NOT send notification types the client has not explicitly
> requested.”

> “The server MUST send `notifications/subscriptions/acknowledged` as the first
> message carrying the subscription's ID in `_meta` under
> `io.modelcontextprotocol/subscriptionId`, and MUST NOT send any notification
> on the subscription before it.”

> “All notifications delivered on the stream carry
> `io.modelcontextprotocol/subscriptionId` in `_meta`.”

Its example acknowledgement contains `params._meta` with the subscription ID
and `params.notifications` with the accepted subset. The schema confirms that
`SubscriptionsListenRequest` requires `id`, `jsonrpc`, `method`, and `params`;
`SubscriptionsListenRequestParams` requires both `_meta` and `notifications`;
and `SubscriptionFilter` defines optional boolean fields
`toolsListChanged`, `promptsListChanged`, and `resourcesListChanged`, plus an
optional string-array `resourceSubscriptions`.

The prompt says a listen stream “never” carries a JSON-RPC response. The
normative Subscriptions page has a graceful-closure exception: the server
SHOULD send a successful completion response for the original request before
closing a subscription itself. Normal long-lived delivery is notification-only;
graceful server-initiated closure must follow the specification.

The 2026-07-28 transport page explicitly removes GET streams and
protocol-level sessions. Those remain legacy behavior for earlier
Streamable HTTP versions. The 2025 transport material must not be applied to
modern requests.

The earlier transport overviews state:

> “This transport uses HTTP POST and GET requests.”

The 2025-11-25 transport additionally says:

> “The client MUST respect the `retry` field, waiting the given number of
> milliseconds before attempting to reconnect.”

> “If the client wishes to resume after a disconnection (whether due to network failure
> or server-initiated closure), it SHOULD issue an HTTP GET to the MCP endpoint,
> and include the `Last-Event-ID` header to indicate the last event ID it received.”

These are versioned legacy rules, not modern requirements. In particular,
legacy resumption and retry handling must remain separate from modern listen
streams, which do not support `Last-Event-ID`.
