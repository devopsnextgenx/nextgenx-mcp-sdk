#!/usr/bin/env bash
# scripts/verify_5.sh : executable acceptance gate for Phase 5.
# Scope: prompts, resources, URI templates, completion, caching/pagination, file sandbox,
#        MRTR for prompts/get and resources/read, resource subscriptions and list_changed.
# Usage: scripts/verify_5.sh [--slice N] [--sanitizers] [--interop]
# Env:   BUILD_DIR (default build), HTTP_BIN, CLIENT_BIN, STDIO_BIN, SKIP_BUILD=1 (skip cmake build)
#
# ------------------------------ example-server contract ------------------------------
# http_server flags (new): --resource-root DIR   --max-resource-bytes N (default 1048576)
#                          --page-size N         --tick-ms N     --churn-ms N
#                          (existing: --port --mode --request-state-key --validate-output ...)
#   --page-size N  : every list method returns at most N items per page (default: no paging)
#   --tick-ms N    : every N ms update `demo://counter` and `demo://other` alternately and call notify_resource_updated(uri)
#   --churn-ms N   : every N ms toggle a runtime-registered prompt `dynamic_prompt` and resource `demo://dynamic`
#                    and send prompts/resources list_changed notifications
# stdio_server registers the same prompts/resources/templates (file-backed template only when --resource-root is given).
#
# Prompts
#   summarize   args: text (required), style (optional; completion: brief|detailed|bullets)
#               -> one user message, text content containing the supplied text
#   code_review args: language (required), framework (optional)
#               completion: framework depends on context.arguments.language:
#               python -> [django, flask], rust -> [axum, actix], others -> []
#   ask_name    no args; asks via elicitation (form) for {name:string}; completes with text "Hello <name>!"
# Resources (static)
#   demo://readme               text/markdown, non-empty text
#   demo://image                image/png, blob (valid base64)
#   demo://counter              text "counter=<n>" (n increments with --tick-ms)
#   demo://secret/needs_confirm asks via elicitation (form) for {confirm:boolean};
#                               accept+confirm=true -> text "secret-value"; otherwise text "access denied"
# Resource templates
#   demo://users/{id}/profile{?format}   text with the id; ?format=json -> application/json with {"id":"<id>"...}
#   demo://files/{+path}                 file-backed under --resource-root (sandboxed); text vs blob by content
# Completion
#   ref/resource template users, argument id: ids [1,2,42,420,7] filtered by prefix ("4" -> 42, 420)
# Client (http_client) new flags: --request METHOD --params JSON  (prints RESULT <json>; drives MRTR when --elicit given)
#                                 --subscribe-resource URI (repeatable, with --listen)
#   `--listen` now subscribes to tools, prompts and resources list-changed notifications.
#   stdout: NOTIFY <method> [<uri>]    (uri printed for notifications/resources/updated)
# Fixtures authored by the agent from the spec (scripts/fixtures/):
#   mrtr_accept_response.json   (from Phase 4)   exact inputResponses accepting {confirm:true}, key placeholder __KEY__
#   mrtr_name_response.json     exact inputResponses answering the ask_name elicitation with {name:"Ada"}, key __KEY__
#   listen_resources_request.json  complete modern subscriptions/listen request body asking for
#                               resourcesListChanged, promptsListChanged and resourceSubscriptions ["demo://counter"]
# --------------------------------------------------------------------------------------
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
for t in curl jq python3 base64; do need "$t"; done

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
  if cmake -S . -B "$BUILD" >"$LOG_DIR/cfg.log" 2>&1 && cmake --build "$BUILD" -j"$JOBS" >"$LOG_DIR/build.log" 2>&1; then
    ok "configure and build"
  else
    bad "configure and build (see $LOG_DIR/build.log)"; tail -20 "$LOG_DIR/build.log"; exit 1
  fi
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

# ---- sandbox directory for file-backed resources ----
RDIR="$LOG_DIR/root"; mkdir -p "$RDIR/sub"
printf 'hello world' >"$RDIR/hello.txt"
printf '{"k":1}\n' >"$RDIR/sub/data.json"
head -c 64 /dev/urandom >"$RDIR/bin.dat"
head -c 2097152 /dev/zero >"$RDIR/big.bin"
ln -s /etc "$RDIR/escape" 2>/dev/null

