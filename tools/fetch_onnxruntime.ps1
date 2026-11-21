# Downloads ONNX Runtime (MIT, Microsoft) from the official NuGet package Microsoft.ML.OnnxRuntime
# (pinned version and SHA-256) and extracts the Windows x64 runtime into
# third_party\onnxruntime\lib\win-x64. Folvio loads it at run time (C API) for handwriting
# recognition; the app works without it. The DLL needs the Microsoft Visual C++ runtime.
param(
    [string]$Version = "1.31.1",
    [string]$Sha256 = "6871ccbc97f3d5bdb4095a1dcec33aa78d56f6ed8de5980ef5fa1d9f2e7c679a"
)
$ErrorActionPreference = 'Stop'
$dest = Join-Path (Split-Path -Parent $PSScriptRoot) 'third_party\onnxruntime\lib\win-x64'
if (Test-Path (Join-Path $dest 'onnxruntime.dll')) { Write-Host "ONNX Runtime already present in $dest"; exit 0 }
New-Item -ItemType Directory -Force $dest | Out-Null
$pkg = Join-Path $env:TEMP "onnxruntime.$Version.nupkg.zip"
Invoke-WebRequest -Uri "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/$Version/microsoft.ml.onnxruntime.$Version.nupkg" -OutFile $pkg
$actual = (Get-FileHash -Algorithm SHA256 $pkg).Hash.ToLower()
if ($actual -ne $Sha256) { Remove-Item $pkg; throw "ONNX Runtime checksum mismatch: $actual" }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($pkg)
try {
    $entry = $zip.GetEntry('runtimes/win-x64/native/onnxruntime.dll')
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $dest 'onnxruntime.dll'), $true)
} finally { $zip.Dispose() }
Remove-Item $pkg
Write-Host "ONNX Runtime $Version extracted to $dest"
