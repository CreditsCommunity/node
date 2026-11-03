#!/usr/bin/env bash
# Guided set-up of the node in this folder: storage (pruned or full), database, how the first start
# gets the chain, whether a full node fetches the older blocks in the background (backfill), whether
# to share snapshots, which contract executor to run, a wallet account, the token cache. Writes the
# [snapshot] and [storage] sections of config.ini, the executor builds it accepts and tokens_cache
# (the previous file is kept as config.ini.bak-<date>), can replace the executor jars, writes
# wallet.toml (the wallet profile of this node), can create or import a wallet account with
# credits-wallet, and can start the node.
#
#   ./setup.sh            asks, showing each default: Enter keeps it
#   ./setup.sh --defaults takes every default without asking
#
# Runs with the bash of Linux and of macOS (3.2).
set -eu

cd "$(dirname "$0")"

# Never work as root in a folder another user controls: every file the set-up writes here
# (config.ini and its backup and temporary files, wallet.toml) could be a symlink planted by that
# user, and root would write through it. As root in a folder owned by another user, the set-up runs
# again as that user (sudo); in a root-owned folder that others can write to, it stops.
if [ "$(id -u)" = 0 ]; then
    owner_uid=$(ls -nd . | awk '{ print $3 }')
    owner_name=$(ls -ld . | awk '{ print $3 }')
    if [ "$owner_uid" != 0 ]; then
        command -v sudo >/dev/null 2>&1 \
            || { echo "This folder belongs to $owner_name: run the set-up as $owner_name, not as root."; exit 1; }
        echo "This folder belongs to $owner_name: the set-up continues as $owner_name."
        exec sudo -u "#$owner_uid" -H env CREDITS_SETUP_INVOKER="${SUDO_USER:-root}" \
            ${CREDITS_WALLET_HOME:+CREDITS_WALLET_HOME="$CREDITS_WALLET_HOME"} "$(pwd)/$(basename "$0")" "$@"
    fi
    case "$(ls -ld . | cut -c6,9)" in
        *w*) echo "This folder is owned by root but others can write to it: fix its permissions (chmod go-w) first."; exit 1 ;;
    esac
fi

defaults=no
case "${1:-}" in
    --defaults|-y) defaults=yes ;;
    "") ;;
    *) echo "usage: $0 [--defaults]"; exit 2 ;;
esac

node=./node
config=config.ini
[ -x "$node" ] || { echo "No node binary here: run setup.sh from the csnode folder of the package."; exit 1; }
[ -f "$config" ] || { echo "No $config here: run setup.sh from the csnode folder of the package."; exit 1; }

# ask "question" default choice... -> sets $answer to one of the choices (the first letters are enough)
ask() {
    local question=$1 default=$2 reply c
    shift 2
    while :; do
        if [ "$defaults" = yes ]; then
            answer=$default
            echo "$question [$default]: $default"
            return
        fi
        printf '%s [%s]: ' "$question" "$default"
        read -r reply || reply=
        reply=$(printf '%s' "$reply" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]')
        [ -n "$reply" ] || { answer=$default; return; }
        for c in "$@"; do
            case "$c" in "$reply"*) answer=$c; return ;; esac
        done
        echo "  Please answer one of: $*"
    done
}