KEY="00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
PORT="$(free_port)"
start_server "$PORT" --mode dual --request-state-key "$KEY" --resource-root "$RDIR" --max-resource-bytes 1048576 \
  || { bad "dual server failed to start"; cat "$LOG_DIR/server-$PORT.log"; exit 1; }
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
mreq() { # mreq METHOD NAME PARAMS [EXTRA_META] [URL] -> prints final JSON-RPC message
  msend "${5:-$URL}" "$1" "$2" "$(mbody "$1" "$3" "${4:-{\}}")"; mfinal
}

# ---------- legacy helpers ----------
LH=(-H "Content-Type: application/json" -H "Accept: application/json, text/event-stream")
SID=""
l_init() { # l_init [URL]
  local u="${1:-$URL}"
  curl -sS -m 15 -D "$LOG_DIR/li.hdr" -o "$LOG_DIR/li.body" "${LH[@]}" \
    -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{"elicitation":{"form":{}}},"clientInfo":{"name":"verify","version":"0"}}}' "$u" >/dev/null 2>&1
  SID="$(grep -i '^mcp-session-id:' "$LOG_DIR/li.hdr" | awk '{print $2}' | tr -d '\r\n')"
  curl -sS -m 10 -o /dev/null "${LH[@]}" -H "Mcp-Session-Id: $SID" -H 'MCP-Protocol-Version: 2025-11-25' -d '{"jsonrpc":"2.0","method":"notifications/initialized"}' "$u" >/dev/null 2>&1
}
l_send() { curl -sS -m 25 -D "$LOG_DIR/l.hdr" -o "$LOG_DIR/l.body" "${LH[@]}" -H "Mcp-Session-Id: $SID" -H 'MCP-Protocol-Version: 2025-11-25' -d "$1" "${2:-$URL}" >/dev/null 2>&1; }
l_events() { if grep -qi '^content-type: *text/event-stream' "$LOG_DIR/l.hdr"; then grep '^data:' "$LOG_DIR/l.body" | sed 's/^data: *//'; else cat "$LOG_DIR/l.body"; fi; }
l_final() { l_events | jq -c 'select(has("result") or has("error"))' 2>/dev/null | tail -n1; }
lreq() { l_send "$(jq -nc --arg m "$1" --argjson p "${2:-{\}}" '{jsonrpc:"2.0",id:3,method:$m,params:$p}')"; l_final; }
l_init
[[ -n "$SID" ]] && ok "legacy session established" || bad "legacy initialize failed (no session id)"

stdio_req() { # stdio_req METHOD PARAMS
  [[ -z "$STDIO_BIN" ]] && return 1
  jq -nc --arg m "$1" --argjson p "$2" '{jsonrpc:"2.0",id:1,method:$m,params:($p + {_meta:{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{name:"v",version:"0"},"io.modelcontextprotocol/clientCapabilities":{}}})}' \
    | timeout 10 "$STDIO_BIN" --resource-root "$RDIR" 2>/dev/null | head -n1
}

echo "== slice 1: prompts, resources, templates, caching, pagination"
r="$(mreq server/discover "" '{}')"
echo "$r" | jq -e '.result.capabilities | (.prompts != null) and (.resources != null) and (.completions != null)' >/dev/null 2>&1 \
  && ok "server/discover advertises prompts, resources, completions" || bad "capabilities missing prompts/resources/completions (got: ${r:0:200})"
r="$(mreq tools/list "" '{}')"
echo "$r" | jq -e '[.result.tools[].name | test("^[A-Za-z0-9_.-]{1,128}$")] | all' >/dev/null 2>&1 && ok "all tool names satisfy the naming rule" || bad "a tool name violates [A-Za-z0-9_.-]{1,128}"

r="$(mreq prompts/list "" '{}')"; echo "-- prompts/list: ${r:0:300}"
echo "$r" | jq -e '(.result.prompts|map(.name)|(index("summarize")!=null) and (index("code_review")!=null) and (index("ask_name")!=null)) and (.result.ttlMs|type=="number") and (.result.cacheScope=="public" or .result.cacheScope=="private") and (.result.resultType=="complete")' >/dev/null 2>&1 \
  && ok "prompts/list: prompts + ttlMs + cacheScope + resultType" || bad "prompts/list shape wrong"
