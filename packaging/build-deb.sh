#!/usr/bin/env bash
# Native Debian packaging. --arch verifies the host AND ELF architecture; it is
# not a cross-compilation switch. System and bundle builds use separate caches.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ARCH=""
VERSION=""
QT_PREFIX=""
QMAKE=""
BUILD_DIR=""
OUTPUT_DIR=""
QT_CHECK_METADATA=""
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
        '       --check-qt-selection METADATA [--qmake FILE] [--qt-prefix DIR] [--bundle-qt]' \
        'Builds an xz-compressed .deb and a .deb.sha256 file without overwriting either.' \
        '--check-qt-selection uses trusted qmake/ldd and does not configure or build.' \
        'The selected qmake, CMake Qt libraries and bundle plugins must match.'
}
die() { echo "ERROR: $*" >&2; exit 1; }
while (($#)); do
    case "$1" in
        --arch|--version|--qt-prefix|--qmake|--build-dir|--output-dir|--check-qt-selection|--prefix|--jobs)
            (($# >= 2)) || die "$1 requires a value"
            [[ -n "$2" ]] || die "$1 requires a nonempty value"
            case "$1" in
                --arch) ARCH="$2";; --version) VERSION="$2";;
                --qt-prefix) QT_PREFIX="$2";; --qmake) QMAKE="$2";;
                --build-dir) BUILD_DIR="$2";; --output-dir) OUTPUT_DIR="$2";;
                --check-qt-selection) QT_CHECK_METADATA="$2";; --prefix) INSTALL_PREFIX="$2";;
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

