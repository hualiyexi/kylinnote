#!/usr/bin/env bash
# Runtime validation in a NEW ubuntu:20.04 container, with no system Qt.
# Phase 1: --prepare FILE.deb installs only the declared base graphics/runtime
#          dependencies and validation tools. Commit this container to an image.
# Phase 2: run that image with --network none, then execute FILE.deb validation.
# See docs/building.md and .github/workflows/build.yml for exact commands.
set -euo pipefail
PREPARE=no
while (($#)); do
    case "$1" in
        --prepare) PREPARE=yes; shift;;
        -h|--help) echo 'Usage: test-bundle.sh [--prepare] FILE.deb'; exit 0;;
        --*) echo "Unknown argument: $1" >&2; exit 2;;
        *) [[ -z "${DEB:-}" ]] || { echo 'Pass exactly one deb' >&2; exit 2; }; DEB="$1"; shift;;
    esac
done
die() { echo "ERROR: $*" >&2; exit 1; }
[[ -f "${DEB:-}" ]] || die 'A bundle .deb path is required'
[[ "$EUID" == 0 ]] || die 'Run inside a disposable container as root'
[[ "$(dpkg-deb -f "$DEB" Package)" == kylinnote ]] || die 'Unexpected package name'
[[ "$(dpkg-deb -f "$DEB" Architecture)" == "$(dpkg --print-architecture)" ]] || die 'Wrong package architecture'
DEPENDENCIES="$(dpkg-deb -f "$DEB" Depends)"
[[ "$DEPENDENCIES" != *libqt5* && "$DEPENDENCIES" != *qtbase* ]] || die 'Bundle must not depend on system Qt packages'

assert_no_system_qt() {
    local installed
    installed="$(dpkg-query -W -f='${binary:Package} ${db:Status-Status}\n' | awk '$2 == "installed" && $1 ~ /^(libqt5|qt5-|qtbase5|qtchooser)/ {print $1}')"
    [[ -z "$installed" ]] || die "This is not a clean runtime environment; Qt packages installed: $installed"
    [[ ! -e /usr/bin/qmake ]] || die 'System qmake must not be present'
}
assert_no_system_qt
if [[ "$PREPARE" == yes ]]; then
    export DEBIAN_FRONTEND=noninteractive
    if ! apt-get update -o APT::Update::Error-Mode=any; then
        sed -i -E \
            -e 's|https?://(archive.ubuntu.com/ubuntu|security.ubuntu.com/ubuntu|ports.ubuntu.com/ubuntu-ports)|http://old-releases.ubuntu.com/ubuntu|g' \
            /etc/apt/sources.list
        apt-get update -o APT::Update::Error-Mode=any
    fi
    # Expand only package names from our generated, single-choice Depends field.
    [[ "$DEPENDENCIES" != *'|'* ]] || die 'Alternative dependencies require explicit test preparation'
    BASE_PACKAGES="$(printf '%s\n' "$DEPENDENCIES" | sed -E 's/\([^)]*\)//g; s/,/ /g')"
    # shellcheck disable=SC2086
    apt-get install -y --no-install-recommends $BASE_PACKAGES \
        binutils file xvfb xauth dbus-x11 fonts-dejavu-core fonts-wqy-microhei
    assert_no_system_qt
    echo '==> Prepared a runtime image with base graphics dependencies and no system Qt'
    exit 0
fi

for tool in readelf ldd xvfb-run dbus-run-session timeout; do
    command -v "$tool" >/dev/null || die "Missing $tool; run --prepare in a separate base image first"
