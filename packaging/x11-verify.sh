#!/bin/bash
# 在独立 X server（Xvfb）上以真实 xcb 平台插件运行自包含包，并截图确认渲染结果。
# 用途：验证 X11 路径下窗口/标题栏/输入文字是否真的绘制出来（offscreen 测不出来）。
set -u

PKG="${1:?用法: x11-verify.sh <deb 包路径> <输出 png 路径>}"
OUT="${2:?}"
ROOT=/home/kylin/.kn-verify

# 删除失败必须退出：否则会复用上一轮数据，"还原 + 新建"混在一起看不出问题
if [ -d "$ROOT" ]; then
    rm -rf "$ROOT" 2>/dev/null || { echo "无法清理 $ROOT，请手动删除后重试" >&2; exit 1; }
fi
[ -d "$ROOT" ] && { echo "清理 $ROOT 失败，请手动删除后重试" >&2; exit 1; }
mkdir -p "$ROOT/pkg" "$ROOT/home" "$ROOT/run"
dpkg-deb -x "$PKG" "$ROOT/pkg"
chmod 700 "$ROOT/run"

pkill -f "Xvfb :99" >/dev/null 2>&1
Xvfb :99 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
echo "[1] Xvfb :99 已启动 (pid $(pgrep -f 'Xvfb :99' | head -n1))"

BUS=$(dbus-daemon --session --fork --print-address=1 2>/dev/null)
export DBUS_SESSION_BUS_ADDRESS="$BUS"
for i in $(seq 1 20); do
    dbus-send --bus="$BUS" --dest=org.freedesktop.DBus --type=method_call \
              /org/freedesktop/DBus org.freedesktop.DBus.ListNames >/dev/null 2>&1 && break
    sleep 0.5
done
echo "[2] 会话总线就绪"

BIN="$ROOT/pkg/usr/lib/kylinnote/kylinnote"
HOME="$ROOT/home" XDG_RUNTIME_DIR="$ROOT/run" \
    QT_QPA_PLATFORM=xcb DISPLAY=:99 \
    DBUS_SESSION_BUS_ADDRESS="$BUS" \
    "$BIN" > "$ROOT/app.log" 2>&1 &
APP=$!
sleep 3
echo "[3] 应用已启动 pid=$APP"

dbus-send --bus="$BUS" --dest=org.kylin.KylinNote --type=method_call \
          /KylinNote org.kylin.KylinNote.newNote >/dev/null 2>&1
sleep 1
dbus-send --bus="$BUS" --dest=org.kylin.KylinNote --type=method_call \
          /KylinNote org.kylin.KylinNote.newNote >/dev/null 2>&1
echo "[4] 已通过 D-Bus 新建两条便笺"
sleep 2

# 敲入文字，验证文本是否真的绘制
xdotool search --sync --onlyvisible --class "" >/dev/null 2>&1
xdotool click 1 >/dev/null 2>&1
sleep 0.5
DISPLAY=:99 xdotool type --delay 20 "KylinNote-OK" 2>/dev/null
sleep 2

echo "[5] 窗口列表:"
DISPLAY=:99 xdotool search --onlyvisible --name ".*" 2>/dev/null | while read -r w; do
    printf "    id=%s %s\n" "$w" "$(DISPLAY=:99 xdotool getwindowname "$w" 2>/dev/null)"
done

DISPLAY=:99 import -window root "$OUT" 2>&1
echo "[6] 截图: $OUT ($(stat -c%s "$OUT" 2>/dev/null || echo 0) 字节)"

kill "$APP" 2>/dev/null; sleep 1; pkill -f "$BIN" 2>/dev/null
pkill -f "Xvfb :99" >/dev/null 2>&1

echo "=== 应用日志 ==="
sed 's/^/  /' "$ROOT/app.log"