# ask_text "question" default -> sets $answer to the reply, trimmed (the default for an empty one)
ask_text() {
    local question=$1 default=$2 reply
    if [ "$defaults" = yes ]; then
        answer=$default
        echo "$question [$default]: $default"
        return
    fi
    printf '%s [%s]: ' "$question" "$default"
    read -r reply || reply=
    reply=$(printf '%s' "$reply" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
    answer=${reply:-$default}
}

# the backend of an existing database (as the node recognises it), empty when there is none
existing_backend() {
    if [ -f db/.backend ]; then tr -d '[:space:]' < db/.backend; return; fi
    if [ -f db/blockchain.db ] || [ -f db/sequence.db ]; then echo berkeleydb; return; fi
    if [ -f db/lmdb/data.mdb ]; then echo lmdb; return; fi
    if [ -f db/CURRENT ]; then echo rocksdb; return; fi
}

# the value of key in [section] of the current config.ini, empty when not set
current() {
    awk -v s="[$1]" -v k="$2" '
        /^[[:space:]]*\[/ { in_s = ($0 ~ "^[[:space:]]*\\[" substr(s, 2, length(s) - 2) "\\]") ; next }
        in_s && $0 ~ "^[[:space:]]*" k "[[:space:]]*=" { sub(/^[^=]*=[[:space:]]*/, ""); sub(/[[:space:]]+$/, ""); v = $0 }
        END { print v }
    ' "$config" | tr -d '\r'
}

# --- contract executor ---------------------------------------------------------------------------
# The node runs smart contracts with contract-executor.jar (it loads sc-api-support.jar from the same
# folder) and starts it only if the build the jar reports (its git commit count) is within
# [executor_commit_min, executor_commit_max] of [api]; otherwise it stops the executor and runs no
# contract. The official release is build 1518; community builds add fixes on top and count higher.
official_build=1518
official_url=https://github.com/CREDITSCOM/node/releases/download/mainnet-4.2.532.0/Ubuntu20_Mainnet-4.2.532.0.tar.gz
official_sha_executor=592510d1586818348c65bea9b443e7f58aa6947113e43aa8906af8426aeef028
official_sha_support=395d9a108a66fe1d92c1d2096a9d11f4c7662b51b47af792db0ff4cc91df9807
community_url=https://github.com/CreditsCommunity/contract-executor/releases/latest/download

# the java of executor_command, to find the JDK's jar tool when neither unzip nor python3 is there
executor_java() {
    current api executor_command | awk '{ print $1 }'
}

# the build (git commit count) an executor jar reports, empty when it cannot be read
jar_build() {
    local jar=$1 props="" java tmp
    [ -f "$jar" ] || return 0
    if command -v unzip >/dev/null 2>&1; then
        props=$(unzip -p "$jar" git.properties 2>/dev/null || true)
    elif command -v python3 >/dev/null 2>&1; then
        props=$(python3 -c 'import sys, zipfile; print(zipfile.ZipFile(sys.argv[1]).read("git.properties").decode())' "$jar" 2>/dev/null || true)
    else
        java=$(executor_java)
        case "$java" in /*) ;; *) java="$(pwd)/$java" ;; esac
        if [ -n "$java" ] && [ -x "$(dirname "$java")/jar" ]; then
            tmp=$(mktemp -d)
            (cd "$tmp" && "$(dirname "$java")/jar" xf "$OLDPWD/$jar" git.properties 2>/dev/null) || true
            props=$(cat "$tmp/git.properties" 2>/dev/null || true)
            rm -rf "$tmp"
        fi
    fi
    printf '%s\n' "$props" | sed -n 's/^git.total.commit.count=//p' | tr -d '\r'
}

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{ print $1 }'; else shasum -a 256 "$1" | awk '{ print $1 }'; fi
}

describe_build() {
    case "$1" in
        "") echo "none or unreadable" ;;
        "$official_build") echo "build $1 (official)" ;;
        *) [ "$1" -gt "$official_build" ] 2>/dev/null && echo "build $1 (community)" || echo "build $1" ;;
    esac
}

# downloads the jars of the chosen executor into $1; prints the reason and fails when it cannot
download_executor() {
    local kind=$1 dir=$2
    command -v curl >/dev/null 2>&1 || { echo "   curl is not installed."; return 1; }
    if [ "$kind" = community ]; then
        curl -fsSL -o "$dir/contract-executor.jar" "$community_url/contract-executor.jar" \
            && curl -fsSL -o "$dir/sc-api-support.jar" "$community_url/sc-api-support.jar" \
            && curl -fsSL -o "$dir/SHA256SUMS" "$community_url/SHA256SUMS" \
            || { echo "   The download from GitHub failed."; return 1; }
        for f in contract-executor.jar sc-api-support.jar; do
            grep -q "^$(sha256 "$dir/$f")  *\*\{0,1\}$f\$" "$dir/SHA256SUMS" \
                || { echo "   $f does not match the SHA256SUMS of the release."; return 1; }
        done
    else
        echo "   The jars come from the official release package (about 350 MB to download, only they are kept)."
        curl -fsSL "$official_url" | tar -xzf - -C "$dir" csnode/contract-executor.jar csnode/sc-api-support.jar \
            || { echo "   The download of the official release failed."; return 1; }
        mv "$dir/csnode/contract-executor.jar" "$dir/csnode/sc-api-support.jar" "$dir/"
        [ "$(sha256 "$dir/contract-executor.jar")" = "$official_sha_executor" ] \
            && [ "$(sha256 "$dir/sc-api-support.jar")" = "$official_sha_support" ] \
            || { echo "   The jars of the official release do not have the expected checksums."; return 1; }
    fi
}

# sets key=value in [api] of config.ini, adding the key at the end of the section when missing
set_api_key() {
    local tmp="$config.tmp.$$"
    awk -v k="$1" -v v="$2" '
        /^[[:space:]]*\[/ { if (in_api && !done) { print k "=" v; done = 1 } in_api = ($0 ~ /^[[:space:]]*\[api\]/) }
        in_api && $0 ~ "^[[:space:]]*" k "[[:space:]]*=" { if (!done) print k "=" v; done = 1; next }
        { print }
        END { if (in_api && !done) print k "=" v }
    ' "$config" > "$tmp" && mv "$tmp" "$config"
}

echo "Credits node set-up ($(cat VERSION 2>/dev/null || echo 'unknown version'))"
echo "Each question shows the default in brackets: press Enter to keep it."
echo

have=$(existing_backend)
if [ -n "$have" ]; then
    echo "This folder already holds a $have database: it is kept."
    echo
fi

# defaults from the current configuration, so that running the set-up again changes nothing by Enter
cur_backend=$(current storage db_backend)
[ -n "$cur_backend" ] || cur_backend=$(current storage block_db)
cur_prune=$(current storage prune_keep_blocks)
cur_serve=$(current snapshot serve)

# 1. pruned or full
storage_default=pruned
if [ -n "$cur_backend$cur_prune" ] && { [ -z "$cur_prune" ] || [ "$cur_prune" = 0 ]; }; then storage_default=full; fi
[ "$have" != berkeleydb ] || storage_default=full
echo "1. Storage"
echo "   pruned: keeps only recent blocks (a few GB), starts in minutes from a snapshot. Enough to run a node."
echo "   full:   keeps every block since the start of the chain (tens of GB), for history and explorers."
ask "   Pruned or full node? (pruned/full)" "$storage_default" pruned full
storage=$answer
pruned_to_full=no
if [ "$storage" = full ] && [ -n "$have" ] && [ -n "$cur_prune" ] && [ "$cur_prune" != 0 ]; then
    echo "   This folder holds a pruned database: as a full node it keeps its blocks and fetches the ones"
    echo "   it removed from other nodes in the background (backfill, question 3)."
    pruned_to_full=yes
fi
echo

# 2. database
if [ "$storage" = pruned ]; then
    backend_default=lmdb
    [ "$cur_backend" != rocksdb ] && [ "$have" != rocksdb ] || backend_default=rocksdb
    echo "2. Database"
    echo "   lmdb:    the usual choice for pruned nodes."
    echo "   rocksdb: compressed, smaller on disk, a little more CPU."
    ask "   Database? (lmdb/rocksdb)" "$backend_default" lmdb rocksdb
else
    backend_default=berkeleydb
    case "$cur_backend" in lmdb|rocksdb) [ "$cur_prune" != "" ] && [ "$cur_prune" != 0 ] || backend_default=$cur_backend ;; esac
    [ -z "$have" ] || backend_default=$have
    echo "2. Database"
    echo "   berkeleydb: the official node's format (the official node can open this folder too)."
    echo "   rocksdb:    the smallest full node (about a third of berkeleydb)."
    echo "   lmdb:       fast, larger on disk."
    ask "   Database? (berkeleydb/rocksdb/lmdb)" "$backend_default" berkeleydb rocksdb lmdb
fi
backend=$answer
echo

convert=no
if [ -n "$have" ] && [ "$have" != "$backend" ]; then
    echo "   The database in this folder is $have, you chose $backend."
    if [ "$backend" = berkeleydb ] && [ "$storage" = pruned ]; then
        echo "   A pruned node cannot use berkeleydb."
    fi
    ask "   Convert it to $backend at the next start (it can take hours)? (yes/no)" yes yes no
    if [ "$answer" = yes ]; then
        convert=yes
    else
        backend=$have
        echo "   Keeping $have."
    fi
    echo
fi

# 3. how the first start gets the chain (only for an empty folder)
first=keep
if [ -z "$have" ]; then
    echo "3. First start"
    if [ "$backend" = berkeleydb ]; then
        echo "   A berkeleydb node syncs from block 0 (about 35 hours on mainnet)."
        first=sync
    else
        echo "   github:  download the snapshot published on GitHub now, check it and restore it (minutes)."
        echo "   network: the node fetches a snapshot from other nodes that share it; it needs at least two"
        echo "            of them, and few nodes share yet: without them it syncs from block 0 after 10 minutes."
        echo "   sync:    sync from block 0 by itself (about 35 hours on mainnet)."
        first_default=github
        if [ "$storage" = full ]; then
            echo "   A full node started from a snapshot runs in minutes and fetches the older blocks from"
            echo "   other nodes in the background (backfill, next question)."
        fi
        ask "   How should the node get the chain? (github/network/sync)" "$first_default" github network sync
        first=$answer
    fi
    echo
fi

# 3b. backfill: a full node that starts above block 0 (from a snapshot, or a pruned folder) fetches
# the older blocks from other nodes in the background
backfill=
if [ "$storage" = full ] && { [ "$first" = github ] || [ "$first" = network ] || [ "$pruned_to_full" = yes ]; }; then
    backfill_default=yes
    [ "$(current storage backfill)" != off ] || backfill_default=no
    echo "3b. History (backfill)"
    echo "   yes: fetch every older block down to block 0 in the background, each checked against the chain;"
    echo "        the node runs and takes part in the network meanwhile. On mainnet about 186 million blocks:"
    echo "        one to five days, and the disk of a full node (about 130 GB or more with rocksdb, 400 GB"
    echo "        with lmdb or berkeleydb)."
    echo "   no:  keep only the blocks from the snapshot on (a few GB); the history before is not served."
    ask "   Fetch the older blocks? (yes/no)" "$backfill_default" yes no
    backfill=$answer
    echo
elif [ "$storage" = full ] && [ "$(current storage backfill)" = off ]; then
    backfill=no   # not asked: a choice made before is kept
fi

# 4. sharing snapshots
serve_default=yes
[ "$cur_serve" != off ] || serve_default=no
echo "4. Sharing"
echo "   The node can share snapshots of the chain with new nodes, so that they start in minutes."
echo "   It uses at most 2 MB/s of upload, only while the node is synced, and about 200 MB of disk."
ask "   Share snapshots with other nodes? (yes/no)" "$serve_default" yes no
serve=$answer
echo

# 5. the contract executor (when config.ini runs one)
executor_choice=none
executor_action=keep
if [ -n "$(current api executor_command)" ]; then
    have_build=$(jar_build contract-executor.jar)
    support_build=$(jar_build sc-api-support.jar)
    cur_min=$(current api executor_commit_min)
    cur_max=$(current api executor_commit_max)
    echo "5. Contract executor"
    echo "   The node runs smart contracts with contract-executor.jar and sc-api-support.jar, and starts"
    echo "   the executor only if its build is within executor_commit_min..executor_commit_max of config.ini."
    echo "   In this folder: $(describe_build "$have_build"); config.ini accepts ${cur_min:-1506}..${cur_max:-any}."
    if [ -n "$have_build" ] && [ -n "$support_build" ] && [ "$have_build" != "$support_build" ]; then
        echo "   sc-api-support.jar is $(describe_build "$support_build"): the two jars come from different packages."
    fi
    echo "   community: the latest community build, the official one with fixes (code of a contract that never"
    echo "              returns is stopped, tokens of every standard are reported to the API). Contracts give"
    echo "              the same results as with the official one."
    echo "   official:  build $official_build of the official release."
    # unattended (--defaults), nothing is downloaded: the jars in the folder are kept
    download_default=yes
    [ "$defaults" = no ] || download_default=no
    executor_default=official
    if [ -n "$have_build" ] && [ "$have_build" -gt "$official_build" ] 2>/dev/null; then executor_default=community; fi
    ask "   Which executor? (community/official)" "$executor_default" community official
    executor_choice=$answer
    if [ "$executor_choice" = community ] && { [ -z "$have_build" ] || [ "$have_build" -le "$official_build" ] 2>/dev/null; }; then
        echo "   The jars in this folder are not a community build (copying only the node binary keeps the old ones)."
        ask "   Download the latest community executor from GitHub now (about 25 MB)? (yes/no)" "$download_default" yes no
        [ "$answer" = yes ] && executor_action=download || executor_action=manual
    elif [ "$executor_choice" = official ] && [ "$have_build" != "$official_build" ]; then
        if [ -f contract-executor.jar.official ] && [ -f sc-api-support.jar.official ]; then
            echo "   The official jars set aside by an earlier set-up are here: they are put back."
            executor_action=restore
        else
            ask "   Download the official executor now (from the official release, about 350 MB)? (yes/no)" "$download_default" yes no
            [ "$answer" = yes ] && executor_action=download || executor_action=manual
        fi
    fi
    echo
fi

# 6. the wallet (the package's credits-wallet): an account now, with a profile for this node.
# Run as root (sudo ./setup.sh) in a root-owned folder, the wallet's steps run as the user who
# called sudo, in that user's home: a key store under /root would be the wrong place. Root without
# sudo: no account. Started by sudo in another user's folder, the set-up runs as the folder's
# owner (above): an account would then belong to the owner, so it is made only when the owner is
# the user who called sudo.
wallet_bin=./credits-wallet
wallet=none
wallet_name=
wallet_file=
run_as=
if [ "$(id -u)" = 0 ] && [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
    run_as=$SUDO_USER
    user_home=$(sudo -u "$run_as" -H sh -c 'echo "$HOME"')
else
    user_home=$HOME
fi
wallet_home=${CREDITS_WALLET_HOME:-$user_home/.credits-wallet}
# as_user CMD...: runs CMD as the user the wallet belongs to (CREDITS_WALLET_HOME passed on)
as_user() {
    if [ -n "$run_as" ]; then
        sudo -u "$run_as" -H env ${CREDITS_WALLET_HOME:+CREDITS_WALLET_HOME="$CREDITS_WALLET_HOME"} "$@"
    else
        "$@"
    fi
}
if [ -x "$wallet_bin" ]; then
    echo "6. Wallet"
    echo "   credits-wallet, in this folder, is a terminal wallet: it keeps your keys on this machine,"
    echo "   encrypted, and sends through this node. The user guide is wallet-guide.md."
    me=$(id -un)
    if [ "$(id -u)" = 0 ] && [ -z "$run_as" ]; then
        echo "   The set-up runs as root, not through sudo: a wallet account is not created here (its keys"
        echo "   would belong to root). Later, as the user who runs the node:"
        echo "     cd $(printf '%q' "$(pwd)") && ./credits-wallet --config wallet.toml account new main"
        wallet=no
    elif [ -n "${CREDITS_SETUP_INVOKER:-}" ] && [ "$CREDITS_SETUP_INVOKER" != "$me" ]; then
        echo "   The set-up runs as $me, who owns this folder, for $CREDITS_SETUP_INVOKER: an account made"
        echo "   here would belong to $me. Later, as the user the wallet is for:"
        echo "     cd $(printf '%q' "$(pwd)") && ./credits-wallet --config wallet.toml account new main"
        wallet=no
    else
        [ -z "$run_as" ] || echo "   The account will belong to $run_as (who ran sudo), in $wallet_home."
        have_accounts=no
        if [ -d "$wallet_home/accounts" ] && [ -n "$(ls "$wallet_home/accounts" 2>/dev/null)" ]; then
            echo "   The wallet in $wallet_home has accounts already."
            have_accounts=yes
        fi
        echo "   create: a new account, separate from the node (recommended): you write its 24-word"
        echo "           recovery phrase on paper, in this terminal."
        echo "   import: a key you have: a 24-word phrase, a base58 key, or the keys JSON of the official"
        echo "           wallet."
        echo "   node:   this node's own key (NodePrivate.txt), to see its rewards in the wallet. Not for"
        echo "           funds: that key stays in clear on this server for the node."
        echo "   no:     not now (later: ./credits-wallet)."
        wallet_default=create
        [ "$have_accounts" = no ] && [ "$defaults" = no ] || wallet_default=no
        ask "   Create a wallet account now? (create/import/node/no)" "$wallet_default" create import node no
        wallet=$answer
        if [ "$wallet" = import ]; then
            ask_text "   File holding the key (Enter: paste or type it when the wallet asks)" ""
            wallet_file=$answer
            if [ -n "$wallet_file" ] && [ ! -f "$wallet_file" ]; then
                echo "   $wallet_file is not a file: the wallet will ask for the key."
                wallet_file=
            fi
            case "$(basename "${wallet_file:-x}")" in NodePrivate.txt) wallet=node ;; esac
            if [ -n "$wallet_file" ] && [ -f "$(dirname "$wallet_file")/NodePublic.txt" ]; then wallet=node; fi
        elif [ "$wallet" = node ]; then
            wallet_file=NodePrivate.txt
        fi
        if [ "$wallet" = node ]; then
            echo "   This is a node's key. Importing it makes it a spending key as well, and the node keeps"
            echo "   it in clear on this server: anyone who gets into the server can take that account's CS."
            echo "   Keep funds in a separate account (create), and never delete NodePrivate.txt: the node"
            echo "   needs it."
            if [ ! -f "$wallet_file" ]; then
                echo "   There is no $wallet_file here: nothing is imported."
                wallet=no
            else
                ask "   Import the node's key anyway? (no/yes)" no no yes
                [ "$answer" = yes ] || wallet=no
            fi
        fi
        if [ "$wallet" != no ]; then
            default_name=main
            [ "$wallet" != node ] || default_name=node
            while :; do
                ask_text "   Account name" "$default_name"
                printf '%s' "$answer" | grep -Eq '^[A-Za-z0-9][A-Za-z0-9_.-]{0,31}$' && [ "${answer%.}" = "$answer" ] && break
                echo "  An account name has 1 to 32 letters, digits, '.', '_' or '-'."
                [ "$defaults" = no ] || break
            done
            wallet_name=$answer
            if [ -e "$wallet_home/accounts/$wallet_name.json" ]; then
                echo "   The wallet has an account named $wallet_name already: nothing will be created."
                wallet=no
            fi
        fi
    fi
    echo
fi

# 7. the token cache: the node's API lists the tokens of every wallet ([api] tokens_cache)
cur_tokens=$(current api tokens_cache)
tokens_default=no
[ "$cur_tokens" != true ] || tokens_default=yes
echo "7. Token cache"
echo "   With it, the node's API lists every token a wallet holds, and the wallet shows them all."
echo "   Without it, the wallet finds an account's tokens in its own history (it may miss some)."
echo "   Cost: at its first start with the cache, the node looks for tokens among all deployed contracts"
echo "   and their transactions (it takes a while), then reads token balances in the background after"
echo "   each token call. It is complete only if it was on while the node read every block: a pruned"
echo "   node, or one started from the published snapshot, lists every token with correct balances but"
echo "   may miss wallets whose last use of a token is older than its oldest block (the wallet then"
echo "   finds the token in the account's history, or you add it there)."
ask "   Keep a token cache? (yes/no)" "$tokens_default" yes no
tokens=$answer
echo

# 8. start now
ask "8. Start the node when the set-up is done? (yes/no)" yes yes no
start=$answer
echo

echo "Summary"
echo "  storage:      $storage"
echo "  database:     $backend$( [ "$convert" = yes ] && echo " (converted from $have at the next start)")"
case "$first" in
    keep)    echo "  first start:  the database in this folder is kept" ;;
    network) echo "  first start:  snapshot from other nodes" ;;
    github)  echo "  first start:  snapshot downloaded from GitHub now" ;;
    sync)    echo "  first start:  sync from block 0" ;;
esac
[ -z "$backfill" ] || echo "  backfill:     $backfill"
echo "  share:        $serve"
case "$executor_choice/$executor_action" in
    none/*)            ;;
    */keep)            echo "  executor:     $executor_choice, the jars in this folder are kept" ;;
    */download)        echo "  executor:     $executor_choice, downloaded now (the jars in this folder are set aside)" ;;
    */restore)         echo "  executor:     official, the jars set aside before are put back" ;;
    */manual)          echo "  executor:     $executor_choice, to copy by hand (see below)" ;;
