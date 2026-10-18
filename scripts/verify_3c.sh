#!/usr/bin/env bash
# scripts/verify_3c.sh : executable acceptance gate for Phase 3C (legacy Streamable HTTP + dual mode).
# Usage: scripts/verify_3c.sh [--slice N] [--sanitizers] [--interop]
#   --slice N       run slices 1..N (default 4)
#   --sanitizers    also run ASan+UBSan and TSan builds
#   --interop       also try MCP Inspector CLI and `claude mcp list` (informational; WARN if tools missing)
# Env: BUILD_DIR (default build), HTTP_BIN, SERVER_ARGS (default "--mode dual --ping-interval-ms 500")
# The example server MUST support: --port N, --mode modern|legacy|dual, --ping-interval-ms N
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
for t in curl jq python3 cmake git; do need "$t"; done

ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT" || exit 2
BUILD="${BUILD_DIR:-build}"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
LOG_DIR="$(mktemp -d)"
PIDS=()
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
[[ -z "$HTTP_BIN" ]] && { bad "http_server binary not found"; exit 1; }
SERVER_ARGS="${SERVER_ARGS:---mode dual --ping-interval-ms 500}"

free_port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
start_server() { # args: port, extra args...
  local port="$1"; shift
  "$HTTP_BIN" --port "$port" "$@" >"$LOG_DIR/server-$port.log" 2>&1 &
  local pid=$!; PIDS+=("$pid")
  for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$port/" && break; kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$pid" 2>/dev/null && return 0 || return 1
}

PORT="$(free_port)"
# shellcheck disable=SC2086
if start_server "$PORT" $SERVER_ARGS; then ok "dual-mode server running on :$PORT"; else bad "server failed to start with: $SERVER_ARGS"; cat "$LOG_DIR/server-$PORT.log"; exit 1; fi
URL="http://127.0.0.1:$PORT/mcp"

LH=(-H "Content-Type: application/json" -H "Accept: application/json, text/event-stream")
LCODE=""
lpost() { # lpost SID PAYLOAD [extra curl args]; omits version header when SID empty (initialize)
  local sid="$1" payload="$2"; shift 2
  local h=("${LH[@]}")
  if [[ -n "$sid" ]]; then h+=(-H "Mcp-Session-Id: $sid" -H "MCP-Protocol-Version: 2025-11-25"); fi
  LCODE="$(curl -sS -m 15 -D "$LOG_DIR/l.hdr" -o "$LOG_DIR/l.body" -w '%{http_code}' "${h[@]}" "$@" -d "$payload" "$URL")"
}
ljson() { if grep -qi '^content-type: *text/event-stream' "$LOG_DIR/l.hdr"; then grep '^data:' "$LOG_DIR/l.body" | tail -n1 | sed 's/^data: *//'; else cat "$LOG_DIR/l.body"; fi; }

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"verify","version":"0"}}}'

