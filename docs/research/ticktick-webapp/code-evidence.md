# 滴答清单 Web：公开发布代码证据（只读）

日期：2026-11-07。输入范围仅为 `code-inspection-brief.md` 明列、已由页面证实的 8 个静态 JS/CSS URL。本文件独立记录代码证据，不以截图代替实现核查。

## 1. 方法、边界与定位约定

- 使用 `curl -q` 对精确 URL 发起无登录凭据的公开下载，不携带 Cookie/Authorization，不跟随重定向；8 项实际 URL 均与底稿相同。
- 没有驱动浏览器，没有访问账号 API、任务数据、Cookie、存储或隐藏 React state/props；下文 state/props 名称若出现，仅指发布文本中的代码标识。
- 未执行任何下载的 JavaScript、构建工具或依赖安装；仅由独立目录中的自编 Python 脚本，以 `python -I` 读取原始字节并做字符串/规则检索。
- 下载根目录：`C:/Users/huaye/AppData/Local/Temp/ticktick-public-static-75hx4h9v/`；每项均进入各自新建的空子目录，详见清单。分析脚本另置 `C:/Users/huaye/AppData/Local/Temp/ticktick-static-analysis-scripts-9tei7_gy/inspect_static.py`。
- SHA256 对实际保存的原始文件字节计算。偏移以文件原始字节直接 UTF-8 解码、不格式化、不转换换行后的文本为准，**从 0 开始**；`@n` 表示所列定位词/短片段起点，`[a,b)` 表示包含摘录的规则/代码范围。
- 8 个文件均为 ASCII，故本批字符偏移也等于字节偏移。反斜杠保留原样；摘录不是完整第三方代码。
- “事实”是发布文本直接包含的内容；“解释/推断”是其结构含义；静态存在不保证当前页面实际走到该分支。

## 2. 下载清单与 SHA256

合计 8 个资源、12,103,851 字节，均 HTTP 200；没有下载失败或鉴权阻断项。

| ID / 临时子目录 | 实际 URL | 字节数 | HTTP / Content-Type | SHA256 |
| --- | --- | ---: | --- | --- |
| R1 / `1-414mgrw2` | https://cncdn.dida365.com/web/static/build/app/react_lib.ap-4cd78ae6758ff342ec6a.js | 127895 | 200 / application/javascript | `483997698f410b78e0ab25a413c025531151cbbdff13eda47f091d1f1c254aee` |
| R2 / `2-2hjw4oko` | https://cncdn.dida365.com/web/static/build/app/runtime-web.ap-c389859293c30a9ecc91.js | 6955 | 200 / application/javascript | `8ce6ad57736d2113c287ff70e2c31d18d273eb7a5a23de4df32e969eb74cb1a0` |
| R3 / `3-qxqwwaee` | https://cncdn.dida365.com/web/static/build/app/tt_modules.ap-f029e8d8466da2960349.js | 601184 | 200 / application/javascript | `16318079a3b99305644c1d8e16d644381bbdc63ab00b904d443970dff0cc324a` |
| R4 / `4-1qrmnluc` | https://cncdn.dida365.com/web/static/build/app/tt_commons.ap-16d5fe3442b603fdcf0f.js | 2831574 | 200 / application/javascript | `b2e15ec505c4c2a040c748e2bac02928bad4ebf1eb6274a025c65a085015af33` |
| R5 / `5-43nr5ob_` | https://cncdn.dida365.com/web/static/build/app/web.ap-f962bf55bba66ee7c14f.js | 7311645 | 200 / application/javascript | `54948eab1a84921b8822b3c98db5be22d832a8eb57c7b670ba48f8d742be2b95` |
| R6 / `6-nxlc9ai2` | https://cncdn.dida365.com/web/static/build/app/css/tt_modules.ap-99c17312.css | 19618 | 200 / text/css | `74510cf0092b526d87fc91836e86b88eb4e9624f9e0ed8ea48a79114b7d66891` |
| R7 / `7-8x6jpzmx` | https://cncdn.dida365.com/web/static/build/app/css/tt_commons.ap-0e8cd55b.css | 1121304 | 200 / text/css | `bab8d853815f6b716436f27d0e09d8117c3d121af7b0d6e5d13fa98d8489207d` |
| R8 / `8-e3yw7bxu` | https://cncdn.dida365.com/web/static/build/app/css/web.ap-5b93635b.css | 83676 | 200 / text/css | `6d9b8145c3699b540ba7d1b91e8fad8fe8fcb79995fa3f0c9fd3490ed2396e39` |

