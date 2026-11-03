#!/usr/bin/env python3
"""Compare the token cache of other nodes with n1's (a full node with the cache on from block 0).

Usage: tokens_pruned.py <label> <ref host:port> <host:port> [<host:port> ...]

For every token n1 lists: does the node list the token, which holders it knows, the balance it gives
each holder (TokenHoldersGet), and whether TokenBalancesGet(holder) lists the token for each of n1's
holders with a balance. Report only: prints one line per difference and a summary line per node,
"TOKENS-PRUNED <label> <node>: complete" or "... INCOMPLETE ...". Never fails.
"""
import os
import sys
from decimal import Decimal, InvalidOperation

import base58
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fund import api  # noqa: E402


def b58(b):
    return base58.b58encode(b).decode()


def dec(s):
    try:
        return Decimal(s or "0")
    except InvalidOperation:
        return None


def client(hp):
    host, port = hp.rsplit(":", 1)
    return make_client(api.API, host=host, port=int(port), proto_factory=TBinaryProtocolFactory(),
                       trans_factory=TBufferedTransportFactory(), timeout=60000)


def snapshot(c):
    listed = c.TokensListGet(0, 1000, 0, False, api.TokenFilters(name="", code="", tokenStandard=0))
    if listed.status.code != 0:
        return None, listed.status.message
    tokens = {}
    for t in listed.tokens or []:
        h = c.TokenHoldersGet(t.address, 0, 1000, 0, True)
        tokens[t.address] = (t.code, {x.holder: x.balance for x in (h.holders or [])})
    return tokens, ""


def main():
    label, ref = sys.argv[1], sys.argv[2]
    want, why = snapshot(client(ref))
    if want is None:
        print(f"TOKENS-PRUNED {label}: reference {ref} has no token list ({why})", flush=True)
        return
    print(f"TOKENS-PRUNED {label}: reference {ref}: " + "; ".join(
        f"{code} {b58(a)[:8]} {len(hs)} holders ({sum(1 for v in hs.values() if dec(v))} with a balance)"
        for a, (code, hs) in want.items()), flush=True)
    for hp in sys.argv[3:]:
        c = client(hp)
        have, why = snapshot(c)
        if have is None:
            print(f"TOKENS-PRUNED {label} {hp}: no token list ({why})", flush=True)
            continue
        diffs = []
        for a, (code, hs) in want.items():
            if a not in have:
                diffs.append(f"token {code} {b58(a)[:8]} not listed")
                continue
            got = have[a][1]
            for holder, bal in hs.items():
                if holder not in got:
                    diffs.append(f"{code}: holder {b58(holder)[:8]} (balance {bal}) unknown")
                elif dec(got[holder]) != dec(bal):
                    diffs.append(f"{code}: holder {b58(holder)[:8]} balance {got[holder]}, reference {bal}")
        # TokenBalancesGet, as a wallet asks it, for every holder with a balance on the reference
        holders = {h for _, hs in want.values() for h, v in hs.items() if dec(v)}
        for holder in sorted(holders):
            tb = c.TokenBalancesGet(holder)
            got = {b.token: b.balance for b in (tb.balances or [])}
            for a, (code, hs) in want.items():
                if dec(hs.get(holder)):
                    if a not in got:
                        diffs.append(f"TokenBalancesGet {b58(holder)[:8]}: {code} missing (reference {hs[holder]})")
                    elif dec(got[a]) != dec(hs[holder]):
                        diffs.append(f"TokenBalancesGet {b58(holder)[:8]}: {code} {got[a]}, reference {hs[holder]}")
        for d in diffs:
            print(f"TOKENS-PRUNED {label} {hp}: {d}", flush=True)
        known = sum(len(set(have.get(a, ("", {}))[1]) & set(hs)) for a, (_, hs) in want.items())
        total = sum(len(hs) for _, hs in want.values())
        print(f"TOKENS-PRUNED {label} {hp}: {'complete' if not diffs else 'INCOMPLETE'}: "
              f"{sum(1 for a in want if a in have)}/{len(want)} tokens, {known}/{total} holders, "
              f"{len(diffs)} differences", flush=True)


if __name__ == "__main__":
    main()
