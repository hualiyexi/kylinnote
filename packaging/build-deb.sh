#!/usr/bin/env bash
# Build in Ubuntu 20.04 with its Qt 5.12.8. --arch verifies the host and ELF
# architecture; it is not a cross-compilation switch. Caches are mode-specific.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ARCH=""
VERSION=""
QT_PREFIX=""
QMAKE=""
BUILD_DIR=""
OUTPUT_DIR=""
INSTALL_PREFIX="/usr"
BUNDLE_QT=OFF
DO_STRIP=yes
USE_SHLIBDEPS=no
JOBS="${CMAKE_BUILD_PARALLEL_LEVEL:-}"

usage() {
    printf '%s\n' \
        'Usage: bash packaging/build-deb.sh [--bundle-qt] [--arch arm64|amd64]' \
        '       [--version VERSION] [--qt-prefix DIR] [--qmake FILE]' \
        '       [--build-dir DIR] [--output-dir DIR] [--prefix /usr] [--jobs N]' \
        '       [--no-strip] [--shlibdeps]' \
        'Builds an xz-compressed .deb and a .deb.sha256 file without overwriting either.'
}
die() { echo "ERROR: $*" >&2; exit 1; }
while (($#)); do
    case "$1" in
        --arch|--version|--qt-prefix|--qmake|--build-dir|--output-dir|--prefix|--jobs)
            (($# >= 2)) || die "$1 requires a value"
            [[ -n "$2" ]] || die "$1 requires a nonempty value"
            case "$1" in
                --arch) ARCH="$2";; --version) VERSION="$2";;
                --qt-prefix) QT_PREFIX="$2";; --qmake) QMAKE="$2";;
                --build-dir) BUILD_DIR="$2";; --output-dir) OUTPUT_DIR="$2";;
                --prefix) INSTALL_PREFIX="$2";; --jobs) JOBS="$2";;
            esac
            shift 2;;
        --bundle-qt) BUNDLE_QT=ON; shift;;
        --no-strip) DO_STRIP=no; shift;;
        --shlibdeps) USE_SHLIBDEPS=yes; shift;;
        -h|--help) usage; exit 0;;
        *) die "Unknown argument: $1";;
    esac
done

for tool in cmake ctest dpkg dpkg-deb sha256sum readelf ldd; do
    command -v "$tool" >/dev/null || die "Missing tool: $tool"
done
[[ "$DO_STRIP" == no ]] || command -v strip >/dev/null || die 'Missing tool: strip'
NATIVE_ARCH="$(dpkg --print-architecture)"
ARCH="${ARCH:-$NATIVE_ARCH}"
[[ "$ARCH" == "$NATIVE_ARCH" ]] || die "--arch=$ARCH does not match this native build host ($NATIVE_ARCH)"
case "$ARCH:$(uname -m)" in
    arm64:aarch64) ELF_MACHINE='AArch64';;
    amd64:x86_64) ELF_MACHINE='Advanced Micro Devices X86-64';;
    *) die "Unsupported native architecture: $ARCH / $(uname -m)";;
esac
[[ "$INSTALL_PREFIX" == /usr ]] || die 'Debian packages require --prefix /usr'
if [[ -z "$JOBS" ]]; then
    command -v nproc >/dev/null || die 'Missing tool: nproc (or specify --jobs)'
    JOBS="$(nproc)"
fi
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die '--jobs must be a positive integer'
# CMakeLists.txt is the single version source. Without --version the package
# always uses it; an explicit --version that differs is only a local experiment
# (the version is part of the artifact name) and can never enter the stable
# update channel, because generate-update-manifest.py only accepts the strict
# CMake X.Y.Z version.
CMAKE_VERSION="$(sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9][0-9A-Za-z.+~-]*).*/\1/p' "$PROJECT_DIR/CMakeLists.txt" | head -n1)"
[[ -n "$CMAKE_VERSION" ]] || die 'No VERSION found in CMakeLists.txt'
if [[ -z "$VERSION" ]]; then
    VERSION="$CMAKE_VERSION"
fi
[[ "$VERSION" =~ ^[0-9][0-9A-Za-z.+~-]*$ ]] || die 'Invalid --version value'
if [[ "$VERSION" != "$CMAKE_VERSION" ]]; then
    echo "WARNING: package version $VERSION differs from the CMake project version $CMAKE_VERSION;" \
        "this experiment package is not eligible for the stable update channel" >&2
fi

