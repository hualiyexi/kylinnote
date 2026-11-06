#!/usr/bin/env bash
# KylinNote deb 打包脚本（银河麒麟 V10 SP1 / arm64）
#
# 用法：
#   ./packaging/build-deb.sh                      # 常规包：依赖目标机已装的 Qt 运行时
#   ./packaging/build-deb.sh --bundle-qt          # 自包含包：Qt 库与插件随包携带，内网离线可用
#   ./packaging/build-deb.sh --arch arm64 --version 1.0.0
#   ./packaging/build-deb.sh --qt-prefix /opt/Qt5.12.8/gcc_64   # 指定非系统 Qt
#   ./packaging/build-deb.sh --shlibdeps          # 用 dpkg-shlibdeps 自动推算库依赖
#   ./packaging/build-deb.sh --no-strip           # 保留调试符号
#
# 产物：kylinnote_<version>_<arch>[_bundle].deb

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

ARCH="$(dpkg --print-architecture 2>/dev/null || echo arm64)"
VERSION=""
QT_PREFIX=""
USE_SHLIBDEPS="no"
BUNDLE_QT="no"
DO_STRIP="yes"
INSTALL_PREFIX="/usr"
BUILD_DIR="$PROJECT_DIR/build-deb"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)        ARCH="$2"; shift 2 ;;
        --version)     VERSION="$2"; shift 2 ;;
        --qt-prefix)   QT_PREFIX="$2"; shift 2 ;;
        --build-dir)   BUILD_DIR="$2"; shift 2 ;;
        --prefix)      INSTALL_PREFIX="$2"; shift 2 ;;
        --shlibdeps)   USE_SHLIBDEPS="yes"; shift ;;
        --bundle-qt)   BUNDLE_QT="yes"; shift ;;
        --no-strip)    DO_STRIP="no"; shift ;;
        -h|--help)     sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$VERSION" ]]; then
    VERSION="$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\+\([0-9][0-9A-Za-z.+-]*\).*/\1/p' \
               "$PROJECT_DIR/CMakeLists.txt" | head -n1)"
fi
if [[ -z "$VERSION" ]]; then
    echo "无法从 CMakeLists.txt 解析版本号，请用 --version 指定" >&2
    exit 2
fi

command -v cmake >/dev/null || { echo "缺少 cmake（sudo apt install cmake）" >&2; exit 1; }
command -v dpkg-deb >/dev/null || { echo "缺少 dpkg-deb" >&2; exit 1; }

CMAKE_ARGS=(-S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX")
[[ -n "$QT_PREFIX" ]] && CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="$QT_PREFIX")
[[ "$BUNDLE_QT" == "yes" ]] && CMAKE_ARGS+=(-DKYLINNOTE_BUNDLE_QT=ON)

echo "==> 配置 (arch=$ARCH version=$VERSION prefix=$INSTALL_PREFIX bundle-qt=$BUNDLE_QT)"
cmake "${CMAKE_ARGS[@]}"

echo "==> 编译"
cmake --build "$BUILD_DIR" --parallel "$(nproc 2>/dev/null || echo 2)"

PKG_NAME="kylinnote_${VERSION}_${ARCH}"
[[ "$BUNDLE_QT" == "yes" ]] && PKG_NAME="${PKG_NAME}_bundle"

STAGING="$BUILD_DIR/staging/$PKG_NAME"
rm -rf "$STAGING"
mkdir -p "$STAGING"

echo "==> 安装到打包暂存目录"
DESTDIR="$STAGING" cmake --install "$BUILD_DIR"

echo "==> 写入 DEBIAN 控制文件"
mkdir -p "$STAGING/DEBIAN"
sed "s/^Version:.*/Version: $VERSION/; s/^Architecture:.*/Architecture: $ARCH/" \
    "$PROJECT_DIR/packaging/deb/DEBIAN/control" > "$STAGING/DEBIAN/control"

install -m 0755 "$PROJECT_DIR/packaging/deb/DEBIAN/postinst" "$STAGING/DEBIAN/postinst"
install -m 0755 "$PROJECT_DIR/packaging/deb/DEBIAN/prerm"    "$STAGING/DEBIAN/prerm"

