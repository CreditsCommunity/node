#!/usr/bin/env python3
"""Robustness test client (scenario net-inflated-rounds): one peer sends a node, for a while, messages
whose round number is far ahead of the network's, as a peer could to make it believe it lags.

It opens one connection to the node's p2p TCP port, registers with a random id and sends there, every
--interval seconds, a direct p2p packet to the node's key carrying a node message (flags 0, type
FirstStage, round = --round plus the number of packets sent so far, a few random bytes). The node
logs "detect round lag" for each of its own rounds and asks this peer for the round; what it sends
back is read and dropped. The node must not see a ROUND LAG in this, let alone reset its network:
one peer is no quorum of its neighbours.

The p2p packet header is 77 bytes, little-endian: type (1 byte), data_size (8 bytes), sender id
(32 bytes), receiver id (32 bytes), reserved (4 bytes); see huge_frame.py.

Usage: inflated_rounds.py HOST PORT NODE_KEY_BASE58 ROUND [--seconds S] [--interval I]
Exit status: 0 when every packet was sent, 1 when the connection could not be kept.
"""
import argparse
import os
import socket
import struct
import sys
import time

import base58

DIRECT = 0
REGISTRATION = 2
FIRST_STAGE = 7


def header(packet_type, data_size, sender, receiver):
    return struct.pack("<BQ32s32sI", packet_type, data_size, sender, receiver, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("key")
    ap.add_argument("round", type=int)
    ap.add_argument("--seconds", type=float, default=1200.0)
    ap.add_argument("--interval", type=float, default=0.5)
    a = ap.parse_args()

    node = base58.b58decode(a.key)
    if len(node) != 32:
        print(f"inflated-rounds: bad key {a.key}")
        return 1
    me = os.urandom(32)
    reg = struct.pack("<B4sH", 0, bytes([127, 0, 0, 1]), 1)
    sent = 0
    reconnects = 0
    deadline = time.monotonic() + a.seconds
    s = None
    while time.monotonic() < deadline:
        try:
            if s is None:
                s = socket.create_connection((a.host, a.port), timeout=5)
                s.sendall(header(REGISTRATION, len(reg), me, node) + reg)
                s.setblocking(False)
            msg = struct.pack("<BBQ", 0, FIRST_STAGE, a.round + sent) + os.urandom(16)
            s.setblocking(True)
            s.sendall(header(DIRECT, len(msg), me, node) + msg)
            s.setblocking(False)
            sent += 1
            try:
                while s.recv(65536):
                    pass
            except BlockingIOError:
                pass
        except OSError as e:
            reconnects += 1
            print(f"inflated-rounds: {a.host}:{a.port}: {e}, connecting again")
            if s is not None:
                s.close()
            s = None
            if reconnects > 20:
                print(f"inflated-rounds: gave up after {sent} packets")
                return 1
        time.sleep(a.interval)
    if s is not None:
        s.close()
    print(f"inflated-rounds: {sent} packets with rounds {a.round}..{a.round + sent - 1} sent to {a.host}:{a.port} "
          f"in {a.seconds:g} s ({reconnects} reconnections)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
