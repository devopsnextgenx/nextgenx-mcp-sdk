# Phase 6 Progress

## Status

Slices 1 and 2 and the carry-over are complete. Slice 1 adds a test-only OAuth
authorization server, path-inserted/root Protected Resource Metadata, bearer
challenges, static/JWT/introspection verifier interfaces, a bounded JWKS
refresh path, outbound DNS-pinned network policy, request `AuthContext`,
legacy session principal binding, and auth CLI configuration guards.
Slice 2 adds static and dynamic scope requirements to tool, prompt, resource
and template builders; transitive scope hierarchy; one aggregated insufficient
scope challenge; optional filtered listings; and stream closure at token
expiry. The HTTP handler parses JSON before authentication, applies operation
authorization before dispatch, and re-authorizes every continuation. The
prompt/resource typed client MRTR continuations use a registered
`ElicitationHandler`, validate form schemas, and share the tool-call round
limit error behavior.

The shared network policy explicitly blocks IPv4-mapped IPv6 destinations by
default; enabling mapped addresses is a separate opt-in from the loopback-only
HTTP testing exception. The public auth API also includes the
`ITokenExchanger` interface for obtaining a distinct upstream credential.

The Phase 5B commit is present at `2078252`; the carry-over commit is
`89a1ae3`. Starting CTest baseline was 342 passing tests; current CTest has 343
passing tests. The working tree was not clean at task start because the
provided `scripts/verify_6.sh` was untracked; it has been left untouched and
excluded from commits.

## Latest verification

`scripts/verify_6.sh --slice 2` (includes slice 1):

```text
  PASS: scope hierarchy: projects:admin implies projects:write
  PASS: resource template requires files:read
  PASS: files:write implies files:read
  PASS: static resource scope enforced
  PASS: show_all listing exposes tools needing more scope (step-up is discoverable)
  PASS: AuthContext visible in handlers (sub, client_id, scopes)
  PASS: raw token not exposed to handlers
  PASS: authorization re-checked on the retry (scope dropped -> 403)
  PASS: --auth-listing filter hides tools the token cannot call
  PASS: listen stream closed by the server at token expiry (4s)
  PASS: listen without token -> 401
RESULT: 65 passed, 0 failed, 0 warnings (logs: /tmp/tmp.y5C2Fy0dVZ)
```

Focused scope tests and full CTest passed:

```text
2/2 focused scope tests passed
100% tests passed, 0 tests failed out of 345
```

## Next action

Implement slice 3 OAuth client discovery, registration, PKCE, token storage,
refresh/single-flight, step-up and `client_credentials`.
