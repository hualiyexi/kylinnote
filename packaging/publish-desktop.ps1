#requires -Version 5.1
<#
Builds the Windows preview package and mirrors it onto a fixed desktop folder,
so the same launch path always holds the newest build.

The desktop target is fully replaced (robocopy /MIR) on every run. To make that
safe, the first run stamps the folder with a marker file, and every later run
refuses to touch a folder that does not carry it.

Example (from repository root):
  pwsh -File packaging/publish-desktop.ps1
  pwsh -File packaging/publish-desktop.ps1 -SkipTests
  pwsh -File packaging/publish-desktop.ps1 -TargetDir 'D:\KylinList-Test'
#>
[CmdletBinding()]
param(
    [string]$TargetDir,
    [string]$CMake,
    [string]$Ninja,
    [string]$QtRoot,
    [string]$CompilerRoot,
    [string]$BuildDir,
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [ValidateRange(1, 128)][int]$Parallel = 4,
    [switch]$SkipTests,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$ProjectDir = Split-Path -Parent $PSScriptRoot
$MarkerName = '.kylinlist-portable'
$DefaultTargetLeaf = 'KylinList-Test'

if (-not $CMake) { $CMake = Join-Path $ProjectDir '.tools/cmake/bin/cmake.exe' }
if (-not $Ninja) { $Ninja = Join-Path $ProjectDir '.tools/bin/ninja.exe' }
if (-not $QtRoot) { $QtRoot = Join-Path $ProjectDir '.tools/Qt/5.12.8/mingw73_64' }
if (-not $CompilerRoot) { $CompilerRoot = Join-Path $ProjectDir '.tools/Qt/Tools/mingw730_64' }
if (-not $BuildDir) { $BuildDir = Join-Path $ProjectDir '.tools/build-v5-windows' }
if (-not $TargetDir) {
    $Desktop = [Environment]::GetFolderPath('Desktop')
    if (-not $Desktop) { throw 'Cannot resolve the current user Desktop folder. Pass -TargetDir.' }
    $TargetDir = Join-Path $Desktop $DefaultTargetLeaf
}
$TargetDir = [IO.Path]::GetFullPath($TargetDir).TrimEnd('\', '/')
if (-not $TargetDir) { throw 'TargetDir must not be empty' }
if ($TargetDir -eq [IO.Path]::GetPathRoot($TargetDir)) { throw "Refusing to mirror into a drive root: $TargetDir" }
$TargetParent = Split-Path -Parent $TargetDir
if (-not $TargetParent) { throw "Refusing to mirror into a top-level path: $TargetDir" }

if (Get-Process -Name kylinlist -ErrorAction SilentlyContinue) {
    throw "The preview build is still running. Close it first, then re-run: $TargetDir"
}

$VersionMatch = [regex]::Match(
    (Get-Content -LiteralPath (Join-Path $ProjectDir 'CMakeLists.txt') -Raw),
    '(?m)^\s*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $VersionMatch.Success) { throw 'Cannot parse project version from CMakeLists.txt' }
$Version = $VersionMatch.Groups[1].Value
$StageDir = Join-Path $BuildDir "package/KylinList-$Version-Windows-x64"

if (-not $SkipBuild) {
    Write-Host "== Build + package -> $StageDir"
    $BuildArgs = @(
        '-File', (Join-Path $PSScriptRoot 'build-windows.ps1'),
        '-CMake', $CMake,
        '-Ninja', $Ninja,
        '-QtRoot', $QtRoot,
        '-CompilerRoot', $CompilerRoot,
        '-BuildDir', $BuildDir,
        '-Configuration', $Configuration,
        '-Parallel', "$Parallel",
        '-SkipInstaller',
        '-SkipArchive'
    )
    if ($SkipTests) { $BuildArgs += '-SkipTests' }
    $HostExe = [Environment]::ProcessPath
    if (-not $HostExe) { $HostExe = (Get-Process -Id $PID).Path }
    & $HostExe @BuildArgs
    if ($LASTEXITCODE -ne 0) { throw "build-windows.ps1 failed with exit code $LASTEXITCODE" }
} elseif (-not (Test-Path -LiteralPath (Join-Path $StageDir 'kylinlist.exe'))) {
    throw "No staged package at $StageDir; drop -SkipBuild or build it first"
}

$SourceExe = Join-Path $StageDir 'kylinlist.exe'
if (-not (Test-Path -LiteralPath $SourceExe)) { throw "Staged executable missing: $SourceExe" }

if (Test-Path -LiteralPath $TargetDir) {
    if (Test-Path -LiteralPath $TargetDir -PathType Leaf) {
        throw "Target exists and is a file, not a portable folder: $TargetDir"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $TargetDir $MarkerName))) {
        throw "Refusing to replace '$TargetDir': it was not created by this script (missing $MarkerName)."
    }
    # Re-verify the resolved path is exactly the requested leaf, so a junction or
    # a symlinked parent cannot redirect the mirror somewhere else.
    $ResolvedTarget = (Resolve-Path -LiteralPath $TargetDir).Path.TrimEnd('\', '/')
    if ($ResolvedTarget -ne $TargetDir) {
        throw "Refusing to mirror through a redirected path: $ResolvedTarget"
    }
} else {
    New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null
}

Write-Host "== Mirror $StageDir -> $TargetDir"
$RobocopyArgs = @($StageDir, $TargetDir, '/MIR', '/COPY:DAT', '/DCOPY:T',
    '/NFL', '/NDL', '/NJH', '/NJS', '/NP', '/R:1', '/W:1')
& robocopy @RobocopyArgs | Out-Null
$RobocopyCode = $LASTEXITCODE
# robocopy: 0-7 are success, 8+ are failures.
if ($RobocopyCode -ge 8) { throw "robocopy failed with exit code $RobocopyCode" }

$TargetExe = Join-Path $TargetDir 'kylinlist.exe'
if (-not (Test-Path -LiteralPath $TargetExe)) { throw "Mirrored executable missing: $TargetExe" }
$SourceHash = (Get-FileHash -LiteralPath $SourceExe -Algorithm SHA256).Hash
$TargetHash = (Get-FileHash -LiteralPath $TargetExe -Algorithm SHA256).Hash
if ($SourceHash -ne $TargetHash) { throw 'Mirrored executable differs from the staged package' }

$Stamp = "version=$Version`nbuilt=$($Configuration)`nupdated=$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')`nsource=$StageDir`n"
Set-Content -LiteralPath (Join-Path $TargetDir $MarkerName) -Value $Stamp -Encoding UTF8

Write-Host ''
Write-Host "Portable preview ready: $TargetExe"
Write-Host "Version $Version, updated $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
