# 数据模型重构实施指引

本文件是给实施代理的操作手册：按阶段改哪几个文件、改什么、每阶段怎么验收。**语义以[数据模型文档](data-model.md)为准**，本文只讲动作。

## 目的与边界

- 把 `tasks` 单表改造为 `entities` 单表（`kind = task | note`），新增活动日志，新增正文内链，重整视图排序。
- 当前模型不设置工作区：删除 `workspaces` 表、全部 `workspace_id` 与默认工作区概念。文件夹只有一层且只容纳清单；清单可不属于文件夹，并以 `kind=task|note` 决定新建实体的默认类型与前端提示，不限制实际实体类型。
- 只改客户端本地层。**不实现**登录、服务器、网络同步、自定义视图界面。
- 不做数据迁移：schema 版本从 4 升到 5，旧的 v4 本地库直接拒绝打开（提示版本不支持），旧 `.klb` 不再导入。
- 完成后必须：整项目编译通过、`ctest` 全绿、手工冒烟通过。

## 硬性命名决策

| 决策 | 内容 |
|---|---|
| 领域类型 | `Task` → `Entity`，文件 `src/domain/Task.h` → `src/domain/Entity.h`；新增 `enum class EntityKind { Task, Note }` |
| 枚举命名 | `TaskStatus` 保留（值仍是 active/completed/abandoned） |
| 门面类 | 保留类名 `TaskStore`、`TaskRepository`，不重命名，避免 UI 大范围改动 |
| 访问器 | `TaskStore::tasks()` → `entities()`；`task(id)` → `entity(id)` |
| 便捷方法 | 保留 `createTask(title, listId)`（内部 `kind=Task`），新增 `createNote(title, listId)` |
| UI 类 | `TaskPanel` / `TaskDetailWidget` / `TaskListModel` / `TaskDelegate` 名称保留；它们现在同时渲染两种实体 |
| 兼容别名 | 如需过渡可用 `using Task = Entity;`，但**最终必须删除**，不允许长期保留两套名字 |

## 阶段 0：基线

1. 运行一次现有测试，记录通过状态：`ctest --test-dir <build> --output-on-failure`。
2. 确认工作树干净或已知改动范围；本阶段不提交。

## 阶段 1：schema（`src/db/Database.cpp`）

### 1.1 版本

- `schemaVersion()` 返回 **5**；`applicationId()` 不变（`0x4B4C5354`）。
- `initializeSchema()` 里的 `PRAGMA user_version` 写 5。

### 1.2 `entities`

```sql
CREATE TABLE entities(
  id TEXT PRIMARY KEY,
  kind TEXT NOT NULL CHECK(kind IN ('task','note')),
  title TEXT NOT NULL CHECK(length(trim(title))>0),
  body_markdown TEXT NOT NULL DEFAULT '',
  folder_id TEXT, list_id TEXT NOT NULL, section_id TEXT, parent_id TEXT,
  sort_order INTEGER NOT NULL DEFAULT 0 CHECK(sort_order>=0),
  status TEXT NOT NULL DEFAULT 'active' CHECK(status IN ('active','completed','abandoned')),
  deleted_at_utc_ms INTEGER,
  start_at_utc_ms INTEGER, due_at_utc_ms INTEGER, all_day_date TEXT,
  timezone TEXT NOT NULL DEFAULT 'Asia/Shanghai',
  recurrence_series_id TEXT, occurrence_key TEXT,
  priority INTEGER NOT NULL DEFAULT 0 CHECK(priority BETWEEN 0 AND 3),
  pinned_at_utc_ms INTEGER,
  revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),
  server_revision INTEGER NOT NULL DEFAULT 0 CHECK(server_revision>=0),
  created_at_utc_ms INTEGER NOT NULL, updated_at_utc_ms INTEGER NOT NULL,
  completed_at_utc_ms INTEGER, abandoned_at_utc_ms INTEGER,
  UNIQUE(id, kind),
  CHECK(kind='task' OR parent_id IS NULL),
  CHECK(parent_id IS NULL OR parent_id<>id),
  CHECK((status='completed')=(completed_at_utc_ms IS NOT NULL)),
  CHECK((status='abandoned')=(abandoned_at_utc_ms IS NOT NULL)),
  CHECK((recurrence_series_id IS NULL)=(occurrence_key IS NULL)),
  UNIQUE(recurrence_series_id, occurrence_key),
  FOREIGN KEY(folder_id) REFERENCES folders(id) ON DELETE RESTRICT,
  FOREIGN KEY(list_id) REFERENCES lists(id) ON DELETE RESTRICT,
  FOREIGN KEY(section_id) REFERENCES sections(id) ON DELETE RESTRICT,
  FOREIGN KEY(parent_id, kind) REFERENCES entities(id, kind) ON DELETE CASCADE,
  FOREIGN KEY(recurrence_series_id) REFERENCES recurrence_series(id) ON DELETE RESTRICT
);
```

