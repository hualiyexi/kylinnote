# 合成演示数据（demo data）使用指南

本文档描述 KylinList 的合成演示数据链路：原生 `--seed-demo` / `--verify-demo` 命令、
`tools/generate_test_data.py` 驱动脚本、`demo-manifest.json` 所有权格式，以及安全边界。
适用对象：开发者在麒麟 V10 SP1 / Windows 上生成一份"看起来真实"的测试数据目录，用于手工
验收、截图和 UI 走查；自动化回归由 `test_demo`（CTest 第 19 项）覆盖。

## 组成

| 部件 | 位置 | 说明 |
| --- | --- | --- |
| 数据集实现 | `src/app/DemoData.h/.cpp`（`kl::demo`） | 全部业务写入走 `TaskStore`（`saveSchedule`、`setCompleted`、`saveDesktopState`、`importAttachment` 等），无直接 SQL |
| CLI 入口 | `src/main.cpp` | `--seed-demo` / `--verify-demo` 在任何 `QApplication`/`Branding`/`KylinListApp` 初始化之前处理，只创建 `QCoreApplication` |
| 驱动脚本 | `tools/generate_test_data.py` | 仅 Python 标准库；负责安全检查、调用原生命令、隔离 reset 与隔离启动 |
| 测试 | `tests/tst_demo.cpp` → CTest `demo` | 生成、校验、备份、manifest、安全/锁行为 |

## 原生命令

```
kylinlist --seed-demo   --data-dir <绝对路径>   # 生成；目录必须为全新/空（仅允许残留锁文件）
kylinlist --verify-demo --data-dir <绝对路径>   # 校验已有演示目录
```

规则：

- `--data-dir` **必填且必须是绝对路径**；拒绝文件系统根目录与包含符号链接/junction 组件的路径。
- seed 在写入前用 `SingleInstanceGuard` 锁目录：锁被其他实例持有时直接失败（退出码 3），
  **不会激活已运行实例**；全程不读写默认用户数据目录。
- 首个失败即停止并输出原因；校验报告写入 `<data-dir>/demo-verify-report.txt`。
- 生成产物：`kylinlist.db`、`attachments/`、`demo-backup.klb`（KLB1 容器 + SHA256 记录在
  manifest）、`demo-manifest.json`。
- 退出码：`0` 成功；`1` 数据/校验失败；`2` 用法错误；`3` 目录被锁。
- Windows 正式包是 GUI 子系统程序，控制台输出可能不可见：请以退出码与文件为准，
  或直接使用下面的 Python 驱动脚本。

## Python 驱动脚本（推荐入口）

`tools/generate_test_data.py` 只用标准库；不带参数时打印帮助并以退出码 2 结束。

```bash
# 生成（目标必须是不存在或完全为空的绝对路径目录）
python tools/generate_test_data.py --app build-windows-qt512-final/bin/kylinlist.exe \
    --seed --data-dir "D:/kylinlist-demo"

# 校验（重开全新数据库核对数量/ID/字段/日程/提醒/重复/层级/链接/附件/桌面状态/备份）
python tools/generate_test_data.py --app build-windows-qt512-final/bin/kylinlist.exe \
    --verify --data-dir "D:/kylinlist-demo"

# 隔离重置（安全重建）：manifest 所有权 + 原生校验通过 + 未被锁，
# 先把旧目录改名为同级时间戳目录（绝不删除），再原位重新生成一份新数据
python tools/generate_test_data.py --app build-windows-qt512-final/bin/kylinlist.exe \
    --reset --data-dir "D:/kylinlist-demo"
# 结果示例：旧数据在 D:/kylinlist-demo.demo-quarantine-20261117-103012，
# D:/kylinlist-demo 已重建为全新演示数据；重建失败时旧数据保留

# 隔离启动：要求有效 manifest
python tools/generate_test_data.py --app build-windows-qt512-final/bin/kylinlist.exe \
    --launch --data-dir "D:/kylinlist-demo"
```

脚本级安全检查（在调用原生程序之前）：

- `--app` 必须是存在的可执行文件；`--data-dir` 必须是绝对路径。
- 拒绝文件系统根；拒绝任何符号链接/junction 组件（Windows 上按 reparse point 检测）；
  `realpath` 解析结果与规范化路径不一致即拒绝。
- `--seed` 只接受全新或完全为空的目录（唯一例外：残留的 `.kylinlist.lock`，活锁由原生端
  以退出码 3 拒绝，死锁超过 stale 时限会被原生端清理后重建）。
- `--reset` 顺序强制为：读取并校验 `demo-manifest.json` 所有权 → 原生 `--verify-demo`
  必须成功（同时证明目录未被锁） → `os.rename` 改名为 `<目录名>.demo-quarantine-<时间戳>` →
  原位重新生成。任何一步失败都拒绝；永远不会递归删除任何路径；重建失败时已隔离数据保留。
- `--launch` 在启动前净化子进程环境（删除所有 `KYLINLIST_*` 与 `QT_*` 变量），只显式传
  `--data-dir`；并写入 `<data-dir>/settings/kylinlist/kylinlist.ini` 关闭自动更新：
  `updates/autoCheck=false`、`updates/baseUrl=`（显式空串 = 禁用更新源）。
