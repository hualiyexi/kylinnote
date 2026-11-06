# KylinNote

面向**银河麒麟 V10 SP1 ARM64** 的 Windows 7 风格桌面便笺。纯单机、无网络、无 AI 依赖。

- 无边框便签窗口：标题条拖动、右下角拖拽缩放、默认**贴在桌面**（`F12` 循环层位：贴在桌面 → 普通 → 始终置顶）
- 内容实时自动保存（300ms 防抖，无保存按钮）；写库失败时内存保留并给出状态提示
- 每条便签 6 色循环切换（黄 / 蓝 / 绿 / 粉 / 紫 / 白），默认黄色
- 系统托盘右键菜单：新建 / 显示全部 / 隐藏全部 / **便签层位（贴在桌面 / 普通 / 始终置顶）** / 退出
- 开机自启（`~/.config/autostart/kylinnote.desktop`），启动时还原位置、尺寸、颜色与叠放顺序
- 单实例：`QLockFile` + D-Bus，重复启动转为"显示全部"
- 数据：`~/.local/share/kylinnote/notes.db`，SQLite WAL

---

## 1. 目录结构

```
kylinnote/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.cpp                     # 入口：平台检查、字体、单实例判定
│   ├── Common.h / Common.cpp        # 常量、NoteRecord、几何编解码、标题派生
│   ├── app/
│   │   ├── KylinNoteApp.h/.cpp      # 装配：数据库/自启/还原/托盘/D-Bus/快捷键
│   │   ├── SingleInstanceGuard.h/.cpp  # QLockFile 独占锁
│   │   ├── DbusAdapter.h/.cpp       # D-Bus 服务注册与跨实例唤醒
│   │   ├── NoteManager.h/.cpp       # 新建/软删/显示隐藏/防抖落库/失败重试
│   │   ├── TrayController.h/.cpp    # 托盘图标与右键菜单
│   │   └── Settings.h/.cpp          # QSettings 偏好（不再提示、首次提示）
│   ├── db/
│   │   ├── Database.h/.cpp          # SQLite 连接、WAL、schema 迁移
│   │   └── NoteDao.h/.cpp           # notes 表 CRUD 与软删
│   ├── ui/
│   │   ├── NoteWidget.h/.cpp        # 无边框便签窗口
│   │   ├── TitleBar.h/.cpp          # 标题条：拖动 + 颜色 + 关闭
│   │   ├── NoteEditor.h/.cpp        # 编辑器：字号/删除线/列表循环
│   │   ├── ResizeGrip.h/.cpp        # 右下角缩放手柄
│   │   ├── ColorTheme.h/.cpp        # 6 色调色板与样式表
│   │   └── ConfirmDeleteDialog.h/.cpp
│   ├── platform/
│   │   ├── Autostart.h/.cpp         # 自启 desktop 文件写入
│   │   └── ScreenFit.h/.cpp         # 多屏越界修正、窗管就绪判定
│   └── resources/icons/kylinnote.svg
└── packaging/
    ├── build-deb.sh                 # 一键构建 arm64 deb
    ├── KylinNote.desktop            # 应用菜单项
    └── deb/DEBIAN/{control,postinst,prerm}
```

## 2. 运行环境要求

| 项目 | 要求 |
|---|---|
| 系统 | 银河麒麟 V10 SP1，ARM64（aarch64） |
| 会话 | **X11**（不支持 Wayland，Wayland 下无边框置顶与全局坐标行为不可用） |
| Qt | 5.12.x（已在 5.12.12 运行时上验证依赖面） |
| 字体 | 文泉驿微米黑 `WenQuanYi Micro Hei`（缺失时回退到 SansSerif） |
| 运行时库 | `libqt5core5a libqt5gui5 libqt5widgets5 libqt5sql5 libqt5sql5-sqlite libqt5dbus5` |

## 3. 构建

### 3.1 安装构建依赖

```bash
sudo apt update
sudo apt install -y build-essential cmake \
    qtbase5-dev qtbase5-dev-tools qttools5-dev-tools \
    libqt5sql5-sqlite qt5-qmake
```

> 麒麟源里的 Qt 版本若是 5.12.x 可直接使用；若系统只有更低版本（如 5.9），
> 见 3.3 使用独立的 Qt 5.12.8。

### 3.2 本地编译

```bash
cd kylinnote
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/bin/kylinnote
```

`CMakeLists.txt` 中 `find_package(Qt5 5.12 REQUIRED COMPONENTS Core Gui Widgets Sql DBus)`，
配置阶段会打印实际找到的 Qt 版本与目录，便于排错。

### 3.3 Qt 库依赖处理的三种方式

