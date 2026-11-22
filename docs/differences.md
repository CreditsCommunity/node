# The community node compared with the official node

This page explains what changes for someone who runs the community node (`4.2.532-community.N`) in
place of the official 4.2.532 node from CREDITSCOM. It covers behaviour, not code. For the full
list of changes, pull request by pull request, see [CHANGES.md](CHANGES.md).

## What stays the same

- **Network.** The protocol version (532), the block format and the hashes are unchanged, and so
  are the consensus rules, except for the cases listed under "Consensus fixes" below, which only
  change what a node does when a round ends in conflict. A community node syncs and takes part in
  consensus alongside official nodes.
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
| `10: optional string communityVersion` | The package version, the first line of the `VERSION` file, for example `4.2.532-community.18.2`. Without a `VERSION` file it is `4.2.<protocol>.<minor>`. Always set by the community node. |

The official node sets neither field, and clients built from the official definitions ignore them,
so a client can detect a community node by the presence of `communityVersion`. A local clone made
before this change needs `git submodule sync` once, since the submodule now points to the fork.

### Token cache

`[api] tokens_cache=true` turns on the token cache behind the `Token*` API methods in any build.
The official node has it only in its monitor and wallet builds. The cache follows tokens of every
standard, including v1/v2 tokens whose amounts are BigDecimals, and it skips failed calls. It is
rebuilt from the chain when the checkpoint holds none. See [tokens.md](tokens.md).

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
  it. This is the deliberate choice of this change; `--resolve-fork` is the remedy. The opt-in
  community bootstrap fallback (PR #188, not merged yet) expects the longer chain to win after a
  partition; with this change that switch is an operator decision too.

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

- **Fees of contract payouts.** A transaction a contract emits is charged to the contract's
  latest successful execution in the same block, which is what a node that executed the contract
  does (it applies every new state of the block before the payouts). A syncing official node whose
  re-execution failed charges it to an earlier caller instead. This changes, by one fee, the
  balances such a node computes for those callers, and makes them those of the nodes that took
  part in the rounds.

- **Known state corrections.** The lineage of the published pruned snapshots synced the dice-contract
  blocks of 2026-06-05 with the official replay rule above. The payout 176760929.2 (block
  176,760,929) pays `MooNRor8…` the win of its call 176760925.1, whose fee already includes the
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