esac
case "$wallet" in
    create) echo "  wallet:       create the account $wallet_name${run_as:+ (for $run_as)}" ;;
    import) echo "  wallet:       import the account $wallet_name${wallet_file:+ from $wallet_file}${run_as:+ (for $run_as)}" ;;
    node)   echo "  wallet:       import the node's key as $wallet_name (not for funds)${run_as:+ (for $run_as)}" ;;
    no)     echo "  wallet:       no account now" ;;
esac
echo "  token cache:  $tokens"
echo "  start now:    $start"
if [ "$defaults" = no ]; then
    ask "Write config.ini? (yes/no)" yes yes no
    [ "$answer" = yes ] || { echo "Nothing changed."; exit 0; }
fi

backup="$config.bak-$(date +%Y%m%d-%H%M%S)"
cp -p "$config" "$backup"

case "$first" in network) fetch=auto ;; *) fetch=off ;; esac
[ "$serve" = yes ] && serve_value=on || serve_value=off

# config.ini without its [snapshot] and [storage] sections, then the new ones ([snapshot] first)
tmp="$config.tmp.$$"
awk '
    /^[[:space:]]*\[/ { skip = ($0 ~ /^[[:space:]]*\[(snapshot|storage)\]/) }
    !skip && !/^; written by setup\.(sh|bat) on / { print }
' "$backup" | tr -d '\r' | awk 'NF { blank = 0; print; next } !blank++ { print }' > "$tmp"
{
    echo
    echo "; written by setup.sh on $(date '+%Y-%m-%d %H:%M'), the previous file is $backup"
    echo "[snapshot]"
    echo "serve=$serve_value"
    echo "fetch=$fetch"
    echo
    echo "[storage]"
    echo "db_backend=$backend"
    if [ "$storage" = pruned ]; then
        echo "checkpoint_every=100000"
        echo "checkpoint_keep=2"
        echo "prune_keep_blocks=1000"
    fi
    case "$backfill" in
        yes) echo "backfill=on" ;;
        no)  echo "backfill=off" ;;
    esac
    # the backfill's tuning keys set by hand (backfill_rate, backfill_batch...) are kept
    if [ "$storage" = full ]; then
        awk '/^[[:space:]]*\[/ { in_s = ($0 ~ /^[[:space:]]*\[storage\]/); next }
             in_s && /^[[:space:]]*backfill_[a-z_]+[[:space:]]*=/ { sub(/^[[:space:]]+/, ""); print }' "$backup" | tr -d '\r'
    fi
} >> "$tmp"
mv "$tmp" "$config"
echo "config.ini written; the previous one is $backup"
if [ "$tokens" = yes ] && [ "$cur_tokens" != true ]; then
    set_api_key tokens_cache true
    echo "config.ini: tokens_cache=true"
