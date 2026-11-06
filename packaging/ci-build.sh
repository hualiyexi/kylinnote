#!/usr/bin/env bash
# Run in a native ARM64 ubuntu:20.04 container. glibc 2.31 / Qt 5.12
# is a compatibility reference, not proof of support for every Kylin system.
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

# Each mode explicitly configures ON/OFF and has its own cache/staging tree.
bash packaging/build-deb.sh --arch arm64 --build-dir "$PWD/build-deb-arm64-system"
bash packaging/build-deb.sh --arch arm64 --bundle-qt --build-dir "$PWD/build-deb-arm64-bundle"
mkdir -p dist
for package in kylinnote_*_arm64.deb kylinnote_*_arm64_bundle.deb; do
    [[ -f "$package" ]] || continue
    # Ignore historical .deb files in the checkout; retain only this build's version.
    built_version="$(sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9][0-9A-Za-z.+~-]*).*/\1/p' CMakeLists.txt | head -n1)"
    [[ "$(dpkg-deb -f "$package" Version)" == "$built_version" ]] || continue
    cp "$package" dist/
done
find dist -maxdepth 1 -name '*.deb' -exec ls -lh {} \;