# ---------------------------------------------------------------------------
# 自包含模式：把 Qt 运行库与插件拷进 /usr/lib/kylinnote
# ---------------------------------------------------------------------------
if [[ "$BUNDLE_QT" == "yes" ]]; then
    echo "==> 收集随包携带的 Qt 运行库与插件"

    LIBDIR="$STAGING$INSTALL_PREFIX/lib/kylinnote"
    mkdir -p "$LIBDIR/plugins"
    BIN="$LIBDIR/kylinnote"

    [[ -x "$BIN" ]] || { echo "未找到 $BIN，自包含模式需要 KYLINNOTE_BUNDLE_QT=ON 的构建产物" >&2; exit 1; }

    # 以下必须由目标系统提供：C/C++ 运行时、X11 与图形栈、字体与基础库。
    # 其余（libQt5*、libicu*、libpcre2-16、libdouble-conversion、libpng 等）随包携带。
    EXCLUDE_RE='ld-linux|linux-vdso|libc\.so|libm\.so|libpthread\.so|libdl\.so|librt\.so|libstdc\+\+|libgcc_s|'
    EXCLUDE_RE+='libX11\.so|libX11-xcb|libXext\.so|libXrender\.so|libXi\.so|libSM\.so|libICE\.so|libXtst|libXcursor|libXrandr|libXfixes|libXdamage|libXcomposite|'
    EXCLUDE_RE+='libxcb\.so|libxcb-[a-z]|libxkbcommon|libxshmfence|'
    EXCLUDE_RE+='libGL\.so|libGLX|libGLdispatch|libEGL\.so|libgbm|libglapi|libwayland-|libdrm|'
    EXCLUDE_RE+='libXau|libXdmcp|libbsd|libffi|libresolv|libgpg-error|'
    EXCLUDE_RE+='libfontconfig|libfreetype|libharfbuzz|libgraphite|'
    EXCLUDE_RE+='libglib-2\.0|libgobject-2\.0|libgthread-2\.0|libgio-2\.0|libgmodule-2\.0|'
    EXCLUDE_RE+='libz\.so|libdbus-1|libexpat|libselinux|libmount|libblkid|libuuid|'
    EXCLUDE_RE+='libpcre\.so|libpcre2-8|libsystemd|libcap|libgcrypt|liblz4|liblzma|libbz2|libacl|libattr'

    collect_libs() {
        ldd "$1" 2>/dev/null | awk '{print $3}' | grep -E '^/' | sort -u \
            | grep -Ev "$EXCLUDE_RE" || true
    }

    copy_libs() {
        local lib base
        while read -r lib; do
            [[ -n "$lib" && -f "$lib" ]] || continue
            base="$(basename "$lib")"
            [[ -e "$LIBDIR/$base" ]] && continue
            cp -L "$lib" "$LIBDIR/$base"
        done
    }

    copy_libs <<< "$(collect_libs "$BIN")"

    # Qt 插件：平台(xcb)、SQLite 驱动、图片格式、输入法
    PLUGIN_SRC=""
    if command -v qmake >/dev/null 2>&1; then
        PLUGIN_SRC="$(qmake -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
    fi
    if [[ -z "$PLUGIN_SRC" || ! -d "$PLUGIN_SRC" ]]; then
        for cand in "/usr/lib/$ARCH-linux-gnu/qt5/plugins" \
                    "/usr/lib/$(uname -m)-linux-gnu/qt5/plugins" \
                    /usr/lib/qt5/plugins /usr/lib64/qt5/plugins; do
            [[ -d "$cand" ]] && { PLUGIN_SRC="$cand"; break; }
        done
    fi

    if [[ -n "$PLUGIN_SRC" && -d "$PLUGIN_SRC" ]]; then
        echo "    插件来源: $PLUGIN_SRC"
        for sub in platforms platforminputcontexts sqldrivers imageformats iconengines xcbglintegrations; do
            [[ -d "$PLUGIN_SRC/$sub" ]] || continue
            mkdir -p "$LIBDIR/plugins/$sub"
            find "$PLUGIN_SRC/$sub" -maxdepth 1 -name '*.so' -exec cp -L {} "$LIBDIR/plugins/$sub/" \;
        done
    else
        echo "    警告: 未找到 Qt 插件目录，缺少平台插件将导致程序无法启动" >&2
    fi

    # 插件自身还会引入新的 Qt 库（典型：plugins/platforms/libqxcb.so -> libQt5XcbQpa.so.5）。
    # 注意必须全目录递归扫描：平台插件位于 plugins/platforms 下，相对 LIBDIR 是第三层，
    # 用 maxdepth 2 会整层漏掉，导致私有 Qt 库缺失。
    for _round in 1 2 3; do
        before="$(find "$LIBDIR" -name '*.so*' | wc -l)"
        while read -r so; do
            copy_libs <<< "$(collect_libs "$so")"
        done < <(find "$LIBDIR" -type f -name '*.so*')
        after="$(find "$LIBDIR" -name '*.so*' | wc -l)"
        [[ "$after" -eq "$before" ]] && break
    done

    # 交验依赖闭合：包内每个 ELF 的直接依赖，要么已随包携带，要么属于明确交给系统的白名单。
    # 缺少 Qt 私有库（如 libQt5XcbQpa）时，在没装 Qt 的目标机上会加载不了平台插件；
    # 若目标机残留了别的版本 Qt，动态链接器会回退到系统库，包内 Qt 与系统私有库
    # 版本不一致会引起 ABI 混合——表现就是窗口能映射但完全不绘制、文字不显示。
    echo "==> 交验随包依赖闭合"
    closure_bad=0
    while read -r elf; do
        while read -r need; do
            [[ -z "$need" ]] && continue
            if [[ ! -e "$LIBDIR/$need" ]] && ! grep -Eq "$EXCLUDE_RE" <<< "$need"; then
                echo "    错误: ${need} 既未随包携带、也不在系统库白名单（被 ${elf} 需要）" >&2
                closure_bad=1
            fi
        done < <(LD_LIBRARY_PATH="$LIBDIR" ldd "$elf" 2>/dev/null | awk '/not found/{print $1}')
    done < <(find "$LIBDIR" -type f -name '*.so*'; echo "$BIN")
    if [[ "$closure_bad" -ne 0 ]]; then
        echo "依赖未闭合，请修正后重打包" >&2
        exit 1
    fi
    echo "    依赖闭合 OK（包内不依赖任何系统 Qt）"

    if [[ "$DO_STRIP" == "yes" ]] && command -v strip >/dev/null 2>&1; then
        echo "==> 精简符号"
        strip --strip-unneeded "$BIN" 2>/dev/null || true
        find "$LIBDIR" -type f -name '*.so*' -exec strip --strip-unneeded {} \; 2>/dev/null || true
    fi

    echo "    随包库文件: $(find "$LIBDIR" -name '*.so*' | wc -l) 个，目录体积 $(du -sh "$LIBDIR" | cut -f1)"

    # 自包含包不依赖系统 Qt，只保留基础系统库
    BUNDLE_DEPENDS="libc6, libstdc++6, libx11-6, libxext6, libxrender1, libxi6, libxcb1, libxkbcommon0, libfontconfig1, libfreetype6, libglib2.0-0, zlib1g"
    sed -i "s/^Depends:.*/Depends: $BUNDLE_DEPENDS/" "$STAGING/DEBIAN/control"

