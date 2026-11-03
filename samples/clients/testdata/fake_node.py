#!/usr/bin/env python3
"""A stand-in for a node's API that checks the transfers the sample clients send, offline.

  fake_node.py [--port 19090] [--idl DIR]

It answers WalletTransactionsCountGet (last inner id 6, so the next transfer has inner id 7) and
TransactionFlow, which accepts a transfer only when its fields are those of a CS transfer and its
ed25519 signature is valid over csdb::Transaction::to_byte_stream_for_sig() rebuilt from the
fields the client sent: what a real node checks first. Every transfer is printed. Nothing is
forwarded anywhere. Used by .github/workflows/samples.yml; run it to try `transfer` safely.
"""
import argparse
import os
import struct
import time

import base58
import nacl.exceptions
import nacl.signing
import thriftpy2
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_server
from thriftpy2.transport import TBufferedTransportFactory

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_IDL = os.path.join(HERE, "..", "..", "..", "third-party", "thrift-interface-definitions")
LAST_INNER_ID = 6


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=19090)
    parser.add_argument("--idl", default=DEFAULT_IDL)
    args = parser.parse_args()
    idl = os.path.abspath(args.idl)
    api = thriftpy2.load(os.path.join(idl, "api.thrift"), module_name="api_thrift", include_dirs=[idl])
    general = api.general

    def ok():
        return general.APIResponse(code=0, message="")

    def refuse(why):
        print(f"REFUSED: {why}", flush=True)
        return api.TransactionFlowResult(status=general.APIResponse(code=1, message=why))

    class Handler:
        def WalletTransactionsCountGet(self, address):
            return api.WalletTransactionsCountGetResult(status=ok(), lastTransactionInnerId=LAST_INNER_ID)

        def TransactionFlow(self, t):
            if t.type != api.TransactionType.TT_Transfer or t.currency != 1 or t.smartContract is not None:
                return refuse(f"not a CS transfer: type {t.type}, currency {t.currency}")
            if t.id != LAST_INNER_ID + 1:
                return refuse(f"inner id {t.id}, expected {LAST_INNER_ID + 1}")
            if t.userFields:
                return refuse("unexpected user fields")
            if t.balance is None or t.fee is None or abs(t.timeCreation - time.time() * 1000) > 600_000:
                return refuse("balance, fee or creation time missing or wrong")
            if not 0 <= t.amount.fraction < 10**18:
                return refuse(f"amount fraction {t.amount.fraction} out of range")
            signed = b"".join([
                t.id.to_bytes(6, "little"), t.source, t.target,
                struct.pack("<iQ", t.amount.integral, t.amount.fraction),
                struct.pack("<H", t.fee.commission & 0xFFFF), struct.pack("<B", t.currency), b"\x00",
            ])
            try:
                nacl.signing.VerifyKey(t.source).verify(signed, t.signature)
            except (nacl.exceptions.BadSignatureError, ValueError) as e:
                return refuse(f"bad signature: {e}")
            print(f"ACCEPTED: {base58.b58encode(t.source).decode()} -> {base58.b58encode(t.target).decode()} "
                  f"{t.amount.integral}.{t.amount.fraction:018d} CS, inner id {t.id}, max fee {t.fee.commission}",
                  flush=True)
            return api.TransactionFlowResult(status=ok(), roundNum=1, id=api.TransactionId(poolSeq=1, index=0),
                                             fee=general.Amount(integral=0, fraction=8_740_000_000_000_000))

    server = make_server(api.API, Handler(), host="127.0.0.1", port=args.port, proto_factory=TBinaryProtocolFactory(),
                         trans_factory=TBufferedTransportFactory(), client_timeout=None)
    print(f"fake node listening on 127.0.0.1:{args.port}", flush=True)
    server.serve()


if __name__ == "__main__":
    main()
