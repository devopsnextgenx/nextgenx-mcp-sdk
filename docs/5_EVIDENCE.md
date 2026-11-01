# Phase 5 Evidence

## Conformance CLI

Ran the published MCP conformance CLI against the Phase 5 legacy HTTP server:

```text
$ npx -y @modelcontextprotocol/conformance server --url http://127.0.0.1:45732/mcp --spec-version 2025-11-25 --verbose
✓ resources-list: 1 passed, 0 failed
✗ resources-read-text: 0 passed, 1 failed
✗ resources-read-binary: 0 passed, 1 failed
✗ resources-templates-read: 0 passed, 1 failed
✓ resources-subscribe: 1 passed, 0 failed
✓ resources-unsubscribe: 1 passed, 0 failed
✓ prompts-list: 1 passed, 0 failed
✗ prompts-get-simple: 0 passed, 1 failed
✗ prompts-get-with-args: 0 passed, 1 failed
✗ prompts-get-embedded-resource: 0 passed, 1 failed
✗ prompts-get-with-image: 0 passed, 1 failed
✓ server-initialize: 1 passed, 0 failed
✓ logging-set-level: 1 passed, 0 failed
✓ ping: 1 passed, 0 failed
✓ tools-list: 1 passed, 0 failed
✓ server-sse-multiple-streams: 1 passed, 0 failed
✓ dns-rebinding-protection: 2 passed, 0 failed
Total: 11 passed, 20 failed
```

The command exited 1. Its verbose output did not include assertion details for
the failed fixture-specific reads and prompt gets. The CLI rejected
`--spec-version 2026-07-28` with:

```text
Unknown spec version: 2026-07-28
Valid versions: 2025-03-26, 2025-06-18, 2025-11-25, draft, extension
```

## Unit tests and fuzzers

```text
$ ctest --test-dir build --output-on-failure --timeout 300
100% tests passed, 0 tests failed out of 213
The following tests did not run:
  cppmcp_json_schema_suite_defs (Skipped)
  cppmcp_json_schema_suite_refRemote (Skipped)
  cppmcp_json_schema_suite_vocabulary (Skipped)
```

Built all eleven libFuzzer targets with Clang. Each of the four new targets
(`cppmcp_fuzz_uri_template`, `cppmcp_fuzz_pagination_cursor`,
`cppmcp_fuzz_file_sandbox`, and `cppmcp_fuzz_subscription_filter`) completed
100 generated runs without a crash after invalid UTF-8 resource URIs were
rejected explicitly by the subscription registry.

The first subscription-filter fuzz smoke run found a `nlohmann::json` type
error for invalid UTF-8 passed through the public URI API. The registry now
raises `std::invalid_argument` at canonicalization, and the focused
`subscription_limit` test verifies that boundary.

## Slice gate

The first Slice 4 baseline attempt exposed missing tests/docs/evidence only:

```text
RESULT: 92 passed, 15 failed, 0 warnings
```

After adding the required tests and documentation, the full command
`timeout 600s scripts/verify_5.sh --slice 4 --sanitizers --interop </dev/null`
passed:

```text
PASS: ctest discovers 213 tests
PASS: ctest passes
PASS: >= 11 fuzz targets
PASS: Inspector: prompts/list
PASS: Inspector: resources/list
PASS: Inspector: resources/read
PASS: Inspector: prompts/get
cppmcp-verify: http://127.0.0.1:51231/mcp (HTTP) - ✔ Connected
PASS: Claude Code connected
PASS: sanitizer [address,undefined]: tests pass, zero findings
PASS: sanitizer [thread]: tests pass, zero findings
RESULT: 114 passed, 0 failed, 0 warnings
```

The same gate exercised modern and legacy prompt/resource calls, prompt and
resource MRTR, subscriptions, notification filters, and reconnecting clients.
The TSan suite includes concurrent registry add/remove, prompt-list, resource
read, subscription, update, and notification operations across 100 workers.
