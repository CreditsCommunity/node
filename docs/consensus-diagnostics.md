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
show which input differed. They appear only when something goes wrong (a failed check, a
gray-listing, a missing majority): a node in a healthy network logs none of them. They are written only to the log: nothing is signed, sent or stored
differently, and the consensus result is the same as without them.

## 1. What this node signed: `stage3 inputs`

What this node put into the hashes it signed. It is logged at info level once per iteration, just
before the first `stage3 check failed` line, so both sides of a split print it for the same round;
at debug level it is logged for every stage 3:

```
info SolverCore: stage3 inputs: R-37 iter=0 seq=37 prev=7C839893 ts=1793783441147 reward=- tx=1
  txIds=40E79CA6 fees=69F07B9F newWallets=0 smartSigs=0 conf=5/5 confMask=1F confSigs=9A0B7C21
  realTrusted=[0] [1] [2] [3] [4] version=0 roundCost=0.0 bootstrap=0 metaTs=1793783441147
  charMask=FC87F4F1/1 writer=2 block=BC24F874 round=44C9C6F6 trusted=D7B1BD25 mask=[3] [4] [0] [1] [2]
```

(one line in the log; wrapped here.) Hashes are shortened to their first 8 hex characters; `-` means
empty (no transactions, no reward field). The fields from `seq` to `bootstrap` are the hashed
fields of the block, in the order of the block binary; the `stage3 network block` line of
section 1b prints the same fields for the block the network stores.

| Field | Meaning | Goes into |
|---|---|---|
| `seq`, `prev` | sequence and previous block hash of the new block | block hash |
| `ts` | block timestamp chosen from the stage-1 timestamps (section 1a shows how) | block hash and round table |
| `reward` | block reward field (hex; `-` before DPoS or with mining off), computed from the confidants' cached wallet data | block hash and round table |
| `tx`, `txIds` | number of transactions and a hash of their signatures in block order | block hash |
| `fees` | hash of the counted fee of every transaction, in order | block hash |
| `newWallets` | number of new wallet ids the block assigns | block hash |
| `smartSigs` | number of contract-consensus signatures carried in the block | block hash |
| `conf` | round confirmations in the block: mask size / signature count | block hash |
| `confMask` | the confirmation mask bits (hex): which of the previous round's trusted signed this round's table | block hash |
| `confSigs` | hash of the confirmation signatures in order; `(n zero)` counts empty signature slots | block hash |
| `realTrusted` | the real-trusted mask written into the block | block hash |
| `version`, `roundCost`, `bootstrap` | block version, round cost and the bootstrap flag of the block | block hash |
| `metaTs` | the timestamp of the round package being built (equal to `ts`) | round table |
| `charMask` | hash and length of the characteristic (the mask that chose the transactions) | block hash |
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
`confMask`, `confSigs`, `smartSigs`).

### 1a. How the timestamp was chosen: `stage3 timestamp`

Logged right after `stage3 inputs` (info level on a failure, debug level otherwise). The block
timestamp is the mean of the stage-1 timestamps of the confidants in the mask, taken as offsets
from the last block's timestamp, with a 3-sigma outlier drop (`SolverCore::chooseTimeStamp`).
The line shows every value that went into it:

```
info R-37 SolverCore> stage3 timestamp: R-37 last=1793783440011 chosen=1793783441147 N=5/5 mean=1136.200 disp=18.560
  [T0:KasmLeQs:1793783441140:in:1129] [T1:7gZ4BWYh:1793783441152:in:1141] [T2:B9XbBZUD:1793783441143:in:1132]
  [T3:Br6U154N:1793783441150:in:1139] [T4:EJGNJj2S:1793783441146:in:1135]
```

| Field | Meaning |
|---|---|
| `last` | the last block's timestamp this node used (`BlockChain::getLastTimeStamp()`: the deferred block or the last stored one) |
| `chosen` | the timestamp written into the block (`ts` of the inputs line) |
| `N=a/b` | stamps left after the outlier drop / stamps that entered the mean |
| `mean`, `disp` | mean and dispersion of the offsets (in ms) at the end of the computation |
| `fallback` | present when no stamp was usable: `plain-mean-of-all` (integer mean of every stage-1 timestamp, masked or not), `last+1`, or `no-last-timestamp` (no block timestamp could be read: `0` is chosen) |
| `[Ti:key:stamp:fate[:offset]]` | one entry per stage 1 held for the round: confidant index, key, the raw stage-1 timestamp and its fate: `in` (used, with its offset from `last`), `outlier` (dropped by the 3-sigma test), `masked` (confidant excluded in the mask), `not-after-last` (not later than `last`), `empty`, `invalid` (not a number), `sender-out-of-range` |

**How to compare it between nodes.** Stage-1 messages are signed, so two nodes hold the same
stamp for the same confidant: entries with a different stamp for the same index mean one node holds
another stage-1 message of that sender (a stage 1 built twice in the round). The set of entries
and their fates must agree too: a different `last` changes which stamps pass `not-after-last`, and
a stamp present on one node and missing on the other changes the mean. Note that the `mean`
alone, with the same `last`, gives `chosen`: with the same entries and the same `last`, two nodes
cannot choose different timestamps.

