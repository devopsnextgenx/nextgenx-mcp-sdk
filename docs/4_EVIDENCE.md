# Phase 4 Evidence

## Environment

Compilation and runtime validation run inside `cppmcp-dev:ubuntu25.04`, built
from [docker/Dockerfile](../docker/Dockerfile). The Windows checkout is mounted
at `/workspace`; `cppmcp-phase4-build` holds the main build cache. Gate scripts
are normalized from CRLF in memory; the acceptance script is not edited.
Parallelism is bounded to four container CPUs.

Ubuntu 25.04 is the requested test base and is out of security support, not a
production runtime recommendation.

## Committed Slices

| Slice | Commit | Gate Result |
| --- | --- | --- |
| Schema/output foundation | `7cbc83c` | 16 passed, 0 failed, 0 warnings |
| Progress/logging | `9197313` | 30 passed, 0 failed, 0 warnings |
| Authenticated MRTR | `8ce1cbd` | 48 passed, 0 failed, 0 warnings |

## Additional Verification

Focused Docker tests passed for form/URL elicitation, state-only continuation,
generic callbacks, tampering/parameter binding, expiry, key rotation,
two-instance keys, authenticated subjects, the legacy adapter, actual legacy
HTTP/stdio RPC exchanges, and progress before operation completion.

The official JSON Schema Test Suite is pinned at commit
`89e83a74014617ffbe79a56d83d14d054cd11417`, archive SHA-256
`70e238f62ee2d5143e9b292109dbf054f5055d31111ae20f75af754807c5323a`.
The schema-focused run completed 52 CTest cases without failures. `defs`,
`refRemote` and `vocabulary` were entirely policy-skipped. Individual groups
with external/cyclic references, duplicate anchors or unsupported regex syntax
are counted as skipped, never silently passed.
The verbose category reports total **1167 passed, 0 failed, 135 skipped**
instance cases across 46 categories.

The first full run exposed six compatibility regressions: outdated scalar
expectations, null-valued empty test arguments, and forced-SSE startup behavior.
Their focused rerun passed after correcting the contracts and restoring forced
SSE keepalives. An HTTP type-detection link regression in the stdio-only client
was fixed by recording the negotiated mode without coupling the core client
to HTTP symbols. The all-target rebuild then passed.

## Completed Final Checks

- Unchanged `scripts/verify_4.sh --slice 4`: **70 passed, 0 failed, 0 warnings**.
	The gate discovered 150 CTest cases and its full CTest run passed. Three
	official-suite categories are intentionally skipped as described above.
- Seven Clang/libFuzzer targets completed 1000 runs each with seed 12345,
	ASan/UBSan enabled and no findings. The core/transport libraries themselves
	are instrumented. Schema and state targets use checked-in seed inputs;
	generated corpora and logs live in the Docker build volume.
- `scripts/verify_install.sh` installed the library, configured a separate
	consumer, resolved OpenSSL 3.4.1 Crypto/SSL, built and ran the consumer.
- The container compiler is GCC 14.2.0; C++ compilation is not run on Windows.

## Interop and Sanitizer Results

Inspector CLI 2.13.0 successfully ran `tools/call add` against the dual server.
Its npm installation warned that the image's Node 20.18.1 is below the declared
Node >=22.19.0 engine range. Claude CLI is not installed in this image and was
not verified in the Phase 4 run.
An independent stdout capture parsed as JSON and verified `structuredContent == 3`
with exit 0. Its compatible text block was `"3.0"`; npm warnings were captured
separately rather than used as result evidence.

The completed unchanged gate with `--slice 4 --sanitizers --interop` reported
**73 passed, 0 failed, 1 warning**. The warning is the unavailable Claude CLI.
Both `sanitizer [address,undefined]` and `sanitizer [thread]` report tests pass
with zero findings. Each configuration discovers the same 150 tests, with the
three documented schema-suite categories skipped by policy.

The first sanitizer attempt was blocked by GCC 14.2 regex-header diagnostics
promoted to errors and by Docker denying TSan's `personality(ADDR_NO_RANDOMIZE)`
setup. The ASan core target rebuilt successfully after a source-only warning
downgrade; a TSan MRTR case then passed in a disposable container with seccomp
unconfined. The complete rerun uses that container-only setting and static
sanitizer libraries. No acceptance-script or host security setting was changed.

## Reproduction and Logs

Use the mounted-source command in [README.md](../README.md), adding
`--sanitizers --interop` to the gate arguments and
`--security-opt seccomp=unconfined` only to the disposable sanitizer container.
The sanitizer run used two assigned CPUs and separate named volumes:
`cppmcp-phase4-asan` at `/workspace/build-san-address-undefined` and
`cppmcp-phase4-tsan` at `/workspace/build-san-thread`. Each sanitizer cache was
configured with `CPPMCP_BUILD_SHARED=OFF` before running the unchanged gate.
The regular acceptance gate built both static and shared variants.

Completed logs in the `cppmcp-phase4-build` volume:

- `phase4-final-gate.log`: standalone acceptance, 70/0/0.
- `phase4-sanitizer-interop-gate-rerun.log`: full rerun, 73/0/1.
- `tmp.4s3O9EwwVA/build-san-address-undefined.test`: ASan/UBSan CTest output.
- `tmp.4s3O9EwwVA/build-san-thread.test`: TSan CTest output.
- `schema-suite-verbose.log`: per-category official-suite pass/skip counts.
- `fuzz-*.log`: seven bounded instrumented fuzz runs.
- `phase4-install.log`: downstream installed-consumer build/run.
- `phase4-inspector.stdout.json` and `phase4-inspector.stderr.log`: separated
	external-client result and environment diagnostics.

No live servers or published container ports are needed after validation.