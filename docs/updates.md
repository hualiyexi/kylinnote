# 内网软件更新

更新日期：2026-11-18。本文描述麒麟清单的可信局域网更新分发：Docker 发布服务、更新清单协议、历史公告、客户端检查与安装流程，以及发布方式。聚合使用统计见[统计说明](stats.md)。当前实现与自动化验证见下节；历史记录不代表当前二进制已通过。数据与 UI 改动不改变更新清单协议 v1 或既有发布流程。

## 当前实现与验证状态

### 2026-11-18 托盘与桌面任务小部件

托盘右键使用统一圆角浮层。菜单提供「添加桌面小部件 → 任务」「主窗口置顶」和「退出」。锁定入口和全局快捷键暂停开放。设置的「桌面小部件」页也可添加任务小部件，两处入口共用一个窗口。

任务小部件共用第三列任务模型、日期控制和实体命令。默认显示收集箱，分组、日期、终态、详情摘要与外观可在右上角设置。位置、大小、清单范围和打开状态存于本机 INI，普通启动和后台启动恢复仍打开的小部件。显式 `--preview-autostart --data-dir <绝对路径>` 的开发预览可手动登记独立启动项。常规隔离运行和自检没有该权限。

<!-- current-verification:2026-11-18-task-widget -->
Windows x64 使用 Qt 5.12.8 与 C++14 构建。离屏 CTest 23/23 套通过。Windows 原生默认缩放和 150% 缩放各有 8 项通过，覆盖菜单、添加与范围选择、已打开状态的后台恢复、设置和会话保存。圆角局部在浅色和深色底上放大核对。独立运行时自检成功。缺失独立目录、相对路径或与自检组合的预览自启参数均返回退出码 2。

验证使用合成数据，保留现有系统自启配置。原生菜单用例从托盘图标绑定的菜单开始，Windows Shell 实际右键投递、真实开机登录和麒麟 ARM64 真机仍待验证。
<!-- current-verification:end -->

### 2026-11-18 开发预览可手动登记开机自启（前序）

用户反馈设置中的「开机自启动」开关点不动。原因是`--data-dir`预览被归入`Mode::Isolated`，按原策略整个后端被置空，开关强制禁用；同时机器上可能残留只有`--background`的旧开发启动项，界面又因禁用而强制显示为关闭。现在新增`Mode::Preview`：开发预览仍绝不自动登记，但允许用户在设置里手动开启；`KylinListApp`把当前`--data-dir`固定写入登录命令，确保登录后恢复的是这组合成便签。`isEnabled()`只认与当前程序、当前数据目录命令完全一致的条目，旧的无参数启动项不会误显示为已开启。Windows Run命令行为含空白的数据目录参数补引号与反斜杠转义，避免`--data-dir`被拆开。设置页在预览模式下显示「当前为开发预览」说明，开关可点击，失败仍回滚并提示。自检和纯隔离模式不接真实后端；正式安装版默认开启、便携版仅手动开启的规则不变。

<!-- previous-verification:2026-11-18-preview-autostart -->
定向验证：`test_updates`离屏49通过／0失败／0跳过（新增Preview手动登记与Windows命令引用2例）；完整`test_ui`离屏117通过／0失败／4跳过。受影响用例离屏与Windows原生各5通过／0失败／0跳过，覆盖预览模式可用性、命令携带数据目录、设置开关点击、会话保存与冷启动便签恢复。原生完整UI批次另有3个焦点／鼠标命中失败，其中设置用例单独重跑3通过／0失败／0跳过；其余窗口悬浮与附件命中失败保留日志，不记作通过。自动化全部使用替身后端或临时数据目录，未写入真实Run项或Linux自启文件；只读取了现有注册表值用于确认旧条目不会被误判为已开启。150%矩阵未在本轮重跑。预览已用新程序按同一合成目录重启，PID27636；真实登录投递和麒麟ARM64由用户验收。
<!-- previous-verification:end -->

### 2026-11-18 通用设置、开机自启与会话恢复

第一列 Logo 的「登录时自动启动」已移除，改在设置左栏新增「通用」，右侧显示「开机自启动」30×18紧凑开关和「登录系统后自动运行，并恢复上次打开的桌面便签」。主窗口设置与便签更多→设置→应用设置共用同一`AutostartSettings`实例；便签菜单关闭后再显示应用级设置窗口，不在QMenu回调里创建后立即失去宿主。

设置标题栏现在横跨整张卡片，左侧保留212px分栏，右侧关闭按钮距卡片右边10px；不再把整个标题栏固定到左栏宽度。Esc和关闭仍走原对话框拒绝路径。正式安装版首次正常运行沿用现有`autostart/initialized`标记默认登记，用户关闭后不再复活；Windows当前用户Run项与麒麟`~/.config/autostart/kylinlist.desktop`继续使用`--background`，Linux随包Qt仍走`/usr/bin/kylinlist`脚本。自检与纯隔离模式完全不接真实登记后端；开发预览（`--data-dir`）允许用户在设置里手动登记，登录命令固定带上当前数据目录，但绝不自动登记；便携版只允许明确开启，不自动登记。写入失败回读真实状态并提示，不新增第二套偏好键。

`KylinListApp::commitSession()`在数据打开后、窗口创建前连接`QGuiApplication::commitDataRequest`，使用`Qt::DirectConnection`并关闭Qt默认的会话回退关闭窗口行为。系统注销／重启只保存正文、编辑会话和桌面投影，不调用便签closeEvent，避免把open=true误写为false。下次后台初始化通过`NoteManager::restoreAll()`恢复仍打开的有效任务便签，正文、位置保留，主动关闭的不恢复；托盘可用时不主动弹主窗口。保存失败取消会话提交并保留草稿。关闭应用时解绑旧会话回调，并清理延迟删除窗口，避免同进程重建时访问旧对象。

参考Windows滴答桌面端公开说明中的「通用／偏好设置」自启归属，未新增「启动至托盘」选项；当前工具无法可靠读取其最小化主窗的设置树，未伪称完成该页面原生视觉复刻。

<!-- previous-verification:2026-11-18-autostart -->
**最终自动化验证：Windows x64／Qt5.12.8／MinGW GCC7.3／C++14，应用与23个测试目标构建成功。完整离屏CTest 23/23套通过（87.14秒），合计899通过／0失败／7跳过；Windows原生完整批次22套通过，随后共享文本完整套件复跑通过，合计905通过／0失败／0跳过。** 原生批次151.92秒，含一项失败；随后独立完整livetext 4.42秒。按测试入口去重后23套合计905，不将失败批次或复核用例重复计入。

| 套件 | offscreen：通过／失败／跳过 | Windows原生：通过／失败／跳过 |
|---|---:|---:|
| tasks | 75／0／0 | 75／0／0 |
| ui | 117／0／4 | 121／0／0 |
| updates | 47／0／0 | 47／0／0 |
| stats | 22／0／0 | 22／0／0 |
| branding | 35／0／0 | 35／0／0 |
| brandicons | 13／0／0 | 13／0／0 |
| database | 47／0／0 | 47／0／0 |
| dropgroups | 35／0／0 | 35／0／0 |
| groupmodel | 29／0／0 | 29／0／0 |
| datepopup | 42／0／0 | 42／0／0 |
| activitytimeline | 12／0／0 | 12／0／0 |
| detail | 90／0／2 | 92／0／0 |
| attachmentcards | 33／0／0 | 33／0／0 |
| editorlinks | 31／0／0 | 31／0／0 |
| editortools | 18／0／0 | 18／0／0 |
| editorinteraction | 50／0／0 | 50／0／0 |
| relateddetail | 28／0／1 | 29／0／0 |
| recurrence | 26／0／0 | 26／0／0 |
| demo | 12／0／0 | 12／0／0 |
| instantmarkdown | 34／0／0 | 34／0／0 |
| livetext | 23／0／0 | 23／0／0 |
| notesnap | 57／0／0 | 57／0／0 |
| desktopnotes | 23／0／0 | 23／0／0 |
| **合计** | **899／0／7** | **905／0／0** |

- `updates`新增7组自启策略用例：首次默认登记恰好一次、已有登记不重写、失败下次重试、显式关闭持续关闭、隔离／不可用零读写、便携版仅显式选择、后端半写错误与信号。真实Run项、Linux自启文件均未在测试中触碰。
- `ui`新增设置分类／开关／提示、失败回滚、原生命中关闭、便签二级菜单共享设置、会话保存与同一隔离库后台重建。Windows原生会话探针只向本进程从未显示的HWND发送`WM_QUERYENDSESSION`与取消后的`WM_ENDSESSION(FALSE)`，不广播、不请求系统关机。它验证真实Qt信号保存标题草稿且便签open保持true；冷启动用例同时验证追加正文、便签位置和主动关闭过滤。
- 150%配置`QT_SCALE_FACTOR=1.5`、`KYLINLIST_EXPECT_DPR=1.5`，UI121、更新47、便签23、日期42、详情92、关联详情29、即时编辑34、编辑交互50、内链31、格式工具18，合计**487通过／0失败／0跳过（122.47秒）**。该矩阵为关闭预览后的单进程串行执行；并行运行原生窗口套件和显示中的预览会干扰真实命中与焦点，相关失败日志保留，未记作通过。窗口遮挡修正后再执行全量原生，产品未为消除测试焦点失败放宽断言。
- 原生livetext完整批次曾出现一次地址身份断言失败，独立单用例3／0／0、随后完整23／0／0均通过；`QPointer`旧对象销毁、恢复文本和撤销历史清空等实际行为断言通过。没有把这一次失败写成零失败，也不以指针地址代替生命周期证据。离屏7项跳过为原有UI3、详情2、关联详情1及新增的仅限Windows原生会话探针1，原生均实际执行。
- 完整离屏保留2753条QWARN及19条`FT_New_Face`枚举QDEBUG；原生所选最终记录37条QWARN，150%为44条，主要为既有负例、安装身份隔离提示与高DPI屏幕限高，不屏蔽输出。日志为`.tools/test-results/autostart-final-full-offscreen-2.log`、`autostart-clean-serial-native-*.log`、`autostart-livetext-final-serial-*.log`、`autostart-clean-serial-dpr150-*.log`及对应JSON；中间并行／遮挡失败日志保留。
- 旧预览按约定结束并删除旧exe后重建；`windeployqt --release --compiler-runtime --no-patchqt`覆盖运行库，保留`qt.conf`与开发Qt缺translations的警告。清除Qt／KylinList变量、PATH仅System32的独立smoke退出0，空库schema10、application_id 0x4B4C5354、完整性ok、外键错误0。核对v20合成manifest后删除旧库，v21由最终程序13/13项验证通过，含42实体／8清单／6提醒／5附件、6桌面状态及2打开便签。
- 为避免真实命中测试被预览遮挡，测试期间停止v21进程，完成后按同一受管合成目录恢复，PID29680的路径、`--data-dir .tools/preview-data-v21-demo`、Responding=true和只读数据库计数已复查。程序SHA-256为`6d0269abb7d6151b4f16444e59f25a40e0b730b6f13fd98109c0a7a15ca05392`。开发预览不会自动登记真实自启；本轮修复后由用户手动点击时才写入当前数据目录命令，自检不写。实际重启登录及银河麒麟V10 SP1／飞腾FT-2000 ARM64由用户验收，不将合成会话测试冒充真机已验。
- 本轮未提交、推送、发布或外部部署，没有浏览器前端测试、依赖安装或Linux／ARM64构建；沿用Qt5.12.8／C++14、既有CI和标签Release流程。
<!-- previous-verification:end -->

### 2026-11-18 前序：简洁桌面便签、窗口吸附与逐字同步

桌面便签改为第四列`TaskDetailWidget`的紧凑宿主，复用同一即时Markdown编辑器和附件卡片，不再维护预览／编辑切换、完成框或编辑按钮。可见纸面为7px抗锯齿圆角、左右14px内容留白、20px顶部拖动区和26px底栏；顶部外观／置顶／关闭使用18px命中与12px图标，底部20px更多按钮仅悬浮显示，隐藏不改变布局。左下日期始终可见，打开共享日期、提醒与重复选择器。更多保留新建、完成、横／纵排列、打开主窗口、设置和删除，不加入开始专注等未实现功能；关闭只取消桌面展示，删除进入回收站。

