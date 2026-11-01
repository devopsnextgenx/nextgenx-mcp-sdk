# Phase 5B Evidence

## Closed and accepted gaps

The Phase 5 SDK gaps closed by this work include public server feature
builders, RFC 6570 template expansion/matching, stable registration-sequence
cursors, typed tool results without an in-band sentinel, an installed-package
consumer, prompt/resource/completion client APIs, bounded client caching,
resource subscription re-establishment, a public file provider, linear-time
schema patterns, request-state key file/environment configuration, and a
dedicated conformance fixture.

The following limitations are explicitly accepted:

- The published conformance CLI has no `2026-07-28` profile. Only targeted
  `2025-11-25` compatibility scenarios are claimed.
- Typed `InputRequired` continuation handling is complete for tool calls.
  Prompt/resource handlers can return typed input-required outcomes, but
  automatic prompt/resource continuation retries are not claimed.
- Windows file reads fail closed unless `allow_unsafe_windows_sandbox` is
  explicitly enabled. The opt-in does not provide the POSIX handle-relative
  symlink guarantee.

## Build, tests, and package consumer

The Phase 5B slice gates reported:

```text
Slice 1: RESULT: 47 passed, 0 failed, 0 warnings
Slice 2: RESULT: 73 passed, 0 failed, 0 warnings
```

The Phase 5 regression gate passed with 81 passed, 0 failed, and 0 warnings.
CTest discovers 342 tests; the full run reports 100% passed, 0 failed, with
three external JSON Schema categories skipped by policy (`defs`, `refRemote`,
and `vocabulary`).

`scripts/verify_install.sh build` installed the package, built both standalone
consumers, and executed them successfully. The API consumer uses only installed
public headers and verifies prompt/resource/template builders, completion,
pagination, typed client calls over `MemoryTransport`, and typed tool results.

## Schema-pattern safety

The jsoncons schema `pattern` and `patternProperties` paths use RE2 2023-03-01,
pinned by archive SHA-256
`7a9a4824958586980926a300b4717202485c4b4115ac031822e29aa4ef207e48`.
`regex_redos_budget` validates nested-quantifier and overlapping-alternation
inputs within its measured budget. Invalid RE2 syntax is rejected during
schema compilation. Pattern size and input size limits remain enabled.

## URI templates and cursors

The official URI-template test data is pinned at commit
`4171dac22aa67fc710b3f6df308a50bd08552986`. The full vector test passes, and
117 positive official expansion vectors are independently registered as
CTest cases alongside negative, ambiguity, round-trip, and server-builder
coverage. Black-box HTTP checks verify RFC operators, one-time percent
decoding, non-matches, and stable cursor validation.

## Conformance

The dedicated `cppmcp_conformance_server` fixture was tested with the published
`2025-11-25` CLI. `resources-list`, `resources-read-text`,
`resources-templates-read`, `prompts-list`, `prompts-get-simple`, and
`tools-list` each reported 1/1 passed, 0 failed, and 0 warnings. The exact
commands and historical full-profile triage are recorded in
[`CONFORMANCE.md`](./CONFORMANCE.md).

## Fuzzing

All 12 Clang/libFuzzer targets build, including URI-template, pagination
cursor, and client-cache targets. Each target completed 100 smoke runs without
a crash.

## Sanitizers and interoperability

The final combined `scripts/verify_5b.sh --slice 3 --sanitizers --interop`
gate completed with 90 passed, 0 failed, and 0 warnings. The full 342-test
suite passed under both AddressSanitizer/UndefinedBehaviorSanitizer and
ThreadSanitizer with zero findings. The installed-package consumer passed
under both sanitizer configurations. The Inspector interoperability check
confirmed that `templates/list` advertises the operator templates.

The gate's targeted `resources-list` run against the `2025-11-25`
conformance profile reported 1/1 passed, 0 failed, and 0 warnings.
