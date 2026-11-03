#!/usr/bin/env bash
# scripts/verify_6.sh : executable acceptance gate for Phase 6 (authentication).
# Usage: scripts/verify_6.sh [--slice N] [--sanitizers] [--interop]
# Env:   BUILD_DIR (default build), HTTP_BIN, CLIENT_BIN, STDIO_BIN, AS_BIN, SKIP_BUILD=1
#
# ======================= CONTRACT (examples the agent must provide) =======================
# Test authorization server  `test_authserver` (test-only; never installed):
#   flags : --port N --issuer URL            (default issuer http://127.0.0.1:<port>)
#   metadata (RFC 8414 at /.well-known/oauth-authorization-server and OIDC /.well-known/openid-configuration):
#       issuer, authorization_endpoint, token_endpoint, jwks_uri, registration_endpoint, introspection_endpoint,
#       code_challenge_methods_supported ["S256"], authorization_response_iss_parameter_supported,
#       client_id_metadata_document_supported, scopes_supported, token_endpoint_auth_methods_supported
#   /authorize  : auto-approves without UI; requires PKCE S256, exact redirect_uri, `resource`; redirects to redirect_uri
#                 with code, state and `iss`. For a CIMD client_id (https/http-loopback URL with path) it fetches and validates the document.
#   /token      : authorization_code (+PKCE verify, resource -> aud), refresh_token (optional rotation), client_credentials
#   /register   : RFC 7591 DCR      /jwks : signing keys (RS256 + ES256)      /introspect : RFC 7662
#   issues JWT access tokens (typ at+jwt) with iss, sub, aud=<resource>, scope (space-delimited), client_id, exp, iat, jti
#   TEST CONTROL (GET, JSON in/out):
#     /__test/mint?sub=&scope=&aud=&iss=&ttl=&client_id=&alg=&kid=&badsig=1&scp=1&nbf_offset=&exp_offset=&noaud=1&opaque=1
#          -> {"access_token": "..."}   alg: RS256|ES256|none|HS256 (HS256 = signed with the RSA PUBLIC key bytes as the HMAC secret: algorithm-confusion attack)
#          opaque=1 mints a random opaque token known to /introspect
#     /__test/rotate            -> rotate signing keys (new kid)      /__test/revoke?token=
#     /__test/config?cimd=0|1&dcr=0|1&pkce=0|1&iss_param=good|bad|absent&iss_advertised=0|1&issuer_doc=good|bad
#                    &ttl=SECONDS&rotate=0|1&grant_limit=SCOPE1,SCOPE2&state=good|bad&introspect_fail=0|1
#     /__test/reset             -> clear config and stats
#     /__test/stats -> {"authorize":[{"client_id","redirect_uri","resource","scope","code_challenge_method","has_state"}],
#                       "token":[{"grant_type","client_id","resource","has_code_verifier","client_auth","scope"}],
#                       "register":[{"application_type","redirect_uris"}], "cimd_fetches":[url...],
#                       "metadata_fetches":N, "jwks_requests":N, "introspect_requests":N,
#                       "bearer_on_non_resource_endpoints":N}
# http_server auth flags:
#   --auth-issuer URL (repeatable)  --auth-resource URI  --auth-scopes-supported "a b c"  --auth-insecure-http-for-testing
#   --auth-clock-skew-s N  --auth-listing show_all|filter  --auth-omit-resource-metadata-param
#   --auth-introspect-url URL --auth-introspect-client-id ID --auth-introspect-secret-file F --auth-introspect-cache-s N
#   --auth-jwks-min-refresh-s N (default 10)   (existing flags unchanged)
# Example scope model (implemented through the PUBLIC API):
#   base scope  mcp:use  required for every request (challenge scope when no token)
#   tool add                      : mcp:use only                      tool whoami: mcp:use; returns {"sub","client_id","scopes"} (never the token)
#   tool delete_project {project} : projects:write, or projects:admin when project=="prod" (dynamic); projects:admin implies projects:write
#   tool export_report {}         : requires BOTH reports:read and files:write (single challenge must list both)
#   resource template demo://files/{+path} : files:read     (files:write implies files:read)
#   resource demo://secret/needs_confirm   : secrets:read
# http_client auth flags and stdout events:
#   --oauth  --oauth-client-id ID  --oauth-client-secret-file F  --oauth-cimd-url URL  --oauth-dcr  --oauth-grant authorization_code|client_credentials
#   --oauth-scopes "a b"  --oauth-launcher http-follow|print|xdg-open  --oauth-redirect-port N  --oauth-store-file PATH
#   --oauth-allow-insecure-loopback  --repeat N --repeat-delay-ms M (repeat --call)  --parallel N (N concurrent --call)
#   events: AUTH discovery prm <url> | AUTH as <issuer> | AUTH registration <prereg|cimd|dcr> | AUTH authorize scope=<s> resource=<r>
#           AUTH token granted_scope=<s> | AUTH refresh | AUTH stepup scope=<s> | AUTH failed <reason>
#   reasons: no_registration pkce_unsupported issuer_mismatch iss_mismatch iss_missing state_mismatch resource_mismatch
#            blocked_address stepup_limit prm_missing
# ==========================================================================================
set -u

SLICE=4; SAN=0; INTEROP=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --slice) SLICE="$2"; shift 2 ;;
    --sanitizers) SAN=1; shift ;;
    --interop) INTEROP=1; shift ;;
    *) echo "unknown argument: $1"; exit 2 ;;
  esac
done

