#!/usr/bin/env bash
# scripts/verify_4.sh : executable acceptance gate for Phase 4.
# Scope: JSON Schema validation, structured output, progress, logging, multi round-trip requests (elicitation).
# Usage: scripts/verify_4.sh [--slice N] [--sanitizers] [--interop]
# Env:   BUILD_DIR (default build), HTTP_BIN, CLIENT_BIN, STDIO_BIN
#
# Example-server contract (http_server and stdio_server register the SAME tools):
#   add             {a:number,b:number} required both            -> text "3" for a=1,b=2
#   structured_demo {city:string} required; outputSchema {city:string,temp_c:number,conditions:string}
#                   returns structuredContent AND the same JSON serialized in content[0].text
#   bad_output      {} ; outputSchema requires {ok:boolean}; handler returns {"ok":"nope"} (must never reach the client)
#   progress_demo   {steps:integer 1..20, delay_ms?:integer}     -> `steps` progress notifications when a progressToken is given
#   log_demo        {}                                           -> logs at debug, info, warning, error
#   delete_project  {project:string}  asks (elicitation, form mode) for {confirm:boolean};
#                   accepted+confirm=true -> text "deleted <project>"; otherwise text "not deleted"
#   loop_input      {}                                           -> always asks again (never completes)
# http_server flags : --port N --mode modern|legacy|dual --request-state-key <64 hex> --validate-output on|off
# http_client flags (new) : --call NAME --args JSON --progress --log-level L --elicit accept|decline|cancel --elicit-content JSON
# http_client stdout (new): RESULT <json> | PROGRESS <p>/<t> | LOG <level> <text> | ELICIT <message>
# Fixture authored by the agent from the spec: scripts/fixtures/mrtr_accept_response.json
#   = the exact `inputResponses` object the client must send to accept the delete_project elicitation,
#     using the placeholder key __KEY__ (the script substitutes the real key from inputRequests).
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
STDIO_BIN="${STDIO_BIN:-$(find_bin stdio_server)}"
[[ -z "$HTTP_BIN" ]]   && { bad "http_server binary not found"; exit 1; }
[[ -z "$CLIENT_BIN" ]] && { bad "http_client binary not found"; exit 1; }

free_port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
start_server() { # start_server PORT args... ; sets LAST_PID
  local port="$1"; shift
  "$HTTP_BIN" --port "$port" "$@" >"$LOG_DIR/server-$port.log" 2>&1 &
  LAST_PID=$!; PIDS+=("$LAST_PID")
  for _ in $(seq 1 60); do
    curl -s -m 2 -o /dev/null "http://127.0.0.1:$port/" && break
    kill -0 "$LAST_PID" 2>/dev/null || break
    sleep 0.1
  done
  kill -0 "$LAST_PID" 2>/dev/null
}
run_client() { timeout "${CLIENT_TIMEOUT:-40}" "$CLIENT_BIN" "$@" </dev/null 2>&1; }

KEY_A="00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
KEY_B="ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100"
PORT="$(free_port)"
start_server "$PORT" --mode dual --request-state-key "$KEY_A" || { bad "dual server failed to start"; cat "$LOG_DIR/server-$PORT.log"; exit 1; }
URL="http://127.0.0.1:$PORT/mcp"
ok "dual-mode server running on :$PORT"

