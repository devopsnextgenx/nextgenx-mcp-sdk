# Project decisions

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