echo "$r" | jq -e '.result.prompts[] | select(.name=="summarize") | .arguments | (map(select(.name=="text"))[0].required==true)' >/dev/null 2>&1 && ok "summarize.text is required" || bad "summarize.text should be required"
r="$(mreq prompts/get summarize '{"name":"summarize","arguments":{"text":"abc123","style":"brief"}}')"; echo "-- prompts/get: ${r:0:300}"
echo "$r" | jq -e '.result.messages[0] | (.role=="user" or .role=="assistant") and (.content.type=="text") and (.content.text|contains("abc123"))' >/dev/null 2>&1 && ok "prompts/get returns a message containing the argument" || bad "prompts/get result wrong"
r="$(mreq prompts/get summarize '{"name":"summarize","arguments":{}}')"; echo "$r" | jq -e '.error.code == -32602' >/dev/null 2>&1 && ok "missing required prompt argument -> -32602" || bad "missing prompt argument should be -32602"
r="$(mreq prompts/get nope '{"name":"nope"}')"; echo "$r" | jq -e '.error.code == -32602' >/dev/null 2>&1 && ok "unknown prompt -> -32602" || bad "unknown prompt should be -32602"
r="$(mreq prompts/get other '{"name":"summarize","arguments":{"text":"x"}}')"; echo "$r" | jq -e '.error.code == -32020' >/dev/null 2>&1 && ok "Mcp-Name mismatch on prompts/get -> -32020" || bad "Mcp-Name mismatch on prompts/get should be -32020"

r="$(mreq resources/list "" '{}')"; echo "-- resources/list: ${r:0:300}"
echo "$r" | jq -e '(.result.resources[] | select(.uri=="demo://readme") | .mimeType=="text/markdown") and (.result.ttlMs|type=="number") and (.result.cacheScope|type=="string")' >/dev/null 2>&1 && ok "resources/list shape (mimeType, ttlMs, cacheScope)" || bad "resources/list shape wrong"
r="$(mreq resources/read demo://readme '{"uri":"demo://readme"}')"
echo "$r" | jq -e '.result.contents[0] | (.uri=="demo://readme") and (.mimeType=="text/markdown") and (.text|length>0)' >/dev/null 2>&1 && ok "resources/read text" || bad "resources/read text wrong"
echo "$r" | jq -e '(.result.ttlMs|type=="number") and (.result.cacheScope|type=="string")' >/dev/null 2>&1 && ok "resources/read carries ttlMs and cacheScope" || bad "resources/read lacks cache fields"
r="$(mreq resources/read demo://image '{"uri":"demo://image"}')"
blob="$(echo "$r" | jq -r '.result.contents[0].blob // empty')"
[[ -n "$blob" ]] && echo "$blob" | base64 -d >/dev/null 2>&1 && [[ "$(echo "$r" | jq -r '.result.contents[0].mimeType')" == image/png ]] && ok "resources/read blob is valid base64 image/png" || bad "resources/read blob wrong"
r="$(mreq resources/read demo://missing '{"uri":"demo://missing"}')"; echo "$r" | jq -e '.error.code == -32602' >/dev/null 2>&1 && ok "unknown resource -> -32602 (not empty contents)" || bad "unknown resource should be -32602"
r="$(mreq resources/read demo://other '{"uri":"demo://readme"}')"; echo "$r" | jq -e '.error.code == -32020' >/dev/null 2>&1 && ok "Mcp-Name mismatch on resources/read -> -32020" || bad "Mcp-Name mismatch on resources/read should be -32020"
r="$(mreq resources/templates/list "" '{}')"; echo "-- templates: ${r:0:300}"
echo "$r" | jq -e '(.result.resourceTemplates | map(.uriTemplate) | index("demo://users/{id}/profile{?format}") != null) and (.result.ttlMs|type=="number")' >/dev/null 2>&1 && ok "resources/templates/list advertises the users template" || bad "templates/list missing users template"
r="$(mreq resources/read demo://users/42/profile '{"uri":"demo://users/42/profile"}')"
echo "$r" | jq -e '.result.contents[0].text | contains("42")' >/dev/null 2>&1 && ok "template read: id expanded" || bad "template read failed"
r="$(mreq resources/read 'demo://users/42/profile?format=json' '{"uri":"demo://users/42/profile?format=json"}')"
echo "$r" | jq -e '.result.contents[0] | (.mimeType=="application/json") and ((.text|fromjson|.id|tostring)=="42")' >/dev/null 2>&1 && ok "template read: query variable selects JSON" || bad "template query variable ignored"