删除列：`description`（→ `body_markdown`）、`content_format`、`favorite`。`pinned` 本来就不是列，不要加。

索引：

```sql
CREATE INDEX idx_entities_list ON entities(list_id, section_id);
CREATE INDEX idx_entities_parent ON entities(parent_id);
CREATE INDEX idx_entities_due ON entities(kind, status, due_at_utc_ms);
CREATE INDEX idx_entities_trash ON entities(deleted_at_utc_ms);
CREATE INDEX idx_entities_pinned ON entities(pinned_at_utc_ms) WHERE pinned_at_utc_ms IS NOT NULL;
CREATE UNIQUE INDEX idx_entity_occurrence ON entities(recurrence_series_id, occurrence_key)
  WHERE recurrence_series_id IS NOT NULL;
```

### 1.3 其余表的改名与降级

| 表 | 改动 |
|---|---|
| `workspaces` | **删除**，同时删除其他表的 `workspace_id` 与相关复合外键；普通 UUID 外键负责引用完整性 |
| `folders` | 删除 `parent_id`，只保留单层文件夹；删除文件夹时清单的 `folder_id` 置空，不删除清单 |
| `task_lists` | **改名 `lists`**；新增 `kind TEXT NOT NULL DEFAULT 'task' CHECK(kind IN ('task','note'))`；`folder_id` 继续可空；全库仅一个 `is_inbox=1` |
| `saved_filters` | **改名 `view_defs`**，列 `expression_json` → `spec_json`（存完整视图定义） |
| `checklist_items` | `task_id` → `entity_id`；`FOREIGN KEY(entity_id) REFERENCES entities(id) ON DELETE CASCADE`；**不加日期/提醒字段** |
| `reminders` | `task_id` → `entity_id`；**普通单列外键**，不带 `kind` 复合约束（复合外键会阻止任务转笔记） |
| `attachments` | `task_id` → `entity_id`，外键指向 `entities` |
| `desktop_notes` | `task_id` → `entity_id`，外键指向 `entities`；仍保留 `device_id` 唯一约束 |
| `task_recurrence_links` | 改名 `recurrence_links`；`source_id` / `next_id` 指向 `entities` |
| `task_tags` | 改名 `entity_tags`，列 `task_id` → `entity_id` |

### 1.4 新增 `entity_events`

```sql
CREATE TABLE entity_events(
  id TEXT PRIMARY KEY,
  entity_id TEXT NOT NULL,            -- 故意不加外键：实体永久删除后日志仍要存在
  entity_kind TEXT NOT NULL CHECK(entity_kind IN ('task','note')),
  entity_title TEXT NOT NULL,
  batch_id TEXT NOT NULL,
  type TEXT NOT NULL CHECK(type IN ('created','changed','kind_changed','trashed','restored',
                                    'deleted','attachment_added','attachment_removed',
                                    'reminder_added','reminder_removed')),
  field TEXT NOT NULL DEFAULT '',
  old_value TEXT, new_value TEXT,
  occurred_at_utc_ms INTEGER NOT NULL
);
CREATE INDEX idx_events_entity ON entity_events(entity_id, occurred_at_utc_ms);
```

日志只增不改，不参与 CAS，明确不设置 `revision`；其他可变、可同步业务行继续保留 revision 条件更新。

### 1.5 种子数据

- 默认创建一个 `kind='task'` 的系统收集箱；`local_meta.default_timezone = Asia/Shanghai`，不再设置默认工作区。
- `local_meta` 其余设备与同步键不变。

### 1.6 验收

