# HTTP OAuth authentication

Phase 6 provides an optional OAuth boundary for Streamable HTTP. It does not
change stdio authentication: stdio applications should obtain credentials from
their environment or another application-controlled source.

## Resource-server configuration

Configure the canonical externally visible MCP resource URI and at least one
authorization-server issuer. The resource URI must match the URI clients use;
use the public HTTPS origin and MCP path when deployed behind a proxy.

```sh
./build/cppmcp_http_server --port 8000 \
  --auth-resource https://mcp.example.com/mcp \
  --auth-issuer https://login.example.com/tenant \
  --auth-scopes-supported 'mcp:use projects:write projects:admin'
```

The server publishes RFC 9728 Protected Resource Metadata at both the
path-inserted and root well-known routes. `authorization_servers` always
contains the configured issuer. The 401 challenge includes the path-inserted
`resource_metadata` URL unless `--auth-omit-resource-metadata-param` is enabled.
That option is useful for testing client well-known fallback. The PRM advertises
supported scopes; it must not advertise `offline_access`.

The example defaults to JWT verification with the configured issuers and the
resource URI as the accepted audience. JWKS refreshes have a configurable
minimum interval (`--auth-jwks-min-refresh-s`). An application can instead
provide an `ITokenVerifier`, compose JWT and RFC 7662 introspection verifiers,
or retain `StaticBearerAuthenticator` as a development verifier. Introspection
client secrets are read from a file, not a command-line value. All metadata,
JWKS, and introspection retrieval uses `cppmcp::net::NetworkPolicy`.

Authorization is enforced on every MCP HTTP request. Send bearer credentials
only in `Authorization` headers, never in query parameters. The server verifies
that the token audience contains the configured resource and does not expose
the inbound token through `AuthContext`. Invalid credentials receive a 401
`invalid_token` challenge; a valid identity lacking permissions receives one
403 challenge containing all missing scopes.

Applications can inject `auth::IRateLimiter` through the HTTP handler options.
It receives the peer address for failed authentication attempts; a denied
attempt receives HTTP 429. The SDK supplies the hook, not a particular
distributed limiter or peer-identity policy.

## Scope design

Every request requires `mcp:use` by default. Per-operation requirements can be
attached with `.requires_scopes(...)` on tool, prompt, resource, and template
builders. Dynamic requirements receive a `RequestView`, allowing the required
scope to depend on validated operation arguments. For example, a destructive
operation can require `projects:write` generally and `projects:admin` for a
production project.

The configured hierarchy is transitive: a broader scope such as
`projects:admin` can imply `projects:write`. Define only implications that are
true for the application's authorization model. The example includes
`projects:admin -> projects:write` and `files:write -> files:read`.

Listing behavior is configurable with `--auth-listing show_all` (default) or
`filter`. `show_all` keeps protected operations discoverable so clients can
step up when challenged; it can reveal operation names and descriptions.
`filter` hides entries the current token cannot use, but may make otherwise
available operations undiscoverable until permissions change. Neither listing
mode replaces authorization at invocation time.

Handlers may inspect verified identity and claims through `ToolContext::auth()`.
Use that for application-specific checks, auditing identifiers, or ownership
rules; do not treat it as a substitute for declared scope requirements.

```cpp
cppmcp::ToolDefinition definition;
definition.name = "whoami";
definition.input_schema = {{"type", "object"}};
server.add_tool(
    cppmcp::ToolBuilder(std::move(definition)).requires_scopes(
        cppmcp::auth::ScopeSet{"mcp:use"}),
    [](const nlohmann::json& args, const nlohmann::json&,
       cppmcp::ToolContext& context) {
        const auto* identity = context.auth();
        return nlohmann::json{{"subject", identity ? identity->subject : ""}};
    });
```

## Client OAuth

The bundled client uses the OAuth provider when `--oauth` is selected. It
discovers PRM from the challenge or the path-inserted/root well-known routes,
then discovers AS metadata in the specified RFC 8414/OIDC order. It verifies
issuer identity, requires S256 PKCE support, binds the `resource` parameter to
the MCP URL, and validates `state` and RFC 9207 `iss` before exchanging the
authorization code.

```sh
./build/cppmcp_http_client --url https://mcp.example.com/mcp \
  --oauth --oauth-launcher print --oauth-store-file "$HOME/.local/state/cppmcp-tokens.json" \
  --call add --args '{"a":2,"b":3}'
```

The `print` launcher is intended for headless use and accepts the callback URL
from standard input. Interactive launchers are `xdg-open`, `open`, and `start`.
The `http-follow` launcher exists for local test fixtures. It follows only the
exact loopback callback; it is not a general redirect-following HTTP client.
Client OAuth includes authorization-code PKCE, refresh-token rotation,
issuer-keyed storage, bounded step-up, and the optional `client_credentials`
grant.

Registration is issuer-scoped and follows pre-registered credentials, CIMD
when advertised, DCR when explicitly enabled and advertised, and finally an
application prompt. A CIMD URL must be HTTPS and its document must identify
itself with the same `client_id`, plus `client_name` and `redirect_uris`.
`--oauth-allow-insecure-loopback` is a test-only exception: it permits local
HTTP fixture endpoints and a loopback-hosted CIMD document, but never relaxes
the block on non-loopback private destinations.

Pre-registered credentials supplied to the library must be associated with the
expected issuer; the provider never reuses a registration for another issuer.
Client secrets are loaded from files or provided through application-owned
configuration; never put secrets in command-line arguments. OAuth event output
reports flow state but must not include authorization codes, access tokens,
refresh tokens, or secret values.

## Deployment and boundaries

Use HTTPS for the MCP server and all authorization-server endpoints. Keep the
backend inaccessible when TLS terminates at a trusted proxy, and configure
allowed hosts/origins there. `--auth-insecure-http-for-testing` permits HTTP
only for loopback destinations and is not a deployment setting.

Token passthrough is prohibited: do not forward an MCP access token to an
upstream API. An MCP token is audience
bound to this resource server. Applications that need upstream authorization
must use a separate credential, for which the public `ITokenExchanger`
interface provides an integration point.

JWT/JWKS and introspection caches are process-local. In a multi-instance
deployment, configure consistent issuer/resource values and account for key
rotation visibility, stale-key grace, introspection cache TTL, and the shared
request-state signing key. Legacy HTTP sessions bind to issuer, subject, and
client ID, but remain state only: each request still requires a valid token.
Long-lived streams are closed when the opening token expires; clients must
reauthorize and reconnect.

The file token store uses restrictive permissions and atomic replacement.
Applications can enable AES-256-GCM encryption with a key obtained from an
environment-variable name supplied to `FileTokenStore`. Environment-based key
delivery is still the application's responsibility; use an OS keychain or a
dedicated secret manager where available. OS keychain adapters are not included
in this phase.

The test authorization server under `tests/` is deliberately unsafe and is not
installed. It is a fixture for local verification, not a production AS.
