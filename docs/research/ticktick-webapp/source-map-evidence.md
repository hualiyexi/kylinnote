# source map 窄范围追加核查证据

日期：2026-11-07。承接 `code-evidence.md`，不重做其 bundle 分析。本轮仅请求已在原注释中发现的 R5 主应用 JS、R3 公共模块 JS、R8 主应用 CSS 的三个 map。

**实际结果：总计 3 次下载，三个精确地址均返回 HTTP 404 的 HTML 响应；没有取得真实 source map，也没有取得任何 `sourcesContent` 原始源码。**

## 1. 地址来源：原 URL 与原注释正常解析

直接读取上轮隔离保存的原文件尾部 `sourceMappingURL`，用标准 URL 相对路径解析；未重新下载原 JS/CSS，未猜测变体。偏移沿用原文件从 0 开始的字符偏移。

| 本轮 ID / 原资源 | 原资源 URL（本轮未请求） | 原注释标识偏移 / 精确相对值 |
| --- | --- | --- |
| M5 / R5 | https://cncdn.dida365.com/web/static/build/app/web.ap-f962bf55bba66ee7c14f.js | @7311594 / `web.ap-f962bf55bba66ee7c14f.js.map` |
| M3 / R3 | https://cncdn.dida365.com/web/static/build/app/tt_modules.ap-f029e8d8466da2960349.js | @601126 / `tt_modules.ap-f029e8d8466da2960349.js.map` |
| M8 / R8 | https://cncdn.dida365.com/web/static/build/app/css/web.ap-5b93635b.css | @83634 / `web.ap-5b93635b.css.map` |

以下三个地址就是本轮实际请求 URL；没有重定向、重试或替代地址请求。

| ID | 正常解析后的精确请求 URL | HTTP / Content-Type | 响应体字节数 |
| --- | --- | --- | ---: |
| M5 | https://cncdn.dida365.com/web/static/build/app/web.ap-f962bf55bba66ee7c14f.js.map | 404 / text/html; charset=utf-8 | 410 |
| M3 | https://cncdn.dida365.com/web/static/build/app/tt_modules.ap-f029e8d8466da2960349.js.map | 404 / text/html; charset=utf-8 | 385 |
| M8 | https://cncdn.dida365.com/web/static/build/app/css/web.ap-5b93635b.css.map | 404 / text/html; charset=utf-8 | 403 |

## 2. SHA256 与隔离保存位置

下列哈希对应**实际收到的 404 HTML 响应体，不是成功取得的 map 哈希**。每份响应均保存到工作区外各自新建的空目录中；文件虽以 `.map` 命名，内容仍为错误 HTML。

临时根目录：`C:/Users/huaye/AppData/Local/Temp/ticktick-public-maps-yqicv26v/`。

| ID / 新建子目录 | 响应体 SHA256 |
| --- | --- |
| M5 / `M5-4djvgkd1` | `fa55399d71588385f00c5c4d9fc1bb2e7b6587012434655b5f523d26b1406e23` |
| M3 / `M3-afpop32x` | `035b4a235d3b2823778fd6c8f07bc1bfb5c22aa895c1e69b01f3562e9efb7d60` |
| M8 / `M8-v9rj9o_e` | `cb9e9d6e750727386a2cf4ee5cb47cbf976560fb533dd4c67def77611ce1e4ec` |

## 3. 内容验证与 sourcesContent 状态

- 三份响应均不能解析为 JSON：Python 标准 JSON 解析器在字符位置 0 报 `Expecting value`；未进入 source-map `version/sources/mappings` 结构核查。
- 三份响应都有同一有限原文摘录：字符范围 `[13,41)` 为 `<title>404 Not Found</title>`；`[56,78)` 为 `<h1>404 Not Found</h1>`。这些是**错误响应摘录，不是原始前端源码**；偏移对原始响应字节直接 UTF-8 解码后计算。
- 三份 HTML 中 `sourcesContent` 字面命中均为 0。**真正 map 是否包含 `sourcesContent`：三项均不可判定，因为未取得 map。** 不能把 HTML 中没有字段写成“网站 map 不携带原始源”。
- 没有 `sources` 索引、原始源路径或源码行号可供验证；不能拿请求 URL、编译后 bundle 偏移或错误页行号冒充原始源路径/行号。

## 4. 指定四类组件的取得情况

| 指定类别 | 本轮原始源片段 / 源路径 / 源码行号 | 能否建立组件对应关系 |
| --- | --- | --- |
| 工作区 / 侧栏 | 均未取得；无可用 `sourcesContent` | 不能从 map 确认工作区根容器、导航条或列表侧栏对应的原始组件文件。 |
| 菜单包装 | 均未取得；无可用 `sourcesContent` | 不能从 map 确认 dropdown wrapper、popup 定位或子菜单对应的原始组件文件。 |
| 详情编辑器 | 均未取得；无可用 `sourcesContent` | 不能从 map 确认详情面板与 CodeMirror 包装组件的原始源；不新增编辑器版本结论。 |
| 设置导航 | 均未取得；无可用 `sourcesContent` | 不能从 map 确认设置入口、分类导航或路由组件；不把设置弹窗 CSS 当作导航逻辑源码。 |

上轮 bundle/CSS 中已经确认的布局、菜单、编辑器等证据仍见 `code-evidence.md`；本轮没有补出原始源码层的独立证据，也不以旧结论伪装成 map 新发现。

## 5. 限制、安全与变更范围

- 网络请求严格止于上述 3 次 `curl -q` 下载；每地址一次，重试次数为 0，不跟随重定向，实际重定向次数均为 0。没有请求其他 chunk、API、额外 map、猜测的 CDN 变体或 `sources` 指向地址。
- 无 Cookie/Authorization 或账号数据访问；没有驱动浏览器，没有读取用户任务、存储或隐藏 React state/props。
- 分析仅用 `python -I` 内联自编代码读取原始字节、计算哈希、尝试 JSON 解析和匹配错误页短片段；未从下载目录运行脚本/工具，未执行下载文件，未做任何前端运行测试。
- 404 只说明这三个由已发布注释指向的精确地址本次未能公开取得；不能外推为“项目不存在 source map/原始源码”，也没有确证其不可用的部署原因。
- 完整响应保留在工作区外；没有将完整 map、第三方源码或错误页放入仓库。
- 本轮仅新增 `source-map-evidence.md`，不修改 `code-evidence.md` 或其他文件，不提交、不推送。原 `code-evidence.md` SHA256 仍为 `2b8706da0fe9ed3b5dfbb49a498c38f668450ed71b6cf68ffde079ad18053a84`。