- `freshSchemaNeverMigratesLegacy` 改断言：`user_version=5`、`entities` 存在、`tasks` 不存在、`entities` 无 `favorite`/`content_format`/`description` 列、`entity_events` 存在且 `entity_id` 无外键、`reminders` 无 `kind` 列。

## 阶段 2：领域层

### 2.1 `src/domain/Entity.h`（由 `Task.h` 改名）

- `struct Entity`：新增 `EntityKind kind`；`description` → `body`；删除 `favorite`、`pinned`、`groupId`（`listId` 唯一）、`contentFormat`；新增 `QDateTime pinnedAt` 与非负 `qint64 sortOrder`，与 DDL 对齐。
- `TaskList` 增加 `EntityKind kind`，默认 `Task`；`Folder` 删除 `parentId`；删除 `Workspace` 领域类型及所有工作区访问器。
- 保留门面：`completed`（= `status==Completed`）、`deleted`（= `deletedAt` 有效）。
- `nextRecurrence` / `nextRecurrenceDate` / `parseRecurrence` 等纯函数原样保留。
- 检查清单 `ChecklistItem` 的 `taskId` → `entityId`。

### 2.2 `src/domain/EntityCodec.h`（由 `TaskCodec.h` 改名）

- JSON 键：`description` → `bodyMarkdown`，新增 `kind`、`sortOrder`、`pinnedAtUtcMs`，删除 `favorite`、`contentFormat`。
- `validate()`：增加 `kind` 合法性；`kind=note` 时**不校验**任务字段为空（字段允许保留），但必须校验 `parentId` 为空；`kind=task` 时保持现有全部校验。
- 不做 `note` 与任务字段的一致性检查——这是有意设计。

### 2.3 `src/domain/BackupCodec.h`

- 标识改为 `KylinList.Entities`，版本 **2**。
- 数组 `tasks` → `entities`，元素带 `kind`；新增 `events` 数组；根元数据保存 `defaultTimezone`。
- 不导出 `workspaces` 或 `local_meta`；系统收集箱由 `lists` 中唯一 `is_inbox=1` 的记录表达。
- 旧格式直接拒绝，不写兼容分支。

### 2.4 验收

- 编译通过；`EntityCodec` 的单元用例覆盖 kind 与保留字段。

## 阶段 3：仓储（`src/db/TaskRepository.{h,cpp}`）

1. 结构体改名：`StoredTask` → `StoredEntity`（成员 `Entity entity`），`TaskSnapshot` → `EntitySnapshot`，`TaskChanges` → `EntityChanges`；成员 `tasks` → `entities`；新增 `QVector<EntityEvent> eventSaves`。
2. 所有 SQL 的 `tasks` → `entities`，列名同步；读取时填充 `kind`、`pinnedAt`，移除 `favorite`。
3. 新增 `bool appendEvents(const QVector<EntityEvent> &, QString *error)`，在 `apply()` 的同一事务内写入。
4. 新增 `QVector<EntityEvent> eventsFor(const QString &entityId, QString *error)`，**只在需要时调用**，不进入快照。
5. 新增 `bool convertKind(const QString &id, EntityKind to, QString *error)`：
   - 校验：`to=Note` 时该实体必须是 `task`、`parent_id` 为空、且不存在子实体；否则失败并给出原因；
   - 只更新 `kind`（以及 `updated_at`、`revision`），**不改动其他列**；
   - 同一事务内写一条 `kind_changed` 事件。
6. 删除 `favorite` 相关读写；`desktop_notes` 与 `reminders` 的 SQL 改用 `entity_id`。
7. 保留既有事务边界与「提交后才回填快照」的约定。

### 验收

- `completeModelAndUuidCrud`、`inboxAndWorkspaceIsolation` 通过；
- 新增：转换后任务字段仍存在（查库断言）；有子任务的实体转换被拒绝。

## 阶段 4：应用门面（`src/app/`）

1. `TaskStore`：
   - `tasks()` → `entities()`，`task(id)` → `entity(id)`；
   - 新增 `createNote(title, listId)`、`convertKind(id, kind)`、`setPinnedAt(id, QDateTime)`（NULL 表示取消置顶）；快速添加不传显式类型时，以目标 `lists.kind` 决定默认实体类型。
   - 新增日志 API：`QVector<EntityEvent> events(const QString &entityId)`、`QString beginEditSession(const QString &entityId)`（返回独立会话 UUID）、`void endEditSession(const QString &sessionId)`；另提供结束全部活动会话的内部入口供退出使用。
   - 删除 `workspaceId()`、工作区快照过滤、`setPinned`（贴桌面）与所有 `favorite` 读写；
   - 贴桌面动作改用 `saveDesktopState(..., open)`，门面字段不再提供。