# ---------- modern helpers ----------
MH=(-H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" -H "MCP-Protocol-Version: 2026-07-28")
CAPS='{}'
mbody() { # mbody METHOD PARAMS_JSON [EXTRA_META_JSON]
  jq -nc --arg m "$1" --argjson p "$2" --argjson x "${3:-{\}}" --argjson caps "$CAPS" \
    '{jsonrpc:"2.0",id:1,method:$m,params:($p + {_meta:({"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{name:"verify",version:"0"},"io.modelcontextprotocol/clientCapabilities":$caps} + $x)})}'
}
msend() { # msend URL METHOD NAME BODY
  local url="$1" method="$2" name="$3" body="$4"; local extra=(-H "Mcp-Method: $method")
  [[ -n "$name" ]] && extra+=(-H "Mcp-Name: $name")
  curl -sS -m 25 -D "$LOG_DIR/m.hdr" -o "$LOG_DIR/m.body" "${MH[@]}" "${extra[@]}" -d "$body" "$url" >/dev/null 2>&1
}
mevents() { if grep -qi '^content-type: *text/event-stream' "$LOG_DIR/m.hdr"; then grep '^data:' "$LOG_DIR/m.body" | sed 's/^data: *//'; else cat "$LOG_DIR/m.body"; fi; }
mfinal()  { mevents | jq -c 'select(has("result") or has("error"))' 2>/dev/null | tail -n1; }
mcall() { # mcall TOOL ARGS_JSON [EXTRA_META_JSON] [URL]
  msend "${4:-$URL}" tools/call "$1" "$(mbody tools/call "$(jq -nc --arg n "$1" --argjson a "$2" '{name:$n,arguments:$a}')" "${3:-{\}}")"
}

# ---------- legacy helpers ----------
LH=(-H "Content-Type: application/json" -H "Accept: application/json, text/event-stream")
SID=""
l_init() {
  curl -sS -m 15 -D "$LOG_DIR/li.hdr" -o "$LOG_DIR/li.body" "${LH[@]}" \
    -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{"elicitation":{"form":{}}},"clientInfo":{"name":"verify","version":"0"}}}' "$URL" >/dev/null 2>&1
  SID="$(grep -i '^mcp-session-id:' "$LOG_DIR/li.hdr" | awk '{print $2}' | tr -d '\r\n')"
  curl -sS -m 10 -o /dev/null "${LH[@]}" -H "Mcp-Session-Id: $SID" -H 'MCP-Protocol-Version: 2025-11-25' -d '{"jsonrpc":"2.0","method":"notifications/initialized"}' "$URL" >/dev/null 2>&1
}
l_send() { curl -sS -m 25 -D "$LOG_DIR/l.hdr" -o "$LOG_DIR/l.body" "${LH[@]}" -H "Mcp-Session-Id: $SID" -H 'MCP-Protocol-Version: 2025-11-25' -d "$1" "$URL" >/dev/null 2>&1; }
l_events() { if grep -qi '^content-type: *text/event-stream' "$LOG_DIR/l.hdr"; then grep '^data:' "$LOG_DIR/l.body" | sed 's/^data: *//'; else cat "$LOG_DIR/l.body"; fi; }
l_final() { l_events | jq -c 'select(has("result") or has("error"))' 2>/dev/null | tail -n1; }
l_call() { l_send "$(jq -nc --arg n "$1" --argjson a "$2" --argjson x "${3:-{\}}" '{jsonrpc:"2.0",id:2,method:"tools/call",params:{name:$n,arguments:$a,_meta:$x}}')"; }
l_init
[[ -n "$SID" ]] && ok "legacy session established" || bad "legacy initialize failed (no session id)"

echo "== slice 1: JSON Schema validation and structured output"
msend "$URL" tools/list "" "$(mbody tools/list '{}')"; r="$(mfinal)"
echo "$r" | jq -e '.result.tools[] | select(.name=="add") | .inputSchema | ((.properties.a.type=="number") or (.properties.a.type=="integer")) and (.required|index("a")!=null) and (.required|index("b")!=null)' >/dev/null 2>&1 \
  && ok "add advertises a real inputSchema (properties + required)" || bad "add inputSchema lacks properties/required"
