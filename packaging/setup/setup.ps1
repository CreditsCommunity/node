# Guided set-up of the node in this folder (Windows): the same questions, the same config.ini, the
# same contract executor choice, wallet.toml and wallet account as setup.sh. Started by setup.bat; "setup.bat --defaults" takes every
# default without asking.
param([switch]$Defaults)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

$node = '.\node.exe'
$config = 'config.ini'
if (-not (Test-Path $node) -or -not (Test-Path $config)) {
    Write-Host 'Run setup.bat from the csnode folder of the package (node.exe and config.ini are not here).'
    exit 1
}

# the choices in $Whole must be typed in full (the first letters are enough for the others)
function Ask([string]$Question, [string]$Default, [string[]]$Choices, [string[]]$Whole = @()) {
    while ($true) {
        if ($Defaults) { Write-Host "$Question [$Default]: $Default"; return $Default }
        if ([Console]::IsInputRedirected) {
            Write-Host -NoNewline "${Question} [$Default]: "
            $reply = [Console]::In.ReadLine()
            if ($null -eq $reply) { $reply = '' }
            Write-Host $reply
        } else {
            $reply = Read-Host "$Question [$Default]"
        }
        $reply = $reply.Trim().ToLower()
        if ($reply -eq '') { return $Default }
        # a whole answer first ("no" is not "node"), then the first one it begins
        foreach ($c in $Choices) { if ($c -eq $reply) { return $c } }
        foreach ($c in $Choices) { if ($Whole -notcontains $c -and $c.StartsWith($reply)) { return $c } }
        Write-Host ("  Please answer one of: $($Choices -join ' ')" + $(if ($Whole.Count) { " ($($Whole -join ' ') in full)" } else { '' }))
    }
}

# restricts a node's key file to its owner (this user) and SYSTEM: no inherited entries
function Restrict-KeyAcl([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return }
    $me = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    Write-Host "NodePrivate.txt: access for its owner only (and SYSTEM), the key is in clear."
    & icacls $Path /inheritance:r /grant:r "*${me}:(F)" '*S-1-5-18:(F)' | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "  icacls failed: restrict NodePrivate.txt yourself (Properties > Security)." }
}

# a free-text answer, trimmed (the default for an empty one)
function Ask-Text([string]$Question, [string]$Default) {
    if ($Defaults) { Write-Host "$Question [$Default]: $Default"; return $Default }
    if ([Console]::IsInputRedirected) {
        Write-Host -NoNewline "${Question} [$Default]: "
        $reply = [Console]::In.ReadLine()
        if ($null -eq $reply) { $reply = '' }
        Write-Host $reply
    } else {
        $reply = Read-Host "$Question [$Default]"
    }
    $reply = $reply.Trim()
    if ($reply -eq '') { return $Default }
    return $reply
}

function Existing-Backend {
    if (Test-Path 'db\.backend') { return (Get-Content 'db\.backend' -Raw).Trim() }
    if ((Test-Path 'db\blockchain.db') -or (Test-Path 'db\sequence.db')) { return 'berkeleydb' }
    if (Test-Path 'db\lmdb\data.mdb') { return 'lmdb' }
    if (Test-Path 'db\CURRENT') { return 'rocksdb' }
    return ''
}

$lines = @(Get-Content -LiteralPath $config)

function Current([string]$Section, [string]$Key) {
    $in = $false; $value = ''
    foreach ($l in $lines) {
        if ($l -match '^\s*\[') { $in = ($l -match "^\s*\[$Section\]"); continue }
        if ($in -and $l -match "^\s*$Key\s*=\s*(.*?)\s*$") { $value = $Matches[1] }
    }
    return $value
}

# --- contract executor ---------------------------------------------------------------------------
# The node runs smart contracts with contract-executor.jar (it loads sc-api-support.jar from the same
# folder) and starts it only if the build the jar reports (its git commit count) is within
# [executor_commit_min, executor_commit_max] of [api]; otherwise it stops the executor and runs no
# contract. The official release is build 1518; community builds add fixes on top and count higher.
$officialBuild = 1518
$officialUrl = 'https://github.com/CREDITSCOM/node/releases/download/mainnet-4.2.532.0/Ubuntu20_Mainnet-4.2.532.0.tar.gz'
$officialShaExecutor = '592510d1586818348c65bea9b443e7f58aa6947113e43aa8906af8426aeef028'
$officialShaSupport = '395d9a108a66fe1d92c1d2096a9d11f4c7662b51b47af792db0ff4cc91df9807'
$communityUrl = 'https://github.com/CreditsCommunity/contract-executor/releases/latest/download'
Add-Type -AssemblyName System.IO.Compression.FileSystem

# the build (git commit count) an executor jar reports, 0 when it cannot be read
function Jar-Build([string]$Jar) {
    if (-not (Test-Path -LiteralPath $Jar)) { return 0 }
    try {
        $zip = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Jar).Path)
        try {
            $entry = $zip.GetEntry('git.properties')
            if ($null -eq $entry) { return 0 }
            $reader = New-Object System.IO.StreamReader($entry.Open())
            try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
        } finally { $zip.Dispose() }
        if ($text -match '(?m)^git\.total\.commit\.count=(\d+)') { return [int]$Matches[1] }
    } catch { }
    return 0
}

