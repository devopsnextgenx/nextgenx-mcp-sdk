#!/usr/bin/env bash
# scripts/verify_5b.sh : gate for Phase 5B (close the Phase 5 gaps).
# The Phase 5 gate exercised example binaries; this gate verifies the SDK LIBRARY:
# public builder APIs, RFC 6570, stable cursors, typed client APIs, regex engine, sandbox policy, conformance triage.
# Usage: scripts/verify_5b.sh [--slice N] [--sanitizers] [--interop]
# Env:   BUILD_DIR (default build), HTTP_BIN, STDIO_BIN, SKIP_BUILD=1, CONFORMANCE_CMD (optional)
#
# ---- extra example-server contract (new; implemented ONLY through the public SDK API) ----
# Templates registered by http_server and stdio_server. Each read returns ONE text content whose text is a JSON
# object containing exactly the variables that matched (absent variables are omitted; explode `*` variables are arrays):
#   demo://m/simple/{a}/{b}      demo://m/reserved/{+p}      demo://m/frag{#f}     demo://m/label{.x}
#   demo://m/path{/segs*}        demo://m/semi{;k,v}         demo://m/query{?q,limit}
#   demo://m/cont?fixed=1{&extra}
# Public API symbols that must exist in installed headers (include/): see SYMBOLS below.
# Anti-adapter rule: examples/ contains no registry, matcher, cursor or file-provider implementation.
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
  if cmake -S . -B "$BUILD" >"$LOG_DIR/cfg.log" 2>&1 && cmake --build "$BUILD" -j"$JOBS" >"$LOG_DIR/build.log" 2>&1; then ok "configure and build"
  else bad "configure and build (see $LOG_DIR/build.log)"; tail -20 "$LOG_DIR/build.log"; exit 1; fi
fi
find_bin() { find "$BUILD" -type f -perm -u+x \( -name "$1" -o -name "cppmcp_$1" \) 2>/dev/null | head -n1; }
HTTP_BIN="${HTTP_BIN:-$(find_bin http_server)}"
[[ -z "$HTTP_BIN" ]] && { bad "http_server binary not found"; exit 1; }