2. `NoteManager`：
   - 判断是否物化便签改为 `store->desktopNote(id).open`，且**仅 `kind == Task`**；
   - 关闭便签写 `saveDesktopState(..., open=false)`，不触碰实体；
   - 文本编辑结束后调用 `endEditSession`。
3. `ReminderScheduler`：所有候选过滤加 `kind == Task`；笔记即使残留提醒行也不得触发。
4. `KylinListApp`：
   - 退出顺序：面板/便签 flush 并完成实体提交 → 结束所有编辑会话并提交日志 → 销毁 UI → 关数据库；
   - 提醒弹窗按 `(taskId, reminderId)` 管理，逻辑不变，但入口加 kind 检查。

### 验收

- `desktopNotesAreDeviceIndependent` 改为断言 `open` 字段，不再断言 `pinned`；
- 新增：把带提醒的任务转笔记后 `checkDue()` 不产生信号。

## 阶段 5：视图（新增纯逻辑模块）

1. 新增 `src/domain/ViewSpec.h/.cpp`，**不含 Qt Widgets**：
   - `enum class ViewRenderer { List, Time }`；
   - `enum class ViewScope { AllLists, List, Today, Next7Days, Overdue, Completed, Abandoned, Trash }`；
   - `struct ViewSpec { ViewScope scope; QString listId; ViewRenderer renderer; QString search; }`；
   - `QDate effectiveDate(const Entity &)`：due 日期 → `all_day_date` → start 日期；
   - `bool contains(const Entity &, const ViewSpec &, const QDate &today)`；
   - `struct ViewSection { QString title; QVector<Entity> items; }`（避免与组织分区 `Section` 重名）；
   - `QVector<ViewSection> buildSections(const QVector<Entity> &, const ViewSpec &, const QDate &today)`。
2. 删除 `src/ui/TaskFilter.{h,cpp}`，把其中的搜索匹配逻辑迁移进 `contains`。
3. 排序实现严格按数据模型文档：
   - 全局前缀 `pinned_at IS NOT NULL DESC, pinned_at DESC`（归档类 scope 不加）；
   - 段内：已过期/未过期按有效日期 ASC；无日期与笔记按 `created_at` DESC；
   - 全序兜底 `id ASC`。
4. `TaskListModel`：角色改为 `IdRole / KindRole / MetaRole / PriorityRole / CompletedRole / DeletedRole / OverdueRole / PinnedRole`，删除 `FavoriteRole`；`setFilter(ViewSpec)` 统一调用 `ViewSpec::buildSections` 并接收其段列表，不另写筛选或排序分支。
5. `TaskDelegate`：删除星标绘制与 `textWidth` 的 40px 预留；新增笔记类型标记与置顶图钉。
6. `TaskPanel`：
   - 侧栏删除「重要与星标」「已贴桌面」两项；
   - 智能列表保留：全部待办、今天、七天内、已逾期、已完成、已放弃、回收站；
   - 加视图切换（清单视图 / 时间视图），时间视图仅对今天/七天内/已逾期可用；
   - 快速添加按当前 `lists.kind` 默认创建任务或笔记，并提供对应图标与提示语；清单类型不限制已有实体。
   - 文件夹只提供单层新建/重命名/删除，不提供嵌套入口；清单可以不归文件夹，删除文件夹后清单平铺。
   - 右键菜单：新增「置顶 / 取消置顶」「转为笔记 / 转为任务」，保留「贴到桌面 / 关闭便签」。
7. `TaskDetailWidget`：
   - 删除「星标关注」；
   - 「贴到桌面」改为动作按钮，不进 dirty 集合；
   - 新增置顶开关与类型转换按钮（转换前确认，列出保留/暂停的字段）；
   - 新增「活动日志」入口。

### 验收

- `filtersUseShanghaiCalendarAndSearch` 改为基于 `ViewSpec::buildSections` 的断言；
- 新增：置顶跨两视图都在最前；过期按日期分块（09:00 截止当天仍算未过期）；笔记与无日期任务不进时间视图。