PASS=0; FAIL=0; WARN=0
ok()   { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
warn() { echo "  WARN: $1"; WARN=$((WARN+1)); }
check() { local d="$1"; shift; if "$@" >/dev/null 2>&1; then ok "$d"; else bad "$d"; fi; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing required tool: $1"; exit 2; }; }
for t in curl jq python3; do need "$t"; done

ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT" || exit 2
BUILD="${BUILD_DIR:-build}"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
LOG_DIR="$(mktemp -d)"
PIDS=(); LAST_PID=""
cleanup() { for p in "${PIDS[@]:-}"; do [[ -n "$p" ]] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

if [[ "${SKIP_BUILD:-0}" != 1 ]]; then
  need cmake; need git
  echo "== build ($BUILD)"
  if cmake -S . -B "$BUILD" >"$LOG_DIR/cfg.log" 2>&1 && cmake --build "$BUILD" -j"$JOBS" >"$LOG_DIR/build.log" 2>&1; then ok "configure and build"
  else bad "configure and build (see $LOG_DIR/build.log)"; tail -20 "$LOG_DIR/build.log"; exit 1; fi
fi
find_bin() { find "$BUILD" -type f -perm -u+x \( -name "$1" -o -name "cppmcp_$1" \) 2>/dev/null | head -n1; }
HTTP_BIN="${HTTP_BIN:-$(find_bin http_server)}"; CLIENT_BIN="${CLIENT_BIN:-$(find_bin http_client)}"
STDIO_BIN="${STDIO_BIN:-$(find_bin stdio_server)}"; AS_BIN="${AS_BIN:-$(find_bin test_authserver)}"
[[ -z "$HTTP_BIN" ]]   && { bad "http_server binary not found"; exit 1; }
[[ -z "$CLIENT_BIN" ]] && { bad "http_client binary not found"; exit 1; }
[[ -z "$AS_BIN" ]]     && { bad "test_authserver binary not found (slice 1 deliverable)"; exit 1; }

free_port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
wait_port() { for _ in $(seq 1 60); do curl -s -m 2 -o /dev/null "http://127.0.0.1:$1/" && return 0; sleep 0.1; done; return 1; }
start_as()  { local p="$1"; shift; "$AS_BIN" --port "$p" "$@" >"$LOG_DIR/as-$p.log" 2>&1 & LAST_PID=$!; PIDS+=("$LAST_PID"); wait_port "$p" && kill -0 "$LAST_PID" 2>/dev/null; }
start_srv() { local p="$1"; shift; "$HTTP_BIN" --port "$p" "$@" >"$LOG_DIR/server-$p.log" 2>&1 & LAST_PID=$!; PIDS+=("$LAST_PID"); wait_port "$p" && kill -0 "$LAST_PID" 2>/dev/null; }
run_client() { timeout "${CLIENT_TIMEOUT:-60}" "$CLIENT_BIN" "$@" </dev/null 2>&1; }

PA="$(free_port)"; ISS="http://127.0.0.1:$PA"
start_as "$PA" || { bad "test_authserver failed to start"; cat "$LOG_DIR/as-$PA.log"; exit 1; }
ok "test authorization server on :$PA"
RDIR="$LOG_DIR/root"; mkdir -p "$RDIR"; printf 'hello world' >"$RDIR/hello.txt"
KEY="00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
PM="$(free_port)"; RES="http://127.0.0.1:$PM/mcp"; MCP="$RES"
SCOPES="mcp:use files:read files:write projects:write projects:admin reports:read secrets:read"
start_srv "$PM" --mode dual --resource-root "$RDIR" --request-state-key "$KEY" --auth-issuer "$ISS" --auth-resource "$RES" \
  --auth-scopes-supported "$SCOPES" --auth-insecure-http-for-testing --auth-jwks-min-refresh-s 5 --tick-ms 300 \
  || { bad "auth-enabled server failed to start"; cat "$LOG_DIR/server-$PM.log"; exit 1; }
ok "auth-enabled MCP server on :$PM (resource $RES)"

# ---------------- helpers ----------------
mint() { local q="${1// /%20}"; curl -s -m 10 "$ISS/__test/mint?$q" | jq -r '.access_token // empty'; }
astats() { curl -s -m 10 "${1:-$ISS}/__test/stats"; }
acfg() { curl -s -m 10 -o /dev/null "${2:-$ISS}/__test/config?$1"; }
areset() { curl -s -m 10 -o /dev/null "${1:-$ISS}/__test/reset"; }
MH=(-H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" -H "MCP-Protocol-Version: 2026-07-28")
CAPS='{}'
mbody() { jq -nc --arg m "$1" --argjson p "$2" --argjson caps "$CAPS" '{jsonrpc:"2.0",id:1,method:$m,params:($p + {_meta:{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{name:"verify",version:"0"},"io.modelcontextprotocol/clientCapabilities":$caps}})}'; }
CODE=""; WWW=""
# req URL TOKEN-HEADER-VALUE METHOD NAME PARAMS  (TOKEN-HEADER-VALUE is the full Authorization header value or "")
req() {
  local url="$1" auth="$2" method="$3" name="$4" params="$5"; local extra=(-H "Mcp-Method: $method")
  [[ -n "$name" ]] && extra+=(-H "Mcp-Name: $name"); [[ -n "$auth" ]] && extra+=(-H "Authorization: $auth")
  CODE="$(curl -sS -m 25 -D "$LOG_DIR/r.hdr" -o "$LOG_DIR/r.body" -w '%{http_code}' "${MH[@]}" "${extra[@]}" -d "$(mbody "$method" "$params")" "$url" 2>/dev/null)"
  WWW="$(grep -i '^www-authenticate:' "$LOG_DIR/r.hdr" | head -n1 | tr -d '\r')"
}
final() { if grep -qi '^content-type: *text/event-stream' "$LOG_DIR/r.hdr"; then grep '^data:' "$LOG_DIR/r.body" | sed 's/^data: *//' | jq -c 'select(has("result") or has("error"))' | tail -n1; else cat "$LOG_DIR/r.body"; fi; }
bearer() { echo "Bearer $1"; }
expect_401() { # expect_401 DESC TOKEN
  req "$MCP" "$(bearer "$2")" tools/list "" '{}'
  if [[ "$CODE" == 401 && "$WWW" == *Bearer* && "$WWW" == *invalid_token* ]]; then ok "$1 -> 401 invalid_token"; else bad "$1 should be 401 invalid_token (got $CODE; WWW: ${WWW:0:160})"; fi
}
BASE="sub=alice&scope=mcp:use&aud=$RES&client_id=test-client"

echo "== slice 1: protected resource metadata, challenges, token verification"
for u in "/.well-known/oauth-protected-resource/mcp" "/.well-known/oauth-protected-resource"; do
  c="$(curl -s -m 10 -D "$LOG_DIR/prm.hdr" -o "$LOG_DIR/prm.body" -w '%{http_code}' "http://127.0.0.1:$PM$u")"
  [[ "$c" == 200 ]] && ok "PRM served at $u" || bad "PRM missing at $u (HTTP $c)"
done
c="$(curl -s -m 10 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PM/.well-known/oauth-protected-resource/other")"; [[ "$c" == 404 ]] && ok "PRM for a different path -> 404" || bad "PRM for a foreign path should be 404 (got $c)"
curl -s -m 10 -D "$LOG_DIR/prm.hdr" -o "$LOG_DIR/prm.body" "http://127.0.0.1:$PM/.well-known/oauth-protected-resource/mcp"
echo "-- PRM: $(head -c 400 "$LOG_DIR/prm.body")"
jq -e --arg r "$RES" --arg i "$ISS" '.resource == $r and (.authorization_servers|index($i) != null) and (.scopes_supported|index("mcp:use") != null) and ((.bearer_methods_supported // ["header"])|index("header") != null)' "$LOG_DIR/prm.body" >/dev/null 2>&1 \
  && ok "PRM fields: resource, authorization_servers, scopes_supported, bearer_methods_supported" || bad "PRM fields wrong"
grep -qi '^content-type: *application/json' "$LOG_DIR/prm.hdr" && ok "PRM content type application/json" || bad "PRM content type"
grep -qi '^access-control-allow-origin:' "$LOG_DIR/prm.hdr" && ok "PRM sends CORS header" || bad "PRM lacks Access-Control-Allow-Origin"
jq -e 'has("offline_access") or ((.scopes_supported // [])|index("offline_access") != null)' "$LOG_DIR/prm.body" >/dev/null 2>&1 && bad "PRM scopes_supported must not include offline_access" || ok "PRM omits offline_access"

req "$MCP" "" tools/list "" '{}'
echo "-- no token: $CODE $WWW"
[[ "$CODE" == 401 ]] && ok "no token -> 401" || bad "no token should be 401 (got $CODE)"
[[ "$WWW" == Www-Authenticate:\ Bearer* || "$WWW" == WWW-Authenticate:\ Bearer* || "$WWW" == www-authenticate:\ Bearer* ]] && ok "challenge uses the Bearer scheme" || bad "challenge must use Bearer"
[[ "$WWW" == *"resource_metadata=\"http://127.0.0.1:$PM/.well-known/oauth-protected-resource/mcp\""* ]] && ok "challenge carries resource_metadata (path-inserted URL)" || bad "challenge lacks the expected resource_metadata"
[[ "$WWW" == *"scope=\"mcp:use\""* ]] && ok "challenge carries scope=\"mcp:use\"" || bad "challenge lacks scope=\"mcp:use\""
[[ "$WWW" != *"error="* ]] && ok "no error attribute when no credentials were presented" || bad "no-credentials challenge must not carry error="
req "$MCP" "$(bearer garbage.not.ajwt)" tools/list "" '{}'; [[ "$CODE" == 401 && "$WWW" == *invalid_token* ]] && ok "garbage token -> 401 invalid_token" || bad "garbage token should be 401 invalid_token"

tok="$(mint "$BASE")"; [[ -n "$tok" ]] || { bad "could not mint a token"; exit 1; }
req "$MCP" "$(bearer "$tok")" tools/list "" '{}'; r="$(final)"; echo "$r" | jq -e '.result.tools|length>0' >/dev/null 2>&1 && [[ "$CODE" == 200 ]] && ok "valid token -> tools/list" || bad "valid token rejected (HTTP $CODE)"
req "$MCP" "bearer $tok" tools/list "" '{}'; [[ "$CODE" == 200 ]] && ok "scheme is case-insensitive (bearer)" || bad "lowercase 'bearer' scheme rejected"
req "$MCP" "Basic dXNlcjpwYXNz" tools/list "" '{}'; [[ "$CODE" == 401 ]] && ok "Basic scheme -> 401" || bad "Basic scheme should be 401 (got $CODE)"
t="$(mint "$BASE&scp=1")"; req "$MCP" "$(bearer "$t")" tools/list "" '{}'; [[ "$CODE" == 200 ]] && ok "scopes as array claim (scp) accepted" || bad "scp array claim rejected"

# negatives
expect_401 "wrong audience" "$(mint "sub=alice&scope=mcp:use&aud=https://other.example/mcp")"
expect_401 "missing audience" "$(mint "$BASE&noaud=1")"
expect_401 "wrong issuer" "$(mint "$BASE&iss=https://evil.example")"
expect_401 "expired token" "$(mint "$BASE&exp_offset=-3600")"
expect_401 "not-yet-valid (nbf in future)" "$(mint "$BASE&nbf_offset=3600")"
expect_401 "alg=none" "$(mint "$BASE&alg=none")"
expect_401 "HS256 algorithm confusion (RSA public key as HMAC secret)" "$(mint "$BASE&alg=HS256")"
expect_401 "bad signature" "$(mint "$BASE&badsig=1")"
expect_401 "unknown kid" "$(mint "$BASE&kid=does-not-exist")"
code="$(curl -s -o /dev/null -w '%{http_code}' -m 10 "${MH[@]}" -H 'Mcp-Method: tools/list' -d "$(mbody tools/list '{}')" "$MCP?access_token=$tok")"
[[ "$code" == 401 ]] && ok "token in query string is not accepted" || bad "query-string token must not authenticate (got $code)"
code="$(curl -s -o /dev/null -w '%{http_code}' -m 10 "${MH[@]}" -H 'Mcp-Method: tools/list' -H "Authorization: Bearer $tok" -H "Authorization: Bearer $tok" -d "$(mbody tools/list '{}')" "$MCP")"
[[ "$code" == 400 || "$code" == 401 ]] && ok "duplicate Authorization headers rejected ($code)" || bad "duplicate Authorization headers should be 400/401 (got $code)"

echo "-- JWKS handling"
sleep 6   # let the JWKS min-refresh window (--auth-jwks-min-refresh-s 5) elapse after the unknown-kid probes above
areset
curl -s -m 10 -o /dev/null "$ISS/__test/rotate"; t="$(mint "$BASE")"
req "$MCP" "$(bearer "$t")" tools/list "" '{}'; [[ "$CODE" == 200 ]] && ok "key rotation: new kid accepted after JWKS refresh" || bad "rotated key rejected (HTTP $CODE)"
areset
for i in $(seq 1 20); do req "$MCP" "$(bearer "$(mint "$BASE&kid=flood-$i")")" tools/list "" '{}' >/dev/null; done
n="$(astats | jq -r '.jwks_requests // 99')"
(( n <= 3 )) && ok "unknown-kid flood caused only $n JWKS fetches (rate limited)" || bad "unknown-kid flood caused $n JWKS fetches (must be <= 3)"
t="$(mint "$BASE")"; req "$MCP" "$(bearer "$t")" tools/list "" '{}'
if grep -qF "$t" "$LOG_DIR/server-$PM.log"; then bad "access token appears in the server log"; else ok "access token never logged"; fi

echo "-- legacy sessions with auth"
LH=(-H "Content-Type: application/json" -H "Accept: application/json, text/event-stream")
INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"verify","version":"0"}}}'
code="$(curl -s -o /dev/null -w '%{http_code}' -m 10 "${LH[@]}" -d "$INIT" "$MCP")"; [[ "$code" == 401 ]] && ok "legacy initialize without token -> 401" || bad "legacy initialize without token should be 401 (got $code)"
curl -sS -m 15 -D "$LOG_DIR/li.hdr" -o /dev/null "${LH[@]}" -H "Authorization: Bearer $tok" -d "$INIT" "$MCP"
SID="$(grep -i '^mcp-session-id:' "$LOG_DIR/li.hdr" | awk '{print $2}' | tr -d '\r\n')"
[[ -n "$SID" ]] && ok "legacy initialize with token succeeds" || bad "legacy initialize with token failed"
code="$(curl -s -o /dev/null -w '%{http_code}' -m 10 "${LH[@]}" -H "Mcp-Session-Id: $SID" -H 'MCP-Protocol-Version: 2025-11-25' -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' "$MCP")"
[[ "$code" == 401 ]] && ok "legacy follow-up request without token -> 401 (token required on every request)" || bad "legacy follow-up without token should be 401 (got $code)"
bob="$(mint "sub=bob&scope=mcp:use&aud=$RES&client_id=other-client")"
code="$(curl -s -o /dev/null -w '%{http_code}' -m 10 "${LH[@]}" -H "Authorization: Bearer $bob" -H "Mcp-Session-Id: $SID" -H 'MCP-Protocol-Version: 2025-11-25' -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' "$MCP")"
[[ "$code" == 404 || "$code" == 403 ]] && ok "another principal cannot use alice's session ($code)" || bad "session hijack: bob used alice's session (got $code)"
code="$(curl -s -o /dev/null -w '%{http_code}' -m 10 "${LH[@]}" -H "Authorization: Bearer $tok" -H "Mcp-Session-Id: $SID" -H 'MCP-Protocol-Version: 2025-11-25' -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' "$MCP")"
[[ "$code" == 200 ]] && ok "owner can continue the session" || bad "session owner rejected (got $code)"

