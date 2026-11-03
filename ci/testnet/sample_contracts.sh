#!/usr/bin/env bash
# The contract examples of samples/contracts, deployed and used on the private test network with one
# of the sample clients of samples/clients, exactly as a user would type the commands.
#
# Usage: sample_contracts.sh <work_dir> <deployer-key-file> <client command...>
#   e.g. sample_contracts.sh "$WORK" "$WORK/n6/NodePrivate.txt" python3 samples/clients/python/credits_client.py
# Env:   NODE (default 127.0.0.1:9090), NODE_FLAG (default --node), LABEL (default: the client's name)
#        TOKEN_API=1: also check that the node's token API (TokensListGet, TokenInfoGet,
#        TokenBalancesGet, needs [api] tokens_cache=true) reports TestToken
#
# The deployer (a node's wallet, funded by fund.py) funds a second wallet, the user, derived from
# LABEL, and both of them then:
#   Counter    deploy, increment, add, setNote by the owner and by the user (refused), getters, a call
#              with an argument the method refuses, a CS transfer to it (payable() throws: refunded)
#   TestToken  deploy, getters, transfer, approve, allowance, transferFrom, a transfer above the
#              balance (refused), and with TOKEN_API=1 the token API
#   TipJar     two payments (CS transfers to the contract), paidBy/received/payments, a withdraw by
#              the user (refused) and by the owner (the contract sends CS)
#   Faucet     a refill, a claim by the user (paid from the contract's balance), a second claim
#              (refused), a claim by the deployer once the balance is below the minimum (refused)
# Every check prints ok or FAILED; the script exits 1 if any failed. Nothing here talks to anything
# but the private network's node.
set -uo pipefail
WORK=$1
KEY=$2
shift 2
CLIENT=("$@")
NODE=${NODE:-127.0.0.1:9090}
NODE_FLAG=${NODE_FLAG:---node}
LABEL=${LABEL:-$(basename "${CLIENT[-1]}")}
HERE="$(cd "$(dirname "$0")" && pwd)"
CONTRACTS="$HERE/../../samples/contracts"
FAILURES=0

c() { "${CLIENT[@]}" "$NODE_FLAG" "$NODE" "$@"; }
say() { echo "[$LABEL] $*"; }
ok() { say "$1: ok"; }
bad() { say "$1: FAILED${2:+ ($2)}"; FAILURES=$((FAILURES + 1)); }
# numeric equality of two decimals (12.5 == 12.50)
num_eq() { python3 -c "import sys; from decimal import Decimal as D; sys.exit(0 if D(sys.argv[1]) == D(sys.argv[2]) else 1)" "$1" "$2" 2>/dev/null; }
# a <= b
num_le() { python3 -c "import sys; from decimal import Decimal as D; sys.exit(0 if D(sys.argv[1]) <= D(sys.argv[2]) else 1)" "$1" "$2" 2>/dev/null; }

# call <label> <expected return or -> <client args...>: a transaction that must succeed
call() {
    local label=$1 want=$2 out got
    shift 2
    if ! out=$(c call "$@" 2>&1); then
        bad "$label" "$(echo "$out" | tail -1)"
        return 1
    fi
    got=$(sed -n 's/^returned: //p' <<<"$out")
    if [ "$want" = - ] || [ "$got" = "$want" ] || num_eq "$got" "$want"; then
        ok "$label -> $got"
    else
        bad "$label" "returned '$got', expected '$want'"
    fi
}
# refused <label> <client args...>: a transaction the contract must refuse (the node answers an error)
refused() {
    local label=$1 out
    shift
    if out=$(c call "$@" 2>&1); then
        bad "$label" "accepted: $(echo "$out" | tail -1)"
    else
        ok "$label refused ($(echo "$out" | tail -1))"
    fi
}
# get <label> <expected> <contract> <method> [args...]: a getter
get() {
    local label=$1 want=$2 got
    shift 2
    got=$(c get "$@" 2>&1)
    if [ "$got" = "$want" ] || num_eq "$got" "$want"; then ok "$label = $got"; else bad "$label" "got '$got', expected '$want'"; fi
}
balance() { c balance "$1" | awk '{print $2}'; }
# wait_for <seconds> <command...>: until the command succeeds
wait_for() {
    local deadline=$(( $(date +%s) + $1 ))
    shift
    until "$@"; do
        [ "$(date +%s)" -lt "$deadline" ] || return 1
        sleep 3
    done
}
balance_at_least() { num_le "$2" "$(balance "$1")"; }
getter_is() { [ "$(c get "$1" "$2" 2>/dev/null)" = "$3" ] || num_eq "$(c get "$1" "$2" 2>/dev/null)" "$3"; }
deploy() {  # deploy <Contract.java>: prints the address
    local out
    if ! out=$(c deploy "$KEY" "$CONTRACTS/$1" 2>&1); then
        say "$out"
        bad "deploy $1"
        return 1
    fi
    echo "$out" | sed 's/^/    /' >&2
    sed -n 's/^contract: //p' <<<"$out"
}

