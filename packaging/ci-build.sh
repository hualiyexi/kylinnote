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

# The expected release version is passed through the KYLINLIST_EXPECT_VERSION
# environment variable (chosen over a positional argument so CI can forward it
# through `docker run -e` without changing the call below). CI packages are
# never version experiments: it must equal the CMake project version.
CMAKE_VERSION="$(sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9][0-9A-Za-z.+~-]*).*/\1/p' CMakeLists.txt | head -n1)"
[[ -n "$CMAKE_VERSION" ]] || { echo 'ERROR: cannot read the project version from CMakeLists.txt' >&2; exit 1; }
if [[ -n "${KYLINLIST_EXPECT_VERSION:-}" && "$KYLINLIST_EXPECT_VERSION" != "$CMAKE_VERSION" ]]; then
    echo "ERROR: KYLINLIST_EXPECT_VERSION=$KYLINLIST_EXPECT_VERSION does not match the CMake project version $CMAKE_VERSION" >&2
    exit 1
fi

bash packaging/prepare-ci-apt.sh
apt-get install -y --no-install-recommends \
    build-essential cmake binutils file dpkg-dev ca-certificates \
    qtbase5-dev qtbase5-dev-tools qt5-qmake \
    libqt5sql5-sqlite libqt5svg5 libqt5svg5-dev \
    libx11-dev libxext-dev libxrender-dev libxi-dev libgl1-mesa-dev \
    libxcb-xinerama0 libxkbcommon-x11-0

# Each mode has its own cache/staging tree and writes directly to dist.
mkdir -p dist
bash packaging/build-deb.sh --arch "$PACKAGE_ARCH" --version "$CMAKE_VERSION" --build-dir "$PWD/build-deb-$PACKAGE_ARCH-system" --output-dir "$PWD/dist"
bash packaging/build-deb.sh --arch "$PACKAGE_ARCH" --bundle-qt --version "$CMAKE_VERSION" --build-dir "$PWD/build-deb-$PACKAGE_ARCH-bundle" --output-dir "$PWD/dist"
find dist -maxdepth 1 -name '*.deb' -exec ls -lh {} \;
