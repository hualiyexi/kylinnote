# 构建、测试与离线打包

KylinNote 1.1.0 使用 C++14、Qt Widgets、Qt Sql/SQLite 和 Qt Network。Linux 固定在 Ubuntu 20.04 容器中构建，使用发行版的 Qt 5.12.8；Windows 同样使用 Qt 5.12.8，搭配 MinGW 7.3 x64。Markdown 解析器 MD4C 0.5.2 已随源码附带。

## 日常优先使用 CI 产物

普通开发无需自己重复搭建打包环境。推送到 `main` 后打开 [Actions](https://github.com/hualiyexi/kylinnote/actions/workflows/build.yml)，进入对应提交的成功运行，在页面底部的 **Artifacts** 下载：

- `kylinnote-linux-arm64`：解压后包含系统 Qt 小包和自带 Qt 的 bundle。麒麟默认使用没有 `_bundle` 后缀的几百 KB 小包。
- `kylinnote-windows-x64`：包含安装 EXE 和便携 ZIP。

安装 Linux 小包使用 `sudo apt install ./kylinnote_<版本>_arm64.deb`；系统依赖已齐时也可离线 `sudo dpkg -i 文件名.deb`。目标机无需 qmake 或开发工具。Artifacts 保留 14 天；`v*` 标签对应的构建通过后，产物会放入 [Releases](https://github.com/hualiyexi/kylinnote/releases)。

具体环境、已验证结果及何时需要重新检查见[兼容性基线](kylin-development-workflow.md)。以下为需要本地构建时使用的命令。

## Windows 本地工具（全部位于仓库 .tools）

需要已有 Python 3.9+ 和 Windows 10/11 自带的 `curl.exe`。以下 PowerShell 命令以仓库根目录为工作目录；本机实测可用的 Python 是 `C:\Users\<用户>\AppData\Local\Programs\Python\Python311\python.exe`（`python -V` 能找到即可，不必安装到 `C:\Python313`）。无需修改全局 PATH，也无需 Qt 账号或用户密钥。

Qt 官方仓库对 `aqtinstall` 拉取 7z 包会限速到几 KB/s（实测 40 分钟未能完成 qtbase），因此不固定 aqt 版本，改为 **`curl` 直连同一批官方归档**，再用 py7zr 解压合并成 aqt 的标准目录布局。`curl` 自动顺延仓库的 `.mirrorlist` 镜像，实测约 1 MB/s。

```powershell
python -m venv .tools/venv
New-Item -ItemType Directory -Force .tools/tmp,.tools/appdata,.tools/dl,.tools/bin,.tools/Qt | Out-Null
# 仅影响这个 PowerShell 进程；pip 缓存和临时目录都留在仓库内。
$env:TEMP = "$PWD/.tools/tmp"
$env:TMP = $env:TEMP
$env:APPDATA = "$PWD/.tools/appdata"
& ./.tools/venv/Scripts/python.exe -m pip install --disable-pip-version-check --timeout 120 --retries 8 py7zr

# Qt 归档固定到 5.12.8-0-202004051457 这一版，SHA1 与官方 .sha1 一致。
$QtPkg  = 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt5_5128/qt.qt5.5128.win64_mingw73/'
$ToolPkg = 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/tools_mingw/qt.tools.win64_mingw730/'
# schannel 证书吊销检查在这类内网/受限网络下会报 CRYPT_E_NO_REVOCATION_CHECK，需关闭。
$Curl = @('--fail', '--location', '--retry', '5', '--retry-delay', '2',
          '--connect-timeout', '20', '--silent', '--show-error', '--ssl-no-revoke')
$Archives = @(
    @{ Url = $QtPkg + '5.12.8-0-202004051457qtbase-Windows-Windows_10-Mingw73-Windows-Windows_10-X86_64.7z';
       Sha1 = '68b7ee7a700d7361042c1f59128bda960879b484' },
    @{ Url = $QtPkg + '5.12.8-0-202004051457qtsvg-Windows-Windows_10-Mingw73-Windows-Windows_10-X86_64.7z';
       Sha1 = '8d4dd27dbb658a56e2f598f0a055123dabe4b046' },
    @{ Url = $QtPkg + '5.12.8-0-202004051457qttools-Windows-Windows_10-Mingw73-Windows-Windows_10-X86_64.7z';
       Sha1 = '3c8941b02126ff488049c7cec5f7407ff6a95024' },
    @{ Url = $QtPkg + '5.12.8-0-202004051457x86_64-7.3.0-release-posix-seh-rt_v5-rev0-runtime.7z';
       Sha1 = '84f0d3499cd42cd82773faa34443446969c865b2' },
    @{ Url = $ToolPkg + '7.3.0-1-202004170606x86_64-7.3.0-release-posix-seh-rt_v5-rev0.7z';
       Sha1 = '518f7ab802dc99f4a309105bed1c9bc01e5f7398' }
)
foreach ($a in $Archives) {
    $file = Join-Path $PWD ('.tools/dl/' + (Split-Path -Leaf $a.Url))
    & curl.exe @Curl -o "$file.part" $a.Url
    if ($LASTEXITCODE -ne 0) { throw "下载失败: $($a.Url)" }
    if ((Get-FileHash "$file.part" -Algorithm SHA1).Hash -ne $a.Sha1) { throw "SHA1 不符: $file" }
    Move-Item -Force "$file.part" $file
}

# 解压后合并：qtbase/qtsvg/qttools/MinGW 运行时都落到同一个 5.12.8/mingw73_64。
# 顶层目录名取自归档内的 5.12.8/ 或 Tools/；robocopy 退出码 <8 即成功。
foreach ($z in Get-ChildItem .tools/dl -Filter *.7z) {
    $out = Join-Path $PWD ('.tools/dl/x-' + $z.BaseName)
    & ./.tools/venv/Scripts/python.exe -m py7zr x $z.FullName $out
    if ($LASTEXITCODE -ne 0) { throw "解压失败: $($z.Name)" }
    $from = Join-Path $out '5.12.8/mingw73_64'
    if (Test-Path $from) {
        & robocopy $from .tools/Qt/5.12.8/mingw73_64 /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "合并失败: $($z.Name)" }
    }
    $from = Join-Path $out 'Tools/mingw730_64'
    if (Test-Path $from) {
        & robocopy $from .tools/Qt/Tools/mingw730_64 /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "合并失败: $($z.Name)" }
    }
    Remove-Item $out -Recurse -Force
}

# CMake 与 Ninja 从官方 GitHub Release 取预编译包。
# files.pythonhosted.org 在本机持续读超时，pip 装 cmake/ninja 的方案不可用。
& curl.exe @Curl -o .tools/dl/ninja.zip 'https://github.com/ninja-build/ninja/releases/download/v1.13.0/ninja-win.zip'
if ($LASTEXITCODE -ne 0) { throw '下载 ninja 失败' }
& curl.exe @Curl -o .tools/dl/cmake.zip 'https://github.com/Kitware/CMake/releases/download/v3.31.10/cmake-3.31.10-windows-x86_64.zip'
if ($LASTEXITCODE -ne 0) { throw '下载 cmake 失败' }
Expand-Archive .tools/dl/ninja.zip -DestinationPath .tools/bin -Force
Expand-Archive .tools/dl/cmake.zip -DestinationPath .tools/dl/cmake-unpack -Force
Move-Item (Get-ChildItem .tools/dl/cmake-unpack -Directory | Select-Object -First 1).FullName .tools/cmake
Remove-Item .tools/dl/cmake-unpack -Recurse -Force

# 自检：以下路径必须全部存在。
& ./.tools/Qt/5.12.8/mingw73_64/bin/qmake.exe -v
& ./.tools/Qt/Tools/mingw730_64/bin/g++.exe --version | Select-Object -First 1
& ./.tools/cmake/bin/cmake.exe --version | Select-Object -First 1
& ./.tools/bin/ninja.exe --version
```

本地构建使用的工具路径：

| 工具 | 仓库相对路径 |
| --- | --- |
| Python（仅用于解压） | `.tools/venv/Scripts/python.exe`（`-m py7zr`） |
| CMake / CTest | `.tools/cmake/bin/cmake.exe` / `ctest.exe` |
| Ninja | `.tools/bin/ninja.exe` |
| Qt 5.12.8 | `.tools/Qt/5.12.8/mingw73_64` |
| MinGW 7.3 | `.tools/Qt/Tools/mingw730_64` |
| windeployqt | `.tools/Qt/5.12.8/mingw73_64/bin/windeployqt.exe` |

下载好的归档留在 `.tools/dl`（约 140 MB），重复执行时会重新校验 SHA1；`.tools/` 已在 `.gitignore` 中，不进仓库。

`packaging/build-windows.ps1` 以 **UTF-8 BOM** 保存；PowerShell 5.1 对无 BOM 的 UTF-8 中文注释可能按 ANSI 误解析。脚本内把 `TEMP/TMP` 定向到构建目录（本机默认临时目录不可写），并在 `finally` 恢复 `PATH/TEMP/TMP/QT_PLUGIN_PATH/QT_QPA_PLATFORM_PLUGIN_PATH/QT_QPA_PLATFORM/KYLINNOTE_DATA_DIR`，不写入用户级或机器级环境变量。本机 PowerShell 执行策略禁止直接运行未签名脚本时，用下面命令的进程级 `Bypass`（不修改策略设置）。

开发构建、运行 CTest、部署并生成便携 ZIP：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File packaging/build-windows.ps1 `
    -CMake "$PWD/.tools/cmake/bin/cmake.exe" `
    -Ninja "$PWD/.tools/bin/ninja.exe" `
    -QtRoot "$PWD/.tools/Qt/5.12.8/mingw73_64" `
    -CompilerRoot "$PWD/.tools/Qt/Tools/mingw730_64" `
    -SkipInstaller
```

默认编译输出在 `build-windows/bin`，实际部署目录在 `build-windows/package/KylinNote-1.1.0-Windows-x64`，ZIP 在 `dist`。脚本先运行 CTest，再对部署后的程序执行 `--smoke-test --data-dir <独立目录>`；冒烟时移除 PATH 中的 Qt/编译器目录，以发现部署遗漏。它保留 Qt 的 SQLite、SVG、Windows 平台插件，并用 `windeployqt --compiler-runtime` 携带 MinGW 运行时。

调试 UI 时运行部署目录中的程序，参数 `--data-dir` 会隔离数据库、设置和单实例通道：

```powershell
& './build-windows/package/KylinNote-1.1.0-Windows-x64/kylinnote.exe' `
    --data-dir "$PWD/.tools/dev-data"
```

生成安装 EXE 还需 Unicode Inno Setup 6，可安装在 `.tools/InnoSetup`，然后将其完整路径传入：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File packaging/build-windows.ps1 `
    -CMake "$PWD/.tools/cmake/bin/cmake.exe" `
    -Ninja "$PWD/.tools/bin/ninja.exe" `
    -QtRoot "$PWD/.tools/Qt/5.12.8/mingw73_64" `
    -CompilerRoot "$PWD/.tools/Qt/Tools/mingw730_64" `
    -InnoSetupCompiler "$PWD/.tools/InnoSetup/ISCC.exe"
```

CI 使用 Windows runner 自带的 `C:/Program Files (x86)/Inno Setup 6/ISCC.exe`。安装器面向 Windows 10/11 x64，默认安装至当前用户的 `%LOCALAPPDATA%\Programs\KylinNote`，无需管理员权限，支持中文/空格路径；Qt 与插件都随包携带，目标机不必安装 Qt。安装器与便携版均不默认开启登录自启。卸载只移除应用文件和指向本安装的登录启动项，保留应用数据目录中的 SQLite 与设置。便携 ZIP 也使用当前用户数据目录，可用 `--data-dir` 指定独立目录。

`-BuildDir`、`-OutputDir`、`-CTest`、`-Parallel` 可显式覆盖；所有工具参数都接受完整路径。`-SkipArchive` 适合重复开发构建，`-SkipTests` 仅用于本地诊断；CI 不跳过测试。MSVC 可直接用 CMake 配置并编译，已启用 `/utf-8`；现有部署脚本固定使用 MinGW，不混用 MSVC 与 MinGW Qt。

## Linux 原生构建

Ubuntu 20.04 的主要开发依赖：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake binutils file dpkg-dev \
  qtbase5-dev qtbase5-dev-tools qt5-qmake libqt5sql5-sqlite \
  libqt5svg5 libqt5svg5-dev libx11-dev libxext-dev libxrender-dev \
  libxi-dev libgl1-mesa-dev libxcb-xinerama0 libxkbcommon-x11-0
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DKYLINNOTE_REQUIRE_X11=ON
cmake --build build-linux --parallel
(cd build-linux && ctest --output-on-failure)
```

`kylinnote_core` 包含数据库、任务领域、调度和跨平台 IPC；`kylinnote_ui` 包含 Qt Widgets 装配，可供 `test_ui` 链接；`test_tasks` 链接 core。Qt Network 的 `QLocalServer`/`QLocalSocket` 负责单实例唤醒。Qt DBus 和旧 D-Bus 适配器仅在 UNIX 且 `KYLINNOTE_WITH_DBUS=ON` 时编译；Windows 不需要 D-Bus。Linux 安装 `libx11-dev` 可保留 X11 平台扩展能力，用户快捷键策略由应用控制。

## ARM64 deb：常规包与离线 bundle

在原生 ARM64 主机运行：

```bash
bash packaging/build-deb.sh --arch arm64
bash packaging/build-deb.sh --arch arm64 --bundle-qt
```

脚本核对构建环境与包内 ELF 的架构；`--arch` 不能转换 CPU 指令。ARM64 用于目标机，amd64 用于 x86 麒麟虚拟机。系统包和 bundle 使用独立的构建目录，避免缓存混用。打包入口要求 Qt 5.12.8，直接查询系统 qmake 的版本与插件路径。

常规包依赖目标机系统 Qt；bundle 将 Qt 和非基础运行库放在 `/usr/lib/kylinnote`，`/usr/bin/kylinnote` 启动包装脚本设置路径。必须携带 xcb、SQLite、SVG 图片及 SVG 图标插件。依赖闭合检查读取每个 ELF 的 `DT_NEEDED`，要求它实际存在于 bundle 内，或属于明确的基础系统库列表；构建机恰好装有 Qt 不会让遗漏通过。

本机已有两份可复用的构建镜像：`kylinnote-builder:focal`（amd64）和 `kylinnote-builder:focal-arm64`（arm64）。它们是在 Ubuntu 20.04 上安装编译器、CMake、Qt 等依赖后保存的本地镜像。Windows PowerShell 下可直接构建小包：

```powershell
docker run --rm --platform linux/arm64 `
  --mount "type=bind,source=$PWD,target=/src" --workdir /src `
  kylinnote-builder:focal-arm64 `
  bash packaging/build-deb.sh --arch arm64 --output-dir /src/dist
```

测试 x86 麒麟虚拟机时，将平台改为 `linux/amd64`、镜像改为 `kylinnote-builder:focal`、架构改为 `amd64`。脚本拒绝覆盖同名 deb 或校验文件；重建同版本时可用新的 `--output-dir`。Git Bash 执行带 `/src` 等容器路径的 Docker 命令时，需在 `docker` 前加 `MSYS_NO_PATHCONV=1`，避免路径被转换成 Windows 路径。

没有本地构建镜像时，完整 CI 构建会在 Ubuntu 20.04 中安装所需依赖并生成两个包：

```powershell
docker run --rm --platform linux/arm64 `
  --mount "type=bind,source=$PWD,target=/src" --workdir /src `
  ubuntu:20.04 bash packaging/ci-build.sh
```

ARM64 容器在 Windows x64 上通过 Docker 的模拟能力运行，CI 则使用原生 ARM64 runner。两种方式均在容器内用 ARM64 工具链生成 ARM64 程序。deb 使用 xz 压缩，默认声明 glibc 2.31 和 Qt 5.12.8 的依赖下限；脚本不再生成逐文件 ABI 报告。具体实测状态见[兼容性基线](kylin-development-workflow.md)。

## bundle 的自动安装启动测试

此流程已由 CI 执行；日常开发直接下载产物即可。修改 bundle 打包逻辑后，需要本地复现时可用以下命令。测试在独立、没有系统 Qt 的 Ubuntu 20.04 容器中进行，避免构建环境掩盖漏包；准备基础依赖后，断网安装并启动应用。

```bash
bundle=dist/kylinnote_1.1.0_arm64_bundle.deb
docker run --name kylinnote-runtime-base --platform linux/arm64 \
  -v "$PWD":/src:ro -w /src ubuntu:20.04 \
  bash packaging/test-bundle.sh --prepare "$bundle"
docker commit kylinnote-runtime-base kylinnote-runtime:test
docker rm kylinnote-runtime-base
docker run --rm --platform linux/arm64 --network none \
  -v "$PWD":/src:ro -w /src kylinnote-runtime:test \
  bash packaging/test-bundle.sh "$bundle"
```

准备阶段安装基础图形依赖；运行阶段断网执行 `dpkg -i`，检查包内动态库解析。随后通过 Xvfb 启动应用，要求 `--smoke-test` 正常退出、SQLite 数据库已创建，且实际加载了 xcb 和 SQLite 插件。

## CI 与发布

GitHub Actions 的 Linux ARM64 与 Windows x64 job 并行执行，均运行 CTest。Linux 构建系统 Qt 小包与 bundle，将产物直接放到 `dist/`，并保留独立容器中的安装与启动测试；Windows 产出便携 ZIP 和当前用户安装 EXE。

默认令牌仅有 `contents: read`。只有已有 `v*` 标签触发、且两个打包 job 都成功后，独立 release job 才获得 `contents: write` 并上传该次产物；普通 push、PR、手工运行不会创建 Release，也不会自动创建标签。

Qt 的 LGPLv3 文本、MD4C MIT 许可证及第三方说明会随包保留。Qt 对应版本源代码可从 https://code.qt.io/ 获取；组件版本与源地址见仓库根的第三方说明。