# The supported build environment has one Qt installation. Query its normal
# paths directly; no per-library provenance report is needed.
if [[ -z "$QMAKE" ]]; then
    if [[ -n "$QT_PREFIX" ]]; then
        QMAKE="$QT_PREFIX/bin/qmake"
    else
        QMAKE=qmake
        if [[ -z "${QT_SELECT:-}" ]]; then export QT_SELECT=qt5; fi
    fi
fi
QMAKE="$(command -v -- "$QMAKE" || true)"
[[ -n "$QMAKE" && -x "$QMAKE" ]] || die 'qmake is required (install qt5-qmake)'
QT_VERSION="$("$QMAKE" -query QT_VERSION)"
[[ "$QT_VERSION" == 5.12.8 ]] || die "Packaging uses Qt 5.12.8; found $QT_VERSION. Use Ubuntu 20.04."
QT_PREFIX="${QT_PREFIX:-$("$QMAKE" -query QT_INSTALL_PREFIX)}"
QT_LIB_DIR="$("$QMAKE" -query QT_INSTALL_LIBS)"
PLUGIN_SRC="$("$QMAKE" -query QT_INSTALL_PLUGINS)"

MODE=system
[[ "$BUNDLE_QT" == ON ]] && MODE=bundle
# Installation identity written to <prefix>/share/kylinlist/install-info.json.
case "$ARCH" in
    arm64) INFO_ARCH=arm64;;
    amd64) INFO_ARCH=amd64;;
    *) die "Unsupported install-info architecture: $ARCH";;
esac
if [[ "$BUNDLE_QT" == ON ]]; then INFO_VARIANT=bundled-qt; else INFO_VARIANT=system-qt; fi
PKG_NAME="kylinlist_${VERSION}_${ARCH}"
[[ "$BUNDLE_QT" == OFF ]] || PKG_NAME="${PKG_NAME}_bundle"
OUTPUT_DIR="${OUTPUT_DIR:-$PROJECT_DIR}"
mkdir -p -- "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd -P)"
OUTPUT="$OUTPUT_DIR/$PKG_NAME.deb"
OUTPUT_HASH="$OUTPUT.sha256"
[[ ! -e "$OUTPUT" && ! -L "$OUTPUT" && ! -e "$OUTPUT_HASH" && ! -L "$OUTPUT_HASH" ]] \
    || die "Refusing to overwrite package or checksum: $OUTPUT / $OUTPUT_HASH"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-deb-$ARCH-$MODE}"
mkdir -p -- "$BUILD_DIR"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd -P)"
PROJECT_REAL="$(cd "$PROJECT_DIR" && pwd -P)"
[[ "$BUILD_DIR" != / && "$BUILD_DIR" != "$PROJECT_REAL" ]] || die 'Use a separate build directory'
TEST_ENV=""
STAGING_ROOT=""
OUTPUT_TMP=""
cleanup() {
    [[ -z "$TEST_ENV" ]] || rm -rf -- "$TEST_ENV"
    [[ -z "$STAGING_ROOT" ]] || rm -rf -- "$STAGING_ROOT"
    [[ -z "$OUTPUT_TMP" ]] || rm -rf -- "$OUTPUT_TMP"
}
trap cleanup EXIT

# Explicit OFF is essential: reusing an old bundle cache must not turn a normal
# package into a half-bundle. The default cache paths are also mode-specific.
CMAKE_ARGS=(-S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" -DKYLINLIST_BUNDLE_QT="$BUNDLE_QT"
    -DBUILD_TESTING=ON -DKYLINLIST_WITH_DBUS=ON -DKYLINLIST_REQUIRE_X11=ON
    -DCMAKE_PREFIX_PATH="$QT_PREFIX")
echo "==> Configure $VERSION / $ARCH / $MODE / Qt $QT_VERSION"
cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" --config Release --parallel "$JOBS"

# Even direct builds must not let Qt tests touch a real user's settings/bus.
TEST_ENV="$(mktemp -d "$BUILD_DIR/.test-env.XXXXXXXX")"
mkdir -m 0700 "$TEST_ENV/home" "$TEST_ENV/config" "$TEST_ENV/data" "$TEST_ENV/cache" \
    "$TEST_ENV/state" "$TEST_ENV/runtime" "$TEST_ENV/tmp"
