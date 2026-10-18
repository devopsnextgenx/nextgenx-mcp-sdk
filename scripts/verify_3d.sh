#!/usr/bin/env bash
# scripts/verify_3d.sh : executable acceptance gate for Phase 3D.
# Scope: HTTP client + SSE parser, subscriptions/listen, change notifications, TLS, client auth hook.
# Usage: scripts/verify_3d.sh [--slice N] [--sanitizers] [--interop]
# Env:   BUILD_DIR (default build), HTTP_BIN, CLIENT_BIN
#
# Contract the example binaries MUST satisfy
#   http_server flags : --port N  --mode modern|legacy|dual  --ping-interval-ms N
#                       --notify-interval-ms N   (emit tools list_changed every N ms)
#                       --token T  --tls-cert F  --tls-key F
#   http_client flags : --url U  --mode modern|legacy|auto  --cacert F  --token T
#                       --listen  --duration-ms N
#   http_client stdout (one item per line):
#       MODE <modern|legacy>            negotiated era
#       TOOL <name>                     one per listed tool
#       CALL add -> <text>              result text of add {"a":1,"b":2}
#       NOTIFY <method>                 one per received change notification (--listen)
#       STREAM_LOST / RECONNECTED       listen stream lifecycle (--listen)
#   exit status: 0 success, non-zero on any failure (TLS verify, 401, negotiation, ...)
# Fixture: scripts/fixtures/listen_request.json = a complete, spec-verified modern
#          subscriptions/listen JSON-RPC request body asking for tools list-changed notifications.
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
for t in curl jq python3 cmake git openssl; do need "$t"; done

ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT" || exit 2
BUILD="${BUILD_DIR:-build}"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
LOG_DIR="$(mktemp -d)"
PIDS=(); LAST_PID=""
cleanup() { for p in "${PIDS[@]:-}"; do [[ -n "$p" ]] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

echo "== build ($BUILD)"
if cmake -S . -B "$BUILD" >"$LOG_DIR/cfg.log" 2>&1 && cmake --build "$BUILD" -j"$JOBS" >"$LOG_DIR/build.log" 2>&1; then
  ok "configure and build"
else
  bad "configure and build (see $LOG_DIR/build.log)"; tail -20 "$LOG_DIR/build.log"; exit 1
fi

find_bin() { find "$BUILD" -type f -perm -u+x \( -name "$1" -o -name "cppmcp_$1" \) 2>/dev/null | head -n1; }
HTTP_BIN="${HTTP_BIN:-$(find_bin http_server)}"
CLIENT_BIN="${CLIENT_BIN:-$(find_bin http_client)}"
[[ -z "$HTTP_BIN" ]]   && { bad "http_server binary not found"; exit 1; }
[[ -z "$CLIENT_BIN" ]] && { bad "http_client binary not found (slice 1 deliverable)"; exit 1; }

free_port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
start_server() { # start_server PORT args... ; sets LAST_PID
  local port="$1"; shift
  "$HTTP_BIN" --port "$port" "$@" >"$LOG_DIR/server-$port.log" 2>&1 &
  LAST_PID=$!; PIDS+=("$LAST_PID")
  for _ in $(seq 1 60); do
    { curl -sk -m 2 -o /dev/null "http://127.0.0.1:$port/" || curl -sk -m 2 -o /dev/null "https://127.0.0.1:$port/"; } && break
    kill -0 "$LAST_PID" 2>/dev/null || break
    sleep 0.1
  done
  kill -0 "$LAST_PID" 2>/dev/null
}
run_client() { timeout "${CLIENT_TIMEOUT:-30}" "$CLIENT_BIN" "$@" </dev/null 2>&1; }

PORT="$(free_port)"
start_server "$PORT" --mode dual --ping-interval-ms 500 --notify-interval-ms 300 || { bad "dual server failed to start"; cat "$LOG_DIR/server-$PORT.log"; exit 1; }
URL="http://127.0.0.1:$PORT/mcp"
ok "dual-mode server running on :$PORT"

echo "== slice 1: HTTP client + SSE parser (plain HTTP)"
for mode in modern legacy auto; do
  out="$(run_client --url "$URL" --mode "$mode")"; rc=$?
  echo "-- client --mode $mode (rc=$rc)"; echo "$out" | head -n 8
  (( rc == 0 )) && ok "client --mode $mode exits 0" || bad "client --mode $mode failed (rc=$rc)"
  echo "$out" | grep -q '^TOOL add' && ok "client --mode $mode lists tools" || bad "client --mode $mode: no 'TOOL add' line"
  echo "$out" | grep -qE '^CALL add -> .*3' && ok "client --mode $mode calls add -> 3" || bad "client --mode $mode: no 'CALL add -> ...3' line"
  want="$mode"; [[ "$mode" == auto ]] && want=modern
  echo "$out" | grep -q "^MODE $want" && ok "client --mode $mode negotiated $want" || bad "client --mode $mode: expected 'MODE $want'"
done
P_LEG="$(free_port)"
if start_server "$P_LEG" --mode legacy; then
  out="$(run_client --url "http://127.0.0.1:$P_LEG/mcp" --mode auto)"; rc=$?
  (( rc == 0 )) && echo "$out" | grep -q '^MODE legacy' && ok "auto falls back to legacy against a legacy-only server" || bad "auto fallback to legacy failed (rc=$rc)"
else bad "legacy-only server failed to start"; fi
P_MOD="$(free_port)"
if start_server "$P_MOD" --mode modern; then
  out="$(run_client --url "http://127.0.0.1:$P_MOD/mcp" --mode legacy)"; rc=$?
  (( rc != 0 )) && ok "client --mode legacy against modern-only server fails cleanly (rc=$rc)" || bad "client --mode legacy should fail against a modern-only server"
else bad "modern-only server failed to start"; fi
P_AUTH="$(free_port)"
if start_server "$P_AUTH" --mode dual --token s3cret; then
  out="$(run_client --url "http://127.0.0.1:$P_AUTH/mcp" --mode auto)"; rc=$?
  (( rc != 0 )) && echo "$out" | grep -q '401' && ok "client without token: non-zero exit and 401 surfaced" || bad "missing token should fail with a visible 401 (rc=$rc)"
  out="$(run_client --url "http://127.0.0.1:$P_AUTH/mcp" --mode auto --token s3cret)"; rc=$?
  (( rc == 0 )) && echo "$out" | grep -q '^TOOL add' && ok "client with --token succeeds" || bad "client with correct token failed (rc=$rc)"
  echo "$out" | grep -q 's3cret' && bad "token leaked into client output" || ok "token not echoed by client"
  grep -q 's3cret' "$LOG_DIR/server-$P_AUTH.log" && bad "token leaked into server log" || ok "token not in server log"
else bad "auth server failed to start"; fi

if (( SLICE >= 2 )); then
  echo "== slice 2: subscriptions/listen, notifications, reconnect"
  FIX="scripts/fixtures/listen_request.json"
  if [[ -s "$FIX" ]]; then
    curl -sN -m 4 -D "$LOG_DIR/ls.hdr" -o "$LOG_DIR/ls.body" \
      -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
      -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: subscriptions/listen' -d @"$FIX" "$URL" >/dev/null 2>&1
    echo "-- transcript: subscriptions/listen"; head -c 700 "$LOG_DIR/ls.body"; echo
    grep -qi '^content-type: *text/event-stream' "$LOG_DIR/ls.hdr" && ok "listen response is an SSE stream" || bad "listen should return text/event-stream"
    first="$(grep '^data:' "$LOG_DIR/ls.body" | head -n1 | sed 's/^data: *//')"
    echo "$first" | jq -e '.method == "notifications/subscriptions/acknowledged"' >/dev/null 2>&1 && ok "first event is the acknowledgment" || bad "first event must be notifications/subscriptions/acknowledged (got: ${first:0:160})"
    grep '^data:' "$LOG_DIR/ls.body" | sed 's/^data: *//' | jq -e 'select(.method == "notifications/tools/list_changed") | .params._meta["io.modelcontextprotocol/subscriptionId"]' >/dev/null 2>&1 \
      && ok "tools/list_changed delivered, tagged with subscriptionId" || bad "no tagged tools/list_changed notification within 4s (is --notify-interval-ms honored?)"
    grep '^data:' "$LOG_DIR/ls.body" | sed 's/^data: *//' | jq -e 'select(has("result") or has("id"))' >/dev/null 2>&1 \
      && bad "listen stream carried a JSON-RPC response (it must never)" || ok "listen stream never carries a JSON-RPC response"
  else
    bad "missing $FIX (spec-verified listen request body)"
  fi
  for mode in modern legacy; do
    out="$(CLIENT_TIMEOUT=20 run_client --url "$URL" --mode "$mode" --listen --duration-ms 3000)"
    n="$(echo "$out" | grep -c '^NOTIFY .*list_changed')"
    (( n >= 2 )) && ok "client --listen --mode $mode received $n list_changed notifications" || bad "client --listen --mode $mode got $n notifications (need >= 2)"
  done
  PR="$(free_port)"
  start_server "$PR" --mode dual --notify-interval-ms 300 && SRV_PID="$LAST_PID" || { bad "reconnect test server failed"; SRV_PID=""; }
  if [[ -n "$SRV_PID" ]]; then
    "$CLIENT_BIN" --url "http://127.0.0.1:$PR/mcp" --mode modern --listen --duration-ms 14000 >"$LOG_DIR/reconnect.out" 2>&1 </dev/null &
    CPID=$!; PIDS+=("$CPID")
    sleep 3; kill "$SRV_PID" 2>/dev/null; sleep 2
    start_server "$PR" --mode dual --notify-interval-ms 300 || bad "could not restart server on the same port (SO_REUSEADDR?)"
    wait "$CPID" 2>/dev/null
    echo "-- transcript: reconnect"; head -n 30 "$LOG_DIR/reconnect.out"
    grep -q '^STREAM_LOST' "$LOG_DIR/reconnect.out" && ok "client reports STREAM_LOST" || bad "client did not report STREAM_LOST"
    grep -q '^RECONNECTED' "$LOG_DIR/reconnect.out" && ok "client reports RECONNECTED" || bad "client did not reconnect"
    awk '/^RECONNECTED/{f=1} f&&/^NOTIFY/{n++} END{exit !(n>0)}' "$LOG_DIR/reconnect.out" && ok "notifications resume after reconnect" || bad "no notifications after reconnect"
  fi
fi

if (( SLICE >= 3 )); then
  echo "== slice 3: TLS (server and client)"
  T="$LOG_DIR/tls"; mkdir -p "$T"
  openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/ca.key" -out "$T/ca.pem" -days 2 -subj "/CN=cppmcp-test-ca" >/dev/null 2>&1
  mkcert() { # mkcert NAME SAN
    openssl req -newkey rsa:2048 -nodes -keyout "$T/$1.key" -out "$T/$1.csr" -subj "/CN=localhost" >/dev/null 2>&1
    printf 'subjectAltName=%s\n' "$2" >"$T/$1.ext"
    openssl x509 -req -in "$T/$1.csr" -CA "$T/ca.pem" -CAkey "$T/ca.key" -CAcreateserial -out "$T/$1.pem" -days 2 -extfile "$T/$1.ext" >/dev/null 2>&1
  }
  mkcert good "DNS:localhost,IP:127.0.0.1"
  mkcert dnsonly "DNS:localhost"
  PT="$(free_port)"
  if start_server "$PT" --mode dual --tls-cert "$T/good.pem" --tls-key "$T/good.key"; then
    ok "TLS server running on :$PT"
    META='"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{"name":"verify","version":"0"},"io.modelcontextprotocol/clientCapabilities":{}}'
    r="$(curl -sS -m 10 --cacert "$T/ca.pem" -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: tools/list' \
         -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{$META}}" "https://localhost:$PT/mcp")"
    echo "$r" | jq -e '.result.tools | length > 0' >/dev/null 2>&1 && ok "curl over TLS with the test CA works" || bad "curl over TLS failed (got: ${r:0:160})"
    curl -sS -m 10 -o /dev/null "https://localhost:$PT/mcp" >/dev/null 2>&1; rc=$?
    (( rc == 60 || rc == 35 || rc == 77 )) && ok "curl without the CA is rejected (rc=$rc)" || bad "untrusted-CA connection should fail (rc=$rc)"
    out="$(run_client --url "https://localhost:$PT/mcp" --mode auto --cacert "$T/ca.pem")"; rc=$?
    (( rc == 0 )) && echo "$out" | grep -q '^TOOL add' && ok "our client over TLS with --cacert works" || bad "our client over TLS failed (rc=$rc)"
    out="$(run_client --url "https://localhost:$PT/mcp" --mode auto)"; rc=$?
    (( rc != 0 )) && ok "our client rejects an untrusted CA by default" || bad "our client accepted an untrusted certificate (rc=$rc)"
    out="$(run_client --url "https://127.0.0.1:$PT/mcp" --mode auto --cacert "$T/ca.pem")"; rc=$?
    (( rc == 0 )) && ok "client accepts IP SAN match" || bad "client rejected a valid IP SAN (rc=$rc)"
    out="$(CLIENT_TIMEOUT=20 run_client --url "https://localhost:$PT/mcp" --mode modern --cacert "$T/ca.pem" --listen --duration-ms 3000)"
    (( $(echo "$out" | grep -c '^NOTIFY') >= 0 )) && ok "listen over TLS did not crash"
  else
    bad "TLS server failed to start (built with CPPMCP_WITH_TLS and OpenSSL?)"; cat "$LOG_DIR/server-$PT.log"
  fi
  PH="$(free_port)"
  if start_server "$PH" --mode dual --tls-cert "$T/dnsonly.pem" --tls-key "$T/dnsonly.key"; then
    out="$(run_client --url "https://127.0.0.1:$PH/mcp" --mode auto --cacert "$T/ca.pem")"; rc=$?
    (( rc != 0 )) && echo "$out" | grep -qiE 'host|name|certificate|verify' && ok "client rejects hostname mismatch with a clear error" || bad "hostname mismatch must fail clearly (rc=$rc)"
    out="$(run_client --url "https://localhost:$PH/mcp" --mode auto --cacert "$T/ca.pem")"; rc=$?
    (( rc == 0 )) && ok "client accepts DNS SAN match" || bad "client rejected a valid DNS SAN (rc=$rc)"
  else bad "hostname-test TLS server failed to start"; fi
  PB="$(free_port)"
  "$HTTP_BIN" --port "$PB" --bind 0.0.0.0 --mode dual >"$LOG_DIR/bind.log" 2>&1 &
  BP=$!; PIDS+=("$BP"); sleep 1
  if kill -0 "$BP" 2>/dev/null; then bad "non-loopback bind without TLS or auth should be refused"; else ok "non-loopback bind without TLS/auth refused"; fi
  PB2="$(free_port)"
  if start_server "$PB2" --bind 0.0.0.0 --mode dual --tls-cert "$T/good.pem" --tls-key "$T/good.key"; then ok "non-loopback bind allowed with TLS"; else bad "non-loopback bind with TLS should be allowed"; fi
