# Downloads the prebuilt PDFium package (BSD-3-Clause) into third_party\pdfium.
param([string]$Url = "https://github.com/bblanchon/pdfium-binaries/releases/latest/download/pdfium-win-x64.tgz")
$ErrorActionPreference = 'Stop'
$dest = Join-Path (Split-Path -Parent $PSScriptRoot) 'third_party\pdfium'
New-Item -ItemType Directory -Force $dest | Out-Null
$tgz = Join-Path $env:TEMP 'pdfium-win-x64.tgz'
Invoke-WebRequest -Uri $Url -OutFile $tgz
tar -xzf $tgz -C $dest
Remove-Item $tgz
Write-Host "PDFium extracted to $dest"
