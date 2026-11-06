#!/bin/bash
# 验证便签层级：贴在桌面（BELOW）/ 普通（normal）/ 置顶（ABOVE）。
# 必须在带窗口管理器的真实 X11 会话上跑（Xvfb 无 WM，_NET_WM_STATE 无人实现，测不出来）。
#
# 用法: bash packaging/layer-verify.sh [可执行文件] [DISPLAY]

set -u

BIN="${1:-/home/kylin/WorkBuddy/2026-10-20-14-13-13/kylinnote/build/bin/kylinnote}"
DISP="${2:-:0}"
ROOT=/home/kylin/.kn-layer-$$
export DISPLAY="$DISP"

# 便签窗口特征：class=kylinnote、已映射、尺寸不小于便签最小尺寸
find_note_window() {
    for id in $(xdotool search --class kylinnote 2>/dev/null); do
        info=$(xwininfo -id "$id" 2>/dev/null) || continue
        case "$info" in *"IsViewable"*) ;; *) continue ;; esac
        w=$(echo "$info" | awk '/Width:/{print $2}')
        h=$(echo "$info" | awk '/Height:/{print $2}')
        if [ "${w:-0}" -ge 160 ] && [ "${h:-0}" -ge 120 ]; then
            echo "$id"
            return 0
        fi
    done
    return 1
}

# 只清理脚本自己开的 xclock。
# 注意：不要在这里 windowkill 所有 class=kylinnote 的窗口——本机可能正跑着
# 用户自己的 KylinNote，会被一起杀掉。
cleanup() {
    pkill -9 xclock 2>/dev/null
    sleep 1
}
trap cleanup EXIT
cleanup

# 记录启动前已有的窗口，之后用差集精确锁定本次新建的便签窗口
xdotool search --class kylinnote 2>/dev/null | sort > "$ROOT/before.txt"

mkdir -p "$ROOT/home" "$ROOT/run"; chmod 700 "$ROOT/run"

BUS=$(dbus-daemon --session --fork --print-address=1 2>/dev/null)
export DBUS_SESSION_BUS_ADDRESS="$BUS"
for i in $(seq 1 20); do
    dbus-send --bus="$BUS" --dest=org.freedesktop.DBus --type=method_call \
              /org/freedesktop/DBus org.freedesktop.DBus.ListNames >/dev/null 2>&1 && break
    sleep 0.5
done
echo "[1] 会话总线就绪 (display=$DISP)"

HOME="$ROOT/home" XDG_RUNTIME_DIR="$ROOT/run" \
    QT_QPA_PLATFORM=xcb DBUS_SESSION_BUS_ADDRESS="$BUS" \
    "$BIN" > "$ROOT/app.log" 2>&1 &
APP=$!
sleep 3

dbus-send --bus="$BUS" --dest=org.kylin.KylinNote --type=method_call \
          /KylinNote org.kylin.KylinNote.newNote >/dev/null 2>&1
sleep 2

xdotool search --class kylinnote 2>/dev/null | sort > "$ROOT/after.txt"
NOTE_WIN=$(comm -13 "$ROOT/before.txt" "$ROOT/after.txt" | head -n1)
[ -n "$NOTE_WIN" ] || NOTE_WIN=$(find_note_window)
echo "[2] 本次新建的便签窗口 id=$NOTE_WIN"

# 一个"其它应用"的普通窗口，盖在便签位置上
xclock -geometry 420x320+100+80 >/dev/null 2>&1 &
sleep 2
echo "[3] 其它应用窗口(xclock) 已启动"

layer_of() {
    xprop -id "$1" _NET_WM_STATE 2>/dev/null | tr ',' '\n' \
        | grep -oE "ABOVE|BELOW" | head -n1 || echo "normal"
}

report() {
    printf "    便签层位 = %s\n" "$(layer_of "$NOTE_WIN")"
}

echo "[4] 新建后（新建时临时置前，应为 normal 以保证可见）："
report

echo "[5] 点一下 xclock 让便签失焦后（应落回用户默认层位 BELOW）："
xdotool windowactivate --sync "$(xdotool search --class xclock | head -n1)" 2>/dev/null
sleep 2
report

echo "[6] 按 1 次 F12（BELOW -> normal）："
xdotool key F12 2>/dev/null
sleep 2
report

echo "[7] 再按 1 次 F12（normal -> ABOVE）："
xdotool key F12 2>/dev/null
sleep 2
report

echo "[8] 再按 1 次 F12（ABOVE -> BELOW，循环回到贴桌面）："
xdotool key F12 2>/dev/null
sleep 2
report

DISPLAY="$DISP" import -window root /home/kylin/.kn-layer.png 2>/dev/null
echo "[9] 截图: /home/kylin/.kn-layer.png"
echo "=== 应用日志 ==="
sed 's/^/  /' "$ROOT/app.log"