function Describe-Build([int]$Build) {
    if ($Build -eq 0) { return 'none or unreadable' }
    if ($Build -eq $officialBuild) { return "build $Build (official)" }
    if ($Build -gt $officialBuild) { return "build $Build (community)" }
    return "build $Build"
}

function Sha256([string]$Path) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLower() }

# downloads the jars of the chosen executor into $Dir; prints the reason and returns $false when it cannot
function Download-Executor([string]$Kind, [string]$Dir) {
    if (-not (Get-Command curl.exe -ErrorAction SilentlyContinue)) { Write-Host '   curl.exe is not available.'; return $false }
    if ($Kind -eq 'community') {
        foreach ($f in @('contract-executor.jar', 'sc-api-support.jar', 'SHA256SUMS')) {
            & curl.exe -fsSL -o (Join-Path $Dir $f) "$communityUrl/$f"
            if ($LASTEXITCODE -ne 0) { Write-Host '   The download from GitHub failed.'; return $false }
        }
        $sums = Get-Content -LiteralPath (Join-Path $Dir 'SHA256SUMS')
        foreach ($f in @('contract-executor.jar', 'sc-api-support.jar')) {
            $sha = Sha256 (Join-Path $Dir $f)
            if (-not ($sums | Where-Object { $_ -match "^$sha\s+\*?$([regex]::Escape($f))$" })) {
                Write-Host "   $f does not match the SHA256SUMS of the release."; return $false
            }
        }
    } else {
        Write-Host '   The jars come from the official release package (about 350 MB to download, only they are kept).'
        $archive = Join-Path $Dir 'official.tar.gz'
        & curl.exe -fsSL -o $archive $officialUrl
        if ($LASTEXITCODE -ne 0) { Write-Host '   The download of the official release failed.'; return $false }
        & tar.exe -xzf $archive -C $Dir csnode/contract-executor.jar csnode/sc-api-support.jar
        if ($LASTEXITCODE -ne 0) { Write-Host '   The official release could not be extracted.'; return $false }
        Remove-Item -LiteralPath $archive
        $extracted = Join-Path $Dir 'csnode'
        Move-Item -LiteralPath (Join-Path $extracted 'contract-executor.jar'), (Join-Path $extracted 'sc-api-support.jar') -Destination $Dir
        if ((Sha256 (Join-Path $Dir 'contract-executor.jar')) -ne $officialShaExecutor -or
            (Sha256 (Join-Path $Dir 'sc-api-support.jar')) -ne $officialShaSupport) {
            Write-Host '   The jars of the official release do not have the expected checksums.'; return $false
        }
    }
    return $true
}

# key=value in [api] of the lines, added at the end of the section when missing
function Set-ApiKey([System.Collections.Generic.List[string]]$Lines, [string]$Key, [string]$Value) {
    $in = $false; $end = -1
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        if ($Lines[$i] -match '^\s*\[') {
            if ($in -and $end -lt 0) { $end = $i }
            $in = ($Lines[$i] -match '^\s*\[api\]')
            continue
        }
        if ($in -and $Lines[$i] -match "^\s*$Key\s*=") { $Lines[$i] = "$Key=$Value"; return }
    }
    if ($in) { $Lines.Add("$Key=$Value") } elseif ($end -ge 0) { $Lines.Insert($end, "$Key=$Value") }
}

$version = if (Test-Path 'VERSION') { (Get-Content 'VERSION' -Raw).Trim() } else { 'unknown version' }
Write-Host "Credits node set-up ($version)"
Write-Host 'Each question shows the default in brackets: press Enter to keep it.'
Write-Host ''

$have = Existing-Backend
if ($have) { Write-Host "This folder already holds a $have database: it is kept."; Write-Host '' }

$curBackend = Current 'storage' 'db_backend'
if (-not $curBackend) { $curBackend = Current 'storage' 'block_db' }
$curPrune = Current 'storage' 'prune_keep_blocks'
$curServe = Current 'snapshot' 'serve'

# 1. pruned or full
$storageDefault = 'pruned'
if (($curBackend + $curPrune) -and (-not $curPrune -or $curPrune -eq '0')) { $storageDefault = 'full' }
if ($have -eq 'berkeleydb') { $storageDefault = 'full' }
Write-Host '1. Storage'
Write-Host '   pruned: keeps only recent blocks (a few GB), starts in minutes from a snapshot. Enough to run a node.'
Write-Host '   full:   keeps every block since the start of the chain (tens of GB), for history and explorers.'
$storage = Ask '   Pruned or full node? (pruned/full)' $storageDefault @('pruned', 'full')
$prunedToFull = $false
if ($storage -eq 'full' -and $have -and $curPrune -and $curPrune -ne '0') {
    Write-Host '   This folder holds a pruned database: as a full node it keeps its blocks and fetches the ones'
    Write-Host '   it removed from other nodes in the background (backfill, question 3).'
    $prunedToFull = $true
}
Write-Host ''