(
    export HOME="$TEST_ENV/home" XDG_CONFIG_HOME="$TEST_ENV/config" XDG_DATA_HOME="$TEST_ENV/data"
    export XDG_CACHE_HOME="$TEST_ENV/cache" XDG_STATE_HOME="$TEST_ENV/state" XDG_RUNTIME_DIR="$TEST_ENV/runtime"
    export XDG_CONFIG_DIRS=/etc/xdg XDG_DATA_DIRS=/usr/local/share:/usr/share
    export TMPDIR="$TEST_ENV/tmp" TMP="$TEST_ENV/tmp" TEMP="$TEST_ENV/tmp"
    export DBUS_SESSION_BUS_ADDRESS="unix:path=$TEST_ENV/no-session-bus" DBUS_SYSTEM_BUS_ADDRESS="unix:path=$TEST_ENV/no-system-bus"
    unset DBUS_STARTER_ADDRESS DBUS_STARTER_BUS_TYPE DBUS_SESSION_BUS_PID XAUTHORITY KYLINLIST_DATA_DIR
    for test in tasks ui; do
        test_list="$(cmake -E chdir "$BUILD_DIR" ctest -C Release -N -R "^${test}$")"
        printf '%s\n' "$test_list"
        printf '%s\n' "$test_list" | grep -Eq '^[[:space:]]*Total Tests:[[:space:]]+1[[:space:]]*$' \
            || die "Required CTest suite is missing or duplicated: $test"
    done
    cmake -E chdir "$BUILD_DIR" ctest -C Release --output-on-failure
)

# Never delete a pre-existing staging path: an intermediate symlink could lead
# outside the build tree. Use only a new private directory created by this run.
STAGING_ROOT="$(mktemp -d "$BUILD_DIR/.staging.XXXXXXXX")"
STAGING="$STAGING_ROOT/$PKG_NAME"
mkdir -p "$STAGING/DEBIAN"
DESTDIR="$STAGING" cmake --install "$BUILD_DIR" --config Release

# Machine-readable installation identity consumed by the in-app updater
# (client lookup path on Linux: /usr[/local]/share/kylinlist/install-info.json).
INFO_SHARE_DIR="$STAGING$INSTALL_PREFIX/share/kylinlist"
mkdir -p -- "$INFO_SHARE_DIR"
printf '{"product": "kylinlist", "platform": "linux", "version": "%s", "arch": "%s", "variant": "%s"}\n' \
    "$VERSION" "$INFO_ARCH" "$INFO_VARIANT" > "$INFO_SHARE_DIR/install-info.json"

sed "s/^Version:.*/Version: $VERSION/; s/^Architecture:.*/Architecture: $ARCH/" \
    "$SCRIPT_DIR/deb/DEBIAN/control" > "$STAGING/DEBIAN/control"
install -m 0755 "$SCRIPT_DIR/deb/DEBIAN/postinst" "$STAGING/DEBIAN/postinst"
install -m 0755 "$SCRIPT_DIR/deb/DEBIAN/prerm" "$STAGING/DEBIAN/prerm"

verify_architecture() {
    local elf="$1" machine
    machine="$(LC_ALL=C readelf -h "$elf" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')"
    [[ "$machine" == "$ELF_MACHINE" ]] || die "ELF architecture mismatch: $elf ($machine, wanted $ELF_MACHINE)"
}
# This is a SONAME allowlist, not a pathname regular expression. Everything
# else MUST be physically present in the bundle, even if the build host has it.
is_system_library() {
    case "$1" in
        ld-linux-*.so.*|libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|libresolv.so.*|libutil.so.*|libstdc++.so.*|libgcc_s.so.*) return 0;;
        libX11.so.*|libX11-xcb.so.*|libXext.so.*|libXrender.so.*|libXi.so.*|libSM.so.*|libICE.so.*|libXau.so.*|libXdmcp.so.*|libxcb.so.*|libxcb-*.so.*|libxkbcommon.so.*|libxkbcommon-x11.so.*) return 0;;
        libGL.so.*|libGLX.so.*|libGLdispatch.so.*|libEGL.so.*|libfontconfig.so.*|libfreetype.so.*|libglib-2.0.so.*|libgobject-2.0.so.*|libgthread-2.0.so.*|libz.so.*|libdbus-1.so.*) return 0;;
        *) return 1;;
    esac
}
needed_libraries() {
    LC_ALL=C readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p'
}

