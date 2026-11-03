<img src="https://raw.githubusercontent.com/CREDITSCOM/Documentation/master/Src/Logo_Credits_horizontal_black.png" align="center">

[Documentation](https://developers.credits.com/en/Articles/Platform) \|
[Guides](https://developers.credits.com/en/Articles/Guides) \|
[CREDITS website](https://credits.com/)

[![Build](https://github.com/CreditsCommunity/node/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/CreditsCommunity/node/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/CreditsCommunity/node?label=Community%20release&style=plastic)](https://github.com/CreditsCommunity/node/releases/latest)
[![AGPL License](https://img.shields.io/github/license/CREDITSCOM/node.svg?color=green&label=License&style=plastic)](LICENSE)
[![Twitter](https://img.shields.io/twitter/follow/creditscom.svg?label=Follow&style=social)](https://twitter.com/intent/follow?screen_name=creditscom)
# Credits Node (community)
The node is the program that runs on the operator's machine and takes part in the CREDITS network.
Written in C++.

## Version
This repository is the community continuation of [CREDITSCOM/node](https://github.com/CREDITSCOM/node), forked from the official release 4.2.532. Community releases are numbered `4.2.532-community.N`. They keep protocol version 532 and stay compatible with the mainnet.

| | |
|---|---|
| What the community node adds, in short | [docs/FEATURES.md](docs/FEATURES.md) |
| What changes from the official node | [docs/differences.md](docs/differences.md) |
| Every change, pull request by pull request | [docs/CHANGES.md](docs/CHANGES.md) |
| Block database, pruned storage, conversion | [docs/storage.md](docs/storage.md) |
| Tokens, the token API and the token tests | [docs/tokens.md](docs/tokens.md) |
| Example API clients (Go, Java, Rust, Python, C#, JavaScript): status, balance, blocks, transactions, a signed transfer, deploying and calling contracts | [samples/clients/](samples/clients/) |
| Smart contract examples (a counter, a test token, a payable tip jar, a faucet that reads its own balance): how they are written, deployed and called | [samples/contracts/](samples/contracts/) |
| Docker (pruned node from a snapshot, or a full node from a snapshot that fetches the older blocks in the background) | [packaging/docker/README.md](packaging/docker/README.md) |
| A full node from a snapshot without Docker (backward sync) | [docs/storage.md](docs/storage.md#a-full-node-from-a-snapshot) |
| Packages (Linux x86-64, Windows x64, macOS Apple Silicon; `config.ini` full and `config.pruned.ini` pruned, one package per system) | [Releases](https://github.com/CreditsCommunity/node/releases) |
| Guided set-up in every package: pruned or full, database, first start, snapshot sharing, contract executor (`csnode/setup.sh` on Linux and macOS, `csnode/setup.bat` on Windows) | [packaging/setup/](packaging/setup/) |

Replacing the official binaries with the community ones, with the official `config.ini`, keeps the same BerkeleyDB database and the same behaviour on the network. The new features (other block databases, pruned storage, conversion, snapshots, backfill) are opt-in.

## What is Credits Node?
Credits Node is the program that runs a node of the CREDITS blockchain network. It processes and
stores transactions, executes and confirms smart contracts, answers requests from third-party
systems, and provides data on request. Every node on the network has the same functions. Its main
parts are:

- the API;
- the decision-making module (Solver);
- the storage (CSDB);
- the transport protocol.

## Running a node
Most operators do not need to build: download the package for your system from the
[Releases](https://github.com/CreditsCommunity/node/releases), unpack it and run the guided set-up
(`csnode/setup.sh` on Linux and macOS, `csnode/setup.bat` on Windows), or use the
[Docker image](packaging/docker/README.md).

Indicative requirements:

| | Pruned node | Full node |
|---|---|---|
| Operating system | Linux x86-64 (glibc 2.31 or later: Ubuntu 20.04, Debian 11 and later), Windows 10 or later (x64), macOS on Apple Silicon | same |
| CPU | 2 cores | 4 cores or more (the backfill checks signatures on several threads) |
| Memory | 4 GB (8 GB recommended) | 8 GB |
| Disk | about 10 GB | about 200 to 250 GB with RocksDB, 400 GB or more with LMDB or BerkeleyDB; an SSD |
| Network | 3 Mbit/s | 5 Mbit/s or more |

See [docs/storage.md](docs/storage.md) for the storage options.

## The wallet
From release community.17 on, every package and the Docker image ship
[credits-wallet](https://github.com/CreditsCommunity/wallet), a terminal wallet that runs next to the
node: keys kept on the machine (encrypted), transfers, delegations, tokens, history and smart
contracts, signed in the wallet and sent through the node's API. It opens no port.

- In the package: `csnode/credits-wallet` (`credits-wallet.exe` on Windows), with its user guide
  `wallet-guide.md` (in Italian `wallet-guide.it.md`). In Docker: `credits-wallet` on the `PATH`, its
  data in `/data/wallet` (set `CREDITS_API=on`; see [packaging/docker/README.md](packaging/docker/README.md)).
- The guided set-up asks whether to create or import an account and writes `wallet.toml`, the
  wallet profile of the node it configured (its API port, its diag port, mainnet). It also offers the
  token cache (`[api] tokens_cache=true`, [docs/tokens.md](docs/tokens.md)), with which the wallet
  lists every token an account holds.
- The wallet asks the node's diagnostic API (`[api] diag_port`, 9088) which network it is on, and
  refuses to send when that contradicts its profile. The node listens on that port, as on the API
  port, on every interface: keep 9088 and 9090 closed to other hosts. The set-up never turns the diag
  port on: with `diag_port=0` the wallet works without that confirmation.
- The package ships the wallet release named in [packaging/wallet-tag](packaging/wallet-tag),
  downloaded from its public release and checked against its `SHA256SUMS`
  ([packaging/fetch-wallet.sh](packaging/fetch-wallet.sh)). A node release ships a final wallet
  release: tag the wallet first, then bump `packaging/wallet-tag`, then release the node.

## Building from source
The builds that CI runs on every change are the reference:
[build.yml](.github/workflows/build.yml) (Linux) and
[build-platforms.yml](.github/workflows/build-platforms.yml) (Windows with Visual Studio 2022,
macOS on Apple Silicon).

Requirements: CMake 3.12 or later, a C++17 compiler (GCC 13 on Ubuntu 24.04 is what CI uses),
[Boost 1.83](https://www.boost.org/users/history/version_1_83_0.html) built as static libraries with
a static runtime, flex, bison, autoconf, automake and libtool (for Apache Thrift and BerkeleyDB).

On Linux (Ubuntu 24.04):

```sh
sudo apt install build-essential cmake ninja-build flex bison autoconf automake libtool pkg-config

# Boost, static with a static runtime
curl -sSfLO https://archives.boost.io/release/1.83.0/source/boost_1_83_0.tar.bz2
tar xjf boost_1_83_0.tar.bz2 && cd boost_1_83_0
./bootstrap.sh --with-libraries=system,filesystem,program_options,date_time,regex,log,iostreams,thread,serialization,atomic,chrono
./b2 -j"$(nproc)" variant=release link=static runtime-link=static threading=multi cxxflags=-fPIC \
  --layout=tagged -sNO_BZIP2=1 -sNO_ZSTD=1 -sNO_LZMA=1 install --prefix="$HOME/boost"
cd ..

# the node
git clone https://github.com/CreditsCommunity/node.git
cd node
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBoost_ROOT="$HOME/boost" -DBoost_NO_SYSTEM_PATHS=ON \
  -DCMAKE_CXX_FLAGS="-include cstdint -include cstddef" -DWITH_HASKELL=OFF
cmake --build build -j"$(nproc)"
```

The binary is `build/client/node`. Add `-DENABLE_TESTS=ON` to build the unit tests, then run
`ctest --test-dir build`.

## Contribution
Contributions are welcome. Fork the repository, make your change on a branch and open a pull
request against `master` of [CreditsCommunity/node](https://github.com/CreditsCommunity/node).
Please:

- follow the [coding style](codingstyle.md);
- prefix commit messages with the part they change (`csnode:`, `api:`, `ci:`...);
- add a line to [docs/CHANGES.md](docs/CHANGES.md);
- update the guided set-up (`packaging/setup/`) when a change adds or changes a setting an
  operator chooses.

Every pull request is built and tested on Linux, Windows and macOS.

### Resources

- [CREDITS website](https://credits.com/)
- [Documentation](https://github.com/CREDITSCOM/Documentation)
- [Whitepaper](https://github.com/CREDITSCOM/Documentation/blob/master/WhitePaper%20CREDITS%20Eng.pdf)
- [Technical paper](https://github.com/CREDITSCOM/Documentation/blob/master/TechnicalPaper%20CREDITS%20Eng.pdf)
- [Developers portal](https://developers.credits.com/)

### Community links

<a href="https://t.me/creditstechnical"><img src="https://cdn.simpleicons.org/telegram" height="40" width="40" alt="Telegram (technical)"></a>
<a href="https://t.me/creditscom"><img src="https://cdn.simpleicons.org/telegram" height="40" width="40" alt="Telegram"></a>
<a href="https://twitter.com/creditscom"><img src="https://cdn.simpleicons.org/x" height="40" width="40" alt="X (Twitter)"></a>
<a href="https://www.reddit.com/r/CreditsOfficial/"><img src="https://cdn.simpleicons.org/reddit" height="40" width="40" alt="Reddit"></a>
<a href="https://medium.com/@credits"><img src="https://cdn.simpleicons.org/medium" height="40" width="40" alt="Medium"></a>
<a href="https://www.facebook.com/creditscom"><img src="https://cdn.simpleicons.org/facebook" height="40" width="40" alt="Facebook"></a>
<a href="https://www.instagram.com/credits_com/"><img src="https://cdn.simpleicons.org/instagram" height="40" width="40" alt="Instagram"></a>
<a href="https://www.youtube.com/channel/UC7kjX_jgauCqmf_a4fqLGOQ"><img src="https://cdn.simpleicons.org/youtube" height="40" width="40" alt="YouTube"></a>
