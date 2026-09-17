#!/bin/sh
# Downloads libopus .debs without sudo and extracts them into a local prefix.
# Run inside server/litecrab (the single true source of this server code).
# Afterwards configure the WSL build with:
#   sh scripts/fetch_opus_deb.sh
#   LITECRAB_OPUS_PREFIX=$PWD/third_party/opus-deb cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Release
set -e
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PREFIX="$ROOT/third_party/opus-deb"
mkdir -p "$PREFIX/debs"
cd "$PREFIX/debs"
rm -f ./*.deb
apt-get download libopus-dev libopus0
for f in ./*.deb; do dpkg -x "$f" "$PREFIX"; done
rm -rf "$PREFIX/include" "$PREFIX/lib"
ln -sfn "$PREFIX/usr/include" "$PREFIX/include"
ln -sfn "$PREFIX/usr/lib" "$PREFIX/lib"
echo "libopus extracted to: $PREFIX"
echo "build with: LITECRAB_OPUS_PREFIX=$PREFIX cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Release"