1. **系统 Qt（推荐）**：安装 `qtbase5-dev`，CMake 自动定位 `/usr/lib/aarch64-linux-gnu/cmake/Qt5`。
2. **独立安装的 Qt 5.12.8**：
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_PREFIX_PATH=/opt/Qt5.12.8/gcc_64
   ```
   运行时需保证可执行文件能找到对应 `.so`：
   - 打包时把 Qt 库一起打进 deb（私有目录 + `RPATH`），或
   - 安装后在 `/etc/ld.so.conf.d/` 增加 Qt 库路径并 `ldconfig`，或
   - 运行前 `export LD_LIBRARY_PATH=/opt/Qt5.12.8/gcc_64/lib`。
   若要"随包携带 Qt 库"，直接用 4.2 的 `--bundle-qt`（内部已配好
   `RPATH=$ORIGIN`、`qt.conf` 与启动脚本，无需手工改 CMake）。
3. **deb 依赖声明**：`packaging/deb/DEBIAN/control` 已静态声明 Qt 运行时包；
   如需按实际链接结果自动推算，用 `./packaging/build-deb.sh --shlibdeps`
   （会调用 `dpkg-shlibdeps` 并替换 `Depends:` 行）。

> 注意：SQLite 驱动是插件形态，**必须**安装 `libqt5sql5-sqlite`，
> 否则 `QSqlDatabase::addDatabase("QSQLITE")` 会失败，便签只能在内存中保存。

## 4. 打包与安装

### 4.1 常规包（目标机已装 Qt 运行时）

```bash
cd kylinnote
chmod +x packaging/build-deb.sh
./packaging/build-deb.sh                       # 产出 kylinnote_1.0.2_arm64.deb
./packaging/build-deb.sh --version 1.0.1       # 指定版本
./packaging/build-deb.sh --qt-prefix /opt/Qt5.12.8/gcc_64
```

### 4.2 自包含包（内网 / 离线，推荐给其他用户）

`--bundle-qt` 会把 **Qt 运行库与插件随包携带**，目标机器不需要安装任何 Qt 包：

```bash
./packaging/build-deb.sh --bundle-qt
# 产出 kylinnote_1.0.2_arm64_bundle.deb
```

分发到内网机器后：

```bash
sudo dpkg -i kylinnote_1.0.2_arm64_bundle.deb
kylinnote &        # 或从应用菜单启动
```

包内布局与原理：

| 路径 | 作用 |
|---|---|
| `/usr/bin/kylinnote` | 启动脚本，设置 `LD_LIBRARY_PATH` 与 `QT_PLUGIN_PATH` 后 exec 真身 |
| `/usr/lib/kylinnote/kylinnote` | 真身，`RPATH=$ORIGIN` 指向同目录的 Qt 库 |
| `/usr/lib/kylinnote/libQt5*.so.5` 等 | 随包 Qt 库（Core/Gui/Widgets/Sql/DBus、ICU、pcre2-16、png 等） |
| `/usr/lib/kylinnote/plugins/` | 平台插件 xcb、SQLite 驱动、图片格式、输入法插件 |
| `/usr/lib/kylinnote/qt.conf` | 保证开机自启直接拉起真身时也能找到插件 |

升级已安装的旧版本（同包名、版本号更高，直接覆盖即可）：

```bash
sudo dpkg -i kylinnote_1.0.2_arm64_bundle.deb
# 升级后先在托盘右键退出旧进程，再重新启动，确保新包文件被加载
```

随包内容由脚本自动收集：`ldd` 扫描依赖 → 排除系统库（glibc、libstdc++、X11/xcb、GL、字体、glib、zlib、dbus 等）→
只拷贝 Qt 自有库与插件 → **递归补齐插件的传递依赖** → 依赖闭合交验 → `strip` 精简。

实测（arm64 / Qt 5.12 / 银河麒麟）：随包目录 55 个库文件、62MB，`strip` 后打成 deb 为 **17MB**。

> **关键：`plugins/platforms/libqxcb.so` 依赖 Qt 私有库 `libQt5XcbQpa.so.5`。**
> 这个库不在主程序 `ldd` 里，只有顺着插件往下扫才能发现。漏带它的后果：
> 没装 Qt 的机器上平台插件加载失败；装了别的版本 Qt 的机器上动态链接器会回退到系统库，
> 包内 `libQt5Gui` 与系统 `libQt5XcbQpa` 版本不一致 → 私有 ABI 混合 →
> **窗口能映射、光标会变，但整体完全不绘制（看起来是"全透明"，输入也看不见文字）**。
> 因此打包脚本在出包前会做一次依赖闭合交验：包内每个 ELF 的直接依赖必须已随包携带，
> 或属于明确交给系统的基础库白名单，否则直接打包失败。改动收集逻辑时不要用 `maxdepth` 限制深度。

### 4.3 自测脚本

`packaging/e2e-test.sh` 在无 X 会话的机器上也能跑通核心链路（`QT_QPA_PLATFORM=offscreen`）：

```bash
bash packaging/e2e-test.sh [可执行文件路径]
```

覆盖：私有 D-Bus 总线拉起 → `newNote` 远程调用落库 → 校验 `journal_mode/user_version/行数/z_order` →
二次启动被单实例拦截并唤醒已有实例 → 预置数据（含中文、多色、软删行）的启动还原。

`packaging/x11-verify.sh` 在**独立 X server（Xvfb）上用真实 xcb 平台插件**运行并截图，
用来确认窗口、标题栏、正文文字是否真的画出来了（offscreen 模式测不出渲染问题）：

```bash
bash packaging/x11-verify.sh kylinnote_1.0.2_arm64_bundle.deb /tmp/shot.png
```

它会打印窗口列表与截图路径，截图里应能看到黄色便笺、标题栏色点/关闭按钮、输入的文字和右下角缩放握把。
若只看到纯黑背景（说明窗口没绘制）或程序直接退出，即为回归。

自包含包的 `Depends` 只保留系统基础库（`libc6 libstdc++6 libx11-6 libxext6 libxrender1 libxi6 libxcb1 libxkbcommon0 libfontconfig1 libfreetype6 libglib2.0-0 zlib1g`），桌面系统必然具备。

> 约束：构建机与目标机需同构（arm64 + 银河麒麟 V10 SP1 + X11）。
> 建议在与目标机相同版本镜像的机器上构建，避免 glibc 版本差异。

安装 / 卸载：

```bash
sudo dpkg -i kylinnote_1.0.2_arm64.deb
sudo apt -f install          # 如有依赖未满足
sudo dpkg -r kylinnote       # 卸载（保留数据）
rm -rf ~/.local/share/kylinnote ~/.config/kylinnote  # 手动清理数据
```

也可直接安装到系统目录：

```bash
cmake --install build          # 默认前缀 /usr/local
cmake --install build --prefix /usr
```

安装内容：

| 文件 | 路径 |
|---|---|
| 可执行文件 | `/usr/bin/kylinnote` |
| 菜单项 | `/usr/share/applications/KylinNote.desktop` |
| 图标 | `/usr/share/icons/hicolor/scalable/apps/kylinnote.svg` |

首次启动会自动写入 `~/.config/autostart/kylinnote.desktop`。

## 5. 快捷键

| 快捷键 | 作用 |
|---|---|
| `Ctrl+N` | 新建便签 |
| `Ctrl+D` | 删除当前便签（首次弹确认框，可勾选"不再提示"） |
| `Ctrl+B` / `I` / `U` | 加粗 / 斜体 / 下划线 |
| `Ctrl+T` | 删除线 |
| `Ctrl+L` / `E` / `R` | 左对齐 / 居中 / 右对齐 |
| `Ctrl+Shift+L` | 列表循环：无 → 实心圆 → 空心圆 → 方块 → 无 |
| `Ctrl+鼠标滚轮` | 字号 ±1pt（有选区作用于选区，否则作用于整篇） |
| `Ctrl+Shift+>` / `<` | 字号 +1pt / -1pt |
| `F12` | 当前便签层位循环：贴在桌面 → 普通 → 始终置顶 → 贴在桌面 |

> `Ctrl+N` / `Ctrl+D` / `F12` 是 **X11 全局热键**（通过 `XGrabKey` 在根窗口抓取），
> 焦点在别的软件里也能响应。若抓取失败（组合被其它程序占用），会自动降级为
> Qt `ApplicationShortcut`，此时仅在本程序窗口处于激活态时生效。

字号范围 8–72pt；窗口最小 160×120，新建默认 250×250。
`>` 与 `.`、` <` 与 `,` 同键位，两种写法均已接受。

## 6. 数据

```
~/.local/share/kylinnote/notes.db        # SQLite，WAL 模式
~/.local/share/kylinnote/.kylinnote.lock # 单实例锁
~/.config/kylinnote/kylinnote.conf       # 偏好（确认框、首次提示）
~/.config/autostart/kylinnote.desktop    # 开机自启
```

表结构：

```sql
CREATE TABLE notes (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  title      TEXT NOT NULL DEFAULT '',
  content    TEXT NOT NULL DEFAULT '',
  color      INTEGER NOT NULL DEFAULT 0,
  geometry   TEXT NOT NULL DEFAULT '',   -- "x,y,w,h"
  z_order    INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL DEFAULT '',
  updated_at TEXT NOT NULL DEFAULT '',
  deleted    INTEGER NOT NULL DEFAULT 0
);
```

关闭便签只置 `deleted=1`（软删），行数据保留；`z_order` 越大越靠上。

## 7. 已知限制与排错

| 现象 | 原因 / 处理 |
|---|---|
| 托盘图标不显示 | UKUI 依赖 XEmbed 系统托盘；确认会话为 X11 且托盘区域未被隐藏 |
| 便签被窗管加上标题栏 | 说明窗口管理器未尊重 `FramelessWindowHint`；检查是否运行在 Wayland（`echo $XDG_SESSION_TYPE` 应为 `x11`） |
| 提示"未保存" | 数据库不可写（磁盘满/权限/WAL 目录不可写）；程序会每 5 秒重试，内存内容不丢 |
| 二次启动没反应 | D-Bus 会话总线不可用时降级为纯文件锁，此时只能保证不重复启动，无法唤醒；检查 `DBUS_SESSION_BUS_ADDRESS` |
| 高分屏字体偏小 | 程序默认不启用 Qt 高 DPI 缩放以保持坐标一致；需要时 `QT_AUTO_SCREEN_SCALE_FACTOR=1 kylinnote` |
| 便签跑到屏幕外 | 拔掉外接屏后历史坐标越界，启动时 `ScreenFit` 会自动夹回主屏可用区 |
| 新建便签立刻提示"未保存"（日志 `NOT NULL constraint failed: notes.title`） | 已修复：`QString()` 是 **null** 而非空串，`QSqlQuery::bindValue` 会把它绑成 SQL `NULL`，而 `DEFAULT ''` 只在省略该列时生效。已在 `deriveTitle()`、`NoteRecord` 默认值、`NoteDao` 绑定三处做归一化 |
| 便笺完全透明看不见，光标移上去有变化，输入的文字也不显示 | 见 4.2 末尾：自包含包漏带 `libQt5XcbQpa.so.5`，xcb 平台插件回退到系统 Qt，私有 ABI 混合导致整体不绘制。1.0.1 起已随包携带并在打包时强制交验依赖闭合 |
| 内网机装完后启动即报 `no Qt platform plugin could be initialized` | 同上，包内 `plugins/platforms/libqxcb.so` 的依赖未闭合。用 `bash packaging/x11-verify.sh <deb> /tmp/shot.png` 可在本机复现渲染问题 |

### 7.1 便签层位（贴在桌面 / 普通 / 始终置顶）

便签在窗口栈里有三种层位，对应 X11 的 `_NET_WM_STATE_BELOW` / 无状态 / `_NET_WM_STATE_ABOVE`：

| 层位 | 表现 |
|---|---|
| **贴在桌面**（默认） | 浮在桌面之上、所有应用窗口之下，不遮挡正在使用的软件 |
| 普通 | 与其它应用窗口按焦点正常竞争 |
| 始终置顶 | 压在所有窗口之上 |

- `F12` 循环切换**当前便签**的层位。
- 托盘右键 →「便签层位」可一次性设定**全部便签**的层位，并作为之后新建便签的默认值（写进
  `~/.config/kylinnote/kylinnote.ini` 的 `ui/defaultLayer`）。
- 新建的便签会**临时置前**一次，保证"点了就有反应"；失焦后自动落回你设定的默认层位。

## 8. 设计说明（实现要点）

- **无边框与缩放**：Qt 5.12 尚无 `QWindow::startSystemMove/Resize`，因此标题条拖动与右下角缩放均手动计算（记录起始全局坐标与窗口几何，按位移增量移动/调整）。
- **置顶切换**：`setWindowFlags()` 会隐式隐藏窗口，切换后恢复原几何与可见性；置顶用 `WindowStaysOnTopHint`，沉底用 `WindowStaysOnBottomHint`。
- **窗管未就绪**：启动时若 `QGuiApplication::screens()` 为空或主屏可用区无效，延迟 500ms 重试（最多 5 次）后再还原；还原后延迟 800ms 补一次 `raise()`，补偿窗管晚设置 `_NET_WM_STATE_ABOVE` 的情况。
- **防抖落库**：`QTimer(300ms, singleShot)`，内容/几何/颜色变更均 `start()` 重启计时；写库失败标记界面状态并进入 5 秒重试队列，退出前 `flushAll()` 再兜一次。
- **单实例**：先 `QLockFile`（30 秒过期）抢占，失败则通过 D-Bus 调用已运行实例的 `showAll()` 后退出；D-Bus 不可用时仅保留锁判定。
