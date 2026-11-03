#!/usr/bin/env python3
"""Check that the node's token API reports the sample TestToken (CTT) after sample_contracts.sh used it.

Usage: sample_token_api.py <host:port> <token> <holder> <balance> [<holder> <balance> ...]

The node must run with [api] tokens_cache=true. Within 180 s TokenInfoGet must report the token
(code CTT, name "Community Test Token", standard 4, supply 1'000'000, at least two holders),
TokensListGet must list it, and TokenBalancesGet must give each holder the balance the contract holds.
"""
import os
import sys
import time
from decimal import Decimal

import base58
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fund import api  # noqa: E402  (the Thrift definitions, loaded from the submodule)


def check(c, token, expected):
    problems = []
    info = c.TokenInfoGet(token)
    if info.status.code != 0:
        return [f"TokenInfoGet: {info.status.message}"]
    t = info.token
    print(f"TokenInfoGet: {t.code} {t.name!r} standard {t.tokenStandard} supply {t.totalSupply} "
          f"holders {t.holdersCount} transfers {t.transfersCount} transactions {t.transactionsCount}", flush=True)
    if (t.code, t.name, t.tokenStandard) != ("CTT", "Community Test Token", 4):
        problems.append("code, name or standard")
    if not t.totalSupply or Decimal(t.totalSupply) != Decimal("1000000"):
        problems.append(f"total supply {t.totalSupply}")
    if t.holdersCount < 2:
        problems.append(f"holders {t.holdersCount}")
    listed = c.TokensListGet(0, 100, 0, False, api.TokenFilters(name="", code="", tokenStandard=0))
    if token not in [x.address for x in (listed.tokens or [])]:
        problems.append(f"TokensListGet does not list it ({listed.status.message!r})")
    for holder, want in expected:
        balances = c.TokenBalancesGet(holder)
        got = {b.token: b.balance for b in (balances.balances or [])}.get(token)
        print(f"TokenBalancesGet {base58.b58encode(holder).decode()[:8]}: {got}", flush=True)
        if got is None or Decimal(got) != Decimal(want):
            problems.append(f"balance of {base58.b58encode(holder).decode()[:8]}: {got}, expected {want}")
    return problems


def main():
    host, port = sys.argv[1].rsplit(":", 1)
    token = base58.b58decode(sys.argv[2])
    rest = sys.argv[3:]
    expected = [(base58.b58decode(rest[i]), rest[i + 1]) for i in range(0, len(rest), 2)]
    c = make_client(api.API, host=host, port=int(port), proto_factory=TBinaryProtocolFactory(),
                    trans_factory=TBufferedTransportFactory(), timeout=60000)
    deadline = time.time() + 180
    while True:
        problems = check(c, token, expected)
        if not problems:
            print("token API: ok", flush=True)
            return
        if time.time() >= deadline:
            print("token API: FAILED: " + "; ".join(problems), flush=True)
            sys.exit(1)
        print("token API: not yet (" + "; ".join(problems) + "), again in 10 s", flush=True)
        time.sleep(10)


if __name__ == "__main__":
    main()
