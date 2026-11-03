# Phase 6 Progress

## Status

Slices 1 through 3 and the carry-over are complete. Slice 1 adds a test-only
OAuth authorization server, path-inserted/root Protected Resource Metadata,
bearer challenges, static/JWT/introspection verifier interfaces, a bounded JWKS
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

Slice 3 adds the public `OAuthProvider`, issuer-keyed in-memory/file token and
registration stores, PRM and authorization-server discovery, PKCE authorization
code and `client_credentials` grants, loopback/CIMD/DCR registration,
state/RFC 9207 issuer validation, refresh-token rotation, step-up, and the HTTP
client example's OAuth, repeat, and parallel-call flags. Redirect handling in
the test launcher is explicit and limited to the exact loopback callback URL.

The Phase 5B commit is present at `2078252`; the carry-over commit is
`89a1ae3`. Starting CTest baseline was 342 passing tests; current CTest has 343
passing tests from the earlier slices. The working tree was not clean at task
start because the provided `scripts/verify_6.sh` was untracked; it has been
left untouched and excluded from commits.

## Latest verification

`scripts/verify_6.sh --slice 3` (includes slices 1 and 2):

```text
  PASS: stored token reused across runs (no second authorization)
  PASS: token store file mode 0600
  PASS: store entries are keyed by issuer
  PASS: credentials of AS-A were not reused at AS-B (separate DCR)
  PASS: falls back to well-known probing when resource_metadata is absent
  PASS: PRM whose resource differs from the server URL is rejected
  PASS: SSRF: private-range authorization server blocked without connecting (0s)
  PASS: client_credentials flow works (resource-bound)
RESULT: 97 passed, 0 failed, 0 warnings (logs: /tmp/tmp.raPtGPmxBk)
```

The slice gate configured and built the project successfully. CTest for this
slice has not yet been run; the slice 4 gate includes CTest and sanitizer runs.

## Next action

Add the remaining slice 4 test and fuzz coverage, complete the authentication
guides and evidence/interop records, then run
`scripts/verify_6.sh --slice 4 --sanitizers --interop` and the Phase 5/5B
regression gates.
