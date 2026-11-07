# 第三方麒麟容器与剪贴板项目：deb 兼容性参考

研究日期：2026-11-07。本文只对三个公开仓库及 KylinNote 的构建/打包配置做静态核查；**没有下载 ISO、拉取或运行第三方镜像、安装 deb、运行项目测试或取得目标麒麟实机验收结果**。项目首要目标及验收约束见[银河麒麟优先开发与验收](../kylin-development-workflow.md)。

## 1. 结论

- 与实际目标系统 **SP、补丁、CPU 和用户态环境一致**、来源可信的麒麟容器，可以增加一层 deb 安装、动态库和自动化启动预检；容器共享宿主 Linux 内核，也不能代表真实麒麟桌面、驱动、输入法、托盘和窗口管理器，不能替代目标机验收。Windows 上运行 Linux 容器时，Linux 容器共享的是其 Linux 虚拟机的内核，不是 Windows 内核。
- 本次所看的 `zx900930/docker-kylin` 是第三方仓库。现有公开代码**不足以证明其最终镜像是完整的指定 V10 SP 用户态、包含可工作的 dpkg、或与实际部署环境一致**。在独立核验镜像来源、身份、架构和包管理能力前，不能把它作为有效的麒麟兼容通过依据；适合作为待评估的实验材料，而非直接接入发布门禁。
- `shuglx/kylinpaste` 使用 Tauri/Rust/React，不是 Qt Widgets。可以借鉴其低版本构建基线、deb 压缩及运行库核对意识，不能直接复制 WebKit 依赖或把其 Ubuntu CI 当作本项目麒麟实机证明。

## 2. `docker-kylin`：证据与边界