echo "$r" | jq -e '.result.tools[] | select(.name=="structured_demo") | .outputSchema.type=="object"' >/dev/null 2>&1 && ok "structured_demo advertises outputSchema" || bad "structured_demo outputSchema missing"
mcall add '{"a":1,"b":2}'; mfinal | jq -e '.result.isError != true and (.result.content|length>0)' >/dev/null 2>&1 && ok "valid add succeeds (modern)" || bad "valid add failed (modern)"
mcall add '{"a":"x","b":2}'; r="$(mfinal)"; echo "-- invalid args (modern): ${r:0:300}"
echo "$r" | jq -e '.result.isError == true and (.result.content[0].text|length>0)' >/dev/null 2>&1 && ok "wrong-typed argument -> tool execution error (isError) with message (modern)" || bad "invalid argument should yield result.isError=true with a message (modern)"
mcall add '{"b":2}'; mfinal | jq -e '.result.isError == true' >/dev/null 2>&1 && ok "missing required argument -> isError (modern)" || bad "missing required argument should be isError (modern)"
l_call add '{"a":"x","b":2}'; l_final | jq -e '.result.isError == true' >/dev/null 2>&1 && ok "wrong-typed argument -> isError (legacy)" || bad "invalid argument should yield isError (legacy)"
mcall no_such_tool '{}'; mfinal | jq -e '.error.code == -32602' >/dev/null 2>&1 && ok "unknown tool -> -32602 protocol error" || bad "unknown tool should be -32602"
mcall structured_demo '{"city":"Paris"}'; r="$(mfinal)"; echo "-- structured_demo: ${r:0:300}"
echo "$r" | jq -e '.result.structuredContent | (.city=="Paris") and (.temp_c|type=="number") and (.conditions|type=="string")' >/dev/null 2>&1 && ok "structuredContent present and conforms" || bad "structuredContent missing or wrong"
echo "$r" | jq -e '(.result.content[0].text | fromjson) == .result.structuredContent' >/dev/null 2>&1 && ok "structuredContent mirrored as serialized JSON in content[0].text" || bad "content[0].text must mirror structuredContent"
mcall bad_output '{}'; r="$(mfinal)"; echo "-- bad_output: ${r:0:300}"
echo "$r" | jq -e '((.error.code == -32603) or (.result.isError == true)) and (.result.structuredContent == null)' >/dev/null 2>&1 && ok "non-conforming handler output is not forwarded" || bad "server forwarded structuredContent that violates outputSchema"
python3 - >"$LOG_DIR/bomb.json" <<'PY'
import json
d = "x"
for _ in range(5000): d = {"a": d}
print(json.dumps({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"add","arguments":d}}))
PY
code="$(curl -s -o /dev/null -w '%{http_code}' -m 20 "${MH[@]}" -H 'Mcp-Method: tools/call' -H 'Mcp-Name: add' --data-binary @"$LOG_DIR/bomb.json" "$URL")"
[[ "$code" =~ ^(200|400|413)$ ]] && ok "deeply nested arguments handled ($code)" || bad "deeply nested arguments: unexpected status $code"
kill -0 "$LAST_PID" 2>/dev/null; curl -s -m 5 -o /dev/null "$URL"; mcall add '{"a":1,"b":2}'; mfinal | jq -e '.result' >/dev/null 2>&1 && ok "server alive after schema bomb" || bad "server unhealthy after schema bomb"
if [[ -n "$STDIO_BIN" ]]; then
  out="$( { jq -nc --argjson p "$(jq -nc '{name:"add",arguments:{a:"x",b:2}}')" '{jsonrpc:"2.0",id:1,method:"tools/call",params:($p + {_meta:{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{name:"v",version:"0"},"io.modelcontextprotocol/clientCapabilities":{}}})}'; } | timeout 10 "$STDIO_BIN" 2>/dev/null | head -n1)"
  echo "$out" | jq -e '.result.isError == true' >/dev/null 2>&1 && ok "stdio: invalid argument -> isError" || bad "stdio: invalid argument should be isError"
fi

