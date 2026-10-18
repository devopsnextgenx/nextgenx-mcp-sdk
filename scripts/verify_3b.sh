#!/usr/bin/env bash
# scripts/verify_3b.sh : executable acceptance gate for Phase 3B (modern HTTP server).
# Usage: scripts/verify_3b.sh [--slice N] [--sanitizers]
#   --slice N       run checks for slices 1..N (default 5 = everything)
#   --sanitizers    also build and run ASan+UBSan and TSan configurations (slow)
# Env: BUILD_DIR (default build), HTTP_BIN, STDIO_BIN, SERVER_ARGS (extra args for http_server)
# Exit status 0 only if every check passes.
set -u

SLICE=5; SAN=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --slice) SLICE="$2"; shift 2 ;;
    --sanitizers) SAN=1; shift ;;
    *) echo "unknown argument: $1"; exit 2 ;;
  esac
done

PASS=0; FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
check() { local d="$1"; shift; if "$@" >/dev/null 2>&1; then ok "$d"; else bad "$d"; fi; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing required tool: $1"; exit 2; }; }
for t in curl jq python3 cmake git; do need "$t"; done

ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT" || exit 2
BUILD="${BUILD_DIR:-build}"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
LOG_DIR="$(mktemp -d)"
SERVER_PID=""
cleanup() { [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "== build ($BUILD)"
if cmake -S . -B "$BUILD" >"$LOG_DIR/cfg.log" 2>&1 && cmake --build "$BUILD" -j"$JOBS" >"$LOG_DIR/build.log" 2>&1; then
  ok "configure and build"
else
  bad "configure and build (see $LOG_DIR/build.log)"; tail -20 "$LOG_DIR/build.log"; exit 1
fi

find_bin() { find "$BUILD" -type f -perm -u+x \( -name "$1" -o -name "cppmcp_$1" \) 2>/dev/null | head -n1; }
HTTP_BIN="${HTTP_BIN:-$(find_bin http_server)}"
STDIO_BIN="${STDIO_BIN:-$(find_bin stdio_server)}"

META='"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{"name":"verify","version":"0"},"io.modelcontextprotocol/clientCapabilities":{}}'
body() { printf '{"jsonrpc":"2.0","id":1,"method":"%s","params":{%s%s}}' "$1" "$2" "$META"; }

echo "== stdio smoke (regression guard)"
if [[ -n "$STDIO_BIN" ]]; then
  out="$(body tools/list '' | timeout 10 "$STDIO_BIN" 2>/dev/null | head -n1)"
  if echo "$out" | jq -e '.result.tools | length > 0' >/dev/null 2>&1; then ok "stdio tools/list returns tools"; else bad "stdio tools/list (got: ${out:0:200})"; fi
else
  bad "stdio_server binary not found"
fi

echo "== slice 1: real listener, Asio + llhttp, modern request flow"
check "llhttp pinned in CMake" git grep -qiE 'llhttp' -- '*.txt' '*.cmake'
check "asio pinned in CMake"   git grep -qiE 'asio'   -- '*.txt' '*.cmake'
if [[ -z "$HTTP_BIN" ]]; then
  bad "http_server example binary not found under $BUILD (expected http_server or cppmcp_http_server)"
  echo; echo "RESULT: $PASS passed, $FAIL failed"; exit 1
fi
ok "http_server binary found: $HTTP_BIN"

PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')"
# shellcheck disable=SC2086
"$HTTP_BIN" --port "$PORT" ${SERVER_ARGS:-} >"$LOG_DIR/server.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
  curl -s -o /dev/null "http://127.0.0.1:$PORT/" && break
  kill -0 "$SERVER_PID" 2>/dev/null || break
  sleep 0.1
done
if kill -0 "$SERVER_PID" 2>/dev/null; then ok "server running on 127.0.0.1:$PORT"; else bad "server exited early"; cat "$LOG_DIR/server.log"; exit 1; fi

URL="http://127.0.0.1:$PORT/mcp"
H=(-H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" -H "MCP-Protocol-Version: 2026-07-28")
post() { local method="$1" name="$2" payload="$3"; shift 3
  local extra=(-H "Mcp-Method: $method"); [[ -n "$name" ]] && extra+=(-H "Mcp-Name: $name")
  curl -sS -m 15 "${H[@]}" "${extra[@]}" "$@" -d "$payload" "$URL"; }

echo "-- transcript: server/discover"
r="$(post server/discover '' "$(body server/discover '')")"; echo "$r" | head -c 600; echo
echo "$r" | jq -e '.result' >/dev/null 2>&1 && ok "server/discover returns a result" || bad "server/discover"

echo "-- transcript: tools/list"
r="$(post tools/list '' "$(body tools/list '')")"; echo "$r" | head -c 600; echo
echo "$r" | jq -e '.result.tools | length > 0' >/dev/null 2>&1 && ok "tools/list returns tools" || bad "tools/list"

echo "-- transcript: tools/call add"
r="$(post tools/call add "$(body tools/call '"name":"add","arguments":{"a":1,"b":2},')" )"; echo "$r" | head -c 600; echo
echo "$r" | jq -e '.result.content | length > 0' >/dev/null 2>&1 && ok "tools/call add returns content" || bad "tools/call add"

if (( SLICE >= 2 )); then
  echo "== slice 2: validation and hardening"
  r="$(post tools/list '' "$(body tools/call '"name":"add","arguments":{"a":1,"b":2},')")"
  echo "$r" | jq -e '.error.code == -32020' >/dev/null 2>&1 && ok "Mcp-Method/body mismatch -> -32020" || bad "header mismatch should return -32020 (got: ${r:0:200})"
  r="$(post tools/call wrong_name "$(body tools/call '"name":"add","arguments":{"a":1,"b":2},')")"
  echo "$r" | jq -e '.error.code == -32020' >/dev/null 2>&1 && ok "Mcp-Name/body mismatch -> -32020" || bad "Mcp-Name mismatch should return -32020"
  code="$(curl -s -o /dev/null -w '%{http_code}' -m 10 "$URL")"; [[ "$code" == 405 ]] && ok "GET -> 405" || bad "GET should be 405 (got $code)"
  code="$(curl -s -o /dev/null -w '%{http_code}' -m 10 -X DELETE "$URL")"; [[ "$code" == 405 ]] && ok "DELETE -> 405" || bad "DELETE should be 405 (got $code)"
  code="$(curl -s -o /dev/null -w '%{http_code}' -m 10 -H 'Content-Type: text/plain' -H 'Mcp-Method: tools/list' -H 'MCP-Protocol-Version: 2026-07-28' -d 'x' "$URL")"; [[ "$code" == 415 ]] && ok "bad Content-Type -> 415" || bad "bad Content-Type should be 415 (got $code)"
  code="$(curl -s -o /dev/null -w '%{http_code}' -m 10 -H 'Host: evil.example' "${H[@]}" -H 'Mcp-Method: tools/list' -d "$(body tools/list '')" "$URL")"; [[ "$code" == 403 ]] && ok "foreign Host -> 403" || bad "foreign Host should be 403 (got $code)"
  code="$(curl -s -o /dev/null -w '%{http_code}' -m 10 -H 'Origin: https://evil.example' "${H[@]}" -H 'Mcp-Method: tools/list' -d "$(body tools/list '')" "$URL")"; [[ "$code" == 403 ]] && ok "foreign Origin -> 403" || bad "foreign Origin should be 403 (got $code)"
  r="$(post initialize '' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"x","version":"0"}}}' )"
  [[ -n "$r" ]] && ok "legacy initialize in modern-only mode gets a prompt, defined response" || bad "legacy initialize hung or returned nothing"

  raw() { python3 - "$PORT" "$1" <<'PY'
import socket, sys
port = int(sys.argv[1]); data = sys.argv[2].encode('latin1').decode('unicode_escape').encode('latin1')
s = socket.create_connection(("127.0.0.1", port), timeout=5)
s.sendall(data)
try: r = s.recv(300)
except Exception: r = b""
print(r.split(b"\r\n")[0].decode('latin1'))
PY
  }
  r="$(raw "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:$PORT\r\nContent-Type: application/json\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n")"
  [[ "$r" == *" 400"* ]] && ok "CL+TE smuggling attempt -> 400" || bad "CL+TE should be 400 (got: $r)"
  r="$(raw "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:$PORT\r\nContent-Type: application/json\r\nContent-Length: 99999999\r\n\r\n")"
  [[ "$r" == *" 413"* ]] && ok "oversize Content-Length -> 413" || bad "oversize body should be 413 (got: $r)"
  r="$(raw "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:$PORT\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nxx")"
  [[ "$r" == *" 400"* ]] && ok "duplicate Content-Length -> 400" || bad "duplicate CL should be 400 (got: $r)"
  kill -0 "$SERVER_PID" 2>/dev/null && ok "server survived malformed input" || bad "server died on malformed input"
fi

if (( SLICE >= 3 )); then
  echo "== slice 3: SSE streaming and disconnect cancellation"
  echo "-- transcript: streaming call (emit_demo)"
  curl -sN -m 15 -D "$LOG_DIR/sse.hdr" "${H[@]}" -H 'Mcp-Method: tools/call' -H 'Mcp-Name: emit_demo' \
    -d "$(body tools/call '"name":"emit_demo","arguments":{"count":3},')" "$URL" >"$LOG_DIR/sse.body"
  head -c 900 "$LOG_DIR/sse.hdr"; head -c 900 "$LOG_DIR/sse.body"; echo
  grep -qi '^content-type: *text/event-stream' "$LOG_DIR/sse.hdr" && ok "SSE content type" || bad "SSE content type (is emit_demo registered and auto-switching to SSE?)"
  (( $(grep -c '^data:' "$LOG_DIR/sse.body") >= 2 )) && ok "SSE stream carries >= 2 events" || bad "SSE stream should carry >= 2 data events"
  tail -n 5 "$LOG_DIR/sse.body" | grep '^data:' | tail -n1 | sed 's/^data: *//' | jq -e '.result or .error' >/dev/null 2>&1 && ok "stream ends with the final response" || bad "final event is not a JSON-RPC response"
  # disconnect: abort a slow call, server must stay healthy
  curl -sN -m 1 "${H[@]}" -H 'Mcp-Method: tools/call' -H 'Mcp-Name: slow_count' \
    -d "$(body tools/call '"name":"slow_count","arguments":{"count":1000,"delay_ms":50},')" "$URL" >/dev/null 2>&1
  sleep 0.5
  r="$(post tools/list '' "$(body tools/list '')")"
  echo "$r" | jq -e '.result.tools' >/dev/null 2>&1 && ok "server healthy after client disconnect" || bad "server unhealthy after disconnect"
  ctest --test-dir "$BUILD" -N 2>/dev/null | grep -qiE 'disconnect|cancel' && ok "ctest lists a disconnect/cancellation test" || bad "no disconnect/cancellation test in ctest list"
fi

if (( SLICE >= 4 )); then
  echo "== slice 4: x-mcp-header"
  r="$(post tools/list '' "$(body tools/list '')")"
  echo "$r" | jq -e 'any(.result.tools[]; (.inputSchema | tostring | contains("x-mcp-header")))' >/dev/null 2>&1 \
    && ok "a tool advertises x-mcp-header in inputSchema" || bad "no tool advertises x-mcp-header"
  ctest --test-dir "$BUILD" -N 2>/dev/null | grep -qiE 'mcp[-_]?param|x[-_]mcp[-_]header' && ok "ctest lists x-mcp-header tests" || bad "no x-mcp-header tests in ctest list"
fi

if (( SLICE >= 5 )); then
  echo "== slice 5: evidence, tests, docs"
  n="$(ctest --test-dir "$BUILD" -N 2>/dev/null | grep -c 'Test *#')"
  (( n >= 30 )) && ok "ctest discovers $n individual tests" || bad "ctest discovers only $n tests (need per-test discovery, >= 30)"
  for pat in smuggl slowloris sse stateless header disconnect; do
    ctest --test-dir "$BUILD" -N 2>/dev/null | grep -qi "$pat" && ok "test name matches '$pat'" || bad "no test name matches '$pat'"
  done
  check "ctest passes"           ctest --test-dir "$BUILD" --output-on-failure
  check "QUICKSTART_HTTP.md"     test -s docs/QUICKSTART_HTTP.md
  check "DECISIONS.md"           test -s docs/DECISIONS.md
  check "SPEC_NOTES.md"          test -s docs/SPEC_NOTES.md
  check "3B evidence file"       test -s docs/3B_EVIDENCE.md
  check ">= 2 fuzz targets"      bash -c "[ \$(git grep -l LLVMFuzzerTestOneInput | wc -l) -ge 2 ]"
  check "CI workflow present"    test -s .github/workflows/ci.yml
fi

if (( SAN == 1 )); then
  echo "== sanitizers"
  for cfg in "address,undefined" "thread"; do
    d="build-san-${cfg//,/-}"
    flags="-fsanitize=$cfg -fno-omit-frame-pointer -g"
    if cmake -S . -B "$d" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=$cfg" >"$LOG_DIR/$d.cfg" 2>&1 \
       && cmake --build "$d" -j"$JOBS" >"$LOG_DIR/$d.build" 2>&1; then
      ctest --test-dir "$d" --output-on-failure >"$LOG_DIR/$d.test" 2>&1; rc=$?
      if (( rc == 0 )) && ! grep -qE 'ERROR: (Address|Leak)Sanitizer|WARNING: ThreadSanitizer|runtime error:' "$LOG_DIR/$d.test"; then
        ok "sanitizer [$cfg]: tests pass, zero findings"
      else
        bad "sanitizer [$cfg]: failures or findings (see $LOG_DIR/$d.test)"
      fi
    else
      bad "sanitizer [$cfg]: build failed (see $LOG_DIR/$d.build)"
    fi
  done
fi

echo
echo "RESULT: $PASS passed, $FAIL failed (logs: $LOG_DIR)"
(( FAIL == 0 ))