核查时固定提交为 [`c93d9eb86d84fe3b1d2e254e1c89a0dc4e9b7d02`](https://github.com/zx900930/docker-kylin/tree/c93d9eb86d84fe3b1d2e254e1c89a0dc4e9b7d02)。

| 公开证据 | 能得出什么 | 不能得出什么 |
|---|---|---|
| [README](https://github.com/zx900930/docker-kylin/blob/c93d9eb86d84fe3b1d2e254e1c89a0dc4e9b7d02/README_zh.md#L1-L3) 宣称银河麒麟及多架构 | 说明作者希望提供相应镜像 | 不能仅凭名称证明官方发布或所有架构、具体 SP 的产物可用 |
| [ISO 清单](https://github.com/zx900930/docker-kylin/blob/c93d9eb86d84fe3b1d2e254e1c89a0dc4e9b7d02/iso_urls.txt#L1-L11) 中的文件名包含 Desktop V10 SP1 2403/2503 的 x86_64/arm64 等，也有 Server 变体 | 是有针对具体系统与架构的构建输入清单 | 文件名不是已验证的镜像内容、来源授权、下载成功或对应镜像已经发布 |
| [构建脚本](https://github.com/zx900930/docker-kylin/blob/c93d9eb86d84fe3b1d2e254e1c89a0dc4e9b7d02/scripts/build.sh#L158-L169) 将 ISO 挂载目录复制到构建目录；[Dockerfile](https://github.com/zx900930/docker-kylin/blob/c93d9eb86d84fe3b1d2e254e1c89a0dc4e9b7d02/Dockerfile#L9-L16) 从未固定 digest 的 `alpine` 开始并复制构建上下文 | 能看到作者的镜像构造路径 | 脚本未展示 squashfs 等已安装根文件系统的解包；没有读取 ISO 或成品，不知道顶层是否已是完整 rootfs、哪些 Alpine 文件保留或被覆盖，也不能断言最终镜像一定失败或一定是麒麟用户态 |
| [下载脚本](https://github.com/zx900930/docker-kylin/blob/c93d9eb86d84fe3b1d2e254e1c89a0dc4e9b7d02/scripts/download-isos.sh#L30-L70) 的自动入口仅接受 amd64/arm64，并用 `wget` 下载 | 当前脚本对自动构建有架构限制 | 其他架构的 README 声称不等于已有可用自动构建；该下载步骤未展示 ISO 哈希/签名核验 |
| [镜像验证脚本](https://github.com/zx900930/docker-kylin/blob/c93d9eb86d84fe3b1d2e254e1c89a0dc4e9b7d02/scripts/verify-image.sh#L26-L27) 运行 `ls /` 和读取 `/etc/os-release` | 提供了有限的镜像内容检查 | 不能证明 dpkg、Qt 插件、deb 依赖或桌面功能可用 |
| [一次公开工作流运行](https://github.com/zx900930/docker-kylin/actions/runs/18623562389) 显示成功 | 表示该工作流在页面上显示成功 | 工作流允许下载失败或无路径时跳过后续步骤；该运行的步骤级结果未取得，不能由绿灯认定每个 ISO 都已构建、推送或存在可验证 digest |

仓库 README 给出镜像名称前缀；工作流发布命名空间使用 secrets。本次没有取得可核实的实际镜像仓库、精确 tag 或 digest，也未检查镜像发行许可。**不得把 README 的镜像名称当成已经验证可拉取的镜像。**

## 3. `kylinpaste`：实际做法与借鉴范围

核查时固定提交为 [`6e30740c9d10da49efbed209e4c137a0c7a3b96b`](https://github.com/shuglx/kylinpaste/tree/6e30740c9d10da49efbed209e4c137a0c7a3b96b)，对应项目 v1.7.0。

| 实际证据 | 可以借鉴的方向 | 不能直接推断 |
|---|---|---|
| [构建 Dockerfile](https://github.com/shuglx/kylinpaste/blob/6e30740c9d10da49efbed209e4c137a0c7a3b96b/docker/Dockerfile.build#L1-L25) 使用 Ubuntu 20.04，针对较老 glibc/WebKit 运行时作约束；CI 在 ARM runner 构建 | 选择与目标机一致或更低且经核验的用户态与工具链基线；按 CPU 分别构建 | 同基线不是同一个麒麟系统，不代表其他系统库和补丁均匹配 |
| [deb 打包脚本](https://github.com/shuglx/kylinpaste/blob/6e30740c9d10da49efbed209e4c137a0c7a3b96b/scripts/package-deb.sh#L64-L73) 显式 `-Zxz`，并检查包成员中无 `.zst` | 检查 deb 的 `control.tar.*`、`data.tar.*` 压缩格式，并记录构建端/目标端 dpkg 版本；必要时在核验后固定格式 | 不能只因作者选 xz 就认定我们的目标系统一定不支持 zstd，也不能把打包检查当成麒麟安装成功 |
| [README 安装说明](https://github.com/shuglx/kylinpaste/blob/6e30740c9d10da49efbed209e4c137a0c7a3b96b/README.md#L55-L64)提及 WebKitGTK 4.1；[构建文件](https://github.com/shuglx/kylinpaste/blob/6e30740c9d10da49efbed209e4c137a0c7a3b96b/docker/Dockerfile.build#L20-L25) 和 [deb Depends](https://github.com/shuglx/kylinpaste/blob/6e30740c9d10da49efbed209e4c137a0c7a3b96b/scripts/package-deb.sh#L49-L61) 指向 WebKitGTK 4.0 | 发布前应核对文档、构建依赖、包依赖与目标机已安装库是否一致 | 4.0 和 4.1 是不同 ABI 系列；此处需要进一步确认目标机依赖，不能只凭文档差异断言其产品无法运行；KylinNote 不应因此引入 WebKit |
| [工作流成功记录](https://github.com/shuglx/kylinpaste/actions/runs/41610486286) 对应 Ubuntu ARM runner 和构建容器 | 可以参考自动构建并产出 deb 的流程 | 不等于 CI 在银河麒麟实机上测试 |
| 作者在 [X11 源码注释](https://github.com/shuglx/kylinpaste/blob/6e30740c9d10da49efbed209e4c137a0c7a3b96b/src-tauri/src/x11.rs#L1-L9) 中描述目标机实测结论 | 有作者人工适配和测试的自述 | 本次未找到与候选包哈希关联、覆盖具体 SP/补丁/步骤/结果的完整验收记录；不能据此声称已获得厂商认证 |

项目源码内有 Rust 单元测试，但本次核查的 CI 未见 `cargo test` 步骤；因此不能写成“没有任何测试”，也不能把自动构建绿灯当成测试全通过。剪贴板模拟输入等逻辑是其特有场景，不应直接移植到 Qt 任务应用。

## 4. 对 KylinNote 的具体建议

1. **先核实目标**：记录真实银河麒麟 V10 具体 SP、补丁、CPU、桌面、目标 Qt/系统库、dpkg 版本。目标未确定前，ISO 文件名或现有 control 描述均不能代替这些信息。
2. **建立可信容器预检层**：优先使用获得授权、来源可追溯且与实际目标系统及架构匹配的用户态。若仍考虑 `docker-kylin`，先独立核对实际镜像 digest、来源及许可、系统标识、架构、glibc、`dpkg`、软件源、Qt/插件与目标环境；如缺失或无法对齐，预检结论只能写“待核实”，不能写“麒麟兼容通过”。
3. **分开测试两类包**：在一次性、隔离的干净环境中，分别验证系统 Qt 包与 Qt bundle 的包架构、声明依赖、安装/配置、动态库和插件、应用冒烟及卸载；离线验收阶段禁网并保留结果。构建、准备系统依赖和真实离线安装应明确分阶段，不混淆网络条件。
4. **核对 deb 压缩**：项目现有 [`dpkg-deb --build`](../../packaging/build-deb.sh) 未显式指定 `-Zxz` 等算法；本次未检查真实产物，不能断言它使用哪一种。后续可对可信候选包只读执行 `ar t <包名.deb>`，记录成员压缩格式、包 SHA-256 及目标 dpkg 版本，再验证目标系统能否安装。是否固定 xz 应在确认两端支持后决定。
5. **安排真实桌面验收**：在目标麒麟上测任务、日历和中文搜索，以及输入法、通知、托盘、窗口/便签层位、屏幕缩放、离线数据库与升级卸载。把结果与同一个候选包 SHA-256 关联。
6. **隔离与授权**：第三方容器不是针对恶意软件的安全沙箱，包安装可能执行维护脚本。未经审查和授权，不运行第三方镜像或 ISO 的构建脚本；拟执行时使用一次性隔离环境和测试数据，不提供宿主 home、Docker socket、特权模式或敏感凭据，不将项目包/系统信息上传第三方。`--data-dir` 不应被当成对所有用户级配置副作用的完整隔离保证。

研究时，上述为拟实施建议。后续已补充参考 CI 校验；具体流程见[开发与验收流程](../kylin-development-workflow.md)。**这些改动不代表目标麒麟设备或厂商认证已经通过。**

## 5. 补充 ISO 地址：可以作为介质线索

用户补充的 [`hao817374/Kylin_iso_urls` README](https://github.com/hao817374/Kylin_iso_urls/blob/d4e077f41ef3a6a3b2eccf2e2d74ddce7a9fddde/README.md) 在所查固定提交中只有标题和“银河麒麟iso文件地址”；具体版本来自其 [ISO 清单](https://github.com/hao817374/Kylin_iso_urls/blob/d4e077f41ef3a6a3b2eccf2e2d74ddce7a9fddde/iso_urls.txt)。以下全部是**文件名声称，不是实测的 ISO 内容**：

- [第 1–4 行](https://github.com/hao817374/Kylin_iso_urls/blob/d4e077f41ef3a6a3b2eccf2e2d74ddce7a9fddde/iso_urls.txt#L1-L4)：Desktop V10 SP1 的 2403/2503，列有 x86_64 和 ARM64。
- [第 7–11 行](https://github.com/hao817374/Kylin_iso_urls/blob/d4e077f41ef3a6a3b2eccf2e2d74ddce7a9fddde/iso_urls.txt#L7-L11)：还有 HWE、Wayland 及特定硬件标识变体；不能因为写着 ARM64 就假设适配所有 ARM64 硬件。
- [第 5–6 行及第 12–14 行](https://github.com/hao817374/Kylin_iso_urls/blob/d4e077f41ef3a6a3b2eccf2e2d74ddce7a9fddde/iso_urls.txt#L5-L14)还混有 V10 SP3 Server 与 V11 Server；不能当成 V10 SP3 桌面版。此清单列出的桌面文件名均为 SP1，不能证明实际部署目标就是 SP1。

本次未在该 README/清单中发现 ISO 的 SHA-256、数字签名或使用/再分发授权，也未下载介质或访问 ISO 目标地址；不能只凭域名、文件名或 Git 提交认定官方来源、完整性及链接有效性。两个仓库的 `iso_urls.txt` Git blob 不同，未逐项比对，不能据此判断其 ISO 是否相同。

**推荐使用方式**：先确认实际目标机的具体版本和架构，从可信、获授权渠道独立获取匹配介质及可信校验值/签名；核验后可在匹配架构的隔离虚拟机内安装完整系统，验证相同候选 deb 的安装、启动、升级与桌面行为。虚拟机比容器更接近完整系统，但其虚拟硬件、显示驱动和宿主条件不同，仍不能替代真实目标设备验收；跨架构模拟也不能当作目标架构原生实机测试。