# 2. database
Write-Host '2. Database'
if ($storage -eq 'pruned') {
    $backendDefault = if ($curBackend -eq 'rocksdb' -or $have -eq 'rocksdb') { 'rocksdb' } else { 'lmdb' }
    Write-Host '   lmdb:    the usual choice for pruned nodes.'
    Write-Host '   rocksdb: compressed, smaller on disk, a little more CPU.'
    $backend = Ask '   Database? (lmdb/rocksdb)' $backendDefault @('lmdb', 'rocksdb')
} else {
    $backendDefault = 'berkeleydb'
    if (($curBackend -eq 'lmdb' -or $curBackend -eq 'rocksdb') -and (-not $curPrune -or $curPrune -eq '0')) { $backendDefault = $curBackend }
    if ($have) { $backendDefault = $have }
    Write-Host "   berkeleydb: the official node's format (the official node can open this folder too)."
    Write-Host '   rocksdb:    the smallest full node (about a third of berkeleydb).'
    Write-Host '   lmdb:       fast, larger on disk.'
    $backend = Ask '   Database? (berkeleydb/rocksdb/lmdb)' $backendDefault @('berkeleydb', 'rocksdb', 'lmdb')
}
Write-Host ''

$convert = $false
if ($have -and $have -ne $backend) {
    Write-Host "   The database in this folder is $have, you chose $backend."
    $a = Ask "   Convert it to $backend at the next start (it can take hours)? (yes/no)" 'yes' @('yes', 'no')
    if ($a -eq 'yes') { $convert = $true } else { $backend = $have; Write-Host "   Keeping $have." }
    Write-Host ''
}

# 3. how the first start gets the chain (only for an empty folder)
$first = 'keep'
if (-not $have) {
    Write-Host '3. First start'
    if ($backend -eq 'berkeleydb') {
        Write-Host '   A berkeleydb node syncs from block 0 (about 35 hours on mainnet).'
        $first = 'sync'
    } else {
        Write-Host '   github:  download the snapshot published on GitHub now, check it and restore it (minutes).'
        Write-Host '   network: the node fetches a snapshot from other nodes that share it; it needs at least two'
        Write-Host '            of them, and few nodes share yet: without them it syncs from block 0 after 10 minutes.'
        Write-Host '   sync:    sync from block 0 by itself (about 35 hours on mainnet).'
        $firstDefault = 'github'
        if ($storage -eq 'full') {
            Write-Host '   A full node started from a snapshot runs in minutes and fetches the older blocks from'
            Write-Host '   other nodes in the background (backfill, next question).'
        }
        $first = Ask '   How should the node get the chain? (github/network/sync)' $firstDefault @('github', 'network', 'sync')
    }
    Write-Host ''
}

# 3b. backfill: a full node that starts above block 0 (from a snapshot, or a pruned folder) fetches
# the older blocks from other nodes in the background
$backfill = ''
if ($storage -eq 'full' -and ($first -eq 'github' -or $first -eq 'network' -or $prunedToFull)) {
    $backfillDefault = if ((Current 'storage' 'backfill') -eq 'off') { 'no' } else { 'yes' }
    Write-Host '3b. History (backfill)'
    Write-Host '   yes: fetch every older block down to block 0 in the background, each checked against the chain;'
    Write-Host '        the node runs and takes part in the network meanwhile. On mainnet about 186 million blocks:'
    Write-Host '        one to five days, and the disk of a full node (about 130 GB or more with rocksdb, 400 GB'
    Write-Host '        with lmdb or berkeleydb).'
    Write-Host '   no:  keep only the blocks from the snapshot on (a few GB); the history before is not served.'
    $backfill = Ask '   Fetch the older blocks? (yes/no)' $backfillDefault @('yes', 'no')
    Write-Host ''
} elseif ($storage -eq 'full' -and (Current 'storage' 'backfill') -eq 'off') {
    $backfill = 'no'   # not asked: a choice made before is kept
}

# 4. sharing snapshots
$serveDefault = if ($curServe -eq 'off') { 'no' } else { 'yes' }
Write-Host '4. Sharing'
Write-Host '   The node can share snapshots of the chain with new nodes, so that they start in minutes.'
Write-Host '   It uses at most 2 MB/s of upload, only while the node is synced, and about 200 MB of disk.'
$serve = Ask '   Share snapshots with other nodes? (yes/no)' $serveDefault @('yes', 'no')
Write-Host ''

