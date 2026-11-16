#!/usr/bin/env bash
# System test: two independent lansync-cli daemons (separate processes, data dirs,
# identities, databases) discover each other over UDP, pair, and sync real files.
#
# Usage: scripts/system_test.sh path/to/lansync-cli
# Exit status 0 = all checks passed. Uses only loopback; no cloud, no root.
set -u

CLI="${1:?usage: $0 path/to/lansync-cli}"
[ -x "$CLI" ] || { echo "not executable: $CLI"; exit 2; }

WORK="$(mktemp -d)"
PIDS=()
FAILS=0
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

pass() { echo "  PASS  $*"; }
fail() { echo "  FAIL  $*"; FAILS=$((FAILS+1)); }

# wait_for <description> <timeout-seconds> <command...>
wait_for() {
  local what="$1" t="$2"; shift 2
  local end=$((SECONDS + t))
  while [ $SECONDS -lt $end ]; do "$@" >/dev/null 2>&1 && { pass "$what"; return 0; }; sleep 0.2; done
  fail "$what (timed out after ${t}s)"; return 1
}
same_file() { [ -f "$1" ] && [ -f "$2" ] && cmp -s "$1" "$2"; }
has_line() { grep -q "$2" "$1" 2>/dev/null; }

freeport() { python3 - <<'PY'
import socket
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])
PY
}

UA=$(freeport); UB=$(freeport)
mkdir -p "$WORK/alice/docs" "$WORK/bob/docs"

start_node() {  # <name> <udp> <peer-udp> <peer-name> <log>
  "$CLI" --data-dir "$WORK/$1/data" --device-name "$1" --log-level warn daemon \
    --bind 127.0.0.1 --listen-port 0 --discovery-port "$2" --discovery-target "127.0.0.1:$3" --no-multicast \
    --pair-with "$4" --share "docs=$WORK/$1/docs" --auto-approve-pairing \
    >"$5" 2>"$5.err" &
  PIDS+=($!)
  echo $! >"$WORK/$1.pid"
}

echo "== two processes on one machine"
start_node alice "$UA" "$UB" bob   "$WORK/alice.log"
start_node bob   "$UB" "$UA" alice "$WORK/bob.log"

wait_for "both daemons ready"            15 bash -c "grep -q READY '$WORK/alice.log' && grep -q READY '$WORK/bob.log'"
wait_for "devices paired with approval"  30 bash -c "grep -q '^PAIRED' '$WORK/alice.log' && grep -q '^PAIRED' '$WORK/bob.log'"
wait_for "folders shared on both sides"  30 bash -c "grep -q '^SHARED' '$WORK/alice.log' && grep -q '^SHARED' '$WORK/bob.log'"

# The pairing codes shown on both sides must match.
CA=$(grep -o 'code=[0-9]*' "$WORK/alice.log" | head -1); CB=$(grep -o 'code=[0-9]*' "$WORK/bob.log" | head -1)
[ -n "$CA" ] && [ "$CA" = "$CB" ] && pass "pairing codes match ($CA)" || fail "pairing codes differ ('$CA' vs '$CB')"

echo "== file creation, nesting, modification"
echo "hello from alice" >"$WORK/alice/docs/hello.txt"
mkdir -p "$WORK/alice/docs/deep/er/still"; echo nested >"$WORK/alice/docs/deep/er/still/n.txt"
wait_for "A -> B: new file"        20 same_file "$WORK/alice/docs/hello.txt" "$WORK/bob/docs/hello.txt"
wait_for "A -> B: nested dirs"     20 same_file "$WORK/alice/docs/deep/er/still/n.txt" "$WORK/bob/docs/deep/er/still/n.txt"
sleep 1; echo "edited by bob" >"$WORK/bob/docs/hello.txt"
wait_for "B -> A: modification"    20 same_file "$WORK/alice/docs/hello.txt" "$WORK/bob/docs/hello.txt"

echo "== large file (200 MB) integrity"
head -c 209715200 /dev/urandom >"$WORK/big.tmp" && mv "$WORK/big.tmp" "$WORK/alice/docs/big.bin"
START=$SECONDS
wait_for "large file arrives intact" 120 same_file "$WORK/alice/docs/big.bin" "$WORK/bob/docs/big.bin"
echo "        ($((SECONDS-START))s)"
[ -z "$(find "$WORK/bob/docs" -name '*.part' -not -path '*/.lansync/*')" ] && pass "no partial files visible" || fail "partial file visible"

echo "== deletion"
rm "$WORK/alice/docs/deep/er/still/n.txt"
wait_for "deletion propagates" 20 bash -c "[ ! -e '$WORK/bob/docs/deep/er/still/n.txt' ]"

echo "== device disconnection and reconnection"
kill "$(cat "$WORK/bob.pid")"; wait "$(cat "$WORK/bob.pid")" 2>/dev/null
echo "made while bob was offline" >"$WORK/alice/docs/offline.txt"
sleep 1
start_node bob "$UB" "$UA" alice "$WORK/bob2.log"
wait_for "B restarts and catches up" 40 same_file "$WORK/alice/docs/offline.txt" "$WORK/bob/docs/offline.txt"

echo "== concurrent edits keep both versions"
kill -STOP "$(cat "$WORK/bob.pid")" 2>/dev/null; sleep 0.2; kill -CONT "$(cat "$WORK/bob.pid")" 2>/dev/null
echo "base" >"$WORK/alice/docs/c.txt"
wait_for "baseline arrives" 20 same_file "$WORK/alice/docs/c.txt" "$WORK/bob/docs/c.txt"
kill "$(cat "$WORK/bob.pid")"; wait "$(cat "$WORK/bob.pid")" 2>/dev/null     # B offline: no sync possible
sleep 1; echo "alice side" >"$WORK/alice/docs/c.txt"
sleep 1.2; echo "bob side" >"$WORK/bob/docs/c.txt"
start_node bob "$UB" "$UA" alice "$WORK/bob3.log"
both() { [ "$(ls "$1"/c*.txt | wc -l)" -ge 2 ] && cat "$1"/c*.txt | grep -q "alice side" && cat "$1"/c*.txt | grep -q "bob side"; }
wait_for "A has both versions"  40 both "$WORK/alice/docs"
wait_for "B has both versions"  40 both "$WORK/bob/docs"
same_file "$WORK/alice/docs/c.txt" "$WORK/bob/docs/c.txt" && pass "same winner on both sides" || fail "different winners"

echo
if [ "$FAILS" -eq 0 ]; then echo "SYSTEM TEST PASSED"; exit 0; fi
echo "SYSTEM TEST FAILED ($FAILS check(s))"
for f in "$WORK"/*.log "$WORK"/*.err; do echo "--- $f"; tail -5 "$f" 2>/dev/null; done
exit 1
