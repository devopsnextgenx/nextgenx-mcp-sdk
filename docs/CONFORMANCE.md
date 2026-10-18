# Phase 3D conformance results

## Tool and protocol version

The official `@modelcontextprotocol/conformance` CLI was installed and run
using the current server command form:

```sh
npx -y @modelcontextprotocol/conformance server \
  --url http://127.0.0.1:<port>/mcp \
  --spec-version 2025-11-25 --verbose
```

The published CLI available from npm lists only `2025-03-26`, `2025-06-18`,
`2025-11-25`, `draft`, and `extension`. Requesting
`--spec-version 2026-07-28` returned `Unknown spec version`; listing draft
server scenarios returned no scenarios. The tool therefore cannot provide
conformance evidence for the 2026-07-28 transport revision in this run. The
2025 suite is useful for checking the legacy compatibility endpoint, but is not
a conformance suite for the modern stateless transport.

The `active` server suite ran all 30 scenarios against each mode. The official
suite reports checks within a scenario; the table reports scenario status. A
scenario with multiple checks can have partial checks pass while the scenario
still fails.

| Scenario | Modern | Legacy | Dual | Gap / explanation |
| --- | --- | --- | --- | --- |
| `server-initialize` | Fail | Pass | Pass | Modern-only correctly rejects the 2025 initialize lifecycle; 2026 scenario unavailable. |
| `logging-set-level` | Fail | Fail | Fail | Logging control is deferred to Phase 4. |
| `ping` | Fail | Pass | Pass | Legacy endpoint supports ping; modern run used unsupported 2025 lifecycle. |
| `completion-complete` | Fail | Fail | Fail | Completions are outside this phase. |
| `tools-list` | Fail | Pass | Pass | Modern run used unsupported 2025 lifecycle. |
| `tools-call-simple-text` | Fail | Fail | Fail | Harness requires its `test_simple_text` fixture tool; the example exposes `add`, `echo`, and demonstration tools. |
| `tools-call-image` | Fail | Fail | Fail | Harness-specific tool/content fixture is not exposed. |
| `tools-call-audio` | Fail | Fail | Fail | Harness-specific tool/content fixture is not exposed. |
| `tools-call-embedded-resource` | Fail | Fail | Fail | Resource content and its harness fixture are Phase 5. |
| `tools-call-mixed-content` | Fail | Fail | Fail | Harness-specific tool/content fixture is not exposed. |
| `tools-call-with-logging` | Fail | Fail | Fail | Logging is deferred to Phase 4; harness fixture is absent. |
| `tools-call-error` | Fail | Fail | Fail | Harness-specific failing tool fixture is not exposed. |
| `tools-call-with-progress` | Fail | Fail | Fail | Progress support is deferred to Phase 4; harness fixture is absent. |
| `tools-call-sampling` | Fail | Fail | Fail | Sampling round trips are outside this phase. |
| `tools-call-elicitation` | Fail | Fail | Fail | Elicitation is deferred to Phase 4. |
| `elicitation-sep1034-defaults` | Fail | Fail | Fail | Elicitation support is deferred to Phase 4. |
| `server-sse-multiple-streams` | Fail | Pass | Pass | Legacy SSE is supported; modern stateless mode has no legacy GET stream. |
| `elicitation-sep1330-enums` | Fail | Fail | Fail | Elicitation support is deferred to Phase 4. |
| `resources-list` | Fail | Fail | Fail | Resource methods are deferred to Phase 5. |
| `resources-read-text` | Fail | Fail | Fail | Resource methods are deferred to Phase 5. |
| `resources-read-binary` | Fail | Fail | Fail | Resource methods are deferred to Phase 5. |
| `resources-templates-read` | Fail | Fail | Fail | Resource methods are deferred to Phase 5. |
| `resources-subscribe` | Fail | Fail | Fail | Resource methods are deferred to Phase 5; the new subscription filter is not the old RPC. |
| `resources-unsubscribe` | Fail | Fail | Fail | Resource methods are deferred to Phase 5. |
| `prompts-list` | Fail | Fail | Fail | Prompt methods are deferred to Phase 5. |
| `prompts-get-simple` | Fail | Fail | Fail | Prompt methods are deferred to Phase 5. |
| `prompts-get-with-args` | Fail | Fail | Fail | Prompt methods are deferred to Phase 5. |
| `prompts-get-embedded-resource` | Fail | Fail | Fail | Prompt/resource methods are deferred to Phase 5. |
| `prompts-get-with-image` | Fail | Fail | Fail | Prompt methods are deferred to Phase 5. |
| `dns-rebinding-protection` | Partial (1/2 checks) | Pass (2/2) | Pass (2/2) | Modern's legacy-lifecycle subcheck is not applicable; Host/Origin checks pass. |

### Suite totals

| Mode | Passed checks | Failed checks | CLI result |
| --- | ---: | ---: | --- |
| Modern | 1 | 30 | Failed |
| Legacy | 6 | 25 | Failed |
| Dual | 6 | 25 | Failed |

An isolated, sequential run against dual mode separately passed
`server-initialize`, `ping`, and `tools-list`. The same run confirmed
`tools-call-simple-text` fails because the conformance harness requests a
`test_simple_text` tool the example does not register. The full suite also
exercised multiple simultaneous legacy GET streams. A shutdown null dereference
found while running the suite was fixed by registering the disconnect callback
before constructing the SSE writer.

## Scope gaps

- The official published CLI has no 2026-07-28 scenario set yet, so modern
  conformance is not run for the protocol version implemented by modern mode.
- The exposed tools are examples, not the suite's dynamic fixture tools. This
  explains tool scenarios that request harness-only tool names or content.
- Logging, progress, elicitation, completions, prompts, and resources are
  scheduled for later phases; these are not claimed as implemented here.
- The old `resources/subscribe` RPC is distinct from 2026
  `subscriptions/listen`; do not use the 2025 scenario as evidence for the
  modern subscription implementation. Modern listen behavior is exercised by
  the Phase 3D acceptance gate and registry tests.
