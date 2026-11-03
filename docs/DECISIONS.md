# Project decisions

## Phase 4

- Use jsoncons 1.10.0 for JSON Schema validation. Upstream documents Draft
  2020-12 support, a header-only C++ API, and immutable, thread-safe compiled
  schemas; its license is Boost Software License 1.0. FetchContent pins the
  v1.10.0 source archive by SHA-256
  `c829350a2eece662beb378143e2ab1c1cbb07d703687bf168626665a8a7d8bc1`.
  This avoids the AGPL-licensed alternative and avoids implementing JSON
  Schema semantics locally.
- Patch jsoncons's schema `pattern` and `patternProperties` validators to use
  RE2 2023-03-01, fetched from its upstream archive with SHA-256
  `7a9a4824958586980926a300b4717202485c4b4115ac031822e29aa4ef207e48`.
  The configure-time patch is version-guarded by expected source markers and
  leaves non-schema format validation unchanged. RE2 is installed and exported
  as a required CMake dependency of the SDK.
- Compile input and output schemas at tool registration and retain the
  compiled validators. External `$ref` is rejected; local fragment references
  are bounded and evaluated without fetching remote resources.
- A known tool with invalid arguments returns a tool execution result with
  `isError: true`; an unknown tool remains a protocol error with `-32602`.
- A handler result that violates `outputSchema` is treated as an internal
  server defect. The server logs the detailed validation failure and returns a
  generic `-32603` response without forwarding the invalid value.
- `structuredContent` preserves the handler's original JSON value, including
  scalar and array values. `ToolResult::structured` also emits the same JSON in
  a text content block by default for compatibility.
- `--validate-output` defaults to `on`; applications can disable server-side
  output validation explicitly. Input validation is always enabled.
- Pin the official JSON Schema Test Suite at commit
  `89e83a74014617ffbe79a56d83d14d054cd11417`, archive SHA-256
  `70e238f62ee2d5143e9b292109dbf054f5055d31111ae20f75af754807c5323a`.
  Each Draft 2020-12 category is a CTest case. Report policy exclusions as skips,
  not passes or generic backend failures. Regex matching uses the bounded RE2
  engine; Unicode properties supported by the pinned RE2 release are available.
- `IRequestStateCodec` separates continuation state from transport. The default
  HMAC codec binds the original operation/principal, has a ten-minute TTL and
  supports key rotation. Claims are signed, not encrypted or single-use.
- Reuse handlers for modern MRTR and true legacy server-initiated requests.
  HTTP correlates response POSTs by session/request ID; stdio keeps its reader
  free with worker dispatch. Modern HTTP rejects incoming server RPC requests.
- Parse HTTP SSE incrementally so callbacks execute while an operation runs.
  Serialize logging-enabled and legacy calls where the wire cannot correlate
  multiple application callbacks unambiguously.
- OpenSSL 3 Crypto is a required core dependency for request-state authentication;
  TLS remains a separate build option. Installed CMake consumers resolve Crypto
  explicitly even when HTTP TLS is disabled.
- Phase 5's example `FeatureRegistry` remains an adapter-level registry, not a
  public `Server::prompt/resource` builder API. It uses mutex-protected
  snapshots during dispatch and preserves registration order. Resource reads
  prefer exact static URIs, then registered templates in order. Pagination
  cursors are method- and registry-scoped; mutable registry ordering remains a
  documented limitation for offset cursors.
- The example URI-template matcher prioritizes deterministic bounded matching
  for its supported demo forms. It is not a full RFC 6570 implementation.
  Windows file-backed reads likewise do not yet provide POSIX-equivalent
  handle-relative symlink/TOCTOU guarantees; see the security guide.
- Docker's default seccomp profile blocks TSan's process-local ASLR setup in this
  environment. Sanitizer verification uses a disposable, unexposed test container
  with `--security-opt seccomp=unconfined`, not a host security-setting change.
- GCC's optimized libstdc++ regex implementation emits a false-positive
  `-Wmaybe-uninitialized` diagnostic in the ASan/UBSan build. Rather than
  downgrading that warning, compile only `json_schema.cpp` without optimization
  in that sanitizer configuration; sanitizer instrumentation and strict warning
  errors remain enabled.

