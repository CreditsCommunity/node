# Quick start: run a Credits node

A short, practical guide to get a community node running on mainnet. Pick one route
(Docker or the native package) and one profile (pruned or full), then follow the steps.

For the details behind each step, see [storage.md](storage.md), [snapshots.md](snapshots.md) and
[packaging/docker/README.md](../packaging/docker/README.md).

## 1. Pick a profile

| | Pruned node (recommended) | Full node |
|---|---|---|
| What it keeps | Only recent blocks | Every block since block 0 |
| Good for | Running a node and taking part in the network | Full history: explorers, wallet histories from the start |
| Database | `lmdb` | `rocksdb` (the smallest for a full node) |
| First start | Minutes, from the published snapshot | Minutes from the snapshot, then the older blocks download in the background for about 2 to 3 days |

Words used below:

- **Snapshot**: a signed copy of the chain state at a recent block. The node checks it and starts
  from it, instead of syncing from block 0 (about 35 hours).
- **Serving snapshots** (`serve=on`): your node shares its own snapshots with new nodes, so they can
  start fast too. It uses at most 2 MB/s of upload and about 200 MB of disk. Please leave it on.
- **Backfill**: a full node started from a snapshot fetches the older blocks from other nodes, down
  to block 0, while it already runs normally. It can be turned off.

## 2. Requirements

| | Pruned node | Full node |
|---|---|---|
| CPU | 2 cores | 4 cores or more (the backfill checks signatures on several threads) |
| Memory | 4 GB (8 GB recommended) | 8 GB |
| Disk | about 10 GB | about 250 to 300 GB with RocksDB (400 GB or more with LMDB or BerkeleyDB); an SSD |
| Network | 3 Mbit/s | 5 Mbit/s or more |

Systems: Linux x86-64, Windows 10 or later (x64), macOS on Apple Silicon, or any system with Docker.

**Ports:**

| Port | Must be | What |
|---|---|---|
| 9510 TCP and UDP | **open** to the internet (firewall and router) | p2p: the other nodes reach yours here |
| 9090 | **closed** to other hosts (localhost only) | the node's API, used by the wallet |
| 9088 | **closed** to other hosts | the diagnostic API (on by default in the packages: the wallet uses it to check the network) |
| 9080, 9070 | **closed** to other hosts | the smart-contract executor |

The node listens on 9090 and 9088 on every interface: block them in the firewall. Docker publishes
9090 on `127.0.0.1` only.

## 3. Docker (any system with Docker)

1. Download the compose file and the settings file:

   ```sh
   mkdir credits && cd credits
   curl -fsSLO https://raw.githubusercontent.com/CreditsCommunity/node/master/packaging/docker/docker-compose.yml
   curl -fsSL -o .env https://raw.githubusercontent.com/CreditsCommunity/node/master/packaging/docker/.env.example
   ```