# 5. the contract executor (when config.ini runs one)
$executorChoice = 'none'
$executorAction = 'keep'
if (Current 'api' 'executor_command') {
    $haveBuild = Jar-Build 'contract-executor.jar'
    $supportBuild = Jar-Build 'sc-api-support.jar'
    $curMin = Current 'api' 'executor_commit_min'
    $curMax = Current 'api' 'executor_commit_max'
    Write-Host '5. Contract executor'
    Write-Host '   The node runs smart contracts with contract-executor.jar and sc-api-support.jar, and starts'
    Write-Host '   the executor only if its build is within executor_commit_min..executor_commit_max of config.ini.'
    Write-Host ("   In this folder: $(Describe-Build $haveBuild); config.ini accepts " + $(if ($curMin) { $curMin } else { '1506' }) + '..' + $(if ($curMax) { $curMax } else { 'any' }) + '.')
    if ($haveBuild -and $supportBuild -and $haveBuild -ne $supportBuild) {
        Write-Host "   sc-api-support.jar is $(Describe-Build $supportBuild): the two jars come from different packages."
    }
    Write-Host '   community: the latest community build, the official one with fixes (code of a contract that never'
    Write-Host '              returns is stopped, tokens of every standard are reported to the API). Contracts give'
    Write-Host '              the same results as with the official one.'
    Write-Host "   official:  build $officialBuild of the official release."
    # unattended (--defaults), nothing is downloaded: the jars in the folder are kept
    $downloadDefault = if ($Defaults) { 'no' } else { 'yes' }
    $executorDefault = if ($haveBuild -gt $officialBuild) { 'community' } else { 'official' }
    $executorChoice = Ask '   Which executor? (community/official)' $executorDefault @('community', 'official')
    if ($executorChoice -eq 'community' -and $haveBuild -le $officialBuild) {
        Write-Host '   The jars in this folder are not a community build (copying only node.exe keeps the old ones).'
        $a = Ask '   Download the latest community executor from GitHub now (about 25 MB)? (yes/no)' $downloadDefault @('yes', 'no')
        $executorAction = if ($a -eq 'yes') { 'download' } else { 'manual' }
    } elseif ($executorChoice -eq 'official' -and $haveBuild -ne $officialBuild) {
        if ((Test-Path 'contract-executor.jar.official') -and (Test-Path 'sc-api-support.jar.official')) {
            Write-Host '   The official jars set aside by an earlier set-up are here: they are put back.'
            $executorAction = 'restore'
        } else {
            $a = Ask '   Download the official executor now (from the official release, about 350 MB)? (yes/no)' $downloadDefault @('yes', 'no')
            $executorAction = if ($a -eq 'yes') { 'download' } else { 'manual' }
        }
    }
    Write-Host ''
}

# 6. the wallet (the package's credits-wallet.exe): an account now, with a profile for this node
$walletBin = '.\credits-wallet.exe'
$wallet = 'none'; $walletName = ''; $walletFile = ''; $nodeKey = ''
$walletHome = if ($env:CREDITS_WALLET_HOME) { $env:CREDITS_WALLET_HOME } else { Join-Path $env:APPDATA 'credits-wallet' }
# an elevated set-up writes the account to the profile that runs it (%APPDATA% of that user)
$elevated = $false
try {
    $elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
} catch { }
if (Test-Path -LiteralPath $walletBin) {
    Write-Host '6. Wallet'
    Write-Host '   credits-wallet.exe, in this folder, is a terminal wallet: it keeps keys on this machine, signs'
    Write-Host '   here and sends through this node. The user guide is wallet-guide.md.'
    if ($elevated) {
        Write-Host "   The set-up runs as administrator: an account goes to $walletHome, the profile of"
        Write-Host "   $env:USERNAME. If you use the wallet as another user, answer no and create it later as that user."
    }
    $accDir = Join-Path $walletHome 'accounts'
    $haveAccounts = (Test-Path -LiteralPath $accDir) -and @(Get-ChildItem -LiteralPath $accDir).Count -gt 0
    if ($haveAccounts) { Write-Host "   The wallet in $walletHome has accounts already." }
    # the node's own key as the wallet's account: used in place when the node has one, made now
    # from a 24-word phrase when it has none
    $nodeKey = if (Test-Path -LiteralPath 'NodePrivate.txt') { 'present' } else { 'missing' }
    if ($nodeKey -eq 'present') {
        Write-Host "   node:   this node's own key (NodePrivate.txt) as the wallet's account `"node`" (default):"
        Write-Host "           the address of the node's rewards and of the delegations to it, where operators"
        Write-Host '           keep their stake. Nothing is copied: the wallet reads the key where the node does.'
    } else {
        Write-Host "   node:   create the node's key now from a new 24-word phrase (default): your paper backup"
        Write-Host "           of the node's identity. You write the words on paper, in this terminal; the node"
        Write-Host "           uses the key at its first start, and it is the wallet's account `"node`"."
    }
    Write-Host '   create: a separate account as well, for savings: you write its 24-word recovery phrase'
    Write-Host '           on paper, in this terminal. It is encrypted with the wallet password (one for'
    Write-Host '           every account of the wallet, chosen with its first account).'
    Write-Host '   import: a key you have, as a separate account: a 24-word phrase, a base58 key, or the'
    Write-Host '           keys JSON of the official wallet.'
    Write-Host '   no:     not now (later: credits-wallet.exe --config wallet.toml).'
    Write-Host "   The node's key is kept in clear on this computer, because the node needs it: CS on it are"
    Write-Host '   as exposed as the computer. Keep on it only what the node needs, and savings in a separate'
    Write-Host '   account.'
    # -Defaults cannot show and check 24 words: the node's key is then made later
    $walletDefault = if ($nodeKey -eq 'missing' -and $Defaults) { 'no' } else { 'node' }
    # making the node's key is never chosen by a stray "n": "node" in full (or Enter)
    $whole = if ($nodeKey -eq 'missing') { @('node') } else { @() }
    $wallet = Ask "   The wallet's account? (node/create/import/no)" $walletDefault @('node', 'create', 'import', 'no') $whole
    if ($wallet -eq 'import') {
        $walletFile = Ask-Text '   File holding the key (Enter: paste or type it when the wallet asks)' ''
        if ($walletFile -and -not (Test-Path -LiteralPath $walletFile -PathType Leaf)) {
            Write-Host "   $walletFile is not a file: the wallet will ask for the key."
            $walletFile = ''
        }
        # this node's own key file: used in place, not imported
        if ($walletFile -and ((Resolve-Path -LiteralPath $walletFile).Path -eq (Join-Path $PSScriptRoot 'NodePrivate.txt'))) {
            Write-Host "   That is this node's key: the wallet uses it in place as the account `"node`"; nothing is imported."
            $wallet = 'node'
            $walletFile = ''
        }
    }
    if ($wallet -eq 'create' -or $wallet -eq 'import') {
        while ($true) {
            $walletName = Ask-Text '   Account name' 'main'
            if ($walletName -cmatch '^[A-Za-z0-9][A-Za-z0-9_.-]{0,31}$' -and -not $walletName.EndsWith('.') -and $walletName -ne 'node') { break }
            Write-Host "  An account name has 1 to 32 letters, digits, '.', '_' or '-' (and `"node`" is the node's key)."
            if ($Defaults) { break }
        }
        if (Test-Path -LiteralPath (Join-Path $accDir "$walletName.json")) {
            Write-Host "   The wallet has an account named $walletName already: nothing will be created."
            $wallet = 'no'
        }
    }
    Write-Host ''
}

