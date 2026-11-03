#!/usr/bin/env bash
# Fetch credits-wallet for a package: fetch-wallet.sh <platform> <out dir>
#   platform: linux-amd64, windows-amd64 or darwin-arm64
#
# The release is the tag in packaging/wallet-tag (or WALLET_TAG) of the public repository
# CreditsCommunity/wallet. Its archive and SHA256SUMS are downloaded anonymously from the release
# (or taken from WALLET_ASSETS, a folder holding both, e.g. from `gh release download`), the archive
# is checked against SHA256SUMS, and <out dir> receives the binary (credits-wallet or
# credits-wallet.exe) and the guide (wallet-guide.md, wallet-guide.it.md).
set -euo pipefail

platform=$1 out=$2
here=$(cd "$(dirname "$0")" && pwd)
tag=${WALLET_TAG:-$(tr -d '[:space:]' < "$here/wallet-tag")}
echo "$tag" | grep -qE '^wallet-v[0-9]+\.[0-9]+\.[0-9]+(-rc[0-9]+)?$' || { echo "bad wallet tag '$tag'" >&2; exit 1; }
version=${tag#wallet-v}
case "$platform" in
    linux-amd64|darwin-arm64) archive=credits-wallet-$version-$platform.tar.gz exe=credits-wallet ;;
    windows-amd64)            archive=credits-wallet-$version-$platform.zip    exe=credits-wallet.exe ;;
    *) echo "unknown platform '$platform'" >&2; exit 1 ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if [ -n "${WALLET_ASSETS:-}" ]; then
    cp "$WALLET_ASSETS/$archive" "$WALLET_ASSETS/SHA256SUMS" "$work/"
else
    url=https://github.com/CreditsCommunity/wallet/releases/download/$tag
    for f in "$archive" SHA256SUMS; do
        curl -sSfL --retry 3 -o "$work/$f" "$url/$f" \
            || { echo "cannot download $url/$f (the release must exist and the repository be public)" >&2; exit 1; }
    done
fi
(cd "$work" && grep -E "^[0-9a-f]{64}  \*?$archive\$" SHA256SUMS | sha256sum -c -) \
    || { echo "$archive does not match the SHA256SUMS of $tag" >&2; exit 1; }

mkdir -p "$work/x" "$out"
case "$archive" in
    *.zip) unzip -q "$work/$archive" -d "$work/x" ;;
    *)     tar -xzf "$work/$archive" -C "$work/x" ;;
esac
src=$work/x/credits-wallet-$version
[ -f "$src/$exe" ] && [ -f "$src/guide.md" ] && [ -f "$src/guide.it.md" ] || { echo "$archive lacks $exe or the guide" >&2; exit 1; }
install -m 755 "$src/$exe" "$out/$exe"
install -m 644 "$src/guide.md" "$out/wallet-guide.md"
install -m 644 "$src/guide.it.md" "$out/wallet-guide.it.md"
echo "credits-wallet $version ($platform) in $out"
