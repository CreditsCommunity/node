#!/usr/bin/env python3
"""A small client of the Credits node API (Thrift, binary protocol, buffered transport).

  credits_client.py [--node HOST:PORT] status
  credits_client.py [--node HOST:PORT] balance <address>
  credits_client.py [--node HOST:PORT] blocks [n]
  credits_client.py [--node HOST:PORT] tx <address> [n]
  credits_client.py [--node HOST:PORT] transfer <key-file> <to-address> <amount>
  credits_client.py sign <key-file> <to-address> <amount> <inner-id>   (offline: prints, sends nothing)
  credits_client.py [--node HOST:PORT] deploy <key-file> <Contract.java>
  credits_client.py [--node HOST:PORT] call <key-file> <contract> <method> [args...]
  credits_client.py [--node HOST:PORT] get <contract> <method> [args...]   (a getter: free, nothing is sent)
  credits_client.py [--node HOST:PORT] methods <contract>

Addresses are base58 public keys. The key file holds a base58 ed25519 secret key, as the node's
NodePrivate.txt does (64 bytes: seed + public key; a bare 32-byte seed is accepted too).

transfer, deploy and call SIGN AND SEND REAL TRANSACTIONS: on mainnet they spend real CS. Try them
on a private or test network first.

Contract arguments are given as text and converted to the type the method declares (the node
refuses a call whose Variants do not match): String, BigDecimal, int, long, boolean, double and
their wrappers, byte[] as hex. With overloads, the method with as many arguments as given is used.

The Thrift definitions are read at run time by thriftpy2 from the node's repository
(third-party/thrift-interface-definitions), or from --idl / $CREDITS_IDL.
"""
import argparse
import datetime
import hashlib
import math
import os
import struct
import sys
import time
from decimal import Decimal

import base58
import nacl.signing
import thriftpy2
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory
from thriftpy2.utils import serialize

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_IDL = os.path.join(HERE, "..", "..", "..", "third-party", "thrift-interface-definitions")
CURRENCY_CS = 1
FRACTION = 10 ** 18  # general.Amount: integral + fraction / 10^18


def encode_max_fee(value):
    """csdb::AmountCommission(double) as its 16-bit pattern (5-bit exponent, 10-bit mantissa)."""
    v = abs(value)
    expf = math.log10(v)
    expi = int(expf + 0.5) if expf >= 0 else int(expf - 0.5)
    v /= 10 ** expi
    if v >= 1.0:
        v *= 0.1
        expi += 1
    return (((expi + 18) & 0x1F) << 10) | (int(round(v * 1024)) & 0x3FF)


MAX_FEE = encode_max_fee(1.0)  # the most the sender accepts to pay; the node charges the actual fee
CONTRACT_MAX_FEE = encode_max_fee(10.0)  # contracts cost more: a deploy, or a call that runs long


def fmt_amount(a):
    return format((Decimal(a.integral) * FRACTION + a.fraction) / FRACTION, "f")


def parse_amount(text):
    """'12.5' -> (12, 500000000000000000), exactly."""
    units = int(Decimal(text) * FRACTION)
    if units <= 0:
        raise SystemExit("the amount must be positive")
    return units // FRACTION, units % FRACTION