echo "-- introspection (opaque tokens)"
PI="$(free_port)"; RES2="http://127.0.0.1:$PI/mcp"
printf 'secret' >"$LOG_DIR/isecret"
if start_srv "$PI" --mode dual --auth-issuer "$ISS" --auth-resource "$RES2" --auth-scopes-supported "mcp:use" --auth-insecure-http-for-testing \
     --auth-introspect-url "$ISS/introspect" --auth-introspect-client-id rs-client --auth-introspect-secret-file "$LOG_DIR/isecret" --auth-introspect-cache-s 1; then
  ot="$(mint "sub=alice&scope=mcp:use&aud=$RES2&opaque=1")"
  req "$RES2" "$(bearer "$ot")" tools/list "" '{}'; [[ "$CODE" == 200 ]] && ok "opaque token accepted via introspection" || bad "opaque token rejected (HTTP $CODE)"
  curl -s -m 10 -o /dev/null -G --data-urlencode "token=$ot" "$ISS/__test/revoke"; sleep 2.5
  req "$RES2" "$(bearer "$ot")" tools/list "" '{}'; [[ "$CODE" == 401 ]] && ok "revoked opaque token rejected after cache TTL" || bad "revoked token still accepted (HTTP $CODE)"
  wt="$(mint "sub=alice&scope=mcp:use&aud=https://other.example/mcp&opaque=1")"
  req "$RES2" "$(bearer "$wt")" tools/list "" '{}'; [[ "$CODE" == 401 ]] && ok "introspection: wrong audience rejected" || bad "introspection accepted a token for another audience"
  acfg "introspect_fail=1"; ot2="$(mint "sub=alice&scope=mcp:use&aud=$RES2&opaque=1")"
  req "$RES2" "$(bearer "$ot2")" tools/list "" '{}'; [[ "$CODE" == 401 || "$CODE" == 503 ]] && ok "introspection outage fails closed ($CODE)" || bad "introspection outage must fail closed (got $CODE)"
  acfg "introspect_fail=0"