# Also available as a read-only diagnostic: no configure/build/install is needed.
# Do not source the CMake-generated file: it is data, not shell code.
qt_metadata_value() {
    local key="$1" lines
    lines="$(grep -c "^${key}=" "$QT_METADATA" || true)"
    [[ "$lines" == 1 ]] || die "Qt metadata must contain exactly one $key entry: $QT_METADATA"
    sed -n "s/^${key}=//p" "$QT_METADATA"
}
verify_qt_plugin() {
    local plugin="$1" output line name path actual expected count=0
    output="$(LC_ALL=C LD_LIBRARY_PATH="$QT_LIB_DIR" ldd "$plugin")" \
        || die "Cannot resolve Qt plugin dependencies: $plugin"
    [[ "$output" != *'not found'* ]] || die "Unresolved Qt plugin dependency: $plugin"
    while IFS= read -r line; do
        if [[ "$line" =~ ^[[:space:]]*(libQt5[^[:space:]]+)[[:space:]]+\=\>[[:space:]]+(.*)[[:space:]]+\(0x[[:xdigit:]]+\)$ ]]; then
            name="${BASH_REMATCH[1]}"; path="${BASH_REMATCH[2]}"
            actual="$(readlink -e -- "$path")" || die "Cannot resolve $name from plugin $plugin"
            expected="$(readlink -e -- "$QT_LIB_DIR/$name")" || die "Selected Qt does not contain $name (plugin $plugin)"
            [[ "$actual" == "$expected" ]] || die "Plugin $plugin resolves $name from a different Qt: $actual (expected $expected)"
            ((count += 1))
        elif [[ "$line" =~ ^[[:space:]]*libQt5 ]]; then
            die "Cannot interpret Qt dependency of $plugin: $line"
        fi
    done <<< "$output"
    ((count > 0)) || die "Plugin has no resolved Qt dependencies: $plugin"
}
verify_qt_selection() {
    local configured_version cmake_dir expected_dir component configured actual expected candidate matched plugin
    QT_METADATA="$1"
    [[ -f "$QT_METADATA" ]] || die "CMake Qt metadata is missing: $QT_METADATA"
    if [[ -z "$QMAKE" ]]; then
        if [[ -n "$QT_PREFIX" ]]; then
            QMAKE="$QT_PREFIX/bin/qmake"
        else
            QMAKE="$(command -v qmake || true)"
            # Debian/Ubuntu may use qtchooser without a configured default.
            # Respect an explicit caller choice and still verify the result.
            if [[ -z "${QT_SELECT:-}" ]]; then export QT_SELECT=qt5; fi
        fi
    fi
    QMAKE="$(command -v -- "$QMAKE" || true)"
    [[ -n "$QMAKE" && -x "$QMAKE" ]] || die 'qmake from the selected Qt installation is required'
    QT_VERSION="$("$QMAKE" -query QT_VERSION)" || die "qmake QT_VERSION query failed: $QMAKE"
    QT_PREFIX_REAL="$("$QMAKE" -query QT_INSTALL_PREFIX)" || die 'qmake QT_INSTALL_PREFIX query failed'
    QT_LIB_DIR="$("$QMAKE" -query QT_INSTALL_LIBS)" || die 'qmake QT_INSTALL_LIBS query failed'
    PLUGIN_SRC="$("$QMAKE" -query QT_INSTALL_PLUGINS)" || die 'qmake QT_INSTALL_PLUGINS query failed'
    [[ "$QT_VERSION" =~ ^5\.[0-9]+\.[0-9]+$ ]] || die "Unsupported qmake Qt version: $QT_VERSION"
    QT_PREFIX_REAL="$(readlink -e -- "$QT_PREFIX_REAL")" || die 'qmake Qt prefix does not exist'
    QT_LIB_DIR="$(readlink -e -- "$QT_LIB_DIR")" || die 'qmake Qt library directory does not exist'
    PLUGIN_SRC="$(readlink -e -- "$PLUGIN_SRC")" || die 'qmake Qt plugin directory does not exist'
    [[ -d "$QT_PREFIX_REAL" && -d "$QT_LIB_DIR" && -d "$PLUGIN_SRC" ]] || die 'qmake returned an invalid Qt directory'
    if [[ -n "$QT_PREFIX" ]]; then
        configured="$(readlink -e -- "$QT_PREFIX")" || die "Qt prefix does not exist: $QT_PREFIX"
        [[ "$configured" == "$QT_PREFIX_REAL" ]] || die "--qt-prefix does not match qmake: $configured != $QT_PREFIX_REAL"
    fi
    # Typical Qt SDK layout and Debian/Ubuntu multiarch layout. A shared /usr
    # prefix alone cannot establish that the plugins belong to the same Qt.
    matched=no
    for candidate in "$QT_PREFIX_REAL/plugins" "$QT_LIB_DIR/qt5/plugins" "$QT_LIB_DIR/plugins"; do
        [[ -d "$candidate" ]] || continue
        [[ "$(readlink -e -- "$candidate")" != "$PLUGIN_SRC" ]] || matched=yes
    done
    [[ "$matched" == yes ]] || die "qmake plugin directory does not match its Qt prefix/library layout: $PLUGIN_SRC"
    configured_version="$(qt_metadata_value Qt5Core_VERSION)"
    [[ "$configured_version" == "$QT_VERSION" ]] || die "CMake Qt $configured_version differs from qmake Qt $QT_VERSION"
    configured="$(qt_metadata_value Qt5Core_DIR)"
    cmake_dir="$(readlink -e -- "$configured")" || die "CMake Qt5Core_DIR does not exist: $configured"
    expected_dir="$(readlink -e -- "$QT_LIB_DIR/cmake/Qt5Core")" || die "Qt5Core CMake package missing under qmake library directory: $QT_LIB_DIR"
    [[ "$cmake_dir" == "$expected_dir" ]] || die "CMake Qt5Core_DIR differs from qmake: $cmake_dir != $expected_dir"
    for component in Core Gui Widgets Sql Network DBus Test; do
        configured="$(qt_metadata_value "Qt5$component")"
        actual="$(readlink -e -- "$configured")" || die "CMake Qt5$component library does not exist: $configured"
        expected="$(readlink -e -- "$QT_LIB_DIR/libQt5${component}.so.5")" \
            || die "qmake Qt5$component library not found under $QT_LIB_DIR"
        [[ "$actual" == "$expected" ]] || die "CMake Qt5$component differs from qmake: $actual != $expected"
    done
    if [[ "$BUNDLE_QT" == ON ]]; then
        for plugin in platforms/libqxcb.so sqldrivers/libqsqlite.so imageformats/libqsvg.so iconengines/libqsvgicon.so; do
            [[ -f "$PLUGIN_SRC/$plugin" ]] || die "Required Qt plugin missing: $PLUGIN_SRC/$plugin"
            verify_qt_plugin "$PLUGIN_SRC/$plugin"
        done
        if [[ -f "$PLUGIN_SRC/xcbglintegrations/libqxcb-glx-integration.so" ]]; then
            verify_qt_plugin "$PLUGIN_SRC/xcbglintegrations/libqxcb-glx-integration.so"
        fi
    fi
    printf '==> Matched Qt %s: CMake %s; libraries %s; plugins %s\n' "$QT_VERSION" "$cmake_dir" "$QT_LIB_DIR" "$PLUGIN_SRC"
}