## 3. 框架与构建：不靠文件名判版本

| 定位 | 可复核原文短片段 | 事实与解释 |
| --- | --- | --- |
| R1 @116695 | `version:"17.0.2",rendererPackageName:"react-dom"` | ReactDOM renderer 实现明确自报版本，不是从 `react_lib` 文件名推断。 |
| R1 @118675、@127734 | `t.version="17.0.2"` | 分别处于 DOM renderer 导出与带 `useState/useRef` 的 React 导出尾部；发布包包含 React/ReactDOM 17.0.2 实现声明。未读取浏览器运行时版本。 |
| R5 @13109 | `r=n("y497"),o=n("dBHc")` | 主入口导入上述 React/DOM 模块；与同文件大量 `r.createElement`、Hook 调用相互印证。 |
| R1 @110；R2 [0,190) | `webpackChunkweb_node`；`var e={},o={};function n(t)` | 分块注册数组、模块缓存及加载运行时，属于 Webpack 分块产物线索；不据此锁定具体 Webpack 版本或构建配置。 |
| R5 @7311502 | `e.O(0,[1958,3344,5106,8257,97,9486,4121,3196]` | 入口还有多个 chunk 前置依赖；本轮不是完整源码/依赖审计，不能把 8 份文件视为完整应用。 |

结论边界：可写“公开发布包内的 React/ReactDOM 实现声明为 17.0.2”；不可写“已现场读取当前页面 React 运行时版本为 17.0.2”。

## 4. 页面布局、固定宽度与详情栏约束

| 定位 | 小片段 / 定位词 | 核查事实及解释 |
| --- | --- | --- |
| R5 @4440347；R7 @894261、@896858、@912505 | `id:"container-main",className:w()("flex h-full overflow-hidden dark:bg-screen-background","")`；`.flex{display:flex}` | 主容器真实采用 Flex、满高、裁切溢出，不只是视觉上“像多栏”。 |
| R5 @735665；R7 @901787、@906266 | `cv.p,"w-[50px] pb-[11px] h-full flex flex-col flex-none items-center z-[5] l-low:z-[4]"`；`.w-\[50px\]{width:50px}`；`.flex-none{flex:none}` | 导航条有明确 50px 宽度、纵向 Flex 和不伸缩约束。**固定宽度不等于 `position:fixed`**。 |
| R5 @737514 | `"g-left flex-none flex overflow-hidden"` | `left-view` 外层同样不参与 Flex 伸缩；与内部 50px 导航条是不同层级。 |
| R5 @3657348、@3657446；[3657348,3658250) | `Math.min(KEe,.9*window.innerWidth)`；`width:E?0:+x`；定位词 `id:"leftListView"` | 列表侧栏有折叠为 0、使用计算宽度的实现；窄屏分支上限取默认宽度与视口 90% 中较小值，并可用 `position:"absolute",zIndex:5,height:"100%"` 覆盖。不能称其在所有视口都固定某个像素宽度。 |
| R5 [4826252,4826730)；R7 @901313、@903781 | `getDetailContainerWidthClassName`、`relative right-0 min-w-[320px]`、`w-[36%]`；`.w-\[36\%\]{width:36%}` | 详情栏默认类为 36%；大尺寸分支相对定位并有 320px 最小宽度；中/小尺寸分支切换绝对定位、移至 `right-[-100%]`。 |
| R5 @4830148 | `style:{width:this.isHabit?` | 另有按视图分支施加百分比内联宽度的实现，故 36% 是类默认值，不是不可变的实测最终宽度；未读取这些变量的账号状态值。 |

解释：发布实现是“定宽导航 + 受控列表侧栏 + 有最小宽度/覆盖模式的详情栏”，并非所有栏都只靠均分 Flex；具体视口行为应与交互实测对照。

