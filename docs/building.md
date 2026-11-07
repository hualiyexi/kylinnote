# 构建、测试与离线打包

KylinNote 1.1.0 使用 C++14、Qt Widgets、Qt Sql/SQLite 和 Qt Network。最低 API 基线是 Qt 5.12；Windows 开发使用 Qt 5.15.2 + MinGW 8.1 x64。Markdown 解析器 MD4C 0.5.2 已随源码附带，无需 npm、浏览器或在线渲染服务。

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

# Qt 归档固定到 5.15.2-0-202011130602 这一版，SHA1 与官方 .sha1 一致。
$QtPkg  = 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt5_5152/qt.qt5.5152.win64_mingw81/'
$ToolPkg = 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/tools_mingw/qt.tools.win64_mingw810/'
# schannel 证书吊销检查在这类内网/受限网络下会报 CRYPT_E_NO_REVOCATION_CHECK，需关闭。
$Curl = @('--fail', '--location', '--retry', '5', '--retry-delay', '2',
          '--connect-timeout', '20', '--silent', '--show-error', '--ssl-no-revoke')
$Archives = @(
    @{ Url = $QtPkg + '5.15.2-0-202011130602qtbase-Windows-Windows_10-Mingw-Windows-Windows_10-X86_64.7z';
       Sha1 = 'cf5936239d84a471c9f44bde6959edb7b350d106' },
    @{ Url = $QtPkg + '5.15.2-0-202011130602qtsvg-Windows-Windows_10-Mingw-Windows-Windows_10-X86_64.7z';
       Sha1 = '9604244c450047f259b53665528011bd3c42573a' },
    @{ Url = $QtPkg + '5.15.2-0-202011130602qttools-Windows-Windows_10-Mingw-Windows-Windows_10-X86_64.7z';
       Sha1 = '65c9eca0302982a0cebc1fd09f3f0a88d18c56ec' },
    @{ Url = $QtPkg + '5.15.2-0-202011130602x86_64-8.1.0-release-posix-seh-rt_v6-rev0-runtime.7z';
       Sha1 = '878628efed4b83f32d5a0651e9cabb2a20994a1a' },
    @{ Url = $ToolPkg + '8.1.0-1-202004170606x86_64-8.1.0-release-posix-seh-rt_v6-rev0.7z';
       Sha1 = 'a65a625a48d485f7f8101dfc2c0ec85c50208feb' }
)
foreach ($a in $Archives) {
    $file = Join-Path $PWD ('.tools/dl/' + (Split-Path -Leaf $a.Url))
    & curl.exe @Curl -o "$file.part" $a.Url
    if ($LASTEXITCODE -ne 0) { throw "下载失败: $($a.Url)" }
    if ((Get-FileHash "$file.part" -Algorithm SHA1).Hash -ne $a.Sha1) { throw "SHA1 不符: $file" }
    Move-Item -Force "$file.part" $file
}

