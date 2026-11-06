#!/bin/bash
# KylinNote 端到端验证：D-Bus 单实例 + newNote 落库 + 二次启动拦截
# 仅用于开发机自测，不参与 deb 打包。
set -u

BIN="${1:-/home/kylin/WorkBuddy/2026-10-20-14-13-13/kylinnote/build/bin/kylinnote}"
ROOT="/home/kylin/.kn-e2e"

# 注意：某些环境会拦截 rm -rf 大目录，若删除失败必须直接退出，
# 否则会复用上一轮的数据库，得到"假通过"的结果
if [ -d "$ROOT" ]; then
    rm -rf "$ROOT" 2>/dev/null || { echo "无法清理 $ROOT，请手动删除后重试" >&2; exit 1; }
fi
[ -d "$ROOT" ] && { echo "清理 $ROOT 失败，请手动删除后重试" >&2; exit 1; }
mkdir -p "$ROOT/home" "$ROOT/run" "$ROOT/xdg"

export HOME="$ROOT/home"
export XDG_RUNTIME_DIR="$ROOT/run"
export XDG_DATA_HOME="$HOME/.local/share"
export XDG_CONFIG_HOME="$HOME/.config"
export QT_QPA_PLATFORM=offscreen
chmod 700 "$ROOT/run"

# 1) 启动私有会话总线
BUSFILE="$ROOT/bus.addr"
dbus-daemon --session --fork --print-address=3 3>"$BUSFILE" --print-pid=4 4>"$ROOT/bus.pid"
BUS="$(cat "$BUSFILE")"
BUSPID="$(cat "$ROOT/bus.pid")"
export DBUS_SESSION_BUS_ADDRESS="$BUS"
echo "[1] session bus = $BUS (pid $BUSPID)"

# 等待总线真正可用（dbus-daemon --fork 与调用方存在竞态）
for i in $(seq 1 40); do
    if dbus-send --bus="$BUS" --dest=org.freedesktop.DBus --type=method_call \
                 /org/freedesktop/DBus org.freedesktop.DBus.ListNames >/dev/null 2>&1; then
        echo "[1a] 总线就绪（第 $i 次探测）"
        break
    fi
    sleep 0.5
done

# 2) 启动主实例
"$BIN" > "$ROOT/inst1.log" 2>&1 &
P1=$!
echo "[2] 主实例启动 pid=$P1"
sleep 3
if kill -0 "$P1" 2>/dev/null; then
    echo "    状态: 存活"
else
    wait "$P1" 2>/dev/null
    echo "    状态: 已退出，退出码=$?"
fi

# 3) 通过 D-Bus 调用 newNote（注意：--bus 与 --session 互斥）
echo "[3] dbus-send newNote"
dbus-send --bus="$BUS" \
          --dest=org.kylin.KylinNote \
          --type=method_call \
          --print-reply \
          /KylinNote org.kylin.KylinNote.newNote 2>&1 | sed 's/^/    /'

# 4) 再新建一条并写点内容（验证另一条路径：直接给主实例发两次）
dbus-send --bus="$BUS" --dest=org.kylin.KylinNote --type=method_call \
          --print-reply /KylinNote org.kylin.KylinNote.newNote 2>&1 | sed 's/^/    /'

sleep 2

# 5) 列出总线上的服务名，确认已注册
echo "[4] 已注册服务名："
dbus-send --bus="$BUS" --dest=org.freedesktop.DBus --type=method_call \
          --print-reply /org/freedesktop/DBus org.freedesktop.DBus.ListNames 2>&1 \
          | grep -o '"org.kylin.KylinNote"' | sed 's/^/    /'

# 6) 查库
DB="$HOME/.local/share/kylinnote/notes.db"
echo "[5] 数据库：$(ls -la "$DB" 2>/dev/null || echo 缺失)"
if [ -f "$DB" ]; then
    python3 - "$DB" <<'PY'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
print("    journal_mode =", c.execute("PRAGMA journal_mode").fetchone()[0])
print("    user_version =", c.execute("PRAGMA user_version").fetchone()[0])
rows = c.execute("SELECT id,title,color,geometry,z_order,deleted FROM notes ORDER BY id").fetchall()
print("    notes 行数 =", len(rows))
for r in rows:
    print("      ", r)
PY
fi

# 7) 第二次启动：应被单实例拦截并唤醒已有实例后退出
echo "[6] 第二次启动（期望快速退出码 0）"
timeout 20 "$BIN" > "$ROOT/inst2.log" 2>&1
RC=$?
echo "    退出码 = $RC"
sed 's/^/    /' "$ROOT/inst2.log"

# 8) 清理进程
kill "$P1" 2>/dev/null
sleep 1
kill "$BUSPID" 2>/dev/null

echo "[7] 主实例日志："
sed 's/^/    /' "$ROOT/inst1.log"

# 9) 启动还原：预置数据（含中文、多色、已软删行），确认还原路径不破坏数据
echo "[8] 启动还原测试"
rm -rf "$ROOT/home2"
mkdir -p "$ROOT/home2"
H2="$ROOT/home2"
python3 - "$H2" <<'PY'
import sqlite3, os, sys
d = os.path.join(sys.argv[1], '.local/share/kylinnote')
os.makedirs(d, exist_ok=True)
c = sqlite3.connect(os.path.join(d, 'notes.db'))
c.execute("CREATE TABLE notes(id INTEGER PRIMARY KEY AUTOINCREMENT,title TEXT NOT NULL DEFAULT '',"
          "content TEXT NOT NULL DEFAULT '',color INTEGER NOT NULL DEFAULT 0,geometry TEXT NOT NULL DEFAULT '',"
          "z_order INTEGER NOT NULL DEFAULT 0,created_at TEXT NOT NULL DEFAULT '',updated_at TEXT NOT NULL DEFAULT '',"
          "deleted INTEGER NOT NULL DEFAULT 0)")
c.execute("PRAGMA user_version=1")
c.executemany("INSERT INTO notes(title,content,color,geometry,z_order,created_at,updated_at,deleted)"
              " VALUES(?,?,?,?,?,?,?,?)", [
    ('会议纪要', '<p>明天十点开会</p>', 2, '300,200,260,240', 5, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 0),
    ('购物清单', '<p>牛奶 鸡蛋</p>',     4, '700,300,220,200', 9, '2026-01-02T00:00:00Z', '2026-01-02T00:00:00Z', 0),
    ('已删除的', '<p>不该被还原</p>',     0, '900,400,200,200', 3, '2026-01-03T00:00:00Z', '2026-01-03T00:00:00Z', 1),
])
c.commit(); c.close()
PY
env HOME="$H2" XDG_RUNTIME_DIR="$ROOT/run" QT_QPA_PLATFORM=offscreen \
    DBUS_SESSION_BUS_ADDRESS="$BUS" timeout 6 "$BIN" > "$ROOT/inst3.log" 2>&1
echo "    退出码=$? (124=存活到超时，符合预期)"
grep -c "does not support raise" "$ROOT/inst3.log" | sed 's/^/    raise 次数(应为2，软删行未还原): /'
python3 - "$H2/.local/share/kylinnote/notes.db" <<'PY'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
rows = c.execute("SELECT id,title,color,z_order,deleted FROM notes ORDER BY id").fetchall()
print("    还原后行数 =", len(rows), "(应为 3，软删行保留在库)")
for r in rows: print("      ", r)
PY
grep -v "does not support raise" "$ROOT/inst3.log" | sed 's/^/    /'

echo "=== 完成 ==="