else bad "introspection server failed to start"; fi

echo "-- configuration guards and stdio"
"$HTTP_BIN" --port "$(free_port)" --auth-issuer http://auth.example.com --auth-resource http://example.com/mcp >"$LOG_DIR/guard.log" 2>&1 &
GP=$!; sleep 1.5
if kill -0 "$GP" 2>/dev/null; then kill "$GP" 2>/dev/null; bad "server started with a non-HTTPS, non-loopback issuer and no testing flag"; else wait "$GP" 2>/dev/null; ok "refuses http:// non-loopback issuer without --auth-insecure-http-for-testing"; fi
if [[ -n "$STDIO_BIN" ]]; then
  out="$(jq -nc '{jsonrpc:"2.0",id:1,method:"tools/list",params:{_meta:{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{name:"v",version:"0"},"io.modelcontextprotocol/clientCapabilities":{}}}}' | timeout 10 "$STDIO_BIN" 2>/dev/null | head -n1)"
  echo "$out" | jq -e '.result.tools|length>0' >/dev/null 2>&1 && ok "stdio server unaffected by HTTP auth" || bad "stdio server broken"
fi

# ------------------------------------------------------------------ slice 2
if (( SLICE >= 2 )); then
  echo "== slice 2: scope-based authorization, AuthContext, streams"
  areset; CAPS='{"elicitation":{"form":{}}}'
  tmin="$(mint "$BASE")"
  scope_of() { echo "$WWW" | sed -n 's/.*scope="\([^"]*\)".*/\1/p'; }
  req "$MCP" "$(bearer "$tmin")" tools/call delete_project '{"name":"delete_project","arguments":{"project":"dev"}}'
  echo "-- insufficient scope: $CODE $WWW"
  [[ "$CODE" == 403 && "$WWW" == *insufficient_scope* ]] && ok "missing scope -> 403 insufficient_scope" || bad "missing scope should be 403 insufficient_scope (got $CODE)"
  [[ " $(scope_of) " == *" projects:write "* ]] && ok "challenge scope names projects:write" || bad "challenge scope should name projects:write (got: $(scope_of))"
  [[ "$WWW" == *"resource_metadata="* ]] && ok "403 challenge carries resource_metadata" || bad "403 challenge lacks resource_metadata"
  req "$MCP" "$(bearer "$tmin")" tools/call delete_project '{"name":"delete_project","arguments":{"project":"prod"}}'
  [[ "$CODE" == 403 && " $(scope_of) " == *" projects:admin "* ]] && ok "dynamic requirement: prod needs projects:admin" || bad "prod should require projects:admin (got $CODE / $(scope_of))"
  req "$MCP" "$(bearer "$tmin")" tools/call export_report '{"name":"export_report","arguments":{}}'
  s=" $(scope_of) "; [[ "$CODE" == 403 && "$s" == *" reports:read "* && "$s" == *" files:write "* ]] && ok "all missing scopes in ONE challenge" || bad "challenge must list reports:read and files:write together (got: $(scope_of))"
  tw="$(mint "sub=alice&scope=mcp:use projects:write&aud=$RES&client_id=test-client")"
  req "$MCP" "$(bearer "$tw")" tools/call delete_project '{"name":"delete_project","arguments":{"project":"dev"}}'; r="$(final)"
  echo "$r" | jq -e '.result.resultType=="input_required"' >/dev/null 2>&1 && ok "sufficient scope reaches the handler (MRTR round 1)" || bad "authorized call did not reach the handler (HTTP $CODE: ${r:0:160})"
  req "$MCP" "$(bearer "$tw")" tools/call delete_project '{"name":"delete_project","arguments":{"project":"prod"}}'; [[ "$CODE" == 403 ]] && ok "projects:write is not enough for prod" || bad "prod accepted with projects:write"
  ta="$(mint "sub=alice&scope=mcp:use projects:admin&aud=$RES&client_id=test-client")"
  req "$MCP" "$(bearer "$ta")" tools/call delete_project '{"name":"delete_project","arguments":{"project":"dev"}}'; r="$(final)"
  echo "$r" | jq -e '.result.resultType=="input_required"' >/dev/null 2>&1 && ok "scope hierarchy: projects:admin implies projects:write" || bad "hierarchy not honored"
  tf="$(mint "sub=alice&scope=mcp:use files:write&aud=$RES&client_id=test-client")"
  req "$MCP" "$(bearer "$tmin")" resources/read "demo://files/hello.txt" '{"uri":"demo://files/hello.txt"}'; [[ "$CODE" == 403 && " $(scope_of) " == *" files:read "* ]] && ok "resource template requires files:read" || bad "file resource should require files:read (got $CODE / $(scope_of))"
  req "$MCP" "$(bearer "$tf")" resources/read "demo://files/hello.txt" '{"uri":"demo://files/hello.txt"}'; r="$(final)"; echo "$r" | jq -e '.result.contents[0].text=="hello world"' >/dev/null 2>&1 && ok "files:write implies files:read" || bad "files:write should imply files:read"
  req "$MCP" "$(bearer "$tmin")" resources/read "demo://secret/needs_confirm" '{"uri":"demo://secret/needs_confirm"}'; [[ "$CODE" == 403 && " $(scope_of) " == *" secrets:read "* ]] && ok "static resource scope enforced" || bad "secret resource scope not enforced"
  req "$MCP" "$(bearer "$tmin")" tools/list "" '{}'; r="$(final)"; echo "$r" | jq -e '[.result.tools[].name]|index("delete_project")!=null' >/dev/null 2>&1 && ok "show_all listing exposes tools needing more scope (step-up is discoverable)" || bad "default listing should show all tools"
  req "$MCP" "$(bearer "$tmin")" tools/call whoami '{"name":"whoami","arguments":{}}'; r="$(final)"; echo "-- whoami: ${r:0:300}"
  echo "$r" | jq -e '.result.structuredContent // (.result.content[0].text|fromjson) | (.sub=="alice") and (.client_id=="test-client") and (.scopes|index("mcp:use")!=null)' >/dev/null 2>&1 && ok "AuthContext visible in handlers (sub, client_id, scopes)" || bad "whoami did not report the authenticated principal"
  echo "$r" | grep -qF "$tmin" && bad "whoami leaked the raw token" || ok "raw token not exposed to handlers"
  # requestState bound to the authenticated subject
  req "$MCP" "$(bearer "$tw")" tools/call delete_project '{"name":"delete_project","arguments":{"project":"dev"}}'; r="$(final)"
  K="$(echo "$r" | jq -r '((.result.inputRequests // {}) | keys[0]) // empty' 2>/dev/null)"; S="$(echo "$r" | jq -r '.result.requestState // empty' 2>/dev/null)"
  if [[ -n "$K" && -n "$S" && -s scripts/fixtures/mrtr_accept_response.json ]]; then
    A="$(sed "s/__KEY__/$K/g" scripts/fixtures/mrtr_accept_response.json)"
    retry_body="$(jq -nc --arg s "$S" --argjson ir "$A" --argjson caps "$CAPS" '{jsonrpc:"2.0",id:2,method:"tools/call",params:{name:"delete_project",arguments:{project:"dev"},inputResponses:$ir,requestState:$s,_meta:{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{name:"verify",version:"0"},"io.modelcontextprotocol/clientCapabilities":$caps}}}')"
    bobw="$(mint "sub=bob&scope=mcp:use projects:write&aud=$RES&client_id=test-client")"
    out="$(curl -s -m 20 "${MH[@]}" -H 'Mcp-Method: tools/call' -H 'Mcp-Name: delete_project' -H "Authorization: Bearer $bobw" -d "$retry_body" "$MCP")"
    echo "$out" | grep -q 'deleted dev' && bad "alice's requestState was accepted for bob" || ok "requestState minted for alice is rejected for bob"
    out="$(curl -s -m 20 "${MH[@]}" -H 'Mcp-Method: tools/call' -H 'Mcp-Name: delete_project' -H "Authorization: Bearer $tw" -d "$retry_body" "$MCP")"
    echo "$out" | grep -q 'deleted dev' && ok "same principal can complete the continuation" || bad "owner could not complete the continuation (${out:0:160})"
    twl="$(mint "sub=alice&scope=mcp:use&aud=$RES&client_id=test-client")"
    out="$(curl -s -m 20 -w '\n%{http_code}' "${MH[@]}" -H 'Mcp-Method: tools/call' -H 'Mcp-Name: delete_project' -H "Authorization: Bearer $twl" -d "$retry_body" "$MCP")"
    echo "$out" | tail -n1 | grep -q 403 && ok "authorization re-checked on the retry (scope dropped -> 403)" || bad "retry bypassed the scope check"
  else warn "MRTR/fixture unavailable; requestState subject-binding checks skipped"; fi
  # filtered listing server
  PF="$(free_port)"; RESF="http://127.0.0.1:$PF/mcp"
  if start_srv "$PF" --mode dual --auth-issuer "$ISS" --auth-resource "$RESF" --auth-scopes-supported "mcp:use projects:write" --auth-insecure-http-for-testing --auth-listing filter; then
    t="$(mint "sub=alice&scope=mcp:use&aud=$RESF&client_id=test-client")"; req "$RESF" "$(bearer "$t")" tools/list "" '{}'; r="$(final)"
    echo "$r" | jq -e '[.result.tools[].name]|(index("delete_project")==null) and (index("add")!=null)' >/dev/null 2>&1 && ok "--auth-listing filter hides tools the token cannot call" || bad "filtered listing wrong"
  else bad "filter-listing server failed to start"; fi
  # streams end when the token expires
  if [[ -s scripts/fixtures/listen_request.json ]]; then
    ts="$(mint "sub=alice&scope=mcp:use&aud=$RES&client_id=test-client&ttl=4")"
    st=$(date +%s); curl -sN -m 12 -o "$LOG_DIR/ls.body" "${MH[@]}" -H 'Mcp-Method: subscriptions/listen' -H "Authorization: Bearer $ts" -d @scripts/fixtures/listen_request.json "$MCP" >/dev/null 2>&1; rc=$?
    el=$(( $(date +%s) - st ))
    (( rc == 0 && el <= 9 )) && ok "listen stream closed by the server at token expiry (${el}s)" || bad "listen stream outlived its token (rc=$rc, ${el}s)"
    code="$(curl -s -o /dev/null -w '%{http_code}' -m 10 "${MH[@]}" -H 'Mcp-Method: subscriptions/listen' -d @scripts/fixtures/listen_request.json "$MCP")"; [[ "$code" == 401 ]] && ok "listen without token -> 401" || bad "listen without token should be 401"
  else warn "scripts/fixtures/listen_request.json missing; stream-expiry check skipped"; fi