if [[ "$BUNDLE_QT" == ON ]]; then
    LIBDIR="$STAGING$INSTALL_PREFIX/lib/kylinlist"
    BIN="$LIBDIR/kylinlist"
    [[ -x "$BIN" ]] || die "Bundle executable missing: $BIN"
    verify_architecture "$BIN"
    echo "==> Using Qt plugins: $PLUGIN_SRC (Qt $QT_VERSION)"
    for plugin in platforms/libqxcb.so sqldrivers/libqsqlite.so imageformats/libqsvg.so iconengines/libqsvgicon.so; do
        [[ -f "$PLUGIN_SRC/$plugin" ]] || die "Required Qt plugin missing: $plugin (install libqt5sql5-sqlite and libqt5svg5)"
        install -D -m 0644 "$PLUGIN_SRC/$plugin" "$LIBDIR/plugins/$plugin"
    done
    # GLX integration is loaded dynamically rather than through DT_NEEDED.
    if [[ -f "$PLUGIN_SRC/xcbglintegrations/libqxcb-glx-integration.so" ]]; then
        install -D -m 0644 "$PLUGIN_SRC/xcbglintegrations/libqxcb-glx-integration.so" \
            "$LIBDIR/plugins/xcbglintegrations/libqxcb-glx-integration.so"
    fi

    collect_dependencies() {
        local elf="$1" output line path soname
        output="$(LC_ALL=C LD_LIBRARY_PATH="$LIBDIR:$QT_LIB_DIR" ldd "$elf")" || die "ldd failed: $elf"
        [[ "$output" != *'not found'* ]] || die "Unresolved dependency on build host: $elf\n$output"
        while IFS= read -r line; do
            [[ "$line" =~ ^[[:space:]]*([^[:space:]]+)[[:space:]]+\=\>[[:space:]]+(/.*)[[:space:]]+\(0x[[:xdigit:]]+\)$ ]] || continue
            soname="${BASH_REMATCH[1]}"; path="${BASH_REMATCH[2]}"
            if ! is_system_library "$soname" && [[ ! -e "$LIBDIR/$soname" ]]; then
                cp -L -- "$path" "$LIBDIR/$soname"
            fi
        done <<< "$output"
    }
    echo '==> Collect Qt and non-system dependencies to a fixed point'
    while :; do
        before="$(find "$LIBDIR" -type f | wc -l)"
        while IFS= read -r -d '' elf; do
            readelf -h "$elf" >/dev/null 2>&1 || continue
            verify_architecture "$elf"
            collect_dependencies "$elf"
        done < <(find "$LIBDIR" -type f -print0)
        after="$(find "$LIBDIR" -type f | wc -l)"
        [[ "$before" != "$after" ]] || break
    done
    for library in Core Gui Widgets Sql Network Svg XcbQpa; do
        [[ -f "$LIBDIR/libQt5${library}.so.5" ]] || die "Required bundled Qt library missing: Qt5$library"
    done
    echo '==> Verify every DT_NEEDED without relying on host Qt resolution'
    while IFS= read -r -d '' elf; do
        readelf -h "$elf" >/dev/null 2>&1 || continue
        verify_architecture "$elf"
        while IFS= read -r need; do
            [[ -f "$LIBDIR/$need" ]] || is_system_library "$need" \
                || die "Dependency is neither bundled nor an allowed base library: $need (needed by $elf)"
        done < <(needed_libraries "$elf")
        if [[ "$DO_STRIP" == yes ]]; then strip --strip-unneeded "$elf"; fi
    done < <(find "$LIBDIR" -type f -print0)

    # These are preinstalled/base graphic dependencies on the target, never Qt.
    # The clean runtime test installs this explicit list BEFORE going offline.
    DEPENDS='libc6 (>= 2.31), libstdc++6, libgcc-s1, tzdata, libx11-6, libx11-xcb1, libxext6, libxrender1, libxi6, libsm6, libice6, libxcb1, libxcb-glx0, libxcb-icccm4, libxcb-image0, libxcb-keysyms1, libxcb-randr0, libxcb-render-util0, libxcb-render0, libxcb-shape0, libxcb-shm0, libxcb-sync1, libxcb-xfixes0, libxcb-xinerama0, libxcb-xinput0, libxcb-xkb1, libxkbcommon0, libxkbcommon-x11-0, libgl1, libegl1, libfontconfig1, libfreetype6, libglib2.0-0, libdbus-1-3, zlib1g'
