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
