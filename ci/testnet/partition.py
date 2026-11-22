#!/usr/bin/env python3
"""The two chains of a partition (run.sh, OFFICIAL_PARTITION_AT) and the chain left after it.

    partition.py dump --from 395 --port 9090 > community.txt   # "seq hash transactions" per block
    partition.py report --community community.txt --official official.txt --port 9090 --port 9102
    partition.py fork --community community.txt --official official.txt   # the first block that differs

dump: the blocks of one node from a sequence to its head, taken just before the reconnection.
report: where the two sides forked, and which side's blocks the nodes hold after the reconnection
(each port given, every block of both sides compared); the side whose blocks are gone was unwound,
and the transactions of its blocks above the fork with them (unless written again on the other side).
"""
import argparse
import sys

from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory

from fund import api


def client_of(port):
    return make_client(api.API, host="127.0.0.1", port=int(port), proto_factory=TBinaryProtocolFactory(),
                       trans_factory=TBufferedTransportFactory(), timeout=60000)


def blocks(client, first, last):
    """{seq: (hash, transactions)} of the blocks first..last a node holds"""
    result = {}
    seq = first
    while seq <= last:
        count = min(100, last - seq + 1)
        pools = client.PoolListGetStable(seq + count - 1, count).pools or []
        for pool in pools:
            if first <= pool.poolNumber <= last:
                result[pool.poolNumber] = (bytes(pool.hash).hex(), pool.transactionsCount)
        seq += count
    return result


def head(client):
    return client.SyncStateGet().lastBlock


def read(path):
    result = {}
    with open(path) as f:
        for line in f:
            seq, h, txs = line.split()
            result[int(seq)] = (h, int(txs))
    return result


def dump(args):
    client = client_of(args.port)
    last = head(client)
    for seq, (h, txs) in sorted(blocks(client, args.first, last).items()):
        print(seq, h, txs)


def fork_of(community, official):
    """the first sequence at which the two sides hold different blocks"""
    common = sorted(set(community) & set(official))
    fork = next((s for s in common if community[s][0] != official[s][0]), None)
    if fork is None:
        # one side may only be shorter: the fork is right above the shorter head
        fork = min(max(community, default=0), max(official, default=0)) + 1
    return fork


def report(args):
    community = read(args.community)
    official = read(args.official)
    c_head = max(community) if community else 0
    o_head = max(official) if official else 0
    fork = fork_of(community, official)
    c_side = {s: v for s, v in community.items() if s >= fork}
    o_side = {s: v for s, v in official.items() if s >= fork}
    print(f"PARTITION RESULT: fork at #{fork}; before the reconnection the community side held #{fork}..#{c_head} "
          f"({len(c_side)} blocks, {sum(t for _, t in c_side.values())} transactions), the official side #{fork}..#{o_head} "
          f"({len(o_side)} blocks, {sum(t for _, t in o_side.values())} transactions)")
    failed = False
    verdicts = set()
    for port in args.port:
        client = client_of(port)
        last = max(c_head, o_head)
        now = blocks(client, fork, last)
        kept_c = sum(1 for s, v in c_side.items() if now.get(s, (None,))[0] == v[0])
        kept_o = sum(1 for s, v in o_side.items() if now.get(s, (None,))[0] == v[0])
        lost_c_txs = sum(v[1] for s, v in c_side.items() if now.get(s, (None,))[0] != v[0])
        lost_o_txs = sum(v[1] for s, v in o_side.items() if now.get(s, (None,))[0] != v[0])
        if not o_side and c_side and kept_c == len(c_side):
            # the official side wrote nothing while cut off (it stalled): no fork, one chain
            verdict = f"no fork: the official side wrote no block, the community blocks were kept ({len(c_side)} blocks)"
        elif not c_side and o_side and kept_o == len(o_side):
            verdict = f"no fork: the community side wrote no block, the official blocks were kept ({len(o_side)} blocks)"
        elif c_side and kept_c == 0 and kept_o == len(o_side):
            verdict = f"the community side was unwound ({len(c_side)} blocks, {lost_c_txs} transactions reverted)"
        elif o_side and kept_o == 0 and kept_c == len(c_side):
            verdict = f"the official side was unwound ({len(o_side)} blocks, {lost_o_txs} transactions reverted)"
        else:
            verdict = f"mixed: {kept_c}/{len(c_side)} community and {kept_o}/{len(o_side)} official blocks kept"
            failed = True
        verdicts.add(verdict)
        print(f"PARTITION RESULT: port {port} (head #{head(client)}): {verdict}")
    if len(verdicts) > 1:
        print("PARTITION RESULT: the nodes compared did not keep the same side")
        failed = True
    sys.exit(1 if failed else 0)


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    d = sub.add_parser("dump")
    d.add_argument("--from", dest="first", type=int, required=True)
    d.add_argument("--port", required=True)
    r = sub.add_parser("report")
    r.add_argument("--community", required=True)
    r.add_argument("--official", required=True)
    r.add_argument("--port", action="append", required=True)
    f = sub.add_parser("fork")
    f.add_argument("--community", required=True)
    f.add_argument("--official", required=True)
    args = parser.parse_args()
    if args.command == "dump":
        dump(args)
    elif args.command == "fork":
        print(fork_of(read(args.community), read(args.official)))
    else:
        report(args)


if __name__ == "__main__":
    main()
