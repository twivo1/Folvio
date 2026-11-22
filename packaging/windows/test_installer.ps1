<#
  End-to-end test of the Windows installer:
    install (silent, per user, into a folder with Polish characters) -> Start menu and desktop shortcuts
    -> built-in self-test of the installed program (with a minimal PATH) -> uninstall
    -> program files removed, user data kept.
  Usage: powershell -ExecutionPolicy Bypass -File packaging\windows\test_installer.ps1 -Setup dist\Folvio-Setup-0.10.0-Windows-x64.exe
         [-Require ffmpeg,soffice,tesseract] [-Report dist\installer-test.txt]
  Do not run it on a machine where Folvio is installed for daily use: it installs into a separate folder,
  but the uninstaller removes that installation's shortcuts.
#>
param(
    [Parameter(Mandatory = $true)][string]$Setup,
    [string]$Require = "",
    [string]$Report = ""
)
$ErrorActionPreference = 'Stop'
$log = New-Object System.Collections.Generic.List[string]
function Note([string]$m) { Write-Host $m; $log.Add($m) }
function Fail([string]$m) { Note "FAIL: $m"; if ($Report) { $log | Set-Content -Encoding utf8 $Report }; exit 1 }

$setupPath = (Resolve-Path $Setup).Path
$dir = Join-Path $env:LOCALAPPDATA 'Programs\Folvio Test Zażółć'
Note "Installer: $(Split-Path -Leaf $setupPath) ($([math]::Round((Get-Item $setupPath).Length / 1MB, 1)) MB)"
Note "Windows: $((Get-CimInstance Win32_OperatingSystem).Caption) $((Get-CimInstance Win32_OperatingSystem).Version)"

$p = Start-Process -FilePath $setupPath -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER',
        "/DIR=`"$dir`"", '/TASKS=desktopicon', '/LANG=english', '/LOG') -PassThru -Wait
if ($p.ExitCode -ne 0) { Fail "installer exit code $($p.ExitCode)" }
$exe = Join-Path $dir 'Folvio.exe'
if (-not (Test-Path $exe)) { Fail "Folvio.exe not installed in $dir" }
foreach ($relative in 'pdfium.dll','onnxruntime.dll','platforms/qwindows.dll','sqldrivers/qsqlite.dll','tls/qschannelbackend.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $dir $relative) -PathType Leaf)) { Fail "missing installed file: $relative" }
}
Note "PASS install into '$dir'"

# Shell.Application reads the link as Unicode (WScript.Shell's TargetPath goes through the ANSI code page
# and turns Polish letters into '?').
$shell = New-Object -ComObject Shell.Application
foreach ($lnk in @((Join-Path ([Environment]::GetFolderPath('Programs')) 'Folvio.lnk'),
                   (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Folvio.lnk'))) {
    if (-not (Test-Path $lnk)) { Fail "shortcut missing: $lnk" }
    $target = $shell.NameSpace((Split-Path $lnk)).ParseName((Split-Path -Leaf $lnk)).GetLink.Path
    if ($target -ne $exe) { Fail "shortcut $lnk points to $target" }
    Note "PASS shortcut $lnk -> $target"
}

# The installed program must work on its own: minimal PATH, isolated data folder (set by --selftest).
$savedPath = $env:PATH
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
$selfReport = Join-Path $env:TEMP 'folvio-installed-selftest.txt'
try {
    $args = @('--selftest', '--selftest-report', "`"$selfReport`"")
    if ($Require) { $args += @('--selftest-require', $Require) }
    $st = Start-Process -FilePath $exe -ArgumentList $args -PassThru -Wait
    $shot = Join-Path $env:TEMP 'folvio-installed.png'
    $render = Start-Process -PassThru -FilePath $exe -ArgumentList @('--page', '6', '--screenshot', "`"$shot`"", '--screenshot-delay', '2500') -Wait
} finally { $env:PATH = $savedPath }
Get-Content $selfReport | ForEach-Object { Note "   $_" }
if ($st.ExitCode -ne 0) { Fail "self-test of the installed program failed" }
if ($render.ExitCode -ne 0 -or -not (Test-Path $shot) -or (Get-Item $shot).Length -eq 0) { Fail "installed program did not render its window" }
Note "PASS installed program: self-test and window"

# User data that must survive uninstallation.
$userData = Join-Path $env:LOCALAPPDATA 'Folvio'
New-Item -ItemType Directory -Force $userData | Out-Null
$marker = Join-Path $userData 'installer-test-marker.txt'
'keep' | Set-Content $marker
$uninstaller = Join-Path $dir 'unins000.exe'
$u = Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') -PassThru -Wait
if ($u.ExitCode -ne 0) { Fail "uninstaller exit code $($u.ExitCode)" }
Start-Sleep -Seconds 3   # the uninstaller removes its own executable after exiting
if (Test-Path $exe) { Fail "Folvio.exe still present after uninstall" }
if (Test-Path (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Folvio.lnk')) { Fail "desktop shortcut left behind" }
if (-not (Test-Path $marker)) { Fail "user data was deleted by the uninstaller" }
Remove-Item $marker
Note "PASS uninstall: program and shortcuts removed, user data kept"
Note "RESULT: OK"
if ($Report) { $log | Set-Content -Encoding utf8 $Report }
