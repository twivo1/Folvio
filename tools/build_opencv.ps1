<#
  Builds a minimal static OpenCV (core + imgproc, Apache-2.0) with the Qt MinGW toolchain
  into third_party\opencv. Official OpenCV Windows binaries target MSVC only.

  Usage: powershell -ExecutionPolicy Bypass -File tools\build_opencv.ps1
#>
param(
    [string]$Version = "4.10.0",
    [string]$QtTools = "C:\Qt\Tools"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$srcRoot = Join-Path $root 'third_party\src'
$src = Join-Path $srcRoot "opencv-$Version"
$build = Join-Path $root "build\opencv-$Version"
$install = Join-Path $root 'third_party\opencv'
$env:PATH = "$QtTools\mingw1310_64\bin;$QtTools\CMake_64\bin;$QtTools\Ninja;$env:PATH"

if (-not (Test-Path (Join-Path $src 'CMakeLists.txt'))) {
    New-Item -ItemType Directory -Force $srcRoot | Out-Null
    $tgz = Join-Path $srcRoot "opencv-$Version.tar.gz"
    Invoke-WebRequest -Uri "https://github.com/opencv/opencv/archive/refs/tags/$Version.tar.gz" -OutFile $tgz
    tar -xzf $tgz -C $srcRoot
}

$args = @('-S', $src, '-B', $build, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_INSTALL_PREFIX=$install",
    '-DBUILD_LIST=core,imgproc', '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TESTS=OFF', '-DBUILD_PERF_TESTS=OFF',
    '-DBUILD_EXAMPLES=OFF', '-DBUILD_DOCS=OFF', '-DBUILD_opencv_apps=OFF', '-DBUILD_opencv_python3=OFF',
    '-DBUILD_JAVA=OFF', '-DWITH_IPP=OFF', '-DWITH_ITT=OFF', '-DWITH_OPENCL=OFF', '-DWITH_EIGEN=OFF',
    '-DWITH_LAPACK=OFF', '-DWITH_OPENMP=OFF', '-DWITH_PROTOBUF=OFF', '-DWITH_ADE=OFF', '-DWITH_QT=OFF',
    '-DWITH_GTK=OFF', '-DWITH_WIN32UI=OFF', '-DWITH_FFMPEG=OFF', '-DWITH_DSHOW=OFF', '-DWITH_MSMF=OFF',
    '-DWITH_JPEG=OFF', '-DWITH_PNG=OFF', '-DWITH_TIFF=OFF', '-DWITH_WEBP=OFF', '-DWITH_OPENEXR=OFF',
    '-DWITH_JASPER=OFF', '-DWITH_OPENJPEG=OFF', '-DWITH_IMGCODEC_HDR=OFF', '-DWITH_VTK=OFF',
    '-DOPENCV_ENABLE_NONFREE=OFF', '-DBUILD_ZLIB=ON', '-DCV_TRACE=OFF', '-DOPENCV_GENERATE_PKGCONFIG=OFF',
    '-DBUILD_WITH_STATIC_CRT=OFF', '-DENABLE_PRECOMPILED_HEADERS=OFF')
& cmake @args
if ($LASTEXITCODE -ne 0) { throw "OpenCV configure failed" }
& cmake --build $build --parallel
if ($LASTEXITCODE -ne 0) { throw "OpenCV build failed" }
& cmake --install $build
if ($LASTEXITCODE -ne 0) { throw "OpenCV install failed" }
Write-Host "OpenCV $Version installed to $install"