## 阶段 6：内链

1. 新增 `src/domain/InternalLink.h/.cpp`：`QStringList internalTargets(const QString &body)`（提取 `kl:` 后的 UUID）、`bool isValidLinkTarget(...)`。
2. `src/ui/Markdown.cpp`：
   - 渲染前收集 `kl:` 目标，向 `TaskStore` 批量解析标题与状态；
   - 输出 HTML 时把链接文字替换为**目标当前标题**；断链降级为 `<span class="broken-link">`，不输出 `href`；
   - 回收站目标保留 `href` 并加状态 class。
3. `DocumentPreview`：
   - `anchorClicked` 中 `scheme()=="kl"` 时不走 `QDesktopServices`，发内部跳转信号；
   - 提供 `internalLinkAt(const QPoint&)` 供悬浮使用。
4. 新增悬浮预览组件（`QWidget` + `QTimer`，延迟 500ms）：显示标题、类型、状态与正文纯文本摘要；目标为笔记时显示"笔记"，任务显示完成/过期状态。
5. 编辑器工具栏新增「引用」按钮：打开实体选择器，插入 `[标题](kl:uuid)`。
6. **不实现**内联链接的可点击编辑；编辑区纯文本，仅预览可点。

### 验收

- 新增用例：改名后预览显示新标题；目标删除后链接降级为文字且不崩溃；跨清单引用可跳转。

## 阶段 7：活动日志

1. 新增 `src/app/Journal.{h,cpp}`：
   - 收集待写事件，`EntityChanges::eventSaves` 为载体；
   - `beginEditSession(id)` 返回独立会话 UUID，记录该编辑器的 `title`/`body` 与清单项文本快照（只在内存）；同一实体的详情和便签可各自持有会话。
   - `endEditSession(sessionId)` 比较当前值：标题或正文有变化则各生成一条 `changed`，`new_value` 为**完整内容**；清单项文本按 `checklist.<条目UUID>.text` 逐条记录完整新文本。勾选使用 `checklist.<条目UUID>.done`，在每次离散动作发生时记录旧/新 `0/1`。
2. 会话边界接入点：
   - `TaskDetailWidget`：标题/正文编辑器失焦、切换实体、面板关闭；
   - `NoteWidget`：便签内编辑失焦、便签关闭；
   - `KylinListApp::requestQuit()` / `shutdown()`：在 flush 之后、提交之前结束所有会话。
3. 标量字段在 `saveTask` 内比较新旧值生成事件；状态、时间、优先级、清单、分区、置顶、重复各一条。
4. `trashed` / `restored` / `deleted` / `attachment_added` / `attachment_removed` / `reminder_added` / `reminder_removed` 在对应操作处生成。
5. `batch_id` 每次 `apply()` 生成一个 UUID，写入该次事务的全部事件。
6. 永久删除实体时写入最后一条 `deleted` 事件；因 `entity_id` 无外键，事件保留。
7. 日志面板：打开时按 `events(entityId)` 查询，倒序展示，按 `batch_id` 折叠同一批变更。

### 验收

- 新增用例：连续修改标题 5 次只产生 1 条（会话内）；失焦后产生 1 条；关闭程序前不丢最后一条；编辑正文后 `new_value` 为全文；永久删除后日志仍可查询。

## 阶段 8：备份（全量导出与覆盖恢复）

语义是镜像，不是合并。`merge()` 与它带来的全部 ID 映射代码都要删除。

1. `BackupCodec`：格式标识 `KylinList.Entities`、版本 2；数组 `tasks` → `entities`；新增 `events`；根元数据保存 `defaultTimezone`。不包含工作区或 `local_meta`，导出为全库镜像。
2. 删除 `TaskRepository::merge()` 及其配套：文件夹/清单/分区/系列/实体/附件的 ID 映射表、引用重写、`protectRecurrenceTargets`、revision 归一化。
3. 新增 `bool replaceAll(const EntitySnapshot &incoming, const QVector<EntityEvent> &events, const QString &defaultTimezone, QString *error)`，在单个事务内：
   - 按依赖顺序清空：`entity_events` → `reminder_deliveries` → `desktop_notes` → `attachments` → `checklist_items` → `reminders` → `entity_tags` → `tags` → `recurrence_links` → `entities` → `recurrence_series` → `sections` → `lists` → `folders` → `view_defs`；
   - 按备份内容原样重建 UUID 与引用；
   - `local_meta` 不清空，只从备份唯一的系统收集箱更新 `inbox_list_id`、从根元数据更新 `default_timezone`，保留 `installation_id` / `device_id`，清空 `signed_in` 与 `sync_base_server_revision`。
