# 任务数据模型与服务契约

核心以 Qt 5.12 / C++14 为基线。`TaskStore`、`TaskRepository`、`Database` 和 `ReminderScheduler` 必须在其 SQLite 连接所属的同一线程使用；通常就是主线程。先打开 `Database`、创建 store 并 `load()`，最后停止 scheduler、销毁 store/连接副本，再关闭数据库。

## schema 2 与旧数据

`Database::instance().open(QString *error = nullptr)` 保持默认入口；`openAt(path, error)` 支持临时数据库和测试。只有连接、pragma、事务迁移全部成功才让 `isOpen()` 返回 true。失败会关闭并移除连接。`user_version > 2` 在任何 schema 或 journal 修改前拒绝打开，不写回版本号。

schema 2 新增 `task_groups`、`tasks` 和内部 `task_recurrence_links`。v0 首次建库和 v1 升级都在一个事务中完成。旧 `notes` 表及 HTML、标题、颜色、geometry、z_order、创建/修改时间、deleted 原始内容完整保留。导入到 tasks 时 HTML 经 `QTextDocument::toPlainText()` 转为 Markdown 纯文本（最新产品约定）；未删便签 pinned=true，已删便签 deleted=true、pinned=false。保留旧 id；缺少标题的新任务使用首行或“未命名便签”。迁移仅执行一次。

## 任务、写入与信号

`Task` 包含约定的标题、正文格式、分组、优先级、四种布尔状态、计划/提醒/审计时间、重复规则、标签、检查清单和桌面便签属性。枚举的数值是持久协议。`TaskCodec.h` 为内部 header-only 校验与 JSON 编解码器。

- `tasks()` 包含回收站任务，`groups()` 不包含虚拟“未分组”；groupId=0 表示未分组。
- `task(id)` 找不到时返回 id=0。`createTask` / `createGroup` 失败返回 0，其余写操作返回 bool。
- `TaskStore::lastError()` 提供最近错误；失败发 `errorOccurred`。`load` 失败保留原缓存。
- 提交成功后才替换缓存、发 `taskChanged` / `tasksReset` / `groupsChanged`。新增、移除、重复生成和组变动可能同时发 reset 和逐项信号；订阅者应允许幂等刷新。
- 标题与分组名不能全为空白；校验优先级、枚举、重复规则、分组引用、日期范围及 startAt <= dueAt。重复任务必须有 startAt 或 dueAt。可选日期用默认 null `QDateTime` 表示未设置。提醒允许在截止之后，以支持逾期任务 snooze。
- 创建/更新时间由 store 管理；完成与 completedAt 同步。任何 save 改变 reminderAt 都清除 notifiedAt；普通编辑保留已保存的通知标记，避免旧编辑器副本覆盖去重状态。
- 删除分组在事务中把所有任务（包括回收站）移到未分组。永久删除要求任务已在回收站。

`TaskRepository::load` 返回包含任务、分组及内部元数据的一致快照；`apply(TaskChanges*, TaskSnapshot*, QString*)` 提供任务/分组插入、更新、删除和重复实例的一次性事务。仅提交后回填新 id/revision/输出快照，失败时输入和输出对象不变。更新/删除按 revision 条件写入，拒绝另一 store 的陈旧覆盖。发生此错误后调用 `load()` 重载再编辑。`merge` 对已校验备份分配全新 id、映射分组和重复引用。

## 重复、月末与安全撤销

完成重复任务时保留本次记录，并在同一事务中生成未完成的下一次。计划基准是 dueAt，有 startAt 而无 dueAt 时用 startAt；从本次计划递增，不从实际完成时刻或今天开始，不自动跳过逾期周期。

每日/每周按计划加 1/7 天。月度保存初始“日”锚点：1/31 -> 2/28（闰年 2/29）-> 3/31；1/30 -> 2/28 -> 3/30。保留开始到截止的时间段、提醒相对计划的秒偏移；新实例重置完成状态、通知标记和清单勾选，保留其它业务属性。手动改计划日期会重新设置月锚点。

内部生成凭证以 source_id 为唯一键，记录 next_id 和其生成时 revision。重复保存、重启、再次 setCompleted(true) 均不会重复生成。下一实例永久删除后，凭证保留 NULL next_id，继续防止重复。

撤销完成会同时撤回尚未修改/通知/完成/删除的下一实例和生成凭证；之后再完成能生成一次新实例。若下一实例已变动、已有后续或被永久删除，则拒绝撤销并给可解释错误，避免丢失用户工作。仅看标题相同不能判断实例身份。

## 提醒的持久去重

`ReminderScheduler::start()` 立即补发逾期提醒，随后 15 秒轮询；事件分发器唤醒或应用激活时也检查。结合墙钟和单调时钟识别恢复/时间跳变，无需 UI 代码。`checkDue(now)` 可供测试或平台恢复事件显式调用。

资格为 reminderAt<=now、未完成、未删，且 notifiedAt 不存在或小于 reminderAt。通过额外的 `TaskStore::markReminderNotified(id, expectedReminder, now)` 先提交去重标记，之后才发 `reminderDue(id)`。数据库失败时不发信号且保留重试资格；重启仍按已保存标记去重。`snooze(id, minutes=10)` 从当前墙钟顺延并清标记，拒绝非正分钟、已完成/已删任务。

先持久保存再发 signal 无法保证“用户一定看到一次”：若程序恰在提交后、UI 接收前崩溃，此次提示可能未显示。当前语义优先持久去重；如需跨进程确认送达，需未来另加 outbox/ack 协议。

## JSON 备份

根包含 `format="KylinNote.Tasks"`、`version=1`、`groups`、`tasks`、`recurrenceLinks`。id 和 revision 用十进制字符串，避免 JSON double 对 64 位整数的精度丢失；日期为含显式时区的 ISO 8601 字符串或 null。含 Unicode、HTML 格式正文、标签、清单、回收站状态和桌面属性均原样往返。UI 目前只提供 Markdown 编辑；保留 Html 枚举不会增加数据库/备份维护路径。

导出使用 `QSaveFile`，禁用直接覆盖回退。导入限制 64 MiB，先解析并全面验证所有类型、范围、日期、唯一 id、分组引用、完成状态和无环重复链，再在一个数据库事务中合并。组和任务一律分配新 id，不覆盖原数据；同名分组也单独创建。再次导入同一备份会再次合并一份，属明确的导入语义。

导入将新行 revision 归一化，同时保留“当前 revision 是否仍等于生成时 revision”的关系；已编辑后代仍禁止撤销，未编辑后代可安全撤回。旧版本/未来格式、数字代替布尔、错误日期或任何 SQL 失败都会保持现有缓存和数据库不变。

## 测试

`tests/tst_tasks.cpp` 使用临时 SQLite、QSignalSpy 和实际 SQL 触发器注入失败。覆盖 v1 源数据保留、迁移回滚、未知版本/失败连接、CRUD 与组删除、非法范围、陈旧 revision、重复/月末/撤销/防重复、提醒补发/重启/失败重试/回拨/snooze/完成/删除、Unicode/HTML/清单备份、全面校验及事务导入回滚。

构建由父级维护的 CMake 目标 `test_tasks`；链接 Qt Core/Gui/Sql/Test（现有目标也可链接 Widgets）。运行 `ctest --test-dir <build> -R tasks --output-on-failure`；测试默认使用 offscreen 平台。需在 Qt 5.12.8 Linux ARM64 和受支持 Windows 构建环境各验证一次。