DEPLOYER=$(python3 -c "import sys, base58; print(base58.b58encode(base58.b58decode(open(sys.argv[1]).read().strip())[32:64]).decode())" "$KEY")
USER_KEY="$WORK/sample-user-$LABEL.txt"
python3 -c "import hashlib, sys, base58, nacl.signing
seed = hashlib.sha256(b'sample contracts user ' + sys.argv[1].encode()).digest()
print(base58.b58encode(seed + bytes(nacl.signing.SigningKey(seed).verify_key)).decode())" "$LABEL" > "$USER_KEY"
USER=$(python3 -c "import sys, base58; print(base58.b58encode(base58.b58decode(open(sys.argv[1]).read().strip())[32:]).decode())" "$USER_KEY")
say "deployer $DEPLOYER ($(balance "$DEPLOYER") CS), user $USER"

c transfer "$KEY" "$USER" 100 >/dev/null || bad "fund the user"
wait_for 180 balance_at_least "$USER" 100 && ok "user funded with 100 CS" || bad "user funded" "balance $(balance "$USER")"

# ---- Counter -------------------------------------------------------------------------------------
say "Counter"
if COUNTER=$(deploy Counter.java) && [ -n "$COUNTER" ]; then
    ok "deploy Counter $COUNTER"
    call "Counter.increment" 1 "$KEY" "$COUNTER" increment
    call "Counter.add 4" 5 "$KEY" "$COUNTER" add 4
    call "Counter.increment by the user" 6 "$USER_KEY" "$COUNTER" increment
    call "Counter.setNote by the owner" void "$KEY" "$COUNTER" setNote "hello from $LABEL"
    refused "Counter.setNote by the user" "$USER_KEY" "$COUNTER" setNote "not mine"
    refused "Counter.add 0 (the method throws)" "$KEY" "$COUNTER" add 0
    if c call "$KEY" "$COUNTER" add four >/dev/null 2>&1; then bad "add four not sent"; else ok "add four not sent (not an int)"; fi
    get "Counter.getValue" 6 "$COUNTER" getValue
    get "Counter.getNote" "hello from $LABEL" "$COUNTER" getNote
    get "Counter.getOwner" "$DEPLOYER" "$COUNTER" getOwner
    before=$(balance "$USER")
    c transfer "$USER_KEY" "$COUNTER" 2 >/dev/null || bad "transfer 2 CS to Counter"
    sleep 20
    after=$(balance "$USER")
    counter_balance=$(balance "$COUNTER")
    # payable() throws: the CS go back to the sender, who pays the fee only
    if num_eq "$counter_balance" 0 && num_le "$(python3 -c "print($before - 1)")" "$after"; then
        ok "2 CS to Counter refunded (contract $counter_balance CS, user $before -> $after CS)"
    else
        bad "2 CS to Counter refunded" "contract $counter_balance CS, user $before -> $after CS"
    fi
else
    bad "deploy Counter"
fi

# ---- TestToken -----------------------------------------------------------------------------------
say "TestToken (test only, never on mainnet)"
if TOKEN=$(deploy TestToken.java) && [ -n "$TOKEN" ]; then
    ok "deploy TestToken $TOKEN"
    get "CTT getName" "Community Test Token" "$TOKEN" getName
    get "CTT getSymbol" CTT "$TOKEN" getSymbol
    get "CTT getDecimal" 2 "$TOKEN" getDecimal
    get "CTT totalSupply" 1000000.00 "$TOKEN" totalSupply
    call "CTT transfer 1000 to the user" true "$KEY" "$TOKEN" transfer "$USER" 1000
    get "CTT balanceOf user" 1000.00 "$TOKEN" balanceOf "$USER"
    call "CTT approve user 250" void "$KEY" "$TOKEN" approve "$USER" 250
    get "CTT allowance deployer -> user" 250.00 "$TOKEN" allowance "$DEPLOYER" "$USER"
    call "CTT transferFrom deployer -> user 100 by the user" true "$USER_KEY" "$TOKEN" transferFrom "$DEPLOYER" "$USER" 100
    get "CTT allowance after transferFrom" 150.00 "$TOKEN" allowance "$DEPLOYER" "$USER"
    refused "CTT transferFrom above the allowance" "$USER_KEY" "$TOKEN" transferFrom "$DEPLOYER" "$USER" 151
    refused "CTT transfer above the balance" "$USER_KEY" "$TOKEN" transfer "$DEPLOYER" 5000
    refused "CTT transfer with 3 decimals" "$USER_KEY" "$TOKEN" transfer "$DEPLOYER" 0.001
    call "CTT transfer 0.5 back" true "$USER_KEY" "$TOKEN" transfer "$DEPLOYER" 0.5
    get "CTT balanceOf user" 1099.50 "$TOKEN" balanceOf "$USER"
    get "CTT balanceOf deployer" 998900.50 "$TOKEN" balanceOf "$DEPLOYER"
    if [ "${TOKEN_API:-0}" = 1 ]; then
        out=$(python3 "$HERE/sample_token_api.py" "$NODE" "$TOKEN" "$USER" 1099.50 "$DEPLOYER" 998900.50 2>&1)
        rc=$?
        echo "$out" | sed "s/^/[$LABEL]   /"
        [ $rc -eq 0 ] && ok "token API reports CTT" || bad "token API reports CTT"
    fi
