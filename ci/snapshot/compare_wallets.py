#!/usr/bin/env python3
"""Compares the balance of every wallet of a node with official mainnet nodes.

  compare_wallets.py --ours HOST:PORT [--official NAME=HOST:PORT ...] [--baseline FILE]
                     [--scan-official N] [--min-official N] [--summary FILE] [--json FILE]
                     [--self-test-mismatch]

Python standard library only: a small Thrift client (binary protocol, buffered transport, as the
node's API on port 9090) is built in, so it runs anywhere python3 does.

1. Reads SyncStateGet from our node and from the official nodes (built-in list, or --official); keeps
   those within --max-lag blocks of the highest head. Fewer than --min-official: exit 3.
2. Reads every wallet (WalletsGet, 100 per page, ordered by transaction count, overlapping pages)
   from our node and from the first --scan-official official nodes, in parallel.
3. A wallet whose balance is not the same on all of them, a wallet missing from one of the scans and
   every wallet of the baseline is read again with WalletBalanceGet on our node and on every
   reachable official node, in rounds --recheck-sec apart, until two rounds in a row give the same
   values (the reads are not at one height: a wallet that moved between them settles once it stops
   moving; one still moving after --rounds rounds is listed as active and not judged).
4. For every settled wallet the official majority is the value more than half of the official nodes
   hold. A wallet where the official nodes differ among themselves is recorded as an official split
   (with the minority nodes), whatever our value. Where our value differs from the majority, the
   delta (ours - majority) is classified by its shape:
     fee-multiple  k x an emitted-transaction fee (0.008740234375, or 0.0004287109375 in 2019): the
                   M1/M2 attribution of contract-emitted fees;
     other         anything else: M3 (a new_state applied without its starter) or unknown.
   See the CI notes in docs/snapshots.md ("How the published snapshot is checked").
5. Against the baseline (default: compare_wallets_baseline.tsv next to this script, the wallets
   where the c3 lineage, which builds the published snapshots, is known to differ, with their
   deltas):
     known     in the baseline with the same delta: reported, does not fail;
     changed   in the baseline with another delta: fails;
     new       not in the baseline: fails;
     resolved  in the baseline, now equal to the majority: reported, does not fail (update the file).

Exit 0: no new or changed difference; 1: a new or changed difference (or a wallet with no official
majority that is not in the baseline); 3: too few official nodes answered, nothing judged.
--self-test-mismatch adds 0.001 CS to one wallet of our node that is not in the baseline, to prove
that a new difference fails.

Reads only: SyncStateGet, WalletsGet and WalletBalanceGet; nothing is sent or changed.
"""
import argparse
import collections
import concurrent.futures
import datetime
import io
import json
import os
import random
import socket
import struct
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
UNIT = 10**18   # balances are kept as integers of 1e-18 CS: integral * UNIT + fraction

# official mainnet nodes (4.2.532) whose API answers on 9090, the first ones are scanned in full
# (--scan-official) and all of them answer the point reads. M9kk, Gc4X and 74xs are left out: they
# hold the uncorrected 7hWM-class balances (knowledge: rebuild-vs-c3-state-divergence)
OFFICIAL = [
    ("7aYW", "195.201.241.46:9090"),
    ("5u5Q", "31.220.78.229:9090"),
    ("CuX7", "84.247.186.12:9090"),
    ("38Ve", "89.117.57.68:9090"),
    ("JCwd", "194.34.232.27:9090"),
    ("DQFh", "84.247.186.38:9090"),
    ("BFS5", "84.247.186.35:9090"),
    ("48RZ", "84.247.186.30:9090"),
    ("7xdm", "138.201.131.252:9090"),
    ("G278", "75.119.134.124:9090"),
    ("6TDS", "37.60.244.107:9090"),
]

# fees charged for one contract-emitted transaction, whose attribution M1/M2 move between wallets
EMITTED_FEES = [
    ("0.008740234375", 8_740_234_375_000_000),   # 2020 and later
    ("0.0004287109375", 428_710_937_500_000),    # 2019, contract 8tvGa1U6
]


