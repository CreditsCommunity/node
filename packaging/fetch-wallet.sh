#!/usr/bin/env bash
# Fetch credits-wallet for a package: fetch-wallet.sh <platform> <out dir>
#   platform: linux-amd64, windows-amd64 or darwin-arm64
#
# The release is the tag in packaging/wallet-tag (or WALLET_TAG) of the public repository
# CreditsCommunity/wallet, and the archive must have the SHA-256 pinned in packaging/wallet-sha256
# (the release's SHA256SUMS, kept in this repository: the two files are bumped together). Instead
# of that file, WALLET_SUMS_SHA256 may name the SHA-256 of the release's SHA256SUMS file itself:
# the file is then downloaded, checked against it and used as the pin. The release's SHA256SUMS is
# otherwise informational: it is compared with the pin, and a difference is reported.
# WALLET_UNPINNED=1 (test builds of another tag only, never a published release: release.yml
# refuses it) takes the release's SHA256SUMS as the pin.
# The pin used is written to <out dir>/wallet-pin.txt (for the release notes; not packaged).
#
# The archive is downloaded anonymously over HTTPS only (or taken from WALLET_ASSETS, a folder
# holding it, e.g. from `gh release download`). <out dir> receives the binary (credits-wallet or
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

url=https://github.com/CreditsCommunity/wallet/releases/download/$tag
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
get() {  # get <file>: from WALLET_ASSETS, or downloaded over HTTPS only (redirects too)
    if [ -n "${WALLET_ASSETS:-}" ]; then
        cp "$WALLET_ASSETS/$1" "$work/$1"
    else
        curl -sSfL --proto '=https' --proto-redir '=https' --retry 3 -o "$work/$1" "$url/$1" \
            || { echo "cannot download $url/$1 (the release must exist and the repository be public)" >&2; return 1; }
    fi
}
sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

# the pin
if [ -n "${WALLET_SUMS_SHA256:-}" ]; then
    get SHA256SUMS
    [ "$(sha256 "$work/SHA256SUMS")" = "$WALLET_SUMS_SHA256" ] \
        || { echo "the SHA256SUMS of $tag does not have the SHA-256 given (WALLET_SUMS_SHA256)" >&2; exit 1; }
    cp "$work/SHA256SUMS" "$work/pin"
    WALLET_PIN=$work/pin "$here/check-wallet-pin.sh" "$tag" >/dev/null
    pin=$work/pin
elif [ "${WALLET_UNPINNED:-}" = 1 ]; then
    echo "WARNING: $tag is not pinned: its own SHA256SUMS is the pin (a test build, never a release)" >&2
    get SHA256SUMS
    cp "$work/SHA256SUMS" "$work/pin"
    WALLET_PIN=$work/pin "$here/check-wallet-pin.sh" "$tag" >/dev/null
    pin=$work/pin
else
    [ -z "${WALLET_TAG:-}" ] || [ "$WALLET_TAG" = "$(tr -d '[:space:]' < "$here/wallet-tag")" ] \
        || { echo "WALLET_TAG $WALLET_TAG is not packaging/wallet-tag: give WALLET_SUMS_SHA256 too" >&2; exit 1; }
    "$here/check-wallet-pin.sh" "$tag" >/dev/null
    pin=$here/wallet-sha256
fi
want=$(awk -v f="$archive" '$2 == f { print $1 }' "$pin")
[ -n "$want" ] || { echo "no pinned hash for $archive" >&2; exit 1; }

get "$archive"
got=$(sha256 "$work/$archive")
[ "$got" = "$want" ] || { echo "$archive: SHA-256 $got, the pinned one is $want: refused" >&2; exit 1; }
echo "$archive: matches the pinned SHA-256 $want"

# the release's own SHA256SUMS: informational
if [ ! -f "$work/SHA256SUMS" ] && get SHA256SUMS 2>/dev/null; then :; fi
if [ -f "$work/SHA256SUMS" ]; then
    rel=$(awk -v f="$archive" '$2 == f { print $1 }' "$work/SHA256SUMS")
    if [ "$rel" = "$want" ]; then echo "the release's SHA256SUMS agrees"
    else echo "WARNING: the release's SHA256SUMS says '${rel:-nothing}' for $archive (the pin decides)" >&2; fi
fi

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
cp "$pin" "$out/wallet-pin.txt"
echo "credits-wallet $version ($platform) in $out"