if (( SLICE >= 2 )); then
  echo "== slice 2: progress and logging"
  mcall progress_demo '{"steps":4,"delay_ms":50}' '{"progressToken":"p1"}'
  echo "-- progress stream:"; mevents | head -n 6 | cut -c1-200
  grep -qi '^content-type: *text/event-stream' "$LOG_DIR/m.hdr" && ok "progress call streams over SSE" || bad "progress call should switch to SSE"
  n="$(mevents | jq -c 'select(.method=="notifications/progress" and .params.progressToken=="p1")' | wc -l)"
  (( n >= 4 )) && ok "received $n progress notifications for token p1" || bad "expected >= 4 progress notifications (got $n)"
  mevents | jq -s -e '[ .[] | select(.method=="notifications/progress") | .params.progress ] | . == (sort) and (unique|length) == length' >/dev/null 2>&1 && ok "progress values strictly increase" || bad "progress must strictly increase"
  mevents | jq -s -e '.[-1] | has("result")' >/dev/null 2>&1 && ok "final message is the result" || bad "stream must end with the final response"
  mcall progress_demo '{"steps":4,"delay_ms":10}'
  [[ "$(mevents | jq -c 'select(.method=="notifications/progress")' | wc -l)" == 0 ]] && ok "no progressToken -> no progress notifications" || bad "progress sent without a progressToken"
  mcall log_demo '{}' '{"io.modelcontextprotocol/logLevel":"warning"}'
  lv="$(mevents | jq -r 'select(.method=="notifications/message") | .params.level' | sort -u | tr '\n' ' ')"
  echo "-- log levels received at logLevel=warning: $lv"
  [[ "$lv" == *warning* && "$lv" == *error* && "$lv" != *debug* && "$lv" != *info* ]] && ok "per-request logLevel filters messages" || bad "logLevel=warning should deliver only warning+ (got: $lv)"
  mcall log_demo '{}'
  [[ "$(mevents | jq -c 'select(.method=="notifications/message")' | wc -l)" == 0 ]] && ok "no logLevel -> no log notifications" || bad "log notifications sent without logLevel"
  l_send '{"jsonrpc":"2.0","id":20,"method":"logging/setLevel","params":{"level":"error"}}'
  l_final | jq -e '.result != null' >/dev/null 2>&1 && ok "legacy logging/setLevel accepted" || bad "legacy logging/setLevel failed"
  l_call log_demo '{}'
  lv="$(l_events | jq -r 'select(.method=="notifications/message") | .params.level' | sort -u | tr '\n' ' ')"
  [[ "$lv" == *error* && "$lv" != *warning* && "$lv" != *info* ]] && ok "legacy setLevel filters messages" || bad "legacy logging filter wrong (got: $lv)"
  l_call progress_demo '{"steps":3,"delay_ms":20}' '{"progressToken":7}'
  (( $(l_events | jq -c 'select(.method=="notifications/progress" and .params.progressToken==7)' | wc -l) >= 3 )) && ok "legacy progress with integer token" || bad "legacy progress failed"
  out="$(run_client --url "$URL" --mode modern --call progress_demo --args '{"steps":3,"delay_ms":20}' --progress)"; echo "$out" | head -n 5
  (( $(echo "$out" | grep -c '^PROGRESS ') >= 3 )) && ok "client surfaces progress (modern)" || bad "client printed < 3 PROGRESS lines (modern)"
  out="$(run_client --url "$URL" --mode legacy --call progress_demo --args '{"steps":3,"delay_ms":20}' --progress)"
  (( $(echo "$out" | grep -c '^PROGRESS ') >= 3 )) && ok "client surfaces progress (legacy)" || bad "client printed < 3 PROGRESS lines (legacy)"
  out="$(run_client --url "$URL" --mode modern --call log_demo --args '{}' --log-level warning)"
  echo "$out" | grep -q '^LOG warning' && ! echo "$out" | grep -q '^LOG debug' && ok "client surfaces filtered logs" || bad "client log output wrong"
  if [[ -n "$STDIO_BIN" ]]; then
    n="$( { jq -nc '{jsonrpc:"2.0",id:1,method:"tools/call",params:{name:"progress_demo",arguments:{steps:3,delay_ms:10},_meta:{progressToken:"s1","io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{name:"v",version:"0"},"io.modelcontextprotocol/clientCapabilities":{}}}}'; sleep 1; } | timeout 10 "$STDIO_BIN" 2>/dev/null | jq -c 'select(.method=="notifications/progress")' | wc -l)"
    (( n >= 3 )) && ok "stdio progress notifications ($n)" || bad "stdio progress notifications missing ($n)"
  fi
fi

