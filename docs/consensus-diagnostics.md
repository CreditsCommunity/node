# Reading the stage-3 diagnostics

A trusted node sometimes reports, at the end of a round:

```
Stage3 from T[4] - final check ... NOT PASSED! This problem will be resolved automatically.
```

and the debug log adds "Block Signatures are not valid" or "Round Signatures are not valid".
Soon after, the node may wait for a bootstrap round, and its own key may be gray-listed for
`GrayListPunishment` rounds (1000 on mainnet).

Stage 3 carries only signatures: each confidant signs the hash of the block it built, the hash of
the next round table and the hash of the next confidant list, and sends the three signatures
without the hashes. A receiver checks each signature against **its own** three hashes. When a
signature fails, either the key or message is wrong, or the sender built a different block or
round table. The lines below, at info level (no debug log needed), tell these cases apart and
show which input differed. They are written only to the log: nothing is signed, sent or stored
differently, and the consensus result is the same as without them.

## 1. What this node signed: `stage3 inputs`

One line per stage 3 sent by this node:

```
SolverCore: stage3 inputs: R-2215 iter=0 seq=2213 prev=3fa1c2d0 ts=1762245211000 reward=- tx=3
  txIds=9b03e4a1 fees=4c2d0e11 charMask=77a10b3c/3 newWallets=1 smartSigs=0 conf=5/5 writer=2
  block=a04f5e11 round=1d33c702 trusted=e0b2a6f4 mask=[0] [1] [2] [3] [4]
```

(one line in the log; wrapped here.) Hashes are shortened to their first 8 hex characters.

| Field | Meaning | Goes into |
|---|---|---|
| `seq`, `prev` | sequence and previous block hash of the new block | block hash |
| `ts` | block timestamp chosen from the stage-1 timestamps | block hash and round table |
| `reward` | block reward field (hex; `-` before DPoS or with mining off), computed from the confidants' cached wallet data | block hash and round table |
| `tx`, `txIds` | number of transactions and a hash of their signatures in block order | block hash |
| `fees` | hash of the counted fee of every transaction, in order | block hash |
| `charMask` | hash and length of the characteristic (the mask that chose the transactions) | block hash |
| `newWallets` | number of new wallet ids the block assigns | block hash |
| `smartSigs` | number of contract-consensus signatures carried in the block | block hash |
| `conf` | round confirmations in the block: mask size / signature count | block hash |
| `writer`, `mask` | writer index and real-trusted mask of this iteration (`X` = excluded) | compared directly |
| `block`, `round`, `trusted` | the three hashes this node signed | signatures |

**How to use it.** Collect this line for the same round (`R-`) and iteration from several
confidants (or ask the operators). Nodes that agree print the same `block`/`round` hashes. On the
nodes that differ, the first field that differs is the cause:

- `prev` or `seq` differs: the node is on another chain tip (a fork or a missed block);
- `ts` differs: the nodes chose different timestamps from stage 1;
- `reward` differs (with `ts`, `prev` equal): the cached wallet data (stakes, delegations) differs
  between the nodes; compare their `STATE DIGEST` lines;
- `txIds`, `fees`, `newWallets` or `charMask` differ: the block content differs;
- `conf` differs: the nodes hold different round confirmations;
- all of these equal but `round` differs: the round table differs (hashes of the next round's
  packets, confidants).

## 2. Why a peer failed: `stage3 check failed`

One line per failing peer:

```
R-2215 SolverCore> stage3 check failed T[4] 7xQm2Bd1 iter=0 msg=ok failed=block+round -> signed another block/round hash (key ok, trusted hash agreed)
```

`T[4]` is the confidant index, followed by the first 8 base58 characters of its public key.
`msg=ok` means the outer signature of the message was valid (it is checked when the message is
received). `failed=` lists the checks that failed: `block`, `round`, `trusted`, `mask`, `writer`,
or `untrusted` (the sender was already excluded in our mask). The verdict:

- **signed another block/round hash (key ok, trusted hash agreed)**: the key is right and the
  sender agrees on the next confidant list, but it signed another block and/or round table. One
  of the inputs of section 1 differs between this node and the sender.
- **key or message mismatch (trusted signature fails too)**: even the confidant-list signature
  fails, so the sender used another key for its index or another confidant list (another round).
- **signatures ok, mask or writer differs**: the signatures match, but the sender excluded other
  nodes or chose another writer.

## 3. Who is in the majority: `stage3 split`

After each failure (at most 10 per iteration) the node logs the split it sees:

```
R-2215 SolverCore> stage3 split iter=0: agree(2)=[T0:5Hj8Kd2a T3:9aZx1Lp0(own)] | block+round(3)=[T1:Fq3n8Yt2 T2:2bWc7Xe4 T4:7xQm2Bd1] of 5
```

`agree` lists the confidants whose signatures matched our hashes, ourselves marked `(own)`; the
other groups list the failing confidants by failure pattern. The hash a failing sender signed is
not sent, so the node cannot tell whether two failing senders signed the same block; senders that
fail with the same pattern are grouped. When `agree` is the smaller side, **this node** is
probably the one that built a different block: compare its `stage3 inputs` line with a node of
the larger group.

## 4. After the round

- `not enough active confidants to make a decision, Bootstrap required (active 2 of 5, need 3, mask ...)`
  (warning): too many peers were excluded in this iteration, so no writer can be chosen.
- `NODE> round package R-2216 from 5Hj8Kd2a gray-lists 1 of 5 confidants of R-2215 for 1000 rounds: [T3:9aZx1Lp0(OWN KEY)] - OWN KEY gray-listed`
  (info): the round package from the writer `5Hj8Kd2a` excluded these confidants of the previous
  round; every node gray-lists them, and they are not chosen as confidants for that many rounds.
  `OWN KEY` means this node is among them: the majority did not accept its stage 3.
- `the signatures of the block are insufficient or incorrect: #<seq> hash <hex> prev <hex>`
  (warning, was debug): a received block failed the signature check and is dropped; its full hash
  and previous hash identify which block it was.
