#!/usr/bin/env bash
# Offline checks of one sample client, without any real node:
#   check.sh <client command...>      e.g. check.sh ./go/credits-client
# 1. `sign` prints the bytes and signature of sign-expected.txt;
# 2. `transfer` to fake_node.py is accepted (well-formed transfer, valid signature);
# 3. with CONTRACTS=1, `deploy`, `call` and `get` against the fake node's made-up contract: the
#    contract address, the signature over the invocation and the declared argument types.
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

if [ "${CONTRACTS:-0}" = 1 ]; then
    run() { "${CLIENT[@]}" "$NODE_FLAG" "127.0.0.1:$PORT" "$@"; }
    CLIENT=("$@")
    expect() {  # expect <expected output> <command args...>
        local want=$1; shift
        local got
        got="$(run "$@")"
        echo "$got"
        [ "$got" = "$want" ] || { echo "FAIL: $* printed the above, expected:"; echo "$want"; cat "$log"; exit 1; }
    }
    CONTRACT=TG1GULQfynpUv8CBewc631R9yrgn2NhFQmvw6p9MUUz
    echo "== deploy to the fake node"
    expect "compiled: 1 class(es), token standard 0
sent: inner id 7, round 1, fee 0.00874 CS
contract: $CONTRACT" deploy "$KEY" "$HERE/../../contracts/Counter.java"
    echo "== call and get"
    expect "sent: inner id 7, round 1, fee 0.00874 CS
returned: 5" call "$KEY" "$CONTRACT" add 5
    expect "sent: inner id 7, round 1, fee 0.00874 CS
returned: void" call "$KEY" "$CONTRACT" setNote "hello, contract"
    expect "12.50" get "$CONTRACT" paidBy "$TO"
    expect "true" get "$CONTRACT" isOpen
    if run call "$KEY" "$CONTRACT" add five 2>/dev/null; then echo "FAIL: add five was sent"; exit 1; fi
    grep -q "^ACCEPTED: call setNote(v_string='hello, contract')" "$log"
    cat "$log"
fi
echo "OK"
