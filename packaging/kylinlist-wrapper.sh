#!/bin/sh
# KylinList 启动包装脚本（自包含包专用）
#
# 真正的可执行文件与 Qt 运行库位于 <prefix>/lib/kylinlist，
# 这里负责把库搜索路径指过去，然后 exec 真身。
# 内网的机器即使没装任何 Qt 包，也能直接运行。
# prefix 由脚本自身位置推导，支持 /usr 之外的安装前缀。

SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
BINDIR="$(dirname "$SELF")"
PREFIX="$(dirname "$BINDIR")"
LIBDIR="$PREFIX/lib/kylinlist"

if [ ! -x "$LIBDIR/kylinlist" ]; then
    echo "KylinList: 未找到 $LIBDIR/kylinlist，安装可能已损坏" >&2
    exit 127
fi

# 随包库优先，系统库兜底
export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Qt 插件（平台插件 xcb、SQLite 驱动、图片格式、输入法）
export QT_PLUGIN_PATH="$LIBDIR/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$LIBDIR/plugins/platforms"

# 本程序只面向 X11，包内也只带 xcb 平台插件。若目标机环境里残留了
# QT_QPA_PLATFORM=wayland 之类的设置，会因缺插件而启动失败，这里给个默认值兜底
# （用户显式指定时不覆盖，便于排障时切 offscreen/minimal）。
: "${QT_QPA_PLATFORM:=xcb}"
export QT_QPA_PLATFORM

exec "$LIBDIR/kylinlist" "$@"
