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
- `ToolDefinition::header_param(name, required)` declares a string input
  property mirrored as `Mcp-Param-<name>`. Registration validates the schema,
  marks the property with `x-mcp-header`, and adds required parameters to the
  schema's `required` list. Request validation decodes the mirrored value and
  requires it to match the JSON argument. Mirrored declarations live on each
  `Server` instance; there is no process-global tool registry.

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
