# 品牌覆盖（branding）

更新日期：2026-11-14。本文说明如何把“麒麟清单”的显示名称与 Logo 换成自己的品牌，覆盖客户端界面与网站两端。品牌覆盖只改显示层，不改任何内部身份，见下文“不变项”。

## 客户端

### 配置查找顺序

启动时按以下顺序找 `branding.json`，第一个存在该文件的目录生效；都没有则使用默认品牌（显示名“麒麟清单”，现有自绘 Logo）：

1. 环境变量 `KYLINLIST_BRANDING_DIR` 指定的目录；
2. 数据目录下的 `branding/`（Linux 为 `~/.local/share/kylinlist/branding/`，Windows 为本机应用数据目录下的 `branding/`）；
3. 程序所在目录下的 `branding/`；
4. Linux 系统目录 `/etc/kylinlist/branding/`（仅 Linux 参与查找）。

某个目录缺 `branding.json` 不算错误，继续找下一个；配置文件损坏、字段非法也只回退默认，不阻止启动。配置只在启动时读取一次，没有热重载。

### branding.json 字段

```json
{
  "displayName": "单位清单",
  "logo": "logo.svg"
}
```

- `displayName`：字符串，去除首尾空白后须非空且不超过 80 个字符，否则该键回退默认。
- `logo`：图片文件路径；相对路径相对配置目录解析，也允许绝对路径。图标要求能被 QIcon 真实解码（png/svg/ico 等），文件缺失或损坏时回退默认 Logo。
- 两个键各自独立覆盖：只写 `displayName` 就只换名字，Logo 保持默认；单键无效只影响该键。

`displayName` 生效的位置：应用显示名与命令行帮助、主窗口/便签/提醒弹窗标题、托盘提示与通知标题、“导出/导入备份”的文件名过滤器、Linux 自启项的显示名称。

### 部署示例

给整个单位统一换名，把配置放进系统目录：

```
/etc/kylinlist/branding/branding.json     # {"displayName":"单位清单","logo":"logo.svg"}
/etc/kylinlist/branding/logo.svg
```

`KYLINLIST_BRANDING_DIR` 优先级最高，会临时盖过数据目录与系统目录里的配置，适合测试或便携运行； unset 后按其余顺序正常生效。

### 银河麒麟开始菜单与桌面入口（CI 包可直接用）

从 1.3.0 起，deb 的安装脚本也会读取 `/etc/kylinlist/branding/`。**先放品牌配置与图片，再安装同一个 CI 包**，无需修改源码或重新构建：

1. 安装时生成 `/usr/local/share/applications/KylinList.desktop`，其优先级高于包内 `/usr/share/applications/KylinList.desktop`；`Name` 与 `Name[zh_CN]` 使用 `displayName`。
2. 自定义 Logo 复制到 `/usr/local/share/kylinlist/branding/active.<扩展名>`，菜单项用它的绝对路径作为 `Icon`；原始包内文件不被修改。
3. 升级时保留并刷新这套覆盖；卸载时只删除带本应用 managed 标记的文件，不碰管理员手工创建的其他菜单项。
4. 安装后从开始菜单新放到桌面的快捷方式会沿用单位名称和 Logo。**已经复制到桌面的旧快捷方式不会被自动改写**，删除旧副本后重新放置即可，避免覆盖用户桌面内容。

安装后修改了配置或素材时，刷新系统入口：

```bash
sudo /usr/bin/kylinlist --refresh-system-branding
# 若桌面环境未立即刷新菜单缓存，且系统提供该工具：
sudo update-desktop-database /usr/local/share/applications
```

该刷新命令使用 `QCoreApplication`，不需要图形显示；系统 Qt 小包和自包含 bundle 都可执行。坏配置或坏图片只回退对应默认值，不阻止安装。没有有效自定义时会移除本模块生成的覆盖，恢复默认“麒麟清单”。

### 不变项（升级与数据不受品牌影响）

品牌覆盖**不改变**以下内部身份，换名后升级、数据与自启都沿用原路径：

- 产品标识 `productId = kylinlist`，安装身份 `install-info.json` 的校验口径不变；
- 数据目录（Linux 为 `~/.local/share/kylinlist/`）与数据库 `kylinlist.db`、锁文件；
- QSettings 的组织名/应用名（均为 `kylinlist`）；
- DBus 服务 `org.kylin.KylinList`（路径 `/KylinList`）；
- Windows 自启 Run 键 `HKEY_CURRENT_USER\...\Run\KylinList`；
- Linux 自启文件 `~/.config/autostart/kylinlist.desktop`（文件名与 `Icon=kylinlist` 不变，仅 `Name=` 跟随 displayName）。

## 网站（server/site）

网站的品牌覆盖放在**运行数据**里，不进源码、不重建镜像：

- 覆盖目录：`server/data/branding/site/`（开发）与 `kylinlist-deploy/data/branding/site/`（生产），两套 Compose 都以只读挂载到容器 `/usr/share/nginx/html/local`。
- 配置文件 `config.json`，四个字段，**值为空字符串表示不覆盖**：

```json
{
  "displayName": "",
  "title": "",
  "description": "",
  "logo": ""
}
```

- `displayName` 换页头品牌名（最长 80 字符）；`title` 换 `<title>`（留空时按 displayName 生成）；`description` 换 meta 描述；`logo` 换页头/主图/收藏图标。
- 镜像内置的 `server/site/local/config.json` 四个字段全为空，即“不部署覆盖 = 默认麒麟清单与现有 Logo”。
- 修改运行数据里的 `config.json`/Logo 文件后刷新页面即生效（页面以 no-cache 拉取 `local/config.json`），**无需重建镜像，也无需重启容器**。
- `logo` 只允许 `local/` 目录内的相对文件名：不能是绝对路径、不能含反斜杠、路径段不能为空或 `.` / `..`；图片加载失败自动回退默认 Logo。
- 首页与 `/s/` 统计管理页共用同一套覆盖（`/s/` 以 `../local/...` 前缀加载），换一次品牌两端同时生效。
- 运行数据不入 Git：仓库里 `server/data/`、`kylinlist-deploy/data/` 均被忽略，本机测试用的覆盖文件不会带入版本库；生产值由管理员在生产机 `data/branding/site/` 下自行填写。

网站脚本按 Chromium 78 基线编写（不用 `:focus-visible` 等新特性）；改品牌逻辑后请以 Chrome 78 起步验证。

## 测试

- `tests/tst_branding.cpp`：查找顺序（env 优先、数据目录兜底）、displayName/logo 独立覆盖、JSON 损坏/字段非法/图片缺失逐项回退、重复 load 状态刷新、内部身份不随 displayName 改变；系统菜单覆盖还测试 managed 文件保护、桌面字符串转义、原子刷新、图标扩展名切换和卸载清理。全部通过 `KYLINLIST_BRANDING_DIR` / `KYLINLIST_DATA_DIR` / `KYLINLIST_DESKTOP_BRANDING_ROOT` 隔离在临时目录，不接触真实配置或 `/usr/local`。
- 网站端覆盖属静态文件行为，用浏览器本地验证（基线 Chrome 78）：改 `server/data/branding/site/config.json` 后刷新首页与 `/s/` 核对名称、标题、描述与 Logo 回退。