elif [ "$tokens" = no ] && [ -n "$cur_tokens" ] && [ "$cur_tokens" != false ]; then
    set_api_key tokens_cache false
    echo "config.ini: tokens_cache=false"
fi

# the executor: put the chosen jars in place, then make config.ini accept their build
if [ "$executor_choice" != none ]; then
    stamp=$(date +%Y%m%d-%H%M%S)
    # keeps the jars in this folder aside: as .official when they are the official ones (a later
    # set-up puts them back without a download), as .bak-<date> otherwise
    set_aside() {
        local suffix=".bak-$stamp"
        [ "$(jar_build contract-executor.jar)" != "$official_build" ] || suffix=.official
        for f in contract-executor.jar sc-api-support.jar; do
            [ ! -f "$f" ] || mv "$f" "$f$suffix"
        done
        echo "The previous executor jars are kept as contract-executor.jar$suffix and sc-api-support.jar$suffix."
    }
    case "$executor_action" in
        download)
            echo
            echo "Downloading the $executor_choice executor..."
            dl=$(mktemp -d)
            if download_executor "$executor_choice" "$dl"; then
                set_aside
                mv "$dl/contract-executor.jar" "$dl/sc-api-support.jar" .
                echo "contract-executor.jar and sc-api-support.jar replaced: $(describe_build "$(jar_build contract-executor.jar)")."
            else
                executor_action=manual
                echo "The jars in this folder are kept."
            fi
            rm -rf "$dl"
            ;;
        restore)
            set_aside
            mv contract-executor.jar.official contract-executor.jar
            mv sc-api-support.jar.official sc-api-support.jar
            echo "The official executor jars are back."
            ;;
    esac
    build=$(jar_build contract-executor.jar)
    if [ -n "$build" ]; then
        min=$(current api executor_commit_min)
        max=$(current api executor_commit_max)
        if [ -n "$min" ] && [ "$build" -lt "$min" ] 2>/dev/null; then set_api_key executor_commit_min "$build"; echo "config.ini: executor_commit_min=$build"; fi
        if [ -n "$max" ] && [ "$max" != -1 ] && [ "$build" -gt "$max" ] 2>/dev/null; then set_api_key executor_commit_max "$build"; echo "config.ini: executor_commit_max=$build (it was $max: the node would not start this executor)"; fi
    fi
    if [ "$executor_action" = manual ]; then
        echo
        echo "To use the $executor_choice executor, with the node stopped:"
        if [ "$executor_choice" = community ]; then
            echo "  copy contract-executor.jar and sc-api-support.jar, both, from the csnode folder of the community"
            echo "  package (or from $community_url/...) into $(pwd),"
        else
            echo "  copy contract-executor.jar and sc-api-support.jar, both, from the official release"
            echo "  ($official_url) into $(pwd),"
        fi
        echo "  then run this set-up again: it makes config.ini accept their build (executor_commit_min/max)."
    fi
    echo
    echo "Updating this folder later: a new node binary is not enough when its package brings a new executor."
    echo "Copy node, contract-executor.jar and sc-api-support.jar together, then run the set-up again."
