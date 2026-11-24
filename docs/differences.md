# The community node compared with the official node

This page explains what changes for someone who runs the community node (`4.2.532-community.N`) in
place of the official 4.2.532 node from CREDITSCOM. It covers behaviour, not code. For the full
list of changes, pull request by pull request, see [CHANGES.md](CHANGES.md).

## What stays the same

- **Network.** The protocol version (532), the block format and the hashes are unchanged, and so
  are the consensus rules, except for the cases listed under "Consensus fixes" below, which only
  change what a node does when a round ends in conflict. A community node syncs and takes part in
  consensus alongside official nodes. One rare case can still make its balances differ from those
  of official nodes: read [Known risk: fees of contract payouts](#known-risk-fees-of-contract-payouts).
- **Configuration.** The official `config.ini` works unchanged. Without a `[storage]` section the
  node is a full node on BerkeleyDB, like the official one. It opens the same `db/` folder and
  converts nothing.
- **Upgrading.** You can upgrade a folder of the official node by replacing the binaries, and go
  back to the official binaries later. Both directions are tested in CI on mainnet data
  (`upgrade-compat`). Going back works only on a full BerkeleyDB folder: the official node reads
  only BerkeleyDB, and needs every block.
- **Keys, ports and the Thrift API.** These are unchanged. The API changes: when blocks are
  pruned, `TransactionsListGet` skips them (see "Pruned storage" below), and the diagnostic API
  returns two more optional fields (see "Network identity in the diagnostic API" below).

## Known risk: fees of contract payouts

**In short.** In one rare kind of block, community nodes and official nodes that follow the network
live charge some contract fees to different wallets. It has not happened since 2021. If it happens
again, the node tells you in its log.

- **What this node does.** When a smart contract sends CS (a payout), someone pays the fee of that
  payout: the wallet that called the contract. This node always picks that wallet by the original
  rule, the *sequential* rule: the caller of the contract execution that comes before the payout in
  the block.
- **What official nodes do.** Since official release 4.2.419 (July 2019), an official node that
  follows the network live, trusted or not, uses another rule: it charges every payout of a block to
  the contract's *latest* execution in that block. An official node that syncs or replays the block
  later (after downtime, or from block 0) uses the sequential rule, like this node.
- **When the two rules differ.** Only in a block that holds two or more successful executions of
  the same contract, started by different wallets, each followed by its payouts. In that block,
  community nodes and live official nodes compute different balances for those callers, by whole
  fees: one payout fee (0.0087 CS today) per payout charged differently.
- **How often.** It happened in 1,064 blocks of 2019 and 2020 (1,299 payouts), and in no block since
  2021. For all that history the mainnet majority holds the sequential values, the same as this
  node: the official nodes have re-synced those blocks since.
- **What it can cause if it happens again.** Until the official nodes re-sync that block, a
  community node disagrees with them on the balances of the wallets involved:
  - if one of those wallets belongs to a trusted node, the reward split this node computes for it
    differs from the official nodes' while it is a confidant, and this node may end up in the
    minority of those rounds;
  - if one of those wallets spends close to its whole balance, this node and an official node may
    disagree on whether a transaction from it is covered, and one may refuse what the other accepts.

  Other wallets, plain transfers and the chain itself are not affected. The official nodes return
  to the sequential values when they re-sync the block (a restart that re-reads it, or a sync from
  block 0).
- **Why this node does not follow the live rule.** The live rule depends on how the block reached
  each node, so even official nodes disagree among themselves after such a block. The sequential
  rule depends only on the block: every community node, whether it followed the round live, synced
  the block or replayed it from block 0, computes the same balances, and these are the balances the
  mainnet majority holds for the whole history. community.12 to community.19 used the live rule and,
  after a sync from block 0, disagreed with the majority on 109 wallets of 2019 and 2020.
- **What to watch for.** When the node processes such a block (live, while syncing, or when it
  replays the history from block 0, where it meets the 1,064 blocks above), it logs one warning per
  block that starts with `EMITTED FEE RULE:`. It names the block, the contract, each payout charged
  differently with its fee, the wallet this node charges and the wallet live official nodes charge,
  and the balance difference per wallet. It only reports: what the node computes and signs is the
  same with or without it.
- **What to do.** Nothing is needed to keep the node running; restarting or resyncing does not
  change the result, since every community node computes the same. On a block above 36,000,000 (a
  new case, not the 2019–2020 history), report the line in the
  [issues](https://github.com/CreditsCommunity/node/issues) or the
  [technical Telegram](https://t.me/creditstechnical). If a wallet it names is yours, keep at least
  the difference it shows in that wallet until the official nodes have re-synced. If your node is
  trusted and one of the wallets is a confidant, expect it to lose some rounds on the reward split
  while that wallet is in the round.

## New options

### Block database and pruned storage

There are new keys in the `[storage]` section of `config.ini`. See [storage.md](storage.md) for
each key and its default.

| Key | What it does |
|---|---|
| `db_backend` | Chooses the block database: `berkeleydb` (the default, the official format), `lmdb` or `rocksdb`. RocksDB is about a third of the BerkeleyDB size on mainnet. |
| `prune_keep_blocks` | Pruned storage: the node keeps only the latest blocks and removes older ones at each periodic checkpoint. Pruned storage runs on LMDB. |
| `async_write_queue_size`, `write_batch_size`, `rocksdb_block_cache_mb`, `rocksdb_memtable_mb` | Tune the database writer and RocksDB. |
| `backfill`, `backfill_batch`, `backfill_rate`, `backfill_store_per_turn`, `backfill_turn_budget_ms`, `backfill_idle_turn_budget_ms`, `backfill_verify_threads` | Backward sync: a full node whose oldest block is above block 0 (started from a snapshot on lmdb or rocksdb, or switched from pruned to full) fetches the older blocks from its neighbours in the background, with the existing block requests, checks each against the chain's hashes and its signatures and stores it without applying it; the API serves each block and history as it arrives. On by default; `backfill=off` keeps only the snapshot's blocks. |

### Converting the block database

The node converts an existing database to another backend in place.

- `--convert-db` converts, then starts the node.
- `--convert-only` converts, then exits.
- `--convert-batch N` sets how many blocks go in each batch.

The conversion is verified batch by batch. If it is interrupted (by Ctrl-C, a kill or a power
cut), it resumes at the next start. Checkpoints and the transaction index stay valid, so the node
quick-starts after a conversion.

### Snapshots built and restored by the node

What an operator sees and sets is in [snapshots.md](snapshots.md).

- `--export-snapshot <dir>`, on a stopped node of any backend, pruned or full, builds a snapshot
  of its latest periodic checkpoint: the checkpoint files, the blocks before it as a hash chain
  ending at the checkpoint's block, compressed, with a SHA-256 for every file and every 512 KiB
  piece, in a `snapshot.json` signed with the node's key (the checkpoint carries the contract
  states at its block).
- `--download-snapshot [url]` downloads the snapshot published on GitHub (or the one at `url`) with
  `curl`, checks it and restores it as `--import-snapshot` does; run by hand or by the Docker
  entrypoint, never by a normal start.
- `--import-snapshot <snapshot.json>` restores such a snapshot into an empty database of the
  configured backend (LMDB or RocksDB). Every file is checked against the manifest and every
  block before it is written: its signatures against its confidants, the confirmations of the
  round before, and the hash chain to the declared block; the checkpoint must declare the
  manifest's state digest. Anything wrong leaves nothing behind. The node then quick-starts from
  that checkpoint.
- The `[snapshot]` section of `config.ini` holds the settings of p2p snapshot sharing
  (snapshots served by community nodes to their neighbours and fetched from them at a first
  start: `serve`, `serve_rate_kbps`, `serve_max_peers`, `snapshot_blocks`, `fetch`,
  `fetch_wait`, `fetch_fallback`, `min_sources`, `sources`, `sources_file`, `fetch_rate_kbps`).
  A key that is not one of these is logged and ignored.
- **Serving snapshots to the neighbours.** With `[snapshot] serve=on` the node builds the snapshot
  of each periodic checkpoint into `snapshot/<block>/` at low priority and answers the snapshot
  requests of its p2p neighbours while it is synced with the network, within `serve_rate_kbps`
  (KB/s, all peers together; half for one peer), `serve_max_peers` peers within 30 s, and half of
  that while it is a confidant or the writer. It announces it in its p2p host data at start, with
  its community version; community peers refresh that value from the node's answers to their
  pings, so a restart with another `serve` value is seen within about a minute (official peers keep the
  value they first saw until the node leaves their routing table). Official nodes neither see nor
  answer these messages.
- **A first start fetches a snapshot from the network.** A node whose `db/` holds no database,
  with no checkpoint in `qs/`, `db_backend` `lmdb` or `rocksdb` and `fetch=auto` (the default),
  starts its p2p transport without opening the chain and looks for neighbours that serve
  snapshots (`SNAPSHOT: empty database: looking for snapshots on the network`). Once
  `min_sources` sources in different /24 (or /48) networks, or keys listed in `sources`, agree
  on a snapshot, it downloads it, imports it and quick-starts from it; meanwhile it processes no
  other message. Without agreement within `fetch_wait` seconds it syncs from block 0
  (`fetch_fallback=sync`) or keeps waiting (`wait`); the log line names `--download-snapshot` as
  the faster way. A BerkeleyDB node never fetches. The sources used are listed in
  `snapshot.sources.json`.
- **Private and genesis networks set `fetch=off`.** Every node of a network that starts from
  block 0 starts on an empty database: with `fetch=auto`, LMDB and RocksDB nodes would wait
  `fetch_wait` for snapshots nobody has.
- **The restored state is checked at start.** When `snapshot.json` is present (written by
  `--import-snapshot` or by the fetch) and `snapshot.verified` is not, the node compares the
  digest of the state its quick start loaded with the manifest's. It writes `snapshot.verified`,
  or `snapshot.mismatch` and stops with exit code 4; it refuses to start while
  `snapshot.mismatch` exists. An import interrupted by a kill leaves `db/.importing`; the next
  start removes what that import wrote before it looks at `db/`.

### Network identity in the diagnostic API

`GetNodeInfo` of the diagnostic API (`[api] diag_port`) returns two optional fields in `NodeInfo`,
from the Thrift definitions of the fork
[CreditsCommunity/thrift-interface-definitions](https://github.com/CreditsCommunity/thrift-interface-definitions)
(branch `community/network-id`):

| Field | Value |
|---|---|
| `9: optional i64 networkUuid` | The chain identifier: the first 8 bytes of the hash of block 1, read as a little-endian unsigned 64-bit number (`BlockChain::uuid()`). It is the same on every node of a network, so a client such as a wallet can tell mainnet from a test network. Pruned nodes and nodes started from a snapshot know it too (it is in the checkpoint). Left out while the node does not know it yet (an empty database before block 1). Thrift has no unsigned type: the same 64 bits travel as a signed number, so mainnet, `11024959585341937636`, arrives as `-7421784488367613980`; add 2^64 to a negative value (or mask with `0xFFFFFFFFFFFFFFFF`) to get the unsigned one. |
| `10: optional string communityVersion` | The package version, the first line of the `VERSION` file, for example `4.2.532-community.20.1`. Without a `VERSION` file it is `4.2.<protocol>.<minor>`. Always set by the community node. |

The official node sets neither field, and clients built from the official definitions ignore them,
so a client can detect a community node by the presence of `communityVersion`. A local clone made
before this change needs `git submodule sync` once, since the submodule now points to the fork.

### Token cache

`[api] tokens_cache=true` turns on the token cache behind the `Token*` API methods in any build.
The official node has it only in its monitor and wallet builds. The cache follows tokens of every
standard, including v1/v2 tokens whose amounts are BigDecimals, and it skips failed calls. It is
rebuilt from the chain when the checkpoint holds none. See [tokens.md](tokens.md).

### Community bootstrap fallback (expert option, off by default)

Only the initial confidants (the `init_trusted` list, or special order 2) may restart a stalled
network with a bootstrap table, and a table with any other key needs the project's signature. If
all official nodes become unavailable at once, the initial confidants still running may be too few
(fewer than 4), and the network stays stalled until the official nodes are back.

The `[community_bootstrap]` section of `config.ini` adds a liveness fallback. It is off unless
`confidants` lists keys:

```ini
[community_bootstrap]
; community node keys (base58), comma or space separated; empty or missing: off
confidants=<key1>,<key2>,...
; minutes without progress before the fallback may act (1 to 10080, default 30)
stall_minutes=30
```

With the list set, and only after `stall_minutes` in which the node saw no new block, no round
package whose signatures it checked and accepted, and no regular bootstrap table, and while no
neighbour reports a block two or more above its own (one above only holds it for a further
`stall_minutes`, since sync would have fetched that block long before):

- a node whose key is an initial confidant or listed may start a bootstrap table of the initial
  confidants and listed nodes that hold the highest block, as an initial confidant does; the table
  is signed by its starter, not by the project;
- a node with the list takes such a table only if it has more than half of the listed keys, each
  key is an initial confidant or listed, and the starter's signature checks. The round window and
  the stall check of every unsigned table still apply. A node takes one such table per half
  `round_elapse_time`; within that time only a larger table whose starter (its lowest key) is not
  above the current table's starter replaces it, so a listed node cannot displace a running table
  by padding its own with keys of stopped nodes. The starter repeats its table every
  `round_elapse_time` with new random bytes after the signature, so that a node whose fallback
  opens later gets the repeat (the transport drops a message it has seen before);
- of an even set of nodes the starter drops the last key, as the official code does, unless that
  would leave the table without more than half of the listed keys (it does drop it when the rest
  is a regular table of initial confidants);
- a jump of the clock between two checks (the host suspended and resumed, the clock set) counts
  as progress: the node watches again for `stall_minutes` before it acts.

In normal operation nothing changes: blocks and round packages keep the fallback closed, a regular
bootstrap table is handled exactly as before and always replaces a community one, and blocks,
hashes and validation are unchanged. A regular table, even one signed by the project, is progress
like a block: if no block follows it for a further `stall_minutes`, a community table may replace it. Official nodes, and community nodes without the list, drop a
community table (it holds a key that is not an initial confidant and is not signed by the
project); they then take the blocks written meanwhile through ordinary sync once they are back,
as the `official-stop-*` scenarios of the robustness workflow check.

**An expert option, not in the set-up.** Every node that should take part needs the same list: it
only helps when more than half of the listed nodes are running, and mismatched lists weaken that
majority rule. So `setup.sh`, `setup.ps1` and the Docker environment do not offer it until the
option has been reviewed and a coordinated list exists (possibly shipped like `known_hosts.txt`);
edit `config.ini` by hand (in Docker, `~/.credits/config.ini`, which the entrypoint writes only
when it is missing).

The risk it brings is a partition. If a network partition cuts more than half of the listed nodes
off from the official nodes for longer than `stall_minutes`, while the official nodes keep
producing blocks, the listed nodes restart on their own and the two sides write different blocks.
There is no rule that makes either side yield to the other, and the threshold stays "more than
half of the list". What happens at reconnection (option A of the bounded-rollback guard, #189: the
shorter side waits for its operator):

- official nodes follow the longer chain, as for any two chains, and unwind their blocks above the
  fork; the transactions of those blocks are reverted and must be sent again unless the other
  side also wrote them;
- community nodes never follow a longer chain on their own unless its blocks are vouched for by
  confidants they hold (the bounded-rollback guard, see "Consensus fixes"). The blocks of the other
  side are not, so each community node on the shorter side keeps its chain and reports **FORK
  HELD** (on every round line, a detail line once a minute, `fork-held.json` next to the
  database, an unhealthy Docker healthcheck). Its operator checks the other nodes or the monitor
  and chooses (docs/quick-start.md, "If your node reports FORK HELD"):
  - **follow the official chain**: after a partition the block the node refused does not continue
    any block it holds, so at first it knows only an upper bound of the fork point. While FORK
    HELD it looks for the last common block itself (#192). Wait until `fork-held.json` shows
    `"fork_point_status": "exact"`, then stop the node, run it once with `--resolve-fork` (Docker:
    `resolve-fork`) and start it again: it cuts at that block in one run. If the status is
    `"located (unverified)"`, check the point against other nodes or the monitor and add
    `--to-located`; if it stays `"upper bound"` or `"deeper"`, cut at the last common block by hand
    with `--set-bc-top <sequence>`. The blocks above the fork, and their transactions, are
    reverted; the quick-start checkpoints above the new top go with them;
  - **keep the community chain**: create `dismiss-fork` next to the database; the node keeps
    refusing that branch and no longer reports it.

The fallback's own blocks pass that guard on the community side: a community bootstrap block is
signed by the confidants of the community table, and a node that took that table holds it as a
round table it accepted, which is what the guard asks of a bootstrap block. Nothing in this option
changes what may unwind a node's chain: it does not touch block removal, the rollback bound or the
fork records. With the option on, the `official-stop-*` scenarios show no refused block and no
reorganisation on any node.

Measured in the `official-partition` scenarios of the robustness workflow (community side cut off
for 6 minutes, `stall_minutes=2`, the official nodes run the official build), with the guard:

- the community side restarts by the fallback 120-180 s after its last block; both sides then
  write blocks (fork at #533-#563);
- after the cut is lifted the two sides do not look each other up again: every node keeps only
  the 4 neighbours of its own side, nothing is refused, and both chains go on until the end of the
  run (`official-partition`);
- with the community nodes restarted after the reconnection (they dial every host again,
  `official-partition-restart`), all five report FORK HELD within 20 s and refuse every block of
  the official chain: the community side is no longer unwound part of the way and left stuck
  above the fork, as before the guard; it stays on its own chain until its operator decides. In
  one run the official side had stalled during the cut (3 live initial confidants, fewer than 4),
  wrote no block of its own, and took the community blocks once the community nodes were
  restarted: every node then held all 217 community blocks and the same head;
- with the operator path (`official-partition-restart-resolve`) and the fork locator of #192: the
  five community nodes held the fork within 20 s and found the exact fork point (#506) 30 s later;
  one `--resolve-fork` each cut there, every start after it succeeded, and every node then held
  the official chain: one chain, 275 community blocks and their 63 transactions reverted. Without
  the locator, `--resolve-fork` cut a single block (the upper bound), FORK HELD came back a block
  lower, and `--set-bc-top` at the last common block was needed (179-215 blocks reverted); the
  first start after each such cut stopped once on a quick-start checkpoint above the new top,
  which #192 removes. The scenario now fails if a cut lands on an upper bound or a start after a
  cut fails.

A partition with this option on therefore needs operator attention. Keep `stall_minutes` long.

**Known limitations** (accepted; the operators control the list):

- **A hostile lowest listed key.** The starter of a table is its lowest key, and a node takes a new
  community table once half a `round_elapse_time` has passed since the last one. A listed node with
  the lowest key that turns hostile can therefore send, every half `round_elapse_time`, a table
  padded with keys of stopped nodes that cannot decide a round, and keep the community restart
  from deciding while the stall lasts. It cannot write blocks the others would not sign, and a
  regular table still replaces its table at once; the remedy is to take its key off the list.
- **Replay across a chain cut.** A community table is signed over the round and the keys only.
  After `--set-bc-top` or `--resolve-fork` below the round of a table sent earlier, that table
  verifies again for the same round on a node whose fallback is open. It changes nothing unless its
  confidants are themselves running and stalled at that height. Binding the last block hash into
  the signed bytes would change the format a project-signed table shares; it is left to the
  BootstrapSignature protocol of the decentralisation plan.

A set at the highest block with listed keys that hold no more than half of the list is no
community table; the initial confidants in it then make the regular table they would make without
the list (the official odd rule applies), so a node with the list never holds back a regular
restart, for example when the official nodes come back while a minority of the listed nodes runs.

**Decentralisation plan.** When `H_activate_decentralization` of BK's plan lands, the fallback is to
be gated on `lastSeq + 1 < H_activate` (it acts only before the activation height) and later folded
into the plan's BootstrapSignature protocol.

## Start-up behaviour

- **A banner names the release.** At start the console and the log show the CREDITS logo with
  "community" under it, then the release (`4.2.532-community.N` from the `VERSION` file), the
  protocol, the build commit, the storage (pruned or full, database), whether snapshots are served,
  and for a full node whether it backfills. `[params] banner=off` keeps the text lines only, for
  log collectors.

- **Database check.** Before opening the block database, the node looks at `db/`. It writes the
  backend's name in `db/.backend`, and holds `db/.lock` so that two node processes cannot use the
  same folder. It stops with exit code 3, changing nothing, in these cases:
  - `db/` holds a database of another backend than `config.ini` asks for, and no conversion was
    requested;
  - `db/` holds two databases with blocks;
  - another process holds the folder.

  The message says what to do. From a terminal, the node asks whether to convert instead.
- **Checkpoints.** The checkpoints used for a quick start (`qs/`) are checked against the stored
  chain before they load. A checkpoint that does not match the blocks on disk is not loaded. If
  it is ahead of them, which can happen after a crash between saving a checkpoint and writing its
  block, it is moved aside to `qs/<version>.bad` and the next start uses an older checkpoint,
  instead of failing the same way every time.
- **No slow start for a lost index.** The official node falls back to a slow start, replaying
  every block from 0, whenever the caches are inconsistent. The community node repairs the
  usual causes instead:
  - it rebuilds the transaction index (`caches/indexdb`) from the stored blocks after a quick
    start;
  - it rebuilds the block hash databases (`blockHashes: rebuilding from the stored blocks ...`)
    when they fail their check.
- **Bootstrap with more than 8 confidants.** The official node gives a bootstrap round table a
  weight that fits in 8 bits and silently drops tables of 9 or more confidants, so a network whose
  initial confidants (or the bootstrap nodes set by special order 2) are 9 or more never starts or
  restarts. The community node accepts tables of up to 25 confidants (`MaxTrustedNodes`). Up to 8 it
  gives exactly the official weights, so both pick the same table when a stalled network starts
  again; a dropped table is now logged.
- **Bootstrap tables are checked against the node's own state.** A bootstrap round table is
  taken only for a round next to the node's own (from its last block to its round + 2), and,
  unless it is signed with the blockchain key, only once the node's own round has lasted at
  least half of `round_elapse_time` (a node younger than `round_elapse_time`, or already in a
  bootstrap round, takes it as before). The tables honest nodes send pass in the cases a mixed
  network relies on, so it makes the same choices; a node that joined the stalled round late, or
  that is several blocks behind and has not learned the round yet, takes the next table instead.
  A bootstrap through the diag API must name a round within two of what the nodes know (their
  last block + 1, as the sending node uses when it raises the round).
- **State kept across a quick start.** Pending consensus-settings changes, special orders and
  deployed smart contracts survive a quick start. The official node can lose them, or apply
  them differently, after a quick start.

## Running

- **Wallet state digest.** Every 1000 blocks the log has a line
  `STATE DIGEST #<sequence> <hex>`. It is a digest of all wallet state, so nodes, builds and
  backends can be compared: two nodes that agree on the chain must print the same digest at the
  same sequence.
  Every checkpoint saves the digest of the state it holds, with a hash of the consensus settings,
  as `qs/<sequence>/state_digest.bin`, and logs it (`STATE DIGEST #<sequence> ... checkpoint`). A
  quick start recomputes it from the loaded caches, logs `... checkpoint loaded`, and moves a
  checkpoint whose caches do not match it aside to `qs/<sequence>.bad`.
- **Stage-3 diagnostics.** A trusted node logs at info level what it signed in stage 3 (sequence,
  previous hash, timestamp, reward, a hash of the transactions and fees, the three signed hashes),
  why a peer's stage 3 failed (another block/round hash, or a key/message mismatch), the
  agree/disagree split among the confidants, and which keys a round package gray-lists, with
  `OWN KEY` when this node is one of them. Logging only: see
  [consensus-diagnostics.md](consensus-diagnostics.md) to tell from these lines which input differed.
- **Stopping on errors.** Some errors that the official node can survive in an inconsistent
  state now stop the node, and are logged with `cserror`:
  - a block that cannot be stored;
  - a block that fails to apply to the caches;
  - a BerkeleyDB environment that needs recovery.

  The caches are not saved in these cases, so the next start rebuilds them from the database.
  Run the node under a supervisor (systemd, Docker `restart:`) so that it restarts.
- **Full disk.** On RocksDB a full disk no longer blocks the writer: the node keeps the blocks in
  memory and retries.
- **Pruned storage.** A pruned node answers NoSuchBlocks for blocks it has pruned, so a peer
  asks another node for them. `TransactionsListGet` skips pruned blocks.
- **Contract states while syncing.** Blocks carry only the hash of a contract's new state. The
  official node, when it syncs, re-executes the calls to rebuild the states; for some contracts
  this is not reproducible (a game that reads its own balance sees it after the block's payout),
  the state stays out of date, and the fee of a later payout was charged to an earlier caller, so
  official nodes disagree among themselves on such callers' balances. The community node:
  - charges a payout's fee as the nodes that executed the contract do (see "Consensus fixes");
  - records, for every contract, the call and the hash of the state the chain declares, and once
    it is live asks the confidants for every state it holds out of date (at most 16 a round,
    then less and less often), taking only the declared one; the log has a line per state received
    and a summary (`contract states: N known, S stale, …`) when the node becomes live and every
    1000 rounds while some remain;
  - `[params] sync_contract_execution` (default `true`): `false` skips the re-execution while
    syncing altogether, and every state comes from the confidants.

- **Contract calls that cannot run are refused.** `TransactionFlow` checks the method and the types
  of the arguments of a contract call against the contract, and refuses a call that no method
  matches, which would fail on every node with its fee paid (a token amount passed as a string to
  a method that takes a BigDecimal, or the reverse). See [tokens.md](tokens.md).
- **Contract results after a quick start.** A node that quick-starts from a checkpoint at its last
  stored block reads no block. The official node then never marks its stored blocks as read, and
  every contract call sent through its API ends with "Transaction in progress" and no result,
  even though the network executed the call. The community node marks the blocks as read when
  it starts running.

- **Who keeps the network going.** Every 10 minutes (and once at the start) the log has a `PEERS`
  line: the neighbours by announced version (official 4.2.532 when the handshake's minor version is
  0, community when it is higher, unknown when not announced yet), the share of official and
  community nodes among the trusted seats (confidants whose bit is set in the block's realTrusted
  mask) of the last 1000 stored blocks, and how often this node was among them. The node warns
  `OFFICIAL NODES GONE` when no official node has been a neighbour for 10 minutes or signed any of the
  last 200 blocks (with at least half the seats classified), `OFFICIAL NODES BACK` when they return,
  and `TRUSTED SEATS COMMUNITY-ONLY` when every seat of the last 1000 blocks was a community node;
  each once on the change, then at most every 10 minutes. The state is written to
  `peer-status.json` next to the `db` folder (atomically, like `fork-held.json`), printed by the
  Docker healthcheck (never unhealthy for this), and the FORK HELD detail line says whether the
  refused peer is an official or a community node. Log and status only: nothing is sent, signed,
  hashed or validated differently, and there is no new option.
- **Round lag that does not go away.** The official node logs `detect round lag` for every message
  of a round ahead of its own (dozens a round) and does nothing more about it; after a network
  outage a node behind a NAT kept missing round packages for hours, until it was restarted. This node
  logs the first such message of a round (the others at debug level) and counts the rounds. A round
  counts only if, at that moment, a quorum of distinct neighbours (at least 3 and more than half)
  announced a last block at least two above the node's own: the round number in a packet header is
  the sender's word, and one peer sending inflated rounds must not make the node reset its network.
  When it lags in at least 5 such rounds in 10 minutes, for 10 minutes, it warns `ROUND LAG` with
  what it does about it and rebuilds its network sessions as a restart would (`net::Host::Reset`:
  every connection closed, the routing table looked up again, the neighbours handshaked again): the
  first reset at once, the next one 15 minutes later at the earliest, the interval doubling up to 4
  hours, never more than 2 in an hour, each logged with its reason; `ROUND LAG over` when it keeps up
  again. No reset while the node holds a fork (FORK HELD: its neighbours are ahead on the chain it
  refuses); the warning says so, and the healthcheck reports the fork. The state is in `round-lag.json` next to
  the `db` folder, and the Docker healthcheck reports the container unhealthy while the lag persists.
  Nothing is sent, signed, hashed or validated differently, and there is no new option.

## Security and correctness fixes

These were found by reviewing the official code. None of them changes the blocks or the
consensus rules.

- **Remote crashes and out-of-bounds reads.** Fixed when a peer sends a malformed compressed
  region, an empty packet, or a pool whose lengths point past its data. Also fixed: a data race
  in `LockFreeChanger`.
- **A size limit on p2p packets.** The node runs its own fork of the p2p library
  ([CreditsCommunity/p2p](https://github.com/CreditsCommunity/p2p), from the commit the official node uses). The
  first packet of a connection (the registration) may announce at most 64 bytes of data, any
  later packet at most 256 MiB, far above any packet the node sends; above that the connection is
  closed before anything is allocated, with an error line at most once a minute per address
  (`Packet header from <address> announces <n> bytes, above the limit of <limit>: connection
  dropped (...)`). Below the limit the data is read in chunks of 1 MiB, so the memory held follows
  the bytes actually received, not the size announced. Official nodes allocate whatever size is
  announced. The fork also shows a peer's latest host data (version, snapshot capability),
  refreshed from its answers to our pings, instead of the first one seen; passes each received
  datagram with its own length; and stops its routing table (threads, UDP socket) with the p2p
  host at exit.
- **Nodes behind a NAT stay reachable.** When a node behind a NAT (a home router, Docker) opened a
  connection to a peer, the official p2p library (since 2020-01-15, commit 4705c36) recorded the
  connection's source port as the node's TCP port: an ephemeral port the node's socket never bound
  (outgoing connections bind no port) and where nothing listens. Once that connection closed (an
  idle timeout, a network outage, a new address) the peer could not dial the node again, marked it
  unreachable every 120 s and dropped what it sent it, relayed round packages included, until the
  node left the peer's routing table (a restart). It is seen on mainnet: a community node behind a
  NAT (UPnP not found, port forwarded by hand) failed to dial another NAT'd community node 229 times
  in 8 hours, "Connection refused" every 120 s, at the port it had recorded from that node's
  connection; and in the private test network (robustness `net-outage-new-address`). The same NAS
  node lagged for five hours after an outage on 2026-11-24 until it was restarted, probably for this
  reason (its p2p log does not cover the start of the lag). The fork keeps the TCP port a peer announces, and erases only the connection that dropped
  (it erased every connection with that peer in the same direction, a newer one too). Official peers
  still record the source port, so a node behind a NAT (private address, no UPnP mapping; with a
  UPnP mapping the peers do not rewrite the port and this does not run) pings the peers of its
  routing table every 2 s, not only its neighbours: it keeps a connection of its own open to each of
  them, through which they reach it. Only the k-buckets count (nodes whose datagrams came from the
  endpoint recorded, never ids other nodes named), at most 32 a time in turn: with the 10 s idle
  timeout that covers about 160 routing-table peers (mainnet has about 25). A ping goes only to a
  node still in the k-buckets: no queue for unknown ids and no lookup (to a known node not yet
  connected it waits in the send queue until the dial ends, as any message). `net::Host::Reset`
  rebuilds the p2p sessions as a restart would (used by `ROUND LAG`, see above; pending dials are
  abandoned without marking their peers unreachable). Nothing changes on the wire but those pings,
  which any node answers.
- **Smart-contract solver.** Fixed:
  - stale contract locks;
  - executor connections lost between calls;
  - a race on a contract call's inputs;
  - contract states accepted from peers without a request;
  - stage-1 messages that arrive early.
- **Consensus state after a restart.** Fixed:
  - the mining and staking flags swapped on restore;
  - the rewards of an uncertain block kept when the trusted mask differs;
  - the revert of a timed delegation;
  - uninitialised consensus settings.
- **Encrypted key file.** Safer handling of the encrypted key file (atomic re-encryption, no empty
  file on failure, clear errors); the file format is unchanged and stays compatible with the
  official node. When the node encrypts or re-encrypts `NodePrivate.txt` (`--encryptkey`, the
  change of password offered at start, a new key at the first start) it writes
  `NodePrivate.txt.tmp`, flushes it and renames it over the key file, so a crash or a full disk
  leaves the previous file, and the log says the write failed instead of reporting success. A
  failed encryption never writes an empty key file, a key file of an unexpected length stops the
  node with `Bad private key file in ... (N bytes)` instead of asking for a password, a failed key
  derivation is an error instead of a crash, and a file used as password is wiped from memory
  after use.

## Consensus fixes

- **Bounded automatic rollback.** When a node is behind and syncs a competing chain that forks below
  its current head, it rolls back its own blocks down to the fork point before taking the other
  chain. The official node puts no limit on that depth. The community node stops the automatic
  rollback once it would unwind more than `max_auto_rollback` of its own confirmed blocks (default
  100) below the head it had when the rollback started: it leaves its chain intact, logs a prominent
  alert and waits for the operator. The bound covers every automatic removal path — the sync fork
  path, the uncertain-block replacement, the antifork cache and the bad-hash reply — because the
  depth check lives inside the two functions they all go through. The depth is measured against a
  fixed reference, so forward progress cannot reset it and a node cannot be made to creep past the
  limit by alternating removals and appends; the reference is kept in `reorg-episode.json` next to
  the database, so a restart does not reset it either. A reorg this deep is a rare event — a
  deliberate switch to another network, or a rejected one — and the choice is left to a human
  instead of happening silently. The node keeps running and does not crash-loop. The start-up
  integrity repair (blocks of the node's own database that fail the hash check, removed when the
  operator chooses to resolve them) is a local repair, not a reorg, and is not bounded.

  Before any of this, the competing block itself must be vouched for by authority the node already
  recognises, or the node does not unwind at all, not even by one block. A competing block that is
  **not a bootstrap block** needs round confirmations from a majority of the confidants of the node's
  own last block, checked as the block verifier checks a block from the network, but more strictly
  (no one-entry confirmation masks). An honest micro-fork passes, because the honest competing block
  at the same height was produced by the same elected trusted group. A **bootstrap block** carries no
  such confirmations (the verifier accepts it without them, so this check refuses it), and its
  bootstrap flag alone proves nothing, so it passes only when it is authenticated locally: its own
  signatures verify and its confidants are exactly those of a round table this node itself accepted
  in the last few rounds. Otherwise the node keeps its chain and the operator decides. Everything
  that passes is still subject to the depth bound.

  **FORK HELD.** Whenever the node refuses to unwind (beyond the bound, or a competing block not
  vouched for), it records the fork in `fork-held.json` next to the database — the fork point (the
  highest of its blocks the other chain can share, with its hash, exact when the competing block
  names it as its previous block), its head, the refused block, the peer and its announced height,
  the time and the episode's reference — and reports **FORK HELD** until the episode ends or the
  operator decides: `| FORK HELD` on every round line (`R-<n> NORMAL | FORK HELD`), a detail line
  with the two remedies once a minute, and an unhealthy Docker healthcheck. The state clears by
  itself when the head climbs back above the reference. A competing block not vouched for is
  refused at once, but reported as FORK HELD only when the head stays at the same block after it
  (two round starts, or a minute), so a peer on a minority branch cannot make it flap. An unreadable
  guard file is reported and fails safe (a new rollback episode at the head; FORK HELD kept), the
  files are written with a flush to the disk, and they are removed when the database is replaced (a
  new empty database, a snapshot restore). The operator, after checking with other
  nodes or the monitor, either keeps this chain — a `dismiss-fork` file next to the database, which
  the running node picks up within a few seconds: the record keeps that branch (its first known block and
  the hashes seen since) as dismissed, the node still refuses it but no longer reports it, and a
  different branch is reported again — or follows the other one: `--resolve-fork` (no argument)
  cuts the chain at the recorded fork point, only if the block there is still the recorded one; the
  node then syncs forward from there. It relaxes nothing else: when the fork point is exact, only
  the recorded competing block right after it — that very hash — may replace a block the node
  stored again right after the fork point; nothing is ever unwound to or below the fork point, every
  other block meets the usual checks, and an unauthenticated offer changes nothing. The approval
  ends once the node goes on past that height, or a competing block that passes the usual checks
  is offered there. When the fork point is
  only an upper bound, the node looks for the last common block itself while FORK HELD: it asks a
  peer for some of its blocks at or below the bound (the usual block request, a few heights per
  request, one request every few seconds at most) and compares their hashes with its own. The peers
  asked are a short list of neighbours, in turn: those that announce the official version or more
  blocks than the node first, then those that sent a refused block (the peer of the refused block
  may sit on the node's own side, having only relayed it); the next one after a timeout, a useless
  answer or a point that does not verify. The search is read-only (nothing stored, unwound or approved) and bounded to 20 times
  `max_auto_rollback` blocks (1,000 to 10,000; 2,000 when it is 0). The point it finds becomes the
  fork point (`"fork_point_status": "exact"` in `fork-held.json`, and `--resolve-fork` cuts there
  in one run) only if the peer's next block verifies as a synced block would after the node's own
  block there: signatures, and round confirmations by a majority of that block's confidants. That
  verified block is then the branch the cut chooses: until the node is past it, it stores at that
  height only that block and refuses any other (neighbours still on the abandoned branch would
  sync it back on top of the cut), asking first, in random order, a few peers that likely have it
  (a peer that brings no block within 20 s, or answers without one, is dropped from them; with none left the usual random
  choice applies, so a peer cannot hold the sync by what it claims); blocks of the abandoned branch
  are not cached while the preference lasts (a block built on the abandoned block, or on a block of
  that branch confirmed by its parent's confidants), and those cached before it began are dropped
  once at its start, nothing else; once the preference ends this is forgotten; after 30 minutes without it
  (logged as an error) only the abandoned block stays refused there, until the operator cuts again.
  The same one-shot preference applies to the recorded competing block of an exact fork point. It
  only refuses blocks (kept in `resolve-approved.json`).
  Otherwise (a bootstrap block, or a peer that answers wrongly) the point is only a hint
  (`"located (unverified)"`, or `"deeper"` beyond the bound). `--resolve-fork` then still cuts at the
  upper bound, and cuts at the hint only with `--to-located`; it prints the bound, the located point,
  its status and the number of blocks it removes before cutting. `--set-bc-top <sequence>` still cuts by
  hand at a block of the operator's choice. After either cut the quick-start checkpoints above the
  new top are set aside (`qs/<n>.bad`), so the first start after it does not stop on one. A node
  whose storage does not start at block 0 (pruned, or a backfill in progress) cannot slow-start, so
  on such a node a cut with no checkpoint left at or below the new top is refused and removes nothing. Both are one-shot
  and exit; neither advice is ever "restart", since a restart alone changes nothing. See "If your
  node reports FORK HELD" in `docs/quick-start.md`.

  This is a local safety bound only: it changes how far this node unwinds on its own, never what a
  block contains or what the node accepts in normal operation, so it is invisible to other nodes and a
  mixed official/community network behaves exactly as before. Two settings tune it, both in `[params]`:
  `max_auto_rollback=` sets the bound (default 100; 0 removes the depth bound, the authority check
  stays), and `allow_deep_rollback=true` accepts a deliberate switch: it lifts both the depth bound
  and the check above, since the other chain's blocks are confirmed by confidants this node does not
  hold. The `CS_ALLOW_DEEP_ROLLBACK` environment variable does the same. It is the last resort, after
  `--resolve-fork` and `--set-bc-top`: once one rollback episode has completed with it, the node
  turns it off for the rest of the run and says so loudly; remove it from the configuration then.

  **After a partition, and a lagging node.** When both sides of a split keep producing blocks, each
  side's blocks are confirmed by its own confidants. On reconnection a node on the shorter side does
  not follow the longer chain on its own, whatever the depth, because that chain's blocks are not
  confirmed by any confidants it holds: it keeps its chain, reports FORK HELD and the operator
  decides. The official node unwinds to the longer chain without asking. A single node or a minority
  that cannot reach consensus while cut off builds no branch and simply syncs forward when it
  reconnects. The same holds for a node that lags behind with a divergent last block it never
  replaced (for example, it missed a bootstrap round): when the network's next block is not
  confirmed by the confidants of that block, the node waits for the operator instead of unwinding
  it. This is the deliberate choice of this change; `--resolve-fork` is the remedy. After a
  partition with the opt-in community bootstrap fallback, the community side holds the fork in the
  same way and its operator decides (see "Community bootstrap fallback").

- **Two blocks of one round.** When a trusted node does not get one stage-2 in time, it fetches it
  with a request that takes exactly as long as the others wait for its stage-3. Depending on a few
  milliseconds, some trusted nodes finish the round with every signature while the others retry
  stage 3 without the late node, and the writer publishes the retried block. The two blocks differ
  only in the set of real trusted. The official node keeps its own block for good, then no longer
  matches the following rounds (seen in CI as a network stuck in bootstrap). The community node
  logs this as a fork and, if it is a trusted node that still holds the block undeferred, takes
  the writer's block when:
  - a majority of all the trusted of the round signed it, checked against the node's own list of
    the round's trusted, so a minority of dishonest signers can never make it switch;
  - the nodes outside that set are too few to form a round on their own block.

  Fees and the block reward move to the new set of signers.
- **The official "more signatures" rule works.** The official node meant to replace its block by
  one with more signatures, but the check before the replacement compared the block with itself and
  always failed. It now works. Residual risk, as in the official design: a package with more
  signatures that reaches only part of the network would move only that part.

- **Fees of contract payouts.** The fee of a transaction a contract emits (a payout) is charged to
  the initiator of one of the contract's executions. The official node charges the contract's last
  execution it holds (`ref_execute`), and what it holds depends on how it got the block:
  - a node that reads or syncs the block applies its new states in order, so a payout is charged to
    the execution whose new state precedes it (the sequential rule);
  - every node that follows the rounds live, trusted or not, applies all the new states of the
    round's block when the block is formed (`statesCreated`), before it stores the block, so it
    charges all payouts of a block to the contract's latest execution in it (official 4.2.419 and
    later);
  - a syncing node whose re-execution of a contract failed keeps an earlier execution and charges
    the previous caller.

  The three agree unless one block holds several executions of one contract with payouts, or a
  re-execution fails. The community node decides from the block alone, so every node computes the
  same result however it got the block: the fee goes to the initiator of the contract's highest
  execution among its new states before the payout in the block that the block declares successful
  (a non-zero state hash, or a non-empty state value in older blocks), whether or not the node
  reproduced that state; a failed execution is passed over. "Highest" is what a node applying the
  block in order holds, since a state older than the contract's execution is ignored; in an ordered
  block it is the nearest one. The charge never goes below the contract's execution before the
  block, and is that execution when no such new state precedes the payout: on a node that follows
  the rounds live, the one it held before applying the block's states. A rollback refunds each
  payout to the execution it was charged to.

  This is the sequential rule, which the mainnet majority holds for the whole history, and the
  node applies it always, also where official nodes that follow the rounds live apply the latest
  one: see [Known risk: fees of contract payouts](#known-risk-fees-of-contract-payouts). The two
  rules charge 1,299 payouts differently, in 1,064 blocks from 10.6M to 35.9M (2019 and 2020), and
  none after 2021. community.12 to community.19 charged every payout of such a block to the latest
  execution instead (#94): a node that processed those blocks with those versions holds different
  balances on 109 wallets, by one fee per payout. That is a node that synced them from block 0 over
  the network as well as one that re-read them from its own database (a slow start). The published
  snapshots are not affected: their lineage processed those blocks with community.3 and .4, before
  #94. Compared with an official node whose re-execution failed while syncing, the balances of the
  callers involved differ by one fee, and are those of the majority.

  Known limit: a charge that is not a new state of the block (no new state of the contract before
  the payout, or only outdated ones) is recorded when the block is stored, so that a rollback
  refunds the same execution. The record lives in memory: after a restart, or after the chain was
  cut with `--set-bc-top` or `--resolve-fork`, a rollback of such a block refunds the contract's
  current execution (`ref_execute`) instead. That needs a payout before any new state of its
  contract in the block, or after an outdated one; none was seen in the blocks examined.

- **Known state corrections.** The lineage of the published pruned snapshots synced the dice-contract
  blocks of 2026-06-05 with the official rule above (a stale `ref_execute` after a failed
  re-execution). The payout 176760929.2 (block 176,760,929) pays `MooNRor8…` the win of its call 176760925.1, whose fee already includes the
  emitted transaction's fee. The replay of that contract state failed, the stale `ref_execute`
  still pointed to the previous successful call, by `7hWM6wvs…`, and the payout's contract-state
  fee (0.008740234375 CS) was charged to `7hWM6wvs…` as well: that lineage holds one fee less for
  `7hWM6wvs…` than the majority that executed the contract. `MooNRor8…` was the expected
  counterpart, but the nodes of the majority report the same `MooNRor8…` balance as that lineage
  (public API of 8 majority and 3 minority nodes at block 187,731,527, 2026-11-10, with `7hWM6wvs…`
  one fee apart on the same calls), so there is none to correct. `7hWM6wvs…` is a confidant in half
  of the rounds, and every reward split a community node signs for it differs from the majority's
  by that fee, which puts the node in the stage-3 minority. `csnode/statecorrection` keeps a table
  of such known differences: the proven wallet, its delta, and optional counterpart adjustments
  applied in the same step (counterparts are not proven by the reward split; they come from the
  transaction that caused the difference). An entry applies only on its network (the chain uuid
  of mainnet, also known to nodes started from a snapshot; never on the test network or private
  networks) and from the block that caused it (176,760,929). A correction is applied once, at the
  first block that proves it: the proven wallet is a trusted confidant of the block, staking is
  on, the block's stored reward split (signed by the majority) differs from the one this node's
  stakes give, and it is reproduced exactly with the delta applied. If a counterpart's balance is
  below its adjustment, nothing is applied and a warning is logged: no balance is made negative. A
  node whose state is already right never applies it, an applied correction is never applied
  twice, and a block that neither value explains is only logged (rate-limited per entry). The node
  logs the block that proved the correction and the new balances of every wallet it changed.
  Rounds where the trusted majority is the uncorrected group may still put a corrected node in the
  minority: the correction makes the node agree with the network majority, not with every round's
  trusted majority.

Official nodes on the side that keeps its block behave as before; mixed networks do not fork (CI:
equal state digests with official binaries on half of the nodes, and the same share of signed
rounds for both).

## Packages and Docker

- **Release packages.** Each release has one package for full and pruned storage (until
  community.13, a full and a pruned package), in these builds:
  - `linux-x64` needs glibc 2.38 (Ubuntu 24.04, Debian 13 and later);
  - `linux-x64-glibc2.31` runs on Ubuntu 20.04, Debian 11 and later;
  - `windows-x64` (a `.zip`, Windows 10 and later; start `node.exe` from a terminal in the `csnode`
    folder) and `macos-arm64` (Apple Silicon; clear the download quarantine once with
    `xattr -dr com.apple.quarantine csnode`; the node is not signed).

  The Linux packages include the JDK of the official release; the Windows and macOS ones include
  Eclipse Temurin 11 (the official release has no JDK for them), in `jdk/`, and their
  `executor_command` points at it. Each package includes the community build of the contract
  executor.
- **Checkpoints and snapshots are the same on every system.** A Windows or macOS node loads what a
  Linux node wrote and the other way round: the binary checkpoint files record `long` as 8 bytes
  whatever the system, and the checkpoint's text files are read and written in binary mode. On
  Linux the files are byte for byte what they were.
- **`config.ini` in the packages.** It is the official one, with five changes:
  - `[event_report]` is commented out (it reports node events to a fixed address);
  - `executor_commit_max` accepts the community executor;
  - `compatible_version=true`, the default of the node binary (the official file sets `false`):
    a synced block that fails the node's own transaction check is logged
    (`skip block validation`) and stored, instead of stopping the node at it for good. See
    "Contract states while syncing";
  - `sync_contract_execution=true` (the default), with a comment;
  - next to it, `config.pruned.ini` is the same file with a `[storage]` section for pruned storage
    on LMDB: `./node --config-file config.pruned.ini`.

  The executor listens on 127.0.0.1 only.
- **Known hosts.** `known_hosts.txt` is refreshed weekly from a verified list of seed nodes.
- **Docker image.** The image is `ghcr.io/creditscommunity/credits-node` (until community.15 `ghcr.io/enzinobb/credits-node`, still updated for a while). By default it runs a pruned
  node that starts from a snapshot of the chain, published every 12 hours, instead of syncing
  from block 0. The snapshot's checksum and its state digest are verified before use. A full node
  on lmdb or rocksdb starts from the snapshot too and fetches the older blocks in the background
  (`CREDITS_BACKFILL=off` to keep only the snapshot's blocks); switching from pruned to full is
  allowed. See [packaging/docker/README.md](../packaging/docker/README.md).