def fmt_time(ms):
    return datetime.datetime.fromtimestamp(ms / 1000, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")


def address(text):
    key = base58.b58decode(text)
    if len(key) != 32:
        raise SystemExit(f"{text}: not a base58 public key (32 bytes)")
    return key


def read_key(path):
    secret = base58.b58decode(open(path).read().strip())
    if len(secret) not in (32, 64):
        raise SystemExit(f"{path}: expected a base58 ed25519 secret key of 64 bytes (or a 32-byte seed)")
    key = nacl.signing.SigningKey(secret[:32])
    if len(secret) == 64 and bytes(key.verify_key) != secret[32:]:
        raise SystemExit(f"{path}: the public half does not match the seed")
    return key


def load_idl(idl):
    idl = os.path.abspath(idl)
    return thriftpy2.load(os.path.join(idl, "api.thrift"), module_name="api_thrift", include_dirs=[idl])


class Node:
    def __init__(self, endpoint, idl):
        host, port = endpoint.rsplit(":", 1)
        self.api = load_idl(idl)
        self.general = self.api.general  # loaded through the include
        self.client = make_client(self.api.API, host=host, port=int(port), proto_factory=TBinaryProtocolFactory(),
                                  trans_factory=TBufferedTransportFactory(), timeout=120000)

    def __getattr__(self, name):
        method = getattr(self.client, name)

        def call(*args):
            result = method(*args)
            status = getattr(result, "status", None)
            if status is not None and status.code != 0:
                raise SystemExit(f"{name}: {status.message} (code {status.code})")
            return result
        return call


def status(node, _args):
    s = node.SyncStateGet()
    head = node.PoolListGet(0, 1).pools[0]
    wallets = node.WalletsGet(0, 1, 2, True)  # one wallet, by transaction count: the cheapest way to the total
    print(f"last block   : {s.lastBlock}")
    print(f"current round: {s.currRound}")
    print(f"head hash    : {head.hash.hex()}")
    print(f"head time    : {fmt_time(head.time)}")
    print(f"wallets      : {wallets.count}")


def balance(node, args):
    b = node.WalletBalanceGet(address(args.address))
    print(f"{args.address}: {fmt_amount(b.balance)} CS")


def blocks(node, args):
    for p in node.PoolListGet(0, args.n).pools:
        print(f"{p.poolNumber:>11}  {p.hash.hex()}  {p.transactionsCount:>4} tx  {fmt_time(p.time)}")


def tx(node, args):
    r = node.TransactionsGet(address(args.address), 0, args.n)
    print(f"{r.total_trxns_count} transactions, the last {len(r.transactions)}:")
    for t in r.transactions:
        x = t.trxn
        print(f"{t.id.poolSeq:>11}.{t.id.index:<4} {fmt_time(x.timeCreation)}  "
              f"{base58.b58encode(x.source).decode()} -> {base58.b58encode(x.target).decode()}  "
              f"{fmt_amount(x.amount)} CS  type {x.type}")


def build_transfer(api, general, key, to, amount, inner_id):
    """A signed CS transfer, and the bytes its signature covers:
    csdb::Transaction::to_byte_stream_for_sig(), little endian."""
    source = bytes(key.verify_key)
    target = address(to)
    integral, fraction = parse_amount(amount)
    signed = b"".join([
        inner_id.to_bytes(6, "little"),     # inner id, 6 bytes (source and target given as public keys)
        source,                             # 32 bytes
        target,                             # 32 bytes
        struct.pack("<iQ", integral, fraction),
        struct.pack("<H", MAX_FEE),         # max fee
        struct.pack("<B", CURRENCY_CS),     # currency
        struct.pack("<B", 0),               # number of user fields
    ])
    t = api.Transaction(
        id=inner_id, source=source, target=target,
        amount=general.Amount(integral=integral, fraction=fraction),
        balance=general.Amount(integral=0, fraction=0),
        currency=CURRENCY_CS,
        signature=key.sign(signed).signature,
        fee=api.AmountCommission(commission=MAX_FEE),
        timeCreation=int(time.time() * 1000),
        userFields=b"",
        type=api.TransactionType.TT_Transfer,
        poolNumber=0)
    return t, signed


def sign(args):
    """What transfer would send, built offline: no node is contacted."""
    api = load_idl(args.idl)
    t, signed = build_transfer(api, api.general, read_key(args.key_file), args.to, args.amount, args.inner_id)
    print(f"source   : {base58.b58encode(t.source).decode()}")
    print(f"signed   : {signed.hex()}")
    print(f"signature: {t.signature.hex()}")


def transfer(node, args):
    key = read_key(args.key_file)
    # the inner id must be the sender's last one + 1 (replay protection); a wallet the node has
    # never seen answers "not found": its first transaction has inner id 1
    last = node.client.WalletTransactionsCountGet(bytes(key.verify_key))
    inner_id = ((last.lastTransactionInnerId or 0) if last.status.code == 0 else 0) + 1
    t, _ = build_transfer(node.api, node.general, key, args.to, args.amount, inner_id)
    r = node.TransactionFlow(t)
    print(f"sent: inner id {inner_id}, round {r.roundNum}, fee {fmt_amount(r.fee)} CS")


# ---- smart contracts ------------------------------------------------------------------------------

def next_inner_id(node, pk):
    """The sender's last inner id + 1 (replay protection); a wallet the node has never seen: 1."""
    last = node.client.WalletTransactionsCountGet(pk)
    return ((last.lastTransactionInnerId or 0) if last.status.code == 0 else 0) + 1


def contract_transaction(api, general, key, target, sci, tx_type, inner_id):
    """A signed contract transaction (deploy or call). The invocation travels as the transaction's
    one user field: its Thrift binary serialization, which the signature covers. Every field the
    IDL does not mark optional is set, so these are the bytes the node serializes again to check."""
    source = bytes(key.verify_key)
    code = serialize(sci, TBinaryProtocolFactory())
    signed = b"".join([
        inner_id.to_bytes(6, "little"), source, target,
        struct.pack("<iQ", 0, 0),                   # amount: none
        struct.pack("<H", CONTRACT_MAX_FEE),
        struct.pack("<B", CURRENCY_CS),
        struct.pack("<BI", 1, len(code)) + code,    # one user field: u32 size + bytes
    ])
    return api.Transaction(
        id=inner_id, source=source, target=target,
        amount=general.Amount(integral=0, fraction=0), balance=general.Amount(integral=0, fraction=0),
        currency=CURRENCY_CS, signature=key.sign(signed).signature, smartContract=sci,
        fee=api.AmountCommission(commission=CONTRACT_MAX_FEE), timeCreation=int(time.time() * 1000), type=tx_type)


def invocation(api, method="", params=(), deploy=None, getter=False):
    return api.SmartContractInvocation(method=method, params=list(params), usedContracts=[], forgetNewState=getter,
                                       smartContractDeploy=deploy, version=1)


def variant(general, declared, text):
    """The Variant the executor matches with a declared Java type."""
    flag = lambda t: {"true": True, "false": False}[t.lower()]  # noqa: E731
    converters = {
        "java.lang.String": lambda t: general.Variant(v_string=t),
        "java.math.BigDecimal": lambda t: general.Variant(v_big_decimal=str(Decimal(t))),
        "int": lambda t: general.Variant(v_int=int(t)),
        "java.lang.Integer": lambda t: general.Variant(v_int_box=int(t)),
        "long": lambda t: general.Variant(v_long=int(t)),
        "java.lang.Long": lambda t: general.Variant(v_long_box=int(t)),
        "boolean": lambda t: general.Variant(v_boolean=flag(t)),
        "java.lang.Boolean": lambda t: general.Variant(v_boolean_box=flag(t)),
        "double": lambda t: general.Variant(v_double=float(t)),
        "java.lang.Double": lambda t: general.Variant(v_double_box=float(t)),
        "byte[]": lambda t: general.Variant(v_byte_array=bytes.fromhex(t)),
    }
    if declared not in converters:
        raise SystemExit(f"arguments of type {declared} are not supported by this client")
    try:
        return converters[declared](text)
    except (ValueError, KeyError, ArithmeticError):
        raise SystemExit(f"{text!r} is not a {declared}")


def show(v):
    """A returned Variant as text, the same way in every sample."""
    if v is None:
        return "(nothing)"
    for name in ("v_boolean", "v_boolean_box"):
        if getattr(v, name) is not None:
            return "true" if getattr(v, name) else "false"
    for name in ("v_string", "v_big_decimal", "v_byte", "v_byte_box", "v_short", "v_short_box", "v_int", "v_int_box",
                 "v_long", "v_long_box", "v_float", "v_float_box", "v_double", "v_double_box"):
        if getattr(v, name) is not None:
            return str(getattr(v, name))
    if v.v_byte_array is not None:
        return v.v_byte_array.hex()
    if v.v_void is not None:
        return "void"
    if v.v_null is not None:
        return "null"
    return repr(v)


def contract_methods(node, contract):
    """The public methods of a deployed contract, as the node's executor lists them from its byte code."""
    deployed = node.SmartContractGet(contract)
    r = node.client.ContractAllMethodsGet(deployed.smartContract.smartContractDeploy.byteCodeObjects)
    if r.code != 0:
        raise SystemExit(f"ContractAllMethodsGet: {r.message}")
    return r.methods


def typed_params(node, contract, method, texts):
    candidates = [m for m in contract_methods(node, contract) if m.name == method]
    match = [m for m in candidates if len(m.arguments) == len(texts)]
    if not match:
        known = ", ".join(f"{m.name}({', '.join(a.type for a in m.arguments)})" for m in candidates)
        raise SystemExit(f"no method {method} with {len(texts)} arguments" + (f"; there is {known}" if known else ""))
    return [variant(node.general, a.type, t) for a, t in zip(match[0].arguments, texts)]


def methods(node, args):
    for m in sorted(contract_methods(node, address(args.contract)), key=lambda m: m.name):
        print(f"{m.returnType} {m.name}({', '.join(a.type + ' ' + a.name for a in m.arguments)})")


def deploy(node, args):
    key = read_key(args.key_file)
    with open(args.source) as f:
        source = f.read()
    # the node's executor compiles the source and checks it against its rules
    compiled = node.SmartContractCompile(source)
    classes = compiled.byteCodeObjects
    print(f"compiled: {len(classes)} class(es), token standard {compiled.tokenStandard}")
    inner_id = next_inner_id(node, bytes(key.verify_key))
    # the address of the new contract (SmartContracts::get_valid_smart_address):
    # blake2s-256(deployer public key, 6-byte inner id, the byte code of every class in order)
    target = hashlib.blake2s(bytes(key.verify_key) + inner_id.to_bytes(6, "little")
                             + b"".join(c.byteCode for c in classes)).digest()
    sci = invocation(node.api, deploy=node.api.SmartContractDeploy(
        sourceCode=source, byteCodeObjects=classes, hashState="", tokenStandard=compiled.tokenStandard, lang=0))
    t = contract_transaction(node.api, node.general, key, target, sci, node.api.TransactionType.TT_ContractDeploy, inner_id)
    r = node.TransactionFlow(t)
    print(f"sent: inner id {inner_id}, round {r.roundNum}, fee {fmt_amount(r.fee)} CS")
    # the contract exists once the deploy is in a block and its executions are done
    deadline = time.time() + 180
    while node.client.SmartContractGet(target).status.code != 0:
        if time.time() > deadline:
            raise SystemExit(f"{base58.b58encode(target).decode()} is not deployed after 180 s")
        time.sleep(2)
    print(f"contract: {base58.b58encode(target).decode()}")


def call(node, args):
    key = read_key(args.key_file)
    contract = address(args.contract)
    params = typed_params(node, contract, args.method, args.args)
    inner_id = next_inner_id(node, bytes(key.verify_key))
    sci = invocation(node.api, args.method, params)
    t = contract_transaction(node.api, node.general, key, contract, sci, node.api.TransactionType.TT_ContractCall, inner_id)
    # the node answers once the confidants executed the call: the result is the method's return value
    r = node.TransactionFlow(t)
    print(f"sent: inner id {inner_id}, round {r.roundNum}, fee {fmt_amount(r.fee)} CS")
    print(f"returned: {show(r.smart_contract_result)}")


def get(node, args):
    contract = address(args.contract)
    params = typed_params(node, contract, args.method, args.args)
    # a getter: TransactionFlow with forgetNewState runs the method on the node's own executor against
    # the current state and throws the new state away; nothing is sent, no fee, no signature is checked
    t = node.api.Transaction(
        id=0, source=bytes(32), target=contract,
        amount=node.general.Amount(integral=0, fraction=0), balance=node.general.Amount(integral=0, fraction=0),
        currency=CURRENCY_CS, signature=bytes(64), smartContract=invocation(node.api, args.method, params, getter=True),
        fee=node.api.AmountCommission(commission=CONTRACT_MAX_FEE), timeCreation=int(time.time() * 1000),
        type=node.api.TransactionType.TT_ContractCall)
    print(show(node.TransactionFlow(t).smart_contract_result))


def main():
    parser = argparse.ArgumentParser(description="Credits node API client (sample)")
    parser.add_argument("--node", default="127.0.0.1:9090", help="API endpoint, HOST:PORT (default %(default)s)")
    parser.add_argument("--idl", default=os.environ.get("CREDITS_IDL", DEFAULT_IDL), help="Thrift IDL directory")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status").set_defaults(run=status)
    p = sub.add_parser("balance")
    p.add_argument("address")
    p.set_defaults(run=balance)
    p = sub.add_parser("blocks")
    p.add_argument("n", nargs="?", type=int, default=10)
    p.set_defaults(run=blocks)
    p = sub.add_parser("tx")
    p.add_argument("address")
    p.add_argument("n", nargs="?", type=int, default=10)
    p.set_defaults(run=tx)
    p = sub.add_parser("transfer", help="spends real CS on mainnet")
    p.add_argument("key_file")
    p.add_argument("to")
    p.add_argument("amount")
    p.set_defaults(run=transfer)
    p = sub.add_parser("sign", help="build and sign a transfer with the given inner id, print it, send nothing")
    p.add_argument("key_file")
    p.add_argument("to")
    p.add_argument("amount")
    p.add_argument("inner_id", type=int)
    p.set_defaults(run=None)
    p = sub.add_parser("deploy", help="compile and deploy a Java contract: spends real CS on mainnet")
    p.add_argument("key_file")
    p.add_argument("source")
    p.set_defaults(run=deploy)
    p = sub.add_parser("call", help="call a contract method in a transaction: spends real CS on mainnet")
    p.add_argument("key_file")
    p.add_argument("contract")
    p.add_argument("method")
    p.add_argument("args", nargs="*")
    p.set_defaults(run=call)
    p = sub.add_parser("get", help="run a contract method as a getter: free, changes nothing")
    p.add_argument("contract")
    p.add_argument("method")
    p.add_argument("args", nargs="*")
    p.set_defaults(run=get)
    p = sub.add_parser("methods", help="list a contract's methods and their argument types")
    p.add_argument("contract")
    p.set_defaults(run=methods)
    args = parser.parse_args()
    if args.run is None:
        sign(args)
    else:
        args.run(Node(args.node, args.idl), args)


if __name__ == "__main__":
    main()