if [[ -n "$QT_CHECK_METADATA" ]]; then
    for tool in readlink grep sed; do command -v "$tool" >/dev/null || die "Missing tool: $tool"; done
    [[ "$BUNDLE_QT" == OFF ]] || command -v ldd >/dev/null || die 'Missing tool: ldd'
    verify_qt_selection "$QT_CHECK_METADATA"
    exit 0
fi
for tool in cmake ctest dpkg dpkg-deb ar xz sha256sum readelf readlink ldd file cmp; do
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
if [[ -z "$VERSION" ]]; then
    VERSION="$(sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9][0-9A-Za-z.+~-]*).*/\1/p' "$PROJECT_DIR/CMakeLists.txt" | head -n1)"
fi
[[ "$VERSION" =~ ^[0-9][0-9A-Za-z.+~-]*$ ]] || die 'Invalid version (or no VERSION in CMakeLists.txt)'
MODE=system
[[ "$BUNDLE_QT" == ON ]] && MODE=bundle
PKG_NAME="kylinnote_${VERSION}_${ARCH}"
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
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" -DKYLINNOTE_BUNDLE_QT="$BUNDLE_QT"
    -DBUILD_TESTING=ON -DKYLINNOTE_WITH_DBUS=ON -DKYLINNOTE_REQUIRE_X11=ON -DKYLINNOTE_RECORD_QT_METADATA=ON)
[[ -z "$QT_PREFIX" ]] || CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="$QT_PREFIX")
echo "==> Configure $VERSION / $ARCH / $MODE"
cmake "${CMAKE_ARGS[@]}"
verify_qt_selection "$BUILD_DIR/kylinnote-qt-Release.txt"
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
    unset DBUS_STARTER_ADDRESS DBUS_STARTER_BUS_TYPE DBUS_SESSION_BUS_PID XAUTHORITY KYLINNOTE_DATA_DIR
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
    echo "==> Using verified Qt plugins: $PLUGIN_SRC (Qt $QT_VERSION)"
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
        local elf="$1" output line path soname expected actual
        output="$(LC_ALL=C LD_LIBRARY_PATH="$LIBDIR:$QT_LIB_DIR" ldd "$elf")" || die "ldd failed: $elf"
        [[ "$output" != *'not found'* ]] || die "Unresolved dependency on build host: $elf\n$output"
        while IFS= read -r line; do
            [[ "$line" =~ ^[[:space:]]*([^[:space:]]+)[[:space:]]+\=\>[[:space:]]+(/.*)[[:space:]]+\(0x[[:xdigit:]]+\)$ ]] || continue
            soname="${BASH_REMATCH[1]}"; path="${BASH_REMATCH[2]}"
            if [[ "$soname" == libQt5*.so.* ]]; then
                expected="$(readlink -e -- "$QT_LIB_DIR/$soname")" || die "Selected Qt lacks $soname"
                if [[ "$path" == "$LIBDIR/"* ]]; then
                    cmp -s -- "$path" "$expected" || die "Bundled $soname differs from selected Qt: $path"
                else
                    actual="$(readlink -e -- "$path")" || die "Cannot resolve Qt library: $path"
                    [[ "$actual" == "$expected" ]] || die "$elf resolved $soname from another Qt: $actual"
                fi
            fi
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
    DEPENDS='libc6 (>= 2.31), libstdc++6, libgcc-s1, libx11-6, libx11-xcb1, libxext6, libxrender1, libxi6, libsm6, libice6, libxcb1, libxcb-glx0, libxcb-icccm4, libxcb-image0, libxcb-keysyms1, libxcb-randr0, libxcb-render-util0, libxcb-render0, libxcb-shape0, libxcb-shm0, libxcb-sync1, libxcb-xfixes0, libxcb-xinerama0, libxcb-xinput0, libxcb-xkb1, libxkbcommon0, libxkbcommon-x11-0, libgl1, libegl1, libfontconfig1, libfreetype6, libglib2.0-0, libdbus-1-3, zlib1g'