if (( SLICE >= 3 )); then
  echo "== slice 3: multi round-trip requests / elicitation"
  CAPS='{"elicitation":{"form":{}}}'
  FIX="scripts/fixtures/mrtr_accept_response.json"
  mcall delete_project '{"project":"alpha"}'; r="$(mfinal)"; echo "-- first round: ${r:0:500}"
  echo "$r" | jq -e '.result.resultType=="input_required" and (.result.inputRequests|length==1) and (.result.requestState|type=="string" and length>0)' >/dev/null 2>&1 \
    && ok "first round returns input_required with inputRequests and requestState" || bad "first round must be an input_required result"
  KEY="$(echo "$r" | jq -r '.result.inputRequests|keys[0]')"; STATE="$(echo "$r" | jq -r '.result.requestState')"
  echo "$r" | jq -e --arg k "$KEY" '.result.inputRequests[$k].method=="elicitation/create"' >/dev/null 2>&1 && ok "input request is elicitation/create" || bad "input request is not elicitation/create"
  retry() { # retry TOOL ARGS STATE INPUT_RESPONSES_JSON [URL]
    local body; body="$(jq -nc --arg n "$1" --argjson a "$2" --arg s "$3" --argjson ir "$4" --argjson caps "$CAPS" \
      '{jsonrpc:"2.0",id:2,method:"tools/call",params:{name:$n,arguments:$a,inputResponses:$ir,requestState:$s,_meta:{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{name:"verify",version:"0"},"io.modelcontextprotocol/clientCapabilities":$caps}}}')"
    msend "${5:-$URL}" tools/call "$1" "$body"
  }
  if [[ -s "$FIX" ]]; then
    ACCEPT="$(sed "s/__KEY__/$KEY/g" "$FIX")"
    retry delete_project '{"project":"alpha"}' "$STATE" "$ACCEPT"; r2="$(mfinal)"; echo "-- accepted retry: ${r2:0:300}"
    echo "$r2" | jq -e '(.result.resultType=="complete" or .result.resultType==null) and (.result.content[0].text|test("deleted alpha")) and (.result.content[0].text|test("not deleted")|not)' >/dev/null 2>&1 \
      && ok "accepted retry completes: deleted alpha" || bad "accepted retry should complete with 'deleted alpha'"
    TAMPERED="${STATE%?}X"; [[ "$TAMPERED" == "$STATE" ]] && TAMPERED="${STATE%?}Y"
    retry delete_project '{"project":"alpha"}' "$TAMPERED" "$ACCEPT"
    mfinal | jq -e '(.error != null) or (.result.isError == true)' >/dev/null 2>&1 && ok "tampered requestState rejected" || bad "tampered requestState was accepted"
    retry delete_project '{"project":"beta"}' "$STATE" "$ACCEPT"
    mfinal | jq -e '((.error != null) or (.result.isError == true)) and ((.result.content[0].text // "")|test("deleted beta")|not)' >/dev/null 2>&1 && ok "requestState bound to the original arguments (beta replay rejected)" || bad "requestState replayed against different arguments was accepted"
    retry delete_project '{"project":"alpha"}' "" "$ACCEPT"
    mfinal | jq -e '((.result.content[0].text // "")|test("deleted alpha")|not)' >/dev/null 2>&1 && ok "missing requestState does not skip the confirmation" || bad "missing requestState bypassed the confirmation"
    DECL="$(echo "$ACCEPT" | jq -c --arg k "$KEY" '.[$k] |= (.action="decline" | del(.content))')"
    retry delete_project '{"project":"alpha"}' "$STATE" "$DECL"; r3="$(mfinal)"
    echo "$r3" | jq -e '(.result.content[0].text|test("not deleted"))' >/dev/null 2>&1 && ok "decline -> 'not deleted'" || bad "decline should complete with 'not deleted'"
    # statelessness: same key on a second instance completes; a different key rejects
    PB="$(free_port)"; PC="$(free_port)"
    start_server "$PB" --mode dual --request-state-key "$KEY_A" && start_server "$PC" --mode dual --request-state-key "$KEY_B"
    retry delete_project '{"project":"alpha"}' "$STATE" "$ACCEPT" "http://127.0.0.1:$PB/mcp"
    mfinal | jq -e '(.result.content[0].text // "")|test("deleted alpha")' >/dev/null 2>&1 && ok "second instance with shared key completes the continuation" || bad "shared-key instance could not verify requestState"
    retry delete_project '{"project":"alpha"}' "$STATE" "$ACCEPT" "http://127.0.0.1:$PC/mcp"
    mfinal | jq -e '((.error != null) or (.result.isError == true))' >/dev/null 2>&1 && ok "instance with a different key rejects the continuation" || bad "different-key instance accepted foreign requestState"
  else
    bad "missing $FIX (spec-verified inputResponses template with __KEY__)"
  fi
  CAPS='{}'
  mcall delete_project '{"project":"alpha"}'; r="$(mfinal)"
  echo "$r" | jq -e '.error.code == -32021' >/dev/null 2>&1 && ok "no elicitation capability -> MissingRequiredClientCapability (-32021)" || bad "expected -32021 when client lacks elicitation (got: ${r:0:200})"
  for mode in modern legacy; do
    out="$(run_client --url "$URL" --mode "$mode" --call delete_project --args '{"project":"gamma"}' --elicit accept --elicit-content '{"confirm":true}')"; echo "-- client $mode accept:"; echo "$out" | head -n 4
    echo "$out" | grep -q '^ELICIT ' && echo "$out" | grep -q 'deleted gamma' && ok "client drives elicitation ($mode, accept)" || bad "client elicitation accept failed ($mode)"
    out="$(run_client --url "$URL" --mode "$mode" --call delete_project --args '{"project":"gamma"}' --elicit decline)"
    echo "$out" | grep -q 'not deleted' && ok "client decline ($mode)" || bad "client decline failed ($mode)"
    out="$(run_client --url "$URL" --mode "$mode" --call delete_project --args '{"project":"gamma"}' --elicit cancel)"
    echo "$out" | grep -q 'deleted gamma' && ! echo "$out" | grep -q 'not deleted' && bad "cancel must not delete ($mode)" || ok "client cancel does not delete ($mode)"
  done
  out="$(run_client --url "$URL" --mode modern --call loop_input --args '{}' --elicit accept --elicit-content '{"confirm":true}')"; rc=$?
  (( rc != 0 )) && echo "$out" | grep -qiE 'round' && ok "client stops after max rounds with a clear error" || bad "loop_input should fail after max rounds (rc=$rc)"
  if [[ -n "$STDIO_BIN" ]]; then
    out="$( { jq -nc '{jsonrpc:"2.0",id:1,method:"tools/call",params:{name:"delete_project",arguments:{project:"alpha"},_meta:{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{name:"v",version:"0"},"io.modelcontextprotocol/clientCapabilities":{elicitation:{form:{}}}}}}'; } | timeout 10 "$STDIO_BIN" 2>/dev/null | head -n1)"
    echo "$out" | jq -e '.result.resultType=="input_required"' >/dev/null 2>&1 && ok "stdio returns input_required" || bad "stdio input_required missing"
  fi
