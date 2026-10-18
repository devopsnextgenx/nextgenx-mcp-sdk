# cppmcp

cppmcp is an original C++20 SDK for MCP, developed in reviewable phases. It
provides the JSON-RPC core, version negotiation, server/client dispatch, stdio
transport, an in-process Streamable HTTP request handler, and an Asio/llhttp
HTTP server.

## Requirements

- CMake 3.24 or newer
- GCC 12+, Clang 15+, or MSVC 2022
- C++20-capable standard library
- Network access on the first configure to fetch the pinned nlohmann/json,
  Asio, and llhttp dependencies (or an existing CMake FetchContent cache for
  offline builds)

## Build, test, and package

The same shell script accepts `linux`, `macos`, or `windows` as its first
argument. For Windows, run it from Git Bash or MSYS2 with the required compiler
and CMake toolchain configured. Builds are native to the current host; the
script rejects a different requested target platform rather than producing a
mislabelled binary.

```sh
./scripts/build.sh linux --deps --shared --static --package
```

`--deps` resolves pinned dependencies during CMake configuration. `--shared`
and `--static` select library variants; if neither is passed, both are built.
`--package` stages installable headers, CMake package files, and built libraries
under `dist/` and writes a platform archive. `--no-deps` requests an offline
configure using the existing FetchContent cache.

For direct CMake use:

```sh
cmake -S . -B build -DCPPMCP_BUILD_SHARED=ON -DCPPMCP_BUILD_STATIC=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix install
```

Installed consumers use `find_package(cppmcp CONFIG REQUIRED)` and link
`cppmcp::core` (static) or `cppmcp::core_shared` (shared).

## Phase 1 API

```cpp
#include <cppmcp/core/json_rpc.hpp>
#include <cppmcp/core/version.hpp>

const auto parsed = cppmcp::core::parse_message(
    R"({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{}})");
```

The core models requests, notifications, results, errors, and request IDs;
rejects malformed JSON-RPC messages, null IDs, non-object MCP params, and
batches; and provides modern/legacy version recognition and version-selection
helpers.

See [SPEC_NOTES.md](docs/SPEC_NOTES.md) for the sources reviewed and
[DECISIONS.md](docs/DECISIONS.md) for assumptions and deferred choices.

## Streamable HTTP server

Link `cppmcp::transport_http` to use the modern HTTP server:

```cpp
#include <cppmcp/transport/http_server.hpp>

cppmcp::Server server;
cppmcp::transport::HttpOptions options;
options.port = 8080;
auto running = server.start_http(options);
```

The `http_server` example exposes `echo`, `add`, `slow_count`, `emit_demo`, and
`weather_like` tools:

```sh
./build/cppmcp_http_server --port 8000
```

The in-process `McpHttpHandler` remains available for applications that supply
their own HTTP exchange adapter. TLS, `subscriptions/listen`, legacy HTTP
sessions, and an HTTP client remain unimplemented.

Tools that need request-scoped streaming or disconnect cancellation can use the
context-handler overload of `Server::add_tool` and call `ToolContext::emit`:

```cpp
server.add_tool(tool_definition,
    [](const nlohmann::json& args, const nlohmann::json&, cppmcp::ToolContext& ctx) {
        ctx.emit({{"type", "progress"}, {"step", 1}});
        return nlohmann::json{{"done", !ctx.cancelled()}};
    });
```

In automatic response mode, the first emission starts chunked SSE; the final
JSON-RPC result is sent as the last `data:` event. A disconnected peer requests
stop through the context token. The outbound SSE queue is bounded and closes a
slow consumer that exceeds its configured limit.
