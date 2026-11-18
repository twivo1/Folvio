<#
  Folvio release build for Windows x64.

  1. Configures and builds a Release with Qt 6 (MinGW) + CMake + Ninja.
  2. Runs the test suite (unless -SkipTests).
  3. Deploys the app into dist\Folvio with windeployqt (Qt DLLs, plugins, MinGW runtime).
  4. Builds the Inno Setup installer into dist\.
  5. Optionally installs it for the current user (-Install), which creates the
     Start menu entry and the desktop shortcut, and verifies the shortcut.

  Usage:  powershell -ExecutionPolicy Bypass -File packaging\build_release.ps1 [-SkipTests] [-Install]
#>
param(
    [string]$QtDir = "C:\Qt\6.10.0\mingw_64",
    [string]$QtTools = "C:\Qt\Tools",
    [switch]$SkipTests,
    [switch]$Install
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build\release'
$dist = Join-Path $root 'dist'
$app = Join-Path $dist 'Folvio'

# Toolchain first in PATH (avoids picking up unrelated compilers).
$env:PATH = "$QtTools\mingw1310_64\bin;$QtTools\CMake_64\bin;$QtTools\Ninja;$QtDir\bin;$env:PATH"

function Invoke-Checked([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}

if (-not (Test-Path (Join-Path $root 'third_party\opencv\OpenCVConfig.cmake'))) {
    Write-Host "== Building OpenCV (first run only)" -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\build_opencv.ps1')
    if ($LASTEXITCODE -ne 0) { throw "OpenCV build failed" }
}

Write-Host "== Configure & build (Release)" -ForegroundColor Cyan
Invoke-Checked cmake @('-S', $root, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_PREFIX_PATH=$QtDir")
Invoke-Checked cmake @('--build', $build, '--parallel')

if (-not $SkipTests) {
    Write-Host "== Tests" -ForegroundColor Cyan
    Push-Location (Join-Path $build 'tests')
    try { Invoke-Checked '.\folvio_tests.exe' @('--gtest_brief=1') } finally { Pop-Location }
}

Write-Host "== Deploy to $app" -ForegroundColor Cyan
if (Test-Path $app) { Remove-Item -Recurse -Force $app }
New-Item -ItemType Directory -Force $app | Out-Null
Copy-Item (Join-Path $build 'Folvio.exe') $app
Copy-Item (Join-Path $root 'third_party\pdfium\bin\pdfium.dll') $app
Copy-Item -Recurse (Join-Path $root 'engines') (Join-Path $app 'engines')
Copy-Item (Join-Path $root 'THIRD_PARTY_LICENSES.md') $app
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $app 'LICENSE.txt')
# Licence texts required by the LGPL (Qt) and for the MinGW-w64 runtime.
New-Item -ItemType Directory -Force (Join-Path $app 'licenses\qt'), (Join-Path $app 'licenses\mingw-runtime') | Out-Null
Copy-Item (Join-Path $root 'packaging\licenses\LGPL-3.0.txt'), (Join-Path $root 'packaging\licenses\GPL-3.0.txt') (Join-Path $app 'licenses\qt')
Copy-Item (Join-Path $root 'packaging\licenses\GPL-3.0.txt'), (Join-Path $root 'packaging\licenses\GCC-RUNTIME-LIBRARY-EXCEPTION-3.1.txt'),
          (Join-Path $root 'packaging\licenses\winpthreads-COPYING.txt'), (Join-Path $root 'packaging\licenses\mingw-w64-runtime-COPYING.txt') (Join-Path $app 'licenses\mingw-runtime')
Copy-Item (Join-Path $root 'engines\tessdata\LICENSE') (Join-Path $app 'licenses\tessdata-LICENSE.txt')
New-Item -ItemType Directory -Force (Join-Path $app 'licenses\pdfium') | Out-Null
Copy-Item (Join-Path $root 'third_party\pdfium\LICENSE') (Join-Path $app 'licenses\pdfium\LICENSE')
Copy-Item (Join-Path $root 'third_party\pdfium\licenses\*') (Join-Path $app 'licenses\pdfium')
New-Item -ItemType Directory -Force (Join-Path $app 'licenses\opencv') | Out-Null
Copy-Item (Join-Path $root 'third_party\opencv\LICENSE') (Join-Path $app 'licenses\opencv\LICENSE')
Copy-Item (Join-Path $root 'third_party\opencv\etc\licenses\zlib-LICENSE') (Join-Path $app 'licenses\opencv\zlib-LICENSE')
# Qt PDF (a second PDFium) and the network/TLS stack are not used by Folvio.
Invoke-Checked (Join-Path $QtDir 'bin\windeployqt.exe') @('--release', '--no-translations', '--no-system-d3d-compiler',
    '--no-opengl-sw', '--compiler-runtime', '--exclude-plugins', 'qpdf', '--skip-plugin-types', 'tls,networkinformation,generic',
    (Join-Path $app 'Folvio.exe'))
foreach ($unused in @('Qt6Pdf.dll', 'Qt6Network.dll')) {
    $f = Join-Path $app $unused
    if (Test-Path $f) { Remove-Item -Force $f }
}

# Only the SQLite driver is used.
Get-ChildItem (Join-Path $app 'sqldrivers') -Filter '*.dll' | Where-Object { $_.Name -ne 'qsqlite.dll' } | Remove-Item -Force

# Smoke test of the deployed copy: it must start from its own folder without the SDK in PATH,
# render its window and pass the built-in self-test (real Combine, Auto Change and conversions).
Write-Host "== Smoke test of deployed app" -ForegroundColor Cyan
$cleanPath = "$env:SystemRoot\System32;$env:SystemRoot"
$shot = Join-Path $env:TEMP 'folvio-smoke.png'
$report = Join-Path $dist 'selftest-windows.txt'
Remove-Item $shot, $report -ErrorAction SilentlyContinue
$savedPath = $env:PATH
$env:PATH = $cleanPath   # child process inherits the minimal PATH
try {
    Start-Process -FilePath (Join-Path $app 'Folvio.exe') -ArgumentList @('--screenshot', "`"$shot`"", '--screenshot-delay', '2500') -Wait
    $st = Start-Process -FilePath (Join-Path $app 'Folvio.exe') -ArgumentList @('--selftest', '--selftest-report', "`"$report`"") -Wait -PassThru
} finally { $env:PATH = $savedPath }
if (-not (Test-Path $shot)) { throw "Deployed app did not start correctly (no screenshot produced)." }
Get-Content $report | ForEach-Object { Write-Host "   $_" }
if ($st.ExitCode -ne 0) { throw "Self-test of the deployed app failed." }
Write-Host "   deployed app started OK"

Write-Host "== Installer" -ForegroundColor Cyan
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 (ISCC.exe) not found. Install with: winget install JRSoftware.InnoSetup" }
$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'VERSION (\d+\.\d+\.\d+)' | Select-Object -First 1).Matches[0].Groups[1].Value
Invoke-Checked $iscc @("/DAppVersion=$version", "/DSourceDir=$app", "/DOutputDir=$dist", (Join-Path $root 'packaging\windows\folvio.iss'))
$setup = Join-Path $dist "Folvio-Setup-$version-Windows-x64.exe"
Write-Host "   $setup"

if ($Install) {
    Write-Host "== Install for current user" -ForegroundColor Cyan
    $proc = Start-Process -FilePath $setup -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER',
        '/TASKS=desktopicon', '/LANG=english') -PassThru -Wait
    if ($proc.ExitCode -ne 0) { throw "Installer failed with exit code $($proc.ExitCode)" }
    # The real Desktop folder (may be redirected to OneDrive).
    $desktop = [Environment]::GetFolderPath('Desktop')
    $lnk = Join-Path $desktop 'Folvio.lnk'
    if (-not (Test-Path $lnk)) { throw "Desktop shortcut not found at $lnk" }
    $target = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk).TargetPath
    if (-not (Test-Path $target)) { throw "Shortcut target does not exist: $target" }
    Write-Host "   Desktop shortcut: $lnk -> $target"
    $startMenu = Join-Path ([Environment]::GetFolderPath('Programs')) 'Folvio.lnk'
    Write-Host ("   Start menu entry: {0} ({1})" -f $startMenu, (Test-Path $startMenu))
}
Write-Host "Done." -ForegroundColor Green
