# Runs husk (or another $HFS/bin tool) in an isolated Houdini environment, as
# tests/reference/tools/refcommon.py IsolatedHoudini does: own preference
# directory, no user packages, the installed Nova package skipped, the staged
# Nova package on HOUDINI_PATH. A running Houdini session is never touched.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File husk_env.ps1 -Tool husk.exe -Stage <stage> -Log <file> -CmdLine '<arguments>'
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Tool = 'husk.exe',
    [string]$Stage = '',
    [string]$Hfs = 'C:\Program Files\Side Effects Software\Houdini 22.0.466',
    [string]$Log = '',
    [int]$TimeoutSec = 600,
    [string]$CmdLine = ''
)

$prefs = Join-Path ([System.IO.Path]::GetTempPath()) 'nova_m6_prefs'
New-Item -ItemType Directory -Force $prefs | Out-Null
$env:HOUDINI_USER_PREF_DIR = (Join-Path $prefs 'houdini__HVER__').Replace('\', '/')
foreach ($v in 'HOME', 'HOUDINI_PACKAGE_DIR', 'HSITE', 'PXR_PLUGINPATH_NAME', 'HOUDINI_USD_DSO_PATH', 'HOUDINI_PATH') {
    Remove-Item "Env:$v" -ErrorAction SilentlyContinue
}
$env:HOUDINI_PACKAGE_SKIPLIST = 'nova'
$env:FXHOUDINIMCP_AUTOSTART = '0'
if ($Stage) {
    $env:HOUDINI_PATH = (Resolve-Path $Stage).Path.Replace('\', '/') + ';&'
}
$env:PATH = (Join-Path $Hfs 'bin') + ';' + $env:PATH

$exe = Join-Path $Hfs ('bin\' + $Tool)
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.Arguments = $CmdLine
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$p = [System.Diagnostics.Process]::Start($psi)
$out = $p.StandardOutput.ReadToEndAsync()
$err = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    Write-Output "TIMEOUT after $TimeoutSec s (process $($p.Id) left running)"
    exit 124
}
$text = "> $exe $CmdLine`n" + $out.Result + $err.Result
if ($Log) { Set-Content -Path $Log -Value $text -Encoding utf8 }
Write-Output $text
exit $p.ExitCode