## 5. utility 与 CSS Modules 式局部类映射

| 定位 | 原文短片段 | 可确认到哪一层 |
| --- | --- | --- |
| R7 @78317 | `/* ! tailwindcss v3.2.7` | 这是明确的 Tailwind CSS 版本横幅，证据强于类名形似；事实是**发布 CSS 含标注 v3.2.7 的 Tailwind 产物标识**。不等于已查证 npm manifest/lockfile、安装依赖或整套样式均由它生成。 |
| R7 @894261、@906183、@906266 | `.flex{display:flex}`；`.flex-1{flex:1 1 0%}`；`.flex-none{flex:none}` | utility 类有可见的实际 CSS 声明，而非只在 HTML/JS 中出现名称。 |
| R7 @902822、@896858 | `.w-full{width:100%}`；`.h-full{height:100%}` | 宽高类的真实规则。 |
| R5 [9644,9702)；R8 [21008,21060)；R5 @735665 | `e.exports={p:"sidebar_2byOi"}`；`.sidebar_2byOi{padding-top:var(--sidabarPaddingTop)}`；`cv.p` | JS 导出局部名称映射，JS 组件引用该映射，CSS 有对应规则，构成闭环。`sidabar` 是发布文本原拼写。 |
| R4 [6042,6097)；R7 [1121119,1121250) | `e.exports={q:"rightView_9bTz2"}`；定位词 `.rightView_9bTz2`、`--rightViewPaddingTop` | 同样存在详情组件局部类映射及规则。 |

推断：上述是 CSS Modules 式的构建输出强线索，而非只靠 `_2byOi` 哈希外观判断；不能进一步确证具体 loader、源文件扩展名、命名模板或构建工具小版本。

## 6. 菜单：布局、层级、悬停、禁用与子菜单

| 定位 | 规则 / 短片段 | 代码层面的事实 |
| --- | --- | --- |
| R7 [854079,854862) | `.dropdown-menu,.dropdown-menu-menu-submenu-popup{max-height:300px}`；定位词 `border-radius:12px`、`max-width:250px`、`padding:4px 0`、`z-index:1000` | 根菜单及弹出子菜单共享边框、阴影、尺寸与层级规则；另有取消最大宽/高的特例类，不能把上限绝对化。 |
| R7 @855353 | `.dropdown-menu-menu{max-height:288px;overflow-y:auto}` | 外层 300px 与内层 288px 滚动约束是两个不同层级。 |
| R7 [855740,856364) | `display:flex;font-size:13px;font-weight:400;height:34px;justify-content:space-between;line-height:34px;max-width:250px;min-width:154px` | 菜单项和子菜单标题真实采用 Flex；同规则还有 `padding:0 12px`、单行省略、`cursor:pointer`。 |
| R7 [863430,863889)；@873292 | 定位词 `:not(.dropdown-section-title):not(.not-active):hover`、`--opacity-variant-grey-4`、`border-radius:8px`；`.dropdown-menu-menu>li[class*=-menu-item-active]:not` | 悬停灰底/8px 圆角，并有独立的 item/submenu active 类规则；不是仅原生 `:hover`。暗色主题有另外覆盖。 |
| R7 [868052,868370) | 定位词 `dropdown-menu-menu-item-disabled`；`color:#a8a8a8;cursor:not-allowed`、`background-color:transparent!important` | 已确认禁用视觉；**CSS 不能单独证明点击/键盘业务事件已被阻止**。 |
| R7 @868370 | `.modal-dropdown-menu{z-index:1100!important}` | 模态场景菜单有更高层级覆盖，不能只报告统一 z-index 1000。 |
| R3 [124281,124575) | `prefixCls:"dropdown-menu",action:["click"]`；定位词 `placement:"bottomLeft"`、`destroyPopupOnHide:!0`、`autoAdjustOverflow:!0` | 菜单包装组件明确默认点击触发、左下放置、隐藏时销毁及溢出调整；不是凭菜单样貌猜机制。 |
| R3 [123186,124155)；@123689 | 定位词 `overflow:{adjustX:1,adjustY:1,shiftX:!0,shiftY:!0}`、`popupPlacement:l,popupAlign:y`；`if(h)return this.getTriggerElement()` | 定位/溢出交给 popup 组件；包装组件 `disabled` 分支不创建 popup trigger。此结论不外推成所有菜单项均有相同事件保护。 |
| R3 @446544、@449156 | `triggerSubMenuAction:"hover"`；`popupOffset:[8,0]` | 至少存在明确的悬停子菜单实现及偏移配置；子菜单复用上述弹出层样式，不是简单把所有嵌套列表放进普通文档流。 |

