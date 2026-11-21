# Downloads the prebuilt PDFium (BSD-3-Clause, bblanchon/pdfium-binaries) into third_party\pdfium.
# Pinned release and SHA-256; the extracted package is committed in the repository.
param(
    [string]$Tag = "chromium/8165",
    [string]$Sha256 = "ca7338ae7779b8b1b82f4580305c4d762d214f8df1d54b47647fe3cc75989ded"
)
$ErrorActionPreference = 'Stop'
$dest = Join-Path (Split-Path -Parent $PSScriptRoot) 'third_party\pdfium'
New-Item -ItemType Directory -Force $dest | Out-Null
$tgz = Join-Path $env:TEMP 'pdfium-win-x64.tgz'
$url = "https://github.com/bblanchon/pdfium-binaries/releases/download/$($Tag -replace '/', '%2F')/pdfium-win-x64.tgz"
Invoke-WebRequest -Uri $url -OutFile $tgz
$actual = (Get-FileHash -Algorithm SHA256 $tgz).Hash.ToLower()
if ($actual -ne $Sha256) { Remove-Item $tgz; throw "PDFium checksum mismatch: $actual" }
tar -xzf $tgz -C $dest
Remove-Item $tgz
Write-Host "PDFium $Tag extracted to $dest"
