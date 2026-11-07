# 组件业务实现：有界离线代码追加证据

日期：2026-11-07。本轮只读取先前保存的 8 份公开 JS/CSS，没有下载任何资源，没有驱动浏览器或操作模块设置。

## 范围与复核方法

- R1–R8 的 URL、原始字节数和 SHA256 沿用 `code-evidence.md` 第 2 节；本轮已重新计算 8 份文件哈希，全部匹配，不重复该表。
- 偏移沿用原报告：原始字节直接 UTF-8 解码，不美化、不转换换行，从 0 开始；`@n` 是定位词/片段起点，`[a,b)` 是包含证据的半开范围。本批文件为 ASCII，字符偏移亦等于字节偏移。
- 仅以 `python -I` 和工作区外自编分析工具做文本检索；不执行下载代码，不读取运行时 state/props、任务/账号数据，不分析 API、鉴权或远端数据模型。
- 每类严格保留 5 条证据，共 20 条；表内短片段为发布文本原文，多个偏移用于交叉确认同一条调用链或局部类映射，不代表取得了原始源码。

## 1. 任务行 / 详情：选中、上下文菜单、子任务和正文边界

| 编号 | R / 定位 | 原文短片段 | 文本可确认的事实 |
| --- | --- | --- | --- |
| T1 | R4 @1612752、@1612952、@1633267 | `i=o.activeTaskId,a=o.selectedTaskIds,s=o.multiSelectMode` | 行节点明确绑定 onClick:fo；fo 从辅助调用结果取出这三个字段，并在分支中调用 qt(i,a)。可确认点击进入选中处理链，但不凭标识名还原全部多选规则或业务模型。 |
| T2 | R4 [1614332,1614500)、@1633295 | `e.preventDefault(),e.stopPropagation(),fn.current={top:e.pageY,left:e.pageX},Tn(!0)` | 行节点 onContextMenu 指向 yo；yo 受布尔短路条件门控，允许时阻止默认菜单/冒泡，保存页面坐标并设置打开状态。未知布尔标识的完整语义未确认。 |
| T3 | R4 @1252100 | `id:"sub-task-list",className:i()("subtasks b-h pb-[20px] relative h-full"` | 子任务有独立 ul 容器和 subtasks 类，不是仅在正文文本中模拟缩进。仅确认 UI 容器边界，不外推其完整存储模型。 |
| T4 | R4 [1051810,1052430) | `value:n||"",setValue:W,updateAttachments:G,isReadOnly:c,prosemirrorViewRef:y` | taskcontent 下的编辑组件接收独立内容、写值、附件、只读及 ProseMirror view 引用；不能把这一正文边界笼统当作所有位置都使用 CodeMirror。 |
| T5 | R5 [4739250,4740180) | `var r=e.getProsemirrorView();if(r){t.preventDefault(),(0,te.ic)(r.state,r.dispatch),r.focus()` | td-editor 的 mousedown 先检查主键、未被阻止及目标包含关系，并排除编辑器、附件、标签、后代任务、sub-task-list、task-desc、富文本和工具栏区域；通过后才 focus 并将选择落到文档末尾。不是点击任意详情子区域都强制移焦。 |

未确认：完整 Shift/Ctrl/Meta 多选矩阵、所有上下文菜单项的事件保护、所有正文编辑器切换条件；未展开这些辅助函数所涉及的数据或同步实现。

## 2. searchWrapper_7f7zb：宽度、分类与触发

| 编号 | R / 定位 | 原文短片段 | 文本可确认的事实 |
| --- | --- | --- | --- |
| S1 | R5 @8277、@147495、@680995 | `searchWrapper_7f7zb` | vN+f 模块导出 s 对应此类；主代码导入为 eo，搜索根节点 className 使用 eo.s，形成类映射—实际组件引用闭环，不只是样式名称命中。 |
| S2 | R5 [680650,681445) | `height:422,maxHeight:"80%",width:"80%",maxWidth:560` | 实际是视口比例宽度 80%、最大宽度 560px，而非始终固定 560px；同段有 visible 为假即 return null、fixed 遮罩、top:160、居中 transform，点击遮罩自身关闭。 |
| S3 | R5 [661251,661515)、[658126,658345) | `case"task":return s.Jt("task");case"tag":return s.Jt("tag")` | 分类标题分支还明确包含 project/filter/event；selectCategory 将 category 设为传入键并重置默认筛选后调用 runBuildQueryString。只确认存在这些分支，不断言每种分类在当前现场都可见。 |
| S4 | R5 @678929、@663785 | `Y.bk.subscribe(this._listenId,"activeSearch",this.openSearchFormWithAnimation)` | 组件订阅 activeSearch；对应函数调用 props.openSearch，随后定时选择输入框文本。可确认事件到打开函数的链路，未据此推定具体全局快捷键。 |
| S5 | R5 @681836 | `onKeyDown:this.quickSelectSuggest,onChange:this.changeSearchTxt` | 搜索输入框明确区分键盘建议项处理和文本变化处理，不是仅有静态搜索外观；本轮不分析搜索 API、索引或远端/本地检索模型。 |

未确认：所有分类的现场可见条件、完整键盘导航/回车决策和搜索结果来源；“最大 560px”不等于浏览器实测宽度。

