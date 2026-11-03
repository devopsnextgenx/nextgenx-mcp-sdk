#!/usr/bin/env bash
# scripts/verify_7.sh : executable acceptance gate for Phase 7 (MCP Apps, extension io.modelcontextprotocol/ui).
# Usage: scripts/verify_7.sh [--slice N] [--sanitizers] [--interop]
# Env:   BUILD_DIR (default build), HTTP_BIN, STDIO_BIN, DEMO_BIN, SKIP_BUILD=1
#
# ======================= CONTRACT (example binaries, implemented through the PUBLIC API) =======================
# http_server and stdio_server register (same behavior in both):
#   tool  chart_demo {points:integer 1..50}  _meta.ui={resourceUri:"ui://demo/chart"}   (default visibility)
#         result: structuredContent {"points":[<n numbers>]} ALWAYS; content[0].text:
#           client supports the UI extension  -> "Chart ready: <n> points (shown in the app)"
#           client does not                   -> "Chart data: [<comma separated numbers>]"
#   tool  chart_refresh {points}             _meta.ui={visibility:["app"]}   (app-only; structuredContent {"points":[...]}, text "refreshed")
#   tool  form_demo {}                       _meta.ui={resourceUri:"ui://demo/form"}
#   tool  model_only {}                      _meta.ui={visibility:["model"]}  (no UI; app-originated calls must be refused by the HOST bridge)
#   tool  add (existing)                     no _meta.ui at all
#   resource ui://demo/chart  mimeType text/html;profile=mcp-app ; HTML in contents[0].text ; self-contained (no external URLs
#            other than the w3.org SVG namespace) ; contains the lifecycle strings ui/initialize, ui/notifications/initialized,
#            ui/notifications/tool-input, ui/notifications/tool-result, ui/resource-teardown ; has an element id "refresh" whose click
#            calls tools/call chart_refresh through the host
#            _meta.ui on BOTH the resources/read content and the resources/list entry: csp {connectDomains:[],resourceDomains:[],
#            frameDomains:[],baseUriDomains:[]}, permissions {}, prefersBorder true
#   resource ui://demo/form   same MIME ; _meta.ui.csp.connectDomains ["https://api.example.com"] ; permissions {"clipboardWrite":{}}
# http_server flags (new): --apps-listing always|supported_only   --apps-legacy-flat-key
#   always         : tools/list always carries _meta.ui
#   supported_only : when the request does NOT declare the UI extension, tools/list omits _meta.ui from linked tools and omits app-only
#                    tools; results then vary by client capability so the result MUST carry ttlMs==0 or cacheScope=="private"
#   --apps-legacy-flat-key : ALSO emit the deprecated flat _meta["ui/resourceUri"] (default: nested form only)
# Host bridge demo `apps_host_demo` (C++ host using the public AppBridge; a Node process plays the View):
#   flags : --url MCP_URL --tool chart_demo|form_demo --view-cmd "node tests/apps/view_harness.js --stdio" [--probe]
#   stdout lines (one per event):
#     APP discovered tool=<name> ui=<uri>          APP resource mime=<mime> bytes=<n>        APP csp <effective policy string>
#     APP view initialized                         APP tool-input delivered                  APP tool-result delivered
#     APP view tools/call chart_refresh allowed    APP teardown ok
#     with --probe additionally:  APP view tools/call model_only denied | APP view open-link javascript denied | APP view resources/read denied
#   exit status 0 only when every step succeeded
# View harness (agent-authored, Node, no npm deps): tests/apps/view_harness.js
#   --check FILE.html : simulate a host against the HTML (mini DOM stub), exit 0 and print VIEW_OK
#   --stdio [--probe] : run the HTML as the View over stdin/stdout JSON lines (host->view on stdin, view->host on stdout);
#                       after tool-result it clicks #refresh; with --probe it also attempts tools/call model_only,
#                       ui/open-link javascript:alert(1), and resources/read of a non-ui URI
# =================================================================================================================
set -u

SLICE=3; SAN=0; INTEROP=0
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
HTTP_BIN="${HTTP_BIN:-$(find_bin http_server)}"; STDIO_BIN="${STDIO_BIN:-$(find_bin stdio_server)}"; DEMO_BIN="${DEMO_BIN:-$(find_bin apps_host_demo)}"
[[ -z "$HTTP_BIN" ]] && { bad "http_server binary not found"; exit 1; }

