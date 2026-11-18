<#
  Installs the external engines exactly like Folvio's "Settings -> Install missing engines" does:
  FFmpeg (Gyan.FFmpeg), LibreOffice and Tesseract (UB-Mannheim) through the Windows package manager.
  Used by the release workflow so the full test suite and the installed-program self-test cover them.
#>
$ErrorActionPreference = 'Stop'
$winget = Get-Command winget -ErrorAction SilentlyContinue
if (-not $winget) { throw "winget is not available on this machine." }
foreach ($id in @('Gyan.FFmpeg', 'TheDocumentFoundation.LibreOffice', 'UB-Mannheim.TesseractOCR')) {
    Write-Host "== winget install $id"
    & winget install --id $id --exact --source winget --accept-package-agreements --accept-source-agreements --disable-interactivity --silent
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) {   # 0x8A15002B = already installed
        throw "winget install $id failed with exit code $LASTEXITCODE"
    }
}