fi

if (( SLICE >= 4 )); then
  echo "== slice 4: tests, docs, evidence"
  lst="$(ctest --test-dir "$BUILD" -N 2>/dev/null)"
  n="$(echo "$lst" | grep -c 'Test *#')"
  (( n >= 120 )) && ok "ctest discovers $n tests" || bad "only $n tests discovered (need >= 120)"
  for pat in json_schema_suite schema_register output_schema progress logging mrtr elicit request_state two_instance; do
    echo "$lst" | grep -qi "$pat" && ok "ctest has a '$pat' test" || bad "no ctest test name matches '$pat'"
  done
  check "ctest passes" ctest --test-dir "$BUILD" --output-on-failure
  for f in docs/QUICKSTART_HTTP.md docs/SECURITY.md docs/CONFORMANCE.md docs/INTEROP.md docs/DECISIONS.md docs/SPEC_NOTES.md docs/4_EVIDENCE.md scripts/fixtures/mrtr_accept_response.json; do
    check "$f present" test -s "$f"
  done
  check "SECURITY.md covers requestState" grep -qi 'requestState' docs/SECURITY.md
  check ">= 7 fuzz targets" bash -c "[ \$(git grep -l LLVMFuzzerTestOneInput | wc -l) -ge 7 ]"
  check "no TODO/placeholder in evidence" bash -c "! grep -qiE 'TODO|TBD|placeholder' docs/4_EVIDENCE.md"
fi

if (( INTEROP == 1 )); then
  echo "== interop (informational)"
  if command -v npx >/dev/null 2>&1; then
    out="$(timeout 90 npx -y @modelcontextprotocol/inspector --cli "$URL" --transport http --method tools/call --tool-name add --tool-arg a=1 --tool-arg b=2 2>&1 </dev/null)"; rc=$?
    echo "$out" | head -c 500; echo
    (( rc == 0 )) && echo "$out" | grep -q '3' && ok "Inspector CLI tools/call add" || warn "Inspector CLI tools/call add did not succeed (rc=$rc; CLI flags may differ)"
  else warn "npx not available"; fi
  if command -v claude >/dev/null 2>&1; then
    CL() { local t="$1"; shift; timeout "$t" claude "$@" </dev/null 2>&1; }
    CL 30 mcp remove cppmcp-verify >/dev/null; CL 30 mcp add --transport http cppmcp-verify "$URL" >/dev/null
    line="$(CL 90 mcp list | grep 'cppmcp-verify')"; echo "$line"
    echo "$line" | grep -q 'Connected' && ! echo "$line" | grep -qi failed && ok "Claude Code connected" || bad "Claude Code not connected"
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