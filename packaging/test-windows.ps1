#requires -Version 5.1
# Test a generated current-user offline installer without modifying user data.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [Parameter(Mandatory = $true)][string]$InstallDir,
    [Parameter(Mandatory = $true)][string]$DataDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
foreach ($Directory in @($InstallDir, $DataDir)) {
    if ([IO.Path]::IsPathRooted($Directory) -eq $false -or $Directory.Length -lt 8 -or $Directory -match '\.\.') {
        throw 'Test directories must be explicit, absolute paths without ..'
    }
    if (Test-Path -LiteralPath $Directory) {
        if ((Get-ChildItem -LiteralPath $Directory -Force | Measure-Object).Count -ne 0) {
            throw "Test directory is not empty: $Directory"
        }
    } else { New-Item -ItemType Directory -Path $Directory -Force | Out-Null }
}
$Installer = (Resolve-Path -LiteralPath $Installer).Path
try {
    Write-Output "Installing $Installer"
    $Setup = Start-Process -PassThru -FilePath $Installer -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER', "/DIR=$InstallDir")
    if (-not $Setup.WaitForExit(120000)) { $Setup.Kill(); throw 'Installer timed out' }
    $Setup.Refresh()
    Write-Output "Installer exit: $($Setup.ExitCode)"
    if ($Setup.ExitCode -ne 0) { throw "Installer failed: $($Setup.ExitCode)" }
    $App = Join-Path $InstallDir 'kylinlist.exe'
    if (-not (Test-Path -LiteralPath $App)) { throw 'Installed application missing' }
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $PowerShell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    & $PowerShell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'windows/Smoke.ps1') `
        -Application $App -DataDir $DataDir -WorkDir $InstallDir
    if ($LASTEXITCODE -ne 0) { throw "Installed application failed: $LASTEXITCODE" }
    if (-not (Test-Path -LiteralPath (Join-Path $DataDir 'kylinlist.db'))) { throw 'Installed application did not create SQLite database' }
    $Uninstaller = Get-ChildItem -LiteralPath $InstallDir -Filter 'unins*.exe' | Select-Object -First 1
    if (-not $Uninstaller) { throw 'Uninstaller missing' }
    $Uninstall = Start-Process -PassThru -FilePath $Uninstaller.FullName -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    if (-not $Uninstall.WaitForExit(60000)) { $Uninstall.Kill(); throw 'Uninstaller timed out' }
    $Uninstall.Refresh()
    if ($Uninstall.ExitCode -ne 0) { throw "Uninstaller failed: $($Uninstall.ExitCode)" }
    if (-not (Test-Path -LiteralPath (Join-Path $DataDir 'kylinlist.db'))) { throw 'Uninstall deleted user data' }
    Write-Output 'PASS: install, isolated app start, SQLite initialization, uninstall, user data preserved.'
} finally {
    $TestProcesses = Get-CimInstance Win32_Process | Where-Object {
        $_.Name -eq 'kylinlist.exe' -and $_.ExecutablePath -eq (Join-Path $InstallDir 'kylinlist.exe') -and $_.CommandLine -like '*--smoke-test*'
    }
    foreach ($Process in $TestProcesses) { Stop-Process -Id $Process.ProcessId -Force -ErrorAction SilentlyContinue }
}