六色纸面、文字与边框全部由`ds::`语义令牌提供。24px透明阴影与内容圆角分别处理，边框使用鼠标透明overlay最后绘制；共享`PopupShadow`只卷积128px小模板，再九宫格伸缩，不在连续缩放时重算整窗阴影，不使用mask或graphics effect。四边／四角均可缩放，最小纸面220×180px。拖动按原始指针位移，在10px捕获区内吸附当前屏幕工作区或其他可见便签，支持0／8px邻接间距和同边对齐；透明阴影不计间隔，能够自然拖出吸附区。恢复位置也按纸面夹入屏幕，不在重启后额外缩进24px。

同任务的主详情、关联详情与便签共享一个`EntityTextSession`，标题原文与Markdown正文在真实编辑事务完成后立即广播，不等待450ms合并保存。每个视图仍有独立富文本`QTextDocument`，避免不同宽度共用一份布局；远端更新保留各自光标、正反选区、链接标签内局部选区与滚动。标题与正文各有共享撤销／重做历史，键盘和右键入口统一路由，不把投影更新算作编辑。IME预编辑只在本地显示；提交字与下一段预编辑共存时仍即时同步，组合期间收到的远端修改延后安全重定位，真正重叠保留可复制草稿。

持久化继续通过`TextDraft`字段基线校验与事务，只写实际修改字段，不覆盖日期、提醒、父级或桌面状态。空标题草稿可以实时镜像，有效正文照常保存，但阻止关闭；SQL／日志失败、删除目标和保存回调重入均保留草稿。日志只归因实际写过的字段，闲置观察窗口不重复记账，直接`saveTask`仍即时记录真实变化。成功恢复备份销毁旧共享会话及撤销上下文，即使恢复前后文本相同；失败恢复保留原会话与草稿。关联主任务选择器在模态返回后重新读取最新实体，避免覆盖其他窗口的新文本。

参考来自用户授权查看的Windows原生客户端便签：已观察静息／悬浮形态、小圆角、紧凑留白、日期及可操作菜单；部分独立WPF窗口和子菜单受到工具输入关联限制，未宣称已在竞品中完整实测全部吸附／同步行为。没有修改竞品真实任务的标题、正文、日期或完成状态。匿名观察记录仅留本机忽略目录，未将截图、账号或调研原始材料写入仓库。

<!-- previous-verification:2026-11-18-desktop-notes -->
**最终自动化验证：Windows x64／Qt5.12.8／MinGW GCC7.3／C++14，应用及23个测试目标构建成功。完整离屏CTest 23/23套通过（88.19秒），合计890通过／0失败／6跳过；同一应用构建的23个测试程序以Windows原生平台逐套执行，合计896通过／0失败／0跳过。** 原生完整批次149.73秒，随后两处测试fixture更新后的详情／便签原生复核30.80秒；汇总保留来源，编排用时之和180.53秒，用例只计一次。CTest注册平台仍为offscreen，不与原生结果混用。

| 套件 | offscreen：通过／失败／跳过 | Windows原生：通过／失败／跳过 |
|---|---:|---:|
| tasks | 75／0／0 | 75／0／0 |
| ui | 115／0／3 | 118／0／0 |
| updates | 40／0／0 | 40／0／0 |
| stats | 22／0／0 | 22／0／0 |
| branding | 35／0／0 | 35／0／0 |
| brandicons | 13／0／0 | 13／0／0 |
| database | 47／0／0 | 47／0／0 |
| dropgroups | 35／0／0 | 35／0／0 |
| groupmodel | 29／0／0 | 29／0／0 |
| datepopup | 42／0／0 | 42／0／0 |
| activitytimeline | 12／0／0 | 12／0／0 |
| detail | 90／0／2 | 92／0／0 |
| attachmentcards | 33／0／0 | 33／0／0 |
| editorlinks | 31／0／0 | 31／0／0 |
| editortools | 18／0／0 | 18／0／0 |
| editorinteraction | 50／0／0 | 50／0／0 |
| relateddetail | 28／0／1 | 29／0／0 |
| recurrence | 26／0／0 | 26／0／0 |
| demo | 12／0／0 | 12／0／0 |
| instantmarkdown | 34／0／0 | 34／0／0 |
| livetext | 23／0／0 | 23／0／0 |
| notesnap | 57／0／0 | 57／0／0 |
| desktopnotes | 23／0／0 | 23／0／0 |
| **合计** | **890／0／6** | **896／0／0** |

- 新增`livetext`覆盖单实体会话身份、即时广播、合并保存及元数据保真、共享字段撤销、连续Delete顺序、历史上限、空标题、外部冲突、SQL／日志失败、保存／结束／取消的同步重入、信号中销毁与备份恢复；`notesnap`覆盖工作区和便签候选、0／8px间距、阈值、多屏负坐标、无效几何与横纵换行；`desktopnotes`覆盖静息／悬浮控件、准确留白／热区、真实菜单及新建／排列／日期／置顶／关闭／删除／完成、双向逐字编辑、独立换行、选区／滚动、链接标签局部选择、IME取消／提交／下一段预编辑、回收站恢复、真实管理器吸附及深浅主题圆角／边框／阴影。鼠标先验证可见、启用、非零几何与原生`widgetAt()`，像素在内存检查，不保存人工验收截图。
- 中间完整离屏曾为883／7／6，下一轮889／1／6，均未作为交付通过。修复包括暗色纸面边框对比、长正文滚动后的标题可达、远端替换选区方向与链接标签局部选择、选区工具条刷新、共享会话重入与日志归因。旧测试已改为真实共享撤销入口、当前可见附件卡片和明确的日志故障上下文。最后1项离屏失败为窗口未激活，补充显式激活及可见区域前置后，定向3／0／0、完整23套均通过；未放宽文字或撤销断言。
- 150%原生配置`QT_SCALE_FACTOR=1.5`、`KYLINLIST_EXPECT_DPR=1.5`，便签和即时编辑等入口实际核对DPR：便签23、详情92、关联详情29、即时编辑34、编辑交互50、内链31、格式工具18、UI118，合计**395通过／0失败／0跳过（100.06秒）**。初轮为392／3／0，三项均为旧附件fixture假定“三张卡必定无滚动”；实际屏幕约束窗口后，三分之一面板预算仍生效，溢出18px。现在精确断言完整卡片高度减实际viewport高度所得滚动范围，并保留末张可达、正文／面板／底栏不随附件滚动等检查，未改产品预算或增加容差；最终标准原生两套复核115／0／0。
- 完整离屏保留2736条QWARN及19条`FT_New_Face`枚举QDEBUG；原生23套所选最终记录共34条QWARN，150%为36条（详情13、关联详情3、UI20），均未屏蔽。包含既有负例提示与高DPI窗口被屏幕约束等输出，**通过不代表无警告**。离屏6项跳过仍为UI3、详情2、关联详情1的既有原生窗口／消息框／合成能力边界，对应原生均实际执行，无新增skip。
- 最终日志为`.tools/test-results/desktop-notes-final-full-offscreen-4.log`、`desktop-notes-final-native-all-*.log`、`desktop-notes-final-native-fixture-recheck-*.log`及对应JSON；去重原生汇总为`desktop-notes-final-native-summary.json`，每套注明来源。150%最终为`desktop-notes-final-native-dpr150-2-*`；带失败的前序`desktop-notes-final-full-offscreen*`和`desktop-notes-final-native-dpr150-*`原样保留，不覆盖或计作通过。调研、辅助脚本与运行日志经`git check-ignore`确认不入库。
- 按开发机约定结束旧PID51948、检查后删除旧可执行文件并重建；`windeployqt --release --compiler-runtime --no-patchqt`覆盖本地运行库，保留`qt.conf`；部署日志另保留开发Qt目录缺少`translations`的警告，未屏蔽或联网补包。清除Qt／KylinList测试变量、PATH仅含`C:\Windows\System32`的独立smoke退出0，空库schema10、application_id 0x4B4C5354、完整性ok、外键错误0，见`desktop-notes-deployed-smoke.log/.json`。
- 核对v19合成manifest、路径和所有权后删除旧开发库；v20由最终程序完成13/13项原生校验，含42实体／8清单／6提醒／5附件、6个桌面状态，其中2个打开。已启动`.tools/build-v5-windows/bin/kylinlist.exe --data-dir .tools/preview-data-v20-demo`，PID38312的完整路径、参数、可读原生窗口标题、句柄与`Responding=true`已复查，只有一个应用进程。程序SHA-256为`0080b048591a1e9f7d3e4d78c2535c096d955364cf1fbe3c2fdbda3bdd449cb5`；启动后只读复查仍为42／8／6／5、完整性ok、外键错误0。进程与数据库记录分别为`desktop-notes-preview-process.json`、`desktop-notes-preview-readonly.json`，自动更新关闭，没有读取或修改真实任务库。
- 本轮没有浏览器前端测试、依赖安装、Linux／ARM64构建、麒麟试装、提交、推送、发布或外部部署；沿用Qt5.12.8／C++14和现有CI／包检查／安装启动／标签Release流程。最终视觉、真实中文输入法手势与银河麒麟V10 SP1／飞腾FT-2000 ARM64验收由用户完成；以上Windows自动化不代表目标机已验收。
<!-- previous-verification:2026-11-18-desktop-notes:end -->

### 2026-11-18 前序：空列表项的输入法预编辑标记定位

修复空检查项、无序／有序列表在首字尚未提交时，前置标记随IME光标移动的问题。根因是`cursorRect(QTextCursor(block))`不是固定的段首锚点：Qt5.12会把同位置的预编辑光标偏移计入该矩形。原生诊断中，预输入从1增至8、16个字母时，该矩形x由26变为32、74、122，而块与首行布局原点始终不变。

`EditorTextEdit`新增统一首行布局坐标换算，以`blockBoundingRect + lineAt(0).rect()`和实际文档滚动偏移定位检查框、圆点、序号、引用条与分隔线；横向RTL偏移按Qt5.12的`maximum-value`计算，不引用私有API。检查框绘制与命中共用固定锚点，不冻结预输入光标、不缓存首帧绝对位置；预输入变长、候选光标移动、换行、取消和首字提交均保持行首标记。输入法事件处理、正文格式／序列化、保存与撤销机制未改。

<!-- previous-verification:2026-11-18-editor-ime -->
**最终自动化验证：Windows x64／Qt5.12.8／MinGW GCC7.3／C++14，应用及20个测试目标构建成功。完整离屏CTest 20/20套通过（69.44秒），合计787通过／0失败／6跳过；同一修复构建的20个测试程序以Windows原生逐套串行执行，合计793通过／0失败／0跳过。** 原生按既有19套与即时编辑1套分两批执行，编排用时之和124.96秒，不把离屏结果冒充原生。

| 套件 | offscreen：通过／失败／跳过 | Windows原生：通过／失败／跳过 |
|---|---:|---:|
| tasks | 75／0／0 | 75／0／0 |
| ui | 115／0／3 | 118／0／0 |
| updates | 40／0／0 | 40／0／0 |
| stats | 22／0／0 | 22／0／0 |
| branding | 35／0／0 | 35／0／0 |
| brandicons | 13／0／0 | 13／0／0 |
| database | 47／0／0 | 47／0／0 |
| dropgroups | 35／0／0 | 35／0／0 |
| groupmodel | 29／0／0 | 29／0／0 |
| datepopup | 42／0／0 | 42／0／0 |
| activitytimeline | 12／0／0 | 12／0／0 |
| detail | 90／0／2 | 92／0／0 |
| attachmentcards | 33／0／0 | 33／0／0 |
| editorlinks | 31／0／0 | 31／0／0 |
| editortools | 18／0／0 | 18／0／0 |
| editorinteraction | 50／0／0 | 50／0／0 |
| relateddetail | 28／0／1 | 29／0／0 |
| recurrence | 26／0／0 | 26／0／0 |
| demo | 12／0／0 | 12／0／0 |
| instantmarkdown | 34／0／0 | 34／0／0 |
| **合计** | **787／0／6** | **793／0／0** |