4. 附件：导出按帧流式写出；导入先写暂存目录，在数据库事务提交前完成可恢复的新旧文件交换，事务失败或文件失败都恢复原状；提交成功后只删除旧文件备份与暂存目录。
5. 导入时序（必须按此顺序）：结束编辑会话 → 冻结旧详情和便签的写入 → 执行可恢复的文件交换与数据库替换 → 提交成功后关闭全部便签与详情并清理文件备份 → 重置调度器 → 重建导航与列表 → 为已到期提醒补投递凭证 → 显示摘要（恢复数量）。失败则解冻原界面并保持原数据。
6. 上限：容器 2 GiB、元数据 256 MiB、单附件 2 MiB；超限在解析前返回明确错误，不得部分写入。
7. UI：导入入口二次确认，文案明确"本机现有全部任务、笔记与清单会被删除"，并提示先导出。

## 逐文件清单

| 文件 | 动作 |
|---|---|
| `src/db/Database.cpp` | 重写 `kSchema`；版本 5；种子数据；新索引；`entity_events` |
| `src/db/TaskRepository.h/.cpp` | 表名/列名改造；结构体改名；`appendEvents`/`eventsFor`/`convertKind`；删 `favorite` |
| `src/domain/Task.h` → `Entity.h` | `Entity` + `EntityKind`；删 `favorite`/`pinned`/`groupId`/`contentFormat`；加 `pinnedAt` |
| `src/domain/TaskCodec.h` → `EntityCodec.h` | JSON 键改造；`validate` 按 kind 分支 |
| `src/domain/BackupCodec.h` | 格式 v2；全量导出；`entities` + `events` + 根级 `defaultTimezone` |
| `src/domain/ViewSpec.h/.cpp`（新增） | 视图范围、有效日期、分段与排序 |
| `src/domain/InternalLink.h/.cpp`（新增） | `kl:` 提取与校验 |
| `src/app/TaskStore.h/.cpp` | `entities()`/`entity()`；`createNote`/`convertKind`/`setPinnedAt`；日志与编辑会话；删 `setPinned` |
| `src/app/Journal.h/.cpp`（新增） | 事件收集与会话比较 |
| `src/app/NoteManager.cpp` | 便签物化改用 `desktopNote().open` 且仅任务 |
| `src/app/ReminderScheduler.cpp` | 候选过滤加 `kind==Task` |
| `src/app/KylinListApp.cpp` | 退出顺序；结束编辑会话 |
| `src/ui/TaskFilter.h/.cpp` | **删除**，逻辑迁入 `ViewSpec` |
| `src/ui/TaskListModel.h/.cpp` | 角色调整；按段渲染 |
| `src/ui/TaskDelegate.cpp` | 删星标；加笔记标记与置顶图钉 |
| `src/ui/TaskPanel.cpp` | 删两项导航；视图切换；新增置顶与转换菜单 |
| `src/ui/TaskDetailWidget.h/.cpp` | 删星标；贴桌面改按钮；置顶、转换、日志入口 |
| `src/ui/NoteWidget.cpp` | 会话边界；不涉及 favorite |
| `src/ui/Markdown.h/.cpp` | `kl:` 标题解析与断链降级 |
| `src/ui/MarkdownEditor.cpp` | 「引用」按钮 |
| `tests/tst_tasks.cpp` | 见下 |
| `tests/tst_ui.cpp` | 见下 |

## `kind='task'` 过滤点清单（最容易漏）

实现时逐个确认，每个点都必须显式判断：

1. `ViewSpec::contains`：今天 / 七天内 / 已逾期 / 时间视图
2. 清单视图的「已过期 / 未过期 / 无日期」三段
3. 已完成、已放弃两个归档 scope
4. `ReminderScheduler::checkDue`
5. `NoteManager::syncTask`（便签物化）
6. 详情页与便签的提醒显示
7. 进度或统计类派生值
8. 未来新增的日历/看板渲染器

