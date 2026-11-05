# Builds the Nova installer (tools/installer/README.md): zips the installed
# package dist/nova into payload/nova.zip, builds the setup executable with
# cargo (Rust, MSVC toolchain) and copies it to dist/NovaSetup-<version>.exe.
# Run after `cmake --install build/vs2022 --config Release`; -Package takes
# another package folder (e.g. build/vs2022/stage/Release/nova while a
# running Houdini holds dist/nova's DLLs).
#
#   powershell -ExecutionPolicy Bypass -File tools\installer\build.ps1 [-Package <folder>]
[CmdletBinding()]
param([string]$Package = '')

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
$dist = Join-Path $repo 'dist'
$nova = if ($Package) { (Resolve-Path $Package).Path } else { Join-Path $dist 'nova' }
if (-not (Test-Path (Join-Path $nova 'dso\usd_plugins\hdNova.dll'))) {
    throw "no package at ${nova}: run cmake --install build/vs2022 --config Release first"
}

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