elif [[ "$USE_SHLIBDEPS" == "yes" ]] && command -v dpkg-shlibdeps >/dev/null; then
    echo "==> 用 dpkg-shlibdeps 推算库依赖"
    mkdir -p "$BUILD_DIR/debian"
    : > "$BUILD_DIR/debian/control"
    dpkg-shlibdeps -O -T"$BUILD_DIR/debian/substvars" \
        -dDepends "$STAGING$INSTALL_PREFIX/bin/kylinnote" > "$BUILD_DIR/depends.txt" || true
    DEPS="$(sed 's/^Depends=//; s/^shlibs:Depends=//' "$BUILD_DIR/depends.txt" | sed 's/[ \t]*$//')"
    if [[ -n "$DEPS" ]]; then
        sed -i "s/^Depends:.*/Depends: $DEPS, libqt5sql5-sqlite/" "$STAGING/DEBIAN/control"
    fi
fi

echo "==> 生成 deb"
# dpkg-deb 压缩时会用 TMPDIR 做临时空间，某些机器 /tmp 很小（如 10M 的 tmpfs），
# 这里指到构建目录下，避免 "设备上没有空间"
TMPROOT="$BUILD_DIR/tmp"
mkdir -p "$TMPROOT"
export TMPDIR="$TMPROOT"

OUTPUT="$PROJECT_DIR/${PKG_NAME}.deb"
rm -f "$OUTPUT"
dpkg-deb --build --root-owner-group "$STAGING" "$OUTPUT"

echo "==> 完成: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
dpkg-deb --info "$OUTPUT" | sed -n '1,14p'

if [[ "$BUNDLE_QT" == "yes" ]]; then
    cat <<'TIP'

自包含包说明：
  - Qt 运行库与插件位于 /usr/lib/kylinnote，/usr/bin/kylinnote 是负责指路的启动脚本
  - 目标机器无需安装任何 Qt 包，dpkg -i 即可运行
  - 请在同构机器上使用（同为 arm64 + 银河麒麟 V10 SP1 + X11 会话）
TIP
fi
