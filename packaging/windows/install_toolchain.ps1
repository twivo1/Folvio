<#
  Installs the Windows build toolchain used by Folvio releases (CI and fresh machines):
  Qt 6.10.0 (win64_mingw + qtimageformats), MinGW-w64 13.1, CMake and Ninja from the official Qt
  repository through aqtinstall (pinned), and Inno Setup 6 if it is missing.

  Usage: powershell -ExecutionPolicy Bypass -File packaging\windows\install_toolchain.ps1 [-Root C:\Qt]
#>
param(
    [string]$Root = "C:\Qt",
    [string]$QtVersion = "6.10.0",
    [string]$AqtVersion = "3.3.0"
)
$ErrorActionPreference = 'Stop'
function Invoke-Checked([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe $($arguments -join ' ') failed with exit code $LASTEXITCODE" }
}
$venv = Join-Path $env:RUNNER_TEMP 'aqt'
if (-not $env:RUNNER_TEMP) { $venv = Join-Path $env:TEMP 'folvio-aqt' }
Invoke-Checked python @('-m', 'venv', $venv)
$py = Join-Path $venv 'Scripts\python.exe'
Invoke-Checked $py @('-m', 'pip', 'install', '--quiet', "aqtinstall==$AqtVersion")
$aqt = Join-Path $venv 'Scripts\aqt.exe'
Invoke-Checked $aqt @('install-qt', 'windows', 'desktop', $QtVersion, 'win64_mingw', '-m', 'qtimageformats', '-O', $Root)
Invoke-Checked $aqt @('install-tool', 'windows', 'desktop', 'tools_mingw1310', 'qt.tools.win64_mingw1310', '-O', $Root)
Invoke-Checked $aqt @('install-tool', 'windows', 'desktop', 'tools_cmake', 'qt.tools.cmake', '-O', $Root)
Invoke-Checked $aqt @('install-tool', 'windows', 'desktop', 'tools_ninja', 'qt.tools.ninja', '-O', $Root)

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) {
    if (Get-Command choco -ErrorAction SilentlyContinue) {
        Invoke-Checked choco @('install', 'innosetup', '--version=6.7.1', '-y', '--no-progress')
    } else {
        Invoke-Checked winget @('install', '--id', 'JRSoftware.InnoSetup', '--exact', '--silent', '--accept-package-agreements', '--accept-source-agreements')
    }
}
Get-ChildItem (Join-Path $Root 'Tools') | ForEach-Object { Write-Host "tool: $($_.Name)" }
Write-Host "Qt $QtVersion installed in $Root"