搜索是例外：**搜索同时覆盖两种实体**，不按 kind 过滤。

## 测试改造清单

| 现有用例 | 处理 |
|---|---|
| `freshSchemaNeverMigratesLegacy` | 改版本号、表名、列断言；加 `entity_events` |
| `completeModelAndUuidCrud` | 表名改 `entities`；加 `kind` 断言 |
| `inboxAndWorkspaceIsolation` | 改名 `inboxAndOrganizationIntegrity`：验证唯一收集箱、单层文件夹、可空清单文件夹归属、外键与越权写入拒绝 |
| `taskAutosaveValidationAndTransactions` | 去 `favorite`；加 kind 校验 |
| `recurrenceMonthEndAndOccurrenceUnique` | 表名改；逻辑不变 |
| `recurrenceDailyWeeklyAndSafeUndo` | 表名改；逻辑不变 |
| `recurrenceUndoPreservesAssociatedData` | 表名改；逻辑不变 |
| `staleStoreCannotUndoAssociatedOccurrence` | 表名改；逻辑不变 |
| `cloneCompletedRecurringTask` | 去 `favorite` 断言 |
| `multipleRemindersDeliveryRestartAndSnooze` | 表名改；加"笔记不触发" |
| `attachmentsShaStateAndTransactions` | 列名 `entity_id` |
| `desktopNotesAreDeviceIndependent` | 用 `open` 断言替换 `pinned` |
| `allDayRecurrenceAndNestedBackup` | 备份数组名改 `entities` |
| `backupRoundTripAndLegacyRejection` | 格式 v2；改为"导出→覆盖恢复→逐表比对"；旧格式仍拒绝 |
| `backupValidationAndSqlRollbackAreAtomic` | 校验失败不得清空本机数据 |
| `backupRecurrenceUndoProtection` | 覆盖恢复后重复撤销保护仍成立 |
| `outboxDoesNotAccumulateWhileLoggedOut` | 逻辑不变 |
| `appTimeZoneHelpers` | 逻辑不变 |
| `tst_ui` 全部 | 去 `favorite`；视图断言改 `ViewSpec`；`pinned` 改 `desktopNote` |

新增用例（至少）：

- 笔记无父子：直接写库被 CHECK 拒绝；
- 有子任务或自身为子任务的实体转换被拒绝；
- 转换后任务字段仍在库中，转回后完全一致；
- 带提醒/截止/已完成的实体转笔记后不进任何工作视图、不触发提醒；
- 置顶在两视图都排最前，且不豁免时间范围过滤；
- 摘要所需：过期按日期判定，09:00 截止当天仍属未过期；
- 内链：改名同步显示、断链降级、回收站可点；
- 日志：标量逐次、文本按会话、`new_value` 为全文、退出不丢、删除后仍可读；
- 备份：覆盖导入后逐表一致（含 `entity_events`）、`defaultTimezone` 与唯一收集箱恢复；UUID 与 `kl:` 内链不变；导入失败后本机数据完好；导入后不补发过期提醒；
- 检查清单：正文 `- [ ]` 不计入进度。

## 验收命令

```bash
cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-check --parallel
ctest --test-dir build-check --output-on-failure
```

手工冒烟（`--data-dir` 独立目录）：

1. 新建任务与笔记，同清单内可见；
2. 置顶一个任务，确认在清单视图与时间视图都排最前；
3. 把「今天」里截止明天的置顶任务移出时间视图；
4. 任务转笔记再转回，时间、提醒、优先级完好；
5. 正文写 `[x](kl:<uuid>)`，预览点跳转、悬浮出摘要，改名后显示新标题；
6. 改标题、改正文、勾选清单项，打开日志核对条目数与全文；
7. 导出 `.klb`，在另一个空数据目录导入，确认实体、清单、日志、附件、内链与导出前完全一致；再往本机加一条数据后重复导入，确认本机数据被完全覆盖而不是合并。

## 明确不做

- 不实现账号、服务器、同步协议；
- 不实现自定义视图编辑界面；
- 不实现正文清单语法与检查清单的双向转换；
- 不实现检查清单项的日期或提醒；
- 不实现日志裁剪与日志分帧（容器 2 GiB、元数据 256 MiB 已够用，超限给明确错误）。
