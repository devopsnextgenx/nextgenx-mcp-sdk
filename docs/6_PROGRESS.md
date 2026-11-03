# Phase 6 Progress

## Status

Slice 1 and the carry-over are complete. Slice 1 adds a test-only OAuth
authorization server, path-inserted/root Protected Resource Metadata, bearer
challenges, static/JWT/introspection verifier interfaces, a bounded JWKS
refresh path, outbound DNS-pinned network policy, request `AuthContext`,
legacy session principal binding, and auth CLI configuration guards. The
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

`scripts/verify_6.sh --slice 1`:

```text
  PASS: unknown-kid flood caused only 0 JWKS fetches (rate limited)
  PASS: access token never logged
-- legacy sessions with auth
  PASS: legacy initialize without token -> 401
  PASS: legacy initialize with token succeeds
  PASS: legacy follow-up request without token -> 401 (token required on every request)
  PASS: another principal cannot use alice's session (404)
  PASS: owner can continue the session
-- introspection (opaque tokens)
  PASS: opaque token accepted via introspection
  PASS: revoked opaque token rejected after cache TTL
  PASS: introspection: wrong audience rejected
  PASS: introspection outage fails closed (503)
-- configuration guards and stdio
  PASS: refuses http:// non-loopback issuer without --auth-insecure-http-for-testing
  PASS: stdio server unaffected by HTTP auth
RESULT: 45 passed, 0 failed, 0 warnings (logs: /tmp/tmp.AODMZPw7aw)
```

The full post-slice CTest run passed:

```text
100% tests passed, 0 tests failed out of 343
```

## Next action

Implement slice 2 scope requirements, authorization context propagation,
single-challenge aggregation, listing policy and token-expiry-bound streams.
