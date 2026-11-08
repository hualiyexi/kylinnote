#requires -Version 5.1
# Build-time smoke helper for a Windows GUI-subsystem application.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Application,
    [Parameter(Mandatory = $true)][string]$DataDir,
    [Parameter(Mandatory = $true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$ProjectDir = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Source = Join-Path $ProjectDir 'packaging/windows/SmokeLauncher.cs'
$Binary = Join-Path $WorkDir 'KylinList-SmokeLauncher.exe'
# Compile only in the isolated build directory. Nothing is shipped to users.
Add-Type -TypeDefinition (Get-Content -LiteralPath $Source -Raw) -OutputAssembly $Binary -OutputType ConsoleApplication
$env:KYLINLIST_DATA_DIR = $DataDir
& $Binary $Application '--smoke-test' '--data-dir' $DataDir
if ($LASTEXITCODE -ne 0) { throw "GUI smoke failed with exit code $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath (Join-Path $DataDir 'kylinlist.db'))) {
    throw 'GUI smoke did not initialize SQLite'
}
Write-Output 'PASS: GUI application started without console and exited successfully.'