边界：CSS 根菜单包含 `float:left`，但仅凭这一条不能推出实际 popup 外层定位方式。未确定所有菜单都共用同一组件，也未确证 rc-* 对应 npm 包/版本。

## 7. CodeMirror：不仅有选择器，还有初始化与扩展

| 定位 | 小片段 / 定位词 | 结论 |
| --- | --- | --- |
| R3 @253872 | `this.codeMirror=l().fromTextArea(this.codeMirrorRef.current,this.getOptions())` | 明确创建编辑器实例，不是 CSS 残留或截图猜测；随后静态代码注册 change、cursorActivity、focus/blur、paste、keydown 等事件。 |
| R3 @140539 | `mode:"markdown",keyMap:"ticktickmd",theme:"markdown-default",lineWrapping:!0` | 应用提供 Markdown 模式、自定义键盘映射、主题和自动换行配置。 |
| R3 @308548、@316203 | `defineMode("link-markdown"`；`defineMode("markdown"` | 存在自定义模式定义，与初始化配置相互支撑。 |
| R7 @1077046、@1077108 | `.CodeMirror{background:#fff;overflow:hidden;position:relative}`；定位词 `.CodeMirror-scroll{height:100%` | 有 CodeMirror 容器、滚动区等实际 CSS；后者明确 `overflow:scroll!important`。 |
| R7 [1082016,1082366) | 定位词 `.CodeMirror-hints`、`max-height:284px`、`position:absolute`、`z-index:1100` | 补全提示菜单有独立样式、滚动和层级，不能与普通 dropdown 的规则混为一谈。 |
| R7 @36578；R4 @105766 | `.editor_1XSTe .ProseMirror{min-height:21px;`；`ProseMirror-gap-paragraph-selection` | 同批产物也有 ProseMirror 样式及交互扩展线索，不能声称“所有编辑区域均由 CodeMirror 实现”。 |

版本边界：`fromTextArea`、`defineMode` 是传统 CodeMirror API 形态线索；本轮未确证 CodeMirror 核心精确版本，不能据此写死版本号。入口尚有未下载的依赖 chunk。

## 8. 显式断点与不同组件的适配范围

以下是发布 CSS 的真实条件，不套用任何框架的默认断点。

| 定位 | 条件与作用范围 | 规则事实 |
| --- | --- | --- |
| R7 @1022679 | `@media (max-width:499px)` | `s:` 规则包含绝对定位、`right:-100%`、单列等；R5 详情栏类名确实使用这些前缀。 |
| R7 @1023177、@1023840 | 500–632px 的 `.m\:`；633–952px 的 `.l\:`（CSS 转义写法） | 对应规则包含绝对定位、移出右边界、层级和背景，不只是通用排版栅格。 |
| R7 @1023534、@1023732、@1024051、@1024311 | `max-width:632px`、`min-width:633px`、`max-width:952px`、`min-width:953px` | 存在 `m-low/l-high/l-low/xl-high` 各自条件；953px 起 `xl-high:min-w-[320px]` 生效。 |
| R7 [999256,999472) | 500–952px，详情栏覆盖模式 | `.detail-show .g-right` 显示并 `right:0`；`.g-right{width:400px!important}`，覆盖常规百分比宽度。 |
| R7 [999472,999645) | ≤499px，详情栏窄屏模式 | `#right-view{min-width:100%;transition:right .2s ease-in-out}`；显示时绝对定位、`right:0;z-index:10`。 |
| R8 [37226,37604) | 设置弹窗，≤632px | 定位词 `.settingModal_2F8UH`；常态视口居中/限高，窄屏改四边为 0、取消 transform/圆角、宽高自动。不是全应用通用弹窗规则。 |
| R7 @1015263 | `@media (prefers-reduced-motion:reduce)` | 有 `.motion-reduce\:transition-none{transition-property:none}`；仅证明规则存在，不保证所有动画元素应用了它。 |