# 7. the token cache: the node's API lists the tokens of every wallet ([api] tokens_cache)
$curTokens = Current 'api' 'tokens_cache'
$tokensDefault = if ($curTokens -eq 'true') { 'yes' } else { 'no' }
Write-Host '7. Token cache'
Write-Host "   With it, the node's API lists every token a wallet holds, and the wallet shows them all."
Write-Host "   Without it, the wallet finds an account's tokens in its own history (it may miss some)."
Write-Host '   Cost: at its first start with the cache, the node looks for tokens among all deployed contracts'
Write-Host '   and their transactions (it takes a while), then reads token balances in the background after'
Write-Host '   each token call. It is complete only if it was on while the node read every block: a pruned'
Write-Host '   node, or one started from the published snapshot, lists every token with correct balances but'
Write-Host '   may miss wallets whose last use of a token is older than its oldest block (the wallet then'
Write-Host "   finds the token in the account's history, or you add it there)."
$tokens = Ask '   Keep a token cache? (yes/no)' $tokensDefault @('yes', 'no')
Write-Host ''

# 8. start now
$start = Ask '8. Start the node when the set-up is done? (yes/no)' 'yes' @('yes', 'no')
Write-Host ''

Write-Host 'Summary'
Write-Host "  storage:      $storage"
Write-Host ("  database:     $backend" + $(if ($convert) { " (converted from $have at the next start)" } else { '' }))
switch ($first) {
    'keep'    { Write-Host '  first start:  the database in this folder is kept' }
    'network' { Write-Host '  first start:  snapshot from other nodes' }
    'github'  { Write-Host '  first start:  snapshot downloaded from GitHub now' }
    'sync'    { Write-Host '  first start:  sync from block 0' }
}
if ($backfill) { Write-Host "  backfill:     $backfill" }
Write-Host "  share:        $serve"
if ($executorChoice -ne 'none') {
    switch ($executorAction) {
        'keep'     { Write-Host "  executor:     $executorChoice, the jars in this folder are kept" }
        'download' { Write-Host "  executor:     $executorChoice, downloaded now (the jars in this folder are set aside)" }
        'restore'  { Write-Host '  executor:     official, the jars set aside before are put back' }
        'manual'   { Write-Host "  executor:     $executorChoice, to copy by hand (see below)" }
    }
}
switch ($wallet) {
    'create' { Write-Host "  wallet:       create the account $walletName" }
    'import' { Write-Host ("  wallet:       import the account $walletName" + $(if ($walletFile) { " from $walletFile" } else { '' })) }
    'node'   {
        if ($nodeKey -eq 'present') { Write-Host "  wallet:       this node's key as the account node (in place, not copied)" }
        else { Write-Host "  wallet:       the node's key made now from a 24-word phrase, as the account node" }
    }
    'no'     { Write-Host '  wallet:       no account now' }
}
Write-Host "  token cache:  $tokens"
Write-Host "  start now:    $start"
if (-not $Defaults) {
    if ((Ask 'Write config.ini? (yes/no)' 'yes' @('yes', 'no')) -ne 'yes') { Write-Host 'Nothing changed.'; exit 0 }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = "$config.bak-$stamp"
Copy-Item -LiteralPath $config -Destination $backup

$fetch = if ($first -eq 'network') { 'auto' } else { 'off' }
$serveValue = if ($serve -eq 'yes') { 'on' } else { 'off' }

# config.ini without its [snapshot] and [storage] sections, then the new ones ([snapshot] first)
$out = New-Object System.Collections.Generic.List[string]
$skip = $false; $blank = $false
foreach ($l in $lines) {
    if ($l -match '^\s*\[') { $skip = ($l -match '^\s*\[(snapshot|storage)\]') }
    if ($skip -or $l -match '^; written by setup\.(sh|bat) on ') { continue }
    if ($l.Trim() -eq '') { if ($blank) { continue }; $blank = $true } else { $blank = $false }
    $out.Add($l)
}
$out.Add('')
$out.Add("; written by setup.bat on $(Get-Date -Format 'yyyy-MM-dd HH:mm'), the previous file is $backup")
$out.Add('[snapshot]')
$out.Add("serve=$serveValue")
$out.Add("fetch=$fetch")
$out.Add('')
$out.Add('[storage]')
$out.Add("db_backend=$backend")
if ($storage -eq 'pruned') {
    $out.Add('checkpoint_every=100000')
    $out.Add('checkpoint_keep=2')
    $out.Add('prune_keep_blocks=1000')
}
if ($backfill -eq 'yes') { $out.Add('backfill=on') } elseif ($backfill -eq 'no') { $out.Add('backfill=off') }
# the backfill's tuning keys set by hand (backfill_rate, backfill_batch...) are kept
if ($storage -eq 'full') {
    $inStorage = $false
    foreach ($l in $lines) {
        if ($l -match '^\s*\[') { $inStorage = ($l -match '^\s*\[storage\]'); continue }
        if ($inStorage -and $l -match '^\s*backfill_[a-z_]+\s*=') { $out.Add($l.Trim()) }
    }
}

# the executor: put the chosen jars in place, then make config.ini accept their build
$executorNotes = New-Object System.Collections.Generic.List[string]
if ($executorChoice -ne 'none') {
    # keeps the jars in this folder aside: as .official when they are the official ones (a later
    # set-up puts them back without a download), as .bak-<date> otherwise
    function Set-Aside {
        $suffix = if ((Jar-Build 'contract-executor.jar') -eq $officialBuild) { '.official' } else { ".bak-$stamp" }
        foreach ($f in @('contract-executor.jar', 'sc-api-support.jar')) {
            if (Test-Path $f) { Move-Item -LiteralPath $f -Destination "$f$suffix" -Force }
        }
        $executorNotes.Add("The previous executor jars are kept as contract-executor.jar$suffix and sc-api-support.jar$suffix.")
    }
    if ($executorAction -eq 'download') {
        Write-Host ''
        Write-Host "Downloading the $executorChoice executor..."
        $dl = Join-Path ([System.IO.Path]::GetTempPath()) ("credits-executor-" + [guid]::NewGuid())
        New-Item -ItemType Directory -Path $dl | Out-Null
        if (Download-Executor $executorChoice $dl) {
            Set-Aside
            Move-Item -LiteralPath (Join-Path $dl 'contract-executor.jar'), (Join-Path $dl 'sc-api-support.jar') -Destination $PSScriptRoot
            $executorNotes.Add("contract-executor.jar and sc-api-support.jar replaced: $(Describe-Build (Jar-Build 'contract-executor.jar')).")
        } else {
            $executorAction = 'manual'
            $executorNotes.Add('The jars in this folder are kept.')
        }
        Remove-Item -LiteralPath $dl -Recurse -Force
    } elseif ($executorAction -eq 'restore') {
        Set-Aside
        Move-Item -LiteralPath 'contract-executor.jar.official' -Destination 'contract-executor.jar'
        Move-Item -LiteralPath 'sc-api-support.jar.official' -Destination 'sc-api-support.jar'
        $executorNotes.Add('The official executor jars are back.')
    }
    $build = Jar-Build 'contract-executor.jar'
    if ($build) {
        $min = Current 'api' 'executor_commit_min'
        $max = Current 'api' 'executor_commit_max'
        if ($min -and $build -lt [int]$min) { Set-ApiKey $out 'executor_commit_min' "$build"; $executorNotes.Add("config.ini: executor_commit_min=$build") }
        if ($max -and $max -ne '-1' -and $build -gt [int]$max) {
            Set-ApiKey $out 'executor_commit_max' "$build"
            $executorNotes.Add("config.ini: executor_commit_max=$build (it was ${max}: the node would not start this executor)")
        }
    }
    if ($executorAction -eq 'manual') {
        $executorNotes.Add('')
        $executorNotes.Add("To use the $executorChoice executor, with the node stopped:")
        if ($executorChoice -eq 'community') {
            $executorNotes.Add('  copy contract-executor.jar and sc-api-support.jar, both, from the csnode folder of the community')
            $executorNotes.Add("  package (or from $communityUrl/...) into $PSScriptRoot,")
        } else {
            $executorNotes.Add('  copy contract-executor.jar and sc-api-support.jar, both, from the official release')
            $executorNotes.Add("  ($officialUrl) into $PSScriptRoot,")
        }
        $executorNotes.Add('  then run this set-up again: it makes config.ini accept their build (executor_commit_min/max).')
    }
    $executorNotes.Add('')
    $executorNotes.Add('Updating this folder later: a new node.exe is not enough when its package brings a new executor.')
    $executorNotes.Add('Copy node.exe, contract-executor.jar and sc-api-support.jar together, then run the set-up again.')
}

# the token cache
if ($tokens -eq 'yes' -and $curTokens -ne 'true') {
    Set-ApiKey $out 'tokens_cache' 'true'; $executorNotes.Add('config.ini: tokens_cache=true')
} elseif ($tokens -eq 'no' -and $curTokens -and $curTokens -ne 'false') {
    Set-ApiKey $out 'tokens_cache' 'false'; $executorNotes.Add('config.ini: tokens_cache=false')
}

# ASCII without a byte-order mark, as the package ships it
[System.IO.File]::WriteAllLines((Join-Path $PSScriptRoot $config), $out, (New-Object System.Text.ASCIIEncoding))
Write-Host "config.ini written; the previous one is $backup"
foreach ($n in $executorNotes) { Write-Host $n }

if ($first -eq 'github') {
    Write-Host ''
    Write-Host 'Downloading the published snapshot...'
    & $node --config-file $config --download-snapshot
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'The download did not finish. Run setup.bat again, or start the node: it can get the snapshot from other nodes.'
        exit 1
    }
}