- 新增10条数据行，覆盖未完成／已完成检查框、圆点、多位序号与引用条的深浅主题。实际聚焦后发送带Cursor属性的`QInputMethodEvent`，验证1字母、7字母、预编辑光标移中间、长预输入换行、取消、提交首个汉字与一次撤销；明确断言真实caret已经移动，但标记区域内存像素完全相同、复选框命中矩形不变、源码不含preedit。它是输入法协议层自动化，不替代具体输入法或目标机手工验收。
- 像素用例首次把空文档原生占位文字的正常消失也计入标记带，产生40条重复失败；多位序号（非空文档）及所有检查框几何断言已通过。随后仅在测试fixture中清空占位提示，隔离标记像素，保留严格逐像素相等与原有全部断言；定向离屏12／0／0、完整即时编辑34／0／0均通过，未为消除失败改动产品输入行为。
- 最终150%原生配置`QT_SCALE_FACTOR=1.5`、`KYLINLIST_EXPECT_DPR=1.5`实际校验DPR：即时编辑34、编辑交互50、内链31、格式工具18、关联详情29，合计**162通过／0失败／0跳过（34.52秒）**。前四套0条QWARN，关联详情2条既有QWARN；标准原生共32条QWARN，完整离屏2575条QWARN及16条`FT_New_Face`枚举QDEBUG均保留。离屏6项跳过仍为UI3、详情2、关联详情1的既有平台边界。
- 日志为`.tools/test-results/editor-ime-final-full-offscreen.log`、`editor-ime-final-native-existing-*.log`、`editor-ime-final-native-instant-*.log`与对应JSON；原生20套合并记录为`editor-ime-final-native-summary.json`，保留两份输入摘要及分批用时口径；150%为`editor-ime-final-native-dpr150-*`。没有生成或保存人工验收截图。
- 按开发机约定结束旧PID45292、检查后删除旧可执行文件并重新构建；使用`windeployqt --release --compiler-runtime --no-patchqt`更新本地运行库，保留`qt.conf`。PATH仅含`C:\Windows\System32`且无Qt／KylinList测试变量的独立smoke退出0，空库schema10、application_id 0x4B4C5354、完整性ok、外键错误0，见`editor-ime-deployed-smoke.log/.json`。
- 核对v18合成manifest和路径所有权后删除旧开发库，v19经最终程序原生校验13/13项通过，含42实体／8清单／6提醒／5附件。已启动`.tools/build-v5-windows/bin/kylinlist.exe --data-dir .tools/preview-data-v19-demo`；PID51948的完整路径、参数、可读窗口标题、原生句柄与`Responding=true`已复查，当时只有一个应用进程。启动后只读复查仍为42／8／6／5、完整性ok、外键错误0；未读取或修改真实任务库。
- 本轮没有浏览器前端测试、依赖安装、Linux／ARM64构建、真机试装、提交、推送、发布或外部部署。最终视觉、实际输入法与银河麒麟V10 SP1／飞腾FT-2000 ARM64验收由用户完成。
<!-- previous-verification:2026-11-18-editor-ime:end -->

### 2026-11-18 前序：正文即时格式与检查项交互

正文从“活动行源码、离开后渲染”改为常驻富文本编辑。`MarkdownDocument`集中处理块与行内语义、Markdown读写和UTF-16映射，`MarkdownEditor`保留宿主、格式命令与链接／补全，`EditorTextEdit`继续使用Qt原生输入、IME和撤销。行首`# `至`###### `立即成为标题，包括空标题插入符与随后文字；行内格式闭合或菜单操作当行呈现，光标进出、选区与失焦不再改变字号／行高或展开源码。标题回车后为正文，列表续项与空项退出、检查项续出未完成状态、块首退格还原正文均有明确命令边界。

检查框改为独立绘制和命中，不把☐／☑混进编辑文本；按下／释放命中才切换完整`- [ ]`／`- [x]`，不移动原光标、正反选区和滚动位置。独立编辑器与第四列外层`DetailScroll`均覆盖远处光标／选区下点击别行检查框的稳定性；点击文字仍可编辑，只读不切换。正文仍保存Markdown，不新增检查项实体／表，也不改变`TextDraft`保存与冲突准入。

未修改块保留原文，修改块按可撤销的Qt格式属性写回；准确源码命令不再经过压缩后的可见选区往返。局部投影保留可见光标并阻止纯呈现操作发出伪导航信号，不靠每次`setContent()`重建全文；代码围栏上下文一次线性计算。IME preedit不入正文、不重写组合块，提交后才转换。额外修复链接标签内Delete／Backspace误删整链、链接标签行内样式丢失、代码连续反引号识别与必要定界符调整、格式片段的Markdown MIME复制粘贴，以及“输入→切换检查项→继续输入”的撤销误合组。原生富文本／内置MD4C继续复用，无新增依赖或浏览器运行时；Qt5.12.8、C++14基线不变。竞品调研只读检查了实际DOM与公开发布脚本，原始证据仅留本机忽略目录，未修改真实任务或外发项目源码。

<!-- previous-verification:2026-11-18-instant-editor -->
**最终自动化验证：Windows x64／Qt5.12.8／MinGW GCC7.3／C++14，应用与20个测试目标全部构建成功。最终完整离屏CTest 20/20套通过（71.69秒），合计777通过／0失败／6跳过；同一最终构建的20个程序以Windows原生平台逐套串行直接运行，合计783通过／0失败／0跳过（152.59秒）。** CTest注册平台仍为offscreen，原生数字来自直接运行，不互相替代。

| 套件 | offscreen：通过／失败／跳过 | Windows原生：通过／失败／跳过 |
|---|---:|---:|
| tasks | 75／0／0 | 75／0／0 |
| ui | 115／0／3 | 118／0／0 |
| updates | 40／0／0 | 40／0／0 |
| stats | 22／0／0 | 22／0／0 |
| branding | 35／0／0 | 35／0／0 |
| brandicons | 13／0／0 | 13／0／0 |
| database | 47／0／0 | 47／0／0 |
| dropgroups | 35／0／0 | 35／0／0 |
| groupmodel | 29／0／0 | 29／0／0 |
| datepopup | 42／0／0 | 42／0／0 |
| activitytimeline | 12／0／0 | 12／0／0 |
| detail | 90／0／2 | 92／0／0 |
| attachmentcards | 33／0／0 | 33／0／0 |
| editorlinks | 31／0／0 | 31／0／0 |
| editortools | 18／0／0 | 18／0／0 |
| editorinteraction | 50／0／0 | 50／0／0 |
| relateddetail | 28／0／1 | 29／0／0 |
| recurrence | 26／0／0 | 26／0／0 |
| demo | 12／0／0 | 12／0／0 |
| instantmarkdown | 24／0／0 | 24／0／0 |
| **合计** | **777／0／6** | **783／0／0** |

- 新增`instantmarkdown`覆盖即时标题／当前输入格式、初始富文本的光标与焦点往返／排版稳定、行内闭合后的普通输入与样式内继续编辑、真实检查框双向切换／空项／缩进／大小写X／只读／原选区／两种滚动宿主、列表与标题回车、块首退格、跨样式与跨段编辑、IME提交／取消、粘贴单步撤销重做、数据库保存、链接标签删除与样式、代码反引号、带格式复制粘贴和检查项撤销隔离。鼠标用例先验证可见几何及原生`widgetAt()`命中，文字／光标／块几何与墨迹使用内存断言，不制作人工验收截图。
- 最终二进制以`QT_SCALE_FACTOR=1.5`、`KYLINLIST_EXPECT_DPR=1.5`再次执行：即时编辑24、编辑交互50、内链31、格式工具18、关联详情29，合计**152通过／0失败／0跳过（42.91秒）**；新增即时套件明确核对窗口实际DPR，四套编辑相关程序均0条QWARN，关联详情保留2条既有QWARN。
- 150%首次完整运行曾在复制标题后的原生剪贴板读取出现1项瞬时空值失败；未修改最终程序或削弱断言，同一二进制独立复核该用例3／0／0，随后五套完整复跑152／0／0。首次失败与复跑日志均保留；没有据此宣称已确认系统时序根因。中间实现回归暴露的撤销事务、复选框滚动与保真缺陷均修复后补测；一条链接前边界测试曾把光标放到标签之后，已改为用实际标签`selectionStart()`确定真实文档边界。
- 完整离屏保留2565条QWARN及15条`FT_New_Face`枚举QDEBUG；完整标准原生保留32条QWARN。离屏6项跳过仍为UI3、详情2、关联详情1的既有平台能力边界，原生均实际执行；“通过”不等于“无警告”。首次未配置完整插件目录的编排曾在测试初始化前退出，补齐插件路径后重跑；该编排失败不计作已执行用例。
- 最终日志为`.tools/test-results/instant-editor-final-full-offscreen.log`、`instant-editor-final-full-native-*.log`与对应JSON；150%首次与完整复跑分别为`instant-editor-final-native-dpr150-*.log`和`instant-editor-final-native-dpr150-rerun-*.log`，独立剪贴板复核为`instant-editor-clipboard-dpr150-rerun-*`。调研文件与自动化日志均经`git check-ignore`确认不入库。
- 结束旧预览PID2160并检查后删除旧程序，重新构建最终应用；`windeployqt --release --compiler-runtime --no-patchqt`补齐本地运行库，保留既有`qt.conf`。PATH仅含`C:\Windows\System32`且清除Qt／KylinList测试变量时，隔离`--smoke-test`退出0；空库为schema10、application_id 0x4B4C5354、完整性ok、外键错误0、实体0、系统清单1，见`instant-editor-deployed-smoke.log/.json`。
- 核对v17 manifest确认为受管合成数据后删除旧开发库；v18通过最终程序原生校验13/13项，包含42实体（38任务／4笔记）、8清单、3文件夹、6提醒、5附件与83条活动事件。已启动`.tools/build-v5-windows/bin/kylinlist.exe --data-dir .tools/preview-data-v18-demo`，PID45292的完整路径、参数、可读窗口标题、原生句柄与`Responding=true`已复查，当时只有一个应用进程。启动后只读复查仍为42实体／8清单／6提醒／5附件、完整性ok、外键错误0。自动更新关闭，未读取或修改真实任务库。
- 本轮未执行Linux／ARM64构建、目标机试装、提交、推送、发布或部署到外部；不宣称任意Markdown语法已实现或绝无缺陷。最终视觉、实际手势、中文输入法与银河麒麟V10 SP1／飞腾FT-2000 ARM64验收由用户完成。既有CI、包检查、安装启动与标签Release流程保留。
<!-- previous-verification:2026-11-18-instant-editor:end -->

### 2026-11-17 前序：时间输入、提醒浮层与独立关联窗口

具体时间与全天自定义提醒共用用户文本解析：小时和分钟均接受一位或两位数字，`9:4`、`9:04`、`09:4`均规范为09:04，非法范围与残缺输入仍保留原草稿；规则持久化继续使用`HH:mm`。系统提醒的稍后提醒菜单改用共享`PopupMenu`圆角透明绘制与高斯阴影，主提醒窗顶部可拖动且双击不最大化。第四列详情头部不再创建独立`DetailReminder`按钮，日期芯片、第三列提醒状态及系统提醒调度保持原路径。

正文关联详情已从主窗口子控件改为无父`Qt::Window`：保留共享`PopupShadow`、610×500详情内容与16px圆角，增加最小化／最大化／关闭、顶部拖动、双击最大化和四边四角缩放；最大化取消圆角与透明边距。主窗口不再重置其几何，也不因主窗口Escape、快速添加或普通导航误关。关闭、切换、恢复和退出仍执行原有草稿保存准入，保存失败保留独立窗口与草稿。

用户实测追加发现两个窗口问题并已修复：关联标题栏拖动区曾因垂直Fixed策略得到472×0几何，现固定为28px并以原生`QApplication::widgetAt()`验证真实命中，不再只向零高度控件直接发事件制造通过；稍后提醒则由Qt默认定位改为按可见卡片尺寸决定下方／上方放置，屏幕底部上翻时卡片底缘与按钮上缘保持4px，透明阴影边距不再拉开菜单与点击位置。

