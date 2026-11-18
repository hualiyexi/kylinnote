# UI 对齐契约：逐区域追踪矩阵

本文件记录麒麟清单各区域的 UI 契约与历史对齐过程。**当前行为以文首2026-11-18当前规格区为准；2026-11-16、2026-11-15 及更早记录均为历史追溯**。旧记录中的数据库版本、行高、任务专属提醒、单日期／原始RRULE、设置布局、检查项、日志对话框、附件邻接／单一内容滚动、双视图／拖放限制、菜单宽度与日期整页尺寸遇冲突时由当前契约取代，不能当作当前验收结果。参照资料仅留本机，本轮不引入竞品 DOM、截图、原始 SVG、账号信息或调研材料，既有 `docs/research/` 保留。

## 2026-11-18 当前规格：桌面便签、即时编辑与四列细节

本区“桌面便签”“便签吸附与排布”“同任务实时编辑”及正文即时格式条目于2026-11-18追加，取代下方活动行源码、编辑／预览切换及每窗口独立文字草稿的历史约定；其余既定规格继续有效。

| 区域 | 当前规格 | 实现入口 |
|---|---|---|
| 数据准入 | schema10只新建当前库，旧预览库不迁移但启动也不删除；当前备份v6完整保存范围、重复规格／进度和提醒规则，无v5导入转换 | `Database`、`TaskRepository`、`ScheduleValue`、`BackupCodec` |
| 四列几何与亮色 | Logo34px、四列首行中线48px、智能清单35px；第一列`#F9F9F9`，其余三列`#FFFFFF`；第三列前置图标绘制下移1px，分隔线选中／悬浮均保留并与背景右端对齐 | `WorkspaceGeometry`、`DesignTokens`、`TaskDelegate` |
| 第三列状态与导航 | 状态顺序清单→闹钟→附件→笔记，使用快照索引且窄行按预算省略；笔记不显示日期文字／提示。数量独立至少24px栏，色点8px、gap4px；提醒／附件／尾部笔记均为14×14px并与前置复选框和清单图标共用y=21中线，尾部笔记使用专用`item-note`语义映射；归档组按普通文件夹展示左箭头；“已归档的清单”使用独立、不可选中的分区行，不复用现有清单行 | `TaskListModel`、`TaskDelegate`、`SidebarSectionHeader`、`AppIcons` |
| 清单目标菜单 | 第四列与MoveTo共用透明搜索框＋独立分隔线，显式18px搜索图标与清单行图标同x轴、输入框18px高／图文gap8px，普通行34px；根层文件夹／平铺清单、搜索路径与UUID目标不变。GroupChip上方可见卡片间距8px，不把透明阴影或QMenu二次偏移算入；GroupChip及菜单行按当前实际文字墨迹对齐18px图标，误差约1逻辑px，搜索原生QLineEdit行为不改；重建按所有权销毁动作并先脱离旧子菜单对象树 | `ListDestinationMenu`、`DetailPopups`、`EntityCommands`、`PopupMenu`、`TextGeometry` |
| 设置与焦点 | 设置212px左栏／正文，分类列从卡片x=0、页面从x=212开始，不重复叠加左偏移；新增「通用」分类及「开机自启动」30×18开关，正式安装版默认开启、手动关闭后不复活；显式开发预览可手动开启独立的预览启动项，登录命令保留数据目录；自检与普通独立目录运行不登记自启。标题栏横跨整张卡片，关闭按钮位于右上角而非左栏；便签更多→设置→应用设置在菜单关闭后显示同一对话框。简单单值选择器统一`MenuComboBox`（含显示设置），普通30px与日期紧凑24px保留各自高度，三种字体下当前值完整不裁字；菜单34px行／12px圆角／上下4px，保留模型、数据、信号与键盘。应用QSS、局部QSS、自绘和Fusion共同消除蓝色焦点框 | `SettingsDialog`、`AutostartSettings`、`CompactSwitch`、`MenuComboBox`、`DisplaySettingsDialog`、`SmartListSettingsWidget`、`WorkspaceStyle`、`QssBuilder` |
| 托盘菜单与小部件 | 托盘使用应用内圆角菜单，入口为任务小部件、主窗口置顶和退出。设置页也可打开任务小部件。任务行复用第三列组件，展示范围、位置、尺寸和选项保存于本机，启动时恢复打开的小部件 | `TrayController`、`TaskWidget`、`TaskListModel`、`KylinListApp` |
| 登录与桌面窗口恢复 | Windows当前用户Run项／麒麟桌面自启文件以`--background`拉起（开发预览另带当前`--data-dir`）；关闭系统会话时只保存草稿和桌面投影，禁止Qt逐个关闭便签并写成open=false。冷启动恢复open=true的有效便签及几何、正文，主动关闭的不恢复；托盘可用时不主动弹主窗口 | `KylinListApp`、`Autostart`、`NoteManager`、`main.cpp` |
| 离线字体 | 默认／Noto Sans CJK SC／霞鹜文楷，官方静态常规／粗体与OFL随包；稳定ID、实际注册族名、统一通知；正文保留选区和撤销，代码／Emoji独立 | `FontManager`、`TypeSystem`、`MarkdownEditor`、字体资源 |
| 第四列与转换 | 笔记保留信息；任务同位置为四色优先级旗子和164px竖向当前勾选菜单。顶部日期与底部清单按钮均按当前实际文字墨迹对齐图标，无上移1px或逐图标补偿；未过期稳定蓝／过期稳定红。有提醒时不再在详情头部额外显示独立闹钟按钮，日期芯片继续统一承接日期、时间与提醒编辑，第三列任务行提醒状态与系统调度不变。转换无二次确认，成功显示底部“已转换” | `TaskDetailWidget`、`EntityCommands`、`ToastIsland`、`TextGeometry` |
| 完整时间段 | 日期／时间段切换，新时刻范围默认晚1小时；同日、跨夜、跨天，非法端点阻止保存，全天结束日包含当天。普通卡片260×539并内嵌月历；时间段260×315、默认无月历，开始／结束日期与时间为28px控件，两个时刻框均为95×28px、`ds::Surface`底与1px`ds::Border`边框／8px圆角；“全天”为30×18px开关并保留时刻草稿。单日时间与范围端点时刻均提供48个半小时选项及精确分钟输入；用户文本接受`H:m`、`H:mm`、`HH:m`、`HH:mm`，小时与分钟均可省略前导零，成功后统一显示`HH:mm`；时间列表在父卡片内展开，单日清空后回车可移除时刻。开始／结束日期月历插入对应行下并推动后续布局，不再使用独立月历弹层；Escape只收当前展开并归还焦点，父卡片随内容自然增高并重新夹入屏幕；超过可用高度时主体出现4px滚动条，底部清除／确定保持可见。四个快捷按钮及图标均26×26px，首个和末尾按钮距卡片左右各22px；未选中的今天使用`ds::PrimarySoft`淡圆底与`ds::Primary`文字，选中由实心Primary覆盖。浅色父卡片不额外画1px边框，深色保留语义边框；底部清除／确定等宽30px高／8px圆角。时间／提醒／重复入口均为252×34px、8px悬浮圆角，在260px卡片内左右各留4px；有值时直接显示值，不添加“时间 ·／重复 ·”前缀。父卡片14px圆角，子卡片252px宽／12px圆角／34px菜单行／24px页脚按钮，共享18px高斯阴影、下偏5px与24px透明边距，不使用graphics effect或mask | `DatePickerPopup`、`DateCalendarPanel`、`AnchoredListPopup`、`PopupShadow`、`ScheduleValue`、`EntityCodec` |
| 正常提醒 | 多提醒保留UUID与版本化相对锚点／偏移；定时预设为准时、提前5／15／30分钟、1小时、1天，范围另有结束时；全天为当天／提前1／2／3天／1周，默认09:00用次要文字。提醒根页为34px菜单行，整行点击多选，选中项为Primary文字与右侧18px对勾，不使用左侧复选框；自定义切换为同尺寸独立编辑页，定时编辑提前量／单位，全天编辑按天／按周提前与本地时刻，并显示规则摘要和实际触发时间；本地时刻与具体时间共用单双数字输入解析，规范化存储仍为`HH:mm`。已有绝对提醒（含旧空ruleJson）继续显示、保留和移除，触发时刻照常校验。父入口按稳定顺序显示真实摘要，全天默认09:00省略，绝对提醒显示本地日期／时刻，长文本省略，无效规则保留“需重选”。提醒确定只提交父草稿，取消、Escape、外关和父层关闭恢复含UUID的打开前快照；自定义取消同样回滚整次编辑。最近自定义规则按本地设置去重保存，父卡片最终accept时才按实际保留规则记录，任何取消均不污染，且不与预设重复。定时与全天各自保留草稿，最终只提交当前模式有效规则；仅父卡片确定才通过控制器写库。任务与笔记都通知，笔记无完成；按本轮确认范围，不提供持续提醒空开关或业务模型 | `DatePickerPopup`、`DatePickerReminderEditor`、`DatePickerControls`、`ReminderRule`、`ReminderScheduler`、`TaskStore`、`Settings` |
| 系统提醒弹窗 | 主提醒窗顶部时间行可拖动，双击不最大化，关闭、完成、打开与稍后提醒按钮保持独立命中。稍后提醒二级菜单复用`PopupMenu`：透明顶层窗口、12px圆角、共享`PopupShadow`高斯阴影与透明外边距，圆角外不得残留矩形背板；菜单按可见卡片尺寸决定下方／上方放置，与按钮保持4px间距，不把透明阴影边距算入间距；四个动作仍提交准确提醒UUID和15／30／60／180分钟，不使用`setMask` | `ReminderPopup`、`PopupMenu`、`PopupShadow`、`WindowChrome` |
| 正文即时格式 | 使用常驻富文本编辑文档，当前段落不展开为源码。行首`# `至`###### `即刻转换为对应标题，空标题的插入符与随后键入文字立即继承标题字号、字重和行高；粗斜体、删除线、下划线、高亮、行内代码在完成标记或格式操作后当行生效。光标进出、选区、失焦／聚焦不改变块格式、文档高度或下方内容位置，也不添加撤销步骤。标题回车后为正文；列表回车续项、有序递增、检查项续出未完成状态，空项回车退出，块首退格还原正文 | `MarkdownDocument`、`MarkdownEditor`、`EditorTextEdit` |
| 正文检查项与写回 | 检查框为独立绘制与命中区域，不把☐／☑字符放入可编辑文字。按下／释放在检查框内才切换完整`- [ ]`／`- [x]`状态，保留原光标、正反选区与滚动位置；点文字继续原生编辑，只读不改状态。Markdown仍是正文保存格式，块／行内语义与源映射放在可撤销Qt格式属性中；未改块保留原文，编辑块按语义写回，检查项、格式、粘贴、补全均沿用单一撤销历史。IME预编辑不保存、不重投影；检查框、圆点、序号与引用条固定锚定段落首行布局原点，不随空项的预输入长度或候选光标移动；首字提交与取消保持同一锚点，滚动后仍按文档到viewport坐标定位。提交后再局部转换。详情、关联窗口及便签编辑共用同一编辑器与附件卡片，不新增独立检查项实体或数据表 | `MarkdownDocument`、`MarkdownEditor`、`EditorTextEdit`、`TextDraft` |
| 桌面便签 | 原生客户端参考；复用`TaskDetailWidget`紧凑呈现，标题和即时Markdown正文直接编辑，不显示任务完成框、编辑按钮或预览切换。可见纸面7px圆角、左右14px内容留白、20px顶部拖动区、26px底栏；顶部外观／置顶／关闭为18px命中与12px图标，底部20px更多按钮仅悬浮显现，保留布局占位。左下日期始终可见，打开共享日期／提醒／重复选择器；更多仅含新建、完成、排列、打开主窗口、设置和删除。关闭只移除桌面投影，删除进回收站，完成改变任务状态。六色纸面、文字和边框全部取`ds::`；24px共享高斯阴影与内容圆角分开，边框overlay最后绘制，不用mask或graphics effect；阴影小模板九宫格缓存，连续缩放不重算整窗卷积。附件复用第四列同一56px卡片与操作，紧凑布局省略附件标题行、使用14px左右留白与独立滚动 | `NoteWidget`、`TitleBar`、`TaskDetailWidget`、`ColorTheme`、`DesignTokens`、`PopupShadow` |
| 便签吸附与排布 | 拖动按原始指针位移计算，10px捕获区内吸附当前屏幕工作区四边，或邻近可见便签的对齐边／邻接边；支持0与8px间距，远处不相关窗口不吸引，可自然拖出。只计算可见纸面，透明阴影不计间距，不移动其他便签。更多支持横向／纵向排列与换行，四边／四角均可缩放，最小纸面220×180；恢复几何同样按纸面夹入屏幕，吸附后重启不多出阴影宽度 | `NoteSnap`、`NoteManager`、`TitleBar`、`WindowResizeArea`、`ScreenFit` |
| 同任务实时编辑 | 主详情、关联窗口和便签共享同一个`EntityTextSession`，每次真实编辑完成立即广播原始标题／Markdown正文，不等待450ms合并保存。各视图保留独立富文本布局、宽度、光标、正反选区和滚动；链接标题的局部选区不得扩成整链。标题与正文分别共享源撤销／重做，快捷键和右键菜单均路由同一历史，投影刷新不成为修改或另存一次。IME预编辑仅本地可见，已提交字即使下一段预编辑仍活动也同步；组合期间的远端修改延后投影，提交／取消后重定位，重叠冲突保留可复制草稿。持久化复用`TextDraft`字段校验与事务，不覆盖其他元数据；空标题仍可同步草稿、保存有效正文，但阻止关闭；SQL／日志失败、删除后未保存内容及同步重入都保留草稿。明确丢弃才清空，回收站任务恢复后重新绑定可编辑会话；日志只记录真实写入者，不重复归因观察窗口 | `EntityTextSession`、`TaskStore`、`Journal`、`TaskDetailWidget`、`MarkdownEditor`、`EditorTextEdit` |
| 正文关联独立窗口 | 正文关联打开无QWidget父对象的独立`Qt::Window`，不再是工作区内透明cover。保留610×500详情内容、16px圆角和24px共享阴影边距，新增28px标题条与右上角最小化／最大化／关闭；顶部拖动区保持28px实际高度并通过原生真实指针命中，支持拖动、双击最大化，四边和四角均可调整尺寸。最大化取消圆角、阴影边距和缩放手柄，还原恢复；主窗口缩放不重置其位置或大小，主窗口Escape、快速添加和普通导航不误关，点击窗口外不自动关闭。每工作区仍仅一个实例，同目标和连续内链复用；便签正文沿用同一路由 | `RelatedDetailPopup`、`TaskPanel`、`TaskDetailWidget`、`WindowChrome`、`PopupShadow` |
| 关联窗口提交与生命周期 | 标题栏关闭、Escape和切换目标继续先`finishEditing`，保存失败保留窗口与草稿；Ctrl+S、IME／日期／格式／补全优先Escape、字段级合并与冲突0写不变。恢复冻结同时作用于主窗口与独立关联窗口，恢复成功清理旧上下文；主面板明确拥有并销毁无父窗口，应用退出保存准入不绕过关联草稿 | `RelatedDetailPopup`、`TaskPanel`、`TextDraft`、`TaskStore`、`KylinListApp` |
| 完整重复 | 预设菜单按锚点显示每天、每周（星期）、每月（日期）、每年（月日）、工作日与自定义；单击预设只改父草稿。自定义在同一252px卡片中切换：顶部为按到期日期／按完成日期／按自选日期，第二行为“每＋间隔＋天／周／月／年”，同时保留农历月／年及闰月重复；按完成日期仅显示日／周／月间隔并禁用不适用频率，不附带固定星期或月日。按到期日期随后按频率显示22px星期圆钮、每月日期网格／序数星期／普通工作日或每年设置；取消恢复进入前规则，确定通过现有校验后提交父草稿。重复生效后父卡片出现“结束重复”行，支持一直重复、按日期结束、按次数结束；结束日期在行下内嵌月历，次数使用行内编辑。自选日期使用`DateCalendarPanel`多选并排序去重，不再输入逗号文本。小屏内容按可用高度滚动并重新锚定。已有规则未编辑直接确定时保留完整规格；农历、年度月末和周起点不会因打开新表单而被改写，月末与多个具体日期组合保持有序；不加入法定节假日、调休、艾宾浩斯或无行为跳过开关 | `DatePickerPopup`、`DatePickerRecurrenceEditor`、`DateCalendarPanel`、`RecurrenceRule`、`RecurrenceEngine`、`LunarCalendar`、`TaskStore` |
| 保存与视图 | 父弹窗确定后一笔事务写计划／规则／提醒；调度基线冲突保护。范围相交决定智能范围可见，结束日决定逾期与互斥Time展示桶，开始日排序；跨日拖放／顺延／重复保留范围与相对提醒 | `TaskDateController`、`TaskStore`、`ViewSpec`、`TaskGrouping` |

