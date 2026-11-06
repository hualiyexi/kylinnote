#!/usr/bin/env bash
# Native Debian packaging. --arch verifies the host AND ELF architecture; it is
# not a cross-compilation switch. System and bundle builds use separate caches.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ARCH="$(dpkg --print-architecture)"
VERSION=""
QT_PREFIX=""
QMAKE=""
BUILD_DIR=""
INSTALL_PREFIX="/usr"
BUNDLE_QT=OFF
DO_STRIP=yes
USE_SHLIBDEPS=no
JOBS="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"

usage() {
    printf '%s\n' \
        'Usage: bash packaging/build-deb.sh [--bundle-qt] [--arch arm64|amd64]' \
        '       [--version VERSION] [--qt-prefix DIR] [--qmake FILE]' \
        '       [--build-dir DIR] [--prefix /usr] [--jobs N] [--no-strip] [--shlibdeps]'
}
die() { echo "ERROR: $*" >&2; exit 1; }
while (($#)); do
    case "$1" in
        --arch|--version|--qt-prefix|--qmake|--build-dir|--prefix|--jobs)
            (($# >= 2)) || die "$1 requires a value"
            case "$1" in
                --arch) ARCH="$2";; --version) VERSION="$2";;
                --qt-prefix) QT_PREFIX="$2";; --qmake) QMAKE="$2";;
                --build-dir) BUILD_DIR="$2";; --prefix) INSTALL_PREFIX="$2";;
                --jobs) JOBS="$2";;
            esac
            shift 2;;
        --bundle-qt) BUNDLE_QT=ON; shift;;
        --no-strip) DO_STRIP=no; shift;;
        --shlibdeps) USE_SHLIBDEPS=yes; shift;;
        -h|--help) usage; exit 0;;
        *) die "Unknown argument: $1";;
    esac
done
for tool in cmake ctest dpkg-deb readelf ldd file; do
    command -v "$tool" >/dev/null || die "Missing tool: $tool"
done
[[ "$ARCH" == "$(dpkg --print-architecture)" ]] || die "--arch=$ARCH does not match this native build host"
case "$ARCH:$(uname -m)" in
    arm64:aarch64) ELF_MACHINE='AArch64';;
    amd64:x86_64) ELF_MACHINE='Advanced Micro Devices X86-64';;
    *) die "Unsupported native architecture: $ARCH / $(uname -m)";;
esac
[[ "$INSTALL_PREFIX" == /* && "$INSTALL_PREFIX" != *..* ]] || die 'Prefix must be an absolute path without ..'
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die '--jobs must be a positive integer'
if [[ -z "$VERSION" ]]; then
    VERSION="$(sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9][0-9A-Za-z.+~-]*).*/\1/p' "$PROJECT_DIR/CMakeLists.txt" | head -n1)"
fi
[[ "$VERSION" =~ ^[0-9][0-9A-Za-z.+~-]*$ ]] || die 'Invalid version (or no VERSION in CMakeLists.txt)'
MODE=system
[[ "$BUNDLE_QT" == ON ]] && MODE=bundle
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-deb-$ARCH-$MODE}"
mkdir -p "$BUILD_DIR"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
[[ "$BUILD_DIR" != / && "$BUILD_DIR" != "$PROJECT_DIR" ]] || die 'Use a separate build directory'

# Explicit OFF is essential: reusing an old bundle cache must not turn a normal
# package into a half-bundle. The default cache paths are also mode-specific.
CMAKE_ARGS=(-S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" -DKYLINNOTE_BUNDLE_QT="$BUNDLE_QT"
    -DBUILD_TESTING=ON -DKYLINNOTE_REQUIRE_X11=ON)
[[ -z "$QT_PREFIX" ]] || CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="$QT_PREFIX")
echo "==> Configure $VERSION / $ARCH / $MODE"
cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" --parallel "$JOBS"
cmake -E chdir "$BUILD_DIR" ctest --output-on-failure