<!-- previous-verification:2026-11-17-window-input -->
**最终自动化验证：Windows x64／Qt5.12.8／MinGW GCC7.3／C++14，应用和19个测试目标全部构建成功。完整离屏CTest 19/19套通过（72.27秒），合计753通过／0失败／6跳过；同一构建的19个测试程序以Windows原生平台逐套串行直接运行，合计759通过／0失败／0跳过（编排总用时121.91秒）。**

| 套件 | offscreen：通过／失败／跳过 | Windows原生：通过／失败／跳过 |
|---|---:|---:|
| tasks | 75／0／0 | 75／0／0 |
| ui | 115／0／3 | 118／0／0 |
| updates | 40／0／0 | 40／0／0 |
| stats | 22／0／0 | 22／0／0 |
| branding | 35／0／0 | 35／0／0 |
| brandicons | 13／0／0 | 13／0／0 |
| database | 47／0／0 | 47／0／0 |
| dropgroups | 35／0／0 | 35／0／0 |
| groupmodel | 29／0／0 | 29／0／0 |
| datepopup | 42／0／0 | 42／0／0 |
| activitytimeline | 12／0／0 | 12／0／0 |
| detail | 90／0／2 | 92／0／0 |
| attachmentcards | 33／0／0 | 33／0／0 |
| editorlinks | 31／0／0 | 31／0／0 |
| editortools | 18／0／0 | 18／0／0 |
| editorinteraction | 50／0／0 | 50／0／0 |
| relateddetail | 28／0／1 | 29／0／0 |
| recurrence | 26／0／0 | 26／0／0 |
| demo | 12／0／0 | 12／0／0 |
| **合计** | **753／0／6** | **759／0／0** |

- 相关定向原生回归为日期42、详情92、关联详情29、UI118，合计281／0／0。最终二进制以`QT_SCALE_FACTOR=1.5`、`KYLINLIST_EXPECT_DPR=1.5`运行时，本轮直接相关的日期**42／0／0**、关联详情**29／0／0**、UI**118／0／0**全部通过；日期套件0条QWARN，关联详情2条、UI27条QWARN均保留。
- 额外把完整详情套件也纳入150%检查时，结果为**89通过／3失败／0跳过**：三个失败均为既有附件滚动预算数据行的`empty/short/long-3-files`，实际`attachmentScroll->verticalScrollBar()->maximum()==18`、旧断言预期0。它们不在本轮时间、提醒或关联窗口修改路径，标准原生详情92项与150%本轮提醒入口用例均通过；该额外缩放失败不写成通过，也未为消除数字而改动附件业务或断言。
- 离屏6项跳过仍是既有平台能力边界：UI3、详情2、关联详情1；对应原生程序均实际执行且无跳过。完整离屏保留2528条QWARN及14条`FT_New_Face`枚举QDEBUG，完整标准原生保留39条QWARN；通过不代表无警告。没有生成人工验收截图。
- 最终日志为`.tools/test-results/small-fixes-full-offscreen.log`、`small-fixes-full-offscreen-detailed.log`、`small-fixes-full-native-*.log`及对应JSON汇总；两项追加修复的150%日志为`small-fixes-target-native-dpr150-*.log`，完整详情150%日志为`window-input-target-native-dpr150-detail.log`。中间1项离屏测试命中失败已修正测试入口并单独复跑通过，不计入最终失败数；上述额外150%附件失败则原样保留。
- 使用`windeployqt --release --compiler-runtime --no-patchqt`覆盖运行库，沿用本地`qt.conf`。部署后的最终程序在PATH仅含`C:\Windows\System32`、无Qt／KylinList测试环境变量时，以全新目录执行`--smoke-test`退出0；空库为schema10、`application_id=0x4B4C5354`、完整性ok、外键错误0、实体0、系统清单1。日志为`small-fixes-deployed-smoke.log`及对应JSON。
- 按开发机约定结束旧PID21232并删除旧程序；确认v16 manifest为受管合成数据后删除旧开发库。新版预览库命名为v17，经最终程序原生校验13/13项通过，包含42实体（38任务／4笔记）、8清单、3文件夹、6提醒、5附件、7组重复序列与83条活动事件。最终启动`.tools/build-v5-windows/bin/kylinlist.exe --data-dir .tools/preview-data-v17-demo`，PID2160的路径、参数、可读窗口标题和`Responding=true`均已核对，当时只有一个应用进程；启动后只读复查仍为42实体／8清单／6提醒／5附件、完整性ok、外键错误0。未读取或修改真实任务库。
- 本轮未执行Linux／ARM64构建、目标机试装、提交、推送、发布或部署到外部；既有CI、包检查、安装启动与标签Release流程保留。最终视觉、实际手势与银河麒麟V10 SP1／飞腾FT-2000 ARM64验收由用户完成。
<!-- previous-verification:2026-11-17-window-input:end -->

### 2026-11-17 前序：日期选择器结构与展开交互重做

按当前滴答网页的 DOM、computed style 和实际展开路径重组日期组件：时间列表、范围开始／结束月历与结束重复月历在父卡片内展开；提醒和重复在菜单页与独立自定义页之间切换。提醒使用252px卡片、34px菜单行、Primary文字与右侧18px对勾，取消／确定为24px高；不再把多种自定义表单常驻在菜单底部。单日清空时间后回车移除时刻，范围仍保留任意精确分钟输入。重复按基准与频率显示对应工作区，自选日期改用多选月历；独立结束条件在更改频率后保留，已有规则未编辑直接确认保留完整规格，包括农历、年度月末与周起点。持续提醒、法定节假日／调休与记忆法仍不在本轮新增业务范围。

