#!/usr/bin/env python3
"""Network identity in the diagnostic API: GetNodeInfo of every node (diag port 9400 + i).

Usage: diag_info.py <nodes> <expected_version> [--community 1,3,5] [--port-base 9400]

The community nodes (--community, by default all) must return networkUuid, the same non-zero value
on every one of them, and communityVersion equal to <expected_version> (the first line of the VERSION
file in their directory). The other nodes (BIN_ALT: the official node or an older build) are read
with the same definitions and reported: an official node simply leaves both fields out. A node that
does not answer (stopped by a scenario) is reported and skipped; at least half of the community
nodes, and at least one, must answer.
"""
import argparse
import os
import sys

import thriftpy2
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory

HERE = os.path.dirname(os.path.abspath(__file__))
IDL = os.path.abspath(os.path.join(HERE, "..", "..", "third-party", "thrift-interface-definitions"))
diag = thriftpy2.load(os.path.join(IDL, "apidiag.thrift"), module_name="apidiag_thrift", include_dirs=[IDL])


def node_info(port):
    client = make_client(diag.API_DIAG, "127.0.0.1", port, timeout=10000,
                         proto_factory=TBinaryProtocolFactory(), trans_factory=TBufferedTransportFactory())
    try:
        req = diag.NodeInfoRequest(session=True, state=False, grayListContent=False, blackListContent=False)
        return client.GetNodeInfo(req)
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("nodes", type=int)
    parser.add_argument("version")
    parser.add_argument("--community", default="")
    parser.add_argument("--port-base", type=int, default=9400)
    args = parser.parse_args()
    community = {int(x) for x in args.community.split(",") if x} or set(range(1, args.nodes + 1))

    ok = True
    uuids = {}
    answered = 0
    for i in range(1, args.nodes + 1):
        try:
            resp = node_info(args.port_base + i)
        except Exception as e:  # noqa: BLE001  (a stopped node: refused, reset, timeout)
            print(f"n{i}: no answer on diag port {args.port_base + i} ({type(e).__name__})")
            continue
        info = resp.info
        uuid = info.networkUuid
        unsigned = uuid & 0xFFFFFFFFFFFFFFFF if uuid is not None else None
        kind = "community" if i in community else "other build"
        print(f"n{i} ({kind}): version {info.version}, communityVersion {info.communityVersion!r}, "
              f"networkUuid {uuid} (unsigned {unsigned}), last block {info.session.lastBlock if info.session else '?'}")
        if i not in community:
            continue
        answered += 1
        if not uuid:
            print(f"FAIL: n{i} returned no networkUuid")
            ok = False
        else:
            uuids[i] = uuid
        if info.communityVersion != args.version:
            print(f"FAIL: n{i} returned communityVersion {info.communityVersion!r}, VERSION holds {args.version!r}")
            ok = False
    if answered < max(1, (len(community) + 1) // 2):
        print(f"FAIL: only {answered} of {len(community)} community nodes answered GetNodeInfo")
        ok = False
    if len(set(uuids.values())) > 1:
        print(f"FAIL: the nodes report different networkUuid values: {uuids}")
        ok = False
    if ok:
        value = next(iter(uuids.values()))
        print(f"diag network id: OK, {answered} community node(s) report networkUuid {value} "
              f"(unsigned {value & 0xFFFFFFFFFFFFFFFF}) and communityVersion {args.version}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