P1="$(free_port)"
if start_server "$P1" --mode dual --page-size 1 --resource-root "$RDIR"; then
  U1="http://127.0.0.1:$P1/mcp"; cursor=""; names=""; pages=0
  for _ in 1 2 3 4 5 6; do
    if [[ -z "$cursor" ]]; then params='{}'; else params="$(jq -nc --arg c "$cursor" '{cursor:$c}')"; fi
    r="$(mreq prompts/list "" "$params" '{}' "$U1")"
    names="$names $(echo "$r" | jq -r '.result.prompts[].name' | tr '\n' ' ')"; pages=$((pages+1))
    cursor="$(echo "$r" | jq -r '.result.nextCursor // empty')"; [[ -z "$cursor" ]] && break
  done
  (( pages >= 3 )) && ok "pagination: $pages pages with --page-size 1" || bad "pagination produced only $pages pages"
  all="$(mreq prompts/list "" '{}' '{}' "$URL" | jq -r '.result.prompts[].name' | tr '\n' ' ')"
  [[ "$(echo $names)" == "$(echo $all)" ]] && ok "paged order equals unpaged order (deterministic)" || bad "paged order differs from unpaged order"
  r="$(mreq prompts/list "" '{"cursor":"not-a-valid-cursor"}' '{}' "$U1")"; echo "$r" | jq -e '.error.code == -32602' >/dev/null 2>&1 && ok "invalid cursor -> -32602" || bad "invalid cursor should be -32602"
  a="$(mreq resources/list "" '{}' '{}' "$U1" | jq -r '.result.nextCursor // empty')"; [[ -n "$a" ]] && ok "resources/list paginates" || bad "resources/list did not paginate"
else bad "paging server failed to start"; fi

r="$(lreq prompts/list '{}')"; echo "-- legacy prompts/list: ${r:0:200}"
echo "$r" | jq -e '(.result.prompts|length>=3) and (.result|has("ttlMs")|not) and (.result|has("cacheScope")|not) and (.result|has("resultType")|not)' >/dev/null 2>&1 \
  && ok "legacy prompts/list works and omits 2026-only fields" || bad "legacy prompts/list wrong or leaks ttlMs/cacheScope/resultType"
r="$(lreq prompts/get '{"name":"summarize","arguments":{"text":"legacy-ok"}}')"; echo "$r" | jq -e '.result.messages[0].content.text|contains("legacy-ok")' >/dev/null 2>&1 && ok "legacy prompts/get" || bad "legacy prompts/get"
r="$(lreq resources/read '{"uri":"demo://readme"}')"; echo "$r" | jq -e '(.result.contents[0].text|length>0) and (.result|has("ttlMs")|not)' >/dev/null 2>&1 && ok "legacy resources/read (no cache fields)" || bad "legacy resources/read wrong"
r="$(lreq resources/read '{"uri":"demo://missing"}')"; echo "$r" | jq -e '.error.code == -32002 or .error.code == -32602' >/dev/null 2>&1 && ok "legacy unknown resource -> -32002/-32602" || bad "legacy unknown resource error code wrong"
r="$(lreq resources/templates/list '{}')"; echo "$r" | jq -e '.result.resourceTemplates|length>=1' >/dev/null 2>&1 && ok "legacy templates/list" || bad "legacy templates/list"
r="$(stdio_req resources/read '{"uri":"demo://readme"}')"
if [[ -n "$STDIO_BIN" ]]; then echo "$r" | jq -e '.result.contents[0].text|length>0' >/dev/null 2>&1 && ok "stdio resources/read" || bad "stdio resources/read"; fi
r="$(stdio_req prompts/list '{}')"
if [[ -n "$STDIO_BIN" ]]; then echo "$r" | jq -e '.result.prompts|length>=3' >/dev/null 2>&1 && ok "stdio prompts/list" || bad "stdio prompts/list"; fi