fi

# ------------------------------------------------------------------ slice 3
if (( SLICE >= 3 )); then
  echo "== slice 3: client OAuth flow"
  CALL=(--url "$MCP" --mode modern --call add --args '{"a":1,"b":2}' --oauth --oauth-launcher http-follow --oauth-allow-insecure-loopback)
  areset; out="$(run_client "${CALL[@]}" --oauth-scopes "mcp:use")"; rc=$?; echo "$out" | head -n 14
  (( rc == 0 )) && ok "OAuth flow with CIMD completes (modern)" || bad "OAuth flow failed (rc=$rc)"
  echo "$out" | grep -q '^AUTH registration cimd' && ok "registration: CIMD preferred when advertised" || bad "expected 'AUTH registration cimd'"
  echo "$out" | grep -qE '^CALL add -> .*3' && ok "authenticated call succeeded" || bad "authenticated call failed"
  echo "$out" | grep -q "^AUTH discovery prm " && ok "PRM discovery event" || bad "no PRM discovery event"
  s="$(astats)"
  echo "$s" | jq -e --arg r "$RES" '.authorize[0] | (.code_challenge_method=="S256") and (.resource==$r) and (.has_state==true) and (.client_id|test("^https?://"))' >/dev/null 2>&1 && ok "authorize request: S256, resource, state, URL client_id" || bad "authorize request malformed ($(echo "$s" | jq -c '.authorize[0]'))"
  echo "$s" | jq -e --arg r "$RES" '.token[0] | (.grant_type=="authorization_code") and (.resource==$r) and (.has_code_verifier==true)' >/dev/null 2>&1 && ok "token request: code_verifier and resource" || bad "token request malformed"
  [[ "$(echo "$s" | jq -r '.bearer_on_non_resource_endpoints')" == 0 ]] && ok "no bearer token sent to authorization-server endpoints" || bad "bearer token leaked to AS endpoints"
  out="$(run_client "${CALL[@]}" --oauth-scopes "mcp:use" --mode legacy)"; echo "$out" | grep -qE '^CALL add -> .*3' && ok "OAuth flow works in legacy mode (token on every request)" || bad "legacy OAuth flow failed"
  areset; acfg "cimd=0"; out="$(run_client "${CALL[@]}" --oauth-scopes "mcp:use" --oauth-dcr)"
  echo "$out" | grep -q '^AUTH registration dcr' && echo "$out" | grep -qE '^CALL add' && ok "falls back to DCR when CIMD unsupported" || bad "DCR fallback failed"
  echo "$(astats)" | jq -e '.register[0].application_type=="native"' >/dev/null 2>&1 && ok "DCR sets application_type=native" || bad "DCR must send application_type"
  areset; acfg "cimd=0&dcr=0"; out="$(run_client "${CALL[@]}" --oauth-scopes "mcp:use")"; rc=$?
  (( rc != 0 )) && echo "$out" | grep -q 'AUTH failed no_registration' && ok "no registration mechanism -> clean failure" || bad "expected 'AUTH failed no_registration'"
  areset; out="$(run_client "${CALL[@]}" --oauth-scopes "mcp:use" --oauth-client-id preset-client)"
  echo "$out" | grep -q '^AUTH registration prereg' && echo "$(astats)" | jq -e '.authorize[0].client_id=="preset-client"' >/dev/null 2>&1 && ok "pre-registered client wins over CIMD" || bad "pre-registered priority not honored"
  echo "-- security checks"
  areset; acfg "pkce=0"; out="$(run_client "${CALL[@]}" --oauth-scopes "mcp:use")"; rc=$?
  (( rc != 0 )) && echo "$out" | grep -q 'AUTH failed pkce_unsupported' && [[ "$(astats | jq '.authorize|length')" == 0 ]] && ok "refuses AS without code_challenge_methods_supported (no authorize request sent)" || bad "must refuse without PKCE support"
  areset; acfg "issuer_doc=bad"; out="$(run_client "${CALL[@]}" --oauth-scopes "mcp:use")"; rc=$?
  (( rc != 0 )) && echo "$out" | grep -q 'AUTH failed issuer_mismatch' && ok "rejects AS metadata whose issuer differs from the URL" || bad "issuer mismatch in metadata not rejected"
  areset; acfg "iss_param=bad&iss_advertised=1"; out="$(run_client "${CALL[@]}" --oauth-scopes "mcp:use")"; rc=$?
  (( rc != 0 )) && echo "$out" | grep -q 'AUTH failed iss_mismatch' && [[ "$(astats | jq '.token|length')" == 0 ]] && ok "mix-up: wrong iss -> code never sent to the token endpoint" || bad "wrong iss must abort before the token request"
  echo "$out" | grep -qiE 'error_description|access_denied' && bad "client displayed error parameters from an unvalidated response" || ok "no error parameters shown after iss mismatch"
  areset; acfg "iss_param=absent&iss_advertised=1"; out="$(run_client "${CALL[@]}" --oauth-scopes "mcp:use")"; rc=$?
  (( rc != 0 )) && echo "$out" | grep -q 'AUTH failed iss_missing' && ok "iss absent while advertised -> rejected" || bad "missing iss must be rejected when advertised"
  areset; acfg "iss_param=absent&iss_advertised=0"; out="$(run_client "${CALL[@]}" --oauth-scopes "mcp:use")"; echo "$out" | grep -qE '^CALL add' && ok "iss absent and not advertised -> proceeds" || bad "client should proceed when iss is neither advertised nor present"
  areset; acfg "state=bad"; out="$(run_client "${CALL[@]}" --oauth-scopes "mcp:use")"; rc=$?
  (( rc != 0 )) && echo "$out" | grep -q 'AUTH failed state_mismatch' && ok "state mismatch rejected" || bad "state mismatch must be rejected"
  echo "-- refresh, step-up, concurrency, store"
  areset; acfg "ttl=3&rotate=1"
  out="$(run_client "${CALL[@]}" --oauth-scopes "mcp:use" --repeat 3 --repeat-delay-ms 4500)"; echo "$out" | grep -E '^(AUTH refresh|CALL)' | head -n 6
  [[ "$(echo "$out" | grep -c '^CALL add')" == 3 ]] && echo "$out" | grep -q '^AUTH refresh' && [[ "$(astats | jq '.authorize|length')" == 1 ]] && ok "expired tokens refreshed (rotating refresh tokens), one interactive authorization" || bad "refresh flow wrong"
  areset; out="$(run_client "${CALL[@]}" --oauth-scopes "mcp:use" --parallel 5)"
  [[ "$(astats | jq '.authorize|length')" == 1 ]] && ok "single-flight: 5 concurrent calls trigger one authorization" || bad "concurrent 401s started several authorizations"
  areset; SC=(--url "$MCP" --mode modern --call delete_project --args '{"project":"dev"}' --elicit accept --elicit-content '{"confirm":true}' --oauth --oauth-launcher http-follow --oauth-allow-insecure-loopback --oauth-scopes "mcp:use")
  out="$(run_client "${SC[@]}")"; rc=$?; echo "$out" | grep -E '^AUTH (stepup|authorize|token)' | head -n 5
  echo "$out" | grep -q '^AUTH stepup scope=.*mcp:use.*projects:write\|^AUTH stepup scope=.*projects:write.*mcp:use' && ok "step-up requests the union of previous and challenged scopes" || bad "step-up must request mcp:use + projects:write"
  echo "$out" | grep -q 'deleted dev' && ok "operation retried and succeeded after step-up" || bad "operation did not complete after step-up"
  areset; acfg "grant_limit=mcp:use"; out="$(run_client "${SC[@]}")"; rc=$?
  n="$(astats | jq '.authorize|length')"
  (( rc != 0 )) && echo "$out" | grep -q 'AUTH failed stepup_limit' && (( n <= 4 )) && ok "step-up retry limit enforced ($n authorizations)" || bad "step-up loop not bounded (rc=$rc, $n authorizations)"
  areset; ST="$LOG_DIR/store.json"; rm -f "$ST"
  out="$(run_client "${CALL[@]}" --oauth-scopes "mcp:use" --oauth-store-file "$ST")"; a1="$(astats | jq '.authorize|length')"
  out2="$(run_client "${CALL[@]}" --oauth-scopes "mcp:use" --oauth-store-file "$ST")"; a2="$(astats | jq '.authorize|length')"
  [[ "$a1" == 1 && "$a2" == 1 ]] && echo "$out2" | grep -qE '^CALL add' && ok "stored token reused across runs (no second authorization)" || bad "token store not reused ($a1 -> $a2)"
  perm="$(stat -c '%a' "$ST" 2>/dev/null || stat -f '%Lp' "$ST" 2>/dev/null)"; [[ "$perm" == 600 ]] && ok "token store file mode 0600" || bad "token store permissions are $perm (need 600)"
  grep -q "$(echo "$ISS")" "$ST" && ok "store entries are keyed by issuer" || bad "store lacks issuer keys"
  # second authorization server => separate registration state
  PB="$(free_port)"; ISSB="http://127.0.0.1:$PB"
  if start_as "$PB"; then
    PM2="$(free_port)"; RES3="http://127.0.0.1:$PM2/mcp"
    if start_srv "$PM2" --mode dual --auth-issuer "$ISSB" --auth-resource "$RES3" --auth-scopes-supported "mcp:use" --auth-insecure-http-for-testing; then
      areset; acfg "cimd=0" ; acfg "cimd=0" "$ISSB"
      r1="$(run_client --url "$MCP" --mode modern --call add --args '{"a":1,"b":2}' --oauth --oauth-dcr --oauth-launcher http-follow --oauth-allow-insecure-loopback --oauth-scopes mcp:use --oauth-store-file "$LOG_DIR/store2.json")"
      r2="$(run_client --url "$RES3" --mode modern --call add --args '{"a":1,"b":2}' --oauth --oauth-dcr --oauth-launcher http-follow --oauth-allow-insecure-loopback --oauth-scopes mcp:use --oauth-store-file "$LOG_DIR/store2.json")"
      c1="$(astats | jq -r '.authorize[0].client_id')"; c2="$(astats "$ISSB" | jq -r '.authorize[0].client_id')"
      [[ -n "$c1" && -n "$c2" && "$c1" != "$c2" ]] && ok "credentials of AS-A were not reused at AS-B (separate DCR)" || bad "client credentials leaked across authorization servers ($c1 / $c2)"
    else bad "second MCP server failed to start"; fi
  else bad "second authorization server failed to start"; fi
  echo "-- discovery edge cases and SSRF"
  PN="$(free_port)"; RESN="http://127.0.0.1:$PN/mcp"
  if start_srv "$PN" --mode dual --auth-issuer "$ISS" --auth-resource "$RESN" --auth-scopes-supported "mcp:use" --auth-insecure-http-for-testing --auth-omit-resource-metadata-param; then
    areset; out="$(run_client --url "$RESN" --mode modern --call add --args '{"a":1,"b":2}' --oauth --oauth-launcher http-follow --oauth-allow-insecure-loopback --oauth-scopes mcp:use)"
    echo "$out" | grep -qE '^CALL add' && ok "falls back to well-known probing when resource_metadata is absent" || bad "well-known fallback failed"
  else bad "fallback-discovery server failed to start"; fi
  PW="$(free_port)"; RESW="http://127.0.0.1:$PW/mcp"
  if start_srv "$PW" --mode dual --auth-issuer "$ISS" --auth-resource "https://elsewhere.example/mcp" --auth-scopes-supported "mcp:use" --auth-insecure-http-for-testing; then
    out="$(run_client --url "$RESW" --mode modern --call add --args '{"a":1,"b":2}' --oauth --oauth-launcher http-follow --oauth-allow-insecure-loopback --oauth-scopes mcp:use)"; rc=$?
    (( rc != 0 )) && echo "$out" | grep -q 'AUTH failed resource_mismatch' && ok "PRM whose resource differs from the server URL is rejected" || bad "PRM resource mismatch not detected"
  else bad "mismatch server failed to start"; fi
  PS="$(free_port)"
  if start_srv "$PS" --mode dual --auth-issuer "http://10.255.255.1" --auth-resource "http://127.0.0.1:$PS/mcp" --auth-scopes-supported "mcp:use" --auth-insecure-http-for-testing; then
    st=$(date +%s); out="$(CLIENT_TIMEOUT=20 run_client --url "http://127.0.0.1:$PS/mcp" --mode modern --call add --args '{"a":1,"b":2}' --oauth --oauth-launcher http-follow --oauth-allow-insecure-loopback --oauth-scopes mcp:use)"; rc=$?; el=$(( $(date +%s) - st ))
    (( rc != 0 && el < 8 )) && echo "$out" | grep -q 'AUTH failed blocked_address' && ok "SSRF: private-range authorization server blocked without connecting (${el}s)" || bad "client contacted or hung on a private-range authorization server (rc=$rc, ${el}s)"
  else bad "SSRF server failed to start"; fi
  areset; cs="$LOG_DIR/csecret"; printf 'cc-secret' >"$cs"
  out="$(run_client --url "$MCP" --mode modern --call add --args '{"a":1,"b":2}' --oauth --oauth-grant client_credentials --oauth-client-id svc-client --oauth-client-secret-file "$cs" --oauth-scopes mcp:use --oauth-allow-insecure-loopback)"
  echo "$out" | grep -qE '^CALL add -> .*3' && echo "$(astats)" | jq -e '.token[0].grant_type=="client_credentials"' >/dev/null 2>&1 && ok "client_credentials flow works (resource-bound)" || bad "client_credentials flow failed"