## Phase 1

- Use nlohmann/json 3.12.0 through CMake FetchContent, pinned by archive hash.
  The public API uses its JSON value type; installed packages include the
  matching headers so consumers do not need a separately-installed JSON package.
- Keep the official versioned MCP JSON schema in the source tree. Tests compare
  the JSON-RPC message contracts and exact reserved MCP error codes against the
  schema. Generated C++ declarations for the full schema are deferred until
  the API surface is established.
- Build static and shared libraries from the same sources by default. The
  public installed CMake targets are `cppmcp::core` and
  `cppmcp::core_shared`.
- Provide one Bash build script with a required `linux`, `macos`, or `windows`
  platform argument. Windows is supported when the script is run from
  Git Bash/MSYS2 with a CMake-capable toolchain; a `.sh` script is not directly
  executable by native PowerShell or Command Prompt. The script currently
  builds natively and rejects a target platform that does not match its host.
- The build script resolves the pinned dependency at CMake configure time;
  `--no-deps` is an offline mode and works only if the dependency is already in
  the CMake FetchContent cache.
- Keep all non-foundation work in the delivery plan's later phases. These
  first-phase APIs do not claim transport, server, client, auth, app, or
  conformance support.

## Deferred decisions

- Whether async handlers will use C++20 coroutines or futures.
- Executor and transport implementation choices beyond the JSON dependency.
- Binary package format and platform-specific dependency deployment beyond the
  current archive/install packaging script.
- Which deprecated current-era features are exposed in addition to legacy
  compatibility. Any feature retained by the live protocol will follow its
  current schema and wire pattern rather than being forced onto the legacy
  pattern.

## HTTP handler foundation

- The socket-free `McpHttpHandler` remains linked as `cppmcp::transport_http`
  for embedding. The same target also contains the Asio/llhttp network adapter,
  which supplies real exchanges to that handler.
- This handler implements the 2026-07-28 request shape only. Legacy session
  behavior is kept separate because the current revision removed both GET
  streams and protocol sessions.
- `automatic` response mode returns JSON unless only SSE is acceptable, or a
  tool emits a request-scoped event, in which case the response switches to
  SSE. `json_only` rejects emissions through the tool context; `sse_only`
  starts an SSE response before dispatch. Final JSON-RPC responses are SSE
  `data:` events when the stream has started.
- The `ITransport` cancellation default sends `notifications/cancelled`.
  Transports that close an individual response stream override the strategy
  and `cancel_request`; closing an entire client transport is not an acceptable
  substitute for per-request cancellation.
- `ToolDefinition::header_param(name, required)` declares a string, integer,
  or boolean input property mirrored as `Mcp-Param-<name>`. Its
  `x-mcp-header` annotation stores only the name suffix. Registration
  validates the property and adds required parameters to the schema's
  `required` list. Request validation decodes the mirrored value, converts
  primitive arguments to their specified string form, and requires a match;
  mirrored integers are restricted to JavaScript's safe range. Declarations
  live on each `Server` instance; there is no process-global tool registry.

## Phase 3B HTTP server

- The network adapter uses standalone Asio 1.30.2 and llhttp 9.2.1, each
  fetched from a versioned archive with a fixed SHA-256. llhttp's C sources are
  compiled into the HTTP transport so installed consumers do not need a
  separately-installed parser library.
- The listener uses one worker thread per accepted connection and runs the
  existing `McpHttpHandler` for each parsed request. TLS, legacy sessions, and
  client-side HTTP remain out of scope.
- `Server::start_http` returns a stoppable RAII handle. `Server::run_http`
  blocks until SIGINT or SIGTERM and then shuts down the listener and active
  connections.
- Request hardening uses bounded header/body sizes and absolute header/body
  deadlines, plus an idle connection deadline. Non-loopback binds require
  explicit opt-in; deployments using public hostnames configure
  `HttpHandlerOptions::allowed_hosts`.