if (( SLICE >= 2 )); then
  echo "== slice 2: completion, file sandbox, MRTR for prompts/get and resources/read"
  r="$(mreq completion/complete "" '{"ref":{"type":"ref/prompt","name":"summarize"},"argument":{"name":"style","value":"b"}}')"; echo "-- completion: ${r:0:300}"
  echo "$r" | jq -e '(.result.completion.values|index("brief")!=null) and (.result.completion.values|length<=100)' >/dev/null 2>&1 && ok "prompt argument completion" || bad "prompt argument completion wrong"
  r="$(mreq completion/complete "" '{"ref":{"type":"ref/resource","uri":"demo://users/{id}/profile{?format}"},"argument":{"name":"id","value":"4"}}')"
  echo "$r" | jq -e '(.result.completion.values|sort) == ["42","420"]' >/dev/null 2>&1 && ok "resource template completion filters by prefix" || bad "template completion wrong (got: ${r:0:200})"
  r="$(mreq completion/complete "" '{"ref":{"type":"ref/prompt","name":"code_review"},"argument":{"name":"framework","value":""},"context":{"arguments":{"language":"python"}}}')"
  echo "$r" | jq -e '(.result.completion.values|sort) == ["django","flask"]' >/dev/null 2>&1 && ok "completion uses context.arguments (python)" || bad "context-dependent completion wrong (python)"
  r="$(mreq completion/complete "" '{"ref":{"type":"ref/prompt","name":"code_review"},"argument":{"name":"framework","value":""},"context":{"arguments":{"language":"rust"}}}')"
  echo "$r" | jq -e '(.result.completion.values|sort) == ["actix","axum"]' >/dev/null 2>&1 && ok "completion uses context.arguments (rust)" || bad "context-dependent completion wrong (rust)"
  r="$(mreq completion/complete "" '{"ref":{"type":"ref/prompt","name":"nope"},"argument":{"name":"x","value":""}}')"; echo "$r" | jq -e '.error.code == -32602' >/dev/null 2>&1 && ok "completion for unknown ref -> -32602" || bad "unknown completion ref should be -32602"
  r="$(lreq completion/complete '{"ref":{"type":"ref/prompt","name":"summarize"},"argument":{"name":"style","value":"d"}}')"; echo "$r" | jq -e '.result.completion.values|index("detailed")!=null' >/dev/null 2>&1 && ok "legacy completion" || bad "legacy completion"

  rf() { mreq resources/read "$1" "$(jq -nc --arg u "$1" '{uri:$u}')"; }
  r="$(rf demo://files/hello.txt)"; echo "$r" | jq -e '.result.contents[0].text=="hello world"' >/dev/null 2>&1 && ok "file resource: text" || bad "file resource text wrong"
  r="$(rf demo://files/sub/data.json)"; echo "$r" | jq -e '.result.contents[0].mimeType=="application/json"' >/dev/null 2>&1 && ok "file resource: MIME by extension" || bad "file resource MIME wrong"
  r="$(rf demo://files/bin.dat)"; n="$(echo "$r" | jq -r '.result.contents[0].blob // empty' | base64 -d 2>/dev/null | wc -c | tr -d ' ')"
  [[ "$n" == 64 ]] && ok "file resource: binary returned as blob (64 bytes)" || bad "binary file resource wrong ($n bytes)"
  for evil in 'demo://files/../../etc/passwd' 'demo://files/..%2f..%2fetc%2fpasswd' 'demo://files/%2e%2e/%2e%2e/etc/passwd' 'demo://files/%252e%252e/etc/passwd' 'demo://files/hello.txt%00.png' 'demo://files/escape/passwd' 'demo://files//etc/passwd' 'demo://files/sub/../../../etc/passwd'; do
    r="$(rf "$evil")"
    if echo "$r" | jq -e '.error != null' >/dev/null 2>&1 && ! echo "$r" | grep -q 'root:'; then ok "sandbox blocks: $evil"; else bad "sandbox escape or leak for: $evil (got: ${r:0:160})"; fi
  done
  r="$(rf demo://files/big.bin)"; echo "$r" | jq -e '.error != null' >/dev/null 2>&1 && ok "oversized file refused (limit 1 MiB)" || bad "2 MiB file should exceed --max-resource-bytes"
  kill -0 "$LAST_PID" 2>/dev/null; r="$(rf demo://readme)"; echo "$r" | jq -e '.result' >/dev/null 2>&1 && ok "server alive after sandbox probes" || bad "server unhealthy after sandbox probes"

  CAPS='{"elicitation":{"form":{}}}'
  NAMEFIX="scripts/fixtures/mrtr_name_response.json"; ACCFIX="scripts/fixtures/mrtr_accept_response.json"
  r="$(mreq prompts/get ask_name '{"name":"ask_name"}')"; echo "-- ask_name round 1: ${r:0:300}"
  echo "$r" | jq -e '.result.resultType=="input_required" and (.result.inputRequests|length==1) and (.result.requestState|length>0)' >/dev/null 2>&1 && ok "prompts/get returns input_required" || bad "prompts/get ask_name should be input_required"
  K="$(echo "$r" | jq -r '.result.inputRequests|keys[0]')"; S="$(echo "$r" | jq -r '.result.requestState')"
  retry() { # retry METHOD NAME PARAMS_NO_META STATE RESPONSES
    local body; body="$(jq -nc --arg m "$1" --argjson p "$3" --arg s "$4" --argjson ir "$5" --argjson caps "$CAPS" \
      '{jsonrpc:"2.0",id:2,method:$m,params:($p + {inputResponses:$ir,requestState:$s,_meta:{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{name:"verify",version:"0"},"io.modelcontextprotocol/clientCapabilities":$caps}})}')"
    msend "$URL" "$1" "$2" "$body"; mfinal
  }
  if [[ -s "$NAMEFIX" ]]; then
    A="$(sed "s/__KEY__/$K/g" "$NAMEFIX")"
    r="$(retry prompts/get ask_name '{"name":"ask_name"}' "$S" "$A")"; echo "-- ask_name retry: ${r:0:300}"
    echo "$r" | jq -e '.result.messages[0].content.text | contains("Ada")' >/dev/null 2>&1 && ok "prompts/get MRTR completes with the answer" || bad "prompts/get MRTR retry failed"
    T="${S%?}X"; [[ "$T" == "$S" ]] && T="${S%?}Y"
    r="$(retry prompts/get ask_name '{"name":"ask_name"}' "$T" "$A")"; echo "$r" | jq -e '.error != null or .result.isError == true' >/dev/null 2>&1 && ok "tampered requestState rejected on prompts/get" || bad "tampered requestState accepted on prompts/get"
    r="$(retry prompts/get summarize '{"name":"summarize","arguments":{"text":"x"}}' "$S" "$A")"; echo "$r" | jq -e '.error != null' >/dev/null 2>&1 && ok "requestState from ask_name rejected for another prompt" || bad "requestState reusable across prompts"
  else bad "missing $NAMEFIX"; fi
  r="$(mreq resources/read demo://secret/needs_confirm '{"uri":"demo://secret/needs_confirm"}')"; echo "-- secret round 1: ${r:0:200}"
  echo "$r" | jq -e '.result.resultType=="input_required"' >/dev/null 2>&1 && ok "resources/read returns input_required" || bad "resources/read secret should be input_required"
  if ! echo "$r" | grep -q 'secret-value'; then ok "secret not leaked before confirmation"; else bad "secret leaked in the first round"; fi
  K="$(echo "$r" | jq -r '.result.inputRequests|keys[0]')"; S="$(echo "$r" | jq -r '.result.requestState')"
  if [[ -s "$ACCFIX" ]]; then
    A="$(sed "s/__KEY__/$K/g" "$ACCFIX")"
    r="$(retry resources/read demo://secret/needs_confirm '{"uri":"demo://secret/needs_confirm"}' "$S" "$A")"
    echo "$r" | jq -e '.result.contents[0].text=="secret-value"' >/dev/null 2>&1 && ok "accepted retry returns the secret" || bad "accepted retry should return secret-value"
    D="$(echo "$A" | jq -c --arg k "$K" '.[$k] |= (.action="decline" | del(.content))')"
    r="$(retry resources/read demo://secret/needs_confirm '{"uri":"demo://secret/needs_confirm"}' "$S" "$D")"
    echo "$r" | jq -e '.result.contents[0].text=="access denied"' >/dev/null 2>&1 && ! echo "$r" | grep -q 'secret-value' && ok "decline -> access denied" || bad "decline should return 'access denied'"
  else bad "missing $ACCFIX"; fi
  CAPS='{}'
  r="$(mreq prompts/get ask_name '{"name":"ask_name"}')"; echo "$r" | jq -e '.error.code == -32021' >/dev/null 2>&1 && ok "no elicitation capability -> -32021 (prompts/get)" || bad "expected -32021 without elicitation capability"
  for mode in modern legacy; do
    out="$(run_client --url "$URL" --mode "$mode" --request prompts/get --params '{"name":"ask_name"}' --elicit accept --elicit-content '{"name":"Ada"}')"; echo "-- client $mode ask_name:"; echo "$out" | head -n 3
    echo "$out" | grep -q 'Ada' && ok "client drives prompts/get elicitation ($mode)" || bad "client prompts/get elicitation failed ($mode)"
    out="$(run_client --url "$URL" --mode "$mode" --request resources/read --params '{"uri":"demo://secret/needs_confirm"}' --elicit accept --elicit-content '{"confirm":true}')"
    echo "$out" | grep -q 'secret-value' && ok "client drives resources/read elicitation ($mode)" || bad "client resources/read elicitation failed ($mode)"
    out="$(run_client --url "$URL" --mode "$mode" --request resources/read --params '{"uri":"demo://secret/needs_confirm"}' --elicit cancel)"
    ! echo "$out" | grep -q 'secret-value' && ok "cancel does not reveal the secret ($mode)" || bad "cancel leaked the secret ($mode)"
  done