echo "== slice 1: legacy session lifecycle and requests (dual mode)"
lpost "" "$INIT"
echo "-- transcript: initialize ($LCODE)"; head -c 500 "$LOG_DIR/l.hdr"; ljson | head -c 500; echo
[[ "$LCODE" == 200 ]] && ok "initialize -> 200" || bad "initialize should be 200 (got $LCODE)"
SID="$(grep -i '^mcp-session-id:' "$LOG_DIR/l.hdr" | awk '{print $2}' | tr -d '\r\n')"
[[ -n "$SID" ]] && ok "Mcp-Session-Id issued (${#SID} chars)" || bad "no Mcp-Session-Id header on initialize response"
(( ${#SID} >= 22 )) && ok "session id long enough to be unguessable" || bad "session id too short (${#SID})"
ljson | jq -e '.result.protocolVersion and .result.serverInfo and .result.capabilities' >/dev/null 2>&1 && ok "initialize result well-formed" || bad "initialize result malformed"
lpost "$SID" '{"jsonrpc":"2.0","method":"notifications/initialized"}'
[[ "$LCODE" == 202 ]] && ok "notifications/initialized -> 202" || bad "initialized notification should be 202 (got $LCODE)"
lpost "$SID" '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
echo "-- transcript: tools/list ($LCODE)"; ljson | head -c 400; echo
ljson | jq -e '.result.tools | length > 0' >/dev/null 2>&1 && ok "legacy tools/list returns tools" || bad "legacy tools/list"
lpost "$SID" '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"add","arguments":{"a":1,"b":2}}}'
ljson | jq -e '.result.content | length > 0' >/dev/null 2>&1 && ok "legacy tools/call add" || bad "legacy tools/call add"
lpost "$SID" '{"jsonrpc":"2.0","id":4,"method":"ping"}'
ljson | jq -e '.result != null and (.error == null)' >/dev/null 2>&1 && ok "legacy ping answered" || bad "legacy ping"
lpost "" '{"jsonrpc":"2.0","id":5,"method":"tools/list","params":{}}' -H "MCP-Protocol-Version: 2025-11-25"
[[ "$LCODE" == 400 ]] && ok "request without session id -> 400" || bad "missing session id should be 400 (got $LCODE)"
lpost "bogus-session-id-0000000000000" '{"jsonrpc":"2.0","id":6,"method":"tools/list","params":{}}'
[[ "$LCODE" == 404 ]] && ok "unknown session id -> 404" || bad "unknown session should be 404 (got $LCODE)"
code="$(curl -s -o /dev/null -w '%{http_code}' -m 10 "${LH[@]}" -H "Mcp-Session-Id: $SID" -H 'MCP-Protocol-Version: 1999-01-01' -d '{"jsonrpc":"2.0","id":7,"method":"tools/list"}' "$URL")"
[[ "$code" == 400 ]] && ok "unsupported MCP-Protocol-Version header -> 400" || bad "unsupported version header should be 400 (got $code)"
# dual mode: a modern request on the SAME endpoint still works
META='"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{"name":"verify","version":"0"},"io.modelcontextprotocol/clientCapabilities":{}}'
r="$(curl -sS -m 10 "${LH[@]}" -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: tools/list' -d "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/list\",\"params\":{$META}}" "$URL")"
echo "$r" | jq -e '.result.tools | length > 0' >/dev/null 2>&1 && ok "dual mode: modern request works on same endpoint" || bad "dual mode: modern request failed (got: ${r:0:200})"
# era isolation: legacy header+body combo that used to trip HeaderMismatch must not be treated as modern
lpost "$SID" '{"jsonrpc":"2.0","id":9,"method":"tools/list","params":{}}'
ljson | jq -e '.error.code != -32020' >/dev/null 2>&1 && ok "legacy request with version header is not misread as modern" || bad "legacy request wrongly rejected with HeaderMismatch"

if (( SLICE >= 2 )); then
  echo "== slice 2: GET stream, server-initiated ping, DELETE, expiry"
  curl -sN -m 4 -D "$LOG_DIR/get.hdr" -o "$LOG_DIR/get.body" -H 'Accept: text/event-stream' -H "Mcp-Session-Id: $SID" -H 'MCP-Protocol-Version: 2025-11-25' "$URL" >/dev/null 2>&1
  head -n1 "$LOG_DIR/get.hdr" | grep -q ' 200' && ok "GET stream opens (200)" || bad "GET stream should be 200 (got: $(head -n1 "$LOG_DIR/get.hdr"))"
  grep -qi '^content-type: *text/event-stream' "$LOG_DIR/get.hdr" && ok "GET stream is text/event-stream" || bad "GET stream content type"
  grep -q '"method" *: *"ping"' "$LOG_DIR/get.body" && ok "server-initiated ping delivered on GET stream" || bad "no server ping on GET stream within 4s (is --ping-interval-ms honored?)"
  code="$(curl -s -o /dev/null -w '%{http_code}' -m 5 -H 'Accept: text/event-stream' "$URL")"
  [[ "$code" == 400 || "$code" == 404 ]] && ok "GET without session -> $code" || bad "GET without session should be 400/404 (got $code)"
  code="$(curl -s -o /dev/null -w '%{http_code}' -m 10 -X DELETE -H "Mcp-Session-Id: $SID" -H 'MCP-Protocol-Version: 2025-11-25' "$URL")"
  [[ "$code" == 200 || "$code" == 204 ]] && ok "DELETE terminates session ($code)" || bad "DELETE should be 200/204 (got $code)"
  lpost "$SID" '{"jsonrpc":"2.0","id":10,"method":"tools/list","params":{}}'
  [[ "$LCODE" == 404 ]] && ok "terminated session -> 404" || bad "terminated session should be 404 (got $LCODE)"
fi

if (( SLICE >= 3 )); then
  echo "== slice 3: resumability tests, mode switches, legacy-only/modern-only behavior"
  lst="$(ctest --test-dir "$BUILD" -N 2>/dev/null)"
  for pat in resum session dual ttl expir; do
    echo "$lst" | grep -qi "$pat" && ok "ctest has a '$pat' test" || bad "no ctest test name matches '$pat'"
  done
  P2="$(free_port)"
  if start_server "$P2" --mode modern; then
    start=$(date +%s)
    code="$(curl -s -o "$LOG_DIR/m.body" -w '%{http_code}' -m 10 "${LH[@]}" -d "$INIT" "http://127.0.0.1:$P2/mcp")"
    el=$(( $(date +%s) - start ))
    [[ "$code" =~ ^4 ]] && (( el < 10 )) && jq -e '.error' "$LOG_DIR/m.body" >/dev/null 2>&1 && ok "modern-only: legacy initialize -> $code with JSON-RPC error" || bad "modern-only: initialize should get a prompt 4xx JSON-RPC error (code=$code)"
  else bad "modern-only server failed to start"; fi
  P3="$(free_port)"
  if start_server "$P3" --mode legacy; then
    lpost_url() { curl -sS -m 10 -o "$LOG_DIR/lo.body" -w '%{http_code}' "${LH[@]}" -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: tools/list' -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{$META}}" "http://127.0.0.1:$P3/mcp"; }
    code="$(lpost_url)"
    [[ "$code" =~ ^4 ]] && ok "legacy-only: modern request rejected ($code)" || bad "legacy-only: modern request should be rejected (got $code)"
  else bad "legacy-only server failed to start"; fi
fi

if (( SLICE >= 4 )); then
  echo "== slice 4: evidence, docs, test volume"
  n="$(ctest --test-dir "$BUILD" -N 2>/dev/null | grep -c 'Test *#')"
  (( n >= 50 )) && ok "ctest discovers $n tests" || bad "only $n tests discovered (need >= 50)"
  check "ctest passes" ctest --test-dir "$BUILD" --output-on-failure
  check "QUICKSTART_HTTP.md documents legacy mode" grep -qiE 'Mcp-Session-Id' docs/QUICKSTART_HTTP.md
  check "3C evidence file" test -s docs/3C_EVIDENCE.md
  check "3B audit file"    test -s docs/3B_AUDIT.md
  check "SPEC_NOTES mentions Last-Event-ID" grep -q 'Last-Event-ID' docs/SPEC_NOTES.md
  check ">= 3 fuzz targets" bash -c "[ \$(git grep -l LLVMFuzzerTestOneInput | wc -l) -ge 3 ]"
fi

if (( INTEROP == 1 )); then
  echo "== interop (informational)"
  if command -v npx >/dev/null 2>&1; then
    out="$(timeout 90 npx -y @modelcontextprotocol/inspector --cli "$URL" --transport http --method tools/list 2>&1)"; rc=$?
    echo "$out" | head -c 800; echo
    (( rc == 0 )) && echo "$out" | grep -q '"tools"' && ok "MCP Inspector CLI: tools/list via dual-mode server" || bad "MCP Inspector CLI failed (rc=$rc)"
  else warn "npx not available; Inspector not run"; fi
  if command -v claude >/dev/null 2>&1; then
    # Every claude call: stdin closed (no interactive prompts), hard timeout.
    CL() { local t="$1"; shift; timeout "$t" claude "$@" </dev/null 2>&1; }
    CL 30 mcp remove cppmcp-verify >/dev/null
    CL 30 mcp add --transport http cppmcp-verify "$URL" >/dev/null
    # `get` health-checks only this server; `list` checks every configured server and can be slow.
    out="$(CL 60 mcp get cppmcp-verify)"; rc=$?
    echo "$out" | head -n 12
    if (( rc == 124 )); then
      warn "claude mcp get timed out after 60s (CLI may be waiting on login/trust; run it manually)"
    elif echo "$out" | grep -qiE 'connected|✓'; then
      ok "claude mcp get shows cppmcp-verify connected"
    else
      bad "claude mcp get does not show a connected server (rc=$rc)"
    fi
    CL 30 mcp remove cppmcp-verify >/dev/null
  else warn "claude CLI not available; Claude Code not run"; fi
fi

if (( SAN == 1 )); then
  echo "== sanitizers"
  for cfg in "address,undefined" "thread"; do
    d="build-san-${cfg//,/-}"; flags="-fsanitize=$cfg -fno-omit-frame-pointer -g"
    if cmake -S . -B "$d" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=$cfg" >"$LOG_DIR/$d.cfg" 2>&1 \
       && cmake --build "$d" -j"$JOBS" >"$LOG_DIR/$d.build" 2>&1; then
      ctest --test-dir "$d" --output-on-failure >"$LOG_DIR/$d.test" 2>&1; rc=$?
      if (( rc == 0 )) && ! grep -qE 'ERROR: (Address|Leak)Sanitizer|WARNING: ThreadSanitizer|runtime error:' "$LOG_DIR/$d.test"; then
        ok "sanitizer [$cfg]: tests pass, zero findings"
      else bad "sanitizer [$cfg]: failures or findings (see $LOG_DIR/$d.test)"; fi
    else bad "sanitizer [$cfg]: build failed (see $LOG_DIR/$d.build)"; fi
  done
fi

echo
echo "RESULT: $PASS passed, $FAIL failed, $WARN warnings (logs: $LOG_DIR)"
(( FAIL == 0 ))