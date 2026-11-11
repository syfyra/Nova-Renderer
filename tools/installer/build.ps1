# Builds the Nova installer (tools/installer/README.md): zips the installed
# package dist/nova into payload/nova.zip, builds the setup executable with
# cargo (Rust, MSVC toolchain) and copies it to dist/NovaSetup-<version>.exe.
# Run after `cmake --install build/vs2022 --config Release`; -Package takes
# another package folder (e.g. build/vs2022/stage/Release/nova while a
# running Houdini holds dist/nova's DLLs).
#
#
# The build in -Build (default build/vs2022) must hold Nova XPU's kernels for
# the release's GPU architectures (core/CMakeLists.txt, NOVA_GPU_ARCHS): a
# shorter list is a development build that runs on fewer cards, and is
# refused unless -DevelopmentKernels says it is meant.
#
#   powershell -ExecutionPolicy Bypass -File tools\installer\build.ps1 [-Package <folder>] [-Build <folder>]
[CmdletBinding()]
param([string]$Package = '', [string]$Build = '', [switch]$DevelopmentKernels)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
$dist = Join-Path $repo 'dist'
$nova = if ($Package) { (Resolve-Path $Package).Path } else { Join-Path $dist 'nova' }
if (-not (Test-Path (Join-Path $nova 'dso\usd_plugins\hdNova.dll'))) {
    throw "no package at ${nova}: run cmake --install build/vs2022 --config Release first"
}

# The GPU architectures of the build the package comes from.
$core = Get-Content (Join-Path $repo 'core\CMakeLists.txt') -Raw
if ($core -notmatch 'set\(NOVA_GPU_ARCHS_RELEASE "([0-9;]+)"\)') { throw 'no NOVA_GPU_ARCHS_RELEASE in core/CMakeLists.txt' }
$releaseArchs = $Matches[1]
# What the installer tells about a graphics card follows the renderer's rule
# (src/main.rs, xpu_support): the oldest architecture and the tested ones.
if ($core -notmatch 'set\(NOVA_GPU_ARCHS_TESTED "([0-9;]+)"\)') { throw 'no NOVA_GPU_ARCHS_TESTED in core/CMakeLists.txt' }
$testedArchs = $Matches[1]
$cache = Join-Path $(if ($Build) { $Build } else { Join-Path $repo 'build\vs2022' }) 'CMakeCache.txt'
if (-not (Test-Path $cache)) { throw "no CMake cache at ${cache}: name the build folder with -Build" }
$line = Select-String -Path $cache -Pattern '^NOVA_GPU_ARCHS:STRING=(.*)$' | Select-Object -First 1
if (-not $line) { throw "${cache} has no NOVA_GPU_ARCHS: the build has no GPU backend" }
$archs = $line.Matches[0].Groups[1].Value
if ($archs -ne $releaseArchs) {
    if (-not $DevelopmentKernels) {
        throw "the build's NOVA_GPU_ARCHS is '$archs', the release's '$releaseArchs': configure with -DNOVA_GPU_ARCHS=`"$releaseArchs`" and rebuild (or pass -DevelopmentKernels)"
    }
    Write-Warning "packaging development kernels: NOVA_GPU_ARCHS is '$archs', the release's '$releaseArchs'"
}
"kernels: sm $($archs -replace ';', ', '), tested on sm $($testedArchs -replace ';', ', ')"
$env:NOVA_GPU_MIN_ARCH = ($archs -split ';' | ForEach-Object { [int]$_ } | Measure-Object -Minimum).Minimum
$env:NOVA_GPU_TESTED_ARCHS = $testedArchs

# The version: CMakeLists.txt's project(Nova VERSION x.y.z).
$cmake = Get-Content (Join-Path $repo 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(Nova VERSION ([0-9.]+)') { throw 'no project version in CMakeLists.txt' }
$version = $Matches[1]

# The payload: dist/nova as nova/... entries (forward slashes), without
# Python's byte-code caches.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$payload = Join-Path $here 'payload'
New-Item -ItemType Directory -Force $payload | Out-Null
$zip = Join-Path $payload 'nova.zip'
Remove-Item $zip -ErrorAction SilentlyContinue
$archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem $nova -Recurse -File) {
        $rel = $file.FullName.Substring($nova.Length).TrimStart('\').Replace('\', '/')
        if ($rel -match '(^|/)__pycache__/') { continue }
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, "nova/$rel",
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $archive.Dispose()
}
'payload: {0:N1} MB from {1}' -f ((Get-Item $zip).Length / 1MB), $nova

$env:NOVA_VERSION = $version
Push-Location $here
try {
    cargo build --release
    if ($LASTEXITCODE -ne 0) { throw "cargo build failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}
New-Item -ItemType Directory -Force $dist | Out-Null
$exe = Join-Path $dist "NovaSetup-$version.exe"
Copy-Item (Join-Path $here 'target\release\nova-setup.exe') $exe -Force
'installer: {0} ({1:N1} MB)' -f $exe, ((Get-Item $exe).Length / 1MB)
