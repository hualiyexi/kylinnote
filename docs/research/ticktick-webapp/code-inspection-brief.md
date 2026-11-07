# 发布代码核查底稿

日期：2026-11-07。用途：对现场页面发现的静态资源进行只读分析，为最终技术章节提供证据。不是原始源码获取任务，不执行下载的代码，不访问账号 API、Cookie、本地存储或私有状态。

## 页面直接提供的地址

- https://cncdn.dida365.com/web/static/build/app/react_lib.ap-4cd78ae6758ff342ec6a.js
- https://cncdn.dida365.com/web/static/build/app/runtime-web.ap-c389859293c30a9ecc91.js
- https://cncdn.dida365.com/web/static/build/app/tt_modules.ap-f029e8d8466da2960349.js
- https://cncdn.dida365.com/web/static/build/app/tt_commons.ap-16d5fe3442b603fdcf0f.js
- https://cncdn.dida365.com/web/static/build/app/web.ap-f962bf55bba66ee7c14f.js
- https://cncdn.dida365.com/web/static/build/app/css/tt_modules.ap-99c17312.css
- https://cncdn.dida365.com/web/static/build/app/css/tt_commons.ap-0e8cd55b.css
- https://cncdn.dida365.com/web/static/build/app/css/web.ap-5b93635b.css

## 核查范围

框架/构建线索；Flex 布局/固定侧栏/详情栏约束；utility 类和哈希类的真实 CSS 规则；`.dropdown-menu` 菜单布局、层级、悬停、禁用与子菜单；CodeMirror 编辑器；视口断点和焦点可访问性。仅在源文件明确包含 sourceMappingURL 时记录该事实，不猜测 `.map` 路径。

## 安全与交付

下载的每个资源放入工作区以外各自新建的空目录；脚本放另一个目录；Python 使用 `-I`。不执行 JavaScript，不运行下载目录里的构建工具。不在仓库保存完整第三方 bundle。记录实际 URL、资源字节数、SHA256、状态、有限原文片段与定位词/字符偏移；将事实和推断分开。最终只写独立的 `code-evidence.md`，不要改动其他文档或应用源码。
