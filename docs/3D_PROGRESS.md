# Phase 3D progress

Updated 2026-10-18.

| Slice | Status | Gate result |
| --- | --- | --- |
| 1 — plain HTTP client, SSE parser, auth hook | Complete | `RESULT: 20 passed, 0 failed, 0 warnings` |
| 2 — subscriptions/listen and notifications | Complete | `RESULT: 29 passed, 0 failed, 0 warnings` |
| 3 — TLS | Complete | `RESULT: 40 passed, 0 failed, 0 warnings` |
| 4 — fuzzing, conformance, interop, evidence | Pending | Not run |

## Last gate tail

```text
PASS: our client rejects an untrusted CA by default
PASS: client accepts IP SAN match
PASS: client rejects hostname mismatch with a clear error
PASS: client accepts DNS SAN match
PASS: non-loopback bind without TLS/auth refused
PASS: non-loopback bind allowed with TLS

RESULT: 40 passed, 0 failed, 0 warnings
```

The first gate attempt could not execute the non-executable script directly
(exit 126); invoking it through Bash worked. The gate script itself has not been
edited. Slice 2 verified modern acknowledgment-first streams, tagged
`tools/list_changed` delivery, legacy GET-stream notifications, client loss and
reconnect callbacks, and notification resumption. TLS was checked against a
locally generated CA for trusted/untrusted chains, DNS/IP SANs, and listen
streams. A SIGTERM test uncovered an SSE disconnect-watch null dereference; the
watch is now registered before the SSE writer starts. The same active-listener
shutdown scenario exits cleanly under ASan+UBSan after the fix.

The regression suite after TLS changes passed all 55 discovered tests.

## Next action

Add TLS truncation/expiry and listen/backoff coverage, increase the test suite to
the gate's required volume, then run sanitizer, conformance, interop, and
documentation/evidence checks with `scripts/verify_3d.sh --slice 4 --sanitizers
--interop`.