# 解压后合并：qtbase/qtsvg/qttools/MinGW 运行时都落到同一个 5.15.2/mingw81_64。
# 顶层目录名取自归档内的 5.15.2/ 或 Tools/；robocopy 退出码 <8 即成功。
foreach ($z in Get-ChildItem .tools/dl -Filter *.7z) {
    $out = Join-Path $PWD ('.tools/dl/x-' + $z.BaseName)
    & ./.tools/venv/Scripts/python.exe -m py7zr x $z.FullName $out
    if ($LASTEXITCODE -ne 0) { throw "解压失败: $($z.Name)" }
    $from = Join-Path $out '5.15.2/mingw81_64'
    if (Test-Path $from) {
        & robocopy $from .tools/Qt/5.15.2/mingw81_64 /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "合并失败: $($z.Name)" }
    }
    $from = Join-Path $out 'Tools/mingw810_64'
    if (Test-Path $from) {
        & robocopy $from .tools/Qt/Tools/mingw810_64 /E /NFL /NDL /NJH /NJS /NP | Out-Null
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
& ./.tools/Qt/5.15.2/mingw81_64/bin/qmake.exe -v
& ./.tools/Qt/Tools/mingw810_64/bin/g++.exe --version | Select-Object -First 1
& ./.tools/cmake/bin/cmake.exe --version | Select-Object -First 1
& ./.tools/bin/ninja.exe --version
```

工具路径（本机已实际安装并验证）：

| 工具 | 仓库相对路径 |
| --- | --- |
| Python（仅用于解压） | `.tools/venv/Scripts/python.exe`（`-m py7zr`） |
| CMake / CTest | `.tools/cmake/bin/cmake.exe` / `ctest.exe` |
| Ninja | `.tools/bin/ninja.exe` |
| Qt 5.15.2 | `.tools/Qt/5.15.2/mingw81_64` |
| MinGW 8.1 | `.tools/Qt/Tools/mingw810_64` |
| windeployqt | `.tools/Qt/5.15.2/mingw81_64/bin/windeployqt.exe` |

下载好的归档留在 `.tools/dl`（约 140 MB），重复执行时会重新校验 SHA1；`.tools/` 已在 `.gitignore` 中，不进仓库。

`packaging/build-windows.ps1` 以 **UTF-8 BOM** 保存；PowerShell 5.1 对无 BOM 的 UTF-8 中文注释可能按 ANSI 误解析。脚本内把 `TEMP/TMP` 定向到构建目录（本机默认临时目录不可写），并在 `finally` 恢复 `PATH/TEMP/TMP/QT_PLUGIN_PATH/QT_QPA_PLATFORM_PLUGIN_PATH/QT_QPA_PLATFORM/KYLINNOTE_DATA_DIR`，不写入用户级或机器级环境变量。本机 PowerShell 执行策略禁止直接运行未签名脚本时，用下面命令的进程级 `Bypass`（不修改策略设置）。

开发构建、运行 CTest、部署并生成便携 ZIP：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File packaging/build-windows.ps1 `
    -CMake "$PWD/.tools/cmake/bin/cmake.exe" `
    -Ninja "$PWD/.tools/bin/ninja.exe" `
    -QtRoot "$PWD/.tools/Qt/5.15.2/mingw81_64" `
    -CompilerRoot "$PWD/.tools/Qt/Tools/mingw810_64" `
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
    -QtRoot "$PWD/.tools/Qt/5.15.2/mingw81_64" `
    -CompilerRoot "$PWD/.tools/Qt/Tools/mingw810_64" `
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

脚本核验 Debian 主机架构、内核架构与所有携带 ELF 的 Machine；`--arch` 不用于伪装架构，也不是交叉编译选项。另支持原生 amd64 供 Linux 开发。两种模式默认分别使用 `build-deb-arm64-system` 和 `build-deb-arm64-bundle`，每次配置都显式设置 `KYLINNOTE_BUNDLE_QT=OFF/ON`，避免复用缓存污染产物。`--qt-prefix` / `--qmake` 可指定一套完整 Qt，缺少必要插件会直接失败。

常规包依赖目标机系统 Qt；bundle 将 Qt 和非基础运行库放在 `/usr/lib/kylinnote`，`/usr/bin/kylinnote` 启动包装脚本设置路径。必须携带 xcb、SQLite、SVG 图片及 SVG 图标插件。依赖闭合检查读取每个 ELF 的 `DT_NEEDED`，要求它实际存在于 bundle 内，或属于明确的基础系统库列表；构建机恰好装有 Qt 不会让遗漏通过。

完整 CI 构建可在原生 ARM64 Docker 主机复现：

```bash
docker run --rm --platform linux/arm64 -v "$PWD":/src -w /src \
  ubuntu:20.04 bash packaging/ci-build.sh
```

Ubuntu 20.04 / glibc 2.31 / Qt 5.12 是兼容性参考基线，不保证所有麒麟版本、桌面、驱动和补丁组合完全兼容。交付前仍应在目标麒麟 ARM64/X11 环境实机验收。离线 bundle 依赖基础 libc/C++、X11/xcb、字体、GL 和 D-Bus 系统库；这些依赖必须由目标机预装，不能承诺在空白系统仅放入 deb 就满足全部图形栈需求。

## 独立无系统 Qt 的离线验收

不要在构建容器里测试 bundle：系统 Qt 可能掩盖漏包。以下命令使用全新的 Ubuntu 20.04，先安装包内声明的基础图形依赖和验证工具，再创建运行镜像；真正安装及启动时断网。

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

准备和运行阶段都拒绝系统 Qt 包/qmake；运行阶段只执行离线 `dpkg -i`，不执行 apt。测试遍历所有 ELF，核对架构以及每条 `DT_NEEDED` 在 `LD_LIBRARY_PATH` 下的实际解析，并要求 Qt 全部从包内加载。最后经 Xvfb + `dbus-run-session` 启动真正的应用，用 `--smoke-test` 自动退出，检查 SQLite 数据库创建及 xcb/SQLite 插件实际载入。`--timeout-mode` 用于还没有冒烟参数的旧应用，要求存活至超时，CI 使用严格自动退出模式。

## CI 与发布

GitHub Actions 的 Linux ARM64 与 Windows x64 job 并行执行，均运行 CTest。ARM64 在 Ubuntu 20.04 容器内构建并用另一个无 Qt、断网的容器验收；Windows 产出便携 ZIP 和当前用户离线安装 EXE。checkout 7.0.1、upload-artifact 7.0.2、download-artifact 8.0.2 是配置时通过公开 GitHub release 与 action.yml 核对的稳定 Node 24 版本。

默认令牌仅有 `contents: read`。只有已有 `v*` 标签触发、且两个打包 job 都成功后，独立 release job 才获得 `contents: write` 并上传该次产物；普通 push、PR、手工运行不会创建 Release，也不会自动创建标签。

Qt 的 LGPLv3 文本、MD4C MIT 许可证及第三方说明会随包保留。Qt 对应版本源代码可从 https://code.qt.io/ 获取；组件版本与源地址见仓库根的第三方说明。
