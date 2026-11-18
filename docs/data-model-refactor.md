# 数据模型重构实施指引

本文件保留最初实体模型改造的决策与实施历史，并在前部维护本轮实施契约。**当前语义以[数据模型文档](data-model.md)及下方 2026-11-17 规格为准**；历史指引不是当前待办，也不能作为测试通过记录。

## 2026-11-17 当前实施契约：完整调度，开发库直接重建

1. 当前只初始化 schema10，不做 v9→v10 或其他旧库迁移，也不做 v5 备份导入转换。开发预览使用新的空数据目录；已有不匹配数据库仍只读拒绝，不能擅自删除或重置。
2. `ScheduleValue` 一次携带时刻／全天范围、多个正常提醒和完整 `RecurrenceSpec`。`all_day_end_date` 与 `has_schedule_range` 明确表达包含式范围；同一天两端也不能压成单点。
3. `recurrence_series.rule_json` 是权威规则，`occurrence_ordinal`／`completion_count` 保存进度；`rrule` 仅为非权威频率摘要。原始 `dtstart`／时区必须传入推进引擎，保持间隔相位与农历锚点。
4. `reminders.rule_json` 保存正常一次性提醒的锚点、偏移和全天本地时间，`trigger_at_utc_ms` 为已解析UTC。修改日期重算相对提醒，稍后提醒仅修改本次绝对时刻。**没有持续提醒开关、字段或逻辑**。
5. 任务和笔记都允许日期及提醒；笔记不进入任务时间聚合，第三列不显示笔记日期，提醒弹窗无完成。没有法定节假日、跳过节假日或艾宾浩斯记忆法。
6. 日期弹窗外层确认才提交；保存前比较调度基线，避免覆盖其他编辑器的日期／提醒。完成最后一次重复正常结束，安全撤销继续保护附件、子任务和后续修改。
7. 当前备份为 `KylinList.Entities` v6，完整保存范围、规则、进度与提醒规则；恢复路径所有显式SQL列和绑定必须同步。测试只覆盖当前格式新建／重开／往返和旧版本拒绝，不编写迁移或回滚到旧版的兼容层。

以下 2026-11-16 及更早段落为历史实施记录，版本号和已被取代的单日期／任务专属提醒描述不适用于当前代码。

## 2026-11-16 历史实施契约

本轮在 Qt 5.12.8／C++14、Ubuntu 20.04 容器构建基线上继续使用 Qt Widgets；不增加依赖、不重做兼容性调研，也不迁移到 QML／WebView。这里只同步已批准规格与落盘入口；全量构建、CTest、原生手势与目标机结论须按实际执行补录，不能把历史指引里的“应通过”写成“已通过”。

### 1. schema v9：仅新库与当前库，不做旧版升级

涉及 `src/db/Database.*`、仓储与当前实体 codec；旧内容迁移器不再是当前入口。

1. 保留 `application_id=0x4B4C5354`，完全空库直接建立 v9；已有数据库必须是标识正确的当前 v9。**v5/v6/v7/v8、错误标识及未知／未来版本一律拒绝**。
2. 已有文件先以只读连接检查，再进入可写 pragma／建库流程；拒绝发生在任何写入、WAL 切换或模式变更之前。不得先改文件再报告不支持。
3. 没有自动升级、补列、旧检查项转正文、迁移回放或升级前 `VACUUM INTO`／文件副本；不静默删除、重置用户旧库，不以旧产品文件回退。旧 `notes.db` 不读取、不转换、不删除。
4. 当前 schema 直接包含事件 `context_json`、清单／文件夹外观等现行结构，没有独立检查项表或领域集合。正文任务列表通过 `- [ ]`／`- [x]` 表示，点击只改 Markdown 源码、进入正文撤销与编辑会话，不产生独立完成对象或业务进度。

### 2. 备份元数据 v5：只保留当前格式

涉及 `src/domain/BackupCodec.h`、`EntityCodec.h`、`TaskRepository::replaceAll()` 和既有附件交换／覆盖恢复链路。

- 导出与读入均为 `KLB1`／`KylinList.Entities` 元数据版本 **5**；根无 `checklistItems`、实体无 `checklist`，事件 `context` 为 JSON 对象。当前格式夹带旧字段即拒绝，不能悄悄忽略。
- v2/v3/v4、其他旧格式和未知未来版本在修改本机库／附件前拒绝。不保留旧 codec、冗余双表示转换、旧外观补齐或旧链接迁移回退。
- 全量覆盖、先暂存附件、可恢复文件交换、单事务替换、失败回滚、导入后历史提醒不补发等契约不变；UUID／revision／更新时间与重复凭证仍按当前镜像恢复契约保存。容器 2 GiB／元数据 256 MiB／单附件 2 MiB 上限不变。

