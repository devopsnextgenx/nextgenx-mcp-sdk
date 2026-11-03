# Phase 6 Progress

## Status

Slices 1 through 4 and the accepted carry-over are implemented. The
user-provided `scripts/verify_6.sh` remains untouched and is not included in
commits.

- Carry-over: typed `Client` now drives prompt and resource MRTR continuations
  with the registered elicitation handler and the existing round/error limits.
- Slice 1: resource metadata, bearer challenges, `ITokenVerifier`,
  `AuthContext`, JWT/JWKS and introspection verification, outbound DNS-pinned
  `NetworkPolicy`, HTTP enforcement, session principal binding, configuration
  guards, and the test-only authorization server.
- Slice 2: static/dynamic scope requirements, hierarchy, aggregated 403
  challenges, listing filter option, stream expiry, and request-state
  authenticated subject binding.
- Slice 3: OAuth discovery and registration, authorization-code PKCE,
  client_credentials, RFC 9207 validation, issuer/resource/client-keyed token
  storage, refresh rotation, single-flight, step-up, CLI flow events, repeat
  and parallel calls.
- Slice 4: shared bounded metadata/scope parsing, peer-keyed failed-auth
  limiter hook, 15 fuzz targets, 423 CTest
  cases, OAuth and security guides, spec notes, evidence, and interop records.

The Phase 5B commit at `2078252` was present at start. The implementation
commits through slice 3 are `89a1ae3`, `199642b`, `470ac8e`, and `56dd37e`.
The original CTest baseline was 342; current CTest discovery and execution
report 423 tests.

## Final verification

`timeout 1800 scripts/verify_6.sh --slice 4 --sanitizers --interop </dev/null`
passed with `RESULT: 134 passed, 0 failed, 2 warnings`. The warnings are
recorded in [6_EVIDENCE.md](6_EVIDENCE.md): Inspector without credentials
returned text not recognized by the gate's 401 matcher, and Claude Code OAuth
login requires a manual interactive run.

Both address/undefined and thread sanitizer suites passed. All 15 fuzz targets
built with Clang; the new JWT, OAuth metadata, and scope-set targets each
completed 1,000 smoke runs. The Phase 5 regression gate passed 107 checks.
The Phase 5B regression gate passed 86 checks with one warning because the
optional external conformance command was not configured. See
[6_EVIDENCE.md](6_EVIDENCE.md) for commands and complete results.

## Remaining interop limitation

The MCP Inspector bearer-header check passed. The required manual Claude Code
OAuth login was not run because it requires interactive browser/consent
authorization; previous Claude Code entries in [INTEROP.md](INTEROP.md) verify
connection health only.
