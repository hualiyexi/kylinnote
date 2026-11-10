# UI 对齐契约：逐区域追踪矩阵

本文件是「按考据文档逐区域对齐滴答清单基础体验」的可验收契约。每个区域记录考据来源、证据等级、目标规格、实施文件与验证方式。`docs/research/**` 是只读证据，不改写。

## 证据等级

沿用考据册分类（`ticktick-windows-6.3.3/01-environment-and-evidence.md:31-43`、`ticktick-webapp/08-evidence-and-coverage.md:25-35`）：

- **①** Web 现场实测截图/DOM
- **②** 官方旧图
- **③** BAML 静态资源定义
- **④** 发布 JS/CSS 代码证据
- **⑤** 设计建议/文字推断

## 三条硬边界

1. Web 册 16 张图是 2026-11-07、1500×940、Chromium 152、**当前账号自选粉色渐变主题**下的现场脱敏截图；顶部 36px 图注与灰块是研究加工，不是产品 UI。粉色 `#FF79A4→#FFAFA6` 是采样值，不是默认规范。
2. Windows 6.3.3 册**本机实测截图 0 张、右键菜单展开 0/21**（`README.md:5-9`、`09-live-capture-checklist.md:14-20`）；6 张配图全是官方旧图/本项目示例图/自制色板。只能提供静态资源定义与 Qt 转译建议。
3. 深色主题**无任何具体色值证据**。深色值由 KylinList 自定，明确标注「非滴答实测」。

## 区域矩阵

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

## 截图对照索引

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

## 偏差记录

| 考据值 | 实际采用值 | 理由 | 证据等级 |
|---|---|---|---|
| 任务行 40px | 44px | 中文标题+元信息两行需要略大高度 | ① |
| 侧栏 249px | 232px | 适配中文密度，保持可读 | ① |
| 圆角分层 14/12/10/8 | 菜单 12px / 行 8px / 控件 6px | Qt QSS 圆角能力有限，取核心层次 | ④ |
| 功能条 50px | 未实现 | 单一任务项目，功能条无多模块可切换（07:24） | ⑤ |
| 粉色渐变主题 | 蓝色 `#4772FA` 默认 | 粉色是采样账号自选主题，非默认规范 | ① |
| 深色主题色值 | KylinList 自定 | 考据无深色色值证据 | — |
| 设置弹窗 800×680 | 集成在溢出菜单中 | 本轮设置项较少，不需要独立弹窗；主题切换已在备份菜单中 | ①③ |
| R07 完整项序 | 部分实现 | EntityCommands 提供框架；专注/模板/打印等未实现功能不展示入口 | ① |
| 日期弹层 260×540 | 约 280×520 | Qt 控件尺寸与 CSS 略有差异，保持比例 | ① |
| 详情低频字段平铺 | 全部收进「更多设置」折叠区 | 用户验收指出首版 R8 仍把三行日期编辑/重复/预设/附件/检查清单/便签外观全铺开，正文被挤到中部；按 `01:29-48`、`07:28` 二次重构为固定顶栏+正文主区+固定底栏 | ①⑤ |
| 日期弹层入口 | 顶栏「设置日期」chip | 首版 R7 的 `DatePickerPopup` 为死代码，本次接入 `openDatePicker()` | ① |
| 顶栏常驻控件 | 仅完成框+设置日期chip+优先级旗标 | 用户确认严格按 g02；置顶/贴桌面收进 ··· 菜单 | ① |
| 格式按钮位置 | 底栏「A」图标 → 悬浮格式面板 + 选中文字气泡条 | 用户确认；考据 `field-notes:87` `td-footer-markdown` + `03:36` + t02 + 官方旧图；气泡条为 KylinList 补设计（Web 现场未采到选区气泡，Windows 册 `editormenuex.baml` 仅静态线索） | ①③⑤ |
| 底栏按钮 | 清单名(左) + A/评论/···(右) | 对齐 t02/官方旧图；去掉「立即保存」「更多设置」平铺 | ①② |
| 日期/时间/提醒/重复 | 全部走 DatePickerPopup 浮层子页面 | 考据 `01:11` 临时操作层；移除折叠区三行 QDateTimeEdit 平铺 | ① |
| 编辑/预览分区 | 去掉，改为行内所见即所得（WYSIWYG） | 用户要求「离开这一行就要渲染好」；当前行显示 Markdown 源码，其他行渲染富文本；类似 Typora/Notion/滴答 ProseMirror | ⑤ |

## 首轮实施记录（不等于精细对齐验收）

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
| 顶栏 | 日期／提醒在左、信息图标在右；任务完成及优先级收进更多菜单 | `TaskDetailWidget` |
| 日期状态 | 上海时区显示 `yyyy/MM/dd`；有效未完成任务显示「延期 N 天」，逾期文字和日历图标同色 | `updateDateChip()` |
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
| 第三列 | 清单标题、计数、排序与更多；40px 整行快速添加；搜索由功能栏按需展开 | `ListPane`、`QuickAddFrame` |
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
| 窗口按钮层级 | 顶部 36px 为无独立标题背景的窗口控制区；详情日期／提醒／信息栏位于其下，不共行 | `TaskPanel`、`WindowControls` |

验证：提示岛自动消失、悬停暂停、动作关闭、完成／放弃撤销、第三列定位与窗口控制区上下层级均有 QtTest 回归；原生截图核对 `task-panel-toast.png`。

补充修正：四列背景与分割线贯穿顶部 36px 窗口控制区，不再出现独立色带；普通窗口使用 10px 圆角与细轮廓，最大化／全屏时取消圆角。`workspaceUsesFourColumnsAndNoPermanentStatusBar` 固定列顶边、透明背景、圆角遮罩与最大化还原行为。

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
| 覆盖详情 | 第四列自动隐藏后，选中实体在右侧显示约 300px 覆盖详情，不改变列表宽度；关闭按钮紧邻提醒，关闭只隐藏浮层 | `TaskPanel::updateDetailOverlayGeometry()`、`TaskDetailWidget::setOverlayMode()` |
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