# the wallet: its profile of this node (wallet.toml), then the account
if ($wallet -ne 'none') {
    $apiPort = Current 'api' 'port'
    if (-not $apiPort) { $apiPort = '9090' }
    $diagPort = Current 'api' 'diag_port'
    # the diag port tells the wallet the node's network; the set-up never turns it on (the node
    # listens on it on every interface): without it the profile decides alone
    $diag = if ($diagPort -and $diagPort -ne '0') { "127.0.0.1:$diagPort" } else { 'off' }
    # this package runs mainnet; a network declared in an earlier wallet.toml, and the settings
    # added there by hand, are kept
    $walletNetwork = 'mainnet'
    $kept = @()
    if (Test-Path 'wallet.toml') {
        foreach ($l in @(Get-Content -LiteralPath 'wallet.toml')) {
            if ($l -match '^network *= *"(testnet|private)"') { $walletNetwork = $Matches[1] }
            if ($l -match '^(history_node|network_uuid|require_network_uuid|node_key) *=') { $kept += $l }
        }
    }
    $toml = New-Object System.Collections.Generic.List[string]
    $toml.Add("# written by setup.bat on $(Get-Date -Format 'yyyy-MM-dd HH:mm'): the wallet profile of the node in this folder")
    $toml.Add('# (credits-wallet.exe --config wallet.toml; see wallet-guide.md, section 4)')
    $toml.Add('node = "local"')
    $toml.Add('')
    $toml.Add('[nodes.local]')
    $toml.Add("address = `"127.0.0.1:$apiPort`"")
    $toml.Add("network = `"$walletNetwork`"")
    $toml.Add("diag = `"$diag`"")
    # the node's key (NodePrivate.txt here) is the wallet's account "node" only when the answer was
    # node; otherwise off. A node_key already in wallet.toml (set by hand, or by an earlier run) is
    # kept, unless the answer is node.
    if ($wallet -eq 'node') {
        $toml.Add('node_key = "NodePrivate.txt"')
        $kept = @($kept | Where-Object { $_ -notmatch '^node_key' })
    } elseif (-not ($kept | Where-Object { $_ -match '^node_key' })) {
        $toml.Add('node_key = "off"')
    }
    foreach ($l in $kept) { $toml.Add($l) }
    # never written through a link (a symlink or junction planted there) or over anything but a
    # regular file: a new file, then moved over; UTF-8 without a byte-order mark (TOML)
    $tomlPath = Join-Path $PSScriptRoot 'wallet.toml'
    if (Test-Path -LiteralPath $tomlPath) {
        $item = Get-Item -LiteralPath $tomlPath -Force
        if ($item.PSIsContainer -or ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
            Write-Host 'wallet.toml is a link or not a regular file: refused, nothing written. Remove it and run the set-up again.'
            exit 1
        }
    }
    $tomlTmp = Join-Path $PSScriptRoot ("wallet.toml." + [guid]::NewGuid().ToString('N'))
    [System.IO.File]::WriteAllLines($tomlTmp, $toml, (New-Object System.Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $tomlTmp -Destination $tomlPath -Force
    Write-Host ''
    Write-Host "wallet.toml written: node 127.0.0.1:$apiPort, $walletNetwork, diag port $diag"
    if ($apiPort -eq '0') { Write-Host "The node's API is off ([api] port=0): the wallet needs it. Set port=9090 in config.ini." }
    if ($wallet -ne 'no') {
        $homeConfig = Join-Path $walletHome 'config.toml'
        if (-not (Test-Path -LiteralPath $homeConfig)) {
            New-Item -ItemType Directory -Force -Path $walletHome | Out-Null
            # there, node_key names this folder's key by its full path (a TOML literal string)
            $nodeKeyAbs = Join-Path $PSScriptRoot 'NodePrivate.txt'
            $homeToml = foreach ($l in [System.IO.File]::ReadAllLines((Join-Path $PSScriptRoot 'wallet.toml'))) {
                if ($l -eq 'node_key = "NodePrivate.txt"') { if (-not $nodeKeyAbs.Contains("'")) { "node_key = '$nodeKeyAbs'" } } else { $l }
            }
            [System.IO.File]::WriteAllLines($homeConfig, [string[]]$homeToml, (New-Object System.Text.UTF8Encoding($false)))
            Write-Host "It is the wallet's configuration too: $homeConfig."
        } elseif ((Get-Content -LiteralPath $homeConfig -Raw) -ne (Get-Content -LiteralPath 'wallet.toml' -Raw)) {
            Write-Host "$homeConfig is kept; this node's profile: credits-wallet.exe --config wallet.toml"
        }
        # the node's key, in clear, for its owner only (and SYSTEM: the node may run as a service)
        if ($wallet -eq 'node') { Restrict-KeyAcl (Join-Path $PSScriptRoot 'NodePrivate.txt') }
        switch ($wallet) {
            'create' { $walletArgs = @('--config', 'wallet.toml', 'account', 'new', $walletName) }
            'import' {
                $walletArgs = @('--config', 'wallet.toml', 'account', 'import', $walletName)
                if ($walletFile) { $walletArgs += $walletFile }
            }
            'node' {
                $walletArgs = if ($nodeKey -eq 'present') { @('--config', 'wallet.toml', 'account', 'node-key') }
                              else { @('--config', 'wallet.toml', 'account', 'node-key', 'new') }
            }
        }
        # the command to run later: an argument with anything but letters, digits and _ . - is quoted
        # for PowerShell (the account names are of those only; a file's path may need it)
        $walletCmd = '.\credits-wallet.exe ' + (($walletArgs | ForEach-Object { if ($_ -cmatch '^[A-Za-z0-9_.-]+$') { $_ } else { "'" + ($_ -replace "'", "''") + "'" } }) -join ' ')
        if (-not [Console]::IsInputRedirected -and -not [Console]::IsOutputRedirected) {
            Write-Host ''
            if ($wallet -eq 'create' -or ($wallet -eq 'node' -and $nodeKey -eq 'missing')) {
                $what = if ($wallet -eq 'node') { "node's key" } else { 'account' }
                Write-Host "The wallet now shows the 24-word recovery phrase of the new $what ONCE. Write it on paper:"
                Write-Host 'anyone who has it owns the account, and nobody can recover the account without it.'
            }
            & $walletBin @walletArgs
            $walletRc = $LASTEXITCODE
            if ($wallet -eq 'node' -and $nodeKey -eq 'missing') { Restrict-KeyAcl (Join-Path $PSScriptRoot 'NodePrivate.txt') }
            if ($walletRc -eq 0) {
                Write-Host "Open the wallet with: cd `"$PSScriptRoot`" ; .\credits-wallet.exe --config wallet.toml"
                if ($wallet -eq 'node') { Write-Host 'Keep NodePrivate.txt: the node needs it. Keep savings in a separate account.' }
            } else {
                Write-Host "The wallet did not finish; run it again later: cd `"$PSScriptRoot`" ; $walletCmd"
            }
        } elseif ($wallet -eq 'node' -and $nodeKey -eq 'present') {
            Write-Host "The node's key is the wallet's account `"node`": cd `"$PSScriptRoot`" ; .\credits-wallet.exe --config wallet.toml"
        } else {
            Write-Host "Creating or importing an account, or the node's key, needs a terminal: run it in one with"
            Write-Host "  cd `"$PSScriptRoot`" ; $walletCmd"
        }
    }
}

$nodeArgs = @('--config-file', $config)
if ($convert) { $nodeArgs += '--convert-db' }
if ($start -eq 'yes') {
    Write-Host ''
    Write-Host 'Starting the node (Ctrl+C stops it and saves its state)...'
    & $node @nodeArgs
    exit $LASTEXITCODE
}
Write-Host ''
Write-Host "Start the node with: cd `"$PSScriptRoot`" ; node.exe $($nodeArgs -join ' ')"