### 3. 活动白名单、context 与内嵌时间线

涉及 `ActivityPolicy.h`、`EntityEvent`、`Journal`、`TaskStore`、`TaskRepository`、`BackupCodec` 和 `ActivityTimelineWidget`。

- `EntityEvent.context` 是 JSON 对象，SQL 列为 `context_json`，备份键为 `context`，默认 `{}`。父项标题、清单名与附件名保存为当时快照，不能把当前查询到的新名称冒充历史；没有名称上下文时用通用文案，不泄露技术 ID。
- 写入端与展示端共用 `ActivityPolicy` 白名单，只记实质字段和生命周期；排序／pin／section／文件夹／外观与本机显示偏好不记。保留旧原始事件供追溯，不必因过滤噪声删除历史。
- 标题／正文编辑会话只比较文本；详情和便签独立基线，失焦／切换／关闭／退出结束。仅日志落盘也要在提交后发 `eventsChanged(entityId)`，不能依赖 `taskChanged` 碰巧刷新。
- 时间线位于第四列 `DetailScroll` 正文流下部，附件另置底部独立滚动面板；时间线可关闭、最新在前。20px 标记、14px 摘要、12px 时间、32px 左缩、20px 间距与 1px 连接线。创建摘要有标题，父级称“主任务”，无账号不伪造 Actor，不弹旧日志对话框。

### 4. 五分组与类型化拖放

涉及 `TaskGrouping.*`、`ViewSpec.*`、`TaskDrop.*`、`TaskListModel`／`TaskListView`、`TaskPanel`、`DisplaySettingsDialog` 与 `TaskStore`。

- GroupMode 为 List／Time／Created／Priority／None；`contains()` 只管范围／搜索／资格，分组不能改变筛选 ID 集。文件夹默认 List，其他 Time；单清单／收集箱隐藏 List。本机 QSettings 以 `grouping/<scope>/<id>` 保存字符串，各范围独立。
- 活动视角顺序 pinned → 普通模式组 → notes → completed → abandoned；None 只隐藏普通任务组标题。`showCompleted` 同时控制完成与放弃，显示设置复选框与菜单 quick toggle 共用状态。
- 历史 Completed／Abandoned／Trash 支持全部五模式并真正分组，忽略 pin，不追加活动特殊页脚，所有模式只读禁拖，不伪造状态。
- Time 六桶 expired（实现 `time:overdue`）／today／tomorrow／next7days（+2..+6）／later（+7 起）／undated；Created 为 0..6／7..29／≥30／未知（含未来）。组内 sortOrder ASC → 自然字段 → id，终态默认对应时间倒序；无排序／顺序 UI。
- 只传 `TaskDrop::Intent`，由 `TaskStore::applyDropIntent()` 重读最新快照，校验源／目标组、锚点、唯一顺序、清单与完整祖先链；显式移动共用 `moveEntityToList()`。不保留多 bool 旧拖放 API 的并行分支。
- 同父级、同显示组重排只改 rank；同显示组 Before／After 跨父级时按锚点进入目标同级，脱父后按真实属性补齐目标组条件。跨 Time 取昨天／今天／明天／后天／+7 最近合法日期，支持无日期互转并保留时刻、跨度、提醒与重复身份，旧非连续 rank 也适用。Priority 目标为 0..3；跨 List 带完整子树（含隐藏、折叠、终态后代），独立跨组子项脱父、离 pin 清 pin；AsChild 只 parent／order，不暗改 date／priority／pin。
- 可见 `orderedIds` 仅作落点提示：以最新完整兄弟集合为基础，仅移动源项，保留隐藏项及非源成员的最新相对顺序；可见同级顺序未变则零写。实际重编号的隐藏成员同样参与 GroupMove／CAS／事务。置顶任务和笔记共用 rank，不以类别覆盖手动顺序。
- Created 实际起拖在第三列底部 WorkspaceToast 提示且零写入；终态目标拒绝，Notes 不隐式转 Task。非法／陈旧／循环及 CAS 失败原子回滚；变更按 ID 合并，不能同一事务重复 touch 导致正常提交冲突。

### 5. UI 与输入边界

具体几何以 [UI 设计](ui-design.md)和 [UI 对齐契约](ui-alignment-contract.md)为准，此处保留跨数据／UI 的准入点。

