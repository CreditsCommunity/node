# AGENTS.md: CREDITS node (community)

Instructions for coding agents working in this repository. Read this before changing anything.

## What this is

The C++ node of the CREDITS blockchain: transport, consensus (the solver), block storage, the Thrift
API and the bridge to the Java contract executor. This repository is the community continuation of
[CREDITSCOM/node](https://github.com/CREDITSCOM/node), forked from the official release 4.2.532
(commit `4a95aaaf`). Community releases are `4.2.532-community.N`: protocol version 532, compatible
with the mainnet.

Related repositories of [CreditsCommunity](https://github.com/CreditsCommunity):

- `p2p`: the Kademlia transport, a git submodule here (`p2p/`).
- `contract-executor`: the Java process that compiles and runs smart contracts; the packages ship the
  release named in `packaging/executor-tag`.
- `smart-contract-api`, `thrift-api-client`: Java libraries the executor is built from.
- `thrift-interface-definitions`: the Thrift IDL of the API (submodule `third-party/thrift-interface-definitions`).
- `wallet`: the terminal wallet the packages ship (`packaging/wallet-tag`, `packaging/wallet-sha256`).

## Platform constraints (read first)

- Mainnet runs the official node 4.2.532 with contract executor build 1518. This node must stay
  compatible with them and must never change consensus rules unilaterally: block format, hashes,
  validation, fees and contract results must stay what the official node computes.
- A contract cannot call another contract on mainnet: executor 1518 fails every
  `invokeExternalContract`, read-only calls included (see the header of `ci/testnet/contracts.py`).
  No value, not even CS, passes from one contract to another: the node refuses a transaction a
  contract emits with `sendTransaction` to another contract (`solver/src/smartcontracts.cpp`).
  Enabling either changes consensus: it needs a coordinated, height-gated upgrade of all
  trusted-capable nodes.
- Amounts in a contract's `sendTransaction` are `double`s. A call whose arguments do not match the
  method's declared types fails on every node and the fee is still charged (see `docs/tokens.md`;
  the community API refuses some mismatches before sending).
- Consensus-relevant changes (anything that can change what a node accepts, computes or signs)
  need extra review, and `docs/differences.md` must describe them.

## Layout

| Path | Contents |
|---|---|
| `client/` | the `node` executable: `main.cpp`, configuration (`client/config`), version |
| `csnode/` | node core: blockchain, conveyer, block validation, sync, backward sync, snapshots |
| `solver/` | consensus rounds and stages, smart-contract consensus (`smartcontracts.cpp`) |
| `csdb/` | storage: BerkeleyDB (official), LMDB and RocksDB backends, amounts, addresses |
| `net/` | packets, neighbourhood, transport on top of `p2p` |
| `api/` | Thrift API and diagnostic API handlers, executor connection, token API |
| `cscrypto/`, `p2p/`, `third-party/*` | git submodules |
| `lib/`, `lmdbxx/` | system helpers, LMDB C++ wrapper |
| `tests/` | GoogleTest unit tests (`tests/csnode`, `tests/lib_system`, `tests/lmdbxx`) |
| `ci/` | scripts the workflows use: private test network (`ci/testnet/run.sh`), contracts, set-up |
| `packaging/` | packages (`make-package.sh`), guided set-up (`packaging/setup/`), Docker, mainnet config |
| `samples/` | API clients in six languages, example contracts, Python token client |
| `docs/` | user docs, `CHANGES.md`, `differences.md`, design specs (`docs/specs`) and plans |

## Build and test

The workflows are the reference: `.github/workflows/build.yml` (Linux) and `build-platforms.yml`
(Windows MSVC, macOS Apple Silicon). Linux (Ubuntu 24.04, GCC 13), with Boost 1.83 built static with a
static runtime (see README.md, "Building from source"):

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=ON \
  -DBoost_ROOT="$HOME/boost" -DBoost_NO_SYSTEM_PATHS=ON \
  -DCMAKE_CXX_FLAGS="-include cstdint -include cstddef" -DWITH_HASKELL=OFF
cmake --build build -j"$(nproc)"
cd build && ctest --output-on-failure --timeout 300
```

The binary is `build/client/node`. `-DCREDITS_TESTNET=ON` builds for a private test network
(genesis and starter keys from `CS_TESTNET_GENESIS_KEY` / `CS_TESTNET_STARTER_KEY`); never enable it
for binaries meant for mainnet. There is no automated linter:
follow `codingstyle.md`.

## CI

On every pull request to `master` these must pass: Linux build and unit tests (`build.yml`), Windows
and macOS builds (`build-platforms.yml`). Also on PRs, by path: `release.yml` (packaging),
`docker.yml` (`packaging/docker`), `samples.yml` (`samples/`). The private test network
(`testnet.yml`, 7 nodes by default, contracts, restarts, pruning) runs nightly and on a PR labelled
`testnet`. Other workflows run on demand or on test tags (`robustness-*`, `scale-*`, `setup-test-*`,
`upgrade-test-*`, `mainnet-join-*`, `platform-sync-*`): robustness scenarios, 5 to 61 node networks,
the guided set-up on the three systems, upgrades from the official node on mainnet data. macOS
jobs sometimes end "cancelled" because a runner was not acquired: rerun them.

## Conventions

- One topic per pull request, against `master`. Commit subjects start with the part they change
  (`csnode:`, `api:`, `ci:`, `docs:`, `packaging:`...).
- Every merged PR adds a row to `docs/CHANGES.md` under "Not released yet" (`| #N | change |`);
  the row usually comes in a second commit once the PR number is known.
- `docs/differences.md` lists every behaviour that differs from the official node: update it with
  any such change.
- A feature that needs configuration updates `packaging/setup/setup.sh`, `packaging/setup/setup.ps1`
  (`setup.bat` only starts it) and the set-up test (`.github/workflows/setup-test.yml`) in the same PR.
- Network-facing features go to the private test network (`testnet.yml`) before mainnet.
- Release tags `community-v4.2.532-community.N` publish packages and the Docker image
  (`release.yml`); only maintainers create them. Do not push tags that trigger publication.
- Docs, comments and commit messages in English.

## Git pitfalls

- Never `git add -A` or `git add .`: stage explicit paths. Stale submodule checkouts get committed
  by accident. Before committing, check that no gitlink changed: `git diff --cached --raw` must show
  no `160000` entry unless the PR is meant to bump that submodule.
- Never delete branches after a merge.
- `git submodule update --init` resets submodules to the pinned commits; `p2p` is pinned to a commit
  of its `community/p9` branch, not its `master`.

## Other pitfalls

- The official `config.ini` must keep working unchanged: new features are opt-in.
- The node checks the executor's build number against `executor_commit_min`/`executor_commit_max`
  in `config.ini` (official: 1518..1518); community executor builds count higher.
- Contract results must stay identical to executor build 1518; `ci/contracts/executor_harness.py`
  compares two executors call by call.

## More

- `README.md`, `docs/quick-start.md`, `docs/FEATURES.md`, `docs/differences.md`, `docs/CHANGES.md`
- `docs/storage.md` (backends, pruning, conversion), `docs/snapshots.md`, `docs/tokens.md`
- `docs/consensus-diagnostics.md`, `samples/contracts/README.md`, `samples/clients/README.md`
- Maintainers' agents: a shared Basic Memory knowledge base (project `credits`) holds platform facts,
  procedures and incident notes; use it when your environment provides it.
