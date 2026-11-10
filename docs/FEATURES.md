# What the community node adds to the official node

The community node is a fork of [CREDITSCOM/node](https://github.com/CREDITSCOM/node) 4.2.532. It
speaks the same protocol and follows the same chain: it runs next to official nodes on mainnet and in
consensus with them. This page lists what it adds; each item links the pull requests that make it.
The full list of changes is in [CHANGES.md](CHANGES.md), the operational details in
[differences.md](differences.md).

Everything listed here is in release 4.2.532-community.18.2 or earlier.

## Storage

- **Choice of block database.** BerkeleyDB (the official one), LMDB or RocksDB, set by
  `[storage] db_backend`.
  [#26](https://github.com/CreditsCommunity/node/pull/26), [#37](https://github.com/CreditsCommunity/node/pull/37),
  [#57](https://github.com/CreditsCommunity/node/pull/57)
- **Conversion between databases in place.** An existing database converts to another backend,
  with a free-space check first; an interrupted conversion resumes.
  [#49](https://github.com/CreditsCommunity/node/pull/49), [#50](https://github.com/CreditsCommunity/node/pull/50),
  [#51](https://github.com/CreditsCommunity/node/pull/51), [#52](https://github.com/CreditsCommunity/node/pull/52),
  [#63](https://github.com/CreditsCommunity/node/pull/63), [#65](https://github.com/CreditsCommunity/node/pull/65),
  [#67](https://github.com/CreditsCommunity/node/pull/67)
- **Pruned nodes.** A node can keep only the recent blocks (a few GB instead of the whole chain) and
  still take part in the network; it answers block requests for what it keeps.
  [#22](https://github.com/CreditsCommunity/node/pull/22), [#25](https://github.com/CreditsCommunity/node/pull/25),
  [#27](https://github.com/CreditsCommunity/node/pull/27), [#58](https://github.com/CreditsCommunity/node/pull/58),
  [#61](https://github.com/CreditsCommunity/node/pull/61), [#62](https://github.com/CreditsCommunity/node/pull/62)
- **No more slow starts after a crash.** The transaction index and the block-hash databases are
  rebuilt from the stored blocks instead of re-reading the chain from block 0.
  [#53](https://github.com/CreditsCommunity/node/pull/53), [#54](https://github.com/CreditsCommunity/node/pull/54),
  [#56](https://github.com/CreditsCommunity/node/pull/56)
- **A full node in minutes, the history in the background.** A full node on lmdb or rocksdb starts
  from the snapshot and then fetches the older blocks from its neighbours down to block 0, checking
  each against the hash chain and its signatures; the API serves every block and history as it
  arrives. Consensus comes first: the signatures are checked on verifier threads and the storing
  gets a bounded share of the processor thread, less while the node is a confidant (about 1,000
  blocks/s on mainnet). A pruned node switched to full fetches what it pruned the same way.
  [#122](https://github.com/CreditsCommunity/node/pull/122), [#123](https://github.com/CreditsCommunity/node/pull/123), [#125](https://github.com/CreditsCommunity/node/pull/125), [#129](https://github.com/CreditsCommunity/node/pull/129), [#130](https://github.com/CreditsCommunity/node/pull/130), [#133](https://github.com/CreditsCommunity/node/pull/133), [#140](https://github.com/CreditsCommunity/node/pull/140), [#145](https://github.com/CreditsCommunity/node/pull/145), [#146](https://github.com/CreditsCommunity/node/pull/146), [#147](https://github.com/CreditsCommunity/node/pull/147), [#148](https://github.com/CreditsCommunity/node/pull/148), [#151](https://github.com/CreditsCommunity/node/pull/151), [#152](https://github.com/CreditsCommunity/node/pull/152)

## Starting a node in minutes: snapshots

- **Snapshots built and restored by the node.** A node packages its latest checkpoint as a signed
  snapshot (`--export-snapshot`) and restores one into an empty database (`--import-snapshot`).
  Every file, block signature and the state digest are checked before anything is written.
  [#87](https://github.com/CreditsCommunity/node/pull/87), [#89](https://github.com/CreditsCommunity/node/pull/89),
  [#90](https://github.com/CreditsCommunity/node/pull/90), [#92](https://github.com/CreditsCommunity/node/pull/92)
- **Published snapshot, downloaded on request.** A snapshot of mainnet is published every 12 hours;
  `--download-snapshot` downloads and restores it.
  [#41](https://github.com/CreditsCommunity/node/pull/41), [#107](https://github.com/CreditsCommunity/node/pull/107)
 , [#108](https://github.com/CreditsCommunity/node/pull/108)
- **Snapshots shared between nodes over p2p.** Community nodes serve snapshots of
  their checkpoints to their neighbours, within bandwidth limits (halved while they are confidants).
  A new node with an empty database fetches one from several independent neighbours that agree on
  it, checks every piece against the signed manifest, imports it and joins, with no central server;
  without agreement it falls back to syncing from block 0. Stress tests with four nodes joining at
  once, packet loss and a slow confidant kept the block rate and the round time of the serving
  confidants within a few percent.
  [#99](https://github.com/CreditsCommunity/node/pull/99), [#100](https://github.com/CreditsCommunity/node/pull/100),
  [#101](https://github.com/CreditsCommunity/node/pull/101), [#102](https://github.com/CreditsCommunity/node/pull/102),
  [#104](https://github.com/CreditsCommunity/node/pull/104), [#105](https://github.com/CreditsCommunity/node/pull/105),
  [#106](https://github.com/CreditsCommunity/node/pull/106), [#110](https://github.com/CreditsCommunity/node/pull/110)
- **A survey of who serves what.** `node --survey-snapshots` asks the neighbours for the snapshots
  they serve without opening the chain and prints what they declared; a weekly job publishes the
  result and the list of serving nodes that ships in the packages.
  [#116](https://github.com/CreditsCommunity/node/pull/116)

## State that can be checked

- **Wallet state digest.** A digest of all wallet state, logged every 1000 blocks and saved with
  every checkpoint, lets different nodes and builds compare their state; a checkpoint whose caches
  do not match is set aside.
  [#14](https://github.com/CreditsCommunity/node/pull/14), [#87](https://github.com/CreditsCommunity/node/pull/87)
- **Checked quick starts.** A checkpoint is checked against the chain before it is loaded, one
  ahead of the stored blocks is set aside instead of stopping the node for good, and governance
  state and special orders survive a quick start.
  [#10](https://github.com/CreditsCommunity/node/pull/10), [#17](https://github.com/CreditsCommunity/node/pull/17),
  [#18](https://github.com/CreditsCommunity/node/pull/18), [#60](https://github.com/CreditsCommunity/node/pull/60)

## Smart contracts

- **Correct contract states and balances after a sync.** A syncing node takes the contract states
  the chain declares from the confidants instead of re-executing contracts whose result is not
  reproducible (games that read their own balance), and charges contract fees as the live nodes do.
  An official node in the same situation ends with other balances, or stops at a valid block.
  [#83](https://github.com/CreditsCommunity/node/pull/83), [#93](https://github.com/CreditsCommunity/node/pull/93),
  [#94](https://github.com/CreditsCommunity/node/pull/94), [#95](https://github.com/CreditsCommunity/node/pull/95),
  [#96](https://github.com/CreditsCommunity/node/pull/96)
- **A dependable contract executor.** Races and lost connections between the node and its executor
  are fixed, the executor starts as soon as it answers, and several nodes on one machine no longer
  stop each other's executors.
  [#28](https://github.com/CreditsCommunity/node/pull/28), [#29](https://github.com/CreditsCommunity/node/pull/29),
  [#30](https://github.com/CreditsCommunity/node/pull/30), [#31](https://github.com/CreditsCommunity/node/pull/31),
  [#32](https://github.com/CreditsCommunity/node/pull/32), [#33](https://github.com/CreditsCommunity/node/pull/33),
  [#73](https://github.com/CreditsCommunity/node/pull/73)
- **Contract reads on pruned and restarted nodes.** Getters, deploy lists and transaction lists
  work on nodes that quick-started or pruned the blocks involved.
  [#20](https://github.com/CreditsCommunity/node/pull/20), [#46](https://github.com/CreditsCommunity/node/pull/46),
  [#74](https://github.com/CreditsCommunity/node/pull/74)
- **Tokens in the API.** A token cache that follows every token standard (`[api] tokens_cache=true`),
  contract results after a quick start, and contract calls that no method matches refused before
  their fee is paid; a token template and a Python client in `samples/`.
  [#126](https://github.com/CreditsCommunity/node/pull/126), [#128](https://github.com/CreditsCommunity/node/pull/128), [#132](https://github.com/CreditsCommunity/node/pull/132), [#134](https://github.com/CreditsCommunity/node/pull/134), [#135](https://github.com/CreditsCommunity/node/pull/135)
- **Executor community-2** (build 1555): tokens of the v2 standard are reported as tokens, and
  contract code that runs past its time limit, or whose caller has gone, is stopped. Contract results
  are the same as with the official build 1518.
  [#137](https://github.com/CreditsCommunity/node/pull/137)
- **Executor community-3** (build 1558): a contract that does not compile reports the compiler's
  errors with their line numbers instead of a bare failure.
- **Contract API fixes.** `ContractAllMethodsGet` returns the annotations of each method and
  argument, so a client can tell a getter; a CS payment to a payable contract is no longer listed
  as a deploy, and the contract lists no longer fail on a page holding one.
  [#164](https://github.com/CreditsCommunity/node/pull/164), [#165](https://github.com/CreditsCommunity/node/pull/165)
- **Example clients and contracts** in Python, Go, Java, Rust, C# and JavaScript (status, balance,
  transactions, signed transfers, deploy, call, get), and sample contracts (a counter, a token, a
  payable tip jar, a faucet), checked in CI.
  [#159](https://github.com/CreditsCommunity/node/pull/159), [#161](https://github.com/CreditsCommunity/node/pull/161)

## Network identity

- **Which network is this?** The diagnostic API's `GetNodeInfo` returns the network identifier (the
  same on every node of a network, taken from block 1) and the community release, so that a wallet
  can tell mainnet from a test network.
  [#162](https://github.com/CreditsCommunity/node/pull/162)

## Consensus robustness

- **Two blocks of one round.** When packet loss makes the round end with two valid blocks of the same
  sequence, a community node follows the majority's block instead of staying on a dead branch.
  [#79](https://github.com/CreditsCommunity/node/pull/79), [#80](https://github.com/CreditsCommunity/node/pull/80)
- **Bootstrap rounds.** Networks with more than 8 initial confidants can start, and a bootstrap
  table is accepted only when it fits the node's own round.
  [#70](https://github.com/CreditsCommunity/node/pull/70), [#71](https://github.com/CreditsCommunity/node/pull/71)
- **Rewards and governance.** Mining and staking flags, rewards of uncertain blocks and timed
  delegations are restored and reverted correctly.
  [#8](https://github.com/CreditsCommunity/node/pull/8), [#9](https://github.com/CreditsCommunity/node/pull/9),
  [#11](https://github.com/CreditsCommunity/node/pull/11), [#12](https://github.com/CreditsCommunity/node/pull/12)
- **Forward sync under special order 35.** A syncing node no longer asks a neighbour for blocks that
  neighbour would refuse as a duplicate, and asks another one instead: a node syncing among mostly
  pruned peers could otherwise wait for ever for its first blocks.
  [#136](https://github.com/CreditsCommunity/node/pull/136)
- **One stage 1 per round.** When a fuller round package replaces the first one, a confidant sends
  the stage-1 message it already signed instead of signing a second, different one. Two different
  stage-1 messages made the confidants build different blocks and left the node in the stage-3
  minority. A replacement on the writer side and in a bootstrap round still signs twice.
  [#178](https://github.com/CreditsCommunity/node/pull/178)
- **Known state correction.** A table of proven state differences (one entry: wallet `7hWM6wvs`,
  one contract-state fee) is applied on mainnet only, only where the stored block reward split
  proves it and a node's own state does not already match, so a node synced from scratch is not
  changed. [#184](https://github.com/CreditsCommunity/node/pull/184)

## Diagnostics

- **Stage-3 diagnostics.** When a node's signed stage 3 disagrees with its peers', the log shows
  what the node signed and why a peer's stage failed, at info level only when a check fails; a
  healthy node logs none of it. See [consensus-diagnostics.md](consensus-diagnostics.md).
  [#167](https://github.com/CreditsCommunity/node/pull/167), [#168](https://github.com/CreditsCommunity/node/pull/168)
- **More stage-3 diagnostics.** A `stage3 timestamp` line shows the last block timestamp, every
  stage-1 timestamp and what became of it, and the value chosen; `stage3 inputs` shows more of what
  the node signed; a `stage3 network block` line shows the block the network stored when stage 3
  failed here, with the fields that differ. Logging only.
  [#176](https://github.com/CreditsCommunity/node/pull/176)

## Crash and safety fixes

Remote crashes and out-of-bounds reads in packet and block parsing, a data race, and failures of
the storage engines (a full disk, a failed write) that hung or stopped the node.
[#2](https://github.com/CreditsCommunity/node/pull/2), [#4](https://github.com/CreditsCommunity/node/pull/4),
[#5](https://github.com/CreditsCommunity/node/pull/5), [#6](https://github.com/CreditsCommunity/node/pull/6),
[#7](https://github.com/CreditsCommunity/node/pull/7), [#47](https://github.com/CreditsCommunity/node/pull/47),
[#57](https://github.com/CreditsCommunity/node/pull/57), [#59](https://github.com/CreditsCommunity/node/pull/59),
[#63](https://github.com/CreditsCommunity/node/pull/63)

Since community.15: packet parsers bounded by the bytes received, a network stop request that no
longer stops the node, a utility message obeyed only with the blockchain key's signature, size
limits in the p2p layer (a fork of the p2p library), and two races in the cache databases.
[#121](https://github.com/CreditsCommunity/node/pull/121), [#124](https://github.com/CreditsCommunity/node/pull/124), [#142](https://github.com/CreditsCommunity/node/pull/142), [#143](https://github.com/CreditsCommunity/node/pull/143), [#144](https://github.com/CreditsCommunity/node/pull/144)

## Packages and Docker

- **Release packages** for Ubuntu 20.04 and later (one package: `config.ini` full, `config.pruned.ini`
  pruned), for Windows (x64, `.zip`) and for macOS (Apple Silicon), and a verified list of known
  hosts updated every week.
  [#34](https://github.com/CreditsCommunity/node/pull/34), [#35](https://github.com/CreditsCommunity/node/pull/35),
  [#42](https://github.com/CreditsCommunity/node/pull/42), [#113](https://github.com/CreditsCommunity/node/pull/113)
- **Windows and macOS.** The node builds and passes its tests on Windows (MSVC) and on macOS
  (Apple Silicon), and a Windows or macOS node loads the checkpoints and snapshots a Linux node
  wrote and syncs mainnet with the same state digests; CI checks that on mainnet for the three
  systems before the packages are published.
  [#114](https://github.com/CreditsCommunity/node/pull/114), [#115](https://github.com/CreditsCommunity/node/pull/115)
- **Docker image.** One command starts a node; the first start of a pruned node restores the
  published snapshot (or fetches one from its neighbours), the node serves snapshots, and a database
  conversion is one variable.
  [#41](https://github.com/CreditsCommunity/node/pull/41), [#51](https://github.com/CreditsCommunity/node/pull/51),
  [#109](https://github.com/CreditsCommunity/node/pull/109)
- **Drop-in upgrade.** An official node's folder upgrades to the community node and back.
  [#43](https://github.com/CreditsCommunity/node/pull/43), [#44](https://github.com/CreditsCommunity/node/pull/44)
- **Guided set-up** in every package (`setup.sh`, `setup.bat`): pruned or full, database, how the
  first start gets the chain, the history of a full node (backfill), snapshot sharing and the
  contract executor, and the wallet (below).
  [#131](https://github.com/CreditsCommunity/node/pull/131), [#154](https://github.com/CreditsCommunity/node/pull/154)
- **A terminal wallet in every package and in the Docker image.** `credits-wallet`, the community
  wallet, comes with the node, downloaded from its release and accepted only with a pinned
  SHA-256, with its user guide. The set-up offers to use the node's own key as the wallet's account
  (the key stays in place, with owner-only permissions), or a separate account, and asks whether to
  keep the token cache. In Docker, the key and the token cache are set with variables.
  [#166](https://github.com/CreditsCommunity/node/pull/166)
- **Start-up banner and Docker health check.** The node shows its release, protocol, build and
  storage at start; the Docker health check no longer reports a node at the head as unhealthy after
  a log rotation.
  [#157](https://github.com/CreditsCommunity/node/pull/157), [#160](https://github.com/CreditsCommunity/node/pull/160)
- **Quick-start guide.** `docs/quick-start.md`: Docker and native Linux, Windows and macOS, with
  requirements, ports, first checks, upgrading and common problems.
  [#172](https://github.com/CreditsCommunity/node/pull/172)
- **RocksDB by default for a new full node**, in the set-up and in Docker; pruned stays on lmdb and
  an existing install keeps its database. Docker CPU and memory limits are set with `CREDITS_CPUS`
  and `CREDITS_MEMORY`.
  [#173](https://github.com/CreditsCommunity/node/pull/173)
- **Using an existing node key**, plain or encrypted, in Docker or native; `--decryptkey` (Docker:
  `decrypt-key`) turns an encrypted key file into the plain key after asking the password, keeping
  the encrypted file.
  [#174](https://github.com/CreditsCommunity/node/pull/174), [#175](https://github.com/CreditsCommunity/node/pull/175)
- **Key file.** `NodePrivate.txt` is written readable by its owner only (mode 0600), and the
  encrypted key file is re-encrypted atomically, with clear errors; the format is unchanged.
  [#169](https://github.com/CreditsCommunity/node/pull/169), [#170](https://github.com/CreditsCommunity/node/pull/170)

## Tested on every change

Unit tests; private networks of 5 to 61 nodes with smart contracts and stake-based election;
robustness scenarios (crashes, power cuts, full disks, packet loss and corruption, slow nodes,
storage conversions, snapshot attacks); and networks where official 4.2.532 nodes run next to
community nodes in consensus.
[#15](https://github.com/CreditsCommunity/node/pull/15), [#37](https://github.com/CreditsCommunity/node/pull/37),
[#39](https://github.com/CreditsCommunity/node/pull/39), [#69](https://github.com/CreditsCommunity/node/pull/69),
[#72](https://github.com/CreditsCommunity/node/pull/72), [#103](https://github.com/CreditsCommunity/node/pull/103)
