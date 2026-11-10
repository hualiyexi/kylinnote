#!/usr/bin/env bash
# Build native ARM64 or amd64 packages in Ubuntu 20.04 with Qt 5.12.8.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
export DEBIAN_FRONTEND=noninteractive
PACKAGE_ARCH="$(dpkg --print-architecture)"
case "$PACKAGE_ARCH:$(uname -m)" in
    arm64:aarch64|amd64:x86_64) ;;
    *) echo "Unsupported CI architecture: $PACKAGE_ARCH / $(uname -m)" >&2; exit 1;;
esac
bash packaging/prepare-ci-apt.sh
apt-get install -y --no-install-recommends \
    build-essential cmake binutils file dpkg-dev ca-certificates \
    qtbase5-dev qtbase5-dev-tools qt5-qmake \
    libqt5sql5-sqlite libqt5svg5 libqt5svg5-dev \
    libx11-dev libxext-dev libxrender-dev libxi-dev libgl1-mesa-dev \
    libxcb-xinerama0 libxkbcommon-x11-0

# Each mode has its own cache/staging tree and writes directly to dist.
mkdir -p dist
bash packaging/build-deb.sh --arch "$PACKAGE_ARCH" --build-dir "$PWD/build-deb-$PACKAGE_ARCH-system" --output-dir "$PWD/dist"
bash packaging/build-deb.sh --arch "$PACKAGE_ARCH" --bundle-qt --build-dir "$PWD/build-deb-$PACKAGE_ARCH-bundle" --output-dir "$PWD/dist"
find dist -maxdepth 1 -name '*.deb' -exec ls -lh {} \;
