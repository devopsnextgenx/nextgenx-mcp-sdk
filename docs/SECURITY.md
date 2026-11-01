# Security Boundaries

## Schema Validation

Tool input schemas are compiled at registration, must describe an object, and
are checked on every invocation. Output schemas are compiled once and checked
by default. A known tool with invalid arguments returns `isError: true`; invalid
handler output returns a generic `-32603` without forwarding that output.
Disabling output validation does not disable input validation.

The default dialect is JSON Schema Draft 2020-12. Boolean schemas are supported
by the validator; the tool input root must still be an object schema. Supported
older dialect declarations are handled by jsoncons rather than reinterpreted
as Draft 2020-12.

Default limits are 1 MiB per schema, schema/instance depth 64, reference depth 64,
and 4 MiB per serialized instance. External `$ref`, `$dynamicRef` and
`$recursiveRef` are disabled, including file/HTTP references. Local JSON pointers
and anchors are permitted; cycles and duplicate anchors are rejected. Embedded
resources requiring duplicate anchors are excluded. This is a restricted profile,
not an unrestricted schema-conformance claim.

The jsoncons JSON Schema backend evaluates `pattern` and `patternProperties`
with the C++ standard-library `std::regex` engine (ECMAScript syntax), which
does not guarantee linear-time matching. The SDK therefore limits patterns to
512 bytes and input strings/property names to 1024 bytes and rejects repeated
nested or ambiguous quantifiers, repeat counts above 256, lookaround and
noncapturing groups, backreferences, and Unicode property escapes. Unsupported
patterns fail during registration; oversized validation inputs fail before
regex evaluation. `regex_redos_budget` covers representative catastrophic
patterns. Do not raise these limits or broaden the accepted syntax for untrusted
schemas without replacing the engine or establishing a measurable work budget.

## Authenticated requestState

Modern continuations use an opaque `requestState` string. The default codec
authenticates it with HMAC-SHA256 using OpenSSL Crypto and checks the MAC with a
constant-time comparison. Claims bind method, tool, original parameters,
authenticated subject, requested input IDs, round count, issue time and expiry.
Only `inputResponses` and `requestState` are excluded from parameter binding;
clients must preserve the original arguments/metadata and use a new RPC ID.

The token is authenticated, **not encrypted**. Its hex-encoded claims are readable.
Never put credentials, access tokens or sensitive private application data in
the continuation payload. The default token limit is 8192 bytes, TTL 600 seconds,
and maximum input-round count ten.

Without configuration each server generates a random process-local key. A
restart invalidates outstanding tokens, and another instance cannot verify them.
For a server pool, configure identical ordered `ServerOptions::request_state_keys`;
the last key signs new tokens and every configured key can verify old tokens.
Keep old keys only for the required expiry window. IDs are unique short ASCII
identifiers, and secrets are exactly 64 hexadecimal characters. Load production
secrets from protected configuration, not source control or command history.
The example's `--request-state-key` flag is for local testing and is visible to
process inspection.

The default subject is `anonymous`. Multi-user HTTP deployments must provide
`HttpHandlerOptions::request_state_subject` using an identity already verified
by the authenticator, or supply `IRequestStateSubjectProvider`. Do not derive it
from an unverified identity header or JSON metadata. Embedding adapters can set
the trusted subject on `ToolContext`. A token issued for one subject cannot
authorize continuation as another subject.

Stateless authentication does not make a token single-use. Replaying a valid
token for the same subject and original operation remains possible until expiry.
Destructive handlers must implement idempotency or consume a durable nonce when
one-time execution is required. Recheck authorization on every retry.

Applications may inject their own `IRequestStateCodec`; they must preserve
authentication, expiry, binding and resource limits themselves.

## User Input

Form elicitation is for non-sensitive values only. Schemas are flat, with
primitive fields or string-selection arrays. Obvious secret field names,
password formats and write-only fields are rejected on the server and by the
built-in form callback path. Applications remain responsible for never asking
for passwords, tokens, recovery codes or other secrets through forms; field-name
checks cannot infer every value's sensitivity.

Use URL elicitation for sensitive interactions. Server URL requests require
HTTPS; clients explicitly opt in with `allow_url_elicitation`. The SDK does not
open URLs or grant consent automatically. Validate/present the destination and
requested action. Generic `on_input` callbacks own their UI and safety policy.

Accepted form responses are checked against the requested schema. Unknown IDs
and invalid actions/content are rejected before invoking the handler. Decline
and cancel are explicit results, not implicit approval.