fi

if (( SLICE >= 3 )); then
  echo "== slice 3: subscriptions and list_changed"
  P3="$(free_port)"
  if start_server "$P3" --mode dual --tick-ms 300 --churn-ms 500 --resource-root "$RDIR"; then
    U3="http://127.0.0.1:$P3/mcp"
    FIX="scripts/fixtures/listen_resources_request.json"
    if [[ -s "$FIX" ]]; then
      curl -sN -m 4 -D "$LOG_DIR/ls.hdr" -o "$LOG_DIR/ls.body" "${MH[@]}" -H 'Mcp-Method: subscriptions/listen' -d @"$FIX" "$U3" >/dev/null 2>&1
      echo "-- listen transcript:"; head -c 800 "$LOG_DIR/ls.body"; echo
      ev() { grep '^data:' "$LOG_DIR/ls.body" | sed 's/^data: *//'; }
      ev | head -n1 | jq -e '.method=="notifications/subscriptions/acknowledged"' >/dev/null 2>&1 && ok "ack is the first event" || bad "first event must be the acknowledgment"
      REQID="$(jq -c '.id' "$FIX")"
      ev | jq -e --argjson id "$REQID" 'select(.method=="notifications/resources/updated" and .params.uri=="demo://counter") | .params._meta["io.modelcontextprotocol/subscriptionId"] == $id' >/dev/null 2>&1 \
        && ok "resources/updated for the subscribed URI, subscriptionId == request id" || bad "no tagged resources/updated for demo://counter"
      ev | jq -e 'select(.method=="notifications/resources/updated" and .params.uri=="demo://other")' >/dev/null 2>&1 && bad "update delivered for a URI the client did not subscribe to" || ok "no updates for unsubscribed URIs"
      ev | jq -e 'select(.method=="notifications/prompts/list_changed")' >/dev/null 2>&1 && ok "prompts/list_changed delivered" || bad "no prompts/list_changed"
      ev | jq -e 'select(.method=="notifications/resources/list_changed")' >/dev/null 2>&1 && ok "resources/list_changed delivered" || bad "no resources/list_changed"
      jq 'del(.params.resourceSubscriptions) | del(.params.notifications.resourceSubscriptions) | del(.params.subscriptions.resourceSubscriptions)' "$FIX" >"$LOG_DIR/listen_nosub.json"
      curl -sN -m 3 -o "$LOG_DIR/ls2.body" "${MH[@]}" -H 'Mcp-Method: subscriptions/listen' -d @"$LOG_DIR/listen_nosub.json" "$U3" >/dev/null 2>&1
      grep '^data:' "$LOG_DIR/ls2.body" | sed 's/^data: *//' | jq -e 'select(.method=="notifications/resources/updated")' >/dev/null 2>&1 && bad "updates delivered without a resource subscription" || ok "no resource updates without resourceSubscriptions"
    else bad "missing $FIX"; fi

    LSID_SAVE="$SID"; l_init "$U3"
    r="$(l_send '{"jsonrpc":"2.0","id":5,"method":"resources/subscribe","params":{"uri":"demo://counter"}}' "$U3"; l_final)"
    echo "$r" | jq -e '.result != null and .error == null' >/dev/null 2>&1 && ok "legacy resources/subscribe accepted" || bad "legacy resources/subscribe failed (got: ${r:0:160})"
    curl -sN -m 4 -o "$LOG_DIR/lg.body" -H 'Accept: text/event-stream' -H "Mcp-Session-Id: $SID" -H 'MCP-Protocol-Version: 2025-11-25' "$U3" >/dev/null 2>&1
    lev() { grep '^data:' "$LOG_DIR/lg.body" | sed 's/^data: *//'; }
    lev | jq -e 'select(.method=="notifications/resources/updated" and .params.uri=="demo://counter")' >/dev/null 2>&1 && ok "legacy GET stream delivers resources/updated" || bad "legacy GET stream got no resources/updated"
    lev | jq -e 'select(.method=="notifications/resources/updated" and .params.uri=="demo://other")' >/dev/null 2>&1 && bad "legacy: update for unsubscribed URI" || ok "legacy: no updates for unsubscribed URI"
    lev | jq -e 'select(.method=="notifications/prompts/list_changed" or .method=="notifications/resources/list_changed")' >/dev/null 2>&1 && ok "legacy list_changed on GET stream" || bad "legacy list_changed missing"
    l_send '{"jsonrpc":"2.0","id":6,"method":"resources/unsubscribe","params":{"uri":"demo://counter"}}' "$U3"; l_final | jq -e '.result != null' >/dev/null 2>&1 && ok "legacy resources/unsubscribe accepted" || bad "legacy resources/unsubscribe failed"
    sleep 0.5
    curl -sN -m 3 -o "$LOG_DIR/lg2.body" -H 'Accept: text/event-stream' -H "Mcp-Session-Id: $SID" -H 'MCP-Protocol-Version: 2025-11-25' "$U3" >/dev/null 2>&1
    grep '^data:' "$LOG_DIR/lg2.body" | sed 's/^data: *//' | jq -e 'select(.method=="notifications/resources/updated")' >/dev/null 2>&1 && bad "updates continue after unsubscribe" || ok "no updates after unsubscribe"
    SID="$LSID_SAVE"

    for mode in modern legacy; do
      out="$(CLIENT_TIMEOUT=25 run_client --url "$U3" --mode "$mode" --listen --subscribe-resource demo://counter --duration-ms 3500)"
      n="$(echo "$out" | grep -c '^NOTIFY notifications/resources/updated demo://counter')"
      (( n >= 2 )) && ok "client ($mode): $n resource updates" || bad "client ($mode): $n resource updates (need >= 2)"
      echo "$out" | grep -qE '^NOTIFY notifications/(prompts|resources)/list_changed' && ok "client ($mode): list_changed observed" || bad "client ($mode): no list_changed"
    done
  else bad "tick/churn server failed to start"; fi
