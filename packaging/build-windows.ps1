#requires -Version 5.1
<#!
Build with explicit local tools. This script changes PATH only for its own
process, restores it on exit, and never installs Qt or tools globally.
Example (from repository root):
  pwsh -File packaging/build-windows.ps1 `
    -CMake "$PWD/.tools/venv/Scripts/cmake.exe" `
    -Ninja "$PWD/.tools/venv/Scripts/ninja.exe" `
    -QtRoot "$PWD/.tools/Qt/5.12.8/mingw73_64" `
    -CompilerRoot "$PWD/.tools/Qt/Tools/mingw730_64" `
    -InnoSetupCompiler "$PWD/.tools/InnoSetup/ISCC.exe"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CMake,
    [Parameter(Mandatory = $true)][string]$Ninja,
    [Parameter(Mandatory = $true)][string]$QtRoot,
    [Parameter(Mandatory = $true)][string]$CompilerRoot,
    [string]$CTest,
    [string]$InnoSetupCompiler,
    [string]$BuildDir,
    [string]$OutputDir,
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [ValidateRange(1, 128)][int]$Parallel = 4,
    [switch]$SkipInstaller,
    [switch]$SkipTests,
    [switch]$SkipArchive
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$ProjectDir = Split-Path -Parent $PSScriptRoot

function Resolve-Tool([string]$Path, [string]$Label) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing $Label. Pass its full executable path: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}
function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE" }
}
$CMake = Resolve-Tool $CMake 'CMake'
$Ninja = Resolve-Tool $Ninja 'Ninja'
if (-not $CTest) { $CTest = Join-Path (Split-Path -Parent $CMake) 'ctest.exe' }
$CTest = Resolve-Tool $CTest 'CTest'
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
$CompilerRoot = (Resolve-Path -LiteralPath $CompilerRoot).Path
$Gcc = Resolve-Tool (Join-Path $CompilerRoot 'bin/gcc.exe') 'MinGW gcc'
$Gxx = Resolve-Tool (Join-Path $CompilerRoot 'bin/g++.exe') 'MinGW g++'
$WinDeployQt = Resolve-Tool (Join-Path $QtRoot 'bin/windeployqt.exe') 'windeployqt'
if (-not $SkipInstaller) { $InnoSetupCompiler = Resolve-Tool $InnoSetupCompiler 'Inno Setup ISCC' }
if (-not $BuildDir) { $BuildDir = Join-Path $ProjectDir 'build-windows' }
if (-not $OutputDir) { $OutputDir = Join-Path $ProjectDir 'dist' }
New-Item -ItemType Directory -Force -Path $BuildDir, $OutputDir | Out-Null
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path
if ($BuildDir -eq $ProjectDir -or $BuildDir -eq [IO.Path]::GetPathRoot($BuildDir)) {
    throw 'BuildDir must be a separate build directory'
}
$VersionMatch = [regex]::Match((Get-Content -LiteralPath (Join-Path $ProjectDir 'CMakeLists.txt') -Raw), '(?m)^\s*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $VersionMatch.Success) { throw 'Cannot parse project version from CMakeLists.txt' }
$Version = $VersionMatch.Groups[1].Value
$PackageName = "KylinNote-$Version-Windows-x64"
$StageParent = Join-Path $BuildDir 'package'
$StageDir = Join-Path $StageParent $PackageName
# This exact, verified package child is the only recursive deletion target.
if (Test-Path -LiteralPath $StageDir) {
    $ResolvedStage = (Resolve-Path -LiteralPath $StageDir).Path
    if ($ResolvedStage -ne $StageDir -or (Split-Path -Parent $ResolvedStage) -ne $StageParent) {
        throw "Unsafe staging directory: $ResolvedStage"
    }
    Remove-Item -LiteralPath $ResolvedStage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

$OriginalPath = $env:PATH
$OriginalTemp = $env:TEMP
$OriginalTmp = $env:TMP
$env:TEMP = Join-Path $BuildDir 'tmp'
$env:TMP = $env:TEMP
New-Item -ItemType Directory -Force -Path $env:TEMP | Out-Null
$OriginalPluginPath = $env:QT_PLUGIN_PATH
$OriginalPlatformPluginPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
$OriginalDataDir = $env:KYLINNOTE_DATA_DIR
$OriginalPlatform = $env:QT_QPA_PLATFORM
try {
    $env:PATH = "$(Join-Path $QtRoot 'bin');$(Join-Path $CompilerRoot 'bin');$OriginalPath"
    Invoke-Checked $CMake @('-S', $ProjectDir, '-B', $BuildDir, '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$Configuration", "-DCMAKE_MAKE_PROGRAM=$Ninja",
        "-DCMAKE_C_COMPILER=$Gcc", "-DCMAKE_CXX_COMPILER=$Gxx",
        "-DCMAKE_PREFIX_PATH=$QtRoot", '-DKYLINNOTE_BUNDLE_QT=OFF',
        '-DKYLINNOTE_WITH_DBUS=OFF', '-DBUILD_TESTING=ON')
    Invoke-Checked $CMake @('--build', $BuildDir, '--config', $Configuration, '--parallel', "$Parallel")
    if (-not $SkipTests) {
        # Qt 5.12.8 的官方 Windows 构建使用相对前缀：QLibraryInfo 会把插件目录
        # 解析成"可执行文件所在目录 + 相对前缀"，而测试程序在 build/bin 下，
        # 不在 Qt 目录里，于是找不到任何平台插件。Qt 找不到插件时会弹出
        # "no Qt platform plugin could be initialized" 对话框并一直等待，在
        # CI 里表现为无输出的超时。显式指定插件目录即可。
        $env:QT_PLUGIN_PATH = Join-Path $QtRoot 'plugins'
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $QtRoot 'plugins/platforms'
        # CI 里没人能点崩溃对话框：关掉 WER 的 UI，让崩溃以退出码结束而不是挂起。
        foreach ($WerRoot in @('HKCU:\Software\Microsoft\Windows\Windows Error Reporting',
                               'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting')) {
            try {
                New-Item -Path $WerRoot -Force | Out-Null
                Set-ItemProperty -Path $WerRoot -Name DontShowUI -Value 1 -Type DWord
            } catch { Write-Host "WER UI 设置跳过: $WerRoot" }
        }
        # 诊断：关键运行库在 PATH 里解析到哪一份。两份不同的 MinGW 运行库会造成崩溃。
        foreach ($LibName in @('Qt5Core.dll', 'libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
            $Hits = @(foreach ($PathDir in ($env:PATH -split ';')) {
                if ($PathDir -and (Test-Path -LiteralPath (Join-Path $PathDir $LibName))) {
                    Join-Path $PathDir $LibName
                }
            })
            Write-Host ("{0}: {1}" -f $LibName, $(if ($Hits.Count) { $Hits -join ' | ' } else { '<none>' }))
        }
        # 预检：-functions 只列出测试函数就退出，用来把 QApplication 构造与
        # 测试逻辑分开。分三组平台对照，并打开插件加载日志，判断是平台插件
        # 初始化卡住，还是进程在进入 Qt 之前就没有起来。
        $env:QT_DEBUG_PLUGINS = '1'
        $Probe = Join-Path $BuildDir 'bin/test_tasks.exe'
        if (Test-Path -LiteralPath $Probe) {
            $Objdump = Join-Path $CompilerRoot 'bin/objdump.exe'
            if (Test-Path -LiteralPath $Objdump) {
                Write-Host '=== test_tasks.exe 的导入表 ==='
                & $Objdump -p $Probe | Select-String 'DLL Name' |
                    ForEach-Object { Write-Host ("  {0}" -f $_.Line.Trim()) }
                # Qt 5.12 的 Qt5Gui 在 Windows 上依赖 d3dcompiler_47.dll，它在
                # Qt 仓库里是独立归档；--archives 过滤掉它会导致加载期失败。
                $GuiDll = Join-Path $QtRoot 'bin/Qt5Gui.dll'
                if (Test-Path -LiteralPath $GuiDll) {
                    Write-Host '=== Qt5Gui.dll 的导入表 ==='
                    & $Objdump -p $GuiDll | Select-String 'DLL Name' |
                        ForEach-Object { Write-Host ("  {0}" -f $_.Line.Trim()) }
                }
                $D3d = Join-Path $QtRoot 'bin/d3dcompiler_47.dll'
                Write-Host ("=== d3dcompiler_47.dll 是否随 Qt 提供: {0} ===" -f (Test-Path -LiteralPath $D3d))
            }
            # 二分：三个最小程序分别验证 工具链 / QtCore / QtGui 初始化。
            # gui.cpp 在构造 QGuiApplication 之前就 fflush，卡住时也能确定位置。
            $ProbeDir = Join-Path $BuildDir 'bisect'
            New-Item -ItemType Directory -Force -Path $ProbeDir | Out-Null
            Set-Content -LiteralPath (Join-Path $ProbeDir 'plain.c') -Encoding ASCII -Value @'
#include <stdio.h>
int main(void) { printf("plain-ok\n"); return 0; }
'@
            Set-Content -LiteralPath (Join-Path $ProbeDir 'core.cpp') -Encoding ASCII -Value @'
#include <QCoreApplication>
#include <cstdio>
int main(int argc, char **argv) {
    std::printf("core-before-app\n"); std::fflush(stdout);
    QCoreApplication app(argc, argv);
    std::printf("core-ok\n");
    return 0;
}
'@
            Set-Content -LiteralPath (Join-Path $ProbeDir 'gui.cpp') -Encoding ASCII -Value @'
#include <QGuiApplication>
#include <cstdio>
int main(int argc, char **argv) {
    std::printf("gui-before-app\n"); std::fflush(stdout);
    QGuiApplication app(argc, argv);
    std::printf("gui-ok\n");
    return 0;
}
'@
            $Gcc = Join-Path $CompilerRoot 'bin/gcc.exe'
            $Gxx = Join-Path $CompilerRoot 'bin/g++.exe'
            $QtInclude = Join-Path $QtRoot 'include'
            $QtLib = Join-Path $QtRoot 'lib'
            # 用 QLibrary 手动加载平台插件：失败时 errorString 会给出 Windows
            # 加载器拒绝加载的具体原因，直接指出缺哪个 DLL 或哪里不匹配。
            Set-Content -LiteralPath (Join-Path $ProbeDir 'plugin.cpp') -Encoding ASCII -Value @'
#include <QCoreApplication>
#include <QLibrary>
#include <QLibraryInfo>
#include <QFileInfo>
#include <cstdio>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString dir = QLibraryInfo::location(QLibraryInfo::PluginsPath);
    const QString so = dir + "/platforms/qoffscreen.dll";
    std::printf("plugins-path: %s\n", qPrintable(dir));
    std::printf("plugin-exists: %d\n", int(QFileInfo::exists(so)));
    std::fflush(stdout);
    QLibrary lib(so);
    const bool ok = lib.load();
    std::printf("plugin-load: %d\n", int(ok));
    std::printf("plugin-error: %s\n", qPrintable(ok ? QStringLiteral("none") : lib.errorString()));
    std::fflush(stdout);
    return 0;
}
'@
            & $Gcc (Join-Path $ProbeDir 'plain.c') -o (Join-Path $ProbeDir 'plain.exe')
            & $Gxx (Join-Path $ProbeDir 'core.cpp') -o (Join-Path $ProbeDir 'core.exe') `
                "-I$QtInclude" "-I$QtInclude/QtCore" "-L$QtLib" -lQt5Core
            & $Gxx (Join-Path $ProbeDir 'plugin.cpp') -o (Join-Path $ProbeDir 'plugin.exe') `
                "-I$QtInclude" "-I$QtInclude/QtCore" "-L$QtLib" -lQt5Core
            & $Gxx (Join-Path $ProbeDir 'gui.cpp') -o (Join-Path $ProbeDir 'gui.exe') `
                "-I$QtInclude" "-I$QtInclude/QtCore" "-I$QtInclude/QtGui" "-L$QtLib" -lQt5Gui -lQt5Core
            foreach ($BisectName in @('plain', 'core', 'plugin', 'gui')) {
                $BisectExe = Join-Path $ProbeDir "$BisectName.exe"
                if (-not (Test-Path -LiteralPath $BisectExe)) {
                    Write-Host ("=== bisect[{0}] 未生成 ===" -f $BisectName)
                    continue
                }
                $BisectProc = Start-Process -FilePath $BisectExe -NoNewWindow -PassThru
                if (-not $BisectProc.WaitForExit(10000)) {
                    $BisectProc.Refresh()
                    $Cpu = $BisectProc.TotalProcessorTime.TotalSeconds
                    $BisectProc.Kill()
                    Write-Host ("=== bisect[{0}] 超时，CPU {1:N2} 秒 ===" -f $BisectName, $Cpu)
                } else {
                    Write-Host ("=== bisect[{0}] 退出码 {1} ===" -f $BisectName, $BisectProc.ExitCode)
                }
            }
            $Probes = @(
                @{ Name = 'default'; Platform = $null },
                @{ Name = 'windows'; Platform = 'windows' }
            )
            foreach ($Case in $Probes) {
                if ($null -eq $Case.Platform) {
                    Remove-Item Env:\QT_QPA_PLATFORM -ErrorAction SilentlyContinue
                } else {
                    $env:QT_QPA_PLATFORM = $Case.Platform
                }
                # 不重定向：控制台是行缓冲，进程卡住时也能看到已经打印到哪一行。
                $CaseProc = Start-Process -FilePath $Probe -ArgumentList '-functions' -NoNewWindow -PassThru
                if (-not $CaseProc.WaitForExit(12000)) {
                    # 还活着：看它有没有弹窗、跑掉了多少 CPU、DLL 加载到哪一步。
                    Write-Host ("=== probe[{0}] 超时 ===" -f $Case.Name)
                    try {
                        $CaseProc.Refresh()
                        Write-Host ("  窗口标题: '{0}'" -f $CaseProc.MainWindowTitle)
                        Write-Host ("  CPU 秒: {0:N2}" -f $CaseProc.TotalProcessorTime.TotalSeconds)
                        $Loaded = @($CaseProc.Modules | ForEach-Object { $_.ModuleName })
                        Write-Host ("  已加载模块 {0} 个: {1}" -f $Loaded.Count, ($Loaded -join ', '))
                    } catch {
                        Write-Host ("  模块枚举失败: {0}" -f $_.Exception.Message)
                    }
                    $CaseProc.Kill()
                } else {
                    Write-Host ("=== probe[{0}] 退出码 {1} ===" -f $Case.Name, $CaseProc.ExitCode)
                }
            }
            Remove-Item Env:\QT_QPA_PLATFORM -ErrorAction SilentlyContinue
        }
        # CTest 在超时时不打印 --output-on-failure 捕获的内容。失败时补打平台
        # 插件清单和 LastTest.log，用来区分"插件没加载"和"测试逻辑卡住"。
        try {
            Invoke-Checked $CTest @('--test-dir', $BuildDir, '-C', $Configuration,
                '--output-on-failure', '--no-tests=error')
        } catch {
            $Platforms = Join-Path $QtRoot 'plugins/platforms'
            if (Test-Path -LiteralPath $Platforms) {
                Write-Host '=== Qt platform plugins ==='
                Get-ChildItem -LiteralPath $Platforms | ForEach-Object { Write-Host " - $($_.Name)" }
            } else {
                Write-Host "=== Qt platform plugins directory missing: $Platforms ==="
            }
            $TestLog = Join-Path $BuildDir 'Testing/Temporary/LastTest.log'
            if (Test-Path -LiteralPath $TestLog) {
                Write-Host '=== LastTest.log (tail 120) ==='
                Get-Content -LiteralPath $TestLog -Tail 120
            }
            throw
        } finally {
            Remove-Item Env:\QT_DEBUG_PLUGINS -ErrorAction SilentlyContinue
            Remove-Item Env:\QT_PLUGIN_PATH -ErrorAction SilentlyContinue
            Remove-Item Env:\QT_QPA_PLATFORM_PLUGIN_PATH -ErrorAction SilentlyContinue
        }
    }
    Invoke-Checked $CMake @('--install', $BuildDir, '--config', $Configuration, '--prefix', $StageDir)
    $App = Join-Path $StageDir 'kylinnote.exe'
    foreach ($LicenseFile in @('LICENSE', 'LICENSE.LGPL3', 'LICENSE.LGPLv3', 'COPYING', 'COPYING.LIB')) {
        $SourceLicense = Join-Path $QtRoot $LicenseFile
        if (Test-Path -LiteralPath $SourceLicense) {
            Copy-Item -LiteralPath $SourceLicense -Destination (Join-Path $StageDir "Qt-$LicenseFile.txt")
        }
    }
    # Force release mode (never --debug), then assert the deployed release DLLs
    # are byte-identical to the configured Qt installation by exact SHA256.
    # --no-patchqt keeps Qt5Core.dll unmodified so that comparison is meaningful.
    Invoke-Checked $WinDeployQt @('--compiler-runtime', '--force', '--no-patchqt',
        '--verbose', '2', '--no-translations', '--svg', '--network',
        '--dir', $StageDir, $App)
    # Explicitly require the features that windeployqt's dependency scan can miss.
    foreach ($Plugin in @('platforms/qwindows.dll', 'platforms/qoffscreen.dll', 'sqldrivers/qsqlite.dll',
            'imageformats/qsvg.dll', 'iconengines/qsvgicon.dll')) {
        $PluginPath = $Plugin
        if ($Configuration -eq 'Debug') { $PluginPath = $Plugin -replace '\.dll$', 'd.dll' }
        $Destination = Join-Path $StageDir $PluginPath
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
        Copy-Item -LiteralPath (Join-Path (Join-Path $QtRoot 'plugins') $PluginPath) -Destination $Destination -Force
    }
    Set-Content -LiteralPath (Join-Path $StageDir 'qt.conf') -Encoding ASCII -Value "[Paths]`nPlugins = .`nLibraries = ."
    Copy-Item -LiteralPath (Join-Path $ProjectDir 'packaging/windows/portable-readme.txt') -Destination $StageDir
    Copy-Item -LiteralPath (Join-Path $ProjectDir 'packaging/windows/Qt-LICENSE.LGPLv3.txt') -Destination $StageDir
    $License = Join-Path $ProjectDir 'third_party/md4c/LICENSE.md'
    if (Test-Path -LiteralPath $License) { Copy-Item -LiteralPath $License -Destination (Join-Path $StageDir 'md4c-LICENSE.txt') }
    $Suffix = ''
    if ($Configuration -eq 'Debug') { $Suffix = 'd' }
    foreach ($Required in @("Qt5Core$Suffix.dll", "Qt5Gui$Suffix.dll", "Qt5Widgets$Suffix.dll",
            "Qt5Sql$Suffix.dll", "Qt5Network$Suffix.dll", "Qt5Svg$Suffix.dll",
            'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
        $DeployedLibrary = Join-Path $StageDir $Required
        if (-not (Test-Path -LiteralPath $DeployedLibrary -PathType Leaf)) {
            throw "Portable deployment is missing $Required"
        }
        if ($Required.StartsWith('Qt5')) {
            $Expected = Join-Path $QtRoot "bin/$Required"
            if ((Get-FileHash -LiteralPath $Expected -Algorithm SHA256).Hash -ne
                (Get-FileHash -LiteralPath $DeployedLibrary -Algorithm SHA256).Hash) {
                throw "$Required is not byte-identical to the configured Qt release"
            }
        }
    }

    # Smoke the DEPLOYED executable with Qt/tool directories removed from PATH.
    # The GUI has to initialize its SQLite database and quit via --smoke-test.
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:QT_PLUGIN_PATH = $StageDir
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $StageDir 'platforms'
    $env:QT_QPA_PLATFORM = 'windows'
    $env:KYLINNOTE_DATA_DIR = Join-Path $BuildDir ("smoke-" + [guid]::NewGuid().ToString('N'))
    # A GUI-subsystem application never creates a black console window.
    # The build-only launcher waits for the real process exit code.
    $PowerShell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    & $PowerShell -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $ProjectDir 'packaging/windows/Smoke.ps1') -Application $App `
        -DataDir "$env:KYLINNOTE_DATA_DIR" -WorkDir $BuildDir
    if ($LASTEXITCODE -ne 0) { throw "GUI smoke helper failed: $LASTEXITCODE" }
    if (-not (Test-Path -LiteralPath (Join-Path $env:KYLINNOTE_DATA_DIR 'notes.db'))) {
        throw 'Deployed app did not initialize its SQLite database'
    }
    if (-not $SkipArchive) {
        $ZipFile = Join-Path $OutputDir "$PackageName.zip"
        # -LiteralPath ensures a top-level portable folder in the archive.
        Compress-Archive -LiteralPath $StageDir -DestinationPath $ZipFile -Force
        Write-Output "Portable archive: $ZipFile"
    }
    if (-not $SkipInstaller) {
        $InstallerScript = Join-Path $ProjectDir 'packaging/windows/KylinNote.iss'
        Invoke-Checked $InnoSetupCompiler @("/DAppVersion=$Version", "/DSourceDir=$StageDir",
            "/DOutputDir=$OutputDir", $InstallerScript)
        $Installer = Join-Path $OutputDir "KylinNote-$Version-Windows-x64-Setup.exe"
        if (-not (Test-Path -LiteralPath $Installer)) { throw "Installer was not generated: $Installer" }
        Write-Output "Current-user offline installer: $Installer"
    }
    Write-Output "Deployed application: $App"
} finally {
    $env:PATH = $OriginalPath
    $env:TEMP = $OriginalTemp
    $env:TMP = $OriginalTmp
    $env:QT_PLUGIN_PATH = $OriginalPluginPath
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $OriginalPlatformPluginPath
    $env:KYLINNOTE_DATA_DIR = $OriginalDataDir
    $env:QT_QPA_PLATFORM = $OriginalPlatform
}
