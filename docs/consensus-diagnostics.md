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
info SolverCore: stage3 inputs: R-37 iter=0 seq=37 prev=7C839893 ts=1793783441147 reward=- tx=1
  txIds=40E79CA6 fees=69F07B9F charMask=FC87F4F1/1 newWallets=0 smartSigs=0 conf=5/5 writer=2
  block=BC24F874 round=44C9C6F6 trusted=D7B1BD25 mask=[3] [4] [0] [1] [2]
```

(one line in the log; wrapped here.) Hashes are shortened to their first 8 hex characters; `-` means
empty (no transactions, no reward field).

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

For example, in a test network where two nodes were made to build their block with the timestamp
1 ms later, the same round printed on a majority node and on a skewed one:

```
stage3 inputs: R-1 iter=0 seq=1 prev=C0541961 ts=1793784561704 ... newWallets=7 ... block=2E6353D9 round=A5256CC5 trusted=CA0A93D1
stage3 inputs: R-1 iter=0 seq=1 prev=C0541961 ts=1793784561705 ... newWallets=7 ... block=9876C3C4 round=7B1677FB trusted=CA0A93D1
```

`prev` and every other input agree, `ts` differs, and so do `block` and `round`; `trusted` is equal.

**How to use it.** Collect this line for the same round (`R-`) and iteration from several
confidants (or ask the operators). Nodes that agree print the same `block`/`round` hashes. On the
nodes that differ, the first field that differs is the cause:

- `prev` or `seq` differs: the node is on another chain tip (a fork or a missed block);
- `ts` differs: the nodes chose different timestamps from stage 1;
- `reward` differs (with `ts`, `prev` equal): the cached wallet data (stakes, delegations) differs
  between the nodes; compare their `STATE DIGEST` lines;
- `txIds`, `fees`, `newWallets` or `charMask` differ: the block content differs;
- `conf` differs: the nodes hold different round confirmations;
- all of these equal but `round` differs: the round table differs (the next confidants, the
  packet hashes chosen for the next round, the sub-round or the iteration).

The round table also signs the timestamp, reward, characteristic, sequence and previous hash, so a
difference in one of them makes both `block` and `round` fail (`failed=block+round` below); only
`block` failing points to the block-only inputs (`txIds`, `fees`, `newWallets`, `conf`,
`smartSigs`).

## 2. Why a peer failed: `stage3 check failed`

One line per failing peer:

```
info R-1 SolverCore> stage3 check failed T[4] EJGNJj2S iter=0 msg=ok failed=block+round -> signed another block/round hash (key ok, trusted hash agreed)
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
info R-1 SolverCore> stage3 split iter=0: agree(5)=[T0:KasmLeQs T1:7gZ4BWYh T2:B9XbBZUD(own) T3:Br6U154N T6:HhovRb4b] | block+round(2)=[T4:EJGNJj2S T5:GqamRj6f] of 7
```

and on one of the two skewed nodes, the same round seen from the minority:

```
info R-1 SolverCore> stage3 split iter=0: agree(1)=[T4:EJGNJj2S(own)] | block+round(5)=[T0:KasmLeQs T1:7gZ4BWYh T2:B9XbBZUD T3:Br6U154N T6:HhovRb4b] of 7
```

`agree` lists the confidants whose signatures matched our hashes, ourselves marked `(own)`; the
other groups list the failing confidants by failure pattern. The hash a failing sender signed is
not sent, so the node cannot tell whether two failing senders signed the same block; senders that
fail with the same pattern are grouped. When `agree` is the smaller side, **this node** is
probably the one that built a different block: compare its `stage3 inputs` line with a node of
the larger group.

## 4. After the round

- `not enough active confidants to make a decision, Bootstrap required (active 2 of 7, need 4, mask [X] [X] [X] [X] [1] [2] [X] )`
  (warning): too many peers were excluded in this iteration, so no writer can be chosen.
- `NODE> round package R-2 from Br6U154N gray-lists 2 of 7 confidants of R-1 for 1000 rounds: [T4:EJGNJj2S T5:GqamRj6f(OWN KEY)] - OWN KEY gray-listed`
  (info): the round package from `Br6U154N` excluded these confidants of the previous
  round; every node gray-lists them, and they are not chosen as confidants for that many rounds.
  `OWN KEY` means this node is among them: the majority did not accept its stage 3.
- `the signatures of the block are insufficient or incorrect: #<seq> hash <hex> prev <hex>`
  (warning, was debug): a received block failed the signature check and is dropped; its full hash
  and previous hash identify which block it was.
