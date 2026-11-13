# 银河麒麟开发与兼容性基线

更新日期：2026-11-10。KylinList（麒麟清单）是个人自用应用，日常开发沿用已经跑通的构建环境。

**日常兼容性策略：在 Ubuntu 20.04 中，使用发行版自带的 Qt 5.12.8，按目标 CPU 架构编译和打包。** 固定这套环境后，普通功能开发无需重复调查系统版本或执行额外的适配工具流程；构建、CTest 和安装启动检查由现有脚本与 CI 完成。

## 固定基线

| 项目 | 当前选择 |
|---|---|
| 主要使用目标 | 银河麒麟桌面操作系统 V10 SP1，飞腾 FT-2000，ARM64 |
| Linux 构建环境 | `ubuntu:20.04` 容器，glibc 2.31，发行版 GCC / libstdc++ |
| Qt 构建版本 | Linux、Windows 均使用 Qt 5.12.8 |
| 源码基线 | C++14、Qt Widgets；新增 API 须能在 Qt 5.12.8 下编译 |
| Linux 桌面路径 | X11，沿用现有托盘、输入法和窗口集成 |
| Windows 用途 | Windows x64 上的开发预览、辅助调试；MinGW 7.3 |
| 默认 Linux 安装包 | 使用系统 Qt 的 `kylinlist_<版本>_arm64.deb` |
| 备用 Linux 安装包 | 自带 Qt 的 `kylinlist_<版本>_arm64_bundle.deb` |

Ubuntu 20.04 提供了与已测麒麟环境相符的旧版基础库。保持这一构建基线，可以避免编译时无意间引入更新系统库的要求。Windows 开发机通过 Docker 的 Linux 环境完成 deb 构建，无需在麒麟虚拟机里开发。

ARM64 和 x86_64 共用源码，分别编译。`--arch` 用于核对构建架构，不能把 x86 程序转换成 ARM 程序。Windows 本地 Docker 可借助模拟运行 ARM64 容器；GitHub CI 为 ARM64 和 amd64 分别使用对应的原生 runner，并在 Ubuntu 20.04 容器中执行同一套构建与验证。沿用编译器的默认架构选项，不添加 `-march=native` 或针对更新 ARM CPU 的指令集要求。

## 已经验证的结论

| 环境或产物 | 已有结果 |
|---|---|
| 银河麒麟 V10 SP1 2503 x86_64 虚拟机（更名前 KylinNote） | 用户已安装 Ubuntu 20.04 构建的系统 Qt 小包，并反馈可以正常使用；该结果不冒充当前 KylinList 新库版本的验收 |
| 银河麒麟 x86_64 虚拟机（当前 KylinList） | 2026-11-10 用户实测当前 Linux 包，反馈窗口表现与 Windows 一致；这是当前版本的实际桌面验证，具体补丁号未另行读取 |
| 上述虚拟机的运行库 | Qt `5.12.12+dfsg-0kylin1k1.54`；glibc `2.31`；libstdc++ 提供至 `GLIBCXX_3.4.28` |
| 本地 Ubuntu 20.04 ARM64 容器构建 | 已产出 `Architecture: arm64`、ELF `AArch64` 的 deb |
| GitHub Actions | 2026-11-10 的运行 [#42763725286](https://github.com/hualiyexi/kylinnote/actions/runs/42763725286) 已通过 Linux ARM64、Linux amd64 与 Windows x64 的构建、测试、打包及现有安装启动检查；Windows 在首次 UI 测试异常退出后，以相同源码重试通过 |
| 飞腾 FT-2000 真机 | 尚未试装，由用户实际使用时确认；具体补丁号和运行库版本尚未从真机读取 |

上述结果足以将 Ubuntu 20.04 定为日常构建方案。x86 虚拟机测试和 ARM64 自动化通过各有意义；飞腾真机的安装与桌面表现仍以实际试用为准。

目标机没有 `qmake` 不代表没有 Qt：`qmake` 属于开发工具，运行应用只需要 Qt 运行库及插件。更名前的系统 Qt 小包和当前 KylinList 均已由用户在虚拟机试装，其中当前版本的窗口表现已获反馈与 Windows 一致。默认选择几百 KB 的普通 deb；只有目标机缺少 Qt 且不便补齐时，才选自带 Qt 的 bundle；bundle 仍使用系统的 glibc、时区数据、图形驱动和基础桌面库。

## 日常工作方式

1. 在 Windows 开发、预览，保持 C++14 / Qt 5.12.8 可用的 API。
2. 推送到 `main`，由 CI 在固定环境构建并运行现有测试；本地需要 deb 时复用 Ubuntu 20.04 Docker 镜像。命令见[构建文档](building.md)。
3. 从对应运行的 Artifacts 下载 `kylinlist-linux-arm64`、`kylinlist-linux-amd64` 或 `kylinlist-windows-x64`。两个 Linux 架构各自包含普通包与 bundle。普通推送、PR 和手动运行只生成构建产物；`v*` 标签在三个构建目标全部通过后自动创建 Release。`kylinlist-update-bundle` 是可直接交给内网发布服务的更新发布目录，部署与发布流程见[内网软件更新](updates.md)。
4. 在麒麟上安装对应架构的 deb 并使用。首次真机试用时确认启动、任务与清单保存、中文输入、多提醒、托盘和便签表现；开发测试使用独立数据目录。新包不迁移旧数据，验收时不得把旧数据库是否存在当作新数据已导入。

保留普通 CTest、包架构和依赖检查，以及打包后的安装启动测试。这些由脚本自动执行。Windows 的 Qt 插件路径、`qt.conf` 和 `windeployqt --release` 是已经定位并修复的问题，继续保留。

## 何时重新检查兼容性

以下变化才需要重新评估构建基线或做针对性实测：

- 升级 Ubuntu 镜像、Qt、编译器或增加新的运行时依赖。
- 更换目标系统、CPU 架构，或改用 Wayland 等其他桌面路径。
- 新增全局快捷键、自启、通知、窗口层位等平台能力，或出现具体安装、加载、桌面行为问题。

普通任务、日历、搜索和界面逻辑的改进沿用现有基线与测试即可。官方适配工具、厂商认证、环境报告归档和发布验收模板不属于本项目的日常流程；遇到实际需要时再选择工具。

早期的第三方容器调研保存在[研究记录](research/kylin-container-and-clipboard.md)，仅作背景参考。当前构建直接使用 Ubuntu 20.04，无需引入第三方麒麟容器。
