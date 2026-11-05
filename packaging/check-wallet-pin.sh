#!/usr/bin/env bash
# check-wallet-pin.sh [tag]: packaging/wallet-sha256 (or WALLET_PIN) must pin exactly the three
# archives of the wallet release in packaging/wallet-tag (or the tag given), in SHA256SUMS format:
#   <64 hex>  credits-wallet-<version>-darwin-arm64.tar.gz
#   <64 hex>  credits-wallet-<version>-linux-amd64.tar.gz
#   <64 hex>  credits-wallet-<version>-windows-amd64.zip
# wallet-tag and wallet-sha256 are bumped together; this check catches one without the other.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tag=${1:-$(tr -d '[:space:]' < "$here/wallet-tag")}
pin=${WALLET_PIN:-$here/wallet-sha256}
echo "$tag" | grep -qE '^wallet-v[0-9]+\.[0-9]+\.[0-9]+(-rc[0-9]+)?$' || { echo "bad wallet tag '$tag'" >&2; exit 1; }
v=${tag#wallet-v}
want=$(printf '%s\n' "credits-wallet-$v-darwin-arm64.tar.gz" "credits-wallet-$v-linux-amd64.tar.gz" "credits-wallet-$v-windows-amd64.zip")
bad=$(grep -vE '^[0-9a-f]{64}  [^ ]+$' "$pin" || true)
[ -z "$bad" ] || { echo "$pin: not SHA256SUMS lines: $bad" >&2; exit 1; }
got=$(awk '{ print $2 }' "$pin" | LC_ALL=C sort)
[ "$got" = "$want" ] || { printf '%s pins:\n%s\nbut %s needs:\n%s\n' "$pin" "$got" "$tag" "$want" >&2; exit 1; }
echo "$pin pins the three archives of $tag"
