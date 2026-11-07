# Credits node in Docker

```sh
mkdir credits && cd credits
curl -fsSLO https://raw.githubusercontent.com/CreditsCommunity/node/master/packaging/docker/docker-compose.yml
curl -fsSL -o .env https://raw.githubusercontent.com/CreditsCommunity/node/master/packaging/docker/.env.example
docker compose up -d && docker compose logs -f
```

A pruned node (the default) starts from a snapshot instead of syncing from block 0 for days. On its first start, with `CREDITS_SNAPSHOT=github` (the default), the container runs `node --download-snapshot`: it downloads the latest snapshot published on GitHub, checks every file against its signed manifest and restores it. When that fails, the node looks for a snapshot among its p2p neighbours (the community nodes that serve one) and, after `fetch_wait` (10 minutes) without one, syncs from block 0. `CREDITS_SNAPSHOT=p2p` skips the GitHub download, `off` syncs from block 0. The node checks a restored snapshot's state digest at its first start and stops if it differs.

A full node (`CREDITS_STORAGE=full`) on rocksdb, its default, or on lmdb (`CREDITS_DB_BACKEND=lmdb`) starts from the snapshot the same way. Then it fetches the older blocks from its neighbours in the background, down to block 0 (backward sync). It takes part in the network meanwhile, and the API serves each block and history as it arrives. On mainnet that is about 186 million blocks: about 2 to 3 days (more on a slow disk or few cores), and the disk of a full node, about 250 to 300 GB with rocksdb or 400 GB or more with lmdb. `CREDITS_BACKFILL=off` keeps only the snapshot's blocks. A full node on berkeleydb (`CREDITS_DB_BACKEND=berkeleydb`, the official node's format) syncs from block 0, as the official node does: a berkeleydb database cannot start above block 0. Until community.17 the full default was berkeleydb; an existing `config.ini` keeps the backend it names. The container's limits in `docker-compose.yml` (`CREDITS_CPUS`, default 1.5, and `CREDITS_MEMORY`, default `4g`, in `.env`) fit a pruned node: for a full node set `CREDITS_CPUS=4` and `CREDITS_MEMORY=8g`. See `docs/storage.md`, "Backward sync".

The node also serves its own snapshots to its neighbours (`CREDITS_SERVE_SNAPSHOT=on`, the default; at most 2 MB/s, half while it is a confidant). See `docs/snapshots.md`.

## Data: `~/.credits`

| Path | What |
| --- | --- |
| `config.ini` | Written from `.env` on the first start, then yours to edit. It wins over `.env` afterwards, which logs a warning if the two differ. |
| `keys/` | The node's identity. **Back it up.** The keys never go into `.env`, the logs or a snapshot. |
| `db/`, `caches/`, `qs/`, `*.dat` | Blocks, caches and checkpoints. |
| `log/` | Node logs (10 × 50 MB) and executor logs (kept 7 days). |
| `wallet/` | The wallet's key files (encrypted), configuration and lists. **Back it up**, with the 24-word phrases on paper. |
| `snapshot.json`, `snapshot.verified` | The snapshot this node started from, and its digest check at the first start. |
| `snapshot.mismatch` | The restored state did not match the snapshot: the node does not start until it is removed (set `CREDITS_RESET=1` once to start over). |
| `snapshot/` | The snapshots this node builds and serves to its neighbours. |
| `snapshot-download/` | A download in progress; the next first start resumes it. |

The container runs as uid 1000: the directory must be writable by it (`sudo chown -R 1000 ~/.credits`), or set `user:` in the compose file to your own uid. Keep `user:` a non-root uid: the node, its keys and the wallet's key store must not run or be written as root.

## The wallet

The image has the terminal wallet, `credits-wallet`, on the `PATH`. Its keys and configuration go
to `/data/wallet` (`~/.credits/wallet` on the host): back that folder up with `keys/`. The wallet
talks to the node's API, so set `CREDITS_API=on`; `CREDITS_TOKENS_CACHE=on` lets it list every
token an account holds (both are written to `config.ini` on the first start only: later, edit
`port` and `tokens_cache` in its `[api]` section). Then:

```sh
docker compose exec node credits-wallet            # the terminal interface
docker compose exec node credits-wallet status
```

The node's own key, `keys/NodePrivate.txt`, is the wallet's account `node` (marked NODE KEY, the
default): the address of the node's rewards and of the delegations to it. The wallet reads it in
place (`CREDITS_WALLET_NODE_KEY`), without copying or encrypting it: it is in clear on the volume,
as the node needs it, so keep on it only what the node needs and savings in a separate account
(`credits-wallet account new NAME`). The wallet uses the key only if it and `keys/` belong to the
container's user (uid 1000) and nobody else can write to them: with a bind mount, `sudo chown -R
1000 ~/.credits` (above), and keep `keys/NodePrivate.txt` at mode 600.

On mainnet every transfer is real. The user guide is `/opt/credits/wallet-guide.md` in the image,
and [docs/guide.md](https://github.com/CreditsCommunity/wallet/blob/main/docs/guide.md) of the wallet.

## Changing the storage

| Change | Result |
| --- | --- |
| Full to pruned, same database | Allowed: old blocks are removed at the next checkpoints. |
| Pruned to full | Allowed: the node keeps its blocks and fetches the pruned ones from its neighbours in the background (backward sync). |
| A different database (`db_backend`) | Refused, unless `CREDITS_CONVERT_DB=yes` is set (see below). |

To start over, set `CREDITS_RESET=1` once. The old data is moved to `~/.credits/old-<date>`, not deleted.

### Changing the block database

To move an existing database to another backend, set `db_backend` in `config.ini` to the new one and set `CREDITS_CONVERT_DB=yes` in `.env`. Stop the node before converting. The node converts the database in place at the next start; it needs about 1 GB free beyond the database when converting from berkeleydb; about the size of the database again when converting from lmdb (its files do not shrink until the conversion ends); up to 4 times the size of the database when converting from rocksdb, which compresses its blocks. The node checks the free space first and names what it needs. It resumes if interrupted. Before converting from berkeleydb, keep a copy of `~/.credits/db`, taken with the node stopped right before the conversion (it takes as much space as `db/`; another disk is fine): a crash during a BerkeleyDB compaction can leave the old database unopenable while blocks are still to convert, and the copy is then the way back (see `docs/storage.md`, Interruptions). Remove `CREDITS_CONVERT_DB` afterwards.

## Upgrades

```sh
docker compose pull && docker compose up -d
```

The data, `config.ini` and the keys stay.