- 退出码与原生命令一致（0/1/2/3）。

## demo-manifest.json 所有权格式

生成时写入目录根，`--reset`/`--launch` 都以它为准。字段（`version: 1`）：

```json
{
  "format": "kylinlist.demo-manifest",
  "version": 1,
  "ownership": {
    "marker": "kylinlist-synthetic-demo",
    "owned": true,
    "canReset": true,
    "policy": "This directory contains machine-generated synthetic demo data only. ..."
  },
  "generator": { "product": "kylinlist", "version": "<KYLINLIST_VERSION>", "mode": "seed-demo" },
  "generatedAtUtcMs": 1765000000000,
  "referenceDate": "2026-11-17",
  "dataDir": "D:/kylinlist-demo",
  "counts": { "folders": 3, "lists": 8, "sections": 3, "tasks": 38, "notes": 4,
              "entities": 42, "attachments": 5, "reminders": 6, "recurrenceSeries": 7,
              "desktopNotes": 6, "events": 83 },
  "ids": { "inboxListId": "...", "taskIds": ["..."], "schedules": { "overnight": "...", "midnight": "..." },
           "recurring": { "daily": "...", "lunarMonthly": "..." }, "hierarchy": { "parent": "..." } },
  "backup": { "file": "demo-backup.klb", "bytes": 78987, "sha256": "..." }
}
```

- `ownership.marker/owned/canReset` 必须同时满足才会被认作演示数据；解析时逐字段严格校验，
  未知/非法值直接拒绝。
- `referenceDate` 是生成时刻 Asia/Shanghai 的"今天"。所有日程锚点（已过期/今天/跨午夜/
  午夜零点/明天/最近7天/未来）都相对它计算，因此**验证始终确定性**，与运行校验的真实日期无关。
- `counts` 与 `ids` 是机器可读的核对清单；`--verify-demo` 重开全新 `TaskStore` 逐项核对。

## 数据覆盖矩阵

- 组织：3 个文件夹（图标）、7 个清单 + 收集箱（颜色 `#RRGGBB`、emoji 图标、笔记类型清单、
  1 个已归档清单）、3 个 section。
- 任务：优先级 0–3、标签（去重共享）、置顶、收集箱任务、父-子-孙三级层级（`createSubtask`
  继承日程）、Markdown 正文（标题/清单/外部链接）与 `[[UUID|标签]]` 内部链接（任务与笔记各一）。
- 日程：过期（全天/时刻）、今天（时刻区间、晨间单点）、跨午夜 23:30→00:30、午夜零点
  00:00→00:30、明日（全天、全天区间、时刻区间）、最近 7 天（全天多日、跨多日时刻）、
  未来单点与全天、**同日全天区间**（`allDayDate == allDayEndDate` 且显式区间）。
- 提醒：Start/End/Schedule 三种锚点的相对提醒（含一实体多条）、绝对提醒、全天提醒
  （提前 1 天 09:00）、带提醒的笔记。
- 重复：日、周（周一+周五）、月（月末）、年（固定日期）、农历每月、农历每年，以及
  "每日打卡"完成后由 `setCompleted` 生成的后继实例（含 recurrence link，备份中核验）。
- 状态：完成（普通 3 + 打卡 1）、放弃 2、回收站 2、归档清单内任务 2。
- 附件：真实合成 PNG（内嵌离线字节，非 GUI 生成）、CSV、TXT，共 5 个；"市场竞品分析"
  一个实体挂 3 个附件；文件内容 SHA256 在校验时重算比对。
- 桌面便签：六种颜色 × 三种层级全覆盖，仅 2 个处于打开状态。
- 活动事件：全部由 TaskStore 操作自然产生（created/changed/attachment_added/trashed 等），
  校验时逐实体核对。

## CTest

`test_demo`（`tests/tst_demo.cpp`）在 CMake 中注册为第 19 个测试，仅链接 `kylinlist_core`：

```bash
cmake --build build-windows-qt512-final --target test_demo
ctest --test-dir build-windows-qt512-final -R demo --output-on-failure
```

覆盖用例：生成→校验→报告、拒绝非空目录、拒绝相对/根路径、seed/verify 与实例锁互斥、
`populate` 直接驱动外部 `TaskStore`、manifest 序列化往返与所有权拒绝、非演示目录
**零写入**、篡改检测（删除附件文件后校验失败）。

构建树内直接运行可执行文件时，Qt 需要能找到插件（QSQLITE/platforms）：设置
`QT_PLUGIN_PATH` 指向 Qt 的 plugins 目录，或在可执行文件旁放置 `qt.conf`
（部署包不需要，插件随包安装）。

## 设计边界

- 不回填 `createdAt`/历史时间戳：完成/放弃时间由库按当前时刻记录，保证事件日志真实。
- 不生成非公开的自定义视图（ViewDef）与标签颜色：数据集只使用产品公开能力。
- 附件 PNG 为内嵌字节（stdlib 生成的真实 PNG），不依赖 GUI/字体环境，离线可复现。
- 演示目录永远与真实用户数据隔离：工具进程内临时切换 `KYLINLIST_DATA_DIR`（RAII 恢复），
  不触碰默认数据目录、开机自启与统计上报。