父卡片在内容超过可用高度时滚动主体、固定保留清除／确定；关闭范围月历会恢复自然315px高度。统一逐层Escape、焦点归还、三级下拉级联关闭、未确认子层回滚与父最终确定的保存边界。最近提醒只保存规则，定时／全天分开，最终父确认才记录、去重且最多3条；保留多提醒UUID、旧绝对提醒、UTC／实体时区与定时／全天双草稿。上一处“已归档的清单”与最后清单行重叠也已修复：归档标题使用独立不可选分区行，不再复用清单item。详细行为见[当前UI契约](ui-alignment-contract.md#2026-11-17-当前规格范围调度设置与四列细节)。

<!-- previous-verification:2026-11-17-date-picker -->
**最终自动化验证：Windows x64／Qt5.12.8／MinGW GCC7.3／C++14，应用和19个测试目标全部构建成功。完整离屏CTest 19/19套通过（74.13秒），合计750通过／0失败／6跳过；同一构建的19个测试程序以Windows原生平台逐套串行直接运行，合计756通过／0失败／0跳过（编排总用时127.00秒）。** CTest注册环境固定为offscreen，原生结果来自直接运行测试程序。

| 套件 | offscreen：通过／失败／跳过 | Windows原生：通过／失败／跳过 |
|---|---:|---:|
| tasks | 75／0／0 | 75／0／0 |
| ui | 114／0／3 | 117／0／0 |
| updates | 40／0／0 | 40／0／0 |
| stats | 22／0／0 | 22／0／0 |
| branding | 35／0／0 | 35／0／0 |
| brandicons | 13／0／0 | 13／0／0 |
| database | 47／0／0 | 47／0／0 |
| dropgroups | 35／0／0 | 35／0／0 |
| groupmodel | 29／0／0 | 29／0／0 |
| datepopup | 41／0／0 | 41／0／0 |
| activitytimeline | 12／0／0 | 12／0／0 |
| detail | 90／0／2 | 92／0／0 |
| attachmentcards | 33／0／0 | 33／0／0 |
| editorlinks | 31／0／0 | 31／0／0 |
| editortools | 18／0／0 | 18／0／0 |
| editorinteraction | 50／0／0 | 50／0／0 |
| relateddetail | 27／0／1 | 28／0／0 |
| recurrence | 26／0／0 | 26／0／0 |
| demo | 12／0／0 | 12／0／0 |
| **合计** | **750／0／6** | **756／0／0** |

- 最终二进制另以`QT_SCALE_FACTOR=1.5`、`KYLINLIST_EXPECT_DPR=1.5`运行完整日期套件：**41通过／0失败／0跳过，17.61秒，0条QWARN**。包括内嵌位置与高度、真实菜单选中墨迹、三级Escape、父关闭链、提醒与重复提交／取消、月末与农历、精确分钟、UTC／DST及控制器原子保存。默认原生日期41项同样0条QWARN；截图输出环境变量未设置，没有生成人工验收截图。
- 离屏6项跳过仍是既有平台能力边界：UI3、详情2、关联详情1；对应原生程序均实际执行且无跳过。**通过不代表无警告**：完整离屏保留2482条QWARN，主要为不支持`propagateSizeHints`／`raise()`／键盘抓取，另有既有光标越界、空action、QSS参数、品牌负例、开发安装身份与附件篡改负例提示；有14条`FT_New_Face`枚举QDEBUG。完整标准原生保留39条既有QWARN，未屏蔽输出。
- 回归中实际修复了范围月历收起后旧最小高度残留、正常状态多出滚动轨道、子卡片复用时嵌套布局尺寸缓存、单选日期重复点击丢高亮、日期结束切Never／Count残留旧结束日期、菜单行回车／方向键及独立结束条件选中态。原生初轮3条失败另包含测试在旧弹窗关闭消息处理前创建新弹窗、目标尚未滚入视口的问题；加入事件清理、原生显示／激活等待与可见命中前置后，定向与完整原生回归均通过。旧测试对隐藏表单、左侧复选框和独立时间窗口的假设已移除，保留原有调度、时区、提醒UUID与写库覆盖。
- 本机最终离屏详细日志为`.tools/test-results/date-picker-rebuild-full-offscreen-detailed.log`，CTest输出为`date-picker-rebuild-full-offscreen.log`；原生分套日志为`date-picker-rebuild-full-native-*.log`，JSON汇总为`date-picker-rebuild-full-native-summary.json`；150%日志为`date-picker-rebuild-native-dpr150-datepopup.log`。中间失败日志单独保留；未选中用例的编排结果不计为通过。
- 使用`windeployqt --release --compiler-runtime --no-patchqt`覆盖运行库，并配置本地`qt.conf`。部署后的最终程序在PATH仅含Windows系统目录、无Qt／MinGW目录且不带测试Qt环境变量时，隔离`--smoke-test`退出0；新空库为schema10、`application_id=0x4B4C5354`、完整性ok、外键错误0、实体0、系统清单1。日志为`date-picker-rebuild-deployed-smoke.log`及对应JSON。
- 按开发机约定结束旧PID51892、核实合成manifest后删除旧v14开发库和旧程序文件，再构建覆盖并启动最终`.tools/build-v5-windows/bin/kylinlist.exe --data-dir .tools/preview-data-v15-demo`。v15经最终程序原生校验13/13项通过，含42实体（38任务／4笔记）、8清单、3文件夹、6提醒、5附件、7组重复序列、6个桌面状态与83条活动事件。启动后只读复查仍为42实体／8清单／6提醒／5附件、完整性ok、外键错误0；PID48396的路径与参数正确、窗口标题可读、`Responding=true`，当时只有一个应用进程。该预览使用隔离设置并关闭自动更新；没有读取或修改真实任务库。
- 本轮未执行Linux／ARM64构建、目标机试装、提交、推送、发布或部署到外部；既有CI、包检查、安装启动与标签Release流程保留。最终视觉、实际手势与银河麒麟／飞腾ARM64验收由用户完成，以上Windows自动化不能替代。
<!-- previous-verification:2026-11-17-date-picker:end -->

### 2026-11-17 前序：完整调度、设置与四列对齐

以下保留前序二进制的实现与验证记录；日期组件结构、当前测试数和预览状态以上方最新条目为准。

本轮新增单日／时间段、多正常提醒、完整自定义重复、笔记提醒、任务优先级旗子与直接转换反馈；设置改为双栏并提供离线字体，四列几何、导航色点、归档文件夹样式和共享层级清单选择器按网页代码证据调整。后续实测验日期／时间段重排：普通卡片不再被隐藏范围页撑高，开始／结束使用独立日期子弹层，全天改为开关，入口文字与图标按墨迹中线对齐；清单搜索图标与行图标同轴，弹窗按可见卡片锚定。再次实测修正设置212px重复左偏移，简单单值下拉统一`MenuComboBox`并消除三种字体下的裁字；日期／清单按当前实际文字墨迹对齐，分段采用30px轨道内26px激活块，父卡片14px／子层12px圆角与共享18px高斯阴影。最新将日期“时间／提醒／重复”入口收窄为252×34px，在260px卡片内左右各留4px，悬浮圆角8px；第三列提醒、附件与尾部笔记使用一致的14×14px光学图形并与复选框、清单图标共用y=21中线，尾部笔记单独映射`item-note`。本轮继续按网页DOM、computed style与实际点击行为对齐：四个日期快捷按钮及图标改为26×26px；未选中的今天使用PrimarySoft淡圆底与Primary文字；浅色卡片取消额外1px边框，底部按钮统一8px圆角；时间段95×28px时刻框补齐语义边框，保留48个半小时选项及任意HH:mm输入。提醒父入口改为按预设顺序显示真实摘要，全天默认09:00在摘要省略；提醒子层增加取消／确定，取消、Escape和父层关闭恢复含UUID的打开前快照；定时／全天分别保留草稿并可在往返切换后恢复，最终只提交当前模式有效规则。数据直接重建为schema10，备份仅当前v6；不做旧库迁移、持续提醒、法定节假日或艾宾浩斯记忆法。详细契约见[数据模型](data-model.md)和[UI设计](ui-design.md)。可重复合成数据的生成、校验与安全重建见[演示数据指南](testing-demo-data.md)。

<!-- previous-verification:2026-11-17-reminders -->
**自动化验证完成：Windows x64／Qt 5.12.8／MinGW GCC 7.3／C++14 全部应用与19个测试目标构建成功。完整离屏 CTest 19/19 套通过（64.34 秒）；最终二进制的19个CTest注册测试程序又逐一以 Windows 原生平台直接运行，标准原生配置均为 0 失败、0 跳过。** CTest 注册属性固定为 offscreen，不能把设置外部环境变量后的第二次 CTest 记作原生；原生结果来自逐套直接执行，两类日志分别保存，不互相替代。

| 套件 | offscreen：通过／失败／跳过 | Windows 原生：通过／失败／跳过 |
|---|---:|---:|
| tasks | 75／0／0 | 75／0／0 |
| ui | 114／0／3 | 117／0／0 |
| updates | 40／0／0 | 40／0／0 |
| stats | 22／0／0 | 22／0／0 |
| branding | 35／0／0 | 35／0／0 |
| brandicons | 13／0／0 | 13／0／0 |
| database | 47／0／0 | 47／0／0 |
| dropgroups | 35／0／0 | 35／0／0 |
| groupmodel | 29／0／0 | 29／0／0 |
| datepopup | 33／0／0 | 33／0／0 |
| activitytimeline | 12／0／0 | 12／0／0 |
| detail | 90／0／2 | 92／0／0 |
| attachmentcards | 33／0／0 | 33／0／0 |
| editorlinks | 31／0／0 | 31／0／0 |
| editortools | 18／0／0 | 18／0／0 |
| editorinteraction | 50／0／0 | 50／0／0 |
| relateddetail | 27／0／1 | 28／0／0 |
| recurrence | 26／0／0 | 26／0／0 |
| demo | 12／0／0 | 12／0／0 |
| **合计** | **742／0／6** | **748／0／0** |

- 离屏6项跳过是既有原生窗口／浮层与真实对话框能力边界：UI 3、详情2、关联详情1；相应原生套件均实际执行且无跳过。完整离屏日志为`.tools/test-results/date-reminder-full-offscreen-detailed.log`，CTest摘要为`date-reminder-full-offscreen.log`；19套标准原生日志为`date-reminder-full-native-*.log`，汇总为`date-reminder-full-native-summary.log`。标准原生配置不生成人工验收截图。最终二进制另以`QT_SCALE_FACTOR=1.5`和`KYLINLIST_EXPECT_DPR=1.5`运行日期全套33项，包含快捷图标、今天淡底、边框、提醒交互和精确分钟输入，0失败／0跳过，日志为`date-reminder-native-dpr150-datepopup.log`。
- 上一轮原生批量编排首轮因子进程PATH漏掉Qt／MinGW运行库目录，19个进程均在QtTest初始化前以`0xc0000135`退出且无测试输出；这是启动器环境失败，不计作产品断言失败。补齐PATH后先单独通过`test_demo`，再完整重跑得到当时的742／0／0。事件记录为`demo-full-native-loader-failure-summary.log`；早期空输出已被同名成功日志覆盖，没有伪称为原始失败日志。
- **0失败不等于无警告**：最终完整离屏日志有1678条QWARN，其中475条为`propagateSizeHints`、668条为不支持`raise()`、496条为不支持键盘抓取，另有7条既有`QTextCursor::setPosition`越界、9条空action及负例／开发安装身份等输出；新增篡改附件负例有1条`QFSFileEngine::open`空文件名提示；另有14条`FT_New_Face`字体枚举QDEBUG。新增提醒／时间交互增加了离屏平台不支持窗口操作的提示，均保留。最终19套标准原生记录共39条QWARN，包含同类7条光标警告、9条空action、既有QSS参数提示、品牌／图标故意负例、开发安装身份提示及1条附件篡改负例提示，未屏蔽输出。
- 两个内置界面字体均通过Qt5.12注册、常规／两个字重请求和中文字形检查；正文切换字体保留内容、选区及可撤销历史。Qt5.12的`QRawFont::styleName()`可能为空，霞鹜文楷 Medium也可作为独立字族匹配，因此断言检查实际注册与字形，不错误要求平台匹配后的族名完全相同。
- 新增回归覆盖重复序列revision CAS、同毫秒创建实例的最终完成撤销、中间实例永久删除后的撤销保护、夏令时回拨UTC端点保持、范围筛选与互斥Time分桶分离、跨天／全天范围拖放，以及归档导航重建后旧item widget销毁。本轮另覆盖普通539px／时间段315px自然卡片、独立端点月历230×252与8px锚定、30×18全天开关、错误文本按宽换行、入口真实墨迹中线，以及清单搜索图标同轴、GroupChip上方8px和文件夹动作／旧子菜单生命周期。最新补充设置x=0／212坐标、三种字体下24／30px下拉完整墨迹、日期三入口／顶部日期／GroupChip／清单菜单约1px中线、30px轨道内26px分段激活块、父14／子12圆角与24px共享阴影边距；相关离屏与原生用例均实际执行。最新三入口进一步验证252×34px、左右4px悬浮留白及8px圆角，行状态在DPR1／2下验证真实墨迹中线与`item-note`光学尺寸。本轮再新增6项日期回归：26px快捷按钮及22px左右留白、今天淡底与选中覆盖、浅色卡片／底部／范围时刻边框左右一致、范围起止09:07／10:13精确分钟及非法输入、提醒摘要顺序与取消／确定回滚、普通单日及时间段定时⇄全天草稿恢复；绝对提醒摘要也显示实际本地日期／时刻。新增`demo`套件覆盖合成库生成后重开校验、非空目录／根路径／实例锁拒绝、非演示目录零写入、manifest所有权、附件篡改检测和备份容器；脚本另以真实程序验证seed／verify／reset安全重建。
- 新增可复用的`--seed-demo`／`--verify-demo`和`tools/generate_test_data.py`，全部业务数据通过`TaskStore`写入；覆盖文件夹、彩色清单、任务／笔记、全部优先级、日程与多提醒、六种重复及后继实例、层级、Markdown内链、真实附件、完成／放弃／回收站、归档、六色三层便签与自然活动事件。生成时导出并校验KLB备份；`--reset`必须先通过所有权和原生校验，再把旧目录改名为时间戳隔离副本并原位重建，不递归删除目录。正常应用启动不会自动生成演示数据。
- 原生`--smoke-test --data-dir <全新空目录>`退出0并输出初始化／SQLite成功。只读检查合成新库：`application_id=0x4B4C5354`、schema10、`integrity_check=ok`、外键错误0、实体0、唯一系统收集箱1；未使用真实任务数据，未打开或改写旧预览库。
- 最新对齐预览已启动：最终构建`.tools/build-v5-windows/bin/kylinlist.exe`使用全新隔离`.tools/preview-data-v14-demo`运行。项目脚本实际生成42个实体（38任务／4笔记）、8清单、3文件夹、6提醒、5附件、7组重复序列与83条自然活动事件，二次原生验证13／13项通过。启动后只读复查数据库仍有42实体、8清单、5附件、6提醒，完整性与外键检查正常。预览PID 51892的可执行路径与`--data-dir`均已核对，窗口标题可读且`Responding=true`。完整构建前已结束旧预览PID 14852并删除旧`preview-data-v13-demo`开发库；`windeployqt`已部署Qt及MinGW运行库，启动子进程未继承Qt／KylinList测试环境变量。旧崩溃转储保留用于对照，未读取或改写真实用户库。
- **快速创建后标题输入法崩溃的根因修复**：第三份Windows转储`kylinlist.exe.49872.dmp`与前两份同为`0xc00000fd`、同一`Qt5Gui.dll+0x43e4ab`故障点。按PE代码段过滤栈后，还原出388次重复周期：标题`textEdited`→`TaskDetailWidget::dirty()`→`setStatus()`→`refreshIcons()`／`updateParentLink()`／Store查询→Qt事件与UIAutomationCore再入。上轮`setStatus`先把提示写成“信息”、`refreshIcons`又对任务改成“优先级”，导致幂等判断永远失败；正文FocusIn延时并未消除这条标题路径。本轮让状态条更新不再刷新整页，统一任务／笔记正常提示，图标刷新保留错误提示，实体加载仍显式刷新图标；正文FocusIn延时保留。新增回归在修复前以连续12次标题编辑稳定产生84次无关控件事件，修复后为0，并覆盖错误恢复及任务／笔记切换。Windows UIA与真实输入法组合无法由这些QtTest复现；随后用户按“快速创建→回车→标题持续输入→唤起输入法”原路径实测，明确确认BUG成功修复。
- **可选截图诊断另有失败，不计入上述标准原生通过数**：显式设置`KYLINLIST_SCREENSHOT_DIR`后，`UiTest::visualPreview()`在保存窄详情图前的`narrow.width() <= 380`断言失败；完整套件与独立用例各复现一次，日志为`native-crash-fix-v2-ui.log`和`native-crash-fix-v2-ui-visual-rerun.log`。未设置截图目录的标准原生UI为117/117通过。该失败不冒充视觉验收，也未被绿色结果隐藏。
- 本轮未执行Linux／ARM64构建、包安装、发布或完整多DPI显示矩阵，未提交／推送；仅完成上述150%关键用例。现有CI、包检查、安装启动和`v*`标签Release流程保留但未触发。用户亲自进行视觉、实际手势及银河麒麟／飞腾ARM64真机验收；上述Windows自动化不能替代。

### 2026-11-16 历史：导航入口、透明品牌与关联详情卡片

本节是**历史**实现与验证记录；下方同日正文／附件条目也为前序历史，不用旧通过数替代当前结果。当时数据库为schema9、备份为5；这些版本号仅用于追溯，已由文首schema10／备份6的开发期重建契约取代。

- 第一列Logo与第二至第四列首行共用相对列顶52px中线，32px Logo顶边距改为36px；功能入口仅任务、同步与数据、设置，采用网页实测40px按钮／28px图标，任务使用对应list-sidebar，激活只显示Primary蓝色而无背景块。第三列全局搜索框、第一列搜索按钮／Ctrl+F与第三个无用笔记按钮移除，底层ViewSpec搜索字段保留给后续完整搜索。
- 同步菜单承接导出备份、覆盖导入和打开数据位置；设置对话框新增默认外观页，主题三态仍由ThemeManager持久化，智能清单设置保留为第二页。Logo仅保留自启、使用统计、更新与退出等工作区动作。窗口按钮维持46×28命中，激活背景四边内缩1px，透明边框overlay最后叠放，避免吞边框。
- 第一列首行使用原始透明品牌Logo，保留32px入口及本机工作区菜单；窗口、任务栏、托盘使用同图的等比白底版本，背景取固定白色语义令牌`AppIconBg`，不改写原图。无配置统一默认品牌回退，不把私人名称／配置／图片嵌入仓库，也不扩展安装菜单资源或产品身份。
- 正文加号只在自身聚焦、可编辑、无选区／IME预编辑且光标位于真正空源码普通段落时出现；不随鼠标换行，空白字符、代码围栏／分隔线不算空行。按实际字体与QTextLayout基线定位，NoFocus且所有交互态无背景。底栏清单按钮复合绘制，18px图标、正文级文字、4px间距共用字体中线，保留省略与选择入口。
- 正文关联打开透明cover上的610×500px参考卡片，16px圆角、共享整图高斯阴影与独立边框overlay；空间不足收缩。独立实例复用完整`TaskDetailWidget`，不重挂原第四列；同目标不重置、循环关联复用单卡片。原筛选、导航、选中、滚动和撤销保留；便签正文同路由，主任务／创建结果／便签标题栏仍走原导航。
- 关闭、换目标与退出前检查文字及会话保存；外部点击消费且不穿透，Logo区域同样由cover统一消费；Esc先交给日期、格式、补全、IME及编辑器工具。保存失败保留卡片与草稿；备份恢复失败解冻保留上下文，成功移除旧上下文，Store销毁后不继续提交。
- `TextDraft`字段三方提交允许不同字段合并，同字段冲突整份零写，目标不存在／回收站不可用，不以旧草稿恢复实体。UI提供复制、确认丢弃重载与Ctrl+S重试；同步观察者的新草稿不被旧成功结果清掉。编辑会话取消只结清自己的已保存字段，日志失败保留会话，成功终结在对外信号前完成。便签文字与桌面状态分开，纯文字自动保存不触碰未改动的默认窗口几何。
- 正文缓存仅在关联目标标题／可用性改变时失效；真实键盘输入使用Qt原生编辑事务并按源码位置合并连续输入，避免呈现操作变成单独的无文字变化撤销项。真实关联打开、关闭后的单次撤销与重做均有回归。

<!-- historical-verification:2026-11-16 -->
**该历史轮次的自动化验证：Windows x64／Qt5.12.8／GCC7.3／C++14完整构建成功，CTest 17/17套通过（58.61秒）；当时相关原生Windows 11/11套均有对应二进制的最终通过记录，合计431个QtTest报告项，0失败／0跳过。** 这不是人工视觉或目标机验收，也不是本轮schema10的通过记录。

- 常规构建目录为`.tools/build-v5-windows`，名称中的v5为历史目录名，不是数据库版本。实际运行`ctest --test-dir .tools/build-v5-windows --output-on-failure`，CTest显式使用offscreen；完整日志为`.tools/build-v5-windows/Testing/Temporary/LastTest.log`。
- 原生套件显式设置`QT_QPA_PLATFORM=windows`并严格串行；无截图输出目录。当前二进制的最终分套通过日志与重跑来源汇总位于本机忽略目录`.tools/native-navigation-verified-q90HUc/`，不将运行日志或私人素材入库。复验过程中editorinteraction／relateddetail等曾在桌面窗口状态变化时出现间歇性原生激活／命中失败，失败中间日志保留；editortools、editorinteraction、relateddetail分别独立重跑后18／50／28项全过，未用较早一轮结果替代当前构建。

| 套件 | CTest passed／failed／skipped | 原生Windows passed／failed／skipped |
|---|---|---|
| tasks | 56／0／0 | 未重复执行 |
| ui | 110／0／4 | 114／0／0 |
| updates | 40／0／0 | 未重复执行 |
| stats | 22／0／0 | 未重复执行 |
| branding | 35／0／0 | 35／0／0 |
| brandicons | 13／0／0 | 13／0／0 |
| database | 45／0／0 | 未重复执行 |
| dropgroups | 35／0／0 | 未重复执行 |
| groupmodel | 29／0／0 | 未重复执行 |
| datepopup | 9／0／0 | 9／0／0 |
| activitytimeline | 12／0／0 | 12／0／0 |
| detail | 85／0／3 | 88／0／0 |
| attachmentcards | 33／0／0 | 33／0／0 |
| editorlinks | 31／0／0 | 31／0／0 |
| editortools | 18／0／0 | 18／0／0 |
| editorinteraction | 50／0／0 | 50／0／0 |
| relateddetail | 27／0／1 | 28／0／0 |

- offscreen共8项明确能力跳过：既有7项原生窗口／浮层／真实QMessageBox前置，以及关联卡片原生子控件合成／层级／圆角阴影像素项1项。全部在对应原生套件执行，无原生跳过；不把offscreen等同窗口合成验证。浅、深背景合成在内存中核对预乘alpha与SourceOver，不保存人工验收截图。
- **通过不等于无警告**：原生ui保留7条既有`QTextCursor::setPosition`越界警告，另有1条开发环境缺安装身份提示；relateddetail有2条同类安装身份提示。branding的12条、brandicons的1条为损坏配置／图片等故意负例。offscreen另有不支持raise／键盘抓取／size hints等平台警告，未隐藏。
- 独立预览`.tools/build-v9-preview`以同一Qt／GCC／C++14工具链重建成功。原生隔离`--smoke-test`退出0、输出初始化／SQLite成功，合成空库application_id为`0x4B4C5354`、integrity_check为ok、外键错误0、实体0；未用真实任务数据作测试。确认旧预览已退出后，沿用原本机启动器和原v9目录启动新版，独立复查路径、品牌标题及窗口响应，不重置数据、不强杀进程。
- 本轮未执行Linux／ARM64构建、包安装、发布或多DPI显示矩阵；内存1x／2x绘制断言不冒充系统缩放实测。Qt5.12.8／C++14及Ubuntu20.04容器基线不变，既有CI、包检查、安装启动和v*标签Release流程保留。人工视觉、实际手势和银河麒麟／飞腾ARM64仍由用户验收。
<!-- previous-verification:2026-11-17-reminders:end -->

### 2026-11-16 前序轮次：正文编辑、底部附件与 fresh-only 数据边界

以下保留同日前序一轮的实现与真实验证。与上方最新规格冲突时以上方为准，原通过数仅用于历史追溯。完整规格见[UI 设计](ui-design.md)、[UI 对齐契约](ui-alignment-contract.md)、[数据模型](data-model.md)与[实施指引](data-model-refactor.md)。

- 第四列附件移至底栏上方独立面板，左右24px、空时整区隐藏，有独立有界滚动；高度同时受最多三张卡片、可用高度1/3及正文至少120px预算约束。标题／正文／可关闭动态仍在DetailScroll，空和短正文都由真实编辑viewport填满余白，不靠透明点击层或背景转发。
- 正文使用真实 gutter、块旁悬浮加号和块菜单；支持段落、三级标题、无序／有序列表、Markdown任务列表、引用、分割线。复选框只切换 `- [ ]`／`- [x]` 源码、参与正文撤销与会话，不恢复独立检查项表、UUID或进度模型。
- 选区岛32px、底栏格式岛36px（常规宽304px），18px glyph、10px圆角；选区按钮24px与上下实际4px布局边距，字形上下约7px留白。文本类型下拉与七个主格式使用统一动作API；更多／色板／类型均为共享PopupMenu的34px行、4px行外缩进、12px圆角。常规更多顺序为附件、链接、关联任务/笔记、引用、代码、标准分隔、清除格式；窄宽动作折叠入同一纵向菜单，不生成第二条横向岛，不保留隐藏／屏幕外格式替身控件。
- PopupMenu、AnchoredListPopup与格式卡片共用PopupShadow的整图高斯阴影，保留既有菜单／日期子层默认外观；透明边距不裁阴影、禁用原生系统阴影，不以graphics effect或mask替代几何。
- 内部引用仅为 `[[UUID|literalalias]]`／`[[UUID]]`：显式别名字面保留，无别名才显示目标当前标题，内部导航href为裸UUID。旧 `kl:` 解析、双读和迁移回退删除；外部链接仍按支持的普通Markdown地址处理。
- 关联任务／笔记为非模态标题**包含**补全，IME preedit不误确认，Esc只取消候选、不改查询。链接悬浮提供复制／编辑，编辑弹层仅“文本”“链接”两字段，确认才更新源码。
- 格式、块、补全与链接操作统一保存绝对UTF-16源偏移＋contextGeneration，跨FocusOut恢复映射；自有弹层通过markdownEditor／ownsPopup协作，不能因焦点交接丢选区或误结束会话。setContent／setStore／setEntityId之后旧弹层和迟到动作不得写入新实体／新正文。
- 当前数据库为**schema9，fresh-only**：空库直接建立、当前正确标识v9可重开；旧v5–v8／错误标识／未知或未来版本在任何写入／WAL切换前拒绝，不升级、不转正文、不生成升级兼容副本、不回退旧产品库。备份仅导出／导入当前元数据v5，旧版本与旧检查项表示在动库／附件前拒绝，无旧codec转换；镜像覆盖、附件原子交换与失败回滚等当前能力保留。数据库9／备份5／更新清单1是三个独立版本。
- 名称与logo的本机覆盖只通过外部 `Branding`；私有显示名、配置路径和logo不进入受跟踪文件。既有 `docs/research/` 与无关工作保留。
- 测试入口使用 `database` 取代 `migrations`，新增 `editorlinks`、`editortools`、`editorinteraction`；文件与目标均已补齐并实际构建，最终执行结果见下方验证记录。

<!-- previous-verification:2026-11-16-editor -->
**自动化验证完成：Windows x64／Qt 5.12.8／GCC 7.3／C++14，最终 CTest 15/15 套通过（54.71 秒）；相关原生 Windows 9/9 套通过，均为 0 失败、0 跳过。** 人工视觉与目标机验收不包含在此结论中。

- 测试在 `.tools/build-v5-windows` 构建，目录名中的 v5 是历史名称，不是当前数据库版本。实际运行 `ctest --test-dir .tools/build-v5-windows --output-on-failure -j 1`；CTest 注册的窗口平台为 `offscreen`。最终日志为 `.tools/build-v5-windows/Testing/Temporary/LastTest.log`。
- 原生补测显式设置 `QT_QPA_PLATFORM=windows`、现有 Qt 插件路径与字体路径，以下九套严格串行执行；没有设置截图输出目录。最终日志目录为 `.tools/native-editor-complete-lza34ms9/`。下表数字为 QtTest 的 passed／failed／skipped，未执行的原生套件明确留空。

| 套件 | CTest | 原生 Windows |
|---|---|---|
| tasks | 53／0／0 | 未重复执行 |
| ui | 108／0／4 | 112／0／0 |
| updates | 40／0／0 | 未重复执行 |
| stats | 22／0／0 | 未重复执行 |
| branding | 35／0／0 | 35／0／0 |
| database | 45／0／0 | 未重复执行 |
| dropgroups | 35／0／0 | 未重复执行 |
| groupmodel | 29／0／0 | 未重复执行 |
| datepopup | 9／0／0 | 9／0／0 |
| activitytimeline | 12／0／0 | 12／0／0 |
| detail | 81／0／3 | 84／0／0 |
| attachmentcards | 33／0／0 | 33／0／0 |
| editorlinks | 31／0／0 | 31／0／0 |
| editortools | 18／0／0 | 18／0／0 |
| editorinteraction | 33／0／0 | 33／0／0 |

- offscreen 的 7 项跳过没有扩大：3 项依赖原生窗口／浮层前置，4 项是 Qt5.12 Windows `QMessageBox` 的 `SC_CLOSE` Show hook 在缺少 native interface 时不能安全执行；相关原生套件均实际执行且无跳过。本轮新增的正文、附件、工具岛、gutter、补全、IME、链接编辑／复制、源码剪贴板与一次撤销用例均执行。
- 原生首次进入只收到 `Enter`、鼠标移动异步送达和窗口激活时序已覆盖；测试等待真实事件与实际命中，不改为合成 hover 或直接触发动作。`editorinteraction` 在收尾前另连续执行三次，每次 33／0／0。时间线的先后顺序样本保证毫秒时间不同，另补同刻事件按 UUID 稳定排序的用例，不依赖偶然的数据库执行速度。
- 主程序已在上述常规构建目录重建成功，另外以同一工具链在 `.tools/build-v9-preview` 构建 `kylinlist` 用于独立预览；原生隔离 `--smoke-test` 退出 0，并输出初始化／SQLite 成功标记。其合成空库 `application_id=0x4B4C5354`、schema9、`integrity_check=ok`、无外键错误、无实体；不使用真实任务数据或旧开发库。
- **0 失败不等于无警告**：`ui` 的既有格式撤销／重选用例仍有 7 条 `QTextCursor::setPosition` 越界警告，断言通过；开发测试还输出缺少安装身份文件提示。`branding` 的 12 条警告来自故意损坏配置／图片、无效字段与桌面覆盖拒绝等负例；offscreen 另有不支持 raise／键盘抓取等平台警告。没有屏蔽这些输出。
- 本轮不生成人工验收截图，不执行多 DPI 矩阵、Linux／ARM64 构建、包安装或发布。现有 CI／包架构与依赖检查／安装启动／标签 Release 流程保留但未触发；原生视觉、实际手势和银河麒麟／飞腾 ARM64 真机仍由用户验收，不把 Windows 自动化等同真机通过。
<!-- previous-verification:end -->

## 目标与边界

- 更新服务器部署在用户自己的局域网内网宿主机上，Docker Compose 管理；**纯 HTTP，无 TLS、无证书、无发布签名**。SHA-256 只用于下载完整性校验，不承担来源认证；网络边界由内网可信性保证，服务不得暴露公网。
- 客户端始终本地优先：断网、服务器维护或检查失败不影响本地任务与清单的任何功能。
- 未来的账号与清单同步是**独立**的业务接口（`/api/v1/`），不复用更新协议；更新服务不依赖登录。客户端中 `UpdateService` 与未来的同步服务各自维护状态。
- 首版不做：长连接推送、后台强制安装、差分更新、管理后台、跨安装形式（系统 Qt ↔ bundle、安装版 ↔ 便携版）的自动切换。

## 组成

```mermaid
flowchart LR
    CI[三构建任务 / 六产物 GitHub CI] --> G[generate-update-manifest.py] --> R[完整 CI release]
    R --> P[投递到隐藏暂存目录] --> M[原子改名] --> S[data/incoming]
    S --> PUB[publisher watch 自动校验发布] --> F[data/public]
    F --> NG[nginx http 容器]
    K[KylinList 客户端] -->|HTTP| NG
    NG -. 未来 .-> API[/api/v1/ 账号与同步/]
```

- `server/`：Compose 定义、nginx 配置与发布工具，部署与命令见 [server/README.md](../server/README.md)；`server/site/` 是同源产品首屏，直接读取 stable 清单提供下载与完整更新日志，客户端与网站的品牌覆盖见[品牌说明](branding.md)。
- `server/stats/`：聚合统计后端，按真实 IP 保存最新使用快照，并从 nginx 下载日志统计各版本实际 GET/200 次数；协议与隐私边界见 [stats.md](stats.md)。
- `kylinlist-deploy/`：生产自包含离线部署目录，包含版本化镜像 tar、宿主 nginx、安装包和发布配置；tar 与运行数据不入库。
- `packaging/generate-update-manifest.py`：CI 在三目标构建通过后生成发布目录（含 manifest 与安装包）。
- 客户端：`src/app/updates/`（身份、协议、服务）、`src/platform/UpdateInstaller.cpp`（安装交接）、`src/ui/UpdateDialog.cpp`（界面）、`KylinListApp` 协调保存与安装。

## 更新清单协议 v1

入口固定为 `<基础地址>/updates/v1/stable/manifest.json`；安装包地址为 `<基础地址>/updates/` + `artifacts[].path`。

```json
{
  "schemaVersion": 1,
  "channel": "stable",
  "version": "1.2.0",
  "publishedAt": "2026-11-13T08:00:00Z",
  "notes": "更新说明",
  "history": [
    {"version": "1.1.0", "publishedAt": "2026-11-12T08:00:00Z", "notes": "上一版公告"},
    {"version": "1.2.0", "publishedAt": "2026-11-13T08:00:00Z", "notes": "更新说明"}
  ],
  "artifacts": [
    {"platform": "linux", "arch": "arm64", "variant": "system-qt",
     "packageVersion": "1.2.0",
     "path": "files/1.2.0/linux-arm64-system-qt/kylinlist_1.2.0_arm64.deb",
     "size": 591548, "sha256": "<64位小写hex>"}
  ]
}
```

规则：

- `platform` ∈ linux/windows；`arch` ∈ arm64/amd64；`variant`：linux 为 `system-qt`/`bundled-qt`，windows 为 `setup`/`portable`。`(platform, arch, variant)` 不得重复。
- `path` 形如 `files/<版本>/<platform>-<arch>-<variant>/<原文件名>`，禁止 `..` 与绝对路径；`packageVersion` 必须等于清单 `version`。
- 客户端只接受 `schemaVersion == 1` 且 `channel == "stable"`；未知字段忽略；任何结构错误整体拒绝，不进入安装流程。
- `history` 是可选的版本公告数组，每项含 `version/publishedAt/notes`，同版本不得重复，按版本数值升序。发布工具从 stable、归档与新 release 合并完整历史；回退也保留已知历史。旧客户端忽略该字段，新客户端对旧服务器回退为顶层 `notes`。
- 客户端更新弹窗只列出“当前版本 < 公告版本 <= 目标版本”的历史，避免把已经装过的版本或回退后更高版本的公告混入本次更新。
- 服务端行为：清单 `no-cache`，安装包按版本路径 `immutable` 长缓存；同版本禁止原地替换内容，修正一律发新版本；撤回通过把 stable 指回历史归档清单实现。

## 版本与安装身份

- **CMake 项目版本是唯一版本来源**：程序内置、deb、Inno Setup、安装身份与发布清单全部取同一值；正式版本只接受三段 `X.Y.Z`（无前导零），按数字比较（`1.9.0 < 1.10.0`）。`v*` 标签必须严格等于 `v<项目版本>`，CI 校验。
- 打包阶段把 `install-info.json` 写入安装：Linux 为 `/usr/share/kylinlist/install-info.json`（普通包与 bundle 同路径、不同 variant），Windows 放在程序旁（ZIP 为 `portable`，安装器写入 `setup`）。客户端读取时核对产品、平台、架构、版本与当前构建一致；缺失或不一致则禁用自动安装。
- 它与数据库中的 `installation_id`（数据 UUID）无关。

## 客户端行为

- 生产构建默认服务器为 `http://143.147.10.31:11924`，用户无需手填；可在「本机工作区 ▸ 更新服务器」修改或恢复默认，显式留空即停用。该地址仍属于已确认的可信内网边界，不代表允许公网暴露。
- 检查时机：启动后约 10 秒后台首查、运行期间每 1 小时复查、「本机工作区 ▸ 检查更新」手动触发。手动与自动检查有明确来源，不会为同一次结果显示两个窗口。
- 自动检查发现新版会弹出更新对话框，列出从当前版本到目标版本之间、按版本升序排列的全部公告。「立即更新」复用既有下载与安装链路；「暂缓 7 天」或关闭该自动提醒后，期间所有新版本均不再自动弹窗，到期后的下一次自动检查恢复。手动检查不受暂缓影响。
- 下载与安装都由用户确认；发现更新本身不自动下载。下载为流式写入数据目录 `updates/cache/`，边下边算 SHA-256；大小或哈希不符即丢弃。清单请求默认 10 秒超时，下载连续 30 秒无进展则停止。
- 「安装并退出」的准入顺序：未提交草稿检查（快速添加框）→ 复用 `prepareForExit()` 保存准入（详情页、便签、编辑会话）→ 生成 `.klb` 内容备份到 `updates/backups/`（保留最近 3 份）→ 再次校验安装包 → 写入待安装记录 → 交给系统安装器（Linux 用默认 deb 图形安装器打开本地文件；Windows 启动 Inno Setup 或打开 ZIP 所在目录）→ 退出应用。
- **「已交给安装器」不等于安装成功**：下次启动按实际版本确认——达到目标版本则提示成功并清理缓存；仍是旧版本则提示上次未完成；已被更高版本取代则清理记录。不做自动降级或自动恢复。
- `.klb` 备份覆盖任务、笔记与附件，不含桌面布局与全部本机设置；因此它是“更新前内容备份”，不是完整回滚方案。
- 安装失败或用户取消授权时，原版本与原数据继续可用；下载的安装包保留供重试。

### 测试钩子（仅测试/开发用）

| 环境变量 | 作用 |
|---|---|
| `KYLINLIST_UPDATE_INSTALL_INFO_PATH` | 指定 install-info.json 路径，供开发构建注入测试身份 |
| `KYLINLIST_UPDATE_FAKE_INSTALLER` | 指定脚本代替系统安装器执行交接（脚本以安装包路径为唯一参数） |
| `KYLINLIST_DATA_DIR` | 隔离数据目录（含 updates/cache、updates/backups） |

## 发布流程

1. GitHub CI 在 Windows x64、Linux ARM64、Linux amd64 构建通过后，运行生成器产出完整六目标发布目录并上传为 `kylinlist-update-bundle`；更新公告必须来自 `docs/release-notes/<版本>.txt`。RELEASE-INFO 记录 run、workflow、headSha 等溯源字段；标签发布仍校验 tag 与版本一致并附到 GitHub Release。
2. 开发机用 `python -I kylinlist-deploy/fetch_ci_release.py <RUN_ID> <新目录>` 下载并交叉核验指定 CI 运行，再执行 `bash kylinlist-deploy/package.sh <目录>`。该入口拒绝 partial/本地手工包，只生成带 SHA256SUMS 与 READY 的 incoming，不编译客户端、不构建服务端镜像。
3. 把结果复制到生产 `data/incoming/.kylinlist-<版本>.uploading/`，完成后在同一文件系统原子改名为正式版本目录。常驻 publisher 自动校验 CI 来源、校验清单、manifest 与安装包，安装文件、合并完整 history 并原子替换 stable。无需手动 publish、重启服务或更新服务端镜像。
4. 回退用显式 `publisher rollback [--to <版本>]`；watch 的一次性消费状态保证回滚后不会被旧 incoming 自动升回。publish/rollback/watch 共用跨进程锁。安装包发布与服务端镜像升级由 `kylinlist-deploy/package.sh` / `kylinlist-deploy/package-images.sh` 分开执行。

日常发布不经过 GitHub 之外的外部服务；服务器无需访问公网。所有业务数据使用宿主 bind mount，不使用 named/anonymous volume。完整操作见 `kylinlist-deploy/README.txt`。

## 已经验证到什么程度

> **以下全部为历史已执行记录，不计入文首当前集成。** 即使日期同为2026-11-16，其中“本轮”“最终”也只指当时二进制；旧schema8升级副本、旧备份转换、旧内链及附件正文流等描述均已由当前fresh-only／新编辑契约取代。历史通过数、日志和原生结果按原记录保留，不作为当前通过依据。

### 2026-11-16 修正正文实际绘制、附件留白与顶部计数

- 用户反馈第三列已有正文摘要、第四列却空白，证实上一轮只验证字段和空正文入口仍不足。已在隔离临时库的真实TaskPanel中复现：非空文档viewport322×20，但首block／line为0、实际墨迹0。不是颜色或高度问题，等待事件循环也不会恢复。
- 根因是Qt5.12 `setBlockCharFormat()`清空了文字布局，普通正文fast path在片段格式已匹配时直接返回、缺少局部layout通知。现仅在真实段落字符格式变更边界对当前block重新标脏，不重写同值正文、不全量重排、不改焦点逻辑。相同viewport恢复有效文字行与完整caret；新增9条data覆盖单／多行／格式化正文、真实列表点击、标题／QuickAdd焦点历史、空任务往返及停靠／overlay重挂，逐阶段校验墨迹、布局、实际命中和输入保存。
- 附件两行文字改为各自自然字体高度、1px行距的整体居中，补齐上下留白和图标中心断言，避免文件名行被拉伸后把大小挤到底边。第三列顶部实体总数控件及刷新接线完整移除，保留侧栏与分组计数。
- Windows x64、Qt5.12.8／GCC7.3／C++14完整CTest **12/12套通过，43.31秒**。原生windows完整结果：ui **112／0／0**、detail **49／0／0**、attachmentcards **33／0／0**；offscreen对应 **108／0／4**、**46／0／3**、**33／0／0**（通过／失败／跳过），旧7项能力边界不变，新增回归无skip。实际日志`.tools/build-v5-windows/Testing/Temporary/LastTest.log`；上一轮详情40项不作为本轮绘制修复证据。
- 新程序完成原生隔离`--smoke-test`，输出初始化／SQLite成功并退出0，随后独立启动供用户验收。未使用真实用户数据做测试，未保存人工验收截图；未新增依赖、未提交／推送／发布，未执行本轮Linux／ARM64构建或麒麟／飞腾真机试用。schema8／备份5不变。

### 2026-11-16 追加对齐：标题、附件与空正文（上一轮）

- 第三列顶部标题复用第四列DetailTitle（14pt／Bold），图标24px，40px标题行／28px控制条／52px中心不变。
- 详情附件改为56px类型卡片、两行文件名／大小、26px文件图标和统一更多菜单；省略号与右键共用“另存为…”／“删除”。复用既有SHA校验／QSaveFile，确认后重新校验实体／存储／附件身份；只删应用副本，不动原文件。错误与删除确认复用统一ModalDialog／ConfirmDialog，桌面compact布局和权限不变。
- 空正文不再只留一行：真实QTextEdit viewport最少三行，无后续内容时承接余白；有附件／动态时恢复自然高度。真实鼠标命中后原生聚焦，hover不抢焦点；单外层滚动、编辑会话与保存链路保持。
- Windows x64、Qt5.12.8／GCC7.3／C++14完成构建；最终完整 `ctest --test-dir .tools/build-v5-windows --output-on-failure` **12/12套通过，41.84秒**，新增`attachmentcards`。两平台三套完整结果为QtTest计数（含初始化／清理）：

  | 套件 | offscreen：通过／失败／跳过 | 原生windows：通过／失败／跳过 |
  |---|---:|---:|
  | ui | 108／0／4 | 112／0／0 |
  | detail | 37／0／3 | 40／0／0 |
  | attachmentcards | 33／0／0 | 33／0／0 |

- 旧7项offscreen能力跳过不变，新卡片／空正文用例无skip。另存为测试使用进程内文件URL接收和真实Qt文件对话框，全部文件与数据库均为合成临时数据，没有启动外部关联程序。最终日志为`.tools/build-v5-windows/Testing/Temporary/LastTest.log`；能力边界和验证职责见[UI契约](ui-alignment-contract.md#本轮自动化验证)。
- 新应用原生`--smoke-test --data-dir <临时目录>`输出初始化／SQLite成功并退出0，之后正常启动供用户验收。这次只改UI呈现与操作接线，不升级schema或备份格式。未保存人工验收截图、未安装依赖、未提交／推送／发布；未执行本轮Linux／ARM64构建或麒麟／飞腾真机试用。

### 2026-11-16 数据收敛与分组轮次

- 本轮实现 schema v8 与升级前一致性副本、备份元数据 v5、旧独立检查项转正文后移除、事件 `context`／ActivityPolicy／内嵌时间线、五种分组与类型化拖放，以及字体、详情、附件拖入和日期子层。详细契约见 [数据模型](data-model.md)、[实施指引](data-model-refactor.md)、[UI 设计](ui-design.md)和 [UI 对齐契约](ui-alignment-contract.md)。
- Windows x64，Qt **5.12.8**、GCC **7.3**、C++14；使用现有 `.tools/build-v5-windows` 完成应用及全部测试目标构建。最终运行 `ctest --test-dir .tools/build-v5-windows --output-on-failure`，**11/11 套通过**，合计 **43.15 秒**。明细为 QtTest 结果数（含初始化／清理）：

  | 套件 | 通过 | 失败 | 跳过 |
  |---|---:|---:|---:|
  | tasks | 56 | 0 | 0 |
  | ui | 108 | 0 | 4 |
  | updates | 40 | 0 | 0 |
  | stats | 22 | 0 | 0 |
  | branding | 35 | 0 | 0 |
  | migrations | 10 | 0 | 0 |
  | dropgroups | 35 | 0 | 0 |
  | groupmodel | 29 | 0 | 0 |
  | datepopup | 9 | 0 | 0 |
  | activitytimeline | 11 | 0 | 0 |
  | detail | 32 | 0 | 3 |

- CTest 使用既有 `offscreen`。其中 3 项原生窗口／浮层前置与 4 项真实 QMessageBox 交互明确跳过：Qt 5.12 Windows 消息框的 `SC_CLOSE` Show hook 会在无 native interface 的 offscreen 后端解引用空指针。按能力边界拆开测试，不吞 `Show`、不修改 Qt 库；纯导航、导入成功、临时删除、几何、真实文字原点与墨迹仍执行。
- 另以 `QT_QPA_PLATFORM=windows` 实跑最终二进制：**ui 112／detail 35／activitytimeline 11／datepopup 9 passed**，四套全部 **0 failed、0 skipped**，覆盖上述全部原生专属项。日历数字改验固定30px按钮相对加宽参考按钮的真实渲染完整性，不用 `sizeHint` 推荐宽度误判裁剪；两平台均通过。日志明细为本机 `.tools/build-v5-windows/Testing/Temporary/LastTest.log`，原生结果另见 [UI 契约](ui-alignment-contract.md#本轮自动化验证)。
- 完整应用以原生 Windows 平台执行 `--smoke-test --data-dir <独立临时目录>`，输出 `KylinList smoke-test: initialization and SQLite succeeded.` 并退出0；自动化均使用隔离临时数据。之后正常启动最新版供用户验收，由应用在本机生成 v7 升级前数据库副本后打开新库，不将正常启动当作真机验收。
- 安装交接、更新清单 `schemaVersion: 1`、六目标产物、CI／安装检查与 `v*` Release 规则不变；数据库版本8、备份版本5与更新清单版本彼此独立。本轮未改项目版本、未提交／推送、未发布，也未安装工具或新增依赖。
- 本轮未执行 Linux／ARM64 容器构建或银河麒麟／飞腾真机试用。原生视觉与实际手势由用户亲自验收，不生成人工验收截图；下方历史通过数只属于相应历史版本，不能外推到本轮。

### 1.3.0 历史验证（2026-11-14）

- Windows Qt 5.12.8 的 `tasks` / `ui` / `updates` / `stats` / `branding` 五套 CTest 全部通过；`branding` 含 35 项名称、图标、运行时配置与系统菜单覆盖测试。测试后确认没有 `test_ui.exe` 的真实自启残留。
- 在 Ubuntu 20.04 / Qt 5.12.8 中构建 amd64、arm64 的 system-qt 与 bundled-qt 四个 deb；每个构建均再次通过五套 CTest，包内版本、ELF 架构和 install-info 均匹配。amd64 为本机原生构建，arm64 使用 QEMU，不等于真机验收。
- 四个 deb 均在一次性容器中完成无 DISPLAY 的合成品牌安装/卸载测试：安装后 `/usr/local` 的高优先级菜单项使用测试名称与 SVG，包内 `/usr/share` 原文件不变；卸载后只清理 managed 覆盖。amd64 bundle 还在无系统 Qt 的运行环境中验证。
- 发布工具 99 项、统计服务 69 项测试在本机与 Python 3.12 Alpine 容器中通过；更新清单生成器 55 项通过（Windows 上 6 项按平台跳过）。CI 下载工具 45 项与 release 打包器 8 项测试通过，GitHub 交互使用本地 fake gh，不访问网络。
- 本机从 1.2.0 发布到 1.3.0 后，stable 清单含两条升序 history 和四个真实安装包；Chrome 78 实测默认品牌、ARM64 分段下载、浅色近全屏多版本更新日志、/s 相对跳转与空统计，鼠标焦点框已清除。
- 早期本地部署流程已验证镜像 tar、四包 incoming、SHA256SUMS、离线导入与生产 Compose 启动。随后已拆分为 `package.sh`（仅 CI release）与 `package-images.sh`（仅服务端镜像）；目录驱动改造没有重新编译客户端。
- 新自动发布链路已用六目标合成 CI release 实测：隐藏暂存目录不发布，原子改名后 watch 自动切换 stable、合并 1.2.0/1.3.0/1.4.0 历史并写 succeeded；显式回滚 1.3.0 后等待多个轮询周期不会自动升回。两套 Compose 均无 named volume，更新后的三个镜像 tar 已重新校验并离线导入。

### 1.2.0 历史验证

2026-11-13 在功能分支上手动派发 CI（[运行 43470023611](https://github.com/hualiyexi/kylinnote/actions/runs/43470023611)）：Windows x64、Linux ARM64、Linux amd64 三个构建目标全部通过，`update-bundle` 作业产出完整六件套发布目录（六项产物、`partial=False`）并上传为 `kylinlist-update-bundle`；`release` 作业按预期跳过（分支推送，不是 `v*` 标签）。

本地另外验证过：

- Windows Qt 5.12.8 下 `tasks` / `ui` / `updates` 三套 CTest 通过。
- 在 Ubuntu 20.04 容器中分别构建 arm64 与 amd64 的普通包和 bundle，包内 `install-info.json` 的版本、架构、安装形式与构建参数一致。
- 用这四个真实 deb 运行生成器，再用 `server/tools/publish.py validate` 与 `publish` 发布，stable 清单与安装包可从数据目录原样取到。

尚未验证：代码推送后真实 GitHub CI 的新来源元数据/公告与下载链路、真实生产宿主的自动目录发布、真实银河麒麟上的图形化 deb 安装器与菜单显示（文件关联、授权窗口、覆盖安装时序），以及飞腾 ARM64 真机。这些必须在相应真实环境确认，不能由上述自动化、fake gh 与容器结果代替。

## 测试

- `tests/tst_updates.cpp`：版本解析与比较、清单校验各类错误、目标选择、安装身份、待安装判定、文件校验、`UpdateService` 检查/下载/取消/停滞/超时/哈希与大小不符（本地 HTTP 夹具）、基础地址前缀、假安装器交接，以及一份与生成器输出同形的完整六件套清单解析——它把生成器与客户端之间的协议契约固定下来。
- `server/tools/test_publish.py`：发布工具的校验、幂等、拒绝规则与回退，以及 READY/SHA256SUMS、CI 门禁、watch 状态、版本顺序、跨进程锁和崩溃恢复。
- `packaging/test_update_manifest_generator.py`：生成器纯函数校验；完整六件套在 CI 的 `update-bundle` 作业中验证。
- CI 保留既有三目标构建、CTest、安装启动检查；新增发布归档生成与 tag 校验。
- 覆盖安装（旧 deb → 新 deb）与升级数据保留断言由打包脚本测试与 CI 安装检查承担；真实麒麟上的图形化安装器行为（授权窗口、关联打开）在虚拟机/真机试用时确认。

### 已知风险

Windows 的 `ui` 测试在 CI 上出现过“进程异常退出、日志为空”的偶发失败，本地与随后的重跑均通过，尚未定位到具体用例。测试已在 `initTestCase` 里关闭 stdout 缓冲，以后再发生会留下崩溃前的完整进度，便于定位；不要把它当作可通过重跑忽略的常规噪声。

## 与数据版本的关系

当前数据库 **schema v10**、备份元数据 **v6**、更新清单协议 **v1** 是三个独立版本。开发阶段仅接受完全空库或标识正确的当前数据库；旧 v5–v9、错误标识和未知／未来版本在任何写入、WAL切换或建表前拒绝。没有旧库升级、旧检查项转换、升级前兼容副本或旧链接双读。

`.klb` 仅读写当前 v6；旧版本或夹带已废弃字段在修改数据库／附件前拒绝。当前格式的镜像覆盖、附件原子交换与失败回滚保留，具体边界见 [数据模型](data-model.md)。

应用启动不静默清空不支持的文件。主动废弃本机开发数据时，先正常退出对应进程并核实数据目录，再由明确的本机操作停用旧文件；这不是产品中的迁移／回退能力。既有自动构建、CTest、包架构／依赖检查、安装启动及标签 Release 流程保留，实际验证按环境与轮次记录，历史通过数不能替代当前结果。