# ---------------------------------------------------------------------------------------------
# Thrift binary protocol, the calls this script needs
T_STOP, T_BOOL, T_BYTE, T_DOUBLE, T_I16, T_I32, T_I64, T_STRING, T_STRUCT, T_MAP, T_SET, T_LIST = \
    0, 2, 3, 4, 6, 8, 10, 11, 12, 13, 14, 15


class Api:
    """One connection to a node's API (Thrift binary protocol, buffered transport)."""

    def __init__(self, api, timeout=60, retries=4):
        host, port = api.rsplit(":", 1)
        self.addr = (host, int(port))
        self.timeout = timeout
        self.retries = retries
        self.sock = None
        self.rfile = None
        self.seq = 0

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = self.rfile = None

    def _connect(self):
        self.close()
        self.sock = socket.create_connection(self.addr, timeout=min(self.timeout, 15))
        self.sock.settimeout(self.timeout)
        self.rfile = self.sock.makefile("rb")

    def call(self, name, args=()):
        """args: (field id, type, value). Returns the result struct as {field id: value}. Retries
        with a new connection on a network error (the node closes idle connections after 30 s)."""
        last = None
        for attempt in range(self.retries):
            try:
                if self.sock is None:
                    self._connect()
                return self._call(name, args)
            except (OSError, EOFError, ValueError) as e:
                last = e
                self.close()
                if attempt + 1 < self.retries:
                    time.sleep(2 ** attempt)
        raise ConnectionError(f"{self.addr[0]}:{self.addr[1]} {name}: {last}")

    def _call(self, name, args):
        self.seq += 1
        out = io.BytesIO()
        out.write(struct.pack(">i", -2147418111))   # 0x80010001: version 1, CALL
        _write_string(out, name.encode())
        out.write(struct.pack(">i", self.seq))
        for fid, ftype, value in args:
            out.write(struct.pack(">bh", ftype, fid))
            if ftype in (T_BOOL, T_BYTE):
                out.write(struct.pack(">b", int(value)))
            elif ftype == T_I32:
                out.write(struct.pack(">i", value))
            elif ftype == T_I64:
                out.write(struct.pack(">q", value))
            elif ftype == T_STRING:
                _write_string(out, value)
            else:
                raise ValueError(f"argument type {ftype} not supported")
        out.write(b"\x00")
        self.sock.sendall(out.getvalue())
        version = self._i32() & 0xffffffff
        if version & 0xffff0000 != 0x80010000:   # strict binary protocol, version 1
            raise ValueError(f"unexpected Thrift message header {version:#x}")
        mtype = version & 0xff
        self._read(self._i32())   # name
        self._i32()               # seqid
        result = self._value(T_STRUCT)
        if mtype == 3:            # EXCEPTION: TApplicationException {1: message, 2: type}
            raise ValueError(f"{name}: application exception {result.get(1, b'').decode(errors='replace')}")
        if 0 not in result:
            raise ValueError(f"{name}: no result")
        return result[0]

    def _read(self, n):
        data = self.rfile.read(n)
        if len(data) != n:
            raise EOFError("connection closed")
        return data

    def _i32(self):
        return struct.unpack(">i", self._read(4))[0]

    def _value(self, t):
        if t in (T_BOOL, T_BYTE):
            return struct.unpack(">b", self._read(1))[0]
        if t == T_I16:
            return struct.unpack(">h", self._read(2))[0]
        if t == T_I32:
            return self._i32()
        if t == T_I64:
            return struct.unpack(">q", self._read(8))[0]
        if t == T_DOUBLE:
            return struct.unpack(">d", self._read(8))[0]
        if t == T_STRING:
            return self._read(self._i32())
        if t == T_STRUCT:
            fields = {}
            while True:
                ftype = self._read(1)[0]
                if ftype == T_STOP:
                    return fields
                fid = struct.unpack(">h", self._read(2))[0]
                fields[fid] = self._value(ftype)
        if t == T_MAP:
            kt, vt = self._read(1)[0], self._read(1)[0]
            return {self._value(kt): self._value(vt) for _ in range(self._i32())}
        if t in (T_SET, T_LIST):
            et = self._read(1)[0]
            return [self._value(et) for _ in range(self._i32())]
        raise ValueError(f"unknown Thrift type {t}")

    # API calls (api.thrift)
    def sync_state(self):
        r = self.call("SyncStateGet")
        return r.get(3, 0), r.get(2, 0)   # lastBlock, currRound

    def wallets(self, offset, limit):
        """WalletsGet ordered by transaction count, ascending: [(key, balance)], count."""
        r = self.call("WalletsGet", [(1, T_I64, offset), (2, T_I64, limit), (3, T_BYTE, 2), (4, T_BOOL, 0)])
        status = r.get(1, {}).get(1, 0)
        if status != 0:
            raise ValueError(f"WalletsGet({offset}, {limit}): status {status}")
        return [(w[1], amount(w.get(2, {}))) for w in r.get(3, [])], r.get(2, 0)

    def balance(self, key):
        """WalletBalanceGet; a wallet the node does not know has balance 0."""
        return amount(self.call("WalletBalanceGet", [(1, T_STRING, key)]).get(2, {}))