Modern requests advertise capabilities per request and use MRTR retries, never
server-initiated RPC requests. Legacy HTTP uses negotiated session capabilities,
server requests on the call's SSE stream and session-correlated response POSTs.
Legacy waits default to 30 seconds, observe disconnects, and permit at most 32
pending inputs per session. The stdio example uses worker dispatch so its reader
can receive nested legacy responses.

## Logging and Callbacks

Modern logs require a per-request log level; legacy filtering follows
`logging/setLevel`. Logging is deprecated in the 2026 protocol. Prefer stderr or
an application telemetry system for operational diagnostics.

Structured log redaction masks common credential fields and path/newline-bearing
strings. Diagnostic stdio logs report frame sizes, not payloads. Tool exceptions
and output-validation failures do not log/return arbitrary exception text.
Redaction is not a DLP guarantee: keep secrets out of application logs/events.

Notification count and emission interval are configurable. Progress requires a
string/integer token, finite values and strictly increasing progress. Finished
or cancelled contexts drop further emissions. The client generates/routes its
own string tokens. Logging-enabled calls and legacy tool calls are serialized
per client because their wire messages lack general per-call correlation.
Callbacks are synchronous: do not make reentrant blocking calls on the same
client. Timeouts cannot forcibly interrupt arbitrary user callback code; UI
callbacks must honor application cancellation/deadlines.

## Transport and Test Environment

Bind locally by default. Remote deployments need TLS, authenticated principals,
Host/Origin allowlists and appropriate proxy/body/header limits. Do not enable
insecure remote mode or disable certificate checks in production. Handlers should
produce explicit SDK results rather than pass through an untrusted object as a
pre-serialized internal ToolResult marker.

Ubuntu 25.04 is the requested development/test base and is out of security
support. Do not publish it as a production runtime. Bounded fuzz and sanitizer
runs are not proof that every input or concurrent schedule is safe.

## File sandbox and URI trust boundaries

The example's file-backed resource is opt-in and accepts a relative path from
the `demo://files/{+path}` template only when `--resource-root` is configured.
The path is percent-decoded once by template matching; the provider rejects
residual encoded traversal, `.`/`..` or empty components, absolute and drive
paths, UNC forms, backslashes, NUL/control bytes, hidden components, and paths
over 4096 bytes. Reads are capped before and during I/O by
`--max-resource-bytes`; only regular files are returned. Failures use a generic
invalid-params error and do not reveal host paths or distinguish forbidden from
missing files. Text is emitted only for valid UTF-8 with a text-like MIME type;
other bytes are base64 blobs.

On POSIX the provider opens the root and walks each component with `openat`,
`O_NOFOLLOW`, and `O_DIRECTORY` for intermediate directories, then checks the
opened file descriptor with `fstat` and performs bounded reads. This prevents
symlink traversal and avoids a check-then-open race. The current Windows
implementation uses canonical-path containment with standard filesystem
streams; it does not provide equivalent handle-relative reparse-point/TOCTOU
guarantees and should not be used as a hostile multi-user sandbox. A future
Windows implementation must use no-follow handles and verify the final path
from the opened handle before this guarantee can be claimed cross-platform.

The example URI-template matcher has bounded, left-to-right matching and
percent-decodes captures once. Do not treat URI template values as filesystem
paths or authorization decisions; each consumer must validate its own
canonicalized values. Template registration and matching currently cover the
simple segment form plus the example's reserved path and form-query forms;
this is not a claim of full RFC 6570 expansion/matching conformance.

Modern listeners are bounded by global/per-peer listener limits, a per-listener
maximum of 256 resource URIs, and a bounded event queue. Resource filters are
canonicalized, deduplicated and limited to the first 256 distinct URIs; the
acknowledgement reports the accepted set. Applications should handle limit
errors and reconnects explicitly; no notification is delivered for an URI that
was not accepted or a notification type that was not requested.

Modern cache fields are emitted with a zero TTL for changing counter data and
otherwise use the example's documented TTL. Applications should use `private`
scope for authorization-dependent content; `public` is safe only when the
result cannot vary by caller. Never share private cache entries across
authorization identities, and do not cache MRTR continuations.

TSan needs process-local ASLR/personality setup that Docker's default seccomp
profile blocks here. The verification container uses
`--security-opt seccomp=unconfined` only for isolated testing, with no published
ports or host security-setting changes. Do not copy this setting into production
container deployments. Sanitizer builds use static libraries; the regular gate
also builds the shared libraries and verifies the installed consumer.