- Caption 9pt=12px，Windows 优先 Microsoft YaHei UI／YaHei 等可缩放中文无衬线，Linux 走 Noto 等；PreferAntialias|PreferOutline，不安装或分发专有字体。
- 主任务链接／标题／正文 origin 24px，body documentMargin 为 0，title 用 textMargins.left=-2 抵消 Qt5.12 固定2px文字内缩；不改既有 28px 控制条／52px 首行中心／48px header／7px 与 5px 补偿。完成框 17px、日历 date-unselected 20px、30px 命中及中间 1×14px 分隔；overlay 旧 info 前分隔保留。
- Footer 右侧只有 A／more 两个 18px 图标；详情更多复用 EntityCommands 164px 卡片／34px 行／18px icon／12px 圆角／4px 行外缩进／12px 行内图标起点／8px 图文间距。WorkspaceMenu 父卡片 202px，GroupingMenu 164px。
- 第四列底栏上方设独立附件区，左右 24px、空时整体隐藏、自己的有界滚动；高度同时受三张卡片、可用高度 1/3 和正文至少 120px 预算约束。标题／正文／动态保留 `DetailScroll`；空和短正文都由真实编辑 viewport 填满余白，不覆盖点击层，不把附件塞回正文流。
- `EditorTextEdit` 提供真实 gutter 和块菜单；加号仅在光标所在的真正空源码普通段落聚焦且可编辑、无选区／IME预编辑时显示，不跟随悬停行，不画交互背景，按字体／QTextLayout基线定位。段落／三级标题／无序、有序、Markdown任务列表／引用／分割线均只写Markdown，非空段落转换走格式入口。选区岛32px、底栏格式岛36px（常规宽304px），18px glyph、10px圆角；选区按钮24px与上下各4px实际布局边距。更多与色板都是同类PopupMenu，34px行／4px外缩进／12px圆角，窄边界折叠进同一列表，不打开第二条横向岛。
- 不创建或保留隐藏／屏幕外 `EditorToolbar`、`format0..` 等替身控件；所有格式入口和测试都使用真实可见工具栏、选区、gutter 与菜单。浮层共用 `PopupShadow` 的整图高斯阴影，不用 graphics effect／mask 掩盖几何。
- 正文内部引用只有 `[[UUID|literalalias]]`／`[[UUID]]`，别名字面保留，无别名才显示当前标题；内部导航 href 为裸 UUID，不读旧 scheme、不双读或迁移。标题包含补全为非模态，IME preedit 不误确认，Escape 只关候选；链接悬浮复制／编辑，编辑弹层仅“文本”和“链接”两字段。
- 格式、块、补全与链接动作保存绝对 UTF-16 源偏移，通过 sourceSelection／selectSource 恢复，附 contextGeneration；setContent／setStore／setEntityId 后旧弹层与迟到回调必须失效。自有弹层以 markdownEditor 属性／ownsPopup 识别，焦点交接不丢选区或误结束编辑会话。
- 正文关联打开独立卡片宿主并复用完整`TaskDetailWidget`，不替换源第四列；同目标不重置，循环关联复用单卡片，便签正文同路由，主任务／创建结果／便签标题栏导航保留。关闭／换目标先finishEditing，失败留草稿；外点不穿透、Esc先交子交互，纳入退出／恢复冻结及Store生命周期。
- 文本使用瞬态`TextDraft`字段三方比较，未改字段保留当前值、同字段冲突整份零写，不恢复回收站／不存在实体。会话取消只结清自身已保存字段，日志失败保留会话，成功终结与归因在对外信号前完成；不改schema或备份格式。便签正文与桌面状态分开提交，原生键盘编辑与呈现共用Qt事务，保留源码撤销／重做。
- 名称与logo的本机定制只通过外部`Branding`配置；第一列首行透明原Logo，窗口／任务栏／托盘共用固定白底版本，不改原图、不扩展安装身份；私有显示名、配置路径和logo不进入任何受跟踪文件。
- 本地单／多文件 dragCopy 覆盖详情及子控件，预先 finishEditing 失败则 0 导入；共用原选择器的 2 MiB 校验与逐文件成功语义，不递归目录、不动原文件、不往正文插 URL。
- 日期子层 AnchoredListPopup 为 252px／34px 行／12px 圆角，时间总高≤280px，固定输入不压缩；下方 4px 锚定，不足先缩列表再上翻。模式按钮 StrongFocus／autoDefault=false、无 focus 边框；只真实支持 daily／weekly／monthly／custom。子取消不关父，父取消 0 写入。

### 6. 验证覆盖与结果交接

