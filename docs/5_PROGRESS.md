# Phase 5 Progress

## Status

Phase 5B slice 1 and 2 are implemented and committed as `b53f58d` and
`92ca27b`. Slice 3 is complete. The final Phase 5B gate passed; Phase 6 was
not started as part of this work.

The documented Phase 5 SDK gaps closed in these slices are: public server
prompt/resource/template builders, RFC 6570 templates, stable registration
cursors, typed tool results without an in-band sentinel, an installed-package
consumer, prompt/resource/completion client calls, client caching and HTTP
subscription re-establishment, a public file provider, linear-time schema
patterns through pinned RE2, protected request-state key file/environment
configuration, and reproducible conformance fixtures.

Explicitly accepted limitations:

- The published MCP conformance CLI has no `2026-07-28` profile. The recorded
  official results therefore cover selected `2025-11-25` compatibility
  scenarios and do not claim full modern-profile conformance.
- Typed `InputRequired` continuation handling is complete for tool calls.
  Prompt/resource handlers can return the typed outcome, but automatic
  prompt/resource continuation retries are not claimed by this phase.
- Windows file reads fail closed unless `allow_unsafe_windows_sandbox` is
  explicitly enabled; that compatibility opt-in does not provide the POSIX
  handle-relative symlink guarantee.

## Final verification

```text
CTest discovery: 342 tests
Full CTest suite: 100% passed, 0 failed; 3 external-schema categories skipped by policy
Phase 5B slice 1 gate: 47 passed, 0 failed, 0 warnings
Phase 5B slice 2 gate: 73 passed, 0 failed, 0 warnings
Phase 5B slice 3 full gate: 90 passed, 0 failed, 0 warnings
RE2 schema tests and typed client tests: passed
Installed-package consumer: passed, including typed client calls over MemoryTransport
12 Clang/libFuzzer targets: built; each completed 100 smoke runs without a crash
Official 2025-11-25 conformance scenarios: six targeted scenarios passed
AddressSanitizer/UndefinedBehaviorSanitizer: full suite passed, zero findings
ThreadSanitizer: full suite passed, zero findings
Inspector interoperability: templates/list advertises operator templates
```

The combined gate's targeted `resources-list` conformance scenario also passed
with 0 failures and 0 warnings. Full evidence is recorded in
[`5B_EVIDENCE.md`](./5B_EVIDENCE.md).

## Next action

Phase 5B is complete. Phase 6 remains outside the scope of this execution.
