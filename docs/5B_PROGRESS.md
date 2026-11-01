# Phase 5B Progress

## Status

Slices 1 and 2 are complete and their gates passed. The Phase 5 regression gate
also passed during both slice gates. Slice 3 is next.

Slice 1 moved the feature registry into the installed public headers, added
`Server` prompt/resource/template builders, typed tool results, RFC 6570
expansion and matching, registration-sequence cursors, public file-provider
headers, and an installed-package consumer test. The operator templates now
use the library matcher. The official URI-template vectors are pinned at
`4171dac22aa67fc710b3f6df308a50bd08552986`.

Slice 2 adds public typed client calls, cursor aggregation, a bounded
TTL-aware `ClientCache`, and re-establishment of HTTP resource subscriptions
after `connect_http()`. Schema `pattern` and `patternProperties` now use RE2
2023-03-01, pinned by SHA-256
`7a9a4824958586980926a300b4717202485c4b4115ac031822e29aa4ef207e48`; the
jsoncons integration is patched at configure time against its expected source
markers. The Windows file provider fails closed unless the explicit unsafe
opt-in is set. The HTTP example accepts request-state keys from a protected
file or `CPPMCP_REQUEST_STATE_KEY`, and a dedicated conformance fixture now
has passing targeted official scenarios.

## Latest verification

`CONFORMANCE_CMD='npx -y @modelcontextprotocol/conformance server --url http://127.0.0.1:8001/mcp --scenario resources-list --spec-version 2025-11-25 --verbose' scripts/verify_5b.sh --slice 2`:

```text
RESULT: 73 passed, 0 failed, 0 warnings
```

`scripts/verify_5.sh --slice 3` passed in the slice 2 gate with 81 passed, 0
failed, and 0 warnings. The installed consumer was also rebuilt and executed
against the installed package, including the typed client API over an in-memory
transport pair.

The last gate output tail was:

```text
  PASS: conformance run reports 0 failures

RESULT: 73 passed, 0 failed, 0 warnings (logs: /tmp/tmp.zkToxk7xyW)
```

## Next action

Complete slice 3: increase CTest discovery to at least 260, reach at least 12
fuzz targets including URI templates/cursors, update evidence and remaining
Phase 5 documentation, and run the sanitizer and interop gates.
