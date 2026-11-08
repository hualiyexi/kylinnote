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
    # bin/qt.conf 让 windeployqt 正确解析 Qt 前缀（见测试段的说明）。
    Set-Content -LiteralPath (Join-Path $QtRoot 'bin/qt.conf') -Encoding ASCII `
        -Value "[Paths]`nPrefix = ..`n"
    # 必须显式 --release：Qt 5.12 的自动检测会把这里判成 debug，部署出
    # Qt5Cored.dll 一类带 d 后缀的库，与下面按 release 名字的校验对不上。
    Invoke-Checked $WinDeployQt @('--release', '--compiler-runtime', '--force', '--no-patchqt',
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