PKG_NAME="kylinnote_${VERSION}_${ARCH}"
[[ "$BUNDLE_QT" == OFF ]] || PKG_NAME="${PKG_NAME}_bundle"
STAGING="$BUILD_DIR/staging/$PKG_NAME"
# Only remove the staging child of this verified build directory.
[[ "$STAGING" == "$BUILD_DIR/staging/"* ]] || die 'Unsafe staging path'
rm -rf -- "$STAGING"
mkdir -p "$STAGING/DEBIAN"
DESTDIR="$STAGING" cmake --install "$BUILD_DIR"

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
    LIBDIR="$STAGING$INSTALL_PREFIX/lib/kylinnote"
    BIN="$LIBDIR/kylinnote"
    [[ -x "$BIN" ]] || die "Bundle executable missing: $BIN"
    verify_architecture "$BIN"
    if [[ -z "$QMAKE" ]]; then
        if [[ -n "$QT_PREFIX" ]]; then QMAKE="$QT_PREFIX/bin/qmake"; else QMAKE="$(command -v qmake || true)"; fi
    fi
    [[ -x "$QMAKE" ]] || die 'qmake from the selected Qt installation is required for plugin discovery'
    PLUGIN_SRC="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
    [[ -d "$PLUGIN_SRC" ]] || die "Qt plugin directory is missing: $PLUGIN_SRC"
    echo "==> Qt plugins: $PLUGIN_SRC ($("$QMAKE" -query QT_VERSION))"
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
        local elf="$1" output path soname
        output="$(LC_ALL=C LD_LIBRARY_PATH="$LIBDIR" ldd "$elf")" || die "ldd failed: $elf"
        [[ "$output" != *'not found'* ]] || die "Unresolved dependency on build host: $elf\n$output"
        while IFS= read -r path; do
            [[ -n "$path" ]] || continue
            soname="$(basename "$path")"
            if ! is_system_library "$soname" && [[ ! -e "$LIBDIR/$soname" ]]; then
                cp -L -- "$path" "$LIBDIR/$soname"
            fi
        done < <(printf '%s\n' "$output" | awk '/=> \// {print $3}')
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
    DEPENDS='libc6 (>= 2.31), libstdc++6, libgcc-s1, libx11-6, libx11-xcb1, libxext6, libxrender1, libxi6, libsm6, libice6, libxcb1, libxcb-glx0, libxcb-icccm4, libxcb-image0, libxcb-keysyms1, libxcb-randr0, libxcb-render-util0, libxcb-render0, libxcb-shape0, libxcb-shm0, libxcb-sync1, libxcb-xfixes0, libxcb-xinerama0, libxcb-xkb1, libxkbcommon0, libxkbcommon-x11-0, libgl1, libegl1, libfontconfig1, libfreetype6, libglib2.0-0, libdbus-1-3, zlib1g'
else
    BIN="$STAGING$INSTALL_PREFIX/bin/kylinnote"
    verify_architecture "$BIN"
    DEPENDS='libc6 (>= 2.31), libstdc++6, libgcc-s1, libqt5core5a (>= 5.12), libqt5gui5 (>= 5.12), libqt5widgets5 (>= 5.12), libqt5sql5 (>= 5.12), libqt5network5 (>= 5.12), libqt5dbus5 (>= 5.12), libqt5sql5-sqlite, libqt5svg5, libx11-6'
    if [[ "$USE_SHLIBDEPS" == yes ]]; then
        command -v dpkg-shlibdeps >/dev/null || die 'Install dpkg-dev for --shlibdeps'
        mkdir -p "$BUILD_DIR/debian"
        printf 'Source: kylinnote\nSection: utils\nPriority: optional\nMaintainer: KylinNote <kylinnote@localhost>\n\nPackage: kylinnote\nArchitecture: any\nDescription: Desktop notes and tasks\n' > "$BUILD_DIR/debian/control"
        DEPS="$(cd "$BUILD_DIR" && dpkg-shlibdeps -O -e"$BIN")"
        DEPENDS="${DEPS#shlibs:Depends=}, libqt5sql5-sqlite, libqt5svg5"
    fi
    if [[ "$DO_STRIP" == yes ]]; then strip --strip-unneeded "$BIN"; fi