def _write_string(out, data):
    out.write(struct.pack(">i", len(data)))
    out.write(data)


def amount(a):
    """general.Amount {1: integral (i32, may be negative), 2: fraction (i64, 1e-18)} as an integer of
    1e-18 CS. A negative balance is a negative integral plus a positive fraction: -0.25 is -1 + 0.75."""
    return a.get(1, 0) * UNIT + a.get(2, 0)


def cs(v):
    """1e-18 units as a decimal string of CS."""
    sign = "-" if v < 0 else ""
    i, f = divmod(abs(v), UNIT)
    return f"{sign}{i}.{f:018d}"


def parse_cs(s):
    s = s.strip()
    sign = -1 if s.startswith("-") else 1
    i, _, f = s.lstrip("+-").partition(".")
    return sign * (int(i or 0) * UNIT + int((f + "0" * 18)[:18]))


B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58encode(data):
    n = int.from_bytes(data, "big")
    out = ""
    while n:
        n, r = divmod(n, 58)
        out = B58[r] + out
    return "1" * (len(data) - len(data.lstrip(b"\x00"))) + out


def b58decode(s):
    n = 0
    for c in s:
        n = n * 58 + B58.index(c)
    body = n.to_bytes((n.bit_length() + 7) // 8, "big") if n else b""
    return b"\x00" * (len(s) - len(s.lstrip("1"))) + body


def log(msg):
    print(msg, flush=True)


# ---------------------------------------------------------------------------------------------
def classify(delta):
    for text, fee in EMITTED_FEES:
        if delta % fee == 0:
            return f"fee-multiple {delta // fee:+d} x {text} (M1/M2)"
    return "other (M3/unknown)"


def load_baseline(path):
    """TSV: address, delta (ours - official majority, CS), mechanism, official minority, note."""
    base = {}
    if not path:
        return base
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            cols = line.split("\t")
            address, delta = cols[0], parse_cs(cols[1])
            mech = cols[2] if len(cols) > 2 else ""
            minority = cols[3] if len(cols) > 3 and cols[3] != "-" else ""
            note = cols[4] if len(cols) > 4 else ""
            base[address] = {"delta": delta, "mechanism": mech, "minority": minority, "note": note}
    return base


def scan(name, api, page, overlap, progress):
    """Every wallet of a node: {key: balance}. Pages overlap by `overlap` wallets, so a wallet that
    moves one place in the order between two pages (its transaction count grew) is still read."""
    node = Api(api)
    seen, offset, step, count, started = {}, 0, page - overlap, 0, time.time()
    try:
        while True:
            rows, count = node.wallets(offset, page)
            for key, bal in rows:
                seen[key] = bal
            if len(rows) < page:
                break
            offset += step
            if offset // step % 300 == 0:
                progress(f"  {name}: {len(seen)} of {count} wallets read ({time.time() - started:.0f} s)")
    finally:
        node.close()
    progress(f"  {name}: {len(seen)} wallets read, the node counts {count} with a balance "
             f"({time.time() - started:.0f} s)")
    return seen


def read_point(api, keys):
    node = Api(api)
    try:
        return {k: node.balance(k) for k in keys}
    finally:
        node.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--ours", required=True, help="HOST:PORT of the node to check")
    parser.add_argument("--official", action="append", default=[],
                        help="NAME=HOST:PORT of an official node (repeat; default: the built-in list)")
    parser.add_argument("--baseline", default=os.path.join(HERE, "compare_wallets_baseline.tsv"),
                        help="known differences ('' for none)")
    parser.add_argument("--scan-official", type=int, default=5, help="official nodes read in full")
    parser.add_argument("--min-official", type=int, default=3,
                        help="official nodes that must answer and be read in full")
    parser.add_argument("--max-lag", type=int, default=20, help="blocks a node may be behind the highest head")
    parser.add_argument("--page", type=int, default=100)
    parser.add_argument("--overlap", type=int, default=20)
    parser.add_argument("--rounds", type=int, default=6, help="point-read rounds, at most")
    parser.add_argument("--recheck-sec", type=int, default=15, help="seconds between point-read rounds")
    parser.add_argument("--summary", help="append a Markdown report to this file (e.g. $GITHUB_STEP_SUMMARY)")
    parser.add_argument("--json", help="write the full result to this file")
    parser.add_argument("--self-test-mismatch", action="store_true")
    args = parser.parse_args()

    officials = [tuple(o.split("=", 1)) for o in args.official] or OFFICIAL
    officials = [(n, a) for n, a in officials if a != args.ours]   # an official node checked as ours
    baseline = load_baseline(args.baseline)
    started = datetime.datetime.now(datetime.timezone.utc)

    # 1. heads
    heads = {}
    for name, api in [("ours", args.ours)] + officials:
        node = Api(api, timeout=20, retries=3)
        try:
            heads[name] = node.sync_state()[0]
        except Exception as e:
            log(f"{name} ({api}): no answer: {e}")
        finally:
            node.close()
    if "ours" not in heads:
        log(f"FAIL: our node {args.ours} does not answer")
        return 1
    top = max(heads.values())
    usable = []
    for name, api in officials:
        if name not in heads:
            continue
        if heads[name] < top - args.max_lag:
            log(f"{name} ({api}): at block {heads[name]}, {top - heads[name]} behind: left out")
            continue
        usable.append((name, api))
    log("heads: " + ", ".join(f"{n} {heads[n]}" for n in ["ours"] + [n for n, _ in usable]))
    if heads["ours"] < top - args.max_lag:
        log(f"FAIL: our node is {top - heads['ours']} blocks behind the official nodes")
        return 1
    if len(usable) < args.min_official:
        log(f"WARNING: {len(usable)} official nodes answer at the head, {args.min_official} needed: not compared")
        return 3

    # 2. full scans, in parallel
    lock = threading.Lock()

    def progress(msg):
        with lock:
            log(msg)

    scanned = {}
    to_scan = [("ours", args.ours)] + usable[:args.scan_official]
    log(f"reading every wallet of {', '.join(n for n, _ in to_scan)}")
    with concurrent.futures.ThreadPoolExecutor(len(to_scan)) as pool:
        futs = {pool.submit(scan, n, a, args.page, args.overlap, progress): n for n, a in to_scan}
        for fut in concurrent.futures.as_completed(futs):
            name = futs[fut]
            try:
                scanned[name] = fut.result()
            except Exception as e:
                log(f"{name}: the scan failed: {e}")
    if "ours" not in scanned:
        log("FAIL: our node could not be read in full")
        return 1
    scanned_official = [n for n, _ in usable if n in scanned]
    if len(scanned_official) < args.min_official:
        log(f"WARNING: {len(scanned_official)} official nodes read in full, {args.min_official} needed: not compared")
        return 3

    keys = set()
    for s in scanned.values():
        keys |= s.keys()

    self_test_key = None
    if args.self_test_mismatch:
        # a wallet every scan agrees on, in the half with fewer transactions (the scan order): likely not moving
        agree = [k for k in scanned["ours"] if b58encode(k) not in baseline
                 and len({scanned[n].get(k) for n in scanned}) == 1]
        self_test_key = random.choice(agree[:len(agree) // 2 or 1])
        log(f"self-test: 0.001 CS added to {b58encode(self_test_key)} on our node")

    def ours_value(key, v):
        return v + UNIT // 1000 if key == self_test_key and v is not None else v

    suspects = set()
    for k in keys:
        vals = [ours_value(k, scanned[n].get(k)) if n == "ours" else scanned[n].get(k) for n in scanned]
        if None in vals or len(set(vals)) > 1:
            suspects.add(k)
    suspects |= {b58decode(a) for a in baseline}
    log(f"{len(keys)} wallets in the scans; {len(suspects)} to read again "
        f"(differ or missing somewhere: {len(suspects - {b58decode(a) for a in baseline})}, baseline {len(baseline)})")

    # 3. point reads on ours and every usable official node, until two rounds agree
    nodes = [("ours", args.ours)] + usable
    pending, prev, settled, active = set(suspects), {}, {}, {}
    for rnd in range(args.rounds):
        if not pending:
            break
        if rnd:
            time.sleep(args.recheck_sec)
        now = {}
        with concurrent.futures.ThreadPoolExecutor(len(nodes)) as pool:
            futs = {pool.submit(read_point, a, sorted(pending)): n for n, a in nodes}
            for fut in concurrent.futures.as_completed(futs):
                try:
                    now[futs[fut]] = fut.result()
                except Exception as e:
                    log(f"{futs[fut]}: point reads failed: {e}")
        if "ours" not in now:
            log("FAIL: our node stopped answering")
            return 1
        names = sorted(now)
        for k in sorted(pending):
            cur = {n: (ours_value(k, now[n][k]) if n == "ours" else now[n][k]) for n in names}
            if k in prev and prev[k] == cur:
                settled[k] = cur
                pending.discard(k)
            else:
                prev[k] = cur
        log(f"round {rnd + 1}: {len(settled)} settled, {len(pending)} still moving")
    for k in pending:
        active[k] = prev.get(k, {})

    # 4./5. majority, split, classification, baseline
    rows, splits, failed = [], [], False
    for k, vals in sorted(settled.items(), key=lambda kv: b58encode(kv[0])):
        address = b58encode(k)
        off = {n: v for n, v in vals.items() if n != "ours"}
        if len(off) < args.min_official:
            active[k] = vals
            continue
        counts = collections.Counter(off.values()).most_common()
        majority = counts[0][0] if counts[0][1] * 2 > len(off) else None
        minority = sorted(n for n, v in off.items() if v != majority) if majority is not None else sorted(off)
        split = len(counts) > 1
        ours = vals["ours"]
        known = baseline.get(address)
        if split:
            splits.append({"address": address, "ours": cs(ours),
                           "officials": {n: cs(v) for n, v in sorted(off.items())},
                           "majority": cs(majority) if majority is not None else None,
                           "minority": minority, "ours_is_majority": ours == majority})
        if majority is not None and ours == majority:
            if known:
                rows.append({"address": address, "status": "resolved", "ours": cs(ours),
                             "majority": cs(majority), "delta": "0", "class": "",
                             "baseline": cs(known["delta"]), "mechanism": known["mechanism"],
                             "minority": ",".join(minority)})
            continue
        if majority is None:
            status = "known" if known else "no-majority"
            failed |= not known
            rows.append({"address": address, "status": status, "ours": cs(ours), "majority": None,
                         "delta": None, "class": "official nodes split, no majority",
                         "baseline": cs(known["delta"]) if known else None,
                         "mechanism": known["mechanism"] if known else "", "minority": ",".join(minority)})
            continue
        delta = ours - majority
        if known is None:
            status = "new"
        elif known["delta"] == delta:
            status = "known"
        else:
            status = "changed"
        failed |= status in ("new", "changed")
        rows.append({"address": address, "status": status, "ours": cs(ours), "majority": cs(majority),
                     "delta": cs(delta), "class": classify(delta),
                     "baseline": cs(known["delta"]) if known else None,
                     "mechanism": known["mechanism"] if known else "", "minority": ",".join(minority)})

    differ = [r for r in rows if r["status"] != "resolved"]
    by_status = collections.Counter(r["status"] for r in rows)
    log("")
    log(f"{len(keys)} wallets compared; our node at block {heads['ours']}; official majority of "
        f"{len(usable)} nodes ({', '.join(n for n, _ in usable)}; read in full: {', '.join(scanned_official)})")
    log(f"{len(differ)} wallets differ from the official majority" +
        "".join(f", {by_status[s]} {s}" for s in ("known", "new", "changed", "no-majority") if by_status[s]) +
        (f"; {by_status['resolved']} baseline wallets now agree (resolved)" if by_status["resolved"] else ""))
    for r in rows:
        log(f"  {r['status']:<9} {r['address']:<44} ours-majority {r['delta'] or '-':>24}  {r['class']}"
            + (f"  [baseline {r['baseline']} {r['mechanism']}]" if r["baseline"] is not None else "")
            + (f"  [official minority: {r['minority']}]" if r["minority"] else ""))
    log(f"{len(splits)} wallets where the official nodes differ among themselves:")
    for s in splits:
        log(f"  {s['address']}: minority {','.join(s['minority'])}; "
            + ", ".join(f"{n}={v}" for n, v in s["officials"].items()) + f"; ours={s['ours']}")
    if active:
        log(f"{len(active)} wallets still moving after {args.rounds} rounds, not judged: "
            + ", ".join(b58encode(k) for k in sorted(active)[:20]) + (" ..." if len(active) > 20 else ""))
    if failed:
        log("FAIL: new or changed differences from the official majority")
    else:
        log("OK: no difference outside the baseline")

    if args.summary:
        with open(args.summary, "a") as f:
            f.write(f"### Wallets compared with official nodes\n\n{len(keys)} wallets, our node at block "
                    f"{heads['ours']}, official nodes: {', '.join(n for n, _ in usable)} "
                    f"(read in full: {', '.join(scanned_official)}). **{len(differ)}** differ from the "
                    f"official majority: {dict(by_status)}. {len(splits)} official splits, "
                    f"{len(active)} wallets still moving. {'**FAIL**' if failed else 'OK'}\n\n")
            if rows:
                f.write("| Status | Wallet | ours − majority | Shape | Baseline | Official minority |\n"
                        "|---|---|---|---|---|---|\n")
                for r in rows:
                    f.write(f"| {r['status']} | `{r['address']}` | {r['delta'] or '-'} | {r['class']} | "
                            f"{(r['baseline'] or '') + ' ' + r['mechanism']} | {r['minority']} |\n")
                f.write("\n")
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"started": started.isoformat(), "heads": heads, "wallets": len(keys),
                       "officials": [n for n, _ in usable], "scanned": list(scanned),
                       "differences": rows, "splits": splits,
                       "active": {b58encode(k): {n: cs(v) for n, v in vals.items()} for k, vals in active.items()},
                       "failed": failed}, f, indent=1)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