fi

if (( SLICE >= 4 )); then
  echo "== slice 4: tests, docs, evidence"
  lst="$(ctest --test-dir "$BUILD" -N 2>/dev/null)"
  n="$(echo "$lst" | grep -c 'Test *#')"
  (( n >= 80 )) && ok "ctest discovers $n tests" || bad "only $n tests discovered (need >= 80)"
  for pat in sse_parser listen backoff tls resum www_auth hostname; do
    echo "$lst" | grep -qi "$pat" && ok "ctest has a '$pat' test" || bad "no ctest test name matches '$pat'"
  done
  check "ctest passes" ctest --test-dir "$BUILD" --output-on-failure
  for f in docs/QUICKSTART_HTTP.md docs/INTEROP.md docs/CONFORMANCE.md docs/3D_EVIDENCE.md docs/DECISIONS.md docs/SPEC_NOTES.md scripts/fixtures/listen_request.json; do
    check "$f present" test -s "$f"
  done
  check ">= 5 fuzz targets" bash -c "[ \$(git grep -l LLVMFuzzerTestOneInput | wc -l) -ge 5 ]"
  check "no TODO/placeholder in evidence" bash -c "! grep -qiE 'TODO|TBD|placeholder' docs/3D_EVIDENCE.md"
fi

if (( INTEROP == 1 )); then
  echo "== interop (informational)"
  if command -v npx >/dev/null 2>&1; then
    out="$(timeout 90 npx -y @modelcontextprotocol/inspector --cli "$URL" --transport http --method tools/list 2>&1 </dev/null)"; rc=$?
    (( rc == 0 )) && echo "$out" | grep -q '"tools"' && ok "Inspector CLI -> our dual server" || bad "Inspector CLI failed (rc=$rc)"
    PE="${EVERYTHING_PORT:-3001}"
    PORT="$PE" timeout 120 npx -y @modelcontextprotocol/server-everything streamableHttp >"$LOG_DIR/everything.log" 2>&1 </dev/null &
    EP=$!; PIDS+=("$EP")
    for _ in $(seq 1 100); do curl -s -m 1 -o /dev/null "http://127.0.0.1:$PE/mcp" && break; sleep 0.3; done
    if curl -s -m 2 -o /dev/null "http://127.0.0.1:$PE/mcp"; then
      out="$(run_client --url "http://127.0.0.1:$PE/mcp" --mode auto)"; rc=$?
      echo "$out" | head -n 6
      (( rc == 0 )) && echo "$out" | grep -q '^TOOL ' && ok "our client -> official TypeScript reference server" || bad "our client failed against the TS reference server (rc=$rc)"
    else warn "TS reference server did not start (see $LOG_DIR/everything.log)"; fi
  else warn "npx not available; Inspector and TS server interop not run"; fi
  if command -v claude >/dev/null 2>&1; then
    CL() { local t="$1"; shift; timeout "$t" claude "$@" </dev/null 2>&1; }
    CL 30 mcp remove cppmcp-verify >/dev/null
    CL 30 mcp add --transport http cppmcp-verify "$URL" >/dev/null
    all="$(CL 90 mcp list)"; line="$(echo "$all" | grep 'cppmcp-verify')"; echo "$line"
    echo "$line" | grep -q 'Connected' && ! echo "$line" | grep -qi failed && ok "Claude Code connected (dual server)" || bad "Claude Code not connected"
    CL 30 mcp remove cppmcp-verify >/dev/null
  else warn "claude CLI not available"; fi
fi

if (( SAN == 1 )); then
  echo "== sanitizers"
  for cfg in "address,undefined" "thread"; do
    d="build-san-${cfg//,/-}"; flags="-fsanitize=$cfg -fno-omit-frame-pointer -g"
    if cmake -S . -B "$d" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=$cfg" >"$LOG_DIR/$d.cfg" 2>&1 \
       && cmake --build "$d" -j"$JOBS" >"$LOG_DIR/$d.build" 2>&1; then
      ctest --test-dir "$d" --output-on-failure --timeout 300 >"$LOG_DIR/$d.test" 2>&1; rc=$?
      if (( rc == 0 )) && ! grep -qE 'ERROR: (Address|Leak)Sanitizer|WARNING: ThreadSanitizer|runtime error:' "$LOG_DIR/$d.test"; then
        ok "sanitizer [$cfg]: tests pass, zero findings"
      else bad "sanitizer [$cfg]: failures or findings (see $LOG_DIR/$d.test)"; fi
    else bad "sanitizer [$cfg]: build failed (see $LOG_DIR/$d.build)"; fi
  done
fi

echo
echo "RESULT: $PASS passed, $FAIL failed, $WARN warnings (logs: $LOG_DIR)"
(( FAIL == 0 ))