### 1b. The block the network stored: `stage3 network block`

When the stage 3 of this node failed against its peers, the block the network writes for that
sequence arrives in the next round package. If it differs from the block this node signed, it is
logged once, with the same fields as `stage3 inputs`, and the names of the fields that differ:

```
info R-38 SolverCore> stage3 network block: seq=37 hash=6AE2F8B5 (we signed C566481F) seq=37 prev=7C839893
  ts=1793783441150 reward=... tx=1 txIds=40E79CA6 fees=69F07B9F newWallets=0 smartSigs=0 conf=5/5
  confMask=1F confSigs=9A0B7C21 realTrusted=[0] [1] [X] [3] [4] version=0 roundCost=0.0 bootstrap=0
  differs=ts,reward,realTrusted (ts and reward also change with the real trusted mask when the writer retried stage 3)
```

Read `differs` with care: when the majority excluded this node (`realTrusted` has an `X` at its
index), the writer rebuilt the block in a second stage-3 iteration with the smaller mask, so `ts`
(the mean of fewer stage-1 stamps) and `reward` (shared among fewer confidants) differ for that
reason alone. Fields that do not depend on the mask (`prev`, `txIds`, `fees`, `newWallets`,
`conf`, `confMask`, `confSigs`, `version`, `roundCost`, `bootstrap`) must be equal on a healthy
node; one of them differing is the cause of the failure. When only `ts`, `reward` and
`realTrusted` differ, the first-iteration block cannot be compared this way: collect the
`stage3 inputs` and `stage3 timestamp` lines of the same round from a peer of the majority.

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

After each failure (at most 10 per iteration) the node logs the split it sees. When the node's
own stage 3 arrives after its peers', the early lines list only the peers checked so far: read the
last `stage3 split` line of the iteration.

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

## 5. Stage 1 entered again in the same round: `stage1 resent unchanged` / `stage1 signed again`

A confidant can receive a second round package for the round it is in, with more real trusted
signatures than the first. It then replaces the first package and enters stage 1 again
(`SolverCore::nextRound(true)`):

```
info NODE> Current Roundpackage of 1205 will be replaced by new one
```

(this line was at debug level before). If this node already signed and sent its stage 1 for that
round and sub-round from the same seat, it now sends the same signed message again instead of
signing a second one with a new timestamp:

```
info Trusted-1: stage1 resent unchanged: R-1205.0 T3 ts 1793783441147, a new one would differ in candidates,packets
```

`a new one would differ in` lists the fields a new stage 1 would have changed (`candidates`: the
trusted candidates as a set, `packets`: the packet hashes); it is absent when nothing differs.
Both are this node's votes for the next round, and the stored message the other confidants
already hold stays valid. Receivers check the resent message like any stage 1. A receiver that
already holds it ignores it, because its timestamp is the same.

Two signed stage-1 messages from one seat with different timestamps can leave the confidants
holding different copies. They then compute different block timestamps, or see two
hash-signature pairs for that seat at stage 2. In both cases the seat, or the block, can fail the
stage-3 check.

The node signs a new stage 1, as before this change, when the stored one would be refused or
would make the seat look faulty:

```
info Trusted-1: stage1 signed again: R-1205.0 T3 ts 1793783441147 -> 1793783442010, content differs (hash), reason hash-changed
```

- `hash-changed`: the characteristic hash changed, because the new package changed the last
  block. This hash must match the majority's.
- `not-after-last-block`: the stored timestamp is not after the last block's, so the receivers
  would refuse it.
- `too-old`: the stored timestamp is more than `DefaultTimeStampRange` (30 s) old, so fresh
  timestamps from the other confidants could fall outside this node's stage-2 window, which is
  measured from its own timestamp.
- `unreadable`: the stored timestamp cannot be read as a number.

Two cases still sign a second stage 1 for the same round, as before this change, and log no
`stage1` line:

- a fuller package that reaches the writer of the round package (the `moreTrusted` branch of
  `Node::getRoundTable` for a node that holds its own package): that path starts the round
  again with `updateRound = false`, which clears the stored stage 1;
- a fuller package that replaces the first one inside a bootstrap round: the start of a bootstrap
  round clears the stored stage 1.

`content` compares the old and the new stage 1 apart from the timestamp: `same`, or `differs`
with the fields (`hash`, `candidates`, `packets`). The stored stage 1 is kept only while the node
is in that round and sub-round on the same seat. A new round, a new sub-round and every bootstrap
round start without it.

The replacement itself also logs `fatal ConveyerBase> Meta round currently in conveyer`. This line
is older than this change and the official node logs it too, on every replacement of a round
package by one for the same round. The conveyer already holds a round table for that round, so it
keeps that table and does not store the second one. Despite its `fatal` level, the node keeps
running.