else
    BIN="$STAGING$INSTALL_PREFIX/bin/kylinlist"
    verify_architecture "$BIN"
    QT_DEPENDS="libqt5core5a (>= $QT_VERSION), libqt5gui5 (>= $QT_VERSION), libqt5widgets5 (>= $QT_VERSION), libqt5sql5 (>= $QT_VERSION), libqt5network5 (>= $QT_VERSION), libqt5dbus5 (>= $QT_VERSION), libqt5sql5-sqlite (>= $QT_VERSION), libqt5svg5 (>= $QT_VERSION)"
    DEPENDS="libc6 (>= 2.31), libstdc++6, libgcc-s1, libx11-6, tzdata, $QT_DEPENDS"
    if [[ "$USE_SHLIBDEPS" == yes ]]; then
        command -v dpkg-shlibdeps >/dev/null || die 'Install dpkg-dev for --shlibdeps'
        # dpkg-shlibdeps expects debian/control in its working directory. Keep
        # this generated file inside our new private staging root, not a reused
        # build directory where it could overwrite an existing file or symlink.
        mkdir -m 0700 "$STAGING_ROOT/debian"
        printf 'Source: kylinlist\nSection: utils\nPriority: optional\nMaintainer: KylinList <kylinlist@localhost>\n\nPackage: kylinlist\nArchitecture: any\nDescription: Local tasks and checklists\n' > "$STAGING_ROOT/debian/control"
        DEPS="$(cd "$STAGING_ROOT" && dpkg-shlibdeps -O -e"$BIN")" || die 'dpkg-shlibdeps failed'
        [[ "$DEPS" == shlibs:Depends=* && -n "${DEPS#shlibs:Depends=}" ]] || die 'dpkg-shlibdeps returned no dependencies'
        DEPENDS="${DEPS#shlibs:Depends=}, tzdata, $QT_DEPENDS"
    fi
    if [[ "$DO_STRIP" == yes ]]; then strip --strip-unneeded "$BIN"; fi
fi
sed -i "s/^Depends:.*/Depends: $DEPENDS/" "$STAGING/DEBIAN/control"
INSTALLED_SIZE="$(du -sk "$STAGING$INSTALL_PREFIX" | cut -f1)"
printf 'Installed-Size: %s\n' "$INSTALLED_SIZE" >> "$STAGING/DEBIAN/control"
# Build and verify privately on the output filesystem, then publish with hard
# links so neither a concurrent build nor an existing artifact is overwritten.
export TMPDIR="$TEST_ENV/tmp"
OUTPUT_TMP="$(mktemp -d "$OUTPUT_DIR/.${PKG_NAME}.XXXXXXXX")"
TEMP_PACKAGE="$OUTPUT_TMP/$PKG_NAME.deb"
dpkg-deb -Zxz --build --root-owner-group "$STAGING" "$TEMP_PACKAGE"
[[ "$(dpkg-deb -f "$TEMP_PACKAGE" Architecture)" == "$ARCH" ]] || die 'Package architecture mismatch'
[[ "$(dpkg-deb -f "$TEMP_PACKAGE" Version)" == "$VERSION" ]] || die 'Package version mismatch'
INFO_CHECK_ROOT="$(mktemp -d "$TEST_ENV/tmp/.install-info.XXXXXXXX")"
dpkg-deb -x "$TEMP_PACKAGE" "$INFO_CHECK_ROOT"
INFO_CHECK_FILE="$INFO_CHECK_ROOT$INSTALL_PREFIX/share/kylinlist/install-info.json"
[[ -f "$INFO_CHECK_FILE" ]] || die 'install-info.json is missing inside the built package'
INFO_EXPECTED="$TEST_ENV/tmp/expected-install-info.json"
printf '{"product": "kylinlist", "platform": "linux", "version": "%s", "arch": "%s", "variant": "%s"}\n' \
    "$VERSION" "$INFO_ARCH" "$INFO_VARIANT" > "$INFO_EXPECTED"
cmp -s "$INFO_CHECK_FILE" "$INFO_EXPECTED" \
    || die "Packaged install-info.json mismatch: $(<"$INFO_CHECK_FILE")"
(cd "$OUTPUT_TMP" && sha256sum "$PKG_NAME.deb" > "$PKG_NAME.deb.sha256")
dpkg-deb --info "$TEMP_PACKAGE"
ln -T -- "$TEMP_PACKAGE" "$OUTPUT" || die "Cannot publish package without overwriting: $OUTPUT"
if ! ln -T -- "$OUTPUT_TMP/$PKG_NAME.deb.sha256" "$OUTPUT_HASH"; then
    [[ ! "$TEMP_PACKAGE" -ef "$OUTPUT" ]] || rm -- "$OUTPUT"
    die "Cannot publish checksum without overwriting: $OUTPUT_HASH"
fi
printf '==> Built %s\n==> SHA-256: %s\n' "$OUTPUT" "$(<"$OUTPUT_HASH")"