2. Edit `.env` for your profile.

   **Pruned** (the defaults already do this):

   ```ini
   CREDITS_STORAGE=pruned
   CREDITS_SNAPSHOT=github
   CREDITS_SERVE_SNAPSHOT=on
   CREDITS_API=on
   ```

   **Full:**

   ```ini
   CREDITS_STORAGE=full
   CREDITS_DB_BACKEND=rocksdb
   CREDITS_SNAPSHOT=github
   CREDITS_SERVE_SNAPSHOT=on
   CREDITS_API=on
   CREDITS_CPUS=4
   CREDITS_MEMORY=8g
   # CREDITS_BACKFILL=off   # uncomment to keep only the snapshot's blocks
   ```

   `rocksdb` is the default for a full node; the line only makes it explicit. (`berkeleydb`, the
   official node's format, cannot start from a snapshot and syncs from block 0.) `CREDITS_API=on`
   is needed by the wallet. `CREDITS_CPUS` and `CREDITS_MEMORY` set the container's limits in
   `docker-compose.yml`: the defaults (1.5 cores, `4g`) fit a pruned node, a full node needs 4
   cores and 8 GB.

   `.env` is read on the **first start only**, to write `~/.credits/config.ini`. After that, edit
   `config.ini`.

3. Make the data folder writable by the container (it runs as uid 1000), then start:

   ```sh
   mkdir -p ~/.credits && sudo chown -R 1000 ~/.credits
   docker compose up -d
   docker compose logs -f        # Ctrl+C leaves the logs, the node keeps running
   ```

4. **Back up `~/.credits/keys/`** (the node's identity) and `~/.credits/wallet/` if you use the
   wallet.

Everyday commands:

| Task | Command |
|---|---|
| Logs | `docker compose logs -f` |
| Stop | `docker compose stop` (it waits up to 120 s, so the node saves its state) or `docker stop -t 120 credits-node` |
| Start again | `docker compose up -d` |
| Upgrade | `docker compose pull && docker compose up -d` (data, `config.ini` and keys stay) |
| Wallet status | `docker compose exec node credits-wallet status` |
| Start over | set `CREDITS_RESET=1` once in `.env` (old data is moved to `~/.credits/old-<date>`), then remove it |

The data lives in `~/.credits` (`CREDITS_HOME` in `.env`): `config.ini`, `keys/`, `db/`,
`caches/`, `qs/`, `log/`, `wallet/`.

Never stop the container with the default `docker stop` (10 seconds): it is too short to save the
caches.

## 4. Native on Linux

### Download and check

1. Open the [latest release](https://github.com/CreditsCommunity/node/releases/latest) and download
   the Linux package and `SHA256SUMS`:
   - `credits-node-4.2.532-community.18-linux-x64.tar.gz` for Ubuntu 24.04, Debian 13 and later
     (glibc 2.38);
   - `credits-node-4.2.532-community.18-linux-x64-glibc2.31.tar.gz` for Ubuntu 20.04, Debian 11
     and later.

2. Check and extract:

   ```sh
   sha256sum -c --ignore-missing SHA256SUMS      # must print "OK"
   tar -xzf credits-node-4.2.532-community.18-linux-x64.tar.gz
   cd csnode
   ```

Run everything as a normal user, not as root.

### Set up and start in tmux

`tmux` keeps the node running after you close the SSH session (`sudo apt install tmux` if missing).
The set-up can start the node at the end, so run it inside tmux:

```sh
tmux new -s credits
./setup.sh
```

The set-up asks a few questions. Enter keeps the default shown in brackets.

| # | Question | Pruned | Full |
|---|---|---|---|
| 1 | Pruned or full node? | `pruned` | `full` |
| 2 | Database? | `lmdb` (default) | `rocksdb` (default) |
| 3 | How should the node get the chain? | `github` (default) | `github` (default) |
| 3b | Fetch the older blocks? (full only) | not asked | `yes` (default); `no` keeps only the snapshot's blocks |
| 4 | Share snapshots with other nodes? | `yes` (default) | `yes` (default) |
| 5 | Which executor? | `community` (default with this package) | `community` |
| 6 | The wallet's account? | `node` (default): the node's own key | `node` |
| 7 | Keep a token cache? | `no` (default), or `yes` to see every token in the wallet | same |
| 8 | Start the node when the set-up is done? | `yes` | `yes` |

Then it shows a summary and asks `Write config.ini?`: answer `yes`. With `github` it downloads and
checks the snapshot (a few minutes), and starts the node in the same window.

If the set-up creates the node's key from a 24-word phrase, **write the words on paper**.

**Detach and reattach:**

- leave the node running: press `Ctrl+B`, then `D`;
- come back later: `tmux attach -t credits`.

**Start it again later** (in tmux, in the `csnode` folder):

```sh
./node --config-file config.ini
```

**Stop it cleanly:** press `Ctrl+C` **once** and wait until the prompt comes back. Saving the
caches can take a minute. Do not press it again, and do not kill the process or close the tmux
window meanwhile.

### Optional: run it as a systemd service

The Linux package has `credits-node.service`. It runs the node at boot and restarts it on failure.

1. Stop the node if it runs in tmux.
2. Copy the unit and edit it:

   ```sh
   sudo cp credits-node.service /etc/systemd/system/
   sudo nano /etc/systemd/system/credits-node.service
   ```

   Set `User=` to your user, and put the full path of your `csnode` folder in `WorkingDirectory=`
   and `ExecStart=`.
3. Enable and start it:

   ```sh
   sudo systemctl daemon-reload && sudo systemctl enable --now credits-node
   journalctl -u credits-node -f              # console output
   sudo systemctl stop credits-node           # clean stop (it waits up to 5 minutes)
   ```

The node's own log is in `csnode/log/`.

## 5. Native on Windows

1. Download `credits-node-4.2.532-community.18-windows-x64.zip` and `SHA256SUMS-windows-macos`
   from the [latest release](https://github.com/CreditsCommunity/node/releases/latest). Check it in
   PowerShell and compare with the file:

   ```powershell
   Get-FileHash credits-node-4.2.532-community.18-windows-x64.zip
   ```

2. Extract the zip, for example to `C:\credits`. Open the `csnode` folder.
3. **Double-click `setup.bat`.** It runs `setup.ps1` for you (no change to the PowerShell execution
   policy is needed). Answer the questions as in the [Linux table](#set-up-and-start-in-tmux).
4. At the end the node starts in the same window. **Keep that window open**: closing it stops the
   node.
5. To start it again later, open `cmd` or PowerShell in the `csnode` folder:

   ```bat
   node.exe --config-file config.ini
   ```

6. To stop it, press `Ctrl+C` once and wait until it exits.

The package has no Windows service. Run the node in a console window you keep open.

## 6. macOS (Apple Silicon)

Download `credits-node-4.2.532-community.18-macos-arm64.tar.gz`, check it with
`shasum -a 256 -c --ignore-missing SHA256SUMS-windows-macos`, extract it, then clear the
download quarantine once (the binaries are not signed) and follow the Linux steps:

```sh
tar -xzf credits-node-4.2.532-community.18-macos-arm64.tar.gz
xattr -dr com.apple.quarantine csnode
cd csnode && ./setup.sh
```

## 7. First checks

In the console or the log (`csnode/log/`, or `docker compose logs -f`), look for:

1. The banner with the version and your settings:

   ```
     node 4.2.532-community.18  (protocol 532, build ...)
     storage: pruned, lmdb    snapshots: serve on
   ```

2. `checkpoint loaded` and `Storage: quick start`: the node started from the snapshot.
3. The status lines: the round number (`R-...`) and `Last written sequence = ...` grow every few
   seconds. A full node with backfill also shows `, oldest stored = ...`, which goes down.
4. A full node logs `BACKFILL: full node with blocks N..M: fetching 0..N-1`, a line every 100,000
   blocks, and at the end `BACKFILL: full history stored`.

Then ask the node with the wallet, in the `csnode` folder:

```sh
./credits-wallet --config wallet.toml status
```

It shows the network, the last block and `synced`. Wait for `synced: yes` before sending anything.

## 8. The wallet in two lines

Open it with `./credits-wallet --config wallet.toml` in `csnode` (`credits-wallet.exe` on Windows;
`docker compose exec node credits-wallet` in Docker). The node's own key is the account `node`:
it receives the node's rewards and delegations; keep savings in a separate account.

Full guide: `wallet-guide.md` in the package, or
[docs/guide.md](https://github.com/CreditsCommunity/wallet/blob/main/docs/guide.md). On mainnet
every transfer is real.

## 9. Upgrading and keeping the database

Docker: `docker compose pull && docker compose up -d`. Native:

1. **Stop the node cleanly**: `Ctrl+C` once and wait for the prompt, or
   `sudo systemctl stop credits-node`.
2. **Extract** the new package into a **new** folder.
3. **Copy** from the old `csnode` folder into the new one: `db/`, `caches/`, `qs/`,
   `NodePrivate.txt`, `NodePublic.txt`, `config.ini` (and `known_hosts.txt` if you edited it).
4. **Run the set-up** in the new folder and give the **same storage answers** as before: pruned or
   full, and the same database. The node must open the database with the backend that wrote it.
5. **Start the node.** The log shows the new version, then `checkpoint loaded` and
   `Storage: quick start`, and the node carries on from its last block.

Never run the old and the new node at the same time with the same key or the same `db/` folder.

## 10. Using your existing node key

Your node's identity is its key: `NodePrivate.txt` and `NodePublic.txt`. To keep an identity you
already have, give the node those two files instead of letting it make new ones. First check which
kind of `NodePrivate.txt` you have:

```sh
wc -c NodePrivate.txt      # about 88 characters: plain; about 120: encrypted with a password
```

A **plain** key works everywhere. A key **encrypted by the node** (`--encryptkey`, or the password
choice at its first start) makes the node ask for the password at every start, on its console.

Before you start:

- **Never run two nodes with the same key**, not even for a moment. Stop every node that uses
  this key first, including an official node or one on another machine: the node can only tell
  a community node running in the same folder.
- **Keep an offline backup** (USB stick, paper) of the original files. A plain key is as exposed
  as the machine it is on.
- The wallet uses a plain node key as the account `node` without a password. On mainnet it still
  asks you to type `send` before every transfer.

### Native, plain key

Put `NodePrivate.txt` and `NodePublic.txt` in the `csnode` folder **before** you run `./setup.sh`
(`setup.bat` on Windows). The set-up finds them, offers the key as the wallet's account `node`
(answer `node`), and sets `NodePrivate.txt` to mode 600.

### Native, encrypted key

Same place, same set-up. At every start the node asks:

```
The key file seems to be encrypted
To use password for encrytion/decryption press "p", to use file press "f": p
Enter password: ********
```

This works in tmux or in a Windows console window you type into. It does **not** work under
systemd or any start without a console: the node cannot ask and does not start. Either keep it
in tmux, or convert the key to plain once with `./node --decryptkey`
([below](#convert-an-encrypted-key-to-plain)). The
wallet opens an encrypted key with `--node-key` or as the account `node`, and asks the node's
password; it needs about 1 GB of free memory for that.

### Docker, plain key

1. Stop the node, and save the keys the container may have made at its first start:

   ```sh
   docker compose stop
   sudo cp -a ~/.credits/keys ~/.credits/keys.generated-$(date +%F)
   ```

2. Copy your two files in, owned by the container's user (uid 1000), mode 600:

   ```sh
   sudo cp NodePrivate.txt NodePublic.txt ~/.credits/keys/
   sudo chown 1000:1000 ~/.credits/keys/NodePrivate.txt ~/.credits/keys/NodePublic.txt
   sudo chmod 600 ~/.credits/keys/NodePrivate.txt ~/.credits/keys/NodePublic.txt
   ```

3. Start it and check the key it runs with:

   ```sh
   docker compose up -d
   docker compose exec node credits-wallet account node-key
   ```

   `address` must be your `NodePublic.txt`, with `public file : ... matches`. The log must not
   show `new node keys` nor ask for a password. (The node does not print its own key in the log.)

On a **new** installation you can put the two files in `~/.credits/keys/` (with the `chown` and
`chmod` above) before the first `docker compose up -d`: the container makes new keys only when
`keys/NodePublic.txt` is missing. Always copy both files.

### Docker, encrypted key

The container cannot type a password at every start: with an encrypted key it stops with
`NOT STARTING: keys/NodePrivate.txt is encrypted`. Turn the key into the plain key once, with the
node of the image (community.18 or later); it asks the old password and needs no network. Opening
the key takes about 1 GB of free memory for a few seconds, so `CREDITS_MEMORY` in `.env` must be at
least about `1.2g` (the default is `4g`). Until the key is converted, `docker compose up -d` keeps
restarting the container with that message: stop it with `docker compose stop`.

1. Stop the node and save the keys it may have made (step 1 above).
2. Copy your two files in, as in step 2 above.
3. Decrypt and start:

   ```sh
   docker compose run --rm node decrypt-key        # press p, Enter, then type the old password
   docker compose up -d
   ```

   It prints `keys/NodePrivate.txt is now in clear (address <your public key>); the encrypted
   original is kept as keys/NodePrivate.txt.encrypted-<date>-<time>`. A wrong password prints
   `Incorrect password (or corrupted file)` and asks again. If `NodePublic.txt` belongs to another
   key, it changes nothing and says so: copy the two files of the same node. The password can
   also come from a pipe (`printf 'p\n%s\n' "$PW" | docker compose run --rm -T node decrypt-key`).

4. Check, as in step 3 for a plain key. Then move the `.encrypted-...` copy offline (or delete it
   once you have an offline backup).

### Convert an encrypted key to plain

Native, for example to run the node under systemd (node community.18 or later): in the `csnode`
folder, with the node **stopped** and the encrypted `NodePrivate.txt` and `NodePublic.txt` in
place:

```sh
./node --decryptkey             # Windows: node.exe --decryptkey
```

Press `p`, Enter, and type the old password. The node writes the plain key in place (mode 600),
keeps the encrypted file as `NodePrivate.txt.encrypted-<date>-<time>`, prints the address and
exits without starting. It needs no `config.ini`, network or database, and refuses while a node
runs in the folder (a community node; stop an official node yourself). A key that is already
plain is left as it is. Other key files: `--private-key-file` and `--public-key-file`. It needs
about 1 GB of free memory for a few seconds; without it, it says so and changes nothing. The node
then starts without asking anything.

On Windows, type the password in a console window (`node.exe` reads it from the console, not from
a pipe). The plain key and the `.encrypted-...` copy keep the folder's permissions (Windows has no
mode 600): keep the `csnode` folder readable by your user only.

## 11. Troubleshooting

| Problem | What to do |
|---|---|
| Few or no neighbours, the node never syncs | Port 9510 (TCP and UDP) is not reachable: open it in the firewall and forward it on the router. |
| Docker: permission denied on `/data`, or the wallet ignores the node's key | The container runs as uid 1000: `sudo chown -R 1000 ~/.credits`, and keep `keys/NodePrivate.txt` at mode 600. |
| The node runs but no smart contract executes | The executor build is outside `executor_commit_min..executor_commit_max` in `config.ini`. Run the set-up again, or set `executor_commit_max=1558` or higher. Copying only the node binary keeps the old executor. |
| `the block database is X, config.ini asks for Y` (exit code 3) | `config.ini` names another database than the one in `db/`. Set `db_backend` back to X, or convert on purpose with `./node --convert-db` (Docker: `CREDITS_CONVERT_DB=yes`). See [storage.md](storage.md#conversion). |
| Snapshot download stopped | Run it again: `./node --download-snapshot` (node stopped) resumes, or run the set-up again. |
| The node stops with exit code 4, `snapshot.mismatch` exists | The restored state did not match the snapshot. Start over with an empty `db/`, `caches/` and `qs/` (Docker: `CREDITS_RESET=1` once). |
| Disk full | A full node needs 250 to 300 GB with RocksDB. Free space or move to a bigger disk, or turn the backfill off (`backfill=off` in `[storage]`, `CREDITS_BACKFILL=off` in Docker before the first start). |
| Linux: `GLIBC_2.38 not found` | Use the `linux-x64-glibc2.31` package. |
| macOS: "cannot be opened" | `xattr -dr com.apple.quarantine csnode` |

Help: [issues](https://github.com/CreditsCommunity/node/issues) or the
[technical Telegram](https://t.me/creditstechnical). Never share your 24 words or `NodePrivate.txt`.