fi

# ------------------------------------------------------------------ slice 4
if (( SLICE >= 4 )); then
  echo "== slice 4: tests, docs, evidence"
  lst="$(ctest --test-dir "$BUILD" -N 2>/dev/null)"; n="$(echo "$lst" | grep -c 'Test *#')"
  (( n >= 420 )) && ok "ctest discovers $n tests" || bad "only $n tests discovered (need >= 420)"
  for pat in prm_ jwt_ jwks_ introspection scope_hierarchy scope_single_challenge session_binding www_authenticate oauth_discovery oauth_pkce oauth_iss oauth_stepup oauth_refresh oauth_single_flight oauth_store_issuer oauth_ssrf token_passthrough token_redaction client_mrtr; do
    echo "$lst" | grep -qi "$pat" && ok "ctest has a '$pat' test" || bad "no ctest test name matches '$pat'"
  done
  check "ctest passes" ctest --test-dir "$BUILD" --output-on-failure
  for f in docs/AUTH.md docs/SECURITY.md docs/6_EVIDENCE.md docs/DECISIONS.md docs/SPEC_NOTES.md docs/INTEROP.md; do check "$f present" test -s "$f"; done
  check "AUTH.md covers token passthrough" grep -qi 'passthrough' docs/AUTH.md
  check "SECURITY.md covers JWT algorithm confusion" grep -qiE 'algorithm confusion|alg=none' docs/SECURITY.md
  check "SECURITY.md covers SSRF policy" grep -qi 'SSRF' docs/SECURITY.md
  check ">= 15 fuzz targets" bash -c "[ \$(git grep -l LLVMFuzzerTestOneInput | wc -l) -ge 15 ]"
  check "no TODO/placeholder in evidence" bash -c "! grep -qiE 'TODO|TBD|placeholder' docs/6_EVIDENCE.md"
  check "AuthContext has no raw token accessor" bash -c "! git grep -nE 'raw_token|access_token\(\)' -- include/cppmcp/server include/cppmcp/auth"
  check "installed-consumer verification passes" bash -c "[ ! -x scripts/verify_install.sh ] || scripts/verify_install.sh"