## 3. quadrant_1bHV2：组件布局与两类拖动链

| 编号 | R / 定位 | 原文短片段 | 文本可确认的事实 |
| --- | --- | --- | --- |
| Q1 | R5 @6611、@938681、@1012167 | `quadrant_1bHV2` | aX2B 导出 v3 对应此类，导入为 xS；实际 quadrant-sortable 象限根节点使用 xS.v3，并具有纵向 Flex、满高和相对定位类。不是凭类名猜完整四象限模型。 |
| Q2 | R8 [24953,25070)；R5 @1023043 | `.wrapperInner_1OIY3{grid-template-columns:repeat(2,minmax(325px,1fr));grid-template-rows:repeat(2,minmax(185px,1fr))}` | matrix 内层使用对应局部类及 grid/gap 类；CSS 明示两列两行，每格列最小 325px、行最小 185px。模板本身不证明当前数据条数，也不证明窄屏会自动变单列。 |
| Q3 | R5 @1011507、[1011652,1012260) | `(0,Ot.gl)({id:t,data:{env:B.Ot_.matrix}` | 象限组件建立排序钩子，取 attributes/listeners、节点 ref、transform、transition 和 activator ref，并向头部 RS 传递 activator/listeners。可确认拖动接线；不凭压缩别名断言 npm 库版本。 |
| Q4 | R5 [1014577,1014756) | `i=(0,Ot.VE)(t,o,a);c(i)` | onDragEnd 在存在 over 时，分别用 active.id 与 over.id 找索引，再经此调用更新序列；前面的 c 回调计算 sortOrder 变更交给既有函数。这里确认的是象限卡片排序，不等同任务跨象限属性变更。 |
| Q5 | R5 @1009375、[1009719,1009848) | `drop:function(e){var n=e.active.data.current.dragItem;o(n)&&((0,HA.M7)(),a_(n,t))}` | 另一路 drop 区声明 accepts:[TASK]，先以 o 判断拖动项，成立才调用 a_(n,t)。这与 Q4 排序回调是不同链路；没有由变量名推断优先级/日期等完整更新规则。 |

未确认：任务落入象限后全部业务字段变更、持久化流程及拖动后的现场结果。本轮没有触发拖动，也没有递归追踪其他 chunk。

## 4. focus / 习惯及 plugin-matrix / plugin-habit / plugin-pomo

| 编号 | R / 定位 | 原文短片段 | 文本可确认的事实 |
| --- | --- | --- | --- |
| M1 | R5 [4945670,4945834)、@4930792、@15134 | `pomo:"plugin-pomo",habit:"plugin-habit"` | 同一 re 常量表明确含 matrix→plugin-matrix；re 导出为 HMJ，主模块通过 B 引用。设置元数据使用 B.HMJ.matrix/pomo/habit，能确认这些开关 ID 的对应，不把字符串等同独立 npm 插件包。 |
| M2 | R4 [600044,600444) | `return!(!t||"active"!==t.status)` | 0+1q 模块的 hook 分别寻找 name 为 MATRIX/HABIT/POMO 的项，再使用此判断生成 isMatrixEnabled/isHabitEnabled/isPomoEnabled。只确认界面布尔计算，不展开其上游数据或账号含义。 |
| M3 | R5 [1992750,1993450)、[1994249,1994519) | `case n.pomo.id:(0,Zt.yZ)("POMO"),Y.bk.dispatch("needToLoadWholePage")` | 设置行将对应布尔 active 值与 onChange:M 传给开关；M 对 matrix/pomo/habit 分别调用 yZ("MATRIX"/"POMO"/"HABIT") 并派发加载通知。不能把事件名解释成已确证新 chunk 下载或浏览器整页刷新。 |
| M4 | R5 [4359400,4359520)、@4362215 | `if(!V&&"focus"===e.id)return!1;if(!U&&"matrix"===e.id)return!1;if(!H&&"habit"===e.id)return!1` | 上文 V/U/H 明确来自 isPomoEnabled/isMatrixEnabled/isHabitEnabled；此动作列表在相应布尔为假时过滤入口。这证明入口渲染门控，不证明所有路由内容都有同样的硬阻断。 |
| M5 | R5 @3639474、@3639532、@4005184 | `path:"/q/all/habit",component:rEe` | 包内明确注册习惯根路由到 rEe、带可选 id 的习惯路由到 oEe.Ay；focus 的 /focus/:id? 注册到 OL。可确认组件入口，不因存在路由或加载通知就声称所有模块已实际加载、已开放或须下载独立 chunk。 |

未确认：按需 chunk 的实际加载边界、加载完成状态、开关对所有深层路由的影响、订阅能力或授权机制；不从本轮文本推断会员状态。本代理没有操作任何开关，临时开启和恢复仍由主会话负责。

## 交付与限制

- 本轮新增的可复核证据仅为上述四类各 5 条；没有新增网络请求、递归扫 chunk、尝试 source map 或执行拖动/设置操作。
- 完整第三方 bundle 仍只在工作区外临时目录，未将完整 JS/CSS、map 或源码复制进仓库；也没有生成全量源码/依赖报告。
- 仅新增本 `component-code-evidence.md`；未改 `code-evidence.md`、`source-map-evidence.md` 或其他文件，未提交、未推送。