else
    BIN="$STAGING$INSTALL_PREFIX/bin/kylinnote"
    verify_architecture "$BIN"
    # The Qt version used to build is a conservative floor, not an ABI guarantee.
    QT_DEPENDS="libqt5core5a (>= $QT_VERSION), libqt5gui5 (>= $QT_VERSION), libqt5widgets5 (>= $QT_VERSION), libqt5sql5 (>= $QT_VERSION), libqt5network5 (>= $QT_VERSION), libqt5dbus5 (>= $QT_VERSION), libqt5sql5-sqlite (>= $QT_VERSION), libqt5svg5 (>= $QT_VERSION)"
    DEPENDS="libc6, libstdc++6, libgcc-s1, libx11-6, $QT_DEPENDS"
    if [[ "$USE_SHLIBDEPS" == yes ]]; then
        command -v dpkg-shlibdeps >/dev/null || die 'Install dpkg-dev for --shlibdeps'
        # dpkg-shlibdeps expects debian/control in its working directory. Keep
        # this generated file inside our new private staging root, not a reused
        # build directory where it could overwrite an existing file or symlink.
        mkdir -m 0700 "$STAGING_ROOT/debian"
        printf 'Source: kylinnote\nSection: utils\nPriority: optional\nMaintainer: KylinNote <kylinnote@localhost>\n\nPackage: kylinnote\nArchitecture: any\nDescription: Desktop notes and tasks\n' > "$STAGING_ROOT/debian/control"
        DEPS="$(cd "$STAGING_ROOT" && dpkg-shlibdeps -O -e"$BIN")" || die 'dpkg-shlibdeps failed'
        [[ "$DEPS" == shlibs:Depends=* && -n "${DEPS#shlibs:Depends=}" ]] || die 'dpkg-shlibdeps returned no dependencies'
        DEPENDS="${DEPS#shlibs:Depends=}, $QT_DEPENDS"
    fi
    if [[ "$DO_STRIP" == yes ]]; then strip --strip-unneeded "$BIN"; fi
fi
# Record the ACTUAL ABI requirements. Building on glibc 2.31 does not imply
# every object needs 2.31 (and does not prove compatibility with every Kylin).
ABI_LINES="$STAGING_ROOT/abi-requirements.txt"
GLIBC_VERSIONS="$STAGING_ROOT/glibc-versions.txt"
GLIBCXX_VERSIONS="$STAGING_ROOT/glibcxx-versions.txt"
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
printf 'Per-object symbol versions: %s\n' "$ABI_LINES"
MAX_GLIBC="$(sort -Vu "$GLIBC_VERSIONS" | tail -n1)"
MAX_GLIBCXX="$(sort -Vu "$GLIBCXX_VERSIONS" | tail -n1)"
[[ -n "$MAX_GLIBC" ]] || die 'Could not determine the executable GLIBC requirement'
if [[ "$BUNDLE_QT" == OFF && "$USE_SHLIBDEPS" == yes ]]; then
    # Keep any stronger dpkg-shlibdeps constraint; Debian combines both terms.
    DEPENDS="$DEPENDS, libc6 (>= $MAX_GLIBC)"