fi
# Record the ACTUAL ABI requirements. Building on glibc 2.31 does not imply
# every object needs 2.31 (and does not prove compatibility with every Kylin).
ABI_LINES="$BUILD_DIR/abi-requirements.txt"
GLIBC_VERSIONS="$BUILD_DIR/glibc-versions.txt"
GLIBCXX_VERSIONS="$BUILD_DIR/glibcxx-versions.txt"
: > "$ABI_LINES"
: > "$GLIBC_VERSIONS"
: > "$GLIBCXX_VERSIONS"
while IFS= read -r -d '' elf; do
    readelf -h "$elf" >/dev/null 2>&1 || continue
    verify_architecture "$elf"
    versions="$(LC_ALL=C readelf --version-info "$elf")"
    glibc="$(printf '%s\n' "$versions" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)*' | sed 's/^GLIBC_//' | sort -Vu | tail -n1 || true)"
    glibcxx="$(printf '%s\n' "$versions" | grep -oE 'GLIBCXX_[0-9]+(\.[0-9]+)*' | sed 's/^GLIBCXX_//' | sort -Vu | tail -n1 || true)"
    [[ -z "$glibc" ]] || printf '%s\n' "$glibc" >> "$GLIBC_VERSIONS"
    [[ -z "$glibcxx" ]] || printf '%s\n' "$glibcxx" >> "$GLIBCXX_VERSIONS"
    printf '%s: GLIBC=%s GLIBCXX=%s\n' "${elf#"$STAGING"}" "${glibc:-none}" "${glibcxx:-none}" >> "$ABI_LINES"
done < <(find "$STAGING$INSTALL_PREFIX" -type f -print0)
MAX_GLIBC="$(sort -Vu "$GLIBC_VERSIONS" | tail -n1)"
MAX_GLIBCXX="$(sort -Vu "$GLIBCXX_VERSIONS" | tail -n1)"
[[ -n "$MAX_GLIBC" ]] || die 'Could not determine the executable GLIBC requirement'
DEPENDS="$(printf '%s\n' "$DEPENDS" | sed -E "s/libc6( \\([^)]*\\))?/libc6 (>= $MAX_GLIBC)/")"
mkdir -p "$STAGING$INSTALL_PREFIX/share/doc/kylinnote"
{
    printf 'KylinNote %s / %s / %s\n' "$VERSION" "$ARCH" "$MODE"
    printf 'Build environment: %s; Qt %s\n' "$(getconf GNU_LIBC_VERSION)" "$("${QMAKE:-qmake}" -query QT_VERSION)"
    printf 'Maximum required symbol versions: GLIBC_%s; GLIBCXX_%s\n' "$MAX_GLIBC" "${MAX_GLIBCXX:-none}"
    printf 'Reference build: Ubuntu 20.04 / Qt 5.12. This is not a guarantee for all Kylin systems.\n'
    printf 'System dependencies: %s\n\n' "$DEPENDS"
    cat "$ABI_LINES"
} > "$STAGING$INSTALL_PREFIX/share/doc/kylinnote/compatibility.txt"
sed -i "s/^Depends:.*/Depends: $DEPENDS/" "$STAGING/DEBIAN/control"
INSTALLED_SIZE="$(du -sk "$STAGING$INSTALL_PREFIX" | cut -f1)"
printf 'Installed-Size: %s\n' "$INSTALLED_SIZE" >> "$STAGING/DEBIAN/control"
# Small /tmp filesystems are common on deployment machines.
mkdir -p "$BUILD_DIR/tmp"
export TMPDIR="$BUILD_DIR/tmp"
OUTPUT="$PROJECT_DIR/$PKG_NAME.deb"
dpkg-deb --build --root-owner-group "$STAGING" "$OUTPUT"
[[ "$(dpkg-deb -f "$OUTPUT" Architecture)" == "$ARCH" ]] || die 'Package architecture mismatch'
echo "==> Built $OUTPUT"
dpkg-deb --info "$OUTPUT"
