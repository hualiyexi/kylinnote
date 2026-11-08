#!/usr/bin/env bash
# Build ARM64 packages in Ubuntu 20.04 with the distribution's Qt 5.12.8.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
export DEBIAN_FRONTEND=noninteractive
[[ "$(dpkg --print-architecture)" == arm64 && "$(uname -m)" == aarch64 ]] || {
    echo 'CI requires a real ARM64 userspace/kernel; --arch only relabels nothing.' >&2
    exit 1
}
if ! apt-get update -o APT::Update::Error-Mode=any; then
    sed -i -E \
        -e 's|https?://(archive.ubuntu.com/ubuntu|security.ubuntu.com/ubuntu|ports.ubuntu.com/ubuntu-ports)|http://old-releases.ubuntu.com/ubuntu|g' \
        /etc/apt/sources.list
    apt-get update -o APT::Update::Error-Mode=any
fi
apt-get install -y --no-install-recommends \
    build-essential cmake binutils file dpkg-dev ca-certificates \
    qtbase5-dev qtbase5-dev-tools qt5-qmake \
    libqt5sql5-sqlite libqt5svg5 libqt5svg5-dev \
    libx11-dev libxext-dev libxrender-dev libxi-dev libgl1-mesa-dev \
    libxcb-xinerama0 libxkbcommon-x11-0

# Each mode has its own cache/staging tree and writes directly to dist.
mkdir -p dist
bash packaging/build-deb.sh --arch arm64 --build-dir "$PWD/build-deb-arm64-system" --output-dir "$PWD/dist"
bash packaging/build-deb.sh --arch arm64 --bundle-qt --build-dir "$PWD/build-deb-arm64-bundle" --output-dir "$PWD/dist"
find dist -maxdepth 1 -name '*.deb' -exec ls -lh {} \;