free_port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
start_server() {
  local port="$1"; shift
  "$HTTP_BIN" --port "$port" "$@" >"$LOG_DIR/server-$port.log" 2>&1 &
  LAST_PID=$!; PIDS+=("$LAST_PID")
  for _ in $(seq 1 60); do curl -s -m 2 -o /dev/null "http://127.0.0.1:$port/" && break; kill -0 "$LAST_PID" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$LAST_PID" 2>/dev/null
}
MH=(-H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" -H "MCP-Protocol-Version: 2026-07-28")
mbody() { jq -nc --arg m "$1" --argjson p "$2" '{jsonrpc:"2.0",id:1,method:$m,params:($p + {_meta:{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientInfo":{name:"verify",version:"0"},"io.modelcontextprotocol/clientCapabilities":{}}})}'; }
mreq() { # mreq URL METHOD NAME PARAMS
  local extra=(-H "Mcp-Method: $2"); [[ -n "$3" ]] && extra+=(-H "Mcp-Name: $3")
  curl -sS -m 25 -D "$LOG_DIR/m.hdr" -o "$LOG_DIR/m.body" "${MH[@]}" "${extra[@]}" -d "$(mbody "$2" "$4")" "$1" >/dev/null 2>&1
  if grep -qi '^content-type: *text/event-stream' "$LOG_DIR/m.hdr"; then grep '^data:' "$LOG_DIR/m.body" | sed 's/^data: *//' | jq -c 'select(has("result") or has("error"))' | tail -n1; else cat "$LOG_DIR/m.body"; fi
}

RDIR="$LOG_DIR/root"; mkdir -p "$RDIR"; printf 'hello world' >"$RDIR/hello.txt"
PORT="$(free_port)"
start_server "$PORT" --mode dual --resource-root "$RDIR" || { bad "server failed to start"; cat "$LOG_DIR/server-$PORT.log"; exit 1; }
URL="http://127.0.0.1:$PORT/mcp"; ok "server running on :$PORT"

# ------------------------------------------------------------------ slice 1
echo "== slice 1: public server API, typed results, RFC 6570, cursors, anti-adapter"
if [[ -x scripts/verify_5.sh ]]; then
  SKIP_BUILD=1 scripts/verify_5.sh --slice 3 >"$LOG_DIR/regress5.log" 2>&1 && ok "Phase 5 gate still passes (slice 3)" || { bad "Phase 5 gate regressed (see $LOG_DIR/regress5.log)"; tail -15 "$LOG_DIR/regress5.log"; }
else warn "scripts/verify_5.sh not found; regression skipped"; fi

SYMBOLS=(
  'Server::prompt|prompt\(' 'resource_template' 'FileResourceProvider' 'ICompletionProvider|CompletionProvider'
  'CacheHint' 'remove_prompt' 'remove_resource' 'notify_resource_updated'
  'uri_template|UriTemplate' 'expand\(' 'match\('
)
for s in "${SYMBOLS[@]}"; do
  git grep -qE "$s" -- 'include/cppmcp' && ok "public header declares: $s" || bad "no public header (include/cppmcp) declares: $s"
done
git grep -qE 'InputRequired' -- 'include/cppmcp' && ok "InputRequired is a public typed result" || bad "InputRequired not public"
if git grep -nE 'std::variant<[^>]*(ToolResult|PromptResult|ReadResult|InputRequired)' -- 'include/cppmcp' >/dev/null 2>&1; then ok "handler results are typed variants (no in-band sentinel)"
else warn "no std::variant handler result found; confirm typed returns in review (tests: tool_result_spoof)"; fi
if git grep -nE '__cppmcp|"\$cppmcp|cppmcp_internal_marker|__internal_tool_result' -- src include >/dev/null 2>&1; then bad "in-band JSON sentinel still present in src/include"; else ok "no in-band JSON sentinel strings in src/include"; fi

EX="$(git ls-files 'examples/*.cpp' 'examples/*.hpp' 'examples/**/*.cpp' 'examples/**/*.hpp' 2>/dev/null)"
if [[ -n "$EX" ]]; then
  # shellcheck disable=SC2086
  if git grep -nE 'match_template|expand_template|class .*Registry|struct .*Registry|openat\(|O_NOFOLLOW|decode_cursor|encode_cursor' -- $EX >/dev/null 2>&1; then
    bad "examples/ still contains registry/matcher/cursor/file-provider logic (must live in the library)"; git grep -nE 'match_template|class .*Registry|struct .*Registry|openat\(|O_NOFOLLOW|decode_cursor|encode_cursor' -- $EX | head -5
  else ok "examples/ contain no registry/matcher/cursor/sandbox implementation"; fi
else bad "no example sources found"; fi

check "installed-consumer API test registered in ctest" bash -c "ctest --test-dir '$BUILD' -N | grep -qi api_consumer"
if ctest --test-dir "$BUILD" -R api_consumer --output-on-failure >"$LOG_DIR/api_consumer.log" 2>&1; then ok "api_consumer test passes"; else bad "api_consumer test failed"; tail -15 "$LOG_DIR/api_consumer.log"; fi
if [[ -d tests/api_consumer ]]; then
  git grep -nE '#include *"(\.\./)*(src|tests)/' -- tests/api_consumer >/dev/null 2>&1 && bad "api_consumer includes private headers" || ok "api_consumer uses only public headers"
  git grep -qE 'resource_template' -- tests/api_consumer && git grep -qE 'prompt' -- tests/api_consumer && git grep -qiE 'complet' -- tests/api_consumer && ok "api_consumer registers prompt + template + completion" || bad "api_consumer must register prompts, templates and completion"
else bad "tests/api_consumer missing"; fi

echo "-- RFC 6570 black box (templates served through the public matcher)"
vars() { # vars URI -> prints JSON object text or an error marker
  local r; r="$(mreq "$URL" resources/read "$1" "$(jq -nc --arg u "$1" '{uri:$u}')")"
  echo "$r" | jq -c 'if .error then {"__error":.error.code} else (.result.contents[0].text | fromjson) end' 2>/dev/null
}
expect() { # expect URI JQ_FILTER DESC
  local v; v="$(vars "$1")"
  if echo "$v" | jq -e "$2" >/dev/null 2>&1; then ok "$3"; else bad "$3 (got: ${v:0:160})"; fi
}
expect 'demo://m/simple/a%2Fb/c'           '. == {"a":"a/b","b":"c"}'                 "simple {a}: %2F decoded once, stays inside the segment"
expect 'demo://m/simple/%252e/c'           '.a == "%2e"'                              "no double decoding (%252e -> %2e)"
expect 'demo://m/reserved/x/y/z'           '.p == "x/y/z"'                            "reserved {+p} spans '/'"
expect 'demo://m/path/a/b/c'               '.segs == ["a","b","c"]'                   "path {/segs*} explode -> list"
expect 'demo://m/query?q=hi&limit=5'       '. == {"q":"hi","limit":"5"}'              "form query {?q,limit}"
expect 'demo://m/query?limit=5'            '. == {"limit":"5"}'                       "query: absent variable omitted"
expect 'demo://m/query'                    '. == {}'                                  "query: no variables -> {}"
expect 'demo://m/semi;k=1;v=2'             '. == {"k":"1","v":"2"}'                   "path-parameter {;k,v}"
expect 'demo://m/label.txt'                '.x == "txt"'                              "label {.x}"
expect 'demo://m/cont?fixed=1&extra=yes'   '.extra == "yes"'                          "continuation {&extra}"
expect 'demo://m/frag#section'             '.f == "section"'                          "fragment {#f}"
expect 'demo://m/simple/onlyone'           '.__error == -32602'                       "non-matching URI -> -32602"
r="$(mreq "$URL" resources/templates/list "" '{}')"
echo "$r" | jq -e '[.result.resourceTemplates[].uriTemplate] | (index("demo://m/path{/segs*}")!=null) and (index("demo://m/query{?q,limit}")!=null)' >/dev/null 2>&1 && ok "templates/list advertises the operator templates" || bad "templates/list missing operator templates"
check "RFC 6570 official vectors vendored" bash -c "ls tests/data 2>/dev/null | grep -qi uritemplate"
check "ctest has uri_template_vectors"      bash -c "ctest --test-dir '$BUILD' -N | grep -qi uri_template_vectors"
check "ctest has template_ambiguous"        bash -c "ctest --test-dir '$BUILD' -N | grep -qi template_ambiguous"
check "ctest has template_roundtrip"        bash -c "ctest --test-dir '$BUILD' -N | grep -qi template_roundtrip"

echo "-- stable, typed cursors"
P1="$(free_port)"
if start_server "$P1" --mode dual --page-size 1 --resource-root "$RDIR"; then
  U1="http://127.0.0.1:$P1/mcp"
  r="$(mreq "$U1" prompts/list "" '{}')"; c1="$(echo "$r" | jq -r '.result.nextCursor // empty')"
  [[ -n "$c1" ]] && ok "page 1 returns a cursor" || bad "no cursor on page 1"
  dec="$(echo "$c1" | tr '_-' '/+' | base64 -d 2>/dev/null | tr -d '\0')"
  if [[ -n "$dec" ]] && ! echo "$dec" | grep -qE '^[0-9]+$' && ! echo "$dec" | grep -qi offset; then ok "cursor is not a bare offset"; else warn "cursor decodes to a bare offset or is not base64 (decoded: ${dec:0:40})"; fi
  r="$(mreq "$U1" prompts/list "" "$(jq -nc --arg c "$c1" '{cursor:$c}')")"; echo "$r" | jq -e '.result.prompts|length>=1' >/dev/null 2>&1 && ok "cursor accepted on the same list" || bad "own cursor rejected"
  rc="$(mreq "$U1" resources/list "" '{}' | jq -r '.result.nextCursor // empty')"
  r="$(mreq "$U1" prompts/list "" "$(jq -nc --arg c "$rc" '{cursor:$c}')")"; echo "$r" | jq -e '.error.code == -32602' >/dev/null 2>&1 && ok "foreign cursor (resources -> prompts) rejected" || bad "cursor from another list was accepted"
  tam="${c1%?}A"; [[ "$tam" == "$c1" ]] && tam="${c1%?}B"
  r="$(mreq "$U1" prompts/list "" "$(jq -nc --arg c "$tam" '{cursor:$c}')")"; echo "$r" | jq -e '.error.code == -32602' >/dev/null 2>&1 && ok "tampered cursor rejected" || warn "tampered cursor accepted (acceptable only if it still decodes to a valid position; see cursor tests)"
  big="$(python3 -c 'print("A"*100000)')"
  r="$(mreq "$U1" prompts/list "" "$(jq -nc --arg c "$big" '{cursor:$c}')")"; echo "$r" | jq -e '.error != null' >/dev/null 2>&1 && ok "oversized cursor rejected" || bad "oversized cursor accepted"
else bad "paging server failed to start"; fi
check "ctest has cursor_stability" bash -c "ctest --test-dir '$BUILD' -N | grep -qi cursor_stability"
check "ctest has tool_result_spoof" bash -c "ctest --test-dir '$BUILD' -N | grep -qi tool_result_spoof"

# ------------------------------------------------------------------ slice 2
if (( SLICE >= 2 )); then
  echo "== slice 2: typed client, library file provider, regex engine, Windows policy, conformance triage"
  for s in list_prompts list_all_prompts get_prompt list_resources list_all_resources list_resource_templates read_resource 'complete\(' ClientCache subscribe_resource; do
    git grep -qE "$s" -- 'include/cppmcp' && ok "public client API: $s" || bad "no public client API: $s"
  done
  for pat in client_prompts client_resources client_completion client_cache client_resubscribe; do
    check "ctest has $pat" bash -c "ctest --test-dir '$BUILD' -N | grep -qi $pat"
  done
  if git grep -nE 'std::regex' -- 'src/*' 'include/*' >/dev/null 2>&1; then bad "std::regex is still used in src/include (schema patterns need a linear-time engine)"; git grep -nE 'std::regex' -- 'src/*' 'include/*' | head -3
  else ok "no std::regex in src/include"; fi
  check "tests include overlapping-alternation ReDoS pattern" bash -c "git grep -qF '(a|aa)+' -- tests"
  check "ctest has regex_redos_budget" bash -c "ctest --test-dir '$BUILD' -N | grep -qi regex_redos_budget"
  check "file provider is a public library class" bash -c "git grep -qE 'class +FileResourceProvider|struct +FileResourceProvider' -- include/cppmcp"
  check "Windows sandbox fails closed (allow_unsafe_windows_sandbox)" bash -c "git grep -q allow_unsafe_windows_sandbox -- include src"
  check "ctest has file_sandbox" bash -c "ctest --test-dir '$BUILD' -N | grep -qi file_sandbox"
  check "request-state key from env/file option documented" bash -c "git grep -qE 'request[-_]state[-_]key[-_]?(file|env)|REQUEST_STATE_KEY' -- examples docs include src"
  check "conformance_server example exists" bash -c "ls examples | grep -qi conformance"
  check "CONFORMANCE.md root-causes failures" bash -c "grep -qi 'root cause' docs/CONFORMANCE.md"
  check "CONFORMANCE.md records the exact command" bash -c "grep -qiE 'npx|conformance' docs/CONFORMANCE.md && grep -qiE 'command' docs/CONFORMANCE.md"
  if [[ -n "${CONFORMANCE_CMD:-}" ]]; then
    out="$(timeout 300 bash -c "$CONFORMANCE_CMD" 2>&1 </dev/null)"; echo "$out" | tail -n 15
    echo "$out" | grep -qiE 'fail(ed)?[: ]+0\b|0 failed' && ok "conformance run reports 0 failures" || bad "conformance run reports failures"
  else warn "CONFORMANCE_CMD not set; conformance not executed by the gate (set it to the documented command)"; fi
fi

# ------------------------------------------------------------------ slice 3
if (( SLICE >= 3 )); then
  echo "== slice 3: tests, docs, evidence"
  lst="$(ctest --test-dir "$BUILD" -N 2>/dev/null)"; n="$(echo "$lst" | grep -c 'Test *#')"
  (( n >= 260 )) && ok "ctest discovers $n tests" || bad "only $n tests discovered (need >= 260)"
  check "ctest passes" ctest --test-dir "$BUILD" --output-on-failure
  for f in docs/5B_EVIDENCE.md docs/5_EVIDENCE.md docs/SECURITY.md docs/CONFORMANCE.md docs/DECISIONS.md docs/SPEC_NOTES.md; do check "$f present" test -s "$f"; done
  check "SECURITY.md describes the regex engine" grep -qiE 're2|linear-time' docs/SECURITY.md
  check "SECURITY.md no longer claims heuristic-only regex defense" bash -c "! grep -qi 'rejects repeated' docs/SECURITY.md"
  check "5_PROGRESS gaps closed or explicitly accepted" bash -c "grep -qiE 'accepted|closed' docs/5B_EVIDENCE.md"
  check ">= 12 fuzz targets" bash -c "[ \$(git grep -l LLVMFuzzerTestOneInput | wc -l) -ge 12 ]"
  check "no TODO/placeholder in evidence" bash -c "! grep -qiE 'TODO|TBD|placeholder' docs/5B_EVIDENCE.md"
  check "installed-consumer verification passes" bash -c "[ ! -x scripts/verify_install.sh ] || scripts/verify_install.sh"
fi

if (( INTEROP == 1 )) && command -v npx >/dev/null 2>&1; then
  echo "== interop (informational)"
  out="$(timeout 90 npx -y @modelcontextprotocol/inspector --cli "$URL" --transport http --method resources/templates/list 2>/dev/null </dev/null)"
  echo "$out" | grep -q 'demo://m/' && ok "Inspector: templates/list shows operator templates" || warn "Inspector templates/list failed"
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