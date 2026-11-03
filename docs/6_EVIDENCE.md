# Phase 6 verification evidence

## Completed slices

The preserved gate summaries for the committed work are:

```text
scripts/verify_6.sh --slice 1
RESULT: 45 passed, 0 failed

scripts/verify_6.sh --slice 2
RESULT: 65 passed, 0 failed

scripts/verify_6.sh --slice 3
RESULT: 97 passed, 0 failed, 0 warnings
```

The slice-3 summary included the client-credentials resource-binding check,
issuer-keyed stored-token reuse, PRM fallback, resource mismatch rejection,
and blocked private-range authorization-server test.

## Slice 4 focused verification

The following build and focused test command completed with exit status 0:

```text
timeout 240 cmake --build build -j2 </dev/null
timeout 240 ctest --test-dir build --output-on-failure -R 'cppmcp_(scope_single_challenge|prm_metadata|jwt_parser|jwks_policy|introspection_config|scope_hierarchy_|scope_single_challenge_|scope_parser_|session_binding_|www_authenticate_|oauth_discovery_|oauth_pkce_|oauth_iss_|oauth_stepup_|oauth_refresh_|oauth_single_flight_|oauth_store_issuer_|oauth_ssrf_|token_passthrough_|token_redaction_)' </dev/null
100% tests passed, 0 tests failed out of 77
```

The focused selection included PRM, JWT/JWKS, introspection, scope, session
binding, WWW-Authenticate parsing, OAuth discovery/PKCE/issuer/step-up/refresh/
single-flight/store/SSRF, token passthrough/redaction, and the
`scope_single_challenge` regression test. CTest discovery initially reported 420 tests. The final gate also added two
independently selectable MRTR cases and the peer-keyed failed-auth limiter test,
so discovery now reports 423 tests.

## Final slice-4 gate

Ran:

```text
timeout 1800 scripts/verify_6.sh --slice 4 --sanitizers --interop </dev/null
RESULT: 134 passed, 0 failed, 2 warnings
PASS: ctest discovers 423 tests
PASS: ctest passes
PASS: sanitizer [address,undefined]: tests pass, zero findings
PASS: sanitizer [thread]: tests pass, zero findings
PASS: Inspector CLI with a bearer header
PASS: >= 15 fuzz targets
PASS: installed-consumer verification passes
```

The two warnings were informational interop results: Inspector's no-token
output did not match the gate's expected 401 text, and the gate reminded that a
Claude Code OAuth login requires an interactive run. The bearer-header
Inspector invocation passed. The uncredentialed Inspector warning is not
counted as a pass.

All 15 fuzzer targets configured and built with Clang in offline mode:

```text
CC=clang CXX=clang++ timeout 900 cmake -S . -B build-fuzz \
  -DCPPMCP_BUILD_FUZZERS=ON -DCPPMCP_BUILD_STATIC=ON \
  -DCPPMCP_BUILD_SHARED=OFF -DCPPMCP_BUILD_TESTS=OFF \
  -DCPPMCP_BUILD_EXAMPLES=OFF -DCPPMCP_OFFLINE=ON
timeout 900 cmake --build build-fuzz -j2
```

The three new authentication fuzzer binaries each completed 1,000 smoke runs:
`cppmcp_fuzz_jwt_parser`, `cppmcp_fuzz_oauth_metadata`, and
`cppmcp_fuzz_scope_set`; no crash or sanitizer finding occurred.

## Regression gates

```text
timeout 1200 scripts/verify_5.sh </dev/null
RESULT: 107 passed, 0 failed, 0 warnings

timeout 1200 scripts/verify_5b.sh </dev/null
RESULT: 86 passed, 0 failed, 1 warning
```

The Phase 5B warning records that the external MCP conformance command was not
configured. The regression CTest suite passed 423 tests in both gates.

## Interactive interop not performed

No manual Claude Code OAuth login was performed. It requires a user-driven
interactive browser/consent step, which is not part of the bounded noninteractive
gate run. Previous Claude Code entries in [INTEROP.md](INTEROP.md) verify
connection health only and are not represented as Phase 6 OAuth evidence.
