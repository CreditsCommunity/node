#!/usr/bin/env bash
# Offline checks of one sample client, without any real node:
#   check.sh <client command...>      e.g. check.sh ./go/credits-client
# 1. `sign` prints the bytes and signature of sign-expected.txt;
# 2. `transfer` to fake_node.py is accepted (well-formed transfer, valid signature).
# The client takes "--node HOST:PORT" (Go: "-node"; set NODE_FLAG). Needs python3 with
# thriftpy2, base58 and PyNaCl for the fake node.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
KEY="$HERE/test-key.txt"
TO=u6PJ8DtQuPFnfmwHbGFULQ4u4EgjDiyYKjVEsynXq2w
PORT="${PORT:-19090}"
NODE_FLAG="${NODE_FLAG:---node}"

echo "== sign"
"$@" sign "$KEY" "$TO" 12.5 7 | tee /dev/stderr | diff -u "$HERE/sign-expected.txt" -

echo "== transfer to the fake node"
log="$(mktemp)"
python3 "$HERE/fake_node.py" --port "$PORT" >"$log" 2>&1 &
fake=$!
trap 'kill $fake 2>/dev/null || true; rm -f "$log"' EXIT
for _ in $(seq 50); do grep -q listening "$log" && break; sleep 0.2; done
out="$("$@" "$NODE_FLAG" "127.0.0.1:$PORT" transfer "$KEY" "$TO" 12.5)"
echo "$out"
cat "$log"
[ "$out" = "sent: inner id 7, round 1, fee 0.00874 CS" ]
grep -q "^ACCEPTED: FAe4sisG95oZ42w7buUn5qEE4TAnfTTFPiguZUHmhiF -> $TO 12.500000000000000000 CS, inner id 7" "$log"
echo "OK"