free_port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
start_srv() { local p="$1"; shift; "$HTTP_BIN" --port "$p" "$@" >"$LOG_DIR/server-$p.log" 2>&1 & LAST_PID=$!; PIDS+=("$LAST_PID")
  for _ in $(seq 1 60); do curl -s -m 2 -o /dev/null "http://127.0.0.1:$p/" && break; kill -0 "$LAST_PID" 2>/dev/null || break; sleep 0.1; done; kill -0 "$LAST_PID" 2>/dev/null; }

MH=(-H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" -H "MCP-Protocol-Version: 2026-07-28")
UICAP='{"extensions":{"io.modelcontextprotocol/ui":{"mimeTypes":["text/html;profile=mcp-app"]}}}'
CAPS='{}'
mbody() { jq -nc --arg m "$1" --argjson p "$2" --argjson caps "$CAPS" '{jsonrpc:"2.0",id:1,method:$m,params:($p + {_meta:{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{name:"verify",version:"0"},"io.modelcontextprotocol/clientCapabilities":$caps}})}'; }
mreq() { # mreq URL METHOD NAME PARAMS
  local extra=(-H "Mcp-Method: $2"); [[ -n "$3" ]] && extra+=(-H "Mcp-Name: $3")
  curl -sS -m 25 -D "$LOG_DIR/m.hdr" -o "$LOG_DIR/m.body" "${MH[@]}" "${extra[@]}" -d "$(mbody "$2" "$4")" "$1" >/dev/null 2>&1
  if grep -qi '^content-type: *text/event-stream' "$LOG_DIR/m.hdr"; then grep '^data:' "$LOG_DIR/m.body" | sed 's/^data: *//' | jq -c 'select(has("result") or has("error"))' | tail -n1; else cat "$LOG_DIR/m.body"; fi
}
LH=(-H "Content-Type: application/json" -H "Accept: application/json, text/event-stream")
SID=""
l_init() { # l_init URL CAPS_JSON
  curl -sS -m 15 -D "$LOG_DIR/li.hdr" -o /dev/null "${LH[@]}" -d "$(jq -nc --argjson c "$2" '{jsonrpc:"2.0",id:1,method:"initialize",params:{protocolVersion:"2025-11-25",capabilities:$c,clientInfo:{name:"verify",version:"0"}}}')" "$1" >/dev/null 2>&1
  SID="$(grep -i '^mcp-session-id:' "$LOG_DIR/li.hdr" | awk '{print $2}' | tr -d '\r\n')"
  curl -sS -m 10 -o /dev/null "${LH[@]}" -H "Mcp-Session-Id: $SID" -H 'MCP-Protocol-Version: 2025-11-25' -d '{"jsonrpc":"2.0","method":"notifications/initialized"}' "$1" >/dev/null 2>&1
}
lreq() { # lreq URL METHOD PARAMS
  curl -sS -m 25 -D "$LOG_DIR/l.hdr" -o "$LOG_DIR/l.body" "${LH[@]}" -H "Mcp-Session-Id: $SID" -H 'MCP-Protocol-Version: 2025-11-25' -d "$(jq -nc --arg m "$2" --argjson p "$3" '{jsonrpc:"2.0",id:3,method:$m,params:$p}')" "$1" >/dev/null 2>&1
  if grep -qi '^content-type: *text/event-stream' "$LOG_DIR/l.hdr"; then grep '^data:' "$LOG_DIR/l.body" | sed 's/^data: *//' | jq -c 'select(has("result") or has("error"))' | tail -n1; else cat "$LOG_DIR/l.body"; fi
}

PA="$(free_port)"; URL="http://127.0.0.1:$PA/mcp"
start_srv "$PA" --mode dual || { bad "server failed to start"; cat "$LOG_DIR/server-$PA.log"; exit 1; }
ok "server running on :$PA"

# ------------------------------------------------------------------ slice 1
echo "== slice 1: server API, UI resources, negotiation, degradation"
echo "-- carry-over and anti-adapter"
check "scripts/verify_all.sh exists" test -x scripts/verify_all.sh
check "ctest has jwt_vectors"        bash -c "ctest --test-dir '$BUILD' -N | grep -qi jwt_vectors"
check "ctest has single_flight_50"   bash -c "ctest --test-dir '$BUILD' -N | grep -qi single_flight_50"
if [[ -x scripts/verify_6.sh ]]; then
  SKIP_BUILD=1 scripts/verify_6.sh --slice 2 >"$LOG_DIR/regress6.log" 2>&1 && ok "Phase 6 gate still passes (slice 2)" || { bad "Phase 6 gate regressed (see $LOG_DIR/regress6.log)"; tail -12 "$LOG_DIR/regress6.log"; }
else warn "scripts/verify_6.sh missing; regression skipped"; fi
for s in 'AppBridge' 'IViewChannel' 'UiLaunch' 'UiResourceSpec|AppSpec' 'app_tool|Server::app' 'client_supports_ui|supports_ui' 'build_csp|CspBuilder|CspPolicy' 'io\.modelcontextprotocol/ui' 'text/html;profile=mcp-app'; do
  git grep -qE "$s" -- 'include/cppmcp' && ok "public header declares: $s" || bad "no public header declares: $s"
done
if git grep -lE 'ui/initialize|ui/notifications' -- 'examples/*.cpp' 'examples/**/*.cpp' 2>/dev/null | grep -q .; then bad "examples/*.cpp contains Apps protocol logic (must be in the library; HTML assets are allowed)"; else ok "examples contain no Apps protocol logic in C++"; fi
check "api_consumer covers apps (ctest api_consumer_apps)" bash -c "ctest --test-dir '$BUILD' -N | grep -qi api_consumer_apps"

echo "-- tools/list: linkage and visibility"
r="$(mreq "$URL" tools/list "" '{}')"
echo "$r" | jq -e '.result.tools[] | select(.name=="chart_demo") | ._meta.ui.resourceUri == "ui://demo/chart"' >/dev/null 2>&1 && ok "chart_demo links ui://demo/chart via _meta.ui.resourceUri" || bad "chart_demo missing _meta.ui.resourceUri"
echo "$r" | jq -e '.result.tools[] | select(.name=="form_demo") | ._meta.ui.resourceUri == "ui://demo/form"' >/dev/null 2>&1 && ok "form_demo links ui://demo/form" || bad "form_demo link missing"
echo "$r" | jq -e '.result.tools[] | select(.name=="chart_demo") | ._meta | has("ui/resourceUri") | not' >/dev/null 2>&1 && ok "deprecated flat _meta[\"ui/resourceUri\"] not emitted by default" || bad "flat deprecated key emitted by default"
echo "$r" | jq -e '.result.tools[] | select(.name=="chart_refresh") | ._meta.ui.visibility == ["app"]' >/dev/null 2>&1 && ok "chart_refresh is app-only (visibility [\"app\"])" || bad "chart_refresh visibility wrong"
echo "$r" | jq -e '.result.tools[] | select(.name=="model_only") | ._meta.ui.visibility == ["model"]' >/dev/null 2>&1 && ok "model_only visibility [\"model\"]" || bad "model_only visibility wrong"
echo "$r" | jq -e '.result.tools[] | select(.name=="add") | (._meta.ui // null) == null' >/dev/null 2>&1 && ok "non-UI tools carry no _meta.ui" || bad "add must not carry _meta.ui"

echo "-- ui:// resources"
r="$(mreq "$URL" resources/list "" '{}')"
echo "$r" | jq -e '[.result.resources[] | select(.uri|startswith("ui://")) | .mimeType] | (length>=2) and all(.=="text/html;profile=mcp-app")' >/dev/null 2>&1 && ok "resources/list lists ui:// resources with the Apps MIME type" || bad "ui:// resources missing or wrong MIME in resources/list"
echo "$r" | jq -e '.result.resources[] | select(.uri=="ui://demo/chart") | ._meta.ui | has("csp") or has("prefersBorder")' >/dev/null 2>&1 && ok "resources/list entry carries _meta.ui" || bad "resources/list entry lacks _meta.ui"
r="$(mreq "$URL" resources/read "ui://demo/chart" '{"uri":"ui://demo/chart"}')"; echo "$r" | jq -r '.result.contents[0].text // empty' >"$LOG_DIR/chart.html"
echo "$r" | jq -e '.result.contents[0] | (.mimeType=="text/html;profile=mcp-app") and (.uri=="ui://demo/chart") and (.text|type=="string" and length>200)' >/dev/null 2>&1 && ok "resources/read returns HTML text with the Apps MIME type" || bad "ui://demo/chart read wrong"
echo "$r" | jq -e '.result.contents[0]._meta.ui | (.csp.connectDomains==[]) and (.csp.resourceDomains==[]) and (.prefersBorder==true)' >/dev/null 2>&1 && ok "read response carries _meta.ui (deny-by-default CSP, prefersBorder)" || bad "read response lacks the expected _meta.ui"
echo "$r" | jq -e '(.result.ttlMs|type=="number") and (.result.cacheScope|type=="string")' >/dev/null 2>&1 && ok "modern read carries ttlMs/cacheScope" || bad "modern read lacks cache fields"
for str in 'ui/initialize' 'ui/notifications/initialized' 'ui/notifications/tool-input' 'ui/notifications/tool-result' 'ui/resource-teardown' 'id="refresh"'; do
  grep -qF "$str" "$LOG_DIR/chart.html" && ok "chart HTML contains: $str" || bad "chart HTML lacks: $str"
done
if grep -oE 'https?://[^"'"'"' )>]+' "$LOG_DIR/chart.html" | grep -v 'www.w3.org' | grep -q .; then bad "chart HTML references external URLs (must be self-contained)"; else ok "chart HTML is self-contained (no external URLs)"; fi
r="$(mreq "$URL" resources/read "ui://demo/form" '{"uri":"ui://demo/form"}')"
echo "$r" | jq -e '.result.contents[0]._meta.ui | (.csp.connectDomains==["https://api.example.com"]) and (.permissions|has("clipboardWrite"))' >/dev/null 2>&1 && ok "form UI declares connectDomains and permissions" || bad "form UI CSP/permissions not passed through"
r="$(mreq "$URL" resources/read "ui://demo/nope" '{"uri":"ui://demo/nope"}')"; echo "$r" | jq -e '.error.code==-32602' >/dev/null 2>&1 && ok "unknown ui:// resource -> -32602" || bad "unknown ui:// resource should be -32602"

echo "-- capability negotiation and graceful degradation"
CAPS='{}'; r="$(mreq "$URL" tools/call chart_demo '{"name":"chart_demo","arguments":{"points":5}}')"
echo "$r" | jq -e '(.result.structuredContent.points|length==5) and (.result.content[0].text|test("^Chart data: "))' >/dev/null 2>&1 && ok "no UI capability: text fallback with data + structuredContent" || bad "fallback result wrong (${r:0:200})"
CAPS="$UICAP"; r="$(mreq "$URL" tools/call chart_demo '{"name":"chart_demo","arguments":{"points":5}}')"
echo "$r" | jq -e '(.result.structuredContent.points|length==5) and (.result.content[0].text|test("shown in the app"))' >/dev/null 2>&1 && ok "UI capability declared: UI-oriented result" || bad "UI result wrong (${r:0:200})"
CAPS='{"extensions":{"io.modelcontextprotocol/ui":{"mimeTypes":["text/plain"]}}}'; r="$(mreq "$URL" tools/call chart_demo '{"name":"chart_demo","arguments":{"points":3}}')"
echo "$r" | jq -e '.result.content[0].text|test("^Chart data: ")' >/dev/null 2>&1 && ok "extension declared without the Apps MIME type -> treated as unsupported" || bad "MIME list must be checked"
CAPS='{}'; r="$(mreq "$URL" tools/call chart_demo '{"name":"chart_demo","arguments":{"points":0}}')"; echo "$r" | jq -e '.result.isError==true' >/dev/null 2>&1 && ok "schema validation still applies to UI tools" || bad "invalid points should be isError"
l_init "$URL" '{}'; r="$(lreq "$URL" tools/call '{"name":"chart_demo","arguments":{"points":4}}')"
echo "$r" | jq -e '.result.content[0].text|test("^Chart data: ")' >/dev/null 2>&1 && ok "legacy session without extension -> fallback" || bad "legacy fallback wrong"
l_init "$URL" "$UICAP"; r="$(lreq "$URL" tools/call '{"name":"chart_demo","arguments":{"points":4}}')"
echo "$r" | jq -e '.result.content[0].text|test("shown in the app")' >/dev/null 2>&1 && ok "legacy session with extension -> UI-oriented result" || bad "legacy UI result wrong"
r="$(lreq "$URL" resources/read '{"uri":"ui://demo/chart"}')"; echo "$r" | jq -e '(.result.contents[0].mimeType=="text/html;profile=mcp-app") and (.result|has("ttlMs")|not) and (.result.contents[0]._meta.ui.csp != null)' >/dev/null 2>&1 && ok "legacy read: Apps MIME + _meta.ui, no 2026-only fields" || bad "legacy ui read wrong"

PB="$(free_port)"
if start_srv "$PB" --mode dual --apps-listing supported_only; then
  UB="http://127.0.0.1:$PB/mcp"
  CAPS='{}'; r="$(mreq "$UB" tools/list "" '{}')"
  echo "$r" | jq -e '([.result.tools[].name]|index("chart_refresh")==null) and ([.result.tools[]|select(.name=="chart_demo")|(._meta.ui // null)]|.[0]==null)' >/dev/null 2>&1 && ok "supported_only: no UI meta and no app-only tools for unsupported clients" || bad "supported_only listing wrong for unsupported clients"
  echo "$r" | jq -e '(.result.ttlMs==0) or (.result.cacheScope=="private")' >/dev/null 2>&1 && ok "capability-dependent listing is not shared-cacheable (ttlMs 0 or private)" || bad "capability-dependent listing must be ttlMs=0 or cacheScope=private"
  CAPS="$UICAP"; r="$(mreq "$UB" tools/list "" '{}')"
  echo "$r" | jq -e '([.result.tools[].name]|index("chart_refresh")!=null) and ([.result.tools[]|select(.name=="chart_demo")|._meta.ui.resourceUri]|.[0]=="ui://demo/chart")' >/dev/null 2>&1 && ok "supported_only: full listing for UI-capable clients" || bad "supported_only listing wrong for UI-capable clients"
else bad "supported_only server failed to start"; fi
PC="$(free_port)"
if start_srv "$PC" --mode dual --apps-legacy-flat-key; then
  CAPS='{}'; r="$(mreq "http://127.0.0.1:$PC/mcp" tools/list "" '{}')"
  echo "$r" | jq -e '.result.tools[]|select(.name=="chart_demo")|(._meta["ui/resourceUri"]=="ui://demo/chart") and (._meta.ui.resourceUri=="ui://demo/chart")' >/dev/null 2>&1 && ok "--apps-legacy-flat-key emits both forms" || bad "flat alias option wrong"
else bad "flat-key server failed to start"; fi

if [[ -n "$STDIO_BIN" ]]; then
  out="$(jq -nc '{jsonrpc:"2.0",id:1,method:"resources/read",params:{uri:"ui://demo/chart",_meta:{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{name:"v",version:"0"},"io.modelcontextprotocol/clientCapabilities":{}}}}' | timeout 10 "$STDIO_BIN" 2>/dev/null | head -n1)"
  echo "$out" | jq -e '.result.contents[0].mimeType=="text/html;profile=mcp-app"' >/dev/null 2>&1 && ok "stdio serves ui:// resources" || bad "stdio ui:// read failed"
fi

echo "-- the example View is a valid MCP App"
if command -v node >/dev/null 2>&1 && [[ -f tests/apps/view_harness.js ]]; then
  out="$(timeout 30 node tests/apps/view_harness.js --check "$LOG_DIR/chart.html" 2>&1 </dev/null)"; rc=$?
  (( rc == 0 )) && echo "$out" | grep -q VIEW_OK && ok "View harness: handshake, tool-input/result, refresh via host, teardown" || { bad "View harness failed (rc=$rc)"; echo "$out" | tail -5; }
else bad "node or tests/apps/view_harness.js missing (required; the harness has no npm dependencies)"; fi

# ------------------------------------------------------------------ slice 2
if (( SLICE >= 2 )); then
  echo "== slice 2: host bridge"
  for s in 'class +AppBridge|struct +AppBridge' 'IViewChannel' 'IHostActions' 'IUiResourceReviewer|UiResourceReviewer' 'visibility' 'build_csp|CspBuilder|CspPolicy' 'call_tool_with_ui|resolve_ui|UiLaunch'; do
    git grep -qE "$s" -- 'include/cppmcp' && ok "public host API: $s" || bad "no public host API: $s"
  done
  for pat in appbridge_lifecycle appbridge_visibility appbridge_open_link appbridge_resources_policy appbridge_teardown appbridge_csp appbridge_unknown_method appbridge_size_limit ui_resource_prefetch ui_meta_roundtrip; do
    check "ctest has $pat" bash -c "ctest --test-dir '$BUILD' -N | grep -qi $pat"
  done
  if [[ -n "$DEMO_BIN" ]] && command -v node >/dev/null 2>&1; then
    VIEW='node tests/apps/view_harness.js --stdio'
    out="$(timeout 60 "$DEMO_BIN" --url "$URL" --tool chart_demo --view-cmd "$VIEW" --probe </dev/null 2>&1)"; rc=$?; echo "$out" | head -n 16
    (( rc == 0 )) && ok "apps_host_demo exits 0" || bad "apps_host_demo failed (rc=$rc)"
    for pat in '^APP discovered tool=chart_demo ui=ui://demo/chart' '^APP resource mime=text/html;profile=mcp-app bytes=[0-9]+' '^APP view initialized' '^APP tool-input delivered' '^APP tool-result delivered' \
               '^APP view tools/call chart_refresh allowed' '^APP view tools/call model_only denied' '^APP view open-link javascript denied' '^APP view resources/read denied' '^APP teardown ok'; do
      echo "$out" | grep -qE "$pat" && ok "event: ${pat//^APP /}" || bad "missing event: $pat"
    done
    echo "$out" | grep -E '^APP csp ' | grep -q "default-src 'none'" && ok "effective CSP is deny-by-default (default-src 'none')" || bad "CSP must start from default-src 'none'"
    echo "$out" | grep -E '^APP csp ' | grep -q 'https://api.example.com' && bad "chart CSP must not allow external domains" || ok "chart CSP has no external domains"
    out="$(timeout 60 "$DEMO_BIN" --url "$URL" --tool form_demo --view-cmd "$VIEW" </dev/null 2>&1)"
    echo "$out" | grep -E '^APP csp ' | grep -q 'https://api.example.com' && ok "form CSP includes the declared connect domain" || bad "form CSP lacks https://api.example.com"
    PD="$(free_port)"; start_srv "$PD" --mode legacy >/dev/null 2>&1
    out="$(timeout 60 "$DEMO_BIN" --url "http://127.0.0.1:$PD/mcp" --tool chart_demo --view-cmd "$VIEW" </dev/null 2>&1)"; rc=$?
    (( rc == 0 )) && ok "host bridge also works over a legacy-only server" || warn "host bridge over legacy-only server failed (rc=$rc)"
  else bad "apps_host_demo binary or node missing"; fi
fi

# ------------------------------------------------------------------ slice 3
if (( SLICE >= 3 )); then
  echo "== slice 3: tests, docs, evidence"
  lst="$(ctest --test-dir "$BUILD" -N 2>/dev/null)"; n="$(echo "$lst" | grep -c 'Test *#')"
  (( n >= 480 )) && ok "ctest discovers $n tests" || bad "only $n tests discovered (need >= 480)"
  check "ctest passes" ctest --test-dir "$BUILD" --output-on-failure
  for f in docs/APPS.md docs/SECURITY.md docs/7_EVIDENCE.md docs/DECISIONS.md docs/SPEC_NOTES.md docs/INTEROP.md; do check "$f present" test -s "$f"; done
  check "APPS.md documents the deprecated flat key decision" grep -qiE 'ui/resourceUri' docs/APPS.md
  check "SECURITY.md covers Apps (CSP, iframe sandbox, link scheme)" bash -c "grep -qi 'CSP' docs/SECURITY.md && grep -qiE 'sandbox' docs/SECURITY.md && grep -qiE 'open-link|javascript:' docs/SECURITY.md"
  check ">= 17 fuzz targets" bash -c "[ \$(git grep -l LLVMFuzzerTestOneInput | wc -l) -ge 17 ]"
  check "no TODO/placeholder in evidence" bash -c "! grep -qiE 'TODO|TBD|placeholder' docs/7_EVIDENCE.md"
  check "installed-consumer verification passes" bash -c "[ ! -x scripts/verify_install.sh ] || scripts/verify_install.sh"
fi

if (( INTEROP == 1 )); then
  echo "== interop (informational)"
  command -v npx >/dev/null 2>&1 && {
    out="$(timeout 90 npx -y @modelcontextprotocol/inspector --cli "$URL" --transport http --method resources/read --uri ui://demo/chart 2>/dev/null </dev/null)"
    echo "$out" | grep -q 'mcp-app' && ok "Inspector reads the ui:// resource" || warn "Inspector read of ui://demo/chart failed (flags may differ)"
  }
  warn "Run the official ext-apps basic-host (github.com/modelcontextprotocol/ext-apps, examples/basic-host) against $URL and record what renders in docs/INTEROP.md"
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