- HTTP status mapping is explicit: malformed requests and mirrored-header
  mismatches use 400; authentication uses 401; denied Host/Origin uses 403;
  unmatched routes use 404; unsupported methods use 405 with `Allow: POST`;
  unacceptable response formats use 406; read deadlines use 408; oversized
  bodies use 413; invalid content types use 415; oversized headers use 431;
  handler exceptions are logged and return 500.
- The HTTP options expose an authenticator callback and a route callback.
  Bearer-token validation is an optional built-in authenticator; the custom
  callback can add application-specific authentication.
- `docs/QUICKSTART_HTTP.md` documents the example server and its command-line
  options. `scripts/verify_install.sh` validates installed headers and exported
  CMake targets with a separate downstream consumer.
- Three optional Clang/libFuzzer targets cover mirrored-header decoding,
  `Mcp-Name`/`Mcp-Param-*` value decoding, and llhttp request-target/header
  parsing. The Linux CI job runs bounded fuzz smoke tests and an HTTP
  performance smoke; neither is a throughput benchmark or a proof of
  unbounded fuzz coverage.

## Phase 3D HTTP client foundation

- The HTTP client lives in `cppmcp::transport_http` and uses Asio plus llhttp
  response parsing. It negotiates the stateless 2026 protocol first, with an
  explicit modern-only or legacy-only policy and a legacy initialize fallback
  in prefer-modern mode.
- HTTP client authentication is a provider seam. The built-in static bearer
  provider attaches credentials only to request headers; challenge details
  are represented separately from token material.
- SSE parsing is incremental and has a configurable event-byte ceiling.
  Incomplete trailing events at end-of-stream are discarded in accordance
  with the event-stream parsing model.
- Modern change streams use `subscriptions/listen` POST requests and acknowledge
  the accepted subset before any event. Only the example's advertised
  `tools.listChanged` capability is accepted today; prompt/resource change
  hooks exist but those features and capabilities are deferred to Phase 5.
- Legacy GET notification streams remain session-scoped and are not used by
  the modern transport. The client reconnects after loss using bounded
  exponential backoff with jitter and signals callers to refresh their state.
- OpenSSL 3 enables optional TLS by default when found. Server TLS defaults to
  TLS 1.2 minimum; clients verify the trust chain and DNS/IP identity unless
  the explicitly named `dangerous_disable_verification` library option is
  selected. That option is not exposed by the example CLI.
- A remote bind must be protected by TLS or an authenticator unless
  `allow_insecure_remote` explicitly opts out. The example server's
  `--allow-insecure-remote` flag is intentionally noisy in deployment guidance.

## Phase 6 OAuth

- OAuth is opt-in and limited to HTTP. Stdio continues to obtain credentials
  from application/environment configuration.
- Resource-server verification is audience-bound to the canonical resource
  URI. The verified `AuthContext` intentionally has no raw-token accessor;
  applications must use `ITokenExchanger` for a separate upstream token rather
  than forwarding the MCP credential.
- Scope requirements are evaluated after request parsing and before dispatch.
  `mcp:use` is the global default; the example hierarchy is explicit and
  transitive. Listing defaults to `show_all` so clients can discover operations
  and step up, with `filter` available where name disclosure is undesirable.
- OAuth metadata, JWKS, introspection, registration, and CIMD fetches share
  `NetworkPolicy`: DNS is resolved once, addresses are checked before
  connection, redirects are not followed automatically, and loopback HTTP is
  opt-in for tests only.
- Registrations are keyed by exact issuer; tokens by issuer, resource, and
  client ID. The file store uses atomic replacement and restrictive mode, with
  optional AES-256-GCM encryption. OS keychain integration remains deferred.
- The client-credentials grant is available with client-secret authentication
  and resource binding, but private-key JWT is not implemented. The 2026-07-28
  optional ext-auth text recommends private-key JWT and has differing wording
  about client-secret transport versus the prompt's Basic requirement. The
  current API supports the prompt's Basic mode (and configured secret-post);
  it does not claim complete conformance to the optional extension until
  private-key JWT assertion generation is added.
- CIMD's HTTPS requirement remains the production rule. A loopback-hosted
  document is accepted only behind the explicit client test option for local
  fixtures; it is not a production deployment pattern.
