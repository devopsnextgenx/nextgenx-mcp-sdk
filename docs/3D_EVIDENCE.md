# Phase 3D verification evidence

Commands were run from the repository root on 2026-10-18. The supplied
`scripts/verify_3d.sh` was not edited.

## Acceptance gates

```text
Slice 1:
RESULT: 20 passed, 0 failed, 0 warnings

Slice 2:
PASS: client reports STREAM_LOST
PASS: client reports RECONNECTED
PASS: notifications resume after reconnect
RESULT: 29 passed, 0 failed, 0 warnings

Slice 3:
PASS: curl over TLS with the test CA works
PASS: curl without the CA is rejected (rc=60)
PASS: our client over TLS with --cacert works
PASS: our client rejects an untrusted CA by default
PASS: client accepts IP SAN match
PASS: client rejects hostname mismatch with a clear error
PASS: client accepts DNS SAN match
PASS: non-loopback bind without TLS/auth refused
PASS: non-loopback bind allowed with TLS
RESULT: 40 passed, 0 failed, 0 warnings

Slice 4, final run:
PASS: our client -> official TypeScript reference server
PASS: Claude Code connected (dual server)
PASS: sanitizer [address,undefined]: tests pass, zero findings
PASS: sanitizer [thread]: tests pass, zero findings
RESULT: 63 passed, 0 failed, 0 warnings
```

The active-listener SIGTERM reproduction initially returned `server_exit=139`.
ASan+UBSan identified a null disconnect-watch dereference in the SSE writer.
After registering the callback before starting the writer, the same
ASan+UBSan reproduction returned `server_exit=0`; the client received the
expected incomplete-stream curl status when the server was shut down.

## CTest

```text
85/85 tests passed, 0 tests failed
Total Test time (real) = 6.70 sec

ASan+UBSan: all 85 tests passed, zero findings
TSan: all 85 tests passed, zero findings
```

The 1000-subscriber registry soak, hostname-mismatch, expired-certificate, and
TLS truncation tests also passed. A focused auth-challenge listener retry test
caught a socket lifetime/cancellation race under both sanitizers. Replacing an
active Asio socket could previously destroy its context too early, and closing
the Asio socket concurrently with a blocking read raced. The implementation
now preserves context lifetime across replacement and shuts down the native
connection descriptor to wake the read without concurrently mutating the
Asio socket. The focused test and the full sanitizer suites then passed.

## Fuzz smoke tests

Five Clang/libFuzzer targets were built and each completed a bounded 10-run
smoke test:

```text
cppmcp_fuzz_mirrored_header: Done 10 runs in 0 second(s)
cppmcp_fuzz_mcp_name_param: Done 10 runs in 0 second(s)
cppmcp_fuzz_http_request: Done 10 runs in 0 second(s)
cppmcp_fuzz_sse_parser: Done 10 runs in 0 second(s)
cppmcp_fuzz_www_authenticate: Done 10 runs in 0 second(s)
```

## Conformance and external interoperability

See [CONFORMANCE.md](./CONFORMANCE.md) for the official scenario results and
known gaps. Fresh Inspector, TypeScript reference-server, Claude Code, and
TypeScript SDK client results are in [INTEROP.md](./INTEROP.md).