R7/R8 还包含注册、付费、图片像素密度及打印等其他媒体查询；不能将其组件专属阈值全部称为工作区主布局断点。

## 9. 焦点与键盘可访问性：已有策略，仍需实测

- **输入模式切换事实**：R5 @4369069 是 `QYe=new Set(["ArrowLeft","ArrowRight","ArrowUp","ArrowDown"])`；[4369350,4370050) 的捕获阶段 keydown 在 Tab/上述方向键时给 body 设置 `data-input-type="keyboard"`，mousedown 改为 `mouse`。这里检查的是代码，不是运行时 body/隐藏状态。
- **非键盘模式**：R7 @589201，`body:not([data-input-type=keyboard]) :focus-visible{outline:none}`；紧接还有 `:focus` 与 button 的取消 outline 规则。
- **键盘模式**：R7 [589755,590088) 为普通焦点/button 设置主题色 `auto 1px` outline；但排除了 menu item/submenu、菜单 ul、active option 等。菜单另有 R7 @873292 的 active 灰底规则，不能只凭全局 outline 得出菜单键盘焦点缺失或完备。
- **重要例外**：R7 [590088,590184) 对 `[contenteditable=true]`、无 type/text input 设置 `outline:none!important`；R8 [16544,16602) 对详情根也移除 outline。需核对替代指示器、焦点落点与对比度，不能直接判定 WCAG 合规。
- **兼容性边界**：本轮不进行任何前端运行测试；不声称 `:focus-visible`、动态类或 CSS 新语法已在旧浏览器验证。后续交互/兼容实测应以用户要求的 `C:/Chrome78` Chromium 78 为起点，并检查已有 `:focus` 路径。

## 10. sourceMappingURL：只记录原文件明示注释

8 个文件末尾均有 1 个 `sourceMappingURL`。下表偏移指向该标识本身，右列是原注释的精确相对值；JS 使用 `//#`，CSS 使用 `/*# ... */`。

| 文件 | 标识偏移 | 原文 sourceMappingURL 值 |
| --- | ---: | --- |
| R1 | 127838 | `react_lib.ap-4cd78ae6758ff342ec6a.js.map` |
| R2 | 6896 | `runtime-web.ap-c389859293c30a9ecc91.js.map` |
| R3 | 601126 | `tt_modules.ap-f029e8d8466da2960349.js.map` |
| R4 | 2831516 | `tt_commons.ap-16d5fe3442b603fdcf0f.js.map` |
| R5 | 7311594 | `web.ap-f962bf55bba66ee7c14f.js.map` |
| R6 | 19569 | `tt_modules.ap-99c17312.css.map` |
| R7 | 1121255 | `tt_commons.ap-0e8cd55b.css.map` |
| R8 | 83634 | `web.ap-5b93635b.css.map` |

**未下载、未探测任何 source map**，没有猜测 CDN 地址；注释不证明对应 map 可公开访问、内含原始源码或能正确映射此版本。

## 11. 下载受限项、未核定项与交付范围

- 网络结果：底稿 8 项全部成功；没有将 HTML 错误页当作 JS/CSS，也没有因失败改猜其他 CDN。
- 范围限制：source map、LICENSE 文件、字体/图片、动态 chunk、API 等均未追加下载；它们是范围外未请求，不是已测试后访问失败。
- 未核定：完整依赖树、npm 安装关系、构建配置、CodeMirror 核心版本、当前页面实际运行分支、真实最终计算尺寸、全部菜单事件保护及可访问性合规。
- 可复核的主要结论：多栏 Flex 与侧栏宽度控制、详情覆盖断点、菜单尺寸/状态/子菜单、CodeMirror 初始化均有真实发布代码支持；Tailwind 横幅及局部类映射的证据强于命名猜测，但仍应保留前述依赖与版本边界。
- 本轮写入仓库的文件仅为本独立文件；完整第三方代码与分析脚本均留在工作区外临时目录，未改其他文档或应用源码，未提交、未推送。