else
    DEPENDS="$(printf '%s\n' "$DEPENDS" | sed -E "s/libc6( \\([^)]*\\))?/libc6 (>= $MAX_GLIBC)/")"
fi
mkdir -p "$STAGING$INSTALL_PREFIX/share/doc/kylinnote"
{
    printf 'KylinNote %s / %s / %s\n' "$VERSION" "$ARCH" "$MODE"
    printf 'Build environment: %s; Qt %s\n' "$(getconf GNU_LIBC_VERSION)" "$QT_VERSION"
    printf 'Maximum required symbol versions: GLIBC_%s; GLIBCXX_%s\n' "$MAX_GLIBC" "${MAX_GLIBCXX:-none}"
    printf 'Qt version floors and recorded symbols do not guarantee ABI or target-system compatibility.\n'
    printf 'System dependencies: %s\n\n' "$DEPENDS"
    cat "$ABI_LINES"
} > "$STAGING$INSTALL_PREFIX/share/doc/kylinnote/compatibility.txt"
sed -i "s/^Depends:.*/Depends: $DEPENDS/" "$STAGING/DEBIAN/control"
INSTALLED_SIZE="$(du -sk "$STAGING$INSTALL_PREFIX" | cut -f1)"
printf 'Installed-Size: %s\n' "$INSTALLED_SIZE" >> "$STAGING/DEBIAN/control"
# Build and verify privately on the output filesystem, then publish with hard
# links so neither a concurrent build nor an existing artifact is overwritten.
export TMPDIR="$TEST_ENV/tmp"
OUTPUT_TMP="$(mktemp -d "$OUTPUT_DIR/.${PKG_NAME}.XXXXXXXX")"
TEMP_PACKAGE="$OUTPUT_TMP/$PKG_NAME.deb"
dpkg-deb -Zxz --build --root-owner-group "$STAGING" "$TEMP_PACKAGE"
MEMBERS="$(ar t "$TEMP_PACKAGE")" || die 'Cannot inspect deb ar archive'
[[ "$MEMBERS" == $'debian-binary\ncontrol.tar.xz\ndata.tar.xz' ]] \
    || die "Unexpected deb ar members (control and data must both be xz): $MEMBERS"
[[ "$(ar p "$TEMP_PACKAGE" debian-binary)" == 2.0 ]] || die 'Unexpected debian-binary version'
ar p "$TEMP_PACKAGE" control.tar.xz | xz -t || die 'Invalid xz control archive'
ar p "$TEMP_PACKAGE" data.tar.xz | xz -t || die 'Invalid xz data archive'
[[ "$(dpkg-deb -f "$TEMP_PACKAGE" Architecture)" == "$ARCH" ]] || die 'Package architecture mismatch'
[[ "$(dpkg-deb -f "$TEMP_PACKAGE" Version)" == "$VERSION" ]] || die 'Package version mismatch'
(cd "$OUTPUT_TMP" && sha256sum "$PKG_NAME.deb" > "$PKG_NAME.deb.sha256")
dpkg-deb --info "$TEMP_PACKAGE"
ln -T -- "$TEMP_PACKAGE" "$OUTPUT" || die "Cannot publish package without overwriting: $OUTPUT"
if ! ln -T -- "$OUTPUT_TMP/$PKG_NAME.deb.sha256" "$OUTPUT_HASH"; then
    [[ ! "$TEMP_PACKAGE" -ef "$OUTPUT" ]] || rm -- "$OUTPUT"
    die "Cannot publish checksum without overwriting: $OUTPUT_HASH"
fi
printf '==> Built %s\n==> SHA-256: %s\n' "$OUTPUT" "$(<"$OUTPUT_HASH")"