本轮网页实证直接读取当前日期卡片的DOM、computed style与实际点击行为：快捷SVG为26px，今天的背景节点为Primary 10%圆形淡底；时间段时刻框为95×28px，开始日期月历内嵌在开始／结束行之间；提醒菜单为252px卡片、34px行、13px文字，选中项为Primary文字和右侧18px对勾，自定义提醒使用独立编辑卡片；重复包含按到期／完成／自选日期的二级菜单、按频率变化的工作区，以及父卡片上的结束重复与内嵌结束月历。全天最近使用的非默认时刻直接排在预设前。当前网页页脚为“取消／确定”，与旧截图“保存／取消”不同，按当前网页版本实现。持续提醒、法定节假日／调休与艾宾浩斯经用户确认不在本轮新增空控件或业务模型。调研结束后重新加载并核对原任务日期摘要，未保存任何调研草稿；截图、账号数据与调研材料未写入仓库。

以下 2026-11-16 及更早内容仅作历史追溯。

## 2026-11-16 历史规格：数据收敛、分组与详情

数据语义见[数据模型](data-model.md)，实施边界见[重构指引](data-model-refactor.md)，窗口与控件设计见[UI 设计](ui-design.md)。下表追踪当前实现；构建、CTest 与原生平台的实际结果分别记录在[当前验证条目](updates.md#当前实现与验证状态)，不以历史通过数替代。原生视觉、实际手势与目标机仍由用户验收。

| 区域 | 当前规格 | 实现入口 |
|---|---|---|
| schema 准入 | schema9，仅完全空库直接创建／标识正确的当前库可用；已有文件只读预检，v5–v8／错误标识／未知及未来版本在写入、WAL切换、建表前拒绝。无升级、补列、正文转换或升级副本，也不回退旧产品库 | `Database`、`TaskRepository` |
| 当前内容模型与备份 | 无独立检查项领域类型／表／NoteChecklist；正文复选框只改 Markdown。备份仅读写当前元数据5，根无 checklistItems、实体无 checklist；夹带旧表示、v2/v3/v4和未来版本在动库／附件前拒绝，无旧codec或双表示转换 | `Entity`、`EntityCodec`、`BackupCodec`、`TaskDetailWidget`、`NoteWidget` |
| 活动持久化与策略 | EntityEvent.context 为 JSON 对象，SQL context_json、备份 context；主任务标题／清单名／附件名保存当时快照。ActivityPolicy 写入与展示共用白名单，只记实质字段／生命周期，排序／pin／外观等不记。日志单独提交也发 eventsChanged，当前实体即时刷，隐藏 dirty 再补刷 | `ActivityPolicy`、`Journal`、`TaskStore`、`TaskRepository`、`ActivityTimelineWidget` |
| 第四列任务动态 | 同 DetailScroll 的正文下部可关闭时间线，附件另置底部独立滚动区；最新在前，20px 标记／14px 摘要／12px 时间与20px行高／32px 左缩／20px 条目间距／1px 连接线，标题上18下8、close18；创建摘要带标题，父级称“主任务”，无账号不伪造 Actor，无名称上下文用通用摘要、不露 UUID／批次／全文；不弹独立日志对话框 | `ActivityTimelineWidget`、`TaskDetailWidget`、`EntityCommands` |
| 五 GroupMode | List／Time／Created／Priority／None 与范围和搜索正交，切换不改筛选 ID 集；文件夹默认 List，其余 Time，单清单／收集箱不提供 List；本机 QSettings 按 scope/id 保存 grouping 字符串，不进业务备份 | `TaskGrouping`、`ViewSpec`、`TaskPanel::syncGroupingPreference()` |
| 特殊区与历史分组 | 活动视角 pinned → 普通组 → notes → completed → abandoned；None 仅普通任务无标题；showCompleted 同时控制两种终态，显示设置复选框和原菜单 quick toggle 共用。Completed／Abandoned／Trash 也能按五模式真正分组，忽略 pin、无活动特殊页脚，全部只读禁拖，可见性不因重分组改变 | `TaskGrouping`、`TaskListModel`、`DisplaySettingsDialog`、`TaskPanel` |
| 桶边界与全序 | Time 六桶 expired（实现 time:overdue）／today／tomorrow／next7days(+2..+6)／later(+7起)／undated；Created 为0..6／7..29／更早／未知（含未来）；活动组内 sortOrder ASC → 自然字段 → id，终态默认对应时间倒序；无“排序／顺序”UI | `TaskGrouping`、`TaskDrop` |
| 类型化拖放 | TaskDrop::Intent 交 TaskStore::applyDropIntent，显式 moveEntityToList 共用完整子树策略；源组／锚点／顺序／清单资格／祖先链复核；Time 取昨天／今天／明天／后天／+7，支持无日期互转，保留时刻／跨度／提醒及旧非连续 rank；Priority 0..3 | `TaskDrop`、`TaskListView`、`TaskListModel`、`TaskStore` |
| 子树与零写入边界 | 跨清单含隐藏／折叠／终态后代，子项独立跨组脱父、离 pin 清 pin；同显示组 Before／After 按锚点父级决定目标同级，脱父后用自身真实属性匹配目标。AsChild 仅 parent/order，不暗改 date/priority/pin，同父同组仅 rank。排序以最新完整兄弟集合稳定插入源项，不从零编号可见子集；非源成员相对顺序保留，可见同级顺序未变零写，实际改 rank 的隐藏成员也参与CAS。Pinned任务／笔记混排遵守rank；非法／循环／CAS失败原子回滚。Created真实起拖在第三列底部WorkspaceToast提示且0写；历史全部禁拖，终态目标拒绝，Notes不隐式转Task | `TaskStore`、`TaskListView`、`TaskPanel`、`ToastIsland` |
| 字体 | Caption9pt=12px，旧8.5pt不用；Windows 优先 Microsoft YaHei UI／YaHei 可缩放中文无衬线，Linux Noto／思源等；PreferAntialias\|PreferOutline；不安装／分发专有字体 | `TypeSystem` |
| 详情内容 origin | body documentMargin0、title 用原生 textMargins.left=-2 抵消 Qt5.12 固定的2px文字内缩，主任务链接／title／body 同左侧文字 origin24；保留28px控制条／52px首行中心／48pxheader及7/5px补偿 | `TaskDetailWidget`、`MarkdownEditor`、`WorkspaceGeometry` |
| 详情顶栏与底栏 | 日历 date-unselected20px glyphbox，checkbox17px与其墨迹同大、30px命中，两者间1×14px竖线；overlay 旧右侧info前竖线仍在。Footer右侧仅A＋more两枚18px图标 td-footer-markdown／more-titlebar，评论删除。左侧清单按钮局部复合绘制：18px图标盒与正文级当前实际文字共用真实墨迹中线、4px图文间隔，保留长名省略与原清单选择入口，不套用导航下沉或日期上移补偿 | `TaskDetailWidget`、`TextGeometry`、`AppIcons`、`QssBuilder` |
| 菜单 | 详情更多统一复用 EntityCommands：164px可见卡片／34px行／18px图标／12px圆角／4px行外margin／12px inset／8px图文gap；实体右键含快捷条仍为既有194px起，不全局套用详情宽度。WorkspaceMenu因当前分组值为202px父卡片，GroupingMenu164px子卡片；尺寸不含阴影 | `EntityCommands`、`PopupMenu`、`TaskPanel` |
| 正文与底部附件 | 空／短正文都由真实QTextEdit viewport填满余白，标题／正文／动态共用DetailScroll；附件位于底栏上方独立面板，左右24px、空时整体隐藏、独立有界滚动。高度同时不超过三卡自然高度与可用高度1/3，并优先为正文保留120px预算；缩放／增删／停靠重挂重算，不靠背景点击转发 | `TaskDetailWidget`、`MarkdownEditor`、`AttachmentWidget` |
| 块 gutter 与正文复选框 | 真 gutter 的加号仅在正文自身聚焦、可编辑、无选区／IME预编辑且光标所在源码普通段落确实为空时显示；失焦、空白字符行、代码围栏／分隔线均隐藏，不跟随鼠标换行。14px加号按实际QTextLayout基线与参考文字墨迹中线定位，22px命中区不抢焦点，任何交互态均无背景。块菜单和格式入口共用块样式API；任务列表只改Markdown并参与撤销 | `EditorTextEdit`、`MarkdownEditor`、`TextGeometry` |
| 格式岛与共享菜单 | 选区32px、底栏36px（常规宽304px）、18px glyph／10px圆角；选区按钮24px与上下实际4px边距，字形约7px上下留白。选区提供文本类型＋七个主格式＋更多；更多是纵向PopupMenu，不生成第二条横岛；色板／更多／类型均34px行／4px行外缩进／12px圆角。附件、链接、关联任务/笔记、引用、代码、标准分隔、清除格式；窄宽折叠进同一菜单。共享PopupShadow，不用graphics effect／mask | `FormatToolbar`、`PopupMenu`、`PopupShadow` |
| 真入口与上下文安全 | 无隐藏／屏幕外EditorToolbar、format0..替身；真实可见按钮／gutter／菜单共用动作API。选区保存绝对UTF-16源码偏移＋contextGeneration，跨FocusOut以sourceSelection／selectSource恢复；自有弹层markdownEditor／ownsPopup保焦点与编辑会话，setContent／setStore／setEntityId后旧菜单／回调不得改新内容 | `MarkdownEditor`、`FormatToolbar`、`EditorPopups` |
| 正文内链与编辑 | 仅双中括号引用：带别名字面保留，无别名显示当前标题；导航href裸UUID，不读旧scheme、不双读／迁移。非模态标题包含补全，IME preedit不误确认、Esc只关候选；链接悬浮复制／编辑，仅“文本”“链接”两字段，确认才写源码 | `Markdown`、`MarkdownEditor`、`EditorPopups`、`DocumentPreview` |
| 功能栏与功能归属 | 第一列仅保留Logo、任务、底部“同步与数据”和“设置”；移除第三列全局搜索框、第一列搜索按钮／Ctrl+F及第三个无用笔记按钮，底层ViewSpec搜索字段留给后续完整搜索。任务、同步、设置均为40×40px命中与28×28px图标，任务使用滴答对应的list-sidebar，激活只变为Primary蓝色、不画背景块。导出／导入备份与打开数据位置归同步菜单；设置含外观三态主题和智能清单页；Logo保留自启、统计、更新与退出 | `TaskPanel`、`SettingsDialog`、`AppIcons`、`QssBuilder` |
| 本机品牌边界 | 第一列首行使用外部Branding提供的透明原Logo，不再用通用任务图标作头像、不加固定白底，保留32px尺寸与本机工作区菜单，并与第二至第四列首行共用相对列顶52px中线（Logo顶边距36px）；窗口／任务栏／托盘共用同Logo的等比白底合成版，固定白底取AppIconBg令牌并提供多尺寸／DPR。未配置时统一默认品牌回退；私有名称、路径、素材不入库，不扩展安装身份或安装菜单图标 | `Branding`、`BrandingIcons`、`TaskPanel`、`TrayController` |
| 正文关联详情 | 当前行为以文首“正文关联独立窗口”为准：无父`Qt::Window`、三按钮、顶部拖动、八向缩放、共享`PopupShadow`，不再使用工作区内透明cover或禁止标题栏的旧模型。每工作区仅一个实例，同目标不重置，继续点关联复用当前窗口；源筛选、选中、滚动、源码与撤销保留。便签正文同路由，主任务／创建结果／便签标题栏导航保持原语义 | `RelatedDetailPopup`、`TaskDetailWidget`、`TaskPanel`、`NoteWidget`、`NoteManager` |
| 详情宿主与共享提交 | 关闭／换目标先finishEditing，失败保留窗口和草稿；Esc先给IME与日期／格式／补全等子交互，独立关联窗口不再有点击外部即关闭或Tab进入下层工作区的cover语义。详情、关联窗口和便签共用TextDraft字段三方比较，不同字段合并、同字段冲突整体0写、不恢复已删除实体；可复制草稿、确认丢弃重载、Ctrl+S重试。投影管理器只存桌面字段。纳入退出准入／恢复冻结；失败解冻保留上下文，成功移除旧上下文后重建 | `TextDraft`、`TaskStore`、`Journal`、`TaskDetailWidget`、`NoteWidget`、`RelatedDetailPopup`、`KylinListApp` |
| 窗口按钮与边框 | 三个窗口按钮保持46×28px完整命中，普通／深色主题沿用既有灰色激活色；激活背景在四边内缩1px，关闭按钮仅普通窗口保留右上角圆弧，最大化不画假圆角。WindowControls先raise、透明WindowBorderOverlay最后raise，激活底色不能吞掉顶部或右侧边框 | `WindowChrome`、`TaskPanel::raiseWindowChrome()`、`WindowBorderOverlay` |
| 附件 dragCopy | 本地单／多文件覆盖整个可写详情及子控件；先 finishEditing，失败0导入；与原选择器共用2MiB上限和逐文件成功语义，遇错停止、保留已成功项；目录不递归、原文件不动、正文不插URL | `TaskDetailWidget::handleFileDrop()`、`AttachmentWidget::importPaths()`、`TaskStore` |
| 日期子层 | AnchoredListPopup可见卡片252px宽／34px行／12px圆角／上下4px内容边距；共享核半径18、下偏5、峰值alpha0.20的高斯阴影与24px透明边距，阴影支持范围不裁底。时间总高≤280px，输入固定、列表用剩余预算；提醒／重复自然限高；入口下4px，不足先缩列表再翻上，兼顾父月历与可用屏幕；不再固定540整页替换 | `DatePickerPopup`、`AnchoredListPopup`、`PopupShadow` |
| 日期键盘与取消 | 模式无focus边框但StrongFocus／autoDefault=false；仅daily／weekly／monthly／custom重复，移除持续提醒／yearly／workdays假选项；子取消只关子并返焦，父取消0写，只有父确认保存草稿 | `DatePickerPopup`、`TaskDateController`、`AnchoredListPopup` |

### 追加对齐：顶部标题、附件卡片与空正文

- 第三列顶部 `ListHeading` 与第四列 `TaskTitle` 共用 `DetailTitle`（14pt／Bold），所有scope的顶部图标24×24px；不扩大普通正文或分组标题；第三列顶部不再创建实体总数控件，保留侧栏和分组计数。40px标题行、28px控制条与52px首行中心不变，自定义Emoji按绘制框正常缩放。
- 第四列底部附件为56px高、14px圆角、6px间距的类型卡片：26px文件图标、14px等效文件名／12px大小两行，分别采用字体自然高度、1px间距，整块垂直居中且上下留白差不超过1px；18px更多图标和30px命中区。背景只取`ds::`，文件名按纯文本省略显示、完整名称保留。桌面便签复用同一卡片与权限，当前紧凑几何以上文便签规格为准。
- 整卡打开；省略号／右键共用`PopupMenu`，只列“另存为…”／“删除”，沿用164px卡片、34px菜单行、12px圆角。另存为复用SHA校验与QSaveFile，删除经统一ConfirmDialog、只移除受管副本；模态前后复核存储／实体／附件身份和权限。只读可另存不可删，操作失败走统一ModalDialog纯文本反馈。
- 附件从正文流移出，在底栏上方独立有界滚动，左右24px，空时整区隐藏；面板不超过三张卡片自然高度／可用高度1/3，并给正文保留至少120px预算。空和短正文的真实编辑viewport始终承接余白，悬浮不抢焦点、点击原生聚焦输入；不盖透明点击层、不全局转发背景点击，长文用DetailScroll而附件单独滚动。
- 已有正文另有独立可见性契约：真实TaskPanel列表点击选中后，首帧、稳定帧及FocusIn后均须生成有效QTextLayout行，实际viewport有文字墨迹、完整caret在viewport内，并能移动／键入／保存。用户反馈曾揭示“字段非空、摘要可见但详情正文无墨迹”；已定位并修正Qt5.12段落字符格式变更后局部layout未重新标脏，不能用先前content断言冒充绘制验证。
### 本轮自动化验证

- **自动化验证记录**：构建环境、完整 CTest、相关原生 Windows 套件，以及通过／失败／跳过与警告统一见 [updates.md 当前条目](updates.md#当前实现与验证状态)。已写入断言不自动等同已执行，也不沿用下方历史通过数。
- 测试入口调整为 `database`（取代旧 `migrations`），现有 `editorlinks`、`editortools`、`editorinteraction` 之外再增加 `brandicons`、`relateddetail`。分别覆盖fresh-only拒绝、规范内链与真实格式／gutter／IME／源码撤销，透明品牌入口、四列首行中线与白底图标，以及关联独立窗口真实点击、主窗共存、拖动与八向缩放、源导航／撤销保留、共享冲突、失败恢复、备份与退出准入；导航菜单归属、设置外观三态和窗口按钮内缩像素使用真实入口回归，不使用隐藏／屏幕外格式控件。
- 2026-11-18新增`livetext`、`notesnap`与`desktopnotes`，共23个CTest入口；覆盖即时草稿、共享分组撤销、写入归因、失败与重入保护、便签紧凑样式／日期／菜单／真实热区、双向逐字输入与IME、链接内局部选区、工作区／便签吸附和排布。执行结果以当前`updates.md`条目为准。
- 2026-11-18前序增加`instantmarkdown`，当时与既有套件共20个CTest入口；覆盖当前行即时格式、往返排版、真实检查框／光标／选区／内外滚动保持、IME、回车与退格、跨段编辑、链接标签、代码反引号、格式MIME往返及独立撤销。执行结果以当前`updates.md`条目为准。
- 窗口合成和浮层必须区分 offscreen 与原生平台；未记录的平台或能力不宣称通过。保留隔离合成数据、既有CI／包检查／启动与Release流程，本轮不生成人工验收截图，银河麒麟／飞腾真机由用户验收。

### 2026-11-16 前序轮次自动化验证（历史）

以下保留同日更早轮次的真实结果；其中“本轮”“最终”仅指当时二进制，schema8迁移入口、附件正文流和旧编辑器行为已被当前契约取代，**不计入当前集成验证**。

- 追加修正版验证完成：Qt5.12.8／GCC7.3／C++14完整CTest **12/12套通过（43.31秒）**；原生Windows附件33、详情49、UI112项均0失败／0跳过，新增正文9条data没有能力跳过。此前41.84秒／详情40项属于未覆盖非空正文实际绘制的上一轮；以本节修正版结果为准。

- **Windows x64／Qt5.12.8／GCC7.3／C++14 构建成功，最终 CTest 12/12 套通过**，合计43.31秒；`attachmentcards`及详情新增非空正文真实绘制／点击回归，包含上述标题／卡片／正文修正。各套明细见 [updates.md 的2026-11-16记录](updates.md#已经验证到什么程度)。最终本机日志：`.tools/build-v5-windows/Testing/Temporary/LastTest.log`。现有自动构建、包架构／依赖检查、安装启动及标签 Release 流程保留，本轮未触发发布。
- CTest 沿用 `offscreen`：`ui` **108 passed／0 failed／4 skipped**，`detail` **46／0／3**，`attachmentcards` **33／0／0**。旧有7项明确跳过的边界不变，包括3项原生窗口／浮层前置，以及4项真实 QMessageBox 交互。后者是 Qt5.12 Windows 的 `SC_CLOSE` Show hook 在 offscreen 缺 native interface 时解引用空指针；按精确能力边界拆出，不吞 `Show`、不修改 Qt 库。新增附件对话框使用统一ModalDialog／ConfirmDialog，卡片与空正文的所有新用例两平台都执行，不新增skip。
- 本次追加以 `QT_QPA_PLATFORM=windows` 串行执行最终二进制：`ui` **112 passed／0 failed／0 skipped**，`detail` **49／0／0**，`attachmentcards` **33／0／0**。包含标题字号与图标、28px顶部对齐、附件卡片／菜单／真实另存为和确认对话框、原文件保留及模态竞态、空viewport真实点击聚焦／输入保存／清空重入，非空单行／多行／格式化正文的首帧与稳定绘制、标题／QuickAdd焦点历史和宽窄重挂，以及既有单滚动／附件邻接。前一轮`activitytimeline` 11／0／0和`datepopup` 9／0／0的原生结果保留为对应轮次记录；本次重链后完整CTest再次通过这两套，未重复宣称本轮原生执行。
- 文字对齐同时校验主任务按钮的style文字区、标题IME光标中心、正文首行布局／光标的实际文字原点，并保留墨迹误差≤2px断言；原生和offscreen均通过。日历数字用同属性加宽参考按钮进行内存渲染对比，检验固定30px格子内的真实文字完整性，不再把推荐 `sizeHint` 当成裁剪证据；两平台均通过。本轮没有额外执行多DPI矩阵，不沿用历史150%结果冒充本轮验证。
- `migrations`／`dropgroups`／`groupmodel` 覆盖一致性副本、失败回滚、旧备份转换／冗余拒绝、完整隐藏子树、隐藏同级排序与CAS、同显示组脱父及五模式归档边界；内嵌动态覆盖上下文快照、实质事件过滤和仅事件刷新；日期子层覆盖锚点、限高／滚动、翻转、取消与原生焦点。应用另以临时数据完成原生 `--smoke-test` 并退出0。
- 格式化／撤销测试仍有既有 `QTextCursor::setPosition` 越界警告，相关断言全部通过；不将“0 failed”写成“无警告”。本轮未执行Linux／ARM64构建或银河麒麟／飞腾真机试用。
- **用户亲自验收**原生视觉、实际手势与银河麒麟／飞腾ARM64；本轮未保存人工验收截图。Windows自动化、Ubuntu容器、QEMU以及下方历史截图记录都不代表麒麟V10 SP1或ARM64真机已验。

## 2026-11-15 清单与智能入口收口（历史，后续规格已取代部分内容）

当时的数据契约（现已取代）为 schema v5/v6→v7 原子迁移，文件夹与清单持久化图标，清单另有 `color`／`archived_at_utc_ms`；备份导出 v4，读入兼容 v2/v3 并补默认外观与未归档状态。当前版本与兼容入口以文首 2026-11-16 和[数据模型](data-model.md)为准。

| 区域 | 当时规格（历史） | 实现 |
|---|---|---|
| 第三列实体与分组 | 普通实体行 40px，显示详细时 62px（详情从 34px 起、18px 高），分组头 36px；标题单行。清单色条距实体区左侧 16px、覆盖整行，父任务拖动柄在色条左侧，悬浮背景从色条右侧开始并覆盖整行，绘制仅外扩半像素消除抗锯齿细缝；子任务拖动柄与背景额外缩进 24px，色条不动，背景圆角 10px。右侧清单图标下移 1px，清单与日期为独立点击区域。拖动预览复用同一行布局但不包含清单色条 | `TaskListModel`、`TaskDelegate`、`TaskListView`、`ListAppearance`、`TaskPanel` |
| 智能日期范围 | 今天含全部逾期（有效日期 ≤ 今天）；明天仅明天；最近7天含全部逾期及今天至今天+6；计数与列表共用筛选，活动聚合排除归档清单 | `ViewSpec`、`TaskListModel`、`TaskPanel` |
| 过期顺延 | 仅聚合智能范围的过期分组提供，普通清单及收集箱无顺延入口；每个任务按自身有效日期平移至今天，保留起止时刻、跨日跨度及原提醒，整批事务提交 | `TaskDelegate`、`TaskListModel`、`TaskStore::postponeOverdueTasks()` |
| 日期编辑入口 | 行内日期、详情与快速添加共用 `TaskDateController` 和唯一 `DatePickerPopup`；已有任务提交前读取最新实体，快速添加走草稿入口且确认后才写入新建任务。四个快捷项为今天、明天、今天+7、下个月同日；纯日期只写 `allDayDate`，明确时刻（含 00:00）只写 `dueAt`，时间页可移除时刻而保留日期 | `TaskDateController`、`DatePickerPopup`、`TaskDetailWidget`、`TaskPanel` |
| 侧栏组织 | 智能入口与历史入口不显示分组标题；“清单”标题 hover 显示加号（键盘聚焦亦可见）。区域间为 1px 横线及上下 16px 留白，非导航分隔项使用无效 scope，不能冒充“所有”。归档清单位于可展开组；不提供独立的“已过期”筛选入口，第三列列表内逾期分组与顺延能力保留 | `SidebarSectionHeader`、`NavigationDelegate`、`TaskPanel` |
| 智能入口设置 | 功能栏设置按钮打开独立 `SettingsDialog`；所有、今天、明天、最近7天、收集箱、已完成、已放弃、垃圾桶均支持显示／有实体时显示／隐藏（界面第二项称“有内容时显示”）；本机保存，不随业务备份，隐藏不阻断显式实体导航 | `SettingsDialog`、`SmartListSettingsWidget`、`Settings`、`TaskPanel` |
| 清单归档与删除 | 归档只改清单资格，保留实体状态、日期、提醒与附件；活动聚合、提醒和活动桌面便签排除归档清单，直接清单与历史仍可见。删清单将全部实体移至收集箱并进入垃圾桶，保留 `status`、提醒与原已删时间，恢复后留在收集箱 | `TaskStore`、`TaskRepository`、`NoteManager`、`TaskPanel` |
| 新建／编辑清单 | 表单只有名称、图标、颜色、文件夹；新建或改名时名称 trim 后 1–64 个 UTF-16 码元，历史长名称未改动时完整保留；外观与文件夹随保存一次提交。图标与名称共用完整输入框边界及焦点状态，色块选中环位于按钮绘制范围内 | `ListEditorDialog`、`EmojiPickerPopup`、`ColorPickerPopup`、`MenuComboBox` |
| 模态、菜单与提示 | `ModalDialog` 使用透明窗口、圆角卡片与应用阴影；`PopupMenu` 对整张圆角 alpha 图做高斯模糊。实体菜单可见卡片最小宽 194px，普通行 34px、左右外边距 4px、悬浮圆角 8px；普通图标实际绘制 18px，位置为行内 12px，文字间距 8px；分隔线通栏 1px、上下 4px。日期/优先级分组各 68px，图标分别为 22/20px，悬浮盒四周外扩 6px。`AppToolTip` 统一使用透明圆角气泡：13px 正文、18px 行高、3px 8px 内边距、6px 圆角和深灰底白字；完整可见的同名文字不重复提示，省略文本、完整日期及操作说明保留 | `ModalDialog`、`PopupMenu`、`AppToolTip`、`EntityHoverPreview`、`EntityCommands`、`QssBuilder`、`TypeSystem`、`DesignTokens` |
| 导航图标 | 使用内置资源的准确 ID；今天按应用时区日号选择 `today-icon-1..31`，最近7天、已完成、已过期、垃圾桶及归档组分别使用对应导航图形。按 viewBox 等比绘制，不再附加统一光学内缩；普通导航图形为 20×20 | `AppIcons`、`ListAppearance`、`NavigationDelegate`、`SidebarSectionHeader` |
| 自定义清单/文件夹图标 | 默认 SVG 与 emoji 共用 `ListAppearance`/`GlyphButton`；显式恢复文本画笔并复用 emoji 字体回退，按绘制框度量留出血，选中不消失、14–20px 小图标不裁切 | `TypeSystem`、`ListAppearance`、`GlyphButton`、`FolderEditorDialog` |
| 清单拖动与文件夹 | 普通未归档清单可拖动：上下边缘排序，平铺清单中心合并，文件夹内中心移动；拖动中显示插入线或圆角包围框。组织区末尾和可见空白可移到顶层末尾。合并先弹名称/图标编辑器，确认后原子建夹，取消不改组织归属、顺序或 revision；正文失焦仍按原编辑会话保存。不创建嵌套文件夹。文件夹可选择、展开/收起，置顶跨清单汇总，其余按清单分节 | `NavigationListWidget`、`NavigationDelegate`、`FolderEditorDialog`、`TaskStore`、`TaskPanel` |
| 任务层级与手动顺序 | 父任务后连续展示子任务，支持折叠；跨分区归组后每个实体仅出现一次，计数不被折叠改变。拖动携带可见子树，同父级内排序保留各自实际置顶状态；下半区左侧为普通后移，达到 `36 + (目标深度 + 1) × 24` 横向阈值的右下半区设为子任务，落点线同步多缩进一级 24px，成功后展开父项。也可通过“关联主任务”设父。纯设父不暗改日期/置顶，源后代保持关系；Store 校验同清单、进行中任务及完整祖先链，关系和顺序原子提交，非法目标零写入。段内先 `sortOrder`，同值保留原日期/创建时间/置顶回退；进入/离开置顶区同步切换置顶状态。文件夹任务只能拖回自己的清单，禁止跨清单误投放 | `TaskListModel`、`TaskListView`、`TaskDelegate`、`TaskStore::moveEntityForDrop()` |
| 时间区拖动 | 使用 `overdue/today/tomorrow/next7days` 稳定键；跨区分别落到昨天、今天、明天、后天，保留时刻/跨度/提醒，同段不改日期。子任务脱离父级按真实日期所属区判断，不沿父级显示日期误判。空置顶区及缺失的空时间分区仅在拖动时追加到归组后的列表底部，不移动原行；结束、取消或离开后移除 | `TaskDrop`、`TaskListModel`、`TaskListView`、`TaskStore` |
| 快速添加与任务右键 | 快速添加为 10px 圆角、透明边框，浅色底使用 `QuickAddBg=#F7F7F7`，placeholder 包含实际目标清单；日期按钮仅在输入行内聚焦或日期弹层打开时显示。文件夹选择成员清单，空文件夹禁用。右键复用日期、优先级、添加子任务、关联主任务、置顶、移动到、副本、转换与删除等已实现命令，保留完成状态和桌面便签，不展示标签/复制链接/未定义命令；添加子任务、关联主任务、取消置顶、移动到与副本使用各自语义 SVG，普通动作和桌面便签统一为 18px | `QssBuilder`、`PopupMenu`、`AppIcons`、`TaskPanel`、`EntityCommands`、`TaskStore::createSubtask()` |

本轮不新增摘要、指派、标签或过滤器等范围外功能；早期参照矩阵中的范围外项目不构成本轮交付项。

### 2026-11-15 已执行验证与边界（历史，不计入本轮）

- 原生 Windows x64、Qt 5.12.8：主 UI 套件 **90 passed，0 failed**；核心 `tasks` **65**、`updates` **40**、`stats` **22**、`branding` **35 passed**。新增覆盖文件夹聚合/建夹、顶层末尾落点、跨分区子树唯一性、首次/末尾置顶投放、同父排序保留置顶、真实日期分区改期、父子 revision 同事务合并及跨清单拒绝。CTest 使用既有 `offscreen` 环境，**5/5 suites passed**；其中原生窗口两项及原生浮层视觉一项明确跳过，不用 offscreen 冒充窗口验收。
- 本次实体菜单与拖放修正：新增纯色探针验证普通图标实际为 18px、行内起点 12px、图标文字间距 8px，并以像素验证通栏分隔线；覆盖预览仅移除色条、右下设父与缩进落点、折叠父项自动展开、持久化及非法祖先/跨清单零写入。Windows Qt 5.12.8 完整 CTest **5/5 suites passed**。按用户要求本轮不生成人工验收截图，原生视觉与手势由用户验收。
- 150% 缩放的 `organizationDragVisualPreview` + `visualPreview` 实测 DPR **1.5**，合计 **4 项通过**。窗口高度被当前可用屏幕夹限，该结果不表示可无限制显示请求的完整逻辑高度。
- 已检查原生深浅主题截图、文件夹合并框、时间空投放区及父子行 2 倍局部图；局部图使用完整面板实际底色合成，裁剪与放大统一为物理像素，避免透明背景吞字或高 DPR 二次缩放。产物仅留本机忽略目录 `.tools/ui-screenshots-round4/` 与 `.tools/ui-screenshots-round4-150/`。
- 本轮未跑 Linux、ARM64 或银河麒麟 V10 SP1 真机；下方 Ubuntu 等验证结果仅对应各自历史轮次。

## 历史证据等级

沿用考据册分类（`ticktick-windows-6.3.3`、`ticktick-webapp` 两册的结论性章节）：

- **①** Web 现场实测截图/DOM
- **②** 官方旧图
- **③** BAML 静态资源定义
- **④** 发布 JS/CSS 代码证据
- **⑤** 设计建议/文字推断

## 历史参照边界

1. Web 册 16 张图是 2026-11-07、1500×940、Chromium 152、**当前账号自选粉色渐变主题**下的现场脱敏截图；顶部 36px 图注与灰块是研究加工，不是产品 UI。粉色 `#FF79A4→#FFAFA6` 是采样值，不是默认规范。
2. Windows 6.3.3 册**本机实测截图 0 张、右键菜单展开 0/21**（`README.md:5-9`、`09-live-capture-checklist.md:14-20`）；6 张配图全是官方旧图/本项目示例图/自制色板。只能提供静态资源定义与 Qt 转译建议。
3. 深色主题**无任何具体色值证据**。深色值由 KylinList 自定，明确标注「非滴答实测」。

## 首轮区域矩阵（历史目标）

| 区域 | 考据来源 | 证据等级 | 目标规格 | 实施文件 | 验证 |
|---|---|---|---|---|---|
| R1 设计系统 | `07:86-92`、`01:62-75`、`windows/02:28-57`、`06:68`、`code-evidence.md:67-81` | ①③④⑤ | 语义令牌 + 主题切换（浅/深/跟随系统）+ 字号字体系统 + SVG 图标 + 零硬编码门禁 | `src/ui/design/*`、`src/ui/AppIcons.*`、`src/ui/AppStyle.*` | CTest + 门禁脚本 |
| R2 三栏几何 | `01:9-30,82`、`01:75`、`01:109-119`、`code-evidence.md:46-51,96-110`、`g01`、`g02` | ①④ | 50px 功能条/249px 组织栏/64px 列表头/40px 任务行/36px 导航行/48px 详情底栏；圆角分层 14/12/10/8 | `TaskPanel.cpp` | 截图对照 g01 |
| R3 左侧导航 | `02:16-80`、`04:49-63`、`m01/m02/m06`、`windows/03` | ①③ | 单层文件夹+可平铺清单+清单 kind+唯一收集箱；侧栏对象右键按状态生成 | `TaskPanel.cpp` | 截图对照 m01/m02 |
| R4 搜索/工具栏/快速添加 | `02:96-141`、`n01`、`04:131-142`、`m04`、`component-code-evidence.md:24-34` | ①④ | 搜索面板/视图 overflow 菜单/固定快速添加 | `TaskPanel.cpp` | 截图对照 m04/n01 |
| R5 任务列表/行内 | `01:27-45`、`03:23-43`、`component-code-evidence.md:16-20`、`04:109-116` | ①④ | MetaRole 拆分/SVG 图标/四态分离/hover 更多按钮/完成框命中区 | `TaskListModel.*`、`TaskDelegate.*` | 截图对照 g01 |
| R6 右键/二级菜单 | `04-menus.md` 全册、`03:75-166`、`code-evidence.md:67-81`、`m01-m06` | ①④ | R01-R27 项序/属性快捷条/破坏性命令分组/菜单 CSS/子菜单键盘路径 | `EntityCommands.*`、`TaskPanel.cpp` | 截图对照 m03/m05 |
| R7 日期选择器 | `03:45-73`、`t01`、`g02`、`field-notes.md:29,80-85`、`04:40-41,148-149` | ① | 260×540 浮层/日期|时间段分段/42 格月历/时间·提醒·重复子层/清除·确定 | `DatePickerPopup.*`、`TaskDetailWidget.cpp` | 截图对照 t01 |
| R8 详情区 | `01:46-48,92-95`、`03:30-39`、`t02`、`m05`、`04:119-129` | ① | 固定顶栏/底栏 48px/正文独立滚动/低频收纳/R11 更多 14 项 | `TaskDetailWidget.cpp` | 截图对照 t02/m05 |
| R9 编辑器/附件/日志 | `03:168-182`、`code-evidence.md:83-94`、`04:121,129,154-160`、`windows/04:63-67` | ①③④ | 文字按钮→SVG 图标+二级菜单/附件/活动日志/检查清单 | `MarkdownEditor.*`、`AttachmentWidget.*`、`ActivityLogDialog.*` | CTest |
| R10 设置/主题/快捷键 | `06:11-45,68-70`、`s01`、`code-evidence.md:107`、`windows/07:5-34` | ①③④ | 设置弹窗 800×680/左栏 213px/主题切换 UI/显示设置/快捷键展示 | `SettingsDialog.*`、`TaskPanel.cpp` | 截图对照 s01 |
| R11 外围一致性 | `windows/06:80-84`、`windows/07:68-71`、`windows/08:40-44`、`01:132-138`、`07:94-108` | ③⑤ | 便签/托盘/提醒弹窗图标与令牌统一/四态/窄窗 | `NoteWidget.*`、`TitleBar.*`、`ReminderPopup.*`、`TrayController.*` | CTest + 截图 |
| R12 契约收口 | 本文件 | — | 逐区域核对/偏差记录/验收矩阵 | 本文件 | 用户验收 |

## 首轮截图对照索引（历史，非本轮验收）

| 截图 | 区域 | 状态 |
|---|---|---|
| `g01-workspace-redacted.png` | R2 | 待对照 |
| `g02-detail-and-date-redacted.png` | R2/R7 | 待对照 |
| `n01-search-empty-result.png` | R4 | 待对照 |
| `t01-date-picker.png` | R7 | 待对照 |
| `t02-detail-footer.png` | R8 | 待对照 |
| `m01-smartlist.png` | R6 | 待对照 |
| `m02-inbox.png` | R6 | 待对照 |
| `m03-task.png` | R6 | 待对照 |
| `m04-workspace.png` | R4/R6 | 待对照 |
| `m05-detail.png` | R8 | 待对照 |
| `m06-filter.png` | R3/R6 | 待对照 |
| `s01-modules.png` | R10 | 待对照 |
| `static-resource-colors.png` | R1 | 已采用（色板来源） |

## 首轮偏差记录（历史，后续记录优先）

| 考据值 | 实际采用值 | 理由 | 证据等级 |
|---|---|---|---|
| 任务行 40px | 首轮 44px；2026-11-15 已为实体 40px／分组头 36px | 首轮两行元信息方案已被本轮单行实体布局替代 | ① |
| 侧栏 249px | 232px | 适配中文密度，保持可读 | ① |
| 圆角分层 14/12/10/8 | 菜单 12px / 行 8px / 控件 6px | Qt QSS 圆角能力有限，取核心层次 | ④ |
| 功能条 50px | 未实现 | 单一任务项目，功能条无多模块可切换（07:24） | ⑤ |
| 粉色渐变主题 | 蓝色 `#4772FA` 默认 | 粉色是采样账号自选主题，非默认规范 | ① |
| 深色主题色值 | KylinList 自定 | 考据无深色色值证据 | — |
| 设置弹窗 800×680 | 首轮集成在溢出菜单；2026-11-15 已有独立 `SettingsDialog` | 首轮菜单方案为历史；当前弹窗提供智能入口三态本机设置 | ①③ |
| R07 完整项序 | 部分实现 | EntityCommands 提供框架；专注/模板等未实现功能不展示入口 | ① |
| 日期弹层 260×540 | 约 280×520 | Qt 控件尺寸与 CSS 略有差异，保持比例 | ① |
| 详情低频字段平铺 | 全部收进「更多设置」折叠区 | 用户验收指出首版 R8 仍把三行日期编辑/重复/预设/附件/检查清单/便签外观全铺开，正文被挤到中部；按 `01:29-48`、`07:28` 二次重构为固定顶栏+正文主区+固定底栏 | ①⑤ |
| 日期弹层入口 | 顶栏「设置日期」chip | 首版 R7 的 `DatePickerPopup` 为死代码，本次接入 `openDatePicker()` | ① |
| 顶栏常驻控件 | 仅完成框+设置日期chip+优先级旗标 | 用户确认严格按 g02；置顶/贴桌面收进 ··· 菜单 | ① |
| 格式按钮位置 | 底栏「A」图标 → 悬浮格式面板 + 选中文字气泡条 | 用户确认；考据 `field-notes:87` `td-footer-markdown` + `03:36` + t02 + 官方旧图；气泡条为 KylinList 补设计（Web 现场未采到选区气泡，Windows 册 `editormenuex.baml` 仅静态线索） | ①③⑤ |
| 底栏按钮 | 清单名(左) + A/评论/···(右) | 对齐 t02/官方旧图；去掉「立即保存」「更多设置」平铺 | ①② |
| 日期/时间/提醒/重复 | 全部走 DatePickerPopup 浮层子页面 | 考据 `01:11` 临时操作层；移除折叠区三行 QDateTimeEdit 平铺 | ① |
| 编辑/预览分区 | 首轮去掉双区，使用活动行源码／其余行富文本（历史） | 该旧模型已由2026-11-17文首“正文即时格式”取代：当前编辑段落持续呈现格式，光标进出不退回源码 | ⑤ |

## 首轮实施记录（历史，不等于当前验收）

| 区域 | 状态 | 提交 |
|---|---|---|
| R1 设计系统 | ✅ 完成 | `2ef6623` |
| R2 三栏几何 | ✅ 完成 | `37174b6` |
| R3 左侧导航 | ✅ 完成 | `37174b6` |
| R4 搜索/工具栏/快速添加 | ✅ 完成 | `37174b6` |
| R5 任务列表/行内 | ✅ 完成 | `37174b6` |
| R6 右键/二级菜单 | ✅ 完成 | `3de318a` |
| R7 日期选择器 | ✅ 完成 | `3de318a` |
| R8 详情区 | ✅ 完成（三次重构：平铺→折叠→g02/t02 严格对齐） | `f043bd7` + 本次 |
| R9 编辑器/附件/日志 | ✅ 完成 | `f043bd7` |
| R10 设置/主题/快捷键 | ✅ 完成（主题切换在备份菜单中） | `f043bd7` |
| R11 外围一致性 | ✅ 完成 | `f043bd7` |
| R12 契约收口 | ✅ 本文档 | — |

## 2026-11-09 详情编辑页精细对齐

本轮以用户新提供的真实 Windows 详情页、信息浮层和横向格式工具岛为更具体的参照。旧表中的「完成」只记录首轮功能落地；用户已指出视觉及交互差距，不能将其作为精细对齐通过的证据。新参照在详情页位置关系上优先于首轮 g02 转译。原图含私人内容，不复制进仓库；验证使用独立临时数据和合成文字。

| 细节 | 本轮规格 | 实现 |
|---|---|---|
| 分隔线 | 工作区列表与详情之间 1 逻辑像素；日期栏下 1 像素细线 | `TaskPanel`、`QssBuilder` |
| 顶栏 | 任务完成框、日期／提醒在左、信息图标在右；优先级收进更多菜单。覆盖详情关闭在最左且右侧信息前有分割线，普通停靠不显示关闭；子任务父级链接为 13px/22px、距标题 8px，可点击跳转 | `TaskDetailWidget` |
| 日期状态 | 本年显示「M月d日」，其他年份用 `yyyy/MM/dd`，起止时刻紧跟对应日期。相对说明及逾期状态以时间段结束日判断；窄栏可换行 | `detailScheduleCaption()`、`updateDateChip()` |
| 标题与正文 | 仍为两个字段；标题无框、同背景、同左边缘，与正文一起滚动，靠字重字号区分 | `TaskDetailForm`、`MarkdownEditor::setFlowLayout()` |
| 检查项与附件 | 每项一行、内容决定高度、无常驻分区标题、无空态占位；一个外层滚动区 | `TaskChecklist`、`AttachmentWidget::setInlineMode()` |
| 信息浮层 | 字数、字符数、创建于、修改于；统计当前可见文字，日期读取实体真实时间 | `EntityInfoPopup` |
| 所属清单 | 底栏图标与清单名；上方打开搜索／文件夹／清单浮层，当前清单有选中标记；移动时保留正文和检查项 UUID，清理旧 section 归属 | `ListSelectPopup` |
| 格式工具岛 | 底部 A 上方单排横向工具岛，窄栏适配、分组细线、紧凑按钮；保留选区工具条 | `FormatToolbar` |
| 文字格式 | 标题、粗体、列表、斜体、下划线、删除线；五种语义高亮色，可保存、重载、撤销 | `MarkdownEditor`、`Markdown`、`DesignTokens` |
| 保存反馈 | 平时不占正文空间，错误时就地显示可操作的失败提示 | `TaskDetailWidget::setStatus()` |
| 系统阴影 | 全部禁用：浮层声明无系统投影标志，另在原生句柄创建时统一清除 Windows 窗口类的 `CS_DROPSHADOW`；阴影只由应用自绘 | `SystemShadow`、`DetailPopups`、`FormatToolbar`、`DatePickerPopup` |

所属清单弹层的搜索和文件夹组织根据已有调研与本项目模型适配；本次没有可核对的所属清单展开截图，不把该展开态称为竞品逐像素复刻。五种高亮色随浅／深主题取语义令牌，为本项目配色。

高亮保存为白名单标记 `<mark data-color="yellow|green|blue|pink|purple">…</mark>`，下划线为 `<u>…</u>`；任意 HTML 仍按原有安全规则处理。字数采用 Qt 单词边界统计文字／数字，字符数统计 Unicode 码点，含空格、标点，不含换行和格式标记。统计包含标题、正文和检查项，不计附件文件名。创建／修改日期按上海时区显示，提示中保留秒级时间。

### 本轮验证记录

- Windows x64、Qt 5.12.8、MinGW 7.3：Release 构建及 CTest `tasks`／`ui` 均通过（2/2；QtTest 分别报告 34／53 个通过条目）。便携包在移除 Qt／编译器 PATH 后完成 GUI 启动、SQLite 初始化及正常退出冒烟。
- Ubuntu 20.04 x86_64、发行版 Qt 5.12.8：构建及 CTest 2/2 通过。构建镜像原先缺少项目已有的 `libqt5svg5-dev`，仅在临时容器内补齐，宿主系统及软件源未修改。
- 原生 Windows 平台生成并检查详情、信息浮层、清单选择、格式岛、360px 窄栏及长文底部截图；位于忽略目录 `.tools/ui-screenshots-detail-refinement/`。offscreen 默认屏幕较小，不能据此判断 1240px 窗口的屏幕边界弹层定位。截图仅含合成内容及本地生成的参考图片。
- 新回归覆盖格式安全与序列化、特殊字符重选／改色／下划线、撤销重做、重开实体、标题及正文共滚动、检查项 UUID、清单移动、日期状态、信息统计和长文收缩。截图发现的「标题字体串入下一普通段落」另由 `markdownBlocksKeepIndependentFonts` 固定回归。
- 本轮是原生 Qt Widgets UI，无浏览器界面。以上结果不等同于银河麒麟 V10 SP 目标机验收，目标机桌面验证仍须实际执行。

## 2026-11-09 整体窗口与四列精细对齐

本轮以用户提供的真实 Windows 整体窗口截图为直接参照，优先处理窗口骨架，不把此前的三列实现当作精细对齐验收。截图含用户内容，不复制进仓库；截图测试继续使用合成数据。

| 细节 | 本轮规格 | 实现 |
|---|---|---|
| 融合标题区 | 不显示系统标题栏或独立应用标题条；各列背景延伸到窗口顶边，仅最右侧保留最小化／最大化／关闭按钮 | `TaskPanel`、`WindowChrome` |
| 窗口操作 | 顶部空白拖动、双击最大化／还原；Windows 边缘和角落可缩放；关闭仍遵循托盘语义 | `WindowDragArea`、`TaskPanel::nativeEvent()` |
| 第一列 | 52px 窄功能栏：本机工作区、任务、搜索、笔记、数据说明、设置 | `ActivityRail` |
| 第二列 | 232px 筛选与组织栏：智能筛选、收集箱、文件夹／清单、历史；新建入口收在顶部 | `Sidebar`、`NavigationDelegate` |
| 第三列 | 折叠、清单标题、计数与更多；40px 整行快速添加，日期入口仅在行内聚焦或日期弹层打开时显示；搜索由功能栏按需展开 | `ListPane`、`QuickAddFrame` |
| 第四列 | 选中实体才显示；详情关闭按钮收起后第三列占满余宽；窗口按钮不与详情信息按钮重叠 | `WorkspaceSplitter`、`DetailClose` |
| 底部状态栏 | 完全移除，不保留空白条；临时反馈使用轻提示，错误仍就地显示 | `TaskPanel::showNotice()` |
| 系统阴影 | 主窗口也声明 `Qt::NoDropShadowWindowHint`，沿用全局原生阴影禁用策略 | `TaskPanel`、`SystemShadow` |

### 本轮验证记录

- Windows x64、Qt 5.12.8、MinGW 7.3：Release 构建与 CTest `tasks`／`ui` 2/2 通过；新增四列几何、详情显隐、无状态栏、窗口最大化／还原与控件避让回归。
- Ubuntu 20.04 x86_64、发行版 Qt 5.12.8：构建及 CTest 2/2 通过。临时容器补充既有 SVG 开发依赖，宿主系统未修改。
- 原生 Windows 截图分别核对三列与四列状态，位于忽略目录 `.tools/ui-screenshots-detail-refinement/`。本轮未改变数据模型、保存语义或托盘行为。
- 以上是 Windows 预览与 Ubuntu 容器验证，不是银河麒麟 V10 SP 目标机验收。

## 2026-11-09 提示岛与窗口控制区修正

| 细节 | 本轮规格 | 实现 |
|---|---|---|
| 可复用提示岛 | 文本、可选动作、3 秒生命周期集中封装；宿主只负责第三列底部居中定位 | `ToastIsland`、`TaskPanel` |
| 悬停与撤销 | 悬停暂停，离开按剩余时间继续；完成／放弃任务提供撤销，恢复原状态；新提示替换旧提示 | `ToastIsland`、`TaskPanel::showUndoableStatus()` |
| 固定配色 | 深灰底、浅灰文字、金色撤销图标，不读取主题色 | `ToastIsland.cpp` |
| 窗口按钮层级 | 第二至第四列共享顶部 28px 窗口控制条，第二、三列无独立标题背景；详情日期／提醒／信息栏位于其下，不共行 | `WorkspaceGeometry`、`TaskPanel`、`WindowControls` |

顶部对齐补充（2026-11-15）：三个窗口按钮均为 46×28px；第四列 48px 详情功能行顶边距 28px，第二列 36px 导航首行顶边距 34px，第三列 40px 标题栏顶边距 32px，三列布局中心统一在相对列顶 52px。详情功能按钮显式垂直居中，内部上／下边距为 7px／5px，补偿 Qt 5.12 偶数矩形整数中心偏上的 1px。普通窗口、最大化／还原和窄窗覆盖详情共用该相对几何。

验证：提示岛自动消失、悬停暂停、动作关闭、完成／放弃撤销、第三列定位与窗口控制区上下层级均有 QtTest 回归；`workspaceHeaderAlignmentAndSidebarIcons`、`workspaceUsesFourColumnsAndNoPermanentStatusBar` 与 `responsiveColumnsAndOverlayDetail` 固定三列首行中心、28px 控制条及按钮无重叠。原生截图核对 `task-panel-toast.png`。

补充修正：四列背景与分割线贯穿顶部 28px 窗口控制区，不再出现独立色带；普通窗口使用 10px 圆角与细轮廓，最大化／全屏时取消圆角。`workspaceUsesFourColumnsAndNoPermanentStatusBar` 固定列顶边、透明背景、圆角遮罩与最大化还原行为。

进一步修正：圆角改由 `WorkspaceSurface` 的抗锯齿路径绘制，取消硬边 `setMask`；四周 12px 透明区域用于应用自绘轻阴影，边框使用语义细线。关闭按钮悬停与其他窗口按钮一致，不使用红色。窗口按钮明确作用于最外层窗口，避免移入圆角容器后误操作内部控件。

按右上角对比图再次修正：边框使用独立 `WindowBorder` 语义色提高可辨识度；阴影由离散描边改为直边线性渐变、转角径向渐变，消除明显阶梯；内容列和窗口关闭按钮的转角使用相同 10px 圆角。原生放大截图 `window-corner-detail.png` 用于核对圆弧、边框与阴影连续性。

最终边界修正：浅色背景下发现分区渐变仍在圆角旁形成直角接缝，改为整张圆角 alpha 图的可分离高斯模糊，直边与转角共享连续阴影场；`WindowBorder` 提升为明确的中性灰实线。放大图合成到浅灰背景后复核，无直角接缝或异常色边。

绘制层级修正：父容器底层边框会被内容覆盖，因此新增 `WindowBorderOverlay` 在所有工作区内容之上绘制实线轮廓且不拦截鼠标；内容容器按同一圆角路径裁剪，详情底栏与内部视口不再留下右下角直角。新增 `window-bottom-corner-detail.png` 与右上角一起核对。

阴影可见性修正：内容圆角遮罩曾把透明外边距里的阴影一并裁掉。现在只裁直接内容子控件，`WorkspaceSurface` 保留完整透明区域绘制高斯阴影；边框 overlay 不裁剪。回归同时断言主窗口和阴影宿主无硬遮罩、最大化时内容遮罩清除。

## 2026-11-10 可调列宽与窄窗覆盖详情

| 细节 | 本轮规格 | 实现 |
|---|---|---|
| 常驻第四列 | 宽窗口始终保留详情列；无选择、关闭或删除后显示纯空白，不占位提示、不收起 | `TaskPanel`、`TaskDetailWidget` |
| 窗口尺寸 | 允许边缘与角落调整长宽，最小 620×560；内部列表、快速添加和详情随宽度伸缩 | `TaskPanel` |
| 两个可调边界 | 第二列 180–360px，第四列按可用空间调整；两个 handle 均为 1px，第一列固定 52px | `WorkspaceSplitter`、`QssBuilder` |
| 第二列显隐 | 列表标题栏提供显隐按钮；手动隐藏与自动隐藏分开记录，列宽分别持久化 | `TaskPanel`、`SidebarToggle` |
| 响应式顺序 | 变窄先收第四列，再收第二列；拉宽先恢复第二列，再恢复第四列，保留用户列宽 | `TaskPanel::reconcileWorkspaceLayout()` |
| 覆盖详情 | 第四列自动隐藏后，选中实体在右侧显示约 300px 覆盖详情，不改变列表宽度；关闭按钮位于最左且只隐藏浮层，普通停靠详情不显示关闭 | `TaskPanel::updateDetailOverlayGeometry()`、`TaskDetailWidget::setOverlayMode()` |
| 分界缺角 | 中间列不再误用窗口外缘圆角；分割线与栏背景不重复描边，第三／第四列上端无透底小角 | `QssBuilder` |

验证：`workspaceUsesFourColumnsAndNoPermanentStatusBar` 更新为常驻空白详情语义；`responsiveColumnsAndOverlayDetail` 覆盖两个 1px 边界、固定功能栏、最小窗口、自动隐藏／恢复、覆盖详情几何与关闭、手动隐藏第二列。原生 Windows 截图新增 `workspace-divider-detail.png`、`task-panel-auto-three-columns.png`、`task-panel-detail-overlay.png`、`task-panel-auto-two-columns.png`，核对分界上端、覆盖堆叠和两列状态。以上仍不是银河麒麟 V10 SP 目标机验收。

## 2026-11-10 窗口缩放、还原与阴影性能修正

| 问题 | 修正 | 验证 |
|---|---|---|
| 边缘无缩放光标且不能拖动 | Windows 无边框窗口补 `WS_THICKFRAME`；可见边框周围增加八方向透明 `WindowResizeArea`，直接调整窗口几何并遵循最小尺寸，X11 共用 | `nativeWindowResizeAndHitTesting` 核对八方向及可见边缘命中；`resizeAreasDragAllWindowEdges` 验证右边与右下角拖动、光标和最大化隐藏；桌面实际拖动 1240×820→1160×820、再至 1130×760 |
| 最小化召出后边框大于内容 | 删除还原路径的 18px 特例；`syncWindowDecoration()` 统一 21px／最大化 0，并覆盖 show、状态变化和窗口按钮操作 | 原生 `nativeWindowDecorationRestore` 与四列回归核对普通→最小化→还原、最大化→还原的真实边框几何 |
| 最大化卡顿 | 直角模式完全不生成阴影；普通窗口按主题缓存 128×128 高斯模板，九宫格伸缩，不再为每次 resize 分配全窗口缓冲并执行 74 次/像素卷积 | 原生截图核对圆角、边框、阴影；桌面最大化操作响应约 50ms，全量 CTest 保持通过 |

测试中的边框按 `objectName=WindowBorderOverlay` 获取；该类没有 `Q_OBJECT`，不能用无名称的 `findChild<WindowBorderOverlay*>()`，否则可能退化为普通 QWidget 匹配并误取其他控件。

## 2026-11-10 列拖动与覆盖详情边界修正

| 细节 | 本轮规格 | 实现与验证 |
|---|---|---|
| 第三／第四列拖动 | 1px 视觉线周围增加 12px 透明命中区；向左拖动同步加宽第四列并移动边界，遵守两侧最小宽度 | `SplitterResizeArea`；回归驱动真实鼠标事件，桌面验证边界 x=796→716→796 |
| 覆盖态顶部残留线 | 删除覆盖 `DetailFrame` 的 `border-left`，不再让顶部 36px 单独露出边框；覆盖时隐藏详情列拖动入口 | `QssBuilder`、`syncColumnResizeAreas()`；原生 `task-panel-detail-overlay.png` |
| 覆盖层级阴影 | 详情左侧增加独立 18px 渐隐阴影，不拦截鼠标、不挤压列表、不改变主窗口外阴影 | `OverlayEdgeShadow`；回归检查位置、显隐及 18px 宽度，原生截图核对 |

Windows 与 Ubuntu 20.04 的 Qt 5.12.8 全量 CTest 均须通过；该记录不代表麒麟目标机验收。

补充：覆盖详情脱离 splitter 后不再使用固定宽度。`OverlayDetailResizeArea` 位于其实际左边缘，默认 300px，可在 260–480px 内调整，并给第三列保留至少 260px；拖动时详情几何、左侧阴影和命中区同步移动，列表宽度不变。`responsiveColumnsAndOverlayDetail` 覆盖 300→380→300 双向拖动、阴影跟随、关闭及恢复停靠后的手柄显隐。

退出覆盖或退出专注模式时，必须用 `WorkspaceSplitter::insertWidget(2, DetailFrame)` 重新加入分割器；单独 `setParent(m_splitter)` 不会恢复 QSplitter 内部项，会导致详情保留旧几何并脱离窗口右边。原生回归同时断言 `indexOf(DetailFrame)==2` 和详情右边缘与内容右边界重合。

覆盖拖动按相邻两次 `mouseMoveEvent` 的全局位置差更新宽度，不能反复累加相对按下点的总位移。回归在一次拖动中连续发送两个移动事件，覆盖 300→380→300，避免实际鼠标多事件拖动时过早撞到上下限。最终桌面预览使用隔离测试数据实测：1240px 窗口缩至 872px 进入覆盖模式，左边缘双向拖动对应 300→400→300px，阴影同步且列表几何不变；随后最大化退出覆盖，详情重新停靠并贴合右边框，顶部按钮保持归位。Windows 与 Ubuntu 20.04 的 Qt 5.12.8 全量 CTest 均通过，仍不代表银河麒麟目标机验收。

## 2026-11-10 覆盖退出后的裁剪残留修正

上轮最大化退出覆盖的验证漏掉了普通窗口拖宽路径。`WorkspaceSurface::updateClip()` 为直接子控件设置内容裁剪；覆盖详情离开 surface、重新插入 splitter 后仍保留旧 mask，而后续裁剪更新只遍历 surface 的直接子控件，不会清理已经嵌入 splitter 的详情。结果是第四列几何已恢复，右侧约 70px 内容仍被覆盖时的 300px 遮罩截掉；最大化恰好先清除该遮罩，因而掩盖问题。

所有详情重新停靠的路径在 `insertWidget()` 前调用 `clearMask()`，将停靠内容的裁剪重新交由 splitter 所在的窗口内容层负责。`detailRedocksAfterWindowEdgeDrag` 保持实体选中，使用窗口右边缘的连续按下／移动／释放事件两轮执行 1240→900→1240px，再双向拖动第三／第四列边界；第二轮先改变覆盖详情宽度，检查恢复后无残留 mask、右侧内容可见、详情贴合窗口、覆盖阴影与拖动区隐藏。该回归在修复前的 offscreen 与原生 Windows 下均能稳定失败，修复后通过。原生前后对照截图保存在本地 `.tools/ui-screenshots-overlay-redock/`。

验证结果：Windows Qt 5.12.8 全量 CTest 2/2、原生 Windows UI 62 项、Ubuntu 20.04 Qt 5.12.8 全量 CTest 2/2 均通过。原生整套测试还暴露出清单输入框用例缺少窗口激活前置条件，补齐该测试夹具后通过，未修改清单产品逻辑。桌面最新预览实测 1448→901px 进入覆盖，再直接拖宽右边框至 921px 退出，并继续拖宽至 961px、双向拖动列边界；全程不经过最大化，详情右侧按钮和底栏均完整显示。对照图 `comparison-right-edge.png` 提供修复前后的右上角／右下角 2 倍局部图，分别合成浅、深底色。桌面可执行文件与本次构建 SHA256 一致。以上仍不代替银河麒麟目标机验证。