下表说明测试职责，不代表运行结果；实际执行环境、通过／失败／跳过与警告统一见[当前验证记录](updates.md#当前实现与验证状态)：

| 入口 | 应覆盖 |
|---|---|
| `tests/tst_database.cpp`（取代 `migrations`） | v9 新库／当前库准入、无旧表、旧 v5–v8／错误标识／未来版本在写入前拒绝且不生成副本；备份仅当前 v5、旧版本及旧字段拒绝、context 往返与事务失败边界 |
| `tests/tst_groupmodel.cpp` | 五模式边界与特殊区、showCompleted 两终态、历史真实分组、筛选 ID 集不变、层级与全序、Created／历史禁拖 |
| `tests/tst_dropgroups.cpp` | 日期／无日期／Priority／pin／完整子树／AsChild、源与锚点验证、CAS 与最新快照、白名单与事件上下文 |
| `tests/tst_activitytimeline.cpp` | 摘要无技术噪声、创建标题、顺序、实体隔离、关闭、eventsChanged 与隐藏 dirty 刷新 |
| `tests/tst_datepopup.cpp` | 子卡片尺寸与锚点、先缩列表再翻转、自然高度、键盘焦点、真实选项、取消／父确认才写入 |
| `tests/tst_detail.cpp`、`tests/tst_attachmentcards.cpp` | 实际文字原点／图标墨迹、仅真实控件、短／空正文可编辑填满、独立底部附件预算／滚动、拖放与文件操作失败准入、菜单键盘、分组偏好、持焦点销毁和删除后关闭 |
| `tests/tst_editorlinks.cpp` | 双中括号解析／字面别名／无别名标题、裸 UUID href、旧 scheme 不识别、反向引用、源码与渲染映射 |
| `tests/tst_editortools.cpp` | 真实32／36px格式岛、24px选区按钮及4px上下边距、主题与共享菜单、动作顺序、源码选区保持、代际失效、窄宽度与滚动裁剪 |
| `tests/tst_editorinteraction.cpp` | 真实 gutter／加号／块菜单、Markdown-only checkbox、补全包含匹配／IME／Escape、链接悬浮复制和双字段编辑、上下文竞态、剪贴板源码往返与一次撤销；文件及目标均已补齐 |
| `tests/tst_brandicons.cpp` | 第一列透明原Logo／固定白底软件图标、多尺寸／DPR／主题、坏图回退、重载及源文件不改写 |
| `tests/tst_relateddetail.cpp` | 正文真实点击、单卡片复用、源搜索／撤销、外点不穿透与子交互Esc、字段冲突／保存失败／确认重载、便签正文路由、恢复与退出／Store销毁 |
| `tests/tst_tasks.cpp`、`tests/tst_ui.cpp` 及既有更新／统计／品牌套件 | 业务保留、字体与详情 origin／菜单／拖入准入、时间线编辑会话、更新退出与其他集成回归 |

沿用既有构建目录及隔离合成数据，不接触真实用户数据。集成方应按现有流程构建并运行完整 `ctest --test-dir <build> --output-on-failure`；窗口与浮层相关验证必须区分 offscreen 与原生 Windows，不能拿 offscreen 判定合成与手势已验。本轮按用户要求不生成人工截图。

**该历史轮次的验证记录**：schema9、当时备份5及当时编辑器／附件布局的构建、完整 CTest、相关原生 Windows 套件与警告保留在 updates.md 对应的2026-11-16历史条目；不把历史计数作为当前schema10的结果。当前原生视觉、实际手势与银河麒麟／飞腾 ARM64 真机仍由用户验收。

历史追溯：前序 2026-11-16 数据收敛轮次曾记录 Windows Qt5.12.8／C++14 完整 CTest 11/11套通过，原生 ui／detail／activitytimeline／datepopup 四套0失败、0跳过；那是 schema8／旧兼容入口的对应结果，不是本轮 schema9 的结果，原始明细保留在 [updates.md 历史记录](updates.md#已经验证到什么程度)。

---

## 历史实施指引：最初 v4→v5（以下全部为追溯材料）

> 下文完整保留最初 Task→Entity、本地优先、日志与镜像备份等设计背景。**其版本号、旧库／旧备份处理、独立检查项、旧内链、双视图、置顶／分区日志及旧日志界面等要求均已被文首当前契约取代，不得照此重新实现或作为当前验收清单。** 当前只接受空库或标识正确的 schema9，旧 v5–v8 也在写入前拒绝；不存在下文迁移流程或旧 `.knb` 兼容入口。历史测试命令和用例名称只记录当时覆盖意图，不表示当前集成 CTest 已通过。

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