fi

if [ "$first" = github ]; then
    echo
    echo "Downloading the published snapshot..."
    if ! "$node" --config-file "$config" --download-snapshot; then
        echo "The download did not finish. Run ./setup.sh again, or start the node: it can get the snapshot from other nodes."
        exit 1
    fi
fi

# the wallet: its profile of this node (wallet.toml), then the account
if [ "$wallet" != none ]; then
    api_port=$(current api port)
    [ -n "$api_port" ] || api_port=9090
    diag_port=$(current api diag_port)
    # the diag port tells the wallet the node's network; the set-up never turns it on (the node
    # listens on it on every interface): without it the profile decides alone
    diag=off
    [ -z "$diag_port" ] || [ "$diag_port" = 0 ] || diag="127.0.0.1:$diag_port"
    # this package runs mainnet; a network declared in an earlier wallet.toml, and the settings
    # added there by hand, are kept
    wallet_network=mainnet
    kept=
    if [ -f wallet.toml ]; then
        n=$(grep -E '^network *= *"(testnet|private)"' wallet.toml | head -1 | sed 's/.*"\(.*\)".*/\1/')
        [ -z "$n" ] || wallet_network=$n
        kept=$(grep -E '^(history_node|network_uuid|require_network_uuid) *=' wallet.toml || true)
    fi
    # never written through a symlink or over anything but a regular file: a new file, then renamed
    if [ -L wallet.toml ] || { [ -e wallet.toml ] && [ ! -f wallet.toml ]; }; then
        echo
        echo "wallet.toml is a symlink or not a regular file: refused, nothing written. Remove it and run the set-up again."
        exit 1
    fi
    wallet_tmp=$(mktemp wallet.toml.XXXXXX)
    {
        echo "# written by setup.sh on $(date '+%Y-%m-%d %H:%M'): the wallet profile of the node in this folder"
        echo "# (./credits-wallet --config wallet.toml; see wallet-guide.md, section 4)"
        echo 'node = "local"'
        echo
        echo '[nodes.local]'
        echo "address = \"127.0.0.1:$api_port\""
        echo "network = \"$wallet_network\""
        echo "diag = \"$diag\""
        [ -z "$kept" ] || echo "$kept"
    } > "$wallet_tmp"
    chmod 644 "$wallet_tmp"
    mv -f "$wallet_tmp" wallet.toml
    echo
    echo "wallet.toml written: node 127.0.0.1:$api_port, $wallet_network, diag port $diag"
    if [ "$api_port" = 0 ]; then
        echo "The node's API is off ([api] port=0): the wallet needs it. Set port=9090 in config.ini."
    fi
    if [ "$wallet" != no ]; then
        if [ ! -f "$wallet_home/config.toml" ]; then
            as_user mkdir -p "$wallet_home" && as_user chmod 700 "$wallet_home"
            as_user sh -c 'umask 077 && cat > "$1"' sh "$wallet_home/config.toml" < wallet.toml
            echo "It is the wallet's configuration too: $wallet_home/config.toml."
        elif ! cmp -s wallet.toml "$wallet_home/config.toml"; then
            echo "$wallet_home/config.toml is kept; this node's profile: ./credits-wallet --config wallet.toml"
        fi
        if [ "$wallet" = create ]; then
            set -- --config wallet.toml account new "$wallet_name"
        else
            set -- --config wallet.toml account import "$wallet_name"
            [ -z "$wallet_file" ] || set -- "$@" "$wallet_file"
        fi
        # the command to run later, each argument quoted for the shell
        wallet_cmd=$(printf '%q ' "$wallet_bin" "$@")
        wallet_cmd=${wallet_cmd% }
        [ -z "$run_as" ] || wallet_cmd="sudo -u $(printf '%q' "$run_as") -H $wallet_cmd"
        here_q=$(printf '%q' "$(pwd)")
        if [ -t 0 ] && [ -t 1 ]; then
            echo
            if [ "$wallet" = create ]; then
                echo "The wallet now shows the 24-word recovery phrase of the new account ONCE. Write it on paper:"
                echo "anyone who has it owns the account, and nobody can recover the account without it."
            fi
            if as_user "$wallet_bin" "$@"; then
                echo "Open the wallet with: cd $here_q && ./credits-wallet"
                [ "$wallet" != node ] || echo "Keep NodePrivate.txt: the node needs it. Keep funds in a separate account."
            else
                echo "The wallet did not finish; run it again later: cd $here_q && $wallet_cmd"
            fi
        else
            echo "Creating or importing an account needs a terminal: run it in one with"
            echo "  cd $here_q && $wallet_cmd"
        fi
    fi
fi

args="--config-file $config"
[ "$convert" = no ] || args="$args --convert-db"
if [ "$start" = yes ]; then
    echo
    echo "Starting the node (Ctrl+C stops it and saves its state)..."
    # shellcheck disable=SC2086
    exec "$node" $args
fi
echo
echo "Start the node with: cd \"$(pwd)\" && $node $args"