else
    bad "deploy TestToken"
fi

# ---- TipJar --------------------------------------------------------------------------------------
say "TipJar"
if TIPJAR=$(deploy TipJar.java) && [ -n "$TIPJAR" ]; then
    ok "deploy TipJar $TIPJAR"
    c transfer "$USER_KEY" "$TIPJAR" 3 >/dev/null || bad "user pays 3 CS"
    wait_for 120 getter_is "$TIPJAR" getPayments 1 || bad "first payment recorded"
    c transfer "$KEY" "$TIPJAR" 2.5 >/dev/null || bad "deployer pays 2.5 CS"
    wait_for 120 getter_is "$TIPJAR" getPayments 2 || bad "second payment recorded"
    get "TipJar.getPayments" 2 "$TIPJAR" getPayments
    get "TipJar.paidBy user" 3 "$TIPJAR" paidBy "$USER"
    get "TipJar.paidBy deployer" 2.5 "$TIPJAR" paidBy "$DEPLOYER"
    get "TipJar.getReceived" 5.5 "$TIPJAR" getReceived
    get "TipJar balance" 5.5 "$TIPJAR" available
    num_eq "$(balance "$TIPJAR")" 5.5 && ok "TipJar holds 5.5 CS" || bad "TipJar holds 5.5 CS" "$(balance "$TIPJAR")"
    refused "TipJar.withdraw by the user" "$USER_KEY" "$TIPJAR" withdraw 1
    refused "TipJar.withdraw more than available" "$KEY" "$TIPJAR" withdraw 6
    before=$(balance "$DEPLOYER")
    call "TipJar.withdraw 4 by the owner" 1.5 "$KEY" "$TIPJAR" withdraw 4
    # the 4 CS the contract emits reach the owner once that transaction is in a block
    if wait_for 120 balance_at_least "$DEPLOYER" "$(python3 -c "print($before + 3)")"; then
        ok "owner received the withdrawal ($before -> $(balance "$DEPLOYER") CS); TipJar now $(balance "$TIPJAR") CS"
    else
        bad "owner received the withdrawal" "$before -> $(balance "$DEPLOYER") CS"
    fi
else
    bad "deploy TipJar"
fi

# ---- Faucet --------------------------------------------------------------------------------------
say "Faucet (reads its own balance)"
if FAUCET=$(deploy Faucet.java) && [ -n "$FAUCET" ]; then
    ok "deploy Faucet $FAUCET"
    c transfer "$KEY" "$FAUCET" 12 >/dev/null || bad "refill 12 CS"
    wait_for 120 balance_at_least "$FAUCET" 12 && ok "Faucet holds $(balance "$FAUCET") CS" || bad "Faucet refilled"
    get "Faucet.balance (getter)" 12 "$FAUCET" balance
    before=$(balance "$USER")
    call "Faucet.claim by the user" 1 "$USER_KEY" "$FAUCET" claim
    if wait_for 120 balance_at_least "$USER" "$(python3 -c "print($before + 4)")"; then
        ok "user received the gift ($before -> $(balance "$USER") CS)"
    else
        bad "user received the gift" "$before -> $(balance "$USER") CS"
    fi
    get "Faucet.getLastBalanceSeen" 12 "$FAUCET" getLastBalanceSeen
    get "Faucet.wasServed user" true "$FAUCET" wasServed "$USER"
    refused "Faucet.claim again by the user" "$USER_KEY" "$FAUCET" claim
    wait_for 60 getter_is "$FAUCET" getServed 1
    # 12 - 5 - the fee of the payout: below MIN_BALANCE (10), so the next claim is refused
    say "Faucet balance now $(balance "$FAUCET") CS"
    refused "Faucet.claim by the deployer (dry)" "$KEY" "$FAUCET" claim
    get "Faucet.getServed" 1 "$FAUCET" getServed
else
    bad "deploy Faucet"
fi

if [ "$FAILURES" -eq 0 ]; then
    say "every sample contract check passed"
else
    say "$FAILURES check(s) FAILED"
    exit 1
fi