fi

if (( SLICE >= 4 )); then
  echo "== slice 4: tests, docs, evidence"
  lst="$(ctest --test-dir "$BUILD" -N 2>/dev/null)"
  n="$(echo "$lst" | grep -c 'Test *#')"
  (( n >= 200 )) && ok "ctest discovers $n tests" || bad "only $n tests discovered (need >= 200)"
  for pat in uri_template template_match cursor file_sandbox symlink completion cache_hint registry_churn subscription_limit list_changed mrtr_prompt mrtr_resource; do
    echo "$lst" | grep -qi "$pat" && ok "ctest has a '$pat' test" || bad "no ctest test name matches '$pat'"
  done
  check "ctest passes" ctest --test-dir "$BUILD" --output-on-failure
  for f in docs/QUICKSTART_HTTP.md docs/SECURITY.md docs/CONFORMANCE.md docs/INTEROP.md docs/DECISIONS.md docs/SPEC_NOTES.md docs/5_EVIDENCE.md scripts/fixtures/mrtr_name_response.json scripts/fixtures/listen_resources_request.json; do
    check "$f present" test -s "$f"
  done
  check "SECURITY.md covers the file sandbox" grep -qiE 'symlink' docs/SECURITY.md
  check ">= 11 fuzz targets" bash -c "[ \$(git grep -l LLVMFuzzerTestOneInput | wc -l) -ge 11 ]"
  check "no TODO/placeholder in evidence" bash -c "! grep -qiE 'TODO|TBD|placeholder' docs/5_EVIDENCE.md"
fi

if (( INTEROP == 1 )); then
  echo "== interop (informational)"
  if command -v npx >/dev/null 2>&1; then
    ins() { timeout 90 npx -y @modelcontextprotocol/inspector --cli "$URL" --transport http "$@" 2>/dev/null </dev/null; }
    out="$(ins --method prompts/list)"; echo "$out" | grep -q 'summarize' && ok "Inspector: prompts/list" || warn "Inspector prompts/list failed"
    out="$(ins --method resources/list)"; echo "$out" | grep -q 'demo://readme' && ok "Inspector: resources/list" || warn "Inspector resources/list failed"
    out="$(ins --method resources/read --uri demo://readme)"; echo "$out" | grep -q 'demo://readme' && ok "Inspector: resources/read" || warn "Inspector resources/read failed (flags may differ)"
    out="$(ins --method prompts/get --prompt-name summarize --prompt-args text=hello)"; echo "$out" | grep -q 'hello' && ok "Inspector: prompts/get" || warn "Inspector prompts/get failed (flags may differ)"
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