done
# No apt operation is permitted below. CI also disables networking here.
dpkg -i "$DEB"
[[ "$(dpkg-query -W -f='${Status}' kylinnote)" == 'install ok installed' ]] || die 'Offline dpkg install failed'
assert_no_system_qt
LIBDIR=/usr/lib/kylinnote
for path in kylinnote libQt5Core.so.5 libQt5Gui.so.5 libQt5Widgets.so.5 libQt5Sql.so.5 libQt5Network.so.5 libQt5Svg.so.5 libQt5XcbQpa.so.5 \
        plugins/platforms/libqxcb.so plugins/sqldrivers/libqsqlite.so \
        plugins/imageformats/libqsvg.so plugins/iconengines/libqsvgicon.so qt.conf; do
    [[ -f "$LIBDIR/$path" ]] || die "Bundle is missing $path"
done
case "$(dpkg --print-architecture):$(uname -m)" in
    arm64:aarch64) ELF_MACHINE='AArch64';;
    amd64:x86_64) ELF_MACHINE='Advanced Micro Devices X86-64';;
    *) die 'Unexpected runtime architecture';;
esac
elf_count=0
while IFS= read -r -d '' elf; do
    readelf -h "$elf" >/dev/null 2>&1 || continue
    ((elf_count += 1))
    machine="$(LC_ALL=C readelf -h "$elf" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')"
    [[ "$machine" == "$ELF_MACHINE" ]] || die "Wrong ELF architecture: $elf ($machine)"
    needed="$(LC_ALL=C readelf -d "$elf" | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p')"
    resolved="$(LC_ALL=C LD_LIBRARY_PATH="$LIBDIR" ldd "$elf")" || die "ldd failed: $elf"
    [[ "$resolved" != *'not found'* ]] || die "Runtime dependency missing in the clean container: $elf\n$resolved"
    while IFS= read -r need; do
        [[ -n "$need" ]] || continue
        # DT_NEEDED, not only `ldd | grep not found`, drives this check.
        found="$(printf '%s\n' "$resolved" | awk -v need="$need" '$1 == need && $2 == "=>" {print $3} $1 ~ /^\// {name=$1; sub(/^.*\//, "", name); if (name == need) print $1}')"
        [[ -n "$found" && -f "$found" ]] || die "DT_NEEDED could not resolve: $need from $elf"
        if [[ "$need" == libQt5* ]]; then
            [[ "$found" == "$LIBDIR/"* ]] || die "Qt fell back to a system library: $found"
        fi
    done <<< "$needed"
done < <(find "$LIBDIR" -type f -print0)
echo "==> $elf_count ELF objects resolve without any system Qt"

RUN_DIR="$(mktemp -d)"
mkdir -m 0700 "$RUN_DIR/runtime"
SMOKE_LOG="$RUN_DIR/smoke.log"
APP_ARGS=(--smoke-test --data-dir "$RUN_DIR/data")
set +e
env LD_LIBRARY_PATH="$LIBDIR" QT_QPA_PLATFORM=xcb QT_DEBUG_PLUGINS=1 \
    QT_PLUGIN_PATH="$LIBDIR/plugins" QT_QPA_PLATFORM_PLUGIN_PATH="$LIBDIR/plugins/platforms" \
    XDG_RUNTIME_DIR="$RUN_DIR/runtime" \
    timeout --signal=TERM --kill-after=3s 20s \
    dbus-run-session -- xvfb-run -a -s '-screen 0 1280x800x24' \
    /usr/bin/kylinnote "${APP_ARGS[@]}" > "$SMOKE_LOG" 2>&1
result=$?
set -e
tail -n 100 "$SMOKE_LOG"
[[ "$result" == 0 ]] || die "--smoke-test failed or did not exit automatically: exit $result ($SMOKE_LOG)"
# Prove that the real platform and database plugins loaded during app startup.
grep -E 'loaded library.*libqxcb\.so' "$SMOKE_LOG" >/dev/null || die 'xcb platform plugin was not loaded'
grep -E 'loaded library.*libqsqlite\.so' "$SMOKE_LOG" >/dev/null || die 'SQLite driver was not loaded'
[[ -s "$RUN_DIR/data/notes.db" ]] || die 'The real application did not initialize its database'
echo "==> Offline install, ELF closure, xcb / SQLite loading and real-app smoke passed ($SMOKE_LOG)"