fi

if (( INTEROP == 1 )) && command -v npx >/dev/null 2>&1; then
  echo "== interop (informational)"
  t="$(mint "$BASE")"
  out="$(timeout 90 npx -y @modelcontextprotocol/inspector --cli "$MCP" --transport http --header "Authorization: Bearer $t" --method tools/list 2>/dev/null </dev/null)"
  echo "$out" | grep -q '"add"' && ok "Inspector CLI with a bearer header" || warn "Inspector with --header failed (flags may differ)"
  out="$(timeout 60 npx -y @modelcontextprotocol/inspector --cli "$MCP" --transport http --method tools/list 2>&1 </dev/null)"
  echo "$out" | grep -qiE '401|unauthor' && ok "Inspector without token sees 401" || warn "Inspector without token: unexpected output"
  warn "Claude Code OAuth login against this server is interactive; run it manually and record it in docs/INTEROP.md"
fi

if (( SAN == 1 )); then
  echo "== sanitizers"
  for cfg in "address,undefined" "thread"; do
    d="build-san-${cfg//,/-}"; flags="-fsanitize=$cfg -fno-omit-frame-pointer -g"
    if cmake -S . -B "$d" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=$cfg" >"$LOG_DIR/$d.cfg" 2>&1 \
       && cmake --build "$d" -j"$JOBS" >"$LOG_DIR/$d.build" 2>&1; then
      ctest --test-dir "$d" --output-on-failure --timeout 300 >"$LOG_DIR/$d.test" 2>&1; rc=$?
      if (( rc == 0 )) && ! grep -qE 'ERROR: (Address|Leak)Sanitizer|WARNING: ThreadSanitizer|runtime error:' "$LOG_DIR/$d.test"; then ok "sanitizer [$cfg]: tests pass, zero findings"
      else bad "sanitizer [$cfg]: failures or findings (see $LOG_DIR/$d.test)"; fi
    else bad "sanitizer [$cfg]: build failed (see $LOG_DIR/$d.build)"; fi
  done
fi

echo
echo "RESULT: $PASS passed, $FAIL failed, $WARN warnings (logs: $LOG_DIR)"
(( FAIL == 0 ))